// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "core/catalog/catalog_column_ordinals.hpp"
#include <array>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <new>
#include <stdexcept>

namespace {
bool forbid_allocation = false;
std::size_t checks = 0;
void Check(bool condition, const char* detail) {
  ++checks;
  if (!condition) throw std::runtime_error(detail);
}
}
void* operator new(std::size_t size) {
  if (forbid_allocation) throw std::bad_alloc{};
  if (void* value = std::malloc(size ? size : 1)) return value;
  throw std::bad_alloc{};
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* value) noexcept { std::free(value); }
void operator delete(void* value, std::size_t) noexcept { std::free(value); }
void operator delete[](void* value) noexcept { std::free(value); }
void operator delete[](void* value, std::size_t) noexcept { std::free(value); }

namespace catalog = scratchbird::core::catalog;
namespace {
constexpr std::uint64_t limit = 4294967296ULL;  // independent wire-domain oracle
constexpr auto maximum = std::numeric_limits<std::uint64_t>::max();
void RangeAndTransition() {
  const std::array<std::uint64_t, 12> waters{
      0, 1, 2, 255, 256, 65535, 65536, limit-2, limit-1, limit, limit+1, maximum};
  const std::array<std::uint64_t, 8> counts{0, 1, 2, 255, 256, limit-1, limit, maximum};
  for (auto h : waters) {
    for (auto count : counts) {
      const auto result = catalog::ComputeCatalogColumnOrdinalRange(h, count);
      const bool valid = h >= 1 && h <= limit;
      const bool fits = valid && count != 0 && count <= limit-h;
      Check(result.ok() == fits, "range bounds differ from ordinal domain");
      if (fits) {
        Check(result.range.first == h && result.range.count == count &&
                  result.range.successor_high_water == h+count &&
                  result.range.successor_high_water <= limit,
              "range wrapped or lost an ordinal");
      } else {
        Check(result.range == catalog::CatalogColumnOrdinalRange{}, "failed range exposed partial allocation");
        const auto expected = !valid ? catalog::CatalogColumnOrdinalError::invalid_high_water :
            !count ? catalog::CatalogColumnOrdinalError::invalid_count : catalog::CatalogColumnOrdinalError::exhausted;
        Check(result.error == expected, "range error precedence differs");
      }
    }
    for (auto next : waters) {
      Check(catalog::CatalogColumnOrdinalHighWaterAdvances(h, next) ==
                (h >= 1 && h <= limit && next >= 1 && next <= limit && next >= h),
            "high-water transition reduced or repaired history");
    }
    for (auto ordinal : std::array<std::uint32_t, 4>{0, 1, 7, UINT32_MAX}) {
      Check(catalog::CatalogColumnOrdinalBelowHighWater(ordinal, h) ==
                (h >= 1 && h <= limit && ordinal < h), "column exceeds high-water");
    }
  }
  const auto last = catalog::ComputeCatalogColumnOrdinalRange(limit-1, 1);
  Check(last.ok() && last.range.first == UINT32_MAX && last.range.successor_high_water == limit,
        "last stored ordinal unavailable");
  Check(!catalog::ComputeCatalogColumnOrdinalRange(last.range.successor_high_water, 1).ok(),
        "exhaustion rolled over");
  // Dropping the highest column does not lower its independently retained H.
  std::uint64_t high_water = 8;
  const std::array<std::uint32_t, 2> survivors{0, 2};
  for (auto ordinal : survivors) Check(catalog::CatalogColumnOrdinalBelowHighWater(ordinal, high_water), "survivor rejected");
  const auto next = catalog::ComputeCatalogColumnOrdinalRange(high_water, 2);
  Check(next.ok() && next.range.first == 8 && next.range.successor_high_water == 10,
        "dropped ordinals were inferred from current columns and reused");
  Check(high_water == 8, "pure range calculation mutated caller state");
}
void FieldCodec() {
  for (auto h : std::array<std::uint64_t, 7>{1, 256, 65536, 0x01020304, limit-2, limit-1, limit}) {
    catalog::CatalogColumnOrdinalHighWaterBytes bytes{};
    Check(catalog::EncodeCatalogColumnOrdinalHighWater(h, &bytes), "valid field refused");
    auto oracle = h;
    for (auto byte : bytes) {
      Check(byte == oracle % 256, "field endian/extent differs from independent oracle");
      oracle /= 256;
    }
    std::uint64_t decoded = 99;
    Check(catalog::DecodeCatalogColumnOrdinalHighWater(bytes, &decoded) && decoded == h,
          "high-water round trip changed allocation history");
    for (std::size_t width = 0; width < 8; ++width) {
      decoded = 99;
      Check(!catalog::DecodeCatalogColumnOrdinalHighWater(std::span(bytes).first(width), &decoded) && decoded == 99,
            "truncated field repaired or published output");
    }
  }
  catalog::CatalogColumnOrdinalHighWaterBytes destination{1,2,3,4,5,6,7,8};
  const auto sentinel = destination;
  for (auto bad : std::array<std::uint64_t, 3>{0, limit+1, maximum}) {
    Check(!catalog::EncodeCatalogColumnOrdinalHighWater(bad, &destination) && destination == sentinel,
          "invalid encode changed destination");
    catalog::CatalogColumnOrdinalHighWaterBytes bytes{};
    for (unsigned i = 0; i < 8; ++i) bytes[i] = static_cast<std::uint8_t>(bad >> (i*8));
    std::uint64_t output = 99;
    Check(!catalog::DecodeCatalogColumnOrdinalHighWater(bytes, &output) && output == 99,
          "invalid decode changed destination");
  }
  std::uint64_t output = 99;
  const std::array<std::uint8_t, 9> trailing{1,0,0,0,0,0,0,0,0};
  Check(!catalog::DecodeCatalogColumnOrdinalHighWater(trailing, &output) && output == 99, "trailing byte accepted");
  Check(!catalog::EncodeCatalogColumnOrdinalHighWater(1, nullptr) &&
        !catalog::DecodeCatalogColumnOrdinalHighWater(sentinel, nullptr), "null output accepted");
}
}
int main() {
  try {
    forbid_allocation = true;
    RangeAndTransition();
    FieldCodec();
    forbid_allocation = false;
    std::cout << "PASS column ordinal lifetime primitives checks=" << checks << "; no heap allocation\n";
    return 0;
  } catch (const std::exception& error) {
    forbid_allocation = false;
    std::cerr << error.what() << '\n';
    return 1;
  }
}

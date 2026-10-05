// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <array>
#include <cstdint>
#include <span>

namespace scratchbird::core::catalog {

// CATALOG-COLUMN-ORDINAL-LIFETIME-NONREUSE-001.
// Structural primitives only. A caller must bind the persisted high-water
// field to its admitted table identity/generation and publish it with the
// new columns. These helpers perform no reservation, identity issuance or I/O.
inline constexpr std::uint64_t kCatalogColumnOrdinalLimit = std::uint64_t{1} << 32;
using CatalogColumnOrdinalHighWaterBytes = std::array<std::uint8_t, 8>;

constexpr bool ValidCatalogColumnOrdinalHighWater(std::uint64_t value) noexcept {
  return value != 0 && value <= kCatalogColumnOrdinalLimit;
}

constexpr bool CatalogColumnOrdinalBelowHighWater(
    std::uint32_t ordinal, std::uint64_t high_water) noexcept {
  return ValidCatalogColumnOrdinalHighWater(high_water) && ordinal < high_water;
}

constexpr bool CatalogColumnOrdinalHighWaterAdvances(
    std::uint64_t previous, std::uint64_t successor) noexcept {
  return ValidCatalogColumnOrdinalHighWater(previous) &&
      ValidCatalogColumnOrdinalHighWater(successor) && successor >= previous;
}

enum class CatalogColumnOrdinalError : std::uint8_t {
  none, invalid_high_water, invalid_count, exhausted
};
struct CatalogColumnOrdinalRange {
  std::uint32_t first = 0;
  std::uint64_t count = 0;
  std::uint64_t successor_high_water = 0;
  bool operator==(const CatalogColumnOrdinalRange&) const = default;
};
struct CatalogColumnOrdinalRangeResult {
  CatalogColumnOrdinalError error = CatalogColumnOrdinalError::invalid_high_water;
  CatalogColumnOrdinalRange range;
  constexpr bool ok() const noexcept { return error == CatalogColumnOrdinalError::none; }
};

// Precedence: malformed high-water, zero count, then exhausted capacity.
// Subtract before addition; even UINT64_MAX requests cannot wrap or truncate.
constexpr CatalogColumnOrdinalRangeResult ComputeCatalogColumnOrdinalRange(
    std::uint64_t high_water, std::uint64_t count) noexcept {
  if (!ValidCatalogColumnOrdinalHighWater(high_water))
    return {CatalogColumnOrdinalError::invalid_high_water, {}};
  if (!count) return {CatalogColumnOrdinalError::invalid_count, {}};
  if (count > kCatalogColumnOrdinalLimit - high_water)
    return {CatalogColumnOrdinalError::exhausted, {}};
  return {CatalogColumnOrdinalError::none,
          {static_cast<std::uint32_t>(high_water), count, high_water + count}};
}

// Exact scalar field, not a standalone native table record. Refusals leave
// the caller's destination unchanged. Neither operation allocates memory.
inline bool EncodeCatalogColumnOrdinalHighWater(
    std::uint64_t high_water, CatalogColumnOrdinalHighWaterBytes* output) noexcept {
  if (!output || !ValidCatalogColumnOrdinalHighWater(high_water)) return false;
  CatalogColumnOrdinalHighWaterBytes staged{};
  for (unsigned i = 0; i < staged.size(); ++i)
    staged[i] = static_cast<std::uint8_t>(high_water >> (8 * i));
  *output = staged;
  return true;
}

inline bool DecodeCatalogColumnOrdinalHighWater(
    std::span<const std::uint8_t> bytes, std::uint64_t* output) noexcept {
  if (!output || bytes.size() != 8) return false;
  std::uint64_t staged = 0;
  for (unsigned i = 0; i < bytes.size(); ++i)
    staged |= std::uint64_t{bytes[i]} << (8 * i);
  if (!ValidCatalogColumnOrdinalHighWater(staged)) return false;
  *output = staged;
  return true;
}
}  // namespace scratchbird::core::catalog

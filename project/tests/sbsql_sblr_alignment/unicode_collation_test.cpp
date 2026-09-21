// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "unicode_collation.hpp"
#include "hash_digest.hpp"
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <new>
#include <sstream>

namespace r = scratchbird::core::resources;
using S = r::UnicodeNormalizationStatus;
using Level = r::UnicodeCollationStrength;
namespace { long fail_after = -1; unsigned faults = 0; }
void* operator new(std::size_t n) {
  if (fail_after == 0) { ++faults; throw std::bad_alloc(); }
  if (fail_after > 0) --fail_after;
  if (auto* p = std::malloc(n ? n : 1)) return p;
  throw std::bad_alloc();
}
void* operator new[](std::size_t n) { return ::operator new(n); }
void* operator new(std::size_t n, const std::nothrow_t&) noexcept { try { return ::operator new(n); } catch (...) { return nullptr; } }
void* operator new[](std::size_t n, const std::nothrow_t&) noexcept { try { return ::operator new(n); } catch (...) { return nullptr; } }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
void operator delete(void* p, const std::nothrow_t&) noexcept { std::free(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { std::free(p); }
namespace {
unsigned checks = 0, failures = 0;
void Check(bool valid, const char* label) {
  ++checks; if (!valid && ++failures <= 20) std::cerr << label << '\n';
}
std::string Read(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) throw std::runtime_error("fixture file missing");
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}
std::string Utf8(std::uint32_t c) {
  std::string out;
  if (c < 128) out.push_back(c);
  else if (c < 2048) { out.push_back(192 + c / 64); out.push_back(128 + c % 64); }
  else if (c < 65536) { out.push_back(224 + c / 4096); out.push_back(128 + c / 64 % 64); out.push_back(128 + c % 64); }
  else { out.push_back(240 + c / 262144); out.push_back(128 + c / 4096 % 64); out.push_back(128 + c / 64 % 64); out.push_back(128 + c % 64); }
  return out;
}
std::string Sequence(const std::string& line, bool* valid = nullptr) {
  if (valid) *valid = true;
  std::istringstream words(line); std::uint32_t c; std::string out;
  while (words >> std::hex >> c) {
    if (c > 0x10ffff || (c >= 0xd800 && c <= 0xdfff)) {
      if (!valid) throw std::runtime_error("non-scalar oracle");
      *valid = false;
    }
    out += Utf8(c);
  }
  return out;
}
void Oracle(const r::UnicodeCollationData& data) {
  const auto bytes = Read(SB_UCA_TEST_FILE);
  const auto hash = scratchbird::core::hash::ComputeSha256Digest(
      reinterpret_cast<const unsigned char*>(bytes.data()), bytes.size());
  Check(hash.ok() && scratchbird::core::hash::HexLower(hash.digest) ==
      "06a8d9d2191574c74623f66af960682feadeb714a9fa12ab4657c425f8683f53", "official oracle digest");
  std::istringstream lines(bytes); std::string line, previous;
  std::array<std::string, 4> previous_keys;
  unsigned rows = 0, rejected = 0;
  while (std::getline(lines, line)) {
    if (line.empty() || line.front() == '#') continue;
    bool valid;
    const auto text = Sequence(line, &valid);
    if (!valid) {
      ++rejected;
      std::string unchanged = "sentinel";
      Check(data.MakeSortKey(text, Level::identical, {4096, 4096}, &unchanged) == S::invalid_utf8 &&
          unchanged == "sentinel", "official nonscalar row not refused atomically");
      continue;
    }
    ++rows;
    for (unsigned i = 0; i < 4; ++i) {
      std::string key;
      const auto level = static_cast<Level>(i + 1);
      Check(data.MakeSortKey(text, level, {4096, 4096}, &key) == S::ok, "official string refused");
      Check(rows == 1 || previous_keys[i] <= key, "official DUCET order violated");
      int comparison = 99;
      Check(data.Compare(previous, text, level, {4096, 4096}, &comparison) == S::ok &&
          comparison == ((previous_keys[i] > key) - (previous_keys[i] < key)), "comparison/key mismatch");
      previous_keys[i] = std::move(key);
    }
    previous = text;
  }
  Check(rows == 208040 && rejected == 30, "official oracle incomplete");
  std::cout << "official DUCET17 scalar_rows=" << rows << " rejected_nonscalar_rows=" << rejected << '\n';
}
void Cases(const r::UnicodeCollationData& data, const r::UnicodeNormalizationData& normalization) {
  const auto equal = [&](const std::string& a, const std::string& b, Level level, int sign) {
    int result = 99;
    Check(data.Compare(a, b, level, {4096, 4096}, &result) == S::ok && result == sign, "specified collation relation");
  };
  equal(Sequence("0391"), Sequence("03B1"), Level::primary, 0);
  equal(Sequence("03A3"), Sequence("03C2"), Level::primary, 0);
  equal(Sequence("0410"), Sequence("0430"), Level::primary, 0);
  equal(Sequence("0065 0301"), "e", Level::primary, 0);
  equal(Sequence("00E9"), Sequence("0065 0301"), Level::identical, 0);
  equal(Sequence("00E9"), "e", Level::secondary, 1);
  equal("a", "A", Level::tertiary, -1);
  equal("a", "A", Level::secondary, 0);
  equal("", std::string(1, '\0'), Level::tertiary, 0);
  equal("", std::string(1, '\0'), Level::identical, -1);
  equal(Sequence("AC01"), Sequence("1100 1161 11A8"), Level::identical, 0);
  equal(Sequence("17000"), Sequence("18D00"), Level::primary, -1);
  equal(Sequence("18D00"), Sequence("18800"), Level::primary, -1);
  equal(Sequence("4E00"), Sequence("3400"), Level::primary, -1);
  equal(Sequence("3400"), Sequence("0378"), Level::primary, -1);
  Check(normalization.IsAssigned(0x17000) && !normalization.IsAssigned(0x18d7f) &&
      normalization.IsAssigned(0x18d00) && normalization.IsAssigned(0xac01) &&
      !normalization.IsAssigned(0x378) && !normalization.IsAssigned(0xd800) &&
      !normalization.IsAssigned(0x110000), "assignment ranges and holes");
  // Independent UCA17 weight derivation examples, not a round-trip oracle.
  std::string key;
  Check(data.MakeSortKey(Utf8(0x18d00), Level::primary, {32, 32}, &key) == S::ok &&
      key == std::string("\xfb\x00\x9d\x00", 4), "Tangut supplement origin");
  Check(data.MakeSortKey(Utf8(0x18d7f), Level::primary, {32, 32}, &key) == S::ok &&
      key == std::string("\xfb\xc3\x8d\x7f", 4), "unassigned siniform hole fallback");
  Check(data.MakeSortKey(Utf8(0x4e00), Level::primary, {32, 32}, &key) == S::ok &&
      key == std::string("\xfb\x40\xce\x00", 4), "core Han implicit weight");
  Check(data.MakeSortKey(Utf8(0x3400), Level::primary, {32, 32}, &key) == S::ok &&
      key == std::string("\xfb\x80\xb4\x00", 4), "extension Han implicit weight");
  key = "sentinel";
  Check(data.MakeSortKey("\xed\xa0\x80", Level::tertiary, {32, 32}, &key) == S::invalid_utf8 && key == "sentinel", "malformed scalar atomicity");
  Check(data.MakeSortKey("a", static_cast<Level>(0), {32, 32}, &key) == S::invalid_argument && key == "sentinel", "invalid strength");
  Check(data.MakeSortKey("a", Level::tertiary, {32, 32}, nullptr) == S::invalid_argument, "null key output");
  const auto input = Sequence("0E40 0E01 0065 0301 0315 1E69 0345 AC01 1D15E");
  std::string expected;
  Check(data.MakeSortKey(input, Level::identical, {4096, 4096}, &expected) == S::ok, "key baseline");
  for (std::size_t limit = 0; limit < expected.size(); ++limit)
    Check(data.MakeSortKey(input, Level::identical, {4096, limit}, &key) == S::output_limit && key == "sentinel", "bounded key atomicity");
  int comparison = 99;
  Check(data.Compare("valid", "\x80", Level::tertiary, {128, 128}, &comparison) == S::invalid_utf8 && comparison == 99,
      "invalid right comparison published a sign");
  bool completed = false;
  for (long i = 0; i < 256; ++i) {
    key = "sentinel"; fail_after = i;
    const auto result = data.MakeSortKey(input, Level::identical, {4096, 4096}, &key);
    const bool exhausted = fail_after == 0; fail_after = -1;
    if (result == S::ok) { Check(key == expected, "OOM fallback changed key"); if (!exhausted) { completed = true; break; } }
    else Check(result == S::allocation_failure && key == "sentinel", "key OOM not atomic");
  }
  Check(completed, "key OOM sweep incomplete");
  std::string tibetan;
  for (unsigned i = 0; i < 20000; ++i) tibetan += Utf8(0xf71);
  Check(data.MakeSortKey(tibetan, Level::tertiary, {100000, 200000}, &key) == S::ok,
        "long equal-class contraction-prefix run refused");
}
} // namespace
int main() {
  try {
    const auto base = std::string(SB_UCA_DATA_DIR) + '/';
    const auto ucd = Read(base + "UnicodeData.txt"), weights = Read(base + "allkeys.txt"), properties = Read(base + "PropList.txt");
    std::shared_ptr<const r::UnicodeNormalizationData> normalization;
    Check(r::UnicodeNormalizationData::Load17(ucd, &normalization) == S::ok, "normalization load");
    std::shared_ptr<const r::UnicodeCollationData> data;
    Check(r::UnicodeCollationData::Load17(weights, properties, normalization, &data) == S::ok && data, "DUCET load");
    if (!data) return 1;
    Oracle(*data); Cases(*data, *normalization);
    auto retained = data;
    auto corrupt = weights; corrupt[100] ^= 1;
    Check(r::UnicodeCollationData::Load17(corrupt, properties, normalization, &retained) == S::invalid_resource && retained == data,
        "corrupt DUCET accepted");
    corrupt = properties; corrupt[100] ^= 1;
    Check(r::UnicodeCollationData::Load17(weights, corrupt, normalization, &retained) == S::invalid_resource && retained == data,
        "corrupt Han properties accepted");
    Check(r::UnicodeCollationData::Load17(weights, properties, {}, &retained) == S::invalid_argument && retained == data,
        "missing normalization authority accepted");
    bool complete = false;
    for (long i = 0; i < 256; ++i) {
      retained = data; fail_after = i;
      const auto result = r::UnicodeCollationData::Load17(weights, properties, normalization, &retained);
      fail_after = -1;
      if (result == S::ok) { complete = true; Check(retained != data, "cross-node cached table"); break; }
      Check(result == S::allocation_failure && retained == data, "load OOM not atomic");
    }
    Check(complete && faults > 10, "load OOM sweep incomplete");
    std::cout << "checks=" << checks << " allocation_faults=" << faults << " failures=" << failures << '\n';
    return failures ? 1 : 0;
  } catch (const std::exception& e) { fail_after = -1; std::cerr << e.what() << '\n'; return 2; }
}

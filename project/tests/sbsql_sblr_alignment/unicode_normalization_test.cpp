// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "unicode_normalization.hpp"
#include "hash_digest.hpp"

#include <array>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <new>
#include <sstream>

namespace r = scratchbird::core::resources;
using Status = r::UnicodeNormalizationStatus;
namespace { long fail_after = -1; unsigned faults = 0; }
void* operator new(std::size_t size) {
  if (fail_after == 0) { ++faults; throw std::bad_alloc(); }
  if (fail_after > 0) --fail_after;
  if (auto* p = std::malloc(size ? size : 1)) return p;
  throw std::bad_alloc();
}
void* operator new[](std::size_t n) { return ::operator new(n); }
void* operator new(std::size_t n, const std::nothrow_t&) noexcept {
  try { return ::operator new(n); } catch (...) { return nullptr; }
}
void* operator new[](std::size_t n, const std::nothrow_t&) noexcept {
  try { return ::operator new(n); } catch (...) { return nullptr; }
}
void operator delete(void* p, const std::nothrow_t&) noexcept { std::free(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { std::free(p); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
namespace {
unsigned checks = 0, failures = 0;
void Check(bool value, const char* message) {
  ++checks;
  if (!value) { if (++failures <= 20) std::cerr << message << '\n'; }
}
std::string Read(const char* path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) throw std::runtime_error("missing independent fixture");
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}
// Fixture encoding is independent of the production UTF-8 implementation.
std::string Utf8(std::uint32_t c) {
  std::string out;
  if (c < 128) out.push_back(c);
  else if (c < 2048) { out.push_back(192 + c / 64); out.push_back(128 + c % 64); }
  else if (c < 65536) {
    out.push_back(224 + c / 4096); out.push_back(128 + c / 64 % 64); out.push_back(128 + c % 64);
  } else {
    out.push_back(240 + c / 262144); out.push_back(128 + c / 4096 % 64);
    out.push_back(128 + c / 64 % 64); out.push_back(128 + c % 64);
  }
  return out;
}
std::string Sequence(const std::string& field) {
  std::istringstream words(field); std::string out; std::uint32_t code;
  while (words >> std::hex >> code) out += Utf8(code);
  return out;
}
void Conformance(const r::UnicodeNormalizationData& data) {
  const auto fixture = Read(SB_NORMALIZATION_TEST_FILE);
  const auto digest = scratchbird::core::hash::ComputeSha256Digest(
      reinterpret_cast<const unsigned char*>(fixture.data()), fixture.size());
  Check(digest.ok() && scratchbird::core::hash::HexLower(digest.digest) ==
      "5019ffd530751a741900c849c0e010332f142a3612234639bd200b82138a87db", "independent vector digest");
  std::istringstream lines(fixture); std::string line;
  bool part1 = false; unsigned rows = 0;
  std::vector<bool> listed(0x110000);
  while (std::getline(lines, line)) {
    if (line.empty() || line.front() == '#') continue;
    if (line.front() == '@') { part1 = line.rfind("@Part1 ", 0) == 0; continue; }
    std::istringstream columns(line); std::array<std::string, 5> values;
    for (unsigned i = 0; i < 5; ++i) {
      std::string field;
      if (!std::getline(columns, field, ';')) throw std::runtime_error("malformed fixture");
      if (part1 && i == 0) {
        std::istringstream words(field); std::uint32_t code;
        if (!(words >> std::hex >> code) || code >= listed.size()) throw std::runtime_error("fixture scalar");
        listed[code] = true;
      }
      values[i] = Sequence(field);
    }
    ++rows;
    for (unsigned i = 0; i < 5; ++i) {
      std::string output = "sentinel";
      const auto& expected = values[i < 3 ? 2 : 4];
      const auto result = data.NormalizeNfd(values[i], expected.size(), &output);
      Check(result == Status::ok && output == expected, "NFD independent vector mismatch");
      if (!expected.empty()) {
        output = "sentinel";
        Check(data.NormalizeNfd(values[i], expected.size() - 1, &output) == Status::output_limit &&
            output == "sentinel", "NFD budget failure not atomic");
      }
    }
  }
  Check(rows == 20034, "Unicode 17 vector row count changed");
  // Stronger than the assigned-only rule: also preserve unassigned scalars,
  // private-use characters and noncharacters outside Part1.
  for (std::uint32_t cp = 0; cp <= 0x10ffff; ++cp) {
    if (listed[cp] || (cp >= 0xd800 && cp <= 0xdfff)) continue;
    const auto original = Utf8(cp); std::string actual;
    Check(data.NormalizeNfd(original, original.size(), &actual) == Status::ok && actual == original,
        "unlisted scalar is not NFD identity");
  }
  std::cout << "Unicode 17 official rows=" << rows << '\n';
}
void Negatives(const std::string& bytes, const std::shared_ptr<const r::UnicodeNormalizationData>& data) {
  auto retained = data;
  Check(r::UnicodeNormalizationData::Load17(bytes, nullptr) == Status::invalid_argument, "null load target");
  Check(r::UnicodeNormalizationData::Load17(std::string_view(bytes).substr(1), &retained) == Status::invalid_resource &&
      retained == data, "truncated resource published");
  auto changed = bytes; changed[100] ^= 1;
  Check(r::UnicodeNormalizationData::Load17(changed, &retained) == Status::invalid_resource && retained == data,
      "same-size corrupted resource published");
  std::string output = "sentinel";
  const std::array<std::string, 9> invalid{
    "\x80", "\xc0\x80", "\xc1\xbf", "\xe0\x80\x80", "\xed\xa0\x80",
    "\xf0\x80\x80\x80", "\xf4\x90\x80\x80", "\xf5\x80\x80\x80", "\xf0\x90\x80"};
  for (const auto& input : invalid)
    Check(data->NormalizeNfd("valid-prefix" + input, 1024, &output) == Status::invalid_utf8 &&
        output == "sentinel", "invalid UTF-8 published a prefix");
  Check(data->NormalizeNfd("", 0, &output) == Status::ok && output.empty(), "empty text");
  const std::string nul("a\0b", 3);
  Check(data->NormalizeNfd(nul, 3, &output) == Status::ok && output == nul, "embedded NUL");
  output = Utf8(0x1e69);
  Check(data->NormalizeNfd(output, 5, &output) == Status::ok && output == Sequence("0073 0323 0307"), "aliased output");
  Check(data->NormalizeNfd("a", 1, nullptr) == Status::invalid_argument, "null normalizer target");
  Check(data->CombiningClass(0x301) == 230 && data->CombiningClass(0x323) == 220 &&
      data->CombiningClass(0x41) == 0, "canonical combining classes");

  bool completed = false;
  for (long i = 0; i < 128; ++i) {
    retained = data; fail_after = i;
    const auto result = r::UnicodeNormalizationData::Load17(bytes, &retained);
    fail_after = -1;
    if (result == Status::ok) { completed = true; Check(retained != data, "cross-node global cache"); break; }
    Check(result == Status::allocation_failure && retained == data, "load OOM not atomic");
  }
  Check(completed, "load allocation sweep did not finish");
  const auto input = Sequence("1E69 031B 0315 0301 0323 0345 AC01 1D15E");
  std::string expected; Check(data->NormalizeNfd(input, 128, &expected) == Status::ok, "OOM baseline");
  completed = false;
  for (long i = 0; i < 128; ++i) {
    output = "sentinel"; fail_after = i;
    const auto result = data->NormalizeNfd(input, 128, &output);
    const bool exhausted = fail_after == 0; fail_after = -1;
    if (result == Status::ok) {
      Check(output == expected, "OOM fallback changed normalization");
      // stable_sort may use an allocation-free fallback after nothrow failure.
      if (!exhausted) { completed = true; break; }
    } else Check(result == Status::allocation_failure && output == "sentinel", "NFD OOM not atomic");
  }
  Check(completed && faults > 10, "normalization allocation sweep incomplete");
}
} // namespace
int main() {
  try {
    const auto bytes = Read(SB_UNICODE_DATA_FILE);
    std::shared_ptr<const r::UnicodeNormalizationData> data;
    Check(r::UnicodeNormalizationData::Load17(bytes, &data) == Status::ok && data, "pinned UCD load");
    if (!data) return 1;
    Conformance(*data); Negatives(bytes, data);
    std::cout << "checks=" << checks << " allocation_faults=" << faults << " failures=" << failures << '\n';
    return failures ? 1 : 0;
  } catch (const std::exception& e) { fail_after = -1; std::cerr << e.what() << '\n'; return 2; }
}

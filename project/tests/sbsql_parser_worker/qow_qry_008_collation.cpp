// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
// Actual canonical comparison component; not live receipt or SQL/IPC evidence.
#include "query/expression_api.hpp"
#include "engine/sblr/relational_descriptor_codec.hpp"
#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <new>
#include <stdexcept>

namespace { long fail_after = -1; std::size_t checks = 0, faults = 0; }
void* operator new(std::size_t size) {
  if (fail_after == 0) throw std::bad_alloc();
  if (fail_after > 0) --fail_after;
  if (void* p = std::malloc(size ? size : 1)) return p;
  throw std::bad_alloc();
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

namespace api = scratchbird::engine::internal_api;
namespace dt = scratchbird::core::datatypes;
using Uuid = api::EngineUuid;
namespace {
void Require(bool condition, const char* message) {
  ++checks;
  if (!condition) throw std::runtime_error(message);
}
Uuid Id(std::uint8_t suffix) {
  return {{0x01,0x9f,0,0,0,0,0x74,0,0x80,0,0,0,0,0,8,suffix}};
}
void Number(std::string& body, std::size_t offset, std::uint64_t value, unsigned width) {
  for (unsigned i = 0; i < width; ++i) body[offset + i] = static_cast<char>(value >> (8 * i));
}
void Identity(std::string& body, std::size_t offset, Uuid uuid) {
  std::copy(uuid.bytes.begin(), uuid.bytes.end(), body.begin() + offset);
}
api::EngineDescriptor TextDescriptor(std::uint8_t suffix) {
  api::EngineDescriptor result;
  result.descriptor_uuid = Id(suffix); result.descriptor_kind = "scalar";
  result.type_uuid = Id(2); result.collation_uuid = Id(1);
  result.canonical_type_name = "text";
  // Independent exact kind213 body, not a call to the production encoder.
  std::string body(152, '\0');
  Number(body, 0, 1, 2); Number(body, 2, 3, 2); Number(body, 4, suffix, 4);
  Identity(body, 8, Id(suffix)); Identity(body, 24, Id(2)); Identity(body, 40, Id(1));
  Identity(body, 56, Id(3)); Identity(body, 72, Id(4));
  for (auto offset : {88,96,104,112,120}) Number(body, offset, 1, 8);
  Number(body, 128, 1, 2); Number(body, 130, 1, 1);
  constexpr std::string_view codec = "datatype.text.utf8.v1";
  Number(body, 144, codec.size(), 4); body.append(codec);
  result.encoded_descriptor = std::move(body);
  return result;
}
api::EngineTypedValue Value(std::string bytes, std::uint8_t suffix = 5) {
  api::EngineTypedValue value; value.descriptor = TextDescriptor(suffix);
  value.encoded_value = std::move(bytes); value.state = api::EngineValueState::value;
  return value;
}
dt::DatatypeTextSeedAuthority Seed() {
  dt::DatatypeTextSeedAuthority result;
  result.active = true; result.seed_pack_name = "qow_core_resource_catalog";
  result.seed_pack_version = "2026.07"; result.charset_name = "UTF-8";
  result.collation_name = "unicode_ci_ai";
  result.collation_case_insensitive = true; result.collation_accent_insensitive = true;
  return result;
}
void Compare(std::string a, std::string b, const dt::DatatypeTextSeedAuthority& seed, int sign) {
  auto left = Value(std::move(a)); auto right = Value(std::move(b), 6);
  int result = 99; std::string refusal;
  Require(api::QowCompareCanonicalCollatedScalarsV1(
      left, right, Id(1), 31, 17, seed, &result, &refusal), "canonical comparison refused");
  Require(refusal.empty() && ((result > 0) - (result < 0)) == sign, "incorrect collation result");
}
void TestComparison() {
  Compare("R\xc3\xa9sum\xc3\xa9", "resume", Seed(), 0);
  Compare("alpha", "Beta", Seed(), -1);
  Compare("z", "a", Seed(), 1); Compare("", "", Seed(), 0); Compare("", "x", Seed(), -1);
  auto exact = Seed(); exact.collation_case_insensitive = false;
  exact.collation_accent_insensitive = false; exact.collation_name = "unicode_cs";
  Compare("A", "a", exact, -1);
}
void TestUnicodeComparison() {
  // These are full Unicode case/accent requirements, not Latin-only examples.
  Compare("\xce\x91", "\xce\xb1", Seed(), 0);  // Greek alpha
  Compare("\xce\xa3", "\xcf\x82", Seed(), 0);  // Greek final sigma
  Compare("\xd0\x90", "\xd0\xb0", Seed(), 0);  // Cyrillic a
  Compare("e\xcc\x81", "e", Seed(), 0);         // combining acute
}
void TestRefusal() {
  const auto left = Value("alpha"); const auto right = Value("ALPHA", 6);
  const auto refuse = [&](const api::EngineTypedValue& a, const api::EngineTypedValue& b,
                          const Uuid& identity, std::uint64_t epoch, std::uint64_t family,
                          const dt::DatatypeTextSeedAuthority& seed) {
    int result = 99; std::string refusal = "old";
    Require(!api::QowCompareCanonicalCollatedScalarsV1(
        a, b, identity, epoch, family, seed, &result, &refusal) &&
        result == 99 && !refusal.empty(), "refusal published comparison or accepted malformed input");
  };
  refuse(left, right, Uuid{}, 31, 17, Seed());
  for (std::size_t i = 0; i < 16; ++i) {
    auto id = Id(1); id.bytes[i] ^= 1; refuse(left, right, id, 31, 17, Seed());
  }
  refuse(left, right, Id(1), 0, 17, Seed()); refuse(left, right, Id(1), 31, 0, Seed());
  for (auto member : {&dt::DatatypeTextSeedAuthority::seed_pack_name,
                     &dt::DatatypeTextSeedAuthority::seed_pack_version,
                     &dt::DatatypeTextSeedAuthority::charset_name,
                     &dt::DatatypeTextSeedAuthority::collation_name}) {
    auto seed = Seed(); (seed.*member).clear(); refuse(left, right, Id(1), 31, 17, seed);
  }
  auto seed = Seed(); seed.active = false; refuse(left, right, Id(1), 31, 17, seed);
  for (bool mutate_left : {false,true}) {
    const auto changed_value = [&](const api::EngineTypedValue& value) {
      refuse(mutate_left ? value : left, mutate_left ? right : value, Id(1), 31, 17, Seed());
    };
    auto v = left; v.descriptor.descriptor_uuid = Id(99); changed_value(v);
    v = left; v.descriptor.type_uuid = Id(99); changed_value(v);
    v = left; v.descriptor.collation_uuid = Id(99); changed_value(v);
    v = left; v.descriptor.type_uuid = {}; changed_value(v);
    v = left; v.descriptor.collation_uuid = {}; changed_value(v);
    v = left; v.descriptor.canonical_type_name = "bigint"; changed_value(v);
    v = left; v.descriptor.descriptor_kind = "row"; changed_value(v);
    v = left; v.descriptor.encoded_descriptor = "type_uuid=019f0000-0000-7400-8000-000000000842;collation_uuid=019f0000-0000-7400-8000-000000000801"; changed_value(v);
    v = left; v.descriptor.encoded_descriptor.push_back('\0'); changed_value(v);
    for (std::size_t size = 0; size < left.descriptor.encoded_descriptor.size(); ++size) {
      v = left; v.descriptor.encoded_descriptor.resize(size); changed_value(v);
    }
    for (auto offset : {0,2,4,8,24,40,56,72,88,96,104,112,120,128,130,131,132,136,140,144,148}) {
      v = left;
      if (offset == 8 || offset == 24 || offset == 40 || offset == 56 || offset == 72)
        v.descriptor.encoded_descriptor[offset + 6] = 0x40;
      else Number(v.descriptor.encoded_descriptor, offset, offset == 131 || offset == 132 || offset == 136 || offset == 140 || offset == 148 ? 1 : 0, offset == 130 || offset == 131 ? 1 : 2);
      changed_value(v);
    }
    v = left; v.descriptor.encoded_descriptor[152] = 'X'; changed_value(v);
    for (const auto state : {api::EngineValueState::missing, api::EngineValueState::default_requested,
                            api::EngineValueState::unknown, api::EngineValueState::error,
                            api::EngineValueState::lob_handle, api::EngineValueState::protected_value}) {
      v = left; v.state = state; changed_value(v);
    }
    v = left; v.state = api::EngineValueState::sql_null; v.is_null = true; changed_value(v);
    v = left; v.is_null = true; changed_value(v);
    v = left; v.binary_value.push_back(1); changed_value(v);
  }
  int result = 99; std::string refusal;
  Require(!api::QowCompareCanonicalCollatedScalarsV1(left,right,Id(1),31,17,Seed(),nullptr,&refusal), "null output accepted");
  Require(!api::QowCompareCanonicalCollatedScalarsV1(left,right,Id(1),31,17,Seed(),&result,nullptr) && result == 99, "null refusal accepted");
}
void TestAtomicity() {
  auto left = Value(std::string(200, 'A')), right = Value(std::string(200, 'a'), 6);
  auto seed = Seed(); bool completed = false;
  for (long site = 0; site < 1000; ++site) {
    int comparison = 99; std::string refusal; fail_after = site;
    try {
      const bool ok = api::QowCompareCanonicalCollatedScalarsV1(left,right,Id(1),31,17,seed,&comparison,&refusal);
      fail_after = -1;
      if (ok) { Require(comparison == 0, "allocation sweep returned wrong result"); completed = true; break; }
      Require(comparison == 99 && !refusal.empty(), "allocation refusal published result");
      ++faults;
    } catch (const std::bad_alloc&) {
      fail_after = -1; ++faults;
      Require(comparison == 99, "allocation failure published comparison");
    }
  }
  Require(completed && faults > 0, "allocation sweep incomplete");
  api::RelationalTypeDescriptor original;
  Require(api::QowDecodeCanonicalTextDescriptorV1(left.descriptor,&original), "valid descriptor decode refused");
  auto changed = left.descriptor; changed.encoded_descriptor.push_back('x');
  auto sentinel = original;
  Require(!api::QowDecodeCanonicalTextDescriptorV1(changed,&sentinel) && sentinel == original,
          "failed descriptor decode modified output");
}
}
int main(int argc, char** argv) {
  try {
    if (argc == 2 && std::string_view(argv[1]) == "--unicode") {
      TestUnicodeComparison();
      std::cout << "PASS Unicode collation checks=" << checks << '\n';
      return EXIT_SUCCESS;
    }
    TestComparison(); TestRefusal(); TestAtomicity();
    std::cout << "PASS binary collation checks=" << checks << " allocation_faults=" << faults << '\n';
    return EXIT_SUCCESS;
  } catch (const std::exception& e) {
    fail_after = -1; std::cerr << "FAIL after " << checks << " checks: " << e.what() << '\n';
    return EXIT_FAILURE;
  }
}

// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0

#include "datatype_bit_string.hpp"
#include "datatype_binary.hpp"
#include "datatype_binary_view.hpp"
#include "datatype_physical_encoding.hpp"
#include "hash_digest.hpp"
#include "../support/binary_uuid_fixture.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {
namespace dt = scratchbird::core::datatypes;
namespace platform = scratchbird::core::platform;

unsigned checks = 0;
[[noreturn]] void Fail(std::string_view message) {
  std::cerr << "FAIL: " << message << '\n';
  std::exit(EXIT_FAILURE);
}
void Check(bool value, std::string_view message) {
  ++checks;
  if (!value) Fail(message);
}

std::vector<platform::byte> Hex(std::string_view text) {
  auto nibble = [](char c) -> unsigned {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return 10 + c - 'a';
    if (c >= 'A' && c <= 'F') return 10 + c - 'A';
    return 99;
  };
  Check((text.size() & 1u) == 0, "hex fixture has odd length");
  std::vector<platform::byte> result(text.size() / 2);
  for (std::size_t i = 0; i < result.size(); ++i) {
    const auto a = nibble(text[2 * i]), b = nibble(text[2 * i + 1]);
    Check(a < 16 && b < 16, "hex fixture has invalid digit");
    result[i] = static_cast<platform::byte>((a << 4) | b);
  }
  return result;
}

dt::BitStringAuthorityReceiptV3 Receipt() {
  return {scratchbird::tests::FixtureUuid(9901, 1), dt::kDatatypeCohortV9,
          9, 9};
}

dt::BitStringDescriptorProfileV3 Profile(
    dt::BitStringSurfaceProfileKindV3 kind, std::uint32_t length) {
  const auto identity = dt::LookupDatatypeTypeCodecIdentityV3(
      dt::kDatatypeCohortV9, 9, 9,
      scratchbird::tests::FixtureUuidLiteral(
          "019d0000-0000-7000-8000-00000000d829"),
      1);
  Check(identity.ok &&
            dt::IsExactCanonicalBitStringTypeCodecIdentityV3(identity.row),
        "exact D709 bit identity lookup failed");
  const auto historical = dt::LookupDatatypeTypeCodecIdentityV3(
      dt::kDatatypeCohortV6, 6, 6,
      scratchbird::tests::FixtureUuidLiteral(
          "019d0000-0000-7000-8000-00000000d829"),
      1);
  Check(historical.ok &&
            !dt::IsExactCanonicalBitStringTypeCodecIdentityV3(historical.row),
        "d706 bit identity did not remain historical-only");
  const auto historical_profile = dt::BuildBitStringDescriptorProfileV3(
      {{scratchbird::tests::FixtureUuid(9901, 2), dt::kDatatypeCohortV6,
        6, 6}, historical.row, kind, length});
  Check(!historical_profile.ok(),
        "d706 historical identity established a current bit profile");
  const auto profile = dt::BuildBitStringDescriptorProfileV3(
      {Receipt(), identity.row, kind, length});
  Check(profile.ok(), "profile construction failed");
  return profile.profile;
}

void CheckDigest(const std::array<platform::byte, 32>& actual,
                 std::string_view expected, std::string_view message) {
  const auto bytes = Hex(expected);
  Check(std::equal(actual.begin(), actual.end(), bytes.begin(), bytes.end()),
        message);
}

void ProfilesAreExact() {
  auto unqualified = Profile(dt::BitStringSurfaceProfileKindV3::unqualified, 1);
  auto varying_max = Profile(dt::BitStringSurfaceProfileKindV3::varying,
                             dt::kBitStringMaximumLogicalBitsV3);
  auto fixed8 = Profile(dt::BitStringSurfaceProfileKindV3::fixed, 8);
  auto varying8 = Profile(dt::BitStringSurfaceProfileKindV3::varying, 8);

  Check(unqualified.canonical_profile_material.size() == 432,
        "profile material is not 432 bytes");
  Check(unqualified.canonical_profile_material ==
            varying_max.canonical_profile_material,
        "unqualified and varying(max) differ");
  CheckDigest(unqualified.profile_fingerprint,
              "df8df3aad936c1b5df7bc9273be4140aa1c17f234db053ee5f4ecd5eb1970205",
              "unqualified profile fingerprint drifted");
  CheckDigest(unqualified.comparison_cohort_fingerprint,
              "3f9a497b21e8b4f589a690077c9ec0df18c3bb0f77fc5026b63a3d39b5f81a5e",
              "unqualified cohort fingerprint drifted");
  CheckDigest(fixed8.profile_fingerprint,
              "5902d69362aac21d7d428490d391c74728fac7d57f8ed84e7c79ec833dd3bbc9",
              "fixed8 profile fingerprint drifted");
  CheckDigest(varying8.profile_fingerprint,
              "ca0177eec7e79c3797a436d0b2ea441ac34478350144d1f94282b25b85371d50",
              "varying8 profile fingerprint drifted");

  const auto decoded = dt::DecodeBitStringDescriptorProfileMaterialV3(
      Receipt(), unqualified.identity, unqualified.canonical_profile_material);
  Check(decoded.ok() && decoded.profile.profile_fingerprint ==
                            unqualified.profile_fingerprint,
        "profile material did not round trip");
  auto dirty = unqualified.canonical_profile_material;
  dirty[117] = 1;
  Check(!dt::DecodeBitStringDescriptorProfileMaterialV3(
             Receipt(), unqualified.identity, dirty).ok(),
        "reserved profile byte was admitted");
  auto substituted = unqualified;
  substituted.render.generation = 2;
  Check(!dt::ValidateBitStringDescriptorProfileV3(substituted).ok(),
        "substituted render policy was admitted");
}

struct AcceptedVector {
  dt::BitStringSurfaceProfileKindV3 kind;
  std::uint32_t bound;
  std::string_view bits;
  std::string_view component;
};

void AcceptedAndNegativeVectors() {
  static constexpr std::array<AcceptedVector, 15> accepted{{
      {dt::BitStringSurfaceProfileKindV3::unqualified, 1, "", "00000000"},
      {dt::BitStringSurfaceProfileKindV3::unqualified, 1, "0", "0100000000"},
      {dt::BitStringSurfaceProfileKindV3::unqualified, 1, "1", "0100000080"},
      {dt::BitStringSurfaceProfileKindV3::unqualified, 1, "10", "0200000080"},
      {dt::BitStringSurfaceProfileKindV3::unqualified, 1, "101", "03000000a0"},
      {dt::BitStringSurfaceProfileKindV3::unqualified, 1, "1010", "04000000a0"},
      {dt::BitStringSurfaceProfileKindV3::unqualified, 1, "10101", "05000000a8"},
      {dt::BitStringSurfaceProfileKindV3::unqualified, 1, "101010", "06000000a8"},
      {dt::BitStringSurfaceProfileKindV3::unqualified, 1, "1010101", "07000000aa"},
      {dt::BitStringSurfaceProfileKindV3::unqualified, 1, "10101010", "08000000aa"},
      {dt::BitStringSurfaceProfileKindV3::unqualified, 1, "101010101", "09000000aa80"},
      {dt::BitStringSurfaceProfileKindV3::fixed, 8, "10100000", "08000000a0"},
      {dt::BitStringSurfaceProfileKindV3::fixed, 8, "11001010", "08000000ca"},
      {dt::BitStringSurfaceProfileKindV3::varying, 8, "", "00000000"},
      {dt::BitStringSurfaceProfileKindV3::varying, 8, "00110101", "0800000035"},
  }};
  unsigned admitted = 0;
  for (const auto& vector : accepted) {
    auto profile = Profile(vector.kind, vector.bound);
    const auto component = Hex(vector.component);
    const auto decoded = dt::DecodeCanonicalBitStringComponentNoAllocV3(
        profile, dt::BitStringValueStateV3::present, true, component);
    Check(decoded.ok(), "accepted canonical vector was refused");
    Check(decoded.value.logical_bit_count == vector.bits.size(),
          "accepted vector logical length changed");
    const auto encoded = dt::EncodeCanonicalBitStringComponentV3(decoded.value);
    Check(encoded.ok() && encoded.bytes == component,
          "accepted vector did not decode/re-encode identically");
    ++admitted;
  }
  Check(admitted == 15, "did not consume all 15 accepted sealed vectors");

  auto unqualified = Profile(dt::BitStringSurfaceProfileKindV3::unqualified, 1);
  unsigned rejected = 0;
  for (unsigned residual = 1; residual <= 7; ++residual) {
    const unsigned unused_bits = 8 - residual;
    const unsigned canonical = 0xaau & (0xffu << unused_bits);
    for (unsigned tail = 1; tail < (1u << unused_bits); ++tail) {
      std::array<platform::byte, 5> component{
          static_cast<platform::byte>(residual), 0, 0, 0,
          static_cast<platform::byte>(canonical | tail)};
      const auto decoded = dt::DecodeCanonicalBitStringComponentNoAllocV3(
          unqualified, dt::BitStringValueStateV3::present, true, component);
      Check(!decoded.ok() && decoded.diagnostic.diagnostic_code ==
                                 "CTB.BIT.CANONICAL_ENCODING_INVALID",
            "dirty-tail vector was admitted or misdiagnosed");
      ++rejected;
    }
  }
  Check(rejected == 247, "dirty-tail vector count is not 247");

  const auto expect_component_refusal = [&](std::span<const platform::byte> bytes,
                                            dt::BitStringValueStateV3 state,
                                            bool nullable,
                                            std::string_view code,
                                            std::string_view message) {
    const auto result = dt::DecodeCanonicalBitStringComponentNoAllocV3(
        unqualified, state, nullable, bytes);
    Check(!result.ok() && result.diagnostic.diagnostic_code == code, message);
    ++rejected;
  };
  const auto truncated0 = Hex("");
  const auto truncated3 = Hex("090000");
  const auto short9 = Hex("0900000080");
  const auto trailing8 = Hex("08000000aa00");
  expect_component_refusal(truncated0, dt::BitStringValueStateV3::present,
      true, "CTB.BIT.CANONICAL_ENCODING_INVALID", "truncated_count_0 drifted");
  expect_component_refusal(truncated3, dt::BitStringValueStateV3::present,
      true, "CTB.BIT.CANONICAL_ENCODING_INVALID", "truncated_count_3 drifted");
  expect_component_refusal(short9, dt::BitStringValueStateV3::present,
      true, "CTB.BIT.CANONICAL_ENCODING_INVALID", "short_payload_9 drifted");
  expect_component_refusal(trailing8, dt::BitStringValueStateV3::present,
      true, "CTB.BIT.CANONICAL_ENCODING_INVALID", "trailing_payload_8 drifted");
  std::vector<platform::byte> maximum_plus_one(4 + 2'097'153, 0);
  maximum_plus_one[0] = 0x01;
  maximum_plus_one[3] = 0x01;
  expect_component_refusal(maximum_plus_one,
      dt::BitStringValueStateV3::present, true, "CTB.BIT.LENGTH_EXCEEDED",
      "maximum_plus_one drifted");
  const auto null_component = Hex("00000000");
  expect_component_refusal(null_component,
      dt::BitStringValueStateV3::sql_null, true,
      "DATATYPE.NULL_STATE.INVALID", "null_with_component drifted");
  expect_component_refusal({}, dt::BitStringValueStateV3::sql_null, false,
      "DATATYPE.NULL_NOT_ADMITTED", "null_not_admitted drifted");

  const auto one_component = Hex("0100000080");
  dt::DatatypeBinaryValue raw_binary;
  raw_binary.type_id = dt::CanonicalTypeId::bit_string;
  raw_binary.payload = one_component;
  const auto raw_binary_encode = dt::EncodeDatatypeBinaryValue(raw_binary);
  Check(!raw_binary_encode.ok() &&
            raw_binary_encode.diagnostic.diagnostic_code ==
                "CTB.BIT.SERIALIZATION_PROFILE_MISSING",
        "raw_SBDVAL01_bit_encode drifted");
  ++rejected;
  std::vector<platform::byte> binary_frame(32 + one_component.size());
  dt::DatatypeBinaryValueView raw_binary_view{
      dt::CanonicalTypeId::bit_string, false, false,
      one_component.data(), one_component.size()};
  const auto structural_binary =
      dt::EncodeDatatypeBinaryStructuralValueIntoNoAlloc(
          raw_binary_view, binary_frame.data(), binary_frame.size());
  Check(structural_binary.ok(), "raw SBDVAL structural fixture failed");
  auto invalid_profile = unqualified;
  invalid_profile.receipt.registry_generation = 5;
  auto malformed_binary = binary_frame;
  malformed_binary[0] ^= 1;
  const auto malformed_binary_direct =
      dt::DecodeDatatypeBinaryStructuralValueViewNoAlloc(
          malformed_binary.data(), malformed_binary.size());
  const auto malformed_binary_composed =
      dt::DecodeBitStringSbdvalComposedNoAllocV3(
          invalid_profile, true, malformed_binary);
  Check(!malformed_binary_direct.ok() && !malformed_binary_composed.ok() &&
            malformed_binary_composed.diagnostic.diagnostic_code ==
                malformed_binary_direct.diagnostic.diagnostic_code,
        "SBDVAL structural failure was masked by invalid profile");
  Check(dt::DecodeBitStringSbdvalComposedNoAllocV3(
                invalid_profile, true, binary_frame)
                .diagnostic.diagnostic_code == "CTB.BIT.DESCRIPTOR_INVALID",
        "valid SBDVAL did not apply profile before component");
  const auto dirty_one_component = Hex("0100000081");
  std::vector<platform::byte> dirty_binary_frame(
      32 + dirty_one_component.size());
  dt::DatatypeBinaryValueView dirty_binary_view{
      dt::CanonicalTypeId::bit_string, false, false,
      dirty_one_component.data(), dirty_one_component.size()};
  Check(dt::EncodeDatatypeBinaryStructuralValueIntoNoAlloc(
            dirty_binary_view, dirty_binary_frame.data(),
            dirty_binary_frame.size()).ok(),
        "dirty SBDVAL structural fixture failed");
  Check(dt::DecodeBitStringSbdvalComposedNoAllocV3(
                invalid_profile, true, dirty_binary_frame)
                .diagnostic.diagnostic_code == "CTB.BIT.DESCRIPTOR_INVALID" &&
            dt::DecodeBitStringSbdvalComposedNoAllocV3(
                unqualified, true, dirty_binary_frame)
                .diagnostic.diagnostic_code ==
                    "CTB.BIT.CANONICAL_ENCODING_INVALID",
        "SBDVAL profile/component precedence drifted");
  const auto raw_binary_decode = dt::DecodeDatatypeBinaryValue(binary_frame);
  Check(!raw_binary_decode.ok() &&
            raw_binary_decode.diagnostic.diagnostic_code ==
                "CTB.BIT.SERIALIZATION_PROFILE_MISSING",
        "raw_SBDVAL01_bit_decode drifted");
  ++rejected;

  dt::DatatypePhysicalValue raw_physical;
  raw_physical.type_id = dt::CanonicalTypeId::bit_string;
  raw_physical.state = dt::DatatypePhysicalValueState::value;
  raw_physical.payload = one_component;
  const auto raw_physical_encode = dt::EncodeDatatypePhysicalValue(raw_physical);
  Check(!raw_physical_encode.ok() &&
            raw_physical_encode.diagnostic.diagnostic_code ==
                "CTB.BIT.SERIALIZATION_PROFILE_MISSING",
        "raw_SBDPV001_bit_encode drifted");
  ++rejected;
  std::vector<platform::byte> physical_frame(24 + one_component.size());
  dt::DatatypePhysicalValueView raw_physical_view{
      dt::CanonicalTypeId::bit_string, dt::DatatypePhysicalValueState::value,
      one_component.data(), one_component.size()};
  const auto structural_physical =
      dt::EncodeDatatypePhysicalStructuralValueIntoNoAlloc(
          raw_physical_view, physical_frame.data(), physical_frame.size());
  Check(structural_physical.ok(), "raw SBDPV structural fixture failed");
  auto malformed_physical = physical_frame;
  malformed_physical[0] ^= 1;
  const auto malformed_physical_direct =
      dt::DecodeDatatypePhysicalStructuralValueViewNoAlloc(
          malformed_physical.data(), malformed_physical.size());
  const auto malformed_physical_composed =
      dt::DecodeBitStringSbdpvComposedNoAllocV3(
          invalid_profile, true, malformed_physical);
  Check(!malformed_physical_direct.ok() && !malformed_physical_composed.ok() &&
            malformed_physical_composed.diagnostic.diagnostic_code ==
                malformed_physical_direct.diagnostic.diagnostic_code,
        "SBDPV structural failure was masked by invalid profile");
  Check(dt::DecodeBitStringSbdpvComposedNoAllocV3(
                invalid_profile, true, physical_frame)
                .diagnostic.diagnostic_code == "CTB.BIT.DESCRIPTOR_INVALID",
        "valid SBDPV did not apply profile before component");
  std::vector<platform::byte> dirty_physical_frame(
      24 + dirty_one_component.size());
  dt::DatatypePhysicalValueView dirty_physical_view{
      dt::CanonicalTypeId::bit_string,
      dt::DatatypePhysicalValueState::value,
      dirty_one_component.data(), dirty_one_component.size()};
  Check(dt::EncodeDatatypePhysicalStructuralValueIntoNoAlloc(
            dirty_physical_view, dirty_physical_frame.data(),
            dirty_physical_frame.size()).ok(),
        "dirty SBDPV structural fixture failed");
  Check(dt::DecodeBitStringSbdpvComposedNoAllocV3(
                invalid_profile, true, dirty_physical_frame)
                .diagnostic.diagnostic_code == "CTB.BIT.DESCRIPTOR_INVALID" &&
            dt::DecodeBitStringSbdpvComposedNoAllocV3(
                unqualified, true, dirty_physical_frame)
                .diagnostic.diagnostic_code ==
                    "CTB.BIT.CANONICAL_ENCODING_INVALID",
        "SBDPV profile/component precedence drifted");
  const auto raw_physical_decode = dt::DecodeDatatypePhysicalValue(
      physical_frame.data(), physical_frame.size());
  Check(!raw_physical_decode.ok() &&
            raw_physical_decode.diagnostic.diagnostic_code ==
                "CTB.BIT.SERIALIZATION_PROFILE_MISSING",
        "raw_SBDPV001_bit_decode drifted");
  ++rejected;
  Check(rejected == 258, "complete negative vector count is not 258");
}

void BoundsOwnershipAndAtomicity() {
  auto profile = Profile(dt::BitStringSurfaceProfileKindV3::unqualified, 1);
  std::vector<platform::byte> source{0x80};
  dt::BitStringValueViewV3 view{&profile, dt::BitStringValueStateV3::present,
                                1, source,
                                dt::BitStringOwnershipV3::borrowed};
  const auto owned = dt::MaterializeBitStringValueV3(view, true);
  Check(owned.ok(), "borrowed value did not materialize");
  source[0] = 0;
  Check(owned.value.packed_msb0 == std::vector<platform::byte>{0x80},
        "owned value retained borrowed storage");

  dt::BitStringValueViewV3 typed_null{
      &profile, dt::BitStringValueStateV3::sql_null, 0, {},
      dt::BitStringOwnershipV3::borrowed};
  const auto owned_null = dt::MaterializeBitStringValueV3(typed_null, true);
  Check(owned_null.ok() && owned_null.value.state ==
                               dt::BitStringValueStateV3::sql_null &&
            owned_null.value.logical_bit_count == 0 &&
            owned_null.value.packed_msb0.empty(),
        "typed NULL did not remain payload-free through materialization");

  dt::BitStringExecutionControlV3 denied;
  denied.maximum_allocation_bytes = 0;
  const auto refused = dt::MaterializeBitStringValueV3(view, true, denied);
  Check(!refused.ok() && refused.value.packed_msb0.empty() &&
            refused.diagnostic.diagnostic_code == "RESOURCE.BUDGET_EXCEEDED",
        "resource refusal published partial bytes");

  static constexpr std::array<std::uint32_t, 14> required_boundaries{
      15, 16, 17, 31, 32, 64, 256, 1'024, 4'096, 16'384, 65'536,
      262'144, 1'048'576, 4'194'304};
  for (const auto logical_bits : required_boundaries) {
    std::vector<platform::byte> storage((logical_bits + 7u) / 8u, 0xaa);
    if ((logical_bits & 7u) != 0)
      storage.back() &= static_cast<platform::byte>(
          0xffu << (8u - (logical_bits & 7u)));
    dt::BitStringValueViewV3 boundary_value{
        &profile, dt::BitStringValueStateV3::present, logical_bits, storage,
        dt::BitStringOwnershipV3::borrowed};
    const auto validated =
        dt::ValidateBitStringValueViewV3(boundary_value, true);
    const auto encoded =
        dt::EncodeCanonicalBitStringComponentV3(boundary_value);
    Check(validated.ok() && encoded.ok() &&
              encoded.bytes.size() == 4u + storage.size(),
          "required length/histogram boundary was not canonical");
  }

  std::vector<platform::byte> maximum(2'097'152, 0xff);
  dt::BitStringValueViewV3 max_view{
      &profile, dt::BitStringValueStateV3::present,
      dt::kBitStringMaximumLogicalBitsV3, maximum,
      dt::BitStringOwnershipV3::borrowed};
  Check(dt::ValidateBitStringValueViewV3(max_view, true).ok(),
        "maximum canonical value was refused");
  const auto maximum_component =
      dt::EncodeCanonicalBitStringComponentV3(max_view);
  Check(maximum_component.ok() && maximum_component.bytes.size() == 2'097'156 &&
            maximum_component.bytes[0] == 0 &&
            maximum_component.bytes[1] == 0 &&
            maximum_component.bytes[2] == 0 &&
            maximum_component.bytes[3] == 1 &&
            std::all_of(maximum_component.bytes.begin() + 4,
                        maximum_component.bytes.end(),
                        [](platform::byte b) { return b == 0xff; }),
        "maximum canonical component recipe drifted");
  const auto component_digest = scratchbird::core::hash::ComputeSha256Digest(
      maximum_component.bytes);
  Check(component_digest.ok() &&
            std::vector<platform::byte>(component_digest.digest.begin(),
                                        component_digest.digest.end()) ==
                Hex("4d9300e99759561a1adf3fd28eeb2faa55922c46a6ed6fa82e182c40113cb692"),
        "maximum canonical component hash drifted");
  Check(dt::HashBitStringValueV3(max_view).bytes ==
            Hex("dc05d507f99ff2895c6875a603f0fff38108ece197c30b4bea9f7668d2056780"),
        "maximum value hash drifted");

  std::array<bool, 4> concurrent_ok{};
  std::array<std::thread, 4> readers;
  for (std::size_t index = 0; index < readers.size(); ++index) {
    readers[index] = std::thread([&, index] {
      const auto validated = dt::ValidateBitStringValueViewV3(max_view, true);
      const auto count = dt::BitStringCountV3(max_view, true);
      concurrent_ok[index] = validated.ok() && count.ok() &&
          count.unsigned_value == dt::kBitStringMaximumLogicalBitsV3;
    });
  }
  for (auto& reader : readers) reader.join();
  Check(std::all_of(concurrent_ok.begin(), concurrent_ok.end(),
                    [](bool value) { return value; }),
        "immutable borrowed value was not safe for concurrent reads");

  auto cancel_immediately = [](void*) noexcept { return true; };
  dt::BitStringExecutionControlV3 cancelled_before;
  cancelled_before.cancelled = cancel_immediately;
  const auto before = dt::MaterializeBitStringValueV3(
      max_view, true, cancelled_before);
  Check(!before.ok() && before.diagnostic.diagnostic_code ==
                            "PROCESS.CANCELLED" &&
            before.value.packed_msb0.empty(),
        "preallocation cancellation published a value");
  unsigned cancellation_checks = 0;
  auto cancel_second = [](void* context) noexcept {
    return ++*static_cast<unsigned*>(context) == 2;
  };
  dt::BitStringExecutionControlV3 at_boundary;
  at_boundary.cancelled = cancel_second;
  at_boundary.cancellation_context = &cancellation_checks;
  const auto boundary = dt::BitStringNotV3(max_view, true, at_boundary);
  Check(!boundary.ok() && boundary.diagnostic.diagnostic_code ==
                              "PROCESS.CANCELLED" &&
            boundary.value.packed_msb0.empty() && cancellation_checks == 2,
        "65,536-bit cancellation checkpoint drifted");
}

}  // namespace

int main() {
  ProfilesAreExact();
  AcceptedAndNegativeVectors();
  BoundsOwnershipAndAtomicity();
  std::cout << "base.bit_string profile/value checks: " << checks << '\n';
  return EXIT_SUCCESS;
}

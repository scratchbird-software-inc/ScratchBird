// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0

#include "datatype_blob.hpp"

#include <array>
#include <cstdlib>
#include <iostream>
#include <string_view>

namespace {

namespace dt = scratchbird::core::datatypes;
namespace platform = scratchbird::core::platform;

unsigned checks = 0;

[[noreturn]] void Fail(std::string_view text) {
  std::cerr << "FAIL: " << text << '\n';
  std::exit(EXIT_FAILURE);
}

void Check(bool condition, std::string_view text) {
  ++checks;
  if (!condition) Fail(text);
}

constexpr platform::Uuid U(std::array<platform::byte, 16> bytes) noexcept {
  return platform::Uuid{bytes};
}

const dt::DatatypeTypeCodecIdentityRowV3& CurrentBlobIdentity() {
  const dt::DatatypeTypeCodecIdentityRowV3* found = nullptr;
  for (const auto& row : dt::CurrentDatatypeTypeCodecIdentityRowsV3()) {
    if (!dt::IsExactCanonicalBlobTypeCodecIdentityV3(row)) continue;
    Check(found == nullptr, "duplicate current blob identity");
    found = &row;
  }
  Check(found != nullptr, "current blob identity missing");
  return *found;
}

dt::BlobValidatedProfileHandleV3 Profile() {
  const auto result =
      dt::BuildCurrentBlobValidatedProfileHandleV3(dt::kBlobV11ReceiptUuid);
  Check(result.ok(), "current blob profile construction failed");
  return result.profile;
}

void ExactV11ProfileAuthority() {
  const auto profile = Profile();
  Check(dt::ValidateBlobProfileHandleV3(profile).ok(),
        "current blob profile validation failed");
  Check(profile.receipt.receipt_uuid == U({
            0x01,0xa1,0x09,0x5f,0xf2,0x05,0x72,0xd3,
            0xab,0xea,0x15,0xc6,0xd4,0x1a,0x4a,0xd4}) &&
        profile.receipt.catalog_snapshot_uuid == profile.receipt.receipt_uuid &&
        profile.receipt.catalog_generation == 11 &&
        profile.receipt.registry_generation == 11,
        "V11 receipt tuple mismatch");

  const auto& identity = profile.identity.legacy_fields;
  Check(identity.descriptor_uuid == U({
            0x01,0x6f,0xd1,0xd3,0x0d,0xaf,0x59,0x67,
            0xb4,0xd7,0x07,0xfe,0x85,0x9a,0x41,0x8e}) &&
        identity.descriptor_generation == 1,
        "blob descriptor identity mismatch");
  Check(identity.type_uuid == U({
            0x01,0xa1,0x09,0x5f,0xf2,0x05,0x7b,0x29,
            0xb6,0x79,0x2a,0xb3,0x75,0x5b,0x37,0xd2}) &&
        identity.type_generation == 1,
        "blob type identity mismatch");
  Check(identity.codec_uuid == U({
            0x01,0xa1,0x09,0x5f,0xf2,0x05,0x79,0xfe,
            0xb1,0xf0,0x29,0xf1,0xe0,0xf4,0x9b,0x05}) &&
        identity.codec_version == 1 && identity.codec_generation == 1,
        "blob codec identity mismatch");
  Check(profile.profile_uuid == U({
            0x01,0xa1,0x09,0x5f,0xf2,0x05,0x73,0x9c,
            0x81,0x66,0x22,0x1d,0xfe,0x6b,0x81,0x8d}) &&
        profile.profile_generation == 1 &&
        profile.profile_fingerprint == dt::kBlobV3ProfileFingerprint &&
        profile.profile_fingerprint_preimage_bytes == 3423,
        "blob profile identity mismatch");

  constexpr std::array<platform::Uuid, dt::kBlobPolicyBindingCountV3>
      expected_policies{{
          U({0x01,0xa1,0x09,0x5f,0xf2,0x05,0x7d,0xaf,0xb6,0xf1,0x29,0x17,0x24,0x84,0x0d,0xe4}),
          U({0x01,0xa1,0x09,0x5f,0xf2,0x05,0x7f,0xc9,0x8f,0xec,0xe6,0xad,0xa4,0x88,0x9d,0x1c}),
          U({0x01,0xa1,0x09,0x5f,0xf2,0x05,0x79,0x29,0x86,0xcf,0x34,0x23,0x5d,0x85,0xac,0xc5}),
          U({0x01,0xa1,0x09,0x5f,0xf2,0x05,0x71,0xf5,0x98,0x22,0x14,0x8c,0x23,0x85,0xb0,0x00}),
          U({0x01,0xa1,0x09,0x5f,0xf2,0x05,0x7b,0x9a,0xa7,0x17,0x68,0x01,0xdc,0xf9,0x9d,0xad}),
          U({0x01,0xa1,0x09,0x5f,0xf2,0x05,0x70,0x92,0xb6,0xce,0x7c,0x4f,0xe3,0x5a,0xa2,0xbe}),
          U({0x01,0xa1,0x09,0x5f,0xf2,0x05,0x75,0x4c,0x8d,0xe7,0xca,0xcf,0x91,0xaf,0xb2,0x58}),
          U({0x01,0xa1,0x09,0x5f,0xf2,0x05,0x77,0x0d,0xb5,0x0b,0x16,0x42,0x49,0x90,0xfe,0x25}),
          U({0x01,0xa1,0x09,0x5f,0xf2,0x05,0x71,0x84,0xad,0x9c,0x9f,0x89,0x17,0x61,0x84,0xc4}),
          U({0x01,0xa1,0x09,0x5f,0xf2,0x05,0x77,0xf1,0xaa,0xc4,0x0e,0x0b,0xd3,0x9f,0xe3,0x98}),
          U({0x01,0xa1,0x09,0x5f,0xf2,0x05,0x74,0xae,0xa0,0xcc,0x24,0xaa,0x66,0x5f,0x50,0x47}),
          U({0x01,0xa1,0x09,0x5f,0xf2,0x05,0x7c,0x23,0xa0,0xd0,0x91,0xcc,0xd2,0xa0,0x78,0xa6}),
          U({0x01,0xa1,0x09,0x5f,0xf2,0x05,0x7d,0x40,0x9f,0x48,0xd5,0x6d,0x0b,0xcf,0x34,0xb2}),
          U({0x01,0xa1,0x09,0x5f,0xf2,0x05,0x74,0xfb,0x9e,0x63,0xaa,0xa3,0x8d,0x70,0xd8,0x90}),
          U({0x01,0xa1,0x09,0x5f,0xf2,0x05,0x74,0x4b,0x99,0xd9,0xd8,0xd7,0x2f,0x10,0xe3,0xea}),
          U({0x01,0xa1,0x09,0x5f,0xf2,0x05,0x7e,0xd4,0x8d,0x29,0xf2,0x57,0x43,0x16,0x7c,0x32}),
          U({0x01,0xa1,0x09,0x5f,0xf2,0x05,0x71,0x79,0xbd,0xa3,0xca,0x90,0xf4,0x29,0x99,0xe1}),
          U({0x01,0xa1,0x09,0x5f,0xf2,0x05,0x76,0x57,0x8e,0xd2,0xa5,0x11,0xbb,0xa1,0x7b,0xe6}),
          U({0x01,0xa1,0x09,0x5f,0xf2,0x05,0x7b,0x41,0xaf,0xce,0x7b,0x75,0x81,0x8d,0x27,0x30}),
          U({0x01,0xa1,0x09,0x5f,0xf2,0x05,0x74,0x12,0x9c,0x35,0x1e,0xf8,0x8f,0xa9,0xeb,0x7f}),
          U({0x01,0xa1,0x09,0x5f,0xf2,0x05,0x79,0x29,0xbf,0x02,0x86,0xaa,0x82,0x85,0x0a,0x2c}),
      }};
  for (std::size_t index = 0; index < expected_policies.size(); ++index) {
    Check(profile.policy_bindings[index].uuid == expected_policies[index] &&
              profile.policy_bindings[index].generation == 1,
          "blob policy UUID/generation mismatch");
  }

  auto aliases = profile;
  aliases.identity.legacy_fields.canonical_name = "BLOB";
  aliases.identity.legacy_fields.codec_id = "presentation-only-codec-label";
  Check(dt::ValidateBlobProfileHandleV3(aliases).ok(),
        "presentation labels became profile authority");

  auto changed = profile;
  changed.receipt.receipt_uuid.bytes[0] ^= 1;
  Check(!dt::ValidateBlobProfileHandleV3(changed).ok(),
        "changed receipt UUID admitted");
  changed = profile;
  ++changed.receipt.catalog_generation;
  Check(!dt::ValidateBlobProfileHandleV3(changed).ok(),
        "changed catalog generation admitted");
  changed = profile;
  changed.identity.legacy_fields.descriptor_uuid.bytes[0] ^= 1;
  Check(!dt::ValidateBlobProfileHandleV3(changed).ok(),
        "changed descriptor UUID admitted");
  changed = profile;
  ++changed.identity.legacy_fields.descriptor_generation;
  Check(!dt::ValidateBlobProfileHandleV3(changed).ok(),
        "changed descriptor generation admitted");
  changed = profile;
  changed.identity.legacy_fields.type_uuid.bytes[0] ^= 1;
  Check(!dt::ValidateBlobProfileHandleV3(changed).ok(),
        "changed type UUID admitted");
  changed = profile;
  ++changed.identity.legacy_fields.type_generation;
  Check(!dt::ValidateBlobProfileHandleV3(changed).ok(),
        "changed type generation admitted");
  changed = profile;
  changed.identity.legacy_fields.codec_uuid.bytes[0] ^= 1;
  Check(!dt::ValidateBlobProfileHandleV3(changed).ok(),
        "changed codec UUID admitted");
  changed = profile;
  ++changed.identity.legacy_fields.codec_generation;
  Check(!dt::ValidateBlobProfileHandleV3(changed).ok(),
        "changed codec generation admitted");
  changed = profile;
  changed.profile_uuid.bytes[0] ^= 1;
  Check(!dt::ValidateBlobProfileHandleV3(changed).ok(),
        "changed profile UUID admitted");
  changed = profile;
  ++changed.profile_generation;
  Check(!dt::ValidateBlobProfileHandleV3(changed).ok(),
        "changed profile generation admitted");
  changed = profile;
  changed.profile_fingerprint[0] ^= 1;
  Check(!dt::ValidateBlobProfileHandleV3(changed).ok(),
        "changed profile fingerprint admitted");
  changed = profile;
  ++changed.profile_fingerprint_preimage_bytes;
  Check(!dt::ValidateBlobProfileHandleV3(changed).ok(),
        "changed profile preimage extent admitted");
  for (std::size_t index = 0; index < changed.policy_bindings.size(); ++index) {
    changed = profile;
    changed.policy_bindings[index].uuid.bytes[0] ^= 1;
    Check(!dt::ValidateBlobProfileHandleV3(changed).ok(),
          "changed policy UUID admitted");
    changed = profile;
    ++changed.policy_bindings[index].generation;
    Check(!dt::ValidateBlobProfileHandleV3(changed).ok(),
          "changed policy generation admitted");
  }

  auto wrong_receipt = dt::kBlobV11ReceiptUuid;
  wrong_receipt.bytes[15] ^= 1;
  Check(!dt::BuildCurrentBlobValidatedProfileHandleV3(wrong_receipt).ok(),
        "wrong receipt built current profile");

  auto wrong_generation_receipt = dt::BlobAuthorityReceiptV3{
      dt::kBlobV11ReceiptUuid, dt::kDatatypeCohortV11, 11, 12};
  const auto wrong_generation = dt::BuildBlobValidatedProfileHandleV3(
      wrong_generation_receipt, CurrentBlobIdentity());
  Check(!wrong_generation.ok() &&
            wrong_generation.profile.profile_generation == 0,
        "wrong receipt generation built or published profile");

  auto wrong_identity = CurrentBlobIdentity();
  wrong_identity.legacy_fields.canonical_binary_type_code = 501;
  const auto refused = dt::BuildBlobValidatedProfileHandleV3(
      {dt::kBlobV11ReceiptUuid, dt::kDatatypeCohortV11, 11, 11},
      wrong_identity);
  Check(!refused.ok() &&
            refused.diagnostic.diagnostic_code ==
                "CINL.LOB.DESCRIPTOR_INVALID",
        "wrong identity built profile");
}

void MaterializedViewStatesAndLifetimeLimit() {
  auto profile = Profile();
  const dt::BlobExecutionControlV3 zero_budget{};

  const auto empty = dt::ValidateBlobMaterializedValueViewNoAllocV3(
      {&profile, dt::BlobValueStateV3::value, 0, {}}, true, zero_budget);
  Check(empty.ok() && empty.value.profile == &profile &&
            empty.value.state == dt::BlobValueStateV3::value &&
            empty.value.logical_length == 0 && empty.value.bytes.empty(),
        "empty VALUE refused");

  const auto clean_null = dt::ValidateBlobMaterializedValueViewNoAllocV3(
      {&profile, dt::BlobValueStateV3::sql_null, 0, {}}, true, zero_budget);
  Check(clean_null.ok() &&
            clean_null.value.state == dt::BlobValueStateV3::sql_null,
        "clean SQL_NULL refused");
  Check(dt::ValidateBlobProfileHandleV3(profile, zero_budget).ok(),
        "zero budget refused allocation-free profile validation");
  const auto nonnullable = dt::ValidateBlobMaterializedValueViewNoAllocV3(
      {&profile, dt::BlobValueStateV3::sql_null, 0, {}}, false);
  Check(!nonnullable.ok() &&
            nonnullable.diagnostic.diagnostic_code == "BLOB.STATE_INVALID",
        "SQL_NULL admitted to nonnullable slot");

  const std::array<platform::byte, 3> bytes{{0x00, 0x7f, 0xff}};
  const auto dirty_null = dt::ValidateBlobMaterializedValueViewNoAllocV3(
      {&profile, dt::BlobValueStateV3::sql_null, bytes.size(), bytes}, true);
  Check(!dirty_null.ok() &&
            dirty_null.diagnostic.diagnostic_code == "BLOB.STATE_INVALID" &&
            dirty_null.value.profile == nullptr &&
            dirty_null.value.bytes.empty(),
        "dirty SQL_NULL published content");

  const auto nonempty = dt::ValidateBlobMaterializedValueViewNoAllocV3(
      {&profile, dt::BlobValueStateV3::value, bytes.size(), bytes}, true);
  Check(!nonempty.ok() &&
            nonempty.diagnostic.diagnostic_code ==
                "CINL.LOB.DESCRIPTOR_INVALID" &&
            nonempty.diagnostic.detail ==
                "nonempty_value_requires_published_lifetime_authority" &&
            nonempty.value.profile == nullptr && nonempty.value.bytes.empty(),
        "nonempty VALUE bypassed missing lifetime authority");

  const auto mismatched_nonempty =
      dt::ValidateBlobMaterializedValueViewNoAllocV3(
          {&profile, dt::BlobValueStateV3::value, 1, {}}, true);
  Check(!mismatched_nonempty.ok() &&
            mismatched_nonempty.diagnostic.detail ==
                "nonempty_value_requires_published_lifetime_authority",
        "declared nonempty VALUE bypassed lifetime gate");

  const auto extent_mismatch = dt::ValidateBlobMaterializedValueViewNoAllocV3(
      {&profile, dt::BlobValueStateV3::value, bytes.size() + 1, bytes}, true,
      zero_budget);
  Check(!extent_mismatch.ok() &&
            extent_mismatch.diagnostic.detail ==
                "nonempty_value_requires_published_lifetime_authority" &&
            extent_mismatch.value.profile == nullptr,
        "extent mismatch bypassed the earlier lifetime gate");

  const auto above_maximum =
      dt::ValidateBlobMaterializedValueViewNoAllocV3(
          {&profile, dt::BlobValueStateV3::value,
           dt::kBlobMaximumLogicalBytesV3 + 1, {}},
          true, zero_budget);
  Check(!above_maximum.ok() &&
            above_maximum.diagnostic.detail ==
                "nonempty_value_requires_published_lifetime_authority" &&
            above_maximum.value.profile == nullptr,
        ">INT64_MAX claim bypassed the earlier lifetime gate");

  const auto invalid_state = dt::ValidateBlobMaterializedValueViewNoAllocV3(
      {&profile, static_cast<dt::BlobValueStateV3>(0xff), bytes.size(), bytes},
      true);
  Check(!invalid_state.ok() &&
            invalid_state.diagnostic.diagnostic_code == "BLOB.STATE_INVALID",
        "invalid state did not precede content admission");

  auto invalid_profile = profile;
  invalid_profile.profile_fingerprint[0] ^= 1;
  const auto bad_profile = dt::ValidateBlobMaterializedValueViewNoAllocV3(
      {&invalid_profile, static_cast<dt::BlobValueStateV3>(0xff),
       bytes.size(), bytes},
      true);
  Check(!bad_profile.ok() &&
            bad_profile.diagnostic.diagnostic_code ==
                "CINL.LOB.DESCRIPTOR_INVALID",
        "profile authority did not precede state/content");
}

}  // namespace

int main() {
  ExactV11ProfileAuthority();
  MaterializedViewStatesAndLifetimeLimit();
  std::cout << "PASS base.blob profile/value checks=" << checks << '\n';
  return EXIT_SUCCESS;
}

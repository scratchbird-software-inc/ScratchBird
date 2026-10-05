// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0

#include "datatype_blob.hpp"

#include <array>
#include <utility>

namespace scratchbird::core::datatypes {
namespace {

using platform::Severity;
using platform::Status;
using platform::StatusCode;
using platform::Subsystem;

constexpr platform::Uuid U(std::array<byte, 16> bytes) noexcept {
  return platform::Uuid{bytes};
}

constexpr DatatypePolicyIdentityV3 P(std::array<byte, 16> bytes) noexcept {
  return {U(bytes), 1};
}

inline constexpr std::array<DatatypePolicyIdentityV3,
                            kBlobPolicyBindingCountV3>
    kExpectedPolicies{{
        P({0x01,0xa1,0x09,0x5f,0xf2,0x05,0x7d,0xaf,
           0xb6,0xf1,0x29,0x17,0x24,0x84,0x0d,0xe4}),
        P({0x01,0xa1,0x09,0x5f,0xf2,0x05,0x7f,0xc9,
           0x8f,0xec,0xe6,0xad,0xa4,0x88,0x9d,0x1c}),
        P({0x01,0xa1,0x09,0x5f,0xf2,0x05,0x79,0x29,
           0x86,0xcf,0x34,0x23,0x5d,0x85,0xac,0xc5}),
        P({0x01,0xa1,0x09,0x5f,0xf2,0x05,0x71,0xf5,
           0x98,0x22,0x14,0x8c,0x23,0x85,0xb0,0x00}),
        P({0x01,0xa1,0x09,0x5f,0xf2,0x05,0x7b,0x9a,
           0xa7,0x17,0x68,0x01,0xdc,0xf9,0x9d,0xad}),
        P({0x01,0xa1,0x09,0x5f,0xf2,0x05,0x70,0x92,
           0xb6,0xce,0x7c,0x4f,0xe3,0x5a,0xa2,0xbe}),
        P({0x01,0xa1,0x09,0x5f,0xf2,0x05,0x75,0x4c,
           0x8d,0xe7,0xca,0xcf,0x91,0xaf,0xb2,0x58}),
        P({0x01,0xa1,0x09,0x5f,0xf2,0x05,0x77,0x0d,
           0xb5,0x0b,0x16,0x42,0x49,0x90,0xfe,0x25}),
        P({0x01,0xa1,0x09,0x5f,0xf2,0x05,0x71,0x84,
           0xad,0x9c,0x9f,0x89,0x17,0x61,0x84,0xc4}),
        P({0x01,0xa1,0x09,0x5f,0xf2,0x05,0x77,0xf1,
           0xaa,0xc4,0x0e,0x0b,0xd3,0x9f,0xe3,0x98}),
        P({0x01,0xa1,0x09,0x5f,0xf2,0x05,0x74,0xae,
           0xa0,0xcc,0x24,0xaa,0x66,0x5f,0x50,0x47}),
        P({0x01,0xa1,0x09,0x5f,0xf2,0x05,0x7c,0x23,
           0xa0,0xd0,0x91,0xcc,0xd2,0xa0,0x78,0xa6}),
        P({0x01,0xa1,0x09,0x5f,0xf2,0x05,0x7d,0x40,
           0x9f,0x48,0xd5,0x6d,0x0b,0xcf,0x34,0xb2}),
        P({0x01,0xa1,0x09,0x5f,0xf2,0x05,0x74,0xfb,
           0x9e,0x63,0xaa,0xa3,0x8d,0x70,0xd8,0x90}),
        P({0x01,0xa1,0x09,0x5f,0xf2,0x05,0x74,0x4b,
           0x99,0xd9,0xd8,0xd7,0x2f,0x10,0xe3,0xea}),
        P({0x01,0xa1,0x09,0x5f,0xf2,0x05,0x7e,0xd4,
           0x8d,0x29,0xf2,0x57,0x43,0x16,0x7c,0x32}),
        P({0x01,0xa1,0x09,0x5f,0xf2,0x05,0x71,0x79,
           0xbd,0xa3,0xca,0x90,0xf4,0x29,0x99,0xe1}),
        P({0x01,0xa1,0x09,0x5f,0xf2,0x05,0x76,0x57,
           0x8e,0xd2,0xa5,0x11,0xbb,0xa1,0x7b,0xe6}),
        P({0x01,0xa1,0x09,0x5f,0xf2,0x05,0x7b,0x41,
           0xaf,0xce,0x7b,0x75,0x81,0x8d,0x27,0x30}),
        P({0x01,0xa1,0x09,0x5f,0xf2,0x05,0x74,0x12,
           0x9c,0x35,0x1e,0xf8,0x8f,0xa9,0xeb,0x7f}),
        P({0x01,0xa1,0x09,0x5f,0xf2,0x05,0x79,0x29,
           0xbf,0x02,0x86,0xaa,0x82,0x85,0x0a,0x2c}),
    }};

Status OkStatus() noexcept {
  return {StatusCode::ok, Severity::info, Subsystem::datatypes};
}

Status ErrorStatus() noexcept {
  return {StatusCode::platform_required_feature_missing, Severity::error,
          Subsystem::datatypes};
}

Status ResourceStatus() noexcept {
  return {StatusCode::memory_allocation_failed, Severity::error,
          Subsystem::datatypes};
}

template <typename Result>
Result Success() noexcept {
  Result result;
  result.status = OkStatus();
  result.diagnostic.status = result.status;
  return result;
}

template <typename Result>
Result Failure(std::string_view code, std::string_view detail,
               Status status = ErrorStatus()) noexcept {
  Result result;
  result.status = status;
  result.diagnostic.status = status;
  result.diagnostic.diagnostic_code = code;
  result.diagnostic.detail = detail;
  return result;
}

bool SamePolicy(const DatatypePolicyIdentityV3& left,
                const DatatypePolicyIdentityV3& right) noexcept {
  return left.uuid == right.uuid && left.generation == right.generation;
}

bool Cancelled(const BlobExecutionControlV3& control) noexcept {
  return control.cancelled != nullptr &&
         control.cancelled(control.cancellation_context);
}

bool ExactReceipt(const BlobAuthorityReceiptV3& receipt) noexcept {
  return receipt.receipt_uuid == kBlobV11ReceiptUuid &&
         receipt.catalog_snapshot_uuid == kDatatypeCohortV11 &&
         receipt.catalog_generation == 11 &&
         receipt.registry_generation == 11;
}

const DatatypeTypeCodecIdentityRowV3* CurrentBlobIdentity() noexcept {
  const DatatypeTypeCodecIdentityRowV3* found = nullptr;
  for (const auto& row : CurrentDatatypeTypeCodecIdentityRowsV3()) {
    if (!IsExactCanonicalBlobTypeCodecIdentityV3(row)) continue;
    if (found != nullptr) return nullptr;
    found = &row;
  }
  return found;
}

bool ExactProfile(const BlobValidatedProfileHandleV3& profile) noexcept {
  if (!ExactReceipt(profile.receipt) ||
      !IsExactCanonicalBlobTypeCodecIdentityV3(profile.identity) ||
      profile.identity.legacy_fields.catalog_snapshot_uuid !=
          profile.receipt.catalog_snapshot_uuid ||
      profile.identity.legacy_fields.catalog_generation !=
          profile.receipt.catalog_generation ||
      profile.identity.legacy_fields.registry_generation !=
          profile.receipt.registry_generation ||
      profile.profile_uuid != kBlobV3ProfileUuid ||
      profile.profile_generation != 1 ||
      profile.profile_fingerprint != kBlobV3ProfileFingerprint ||
      profile.profile_fingerprint_preimage_bytes !=
          kBlobProfileFingerprintPreimageBytesV3) {
    return false;
  }

  for (std::size_t index = 0; index < kExpectedPolicies.size(); ++index) {
    if (!SamePolicy(profile.policy_bindings[index],
                    kExpectedPolicies[index])) {
      return false;
    }
  }

  return SamePolicy(profile.policy_bindings[
                        static_cast<std::size_t>(BlobPolicyKindV3::descriptor)],
                    profile.identity.descriptor_policy) &&
         SamePolicy(profile.policy_bindings[static_cast<std::size_t>(
                        BlobPolicyKindV3::canonicalization)],
                    profile.identity.canonicalization_policy) &&
         SamePolicy(profile.policy_bindings[
                        static_cast<std::size_t>(BlobPolicyKindV3::comparison)],
                    profile.identity.ordering_policy) &&
         SamePolicy(profile.policy_bindings[
                        static_cast<std::size_t>(BlobPolicyKindV3::hash)],
                    profile.identity.hash_policy) &&
         SamePolicy(profile.policy_bindings[
                        static_cast<std::size_t>(BlobPolicyKindV3::operation)],
                    profile.identity.operation_policy) &&
         profile.identity.native_fields.policy_profile_uuid ==
             profile.profile_uuid &&
         profile.identity.native_fields.policy_profile_generation ==
             profile.profile_generation &&
         profile.identity.native_fields.profile_fingerprint_sha256 ==
             profile.profile_fingerprint;
}

// This gate is intentionally isolated. It must be replaced only by the exact
// published token/authority validation contract; no pointer, enum, name, or
// raw span can satisfy lifetime authority in this interim tranche.
bool NonemptyMaterializedLifetimeAuthorityAvailable() noexcept {
  return false;
}

}  // namespace

BlobProfileResultV3 BuildBlobValidatedProfileHandleV3(
    const BlobAuthorityReceiptV3& receipt,
    const DatatypeTypeCodecIdentityRowV3& identity) noexcept {
  if (!ExactReceipt(receipt) ||
      !IsExactCanonicalBlobTypeCodecIdentityV3(identity) ||
      identity.legacy_fields.catalog_snapshot_uuid !=
          receipt.catalog_snapshot_uuid ||
      identity.legacy_fields.catalog_generation != receipt.catalog_generation ||
      identity.legacy_fields.registry_generation !=
          receipt.registry_generation) {
    return Failure<BlobProfileResultV3>("CINL.LOB.DESCRIPTOR_INVALID",
                                        "receipt_or_identity_invalid");
  }

  try {
    auto result = Success<BlobProfileResultV3>();
    result.profile.receipt = receipt;
    result.profile.identity = identity;
    result.profile.profile_uuid = kBlobV3ProfileUuid;
    result.profile.profile_generation = 1;
    result.profile.policy_bindings = kExpectedPolicies;
    result.profile.profile_fingerprint = kBlobV3ProfileFingerprint;
    result.profile.profile_fingerprint_preimage_bytes =
        kBlobProfileFingerprintPreimageBytesV3;
    if (!ExactProfile(result.profile)) {
      return Failure<BlobProfileResultV3>("CINL.LOB.DESCRIPTOR_INVALID",
                                          "constructed_profile_invalid");
    }
    return result;
  } catch (...) {
    return Failure<BlobProfileResultV3>("RESOURCE.BUDGET_EXCEEDED",
                                        "profile_allocation",
                                        ResourceStatus());
  }
}

BlobProfileResultV3 BuildCurrentBlobValidatedProfileHandleV3(
    const platform::Uuid& receipt_uuid) noexcept {
  if (receipt_uuid != kBlobV11ReceiptUuid) {
    return Failure<BlobProfileResultV3>("CINL.LOB.DESCRIPTOR_INVALID",
                                        "receipt_uuid_invalid");
  }
  const auto* identity = CurrentBlobIdentity();
  if (identity == nullptr) {
    return Failure<BlobProfileResultV3>("CINL.LOB.DESCRIPTOR_INVALID",
                                        "current_identity_missing_or_ambiguous");
  }
  return BuildBlobValidatedProfileHandleV3(
      {receipt_uuid, kDatatypeCohortV11, 11, 11}, *identity);
}

BlobValidationResultV3 ValidateBlobProfileHandleV3(
    const BlobValidatedProfileHandleV3& profile,
    const BlobExecutionControlV3& control) noexcept {
  if (!ExactProfile(profile)) {
    return Failure<BlobValidationResultV3>("CINL.LOB.DESCRIPTOR_INVALID",
                                           "profile_invalid");
  }
  if (Cancelled(control)) {
    return Failure<BlobValidationResultV3>("PROCESS.CANCELLED",
                                           "before_publication");
  }
  return Success<BlobValidationResultV3>();
}

BlobMaterializedViewResultV3 ValidateBlobMaterializedValueViewNoAllocV3(
    const BlobMaterializedValueViewV3& value, bool null_allowed,
    const BlobExecutionControlV3& control) noexcept {
  if (value.profile == nullptr || !ExactProfile(*value.profile)) {
    return Failure<BlobMaterializedViewResultV3>(
        "CINL.LOB.DESCRIPTOR_INVALID", "profile_missing_or_invalid");
  }
  if (value.state != BlobValueStateV3::value &&
      value.state != BlobValueStateV3::sql_null) {
    return Failure<BlobMaterializedViewResultV3>("BLOB.STATE_INVALID",
                                                 "state_invalid");
  }

  if (value.state == BlobValueStateV3::sql_null) {
    if (value.logical_length != 0 || !value.bytes.empty()) {
      return Failure<BlobMaterializedViewResultV3>(
          "BLOB.STATE_INVALID", "sql_null_has_content");
    }
    if (!null_allowed) {
      return Failure<BlobMaterializedViewResultV3>(
          "BLOB.STATE_INVALID", "sql_null_not_admitted");
    }
    if (Cancelled(control)) {
      return Failure<BlobMaterializedViewResultV3>("PROCESS.CANCELLED",
                                                   "before_publication");
    }
    auto result = Success<BlobMaterializedViewResultV3>();
    result.value = value;
    return result;
  }

  if (value.logical_length != 0 || !value.bytes.empty()) {
    if (!NonemptyMaterializedLifetimeAuthorityAvailable()) {
      return Failure<BlobMaterializedViewResultV3>(
          "CINL.LOB.DESCRIPTOR_INVALID",
          "nonempty_value_requires_published_lifetime_authority");
    }
  }

  // Reachable for empty VALUE in this tranche. The checks remain here so the
  // exact lifetime gate can later admit nonempty content without changing the
  // structural validation or publication path.
  if (value.logical_length > kBlobMaximumLogicalBytesV3) {
    return Failure<BlobMaterializedViewResultV3>("BLOB.LENGTH_EXCEEDED",
                                                 "logical_length_exceeded");
  }
  if (value.logical_length != static_cast<u64>(value.bytes.size())) {
    return Failure<BlobMaterializedViewResultV3>(
        "BLOB.CANONICAL_ENCODING_INVALID", "length_extent_mismatch");
  }

  if (Cancelled(control)) {
    return Failure<BlobMaterializedViewResultV3>("PROCESS.CANCELLED",
                                                 "before_publication");
  }

  auto result = Success<BlobMaterializedViewResultV3>();
  result.value = value;
  return result;
}

}  // namespace scratchbird::core::datatypes

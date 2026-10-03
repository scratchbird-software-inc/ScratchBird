// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0

#include "datatype_bit_string.hpp"

#include "datatype_binary_view.hpp"
#include "datatype_physical_encoding.hpp"
#include "../hash/hash_digest_parts.hpp"

#include <algorithm>
#include <atomic>
#include <bit>
#include <cstring>
#include <limits>
#include <new>
#include <type_traits>

namespace scratchbird::core::datatypes {
namespace {

using platform::LoadLittle16;
using platform::LoadLittle32;
using platform::LoadLittle64;
using platform::Severity;
using platform::StatusCode;
using platform::StoreLittle16;
using platform::StoreLittle32;
using platform::StoreLittle64;
using platform::Subsystem;

constexpr platform::Uuid U(std::array<byte, 16> bytes) { return {bytes}; }

inline constexpr platform::Uuid kSnapshot = U(
    {0x01,0x9d,0x00,0x00,0x00,0x00,0x70,0x00,0x80,0x00,0x00,0x00,0x00,0x00,0xd7,0x06});
inline constexpr platform::Uuid kDescriptor = U(
    {0x01,0x9d,0x00,0x00,0x00,0x00,0x70,0x00,0x80,0x00,0x00,0x00,0x00,0x00,0xd8,0x29});
inline constexpr platform::Uuid kType = U(
    {0x01,0x9d,0x00,0x00,0x00,0x00,0x70,0x00,0x80,0x00,0x00,0x00,0x00,0x00,0xd8,0x2a});
inline constexpr platform::Uuid kCodec = U(
    {0x01,0x9d,0x00,0x00,0x00,0x00,0x70,0x00,0x80,0x00,0x00,0x00,0x00,0x00,0xd8,0x2b});

inline constexpr DatatypePolicyIdentityV1 kDescriptorPolicy{U(
    {0x01,0xa0,0xff,0x27,0x27,0x15,0x75,0xd2,0x98,0xfb,0xc8,0x53,0x52,0x7d,0x38,0x11}),1};
inline constexpr DatatypePolicyIdentityV1 kCanonicalPolicy{U(
    {0x01,0xa0,0xff,0x27,0x27,0x16,0x7c,0x83,0x9a,0xe4,0x23,0xbb,0xc7,0x3e,0x6a,0x9d}),1};
inline constexpr DatatypePolicyIdentityV1 kOrderingPolicy{U(
    {0x01,0xa0,0xff,0x27,0x27,0x17,0x7a,0x54,0xbd,0xbc,0xa3,0xc1,0xfe,0xe7,0xc5,0xe3}),1};
inline constexpr DatatypePolicyIdentityV1 kHashPolicy{U(
    {0x01,0xa0,0xff,0x27,0x27,0x18,0x7e,0x29,0xb4,0xb3,0x7d,0xe1,0x71,0x97,0x09,0xb8}),1};
inline constexpr DatatypePolicyIdentityV1 kRenderPolicy{U(
    {0x01,0xa0,0xff,0x27,0x27,0x19,0x79,0x13,0xb1,0x96,0x67,0x8c,0x49,0x21,0x97,0x4c}),1};
inline constexpr DatatypePolicyIdentityV1 kCastPolicy{U(
    {0x01,0xa0,0xff,0x27,0x27,0x1a,0x79,0x5d,0x9a,0x2d,0xeb,0x04,0x9b,0xb8,0x77,0x4d}),1};
inline constexpr DatatypePolicyIdentityV1 kOperationPolicy{U(
    {0x01,0xa0,0xff,0x27,0x27,0x1b,0x74,0xde,0x8b,0xc6,0xd6,0xa9,0x34,0x84,0xac,0x36}),1};
inline constexpr DatatypePolicyIdentityV1 kBooleanPolicy{U(
    {0x01,0xa0,0xff,0x27,0x27,0x1c,0x75,0xfc,0x82,0x33,0xfc,0xfe,0x8d,0x8e,0xfe,0x86}),1};
inline constexpr DatatypePolicyIdentityV1 kLobPolicy{U(
    {0x01,0xa0,0xff,0x27,0x27,0x1d,0x7d,0x48,0x82,0xd6,0x68,0x02,0x77,0xcf,0x06,0x2b}),1};
inline constexpr DatatypePolicyIdentityV1 kIndexPolicy{U(
    {0x01,0xa0,0xff,0x27,0x27,0x1e,0x72,0x0b,0x89,0xd1,0x65,0x8c,0x0b,0xbf,0xb4,0x0f}),1};
inline constexpr DatatypePolicyIdentityV1 kStatisticsPolicy{U(
    {0x01,0xa0,0xff,0x27,0x27,0x1f,0x74,0x85,0xa5,0x1d,0x78,0xc5,0xf1,0x4f,0xa3,0xfb}),1};
inline constexpr DatatypePolicyIdentityV1 kBackupPolicy{U(
    {0x01,0xa0,0xff,0x27,0x27,0x20,0x75,0x19,0xae,0x33,0xae,0x20,0x42,0xf1,0x0a,0x28}),1};
inline constexpr DatatypePolicyIdentityV1 kFixedPadding{U(
    {0x01,0xa0,0xff,0x27,0x27,0x21,0x7e,0xaa,0xbf,0x38,0x55,0x07,0x48,0xcd,0x0c,0xf0}),1};
inline constexpr DatatypePolicyIdentityV1 kProtectionPolicy{U(
    {0x01,0xa0,0xff,0x27,0x27,0x22,0x74,0xfe,0xb4,0x44,0x17,0x18,0x82,0xc7,0x23,0x40}),1};
inline constexpr DatatypePolicyIdentityV1 kNoPadding{U(
    {0x01,0xa0,0xff,0x27,0x27,0x23,0x7a,0x11,0x8b,0x22,0x33,0x44,0x55,0x66,0x77,0x88}),1};

constexpr bool Same(const DatatypePolicyIdentityV1& a,
                    const DatatypePolicyIdentityV1& b) noexcept {
  return a.uuid == b.uuid && a.generation == b.generation;
}

bool IsNil(const platform::Uuid& uuid) noexcept {
  return std::all_of(uuid.bytes.begin(), uuid.bytes.end(),
                     [](byte b) { return b == 0; });
}

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

template <class Result>
Result Failure(std::string_view code, std::string_view detail,
               Status status = ErrorStatus()) noexcept {
  Result result;
  result.status = status;
  if constexpr (std::is_same_v<Result, BitStringViewResultV1> ||
                std::is_same_v<Result, BitStringSearchResultV1> ||
                std::is_same_v<Result, BitStringSortKeyViewResultV1>) {
    result.diagnostic.status = status;
    result.diagnostic.diagnostic_code = code;
    result.diagnostic.detail = detail;
  } else {
    try {
      result.diagnostic = MakeBitStringDiagnosticV1(
          status, std::string(code), "datatype.bit_string.rejected",
          std::string(detail));
    } catch (...) {
      result.diagnostic.status = status;
    }
  }
  return result;
}

template <class Result>
Result Success() noexcept {
  Result result;
  result.status = OkStatus();
  result.diagnostic.status = result.status;
  return result;
}

bool Cancelled(const BitStringExecutionControlV1& c) noexcept {
  return c.cancelled != nullptr && c.cancelled(c.cancellation_context);
}

bool CancellationCheckpoint(const BitStringExecutionControlV1& c,
                            u64 processed_bits) noexcept {
  return processed_bits != 0 && (processed_bits % 65'536u) == 0
      ? Cancelled(c) : false;
}

void PutUuid(byte*& p, const platform::Uuid& uuid) noexcept {
  std::memcpy(p, uuid.bytes.data(), 16);
  p += 16;
}
void PutU32(byte*& p, u32 value) noexcept { StoreLittle32(p, value); p += 4; }
void PutU64(byte*& p, u64 value) noexcept { StoreLittle64(p, value); p += 8; }
void PutPolicy(byte*& p, const DatatypePolicyIdentityV1& policy) noexcept {
  PutUuid(p, policy.uuid);
  PutU64(p, policy.generation);
}

std::array<byte, kBitStringProfileMaterialBytesV1> BuildMaterial(
    const BitStringDescriptorProfileV1& profile) noexcept {
  std::array<byte, kBitStringProfileMaterialBytesV1> out{};
  byte* p = out.data();
  const auto& row = profile.identity.legacy_fields;
  PutUuid(p, profile.receipt.catalog_snapshot_uuid);
  PutU64(p, profile.receipt.catalog_generation);
  PutU64(p, profile.receipt.registry_generation);
  PutUuid(p, row.descriptor_uuid); PutU64(p, row.descriptor_generation);
  PutUuid(p, row.type_uuid); PutU64(p, row.type_generation);
  PutUuid(p, row.codec_uuid); PutU32(p, row.codec_version); PutU32(p, 0);
  PutU64(p, row.codec_generation);
  PutU32(p, profile.length_bits);
  *p++ = profile.fixed_length ? 1 : 0;
  p += 3;
  PutPolicy(p, profile.padding);
  PutPolicy(p, profile.identity.canonicalization_policy);
  PutPolicy(p, profile.identity.ordering_policy);
  PutPolicy(p, profile.identity.hash_policy);
  PutPolicy(p, profile.identity.operation_policy);
  PutPolicy(p, profile.boolean_alias);
  PutPolicy(p, profile.render);
  PutPolicy(p, profile.cast);
  PutPolicy(p, profile.no_auto_lob);
  PutPolicy(p, profile.index);
  PutPolicy(p, profile.statistics);
  PutPolicy(p, profile.backup_transport);
  PutPolicy(p, profile.protection);
  return out;
}

bool Digest(std::string_view domain, const byte* material,
            std::size_t material_bytes, std::array<byte, 32>* output) noexcept {
  const hash::HashDigestSegment parts[] = {
      {reinterpret_cast<const byte*>(domain.data()), domain.size()},
      {material, material_bytes},
  };
  const auto digest = hash::ComputeSha256DigestPartsNative(parts, 2);
  if (!digest.ok()) return false;
  *output = digest.digest;
  return true;
}

bool ComputeFingerprints(BitStringDescriptorProfileV1* profile) noexcept {
  profile->canonical_profile_material = BuildMaterial(*profile);
  return Digest("ScratchBird.BitString.Profile.V1",
                profile->canonical_profile_material.data(),
                profile->canonical_profile_material.size(),
                &profile->profile_fingerprint) &&
      Digest("ScratchBird.BitString.ComparisonCohort.V1",
             profile->canonical_profile_material.data(),
             kBitStringComparisonMaterialBytesV1,
             &profile->comparison_cohort_fingerprint);
}

bool ExactReceipt(const BitStringAuthorityReceiptV1& receipt) noexcept;
bool ExactExtraPolicies(const BitStringDescriptorProfileV1& profile) noexcept;

bool ProfileValidNoAlloc(const BitStringDescriptorProfileV1& profile) noexcept {
  if (!ExactReceipt(profile.receipt) ||
      !IsExactCanonicalBitStringTypeCodecIdentityV3(profile.identity) ||
      profile.identity.legacy_fields.catalog_snapshot_uuid !=
          profile.receipt.catalog_snapshot_uuid ||
      profile.identity.legacy_fields.catalog_generation !=
          profile.receipt.catalog_generation ||
      profile.identity.legacy_fields.registry_generation !=
          profile.receipt.registry_generation ||
      profile.identity.legacy_fields.descriptor_uuid != kDescriptor ||
      profile.identity.legacy_fields.type_uuid != kType ||
      profile.identity.legacy_fields.codec_uuid != kCodec ||
      !Same(profile.identity.descriptor_policy, kDescriptorPolicy) ||
      !Same(profile.identity.canonicalization_policy, kCanonicalPolicy) ||
      !Same(profile.identity.ordering_policy, kOrderingPolicy) ||
      !Same(profile.identity.hash_policy, kHashPolicy) ||
      !Same(profile.identity.operation_policy, kOperationPolicy) ||
      !ExactExtraPolicies(profile) || profile.length_bits == 0 ||
      profile.length_bits > kBitStringMaximumLogicalBitsV1 ||
      !Same(profile.padding, profile.fixed_length ? kFixedPadding : kNoPadding))
    return false;
  const auto material = BuildMaterial(profile);
  std::array<byte, 32> fingerprint{}, cohort{};
  return material == profile.canonical_profile_material &&
      Digest("ScratchBird.BitString.Profile.V1", material.data(),
             material.size(), &fingerprint) &&
      Digest("ScratchBird.BitString.ComparisonCohort.V1", material.data(),
             kBitStringComparisonMaterialBytesV1, &cohort) &&
      fingerprint == profile.profile_fingerprint &&
      cohort == profile.comparison_cohort_fingerprint;
}

bool ExactReceipt(const BitStringAuthorityReceiptV1& receipt) noexcept {
  return !IsNil(receipt.statement_receipt_uuid) &&
      receipt.catalog_snapshot_uuid == kSnapshot &&
      receipt.catalog_generation == 6 && receipt.registry_generation == 6;
}

bool ExactExtraPolicies(const BitStringDescriptorProfileV1& p) noexcept {
  return Same(p.render, kRenderPolicy) && Same(p.cast, kCastPolicy) &&
      Same(p.boolean_alias, kBooleanPolicy) && Same(p.no_auto_lob, kLobPolicy) &&
      Same(p.index, kIndexPolicy) && Same(p.statistics, kStatisticsPolicy) &&
      Same(p.backup_transport, kBackupPolicy) &&
      Same(p.protection, kProtectionPolicy);
}

bool SameProfile(const BitStringDescriptorProfileV1& a,
                 const BitStringDescriptorProfileV1& b) noexcept {
  return a.profile_fingerprint == b.profile_fingerprint &&
      a.canonical_profile_material == b.canonical_profile_material;
}

bool SameCohort(const BitStringDescriptorProfileV1& a,
                const BitStringDescriptorProfileV1& b) noexcept {
  return a.comparison_cohort_fingerprint == b.comparison_cohort_fingerprint &&
      std::equal(a.canonical_profile_material.begin(),
                 a.canonical_profile_material.begin() +
                     kBitStringComparisonMaterialBytesV1,
                 b.canonical_profile_material.begin());
}

std::size_t PackedBytes(u64 bits) noexcept {
  return static_cast<std::size_t>((bits + 7u) / 8u);
}

bool ReadBit(const BitStringValueViewV1& value, u64 index) noexcept {
  return (value.packed_msb0[static_cast<std::size_t>(index / 8)] &
          static_cast<byte>(0x80u >> (index % 8))) != 0;
}
void WriteBit(std::vector<byte>* bytes, u64 index, bool bit) noexcept {
  auto& target = (*bytes)[static_cast<std::size_t>(index / 8)];
  const byte mask = static_cast<byte>(0x80u >> (index % 8));
  if (bit) target = static_cast<byte>(target | mask);
  else target = static_cast<byte>(target & static_cast<byte>(~mask));
}

template <class Result>
Result ValidateFor(const BitStringValueViewV1& value, bool null_allowed) noexcept {
  const auto validated = ValidateBitStringValueViewV1(value, null_allowed);
  if (!validated.ok()) {
    return Failure<Result>(validated.diagnostic.diagnostic_code,
                           validated.diagnostic.detail, validated.status);
  }
  return Success<Result>();
}

BitStringViewResultV1 ValidateProfilePhase(
    const BitStringValueViewV1& value) noexcept {
  if (value.profile == nullptr)
    return Failure<BitStringViewResultV1>("CTB.BIT.DESCRIPTOR_INVALID",
                                          "profile_missing");
  if (!ProfileValidNoAlloc(*value.profile))
    return Failure<BitStringViewResultV1>("CTB.BIT.DESCRIPTOR_INVALID",
                                          "profile_invalid");
  auto result = Success<BitStringViewResultV1>();
  result.value = value;
  return result;
}

BitStringViewResultV1 ValidateStatePhase(
    const BitStringValueViewV1& value, bool null_allowed) noexcept {
  if (value.state != BitStringValueStateV1::present &&
      value.state != BitStringValueStateV1::sql_null)
    return Failure<BitStringViewResultV1>("DATATYPE.NULL_STATE.INVALID",
                                          "value_state_invalid");
  if (value.state == BitStringValueStateV1::sql_null) {
    if (value.logical_bit_count != 0 || !value.packed_msb0.empty())
      return Failure<BitStringViewResultV1>("DATATYPE.NULL_STATE.INVALID",
                                            "sql_null_carries_payload");
    if (!null_allowed)
      return Failure<BitStringViewResultV1>("DATATYPE.NULL_NOT_ADMITTED",
                                            "slot_nonnullable");
  }
  auto result = Success<BitStringViewResultV1>();
  result.value = value;
  return result;
}

BitStringResultV1 AllocateLike(const BitStringValueViewV1& source,
                               u32 count,
                               const BitStringDescriptorProfileV1& profile,
                               const BitStringExecutionControlV1& control) noexcept {
  if (Cancelled(control))
    return Failure<BitStringResultV1>("PROCESS.CANCELLED", "before_allocation");
  const auto bytes = PackedBytes(count);
  if (bytes > control.maximum_allocation_bytes)
    return Failure<BitStringResultV1>("RESOURCE.BUDGET_EXCEEDED",
                                      "allocation_grant_too_small",
                                      ResourceStatus());
  auto result = Success<BitStringResultV1>();
  try {
    result.value.profile = profile;
    result.value.state = source.state;
    result.value.logical_bit_count = source.state == BitStringValueStateV1::present
        ? count : 0;
    if (source.state == BitStringValueStateV1::present)
      result.value.packed_msb0.assign(bytes, 0);
  } catch (const std::bad_alloc&) {
    return Failure<BitStringResultV1>("RESOURCE.BUDGET_EXCEEDED",
                                      "allocation_failed", ResourceStatus());
  }
  return result;
}

BitStringResultV1 UnaryCopy(const BitStringValueViewV1& value,
                            bool null_allowed,
                            const BitStringExecutionControlV1& control,
                            bool invert) noexcept {
  auto checked = ValidateFor<BitStringResultV1>(value, null_allowed);
  if (!checked.ok()) return checked;
  auto result = AllocateLike(value, value.logical_bit_count, *value.profile, control);
  if (!result.ok() || value.state == BitStringValueStateV1::sql_null) return result;
  for (u64 i = 0; i < value.logical_bit_count; ++i) {
    WriteBit(&result.value.packed_msb0, i, ReadBit(value, i) != invert ? true : false);
    if (CancellationCheckpoint(control, i + 1u))
      return Failure<BitStringResultV1>("PROCESS.CANCELLED", "operation_checkpoint");
  }
  return result;
}

enum class BinaryOp { and_op, or_op, xor_op };
BitStringResultV1 Binary(const BitStringValueViewV1& left,
                         const BitStringValueViewV1& right,
                         bool null_allowed,
                         const BitStringExecutionControlV1& control,
                         BinaryOp op) noexcept {
  const auto left_profile = ValidateProfilePhase(left);
  if (!left_profile.ok())
    return Failure<BitStringResultV1>(left_profile.diagnostic.diagnostic_code,
                                      left_profile.diagnostic.detail,
                                      left_profile.status);
  const auto right_profile = ValidateProfilePhase(right);
  if (!right_profile.ok())
    return Failure<BitStringResultV1>(right_profile.diagnostic.diagnostic_code,
                                      right_profile.diagnostic.detail,
                                      right_profile.status);
  if (!SameProfile(*left.profile, *right.profile)) {
    return Failure<BitStringResultV1>("CTB.BIT.OPERATION_INCOMPATIBLE",
                                      "profile_or_length_mismatch");
  }
  const auto left_state = ValidateStatePhase(left, null_allowed);
  if (!left_state.ok())
    return Failure<BitStringResultV1>(left_state.diagnostic.diagnostic_code,
                                      left_state.diagnostic.detail,
                                      left_state.status);
  const auto right_state = ValidateStatePhase(right, null_allowed);
  if (!right_state.ok())
    return Failure<BitStringResultV1>(right_state.diagnostic.diagnostic_code,
                                      right_state.diagnostic.detail,
                                      right_state.status);
  if (left.state == BitStringValueStateV1::sql_null ||
      right.state == BitStringValueStateV1::sql_null) {
    BitStringValueViewV1 n{left.profile, BitStringValueStateV1::sql_null, 0, {},
                           BitStringOwnershipV1::borrowed};
    return AllocateLike(n, 0, *left.profile, control);
  }
  auto l = ValidateFor<BitStringResultV1>(left, null_allowed);
  if (!l.ok()) return l;
  auto r = ValidateFor<BitStringResultV1>(right, null_allowed);
  if (!r.ok()) return r;
  if (left.logical_bit_count != right.logical_bit_count)
    return Failure<BitStringResultV1>("CTB.BIT.OPERATION_INCOMPATIBLE",
                                      "profile_or_length_mismatch");
  auto result = AllocateLike(left, left.logical_bit_count, *left.profile, control);
  if (!result.ok()) return result;
  for (u64 i = 0; i < left.logical_bit_count; ++i) {
    const bool a = ReadBit(left, i), b = ReadBit(right, i);
    WriteBit(&result.value.packed_msb0, i,
             op == BinaryOp::and_op ? a && b :
             op == BinaryOp::or_op ? a || b : a != b);
    if (CancellationCheckpoint(control, i + 1u))
      return Failure<BitStringResultV1>("PROCESS.CANCELLED", "operation_checkpoint");
  }
  return result;
}

bool IntegerType(CanonicalTypeId id) noexcept {
  switch (id) {
    case CanonicalTypeId::int8: case CanonicalTypeId::uint8:
    case CanonicalTypeId::int16: case CanonicalTypeId::uint16:
    case CanonicalTypeId::int32: case CanonicalTypeId::uint32:
    case CanonicalTypeId::int64: case CanonicalTypeId::uint64:
    case CanonicalTypeId::int128: case CanonicalTypeId::uint128: return true;
    default: return false;
  }
}
bool SignedInteger(CanonicalTypeId id) noexcept {
  return id == CanonicalTypeId::int8 || id == CanonicalTypeId::int16 ||
      id == CanonicalTypeId::int32 || id == CanonicalTypeId::int64 ||
      id == CanonicalTypeId::int128;
}
std::size_t IntegerBytes(CanonicalTypeId id) noexcept {
  switch (id) {
    case CanonicalTypeId::int8: case CanonicalTypeId::uint8: return 1;
    case CanonicalTypeId::int16: case CanonicalTypeId::uint16: return 2;
    case CanonicalTypeId::int32: case CanonicalTypeId::uint32: return 4;
    case CanonicalTypeId::int64: case CanonicalTypeId::uint64: return 8;
    case CanonicalTypeId::int128: case CanonicalTypeId::uint128: return 16;
    default: return 0;
  }
}

bool EngineUuidEqual(const scratchbird::engine::Uuid& a,
                     const platform::Uuid& b) noexcept {
  return std::equal(std::begin(a.bytes), std::end(a.bytes), b.bytes.begin());
}
bool EngineUuidNil(const scratchbird::engine::Uuid& value) noexcept {
  return std::all_of(std::begin(value.bytes), std::end(value.bytes),
                     [](byte b) { return b == 0; });
}
bool ExactScalarDescriptor(const scratchbird::engine::ExecutionTypeDescriptor& d,
                           CanonicalTypeId id,
                           const BitStringAuthorityReceiptV1& receipt) noexcept {
  const auto canonical = LookupDatatypeDescriptor(id);
  if (!ExactReceipt(receipt) || !canonical.ok()) return false;
  DatatypeCatalogManifestResult manifest;
  try {
    manifest = LoadCurrentCoreDatatypeCatalogManifest();
  } catch (...) {
    return false;
  }
  if (!manifest.ok()) return false;
  const auto catalog_row = LookupDatatypeCatalogRow(manifest.manifest, id);
  if (!catalog_row.ok() || catalog_row.manifest.descriptor_rows.size() != 1)
    return false;
  const auto& live = catalog_row.manifest.descriptor_rows.front();
  if (!live.descriptor_uuid.valid() || live.descriptor_epoch == 0 ||
      !live.descriptor_authoritative)
    return false;
  const bool length_parameterized = id == CanonicalTypeId::character;
  u64 expected_flags = 0;
  if (canonical.descriptor.default_precision != 0)
    expected_flags |= scratchbird::engine::ExecutionTypeModifierFlagBit(
        scratchbird::engine::ExecutionTypeModifierFlag::precision);
  if (canonical.descriptor.default_scale != 0 ||
      id == CanonicalTypeId::decimal || id == CanonicalTypeId::decimal_float)
    expected_flags |= scratchbird::engine::ExecutionTypeModifierFlagBit(
        scratchbird::engine::ExecutionTypeModifierFlag::scale);
  if (d.length != 0)
    expected_flags |= scratchbird::engine::ExecutionTypeModifierFlagBit(
        scratchbird::engine::ExecutionTypeModifierFlag::length);
  return
      EngineUuidEqual(d.descriptor_uuid, live.descriptor_uuid.value) &&
      d.descriptor_epoch == live.descriptor_epoch &&
      d.canonical_type_id == static_cast<u32>(id) &&
      d.family == static_cast<scratchbird::engine::ExecutionTypeFamily>(
          canonical.descriptor.family) &&
      d.width_class == static_cast<scratchbird::engine::ExecutionTypeWidthClass>(
          canonical.descriptor.width_class) &&
      d.bit_width == canonical.descriptor.bit_width &&
      d.precision == canonical.descriptor.default_precision &&
      d.scale == canonical.descriptor.default_scale &&
      ((!length_parameterized && d.length == 0) ||
       (length_parameterized && d.length <= 16'777'216)) &&
      d.vector_dimensions == 0 && d.container_rank == 0 &&
      d.modifier_flags == expected_flags &&
      EngineUuidNil(d.domain_uuid) && d.domain_stack.empty() &&
      EngineUuidNil(d.charset_uuid) && EngineUuidNil(d.collation_uuid) &&
      EngineUuidNil(d.timezone_uuid) &&
      EngineUuidNil(d.element_descriptor_uuid) &&
      EngineUuidNil(d.security_policy_uuid) && d.nullable_allowed &&
      d.descriptor_authoritative && d.parser_independent;
}

bool ExactScalarValue(const DatatypeOperationValue& v,
                      const BitStringAuthorityReceiptV1& receipt) noexcept {
  if (!ExactScalarDescriptor(v.descriptor, v.type_id, receipt)) return false;
  if (v.is_null) return v.encoded_value.empty();
  if (IntegerType(v.type_id)) return v.encoded_value.size() == IntegerBytes(v.type_id);
  if (v.type_id == CanonicalTypeId::character) return true;
  return false;
}

std::atomic<BitStringMetricSinkV1> g_metric_sink{nullptr};
std::atomic<void*> g_metric_context{nullptr};
thread_local bool g_emitting_metric = false;

}  // namespace

DiagnosticRecord MakeBitStringDiagnosticV1(Status status,
                                           std::string diagnostic_code,
                                           std::string message_key,
                                           std::string detail) {
  return MakeDatatypeOperationDiagnostic(status, std::move(diagnostic_code),
                                         std::move(message_key),
                                         std::move(detail));
}

BitStringProfileResultV1 BuildBitStringDescriptorProfileV1(
    const BitStringProfileRequestV1& request) noexcept {
  if (!ExactReceipt(request.receipt) ||
      !IsExactCanonicalBitStringTypeCodecIdentityV3(request.identity) ||
      request.identity.legacy_fields.catalog_snapshot_uuid !=
          request.receipt.catalog_snapshot_uuid ||
      request.identity.legacy_fields.catalog_generation !=
          request.receipt.catalog_generation ||
      request.identity.legacy_fields.registry_generation !=
          request.receipt.registry_generation) {
    return Failure<BitStringProfileResultV1>("CTB.BIT.DESCRIPTOR_INVALID",
                                             "receipt_or_v3_identity_invalid");
  }
  if (request.kind != BitStringSurfaceProfileKindV1::unqualified &&
      request.kind != BitStringSurfaceProfileKindV1::fixed &&
      request.kind != BitStringSurfaceProfileKindV1::varying)
    return Failure<BitStringProfileResultV1>("CTB.BIT.DESCRIPTOR_INVALID",
                                             "surface_profile_kind_invalid");
  const u32 length = request.kind == BitStringSurfaceProfileKindV1::unqualified
      ? kBitStringMaximumLogicalBitsV1 : request.length_bits;
  if (length == 0 || length > kBitStringMaximumLogicalBitsV1)
    return Failure<BitStringProfileResultV1>("CTB.BIT.DESCRIPTOR_INVALID",
                                             "descriptor_length_invalid");
  auto result = Success<BitStringProfileResultV1>();
  try {
    auto& p = result.profile;
    p.receipt = request.receipt;
    p.identity = request.identity;
    p.length_bits = length;
    p.fixed_length = request.kind == BitStringSurfaceProfileKindV1::fixed;
    p.padding = p.fixed_length ? kFixedPadding : kNoPadding;
    p.render = kRenderPolicy; p.cast = kCastPolicy;
    p.boolean_alias = kBooleanPolicy; p.no_auto_lob = kLobPolicy;
    p.index = kIndexPolicy; p.statistics = kStatisticsPolicy;
    p.backup_transport = kBackupPolicy; p.protection = kProtectionPolicy;
    if (!ComputeFingerprints(&p))
      return Failure<BitStringProfileResultV1>("RESOURCE.BUDGET_EXCEEDED",
                                               "sha256_provider_failed",
                                               ResourceStatus());
  } catch (const std::bad_alloc&) {
    return Failure<BitStringProfileResultV1>("RESOURCE.BUDGET_EXCEEDED",
                                             "profile_allocation_failed",
                                             ResourceStatus());
  }
  return result;
}

BitStringProfileResultV1 ValidateBitStringDescriptorProfileV1(
    const BitStringDescriptorProfileV1& profile) noexcept {
  if (!ProfileValidNoAlloc(profile)) {
    return Failure<BitStringProfileResultV1>("CTB.BIT.DESCRIPTOR_INVALID",
                                             "profile_identity_or_policy_invalid");
  }
  auto result = Success<BitStringProfileResultV1>();
  try { result.profile = profile; } catch (const std::bad_alloc&) {
    return Failure<BitStringProfileResultV1>("RESOURCE.BUDGET_EXCEEDED",
                                             "profile_result_allocation_failed",
                                             ResourceStatus());
  }
  return result;
}

BitStringProfileResultV1 DecodeBitStringDescriptorProfileMaterialV1(
    const BitStringAuthorityReceiptV1& receipt,
    const DatatypeTypeCodecIdentityRowV3& identity,
    std::span<const byte> material) noexcept {
  if (material.size() != kBitStringProfileMaterialBytesV1)
    return Failure<BitStringProfileResultV1>("CTB.BIT.DESCRIPTOR_INVALID",
                                             "profile_material_extent_invalid");
  if (std::memcmp(material.data(), receipt.catalog_snapshot_uuid.bytes.data(), 16) ||
      LoadLittle64(material.data() + 16) != receipt.catalog_generation ||
      LoadLittle64(material.data() + 24) != receipt.registry_generation)
    return Failure<BitStringProfileResultV1>("CTB.BIT.DESCRIPTOR_INVALID",
                                             "profile_receipt_mismatch");
  const u32 length = LoadLittle32(material.data() + 112);
  const byte fixed = material[116];
  if (fixed > 1 || material[117] || material[118] || material[119])
    return Failure<BitStringProfileResultV1>("CTB.BIT.DESCRIPTOR_INVALID",
                                             "profile_reserved_or_fixed_invalid");
  BitStringProfileRequestV1 request{receipt, identity,
      fixed ? BitStringSurfaceProfileKindV1::fixed
            : BitStringSurfaceProfileKindV1::varying,
      length};
  auto result = BuildBitStringDescriptorProfileV1(request);
  if (!result.ok()) return result;
  if (!std::equal(material.begin(), material.end(),
                  result.profile.canonical_profile_material.begin()))
    return Failure<BitStringProfileResultV1>("CTB.BIT.DESCRIPTOR_INVALID",
                                             "profile_unknown_or_substituted_field");
  return result;
}

BitStringViewResultV1 ValidateBitStringValueViewV1(
    const BitStringValueViewV1& value, bool null_allowed) noexcept {
  if (value.profile == nullptr)
    return Failure<BitStringViewResultV1>("CTB.BIT.DESCRIPTOR_INVALID",
                                          "profile_missing");
  if (!ProfileValidNoAlloc(*value.profile))
    return Failure<BitStringViewResultV1>("CTB.BIT.DESCRIPTOR_INVALID",
                                          "profile_invalid");
  if (value.state != BitStringValueStateV1::present &&
      value.state != BitStringValueStateV1::sql_null)
    return Failure<BitStringViewResultV1>("DATATYPE.NULL_STATE.INVALID",
                                          "value_state_invalid");
  if (value.state == BitStringValueStateV1::sql_null) {
    if (value.logical_bit_count != 0 || !value.packed_msb0.empty())
      return Failure<BitStringViewResultV1>("DATATYPE.NULL_STATE.INVALID",
                                            "sql_null_carries_payload");
    if (!null_allowed)
      return Failure<BitStringViewResultV1>("DATATYPE.NULL_NOT_ADMITTED",
                                            "slot_nonnullable");
    auto result = Success<BitStringViewResultV1>(); result.value = value;
    return result;
  }
  if (value.logical_bit_count > value.profile->length_bits ||
      value.logical_bit_count > kBitStringMaximumLogicalBitsV1)
    return Failure<BitStringViewResultV1>("CTB.BIT.LENGTH_EXCEEDED",
                                          "logical_count_exceeds_profile");
  if (value.profile->fixed_length &&
      value.logical_bit_count != value.profile->length_bits)
    return Failure<BitStringViewResultV1>("CTB.BIT.CANONICAL_ENCODING_INVALID",
                                          "fixed_profile_length_mismatch");
  if (value.packed_msb0.size() != PackedBytes(value.logical_bit_count))
    return Failure<BitStringViewResultV1>("CTB.BIT.CANONICAL_ENCODING_INVALID",
                                          "packed_extent_mismatch");
  const auto residual = value.logical_bit_count & 7u;
  if (residual != 0 && !value.packed_msb0.empty()) {
    const byte unused = static_cast<byte>((1u << (8u - residual)) - 1u);
    if ((value.packed_msb0.back() & unused) != 0)
      return Failure<BitStringViewResultV1>("CTB.BIT.CANONICAL_ENCODING_INVALID",
                                            "dirty_unused_tail");
  }
  auto result = Success<BitStringViewResultV1>(); result.value = value;
  return result;
}

BitStringViewResultV1 DecodeCanonicalBitStringComponentNoAllocV1(
    const BitStringDescriptorProfileV1& profile, BitStringValueStateV1 state,
    bool null_allowed, std::span<const byte> component) noexcept {
  if (!ProfileValidNoAlloc(profile))
    return Failure<BitStringViewResultV1>("CTB.BIT.DESCRIPTOR_INVALID",
                                          "component_profile_invalid");
  if (state == BitStringValueStateV1::sql_null) {
    BitStringValueViewV1 value{&profile, state, 0, {},
                               BitStringOwnershipV1::borrowed};
    if (!component.empty())
      return Failure<BitStringViewResultV1>("DATATYPE.NULL_STATE.INVALID",
                                            "sql_null_component_nonempty");
    return ValidateBitStringValueViewV1(value, null_allowed);
  }
  if (state != BitStringValueStateV1::present)
    return Failure<BitStringViewResultV1>("DATATYPE.NULL_STATE.INVALID",
                                          "component_state_invalid");
  if (component.size() < 4)
    return Failure<BitStringViewResultV1>("CTB.BIT.CANONICAL_ENCODING_INVALID",
                                          "component_header_truncated");
  const u32 count = LoadLittle32(component.data());
  BitStringValueViewV1 value{&profile, state, count, component.subspan(4),
                             BitStringOwnershipV1::borrowed};
  return ValidateBitStringValueViewV1(value, null_allowed);
}

BitStringBytesResultV1 EncodeCanonicalBitStringComponentV1(
    const BitStringValueViewV1& value,
    const BitStringExecutionControlV1& control) noexcept {
  const auto checked = ValidateBitStringValueViewV1(value, true);
  if (!checked.ok()) {
    auto result = Failure<BitStringBytesResultV1>(checked.diagnostic.diagnostic_code,
                                                  "value_invalid", checked.status);
    return result;
  }
  if (value.state == BitStringValueStateV1::sql_null)
    return Success<BitStringBytesResultV1>();
  const u64 required = 4u + value.packed_msb0.size();
  if (required > control.maximum_allocation_bytes)
    return Failure<BitStringBytesResultV1>("RESOURCE.BUDGET_EXCEEDED",
                                           "component_grant_too_small",
                                           ResourceStatus());
  if (Cancelled(control))
    return Failure<BitStringBytesResultV1>("PROCESS.CANCELLED", "before_allocation");
  auto result = Success<BitStringBytesResultV1>();
  try {
    result.bytes.resize(static_cast<std::size_t>(required));
    StoreLittle32(result.bytes.data(), value.logical_bit_count);
    for (std::size_t index = 0; index < value.packed_msb0.size(); ++index) {
      result.bytes[index + 4] = value.packed_msb0[index];
      const u64 processed = std::min<u64>((index + 1u) * 8u,
                                          value.logical_bit_count);
      if (CancellationCheckpoint(control, processed))
        return Failure<BitStringBytesResultV1>("PROCESS.CANCELLED",
                                                "component_checkpoint");
    }
  } catch (const std::bad_alloc&) {
    return Failure<BitStringBytesResultV1>("RESOURCE.BUDGET_EXCEEDED",
                                           "component_allocation_failed",
                                           ResourceStatus());
  }
  return result;
}

BitStringResultV1 MaterializeBitStringValueV1(
    const BitStringValueViewV1& value, bool null_allowed,
    const BitStringExecutionControlV1& control) noexcept {
  auto checked = ValidateFor<BitStringResultV1>(value, null_allowed);
  if (!checked.ok()) return checked;
  auto result = AllocateLike(value, value.logical_bit_count, *value.profile, control);
  if (!result.ok() || value.state == BitStringValueStateV1::sql_null) return result;
  for (std::size_t index = 0; index < value.packed_msb0.size(); ++index) {
    result.value.packed_msb0[index] = value.packed_msb0[index];
    const u64 processed = std::min<u64>((index + 1u) * 8u,
                                        value.logical_bit_count);
    if (CancellationCheckpoint(control, processed))
      return Failure<BitStringResultV1>("PROCESS.CANCELLED",
                                         "materialization_checkpoint");
  }
  return result;
}

BitStringScalarResultV1 BitStringLogicalLengthV1(
    const BitStringValueViewV1& value, bool null_allowed) noexcept {
  auto result = ValidateFor<BitStringScalarResultV1>(value, null_allowed);
  if (!result.ok()) return result;
  result.is_null = value.state == BitStringValueStateV1::sql_null;
  result.unsigned_value = result.is_null ? 0 : value.logical_bit_count;
  return result;
}

BitStringScalarResultV1 BitStringAtV1(const BitStringValueViewV1& value,
                                     bool null_allowed, u64 index) noexcept {
  auto result = ValidateFor<BitStringScalarResultV1>(value, null_allowed);
  if (!result.ok()) return result;
  if (value.state == BitStringValueStateV1::sql_null) { result.is_null = true; return result; }
  if (index >= value.logical_bit_count)
    return Failure<BitStringScalarResultV1>("SCALAR.OUT_OF_RANGE",
                                            "bit_index_out_of_range");
  result.boolean_value = ReadBit(value, index);
  return result;
}

BitStringScalarResultV1 BitStringCountV1(
    const BitStringValueViewV1& value, bool null_allowed,
    const BitStringExecutionControlV1& control) noexcept {
  auto result = ValidateFor<BitStringScalarResultV1>(value, null_allowed);
  if (!result.ok()) return result;
  if (value.state == BitStringValueStateV1::sql_null) { result.is_null = true; return result; }
  if (Cancelled(control))
    return Failure<BitStringScalarResultV1>("PROCESS.CANCELLED",
                                             "before_count");
  u64 count = 0;
  for (std::size_t index = 0; index < value.packed_msb0.size(); ++index) {
    count += std::popcount(value.packed_msb0[index]);
    const u64 processed = std::min<u64>((index + 1u) * 8u,
                                        value.logical_bit_count);
    if (CancellationCheckpoint(control, processed))
      return Failure<BitStringScalarResultV1>("PROCESS.CANCELLED",
                                               "count_checkpoint");
  }
  result.unsigned_value = count;
  return result;
}

BitStringResultV1 BitStringSetV1(
    const BitStringValueViewV1& value, bool null_allowed, u64 index,
    bool bit, const BitStringExecutionControlV1& control) noexcept {
  auto checked = ValidateFor<BitStringResultV1>(value, null_allowed);
  if (!checked.ok()) return checked;
  if (value.state == BitStringValueStateV1::sql_null)
    return MaterializeBitStringValueV1(value, null_allowed, control);
  if (index >= value.logical_bit_count)
    return Failure<BitStringResultV1>("SCALAR.OUT_OF_RANGE",
                                      "bit_index_out_of_range");
  auto result = MaterializeBitStringValueV1(value, null_allowed, control);
  if (result.ok()) WriteBit(&result.value.packed_msb0, index, bit);
  return result;
}

BitStringResultV1 BitStringConcatenateV1(
    const BitStringValueViewV1& left, const BitStringValueViewV1& right,
    const BitStringDescriptorProfileV1& target, bool null_allowed,
    const BitStringExecutionControlV1& control) noexcept {
  const auto left_profile = ValidateProfilePhase(left);
  if (!left_profile.ok())
    return Failure<BitStringResultV1>(left_profile.diagnostic.diagnostic_code,
                                      left_profile.diagnostic.detail,
                                      left_profile.status);
  const auto right_profile = ValidateProfilePhase(right);
  if (!right_profile.ok())
    return Failure<BitStringResultV1>(right_profile.diagnostic.diagnostic_code,
                                      right_profile.diagnostic.detail,
                                      right_profile.status);
  if (!ProfileValidNoAlloc(target) || target.fixed_length ||
      target.length_bits != kBitStringMaximumLogicalBitsV1 ||
      target.receipt.catalog_snapshot_uuid != left.profile->receipt.catalog_snapshot_uuid ||
      target.receipt.catalog_snapshot_uuid != right.profile->receipt.catalog_snapshot_uuid)
    return Failure<BitStringResultV1>("CTB.BIT.OPERATION_INCOMPATIBLE",
                                      "concatenate_target_profile_invalid");
  if (!SameProfile(*left.profile, *right.profile))
    return Failure<BitStringResultV1>("CTB.BIT.OPERATION_INCOMPATIBLE",
                                      "concatenate_source_profile_mismatch");
  const auto left_state = ValidateStatePhase(left, null_allowed);
  if (!left_state.ok())
    return Failure<BitStringResultV1>(left_state.diagnostic.diagnostic_code,
                                      left_state.diagnostic.detail,
                                      left_state.status);
  const auto right_state = ValidateStatePhase(right, null_allowed);
  if (!right_state.ok())
    return Failure<BitStringResultV1>(right_state.diagnostic.diagnostic_code,
                                      right_state.diagnostic.detail,
                                      right_state.status);
  if (left.state == BitStringValueStateV1::sql_null ||
      right.state == BitStringValueStateV1::sql_null) {
    BitStringValueViewV1 n{&target, BitStringValueStateV1::sql_null,0,{},
                           BitStringOwnershipV1::borrowed};
    return AllocateLike(n, 0, target, control);
  }
  auto l = ValidateFor<BitStringResultV1>(left, null_allowed); if (!l.ok()) return l;
  auto r = ValidateFor<BitStringResultV1>(right, null_allowed); if (!r.ok()) return r;
  const u64 count = static_cast<u64>(left.logical_bit_count) + right.logical_bit_count;
  if (count > kBitStringMaximumLogicalBitsV1)
    return Failure<BitStringResultV1>("CTB.BIT.LENGTH_EXCEEDED",
                                      "concatenate_length_exceeded");
  auto result = AllocateLike(left, static_cast<u32>(count), target, control);
  if (!result.ok()) return result;
  for (u64 i=0;i<count;++i) {
    WriteBit(&result.value.packed_msb0,i,
             i < left.logical_bit_count ? ReadBit(left,i)
                                        : ReadBit(right,i-left.logical_bit_count));
    if (CancellationCheckpoint(control,i + 1u))
      return Failure<BitStringResultV1>("PROCESS.CANCELLED","operation_checkpoint");
  }
  return result;
}

BitStringResultV1 BitStringSliceV1(
    const BitStringValueViewV1& value,
    const BitStringDescriptorProfileV1& target, bool null_allowed,
    u64 start, u64 count, const BitStringExecutionControlV1& control) noexcept {
  auto checked = ValidateFor<BitStringResultV1>(value, null_allowed); if (!checked.ok()) return checked;
  const auto target_ok = ValidateBitStringDescriptorProfileV1(target);
  if (!target_ok.ok() || target.fixed_length ||
      target.length_bits != kBitStringMaximumLogicalBitsV1)
    return Failure<BitStringResultV1>("CTB.BIT.OPERATION_INCOMPATIBLE",
                                      "slice_target_profile_invalid");
  if (value.state == BitStringValueStateV1::sql_null) {
    BitStringValueViewV1 n{&target,BitStringValueStateV1::sql_null,0,{},BitStringOwnershipV1::borrowed};
    return AllocateLike(n,0,target,control);
  }
  if (start > value.logical_bit_count || count > value.logical_bit_count-start)
    return Failure<BitStringResultV1>("SCALAR.OUT_OF_RANGE","slice_not_contained");
  auto result=AllocateLike(value,static_cast<u32>(count),target,control); if(!result.ok()) return result;
  for(u64 i=0;i<count;++i){
    WriteBit(&result.value.packed_msb0,i,ReadBit(value,start+i));
    if(CancellationCheckpoint(control,i + 1u)) return Failure<BitStringResultV1>("PROCESS.CANCELLED","operation_checkpoint");
  }
  return result;
}

BitStringResultV1 BitStringNotV1(const BitStringValueViewV1& value,
                                 bool null_allowed,
                                 const BitStringExecutionControlV1& control) noexcept {
  return UnaryCopy(value,null_allowed,control,true);
}
BitStringResultV1 BitStringAndV1(const BitStringValueViewV1& a,const BitStringValueViewV1& b,bool n,const BitStringExecutionControlV1& c) noexcept{return Binary(a,b,n,c,BinaryOp::and_op);}
BitStringResultV1 BitStringOrV1(const BitStringValueViewV1& a,const BitStringValueViewV1& b,bool n,const BitStringExecutionControlV1& c) noexcept{return Binary(a,b,n,c,BinaryOp::or_op);}
BitStringResultV1 BitStringXorV1(const BitStringValueViewV1& a,const BitStringValueViewV1& b,bool n,const BitStringExecutionControlV1& c) noexcept{return Binary(a,b,n,c,BinaryOp::xor_op);}

BitStringResultV1 BitStringShiftLeftV1(const BitStringValueViewV1& value,bool null_allowed,u64 count,const BitStringExecutionControlV1& control) noexcept {
  auto checked=ValidateFor<BitStringResultV1>(value,null_allowed);if(!checked.ok())return checked;
  auto result=AllocateLike(value,value.logical_bit_count,*value.profile,control);if(!result.ok()||value.state==BitStringValueStateV1::sql_null)return result;
  for(u64 i=0;i<value.logical_bit_count;++i){WriteBit(&result.value.packed_msb0,i,count<value.logical_bit_count&&i+count<value.logical_bit_count?ReadBit(value,i+count):false);if(CancellationCheckpoint(control,i + 1u))return Failure<BitStringResultV1>("PROCESS.CANCELLED","operation_checkpoint");}return result;
}
BitStringResultV1 BitStringShiftRightV1(const BitStringValueViewV1& value,bool null_allowed,u64 count,const BitStringExecutionControlV1& control) noexcept {
  auto checked=ValidateFor<BitStringResultV1>(value,null_allowed);if(!checked.ok())return checked;
  auto result=AllocateLike(value,value.logical_bit_count,*value.profile,control);if(!result.ok()||value.state==BitStringValueStateV1::sql_null)return result;
  for(u64 i=0;i<value.logical_bit_count;++i){WriteBit(&result.value.packed_msb0,i,count<value.logical_bit_count&&i>=count?ReadBit(value,i-count):false);if(CancellationCheckpoint(control,i + 1u))return Failure<BitStringResultV1>("PROCESS.CANCELLED","operation_checkpoint");}return result;
}

BitStringSearchResultV1 SearchBitStringValueNoAllocV1(
    const BitStringValueViewV1& haystack,
    const BitStringValueViewV1& needle, bool null_allowed,
    const BitStringExecutionControlV1& control) noexcept {
  const auto haystack_profile = ValidateProfilePhase(haystack);
  if (!haystack_profile.ok())
    return Failure<BitStringSearchResultV1>(
        haystack_profile.diagnostic.diagnostic_code,
        haystack_profile.diagnostic.detail, haystack_profile.status);
  const auto needle_profile = ValidateProfilePhase(needle);
  if (!needle_profile.ok())
    return Failure<BitStringSearchResultV1>(
        needle_profile.diagnostic.diagnostic_code,
        needle_profile.diagnostic.detail, needle_profile.status);
  if (!SameCohort(*haystack.profile, *needle.profile))
    return Failure<BitStringSearchResultV1>(
        "CTB.BIT.OPERATION_INCOMPATIBLE", "search_profile_mismatch");
  const auto haystack_state = ValidateStatePhase(haystack, null_allowed);
  if (!haystack_state.ok())
    return Failure<BitStringSearchResultV1>(
        haystack_state.diagnostic.diagnostic_code,
        haystack_state.diagnostic.detail, haystack_state.status);
  const auto needle_state = ValidateStatePhase(needle, null_allowed);
  if (!needle_state.ok())
    return Failure<BitStringSearchResultV1>(
        needle_state.diagnostic.diagnostic_code,
        needle_state.diagnostic.detail, needle_state.status);
  auto result = Success<BitStringSearchResultV1>();
  if (haystack.state == BitStringValueStateV1::sql_null ||
      needle.state == BitStringValueStateV1::sql_null) {
    result.is_null = true;
    return result;
  }
  const auto haystack_valid = ValidateBitStringValueViewV1(haystack, null_allowed);
  if (!haystack_valid.ok())
    return Failure<BitStringSearchResultV1>(
        haystack_valid.diagnostic.diagnostic_code,
        haystack_valid.diagnostic.detail, haystack_valid.status);
  const auto needle_valid = ValidateBitStringValueViewV1(needle, null_allowed);
  if (!needle_valid.ok())
    return Failure<BitStringSearchResultV1>(
        needle_valid.diagnostic.diagnostic_code,
        needle_valid.diagnostic.detail, needle_valid.status);
  if (needle.logical_bit_count == 0)
    return Failure<BitStringSearchResultV1>(
        "CTB.BIT.OPERATION_INCOMPATIBLE", "empty_search_needle_refused");
  if (Cancelled(control))
    return Failure<BitStringSearchResultV1>("PROCESS.CANCELLED",
                                             "before_search");
  if (needle.logical_bit_count > haystack.logical_bit_count) return result;
  u64 processed = 0;
  const u64 final_start = haystack.logical_bit_count - needle.logical_bit_count;
  for (u64 start = 0; start <= final_start; ++start) {
    bool equal = true;
    for (u64 index = 0; index < needle.logical_bit_count; ++index) {
      if (++processed % 65'536u == 0 && Cancelled(control))
        return Failure<BitStringSearchResultV1>(
            "PROCESS.CANCELLED", "search_checkpoint");
      if (ReadBit(haystack, start + index) != ReadBit(needle, index)) {
        equal = false;
        break;
      }
    }
    if (equal) {
      result.found = true;
      result.zero_based_position = start;
      return result;
    }
  }
  return result;
}

BitStringComparisonResultV1 CompareBitStringValuesV1(
    const BitStringValueViewV1& left, const BitStringValueViewV1& right,
    BitStringComparisonModeV1 mode) noexcept {
  const auto left_profile = ValidateProfilePhase(left);
  if (!left_profile.ok())
    return Failure<BitStringComparisonResultV1>(
        left_profile.diagnostic.diagnostic_code,
        left_profile.diagnostic.detail, left_profile.status);
  const auto right_profile = ValidateProfilePhase(right);
  if (!right_profile.ok())
    return Failure<BitStringComparisonResultV1>(
        right_profile.diagnostic.diagnostic_code,
        right_profile.diagnostic.detail, right_profile.status);
  if (!SameCohort(*left.profile, *right.profile))
    return Failure<BitStringComparisonResultV1>(
        "CTB.BIT.ORDERING_REFUSED", "comparison_cohort_mismatch");
  if (mode != BitStringComparisonModeV1::scalar_3vl &&
      mode != BitStringComparisonModeV1::distinct &&
      mode != BitStringComparisonModeV1::grouping &&
      mode != BitStringComparisonModeV1::ordered &&
      mode != BitStringComparisonModeV1::present_only)
    return Failure<BitStringComparisonResultV1>(
        "CTB.BIT.OPERATION_INCOMPATIBLE", "comparison_mode_invalid");
  const auto left_state = ValidateStatePhase(left, true);
  if (!left_state.ok())
    return Failure<BitStringComparisonResultV1>(
        left_state.diagnostic.diagnostic_code,
        left_state.diagnostic.detail, left_state.status);
  const auto right_state = ValidateStatePhase(right, true);
  if (!right_state.ok())
    return Failure<BitStringComparisonResultV1>(
        right_state.diagnostic.diagnostic_code,
        right_state.diagnostic.detail, right_state.status);
  if (mode == BitStringComparisonModeV1::present_only &&
      (left.state == BitStringValueStateV1::sql_null ||
       right.state == BitStringValueStateV1::sql_null))
    return Failure<BitStringComparisonResultV1>(
        "DATATYPE.NULL_STATE.INVALID", "present_comparator_received_null");

  auto result = Success<BitStringComparisonResultV1>();
  if (left.state == BitStringValueStateV1::sql_null ||
      right.state == BitStringValueStateV1::sql_null) {
    if (mode == BitStringComparisonModeV1::scalar_3vl) {
      result.is_null = true;
      return result;
    }
    if (mode == BitStringComparisonModeV1::ordered) {
      result.comparison = left.state == right.state
          ? 0 : (left.state == BitStringValueStateV1::sql_null ? -1 : 1);
      return result;
    }
    result.boolean_value = left.state == right.state;
    result.comparison = result.boolean_value
        ? 0 : (left.state == BitStringValueStateV1::sql_null ? -1 : 1);
    return result;
  }

  const auto left_valid = ValidateBitStringValueViewV1(left, true);
  if (!left_valid.ok())
    return Failure<BitStringComparisonResultV1>(
        left_valid.diagnostic.diagnostic_code,
        left_valid.diagnostic.detail, left_valid.status);
  const auto right_valid = ValidateBitStringValueViewV1(right, true);
  if (!right_valid.ok())
    return Failure<BitStringComparisonResultV1>(
        right_valid.diagnostic.diagnostic_code,
        right_valid.diagnostic.detail, right_valid.status);
  const u64 common = std::min(left.logical_bit_count, right.logical_bit_count);
  int comparison = 0;
  for (u64 index = 0; index < common; ++index) {
    if (ReadBit(left, index) != ReadBit(right, index)) {
      comparison = ReadBit(left, index) ? 1 : -1;
      break;
    }
  }
  if (comparison == 0 && left.logical_bit_count != right.logical_bit_count)
    comparison = left.logical_bit_count < right.logical_bit_count ? -1 : 1;
  result.comparison = comparison;
  result.boolean_value = comparison == 0;
  return result;
}

BitStringBytesResultV1 HashBitStringValueV1(const BitStringValueViewV1& value) noexcept {
  const auto checked=ValidateBitStringValueViewV1(value,true);if(!checked.ok())return Failure<BitStringBytesResultV1>(checked.diagnostic.diagnostic_code,"hash_value_invalid",checked.status);
  std::array<byte,80> header{};byte* p=header.data();
  PutUuid(p,value.profile->receipt.catalog_snapshot_uuid);PutU64(p,value.profile->receipt.catalog_generation);PutU64(p,value.profile->receipt.registry_generation);
  std::memcpy(p,value.profile->comparison_cohort_fingerprint.data(),32);p+=32;*p++=value.state==BitStringValueStateV1::sql_null?1:0;p+=7;PutU64(p,value.state==BitStringValueStateV1::sql_null?0:value.logical_bit_count);
  const std::string_view domain="ScratchBird.BaseBitString.Hash.V1";
  const hash::HashDigestSegment parts[]={{reinterpret_cast<const byte*>(domain.data()),domain.size()},{header.data(),header.size()},{value.state==BitStringValueStateV1::present&& !value.packed_msb0.empty()?value.packed_msb0.data():nullptr,value.state==BitStringValueStateV1::present?value.packed_msb0.size():0}};
  const auto digest=hash::ComputeSha256DigestPartsNative(parts,3);if(!digest.ok())return Failure<BitStringBytesResultV1>("RESOURCE.BUDGET_EXCEEDED","sha256_provider_failed",ResourceStatus());
  auto result=Success<BitStringBytesResultV1>();try{result.bytes.assign(digest.digest.begin(),digest.digest.end());}catch(const std::bad_alloc&){return Failure<BitStringBytesResultV1>("RESOURCE.BUDGET_EXCEEDED","hash_result_allocation_failed",ResourceStatus());}return result;
}

BitStringBytesResultV1 MakeBitStringSortKeyV1(const BitStringValueViewV1& value,BitStringSortDirectionV1 direction,BitStringNullModeV1 null_mode,const BitStringExecutionControlV1& control) noexcept {
  const auto checked=ValidateBitStringValueViewV1(value,true);if(!checked.ok())return Failure<BitStringBytesResultV1>(checked.diagnostic.diagnostic_code,"sort_value_invalid",checked.status);
  if((direction!=BitStringSortDirectionV1::ascending&&direction!=BitStringSortDirectionV1::descending)||(null_mode!=BitStringNullModeV1::nulls_first&&null_mode!=BitStringNullModeV1::nulls_last))return Failure<BitStringBytesResultV1>("CTB.BIT.ORDERING_REFUSED","sort_mode_invalid");
  const u64 required=kBitStringSortKeyHeaderBytesV1+(value.state==BitStringValueStateV1::present?static_cast<u64>(value.logical_bit_count)+1:0);
  if(required>control.maximum_allocation_bytes)return Failure<BitStringBytesResultV1>("RESOURCE.BUDGET_EXCEEDED","sort_key_grant_too_small",ResourceStatus());
  if(Cancelled(control))return Failure<BitStringBytesResultV1>("PROCESS.CANCELLED","before_allocation");
  auto result=Success<BitStringBytesResultV1>();try{result.bytes.assign(static_cast<std::size_t>(required),0);}catch(const std::bad_alloc&){return Failure<BitStringBytesResultV1>("RESOURCE.BUDGET_EXCEEDED","sort_key_allocation_failed",ResourceStatus());}
  std::memcpy(result.bytes.data(),"SBBITK01",8);std::memcpy(result.bytes.data()+8,value.profile->receipt.catalog_snapshot_uuid.bytes.data(),16);StoreLittle64(result.bytes.data()+24,value.profile->receipt.catalog_generation);StoreLittle64(result.bytes.data()+32,value.profile->receipt.registry_generation);std::memcpy(result.bytes.data()+40,value.profile->comparison_cohort_fingerprint.data(),32);std::memcpy(result.bytes.data()+72,kOrderingPolicy.uuid.bytes.data(),16);StoreLittle64(result.bytes.data()+88,kOrderingPolicy.generation);result.bytes[96]=direction==BitStringSortDirectionV1::descending;result.bytes[97]=null_mode==BitStringNullModeV1::nulls_last;result.bytes[98]=value.state==BitStringValueStateV1::present?1:(null_mode==BitStringNullModeV1::nulls_first?0:2);
  if(value.state==BitStringValueStateV1::present){for(u64 i=0;i<value.logical_bit_count;++i){const bool b=ReadBit(value,i);result.bytes[kBitStringSortKeyHeaderBytesV1+i]=direction==BitStringSortDirectionV1::ascending?(b?2:1):(b?0xfd:0xfe);if(CancellationCheckpoint(control,i + 1u))return Failure<BitStringBytesResultV1>("PROCESS.CANCELLED","sort_key_checkpoint");}result.bytes.back()=direction==BitStringSortDirectionV1::ascending?0:0xff;}return result;
}

BitStringSortKeyViewResultV1 DecodeBitStringSortKeyNoAllocV1(
    const BitStringDescriptorProfileV1& profile,
    std::span<const byte> encoded) noexcept {
  if (!ProfileValidNoAlloc(profile))
    return Failure<BitStringSortKeyViewResultV1>(
        "CTB.BIT.DESCRIPTOR_INVALID", "sort_key_profile_invalid");
  if (encoded.size() < kBitStringSortKeyHeaderBytesV1 ||
      encoded.size() > kBitStringMaximumSortKeyBytesV1)
    return Failure<BitStringSortKeyViewResultV1>(
        "CTB.BIT.INDEX_KEY_REFUSED", "sort_key_extent_invalid");
  if (std::memcmp(encoded.data(), "SBBITK01", 8) != 0 ||
      std::memcmp(encoded.data() + 8,
                  profile.receipt.catalog_snapshot_uuid.bytes.data(), 16) != 0 ||
      LoadLittle64(encoded.data() + 24) != profile.receipt.catalog_generation ||
      LoadLittle64(encoded.data() + 32) != profile.receipt.registry_generation ||
      std::memcmp(encoded.data() + 40,
                  profile.comparison_cohort_fingerprint.data(), 32) != 0 ||
      std::memcmp(encoded.data() + 72, kOrderingPolicy.uuid.bytes.data(), 16) != 0 ||
      LoadLittle64(encoded.data() + 88) != kOrderingPolicy.generation)
    return Failure<BitStringSortKeyViewResultV1>(
        "CTB.BIT.ORDERING_REFUSED", "sort_key_authority_mismatch");
  if (encoded[96] > 1 || encoded[97] > 1 ||
      encoded[99] != 0 || encoded[100] != 0 || encoded[101] != 0 ||
      encoded[102] != 0 || encoded[103] != 0)
    return Failure<BitStringSortKeyViewResultV1>(
        "CTB.BIT.INDEX_KEY_REFUSED", "sort_key_mode_or_reserved_invalid");
  const auto direction = static_cast<BitStringSortDirectionV1>(encoded[96]);
  const auto null_mode = static_cast<BitStringNullModeV1>(encoded[97]);
  auto result = Success<BitStringSortKeyViewResultV1>();
  result.value.profile = &profile;
  result.value.direction = direction;
  result.value.null_mode = null_mode;
  if (encoded.size() == kBitStringSortKeyHeaderBytesV1) {
    const byte expected_rank = null_mode == BitStringNullModeV1::nulls_first
        ? 0 : 2;
    if (encoded[98] != expected_rank)
      return Failure<BitStringSortKeyViewResultV1>(
          "CTB.BIT.INDEX_KEY_REFUSED", "sort_key_null_rank_invalid");
    result.value.state = BitStringValueStateV1::sql_null;
    return result;
  }
  if (encoded[98] != 1)
    return Failure<BitStringSortKeyViewResultV1>(
        "CTB.BIT.INDEX_KEY_REFUSED", "sort_key_present_rank_invalid");
  const byte zero = direction == BitStringSortDirectionV1::ascending
      ? 0x01 : 0xfe;
  const byte one = direction == BitStringSortDirectionV1::ascending
      ? 0x02 : 0xfd;
  const byte terminator = direction == BitStringSortDirectionV1::ascending
      ? 0x00 : 0xff;
  if (encoded.back() != terminator)
    return Failure<BitStringSortKeyViewResultV1>(
        "CTB.BIT.INDEX_KEY_REFUSED", "sort_key_terminator_invalid");
  const auto suffix = encoded.subspan(kBitStringSortKeyHeaderBytesV1);
  for (std::size_t index = 0; index + 1 < suffix.size(); ++index)
    if (suffix[index] != zero && suffix[index] != one)
      return Failure<BitStringSortKeyViewResultV1>(
          "CTB.BIT.INDEX_KEY_REFUSED", "sort_key_suffix_symbol_invalid");
  const u64 count = suffix.size() - 1;
  if (count > kBitStringMaximumLogicalBitsV1 ||
      count > profile.length_bits ||
      (profile.fixed_length && count != profile.length_bits))
    return Failure<BitStringSortKeyViewResultV1>(
        "CTB.BIT.INDEX_KEY_REFUSED", "sort_key_logical_length_exceeded");
  result.value.state = BitStringValueStateV1::present;
  result.value.logical_bit_count = static_cast<u32>(count);
  result.value.encoded_suffix = suffix;
  return result;
}

BitStringRenderResultV1 RenderBitStringValueV1(const BitStringValueViewV1& value,bool export_literal,const BitStringExecutionControlV1& control) noexcept {
  const auto checked=ValidateBitStringValueViewV1(value,true);if(!checked.ok())return Failure<BitStringRenderResultV1>(checked.diagnostic.diagnostic_code,"render_value_invalid",checked.status);
  auto result=Success<BitStringRenderResultV1>();if(value.state==BitStringValueStateV1::sql_null){result.containing_null=true;return result;}
  const u64 required=value.logical_bit_count+(export_literal?3:2);if(required>control.maximum_allocation_bytes)return Failure<BitStringRenderResultV1>("RESOURCE.BUDGET_EXCEEDED","render_grant_too_small",ResourceStatus());if(Cancelled(control))return Failure<BitStringRenderResultV1>("PROCESS.CANCELLED","before_allocation");
  try{result.text.reserve(static_cast<std::size_t>(required));result.text+=export_literal?"B'":"0b";for(u64 i=0;i<value.logical_bit_count;++i){result.text.push_back(ReadBit(value,i)?'1':'0');if(CancellationCheckpoint(control,i + 1u))return Failure<BitStringRenderResultV1>("PROCESS.CANCELLED","render_checkpoint");}if(export_literal)result.text.push_back('\'');}catch(const std::bad_alloc&){return Failure<BitStringRenderResultV1>("RESOURCE.BUDGET_EXCEEDED","render_allocation_failed",ResourceStatus());}return result;
}

BitStringCastResultV1 CastBitStringValueV1(
    const BitStringCastRequestV1& request) noexcept {
  const bool from_bit = request.bit_source != nullptr;
  const bool from_scalar = request.scalar_source != nullptr;
  const bool to_bit = request.bit_target != nullptr;
  const bool to_scalar = request.scalar_target != CanonicalTypeId::unknown;
  if (from_bit == from_scalar || to_bit == to_scalar)
    return Failure<BitStringCastResultV1>("DATATYPE.CAST_FORBIDDEN",
                                          "cast_shape_invalid");
  const auto receipt = from_bit
      ? (request.bit_source->profile != nullptr
             ? request.bit_source->profile->receipt
             : BitStringAuthorityReceiptV1{})
      : (request.bit_target != nullptr ? request.bit_target->receipt
                                       : BitStringAuthorityReceiptV1{});
  if (request.context != DatatypeCastContext::implicit &&
      request.context != DatatypeCastContext::assignment &&
      request.context != DatatypeCastContext::explicit_cast)
    return Failure<BitStringCastResultV1>("DATATYPE.CAST_FORBIDDEN",
                                          "cast_context_invalid");
  if (from_bit) {
    const auto profile = ValidateProfilePhase(*request.bit_source);
    if (!profile.ok())
      return Failure<BitStringCastResultV1>(
          profile.diagnostic.diagnostic_code, "bit_source_profile_invalid",
          profile.status);
    const auto state = ValidateStatePhase(*request.bit_source, true);
    if (!state.ok())
      return Failure<BitStringCastResultV1>(
          state.diagnostic.diagnostic_code, "bit_source_state_invalid",
          state.status);
  }
  if (from_scalar &&
      !ExactScalarDescriptor(request.scalar_source->descriptor,
                             request.scalar_source->type_id, receipt))
    return Failure<BitStringCastResultV1>("CTB.BIT.DESCRIPTOR_INVALID",
                                          "scalar_source_identity_invalid");
  if (to_bit &&
      !ValidateBitStringDescriptorProfileV1(*request.bit_target).ok())
    return Failure<BitStringCastResultV1>("CTB.BIT.DESCRIPTOR_INVALID",
                                          "bit_target_invalid");
  if (to_scalar &&
      !ExactScalarDescriptor(request.scalar_target_descriptor,
                             request.scalar_target, receipt))
    return Failure<BitStringCastResultV1>("CTB.BIT.DESCRIPTOR_INVALID",
                                          "scalar_target_identity_invalid");

  if (from_bit && to_bit) {
    auto result = Success<BitStringCastResultV1>();
    result.produced_bit_string = true;
    const auto& source = *request.bit_source;
    const auto& target = *request.bit_target;
    if (SameProfile(*source.profile, target)) {
      result.category = DatatypeCastCategory::identity;
      auto value = MaterializeBitStringValueV1(
          source, request.target_null_allowed, request.control);
      if (!value.ok())
        return Failure<BitStringCastResultV1>(
            value.diagnostic.diagnostic_code, "identity_materialization_failed",
            value.status);
      result.bit_value = std::move(value.value);
      return result;
    }
    if (request.context != DatatypeCastContext::explicit_cast)
      return Failure<BitStringCastResultV1>(
          "DATATYPE.CAST_FORBIDDEN", "distinct_profile_requires_explicit");
    result.category = DatatypeCastCategory::lossless_explicit;
    if (source.state == BitStringValueStateV1::sql_null) {
      if (!request.target_null_allowed)
        return Failure<BitStringCastResultV1>("DATATYPE.NULL_NOT_ADMITTED",
                                              "target_nonnullable");
      BitStringValueViewV1 null_value{
          &target, BitStringValueStateV1::sql_null, 0, {},
          BitStringOwnershipV1::borrowed};
      auto value = AllocateLike(null_value, 0, target, request.control);
      if (!value.ok())
        return Failure<BitStringCastResultV1>(
            value.diagnostic.diagnostic_code, "null_cast_failed", value.status);
      result.bit_value = std::move(value.value);
      return result;
    }
    const auto source_valid =
        ValidateBitStringValueViewV1(source, true);
    if (!source_valid.ok())
      return Failure<BitStringCastResultV1>(
          source_valid.diagnostic.diagnostic_code,
          "profile_cast_source_invalid", source_valid.status);
    if (source.profile->fixed_length && target.fixed_length &&
        target.length_bits < source.logical_bit_count)
      return Failure<BitStringCastResultV1>("CTB.BIT.PADDING_REFUSED",
                                            "fixed_target_shorter_than_source");
    if (source.logical_bit_count > target.length_bits)
      return Failure<BitStringCastResultV1>("CTB.BIT.LENGTH_EXCEEDED",
                                            "target_profile_too_short");
    const u32 output_count = target.fixed_length
        ? target.length_bits : source.logical_bit_count;
    auto value = AllocateLike(source, output_count, target, request.control);
    if (!value.ok())
      return Failure<BitStringCastResultV1>(
          value.diagnostic.diagnostic_code, "profile_cast_allocation_failed",
          value.status);
    for (u64 index = 0; index < source.logical_bit_count; ++index) {
      WriteBit(&value.value.packed_msb0, index, ReadBit(source, index));
      if (CancellationCheckpoint(request.control, index + 1u))
        return Failure<BitStringCastResultV1>("PROCESS.CANCELLED",
                                              "profile_cast_checkpoint");
    }
    result.bit_value = std::move(value.value);
    return result;
  }

  if (request.context != DatatypeCastContext::explicit_cast)
    return Failure<BitStringCastResultV1>("DATATYPE.CAST_FORBIDDEN",
                                          "cross_type_requires_explicit");

  if (from_scalar && to_bit) {
    const auto& source = *request.scalar_source;
    const auto& target = *request.bit_target;
    if (source.type_id != CanonicalTypeId::character &&
        !IntegerType(source.type_id))
      return Failure<BitStringCastResultV1>("DATATYPE.CAST_FORBIDDEN",
                                            "source_pair_unregistered");
    if (!ExactScalarValue(source, receipt))
      return Failure<BitStringCastResultV1>(
          source.is_null ? "DATATYPE.NULL_STATE.INVALID"
                         : "DATATYPE.CAST_FORBIDDEN",
          "scalar_source_value_invalid");
    auto result = Success<BitStringCastResultV1>();
    result.category = DatatypeCastCategory::lossless_explicit;
    result.produced_bit_string = true;
    if (source.is_null) {
      if (!request.target_null_allowed)
        return Failure<BitStringCastResultV1>("DATATYPE.NULL_NOT_ADMITTED",
                                              "target_nonnullable");
      BitStringValueViewV1 null_value{
          &target, BitStringValueStateV1::sql_null, 0, {},
          BitStringOwnershipV1::borrowed};
      auto value = AllocateLike(null_value, 0, target, request.control);
      if (!value.ok())
        return Failure<BitStringCastResultV1>(
            value.diagnostic.diagnostic_code, "null_cast_failed", value.status);
      result.bit_value = std::move(value.value);
      return result;
    }

    std::array<byte, 128> logical{};
    u64 logical_count = 0;
    if (source.type_id == CanonicalTypeId::character) {
      const auto& text = source.encoded_value;
      const u64 character_limit = source.descriptor.length == 0
          ? 16'777'216u : source.descriptor.length;
      if (text.size() > character_limit)
        return Failure<BitStringCastResultV1>(
            "CTB.BIT.LENGTH_EXCEEDED",
            "character_source_declared_length_exceeded");
      if (text.size() < 2 || text[0] != '0' || text[1] != 'b')
        return Failure<BitStringCastResultV1>("CTB.BIT.CAST_TEXT_INVALID",
                                              "prefix_invalid");
      logical_count = text.size() - 2u;
      if (logical_count > target.length_bits)
        return Failure<BitStringCastResultV1>(
            "CTB.BIT.LENGTH_EXCEEDED", "character_exceeds_target");
      if (Cancelled(request.control))
        return Failure<BitStringCastResultV1>("PROCESS.CANCELLED",
                                              "before_character_cast_scan");
      for (u64 index = 0; index < logical_count; ++index) {
        const char symbol = text[static_cast<std::size_t>(index + 2u)];
        if (symbol != '0' && symbol != '1')
          return Failure<BitStringCastResultV1>("CTB.BIT.CAST_TEXT_INVALID",
                                                "non_bit_at_offset");
        if (CancellationCheckpoint(request.control, index + 1u))
          return Failure<BitStringCastResultV1>("PROCESS.CANCELLED",
                                                "character_cast_scan_checkpoint");
      }
    } else {
      const std::size_t width = IntegerBytes(source.type_id);
      if (SignedInteger(source.type_id) &&
          (static_cast<byte>(source.encoded_value[width - 1u]) & 0x80u) != 0)
        return Failure<BitStringCastResultV1>("SCALAR.OUT_OF_RANGE",
                                              "negative_integer_source");
      std::size_t highest = width * 8u;
      while (highest > 0) {
        --highest;
        if ((static_cast<byte>(source.encoded_value[highest / 8u]) &
             static_cast<byte>(1u << (highest % 8u))) != 0)
          break;
      }
      const bool zero = std::all_of(
          source.encoded_value.begin(), source.encoded_value.end(),
          [](char value) { return value == 0; });
      logical_count = zero ? 1u : highest + 1u;
      for (u64 index = 0; index < logical_count; ++index)
        logical[static_cast<std::size_t>(logical_count - 1u - index)] =
            (static_cast<byte>(source.encoded_value[index / 8u]) &
             static_cast<byte>(1u << (index % 8u))) != 0;
      if (logical_count > target.length_bits)
        return Failure<BitStringCastResultV1>(
            "SCALAR.OUT_OF_RANGE", "integer_exceeds_target");
    }

    const u32 output_count = target.fixed_length
        ? target.length_bits : static_cast<u32>(logical_count);
    BitStringValueViewV1 seed{
        &target, BitStringValueStateV1::present, output_count, {},
        BitStringOwnershipV1::borrowed};
    auto value = AllocateLike(seed, output_count, target, request.control);
    if (!value.ok())
      return Failure<BitStringCastResultV1>(
          value.diagnostic.diagnostic_code, "cast_allocation_failed",
          value.status);
    const u64 offset = target.fixed_length && IntegerType(source.type_id)
        ? output_count - logical_count : 0;
    for (u64 index = 0; index < logical_count; ++index) {
      const bool bit = source.type_id == CanonicalTypeId::character
          ? source.encoded_value[static_cast<std::size_t>(index + 2u)] == '1'
          : logical[static_cast<std::size_t>(index)];
      WriteBit(&value.value.packed_msb0, offset + index, bit);
      if (CancellationCheckpoint(request.control, index + 1u))
        return Failure<BitStringCastResultV1>(
            "PROCESS.CANCELLED", "incoming_cast_publication_checkpoint");
    }
    result.bit_value = std::move(value.value);
    return result;
  }

  const auto& source = *request.bit_source;
  if (request.scalar_target != CanonicalTypeId::character &&
      !IntegerType(request.scalar_target))
    return Failure<BitStringCastResultV1>("DATATYPE.CAST_FORBIDDEN",
                                          "target_pair_unregistered");
  auto result = Success<BitStringCastResultV1>();
  result.category = DatatypeCastCategory::lossless_explicit;
  result.scalar_value.type_id = request.scalar_target;
  result.scalar_value.descriptor = request.scalar_target_descriptor;
  if (source.state == BitStringValueStateV1::sql_null) {
    if (!request.target_null_allowed)
      return Failure<BitStringCastResultV1>("DATATYPE.NULL_NOT_ADMITTED",
                                            "target_nonnullable");
    result.scalar_value.is_null = true;
    return result;
  }
  const auto source_valid = ValidateBitStringValueViewV1(source, true);
  if (!source_valid.ok())
    return Failure<BitStringCastResultV1>(
        source_valid.diagnostic.diagnostic_code,
        "scalar_cast_source_invalid", source_valid.status);

  if (request.scalar_target == CanonicalTypeId::character) {
    const u64 required = static_cast<u64>(source.logical_bit_count) + 2u;
    const u64 character_limit = request.scalar_target_descriptor.length == 0
        ? 16'777'216u : request.scalar_target_descriptor.length;
    if (required > character_limit)
      return Failure<BitStringCastResultV1>(
          "CTB.BIT.LENGTH_EXCEEDED",
          "character_target_declared_length_exceeded");
    if (required > request.control.maximum_allocation_bytes)
      return Failure<BitStringCastResultV1>("RESOURCE.BUDGET_EXCEEDED",
                                            "character_cast_grant_too_small",
                                            ResourceStatus());
    if (Cancelled(request.control))
      return Failure<BitStringCastResultV1>("PROCESS.CANCELLED",
                                            "before_character_cast_allocation");
    try {
      result.scalar_value.encoded_value.reserve(
          static_cast<std::size_t>(required));
      result.scalar_value.encoded_value = "0b";
      for (u64 index = 0; index < source.logical_bit_count; ++index) {
        result.scalar_value.encoded_value.push_back(
            ReadBit(source, index) ? '1' : '0');
        if (CancellationCheckpoint(request.control, index + 1u))
          return Failure<BitStringCastResultV1>(
              "PROCESS.CANCELLED", "outgoing_character_cast_checkpoint");
      }
    } catch (const std::bad_alloc&) {
      return Failure<BitStringCastResultV1>("RESOURCE.BUDGET_EXCEEDED",
                                            "scalar_cast_allocation_failed",
                                            ResourceStatus());
    }
    return result;
  }

  if (source.logical_bit_count == 0)
    return Failure<BitStringCastResultV1>(
        "SCALAR.OUT_OF_RANGE", "empty_bit_string_has_no_integer_magnitude");
  if (Cancelled(request.control))
    return Failure<BitStringCastResultV1>("PROCESS.CANCELLED",
                                          "before_integer_cast_scan");
  u64 first = 0;
  while (first < source.logical_bit_count && !ReadBit(source, first)) {
    ++first;
    if (CancellationCheckpoint(request.control, first))
      return Failure<BitStringCastResultV1>(
          "PROCESS.CANCELLED", "integer_cast_scan_checkpoint");
  }
  const u64 significant = source.logical_bit_count - first;
  const std::size_t width = IntegerBytes(request.scalar_target);
  const u64 maximum_bits = width * 8u -
      (SignedInteger(request.scalar_target) ? 1u : 0u);
  if (significant > maximum_bits)
    return Failure<BitStringCastResultV1>(
        "SCALAR.OUT_OF_RANGE", "integer_target_range_exceeded");
  if (width > request.control.maximum_allocation_bytes)
    return Failure<BitStringCastResultV1>("RESOURCE.BUDGET_EXCEEDED",
                                          "integer_cast_grant_too_small",
                                          ResourceStatus());
  if (Cancelled(request.control))
    return Failure<BitStringCastResultV1>("PROCESS.CANCELLED",
                                          "before_integer_cast_allocation");
  try {
    result.scalar_value.encoded_value.assign(width, '\0');
  } catch (const std::bad_alloc&) {
    return Failure<BitStringCastResultV1>("RESOURCE.BUDGET_EXCEEDED",
                                          "scalar_cast_allocation_failed",
                                          ResourceStatus());
  }
  for (u64 index = first; index < source.logical_bit_count; ++index) {
    if (ReadBit(source, index)) {
      const u64 magnitude_index = source.logical_bit_count - 1u - index;
      auto& target = result.scalar_value.encoded_value[
          static_cast<std::size_t>(magnitude_index / 8u)];
      target = static_cast<char>(
          static_cast<byte>(target) |
          static_cast<byte>(1u << (magnitude_index % 8u)));
    }
  }
  return result;
}

BitStringViewResultV1 DecodeBitStringSbdvalComposedNoAllocV1(
    const BitStringDescriptorProfileV1& profile, bool null_allowed,
    std::span<const byte> encoded) noexcept {
  const auto frame = DecodeDatatypeBinaryStructuralValueViewNoAlloc(
      encoded.empty() ? nullptr : encoded.data(), encoded.size());
  if (!frame.ok())
    return Failure<BitStringViewResultV1>(
        frame.diagnostic.diagnostic_code,
        "sbdval_structure_invalid", frame.status);
  if (!ProfileValidNoAlloc(profile))
    return Failure<BitStringViewResultV1>("CTB.BIT.DESCRIPTOR_INVALID",
                                          "sbdval_profile_invalid");
  if (frame.value.type_id != CanonicalTypeId::bit_string ||
      frame.value.payload_is_toast_reference)
    return Failure<BitStringViewResultV1>(
        "CTB.BIT.SERIALIZATION_PROFILE_MISSING",
        "sbdval_not_direct_type302_value");
  const auto state = frame.value.is_null ? BitStringValueStateV1::sql_null
                                         : BitStringValueStateV1::present;
  return DecodeCanonicalBitStringComponentNoAllocV1(
      profile, state, null_allowed,
      std::span<const byte>(frame.value.payload_data,
                            frame.value.payload_bytes));
}

BitStringBytesResultV1 EncodeBitStringSbdvalComposedV1(
    const BitStringValueViewV1& value, bool null_allowed,
    const BitStringExecutionControlV1& control) noexcept {
  const auto checked = ValidateBitStringValueViewV1(value, null_allowed);
  if (!checked.ok())
    return Failure<BitStringBytesResultV1>(checked.diagnostic.diagnostic_code,
                                           "sbdval_value_invalid",
                                           checked.status);
  const u64 component_bytes = value.state == BitStringValueStateV1::present
      ? 4u + value.packed_msb0.size() : 0u;
  const u64 total = 32u + component_bytes;
  if (total > control.maximum_allocation_bytes)
    return Failure<BitStringBytesResultV1>("RESOURCE.BUDGET_EXCEEDED",
                                           "sbdval_grant_too_small",
                                           ResourceStatus());
  if (Cancelled(control))
    return Failure<BitStringBytesResultV1>("PROCESS.CANCELLED",
                                           "before_allocation");
  BitStringBytesResultV1 component;
  if (value.state == BitStringValueStateV1::present) {
    auto component_control = control;
    component_control.maximum_allocation_bytes = component_bytes;
    component = EncodeCanonicalBitStringComponentV1(value, component_control);
    if (!component.ok()) return component;
  }
  auto result = Success<BitStringBytesResultV1>();
  try { result.bytes.resize(static_cast<std::size_t>(total)); }
  catch (const std::bad_alloc&) {
    return Failure<BitStringBytesResultV1>("RESOURCE.BUDGET_EXCEEDED",
                                           "sbdval_allocation_failed",
                                           ResourceStatus());
  }
  const DatatypeBinaryValueView structural{
      CanonicalTypeId::bit_string,
      value.state == BitStringValueStateV1::sql_null,
      false,
      component.bytes.empty() ? nullptr : component.bytes.data(),
      component.bytes.size()};
  const auto encoded = EncodeDatatypeBinaryStructuralValueIntoNoAlloc(
      structural, result.bytes.data(), result.bytes.size());
  if (!encoded.ok() || encoded.bytes_written != result.bytes.size())
    return Failure<BitStringBytesResultV1>(
        encoded.diagnostic.diagnostic_code.empty()
            ? "CTB.BIT.SERIALIZATION_PROFILE_MISSING"
            : std::string(encoded.diagnostic.diagnostic_code),
        "sbdval_structural_encode_failed", encoded.status);
  const auto recheck = DecodeBitStringSbdvalComposedNoAllocV1(
      *value.profile, null_allowed, result.bytes);
  if (!recheck.ok())
    return Failure<BitStringBytesResultV1>(recheck.diagnostic.diagnostic_code,
                                           "sbdval_recheck_failed",
                                           recheck.status);
  if (Cancelled(control))
    return Failure<BitStringBytesResultV1>("PROCESS.CANCELLED",
                                           "before_publication");
  return result;
}

BitStringViewResultV1 DecodeBitStringSbdpvComposedNoAllocV1(
    const BitStringDescriptorProfileV1& profile, bool null_allowed,
    std::span<const byte> encoded) noexcept {
  const auto frame = DecodeDatatypePhysicalStructuralValueViewNoAlloc(
      encoded.empty() ? nullptr : encoded.data(), encoded.size());
  if (!frame.ok())
    return Failure<BitStringViewResultV1>(
        frame.diagnostic.diagnostic_code,
        "sbdpv_structure_invalid", frame.status);
  if (!ProfileValidNoAlloc(profile))
    return Failure<BitStringViewResultV1>("CTB.BIT.DESCRIPTOR_INVALID",
                                          "sbdpv_profile_invalid");
  if (frame.value.type_id != CanonicalTypeId::bit_string ||
      (frame.value.state != DatatypePhysicalValueState::value &&
       frame.value.state != DatatypePhysicalValueState::sql_null))
    return Failure<BitStringViewResultV1>(
        "CTB.BIT.SERIALIZATION_PROFILE_MISSING",
        "sbdpv_not_direct_type302_value");
  return DecodeCanonicalBitStringComponentNoAllocV1(
      profile,
      frame.value.state == DatatypePhysicalValueState::sql_null
          ? BitStringValueStateV1::sql_null
          : BitStringValueStateV1::present,
      null_allowed,
      std::span<const byte>(frame.value.payload_data,
                            frame.value.payload_bytes));
}

BitStringBytesResultV1 EncodeBitStringSbdpvComposedV1(
    const BitStringValueViewV1& value, bool null_allowed,
    const BitStringExecutionControlV1& control) noexcept {
  const auto checked = ValidateBitStringValueViewV1(value, null_allowed);
  if (!checked.ok())
    return Failure<BitStringBytesResultV1>(checked.diagnostic.diagnostic_code,
                                           "sbdpv_value_invalid",
                                           checked.status);
  const u64 component_bytes = value.state == BitStringValueStateV1::present
      ? 4u + value.packed_msb0.size() : 0u;
  const u64 total = 24u + component_bytes;
  if (total > control.maximum_allocation_bytes)
    return Failure<BitStringBytesResultV1>("RESOURCE.BUDGET_EXCEEDED",
                                           "sbdpv_grant_too_small",
                                           ResourceStatus());
  if (Cancelled(control))
    return Failure<BitStringBytesResultV1>("PROCESS.CANCELLED",
                                           "before_allocation");
  BitStringBytesResultV1 component;
  if (value.state == BitStringValueStateV1::present) {
    auto component_control = control;
    component_control.maximum_allocation_bytes = component_bytes;
    component = EncodeCanonicalBitStringComponentV1(value, component_control);
    if (!component.ok()) return component;
  }
  auto result = Success<BitStringBytesResultV1>();
  try { result.bytes.resize(static_cast<std::size_t>(total)); }
  catch (const std::bad_alloc&) {
    return Failure<BitStringBytesResultV1>("RESOURCE.BUDGET_EXCEEDED",
                                           "sbdpv_allocation_failed",
                                           ResourceStatus());
  }
  const DatatypePhysicalValueView structural{
      CanonicalTypeId::bit_string,
      value.state == BitStringValueStateV1::sql_null
          ? DatatypePhysicalValueState::sql_null
          : DatatypePhysicalValueState::value,
      component.bytes.empty() ? nullptr : component.bytes.data(),
      component.bytes.size()};
  const auto encoded = EncodeDatatypePhysicalStructuralValueIntoNoAlloc(
      structural, result.bytes.data(), result.bytes.size());
  if (!encoded.ok() || encoded.bytes_written != result.bytes.size())
    return Failure<BitStringBytesResultV1>(
        encoded.diagnostic.diagnostic_code.empty()
            ? "CTB.BIT.SERIALIZATION_PROFILE_MISSING"
            : std::string(encoded.diagnostic.diagnostic_code),
        "sbdpv_structural_encode_failed", encoded.status);
  const auto recheck = DecodeBitStringSbdpvComposedNoAllocV1(
      *value.profile, null_allowed, result.bytes);
  if (!recheck.ok())
    return Failure<BitStringBytesResultV1>(recheck.diagnostic.diagnostic_code,
                                           "sbdpv_recheck_failed",
                                           recheck.status);
  if (Cancelled(control))
    return Failure<BitStringBytesResultV1>("PROCESS.CANCELLED",
                                           "before_publication");
  return result;
}

void SetBitStringMetricSinkV1(BitStringMetricSinkV1 sink,void* context) noexcept {g_metric_context.store(context,std::memory_order_release);g_metric_sink.store(sink,std::memory_order_release);}

BitStringMetricRecordDispositionV1 RecordBitStringMetricAfterCommitV1(
    const BitStringMetricRecordV1& record) noexcept {
  using D = BitStringMetricRecordDispositionV1;
  if (!record.represented_event_committed) return D::event_not_committed;
  if (record.cluster_series) return D::cluster_series_forbidden;
  if (IsNil(record.database_uuid) || IsNil(record.node_uuid) ||
      static_cast<unsigned>(record.metric) >
          static_cast<unsigned>(BitStringMetricV1::operation_input_bits))
    return D::identity_invalid;

  const auto no_state = record.value_state ==
      BitStringMetricValueStateLabelV1::not_applicable;
  const auto no_result = record.result ==
      BitStringMetricResultLabelV1::not_applicable;
  const auto no_allocation = record.allocation_class ==
      BitStringMetricAllocationClassV1::not_applicable;
  const auto no_operation = record.operation ==
      BitStringMetricOperationV1::not_applicable;
  const auto no_reason = record.reason ==
      BitStringMetricReasonV1::not_applicable;
  const auto no_index = record.index_family ==
      BitStringMetricIndexFamilyV1::not_applicable;
  const auto no_lane = record.lane_class ==
      BitStringMetricLaneClassV1::not_applicable;
  const auto no_boundary = record.boundary ==
      BitStringMetricBoundaryV1::not_applicable;
  const auto no_layer = record.layer ==
      BitStringMetricLayerV1::not_applicable;
  const bool common_none = no_result && no_state && no_allocation && no_operation &&
      no_reason && no_index && no_lane && no_boundary && no_layer;

  bool labels_valid = false;
  switch (record.metric) {
    case BitStringMetricV1::descriptor_admissions:
      labels_valid = !no_result && no_state && no_allocation && no_operation &&
          no_reason && no_index && no_lane && no_boundary && no_layer &&
          record.result == BitStringMetricResultLabelV1::admitted;
      break;
    case BitStringMetricV1::empty_values_admitted:
    case BitStringMetricV1::logical_bits:
    case BitStringMetricV1::logical_length:
      labels_valid = common_none;
      break;
    case BitStringMetricV1::values_admitted:
      labels_valid = no_result && !no_state && no_allocation && no_operation && no_reason &&
          no_index && no_lane && no_boundary && no_layer &&
          static_cast<unsigned>(record.value_state) <= 1;
      break;
    case BitStringMetricV1::owned_bytes:
      labels_valid = no_result && no_state && !no_allocation && no_operation && no_reason &&
          no_index && no_lane && no_boundary && no_layer &&
          static_cast<unsigned>(record.allocation_class) <= 4;
      break;
    case BitStringMetricV1::descriptor_refusals:
    case BitStringMetricV1::padding_refusals:
    case BitStringMetricV1::canonical_encoding_refusals:
    case BitStringMetricV1::statistics_stale:
    case BitStringMetricV1::merge_manual_review:
      labels_valid = no_result && no_state && no_allocation && no_operation && !no_reason &&
          no_index && no_lane && no_boundary && no_layer &&
          static_cast<unsigned>(record.reason) <= 16;
      break;
    case BitStringMetricV1::length_refusals:
    case BitStringMetricV1::operation_attempts:
    case BitStringMetricV1::operation_success:
    case BitStringMetricV1::operation_input_bits:
      labels_valid = no_result && no_state && no_allocation && !no_operation && no_reason &&
          no_index && no_lane && no_boundary && no_layer &&
          static_cast<unsigned>(record.operation) <= 21;
      break;
    case BitStringMetricV1::operation_refusals:
      labels_valid = no_result && no_state && no_allocation && !no_operation && !no_reason &&
          no_index && no_lane && no_boundary && no_layer &&
          static_cast<unsigned>(record.operation) <= 21 &&
          static_cast<unsigned>(record.reason) <= 16;
      break;
    case BitStringMetricV1::index_admission_refusals:
      labels_valid = no_result && no_state && no_allocation && no_operation && !no_reason &&
          !no_index && no_lane && no_boundary && no_layer &&
          static_cast<unsigned>(record.reason) <= 16 &&
          static_cast<unsigned>(record.index_family) <= 15;
      break;
    case BitStringMetricV1::compatibility_mapping_misses:
    case BitStringMetricV1::transport_refusals:
      labels_valid = no_result && no_state && no_allocation && no_operation && !no_reason &&
          no_index && !no_lane && no_boundary && no_layer &&
          static_cast<unsigned>(record.reason) <= 16 &&
          static_cast<unsigned>(record.lane_class) <= 3;
      break;
    case BitStringMetricV1::serialization_refusals:
      labels_valid = no_result && no_state && no_allocation && no_operation && !no_reason &&
          no_index && no_lane && !no_boundary && no_layer &&
          static_cast<unsigned>(record.reason) <= 16 &&
          static_cast<unsigned>(record.boundary) <= 9;
      break;
    case BitStringMetricV1::protection_refusals:
      labels_valid = no_result && no_state && no_allocation && no_operation && !no_reason &&
          no_index && no_lane && no_boundary && !no_layer &&
          static_cast<unsigned>(record.reason) <= 16 &&
          static_cast<unsigned>(record.layer) <= 5;
      break;
  }
  if (!labels_valid) return D::label_combination_invalid;

  const bool histogram = record.metric == BitStringMetricV1::logical_length ||
      record.metric == BitStringMetricV1::operation_input_bits;
  const bool gauge = record.metric == BitStringMetricV1::owned_bytes;
  const auto expected_update = histogram
      ? BitStringMetricUpdateV1::histogram_observe
      : gauge ? BitStringMetricUpdateV1::gauge_set
              : BitStringMetricUpdateV1::counter_add;
  if (record.update != expected_update ||
      (histogram && record.value > kBitStringMaximumLogicalBitsV1) ||
      (!histogram && !gauge && record.value == 0))
    return D::update_invalid;
  if (g_emitting_metric) return D::recursive_emission_suppressed;
  auto sink = g_metric_sink.load(std::memory_order_acquire);
  if (sink == nullptr) return D::sink_unavailable;
  g_emitting_metric = true;
  const bool accepted = sink(record,
      g_metric_context.load(std::memory_order_acquire));
  g_emitting_metric = false;
  return accepted ? D::recorded : D::sink_failure_isolated;
}

}  // namespace scratchbird::core::datatypes

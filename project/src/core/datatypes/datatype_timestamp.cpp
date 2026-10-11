// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0

#include "datatype_timestamp.hpp"

#include "datatype_binary_view.hpp"
#include "datatype_physical_encoding.hpp"
#include <algorithm>
#include <array>
#include <charconv>
#include <cstdio>
#include <cstring>
#include <limits>
#include <new>
#include <stdexcept>

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
using TimestampWideSigned = __int128_t;

constexpr platform::Uuid U(std::array<byte, 16> bytes) noexcept {
  return platform::Uuid{bytes};
}

inline constexpr platform::Uuid kSnapshot = U(
    {0x01,0x9d,0x00,0x00,0x00,0x00,0x70,0x00,0x80,0x00,0x00,0x00,0x00,0x00,0xd7,0x10});
inline constexpr platform::Uuid kDescriptor = U(
    {0x92,0x01,0x00,0x00,0x74,0x69,0x7d,0x65,0xb3,0x74,0x61,0x6d,0x70,0x00,0x00,0x00});
inline constexpr platform::Uuid kType = U(
    {0x01,0x9d,0x00,0x00,0x00,0x00,0x70,0x00,0x80,0x00,0x00,0x00,0x00,0x00,0xd8,0x20});
inline constexpr platform::Uuid kCodec = U(
    {0x01,0xa1,0x04,0xf5,0xfb,0x16,0x75,0x00,0x93,0xe8,0x4a,0x99,0xc8,0x6d,0x11,0x30});

inline constexpr DatatypePolicyIdentityV3 kOrderingPolicy{U(
    {0x01,0xa1,0x04,0xec,0x8e,0x3c,0x7d,0xff,0x8e,0x89,0x86,0x8e,0xcc,0x2a,0x8a,0x0c}),1};
inline constexpr DatatypePolicyIdentityV3 kHashPolicy{U(
    {0x01,0xa1,0x04,0xec,0x8e,0x3c,0x7d,0xff,0x8e,0x89,0x86,0x8e,0xcc,0x2a,0x8a,0x0d}),1};
inline constexpr DatatypePolicyIdentityV3 kRenderPolicy{U(
    {0x01,0xa1,0x04,0xec,0x8e,0x3c,0x7d,0xff,0x8e,0x89,0x86,0x8e,0xcc,0x2a,0x8a,0x0f}),1};
inline constexpr DatatypePolicyIdentityV3 kCastPolicy{U(
    {0x01,0xa1,0x04,0xec,0x8e,0x3c,0x7d,0xff,0x8e,0x89,0x86,0x8e,0xcc,0x2a,0x8a,0x10}),1};
inline constexpr DatatypePolicyIdentityV3 kCalendarPolicy{U(
    {0x01,0xa1,0x04,0xec,0x8e,0x3c,0x7d,0xff,0x8e,0x89,0x86,0x8e,0xcc,0x2a,0x8a,0x11}),1};
inline constexpr DatatypePolicyIdentityV3 kStorageEpochPolicy{U(
    {0x01,0xa1,0x04,0xec,0x8e,0x3c,0x7d,0xff,0x8e,0x89,0x86,0x8e,0xcc,0x2a,0x8a,0x12}),1};
inline constexpr DatatypePolicyIdentityV3 kTimezoneNonePolicy{U(
    {0x01,0xa1,0x04,0xec,0x8e,0x3c,0x7d,0xff,0x8e,0x89,0x86,0x8e,0xcc,0x2a,0x8a,0x13}),1};
inline constexpr DatatypePolicyIdentityV3 kLeapSecondPolicy{U(
    {0x01,0xa1,0x04,0xec,0x8e,0x3c,0x7d,0xff,0x8e,0x89,0x86,0x8e,0xcc,0x2a,0x8a,0x14}),1};
inline constexpr DatatypePolicyIdentityV3 kIndexPolicy{U(
    {0x01,0xa1,0x04,0xec,0x8e,0x3c,0x7d,0xff,0x8e,0x89,0x86,0x8e,0xcc,0x2a,0x8a,0x15}),1};
inline constexpr DatatypePolicyIdentityV3 kStatisticsPolicy{U(
    {0x01,0xa1,0x04,0xec,0x8e,0x3c,0x7d,0xff,0x8e,0x89,0x86,0x8e,0xcc,0x2a,0x8a,0x16}),1};
inline constexpr DatatypePolicyIdentityV3 kBackupPolicy{U(
    {0x01,0xa1,0x04,0xec,0x8e,0x3c,0x7d,0xff,0x8e,0x89,0x86,0x8e,0xcc,0x2a,0x8a,0x18}),1};
inline constexpr DatatypePolicyIdentityV3 kProtectionPolicy{U(
    {0x01,0xa1,0x04,0xec,0x8e,0x3c,0x7d,0xff,0x8e,0x89,0x86,0x8e,0xcc,0x2a,0x8a,0x19}),1};
inline constexpr DatatypePolicyIdentityV3 kComponentPolicy{U(
    {0x01,0xa1,0x04,0xec,0x8e,0x3c,0x7d,0xff,0x8e,0x89,0x86,0x8e,0xcc,0x2a,0x8a,0x1a}),1};
inline constexpr DatatypePolicyIdentityV3 kDiagnosticPolicy{U(
    {0x01,0xa1,0x04,0xec,0x8e,0x3c,0x7d,0xff,0x8e,0x89,0x86,0x8e,0xcc,0x2a,0x8a,0x1b}),1};
inline constexpr DatatypePolicyIdentityV3 kMetricPolicy{U(
    {0x01,0xa1,0x04,0xec,0x8e,0x3c,0x7d,0xff,0x8e,0x89,0x86,0x8e,0xcc,0x2a,0x8a,0x1c}),1};

inline constexpr std::array<byte, 32> kProfileFingerprint{{
    0x4a,0xa1,0x38,0x79,0xac,0x9d,0x34,0x5c,0xeb,0x2b,0x17,0xe1,0x06,0x8f,0x0a,0x00,
    0x49,0xff,0x45,0x50,0x8d,0x76,0xf4,0xf5,0xaf,0x5c,0x6d,0xdc,0x1d,0x07,0xa3,0xca}};
inline constexpr std::array<byte, 32> kComparisonFingerprint{{
    0x35,0xce,0x26,0xaf,0xa0,0x8d,0x48,0xaf,0x46,0xc7,0x9e,0x08,0xcd,0x05,0x6a,0x45,
    0x3b,0x7d,0xeb,0xa8,0x0d,0x35,0x69,0xe7,0x8f,0xfd,0xba,0x5b,0x1c,0x93,0xfe,0xe7}};
inline constexpr std::array<byte, 32> kDateProfileFingerprint{{
    0x57,0x75,0x95,0x92,0x31,0x69,0xd5,0x77,0xe4,0xce,0xe1,0x8e,0x11,0x4d,0x9a,0x09,
    0xe3,0x16,0x09,0x82,0x3c,0x53,0xcb,0xd1,0xd6,0x08,0xfa,0x50,0x64,0x1a,0xae,0x55}};
inline constexpr std::array<byte, 32> kTimeProfileFingerprint{{
    0x98,0x6d,0xff,0x7c,0x60,0xb8,0xf3,0xeb,0x6f,0x70,0x1c,0xe8,0xc7,0xb7,0xf0,0x99,
    0x7f,0x94,0xa1,0x8d,0x3a,0x0a,0x9b,0xcb,0x73,0x6c,0x11,0xf3,0x5e,0x47,0x6f,0x48}};
inline constexpr std::array<byte, 32> kDateComparisonFingerprint{{
    0xad,0x45,0x6d,0xaf,0xc3,0x67,0x1a,0x8f,0x31,0x22,0x35,0x2c,0x4d,0xa9,0x85,0x86,
    0xf6,0xb7,0x68,0x4c,0xe2,0x64,0x41,0xff,0xc5,0xc6,0xcc,0x10,0x80,0x9a,0x42,0x37}};
inline constexpr std::array<byte, 32> kTimeComparisonFingerprint{{
    0x75,0xde,0xab,0xa7,0x96,0xda,0x7e,0x20,0x08,0x60,0x02,0x37,0xa5,0xeb,0xfb,0x0d,
    0xd8,0xa7,0x6f,0x2b,0x62,0xea,0x66,0x4b,0x66,0x87,0x6b,0xa4,0x7f,0x05,0xb3,0xd6}};

// Independently sealed D711 materials; frozen D710 constants remain above.
inline constexpr std::array<byte, 32> kD711ProfileFingerprint{{
    0xb3,0xf2,0xa0,0xe9,0xd7,0xc5,0x64,0xc9,0x37,0x54,0xf4,0x47,0x1b,0x37,0x10,0xb7,
    0xe9,0x21,0x52,0x16,0x98,0x71,0x66,0xe5,0xc3,0xf1,0x6f,0xbb,0xe8,0xb2,0x26,0xd8}};
const std::array<byte, 32>& ProfileFingerprint(
    const TimestampAuthorityReceiptV3& receipt) noexcept {
  return receipt.catalog_generation == 10 ? kProfileFingerprint : kD711ProfileFingerprint;
}
inline constexpr std::array<byte, 32> kD711ComparisonFingerprint{{
    0x87,0x90,0x04,0x81,0x4d,0x3f,0xc6,0x58,0x3c,0xc0,0xde,0x32,0x63,0xcd,0x72,0xda,
    0x1d,0x97,0xe1,0x5b,0xb0,0x6d,0x94,0x26,0x09,0x05,0x6f,0x58,0xca,0x9c,0x2d,0xa4}};
const std::array<byte, 32>& ComparisonFingerprint(
    const TimestampAuthorityReceiptV3& receipt) noexcept {
  return receipt.catalog_generation == 10 ? kComparisonFingerprint : kD711ComparisonFingerprint;
}
inline constexpr std::array<byte, 32> kD711DateProfileFingerprint{{
    0x1f,0x28,0x86,0xdb,0x58,0x11,0x46,0xfc,0x27,0x40,0x8d,0x3f,0xc9,0xc2,0x91,0x4b,
    0x34,0x8a,0x6f,0xd9,0x57,0x89,0x19,0x25,0xa0,0xef,0x24,0x5b,0x85,0x8a,0x8a,0x77}};
const std::array<byte, 32>& DateProfileFingerprint(
    const TimestampAuthorityReceiptV3& receipt) noexcept {
  return receipt.catalog_generation == 10 ? kDateProfileFingerprint : kD711DateProfileFingerprint;
}
inline constexpr std::array<byte, 32> kD711TimeProfileFingerprint{{
    0xf1,0x7f,0x78,0x61,0xee,0x31,0x36,0x45,0x30,0xc8,0x80,0xdc,0x01,0xb1,0x83,0x6a,
    0x86,0x29,0xd4,0xc5,0xc6,0xea,0x5d,0x28,0x7b,0x4f,0x9b,0xef,0x18,0x82,0x52,0x8d}};
const std::array<byte, 32>& TimeProfileFingerprint(
    const TimestampAuthorityReceiptV3& receipt) noexcept {
  return receipt.catalog_generation == 10 ? kTimeProfileFingerprint : kD711TimeProfileFingerprint;
}
inline constexpr std::array<byte, 32> kD711DateComparisonFingerprint{{
    0x0d,0x74,0x77,0x69,0xd2,0x32,0x41,0x70,0xe0,0x00,0xcb,0x19,0x73,0xf8,0xb1,0xf0,
    0x28,0x29,0x90,0x34,0x3c,0xd3,0x08,0xda,0x83,0x80,0x4a,0x64,0x1b,0x9c,0x57,0x96}};
const std::array<byte, 32>& DateComparisonFingerprint(
    const TimestampAuthorityReceiptV3& receipt) noexcept {
  return receipt.catalog_generation == 10 ? kDateComparisonFingerprint : kD711DateComparisonFingerprint;
}
inline constexpr std::array<byte, 32> kD711TimeComparisonFingerprint{{
    0x16,0x73,0x47,0x5a,0x46,0x39,0x59,0x37,0xc1,0xee,0xf4,0xe9,0x0b,0xdc,0x92,0xe7,
    0x2e,0xec,0x21,0xa4,0x74,0x04,0x39,0x4b,0xfe,0x27,0x22,0x24,0xcf,0x98,0x3f,0x44}};
const std::array<byte, 32>& TimeComparisonFingerprint(
    const TimestampAuthorityReceiptV3& receipt) noexcept {
  return receipt.catalog_generation == 10 ? kTimeComparisonFingerprint : kD711TimeComparisonFingerprint;
}

Status OkStatus() noexcept { return {StatusCode::ok, Severity::info, Subsystem::datatypes}; }
Status ErrorStatus() noexcept { return {StatusCode::platform_required_feature_missing, Severity::error, Subsystem::datatypes}; }
Status ResourceStatus() noexcept { return {StatusCode::memory_allocation_failed, Severity::error, Subsystem::datatypes}; }

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

bool Same(const DatatypePolicyIdentityV3& a,
          const DatatypePolicyIdentityV3& b) noexcept {
  return a.uuid == b.uuid && a.generation == b.generation;
}

bool EqualLegacyIdentityIgnoringName(const DatatypeTypeCodecIdentityRowV1& a,
                                     const DatatypeTypeCodecIdentityRowV1& b) noexcept {
  // canonical_name and codec_id are presentation/provenance labels. Runtime
  // authority is the UUID/generation/version and semantic/physical tuple.
  const auto& left=a;
  const auto& right=b;
  return a.catalog_snapshot_uuid == b.catalog_snapshot_uuid &&
      left.catalog_generation == right.catalog_generation &&
      left.registry_generation == right.registry_generation &&
      left.descriptor_uuid == right.descriptor_uuid &&
      left.descriptor_generation == right.descriptor_generation &&
      left.type_uuid == right.type_uuid && left.type_generation == right.type_generation &&
      left.codec_uuid == right.codec_uuid &&
      left.codec_version == right.codec_version &&
      left.codec_generation == right.codec_generation &&
      left.canonical_value_bytes == right.canonical_value_bytes &&
      left.null_supported == right.null_supported &&
      left.datatype_identity_code == right.datatype_identity_code &&
      left.null_encoding_code == right.null_encoding_code &&
      left.byte_order_code == right.byte_order_code &&
      left.signed_code == right.signed_code &&
      left.representation_code == right.representation_code &&
      left.canonical_value_minimum_bytes == right.canonical_value_minimum_bytes &&
      left.canonical_value_maximum_bytes == right.canonical_value_maximum_bytes &&
      left.canonical_value_exact_bytes == right.canonical_value_exact_bytes &&
      left.canonical_binary_type_code == right.canonical_binary_type_code &&
      left.canonical_value_variable_width == right.canonical_value_variable_width &&
      left.canonical_value_exact_zero_is_width_marker == right.canonical_value_exact_zero_is_width_marker &&
      left.canonical_byte_order == right.canonical_byte_order &&
      left.canonical_representation == right.canonical_representation &&
      left.canonical_charset == right.canonical_charset &&
      left.shortest_form_utf8_required == right.shortest_form_utf8_required &&
      left.implicit_normalization_allowed == right.implicit_normalization_allowed &&
      left.descriptor_bound_collation_required == right.descriptor_bound_collation_required &&
      left.empty_value_distinct_from_sql_null == right.empty_value_distinct_from_sql_null &&
      left.sql_null_requires_zero_payload == right.sql_null_requires_zero_payload &&
      left.variable_width_storage_without_truncation == right.variable_width_storage_without_truncation &&
      left.invalid_encoding_diagnostic_id == right.invalid_encoding_diagnostic_id &&
      left.numeric_context_uuid == right.numeric_context_uuid &&
      left.numeric_context_generation == right.numeric_context_generation &&
      left.special_value_policy_uuid == right.special_value_policy_uuid &&
      left.special_value_policy_generation == right.special_value_policy_generation &&
      left.comparison_policy_uuid == right.comparison_policy_uuid &&
      left.comparison_policy_generation == right.comparison_policy_generation &&
      left.comparison_profile == right.comparison_profile &&
      left.allow_special_values == right.allow_special_values;
}

bool EqualIdentityIgnoringName(const DatatypeTypeCodecIdentityRowV3& a,
                               const DatatypeTypeCodecIdentityRowV3& b) noexcept {
  return EqualLegacyIdentityIgnoringName(a.legacy_fields, b.legacy_fields) &&
      Same(a.descriptor_policy, b.descriptor_policy) &&
      Same(a.canonicalization_policy, b.canonicalization_policy) &&
      Same(a.ordering_policy, b.ordering_policy) && Same(a.hash_policy, b.hash_policy) &&
      Same(a.operation_policy, b.operation_policy) && a.native_fields == b.native_fields;
}

TimestampAuthorityReceiptV3 ReceiptForIdentity(
    const DatatypeTypeCodecIdentityRowV3& identity) noexcept {
  const auto& row = identity.legacy_fields;
  return {row.catalog_snapshot_uuid, row.catalog_snapshot_uuid,
          row.catalog_generation, row.registry_generation};
}

bool ExactReceipt(const TimestampAuthorityReceiptV3& receipt) noexcept {
  return receipt.statement_receipt_uuid == receipt.catalog_snapshot_uuid &&
      ((receipt.catalog_snapshot_uuid == kSnapshot &&
        receipt.catalog_generation == 10 && receipt.registry_generation == 10) ||
       (receipt.catalog_snapshot_uuid == kDatatypeCohortV11 &&
        receipt.catalog_generation == 11 && receipt.registry_generation == 11));
}

const DatatypeTypeCodecIdentityRowV3* IdentityForReceipt(
    CanonicalTypeId type, const TimestampAuthorityReceiptV3& receipt) noexcept {
  if (!ExactReceipt(receipt)) return nullptr;
  const auto code = static_cast<u32>(type);
  const DatatypeTypeCodecIdentityRowV3* found = nullptr;
  for (const auto& row : CurrentDatatypeTypeCodecIdentityRowsV3()) {
    const auto& legacy = row.legacy_fields;
    if (legacy.catalog_snapshot_uuid != receipt.catalog_snapshot_uuid ||
        legacy.catalog_generation != receipt.catalog_generation ||
        legacy.registry_generation != receipt.registry_generation || legacy.canonical_binary_type_code != code)
      continue;
    if (found != nullptr) return nullptr;
    found = &row;
  }
  return found;
}

bool ExactTimestampIdentity(const DatatypeTypeCodecIdentityRowV3& identity) noexcept {
  const auto* current = IdentityForReceipt(CanonicalTypeId::timestamp, ReceiptForIdentity(identity));
  return current != nullptr && EqualIdentityIgnoringName(identity, *current) &&
      identity.legacy_fields.descriptor_uuid == kDescriptor &&
      identity.legacy_fields.type_uuid == kType &&
      identity.legacy_fields.codec_uuid == kCodec &&
      identity.legacy_fields.codec_version == 1 &&
      identity.legacy_fields.codec_generation == 1 &&
      identity.legacy_fields.canonical_value_exact_bytes == 16 &&
      identity.legacy_fields.sql_null_requires_zero_payload;
}

bool Cancelled(const TimestampExecutionControlV3& control) noexcept {
  return control.cancelled != nullptr && control.cancelled(control.cancellation_context);
}

void SecureClear(void* pointer, std::size_t bytes) noexcept {
  auto* output = static_cast<volatile unsigned char*>(pointer);
  while (bytes != 0) { *output++ = 0; --bytes; }
}

class ScopedClear final {
 public:
  ScopedClear(void* pointer, std::size_t bytes,TimestampScrubClassV3 scrub_class,
      void (*observer)(void*,TimestampScrubClassV3,const byte*,u64) noexcept=nullptr,
      void* observer_context=nullptr) noexcept
      : pointer_(pointer), bytes_(bytes), scrub_class_(scrub_class), observer_(observer), observer_context_(observer_context) {}
  ~ScopedClear() { SecureClear(pointer_, bytes_);if(observer_)observer_(observer_context_,scrub_class_,static_cast<const byte*>(pointer_),bytes_); }
 private:
  void* pointer_;
  std::size_t bytes_;
  TimestampScrubClassV3 scrub_class_;
  void (*observer_)(void*,TimestampScrubClassV3,const byte*,u64) noexcept;
  void* observer_context_;
};

template <typename T>
class ScopedVectorClear final {
 public:
  explicit ScopedVectorClear(std::vector<T>* value,TimestampScrubClassV3 scrub_class,
      void (*observer)(void*,TimestampScrubClassV3,const byte*,u64) noexcept=nullptr,
      void* observer_context=nullptr) noexcept
      : value_(value),scrub_class_(scrub_class),observer_(observer),observer_context_(observer_context) {}
  ~ScopedVectorClear() {
    if(active_&&value_!=nullptr&&!value_->empty()){
      const auto extent=value_->size()*sizeof(T);SecureClear(value_->data(),extent);
      if(observer_)observer_(observer_context_,scrub_class_,reinterpret_cast<const byte*>(value_->data()),extent);
    }
  }
  void Disarm() noexcept { active_=false; }
 private:
  std::vector<T>* value_;
  TimestampScrubClassV3 scrub_class_;
  void (*observer_)(void*,TimestampScrubClassV3,const byte*,u64) noexcept;
  void* observer_context_;
  bool active_=true;
};

bool RangesOverlap(const void* a, std::size_t a_size,
                   const void* b, std::size_t b_size) noexcept {
  if (a == nullptr || b == nullptr || a_size == 0 || b_size == 0) return false;
  const auto left = reinterpret_cast<std::uintptr_t>(a);
  const auto right = reinterpret_cast<std::uintptr_t>(b);
  return left <= right ? right - left < a_size : left - right < b_size;
}

bool OutputOverlapsProfile(const void* output, std::size_t bytes,
                           const TimestampValidatedProfileHandleV3& profile) noexcept {
  return RangesOverlap(output, bytes, &profile, sizeof(profile)) ||
      RangesOverlap(output, bytes, profile.profile_material.data(), profile.profile_material.size()) ||
      RangesOverlap(output, bytes, profile.comparison_material.data(), profile.comparison_material.size());
}

void PutUuid(byte* output, const platform::Uuid& uuid) noexcept {
  std::memcpy(output, uuid.bytes.data(), uuid.bytes.size());
}
void PutPolicy(byte* output, const DatatypePolicyIdentityV3& policy) noexcept {
  PutUuid(output, policy.uuid);
  StoreLittle64(output + 16, policy.generation);
}

bool Digest(std::span<const byte> material, std::array<byte, 32>* output,
            const TimestampExecutionControlV3* control=nullptr) noexcept {
  if(output==nullptr)return false;
  const auto observer=control==nullptr?nullptr:control->observe_scrubbed;
  void* observer_context=control==nullptr?nullptr:control->scrub_observer_context;
  std::array<u32,8> state{{0x6a09e667u,0xbb67ae85u,0x3c6ef372u,0xa54ff53au,
                           0x510e527fu,0x9b05688cu,0x1f83d9abu,0x5be0cd19u}};
  ScopedClear clear_state(state.data(),state.size()*sizeof(u32),TimestampScrubClassV3::sha_state,observer,observer_context);
  constexpr std::array<u32,64> constants{{
      0x428a2f98u,0x71374491u,0xb5c0fbcfu,0xe9b5dba5u,0x3956c25bu,0x59f111f1u,0x923f82a4u,0xab1c5ed5u,
      0xd807aa98u,0x12835b01u,0x243185beu,0x550c7dc3u,0x72be5d74u,0x80deb1feu,0x9bdc06a7u,0xc19bf174u,
      0xe49b69c1u,0xefbe4786u,0x0fc19dc6u,0x240ca1ccu,0x2de92c6fu,0x4a7484aau,0x5cb0a9dcu,0x76f988dau,
      0x983e5152u,0xa831c66du,0xb00327c8u,0xbf597fc7u,0xc6e00bf3u,0xd5a79147u,0x06ca6351u,0x14292967u,
      0x27b70a85u,0x2e1b2138u,0x4d2c6dfcu,0x53380d13u,0x650a7354u,0x766a0abbu,0x81c2c92eu,0x92722c85u,
      0xa2bfe8a1u,0xa81a664bu,0xc24b8b70u,0xc76c51a3u,0xd192e819u,0xd6990624u,0xf40e3585u,0x106aa070u,
      0x19a4c116u,0x1e376c08u,0x2748774cu,0x34b0bcb5u,0x391c0cb3u,0x4ed8aa4au,0x5b9cca4fu,0x682e6ff3u,
      0x748f82eeu,0x78a5636fu,0x84c87814u,0x8cc70208u,0x90befffau,0xa4506cebu,0xbef9a3f7u,0xc67178f2u}};
  const auto rotate=[](u32 value,unsigned bits) noexcept {
    return static_cast<u32>((value>>bits)|(value<<(32-bits)));
  };
  const auto transform=[&](const byte* block) noexcept {
    std::array<u32,64> words{};ScopedClear clear_words(words.data(),words.size()*sizeof(u32),TimestampScrubClassV3::sha_schedule,observer,observer_context);
    for(unsigned i=0;i<16;++i)words[i]=(static_cast<u32>(block[i*4])<<24)|
        (static_cast<u32>(block[i*4+1])<<16)|(static_cast<u32>(block[i*4+2])<<8)|block[i*4+3];
    for(unsigned i=16;i<64;++i){const u32 s0=rotate(words[i-15],7)^rotate(words[i-15],18)^(words[i-15]>>3);const u32 s1=rotate(words[i-2],17)^rotate(words[i-2],19)^(words[i-2]>>10);words[i]=words[i-16]+s0+words[i-7]+s1;}
    u32 a=state[0],b=state[1],c=state[2],d=state[3],e=state[4],f=state[5],g=state[6],h=state[7];
    for(unsigned i=0;i<64;++i){const u32 s1=rotate(e,6)^rotate(e,11)^rotate(e,25);const u32 choose=(e&f)^((~e)&g);const u32 first=h+s1+choose+constants[i]+words[i];const u32 s0=rotate(a,2)^rotate(a,13)^rotate(a,22);const u32 majority=(a&b)^(a&c)^(b&c);const u32 second=s0+majority;h=g;g=f;f=e;e=d+first;d=c;c=b;b=a;a=first+second;}
    state[0]+=a;state[1]+=b;state[2]+=c;state[3]+=d;state[4]+=e;state[5]+=f;state[6]+=g;state[7]+=h;
  };
  std::size_t offset=0;while(material.size()-offset>=64){transform(material.data()+offset);offset+=64;}
  std::array<byte,128> tail{};ScopedClear clear_tail(tail.data(),tail.size(),TimestampScrubClassV3::sha_tail,observer,observer_context);const std::size_t remainder=material.size()-offset;if(remainder!=0)std::memcpy(tail.data(),material.data()+offset,remainder);tail[remainder]=0x80;const std::size_t padded=remainder<56?64:128;const u64 bits=static_cast<u64>(material.size())*8;
  for(unsigned i=0;i<8;++i)tail[padded-1-i]=static_cast<byte>(bits>>(i*8));
  transform(tail.data());if(padded==128)transform(tail.data()+64);
  for(unsigned i=0;i<8;++i){(*output)[i*4]=static_cast<byte>(state[i]>>24);(*output)[i*4+1]=static_cast<byte>(state[i]>>16);(*output)[i*4+2]=static_cast<byte>(state[i]>>8);(*output)[i*4+3]=static_cast<byte>(state[i]);}
  return true;
}

std::array<byte, kTimestampProfileMaterialBytesV3> BuildProfileMaterial(
    const TimestampValidatedProfileHandleV3& profile) noexcept {
  std::array<byte, kTimestampProfileMaterialBytesV3> result{};
  const auto& row = profile.identity.legacy_fields;
  std::memcpy(result.data(), "SBTSPP01", 8);
  StoreLittle16(result.data() + 8, 1);
  StoreLittle16(result.data() + 10, kTimestampProfileMaterialBytesV3);
  StoreLittle32(result.data() + 12, kTimestampProfileMaterialBytesV3);
  PutUuid(result.data() + 16, profile.receipt.catalog_snapshot_uuid);
  StoreLittle64(result.data() + 32, profile.receipt.catalog_generation);
  StoreLittle64(result.data() + 40, profile.receipt.registry_generation);
  PutUuid(result.data() + 48, row.descriptor_uuid);
  StoreLittle64(result.data() + 64, row.descriptor_generation);
  PutUuid(result.data() + 72, row.type_uuid);
  StoreLittle64(result.data() + 88, row.type_generation);
  PutUuid(result.data() + 96, row.codec_uuid);
  StoreLittle32(result.data() + 112, row.codec_version);
  StoreLittle64(result.data() + 120, row.codec_generation);
  const std::array<DatatypePolicyIdentityV3, 18> policies{{
      profile.identity.descriptor_policy, profile.identity.canonicalization_policy,
      profile.identity.ordering_policy, profile.identity.hash_policy,
      profile.identity.operation_policy, profile.render_policy, profile.cast_policy,
      profile.calendar_policy, profile.storage_epoch_policy,
      profile.timezone_none_policy, profile.leap_second_policy, profile.index_policy,
      profile.statistics_policy, profile.backup_transport_policy,
      profile.protection_policy, profile.component_adapter_policy,
      profile.diagnostic_policy, profile.metric_policy}};
  for (std::size_t index = 0; index < policies.size(); ++index)
    PutPolicy(result.data() + 128 + index * 24, policies[index]);
  StoreLittle64(result.data() + 560, static_cast<u64>(kTimestampMinimumCivilSecondV3));
  StoreLittle64(result.data() + 568, static_cast<u64>(kTimestampMaximumCivilSecondV3));
  StoreLittle32(result.data() + 576, 9);
  StoreLittle32(result.data() + 580, 16);
  StoreLittle32(result.data() + 584, 0x3fff);
  StoreLittle32(result.data() + 588, 0);
  std::memcpy(result.data() + 592, DateProfileFingerprint(profile.receipt).data(), 32);
  std::memcpy(result.data() + 624, TimeProfileFingerprint(profile.receipt).data(), 32);
  return result;
}

std::array<byte, kTimestampComparisonMaterialBytesV3> BuildComparisonMaterial(
    const TimestampValidatedProfileHandleV3& profile) noexcept {
  std::array<byte, kTimestampComparisonMaterialBytesV3> result{};
  const auto& row = profile.identity.legacy_fields;
  std::memcpy(result.data(), "SBTSPC01", 8);
  StoreLittle16(result.data() + 8, 1);
  StoreLittle16(result.data() + 10, kTimestampComparisonMaterialBytesV3);
  StoreLittle32(result.data() + 12, 0);
  PutUuid(result.data() + 16, profile.receipt.catalog_snapshot_uuid);
  StoreLittle64(result.data() + 32, profile.receipt.catalog_generation);
  StoreLittle64(result.data() + 40, profile.receipt.registry_generation);
  PutUuid(result.data() + 48, row.descriptor_uuid);
  StoreLittle64(result.data() + 64, row.descriptor_generation);
  PutUuid(result.data() + 72, row.type_uuid);
  StoreLittle64(result.data() + 88, row.type_generation);
  PutUuid(result.data() + 96, row.codec_uuid);
  StoreLittle32(result.data() + 112, row.codec_version);
  StoreLittle64(result.data() + 120, row.codec_generation);
  const std::array<DatatypePolicyIdentityV3, 8> policies{{
      profile.identity.descriptor_policy, profile.identity.canonicalization_policy,
      profile.identity.ordering_policy, profile.calendar_policy,
      profile.storage_epoch_policy, profile.timezone_none_policy,
      profile.leap_second_policy, profile.identity.hash_policy}};
  for (std::size_t index = 0; index < policies.size(); ++index)
    PutPolicy(result.data() + 128 + index * 24, policies[index]);
  StoreLittle32(result.data() + 320, 0x01ff);
  StoreLittle32(result.data() + 324, 9);
  StoreLittle64(result.data() + 328, static_cast<u64>(kTimestampMinimumCivilSecondV3));
  StoreLittle64(result.data() + 336, static_cast<u64>(kTimestampMaximumCivilSecondV3));
  StoreLittle32(result.data() + 344, 16);
  StoreLittle32(result.data() + 348, 0);
  std::memcpy(result.data() + 352, DateComparisonFingerprint(profile.receipt).data(), 32);
  std::memcpy(result.data() + 384, TimeComparisonFingerprint(profile.receipt).data(), 32);
  return result;
}

bool ProfileValidNoAlloc(const TimestampValidatedProfileHandleV3& profile,
                         const TimestampExecutionControlV3* control=nullptr) noexcept {
  if (!ExactReceipt(profile.receipt) || !ExactTimestampIdentity(profile.identity) ||
      profile.identity.legacy_fields.catalog_snapshot_uuid != profile.receipt.catalog_snapshot_uuid ||
      profile.identity.legacy_fields.catalog_generation != profile.receipt.catalog_generation ||
      profile.identity.legacy_fields.registry_generation != profile.receipt.registry_generation ||
      !Same(profile.render_policy, kRenderPolicy) || !Same(profile.cast_policy, kCastPolicy) ||
      !Same(profile.calendar_policy, kCalendarPolicy) ||
      !Same(profile.storage_epoch_policy, kStorageEpochPolicy) ||
      !Same(profile.timezone_none_policy, kTimezoneNonePolicy) ||
      !Same(profile.leap_second_policy, kLeapSecondPolicy) ||
      !Same(profile.index_policy, kIndexPolicy) ||
      !Same(profile.statistics_policy, kStatisticsPolicy) ||
      !Same(profile.backup_transport_policy, kBackupPolicy) ||
      !Same(profile.protection_policy, kProtectionPolicy) ||
      !Same(profile.component_adapter_policy, kComponentPolicy) ||
      !Same(profile.diagnostic_policy, kDiagnosticPolicy) ||
      !Same(profile.metric_policy, kMetricPolicy)) return false;
  auto rebuilt_profile=BuildProfileMaterial(profile);
  auto rebuilt_comparison=BuildComparisonMaterial(profile);
  const auto observer=control==nullptr?nullptr:control->observe_scrubbed;
  void* observer_context=control==nullptr?nullptr:control->scrub_observer_context;
  ScopedClear clear_rebuilt_profile(rebuilt_profile.data(),rebuilt_profile.size(),TimestampScrubClassV3::profile_material,observer,observer_context);
  ScopedClear clear_rebuilt_comparison(rebuilt_comparison.data(),rebuilt_comparison.size(),TimestampScrubClassV3::comparison_material,observer,observer_context);
  if(profile.profile_material!=rebuilt_profile ||
     profile.comparison_material!=rebuilt_comparison)return false;
  std::array<byte,32> profile_digest{},comparison_digest{};
  ScopedClear clear_profile(profile_digest.data(),profile_digest.size(),TimestampScrubClassV3::profile_digest,observer,observer_context);
  ScopedClear clear_comparison(comparison_digest.data(),comparison_digest.size(),TimestampScrubClassV3::comparison_digest,observer,observer_context);
  return Digest(profile.profile_material,&profile_digest,control) &&
      Digest(profile.comparison_material,&comparison_digest,control) &&
      profile.profile_fingerprint==profile_digest &&
      profile.comparison_fingerprint==comparison_digest &&
      profile.profile_fingerprint == ProfileFingerprint(profile.receipt) &&
      profile.comparison_fingerprint == ComparisonFingerprint(profile.receipt);
}

bool ValidState(TimestampValueStateV3 state) noexcept {
  return state == TimestampValueStateV3::value || state == TimestampValueStateV3::sql_null;
}

struct ParsedTimestamp {
  bool ok = false;
  std::int32_t civil_day = 0;
  u64 nanoseconds_since_midnight = 0;
  std::string_view code;
  std::string_view detail;
};

bool Digit(char value) noexcept { return value >= '0' && value <= '9'; }
unsigned DigitValue(char value) noexcept {
  return static_cast<unsigned>(value - '0');
}

std::int64_t FloorDiv(std::int64_t value, std::int64_t divisor) noexcept {
  std::int64_t quotient=value/divisor;
  if(value%divisor<0)--quotient;
  return quotient;
}
std::int64_t FloorMod(std::int64_t value,std::int64_t divisor) noexcept {
  const auto remainder=value%divisor;return remainder<0?remainder+divisor:remainder;
}
bool Leap(std::int64_t year) noexcept {
  return FloorMod(year,4)==0&&(FloorMod(year,100)!=0||FloorMod(year,400)==0);
}
u8 DaysInMonth(std::int64_t year,u8 month) noexcept {
  static constexpr u8 kDays[]{31,28,31,30,31,30,31,31,30,31,30,31};
  return month==2&&Leap(year)?29:month>=1&&month<=12?kDays[month-1]:0;
}
bool CivilToDays(std::int64_t year,u8 month,u8 day,std::int64_t* output) noexcept {
  if(output==nullptr||month<1||month>12||day<1||day>DaysInMonth(year,month))return false;
  TimestampWideSigned adjusted=static_cast<TimestampWideSigned>(year)-
      (month<=2?1:0);
  TimestampWideSigned era=adjusted/400;if(adjusted%400<0)--era;
  const TimestampWideSigned yoe=adjusted-era*400;
  const TimestampWideSigned mp=static_cast<int>(month)+(month>2?-3:9);
  const TimestampWideSigned doy=(153*mp+2)/5+day-1;
  const TimestampWideSigned doe=yoe*365+yoe/4-yoe/100+doy;
  const TimestampWideSigned result=era*146097+doe-719468;
  if(result<std::numeric_limits<std::int64_t>::min()||result>std::numeric_limits<std::int64_t>::max())return false;
  *output=static_cast<std::int64_t>(result);return true;
}
TimestampCivilV3 DaysToCivil(std::int64_t day) noexcept {
  const std::int64_t z=day+719468;
  const std::int64_t era=FloorDiv(z,146097);
  const std::int64_t doe=z-era*146097;
  const std::int64_t yoe=(doe-doe/1460+doe/36524-doe/146096)/365;
  std::int64_t year=yoe+era*400;
  const std::int64_t doy=doe-(365*yoe+yoe/4-yoe/100);
  const std::int64_t mp=(5*doy+2)/153;
  const u8 civil_day=static_cast<u8>(doy-(153*mp+2)/5+1);
  const u8 month=static_cast<u8>(mp+(mp<10?3:-9));
  year+=month<=2;
  return {static_cast<std::int32_t>(year),month,civil_day,0,0,0,0};
}
bool ParseDigits(std::string_view text,std::int64_t* value) noexcept {
  if(value==nullptr||text.empty()||!std::all_of(text.begin(),text.end(),Digit))return false;
  std::int64_t result=0;const auto parsed=std::from_chars(text.data(),text.data()+text.size(),result);
  if(parsed.ec!=std::errc{}||parsed.ptr!=text.data()+text.size())return false;
  *value=result;return true;
}
ParsedTimestamp ParseStrict(std::string_view text) noexcept {
  const std::size_t date_bytes=text.size()>=15&&(text[0]=='+'||text[0]=='-')?14:10;
  if(text.size()<date_bytes+1+8)
    return {false,0,0,"CTI.TEMPORAL.INVALID_LITERAL","canonical_text_shape"};
  const auto date=text.substr(0,date_bytes);
  std::int64_t year=0,month=0,day=0;
  if(date_bytes==10){
    if(date[4]!='-'||date[7]!='-'||!ParseDigits(date.substr(0,4),&year)||
       !ParseDigits(date.substr(5,2),&month)||!ParseDigits(date.substr(8,2),&day))
      return {false,0,0,"CTI.TEMPORAL.INVALID_LITERAL","date_grammar"};
  }else{
    std::int64_t magnitude=0;
    if(date[8]!='-'||date[11]!='-'||!ParseDigits(date.substr(1,7),&magnitude)||
       !ParseDigits(date.substr(9,2),&month)||!ParseDigits(date.substr(12,2),&day))
      return {false,0,0,"CTI.TEMPORAL.INVALID_LITERAL","date_grammar"};
    year=date[0]=='-'?-magnitude:magnitude;
    if(year>=0&&year<=9999)return {false,0,0,"CTI.TEMPORAL.INVALID_LITERAL","expanded_four_digit_alias"};
  }
  if(month==0||day==0)return {false,0,0,"CTI.TEMPORAL.ZERO_DATE_REFUSED","zero_month_or_day"};
  std::int64_t civil_day=0;
  if(!CivilToDays(year,static_cast<u8>(month),static_cast<u8>(day),&civil_day))
    return {false,0,0,"CTI.TEMPORAL.INVALID_LITERAL","civil_date_invalid"};
  if(civil_day<std::numeric_limits<std::int32_t>::min()||civil_day>std::numeric_limits<std::int32_t>::max())
    return {false,0,0,"CTI.TEMPORAL.RANGE_EXCEEDED","civil_date_out_of_range"};
  if(text[date_bytes]!='T')
    return {false,0,0,"CTI.TEMPORAL.INVALID_LITERAL","separator_T"};
  const auto time=text.substr(date_bytes+1);
  if(time.size()<8||time[2]!=':'||time[5]!=':'||!Digit(time[0])||!Digit(time[1])||
     !Digit(time[3])||!Digit(time[4])||!Digit(time[6])||!Digit(time[7]))
    return {false,0,0,"CTI.TEMPORAL.INVALID_LITERAL","time_grammar"};
  const unsigned hour=DigitValue(time[0])*10u+DigitValue(time[1]);
  const unsigned minute=DigitValue(time[3])*10u+DigitValue(time[4]);
  const unsigned second=DigitValue(time[6])*10u+DigitValue(time[7]);
  if(hour>23||minute>59||second>60)return {false,0,0,"CTI.TEMPORAL.INVALID_LITERAL","time_field_invalid"};
  if(second==60)return {false,0,0,"CTI.TEMPORAL.LEAP_SECOND_REFUSED","second"};
  u32 fraction=0;
  if(time.size()!=8){
    const auto suffix=time.substr(8);
    if(suffix.size()<2||suffix[0]!='.'||!std::all_of(suffix.begin()+1,suffix.end(),Digit))
      return {false,0,0,"CTI.TEMPORAL.INVALID_LITERAL","fraction_syntax"};
    const auto digits=suffix.size()-1;
    if(digits>9)return {false,0,0,"CTI.TEMPORAL.PRECISION_LOSS","fraction_precision"};
    if(suffix.back()=='0')return {false,0,0,"CTI.TEMPORAL.INVALID_LITERAL","fraction_trailing_zero"};
    for(std::size_t i=1;i<suffix.size();++i)fraction=fraction*10u+static_cast<u32>(suffix[i]-'0');
    for(std::size_t i=digits;i<9;++i)fraction*=10u;
  }
  const u64 nanos=static_cast<u64>(hour)*3'600'000'000'000ull+
      static_cast<u64>(minute)*60'000'000'000ull+static_cast<u64>(second)*1'000'000'000ull+fraction;
  return {true,static_cast<std::int32_t>(civil_day),nanos,{},{}};
}

std::size_t RenderTime(u64 value, char* output) noexcept {
  const u64 hour = value / 3'600'000'000'000ull;
  value %= 3'600'000'000'000ull;
  const u64 minute = value / 60'000'000'000ull;
  value %= 60'000'000'000ull;
  const u64 second = value / 1'000'000'000ull;
  const u32 fraction = static_cast<u32>(value % 1'000'000'000ull);
  output[0]=static_cast<char>('0'+hour/10); output[1]=static_cast<char>('0'+hour%10);
  output[2]=':'; output[3]=static_cast<char>('0'+minute/10); output[4]=static_cast<char>('0'+minute%10);
  output[5]=':'; output[6]=static_cast<char>('0'+second/10); output[7]=static_cast<char>('0'+second%10);
  if (fraction == 0) return 8;
  output[8]='.';
  u32 divisor=100'000'000;
  std::size_t end=18;
  for (std::size_t index=9; index<18; ++index) {
    output[index]=static_cast<char>('0'+(fraction/divisor)%10);
    divisor/=10;
  }
  while (end>9 && output[end-1]=='0') --end;
  return end;
}

std::size_t RenderStrict(std::int32_t day,u64 nanos,char* output) noexcept {
  const auto civil=DaysToCivil(day);
  const int date_bytes=civil.year>=0&&civil.year<=9999
      ?std::snprintf(output,15,"%04d-%02u-%02u",civil.year,civil.month,civil.day)
      :std::snprintf(output,15,"%c%07lld-%02u-%02u",civil.year<0?'-':'+',
          static_cast<long long>(civil.year<0?-static_cast<std::int64_t>(civil.year):civil.year),
          civil.month,civil.day);
  if(date_bytes!=10&&date_bytes!=14)return 0;
  output[date_bytes]='T';return static_cast<std::size_t>(date_bytes)+1+RenderTime(nanos,output+date_bytes+1);
}

bool EngineUuidEqual(const scratchbird::engine::Uuid& left,
                     const platform::Uuid& right) noexcept {
  return std::equal(std::begin(left.bytes), std::end(left.bytes),
                    right.bytes.begin());
}

bool EngineUuidNil(const scratchbird::engine::Uuid& value) noexcept {
  return std::all_of(std::begin(value.bytes), std::end(value.bytes),
                     [](byte octet) { return octet == 0; });
}

struct TimestampPeerDescriptorShape {
  scratchbird::engine::ExecutionTypeFamily family;
  scratchbird::engine::ExecutionTypeWidthClass width;
  u32 bits;
  u32 precision;
  u32 scale;
};

bool TimestampPeerDescriptorShapeFor(
    CanonicalTypeId type, TimestampPeerDescriptorShape* shape) noexcept {
  using Family = scratchbird::engine::ExecutionTypeFamily;
  using Width = scratchbird::engine::ExecutionTypeWidthClass;
  if (shape == nullptr) return false;
  switch (type) {
    case CanonicalTypeId::boolean: *shape={Family::boolean,Width::fixed,1,0,0};break;
    case CanonicalTypeId::int8: *shape={Family::signed_integer,Width::fixed,8,0,0};break;
    case CanonicalTypeId::int16: *shape={Family::signed_integer,Width::fixed,16,0,0};break;
    case CanonicalTypeId::int32: *shape={Family::signed_integer,Width::fixed,32,0,0};break;
    case CanonicalTypeId::int64: *shape={Family::signed_integer,Width::fixed,64,0,0};break;
    case CanonicalTypeId::int128: *shape={Family::signed_integer,Width::fixed,128,128,0};break;
    case CanonicalTypeId::uint8: *shape={Family::unsigned_integer,Width::fixed,8,0,0};break;
    case CanonicalTypeId::uint16: *shape={Family::unsigned_integer,Width::fixed,16,0,0};break;
    case CanonicalTypeId::uint32: *shape={Family::unsigned_integer,Width::fixed,32,0,0};break;
    case CanonicalTypeId::uint64: *shape={Family::unsigned_integer,Width::fixed,64,0,0};break;
    case CanonicalTypeId::uint128: *shape={Family::unsigned_integer,Width::fixed,128,128,0};break;
    case CanonicalTypeId::bfloat16:
    case CanonicalTypeId::real16: *shape={Family::real,Width::fixed,16,0,0};break;
    case CanonicalTypeId::real32: *shape={Family::real,Width::fixed,32,0,0};break;
    case CanonicalTypeId::real64: *shape={Family::real,Width::fixed,64,0,0};break;
    case CanonicalTypeId::real128: *shape={Family::real,Width::fixed,128,113,0};break;
    case CanonicalTypeId::decimal: *shape={Family::decimal,Width::descriptor_defined,0,38,0};break;
    case CanonicalTypeId::decimal_float: *shape={Family::decimal,Width::descriptor_defined,0,34,0};break;
    case CanonicalTypeId::uuid: *shape={Family::uuid,Width::fixed,128,0,0};break;
    case CanonicalTypeId::ip_address: *shape={Family::network,Width::fixed,128,0,0};break;
    case CanonicalTypeId::network_prefix: *shape={Family::network,Width::fixed,144,0,0};break;
    case CanonicalTypeId::mac_address: *shape={Family::network,Width::fixed,64,0,0};break;
    case CanonicalTypeId::character: *shape={Family::character,Width::variable,0,0,0};break;
    case CanonicalTypeId::binary: *shape={Family::binary,Width::variable,0,0,0};break;
    case CanonicalTypeId::bit_string: *shape={Family::bit_string,Width::descriptor_defined,0,0,0};break;
    case CanonicalTypeId::date: *shape={Family::temporal,Width::fixed,32,0,0};break;
    case CanonicalTypeId::time: *shape={Family::temporal,Width::fixed,64,0,0};break;
    case CanonicalTypeId::timestamp:
    case CanonicalTypeId::interval: *shape={Family::temporal,Width::fixed,128,0,0};break;
    case CanonicalTypeId::json_document: *shape={Family::document,Width::descriptor_defined,0,0,0};break;
    case CanonicalTypeId::enum_value: *shape={Family::structured,Width::fixed,128,0,0};break;
    case CanonicalTypeId::list: *shape={Family::structured,Width::descriptor_defined,0,0,0};break;
    case CanonicalTypeId::geometry: *shape={Family::spatial,Width::descriptor_defined,0,0,0};break;
    default:return false;
  }
  return true;
}

bool DescriptorBindsIdentityNoAlloc(
    const scratchbird::engine::ExecutionTypeDescriptor& descriptor,
    const DatatypeTypeCodecIdentityRowV3& identity,
    CanonicalTypeId expected_type) noexcept {
  TimestampPeerDescriptorShape shape{};
  const auto* current = IdentityForReceipt(expected_type, ReceiptForIdentity(identity));
  if (current == nullptr || !EqualIdentityIgnoringName(identity, *current) ||
      !TimestampPeerDescriptorShapeFor(expected_type, &shape) ||
      !EngineUuidEqual(descriptor.descriptor_uuid,
                       identity.legacy_fields.descriptor_uuid) ||
      descriptor.descriptor_epoch !=
          identity.legacy_fields.descriptor_generation ||
      descriptor.canonical_type_id != static_cast<u32>(expected_type)) {
    return false;
  }
  u64 flags = 0;
  if (shape.precision != 0)
    flags |= scratchbird::engine::ExecutionTypeModifierFlagBit(
        scratchbird::engine::ExecutionTypeModifierFlag::precision);
  if (expected_type == CanonicalTypeId::decimal ||
      expected_type == CanonicalTypeId::decimal_float)
    flags |= scratchbird::engine::ExecutionTypeModifierFlagBit(
        scratchbird::engine::ExecutionTypeModifierFlag::scale);
  if (expected_type == CanonicalTypeId::character && descriptor.length != 0)
    flags |= scratchbird::engine::ExecutionTypeModifierFlagBit(
        scratchbird::engine::ExecutionTypeModifierFlag::length);
  return descriptor.family == shape.family && descriptor.width_class == shape.width &&
      descriptor.bit_width == shape.bits &&
      descriptor.precision == shape.precision && descriptor.scale == shape.scale &&
      (expected_type == CanonicalTypeId::character || descriptor.length == 0) &&
      descriptor.vector_dimensions == 0 && descriptor.container_rank == 0 &&
      descriptor.modifier_flags == flags && EngineUuidNil(descriptor.domain_uuid) &&
      descriptor.domain_stack.empty() && EngineUuidNil(descriptor.charset_uuid) &&
      EngineUuidNil(descriptor.collation_uuid) && EngineUuidNil(descriptor.timezone_uuid) &&
      EngineUuidNil(descriptor.element_descriptor_uuid) &&
      EngineUuidNil(descriptor.security_policy_uuid) && descriptor.nullable_allowed &&
      descriptor.descriptor_authoritative && descriptor.parser_independent;
}

bool ExecutionDescriptorPresentNoAlloc(
    const scratchbird::engine::ExecutionTypeDescriptor& descriptor) noexcept {
  return !EngineUuidNil(descriptor.descriptor_uuid) ||
      descriptor.descriptor_epoch != 0 || descriptor.canonical_type_id != 0 ||
      descriptor.family != scratchbird::engine::ExecutionTypeFamily::unknown ||
      descriptor.width_class != scratchbird::engine::ExecutionTypeWidthClass::unknown ||
      !descriptor.stable_name.empty() || descriptor.bit_width != 0 ||
      descriptor.precision != 0 || descriptor.scale != 0 || descriptor.length != 0 ||
      descriptor.vector_dimensions != 0 || descriptor.container_rank != 0 ||
      descriptor.modifier_flags != 0 || !EngineUuidNil(descriptor.domain_uuid) ||
      !descriptor.domain_stack.empty() || !EngineUuidNil(descriptor.charset_uuid) ||
      !EngineUuidNil(descriptor.collation_uuid) || !EngineUuidNil(descriptor.timezone_uuid) ||
      !EngineUuidNil(descriptor.element_descriptor_uuid) ||
      !EngineUuidNil(descriptor.security_policy_uuid) || !descriptor.nullable_allowed ||
      !descriptor.descriptor_authoritative || !descriptor.parser_independent;
}

struct TimestampCastRowShape {
  bool incoming = false;
  bool contextual_null = false;
  bool exact_peer_identity = false;
  CanonicalTypeId peer_type = CanonicalTypeId::unknown;
};

bool ResolveTimestampCastRowShape(u32 row,
                                  TimestampCastRowShape* output) noexcept {
  if (output == nullptr || row == 0 || row > kTimestampClosedCastPolicyRowsV3)
    return false;
  if (row == 1) {
    *output = {true, true, false, CanonicalTypeId::null_type};
    return true;
  }
  static constexpr std::array<CanonicalTypeId,29> kScalarThroughInterval{{
      CanonicalTypeId::boolean,CanonicalTypeId::int8,CanonicalTypeId::int16,
      CanonicalTypeId::int32,CanonicalTypeId::int64,CanonicalTypeId::int128,
      CanonicalTypeId::uint8,CanonicalTypeId::uint16,CanonicalTypeId::uint32,
      CanonicalTypeId::uint64,CanonicalTypeId::uint128,CanonicalTypeId::bfloat16,
      CanonicalTypeId::real16,CanonicalTypeId::real32,CanonicalTypeId::real64,
      CanonicalTypeId::real128,CanonicalTypeId::decimal,CanonicalTypeId::decimal_float,
      CanonicalTypeId::uuid,CanonicalTypeId::ip_address,CanonicalTypeId::network_prefix,
      CanonicalTypeId::mac_address,CanonicalTypeId::character,CanonicalTypeId::binary,
      CanonicalTypeId::bit_string,CanonicalTypeId::timestamp,CanonicalTypeId::date,
      CanonicalTypeId::time,CanonicalTypeId::interval}};
  static constexpr std::array<CanonicalTypeId,56> kRemainingBaseTypes{{
      CanonicalTypeId::blob,CanonicalTypeId::document,CanonicalTypeId::json_document,
      CanonicalTypeId::binary_json_document,CanonicalTypeId::bson_document,
      CanonicalTypeId::xml_document,CanonicalTypeId::hstore_document,
      CanonicalTypeId::object_document,CanonicalTypeId::flattened_object_document,
      CanonicalTypeId::enum_value,CanonicalTypeId::set_value,CanonicalTypeId::array,
      CanonicalTypeId::list,CanonicalTypeId::map,CanonicalTypeId::row,
      CanonicalTypeId::composite,CanonicalTypeId::variant,CanonicalTypeId::range,
      CanonicalTypeId::multirange,CanonicalTypeId::token_stream,
      CanonicalTypeId::search_query,CanonicalTypeId::search_rank_feature,
      CanonicalTypeId::search_completion,CanonicalTypeId::search_percolator,
      CanonicalTypeId::geometry,CanonicalTypeId::geography,CanonicalTypeId::point,
      CanonicalTypeId::shape,CanonicalTypeId::raster,CanonicalTypeId::vector,
      CanonicalTypeId::dense_vector,CanonicalTypeId::sparse_vector,
      CanonicalTypeId::binary_vector,CanonicalTypeId::quantized_vector,
      CanonicalTypeId::graph_node,CanonicalTypeId::graph_edge,CanonicalTypeId::graph_path,
      CanonicalTypeId::time_series_value,CanonicalTypeId::columnar_segment,
      CanonicalTypeId::aggregate_state,CanonicalTypeId::hll_sketch,
      CanonicalTypeId::bloom_filter,CanonicalTypeId::quantile_sketch,
      CanonicalTypeId::histogram_sketch,CanonicalTypeId::ranking_summary,
      CanonicalTypeId::vector_summary,CanonicalTypeId::lob_locator,
      CanonicalTypeId::external_file_locator,CanonicalTypeId::remote_object_locator,
      CanonicalTypeId::bridge_handle,CanonicalTypeId::cursor_handle,
      CanonicalTypeId::system_reference,CanonicalTypeId::opaque_extension,
      CanonicalTypeId::cursor,CanonicalTypeId::result_set,CanonicalTypeId::table_value}};
  bool incoming = row >= 138;
  const u32 outbound_row = incoming ? row - 84 : row;
  CanonicalTypeId peer = CanonicalTypeId::unknown;
  bool exact = false;
  if (outbound_row >= 28 && outbound_row <= 56) {
    peer = kScalarThroughInterval[outbound_row - 28];
    exact = true;
  } else if (outbound_row >= 57 && outbound_row <= 112) {
    peer = kRemainingBaseTypes[outbound_row - 57];
    exact = outbound_row == 59 || outbound_row == 66 ||
            outbound_row == 69 || outbound_row == 81;
  } else if (row >= 2 && row <= 26) {
    incoming = true;
    peer = kScalarThroughInterval[row - 2];
    exact = true;
  } else if (outbound_row == 27) {
    peer = CanonicalTypeId::null_type;
  } else if (outbound_row < 113 || outbound_row > 137) {
    return false;
  }
  *output = {incoming, false, exact, peer};
  return true;
}

bool ExactPeerIdentity(const DatatypeTypeCodecIdentityRowV3* supplied,
                       CanonicalTypeId expected_type) noexcept {
  const auto* expected = supplied == nullptr ? nullptr :
      IdentityForReceipt(expected_type, ReceiptForIdentity(*supplied));
  return supplied != nullptr && expected != nullptr &&
      EqualIdentityIgnoringName(*supplied, *expected);
}

bool ExactCharacterDescriptor(
    const scratchbird::engine::ExecutionTypeDescriptor& descriptor,
    const TimestampAuthorityReceiptV3& receipt) noexcept {
  const auto* identity = IdentityForReceipt(CanonicalTypeId::character, receipt);
  return ExactReceipt(receipt) && identity != nullptr &&
      descriptor.length <= 16'777'216 &&
      DescriptorBindsIdentityNoAlloc(descriptor, *identity,
                                     CanonicalTypeId::character);
}

bool CharacterIdentity(const DatatypeTypeCodecIdentityRowV3* identity) noexcept {
  const auto* current = identity == nullptr ? nullptr :
      IdentityForReceipt(CanonicalTypeId::character, ReceiptForIdentity(*identity));
  return identity != nullptr && current != nullptr &&
      EqualIdentityIgnoringName(*identity, *current);
}

bool CharacterDescriptor(const scratchbird::engine::ExecutionTypeDescriptor* descriptor,
                         const DatatypeTypeCodecIdentityRowV3* identity) noexcept {
  const auto nil=[](const auto& value) noexcept {
    return std::all_of(std::begin(value.bytes),std::end(value.bytes),
                       [](byte octet){return octet==0;});
  };
  const u64 length_flag=scratchbird::engine::ExecutionTypeModifierFlagBit(
      scratchbird::engine::ExecutionTypeModifierFlag::length);
  return descriptor != nullptr && identity != nullptr &&
      std::equal(std::begin(descriptor->descriptor_uuid.bytes),
                 std::end(descriptor->descriptor_uuid.bytes),
                 identity->legacy_fields.descriptor_uuid.bytes.begin()) &&
      descriptor->canonical_type_id == static_cast<u32>(CanonicalTypeId::character) &&
      descriptor->descriptor_epoch == identity->legacy_fields.descriptor_generation &&
      descriptor->family==scratchbird::engine::ExecutionTypeFamily::character &&
      descriptor->width_class==scratchbird::engine::ExecutionTypeWidthClass::variable &&
      descriptor->bit_width==0 && descriptor->precision==0 && descriptor->scale==0 &&
      descriptor->length<=16'777'216 && descriptor->vector_dimensions==0 &&
      descriptor->container_rank==0 &&
      descriptor->modifier_flags==(descriptor->length==0?0:length_flag) &&
      nil(descriptor->domain_uuid) && descriptor->domain_stack.empty() &&
      nil(descriptor->charset_uuid) && nil(descriptor->collation_uuid) &&
      nil(descriptor->timezone_uuid) && nil(descriptor->element_descriptor_uuid) &&
      nil(descriptor->security_policy_uuid) && descriptor->descriptor_authoritative &&
      descriptor->parser_independent;
}

bool TimestampDescriptor(const scratchbird::engine::ExecutionTypeDescriptor* descriptor,
                    const DatatypeTypeCodecIdentityRowV3& identity,
                    bool expected_nullable) noexcept {
  if(descriptor==nullptr)return false;
  const auto nil=[](const auto& value) noexcept {
    return std::all_of(std::begin(value.bytes),std::end(value.bytes),
                       [](byte octet){return octet==0;});
  };
  return std::equal(std::begin(descriptor->descriptor_uuid.bytes),
                    std::end(descriptor->descriptor_uuid.bytes),
                    identity.legacy_fields.descriptor_uuid.bytes.begin()) &&
      descriptor->descriptor_epoch==identity.legacy_fields.descriptor_generation &&
      descriptor->canonical_type_id==static_cast<u32>(CanonicalTypeId::timestamp) &&
      descriptor->family==scratchbird::engine::ExecutionTypeFamily::temporal &&
      descriptor->width_class==scratchbird::engine::ExecutionTypeWidthClass::fixed &&
      descriptor->bit_width==128 && descriptor->precision==0 && descriptor->scale==0 &&
      descriptor->length==0 && descriptor->vector_dimensions==0 &&
      descriptor->container_rank==0 && descriptor->modifier_flags==0 &&
      descriptor->nullable_allowed==expected_nullable &&
      nil(descriptor->domain_uuid) && descriptor->domain_stack.empty() &&
      nil(descriptor->charset_uuid) && nil(descriptor->collation_uuid) &&
      nil(descriptor->timezone_uuid) && nil(descriptor->element_descriptor_uuid) &&
      nil(descriptor->security_policy_uuid) && descriptor->descriptor_authoritative &&
      descriptor->parser_independent;
}

}  // namespace

TimestampProfileResultV3 BuildCurrentTimestampValidatedProfileHandleV3(
    const platform::Uuid& statement_receipt_uuid) noexcept {
  const auto* identity = IdentityForReceipt(CanonicalTypeId::timestamp, {kSnapshot,kSnapshot,10,10});
  if (identity == nullptr)
    return Failure<TimestampProfileResultV3>("CTI.TEMPORAL.DESCRIPTOR_INVALID",
                                        "d710_timestamp_identity_missing");
  return BuildTimestampValidatedProfileHandleV3(
      {statement_receipt_uuid,kSnapshot,10,10}, *identity);
}

TimestampProfileResultV3 BuildTimestampValidatedProfileHandleV3(
    const TimestampAuthorityReceiptV3& receipt,
    const DatatypeTypeCodecIdentityRowV3& identity) noexcept {
  if (!ExactReceipt(receipt) || !ExactTimestampIdentity(identity) ||
      identity.legacy_fields.catalog_snapshot_uuid != receipt.catalog_snapshot_uuid ||
      identity.legacy_fields.catalog_generation != receipt.catalog_generation ||
      identity.legacy_fields.registry_generation != receipt.registry_generation)
    return Failure<TimestampProfileResultV3>("CTI.TEMPORAL.DESCRIPTOR_INVALID",
                                        "receipt_or_identity_invalid");
  try {
    auto result = Success<TimestampProfileResultV3>();
    auto& p = result.profile;
    p.receipt=receipt; p.identity=identity;
    p.render_policy=kRenderPolicy; p.cast_policy=kCastPolicy;
    p.calendar_policy=kCalendarPolicy; p.storage_epoch_policy=kStorageEpochPolicy;
    p.timezone_none_policy=kTimezoneNonePolicy; p.leap_second_policy=kLeapSecondPolicy;
    p.index_policy=kIndexPolicy; p.statistics_policy=kStatisticsPolicy;
    p.backup_transport_policy=kBackupPolicy; p.protection_policy=kProtectionPolicy;
    p.component_adapter_policy=kComponentPolicy; p.diagnostic_policy=kDiagnosticPolicy;
    p.metric_policy=kMetricPolicy;
    p.profile_material=BuildProfileMaterial(p);
    p.comparison_material=BuildComparisonMaterial(p);
    if (!Digest(p.profile_material,&p.profile_fingerprint) ||
        !Digest(p.comparison_material,&p.comparison_fingerprint))
      return Failure<TimestampProfileResultV3>("RESOURCE.BUDGET_EXCEEDED",
                                          "sha256_provider",ResourceStatus());
    if (p.profile_fingerprint != ProfileFingerprint(p.receipt))
      return Failure<TimestampProfileResultV3>("CTI.TEMPORAL.DESCRIPTOR_INVALID",
                                                "profile_fingerprint_mismatch");
    if (p.comparison_fingerprint != ComparisonFingerprint(p.receipt))
      return Failure<TimestampProfileResultV3>("CTI.TEMPORAL.DESCRIPTOR_INVALID",
                                                "comparison_fingerprint_mismatch");
    if (!ProfileValidNoAlloc(p))
      return Failure<TimestampProfileResultV3>("CTI.TEMPORAL.DESCRIPTOR_INVALID",
                                          "profile_material_mismatch");
    return result;
  } catch (...) {
    return Failure<TimestampProfileResultV3>("RESOURCE.BUDGET_EXCEEDED",
                                        "profile_allocation",ResourceStatus());
  }
}

TimestampValidationResultV3 ValidateTimestampProfileHandleV3(
    const TimestampValidatedProfileHandleV3& profile,
    const TimestampExecutionControlV3& control) noexcept {
  if (!ProfileValidNoAlloc(profile,&control))
    return Failure<TimestampValidationResultV3>("CTI.TEMPORAL.DESCRIPTOR_INVALID",
                                           "profile_invalid");
  if(Cancelled(control))return Failure<TimestampValidationResultV3>("PROCESS.CANCELLED","before_publication");
  return Success<TimestampValidationResultV3>();
}

TimestampViewResultV3 ValidateTimestampValueViewV3(
    const TimestampValueViewV3& value, bool null_allowed) noexcept {
  if (value.profile == nullptr || !ProfileValidNoAlloc(*value.profile))
    return Failure<TimestampViewResultV3>("CTI.TEMPORAL.DESCRIPTOR_INVALID", "profile_invalid");
  if (!ValidState(value.state))
    return Failure<TimestampViewResultV3>("DATATYPE.NULL_STATE.INVALID", "state_unknown");
  if (value.state == TimestampValueStateV3::sql_null) {
    if (value.civil_day != 0 || value.nanoseconds_since_midnight != 0)
      return Failure<TimestampViewResultV3>("DATATYPE.NULL_STATE.INVALID", "dirty_null_scalar");
    if (!null_allowed)
      return Failure<TimestampViewResultV3>("DATATYPE.NULL_NOT_ADMITTED", "null_not_allowed");
  } else if (value.nanoseconds_since_midnight > kTimestampMaximumNanosecondsV3) {
    return Failure<TimestampViewResultV3>("CTI.TEMPORAL.CANONICAL_ENCODING_INVALID", "value_out_of_range");
  }
  auto result=Success<TimestampViewResultV3>(); result.value=value; return result;
}

TimestampViewResultV3 AdmitTimestampOperandV3(
    const TimestampOperandV3& operand, bool null_allowed) noexcept {
  if (operand.day_carrier != TimestampDayCarrierKindV3::signed_i32 ||
      operand.time_carrier != TimestampUnsignedCarrierKindV3::unsigned_u64)
    return Failure<TimestampViewResultV3>("SBLR.OPERAND_INVALID", "operand_carrier_invalid");
  if (!operand.profile || !ProfileValidNoAlloc(*operand.profile))
    return Failure<TimestampViewResultV3>("CTI.TEMPORAL.DESCRIPTOR_INVALID", "operand_profile_invalid");
  if (!ValidState(operand.state))
    return Failure<TimestampViewResultV3>("DATATYPE.NULL_STATE.INVALID", "operand_state_unknown");
  if (operand.state == TimestampValueStateV3::sql_null) {
    if (operand.civil_day != 0 || operand.nanoseconds_since_midnight != 0)
      return Failure<TimestampViewResultV3>("DATATYPE.NULL_STATE.INVALID", "operand_dirty_null");
    if (!null_allowed)
      return Failure<TimestampViewResultV3>("DATATYPE.NULL_NOT_ADMITTED", "operand_null_not_allowed");
  } else {
    if (operand.civil_day < std::numeric_limits<std::int32_t>::min() ||
        operand.civil_day > std::numeric_limits<std::int32_t>::max() ||
        operand.nanoseconds_since_midnight > kTimestampMaximumNanosecondsV3)
      return Failure<TimestampViewResultV3>("CTI.TEMPORAL.RANGE_EXCEEDED", "operand_value_out_of_range");
  }
  auto result=Success<TimestampViewResultV3>();
  result.value={operand.profile.get(),operand.state,static_cast<std::int32_t>(operand.civil_day),operand.nanoseconds_since_midnight};
  return result;
}

namespace {
void EncodeComponent(std::int32_t day,u64 nanos,byte* output) noexcept {
  const std::int64_t seconds=static_cast<std::int64_t>(day)*86'400+
      static_cast<std::int64_t>(nanos/1'000'000'000ull);
  StoreLittle64(output,static_cast<u64>(seconds));
  StoreLittle32(output+8,static_cast<u32>(nanos%1'000'000'000ull));
  StoreLittle32(output+12,0);
}
TimestampOwnedValueV3 Owned(const std::shared_ptr<const TimestampValidatedProfileHandleV3>& profile,
                            TimestampValueStateV3 state,std::int32_t day,u64 nanos) {
  return {profile,state,day,nanos};
}
TimestampValueResultV3 PublishValue(TimestampOwnedValueV3 value,
                                    const TimestampExecutionControlV3& control) noexcept {
  if(Cancelled(control))return Failure<TimestampValueResultV3>("PROCESS.CANCELLED","before_publication");
  auto r=Success<TimestampValueResultV3>();r.value=std::move(value);return r;
}
TimestampValidationResultV3 Authority(const TimestampValueViewV3& value) noexcept {
  if(value.profile==nullptr||!ProfileValidNoAlloc(*value.profile))
    return Failure<TimestampValidationResultV3>("CTI.TEMPORAL.DESCRIPTOR_INVALID","profile_invalid");
  return Success<TimestampValidationResultV3>();
}
TimestampScalarResultV3 ScalarNullOrReady(const TimestampValueViewV3& value,bool null_allowed,
                                          const TimestampExecutionControlV3& control,
                                          bool* ready) noexcept {
  *ready=false;const auto checked=ValidateTimestampValueViewV3(value,null_allowed);
  if(!checked.ok())return Failure<TimestampScalarResultV3>(checked.diagnostic.diagnostic_code,checked.diagnostic.detail,checked.status);
  auto r=Success<TimestampScalarResultV3>();
  if(value.state==TimestampValueStateV3::sql_null){if(Cancelled(control))return Failure<TimestampScalarResultV3>("PROCESS.CANCELLED","before_publication");r.is_null=true;return r;}
  *ready=true;return r;
}
}

TimestampViewResultV3 DecodeCanonicalTimestampComponentNoAllocV3(
    const TimestampValidatedProfileHandleV3& profile, TimestampValueStateV3 state,
    bool null_allowed, std::span<const byte> component,
    const TimestampExecutionControlV3& control) noexcept {
  if(!ValidState(state))return Failure<TimestampViewResultV3>("CTI.TEMPORAL.CANONICAL_ENCODING_INVALID","component_state");
  if((state==TimestampValueStateV3::sql_null&&!component.empty())||
     (state==TimestampValueStateV3::value&&component.size()!=16))
    return Failure<TimestampViewResultV3>("CTI.TEMPORAL.CANONICAL_ENCODING_INVALID","component_extent");
  if(!ProfileValidNoAlloc(profile,&control))return Failure<TimestampViewResultV3>("CTI.TEMPORAL.DESCRIPTOR_INVALID","profile_invalid");
  if(state==TimestampValueStateV3::sql_null){
    if(!null_allowed)return Failure<TimestampViewResultV3>("DATATYPE.NULL_NOT_ADMITTED","null_not_allowed");
    if(Cancelled(control))return Failure<TimestampViewResultV3>("PROCESS.CANCELLED","before_publication");
    auto r=Success<TimestampViewResultV3>();r.value={&profile,state,0,0};return r;
  }
  const std::int64_t civil_second=static_cast<std::int64_t>(LoadLittle64(component.data()));
  const u32 nanos_second=LoadLittle32(component.data()+8);
  if(nanos_second>=1'000'000'000u||LoadLittle32(component.data()+12)!=0)
    return Failure<TimestampViewResultV3>("CTI.TEMPORAL.CANONICAL_ENCODING_INVALID","component_field_invalid");
  const std::int64_t day=FloorDiv(civil_second,86'400);
  const std::int64_t second_of_day=civil_second-day*86'400;
  if(day<std::numeric_limits<std::int32_t>::min()||day>std::numeric_limits<std::int32_t>::max())
    return Failure<TimestampViewResultV3>("CTI.TEMPORAL.CANONICAL_ENCODING_INVALID","component_day_range");
  const u64 nanos=static_cast<u64>(second_of_day)*1'000'000'000ull+nanos_second;
  std::array<byte,16> check{};ScopedClear clear(check.data(),check.size(),TimestampScrubClassV3::component_decode_reencode,control.observe_scrubbed,control.scrub_observer_context);
  EncodeComponent(static_cast<std::int32_t>(day),nanos,check.data());
  if(control.force_reencode_mismatch_for_conformance)check[0]^=1;
  if(!std::equal(check.begin(),check.end(),component.begin()))
    return Failure<TimestampViewResultV3>("CTI.TEMPORAL.CANONICAL_ENCODING_INVALID","component_reencode");
  if(component.size()>control.maximum_allocation_bytes)return Failure<TimestampViewResultV3>("RESOURCE.BUDGET_EXCEEDED","component_decode_budget",ResourceStatus());
  if(Cancelled(control))return Failure<TimestampViewResultV3>("PROCESS.CANCELLED","before_publication");
  auto r=Success<TimestampViewResultV3>();r.value={&profile,state,static_cast<std::int32_t>(day),nanos};return r;
}

TimestampNoAllocWriteResultV3 EncodeCanonicalTimestampComponentIntoNoAllocV3(
    const TimestampOwnedValueV3& value, byte* output, u64 capacity,
    const TimestampExecutionControlV3& control) noexcept {
  const auto checked=ValidateTimestampValueViewV3(value.view(),true);
  if(!checked.ok())return Failure<TimestampNoAllocWriteResultV3>(checked.diagnostic.diagnostic_code,checked.diagnostic.detail,checked.status);
  const u64 required=value.state==TimestampValueStateV3::value?16:0;
  std::array<byte,16> staged{};ScopedClear clear(staged.data(),staged.size(),TimestampScrubClassV3::component_encode_staging,control.observe_scrubbed,control.scrub_observer_context);
  if(output&&(RangesOverlap(output,required,&value,sizeof(value))||RangesOverlap(output,required,&control,sizeof(control))||OutputOverlapsProfile(output,required,*value.profile)))
    return Failure<TimestampNoAllocWriteResultV3>("CTI.TEMPORAL.CANONICAL_ENCODING_INVALID","component_overlap");
  if(required>capacity||(required&&output==nullptr)){auto r=Failure<TimestampNoAllocWriteResultV3>("RESOURCE.BUDGET_EXCEEDED","component_capacity",ResourceStatus());r.bytes_required=required;return r;}
  if(required>control.maximum_allocation_bytes){auto r=Failure<TimestampNoAllocWriteResultV3>("RESOURCE.BUDGET_EXCEEDED","component_budget",ResourceStatus());r.bytes_required=required;return r;}
  if(required)EncodeComponent(value.civil_day,value.nanoseconds_since_midnight,staged.data());
  if(Cancelled(control))return Failure<TimestampNoAllocWriteResultV3>("PROCESS.CANCELLED","before_publication");
  if(required)std::memcpy(output,staged.data(),required);
  auto r=Success<TimestampNoAllocWriteResultV3>();r.bytes_required=required;r.bytes_written=required;r.containing_null=required==0;return r;
}
TimestampBytesResultV3 EncodeCanonicalTimestampComponentV3(
    const TimestampOwnedValueV3& value,const TimestampExecutionControlV3& control) noexcept {
  const auto checked=ValidateTimestampValueViewV3(value.view(),true);
  if(!checked.ok())return Failure<TimestampBytesResultV3>(checked.diagnostic.diagnostic_code,checked.diagnostic.detail,checked.status);
  auto profile_pin=value.profile;
  const u64 required=value.state==TimestampValueStateV3::value?16:0;
  if(required>control.maximum_allocation_bytes)return Failure<TimestampBytesResultV3>("RESOURCE.BUDGET_EXCEEDED","component_budget",ResourceStatus());
  if(Cancelled(control))return Failure<TimestampBytesResultV3>("PROCESS.CANCELLED","before_allocation");
  auto r=Success<TimestampBytesResultV3>();try{r.bytes.resize(required);}catch(...){return Failure<TimestampBytesResultV3>("RESOURCE.BUDGET_EXCEEDED","component_allocation",ResourceStatus());}
  ScopedVectorClear clear(&r.bytes,TimestampScrubClassV3::component_owned_buffer,control.observe_scrubbed,control.scrub_observer_context);
  if(required)EncodeComponent(value.civil_day,value.nanoseconds_since_midnight,r.bytes.data());
  if(Cancelled(control))return Failure<TimestampBytesResultV3>("PROCESS.CANCELLED","before_publication");
  clear.Disarm();(void)profile_pin;return r;
}

TimestampValueResultV3 ConstructTimestampFromCivilV3(
    const std::shared_ptr<const TimestampValidatedProfileHandleV3>& profile,
    std::int64_t year,u64 month,u64 day,u64 hour,u64 minute,u64 second,u64 nanosecond,
    bool null_allowed,const TimestampExecutionControlV3& control) noexcept {
  return ConstructTimestampFromCivilV3(profile,
      {TimestampI64CarrierKindV3::signed_i64,TimestampValueStateV3::value,year},
      {TimestampUnsignedCarrierKindV3::unsigned_u64,TimestampValueStateV3::value,month},
      {TimestampUnsignedCarrierKindV3::unsigned_u64,TimestampValueStateV3::value,day},
      {TimestampUnsignedCarrierKindV3::unsigned_u64,TimestampValueStateV3::value,hour},
      {TimestampUnsignedCarrierKindV3::unsigned_u64,TimestampValueStateV3::value,minute},
      {TimestampUnsignedCarrierKindV3::unsigned_u64,TimestampValueStateV3::value,second},
      {TimestampUnsignedCarrierKindV3::unsigned_u64,TimestampValueStateV3::value,nanosecond},null_allowed,control);
}
TimestampValueResultV3 ConstructTimestampFromCivilV3(
    const std::shared_ptr<const TimestampValidatedProfileHandleV3>& profile,
    const TimestampNullableI64FactV3& year,const TimestampNullableUnsignedFactV3& month,
    const TimestampNullableUnsignedFactV3& day,const TimestampNullableUnsignedFactV3& hour,
    const TimestampNullableUnsignedFactV3& minute,const TimestampNullableUnsignedFactV3& second,
    const TimestampNullableUnsignedFactV3& nanosecond,bool null_allowed,
    const TimestampExecutionControlV3& control) noexcept {
  if(year.carrier!=TimestampI64CarrierKindV3::signed_i64)return Failure<TimestampValueResultV3>("SBLR.OPERAND_INVALID","year_carrier");
  const TimestampNullableUnsignedFactV3* facts[]{&month,&day,&hour,&minute,&second,&nanosecond};
  for(auto* fact:facts)if(fact->carrier!=TimestampUnsignedCarrierKindV3::unsigned_u64)return Failure<TimestampValueResultV3>("SBLR.OPERAND_INVALID","civil_carrier");
  if(!profile||!ProfileValidNoAlloc(*profile))return Failure<TimestampValueResultV3>("CTI.TEMPORAL.DESCRIPTOR_INVALID","profile_invalid");
  if(!ValidState(year.state)||(year.state==TimestampValueStateV3::sql_null&&year.value!=0))return Failure<TimestampValueResultV3>("DATATYPE.NULL_STATE.INVALID","year_state");
  bool any_null=year.state==TimestampValueStateV3::sql_null;
  for(auto* fact:facts){if(!ValidState(fact->state)||(fact->state==TimestampValueStateV3::sql_null&&fact->value!=0))return Failure<TimestampValueResultV3>("DATATYPE.NULL_STATE.INVALID","civil_state");any_null|=fact->state==TimestampValueStateV3::sql_null;}
  if(any_null){if(!null_allowed)return Failure<TimestampValueResultV3>("DATATYPE.NULL_NOT_ADMITTED","civil_null");return PublishValue(Owned(profile,TimestampValueStateV3::sql_null,0,0),control);}
  if(month.value==0||day.value==0)return Failure<TimestampValueResultV3>("CTI.TEMPORAL.ZERO_DATE_REFUSED","civil_date_zero");
  if(month.value>12||day.value>31)return Failure<TimestampValueResultV3>("CTI.TEMPORAL.INVALID_LITERAL","civil_date_field");
  if(day.value>DaysInMonth(year.value,static_cast<u8>(month.value)))return Failure<TimestampValueResultV3>("CTI.TEMPORAL.INVALID_LITERAL","civil_date_invalid");
  std::int64_t day_count=0;if(!CivilToDays(year.value,static_cast<u8>(month.value),static_cast<u8>(day.value),&day_count))return Failure<TimestampValueResultV3>("CTI.TEMPORAL.RANGE_EXCEEDED","civil_date_carrier_range");
  if(day_count<std::numeric_limits<std::int32_t>::min()||day_count>std::numeric_limits<std::int32_t>::max())return Failure<TimestampValueResultV3>("CTI.TEMPORAL.RANGE_EXCEEDED","civil_date_range");
  if(hour.value>23||minute.value>59||second.value>60||nanosecond.value>999'999'999)return Failure<TimestampValueResultV3>("CTI.TEMPORAL.INVALID_LITERAL","civil_time_field");
  if(second.value==60)return Failure<TimestampValueResultV3>("CTI.TEMPORAL.LEAP_SECOND_REFUSED","second");
  const u64 nanos=hour.value*3'600'000'000'000ull+minute.value*60'000'000'000ull+second.value*1'000'000'000ull+nanosecond.value;
  return PublishValue(Owned(profile,TimestampValueStateV3::value,static_cast<std::int32_t>(day_count),nanos),control);
}
TimestampCivilResultV3 DecomposeTimestampCivilV3(const TimestampValueViewV3& value,
    bool null_allowed,const TimestampExecutionControlV3& control) noexcept {
  const auto checked=ValidateTimestampValueViewV3(value,null_allowed);if(!checked.ok())return Failure<TimestampCivilResultV3>(checked.diagnostic.diagnostic_code,checked.diagnostic.detail,checked.status);
  auto r=Success<TimestampCivilResultV3>();if(value.state==TimestampValueStateV3::sql_null){if(Cancelled(control))return Failure<TimestampCivilResultV3>("PROCESS.CANCELLED","before_publication");r.is_null=true;return r;}
  r.civil=DaysToCivil(value.civil_day);u64 rem=value.nanoseconds_since_midnight;r.civil.hour=static_cast<u8>(rem/3'600'000'000'000ull);rem%=3'600'000'000'000ull;r.civil.minute=static_cast<u8>(rem/60'000'000'000ull);rem%=60'000'000'000ull;r.civil.second=static_cast<u8>(rem/1'000'000'000ull);r.civil.nanosecond=static_cast<u32>(rem%1'000'000'000ull);
  if(Cancelled(control))return Failure<TimestampCivilResultV3>("PROCESS.CANCELLED","before_publication");
  return r;
}
TimestampValueResultV3 ParseCanonicalTimestampV3(
    const std::shared_ptr<const TimestampValidatedProfileHandleV3>& profile,std::string_view text,
    bool,const TimestampExecutionControlV3& control) noexcept {
  if(!profile||!ProfileValidNoAlloc(*profile))return Failure<TimestampValueResultV3>("CTI.TEMPORAL.DESCRIPTOR_INVALID","profile_invalid");
  const auto parsed=ParseStrict(text);if(!parsed.ok)return Failure<TimestampValueResultV3>(parsed.code,parsed.detail);
  return PublishValue(Owned(profile,TimestampValueStateV3::value,parsed.civil_day,parsed.nanoseconds_since_midnight),control);
}
TimestampValueResultV3 ParseCanonicalTimestampOperandV3(
    const std::shared_ptr<const TimestampValidatedProfileHandleV3>& profile,
    const TimestampTextOperandV3& operand,bool null_allowed,
    const TimestampExecutionControlV3& control) noexcept {
  if(operand.carrier!=TimestampTextCarrierKindV3::utf8_bytes)return Failure<TimestampValueResultV3>("SBLR.OPERAND_INVALID","text_carrier");
  if(!profile||!ProfileValidNoAlloc(*profile))return Failure<TimestampValueResultV3>("CTI.TEMPORAL.DESCRIPTOR_INVALID","profile_invalid");
  if(!CharacterIdentity(operand.identity)||!CharacterDescriptor(operand.descriptor,operand.identity))return Failure<TimestampValueResultV3>("CTI.TEMPORAL.DESCRIPTOR_INVALID","text_authority");
  if(!ValidState(operand.state)||operand.extent!=operand.bytes.size())return Failure<TimestampValueResultV3>("DATATYPE.NULL_STATE.INVALID","text_envelope");
  if(operand.state==TimestampValueStateV3::sql_null){if(!operand.bytes.empty())return Failure<TimestampValueResultV3>("DATATYPE.NULL_STATE.INVALID","dirty_null_text");if(!null_allowed||!operand.descriptor->nullable_allowed)return Failure<TimestampValueResultV3>("DATATYPE.NULL_NOT_ADMITTED","text_null");return PublishValue(Owned(profile,TimestampValueStateV3::sql_null,0,0),control);}
  if(operand.descriptor->length&&operand.extent>operand.descriptor->length)return Failure<TimestampValueResultV3>("CTB.TEXT.LENGTH_EXCEEDED","text_length");
  return ParseCanonicalTimestampV3(profile,operand.bytes,null_allowed,control);
}

TimestampNoAllocWriteResultV3 RenderCanonicalTimestampIntoNoAllocV3(
    const TimestampOwnedValueV3& value,bool export_literal,char* output,u64 capacity,
    const TimestampExecutionControlV3& control) noexcept {
  const auto checked=ValidateTimestampValueViewV3(value.view(),true);if(!checked.ok())return Failure<TimestampNoAllocWriteResultV3>(checked.diagnostic.diagnostic_code,checked.diagnostic.detail,checked.status);
  std::array<char,45> staged{};ScopedClear clear(staged.data(),staged.size(),TimestampScrubClassV3::render_staging,control.observe_scrubbed,control.scrub_observer_context);
  std::size_t plain=0,offset=0;if(value.state==TimestampValueStateV3::value){if(export_literal){std::memcpy(staged.data(),"TIMESTAMP '",11);offset=11;}plain=RenderStrict(value.civil_day,value.nanoseconds_since_midnight,staged.data()+offset);if(plain==0)return Failure<TimestampNoAllocWriteResultV3>("CTI.TEMPORAL.CANONICAL_ENCODING_INVALID","render_internal");offset+=plain;if(export_literal)staged[offset++]='\'';}
  const u64 required=offset;if(output&&(RangesOverlap(output,required,&value,sizeof(value))||RangesOverlap(output,required,&control,sizeof(control))||OutputOverlapsProfile(output,required,*value.profile)))return Failure<TimestampNoAllocWriteResultV3>("CTI.TEMPORAL.CANONICAL_ENCODING_INVALID","render_overlap");
  if(required>capacity||(required&&output==nullptr)){auto r=Failure<TimestampNoAllocWriteResultV3>("CTB.TEXT.LENGTH_EXCEEDED","render_capacity");r.bytes_required=required;return r;}if(required>control.maximum_allocation_bytes){auto r=Failure<TimestampNoAllocWriteResultV3>("RESOURCE.BUDGET_EXCEEDED","render_budget",ResourceStatus());r.bytes_required=required;return r;}if(Cancelled(control))return Failure<TimestampNoAllocWriteResultV3>("PROCESS.CANCELLED","before_publication");if(required)std::memcpy(output,staged.data(),required);auto r=Success<TimestampNoAllocWriteResultV3>();r.bytes_required=required;r.bytes_written=required;r.containing_null=value.state==TimestampValueStateV3::sql_null;return r;
}
TimestampTextResultV3 RenderCanonicalTimestampV3(const TimestampOwnedValueV3& value,bool export_literal,
    const TimestampExecutionControlV3& control) noexcept {
  auto profile_pin=value.profile;std::array<char,45> buffer{};ScopedClear clear(buffer.data(),buffer.size(),TimestampScrubClassV3::render_owned_buffer,control.observe_scrubbed,control.scrub_observer_context);auto written=RenderCanonicalTimestampIntoNoAllocV3(value,export_literal,buffer.data(),buffer.size(),control);if(!written.ok())return Failure<TimestampTextResultV3>(written.diagnostic.diagnostic_code,written.diagnostic.detail,written.status);auto r=Success<TimestampTextResultV3>();r.containing_null=written.containing_null;try{r.text.assign(buffer.data(),written.bytes_written);}catch(...){return Failure<TimestampTextResultV3>("RESOURCE.BUDGET_EXCEEDED","render_allocation",ResourceStatus());}if(Cancelled(control)){if(!r.text.empty())SecureClear(r.text.data(),r.text.size());return Failure<TimestampTextResultV3>("PROCESS.CANCELLED","before_publication");}(void)profile_pin;return r;
}
TimestampValueResultV3 ValidateCanonicalTimestampV3(const TimestampOwnedValueV3& value,bool null_allowed,const TimestampExecutionControlV3& control) noexcept {const auto checked=ValidateTimestampValueViewV3(value.view(),null_allowed);if(!checked.ok())return Failure<TimestampValueResultV3>(checked.diagnostic.diagnostic_code,checked.diagnostic.detail,checked.status);return PublishValue(value,control);}
TimestampValueResultV3 TruncateTimestampNanosecondV3(const TimestampOwnedValueV3& value,bool null_allowed,const TimestampExecutionControlV3& control) noexcept{return ValidateCanonicalTimestampV3(value,null_allowed,control);}
TimestampValueResultV3 RoundTimestampNanosecondV3(const TimestampOwnedValueV3& value,bool null_allowed,const TimestampExecutionControlV3& control) noexcept{return ValidateCanonicalTimestampV3(value,null_allowed,control);}

TimestampIntrinsicDispositionV3 ClassifyTimestampIntrinsicOperationV3(TimestampIntrinsicOperationV3 operation) noexcept {
  const auto raw=static_cast<unsigned>(operation);if(raw<=static_cast<unsigned>(TimestampIntrinsicOperationV3::round_nanosecond_identity))return TimestampIntrinsicDispositionV3::admitted;if(raw<=static_cast<unsigned>(TimestampIntrinsicOperationV3::bucketing))return TimestampIntrinsicDispositionV3::registered_refused;return TimestampIntrinsicDispositionV3::unknown;
}
TimestampValueResultV3 RefuseTimestampIntrinsicOperationV3(const TimestampValueViewV3& operand,TimestampIntrinsicOperationV3 operation) noexcept {
  if(operand.profile==nullptr||!ProfileValidNoAlloc(*operand.profile))
    return Failure<TimestampValueResultV3>("CTI.TEMPORAL.DESCRIPTOR_INVALID","intrinsic_profile");
  if(operand.state!=TimestampValueStateV3::value&&
     operand.state!=TimestampValueStateV3::sql_null)
    return Failure<TimestampValueResultV3>("DATATYPE.NULL_STATE.INVALID","intrinsic_state");
  if(operand.state==TimestampValueStateV3::sql_null&&
     (operand.civil_day!=0||operand.nanoseconds_since_midnight!=0))
    return Failure<TimestampValueResultV3>("DATATYPE.NULL_STATE.INVALID","intrinsic_dirty_null");
  const auto disposition=ClassifyTimestampIntrinsicOperationV3(operation);
  if(disposition==TimestampIntrinsicDispositionV3::unknown)
    return Failure<TimestampValueResultV3>("CTI.TEMPORAL.OPERATION_REFUSED","unknown_operation");
  if(disposition!=TimestampIntrinsicDispositionV3::registered_refused)
    return Failure<TimestampValueResultV3>("CTI.TEMPORAL.OPERATION_REFUSED","operation_not_refusal");
  if(operation==TimestampIntrinsicOperationV3::calendar_interval_arithmetic||
     operation==TimestampIntrinsicOperationV3::bucketing)
    return Failure<TimestampValueResultV3>("CTI.INTERVAL.CALENDAR_OPERATION_REFUSED","registered_calendar_refusal");
  return Failure<TimestampValueResultV3>("CTI.TEMPORAL.OPERATION_REFUSED","registered_refusal");
}

namespace {
TimestampViewResultV3 ValidateTimestampEnvelopeNoPayload(
    const TimestampValueViewV3& value,bool null_allowed) noexcept {
  if(value.profile==nullptr||!ProfileValidNoAlloc(*value.profile))
    return Failure<TimestampViewResultV3>("CTI.TEMPORAL.DESCRIPTOR_INVALID","profile_invalid");
  if(!ValidState(value.state))
    return Failure<TimestampViewResultV3>("DATATYPE.NULL_STATE.INVALID","state_unknown");
  if(value.state==TimestampValueStateV3::sql_null){
    if(value.civil_day!=0||value.nanoseconds_since_midnight!=0)
      return Failure<TimestampViewResultV3>("DATATYPE.NULL_STATE.INVALID","dirty_null_scalar");
    if(!null_allowed)
      return Failure<TimestampViewResultV3>("DATATYPE.NULL_NOT_ADMITTED","null_not_allowed");
  }
  auto result=Success<TimestampViewResultV3>();result.value=value;return result;
}
TimestampViewResultV3 ValidateTimestampProfileOnly(
    const TimestampValueViewV3& value) noexcept {
  if(value.profile==nullptr||!ProfileValidNoAlloc(*value.profile))
    return Failure<TimestampViewResultV3>("CTI.TEMPORAL.DESCRIPTOR_INVALID","profile_invalid");
  auto result=Success<TimestampViewResultV3>();result.value=value;return result;
}
TimestampViewResultV3 ValidateTimestampStateNoPayload(
    const TimestampValueViewV3& value,bool null_allowed) noexcept {
  if(!ValidState(value.state))
    return Failure<TimestampViewResultV3>("DATATYPE.NULL_STATE.INVALID","state_unknown");
  if(value.state==TimestampValueStateV3::sql_null){
    if(value.civil_day!=0||value.nanoseconds_since_midnight!=0)
      return Failure<TimestampViewResultV3>("DATATYPE.NULL_STATE.INVALID","dirty_null_scalar");
    if(!null_allowed)
      return Failure<TimestampViewResultV3>("DATATYPE.NULL_NOT_ADMITTED","null_not_allowed");
  }
  auto result=Success<TimestampViewResultV3>();result.value=value;return result;
}
TimestampValueResultV3 AddWide(const TimestampOwnedValueV3& value,TimestampWideSigned delta,bool null_allowed,const TimestampExecutionControlV3& control) noexcept {
  const auto checked=ValidateTimestampValueViewV3(value.view(),null_allowed);if(!checked.ok())return Failure<TimestampValueResultV3>(checked.diagnostic.diagnostic_code,checked.diagnostic.detail,checked.status);if(value.state==TimestampValueStateV3::sql_null)return PublishValue(value,control);
  constexpr TimestampWideSigned day_ns=static_cast<TimestampWideSigned>(86'400'000'000'000ull);
  const TimestampWideSigned current=static_cast<TimestampWideSigned>(value.civil_day)*day_ns+value.nanoseconds_since_midnight;
  const TimestampWideSigned next=current+delta;TimestampWideSigned day=next/day_ns;TimestampWideSigned remainder=next%day_ns;if(remainder<0){remainder+=day_ns;--day;}
  if(day<std::numeric_limits<std::int32_t>::min()||day>std::numeric_limits<std::int32_t>::max())return Failure<TimestampValueResultV3>("CTI.TEMPORAL.RANGE_EXCEEDED","arithmetic_range");
  return PublishValue(Owned(value.profile,TimestampValueStateV3::value,static_cast<std::int32_t>(day),static_cast<u64>(remainder)),control);
}
TimestampScalarResultV3 ExtractTimePart(const TimestampValueViewV3& value,u64 divisor,u64 modulo,bool null_allowed,const TimestampExecutionControlV3& control) noexcept {bool ready=false;auto r=ScalarNullOrReady(value,null_allowed,control,&ready);if(!r.ok()||!ready)return r;r.unsigned_value=(value.nanoseconds_since_midnight/divisor)%modulo;r.signed_value=static_cast<std::int64_t>(r.unsigned_value);if(Cancelled(control))return Failure<TimestampScalarResultV3>("PROCESS.CANCELLED","before_publication");return r;}
TimestampScalarResultV3 ExtractCivilPart(const TimestampValueViewV3& value,int part,bool null_allowed,const TimestampExecutionControlV3& control) noexcept {bool ready=false;auto r=ScalarNullOrReady(value,null_allowed,control,&ready);if(!r.ok()||!ready)return r;const auto c=DaysToCivil(value.civil_day);r.signed_value=part==0?c.year:part==1?c.month:c.day;r.unsigned_value=static_cast<u64>(r.signed_value);if(Cancelled(control))return Failure<TimestampScalarResultV3>("PROCESS.CANCELLED","before_publication");return r;}
}
TimestampValueResultV3 AddTimestampNanosecondsV3(const TimestampOwnedValueV3& value,std::int64_t delta,bool null_allowed,const TimestampExecutionControlV3& control) noexcept{return AddTimestampNanosecondsV3(value,{TimestampI64CarrierKindV3::signed_i64,TimestampValueStateV3::value,delta},null_allowed,control);}
TimestampValueResultV3 AddTimestampNanosecondsV3(const TimestampOwnedValueV3& value,const TimestampNullableI64FactV3& delta,bool null_allowed,const TimestampExecutionControlV3& control) noexcept {if(delta.carrier!=TimestampI64CarrierKindV3::signed_i64)return Failure<TimestampValueResultV3>("SBLR.OPERAND_INVALID","delta_carrier");const auto a=ValidateTimestampEnvelopeNoPayload(value.view(),null_allowed);if(!a.ok())return Failure<TimestampValueResultV3>(a.diagnostic.diagnostic_code,a.diagnostic.detail,a.status);if(!ValidState(delta.state)||(delta.state==TimestampValueStateV3::sql_null&&delta.value!=0))return Failure<TimestampValueResultV3>("DATATYPE.NULL_STATE.INVALID","delta_state");if(value.state==TimestampValueStateV3::sql_null||delta.state==TimestampValueStateV3::sql_null){if(!null_allowed)return Failure<TimestampValueResultV3>("DATATYPE.NULL_NOT_ADMITTED","delta_null");return PublishValue(Owned(value.profile,TimestampValueStateV3::sql_null,0,0),control);}const auto present=ValidateTimestampValueViewV3(value.view(),false);if(!present.ok())return Failure<TimestampValueResultV3>(present.diagnostic.diagnostic_code,present.diagnostic.detail,present.status);return AddWide(value,static_cast<TimestampWideSigned>(delta.value),false,control);}
TimestampValueResultV3 SubtractTimestampNanosecondsV3(const TimestampOwnedValueV3& value,std::int64_t delta,bool null_allowed,const TimestampExecutionControlV3& control) noexcept{return SubtractTimestampNanosecondsV3(value,{TimestampI64CarrierKindV3::signed_i64,TimestampValueStateV3::value,delta},null_allowed,control);}
TimestampValueResultV3 SubtractTimestampNanosecondsV3(const TimestampOwnedValueV3& value,const TimestampNullableI64FactV3& delta,bool null_allowed,const TimestampExecutionControlV3& control) noexcept {if(delta.carrier!=TimestampI64CarrierKindV3::signed_i64)return Failure<TimestampValueResultV3>("SBLR.OPERAND_INVALID","delta_carrier");const auto a=ValidateTimestampEnvelopeNoPayload(value.view(),null_allowed);if(!a.ok())return Failure<TimestampValueResultV3>(a.diagnostic.diagnostic_code,a.diagnostic.detail,a.status);if(!ValidState(delta.state)||(delta.state==TimestampValueStateV3::sql_null&&delta.value!=0))return Failure<TimestampValueResultV3>("DATATYPE.NULL_STATE.INVALID","delta_state");if(value.state==TimestampValueStateV3::sql_null||delta.state==TimestampValueStateV3::sql_null){if(!null_allowed)return Failure<TimestampValueResultV3>("DATATYPE.NULL_NOT_ADMITTED","delta_null");return PublishValue(Owned(value.profile,TimestampValueStateV3::sql_null,0,0),control);}const auto present=ValidateTimestampValueViewV3(value.view(),false);if(!present.ok())return Failure<TimestampValueResultV3>(present.diagnostic.diagnostic_code,present.diagnostic.detail,present.status);return AddWide(value,-static_cast<TimestampWideSigned>(delta.value),false,control);}
TimestampValueResultV3 TimestampSuccessorV3(const TimestampOwnedValueV3& value,bool null_allowed,const TimestampExecutionControlV3& control) noexcept{return AddWide(value,1,null_allowed,control);}
TimestampValueResultV3 TimestampPredecessorV3(const TimestampOwnedValueV3& value,bool null_allowed,const TimestampExecutionControlV3& control) noexcept{return AddWide(value,-1,null_allowed,control);}
TimestampDifferenceResultV3 DifferenceTimestampV3(const TimestampValueViewV3& left,const TimestampValueViewV3& right,bool null_allowed,const TimestampExecutionControlV3& control) noexcept {const auto lp0=ValidateTimestampProfileOnly(left);if(!lp0.ok())return Failure<TimestampDifferenceResultV3>(lp0.diagnostic.diagnostic_code,lp0.diagnostic.detail,lp0.status);const auto rp0=ValidateTimestampProfileOnly(right);if(!rp0.ok())return Failure<TimestampDifferenceResultV3>(rp0.diagnostic.diagnostic_code,rp0.diagnostic.detail,rp0.status);const auto l=ValidateTimestampStateNoPayload(left,null_allowed);if(!l.ok())return Failure<TimestampDifferenceResultV3>(l.diagnostic.diagnostic_code,l.diagnostic.detail,l.status);const auto rcheck=ValidateTimestampStateNoPayload(right,null_allowed);if(!rcheck.ok())return Failure<TimestampDifferenceResultV3>(rcheck.diagnostic.diagnostic_code,rcheck.diagnostic.detail,rcheck.status);if(left.profile->comparison_fingerprint!=right.profile->comparison_fingerprint)return Failure<TimestampDifferenceResultV3>("CTI.TEMPORAL.ORDERING_REFUSED","comparison_cohort");auto r=Success<TimestampDifferenceResultV3>();if(left.state==TimestampValueStateV3::sql_null||right.state==TimestampValueStateV3::sql_null){r.is_null=true;if(Cancelled(control))return Failure<TimestampDifferenceResultV3>("PROCESS.CANCELLED","before_publication");return r;}const auto lp=ValidateTimestampValueViewV3(left,false);if(!lp.ok())return Failure<TimestampDifferenceResultV3>(lp.diagnostic.diagnostic_code,lp.diagnostic.detail,lp.status);const auto rp=ValidateTimestampValueViewV3(right,false);if(!rp.ok())return Failure<TimestampDifferenceResultV3>(rp.diagnostic.diagnostic_code,rp.diagnostic.detail,rp.status);constexpr TimestampWideSigned day_ns=static_cast<TimestampWideSigned>(86'400'000'000'000ull);const TimestampWideSigned a=static_cast<TimestampWideSigned>(left.civil_day)*day_ns+left.nanoseconds_since_midnight;const TimestampWideSigned b=static_cast<TimestampWideSigned>(right.civil_day)*day_ns+right.nanoseconds_since_midnight;TimestampWideSigned d=a-b;r.difference.negative=d<0;if(d<0)d=-d;r.difference.magnitude_whole_days=static_cast<u64>(d/day_ns);r.difference.magnitude_nanoseconds_remainder=static_cast<u64>(d%day_ns);if(d==0)r.difference.negative=false;if(Cancelled(control))return Failure<TimestampDifferenceResultV3>("PROCESS.CANCELLED","before_publication");return r;}

TimestampScalarResultV3 ExtractTimestampYearV3(const TimestampValueViewV3& v,bool n,const TimestampExecutionControlV3& c) noexcept{return ExtractCivilPart(v,0,n,c);}TimestampScalarResultV3 ExtractTimestampMonthV3(const TimestampValueViewV3& v,bool n,const TimestampExecutionControlV3& c) noexcept{return ExtractCivilPart(v,1,n,c);}TimestampScalarResultV3 ExtractTimestampDayV3(const TimestampValueViewV3& v,bool n,const TimestampExecutionControlV3& c) noexcept{return ExtractCivilPart(v,2,n,c);}TimestampScalarResultV3 ExtractTimestampHourV3(const TimestampValueViewV3& v,bool n,const TimestampExecutionControlV3& c) noexcept{return ExtractTimePart(v,3'600'000'000'000ull,24,n,c);}TimestampScalarResultV3 ExtractTimestampMinuteV3(const TimestampValueViewV3& v,bool n,const TimestampExecutionControlV3& c) noexcept{return ExtractTimePart(v,60'000'000'000ull,60,n,c);}TimestampScalarResultV3 ExtractTimestampSecondV3(const TimestampValueViewV3& v,bool n,const TimestampExecutionControlV3& c) noexcept{return ExtractTimePart(v,1'000'000'000ull,60,n,c);}TimestampScalarResultV3 ExtractTimestampNanosecondV3(const TimestampValueViewV3& v,bool n,const TimestampExecutionControlV3& c) noexcept{return ExtractTimePart(v,1,1'000'000'000ull,n,c);}TimestampScalarResultV3 TimestampNanosecondOfDayV3(const TimestampValueViewV3& v,bool n,const TimestampExecutionControlV3& c) noexcept{return ExtractTimePart(v,1,kTimestampMaximumNanosecondsV3+1,n,c);}
TimestampScalarResultV3 TimestampDayCountV3(const TimestampValueViewV3& v,bool n,const TimestampExecutionControlV3& c) noexcept {bool ready=false;auto r=ScalarNullOrReady(v,n,c,&ready);if(!r.ok()||!ready)return r;r.signed_value=v.civil_day;if(Cancelled(c))return Failure<TimestampScalarResultV3>("PROCESS.CANCELLED","before_publication");return r;}
TimestampScalarResultV3 TimestampIsLeapYearV3(const TimestampValueViewV3& v,bool n,const TimestampExecutionControlV3& c) noexcept {bool ready=false;auto r=ScalarNullOrReady(v,n,c,&ready);if(!r.ok()||!ready)return r;r.boolean_value=Leap(DaysToCivil(v.civil_day).year);r.unsigned_value=r.boolean_value;r.signed_value=r.boolean_value;if(Cancelled(c))return Failure<TimestampScalarResultV3>("PROCESS.CANCELLED","before_publication");return r;}
TimestampScalarResultV3 TimestampDaysInMonthV3(const TimestampValueViewV3& v,bool n,const TimestampExecutionControlV3& c) noexcept {bool ready=false;auto r=ScalarNullOrReady(v,n,c,&ready);if(!r.ok()||!ready)return r;const auto x=DaysToCivil(v.civil_day);r.unsigned_value=DaysInMonth(x.year,x.month);r.signed_value=r.unsigned_value;if(Cancelled(c))return Failure<TimestampScalarResultV3>("PROCESS.CANCELLED","before_publication");return r;}
TimestampScalarResultV3 TimestampIsoWeekdayV3(const TimestampValueViewV3& v,bool n,const TimestampExecutionControlV3& c) noexcept {bool ready=false;auto r=ScalarNullOrReady(v,n,c,&ready);if(!r.ok()||!ready)return r;r.unsigned_value=static_cast<u64>(FloorMod(static_cast<std::int64_t>(v.civil_day)+3,7)+1);r.signed_value=r.unsigned_value;if(Cancelled(c))return Failure<TimestampScalarResultV3>("PROCESS.CANCELLED","before_publication");return r;}
TimestampScalarResultV3 TimestampDayOfYearV3(const TimestampValueViewV3& v,bool n,const TimestampExecutionControlV3& c) noexcept {bool ready=false;auto r=ScalarNullOrReady(v,n,c,&ready);if(!r.ok()||!ready)return r;const auto x=DaysToCivil(v.civil_day);std::int64_t first=0;CivilToDays(x.year,1,1,&first);r.unsigned_value=static_cast<u64>(static_cast<std::int64_t>(v.civil_day)-first+1);r.signed_value=r.unsigned_value;if(Cancelled(c))return Failure<TimestampScalarResultV3>("PROCESS.CANCELLED","before_publication");return r;}
TimestampScalarResultV3 TimestampQuarterV3(const TimestampValueViewV3& v,bool n,const TimestampExecutionControlV3& c) noexcept {bool ready=false;auto r=ScalarNullOrReady(v,n,c,&ready);if(!r.ok()||!ready)return r;r.unsigned_value=(DaysToCivil(v.civil_day).month-1)/3+1;r.signed_value=r.unsigned_value;if(Cancelled(c))return Failure<TimestampScalarResultV3>("PROCESS.CANCELLED","before_publication");return r;}
TimestampIsoWeekResultV3 TimestampIsoWeekV3(const TimestampValueViewV3& v,bool n,const TimestampExecutionControlV3& c) noexcept {const auto checked=ValidateTimestampValueViewV3(v,n);if(!checked.ok())return Failure<TimestampIsoWeekResultV3>(checked.diagnostic.diagnostic_code,checked.diagnostic.detail,checked.status);auto r=Success<TimestampIsoWeekResultV3>();if(v.state==TimestampValueStateV3::sql_null){r.is_null=true;if(Cancelled(c))return Failure<TimestampIsoWeekResultV3>("PROCESS.CANCELLED","before_publication");return r;}const std::int64_t weekday=FloorMod(static_cast<std::int64_t>(v.civil_day)+3,7)+1;const std::int64_t thursday=static_cast<std::int64_t>(v.civil_day)+(4-weekday);const auto tc=DaysToCivil(thursday);std::int64_t jan4=0;CivilToDays(tc.year,1,4,&jan4);const std::int64_t jan4wd=FloorMod(jan4+3,7)+1;const std::int64_t week1=jan4-(jan4wd-1);r.value.iso_year=tc.year;r.value.iso_week=static_cast<u8>((thursday-week1)/7+1);if(Cancelled(c))return Failure<TimestampIsoWeekResultV3>("PROCESS.CANCELLED","before_publication");return r;}

namespace {TimestampComparisonResultV3 CompareResolved(const TimestampValueViewV3& left,const TimestampValueViewV3& right,const std::array<byte,32>& fingerprint,const TimestampExecutionControlV3& control) noexcept {const auto lp0=ValidateTimestampProfileOnly(left);if(!lp0.ok())return Failure<TimestampComparisonResultV3>(lp0.diagnostic.diagnostic_code,lp0.diagnostic.detail,lp0.status);const auto rp0=ValidateTimestampProfileOnly(right);if(!rp0.ok())return Failure<TimestampComparisonResultV3>(rp0.diagnostic.diagnostic_code,rp0.diagnostic.detail,rp0.status);const auto l=ValidateTimestampStateNoPayload(left,true);if(!l.ok())return Failure<TimestampComparisonResultV3>(l.diagnostic.diagnostic_code,l.diagnostic.detail,l.status);const auto rcheck=ValidateTimestampStateNoPayload(right,true);if(!rcheck.ok())return Failure<TimestampComparisonResultV3>(rcheck.diagnostic.diagnostic_code,rcheck.diagnostic.detail,rcheck.status);if(left.profile->comparison_fingerprint!=right.profile->comparison_fingerprint||left.profile->comparison_fingerprint!=fingerprint)return Failure<TimestampComparisonResultV3>("CTI.TEMPORAL.ORDERING_REFUSED","comparison_cohort");auto r=Success<TimestampComparisonResultV3>();if(left.state==TimestampValueStateV3::sql_null||right.state==TimestampValueStateV3::sql_null){r.fact=TimestampComparisonFactV3::unordered_null;r.grouping_equivalent=left.state==right.state;r.null_equivalent=r.grouping_equivalent;if(Cancelled(control))return Failure<TimestampComparisonResultV3>("PROCESS.CANCELLED","before_publication");return r;}const auto lp=ValidateTimestampValueViewV3(left,false);if(!lp.ok())return Failure<TimestampComparisonResultV3>(lp.diagnostic.diagnostic_code,lp.diagnostic.detail,lp.status);const auto rp=ValidateTimestampValueViewV3(right,false);if(!rp.ok())return Failure<TimestampComparisonResultV3>(rp.diagnostic.diagnostic_code,rp.diagnostic.detail,rp.status);if(left.civil_day<right.civil_day||(left.civil_day==right.civil_day&&left.nanoseconds_since_midnight<right.nanoseconds_since_midnight))r.fact=TimestampComparisonFactV3::less;else if(left.civil_day>right.civil_day||(left.civil_day==right.civil_day&&left.nanoseconds_since_midnight>right.nanoseconds_since_midnight))r.fact=TimestampComparisonFactV3::greater;else{r.fact=TimestampComparisonFactV3::equal;r.grouping_equivalent=true;}if(Cancelled(control))return Failure<TimestampComparisonResultV3>("PROCESS.CANCELLED","before_publication");return r;}}
TimestampComparisonResultV3 CompareTimestampValuesV3(const TimestampValueViewV3& l,const TimestampValueViewV3& r,const TimestampExecutionControlV3& c) noexcept {return CompareResolved(l,r,l.profile == nullptr ? kComparisonFingerprint : l.profile->comparison_fingerprint,c);}TimestampComparisonResultV3 CompareTimestampValuesWithValidatedCohortForConformanceV3(const TimestampValueViewV3& l,const TimestampValueViewV3& r,const std::array<byte,32>& f,const TimestampExecutionControlV3& c) noexcept{return CompareResolved(l,r,f,c);}

TimestampNoAllocWriteResultV3 HashTimestampValueIntoNoAllocV3(const TimestampOwnedValueV3& value,byte* output,u64 capacity,const TimestampExecutionControlV3& control) noexcept {const auto checked=ValidateTimestampValueViewV3(value.view(),true);if(!checked.ok())return Failure<TimestampNoAllocWriteResultV3>(checked.diagnostic.diagnostic_code,checked.diagnostic.detail,checked.status);std::array<byte,117> preimage{};ScopedClear clear(preimage.data(),preimage.size(),TimestampScrubClassV3::hash_preimage,control.observe_scrubbed,control.scrub_observer_context);std::array<byte,32> digest{};ScopedClear clear_digest(digest.data(),digest.size(),TimestampScrubClassV3::hash_digest,control.observe_scrubbed,control.scrub_observer_context);if(output&&(RangesOverlap(output,32,&value,sizeof(value))||RangesOverlap(output,32,&control,sizeof(control))||OutputOverlapsProfile(output,32,*value.profile)))return Failure<TimestampNoAllocWriteResultV3>("CTI.TEMPORAL.CANONICAL_ENCODING_INVALID","hash_overlap");if(capacity<32||!output){auto r=Failure<TimestampNoAllocWriteResultV3>("RESOURCE.BUDGET_EXCEEDED","hash_capacity",ResourceStatus());r.bytes_required=32;return r;}if(control.maximum_allocation_bytes<32){auto r=Failure<TimestampNoAllocWriteResultV3>("RESOURCE.BUDGET_EXCEEDED","hash_budget",ResourceStatus());r.bytes_required=32;return r;}std::memcpy(preimage.data(),"SBTSPH01",8);PutUuid(preimage.data()+8,value.profile->receipt.catalog_snapshot_uuid);StoreLittle64(preimage.data()+24,value.profile->receipt.catalog_generation);StoreLittle64(preimage.data()+32,value.profile->receipt.registry_generation);std::memcpy(preimage.data()+40,value.profile->comparison_fingerprint.data(),32);PutPolicy(preimage.data()+72,kHashPolicy);preimage[96]=value.state==TimestampValueStateV3::sql_null?0:1;StoreLittle32(preimage.data()+97,value.state==TimestampValueStateV3::sql_null?0:16);std::size_t extent=101;if(value.state==TimestampValueStateV3::value){EncodeComponent(value.civil_day,value.nanoseconds_since_midnight,preimage.data()+101);extent=117;}if(!Digest(std::span<const byte>(preimage.data(),extent),&digest,&control))return Failure<TimestampNoAllocWriteResultV3>("RESOURCE.BUDGET_EXCEEDED","hash_provider",ResourceStatus());if(Cancelled(control))return Failure<TimestampNoAllocWriteResultV3>("PROCESS.CANCELLED","before_publication");std::memcpy(output,digest.data(),32);auto r=Success<TimestampNoAllocWriteResultV3>();r.bytes_required=32;r.bytes_written=32;return r;}
TimestampBytesResultV3 HashTimestampValueV3(
    const TimestampOwnedValueV3& value) noexcept {
  return HashTimestampValueV3(value, {});
}

TimestampBytesResultV3 HashTimestampValueV3(
    const TimestampOwnedValueV3& value,
    const TimestampExecutionControlV3& control) noexcept {
  auto profile_pin = value.profile;
  std::array<byte, kTimestampHashBytesV3> staged{};
  ScopedClear clear_staged(
      staged.data(), staged.size(), TimestampScrubClassV3::hash_owned_buffer,
      control.observe_scrubbed, control.scrub_observer_context);
  const auto written = HashTimestampValueIntoNoAllocV3(
      value, staged.data(), staged.size(), control);
  if (!written.ok()) {
    return Failure<TimestampBytesResultV3>(
        written.diagnostic.diagnostic_code, written.diagnostic.detail,
        written.status);
  }
  auto result = Success<TimestampBytesResultV3>();
  ScopedVectorClear clear_result(
      &result.bytes, TimestampScrubClassV3::hash_owned_buffer,
      control.observe_scrubbed, control.scrub_observer_context);
  try {
    result.bytes.assign(staged.begin(), staged.end());
  } catch (...) {
    return Failure<TimestampBytesResultV3>(
        "RESOURCE.BUDGET_EXCEEDED", "hash_allocation", ResourceStatus());
  }
  if (Cancelled(control)) {
    return Failure<TimestampBytesResultV3>(
        "PROCESS.CANCELLED", "before_publication");
  }
  clear_result.Disarm();
  (void)profile_pin;
  return result;
}

TimestampNoAllocWriteResultV3 MakeTimestampSortKeyIntoNoAllocV3(const TimestampOwnedValueV3& value,TimestampSortDirectionV3 direction,TimestampNullModeV3 null_mode,byte* output,u64 capacity,const TimestampExecutionControlV3& control) noexcept {const auto checked=ValidateTimestampValueViewV3(value.view(),true);if(!checked.ok())return Failure<TimestampNoAllocWriteResultV3>(checked.diagnostic.diagnostic_code,checked.diagnostic.detail,checked.status);if(static_cast<unsigned>(direction)>1||static_cast<unsigned>(null_mode)>1)return Failure<TimestampNoAllocWriteResultV3>("CTI.TEMPORAL.INDEX_KEY_REFUSED","key_mode");const u64 extent=value.state==TimestampValueStateV3::sql_null?100:112;std::array<byte,112> staged{};ScopedClear clear(staged.data(),staged.size(),TimestampScrubClassV3::ordered_key_staging,control.observe_scrubbed,control.scrub_observer_context);if(output&&(RangesOverlap(output,extent,&value,sizeof(value))||RangesOverlap(output,extent,&control,sizeof(control))||OutputOverlapsProfile(output,extent,*value.profile)))return Failure<TimestampNoAllocWriteResultV3>("CTI.TEMPORAL.CANONICAL_ENCODING_INVALID","key_overlap");if(capacity<extent||!output){auto r=Failure<TimestampNoAllocWriteResultV3>("RESOURCE.BUDGET_EXCEEDED","key_capacity",ResourceStatus());r.bytes_required=extent;return r;}if(extent>control.maximum_allocation_bytes){auto r=Failure<TimestampNoAllocWriteResultV3>("RESOURCE.BUDGET_EXCEEDED","key_budget",ResourceStatus());r.bytes_required=extent;return r;}std::memcpy(staged.data(),"SBTSPK01",8);PutUuid(staged.data()+8,value.profile->receipt.catalog_snapshot_uuid);StoreLittle64(staged.data()+24,value.profile->receipt.catalog_generation);StoreLittle64(staged.data()+32,value.profile->receipt.registry_generation);std::memcpy(staged.data()+40,value.profile->comparison_fingerprint.data(),32);PutPolicy(staged.data()+72,kOrderingPolicy);staged[96]=static_cast<byte>(direction);staged[97]=static_cast<byte>(null_mode);staged[98]=value.state==TimestampValueStateV3::value?1:(null_mode==TimestampNullModeV3::nulls_first?0:2);staged[99]=value.state==TimestampValueStateV3::value?12:0;if(value.state==TimestampValueStateV3::value){const std::int64_t sec=static_cast<std::int64_t>(value.civil_day)*86'400+static_cast<std::int64_t>(value.nanoseconds_since_midnight/1'000'000'000ull);u64 sortable=static_cast<u64>(sec)^0x8000000000000000ull;const u32 ns=static_cast<u32>(value.nanoseconds_since_midnight%1'000'000'000ull);for(unsigned i=0;i<8;++i)staged[100+i]=static_cast<byte>(sortable>>(56-i*8));for(unsigned i=0;i<4;++i)staged[108+i]=static_cast<byte>(ns>>(24-i*8));if(direction==TimestampSortDirectionV3::descending)for(unsigned i=100;i<112;++i)staged[i]=static_cast<byte>(~staged[i]);}if(Cancelled(control))return Failure<TimestampNoAllocWriteResultV3>("PROCESS.CANCELLED","before_publication");std::memcpy(output,staged.data(),extent);auto r=Success<TimestampNoAllocWriteResultV3>();r.bytes_required=extent;r.bytes_written=extent;r.containing_null=value.state==TimestampValueStateV3::sql_null;return r;}
TimestampBytesResultV3 MakeTimestampSortKeyV3(
    const TimestampOwnedValueV3& value,
    TimestampSortDirectionV3 direction,
    TimestampNullModeV3 null_mode,
    const TimestampExecutionControlV3& control) noexcept {
  auto profile_pin = value.profile;
  std::array<byte, kTimestampValueSortKeyBytesV3> staged{};
  ScopedClear clear_staged(
      staged.data(), staged.size(),
      TimestampScrubClassV3::ordered_key_owned_buffer,
      control.observe_scrubbed, control.scrub_observer_context);
  const auto written = MakeTimestampSortKeyIntoNoAllocV3(
      value, direction, null_mode, staged.data(), staged.size(), control);
  if (!written.ok()) {
    return Failure<TimestampBytesResultV3>(
        written.diagnostic.diagnostic_code, written.diagnostic.detail,
        written.status);
  }
  auto result = Success<TimestampBytesResultV3>();
  ScopedVectorClear clear_result(
      &result.bytes, TimestampScrubClassV3::ordered_key_owned_buffer,
      control.observe_scrubbed, control.scrub_observer_context);
  try {
    result.bytes.assign(
        staged.begin(),
        staged.begin() + static_cast<std::ptrdiff_t>(written.bytes_written));
  } catch (...) {
    return Failure<TimestampBytesResultV3>(
        "RESOURCE.BUDGET_EXCEEDED", "key_allocation", ResourceStatus());
  }
  if (Cancelled(control)) {
    return Failure<TimestampBytesResultV3>(
        "PROCESS.CANCELLED", "before_publication");
  }
  clear_result.Disarm();
  (void)profile_pin;
  return result;
}

TimestampSortKeyViewResultV3 DecodeTimestampSortKeyNoAllocV3(
    const TimestampValidatedProfileHandleV3& profile,
    std::span<const byte> encoded,
    const TimestampExecutionControlV3& control) noexcept {
  if ((encoded.size() != kTimestampNullSortKeyBytesV3 &&
       encoded.size() != kTimestampValueSortKeyBytesV3) ||
      std::memcmp(encoded.data(), "SBTSPK01", 8) != 0) {
    return Failure<TimestampSortKeyViewResultV3>(
        "CTI.TEMPORAL.CANONICAL_ENCODING_INVALID", "key_extent_or_magic");
  }
  if (!ProfileValidNoAlloc(profile, &control) ||
      std::memcmp(encoded.data() + 8,
                  profile.receipt.catalog_snapshot_uuid.bytes.data(), 16) != 0 ||
      LoadLittle64(encoded.data() + 24) != profile.receipt.catalog_generation ||
      LoadLittle64(encoded.data() + 32) != profile.receipt.registry_generation ||
      std::memcmp(encoded.data() + 40,
                  profile.comparison_fingerprint.data(), 32) != 0 ||
      std::memcmp(encoded.data() + 72,
                  profile.identity.ordering_policy.uuid.bytes.data(), 16) != 0 ||
      LoadLittle64(encoded.data() + 88) !=
          profile.identity.ordering_policy.generation) {
    return Failure<TimestampSortKeyViewResultV3>(
        "CTI.TEMPORAL.DESCRIPTOR_INVALID", "key_authority");
  }
  if (encoded[96] > 1 || encoded[97] > 1) {
    return Failure<TimestampSortKeyViewResultV3>(
        "CTI.TEMPORAL.CANONICAL_ENCODING_INVALID", "key_mode");
  }

  const auto direction = static_cast<TimestampSortDirectionV3>(encoded[96]);
  const auto null_mode = static_cast<TimestampNullModeV3>(encoded[97]);
  TimestampValueStateV3 state = TimestampValueStateV3::sql_null;
  std::int32_t civil_day = 0;
  u64 nanoseconds_since_midnight = 0;
  std::int64_t civil_seconds = 0;
  u32 nanosecond = 0;

  if (encoded.size() == kTimestampNullSortKeyBytesV3) {
    if (encoded[98] !=
            (null_mode == TimestampNullModeV3::nulls_first ? 0 : 2) ||
        encoded[99] != 0) {
      return Failure<TimestampSortKeyViewResultV3>(
          "CTI.TEMPORAL.CANONICAL_ENCODING_INVALID", "key_null_rank");
    }
  } else {
    if (encoded[98] != 1 || encoded[99] != 12) {
      return Failure<TimestampSortKeyViewResultV3>(
          "CTI.TEMPORAL.CANONICAL_ENCODING_INVALID", "key_value_rank");
    }
    u64 sortable_seconds = 0;
    for (unsigned index = 0; index < 8; ++index) {
      byte octet = encoded[100 + index];
      if (direction == TimestampSortDirectionV3::descending)
        octet = static_cast<byte>(~octet);
      sortable_seconds = (sortable_seconds << 8) | octet;
    }
    for (unsigned index = 0; index < 4; ++index) {
      byte octet = encoded[108 + index];
      if (direction == TimestampSortDirectionV3::descending)
        octet = static_cast<byte>(~octet);
      nanosecond = (nanosecond << 8) | octet;
    }
    if (nanosecond >= 1'000'000'000) {
      return Failure<TimestampSortKeyViewResultV3>(
          "CTI.TEMPORAL.CANONICAL_ENCODING_INVALID", "key_nanosecond");
    }
    civil_seconds = static_cast<std::int64_t>(
        sortable_seconds ^ 0x8000000000000000ull);
    const std::int64_t day = FloorDiv(civil_seconds, 86'400);
    if (day < std::numeric_limits<std::int32_t>::min() ||
        day > std::numeric_limits<std::int32_t>::max()) {
      return Failure<TimestampSortKeyViewResultV3>(
          "CTI.TEMPORAL.CANONICAL_ENCODING_INVALID", "key_day");
    }
    civil_day = static_cast<std::int32_t>(day);
    nanoseconds_since_midnight =
        static_cast<u64>(civil_seconds - day * 86'400) * 1'000'000'000ull +
        nanosecond;
    state = TimestampValueStateV3::value;
  }

  std::array<byte, kTimestampValueSortKeyBytesV3> reencoded{};
  ScopedClear clear_reencoded(
      reencoded.data(), reencoded.size(),
      TimestampScrubClassV3::ordered_key_decode_reencode,
      control.observe_scrubbed, control.scrub_observer_context);
  std::memcpy(reencoded.data(), "SBTSPK01", 8);
  PutUuid(reencoded.data() + 8, profile.receipt.catalog_snapshot_uuid);
  StoreLittle64(reencoded.data() + 24, profile.receipt.catalog_generation);
  StoreLittle64(reencoded.data() + 32, profile.receipt.registry_generation);
  std::memcpy(reencoded.data() + 40,
              profile.comparison_fingerprint.data(), 32);
  PutPolicy(reencoded.data() + 72, profile.identity.ordering_policy);
  reencoded[96] = encoded[96];
  reencoded[97] = encoded[97];
  reencoded[98] = state == TimestampValueStateV3::value
      ? 1
      : (null_mode == TimestampNullModeV3::nulls_first ? 0 : 2);
  reencoded[99] = state == TimestampValueStateV3::value ? 12 : 0;
  if (state == TimestampValueStateV3::value) {
    u64 sortable_seconds = static_cast<u64>(civil_seconds) ^
                           0x8000000000000000ull;
    for (unsigned index = 0; index < 8; ++index)
      reencoded[100 + index] =
          static_cast<byte>(sortable_seconds >> (56 - index * 8));
    for (unsigned index = 0; index < 4; ++index)
      reencoded[108 + index] =
          static_cast<byte>(nanosecond >> (24 - index * 8));
    if (direction == TimestampSortDirectionV3::descending) {
      for (unsigned index = 100; index < 112; ++index)
        reencoded[index] = static_cast<byte>(~reencoded[index]);
    }
  }
  if (control.force_reencode_mismatch_for_conformance)
    reencoded[0] ^= 1;
  if (!std::equal(reencoded.begin(),
                  reencoded.begin() +
                      static_cast<std::ptrdiff_t>(encoded.size()),
                  encoded.begin())) {
    return Failure<TimestampSortKeyViewResultV3>(
        "CTI.TEMPORAL.CANONICAL_ENCODING_INVALID", "key_reencode");
  }
  if (encoded.size() > control.maximum_allocation_bytes) {
    return Failure<TimestampSortKeyViewResultV3>(
        "RESOURCE.BUDGET_EXCEEDED", "key_extent_budget", ResourceStatus());
  }
  if (Cancelled(control)) {
    return Failure<TimestampSortKeyViewResultV3>(
        "PROCESS.CANCELLED", "before_publication");
  }
  auto result = Success<TimestampSortKeyViewResultV3>();
  result.value = {&profile, direction, null_mode, state, civil_day,
                  nanoseconds_since_midnight};
  return result;
}

TimestampCastPolicyDispositionV3 ClassifyTimestampCastPolicyRowV3(u32 row,DatatypeCastContext context) noexcept {if(row==0||row>221||(context!=DatatypeCastContext::implicit&&context!=DatatypeCastContext::assignment&&context!=DatatypeCastContext::explicit_cast))return TimestampCastPolicyDispositionV3::forbidden;if(row==1)return TimestampCastPolicyDispositionV3::contextual_null;if(row==53)return TimestampCastPolicyDispositionV3::identity;if(row==24&&context==DatatypeCastContext::explicit_cast)return TimestampCastPolicyDispositionV3::explicit_character_to_timestamp;if(row==50&&context==DatatypeCastContext::explicit_cast)return TimestampCastPolicyDispositionV3::explicit_timestamp_to_character;return TimestampCastPolicyDispositionV3::forbidden;}
TimestampCastResultV3 CastTimestampValueV3(
    const TimestampCastRequestV3& request) noexcept {
  TimestampCastRowShape shape;
  if (!ResolveTimestampCastRowShape(request.one_based_policy_row, &shape))
    return Failure<TimestampCastResultV3>(
        "DATATYPE.CAST_FORBIDDEN", "timestamp_policy_row_required");
  const auto disposition = ClassifyTimestampCastPolicyRowV3(
      request.one_based_policy_row, request.context);

  // Row 53 is the sole timestamp-to-timestamp row. Every other row has one
  // timestamp endpoint and one scalar endpoint. Extra endpoint fields cannot
  // silently select a different registry row.
  if (request.one_based_policy_row == 53) {
    const bool has_owned_source = request.timestamp_source != nullptr;
    const bool has_dynamic_source = request.dynamic_timestamp_source != nullptr;
    if (has_owned_source == has_dynamic_source ||
        request.timestamp_target == nullptr || !*request.timestamp_target ||
        request.scalar_source != nullptr ||
        request.scalar_source_identity != nullptr ||
        request.timestamp_target_descriptor != nullptr ||
        request.scalar_target != CanonicalTypeId::unknown ||
        request.scalar_target_identity != nullptr ||
        ExecutionDescriptorPresentNoAlloc(request.scalar_target_descriptor) ||
        request.use_character_output_buffer ||
        request.character_output != nullptr ||
        request.character_output_capacity != 0)
      return Failure<TimestampCastResultV3>(
          "CTI.TEMPORAL.DESCRIPTOR_INVALID", "identity_cast_shape_invalid");
    const auto& source_profile = has_owned_source
        ? request.timestamp_source->profile
        : request.dynamic_timestamp_source->profile;
    if (!source_profile || !ProfileValidNoAlloc(*source_profile))
      return Failure<TimestampCastResultV3>(
          "CTI.TEMPORAL.DESCRIPTOR_INVALID", "identity_source_profile_invalid");
    if (!ProfileValidNoAlloc(**request.timestamp_target) ||
        source_profile->comparison_fingerprint !=
            (*request.timestamp_target)->comparison_fingerprint)
      return Failure<TimestampCastResultV3>(
          "CTI.TEMPORAL.DESCRIPTOR_INVALID", "identity_cast_cohort_mismatch");
    const auto source = has_owned_source
        ? ValidateTimestampValueViewV3(request.timestamp_source->view(), true)
        : AdmitTimestampOperandV3(*request.dynamic_timestamp_source, true);
    if (!source.ok())
      return Failure<TimestampCastResultV3>(
          source.diagnostic.diagnostic_code, source.diagnostic.detail,
          source.status);
    if (source.value.state == TimestampValueStateV3::sql_null &&
        !request.target_null_allowed)
      return Failure<TimestampCastResultV3>(
          "DATATYPE.NULL_NOT_ADMITTED", "identity_target_nonnullable");
    if (disposition != TimestampCastPolicyDispositionV3::identity)
      return Failure<TimestampCastResultV3>(
          "DATATYPE.CAST_FORBIDDEN", "identity_context_forbidden");
    auto source_pin = source_profile;
    auto target_pin = *request.timestamp_target;
    if (source.value.state == TimestampValueStateV3::value &&
        request.control.maximum_allocation_bytes <
            kTimestampComponentBytesV3) {
      auto failure = Failure<TimestampCastResultV3>(
          "RESOURCE.BUDGET_EXCEEDED", "identity_result_grant",
          ResourceStatus());
      failure.bytes_required = kTimestampComponentBytesV3;
      return failure;
    }
    if (Cancelled(request.control))
      return Failure<TimestampCastResultV3>(
          "PROCESS.CANCELLED", "before_cast_publication");
    auto result = Success<TimestampCastResultV3>();
    result.category = DatatypeCastCategory::identity;
    result.produced_timestamp = true;
    result.timestamp_value = {
        std::move(target_pin), source.value.state, source.value.civil_day,
        source.value.nanoseconds_since_midnight};
    (void)source_pin;
    return result;
  }

  if (shape.incoming) {
    if (request.timestamp_target == nullptr || !*request.timestamp_target ||
        request.timestamp_source != nullptr ||
        request.dynamic_timestamp_source != nullptr ||
        request.scalar_source == nullptr ||
        request.scalar_target != CanonicalTypeId::unknown ||
        request.scalar_target_identity != nullptr ||
        ExecutionDescriptorPresentNoAlloc(request.scalar_target_descriptor) ||
        request.use_character_output_buffer ||
        request.character_output != nullptr ||
        request.character_output_capacity != 0)
      return Failure<TimestampCastResultV3>(
          "CTI.TEMPORAL.DESCRIPTOR_INVALID", "incoming_cast_shape_invalid");

    // The timestamp endpoint is always resolved before the peer carrier. This
    // preserves the Core authority-before-state/policy precedence.
    if (!ProfileValidNoAlloc(**request.timestamp_target))
      return Failure<TimestampCastResultV3>(
          "CTI.TEMPORAL.DESCRIPTOR_INVALID", "timestamp_target_invalid");
    if (request.timestamp_target_descriptor == nullptr ||
        !DescriptorBindsIdentityNoAlloc(
            *request.timestamp_target_descriptor,
            (*request.timestamp_target)->identity,
            CanonicalTypeId::timestamp))
      return Failure<TimestampCastResultV3>(
          "CTI.TEMPORAL.DESCRIPTOR_INVALID",
          "timestamp_target_admission_invalid");

    if (shape.contextual_null) {
      if (request.scalar_source_identity != nullptr ||
          request.scalar_source->type_id != CanonicalTypeId::null_type)
        return Failure<TimestampCastResultV3>(
            "CTI.TEMPORAL.DESCRIPTOR_INVALID",
            "contextual_null_authority_invalid");
      if (ExecutionDescriptorPresentNoAlloc(
              request.scalar_source->descriptor))
        return Failure<TimestampCastResultV3>(
            "CTI.TEMPORAL.DESCRIPTOR_INVALID",
            "contextual_null_descriptor_forbidden");
      if (!request.scalar_source->is_null ||
          !request.scalar_source->encoded_value.empty())
        return Failure<TimestampCastResultV3>(
            "DATATYPE.NULL_STATE.INVALID", "contextual_null_dirty");
      if (!request.target_null_allowed)
        return Failure<TimestampCastResultV3>(
            "DATATYPE.NULL_NOT_ADMITTED", "target_nonnullable");
      if (disposition != TimestampCastPolicyDispositionV3::contextual_null)
        return Failure<TimestampCastResultV3>(
            "DATATYPE.CAST_FORBIDDEN",
            "contextual_null_context_forbidden");
      auto target_pin = *request.timestamp_target;
      if (Cancelled(request.control))
        return Failure<TimestampCastResultV3>(
            "PROCESS.CANCELLED", "before_cast_publication");
      auto result = Success<TimestampCastResultV3>();
      result.category = DatatypeCastCategory::identity;
      result.produced_timestamp = true;
      result.timestamp_value = {std::move(target_pin),
          TimestampValueStateV3::sql_null, 0, 0};
      return result;
    }

    if (shape.peer_type != CanonicalTypeId::unknown &&
        request.scalar_source->type_id != shape.peer_type)
      return Failure<TimestampCastResultV3>(
          "CTI.TEMPORAL.DESCRIPTOR_INVALID",
          "incoming_peer_type_mismatch");
    if (shape.peer_type == CanonicalTypeId::unknown &&
        request.scalar_source->type_id != CanonicalTypeId::unknown)
      return Failure<TimestampCastResultV3>(
          "CTI.TEMPORAL.DESCRIPTOR_INVALID",
          "unregistered_incoming_type_claim");
    if (shape.exact_peer_identity) {
      if (!ExactPeerIdentity(request.scalar_source_identity,
                             shape.peer_type) ||
          !DescriptorBindsIdentityNoAlloc(
              request.scalar_source->descriptor,
              *request.scalar_source_identity, shape.peer_type))
        return Failure<TimestampCastResultV3>(
            "CTI.TEMPORAL.DESCRIPTOR_INVALID",
            "incoming_peer_identity_invalid");
    } else if (request.scalar_source_identity != nullptr ||
               ExecutionDescriptorPresentNoAlloc(
                   request.scalar_source->descriptor)) {
      return Failure<TimestampCastResultV3>(
          "CTI.TEMPORAL.DESCRIPTOR_INVALID",
          "unregistered_incoming_authority_claim");
    }
    if (shape.peer_type == CanonicalTypeId::character &&
        !ExactCharacterDescriptor(request.scalar_source->descriptor,
                                  ReceiptForIdentity(*request.scalar_source_identity)))
      return Failure<TimestampCastResultV3>(
          "CTI.TEMPORAL.DESCRIPTOR_INVALID",
          "character_source_descriptor_invalid");
    if (request.scalar_source->is_null) {
      if (!request.scalar_source->encoded_value.empty())
        return Failure<TimestampCastResultV3>(
            "DATATYPE.NULL_STATE.INVALID", "incoming_peer_dirty_null");
      if (!request.target_null_allowed)
        return Failure<TimestampCastResultV3>(
            "DATATYPE.NULL_NOT_ADMITTED", "target_nonnullable");
    }
    if (disposition == TimestampCastPolicyDispositionV3::forbidden)
      return Failure<TimestampCastResultV3>(
          "DATATYPE.CAST_FORBIDDEN",
          "closed_timestamp_cast_policy_forbidden");
    if (disposition !=
            TimestampCastPolicyDispositionV3::
                explicit_character_to_timestamp ||
        shape.peer_type != CanonicalTypeId::character)
      return Failure<TimestampCastResultV3>(
          "CTI.TEMPORAL.DESCRIPTOR_INVALID",
          "character_source_descriptor_invalid");

    auto target_pin = *request.timestamp_target;
    if (request.scalar_source->is_null) {
      if (Cancelled(request.control))
        return Failure<TimestampCastResultV3>(
            "PROCESS.CANCELLED", "before_cast_publication");
      auto result = Success<TimestampCastResultV3>();
      result.category = DatatypeCastCategory::lossless_explicit;
      result.produced_timestamp = true;
      result.timestamp_value = {std::move(target_pin),
          TimestampValueStateV3::sql_null, 0, 0};
      return result;
    }
    const auto parsed = ParseStrict(request.scalar_source->encoded_value);
    if (!parsed.ok)
      return Failure<TimestampCastResultV3>(parsed.code, parsed.detail);
    if (request.scalar_source->descriptor.length != 0 &&
        request.scalar_source->encoded_value.size() >
            request.scalar_source->descriptor.length)
      return Failure<TimestampCastResultV3>(
          "CTB.TEXT.LENGTH_EXCEEDED", "character_source_length");
    if (request.control.maximum_allocation_bytes <
        kTimestampComponentBytesV3) {
      auto failure = Failure<TimestampCastResultV3>(
          "RESOURCE.BUDGET_EXCEEDED", "timestamp_result_grant",
          ResourceStatus());
      failure.bytes_required = kTimestampComponentBytesV3;
      return failure;
    }
    if (Cancelled(request.control))
      return Failure<TimestampCastResultV3>(
          "PROCESS.CANCELLED", "before_cast_publication");
    auto result = Success<TimestampCastResultV3>();
    result.category = DatatypeCastCategory::lossless_explicit;
    result.produced_timestamp = true;
    result.timestamp_value = {std::move(target_pin),
        TimestampValueStateV3::value, parsed.civil_day,
        parsed.nanoseconds_since_midnight};
    return result;
  }

  const bool has_owned_source = request.timestamp_source != nullptr;
  const bool has_dynamic_source =
      request.dynamic_timestamp_source != nullptr;
  if (has_owned_source == has_dynamic_source ||
      request.timestamp_target != nullptr ||
      request.timestamp_target_descriptor != nullptr ||
      request.scalar_source != nullptr ||
      request.scalar_source_identity != nullptr ||
      request.scalar_target != shape.peer_type ||
      ((request.one_based_policy_row != 50 ||
        !request.use_character_output_buffer) &&
       (request.character_output != nullptr ||
        request.character_output_capacity != 0)) ||
      (request.one_based_policy_row != 50 &&
       request.use_character_output_buffer))
    return Failure<TimestampCastResultV3>(
        "CTI.TEMPORAL.DESCRIPTOR_INVALID", "outgoing_cast_shape_invalid");
  const auto& source_profile = has_owned_source
      ? request.timestamp_source->profile
      : request.dynamic_timestamp_source->profile;
  if (!source_profile || !ProfileValidNoAlloc(*source_profile))
    return Failure<TimestampCastResultV3>(
        "CTI.TEMPORAL.DESCRIPTOR_INVALID",
        "outgoing_source_profile_invalid");

  if (shape.exact_peer_identity) {
    if (!ExactPeerIdentity(request.scalar_target_identity,
                           shape.peer_type) ||
        !DescriptorBindsIdentityNoAlloc(
            request.scalar_target_descriptor,
            *request.scalar_target_identity, shape.peer_type))
      return Failure<TimestampCastResultV3>(
          "CTI.TEMPORAL.DESCRIPTOR_INVALID",
          "outgoing_peer_identity_invalid");
  } else if (request.scalar_target_identity != nullptr ||
             ExecutionDescriptorPresentNoAlloc(
                 request.scalar_target_descriptor)) {
    return Failure<TimestampCastResultV3>(
        "CTI.TEMPORAL.DESCRIPTOR_INVALID",
        "unregistered_outgoing_authority_claim");
  }
  if (shape.peer_type == CanonicalTypeId::character &&
      !ExactCharacterDescriptor(request.scalar_target_descriptor,
                                ReceiptForIdentity(*request.scalar_target_identity)))
    return Failure<TimestampCastResultV3>(
        "CTI.TEMPORAL.DESCRIPTOR_INVALID",
        "character_target_descriptor_invalid");
  const auto source = has_owned_source
      ? ValidateTimestampValueViewV3(request.timestamp_source->view(), true)
      : AdmitTimestampOperandV3(*request.dynamic_timestamp_source, true);
  if (!source.ok())
    return Failure<TimestampCastResultV3>(
        source.diagnostic.diagnostic_code, source.diagnostic.detail,
        source.status);
  if (source.value.state == TimestampValueStateV3::sql_null &&
      (!request.target_null_allowed ||
       !request.scalar_target_descriptor.nullable_allowed))
    return Failure<TimestampCastResultV3>(
        "DATATYPE.NULL_NOT_ADMITTED", "scalar_target_nonnullable");
  if (disposition == TimestampCastPolicyDispositionV3::forbidden)
    return Failure<TimestampCastResultV3>(
        "DATATYPE.CAST_FORBIDDEN",
        "closed_timestamp_cast_policy_forbidden");
  if (disposition !=
          TimestampCastPolicyDispositionV3::
              explicit_timestamp_to_character ||
      shape.peer_type != CanonicalTypeId::character)
    return Failure<TimestampCastResultV3>(
        "CTI.TEMPORAL.DESCRIPTOR_INVALID",
        "character_target_descriptor_invalid");

  std::array<char,45> rendered{};
  ScopedClear clear_rendered(
      rendered.data(), rendered.size(),
      TimestampScrubClassV3::character_render_staging,
      request.control.observe_scrubbed,
      request.control.scrub_observer_context);
  const bool containing_null =
      source.value.state == TimestampValueStateV3::sql_null;
  const u64 extent = containing_null
      ? 0
      : RenderStrict(source.value.civil_day,
                     source.value.nanoseconds_since_midnight,
                     rendered.data());
  auto source_pin = source_profile;
  if (request.use_character_output_buffer &&
      request.character_output != nullptr && extent != 0 &&
      ((has_owned_source &&
        RangesOverlap(request.character_output, extent,
                      request.timestamp_source,
                      sizeof(*request.timestamp_source))) ||
       (has_dynamic_source &&
        RangesOverlap(request.character_output, extent,
                      request.dynamic_timestamp_source,
                      sizeof(*request.dynamic_timestamp_source))) ||
       RangesOverlap(request.character_output, extent,
                     &request, sizeof(request)) ||
       RangesOverlap(request.character_output, extent,
                     &request.scalar_target_descriptor,
                     sizeof(request.scalar_target_descriptor)) ||
       (request.scalar_target_identity != nullptr &&
        RangesOverlap(request.character_output, extent,
                      request.scalar_target_identity,
                      sizeof(*request.scalar_target_identity))) ||
       OutputOverlapsProfile(request.character_output, extent,
                             *source_pin)))
    return Failure<TimestampCastResultV3>(
        "CTI.TEMPORAL.CANONICAL_ENCODING_INVALID",
        "character_output_aliases_input_or_profile");
  if (request.scalar_target_descriptor.length != 0 &&
      extent > request.scalar_target_descriptor.length) {
    auto failure = Failure<TimestampCastResultV3>(
        "CTB.TEXT.LENGTH_EXCEEDED", "character_target_length");
    failure.bytes_required = extent;
    return failure;
  }
  if (request.use_character_output_buffer &&
      (extent > request.character_output_capacity ||
       (extent != 0 && request.character_output == nullptr))) {
    auto failure = Failure<TimestampCastResultV3>(
        "CTB.TEXT.LENGTH_EXCEEDED", "character_output_capacity");
    failure.bytes_required = extent;
    return failure;
  }
  if (extent > request.control.maximum_allocation_bytes) {
    auto failure = Failure<TimestampCastResultV3>(
        "RESOURCE.BUDGET_EXCEEDED", "render_resource_grant",
        ResourceStatus());
    failure.bytes_required = extent;
    return failure;
  }
  if (Cancelled(request.control))
    return Failure<TimestampCastResultV3>(
        "PROCESS.CANCELLED", "before_cast_publication");

  if (request.use_character_output_buffer) {
    if (extent != 0)
      std::memcpy(request.character_output, rendered.data(), extent);
    auto result = Success<TimestampCastResultV3>();
    result.category = DatatypeCastCategory::lossless_explicit;
    result.used_character_output_buffer = true;
    result.bytes_required = extent;
    result.bytes_written = extent;
    result.scalar_value.type_id = CanonicalTypeId::character;
    result.scalar_value.is_null = containing_null;
    return result;
  }
  try {
    auto result = Success<TimestampCastResultV3>();
    result.category = DatatypeCastCategory::lossless_explicit;
    result.bytes_required = extent;
    result.bytes_written = extent;
    result.scalar_value.type_id = CanonicalTypeId::character;
    result.scalar_value.is_null = containing_null;
    result.scalar_value.descriptor = request.scalar_target_descriptor;
    result.scalar_value.encoded_value.assign(rendered.data(), extent);
    if (Cancelled(request.control)) {
      if (!result.scalar_value.encoded_value.empty())
        SecureClear(result.scalar_value.encoded_value.data(),
                    result.scalar_value.encoded_value.size());
      return Failure<TimestampCastResultV3>(
          "PROCESS.CANCELLED", "before_cast_publication");
    }
    return result;
  } catch (...) {
    return Failure<TimestampCastResultV3>(
        "RESOURCE.BUDGET_EXCEEDED", "cast_result_allocation",
        ResourceStatus());
  }
}

TimestampValidationResultV3 ValidateTimestampBatchViewV3(
    const TimestampBatchViewV3& batch) noexcept {
  if (batch.profile == nullptr || !ProfileValidNoAlloc(*batch.profile)) {
    return Failure<TimestampValidationResultV3>(
        "CTI.TEMPORAL.DESCRIPTOR_INVALID", "batch_profile");
  }
  if (batch.civil_days.size() != batch.nanoseconds_since_midnight.size()) {
    return Failure<TimestampValidationResultV3>(
        "CTI.TEMPORAL.CANONICAL_ENCODING_INVALID", "batch_array_extent");
  }
  const std::size_t rows = batch.civil_days.size();
  if (batch.null_bitmap_lsb0.size() != (rows + 7) / 8) {
    return Failure<TimestampValidationResultV3>(
        "DATATYPE.NULL_STATE.INVALID", "batch_bitmap_extent");
  }
  if (rows != 0 && rows % 8 != 0) {
    const byte allowed = static_cast<byte>((1u << (rows % 8)) - 1u);
    if ((batch.null_bitmap_lsb0.back() & static_cast<byte>(~allowed)) != 0) {
      return Failure<TimestampValidationResultV3>(
          "DATATYPE.NULL_STATE.INVALID", "batch_bitmap_tail");
    }
  }
  for (std::size_t index = 0; index < rows; ++index) {
    const bool is_null =
        (batch.null_bitmap_lsb0[index / 8] & (1u << (index % 8))) != 0;
    if (is_null &&
        (batch.civil_days[index] != 0 ||
         batch.nanoseconds_since_midnight[index] != 0)) {
      return Failure<TimestampValidationResultV3>(
          "DATATYPE.NULL_STATE.INVALID", "batch_dirty_null");
    }
    if (!is_null &&
        batch.nanoseconds_since_midnight[index] >
            kTimestampMaximumNanosecondsV3) {
      return Failure<TimestampValidationResultV3>(
          "CTI.TEMPORAL.RANGE_EXCEEDED", "batch_time_range");
    }
  }
  return Success<TimestampValidationResultV3>();
}

TimestampBatchExtentsResultV3 ComputeTimestampBatchExtentsV3(
    u64 rows, u64 limit) noexcept {
  if (rows > (std::numeric_limits<u64>::max() - 7) / 8) {
    return Failure<TimestampBatchExtentsResultV3>(
        "RESOURCE.BUDGET_EXCEEDED", "batch_extent", ResourceStatus());
  }
  const u64 civil_days_bytes = rows * sizeof(std::int32_t);
  const u64 nanoseconds_bytes = rows * sizeof(u64);
  const u64 bitmap_bytes = (rows + 7) / 8;
  if ((rows != 0 && civil_days_bytes / sizeof(std::int32_t) != rows) ||
      (rows != 0 && nanoseconds_bytes / sizeof(u64) != rows) ||
      civil_days_bytes >
          std::numeric_limits<u64>::max() - nanoseconds_bytes ||
      civil_days_bytes + nanoseconds_bytes >
          std::numeric_limits<u64>::max() - bitmap_bytes ||
      civil_days_bytes + nanoseconds_bytes + bitmap_bytes > limit) {
    return Failure<TimestampBatchExtentsResultV3>(
        "RESOURCE.BUDGET_EXCEEDED", "batch_limit", ResourceStatus());
  }
  auto result = Success<TimestampBatchExtentsResultV3>();
  result.civil_days_bytes = civil_days_bytes;
  result.nanoseconds_bytes = nanoseconds_bytes;
  result.bitmap_bytes = bitmap_bytes;
  result.combined_bytes =
      civil_days_bytes + nanoseconds_bytes + bitmap_bytes;
  return result;
}

TimestampBatchExtentsResultV3 MaterializeTimestampBatchIntoV3(
    const std::shared_ptr<const TimestampValidatedProfileHandleV3>& profile,
    std::span<const std::int32_t> civil_days,
    std::span<const u64> nanoseconds_since_midnight,
    std::span<const byte> null_bitmap_lsb0,
    std::int32_t* output_civil_days, u64 output_civil_days_bytes,
    u64* output_nanoseconds, u64 output_nanoseconds_bytes,
    byte* output_null_bitmap_lsb0, u64 output_bitmap_bytes,
    const TimestampExecutionControlV3& control) noexcept {
  const auto checked = ValidateTimestampBatchViewV3(
      {profile.get(), civil_days, nanoseconds_since_midnight,
       null_bitmap_lsb0});
  if (!checked.ok()) {
    return Failure<TimestampBatchExtentsResultV3>(
        checked.diagnostic.diagnostic_code, checked.diagnostic.detail,
        checked.status);
  }
  auto extents = ComputeTimestampBatchExtentsV3(
      civil_days.size(), control.maximum_allocation_bytes);
  if (!extents.ok())
    return extents;
  if ((extents.civil_days_bytes != 0 && output_civil_days == nullptr) ||
      (extents.nanoseconds_bytes != 0 && output_nanoseconds == nullptr) ||
      (extents.bitmap_bytes != 0 && output_null_bitmap_lsb0 == nullptr) ||
      output_civil_days_bytes < extents.civil_days_bytes ||
      output_nanoseconds_bytes < extents.nanoseconds_bytes ||
      output_bitmap_bytes < extents.bitmap_bytes) {
    auto result = Failure<TimestampBatchExtentsResultV3>(
        "RESOURCE.BUDGET_EXCEEDED", "batch_capacity", ResourceStatus());
    result.civil_days_bytes = extents.civil_days_bytes;
    result.nanoseconds_bytes = extents.nanoseconds_bytes;
    result.bitmap_bytes = extents.bitmap_bytes;
    result.combined_bytes = extents.combined_bytes;
    return result;
  }

  const auto overlaps_any_input = [&](const void* output, u64 extent) {
    return RangesOverlap(output, extent, civil_days.data(),
                         extents.civil_days_bytes) ||
        RangesOverlap(output, extent, nanoseconds_since_midnight.data(),
                      extents.nanoseconds_bytes) ||
        RangesOverlap(output, extent, null_bitmap_lsb0.data(),
                      extents.bitmap_bytes);
  };
  if (overlaps_any_input(output_civil_days, extents.civil_days_bytes) ||
      overlaps_any_input(output_nanoseconds, extents.nanoseconds_bytes) ||
      overlaps_any_input(output_null_bitmap_lsb0, extents.bitmap_bytes) ||
      RangesOverlap(output_civil_days, extents.civil_days_bytes,
                    output_nanoseconds, extents.nanoseconds_bytes) ||
      RangesOverlap(output_civil_days, extents.civil_days_bytes,
                    output_null_bitmap_lsb0, extents.bitmap_bytes) ||
      RangesOverlap(output_nanoseconds, extents.nanoseconds_bytes,
                    output_null_bitmap_lsb0, extents.bitmap_bytes) ||
      RangesOverlap(output_civil_days, extents.civil_days_bytes,
                    &control, sizeof(control)) ||
      RangesOverlap(output_nanoseconds, extents.nanoseconds_bytes,
                    &control, sizeof(control)) ||
      RangesOverlap(output_null_bitmap_lsb0, extents.bitmap_bytes,
                    &control, sizeof(control)) ||
      OutputOverlapsProfile(output_civil_days, extents.civil_days_bytes,
                            *profile) ||
      OutputOverlapsProfile(output_nanoseconds, extents.nanoseconds_bytes,
                            *profile) ||
      OutputOverlapsProfile(output_null_bitmap_lsb0, extents.bitmap_bytes,
                            *profile)) {
    return Failure<TimestampBatchExtentsResultV3>(
        "CTI.TEMPORAL.CANONICAL_ENCODING_INVALID", "batch_overlap");
  }
  if ((output_civil_days != nullptr &&
       reinterpret_cast<std::uintptr_t>(output_civil_days) %
               alignof(std::int32_t) !=
           0) ||
      (output_nanoseconds != nullptr &&
       reinterpret_cast<std::uintptr_t>(output_nanoseconds) % alignof(u64) !=
           0)) {
    return Failure<TimestampBatchExtentsResultV3>(
        "CTI.TEMPORAL.CANONICAL_ENCODING_INVALID", "batch_alignment");
  }
  if (Cancelled(control)) {
    return Failure<TimestampBatchExtentsResultV3>(
        "PROCESS.CANCELLED", "batch_before_allocation");
  }

  std::vector<std::int32_t> staged_days;
  std::vector<u64> staged_nanoseconds;
  std::vector<byte> staged_bitmap;
  ScopedVectorClear clear_days(
      &staged_days, TimestampScrubClassV3::batch_day_staging,
      control.observe_scrubbed, control.scrub_observer_context);
  ScopedVectorClear clear_nanoseconds(
      &staged_nanoseconds, TimestampScrubClassV3::batch_time_staging,
      control.observe_scrubbed, control.scrub_observer_context);
  ScopedVectorClear clear_bitmap(
      &staged_bitmap, TimestampScrubClassV3::batch_bitmap_staging,
      control.observe_scrubbed, control.scrub_observer_context);
  try {
    staged_days.assign(civil_days.begin(), civil_days.end());
    staged_nanoseconds.assign(nanoseconds_since_midnight.begin(),
                              nanoseconds_since_midnight.end());
    staged_bitmap.assign(null_bitmap_lsb0.begin(), null_bitmap_lsb0.end());
  } catch (...) {
    return Failure<TimestampBatchExtentsResultV3>(
        "RESOURCE.BUDGET_EXCEEDED", "batch_allocation", ResourceStatus());
  }
  if (Cancelled(control)) {
    return Failure<TimestampBatchExtentsResultV3>(
        "PROCESS.CANCELLED", "batch_before_publication");
  }
  if (extents.civil_days_bytes != 0) {
    std::memcpy(output_civil_days, staged_days.data(),
                extents.civil_days_bytes);
  }
  if (extents.nanoseconds_bytes != 0) {
    std::memcpy(output_nanoseconds, staged_nanoseconds.data(),
                extents.nanoseconds_bytes);
  }
  if (extents.bitmap_bytes != 0) {
    std::memcpy(output_null_bitmap_lsb0, staged_bitmap.data(),
                extents.bitmap_bytes);
  }
  return extents;
}

TimestampBatchResultV3 MaterializeTimestampBatchV3(
    std::shared_ptr<const TimestampValidatedProfileHandleV3> profile,
    std::span<const std::int32_t> civil_days,
    std::span<const u64> nanoseconds_since_midnight,
    std::span<const byte> null_bitmap_lsb0,
    const TimestampExecutionControlV3& control) noexcept {
  const auto checked = ValidateTimestampBatchViewV3(
      {profile.get(), civil_days, nanoseconds_since_midnight,
       null_bitmap_lsb0});
  if (!checked.ok()) {
    return Failure<TimestampBatchResultV3>(
        checked.diagnostic.diagnostic_code, checked.diagnostic.detail,
        checked.status);
  }
  const auto extents = ComputeTimestampBatchExtentsV3(
      civil_days.size(), control.maximum_allocation_bytes);
  if (!extents.ok()) {
    return Failure<TimestampBatchResultV3>(
        extents.diagnostic.diagnostic_code, extents.diagnostic.detail,
        extents.status);
  }
  if (Cancelled(control)) {
    return Failure<TimestampBatchResultV3>(
        "PROCESS.CANCELLED", "batch_before_allocation");
  }

  auto result = Success<TimestampBatchResultV3>();
  result.batch.profile = std::move(profile);
  ScopedVectorClear clear_days(
      &result.batch.civil_days, TimestampScrubClassV3::batch_day_staging,
      control.observe_scrubbed, control.scrub_observer_context);
  ScopedVectorClear clear_nanoseconds(
      &result.batch.nanoseconds_since_midnight,
      TimestampScrubClassV3::batch_time_staging,
      control.observe_scrubbed, control.scrub_observer_context);
  ScopedVectorClear clear_bitmap(
      &result.batch.null_bitmap_lsb0,
      TimestampScrubClassV3::batch_bitmap_staging,
      control.observe_scrubbed, control.scrub_observer_context);
  try {
    result.batch.civil_days.assign(civil_days.begin(), civil_days.end());
    result.batch.nanoseconds_since_midnight.assign(
        nanoseconds_since_midnight.begin(), nanoseconds_since_midnight.end());
    result.batch.null_bitmap_lsb0.assign(null_bitmap_lsb0.begin(),
                                         null_bitmap_lsb0.end());
  } catch (...) {
    return Failure<TimestampBatchResultV3>(
        "RESOURCE.BUDGET_EXCEEDED", "batch_allocation", ResourceStatus());
  }
  if (Cancelled(control)) {
    return Failure<TimestampBatchResultV3>(
        "PROCESS.CANCELLED", "batch_before_publication");
  }
  clear_days.Disarm();
  clear_nanoseconds.Disarm();
  clear_bitmap.Disarm();
  return result;
}

TimestampViewResultV3 DecodeTimestampSbdvalComposedNoAllocV3(
    const TimestampValidatedProfileHandleV3& profile, bool null_allowed,
    std::span<const byte> encoded,
    const TimestampExecutionControlV3& control) noexcept {
  if (encoded.size() < 32 ||
      std::memcmp(encoded.data(), "SBDVAL01", 8) != 0 ||
      LoadLittle16(encoded.data() + 14) != 32 ||
      LoadLittle32(encoded.data() + 16) != encoded.size() - 32) {
    return Failure<TimestampViewResultV3>(
        "CTI.TEMPORAL.CANONICAL_ENCODING_INVALID", "sbdval_structure");
  }
  const u16 flags = LoadLittle16(encoded.data() + 12);
  if ((flags & ~u16{3}) != 0 || LoadLittle32(encoded.data() + 20) != 0 ||
      LoadLittle64(encoded.data() + 24) !=
          ComputeDatatypeBinaryPayloadChecksumV1(
              encoded.data() + 32, encoded.size() - 32)) {
    return Failure<TimestampViewResultV3>(
        "CTI.TEMPORAL.CANONICAL_ENCODING_INVALID", "sbdval_integrity");
  }
  if (static_cast<CanonicalTypeId>(LoadLittle32(encoded.data() + 8)) !=
      CanonicalTypeId::timestamp) {
    return Failure<TimestampViewResultV3>(
        "CTI.TEMPORAL.DESCRIPTOR_INVALID", "sbdval_type");
  }
  if (!ProfileValidNoAlloc(profile, &control)) {
    return Failure<TimestampViewResultV3>(
        "CTI.TEMPORAL.DESCRIPTOR_INVALID", "sbdval_profile");
  }
  if ((flags & 2) != 0) {
    return Failure<TimestampViewResultV3>(
        "CTI.TEMPORAL.CANONICAL_ENCODING_INVALID", "sbdval_toast");
  }

  const bool is_null = (flags & 1) != 0;
  TimestampExecutionControlV3 component_control = control;
  component_control.force_reencode_mismatch_for_conformance = false;
  component_control.maximum_allocation_bytes = ~u64{0};
  component_control.cancelled = nullptr;
  component_control.cancellation_context = nullptr;
  const auto decoded = DecodeCanonicalTimestampComponentNoAllocV3(
      profile,
      is_null ? TimestampValueStateV3::sql_null
              : TimestampValueStateV3::value,
      null_allowed,
      {encoded.data() + 32, encoded.size() - 32}, component_control);
  if (!decoded.ok())
    return decoded;

  std::array<byte, 48> reencoded{};
  ScopedClear clear_reencoded(
      reencoded.data(), reencoded.size(), TimestampScrubClassV3::sbdval_reencode,
      control.observe_scrubbed, control.scrub_observer_context);
  const DatatypeBinaryValueView structural{
      CanonicalTypeId::timestamp, is_null, false,
      is_null ? nullptr : encoded.data() + 32, encoded.size() - 32};
  const auto rewritten = EncodeDatatypeBinaryStructuralValueIntoNoAlloc(
      structural, reencoded.data(), encoded.size());
  if (control.force_reencode_mismatch_for_conformance)
    reencoded[0] ^= 1;
  if (!rewritten.ok() || rewritten.bytes_written != encoded.size() ||
      !std::equal(reencoded.begin(),
                  reencoded.begin() +
                      static_cast<std::ptrdiff_t>(encoded.size()),
                  encoded.begin())) {
    return Failure<TimestampViewResultV3>(
        "CTI.TEMPORAL.CANONICAL_ENCODING_INVALID", "sbdval_reencode");
  }
  if (encoded.size() > control.maximum_allocation_bytes) {
    return Failure<TimestampViewResultV3>(
        "RESOURCE.BUDGET_EXCEEDED", "sbdval_decode_budget", ResourceStatus());
  }
  if (Cancelled(control)) {
    return Failure<TimestampViewResultV3>(
        "PROCESS.CANCELLED", "sbdval_before_publication");
  }
  auto result = Success<TimestampViewResultV3>();
  result.value = {&profile, decoded.value.state, decoded.value.civil_day,
                  decoded.value.nanoseconds_since_midnight};
  return result;
}

TimestampBytesResultV3 EncodeTimestampSbdvalComposedV3(
    const TimestampOwnedValueV3& value, bool null_allowed,
    const TimestampExecutionControlV3& control) noexcept {
  const auto checked = ValidateTimestampValueViewV3(value.view(), null_allowed);
  if (!checked.ok()) {
    return Failure<TimestampBytesResultV3>(
        checked.diagnostic.diagnostic_code, checked.diagnostic.detail,
        checked.status);
  }
  auto profile_pin=value.profile;
  const std::size_t payload =
      value.state == TimestampValueStateV3::value ? 16 : 0;
  const std::size_t total = 32 + payload;
  if (total > control.maximum_allocation_bytes) {
    return Failure<TimestampBytesResultV3>(
        "RESOURCE.BUDGET_EXCEEDED", "sbdval_capacity", ResourceStatus());
  }
  if (Cancelled(control)) {
    return Failure<TimestampBytesResultV3>(
        "PROCESS.CANCELLED", "before_allocation");
  }

  std::array<byte, 16> component{};
  ScopedClear clear_component(
      component.data(), component.size(),
      TimestampScrubClassV3::sbdval_component_staging,
      control.observe_scrubbed, control.scrub_observer_context);
  if (payload != 0)
    EncodeComponent(value.civil_day, value.nanoseconds_since_midnight,
                    component.data());

  auto result = Success<TimestampBytesResultV3>();
  ScopedVectorClear clear_result(
      &result.bytes, TimestampScrubClassV3::sbdval_owned_buffer,
      control.observe_scrubbed, control.scrub_observer_context);
  try {
    result.bytes.resize(total);
  } catch (...) {
    return Failure<TimestampBytesResultV3>(
        "RESOURCE.BUDGET_EXCEEDED", "sbdval_allocation", ResourceStatus());
  }
  const DatatypeBinaryValueView structural{
      CanonicalTypeId::timestamp,
      value.state == TimestampValueStateV3::sql_null, false,
      payload != 0 ? component.data() : nullptr, payload};
  const auto encoded = EncodeDatatypeBinaryStructuralValueIntoNoAlloc(
      structural, result.bytes.data(), result.bytes.size());
  if (!encoded.ok() || encoded.bytes_written != total) {
    return Failure<TimestampBytesResultV3>(
        encoded.diagnostic.diagnostic_code, "sbdval_encode", encoded.status);
  }
  TimestampExecutionControlV3 recheck_control;
  recheck_control.observe_scrubbed=control.observe_scrubbed;
  recheck_control.scrub_observer_context=control.scrub_observer_context;
  const auto recheck = DecodeTimestampSbdvalComposedNoAllocV3(
      *profile_pin, null_allowed, result.bytes, recheck_control);
  if (!recheck.ok()) {
    return Failure<TimestampBytesResultV3>(
        recheck.diagnostic.diagnostic_code, "sbdval_recheck", recheck.status);
  }
  if (Cancelled(control)) {
    return Failure<TimestampBytesResultV3>(
        "PROCESS.CANCELLED", "before_publication");
  }
  clear_result.Disarm();
  return result;
}

TimestampViewResultV3 DecodeTimestampSbdpvComposedNoAllocV3(
    const TimestampValidatedProfileHandleV3& profile, bool null_allowed,
    std::span<const byte> encoded,
    const TimestampExecutionControlV3& control) noexcept {
  if (encoded.size() < 24 ||
      std::memcmp(encoded.data(), "SBDPV001", 8) != 0 ||
      LoadLittle16(encoded.data() + 14) != 0 ||
      LoadLittle32(encoded.data() + 16) != encoded.size() - 24) {
    return Failure<TimestampViewResultV3>(
        "CTI.TEMPORAL.CANONICAL_ENCODING_INVALID", "sbdpv_structure");
  }
  const auto type =
      static_cast<CanonicalTypeId>(LoadLittle32(encoded.data() + 8));
  const auto state =
      static_cast<DatatypePhysicalValueState>(LoadLittle16(encoded.data() + 12));
  u32 checksum = 2166136261u;
  const auto mix = [&checksum](u32 value) noexcept {
    checksum ^= value;
    checksum *= 16777619u;
  };
  mix(static_cast<u32>(type));
  mix(static_cast<u32>(state));
  for (std::size_t index = 24; index < encoded.size(); ++index)
    mix(encoded[index]);
  if (checksum != LoadLittle32(encoded.data() + 20)) {
    return Failure<TimestampViewResultV3>(
        "CTI.TEMPORAL.CANONICAL_ENCODING_INVALID", "sbdpv_integrity");
  }
  if (type != CanonicalTypeId::timestamp) {
    return Failure<TimestampViewResultV3>(
        "CTI.TEMPORAL.DESCRIPTOR_INVALID", "sbdpv_type");
  }
  if (!ProfileValidNoAlloc(profile, &control)) {
    return Failure<TimestampViewResultV3>(
        "CTI.TEMPORAL.DESCRIPTOR_INVALID", "sbdpv_profile");
  }
  if (state != DatatypePhysicalValueState::value &&
      state != DatatypePhysicalValueState::sql_null) {
    return Failure<TimestampViewResultV3>(
        "DATATYPE.NULL_STATE.INVALID", "sbdpv_state");
  }

  const bool is_null = state == DatatypePhysicalValueState::sql_null;
  TimestampExecutionControlV3 component_control = control;
  component_control.force_reencode_mismatch_for_conformance = false;
  component_control.maximum_allocation_bytes = ~u64{0};
  component_control.cancelled = nullptr;
  component_control.cancellation_context = nullptr;
  const auto decoded = DecodeCanonicalTimestampComponentNoAllocV3(
      profile,
      is_null ? TimestampValueStateV3::sql_null
              : TimestampValueStateV3::value,
      null_allowed,
      {encoded.data() + 24, encoded.size() - 24}, component_control);
  if (!decoded.ok())
    return decoded;

  std::array<byte, 40> reencoded{};
  ScopedClear clear_reencoded(
      reencoded.data(), reencoded.size(), TimestampScrubClassV3::sbdpv_reencode,
      control.observe_scrubbed, control.scrub_observer_context);
  const DatatypePhysicalValueView structural{
      CanonicalTypeId::timestamp, state,
      is_null ? nullptr : encoded.data() + 24, encoded.size() - 24};
  const auto rewritten = EncodeDatatypePhysicalStructuralValueIntoNoAlloc(
      structural, reencoded.data(), encoded.size());
  if (control.force_reencode_mismatch_for_conformance)
    reencoded[0] ^= 1;
  if (!rewritten.ok() || rewritten.bytes_written != encoded.size() ||
      !std::equal(reencoded.begin(),
                  reencoded.begin() +
                      static_cast<std::ptrdiff_t>(encoded.size()),
                  encoded.begin())) {
    return Failure<TimestampViewResultV3>(
        "CTI.TEMPORAL.CANONICAL_ENCODING_INVALID", "sbdpv_reencode");
  }
  if (encoded.size() > control.maximum_allocation_bytes) {
    return Failure<TimestampViewResultV3>(
        "RESOURCE.BUDGET_EXCEEDED", "sbdpv_decode_budget", ResourceStatus());
  }
  if (Cancelled(control)) {
    return Failure<TimestampViewResultV3>(
        "PROCESS.CANCELLED", "sbdpv_before_publication");
  }
  auto result = Success<TimestampViewResultV3>();
  result.value = {&profile, decoded.value.state, decoded.value.civil_day,
                  decoded.value.nanoseconds_since_midnight};
  return result;
}

TimestampBytesResultV3 EncodeTimestampSbdpvComposedV3(
    const TimestampOwnedValueV3& value, bool null_allowed,
    const TimestampExecutionControlV3& control) noexcept {
  const auto checked = ValidateTimestampValueViewV3(value.view(), null_allowed);
  if (!checked.ok()) {
    return Failure<TimestampBytesResultV3>(
        checked.diagnostic.diagnostic_code, checked.diagnostic.detail,
        checked.status);
  }
  auto profile_pin=value.profile;
  const std::size_t payload =
      value.state == TimestampValueStateV3::value ? 16 : 0;
  const std::size_t total = 24 + payload;
  if (total > control.maximum_allocation_bytes) {
    return Failure<TimestampBytesResultV3>(
        "RESOURCE.BUDGET_EXCEEDED", "sbdpv_capacity", ResourceStatus());
  }
  if (Cancelled(control)) {
    return Failure<TimestampBytesResultV3>(
        "PROCESS.CANCELLED", "before_allocation");
  }

  std::array<byte, 16> component{};
  ScopedClear clear_component(
      component.data(), component.size(),
      TimestampScrubClassV3::sbdpv_component_staging,
      control.observe_scrubbed, control.scrub_observer_context);
  if (payload != 0)
    EncodeComponent(value.civil_day, value.nanoseconds_since_midnight,
                    component.data());

  auto result = Success<TimestampBytesResultV3>();
  ScopedVectorClear clear_result(
      &result.bytes, TimestampScrubClassV3::sbdpv_owned_buffer,
      control.observe_scrubbed, control.scrub_observer_context);
  try {
    result.bytes.resize(total);
  } catch (...) {
    return Failure<TimestampBytesResultV3>(
        "RESOURCE.BUDGET_EXCEEDED", "sbdpv_allocation", ResourceStatus());
  }
  const DatatypePhysicalValueView structural{
      CanonicalTypeId::timestamp,
      value.state == TimestampValueStateV3::sql_null
          ? DatatypePhysicalValueState::sql_null
          : DatatypePhysicalValueState::value,
      payload != 0 ? component.data() : nullptr, payload};
  const auto encoded = EncodeDatatypePhysicalStructuralValueIntoNoAlloc(
      structural, result.bytes.data(), result.bytes.size());
  if (!encoded.ok() || encoded.bytes_written != total) {
    return Failure<TimestampBytesResultV3>(
        encoded.diagnostic.diagnostic_code, "sbdpv_encode", encoded.status);
  }
  TimestampExecutionControlV3 recheck_control;
  recheck_control.observe_scrubbed=control.observe_scrubbed;
  recheck_control.scrub_observer_context=control.scrub_observer_context;
  const auto recheck = DecodeTimestampSbdpvComposedNoAllocV3(
      *profile_pin, null_allowed, result.bytes, recheck_control);
  if (!recheck.ok()) {
    return Failure<TimestampBytesResultV3>(
        recheck.diagnostic.diagnostic_code, "sbdpv_recheck", recheck.status);
  }
  if (Cancelled(control)) {
    return Failure<TimestampBytesResultV3>(
        "PROCESS.CANCELLED", "before_publication");
  }
  clear_result.Disarm();
  return result;
}

DiagnosticRecord MakeTimestampDiagnosticV3(Status status,std::string diagnostic_code,std::string message_key,std::string detail){return MakeDatatypeOperationDiagnostic(status,std::move(diagnostic_code),std::move(message_key),std::move(detail));}

}  // namespace scratchbird::core::datatypes

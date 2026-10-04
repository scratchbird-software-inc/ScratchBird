// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0

#include "datatype_date.hpp"

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
#include <type_traits>

namespace scratchbird::core::datatypes {
namespace {

using platform::LoadLittle32;
using platform::LoadLittle16;
using platform::LoadLittle64;
using platform::Severity;
using platform::StatusCode;
using platform::StoreLittle16;
using platform::StoreLittle32;
using platform::StoreLittle64;
using platform::Subsystem;

constexpr platform::Uuid U(std::array<byte, 16> bytes) noexcept {
  return platform::Uuid{bytes};
}

inline constexpr platform::Uuid kSnapshot = U(
    {0x01,0x9d,0x00,0x00,0x00,0x00,0x70,0x00,0x80,0x00,0x00,0x00,0x00,0x00,0xd7,0x09});
inline constexpr platform::Uuid kDescriptor = U(
    {0x90,0x01,0x00,0x00,0x64,0x61,0x74,0x65,0x80,0x00,0x00,0x00,0x00,0x00,0x00,0x00});
inline constexpr platform::Uuid kType = U(
    {0x01,0x9d,0x00,0x00,0x00,0x00,0x70,0x00,0x80,0x00,0x00,0x00,0x00,0x00,0xd8,0x1c});
inline constexpr platform::Uuid kCodec = U(
    {0x01,0x9d,0x00,0x00,0x00,0x00,0x70,0x00,0x80,0x00,0x00,0x00,0x00,0x00,0xd8,0x1d});

inline constexpr DatatypePolicyIdentityV3 kDescriptorPolicy{U(
    {0x01,0xa1,0x00,0x8e,0xb7,0xf0,0x79,0x13,0x9a,0x16,0x00,0x04,0x09,0xf9,0xff,0xb1}),1};
inline constexpr DatatypePolicyIdentityV3 kCanonicalPolicy{U(
    {0x01,0xa1,0x00,0x8e,0xb7,0xf1,0x72,0xb9,0xba,0x08,0x95,0xb6,0x04,0xca,0xeb,0xf3}),1};
inline constexpr DatatypePolicyIdentityV3 kOrderingPolicy{U(
    {0x01,0xa1,0x00,0x8e,0xb7,0xf2,0x7f,0xeb,0x85,0xa8,0x2d,0x74,0xfe,0x2f,0xde,0x38}),1};
inline constexpr DatatypePolicyIdentityV3 kHashPolicy{U(
    {0x01,0xa1,0x00,0x8e,0xb7,0xf3,0x7a,0x61,0xa1,0x59,0x5d,0xdb,0x1c,0xbc,0x1d,0xf6}),1};
inline constexpr DatatypePolicyIdentityV3 kRenderPolicy{U(
    {0x01,0xa1,0x00,0x8e,0xb7,0xf4,0x73,0xf2,0x9e,0x43,0x9d,0xa0,0x1d,0x98,0xe4,0x28}),1};
inline constexpr DatatypePolicyIdentityV3 kCastPolicy{U(
    {0x01,0xa1,0x00,0x8e,0xb7,0xf5,0x78,0xdf,0x9d,0xba,0xd8,0x2a,0x29,0x55,0x5e,0x94}),1};
inline constexpr DatatypePolicyIdentityV3 kOperationPolicy{U(
    {0x01,0xa1,0x00,0x8e,0xb7,0xf6,0x7b,0x7a,0x8e,0xe0,0xff,0xba,0xcb,0xcd,0x7d,0xff}),1};
inline constexpr DatatypePolicyIdentityV3 kCalendarPolicy{U(
    {0x01,0xa1,0x00,0x8e,0xb7,0xf7,0x7c,0x8d,0xa9,0xca,0x66,0x0c,0x34,0x10,0xda,0x57}),1};
inline constexpr DatatypePolicyIdentityV3 kStorageEpochPolicy{U(
    {0x01,0xa1,0x00,0x8e,0xb7,0xf9,0x7f,0xb8,0xad,0xc7,0x08,0xa5,0xf6,0x83,0x81,0xa5}),1};
inline constexpr DatatypePolicyIdentityV3 kTimezoneNonePolicy{U(
    {0x01,0xa1,0x00,0x8e,0xb7,0xfa,0x74,0x88,0x8b,0x57,0xc0,0x7c,0x21,0xb7,0x11,0x5a}),1};
inline constexpr DatatypePolicyIdentityV3 kLeapNaPolicy{U(
    {0x01,0xa1,0x00,0x8e,0xb7,0xfb,0x78,0xa5,0xb7,0x57,0x23,0xfa,0x8f,0x0c,0xaa,0x90}),1};
inline constexpr DatatypePolicyIdentityV3 kIndexPolicy{U(
    {0x01,0xa1,0x00,0x8e,0xb7,0xfc,0x70,0x50,0xa2,0xd5,0x69,0x3c,0xf0,0xef,0x4f,0x27}),1};
inline constexpr DatatypePolicyIdentityV3 kStatisticsPolicy{U(
    {0x01,0xa1,0x00,0x8e,0xb7,0xfd,0x72,0x2e,0x94,0x1e,0xc5,0x39,0x7e,0x9d,0x61,0xbc}),1};
inline constexpr DatatypePolicyIdentityV3 kBackupPolicy{U(
    {0x01,0xa1,0x00,0x8e,0xb7,0xfe,0x7f,0x46,0xac,0x7f,0x04,0xa7,0xd6,0x04,0x1b,0x34}),1};
inline constexpr DatatypePolicyIdentityV3 kProtectionPolicy{U(
    {0x01,0xa1,0x00,0x8e,0xb7,0xff,0x71,0x3c,0xb2,0xb9,0xad,0x24,0x4c,0xd3,0x03,0x9c}),1};
inline constexpr DatatypePolicyIdentityV3 kComponentPolicy{U(
    {0x01,0xa1,0x00,0x8e,0xb8,0x00,0x77,0xdb,0x95,0x3f,0x86,0xc9,0xa0,0xf2,0x35,0xa9}),1};
inline constexpr DatatypePolicyIdentityV3 kDiagnosticPolicy{U(
    {0x01,0xa1,0x00,0x8e,0xb8,0x03,0x71,0x11,0xb3,0x61,0x45,0x0d,0x04,0xde,0x40,0x27}),1};
inline constexpr DatatypePolicyIdentityV3 kMetricPolicy{U(
    {0x01,0xa1,0x00,0x8e,0xb8,0x04,0x74,0x9f,0xab,0x09,0xbd,0xde,0xe3,0x6e,0xe5,0xef}),1};

inline constexpr std::array<byte, 32> kProfileFingerprint{{
    0xfc,0x6e,0x4b,0xb3,0xa6,0xcc,0xdb,0x34,0x76,0x7a,0xb6,0xc8,0x03,0xa8,0x83,0x81,
    0x69,0xb8,0xd6,0x40,0x2c,0x33,0xe2,0xca,0xcc,0xe7,0xdb,0x5f,0x15,0x61,0xd6,0xc7}};
inline constexpr std::array<byte, 32> kComparisonFingerprint{{
    0xda,0x43,0xf7,0x4c,0xbf,0x04,0x0c,0x5c,0xc7,0x34,0xf8,0x3d,0x1f,0xa4,0x60,0x88,
    0x6c,0x14,0xe5,0xab,0x0e,0xaf,0x98,0x88,0x69,0x8d,0x18,0x5d,0x4e,0x6f,0xfa,0xc9}};

constexpr bool Same(const DatatypePolicyIdentityV3& left,
                    const DatatypePolicyIdentityV3& right) noexcept {
  return left.uuid == right.uuid && left.generation == right.generation;
}

bool IsNil(const platform::Uuid& uuid) noexcept {
  return std::all_of(uuid.bytes.begin(), uuid.bytes.end(),
                     [](byte value) { return value == 0; });
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

bool EqualLegacyIdentityIgnoringName(
    const DatatypeTypeCodecIdentityRowV1& left,
    const DatatypeTypeCodecIdentityRowV1& right) noexcept {
  return left.catalog_snapshot_uuid == right.catalog_snapshot_uuid &&
      left.catalog_generation == right.catalog_generation &&
      left.registry_generation == right.registry_generation &&
      left.descriptor_uuid == right.descriptor_uuid &&
      left.descriptor_generation == right.descriptor_generation &&
      left.type_uuid == right.type_uuid &&
      left.type_generation == right.type_generation &&
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
      left.codec_uuid == right.codec_uuid &&
      left.canonical_value_variable_width == right.canonical_value_variable_width &&
      left.canonical_value_exact_zero_is_width_marker ==
          right.canonical_value_exact_zero_is_width_marker &&
      left.canonical_byte_order == right.canonical_byte_order &&
      left.canonical_representation == right.canonical_representation &&
      left.canonical_charset == right.canonical_charset &&
      left.shortest_form_utf8_required == right.shortest_form_utf8_required &&
      left.implicit_normalization_allowed == right.implicit_normalization_allowed &&
      left.descriptor_bound_collation_required == right.descriptor_bound_collation_required &&
      left.empty_value_distinct_from_sql_null == right.empty_value_distinct_from_sql_null &&
      left.sql_null_requires_zero_payload == right.sql_null_requires_zero_payload &&
      left.variable_width_storage_without_truncation ==
          right.variable_width_storage_without_truncation &&
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

bool EqualV3IdentityIgnoringName(const DatatypeTypeCodecIdentityRowV3& left,
                                 const DatatypeTypeCodecIdentityRowV3& right) noexcept {
  return EqualLegacyIdentityIgnoringName(left.legacy_fields, right.legacy_fields) &&
      Same(left.descriptor_policy, right.descriptor_policy) &&
      Same(left.canonicalization_policy, right.canonicalization_policy) &&
      Same(left.ordering_policy, right.ordering_policy) &&
      Same(left.hash_policy, right.hash_policy) &&
      Same(left.operation_policy, right.operation_policy);
}

const DatatypeTypeCodecIdentityRowV3* CurrentIdentityFor(
    CanonicalTypeId type_id) noexcept {
  const auto code = static_cast<u32>(type_id);
  const DatatypeTypeCodecIdentityRowV3* match = nullptr;
  for (const auto& row : CurrentDatatypeTypeCodecIdentityRowsV3()) {
    const auto& legacy = row.legacy_fields;
    if (legacy.catalog_snapshot_uuid != kSnapshot ||
        legacy.catalog_generation != 9 || legacy.registry_generation != 9 ||
        legacy.canonical_binary_type_code != code) continue;
    if (match != nullptr) return nullptr;
    match = &row;
  }
  return match;
}

template <typename Result>
Result Success() noexcept {
  Result result;
  result.status = OkStatus();
  result.diagnostic.status = result.status;
  return result;
}

bool Cancelled(const DateExecutionControlV3& control) noexcept {
  return control.cancelled != nullptr &&
         control.cancelled(control.cancellation_context);
}

void SecureClearBytes(void* pointer, std::size_t bytes) noexcept {
  auto* output = static_cast<volatile unsigned char*>(pointer);
  while (bytes != 0) {
    *output++ = 0;
    --bytes;
  }
}

class ScopedSecureClear final {
 public:
  ScopedSecureClear(void* pointer, std::size_t bytes,
                    DateScrubClassV3 scrub_class = DateScrubClassV3::count,
                    void (*observer)(void*, DateScrubClassV3, const byte*, u64) noexcept = nullptr,
                    void* observer_context = nullptr) noexcept
      : pointer_(pointer), bytes_(bytes), scrub_class_(scrub_class),
        observer_(observer), observer_context_(observer_context) {}
  ScopedSecureClear(const ScopedSecureClear&) = delete;
  ScopedSecureClear& operator=(const ScopedSecureClear&) = delete;
  ~ScopedSecureClear() {
    SecureClearBytes(pointer_, bytes_);
    if (observer_ != nullptr && scrub_class_ != DateScrubClassV3::count)
      observer_(observer_context_, scrub_class_,
                static_cast<const byte*>(pointer_), bytes_);
  }

 private:
  void* pointer_;
  std::size_t bytes_;
  DateScrubClassV3 scrub_class_;
  void (*observer_)(void*, DateScrubClassV3, const byte*, u64) noexcept;
  void* observer_context_;
};

template <typename T>
class ScopedVectorSecureClear final {
 public:
  explicit ScopedVectorSecureClear(
      std::vector<T>* value,
      DateScrubClassV3 scrub_class = DateScrubClassV3::count,
      void (*observer)(void*, DateScrubClassV3, const byte*, u64) noexcept = nullptr,
      void* observer_context = nullptr) noexcept
      : value_(value), scrub_class_(scrub_class), observer_(observer),
        observer_context_(observer_context) {}
  ScopedVectorSecureClear(const ScopedVectorSecureClear&) = delete;
  ScopedVectorSecureClear& operator=(const ScopedVectorSecureClear&) = delete;
  ~ScopedVectorSecureClear() {
    if (value_ != nullptr && !value_->empty()) {
      const auto extent = value_->size() * sizeof(T);
      SecureClearBytes(value_->data(), extent);
      if (observer_ != nullptr && scrub_class_ != DateScrubClassV3::count)
        observer_(observer_context_, scrub_class_,
                  reinterpret_cast<const byte*>(value_->data()), extent);
    }
  }
  void Disarm() noexcept { value_ = nullptr; }

 private:
  std::vector<T>* value_;
  DateScrubClassV3 scrub_class_;
  void (*observer_)(void*, DateScrubClassV3, const byte*, u64) noexcept;
  void* observer_context_;
};

class ScopedStringSecureClear final {
 public:
  explicit ScopedStringSecureClear(
      std::string* value,
      DateScrubClassV3 scrub_class = DateScrubClassV3::count,
      void (*observer)(void*, DateScrubClassV3, const byte*, u64) noexcept = nullptr,
      void* observer_context = nullptr) noexcept
      : value_(value), scrub_class_(scrub_class), observer_(observer),
        observer_context_(observer_context) {}
  ScopedStringSecureClear(const ScopedStringSecureClear&) = delete;
  ScopedStringSecureClear& operator=(const ScopedStringSecureClear&) = delete;
  ~ScopedStringSecureClear() {
    if (value_ != nullptr && !value_->empty()) {
      SecureClearBytes(value_->data(), value_->size());
      if (observer_ != nullptr && scrub_class_ != DateScrubClassV3::count)
        observer_(observer_context_, scrub_class_,
                  reinterpret_cast<const byte*>(value_->data()), value_->size());
    }
  }
  void Disarm() noexcept { value_ = nullptr; }

 private:
  std::string* value_;
  DateScrubClassV3 scrub_class_;
  void (*observer_)(void*, DateScrubClassV3, const byte*, u64) noexcept;
  void* observer_context_;
};

class ScopedOwnedDateValueClear final {
 public:
  explicit ScopedOwnedDateValueClear(DateOwnedValueV3* value) noexcept
      : value_(value) {}
  ScopedOwnedDateValueClear(const ScopedOwnedDateValueClear&) = delete;
  ScopedOwnedDateValueClear& operator=(const ScopedOwnedDateValueClear&) = delete;
  ~ScopedOwnedDateValueClear() {
    if (value_ == nullptr) return;
    value_->day = 0;
    value_->state = DateValueStateV3::sql_null;
    value_->profile.reset();
  }
  void Disarm() noexcept { value_ = nullptr; }

 private:
  DateOwnedValueV3* value_;
};

class ScopedDateValueViewClear final {
 public:
  explicit ScopedDateValueViewClear(DateValueViewV3* value) noexcept
      : value_(value) {}
  ~ScopedDateValueViewClear(){
    if(!value_)return;
    value_->day=0;value_->state=DateValueStateV3::sql_null;value_->profile=nullptr;
  }
 private:DateValueViewV3* value_;
};

bool RangesOverlap(const void* left, std::size_t left_bytes,
                   const void* right, std::size_t right_bytes) noexcept {
  if (left == nullptr || right == nullptr || left_bytes == 0 || right_bytes == 0)
    return false;
  const auto l = reinterpret_cast<std::uintptr_t>(left);
  const auto r = reinterpret_cast<std::uintptr_t>(right);
  return l <= r ? r - l < left_bytes : l - r < right_bytes;
}

bool OutputOverlapsString(const void* output, std::size_t output_bytes,
                          const std::string& value) noexcept {
  return !value.empty() &&
      RangesOverlap(output, output_bytes, value.data(), value.size());
}

bool OutputOverlapsIdentityBuffers(
    const void* output, std::size_t output_bytes,
    const DatatypeTypeCodecIdentityRowV3& identity) noexcept {
  const auto& row = identity.legacy_fields;
  return OutputOverlapsString(output, output_bytes, row.codec_id) ||
      OutputOverlapsString(output, output_bytes, row.canonical_name) ||
      OutputOverlapsString(output, output_bytes, row.canonical_byte_order) ||
      OutputOverlapsString(output, output_bytes, row.canonical_representation) ||
      OutputOverlapsString(output, output_bytes, row.canonical_charset) ||
      OutputOverlapsString(output, output_bytes,
                           row.invalid_encoding_diagnostic_id) ||
      OutputOverlapsString(output, output_bytes, row.comparison_profile);
}

bool OutputOverlapsDescriptorBuffers(
    const void* output, std::size_t output_bytes,
    const scratchbird::engine::ExecutionTypeDescriptor& descriptor) noexcept {
  return OutputOverlapsString(output, output_bytes, descriptor.stable_name) ||
      (!descriptor.domain_stack.empty() &&
       RangesOverlap(output, output_bytes, descriptor.domain_stack.data(),
                     descriptor.domain_stack.size() *
                         sizeof(descriptor.domain_stack.front())));
}

bool OutputOverlapsProfile(const void* output, std::size_t output_bytes,
                           const DateValidatedProfileHandleV3& profile) noexcept {
  return RangesOverlap(output, output_bytes, &profile, sizeof(profile)) ||
      OutputOverlapsIdentityBuffers(output, output_bytes, profile.identity);
}

bool OutputOverlapsDateInput(const DateValueViewV3& value,
                             const void* output,
                             std::size_t output_bytes) noexcept {
  return RangesOverlap(output, output_bytes, &value, sizeof(value)) ||
      (value.profile != nullptr &&
       OutputOverlapsProfile(output, output_bytes, *value.profile));
}

bool OutputOverlapsOwnedDateInput(const DateOwnedValueV3& value,
                                  const void* output,
                                  std::size_t output_bytes) noexcept {
  return RangesOverlap(output, output_bytes, &value, sizeof(value)) ||
      RangesOverlap(output, output_bytes, &value.profile,
                    sizeof(value.profile)) ||
      (value.profile != nullptr &&
       OutputOverlapsProfile(output, output_bytes, *value.profile));
}

bool OutputOverlapsDateOperand(const DateOperandV3& value,
                               const void* output,
                               std::size_t output_bytes) noexcept {
  return RangesOverlap(output, output_bytes, &value, sizeof(value)) ||
      RangesOverlap(output, output_bytes, &value.profile,
                    sizeof(value.profile)) ||
      (value.profile != nullptr &&
       OutputOverlapsProfile(output, output_bytes, *value.profile));
}

void PutUuid(byte* output, const platform::Uuid& uuid) noexcept {
  std::memcpy(output, uuid.bytes.data(), uuid.bytes.size());
}

template <typename PolicyIdentity>
void PutPolicy(byte* output, const PolicyIdentity& policy) noexcept {
  PutUuid(output, policy.uuid);
  StoreLittle64(output + 16, policy.generation);
}

bool Digest(std::span<const byte> material, std::array<byte, 32>* output,
            const DateExecutionControlV3* control = nullptr) noexcept {
  if (output == nullptr) return false;
  const auto observer = control == nullptr ? nullptr : control->observe_scrubbed;
  void* observer_context =
      control == nullptr ? nullptr : control->scrub_observer_context;
  std::array<u32, 8> state{{0x6a09e667u,0xbb67ae85u,0x3c6ef372u,0xa54ff53au,
                            0x510e527fu,0x9b05688cu,0x1f83d9abu,0x5be0cd19u}};
  ScopedSecureClear clear_state(state.data(), state.size() * sizeof(u32),
      DateScrubClassV3::sha_state, observer, observer_context);
  constexpr std::array<u32, 64> constants{{
      0x428a2f98u,0x71374491u,0xb5c0fbcfu,0xe9b5dba5u,0x3956c25bu,0x59f111f1u,0x923f82a4u,0xab1c5ed5u,
      0xd807aa98u,0x12835b01u,0x243185beu,0x550c7dc3u,0x72be5d74u,0x80deb1feu,0x9bdc06a7u,0xc19bf174u,
      0xe49b69c1u,0xefbe4786u,0x0fc19dc6u,0x240ca1ccu,0x2de92c6fu,0x4a7484aau,0x5cb0a9dcu,0x76f988dau,
      0x983e5152u,0xa831c66du,0xb00327c8u,0xbf597fc7u,0xc6e00bf3u,0xd5a79147u,0x06ca6351u,0x14292967u,
      0x27b70a85u,0x2e1b2138u,0x4d2c6dfcu,0x53380d13u,0x650a7354u,0x766a0abbu,0x81c2c92eu,0x92722c85u,
      0xa2bfe8a1u,0xa81a664bu,0xc24b8b70u,0xc76c51a3u,0xd192e819u,0xd6990624u,0xf40e3585u,0x106aa070u,
      0x19a4c116u,0x1e376c08u,0x2748774cu,0x34b0bcb5u,0x391c0cb3u,0x4ed8aa4au,0x5b9cca4fu,0x682e6ff3u,
      0x748f82eeu,0x78a5636fu,0x84c87814u,0x8cc70208u,0x90befffau,0xa4506cebu,0xbef9a3f7u,0xc67178f2u}};
  const auto rotate = [](u32 value, unsigned bits) noexcept {
    return static_cast<u32>((value >> bits) | (value << (32 - bits)));
  };
  const auto transform = [&](const byte* block) noexcept {
    std::array<u32, 64> words{};
    ScopedSecureClear clear_words(words.data(), words.size() * sizeof(u32),
        DateScrubClassV3::sha_schedule, observer, observer_context);
    for (unsigned i = 0; i < 16; ++i)
      words[i] = (static_cast<u32>(block[i * 4]) << 24) |
          (static_cast<u32>(block[i * 4 + 1]) << 16) |
          (static_cast<u32>(block[i * 4 + 2]) << 8) | block[i * 4 + 3];
    for (unsigned i = 16; i < 64; ++i) {
      const u32 s0 = rotate(words[i - 15], 7) ^
          rotate(words[i - 15], 18) ^ (words[i - 15] >> 3);
      const u32 s1 = rotate(words[i - 2], 17) ^
          rotate(words[i - 2], 19) ^ (words[i - 2] >> 10);
      words[i] = words[i - 16] + s0 + words[i - 7] + s1;
    }
    u32 a=state[0],b=state[1],c=state[2],d=state[3];
    u32 e=state[4],f=state[5],g=state[6],h=state[7];
    for (unsigned i = 0; i < 64; ++i) {
      const u32 s1 = rotate(e,6)^rotate(e,11)^rotate(e,25);
      const u32 choose=(e&f)^((~e)&g);
      const u32 first=h+s1+choose+constants[i]+words[i];
      const u32 s0=rotate(a,2)^rotate(a,13)^rotate(a,22);
      const u32 majority=(a&b)^(a&c)^(b&c);
      const u32 second=s0+majority;
      h=g;g=f;f=e;e=d+first;d=c;c=b;b=a;a=first+second;
    }
    state[0]+=a;state[1]+=b;state[2]+=c;state[3]+=d;
    state[4]+=e;state[5]+=f;state[6]+=g;state[7]+=h;
  };
  std::size_t offset = 0;
  while (material.size() - offset >= 64) {
    transform(material.data() + offset);
    offset += 64;
  }
  std::array<byte, 128> tail{};
  ScopedSecureClear clear_tail(tail.data(), tail.size(),
      DateScrubClassV3::sha_tail, observer, observer_context);
  const std::size_t remainder = material.size() - offset;
  if (remainder != 0)
    std::memcpy(tail.data(), material.data() + offset, remainder);
  tail[remainder] = 0x80;
  const std::size_t padded = remainder < 56 ? 64 : 128;
  const u64 bits = static_cast<u64>(material.size()) * 8;
  for (unsigned i = 0; i < 8; ++i)
    tail[padded - 1 - i] = static_cast<byte>(bits >> (i * 8));
  transform(tail.data());
  if (padded == 128) transform(tail.data() + 64);
  for (unsigned i = 0; i < 8; ++i) {
    (*output)[i * 4] = static_cast<byte>(state[i] >> 24);
    (*output)[i * 4 + 1] = static_cast<byte>(state[i] >> 16);
    (*output)[i * 4 + 2] = static_cast<byte>(state[i] >> 8);
    (*output)[i * 4 + 3] = static_cast<byte>(state[i]);
  }
  return true;
}

DatatypeTypeCodecIdentityRowV3 ExpectedDateIdentity() {
  DatatypeTypeCodecIdentityRowV3 row;
  auto& legacy = row.legacy_fields;
  legacy.catalog_snapshot_uuid = kSnapshot;
  legacy.catalog_generation = 9;
  legacy.registry_generation = 9;
  legacy.descriptor_uuid = kDescriptor;
  legacy.descriptor_generation = 1;
  legacy.type_uuid = kType;
  legacy.type_generation = 1;
  legacy.codec_id = "datatype.date.days.le.v1";
  legacy.codec_version = 1;
  legacy.codec_generation = 1;
  legacy.canonical_value_bytes = 4;
  legacy.null_supported = true;
  legacy.canonical_name = "date";
  legacy.datatype_identity_code = 0;
  legacy.null_encoding_code = 1;
  legacy.byte_order_code = 0;
  legacy.signed_code = false;
  legacy.representation_code = 0;
  legacy.canonical_value_minimum_bytes = 4;
  legacy.canonical_value_maximum_bytes = 4;
  legacy.canonical_value_exact_bytes = 4;
  legacy.canonical_binary_type_code = static_cast<u32>(CanonicalTypeId::date);
  legacy.codec_uuid = kCodec;
  legacy.canonical_value_variable_width = false;
  legacy.canonical_value_exact_zero_is_width_marker = false;
  legacy.canonical_byte_order = "little_endian";
  legacy.canonical_representation = "signed_i32_days_since_Unix_epoch";
  legacy.canonical_charset.clear();
  legacy.shortest_form_utf8_required = false;
  legacy.implicit_normalization_allowed = false;
  legacy.descriptor_bound_collation_required = false;
  legacy.empty_value_distinct_from_sql_null = false;
  legacy.sql_null_requires_zero_payload = true;
  legacy.variable_width_storage_without_truncation = false;
  legacy.invalid_encoding_diagnostic_id = "DATATYPE.DESCRIPTOR.INVALID";
  row.descriptor_policy = DatatypePolicyIdentityV3{kDescriptorPolicy.uuid, kDescriptorPolicy.generation};
  row.canonicalization_policy = DatatypePolicyIdentityV3{kCanonicalPolicy.uuid, kCanonicalPolicy.generation};
  row.ordering_policy = DatatypePolicyIdentityV3{kOrderingPolicy.uuid, kOrderingPolicy.generation};
  row.hash_policy = DatatypePolicyIdentityV3{kHashPolicy.uuid, kHashPolicy.generation};
  row.operation_policy = DatatypePolicyIdentityV3{kOperationPolicy.uuid, kOperationPolicy.generation};
  return row;
}

bool ExactDateIdentity(const DatatypeTypeCodecIdentityRowV3& row) noexcept {
  const auto* expected = CurrentIdentityFor(CanonicalTypeId::date);
  return expected != nullptr && EqualV3IdentityIgnoringName(row, *expected);
}

bool ExactReceipt(const DateAuthorityReceiptV3& receipt) noexcept {
  return receipt.statement_receipt_uuid == kSnapshot &&
      receipt.catalog_snapshot_uuid == kSnapshot &&
      receipt.catalog_generation == 9 && receipt.registry_generation == 9;
}

std::array<byte, kDateProfileMaterialBytesV3> BuildProfileMaterial(
    const DateValidatedProfileHandleV3& profile) noexcept {
  std::array<byte, kDateProfileMaterialBytesV3> material{};
  const auto& row = profile.identity.legacy_fields;
  std::memcpy(material.data(), "SBDATP01", 8);
  StoreLittle16(material.data() + 8, 1);
  StoreLittle16(material.data() + 10, kDateProfileMaterialBytesV3);
  StoreLittle32(material.data() + 12, kDateProfileMaterialBytesV3);
  PutUuid(material.data() + 16, profile.receipt.catalog_snapshot_uuid);
  StoreLittle64(material.data() + 32, profile.receipt.catalog_generation);
  StoreLittle64(material.data() + 40, profile.receipt.registry_generation);
  PutUuid(material.data() + 48, row.descriptor_uuid);
  StoreLittle64(material.data() + 64, row.descriptor_generation);
  PutUuid(material.data() + 72, row.type_uuid);
  StoreLittle64(material.data() + 88, row.type_generation);
  PutUuid(material.data() + 96, row.codec_uuid);
  StoreLittle32(material.data() + 112, row.codec_version);
  StoreLittle64(material.data() + 120, row.codec_generation);
  PutPolicy(material.data() + 128, profile.identity.descriptor_policy);
  PutPolicy(material.data() + 152, profile.identity.canonicalization_policy);
  PutPolicy(material.data() + 176, profile.identity.ordering_policy);
  PutPolicy(material.data() + 200, profile.identity.hash_policy);
  PutPolicy(material.data() + 224, profile.identity.operation_policy);
  PutPolicy(material.data() + 248, profile.render_policy);
  PutPolicy(material.data() + 272, profile.cast_policy);
  PutPolicy(material.data() + 296, profile.calendar_policy);
  PutPolicy(material.data() + 320, profile.storage_epoch_policy);
  PutPolicy(material.data() + 344, profile.timezone_none_policy);
  PutPolicy(material.data() + 368, profile.leap_not_applicable_policy);
  PutPolicy(material.data() + 392, profile.index_policy);
  PutPolicy(material.data() + 416, profile.statistics_policy);
  PutPolicy(material.data() + 440, profile.backup_transport_policy);
  PutPolicy(material.data() + 464, profile.protection_policy);
  PutPolicy(material.data() + 488, profile.component_adapter_policy);
  PutPolicy(material.data() + 512, profile.diagnostic_policy);
  PutPolicy(material.data() + 536, profile.metric_policy);
  StoreLittle32(material.data() + 560, 0x80000000u);
  StoreLittle32(material.data() + 564, 0x7fffffffu);
  StoreLittle32(material.data() + 568, 0);
  StoreLittle32(material.data() + 572, 4);
  StoreLittle32(material.data() + 576, 1023);
  return material;
}

std::array<byte, kDateComparisonMaterialBytesV3> BuildComparisonMaterial(
    const DateValidatedProfileHandleV3& profile) noexcept {
  std::array<byte, kDateComparisonMaterialBytesV3> material{};
  const auto& row = profile.identity.legacy_fields;
  std::memcpy(material.data(), "SBDACC01", 8);
  StoreLittle16(material.data() + 8, 1);
  StoreLittle16(material.data() + 10, kDateComparisonMaterialBytesV3);
  PutUuid(material.data() + 16, profile.receipt.catalog_snapshot_uuid);
  StoreLittle64(material.data() + 32, profile.receipt.catalog_generation);
  StoreLittle64(material.data() + 40, profile.receipt.registry_generation);
  PutUuid(material.data() + 48, row.descriptor_uuid);
  StoreLittle64(material.data() + 64, row.descriptor_generation);
  PutUuid(material.data() + 72, row.type_uuid);
  StoreLittle64(material.data() + 88, row.type_generation);
  PutUuid(material.data() + 96, row.codec_uuid);
  StoreLittle32(material.data() + 112, row.codec_version);
  StoreLittle64(material.data() + 120, row.codec_generation);
  PutPolicy(material.data() + 128, profile.identity.descriptor_policy);
  PutPolicy(material.data() + 152, profile.identity.canonicalization_policy);
  PutPolicy(material.data() + 176, profile.identity.ordering_policy);
  PutPolicy(material.data() + 200, profile.calendar_policy);
  PutPolicy(material.data() + 224, profile.storage_epoch_policy);
  PutPolicy(material.data() + 248, profile.timezone_none_policy);
  PutPolicy(material.data() + 272, profile.leap_not_applicable_policy);
  PutPolicy(material.data() + 296, profile.identity.hash_policy);
  StoreLittle32(material.data() + 320, 1023);
  StoreLittle32(material.data() + 324, 0x80000000u);
  StoreLittle32(material.data() + 328, 0x7fffffffu);
  StoreLittle32(material.data() + 332, 0);
  StoreLittle32(material.data() + 336, 4);
  return material;
}

bool ProfileValidNoAlloc(const DateValidatedProfileHandleV3& profile,
                         const DateExecutionControlV3* control = nullptr) noexcept {
  if (!ExactReceipt(profile.receipt) || !ExactDateIdentity(profile.identity) ||
      profile.identity.legacy_fields.catalog_snapshot_uuid != profile.receipt.catalog_snapshot_uuid ||
      profile.identity.legacy_fields.catalog_generation != profile.receipt.catalog_generation ||
      profile.identity.legacy_fields.registry_generation != profile.receipt.registry_generation ||
      !Same(profile.render_policy, kRenderPolicy) ||
      !Same(profile.cast_policy, kCastPolicy) ||
      !Same(profile.calendar_policy, kCalendarPolicy) ||
      !Same(profile.storage_epoch_policy, kStorageEpochPolicy) ||
      !Same(profile.timezone_none_policy, kTimezoneNonePolicy) ||
      !Same(profile.leap_not_applicable_policy, kLeapNaPolicy) ||
      !Same(profile.index_policy, kIndexPolicy) ||
      !Same(profile.statistics_policy, kStatisticsPolicy) ||
      !Same(profile.backup_transport_policy, kBackupPolicy) ||
      !Same(profile.protection_policy, kProtectionPolicy) ||
      !Same(profile.component_adapter_policy, kComponentPolicy) ||
      !Same(profile.diagnostic_policy, kDiagnosticPolicy) ||
      !Same(profile.metric_policy, kMetricPolicy)) return false;
  auto profile_material = BuildProfileMaterial(profile);
  ScopedSecureClear clear_profile_material(
      profile_material.data(), profile_material.size(),
      DateScrubClassV3::profile_material,
      control == nullptr ? nullptr : control->observe_scrubbed,
      control == nullptr ? nullptr : control->scrub_observer_context);
  auto comparison_material = BuildComparisonMaterial(profile);
  ScopedSecureClear clear_comparison_material(
      comparison_material.data(), comparison_material.size(),
      DateScrubClassV3::comparison_material,
      control == nullptr ? nullptr : control->observe_scrubbed,
      control == nullptr ? nullptr : control->scrub_observer_context);
  std::array<byte, 32> profile_digest{}, comparison_digest{};
  ScopedSecureClear clear_profile_digest(
      profile_digest.data(), profile_digest.size(),
      DateScrubClassV3::profile_digest,
      control == nullptr ? nullptr : control->observe_scrubbed,
      control == nullptr ? nullptr : control->scrub_observer_context);
  ScopedSecureClear clear_comparison_digest(
      comparison_digest.data(), comparison_digest.size(),
      DateScrubClassV3::comparison_digest,
      control == nullptr ? nullptr : control->observe_scrubbed,
      control == nullptr ? nullptr : control->scrub_observer_context);
  return profile.profile_material == profile_material &&
      profile.comparison_material == comparison_material &&
      Digest(profile_material, &profile_digest, control) &&
      Digest(comparison_material, &comparison_digest, control) &&
      profile_digest == kProfileFingerprint &&
      comparison_digest == kComparisonFingerprint &&
      profile.profile_fingerprint == profile_digest &&
      profile.comparison_fingerprint == comparison_digest;
}

std::int64_t FloorDiv(std::int64_t value, std::int64_t divisor) noexcept {
  std::int64_t quotient = value / divisor;
  const std::int64_t remainder = value % divisor;
  if (remainder < 0) --quotient;
  return quotient;
}

std::int64_t FloorMod(std::int64_t value, std::int64_t divisor) noexcept {
  const std::int64_t remainder = value % divisor;
  return remainder < 0 ? remainder + divisor : remainder;
}

bool Leap(std::int64_t year) noexcept {
  return FloorMod(year, 4) == 0 &&
      (FloorMod(year, 100) != 0 || FloorMod(year, 400) == 0);
}

u8 DaysInMonth(std::int64_t year, u8 month) noexcept {
  static constexpr u8 days[] = {31,28,31,30,31,30,31,31,30,31,30,31};
  if (month == 2 && Leap(year)) return 29;
  return month >= 1 && month <= 12 ? days[month - 1] : 0;
}

using DateWideSigned = __int128_t;

DateWideSigned FloorDivWide(DateWideSigned value,
                            DateWideSigned divisor) noexcept {
  DateWideSigned quotient = value / divisor;
  const DateWideSigned remainder = value % divisor;
  if (remainder < 0) --quotient;
  return quotient;
}

bool CivilToDaysWide(std::int64_t year, u8 month, u8 day,
                     DateWideSigned* output) noexcept {
  if (month < 1 || month > 12 || day < 1 || day > DaysInMonth(year, month))
    return false;
  DateWideSigned adjusted_year = static_cast<DateWideSigned>(year);
  adjusted_year -= month <= 2 ? 1 : 0;
  const DateWideSigned era = FloorDivWide(adjusted_year, 400);
  const DateWideSigned year_of_era = adjusted_year - era * 400;
  const DateWideSigned month_prime = month + (month > 2 ? -3 : 9);
  const DateWideSigned day_of_year = (153 * month_prime + 2) / 5 + day - 1;
  const DateWideSigned day_of_era = year_of_era * 365 + year_of_era / 4 -
      year_of_era / 100 + day_of_year;
  *output = era * 146097 + day_of_era - 719468;
  return true;
}

bool CivilToDays(std::int64_t year, u8 month, u8 day,
                 std::int64_t* output) noexcept {
  DateWideSigned wide = 0;
  if (!CivilToDaysWide(year, month, day, &wide) ||
      wide < std::numeric_limits<std::int64_t>::min() ||
      wide > std::numeric_limits<std::int64_t>::max()) return false;
  *output = static_cast<std::int64_t>(wide);
  return true;
}

DateCivilV3 DaysToCivil(std::int64_t day) noexcept {
  std::int64_t z = day + 719468;
  const std::int64_t era = FloorDiv(z, 146097);
  const std::int64_t day_of_era = z - era * 146097;
  const std::int64_t year_of_era =
      (day_of_era - day_of_era / 1460 + day_of_era / 36524 -
       day_of_era / 146096) / 365;
  std::int64_t year = year_of_era + era * 400;
  const std::int64_t day_of_year = day_of_era -
      (365 * year_of_era + year_of_era / 4 - year_of_era / 100);
  const std::int64_t month_prime = (5 * day_of_year + 2) / 153;
  const u8 civil_day = static_cast<u8>(day_of_year -
      (153 * month_prime + 2) / 5 + 1);
  const u8 month = static_cast<u8>(month_prime + (month_prime < 10 ? 3 : -9));
  year += month <= 2;
  return {static_cast<std::int32_t>(year), month, civil_day};
}

bool ParseDigits(std::string_view text, std::int64_t* value) noexcept {
  if (text.empty() || !std::all_of(text.begin(), text.end(),
      [](char c) { return c >= '0' && c <= '9'; })) return false;
  std::int64_t parsed = 0;
  const auto converted = std::from_chars(text.data(), text.data() + text.size(), parsed);
  if (converted.ec != std::errc{} || converted.ptr != text.data() + text.size())
    return false;
  *value = parsed;
  return true;
}

bool EngineUuidEqual(const scratchbird::engine::Uuid& left,
                     const platform::Uuid& right) noexcept {
  return std::equal(std::begin(left.bytes), std::end(left.bytes),
                    right.bytes.begin());
}

bool EngineUuidEqual(const scratchbird::engine::Uuid& left,
                     const scratchbird::engine::Uuid& right) noexcept {
  return std::equal(std::begin(left.bytes), std::end(left.bytes),
                    std::begin(right.bytes));
}

bool EngineUuidNil(const scratchbird::engine::Uuid& value) noexcept {
  return std::all_of(std::begin(value.bytes), std::end(value.bytes),
                     [](byte octet) { return octet == 0; });
}

const DatatypeTypeCodecIdentityRowV3* CurrentIdentityFor(
    CanonicalTypeId type_id) noexcept;

struct DatePeerDescriptorShape {
  scratchbird::engine::ExecutionTypeFamily family;
  scratchbird::engine::ExecutionTypeWidthClass width;
  u32 bits;
  u32 precision;
  u32 scale;
};

bool DatePeerDescriptorShapeFor(CanonicalTypeId type,
                                DatePeerDescriptorShape* shape) noexcept {
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
  DatePeerDescriptorShape shape{};
  const auto* current = CurrentIdentityFor(expected_type);
  if (current == nullptr || !EqualV3IdentityIgnoringName(identity, *current) ||
      !DatePeerDescriptorShapeFor(expected_type, &shape) ||
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
      descriptor.width_class !=
          scratchbird::engine::ExecutionTypeWidthClass::unknown ||
      !descriptor.stable_name.empty() || descriptor.bit_width != 0 ||
      descriptor.precision != 0 || descriptor.scale != 0 ||
      descriptor.length != 0 || descriptor.vector_dimensions != 0 ||
      descriptor.container_rank != 0 || descriptor.modifier_flags != 0 ||
      !EngineUuidNil(descriptor.domain_uuid) ||
      !descriptor.domain_stack.empty() ||
      !EngineUuidNil(descriptor.charset_uuid) ||
      !EngineUuidNil(descriptor.collation_uuid) ||
      !EngineUuidNil(descriptor.timezone_uuid) ||
      !EngineUuidNil(descriptor.element_descriptor_uuid) ||
      !EngineUuidNil(descriptor.security_policy_uuid) ||
      !descriptor.nullable_allowed || !descriptor.descriptor_authoritative ||
      !descriptor.parser_independent;
}

bool ExactCharacterDescriptor(
    const scratchbird::engine::ExecutionTypeDescriptor& descriptor,
    const DateAuthorityReceiptV3& receipt) noexcept {
  const auto* identity = CurrentIdentityFor(CanonicalTypeId::character);
  return ExactReceipt(receipt) && identity != nullptr &&
      descriptor.length <= 16'777'216 &&
      DescriptorBindsIdentityNoAlloc(descriptor, *identity,
                                     CanonicalTypeId::character);
}

struct DateCastRowShape {
  bool incoming = false;
  bool contextual_null = false;
  bool exact_peer_identity = false;
  CanonicalTypeId peer_type = CanonicalTypeId::unknown;
};

bool ResolveDateCastRowShape(u32 row, DateCastRowShape* output) noexcept {
  if (output == nullptr || row == 0 || row > kDateClosedCastPolicyRowsV3)
    return false;
  if (row == 1) {
    *output = {true, true, false, CanonicalTypeId::null_type};
    return true;
  }
  static constexpr std::array<CanonicalTypeId, 29> kScalarThroughInterval{{
      CanonicalTypeId::boolean, CanonicalTypeId::int8, CanonicalTypeId::int16,
      CanonicalTypeId::int32, CanonicalTypeId::int64, CanonicalTypeId::int128,
      CanonicalTypeId::uint8, CanonicalTypeId::uint16, CanonicalTypeId::uint32,
      CanonicalTypeId::uint64, CanonicalTypeId::uint128,
      CanonicalTypeId::bfloat16, CanonicalTypeId::real16,
      CanonicalTypeId::real32, CanonicalTypeId::real64,
      CanonicalTypeId::real128, CanonicalTypeId::decimal,
      CanonicalTypeId::decimal_float, CanonicalTypeId::uuid,
      CanonicalTypeId::ip_address, CanonicalTypeId::network_prefix,
      CanonicalTypeId::mac_address, CanonicalTypeId::character,
      CanonicalTypeId::binary, CanonicalTypeId::bit_string,
      CanonicalTypeId::date, CanonicalTypeId::time,
      CanonicalTypeId::timestamp, CanonicalTypeId::interval}};
  static constexpr std::array<CanonicalTypeId, 56> kRemainingBaseTypes{{
      CanonicalTypeId::blob, CanonicalTypeId::document,
      CanonicalTypeId::json_document, CanonicalTypeId::binary_json_document,
      CanonicalTypeId::bson_document, CanonicalTypeId::xml_document,
      CanonicalTypeId::hstore_document, CanonicalTypeId::object_document,
      CanonicalTypeId::flattened_object_document, CanonicalTypeId::enum_value,
      CanonicalTypeId::set_value, CanonicalTypeId::array,
      CanonicalTypeId::list, CanonicalTypeId::map, CanonicalTypeId::row,
      CanonicalTypeId::composite, CanonicalTypeId::variant,
      CanonicalTypeId::range, CanonicalTypeId::multirange,
      CanonicalTypeId::token_stream, CanonicalTypeId::search_query,
      CanonicalTypeId::search_rank_feature,
      CanonicalTypeId::search_completion,
      CanonicalTypeId::search_percolator, CanonicalTypeId::geometry,
      CanonicalTypeId::geography, CanonicalTypeId::point,
      CanonicalTypeId::shape, CanonicalTypeId::raster,
      CanonicalTypeId::vector, CanonicalTypeId::dense_vector,
      CanonicalTypeId::sparse_vector, CanonicalTypeId::binary_vector,
      CanonicalTypeId::quantized_vector, CanonicalTypeId::graph_node,
      CanonicalTypeId::graph_edge, CanonicalTypeId::graph_path,
      CanonicalTypeId::time_series_value, CanonicalTypeId::columnar_segment,
      CanonicalTypeId::aggregate_state, CanonicalTypeId::hll_sketch,
      CanonicalTypeId::bloom_filter, CanonicalTypeId::quantile_sketch,
      CanonicalTypeId::histogram_sketch, CanonicalTypeId::ranking_summary,
      CanonicalTypeId::vector_summary, CanonicalTypeId::lob_locator,
      CanonicalTypeId::external_file_locator,
      CanonicalTypeId::remote_object_locator, CanonicalTypeId::bridge_handle,
      CanonicalTypeId::cursor_handle, CanonicalTypeId::system_reference,
      CanonicalTypeId::opaque_extension, CanonicalTypeId::cursor,
      CanonicalTypeId::result_set, CanonicalTypeId::table_value}};

  bool incoming = row >= 138;
  u32 outbound_row = incoming ? row - 84 : row;
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
  const auto* expected = CurrentIdentityFor(expected_type);
  return supplied != nullptr && expected != nullptr &&
      EqualV3IdentityIgnoringName(*supplied, *expected);
}

bool DescriptorBindsIdentity(
    const scratchbird::engine::ExecutionTypeDescriptor& descriptor,
    const DatatypeTypeCodecIdentityRowV3& identity,
    CanonicalTypeId expected_type) noexcept {
  return DescriptorBindsIdentityNoAlloc(descriptor, identity, expected_type);
}

struct ParsedCanonicalDateDayV3 {
  std::string_view diagnostic_code;
  std::string_view detail;
  std::int32_t day = 0;
  bool ok = false;
};

ParsedCanonicalDateDayV3 ParseCanonicalDateDayNoAlloc(
    std::string_view text) noexcept {
  std::int64_t year = 0, month = 0, day = 0;
  if (text.size() == 10) {
    if (text[4] != '-' || text[7] != '-' ||
        !ParseDigits(text.substr(0, 4), &year) ||
        !ParseDigits(text.substr(5, 2), &month) ||
        !ParseDigits(text.substr(8, 2), &day))
      return {"CTI.TEMPORAL.INVALID_LITERAL", "canonical_text_grammar"};
  } else if (text.size() == 14 && (text[0] == '+' || text[0] == '-')) {
    std::int64_t magnitude = 0;
    if (text[8] != '-' || text[11] != '-' ||
        !ParseDigits(text.substr(1, 7), &magnitude) ||
        !ParseDigits(text.substr(9, 2), &month) ||
        !ParseDigits(text.substr(12, 2), &day))
      return {"CTI.TEMPORAL.INVALID_LITERAL", "canonical_text_grammar"};
    year = text[0] == '-' ? -magnitude : magnitude;
    if (year >= 0 && year <= 9999)
      return {"CTI.TEMPORAL.INVALID_LITERAL", "expanded_four_digit_alias"};
  } else {
    return {"CTI.TEMPORAL.INVALID_LITERAL", "canonical_text_extent"};
  }
  if (month == 0 || day == 0)
    return {"CTI.TEMPORAL.ZERO_DATE_REFUSED", "zero_month_or_day"};
  if (month < 1 || month > 12 || day < 1 || day > 31)
    return {"CTI.TEMPORAL.INVALID_LITERAL", "civil_field_invalid"};
  DateWideSigned offset = 0;
  if (!CivilToDaysWide(year, static_cast<u8>(month), static_cast<u8>(day),
                       &offset))
    return {"CTI.TEMPORAL.INVALID_LITERAL", "civil_date_invalid"};
  if (offset < std::numeric_limits<std::int32_t>::min() ||
      offset > std::numeric_limits<std::int32_t>::max())
    return {"CTI.TEMPORAL.RANGE_EXCEEDED", "civil_date_out_of_range"};
  return {{}, {}, static_cast<std::int32_t>(offset), true};
}

}  // namespace

DateProfileResultV3 BuildCurrentDateValidatedProfileHandleV3(
    const platform::Uuid& statement_receipt_uuid) noexcept {
  try {
    return BuildDateValidatedProfileHandleV3(
        {statement_receipt_uuid, kSnapshot, 9, 9}, ExpectedDateIdentity());
  } catch (...) {
    return Failure<DateProfileResultV3>("RESOURCE.BUDGET_EXCEEDED",
                                        "profile_identity_allocation",
                                        ResourceStatus());
  }
}

DateProfileResultV3 BuildDateValidatedProfileHandleV3(
    const DateAuthorityReceiptV3& receipt,
    const DatatypeTypeCodecIdentityRowV3& identity) noexcept {
  if (!ExactReceipt(receipt) || !ExactDateIdentity(identity) ||
      identity.legacy_fields.catalog_snapshot_uuid != receipt.catalog_snapshot_uuid ||
      identity.legacy_fields.catalog_generation != receipt.catalog_generation ||
      identity.legacy_fields.registry_generation != receipt.registry_generation) {
    return Failure<DateProfileResultV3>("CTI.TEMPORAL.DESCRIPTOR_INVALID",
                                        "receipt_or_identity_invalid");
  }
  try {
    auto result = Success<DateProfileResultV3>();
    auto& profile = result.profile;
    profile.receipt = receipt;
    profile.identity = identity;
    profile.render_policy = kRenderPolicy;
    profile.cast_policy = kCastPolicy;
    profile.calendar_policy = kCalendarPolicy;
    profile.storage_epoch_policy = kStorageEpochPolicy;
    profile.timezone_none_policy = kTimezoneNonePolicy;
    profile.leap_not_applicable_policy = kLeapNaPolicy;
    profile.index_policy = kIndexPolicy;
    profile.statistics_policy = kStatisticsPolicy;
    profile.backup_transport_policy = kBackupPolicy;
    profile.protection_policy = kProtectionPolicy;
    profile.component_adapter_policy = kComponentPolicy;
    profile.diagnostic_policy = kDiagnosticPolicy;
    profile.metric_policy = kMetricPolicy;
    profile.profile_material = BuildProfileMaterial(profile);
    profile.comparison_material = BuildComparisonMaterial(profile);
    if (!Digest(profile.profile_material, &profile.profile_fingerprint) ||
        !Digest(profile.comparison_material, &profile.comparison_fingerprint)) {
      return Failure<DateProfileResultV3>("RESOURCE.BUDGET_EXCEEDED",
                                          "sha256_provider_failed", ResourceStatus());
    }
    if (!ProfileValidNoAlloc(profile))
      return Failure<DateProfileResultV3>("CTI.TEMPORAL.DESCRIPTOR_INVALID",
                                          "profile_fingerprint_mismatch");
    return result;
  } catch (...) {
    return Failure<DateProfileResultV3>("RESOURCE.BUDGET_EXCEEDED",
                                        "profile_allocation", ResourceStatus());
  }
}

DateValidationResultV3 ValidateDateProfileHandleV3(
    const DateValidatedProfileHandleV3& profile,
    const DateExecutionControlV3& control) noexcept {
  if (!ProfileValidNoAlloc(profile, &control))
    return Failure<DateValidationResultV3>("CTI.TEMPORAL.DESCRIPTOR_INVALID",
                                           "profile_invalid");
  if (Cancelled(control))
    return Failure<DateValidationResultV3>("PROCESS.CANCELLED",
                                           "before_publication");
  auto result = Success<DateValidationResultV3>();
  return result;
}

DateViewResultV3 ValidateDateValueViewV3(const DateValueViewV3& value,
                                        bool null_allowed) noexcept {
  if (value.profile == nullptr || !ProfileValidNoAlloc(*value.profile))
    return Failure<DateViewResultV3>("CTI.TEMPORAL.DESCRIPTOR_INVALID",
                                     "profile_missing_or_invalid");
  if (value.state != DateValueStateV3::value &&
      value.state != DateValueStateV3::sql_null)
    return Failure<DateViewResultV3>("DATATYPE.NULL_STATE.INVALID",
                                     "state_invalid");
  if (value.state == DateValueStateV3::sql_null) {
    if (value.day != 0)
      return Failure<DateViewResultV3>("DATATYPE.NULL_STATE.INVALID",
                                       "sql_null_day_nonzero");
    if (!null_allowed)
      return Failure<DateViewResultV3>("DATATYPE.NULL_NOT_ADMITTED",
                                       "slot_nonnullable");
  }
  auto result = Success<DateViewResultV3>();
  result.value = value;
  return result;
}

DateViewResultV3 AdmitDateOperandV3(const DateOperandV3& operand,
                                   bool null_allowed) noexcept {
  if (operand.profile == nullptr || !ProfileValidNoAlloc(*operand.profile))
    return Failure<DateViewResultV3>("CTI.TEMPORAL.DESCRIPTOR_INVALID",
                                     "operand_profile_missing_or_invalid");
  if (operand.state != DateValueStateV3::value &&
      operand.state != DateValueStateV3::sql_null)
    return Failure<DateViewResultV3>("DATATYPE.NULL_STATE.INVALID",
                                     "operand_state_invalid");
  if (operand.state == DateValueStateV3::sql_null) {
    if (operand.day != 0)
      return Failure<DateViewResultV3>("DATATYPE.NULL_STATE.INVALID",
                                       "operand_sql_null_day_nonzero");
    if (!null_allowed)
      return Failure<DateViewResultV3>("DATATYPE.NULL_NOT_ADMITTED",
                                       "operand_slot_nonnullable");
    auto result = Success<DateViewResultV3>();
    result.value = {operand.profile.get(), DateValueStateV3::sql_null, 0};
    return result;
  }
  if (operand.carrier != DateDayCarrierKindV3::signed_i32 ||
      operand.day < std::numeric_limits<std::int32_t>::min() ||
      operand.day > std::numeric_limits<std::int32_t>::max())
    return Failure<DateViewResultV3>(
        "CTI.TEMPORAL.CANONICAL_ENCODING_INVALID",
        "date_operand_not_host_i32");
  auto result = Success<DateViewResultV3>();
  result.value = {operand.profile.get(), DateValueStateV3::value,
                  static_cast<std::int32_t>(operand.day)};
  return result;
}

DateViewResultV3 DecodeCanonicalDateComponentNoAllocV3(
    const DateValidatedProfileHandleV3& profile, DateValueStateV3 state,
    bool null_allowed, std::span<const byte> component,
    const DateExecutionControlV3& control) noexcept {
  if (!ProfileValidNoAlloc(profile, &control))
    return Failure<DateViewResultV3>("CTI.TEMPORAL.DESCRIPTOR_INVALID",
                                     "component_profile_invalid");
  if (state != DateValueStateV3::value && state != DateValueStateV3::sql_null)
    return Failure<DateViewResultV3>("DATATYPE.NULL_STATE.INVALID",
                                     "component_state_invalid");
  if (state == DateValueStateV3::sql_null) {
    if (!component.empty())
      return Failure<DateViewResultV3>("DATATYPE.NULL_STATE.INVALID",
                                       "sql_null_component_nonempty");
    const auto validated = ValidateDateValueViewV3({&profile, state, 0}, null_allowed);
    if (!validated.ok()) return validated;
    if (Cancelled(control))
      return Failure<DateViewResultV3>("PROCESS.CANCELLED",
                                       "before_publication");
    return validated;
  }
  if (component.size() != 4)
    return Failure<DateViewResultV3>("CTI.TEMPORAL.CANONICAL_ENCODING_INVALID",
                                     "component_extent_invalid");
  const auto bits = LoadLittle32(component.data());
  const auto day = static_cast<std::int32_t>(bits);
  std::array<byte, 4> reencoded{};
  ScopedSecureClear clear_reencoded(
      reencoded.data(), reencoded.size(),
      DateScrubClassV3::component_decode_reencode,
      control.observe_scrubbed, control.scrub_observer_context);
  StoreLittle32(reencoded.data(), static_cast<u32>(day));
  if (control.force_reencode_mismatch_for_conformance)
    reencoded[0] ^= 1;
  if (reencoded != std::array<byte, 4>{component[0], component[1],
                                      component[2], component[3]})
    return Failure<DateViewResultV3>(
        "CTI.TEMPORAL.CANONICAL_ENCODING_INVALID", "component_reencode");
  const auto validated =
      ValidateDateValueViewV3({&profile, state, day}, null_allowed);
  if (!validated.ok()) return validated;
  if (Cancelled(control))
    return Failure<DateViewResultV3>("PROCESS.CANCELLED",
                                     "before_publication");
  return validated;
}

DateNoAllocWriteResultV3 EncodeCanonicalDateComponentIntoNoAllocV3(
    const DateOwnedValueV3& owned_value, byte* output, u64 output_capacity,
    const DateExecutionControlV3& control) noexcept {
  const auto value = owned_value.view();
  const auto checked = ValidateDateValueViewV3(value, true);
  if (!checked.ok())
    return Failure<DateNoAllocWriteResultV3>(checked.diagnostic.diagnostic_code,
                                             checked.diagnostic.detail,
                                             checked.status);
  auto result = Success<DateNoAllocWriteResultV3>();
  result.containing_null = value.state == DateValueStateV3::sql_null;
  result.bytes_required = result.containing_null ? 0 : 4;
  // The operation pin is acquired once the carrier and required extent are
  // known.  It therefore remains live across every publication refusal,
  // including alias, capacity, resource, and final cancellation.
  auto profile_pin = owned_value.profile;
  if (output != nullptr &&
      (OutputOverlapsOwnedDateInput(owned_value, output, result.bytes_required) ||
       RangesOverlap(output, result.bytes_required, &control,
                     sizeof(control))))
    return Failure<DateNoAllocWriteResultV3>(
        "CTI.TEMPORAL.CANONICAL_ENCODING_INVALID",
        "component_output_aliases_input_or_profile");
  if (result.bytes_required > output_capacity ||
      result.bytes_required > control.maximum_allocation_bytes ||
      (result.bytes_required != 0 && output == nullptr)) {
    auto failure = Failure<DateNoAllocWriteResultV3>(
        "RESOURCE.BUDGET_EXCEEDED", "component_capacity", ResourceStatus());
    failure.bytes_required = result.bytes_required;
    return failure;
  }
  std::array<byte, 4> staged{};
  ScopedSecureClear clear_staged(
      staged.data(), staged.size(), DateScrubClassV3::component_encode_staging,
      control.observe_scrubbed, control.scrub_observer_context);
  if (!result.containing_null)
    StoreLittle32(staged.data(), static_cast<u32>(value.day));
  if (Cancelled(control))
    return Failure<DateNoAllocWriteResultV3>("PROCESS.CANCELLED",
                                             "before_publication");
  if (!result.containing_null) {
    std::memcpy(output, staged.data(), staged.size());
    result.bytes_written = staged.size();
  }
  (void)profile_pin;
  return result;
}

DateBytesResultV3 EncodeCanonicalDateComponentV3(
    const DateOwnedValueV3& owned_value,
    const DateExecutionControlV3& control) noexcept {
  const auto value = owned_value.view();
  const auto checked = ValidateDateValueViewV3(value, true);
  if (!checked.ok())
    return Failure<DateBytesResultV3>(checked.diagnostic.diagnostic_code,
                                      checked.diagnostic.detail, checked.status);
  const u64 required = value.state == DateValueStateV3::sql_null ? 0 : 4;
  auto profile_pin = owned_value.profile;
  if (required > control.maximum_allocation_bytes)
    return Failure<DateBytesResultV3>("RESOURCE.BUDGET_EXCEEDED",
                                      "component_capacity", ResourceStatus());
  if (Cancelled(control))
    return Failure<DateBytesResultV3>("PROCESS.CANCELLED", "before_allocation");
  auto result = Success<DateBytesResultV3>();
  ScopedVectorSecureClear<byte> clear_result(
      &result.bytes, DateScrubClassV3::component_owned_buffer,
      control.observe_scrubbed, control.scrub_observer_context);
  try { result.bytes.resize(required); }
  catch (...) {
    return Failure<DateBytesResultV3>("RESOURCE.BUDGET_EXCEEDED",
                                      "component_allocation", ResourceStatus());
  }
  if (required != 0)
    StoreLittle32(result.bytes.data(), static_cast<u32>(value.day));
  if (Cancelled(control))
    return Failure<DateBytesResultV3>("PROCESS.CANCELLED",
                                      "before_publication");
  clear_result.Disarm();
  (void)profile_pin;
  return result;
}

DateValueResultV3 ConstructDateFromCivilV3(
    const std::shared_ptr<const DateValidatedProfileHandleV3>& profile, std::int64_t year,
    std::int64_t month, std::int64_t day, bool null_allowed) noexcept {
  return ConstructDateFromCivilV3(
      profile,
      {DateI64CarrierKindV3::signed_i64, DateValueStateV3::value, year},
      {DateI64CarrierKindV3::signed_i64, DateValueStateV3::value, month},
      {DateI64CarrierKindV3::signed_i64, DateValueStateV3::value, day},
      null_allowed);
}

DateValueResultV3 ConstructDateFromCivilV3(
    const std::shared_ptr<const DateValidatedProfileHandleV3>& profile,
    const DateNullableI64FactV3& year,
    const DateNullableI64FactV3& month,
    const DateNullableI64FactV3& day,
    bool null_allowed) noexcept {
  if (profile == nullptr || !ProfileValidNoAlloc(*profile))
    return Failure<DateValueResultV3>("CTI.TEMPORAL.DESCRIPTOR_INVALID",
                                      "civil_profile_invalid");
  const DateNullableI64FactV3* facts[] = {&year, &month, &day};
  for (const auto* fact : facts) {
    if (fact->state != DateValueStateV3::value &&
        fact->state != DateValueStateV3::sql_null)
      return Failure<DateValueResultV3>("DATATYPE.NULL_STATE.INVALID",
                                        "civil_component_state_invalid");
    if (fact->state == DateValueStateV3::sql_null &&
        (fact->carrier != DateI64CarrierKindV3::signed_i64 ||
         fact->value != 0))
      return Failure<DateValueResultV3>("DATATYPE.NULL_STATE.INVALID",
                                        "civil_component_dirty_null");
  }
  if (year.state == DateValueStateV3::sql_null ||
      month.state == DateValueStateV3::sql_null ||
      day.state == DateValueStateV3::sql_null) {
    if (!null_allowed)
      return Failure<DateValueResultV3>("DATATYPE.NULL_NOT_ADMITTED",
                                        "civil_result_nonnullable");
    auto result = Success<DateValueResultV3>();
    result.value = {profile, DateValueStateV3::sql_null, 0};
    return result;
  }
  for (const auto* fact : facts) {
    if (fact->carrier != DateI64CarrierKindV3::signed_i64)
      return Failure<DateValueResultV3>(
          "CTI.TEMPORAL.CANONICAL_ENCODING_INVALID",
          "civil_component_not_signed_i64");
  }
  if (month.value == 0 || day.value == 0)
    return Failure<DateValueResultV3>("CTI.TEMPORAL.ZERO_DATE_REFUSED",
                                      "zero_month_or_day");
  if (month.value < 1 || month.value > 12 || day.value < 1 || day.value > 31)
    return Failure<DateValueResultV3>("CTI.TEMPORAL.INVALID_LITERAL",
                                      "civil_field_invalid");
  DateWideSigned offset = 0;
  if (!CivilToDaysWide(year.value, static_cast<u8>(month.value),
                       static_cast<u8>(day.value), &offset))
    return Failure<DateValueResultV3>("CTI.TEMPORAL.INVALID_LITERAL",
                                      "civil_date_invalid");
  if (offset < std::numeric_limits<std::int32_t>::min() ||
      offset > std::numeric_limits<std::int32_t>::max())
    return Failure<DateValueResultV3>("CTI.TEMPORAL.RANGE_EXCEEDED",
                                      "civil_date_out_of_range");
  auto result = Success<DateValueResultV3>();
  result.value = {profile, DateValueStateV3::value,
                  static_cast<std::int32_t>(offset)};
  return result;
}

DateCivilResultV3 DecomposeDateCivilV3(const DateValueViewV3& value,
                                      bool null_allowed) noexcept {
  const auto checked = ValidateDateValueViewV3(value, null_allowed);
  if (!checked.ok())
    return Failure<DateCivilResultV3>(checked.diagnostic.diagnostic_code,
                                      checked.diagnostic.detail, checked.status);
  auto result = Success<DateCivilResultV3>();
  if (value.state == DateValueStateV3::sql_null) {
    result.is_null = true;
    result.civil = {0, 0, 0};
    return result;
  }
  result.civil = DaysToCivil(value.day);
  return result;
}

DateValueResultV3 ParseCanonicalDateV3(
    const std::shared_ptr<const DateValidatedProfileHandleV3>& profile, std::string_view text,
    bool null_allowed) noexcept {
  if (profile == nullptr || !ProfileValidNoAlloc(*profile))
    return Failure<DateValueResultV3>("CTI.TEMPORAL.DESCRIPTOR_INVALID",
                                      "parse_profile_invalid");
  (void)null_allowed;
  const auto parsed=ParseCanonicalDateDayNoAlloc(text);
  if(!parsed.ok)
    return Failure<DateValueResultV3>(parsed.diagnostic_code,parsed.detail);
  auto result=Success<DateValueResultV3>();
  result.value={profile,DateValueStateV3::value,parsed.day};
  return result;
}

DateValueResultV3 ParseCanonicalDateOperandV3(
    const std::shared_ptr<const DateValidatedProfileHandleV3>& profile,
    const DateTextOperandV3& operand,
    bool null_allowed) noexcept {
  if (profile == nullptr || !ProfileValidNoAlloc(*profile))
    return Failure<DateValueResultV3>("CTI.TEMPORAL.DESCRIPTOR_INVALID",
                                      "parse_profile_invalid");
  if (!ExactPeerIdentity(operand.identity, CanonicalTypeId::character) ||
      operand.descriptor == nullptr ||
      !DescriptorBindsIdentity(*operand.descriptor, *operand.identity,
                               CanonicalTypeId::character) ||
      !ExactCharacterDescriptor(*operand.descriptor, profile->receipt))
    return Failure<DateValueResultV3>("CTI.TEMPORAL.DESCRIPTOR_INVALID",
                                      "text_operand_authority_invalid");
  if (operand.state != DateValueStateV3::value &&
      operand.state != DateValueStateV3::sql_null)
    return Failure<DateValueResultV3>("DATATYPE.NULL_STATE.INVALID",
                                      "text_operand_state_invalid");
  if (operand.state == DateValueStateV3::sql_null) {
    if (operand.carrier != DateTextCarrierKindV3::utf8_bytes ||
        operand.extent != 0 || !operand.bytes.empty())
      return Failure<DateValueResultV3>("DATATYPE.NULL_STATE.INVALID",
                                        "text_operand_dirty_null");
    if (!null_allowed)
      return Failure<DateValueResultV3>("DATATYPE.NULL_NOT_ADMITTED",
                                        "parse_result_nonnullable");
    auto result = Success<DateValueResultV3>();
    result.value = {profile, DateValueStateV3::sql_null, 0};
    return result;
  }
  if (operand.carrier != DateTextCarrierKindV3::utf8_bytes)
    return Failure<DateValueResultV3>(
        "CTI.TEMPORAL.CANONICAL_ENCODING_INVALID",
        "text_operand_not_byte_sequence");
  if (operand.extent != operand.bytes.size())
    return Failure<DateValueResultV3>(
        "CTI.TEMPORAL.CANONICAL_ENCODING_INVALID",
        "text_operand_extent_mismatch");
  if (operand.extent > 16'777'216 ||
      (operand.descriptor->length != 0 &&
       operand.extent > operand.descriptor->length))
    return Failure<DateValueResultV3>("CTI.TEMPORAL.RANGE_EXCEEDED",
                                      "text_operand_extent_limit");
  return ParseCanonicalDateV3(profile, operand.bytes, null_allowed);
}

DateNoAllocWriteResultV3 RenderCanonicalDateIntoNoAllocV3(
    const DateOwnedValueV3& owned_value, bool export_literal, char* output,
    u64 output_capacity, const DateExecutionControlV3& control) noexcept {
  const auto value = owned_value.view();
  const auto checked = ValidateDateValueViewV3(value, true);
  if (!checked.ok())
    return Failure<DateNoAllocWriteResultV3>(checked.diagnostic.diagnostic_code,
                                             checked.diagnostic.detail,
                                             checked.status);
  auto result = Success<DateNoAllocWriteResultV3>();
  const bool containing_null = value.state == DateValueStateV3::sql_null;
  const auto civil = containing_null ? DateCivilV3{} : DaysToCivil(value.day);
  const u64 canonical_bytes = containing_null ? 0u
      : (civil.year >= 0 && civil.year <= 9999 ? 10u : 14u);
  const u64 required = containing_null ? 0u
      : canonical_bytes + (export_literal ? 7u : 0u);
  result.containing_null = containing_null;
  result.bytes_required = required;
  auto profile_pin = owned_value.profile;
  if (output != nullptr &&
      (OutputOverlapsOwnedDateInput(owned_value, output, required) ||
       RangesOverlap(output, required, &control, sizeof(control))))
    return Failure<DateNoAllocWriteResultV3>(
        "CTI.TEMPORAL.CANONICAL_ENCODING_INVALID",
        "render_output_aliases_input_or_profile");
  if (required > output_capacity || (required != 0 && output == nullptr)) {
    auto failure = Failure<DateNoAllocWriteResultV3>(
        "CTB.TEXT.LENGTH_EXCEEDED", "render_capacity");
    failure.bytes_required = required;
    return failure;
  }
  if (required > control.maximum_allocation_bytes) {
    auto failure = Failure<DateNoAllocWriteResultV3>(
        "RESOURCE.BUDGET_EXCEEDED", "render_resource_grant", ResourceStatus());
    failure.bytes_required = required;
    return failure;
  }
  std::array<char, 15> canonical{};
  ScopedSecureClear clear_canonical(
      canonical.data(), canonical.size(), DateScrubClassV3::render_staging,
      control.observe_scrubbed, control.scrub_observer_context);
  std::array<char, 21> staged{};
  ScopedSecureClear clear_staged(
      staged.data(), staged.size(), DateScrubClassV3::render_staging,
      control.observe_scrubbed, control.scrub_observer_context);
  std::size_t offset = 0;
  if (!containing_null) {
    const int written = civil.year >= 0 && civil.year <= 9999
        ? std::snprintf(canonical.data(), canonical.size(), "%04d-%02u-%02u",
                        civil.year, civil.month, civil.day)
        : std::snprintf(canonical.data(), canonical.size(), "%c%07lld-%02u-%02u",
                        civil.year < 0 ? '-' : '+',
                        static_cast<long long>(civil.year < 0
                            ? -static_cast<std::int64_t>(civil.year)
                            : static_cast<std::int64_t>(civil.year)),
                        civil.month, civil.day);
    if (written != static_cast<int>(canonical_bytes))
      return Failure<DateNoAllocWriteResultV3>(
          "CTI.TEMPORAL.CANONICAL_ENCODING_INVALID", "render_internal_extent");
    if (export_literal) {
      std::memcpy(staged.data(), "DATE '", 6);
      offset = 6;
    }
    std::memcpy(staged.data() + offset, canonical.data(), canonical_bytes);
    offset += canonical_bytes;
    if (export_literal) staged[offset++] = '\'';
  }
  if (Cancelled(control))
    return Failure<DateNoAllocWriteResultV3>("PROCESS.CANCELLED",
                                             "before_publication");
  if (offset != 0) std::memcpy(output, staged.data(), offset);
  result.bytes_written = offset;
  (void)profile_pin;
  return result;
}

DateTextResultV3 RenderCanonicalDateV3(
    const DateOwnedValueV3& owned_value, bool export_literal,
    const DateExecutionControlV3& control) noexcept {
  const auto value = owned_value.view();
  const auto checked = ValidateDateValueViewV3(value, true);
  if (!checked.ok())
    return Failure<DateTextResultV3>(checked.diagnostic.diagnostic_code,
                                     checked.diagnostic.detail, checked.status);
  u64 required = 0;
  if (value.state == DateValueStateV3::value) {
    const auto civil = DaysToCivil(value.day);
    required = (civil.year >= 0 && civil.year <= 9999 ? 10u : 14u) +
        (export_literal ? 7u : 0u);
  }
  auto profile_pin = owned_value.profile;
  if (required > control.maximum_allocation_bytes)
    return Failure<DateTextResultV3>("RESOURCE.BUDGET_EXCEEDED",
                                     "render_resource_grant", ResourceStatus());
  if (Cancelled(control))
    return Failure<DateTextResultV3>("PROCESS.CANCELLED", "before_work");
  std::array<char, 21> staged{};
  ScopedSecureClear clear_staged(
      staged.data(), staged.size(), DateScrubClassV3::render_owned_buffer,
      control.observe_scrubbed, control.scrub_observer_context);
  auto result = Success<DateTextResultV3>();
  ScopedStringSecureClear clear_result(
      &result.text, DateScrubClassV3::render_owned_buffer,
      control.observe_scrubbed, control.scrub_observer_context);
  result.containing_null = value.state == DateValueStateV3::sql_null;
  if (result.containing_null) {
    if (Cancelled(control))
      return Failure<DateTextResultV3>("PROCESS.CANCELLED",
                                       "before_publication");
    clear_result.Disarm();
    return result;
  }
  const auto civil = DaysToCivil(value.day);
  std::size_t offset = 0;
  if (export_literal) {
    std::memcpy(staged.data(), "DATE '", 6);
    offset = 6;
  }
  const int written = civil.year >= 0 && civil.year <= 9999
      ? std::snprintf(staged.data() + offset, staged.size() - offset,
                      "%04d-%02u-%02u", civil.year, civil.month, civil.day)
      : std::snprintf(staged.data() + offset, staged.size() - offset,
                      "%c%07lld-%02u-%02u", civil.year < 0 ? '-' : '+',
                      static_cast<long long>(civil.year < 0
                          ? -static_cast<std::int64_t>(civil.year)
                          : static_cast<std::int64_t>(civil.year)),
                      civil.month, civil.day);
  if (written != 10 && written != 14)
    return Failure<DateTextResultV3>(
        "CTI.TEMPORAL.CANONICAL_ENCODING_INVALID", "render_internal_extent");
  offset += static_cast<std::size_t>(written);
  if (export_literal) staged[offset++] = '\'';
  try {
    result.text.assign(staged.data(), offset);
  } catch (...) {
    return Failure<DateTextResultV3>("RESOURCE.BUDGET_EXCEEDED",
                                     "render_allocation", ResourceStatus());
  }
  if (Cancelled(control))
    return Failure<DateTextResultV3>("PROCESS.CANCELLED", "before_publication");
  clear_result.Disarm();
  return result;
}

DateValueResultV3 ValidateCanonicalDateV3(const DateOwnedValueV3& owned_value,
                                          bool null_allowed) noexcept {
  const auto value = owned_value.view();
  const auto checked = ValidateDateValueViewV3(value, null_allowed);
  if (!checked.ok())
    return Failure<DateValueResultV3>(checked.diagnostic.diagnostic_code,
                                      checked.diagnostic.detail, checked.status);
  auto result = Success<DateValueResultV3>();
  result.value = owned_value;
  return result;
}

DateValueResultV3 TruncateDateDayV3(const DateOwnedValueV3& value,
                                    bool null_allowed) noexcept {
  return ValidateCanonicalDateV3(value, null_allowed);
}

DateValueResultV3 RoundDateDayV3(const DateOwnedValueV3& value,
                                 bool null_allowed) noexcept {
  return ValidateCanonicalDateV3(value, null_allowed);
}

DateIntrinsicDispositionV3 ClassifyDateIntrinsicOperationV3(
    DateIntrinsicOperationV3 operation) noexcept {
  const auto ordinal = static_cast<u8>(operation);
  if (ordinal <= static_cast<u8>(DateIntrinsicOperationV3::round_day))
    return DateIntrinsicDispositionV3::admitted;
  if (operation == DateIntrinsicOperationV3::larger_truncate_round_or_bucket ||
      operation == DateIntrinsicOperationV3::calendar_interval_or_cross_temporal)
    return DateIntrinsicDispositionV3::registered_refused;
  if (operation == DateIntrinsicOperationV3::aggregate_min_max_count_dispatch)
    return DateIntrinsicDispositionV3::receiving_owner;
  return DateIntrinsicDispositionV3::unknown;
}

DateValueResultV3 RefuseDateIntrinsicOperationV3(
    const DateValueViewV3& operand, DateIntrinsicOperationV3 operation) noexcept {
  const auto checked = ValidateDateValueViewV3(operand, true);
  if (!checked.ok())
    return Failure<DateValueResultV3>(checked.diagnostic.diagnostic_code,
                                      checked.diagnostic.detail, checked.status);
  if (ClassifyDateIntrinsicOperationV3(operation) ==
      DateIntrinsicDispositionV3::registered_refused)
    return Failure<DateValueResultV3>("CTI.INTERVAL.CALENDAR_OPERATION_REFUSED",
                                      "registered_date_operation_refused");
  return Failure<DateValueResultV3>("CTI.TEMPORAL.OPERATION_REFUSED",
                                    "date_operation_not_dispatchable_here");
}

DateValueResultV3 AddDateDaysV3(const DateOwnedValueV3& owned_value,
                               std::int64_t delta, bool null_allowed,
                               const DateExecutionControlV3& control) noexcept {
  return AddDateDaysV3(
      owned_value,
      {DateI64CarrierKindV3::signed_i64, DateValueStateV3::value, delta},
      null_allowed, control);
}

DateValueResultV3 AddDateDaysV3(const DateOwnedValueV3& owned_value,
                               const DateNullableI64FactV3& delta,
                               bool null_allowed,
                               const DateExecutionControlV3& control) noexcept {
  const auto value = owned_value.view();
  const auto checked = ValidateDateValueViewV3(value, true);
  if (!checked.ok())
    return Failure<DateValueResultV3>(checked.diagnostic.diagnostic_code,
                                      checked.diagnostic.detail, checked.status);
  if (delta.state != DateValueStateV3::value &&
      delta.state != DateValueStateV3::sql_null)
    return Failure<DateValueResultV3>("DATATYPE.NULL_STATE.INVALID",
                                      "date_delta_state_invalid");
  if (delta.state == DateValueStateV3::sql_null &&
      (delta.carrier != DateI64CarrierKindV3::signed_i64 || delta.value != 0))
    return Failure<DateValueResultV3>("DATATYPE.NULL_STATE.INVALID",
                                      "date_delta_dirty_null");
  if (value.state == DateValueStateV3::sql_null ||
      delta.state == DateValueStateV3::sql_null) {
    if (!null_allowed)
      return Failure<DateValueResultV3>("DATATYPE.NULL_NOT_ADMITTED",
                                        "date_add_result_nonnullable");
    DateOwnedValueV3 staged{owned_value.profile, DateValueStateV3::sql_null, 0};
    ScopedOwnedDateValueClear clear_staged(&staged);
    if (Cancelled(control)) {
      return Failure<DateValueResultV3>("PROCESS.CANCELLED",
                                        "before_atomic_publication");
    }
    auto result = Success<DateValueResultV3>();
    result.value = std::move(staged);
    return result;
  }
  if (delta.carrier != DateI64CarrierKindV3::signed_i64)
    return Failure<DateValueResultV3>(
        "CTI.TEMPORAL.CANONICAL_ENCODING_INVALID",
        "date_delta_not_signed_i64");
  const auto current = static_cast<std::int64_t>(value.day);
  if (delta.value > std::numeric_limits<std::int32_t>::max() - current ||
      delta.value < std::numeric_limits<std::int32_t>::min() - current)
    return Failure<DateValueResultV3>("CTI.TEMPORAL.RANGE_EXCEEDED",
                                      "date_add_range");
  DateOwnedValueV3 staged{owned_value.profile, DateValueStateV3::value,
                          static_cast<std::int32_t>(current + delta.value)};
  ScopedOwnedDateValueClear clear_staged(&staged);
  if (Cancelled(control)) {
    return Failure<DateValueResultV3>("PROCESS.CANCELLED",
                                      "before_atomic_publication");
  }
  auto result = Success<DateValueResultV3>();
  result.value = std::move(staged);
  return result;
}

DateValueResultV3 SubtractDateDaysV3(const DateOwnedValueV3& owned_value,
                                    std::int64_t delta, bool null_allowed,
                                    const DateExecutionControlV3& control) noexcept {
  return SubtractDateDaysV3(
      owned_value,
      {DateI64CarrierKindV3::signed_i64, DateValueStateV3::value, delta},
      null_allowed, control);
}

DateValueResultV3 SubtractDateDaysV3(const DateOwnedValueV3& owned_value,
                                    const DateNullableI64FactV3& delta,
                                    bool null_allowed,
                                    const DateExecutionControlV3& control) noexcept {
  const auto value = owned_value.view();
  const auto checked = ValidateDateValueViewV3(value, true);
  if (!checked.ok())
    return Failure<DateValueResultV3>(checked.diagnostic.diagnostic_code,
                                      checked.diagnostic.detail, checked.status);
  if (delta.state != DateValueStateV3::value &&
      delta.state != DateValueStateV3::sql_null)
    return Failure<DateValueResultV3>("DATATYPE.NULL_STATE.INVALID",
                                      "date_delta_state_invalid");
  if (delta.state == DateValueStateV3::sql_null &&
      (delta.carrier != DateI64CarrierKindV3::signed_i64 || delta.value != 0))
    return Failure<DateValueResultV3>("DATATYPE.NULL_STATE.INVALID",
                                      "date_delta_dirty_null");
  if (value.state == DateValueStateV3::sql_null ||
      delta.state == DateValueStateV3::sql_null) {
    if (!null_allowed)
      return Failure<DateValueResultV3>("DATATYPE.NULL_NOT_ADMITTED",
                                        "date_subtract_result_nonnullable");
    DateOwnedValueV3 staged{owned_value.profile, DateValueStateV3::sql_null, 0};
    ScopedOwnedDateValueClear clear_staged(&staged);
    if (Cancelled(control)) {
      return Failure<DateValueResultV3>("PROCESS.CANCELLED",
                                        "before_atomic_publication");
    }
    auto result = Success<DateValueResultV3>();
    result.value = std::move(staged);
    return result;
  }
  if (delta.carrier != DateI64CarrierKindV3::signed_i64)
    return Failure<DateValueResultV3>(
        "CTI.TEMPORAL.CANONICAL_ENCODING_INVALID",
        "date_delta_not_signed_i64");
  const auto current = static_cast<std::int64_t>(value.day);
  if (delta.value > current - std::numeric_limits<std::int32_t>::min() ||
      delta.value < current - std::numeric_limits<std::int32_t>::max())
    return Failure<DateValueResultV3>("CTI.TEMPORAL.RANGE_EXCEEDED",
                                      "date_subtract_range");
  DateOwnedValueV3 staged{owned_value.profile, DateValueStateV3::value,
                          static_cast<std::int32_t>(current - delta.value)};
  ScopedOwnedDateValueClear clear_staged(&staged);
  if (Cancelled(control)) {
    return Failure<DateValueResultV3>("PROCESS.CANCELLED",
                                      "before_atomic_publication");
  }
  auto result = Success<DateValueResultV3>();
  result.value = std::move(staged);
  return result;
}

DateValueResultV3 DateSuccessorV3(const DateOwnedValueV3& value,
                                 bool null_allowed,
                                 const DateExecutionControlV3& control) noexcept {
  return AddDateDaysV3(value, 1, null_allowed, control);
}

DateValueResultV3 DatePredecessorV3(const DateOwnedValueV3& value,
                                   bool null_allowed,
                                   const DateExecutionControlV3& control) noexcept {
  return SubtractDateDaysV3(value, 1, null_allowed, control);
}

DateScalarResultV3 DifferenceDateDaysV3(const DateValueViewV3& left,
                                       const DateValueViewV3& right,
                                       bool null_allowed) noexcept {
  const auto left_checked = ValidateDateValueViewV3(left, true);
  if (!left_checked.ok())
    return Failure<DateScalarResultV3>(left_checked.diagnostic.diagnostic_code,
                                       left_checked.diagnostic.detail, left_checked.status);
  const auto right_checked = ValidateDateValueViewV3(right, true);
  if (!right_checked.ok())
    return Failure<DateScalarResultV3>(right_checked.diagnostic.diagnostic_code,
                                       right_checked.diagnostic.detail, right_checked.status);
  if (left.profile->comparison_fingerprint != right.profile->comparison_fingerprint)
    return Failure<DateScalarResultV3>("CTI.TEMPORAL.DESCRIPTOR_INVALID",
                                       "comparison_cohort_mismatch");
  auto result = Success<DateScalarResultV3>();
  if (left.state == DateValueStateV3::sql_null ||
      right.state == DateValueStateV3::sql_null) {
    if (!null_allowed)
      return Failure<DateScalarResultV3>("DATATYPE.NULL_NOT_ADMITTED",
                                         "date_difference_result_nonnullable");
    result.is_null = true;
    return result;
  }
  result.signed_value = static_cast<std::int64_t>(left.day) -
                        static_cast<std::int64_t>(right.day);
  return result;
}

DateScalarResultV3 DateIsLeapYearV3(const DateValueViewV3& value,
                                   bool null_allowed) noexcept {
  const auto civil = DecomposeDateCivilV3(value, null_allowed);
  if (!civil.ok())
    return Failure<DateScalarResultV3>(civil.diagnostic.diagnostic_code,
                                       "decompose_failed", civil.status);
  auto result = Success<DateScalarResultV3>();
  result.is_null = civil.is_null;
  result.boolean_value = !civil.is_null && Leap(civil.civil.year);
  result.signed_value = result.boolean_value ? 1 : 0;
  return result;
}

DateScalarResultV3 DateDaysInMonthV3(const DateValueViewV3& value,
                                    bool null_allowed) noexcept {
  const auto civil = DecomposeDateCivilV3(value, null_allowed);
  if (!civil.ok())
    return Failure<DateScalarResultV3>(civil.diagnostic.diagnostic_code,
                                       "decompose_failed", civil.status);
  auto result = Success<DateScalarResultV3>();
  result.is_null = civil.is_null;
  if (!civil.is_null) result.signed_value = DaysInMonth(civil.civil.year, civil.civil.month);
  return result;
}

DateScalarResultV3 DateIsoWeekdayV3(const DateValueViewV3& value,
                                   bool null_allowed) noexcept {
  const auto checked = ValidateDateValueViewV3(value, null_allowed);
  if (!checked.ok())
    return Failure<DateScalarResultV3>(checked.diagnostic.diagnostic_code,
                                       checked.diagnostic.detail, checked.status);
  auto result = Success<DateScalarResultV3>();
  result.is_null = value.state == DateValueStateV3::sql_null;
  if (!result.is_null) result.signed_value = FloorMod(static_cast<std::int64_t>(value.day) + 3, 7) + 1;
  return result;
}

DateScalarResultV3 DateDayOfYearV3(const DateValueViewV3& value,
                                  bool null_allowed) noexcept {
  const auto civil = DecomposeDateCivilV3(value, null_allowed);
  if (!civil.ok())
    return Failure<DateScalarResultV3>(civil.diagnostic.diagnostic_code,
                                       "decompose_failed", civil.status);
  auto result = Success<DateScalarResultV3>();
  result.is_null = civil.is_null;
  if (!civil.is_null) {
    std::int64_t first = 0;
    CivilToDays(civil.civil.year, 1, 1, &first);
    result.signed_value = static_cast<std::int64_t>(value.day) - first + 1;
  }
  return result;
}

DateScalarResultV3 DateQuarterV3(const DateValueViewV3& value,
                                bool null_allowed) noexcept {
  const auto civil = DecomposeDateCivilV3(value, null_allowed);
  if (!civil.ok())
    return Failure<DateScalarResultV3>(civil.diagnostic.diagnostic_code,
                                       "decompose_failed", civil.status);
  auto result = Success<DateScalarResultV3>();
  result.is_null = civil.is_null;
  if (!civil.is_null) result.signed_value = 1 + (civil.civil.month - 1) / 3;
  return result;
}

DateIsoWeekResultV3 DateIsoWeekV3(const DateValueViewV3& value,
                                 bool null_allowed) noexcept {
  const auto checked = ValidateDateValueViewV3(value, null_allowed);
  if (!checked.ok())
    return Failure<DateIsoWeekResultV3>(checked.diagnostic.diagnostic_code,
                                        checked.diagnostic.detail, checked.status);
  auto result = Success<DateIsoWeekResultV3>();
  if (value.state == DateValueStateV3::sql_null) {
    result.is_null = true;
    return result;
  }
  const auto weekday = static_cast<std::int32_t>(
      FloorMod(static_cast<std::int64_t>(value.day) + 3, 7) + 1);
  const auto thursday = static_cast<std::int64_t>(value.day) + (4 - weekday);
  const auto thursday_civil = DaysToCivil(thursday);
  std::int64_t january4 = 0;
  CivilToDays(thursday_civil.year, 1, 4, &january4);
  const auto january4_weekday = FloorMod(january4 + 3, 7) + 1;
  const auto week1_monday = january4 - (january4_weekday - 1);
  result.iso_year = thursday_civil.year;
  result.iso_week = static_cast<u8>((static_cast<std::int64_t>(value.day) - week1_monday) / 7 + 1);
  result.iso_weekday = static_cast<u8>(weekday);
  return result;
}

namespace {
DateComparisonResultV3 CompareDateValuesResolvedCohort(
    const DateValueViewV3& left, const DateValueViewV3& right,
    const std::array<byte, 32>& right_comparison_fingerprint) noexcept {
  if (left.profile == nullptr || !ProfileValidNoAlloc(*left.profile) ||
      right.profile == nullptr || !ProfileValidNoAlloc(*right.profile))
    return Failure<DateComparisonResultV3>("CTI.TEMPORAL.DESCRIPTOR_INVALID",
                                           "comparison_profile_invalid");
  if ((left.state != DateValueStateV3::value &&
       left.state != DateValueStateV3::sql_null) ||
      (left.state == DateValueStateV3::sql_null && left.day != 0))
    return Failure<DateComparisonResultV3>("DATATYPE.NULL_STATE.INVALID",
                                           "comparison_left_state_invalid");
  if ((right.state != DateValueStateV3::value &&
       right.state != DateValueStateV3::sql_null) ||
      (right.state == DateValueStateV3::sql_null && right.day != 0))
    return Failure<DateComparisonResultV3>("DATATYPE.NULL_STATE.INVALID",
                                           "comparison_right_state_invalid");
  if (left.profile->comparison_fingerprint != right_comparison_fingerprint)
    return Failure<DateComparisonResultV3>("CTI.TEMPORAL.ORDERING_REFUSED",
                                           "comparison_cohort_mismatch");
  auto result = Success<DateComparisonResultV3>();
  if (left.state == DateValueStateV3::sql_null || right.state == DateValueStateV3::sql_null) {
    result.fact = DateComparisonFactV3::unordered_null;
    result.null_equivalent = left.state == right.state;
    result.grouping_equivalent = result.null_equivalent;
    return result;
  }
  result.fact = left.day < right.day ? DateComparisonFactV3::less
      : left.day > right.day ? DateComparisonFactV3::greater
                             : DateComparisonFactV3::equal;
  result.grouping_equivalent = result.fact == DateComparisonFactV3::equal;
  return result;
}
}  // namespace

DateComparisonResultV3 CompareDateValuesV3(const DateValueViewV3& left,
                                           const DateValueViewV3& right) noexcept {
  const std::array<byte, 32> unresolved{};
  return CompareDateValuesResolvedCohort(
      left, right,
      right.profile == nullptr ? unresolved : right.profile->comparison_fingerprint);
}

DateComparisonResultV3 CompareDateValuesWithValidatedCohortForConformanceV3(
    const DateValueViewV3& left, const DateValueViewV3& right,
    const std::array<byte, 32>& validated_right_comparison_fingerprint) noexcept {
  return CompareDateValuesResolvedCohort(
      left, right, validated_right_comparison_fingerprint);
}

DateNoAllocWriteResultV3 HashDateValueIntoNoAllocV3(
    const DateOwnedValueV3& owned_value, byte* output, u64 output_capacity,
    const DateExecutionControlV3& control) noexcept {
  const auto value = owned_value.view();
  const auto checked = ValidateDateValueViewV3(value, true);
  if (!checked.ok())
    return Failure<DateNoAllocWriteResultV3>(checked.diagnostic.diagnostic_code,
                                             checked.diagnostic.detail,
                                             checked.status);
  auto profile_pin = owned_value.profile;
  if (output != nullptr &&
      (OutputOverlapsOwnedDateInput(owned_value, output, kDateHashBytesV3) ||
       RangesOverlap(output, kDateHashBytesV3, &control, sizeof(control))))
    return Failure<DateNoAllocWriteResultV3>(
        "CTI.TEMPORAL.CANONICAL_ENCODING_INVALID",
        "hash_output_aliases_input_or_profile");
  if (output_capacity < kDateHashBytesV3 ||
      control.maximum_allocation_bytes < kDateHashBytesV3 || output == nullptr) {
    auto failure = Failure<DateNoAllocWriteResultV3>(
        "RESOURCE.BUDGET_EXCEEDED", "hash_capacity", ResourceStatus());
    failure.bytes_required = kDateHashBytesV3;
    return failure;
  }
  std::array<byte, 105> preimage{};
  ScopedSecureClear clear_preimage(
      preimage.data(), preimage.size(), DateScrubClassV3::hash_preimage,
      control.observe_scrubbed, control.scrub_observer_context);
  std::memcpy(preimage.data(), "SBDATH01", 8);
  PutUuid(preimage.data() + 8, value.profile->receipt.catalog_snapshot_uuid);
  StoreLittle64(preimage.data() + 24, value.profile->receipt.catalog_generation);
  StoreLittle64(preimage.data() + 32, value.profile->receipt.registry_generation);
  std::memcpy(preimage.data() + 40, value.profile->comparison_fingerprint.data(), 32);
  PutPolicy(preimage.data() + 72, value.profile->identity.hash_policy);
  preimage[96] = value.state == DateValueStateV3::sql_null ? 0 : 1;
  StoreLittle32(preimage.data() + 97,
                value.state == DateValueStateV3::sql_null ? 0u : 4u);
  std::size_t extent = 101;
  if (value.state == DateValueStateV3::value) {
    StoreLittle32(preimage.data() + 101, static_cast<u32>(value.day));
    extent = 105;
  }
  std::array<byte, 32> digest{};
  ScopedSecureClear clear_digest(
      digest.data(), digest.size(), DateScrubClassV3::hash_digest,
      control.observe_scrubbed, control.scrub_observer_context);
  if (!Digest(std::span<const byte>(preimage.data(), extent), &digest, &control))
    return Failure<DateNoAllocWriteResultV3>("RESOURCE.BUDGET_EXCEEDED",
                                             "hash_provider", ResourceStatus());
  if (Cancelled(control))
    return Failure<DateNoAllocWriteResultV3>("PROCESS.CANCELLED",
                                             "before_publication");
  std::memcpy(output, digest.data(), digest.size());
  auto result = Success<DateNoAllocWriteResultV3>();
  result.bytes_required = digest.size();
  result.bytes_written = digest.size();
  return result;
}

DateBytesResultV3 HashDateValueV3(const DateOwnedValueV3& value) noexcept {
  return HashDateValueV3(value, {});
}

DateBytesResultV3 HashDateValueV3(
    const DateOwnedValueV3& owned_value,
    const DateExecutionControlV3& control) noexcept {
  const auto value = owned_value.view();
  const auto checked = ValidateDateValueViewV3(value, true);
  if (!checked.ok())
    return Failure<DateBytesResultV3>(checked.diagnostic.diagnostic_code,
                                      checked.diagnostic.detail, checked.status);
  auto profile_pin = owned_value.profile;
  if (kDateHashBytesV3 > control.maximum_allocation_bytes)
    return Failure<DateBytesResultV3>("RESOURCE.BUDGET_EXCEEDED",
                                      "hash_capacity", ResourceStatus());
  if (Cancelled(control))
    return Failure<DateBytesResultV3>("PROCESS.CANCELLED", "before_work");
  std::array<byte, 105> preimage{};
  ScopedSecureClear clear_preimage(
      preimage.data(), preimage.size(), DateScrubClassV3::hash_preimage,
      control.observe_scrubbed, control.scrub_observer_context);
  std::memcpy(preimage.data(), "SBDATH01", 8);
  PutUuid(preimage.data() + 8, value.profile->receipt.catalog_snapshot_uuid);
  StoreLittle64(preimage.data() + 24, value.profile->receipt.catalog_generation);
  StoreLittle64(preimage.data() + 32, value.profile->receipt.registry_generation);
  std::memcpy(preimage.data() + 40, value.profile->comparison_fingerprint.data(), 32);
  PutPolicy(preimage.data() + 72, value.profile->identity.hash_policy);
  preimage[96] = value.state == DateValueStateV3::sql_null ? 0 : 1;
  StoreLittle32(preimage.data() + 97,
                value.state == DateValueStateV3::sql_null ? 0u : 4u);
  std::size_t extent = 101;
  if (value.state == DateValueStateV3::value) {
    StoreLittle32(preimage.data() + 101, static_cast<u32>(value.day));
    extent = 105;
  }
  std::array<byte, kDateHashBytesV3> staged{};
  ScopedSecureClear clear_staged(
      staged.data(), staged.size(), DateScrubClassV3::hash_digest,
      control.observe_scrubbed, control.scrub_observer_context);
  if (!Digest(std::span<const byte>(preimage.data(), extent), &staged, &control))
    return Failure<DateBytesResultV3>("RESOURCE.BUDGET_EXCEEDED",
                                      "hash_provider", ResourceStatus());
  auto result = Success<DateBytesResultV3>();
  ScopedVectorSecureClear<byte> clear_result(
      &result.bytes, DateScrubClassV3::hash_owned_buffer,
      control.observe_scrubbed, control.scrub_observer_context);
  try { result.bytes.assign(staged.begin(), staged.end()); }
  catch (...) {
    return Failure<DateBytesResultV3>("RESOURCE.BUDGET_EXCEEDED",
                                      "hash_allocation", ResourceStatus());
  }
  if (Cancelled(control))
    return Failure<DateBytesResultV3>("PROCESS.CANCELLED",
                                      "before_publication");
  clear_result.Disarm();
  return result;
}

DateNoAllocWriteResultV3 MakeDateSortKeyIntoNoAllocV3(
    const DateOwnedValueV3& owned_value, DateSortDirectionV3 direction,
    DateNullModeV3 null_mode, byte* output, u64 output_capacity,
    const DateExecutionControlV3& control) noexcept {
  const auto value = owned_value.view();
  if (value.profile == nullptr || !ProfileValidNoAlloc(*value.profile))
    return Failure<DateNoAllocWriteResultV3>(
        "CTI.TEMPORAL.DESCRIPTOR_INVALID", "key_profile_invalid");
  if ((direction != DateSortDirectionV3::ascending && direction != DateSortDirectionV3::descending) ||
      (null_mode != DateNullModeV3::nulls_first && null_mode != DateNullModeV3::nulls_last))
    return Failure<DateNoAllocWriteResultV3>("CTI.TEMPORAL.INDEX_KEY_REFUSED",
                                             "sort_mode_invalid");
  const auto checked = ValidateDateValueViewV3(value, true);
  if (!checked.ok())
    return Failure<DateNoAllocWriteResultV3>(checked.diagnostic.diagnostic_code,
                                             checked.diagnostic.detail,
                                             checked.status);
  const std::size_t extent = value.state == DateValueStateV3::sql_null
      ? kDateNullSortKeyBytesV3 : kDateValueSortKeyBytesV3;
  auto profile_pin = owned_value.profile;
  if (output != nullptr &&
      (OutputOverlapsOwnedDateInput(owned_value, output, extent) ||
       RangesOverlap(output, extent, &control, sizeof(control))))
    return Failure<DateNoAllocWriteResultV3>(
        "CTI.TEMPORAL.CANONICAL_ENCODING_INVALID",
        "sort_key_output_aliases_input_or_profile");
  if (extent > output_capacity || extent > control.maximum_allocation_bytes ||
      output == nullptr) {
    auto failure = Failure<DateNoAllocWriteResultV3>(
        "RESOURCE.BUDGET_EXCEEDED", "sort_key_capacity", ResourceStatus());
    failure.bytes_required = extent;
    return failure;
  }
  std::array<byte, kDateValueSortKeyBytesV3> staged{};
  ScopedSecureClear clear_staged(
      staged.data(), staged.size(), DateScrubClassV3::ordered_key_staging,
      control.observe_scrubbed, control.scrub_observer_context);
  std::memcpy(staged.data(), "SBDATK01", 8);
  PutUuid(staged.data() + 8, value.profile->receipt.catalog_snapshot_uuid);
  StoreLittle64(staged.data() + 24, value.profile->receipt.catalog_generation);
  StoreLittle64(staged.data() + 32, value.profile->receipt.registry_generation);
  std::memcpy(staged.data() + 40, value.profile->comparison_fingerprint.data(), 32);
  PutPolicy(staged.data() + 72, value.profile->identity.ordering_policy);
  staged[96] = direction == DateSortDirectionV3::descending ? 1 : 0;
  staged[97] = null_mode == DateNullModeV3::nulls_last ? 1 : 0;
  staged[98] = value.state == DateValueStateV3::value ? 1
      : (null_mode == DateNullModeV3::nulls_first ? 0 : 2);
  staged[99] = value.state == DateValueStateV3::value ? 4 : 0;
  if (value.state == DateValueStateV3::value) {
    const u32 sortable = static_cast<u32>(value.day) ^ 0x80000000u;
    staged[100] = static_cast<byte>(sortable >> 24);
    staged[101] = static_cast<byte>(sortable >> 16);
    staged[102] = static_cast<byte>(sortable >> 8);
    staged[103] = static_cast<byte>(sortable);
    if (direction == DateSortDirectionV3::descending)
      for (std::size_t index = 100; index < 104; ++index)
        staged[index] = static_cast<byte>(~staged[index]);
  }
  if (Cancelled(control))
    return Failure<DateNoAllocWriteResultV3>("PROCESS.CANCELLED",
                                             "before_publication");
  std::memcpy(output, staged.data(), extent);
  auto result = Success<DateNoAllocWriteResultV3>();
  result.bytes_required = extent;
  result.bytes_written = extent;
  return result;
}

DateBytesResultV3 MakeDateSortKeyV3(
    const DateOwnedValueV3& owned_value, DateSortDirectionV3 direction,
    DateNullModeV3 null_mode, const DateExecutionControlV3& control) noexcept {
  const auto value = owned_value.view();
  if (value.profile == nullptr || !ProfileValidNoAlloc(*value.profile))
    return Failure<DateBytesResultV3>("CTI.TEMPORAL.DESCRIPTOR_INVALID",
                                      "key_profile_invalid");
  if ((direction != DateSortDirectionV3::ascending &&
       direction != DateSortDirectionV3::descending) ||
      (null_mode != DateNullModeV3::nulls_first &&
       null_mode != DateNullModeV3::nulls_last))
    return Failure<DateBytesResultV3>("CTI.TEMPORAL.INDEX_KEY_REFUSED",
                                      "sort_mode_invalid");
  const auto checked = ValidateDateValueViewV3(value, true);
  if (!checked.ok())
    return Failure<DateBytesResultV3>(checked.diagnostic.diagnostic_code,
                                      checked.diagnostic.detail, checked.status);
  const std::size_t extent = value.state == DateValueStateV3::sql_null
      ? kDateNullSortKeyBytesV3 : kDateValueSortKeyBytesV3;
  auto profile_pin = owned_value.profile;
  if (extent > control.maximum_allocation_bytes)
    return Failure<DateBytesResultV3>("RESOURCE.BUDGET_EXCEEDED",
                                      "sort_key_capacity", ResourceStatus());
  if (Cancelled(control))
    return Failure<DateBytesResultV3>("PROCESS.CANCELLED", "before_work");
  std::array<byte, kDateValueSortKeyBytesV3> staged{};
  ScopedSecureClear clear_staged(
      staged.data(), staged.size(), DateScrubClassV3::ordered_key_owned_buffer,
      control.observe_scrubbed, control.scrub_observer_context);
  std::memcpy(staged.data(), "SBDATK01", 8);
  PutUuid(staged.data() + 8, value.profile->receipt.catalog_snapshot_uuid);
  StoreLittle64(staged.data() + 24, value.profile->receipt.catalog_generation);
  StoreLittle64(staged.data() + 32, value.profile->receipt.registry_generation);
  std::memcpy(staged.data() + 40,
              value.profile->comparison_fingerprint.data(), 32);
  PutPolicy(staged.data() + 72, value.profile->identity.ordering_policy);
  staged[96] = direction == DateSortDirectionV3::descending ? 1 : 0;
  staged[97] = null_mode == DateNullModeV3::nulls_last ? 1 : 0;
  staged[98] = value.state == DateValueStateV3::value ? 1
      : (null_mode == DateNullModeV3::nulls_first ? 0 : 2);
  staged[99] = value.state == DateValueStateV3::value ? 4 : 0;
  if (value.state == DateValueStateV3::value) {
    const u32 sortable = static_cast<u32>(value.day) ^ 0x80000000u;
    staged[100] = static_cast<byte>(sortable >> 24);
    staged[101] = static_cast<byte>(sortable >> 16);
    staged[102] = static_cast<byte>(sortable >> 8);
    staged[103] = static_cast<byte>(sortable);
    if (direction == DateSortDirectionV3::descending)
      for (std::size_t index = 100; index < 104; ++index)
        staged[index] = static_cast<byte>(~staged[index]);
  }
  auto result = Success<DateBytesResultV3>();
  ScopedVectorSecureClear<byte> clear_result(
      &result.bytes, DateScrubClassV3::ordered_key_owned_buffer,
      control.observe_scrubbed, control.scrub_observer_context);
  try {
    result.bytes.assign(staged.begin(),
                        staged.begin() + static_cast<std::ptrdiff_t>(extent));
  }
  catch (...) {
    return Failure<DateBytesResultV3>("RESOURCE.BUDGET_EXCEEDED",
                                      "sort_key_allocation", ResourceStatus());
  }
  if (Cancelled(control))
    return Failure<DateBytesResultV3>("PROCESS.CANCELLED", "before_publication");
  clear_result.Disarm();
  return result;
}

DateSortKeyViewResultV3 DecodeDateSortKeyNoAllocV3(
    const DateValidatedProfileHandleV3& profile,
    std::span<const byte> encoded,
    const DateExecutionControlV3& control) noexcept {
  if ((encoded.size() != 100 && encoded.size() != 104) ||
      std::memcmp(encoded.data(), "SBDATK01", 8) != 0)
    return Failure<DateSortKeyViewResultV3>("CTI.TEMPORAL.CANONICAL_ENCODING_INVALID",
                                            "K01_extent_or_magic");
  if (!ProfileValidNoAlloc(profile, &control) ||
      std::memcmp(encoded.data() + 8, profile.receipt.catalog_snapshot_uuid.bytes.data(), 16) != 0 ||
      LoadLittle64(encoded.data() + 24) != profile.receipt.catalog_generation ||
      LoadLittle64(encoded.data() + 32) != profile.receipt.registry_generation ||
      std::memcmp(encoded.data() + 40, profile.comparison_fingerprint.data(), 32) != 0 ||
      std::memcmp(encoded.data() + 72, kOrderingPolicy.uuid.bytes.data(), 16) != 0 ||
      LoadLittle64(encoded.data() + 88) != kOrderingPolicy.generation)
    return Failure<DateSortKeyViewResultV3>("CTI.TEMPORAL.DESCRIPTOR_INVALID",
                                            "K02_authority");
  if (encoded[96] > 1 || encoded[97] > 1)
    return Failure<DateSortKeyViewResultV3>("CTI.TEMPORAL.CANONICAL_ENCODING_INVALID",
                                            "K03_mode");
  const auto direction = static_cast<DateSortDirectionV3>(encoded[96]);
  const auto null_mode = static_cast<DateNullModeV3>(encoded[97]);
  auto state=DateValueStateV3::sql_null;
  std::int32_t day=0;
  ScopedSecureClear clear_state(
      &state, sizeof(state), DateScrubClassV3::ordered_key_decode_reencode,
      control.observe_scrubbed, control.scrub_observer_context);
  ScopedSecureClear clear_day(
      &day, sizeof(day), DateScrubClassV3::ordered_key_decode_reencode,
      control.observe_scrubbed, control.scrub_observer_context);
  if (encoded.size() == 100) {
    const byte rank = null_mode == DateNullModeV3::nulls_first ? 0 : 2;
    if (encoded[98] != rank || encoded[99] != 0)
      return Failure<DateSortKeyViewResultV3>("CTI.TEMPORAL.CANONICAL_ENCODING_INVALID",
                                              "K03_null_rank_or_length");
  } else {
    if (encoded[98] != 1 || encoded[99] != 4)
      return Failure<DateSortKeyViewResultV3>("CTI.TEMPORAL.CANONICAL_ENCODING_INVALID",
                                              "K03_value_rank_or_length");
    byte suffix[4];
    ScopedSecureClear clear_suffix(
        suffix, sizeof(suffix),
        DateScrubClassV3::ordered_key_decode_reencode,
        control.observe_scrubbed, control.scrub_observer_context);
    std::memcpy(suffix, encoded.data() + 100, 4);
    if (direction == DateSortDirectionV3::descending)
      for (auto& octet : suffix) octet = static_cast<byte>(~octet);
    const u32 sortable = (static_cast<u32>(suffix[0]) << 24) |
        (static_cast<u32>(suffix[1]) << 16) |
        (static_cast<u32>(suffix[2]) << 8) | suffix[3];
    state = DateValueStateV3::value;
    day = static_cast<std::int32_t>(sortable ^ 0x80000000u);
  }
  std::array<byte, kDateValueSortKeyBytesV3> reencoded{};
  ScopedSecureClear clear_reencoded(
      reencoded.data(), reencoded.size(),
      DateScrubClassV3::ordered_key_decode_reencode,
      control.observe_scrubbed, control.scrub_observer_context);
  std::memcpy(reencoded.data(), "SBDATK01", 8);
  PutUuid(reencoded.data() + 8, profile.receipt.catalog_snapshot_uuid);
  StoreLittle64(reencoded.data() + 24, profile.receipt.catalog_generation);
  StoreLittle64(reencoded.data() + 32, profile.receipt.registry_generation);
  std::memcpy(reencoded.data() + 40, profile.comparison_fingerprint.data(), 32);
  PutPolicy(reencoded.data() + 72, profile.identity.ordering_policy);
  reencoded[96] = encoded[96];
  reencoded[97] = encoded[97];
  reencoded[98] = state == DateValueStateV3::value ? 1
      : (null_mode == DateNullModeV3::nulls_first ? 0 : 2);
  reencoded[99] = state == DateValueStateV3::value ? 4 : 0;
  if (state == DateValueStateV3::value) {
    const u32 sortable = static_cast<u32>(day) ^ 0x80000000u;
    reencoded[100] = static_cast<byte>(sortable >> 24);
    reencoded[101] = static_cast<byte>(sortable >> 16);
    reencoded[102] = static_cast<byte>(sortable >> 8);
    reencoded[103] = static_cast<byte>(sortable);
    if (direction == DateSortDirectionV3::descending)
      for (std::size_t index = 100; index < 104; ++index)
        reencoded[index] = static_cast<byte>(~reencoded[index]);
  }
  if (control.force_reencode_mismatch_for_conformance)
    reencoded[0] ^= 1;
  if (!std::equal(reencoded.begin(),
                  reencoded.begin() + static_cast<std::ptrdiff_t>(encoded.size()),
                  encoded.begin()))
    return Failure<DateSortKeyViewResultV3>("CTI.TEMPORAL.CANONICAL_ENCODING_INVALID",
                                            "K04_reencode");
  if (Cancelled(control))
    return Failure<DateSortKeyViewResultV3>("PROCESS.CANCELLED",
                                            "before_publication");
  auto result = Success<DateSortKeyViewResultV3>();
  result.value = {&profile,direction,null_mode,state,day};
  return result;
}

DateCastPolicyDispositionV3 ClassifyDateCastPolicyRowV3(
    u32 row, DatatypeCastContext context) noexcept {
  if (context != DatatypeCastContext::implicit &&
      context != DatatypeCastContext::assignment &&
      context != DatatypeCastContext::explicit_cast)
    return DateCastPolicyDispositionV3::forbidden;
  if (row == 1) return DateCastPolicyDispositionV3::contextual_null;
  if (row == 53) return DateCastPolicyDispositionV3::identity;
  if (row == 24 && context == DatatypeCastContext::explicit_cast)
    return DateCastPolicyDispositionV3::explicit_character_to_date;
  if (row == 50 && context == DatatypeCastContext::explicit_cast)
    return DateCastPolicyDispositionV3::explicit_date_to_character;
  return DateCastPolicyDispositionV3::forbidden;
}

DateCastResultV3 CastDateValueV3(const DateCastRequestV3& request) noexcept {
  DateCastRowShape shape;
  if (!ResolveDateCastRowShape(request.one_based_policy_row, &shape))
    return Failure<DateCastResultV3>("DATATYPE.CAST_FORBIDDEN",
                                     "date_policy_row_required");
  const auto disposition = ClassifyDateCastPolicyRowV3(
      request.one_based_policy_row, request.context);

  // Row 53 is the sole date-to-date row and therefore has two specialized
  // endpoints.  Every other row has exactly one date endpoint and one scalar
  // endpoint; extra fields are an invalid dynamic request, never an alias for
  // another registry row.
  if (request.one_based_policy_row == 53) {
    const bool has_owned_source = request.date_source != nullptr;
    const bool has_dynamic_source = request.dynamic_date_source != nullptr;
    if (has_owned_source == has_dynamic_source || request.date_target == nullptr ||
        *request.date_target == nullptr ||
        request.scalar_source != nullptr ||
        request.scalar_source_identity != nullptr ||
        request.date_target_descriptor != nullptr ||
        request.scalar_target != CanonicalTypeId::unknown ||
        request.scalar_target_identity != nullptr ||
        ExecutionDescriptorPresentNoAlloc(request.scalar_target_descriptor) ||
        request.use_character_output_buffer || request.character_output != nullptr ||
        request.character_output_capacity != 0)
      return Failure<DateCastResultV3>("CTI.TEMPORAL.DESCRIPTOR_INVALID",
                                       "identity_cast_shape_invalid");
    const auto& source_profile = has_owned_source
        ? request.date_source->profile
        : request.dynamic_date_source->profile;
    if (source_profile == nullptr || !ProfileValidNoAlloc(*source_profile))
      return Failure<DateCastResultV3>("CTI.TEMPORAL.DESCRIPTOR_INVALID",
                                       "identity_source_profile_invalid");
    if (!ProfileValidNoAlloc(**request.date_target) ||
        source_profile->comparison_fingerprint !=
            (*request.date_target)->comparison_fingerprint)
      return Failure<DateCastResultV3>("CTI.TEMPORAL.DESCRIPTOR_INVALID",
                                       "identity_cast_cohort_mismatch");
    const auto source = has_owned_source
        ? ValidateDateValueViewV3(request.date_source->view(), true)
        : AdmitDateOperandV3(*request.dynamic_date_source, true);
    if (!source.ok())
      return Failure<DateCastResultV3>(source.diagnostic.diagnostic_code,
                                       source.diagnostic.detail, source.status);
    if (source.value.state == DateValueStateV3::sql_null &&
        !request.target_null_allowed)
      return Failure<DateCastResultV3>("DATATYPE.NULL_NOT_ADMITTED",
                                       "identity_target_nonnullable");
    if (disposition != DateCastPolicyDispositionV3::identity)
      return Failure<DateCastResultV3>("DATATYPE.CAST_FORBIDDEN",
                                       "identity_context_forbidden");
    auto source_pin = source_profile;
    auto target_pin = *request.date_target;
    if (source.value.state == DateValueStateV3::value &&
        request.control.maximum_allocation_bytes < kDateComponentBytesV3) {
      auto failure = Failure<DateCastResultV3>("RESOURCE.BUDGET_EXCEEDED",
                                                "identity_result_grant",
                                                ResourceStatus());
      failure.bytes_required = kDateComponentBytesV3;
      return failure;
    }
    if (Cancelled(request.control))
      return Failure<DateCastResultV3>("PROCESS.CANCELLED",
                                       "before_cast_work");
    DateOwnedValueV3 staged{std::move(target_pin), source.value.state,
                            source.value.day};
    ScopedOwnedDateValueClear clear_staged(&staged);
    if (Cancelled(request.control))
      return Failure<DateCastResultV3>("PROCESS.CANCELLED",
                                       "before_cast_publication");
    auto result = Success<DateCastResultV3>();
    result.category = DatatypeCastCategory::identity;
    result.produced_date = true;
    result.date_value = std::move(staged);
    return result;
  }

  if (shape.incoming) {
    if ((request.date_target == nullptr || *request.date_target == nullptr) || request.date_source != nullptr ||
        request.dynamic_date_source != nullptr ||
        request.scalar_source == nullptr ||
        request.scalar_target != CanonicalTypeId::unknown ||
        request.scalar_target_identity != nullptr ||
        ExecutionDescriptorPresentNoAlloc(request.scalar_target_descriptor) ||
        request.use_character_output_buffer || request.character_output != nullptr ||
        request.character_output_capacity != 0)
      return Failure<DateCastResultV3>("CTI.TEMPORAL.DESCRIPTOR_INVALID",
                                       "incoming_cast_shape_invalid");
    if (shape.contextual_null) {
      // base.null is the process-local contextual sentinel and deliberately
      // has no fabricated V3 type/codec identity.
      if (!ProfileValidNoAlloc(**request.date_target))
        return Failure<DateCastResultV3>("CTI.TEMPORAL.DESCRIPTOR_INVALID",
                                         "date_target_invalid");
      if (request.date_target_descriptor == nullptr ||
          !DescriptorBindsIdentityNoAlloc(*request.date_target_descriptor,
                                          (*request.date_target)->identity,
                                          CanonicalTypeId::date))
        return Failure<DateCastResultV3>("CTI.TEMPORAL.DESCRIPTOR_INVALID",
                                         "date_target_admission_invalid");
      if (request.scalar_source_identity != nullptr ||
          request.scalar_source->type_id != CanonicalTypeId::null_type)
        return Failure<DateCastResultV3>("CTI.TEMPORAL.DESCRIPTOR_INVALID",
                                         "contextual_null_authority_invalid");
      if (ExecutionDescriptorPresentNoAlloc(request.scalar_source->descriptor))
        return Failure<DateCastResultV3>("CTI.TEMPORAL.DESCRIPTOR_INVALID",
                                         "contextual_null_descriptor_forbidden");
      if (!request.scalar_source->is_null ||
          !request.scalar_source->encoded_value.empty())
        return Failure<DateCastResultV3>("DATATYPE.NULL_STATE.INVALID",
                                         "contextual_null_dirty");
      if (!request.target_null_allowed)
        return Failure<DateCastResultV3>("DATATYPE.NULL_NOT_ADMITTED",
                                         "target_nonnullable");
      if (disposition != DateCastPolicyDispositionV3::contextual_null)
        return Failure<DateCastResultV3>("DATATYPE.CAST_FORBIDDEN",
                                         "contextual_null_context_forbidden");
      auto target_pin = *request.date_target;
      if (Cancelled(request.control))
        return Failure<DateCastResultV3>("PROCESS.CANCELLED",
                                         "before_cast_work");
      DateOwnedValueV3 staged{std::move(target_pin),
                              DateValueStateV3::sql_null, 0};
      ScopedOwnedDateValueClear clear_staged(&staged);
      if (Cancelled(request.control))
        return Failure<DateCastResultV3>("PROCESS.CANCELLED",
                                         "before_cast_publication");
      auto result = Success<DateCastResultV3>();
      result.category = DatatypeCastCategory::identity;
      result.produced_date = true;
      result.date_value = std::move(staged);
      return result;
    }

    // Resolve the complete date target authority before consulting the scalar
    // carrier.  A malformed peer cannot mask a stale or fabricated target.
    if (!ProfileValidNoAlloc(**request.date_target))
      return Failure<DateCastResultV3>("CTI.TEMPORAL.DESCRIPTOR_INVALID",
                                       "date_target_invalid");
    if (request.date_target_descriptor == nullptr ||
        !DescriptorBindsIdentityNoAlloc(*request.date_target_descriptor,
                                        (*request.date_target)->identity,
                                        CanonicalTypeId::date))
      return Failure<DateCastResultV3>("CTI.TEMPORAL.DESCRIPTOR_INVALID",
                                       "date_target_admission_invalid");
    if (shape.peer_type != CanonicalTypeId::unknown &&
        request.scalar_source->type_id != shape.peer_type)
      return Failure<DateCastResultV3>("CTI.TEMPORAL.DESCRIPTOR_INVALID",
                                       "incoming_peer_type_mismatch");
    if (shape.exact_peer_identity) {
      if (!ExactPeerIdentity(request.scalar_source_identity, shape.peer_type) ||
          !DescriptorBindsIdentity(request.scalar_source->descriptor,
                                   *request.scalar_source_identity,
                                   shape.peer_type))
        return Failure<DateCastResultV3>("CTI.TEMPORAL.DESCRIPTOR_INVALID",
                                         "incoming_peer_identity_invalid");
    } else if (request.scalar_source_identity != nullptr) {
      return Failure<DateCastResultV3>("CTI.TEMPORAL.DESCRIPTOR_INVALID",
                                       "unregistered_incoming_identity_claim");
    }
    if (shape.peer_type == CanonicalTypeId::character &&
        !ExactCharacterDescriptor(request.scalar_source->descriptor,
                                  (*request.date_target)->receipt))
      return Failure<DateCastResultV3>("CTI.TEMPORAL.DESCRIPTOR_INVALID",
                                       "character_source_descriptor_invalid");
    if (request.scalar_source->is_null) {
      if (!request.scalar_source->encoded_value.empty())
        return Failure<DateCastResultV3>("DATATYPE.NULL_STATE.INVALID",
                                         "incoming_peer_dirty_null");
      if (!request.target_null_allowed)
        return Failure<DateCastResultV3>("DATATYPE.NULL_NOT_ADMITTED",
                                         "target_nonnullable");
    }
    if (disposition == DateCastPolicyDispositionV3::forbidden)
      return Failure<DateCastResultV3>("DATATYPE.CAST_FORBIDDEN",
                                       "closed_date_cast_policy_forbidden");

    if (disposition !=
            DateCastPolicyDispositionV3::explicit_character_to_date ||
        shape.peer_type != CanonicalTypeId::character)
      return Failure<DateCastResultV3>("CTI.TEMPORAL.DESCRIPTOR_INVALID",
                                       "character_source_descriptor_invalid");
    if (request.scalar_source->is_null) {
      auto target_pin = *request.date_target;
      if (Cancelled(request.control))
        return Failure<DateCastResultV3>("PROCESS.CANCELLED",
                                         "before_cast_work");
      DateOwnedValueV3 staged{std::move(target_pin),
                              DateValueStateV3::sql_null, 0};
      ScopedOwnedDateValueClear clear_staged(&staged);
      if (Cancelled(request.control))
        return Failure<DateCastResultV3>("PROCESS.CANCELLED",
                                         "before_cast_publication");
      auto result = Success<DateCastResultV3>();
      result.category = DatatypeCastCategory::lossless_explicit;
      result.produced_date = true;
      result.date_value = std::move(staged);
      return result;
    }
    auto parsed = ParseCanonicalDateDayNoAlloc(
        request.scalar_source->encoded_value);
    if (!parsed.ok)
      return Failure<DateCastResultV3>(
          parsed.diagnostic_code.empty()
              ? "CTI.TEMPORAL.INVALID_LITERAL"
              : parsed.diagnostic_code,
          parsed.detail.empty()?"character_to_date":parsed.detail);
    if (request.scalar_source->descriptor.length != 0 &&
        request.scalar_source->encoded_value.size() >
            request.scalar_source->descriptor.length)
      return Failure<DateCastResultV3>("CTI.TEMPORAL.RANGE_EXCEEDED",
                                       "character_source_length");
    auto target_pin=*request.date_target;
    if (request.control.maximum_allocation_bytes < kDateComponentBytesV3) {
      auto failure = Failure<DateCastResultV3>("RESOURCE.BUDGET_EXCEEDED",
                                                "date_result_grant",
                                                ResourceStatus());
      failure.bytes_required = kDateComponentBytesV3;
      return failure;
    }
    if (Cancelled(request.control))
      return Failure<DateCastResultV3>("PROCESS.CANCELLED",
                                       "before_cast_work");
    DateOwnedValueV3 staged{std::move(target_pin),DateValueStateV3::value,
                            parsed.day};
    ScopedOwnedDateValueClear clear_staged(&staged);
    if (Cancelled(request.control))
      return Failure<DateCastResultV3>("PROCESS.CANCELLED",
                                       "before_cast_publication");
    auto result = Success<DateCastResultV3>();
    result.category = DatatypeCastCategory::lossless_explicit;
    result.produced_date = true;
    result.date_value = std::move(staged);
    return result;
  }

  const bool has_owned_source = request.date_source != nullptr;
  const bool has_dynamic_source = request.dynamic_date_source != nullptr;
  if (has_owned_source == has_dynamic_source || request.date_target != nullptr ||
      request.date_target_descriptor != nullptr ||
      request.scalar_source != nullptr ||
      request.scalar_source_identity != nullptr ||
      request.scalar_target != shape.peer_type ||
      ((request.one_based_policy_row != 50 || !request.use_character_output_buffer) &&
       (request.character_output != nullptr ||
        request.character_output_capacity != 0)) ||
      (request.one_based_policy_row != 50 && request.use_character_output_buffer))
    return Failure<DateCastResultV3>("CTI.TEMPORAL.DESCRIPTOR_INVALID",
                                     "outgoing_cast_shape_invalid");
  const auto& source_profile = has_owned_source
      ? request.date_source->profile
      : request.dynamic_date_source->profile;
  if (source_profile == nullptr || !ProfileValidNoAlloc(*source_profile))
    return Failure<DateCastResultV3>("CTI.TEMPORAL.DESCRIPTOR_INVALID",
                                     "outgoing_source_profile_invalid");
  if (shape.exact_peer_identity) {
    if (!ExactPeerIdentity(request.scalar_target_identity, shape.peer_type) ||
        !DescriptorBindsIdentity(request.scalar_target_descriptor,
                                 *request.scalar_target_identity,
                                 shape.peer_type))
      return Failure<DateCastResultV3>("CTI.TEMPORAL.DESCRIPTOR_INVALID",
                                       "outgoing_peer_identity_invalid");
  } else if (request.scalar_target_identity != nullptr) {
    return Failure<DateCastResultV3>("CTI.TEMPORAL.DESCRIPTOR_INVALID",
                                     "unregistered_outgoing_identity_claim");
  }
  if (shape.peer_type == CanonicalTypeId::character &&
      !ExactCharacterDescriptor(request.scalar_target_descriptor,
                                source_profile->receipt))
    return Failure<DateCastResultV3>("CTI.TEMPORAL.DESCRIPTOR_INVALID",
                                     "character_target_descriptor_invalid");
  const auto source = has_owned_source
      ? ValidateDateValueViewV3(request.date_source->view(), true)
      : AdmitDateOperandV3(*request.dynamic_date_source, true);
  if (!source.ok())
    return Failure<DateCastResultV3>(source.diagnostic.diagnostic_code,
                                     source.diagnostic.detail, source.status);
  if (source.value.state == DateValueStateV3::sql_null &&
      (!request.target_null_allowed ||
       !request.scalar_target_descriptor.nullable_allowed))
    return Failure<DateCastResultV3>("DATATYPE.NULL_NOT_ADMITTED",
                                     "scalar_target_nonnullable");
  if (disposition == DateCastPolicyDispositionV3::forbidden)
    return Failure<DateCastResultV3>("DATATYPE.CAST_FORBIDDEN",
                                     "closed_date_cast_policy_forbidden");
  if (disposition != DateCastPolicyDispositionV3::explicit_date_to_character ||
      shape.peer_type != CanonicalTypeId::character)
    return Failure<DateCastResultV3>("CTI.TEMPORAL.DESCRIPTOR_INVALID",
                                     "character_target_descriptor_invalid");
  u64 publication_extent = 0;
  if (source.value.state == DateValueStateV3::value) {
    const auto civil = DaysToCivil(source.value.day);
    publication_extent = civil.year >= 0 && civil.year <= 9999 ? 10u : 14u;
  }
  auto source_pin = source_profile;
  if (request.use_character_output_buffer &&
      request.character_output != nullptr && publication_extent != 0) {
    if ((has_owned_source
             ? OutputOverlapsOwnedDateInput(*request.date_source,
                                            request.character_output,
                                            publication_extent)
             : OutputOverlapsDateOperand(*request.dynamic_date_source,
                                         request.character_output,
                                         publication_extent)) ||
        RangesOverlap(request.character_output, publication_extent, &request,
                      sizeof(request)) ||
        (request.scalar_target_identity != nullptr &&
         RangesOverlap(request.character_output, publication_extent,
                       request.scalar_target_identity,
                       sizeof(*request.scalar_target_identity))) ||
        (request.scalar_target_identity != nullptr &&
         OutputOverlapsIdentityBuffers(request.character_output,
                                       publication_extent,
                                       *request.scalar_target_identity)) ||
        RangesOverlap(request.character_output, publication_extent,
                      &request.scalar_target_descriptor,
                      sizeof(request.scalar_target_descriptor)) ||
        OutputOverlapsDescriptorBuffers(request.character_output,
                                        publication_extent,
                                        request.scalar_target_descriptor)) {
      return Failure<DateCastResultV3>(
          "CTI.TEMPORAL.CANONICAL_ENCODING_INVALID",
          "character_output_aliases_input_or_profile");
    }
  }
  if (publication_extent != 0 &&
      request.scalar_target_descriptor.length != 0 &&
      publication_extent > request.scalar_target_descriptor.length) {
    auto failure = Failure<DateCastResultV3>("CTB.TEXT.LENGTH_EXCEEDED",
                                              "character_target_length");
    failure.bytes_required = publication_extent;
    return failure;
  }
  if (request.use_character_output_buffer &&
      (publication_extent > request.character_output_capacity ||
       (publication_extent != 0 && request.character_output == nullptr))) {
    auto failure = Failure<DateCastResultV3>("CTB.TEXT.LENGTH_EXCEEDED",
                                             "character_output_capacity");
    failure.bytes_required = publication_extent;
    return failure;
  }
  if (publication_extent > request.control.maximum_allocation_bytes) {
    auto failure = Failure<DateCastResultV3>("RESOURCE.BUDGET_EXCEEDED",
                                              "render_resource_grant",
                                              ResourceStatus());
    failure.bytes_required = publication_extent;
    return failure;
  }
  if (Cancelled(request.control))
    return Failure<DateCastResultV3>("PROCESS.CANCELLED",
                                     "before_cast_work");
  std::array<char, 15> rendered_bytes{};
  ScopedSecureClear clear_rendered(
      rendered_bytes.data(), rendered_bytes.size(),
      DateScrubClassV3::character_render_staging,
      request.control.observe_scrubbed,
      request.control.scrub_observer_context);
  const bool containing_null =
      source.value.state == DateValueStateV3::sql_null;
  u64 required = 0;
  if (!containing_null) {
    const auto civil = DaysToCivil(source.value.day);
    const int written = civil.year >= 0 && civil.year <= 9999
        ? std::snprintf(rendered_bytes.data(), rendered_bytes.size(),
                        "%04d-%02u-%02u", civil.year, civil.month, civil.day)
        : std::snprintf(rendered_bytes.data(), rendered_bytes.size(),
                        "%c%07lld-%02u-%02u", civil.year < 0 ? '-' : '+',
                        static_cast<long long>(civil.year < 0
                            ? -static_cast<std::int64_t>(civil.year)
                            : static_cast<std::int64_t>(civil.year)),
                        civil.month, civil.day);
    if (written != 10 && written != 14)
      return Failure<DateCastResultV3>(
          "CTI.TEMPORAL.CANONICAL_ENCODING_INVALID", "date_to_character");
    required = static_cast<u64>(written);
  }
  if (!containing_null && request.scalar_target_descriptor.length != 0 &&
      required > request.scalar_target_descriptor.length) {
    auto failure = Failure<DateCastResultV3>("CTB.TEXT.LENGTH_EXCEEDED",
                                              "character_target_length");
    failure.bytes_required = required;
    return failure;
  }
  if (request.use_character_output_buffer) {
    if (request.character_output != nullptr && required != 0 &&
        ((has_owned_source
              ? OutputOverlapsOwnedDateInput(*request.date_source,
                                             request.character_output, required)
              : OutputOverlapsDateOperand(*request.dynamic_date_source,
                                          request.character_output, required)) ||
         RangesOverlap(request.character_output, required, &request,
                       sizeof(request)) ||
         (request.scalar_target_identity != nullptr &&
          RangesOverlap(request.character_output, required,
                        request.scalar_target_identity,
                        sizeof(*request.scalar_target_identity))) ||
         (request.scalar_target_identity != nullptr &&
          OutputOverlapsIdentityBuffers(request.character_output, required,
                                        *request.scalar_target_identity)) ||
         RangesOverlap(request.character_output, required,
                       &request.scalar_target_descriptor,
                       sizeof(request.scalar_target_descriptor)) ||
         OutputOverlapsDescriptorBuffers(request.character_output, required,
                                         request.scalar_target_descriptor)))
      return Failure<DateCastResultV3>(
          "CTI.TEMPORAL.CANONICAL_ENCODING_INVALID",
          "character_output_aliases_input_or_profile");
    if (required > request.character_output_capacity ||
        (required != 0 && request.character_output == nullptr)) {
      auto failure = Failure<DateCastResultV3>("CTB.TEXT.LENGTH_EXCEEDED",
                                               "character_output_capacity");
      failure.bytes_required = required;
      return failure;
    }
    if (Cancelled(request.control))
      return Failure<DateCastResultV3>("PROCESS.CANCELLED",
                                       "before_cast_publication");
    if (required != 0)
      std::memcpy(request.character_output, rendered_bytes.data(), required);
    auto result = Success<DateCastResultV3>();
    result.category = DatatypeCastCategory::lossless_explicit;
    result.used_character_output_buffer = true;
    result.bytes_required = required;
    result.bytes_written = required;
    result.scalar_value.type_id = CanonicalTypeId::character;
    result.scalar_value.is_null = containing_null;
    return result;
  }
  try {
    auto result = Success<DateCastResultV3>();
    ScopedStringSecureClear clear_result(
        &result.scalar_value.encoded_value,
        DateScrubClassV3::character_render_staging,
        request.control.observe_scrubbed,
        request.control.scrub_observer_context);
    result.category = DatatypeCastCategory::lossless_explicit;
    result.scalar_value.type_id = CanonicalTypeId::character;
    result.scalar_value.descriptor = request.scalar_target_descriptor;
    result.scalar_value.is_null = containing_null;
    result.bytes_required = required;
    result.bytes_written = required;
    result.scalar_value.encoded_value.assign(rendered_bytes.data(), required);
    if (Cancelled(request.control))
      return Failure<DateCastResultV3>("PROCESS.CANCELLED",
                                       "before_cast_publication");
    clear_result.Disarm();
    return result;
  } catch (...) {
    return Failure<DateCastResultV3>("RESOURCE.BUDGET_EXCEEDED",
                                     "cast_result_allocation",
                                     ResourceStatus());
  }
}

DateValidationResultV3 ValidateDateBatchViewV3(const DateBatchViewV3& batch) noexcept {
  if (batch.profile == nullptr || !ProfileValidNoAlloc(*batch.profile))
    return Failure<DateValidationResultV3>("CTI.TEMPORAL.DESCRIPTOR_INVALID",
                                     "batch_profile_invalid");
  // The admitted batch cardinality is u32.  This precedes all extent math,
  // bitmap access, and row reads so an overlarge synthetic span is refused
  // without dereferencing its carrier.
  if (batch.days.size() > std::numeric_limits<u32>::max())
    return Failure<DateValidationResultV3>("CTI.TEMPORAL.RANGE_EXCEEDED",
                                           "batch_row_count");
  if (batch.days.size() >
      std::numeric_limits<std::size_t>::max() / sizeof(std::int32_t))
    return Failure<DateValidationResultV3>("CTI.TEMPORAL.CANONICAL_ENCODING_INVALID",
                                     "batch_days_extent_overflow");
  if (!batch.days.empty() &&
      reinterpret_cast<std::uintptr_t>(batch.days.data()) %
              alignof(std::int32_t) !=
          0)
    return Failure<DateValidationResultV3>("CTI.TEMPORAL.CANONICAL_ENCODING_INVALID",
                                     "batch_days_alignment");
  const std::size_t days_bytes = batch.days.size() * sizeof(std::int32_t);
  if (RangesOverlap(batch.days.data(), days_bytes,
                    batch.null_bitmap_lsb0.data(),
                    batch.null_bitmap_lsb0.size()))
    return Failure<DateValidationResultV3>("CTI.TEMPORAL.CANONICAL_ENCODING_INVALID",
                                     "batch_input_spans_overlap");
  const std::size_t expected_bitmap = batch.days.size() / 8 +
      (batch.days.size() % 8 == 0 ? 0 : 1);
  if (batch.null_bitmap_lsb0.size() != expected_bitmap)
    return Failure<DateValidationResultV3>("CTI.TEMPORAL.CANONICAL_ENCODING_INVALID",
                                     "batch_bitmap_extent");
  if (!batch.null_bitmap_lsb0.empty() && batch.days.size() % 8 != 0) {
    const byte valid = static_cast<byte>((1u << (batch.days.size() % 8)) - 1u);
    if ((batch.null_bitmap_lsb0.back() & static_cast<byte>(~valid)) != 0)
      return Failure<DateValidationResultV3>("CTI.TEMPORAL.CANONICAL_ENCODING_INVALID",
                                       "batch_bitmap_tail");
  }
  for (std::size_t index = 0; index < batch.days.size(); ++index) {
    const bool is_null = (batch.null_bitmap_lsb0[index >> 3] &
                          static_cast<byte>(1u << (index & 7))) != 0;
    if (is_null && batch.days[index] != 0)
      return Failure<DateValidationResultV3>("DATATYPE.NULL_STATE.INVALID",
                                       "batch_dirty_null_slot");
  }
  auto result = Success<DateValidationResultV3>();
  return result;
}

DateBatchExtentsResultV3 ComputeDateBatchExtentsV3(u64 row_count,
                                                   u64 size_limit) noexcept {
  if (row_count > std::numeric_limits<u32>::max())
    return Failure<DateBatchExtentsResultV3>(
        "CTI.TEMPORAL.RANGE_EXCEEDED", "batch_row_count");
  const u64 days_bytes = row_count * 4u;
  const u64 bitmap_bytes = row_count / 8u + (row_count % 8u != 0u ? 1u : 0u);
  if (days_bytes > std::numeric_limits<u64>::max() - bitmap_bytes)
    return Failure<DateBatchExtentsResultV3>(
        "CTI.TEMPORAL.RANGE_EXCEEDED", "batch_extent_overflow");
  const u64 combined = days_bytes + bitmap_bytes;
  const auto size_max = static_cast<u64>(std::numeric_limits<std::size_t>::max());
  const std::vector<std::int32_t> empty_days;
  const std::vector<byte> empty_bitmap;
  if (days_bytes > size_max || bitmap_bytes > size_max || combined > size_max ||
      row_count > static_cast<u64>(empty_days.max_size()) ||
      bitmap_bytes > static_cast<u64>(empty_bitmap.max_size())) {
    auto failure = Failure<DateBatchExtentsResultV3>(
        "RESOURCE.BUDGET_EXCEEDED", "batch_extent_host_limit",
        ResourceStatus());
    failure.days_bytes = days_bytes;
    failure.bitmap_bytes = bitmap_bytes;
    failure.combined_bytes = combined;
    return failure;
  }
  if (days_bytes > size_limit || bitmap_bytes > size_limit ||
      combined > size_limit) {
    auto failure = Failure<DateBatchExtentsResultV3>(
        "RESOURCE.BUDGET_EXCEEDED", "batch_extent_size_limit",
        ResourceStatus());
    failure.days_bytes = days_bytes;
    failure.bitmap_bytes = bitmap_bytes;
    failure.combined_bytes = combined;
    return failure;
  }
  auto result = Success<DateBatchExtentsResultV3>();
  result.days_bytes = days_bytes;
  result.bitmap_bytes = bitmap_bytes;
  result.combined_bytes = combined;
  return result;
}

DateBatchExtentsResultV3 MaterializeDateBatchIntoV3(
    const std::shared_ptr<const DateValidatedProfileHandleV3>& profile,
    std::span<const std::int32_t> days,
    std::span<const byte> null_bitmap_lsb0,
    std::int32_t* output_days, u64 output_days_bytes,
    byte* output_null_bitmap_lsb0, u64 output_bitmap_bytes,
    const DateExecutionControlV3& control) noexcept {
  const auto checked = ValidateDateBatchViewV3(
      {profile.get(), days, null_bitmap_lsb0});
  if (!checked.ok())
    return Failure<DateBatchExtentsResultV3>(
        checked.diagnostic.diagnostic_code, checked.diagnostic.detail,
        checked.status);
  const auto extents = ComputeDateBatchExtentsV3(
      days.size(), control.maximum_allocation_bytes);
  if (!extents.ok()) return extents;
  const auto days_bytes = static_cast<std::size_t>(extents.days_bytes);
  const auto bitmap_bytes = static_cast<std::size_t>(extents.bitmap_bytes);

  // Detect aliasing over the exact publication extents even when a caller also
  // supplies a short capacity.  Alias is the earlier canonical-integrity gate.
  const auto aliases_nonprofile = [&](const void* output,
                                      std::size_t bytes) noexcept {
    return RangesOverlap(output, bytes, days.data(), days_bytes) ||
        RangesOverlap(output, bytes, null_bitmap_lsb0.data(), bitmap_bytes) ||
        RangesOverlap(output, bytes, &profile, sizeof(profile)) ||
        RangesOverlap(output, bytes, &control, sizeof(control));
  };
  const bool profile_overlap = profile != nullptr &&
      (OutputOverlapsProfile(output_days, days_bytes, *profile) ||
       OutputOverlapsProfile(output_null_bitmap_lsb0, bitmap_bytes, *profile));
  if (profile_overlap) {
    auto refusal_pin = profile;
    (void)refusal_pin;
    return Failure<DateBatchExtentsResultV3>(
        "CTI.TEMPORAL.CANONICAL_ENCODING_INVALID",
        "batch_publication_profile_overlap");
  }
  if (aliases_nonprofile(output_days, days_bytes) ||
      aliases_nonprofile(output_null_bitmap_lsb0, bitmap_bytes) ||
      RangesOverlap(output_days, days_bytes, output_null_bitmap_lsb0,
                    bitmap_bytes))
    return Failure<DateBatchExtentsResultV3>(
        "CTI.TEMPORAL.CANONICAL_ENCODING_INVALID",
        "batch_publication_spans_overlap");
  if (days_bytes != 0 && output_days != nullptr &&
      reinterpret_cast<std::uintptr_t>(output_days) %
              alignof(std::int32_t) !=
          0)
    return Failure<DateBatchExtentsResultV3>(
        "CTI.TEMPORAL.CANONICAL_ENCODING_INVALID",
        "batch_output_days_alignment");
  if ((days_bytes != 0 && output_days == nullptr) ||
      output_days_bytes < extents.days_bytes ||
      (bitmap_bytes != 0 && output_null_bitmap_lsb0 == nullptr) ||
      output_bitmap_bytes < extents.bitmap_bytes) {
    auto failure = Failure<DateBatchExtentsResultV3>(
        "RESOURCE.BUDGET_EXCEEDED", "batch_caller_capacity",
        ResourceStatus());
    failure.days_bytes = extents.days_bytes;
    failure.bitmap_bytes = extents.bitmap_bytes;
    failure.combined_bytes = extents.combined_bytes;
    return failure;
  }
  if (Cancelled(control))
    return Failure<DateBatchExtentsResultV3>("PROCESS.CANCELLED",
                                              "batch_before_allocation");

  // Acquire the one publication pin only after the no-allocation gates and the
  // before-allocation cancellation checkpoint have passed.
  auto staged_profile = profile;
  std::vector<std::int32_t> staged_days;
  std::vector<byte> staged_bitmap;
  ScopedVectorSecureClear<std::int32_t> clear_days(
      &staged_days, DateScrubClassV3::batch_days_staging,
      control.observe_scrubbed, control.scrub_observer_context);
  ScopedVectorSecureClear<byte> clear_bitmap(
      &staged_bitmap, DateScrubClassV3::batch_bitmap_staging,
      control.observe_scrubbed, control.scrub_observer_context);
  try {
    staged_days.resize(days.size());
    staged_bitmap.resize(null_bitmap_lsb0.size());
  } catch (const std::bad_alloc&) {
    return Failure<DateBatchExtentsResultV3>("RESOURCE.BUDGET_EXCEEDED",
                                              "batch_allocation",
                                              ResourceStatus());
  } catch (const std::length_error&) {
    return Failure<DateBatchExtentsResultV3>("RESOURCE.BUDGET_EXCEEDED",
                                              "batch_allocation_length",
                                              ResourceStatus());
  } catch (...) {
    return Failure<DateBatchExtentsResultV3>("RESOURCE.BUDGET_EXCEEDED",
                                              "batch_allocation_exception",
                                              ResourceStatus());
  }
  if (Cancelled(control))
    return Failure<DateBatchExtentsResultV3>("PROCESS.CANCELLED",
                                              "batch_row0");
  for (std::size_t block = 0; block < days.size(); block += 4096) {
    if (block != 0 && Cancelled(control))
      return Failure<DateBatchExtentsResultV3>("PROCESS.CANCELLED",
                                                "batch_row4096");
    const auto end = std::min<std::size_t>(days.size(), block + 4096);
    std::copy(days.begin() + static_cast<std::ptrdiff_t>(block),
              days.begin() + static_cast<std::ptrdiff_t>(end),
              staged_days.begin() + static_cast<std::ptrdiff_t>(block));
    const auto bitmap_begin = block / 8;
    const auto bitmap_end = (end + 7) / 8;
    std::copy(null_bitmap_lsb0.begin() +
                  static_cast<std::ptrdiff_t>(bitmap_begin),
              null_bitmap_lsb0.begin() +
                  static_cast<std::ptrdiff_t>(bitmap_end),
              staged_bitmap.begin() +
                  static_cast<std::ptrdiff_t>(bitmap_begin));
  }
  if (Cancelled(control))
    return Failure<DateBatchExtentsResultV3>("PROCESS.CANCELLED",
                                              "batch_before_publication");
  if (days_bytes != 0) std::memcpy(output_days, staged_days.data(), days_bytes);
  if (bitmap_bytes != 0)
    std::memcpy(output_null_bitmap_lsb0, staged_bitmap.data(), bitmap_bytes);
  (void)staged_profile;
  return extents;
}

DateBatchResultV3 MaterializeDateBatchV3(
    std::shared_ptr<const DateValidatedProfileHandleV3> profile,
    std::span<const std::int32_t> days,
    std::span<const byte> null_bitmap_lsb0,
    const DateExecutionControlV3& control) noexcept {
  const auto checked = ValidateDateBatchViewV3(
      {profile.get(), days, null_bitmap_lsb0});
  if (!checked.ok())
    return Failure<DateBatchResultV3>(checked.diagnostic.diagnostic_code,
                                      checked.diagnostic.detail, checked.status);
  const auto extents = ComputeDateBatchExtentsV3(days.size(),
                                                 control.maximum_allocation_bytes);
  if (!extents.ok())
    return Failure<DateBatchResultV3>(extents.diagnostic.diagnostic_code,
                                      "batch_extents", extents.status);
  if (Cancelled(control))
    return Failure<DateBatchResultV3>("PROCESS.CANCELLED",
                                      "batch_before_allocation");
  DateOwnedBatchV3 staged;
  ScopedVectorSecureClear<std::int32_t> clear_days(
      &staged.days, DateScrubClassV3::batch_days_staging,
      control.observe_scrubbed, control.scrub_observer_context);
  ScopedVectorSecureClear<byte> clear_bitmap(
      &staged.null_bitmap_lsb0, DateScrubClassV3::batch_bitmap_staging,
      control.observe_scrubbed, control.scrub_observer_context);
  staged.profile = std::move(profile);
  try {
    staged.days.resize(days.size());
    staged.null_bitmap_lsb0.resize(null_bitmap_lsb0.size());
  } catch (const std::bad_alloc&) {
    return Failure<DateBatchResultV3>("RESOURCE.BUDGET_EXCEEDED",
                                      "batch_allocation", ResourceStatus());
  } catch (const std::length_error&) {
    return Failure<DateBatchResultV3>("RESOURCE.BUDGET_EXCEEDED",
                                      "batch_allocation_length",
                                      ResourceStatus());
  } catch (...) {
    return Failure<DateBatchResultV3>("RESOURCE.BUDGET_EXCEEDED",
                                      "batch_allocation_exception",
                                      ResourceStatus());
  }
  if (Cancelled(control))
    return Failure<DateBatchResultV3>("PROCESS.CANCELLED", "batch_row0");
  for (std::size_t block = 0; block < days.size(); block += 4096) {
    if (block != 0 && Cancelled(control))
      return Failure<DateBatchResultV3>("PROCESS.CANCELLED",
                                        "batch_row4096");
    const auto end = std::min<std::size_t>(days.size(), block + 4096);
    std::copy(days.begin() + static_cast<std::ptrdiff_t>(block),
              days.begin() + static_cast<std::ptrdiff_t>(end),
              staged.days.begin() + static_cast<std::ptrdiff_t>(block));
    const auto bitmap_begin = block / 8;
    const auto bitmap_end = (end + 7) / 8;
    std::copy(null_bitmap_lsb0.begin() +
                  static_cast<std::ptrdiff_t>(bitmap_begin),
              null_bitmap_lsb0.begin() +
                  static_cast<std::ptrdiff_t>(bitmap_end),
              staged.null_bitmap_lsb0.begin() +
                  static_cast<std::ptrdiff_t>(bitmap_begin));
  }
  if (Cancelled(control))
    return Failure<DateBatchResultV3>("PROCESS.CANCELLED",
                                      "batch_before_publication");
  auto result = Success<DateBatchResultV3>();
  result.batch = std::move(staged);
  clear_days.Disarm();
  clear_bitmap.Disarm();
  return result;
}

DateViewResultV3 DecodeDateSbdvalComposedNoAllocV3(
    const DateValidatedProfileHandleV3& profile, bool null_allowed,
    std::span<const byte> encoded,
    const DateExecutionControlV3& control) noexcept {
  const auto frame = DecodeDatatypeBinaryStructuralValueViewNoAlloc(
      encoded.empty() ? nullptr : encoded.data(), encoded.size());
  if (!frame.ok()) {
    const auto& code = frame.diagnostic.diagnostic_code;
    // The structural decoder has already accepted header, extent, flags, and
    // checksum before it can report a NULL-state contradiction.  At that safe
    // point the containing date adapter must dispatch exact type 400 before
    // consulting the supplied date profile or datatype state.
    if (code == "DATATYPE.NULL_STATE.INVALID" && encoded.size() >= 32 &&
        static_cast<CanonicalTypeId>(LoadLittle32(encoded.data() + 8)) !=
            CanonicalTypeId::date)
      return Failure<DateViewResultV3>("CTI.TEMPORAL.DESCRIPTOR_INVALID",
                                       "sbdval_type");
    if (code == "DATATYPE.NULL_STATE.INVALID" &&
        !ProfileValidNoAlloc(profile, &control))
      return Failure<DateViewResultV3>("CTI.TEMPORAL.DESCRIPTOR_INVALID",
                                       "sbdval_profile");
    return Failure<DateViewResultV3>(
        code == "DATATYPE.NULL_STATE.INVALID"
            ? "DATATYPE.NULL_STATE.INVALID"
            : "CTI.TEMPORAL.CANONICAL_ENCODING_INVALID",
        "sbdval_structure", frame.status);
  }
  if (frame.value.type_id != CanonicalTypeId::date)
    return Failure<DateViewResultV3>("CTI.TEMPORAL.DESCRIPTOR_INVALID",
                                     "sbdval_type");
  if (!ProfileValidNoAlloc(profile, &control))
    return Failure<DateViewResultV3>("CTI.TEMPORAL.DESCRIPTOR_INVALID",
                                     "sbdval_profile");
  if (frame.value.payload_is_toast_reference)
    return Failure<DateViewResultV3>("CTI.TEMPORAL.CANONICAL_ENCODING_INVALID",
                                     "sbdval_toast_refused");
  auto component_control = control;
  component_control.force_reencode_mismatch_for_conformance = false;
  auto decoded = DecodeCanonicalDateComponentNoAllocV3(
      profile, frame.value.is_null ? DateValueStateV3::sql_null
                                   : DateValueStateV3::value,
      null_allowed,
      std::span<const byte>(frame.value.payload_data, frame.value.payload_bytes),
      component_control);
  if (!decoded.ok()) return decoded;
  ScopedDateValueViewClear clear_decoded(&decoded.value);
  std::array<byte, 36> reencoded{};
  ScopedSecureClear clear_reencoded(
      reencoded.data(), reencoded.size(), DateScrubClassV3::sbdval_reencode,
      control.observe_scrubbed, control.scrub_observer_context);
  const DatatypeBinaryValueView structural{
      frame.value.type_id, frame.value.is_null,
      frame.value.payload_is_toast_reference, frame.value.payload_data,
      frame.value.payload_bytes};
  const auto rewritten = EncodeDatatypeBinaryStructuralValueIntoNoAlloc(
      structural, reencoded.data(), encoded.size());
  if (control.force_reencode_mismatch_for_conformance)
    reencoded[0] ^= 1;
  if (!rewritten.ok() || rewritten.bytes_written != encoded.size() ||
      !std::equal(reencoded.begin(),
                  reencoded.begin() + static_cast<std::ptrdiff_t>(encoded.size()),
                  encoded.begin()))
    return Failure<DateViewResultV3>(
        "CTI.TEMPORAL.CANONICAL_ENCODING_INVALID", "V08_reencode");
  if (Cancelled(control))
    return Failure<DateViewResultV3>("PROCESS.CANCELLED",
                                      "sbdval_before_publication");
  auto result=Success<DateViewResultV3>();result.value={&profile,decoded.value.state,decoded.value.day};return result;
}

DateBytesResultV3 EncodeDateSbdvalComposedV3(
    const DateOwnedValueV3& owned_value, bool null_allowed,
    const DateExecutionControlV3& control) noexcept {
  const auto value = owned_value.view();
  const auto checked = ValidateDateValueViewV3(value, null_allowed);
  if (!checked.ok())
    return Failure<DateBytesResultV3>(checked.diagnostic.diagnostic_code,
                                      checked.diagnostic.detail, checked.status);
  const std::size_t payload_bytes = value.state == DateValueStateV3::value ? 4 : 0;
  const std::size_t total = 32 + payload_bytes;
  auto profile_pin = owned_value.profile;
  if (total > control.maximum_allocation_bytes)
    return Failure<DateBytesResultV3>("RESOURCE.BUDGET_EXCEEDED",
                                      "sbdval_capacity", ResourceStatus());
  if (Cancelled(control))
    return Failure<DateBytesResultV3>("PROCESS.CANCELLED", "before_allocation");
  std::array<byte, 4> component{};
  ScopedSecureClear clear_component(
      component.data(), component.size(),
      DateScrubClassV3::sbdval_component_staging,
      control.observe_scrubbed, control.scrub_observer_context);
  if (payload_bytes) StoreLittle32(component.data(), static_cast<u32>(value.day));
  auto result = Success<DateBytesResultV3>();
  ScopedVectorSecureClear<byte> clear_result(
      &result.bytes, DateScrubClassV3::sbdval_reencode,
      control.observe_scrubbed, control.scrub_observer_context);
  try { result.bytes.resize(total); }
  catch (...) {
    return Failure<DateBytesResultV3>("RESOURCE.BUDGET_EXCEEDED",
                                      "sbdval_allocation", ResourceStatus());
  }
  const DatatypeBinaryValueView structural{
      CanonicalTypeId::date, value.state == DateValueStateV3::sql_null, false,
      payload_bytes ? component.data() : nullptr, payload_bytes};
  const auto encoded_result = EncodeDatatypeBinaryStructuralValueIntoNoAlloc(
      structural, result.bytes.data(), result.bytes.size());
  if (!encoded_result.ok() || encoded_result.bytes_written != total)
    return Failure<DateBytesResultV3>(encoded_result.diagnostic.diagnostic_code,
                                      "sbdval_encode", encoded_result.status);
  const auto recheck = DecodeDateSbdvalComposedNoAllocV3(
      *value.profile, null_allowed, result.bytes, {});
  if (!recheck.ok())
    return Failure<DateBytesResultV3>(recheck.diagnostic.diagnostic_code,
                                      "sbdval_recheck", recheck.status);
  if (Cancelled(control))
    return Failure<DateBytesResultV3>("PROCESS.CANCELLED", "before_publication");
  clear_result.Disarm();
  return result;
}

DateViewResultV3 DecodeDateSbdpvComposedNoAllocV3(
    const DateValidatedProfileHandleV3& profile, bool null_allowed,
    std::span<const byte> encoded,
    const DateExecutionControlV3& control) noexcept {
  const auto frame = DecodeDatatypePhysicalStructuralValueViewNoAlloc(
      encoded.empty() ? nullptr : encoded.data(), encoded.size());
  if (!frame.ok()) {
    const auto& code = frame.diagnostic.diagnostic_code;
    bool null_state = code == "DATATYPE.NULL_STATE.INVALID";
    const bool state_refusal = null_state ||
        code == "SB-DATATYPE-PHYSICAL-PAYLOAD-REFUSED";
    // As above, these state diagnostics are reachable only after structural
    // extent/reserved/checksum validation, so exact date type dispatch is safe.
    if (state_refusal && encoded.size() >= 24 &&
        static_cast<CanonicalTypeId>(LoadLittle32(encoded.data() + 8)) !=
            CanonicalTypeId::date)
      return Failure<DateViewResultV3>("CTI.TEMPORAL.DESCRIPTOR_INVALID",
                                       "sbdpv_type");
    if (code == "SB-DATATYPE-PHYSICAL-PAYLOAD-REFUSED" &&
        encoded.size() >= 24) {
      const u16 state = LoadLittle16(encoded.data() + 12);
      null_state = state != static_cast<u16>(DatatypePhysicalValueState::value);
    }
    if (null_state && !ProfileValidNoAlloc(profile, &control))
      return Failure<DateViewResultV3>("CTI.TEMPORAL.DESCRIPTOR_INVALID",
                                       "sbdpv_profile");
    return Failure<DateViewResultV3>(
        null_state ? "DATATYPE.NULL_STATE.INVALID"
                   : "CTI.TEMPORAL.CANONICAL_ENCODING_INVALID",
        "sbdpv_structure", frame.status);
  }
  if (frame.value.type_id != CanonicalTypeId::date)
    return Failure<DateViewResultV3>("CTI.TEMPORAL.DESCRIPTOR_INVALID",
                                     "sbdpv_type");
  if (!ProfileValidNoAlloc(profile, &control))
    return Failure<DateViewResultV3>("CTI.TEMPORAL.DESCRIPTOR_INVALID",
                                     "sbdpv_profile");
  if (frame.value.state != DatatypePhysicalValueState::value &&
      frame.value.state != DatatypePhysicalValueState::sql_null)
    return Failure<DateViewResultV3>("DATATYPE.NULL_STATE.INVALID",
                                     "sbdpv_state");
  auto component_control = control;
  component_control.force_reencode_mismatch_for_conformance = false;
  auto decoded = DecodeCanonicalDateComponentNoAllocV3(
      profile,
      frame.value.state == DatatypePhysicalValueState::sql_null
          ? DateValueStateV3::sql_null : DateValueStateV3::value,
      null_allowed,
      std::span<const byte>(frame.value.payload_data, frame.value.payload_bytes),
      component_control);
  if (!decoded.ok()) return decoded;
  ScopedDateValueViewClear clear_decoded(&decoded.value);
  std::array<byte, 28> reencoded{};
  ScopedSecureClear clear_reencoded(
      reencoded.data(), reencoded.size(), DateScrubClassV3::sbdpv_reencode,
      control.observe_scrubbed, control.scrub_observer_context);
  const DatatypePhysicalValueView structural{
      frame.value.type_id, frame.value.state, frame.value.payload_data,
      frame.value.payload_bytes};
  const auto rewritten = EncodeDatatypePhysicalStructuralValueIntoNoAlloc(
      structural, reencoded.data(), encoded.size());
  if (control.force_reencode_mismatch_for_conformance)
    reencoded[0] ^= 1;
  if (!rewritten.ok() || rewritten.bytes_written != encoded.size() ||
      !std::equal(reencoded.begin(),
                  reencoded.begin() + static_cast<std::ptrdiff_t>(encoded.size()),
                  encoded.begin()))
    return Failure<DateViewResultV3>(
        "CTI.TEMPORAL.CANONICAL_ENCODING_INVALID", "PV08_reencode");
  if (Cancelled(control))
    return Failure<DateViewResultV3>("PROCESS.CANCELLED",
                                      "sbdpv_before_publication");
  auto result=Success<DateViewResultV3>();result.value={&profile,decoded.value.state,decoded.value.day};return result;
}

DateBytesResultV3 EncodeDateSbdpvComposedV3(
    const DateOwnedValueV3& owned_value, bool null_allowed,
    const DateExecutionControlV3& control) noexcept {
  const auto value = owned_value.view();
  const auto checked = ValidateDateValueViewV3(value, null_allowed);
  if (!checked.ok())
    return Failure<DateBytesResultV3>(checked.diagnostic.diagnostic_code,
                                      checked.diagnostic.detail, checked.status);
  const std::size_t payload_bytes = value.state == DateValueStateV3::value ? 4 : 0;
  const std::size_t total = 24 + payload_bytes;
  auto profile_pin = owned_value.profile;
  if (total > control.maximum_allocation_bytes)
    return Failure<DateBytesResultV3>("RESOURCE.BUDGET_EXCEEDED",
                                      "sbdpv_capacity", ResourceStatus());
  if (Cancelled(control))
    return Failure<DateBytesResultV3>("PROCESS.CANCELLED", "before_allocation");
  std::array<byte, 4> component{};
  ScopedSecureClear clear_component(
      component.data(), component.size(),
      DateScrubClassV3::sbdpv_component_staging,
      control.observe_scrubbed, control.scrub_observer_context);
  if (payload_bytes) StoreLittle32(component.data(), static_cast<u32>(value.day));
  auto result = Success<DateBytesResultV3>();
  ScopedVectorSecureClear<byte> clear_result(
      &result.bytes, DateScrubClassV3::sbdpv_reencode,
      control.observe_scrubbed, control.scrub_observer_context);
  try { result.bytes.resize(total); }
  catch (...) {
    return Failure<DateBytesResultV3>("RESOURCE.BUDGET_EXCEEDED",
                                      "sbdpv_allocation", ResourceStatus());
  }
  const DatatypePhysicalValueView structural{
      CanonicalTypeId::date,
      value.state == DateValueStateV3::sql_null
          ? DatatypePhysicalValueState::sql_null
          : DatatypePhysicalValueState::value,
      payload_bytes ? component.data() : nullptr, payload_bytes};
  const auto encoded_result = EncodeDatatypePhysicalStructuralValueIntoNoAlloc(
      structural, result.bytes.data(), result.bytes.size());
  if (!encoded_result.ok() || encoded_result.bytes_written != total)
    return Failure<DateBytesResultV3>(encoded_result.diagnostic.diagnostic_code,
                                      "sbdpv_encode", encoded_result.status);
  const auto recheck = DecodeDateSbdpvComposedNoAllocV3(
      *value.profile, null_allowed, result.bytes, {});
  if (!recheck.ok())
    return Failure<DateBytesResultV3>(recheck.diagnostic.diagnostic_code,
                                      "sbdpv_recheck", recheck.status);
  if (Cancelled(control))
    return Failure<DateBytesResultV3>("PROCESS.CANCELLED", "before_publication");
  clear_result.Disarm();
  return result;
}

DiagnosticRecord MakeDateDiagnosticV3(Status status,
                                      std::string diagnostic_code,
                                      std::string message_key,
                                      std::string detail) {
  return MakeDatatypeOperationDiagnostic(status, std::move(diagnostic_code),
                                         std::move(message_key),
                                         std::move(detail));
}

}  // namespace scratchbird::core::datatypes

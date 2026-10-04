// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0

#include "datatype_time.hpp"

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

constexpr platform::Uuid U(std::array<byte, 16> bytes) noexcept {
  return platform::Uuid{bytes};
}

inline constexpr platform::Uuid kSnapshot = U(
    {0x01,0x9d,0x00,0x00,0x00,0x00,0x70,0x00,0x80,0x00,0x00,0x00,0x00,0x00,0xd7,0x10});
inline constexpr platform::Uuid kDescriptor = U(
    {0x91,0x01,0x00,0x00,0x74,0x69,0x7d,0x65,0x80,0x00,0x00,0x00,0x00,0x00,0x00,0x00});
inline constexpr platform::Uuid kType = U(
    {0x01,0x9d,0x00,0x00,0x00,0x00,0x70,0x00,0x80,0x00,0x00,0x00,0x00,0x00,0xd8,0x1e});
inline constexpr platform::Uuid kCodec = U(
    {0x01,0x9d,0x00,0x00,0x00,0x00,0x70,0x00,0x80,0x00,0x00,0x00,0x00,0x00,0xd8,0x1f});

inline constexpr DatatypePolicyIdentityV3 kOrderingPolicy{U(
    {0x01,0xa1,0x03,0x2b,0x9f,0x51,0x72,0x29,0x97,0x7b,0x45,0x73,0x0f,0x3d,0xff,0x36}),1};
inline constexpr DatatypePolicyIdentityV3 kHashPolicy{U(
    {0x01,0xa1,0x03,0x2b,0x9f,0x51,0x72,0x29,0x97,0x7b,0x45,0x73,0x0f,0x3d,0xff,0x37}),1};
inline constexpr DatatypePolicyIdentityV3 kRenderPolicy{U(
    {0x01,0xa1,0x03,0x2b,0x9f,0x51,0x72,0x29,0x97,0x7b,0x45,0x73,0x0f,0x3d,0xff,0x38}),1};
inline constexpr DatatypePolicyIdentityV3 kCastPolicy{U(
    {0x01,0xa1,0x03,0x2b,0x9f,0x51,0x72,0x29,0x97,0x7b,0x45,0x73,0x0f,0x3d,0xff,0x39}),1};
inline constexpr DatatypePolicyIdentityV3 kCivilDayPolicy{U(
    {0x01,0xa1,0x03,0x2b,0x9f,0x51,0x72,0x29,0x97,0x7b,0x45,0x73,0x0f,0x3d,0xff,0x3b}),1};
inline constexpr DatatypePolicyIdentityV3 kStorageEpochPolicy{U(
    {0x01,0xa1,0x03,0x2b,0x9f,0x51,0x72,0x29,0x97,0x7b,0x45,0x73,0x0f,0x3d,0xff,0x3c}),1};
inline constexpr DatatypePolicyIdentityV3 kTimezoneNonePolicy{U(
    {0x01,0xa1,0x03,0x2b,0x9f,0x51,0x72,0x29,0x97,0x7b,0x45,0x73,0x0f,0x3d,0xff,0x3d}),1};
inline constexpr DatatypePolicyIdentityV3 kLeapSecondPolicy{U(
    {0x01,0xa1,0x03,0x2b,0x9f,0x51,0x72,0x29,0x97,0x7b,0x45,0x73,0x0f,0x3d,0xff,0x3e}),1};
inline constexpr DatatypePolicyIdentityV3 kIndexPolicy{U(
    {0x01,0xa1,0x03,0x2b,0x9f,0x51,0x72,0x29,0x97,0x7b,0x45,0x73,0x0f,0x3d,0xff,0x3f}),1};
inline constexpr DatatypePolicyIdentityV3 kStatisticsPolicy{U(
    {0x01,0xa1,0x03,0x2b,0x9f,0x51,0x72,0x29,0x97,0x7b,0x45,0x73,0x0f,0x3d,0xff,0x40}),1};
inline constexpr DatatypePolicyIdentityV3 kBackupPolicy{U(
    {0x01,0xa1,0x03,0x2b,0x9f,0x51,0x72,0x29,0x97,0x7b,0x45,0x73,0x0f,0x3d,0xff,0x42}),1};
inline constexpr DatatypePolicyIdentityV3 kProtectionPolicy{U(
    {0x01,0xa1,0x03,0x2b,0x9f,0x51,0x72,0x29,0x97,0x7b,0x45,0x73,0x0f,0x3d,0xff,0x43}),1};
inline constexpr DatatypePolicyIdentityV3 kComponentPolicy{U(
    {0x01,0xa1,0x03,0x2b,0x9f,0x51,0x72,0x29,0x97,0x7b,0x45,0x73,0x0f,0x3d,0xff,0x44}),1};
inline constexpr DatatypePolicyIdentityV3 kDiagnosticPolicy{U(
    {0x01,0xa1,0x03,0x2b,0x9f,0x51,0x72,0x29,0x97,0x7b,0x45,0x73,0x0f,0x3d,0xff,0x46}),1};
inline constexpr DatatypePolicyIdentityV3 kMetricPolicy{U(
    {0x01,0xa1,0x03,0x2b,0x9f,0x51,0x72,0x29,0x97,0x7b,0x45,0x73,0x0f,0x3d,0xff,0x47}),1};

inline constexpr std::array<byte, 32> kProfileFingerprint{{
    0x98,0x6d,0xff,0x7c,0x60,0xb8,0xf3,0xeb,0x6f,0x70,0x1c,0xe8,0xc7,0xb7,0xf0,0x99,
    0x7f,0x94,0xa1,0x8d,0x3a,0x0a,0x9b,0xcb,0x73,0x6c,0x11,0xf3,0x5e,0x47,0x6f,0x48}};
inline constexpr std::array<byte, 32> kComparisonFingerprint{{
    0x75,0xde,0xab,0xa7,0x96,0xda,0x7e,0x20,0x08,0x60,0x02,0x37,0xa5,0xeb,0xfb,0x0d,
    0xd8,0xa7,0x6f,0x2b,0x62,0xea,0x66,0x4b,0x66,0x87,0x6b,0xa4,0x7f,0x05,0xb3,0xd6}};

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
      Same(a.operation_policy, b.operation_policy);
}

const DatatypeTypeCodecIdentityRowV3* CurrentIdentityFor(CanonicalTypeId type) noexcept {
  const auto code = static_cast<u32>(type);
  const DatatypeTypeCodecIdentityRowV3* found = nullptr;
  for (const auto& row : CurrentDatatypeTypeCodecIdentityRowsV3()) {
    const auto& legacy = row.legacy_fields;
    if (legacy.catalog_snapshot_uuid != kSnapshot || legacy.catalog_generation != 10 ||
        legacy.registry_generation != 10 || legacy.canonical_binary_type_code != code)
      continue;
    if (found != nullptr) return nullptr;
    found = &row;
  }
  return found;
}

bool ExactTimeIdentity(const DatatypeTypeCodecIdentityRowV3& identity) noexcept {
  const auto* current = CurrentIdentityFor(CanonicalTypeId::time);
  return current != nullptr && EqualIdentityIgnoringName(identity, *current) &&
      identity.legacy_fields.descriptor_uuid == kDescriptor &&
      identity.legacy_fields.type_uuid == kType &&
      identity.legacy_fields.codec_uuid == kCodec &&
      identity.legacy_fields.codec_version == 1 &&
      identity.legacy_fields.codec_generation == 1 &&
      identity.legacy_fields.canonical_value_exact_bytes == 8 &&
      identity.legacy_fields.sql_null_requires_zero_payload;
}

bool ExactReceipt(const TimeAuthorityReceiptV3& receipt) noexcept {
  return receipt.statement_receipt_uuid == kSnapshot &&
      receipt.catalog_snapshot_uuid == kSnapshot &&
      receipt.catalog_generation == 10 && receipt.registry_generation == 10;
}

bool Cancelled(const TimeExecutionControlV3& control) noexcept {
  return control.cancelled != nullptr && control.cancelled(control.cancellation_context);
}

void SecureClear(void* pointer, std::size_t bytes) noexcept {
  auto* output = static_cast<volatile unsigned char*>(pointer);
  while (bytes != 0) { *output++ = 0; --bytes; }
}

class ScopedClear final {
 public:
  ScopedClear(void* pointer, std::size_t bytes,TimeScrubClassV3 scrub_class,
      void (*observer)(void*,TimeScrubClassV3,const byte*,u64) noexcept=nullptr,
      void* observer_context=nullptr) noexcept
      : pointer_(pointer), bytes_(bytes), scrub_class_(scrub_class), observer_(observer), observer_context_(observer_context) {}
  ~ScopedClear() { SecureClear(pointer_, bytes_);if(observer_)observer_(observer_context_,scrub_class_,static_cast<const byte*>(pointer_),bytes_); }
 private:
  void* pointer_;
  std::size_t bytes_;
  TimeScrubClassV3 scrub_class_;
  void (*observer_)(void*,TimeScrubClassV3,const byte*,u64) noexcept;
  void* observer_context_;
};

template <typename T>
class ScopedVectorClear final {
 public:
  explicit ScopedVectorClear(std::vector<T>* value,TimeScrubClassV3 scrub_class,
      void (*observer)(void*,TimeScrubClassV3,const byte*,u64) noexcept=nullptr,
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
  TimeScrubClassV3 scrub_class_;
  void (*observer_)(void*,TimeScrubClassV3,const byte*,u64) noexcept;
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
                           const TimeValidatedProfileHandleV3& profile) noexcept {
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
            const TimeExecutionControlV3* control=nullptr) noexcept {
  if(output==nullptr)return false;
  const auto observer=control==nullptr?nullptr:control->observe_scrubbed;
  void* observer_context=control==nullptr?nullptr:control->scrub_observer_context;
  std::array<u32,8> state{{0x6a09e667u,0xbb67ae85u,0x3c6ef372u,0xa54ff53au,
                           0x510e527fu,0x9b05688cu,0x1f83d9abu,0x5be0cd19u}};
  ScopedClear clear_state(state.data(),state.size()*sizeof(u32),TimeScrubClassV3::sha_state,observer,observer_context);
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
    std::array<u32,64> words{};ScopedClear clear_words(words.data(),words.size()*sizeof(u32),TimeScrubClassV3::sha_schedule,observer,observer_context);
    for(unsigned i=0;i<16;++i)words[i]=(static_cast<u32>(block[i*4])<<24)|
        (static_cast<u32>(block[i*4+1])<<16)|(static_cast<u32>(block[i*4+2])<<8)|block[i*4+3];
    for(unsigned i=16;i<64;++i){const u32 s0=rotate(words[i-15],7)^rotate(words[i-15],18)^(words[i-15]>>3);const u32 s1=rotate(words[i-2],17)^rotate(words[i-2],19)^(words[i-2]>>10);words[i]=words[i-16]+s0+words[i-7]+s1;}
    u32 a=state[0],b=state[1],c=state[2],d=state[3],e=state[4],f=state[5],g=state[6],h=state[7];
    for(unsigned i=0;i<64;++i){const u32 s1=rotate(e,6)^rotate(e,11)^rotate(e,25);const u32 choose=(e&f)^((~e)&g);const u32 first=h+s1+choose+constants[i]+words[i];const u32 s0=rotate(a,2)^rotate(a,13)^rotate(a,22);const u32 majority=(a&b)^(a&c)^(b&c);const u32 second=s0+majority;h=g;g=f;f=e;e=d+first;d=c;c=b;b=a;a=first+second;}
    state[0]+=a;state[1]+=b;state[2]+=c;state[3]+=d;state[4]+=e;state[5]+=f;state[6]+=g;state[7]+=h;
  };
  std::size_t offset=0;while(material.size()-offset>=64){transform(material.data()+offset);offset+=64;}
  std::array<byte,128> tail{};ScopedClear clear_tail(tail.data(),tail.size(),TimeScrubClassV3::sha_tail,observer,observer_context);const std::size_t remainder=material.size()-offset;if(remainder!=0)std::memcpy(tail.data(),material.data()+offset,remainder);tail[remainder]=0x80;const std::size_t padded=remainder<56?64:128;const u64 bits=static_cast<u64>(material.size())*8;
  for(unsigned i=0;i<8;++i)tail[padded-1-i]=static_cast<byte>(bits>>(i*8));
  transform(tail.data());if(padded==128)transform(tail.data()+64);
  for(unsigned i=0;i<8;++i){(*output)[i*4]=static_cast<byte>(state[i]>>24);(*output)[i*4+1]=static_cast<byte>(state[i]>>16);(*output)[i*4+2]=static_cast<byte>(state[i]>>8);(*output)[i*4+3]=static_cast<byte>(state[i]);}
  return true;
}

std::array<byte, kTimeProfileMaterialBytesV3> BuildProfileMaterial(
    const TimeValidatedProfileHandleV3& profile) noexcept {
  std::array<byte, kTimeProfileMaterialBytesV3> result{};
  const auto& row = profile.identity.legacy_fields;
  std::memcpy(result.data(), "SBTIMP01", 8);
  StoreLittle16(result.data() + 8, 1);
  StoreLittle16(result.data() + 10, kTimeProfileMaterialBytesV3);
  StoreLittle32(result.data() + 12, kTimeProfileMaterialBytesV3);
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
      profile.civil_day_policy, profile.storage_epoch_policy,
      profile.timezone_none_policy, profile.leap_second_policy, profile.index_policy,
      profile.statistics_policy, profile.backup_transport_policy,
      profile.protection_policy, profile.component_adapter_policy,
      profile.diagnostic_policy, profile.metric_policy}};
  for (std::size_t index = 0; index < policies.size(); ++index)
    PutPolicy(result.data() + 128 + index * 24, policies[index]);
  StoreLittle64(result.data() + 560, 0);
  StoreLittle64(result.data() + 568, kTimeMaximumNanosecondsV3);
  StoreLittle32(result.data() + 576, 9);
  StoreLittle32(result.data() + 580, 8);
  StoreLittle32(result.data() + 584, 0x0fff);
  StoreLittle32(result.data() + 588, 0);
  return result;
}

std::array<byte, kTimeComparisonMaterialBytesV3> BuildComparisonMaterial(
    const TimeValidatedProfileHandleV3& profile) noexcept {
  std::array<byte, kTimeComparisonMaterialBytesV3> result{};
  const auto& row = profile.identity.legacy_fields;
  std::memcpy(result.data(), "SBTIMC01", 8);
  StoreLittle16(result.data() + 8, 1);
  StoreLittle16(result.data() + 10, kTimeComparisonMaterialBytesV3);
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
      profile.identity.ordering_policy, profile.civil_day_policy,
      profile.storage_epoch_policy, profile.timezone_none_policy,
      profile.leap_second_policy, profile.identity.hash_policy}};
  for (std::size_t index = 0; index < policies.size(); ++index)
    PutPolicy(result.data() + 128 + index * 24, policies[index]);
  StoreLittle32(result.data() + 320, 0x00ff);
  StoreLittle32(result.data() + 324, 9);
  StoreLittle64(result.data() + 328, 0);
  StoreLittle64(result.data() + 336, kTimeMaximumNanosecondsV3);
  StoreLittle32(result.data() + 344, 8);
  StoreLittle32(result.data() + 348, 0);
  return result;
}

bool ProfileValidNoAlloc(const TimeValidatedProfileHandleV3& profile,
                         const TimeExecutionControlV3* control=nullptr) noexcept {
  if (!ExactReceipt(profile.receipt) || !ExactTimeIdentity(profile.identity) ||
      profile.identity.legacy_fields.catalog_snapshot_uuid != profile.receipt.catalog_snapshot_uuid ||
      profile.identity.legacy_fields.catalog_generation != 10 ||
      profile.identity.legacy_fields.registry_generation != 10 ||
      !Same(profile.render_policy, kRenderPolicy) || !Same(profile.cast_policy, kCastPolicy) ||
      !Same(profile.civil_day_policy, kCivilDayPolicy) ||
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
  ScopedClear clear_rebuilt_profile(rebuilt_profile.data(),rebuilt_profile.size(),TimeScrubClassV3::profile_material,observer,observer_context);
  ScopedClear clear_rebuilt_comparison(rebuilt_comparison.data(),rebuilt_comparison.size(),TimeScrubClassV3::comparison_material,observer,observer_context);
  if(profile.profile_material!=rebuilt_profile ||
     profile.comparison_material!=rebuilt_comparison)return false;
  std::array<byte,32> profile_digest{},comparison_digest{};
  ScopedClear clear_profile(profile_digest.data(),profile_digest.size(),TimeScrubClassV3::profile_digest,observer,observer_context);
  ScopedClear clear_comparison(comparison_digest.data(),comparison_digest.size(),TimeScrubClassV3::comparison_digest,observer,observer_context);
  return Digest(profile.profile_material,&profile_digest,control) &&
      Digest(profile.comparison_material,&comparison_digest,control) &&
      profile.profile_fingerprint==profile_digest &&
      profile.comparison_fingerprint==comparison_digest &&
      profile.profile_fingerprint == kProfileFingerprint &&
      profile.comparison_fingerprint == kComparisonFingerprint;
}

bool ValidState(TimeValueStateV3 state) noexcept {
  return state == TimeValueStateV3::value || state == TimeValueStateV3::sql_null;
}

TimeValueResultV3 PropagateOwned(const TimeOwnedValueV3& value,
                                 bool null_allowed,
                                 const TimeExecutionControlV3& control) noexcept {
  const auto checked = ValidateTimeValueViewV3(value.view(), null_allowed);
  if (!checked.ok())
    return Failure<TimeValueResultV3>(checked.diagnostic.diagnostic_code,
                                     checked.diagnostic.detail, checked.status);
  if(Cancelled(control))return Failure<TimeValueResultV3>("PROCESS.CANCELLED","before_publication");
  auto result = Success<TimeValueResultV3>();
  result.value = value;
  return result;
}

struct ParsedTime {
  bool ok = false;
  u64 value = 0;
  std::string_view code;
  std::string_view detail;
};

bool Digit(char value) noexcept { return value >= '0' && value <= '9'; }
unsigned DigitValue(char value) noexcept {
  return static_cast<unsigned>(value - '0');
}

ParsedTime ParseStrict(std::string_view text) noexcept {
  if (text.size() < 8 || text[2] != ':' || text[5] != ':' ||
      !Digit(text[0]) || !Digit(text[1]) || !Digit(text[3]) ||
      !Digit(text[4]) || !Digit(text[6]) || !Digit(text[7]))
    return {false, 0, "CTI.TEMPORAL.INVALID_LITERAL", "shape"};
  const unsigned hour = DigitValue(text[0])*10u + DigitValue(text[1]);
  if (hour > 23) return {false,0,"CTI.TEMPORAL.INVALID_LITERAL","hour"};
  const unsigned minute = DigitValue(text[3])*10u + DigitValue(text[4]);
  if (minute > 59) return {false,0,"CTI.TEMPORAL.INVALID_LITERAL","minute"};
  const unsigned second = DigitValue(text[6])*10u + DigitValue(text[7]);
  if (second == 60)
    return {false,0,"CTI.TEMPORAL.LEAP_SECOND_REFUSED","second"};
  if (second > 59) return {false,0,"CTI.TEMPORAL.INVALID_LITERAL","second"};
  u32 fraction = 0;
  if (text.size() != 8) {
    const auto suffix = text.substr(8);
    if (suffix.size() < 2 || suffix[0] != '.' ||
        !std::all_of(suffix.begin()+1, suffix.end(), Digit))
      return {false,0,"CTI.TEMPORAL.INVALID_LITERAL","fraction_syntax"};
    const auto digits = suffix.size()-1;
    if (digits > 9)
      return {false,0,"CTI.TEMPORAL.PRECISION_LOSS","fraction_precision"};
    if (suffix.back() == '0')
      return {false,0,"CTI.TEMPORAL.INVALID_LITERAL","fraction_trailing_zero"};
    for (std::size_t index=1; index<suffix.size(); ++index)
      fraction = fraction*10u + static_cast<u32>(suffix[index]-'0');
    for (std::size_t index=digits; index<9; ++index) fraction *= 10u;
  }
  const u64 value = static_cast<u64>(hour)*3'600'000'000'000ull +
      static_cast<u64>(minute)*60'000'000'000ull +
      static_cast<u64>(second)*1'000'000'000ull + fraction;
  return {true,value,{},{}};
}

std::size_t RenderStrict(u64 value, char* output) noexcept {
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

bool CharacterIdentity(const DatatypeTypeCodecIdentityRowV3* identity) noexcept {
  const auto* current = CurrentIdentityFor(CanonicalTypeId::character);
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

bool TimeDescriptor(const scratchbird::engine::ExecutionTypeDescriptor* descriptor,
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
      descriptor->canonical_type_id==static_cast<u32>(CanonicalTypeId::time) &&
      descriptor->family==scratchbird::engine::ExecutionTypeFamily::temporal &&
      descriptor->width_class==scratchbird::engine::ExecutionTypeWidthClass::fixed &&
      descriptor->bit_width==64 && descriptor->precision==0 && descriptor->scale==0 &&
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

TimeProfileResultV3 BuildCurrentTimeValidatedProfileHandleV3(
    const platform::Uuid& statement_receipt_uuid) noexcept {
  const auto* identity = CurrentIdentityFor(CanonicalTypeId::time);
  if (identity == nullptr)
    return Failure<TimeProfileResultV3>("CTI.TEMPORAL.DESCRIPTOR_INVALID",
                                        "d710_time_identity_missing");
  return BuildTimeValidatedProfileHandleV3(
      {statement_receipt_uuid,kSnapshot,10,10}, *identity);
}

TimeProfileResultV3 BuildTimeValidatedProfileHandleV3(
    const TimeAuthorityReceiptV3& receipt,
    const DatatypeTypeCodecIdentityRowV3& identity) noexcept {
  if (!ExactReceipt(receipt) || !ExactTimeIdentity(identity) ||
      identity.legacy_fields.catalog_snapshot_uuid != receipt.catalog_snapshot_uuid ||
      identity.legacy_fields.catalog_generation != receipt.catalog_generation ||
      identity.legacy_fields.registry_generation != receipt.registry_generation)
    return Failure<TimeProfileResultV3>("CTI.TEMPORAL.DESCRIPTOR_INVALID",
                                        "receipt_or_identity_invalid");
  try {
    auto result = Success<TimeProfileResultV3>();
    auto& p = result.profile;
    p.receipt=receipt; p.identity=identity;
    p.render_policy=kRenderPolicy; p.cast_policy=kCastPolicy;
    p.civil_day_policy=kCivilDayPolicy; p.storage_epoch_policy=kStorageEpochPolicy;
    p.timezone_none_policy=kTimezoneNonePolicy; p.leap_second_policy=kLeapSecondPolicy;
    p.index_policy=kIndexPolicy; p.statistics_policy=kStatisticsPolicy;
    p.backup_transport_policy=kBackupPolicy; p.protection_policy=kProtectionPolicy;
    p.component_adapter_policy=kComponentPolicy; p.diagnostic_policy=kDiagnosticPolicy;
    p.metric_policy=kMetricPolicy;
    p.profile_material=BuildProfileMaterial(p);
    p.comparison_material=BuildComparisonMaterial(p);
    if (!Digest(p.profile_material,&p.profile_fingerprint) ||
        !Digest(p.comparison_material,&p.comparison_fingerprint))
      return Failure<TimeProfileResultV3>("RESOURCE.BUDGET_EXCEEDED",
                                          "sha256_provider",ResourceStatus());
    if (!ProfileValidNoAlloc(p))
      return Failure<TimeProfileResultV3>("CTI.TEMPORAL.DESCRIPTOR_INVALID",
                                          "profile_material_mismatch");
    return result;
  } catch (...) {
    return Failure<TimeProfileResultV3>("RESOURCE.BUDGET_EXCEEDED",
                                        "profile_allocation",ResourceStatus());
  }
}

TimeValidationResultV3 ValidateTimeProfileHandleV3(
    const TimeValidatedProfileHandleV3& profile,
    const TimeExecutionControlV3& control) noexcept {
  if (!ProfileValidNoAlloc(profile,&control))
    return Failure<TimeValidationResultV3>("CTI.TEMPORAL.DESCRIPTOR_INVALID",
                                           "profile_invalid");
  if(Cancelled(control))return Failure<TimeValidationResultV3>("PROCESS.CANCELLED","before_publication");
  return Success<TimeValidationResultV3>();
}

TimeViewResultV3 ValidateTimeValueViewV3(const TimeValueViewV3& value,
                                        bool null_allowed) noexcept {
  if (value.profile == nullptr || !ProfileValidNoAlloc(*value.profile))
    return Failure<TimeViewResultV3>("CTI.TEMPORAL.DESCRIPTOR_INVALID",
                                     "profile_invalid");
  if (!ValidState(value.state))
    return Failure<TimeViewResultV3>("DATATYPE.NULL_STATE.INVALID","state_unknown");
  if (value.state == TimeValueStateV3::sql_null) {
    if (value.nanoseconds_since_midnight != 0)
      return Failure<TimeViewResultV3>("DATATYPE.NULL_STATE.INVALID","dirty_null_scalar");
    if (!null_allowed)
      return Failure<TimeViewResultV3>("DATATYPE.NULL_NOT_ADMITTED","null_not_allowed");
  } else if (value.nanoseconds_since_midnight > kTimeMaximumNanosecondsV3) {
    return Failure<TimeViewResultV3>("CTI.TEMPORAL.CANONICAL_ENCODING_INVALID",
                                     "value_out_of_range");
  }
  auto result=Success<TimeViewResultV3>(); result.value=value; return result;
}

TimeViewResultV3 DecodeCanonicalTimeComponentNoAllocV3(
    const TimeValidatedProfileHandleV3& profile, TimeValueStateV3 state,
    bool null_allowed, std::span<const byte> component,
    const TimeExecutionControlV3& control) noexcept {
  if (!ProfileValidNoAlloc(profile,&control))
    return Failure<TimeViewResultV3>("CTI.TEMPORAL.DESCRIPTOR_INVALID","profile_invalid");
  if (!ValidState(state))
    return Failure<TimeViewResultV3>("DATATYPE.NULL_STATE.INVALID","state_unknown");
  if (state == TimeValueStateV3::sql_null) {
    if (!component.empty())
      return Failure<TimeViewResultV3>("DATATYPE.NULL_STATE.INVALID","dirty_null_component");
    if (!null_allowed)
      return Failure<TimeViewResultV3>("DATATYPE.NULL_NOT_ADMITTED","null_not_allowed");
    if(Cancelled(control))return Failure<TimeViewResultV3>("PROCESS.CANCELLED","before_publication");
    auto result=Success<TimeViewResultV3>(); result.value={&profile,state,0}; return result;
  }
  if (component.size()!=8)
    return Failure<TimeViewResultV3>("CTI.TEMPORAL.CANONICAL_ENCODING_INVALID","component_extent");
  const u64 value=LoadLittle64(component.data());
  if (value>kTimeMaximumNanosecondsV3)
    return Failure<TimeViewResultV3>("CTI.TEMPORAL.CANONICAL_ENCODING_INVALID","component_range");
  std::array<byte,8> check{};ScopedClear clear_check(check.data(),check.size(),TimeScrubClassV3::component_decode_reencode,control.observe_scrubbed,control.scrub_observer_context); StoreLittle64(check.data(),value);
  if (!std::equal(check.begin(),check.end(),component.begin()))
    return Failure<TimeViewResultV3>("CTI.TEMPORAL.CANONICAL_ENCODING_INVALID","component_reencode");
  if(component.size()>control.maximum_allocation_bytes)
    return Failure<TimeViewResultV3>("RESOURCE.BUDGET_EXCEEDED","component_decode_budget",ResourceStatus());
  if(Cancelled(control))return Failure<TimeViewResultV3>("PROCESS.CANCELLED","before_publication");
  auto result=Success<TimeViewResultV3>(); result.value={&profile,state,value}; return result;
}

TimeNoAllocWriteResultV3 EncodeCanonicalTimeComponentIntoNoAllocV3(
    const TimeOwnedValueV3& owned, byte* output, u64 capacity,
    const TimeExecutionControlV3& control) noexcept {
  const auto checked=ValidateTimeValueViewV3(owned.view(),true);
  if(!checked.ok()) return Failure<TimeNoAllocWriteResultV3>(checked.diagnostic.diagnostic_code,checked.diagnostic.detail,checked.status);
  const u64 required=owned.state==TimeValueStateV3::value?8:0;
  std::array<byte,8> staged{};ScopedClear clear_staged(staged.data(),staged.size(),TimeScrubClassV3::component_encode_staging,control.observe_scrubbed,control.scrub_observer_context);
  if(output!=nullptr && (RangesOverlap(output,required,&owned,sizeof(owned)) ||
      RangesOverlap(output,required,&control,sizeof(control)) ||
      OutputOverlapsProfile(output,required,*owned.profile)))
    return Failure<TimeNoAllocWriteResultV3>("CTI.TEMPORAL.CANONICAL_ENCODING_INVALID","component_overlap");
  if(required>capacity || required>control.maximum_allocation_bytes || (required!=0&&output==nullptr)) {
    auto r=Failure<TimeNoAllocWriteResultV3>("RESOURCE.BUDGET_EXCEEDED","component_capacity",ResourceStatus());
    r.bytes_required=required; return r;
  }
  if(Cancelled(control)) return Failure<TimeNoAllocWriteResultV3>("PROCESS.CANCELLED","before_publication");
  StoreLittle64(staged.data(),owned.nanoseconds_since_midnight);
  if(required) std::memcpy(output,staged.data(),8);
  auto r=Success<TimeNoAllocWriteResultV3>(); r.bytes_required=required;r.bytes_written=required;r.containing_null=required==0;return r;
}

TimeBytesResultV3 EncodeCanonicalTimeComponentV3(
    const TimeOwnedValueV3& value,const TimeExecutionControlV3& control) noexcept {
  const auto checked=ValidateTimeValueViewV3(value.view(),true);
  if(!checked.ok()) return Failure<TimeBytesResultV3>(checked.diagnostic.diagnostic_code,checked.diagnostic.detail,checked.status);
  const u64 required=value.state==TimeValueStateV3::value?8:0;
  if(required>control.maximum_allocation_bytes) return Failure<TimeBytesResultV3>("RESOURCE.BUDGET_EXCEEDED","component_capacity",ResourceStatus());
  if(Cancelled(control)) return Failure<TimeBytesResultV3>("PROCESS.CANCELLED","before_allocation");
  auto r=Success<TimeBytesResultV3>();
  try { r.bytes.resize(required); } catch(...) { return Failure<TimeBytesResultV3>("RESOURCE.BUDGET_EXCEEDED","component_allocation",ResourceStatus()); }
  ScopedVectorClear clear_result(&r.bytes,TimeScrubClassV3::component_owned_buffer,
      control.observe_scrubbed,control.scrub_observer_context);
  if(required) StoreLittle64(r.bytes.data(),value.nanoseconds_since_midnight);
  if(Cancelled(control)) return Failure<TimeBytesResultV3>("PROCESS.CANCELLED","before_publication");
  clear_result.Disarm();
  return r;
}

TimeValueResultV3 ConstructTimeFromCivilV3(
    const std::shared_ptr<const TimeValidatedProfileHandleV3>& profile,
    u64 hour,u64 minute,u64 second,u64 nanosecond,bool null_allowed,
    const TimeExecutionControlV3& control) noexcept {
  return ConstructTimeFromCivilV3(profile,
      {TimeUnsignedCarrierKindV3::unsigned_u64,TimeValueStateV3::value,hour},
      {TimeUnsignedCarrierKindV3::unsigned_u64,TimeValueStateV3::value,minute},
      {TimeUnsignedCarrierKindV3::unsigned_u64,TimeValueStateV3::value,second},
      {TimeUnsignedCarrierKindV3::unsigned_u64,TimeValueStateV3::value,nanosecond},null_allowed,control);
}

TimeValueResultV3 ConstructTimeFromCivilV3(
    const std::shared_ptr<const TimeValidatedProfileHandleV3>& profile,
    const TimeNullableUnsignedFactV3& hour,
    const TimeNullableUnsignedFactV3& minute,
    const TimeNullableUnsignedFactV3& second,
    const TimeNullableUnsignedFactV3& nanosecond,bool null_allowed,
    const TimeExecutionControlV3& control) noexcept {
  if(!profile || !ProfileValidNoAlloc(*profile)) return Failure<TimeValueResultV3>("CTI.TEMPORAL.DESCRIPTOR_INVALID","profile_invalid");
  const std::array<TimeNullableUnsignedFactV3,4> facts{{hour,minute,second,nanosecond}};
  for(const auto& fact:facts) if(fact.carrier!=TimeUnsignedCarrierKindV3::unsigned_u64)
    return Failure<TimeValueResultV3>("SBLR.OPERAND_INVALID","civil_carrier");
  for(const auto& fact:facts) if(!ValidState(fact.state))
    return Failure<TimeValueResultV3>("DATATYPE.NULL_STATE.INVALID","civil_state");
  for(const auto& fact:facts) if(fact.state==TimeValueStateV3::sql_null && fact.value!=0)
    return Failure<TimeValueResultV3>("DATATYPE.NULL_STATE.INVALID","civil_dirty_null");
  if(std::any_of(facts.begin(),facts.end(),[](const auto& x){return x.state==TimeValueStateV3::sql_null;})) {
    if(!null_allowed) return Failure<TimeValueResultV3>("DATATYPE.NULL_NOT_ADMITTED","civil_null");
    if(Cancelled(control))return Failure<TimeValueResultV3>("PROCESS.CANCELLED","before_publication");
    auto r=Success<TimeValueResultV3>();r.value={profile,TimeValueStateV3::sql_null,0};return r;
  }
  if(hour.value>23) return Failure<TimeValueResultV3>("CTI.TEMPORAL.INVALID_LITERAL","hour");
  if(minute.value>59) return Failure<TimeValueResultV3>("CTI.TEMPORAL.INVALID_LITERAL","minute");
  if(second.value==60) return Failure<TimeValueResultV3>("CTI.TEMPORAL.LEAP_SECOND_REFUSED","second");
  if(second.value>59) return Failure<TimeValueResultV3>("CTI.TEMPORAL.INVALID_LITERAL","second");
  if(nanosecond.value>999'999'999) return Failure<TimeValueResultV3>("CTI.TEMPORAL.INVALID_LITERAL","nanosecond");
  const u64 value=hour.value*3'600'000'000'000ull+minute.value*60'000'000'000ull+second.value*1'000'000'000ull+nanosecond.value;
  if(Cancelled(control))return Failure<TimeValueResultV3>("PROCESS.CANCELLED","before_publication");
  auto r=Success<TimeValueResultV3>();r.value={profile,TimeValueStateV3::value,value};return r;
}

TimeCivilResultV3 DecomposeTimeCivilV3(const TimeValueViewV3& value,bool null_allowed,
                                      const TimeExecutionControlV3& control) noexcept {
  const auto checked=ValidateTimeValueViewV3(value,null_allowed);
  if(!checked.ok()) return Failure<TimeCivilResultV3>(checked.diagnostic.diagnostic_code,checked.diagnostic.detail,checked.status);
  auto r=Success<TimeCivilResultV3>();
  if(value.state==TimeValueStateV3::sql_null){if(Cancelled(control))return Failure<TimeCivilResultV3>("PROCESS.CANCELLED","before_publication");r.is_null=true;return r;}
  u64 remainder=value.nanoseconds_since_midnight;
  r.civil.hour=static_cast<u8>(remainder/3'600'000'000'000ull);remainder%=3'600'000'000'000ull;
  r.civil.minute=static_cast<u8>(remainder/60'000'000'000ull);remainder%=60'000'000'000ull;
  r.civil.second=static_cast<u8>(remainder/1'000'000'000ull);
  r.civil.nanosecond=static_cast<u32>(remainder%1'000'000'000ull);
  if(Cancelled(control))
    return Failure<TimeCivilResultV3>("PROCESS.CANCELLED","before_publication");
  return r;
}

TimeValueResultV3 ParseCanonicalTimeV3(
    const std::shared_ptr<const TimeValidatedProfileHandleV3>& profile,
    std::string_view text,bool,const TimeExecutionControlV3& control) noexcept {
  if(!profile || !ProfileValidNoAlloc(*profile)) return Failure<TimeValueResultV3>("CTI.TEMPORAL.DESCRIPTOR_INVALID","profile_invalid");
  const auto parsed=ParseStrict(text);
  if(!parsed.ok) return Failure<TimeValueResultV3>(parsed.code,parsed.detail);
  if(Cancelled(control))return Failure<TimeValueResultV3>("PROCESS.CANCELLED","before_publication");
  auto r=Success<TimeValueResultV3>();r.value={profile,TimeValueStateV3::value,parsed.value};return r;
}

TimeValueResultV3 ParseCanonicalTimeOperandV3(
    const std::shared_ptr<const TimeValidatedProfileHandleV3>& profile,
    const TimeTextOperandV3& operand,bool null_allowed,
    const TimeExecutionControlV3& control) noexcept {
  if(!profile || !ProfileValidNoAlloc(*profile)) return Failure<TimeValueResultV3>("CTI.TEMPORAL.DESCRIPTOR_INVALID","profile_invalid");
  if(!CharacterIdentity(operand.identity) || !CharacterDescriptor(operand.descriptor,operand.identity))
    return Failure<TimeValueResultV3>("CTI.TEMPORAL.DESCRIPTOR_INVALID","text_profile");
  if(operand.carrier!=TimeTextCarrierKindV3::utf8_bytes)
    return Failure<TimeValueResultV3>(operand.state==TimeValueStateV3::sql_null?"DATATYPE.NULL_STATE.INVALID":"SBLR.OPERAND_INVALID","text_carrier");
  if(!ValidState(operand.state)) return Failure<TimeValueResultV3>("DATATYPE.NULL_STATE.INVALID","text_state");
  if(operand.extent!=operand.bytes.size()) return Failure<TimeValueResultV3>(operand.state==TimeValueStateV3::sql_null?"DATATYPE.NULL_STATE.INVALID":"SBLR.OPERAND_INVALID","text_extent");
  if(operand.state==TimeValueStateV3::sql_null){
    if(!operand.bytes.empty() || operand.extent!=0) return Failure<TimeValueResultV3>("DATATYPE.NULL_STATE.INVALID","dirty_null_text");
    if(!operand.descriptor->nullable_allowed||!null_allowed) return Failure<TimeValueResultV3>("DATATYPE.NULL_NOT_ADMITTED","text_null");
    if(Cancelled(control))return Failure<TimeValueResultV3>("PROCESS.CANCELLED","before_publication");
    auto r=Success<TimeValueResultV3>();r.value={profile,TimeValueStateV3::sql_null,0};return r;
  }
  return ParseCanonicalTimeV3(profile,operand.bytes,null_allowed,control);
}

TimeNoAllocWriteResultV3 RenderCanonicalTimeIntoNoAllocV3(
    const TimeOwnedValueV3& value,bool export_literal,char* output,u64 capacity,
    const TimeExecutionControlV3& control) noexcept {
  const auto checked=ValidateTimeValueViewV3(value.view(),true);
  if(!checked.ok()) return Failure<TimeNoAllocWriteResultV3>(checked.diagnostic.diagnostic_code,checked.diagnostic.detail,checked.status);
  std::array<char,25> staged{};ScopedClear clear_staged(staged.data(),staged.size(),TimeScrubClassV3::render_staging,control.observe_scrubbed,control.scrub_observer_context);
  std::size_t plain=0;
  if(value.state==TimeValueStateV3::value) plain=RenderStrict(value.nanoseconds_since_midnight,staged.data()+(export_literal?6:0));
  std::size_t required=plain;
  if(export_literal && value.state==TimeValueStateV3::value){std::memcpy(staged.data(),"TIME '",6);staged[6+plain]='\'';required=7+plain;}
  if(output!=nullptr && (RangesOverlap(output,required,&value,sizeof(value)) ||
      RangesOverlap(output,required,&control,sizeof(control)) ||
      OutputOverlapsProfile(output,required,*value.profile)))
    return Failure<TimeNoAllocWriteResultV3>("CTI.TEMPORAL.CANONICAL_ENCODING_INVALID","render_overlap");
  if(required>capacity || required>control.maximum_allocation_bytes || (required&&output==nullptr)){
    auto r=Failure<TimeNoAllocWriteResultV3>("RESOURCE.BUDGET_EXCEEDED","render_capacity",ResourceStatus());r.bytes_required=required;return r;}
  if(Cancelled(control)) return Failure<TimeNoAllocWriteResultV3>("PROCESS.CANCELLED","before_publication");
  if(required)std::memcpy(output,staged.data(),required);
  auto r=Success<TimeNoAllocWriteResultV3>();r.bytes_required=required;r.bytes_written=required;r.containing_null=value.state==TimeValueStateV3::sql_null;return r;
}

TimeTextResultV3 RenderCanonicalTimeV3(const TimeOwnedValueV3& value,bool export_literal,
                                      const TimeExecutionControlV3& control) noexcept {
  const auto checked=ValidateTimeValueViewV3(value.view(),true);
  if(!checked.ok())return Failure<TimeTextResultV3>(checked.diagnostic.diagnostic_code,checked.diagnostic.detail,checked.status);
  if(Cancelled(control))return Failure<TimeTextResultV3>("PROCESS.CANCELLED","before_allocation");
  std::array<char,25> buffer{};ScopedClear clear_buffer(buffer.data(),buffer.size(),TimeScrubClassV3::render_owned_buffer,control.observe_scrubbed,control.scrub_observer_context);const auto written=RenderCanonicalTimeIntoNoAllocV3(value,export_literal,buffer.data(),buffer.size(),control);
  if(!written.ok())return Failure<TimeTextResultV3>(written.diagnostic.diagnostic_code,written.diagnostic.detail,written.status);
  auto r=Success<TimeTextResultV3>();r.containing_null=written.containing_null;
  try{r.text.assign(buffer.data(),static_cast<std::size_t>(written.bytes_written));}catch(...){return Failure<TimeTextResultV3>("RESOURCE.BUDGET_EXCEEDED","render_allocation",ResourceStatus());}
  if(Cancelled(control)){if(!r.text.empty())SecureClear(r.text.data(),r.text.size());return Failure<TimeTextResultV3>("PROCESS.CANCELLED","before_publication");}
  return r;
}

TimeValueResultV3 ValidateCanonicalTimeV3(const TimeOwnedValueV3& value,bool null_allowed,const TimeExecutionControlV3& control) noexcept {return PropagateOwned(value,null_allowed,control);}
TimeValueResultV3 TruncateTimeNanosecondV3(const TimeOwnedValueV3& value,bool null_allowed,const TimeExecutionControlV3& control) noexcept {return PropagateOwned(value,null_allowed,control);}
TimeValueResultV3 RoundTimeNanosecondV3(const TimeOwnedValueV3& value,bool null_allowed,const TimeExecutionControlV3& control) noexcept {return PropagateOwned(value,null_allowed,control);}

TimeIntrinsicDispositionV3 ClassifyTimeIntrinsicOperationV3(
    TimeIntrinsicOperationV3 operation) noexcept {
  switch(operation){
    case TimeIntrinsicOperationV3::validate_canonicalize:
    case TimeIntrinsicOperationV3::civil_construct:
    case TimeIntrinsicOperationV3::decompose_civil:
    case TimeIntrinsicOperationV3::parse_canonical:
    case TimeIntrinsicOperationV3::render_canonical:
    case TimeIntrinsicOperationV3::add_nanoseconds:
    case TimeIntrinsicOperationV3::subtract_nanoseconds:
    case TimeIntrinsicOperationV3::successor:
    case TimeIntrinsicOperationV3::predecessor:
    case TimeIntrinsicOperationV3::difference_nanoseconds:
    case TimeIntrinsicOperationV3::extract_hour:
    case TimeIntrinsicOperationV3::extract_minute:
    case TimeIntrinsicOperationV3::extract_second:
    case TimeIntrinsicOperationV3::extract_nanosecond:
    case TimeIntrinsicOperationV3::nanosecond_of_day:
    case TimeIntrinsicOperationV3::truncate_nanosecond:
    case TimeIntrinsicOperationV3::round_nanosecond:
      return TimeIntrinsicDispositionV3::admitted;
    case TimeIntrinsicOperationV3::larger_truncate_round_or_bucket:
    case TimeIntrinsicOperationV3::duration_interval_or_modulo_day:
    case TimeIntrinsicOperationV3::date_timestamp_or_timezone_cross_temporal:
      return TimeIntrinsicDispositionV3::registered_refused;
    case TimeIntrinsicOperationV3::aggregate_min_max_count_dispatch:
      return TimeIntrinsicDispositionV3::receiving_owner;
  }
  return TimeIntrinsicDispositionV3::unknown;
}

TimeValueResultV3 RefuseTimeIntrinsicOperationV3(
    const TimeValueViewV3& operand,TimeIntrinsicOperationV3 operation) noexcept {
  if(operand.profile==nullptr || !ProfileValidNoAlloc(*operand.profile))
    return Failure<TimeValueResultV3>("CTI.TEMPORAL.DESCRIPTOR_INVALID","operation_profile");
  if(!ValidState(operand.state) ||
     (operand.state==TimeValueStateV3::sql_null && operand.nanoseconds_since_midnight!=0))
    return Failure<TimeValueResultV3>("DATATYPE.NULL_STATE.INVALID","operation_state");
  const auto disposition=ClassifyTimeIntrinsicOperationV3(operation);
  if(disposition==TimeIntrinsicDispositionV3::registered_refused)
    return Failure<TimeValueResultV3>("CTI.INTERVAL.CALENDAR_OPERATION_REFUSED","registered_time_refusal");
  if(disposition==TimeIntrinsicDispositionV3::receiving_owner)
    return Failure<TimeValueResultV3>("CTI.TEMPORAL.OPERATION_REFUSED","aggregate_owner_handoff");
  return Failure<TimeValueResultV3>("CTI.TEMPORAL.OPERATION_REFUSED","operation_unknown_or_not_refusal_api");
}

namespace {
TimeValueResultV3 ArithmeticResult(const TimeOwnedValueV3& value,u64 result,
                                   const TimeExecutionControlV3& control) noexcept {
  if(Cancelled(control))return Failure<TimeValueResultV3>("PROCESS.CANCELLED","before_publication");
  auto r=Success<TimeValueResultV3>();r.value={value.profile,TimeValueStateV3::value,result};return r;
}

TimeValueResultV3 ArithmeticNull(const TimeOwnedValueV3& value,bool null_allowed,
                                 const TimeExecutionControlV3& control) noexcept {
  if(!null_allowed)return Failure<TimeValueResultV3>("DATATYPE.NULL_NOT_ADMITTED","arithmetic_null");
  if(Cancelled(control))return Failure<TimeValueResultV3>("PROCESS.CANCELLED","before_publication");
  auto r=Success<TimeValueResultV3>();r.value={value.profile,TimeValueStateV3::sql_null,0};return r;
}

TimeValidationResultV3 ValidateTimeAuthorityEnvelope(const TimeValueViewV3& value) noexcept {
  if(value.profile==nullptr || !ProfileValidNoAlloc(*value.profile))
    return Failure<TimeValidationResultV3>("CTI.TEMPORAL.DESCRIPTOR_INVALID","profile_invalid");
  return Success<TimeValidationResultV3>();
}

TimeValidationResultV3 ValidateTimeStateEnvelope(const TimeValueViewV3& value) noexcept {
  if(!ValidState(value.state) ||
     (value.state==TimeValueStateV3::sql_null && value.nanoseconds_since_midnight!=0))
    return Failure<TimeValidationResultV3>("DATATYPE.NULL_STATE.INVALID","state_or_dirty_null");
  return Success<TimeValidationResultV3>();
}

TimeValidationResultV3 ValidateTimePresentRange(const TimeValueViewV3& value) noexcept {
  if(value.state==TimeValueStateV3::value &&
     value.nanoseconds_since_midnight>kTimeMaximumNanosecondsV3)
    return Failure<TimeValidationResultV3>("CTI.TEMPORAL.CANONICAL_ENCODING_INVALID","value_out_of_range");
  return Success<TimeValidationResultV3>();
}

TimeScalarResultV3 ExtractPart(const TimeValueViewV3& value,u64 divisor,u64 modulo,
                               bool null_allowed,const TimeExecutionControlV3& control) noexcept {
  const auto checked=ValidateTimeValueViewV3(value,null_allowed);
  if(!checked.ok())return Failure<TimeScalarResultV3>(checked.diagnostic.diagnostic_code,checked.diagnostic.detail,checked.status);
  auto r=Success<TimeScalarResultV3>();
  if(value.state==TimeValueStateV3::sql_null){if(Cancelled(control))return Failure<TimeScalarResultV3>("PROCESS.CANCELLED","before_publication");r.is_null=true;return r;}
  r.unsigned_value=(value.nanoseconds_since_midnight/divisor)%modulo;
  r.signed_value=static_cast<std::int64_t>(r.unsigned_value);
  if(Cancelled(control))
    return Failure<TimeScalarResultV3>("PROCESS.CANCELLED","before_publication");
  return r;
}
}  // namespace

TimeValueResultV3 AddTimeNanosecondsV3(const TimeOwnedValueV3& value,std::int64_t delta,
    bool null_allowed,const TimeExecutionControlV3& control) noexcept {
  return AddTimeNanosecondsV3(value,{TimeI64CarrierKindV3::signed_i64,TimeValueStateV3::value,delta},null_allowed,control);
}

TimeValueResultV3 AddTimeNanosecondsV3(const TimeOwnedValueV3& value,
    const TimeNullableI64FactV3& delta,bool null_allowed,
    const TimeExecutionControlV3& control) noexcept {
  const auto authority=ValidateTimeAuthorityEnvelope(value.view());
  if(!authority.ok())return Failure<TimeValueResultV3>(authority.diagnostic.diagnostic_code,authority.diagnostic.detail,authority.status);
  const auto envelope=ValidateTimeStateEnvelope(value.view());
  if(!envelope.ok())return Failure<TimeValueResultV3>(envelope.diagnostic.diagnostic_code,envelope.diagnostic.detail,envelope.status);
  if(delta.carrier!=TimeI64CarrierKindV3::signed_i64)return Failure<TimeValueResultV3>("SBLR.OPERAND_INVALID","delta_carrier");
  if(!ValidState(delta.state) || (delta.state==TimeValueStateV3::sql_null&&delta.value!=0))return Failure<TimeValueResultV3>("DATATYPE.NULL_STATE.INVALID","delta_state");
  if(value.state==TimeValueStateV3::sql_null || delta.state==TimeValueStateV3::sql_null)return ArithmeticNull(value,null_allowed,control);
  const auto checked=ValidateTimeValueViewV3(value.view(),true);
  if(!checked.ok())return Failure<TimeValueResultV3>(checked.diagnostic.diagnostic_code,checked.diagnostic.detail,checked.status);
  const u64 current=value.nanoseconds_since_midnight;
  if(delta.value>=0){const u64 amount=static_cast<u64>(delta.value);if(amount>kTimeMaximumNanosecondsV3-current)return Failure<TimeValueResultV3>("CTI.TEMPORAL.RANGE_EXCEEDED","add_overflow");return ArithmeticResult(value,current+amount,control);}
  const u64 amount=static_cast<u64>(-(delta.value+1))+1;
  if(amount>current)return Failure<TimeValueResultV3>("CTI.TEMPORAL.RANGE_EXCEEDED","add_underflow");
  return ArithmeticResult(value,current-amount,control);
}

TimeValueResultV3 SubtractTimeNanosecondsV3(const TimeOwnedValueV3& value,std::int64_t delta,
    bool null_allowed,const TimeExecutionControlV3& control) noexcept {
  return SubtractTimeNanosecondsV3(value,{TimeI64CarrierKindV3::signed_i64,TimeValueStateV3::value,delta},null_allowed,control);
}

TimeValueResultV3 SubtractTimeNanosecondsV3(const TimeOwnedValueV3& value,
    const TimeNullableI64FactV3& delta,bool null_allowed,
    const TimeExecutionControlV3& control) noexcept {
  const auto authority=ValidateTimeAuthorityEnvelope(value.view());
  if(!authority.ok())return Failure<TimeValueResultV3>(authority.diagnostic.diagnostic_code,authority.diagnostic.detail,authority.status);
  const auto envelope=ValidateTimeStateEnvelope(value.view());
  if(!envelope.ok())return Failure<TimeValueResultV3>(envelope.diagnostic.diagnostic_code,envelope.diagnostic.detail,envelope.status);
  if(delta.carrier!=TimeI64CarrierKindV3::signed_i64)return Failure<TimeValueResultV3>("SBLR.OPERAND_INVALID","delta_carrier");
  if(!ValidState(delta.state) || (delta.state==TimeValueStateV3::sql_null&&delta.value!=0))return Failure<TimeValueResultV3>("DATATYPE.NULL_STATE.INVALID","delta_state");
  if(value.state==TimeValueStateV3::sql_null || delta.state==TimeValueStateV3::sql_null)return ArithmeticNull(value,null_allowed,control);
  const auto checked=ValidateTimeValueViewV3(value.view(),true);
  if(!checked.ok())return Failure<TimeValueResultV3>(checked.diagnostic.diagnostic_code,checked.diagnostic.detail,checked.status);
  const u64 current=value.nanoseconds_since_midnight;
  if(delta.value>=0){const u64 amount=static_cast<u64>(delta.value);if(amount>current)return Failure<TimeValueResultV3>("CTI.TEMPORAL.RANGE_EXCEEDED","subtract_underflow");return ArithmeticResult(value,current-amount,control);}
  const u64 amount=static_cast<u64>(-(delta.value+1))+1;
  if(amount>kTimeMaximumNanosecondsV3-current)return Failure<TimeValueResultV3>("CTI.TEMPORAL.RANGE_EXCEEDED","subtract_overflow");
  return ArithmeticResult(value,current+amount,control);
}

TimeValueResultV3 TimeSuccessorV3(const TimeOwnedValueV3& value,bool null_allowed,
                                  const TimeExecutionControlV3& control) noexcept {
  return AddTimeNanosecondsV3(value,1,null_allowed,control);
}
TimeValueResultV3 TimePredecessorV3(const TimeOwnedValueV3& value,bool null_allowed,
                                    const TimeExecutionControlV3& control) noexcept {
  return SubtractTimeNanosecondsV3(value,1,null_allowed,control);
}

TimeScalarResultV3 DifferenceTimeNanosecondsV3(const TimeValueViewV3& left,
    const TimeValueViewV3& right,bool null_allowed,
    const TimeExecutionControlV3& control) noexcept {
  const auto la=ValidateTimeAuthorityEnvelope(left);if(!la.ok())return Failure<TimeScalarResultV3>(la.diagnostic.diagnostic_code,la.diagnostic.detail,la.status);
  const auto ra=ValidateTimeAuthorityEnvelope(right);if(!ra.ok())return Failure<TimeScalarResultV3>(ra.diagnostic.diagnostic_code,ra.diagnostic.detail,ra.status);
  const auto le=ValidateTimeStateEnvelope(left);if(!le.ok())return Failure<TimeScalarResultV3>(le.diagnostic.diagnostic_code,le.diagnostic.detail,le.status);
  const auto re=ValidateTimeStateEnvelope(right);if(!re.ok())return Failure<TimeScalarResultV3>(re.diagnostic.diagnostic_code,re.diagnostic.detail,re.status);
  if(left.profile->comparison_fingerprint!=right.profile->comparison_fingerprint)return Failure<TimeScalarResultV3>("CTI.TEMPORAL.ORDERING_REFUSED","comparison_cohort");
  if(left.state==TimeValueStateV3::sql_null || right.state==TimeValueStateV3::sql_null){if(!null_allowed)return Failure<TimeScalarResultV3>("DATATYPE.NULL_NOT_ADMITTED","difference_null");if(Cancelled(control))return Failure<TimeScalarResultV3>("PROCESS.CANCELLED","before_publication");auto out=Success<TimeScalarResultV3>();out.is_null=true;return out;}
  const auto lr=ValidateTimePresentRange(left);if(!lr.ok())return Failure<TimeScalarResultV3>(lr.diagnostic.diagnostic_code,lr.diagnostic.detail,lr.status);
  const auto rr=ValidateTimePresentRange(right);if(!rr.ok())return Failure<TimeScalarResultV3>(rr.diagnostic.diagnostic_code,rr.diagnostic.detail,rr.status);
  auto out=Success<TimeScalarResultV3>();out.signed_value=left.nanoseconds_since_midnight>=right.nanoseconds_since_midnight?static_cast<std::int64_t>(left.nanoseconds_since_midnight-right.nanoseconds_since_midnight):-static_cast<std::int64_t>(right.nanoseconds_since_midnight-left.nanoseconds_since_midnight);if(Cancelled(control))return Failure<TimeScalarResultV3>("PROCESS.CANCELLED","before_publication");return out;
}

TimeScalarResultV3 ExtractTimeHourV3(const TimeValueViewV3& value,bool null_allowed,const TimeExecutionControlV3& control) noexcept{return ExtractPart(value,3'600'000'000'000ull,24,null_allowed,control);}
TimeScalarResultV3 ExtractTimeMinuteV3(const TimeValueViewV3& value,bool null_allowed,const TimeExecutionControlV3& control) noexcept{return ExtractPart(value,60'000'000'000ull,60,null_allowed,control);}
TimeScalarResultV3 ExtractTimeSecondV3(const TimeValueViewV3& value,bool null_allowed,const TimeExecutionControlV3& control) noexcept{return ExtractPart(value,1'000'000'000ull,60,null_allowed,control);}
TimeScalarResultV3 ExtractTimeNanosecondV3(const TimeValueViewV3& value,bool null_allowed,const TimeExecutionControlV3& control) noexcept{return ExtractPart(value,1,1'000'000'000ull,null_allowed,control);}
TimeScalarResultV3 TimeNanosecondOfDayV3(const TimeValueViewV3& value,bool null_allowed,const TimeExecutionControlV3& control) noexcept{return ExtractPart(value,1,kTimeMaximumNanosecondsV3+1,null_allowed,control);}

TimeAggregateHandoffResultV3 ValidateTimeAggregateHandoffV3(
    const TimeOwnedValueV3& operand,bool null_allowed,
    const TimeExecutionControlV3& control) noexcept {
  const auto checked=ValidateTimeValueViewV3(operand.view(),null_allowed);
  if(!checked.ok())return Failure<TimeAggregateHandoffResultV3>(checked.diagnostic.diagnostic_code,checked.diagnostic.detail,checked.status);
  if(Cancelled(control))return Failure<TimeAggregateHandoffResultV3>("PROCESS.CANCELLED","before_publication");
  auto out=Success<TimeAggregateHandoffResultV3>();out.operand=operand;return out;
}

namespace {
TimeComparisonResultV3 CompareResolved(const TimeValueViewV3& left,
    const TimeValueViewV3& right,const std::array<byte,32>& right_fingerprint,
    const TimeExecutionControlV3& control) noexcept {
  const auto la=ValidateTimeAuthorityEnvelope(left);if(!la.ok())return Failure<TimeComparisonResultV3>(la.diagnostic.diagnostic_code,la.diagnostic.detail,la.status);
  const auto ra=ValidateTimeAuthorityEnvelope(right);if(!ra.ok())return Failure<TimeComparisonResultV3>(ra.diagnostic.diagnostic_code,ra.diagnostic.detail,ra.status);
  const auto le=ValidateTimeStateEnvelope(left);if(!le.ok())return Failure<TimeComparisonResultV3>(le.diagnostic.diagnostic_code,le.diagnostic.detail,le.status);
  const auto re=ValidateTimeStateEnvelope(right);if(!re.ok())return Failure<TimeComparisonResultV3>(re.diagnostic.diagnostic_code,re.diagnostic.detail,re.status);
  if(left.profile->comparison_fingerprint!=right_fingerprint)return Failure<TimeComparisonResultV3>("CTI.TEMPORAL.ORDERING_REFUSED","comparison_cohort");
  auto out=Success<TimeComparisonResultV3>();
  if(left.state==TimeValueStateV3::sql_null || right.state==TimeValueStateV3::sql_null){out.fact=TimeComparisonFactV3::unordered_null;out.grouping_equivalent=left.state==right.state;out.null_equivalent=left.state==right.state;if(Cancelled(control))return Failure<TimeComparisonResultV3>("PROCESS.CANCELLED","before_publication");return out;}
  const auto l=ValidateTimePresentRange(left);if(!l.ok())return Failure<TimeComparisonResultV3>(l.diagnostic.diagnostic_code,l.diagnostic.detail,l.status);
  const auto r=ValidateTimePresentRange(right);if(!r.ok())return Failure<TimeComparisonResultV3>(r.diagnostic.diagnostic_code,r.diagnostic.detail,r.status);
  out.fact=left.nanoseconds_since_midnight<right.nanoseconds_since_midnight?TimeComparisonFactV3::less:left.nanoseconds_since_midnight>right.nanoseconds_since_midnight?TimeComparisonFactV3::greater:TimeComparisonFactV3::equal;
  out.grouping_equivalent=out.fact==TimeComparisonFactV3::equal;if(Cancelled(control))return Failure<TimeComparisonResultV3>("PROCESS.CANCELLED","before_publication");return out;
}
}  // namespace

TimeComparisonResultV3 CompareTimeValuesV3(const TimeValueViewV3& left,
                                           const TimeValueViewV3& right,
                                           const TimeExecutionControlV3& control) noexcept {
  std::array<byte,32> empty{};return CompareResolved(left,right,right.profile?right.profile->comparison_fingerprint:empty,control);
}
TimeComparisonResultV3 CompareTimeValuesWithValidatedCohortForConformanceV3(
    const TimeValueViewV3& left,const TimeValueViewV3& right,
    const std::array<byte,32>& fingerprint,
    const TimeExecutionControlV3& control) noexcept{return CompareResolved(left,right,fingerprint,control);}

TimeNoAllocWriteResultV3 HashTimeValueIntoNoAllocV3(const TimeOwnedValueV3& owned,
    byte* output,u64 capacity,const TimeExecutionControlV3& control) noexcept {
  const auto checked=ValidateTimeValueViewV3(owned.view(),true);if(!checked.ok())return Failure<TimeNoAllocWriteResultV3>(checked.diagnostic.diagnostic_code,checked.diagnostic.detail,checked.status);
  std::array<byte,109> preimage{};ScopedClear clear(preimage.data(),preimage.size(),TimeScrubClassV3::hash_preimage,control.observe_scrubbed,control.scrub_observer_context);
  std::array<byte,32> digest{};ScopedClear clear_digest(digest.data(),digest.size(),TimeScrubClassV3::hash_digest,control.observe_scrubbed,control.scrub_observer_context);
  if(output!=nullptr && (RangesOverlap(output,32,&owned,sizeof(owned))||RangesOverlap(output,32,&control,sizeof(control))||OutputOverlapsProfile(output,32,*owned.profile)))return Failure<TimeNoAllocWriteResultV3>("CTI.TEMPORAL.CANONICAL_ENCODING_INVALID","hash_overlap");
  if(capacity<32 || control.maximum_allocation_bytes<32 || output==nullptr){auto r=Failure<TimeNoAllocWriteResultV3>("RESOURCE.BUDGET_EXCEEDED","hash_capacity",ResourceStatus());r.bytes_required=32;return r;}
  std::memcpy(preimage.data(),"SBTIMH01",8);PutUuid(preimage.data()+8,kSnapshot);StoreLittle64(preimage.data()+24,10);StoreLittle64(preimage.data()+32,10);
  std::memcpy(preimage.data()+40,owned.profile->comparison_fingerprint.data(),32);PutPolicy(preimage.data()+72,kHashPolicy);
  preimage[96]=owned.state==TimeValueStateV3::sql_null?0:1;StoreLittle32(preimage.data()+97,owned.state==TimeValueStateV3::sql_null?0:8);
  std::size_t extent=101;if(owned.state==TimeValueStateV3::value){StoreLittle64(preimage.data()+101,owned.nanoseconds_since_midnight);extent=109;}
  if(!Digest(std::span<const byte>(preimage.data(),extent),&digest,&control))return Failure<TimeNoAllocWriteResultV3>("RESOURCE.BUDGET_EXCEEDED","hash_provider",ResourceStatus());
  if(Cancelled(control))
    return Failure<TimeNoAllocWriteResultV3>("PROCESS.CANCELLED","before_publication");
  std::memcpy(output,digest.data(),32);
  auto r=Success<TimeNoAllocWriteResultV3>();r.bytes_required=32;r.bytes_written=32;return r;
}

TimeBytesResultV3 HashTimeValueV3(const TimeOwnedValueV3& value) noexcept{return HashTimeValueV3(value,{});}
TimeBytesResultV3 HashTimeValueV3(const TimeOwnedValueV3& value,const TimeExecutionControlV3& control) noexcept {
  if(control.maximum_allocation_bytes<32)return Failure<TimeBytesResultV3>("RESOURCE.BUDGET_EXCEEDED","hash_capacity",ResourceStatus());
  std::array<byte,32> bytes{};ScopedClear clear_bytes(bytes.data(),bytes.size(),TimeScrubClassV3::hash_owned_buffer,control.observe_scrubbed,control.scrub_observer_context);const auto written=HashTimeValueIntoNoAllocV3(value,bytes.data(),bytes.size(),control);if(!written.ok())return Failure<TimeBytesResultV3>(written.diagnostic.diagnostic_code,written.diagnostic.detail,written.status);
  auto r=Success<TimeBytesResultV3>();try{r.bytes.assign(bytes.begin(),bytes.end());}catch(...){return Failure<TimeBytesResultV3>("RESOURCE.BUDGET_EXCEEDED","hash_allocation",ResourceStatus());}if(Cancelled(control)){SecureClear(r.bytes.data(),r.bytes.size());return Failure<TimeBytesResultV3>("PROCESS.CANCELLED","before_publication");}return r;
}

TimeNoAllocWriteResultV3 MakeTimeSortKeyIntoNoAllocV3(
    const TimeOwnedValueV3& value,TimeSortDirectionV3 direction,
    TimeNullModeV3 null_mode,byte* output,u64 capacity,
    const TimeExecutionControlV3& control) noexcept {
  const auto authority=ValidateTimeAuthorityEnvelope(value.view());
  if(!authority.ok())return Failure<TimeNoAllocWriteResultV3>(authority.diagnostic.diagnostic_code,authority.diagnostic.detail,authority.status);
  if(direction!=TimeSortDirectionV3::ascending&&direction!=TimeSortDirectionV3::descending)
    return Failure<TimeNoAllocWriteResultV3>("CTI.TEMPORAL.INDEX_KEY_REFUSED","direction");
  if(null_mode!=TimeNullModeV3::nulls_first&&null_mode!=TimeNullModeV3::nulls_last)
    return Failure<TimeNoAllocWriteResultV3>("CTI.TEMPORAL.INDEX_KEY_REFUSED","null_mode");
  const auto state=ValidateTimeStateEnvelope(value.view());if(!state.ok())return Failure<TimeNoAllocWriteResultV3>(state.diagnostic.diagnostic_code,state.diagnostic.detail,state.status);
  const auto range=ValidateTimePresentRange(value.view());if(!range.ok())return Failure<TimeNoAllocWriteResultV3>(range.diagnostic.diagnostic_code,range.diagnostic.detail,range.status);
  const u64 extent=value.state==TimeValueStateV3::sql_null?100:108;
  std::array<byte,108> staged{};ScopedClear clear_staged(staged.data(),staged.size(),TimeScrubClassV3::ordered_key_staging,control.observe_scrubbed,control.scrub_observer_context);
  if(output!=nullptr&&(RangesOverlap(output,extent,&value,sizeof(value))||RangesOverlap(output,extent,&control,sizeof(control))||OutputOverlapsProfile(output,extent,*value.profile)))return Failure<TimeNoAllocWriteResultV3>("CTI.TEMPORAL.CANONICAL_ENCODING_INVALID","key_overlap");
  if(capacity<extent||control.maximum_allocation_bytes<extent||output==nullptr){auto r=Failure<TimeNoAllocWriteResultV3>("RESOURCE.BUDGET_EXCEEDED","key_capacity",ResourceStatus());r.bytes_required=extent;return r;}
  std::memcpy(staged.data(),"SBTIMK01",8);PutUuid(staged.data()+8,kSnapshot);StoreLittle64(staged.data()+24,10);StoreLittle64(staged.data()+32,10);
  std::memcpy(staged.data()+40,value.profile->comparison_fingerprint.data(),32);PutPolicy(staged.data()+72,kOrderingPolicy);
  staged[96]=direction==TimeSortDirectionV3::descending?1:0;staged[97]=null_mode==TimeNullModeV3::nulls_last?1:0;
  staged[98]=value.state==TimeValueStateV3::value?1:(null_mode==TimeNullModeV3::nulls_first?0:2);staged[99]=value.state==TimeValueStateV3::value?8:0;
  if(value.state==TimeValueStateV3::value){for(unsigned i=0;i<8;++i)staged[100+i]=static_cast<byte>(value.nanoseconds_since_midnight>>(56-8*i));if(direction==TimeSortDirectionV3::descending)for(unsigned i=100;i<108;++i)staged[i]=static_cast<byte>(~staged[i]);}
  if(Cancelled(control))
    return Failure<TimeNoAllocWriteResultV3>("PROCESS.CANCELLED","before_publication");
  std::memcpy(output,staged.data(),extent);
  auto r=Success<TimeNoAllocWriteResultV3>();r.bytes_required=extent;r.bytes_written=extent;r.containing_null=value.state==TimeValueStateV3::sql_null;return r;
}

TimeBytesResultV3 MakeTimeSortKeyV3(const TimeOwnedValueV3& value,
    TimeSortDirectionV3 direction,TimeNullModeV3 null_mode,
    const TimeExecutionControlV3& control) noexcept {
  std::array<byte,108> bytes{};ScopedClear clear_bytes(bytes.data(),bytes.size(),TimeScrubClassV3::ordered_key_owned_buffer,control.observe_scrubbed,control.scrub_observer_context);const auto written=MakeTimeSortKeyIntoNoAllocV3(value,direction,null_mode,bytes.data(),bytes.size(),control);
  if(!written.ok())return Failure<TimeBytesResultV3>(written.diagnostic.diagnostic_code,written.diagnostic.detail,written.status);
  auto r=Success<TimeBytesResultV3>();try{r.bytes.assign(bytes.begin(),bytes.begin()+static_cast<std::ptrdiff_t>(written.bytes_written));}catch(...){return Failure<TimeBytesResultV3>("RESOURCE.BUDGET_EXCEEDED","key_allocation",ResourceStatus());}if(Cancelled(control)){SecureClear(r.bytes.data(),r.bytes.size());return Failure<TimeBytesResultV3>("PROCESS.CANCELLED","before_publication");}return r;
}

TimeSortKeyViewResultV3 DecodeTimeSortKeyNoAllocV3(
    const TimeValidatedProfileHandleV3& profile,std::span<const byte> encoded,
    const TimeExecutionControlV3& control) noexcept {
  if((encoded.size()!=100&&encoded.size()!=108)||std::memcmp(encoded.data(),"SBTIMK01",8)!=0)return Failure<TimeSortKeyViewResultV3>("CTI.TEMPORAL.CANONICAL_ENCODING_INVALID","key_extent_or_magic");
  if(!ProfileValidNoAlloc(profile,&control)||std::memcmp(encoded.data()+8,kSnapshot.bytes.data(),16)!=0||LoadLittle64(encoded.data()+24)!=10||LoadLittle64(encoded.data()+32)!=10||std::memcmp(encoded.data()+40,profile.comparison_fingerprint.data(),32)!=0||std::memcmp(encoded.data()+72,kOrderingPolicy.uuid.bytes.data(),16)!=0||LoadLittle64(encoded.data()+88)!=1)return Failure<TimeSortKeyViewResultV3>("CTI.TEMPORAL.DESCRIPTOR_INVALID","key_authority");
  if(encoded[96]>1||encoded[97]>1)return Failure<TimeSortKeyViewResultV3>("CTI.TEMPORAL.INDEX_KEY_REFUSED","key_mode");
  const auto direction=static_cast<TimeSortDirectionV3>(encoded[96]);const auto mode=static_cast<TimeNullModeV3>(encoded[97]);
  TimeValueStateV3 state=TimeValueStateV3::sql_null;u64 value=0;
  if(encoded.size()==100){if(encoded[98]!=(mode==TimeNullModeV3::nulls_first?0:2)||encoded[99]!=0)return Failure<TimeSortKeyViewResultV3>("CTI.TEMPORAL.CANONICAL_ENCODING_INVALID","key_null_rank");}
  else{if(encoded[98]!=1||encoded[99]!=8)return Failure<TimeSortKeyViewResultV3>("CTI.TEMPORAL.CANONICAL_ENCODING_INVALID","key_value_rank");for(unsigned i=0;i<8;++i){byte x=encoded[100+i];if(direction==TimeSortDirectionV3::descending)x=static_cast<byte>(~x);value=(value<<8)|x;}if(value>kTimeMaximumNanosecondsV3)return Failure<TimeSortKeyViewResultV3>("CTI.TEMPORAL.CANONICAL_ENCODING_INVALID","key_range");state=TimeValueStateV3::value;}
  TimeOwnedValueV3 owned{std::shared_ptr<const TimeValidatedProfileHandleV3>{},state,value};
  std::array<byte,108> check{};ScopedClear clear_check(check.data(),check.size(),TimeScrubClassV3::ordered_key_decode_reencode,control.observe_scrubbed,control.scrub_observer_context);std::memcpy(check.data(),"SBTIMK01",8);PutUuid(check.data()+8,kSnapshot);StoreLittle64(check.data()+24,10);StoreLittle64(check.data()+32,10);std::memcpy(check.data()+40,profile.comparison_fingerprint.data(),32);PutPolicy(check.data()+72,kOrderingPolicy);check[96]=encoded[96];check[97]=encoded[97];check[98]=state==TimeValueStateV3::value?1:(mode==TimeNullModeV3::nulls_first?0:2);check[99]=state==TimeValueStateV3::value?8:0;if(state==TimeValueStateV3::value){for(unsigned i=0;i<8;++i)check[100+i]=static_cast<byte>(value>>(56-8*i));if(direction==TimeSortDirectionV3::descending)for(unsigned i=100;i<108;++i)check[i]=static_cast<byte>(~check[i]);}
  if(control.force_reencode_mismatch_for_conformance)check[0]^=1;
  if(!std::equal(check.begin(),check.begin()+static_cast<std::ptrdiff_t>(encoded.size()),encoded.begin()))return Failure<TimeSortKeyViewResultV3>("CTI.TEMPORAL.CANONICAL_ENCODING_INVALID","key_reencode");
  if(Cancelled(control))return Failure<TimeSortKeyViewResultV3>("PROCESS.CANCELLED","before_publication");
  auto r=Success<TimeSortKeyViewResultV3>();r.value={&profile,direction,mode,state,value};return r;
}

TimeCastPolicyDispositionV3 ClassifyTimeCastPolicyRowV3(u32 row,DatatypeCastContext context) noexcept {
  if(row==0||row>221||(context!=DatatypeCastContext::implicit&&context!=DatatypeCastContext::assignment&&context!=DatatypeCastContext::explicit_cast))return TimeCastPolicyDispositionV3::forbidden;
  if(row==1)return TimeCastPolicyDispositionV3::contextual_null;
  if(row==53)return TimeCastPolicyDispositionV3::identity;
  if(row==24&&context==DatatypeCastContext::explicit_cast)return TimeCastPolicyDispositionV3::explicit_character_to_time;
  if(row==50&&context==DatatypeCastContext::explicit_cast)return TimeCastPolicyDispositionV3::explicit_time_to_character;
  return TimeCastPolicyDispositionV3::forbidden;
}

TimeCastResultV3 CastTimeValueV3(const TimeCastRequestV3& request) noexcept {
  if(request.one_based_policy_row==0||request.one_based_policy_row>221)return Failure<TimeCastResultV3>("DATATYPE.CAST_FORBIDDEN","policy_row");
  const auto disposition=ClassifyTimeCastPolicyRowV3(request.one_based_policy_row,request.context);
  if(disposition==TimeCastPolicyDispositionV3::identity){
    if(request.time_source==nullptr||request.time_target==nullptr||!*request.time_target)return Failure<TimeCastResultV3>("CTI.TEMPORAL.DESCRIPTOR_INVALID","identity_shape");
    const auto source_authority=ValidateTimeAuthorityEnvelope(request.time_source->view());
    if(!source_authority.ok())return Failure<TimeCastResultV3>(source_authority.diagnostic.diagnostic_code,source_authority.diagnostic.detail,source_authority.status);
    if(!ProfileValidNoAlloc(**request.time_target)||
       !TimeDescriptor(request.time_target_descriptor,(*request.time_target)->identity,request.target_null_allowed)||
       request.time_source->profile->comparison_fingerprint!=(*request.time_target)->comparison_fingerprint)
      return Failure<TimeCastResultV3>("CTI.TEMPORAL.DESCRIPTOR_INVALID","identity_cohort");
    const auto source_state=ValidateTimeStateEnvelope(request.time_source->view());
    if(!source_state.ok())return Failure<TimeCastResultV3>(source_state.diagnostic.diagnostic_code,source_state.diagnostic.detail,source_state.status);
    if(request.time_source->state==TimeValueStateV3::sql_null&&!request.target_null_allowed)return Failure<TimeCastResultV3>("DATATYPE.NULL_NOT_ADMITTED","target_null");
    const auto source_range=ValidateTimePresentRange(request.time_source->view());
    if(!source_range.ok())return Failure<TimeCastResultV3>(source_range.diagnostic.diagnostic_code,source_range.diagnostic.detail,source_range.status);
    if(request.time_source->state==TimeValueStateV3::value&&request.control.maximum_allocation_bytes<8){auto r=Failure<TimeCastResultV3>("RESOURCE.BUDGET_EXCEEDED","identity_grant",ResourceStatus());r.bytes_required=8;return r;}
    if(Cancelled(request.control))
      return Failure<TimeCastResultV3>("PROCESS.CANCELLED","before_publication");
    auto r=Success<TimeCastResultV3>();r.category=DatatypeCastCategory::identity;r.produced_time=true;r.time_value={*request.time_target,request.time_source->state,request.time_source->nanoseconds_since_midnight};return r;
  }
  if(disposition==TimeCastPolicyDispositionV3::contextual_null){
    if(request.time_target==nullptr||!*request.time_target||
       !ProfileValidNoAlloc(**request.time_target)||
       !TimeDescriptor(request.time_target_descriptor,(*request.time_target)->identity,request.target_null_allowed))
      return Failure<TimeCastResultV3>("CTI.TEMPORAL.DESCRIPTOR_INVALID","target_profile");
    if(request.scalar_source==nullptr||request.scalar_source->type_id!=CanonicalTypeId::null_type||!request.scalar_source->is_null||!request.scalar_source->encoded_value.empty())return Failure<TimeCastResultV3>("DATATYPE.NULL_STATE.INVALID","contextual_null");
    if(!request.target_null_allowed)
      return Failure<TimeCastResultV3>("DATATYPE.NULL_NOT_ADMITTED","target_null");
    if(Cancelled(request.control))return Failure<TimeCastResultV3>("PROCESS.CANCELLED","before_publication");
    auto r=Success<TimeCastResultV3>();r.category=DatatypeCastCategory::identity;r.produced_time=true;r.time_value={*request.time_target,TimeValueStateV3::sql_null,0};return r;
  }
  if(disposition==TimeCastPolicyDispositionV3::explicit_character_to_time){
    if(request.scalar_source==nullptr||request.scalar_source->type_id!=CanonicalTypeId::character||!CharacterIdentity(request.scalar_source_identity)||!CharacterDescriptor(&request.scalar_source->descriptor,request.scalar_source_identity))return Failure<TimeCastResultV3>("CTI.TEMPORAL.DESCRIPTOR_INVALID","character_source_authority");
    if(request.time_target==nullptr||!*request.time_target||!ProfileValidNoAlloc(**request.time_target)||!TimeDescriptor(request.time_target_descriptor,(*request.time_target)->identity,request.target_null_allowed))return Failure<TimeCastResultV3>("CTI.TEMPORAL.DESCRIPTOR_INVALID","time_target_authority");
    if(request.scalar_source->is_null){if(!request.scalar_source->encoded_value.empty())return Failure<TimeCastResultV3>("DATATYPE.NULL_STATE.INVALID","character_dirty_null");if(!request.scalar_source->descriptor.nullable_allowed||!request.target_null_allowed)return Failure<TimeCastResultV3>("DATATYPE.NULL_NOT_ADMITTED","target_null");if(Cancelled(request.control))return Failure<TimeCastResultV3>("PROCESS.CANCELLED","before_publication");auto r=Success<TimeCastResultV3>();r.category=DatatypeCastCategory::lossless_explicit;r.produced_time=true;r.time_value={*request.time_target,TimeValueStateV3::sql_null,0};return r;}
    const auto parsed=ParseStrict(request.scalar_source->encoded_value);if(!parsed.ok)return Failure<TimeCastResultV3>(parsed.code,parsed.detail);if(Cancelled(request.control))return Failure<TimeCastResultV3>("PROCESS.CANCELLED","before_publication");auto r=Success<TimeCastResultV3>();r.category=DatatypeCastCategory::lossless_explicit;r.produced_time=true;r.time_value={*request.time_target,TimeValueStateV3::value,parsed.value};return r;
  }
  if(disposition==TimeCastPolicyDispositionV3::explicit_time_to_character){
    if(request.time_source==nullptr)return Failure<TimeCastResultV3>("CTI.TEMPORAL.DESCRIPTOR_INVALID","time_source_shape");
    const auto source_authority=ValidateTimeAuthorityEnvelope(request.time_source->view());if(!source_authority.ok())return Failure<TimeCastResultV3>(source_authority.diagnostic.diagnostic_code,source_authority.diagnostic.detail,source_authority.status);
    if(request.scalar_target!=CanonicalTypeId::character||!CharacterIdentity(request.scalar_target_identity)||!CharacterDescriptor(&request.scalar_target_descriptor,request.scalar_target_identity)||request.scalar_target_descriptor.nullable_allowed!=request.target_null_allowed)return Failure<TimeCastResultV3>("CTI.TEMPORAL.DESCRIPTOR_INVALID","character_target_authority");
    const auto source_state=ValidateTimeStateEnvelope(request.time_source->view());if(!source_state.ok())return Failure<TimeCastResultV3>(source_state.diagnostic.diagnostic_code,source_state.diagnostic.detail,source_state.status);
    if(request.time_source->state==TimeValueStateV3::sql_null&&!request.target_null_allowed)return Failure<TimeCastResultV3>("DATATYPE.NULL_NOT_ADMITTED","target_null");
    const auto source_range=ValidateTimePresentRange(request.time_source->view());if(!source_range.ok())return Failure<TimeCastResultV3>(source_range.diagnostic.diagnostic_code,source_range.diagnostic.detail,source_range.status);
    std::array<char,18> rendered{};ScopedClear clear_rendered(rendered.data(),rendered.size(),TimeScrubClassV3::character_render_staging,request.control.observe_scrubbed,request.control.scrub_observer_context);const std::size_t extent=request.time_source->state==TimeValueStateV3::value?RenderStrict(request.time_source->nanoseconds_since_midnight,rendered.data()):0;
    if(request.scalar_target_descriptor.length!=0&&extent>request.scalar_target_descriptor.length){auto r=Failure<TimeCastResultV3>("CTB.TEXT.LENGTH_EXCEEDED","character_length");r.bytes_required=extent;return r;}
    if(extent>request.control.maximum_allocation_bytes){auto r=Failure<TimeCastResultV3>("RESOURCE.BUDGET_EXCEEDED","character_budget",ResourceStatus());r.bytes_required=extent;return r;}
    if(request.use_character_output_buffer&&(extent>request.character_output_capacity||(extent&&request.character_output==nullptr))){auto r=Failure<TimeCastResultV3>("CTB.TEXT.LENGTH_EXCEEDED","character_capacity");r.bytes_required=extent;return r;}
    if(request.use_character_output_buffer&&request.character_output!=nullptr&&
       (RangesOverlap(request.character_output,extent,request.time_source,sizeof(*request.time_source))||
        RangesOverlap(request.character_output,extent,&request,sizeof(request))||
        RangesOverlap(request.character_output,extent,&request.control,sizeof(request.control))||
        OutputOverlapsProfile(request.character_output,extent,*request.time_source->profile)))return Failure<TimeCastResultV3>("CTI.TEMPORAL.CANONICAL_ENCODING_INVALID","character_overlap");
    if(Cancelled(request.control))
      return Failure<TimeCastResultV3>("PROCESS.CANCELLED","before_publication");
    auto r=Success<TimeCastResultV3>();r.category=DatatypeCastCategory::lossless_explicit;r.used_character_output_buffer=request.use_character_output_buffer;r.bytes_required=extent;r.bytes_written=extent;
    if(request.use_character_output_buffer){if(extent)std::memcpy(request.character_output,rendered.data(),extent);}else{try{r.scalar_value.type_id=CanonicalTypeId::character;r.scalar_value.is_null=request.time_source->state==TimeValueStateV3::sql_null;r.scalar_value.descriptor=request.scalar_target_descriptor;r.scalar_value.encoded_value.assign(rendered.data(),extent);}catch(...){if(!r.scalar_value.encoded_value.empty())SecureClear(r.scalar_value.encoded_value.data(),r.scalar_value.encoded_value.size());return Failure<TimeCastResultV3>("RESOURCE.BUDGET_EXCEEDED","character_allocation",ResourceStatus());}if(Cancelled(request.control)){if(!r.scalar_value.encoded_value.empty())SecureClear(r.scalar_value.encoded_value.data(),r.scalar_value.encoded_value.size());return Failure<TimeCastResultV3>("PROCESS.CANCELLED","before_publication");}}
    return r;
  }
  return Failure<TimeCastResultV3>("DATATYPE.CAST_FORBIDDEN","closed_time_cast_policy");
}

TimeValidationResultV3 ValidateTimeBatchViewV3(const TimeBatchViewV3& batch) noexcept {
  if(batch.profile==nullptr||!ProfileValidNoAlloc(*batch.profile))return Failure<TimeValidationResultV3>("CTI.TEMPORAL.DESCRIPTOR_INVALID","batch_profile");
  const std::size_t expected=(batch.values.size()+7)/8;if(batch.null_bitmap_lsb0.size()!=expected)return Failure<TimeValidationResultV3>("DATATYPE.NULL_STATE.INVALID","batch_bitmap_extent");
  if(!batch.values.empty()&&batch.values.size()%8!=0){const byte allowed=static_cast<byte>((1u<<(batch.values.size()%8))-1u);if((batch.null_bitmap_lsb0.back()&static_cast<byte>(~allowed))!=0)return Failure<TimeValidationResultV3>("DATATYPE.NULL_STATE.INVALID","batch_bitmap_tail");}
  for(std::size_t i=0;i<batch.values.size();++i){const bool is_null=(batch.null_bitmap_lsb0[i/8]&(1u<<(i%8)))!=0;if(is_null&&batch.values[i]!=0)return Failure<TimeValidationResultV3>("DATATYPE.NULL_STATE.INVALID","batch_dirty_null");if(!is_null&&batch.values[i]>kTimeMaximumNanosecondsV3)return Failure<TimeValidationResultV3>("CTI.TEMPORAL.CANONICAL_ENCODING_INVALID","batch_range");}
  return Success<TimeValidationResultV3>();
}

TimeBatchExtentsResultV3 ComputeTimeBatchExtentsV3(u64 rows,u64 limit) noexcept {
  if(rows>(std::numeric_limits<u64>::max()-7)/8)return Failure<TimeBatchExtentsResultV3>("RESOURCE.BUDGET_EXCEEDED","batch_extent",ResourceStatus());
  const u64 values=rows*8;const u64 bitmap=(rows+7)/8;if(rows!=0&&values/8!=rows)return Failure<TimeBatchExtentsResultV3>("RESOURCE.BUDGET_EXCEEDED","batch_extent",ResourceStatus());if(values>std::numeric_limits<u64>::max()-bitmap||values+bitmap>limit)return Failure<TimeBatchExtentsResultV3>("RESOURCE.BUDGET_EXCEEDED","batch_limit",ResourceStatus());auto r=Success<TimeBatchExtentsResultV3>();r.values_bytes=values;r.bitmap_bytes=bitmap;r.combined_bytes=values+bitmap;return r;
}

TimeBatchExtentsResultV3 MaterializeTimeBatchIntoV3(
    const std::shared_ptr<const TimeValidatedProfileHandleV3>& profile,
    std::span<const u64> values,std::span<const byte> bitmap,
    u64* output_values,u64 output_values_bytes,byte* output_bitmap,u64 output_bitmap_bytes,
    const TimeExecutionControlV3& control) noexcept {
  const auto checked=ValidateTimeBatchViewV3({profile.get(),values,bitmap});if(!checked.ok())return Failure<TimeBatchExtentsResultV3>(checked.diagnostic.diagnostic_code,checked.diagnostic.detail,checked.status);
  auto extents=ComputeTimeBatchExtentsV3(values.size(),control.maximum_allocation_bytes);if(!extents.ok())return extents;
  if((extents.values_bytes&&output_values==nullptr)||(extents.bitmap_bytes&&output_bitmap==nullptr)||output_values_bytes<extents.values_bytes||output_bitmap_bytes<extents.bitmap_bytes){auto r=Failure<TimeBatchExtentsResultV3>("RESOURCE.BUDGET_EXCEEDED","batch_capacity",ResourceStatus());r.values_bytes=extents.values_bytes;r.bitmap_bytes=extents.bitmap_bytes;r.combined_bytes=extents.combined_bytes;return r;}
  if((output_values&&RangesOverlap(output_values,extents.values_bytes,values.data(),extents.values_bytes))||(output_bitmap&&RangesOverlap(output_bitmap,extents.bitmap_bytes,bitmap.data(),extents.bitmap_bytes))||RangesOverlap(output_values,extents.values_bytes,output_bitmap,extents.bitmap_bytes)||RangesOverlap(output_values,extents.values_bytes,&control,sizeof(control))||RangesOverlap(output_bitmap,extents.bitmap_bytes,&control,sizeof(control))||OutputOverlapsProfile(output_values,extents.values_bytes,*profile)||OutputOverlapsProfile(output_bitmap,extents.bitmap_bytes,*profile))return Failure<TimeBatchExtentsResultV3>("CTI.TEMPORAL.CANONICAL_ENCODING_INVALID","batch_overlap");
  if(output_values&&reinterpret_cast<std::uintptr_t>(output_values)%alignof(u64)!=0)return Failure<TimeBatchExtentsResultV3>("CTI.TEMPORAL.CANONICAL_ENCODING_INVALID","batch_alignment");
  if(Cancelled(control))return Failure<TimeBatchExtentsResultV3>("PROCESS.CANCELLED","batch_before_allocation");
  std::vector<u64> staged_values;std::vector<byte> staged_bitmap;ScopedVectorClear clear_values(&staged_values,TimeScrubClassV3::batch_values_staging,control.observe_scrubbed,control.scrub_observer_context);ScopedVectorClear clear_bitmap(&staged_bitmap,TimeScrubClassV3::batch_bitmap_staging,control.observe_scrubbed,control.scrub_observer_context);try{staged_values.assign(values.begin(),values.end());staged_bitmap.assign(bitmap.begin(),bitmap.end());}catch(...){return Failure<TimeBatchExtentsResultV3>("RESOURCE.BUDGET_EXCEEDED","batch_allocation",ResourceStatus());}
  if(Cancelled(control)){if(!staged_values.empty())SecureClear(staged_values.data(),staged_values.size()*8);if(!staged_bitmap.empty())SecureClear(staged_bitmap.data(),staged_bitmap.size());return Failure<TimeBatchExtentsResultV3>("PROCESS.CANCELLED","batch_before_publication");}
  if(extents.values_bytes)
    std::memcpy(output_values,staged_values.data(),extents.values_bytes);
  if(extents.bitmap_bytes)
    std::memcpy(output_bitmap,staged_bitmap.data(),extents.bitmap_bytes);
  return extents;
}

TimeBatchResultV3 MaterializeTimeBatchV3(
    std::shared_ptr<const TimeValidatedProfileHandleV3> profile,
    std::span<const u64> values,std::span<const byte> bitmap,
    const TimeExecutionControlV3& control) noexcept {
  const auto checked=ValidateTimeBatchViewV3({profile.get(),values,bitmap});if(!checked.ok())return Failure<TimeBatchResultV3>(checked.diagnostic.diagnostic_code,checked.diagnostic.detail,checked.status);
  const auto extents=ComputeTimeBatchExtentsV3(values.size(),control.maximum_allocation_bytes);if(!extents.ok())return Failure<TimeBatchResultV3>(extents.diagnostic.diagnostic_code,extents.diagnostic.detail,extents.status);if(Cancelled(control))return Failure<TimeBatchResultV3>("PROCESS.CANCELLED","batch_before_allocation");
  auto r=Success<TimeBatchResultV3>();r.batch.profile=std::move(profile);try{r.batch.values.assign(values.begin(),values.end());r.batch.null_bitmap_lsb0.assign(bitmap.begin(),bitmap.end());}catch(...){if(!r.batch.values.empty())SecureClear(r.batch.values.data(),r.batch.values.size()*sizeof(u64));if(!r.batch.null_bitmap_lsb0.empty())SecureClear(r.batch.null_bitmap_lsb0.data(),r.batch.null_bitmap_lsb0.size());return Failure<TimeBatchResultV3>("RESOURCE.BUDGET_EXCEEDED","batch_allocation",ResourceStatus());}if(Cancelled(control)){if(!r.batch.values.empty())SecureClear(r.batch.values.data(),r.batch.values.size()*8);if(!r.batch.null_bitmap_lsb0.empty())SecureClear(r.batch.null_bitmap_lsb0.data(),r.batch.null_bitmap_lsb0.size());return Failure<TimeBatchResultV3>("PROCESS.CANCELLED","batch_before_publication");}return r;
}

TimeViewResultV3 DecodeTimeSbdvalComposedNoAllocV3(
    const TimeValidatedProfileHandleV3& profile,bool null_allowed,
    std::span<const byte> encoded,const TimeExecutionControlV3& control) noexcept {
  if(encoded.size()<32||std::memcmp(encoded.data(),"SBDVAL01",8)!=0||
     LoadLittle16(encoded.data()+14)!=32||
     LoadLittle32(encoded.data()+16)!=encoded.size()-32)
    return Failure<TimeViewResultV3>("CTI.TEMPORAL.CANONICAL_ENCODING_INVALID","sbdval_structure");
  const u16 flags=LoadLittle16(encoded.data()+12);
  if((flags&~u16{3})!=0||LoadLittle32(encoded.data()+20)!=0||
     LoadLittle64(encoded.data()+24)!=ComputeDatatypeBinaryPayloadChecksumV1(
         encoded.data()+32,encoded.size()-32))
    return Failure<TimeViewResultV3>("CTI.TEMPORAL.CANONICAL_ENCODING_INVALID","sbdval_integrity");
  if(static_cast<CanonicalTypeId>(LoadLittle32(encoded.data()+8))!=CanonicalTypeId::time)
    return Failure<TimeViewResultV3>("CTI.TEMPORAL.DESCRIPTOR_INVALID","sbdval_type");
  if(!ProfileValidNoAlloc(profile,&control))return Failure<TimeViewResultV3>("CTI.TEMPORAL.DESCRIPTOR_INVALID","sbdval_profile");
  if((flags&2)!=0)return Failure<TimeViewResultV3>("CTI.TEMPORAL.CANONICAL_ENCODING_INVALID","sbdval_toast");
  const bool is_null=(flags&1)!=0;
  auto decoded=DecodeCanonicalTimeComponentNoAllocV3(
      profile,is_null?TimeValueStateV3::sql_null:TimeValueStateV3::value,
      null_allowed,{encoded.data()+32,encoded.size()-32});
  if(!decoded.ok())return decoded;
  std::array<byte,40> check{};ScopedClear clear_check(check.data(),check.size(),TimeScrubClassV3::sbdval_reencode,control.observe_scrubbed,control.scrub_observer_context);
  const DatatypeBinaryValueView view{CanonicalTypeId::time,is_null,false,
      is_null?nullptr:encoded.data()+32,encoded.size()-32};
  const auto rewritten=EncodeDatatypeBinaryStructuralValueIntoNoAlloc(view,check.data(),encoded.size());
  if(control.force_reencode_mismatch_for_conformance)check[0]^=1;
  if(!rewritten.ok()||rewritten.bytes_written!=encoded.size()||
     !std::equal(check.begin(),check.begin()+static_cast<std::ptrdiff_t>(encoded.size()),encoded.begin()))
    return Failure<TimeViewResultV3>("CTI.TEMPORAL.CANONICAL_ENCODING_INVALID","sbdval_reencode");
  if(encoded.size()>control.maximum_allocation_bytes)
    return Failure<TimeViewResultV3>("RESOURCE.BUDGET_EXCEEDED","sbdval_decode_budget",ResourceStatus());
  if(Cancelled(control))return Failure<TimeViewResultV3>("PROCESS.CANCELLED","sbdval_before_publication");
  auto r=Success<TimeViewResultV3>();r.value={&profile,decoded.value.state,decoded.value.nanoseconds_since_midnight};return r;
}

TimeBytesResultV3 EncodeTimeSbdvalComposedV3(const TimeOwnedValueV3& value,
    bool null_allowed,const TimeExecutionControlV3& control) noexcept {
  const auto checked=ValidateTimeValueViewV3(value.view(),null_allowed);if(!checked.ok())return Failure<TimeBytesResultV3>(checked.diagnostic.diagnostic_code,checked.diagnostic.detail,checked.status);const std::size_t payload=value.state==TimeValueStateV3::value?8:0,total=32+payload;if(total>control.maximum_allocation_bytes)return Failure<TimeBytesResultV3>("RESOURCE.BUDGET_EXCEEDED","sbdval_capacity",ResourceStatus());if(Cancelled(control))return Failure<TimeBytesResultV3>("PROCESS.CANCELLED","before_allocation");std::array<byte,8> component{};ScopedClear clear_component(component.data(),component.size(),TimeScrubClassV3::sbdval_component_staging,control.observe_scrubbed,control.scrub_observer_context);if(payload)StoreLittle64(component.data(),value.nanoseconds_since_midnight);auto r=Success<TimeBytesResultV3>();try{r.bytes.resize(total);}catch(...){return Failure<TimeBytesResultV3>("RESOURCE.BUDGET_EXCEEDED","sbdval_allocation",ResourceStatus());}const DatatypeBinaryValueView view{CanonicalTypeId::time,value.state==TimeValueStateV3::sql_null,false,payload?component.data():nullptr,payload};const auto encoded=EncodeDatatypeBinaryStructuralValueIntoNoAlloc(view,r.bytes.data(),r.bytes.size());if(!encoded.ok()||encoded.bytes_written!=total){SecureClear(r.bytes.data(),r.bytes.size());return Failure<TimeBytesResultV3>(encoded.diagnostic.diagnostic_code,"sbdval_encode",encoded.status);}const auto recheck=DecodeTimeSbdvalComposedNoAllocV3(*value.profile,null_allowed,r.bytes,{});if(!recheck.ok()){SecureClear(r.bytes.data(),r.bytes.size());return Failure<TimeBytesResultV3>(recheck.diagnostic.diagnostic_code,"sbdval_recheck",recheck.status);}if(Cancelled(control)){SecureClear(r.bytes.data(),r.bytes.size());return Failure<TimeBytesResultV3>("PROCESS.CANCELLED","before_publication");}return r;
}

TimeViewResultV3 DecodeTimeSbdpvComposedNoAllocV3(
    const TimeValidatedProfileHandleV3& profile,bool null_allowed,
    std::span<const byte> encoded,const TimeExecutionControlV3& control) noexcept {
  if(encoded.size()<24||std::memcmp(encoded.data(),"SBDPV001",8)!=0||
     LoadLittle16(encoded.data()+14)!=0||
     LoadLittle32(encoded.data()+16)!=encoded.size()-24)
    return Failure<TimeViewResultV3>("CTI.TEMPORAL.CANONICAL_ENCODING_INVALID","sbdpv_structure");
  const auto type=static_cast<CanonicalTypeId>(LoadLittle32(encoded.data()+8));
  const auto state=static_cast<DatatypePhysicalValueState>(LoadLittle16(encoded.data()+12));
  u32 checksum=2166136261u;
  const auto mix=[&checksum](u32 next) noexcept {checksum^=next;checksum*=16777619u;};
  mix(static_cast<u32>(type));mix(static_cast<u32>(state));
  for(std::size_t i=24;i<encoded.size();++i)mix(encoded[i]);
  if(checksum!=LoadLittle32(encoded.data()+20))
    return Failure<TimeViewResultV3>("CTI.TEMPORAL.CANONICAL_ENCODING_INVALID","sbdpv_integrity");
  if(type!=CanonicalTypeId::time)return Failure<TimeViewResultV3>("CTI.TEMPORAL.DESCRIPTOR_INVALID","sbdpv_type");
  if(!ProfileValidNoAlloc(profile,&control))return Failure<TimeViewResultV3>("CTI.TEMPORAL.DESCRIPTOR_INVALID","sbdpv_profile");
  if(state!=DatatypePhysicalValueState::value&&state!=DatatypePhysicalValueState::sql_null)
    return Failure<TimeViewResultV3>("DATATYPE.NULL_STATE.INVALID","sbdpv_state");
  const bool is_null=state==DatatypePhysicalValueState::sql_null;
  auto decoded=DecodeCanonicalTimeComponentNoAllocV3(
      profile,is_null?TimeValueStateV3::sql_null:TimeValueStateV3::value,
      null_allowed,{encoded.data()+24,encoded.size()-24});
  if(!decoded.ok())return decoded;
  std::array<byte,32> check{};ScopedClear clear_check(check.data(),check.size(),TimeScrubClassV3::sbdpv_reencode,control.observe_scrubbed,control.scrub_observer_context);
  const DatatypePhysicalValueView view{CanonicalTypeId::time,state,
      is_null?nullptr:encoded.data()+24,encoded.size()-24};
  const auto rewritten=EncodeDatatypePhysicalStructuralValueIntoNoAlloc(view,check.data(),encoded.size());
  if(control.force_reencode_mismatch_for_conformance)check[0]^=1;
  if(!rewritten.ok()||rewritten.bytes_written!=encoded.size()||
     !std::equal(check.begin(),check.begin()+static_cast<std::ptrdiff_t>(encoded.size()),encoded.begin()))
    return Failure<TimeViewResultV3>("CTI.TEMPORAL.CANONICAL_ENCODING_INVALID","sbdpv_reencode");
  if(encoded.size()>control.maximum_allocation_bytes)
    return Failure<TimeViewResultV3>("RESOURCE.BUDGET_EXCEEDED","sbdpv_decode_budget",ResourceStatus());
  if(Cancelled(control))return Failure<TimeViewResultV3>("PROCESS.CANCELLED","sbdpv_before_publication");
  auto r=Success<TimeViewResultV3>();r.value={&profile,decoded.value.state,decoded.value.nanoseconds_since_midnight};return r;
}

TimeBytesResultV3 EncodeTimeSbdpvComposedV3(const TimeOwnedValueV3& value,
    bool null_allowed,const TimeExecutionControlV3& control) noexcept {
  const auto checked=ValidateTimeValueViewV3(value.view(),null_allowed);if(!checked.ok())return Failure<TimeBytesResultV3>(checked.diagnostic.diagnostic_code,checked.diagnostic.detail,checked.status);const std::size_t payload=value.state==TimeValueStateV3::value?8:0,total=24+payload;if(total>control.maximum_allocation_bytes)return Failure<TimeBytesResultV3>("RESOURCE.BUDGET_EXCEEDED","sbdpv_capacity",ResourceStatus());if(Cancelled(control))return Failure<TimeBytesResultV3>("PROCESS.CANCELLED","before_allocation");std::array<byte,8> component{};ScopedClear clear_component(component.data(),component.size(),TimeScrubClassV3::sbdpv_component_staging,control.observe_scrubbed,control.scrub_observer_context);if(payload)StoreLittle64(component.data(),value.nanoseconds_since_midnight);auto r=Success<TimeBytesResultV3>();try{r.bytes.resize(total);}catch(...){return Failure<TimeBytesResultV3>("RESOURCE.BUDGET_EXCEEDED","sbdpv_allocation",ResourceStatus());}const DatatypePhysicalValueView view{CanonicalTypeId::time,value.state==TimeValueStateV3::sql_null?DatatypePhysicalValueState::sql_null:DatatypePhysicalValueState::value,payload?component.data():nullptr,payload};const auto encoded=EncodeDatatypePhysicalStructuralValueIntoNoAlloc(view,r.bytes.data(),r.bytes.size());if(!encoded.ok()||encoded.bytes_written!=total){SecureClear(r.bytes.data(),r.bytes.size());return Failure<TimeBytesResultV3>(encoded.diagnostic.diagnostic_code,"sbdpv_encode",encoded.status);}const auto recheck=DecodeTimeSbdpvComposedNoAllocV3(*value.profile,null_allowed,r.bytes,{});if(!recheck.ok()){SecureClear(r.bytes.data(),r.bytes.size());return Failure<TimeBytesResultV3>(recheck.diagnostic.diagnostic_code,"sbdpv_recheck",recheck.status);}if(Cancelled(control)){SecureClear(r.bytes.data(),r.bytes.size());return Failure<TimeBytesResultV3>("PROCESS.CANCELLED","before_publication");}return r;
}

DiagnosticRecord MakeTimeDiagnosticV3(Status status,std::string diagnostic_code,
                                      std::string message_key,std::string detail) {
  return MakeDatatypeOperationDiagnostic(status,std::move(diagnostic_code),
                                         std::move(message_key),std::move(detail));
}

}  // namespace scratchbird::core::datatypes

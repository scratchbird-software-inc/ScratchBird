// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "datatype_date.hpp"
#include "datatype_timestamp_diagnostic.hpp"
#include "datatype_operations.hpp"
#include "datatype_type_codec_identity_v3.hpp"

#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace scratchbird::core::datatypes {

using platform::byte;
using platform::u8;
using platform::u16;
using platform::u32;
using platform::u64;

inline constexpr u64 kTimestampMaximumNanosecondsV3 = 86'399'999'999'999ull;
inline constexpr std::int64_t kTimestampMinimumCivilSecondV3 = -185'542'587'187'200ll;
inline constexpr std::int64_t kTimestampMaximumCivilSecondV3 = 185'542'587'187'199ll;
inline constexpr u32 kTimestampProfileMaterialBytesV3 = 656;
inline constexpr u32 kTimestampComparisonMaterialBytesV3 = 416;
inline constexpr u32 kTimestampComponentBytesV3 = 16;
inline constexpr u32 kTimestampNullSortKeyBytesV3 = 100;
inline constexpr u32 kTimestampValueSortKeyBytesV3 = 112;
inline constexpr u32 kTimestampHashBytesV3 = 32;
inline constexpr u32 kTimestampClosedCastPolicyRowsV3 = 221;
inline constexpr u32 kTimestampClosedCastDecisionsV3 = 663;
inline constexpr u32 kTimestampIntrinsicOperationRowsV3 = 33;

struct TimestampAuthorityReceiptV3 {
  platform::Uuid statement_receipt_uuid;
  platform::Uuid catalog_snapshot_uuid;
  u64 catalog_generation = 0;
  u64 registry_generation = 0;
};

struct TimestampValidatedProfileHandleV3 {
  TimestampAuthorityReceiptV3 receipt;
  DatatypeTypeCodecIdentityRowV3 identity;
  DatatypePolicyIdentityV3 render_policy;
  DatatypePolicyIdentityV3 cast_policy;
  DatatypePolicyIdentityV3 calendar_policy;
  DatatypePolicyIdentityV3 storage_epoch_policy;
  DatatypePolicyIdentityV3 timezone_none_policy;
  DatatypePolicyIdentityV3 leap_second_policy;
  DatatypePolicyIdentityV3 index_policy;
  DatatypePolicyIdentityV3 statistics_policy;
  DatatypePolicyIdentityV3 backup_transport_policy;
  DatatypePolicyIdentityV3 protection_policy;
  DatatypePolicyIdentityV3 component_adapter_policy;
  DatatypePolicyIdentityV3 diagnostic_policy;
  DatatypePolicyIdentityV3 metric_policy;
  std::array<byte, kTimestampProfileMaterialBytesV3> profile_material{};
  std::array<byte, kTimestampComparisonMaterialBytesV3> comparison_material{};
  std::array<byte, 32> profile_fingerprint{};
  std::array<byte, 32> comparison_fingerprint{};
};

enum class TimestampValueStateV3 : u8 { value = 0, sql_null = 1 };
enum class TimestampI64CarrierKindV3 : u8 {
  signed_i64 = 0, wrong_host_type = 1, below_i64 = 2, above_i64 = 3,
};
enum class TimestampDayCarrierKindV3 : u8 {
  signed_i32 = 0, wrong_host_type = 1, wrong_host_boolean = 2,
  wrong_host_string = 3, below_i32 = 4, above_i32 = 5,
};
enum class TimestampUnsignedCarrierKindV3 : u8 {
  unsigned_u64 = 0, wrong_host_type = 1, below_zero = 2, above_u64 = 3,
};
enum class TimestampTextCarrierKindV3 : u8 { utf8_bytes = 0, wrong_host_type = 1 };

struct TimestampNullableI64FactV3 {
  TimestampI64CarrierKindV3 carrier = TimestampI64CarrierKindV3::signed_i64;
  TimestampValueStateV3 state = TimestampValueStateV3::value;
  std::int64_t value = 0;
};
struct TimestampNullableUnsignedFactV3 {
  TimestampUnsignedCarrierKindV3 carrier = TimestampUnsignedCarrierKindV3::unsigned_u64;
  TimestampValueStateV3 state = TimestampValueStateV3::value;
  u64 value = 0;
};
struct TimestampTextOperandV3 {
  const DatatypeTypeCodecIdentityRowV3* identity = nullptr;
  const scratchbird::engine::ExecutionTypeDescriptor* descriptor = nullptr;
  TimestampTextCarrierKindV3 carrier = TimestampTextCarrierKindV3::utf8_bytes;
  TimestampValueStateV3 state = TimestampValueStateV3::value;
  std::string_view bytes;
  u64 extent = 0;
};
struct TimestampOperandV3 {
  std::shared_ptr<const TimestampValidatedProfileHandleV3> profile;
  TimestampValueStateV3 state = TimestampValueStateV3::value;
  TimestampDayCarrierKindV3 day_carrier = TimestampDayCarrierKindV3::signed_i32;
  std::int64_t civil_day = 0;
  TimestampUnsignedCarrierKindV3 time_carrier = TimestampUnsignedCarrierKindV3::unsigned_u64;
  u64 nanoseconds_since_midnight = 0;
};

struct TimestampValueViewV3 {
  const TimestampValidatedProfileHandleV3* profile = nullptr;
  TimestampValueStateV3 state = TimestampValueStateV3::value;
  std::int32_t civil_day = 0;
  u64 nanoseconds_since_midnight = 0;
};
struct TimestampOwnedValueV3 {
  std::shared_ptr<const TimestampValidatedProfileHandleV3> profile;
  TimestampValueStateV3 state = TimestampValueStateV3::value;
  std::int32_t civil_day = 0;
  u64 nanoseconds_since_midnight = 0;
  TimestampValueViewV3 view() const noexcept {
    return {profile.get(), state, civil_day, nanoseconds_since_midnight};
  }
};
struct TimestampBatchViewV3 {
  const TimestampValidatedProfileHandleV3* profile = nullptr;
  std::span<const std::int32_t> civil_days;
  std::span<const u64> nanoseconds_since_midnight;
  std::span<const byte> null_bitmap_lsb0;
};
struct TimestampOwnedBatchV3 {
  std::shared_ptr<const TimestampValidatedProfileHandleV3> profile;
  std::vector<std::int32_t> civil_days;
  std::vector<u64> nanoseconds_since_midnight;
  std::vector<byte> null_bitmap_lsb0;
  TimestampBatchViewV3 view() const noexcept {
    return {profile.get(), civil_days, nanoseconds_since_midnight, null_bitmap_lsb0};
  }
};

enum class TimestampScrubClassV3 : u8 {
  sha_state, sha_schedule, sha_tail,
  profile_material, comparison_material, profile_digest, comparison_digest,
  component_decode_reencode, component_encode_staging, component_owned_buffer,
  render_staging, render_owned_buffer, hash_preimage, hash_digest, hash_owned_buffer,
  ordered_key_staging, ordered_key_owned_buffer, ordered_key_decode_reencode,
  character_render_staging, batch_day_staging, batch_time_staging, batch_bitmap_staging,
  sbdval_reencode, sbdval_component_staging, sbdval_owned_buffer,
  sbdpv_reencode, sbdpv_component_staging, sbdpv_owned_buffer,
  covering_decode_reencode, zone_map_decode_reencode,
  statistics_decode_reencode, statistics_prior_hash,
  backup_decode_reencode, projection_owned_buffer, count,
};
struct TimestampExecutionControlV3 {
  u64 maximum_allocation_bytes = ~u64{0};
  bool (*cancelled)(void*) noexcept = nullptr;
  void* cancellation_context = nullptr;
  void (*observe_scrubbed)(void*, TimestampScrubClassV3, const byte*, u64) noexcept = nullptr;
  void* scrub_observer_context = nullptr;
  bool force_reencode_mismatch_for_conformance = false;
};


#define SB_TIMESTAMP_RESULT(name, payload) \
  struct name { Status status; TimestampDiagnosticFactV3 diagnostic; payload; \
    bool ok() const noexcept { return status.ok(); } }
SB_TIMESTAMP_RESULT(TimestampProfileResultV3, TimestampValidatedProfileHandleV3 profile);
struct TimestampValidationResultV3 {
  Status status;
  TimestampDiagnosticFactV3 diagnostic;
  bool ok() const noexcept { return status.ok(); }
};
SB_TIMESTAMP_RESULT(TimestampViewResultV3, TimestampValueViewV3 value);
SB_TIMESTAMP_RESULT(TimestampValueResultV3, TimestampOwnedValueV3 value);
SB_TIMESTAMP_RESULT(TimestampBytesResultV3, std::vector<byte> bytes);
SB_TIMESTAMP_RESULT(TimestampTextResultV3, std::string text; bool containing_null = false);
SB_TIMESTAMP_RESULT(TimestampNoAllocWriteResultV3, u64 bytes_required = 0; u64 bytes_written = 0; bool containing_null = false);
SB_TIMESTAMP_RESULT(TimestampBatchExtentsResultV3, u64 civil_days_bytes = 0; u64 nanoseconds_bytes = 0; u64 bitmap_bytes = 0; u64 combined_bytes = 0);
SB_TIMESTAMP_RESULT(TimestampBatchResultV3, TimestampOwnedBatchV3 batch);

TimestampProfileResultV3 BuildCurrentTimestampValidatedProfileHandleV3(
    const platform::Uuid& statement_receipt_uuid) noexcept;
TimestampProfileResultV3 BuildTimestampValidatedProfileHandleV3(
    const TimestampAuthorityReceiptV3& receipt,
    const DatatypeTypeCodecIdentityRowV3& identity) noexcept;
TimestampValidationResultV3 ValidateTimestampProfileHandleV3(
    const TimestampValidatedProfileHandleV3& profile,
    const TimestampExecutionControlV3& control = {}) noexcept;
// Representation authority only; containing object/operation admission is
// retained by the caller. Nullable and nonnullable concrete slots are valid.
TimestampValidationResultV3 ValidateTimestampExecutionDescriptorV3(
    const scratchbird::engine::ExecutionTypeDescriptor& descriptor,
    const DatatypeTypeCodecIdentityRowV3& identity) noexcept;
TimestampViewResultV3 ValidateTimestampValueViewV3(
    const TimestampValueViewV3& value, bool null_allowed = true) noexcept;
TimestampViewResultV3 AdmitTimestampOperandV3(
    const TimestampOperandV3& operand, bool null_allowed = true) noexcept;
TimestampViewResultV3 DecodeCanonicalTimestampComponentNoAllocV3(
    const TimestampValidatedProfileHandleV3& profile, TimestampValueStateV3 state,
    bool null_allowed, std::span<const byte> component,
    const TimestampExecutionControlV3& control = {}) noexcept;
TimestampBytesResultV3 EncodeCanonicalTimestampComponentV3(
    const TimestampOwnedValueV3& value,
    const TimestampExecutionControlV3& control = {}) noexcept;
TimestampNoAllocWriteResultV3 EncodeCanonicalTimestampComponentIntoNoAllocV3(
    const TimestampOwnedValueV3& value, byte* output, u64 output_capacity,
    const TimestampExecutionControlV3& control = {}) noexcept;

struct TimestampCivilV3 {
  std::int32_t year = 0;
  u8 month = 1, day = 1, hour = 0, minute = 0, second = 0;
  u32 nanosecond = 0;
};
SB_TIMESTAMP_RESULT(TimestampCivilResultV3, bool is_null = false; TimestampCivilV3 civil);
SB_TIMESTAMP_RESULT(TimestampScalarResultV3, bool is_null = false; std::int64_t signed_value = 0; u64 unsigned_value = 0; bool boolean_value = false);
struct TimestampExactDifferenceV3 {
  bool negative = false;
  u64 magnitude_whole_days = 0;
  u64 magnitude_nanoseconds_remainder = 0;
};
SB_TIMESTAMP_RESULT(TimestampDifferenceResultV3, bool is_null = false; TimestampExactDifferenceV3 difference);

TimestampValueResultV3 ConstructTimestampFromCivilV3(
    const std::shared_ptr<const TimestampValidatedProfileHandleV3>& profile,
    std::int64_t year, u64 month, u64 day, u64 hour, u64 minute,
    u64 second, u64 nanosecond, bool null_allowed = true,
    const TimestampExecutionControlV3& control = {}) noexcept;
TimestampValueResultV3 ConstructTimestampFromCivilV3(
    const std::shared_ptr<const TimestampValidatedProfileHandleV3>& profile,
    const TimestampNullableI64FactV3& year,
    const TimestampNullableUnsignedFactV3& month,
    const TimestampNullableUnsignedFactV3& day,
    const TimestampNullableUnsignedFactV3& hour,
    const TimestampNullableUnsignedFactV3& minute,
    const TimestampNullableUnsignedFactV3& second,
    const TimestampNullableUnsignedFactV3& nanosecond,
    bool null_allowed = true,
    const TimestampExecutionControlV3& control = {}) noexcept;
TimestampCivilResultV3 DecomposeTimestampCivilV3(
    const TimestampValueViewV3& value, bool null_allowed = true,
    const TimestampExecutionControlV3& control = {}) noexcept;
TimestampValueResultV3 ParseCanonicalTimestampV3(
    const std::shared_ptr<const TimestampValidatedProfileHandleV3>& profile,
    std::string_view text, bool null_allowed = true,
    const TimestampExecutionControlV3& control = {}) noexcept;
TimestampValueResultV3 ParseCanonicalTimestampOperandV3(
    const std::shared_ptr<const TimestampValidatedProfileHandleV3>& profile,
    const TimestampTextOperandV3& operand, bool null_allowed = true,
    const TimestampExecutionControlV3& control = {}) noexcept;
TimestampTextResultV3 RenderCanonicalTimestampV3(
    const TimestampOwnedValueV3& value, bool export_literal = false,
    const TimestampExecutionControlV3& control = {}) noexcept;
TimestampNoAllocWriteResultV3 RenderCanonicalTimestampIntoNoAllocV3(
    const TimestampOwnedValueV3& value, bool export_literal, char* output,
    u64 output_capacity, const TimestampExecutionControlV3& control = {}) noexcept;
TimestampValueResultV3 ValidateCanonicalTimestampV3(
    const TimestampOwnedValueV3& value, bool null_allowed = true,
    const TimestampExecutionControlV3& control = {}) noexcept;
TimestampValueResultV3 TruncateTimestampNanosecondV3(
    const TimestampOwnedValueV3& value, bool null_allowed = true,
    const TimestampExecutionControlV3& control = {}) noexcept;
TimestampValueResultV3 RoundTimestampNanosecondV3(
    const TimestampOwnedValueV3& value, bool null_allowed = true,
    const TimestampExecutionControlV3& control = {}) noexcept;

enum class TimestampIntrinsicOperationV3 : u8 {
  validate, canonicalize, construct_civil, decompose_civil, parse, render,
  add_i64_nanoseconds_checked, subtract_i64_nanoseconds_checked,
  successor_checked, predecessor_checked, difference_private_magnitude_fact,
  extract_year, extract_month, extract_day, extract_hour, extract_minute,
  extract_second, extract_nanosecond, extract_day_count,
  extract_nanosecond_of_day, is_leap_year, days_in_month, iso_weekday,
  day_of_year, quarter, iso_week, truncate_nanosecond_identity,
  round_nanosecond_identity, calendar_interval_arithmetic,
  timezone_conversion, instant_conversion, larger_unit_truncate_round,
  bucketing,
};
enum class TimestampIntrinsicDispositionV3 : u8 {
  admitted, registered_refused, unknown,
};
TimestampIntrinsicDispositionV3 ClassifyTimestampIntrinsicOperationV3(
    TimestampIntrinsicOperationV3 operation) noexcept;
TimestampValueResultV3 RefuseTimestampIntrinsicOperationV3(
    const TimestampValueViewV3& operand, TimestampIntrinsicOperationV3 operation) noexcept;
TimestampValueResultV3 AddTimestampNanosecondsV3(
    const TimestampOwnedValueV3& value, std::int64_t delta,
    bool null_allowed = true, const TimestampExecutionControlV3& control = {}) noexcept;
TimestampValueResultV3 AddTimestampNanosecondsV3(
    const TimestampOwnedValueV3& value, const TimestampNullableI64FactV3& delta,
    bool null_allowed = true, const TimestampExecutionControlV3& control = {}) noexcept;
TimestampValueResultV3 SubtractTimestampNanosecondsV3(
    const TimestampOwnedValueV3& value, std::int64_t delta,
    bool null_allowed = true, const TimestampExecutionControlV3& control = {}) noexcept;
TimestampValueResultV3 SubtractTimestampNanosecondsV3(
    const TimestampOwnedValueV3& value, const TimestampNullableI64FactV3& delta,
    bool null_allowed = true, const TimestampExecutionControlV3& control = {}) noexcept;
TimestampValueResultV3 TimestampSuccessorV3(const TimestampOwnedValueV3& value,
    bool null_allowed = true, const TimestampExecutionControlV3& control = {}) noexcept;
TimestampValueResultV3 TimestampPredecessorV3(const TimestampOwnedValueV3& value,
    bool null_allowed = true, const TimestampExecutionControlV3& control = {}) noexcept;
TimestampDifferenceResultV3 DifferenceTimestampV3(
    const TimestampValueViewV3& left, const TimestampValueViewV3& right,
    bool null_allowed = true, const TimestampExecutionControlV3& control = {}) noexcept;

#define SB_TIMESTAMP_SCALAR_DECL(name) TimestampScalarResultV3 name( \
    const TimestampValueViewV3& value, bool null_allowed = true, \
    const TimestampExecutionControlV3& control = {}) noexcept
SB_TIMESTAMP_SCALAR_DECL(ExtractTimestampYearV3);
SB_TIMESTAMP_SCALAR_DECL(ExtractTimestampMonthV3);
SB_TIMESTAMP_SCALAR_DECL(ExtractTimestampDayV3);
SB_TIMESTAMP_SCALAR_DECL(ExtractTimestampHourV3);
SB_TIMESTAMP_SCALAR_DECL(ExtractTimestampMinuteV3);
SB_TIMESTAMP_SCALAR_DECL(ExtractTimestampSecondV3);
SB_TIMESTAMP_SCALAR_DECL(ExtractTimestampNanosecondV3);
SB_TIMESTAMP_SCALAR_DECL(TimestampDayCountV3);
SB_TIMESTAMP_SCALAR_DECL(TimestampNanosecondOfDayV3);
SB_TIMESTAMP_SCALAR_DECL(TimestampIsLeapYearV3);
SB_TIMESTAMP_SCALAR_DECL(TimestampDaysInMonthV3);
SB_TIMESTAMP_SCALAR_DECL(TimestampIsoWeekdayV3);
SB_TIMESTAMP_SCALAR_DECL(TimestampDayOfYearV3);
SB_TIMESTAMP_SCALAR_DECL(TimestampQuarterV3);
#undef SB_TIMESTAMP_SCALAR_DECL
struct TimestampIsoWeekFactV3 { std::int64_t iso_year = 0; u8 iso_week = 0; };
SB_TIMESTAMP_RESULT(TimestampIsoWeekResultV3, bool is_null = false; TimestampIsoWeekFactV3 value);
TimestampIsoWeekResultV3 TimestampIsoWeekV3(
    const TimestampValueViewV3& value, bool null_allowed = true,
    const TimestampExecutionControlV3& control = {}) noexcept;

enum class TimestampComparisonFactV3 : u8 { less, equal, greater, unordered_null };
SB_TIMESTAMP_RESULT(TimestampComparisonResultV3, TimestampComparisonFactV3 fact = TimestampComparisonFactV3::equal; bool grouping_equivalent = false; bool null_equivalent = false);
TimestampComparisonResultV3 CompareTimestampValuesV3(
    const TimestampValueViewV3& left, const TimestampValueViewV3& right,
    const TimestampExecutionControlV3& control = {}) noexcept;
TimestampComparisonResultV3 CompareTimestampValuesWithValidatedCohortForConformanceV3(
    const TimestampValueViewV3& left, const TimestampValueViewV3& right,
    const std::array<byte, 32>& validated_right_comparison_fingerprint,
    const TimestampExecutionControlV3& control = {}) noexcept;
TimestampBytesResultV3 HashTimestampValueV3(const TimestampOwnedValueV3& value) noexcept;
TimestampBytesResultV3 HashTimestampValueV3(const TimestampOwnedValueV3& value,
    const TimestampExecutionControlV3& control) noexcept;
TimestampNoAllocWriteResultV3 HashTimestampValueIntoNoAllocV3(
    const TimestampOwnedValueV3& value, byte* output, u64 output_capacity,
    const TimestampExecutionControlV3& control = {}) noexcept;

enum class TimestampSortDirectionV3 : u8 { ascending = 0, descending = 1 };
enum class TimestampNullModeV3 : u8 { nulls_first = 0, nulls_last = 1 };
struct TimestampSortKeyViewV3 {
  const TimestampValidatedProfileHandleV3* profile = nullptr;
  TimestampSortDirectionV3 direction = TimestampSortDirectionV3::ascending;
  TimestampNullModeV3 null_mode = TimestampNullModeV3::nulls_first;
  TimestampValueStateV3 state = TimestampValueStateV3::value;
  std::int32_t civil_day = 0;
  u64 nanoseconds_since_midnight = 0;
};
SB_TIMESTAMP_RESULT(TimestampSortKeyViewResultV3, TimestampSortKeyViewV3 value);
TimestampBytesResultV3 MakeTimestampSortKeyV3(const TimestampOwnedValueV3& value,
    TimestampSortDirectionV3 direction, TimestampNullModeV3 null_mode,
    const TimestampExecutionControlV3& control = {}) noexcept;
TimestampNoAllocWriteResultV3 MakeTimestampSortKeyIntoNoAllocV3(
    const TimestampOwnedValueV3& value, TimestampSortDirectionV3 direction,
    TimestampNullModeV3 null_mode, byte* output, u64 output_capacity,
    const TimestampExecutionControlV3& control = {}) noexcept;
// Borrowed profile must outlive this synchronous call. No allocation or
// shared-owner acquisition; overlap/capacity/cancellation remain atomic.
TimestampNoAllocWriteResultV3 MakeTimestampSortKeyViewIntoNoAllocV3(
    const TimestampValueViewV3& value, TimestampSortDirectionV3 direction,
    TimestampNullModeV3 null_mode, byte* output, u64 output_capacity,
    const TimestampExecutionControlV3& control = {}) noexcept;
TimestampSortKeyViewResultV3 DecodeTimestampSortKeyNoAllocV3(
    const TimestampValidatedProfileHandleV3& expected_profile,
    std::span<const byte> encoded,
    const TimestampExecutionControlV3& control = {}) noexcept;

enum class TimestampCastPolicyDispositionV3 : u8 {
  contextual_null, identity, explicit_character_to_timestamp,
  explicit_timestamp_to_character, forbidden,
};
TimestampCastPolicyDispositionV3 ClassifyTimestampCastPolicyRowV3(
    u32 one_based_policy_row, DatatypeCastContext context) noexcept;
struct TimestampCastRequestV3 {
  u32 one_based_policy_row = 0;
  const TimestampOwnedValueV3* timestamp_source = nullptr;
  // Boundary form used before the incoming host carriers have proved the
  // native int32/u64 tuple. Timestamp-source rows require exactly one of the
  // owned and dynamic forms.
  const TimestampOperandV3* dynamic_timestamp_source = nullptr;
  const DatatypeOperationValue* scalar_source = nullptr;
  const DatatypeTypeCodecIdentityRowV3* scalar_source_identity = nullptr;
  const std::shared_ptr<const TimestampValidatedProfileHandleV3>* timestamp_target = nullptr;
  const scratchbird::engine::ExecutionTypeDescriptor* timestamp_target_descriptor = nullptr;
  CanonicalTypeId scalar_target = CanonicalTypeId::unknown;
  scratchbird::engine::ExecutionTypeDescriptor scalar_target_descriptor;
  const DatatypeTypeCodecIdentityRowV3* scalar_target_identity = nullptr;
  DatatypeCastContext context = DatatypeCastContext::implicit;
  bool target_null_allowed = true;
  bool use_character_output_buffer = false;
  char* character_output = nullptr;
  u64 character_output_capacity = 0;
  TimestampExecutionControlV3 control;
};
SB_TIMESTAMP_RESULT(TimestampCastResultV3, DatatypeCastCategory category = DatatypeCastCategory::forbidden; bool produced_timestamp = false; bool used_character_output_buffer = false; u64 bytes_required = 0; u64 bytes_written = 0; TimestampOwnedValueV3 timestamp_value; DatatypeOperationValue scalar_value);
TimestampCastResultV3 CastTimestampValueV3(const TimestampCastRequestV3& request) noexcept;

TimestampValidationResultV3 ValidateTimestampBatchViewV3(const TimestampBatchViewV3& batch) noexcept;
TimestampBatchExtentsResultV3 ComputeTimestampBatchExtentsV3(u64 row_count, u64 size_limit) noexcept;
TimestampBatchExtentsResultV3 MaterializeTimestampBatchIntoV3(
    const std::shared_ptr<const TimestampValidatedProfileHandleV3>& profile,
    std::span<const std::int32_t> civil_days,
    std::span<const u64> nanoseconds_since_midnight,
    std::span<const byte> null_bitmap_lsb0,
    std::int32_t* output_days, u64 output_day_bytes,
    u64* output_times, u64 output_time_bytes,
    byte* output_null_bitmap_lsb0, u64 output_bitmap_bytes,
    const TimestampExecutionControlV3& control = {}) noexcept;
TimestampBatchResultV3 MaterializeTimestampBatchV3(
    std::shared_ptr<const TimestampValidatedProfileHandleV3> profile,
    std::span<const std::int32_t> civil_days,
    std::span<const u64> nanoseconds_since_midnight,
    std::span<const byte> null_bitmap_lsb0,
    const TimestampExecutionControlV3& control = {}) noexcept;

TimestampViewResultV3 DecodeTimestampSbdvalComposedNoAllocV3(
    const TimestampValidatedProfileHandleV3& profile, bool null_allowed,
    std::span<const byte> encoded,
    const TimestampExecutionControlV3& control = {}) noexcept;
TimestampBytesResultV3 EncodeTimestampSbdvalComposedV3(
    const TimestampOwnedValueV3& value, bool null_allowed,
    const TimestampExecutionControlV3& control = {}) noexcept;
TimestampViewResultV3 DecodeTimestampSbdpvComposedNoAllocV3(
    const TimestampValidatedProfileHandleV3& profile, bool null_allowed,
    std::span<const byte> encoded,
    const TimestampExecutionControlV3& control = {}) noexcept;
TimestampBytesResultV3 EncodeTimestampSbdpvComposedV3(
    const TimestampOwnedValueV3& value, bool null_allowed,
    const TimestampExecutionControlV3& control = {}) noexcept;

DiagnosticRecord MakeTimestampDiagnosticV3(Status status, std::string diagnostic_code,
    std::string message_key, std::string detail = {});

#undef SB_TIMESTAMP_RESULT

}  // namespace scratchbird::core::datatypes

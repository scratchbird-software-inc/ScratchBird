// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once

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

inline constexpr u64 kTimeMaximumNanosecondsV3 = 86'399'999'999'999ull;
inline constexpr u32 kTimeProfileMaterialBytesV3 = 592;
inline constexpr u32 kTimeComparisonMaterialBytesV3 = 352;
inline constexpr u32 kTimeComponentBytesV3 = 8;
inline constexpr u32 kTimeNullSortKeyBytesV3 = 100;
inline constexpr u32 kTimeValueSortKeyBytesV3 = 108;
inline constexpr u32 kTimeHashBytesV3 = 32;
inline constexpr u32 kTimeClosedCastPolicyRowsV3 = 221;
inline constexpr u32 kTimeClosedCastDecisionsV3 = 663;
inline constexpr u32 kTimeIntrinsicOperationRowsV3 = 21;

struct TimeAuthorityReceiptV3 {
  platform::Uuid statement_receipt_uuid;
  platform::Uuid catalog_snapshot_uuid;
  u64 catalog_generation = 0;
  u64 registry_generation = 0;
};

struct TimeValidatedProfileHandleV3 {
  TimeAuthorityReceiptV3 receipt;
  DatatypeTypeCodecIdentityRowV3 identity;
  DatatypePolicyIdentityV3 render_policy;
  DatatypePolicyIdentityV3 cast_policy;
  DatatypePolicyIdentityV3 civil_day_policy;
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
  std::array<byte, kTimeProfileMaterialBytesV3> profile_material{};
  std::array<byte, kTimeComparisonMaterialBytesV3> comparison_material{};
  std::array<byte, 32> profile_fingerprint{};
  std::array<byte, 32> comparison_fingerprint{};
};

enum class TimeValueStateV3 : u8 { value = 0, sql_null = 1 };
enum class TimeUnsignedCarrierKindV3 : u8 {
  unsigned_u64 = 0,
  wrong_host_type = 1,
  below_zero = 2,
  above_u64 = 3,
};
enum class TimeI64CarrierKindV3 : u8 {
  signed_i64 = 0,
  wrong_host_type = 1,
  below_i64 = 2,
  above_i64 = 3,
};
enum class TimeTextCarrierKindV3 : u8 { utf8_bytes = 0, wrong_host_type = 1 };

struct TimeNullableUnsignedFactV3 {
  TimeUnsignedCarrierKindV3 carrier = TimeUnsignedCarrierKindV3::unsigned_u64;
  TimeValueStateV3 state = TimeValueStateV3::value;
  u64 value = 0;
};

struct TimeNullableI64FactV3 {
  TimeI64CarrierKindV3 carrier = TimeI64CarrierKindV3::signed_i64;
  TimeValueStateV3 state = TimeValueStateV3::value;
  std::int64_t value = 0;
};

struct TimeTextOperandV3 {
  const DatatypeTypeCodecIdentityRowV3* identity = nullptr;
  const scratchbird::engine::ExecutionTypeDescriptor* descriptor = nullptr;
  TimeTextCarrierKindV3 carrier = TimeTextCarrierKindV3::utf8_bytes;
  TimeValueStateV3 state = TimeValueStateV3::value;
  std::string_view bytes;
  u64 extent = 0;
};

struct TimeValueViewV3 {
  const TimeValidatedProfileHandleV3* profile = nullptr;
  TimeValueStateV3 state = TimeValueStateV3::value;
  u64 nanoseconds_since_midnight = 0;
};

struct TimeOwnedValueV3 {
  std::shared_ptr<const TimeValidatedProfileHandleV3> profile;
  TimeValueStateV3 state = TimeValueStateV3::value;
  u64 nanoseconds_since_midnight = 0;
  TimeValueViewV3 view() const noexcept {
    return {profile.get(), state, nanoseconds_since_midnight};
  }
};

struct TimeBatchViewV3 {
  const TimeValidatedProfileHandleV3* profile = nullptr;
  std::span<const u64> values;
  std::span<const byte> null_bitmap_lsb0;
};

struct TimeOwnedBatchV3 {
  std::shared_ptr<const TimeValidatedProfileHandleV3> profile;
  std::vector<u64> values;
  std::vector<byte> null_bitmap_lsb0;
  TimeBatchViewV3 view() const noexcept {
    return {profile.get(), values, null_bitmap_lsb0};
  }
};

enum class TimeScrubClassV3 : u8 {
  sha_state, sha_schedule, sha_tail,
  profile_material, comparison_material, profile_digest, comparison_digest,
  component_decode_reencode, component_encode_staging,
  component_owned_buffer,
  render_staging, render_owned_buffer,
  hash_preimage, hash_digest, hash_owned_buffer,
  ordered_key_staging, ordered_key_owned_buffer, ordered_key_decode_reencode,
  character_render_staging, batch_values_staging, batch_bitmap_staging,
  sbdval_reencode, sbdval_component_staging,
  sbdpv_reencode, sbdpv_component_staging,
  covering_decode_reencode, zone_map_decode_reencode,
  statistics_decode_reencode, statistics_prior_hash,
  backup_decode_reencode, projection_owned_buffer,
  count,
};

struct TimeExecutionControlV3 {
  u64 maximum_allocation_bytes = ~u64{0};
  bool (*cancelled)(void*) noexcept = nullptr;
  void* cancellation_context = nullptr;
  void (*observe_scrubbed)(void*, TimeScrubClassV3, const byte*, u64) noexcept = nullptr;
  void* scrub_observer_context = nullptr;
  bool force_reencode_mismatch_for_conformance = false;
};

enum class TimeDiagnosticParameterKindV3 : u8 {
  none = 0,
  unsigned_u64 = 1,
  signed_i64 = 2,
  uuid = 3,
  token = 4,
};

struct TimeDiagnosticParameterV3 {
  TimeDiagnosticParameterKindV3 kind = TimeDiagnosticParameterKindV3::none;
  std::string_view name;
  u64 unsigned_value = 0;
  std::int64_t signed_value = 0;
  platform::Uuid uuid_value;
  std::string_view token_value;
};

struct TimeDiagnosticFactV3 {
  Status status;
  std::string_view diagnostic_code;
  std::string_view detail;
  std::array<TimeDiagnosticParameterV3, 4> parameters{};
  u8 parameter_count = 0;
};

struct TimeProfileResultV3 {
  Status status;
  TimeDiagnosticFactV3 diagnostic;
  TimeValidatedProfileHandleV3 profile;
  bool ok() const noexcept { return status.ok(); }
};
struct TimeValidationResultV3 {
  Status status;
  TimeDiagnosticFactV3 diagnostic;
  bool ok() const noexcept { return status.ok(); }
};
struct TimeViewResultV3 {
  Status status;
  TimeDiagnosticFactV3 diagnostic;
  TimeValueViewV3 value;
  bool ok() const noexcept { return status.ok(); }
};
struct TimeValueResultV3 {
  Status status;
  TimeDiagnosticFactV3 diagnostic;
  TimeOwnedValueV3 value;
  bool ok() const noexcept { return status.ok(); }
};
struct TimeBytesResultV3 {
  Status status;
  TimeDiagnosticFactV3 diagnostic;
  std::vector<byte> bytes;
  bool ok() const noexcept { return status.ok(); }
};
struct TimeNoAllocWriteResultV3 {
  Status status;
  TimeDiagnosticFactV3 diagnostic;
  u64 bytes_required = 0;
  u64 bytes_written = 0;
  bool containing_null = false;
  bool ok() const noexcept { return status.ok(); }
};
struct TimeTextResultV3 {
  Status status;
  TimeDiagnosticFactV3 diagnostic;
  std::string text;
  bool containing_null = false;
  bool ok() const noexcept { return status.ok(); }
};
struct TimeBatchExtentsResultV3 {
  Status status;
  TimeDiagnosticFactV3 diagnostic;
  u64 values_bytes = 0;
  u64 bitmap_bytes = 0;
  u64 combined_bytes = 0;
  bool ok() const noexcept { return status.ok(); }
};
struct TimeBatchResultV3 {
  Status status;
  TimeDiagnosticFactV3 diagnostic;
  TimeOwnedBatchV3 batch;
  bool ok() const noexcept { return status.ok(); }
};

TimeProfileResultV3 BuildCurrentTimeValidatedProfileHandleV3(
    const platform::Uuid& statement_receipt_uuid) noexcept;
TimeProfileResultV3 BuildTimeValidatedProfileHandleV3(
    const TimeAuthorityReceiptV3& receipt,
    const DatatypeTypeCodecIdentityRowV3& identity) noexcept;
TimeValidationResultV3 ValidateTimeProfileHandleV3(
    const TimeValidatedProfileHandleV3& profile,
    const TimeExecutionControlV3& control = {}) noexcept;
TimeViewResultV3 ValidateTimeValueViewV3(const TimeValueViewV3& value,
                                        bool null_allowed = true) noexcept;
TimeViewResultV3 DecodeCanonicalTimeComponentNoAllocV3(
    const TimeValidatedProfileHandleV3& profile, TimeValueStateV3 state,
    bool null_allowed, std::span<const byte> component,
    const TimeExecutionControlV3& control = {}) noexcept;
TimeBytesResultV3 EncodeCanonicalTimeComponentV3(
    const TimeOwnedValueV3& value,
    const TimeExecutionControlV3& control = {}) noexcept;
TimeNoAllocWriteResultV3 EncodeCanonicalTimeComponentIntoNoAllocV3(
    const TimeOwnedValueV3& value, byte* output, u64 output_capacity,
    const TimeExecutionControlV3& control = {}) noexcept;

struct TimeCivilV3 {
  u8 hour = 0;
  u8 minute = 0;
  u8 second = 0;
  u32 nanosecond = 0;
};
struct TimeCivilResultV3 {
  Status status;
  TimeDiagnosticFactV3 diagnostic;
  bool is_null = false;
  TimeCivilV3 civil;
  bool ok() const noexcept { return status.ok(); }
};
struct TimeScalarResultV3 {
  Status status;
  TimeDiagnosticFactV3 diagnostic;
  bool is_null = false;
  std::int64_t signed_value = 0;
  u64 unsigned_value = 0;
  bool ok() const noexcept { return status.ok(); }
};

TimeValueResultV3 ConstructTimeFromCivilV3(
    const std::shared_ptr<const TimeValidatedProfileHandleV3>& profile,
    u64 hour, u64 minute, u64 second, u64 nanosecond,
    bool null_allowed = true,
    const TimeExecutionControlV3& control = {}) noexcept;
TimeValueResultV3 ConstructTimeFromCivilV3(
    const std::shared_ptr<const TimeValidatedProfileHandleV3>& profile,
    const TimeNullableUnsignedFactV3& hour,
    const TimeNullableUnsignedFactV3& minute,
    const TimeNullableUnsignedFactV3& second,
    const TimeNullableUnsignedFactV3& nanosecond,
    bool null_allowed = true,
    const TimeExecutionControlV3& control = {}) noexcept;
TimeCivilResultV3 DecomposeTimeCivilV3(const TimeValueViewV3& value,
                                      bool null_allowed = true,
                                      const TimeExecutionControlV3& control = {}) noexcept;
TimeValueResultV3 ParseCanonicalTimeV3(
    const std::shared_ptr<const TimeValidatedProfileHandleV3>& profile,
    std::string_view text, bool null_allowed = true,
    const TimeExecutionControlV3& control = {}) noexcept;
TimeValueResultV3 ParseCanonicalTimeOperandV3(
    const std::shared_ptr<const TimeValidatedProfileHandleV3>& profile,
    const TimeTextOperandV3& operand, bool null_allowed = true,
    const TimeExecutionControlV3& control = {}) noexcept;
TimeTextResultV3 RenderCanonicalTimeV3(
    const TimeOwnedValueV3& value, bool export_literal = false,
    const TimeExecutionControlV3& control = {}) noexcept;
TimeNoAllocWriteResultV3 RenderCanonicalTimeIntoNoAllocV3(
    const TimeOwnedValueV3& value, bool export_literal, char* output,
    u64 output_capacity,
    const TimeExecutionControlV3& control = {}) noexcept;
TimeValueResultV3 ValidateCanonicalTimeV3(
    const TimeOwnedValueV3& value, bool null_allowed = true,
    const TimeExecutionControlV3& control = {}) noexcept;
TimeValueResultV3 TruncateTimeNanosecondV3(
    const TimeOwnedValueV3& value, bool null_allowed = true,
    const TimeExecutionControlV3& control = {}) noexcept;
TimeValueResultV3 RoundTimeNanosecondV3(
    const TimeOwnedValueV3& value, bool null_allowed = true,
    const TimeExecutionControlV3& control = {}) noexcept;

enum class TimeIntrinsicOperationV3 : u8 {
  validate_canonicalize,
  civil_construct,
  decompose_civil,
  parse_canonical,
  render_canonical,
  add_nanoseconds,
  subtract_nanoseconds,
  successor,
  predecessor,
  difference_nanoseconds,
  extract_hour,
  extract_minute,
  extract_second,
  extract_nanosecond,
  nanosecond_of_day,
  truncate_nanosecond,
  round_nanosecond,
  larger_truncate_round_or_bucket,
  duration_interval_or_modulo_day,
  date_timestamp_or_timezone_cross_temporal,
  aggregate_min_max_count_dispatch,
};
enum class TimeIntrinsicDispositionV3 : u8 {
  admitted,
  registered_refused,
  receiving_owner,
  unknown,
};
TimeIntrinsicDispositionV3 ClassifyTimeIntrinsicOperationV3(
    TimeIntrinsicOperationV3 operation) noexcept;
TimeValueResultV3 RefuseTimeIntrinsicOperationV3(
    const TimeValueViewV3& operand, TimeIntrinsicOperationV3 operation) noexcept;
TimeValueResultV3 AddTimeNanosecondsV3(
    const TimeOwnedValueV3& value, std::int64_t delta,
    bool null_allowed = true,
    const TimeExecutionControlV3& control = {}) noexcept;
TimeValueResultV3 AddTimeNanosecondsV3(
    const TimeOwnedValueV3& value, const TimeNullableI64FactV3& delta,
    bool null_allowed = true,
    const TimeExecutionControlV3& control = {}) noexcept;
TimeValueResultV3 SubtractTimeNanosecondsV3(
    const TimeOwnedValueV3& value, std::int64_t delta,
    bool null_allowed = true,
    const TimeExecutionControlV3& control = {}) noexcept;
TimeValueResultV3 SubtractTimeNanosecondsV3(
    const TimeOwnedValueV3& value, const TimeNullableI64FactV3& delta,
    bool null_allowed = true,
    const TimeExecutionControlV3& control = {}) noexcept;
TimeValueResultV3 TimeSuccessorV3(
    const TimeOwnedValueV3& value, bool null_allowed = true,
    const TimeExecutionControlV3& control = {}) noexcept;
TimeValueResultV3 TimePredecessorV3(
    const TimeOwnedValueV3& value, bool null_allowed = true,
    const TimeExecutionControlV3& control = {}) noexcept;
TimeScalarResultV3 DifferenceTimeNanosecondsV3(
    const TimeValueViewV3& left, const TimeValueViewV3& right,
    bool null_allowed = true,
    const TimeExecutionControlV3& control = {}) noexcept;
TimeScalarResultV3 ExtractTimeHourV3(const TimeValueViewV3& value,
                                    bool null_allowed = true,
                                    const TimeExecutionControlV3& control = {}) noexcept;
TimeScalarResultV3 ExtractTimeMinuteV3(const TimeValueViewV3& value,
                                      bool null_allowed = true,
                                      const TimeExecutionControlV3& control = {}) noexcept;
TimeScalarResultV3 ExtractTimeSecondV3(const TimeValueViewV3& value,
                                      bool null_allowed = true,
                                      const TimeExecutionControlV3& control = {}) noexcept;
TimeScalarResultV3 ExtractTimeNanosecondV3(const TimeValueViewV3& value,
                                          bool null_allowed = true,
                                          const TimeExecutionControlV3& control = {}) noexcept;
TimeScalarResultV3 TimeNanosecondOfDayV3(const TimeValueViewV3& value,
                                        bool null_allowed = true,
                                        const TimeExecutionControlV3& control = {}) noexcept;

struct TimeAggregateHandoffResultV3 {
  Status status;
  TimeDiagnosticFactV3 diagnostic;
  TimeOwnedValueV3 operand;
  bool ok() const noexcept { return status.ok(); }
};
TimeAggregateHandoffResultV3 ValidateTimeAggregateHandoffV3(
    const TimeOwnedValueV3& operand, bool null_allowed = true,
    const TimeExecutionControlV3& control = {}) noexcept;

enum class TimeComparisonFactV3 : u8 { less, equal, greater, unordered_null };
struct TimeComparisonResultV3 {
  Status status;
  TimeDiagnosticFactV3 diagnostic;
  TimeComparisonFactV3 fact = TimeComparisonFactV3::equal;
  bool grouping_equivalent = false;
  bool null_equivalent = false;
  bool ok() const noexcept { return status.ok(); }
};
TimeComparisonResultV3 CompareTimeValuesV3(const TimeValueViewV3& left,
                                           const TimeValueViewV3& right,
                                           const TimeExecutionControlV3& control = {}) noexcept;
TimeComparisonResultV3 CompareTimeValuesWithValidatedCohortForConformanceV3(
    const TimeValueViewV3& left, const TimeValueViewV3& right,
    const std::array<byte, 32>& validated_right_comparison_fingerprint,
    const TimeExecutionControlV3& control = {}) noexcept;
TimeBytesResultV3 HashTimeValueV3(const TimeOwnedValueV3& value) noexcept;
TimeBytesResultV3 HashTimeValueV3(
    const TimeOwnedValueV3& value,
    const TimeExecutionControlV3& control) noexcept;
TimeNoAllocWriteResultV3 HashTimeValueIntoNoAllocV3(
    const TimeOwnedValueV3& value, byte* output, u64 output_capacity,
    const TimeExecutionControlV3& control = {}) noexcept;

enum class TimeSortDirectionV3 : u8 { ascending = 0, descending = 1 };
enum class TimeNullModeV3 : u8 { nulls_first = 0, nulls_last = 1 };
struct TimeSortKeyViewV3 {
  const TimeValidatedProfileHandleV3* profile = nullptr;
  TimeSortDirectionV3 direction = TimeSortDirectionV3::ascending;
  TimeNullModeV3 null_mode = TimeNullModeV3::nulls_first;
  TimeValueStateV3 state = TimeValueStateV3::value;
  u64 nanoseconds_since_midnight = 0;
};
struct TimeSortKeyViewResultV3 {
  Status status;
  TimeDiagnosticFactV3 diagnostic;
  TimeSortKeyViewV3 value;
  bool ok() const noexcept { return status.ok(); }
};
TimeBytesResultV3 MakeTimeSortKeyV3(
    const TimeOwnedValueV3& value, TimeSortDirectionV3 direction,
    TimeNullModeV3 null_mode,
    const TimeExecutionControlV3& control = {}) noexcept;
TimeNoAllocWriteResultV3 MakeTimeSortKeyIntoNoAllocV3(
    const TimeOwnedValueV3& value, TimeSortDirectionV3 direction,
    TimeNullModeV3 null_mode, byte* output, u64 output_capacity,
    const TimeExecutionControlV3& control = {}) noexcept;
TimeSortKeyViewResultV3 DecodeTimeSortKeyNoAllocV3(
    const TimeValidatedProfileHandleV3& expected_profile,
    std::span<const byte> encoded,
    const TimeExecutionControlV3& control = {}) noexcept;

enum class TimeCastPolicyDispositionV3 : u8 {
  contextual_null,
  identity,
  explicit_character_to_time,
  explicit_time_to_character,
  forbidden,
};
TimeCastPolicyDispositionV3 ClassifyTimeCastPolicyRowV3(
    u32 one_based_policy_row, DatatypeCastContext context) noexcept;
struct TimeCastRequestV3 {
  u32 one_based_policy_row = 0;
  const TimeOwnedValueV3* time_source = nullptr;
  const DatatypeOperationValue* scalar_source = nullptr;
  const DatatypeTypeCodecIdentityRowV3* scalar_source_identity = nullptr;
  const std::shared_ptr<const TimeValidatedProfileHandleV3>* time_target = nullptr;
  const scratchbird::engine::ExecutionTypeDescriptor* time_target_descriptor = nullptr;
  CanonicalTypeId scalar_target = CanonicalTypeId::unknown;
  scratchbird::engine::ExecutionTypeDescriptor scalar_target_descriptor;
  const DatatypeTypeCodecIdentityRowV3* scalar_target_identity = nullptr;
  DatatypeCastContext context = DatatypeCastContext::implicit;
  bool target_null_allowed = true;
  bool use_character_output_buffer = false;
  char* character_output = nullptr;
  u64 character_output_capacity = 0;
  TimeExecutionControlV3 control;
};
struct TimeCastResultV3 {
  Status status;
  TimeDiagnosticFactV3 diagnostic;
  DatatypeCastCategory category = DatatypeCastCategory::forbidden;
  bool produced_time = false;
  bool used_character_output_buffer = false;
  u64 bytes_required = 0;
  u64 bytes_written = 0;
  TimeOwnedValueV3 time_value;
  DatatypeOperationValue scalar_value;
  bool ok() const noexcept { return status.ok(); }
};
TimeCastResultV3 CastTimeValueV3(const TimeCastRequestV3& request) noexcept;

TimeValidationResultV3 ValidateTimeBatchViewV3(
    const TimeBatchViewV3& batch) noexcept;
TimeBatchExtentsResultV3 ComputeTimeBatchExtentsV3(u64 row_count,
                                                   u64 size_limit) noexcept;
TimeBatchExtentsResultV3 MaterializeTimeBatchIntoV3(
    const std::shared_ptr<const TimeValidatedProfileHandleV3>& profile,
    std::span<const u64> values, std::span<const byte> null_bitmap_lsb0,
    u64* output_values, u64 output_values_bytes,
    byte* output_null_bitmap_lsb0, u64 output_bitmap_bytes,
    const TimeExecutionControlV3& control = {}) noexcept;
TimeBatchResultV3 MaterializeTimeBatchV3(
    std::shared_ptr<const TimeValidatedProfileHandleV3> profile,
    std::span<const u64> values, std::span<const byte> null_bitmap_lsb0,
    const TimeExecutionControlV3& control = {}) noexcept;

TimeViewResultV3 DecodeTimeSbdvalComposedNoAllocV3(
    const TimeValidatedProfileHandleV3& profile, bool null_allowed,
    std::span<const byte> encoded,
    const TimeExecutionControlV3& control = {}) noexcept;
TimeBytesResultV3 EncodeTimeSbdvalComposedV3(
    const TimeOwnedValueV3& value, bool null_allowed,
    const TimeExecutionControlV3& control = {}) noexcept;
TimeViewResultV3 DecodeTimeSbdpvComposedNoAllocV3(
    const TimeValidatedProfileHandleV3& profile, bool null_allowed,
    std::span<const byte> encoded,
    const TimeExecutionControlV3& control = {}) noexcept;
TimeBytesResultV3 EncodeTimeSbdpvComposedV3(
    const TimeOwnedValueV3& value, bool null_allowed,
    const TimeExecutionControlV3& control = {}) noexcept;

DiagnosticRecord MakeTimeDiagnosticV3(Status status,
                                      std::string diagnostic_code,
                                      std::string message_key,
                                      std::string detail = {});

}  // namespace scratchbird::core::datatypes

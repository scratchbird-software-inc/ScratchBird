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

inline constexpr u32 kDateProfileMaterialBytesV3 = 584;
inline constexpr u32 kDateComparisonMaterialBytesV3 = 344;
inline constexpr u32 kDateComponentBytesV3 = 4;
inline constexpr u32 kDateNullSortKeyBytesV3 = 100;
inline constexpr u32 kDateValueSortKeyBytesV3 = 104;
inline constexpr u32 kDateHashBytesV3 = 32;
inline constexpr u32 kDateClosedCastPolicyRowsV3 = 221;

struct DateAuthorityReceiptV3 {
  platform::Uuid statement_receipt_uuid;
  platform::Uuid catalog_snapshot_uuid;
  u64 catalog_generation = 0;
  u64 registry_generation = 0;
};

struct DateValidatedProfileHandleV3 {
  DateAuthorityReceiptV3 receipt;
  DatatypeTypeCodecIdentityRowV3 identity;
  DatatypePolicyIdentityV3 render_policy;
  DatatypePolicyIdentityV3 cast_policy;
  DatatypePolicyIdentityV3 calendar_policy;
  DatatypePolicyIdentityV3 storage_epoch_policy;
  DatatypePolicyIdentityV3 timezone_none_policy;
  DatatypePolicyIdentityV3 leap_not_applicable_policy;
  DatatypePolicyIdentityV3 index_policy;
  DatatypePolicyIdentityV3 statistics_policy;
  DatatypePolicyIdentityV3 backup_transport_policy;
  DatatypePolicyIdentityV3 protection_policy;
  DatatypePolicyIdentityV3 component_adapter_policy;
  DatatypePolicyIdentityV3 diagnostic_policy;
  DatatypePolicyIdentityV3 metric_policy;
  std::array<byte, kDateProfileMaterialBytesV3> profile_material{};
  std::array<byte, kDateComparisonMaterialBytesV3> comparison_material{};
  std::array<byte, 32> profile_fingerprint{};
  std::array<byte, 32> comparison_fingerprint{};
};

enum class DateValueStateV3 : u8 { value = 0, sql_null = 1 };

// Exact host carrier used by intrinsic operands whose SQL state is independent
// of the signed-i64 payload.  The non-i64 forms are refusal probes for dynamic
// host adapters; they can never publish a value.
enum class DateI64CarrierKindV3 : u8 {
  signed_i64 = 0,
  wrong_host_type = 1,
  below_i64 = 2,
  above_i64 = 3,
};

struct DateNullableI64FactV3 {
  DateI64CarrierKindV3 carrier = DateI64CarrierKindV3::signed_i64;
  DateValueStateV3 state = DateValueStateV3::value;
  std::int64_t value = 0;
};

enum class DateTextCarrierKindV3 : u8 {
  utf8_bytes = 0,
  wrong_host_type = 1,
};

// Logical TextOperand from the admitted intrinsic contract.  The V3 identity
// carries the d709 text profile/codec authority; the execution descriptor is
// the independently validated live descriptor.  `extent` is supplied rather
// than inferred so a malformed dynamic carrier cannot be normalized by this
// API before the canonical gate observes it.
struct DateTextOperandV3 {
  const DatatypeTypeCodecIdentityRowV3* identity = nullptr;
  const scratchbird::engine::ExecutionTypeDescriptor* descriptor = nullptr;
  DateTextCarrierKindV3 carrier = DateTextCarrierKindV3::utf8_bytes;
  DateValueStateV3 state = DateValueStateV3::value;
  std::string_view bytes;
  u64 extent = 0;
};

struct DateValueViewV3 {
  const DateValidatedProfileHandleV3* profile = nullptr;
  DateValueStateV3 state = DateValueStateV3::value;
  std::int32_t day = 0;
};

struct DateOwnedValueV3 {
  std::shared_ptr<const DateValidatedProfileHandleV3> profile;
  DateValueStateV3 state = DateValueStateV3::value;
  std::int32_t day = 0;

  DateValueViewV3 view() const noexcept { return {profile.get(), state, day}; }
};

// Dynamic admission carrier for boundaries that have not yet established a
// host int32 value.  `day` is never narrowed until `carrier` and the signed
// int32 range have both been validated.
enum class DateDayCarrierKindV3 : u8 {
  signed_i32 = 0,
  wrong_host_type = 1,
  wrong_host_boolean = 2,
  wrong_host_string = 3,
  below_i32 = 4,
  above_i32 = 5,
};

struct DateOperandV3 {
  std::shared_ptr<const DateValidatedProfileHandleV3> profile;
  DateValueStateV3 state = DateValueStateV3::value;
  DateDayCarrierKindV3 carrier = DateDayCarrierKindV3::signed_i32;
  std::int64_t day = 0;
};

struct DateBatchViewV3 {
  const DateValidatedProfileHandleV3* profile = nullptr;
  std::span<const std::int32_t> days;
  std::span<const byte> null_bitmap_lsb0;
};

struct DateOwnedBatchV3 {
  std::shared_ptr<const DateValidatedProfileHandleV3> profile;
  std::vector<std::int32_t> days;
  std::vector<byte> null_bitmap_lsb0;

  DateBatchViewV3 view() const noexcept {
    return {profile.get(), days, null_bitmap_lsb0};
  }
};

enum class DateScrubClassV3 : u8 {
  sha_state,
  sha_schedule,
  sha_tail,
  profile_material,
  comparison_material,
  profile_digest,
  comparison_digest,
  component_decode_reencode,
  component_encode_staging,
  component_owned_buffer,
  render_staging,
  render_owned_buffer,
  hash_preimage,
  hash_digest,
  hash_owned_buffer,
  ordered_key_staging,
  ordered_key_owned_buffer,
  ordered_key_decode_reencode,
  character_render_staging,
  batch_days_staging,
  batch_bitmap_staging,
  sbdval_reencode,
  sbdval_component_staging,
  sbdpv_reencode,
  sbdpv_component_staging,
  covering_decode_reencode,
  zone_map_decode_reencode,
  statistics_decode_reencode,
  statistics_prior_hash,
  backup_decode_reencode,
  projection_owned_buffer,
  count,
};

struct DateExecutionControlV3 {
  u64 maximum_allocation_bytes = ~u64{0};
  bool (*cancelled)(void*) noexcept = nullptr;
  void* cancellation_context = nullptr;
  void (*observe_scrubbed)(void*, DateScrubClassV3, const byte*, u64) noexcept = nullptr;
  void* scrub_observer_context = nullptr;
  bool force_reencode_mismatch_for_conformance = false;
};

struct DateDiagnosticFactV3 {
  Status status;
  std::string_view diagnostic_code;
  std::string_view detail;
};

struct DateProfileResultV3 {
  Status status;
  DateDiagnosticFactV3 diagnostic;
  DateValidatedProfileHandleV3 profile;
  bool ok() const noexcept { return status.ok(); }
};

struct DateValidationResultV3 {
  Status status;
  DateDiagnosticFactV3 diagnostic;
  bool ok() const noexcept { return status.ok(); }
};

struct DateViewResultV3 {
  Status status;
  DateDiagnosticFactV3 diagnostic;
  DateValueViewV3 value;
  bool ok() const noexcept { return status.ok(); }
};

struct DateValueResultV3 {
  Status status;
  DateDiagnosticFactV3 diagnostic;
  DateOwnedValueV3 value;
  bool ok() const noexcept { return status.ok(); }
};

struct DateBytesResultV3 {
  Status status;
  DateDiagnosticFactV3 diagnostic;
  std::vector<byte> bytes;
  bool ok() const noexcept { return status.ok(); }
};

struct DateNoAllocWriteResultV3 {
  Status status;
  DateDiagnosticFactV3 diagnostic;
  u64 bytes_required = 0;
  u64 bytes_written = 0;
  bool containing_null = false;
  bool ok() const noexcept { return status.ok(); }
};

struct DateTextResultV3 {
  Status status;
  DateDiagnosticFactV3 diagnostic;
  std::string text;
  bool containing_null = false;
  bool ok() const noexcept { return status.ok(); }
};

struct DateBatchExtentsResultV3 {
  Status status;
  DateDiagnosticFactV3 diagnostic;
  u64 days_bytes = 0;
  u64 bitmap_bytes = 0;
  u64 combined_bytes = 0;
  bool ok() const noexcept { return status.ok(); }
};

struct DateBatchResultV3 {
  Status status;
  DateDiagnosticFactV3 diagnostic;
  DateOwnedBatchV3 batch;
  bool ok() const noexcept { return status.ok(); }
};

DateProfileResultV3 BuildCurrentDateValidatedProfileHandleV3(
    const platform::Uuid& statement_receipt_uuid) noexcept;
DateProfileResultV3 BuildDateValidatedProfileHandleV3(
    const DateAuthorityReceiptV3& receipt,
    const DatatypeTypeCodecIdentityRowV3& identity) noexcept;
DateValidationResultV3 ValidateDateProfileHandleV3(
    const DateValidatedProfileHandleV3& profile,
    const DateExecutionControlV3& control = {}) noexcept;
DateViewResultV3 ValidateDateValueViewV3(const DateValueViewV3& value,
                                        bool null_allowed = true) noexcept;
DateViewResultV3 AdmitDateOperandV3(const DateOperandV3& operand,
                                   bool null_allowed = true) noexcept;
DateViewResultV3 DecodeCanonicalDateComponentNoAllocV3(
    const DateValidatedProfileHandleV3& profile, DateValueStateV3 state,
    bool null_allowed, std::span<const byte> component,
    const DateExecutionControlV3& control = {}) noexcept;
DateBytesResultV3 EncodeCanonicalDateComponentV3(
    const DateOwnedValueV3& value,
    const DateExecutionControlV3& control = {}) noexcept;
DateNoAllocWriteResultV3 EncodeCanonicalDateComponentIntoNoAllocV3(
    const DateOwnedValueV3& value, byte* output, u64 output_capacity,
    const DateExecutionControlV3& control = {}) noexcept;

struct DateCivilV3 {
  std::int32_t year = 0;
  u8 month = 1;
  u8 day = 1;
};

struct DateCivilResultV3 {
  Status status;
  DateDiagnosticFactV3 diagnostic;
  bool is_null = false;
  DateCivilV3 civil;
  bool ok() const noexcept { return status.ok(); }
};

struct DateScalarResultV3 {
  Status status;
  DateDiagnosticFactV3 diagnostic;
  bool is_null = false;
  std::int64_t signed_value = 0;
  bool boolean_value = false;
  bool ok() const noexcept { return status.ok(); }
};

DateValueResultV3 ConstructDateFromCivilV3(
    const std::shared_ptr<const DateValidatedProfileHandleV3>& profile,
    std::int64_t year,
    std::int64_t month, std::int64_t day, bool null_allowed = true) noexcept;
DateValueResultV3 ConstructDateFromCivilV3(
    const std::shared_ptr<const DateValidatedProfileHandleV3>& profile,
    const DateNullableI64FactV3& year,
    const DateNullableI64FactV3& month,
    const DateNullableI64FactV3& day,
    bool null_allowed = true) noexcept;
DateCivilResultV3 DecomposeDateCivilV3(const DateValueViewV3& value,
                                      bool null_allowed = true) noexcept;
DateValueResultV3 ParseCanonicalDateV3(
    const std::shared_ptr<const DateValidatedProfileHandleV3>& profile,
    std::string_view text,
    bool null_allowed = true) noexcept;
DateValueResultV3 ParseCanonicalDateOperandV3(
    const std::shared_ptr<const DateValidatedProfileHandleV3>& profile,
    const DateTextOperandV3& operand,
    bool null_allowed = true) noexcept;
DateTextResultV3 RenderCanonicalDateV3(
    const DateOwnedValueV3& value, bool export_literal = false,
    const DateExecutionControlV3& control = {}) noexcept;
DateNoAllocWriteResultV3 RenderCanonicalDateIntoNoAllocV3(
    const DateOwnedValueV3& value, bool export_literal, char* output,
    u64 output_capacity,
    const DateExecutionControlV3& control = {}) noexcept;
DateValueResultV3 ValidateCanonicalDateV3(
    const DateOwnedValueV3& value, bool null_allowed = true) noexcept;
DateValueResultV3 TruncateDateDayV3(
    const DateOwnedValueV3& value, bool null_allowed = true) noexcept;
DateValueResultV3 RoundDateDayV3(
    const DateOwnedValueV3& value, bool null_allowed = true) noexcept;

enum class DateIntrinsicOperationV3 : u8 {
  validate_canonicalize,
  civil_construct,
  decompose_civil,
  parse_canonical,
  render_canonical,
  add_days,
  subtract_days,
  successor,
  predecessor,
  difference_days,
  is_leap_year,
  days_in_month,
  iso_weekday,
  day_of_year,
  quarter,
  iso_week,
  truncate_day,
  round_day,
  larger_truncate_round_or_bucket,
  calendar_interval_or_cross_temporal,
  aggregate_min_max_count_dispatch,
};
enum class DateIntrinsicDispositionV3 : u8 {
  admitted,
  registered_refused,
  receiving_owner,
  unknown,
};
DateIntrinsicDispositionV3 ClassifyDateIntrinsicOperationV3(
    DateIntrinsicOperationV3 operation) noexcept;
DateValueResultV3 RefuseDateIntrinsicOperationV3(
    const DateValueViewV3& operand, DateIntrinsicOperationV3 operation) noexcept;
DateValueResultV3 AddDateDaysV3(const DateOwnedValueV3& value,
                               std::int64_t delta,
                               bool null_allowed = true,
                               const DateExecutionControlV3& control = {}) noexcept;
DateValueResultV3 AddDateDaysV3(const DateOwnedValueV3& value,
                               const DateNullableI64FactV3& delta,
                               bool null_allowed = true,
                               const DateExecutionControlV3& control = {}) noexcept;
DateValueResultV3 SubtractDateDaysV3(const DateOwnedValueV3& value,
                                    std::int64_t delta,
                                    bool null_allowed = true,
                                    const DateExecutionControlV3& control = {}) noexcept;
DateValueResultV3 SubtractDateDaysV3(const DateOwnedValueV3& value,
                                    const DateNullableI64FactV3& delta,
                                    bool null_allowed = true,
                                    const DateExecutionControlV3& control = {}) noexcept;
DateValueResultV3 DateSuccessorV3(const DateOwnedValueV3& value,
                                 bool null_allowed = true,
                                 const DateExecutionControlV3& control = {}) noexcept;
DateValueResultV3 DatePredecessorV3(const DateOwnedValueV3& value,
                                   bool null_allowed = true,
                                   const DateExecutionControlV3& control = {}) noexcept;
DateScalarResultV3 DifferenceDateDaysV3(const DateValueViewV3& left,
                                       const DateValueViewV3& right,
                                       bool null_allowed = true) noexcept;
DateScalarResultV3 DateIsLeapYearV3(const DateValueViewV3& value,
                                   bool null_allowed = true) noexcept;
DateScalarResultV3 DateDaysInMonthV3(const DateValueViewV3& value,
                                    bool null_allowed = true) noexcept;
DateScalarResultV3 DateIsoWeekdayV3(const DateValueViewV3& value,
                                   bool null_allowed = true) noexcept;
DateScalarResultV3 DateDayOfYearV3(const DateValueViewV3& value,
                                  bool null_allowed = true) noexcept;
DateScalarResultV3 DateQuarterV3(const DateValueViewV3& value,
                                bool null_allowed = true) noexcept;

struct DateIsoWeekResultV3 {
  Status status;
  DateDiagnosticFactV3 diagnostic;
  bool is_null = false;
  std::int32_t iso_year = 0;
  u8 iso_week = 0;
  u8 iso_weekday = 0;
  bool ok() const noexcept { return status.ok(); }
};
DateIsoWeekResultV3 DateIsoWeekV3(const DateValueViewV3& value,
                                 bool null_allowed = true) noexcept;

enum class DateComparisonFactV3 : u8 {
  less,
  equal,
  greater,
  unordered_null,
};

struct DateComparisonResultV3 {
  Status status;
  DateDiagnosticFactV3 diagnostic;
  DateComparisonFactV3 fact = DateComparisonFactV3::equal;
  bool grouping_equivalent = false;
  bool null_equivalent = false;
  bool ok() const noexcept { return status.ok(); }
};

DateComparisonResultV3 CompareDateValuesV3(const DateValueViewV3& left,
                                           const DateValueViewV3& right) noexcept;
// Conformance-only seam for exercising a separately validated comparison
// cohort that cannot otherwise coexist with the sole admitted d709 profile.
DateComparisonResultV3 CompareDateValuesWithValidatedCohortForConformanceV3(
    const DateValueViewV3& left, const DateValueViewV3& right,
    const std::array<byte, 32>& validated_right_comparison_fingerprint) noexcept;
DateBytesResultV3 HashDateValueV3(const DateOwnedValueV3& value) noexcept;
DateBytesResultV3 HashDateValueV3(
    const DateOwnedValueV3& value,
    const DateExecutionControlV3& control) noexcept;
DateNoAllocWriteResultV3 HashDateValueIntoNoAllocV3(
    const DateOwnedValueV3& value, byte* output, u64 output_capacity,
    const DateExecutionControlV3& control = {}) noexcept;

enum class DateSortDirectionV3 : u8 { ascending = 0, descending = 1 };
enum class DateNullModeV3 : u8 { nulls_first = 0, nulls_last = 1 };

struct DateSortKeyViewV3 {
  const DateValidatedProfileHandleV3* profile = nullptr;
  DateSortDirectionV3 direction = DateSortDirectionV3::ascending;
  DateNullModeV3 null_mode = DateNullModeV3::nulls_first;
  DateValueStateV3 state = DateValueStateV3::value;
  std::int32_t day = 0;
};

struct DateSortKeyViewResultV3 {
  Status status;
  DateDiagnosticFactV3 diagnostic;
  DateSortKeyViewV3 value;
  bool ok() const noexcept { return status.ok(); }
};

DateBytesResultV3 MakeDateSortKeyV3(
    const DateOwnedValueV3& value, DateSortDirectionV3 direction,
    DateNullModeV3 null_mode,
    const DateExecutionControlV3& control = {}) noexcept;
DateNoAllocWriteResultV3 MakeDateSortKeyIntoNoAllocV3(
    const DateOwnedValueV3& value, DateSortDirectionV3 direction,
    DateNullModeV3 null_mode, byte* output, u64 output_capacity,
    const DateExecutionControlV3& control = {}) noexcept;
DateSortKeyViewResultV3 DecodeDateSortKeyNoAllocV3(
    const DateValidatedProfileHandleV3& expected_profile,
    std::span<const byte> encoded,
    const DateExecutionControlV3& control = {}) noexcept;

enum class DateCastPolicyDispositionV3 : u8 {
  contextual_null,
  identity,
  explicit_character_to_date,
  explicit_date_to_character,
  forbidden,
};
DateCastPolicyDispositionV3 ClassifyDateCastPolicyRowV3(
    u32 one_based_policy_row, DatatypeCastContext context) noexcept;

struct DateCastRequestV3 {
  u32 one_based_policy_row = 0;
  const DateOwnedValueV3* date_source = nullptr;
  // Boundary form used when the incoming host carrier has not yet proved an
  // int32 payload.  Date-source rows require exactly one of date_source and
  // dynamic_date_source.
  const DateOperandV3* dynamic_date_source = nullptr;
  const DatatypeOperationValue* scalar_source = nullptr;
  const DatatypeTypeCodecIdentityRowV3* scalar_source_identity = nullptr;
  const std::shared_ptr<const DateValidatedProfileHandleV3>* date_target = nullptr;
  // Required for every incoming row.  This proves the exact base.date target
  // shape, including its domain/security-policy facets.  Security visibility
  // authorization remains a receiving-owner precondition outside this API.
  const scratchbird::engine::ExecutionTypeDescriptor* date_target_descriptor = nullptr;
  CanonicalTypeId scalar_target = CanonicalTypeId::unknown;
  scratchbird::engine::ExecutionTypeDescriptor scalar_target_descriptor;
  const DatatypeTypeCodecIdentityRowV3* scalar_target_identity = nullptr;
  DatatypeCastContext context = DatatypeCastContext::implicit;
  bool target_null_allowed = true;
  // Optional canonical caller-storage path for date -> character.  The flag is
  // explicit because SQL NULL legitimately uses a null pointer and zero
  // capacity.  Convenience callers may leave it false and receive the owning
  // scalar_value form.
  bool use_character_output_buffer = false;
  char* character_output = nullptr;
  u64 character_output_capacity = 0;
  DateExecutionControlV3 control;
};

struct DateCastResultV3 {
  Status status;
  DateDiagnosticFactV3 diagnostic;
  DatatypeCastCategory category = DatatypeCastCategory::forbidden;
  bool produced_date = false;
  bool used_character_output_buffer = false;
  u64 bytes_required = 0;
  u64 bytes_written = 0;
  DateOwnedValueV3 date_value;
  DatatypeOperationValue scalar_value;
  bool ok() const noexcept { return status.ok(); }
};
DateCastResultV3 CastDateValueV3(const DateCastRequestV3& request) noexcept;

DateValidationResultV3 ValidateDateBatchViewV3(
    const DateBatchViewV3& batch) noexcept;
DateBatchExtentsResultV3 ComputeDateBatchExtentsV3(u64 row_count,
                                                   u64 size_limit) noexcept;
// Bounded caller-publication path.  The implementation may allocate private
// staging, but it never writes either caller span until every validation,
// resource, overlap, and cancellation gate has succeeded.
DateBatchExtentsResultV3 MaterializeDateBatchIntoV3(
    const std::shared_ptr<const DateValidatedProfileHandleV3>& profile,
    std::span<const std::int32_t> days,
    std::span<const byte> null_bitmap_lsb0,
    std::int32_t* output_days, u64 output_days_bytes,
    byte* output_null_bitmap_lsb0, u64 output_bitmap_bytes,
    const DateExecutionControlV3& control = {}) noexcept;
DateBatchResultV3 MaterializeDateBatchV3(
    std::shared_ptr<const DateValidatedProfileHandleV3> profile,
    std::span<const std::int32_t> days,
    std::span<const byte> null_bitmap_lsb0,
    const DateExecutionControlV3& control = {}) noexcept;

DateViewResultV3 DecodeDateSbdvalComposedNoAllocV3(
    const DateValidatedProfileHandleV3& profile, bool null_allowed,
    std::span<const byte> encoded,
    const DateExecutionControlV3& control = {}) noexcept;
DateBytesResultV3 EncodeDateSbdvalComposedV3(
    const DateOwnedValueV3& value, bool null_allowed,
    const DateExecutionControlV3& control = {}) noexcept;
DateViewResultV3 DecodeDateSbdpvComposedNoAllocV3(
    const DateValidatedProfileHandleV3& profile, bool null_allowed,
    std::span<const byte> encoded,
    const DateExecutionControlV3& control = {}) noexcept;
DateBytesResultV3 EncodeDateSbdpvComposedV3(
    const DateOwnedValueV3& value, bool null_allowed,
    const DateExecutionControlV3& control = {}) noexcept;

DiagnosticRecord MakeDateDiagnosticV3(Status status,
                                      std::string diagnostic_code,
                                      std::string message_key,
                                      std::string detail = {});

}  // namespace scratchbird::core::datatypes

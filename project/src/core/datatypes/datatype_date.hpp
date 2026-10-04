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

inline constexpr u32 kDateProfileMaterialBytesV1 = 584;
inline constexpr u32 kDateComparisonMaterialBytesV1 = 344;
inline constexpr u32 kDateComponentBytesV1 = 4;
inline constexpr u32 kDateNullSortKeyBytesV1 = 100;
inline constexpr u32 kDateValueSortKeyBytesV1 = 104;
inline constexpr u32 kDateHashBytesV1 = 32;
inline constexpr u32 kDateClosedCastPolicyRowsV1 = 221;

struct DateAuthorityReceiptV1 {
  platform::Uuid statement_receipt_uuid;
  platform::Uuid catalog_snapshot_uuid;
  u64 catalog_generation = 0;
  u64 registry_generation = 0;
};

struct DateValidatedProfileHandleV1 {
  DateAuthorityReceiptV1 receipt;
  DatatypeTypeCodecIdentityRowV3 identity;
  DatatypePolicyIdentityV1 render_policy;
  DatatypePolicyIdentityV1 cast_policy;
  DatatypePolicyIdentityV1 calendar_policy;
  DatatypePolicyIdentityV1 storage_epoch_policy;
  DatatypePolicyIdentityV1 timezone_none_policy;
  DatatypePolicyIdentityV1 leap_not_applicable_policy;
  DatatypePolicyIdentityV1 index_policy;
  DatatypePolicyIdentityV1 statistics_policy;
  DatatypePolicyIdentityV1 backup_transport_policy;
  DatatypePolicyIdentityV1 protection_policy;
  DatatypePolicyIdentityV1 component_adapter_policy;
  DatatypePolicyIdentityV1 diagnostic_policy;
  DatatypePolicyIdentityV1 metric_policy;
  std::array<byte, kDateProfileMaterialBytesV1> profile_material{};
  std::array<byte, kDateComparisonMaterialBytesV1> comparison_material{};
  std::array<byte, 32> profile_fingerprint{};
  std::array<byte, 32> comparison_fingerprint{};
};

enum class DateValueStateV1 : u8 { value = 0, sql_null = 1 };

// Exact host carrier used by intrinsic operands whose SQL state is independent
// of the signed-i64 payload.  The non-i64 forms are refusal probes for dynamic
// host adapters; they can never publish a value.
enum class DateI64CarrierKindV1 : u8 {
  signed_i64 = 0,
  wrong_host_type = 1,
  below_i64 = 2,
  above_i64 = 3,
};

struct DateNullableI64FactV1 {
  DateI64CarrierKindV1 carrier = DateI64CarrierKindV1::signed_i64;
  DateValueStateV1 state = DateValueStateV1::value;
  std::int64_t value = 0;
};

enum class DateTextCarrierKindV1 : u8 {
  utf8_bytes = 0,
  wrong_host_type = 1,
};

// Logical TextOperand from the admitted intrinsic contract.  The V3 identity
// carries the d707 text profile/codec authority; the execution descriptor is
// the independently validated live descriptor.  `extent` is supplied rather
// than inferred so a malformed dynamic carrier cannot be normalized by this
// API before the canonical gate observes it.
struct DateTextOperandV1 {
  const DatatypeTypeCodecIdentityRowV3* identity = nullptr;
  const scratchbird::engine::ExecutionTypeDescriptor* descriptor = nullptr;
  DateTextCarrierKindV1 carrier = DateTextCarrierKindV1::utf8_bytes;
  DateValueStateV1 state = DateValueStateV1::value;
  std::string_view bytes;
  u64 extent = 0;
};

struct DateValueViewV1 {
  const DateValidatedProfileHandleV1* profile = nullptr;
  DateValueStateV1 state = DateValueStateV1::value;
  std::int32_t day = 0;
};

struct DateOwnedValueV1 {
  std::shared_ptr<const DateValidatedProfileHandleV1> profile;
  DateValueStateV1 state = DateValueStateV1::value;
  std::int32_t day = 0;

  DateValueViewV1 view() const noexcept { return {profile.get(), state, day}; }
};

struct DateBatchViewV1 {
  const DateValidatedProfileHandleV1* profile = nullptr;
  std::span<const std::int32_t> days;
  std::span<const byte> null_bitmap_lsb0;
};

struct DateOwnedBatchV1 {
  std::shared_ptr<const DateValidatedProfileHandleV1> profile;
  std::vector<std::int32_t> days;
  std::vector<byte> null_bitmap_lsb0;

  DateBatchViewV1 view() const noexcept {
    return {profile.get(), days, null_bitmap_lsb0};
  }
};

struct DateExecutionControlV1 {
  u64 maximum_allocation_bytes = ~u64{0};
  bool (*cancelled)(void*) noexcept = nullptr;
  void* cancellation_context = nullptr;
};

struct DateDiagnosticFactV1 {
  Status status;
  std::string_view diagnostic_code;
  std::string_view detail;
};

struct DateProfileResultV1 {
  Status status;
  DateDiagnosticFactV1 diagnostic;
  DateValidatedProfileHandleV1 profile;
  bool ok() const noexcept { return status.ok(); }
};

struct DateValidationResultV1 {
  Status status;
  DateDiagnosticFactV1 diagnostic;
  bool ok() const noexcept { return status.ok(); }
};

struct DateViewResultV1 {
  Status status;
  DateDiagnosticFactV1 diagnostic;
  DateValueViewV1 value;
  bool ok() const noexcept { return status.ok(); }
};

struct DateValueResultV1 {
  Status status;
  DateDiagnosticFactV1 diagnostic;
  DateOwnedValueV1 value;
  bool ok() const noexcept { return status.ok(); }
};

struct DateBytesResultV1 {
  Status status;
  DateDiagnosticFactV1 diagnostic;
  std::vector<byte> bytes;
  bool ok() const noexcept { return status.ok(); }
};

struct DateNoAllocWriteResultV1 {
  Status status;
  DateDiagnosticFactV1 diagnostic;
  u64 bytes_required = 0;
  u64 bytes_written = 0;
  bool containing_null = false;
  bool ok() const noexcept { return status.ok(); }
};

struct DateTextResultV1 {
  Status status;
  DateDiagnosticFactV1 diagnostic;
  std::string text;
  bool containing_null = false;
  bool ok() const noexcept { return status.ok(); }
};

struct DateBatchExtentsResultV1 {
  Status status;
  DateDiagnosticFactV1 diagnostic;
  u64 days_bytes = 0;
  u64 bitmap_bytes = 0;
  u64 combined_bytes = 0;
  bool ok() const noexcept { return status.ok(); }
};

struct DateBatchResultV1 {
  Status status;
  DateDiagnosticFactV1 diagnostic;
  DateOwnedBatchV1 batch;
  bool ok() const noexcept { return status.ok(); }
};

DateProfileResultV1 BuildCurrentDateValidatedProfileHandleV1(
    const platform::Uuid& statement_receipt_uuid) noexcept;
DateProfileResultV1 BuildDateValidatedProfileHandleV1(
    const DateAuthorityReceiptV1& receipt,
    const DatatypeTypeCodecIdentityRowV3& identity) noexcept;
DateValidationResultV1 ValidateDateProfileHandleV1(
    const DateValidatedProfileHandleV1& profile) noexcept;
DateViewResultV1 ValidateDateValueViewV1(const DateValueViewV1& value,
                                        bool null_allowed = true) noexcept;
DateViewResultV1 DecodeCanonicalDateComponentNoAllocV1(
    const DateValidatedProfileHandleV1& profile, DateValueStateV1 state,
    bool null_allowed, std::span<const byte> component) noexcept;
DateBytesResultV1 EncodeCanonicalDateComponentV1(
    const DateOwnedValueV1& value,
    const DateExecutionControlV1& control = {}) noexcept;
DateNoAllocWriteResultV1 EncodeCanonicalDateComponentIntoNoAllocV1(
    const DateOwnedValueV1& value, byte* output, u64 output_capacity,
    const DateExecutionControlV1& control = {}) noexcept;

struct DateCivilV1 {
  std::int32_t year = 0;
  u8 month = 1;
  u8 day = 1;
};

struct DateCivilResultV1 {
  Status status;
  DateDiagnosticFactV1 diagnostic;
  bool is_null = false;
  DateCivilV1 civil;
  bool ok() const noexcept { return status.ok(); }
};

struct DateScalarResultV1 {
  Status status;
  DateDiagnosticFactV1 diagnostic;
  bool is_null = false;
  std::int64_t signed_value = 0;
  bool boolean_value = false;
  bool ok() const noexcept { return status.ok(); }
};

DateValueResultV1 ConstructDateFromCivilV1(
    const std::shared_ptr<const DateValidatedProfileHandleV1>& profile,
    std::int64_t year,
    std::int64_t month, std::int64_t day, bool null_allowed = true) noexcept;
DateValueResultV1 ConstructDateFromCivilV1(
    const std::shared_ptr<const DateValidatedProfileHandleV1>& profile,
    const DateNullableI64FactV1& year,
    const DateNullableI64FactV1& month,
    const DateNullableI64FactV1& day,
    bool null_allowed = true) noexcept;
DateCivilResultV1 DecomposeDateCivilV1(const DateValueViewV1& value,
                                      bool null_allowed = true) noexcept;
DateValueResultV1 ParseCanonicalDateV1(
    const std::shared_ptr<const DateValidatedProfileHandleV1>& profile,
    std::string_view text,
    bool null_allowed = true) noexcept;
DateValueResultV1 ParseCanonicalDateOperandV1(
    const std::shared_ptr<const DateValidatedProfileHandleV1>& profile,
    const DateTextOperandV1& operand,
    bool null_allowed = true) noexcept;
DateTextResultV1 RenderCanonicalDateV1(
    const DateOwnedValueV1& value, bool export_literal = false,
    const DateExecutionControlV1& control = {}) noexcept;
DateNoAllocWriteResultV1 RenderCanonicalDateIntoNoAllocV1(
    const DateOwnedValueV1& value, bool export_literal, char* output,
    u64 output_capacity,
    const DateExecutionControlV1& control = {}) noexcept;
DateValueResultV1 ValidateCanonicalDateV1(
    const DateOwnedValueV1& value, bool null_allowed = true) noexcept;
DateValueResultV1 TruncateDateDayV1(
    const DateOwnedValueV1& value, bool null_allowed = true) noexcept;
DateValueResultV1 RoundDateDayV1(
    const DateOwnedValueV1& value, bool null_allowed = true) noexcept;

enum class DateIntrinsicOperationV1 : u8 {
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
enum class DateIntrinsicDispositionV1 : u8 {
  admitted,
  registered_refused,
  receiving_owner,
  unknown,
};
DateIntrinsicDispositionV1 ClassifyDateIntrinsicOperationV1(
    DateIntrinsicOperationV1 operation) noexcept;
DateValueResultV1 RefuseDateIntrinsicOperationV1(
    const DateValueViewV1& operand, DateIntrinsicOperationV1 operation) noexcept;
DateValueResultV1 AddDateDaysV1(const DateOwnedValueV1& value,
                               std::int64_t delta,
                               bool null_allowed = true,
                               const DateExecutionControlV1& control = {}) noexcept;
DateValueResultV1 AddDateDaysV1(const DateOwnedValueV1& value,
                               const DateNullableI64FactV1& delta,
                               bool null_allowed = true,
                               const DateExecutionControlV1& control = {}) noexcept;
DateValueResultV1 SubtractDateDaysV1(const DateOwnedValueV1& value,
                                    std::int64_t delta,
                                    bool null_allowed = true,
                                    const DateExecutionControlV1& control = {}) noexcept;
DateValueResultV1 SubtractDateDaysV1(const DateOwnedValueV1& value,
                                    const DateNullableI64FactV1& delta,
                                    bool null_allowed = true,
                                    const DateExecutionControlV1& control = {}) noexcept;
DateValueResultV1 DateSuccessorV1(const DateOwnedValueV1& value,
                                 bool null_allowed = true,
                                 const DateExecutionControlV1& control = {}) noexcept;
DateValueResultV1 DatePredecessorV1(const DateOwnedValueV1& value,
                                   bool null_allowed = true,
                                   const DateExecutionControlV1& control = {}) noexcept;
DateScalarResultV1 DifferenceDateDaysV1(const DateValueViewV1& left,
                                       const DateValueViewV1& right,
                                       bool null_allowed = true) noexcept;
DateScalarResultV1 DateIsLeapYearV1(const DateValueViewV1& value,
                                   bool null_allowed = true) noexcept;
DateScalarResultV1 DateDaysInMonthV1(const DateValueViewV1& value,
                                    bool null_allowed = true) noexcept;
DateScalarResultV1 DateIsoWeekdayV1(const DateValueViewV1& value,
                                   bool null_allowed = true) noexcept;
DateScalarResultV1 DateDayOfYearV1(const DateValueViewV1& value,
                                  bool null_allowed = true) noexcept;
DateScalarResultV1 DateQuarterV1(const DateValueViewV1& value,
                                bool null_allowed = true) noexcept;

struct DateIsoWeekResultV1 {
  Status status;
  DateDiagnosticFactV1 diagnostic;
  bool is_null = false;
  std::int32_t iso_year = 0;
  u8 iso_week = 0;
  u8 iso_weekday = 0;
  bool ok() const noexcept { return status.ok(); }
};
DateIsoWeekResultV1 DateIsoWeekV1(const DateValueViewV1& value,
                                 bool null_allowed = true) noexcept;

enum class DateComparisonFactV1 : u8 {
  less,
  equal,
  greater,
  unordered_null,
};

struct DateComparisonResultV1 {
  Status status;
  DateDiagnosticFactV1 diagnostic;
  DateComparisonFactV1 fact = DateComparisonFactV1::equal;
  bool grouping_equivalent = false;
  bool null_equivalent = false;
  bool ok() const noexcept { return status.ok(); }
};

DateComparisonResultV1 CompareDateValuesV1(const DateValueViewV1& left,
                                           const DateValueViewV1& right) noexcept;
// Conformance-only seam for exercising a separately validated comparison
// cohort that cannot otherwise coexist with the sole admitted d707 profile.
DateComparisonResultV1 CompareDateValuesWithValidatedCohortForConformanceV1(
    const DateValueViewV1& left, const DateValueViewV1& right,
    const std::array<byte, 32>& validated_right_comparison_fingerprint) noexcept;
DateBytesResultV1 HashDateValueV1(const DateOwnedValueV1& value) noexcept;
DateBytesResultV1 HashDateValueV1(
    const DateOwnedValueV1& value,
    const DateExecutionControlV1& control) noexcept;
DateNoAllocWriteResultV1 HashDateValueIntoNoAllocV1(
    const DateOwnedValueV1& value, byte* output, u64 output_capacity,
    const DateExecutionControlV1& control = {}) noexcept;

enum class DateSortDirectionV1 : u8 { ascending = 0, descending = 1 };
enum class DateNullModeV1 : u8 { nulls_first = 0, nulls_last = 1 };

struct DateSortKeyViewV1 {
  const DateValidatedProfileHandleV1* profile = nullptr;
  DateSortDirectionV1 direction = DateSortDirectionV1::ascending;
  DateNullModeV1 null_mode = DateNullModeV1::nulls_first;
  DateValueStateV1 state = DateValueStateV1::value;
  std::int32_t day = 0;
};

struct DateSortKeyViewResultV1 {
  Status status;
  DateDiagnosticFactV1 diagnostic;
  DateSortKeyViewV1 value;
  bool ok() const noexcept { return status.ok(); }
};

DateBytesResultV1 MakeDateSortKeyV1(
    const DateOwnedValueV1& value, DateSortDirectionV1 direction,
    DateNullModeV1 null_mode,
    const DateExecutionControlV1& control = {}) noexcept;
DateNoAllocWriteResultV1 MakeDateSortKeyIntoNoAllocV1(
    const DateOwnedValueV1& value, DateSortDirectionV1 direction,
    DateNullModeV1 null_mode, byte* output, u64 output_capacity,
    const DateExecutionControlV1& control = {}) noexcept;
DateSortKeyViewResultV1 DecodeDateSortKeyNoAllocV1(
    const DateValidatedProfileHandleV1& expected_profile,
    std::span<const byte> encoded) noexcept;

enum class DateCastPolicyDispositionV1 : u8 {
  contextual_null,
  identity,
  explicit_character_to_date,
  explicit_date_to_character,
  forbidden,
};
DateCastPolicyDispositionV1 ClassifyDateCastPolicyRowV1(
    u32 one_based_policy_row, DatatypeCastContext context) noexcept;

struct DateCastRequestV1 {
  u32 one_based_policy_row = 0;
  const DateOwnedValueV1* date_source = nullptr;
  const DatatypeOperationValue* scalar_source = nullptr;
  const DatatypeTypeCodecIdentityRowV3* scalar_source_identity = nullptr;
  const std::shared_ptr<const DateValidatedProfileHandleV1>* date_target = nullptr;
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
  DateExecutionControlV1 control;
};

struct DateCastResultV1 {
  Status status;
  DateDiagnosticFactV1 diagnostic;
  DatatypeCastCategory category = DatatypeCastCategory::forbidden;
  bool produced_date = false;
  bool used_character_output_buffer = false;
  u64 bytes_required = 0;
  u64 bytes_written = 0;
  DateOwnedValueV1 date_value;
  DatatypeOperationValue scalar_value;
  bool ok() const noexcept { return status.ok(); }
};
DateCastResultV1 CastDateValueV1(const DateCastRequestV1& request) noexcept;

DateValidationResultV1 ValidateDateBatchViewV1(
    const DateBatchViewV1& batch) noexcept;
DateBatchExtentsResultV1 ComputeDateBatchExtentsV1(u64 row_count,
                                                   u64 size_limit) noexcept;
// Bounded caller-publication path.  The implementation may allocate private
// staging, but it never writes either caller span until every validation,
// resource, overlap, and cancellation gate has succeeded.
DateBatchExtentsResultV1 MaterializeDateBatchIntoV1(
    const std::shared_ptr<const DateValidatedProfileHandleV1>& profile,
    std::span<const std::int32_t> days,
    std::span<const byte> null_bitmap_lsb0,
    std::int32_t* output_days, u64 output_days_bytes,
    byte* output_null_bitmap_lsb0, u64 output_bitmap_bytes,
    const DateExecutionControlV1& control = {}) noexcept;
DateBatchResultV1 MaterializeDateBatchV1(
    std::shared_ptr<const DateValidatedProfileHandleV1> profile,
    std::span<const std::int32_t> days,
    std::span<const byte> null_bitmap_lsb0,
    const DateExecutionControlV1& control = {}) noexcept;

DateViewResultV1 DecodeDateSbdvalComposedNoAllocV1(
    const DateValidatedProfileHandleV1& profile, bool null_allowed,
    std::span<const byte> encoded,
    const DateExecutionControlV1& control = {}) noexcept;
DateBytesResultV1 EncodeDateSbdvalComposedV1(
    const DateOwnedValueV1& value, bool null_allowed,
    const DateExecutionControlV1& control = {}) noexcept;
DateViewResultV1 DecodeDateSbdpvComposedNoAllocV1(
    const DateValidatedProfileHandleV1& profile, bool null_allowed,
    std::span<const byte> encoded,
    const DateExecutionControlV1& control = {}) noexcept;
DateBytesResultV1 EncodeDateSbdpvComposedV1(
    const DateOwnedValueV1& value, bool null_allowed,
    const DateExecutionControlV1& control = {}) noexcept;

DiagnosticRecord MakeDateDiagnosticV1(Status status,
                                      std::string diagnostic_code,
                                      std::string message_key,
                                      std::string detail = {});

}  // namespace scratchbird::core::datatypes

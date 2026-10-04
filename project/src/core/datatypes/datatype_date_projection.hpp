// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "datatype_date.hpp"

#include <array>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace scratchbird::core::datatypes {

using platform::i32;

inline constexpr u32 kDateZoneMapEmptyBytesV3 = 96;
inline constexpr u32 kDateZoneMapValueBytesV3 = 304;
inline constexpr u32 kDateStatisticsHeaderBytesV3 = 512;
inline constexpr u32 kDateStatisticsMaximumBytesV3 = 4608;
inline constexpr u32 kDateBackupNullBytesV3 = 296;
inline constexpr u32 kDateBackupValueBytesV3 = 300;

enum class DateIndexFamilyV3 : std::uint8_t {
  aggregate_sketch, bitmap, brin_like, btree, columnar_zone_map,
  covering_included, document_path, expression, full_text, graph, hash,
  partial_filtered, range_exclusion, spatial, temporary_work, vector_ann,
};

enum class DateIndexDispositionV3 : std::uint8_t {
  admitted_with_recheck,
  admitted_exact_if_budget,
  admitted_payload_only,
  admitted_exact_selected_mode,
  conditional,
  not_applicable,
};

enum class DateProjectionKindV3 : std::uint8_t {
  none,
  equality_hash,
  sort_key,
  covering_value,
  zone_map,
  range_pair,
  conditional_result,
  conditional_underlying,
};

enum class DateProjectionModeV3 : std::uint8_t {
  unspecified,
  equality_hash,
  zone_map,
  ascending_nulls_first,
  ascending_nulls_last,
  descending_nulls_first,
  descending_nulls_last,
  payload,
  ascending_nulls_last_pair,
};

struct DateIndexResolutionV3 {
  DateIndexFamilyV3 family = DateIndexFamilyV3::btree;
  platform::Uuid compatibility_uuid{};
  u64 compatibility_generation = 0;
  DateIndexDispositionV3 disposition = DateIndexDispositionV3::not_applicable;
  DateProjectionKindV3 projection = DateProjectionKindV3::none;
  bool exact_recheck_required = false;
  bool receiving_owner_resolution_required = false;
};

struct DateIndexProjectionRequestV3 {
  DateIndexFamilyV3 family = DateIndexFamilyV3::btree;
  DateProjectionModeV3 mode = DateProjectionModeV3::unspecified;
  const DateOwnedValueV3* value = nullptr;
  const DateOwnedValueV3* upper_value = nullptr;
  const struct DateZoneMapRequestV3* zone_map = nullptr;
  const DateIndexResolutionV3* selected_conditional_family = nullptr;
  u64 provider_max_key_bytes = ~u64{0};
  DateExecutionControlV3 control;
};

struct DateIndexProjectionResultV3 {
  Status status;
  DateDiagnosticFactV3 diagnostic;
  DateIndexResolutionV3 resolution;
  std::vector<byte> bytes;
  bool owner_fact_only = false;
  bool ok() const noexcept { return status.ok(); }
};

DateIndexResolutionV3 ResolveDateIndexFamilyV3(
    DateIndexFamilyV3 family) noexcept;

enum class DateIndexPredicateOperationV3 : std::uint8_t {
  equality,
  in,
  is_null,
  range,
  order,
  prune_range,
  include_payload,
  overlap,
  exclusion,
  unsupported,
};
enum class DateIndexPredicateFactV3 : std::uint8_t {
  exact_no_recheck_after_decode_reencode,
  requires_exact_state_day_recheck_never_final_match,
  prune_only_mandatory_source_row_predicate_recheck_never_final_match,
  refused,
};
struct DateIndexPredicateResolutionV3 {
  bool admitted = false;
  DateIndexPredicateFactV3 fact = DateIndexPredicateFactV3::refused;
  std::string_view diagnostic;
};
DateIndexPredicateResolutionV3 ResolveDateIndexPredicateV3(
    DateIndexFamilyV3 family, DateIndexPredicateOperationV3 operation) noexcept;
DateIndexProjectionResultV3 ProjectDateIndexValueV3(
    const DateIndexProjectionRequestV3& request) noexcept;

struct DateCoveringValueViewV3 {
  const DateValidatedProfileHandleV3* profile_handle = nullptr;
  DateValueStateV3 state = DateValueStateV3::value;
  i32 day = 0;
};
struct DateCoveringValueViewResultV3 {
  Status status;
  DateDiagnosticFactV3 diagnostic;
  DateCoveringValueViewV3 value;
  bool ok() const noexcept { return status.ok(); }
};
DateBytesResultV3 EncodeDateCoveringValueV3(
    const DateOwnedValueV3& value,
    const DateExecutionControlV3& control = {}) noexcept;
DateCoveringValueViewResultV3 DecodeDateCoveringValueNoAllocV3(
    const DateValidatedProfileHandleV3& profile_handle, bool null_allowed,
    std::span<const byte> encoded,
    const DateExecutionControlV3& control = {}) noexcept;

struct DateRangePairViewV3 {
  i32 lower_day = 0;
  i32 upper_day = 0;
  std::span<const byte> lower_key;
  std::span<const byte> upper_key;
};
struct DateRangePairViewResultV3 {
  Status status;
  DateDiagnosticFactV3 diagnostic;
  DateRangePairViewV3 value;
  bool ok() const noexcept { return status.ok(); }
};
DateBytesResultV3 EncodeDateRangePairV3(
    const DateOwnedValueV3& lower, const DateOwnedValueV3& upper,
    const DateExecutionControlV3& control = {}) noexcept;
DateRangePairViewResultV3 DecodeDateRangePairNoAllocV3(
    const DateValidatedProfileHandleV3& profile_handle,
    std::span<const byte> encoded) noexcept;

struct DateZoneMapRequestV3 {
  std::shared_ptr<const DateValidatedProfileHandleV3> profile;
  const DateOwnedValueV3* minimum = nullptr;
  const DateOwnedValueV3* maximum = nullptr;
  u64 null_count = 0;
  u64 value_count = 0;
  u64 provider_max_key_bytes = ~u64{0};
  DateExecutionControlV3 control;
};
struct DateZoneMapViewV3 {
  const DateValidatedProfileHandleV3* profile_handle = nullptr;
  u64 null_count = 0;
  u64 value_count = 0;
  i32 minimum_day = 0;
  i32 maximum_day = 0;
  std::span<const byte> minimum_key;
  std::span<const byte> maximum_key;
};
struct DateZoneMapViewResultV3 {
  Status status;
  DateDiagnosticFactV3 diagnostic;
  DateZoneMapViewV3 value;
  bool ok() const noexcept { return status.ok(); }
};
DateBytesResultV3 EncodeDateZoneMapV3(
    const DateZoneMapRequestV3& request) noexcept;
DateZoneMapViewResultV3 DecodeDateZoneMapNoAllocV3(
    const DateValidatedProfileHandleV3& profile_handle,
    std::span<const byte> encoded,
    const DateExecutionControlV3& control = {}) noexcept;

struct DateStatisticsHistogramRecordV3 {
  i32 inclusive_upper_day = 0;
  u64 noncumulative_count = 0;
};
struct DateStatisticsMcvRecordV3 {
  std::array<byte, 32> value_hash{};
  u64 frequency = 0;
};
// Lifetime pin for the immutable receiving-owner evidence record.  The date
// datatype validates only the nonnil V7 identity shape; ownership does not
// confer provenance or authorization.
struct DateStatisticsProviderEvidenceHandleV3 {
  platform::Uuid provider_evidence_uuid{};
};
struct DateStatisticsProjectionV3 {
  std::shared_ptr<const DateValidatedProfileHandleV3> profile;
  std::shared_ptr<const DateStatisticsProviderEvidenceHandleV3>
      provider_evidence;
  platform::Uuid statistics_uuid{};
  platform::Uuid source_object_uuid{};
  u64 schema_epoch = 0;
  u64 collection_epoch = 0;
  u64 security_epoch = 0;
  u64 row_count = 0;
  u64 null_count = 0;
  u64 value_count = 0;
  bool minimum_maximum_present = false;
  i32 minimum_day = 0;
  i32 maximum_day = 0;
  std::span<const DateStatisticsHistogramRecordV3> histogram;
  std::span<const DateStatisticsMcvRecordV3> mcv;
};
struct DateDecodedStatisticsV3 {
  std::shared_ptr<const DateValidatedProfileHandleV3> profile;
  platform::Uuid statistics_uuid{};
  platform::Uuid source_object_uuid{};
  platform::Uuid provider_evidence_uuid{};
  u64 schema_epoch = 0;
  u64 collection_epoch = 0;
  u64 security_epoch = 0;
  u64 row_count = 0;
  u64 null_count = 0;
  u64 value_count = 0;
  bool minimum_maximum_present = false;
  i32 minimum_day = 0;
  i32 maximum_day = 0;
  std::vector<DateStatisticsHistogramRecordV3> histogram;
  std::vector<DateStatisticsMcvRecordV3> mcv;
};
struct DateStatisticsDecodeResultV3 {
  Status status;
  DateDiagnosticFactV3 diagnostic;
  DateDecodedStatisticsV3 statistics;
  bool ok() const noexcept { return status.ok(); }
};
DateBytesResultV3 EncodeDateStatisticsProjectionV3(
    const DateStatisticsProjectionV3& projection,
    const DateExecutionControlV3& control = {}) noexcept;
DateStatisticsDecodeResultV3 DecodeDateStatisticsProjectionV3(
    const std::shared_ptr<const DateValidatedProfileHandleV3>& profile,
    std::span<const byte> encoded,
    const DateExecutionControlV3& control = {}) noexcept;

enum class DateStatisticsReceivingDispositionV3 : std::uint8_t {
  admitted,
  security_denied,
  privacy_denied,
  schema_epoch_mismatch,
  collection_epoch_mismatch,
  security_epoch_mismatch,
  full_recollection_required,
  provider_evidence_mismatch,
  manual_review_required,
};
enum class DateStatisticsEvidenceClassV3 : std::uint8_t {
  full_visible_population,
  partial_scan,
  weighted_input,
  sampled_input,
  snapshot_merge,
  histogram_source_contradiction,
};
struct DateStatisticsReceivingFactsV3 {
  bool security_visible = false;
  bool privacy_admitted = false;
  u64 schema_epoch = 0;
  u64 collection_epoch = 0;
  u64 security_epoch = 0;
  platform::Uuid provider_evidence_uuid{};
};
DateStatisticsReceivingDispositionV3 ResolveDateStatisticsReceivingFactsV3(
    const DateDecodedStatisticsV3& decoded,
    const DateStatisticsReceivingFactsV3& current) noexcept;
DateStatisticsReceivingDispositionV3 ResolveDateStatisticsEvidenceV3(
    DateStatisticsEvidenceClassV3 evidence) noexcept;

struct DateBackupTupleViewV3 {
  const DateValidatedProfileHandleV3* profile_handle = nullptr;
  DateValueStateV3 state = DateValueStateV3::value;
  i32 day = 0;
};
struct DateBackupTupleViewResultV3 {
  Status status;
  DateDiagnosticFactV3 diagnostic;
  DateBackupTupleViewV3 tuple;
  bool ok() const noexcept { return status.ok(); }
};
DateBytesResultV3 EncodeDateBackupTupleV3(
    const DateOwnedValueV3& value,
    const DateExecutionControlV3& control = {}) noexcept;
DateBackupTupleViewResultV3 DecodeDateBackupTupleNoAllocV3(
    const DateValidatedProfileHandleV3& profile_handle, bool null_allowed,
    std::span<const byte> encoded,
    const DateExecutionControlV3& control = {}) noexcept;

enum class DateProtectionCellV3 : std::uint8_t {
  canonical_plain, inner_compression, inner_encryption, outer_compression,
  outer_authenticated_protection, outer_compress_then_protect,
  outer_protect_then_compress, page_or_filespace_crypto, transport_tls,
  protected_index,
};
struct DateProtectionResolutionV3 {
  DateProtectionCellV3 cell = DateProtectionCellV3::canonical_plain;
  bool datatype_admitted = false;
  bool receiving_owner_handoff = false;
  std::string_view disposition;
  std::string_view diagnostic;
};
DateProtectionResolutionV3 ResolveDateProtectionV3(
    DateProtectionCellV3 cell) noexcept;

enum class DateWireLaneV3 : std::uint8_t {
  native_sbwp, parser_server_ipc, canonical_sblr, apache_ignite, cassandra,
  clickhouse, cockroachdb, dolt, duckdb, firebird, foundationdb, immudb,
  influxdb, mariadb, milvus, mongodb, mysql, neo4j, opensearch, postgresql,
  redis, sqlite, tidb, tikv, vitess, xtdb, yugabytedb,
};
struct DateWireLaneResolutionV3 {
  DateWireLaneV3 lane = DateWireLaneV3::native_sbwp;
  bool admitted = false;
  bool component_mapping_complete = false;
  bool receiving_owner_handoff = true;
  std::string_view disposition;
  std::string_view diagnostic = "CTI.TRANSPORT.UNSUPPORTED";
};
DateWireLaneResolutionV3 ResolveDateWireLaneV3(DateWireLaneV3 lane) noexcept;

enum class DateDiagnosticAxisV3 : std::uint8_t {
  input_parse, descriptor_codec, storage_read_write, cast_invalid,
  cast_range_loss, operation_invalid, bounds_overflow_underflow,
  resource_cancellation, compression_corruption, encryption_auth_key,
  wire_decode_encode, index_key, domain_validation, recovery_corruption,
  statistics_read,
};
struct DateDiagnosticRouteV3 {
  DateDiagnosticAxisV3 axis = DateDiagnosticAxisV3::descriptor_codec;
  std::string_view code;
  platform::Uuid diagnostic_uuid{};
  std::string_view ordered_parameter_schema;
  std::string_view redaction;
};
std::span<const DateDiagnosticRouteV3> DateDiagnosticRoutesV3() noexcept;

enum class DateMetricEvidenceTypeV3 : std::uint8_t {
  descriptor_admissions, invalid_literals, range_refusals,
  operation_attempts, operation_success, operation_refusals, cast_attempts,
  cast_success, index_admission_refusals, statistics_stale,
  reference_mapping_misses, transport_refusals, serialization_refusals,
  protection_refusals, merge_manual_review, resource_budget_refusals,
  cancellations, decode_corruption, diagnostic_family,
};
struct DateMetricEvidenceTypeDescriptorV3 {
  DateMetricEvidenceTypeV3 type = DateMetricEvidenceTypeV3::descriptor_admissions;
  platform::Uuid evidence_type_uuid{};
  u64 generation = 0;
  std::string_view metric_name_metadata;
  std::string_view evidence_producer;
  std::string_view accepted_final_outcome;
};
std::span<const DateMetricEvidenceTypeDescriptorV3>
DateMetricEvidenceTypesV3() noexcept;

}  // namespace scratchbird::core::datatypes

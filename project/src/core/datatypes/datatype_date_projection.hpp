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

inline constexpr u32 kDateZoneMapEmptyBytesV1 = 96;
inline constexpr u32 kDateZoneMapValueBytesV1 = 304;
inline constexpr u32 kDateStatisticsHeaderBytesV1 = 512;
inline constexpr u32 kDateStatisticsMaximumBytesV1 = 4608;
inline constexpr u32 kDateBackupNullBytesV1 = 296;
inline constexpr u32 kDateBackupValueBytesV1 = 300;

enum class DateIndexFamilyV1 : std::uint8_t {
  aggregate_sketch, bitmap, brin_like, btree, columnar_zone_map,
  covering_included, document_path, expression, full_text, graph, hash,
  partial_filtered, range_exclusion, spatial, temporary_work, vector_ann,
};

enum class DateIndexDispositionV1 : std::uint8_t {
  admitted_with_recheck,
  admitted_exact_if_budget,
  admitted_payload_only,
  admitted_exact_selected_mode,
  conditional,
  not_applicable,
};

enum class DateProjectionKindV1 : std::uint8_t {
  none,
  equality_hash,
  sort_key,
  covering_value,
  zone_map,
  range_pair,
  conditional_result,
  conditional_underlying,
};

enum class DateProjectionModeV1 : std::uint8_t {
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

struct DateIndexResolutionV1 {
  DateIndexFamilyV1 family = DateIndexFamilyV1::btree;
  platform::Uuid compatibility_uuid{};
  u64 compatibility_generation = 0;
  DateIndexDispositionV1 disposition = DateIndexDispositionV1::not_applicable;
  DateProjectionKindV1 projection = DateProjectionKindV1::none;
  bool exact_recheck_required = false;
  bool receiving_owner_resolution_required = false;
};

struct DateIndexProjectionRequestV1 {
  DateIndexFamilyV1 family = DateIndexFamilyV1::btree;
  DateProjectionModeV1 mode = DateProjectionModeV1::unspecified;
  const DateOwnedValueV1* value = nullptr;
  const DateOwnedValueV1* upper_value = nullptr;
  const struct DateZoneMapRequestV1* zone_map = nullptr;
  const DateIndexResolutionV1* selected_conditional_family = nullptr;
  u64 provider_max_key_bytes = ~u64{0};
  DateExecutionControlV1 control;
};

struct DateIndexProjectionResultV1 {
  Status status;
  DateDiagnosticFactV1 diagnostic;
  DateIndexResolutionV1 resolution;
  std::vector<byte> bytes;
  bool owner_fact_only = false;
  bool ok() const noexcept { return status.ok(); }
};

DateIndexResolutionV1 ResolveDateIndexFamilyV1(
    DateIndexFamilyV1 family) noexcept;

enum class DateIndexPredicateOperationV1 : std::uint8_t {
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
enum class DateIndexPredicateFactV1 : std::uint8_t {
  exact_no_recheck_after_decode_reencode,
  requires_exact_state_day_recheck_never_final_match,
  prune_only_mandatory_source_row_predicate_recheck_never_final_match,
  refused,
};
struct DateIndexPredicateResolutionV1 {
  bool admitted = false;
  DateIndexPredicateFactV1 fact = DateIndexPredicateFactV1::refused;
  std::string_view diagnostic;
};
DateIndexPredicateResolutionV1 ResolveDateIndexPredicateV1(
    DateIndexFamilyV1 family, DateIndexPredicateOperationV1 operation) noexcept;
DateIndexProjectionResultV1 ProjectDateIndexValueV1(
    const DateIndexProjectionRequestV1& request) noexcept;

struct DateCoveringValueViewV1 {
  const DateValidatedProfileHandleV1* profile_handle = nullptr;
  DateValueStateV1 state = DateValueStateV1::value;
  i32 day = 0;
};
struct DateCoveringValueViewResultV1 {
  Status status;
  DateDiagnosticFactV1 diagnostic;
  DateCoveringValueViewV1 value;
  bool ok() const noexcept { return status.ok(); }
};
DateBytesResultV1 EncodeDateCoveringValueV1(
    const DateOwnedValueV1& value,
    const DateExecutionControlV1& control = {}) noexcept;
DateCoveringValueViewResultV1 DecodeDateCoveringValueNoAllocV1(
    const DateValidatedProfileHandleV1& profile_handle, bool null_allowed,
    std::span<const byte> encoded) noexcept;

struct DateRangePairViewV1 {
  i32 lower_day = 0;
  i32 upper_day = 0;
  std::span<const byte> lower_key;
  std::span<const byte> upper_key;
};
struct DateRangePairViewResultV1 {
  Status status;
  DateDiagnosticFactV1 diagnostic;
  DateRangePairViewV1 value;
  bool ok() const noexcept { return status.ok(); }
};
DateBytesResultV1 EncodeDateRangePairV1(
    const DateOwnedValueV1& lower, const DateOwnedValueV1& upper,
    const DateExecutionControlV1& control = {}) noexcept;
DateRangePairViewResultV1 DecodeDateRangePairNoAllocV1(
    const DateValidatedProfileHandleV1& profile_handle,
    std::span<const byte> encoded) noexcept;

struct DateZoneMapRequestV1 {
  std::shared_ptr<const DateValidatedProfileHandleV1> profile;
  const DateOwnedValueV1* minimum = nullptr;
  const DateOwnedValueV1* maximum = nullptr;
  u64 null_count = 0;
  u64 value_count = 0;
  u64 provider_max_key_bytes = ~u64{0};
  DateExecutionControlV1 control;
};
struct DateZoneMapViewV1 {
  const DateValidatedProfileHandleV1* profile_handle = nullptr;
  u64 null_count = 0;
  u64 value_count = 0;
  i32 minimum_day = 0;
  i32 maximum_day = 0;
  std::span<const byte> minimum_key;
  std::span<const byte> maximum_key;
};
struct DateZoneMapViewResultV1 {
  Status status;
  DateDiagnosticFactV1 diagnostic;
  DateZoneMapViewV1 value;
  bool ok() const noexcept { return status.ok(); }
};
DateBytesResultV1 EncodeDateZoneMapV1(
    const DateZoneMapRequestV1& request) noexcept;
DateZoneMapViewResultV1 DecodeDateZoneMapNoAllocV1(
    const DateValidatedProfileHandleV1& profile_handle,
    std::span<const byte> encoded) noexcept;

struct DateStatisticsHistogramRecordV1 {
  i32 inclusive_upper_day = 0;
  u64 noncumulative_count = 0;
};
struct DateStatisticsMcvRecordV1 {
  std::array<byte, 32> value_hash{};
  u64 frequency = 0;
};
// Lifetime pin for the immutable receiving-owner evidence record.  The date
// datatype validates only the nonnil V7 identity shape; ownership does not
// confer provenance or authorization.
struct DateStatisticsProviderEvidenceHandleV1 {
  platform::Uuid provider_evidence_uuid{};
};
struct DateStatisticsProjectionV1 {
  std::shared_ptr<const DateValidatedProfileHandleV1> profile;
  std::shared_ptr<const DateStatisticsProviderEvidenceHandleV1>
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
  std::span<const DateStatisticsHistogramRecordV1> histogram;
  std::span<const DateStatisticsMcvRecordV1> mcv;
};
struct DateDecodedStatisticsV1 {
  std::shared_ptr<const DateValidatedProfileHandleV1> profile;
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
  std::vector<DateStatisticsHistogramRecordV1> histogram;
  std::vector<DateStatisticsMcvRecordV1> mcv;
};
struct DateStatisticsDecodeResultV1 {
  Status status;
  DateDiagnosticFactV1 diagnostic;
  DateDecodedStatisticsV1 statistics;
  bool ok() const noexcept { return status.ok(); }
};
DateBytesResultV1 EncodeDateStatisticsProjectionV1(
    const DateStatisticsProjectionV1& projection,
    const DateExecutionControlV1& control = {}) noexcept;
DateStatisticsDecodeResultV1 DecodeDateStatisticsProjectionV1(
    const std::shared_ptr<const DateValidatedProfileHandleV1>& profile,
    std::span<const byte> encoded,
    const DateExecutionControlV1& control = {}) noexcept;

enum class DateStatisticsReceivingDispositionV1 : std::uint8_t {
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
enum class DateStatisticsEvidenceClassV1 : std::uint8_t {
  full_visible_population,
  partial_scan,
  weighted_input,
  sampled_input,
  snapshot_merge,
  histogram_source_contradiction,
};
struct DateStatisticsReceivingFactsV1 {
  bool security_visible = false;
  bool privacy_admitted = false;
  u64 schema_epoch = 0;
  u64 collection_epoch = 0;
  u64 security_epoch = 0;
  platform::Uuid provider_evidence_uuid{};
};
DateStatisticsReceivingDispositionV1 ResolveDateStatisticsReceivingFactsV1(
    const DateDecodedStatisticsV1& decoded,
    const DateStatisticsReceivingFactsV1& current) noexcept;
DateStatisticsReceivingDispositionV1 ResolveDateStatisticsEvidenceV1(
    DateStatisticsEvidenceClassV1 evidence) noexcept;

struct DateBackupTupleViewV1 {
  const DateValidatedProfileHandleV1* profile_handle = nullptr;
  DateValueStateV1 state = DateValueStateV1::value;
  i32 day = 0;
};
struct DateBackupTupleViewResultV1 {
  Status status;
  DateDiagnosticFactV1 diagnostic;
  DateBackupTupleViewV1 tuple;
  bool ok() const noexcept { return status.ok(); }
};
DateBytesResultV1 EncodeDateBackupTupleV1(
    const DateOwnedValueV1& value,
    const DateExecutionControlV1& control = {}) noexcept;
DateBackupTupleViewResultV1 DecodeDateBackupTupleNoAllocV1(
    const DateValidatedProfileHandleV1& profile_handle, bool null_allowed,
    std::span<const byte> encoded) noexcept;

enum class DateProtectionCellV1 : std::uint8_t {
  canonical_plain, inner_compression, inner_encryption, outer_compression,
  outer_authenticated_protection, outer_compress_then_protect,
  outer_protect_then_compress, page_or_filespace_crypto, transport_tls,
  protected_index,
};
struct DateProtectionResolutionV1 {
  DateProtectionCellV1 cell = DateProtectionCellV1::canonical_plain;
  bool datatype_admitted = false;
  bool receiving_owner_handoff = false;
  std::string_view disposition;
  std::string_view diagnostic;
};
DateProtectionResolutionV1 ResolveDateProtectionV1(
    DateProtectionCellV1 cell) noexcept;

enum class DateWireLaneV1 : std::uint8_t {
  native_sbwp, parser_server_ipc, canonical_sblr, apache_ignite, cassandra,
  clickhouse, cockroachdb, dolt, duckdb, firebird, foundationdb, immudb,
  influxdb, mariadb, milvus, mongodb, mysql, neo4j, opensearch, postgresql,
  redis, sqlite, tidb, tikv, vitess, xtdb, yugabytedb,
};
struct DateWireLaneResolutionV1 {
  DateWireLaneV1 lane = DateWireLaneV1::native_sbwp;
  bool admitted = false;
  bool component_mapping_complete = false;
  bool receiving_owner_handoff = true;
  std::string_view disposition;
  std::string_view diagnostic = "CTI.TRANSPORT.UNSUPPORTED";
};
DateWireLaneResolutionV1 ResolveDateWireLaneV1(DateWireLaneV1 lane) noexcept;

enum class DateDiagnosticAxisV1 : std::uint8_t {
  input_parse, descriptor_codec, storage_read_write, cast_invalid,
  cast_range_loss, operation_invalid, bounds_overflow_underflow,
  resource_cancellation, compression_corruption, encryption_auth_key,
  wire_decode_encode, index_key, domain_validation, recovery_corruption,
  statistics_read,
};
struct DateDiagnosticRouteV1 {
  DateDiagnosticAxisV1 axis = DateDiagnosticAxisV1::descriptor_codec;
  std::string_view code;
  platform::Uuid diagnostic_uuid{};
  std::string_view ordered_parameter_schema;
  std::string_view redaction;
};
std::span<const DateDiagnosticRouteV1> DateDiagnosticRoutesV1() noexcept;

enum class DateMetricEvidenceTypeV1 : std::uint8_t {
  descriptor_admissions, invalid_literals, range_refusals,
  operation_attempts, operation_success, operation_refusals, cast_attempts,
  cast_success, index_admission_refusals, statistics_stale,
  reference_mapping_misses, transport_refusals, serialization_refusals,
  protection_refusals, merge_manual_review, resource_budget_refusals,
  cancellations, decode_corruption, diagnostic_family,
};
struct DateMetricEvidenceTypeDescriptorV1 {
  DateMetricEvidenceTypeV1 type = DateMetricEvidenceTypeV1::descriptor_admissions;
  platform::Uuid evidence_type_uuid{};
  u64 generation = 0;
  std::string_view metric_name_metadata;
  std::string_view evidence_producer;
  std::string_view accepted_final_outcome;
};
std::span<const DateMetricEvidenceTypeDescriptorV1>
DateMetricEvidenceTypesV1() noexcept;

}  // namespace scratchbird::core::datatypes

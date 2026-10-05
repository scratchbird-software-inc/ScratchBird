// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "datatype_timestamp.hpp"

#include <array>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace scratchbird::core::datatypes {



inline constexpr u32 kTimestampCoveringNullBytesV3 = 1;
inline constexpr u32 kTimestampCoveringValueBytesV3 = 17;
inline constexpr u32 kTimestampRangePairBytesV3 = 224;
inline constexpr u32 kTimestampZoneMapEmptyBytesV3 = 112;
inline constexpr u32 kTimestampZoneMapValueBytesV3 = 336;
inline constexpr u32 kTimestampStatisticsHeaderBytesV3 = 544;
inline constexpr u32 kTimestampStatisticsMaximumBytesV3 = 5152;
inline constexpr u32 kTimestampBackupNullBytesV3 = 296;
inline constexpr u32 kTimestampBackupValueBytesV3 = 312;

enum class TimestampIndexFamilyV3 : std::uint8_t {
  aggregate_sketch, bitmap, brin_like, btree, columnar_zone_map,
  covering_included, document_path, expression, full_text, graph, hash,
  partial_filtered, range_exclusion, spatial, temporary_work, vector_ann,
};

enum class TimestampIndexDispositionV3 : std::uint8_t {
  admitted_with_recheck,
  admitted_exact_if_budget,
  admitted_payload_only,
  admitted_exact_selected_mode,
  conditional,
  not_applicable,
};

enum class TimestampProjectionKindV3 : std::uint8_t {
  none,
  equality_hash,
  sort_key,
  covering_value,
  zone_map,
  range_pair,
  conditional_result,
  conditional_underlying,
};

enum class TimestampProjectionModeV3 : std::uint8_t {
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

struct TimestampIndexResolutionV3 {
  TimestampIndexFamilyV3 family = TimestampIndexFamilyV3::btree;
  platform::Uuid compatibility_uuid{};
  u64 compatibility_generation = 0;
  TimestampIndexDispositionV3 disposition = TimestampIndexDispositionV3::not_applicable;
  TimestampProjectionKindV3 projection = TimestampProjectionKindV3::none;
  bool exact_recheck_required = false;
  bool receiving_owner_resolution_required = false;
};

struct TimestampIndexCompatibilityIdentityV3 {
  TimestampIndexFamilyV3 family = TimestampIndexFamilyV3::btree;
  platform::Uuid compatibility_uuid{};
  u64 compatibility_generation = 0;
};

struct TimestampIndexAdmissionResultV3 {
  bool admitted = false;
  TimestampIndexResolutionV3 resolution;
  std::string_view diagnostic = "OPTIMIZER.INDEX_COMPATIBILITY_MISSING";
};

TimestampIndexCompatibilityIdentityV3 LookupTimestampIndexCompatibilityIdentityV3(
    TimestampIndexFamilyV3 family) noexcept;
TimestampIndexAdmissionResultV3 AdmitTimestampIndexCompatibilityV3(
    const TimestampIndexCompatibilityIdentityV3& supplied) noexcept;

struct TimestampIndexProjectionRequestV3 {
  TimestampIndexCompatibilityIdentityV3 compatibility;
  TimestampProjectionModeV3 mode = TimestampProjectionModeV3::unspecified;
  const TimestampOwnedValueV3* value = nullptr;
  const TimestampOwnedValueV3* upper_value = nullptr;
  const struct TimestampZoneMapRequestV3* zone_map = nullptr;
  const TimestampIndexResolutionV3* selected_conditional_family = nullptr;
  u64 provider_max_key_bytes = ~u64{0};
  TimestampExecutionControlV3 control;
};

struct TimestampIndexProjectionResultV3 {
  Status status;
  TimestampDiagnosticFactV3 diagnostic;
  TimestampIndexResolutionV3 requested_resolution;
  TimestampIndexResolutionV3 resolution;
  std::vector<byte> bytes;
  bool owner_fact_only = false;
  bool ok() const noexcept { return status.ok(); }
};

TimestampIndexResolutionV3 ResolveTimestampIndexFamilyV3(
    TimestampIndexFamilyV3 family) noexcept;

enum class TimestampIndexPredicateOperationV3 : std::uint8_t {
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
enum class TimestampIndexPredicateFactV3 : std::uint8_t {
  exact_no_recheck_after_decode_reencode,
  requires_exact_state_component_recheck_never_final_match,
  prune_only_mandatory_source_row_predicate_recheck_never_final_match,
  refused,
};
struct TimestampIndexPredicateResolutionV3 {
  bool admitted = false;
  TimestampIndexPredicateFactV3 fact = TimestampIndexPredicateFactV3::refused;
  std::string_view diagnostic;
  TimestampIndexResolutionV3 requested_resolution;
  TimestampIndexResolutionV3 effective_resolution;
  bool predicate_owner_handoff_preserved = false;
};
struct TimestampIndexPredicateRequestV3 {
  TimestampIndexCompatibilityIdentityV3 requested;
  TimestampIndexPredicateOperationV3 operation =
      TimestampIndexPredicateOperationV3::unsupported;
  const TimestampIndexCompatibilityIdentityV3* selected_conditional = nullptr;
};
TimestampIndexPredicateResolutionV3 ResolveTimestampIndexPredicateV3(
    const TimestampIndexPredicateRequestV3& request) noexcept;

enum class TimestampIndexCandidateDispositionV3 : std::uint8_t {
  not_a_candidate,
  exact_match,
  exact_state_component_recheck_required,
  source_row_predicate_recheck_required,
  rejected_after_required_recheck,
};
struct TimestampIndexCandidateFactsV3 {
  TimestampIndexCandidateDispositionV3 disposition =
      TimestampIndexCandidateDispositionV3::not_a_candidate;
  bool final_match = false;
  bool exact_state_component_recheck_required = false;
  bool source_row_predicate_recheck_required = false;
};
TimestampIndexCandidateFactsV3 ClassifyTimestampIndexCandidateV3(
    TimestampIndexPredicateFactV3 predicate_fact, bool projection_candidate_equal,
    bool exact_state_component_equal,
    bool source_row_predicate_equal) noexcept;
TimestampIndexProjectionResultV3 ProjectTimestampIndexValueV3(
    const TimestampIndexProjectionRequestV3& request) noexcept;

struct TimestampCoveringValueViewV3 {
  const TimestampValidatedProfileHandleV3* profile_handle = nullptr;
  TimestampValueStateV3 state = TimestampValueStateV3::value;
  std::int32_t civil_day = 0;
  u64 nanoseconds_since_midnight = 0;
};
struct TimestampCoveringValueViewResultV3 {
  Status status;
  TimestampDiagnosticFactV3 diagnostic;
  TimestampCoveringValueViewV3 value;
  bool ok() const noexcept { return status.ok(); }
};
TimestampBytesResultV3 EncodeTimestampCoveringValueV3(
    const TimestampOwnedValueV3& value,
    const TimestampExecutionControlV3& control = {}) noexcept;
TimestampCoveringValueViewResultV3 DecodeTimestampCoveringValueNoAllocV3(
    const TimestampValidatedProfileHandleV3& profile_handle, bool null_allowed,
    std::span<const byte> encoded,
    const TimestampExecutionControlV3& control = {}) noexcept;

struct TimestampRangePairViewV3 {
  std::int32_t lower_civil_day = 0;
  u64 lower_nanoseconds_since_midnight = 0;
  std::int32_t upper_civil_day = 0;
  u64 upper_nanoseconds_since_midnight = 0;
  std::span<const byte> lower_key;
  std::span<const byte> upper_key;
};
struct TimestampRangePairViewResultV3 {
  Status status;
  TimestampDiagnosticFactV3 diagnostic;
  TimestampRangePairViewV3 value;
  bool ok() const noexcept { return status.ok(); }
};
TimestampBytesResultV3 EncodeTimestampRangePairV3(
    const TimestampOwnedValueV3& lower, const TimestampOwnedValueV3& upper,
    const TimestampExecutionControlV3& control = {}) noexcept;
TimestampRangePairViewResultV3 DecodeTimestampRangePairNoAllocV3(
    const TimestampValidatedProfileHandleV3& profile_handle,
    std::span<const byte> encoded,
    const TimestampExecutionControlV3& control = {}) noexcept;

struct TimestampZoneMapRequestV3 {
  std::shared_ptr<const TimestampValidatedProfileHandleV3> profile;
  const TimestampOwnedValueV3* minimum = nullptr;
  const TimestampOwnedValueV3* maximum = nullptr;
  u64 null_count = 0;
  u64 value_count = 0;
  u64 provider_max_key_bytes = ~u64{0};
  TimestampExecutionControlV3 control;
};
struct TimestampZoneMapViewV3 {
  const TimestampValidatedProfileHandleV3* profile_handle = nullptr;
  u64 null_count = 0;
  u64 value_count = 0;
  std::int32_t minimum_civil_day = 0;
  u64 minimum_nanoseconds_since_midnight = 0;
  std::int32_t maximum_civil_day = 0;
  u64 maximum_nanoseconds_since_midnight = 0;
  std::span<const byte> minimum_key;
  std::span<const byte> maximum_key;
};
struct TimestampZoneMapViewResultV3 {
  Status status;
  TimestampDiagnosticFactV3 diagnostic;
  TimestampZoneMapViewV3 value;
  bool ok() const noexcept { return status.ok(); }
};
TimestampBytesResultV3 EncodeTimestampZoneMapV3(
    const TimestampZoneMapRequestV3& request) noexcept;
TimestampZoneMapViewResultV3 DecodeTimestampZoneMapNoAllocV3(
    const TimestampValidatedProfileHandleV3& profile_handle,
    std::span<const byte> encoded,
    const TimestampExecutionControlV3& control = {}) noexcept;

struct TimestampStatisticsHistogramRecordV3 {
  std::int32_t inclusive_upper_civil_day = 0;
  u64 inclusive_upper_nanoseconds_since_midnight = 0;
  u64 cumulative_count = 0;
  bool operator==(const TimestampStatisticsHistogramRecordV3&) const = default;
};
struct TimestampStatisticsMcvRecordV3 {
  std::array<byte, 32> value_hash{};
  u64 frequency = 0;
  bool operator==(const TimestampStatisticsMcvRecordV3&) const = default;
};
enum class TimestampStatisticsEvidenceClassV3 : std::uint8_t {
  full_visible_population,
  partial_scan,
  weighted_input,
  sampled_input,
  snapshot_merge,
  histogram_source_contradiction,
};
// Immutable authenticated facts supplied by the receiving statistics owner.
// The datatype validates these facts and never derives visibility, population,
// privacy, or epoch authority from shared ownership alone.
struct TimestampStatisticsProviderEvidenceHandleV3 {
  platform::Uuid provider_evidence_uuid{};
  platform::Uuid source_object_uuid{};
  bool authenticated_population = false;
  bool source_authorized = false;
  bool privacy_admitted = false;
  TimestampStatisticsEvidenceClassV3 evidence_class =
      TimestampStatisticsEvidenceClassV3::partial_scan;
  u64 schema_epoch = 0;
  u64 collection_epoch = 0;
  u64 security_epoch = 0;
  u64 row_count = 0;
  u64 null_count = 0;
  u64 value_count = 0;
  bool minimum_maximum_present = false;
  std::int32_t minimum_civil_day = 0;
  u64 minimum_nanoseconds_since_midnight = 0;
  std::int32_t maximum_civil_day = 0;
  u64 maximum_nanoseconds_since_midnight = 0;
  std::vector<TimestampStatisticsHistogramRecordV3> histogram;
  std::vector<TimestampStatisticsMcvRecordV3> mcv;
};
struct TimestampStatisticsProjectionV3 {
  std::shared_ptr<const TimestampValidatedProfileHandleV3> profile;
  std::shared_ptr<const TimestampStatisticsProviderEvidenceHandleV3>
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
  std::int32_t minimum_civil_day = 0;
  u64 minimum_nanoseconds_since_midnight = 0;
  std::int32_t maximum_civil_day = 0;
  u64 maximum_nanoseconds_since_midnight = 0;
  std::span<const TimestampStatisticsHistogramRecordV3> histogram;
  std::span<const TimestampStatisticsMcvRecordV3> mcv;
};
struct TimestampDecodedStatisticsV3 {
  std::shared_ptr<const TimestampValidatedProfileHandleV3> profile;
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
  std::int32_t minimum_civil_day = 0;
  u64 minimum_nanoseconds_since_midnight = 0;
  std::int32_t maximum_civil_day = 0;
  u64 maximum_nanoseconds_since_midnight = 0;
  std::vector<TimestampStatisticsHistogramRecordV3> histogram;
  std::vector<TimestampStatisticsMcvRecordV3> mcv;
};
struct TimestampStatisticsDecodeResultV3 {
  Status status;
  TimestampDiagnosticFactV3 diagnostic;
  TimestampDecodedStatisticsV3 statistics;
  bool ok() const noexcept { return status.ok(); }
};
TimestampBytesResultV3 EncodeTimestampStatisticsProjectionV3(
    const TimestampStatisticsProjectionV3& projection,
    const TimestampExecutionControlV3& control = {}) noexcept;
TimestampStatisticsDecodeResultV3 DecodeTimestampStatisticsProjectionV3(
    const std::shared_ptr<const TimestampValidatedProfileHandleV3>& profile,
    std::span<const byte> encoded,
    const TimestampExecutionControlV3& control = {}) noexcept;

enum class TimestampStatisticsReceivingDispositionV3 : std::uint8_t {
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
struct TimestampStatisticsReceivingFactsV3 {
  bool security_visible = false;
  bool privacy_admitted = false;
  u64 schema_epoch = 0;
  u64 collection_epoch = 0;
  u64 security_epoch = 0;
  platform::Uuid provider_evidence_uuid{};
  platform::Uuid source_object_uuid{};
};
TimestampStatisticsReceivingDispositionV3 ResolveTimestampStatisticsReceivingFactsV3(
    const TimestampDecodedStatisticsV3& decoded,
    const TimestampStatisticsReceivingFactsV3& current) noexcept;
TimestampStatisticsReceivingDispositionV3 ResolveTimestampStatisticsEvidenceV3(
    TimestampStatisticsEvidenceClassV3 evidence) noexcept;

enum class TimestampStatisticsReadAdmissionDispositionV3 : std::uint8_t {
  admitted,
  security_denied,
  privacy_denied,
  profile_refused,
  format_refused,
  resource_refused,
  cancelled,
  schema_epoch_mismatch,
  collection_epoch_mismatch,
  security_epoch_mismatch,
  provider_evidence_mismatch,
};
struct TimestampStatisticsReadAdmissionResultV3 {
  TimestampStatisticsReadAdmissionDispositionV3 disposition =
      TimestampStatisticsReadAdmissionDispositionV3::format_refused;
  TimestampDiagnosticFactV3 diagnostic;
  TimestampDecodedStatisticsV3 statistics;
  bool decode_attempted = false;
  u64 payload_reads = 0;
  bool receiving_owner_enforcement_required = true;
  bool datatype_outcome_unchanged = true;
  bool metric_outcome_unchanged = true;
  bool ok() const noexcept {
    return disposition == TimestampStatisticsReadAdmissionDispositionV3::admitted;
  }
};
// Pure admission classifier.  The receiving owner remains responsible for
// producing authoritative security/privacy facts and enforcing the decision;
// this API only fixes their precedence ahead of datatype format decoding.
TimestampStatisticsReadAdmissionResultV3 AdmitTimestampStatisticsReadV3(
    const std::shared_ptr<const TimestampValidatedProfileHandleV3>& profile,
    std::span<const byte> encoded,
    const TimestampStatisticsReceivingFactsV3& current,
    const TimestampExecutionControlV3& control = {}) noexcept;

struct TimestampBackupTupleViewV3 {
  const TimestampValidatedProfileHandleV3* profile_handle = nullptr;
  TimestampValueStateV3 state = TimestampValueStateV3::value;
  std::int32_t civil_day = 0;
  u64 nanoseconds_since_midnight = 0;
};
struct TimestampBackupTupleViewResultV3 {
  Status status;
  TimestampDiagnosticFactV3 diagnostic;
  TimestampBackupTupleViewV3 tuple;
  bool ok() const noexcept { return status.ok(); }
};
TimestampBytesResultV3 EncodeTimestampBackupTupleV3(
    const TimestampOwnedValueV3& value,
    const TimestampExecutionControlV3& control = {}) noexcept;
TimestampBackupTupleViewResultV3 DecodeTimestampBackupTupleNoAllocV3(
    const TimestampValidatedProfileHandleV3& profile_handle, bool null_allowed,
    std::span<const byte> encoded,
    const TimestampExecutionControlV3& control = {}) noexcept;

enum class TimestampProtectionCellV3 : std::uint8_t {
  canonical_plain, inner_compression, inner_encryption, outer_compression,
  outer_authenticated_protection, outer_compress_then_protect,
  outer_protect_then_compress, page_or_filespace_crypto, transport_tls,
  protected_index,
};
struct TimestampProtectionResolutionV3 {
  TimestampProtectionCellV3 cell = TimestampProtectionCellV3::canonical_plain;
  bool datatype_admitted = false;
  bool receiving_owner_handoff = false;
  std::string_view disposition;
  std::string_view diagnostic;
};
TimestampProtectionResolutionV3 ResolveTimestampProtectionV3(
    TimestampProtectionCellV3 cell) noexcept;

enum class TimestampProtectionModeV3 : std::uint8_t {
  clear,
  compressed,
  encrypted,
  compressed_encrypted,
};
enum class TimestampProtectionIndexModeV3 : std::uint8_t {
  not_indexed,
  indexed,
};
struct TimestampProtectionCombinationResolutionV3 {
  TimestampProtectionModeV3 storage_mode = TimestampProtectionModeV3::clear;
  TimestampProtectionModeV3 stream_mode = TimestampProtectionModeV3::clear;
  TimestampProtectionIndexModeV3 index_mode =
      TimestampProtectionIndexModeV3::not_indexed;
  bool admitted = false;
  std::string_view disposition;
  std::string_view diagnostic;
};
TimestampProtectionCombinationResolutionV3
ResolveTimestampProtectionCombinationV3(
    TimestampProtectionModeV3 storage_mode,
    TimestampProtectionModeV3 stream_mode,
    TimestampProtectionIndexModeV3 index_mode) noexcept;
TimestampProtectionCombinationResolutionV3
AdmitTimestampProtectionCombinationV3(
    const TimestampValidatedProfileHandleV3& profile,
    TimestampProtectionModeV3 storage_mode,
    TimestampProtectionModeV3 stream_mode,
    TimestampProtectionIndexModeV3 index_mode) noexcept;

enum class TimestampWireLaneV3 : std::uint8_t {
  native_sbwp, parser_server_ipc, canonical_sblr, apache_ignite, cassandra,
  clickhouse, cockroachdb, dolt, duckdb, firebird, foundationdb, immudb,
  influxdb, mariadb, milvus, mongodb, mysql, neo4j, opensearch, postgresql,
  redis, sqlite, tidb, tikv, vitess, xtdb, yugabytedb,
};
struct TimestampWireLaneResolutionV3 {
  TimestampWireLaneV3 lane = TimestampWireLaneV3::native_sbwp;
  bool admitted = false;
  bool component_mapping_complete = false;
  bool receiving_owner_handoff = true;
  std::string_view disposition;
  std::string_view diagnostic = "CTI.TRANSPORT.UNSUPPORTED";
};
TimestampWireLaneResolutionV3 ResolveTimestampWireLaneV3(TimestampWireLaneV3 lane) noexcept;

enum class TimestampDiagnosticAxisV3 : std::uint8_t {
  input_parse, descriptor_codec, storage_read_write, cast_invalid,
  cast_range_loss, operation_invalid, bounds_overflow_underflow,
  resource_cancellation, compression_corruption, encryption_auth_key,
  wire_decode_encode, index_key, domain_validation, recovery_corruption,
  statistics_read,
};
struct TimestampDiagnosticRouteV3 {
  TimestampDiagnosticAxisV3 axis = TimestampDiagnosticAxisV3::descriptor_codec;
  std::string_view code;
  platform::Uuid diagnostic_uuid{};
  std::string_view ordered_parameter_schema;
  std::string_view redaction;
};
std::span<const TimestampDiagnosticRouteV3> TimestampDiagnosticRoutesV3() noexcept;

enum class TimestampDiagnosticRouteScalarTypeV3 : std::uint8_t {
  uuid,
  u8,
  u32,
  u64,
  closed_enum,
};
struct TimestampDiagnosticRouteParameterV3 {
  std::string_view token;
  TimestampDiagnosticRouteScalarTypeV3 scalar_type =
      TimestampDiagnosticRouteScalarTypeV3::uuid;
  platform::Uuid uuid_value{};
  // Stored in u64 so callers can present and deterministically reject a value
  // above UINT32_MAX without narrowing it before validation.
  u64 unsigned_value = 0;
  std::string_view enum_value;
  friend bool operator==(const TimestampDiagnosticRouteParameterV3&,
                         const TimestampDiagnosticRouteParameterV3&) = default;
};
struct TimestampDiagnosticRouteExecutionRequestV3 {
  TimestampDiagnosticAxisV3 axis = TimestampDiagnosticAxisV3::descriptor_codec;
  std::string_view diagnostic_code;
  platform::Uuid diagnostic_uuid{};
  std::span<const TimestampDiagnosticRouteParameterV3> ordered_payload;
  std::string_view redaction;
};
enum class TimestampDiagnosticRouteExecutionDispositionV3 : std::uint8_t {
  admitted,
  missing_token,
  extra_token,
  reordered_tokens,
  wrong_scalar_type,
  unknown_token,
  mismatched_known_token,
  invalid_scalar_value,
  unknown_route,
  code_uuid_mismatch,
  redaction_mismatch,
};
struct TimestampDiagnosticRouteExecutionFactV3 {
  TimestampDiagnosticRouteExecutionDispositionV3 disposition =
      TimestampDiagnosticRouteExecutionDispositionV3::unknown_route;
  TimestampDiagnosticAxisV3 axis = TimestampDiagnosticAxisV3::descriptor_codec;
  std::string_view diagnostic_code;
  platform::Uuid diagnostic_uuid{};
  std::array<TimestampDiagnosticRouteParameterV3, 5> ordered_payload{};
  std::uint8_t parameter_count = 0;
  std::string_view redaction;
  platform::Uuid metric_evidence_type_uuid{};
  std::string_view metric_name;
  bool diagnostic_fact_emitted = false;
  bool receiving_owner_evidence_handoff_ready = false;
  bool metric_mutation_admitted = false;
  bool datatype_outcome_unchanged = true;
  bool metric_outcome_unchanged = true;
  bool ok() const noexcept {
    return disposition == TimestampDiagnosticRouteExecutionDispositionV3::admitted;
  }
};
TimestampDiagnosticRouteExecutionFactV3 ExecuteTimestampDiagnosticRouteV3(
    const TimestampDiagnosticRouteExecutionRequestV3& request) noexcept;

enum class TimestampDiagnosticPrecedenceScopeV3 : std::uint8_t {
  global,
  artifact_decode,
  projection_decode,
  boundary_decode,
  cast,
  operation,
  index_admission,
  recovery_merge,
  statistics_read,
  text_fields,
  intrinsic_operation_gates,
};
struct TimestampDiagnosticFormatOrderV3 {
  std::string_view format_magic;
  std::string_view authority_file;
};
struct TimestampDiagnosticCombinedFaultV3 {
  std::string_view supplied_text;
  std::string_view selected_reason;
};
struct TimestampDiagnosticClassificationRuleV3 {
  std::string_view scope;
  std::string_view rule;
};
std::span<const std::string_view> TimestampDiagnosticPrecedenceV3(
    TimestampDiagnosticPrecedenceScopeV3 scope) noexcept;
std::span<const TimestampDiagnosticFormatOrderV3>
TimestampDiagnosticFormatOrdersV3() noexcept;
std::span<const TimestampDiagnosticCombinedFaultV3>
TimestampDiagnosticTextCombinedFaultsV3() noexcept;
std::span<const TimestampDiagnosticClassificationRuleV3>
TimestampDiagnosticClassificationRulesV3() noexcept;
std::string_view TimestampDiagnosticSelectionRuleV3() noexcept;

struct TimestampDiagnosticRouteKeyV3 {
  TimestampDiagnosticAxisV3 axis = TimestampDiagnosticAxisV3::descriptor_codec;
  platform::Uuid diagnostic_uuid{};
};
struct TimestampDiagnosticMetricRouteV3 {
  TimestampDiagnosticAxisV3 diagnostic_family =
      TimestampDiagnosticAxisV3::descriptor_codec;
  platform::Uuid diagnostic_uuid{};
  platform::Uuid metric_evidence_type_uuid{};
  std::string_view metric_name;
  std::string_view rule =
      "after final primary diagnostic selection exactly once";
};
enum class TimestampDiagnosticRouteFactDispositionV3 : std::uint8_t {
  admitted,
  missing,
  unknown,
  wrong_type,
  reordered,
  duplicate,
};
struct TimestampDiagnosticMetricRouteResultV3 {
  TimestampDiagnosticRouteFactDispositionV3 disposition =
      TimestampDiagnosticRouteFactDispositionV3::unknown;
  TimestampDiagnosticMetricRouteV3 route;
  bool metric_mutation_admitted = false;
};
std::span<const TimestampDiagnosticMetricRouteV3>
TimestampDiagnosticMetricRoutesV3() noexcept;
TimestampDiagnosticMetricRouteResultV3 ResolveTimestampDiagnosticMetricRouteV3(
    const TimestampDiagnosticRouteKeyV3& key) noexcept;
TimestampDiagnosticRouteFactDispositionV3 ValidateTimestampDiagnosticMetricRouteTableV3(
    std::span<const TimestampDiagnosticMetricRouteV3> supplied,
    bool scalar_types_valid = true) noexcept;

enum class TimestampMetricEvidenceTypeV3 : std::uint8_t {
  operation_attempt,
  operation_success,
  operation_refusal,
  cancellation,
  serialization_refusal,
  decode_corruption,
  profile_refusal,
  cast_refusal,
  projection_refusal,
};
struct TimestampMetricEvidenceTypeDescriptorV3 {
  TimestampMetricEvidenceTypeV3 type = TimestampMetricEvidenceTypeV3::operation_attempt;
  platform::Uuid evidence_type_uuid{};
  u64 generation = 0;
  std::string_view metric_name_metadata;
  std::string_view evidence_producer;
  std::string_view evidence_record;
  std::string_view manager_consumer;
  platform::Uuid metric_uuid{};
  std::string_view closed_label_schema;
  std::string_view cardinality_bound;
  std::string_view trigger_phase;
  std::string_view trigger_condition;
  std::string_view update_rule;
  std::string_view failure_behavior;
  std::string_view accepted_final_outcome;
};
std::span<const TimestampMetricEvidenceTypeDescriptorV3>
TimestampMetricEvidenceTypesV3() noexcept;

struct TimestampMetricLabelMemberV3 {
  std::string_view name;
  std::string_view value;
};
struct TimestampMetricClosedLabelTupleV3 {
  TimestampMetricEvidenceTypeV3 type = TimestampMetricEvidenceTypeV3::operation_attempt;
  std::array<TimestampMetricLabelMemberV3, 2> members{};
  std::uint8_t member_count = 0;
};
std::span<const TimestampMetricClosedLabelTupleV3>
TimestampMetricClosedLabelTuplesV3() noexcept;

struct TimestampMetricEvidenceEnvelopeIdentityV3 {
  platform::Uuid schema_uuid{};
  u64 generation = 0;
};
TimestampMetricEvidenceEnvelopeIdentityV3 TimestampMetricEvidenceEnvelopeIdentityV3Value()
    noexcept;

struct TimestampMetricEvidenceEnvelopeV3 {
  TimestampMetricEvidenceEnvelopeIdentityV3 envelope_identity;
  platform::Uuid evidence_type_uuid{};
  platform::Uuid metric_uuid{};
  platform::Uuid source_event_uuid{};
  platform::Uuid database_uuid{};
  platform::Uuid node_uuid{};
  u64 process_epoch = 0;
  u64 event_sequence = 0;
  u64 monotonic_timestamp_ns = 0;
  u64 counter_delta = 0;
  std::span<const TimestampMetricLabelMemberV3> closed_label_tuple;
  bool outcome_committed = false;
  bool all_required_fields_present = true;
  bool scalar_types_valid = true;
};
struct TimestampMetricEvidenceValidationContextV3 {
  u64 current_process_epoch = 0;
  u64 current_counter_value = 0;
};
enum class TimestampMetricEvidenceDispositionV3 : std::uint8_t {
  admitted,
  missing_field,
  unknown_evidence_type,
  undeclared_label_tuple,
  wrong_scalar,
  outcome_not_committed,
  invalid_counter_delta,
  stale_process_epoch,
  counter_overflow,
};
struct TimestampMetricEvidenceIdempotencyFactV3 {
  u64 process_epoch = 0;
  u64 event_sequence = 0;
  platform::Uuid source_event_uuid{};
  platform::Uuid evidence_type_uuid{};
};
struct TimestampMetricEvidenceValidationResultV3 {
  TimestampMetricEvidenceDispositionV3 disposition =
      TimestampMetricEvidenceDispositionV3::missing_field;
  TimestampMetricEvidenceTypeV3 type =
      TimestampMetricEvidenceTypeV3::operation_attempt;
  TimestampMetricEvidenceIdempotencyFactV3 idempotency;
  bool receiving_owner_update_admitted = false;
  bool datatype_outcome_unchanged = true;
};
TimestampMetricEvidenceValidationResultV3 ValidateTimestampMetricEvidenceV3(
    const TimestampMetricEvidenceEnvelopeV3& evidence,
    const TimestampMetricEvidenceValidationContextV3& context) noexcept;

enum class TimestampMetricSameSourceDispositionV3 : std::uint8_t {
  independent_sources,
  admitted_ordered_pair,
  duplicate_type_refused,
  unregistered_pair_refused,
  reversed_order_refused,
  stale_process_epoch_refused,
};
struct TimestampMetricSameSourceFactV3 {
  TimestampMetricSameSourceDispositionV3 disposition =
      TimestampMetricSameSourceDispositionV3::unregistered_pair_refused;
  bool second_emission_admitted = false;
  bool receiving_owner_update_admitted = false;
  bool datatype_outcome_unchanged = true;
};
TimestampMetricSameSourceFactV3 ClassifyTimestampMetricSameSourceEmissionV3(
    const TimestampDiagnosticRouteKeyV3& route,
    const TimestampMetricEvidenceIdempotencyFactV3& first,
    const TimestampMetricEvidenceIdempotencyFactV3& second) noexcept;

}  // namespace scratchbird::core::datatypes

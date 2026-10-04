// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "datatype_time.hpp"

#include <array>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace scratchbird::core::datatypes {



inline constexpr u32 kTimeZoneMapEmptyBytesV3 = 96;
inline constexpr u32 kTimeZoneMapValueBytesV3 = 312;
inline constexpr u32 kTimeStatisticsHeaderBytesV3 = 528;
inline constexpr u32 kTimeStatisticsMaximumBytesV3 = 4624;
inline constexpr u32 kTimeBackupNullBytesV3 = 296;
inline constexpr u32 kTimeBackupValueBytesV3 = 304;

enum class TimeIndexFamilyV3 : std::uint8_t {
  aggregate_sketch, bitmap, brin_like, btree, columnar_zone_map,
  covering_included, document_path, expression, full_text, graph, hash,
  partial_filtered, range_exclusion, spatial, temporary_work, vector_ann,
};

enum class TimeIndexDispositionV3 : std::uint8_t {
  admitted_with_recheck,
  admitted_exact_if_budget,
  admitted_payload_only,
  admitted_exact_selected_mode,
  conditional,
  not_applicable,
};

enum class TimeProjectionKindV3 : std::uint8_t {
  none,
  equality_hash,
  sort_key,
  covering_value,
  zone_map,
  range_pair,
  conditional_result,
  conditional_underlying,
};

enum class TimeProjectionModeV3 : std::uint8_t {
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

struct TimeIndexResolutionV3 {
  TimeIndexFamilyV3 family = TimeIndexFamilyV3::btree;
  platform::Uuid compatibility_uuid{};
  u64 compatibility_generation = 0;
  TimeIndexDispositionV3 disposition = TimeIndexDispositionV3::not_applicable;
  TimeProjectionKindV3 projection = TimeProjectionKindV3::none;
  bool exact_recheck_required = false;
  bool receiving_owner_resolution_required = false;
};

struct TimeIndexCompatibilityIdentityV3 {
  TimeIndexFamilyV3 family = TimeIndexFamilyV3::btree;
  platform::Uuid compatibility_uuid{};
  u64 compatibility_generation = 0;
};

struct TimeIndexAdmissionResultV3 {
  bool admitted = false;
  TimeIndexResolutionV3 resolution;
  std::string_view diagnostic = "OPTIMIZER.INDEX_COMPATIBILITY_MISSING";
};

TimeIndexCompatibilityIdentityV3 LookupTimeIndexCompatibilityIdentityV3(
    TimeIndexFamilyV3 family) noexcept;
TimeIndexAdmissionResultV3 AdmitTimeIndexCompatibilityV3(
    const TimeIndexCompatibilityIdentityV3& supplied) noexcept;

struct TimeIndexProjectionRequestV3 {
  TimeIndexCompatibilityIdentityV3 compatibility;
  TimeProjectionModeV3 mode = TimeProjectionModeV3::unspecified;
  const TimeOwnedValueV3* value = nullptr;
  const TimeOwnedValueV3* upper_value = nullptr;
  const struct TimeZoneMapRequestV3* zone_map = nullptr;
  const TimeIndexResolutionV3* selected_conditional_family = nullptr;
  u64 provider_max_key_bytes = ~u64{0};
  TimeExecutionControlV3 control;
};

struct TimeIndexProjectionResultV3 {
  Status status;
  TimeDiagnosticFactV3 diagnostic;
  TimeIndexResolutionV3 requested_resolution;
  TimeIndexResolutionV3 resolution;
  std::vector<byte> bytes;
  bool owner_fact_only = false;
  bool ok() const noexcept { return status.ok(); }
};

TimeIndexResolutionV3 ResolveTimeIndexFamilyV3(
    TimeIndexFamilyV3 family) noexcept;

enum class TimeIndexPredicateOperationV3 : std::uint8_t {
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
enum class TimeIndexPredicateFactV3 : std::uint8_t {
  exact_no_recheck_after_decode_reencode,
  requires_exact_state_component_recheck_never_final_match,
  prune_only_mandatory_source_row_predicate_recheck_never_final_match,
  refused,
};
struct TimeIndexPredicateResolutionV3 {
  bool admitted = false;
  TimeIndexPredicateFactV3 fact = TimeIndexPredicateFactV3::refused;
  std::string_view diagnostic;
  TimeIndexResolutionV3 requested_resolution;
  TimeIndexResolutionV3 effective_resolution;
  bool predicate_owner_handoff_preserved = false;
};
struct TimeIndexPredicateRequestV3 {
  TimeIndexCompatibilityIdentityV3 requested;
  TimeIndexPredicateOperationV3 operation =
      TimeIndexPredicateOperationV3::unsupported;
  const TimeIndexCompatibilityIdentityV3* selected_conditional = nullptr;
};
TimeIndexPredicateResolutionV3 ResolveTimeIndexPredicateV3(
    const TimeIndexPredicateRequestV3& request) noexcept;

enum class TimeIndexCandidateDispositionV3 : std::uint8_t {
  not_a_candidate,
  exact_match,
  exact_state_component_recheck_required,
  source_row_predicate_recheck_required,
  rejected_after_required_recheck,
};
struct TimeIndexCandidateFactsV3 {
  TimeIndexCandidateDispositionV3 disposition =
      TimeIndexCandidateDispositionV3::not_a_candidate;
  bool final_match = false;
  bool exact_state_component_recheck_required = false;
  bool source_row_predicate_recheck_required = false;
};
TimeIndexCandidateFactsV3 ClassifyTimeIndexCandidateV3(
    TimeIndexPredicateFactV3 predicate_fact, bool projection_candidate_equal,
    bool exact_state_component_equal,
    bool source_row_predicate_equal) noexcept;
TimeIndexProjectionResultV3 ProjectTimeIndexValueV3(
    const TimeIndexProjectionRequestV3& request) noexcept;

struct TimeCoveringValueViewV3 {
  const TimeValidatedProfileHandleV3* profile_handle = nullptr;
  TimeValueStateV3 state = TimeValueStateV3::value;
  u64 nanoseconds_since_midnight = 0;
};
struct TimeCoveringValueViewResultV3 {
  Status status;
  TimeDiagnosticFactV3 diagnostic;
  TimeCoveringValueViewV3 value;
  bool ok() const noexcept { return status.ok(); }
};
TimeBytesResultV3 EncodeTimeCoveringValueV3(
    const TimeOwnedValueV3& value,
    const TimeExecutionControlV3& control = {}) noexcept;
TimeCoveringValueViewResultV3 DecodeTimeCoveringValueNoAllocV3(
    const TimeValidatedProfileHandleV3& profile_handle, bool null_allowed,
    std::span<const byte> encoded,
    const TimeExecutionControlV3& control = {}) noexcept;

struct TimeRangePairViewV3 {
  u64 lower_nanoseconds = 0;
  u64 upper_nanoseconds = 0;
  std::span<const byte> lower_key;
  std::span<const byte> upper_key;
};
struct TimeRangePairViewResultV3 {
  Status status;
  TimeDiagnosticFactV3 diagnostic;
  TimeRangePairViewV3 value;
  bool ok() const noexcept { return status.ok(); }
};
TimeBytesResultV3 EncodeTimeRangePairV3(
    const TimeOwnedValueV3& lower, const TimeOwnedValueV3& upper,
    const TimeExecutionControlV3& control = {}) noexcept;
TimeRangePairViewResultV3 DecodeTimeRangePairNoAllocV3(
    const TimeValidatedProfileHandleV3& profile_handle,
    std::span<const byte> encoded,
    const TimeExecutionControlV3& control = {}) noexcept;

struct TimeZoneMapRequestV3 {
  std::shared_ptr<const TimeValidatedProfileHandleV3> profile;
  const TimeOwnedValueV3* minimum = nullptr;
  const TimeOwnedValueV3* maximum = nullptr;
  u64 null_count = 0;
  u64 value_count = 0;
  u64 provider_max_key_bytes = ~u64{0};
  TimeExecutionControlV3 control;
};
struct TimeZoneMapViewV3 {
  const TimeValidatedProfileHandleV3* profile_handle = nullptr;
  u64 null_count = 0;
  u64 value_count = 0;
  u64 minimum_nanoseconds = 0;
  u64 maximum_nanoseconds = 0;
  std::span<const byte> minimum_key;
  std::span<const byte> maximum_key;
};
struct TimeZoneMapViewResultV3 {
  Status status;
  TimeDiagnosticFactV3 diagnostic;
  TimeZoneMapViewV3 value;
  bool ok() const noexcept { return status.ok(); }
};
TimeBytesResultV3 EncodeTimeZoneMapV3(
    const TimeZoneMapRequestV3& request) noexcept;
TimeZoneMapViewResultV3 DecodeTimeZoneMapNoAllocV3(
    const TimeValidatedProfileHandleV3& profile_handle,
    std::span<const byte> encoded,
    const TimeExecutionControlV3& control = {}) noexcept;

struct TimeStatisticsHistogramRecordV3 {
  u64 inclusive_upper_nanoseconds = 0;
  u64 cumulative_count = 0;
};
struct TimeStatisticsMcvRecordV3 {
  std::array<byte, 32> value_hash{};
  u64 frequency = 0;
};
// Lifetime pin for the immutable receiving-owner evidence record.  The time
// datatype validates only the nonnil V7 identity shape; ownership does not
// confer provenance or authorization.
struct TimeStatisticsProviderEvidenceHandleV3 {
  platform::Uuid provider_evidence_uuid{};
};
struct TimeStatisticsProjectionV3 {
  std::shared_ptr<const TimeValidatedProfileHandleV3> profile;
  std::shared_ptr<const TimeStatisticsProviderEvidenceHandleV3>
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
  u64 minimum_nanoseconds = 0;
  u64 maximum_nanoseconds = 0;
  std::span<const TimeStatisticsHistogramRecordV3> histogram;
  std::span<const TimeStatisticsMcvRecordV3> mcv;
};
struct TimeDecodedStatisticsV3 {
  std::shared_ptr<const TimeValidatedProfileHandleV3> profile;
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
  u64 minimum_nanoseconds = 0;
  u64 maximum_nanoseconds = 0;
  std::vector<TimeStatisticsHistogramRecordV3> histogram;
  std::vector<TimeStatisticsMcvRecordV3> mcv;
};
struct TimeStatisticsDecodeResultV3 {
  Status status;
  TimeDiagnosticFactV3 diagnostic;
  TimeDecodedStatisticsV3 statistics;
  bool ok() const noexcept { return status.ok(); }
};
TimeBytesResultV3 EncodeTimeStatisticsProjectionV3(
    const TimeStatisticsProjectionV3& projection,
    const TimeExecutionControlV3& control = {}) noexcept;
TimeStatisticsDecodeResultV3 DecodeTimeStatisticsProjectionV3(
    const std::shared_ptr<const TimeValidatedProfileHandleV3>& profile,
    std::span<const byte> encoded,
    const TimeExecutionControlV3& control = {}) noexcept;

enum class TimeStatisticsReceivingDispositionV3 : std::uint8_t {
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
enum class TimeStatisticsEvidenceClassV3 : std::uint8_t {
  full_visible_population,
  partial_scan,
  weighted_input,
  sampled_input,
  snapshot_merge,
  histogram_source_contradiction,
};
struct TimeStatisticsReceivingFactsV3 {
  bool security_visible = false;
  bool privacy_admitted = false;
  u64 schema_epoch = 0;
  u64 collection_epoch = 0;
  u64 security_epoch = 0;
  platform::Uuid provider_evidence_uuid{};
};
TimeStatisticsReceivingDispositionV3 ResolveTimeStatisticsReceivingFactsV3(
    const TimeDecodedStatisticsV3& decoded,
    const TimeStatisticsReceivingFactsV3& current) noexcept;
TimeStatisticsReceivingDispositionV3 ResolveTimeStatisticsEvidenceV3(
    TimeStatisticsEvidenceClassV3 evidence) noexcept;

enum class TimeStatisticsReadAdmissionDispositionV3 : std::uint8_t {
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
struct TimeStatisticsReadAdmissionResultV3 {
  TimeStatisticsReadAdmissionDispositionV3 disposition =
      TimeStatisticsReadAdmissionDispositionV3::format_refused;
  TimeDiagnosticFactV3 diagnostic;
  TimeDecodedStatisticsV3 statistics;
  bool decode_attempted = false;
  bool receiving_owner_enforcement_required = true;
  bool datatype_outcome_unchanged = true;
  bool metric_outcome_unchanged = true;
  bool ok() const noexcept {
    return disposition == TimeStatisticsReadAdmissionDispositionV3::admitted;
  }
};
// Pure admission classifier.  The receiving owner remains responsible for
// producing authoritative security/privacy facts and enforcing the decision;
// this API only fixes their precedence ahead of datatype format decoding.
TimeStatisticsReadAdmissionResultV3 AdmitTimeStatisticsReadV3(
    const std::shared_ptr<const TimeValidatedProfileHandleV3>& profile,
    std::span<const byte> encoded,
    const TimeStatisticsReceivingFactsV3& current,
    const TimeExecutionControlV3& control = {}) noexcept;

struct TimeBackupTupleViewV3 {
  const TimeValidatedProfileHandleV3* profile_handle = nullptr;
  TimeValueStateV3 state = TimeValueStateV3::value;
  u64 nanoseconds_since_midnight = 0;
};
struct TimeBackupTupleViewResultV3 {
  Status status;
  TimeDiagnosticFactV3 diagnostic;
  TimeBackupTupleViewV3 tuple;
  bool ok() const noexcept { return status.ok(); }
};
TimeBytesResultV3 EncodeTimeBackupTupleV3(
    const TimeOwnedValueV3& value,
    const TimeExecutionControlV3& control = {}) noexcept;
TimeBackupTupleViewResultV3 DecodeTimeBackupTupleNoAllocV3(
    const TimeValidatedProfileHandleV3& profile_handle, bool null_allowed,
    std::span<const byte> encoded,
    const TimeExecutionControlV3& control = {}) noexcept;

enum class TimeProtectionCellV3 : std::uint8_t {
  canonical_plain, inner_compression, inner_encryption, outer_compression,
  outer_authenticated_protection, outer_compress_then_protect,
  outer_protect_then_compress, page_or_filespace_crypto, transport_tls,
  protected_index,
};
struct TimeProtectionResolutionV3 {
  TimeProtectionCellV3 cell = TimeProtectionCellV3::canonical_plain;
  bool datatype_admitted = false;
  bool receiving_owner_handoff = false;
  std::string_view disposition;
  std::string_view diagnostic;
};
TimeProtectionResolutionV3 ResolveTimeProtectionV3(
    TimeProtectionCellV3 cell) noexcept;

enum class TimeWireLaneV3 : std::uint8_t {
  native_sbwp, parser_server_ipc, canonical_sblr, apache_ignite, cassandra,
  clickhouse, cockroachdb, dolt, duckdb, firebird, foundationdb, immudb,
  influxdb, mariadb, milvus, mongodb, mysql, neo4j, opensearch, postgresql,
  redis, sqlite, tidb, tikv, vitess, xtdb, yugabytedb,
};
struct TimeWireLaneResolutionV3 {
  TimeWireLaneV3 lane = TimeWireLaneV3::native_sbwp;
  bool admitted = false;
  bool component_mapping_complete = false;
  bool receiving_owner_handoff = true;
  std::string_view disposition;
  std::string_view diagnostic = "CTI.TRANSPORT.UNSUPPORTED";
};
TimeWireLaneResolutionV3 ResolveTimeWireLaneV3(TimeWireLaneV3 lane) noexcept;

enum class TimeDiagnosticAxisV3 : std::uint8_t {
  input_parse, descriptor_codec, storage_read_write, cast_invalid,
  cast_range_loss, operation_invalid, bounds_overflow_underflow,
  resource_cancellation, compression_corruption, encryption_auth_key,
  wire_decode_encode, index_key, domain_validation, recovery_corruption,
  statistics_read,
};
struct TimeDiagnosticRouteV3 {
  TimeDiagnosticAxisV3 axis = TimeDiagnosticAxisV3::descriptor_codec;
  std::string_view code;
  platform::Uuid diagnostic_uuid{};
  std::string_view ordered_parameter_schema;
  std::string_view redaction;
};
std::span<const TimeDiagnosticRouteV3> TimeDiagnosticRoutesV3() noexcept;

enum class TimeDiagnosticRouteScalarTypeV3 : std::uint8_t {
  uuid,
  u32,
  closed_enum,
};
struct TimeDiagnosticRouteParameterV3 {
  std::string_view token;
  TimeDiagnosticRouteScalarTypeV3 scalar_type =
      TimeDiagnosticRouteScalarTypeV3::uuid;
  platform::Uuid uuid_value{};
  // Stored in u64 so callers can present and deterministically reject a value
  // above UINT32_MAX without narrowing it before validation.
  u64 unsigned_value = 0;
  std::string_view enum_value;
  friend bool operator==(const TimeDiagnosticRouteParameterV3&,
                         const TimeDiagnosticRouteParameterV3&) = default;
};
struct TimeDiagnosticRouteExecutionRequestV3 {
  TimeDiagnosticAxisV3 axis = TimeDiagnosticAxisV3::descriptor_codec;
  std::string_view diagnostic_code;
  platform::Uuid diagnostic_uuid{};
  std::span<const TimeDiagnosticRouteParameterV3> ordered_payload;
  std::string_view redaction;
};
enum class TimeDiagnosticRouteExecutionDispositionV3 : std::uint8_t {
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
struct TimeDiagnosticRouteExecutionFactV3 {
  TimeDiagnosticRouteExecutionDispositionV3 disposition =
      TimeDiagnosticRouteExecutionDispositionV3::unknown_route;
  TimeDiagnosticAxisV3 axis = TimeDiagnosticAxisV3::descriptor_codec;
  std::string_view diagnostic_code;
  platform::Uuid diagnostic_uuid{};
  std::array<TimeDiagnosticRouteParameterV3, 5> ordered_payload{};
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
    return disposition == TimeDiagnosticRouteExecutionDispositionV3::admitted;
  }
};
TimeDiagnosticRouteExecutionFactV3 ExecuteTimeDiagnosticRouteV3(
    const TimeDiagnosticRouteExecutionRequestV3& request) noexcept;

enum class TimeDiagnosticPrecedenceScopeV3 : std::uint8_t {
  global,
  cast,
  operation,
  text_fields,
  intrinsic_operation_gates,
};
struct TimeDiagnosticFormatOrderV3 {
  std::string_view format_magic;
  std::string_view authority_file;
};
struct TimeDiagnosticCombinedFaultV3 {
  std::string_view supplied_text;
  std::string_view selected_reason;
};
struct TimeDiagnosticClassificationRuleV3 {
  std::string_view scope;
  std::string_view rule;
};
std::span<const std::string_view> TimeDiagnosticPrecedenceV3(
    TimeDiagnosticPrecedenceScopeV3 scope) noexcept;
std::span<const TimeDiagnosticFormatOrderV3>
TimeDiagnosticFormatOrdersV3() noexcept;
std::span<const TimeDiagnosticCombinedFaultV3>
TimeDiagnosticTextCombinedFaultsV3() noexcept;
std::span<const TimeDiagnosticClassificationRuleV3>
TimeDiagnosticClassificationRulesV3() noexcept;
std::string_view TimeDiagnosticSelectionRuleV3() noexcept;

struct TimeDiagnosticRouteKeyV3 {
  TimeDiagnosticAxisV3 axis = TimeDiagnosticAxisV3::descriptor_codec;
  platform::Uuid diagnostic_uuid{};
};
struct TimeDiagnosticMetricRouteV3 {
  TimeDiagnosticAxisV3 diagnostic_family =
      TimeDiagnosticAxisV3::descriptor_codec;
  platform::Uuid metric_evidence_type_uuid{};
  std::string_view metric_name = "sb_time_diagnostic_family_total";
  std::string_view rule =
      "after final primary diagnostic selection exactly once";
};
enum class TimeDiagnosticRouteFactDispositionV3 : std::uint8_t {
  admitted,
  missing,
  unknown,
  wrong_type,
  reordered,
  duplicate,
};
struct TimeDiagnosticMetricRouteResultV3 {
  TimeDiagnosticRouteFactDispositionV3 disposition =
      TimeDiagnosticRouteFactDispositionV3::unknown;
  TimeDiagnosticMetricRouteV3 route;
  bool metric_mutation_admitted = false;
};
std::span<const TimeDiagnosticMetricRouteV3>
TimeDiagnosticMetricRoutesV3() noexcept;
TimeDiagnosticMetricRouteResultV3 ResolveTimeDiagnosticMetricRouteV3(
    const TimeDiagnosticRouteKeyV3& key) noexcept;
TimeDiagnosticRouteFactDispositionV3 ValidateTimeDiagnosticMetricRouteTableV3(
    std::span<const TimeDiagnosticMetricRouteV3> supplied,
    bool scalar_types_valid = true) noexcept;

enum class TimeMetricEvidenceTypeV3 : std::uint8_t {
  descriptor_admissions, invalid_literals, range_refusals,
  operation_attempts, operation_success, operation_refusals, cast_attempts,
  cast_success, index_admission_refusals, statistics_stale,
  reference_mapping_misses, transport_refusals, serialization_refusals,
  protection_refusals, merge_manual_review, resource_budget_refusals,
  cancellations, decode_corruption, diagnostic_family,
  precision_loss_refusals, leap_second_refusals,
};
struct TimeMetricEvidenceTypeDescriptorV3 {
  TimeMetricEvidenceTypeV3 type = TimeMetricEvidenceTypeV3::descriptor_admissions;
  platform::Uuid evidence_type_uuid{};
  u64 generation = 0;
  std::string_view metric_name_metadata;
  std::string_view evidence_producer;
  std::string_view evidence_record;
  std::string_view manager_consumer;
  platform::Uuid source_event_uuid{};
  std::string_view closed_label_schema;
  std::string_view cardinality_bound;
  std::string_view trigger_phase;
  std::string_view trigger_condition;
  std::string_view update_rule;
  std::string_view failure_behavior;
  std::string_view accepted_final_outcome;
};
std::span<const TimeMetricEvidenceTypeDescriptorV3>
TimeMetricEvidenceTypesV3() noexcept;

struct TimeMetricLabelMemberV3 {
  std::string_view name;
  std::string_view value;
};
struct TimeMetricClosedLabelTupleV3 {
  TimeMetricEvidenceTypeV3 type = TimeMetricEvidenceTypeV3::descriptor_admissions;
  std::array<TimeMetricLabelMemberV3, 2> members{};
  std::uint8_t member_count = 0;
};
std::span<const TimeMetricClosedLabelTupleV3>
TimeMetricClosedLabelTuplesV3() noexcept;

struct TimeMetricEvidenceEnvelopeIdentityV3 {
  platform::Uuid schema_uuid{};
  u64 generation = 0;
};
TimeMetricEvidenceEnvelopeIdentityV3 TimeMetricEvidenceEnvelopeIdentityV3Value()
    noexcept;

struct TimeMetricEvidenceEnvelopeV3 {
  TimeMetricEvidenceEnvelopeIdentityV3 envelope_identity;
  platform::Uuid evidence_type_uuid{};
  platform::Uuid source_event_uuid{};
  platform::Uuid database_uuid{};
  platform::Uuid node_uuid{};
  u64 process_epoch = 0;
  u64 event_sequence = 0;
  u64 monotonic_timestamp_ns = 0;
  u64 counter_delta = 0;
  std::span<const TimeMetricLabelMemberV3> closed_label_tuple;
  bool outcome_committed = false;
  bool all_required_fields_present = true;
  bool scalar_types_valid = true;
};
struct TimeMetricEvidenceValidationContextV3 {
  u64 current_process_epoch = 0;
  u64 current_counter_value = 0;
};
enum class TimeMetricEvidenceDispositionV3 : std::uint8_t {
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
struct TimeMetricEvidenceIdempotencyFactV3 {
  u64 process_epoch = 0;
  u64 event_sequence = 0;
  platform::Uuid source_event_uuid{};
  platform::Uuid evidence_type_uuid{};
};
struct TimeMetricEvidenceValidationResultV3 {
  TimeMetricEvidenceDispositionV3 disposition =
      TimeMetricEvidenceDispositionV3::missing_field;
  TimeMetricEvidenceTypeV3 type =
      TimeMetricEvidenceTypeV3::descriptor_admissions;
  TimeMetricEvidenceIdempotencyFactV3 idempotency;
  bool receiving_owner_update_admitted = false;
  bool datatype_outcome_unchanged = true;
};
TimeMetricEvidenceValidationResultV3 ValidateTimeMetricEvidenceV3(
    const TimeMetricEvidenceEnvelopeV3& evidence,
    const TimeMetricEvidenceValidationContextV3& context) noexcept;

enum class TimeMetricSameSourceDispositionV3 : std::uint8_t {
  independent_sources,
  admitted_ordered_pair,
  duplicate_type_refused,
  unregistered_pair_refused,
  reversed_order_refused,
  stale_process_epoch_refused,
};
struct TimeMetricSameSourceFactV3 {
  TimeMetricSameSourceDispositionV3 disposition =
      TimeMetricSameSourceDispositionV3::unregistered_pair_refused;
  bool second_emission_admitted = false;
  bool receiving_owner_update_admitted = false;
  bool datatype_outcome_unchanged = true;
};
TimeMetricSameSourceFactV3 ClassifyTimeMetricSameSourceEmissionV3(
    const TimeMetricEvidenceIdempotencyFactV3& first,
    const TimeMetricEvidenceIdempotencyFactV3& second) noexcept;

}  // namespace scratchbird::core::datatypes

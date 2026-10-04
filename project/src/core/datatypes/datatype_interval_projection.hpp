// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "datatype_interval.hpp"

#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

namespace scratchbird::core::datatypes {

enum class IntervalIndexFamilyV3 : std::uint8_t {
  aggregate_sketch, bitmap, brin_like, btree, columnar_zone_map,
  covering_included, document_path, expression, full_text, graph, hash,
  partial_filtered, range_exclusion, spatial, temporary_work, vector_ann,
};

inline constexpr u32 kIntervalCoveringNullBytesV3 = 1;
inline constexpr u32 kIntervalCoveringValueBytesV3 = 17;
enum class IntervalIndexDispositionV3 : std::uint8_t {
  admitted_with_recheck = 0, admitted_payload_only = 1,
  conditional = 2, not_applicable = 3,
};
enum class IntervalProjectionKindV3 : std::uint8_t {
  none = 0, equality_hash = 1, covering_value = 2,
  conditional_result = 3, conditional_underlying = 4,
};
enum class IntervalProjectionModeV3 : std::uint8_t {
  unspecified = 0, equality_hash = 1, payload = 2,
};
struct IntervalIndexResolutionV3 {
  IntervalIndexFamilyV3 family = IntervalIndexFamilyV3::btree;
  platform::Uuid compatibility_uuid{};
  u64 compatibility_generation = 0;
  IntervalIndexDispositionV3 disposition =
      IntervalIndexDispositionV3::not_applicable;
  IntervalProjectionKindV3 projection = IntervalProjectionKindV3::none;
  bool exact_state_component_recheck_required = false;
  bool receiving_owner_resolution_required = false;
};
struct IntervalIndexCompatibilityIdentityV3 {
  IntervalIndexFamilyV3 family = IntervalIndexFamilyV3::btree;
  platform::Uuid compatibility_uuid{};
  u64 compatibility_generation = 0;
};
struct IntervalIndexAdmissionResultV3 {
  bool admitted = false;
  IntervalIndexResolutionV3 resolution;
  std::string_view diagnostic = "OPTIMIZER.INDEX_COMPATIBILITY_MISSING";
};
enum class IntervalIndexPredicateOperationV3 : std::uint8_t {
  equality = 0, in = 1, is_null = 2, include_payload = 3, range = 4,
  order = 5, prune_range = 6, overlap = 7, exclusion = 8, unsupported = 9,
};
enum class IntervalIndexPredicateFactV3 : std::uint8_t {
  exact_no_recheck_after_decode_reencode = 0,
  requires_exact_state_component_recheck_never_final_match = 1,
  prune_only_mandatory_source_row_predicate_recheck_never_final_match = 2,
  refused = 3,
};
struct IntervalIndexPredicateRequestV3 {
  IntervalIndexCompatibilityIdentityV3 requested;
  IntervalIndexPredicateOperationV3 operation =
      IntervalIndexPredicateOperationV3::unsupported;
  const IntervalIndexCompatibilityIdentityV3* selected_conditional = nullptr;
};
struct IntervalIndexPredicateResolutionV3 {
  bool admitted = false;
  IntervalIndexPredicateFactV3 fact = IntervalIndexPredicateFactV3::refused;
  std::string_view diagnostic;
  IntervalIndexResolutionV3 requested_resolution;
  IntervalIndexResolutionV3 effective_resolution;
  bool predicate_owner_handoff_preserved = false;
};
enum class IntervalIndexCandidateDispositionV3 : std::uint8_t {
  not_a_candidate = 0, exact_match = 1,
  exact_state_component_recheck_required = 2,
  source_row_predicate_recheck_required = 3,
  rejected_after_required_recheck = 4,
};
struct IntervalIndexCandidateFactsV3 {
  IntervalIndexCandidateDispositionV3 disposition =
      IntervalIndexCandidateDispositionV3::not_a_candidate;
  bool final_match = false;
  bool exact_state_component_recheck_required = false;
  bool source_row_predicate_recheck_required = false;
};
struct IntervalIndexProjectionRequestV3 {
  IntervalIndexCompatibilityIdentityV3 compatibility;
  IntervalProjectionModeV3 mode = IntervalProjectionModeV3::unspecified;
  const IntervalOwnedValueV3* value = nullptr;
  const IntervalIndexResolutionV3* selected_conditional_family = nullptr;
  u64 provider_max_key_bytes = ~u64{0};
  IntervalExecutionControlV3 control;
};
struct IntervalIndexProjectionResultV3 {
  Status status;
  IntervalDiagnosticFactV3 diagnostic;
  IntervalIndexResolutionV3 requested_resolution;
  IntervalIndexResolutionV3 resolution;
  std::vector<byte> bytes;
  bool owner_fact_only = false;
  bool ok() const noexcept { return status.ok(); }
};

IntervalIndexCompatibilityIdentityV3 LookupIntervalIndexCompatibilityIdentityV3(
    IntervalIndexFamilyV3 family) noexcept;
IntervalIndexAdmissionResultV3 AdmitIntervalIndexCompatibilityV3(
    const IntervalIndexCompatibilityIdentityV3& supplied) noexcept;
IntervalIndexResolutionV3 ResolveIntervalIndexFamilyV3(
    IntervalIndexFamilyV3 family) noexcept;
IntervalIndexPredicateResolutionV3 ResolveIntervalIndexPredicateV3(
    const IntervalIndexPredicateRequestV3& request) noexcept;
IntervalIndexCandidateFactsV3 ClassifyIntervalIndexCandidateV3(
    IntervalIndexPredicateFactV3 predicate_fact,
    bool projection_candidate_equal,
    bool exact_state_component_equal,
    bool source_row_predicate_equal) noexcept;
IntervalIndexProjectionResultV3 ProjectIntervalIndexValueV3(
    const IntervalIndexProjectionRequestV3& request) noexcept;
struct IntervalCoveringValueViewV3 {
  const IntervalValidatedProfileHandleV3* profile_handle = nullptr;
  IntervalValueStateV3 state = IntervalValueStateV3::value;
  std::int32_t months = 0;
  std::int32_t civil_days = 0;
  std::int64_t fixed_nanoseconds = 0;
};
struct IntervalCoveringValueViewResultV3 {
  Status status;
  IntervalDiagnosticFactV3 diagnostic;
  IntervalCoveringValueViewV3 value;
  bool ok() const noexcept { return status.ok(); }
};
IntervalBytesResultV3 EncodeIntervalCoveringValueV3(
    const IntervalOwnedValueV3& value,
    const IntervalExecutionControlV3& control = {}) noexcept;
IntervalCoveringValueViewResultV3 DecodeIntervalCoveringValueNoAllocV3(
    const IntervalValidatedProfileHandleV3& profile_handle,
    bool null_allowed, std::span<const byte> encoded,
    const IntervalExecutionControlV3& control = {}) noexcept;

inline constexpr u32 kIntervalStatisticsHeaderBytesV3 = 448;
inline constexpr u32 kIntervalStatisticsMcvRecordBytesV3 = 48;
inline constexpr u32 kIntervalStatisticsMaximumMcvRecordsV3 = 64;
inline constexpr u32 kIntervalStatisticsMaximumBytesV3 = 3520;

struct IntervalStatisticsMcvRecordV3 {
  std::array<byte, 32> value_hash{};
  u64 frequency = 0;
  u32 rank = 0;
  u32 flags = 0;
  bool operator==(const IntervalStatisticsMcvRecordV3&) const = default;
};

// Immutable authenticated facts supplied and owned by the receiving
// statistics owner. Shared ownership pins lifetime only; it does not confer
// authorization, privacy, population, or epoch authority.
struct IntervalStatisticsProviderEvidenceHandleV3 {
  platform::Uuid provider_evidence_uuid{};
  platform::Uuid source_object_uuid{};
  bool authenticated_population = false;
  bool source_authorized = false;
  bool privacy_admitted = false;
  u64 schema_epoch = 0;
  u64 collection_epoch = 0;
  u64 security_epoch = 0;
  u64 row_count = 0;
  u64 null_count = 0;
  u64 value_count = 0;
  u64 equality_distinct_estimate = 0;
  std::vector<IntervalStatisticsMcvRecordV3> mcv;
};

struct IntervalStatisticsProjectionV3 {
  std::shared_ptr<const IntervalValidatedProfileHandleV3> profile;
  std::shared_ptr<const IntervalStatisticsProviderEvidenceHandleV3>
      provider_evidence;
  platform::Uuid statistics_snapshot_uuid{};
  platform::Uuid source_object_uuid{};
  u64 schema_epoch = 0;
  u64 collection_epoch = 0;
  u64 security_epoch = 0;
  u64 row_count = 0;
  u64 null_count = 0;
  u64 value_count = 0;
  u64 equality_distinct_estimate = 0;
  std::span<const IntervalStatisticsMcvRecordV3> mcv;
};

struct IntervalDecodedStatisticsV3 {
  std::shared_ptr<const IntervalValidatedProfileHandleV3> profile;
  platform::Uuid statistics_snapshot_uuid{};
  platform::Uuid source_object_uuid{};
  platform::Uuid provider_evidence_uuid{};
  u64 schema_epoch = 0;
  u64 collection_epoch = 0;
  u64 security_epoch = 0;
  u64 row_count = 0;
  u64 null_count = 0;
  u64 value_count = 0;
  u64 equality_distinct_estimate = 0;
  std::vector<IntervalStatisticsMcvRecordV3> mcv;
};

struct IntervalStatisticsReceivingFactsV3 {
  bool security_visible = false;
  bool privacy_admitted = false;
  platform::Uuid source_object_uuid{};
  platform::Uuid provider_evidence_uuid{};
  u64 schema_epoch = 0;
  u64 collection_epoch = 0;
  u64 security_epoch = 0;
};

enum class IntervalStatisticsReceivingDispositionV3 : std::uint8_t {
  admitted = 0,
  security_denied = 1,
  privacy_denied = 2,
  schema_epoch_mismatch = 3,
  collection_epoch_mismatch = 4,
  security_epoch_mismatch = 5,
  provider_evidence_mismatch = 6,
};

struct IntervalStatisticsDecodeResultV3 {
  Status status;
  IntervalDiagnosticFactV3 diagnostic;
  IntervalDecodedStatisticsV3 statistics;
  u32 mcv_records_read = 0;
  bool ok() const noexcept { return status.ok(); }
};

enum class IntervalStatisticsReadAdmissionDispositionV3 : std::uint8_t {
  admitted = 0,
  security_denied = 1,
  privacy_denied = 2,
  profile_refused = 3,
  format_refused = 4,
  resource_refused = 5,
  cancelled = 6,
  schema_epoch_mismatch = 7,
  collection_epoch_mismatch = 8,
  security_epoch_mismatch = 9,
  provider_evidence_mismatch = 10,
};

struct IntervalStatisticsReadAdmissionResultV3 {
  IntervalStatisticsReadAdmissionDispositionV3 disposition =
      IntervalStatisticsReadAdmissionDispositionV3::format_refused;
  IntervalDiagnosticFactV3 diagnostic;
  IntervalDecodedStatisticsV3 statistics;
  bool decode_attempted = false;
  u32 mcv_records_read = 0;
  bool receiving_owner_enforcement_required = true;
  bool datatype_outcome_unchanged = true;
  bool metric_outcome_unchanged = true;
  bool ok() const noexcept {
    return disposition ==
           IntervalStatisticsReadAdmissionDispositionV3::admitted;
  }
};

IntervalBytesResultV3 EncodeIntervalStatisticsProjectionV3(
    const IntervalStatisticsProjectionV3& projection,
    const IntervalExecutionControlV3& control = {}) noexcept;

// Public decode always receives authenticated receiving-owner facts. There is
// no public privacy-blind payload decoder.
IntervalStatisticsDecodeResultV3 DecodeIntervalStatisticsProjectionV3(
    const std::shared_ptr<const IntervalValidatedProfileHandleV3>& profile,
    std::span<const byte> encoded,
    const IntervalStatisticsReceivingFactsV3& current,
    const IntervalExecutionControlV3& control = {}) noexcept;

IntervalStatisticsReceivingDispositionV3
ResolveIntervalStatisticsReceivingFactsV3(
    const IntervalDecodedStatisticsV3& decoded,
    const IntervalStatisticsReceivingFactsV3& current) noexcept;

IntervalStatisticsReadAdmissionResultV3 AdmitIntervalStatisticsReadV3(
    const std::shared_ptr<const IntervalValidatedProfileHandleV3>& profile,
    std::span<const byte> encoded,
    const IntervalStatisticsReceivingFactsV3& current,
    const IntervalExecutionControlV3& control = {}) noexcept;

struct IntervalBackupTupleViewV3 {
  const IntervalValidatedProfileHandleV3* profile_handle = nullptr;
  IntervalValueStateV3 state = IntervalValueStateV3::value;
  std::int32_t months = 0;
  std::int32_t civil_days = 0;
  std::int64_t fixed_nanoseconds = 0;
};
struct IntervalBackupTupleViewResultV3 {
  Status status;
  IntervalDiagnosticFactV3 diagnostic;
  IntervalBackupTupleViewV3 tuple;
  bool ok() const noexcept { return status.ok(); }
};
IntervalBytesResultV3 EncodeIntervalBackupTupleV3(
    const IntervalOwnedValueV3& value,
    const IntervalExecutionControlV3& control = {}) noexcept;
IntervalBackupTupleViewResultV3 DecodeIntervalBackupTupleNoAllocV3(
    const IntervalValidatedProfileHandleV3& profile_handle,
    bool null_allowed, std::span<const byte> encoded,
    const IntervalExecutionControlV3& control = {}) noexcept;

enum class IntervalProtectionCellV3 : std::uint8_t {
  canonical_plain, inner_compression, inner_encryption, outer_compression,
  outer_authenticated_protection, outer_compress_then_protect,
  outer_protect_then_compress, page_or_filespace_crypto, transport_tls,
  protected_index,
};
struct IntervalProtectionResolutionV3 {
  IntervalProtectionCellV3 cell = IntervalProtectionCellV3::canonical_plain;
  bool datatype_admitted = false;
  bool receiving_owner_handoff = false;
  std::string_view disposition;
  std::string_view diagnostic;
};
IntervalProtectionResolutionV3 ResolveIntervalProtectionV3(
    IntervalProtectionCellV3 cell) noexcept;

enum class IntervalProtectionModeV3 : std::uint8_t {
  clear, compressed, encrypted, compressed_encrypted,
};
enum class IntervalProtectionIndexModeV3 : std::uint8_t {
  not_indexed, indexed,
};
struct IntervalProtectionCombinationResolutionV3 {
  IntervalProtectionModeV3 storage_mode = IntervalProtectionModeV3::clear;
  IntervalProtectionModeV3 stream_mode = IntervalProtectionModeV3::clear;
  IntervalProtectionIndexModeV3 index_mode =
      IntervalProtectionIndexModeV3::not_indexed;
  bool admitted = false;
  std::string_view disposition;
  std::string_view diagnostic;
};
IntervalProtectionCombinationResolutionV3
ResolveIntervalProtectionCombinationV3(
    IntervalProtectionModeV3 storage_mode,
    IntervalProtectionModeV3 stream_mode,
    IntervalProtectionIndexModeV3 index_mode) noexcept;
IntervalProtectionCombinationResolutionV3
AdmitIntervalProtectionCombinationV3(
    const IntervalValidatedProfileHandleV3& profile,
    IntervalProtectionModeV3 storage_mode,
    IntervalProtectionModeV3 stream_mode,
    IntervalProtectionIndexModeV3 index_mode) noexcept;

enum class IntervalWireLaneV3 : std::uint8_t {
  native_sbwp = 0, parser_server_ipc = 1, canonical_sblr = 2,
  apache_ignite = 3, cassandra = 4, clickhouse = 5, cockroachdb = 6,
  dolt = 7, duckdb = 8, firebird = 9, foundationdb = 10, immudb = 11,
  influxdb = 12, mariadb = 13, milvus = 14, mongodb = 15, mysql = 16,
  neo4j = 17, opensearch = 18, postgresql = 19, redis = 20,
  sqlite = 21, tidb = 22, tikv = 23, vitess = 24, xtdb = 25,
  yugabytedb = 26,
};
struct IntervalWireLaneResolutionV3 {
  IntervalWireLaneV3 lane = IntervalWireLaneV3::native_sbwp;
  bool admitted = false;
  bool component_mapping_complete = false;
  bool receiving_owner_handoff = true;
  std::string_view disposition;
  std::string_view diagnostic = "CTI.TRANSPORT.UNSUPPORTED";
};
IntervalWireLaneResolutionV3 ResolveIntervalWireLaneV3(
    IntervalWireLaneV3 lane) noexcept;

enum class IntervalDiagnosticAxisV3 : std::uint8_t {
  input_parse = 0,
  descriptor_codec = 1,
  storage_read_write = 2,
  cast_invalid = 3,
  cast_range_loss = 4,
  operation_invalid = 5,
  bounds_overflow_underflow = 6,
  resource_cancellation = 7,
  compression_corruption = 8,
  encryption_auth_key = 9,
  wire_decode_encode = 10,
  index_key = 11,
  domain_validation = 12,
  recovery_corruption = 13,
  statistics_read = 14,
};

enum class IntervalMetricEvidenceTypeV3 : std::uint8_t {
  validation_failure_total = 0,
  cast_refusal_total = 1,
  arithmetic_overflow_total = 2,
  ordering_refusal_total = 3,
  temporal_application_refusal_total = 4,
};

enum class IntervalMetricOperationV3 : std::uint8_t {
  validate = 0,
  parse = 1,
  cast = 2,
  construct = 3,
  decompose = 4,
  negate = 5,
  add = 6,
  subtract = 7,
  equality = 8,
  hash = 9,
  order = 10,
  temporal_apply = 11,
  persistence = 12,
  wire = 13,
  protection = 14,
};

enum class IntervalMetricSourceFamilyV3 : std::uint8_t {
  native_value = 0,
  native_character = 1,
  native_component = 2,
  native_outer_lane = 3,
  donor_lane = 4,
  none = 5,
};

enum class IntervalMetricStateV3 : std::uint8_t {
  present = 0,
  sql_null = 1,
  invalid_state = 2,
  unavailable = 3,
};

enum class IntervalMetricReasonV3 : std::uint8_t {
  success = 0,
  descriptor_invalid = 1,
  subtype_mismatch = 2,
  invalid_literal = 3,
  canonical_encoding_invalid = 4,
  range_exceeded = 5,
  cast_forbidden = 6,
  ordering_refused = 7,
  calendar_operation_refused = 8,
  donor_mapping_missing = 9,
  protection_unsupported = 10,
  transport_unsupported = 11,
  resource_failure = 12,
  cancelled = 13,
  validation_failure = 14,
};

struct IntervalMetricClosedLabelTupleV3 {
  IntervalMetricOperationV3 operation = IntervalMetricOperationV3::validate;
  IntervalMetricSourceFamilyV3 source_family =
      IntervalMetricSourceFamilyV3::none;
  IntervalMetricStateV3 state = IntervalMetricStateV3::unavailable;
  IntervalMetricReasonV3 reason =
      IntervalMetricReasonV3::validation_failure;
  bool operator==(const IntervalMetricClosedLabelTupleV3&) const = default;
};

struct IntervalMetricEvidenceTypeDescriptorV3 {
  IntervalMetricEvidenceTypeV3 type =
      IntervalMetricEvidenceTypeV3::validation_failure_total;
  platform::Uuid evidence_type_uuid{};
  platform::Uuid metric_uuid{};
  u64 generation = 0;
  std::string_view canonical_name;
  std::string_view kind;
  std::string_view producer;
  std::string_view consumer_maintainer;
  std::string_view trigger_commit_point;
  std::string_view stored_aggregate;
  std::string_view unit;
  std::string_view closed_label_schema;
  std::string_view cardinality;
  std::string_view update_responsibility;
  std::string_view emission;
  std::string_view update_owner;
};

struct IntervalDiagnosticRouteKeyV3 {
  IntervalDiagnosticAxisV3 axis = IntervalDiagnosticAxisV3::descriptor_codec;
  platform::Uuid diagnostic_uuid{};
  bool operator==(const IntervalDiagnosticRouteKeyV3&) const = default;
};

struct IntervalDiagnosticMetricRouteV3 {
  IntervalDiagnosticRouteKeyV3 key;
  IntervalMetricEvidenceTypeV3 type =
      IntervalMetricEvidenceTypeV3::validation_failure_total;
  platform::Uuid metric_uuid{};
  std::string_view canonical_name;
  std::uint16_t permitted_operation_mask = 0;
  std::uint8_t permitted_source_family_mask = 0;
  std::uint8_t permitted_state_mask = 0;
  IntervalMetricReasonV3 reason =
      IntervalMetricReasonV3::validation_failure;
};

enum class IntervalDiagnosticMetricRouteDispositionV3 : std::uint8_t {
  admitted = 0,
  missing = 1,
  unknown = 2,
  wrong_type = 3,
  reordered = 4,
  duplicate = 5,
};

struct IntervalDiagnosticMetricRouteResultV3 {
  IntervalDiagnosticMetricRouteDispositionV3 disposition =
      IntervalDiagnosticMetricRouteDispositionV3::unknown;
  IntervalDiagnosticMetricRouteV3 route;
  bool evidence_type_selected = false;
  bool metric_mutation_admitted = false;
};

std::span<const IntervalMetricEvidenceTypeDescriptorV3>
IntervalMetricEvidenceTypesV3() noexcept;

std::span<const IntervalDiagnosticMetricRouteV3>
IntervalDiagnosticMetricRoutesV3() noexcept;

IntervalDiagnosticMetricRouteResultV3 ResolveIntervalDiagnosticMetricRouteV3(
    const IntervalDiagnosticRouteKeyV3& key) noexcept;

IntervalDiagnosticMetricRouteDispositionV3
ValidateIntervalDiagnosticMetricRouteTableV3(
    std::span<const IntervalDiagnosticMetricRouteV3> supplied,
    bool scalar_types_valid = true) noexcept;

bool IsIntervalMetricLabelTupleAdmittedV3(
    const IntervalDiagnosticMetricRouteV3& route,
    const IntervalMetricClosedLabelTupleV3& labels) noexcept;

struct IntervalMetricEvidenceV3 {
  platform::Uuid evidence_type_uuid{};
  platform::Uuid metric_uuid{};
  platform::Uuid source_event_uuid{};
  platform::Uuid database_uuid{};
  platform::Uuid node_uuid{};
  u64 process_epoch = 0;
  u64 event_sequence = 0;
  u64 monotonic_timestamp_ns = 0;
  u64 counter_delta = 0;
  IntervalMetricClosedLabelTupleV3 labels;
  bool outcome_committed = false;
  bool all_required_fields_present = true;
  bool scalar_types_valid = true;
  bool operator==(const IntervalMetricEvidenceV3&) const = default;
};

struct IntervalMetricEvidenceProductionRequestV3 {
  IntervalDiagnosticRouteKeyV3 selected_final_diagnostic;
  platform::Uuid source_event_uuid{};
  platform::Uuid database_uuid{};
  platform::Uuid node_uuid{};
  u64 process_epoch = 0;
  u64 event_sequence = 0;
  u64 monotonic_timestamp_ns = 0;
  IntervalMetricOperationV3 operation = IntervalMetricOperationV3::validate;
  IntervalMetricSourceFamilyV3 source_family =
      IntervalMetricSourceFamilyV3::none;
  IntervalMetricStateV3 state = IntervalMetricStateV3::unavailable;
};

enum class IntervalMetricEvidenceProductionDispositionV3 : std::uint8_t {
  admitted = 0,
  profile_refused = 1,
  missing_field = 2,
  route_mismatch = 3,
  undeclared_label_tuple = 4,
};

struct IntervalMetricEvidenceProductionResultV3 {
  IntervalMetricEvidenceProductionDispositionV3 disposition =
      IntervalMetricEvidenceProductionDispositionV3::missing_field;
  IntervalMetricEvidenceV3 evidence;
  bool receiving_owner_evidence_handoff_ready = false;
  bool metric_mutation_admitted = false;
  bool datatype_outcome_unchanged = true;
  bool metric_outcome_unchanged = true;
  bool ok() const noexcept {
    return disposition ==
           IntervalMetricEvidenceProductionDispositionV3::admitted;
  }
};

IntervalMetricEvidenceProductionResultV3 ProduceIntervalMetricEvidenceV3(
    const IntervalValidatedProfileHandleV3& profile,
    const IntervalMetricEvidenceProductionRequestV3& request) noexcept;

struct IntervalMetricEvidenceValidationContextV3 {
  u64 current_process_epoch = 0;
  u64 current_counter_value = 0;
};

enum class IntervalMetricEvidenceDispositionV3 : std::uint8_t {
  admitted = 0,
  profile_refused = 1,
  missing_field = 2,
  wrong_scalar = 3,
  unknown_evidence_type = 4,
  metric_identity_mismatch = 5,
  route_mismatch = 6,
  undeclared_label_tuple = 7,
  outcome_not_committed = 8,
  invalid_counter_delta = 9,
  stale_process_epoch = 10,
  counter_overflow = 11,
};

struct IntervalMetricEvidenceIdempotencyFactV3 {
  u64 process_epoch = 0;
  u64 event_sequence = 0;
  platform::Uuid source_event_uuid{};
  platform::Uuid evidence_type_uuid{};
};

struct IntervalMetricEvidenceValidationResultV3 {
  IntervalMetricEvidenceDispositionV3 disposition =
      IntervalMetricEvidenceDispositionV3::missing_field;
  IntervalMetricEvidenceTypeV3 type =
      IntervalMetricEvidenceTypeV3::validation_failure_total;
  IntervalMetricEvidenceIdempotencyFactV3 idempotency;
  bool receiving_owner_update_admitted = false;
  bool datatype_outcome_unchanged = true;
  bool metric_outcome_unchanged = true;
  bool ok() const noexcept {
    return disposition == IntervalMetricEvidenceDispositionV3::admitted;
  }
};

IntervalMetricEvidenceValidationResultV3 ValidateIntervalMetricEvidenceV3(
    const IntervalValidatedProfileHandleV3& profile,
    const IntervalDiagnosticRouteKeyV3& selected_final_diagnostic,
    const IntervalMetricEvidenceV3& evidence,
    const IntervalMetricEvidenceValidationContextV3& context) noexcept;

enum class IntervalMetricReplayDispositionV3 : std::uint8_t {
  independent_source_admitted = 0,
  exact_duplicate_idempotent = 1,
  same_identity_payload_conflict = 2,
  distinct_type_same_source_refused = 3,
  replay_or_nonmonotonic_sequence_refused = 4,
  stale_process_epoch_refused = 5,
};

struct IntervalMetricReplayFactV3 {
  IntervalMetricReplayDispositionV3 disposition =
      IntervalMetricReplayDispositionV3::same_identity_payload_conflict;
  bool second_emission_admitted = false;
  bool receiving_owner_update_admitted = false;
  bool idempotent_without_increment = false;
  bool datatype_outcome_unchanged = true;
  bool metric_outcome_unchanged = true;
};

// Both inputs must already have returned admitted from
// ValidateIntervalMetricEvidenceV3 under their respective current owner
// contexts. Zero or different process epochs are nevertheless refused by the
// classifier's first defensive gate.
IntervalMetricReplayFactV3 ClassifyIntervalMetricReplayV3(
    const IntervalMetricEvidenceV3& first,
    const IntervalMetricEvidenceV3& second) noexcept;

}  // namespace scratchbird::core::datatypes

// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace scratchbird::engine::optimizer {

// SEARCH_KEY: CEIC_054_SELECTIVITY_DRIFT_PLAN_STABILITY_OBSERVABILITY
// Selectivity drift and plan-stability reports are bounded optimizer
// observability evidence only. They may inform support bundles, feedback
// quarantine, and diagnostics, but they are not transaction finality,
// visibility, authorization/security, recovery, parser, reference, WAL, benchmark,
// optimizer-plan, index-finality, provider-finality, cluster, or agent-action
// authority.
inline constexpr const char* kOptimizerSelectivityObservabilitySchemaId =
    "sb.optimizer.selectivity_drift_observability.v1";
inline constexpr std::uint32_t kOptimizerSelectivityObservabilitySchemaMajor =
    1;
inline constexpr std::uint32_t kOptimizerSelectivityObservabilitySchemaMinor =
    0;

enum class OptimizerObservabilityClusterMode {
  kNoCluster,
  kExternalProviderDelegated,
  kLocalClusterEvidence,
};

enum class OptimizerDriftBucket {
  kWithinTolerance,
  kModerateUnderEstimate,
  kModerateOverEstimate,
  kSevereUnderEstimate,
  kSevereOverEstimate,
};

enum class OptimizerStatisticUsefulnessKind {
  kHistogram,
  kMostCommonValues,
  kExtendedStatistics,
};

enum class OptimizerStatisticUsefulnessOutcome {
  kUseful,
  kNeutral,
  kRejected,
};

enum class OptimizerPlanFlipReason {
  kNoFlip,
  kStatisticsRefresh,
  kAdaptiveFeedbackAccepted,
  kFeedbackQuarantine,
  kMemoryPressure,
  kSpillPredictionChange,
  kIndexReadinessChange,
  kPlanCacheInvalidation,
  kSecurityRedactionEpoch,
  kCostCalibration,
  kUnclassified,
};

enum class OptimizerFeedbackDecisionState {
  kObservationOnly,
  kAcceptedAdvisory,
  kRejected,
  kQuarantined,
};

struct OptimizerSelectivityObservabilityAuthorityFlags {
  bool transaction_finality_authority = false;
  bool visibility_authority = false;
  bool authorization_security_authority = false;
  bool recovery_authority = false;
  bool parser_authority = false;
  bool reference_authority = false;
  bool wal_authority = false;
  bool benchmark_authority = false;
  bool optimizer_plan_authority = false;
  bool index_finality_authority = false;
  bool provider_finality_authority = false;
  bool local_cluster_authority = false;
  bool cluster_authority = false;
  bool agent_action_authority = false;
};

struct OptimizerSelectivityDriftObservation {
  std::string observation_id;
  std::string route_kind;
  std::string route_lane;
  std::string route_label;
  std::string predicate_id;
  std::string estimator_id;
  std::string result_contract_hash;
  std::string logical_plan_hash;
  std::string physical_plan_hash;
  std::string estimate_source_digest;
  std::string actual_source_digest;
  std::string metric_snapshot_digest;
  std::string histogram_digest;
  std::string mcv_digest;
  std::string extended_stat_digest;

  double estimated_rows = 0.0;
  double actual_rows = 0.0;
  double drift_ratio = 0.0;
  double absolute_error_ratio = 0.0;
  double allowed_error_ratio = 0.0;
  OptimizerDriftBucket drift_bucket =
      OptimizerDriftBucket::kWithinTolerance;

  std::uint64_t catalog_epoch = 0;
  std::uint64_t security_epoch = 0;
  std::uint64_t redaction_epoch = 0;
  std::uint64_t statistics_epoch = 0;
  std::uint64_t feedback_generation = 0;
  std::uint64_t provider_generation = 0;

  bool live_route_consumed = false;
  bool exact_mga_recheck_proven = false;
  bool security_recheck_proven = false;
  bool synthetic_only = false;
  bool placeholder_runtime_evidence = false;
};

struct OptimizerStatisticUsefulnessEvidence {
  std::string evidence_id;
  OptimizerStatisticUsefulnessKind kind =
      OptimizerStatisticUsefulnessKind::kHistogram;
  OptimizerStatisticUsefulnessOutcome outcome =
      OptimizerStatisticUsefulnessOutcome::kUseful;
  std::string statistic_uuid;
  std::string statistic_digest;
  std::string source_observation_id;
  double drift_before_ratio = 0.0;
  double drift_after_ratio = 0.0;
  double usefulness_score = 0.0;
  std::uint64_t sample_count = 0;
  std::uint64_t statistic_epoch = 0;
  bool fresh = false;
  bool trusted_source = false;
  bool accepted_by_optimizer = false;
};

struct OptimizerAnnRecallEvidence {
  std::string evidence_id;
  std::string route_label;
  std::string vector_index_family;
  std::string candidate_digest;
  std::string exact_rerank_digest;
  std::string exact_result_hash;
  double observed_recall = 0.0;
  double target_recall = 0.0;
  std::uint64_t candidate_count = 0;
  std::uint64_t reranked_count = 0;
  bool exact_rerank_proven = false;
  bool metadata_prefilter_proven = false;
  bool approximate_candidates_only = true;
};

struct OptimizerSpillPredictionEvidence {
  std::string evidence_id;
  std::string operator_id;
  std::string memory_reservation_digest;
  std::string spill_metric_digest;
  std::uint64_t predicted_spill_bytes = 0;
  std::uint64_t actual_spill_bytes = 0;
  double prediction_error_ratio = 0.0;
  double allowed_error_ratio = 0.0;
  bool predicted_spill = false;
  bool actual_spill = false;
  bool governed_memory_reservation = false;
  bool spill_observed_from_metric = false;
};

struct OptimizerPlanFlipEvidence {
  std::string flip_id;
  std::string route_label;
  std::string previous_plan_hash;
  std::string current_plan_hash;
  std::string previous_result_contract_hash;
  std::string current_result_contract_hash;
  std::string reason_digest;
  OptimizerPlanFlipReason reason = OptimizerPlanFlipReason::kNoFlip;
  std::uint64_t previous_generation = 0;
  std::uint64_t current_generation = 0;
  bool flip_detected = false;
  bool result_contract_continuity_proven = false;
  bool reason_classified = false;
  bool stability_window_updated = false;
};

struct OptimizerFeedbackDecisionEvidence {
  std::string feedback_id;
  OptimizerFeedbackDecisionState state =
      OptimizerFeedbackDecisionState::kObservationOnly;
  std::string source_provenance_digest;
  std::string metric_snapshot_digest;
  std::string quarantine_reason;
  std::string decision_digest;
  std::uint64_t feedback_generation = 0;
  bool advisory_only = true;
  bool fresh = false;
  bool trusted_source = false;
  bool redacted = false;
  bool quarantined = false;
  bool applied_to_plan_authority = false;
};

struct OptimizerStabilityWindowEvidence {
  std::string window_id;
  std::string route_label;
  std::string stable_plan_hash;
  std::string window_digest;
  std::uint64_t window_start_unix_micros = 0;
  std::uint64_t window_end_unix_micros = 0;
  std::uint64_t start_generation = 0;
  std::uint64_t end_generation = 0;
  std::uint64_t observation_count = 0;
  std::uint64_t plan_flip_count = 0;
  bool plan_hash_continuity_proven = false;
  bool stability_threshold_met = false;
};

struct OptimizerSupportBundleReportEvidence {
  std::string report_id;
  std::string report_schema_id;
  std::string report_digest;
  std::string support_bundle_digest;
  std::string source_provenance_digest;
  std::string redaction_digest;
  std::string retention_class;
  std::vector<std::string> route_labels;
  std::uint64_t row_count = 0;
  std::uint64_t max_row_count = 0;
  std::uint64_t byte_count = 0;
  std::uint64_t max_byte_count = 0;
  std::uint64_t freshness_microseconds = 0;
  std::uint64_t max_freshness_microseconds = 0;
  bool bounded = false;
  bool fresh = false;
  bool trusted_source = false;
  bool redaction_applied = false;
  bool protected_material_redacted = false;
  bool low_memory_safe = false;
  bool evidence_only = true;
  bool synthetic_only = false;
  OptimizerSelectivityObservabilityAuthorityFlags authority;
};

struct OptimizerSelectivityObservabilityReport {
  std::string schema_id = kOptimizerSelectivityObservabilitySchemaId;
  std::uint32_t schema_version_major =
      kOptimizerSelectivityObservabilitySchemaMajor;
  std::uint32_t schema_version_minor =
      kOptimizerSelectivityObservabilitySchemaMinor;
  std::string report_uuid;
  std::string report_generation_uuid;
  std::string report_digest;
  std::string optimizer_profile;
  bool production_observability_claim = false;
  bool evidence_only = true;
  bool reference_reference_only = true;
  bool reference_as_authority = false;
  bool uses_reference_storage_or_finality_for_scratchbird = false;
  OptimizerObservabilityClusterMode cluster_mode =
      OptimizerObservabilityClusterMode::kNoCluster;
  std::string external_cluster_provider_id;
  bool cluster_claim_blocked = false;
  OptimizerSelectivityObservabilityAuthorityFlags authority;

  OptimizerSupportBundleReportEvidence support_bundle;
  std::vector<OptimizerSelectivityDriftObservation> drift_observations;
  std::vector<OptimizerStatisticUsefulnessEvidence> statistic_usefulness;
  std::vector<OptimizerAnnRecallEvidence> ann_recall;
  std::vector<OptimizerSpillPredictionEvidence> spill_predictions;
  std::vector<OptimizerPlanFlipEvidence> plan_flips;
  std::vector<OptimizerFeedbackDecisionEvidence> feedback_decisions;
  std::vector<OptimizerStabilityWindowEvidence> stability_windows;
};

struct OptimizerSelectivityObservabilityValidation {
  bool ok = false;
  bool support_report_admissible = false;
  std::string diagnostic_code;
  std::vector<std::string> missing_fields;
  std::vector<std::string> diagnostics;
};

const char* OptimizerDriftBucketName(OptimizerDriftBucket value);

const char* OptimizerStatisticUsefulnessKindName(
    OptimizerStatisticUsefulnessKind value);

const char* OptimizerPlanFlipReasonName(OptimizerPlanFlipReason value);

OptimizerSelectivityObservabilityValidation
ValidateOptimizerSelectivityObservabilityReport(
    const OptimizerSelectivityObservabilityReport& report);

OptimizerSelectivityObservabilityValidation
ValidateOptimizerSelectivityObservabilityReportSet(
    const std::vector<OptimizerSelectivityObservabilityReport>& reports,
    const std::vector<std::string>& required_routes);

}  // namespace scratchbird::engine::optimizer

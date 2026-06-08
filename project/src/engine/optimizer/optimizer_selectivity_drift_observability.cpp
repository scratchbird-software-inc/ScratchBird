// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "optimizer_selectivity_drift_observability.hpp"

#include <algorithm>
#include <cmath>
#include <set>
#include <string>
#include <string_view>
#include <utility>

namespace scratchbird::engine::optimizer {
namespace {

bool Empty(std::string_view value) {
  return value.empty();
}

bool StartsWith(std::string_view value, std::string_view prefix) {
  return value.size() >= prefix.size() &&
         value.substr(0, prefix.size()) == prefix;
}

bool IsHashLike(std::string_view value) {
  return StartsWith(value, "sha256:");
}

bool IsPlaceholderResultContract(std::string_view value) {
  return value.empty() || value == "result-contract-v1" ||
         value == "sha256:result-contract-v1";
}

bool Positive(double value) {
  return value > 0.0;
}

bool HasForbiddenAuthority(
    const OptimizerSelectivityObservabilityAuthorityFlags& authority) {
  return authority.transaction_finality_authority ||
         authority.visibility_authority ||
         authority.authorization_security_authority ||
         authority.recovery_authority ||
         authority.parser_authority ||
         authority.donor_authority ||
         authority.wal_authority ||
         authority.benchmark_authority ||
         authority.optimizer_plan_authority ||
         authority.index_finality_authority ||
         authority.provider_finality_authority ||
         authority.local_cluster_authority ||
         authority.cluster_authority ||
         authority.agent_action_authority;
}

void AddDiagnostic(OptimizerSelectivityObservabilityValidation* validation,
                   std::string diagnostic) {
  validation->diagnostics.push_back(std::move(diagnostic));
}

void RequireField(OptimizerSelectivityObservabilityValidation* validation,
                  bool present,
                  std::string field_name) {
  if (!present) validation->missing_fields.push_back(std::move(field_name));
}

std::string ReportPrefix(
    const OptimizerSelectivityObservabilityReport& report) {
  if (!report.report_uuid.empty()) return report.report_uuid;
  return "unnamed_selectivity_observability_report";
}

std::string ObservationPrefix(
    const OptimizerSelectivityDriftObservation& observation) {
  if (!observation.observation_id.empty()) return observation.observation_id;
  if (!observation.route_label.empty()) return observation.route_label;
  return "unnamed_drift_observation";
}

std::string StatPrefix(
    const OptimizerStatisticUsefulnessEvidence& stat) {
  if (!stat.evidence_id.empty()) return stat.evidence_id;
  return OptimizerStatisticUsefulnessKindName(stat.kind);
}

double ExpectedDriftRatio(double estimated_rows, double actual_rows) {
  if (estimated_rows <= 0.0 || actual_rows <= 0.0) return 0.0;
  return actual_rows / estimated_rows;
}

double ExpectedAbsErrorRatio(double estimated_rows, double actual_rows) {
  if (actual_rows <= 0.0) return 0.0;
  return std::abs(actual_rows - estimated_rows) / actual_rows;
}

bool SameMetric(double lhs, double rhs) {
  return std::abs(lhs - rhs) <= 0.0001;
}

bool DriftBucketMatches(const OptimizerSelectivityDriftObservation& row) {
  const double ratio = row.drift_ratio;
  switch (row.drift_bucket) {
    case OptimizerDriftBucket::kWithinTolerance:
      return ratio >= 0.8 && ratio <= 1.25;
    case OptimizerDriftBucket::kModerateUnderEstimate:
      return ratio > 1.25 && ratio <= 4.0;
    case OptimizerDriftBucket::kModerateOverEstimate:
      return ratio >= 0.25 && ratio < 0.8;
    case OptimizerDriftBucket::kSevereUnderEstimate:
      return ratio > 4.0;
    case OptimizerDriftBucket::kSevereOverEstimate:
      return ratio > 0.0 && ratio < 0.25;
  }
  return false;
}

bool ValidRouteKind(std::string_view route_kind) {
  static const std::set<std::string_view> kRoutes = {
      "embedded", "ipc", "inet", "cli", "driver", "document", "vector",
      "text",     "graph", "mixed_fusion"};
  return kRoutes.find(route_kind) != kRoutes.end();
}

void ValidateDriftObservation(
    OptimizerSelectivityObservabilityValidation* validation,
    const OptimizerSelectivityDriftObservation& row) {
  const auto prefix = ObservationPrefix(row);
  if (row.observation_id.empty() || row.route_kind.empty() ||
      row.route_lane.empty() || row.route_label.empty() ||
      row.predicate_id.empty() || row.estimator_id.empty()) {
    AddDiagnostic(validation,
                  prefix +
                      ":SB_OPT_SELECTIVITY_OBSERVABILITY.DRIFT_IDENTITY_MISSING");
  }
  if (!ValidRouteKind(row.route_kind)) {
    AddDiagnostic(validation,
                  prefix +
                      ":SB_OPT_SELECTIVITY_OBSERVABILITY.ROUTE_KIND_INVALID");
  }
  if (IsPlaceholderResultContract(row.result_contract_hash) ||
      !IsHashLike(row.result_contract_hash) ||
      !IsHashLike(row.logical_plan_hash) ||
      !IsHashLike(row.physical_plan_hash)) {
    AddDiagnostic(validation,
                  prefix +
                      ":SB_OPT_SELECTIVITY_OBSERVABILITY.PLACEHOLDER_OR_HASH_MISSING");
  }
  if (!IsHashLike(row.estimate_source_digest) ||
      !IsHashLike(row.actual_source_digest) ||
      !IsHashLike(row.metric_snapshot_digest) ||
      !IsHashLike(row.histogram_digest) || !IsHashLike(row.mcv_digest) ||
      !IsHashLike(row.extended_stat_digest)) {
    AddDiagnostic(validation,
                  prefix +
                      ":SB_OPT_SELECTIVITY_OBSERVABILITY.DRIFT_SOURCE_DIGEST_MISSING");
  }
  if (!Positive(row.estimated_rows) || !Positive(row.actual_rows) ||
      !Positive(row.drift_ratio) || row.allowed_error_ratio <= 0.0) {
    AddDiagnostic(validation,
                  prefix +
                      ":SB_OPT_SELECTIVITY_OBSERVABILITY.DRIFT_METRICS_MISSING");
  } else {
    if (!SameMetric(row.drift_ratio,
                    ExpectedDriftRatio(row.estimated_rows, row.actual_rows)) ||
        !SameMetric(row.absolute_error_ratio,
                    ExpectedAbsErrorRatio(row.estimated_rows,
                                          row.actual_rows)) ||
        !DriftBucketMatches(row)) {
      AddDiagnostic(validation,
                    prefix +
                        ":SB_OPT_SELECTIVITY_OBSERVABILITY.DRIFT_METRICS_INCONSISTENT");
    }
  }
  if (row.catalog_epoch <= 1 || row.security_epoch <= 1 ||
      row.redaction_epoch <= 1 || row.statistics_epoch <= 1 ||
      row.feedback_generation <= 1 || row.provider_generation <= 1) {
    AddDiagnostic(validation,
                  prefix +
                      ":SB_OPT_SELECTIVITY_OBSERVABILITY.PLACEHOLDER_EPOCH");
  }
  if (!row.live_route_consumed || !row.exact_mga_recheck_proven ||
      !row.security_recheck_proven) {
    AddDiagnostic(validation,
                  prefix +
                      ":SB_OPT_SELECTIVITY_OBSERVABILITY.LIVE_RECHECK_PROOF_MISSING");
  }
  if (row.synthetic_only || row.placeholder_runtime_evidence) {
    AddDiagnostic(validation,
                  prefix +
                      ":SB_OPT_SELECTIVITY_OBSERVABILITY.SYNTHETIC_OR_PLACEHOLDER");
  }
}

void ValidateStatisticUsefulness(
    OptimizerSelectivityObservabilityValidation* validation,
    const OptimizerStatisticUsefulnessEvidence& stat) {
  const auto prefix = StatPrefix(stat);
  if (stat.evidence_id.empty() || stat.statistic_uuid.empty() ||
      stat.source_observation_id.empty() ||
      !IsHashLike(stat.statistic_digest)) {
    AddDiagnostic(validation,
                  prefix +
                      ":SB_OPT_SELECTIVITY_OBSERVABILITY.STAT_USEFULNESS_IDENTITY_MISSING");
  }
  if (!Positive(stat.drift_before_ratio) ||
      !Positive(stat.drift_after_ratio) ||
      stat.usefulness_score <= 0.0 || stat.sample_count == 0 ||
      stat.statistic_epoch <= 1) {
    AddDiagnostic(validation,
                  prefix +
                      ":SB_OPT_SELECTIVITY_OBSERVABILITY.STAT_USEFULNESS_METRICS_MISSING");
  }
  if (stat.outcome == OptimizerStatisticUsefulnessOutcome::kUseful &&
      stat.drift_after_ratio > stat.drift_before_ratio) {
    AddDiagnostic(validation,
                  prefix +
                      ":SB_OPT_SELECTIVITY_OBSERVABILITY.STAT_USEFULNESS_REGRESSION");
  }
  if (!stat.fresh || !stat.trusted_source || !stat.accepted_by_optimizer) {
    AddDiagnostic(validation,
                  prefix +
                      ":SB_OPT_SELECTIVITY_OBSERVABILITY.STAT_USEFULNESS_PROVENANCE_INVALID");
  }
}

void ValidateAnnRecall(OptimizerSelectivityObservabilityValidation* validation,
                       const OptimizerAnnRecallEvidence& ann) {
  const auto prefix = ann.evidence_id.empty() ? "ann_recall" : ann.evidence_id;
  if (ann.evidence_id.empty() || ann.route_label.empty() ||
      ann.vector_index_family.empty() || !IsHashLike(ann.candidate_digest) ||
      !IsHashLike(ann.exact_rerank_digest) ||
      !IsHashLike(ann.exact_result_hash)) {
    AddDiagnostic(validation,
                  prefix +
                      ":SB_OPT_SELECTIVITY_OBSERVABILITY.ANN_RECALL_IDENTITY_MISSING");
  }
  if (ann.target_recall <= 0.0 || ann.target_recall > 1.0 ||
      ann.observed_recall <= 0.0 || ann.observed_recall > 1.0 ||
      ann.observed_recall < ann.target_recall ||
      ann.candidate_count == 0 || ann.reranked_count == 0 ||
      ann.reranked_count > ann.candidate_count ||
      !ann.exact_rerank_proven || !ann.metadata_prefilter_proven ||
      !ann.approximate_candidates_only) {
    AddDiagnostic(validation,
                  prefix +
                      ":SB_OPT_SELECTIVITY_OBSERVABILITY.ANN_RECALL_INVALID");
  }
}

void ValidateSpillPrediction(
    OptimizerSelectivityObservabilityValidation* validation,
    const OptimizerSpillPredictionEvidence& spill) {
  const auto prefix =
      spill.evidence_id.empty() ? "spill_prediction" : spill.evidence_id;
  if (spill.evidence_id.empty() || spill.operator_id.empty() ||
      !IsHashLike(spill.memory_reservation_digest) ||
      !IsHashLike(spill.spill_metric_digest)) {
    AddDiagnostic(validation,
                  prefix +
                      ":SB_OPT_SELECTIVITY_OBSERVABILITY.SPILL_IDENTITY_MISSING");
  }
  if (spill.allowed_error_ratio <= 0.0 ||
      spill.prediction_error_ratio > spill.allowed_error_ratio ||
      spill.actual_spill != (spill.actual_spill_bytes > 0) ||
      spill.predicted_spill != (spill.predicted_spill_bytes > 0) ||
      (spill.actual_spill && !spill.predicted_spill) ||
      !spill.governed_memory_reservation ||
      !spill.spill_observed_from_metric) {
    AddDiagnostic(validation,
                  prefix +
                      ":SB_OPT_SELECTIVITY_OBSERVABILITY.SPILL_PREDICTION_INVALID");
  }
}

void ValidatePlanFlip(OptimizerSelectivityObservabilityValidation* validation,
                      const OptimizerPlanFlipEvidence& flip) {
  const auto prefix = flip.flip_id.empty() ? "plan_flip" : flip.flip_id;
  if (flip.flip_id.empty() || flip.route_label.empty() ||
      !IsHashLike(flip.previous_plan_hash) ||
      !IsHashLike(flip.current_plan_hash) ||
      IsPlaceholderResultContract(flip.previous_result_contract_hash) ||
      IsPlaceholderResultContract(flip.current_result_contract_hash) ||
      !IsHashLike(flip.previous_result_contract_hash) ||
      !IsHashLike(flip.current_result_contract_hash) ||
      !IsHashLike(flip.reason_digest)) {
    AddDiagnostic(validation,
                  prefix +
                      ":SB_OPT_SELECTIVITY_OBSERVABILITY.PLAN_FLIP_IDENTITY_MISSING");
  }
  if (flip.previous_generation <= 1 || flip.current_generation <= 1 ||
      flip.current_generation <= flip.previous_generation ||
      !flip.result_contract_continuity_proven ||
      !flip.stability_window_updated) {
    AddDiagnostic(validation,
                  prefix +
                      ":SB_OPT_SELECTIVITY_OBSERVABILITY.PLAN_FLIP_GENERATION_INVALID");
  }
  if (flip.flip_detected) {
    if (flip.previous_plan_hash == flip.current_plan_hash ||
        !flip.reason_classified ||
        flip.reason == OptimizerPlanFlipReason::kNoFlip ||
        flip.reason == OptimizerPlanFlipReason::kUnclassified) {
      AddDiagnostic(validation,
                    prefix +
                        ":SB_OPT_SELECTIVITY_OBSERVABILITY.PLAN_FLIP_CLASSIFICATION_MISSING");
    }
  } else if (flip.reason != OptimizerPlanFlipReason::kNoFlip) {
    AddDiagnostic(validation,
                  prefix +
                      ":SB_OPT_SELECTIVITY_OBSERVABILITY.PLAN_FLIP_FALSE_REASON");
  }
}

void ValidateFeedbackDecision(
    OptimizerSelectivityObservabilityValidation* validation,
    const OptimizerFeedbackDecisionEvidence& feedback) {
  const auto prefix =
      feedback.feedback_id.empty() ? "feedback_decision" : feedback.feedback_id;
  if (feedback.feedback_id.empty() ||
      !IsHashLike(feedback.source_provenance_digest) ||
      !IsHashLike(feedback.metric_snapshot_digest) ||
      !IsHashLike(feedback.decision_digest)) {
    AddDiagnostic(validation,
                  prefix +
                      ":SB_OPT_SELECTIVITY_OBSERVABILITY.FEEDBACK_IDENTITY_MISSING");
  }
  if (feedback.feedback_generation <= 1 || !feedback.advisory_only ||
      !feedback.fresh || !feedback.trusted_source || !feedback.redacted ||
      feedback.applied_to_plan_authority) {
    AddDiagnostic(validation,
                  prefix +
                      ":SB_OPT_SELECTIVITY_OBSERVABILITY.FEEDBACK_PROVENANCE_INVALID");
  }
  if (feedback.state == OptimizerFeedbackDecisionState::kQuarantined) {
    if (!feedback.quarantined || feedback.quarantine_reason.empty()) {
      AddDiagnostic(validation,
                    prefix +
                        ":SB_OPT_SELECTIVITY_OBSERVABILITY.FEEDBACK_QUARANTINE_MISSING");
    }
  }
}

void ValidateStabilityWindow(
    OptimizerSelectivityObservabilityValidation* validation,
    const OptimizerStabilityWindowEvidence& window) {
  const auto prefix =
      window.window_id.empty() ? "stability_window" : window.window_id;
  if (window.window_id.empty() || window.route_label.empty() ||
      !IsHashLike(window.stable_plan_hash) ||
      !IsHashLike(window.window_digest)) {
    AddDiagnostic(validation,
                  prefix +
                      ":SB_OPT_SELECTIVITY_OBSERVABILITY.STABILITY_WINDOW_IDENTITY_MISSING");
  }
  if (window.window_start_unix_micros == 0 ||
      window.window_end_unix_micros <= window.window_start_unix_micros ||
      window.start_generation <= 1 || window.end_generation <= 1 ||
      window.end_generation < window.start_generation ||
      window.observation_count == 0 ||
      !window.plan_hash_continuity_proven ||
      !window.stability_threshold_met) {
    AddDiagnostic(validation,
                  prefix +
                      ":SB_OPT_SELECTIVITY_OBSERVABILITY.STABILITY_WINDOW_INVALID");
  }
}

void ValidateSupportBundle(
    OptimizerSelectivityObservabilityValidation* validation,
    const OptimizerSupportBundleReportEvidence& support) {
  const auto prefix =
      support.report_id.empty() ? "support_bundle_report" : support.report_id;
  if (support.report_id.empty() || support.report_schema_id.empty() ||
      !IsHashLike(support.report_digest) ||
      !IsHashLike(support.support_bundle_digest) ||
      !IsHashLike(support.source_provenance_digest) ||
      !IsHashLike(support.redaction_digest) ||
      support.retention_class.empty() || support.route_labels.empty()) {
    AddDiagnostic(validation,
                  prefix +
                      ":SB_OPT_SELECTIVITY_OBSERVABILITY.SUPPORT_REPORT_IDENTITY_MISSING");
  }
  if (!support.bounded || support.row_count == 0 ||
      support.max_row_count == 0 || support.row_count > support.max_row_count ||
      support.byte_count == 0 || support.max_byte_count == 0 ||
      support.byte_count > support.max_byte_count ||
      support.freshness_microseconds == 0 ||
      support.max_freshness_microseconds == 0 ||
      support.freshness_microseconds > support.max_freshness_microseconds ||
      !support.fresh || !support.trusted_source ||
      !support.redaction_applied || !support.protected_material_redacted ||
      !support.low_memory_safe || !support.evidence_only) {
    AddDiagnostic(validation,
                  prefix +
                      ":SB_OPT_SELECTIVITY_OBSERVABILITY.SUPPORT_REPORT_INVALID");
  }
  if (support.synthetic_only) {
    AddDiagnostic(validation,
                  prefix +
                      ":SB_OPT_SELECTIVITY_OBSERVABILITY.SYNTHETIC_SUPPORT_REPORT");
  }
  if (HasForbiddenAuthority(support.authority)) {
    AddDiagnostic(validation,
                  prefix +
                      ":SB_OPT_SELECTIVITY_OBSERVABILITY.SUPPORT_FORBIDDEN_AUTHORITY");
  }
}

}  // namespace

const char* OptimizerDriftBucketName(OptimizerDriftBucket value) {
  switch (value) {
    case OptimizerDriftBucket::kWithinTolerance:
      return "within_tolerance";
    case OptimizerDriftBucket::kModerateUnderEstimate:
      return "moderate_underestimate";
    case OptimizerDriftBucket::kModerateOverEstimate:
      return "moderate_overestimate";
    case OptimizerDriftBucket::kSevereUnderEstimate:
      return "severe_underestimate";
    case OptimizerDriftBucket::kSevereOverEstimate:
      return "severe_overestimate";
  }
  return "unknown";
}

const char* OptimizerStatisticUsefulnessKindName(
    OptimizerStatisticUsefulnessKind value) {
  switch (value) {
    case OptimizerStatisticUsefulnessKind::kHistogram:
      return "histogram";
    case OptimizerStatisticUsefulnessKind::kMostCommonValues:
      return "mcv";
    case OptimizerStatisticUsefulnessKind::kExtendedStatistics:
      return "extended_statistics";
  }
  return "unknown";
}

const char* OptimizerPlanFlipReasonName(OptimizerPlanFlipReason value) {
  switch (value) {
    case OptimizerPlanFlipReason::kNoFlip:
      return "no_flip";
    case OptimizerPlanFlipReason::kStatisticsRefresh:
      return "statistics_refresh";
    case OptimizerPlanFlipReason::kAdaptiveFeedbackAccepted:
      return "adaptive_feedback_accepted";
    case OptimizerPlanFlipReason::kFeedbackQuarantine:
      return "feedback_quarantine";
    case OptimizerPlanFlipReason::kMemoryPressure:
      return "memory_pressure";
    case OptimizerPlanFlipReason::kSpillPredictionChange:
      return "spill_prediction_change";
    case OptimizerPlanFlipReason::kIndexReadinessChange:
      return "index_readiness_change";
    case OptimizerPlanFlipReason::kPlanCacheInvalidation:
      return "plan_cache_invalidation";
    case OptimizerPlanFlipReason::kSecurityRedactionEpoch:
      return "security_redaction_epoch";
    case OptimizerPlanFlipReason::kCostCalibration:
      return "cost_calibration";
    case OptimizerPlanFlipReason::kUnclassified:
      return "unclassified";
  }
  return "unknown";
}

// SEARCH_KEY: ValidateOptimizerSelectivityObservabilityReport
OptimizerSelectivityObservabilityValidation
ValidateOptimizerSelectivityObservabilityReport(
    const OptimizerSelectivityObservabilityReport& report) {
  OptimizerSelectivityObservabilityValidation validation;
  const auto prefix = ReportPrefix(report);

  RequireField(&validation,
               report.schema_id == kOptimizerSelectivityObservabilitySchemaId,
               "schema_id");
  RequireField(&validation,
               report.schema_version_major ==
                   kOptimizerSelectivityObservabilitySchemaMajor,
               "schema_version_major");
  RequireField(&validation,
               report.schema_version_minor ==
                   kOptimizerSelectivityObservabilitySchemaMinor,
               "schema_version_minor");
  RequireField(&validation, !Empty(report.report_uuid), "report_uuid");
  RequireField(&validation,
               !Empty(report.report_generation_uuid),
               "report_generation_uuid");
  RequireField(&validation, IsHashLike(report.report_digest), "report_digest");
  RequireField(&validation,
               !Empty(report.optimizer_profile),
               "optimizer_profile");

  if (!report.evidence_only || !report.donor_reference_only ||
      report.donor_as_authority ||
      report.uses_donor_storage_or_finality_for_scratchbird) {
    AddDiagnostic(&validation,
                  prefix +
                      ":SB_OPT_SELECTIVITY_OBSERVABILITY.DONOR_AUTHORITY_DRIFT");
  }
  if (HasForbiddenAuthority(report.authority)) {
    AddDiagnostic(&validation,
                  prefix +
                      ":SB_OPT_SELECTIVITY_OBSERVABILITY.FORBIDDEN_AUTHORITY");
  }
  if (report.cluster_mode ==
          OptimizerObservabilityClusterMode::kLocalClusterEvidence) {
    AddDiagnostic(&validation,
                  prefix +
                      ":SB_OPT_SELECTIVITY_OBSERVABILITY.LOCAL_CLUSTER_FORBIDDEN");
  } else if (report.cluster_mode ==
             OptimizerObservabilityClusterMode::kExternalProviderDelegated) {
    if (report.external_cluster_provider_id.empty() ||
        !report.cluster_claim_blocked ||
        report.production_observability_claim) {
      AddDiagnostic(
          &validation,
          prefix +
              ":SB_OPT_SELECTIVITY_OBSERVABILITY.EXTERNAL_CLUSTER_CLAIM_BLOCK_REQUIRED");
    }
  }

  ValidateSupportBundle(&validation, report.support_bundle);

  if (report.drift_observations.empty()) {
    AddDiagnostic(&validation,
                  prefix +
                      ":SB_OPT_SELECTIVITY_OBSERVABILITY.DRIFT_METRICS_MISSING");
  }
  for (const auto& row : report.drift_observations) {
    ValidateDriftObservation(&validation, row);
  }

  bool saw_histogram = false;
  bool saw_mcv = false;
  bool saw_extended = false;
  for (const auto& stat : report.statistic_usefulness) {
    saw_histogram =
        saw_histogram ||
        stat.kind == OptimizerStatisticUsefulnessKind::kHistogram;
    saw_mcv = saw_mcv ||
              stat.kind == OptimizerStatisticUsefulnessKind::kMostCommonValues;
    saw_extended =
        saw_extended ||
        stat.kind == OptimizerStatisticUsefulnessKind::kExtendedStatistics;
    ValidateStatisticUsefulness(&validation, stat);
  }
  if (!saw_histogram || !saw_mcv || !saw_extended) {
    AddDiagnostic(&validation,
                  prefix +
                      ":SB_OPT_SELECTIVITY_OBSERVABILITY.STATS_USEFULNESS_MISSING");
  }

  if (report.ann_recall.empty()) {
    AddDiagnostic(&validation,
                  prefix +
                      ":SB_OPT_SELECTIVITY_OBSERVABILITY.ANN_RECALL_INVALID");
  }
  for (const auto& ann : report.ann_recall) {
    ValidateAnnRecall(&validation, ann);
  }

  if (report.spill_predictions.empty()) {
    AddDiagnostic(&validation,
                  prefix +
                      ":SB_OPT_SELECTIVITY_OBSERVABILITY.SPILL_PREDICTION_INVALID");
  }
  for (const auto& spill : report.spill_predictions) {
    ValidateSpillPrediction(&validation, spill);
  }

  if (report.plan_flips.empty()) {
    AddDiagnostic(
        &validation,
        prefix +
            ":SB_OPT_SELECTIVITY_OBSERVABILITY.PLAN_FLIP_CLASSIFICATION_MISSING");
  }
  for (const auto& flip : report.plan_flips) {
    ValidatePlanFlip(&validation, flip);
  }

  if (report.feedback_decisions.empty()) {
    AddDiagnostic(
        &validation,
        prefix +
            ":SB_OPT_SELECTIVITY_OBSERVABILITY.FEEDBACK_PROVENANCE_INVALID");
  }
  for (const auto& feedback : report.feedback_decisions) {
    ValidateFeedbackDecision(&validation, feedback);
  }

  if (report.stability_windows.empty()) {
    AddDiagnostic(&validation,
                  prefix +
                      ":SB_OPT_SELECTIVITY_OBSERVABILITY.STABILITY_WINDOW_INVALID");
  }
  for (const auto& window : report.stability_windows) {
    ValidateStabilityWindow(&validation, window);
  }

  for (const auto& label : report.support_bundle.route_labels) {
    const bool matched =
        std::any_of(report.drift_observations.begin(),
                    report.drift_observations.end(), [&](const auto& row) {
                      return row.route_label == label;
                    });
    if (!matched) {
      AddDiagnostic(&validation,
                    label +
                        ":SB_OPT_SELECTIVITY_OBSERVABILITY.SUPPORT_ROUTE_LABEL_UNMATCHED");
    }
  }

  validation.ok = validation.missing_fields.empty() &&
                  validation.diagnostics.empty();
  validation.support_report_admissible =
      validation.ok && report.production_observability_claim &&
      report.cluster_mode == OptimizerObservabilityClusterMode::kNoCluster;
  validation.diagnostic_code =
      validation.ok ? "SB_OPT_SELECTIVITY_OBSERVABILITY.OK"
                    : (!validation.missing_fields.empty()
                           ? "SB_OPT_SELECTIVITY_OBSERVABILITY.MISSING_REQUIRED_FIELD"
                           : "SB_OPT_SELECTIVITY_OBSERVABILITY.INVALID_CONTRACT");
  return validation;
}

OptimizerSelectivityObservabilityValidation
ValidateOptimizerSelectivityObservabilityReportSet(
    const std::vector<OptimizerSelectivityObservabilityReport>& reports,
    const std::vector<std::string>& required_routes) {
  OptimizerSelectivityObservabilityValidation validation;
  if (reports.empty()) {
    validation.diagnostic_code =
        "SB_OPT_SELECTIVITY_OBSERVABILITY.EMPTY_REPORT_SET";
    validation.diagnostics.push_back(
        "SB_OPT_SELECTIVITY_OBSERVABILITY.EMPTY_REPORT_SET");
    return validation;
  }

  std::set<std::string> seen_reports;
  std::set<std::string> seen_routes;
  bool all_admissible = true;
  for (const auto& report : reports) {
    if (!report.report_uuid.empty() &&
        !seen_reports.insert(report.report_uuid).second) {
      AddDiagnostic(&validation,
                    report.report_uuid +
                        ":SB_OPT_SELECTIVITY_OBSERVABILITY.DUPLICATE_REPORT");
    }
    for (const auto& row : report.drift_observations) {
      if (!row.route_kind.empty()) seen_routes.insert(row.route_kind);
    }
    const auto report_validation =
        ValidateOptimizerSelectivityObservabilityReport(report);
    if (!report_validation.ok) {
      AddDiagnostic(&validation,
                    ReportPrefix(report) + ":" +
                        report_validation.diagnostic_code);
      validation.diagnostics.insert(validation.diagnostics.end(),
                                    report_validation.diagnostics.begin(),
                                    report_validation.diagnostics.end());
      validation.missing_fields.insert(
          validation.missing_fields.end(),
          report_validation.missing_fields.begin(),
          report_validation.missing_fields.end());
    }
    all_admissible =
        all_admissible && report_validation.support_report_admissible;
  }

  for (const auto& route : required_routes) {
    if (seen_routes.find(route) == seen_routes.end()) {
      AddDiagnostic(&validation,
                    route +
                        ":SB_OPT_SELECTIVITY_OBSERVABILITY.MISSING_ROUTE");
    }
  }

  validation.ok = validation.missing_fields.empty() &&
                  validation.diagnostics.empty();
  validation.support_report_admissible = validation.ok && all_admissible;
  validation.diagnostic_code =
      validation.ok ? "SB_OPT_SELECTIVITY_OBSERVABILITY.SET_OK"
                    : "SB_OPT_SELECTIVITY_OBSERVABILITY.SET_INVALID";
  return validation;
}

}  // namespace scratchbird::engine::optimizer

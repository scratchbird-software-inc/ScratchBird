// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// CEIC-054 focused validation for optimizer selectivity drift and
// plan-stability observability evidence.
#include "optimizer_selectivity_drift_observability.hpp"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace opt = scratchbird::engine::optimizer;

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view message) {
  if (!condition) Fail(message);
}

bool HasDiagnostic(const std::vector<std::string>& diagnostics,
                   std::string_view token) {
  for (const auto& diagnostic : diagnostics) {
    if (diagnostic.find(token) != std::string::npos) return true;
  }
  return false;
}

opt::OptimizerSelectivityDriftObservation Drift(std::string route_kind,
                                                std::string route_label,
                                                double estimated_rows,
                                                double actual_rows) {
  opt::OptimizerSelectivityDriftObservation row;
  row.observation_id = "ceic054-drift-" + route_kind;
  row.route_kind = route_kind;
  row.route_lane = "ceic054/selectivity_drift/" + route_kind;
  row.route_label = route_label;
  row.predicate_id = "ceic054-predicate-" + route_kind;
  row.estimator_id = "ceic054-enterprise-estimator";
  row.result_contract_hash = "sha256:ceic054-result-contract-" + route_kind;
  row.logical_plan_hash = "sha256:ceic054-logical-" + route_kind;
  row.physical_plan_hash = "sha256:ceic054-physical-" + route_kind;
  row.estimate_source_digest = "sha256:ceic054-estimate-" + route_kind;
  row.actual_source_digest = "sha256:ceic054-actual-" + route_kind;
  row.metric_snapshot_digest = "sha256:ceic054-metric-" + route_kind;
  row.histogram_digest = "sha256:ceic054-histogram-" + route_kind;
  row.mcv_digest = "sha256:ceic054-mcv-" + route_kind;
  row.extended_stat_digest = "sha256:ceic054-extended-" + route_kind;
  row.estimated_rows = estimated_rows;
  row.actual_rows = actual_rows;
  row.drift_ratio = actual_rows / estimated_rows;
  row.absolute_error_ratio =
      std::abs(actual_rows - estimated_rows) / actual_rows;
  row.allowed_error_ratio = 0.75;
  row.drift_bucket =
      row.drift_ratio > 1.25
          ? opt::OptimizerDriftBucket::kModerateUnderEstimate
          : opt::OptimizerDriftBucket::kWithinTolerance;
  row.catalog_epoch = 5401;
  row.security_epoch = 5402;
  row.redaction_epoch = 5403;
  row.statistics_epoch = 5404;
  row.feedback_generation = 5405;
  row.provider_generation = 5406;
  row.live_route_consumed = true;
  row.exact_mga_recheck_proven = true;
  row.security_recheck_proven = true;
  return row;
}

opt::OptimizerStatisticUsefulnessEvidence Stat(
    opt::OptimizerStatisticUsefulnessKind kind,
    std::string id,
    double before,
    double after) {
  opt::OptimizerStatisticUsefulnessEvidence stat;
  stat.evidence_id = "ceic054-stat-" + id;
  stat.kind = kind;
  stat.outcome = opt::OptimizerStatisticUsefulnessOutcome::kUseful;
  stat.statistic_uuid = "ceic054-stat-uuid-" + id;
  stat.statistic_digest = "sha256:ceic054-stat-digest-" + id;
  stat.source_observation_id = "ceic054-drift-embedded";
  stat.drift_before_ratio = before;
  stat.drift_after_ratio = after;
  stat.usefulness_score = (before - after) / before;
  stat.sample_count = 1000;
  stat.statistic_epoch = 5410;
  stat.fresh = true;
  stat.trusted_source = true;
  stat.accepted_by_optimizer = true;
  return stat;
}

opt::OptimizerAnnRecallEvidence AnnRecall() {
  opt::OptimizerAnnRecallEvidence ann;
  ann.evidence_id = "ceic054-ann-recall";
  ann.route_label = "ceic054/vector_ann";
  ann.vector_index_family = "hnsw";
  ann.candidate_digest = "sha256:ceic054-ann-candidates";
  ann.exact_rerank_digest = "sha256:ceic054-ann-rerank";
  ann.exact_result_hash = "sha256:ceic054-ann-exact-result";
  ann.observed_recall = 0.982;
  ann.target_recall = 0.950;
  ann.candidate_count = 250;
  ann.reranked_count = 250;
  ann.exact_rerank_proven = true;
  ann.metadata_prefilter_proven = true;
  return ann;
}

opt::OptimizerSpillPredictionEvidence SpillPrediction() {
  opt::OptimizerSpillPredictionEvidence spill;
  spill.evidence_id = "ceic054-spill";
  spill.operator_id = "hash_join_customer_orders";
  spill.memory_reservation_digest = "sha256:ceic054-memory-reservation";
  spill.spill_metric_digest = "sha256:ceic054-spill-metric";
  spill.predicted_spill_bytes = 64 * 1024 * 1024;
  spill.actual_spill_bytes = 68 * 1024 * 1024;
  spill.prediction_error_ratio = 0.058824;
  spill.allowed_error_ratio = 0.25;
  spill.predicted_spill = true;
  spill.actual_spill = true;
  spill.governed_memory_reservation = true;
  spill.spill_observed_from_metric = true;
  return spill;
}

opt::OptimizerPlanFlipEvidence PlanFlip() {
  opt::OptimizerPlanFlipEvidence flip;
  flip.flip_id = "ceic054-plan-flip";
  flip.route_label = "ceic054/embedded_lookup";
  flip.previous_plan_hash = "sha256:ceic054-prev-plan";
  flip.current_plan_hash = "sha256:ceic054-current-plan";
  flip.previous_result_contract_hash = "sha256:ceic054-prev-contract";
  flip.current_result_contract_hash = "sha256:ceic054-current-contract";
  flip.reason_digest = "sha256:ceic054-plan-flip-reason";
  flip.reason = opt::OptimizerPlanFlipReason::kStatisticsRefresh;
  flip.previous_generation = 5408;
  flip.current_generation = 5409;
  flip.flip_detected = true;
  flip.result_contract_continuity_proven = true;
  flip.reason_classified = true;
  flip.stability_window_updated = true;
  return flip;
}

opt::OptimizerFeedbackDecisionEvidence FeedbackDecision() {
  opt::OptimizerFeedbackDecisionEvidence feedback;
  feedback.feedback_id = "ceic054-feedback";
  feedback.state = opt::OptimizerFeedbackDecisionState::kAcceptedAdvisory;
  feedback.source_provenance_digest = "sha256:ceic054-feedback-provenance";
  feedback.metric_snapshot_digest = "sha256:ceic054-feedback-metric";
  feedback.decision_digest = "sha256:ceic054-feedback-decision";
  feedback.feedback_generation = 5415;
  feedback.advisory_only = true;
  feedback.fresh = true;
  feedback.trusted_source = true;
  feedback.redacted = true;
  return feedback;
}

opt::OptimizerStabilityWindowEvidence StabilityWindow() {
  opt::OptimizerStabilityWindowEvidence window;
  window.window_id = "ceic054-window";
  window.route_label = "ceic054/embedded_lookup";
  window.stable_plan_hash = "sha256:ceic054-current-plan";
  window.window_digest = "sha256:ceic054-window-digest";
  window.window_start_unix_micros = 1710000000000000ULL;
  window.window_end_unix_micros = 1710003600000000ULL;
  window.start_generation = 5400;
  window.end_generation = 5410;
  window.observation_count = 64;
  window.plan_flip_count = 1;
  window.plan_hash_continuity_proven = true;
  window.stability_threshold_met = true;
  return window;
}

opt::OptimizerSupportBundleReportEvidence SupportBundle() {
  opt::OptimizerSupportBundleReportEvidence support;
  support.report_id = "ceic054-support-report";
  support.report_schema_id =
      "sb.optimizer.selectivity_drift_observability.report.v1";
  support.report_digest = "sha256:ceic054-report";
  support.support_bundle_digest = "sha256:ceic054-support-bundle";
  support.source_provenance_digest = "sha256:ceic054-support-provenance";
  support.redaction_digest = "sha256:ceic054-support-redaction";
  support.retention_class = "support_bundle_redacted_optimizer_observability";
  support.route_labels = {"ceic054/embedded_lookup", "ceic054/vector_ann"};
  support.row_count = 14;
  support.max_row_count = 128;
  support.byte_count = 48 * 1024;
  support.max_byte_count = 256 * 1024;
  support.freshness_microseconds = 1000;
  support.max_freshness_microseconds = 60000000;
  support.bounded = true;
  support.fresh = true;
  support.trusted_source = true;
  support.redaction_applied = true;
  support.protected_material_redacted = true;
  support.low_memory_safe = true;
  return support;
}

opt::OptimizerSelectivityObservabilityReport Report() {
  opt::OptimizerSelectivityObservabilityReport report;
  report.report_uuid = "ceic054-report-uuid";
  report.report_generation_uuid = "ceic054-generation-uuid";
  report.report_digest = "sha256:ceic054-report-digest";
  report.optimizer_profile = "enterprise";
  report.production_observability_claim = true;
  report.support_bundle = SupportBundle();
  report.drift_observations = {
      Drift("embedded", "ceic054/embedded_lookup", 1000.0, 3000.0),
      Drift("vector", "ceic054/vector_ann", 1000.0, 1100.0)};
  report.statistic_usefulness = {
      Stat(opt::OptimizerStatisticUsefulnessKind::kHistogram,
           "histogram",
           3.0,
           1.4),
      Stat(opt::OptimizerStatisticUsefulnessKind::kMostCommonValues,
           "mcv",
           2.8,
           1.2),
      Stat(opt::OptimizerStatisticUsefulnessKind::kExtendedStatistics,
           "extended",
           5.0,
           1.1)};
  report.ann_recall = {AnnRecall()};
  report.spill_predictions = {SpillPrediction()};
  report.plan_flips = {PlanFlip()};
  report.feedback_decisions = {FeedbackDecision()};
  report.stability_windows = {StabilityWindow()};
  return report;
}

void PositiveReportIsAdmissible() {
  const auto validation =
      opt::ValidateOptimizerSelectivityObservabilityReportSet(
          {Report()}, {"embedded", "vector"});
  Require(validation.ok, "CEIC-054 valid report set was rejected");
  Require(validation.support_report_admissible,
          "CEIC-054 valid support report was not admissible");
}

void MissingDriftMetricsFailClosed() {
  auto report = Report();
  report.drift_observations.front().estimated_rows = 0.0;
  const auto validation =
      opt::ValidateOptimizerSelectivityObservabilityReport(report);
  Require(!validation.ok, "CEIC-054 missing drift metrics were accepted");
  Require(HasDiagnostic(validation.diagnostics, "DRIFT_METRICS_MISSING"),
          "CEIC-054 missing drift diagnostic absent");
}

void SupportBundleSecurityFailuresFailClosed() {
  auto report = Report();
  report.support_bundle.fresh = false;
  report.support_bundle.trusted_source = false;
  report.support_bundle.redaction_applied = false;
  report.support_bundle.protected_material_redacted = false;
  report.support_bundle.authority.transaction_finality_authority = true;
  report.support_bundle.authority.optimizer_plan_authority = true;
  const auto validation =
      opt::ValidateOptimizerSelectivityObservabilityReport(report);
  Require(!validation.ok, "CEIC-054 unsafe support bundle was accepted");
  Require(HasDiagnostic(validation.diagnostics, "SUPPORT_REPORT_INVALID"),
          "CEIC-054 support invalid diagnostic absent");
  Require(HasDiagnostic(validation.diagnostics, "SUPPORT_FORBIDDEN_AUTHORITY"),
          "CEIC-054 support authority diagnostic absent");
}

void SyntheticReferenceAndAuthorityClaimsFailClosed() {
  auto report = Report();
  report.reference_reference_only = false;
  report.reference_as_authority = true;
  report.uses_reference_storage_or_finality_for_scratchbird = true;
  report.authority.recovery_authority = true;
  report.drift_observations.front().synthetic_only = true;
  report.support_bundle.synthetic_only = true;
  const auto validation =
      opt::ValidateOptimizerSelectivityObservabilityReport(report);
  Require(!validation.ok,
          "CEIC-054 synthetic reference/authority claims were accepted");
  Require(HasDiagnostic(validation.diagnostics, "REFERENCE_AUTHORITY_DRIFT"),
          "CEIC-054 reference authority diagnostic absent");
  Require(HasDiagnostic(validation.diagnostics, "FORBIDDEN_AUTHORITY"),
          "CEIC-054 forbidden authority diagnostic absent");
  Require(HasDiagnostic(validation.diagnostics, "SYNTHETIC_OR_PLACEHOLDER"),
          "CEIC-054 synthetic drift diagnostic absent");
  Require(HasDiagnostic(validation.diagnostics, "SYNTHETIC_SUPPORT_REPORT"),
          "CEIC-054 synthetic support diagnostic absent");
}

void PlaceholderEpochAndContractFailClosed() {
  auto report = Report();
  report.drift_observations.front().result_contract_hash = "result-contract-v1";
  report.drift_observations.front().catalog_epoch = 1;
  report.drift_observations.front().provider_generation = 1;
  const auto validation =
      opt::ValidateOptimizerSelectivityObservabilityReport(report);
  Require(!validation.ok,
          "CEIC-054 placeholder epoch/contract was accepted");
  Require(HasDiagnostic(validation.diagnostics, "PLACEHOLDER_OR_HASH_MISSING"),
          "CEIC-054 placeholder result contract diagnostic absent");
  Require(HasDiagnostic(validation.diagnostics, "PLACEHOLDER_EPOCH"),
          "CEIC-054 placeholder epoch diagnostic absent");
}

void AnnSpillPlanFlipFeedbackAndWindowFailuresFailClosed() {
  auto ann = Report();
  ann.ann_recall.front().exact_rerank_proven = false;
  auto ann_validation =
      opt::ValidateOptimizerSelectivityObservabilityReport(ann);
  Require(!ann_validation.ok, "CEIC-054 ANN recall gap was accepted");
  Require(HasDiagnostic(ann_validation.diagnostics, "ANN_RECALL_INVALID"),
          "CEIC-054 ANN recall diagnostic absent");

  auto spill = Report();
  spill.spill_predictions.front().predicted_spill = false;
  auto spill_validation =
      opt::ValidateOptimizerSelectivityObservabilityReport(spill);
  Require(!spill_validation.ok,
          "CEIC-054 spill prediction mismatch was accepted");
  Require(HasDiagnostic(spill_validation.diagnostics,
                        "SPILL_PREDICTION_INVALID"),
          "CEIC-054 spill prediction diagnostic absent");

  auto flip = Report();
  flip.plan_flips.front().reason = opt::OptimizerPlanFlipReason::kUnclassified;
  flip.plan_flips.front().reason_classified = false;
  auto flip_validation =
      opt::ValidateOptimizerSelectivityObservabilityReport(flip);
  Require(!flip_validation.ok,
          "CEIC-054 unclassified plan flip was accepted");
  Require(HasDiagnostic(flip_validation.diagnostics,
                        "PLAN_FLIP_CLASSIFICATION_MISSING"),
          "CEIC-054 plan flip diagnostic absent");

  auto feedback = Report();
  feedback.feedback_decisions.front().applied_to_plan_authority = true;
  auto feedback_validation =
      opt::ValidateOptimizerSelectivityObservabilityReport(feedback);
  Require(!feedback_validation.ok,
          "CEIC-054 feedback plan authority was accepted");
  Require(HasDiagnostic(feedback_validation.diagnostics,
                        "FEEDBACK_PROVENANCE_INVALID"),
          "CEIC-054 feedback diagnostic absent");

  auto window = Report();
  window.stability_windows.front().plan_hash_continuity_proven = false;
  auto window_validation =
      opt::ValidateOptimizerSelectivityObservabilityReport(window);
  Require(!window_validation.ok,
          "CEIC-054 unstable window was accepted");
  Require(HasDiagnostic(window_validation.diagnostics,
                        "STABILITY_WINDOW_INVALID"),
          "CEIC-054 stability window diagnostic absent");
}

void ClusterModesRemainClaimBlocked() {
  auto local = Report();
  local.cluster_mode = opt::OptimizerObservabilityClusterMode::kLocalClusterEvidence;
  const auto local_validation =
      opt::ValidateOptimizerSelectivityObservabilityReport(local);
  Require(!local_validation.ok,
          "CEIC-054 local cluster observability was accepted");
  Require(HasDiagnostic(local_validation.diagnostics, "LOCAL_CLUSTER_FORBIDDEN"),
          "CEIC-054 local cluster diagnostic absent");

  auto external = Report();
  external.cluster_mode =
      opt::OptimizerObservabilityClusterMode::kExternalProviderDelegated;
  external.external_cluster_provider_id = "external-cluster-provider";
  external.cluster_claim_blocked = true;
  external.production_observability_claim = false;
  const auto external_validation =
      opt::ValidateOptimizerSelectivityObservabilityReport(external);
  Require(external_validation.ok,
          "CEIC-054 blocked external cluster delegation was rejected");
  Require(!external_validation.support_report_admissible,
          "CEIC-054 external cluster delegation became admissible");

  external.production_observability_claim = true;
  const auto overclaim =
      opt::ValidateOptimizerSelectivityObservabilityReport(external);
  Require(!overclaim.ok, "CEIC-054 external cluster overclaim was accepted");
  Require(HasDiagnostic(overclaim.diagnostics,
                        "EXTERNAL_CLUSTER_CLAIM_BLOCK_REQUIRED"),
          "CEIC-054 external cluster overclaim diagnostic absent");
}

void RequiredRoutesRemainExplicit() {
  const auto validation =
      opt::ValidateOptimizerSelectivityObservabilityReportSet({Report()},
                                                              {"embedded", "inet"});
  Require(!validation.ok, "CEIC-054 missing route was accepted");
  Require(HasDiagnostic(validation.diagnostics, "MISSING_ROUTE"),
          "CEIC-054 missing route diagnostic absent");
}

}  // namespace

int main() {
  PositiveReportIsAdmissible();
  MissingDriftMetricsFailClosed();
  SupportBundleSecurityFailuresFailClosed();
  SyntheticReferenceAndAuthorityClaimsFailClosed();
  PlaceholderEpochAndContractFailClosed();
  AnnSpillPlanFlipFeedbackAndWindowFailuresFailClosed();
  ClusterModesRemainClaimBlocked();
  RequiredRoutesRemainExplicit();
  std::cout
      << "ceic_054_selectivity_drift_plan_stability_observability_gate=pass\n";
  return EXIT_SUCCESS;
}

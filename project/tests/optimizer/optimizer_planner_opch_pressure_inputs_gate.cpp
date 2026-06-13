// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "agent_optimizer_recommendation_bridge.hpp"
#include "optimizer_cost_full.hpp"
#include "optimizer_memory_feedback_bridge.hpp"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace agents = scratchbird::core::agents;
namespace opt = scratchbird::engine::optimizer;

namespace {

bool Require(bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << "OPCH pressure-input gate failure: " << message << '\n';
    return false;
  }
  return true;
}

bool Has(const std::vector<std::string>& values, const std::string& expected) {
  return std::find(values.begin(), values.end(), expected) != values.end();
}

opt::CostVector BaseCost() {
  opt::CostVector cost;
  cost.startup_cost = 10;
  cost.row_cost = 100;
  cost.io_cost = 20;
  cost.memory_cost = 5;
  cost.total_cost = 135;
  cost.selectable = true;
  cost.confidence = opt::CostConfidence::kHigh;
  cost.reason = "base";
  return cost;
}

bool MgaPressureCostsOnlyTrustedObservedMetrics() {
  // SEARCH_KEY: OPCH_MGA_PRESSURE_COSTING
  opt::OptimizerMgaPressureEvidence pressure;
  pressure.cleanup_debt_bytes = 4096 * 3;
  pressure.retained_dead_bytes = 4096 * 2;
  pressure.index_backlog_entries = 17;
  pressure.chain_depth_bucket = 4;
  pressure.chain_scatter_bucket = 2;
  pressure.same_page_update_ratio = 0.5;
  pressure.commit_fence_backlog = 9;
  pressure.observed_runtime_metric = true;
  pressure.trusted_metric_digest = true;
  pressure.fresh = true;
  pressure.exact_recheck_required = true;
  pressure.mga_visibility_recheck_required = true;
  pressure.security_recheck_required = true;

  const auto adjusted = opt::ApplyMgaPressureCost(BaseCost(), pressure);
  auto unsafe = pressure;
  unsafe.parser_or_reference_authority = true;
  const auto rejected = opt::ApplyMgaPressureCost(BaseCost(), unsafe);
  auto missing_recheck = pressure;
  missing_recheck.exact_recheck_required = false;
  const auto recheck_rejected = opt::ApplyMgaPressureCost(BaseCost(), missing_recheck);

  return Require(adjusted.selectable, "trusted MGA pressure rejected") &&
         Require(adjusted.total_cost > BaseCost().total_cost,
                 "MGA pressure did not increase cost") &&
         Require(adjusted.reason.find("mga_pressure_costed") != std::string::npos,
                 "MGA pressure cost reason missing") &&
         Require(!rejected.selectable &&
                     rejected.rejection_reason ==
                         "mga_pressure_untrusted_or_unsafe_authority",
                 "unsafe MGA pressure authority accepted") &&
         Require(!recheck_rejected.selectable &&
                     recheck_rejected.rejection_reason ==
                         "mga_pressure_recheck_proof_required",
                 "MGA pressure without exact/MGA/security recheck accepted");
}

opt::OptimizerMemoryFeedbackEvidence MemoryFeedback() {
  opt::OptimizerMemoryFeedbackEvidence feedback;
  feedback.query_uuid = "query:opch061";
  feedback.scope_uuid = "scope:opch061";
  feedback.route_kind = "sql_select";
  feedback.operator_family = "hash_join";
  feedback.plan_shape = "join_hash";
  feedback.route_label = "opch061/hash_join";
  feedback.plan_node_id = "opch061-plan-node-hash-join";
  feedback.provenance_digest = "sha256:opch061-memory-feedback-provenance";
  feedback.redaction_digest = "sha256:opch061-memory-feedback-redaction";
  feedback.metric_snapshot_digest = "sha256:opch061-memory-feedback-metrics";
  feedback.reservation_id = "reservation-opch061-hash-join";
  feedback.reservation_token = "reservation-token-opch061-hash-join";
  feedback.reservation_generation = 13;
  feedback.policy_generation = 7;
  feedback.feedback_generation = 11;
  feedback.catalog_epoch = 17;
  feedback.security_epoch = 19;
  feedback.redaction_epoch = 23;
  feedback.statistics_epoch = 29;
  feedback.observed_timestamp_ticks = 100;
  feedback.received_timestamp_ticks = 150;
  feedback.max_age_ticks = 1000;
  feedback.memory_grant_bytes = 1024;
  feedback.peak_memory_bytes = 4096;
  feedback.spill_bytes = 8192;
  feedback.allocation_failure_count = 1;
  feedback.governed_reservation = true;
  feedback.reservation_token_bound = true;
  feedback.resource_governance_ledger_recorded = true;
  feedback.protected_material_redacted = true;
  feedback.advisory_only = true;
  feedback.mga_visibility_recheck_preserved = true;
  feedback.security_recheck_preserved = true;
  return feedback;
}

bool MemoryFeedbackRequiresGovernedTrustedEvidence() {
  // SEARCH_KEY: OPCH_MEMORY_GRANT_FEEDBACK_SPILL_COSTING
  const auto accepted = opt::BuildOptimizerMemoryFeedbackForPlanner(MemoryFeedback());
  auto synthetic = MemoryFeedback();
  synthetic.synthetic = true;
  const auto rejected = opt::BuildOptimizerMemoryFeedbackForPlanner(synthetic);

  return Require(accepted.ok(), "trusted governed memory feedback rejected") &&
         Require(Has(accepted.evidence, "optimizer_memory_feedback.accepted=true"),
                 "accepted memory feedback evidence missing") &&
         Require(accepted.runtime_feedback.actual_spill_bytes == 8192,
                 "spill bytes not transferred into runtime feedback") &&
         Require(accepted.runtime_feedback.memory_grant_bytes == 1024,
                 "memory grant not transferred into runtime feedback") &&
         Require(!rejected.ok() &&
                     rejected.diagnostic_code ==
                         "SB_OPTIMIZER_MEMORY_FEEDBACK.SYNTHETIC",
                 "synthetic memory feedback accepted");
}

opt::OptimizerRuntimeFeedback RuntimeFeedback() {
  opt::OptimizerRuntimeFeedback feedback;
  feedback.operator_family = "agent_guided_hash_join";
  feedback.plan_shape = "join_hash";
  feedback.cost_profile_id = "agent:opch062";
  feedback.estimated_rows = 100;
  feedback.actual_rows = 5000;
  feedback.estimated_pages = 10;
  feedback.actual_pages = 50;
  feedback.estimated_io_operations = 10;
  feedback.actual_io_operations = 40;
  feedback.estimated_visibility_recheck_rows = 100;
  feedback.actual_visibility_recheck_rows = 5000;
  feedback.estimated_spill_bytes = 0;
  feedback.actual_spill_bytes = 4096;
  feedback.memory_grant_bytes = 1024;
  feedback.peak_memory_bytes = 4096;
  feedback.estimated_latency_microseconds = 100;
  feedback.actual_latency_microseconds = 1000;
  feedback.estimated_resource_units = 100;
  feedback.actual_resource_units = 500;
  feedback.policy_allowed = true;
  feedback.advisory_only = true;
  feedback.mga_visibility_recheck_preserved = true;
  feedback.transaction_finality_authority = "engine_transaction_inventory";
  return feedback;
}

agents::AgentOptimizerRecommendationEvidence AgentEvidence() {
  agents::AgentOptimizerRecommendationEvidence evidence;
  evidence.recommendation_uuid = "agent-rec:opch062";
  evidence.agent_type_id = "storage_health_manager";
  evidence.evidence_uuid = "agent-evidence:opch062";
  evidence.metric_digest = "metric-digest:opch062";
  evidence.scope_uuid = "scope:opch062";
  evidence.recommendation_kind = "avoid";
  evidence.redaction_class = "standard";
  evidence.principal_uuid = "principal:engine";
  evidence.policy_generation = 9;
  evidence.observed_policy_generation = 9;
  evidence.durable_catalog_state = true;
  evidence.strict_metric_snapshot = true;
  evidence.metric_trusted = true;
  evidence.metric_fresh = true;
  return evidence;
}

agents::AgentIndexReadinessEvidence IndexReadiness() {
  agents::AgentIndexReadinessEvidence evidence;
  evidence.evidence_digest = "sha256:opch:index-readiness";
  evidence.family_id = "btree";
  evidence.manifest_generation = 42;
  evidence.observed_manifest_generation = 42;
  evidence.present = true;
  evidence.ceic_042_complete = true;
  evidence.freshness_gate_complete = true;
  evidence.route_capability_complete = true;
  evidence.provider_closure_complete = true;
  evidence.metric_producer_complete = true;
  evidence.crash_cleanup_corruption_complete = true;
  evidence.artifact_registration_complete = true;
  return evidence;
}

agents::AgentOptimizerReadinessEvidence OptimizerReadiness() {
  agents::AgentOptimizerReadinessEvidence evidence;
  evidence.evidence_digest = "sha256:opch:optimizer-readiness";
  evidence.manifest_generation = 62;
  evidence.observed_manifest_generation = 62;
  evidence.present = true;
  evidence.ceic_062_complete = true;
  evidence.live_routes_complete = true;
  evidence.benchmark_evidence_complete = true;
  evidence.correctness_oracles_complete = true;
  evidence.crash_reopen_complete = true;
  evidence.metrics_feedback_complete = true;
  evidence.transformation_memo_complete = true;
  evidence.workload_regression_complete = true;
  evidence.driver_explain_complete = true;
  evidence.reference_comparison_complete = true;
  evidence.memory_feedback_complete = true;
  evidence.index_readiness_coupling_complete = true;
  evidence.llvm_memory_accounting_complete = true;
  return evidence;
}

bool AgentRecommendationsRequireDurableScopedMetrics() {
  // SEARCH_KEY: OPCH_AGENT_RECOMMENDATION_METRIC_INTEGRATION
  opt::OptimizerAgentRecommendationRequest request;
  request.agent_evidence = AgentEvidence();
  request.index_readiness = IndexReadiness();
  request.optimizer_readiness = OptimizerReadiness();
  request.requested_action =
      agents::AgentIndexOptimizerBoundaryActionKind::
          optimizer_learning_advisory_note;
  request.workflow_uuid = "019f0930-0000-7000-8000-000000000062";
  request.feedback = RuntimeFeedback();
  const auto accepted = opt::EvaluateOptimizerAgentRecommendation(request);

  auto unsafe = request;
  unsafe.agent_evidence.sidecar_only_state = true;
  unsafe.agent_evidence.durable_catalog_state = false;
  const auto rejected = opt::EvaluateOptimizerAgentRecommendation(unsafe);

  return Require(accepted.ok && accepted.benchmark_clean && !accepted.fail_closed,
                 "durable scoped agent recommendation rejected") &&
         Require(Has(accepted.evidence, "optimizer_agent_recommendation.accepted=true"),
                 "accepted agent recommendation evidence missing") &&
         Require(Has(accepted.evidence, "optimizer_agent_recommendation.finality_authority=false"),
                 "agent finality non-authority evidence missing") &&
         Require(!rejected.ok &&
                     rejected.diagnostic_code ==
                         "SB_AGENT_OPTIMIZER_RECOMMENDATION.DURABLE_STATE_REQUIRED",
                 "sidecar-only agent recommendation accepted");
}

}  // namespace

int main() {
  if (!MgaPressureCostsOnlyTrustedObservedMetrics()) return EXIT_FAILURE;
  if (!MemoryFeedbackRequiresGovernedTrustedEvidence()) return EXIT_FAILURE;
  if (!AgentRecommendationsRequireDurableScopedMetrics()) return EXIT_FAILURE;
  return EXIT_SUCCESS;
}

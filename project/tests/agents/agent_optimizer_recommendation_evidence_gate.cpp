// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "agent_optimizer_recommendation_bridge.hpp"

#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>

namespace agents = scratchbird::core::agents;
namespace opt = scratchbird::engine::optimizer;

namespace {

void Require(bool condition, const std::string& message) {
  if (!condition) { throw std::runtime_error(message); }
}

agents::AgentOptimizerRecommendationEvidence AgentEvidence() {
  agents::AgentOptimizerRecommendationEvidence evidence;
  evidence.recommendation_uuid = "019f0930-0000-7000-8000-000000000001";
  evidence.agent_type_id = "storage_health_manager";
  evidence.evidence_uuid = "019f0930-0000-7000-8000-000000000002";
  evidence.metric_digest = "metric-digest-arhc-093";
  evidence.scope_uuid = "019f0930-0000-7000-8000-000000000003";
  evidence.recommendation_kind = "optimizer_storage_cost";
  evidence.redaction_class = "standard";
  evidence.principal_uuid = "019f0930-0000-7000-8000-000000000004";
  evidence.policy_generation = 31;
  evidence.observed_policy_generation = 31;
  evidence.durable_catalog_state = true;
  evidence.strict_metric_snapshot = true;
  evidence.metric_trusted = true;
  evidence.metric_fresh = true;
  return evidence;
}

opt::OptimizerRuntimeFeedback Feedback() {
  opt::OptimizerRuntimeFeedback feedback;
  feedback.operator_family = "storage_scan";
  feedback.plan_shape = "storage_health_cost_recommendation";
  feedback.cost_profile_id = "agent-recommendation-v1";
  feedback.estimated_rows = 100;
  feedback.actual_rows = 120;
  feedback.estimated_pages = 10;
  feedback.actual_pages = 12;
  feedback.estimated_io_operations = 10;
  feedback.actual_io_operations = 12;
  feedback.estimated_visibility_recheck_rows = 5;
  feedback.actual_visibility_recheck_rows = 5;
  feedback.memory_grant_bytes = 1024 * 1024;
  feedback.peak_memory_bytes = 256 * 1024;
  feedback.freshness_microseconds = 10;
  feedback.max_freshness_microseconds = 1000;
  feedback.policy_allowed = true;
  feedback.advisory_only = true;
  feedback.mga_visibility_recheck_preserved = true;
  feedback.transaction_finality_authority = "engine_transaction_inventory";
  return feedback;
}

agents::AgentIndexReadinessEvidence IndexReadiness() {
  agents::AgentIndexReadinessEvidence evidence;
  evidence.evidence_digest = "sha256:ceic042:index-readiness";
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
  evidence.evidence_digest = "sha256:ceic062:optimizer-readiness";
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
  evidence.donor_comparison_complete = true;
  evidence.memory_feedback_complete = true;
  evidence.index_readiness_coupling_complete = true;
  evidence.llvm_memory_accounting_complete = true;
  return evidence;
}

opt::OptimizerAgentRecommendationResult Evaluate(
    agents::AgentOptimizerRecommendationEvidence evidence,
    opt::OptimizerRuntimeFeedback feedback = Feedback()) {
  opt::OptimizerAgentRecommendationRequest request;
  request.agent_evidence = std::move(evidence);
  request.index_readiness = IndexReadiness();
  request.optimizer_readiness = OptimizerReadiness();
  request.requested_action =
      agents::AgentIndexOptimizerBoundaryActionKind::
          optimizer_learning_advisory_note;
  request.workflow_uuid = "019f0930-0000-7000-8000-000000000005";
  request.feedback = std::move(feedback);
  return opt::EvaluateOptimizerAgentRecommendation(request);
}

bool HasEvidence(const std::vector<std::string>& evidence,
                 const std::string& expected) {
  for (const auto& row : evidence) {
    if (row == expected) { return true; }
  }
  return false;
}

void TestValidDurableRecommendationAccepted() {
  const auto result = Evaluate(AgentEvidence());
  Require(result.ok && result.benchmark_clean,
          "valid durable optimizer recommendation was refused: " +
              result.diagnostic_code);
  Require(result.diagnostic_code == "SB_OPTIMIZER_AGENT_RECOMMENDATION.OK",
          "accepted diagnostic mismatch");
  Require(HasEvidence(result.evidence,
                      "optimizer_agent_recommendation.benchmark_clean=true"),
          "benchmark-clean evidence missing");
  Require(HasEvidence(result.evidence,
                      "optimizer_agent_recommendation.finality_authority=false"),
          "authority non-drift evidence missing");
  Require(HasEvidence(result.evidence,
                      "optimizer_agent_recommendation.optimizer_plan_authority=false"),
          "optimizer plan non-authority evidence missing");
  Require(HasEvidence(result.evidence,
                      "optimizer_agent_recommendation.index_finality_authority=false"),
          "index finality non-authority evidence missing");
}

void TestUnsafeAgentEvidenceRejected() {
  auto sidecar = AgentEvidence();
  sidecar.durable_catalog_state = false;
  sidecar.sidecar_only_state = true;
  auto result = Evaluate(sidecar);
  Require(!result.ok &&
              result.diagnostic_code ==
                  "SB_AGENT_OPTIMIZER_RECOMMENDATION.DURABLE_STATE_REQUIRED",
          "sidecar optimizer recommendation was accepted");

  auto relaxed = AgentEvidence();
  relaxed.strict_metric_snapshot = false;
  relaxed.relaxed_metric_path = true;
  result = Evaluate(relaxed);
  Require(!result.ok &&
              result.diagnostic_code ==
                  "SB_AGENT_OPTIMIZER_RECOMMENDATION.STRICT_METRIC_REQUIRED",
          "relaxed metric optimizer recommendation was accepted");

  auto stale_policy = AgentEvidence();
  stale_policy.observed_policy_generation = 30;
  result = Evaluate(stale_policy);
  Require(!result.ok &&
              result.diagnostic_code ==
                  "SB_AGENT_OPTIMIZER_RECOMMENDATION.POLICY_GENERATION_REQUIRED",
          "stale policy optimizer recommendation was accepted");

  auto untrusted = AgentEvidence();
  untrusted.parser_authority = true;
  result = Evaluate(untrusted);
  Require(!result.ok &&
              result.diagnostic_code ==
                  "SB_AGENT_OPTIMIZER_RECOMMENDATION.UNSAFE_AUTHORITY",
          "unsafe authority optimizer recommendation was accepted");
}

void TestProtectedMaterialSuppressedButAdvisoryAllowed() {
  auto protected_evidence = AgentEvidence();
  protected_evidence.redaction_class = "protected_material";
  protected_evidence.protected_material = true;
  const auto result = Evaluate(protected_evidence);
  Require(result.ok, "protected recommendation should remain advisory after suppression");
  Require(result.agent_validation.protected_material_suppressed,
          "protected material suppression flag missing");
  Require(HasEvidence(result.evidence,
                      "optimizer_agent_recommendation.redacted=true"),
          "redaction evidence missing");
}

void TestUnsafeOptimizerFeedbackRejected() {
  auto feedback = Feedback();
  feedback.parser_or_donor_authority = true;
  const auto result = Evaluate(AgentEvidence(), feedback);
  Require(!result.ok &&
              result.diagnostic_code == "SB_OPTIMIZER_FEEDBACK.REJECTED_UNSAFE",
          "unsafe optimizer feedback accepted through agent bridge");
}

}  // namespace

int main() {
  try {
    TestValidDurableRecommendationAccepted();
    TestUnsafeAgentEvidenceRejected();
    TestProtectedMaterialSuppressedButAdvisoryAllowed();
    TestUnsafeOptimizerFeedbackRejected();
  } catch (const std::exception& e) {
    std::cerr << e.what() << '\n';
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}

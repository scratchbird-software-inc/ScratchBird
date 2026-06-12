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
#include <utility>
#include <vector>

namespace agents = scratchbird::core::agents;
namespace opt = scratchbird::engine::optimizer;

namespace {

void Require(bool condition, const std::string& message) {
  if (!condition) { throw std::runtime_error(message); }
}

bool HasEvidence(const std::vector<std::string>& evidence,
                 const std::string& expected) {
  for (const auto& row : evidence) {
    if (row == expected) { return true; }
  }
  return false;
}

agents::AgentOptimizerRecommendationEvidence AgentEvidence() {
  agents::AgentOptimizerRecommendationEvidence evidence;
  evidence.recommendation_uuid = "019f0840-0000-7000-8000-000000000001";
  evidence.agent_type_id = "index_health_manager";
  evidence.evidence_uuid = "019f0840-0000-7000-8000-000000000002";
  evidence.metric_digest = "sha256:ceic084:agent-metric";
  evidence.scope_uuid = "019f0840-0000-7000-8000-000000000003";
  evidence.recommendation_kind = "index_optimizer_advisory";
  evidence.redaction_class = "standard";
  evidence.principal_uuid = "019f0840-0000-7000-8000-000000000004";
  evidence.policy_generation = 84;
  evidence.observed_policy_generation = 84;
  evidence.durable_catalog_state = true;
  evidence.strict_metric_snapshot = true;
  evidence.metric_trusted = true;
  evidence.metric_fresh = true;
  return evidence;
}

agents::AgentIndexReadinessEvidence IndexReadiness() {
  agents::AgentIndexReadinessEvidence evidence;
  evidence.evidence_digest = "sha256:ceic084:ceic042-index-readiness";
  evidence.family_id = "btree";
  evidence.manifest_generation = 42084;
  evidence.observed_manifest_generation = 42084;
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
  evidence.evidence_digest = "sha256:ceic084:ceic062-optimizer-readiness";
  evidence.manifest_generation = 62084;
  evidence.observed_manifest_generation = 62084;
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

agents::AgentIndexOptimizerBoundaryRequest BoundaryRequest(
    agents::AgentIndexOptimizerBoundaryActionKind action =
        agents::AgentIndexOptimizerBoundaryActionKind::
            optimizer_learning_advisory_note) {
  agents::AgentIndexOptimizerBoundaryRequest request;
  request.agent_evidence = AgentEvidence();
  request.index_readiness = IndexReadiness();
  request.optimizer_readiness = OptimizerReadiness();
  request.requested_action = action;
  request.workflow_uuid = "019f0840-0000-7000-8000-000000000005";
  return request;
}

void ExpectBoundaryRefusal(agents::AgentIndexOptimizerBoundaryRequest request,
                           const std::string& diagnostic_code) {
  const auto result = agents::EvaluateAgentIndexOptimizerBoundary(request);
  Require(!result.ok && result.fail_closed,
          "boundary request unexpectedly accepted: " + diagnostic_code);
  Require(result.status.diagnostic_code == diagnostic_code,
          "boundary refusal diagnostic mismatch: " +
              result.status.diagnostic_code + " expected " + diagnostic_code);
  Require(HasEvidence(result.evidence,
                      "agent_index_optimizer_boundary.recommendation_only=true"),
          "refusal lacked recommendation-only evidence");
  Require(HasEvidence(result.evidence,
                      "agent_index_optimizer_boundary.optimizer_plan_authority=false"),
          "refusal lacked optimizer plan non-authority evidence");
  Require(HasEvidence(result.evidence,
                      "agent_index_optimizer_boundary.index_finality_authority=false"),
          "refusal lacked index finality non-authority evidence");
}

void TestAllowedRecommendationOutputs() {
  const agents::AgentIndexOptimizerBoundaryActionKind actions[] = {
      agents::AgentIndexOptimizerBoundaryActionKind::analyze_recommendation,
      agents::AgentIndexOptimizerBoundaryActionKind::index_rebuild_request,
      agents::AgentIndexOptimizerBoundaryActionKind::
          shadow_rollout_recommendation,
      agents::AgentIndexOptimizerBoundaryActionKind::cleanup_recommendation,
      agents::AgentIndexOptimizerBoundaryActionKind::
          statistics_refresh_recommendation,
      agents::AgentIndexOptimizerBoundaryActionKind::
          optimizer_learning_advisory_note};
  for (const auto action : actions) {
    const auto result =
        agents::EvaluateAgentIndexOptimizerBoundary(BoundaryRequest(action));
    Require(result.ok && !result.fail_closed,
            "valid CEIC-084 action refused: " +
                result.status.diagnostic_code);
    Require(result.recommendation_only,
            "CEIC-084 output was not recommendation-only");
    Require(result.output_action == action,
            "CEIC-084 action changed during boundary evaluation");
    Require(result.workflow_output ==
                std::string("recommendation_only.") +
                    agents::AgentIndexOptimizerBoundaryActionKindName(action),
            "CEIC-084 workflow output mismatch");
    Require(HasEvidence(result.evidence,
                        "agent_index_optimizer_boundary.support_bundle_rows_bounded=true"),
            "CEIC-084 bounded support-bundle marker missing");
    Require(HasEvidence(result.evidence,
                        "agent_index_optimizer_boundary.transaction_finality_authority=false"),
            "CEIC-084 transaction finality marker missing");
    Require(HasEvidence(result.evidence,
                        "agent_index_optimizer_boundary.security_authority=false"),
            "CEIC-084 security non-authority marker missing");
    Require(HasEvidence(result.evidence,
                        "agent_index_optimizer_boundary.wal_authority=false"),
            "CEIC-084 wal non-authority marker missing");
  }
}

void TestReadinessRefusals() {
  auto missing_index = BoundaryRequest();
  missing_index.index_readiness.present = false;
  ExpectBoundaryRefusal(
      missing_index,
      "SB_AGENT_INDEX_OPTIMIZER_BOUNDARY.INDEX_READINESS_REQUIRED");

  auto stale_index = BoundaryRequest();
  stale_index.index_readiness.observed_manifest_generation = 1;
  ExpectBoundaryRefusal(
      stale_index,
      "SB_AGENT_INDEX_OPTIMIZER_BOUNDARY.INDEX_READINESS_STALE");

  auto static_index = BoundaryRequest();
  static_index.index_readiness.static_or_smoke_only = true;
  ExpectBoundaryRefusal(
      static_index,
      "SB_AGENT_INDEX_OPTIMIZER_BOUNDARY.INDEX_READINESS_PLACEHOLDER");

  auto missing_optimizer = BoundaryRequest();
  missing_optimizer.optimizer_readiness.present = false;
  ExpectBoundaryRefusal(
      missing_optimizer,
      "SB_AGENT_INDEX_OPTIMIZER_BOUNDARY.OPTIMIZER_READINESS_REQUIRED");

  auto stale_optimizer = BoundaryRequest();
  stale_optimizer.optimizer_readiness.stale_manifest = true;
  ExpectBoundaryRefusal(
      stale_optimizer,
      "SB_AGENT_INDEX_OPTIMIZER_BOUNDARY.OPTIMIZER_READINESS_STALE");

  auto placeholder_optimizer = BoundaryRequest();
  placeholder_optimizer.optimizer_readiness.placeholder_runtime_evidence = true;
  ExpectBoundaryRefusal(
      placeholder_optimizer,
      "SB_AGENT_INDEX_OPTIMIZER_BOUNDARY.OPTIMIZER_READINESS_PLACEHOLDER");
}

void TestAuthorityAndClusterRefusals() {
  auto selected_plan = BoundaryRequest();
  selected_plan.optimizer_selected_plan = true;
  ExpectBoundaryRefusal(selected_plan,
                        "SB_AGENT_INDEX_OPTIMIZER_BOUNDARY.UNSAFE_AUTHORITY");

  auto finality_claim = BoundaryRequest();
  finality_claim.index_readiness.index_finality_authority = true;
  ExpectBoundaryRefusal(finality_claim,
                        "SB_AGENT_INDEX_OPTIMIZER_BOUNDARY.UNSAFE_AUTHORITY");

  auto forbidden_kind = BoundaryRequest();
  forbidden_kind.agent_evidence.recommendation_kind =
      "selected_plan_authority";
  ExpectBoundaryRefusal(forbidden_kind,
                        "SB_AGENT_INDEX_OPTIMIZER_BOUNDARY.UNSAFE_AUTHORITY");

  auto cluster_without_provider = BoundaryRequest();
  cluster_without_provider.index_readiness.cluster_evidence_present = true;
  ExpectBoundaryRefusal(
      cluster_without_provider,
      "SB_AGENT_INDEX_OPTIMIZER_BOUNDARY.CLUSTER_PROVIDER_REQUIRED");

  auto cluster_with_provider = BoundaryRequest();
  cluster_with_provider.index_readiness.cluster_evidence_present = true;
  cluster_with_provider.index_readiness.external_provider_proof = true;
  const auto accepted =
      agents::EvaluateAgentIndexOptimizerBoundary(cluster_with_provider);
  Require(accepted.ok,
          "external-provider-backed cluster evidence should remain advisory");
  Require(HasEvidence(accepted.evidence,
                      "agent_index_optimizer_boundary.cluster_authority=false"),
          "cluster advisory path lacked no-authority marker");
}

opt::OptimizerRuntimeFeedback Feedback() {
  opt::OptimizerRuntimeFeedback feedback;
  feedback.operator_family = "index_maintenance";
  feedback.plan_shape = "agent_index_optimizer_boundary";
  feedback.cost_profile_id = "ceic084";
  feedback.estimated_rows = 100;
  feedback.actual_rows = 110;
  feedback.estimated_pages = 10;
  feedback.actual_pages = 11;
  feedback.estimated_io_operations = 10;
  feedback.actual_io_operations = 11;
  feedback.estimated_visibility_recheck_rows = 4;
  feedback.actual_visibility_recheck_rows = 4;
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

opt::OptimizerAgentRecommendationRequest BridgeRequest() {
  opt::OptimizerAgentRecommendationRequest request;
  request.agent_evidence = AgentEvidence();
  request.index_readiness = IndexReadiness();
  request.optimizer_readiness = OptimizerReadiness();
  request.requested_action =
      agents::AgentIndexOptimizerBoundaryActionKind::index_rebuild_request;
  request.workflow_uuid = "019f0840-0000-7000-8000-000000000006";
  request.feedback = Feedback();
  return request;
}

void TestOptimizerBridgeUsesBoundary() {
  auto accepted = opt::EvaluateOptimizerAgentRecommendation(BridgeRequest());
  Require(accepted.ok && accepted.boundary_result.ok,
          "optimizer bridge refused valid CEIC-084 boundary");
  Require(HasEvidence(accepted.evidence,
                      "optimizer_agent_recommendation.optimizer_plan_authority=false"),
          "optimizer bridge lacked plan non-authority marker");
  Require(HasEvidence(accepted.evidence,
                      "boundary.CEIC_084_AGENT_INDEX_OPTIMIZER_BOUNDARY"),
          "optimizer bridge did not emit boundary evidence");

  auto missing = BridgeRequest();
  missing.index_readiness.present = false;
  auto refused = opt::EvaluateOptimizerAgentRecommendation(missing);
  Require(!refused.ok &&
              refused.diagnostic_code ==
                  "SB_AGENT_INDEX_OPTIMIZER_BOUNDARY.INDEX_READINESS_REQUIRED",
          "optimizer bridge did not fail closed on missing index readiness");

  auto unsafe_feedback = BridgeRequest();
  unsafe_feedback.feedback.parser_or_reference_authority = true;
  refused = opt::EvaluateOptimizerAgentRecommendation(unsafe_feedback);
  Require(!refused.ok &&
              refused.diagnostic_code == "SB_OPTIMIZER_FEEDBACK.REJECTED_UNSAFE",
          "optimizer bridge accepted unsafe feedback after boundary");
}

}  // namespace

int main() {
  try {
    TestAllowedRecommendationOutputs();
    TestReadinessRefusals();
    TestAuthorityAndClusterRefusals();
    TestOptimizerBridgeUsesBoundary();
  } catch (const std::exception& e) {
    std::cerr << e.what() << '\n';
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}

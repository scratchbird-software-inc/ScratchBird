// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "agent_optimizer_recommendation_bridge.hpp"

#include <utility>

namespace scratchbird::engine::optimizer {
namespace {

void Add(OptimizerAgentRecommendationResult* result, std::string value) {
  result->evidence.push_back(std::move(value));
}

OptimizerAgentRecommendationResult Refuse(
    const OptimizerAgentRecommendationRequest& request,
    std::string diagnostic,
    scratchbird::core::agents::AgentOptimizerRecommendationValidation validation = {},
    scratchbird::core::agents::AgentIndexOptimizerBoundaryResult boundary = {}) {
  OptimizerAgentRecommendationResult result;
  result.ok = false;
  result.benchmark_clean = false;
  result.fail_closed = true;
  result.diagnostic_code = std::move(diagnostic);
  result.agent_validation = std::move(validation);
  result.boundary_result = std::move(boundary);
  Add(&result, "ARHC_OPTIMIZER_RECOMMENDATION_EVIDENCE_CONTRACT");
  Add(&result, "CEIC_084_AGENT_INDEX_OPTIMIZER_BOUNDARY");
  Add(&result, "optimizer_agent_recommendation.refused=" + result.diagnostic_code);
  Add(&result, "optimizer_agent_recommendation.agent_type_id=" +
                   request.agent_evidence.agent_type_id);
  Add(&result, "optimizer_agent_recommendation.recommendation_kind=" +
                   request.agent_evidence.recommendation_kind);
  Add(&result, "optimizer_agent_recommendation.benchmark_clean=false");
  Add(&result, "optimizer_agent_recommendation.advisory_only=true");
  Add(&result, "optimizer_agent_recommendation.parser_authority=false");
  Add(&result, "optimizer_agent_recommendation.reference_authority=false");
  Add(&result, "optimizer_agent_recommendation.client_authority=false");
  Add(&result, "optimizer_agent_recommendation.finality_authority=false");
  Add(&result, "optimizer_agent_recommendation.visibility_authority=false");
  Add(&result, "optimizer_agent_recommendation.recovery_authority=false");
  Add(&result, "optimizer_agent_recommendation.security_authority=false");
  Add(&result, "optimizer_agent_recommendation.transaction_finality_authority=false");
  Add(&result, "optimizer_agent_recommendation.wal_authority=false");
  Add(&result, "optimizer_agent_recommendation.benchmark_authority=false");
  Add(&result, "optimizer_agent_recommendation.optimizer_plan_authority=false");
  Add(&result, "optimizer_agent_recommendation.optimizer_selected_plan=false");
  Add(&result, "optimizer_agent_recommendation.index_finality_authority=false");
  Add(&result, "optimizer_agent_recommendation.provider_finality_authority=false");
  Add(&result, "optimizer_agent_recommendation.cluster_authority=false");
  Add(&result, "optimizer_agent_recommendation.memory_authority=false");
  Add(&result, "optimizer_agent_recommendation.agent_action_authority=false");
  for (const auto& evidence : result.boundary_result.evidence) {
    Add(&result, "boundary." + evidence);
  }
  return result;
}

}  // namespace

OptimizerAgentRecommendationResult EvaluateOptimizerAgentRecommendation(
    const OptimizerAgentRecommendationRequest& request) {
  auto agent_validation =
      scratchbird::core::agents::ValidateAgentOptimizerRecommendationEvidence(
          request.agent_evidence);
  if (!agent_validation.status.ok) {
    return Refuse(request,
                  agent_validation.status.diagnostic_code,
                  agent_validation);
  }

  scratchbird::core::agents::AgentIndexOptimizerBoundaryRequest boundary_request;
  boundary_request.agent_evidence = request.agent_evidence;
  boundary_request.index_readiness = request.index_readiness;
  boundary_request.optimizer_readiness = request.optimizer_readiness;
  boundary_request.requested_action = request.requested_action;
  boundary_request.workflow_uuid = request.workflow_uuid;
  boundary_request.recommendation_only = request.recommendation_only;
  boundary_request.scheduling_output_allowed = request.scheduling_output_allowed;
  boundary_request.bounded_diagnostics_required =
      request.bounded_diagnostics_required;
  boundary_request.support_bundle_rows_required =
      request.support_bundle_rows_required;
  boundary_request.optimizer_selected_plan = request.optimizer_selected_plan;
  boundary_request.optimizer_plan_authority = request.optimizer_plan_authority;
  boundary_request.index_finality_authority = request.index_finality_authority;
  boundary_request.row_visibility_authority = request.row_visibility_authority;
  boundary_request.security_authority = request.security_authority;
  boundary_request.transaction_finality_authority =
      request.transaction_finality_authority;
  boundary_request.recovery_authority = request.recovery_authority;
  boundary_request.parser_authority = request.parser_authority;
  boundary_request.reference_authority = request.reference_authority;
  boundary_request.wal_authority = request.wal_authority;
  boundary_request.benchmark_authority = request.benchmark_authority;
  boundary_request.provider_finality_authority =
      request.provider_finality_authority;
  boundary_request.cluster_authority = request.cluster_authority;
  boundary_request.memory_authority = request.memory_authority;
  boundary_request.agent_action_authority = request.agent_action_authority;

  auto boundary =
      scratchbird::core::agents::EvaluateAgentIndexOptimizerBoundary(
          boundary_request);
  if (!boundary.ok) {
    return Refuse(request,
                  boundary.status.diagnostic_code,
                  boundary.agent_validation,
                  boundary);
  }

  OptimizerAgentRecommendationResult result;
  result.agent_validation = boundary.agent_validation;
  result.boundary_result = boundary;
  result.feedback_status = EvaluateOptimizerRuntimeFeedback(request.feedback);
  if (!result.feedback_status.ok || !result.feedback_status.applied) {
    return Refuse(request,
                  result.feedback_status.diagnostic_code,
                  boundary.agent_validation,
                  boundary);
  }

  result.ok = true;
  result.benchmark_clean = true;
  result.fail_closed = false;
  result.diagnostic_code = "SB_OPTIMIZER_AGENT_RECOMMENDATION.OK";
  Add(&result, "ARHC_OPTIMIZER_RECOMMENDATION_EVIDENCE_CONTRACT");
  Add(&result, "optimizer_agent_recommendation.accepted=true");
  Add(&result, "optimizer_agent_recommendation.agent_type_id=" +
                   request.agent_evidence.agent_type_id);
  Add(&result, "optimizer_agent_recommendation.recommendation_kind=" +
                   request.agent_evidence.recommendation_kind);
  Add(&result, "optimizer_agent_recommendation.policy_generation=" +
                   std::to_string(request.agent_evidence.policy_generation));
  Add(&result, "optimizer_agent_recommendation.metric_digest_present=true");
  Add(&result, "optimizer_agent_recommendation.redacted=" +
                   std::string(boundary.agent_validation.redacted ? "true" : "false"));
  Add(&result, "optimizer_agent_recommendation.benchmark_clean=true");
  Add(&result, "optimizer_agent_recommendation.advisory_only=true");
  Add(&result, "optimizer_agent_recommendation.parser_authority=false");
  Add(&result, "optimizer_agent_recommendation.reference_authority=false");
  Add(&result, "optimizer_agent_recommendation.client_authority=false");
  Add(&result, "optimizer_agent_recommendation.finality_authority=false");
  Add(&result, "optimizer_agent_recommendation.visibility_authority=false");
  Add(&result, "optimizer_agent_recommendation.recovery_authority=false");
  Add(&result, "optimizer_agent_recommendation.security_authority=false");
  Add(&result, "optimizer_agent_recommendation.transaction_finality_authority=false");
  Add(&result, "optimizer_agent_recommendation.wal_authority=false");
  Add(&result, "optimizer_agent_recommendation.benchmark_authority=false");
  Add(&result, "optimizer_agent_recommendation.optimizer_plan_authority=false");
  Add(&result, "optimizer_agent_recommendation.optimizer_selected_plan=false");
  Add(&result, "optimizer_agent_recommendation.index_finality_authority=false");
  Add(&result, "optimizer_agent_recommendation.provider_finality_authority=false");
  Add(&result, "optimizer_agent_recommendation.cluster_authority=false");
  Add(&result, "optimizer_agent_recommendation.memory_authority=false");
  Add(&result, "optimizer_agent_recommendation.agent_action_authority=false");
  for (const auto& evidence : boundary.agent_validation.evidence) {
    Add(&result, "agent_validation." + evidence);
  }
  for (const auto& evidence : boundary.evidence) {
    Add(&result, "boundary." + evidence);
  }
  for (const auto& evidence : result.feedback_status.evidence) {
    Add(&result, "optimizer_feedback." + evidence);
  }
  return result;
}

}  // namespace scratchbird::engine::optimizer

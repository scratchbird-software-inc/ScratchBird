// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// SEARCH_KEY: ARHC_OPTIMIZER_RECOMMENDATION_EVIDENCE_CONTRACT

#include "agent_optimizer_recommendation.hpp"
#include "optimizer_feedback.hpp"

#include <string>
#include <vector>

namespace scratchbird::engine::optimizer {

// SEARCH_KEY: OPCH_AGENT_RECOMMENDATION_METRIC_INTEGRATION
struct OptimizerAgentRecommendationRequest {
  scratchbird::core::agents::AgentOptimizerRecommendationEvidence agent_evidence;
  scratchbird::core::agents::AgentIndexReadinessEvidence index_readiness;
  scratchbird::core::agents::AgentOptimizerReadinessEvidence optimizer_readiness;
  scratchbird::core::agents::AgentIndexOptimizerBoundaryActionKind
      requested_action =
          scratchbird::core::agents::AgentIndexOptimizerBoundaryActionKind::
              optimizer_learning_advisory_note;
  std::string workflow_uuid;
  bool recommendation_only = true;
  bool scheduling_output_allowed = true;
  bool bounded_diagnostics_required = true;
  bool support_bundle_rows_required = true;
  bool optimizer_selected_plan = false;
  bool optimizer_plan_authority = false;
  bool index_finality_authority = false;
  bool row_visibility_authority = false;
  bool security_authority = false;
  bool transaction_finality_authority = false;
  bool recovery_authority = false;
  bool parser_authority = false;
  bool reference_authority = false;
  bool wal_authority = false;
  bool benchmark_authority = false;
  bool provider_finality_authority = false;
  bool cluster_authority = false;
  bool memory_authority = false;
  bool agent_action_authority = false;
  OptimizerRuntimeFeedback feedback;
};

struct OptimizerAgentRecommendationResult {
  bool ok = false;
  bool benchmark_clean = false;
  bool fail_closed = true;
  std::string diagnostic_code;
  scratchbird::core::agents::AgentOptimizerRecommendationValidation agent_validation;
  scratchbird::core::agents::AgentIndexOptimizerBoundaryResult boundary_result;
  OptimizerFeedbackStatus feedback_status;
  std::vector<std::string> evidence;
};

OptimizerAgentRecommendationResult EvaluateOptimizerAgentRecommendation(
    const OptimizerAgentRecommendationRequest& request);

}  // namespace scratchbird::engine::optimizer

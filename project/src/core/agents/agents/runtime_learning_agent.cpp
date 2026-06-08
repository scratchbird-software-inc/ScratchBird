// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "agents/runtime_learning_agent.hpp"

// CanonicalAgentRegistry/CanonicalAgentManifest owns production exposure;
// this file provides the local runtime-learning advisory handler.

#include <utility>

namespace scratchbird::core::agents::implemented_agents {
namespace {

using scratchbird::core::platform::Severity;
using scratchbird::core::platform::StatusCode;
using scratchbird::core::platform::Subsystem;

Status OkStatus() { return {StatusCode::ok, Severity::info, Subsystem::engine}; }
Status ErrorStatus() {
  return {StatusCode::memory_invalid_request, Severity::error, Subsystem::engine};
}

void AddEvidence(RuntimeLearningAgentResult* result,
                 std::string key,
                 std::string value) {
  result->evidence.push_back({std::move(key), std::move(value)});
}

RuntimeLearningAgentResult Finish(RuntimeLearningAgentDecisionKind decision,
                                  Status status,
                                  std::string code,
                                  std::string key,
                                  std::string detail,
                                  bool fail_closed) {
  RuntimeLearningAgentResult result;
  result.status = status;
  result.decision = decision;
  result.fail_closed = fail_closed;
  result.diagnostic = MakeRuntimeLearningAgentDiagnostic(
      result.status, std::move(code), std::move(key), std::move(detail));
  AddEvidence(&result, "decision",
              RuntimeLearningAgentDecisionKindName(result.decision));
  AddEvidence(&result, "advisory_only", "true");
  return result;
}

}  // namespace

const char* RuntimeLearningAgentDecisionKindName(
    RuntimeLearningAgentDecisionKind decision) {
  switch (decision) {
    case RuntimeLearningAgentDecisionKind::no_action: return "no_action";
    case RuntimeLearningAgentDecisionKind::recommend_planner_correction:
      return "recommend_planner_correction";
    case RuntimeLearningAgentDecisionKind::quarantine_learning_shape:
      return "quarantine_learning_shape";
    case RuntimeLearningAgentDecisionKind::refused: return "refused";
  }
  return "refused";
}

DiagnosticRecord MakeRuntimeLearningAgentDiagnostic(Status status,
                                                    std::string diagnostic_code,
                                                    std::string message_key,
                                                    std::string detail) {
  return scratchbird::core::platform::MakeDiagnostic(
      status.code, status.severity, status.subsystem,
      std::move(diagnostic_code), std::move(message_key),
      {{"detail", std::move(detail)}}, {}, "runtime_learning_agent", {});
}

RuntimeLearningAgentResult EvaluateRuntimeLearningAgent(
    const RuntimeLearningAgentSnapshot& snapshot,
    const RuntimeLearningAgentPolicy& policy) {
  if (!policy.present || !policy.valid || !policy.scope_compatible ||
      snapshot.query_shape_digest.empty()) {
    return Finish(RuntimeLearningAgentDecisionKind::refused, ErrorStatus(),
                  "SB_AGENT_RUNTIME_LEARNING_POLICY_INVALID",
                  "agents.runtime_learning.policy_invalid",
                  "policy and query shape digest are required", true);
  }
  if (!snapshot.feedback_authoritative || snapshot.parser_authority ||
      snapshot.benchmark_authority || !snapshot.exact_result_fallback_present ||
      !snapshot.mga_recheck_preserved || !snapshot.security_recheck_preserved) {
    return Finish(RuntimeLearningAgentDecisionKind::refused, ErrorStatus(),
                  "SB_AGENT_RUNTIME_LEARNING_AUTHORITY_UNTRUSTED",
                  "agents.runtime_learning.untrusted_authority",
                  "learning evidence must be runtime-authoritative and exact-fallback safe",
                  true);
  }
  if (snapshot.runtime_samples < policy.min_runtime_samples) {
    return Finish(RuntimeLearningAgentDecisionKind::no_action, OkStatus(),
                  "SB_AGENT_RUNTIME_LEARNING_INSUFFICIENT_SAMPLES",
                  "agents.runtime_learning.insufficient_samples",
                  "sample count below policy threshold", false);
  }
  if (snapshot.estimate_error_ratio_per_mille >=
          policy.estimate_error_threshold_per_mille &&
      policy.planner_correction_allowed) {
    auto result = Finish(
        RuntimeLearningAgentDecisionKind::recommend_planner_correction,
        OkStatus(),
        "SB_AGENT_RUNTIME_LEARNING_PLANNER_CORRECTION",
        "agents.runtime_learning.planner_correction",
        "observed estimate error exceeds policy threshold",
        false);
    AddEvidence(&result, "estimate_error_ratio_per_mille",
                std::to_string(snapshot.estimate_error_ratio_per_mille));
    return result;
  }
  return Finish(RuntimeLearningAgentDecisionKind::no_action, OkStatus(),
                "SB_AGENT_RUNTIME_LEARNING_NO_ACTION",
                "agents.runtime_learning.no_action",
                "runtime estimate error within policy", false);
}

const char* runtime_learning_agent_implementation_anchor() {
  return "runtime_learning_agent";
}

}  // namespace scratchbird::core::agents::implemented_agents

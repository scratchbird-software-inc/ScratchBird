// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "agents/policy_recommendation_manager.hpp"

// CanonicalAgentRegistry/CanonicalAgentManifest owns production exposure;
// this file provides the local policy-recommendation handler.

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

void AddEvidence(PolicyRecommendationManagerResult* result,
                 std::string key,
                 std::string value) {
  result->evidence.push_back({std::move(key), std::move(value)});
}

PolicyRecommendationManagerResult Finish(
    PolicyRecommendationManagerDecisionKind decision,
    Status status,
    std::string code,
    std::string key,
    std::string detail,
    bool fail_closed) {
  PolicyRecommendationManagerResult result;
  result.status = status;
  result.decision = decision;
  result.fail_closed = fail_closed;
  result.diagnostic = MakePolicyRecommendationManagerDiagnostic(
      result.status, std::move(code), std::move(key), std::move(detail));
  AddEvidence(&result, "decision",
              PolicyRecommendationManagerDecisionKindName(result.decision));
  AddEvidence(&result, "advisory_only", "true");
  return result;
}

}  // namespace

const char* PolicyRecommendationManagerDecisionKindName(
    PolicyRecommendationManagerDecisionKind decision) {
  switch (decision) {
    case PolicyRecommendationManagerDecisionKind::no_action: return "no_action";
    case PolicyRecommendationManagerDecisionKind::create_policy_recommendation:
      return "create_policy_recommendation";
    case PolicyRecommendationManagerDecisionKind::refused: return "refused";
  }
  return "refused";
}

DiagnosticRecord MakePolicyRecommendationManagerDiagnostic(
    Status status,
    std::string diagnostic_code,
    std::string message_key,
    std::string detail) {
  return scratchbird::core::platform::MakeDiagnostic(
      status.code, status.severity, status.subsystem,
      std::move(diagnostic_code), std::move(message_key),
      {{"detail", std::move(detail)}}, {}, "policy_recommendation_manager", {});
}

PolicyRecommendationManagerResult EvaluatePolicyRecommendationManager(
    const PolicyRecommendationManagerSnapshot& snapshot,
    const PolicyRecommendationManagerPolicy& policy) {
  if (!policy.present || !policy.valid || !policy.scope_compatible ||
      snapshot.policy_family.empty()) {
    return Finish(PolicyRecommendationManagerDecisionKind::refused,
                  ErrorStatus(),
                  "SB_AGENT_POLICY_RECOMMENDATION_POLICY_INVALID",
                  "agents.policy_recommendation.policy_invalid",
                  "policy and target family are required", true);
  }
  if (!snapshot.policy_metrics_authoritative ||
      !snapshot.recommendation_target_valid ||
      !snapshot.redaction_policy_valid || snapshot.parser_authority ||
      snapshot.client_authority) {
    return Finish(PolicyRecommendationManagerDecisionKind::refused,
                  ErrorStatus(),
                  "SB_AGENT_POLICY_RECOMMENDATION_AUTHORITY_UNTRUSTED",
                  "agents.policy_recommendation.untrusted_authority",
                  "recommendations require trusted metrics target and redaction proof",
                  true);
  }
  if (snapshot.policy_evaluations_total >= policy.min_policy_evaluations &&
      snapshot.workload_slo_burn_rate_per_mille >=
          policy.slo_burn_rate_threshold_per_mille &&
      policy.recommendations_allowed) {
    auto result = Finish(
        PolicyRecommendationManagerDecisionKind::create_policy_recommendation,
        OkStatus(),
        "SB_AGENT_POLICY_RECOMMENDATION_CREATED",
        "agents.policy_recommendation.created",
        "policy evaluation and SLO burn evidence justify recommendation",
        false);
    AddEvidence(&result, "policy_family", snapshot.policy_family);
    AddEvidence(&result, "workload_slo_burn_rate_per_mille",
                std::to_string(snapshot.workload_slo_burn_rate_per_mille));
    return result;
  }
  return Finish(PolicyRecommendationManagerDecisionKind::no_action, OkStatus(),
                "SB_AGENT_POLICY_RECOMMENDATION_NO_ACTION",
                "agents.policy_recommendation.no_action",
                "policy metrics within recommendation threshold", false);
}

const char* policy_recommendation_manager_implementation_anchor() {
  return "policy_recommendation_manager";
}

}  // namespace scratchbird::core::agents::implemented_agents

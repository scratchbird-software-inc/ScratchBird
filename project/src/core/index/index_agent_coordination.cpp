// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "index_agent_coordination.hpp"

#include <utility>

namespace scratchbird::core::index {
namespace {
using scratchbird::core::platform::DiagnosticArgument;
using scratchbird::core::platform::MakeDiagnostic;
using scratchbird::core::platform::Severity;
using scratchbird::core::platform::StatusCode;
using scratchbird::core::platform::Subsystem;

Status OkStatus() { return {StatusCode::ok, Severity::info, Subsystem::engine}; }
Status RefuseStatus() { return {StatusCode::platform_required_feature_missing, Severity::warning, Subsystem::engine}; }
}  // namespace

IndexAgentRecommendation RecommendIndexHealthAction(const IndexAgentObservation& observation) {
  IndexAgentRecommendation recommendation;
  if (!observation.index_uuid.valid() || observation.family == IndexFamily::unknown) {
    recommendation.status = RefuseStatus();
    recommendation.diagnostic = MakeIndexAgentDiagnostic(recommendation.status,
                                                         "SB-INDEX-AGENT-OBSERVATION-INVALID",
                                                         "index.agent.observation_invalid");
    return recommendation;
  }
  recommendation.status = OkStatus();
  recommendation.admitted = true;
  recommendation.owner_agent = "index_health_manager";
  if (!observation.policy_allows_action) {
    recommendation.kind = IndexAgentRecommendationKind::observe;
    recommendation.explanation = "policy_allows_observe_only";
  } else if (observation.stale_resource_count > 0) {
    recommendation.kind = IndexAgentRecommendationKind::rebuild;
    recommendation.explanation = "stale_resource_requires_rebuild";
  } else if (observation.fragmentation_ratio > 0.35 || observation.read_amplification_ratio > 3.0) {
    recommendation.kind = IndexAgentRecommendationKind::rebalance;
    recommendation.explanation = "fragmentation_or_read_amplification_exceeded";
  } else if (observation.memory_pressure_score < 50 && observation.resident_bytes == 0) {
    recommendation.kind = IndexAgentRecommendationKind::warm;
    recommendation.explanation = "low_pressure_hot_index_warmup_candidate";
  } else {
    recommendation.kind = IndexAgentRecommendationKind::verify;
    recommendation.explanation = "routine_health_verification";
  }
  return recommendation;
}

IndexResidencyDecision RecommendIndexResidencyAction(const IndexAgentObservation& observation,
                                                     const IndexResidencyRequest& request) {
  IndexResidencyRequest adjusted = request;
  adjusted.current_pressure_score = static_cast<u64>(observation.memory_pressure_score);
  adjusted.policy_allows_pin = adjusted.policy_allows_pin && observation.policy_allows_action;
  return PlanIndexResidency(adjusted);
}

DiagnosticRecord MakeIndexAgentDiagnostic(Status status,
                                          std::string diagnostic_code,
                                          std::string message_key,
                                          std::string detail) {
  std::vector<DiagnosticArgument> arguments;
  if (!detail.empty()) {
    arguments.push_back({"detail", std::move(detail)});
  }
  return MakeDiagnostic(status.code, status.severity, status.subsystem,
                        std::move(diagnostic_code), std::move(message_key),
                        std::move(arguments), {}, "core.index.agent_coordination");
}

}  // namespace scratchbird::core::index

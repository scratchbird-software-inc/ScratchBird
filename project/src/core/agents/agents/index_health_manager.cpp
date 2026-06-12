// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "agents/index_health_manager.hpp"

// CanonicalAgentRegistry/CanonicalAgentManifest owns production exposure;
// this file provides the local index-health advisory handler.

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

void AddEvidence(IndexHealthManagerResult* result,
                 std::string key,
                 std::string value) {
  result->evidence.push_back({std::move(key), std::move(value)});
}

IndexHealthManagerResult Finish(IndexHealthManagerDecisionKind decision,
                                Status status,
                                std::string code,
                                std::string key,
                                std::string detail,
                                bool fail_closed) {
  IndexHealthManagerResult result;
  result.status = status;
  result.decision = decision;
  result.fail_closed = fail_closed;
  result.diagnostic = MakeIndexHealthManagerDiagnostic(result.status,
                                                       std::move(code),
                                                       std::move(key),
                                                       std::move(detail));
  AddEvidence(&result, "decision",
              IndexHealthManagerDecisionKindName(result.decision));
  AddEvidence(&result, "advisory_only", "true");
  return result;
}

}  // namespace

const char* IndexHealthManagerDecisionKindName(
    IndexHealthManagerDecisionKind decision) {
  switch (decision) {
    case IndexHealthManagerDecisionKind::no_action: return "no_action";
    case IndexHealthManagerDecisionKind::recommend_index_rebuild:
      return "recommend_index_rebuild";
    case IndexHealthManagerDecisionKind::recommend_index_drop:
      return "recommend_index_drop";
    case IndexHealthManagerDecisionKind::request_fast_filespace_for_index_rebuild:
      return "request_fast_filespace_for_index_rebuild";
    case IndexHealthManagerDecisionKind::refused: return "refused";
  }
  return "refused";
}

DiagnosticRecord MakeIndexHealthManagerDiagnostic(Status status,
                                                  std::string diagnostic_code,
                                                  std::string message_key,
                                                  std::string detail) {
  return scratchbird::core::platform::MakeDiagnostic(
      status.code, status.severity, status.subsystem,
      std::move(diagnostic_code), std::move(message_key),
      {{"detail", std::move(detail)}}, {}, "index_health_manager", {});
}

IndexHealthManagerResult EvaluateIndexHealthManager(
    const IndexHealthManagerSnapshot& snapshot,
    const IndexHealthManagerPolicy& policy) {
  if (!policy.present || !policy.valid || !policy.scope_compatible ||
      snapshot.index_uuid.empty()) {
    return Finish(IndexHealthManagerDecisionKind::refused, ErrorStatus(),
                  "SB_AGENT_INDEX_HEALTH_POLICY_INVALID",
                  "agents.index_health.policy_invalid",
                  "policy and index identity are required", true);
  }
  if (!snapshot.index_metrics_authoritative || snapshot.parser_authority ||
      snapshot.reference_authority || !snapshot.index_visible) {
    return Finish(IndexHealthManagerDecisionKind::refused, ErrorStatus(),
                  "SB_AGENT_INDEX_HEALTH_AUTHORITY_UNTRUSTED",
                  "agents.index_health.untrusted_authority",
                  "trusted index metrics and visible index identity are required",
                  true);
  }
  if (snapshot.read_amplification_ratio >= policy.read_amplification_threshold &&
      policy.rebuild_recommendation_allowed) {
    auto result = Finish(IndexHealthManagerDecisionKind::recommend_index_rebuild,
                         OkStatus(),
                         "SB_AGENT_INDEX_HEALTH_REBUILD_RECOMMENDED",
                         "agents.index_health.rebuild_recommended",
                         "read amplification exceeds policy threshold", false);
    AddEvidence(&result, "read_amplification_ratio",
                std::to_string(snapshot.read_amplification_ratio));
    return result;
  }
  if (!snapshot.index_unique_or_constraint_backed &&
      snapshot.unused_for_microseconds >= policy.unused_window_microseconds &&
      policy.drop_recommendation_allowed) {
    return Finish(IndexHealthManagerDecisionKind::recommend_index_drop,
                  OkStatus(),
                  "SB_AGENT_INDEX_HEALTH_DROP_RECOMMENDED",
                  "agents.index_health.drop_recommended",
                  "unused non-constraint index exceeded policy window", false);
  }
  if (snapshot.filespace_metrics_authoritative &&
      snapshot.filespace_fsync_p99_microseconds >=
          policy.fsync_p99_fast_filespace_threshold_microseconds &&
      policy.filespace_request_allowed) {
    return Finish(IndexHealthManagerDecisionKind::request_fast_filespace_for_index_rebuild,
                  OkStatus(),
                  "SB_AGENT_INDEX_HEALTH_FAST_FILESPACE_REQUESTED",
                  "agents.index_health.fast_filespace_requested",
                  "filespace latency justifies fast rebuild placement", false);
  }
  return Finish(IndexHealthManagerDecisionKind::no_action, OkStatus(),
                "SB_AGENT_INDEX_HEALTH_NO_ACTION",
                "agents.index_health.no_action",
                "index health within policy", false);
}

const char* index_health_manager_implementation_anchor() {
  return "index_health_manager";
}

}  // namespace scratchbird::core::agents::implemented_agents

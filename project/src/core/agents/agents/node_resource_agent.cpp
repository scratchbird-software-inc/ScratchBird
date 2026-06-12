// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "agents/node_resource_agent.hpp"

// CanonicalAgentRegistry/CanonicalAgentManifest owns production exposure;
// this file provides the local node resource handler.

#include <algorithm>
#include <utility>

namespace scratchbird::core::agents::implemented_agents {
namespace {

using scratchbird::core::platform::Severity;
using scratchbird::core::platform::StatusCode;
using scratchbird::core::platform::Subsystem;

Status OkStatus() {
  return {StatusCode::ok, Severity::info, Subsystem::engine};
}

Status ErrorStatus() {
  return {StatusCode::memory_invalid_request, Severity::error, Subsystem::engine};
}

void AddEvidence(NodeResourceAgentResult* result,
                 std::string key,
                 std::string value) {
  result->evidence.push_back({std::move(key), std::move(value)});
}

NodeResourceAgentResult Refuse(std::string code,
                               std::string key,
                               std::string detail) {
  NodeResourceAgentResult result;
  result.status = ErrorStatus();
  result.decision = NodeResourceAgentDecisionKind::refused;
  result.fail_closed = true;
  result.diagnostic = MakeNodeResourceAgentDiagnostic(result.status,
                                                      std::move(code),
                                                      std::move(key),
                                                      std::move(detail));
  AddEvidence(&result, "decision",
              NodeResourceAgentDecisionKindName(result.decision));
  AddEvidence(&result, "failed_closed", "true");
  return result;
}

}  // namespace

const char* NodeResourceAgentDecisionKindName(
    NodeResourceAgentDecisionKind decision) {
  switch (decision) {
    case NodeResourceAgentDecisionKind::publish_capability:
      return "publish_capability";
    case NodeResourceAgentDecisionKind::publish_role_suitability:
      return "publish_role_suitability";
    case NodeResourceAgentDecisionKind::refused:
      return "refused";
  }
  return "refused";
}

DiagnosticRecord MakeNodeResourceAgentDiagnostic(Status status,
                                                 std::string diagnostic_code,
                                                 std::string message_key,
                                                 std::string detail) {
  return scratchbird::core::platform::MakeDiagnostic(
      status.code,
      status.severity,
      status.subsystem,
      std::move(diagnostic_code),
      std::move(message_key),
      {{"detail", std::move(detail)}},
      {},
      "node_resource_agent",
      {});
}

NodeResourceAgentResult EvaluateNodeResourceAgentSnapshot(
    const NodeResourceAgentSnapshot& snapshot,
    const NodeResourceAgentPolicy& policy) {
  if (!policy.present || !policy.valid || !policy.scope_compatible) {
    return Refuse("SB_AGENT_NODE_RESOURCE_POLICY_INVALID",
                  "agents.node_resource.policy_invalid",
                  "policy missing invalid or outside scope");
  }
  if (snapshot.cluster_metric_route_requested) {
    return Refuse("SB_AGENT_CLUSTER_PROVIDER_REQUIRED",
                  "agents.node_resource.cluster_metric_external_provider_required",
                  "cluster node metrics must be supplied by external cluster provider");
  }
  if (!snapshot.os_probe_authoritative ||
      !snapshot.metric_registry_authoritative ||
      snapshot.parser_authority || snapshot.client_authority ||
      snapshot.reference_authority) {
    return Refuse("SB_AGENT_NODE_RESOURCE_AUTHORITY_UNTRUSTED",
                  "agents.node_resource.untrusted_authority",
                  "node resource facts must come from trusted platform metrics");
  }
  if (snapshot.cpu_count == 0 || snapshot.total_memory_bytes == 0 ||
      snapshot.page_size_bytes == 0 || !snapshot.cpu_feature_probe_present ||
      !snapshot.page_size_supported || !snapshot.resource_governance_enabled) {
    return Refuse("SB_AGENT_NODE_RESOURCE_CAPABILITY_INCOMPLETE",
                  "agents.node_resource.capability_incomplete",
                  "required node capability facts are absent or unsupported");
  }

  NodeResourceAgentResult result;
  result.status = OkStatus();
  result.fail_closed = false;
  result.role_suitability_score = 100;
  result.role_suitability_score -=
      std::min<u64>(snapshot.memory_pressure_percent, 100);
  if (snapshot.scheduler_queue_depth >
      policy.max_scheduler_queue_depth_for_primary) {
    result.role_suitability_score /= 2;
  }
  if (snapshot.memory_pressure_percent >= policy.high_memory_pressure_percent) {
    result.role_suitability_score =
        std::min<u64>(result.role_suitability_score, 25);
  }
  result.decision =
      policy.publish_role_suitability_allowed
          ? NodeResourceAgentDecisionKind::publish_role_suitability
          : NodeResourceAgentDecisionKind::publish_capability;
  result.publish_node_capability = policy.publish_node_capability_allowed;
  result.publish_role_suitability = policy.publish_role_suitability_allowed;
  result.diagnostic = MakeNodeResourceAgentDiagnostic(
      result.status,
      "SB_AGENT_NODE_RESOURCE_PUBLISH_READY",
      "agents.node_resource.publish_ready",
      "trusted node capability and role suitability facts available");
  AddEvidence(&result, "decision",
              NodeResourceAgentDecisionKindName(result.decision));
  AddEvidence(&result, "cpu_count", std::to_string(snapshot.cpu_count));
  AddEvidence(&result, "page_size_bytes",
              std::to_string(snapshot.page_size_bytes));
  AddEvidence(&result, "role_suitability_score",
              std::to_string(result.role_suitability_score));
  AddEvidence(&result, "cluster_metric_implementation", "external_provider_only");
  AddEvidence(&result, "transaction_finality_authority", "false");
  AddEvidence(&result, "visibility_authority", "false");
  AddEvidence(&result, "recovery_authority", "false");
  return result;
}

const char* node_resource_agent_implementation_anchor() {
  return "node_resource_agent";
}

}  // namespace scratchbird::core::agents::implemented_agents

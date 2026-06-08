// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "agent_cluster_projection_guard.hpp"

#include <utility>

namespace scratchbird::core::agents {
namespace {

std::string BoolText(bool value) {
  return value ? "true" : "false";
}

void Add(AgentClusterProjectionGuardResult* result, std::string evidence) {
  if (result != nullptr) { result->evidence.push_back(std::move(evidence)); }
}

bool AuthorityRequested(const AgentClusterProjectionGuardRequest& request) {
  return request.wants_cluster_authority ||
         request.wants_agent_action_authority;
}

bool ExternalProviderProofValid(
    const AgentClusterProjectionGuardRequest& request) {
  return request.external_provider_authority_proof_present &&
         request.external_provider_authority_digest_verified &&
         !request.external_provider_authority_digest.empty();
}

AgentClusterProjectionGuardResult Finish(
    const AgentClusterProjectionGuardRequest& request,
    bool ok,
    bool fail_closed,
    bool evidence_only,
    bool cluster_authority_granted,
    bool agent_action_authority_granted,
    std::string diagnostic_code,
    std::string detail) {
  AgentClusterProjectionGuardResult result;
  result.ok = ok;
  result.fail_closed = fail_closed;
  result.evidence_only = evidence_only;
  result.local_projection_authority = false;
  result.cluster_authority_granted = cluster_authority_granted;
  result.agent_action_authority_granted = agent_action_authority_granted;
  result.external_provider_authority_proof_used =
      cluster_authority_granted || agent_action_authority_granted;
  result.source = AgentClusterProjectionGuardSourceName(request.source);
  result.diagnostic_code = std::move(diagnostic_code);
  result.detail = std::move(detail);
  Add(&result, "CLUSTER_AGENT_PROJECTION_GUARD");
  Add(&result, "agent.cluster_projection_guard.source=" + result.source);
  Add(&result, "agent.cluster_projection_guard.artifact_id=" +
                   request.artifact_id);
  Add(&result, "agent.cluster_projection_guard.projection_digest_present=" +
                   BoolText(!request.projection_digest.empty()));
  Add(&result, "agent.cluster_projection_guard.local_projection_present=" +
                   BoolText(request.local_projection_present));
  Add(&result, "agent.cluster_projection_guard.evidence_only=" +
                   BoolText(result.evidence_only));
  Add(&result, "agent.cluster_projection_guard.fail_closed=" +
                   BoolText(result.fail_closed));
  Add(&result, "agent.cluster_projection_guard.local_projection_authority=false");
  Add(&result, "agent.cluster_projection_guard.cluster_authority_granted=" +
                   BoolText(result.cluster_authority_granted));
  Add(&result,
      "agent.cluster_projection_guard.agent_action_authority_granted=" +
          BoolText(result.agent_action_authority_granted));
  Add(&result, "agent.cluster_projection_guard.external_provider_proof_present=" +
                   BoolText(request.external_provider_authority_proof_present));
  Add(&result, "agent.cluster_projection_guard.external_provider_digest_verified=" +
                   BoolText(request.external_provider_authority_digest_verified));
  Add(&result, "agent.cluster_projection_guard.diagnostic_code=" +
                   result.diagnostic_code);
  return result;
}

}  // namespace

const char* AgentClusterProjectionGuardSourceName(
    AgentClusterProjectionGuardSource source) {
  switch (source) {
    case AgentClusterProjectionGuardSource::projection_cache:
      return "projection_cache";
    case AgentClusterProjectionGuardSource::route_cache:
      return "route_cache";
    case AgentClusterProjectionGuardSource::metric:
      return "metric";
    case AgentClusterProjectionGuardSource::optimizer_recommendation:
      return "optimizer_recommendation";
  }
  return "unknown";
}

// SEARCH_KEY: CLUSTER_AGENT_PROJECTION_GUARD
AgentClusterProjectionGuardResult EvaluateAgentClusterProjectionGuard(
    const AgentClusterProjectionGuardRequest& request) {
  if (request.artifact_id.empty() || request.projection_digest.empty()) {
    return Finish(
        request,
        false,
        true,
        true,
        false,
        false,
        "SB_AGENT_CLUSTER_PROJECTION_GUARD.IDENTITY_REQUIRED",
        "artifact_id_and_projection_digest_required");
  }
  if (!request.local_projection_present) {
    return Finish(
        request,
        false,
        true,
        true,
        false,
        false,
        "SB_AGENT_CLUSTER_PROJECTION_GUARD.PROJECTION_REQUIRED",
        "local_projection_evidence_required");
  }
  if (!AuthorityRequested(request)) {
    return Finish(
        request,
        true,
        false,
        true,
        false,
        false,
        "SB_AGENT_CLUSTER_PROJECTION_GUARD.EVIDENCE_ONLY",
        "local_cluster_projection_remains_agent_evidence_only");
  }
  if (!ExternalProviderProofValid(request)) {
    return Finish(
        request,
        false,
        true,
        true,
        false,
        false,
        "SB_AGENT_CLUSTER_PROJECTION_GUARD.EXTERNAL_PROVIDER_REQUIRED",
        "cluster_projection_authority_requires_external_provider_proof");
  }
  return Finish(
      request,
      true,
      false,
      false,
      request.wants_cluster_authority,
      request.wants_agent_action_authority,
      "SB_AGENT_CLUSTER_PROJECTION_GUARD.PROVIDER_DELEGATED",
      "cluster_authority_delegated_to_external_provider_proof");
}

}  // namespace scratchbird::core::agents

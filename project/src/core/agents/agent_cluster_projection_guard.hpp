// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include <string>
#include <vector>

namespace scratchbird::core::agents {

// SEARCH_KEY: CLUSTER_AGENT_PROJECTION_GUARD
// Agent-visible cluster projections, metrics, route caches, and optimizer
// recommendations are evidence-only unless an external provider authority
// proof is supplied and verified.
enum class AgentClusterProjectionGuardSource {
  projection_cache,
  route_cache,
  metric,
  optimizer_recommendation,
};

struct AgentClusterProjectionGuardRequest {
  AgentClusterProjectionGuardSource source =
      AgentClusterProjectionGuardSource::projection_cache;
  std::string artifact_id;
  std::string projection_digest;
  std::string external_provider_authority_digest;
  bool local_projection_present = false;
  bool wants_cluster_authority = false;
  bool wants_agent_action_authority = false;
  bool external_provider_authority_proof_present = false;
  bool external_provider_authority_digest_verified = false;
};

struct AgentClusterProjectionGuardResult {
  bool ok = false;
  bool fail_closed = true;
  bool evidence_only = true;
  bool local_projection_authority = false;
  bool cluster_authority_granted = false;
  bool agent_action_authority_granted = false;
  bool external_provider_authority_proof_used = false;
  std::string source;
  std::string diagnostic_code;
  std::string detail;
  std::vector<std::string> evidence;
};

const char* AgentClusterProjectionGuardSourceName(
    AgentClusterProjectionGuardSource source);

AgentClusterProjectionGuardResult EvaluateAgentClusterProjectionGuard(
    const AgentClusterProjectionGuardRequest& request);

}  // namespace scratchbird::core::agents

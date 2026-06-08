// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// SEARCH_KEY: SB_AGENT_CLUSTER_PROVIDER_BOUNDARY
// Agent-facing cluster provider and leadership lease boundary. This module owns
// deterministic precondition checks only; real cluster behavior remains behind
// the compile-time sb_cluster_provider target.

#include "agent_runtime.hpp"

#include <string>
#include <vector>

namespace scratchbird::core::agents {

inline constexpr const char* kAgentClusterSupportNotEnabledCode =
    "SBLR.CLUSTER.SUPPORT_NOT_ENABLED";
inline constexpr const char* kAgentClusterLeaseRequiredCode =
    "CLUSTER.LEASE_REQUIRED";
inline constexpr const char* kAgentClusterLeaderExistsCode =
    "CLUSTER.LEADER_EXISTS";
inline constexpr const char* kAgentClusterFenceStaleCode =
    "CLUSTER.FENCE_STALE";
inline constexpr const char* kAgentClusterSplitBrainRefusedCode =
    "CLUSTER.SPLIT_BRAIN_REFUSED";
inline constexpr const char* kAgentClusterAuthorityUnavailableCode =
    "CLUSTER.AUTHORITY_UNAVAILABLE";
inline constexpr const char* kAgentClusterExternalProviderRequiredCode =
    "CLUSTER.EXTERNAL_PROVIDER_REQUIRED";

enum class AgentClusterLeadershipState {
  follower,
  candidate,
  leader_pending_fence,
  leader_active,
  leader_draining,
  lease_expired,
  quarantined
};

enum class AgentClusterLeaseSurface {
  inspect,
  acquire_lease,
  renew_lease,
  failover,
  authorize_action
};

struct AgentClusterEvidence {
  std::string evidence_kind;
  std::string evidence_id;
};

struct AgentClusterBoundaryResult {
  bool ok = false;
  bool provider_called = false;
  bool cluster_path_failed_closed = false;
  bool cluster_authority_required = false;
  bool external_provider_required = false;
  bool preconditions_satisfied = false;
  bool action_authorized = false;
  std::string operation_id;
  std::string diagnostic_code;
  std::string detail;
  std::string provider_name;
  std::string provider_type;
  std::string provider_support_status;
  std::vector<std::string> diagnostic_codes;
  std::vector<AgentClusterEvidence> evidence;
};

struct AgentClusterLeaseState {
  AgentClusterLeadershipState state = AgentClusterLeadershipState::follower;
  std::string owner_instance_uuid;
  std::string fence_token;
  u64 epoch = 0;
  u64 lease_until_microseconds = 0;
};

struct AgentClusterLeaseRequest {
  AgentClusterLeaseSurface surface = AgentClusterLeaseSurface::inspect;
  std::string agent_type_id;
  std::string instance_uuid;
  std::string fence_token;
  u64 epoch = 0;
  u64 now_microseconds = 0;
  u64 lease_duration_microseconds = 0;
  bool destructive_or_control_action = false;
  bool production_live_path = false;
};

const char* AgentClusterLeadershipStateName(AgentClusterLeadershipState state);
const char* AgentClusterLeaseSurfaceName(AgentClusterLeaseSurface surface);

AgentClusterBoundaryResult RouteAgentClusterProviderBoundary(
    const AgentRuntimeContext& context,
    std::string operation_id,
    bool production_live_path = false);

AgentClusterBoundaryResult ApplyAgentClusterLeaseSurface(
    const AgentRuntimeContext& context,
    const AgentClusterLeaseRequest& request,
    AgentClusterLeaseState* state);

}  // namespace scratchbird::core::agents

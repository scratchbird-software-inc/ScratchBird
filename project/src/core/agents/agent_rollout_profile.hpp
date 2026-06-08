// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "agent_runtime.hpp"

// SEARCH_KEY: SB_AGENT_RUNTIME_AGENT_ROLLOUT_PROFILE_HEADER
#include <map>
#include <string>
#include <vector>

namespace scratchbird::core::agents {

// SEARCH_KEY: CEIC_075_AGENT_ROLLOUT_PROFILE
enum class AgentActionRolloutMode {
  disabled,
  shadow,
  observe,
  dry_run,
  canary,
  phased,
  live
};

enum class AgentActionRolloutState {
  disabled,
  pending,
  active,
  paused,
  completed,
  failed,
  quarantined
};

struct AgentActionRolloutProfile {
  std::string profile_uuid;
  std::string evidence_uuid;
  AgentActionRolloutMode mode = AgentActionRolloutMode::disabled;
  AgentActionRolloutState state = AgentActionRolloutState::disabled;
  u64 canary_percent = 0;
  u64 canary_max_subjects = 0;
  u64 canary_current_subjects = 0;
  u64 phased_percent = 0;
  u64 phased_target_percent = 0;
  u64 failure_threshold = 0;
  u64 observed_failures = 0;
  u64 retry_limit = 0;
  u64 retry_count = 0;
  bool quarantine_on_failure = true;
  bool external_cluster_provider_attested = false;
  std::vector<std::string> authority_claims;
};

const char* AgentActionRolloutModeName(AgentActionRolloutMode mode);
const char* AgentActionRolloutStateName(AgentActionRolloutState state);

AgentActionRolloutMode ParseAgentActionRolloutMode(
    const std::string& value,
    AgentActionRolloutMode fallback = AgentActionRolloutMode::disabled);
AgentActionRolloutState ParseAgentActionRolloutState(
    const std::string& value,
    AgentActionRolloutState fallback = AgentActionRolloutState::disabled);

AgentActionRolloutProfile AgentActionRolloutProfileFromInputs(
    const std::map<std::string, std::string>& inputs);

bool AgentActionRolloutModeRequiresDryRun(AgentActionRolloutMode mode);
bool AgentActionRolloutModeAllowsMutation(AgentActionRolloutMode mode);
AgentRuntimeStatus ValidateAgentActionRolloutProfile(
    const AgentActionRolloutProfile& profile,
    bool requested_live_mutation,
    bool cluster_route_requested);

}  // namespace scratchbird::core::agents

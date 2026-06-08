// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "agent_rollout_profile.hpp"

#include <algorithm>
#include <cctype>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace scratchbird::core::agents {
namespace {

std::string Lower(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return value;
}

std::string InputOr(const std::map<std::string, std::string>& inputs,
                    const std::string& key,
                    std::string fallback = {}) {
  const auto it = inputs.find(key);
  return it == inputs.end() ? std::move(fallback) : it->second;
}

u64 ParseU64(const std::map<std::string, std::string>& inputs,
             const std::string& key,
             u64 fallback = 0) {
  const auto value = InputOr(inputs, key);
  if (value.empty()) { return fallback; }
  try {
    return static_cast<u64>(std::stoull(value));
  } catch (const std::exception&) {
    return fallback;
  }
}

bool ParseBool(const std::map<std::string, std::string>& inputs,
               const std::string& key,
               bool fallback = false) {
  const auto value = Lower(InputOr(inputs, key));
  if (value.empty()) { return fallback; }
  return value == "1" || value == "true" || value == "yes";
}

std::vector<std::string> SplitCsv(const std::string& value) {
  std::vector<std::string> result;
  std::string item;
  std::istringstream in(value);
  while (std::getline(in, item, ',')) {
    if (!item.empty()) { result.push_back(item); }
  }
  return result;
}

}  // namespace

const char* AgentActionRolloutModeName(AgentActionRolloutMode mode) {
  switch (mode) {
    case AgentActionRolloutMode::disabled: return "disabled";
    case AgentActionRolloutMode::shadow: return "shadow";
    case AgentActionRolloutMode::observe: return "observe";
    case AgentActionRolloutMode::dry_run: return "dry_run";
    case AgentActionRolloutMode::canary: return "canary";
    case AgentActionRolloutMode::phased: return "phased";
    case AgentActionRolloutMode::live: return "live";
  }
  return "disabled";
}

const char* AgentActionRolloutStateName(AgentActionRolloutState state) {
  switch (state) {
    case AgentActionRolloutState::disabled: return "disabled";
    case AgentActionRolloutState::pending: return "pending";
    case AgentActionRolloutState::active: return "active";
    case AgentActionRolloutState::paused: return "paused";
    case AgentActionRolloutState::completed: return "completed";
    case AgentActionRolloutState::failed: return "failed";
    case AgentActionRolloutState::quarantined: return "quarantined";
  }
  return "disabled";
}

AgentActionRolloutMode ParseAgentActionRolloutMode(
    const std::string& value,
    AgentActionRolloutMode fallback) {
  const auto mode = Lower(value);
  if (mode == "shadow") { return AgentActionRolloutMode::shadow; }
  if (mode == "observe" || mode == "observe_only") {
    return AgentActionRolloutMode::observe;
  }
  if (mode == "dry_run" || mode == "dry-run") {
    return AgentActionRolloutMode::dry_run;
  }
  if (mode == "canary") { return AgentActionRolloutMode::canary; }
  if (mode == "phased") { return AgentActionRolloutMode::phased; }
  if (mode == "live") { return AgentActionRolloutMode::live; }
  if (mode == "disabled") { return AgentActionRolloutMode::disabled; }
  return fallback;
}

AgentActionRolloutState ParseAgentActionRolloutState(
    const std::string& value,
    AgentActionRolloutState fallback) {
  const auto state = Lower(value);
  if (state == "pending") { return AgentActionRolloutState::pending; }
  if (state == "active") { return AgentActionRolloutState::active; }
  if (state == "paused") { return AgentActionRolloutState::paused; }
  if (state == "completed") { return AgentActionRolloutState::completed; }
  if (state == "failed") { return AgentActionRolloutState::failed; }
  if (state == "quarantined") { return AgentActionRolloutState::quarantined; }
  if (state == "disabled") { return AgentActionRolloutState::disabled; }
  return fallback;
}

AgentActionRolloutProfile AgentActionRolloutProfileFromInputs(
    const std::map<std::string, std::string>& inputs) {
  AgentActionRolloutProfile profile;
  profile.profile_uuid = InputOr(inputs, "rollout_profile_uuid");
  profile.evidence_uuid = InputOr(inputs, "rollout_evidence_uuid");
  profile.mode = ParseAgentActionRolloutMode(
      InputOr(inputs, "rollout_mode"), AgentActionRolloutMode::disabled);
  profile.state = ParseAgentActionRolloutState(
      InputOr(inputs, "rollout_state"), AgentActionRolloutState::disabled);
  profile.canary_percent = ParseU64(inputs, "canary_percent");
  profile.canary_max_subjects = ParseU64(inputs, "canary_max_subjects");
  profile.canary_current_subjects = ParseU64(inputs, "canary_current_subjects");
  profile.phased_percent = ParseU64(inputs, "phased_percent");
  profile.phased_target_percent = ParseU64(inputs, "phased_target_percent");
  profile.failure_threshold = ParseU64(inputs, "failure_threshold");
  profile.observed_failures = ParseU64(inputs, "observed_failures");
  profile.retry_limit = ParseU64(inputs, "retry_limit");
  profile.retry_count = ParseU64(inputs, "retry_count");
  profile.quarantine_on_failure =
      ParseBool(inputs, "quarantine_on_failure", true);
  profile.external_cluster_provider_attested =
      ParseBool(inputs, "external_cluster_provider_attested");
  profile.authority_claims = SplitCsv(InputOr(inputs, "authority_claims"));
  return profile;
}

bool AgentActionRolloutModeRequiresDryRun(AgentActionRolloutMode mode) {
  return mode == AgentActionRolloutMode::shadow ||
         mode == AgentActionRolloutMode::observe ||
         mode == AgentActionRolloutMode::dry_run;
}

bool AgentActionRolloutModeAllowsMutation(AgentActionRolloutMode mode) {
  return mode == AgentActionRolloutMode::canary ||
         mode == AgentActionRolloutMode::phased ||
         mode == AgentActionRolloutMode::live;
}

AgentRuntimeStatus ValidateAgentActionRolloutProfile(
    const AgentActionRolloutProfile& profile,
    bool requested_live_mutation,
    bool cluster_route_requested) {
  if (profile.evidence_uuid.empty()) {
    return AgentError("SB_AGENT_ROLLOUT.EVIDENCE_REQUIRED");
  }
  if (cluster_route_requested) {
    return AgentError("SB_AGENT_ROLLOUT.CLUSTER_EXTERNAL_PROVIDER_REQUIRED");
  }
  if (profile.mode == AgentActionRolloutMode::disabled ||
      profile.state == AgentActionRolloutState::disabled) {
    return AgentError("SB_AGENT_ROLLOUT.DISABLED");
  }
  if (profile.state == AgentActionRolloutState::failed ||
      profile.state == AgentActionRolloutState::quarantined) {
    return AgentError("SB_AGENT_ROLLOUT.UNSAFE_STATE",
                      AgentActionRolloutStateName(profile.state));
  }
  if (profile.state != AgentActionRolloutState::active &&
      profile.state != AgentActionRolloutState::pending) {
    return AgentError("SB_AGENT_ROLLOUT.STATE_NOT_ACTIVE",
                      AgentActionRolloutStateName(profile.state));
  }
  if (AgentActionRolloutModeRequiresDryRun(profile.mode) &&
      requested_live_mutation) {
    return AgentError("SB_AGENT_ROLLOUT.DRY_RUN_REQUIRED",
                      AgentActionRolloutModeName(profile.mode));
  }
  if (requested_live_mutation &&
      !AgentActionRolloutModeAllowsMutation(profile.mode)) {
    return AgentError("SB_AGENT_ROLLOUT.LIVE_NOT_ALLOWED",
                      AgentActionRolloutModeName(profile.mode));
  }
  if (profile.mode == AgentActionRolloutMode::canary) {
    if (profile.canary_percent == 0 || profile.canary_percent > 100 ||
        profile.canary_max_subjects == 0 ||
        profile.canary_current_subjects > profile.canary_max_subjects) {
      return AgentError("SB_AGENT_ROLLOUT.CANARY_LIMIT_INVALID");
    }
  }
  if (profile.mode == AgentActionRolloutMode::phased) {
    if (profile.phased_percent == 0 || profile.phased_target_percent == 0 ||
        profile.phased_percent > profile.phased_target_percent ||
        profile.phased_target_percent > 100) {
      return AgentError("SB_AGENT_ROLLOUT.PHASED_LIMIT_INVALID");
    }
  }
  if (profile.failure_threshold == 0) {
    return AgentError("SB_AGENT_ROLLOUT.FAILURE_THRESHOLD_REQUIRED");
  }
  if (requested_live_mutation && !profile.quarantine_on_failure) {
    return AgentError("SB_AGENT_ROLLOUT.QUARANTINE_REQUIRED");
  }
  if (profile.observed_failures >= profile.failure_threshold) {
    return AgentError("SB_AGENT_ROLLOUT.FAILURE_THRESHOLD_EXCEEDED");
  }
  if (profile.retry_count > profile.retry_limit) {
    return AgentError("SB_AGENT_ROLLOUT.RETRY_LIMIT_EXCEEDED");
  }
  return AgentOk();
}

}  // namespace scratchbird::core::agents

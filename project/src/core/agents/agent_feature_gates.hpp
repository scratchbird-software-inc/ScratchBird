// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "agent_runtime.hpp"

#include <optional>
#include <string>
#include <vector>

// SEARCH_KEY: SB_AGENT_RUNTIME_AGENT_FEATURE_GATES_HEADER
// First-class DBLC-013AJ capability gate model. Parser packages may be
// required evidence, but parser components are never lifecycle authority.

namespace scratchbird::core::agents {

using scratchbird::core::platform::u64;

enum class CapabilityEditionScope {
  community,
  private_build,
  enterprise,
  cluster
};

enum class CapabilityLifecycleState {
  installed,
  enabled,
  disabled,
  quarantined,
  retired
};

enum class FeatureGateDecisionClass {
  allow,
  deny,
  fail_closed
};

struct InstalledCapabilityRecord {
  std::string capability_id;
  std::string provider_id;
  CapabilityEditionScope edition_scope = CapabilityEditionScope::community;
  CapabilityLifecycleState lifecycle_state = CapabilityLifecycleState::installed;
  u64 capability_epoch = 1;
  u64 installed_policy_epoch = 0;
  bool requires_parser_package = false;
  std::string required_parser_package_id;
  bool cluster_authority_required = false;
  bool parser_package_installed = false;
};

struct FeatureGateRequest {
  std::string request_id;
  std::string capability_id;
  CapabilityEditionScope requested_edition_scope = CapabilityEditionScope::community;
  u64 observed_policy_epoch = 0;
  u64 minimum_capability_epoch = 1;
  bool cluster_authority_available = false;
  bool parser_package_available = false;
  bool security_context_present = true;
  bool engine_authorization_granted = true;
  bool package_policy_present = true;
  bool trusted_package_signature_present = true;
  bool plugin_or_package_load_requested = false;
  bool parser_listener_driver_claims_authority = false;
  std::string authority_actor = "engine";
};

struct FeatureGateDecision {
  FeatureGateDecisionClass decision_class = FeatureGateDecisionClass::fail_closed;
  AgentRuntimeStatus status;
  std::string capability_id;
  std::string diagnostic_code;
  std::string detail;
  bool parser_package_required = false;
  bool mutates_state = false;
};

const char* CapabilityEditionScopeName(CapabilityEditionScope value);
const char* CapabilityLifecycleStateName(CapabilityLifecycleState value);
const char* FeatureGateDecisionClassName(FeatureGateDecisionClass value);

AgentRuntimeStatus ValidateInstalledCapabilityRecord(const InstalledCapabilityRecord& record);
std::optional<InstalledCapabilityRecord> FindInstalledCapability(
    const std::vector<InstalledCapabilityRecord>& installed,
    const std::string& capability_id);
AgentRuntimeStatus ValidateCapabilityPolicyEpoch(const InstalledCapabilityRecord& record,
                                                 const FeatureGateRequest& request,
                                                 u64 current_policy_epoch);
AgentRuntimeStatus ValidateCapabilityNoDowngrade(const InstalledCapabilityRecord& current,
                                                 const InstalledCapabilityRecord& replacement);
FeatureGateDecision EvaluateFeatureGateRequest(const InstalledCapabilityRecord& record,
                                               const FeatureGateRequest& request,
                                               u64 current_policy_epoch);
FeatureGateDecision EvaluateFeatureGateRequest(
    const std::vector<InstalledCapabilityRecord>& installed,
    const FeatureGateRequest& request,
    u64 current_policy_epoch);
AgentRuntimeStatus ApplyCapabilityLifecycleDecision(const InstalledCapabilityRecord& current,
                                                    const InstalledCapabilityRecord& replacement,
                                                    u64 current_policy_epoch,
                                                    InstalledCapabilityRecord* out_record);

}  // namespace scratchbird::core::agents

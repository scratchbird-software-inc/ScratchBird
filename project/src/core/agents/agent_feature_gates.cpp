// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "agent_feature_gates.hpp"

#include <algorithm>
#include <utility>

namespace scratchbird::core::agents {
namespace {

bool ScopeAllows(CapabilityEditionScope installed, CapabilityEditionScope requested) {
  return static_cast<int>(installed) >= static_cast<int>(requested);
}

FeatureGateDecision Deny(const InstalledCapabilityRecord& record,
                         FeatureGateDecisionClass decision_class,
                         std::string code,
                         std::string detail,
                         bool parser_package_required = false) {
  FeatureGateDecision decision;
  decision.decision_class = decision_class;
  decision.status = AgentError(code, detail);
  decision.capability_id = record.capability_id;
  decision.diagnostic_code = std::move(code);
  decision.detail = std::move(detail);
  decision.parser_package_required = parser_package_required;
  decision.mutates_state = false;
  return decision;
}

}  // namespace

const char* CapabilityEditionScopeName(CapabilityEditionScope value) {
  switch (value) {
    case CapabilityEditionScope::community: return "community";
    case CapabilityEditionScope::private_build: return "private_build";
    case CapabilityEditionScope::enterprise: return "enterprise";
    case CapabilityEditionScope::cluster: return "cluster";
  }
  return "unknown";
}

const char* CapabilityLifecycleStateName(CapabilityLifecycleState value) {
  switch (value) {
    case CapabilityLifecycleState::installed: return "installed";
    case CapabilityLifecycleState::enabled: return "enabled";
    case CapabilityLifecycleState::disabled: return "disabled";
    case CapabilityLifecycleState::quarantined: return "quarantined";
    case CapabilityLifecycleState::retired: return "retired";
  }
  return "unknown";
}

const char* FeatureGateDecisionClassName(FeatureGateDecisionClass value) {
  switch (value) {
    case FeatureGateDecisionClass::allow: return "allow";
    case FeatureGateDecisionClass::deny: return "deny";
    case FeatureGateDecisionClass::fail_closed: return "fail_closed";
  }
  return "unknown";
}

AgentRuntimeStatus ValidateInstalledCapabilityRecord(const InstalledCapabilityRecord& record) {
  if (record.capability_id.empty()) {
    return AgentError("SB_AGENT_CAPABILITY.CAPABILITY_ID_REQUIRED");
  }
  if (record.provider_id.empty()) {
    return AgentError("SB_AGENT_CAPABILITY.PROVIDER_ID_REQUIRED", record.capability_id);
  }
  if (record.capability_epoch == 0) {
    return AgentError("SB_AGENT_CAPABILITY.EPOCH_REQUIRED", record.capability_id);
  }
  if (record.installed_policy_epoch == 0) {
    return AgentError("SB_AGENT_CAPABILITY.POLICY_EPOCH_REQUIRED", record.capability_id);
  }
  if (record.requires_parser_package && record.required_parser_package_id.empty()) {
    return AgentError("SB_AGENT_CAPABILITY.PARSER_PACKAGE_ID_REQUIRED", record.capability_id);
  }
  return AgentOk();
}

std::optional<InstalledCapabilityRecord> FindInstalledCapability(
    const std::vector<InstalledCapabilityRecord>& installed,
    const std::string& capability_id) {
  const auto it = std::find_if(installed.begin(), installed.end(),
                               [&](const InstalledCapabilityRecord& record) {
                                 return record.capability_id == capability_id;
                               });
  if (it == installed.end()) {
    return std::nullopt;
  }
  return *it;
}

AgentRuntimeStatus ValidateCapabilityPolicyEpoch(const InstalledCapabilityRecord& record,
                                                 const FeatureGateRequest& request,
                                                 u64 current_policy_epoch) {
  if (current_policy_epoch == 0 || request.observed_policy_epoch == 0) {
    return AgentError("SB_AGENT_CAPABILITY.POLICY_EPOCH_REQUIRED", record.capability_id);
  }
  if (record.installed_policy_epoch > current_policy_epoch) {
    return AgentError("SB_AGENT_CAPABILITY.POLICY_EPOCH_FROM_FUTURE", record.capability_id);
  }
  if (request.observed_policy_epoch != current_policy_epoch) {
    return AgentError("SB_AGENT_CAPABILITY.POLICY_EPOCH_STALE", record.capability_id);
  }
  if (record.installed_policy_epoch < current_policy_epoch) {
    return AgentError("SB_AGENT_CAPABILITY.POLICY_EPOCH_REVALIDATION_REQUIRED", record.capability_id);
  }
  return AgentOk();
}

AgentRuntimeStatus ValidateCapabilityNoDowngrade(const InstalledCapabilityRecord& current,
                                                 const InstalledCapabilityRecord& replacement) {
  if (current.capability_id != replacement.capability_id) {
    return AgentError("SB_AGENT_CAPABILITY.REPLACEMENT_ID_MISMATCH",
                      current.capability_id + "!=" + replacement.capability_id);
  }
  if (replacement.capability_epoch < current.capability_epoch) {
    return AgentError("SB_AGENT_CAPABILITY.DOWNGRADE_REFUSED", current.capability_id);
  }
  if (!ScopeAllows(replacement.edition_scope, current.edition_scope)) {
    return AgentError("SB_AGENT_CAPABILITY.EDITION_DOWNGRADE_REFUSED", current.capability_id);
  }
  if (current.requires_parser_package && !replacement.requires_parser_package) {
    return AgentError("SB_AGENT_CAPABILITY.PARSER_REQUIREMENT_DOWNGRADE_REFUSED",
                      current.capability_id);
  }
  if (current.cluster_authority_required && !replacement.cluster_authority_required) {
    return AgentError("SB_AGENT_CAPABILITY.CLUSTER_AUTHORITY_DOWNGRADE_REFUSED",
                      current.capability_id);
  }
  return AgentOk();
}

FeatureGateDecision EvaluateFeatureGateRequest(const InstalledCapabilityRecord& record,
                                               const FeatureGateRequest& request,
                                               u64 current_policy_epoch) {
  const auto record_status = ValidateInstalledCapabilityRecord(record);
  if (!record_status.ok) {
    return Deny(record, FeatureGateDecisionClass::fail_closed,
                record_status.diagnostic_code, record_status.detail);
  }
  if (request.capability_id != record.capability_id) {
    return Deny(record, FeatureGateDecisionClass::fail_closed,
                "SB_AGENT_CAPABILITY.REQUEST_ID_MISMATCH", request.capability_id);
  }
  if (!request.security_context_present) {
    return Deny(record, FeatureGateDecisionClass::fail_closed,
                "SB_AGENT_CAPABILITY.SECURITY_CONTEXT_REQUIRED", record.capability_id);
  }
  if (request.authority_actor != "engine" ||
      request.parser_listener_driver_claims_authority) {
    return Deny(record, FeatureGateDecisionClass::fail_closed,
                "SB_AGENT_CAPABILITY.ENGINE_AUTHORITY_REQUIRED",
                request.authority_actor);
  }
  if (!request.engine_authorization_granted) {
    return Deny(record, FeatureGateDecisionClass::deny,
                "SB_AGENT_CAPABILITY.AUTHORIZATION_DENIED", record.capability_id);
  }
  if (request.plugin_or_package_load_requested && !request.package_policy_present) {
    return Deny(record, FeatureGateDecisionClass::fail_closed,
                "SB_AGENT_CAPABILITY.PACKAGE_POLICY_REQUIRED", record.capability_id);
  }
  if (request.plugin_or_package_load_requested &&
      !request.trusted_package_signature_present) {
    return Deny(record, FeatureGateDecisionClass::fail_closed,
                "SB_AGENT_CAPABILITY.PACKAGE_TRUST_REQUIRED", record.capability_id);
  }
  if (record.lifecycle_state == CapabilityLifecycleState::disabled ||
      record.lifecycle_state == CapabilityLifecycleState::retired) {
    return Deny(record, FeatureGateDecisionClass::deny,
                "SB_AGENT_CAPABILITY.NOT_ACTIVE",
                CapabilityLifecycleStateName(record.lifecycle_state));
  }
  if (record.lifecycle_state == CapabilityLifecycleState::quarantined) {
    return Deny(record, FeatureGateDecisionClass::fail_closed,
                "SB_AGENT_CAPABILITY.QUARANTINED", record.capability_id);
  }
  if (!ScopeAllows(record.edition_scope, request.requested_edition_scope)) {
    return Deny(record, FeatureGateDecisionClass::deny,
                "SB_AGENT_CAPABILITY.EDITION_SCOPE_DENIED",
                std::string(CapabilityEditionScopeName(record.edition_scope)) + "<" +
                    CapabilityEditionScopeName(request.requested_edition_scope));
  }
  if (record.capability_epoch < request.minimum_capability_epoch) {
    return Deny(record, FeatureGateDecisionClass::fail_closed,
                "SB_AGENT_CAPABILITY.MINIMUM_EPOCH_UNMET", record.capability_id);
  }
  const auto epoch_status = ValidateCapabilityPolicyEpoch(record, request, current_policy_epoch);
  if (!epoch_status.ok) {
    return Deny(record, FeatureGateDecisionClass::fail_closed,
                epoch_status.diagnostic_code, epoch_status.detail);
  }
  if (record.cluster_authority_required && !request.cluster_authority_available) {
    return Deny(record, FeatureGateDecisionClass::fail_closed,
                "SB_AGENT_CAPABILITY.CLUSTER_AUTHORITY_REQUIRED", record.capability_id);
  }
  const bool parser_available = record.parser_package_installed ||
                                request.parser_package_available;
  if (record.requires_parser_package && !parser_available) {
    return Deny(record, FeatureGateDecisionClass::fail_closed,
                "SB_AGENT_CAPABILITY.PARSER_PACKAGE_REQUIRED",
                record.required_parser_package_id,
                true);
  }

  FeatureGateDecision decision;
  decision.decision_class = FeatureGateDecisionClass::allow;
  decision.status = AgentOk();
  decision.capability_id = record.capability_id;
  decision.diagnostic_code = "SB_AGENT_CAPABILITY.ALLOWED";
  decision.detail = record.provider_id;
  decision.parser_package_required = record.requires_parser_package;
  decision.mutates_state = false;
  return decision;
}

FeatureGateDecision EvaluateFeatureGateRequest(
    const std::vector<InstalledCapabilityRecord>& installed,
    const FeatureGateRequest& request,
    u64 current_policy_epoch) {
  const auto record = FindInstalledCapability(installed, request.capability_id);
  if (!record.has_value()) {
    InstalledCapabilityRecord missing;
    missing.capability_id = request.capability_id;
    return Deny(missing, FeatureGateDecisionClass::fail_closed,
                "SB_AGENT_CAPABILITY.NOT_INSTALLED", request.capability_id);
  }
  return EvaluateFeatureGateRequest(*record, request, current_policy_epoch);
}

AgentRuntimeStatus ApplyCapabilityLifecycleDecision(const InstalledCapabilityRecord& current,
                                                    const InstalledCapabilityRecord& replacement,
                                                    u64 current_policy_epoch,
                                                    InstalledCapabilityRecord* out_record) {
  if (out_record == nullptr) {
    return AgentError("SB_AGENT_CAPABILITY.OUTPUT_RECORD_REQUIRED", current.capability_id);
  }
  const auto current_status = ValidateInstalledCapabilityRecord(current);
  if (!current_status.ok) {
    return current_status;
  }
  const auto replacement_status = ValidateInstalledCapabilityRecord(replacement);
  if (!replacement_status.ok) {
    return replacement_status;
  }
  const auto downgrade_status = ValidateCapabilityNoDowngrade(current, replacement);
  if (!downgrade_status.ok) {
    return downgrade_status;
  }
  if (replacement.installed_policy_epoch != current_policy_epoch) {
    return AgentError("SB_AGENT_CAPABILITY.REPLACEMENT_POLICY_EPOCH_INVALID",
                      replacement.capability_id);
  }
  *out_record = replacement;
  return AgentOk();
}

}  // namespace scratchbird::core::agents

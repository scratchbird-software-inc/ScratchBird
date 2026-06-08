// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "agent_action_safety.hpp"

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

bool HasForbiddenAuthorityClaim(const std::vector<std::string>& claims) {
  static const char* kForbidden[] = {
      "transaction_finality", "transaction_authority", "visibility_authority",
      "security_authority", "authorization_authority", "recovery_authority",
      "parser_authority", "parser_execution_authority", "donor_authority",
      "donor_finality", "wal_authority", "benchmark_authority",
      "cluster_authority", "provider_finality", "action_authority",
      "metric_authority", "memory_authority", "optimizer_plan_authority",
      "index_finality"};
  for (const auto& claim : claims) {
    const auto lower = Lower(claim);
    for (const char* forbidden : kForbidden) {
      if (lower == forbidden) { return true; }
    }
  }
  return false;
}

}  // namespace

AgentActionSafetyEnvelope AgentActionSafetyEnvelopeFromInputs(
    const AgentActionRequest& action,
    const AgentActionSafetyProviderContext& provider,
    const AgentActionRolloutProfile& rollout,
    const std::string& scope_uuid,
    const std::string& input_evidence_digest,
    const std::string& metric_digest,
    const std::string& catalog_root_digest,
    u64 policy_generation,
    bool contract_manual_approval_required,
    bool authority_parser_claim,
    bool authority_client_claim,
    bool authority_donor_claim,
    bool authority_sidecar_claim) {
  AgentActionSafetyEnvelope envelope;
  envelope.action_uuid = action.action_uuid;
  envelope.agent_type_id = action.agent_type_id;
  envelope.operation_id = action.operation_id;
  envelope.actuator_id = action.actuator_id;
  envelope.scope_uuid = scope_uuid;
  envelope.input_evidence_digest = input_evidence_digest;
  envelope.metric_digest = metric_digest;
  envelope.catalog_root_digest = catalog_root_digest;
  envelope.policy_generation = policy_generation;
  envelope.policy_evidence_uuid = InputOr(action.inputs, "policy_evidence_uuid");
  envelope.safety_evidence_uuid = InputOr(action.inputs, "safety_evidence_uuid");
  envelope.safety_envelope_present =
      InputOr(action.inputs, "safety_envelope_version") == "1";
  envelope.requested_dry_run = action.dry_run;
  envelope.manual_approval_required =
      contract_manual_approval_required ||
      ParseBool(action.inputs, "approval_required") ||
      ParseBool(action.inputs, "manual_approval_required");
  envelope.manual_approval_present =
      action.manual_approval_present ||
      ParseBool(action.inputs, "manual_approval_present");
  envelope.approval_evidence_uuid = InputOr(action.inputs, "approval_evidence_uuid");
  envelope.rate_limit_per_window = ParseU64(action.inputs, "rate_limit_per_window");
  envelope.action_count_in_window = ParseU64(action.inputs, "action_count_in_window");
  envelope.rate_limit_key = InputOr(action.inputs, "rate_limit_key");
  envelope.rate_limit_evidence_uuid = InputOr(action.inputs, "rate_limit_evidence_uuid");
  envelope.blast_radius_units = ParseU64(action.inputs, "blast_radius_units");
  envelope.max_blast_radius_units =
      ParseU64(action.inputs, "max_blast_radius_units");
  envelope.blast_radius_evidence_uuid =
      InputOr(action.inputs, "blast_radius_evidence_uuid");
  envelope.backup_check_required =
      ParseBool(action.inputs, "backup_check_required",
                !action.dry_run && AgentActionRolloutModeAllowsMutation(rollout.mode));
  envelope.checkpoint_check_required =
      ParseBool(action.inputs, "checkpoint_check_required",
                !action.dry_run && AgentActionRolloutModeAllowsMutation(rollout.mode));
  envelope.storage_check_required =
      ParseBool(action.inputs, "storage_check_required",
                !action.dry_run && AgentActionRolloutModeAllowsMutation(rollout.mode));
  envelope.transaction_check_required =
      ParseBool(action.inputs, "transaction_check_required",
                !action.dry_run && AgentActionRolloutModeAllowsMutation(rollout.mode));
  envelope.backup_evidence_uuid = InputOr(action.inputs, "backup_evidence_uuid");
  envelope.checkpoint_evidence_uuid =
      InputOr(action.inputs, "checkpoint_evidence_uuid");
  envelope.storage_check_evidence_uuid =
      InputOr(action.inputs, "storage_check_evidence_uuid");
  envelope.transaction_evidence_uuid =
      InputOr(action.inputs, "transaction_evidence_uuid");
  envelope.compensation_required =
      ParseBool(action.inputs, "compensation_required",
                !action.dry_run && AgentActionRolloutModeAllowsMutation(rollout.mode));
  envelope.rollback_required =
      ParseBool(action.inputs, "rollback_required",
                !action.dry_run && AgentActionRolloutModeAllowsMutation(rollout.mode));
  envelope.compensation_plan_evidence_uuid =
      InputOr(action.inputs, "compensation_plan_evidence_uuid");
  envelope.rollback_plan_evidence_uuid =
      InputOr(action.inputs, "rollback_plan_evidence_uuid");
  envelope.cluster_route_requested =
      ParseBool(action.inputs, "cluster_route_requested") ||
      provider.authority_domain == AgentActuatorAuthorityDomain::cluster_provider ||
      provider.cluster_scoped_contract ||
      action.agent_type_id.find("cluster_") == 0;
  envelope.external_cluster_provider_attested =
      ParseBool(action.inputs, "external_cluster_provider_attested");
  envelope.provider = provider;
  envelope.rollout = rollout;
  envelope.authority_claims = rollout.authority_claims;
  const auto input_claims = SplitCsv(InputOr(action.inputs, "authority_claims"));
  envelope.authority_claims.insert(envelope.authority_claims.end(),
                                   input_claims.begin(),
                                   input_claims.end());
  if (authority_parser_claim) { envelope.authority_claims.push_back("parser_authority"); }
  if (authority_client_claim) { envelope.authority_claims.push_back("client_authority"); }
  if (authority_donor_claim) { envelope.authority_claims.push_back("donor_authority"); }
  if (authority_sidecar_claim) { envelope.authority_claims.push_back("sidecar_authority"); }
  return envelope;
}

AgentRuntimeStatus ValidateAgentActionSafetyEnvelope(
    const AgentActionSafetyEnvelope& envelope) {
  if (!envelope.safety_envelope_present) {
    return AgentError("SB_AGENT_ACTION_SAFETY.ENVELOPE_REQUIRED",
                      envelope.action_uuid);
  }
  if (envelope.action_uuid.empty() || envelope.agent_type_id.empty() ||
      envelope.operation_id.empty() || envelope.actuator_id.empty()) {
    return AgentError("SB_AGENT_ACTION_SAFETY.ACTION_IDENTITY_REQUIRED");
  }
  if (envelope.safety_evidence_uuid.empty() ||
      envelope.input_evidence_digest.empty() ||
      envelope.metric_digest.empty() ||
      envelope.catalog_root_digest.empty() ||
      envelope.policy_evidence_uuid.empty() ||
      envelope.policy_generation == 0) {
    return AgentError("SB_AGENT_ACTION_SAFETY.EVIDENCE_REQUIRED",
                      envelope.action_uuid);
  }
  if (HasForbiddenAuthorityClaim(envelope.authority_claims)) {
    return AgentError("SB_AGENT_ACTION_SAFETY.FORBIDDEN_AUTHORITY_CLAIM",
                      envelope.action_uuid);
  }
  if (envelope.cluster_route_requested ||
      envelope.provider.authority_domain ==
          AgentActuatorAuthorityDomain::cluster_provider) {
    return AgentError("SB_AGENT_ACTION_SAFETY.CLUSTER_EXTERNAL_PROVIDER_REQUIRED",
                      envelope.action_uuid);
  }
  const bool live_mutation = !envelope.requested_dry_run;
  const auto rollout_status = ValidateAgentActionRolloutProfile(
      envelope.rollout, live_mutation, envelope.cluster_route_requested);
  if (!rollout_status.ok) { return rollout_status; }
  if (!envelope.provider.provider_registered) {
    return AgentError("SB_AGENT_ACTION_SAFETY.PROVIDER_REQUIRED",
                      envelope.action_uuid);
  }
  if (envelope.requested_dry_run) {
    if (!envelope.provider.supports_dry_run) {
      return AgentError("SB_AGENT_ACTION_SAFETY.DRY_RUN_UNSUPPORTED",
                        envelope.action_uuid);
    }
  } else {
    if (!envelope.provider.live_route_available ||
        !envelope.provider.real_subsystem_handler ||
        envelope.provider.fixture_or_simulated_provider) {
      return AgentError("SB_AGENT_ACTION_SAFETY.LIVE_PROVIDER_UNSAFE",
                        envelope.action_uuid);
    }
  }
  if (envelope.manual_approval_required &&
      (!envelope.manual_approval_present ||
       envelope.approval_evidence_uuid.empty())) {
    return AgentError("SB_AGENT_ACTION_SAFETY.APPROVAL_REQUIRED",
                      envelope.action_uuid);
  }
  if (envelope.rate_limit_key.empty() ||
      envelope.rate_limit_evidence_uuid.empty() ||
      envelope.rate_limit_per_window == 0) {
    return AgentError("SB_AGENT_ACTION_SAFETY.RATE_LIMIT_REQUIRED",
                      envelope.action_uuid);
  }
  if (envelope.action_count_in_window >= envelope.rate_limit_per_window) {
    return AgentError("SB_AGENT_ACTION_SAFETY.RATE_LIMIT_EXCEEDED",
                      envelope.action_uuid);
  }
  if (envelope.max_blast_radius_units == 0 ||
      envelope.blast_radius_units == 0 ||
      envelope.blast_radius_evidence_uuid.empty()) {
    return AgentError("SB_AGENT_ACTION_SAFETY.BLAST_RADIUS_REQUIRED",
                      envelope.action_uuid);
  }
  if (envelope.blast_radius_units > envelope.max_blast_radius_units) {
    return AgentError("SB_AGENT_ACTION_SAFETY.BLAST_RADIUS_EXCEEDED",
                      envelope.action_uuid);
  }
  if (envelope.backup_check_required && envelope.backup_evidence_uuid.empty()) {
    return AgentError("SB_AGENT_ACTION_SAFETY.BACKUP_CHECK_REQUIRED",
                      envelope.action_uuid);
  }
  if (envelope.checkpoint_check_required &&
      envelope.checkpoint_evidence_uuid.empty()) {
    return AgentError("SB_AGENT_ACTION_SAFETY.CHECKPOINT_CHECK_REQUIRED",
                      envelope.action_uuid);
  }
  if (envelope.storage_check_required &&
      envelope.storage_check_evidence_uuid.empty()) {
    return AgentError("SB_AGENT_ACTION_SAFETY.STORAGE_CHECK_REQUIRED",
                      envelope.action_uuid);
  }
  if (envelope.transaction_check_required &&
      envelope.transaction_evidence_uuid.empty()) {
    return AgentError("SB_AGENT_ACTION_SAFETY.TRANSACTION_CHECK_REQUIRED",
                      envelope.action_uuid);
  }
  if (live_mutation && envelope.provider.requires_outcome_verification &&
      !envelope.provider.supports_retry) {
    return AgentError("SB_AGENT_ACTION_SAFETY.RETRY_CAPABILITY_REQUIRED",
                      envelope.action_uuid);
  }
  if (envelope.compensation_required) {
    if (!envelope.provider.supports_rollback_compensation ||
        envelope.compensation_plan_evidence_uuid.empty()) {
      return AgentError("SB_AGENT_ACTION_SAFETY.COMPENSATION_REQUIRED",
                        envelope.action_uuid);
    }
  }
  if (envelope.rollback_required) {
    if (!envelope.provider.supports_rollback_compensation ||
        envelope.rollback_plan_evidence_uuid.empty()) {
      return AgentError("SB_AGENT_ACTION_SAFETY.ROLLBACK_REQUIRED",
                        envelope.action_uuid);
    }
  }
  return AgentOk();
}

}  // namespace scratchbird::core::agents

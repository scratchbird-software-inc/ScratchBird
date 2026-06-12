// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "agent_runtime.hpp"
#include "agent_rollout_profile.hpp"

// SEARCH_KEY: SB_AGENT_RUNTIME_AGENT_ACTION_SAFETY_HEADER
#include <map>
#include <string>
#include <vector>

namespace scratchbird::core::agents {

struct AgentActionSafetyProviderContext {
  bool provider_registered = false;
  bool supports_dry_run = false;
  bool live_route_available = false;
  bool real_subsystem_handler = false;
  bool fixture_or_simulated_provider = false;
  bool supports_retry = false;
  bool supports_rollback_compensation = false;
  bool requires_outcome_verification = true;
  AgentActuatorAuthorityDomain authority_domain =
      AgentActuatorAuthorityDomain::unknown;
  bool cluster_scoped_contract = false;
};

// SEARCH_KEY: CEIC_075_AGENT_ACTION_SAFETY_ENVELOPE
struct AgentActionSafetyEnvelope {
  std::string action_uuid;
  std::string agent_type_id;
  std::string operation_id;
  std::string actuator_id;
  std::string scope_uuid;
  std::string safety_evidence_uuid;
  std::string input_evidence_digest;
  std::string metric_digest;
  std::string catalog_root_digest;
  std::string policy_evidence_uuid;
  u64 policy_generation = 0;

  bool safety_envelope_present = false;
  bool requested_dry_run = true;
  bool manual_approval_required = false;
  bool manual_approval_present = false;
  std::string approval_evidence_uuid;

  u64 rate_limit_per_window = 0;
  u64 action_count_in_window = 0;
  std::string rate_limit_key;
  std::string rate_limit_evidence_uuid;

  u64 blast_radius_units = 0;
  u64 max_blast_radius_units = 0;
  std::string blast_radius_evidence_uuid;

  bool backup_check_required = false;
  bool checkpoint_check_required = false;
  bool storage_check_required = false;
  bool transaction_check_required = false;
  std::string backup_evidence_uuid;
  std::string checkpoint_evidence_uuid;
  std::string storage_check_evidence_uuid;
  std::string transaction_evidence_uuid;

  bool compensation_required = false;
  bool rollback_required = false;
  std::string compensation_plan_evidence_uuid;
  std::string rollback_plan_evidence_uuid;

  bool cluster_route_requested = false;
  bool external_cluster_provider_attested = false;
  AgentActionSafetyProviderContext provider;
  AgentActionRolloutProfile rollout;
  std::vector<std::string> authority_claims;
};

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
    bool authority_reference_claim,
    bool authority_sidecar_claim);

AgentRuntimeStatus ValidateAgentActionSafetyEnvelope(
    const AgentActionSafetyEnvelope& envelope);

}  // namespace scratchbird::core::agents

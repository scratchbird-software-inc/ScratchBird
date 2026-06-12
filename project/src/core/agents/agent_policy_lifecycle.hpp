// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// SEARCH_KEY: CEIC_071_SIGNED_TYPED_POLICY_LIFECYCLE
//
// Signed typed policy lifecycle validation for operational agents. This surface
// validates policy evidence only; it does not own transaction finality,
// visibility, authorization/security, recovery, parser execution, reference
// behavior, WAL recovery, benchmark truth, optimizer plans, index finality,
// provider finality, cluster authority, memory authority, or agent actions.

#include "agent_policy_schema.hpp"
#include "agent_system_profile.hpp"

#include <optional>
#include <string>
#include <vector>

namespace scratchbird::core::agents {

enum class AgentPolicyLifecycleState {
  disabled,
  advisory,
  active,
  superseded,
  rollback_only,
  retired
};

struct AgentPolicyTypedFieldEvidence {
  std::string name;
  AgentPolicyFieldType type = AgentPolicyFieldType::token;
  std::string units;
  std::string value;
  std::optional<double> minimum;
  std::optional<double> maximum;
  AgentPolicyFieldSensitivity sensitivity =
      AgentPolicyFieldSensitivity::operational;
  bool required = true;
};

struct AgentSignedPolicyLifecycle {
  std::string schema_version = "sb.agent.policy.lifecycle.v1";
  std::string agent_type_id;
  AgentPolicy policy;
  std::vector<AgentPolicyTypedFieldEvidence> typed_fields;

  std::string author_principal_uuid;
  std::string approver_principal_uuid;
  std::string approval_evidence_uuid;

  AgentPolicyLifecycleState lifecycle_state =
      AgentPolicyLifecycleState::active;
  u64 generation = 0;
  u64 issued_at_microseconds = 0;
  u64 activation_time_microseconds = 0;
  u64 expiry_time_microseconds = 0;
  u64 max_staleness_microseconds = 0;

  std::string supersedes_policy_uuid;
  u64 supersedes_policy_generation = 0;
  std::string rollback_policy_uuid;
  u64 rollback_policy_generation = 0;

  u64 engine_min_generation = 0;
  u64 engine_max_generation = 0;

  std::string coupled_profile_agent_type_id;
  u64 coupled_profile_generation = 0;
  std::string coupled_profile_digest;

  std::string key_policy_id;
  std::string key_policy_provenance;
  u64 key_policy_generation = 0;
  std::string signing_key_id;
  std::string signing_key_provenance;
  u64 signing_key_generation = 0;

  std::string policy_digest_algorithm = "sha256-v1";
  std::string policy_digest;
  std::string policy_signature_algorithm = "hmac-sha256-v1";
  std::string policy_signature;

  bool production_live_policy = false;
  bool local_cluster_policy_claim = false;

  AgentSystemProfileForbiddenAuthority no_authority;
};

struct AgentPolicyLifecycleValidationContext {
  u64 now_microseconds = 0;
  u64 engine_generation = 0;
  bool production_environment = false;
  const AgentSystemProfile* system_profile = nullptr;
  AgentSystemProfileValidationContext profile_context;
};

struct AgentPolicyLifecycleValidationResult {
  AgentRuntimeStatus status;
  bool digest_valid = false;
  bool signature_valid = false;
  bool typed_fields_valid = false;
  bool lifecycle_time_valid = false;
  bool engine_bounds_valid = false;
  bool profile_coupling_valid = false;
  bool profile_validation_passed = false;
  bool production_live_policy = false;
  bool authority_clean = false;
  std::vector<std::string> evidence_fields;
};

const char* AgentPolicyLifecycleStateName(
    AgentPolicyLifecycleState state);

std::string AgentPolicyLifecycleDigest(
    const AgentSignedPolicyLifecycle& lifecycle);
std::string AgentPolicyLifecycleSignatureDigest(
    const AgentSignedPolicyLifecycle& lifecycle);
void FinalizeAgentSignedPolicyLifecycle(
    AgentSignedPolicyLifecycle* lifecycle);

std::vector<AgentPolicyTypedFieldEvidence>
AgentPolicyTypedFieldEvidenceFromPolicy(const AgentPolicy& policy);

AgentPolicyLifecycleValidationResult ValidateAgentSignedPolicyLifecycle(
    const AgentSignedPolicyLifecycle& lifecycle,
    const AgentPolicyLifecycleValidationContext& context =
        AgentPolicyLifecycleValidationContext{});

}  // namespace scratchbird::core::agents

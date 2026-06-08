// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// SEARCH_KEY: CEIC_072_AGENT_POLICY_OVERRIDE_RESOLUTION
//
// Signed policy inheritance and override resolution for operational agents.
// This resolver validates already-signed lifecycle inputs and produces
// evidence-only resolved policy state. It does not own transaction finality,
// visibility, authorization/security, recovery, parser execution, donor
// behavior, WAL recovery, benchmark truth, optimizer plans, index finality,
// provider finality, cluster authority, memory authority, or agent actions.

#include "agent_policy_lifecycle.hpp"

#include <string>
#include <vector>

namespace scratchbird::core::agents {

enum class AgentPolicyScopeKind {
  unknown,
  root,
  database,
  filespace,
  tenant,
  application,
  session
};

struct AgentPolicyOverrideApproval {
  bool override_permission_granted = false;
  std::string approval_uuid;
  std::string approver_principal_uuid;
  u64 created_at_microseconds = 0;
  u64 expires_at_microseconds = 0;
  std::vector<std::string> allowed_override_fields;
};

struct AgentPolicyOverrideLayer {
  AgentPolicyScopeKind scope_kind = AgentPolicyScopeKind::unknown;
  std::string scope_uuid;
  AgentSignedPolicyLifecycle lifecycle;
  AgentPolicyOverrideApproval approval;
};

struct AgentPolicyResolvedField {
  std::string field_name;
  std::string value;
  AgentPolicyScopeKind source_scope_kind = AgentPolicyScopeKind::unknown;
  std::string source_scope_uuid;
  std::string source_policy_uuid;
  u64 source_policy_generation = 0;
  bool overridden = false;
  std::string prior_value;
  std::string approval_uuid;
};

struct AgentPolicyOverrideConflict {
  AgentPolicyScopeKind scope_kind = AgentPolicyScopeKind::unknown;
  std::string scope_uuid;
  std::string field_name;
  std::string diagnostic_code;
  std::string detail;
};

struct AgentPolicyOverrideResolutionContext {
  AgentPolicyLifecycleValidationContext lifecycle_context;
  u64 now_microseconds = 0;
  bool production_environment = false;
};

struct AgentPolicyOverrideResolutionResult {
  AgentRuntimeStatus status;
  bool root_present = false;
  bool order_valid = false;
  bool all_lifecycles_valid = false;
  bool approvals_valid = false;
  bool conflicts_absent = false;
  bool digest_valid = false;
  bool authority_clean = false;
  AgentSignedPolicyLifecycle resolved_lifecycle;
  std::string resolved_policy_uuid;
  std::string resolved_policy_digest_algorithm = "sha256-v1";
  std::string resolved_policy_digest;
  std::vector<std::string> applied_layers;
  std::vector<AgentPolicyResolvedField> resolved_fields;
  std::vector<AgentPolicyOverrideConflict> conflicts;
  std::vector<std::string> evidence_fields;
};

const char* AgentPolicyScopeKindName(AgentPolicyScopeKind kind);

std::string AgentPolicyOverrideResolutionDigest(
    const std::vector<AgentPolicyOverrideLayer>& layers);

AgentPolicyOverrideResolutionResult ResolveAgentPolicyOverrides(
    const std::vector<AgentPolicyOverrideLayer>& layers,
    const AgentPolicyOverrideResolutionContext& context =
        AgentPolicyOverrideResolutionContext{});

}  // namespace scratchbird::core::agents

// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// SEARCH_KEY: CEIC_076_AGENT_MANUAL_APPROVAL_WORKFLOW

#include "agent_runtime.hpp"

#include <string>
#include <vector>

namespace scratchbird::core::agents {

enum class AgentManualApprovalMode {
  not_required,
  normal_two_person,
  emergency_break_glass
};

struct AgentManualApprovalGrant {
  std::string approval_uuid;
  std::string action_uuid;
  std::string agent_type_id;
  std::string operation_id;
  std::string scope_uuid;
  std::string principal_uuid;
  std::string evidence_uuid;
  std::string ticket_id;
  u64 policy_generation = 0;
  u64 approved_at_microseconds = 0;
  u64 expires_at_microseconds = 0;
  bool approved = true;
  bool revoked = false;
  u64 revoked_at_microseconds = 0;
  std::string revocation_evidence_uuid;
  bool emergency_approval = false;
  std::vector<std::string> authority_claims;
};

struct AgentManualApprovalNotificationProof {
  bool required = true;
  bool triggered = false;
  std::string evidence_uuid;
  std::string channel;
  std::vector<std::string> notified_principal_uuids;
};

struct AgentManualApprovalEscalationProof {
  bool required = false;
  bool triggered = false;
  std::string evidence_uuid;
  std::string escalation_chain_id;
  std::vector<std::string> escalated_principal_uuids;
};

struct AgentBreakGlassWorkflow {
  bool requested = false;
  std::string reason;
  std::string scope_uuid;
  std::string ticket_id;
  std::string activated_by_principal_uuid;
  u64 activated_at_microseconds = 0;
  u64 expires_at_microseconds = 0;
  u64 max_duration_microseconds = 0;
  u64 review_deadline_microseconds = 0;
  std::vector<std::string> authority_claims;
};

struct AgentManualApprovalWorkflowRequest {
  std::string workflow_version;
  std::string action_uuid;
  std::string agent_type_id;
  std::string operation_id;
  std::string scope_uuid;
  u64 policy_generation = 0;
  std::string requester_principal_uuid;
  std::string workflow_evidence_uuid;
  std::string ticket_id;
  u64 now_microseconds = 0;
  u64 review_deadline_microseconds = 0;
  bool approval_required = false;
  bool manual_approval_present = false;
  bool dry_run = true;
  bool cluster_route_requested = false;
  bool external_cluster_provider_attested = false;
  AgentManualApprovalNotificationProof notification;
  AgentManualApprovalEscalationProof escalation;
  AgentBreakGlassWorkflow break_glass;
  std::vector<AgentManualApprovalGrant> approvals;
  std::vector<std::string> authority_claims;
};

struct AgentManualApprovalWorkflowEvaluation {
  AgentRuntimeStatus status;
  AgentManualApprovalMode mode = AgentManualApprovalMode::not_required;
  bool required = false;
  bool accepted = false;
  bool break_glass_accepted = false;
  bool notification_evidence_present = false;
  bool escalation_evidence_present = false;
  bool review_deadline_present = false;
  std::string approval_evidence_uuid;
  std::vector<std::string> accepted_approval_uuids;
};

const char* AgentManualApprovalModeName(AgentManualApprovalMode mode);
AgentManualApprovalWorkflowRequest AgentManualApprovalWorkflowFromActionInputs(
    const AgentActionRequest& action,
    const std::string& requester_principal_uuid,
    const std::string& scope_uuid,
    u64 policy_generation,
    u64 now_microseconds,
    bool contract_or_safety_approval_required,
    bool cluster_route_requested,
    bool external_cluster_provider_attested);
AgentManualApprovalWorkflowEvaluation ValidateAgentManualApprovalWorkflow(
    const AgentManualApprovalWorkflowRequest& request);

}  // namespace scratchbird::core::agents

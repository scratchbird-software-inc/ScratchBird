// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "agent_manual_approval.hpp"

#include <algorithm>
#include <cctype>
#include <map>
#include <set>
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

bool ParseBool(const std::map<std::string, std::string>& inputs,
               const std::string& key,
               bool fallback = false) {
  const auto value = Lower(InputOr(inputs, key));
  if (value.empty()) { return fallback; }
  return value == "1" || value == "true" || value == "yes";
}

u64 ParseU64Text(const std::string& value, u64 fallback = 0) {
  if (value.empty()) { return fallback; }
  try {
    return static_cast<u64>(std::stoull(value));
  } catch (const std::exception&) {
    return fallback;
  }
}

u64 ParseU64(const std::map<std::string, std::string>& inputs,
             const std::string& key,
             u64 fallback = 0) {
  return ParseU64Text(InputOr(inputs, key), fallback);
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

std::string ApprovalPrefix(std::size_t index) {
  return "approval_" + std::to_string(index) + "_";
}

AgentManualApprovalGrant GrantFromInputs(const AgentActionRequest& action,
                                         std::size_t index,
                                         u64 policy_generation,
                                         const std::string& scope_uuid,
                                         const std::string& ticket_id) {
  const auto prefix = ApprovalPrefix(index);
  AgentManualApprovalGrant grant;
  grant.approval_uuid = InputOr(action.inputs, prefix + "uuid");
  grant.action_uuid = InputOr(action.inputs, prefix + "action_uuid", action.action_uuid);
  grant.agent_type_id =
      InputOr(action.inputs, prefix + "agent_type_id", action.agent_type_id);
  grant.operation_id =
      InputOr(action.inputs, prefix + "operation_id", action.operation_id);
  grant.scope_uuid = InputOr(action.inputs, prefix + "scope_uuid", scope_uuid);
  grant.principal_uuid = InputOr(action.inputs, prefix + "principal_uuid");
  grant.evidence_uuid = InputOr(action.inputs, prefix + "evidence_uuid");
  grant.ticket_id = InputOr(action.inputs, prefix + "ticket_id", ticket_id);
  grant.policy_generation =
      ParseU64(action.inputs, prefix + "policy_generation", policy_generation);
  grant.approved_at_microseconds =
      ParseU64(action.inputs, prefix + "approved_at_microseconds");
  grant.expires_at_microseconds =
      ParseU64(action.inputs, prefix + "expires_at_microseconds");
  grant.approved = ParseBool(action.inputs, prefix + "approved", true);
  grant.revoked = ParseBool(action.inputs, prefix + "revoked");
  grant.revoked_at_microseconds =
      ParseU64(action.inputs, prefix + "revoked_at_microseconds");
  grant.revocation_evidence_uuid =
      InputOr(action.inputs, prefix + "revocation_evidence_uuid");
  grant.emergency_approval =
      ParseBool(action.inputs, prefix + "emergency_approval");
  grant.authority_claims =
      SplitCsv(InputOr(action.inputs, prefix + "authority_claims"));
  return grant;
}

AgentManualApprovalWorkflowEvaluation Refuse(std::string code,
                                             std::string detail,
                                             AgentManualApprovalMode mode,
                                             bool required) {
  AgentManualApprovalWorkflowEvaluation evaluation;
  evaluation.status = AgentError(std::move(code), std::move(detail));
  evaluation.mode = mode;
  evaluation.required = required;
  return evaluation;
}

bool IsValidGrantForRequest(const AgentManualApprovalWorkflowRequest& request,
                            const AgentManualApprovalGrant& grant,
                            AgentRuntimeStatus* status) {
  if (grant.approval_uuid.empty() || grant.principal_uuid.empty() ||
      grant.evidence_uuid.empty() || grant.ticket_id.empty() ||
      grant.action_uuid.empty() || grant.agent_type_id.empty() ||
      grant.operation_id.empty() || grant.scope_uuid.empty() ||
      grant.policy_generation == 0 || grant.approved_at_microseconds == 0 ||
      grant.expires_at_microseconds == 0) {
    *status = AgentError("SB_AGENT_APPROVAL_WORKFLOW.APPROVAL_EVIDENCE_REQUIRED",
                         request.action_uuid);
    return false;
  }
  if (HasForbiddenAuthorityClaim(grant.authority_claims)) {
    *status = AgentError("SB_AGENT_APPROVAL_WORKFLOW.FORBIDDEN_AUTHORITY_CLAIM",
                         grant.approval_uuid);
    return false;
  }
  if (!grant.approved) {
    *status = AgentError("SB_AGENT_APPROVAL_WORKFLOW.APPROVAL_DENIED",
                         grant.approval_uuid);
    return false;
  }
  if (grant.revoked) {
    *status = AgentError("SB_AGENT_APPROVAL_WORKFLOW.APPROVAL_REVOKED",
                         grant.approval_uuid);
    return false;
  }
  if (request.now_microseconds < grant.approved_at_microseconds ||
      request.now_microseconds >= grant.expires_at_microseconds) {
    *status = AgentError("SB_AGENT_APPROVAL_WORKFLOW.APPROVAL_EXPIRED",
                         grant.approval_uuid);
    return false;
  }
  if (grant.action_uuid != request.action_uuid ||
      grant.agent_type_id != request.agent_type_id ||
      grant.operation_id != request.operation_id) {
    *status = AgentError("SB_AGENT_APPROVAL_WORKFLOW.ACTION_BINDING_MISMATCH",
                         grant.approval_uuid);
    return false;
  }
  if (grant.scope_uuid != request.scope_uuid) {
    *status = AgentError("SB_AGENT_APPROVAL_WORKFLOW.SCOPE_MISMATCH",
                         grant.approval_uuid);
    return false;
  }
  if (grant.policy_generation != request.policy_generation) {
    *status = AgentError("SB_AGENT_APPROVAL_WORKFLOW.POLICY_GENERATION_MISMATCH",
                         grant.approval_uuid);
    return false;
  }
  if (grant.ticket_id != request.ticket_id) {
    *status = AgentError("SB_AGENT_APPROVAL_WORKFLOW.TICKET_MISMATCH",
                         grant.approval_uuid);
    return false;
  }
  if (grant.principal_uuid == request.requester_principal_uuid) {
    *status = AgentError("SB_AGENT_APPROVAL_WORKFLOW.SAME_PRINCIPAL_REFUSED",
                         grant.approval_uuid);
    return false;
  }
  if (request.break_glass.requested &&
      grant.principal_uuid ==
          request.break_glass.activated_by_principal_uuid) {
    *status = AgentError(
        "SB_AGENT_APPROVAL_WORKFLOW.BREAK_GLASS_ACTIVATOR_APPROVER_SEPARATION_REQUIRED",
        grant.approval_uuid);
    return false;
  }
  *status = AgentOk();
  return true;
}

}  // namespace

const char* AgentManualApprovalModeName(AgentManualApprovalMode mode) {
  switch (mode) {
    case AgentManualApprovalMode::not_required: return "not_required";
    case AgentManualApprovalMode::normal_two_person: return "normal_two_person";
    case AgentManualApprovalMode::emergency_break_glass:
      return "emergency_break_glass";
  }
  return "not_required";
}

AgentManualApprovalWorkflowRequest AgentManualApprovalWorkflowFromActionInputs(
    const AgentActionRequest& action,
    const std::string& requester_principal_uuid,
    const std::string& scope_uuid,
    u64 policy_generation,
    u64 now_microseconds,
    bool contract_or_safety_approval_required,
    bool cluster_route_requested,
    bool external_cluster_provider_attested) {
  AgentManualApprovalWorkflowRequest request;
  request.workflow_version =
      InputOr(action.inputs, "approval_workflow_version");
  request.action_uuid = action.action_uuid;
  request.agent_type_id = action.agent_type_id;
  request.operation_id = action.operation_id;
  request.scope_uuid = scope_uuid;
  request.policy_generation = policy_generation;
  request.requester_principal_uuid = requester_principal_uuid;
  request.workflow_evidence_uuid =
      InputOr(action.inputs, "approval_evidence_uuid");
  request.ticket_id = InputOr(action.inputs, "approval_ticket_id");
  request.now_microseconds = now_microseconds;
  request.review_deadline_microseconds =
      ParseU64(action.inputs, "approval_review_deadline_microseconds");
  request.approval_required =
      contract_or_safety_approval_required ||
      ParseBool(action.inputs, "approval_required") ||
      ParseBool(action.inputs, "manual_approval_required");
  request.manual_approval_present =
      action.manual_approval_present ||
      ParseBool(action.inputs, "manual_approval_present");
  request.dry_run = action.dry_run;
  request.cluster_route_requested =
      cluster_route_requested ||
      ParseBool(action.inputs, "cluster_route_requested") ||
      action.agent_type_id.find("cluster_") == 0;
  request.external_cluster_provider_attested =
      external_cluster_provider_attested ||
      ParseBool(action.inputs, "external_cluster_provider_attested");

  request.notification.required =
      ParseBool(action.inputs, "approval_notification_required", true);
  request.notification.triggered =
      ParseBool(action.inputs, "approval_notification_triggered");
  request.notification.evidence_uuid =
      InputOr(action.inputs, "approval_notification_evidence_uuid");
  request.notification.channel =
      InputOr(action.inputs, "approval_notification_channel");
  request.notification.notified_principal_uuids =
      SplitCsv(InputOr(action.inputs, "approval_notified_principals"));

  request.escalation.required =
      ParseBool(action.inputs, "approval_escalation_required");
  request.escalation.triggered =
      ParseBool(action.inputs, "approval_escalation_triggered");
  request.escalation.evidence_uuid =
      InputOr(action.inputs, "approval_escalation_evidence_uuid");
  request.escalation.escalation_chain_id =
      InputOr(action.inputs, "approval_escalation_chain_id");
  request.escalation.escalated_principal_uuids =
      SplitCsv(InputOr(action.inputs, "approval_escalated_principals"));

  request.break_glass.requested =
      ParseBool(action.inputs, "break_glass_requested");
  request.break_glass.reason = InputOr(action.inputs, "break_glass_reason");
  request.break_glass.scope_uuid =
      InputOr(action.inputs, "break_glass_scope_uuid", scope_uuid);
  request.break_glass.ticket_id =
      InputOr(action.inputs, "break_glass_ticket_id", request.ticket_id);
  request.break_glass.activated_by_principal_uuid =
      InputOr(action.inputs, "break_glass_activated_by_principal_uuid",
              requester_principal_uuid);
  request.break_glass.activated_at_microseconds =
      ParseU64(action.inputs, "break_glass_activated_at_microseconds");
  request.break_glass.expires_at_microseconds =
      ParseU64(action.inputs, "break_glass_expires_at_microseconds");
  request.break_glass.max_duration_microseconds =
      ParseU64(action.inputs, "break_glass_max_duration_microseconds");
  request.break_glass.review_deadline_microseconds =
      ParseU64(action.inputs, "break_glass_review_deadline_microseconds",
               request.review_deadline_microseconds);
  request.break_glass.authority_claims =
      SplitCsv(InputOr(action.inputs, "break_glass_authority_claims"));
  if (request.break_glass.requested) {
    request.escalation.required = true;
    request.approval_required = true;
  }

  request.authority_claims =
      SplitCsv(InputOr(action.inputs, "approval_authority_claims"));
  const std::size_t count =
      static_cast<std::size_t>(ParseU64(action.inputs, "approval_count"));
  for (std::size_t i = 1; i <= count; ++i) {
    request.approvals.push_back(
        GrantFromInputs(action, i, policy_generation, scope_uuid,
                        request.ticket_id));
  }
  return request;
}

AgentManualApprovalWorkflowEvaluation ValidateAgentManualApprovalWorkflow(
    const AgentManualApprovalWorkflowRequest& request) {
  const bool required = request.approval_required || request.break_glass.requested;
  const AgentManualApprovalMode mode =
      !required ? AgentManualApprovalMode::not_required
                : request.break_glass.requested
                      ? AgentManualApprovalMode::emergency_break_glass
                      : AgentManualApprovalMode::normal_two_person;
  if (!required) {
    AgentManualApprovalWorkflowEvaluation evaluation;
    evaluation.status = {true, "SB_AGENT_APPROVAL_WORKFLOW.NOT_REQUIRED",
                         request.action_uuid};
    evaluation.mode = mode;
    evaluation.required = false;
    evaluation.accepted = true;
    return evaluation;
  }
  if ((request.cluster_route_requested ||
       request.agent_type_id.find("cluster_") == 0) &&
      !request.external_cluster_provider_attested) {
    return Refuse("SB_AGENT_APPROVAL_WORKFLOW.CLUSTER_EXTERNAL_PROVIDER_REQUIRED",
                  request.agent_type_id, mode, required);
  }
  if (request.workflow_version != "1" ||
      request.action_uuid.empty() ||
      request.agent_type_id.empty() ||
      request.operation_id.empty() ||
      request.scope_uuid.empty() ||
      request.policy_generation == 0 ||
      request.requester_principal_uuid.empty() ||
      request.workflow_evidence_uuid.empty() ||
      request.now_microseconds == 0) {
    return Refuse("SB_AGENT_APPROVAL_WORKFLOW.WORKFLOW_REQUIRED",
                  request.action_uuid, mode, required);
  }
  if (!request.manual_approval_present) {
    return Refuse("SB_AGENT_APPROVAL_WORKFLOW.MANUAL_APPROVAL_REQUIRED",
                  request.action_uuid, mode, required);
  }
  if (request.ticket_id.empty()) {
    return Refuse("SB_AGENT_APPROVAL_WORKFLOW.TICKET_REQUIRED",
                  request.action_uuid, mode, required);
  }
  if (HasForbiddenAuthorityClaim(request.authority_claims) ||
      HasForbiddenAuthorityClaim(request.break_glass.authority_claims)) {
    return Refuse("SB_AGENT_APPROVAL_WORKFLOW.FORBIDDEN_AUTHORITY_CLAIM",
                  request.action_uuid, mode, required);
  }
  if (request.notification.required &&
      (!request.notification.triggered ||
       request.notification.evidence_uuid.empty() ||
       request.notification.channel.empty() ||
       request.notification.notified_principal_uuids.empty())) {
    return Refuse("SB_AGENT_APPROVAL_WORKFLOW.NOTIFICATION_REQUIRED",
                  request.action_uuid, mode, required);
  }
  if (request.review_deadline_microseconds == 0 ||
      request.review_deadline_microseconds <= request.now_microseconds) {
    return Refuse("SB_AGENT_APPROVAL_WORKFLOW.REVIEW_DEADLINE_REQUIRED",
                  request.action_uuid, mode, required);
  }
  if (request.break_glass.requested) {
    if (request.break_glass.reason.empty() ||
        request.break_glass.ticket_id.empty() ||
        request.break_glass.scope_uuid.empty() ||
        request.break_glass.activated_by_principal_uuid.empty() ||
        request.break_glass.activated_at_microseconds == 0 ||
        request.break_glass.expires_at_microseconds == 0 ||
        request.break_glass.max_duration_microseconds == 0 ||
        request.break_glass.review_deadline_microseconds == 0) {
      return Refuse("SB_AGENT_APPROVAL_WORKFLOW.BREAK_GLASS_REQUIRED",
                    request.action_uuid, mode, required);
    }
    if (request.break_glass.scope_uuid != request.scope_uuid) {
      return Refuse("SB_AGENT_APPROVAL_WORKFLOW.SCOPE_MISMATCH",
                    request.action_uuid, mode, required);
    }
    if (request.break_glass.ticket_id != request.ticket_id) {
      return Refuse("SB_AGENT_APPROVAL_WORKFLOW.TICKET_MISMATCH",
                    request.action_uuid, mode, required);
    }
    if (request.now_microseconds < request.break_glass.activated_at_microseconds ||
        request.now_microseconds >= request.break_glass.expires_at_microseconds) {
      return Refuse("SB_AGENT_APPROVAL_WORKFLOW.BREAK_GLASS_EXPIRED",
                    request.action_uuid, mode, required);
    }
    const u64 duration = request.break_glass.expires_at_microseconds -
                         request.break_glass.activated_at_microseconds;
    if (duration == 0 || duration > request.break_glass.max_duration_microseconds) {
      return Refuse("SB_AGENT_APPROVAL_WORKFLOW.BREAK_GLASS_DURATION_EXCEEDED",
                    request.action_uuid, mode, required);
    }
    if (request.break_glass.review_deadline_microseconds <=
        request.now_microseconds) {
      return Refuse("SB_AGENT_APPROVAL_WORKFLOW.REVIEW_DEADLINE_REQUIRED",
                    request.action_uuid, mode, required);
    }
    if (request.escalation.required &&
        (!request.escalation.triggered ||
         request.escalation.evidence_uuid.empty() ||
         request.escalation.escalation_chain_id.empty() ||
         request.escalation.escalated_principal_uuids.empty())) {
      return Refuse("SB_AGENT_APPROVAL_WORKFLOW.ESCALATION_REQUIRED",
                    request.action_uuid, mode, required);
    }
  }

  std::set<std::string> accepted_principals;
  std::vector<std::string> accepted_approval_uuids;
  bool emergency_approval_seen = false;
  for (const auto& grant : request.approvals) {
    AgentRuntimeStatus status;
    if (!IsValidGrantForRequest(request, grant, &status)) {
      return Refuse(status.diagnostic_code, status.detail, mode, required);
    }
    if (!accepted_principals.insert(grant.principal_uuid).second) {
      return Refuse("SB_AGENT_APPROVAL_WORKFLOW.TWO_PERSON_SEPARATION_REQUIRED",
                    grant.principal_uuid, mode, required);
    }
    emergency_approval_seen = emergency_approval_seen || grant.emergency_approval;
    accepted_approval_uuids.push_back(grant.approval_uuid);
  }

  const std::size_t required_approvals =
      request.break_glass.requested ? 1U : 2U;
  if (accepted_principals.size() < required_approvals ||
      (!request.break_glass.requested && accepted_principals.size() < 2U)) {
    return Refuse("SB_AGENT_APPROVAL_WORKFLOW.TWO_PERSON_SEPARATION_REQUIRED",
                  request.action_uuid, mode, required);
  }
  if (request.break_glass.requested && !emergency_approval_seen) {
    return Refuse("SB_AGENT_APPROVAL_WORKFLOW.BREAK_GLASS_APPROVAL_REQUIRED",
                  request.action_uuid, mode, required);
  }

  AgentManualApprovalWorkflowEvaluation evaluation;
  evaluation.status = {true,
                       request.break_glass.requested
                           ? "SB_AGENT_APPROVAL_WORKFLOW.BREAK_GLASS_ACCEPTED"
                           : "SB_AGENT_APPROVAL_WORKFLOW.APPROVED",
                       request.workflow_evidence_uuid};
  evaluation.mode = mode;
  evaluation.required = true;
  evaluation.accepted = true;
  evaluation.break_glass_accepted = request.break_glass.requested;
  evaluation.notification_evidence_present =
      request.notification.triggered &&
      !request.notification.evidence_uuid.empty();
  evaluation.escalation_evidence_present =
      request.escalation.triggered &&
      !request.escalation.evidence_uuid.empty();
  evaluation.review_deadline_present = true;
  evaluation.approval_evidence_uuid = request.workflow_evidence_uuid;
  evaluation.accepted_approval_uuids = std::move(accepted_approval_uuids);
  return evaluation;
}

}  // namespace scratchbird::core::agents

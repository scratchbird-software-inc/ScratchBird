// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "agent_tenant_coordination.hpp"

#include <algorithm>
#include <map>
#include <set>
#include <utility>

namespace scratchbird::core::agents {
namespace {

bool IsLiveAction(const AgentPolicy& policy,
                  const AgentTenantCoordinationRequest& request) {
  return request.live_action_requested ||
         (policy.activation == AgentActivationProfile::live_action &&
          policy.allow_live_action);
}

bool IsMutableAction(const AgentTenantCoordinationRequest& request) {
  return request.mutable_action_requested ||
         request.lock_request.mutates_resource ||
         IsLiveAction(request.policy, request);
}

bool OverLimit(u64 limit, u64 current, u64 requested = 0) {
  return limit != 0 && current + requested > limit;
}

bool AuthorityClean(const AgentSystemProfileForbiddenAuthority& authority) {
  return !authority.transaction_finality_authority &&
         !authority.visibility_authority &&
         !authority.authorization_security_authority &&
         !authority.recovery_authority &&
         !authority.parser_authority &&
         !authority.reference_authority &&
         !authority.wal_authority &&
         !authority.benchmark_authority &&
         !authority.optimizer_plan_authority &&
         !authority.index_finality_authority &&
         !authority.provider_finality_authority &&
         !authority.cluster_authority &&
         !authority.memory_authority &&
         !authority.agent_action_authority;
}

bool HasForbiddenAuthorityClaim(const std::vector<std::string>& claims) {
  static const std::set<std::string> forbidden = {
      "transaction_finality",
      "visibility",
      "authorization_security",
      "security",
      "recovery",
      "parser",
      "reference",
      "wal",
      "benchmark",
      "optimizer_plan",
      "index_finality",
      "provider_finality",
      "cluster",
      "memory",
      "agent_action"};
  for (const auto& claim : claims) {
    if (forbidden.count(claim) != 0) { return true; }
  }
  return false;
}

AgentTenantCoordinationDecision Refuse(
    AgentTenantCoordinationDecision result,
    std::string code,
    std::string detail) {
  result.status = AgentError(std::move(code), std::move(detail));
  result.decision = AgentTenantCoordinationDecisionKind::refused;
  result.admitted = false;
  result.queued = false;
  result.fail_closed = true;
  result.evidence_fields.push_back("diagnostic_code=" +
                                   result.status.diagnostic_code);
  result.evidence_fields.push_back("diagnostic_detail=" +
                                   result.status.detail);
  return result;
}

AgentTenantCoordinationDecision Queue(
    AgentTenantCoordinationDecision result,
    std::string code,
    std::string detail) {
  result.status = AgentRuntimeStatus{true, std::move(code), std::move(detail)};
  result.decision = AgentTenantCoordinationDecisionKind::queued;
  result.admitted = false;
  result.queued = true;
  result.fail_closed = false;
  result.evidence_fields.push_back("queued=true");
  result.evidence_fields.push_back("diagnostic_code=" +
                                   result.status.diagnostic_code);
  result.evidence_fields.push_back("diagnostic_detail=" +
                                   result.status.detail);
  return result;
}

std::vector<std::string> BaseEvidence(
    const AgentTenantCoordinationRequest& request) {
  return {
      "source=agent_tenant_coordination",
      "schema_version=sb.agent.tenant_coordination.v1",
      "agent_type_id=" + request.descriptor.type_id,
      "policy_uuid=" + request.policy.policy_uuid,
      "policy_generation=" + std::to_string(request.policy.policy_generation),
      "action_id=" + request.action_id,
      "tenant_uuid=" + request.tenant_budget.tenant_uuid,
      "coordination_group_id=" + request.coordination_group.group_id,
      "requester_instance_id=" + request.requester_instance_id,
      "transaction_finality_authority=false",
      "visibility_authority=false",
      "authorization_security_authority=false",
      "recovery_authority=false",
      "parser_authority=false",
      "reference_authority=false",
      "wal_authority=false",
      "benchmark_authority=false",
      "optimizer_plan_authority=false",
      "index_finality_authority=false",
      "provider_finality_authority=false",
      "cluster_authority=false",
      "memory_authority=false",
      "agent_action_authority=false"};
}

std::vector<std::string> DefaultRequiredMetricFamilies() {
  return {
      "tenant.workload.quota",
      "agent.coordination.group",
      "agent.coordination.lock"};
}

const AgentTenantCoordinationMember* FindRequester(
    const AgentTenantCoordinationGroup& group,
    const std::string& requester_instance_id) {
  for (const auto& member : group.members) {
    if (member.instance_id == requester_instance_id) { return &member; }
  }
  return nullptr;
}

bool MemberHeartbeatFresh(const AgentTenantCoordinationMember& member) {
  return member.heartbeat_fresh &&
         (member.max_heartbeat_age_microseconds == 0 ||
          member.heartbeat_age_microseconds <=
              member.max_heartbeat_age_microseconds);
}

const AgentTenantCoordinationMember* SelectLeader(
    const AgentTenantCoordinationGroup& group) {
  const AgentTenantCoordinationMember* selected = nullptr;
  for (const auto& member : group.members) {
    if (member.role != AgentTenantCoordinationRole::leader ||
        !member.healthy ||
        !MemberHeartbeatFresh(member) ||
        !member.live_action_capable ||
        member.external_cluster_member) {
      continue;
    }
    if (selected == nullptr ||
        member.election_term > selected->election_term ||
        (member.election_term == selected->election_term &&
         member.priority > selected->priority) ||
        (member.election_term == selected->election_term &&
         member.priority == selected->priority &&
         member.instance_id < selected->instance_id)) {
      selected = &member;
    }
  }
  return selected;
}

u64 HealthyLeaderCount(const AgentTenantCoordinationGroup& group) {
  u64 count = 0;
  for (const auto& member : group.members) {
    if (member.role == AgentTenantCoordinationRole::leader &&
        member.healthy &&
        MemberHeartbeatFresh(member) &&
        member.live_action_capable &&
        !member.external_cluster_member) {
      ++count;
    }
  }
  return count;
}

bool SameTenantAndScope(const AgentTenantCoordinationRequest& request) {
  return !request.tenant_budget.tenant_uuid.empty() &&
         request.tenant_budget.tenant_uuid ==
             request.coordination_group.tenant_uuid &&
         !request.coordination_group.database_uuid.empty() &&
         (request.runtime_context.database_uuid.empty() ||
          request.runtime_context.database_uuid ==
              request.coordination_group.database_uuid);
}

bool CompatibleSharedLock(const AgentTenantCoordinationRequest& request,
                          const AgentTenantCoordinationLock& active) {
  return request.lock_request.mode == AgentTenantLockMode::shared &&
         active.mode == AgentTenantLockMode::shared &&
         request.coordination_group.conflict_policy ==
             AgentTenantConflictPolicy::allow_shared_readers &&
         active.tenant_uuid == request.tenant_budget.tenant_uuid;
}

bool ActiveLockApplies(const AgentTenantCoordinationLock& lock,
                       u64 now_microseconds) {
  return !lock.released &&
         lock.mode != AgentTenantLockMode::none &&
         lock.expires_at_microseconds > now_microseconds &&
         lock.durable_lock_evidence_present &&
         !lock.lock_evidence_uuid.empty();
}

std::string CoordinationToken(const AgentTenantCoordinationRequest& request) {
  return DeterministicAgentRuntimeObjectUuidFromKey(
      "agent_tenant_coordination|" + request.coordination_group.group_id +
      "|" + request.tenant_budget.tenant_uuid + "|" +
      request.lock_request.resource_key + "|" + request.requester_instance_id +
      "|" + request.action_id);
}

std::string MetricsInputDigest(
    const std::vector<AgentTenantSharedMetricSnapshot>& snapshots) {
  std::string input = "tenant_coordination_metrics";
  for (const auto& snapshot : snapshots) {
    input.append("|")
        .append(snapshot.metric_family)
        .append("|")
        .append(snapshot.source_id)
        .append("|")
        .append(snapshot.digest)
        .append("|")
        .append(std::to_string(snapshot.generation));
  }
  return DeterministicAgentRuntimeObjectUuidFromKey(input);
}

}  // namespace

const char* AgentTenantCoordinationRoleName(
    AgentTenantCoordinationRole role) {
  switch (role) {
    case AgentTenantCoordinationRole::leader: return "leader";
    case AgentTenantCoordinationRole::follower: return "follower";
    case AgentTenantCoordinationRole::observer: return "observer";
    case AgentTenantCoordinationRole::unknown: return "unknown";
  }
  return "unknown";
}

const char* AgentTenantLockModeName(AgentTenantLockMode mode) {
  switch (mode) {
    case AgentTenantLockMode::shared: return "shared";
    case AgentTenantLockMode::exclusive: return "exclusive";
    case AgentTenantLockMode::none: return "none";
  }
  return "none";
}

const char* AgentTenantConflictPolicyName(
    AgentTenantConflictPolicy policy) {
  switch (policy) {
    case AgentTenantConflictPolicy::fail_closed: return "fail_closed";
    case AgentTenantConflictPolicy::queue_if_safe: return "queue_if_safe";
    case AgentTenantConflictPolicy::allow_shared_readers:
      return "allow_shared_readers";
  }
  return "fail_closed";
}

const char* AgentTenantCoordinationDecisionKindName(
    AgentTenantCoordinationDecisionKind decision) {
  switch (decision) {
    case AgentTenantCoordinationDecisionKind::admitted: return "admitted";
    case AgentTenantCoordinationDecisionKind::queued: return "queued";
    case AgentTenantCoordinationDecisionKind::refused: return "refused";
  }
  return "refused";
}

AgentTenantCoordinationDecision EvaluateAgentTenantWorkloadCoordination(
    const AgentTenantCoordinationRequest& request) {
  AgentTenantCoordinationDecision result;
  result.evidence_fields = BaseEvidence(request);
  result.evidence_fields.push_back(
      "conflict_policy=" +
      std::string(AgentTenantConflictPolicyName(
          request.coordination_group.conflict_policy)));
  result.evidence_fields.push_back(
      "lock_mode=" +
      std::string(AgentTenantLockModeName(request.lock_request.mode)));

  const auto type_status = ValidateAgentType(request.descriptor);
  if (!type_status.ok) {
    return Refuse(std::move(result), type_status.diagnostic_code,
                  type_status.detail);
  }
  const auto policy_status =
      ValidateAgentPolicy(request.policy, request.descriptor);
  if (!policy_status.ok) {
    return Refuse(std::move(result), policy_status.diagnostic_code,
                  policy_status.detail);
  }

  result.authority_clean = AuthorityClean(request.no_authority);
  if (!result.authority_clean) {
    return Refuse(std::move(result),
                  "SB_AGENT_TENANT_COORDINATION.FORBIDDEN_AUTHORITY",
                  request.descriptor.type_id);
  }

  if (request.coordination_group.local_cluster_claim ||
      !request.coordination_group.local_noncluster_group) {
    if (!request.coordination_group.external_cluster_provider_proof_present ||
        request.coordination_group.external_cluster_provider_id.empty() ||
        request.coordination_group.external_cluster_provider_evidence_uuid.empty()) {
      return Refuse(
          std::move(result),
          "SB_AGENT_TENANT_COORDINATION.EXTERNAL_CLUSTER_PROVIDER_REQUIRED",
          request.coordination_group.group_id);
    }
    return Refuse(std::move(result),
                  "SB_AGENT_TENANT_COORDINATION.CLUSTER_DELEGATION_REQUIRED",
                  request.coordination_group.external_cluster_provider_id);
  }

  if (!SameTenantAndScope(request)) {
    return Refuse(std::move(result),
                  "SB_AGENT_TENANT_COORDINATION.SCOPE_MISMATCH",
                  request.coordination_group.group_id);
  }
  if (request.coordination_group.group_id.empty() ||
      request.coordination_group.group_generation == 0 ||
      request.coordination_group.group_evidence_uuid.empty()) {
    return Refuse(std::move(result),
                  "SB_AGENT_TENANT_COORDINATION.GROUP_EVIDENCE_REQUIRED",
                  request.coordination_group.group_id);
  }
  if (request.coordination_group.members.empty()) {
    return Refuse(std::move(result),
                  "SB_AGENT_TENANT_COORDINATION.GROUP_MEMBERS_REQUIRED",
                  request.coordination_group.group_id);
  }

  if (request.tenant_budget.budget_generation == 0 ||
      request.tenant_budget.budget_evidence_uuid.empty()) {
    return Refuse(std::move(result),
                  "SB_AGENT_TENANT_COORDINATION.BUDGET_EVIDENCE_REQUIRED",
                  request.tenant_budget.tenant_uuid);
  }
  if (OverLimit(request.tenant_budget.max_tenant_live_actions,
                request.tenant_budget.active_tenant_live_actions,
                IsLiveAction(request.policy, request) ? 1 : 0)) {
    if (request.coordination_group.conflict_policy ==
            AgentTenantConflictPolicy::queue_if_safe &&
        !OverLimit(request.tenant_budget.max_queue_depth,
                   request.tenant_budget.current_queue_depth, 1)) {
      result.tenant_budget_valid = true;
      return Queue(std::move(result),
                   "SB_AGENT_TENANT_COORDINATION.QUEUED_TENANT_BUDGET",
                   request.tenant_budget.tenant_uuid);
    }
    return Refuse(std::move(result),
                  "SB_AGENT_TENANT_COORDINATION.TENANT_LIVE_QUOTA_EXCEEDED",
                  request.tenant_budget.tenant_uuid);
  }
  if (OverLimit(request.tenant_budget.max_agent_live_actions,
                request.tenant_budget.active_agent_live_actions,
                IsLiveAction(request.policy, request) ? 1 : 0)) {
    return Refuse(std::move(result),
                  "SB_AGENT_TENANT_COORDINATION.AGENT_LIVE_QUOTA_EXCEEDED",
                  request.descriptor.type_id);
  }
  if (OverLimit(request.tenant_budget.max_queue_depth,
                request.tenant_budget.current_queue_depth)) {
    return Refuse(std::move(result),
                  "SB_AGENT_TENANT_COORDINATION.QUEUE_DEPTH_EXCEEDED",
                  request.tenant_budget.tenant_uuid);
  }
  if (OverLimit(request.tenant_budget.max_memory_bytes,
                request.tenant_budget.used_memory_bytes,
                request.tenant_budget.requested_memory_bytes)) {
    return Refuse(std::move(result),
                  "SB_AGENT_TENANT_COORDINATION.MEMORY_QUOTA_EXCEEDED",
                  request.tenant_budget.tenant_uuid);
  }
  if (OverLimit(request.tenant_budget.max_worker_slots,
                request.tenant_budget.used_worker_slots,
                request.tenant_budget.requested_worker_slots)) {
    return Refuse(std::move(result),
                  "SB_AGENT_TENANT_COORDINATION.WORKER_QUOTA_EXCEEDED",
                  request.tenant_budget.tenant_uuid);
  }
  if (OverLimit(request.tenant_budget.max_io_bytes,
                request.tenant_budget.used_io_bytes,
                request.tenant_budget.requested_io_bytes)) {
    return Refuse(std::move(result),
                  "SB_AGENT_TENANT_COORDINATION.IO_QUOTA_EXCEEDED",
                  request.tenant_budget.tenant_uuid);
  }
  if (request.tenant_budget.protect_foreground_work &&
      request.tenant_budget.foreground_database_work_active &&
      IsMutableAction(request)) {
    return Refuse(std::move(result),
                  "SB_AGENT_TENANT_COORDINATION.FOREGROUND_PROTECTION",
                  request.tenant_budget.tenant_uuid);
  }
  result.tenant_budget_valid = true;

  if (request.production_environment && IsLiveAction(request.policy, request) &&
      (request.live_action_evidence_uuid.empty() ||
       request.tenant_live_action_evidence_uuid.empty())) {
    return Refuse(std::move(result),
                  "SB_AGENT_TENANT_COORDINATION.LIVE_EVIDENCE_REQUIRED",
                  request.action_id);
  }

  const auto required_metrics = request.required_metric_families.empty()
                                    ? DefaultRequiredMetricFamilies()
                                    : request.required_metric_families;
  for (const auto& family : required_metrics) {
    std::set<std::string> sources;
    for (const auto& snapshot : request.shared_metrics) {
      if (snapshot.metric_family != family) { continue; }
      if (!snapshot.present || !snapshot.trusted ||
          !snapshot.attestation_verified ||
          !snapshot.redacted ||
          snapshot.protected_material_present ||
          snapshot.source_quality == AgentMetricSourceQuality::unknown ||
          snapshot.source_id.empty() ||
          snapshot.digest.empty() ||
          snapshot.schema_digest.empty() ||
          snapshot.evidence_uuid.empty() ||
          snapshot.generation == 0) {
        return Refuse(
            std::move(result),
            "SB_AGENT_TENANT_COORDINATION.METRIC_TRUST_REQUIRED",
            family);
      }
      if (!snapshot.tenant_uuid.empty() &&
          snapshot.tenant_uuid != request.tenant_budget.tenant_uuid) {
        return Refuse(
            std::move(result),
            "SB_AGENT_TENANT_COORDINATION.METRIC_SCOPE_MISMATCH",
            family);
      }
      if (!snapshot.scope_uuid.empty() &&
          snapshot.scope_uuid != request.coordination_group.database_uuid &&
          snapshot.scope_uuid != request.tenant_budget.tenant_uuid) {
        return Refuse(
            std::move(result),
            "SB_AGENT_TENANT_COORDINATION.METRIC_SCOPE_MISMATCH",
            family);
      }
      if (snapshot.observed_wall_microseconds >
          request.runtime_context.wall_now_microseconds) {
        return Refuse(std::move(result),
                      "SB_AGENT_TENANT_COORDINATION.METRIC_FROM_FUTURE",
                      family);
      }
      const auto age = request.runtime_context.wall_now_microseconds -
                       snapshot.observed_wall_microseconds;
      if (snapshot.max_freshness_microseconds != 0 &&
          age > snapshot.max_freshness_microseconds) {
        return Refuse(std::move(result),
                      "SB_AGENT_TENANT_COORDINATION.METRIC_STALE",
                      family);
      }
      if (HasForbiddenAuthorityClaim(snapshot.authority_claims)) {
        return Refuse(
            std::move(result),
            "SB_AGENT_TENANT_COORDINATION.METRIC_FORBIDDEN_AUTHORITY",
            family);
      }
      sources.insert(snapshot.source_id);
    }
    if (sources.size() < request.required_metric_quorum) {
      return Refuse(std::move(result),
                    "SB_AGENT_TENANT_COORDINATION.METRIC_QUORUM_NOT_MET",
                    family);
    }
  }
  result.shared_metrics_valid = true;

  std::set<std::string> instance_ids;
  for (const auto& member : request.coordination_group.members) {
    if (member.instance_id.empty() ||
        !instance_ids.insert(member.instance_id).second) {
      return Refuse(
          std::move(result),
          "SB_AGENT_TENANT_COORDINATION.DUPLICATE_OR_BLANK_MEMBER",
          request.coordination_group.group_id);
    }
    if (member.tenant_uuid != request.tenant_budget.tenant_uuid ||
        member.external_cluster_member) {
      return Refuse(
          std::move(result),
          "SB_AGENT_TENANT_COORDINATION.MEMBER_SCOPE_INVALID",
          member.instance_id);
    }
  }
  const auto* requester =
      FindRequester(request.coordination_group, request.requester_instance_id);
  if (requester == nullptr) {
    return Refuse(std::move(result),
                  "SB_AGENT_TENANT_COORDINATION.REQUESTER_NOT_MEMBER",
                  request.requester_instance_id);
  }
  if (!requester->healthy || !MemberHeartbeatFresh(*requester)) {
    return Refuse(std::move(result),
                  "SB_AGENT_TENANT_COORDINATION.REQUESTER_NOT_HEALTHY",
                  request.requester_instance_id);
  }
  const auto* leader = SelectLeader(request.coordination_group);
  if (leader == nullptr) {
    return Refuse(std::move(result),
                  "SB_AGENT_TENANT_COORDINATION.LEADER_REQUIRED",
                  request.coordination_group.group_id);
  }
  if (request.coordination_group.require_single_leader &&
      HealthyLeaderCount(request.coordination_group) != 1) {
    return Refuse(std::move(result),
                  "SB_AGENT_TENANT_COORDINATION.MULTIPLE_LEADERS",
                  request.coordination_group.group_id);
  }
  result.selected_leader_instance_id = leader->instance_id;
  result.leader_valid = true;
  result.requester_is_leader = requester->instance_id == leader->instance_id;
  result.requester_is_follower =
      requester->role == AgentTenantCoordinationRole::follower;
  if (request.lock_request.leader_only &&
      IsMutableAction(request) &&
      !result.requester_is_leader &&
      !request.coordination_group.allow_follower_live_actions) {
    return Refuse(
        std::move(result),
        "SB_AGENT_TENANT_COORDINATION.FOLLOWER_LIVE_ACTION_FORBIDDEN",
        request.requester_instance_id);
  }
  result.coordination_group_valid = true;

  if (IsMutableAction(request)) {
    if (request.lock_request.lock_id.empty() ||
        request.lock_request.resource_key.empty() ||
        request.lock_request.mode == AgentTenantLockMode::none ||
        request.lock_request.requested_lease_microseconds == 0) {
      return Refuse(std::move(result),
                    "SB_AGENT_TENANT_COORDINATION.LOCK_REQUIRED",
                    request.action_id);
    }
    for (const auto& active : request.active_locks) {
      if (!ActiveLockApplies(active,
                             request.runtime_context.wall_now_microseconds) ||
          active.resource_key != request.lock_request.resource_key) {
        continue;
      }
      if (active.owner_instance_id == request.requester_instance_id) {
        continue;
      }
      if (CompatibleSharedLock(request, active)) {
        continue;
      }
      if (request.coordination_group.conflict_policy ==
              AgentTenantConflictPolicy::queue_if_safe &&
          !OverLimit(request.tenant_budget.max_queue_depth,
                     request.tenant_budget.current_queue_depth, 1)) {
        return Queue(std::move(result),
                     "SB_AGENT_TENANT_COORDINATION.QUEUED_LOCK_CONFLICT",
                     active.lock_id);
      }
      return Refuse(std::move(result),
                    "SB_AGENT_TENANT_COORDINATION.LOCK_CONFLICT",
                    active.lock_id);
    }
    result.lock_acquired = true;
    result.lock_token_id = CoordinationToken(request);
  }

  result.evidence_fields.push_back("tenant_budget_valid=true");
  result.evidence_fields.push_back("coordination_group_valid=true");
  result.evidence_fields.push_back("shared_metrics_valid=true");
  result.evidence_fields.push_back("leader_valid=true");
  result.evidence_fields.push_back("requester_is_leader=" +
                                   std::string(result.requester_is_leader
                                                   ? "true"
                                                   : "false"));
  result.evidence_fields.push_back("requester_is_follower=" +
                                   std::string(result.requester_is_follower
                                                   ? "true"
                                                   : "false"));
  result.evidence_fields.push_back("lock_acquired=" +
                                   std::string(result.lock_acquired
                                                   ? "true"
                                                   : "false"));
  result.evidence_fields.push_back(
      "selected_leader_instance_id=" + result.selected_leader_instance_id);
  result.evidence_fields.push_back(
      "metrics_input_digest=" + MetricsInputDigest(request.shared_metrics));
  result.evidence_fields.push_back(
      "budget_generation=" +
      std::to_string(request.tenant_budget.budget_generation));
  result.evidence_fields.push_back(
      "group_generation=" +
      std::to_string(request.coordination_group.group_generation));
  result.evidence_fields.push_back(
      "live_action_evidence_uuid=" + request.live_action_evidence_uuid);
  result.evidence_fields.push_back(
      "tenant_live_action_evidence_uuid=" +
      request.tenant_live_action_evidence_uuid);
  result.evidence_fields.push_back("authority_clean=true");

  result.status = AgentRuntimeStatus{
      true,
      "SB_AGENT_TENANT_COORDINATION.ADMITTED",
      request.coordination_group.group_id + ":" + request.action_id};
  result.decision = AgentTenantCoordinationDecisionKind::admitted;
  result.admitted = true;
  result.fail_closed = false;
  return result;
}

}  // namespace scratchbird::core::agents

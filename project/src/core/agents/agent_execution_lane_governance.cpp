// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "agent_execution_lane_governance.hpp"

#include <algorithm>
#include <utility>

namespace scratchbird::core::agents {
namespace {

std::string BoolText(bool value) {
  return value ? "true" : "false";
}

bool AuthorityClean(const AgentSystemProfileForbiddenAuthority& authority) {
  return !authority.transaction_finality_authority &&
         !authority.visibility_authority &&
         !authority.authorization_security_authority &&
         !authority.recovery_authority &&
         !authority.parser_authority &&
         !authority.donor_authority &&
         !authority.wal_authority &&
         !authority.benchmark_authority &&
         !authority.optimizer_plan_authority &&
         !authority.index_finality_authority &&
         !authority.provider_finality_authority &&
         !authority.cluster_authority &&
         !authority.memory_authority &&
         !authority.agent_action_authority;
}

bool ProductionLivePolicy(const AgentPolicy& policy) {
  return policy.activation == AgentActivationProfile::live_action &&
         policy.allow_live_action;
}

AgentExecutionLaneDecision Refuse(AgentExecutionLaneDecision result,
                                  std::string code,
                                  std::string detail) {
  result.status = AgentError(std::move(code), std::move(detail));
  result.admitted = false;
  result.fail_closed = true;
  result.evidence_fields.push_back(
      "diagnostic_code=" + result.status.diagnostic_code);
  result.evidence_fields.push_back("diagnostic_detail=" + result.status.detail);
  return result;
}

std::vector<std::string> BaseEvidence(const AgentExecutionLaneRequest& request) {
  return {
      "source=agent_execution_lane_governance",
      "schema_version=sb.agent.execution_lane.v1",
      "lane=" +
          std::string(AgentExecutionLaneKindName(request.lane_policy.lane)),
      "lane_id=" + request.lane_policy.lane_id,
      "agent_type_id=" + request.descriptor.type_id,
      "policy_uuid=" + request.policy.policy_uuid,
      "policy_generation=" + std::to_string(request.policy.policy_generation),
      "action_id=" + request.action_id,
      "cost_center=" + request.lane_policy.cost_center,
      "transaction_finality_authority=false",
      "visibility_authority=false",
      "authorization_security_authority=false",
      "recovery_authority=false",
      "parser_authority=false",
      "donor_authority=false",
      "wal_authority=false",
      "benchmark_authority=false",
      "optimizer_plan_authority=false",
      "index_finality_authority=false",
      "provider_finality_authority=false",
      "cluster_authority=false",
      "memory_authority=false",
      "agent_action_authority=false"};
}

bool Exceeds(u64 limit, u64 value) {
  return limit != 0 && value > limit;
}

}  // namespace

const char* AgentExecutionLaneKindName(AgentExecutionLaneKind lane) {
  switch (lane) {
    case AgentExecutionLaneKind::foreground_guard: return "foreground_guard";
    case AgentExecutionLaneKind::storage_maintenance:
      return "storage_maintenance";
    case AgentExecutionLaneKind::memory_pressure: return "memory_pressure";
    case AgentExecutionLaneKind::optimizer_advisory:
      return "optimizer_advisory";
    case AgentExecutionLaneKind::index_maintenance:
      return "index_maintenance";
    case AgentExecutionLaneKind::backup_archive: return "backup_archive";
    case AgentExecutionLaneKind::security_session: return "security_session";
    case AgentExecutionLaneKind::support_observability:
      return "support_observability";
    case AgentExecutionLaneKind::low_priority_background:
      return "low_priority_background";
    case AgentExecutionLaneKind::unknown: return "unknown";
  }
  return "unknown";
}

AgentExecutionLanePolicy DefaultAgentExecutionLanePolicy(
    AgentExecutionLaneKind lane) {
  AgentExecutionLanePolicy policy;
  policy.lane = lane;
  policy.lane_id = AgentExecutionLaneKindName(lane);
  policy.enabled = lane != AgentExecutionLaneKind::unknown;
  policy.priority = 50;
  policy.max_queue_depth = 4;
  policy.max_concurrent_actions = 1;
  policy.max_schedule_latency_microseconds = 1000000;
  policy.max_action_latency_microseconds = 5000000;
  policy.max_evidence_latency_microseconds = 1000000;
  policy.max_heartbeat_latency_microseconds = 1000000;
  policy.protect_foreground_work = true;
  policy.cost_center = "agent.local.default";
  policy.chargeback_tags = {"agent", "local"};

  switch (lane) {
    case AgentExecutionLaneKind::foreground_guard:
      policy.priority = 100;
      policy.max_queue_depth = 2;
      policy.production_live_allowed = true;
      policy.cost_center = "agent.foreground_guard";
      break;
    case AgentExecutionLaneKind::storage_maintenance:
      policy.priority = 80;
      policy.max_queue_depth = 8;
      policy.max_concurrent_actions = 2;
      policy.production_live_allowed = true;
      policy.cost_center = "agent.storage";
      break;
    case AgentExecutionLaneKind::memory_pressure:
      policy.priority = 95;
      policy.max_queue_depth = 8;
      policy.production_live_allowed = true;
      policy.cost_center = "agent.memory";
      break;
    case AgentExecutionLaneKind::optimizer_advisory:
      policy.priority = 55;
      policy.production_live_allowed = false;
      policy.cost_center = "agent.optimizer";
      break;
    case AgentExecutionLaneKind::index_maintenance:
      policy.priority = 70;
      policy.max_queue_depth = 8;
      policy.max_concurrent_actions = 2;
      policy.production_live_allowed = true;
      policy.cost_center = "agent.index";
      break;
    case AgentExecutionLaneKind::backup_archive:
      policy.priority = 60;
      policy.max_queue_depth = 4;
      policy.production_live_allowed = true;
      policy.cost_center = "agent.backup_archive";
      break;
    case AgentExecutionLaneKind::security_session:
      policy.priority = 90;
      policy.max_queue_depth = 8;
      policy.production_live_allowed = true;
      policy.cost_center = "agent.security_session";
      break;
    case AgentExecutionLaneKind::support_observability:
      policy.priority = 40;
      policy.max_queue_depth = 16;
      policy.production_live_allowed = false;
      policy.cost_center = "agent.support";
      break;
    case AgentExecutionLaneKind::low_priority_background:
      policy.priority = 10;
      policy.max_queue_depth = 32;
      policy.max_concurrent_actions = 2;
      policy.production_live_allowed = false;
      policy.cost_center = "agent.background";
      break;
    case AgentExecutionLaneKind::unknown:
      policy.priority = 0;
      policy.enabled = false;
      policy.cost_center.clear();
      policy.chargeback_tags.clear();
      break;
  }
  policy.chargeback_tags.push_back(policy.lane_id);
  return policy;
}

std::vector<AgentExecutionLanePolicy> DefaultAgentExecutionLanePolicies() {
  return {
      DefaultAgentExecutionLanePolicy(AgentExecutionLaneKind::foreground_guard),
      DefaultAgentExecutionLanePolicy(
          AgentExecutionLaneKind::storage_maintenance),
      DefaultAgentExecutionLanePolicy(AgentExecutionLaneKind::memory_pressure),
      DefaultAgentExecutionLanePolicy(
          AgentExecutionLaneKind::optimizer_advisory),
      DefaultAgentExecutionLanePolicy(AgentExecutionLaneKind::index_maintenance),
      DefaultAgentExecutionLanePolicy(AgentExecutionLaneKind::backup_archive),
      DefaultAgentExecutionLanePolicy(AgentExecutionLaneKind::security_session),
      DefaultAgentExecutionLanePolicy(
          AgentExecutionLaneKind::support_observability),
      DefaultAgentExecutionLanePolicy(
          AgentExecutionLaneKind::low_priority_background)};
}

AgentExecutionLaneDecision EvaluateAgentExecutionLaneAdmission(
    const AgentExecutionLaneRequest& request) {
  AgentExecutionLaneDecision result;
  result.evidence_fields = BaseEvidence(request);

  if (request.lane_policy.lane == AgentExecutionLaneKind::unknown ||
      request.lane_policy.lane_id.empty()) {
    return Refuse(std::move(result),
                  "SB_AGENT_EXECUTION_LANE.UNKNOWN_LANE",
                  request.descriptor.type_id);
  }
  if (!request.lane_policy.enabled) {
    return Refuse(std::move(result),
                  "SB_AGENT_EXECUTION_LANE.DISABLED",
                  request.lane_policy.lane_id);
  }
  result.lane_valid = true;

  const auto type_status = ValidateAgentType(request.descriptor);
  if (!type_status.ok) {
    return Refuse(std::move(result),
                  type_status.diagnostic_code,
                  type_status.detail);
  }
  const auto policy_status =
      ValidateAgentPolicy(request.policy, request.descriptor);
  if (!policy_status.ok) {
    return Refuse(std::move(result),
                  policy_status.diagnostic_code,
                  policy_status.detail);
  }

  result.authority_clean = AuthorityClean(request.no_authority);
  if (!result.authority_clean) {
    return Refuse(std::move(result),
                  "SB_AGENT_EXECUTION_LANE.FORBIDDEN_AUTHORITY",
                  request.descriptor.type_id);
  }
  if (request.lane_policy.local_cluster_lane_claim &&
      !request.lane_policy.external_cluster_provider_proof_present) {
    return Refuse(std::move(result),
                  "SB_AGENT_EXECUTION_LANE.LOCAL_CLUSTER_FORBIDDEN",
                  request.lane_policy.lane_id);
  }

  if (request.lane_policy.cost_center.empty()) {
    return Refuse(std::move(result),
                  "SB_AGENT_EXECUTION_LANE.COST_CENTER_REQUIRED",
                  request.lane_policy.lane_id);
  }
  if (request.lane_policy.chargeback_tags.empty()) {
    return Refuse(std::move(result),
                  "SB_AGENT_EXECUTION_LANE.CHARGEBACK_TAG_REQUIRED",
                  request.lane_policy.lane_id);
  }
  result.cost_governance_valid = true;

  if (Exceeds(request.lane_policy.max_queue_depth,
              request.lane_policy.current_queue_depth)) {
    return Refuse(std::move(result),
                  "SB_AGENT_EXECUTION_LANE.QUEUE_DEPTH_EXCEEDED",
                  request.lane_policy.lane_id);
  }
  if (Exceeds(request.lane_policy.max_concurrent_actions,
              request.lane_policy.active_actions)) {
    return Refuse(std::move(result),
                  "SB_AGENT_EXECUTION_LANE.CONCURRENCY_EXCEEDED",
                  request.lane_policy.lane_id);
  }
  if (Exceeds(request.lane_policy.max_schedule_latency_microseconds,
              request.lane_policy.schedule_latency_microseconds)) {
    return Refuse(std::move(result),
                  "SB_AGENT_EXECUTION_LANE.SCHEDULE_SLO_EXCEEDED",
                  request.lane_policy.lane_id);
  }
  if (Exceeds(request.lane_policy.max_action_latency_microseconds,
              request.lane_policy.action_latency_microseconds)) {
    return Refuse(std::move(result),
                  "SB_AGENT_EXECUTION_LANE.ACTION_SLO_EXCEEDED",
                  request.lane_policy.lane_id);
  }
  if (Exceeds(request.lane_policy.max_evidence_latency_microseconds,
              request.lane_policy.evidence_latency_microseconds)) {
    return Refuse(std::move(result),
                  "SB_AGENT_EXECUTION_LANE.EVIDENCE_SLO_EXCEEDED",
                  request.lane_policy.lane_id);
  }
  if (Exceeds(request.lane_policy.max_heartbeat_latency_microseconds,
              request.lane_policy.heartbeat_latency_microseconds)) {
    return Refuse(std::move(result),
                  "SB_AGENT_EXECUTION_LANE.HEARTBEAT_SLO_EXCEEDED",
                  request.lane_policy.lane_id);
  }
  result.slo_valid = true;

  if (ProductionLivePolicy(request.policy) &&
      !request.lane_policy.production_live_allowed) {
    return Refuse(std::move(result),
                  "SB_AGENT_EXECUTION_LANE.LIVE_ACTION_FORBIDDEN",
                  request.lane_policy.lane_id);
  }

  if (request.production_environment &&
      ProductionLivePolicy(request.policy)) {
    auto profile_context = request.profile_context;
    if (profile_context.now_microseconds == 0) {
      profile_context.now_microseconds =
          request.runtime_context.monotonic_now_microseconds;
    }
    const auto profile =
        ValidateAgentSystemProfileClaim(request.system_profile, profile_context);
    if (!profile.status.ok) {
      return Refuse(std::move(result),
                    "SB_AGENT_EXECUTION_LANE.PROFILE_INVALID",
                    profile.status.diagnostic_code + ":" +
                        profile.status.detail);
    }
    if (!profile.production_live_claim ||
        request.system_profile.metric_strictness !=
            AgentSystemProfileMetricStrictness::strict) {
      return Refuse(std::move(result),
                    "SB_AGENT_EXECUTION_LANE.STRICT_PROFILE_REQUIRED",
                    request.system_profile.agent_type_id);
    }
  }
  result.profile_valid = true;

  if (!request.resource_budget_evidence_present) {
    return Refuse(std::move(result),
                  "SB_AGENT_EXECUTION_LANE.RESOURCE_BUDGET_REQUIRED",
                  request.lane_policy.lane_id);
  }
  auto budget_input = request.resource_budget;
  budget_input.foreground_database_work_active =
      budget_input.foreground_database_work_active ||
      request.lane_policy.foreground_database_work_active;
  budget_input.budget.protect_foreground_work =
      budget_input.budget.protect_foreground_work ||
      request.lane_policy.protect_foreground_work;
  result.resource_decision = EvaluateAgentResourceBudget(
      request.descriptor, request.policy, request.runtime_context,
      budget_input);
  if (result.resource_decision.decision !=
      AgentResourceBudgetDecisionKind::allow) {
    return Refuse(std::move(result),
                  "SB_AGENT_EXECUTION_LANE.RESOURCE_BUDGET_REFUSED",
                  result.resource_decision.status.diagnostic_code + ":" +
                      result.resource_decision.status.detail);
  }
  result.resource_budget_valid = true;

  result.evidence_fields.push_back("lane_valid=true");
  result.evidence_fields.push_back("slo_valid=true");
  result.evidence_fields.push_back("cost_governance_valid=true");
  result.evidence_fields.push_back("resource_budget_valid=true");
  result.evidence_fields.push_back("profile_valid=" +
                                   BoolText(result.profile_valid));
  result.evidence_fields.push_back("authority_clean=true");
  result.evidence_fields.push_back("priority=" +
                                   std::to_string(request.lane_policy.priority));
  result.evidence_fields.push_back(
      "max_queue_depth=" +
      std::to_string(request.lane_policy.max_queue_depth));
  result.evidence_fields.push_back(
      "max_concurrent_actions=" +
      std::to_string(request.lane_policy.max_concurrent_actions));
  result.evidence_fields.push_back(
      "schedule_latency_us=" +
      std::to_string(request.lane_policy.schedule_latency_microseconds));
  result.evidence_fields.push_back(
      "action_latency_us=" +
      std::to_string(request.lane_policy.action_latency_microseconds));
  result.evidence_fields.push_back(
      "evidence_latency_us=" +
      std::to_string(request.lane_policy.evidence_latency_microseconds));
  result.evidence_fields.push_back(
      "heartbeat_latency_us=" +
      std::to_string(request.lane_policy.heartbeat_latency_microseconds));
  result.evidence_fields.push_back(
      "resource_budget_decision=" +
      std::string(AgentResourceBudgetDecisionKindName(
          result.resource_decision.decision)));
  for (const auto& tag : request.lane_policy.chargeback_tags) {
    result.evidence_fields.push_back("chargeback_tag=" + tag);
  }

  result.status = AgentRuntimeStatus{
      true,
      "SB_AGENT_EXECUTION_LANE.ADMITTED",
      request.lane_policy.lane_id + ":" + request.descriptor.type_id};
  result.admitted = true;
  result.fail_closed = false;
  return result;
}

}  // namespace scratchbird::core::agents

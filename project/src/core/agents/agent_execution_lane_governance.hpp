// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// SEARCH_KEY: CEIC_073_AGENT_EXECUTION_LANE_GOVERNANCE
//
// Enterprise execution lane, SLO, and cost-governance admission for
// noncluster operational agents. Lane evidence is an admission/control-plane
// record only; it is not transaction finality, visibility,
// authorization/security, recovery, parser execution, donor behavior, WAL
// recovery, benchmark truth, optimizer plan authority, index finality,
// provider finality, cluster authority, memory authority, or agent action
// authority.

#include "agent_policy_lifecycle.hpp"

#include <string>
#include <vector>

namespace scratchbird::core::agents {

enum class AgentExecutionLaneKind {
  unknown,
  foreground_guard,
  storage_maintenance,
  memory_pressure,
  optimizer_advisory,
  index_maintenance,
  backup_archive,
  security_session,
  support_observability,
  low_priority_background
};

struct AgentExecutionLanePolicy {
  AgentExecutionLaneKind lane = AgentExecutionLaneKind::unknown;
  std::string lane_id;
  bool enabled = true;
  u64 priority = 0;
  u64 max_queue_depth = 0;
  u64 current_queue_depth = 0;
  u64 max_concurrent_actions = 0;
  u64 active_actions = 0;
  u64 max_schedule_latency_microseconds = 0;
  u64 max_action_latency_microseconds = 0;
  u64 max_evidence_latency_microseconds = 0;
  u64 max_heartbeat_latency_microseconds = 0;
  u64 schedule_latency_microseconds = 0;
  u64 action_latency_microseconds = 0;
  u64 evidence_latency_microseconds = 0;
  u64 heartbeat_latency_microseconds = 0;
  bool protect_foreground_work = true;
  bool foreground_database_work_active = false;
  bool production_live_allowed = false;
  bool local_cluster_lane_claim = false;
  bool external_cluster_provider_proof_present = false;
  std::string cost_center;
  std::vector<std::string> chargeback_tags;
};

struct AgentExecutionLaneRequest {
  std::string action_id;
  AgentTypeDescriptor descriptor;
  AgentPolicy policy;
  AgentRuntimeContext runtime_context;
  AgentSystemProfile system_profile;
  AgentPolicyLifecycleValidationContext lifecycle_context;
  AgentSystemProfileValidationContext profile_context;
  AgentExecutionLanePolicy lane_policy;
  AgentResourceBudgetEvaluationInput resource_budget;
  bool resource_budget_evidence_present = false;
  bool production_environment = false;
  AgentSystemProfileForbiddenAuthority no_authority;
};

struct AgentExecutionLaneDecision {
  AgentRuntimeStatus status;
  bool admitted = false;
  bool fail_closed = true;
  bool lane_valid = false;
  bool slo_valid = false;
  bool cost_governance_valid = false;
  bool resource_budget_valid = false;
  bool profile_valid = false;
  bool authority_clean = false;
  AgentResourceBudgetDecision resource_decision;
  std::vector<std::string> evidence_fields;
};

const char* AgentExecutionLaneKindName(AgentExecutionLaneKind lane);
std::vector<AgentExecutionLanePolicy> DefaultAgentExecutionLanePolicies();
AgentExecutionLanePolicy DefaultAgentExecutionLanePolicy(
    AgentExecutionLaneKind lane);

AgentExecutionLaneDecision EvaluateAgentExecutionLaneAdmission(
    const AgentExecutionLaneRequest& request);

}  // namespace scratchbird::core::agents

// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// SEARCH_KEY: CEIC_079_AGENT_TENANT_COORDINATION
//
// Local noncluster tenant workload isolation and agent coordination admission.
// This evidence is operational admission evidence only; it is not transaction
// finality, visibility, authorization/security, recovery, parser execution,
// donor behavior, WAL recovery, benchmark truth, optimizer plan authority,
// index finality, provider finality, cluster authority, memory authority, or
// agent action authority.

#include "agent_metric_runtime.hpp"
#include "agent_system_profile.hpp"

#include <string>
#include <vector>

namespace scratchbird::core::agents {

enum class AgentTenantCoordinationRole {
  unknown,
  leader,
  follower,
  observer
};

enum class AgentTenantLockMode {
  none,
  shared,
  exclusive
};

enum class AgentTenantConflictPolicy {
  fail_closed,
  queue_if_safe,
  allow_shared_readers
};

enum class AgentTenantCoordinationDecisionKind {
  admitted,
  queued,
  refused
};

struct AgentTenantWorkloadBudget {
  std::string tenant_uuid;
  std::string budget_evidence_uuid;
  u64 budget_generation = 0;

  u64 max_tenant_live_actions = 0;
  u64 active_tenant_live_actions = 0;
  u64 max_agent_live_actions = 0;
  u64 active_agent_live_actions = 0;
  u64 max_queue_depth = 0;
  u64 current_queue_depth = 0;
  u64 max_memory_bytes = 0;
  u64 used_memory_bytes = 0;
  u64 requested_memory_bytes = 0;
  u64 max_worker_slots = 0;
  u64 used_worker_slots = 0;
  u64 requested_worker_slots = 0;
  u64 max_io_bytes = 0;
  u64 used_io_bytes = 0;
  u64 requested_io_bytes = 0;

  bool protect_foreground_work = true;
  bool foreground_database_work_active = false;
};

struct AgentTenantCoordinationMember {
  std::string instance_id;
  std::string agent_type_id;
  std::string tenant_uuid;
  AgentTenantCoordinationRole role = AgentTenantCoordinationRole::unknown;
  bool healthy = false;
  bool heartbeat_fresh = false;
  bool live_action_capable = false;
  bool external_cluster_member = false;
  u64 election_term = 0;
  u64 priority = 0;
  u64 heartbeat_age_microseconds = 0;
  u64 max_heartbeat_age_microseconds = 0;
};

struct AgentTenantCoordinationGroup {
  std::string group_id;
  std::string tenant_uuid;
  std::string database_uuid;
  std::string group_evidence_uuid;
  u64 group_generation = 0;
  AgentTenantConflictPolicy conflict_policy =
      AgentTenantConflictPolicy::fail_closed;
  bool local_noncluster_group = true;
  bool local_cluster_claim = false;
  bool external_cluster_provider_proof_present = false;
  std::string external_cluster_provider_id;
  std::string external_cluster_provider_evidence_uuid;
  bool require_single_leader = true;
  bool allow_follower_live_actions = false;
  std::vector<AgentTenantCoordinationMember> members;
};

struct AgentTenantCoordinationLock {
  std::string lock_id;
  std::string resource_key;
  std::string tenant_uuid;
  std::string owner_instance_id;
  AgentTenantLockMode mode = AgentTenantLockMode::none;
  u64 lease_generation = 0;
  u64 expires_at_microseconds = 0;
  bool durable_lock_evidence_present = false;
  std::string lock_evidence_uuid;
  bool released = false;
};

struct AgentTenantCoordinationLockRequest {
  std::string lock_id;
  std::string resource_key;
  AgentTenantLockMode mode = AgentTenantLockMode::none;
  u64 requested_lease_microseconds = 0;
  bool leader_only = true;
  bool mutates_resource = true;
};

struct AgentTenantSharedMetricSnapshot {
  std::string metric_family;
  std::string tenant_uuid;
  std::string scope_uuid;
  std::string source_id;
  std::string digest;
  std::string schema_digest;
  std::string evidence_uuid;
  u64 generation = 0;
  u64 observed_wall_microseconds = 0;
  u64 max_freshness_microseconds = 0;
  AgentMetricSourceQuality source_quality = AgentMetricSourceQuality::unknown;
  bool present = true;
  bool trusted = false;
  bool attestation_verified = false;
  bool redacted = false;
  bool protected_material_present = false;
  std::vector<std::string> authority_claims;
};

struct AgentTenantCoordinationRequest {
  AgentTypeDescriptor descriptor;
  AgentPolicy policy;
  AgentRuntimeContext runtime_context;
  AgentSystemProfileForbiddenAuthority no_authority;
  std::string action_id;
  std::string requester_instance_id;
  bool production_environment = false;
  bool live_action_requested = false;
  bool mutable_action_requested = false;
  std::string live_action_evidence_uuid;
  std::string tenant_live_action_evidence_uuid;
  AgentTenantWorkloadBudget tenant_budget;
  AgentTenantCoordinationGroup coordination_group;
  AgentTenantCoordinationLockRequest lock_request;
  std::vector<AgentTenantCoordinationLock> active_locks;
  std::vector<AgentTenantSharedMetricSnapshot> shared_metrics;
  std::vector<std::string> required_metric_families;
  u64 required_metric_quorum = 2;
};

struct AgentTenantCoordinationDecision {
  AgentRuntimeStatus status;
  AgentTenantCoordinationDecisionKind decision =
      AgentTenantCoordinationDecisionKind::refused;
  bool admitted = false;
  bool queued = false;
  bool fail_closed = true;
  bool tenant_budget_valid = false;
  bool coordination_group_valid = false;
  bool shared_metrics_valid = false;
  bool leader_valid = false;
  bool requester_is_leader = false;
  bool requester_is_follower = false;
  bool lock_acquired = false;
  bool authority_clean = false;
  std::string selected_leader_instance_id;
  std::string lock_token_id;
  std::vector<std::string> evidence_fields;
};

const char* AgentTenantCoordinationRoleName(
    AgentTenantCoordinationRole role);
const char* AgentTenantLockModeName(AgentTenantLockMode mode);
const char* AgentTenantConflictPolicyName(
    AgentTenantConflictPolicy policy);
const char* AgentTenantCoordinationDecisionKindName(
    AgentTenantCoordinationDecisionKind decision);

AgentTenantCoordinationDecision EvaluateAgentTenantWorkloadCoordination(
    const AgentTenantCoordinationRequest& request);

}  // namespace scratchbird::core::agents

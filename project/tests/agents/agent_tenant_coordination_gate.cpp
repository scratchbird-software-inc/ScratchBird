// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "agent_tenant_coordination.hpp"

#include <cstdlib>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

namespace {

namespace agents = scratchbird::core::agents;

constexpr agents::u64 kNowMicros = 4100000000ull;
constexpr const char* kTenantUuid = "018f2000-0000-7000-8000-000000079001";
constexpr const char* kDatabaseUuid = "018f2000-0000-7000-8000-000000079002";
constexpr const char* kLeaderInstance = "018f2000-0000-7000-8000-000000079003";
constexpr const char* kFollowerInstance = "018f2000-0000-7000-8000-000000079004";

[[noreturn]] void Fail(const std::string& message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, const std::string& message) {
  if (!condition) { Fail(message); }
}

const agents::AgentTypeDescriptor& Descriptor(const std::string& id) {
  const auto descriptor = agents::FindAgentType(id);
  Require(descriptor.has_value(), "missing descriptor: " + id);
  static agents::AgentTypeDescriptor storage;
  storage = *descriptor;
  return storage;
}

agents::AgentPolicy PolicyFor(const std::string& agent_type_id,
                              const std::string& family,
                              bool live) {
  auto policy = agents::BaselinePolicyForAgentFamily(
      Descriptor(agent_type_id), family, 79);
  if (live) {
    policy.activation = agents::AgentActivationProfile::live_action;
    policy.allow_live_action = true;
    policy.action_mode = "request_action";
    policy.require_manual_approval = true;
    policy.require_dry_run_before_live = true;
  } else {
    policy.activation = agents::AgentActivationProfile::recommend_only;
    policy.allow_live_action = false;
    policy.action_mode = "recommend_only";
  }
  return policy;
}

agents::AgentRuntimeContext RuntimeContext() {
  agents::AgentRuntimeContext context;
  context.security_context_present = true;
  context.private_features_available = true;
  context.standalone_edition = true;
  context.database_uuid = kDatabaseUuid;
  context.principal_uuid = "018f2000-0000-7000-8000-000000079005";
  context.wall_now_microseconds = kNowMicros;
  context.monotonic_now_microseconds = kNowMicros;
  return context;
}

agents::AgentTenantWorkloadBudget TenantBudget() {
  agents::AgentTenantWorkloadBudget budget;
  budget.tenant_uuid = kTenantUuid;
  budget.budget_evidence_uuid = "018f2000-0000-7000-8000-000000079006";
  budget.budget_generation = 79;
  budget.max_tenant_live_actions = 4;
  budget.active_tenant_live_actions = 1;
  budget.max_agent_live_actions = 2;
  budget.active_agent_live_actions = 0;
  budget.max_queue_depth = 8;
  budget.current_queue_depth = 1;
  budget.max_memory_bytes = 64ull * 1024ull * 1024ull;
  budget.used_memory_bytes = 4ull * 1024ull * 1024ull;
  budget.requested_memory_bytes = 2ull * 1024ull * 1024ull;
  budget.max_worker_slots = 4;
  budget.used_worker_slots = 1;
  budget.requested_worker_slots = 1;
  budget.max_io_bytes = 128ull * 1024ull * 1024ull;
  budget.used_io_bytes = 8ull * 1024ull * 1024ull;
  budget.requested_io_bytes = 1ull * 1024ull * 1024ull;
  budget.protect_foreground_work = true;
  return budget;
}

agents::AgentTenantCoordinationMember Member(
    std::string instance_id,
    agents::AgentTenantCoordinationRole role,
    agents::u64 election_term,
    agents::u64 priority) {
  agents::AgentTenantCoordinationMember member;
  member.instance_id = std::move(instance_id);
  member.agent_type_id = "page_allocation_manager";
  member.tenant_uuid = kTenantUuid;
  member.role = role;
  member.healthy = true;
  member.heartbeat_fresh = true;
  member.live_action_capable = role != agents::AgentTenantCoordinationRole::observer;
  member.election_term = election_term;
  member.priority = priority;
  member.heartbeat_age_microseconds = 100;
  member.max_heartbeat_age_microseconds = 1000;
  return member;
}

agents::AgentTenantCoordinationGroup Group(
    agents::AgentTenantConflictPolicy conflict_policy =
        agents::AgentTenantConflictPolicy::fail_closed) {
  agents::AgentTenantCoordinationGroup group;
  group.group_id = "tenant-agent-group-a";
  group.tenant_uuid = kTenantUuid;
  group.database_uuid = kDatabaseUuid;
  group.group_evidence_uuid = "018f2000-0000-7000-8000-000000079007";
  group.group_generation = 79;
  group.conflict_policy = conflict_policy;
  group.local_noncluster_group = true;
  group.local_cluster_claim = false;
  group.require_single_leader = true;
  group.allow_follower_live_actions = false;
  group.members.push_back(Member(kLeaderInstance,
                                 agents::AgentTenantCoordinationRole::leader,
                                 9, 100));
  group.members.push_back(Member(kFollowerInstance,
                                 agents::AgentTenantCoordinationRole::follower,
                                 9, 10));
  return group;
}

agents::AgentTenantSharedMetricSnapshot Metric(
    std::string family,
    std::string source_id) {
  agents::AgentTenantSharedMetricSnapshot snapshot;
  snapshot.metric_family = std::move(family);
  snapshot.tenant_uuid = kTenantUuid;
  snapshot.scope_uuid = kDatabaseUuid;
  snapshot.source_id = std::move(source_id);
  snapshot.digest = "sha256:tenant-coordination:" + snapshot.metric_family +
                    ":" + snapshot.source_id;
  snapshot.schema_digest = "sha256:schema:" + snapshot.metric_family;
  snapshot.evidence_uuid = agents::DeterministicAgentRuntimeObjectUuidFromKey(
      "ceic079|" + snapshot.metric_family + "|" + snapshot.source_id);
  snapshot.generation = 79;
  snapshot.observed_wall_microseconds = kNowMicros - 100;
  snapshot.max_freshness_microseconds = 1000;
  snapshot.source_quality = agents::AgentMetricSourceQuality::trusted;
  snapshot.present = true;
  snapshot.trusted = true;
  snapshot.attestation_verified = true;
  snapshot.redacted = true;
  snapshot.protected_material_present = false;
  snapshot.authority_claims = {"metric_evidence"};
  return snapshot;
}

std::vector<agents::AgentTenantSharedMetricSnapshot> SharedMetrics() {
  std::vector<agents::AgentTenantSharedMetricSnapshot> metrics;
  for (const std::string family : {"tenant.workload.quota",
                                   "agent.coordination.group",
                                   "agent.coordination.lock"}) {
    metrics.push_back(Metric(family, "source-a"));
    metrics.push_back(Metric(family, "source-b"));
  }
  return metrics;
}

agents::AgentTenantCoordinationRequest Request(
    bool live = true,
    std::string requester = kLeaderInstance,
    agents::AgentTenantConflictPolicy conflict_policy =
        agents::AgentTenantConflictPolicy::fail_closed) {
  agents::AgentTenantCoordinationRequest request;
  request.descriptor = Descriptor("page_allocation_manager");
  request.policy = PolicyFor("page_allocation_manager",
                             "page_preallocation_policy", live);
  request.runtime_context = RuntimeContext();
  request.action_id = live ? "preallocate_page_family" : "observe";
  request.requester_instance_id = std::move(requester);
  request.production_environment = true;
  request.live_action_requested = live;
  request.mutable_action_requested = live;
  request.live_action_evidence_uuid =
      "018f2000-0000-7000-8000-000000079008";
  request.tenant_live_action_evidence_uuid =
      "018f2000-0000-7000-8000-000000079009";
  request.tenant_budget = TenantBudget();
  request.coordination_group = Group(conflict_policy);
  request.lock_request.lock_id = "page-family-pool-lock";
  request.lock_request.resource_key = "filespace:default:page-family:table";
  request.lock_request.mode = agents::AgentTenantLockMode::exclusive;
  request.lock_request.requested_lease_microseconds = 5000;
  request.lock_request.leader_only = true;
  request.lock_request.mutates_resource = live;
  request.shared_metrics = SharedMetrics();
  return request;
}

agents::AgentTenantCoordinationLock ActiveLock(
    std::string owner,
    agents::AgentTenantLockMode mode =
        agents::AgentTenantLockMode::exclusive) {
  agents::AgentTenantCoordinationLock lock;
  lock.lock_id = "active-lock";
  lock.resource_key = "filespace:default:page-family:table";
  lock.tenant_uuid = kTenantUuid;
  lock.owner_instance_id = std::move(owner);
  lock.mode = mode;
  lock.lease_generation = 4;
  lock.expires_at_microseconds = kNowMicros + 5000;
  lock.durable_lock_evidence_present = true;
  lock.lock_evidence_uuid = "018f2000-0000-7000-8000-000000079010";
  return lock;
}

void RequireOk(const agents::AgentTenantCoordinationDecision& decision,
               const std::string& label) {
  Require(decision.status.ok,
          label + " failed: " + decision.status.diagnostic_code + " " +
              decision.status.detail);
  Require(decision.admitted, label + " was not admitted");
  Require(!decision.fail_closed, label + " failed closed");
  Require(decision.tenant_budget_valid, label + " budget invalid");
  Require(decision.coordination_group_valid, label + " group invalid");
  Require(decision.shared_metrics_valid, label + " metrics invalid");
  Require(decision.leader_valid, label + " leader invalid");
  Require(decision.authority_clean, label + " authority invalid");
  Require(decision.selected_leader_instance_id == kLeaderInstance,
          label + " selected wrong leader");
}

void RequireCode(const agents::AgentTenantCoordinationRequest& request,
                 const std::string& code,
                 const std::string& label) {
  const auto decision =
      agents::EvaluateAgentTenantWorkloadCoordination(request);
  Require(!decision.status.ok, label + " unexpectedly passed");
  Require(decision.fail_closed, label + " did not fail closed");
  Require(decision.status.diagnostic_code == code,
          label + " expected " + code + " got " +
              decision.status.diagnostic_code + " " + decision.status.detail);
}

void TestPositiveLeaderLiveAction() {
  const auto decision =
      agents::EvaluateAgentTenantWorkloadCoordination(Request());
  RequireOk(decision, "leader live action");
  Require(decision.requester_is_leader, "leader request did not record leader");
  Require(decision.lock_acquired, "leader live action did not acquire lock");
  Require(!decision.lock_token_id.empty(), "lock token missing");
}

void TestFollowerObserveOnly() {
  auto request = Request(false, kFollowerInstance);
  request.lock_request.mode = agents::AgentTenantLockMode::none;
  request.lock_request.resource_key.clear();
  request.lock_request.lock_id.clear();
  request.lock_request.requested_lease_microseconds = 0;
  const auto decision =
      agents::EvaluateAgentTenantWorkloadCoordination(request);
  RequireOk(decision, "follower observe");
  Require(decision.requester_is_follower,
          "follower observe did not record follower");
  Require(!decision.lock_acquired, "follower observe acquired a lock");
}

void TestBudgetQueueAndQuotaRefusals() {
  auto queued = Request(true, kLeaderInstance,
                        agents::AgentTenantConflictPolicy::queue_if_safe);
  queued.tenant_budget.active_tenant_live_actions =
      queued.tenant_budget.max_tenant_live_actions;
  const auto decision =
      agents::EvaluateAgentTenantWorkloadCoordination(queued);
  Require(decision.status.ok, "queue budget did not return ok status");
  Require(decision.queued, "budget overage did not queue");
  Require(decision.status.diagnostic_code ==
              "SB_AGENT_TENANT_COORDINATION.QUEUED_TENANT_BUDGET",
          "budget queue diagnostic mismatch");

  auto memory = Request();
  memory.tenant_budget.used_memory_bytes = memory.tenant_budget.max_memory_bytes;
  RequireCode(memory, "SB_AGENT_TENANT_COORDINATION.MEMORY_QUOTA_EXCEEDED",
              "memory quota");
}

void TestMetricRefusals() {
  auto stale = Request();
  stale.shared_metrics.front().observed_wall_microseconds =
      kNowMicros - stale.shared_metrics.front().max_freshness_microseconds - 1;
  RequireCode(stale, "SB_AGENT_TENANT_COORDINATION.METRIC_STALE",
              "stale metric");

  auto untrusted = Request();
  untrusted.shared_metrics.front().trusted = false;
  RequireCode(untrusted, "SB_AGENT_TENANT_COORDINATION.METRIC_TRUST_REQUIRED",
              "untrusted metric");

  auto authority = Request();
  authority.shared_metrics.front().authority_claims.push_back(
      "transaction_finality");
  RequireCode(authority,
              "SB_AGENT_TENANT_COORDINATION.METRIC_FORBIDDEN_AUTHORITY",
              "metric authority drift");

  auto quorum = Request();
  quorum.shared_metrics.erase(quorum.shared_metrics.begin() + 1);
  RequireCode(quorum, "SB_AGENT_TENANT_COORDINATION.METRIC_QUORUM_NOT_MET",
              "metric quorum");
}

void TestLeadershipAndLockRefusals() {
  auto follower_live = Request(true, kFollowerInstance);
  RequireCode(follower_live,
              "SB_AGENT_TENANT_COORDINATION.FOLLOWER_LIVE_ACTION_FORBIDDEN",
              "follower live action");

  auto split_brain = Request();
  split_brain.coordination_group.members.push_back(
      Member("018f2000-0000-7000-8000-000000079011",
             agents::AgentTenantCoordinationRole::leader, 9, 90));
  RequireCode(split_brain, "SB_AGENT_TENANT_COORDINATION.MULTIPLE_LEADERS",
              "multiple leaders");

  auto conflict = Request();
  conflict.active_locks.push_back(ActiveLock(kFollowerInstance));
  RequireCode(conflict, "SB_AGENT_TENANT_COORDINATION.LOCK_CONFLICT",
              "lock conflict");

  auto queued = Request(true, kLeaderInstance,
                        agents::AgentTenantConflictPolicy::queue_if_safe);
  queued.active_locks.push_back(ActiveLock(kFollowerInstance));
  const auto queued_decision =
      agents::EvaluateAgentTenantWorkloadCoordination(queued);
  Require(queued_decision.status.ok, "queued conflict did not return ok");
  Require(queued_decision.queued, "lock conflict did not queue");
  Require(queued_decision.status.diagnostic_code ==
              "SB_AGENT_TENANT_COORDINATION.QUEUED_LOCK_CONFLICT",
          "lock queue diagnostic mismatch");

  auto shared = Request(true, kLeaderInstance,
                        agents::AgentTenantConflictPolicy::allow_shared_readers);
  shared.lock_request.mode = agents::AgentTenantLockMode::shared;
  shared.active_locks.push_back(ActiveLock(kFollowerInstance,
                                           agents::AgentTenantLockMode::shared));
  RequireOk(agents::EvaluateAgentTenantWorkloadCoordination(shared),
            "shared reader lock");
}

void TestClusterAndAuthorityRefusals() {
  auto cluster_missing = Request();
  cluster_missing.coordination_group.local_cluster_claim = true;
  RequireCode(cluster_missing,
              "SB_AGENT_TENANT_COORDINATION.EXTERNAL_CLUSTER_PROVIDER_REQUIRED",
              "missing external provider proof");

  auto cluster_delegated = Request();
  cluster_delegated.coordination_group.local_cluster_claim = true;
  cluster_delegated.coordination_group.external_cluster_provider_proof_present =
      true;
  cluster_delegated.coordination_group.external_cluster_provider_id =
      "external-cluster-provider";
  cluster_delegated.coordination_group.external_cluster_provider_evidence_uuid =
      "018f2000-0000-7000-8000-000000079012";
  RequireCode(cluster_delegated,
              "SB_AGENT_TENANT_COORDINATION.CLUSTER_DELEGATION_REQUIRED",
              "local cluster delegation");

  auto authority = Request();
  authority.no_authority.memory_authority = true;
  RequireCode(authority, "SB_AGENT_TENANT_COORDINATION.FORBIDDEN_AUTHORITY",
              "authority drift");
}

void TestLiveEvidenceAndForegroundRefusals() {
  auto live_evidence = Request();
  live_evidence.live_action_evidence_uuid.clear();
  RequireCode(live_evidence,
              "SB_AGENT_TENANT_COORDINATION.LIVE_EVIDENCE_REQUIRED",
              "live evidence");

  auto foreground = Request();
  foreground.tenant_budget.foreground_database_work_active = true;
  RequireCode(foreground,
              "SB_AGENT_TENANT_COORDINATION.FOREGROUND_PROTECTION",
              "foreground protection");
}

}  // namespace

int main() {
  TestPositiveLeaderLiveAction();
  TestFollowerObserveOnly();
  TestBudgetQueueAndQuotaRefusals();
  TestMetricRefusals();
  TestLeadershipAndLockRefusals();
  TestClusterAndAuthorityRefusals();
  TestLiveEvidenceAndForegroundRefusals();
  return EXIT_SUCCESS;
}

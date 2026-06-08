// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "agent_memory_coupling.hpp"

#include <cstdlib>
#include <iostream>
#include <string>

namespace {

namespace agents = scratchbird::core::agents;
namespace memory = scratchbird::core::memory;

constexpr agents::u64 kNowMicros = 83000000ull;

[[noreturn]] void Fail(const std::string& message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, const std::string& message) {
  if (!condition) {
    Fail(message);
  }
}

const agents::AgentTypeDescriptor& Descriptor(const std::string& id) {
  const auto descriptor = agents::FindAgentType(id);
  Require(descriptor.has_value(), "missing descriptor: " + id);
  static agents::AgentTypeDescriptor storage;
  storage = *descriptor;
  return storage;
}

agents::AgentRuntimeContext RuntimeContext() {
  agents::AgentRuntimeContext context;
  context.security_context_present = true;
  context.private_features_available = true;
  context.standalone_edition = true;
  context.principal_uuid = "019f0083-0000-7000-8000-000000000001";
  context.database_uuid = "019f0083-0000-7000-8000-000000000002";
  context.monotonic_now_microseconds = kNowMicros;
  context.wall_now_microseconds = kNowMicros;
  return context;
}

agents::AgentPolicy PolicyFor(const std::string& agent_type_id,
                              const std::string& family,
                              agents::u64 generation,
                              bool live) {
  agents::AgentPolicy policy = agents::BaselinePolicyForAgentFamily(
      Descriptor(agent_type_id), family, generation);
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

agents::AgentResourceBudgetEvaluationInput Budget() {
  agents::AgentResourceBudgetEvaluationInput input;
  input.budget.protect_foreground_work = true;
  input.budget.max_cpu_time_microseconds = 1000000;
  input.budget.max_memory_bytes = 16 * 1024 * 1024;
  input.budget.max_io_bytes = 1024 * 1024;
  input.budget.max_io_ops = 1024;
  input.budget.max_thread_slots = 2;
  input.budget.max_queue_depth = 8;
  input.budget.min_run_interval_microseconds = 1;
  input.budget.retry_backoff_microseconds = 1;
  input.budget.watchdog_timeout_microseconds = 2000000;
  input.budget.max_history_query_rows = 32;
  input.budget.max_evidence_fanout = 16;
  input.budget.max_label_cardinality = 16;
  input.usage.thread_slots = 1;
  input.usage.queue_depth = 1;
  return input;
}

agents::AgentExecutionLaneDecision LaneDecision(
    const std::string& agent_type_id,
    const std::string& policy_family,
    agents::AgentExecutionLanePolicy lane_policy,
    bool live) {
  agents::AgentExecutionLaneRequest request;
  request.action_id = live ? "shrink_cache" : "recommend";
  request.descriptor = Descriptor(agent_type_id);
  request.policy = PolicyFor(agent_type_id, policy_family, 83, live);
  request.runtime_context = RuntimeContext();
  request.lane_policy = lane_policy;
  request.resource_budget = Budget();
  request.resource_budget_evidence_present = true;
  request.production_environment = false;
  auto decision = agents::EvaluateAgentExecutionLaneAdmission(request);
  Require(decision.status.ok && decision.admitted,
          "lane admission refused: " + decision.status.diagnostic_code +
              " " + decision.status.detail);
  return decision;
}

void RegisterMemoryScopes(agents::HierarchicalMemoryBudgetLedger* ledger,
                          const std::string& agent_type_id) {
  agents::HierarchicalMemoryBudgetScope database;
  database.scope_id = "database:ceic083";
  database.kind = agents::HierarchicalMemoryBudgetScopeKind::kDatabase;
  database.limit_bytes = 128 * 1024;
  Require(ledger->RegisterScope(database).ok,
          "database memory scope registration failed");

  agents::HierarchicalMemoryBudgetScope background;
  background.scope_id = "background:" + agent_type_id;
  background.parent_scope_id = database.scope_id;
  background.kind = agents::HierarchicalMemoryBudgetScopeKind::kBackground;
  background.limit_bytes = 64 * 1024;
  Require(ledger->RegisterScope(background).ok,
          "background memory scope registration failed");
}

agents::AgentMemoryReservationResult Reservation(
    const std::string& agent_type_id,
    const std::string& action_id,
    const std::string& operation_id) {
  agents::HierarchicalMemoryBudgetLedger memory_ledger("ceic083-memory");
  RegisterMemoryScopes(&memory_ledger, agent_type_id);
  agents::ResourceGovernanceReservationLedger resource_ledger("ceic083-resource");

  agents::AgentMemoryReservationRequest request;
  request.agent_type_id = agent_type_id;
  request.action_id = action_id;
  request.operation_id = operation_id;
  request.owner_scope = "agent:" + agent_type_id;
  request.leaf_memory_scope_id = "background:" + agent_type_id;
  request.memory_bytes = 4096;
  request.live_action = action_id == "shrink_cache";
  request.allocation_or_memory_action = true;
  auto result = agents::AcquireAgentMemoryReservations(
      request, &memory_ledger, &resource_ledger);
  Require(result.ok, "memory reservation refused: " +
                         result.status.diagnostic_code);
  return result;
}

agents::AgentMemoryMetricSnapshot Snapshot() {
  agents::AgentMemoryMetricSnapshot snapshot;
  snapshot.metric_family = "sb_memory_allocated_bytes";
  snapshot.namespace_path = "sys.metrics.memory";
  snapshot.generation = 83;
  snapshot.sampled_at_microseconds = kNowMicros - 1000;
  snapshot.scope_uuid = "019f0083-0000-7000-8000-000000000003";
  snapshot.digest = "sha256:ceic083-trusted-memory-snapshot";
  snapshot.max_freshness_microseconds = 10000;
  snapshot.source_quality = agents::AgentMetricSourceQuality::trusted;
  snapshot.trusted = true;
  return snapshot;
}

memory::MemoryPressureDecision Pressure(memory::MemoryPressureState state) {
  memory::MemoryPressurePolicy policy;
  memory::MemoryPressureObservation observation;
  observation.route_label = "agent_memory_pressure_lane";
  observation.operation_id = "ceic083-pressure";
  observation.current_bytes = 100;
  observation.soft_limit_bytes = 700;
  observation.hard_limit_bytes = 1000;
  observation.emergency_limit_bytes = 980;
  observation.unified_budget_bytes = 100;
  observation.unified_budget_limit_bytes = 1000;
  observation.spill_supported = true;
  observation.forced_spill_supported = true;
  observation.page_cache_shrink_supported = true;
  observation.background_cleanup_supported = true;
  observation.cancellation_supported = true;
  observation.low_priority_cancellation_supported = true;
  observation.forced_cancel_supported = true;
  observation.noncritical_agent_suspend_supported = true;
  observation.reclaimable_background_bytes = 64;
  observation.page_cache_resident_bytes = 900;
  observation.page_cache_target_bytes = 800;
  observation.low_priority_query_count = 1;
  observation.low_priority_session_count = 1;
  observation.affected_scopes = {"agent:memory", "tenant:default"};

  switch (state) {
    case memory::MemoryPressureState::normal:
      observation.current_bytes = 100;
      observation.unified_budget_bytes = 100;
      observation.page_cache_resident_bytes = 100;
      break;
    case memory::MemoryPressureState::soft_pressure:
      observation.current_bytes = 720;
      observation.unified_budget_bytes = 720;
      observation.page_cache_resident_bytes = 100;
      break;
    case memory::MemoryPressureState::high_pressure:
      observation.current_bytes = 900;
      observation.unified_budget_bytes = 900;
      break;
    case memory::MemoryPressureState::emergency_pressure:
      observation.current_bytes = 990;
      observation.unified_budget_bytes = 990;
      break;
    case memory::MemoryPressureState::recovery:
      observation.previous_state = memory::MemoryPressureState::high_pressure;
      observation.current_bytes = 500;
      observation.unified_budget_bytes = 500;
      observation.pending_readmission_count = 4;
      observation.stable_recovery_observation_count = 0;
      break;
  }
  auto decision = memory::PlanMemoryPressureResponse(policy, observation);
  Require(decision.ok(), "pressure planning refused");
  return decision;
}

agents::AgentMemoryPressureLaneIntegrationRequest IntegrationRequest(
    const std::string& agent_type_id,
    const std::string& policy_family,
    agents::AgentExecutionLaneKind lane,
    const std::string& action_id,
    bool live,
    memory::MemoryPressureState pressure_state) {
  agents::AgentMemoryPressureLaneIntegrationRequest request;
  request.agent_type_id = agent_type_id;
  request.action_id = action_id;
  request.operation_id = "ceic083-" + agent_type_id + "-" + action_id;
  request.lane_policy = agents::DefaultAgentExecutionLanePolicy(lane);
  request.lane_policy.current_queue_depth = 2;
  request.lane_decision =
      LaneDecision(agent_type_id, policy_family, request.lane_policy, live);
  request.reservation_result =
      Reservation(agent_type_id, action_id, request.operation_id);
  request.metric_snapshot = Snapshot();
  request.metric_decision = agents::ValidateAgentMemoryMetricSnapshot(
      request.metric_snapshot, kNowMicros, false);
  Require(request.metric_decision.ok, "metric snapshot refused");
  request.pressure_decision = Pressure(pressure_state);
  request.overhead_budget_microseconds = 10000;
  request.max_evidence_rows = 96;
  request.production_environment = true;
  return request;
}

bool HasMetric(const agents::AgentMemoryPressureLaneIntegrationDecision& decision,
               const std::string& key,
               const std::string& value = {}) {
  for (const auto& row : decision.metric_rows) {
    if (row.key == key && (value.empty() || row.value == value)) {
      return true;
    }
  }
  return false;
}

void RequireDecision(
    const agents::AgentMemoryPressureLaneIntegrationDecision& decision,
    agents::AgentMemoryPressureLaneDecisionKind expected,
    const std::string& message) {
  Require(decision.ok, message + ": decision not ok " +
                           decision.status.diagnostic_code);
  Require(decision.decision == expected,
          message + ": unexpected decision " +
              agents::AgentMemoryPressureLaneDecisionKindName(
                  decision.decision));
  Require(!decision.fail_closed, message + ": failed closed");
}

void TestNormalAdmittedPath() {
  auto request = IntegrationRequest(
      "memory_governor", "memory_governor_policy",
      agents::AgentExecutionLaneKind::memory_pressure, "shrink_cache", true,
      memory::MemoryPressureState::normal);
  const auto decision =
      agents::EvaluateAgentMemoryPressureLaneIntegration(request);
  RequireDecision(decision, agents::AgentMemoryPressureLaneDecisionKind::allow,
                  "normal admitted path");
  Require(decision.action_allowed, "normal path did not allow action");
  Require(HasMetric(decision, "agent_memory_pressure_lane.decision", "allow"),
          "normal path decision metric missing");
}

void TestSoftAndHighPressureLowPriority() {
  auto soft = IntegrationRequest(
      "support_bundle_triage_agent", "support_bundle_policy",
      agents::AgentExecutionLaneKind::support_observability, "recommend", false,
      memory::MemoryPressureState::soft_pressure);
  const auto soft_decision =
      agents::EvaluateAgentMemoryPressureLaneIntegration(soft);
  RequireDecision(soft_decision,
                  agents::AgentMemoryPressureLaneDecisionKind::throttle,
                  "soft pressure low priority");
  Require(soft_decision.throttle_required,
          "soft pressure did not set throttle flag");

  auto high = IntegrationRequest(
      "support_bundle_triage_agent", "support_bundle_policy",
      agents::AgentExecutionLaneKind::support_observability, "recommend", false,
      memory::MemoryPressureState::high_pressure);
  const auto high_decision =
      agents::EvaluateAgentMemoryPressureLaneIntegration(high);
  RequireDecision(high_decision,
                  agents::AgentMemoryPressureLaneDecisionKind::shed_defer,
                  "high pressure low priority");
  Require(high_decision.shed_or_defer_required,
          "high pressure did not shed/defer low-priority work");
}

void TestEmergencyCancelsNoncritical() {
  auto request = IntegrationRequest(
      "support_bundle_triage_agent", "support_bundle_policy",
      agents::AgentExecutionLaneKind::support_observability, "recommend", false,
      memory::MemoryPressureState::emergency_pressure);
  const auto decision =
      agents::EvaluateAgentMemoryPressureLaneIntegration(request);
  RequireDecision(decision, agents::AgentMemoryPressureLaneDecisionKind::cancel,
                  "emergency noncritical");
  Require(decision.cancel_required, "emergency did not request cancellation");
  Require(!decision.action_allowed, "emergency noncritical action allowed");
}

void TestForceSpillCleanupMemoryLane() {
  auto request = IntegrationRequest(
      "memory_governor", "memory_governor_policy",
      agents::AgentExecutionLaneKind::memory_pressure, "shrink_cache", true,
      memory::MemoryPressureState::high_pressure);
  const auto decision =
      agents::EvaluateAgentMemoryPressureLaneIntegration(request);
  RequireDecision(
      decision,
      agents::AgentMemoryPressureLaneDecisionKind::force_spill_cleanup,
      "high pressure memory lane");
  Require(decision.force_spill_or_cleanup_required,
          "high pressure memory lane did not request cleanup");
}

void TestFailClosedEvidenceRequirements() {
  auto request = IntegrationRequest(
      "memory_governor", "memory_governor_policy",
      agents::AgentExecutionLaneKind::memory_pressure, "shrink_cache", true,
      memory::MemoryPressureState::normal);

  auto missing_reservation = request;
  missing_reservation.reservation_result = {};
  auto refused =
      agents::EvaluateAgentMemoryPressureLaneIntegration(missing_reservation);
  Require(!refused.ok && refused.fail_closed,
          "missing reservation did not fail closed");
  Require(refused.status.diagnostic_code ==
              "SB_AGENT_MEMORY_PRESSURE_LANE.RESERVATION_REQUIRED",
          "missing reservation diagnostic mismatch");

  auto missing_metric = request;
  missing_metric.metric_decision = {};
  refused = agents::EvaluateAgentMemoryPressureLaneIntegration(missing_metric);
  Require(!refused.ok && refused.fail_closed,
          "missing metric did not fail closed");
  Require(refused.status.diagnostic_code ==
              "SB_AGENT_MEMORY_PRESSURE_LANE.METRIC_SNAPSHOT_REQUIRED",
          "missing metric diagnostic mismatch");

  auto untrusted_metric = request;
  untrusted_metric.metric_snapshot.trusted = false;
  untrusted_metric.metric_snapshot.source_quality =
      agents::AgentMetricSourceQuality::unknown;
  refused =
      agents::EvaluateAgentMemoryPressureLaneIntegration(untrusted_metric);
  Require(!refused.ok && refused.fail_closed,
          "untrusted metric snapshot did not fail closed");
  Require(refused.status.diagnostic_code ==
              "SB_AGENT_MEMORY_PRESSURE_LANE.UNTRUSTED_METRIC_SNAPSHOT",
          "untrusted metric diagnostic mismatch");

  auto missing_pressure = request;
  missing_pressure.pressure_evidence_present = false;
  refused =
      agents::EvaluateAgentMemoryPressureLaneIntegration(missing_pressure);
  Require(!refused.ok && refused.fail_closed,
          "missing pressure evidence did not fail closed");
  Require(refused.status.diagnostic_code ==
              "SB_AGENT_MEMORY_PRESSURE_LANE.PRESSURE_EVIDENCE_REQUIRED",
          "missing pressure diagnostic mismatch");
}

void TestForegroundProtectionSuppressesCancel() {
  auto request = IntegrationRequest(
      "memory_governor", "memory_governor_policy",
      agents::AgentExecutionLaneKind::memory_pressure, "shrink_cache", true,
      memory::MemoryPressureState::emergency_pressure);
  request.foreground_work_critical = true;
  const auto decision =
      agents::EvaluateAgentMemoryPressureLaneIntegration(request);
  RequireDecision(decision,
                  agents::AgentMemoryPressureLaneDecisionKind::throttle,
                  "foreground protected emergency");
  Require(decision.foreground_protected,
          "foreground protection flag missing");
  Require(!decision.cancel_required,
          "foreground-protected work received cancel decision");
}

void TestLocalClusterPressureRefusal() {
  auto request = IntegrationRequest(
      "memory_governor", "memory_governor_policy",
      agents::AgentExecutionLaneKind::memory_pressure, "shrink_cache", true,
      memory::MemoryPressureState::normal);
  request.local_cluster_pressure_claim = true;
  const auto refused =
      agents::EvaluateAgentMemoryPressureLaneIntegration(request);
  Require(!refused.ok && refused.fail_closed,
          "local cluster pressure claim did not fail closed");
  Require(refused.local_cluster_refused,
          "local cluster refusal flag missing");
  Require(refused.status.diagnostic_code ==
              "SB_AGENT_MEMORY_PRESSURE_LANE.LOCAL_CLUSTER_PRESSURE_REFUSED",
          "local cluster diagnostic mismatch");
}

void TestEvidenceRowsAndAuthorityMarkers() {
  auto request = IntegrationRequest(
      "memory_governor", "memory_governor_policy",
      agents::AgentExecutionLaneKind::memory_pressure, "shrink_cache", true,
      memory::MemoryPressureState::normal);
  request.lane_policy.current_queue_depth = 7;
  const auto decision =
      agents::EvaluateAgentMemoryPressureLaneIntegration(request);
  RequireDecision(decision, agents::AgentMemoryPressureLaneDecisionKind::allow,
                  "evidence rows");
  Require(HasMetric(decision, "agent_memory_pressure_lane.queue_depth", "7"),
          "queue depth row missing");
  Require(HasMetric(decision,
                    "agent_memory_pressure_lane.queue_backlog_depth", "0"),
          "queue backlog row missing");
  Require(HasMetric(decision,
                    "agent_memory_pressure_lane.overhead_budget_microseconds"),
          "overhead budget row missing");
  Require(HasMetric(decision,
                    "agent_memory_pressure_lane.estimated_overhead_microseconds"),
          "estimated overhead row missing");
  Require(HasMetric(decision,
                    "agent_memory_pressure_lane.metric_digest",
                    "sha256:ceic083-trusted-memory-snapshot"),
          "metric digest row missing");
  Require(HasMetric(decision,
                    "agent_memory_pressure_lane.reservation_resource_token_present",
                    "true"),
          "reservation token row missing");
  Require(HasMetric(decision,
                    "agent_memory_pressure_lane.transaction_finality_authority",
                    "false"),
          "transaction finality no-authority row missing");
  Require(HasMetric(decision,
                    "agent_memory_pressure_lane.memory_authority", "false"),
          "memory no-authority row missing");
  Require(HasMetric(decision,
                    "agent_memory_pressure_lane.agent_action_authority",
                    "false"),
          "agent action no-authority row missing");
}

}  // namespace

int main() {
  TestNormalAdmittedPath();
  TestSoftAndHighPressureLowPriority();
  TestEmergencyCancelsNoncritical();
  TestForceSpillCleanupMemoryLane();
  TestFailClosedEvidenceRequirements();
  TestForegroundProtectionSuppressesCancel();
  TestLocalClusterPressureRefusal();
  TestEvidenceRowsAndAuthorityMarkers();
  std::cout << "CEIC_083_AGENT_MEMORY_PRESSURE_METRIC_INTEGRATION ok\n";
  return EXIT_SUCCESS;
}

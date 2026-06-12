// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "agent_local_workflow.hpp"
#include "agents/admission_control_manager.hpp"
#include "agents/job_control_manager.hpp"
#include "agents/memory_governor.hpp"
#include "agents/support_bundle_triage_agent.hpp"
#include "memory.hpp"
#include "memory_pressure_response.hpp"
#include "page_cache.hpp"
#include "query_memory_arena.hpp"
#include "temp_workspace_lifecycle.hpp"
#include "uuid.hpp"

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace agents = scratchbird::core::agents;
namespace impl = scratchbird::core::agents::implemented_agents;
namespace memory = scratchbird::core::memory;
namespace page = scratchbird::storage::page;
namespace uuid = scratchbird::core::uuid;
using scratchbird::core::platform::TypedUuid;
using scratchbird::core::platform::UuidKind;
using scratchbird::storage::disk::PageType;

constexpr std::uint32_t kPageSize = 16384;

struct ExecutionCounters {
  int admission_control = 0;
  int spill_preference = 0;
  int page_cache_shrink = 0;
  int query_cancel = 0;
  int forced_spill = 0;
  int agent_suspend = 0;
  int diagnostics = 0;
};

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view message) {
  if (!condition) {
    Fail(message);
  }
}

bool HasAction(const memory::MemoryPressureDecision& decision,
               memory::MemoryPressureActionKind action) {
  return decision.HasAction(action);
}

bool HasMissingAction(const memory::MemoryPressureExecutorResult& result,
                      memory::MemoryPressureActionKind action) {
  for (const auto missing : result.missing_executor_actions) {
    if (missing == action) {
      return true;
    }
  }
  return false;
}

std::uint64_t CurrentUnixMillis() {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());
}

TypedUuid MakeUuid(UuidKind kind, std::uint64_t offset) {
  auto generated =
      uuid::GenerateEngineIdentityV7(kind, CurrentUnixMillis() + offset);
  Require(generated.ok(), "uuid generation failed");
  return generated.value;
}

memory::AllocationPolicy AllocatorPolicy(std::uint64_t limit = 1024 * 1024) {
  memory::AllocationPolicy policy;
  policy.policy_name = "public_memory_pressure_executor_gate";
  policy.hard_limit_bytes = limit;
  policy.soft_limit_bytes = limit;
  policy.per_context_limit_bytes = limit;
  policy.track_allocations = true;
  policy.zero_memory_on_release = true;
  return policy;
}

memory::QueryMemoryContext QueryContext(std::string suffix) {
  memory::QueryMemoryContext context;
  context.engine_id = "public-engine";
  context.database_id = "public-db";
  context.session_id = "session-" + suffix;
  context.transaction_id = "transaction-" + suffix;
  context.statement_id = "statement-" + suffix;
  context.query_id = "query-" + suffix;
  context.operation_id = "operation-" + suffix;
  return context;
}

memory::QueryMemoryArenaLimits QueryLimits(bool allow_spill) {
  memory::QueryMemoryArenaLimits limits;
  limits.hard_limit_bytes = 4096;
  limits.soft_limit_bytes = allow_spill ? 128 : 4096;
  limits.family_limit_bytes = 4096;
  limits.query_limit_bytes = 4096;
  limits.spill_limit_bytes = 4096;
  limits.allow_spill = allow_spill;
  limits.require_hierarchical_reservation = true;
  return limits;
}

memory::MemoryManager PageCacheManager(std::uint64_t pages) {
  auto policy = memory::DefaultLocalEngineMemoryPolicy();
  policy.policy_name = "public_memory_pressure_page_cache";
  policy.hard_limit_bytes = pages * kPageSize;
  policy.soft_limit_bytes = pages * kPageSize;
  policy.per_context_limit_bytes = pages * kPageSize;
  policy.page_buffer_pool_limit_bytes = pages * kPageSize;
  policy.reject_over_soft_limit = false;
  return memory::MemoryManager(policy);
}

page::PageCachePolicy CachePolicy(std::uint64_t pages) {
  page::PageCachePolicy policy;
  policy.max_resident_pages = pages;
  policy.max_resident_bytes = pages * kPageSize;
  policy.require_memory_manager_frames = true;
  policy.allow_dirty_eviction = false;
  return policy;
}

page::PageCacheEntry PageEntry(const TypedUuid& database_uuid,
                               const TypedUuid& filespace_uuid,
                               std::uint64_t index) {
  page::PageCacheEntry entry;
  entry.database_uuid = database_uuid;
  entry.filespace_uuid = filespace_uuid;
  entry.page_uuid = MakeUuid(UuidKind::page, 100 + index);
  entry.page_type = PageType::row_data;
  entry.page_number = index;
  entry.page_generation = 1;
  entry.page_size = kPageSize;
  return entry;
}

page::PageCacheLifecycleInput LifecycleInput(const TypedUuid& database_uuid,
                                             const TypedUuid& filespace_uuid,
                                             std::uint64_t target_pages) {
  page::PageCacheLifecycleInput input;
  input.database_uuid = database_uuid;
  input.filespace_uuid = filespace_uuid;
  input.database_lifecycle_state = "opened";
  input.policy_generation = 13;
  input.checkpoint_generation = 130;
  input.target_resident_pages = target_pages;
  input.tx2_activation_committed = true;
  input.cache_runtime_started = true;
  input.engine_agent_active = true;
  input.writeback_allowed = true;
  input.checkpoint_allowed = true;
  input.standalone_mode = true;
  input.cluster_authority_available = false;
  return input;
}

memory::MemoryPressureActionExecutionResult Failure(
    memory::MemoryPressureActionKind action,
    std::string executor_name,
    std::string detail) {
  return memory::MemoryPressureActionExecutorFailClosed(
      action,
      std::move(executor_name),
      "memory_pressure_public_executor_gate_failed",
      std::move(detail));
}

memory::MemoryPressureActionExecutionResult RunPageCacheShrink(
    memory::MemoryPressureActionKind action) {
  auto manager = PageCacheManager(4);
  page::PageCacheLedger ledger;
  page::BindPageCacheMemoryManager(&ledger, &manager);
  const auto database_uuid = MakeUuid(UuidKind::database, 1);
  const auto filespace_uuid = MakeUuid(UuidKind::filespace, 2);
  const auto admit_policy = CachePolicy(3);

  if (!page::AdmitPageCacheEntry(
           &ledger, admit_policy,
           PageEntry(database_uuid, filespace_uuid, 1))
           .ok() ||
      !page::AdmitPageCacheEntry(
           &ledger, admit_policy,
           PageEntry(database_uuid, filespace_uuid, 2))
           .ok() ||
      !page::AdmitPageCacheEntry(
           &ledger, admit_policy,
           PageEntry(database_uuid, filespace_uuid, 3))
           .ok()) {
    return Failure(action, "storage.page_cache", "page_cache_setup_failed");
  }

  const auto pressure = page::ApplyPageCacheMemoryPressure(
      &ledger,
      CachePolicy(1),
      LifecycleInput(database_uuid, filespace_uuid, 1));
  if (!pressure.ok() || !pressure.publication.memory_pressure_handled ||
      pressure.evicted_pages != 2 || pressure.snapshot.resident_pages != 1 ||
      manager.Snapshot().page_buffer_current_bytes != kPageSize) {
    return Failure(action,
                   "storage.page_cache.ApplyPageCacheMemoryPressure",
                   "page_cache_pressure_path_failed");
  }
  return memory::MemoryPressureActionExecutorOk(
      action,
      "storage.page_cache.ApplyPageCacheMemoryPressure",
      {"public_memory_pressure_executor.page_cache_evicted_pages=2"});
}

std::vector<std::string> AdmissionEvidence(
    const impl::AdmissionControlResult& result) {
  std::vector<std::string> evidence;
  for (const auto& row : result.evidence) {
    evidence.push_back("public_memory_pressure_executor.admission." +
                       row.key + "=" + row.value);
  }
  evidence.push_back("public_memory_pressure_executor.admission.decision=" +
                     std::string(impl::AdmissionControlDecisionKindName(
                         result.decision)));
  return evidence;
}

std::vector<std::string> GovernorEvidence(
    const impl::MemoryGovernorResult& result) {
  std::vector<std::string> evidence;
  for (const auto& row : result.evidence) {
    evidence.push_back("public_memory_pressure_executor.governor." +
                       row.key + "=" + row.value);
  }
  evidence.push_back("public_memory_pressure_executor.governor.decision=" +
                     std::string(impl::MemoryGovernorDecisionKindName(
                         result.decision)));
  return evidence;
}

memory::MemoryPressureActionExecutionResult RunAdmissionControl(
    memory::MemoryPressureActionKind action) {
  if (action == memory::MemoryPressureActionKind::block_large_grants) {
    impl::MemoryGovernorPolicy policy;
    policy.hard_limit_bytes = 1000;
    policy.soft_limit_bytes = 800;
    impl::MemoryGovernorSnapshot snapshot;
    snapshot.current_bytes = 990;
    snapshot.requested_grant_bytes = 64;
    snapshot.memory_metrics_authoritative = true;
    snapshot.resource_reservation_authoritative = true;
    const auto result = impl::EvaluateMemoryGovernorGrant(snapshot, policy);
    if (!result.ok() ||
        result.decision != impl::MemoryGovernorDecisionKind::deny_large_grant) {
      return Failure(action, "agents.memory_governor", "large_grant_not_denied");
    }
    return memory::MemoryPressureActionExecutorOk(
        action, "agents.memory_governor", GovernorEvidence(result));
  }

  impl::AdmissionControlPolicy policy;
  impl::AdmissionControlSnapshot snapshot;
  snapshot.pressure_metrics_authoritative = true;
  snapshot.resource_ledger_authoritative = true;
  if (action == memory::MemoryPressureActionKind::throttle) {
    policy.throttle_listener_queue_depth = 4;
    snapshot.emergency_reserve_bytes = 128;
    snapshot.listener_queue_depth = 4;
    const auto result = impl::EvaluateAdmissionControlRequest(snapshot, policy);
    if (!result.ok() || !result.throttled) {
      return Failure(action, "agents.admission_control", "throttle_not_routed");
    }
    return memory::MemoryPressureActionExecutorOk(
        action, "agents.admission_control", AdmissionEvidence(result));
  }

  policy.min_emergency_reserve_bytes = 64;
  snapshot.emergency_reserve_bytes = 0;
  const auto result = impl::EvaluateAdmissionControlRequest(snapshot, policy);
  if (!result.ok() || !result.denied) {
    return Failure(action, "agents.admission_control", "deny_not_routed");
  }
  return memory::MemoryPressureActionExecutorOk(
      action, "agents.admission_control", AdmissionEvidence(result));
}

memory::MemoryPressureActionExecutionResult RunSpillGrant(
    memory::MemoryPressureActionKind action,
    const std::filesystem::path& temp_root,
    std::string suffix) {
  const auto root = temp_root / suffix;
  std::filesystem::remove_all(root);

  memory::BoundedAllocator allocator(AllocatorPolicy());
  memory::HierarchicalMemoryBudgetLedger ledger;
  memory::UnifiedMemorySpillBudgetLedger unified(suffix, 4096);
  memory::TempWorkspacePolicy temp_policy;
  temp_policy.policy_name = "public_memory_pressure_executor_spill";
  temp_policy.root_path = root;
  temp_policy.filespace_quota_bytes = 4096;
  temp_policy.session_quota_bytes = 4096;
  temp_policy.transaction_quota_bytes = 4096;
  temp_policy.statement_quota_bytes = 4096;
  temp_policy.operation_quota_bytes = 4096;
  memory::TempWorkspaceLifecycleManager temp_workspace(temp_policy);
  memory::QueryMemoryArena arena(QueryContext(suffix),
                                 QueryLimits(true),
                                 &allocator,
                                 &temp_workspace,
                                 &unified,
                                 &ledger);

  memory::QueryMemoryGrantRequest request;
  request.family = memory::QueryMemoryFamily::relational;
  request.bytes = 512;
  request.spillable = true;
  request.purpose = suffix;
  const auto grant = arena.Grant(request);
  if (!grant.ok() || !grant.grant.has_value() || !grant.grant->spilled ||
      arena.Snapshot().spilled_bytes != 512 ||
      ledger.Snapshot().current_bytes != 512) {
    std::filesystem::remove_all(root);
    return Failure(action, "core.memory.query_memory_arena",
                   "spill_grant_not_created");
  }
  const auto release = arena.Release(grant.grant->grant_id);
  const bool released = release.ok() && ledger.Snapshot().current_bytes == 0 &&
                        unified.Snapshot().total_bytes == 0;
  std::filesystem::remove_all(root);
  if (!released) {
    return Failure(action, "core.memory.query_memory_arena",
                   "spill_grant_not_released");
  }
  return memory::MemoryPressureActionExecutorOk(
      action,
      "core.memory.query_memory_arena.spill",
      {"public_memory_pressure_executor.spilled_bytes=512"});
}

memory::MemoryPressureActionExecutionResult RunQueryCancel(
    memory::MemoryPressureActionKind action) {
  memory::BoundedAllocator allocator(AllocatorPolicy());
  memory::HierarchicalMemoryBudgetLedger ledger;
  memory::UnifiedMemorySpillBudgetLedger unified("cancel", 4096);
  memory::QueryMemoryArena arena(QueryContext("cancel"),
                                 QueryLimits(false),
                                 &allocator,
                                 nullptr,
                                 &unified,
                                 &ledger);
  memory::QueryMemoryGrantRequest request;
  request.family = memory::QueryMemoryFamily::document;
  request.bytes = 256;
  request.purpose = "cancel";
  const auto grant = arena.Grant(request);
  if (!grant.ok() || ledger.Snapshot().current_bytes != 256) {
    return Failure(action, "core.memory.query_memory_arena",
                   "cancel_grant_setup_failed");
  }
  const auto cancel = arena.Cancel("public_memory_pressure_executor_gate");
  if (!cancel.ok() || arena.Snapshot().active_grant_count != 0 ||
      ledger.Snapshot().current_bytes != 0 ||
      unified.Snapshot().total_bytes != 0) {
    return Failure(action, "core.memory.query_memory_arena.cancel",
                   "query_cancel_not_routed");
  }
  return memory::MemoryPressureActionExecutorOk(
      action,
      "core.memory.query_memory_arena.cancel",
      {"public_memory_pressure_executor.query_cancelled=true"});
}

memory::MemoryPressureActionExecutionResult RunAgentSuspend(
    memory::MemoryPressureActionKind action) {
  agents::AgentLocalWorkflowLedger ledger;
  impl::JobControlManagerRequest request;
  request.job_uuid = "job-control-subject-uuid";
  request.database_uuid = "database-uuid";
  request.principal_uuid = "principal-uuid";
  request.mga_transaction_uuid = "mga-transaction-uuid";
  request.evidence_uuid = "evidence-uuid";
  request.idempotency_key = "public-memory-pressure-agent-suspend";
  request.local_transaction_id = 1;
  request.catalog_generation = 1;
  request.suppress_requested = true;
  request.suppression_scope_valid = true;
  request.job_visible = true;
  request.job_metrics_authoritative = true;
  request.durable_catalog_bound = true;
  request.transaction_inventory_bound = true;
  request.intended_state_observed = true;
  const auto result = impl::EvaluateJobControlManagerRequest(&ledger, request);
  if (!result.ok() ||
      result.decision != impl::JobControlManagerDecisionKind::suppress_job ||
      !result.outcome_verified || ledger.records().empty()) {
    return Failure(action, "agents.job_control_manager",
                   "agent_job_suppress_not_routed");
  }
  return memory::MemoryPressureActionExecutorOk(
      action,
      "agents.job_control_manager.suppress_job",
      {"public_memory_pressure_executor.agent_suspend_workflow=true"});
}

memory::MemoryPressureActionExecutionResult RunDiagnostics(
    const memory::MemoryPressureDecision& decision,
    memory::MemoryPressureActionKind action) {
  impl::SupportBundleTriageSnapshot snapshot;
  snapshot.completeness_ratio_per_mille = 1000;
  snapshot.agent_actions_total = 1;
  snapshot.evidence_catalog_authoritative = true;
  snapshot.tamper_evidence_valid = true;
  snapshot.redaction_policy_valid = true;
  snapshot.protected_material_present = true;
  snapshot.support_bundle_sink_available = true;
  const auto result = impl::EvaluateSupportBundleTriage(snapshot);
  if (!decision.emergency_diagnostics.emitted || !result.ok() ||
      result.decision !=
          impl::SupportBundleTriageDecisionKind::prepare_redacted_bundle ||
      !result.protected_material_suppressed) {
    return Failure(action, "agents.support_bundle_triage",
                   "emergency_diagnostics_not_routed");
  }
  return memory::MemoryPressureActionExecutorOk(
      action,
      "agents.support_bundle_triage.prepare_redacted_bundle",
      {"public_memory_pressure_executor.diagnostics_redacted=true"});
}

memory::MemoryPressureActionExecutorSet Executors(
    ExecutionCounters* counters,
    const std::filesystem::path& temp_root) {
  memory::MemoryPressureActionExecutorSet executors;
  executors.admission_control =
      [counters](const memory::MemoryPressureDecision&,
                 memory::MemoryPressureActionKind action) {
        ++counters->admission_control;
        return RunAdmissionControl(action);
      };
  executors.spill_preference =
      [counters, temp_root](const memory::MemoryPressureDecision&,
                            memory::MemoryPressureActionKind action) {
        ++counters->spill_preference;
        return RunSpillGrant(action, temp_root, "prefer-spill");
      };
  executors.page_cache_shrink =
      [counters](const memory::MemoryPressureDecision&,
                 memory::MemoryPressureActionKind action) {
        ++counters->page_cache_shrink;
        return RunPageCacheShrink(action);
      };
  executors.query_cancel =
      [counters](const memory::MemoryPressureDecision&,
                 memory::MemoryPressureActionKind action) {
        ++counters->query_cancel;
        return RunQueryCancel(action);
      };
  executors.diagnostics =
      [counters](const memory::MemoryPressureDecision& decision,
                 memory::MemoryPressureActionKind action) {
        ++counters->diagnostics;
        return RunDiagnostics(decision, action);
      };
  executors.agent_suspend =
      [counters](const memory::MemoryPressureDecision&,
                 memory::MemoryPressureActionKind action) {
        ++counters->agent_suspend;
        return RunAgentSuspend(action);
      };
  executors.forced_spill =
      [counters, temp_root](const memory::MemoryPressureDecision&,
                            memory::MemoryPressureActionKind action) {
        ++counters->forced_spill;
        return RunSpillGrant(action, temp_root, "forced-spill");
      };
  return executors;
}

memory::MemoryPressureDecision PlannedPressureDecision() {
  memory::MemoryPressurePolicy policy;
  memory::MemoryPressureObservation observation;
  observation.route_label = "public.memory_pressure.executor";
  observation.operation_id = "PCR-013";
  observation.current_bytes = 990;
  observation.soft_limit_bytes = 700;
  observation.hard_limit_bytes = 1000;
  observation.emergency_limit_bytes = 990;
  observation.unified_budget_bytes = 990;
  observation.unified_budget_limit_bytes = 1000;
  observation.page_cache_resident_bytes = 3ull * kPageSize;
  observation.page_cache_target_bytes = kPageSize;
  observation.spill_supported = true;
  observation.forced_spill_supported = true;
  observation.page_cache_shrink_supported = true;
  observation.cancellation_supported = true;
  observation.noncritical_agent_suspend_supported = true;
  observation.emergency_diagnostics_supported = true;
  observation.background_cleanup_supported = false;
  observation.adaptive_batch_reduction_supported = false;
  return memory::PlanMemoryPressureResponse(policy, observation);
}

void RequirePlannedActions(const memory::MemoryPressureDecision& decision) {
  Require(decision.ok(), "pressure decision should be plannable");
  Require(decision.new_state == memory::MemoryPressureState::emergency_pressure,
          "pressure decision should enter emergency state");
  Require(HasAction(decision,
                    memory::MemoryPressureActionKind::shrink_page_cache),
          "pressure decision missing page-cache shrink");
  Require(HasAction(decision,
                    memory::MemoryPressureActionKind::emergency_admission_shutdown),
          "pressure decision missing admission shutdown");
  Require(HasAction(decision, memory::MemoryPressureActionKind::refuse_allocation),
          "pressure decision missing allocation refusal");
  Require(HasAction(decision, memory::MemoryPressureActionKind::throttle),
          "pressure decision missing throttle");
  Require(HasAction(decision, memory::MemoryPressureActionKind::cancel_query),
          "pressure decision missing query cancel");
  Require(HasAction(decision, memory::MemoryPressureActionKind::prefer_spill),
          "pressure decision missing spill preference");
  Require(HasAction(decision, memory::MemoryPressureActionKind::forced_spill),
          "pressure decision missing forced spill");
  Require(HasAction(decision,
                    memory::MemoryPressureActionKind::suspend_noncritical_agents_jobs),
          "pressure decision missing agent suspend");
  Require(HasAction(decision,
                    memory::MemoryPressureActionKind::emergency_diagnostics),
          "pressure decision missing diagnostics");
}

void ExecutesEveryBoundExecutor(const std::filesystem::path& temp_root) {
  const auto decision = PlannedPressureDecision();
  RequirePlannedActions(decision);
  ExecutionCounters counters;
  auto result =
      memory::ExecuteMemoryPressureDecision(decision, Executors(&counters, temp_root));
  Require(result.ok(), "bound pressure executors should succeed");
  Require(result.page_cache_shrink_executed,
          "page-cache shrink executor was not marked executed");
  Require(result.admission_control_executed,
          "admission-control executor was not marked executed");
  Require(result.query_cancel_executed,
          "query-cancel executor was not marked executed");
  Require(result.spill_preference_executed,
          "spill-preference executor was not marked executed");
  Require(result.forced_spill_executed,
          "forced-spill executor was not marked executed");
  Require(result.agent_suspend_executed,
          "agent-suspend executor was not marked executed");
  Require(result.diagnostics_executed,
          "diagnostics executor was not marked executed");
  Require(counters.page_cache_shrink == 1, "page-cache callback count changed");
  Require(counters.admission_control >= 4, "admission callback count changed");
  Require(counters.query_cancel == 1, "query-cancel callback count changed");
  Require(counters.spill_preference == 1, "spill-preference count changed");
  Require(counters.forced_spill == 1, "forced-spill count changed");
  Require(counters.agent_suspend == 1, "agent-suspend count changed");
  Require(counters.diagnostics == 1, "diagnostics count changed");
  Require(result.executed_actions.size() == decision.actions.size(),
          "not every planned action was consumed");
}

void MissingExecutorFailsClosedBeforeSideEffects(
    const std::filesystem::path& temp_root) {
  const auto decision = PlannedPressureDecision();
  ExecutionCounters counters;
  auto executors = Executors(&counters, temp_root);
  executors.forced_spill = {};
  const auto result =
      memory::ExecuteMemoryPressureDecision(decision, executors);
  Require(!result.ok() && result.fail_closed,
          "missing pressure executor should fail closed");
  Require(HasMissingAction(result, memory::MemoryPressureActionKind::forced_spill),
          "missing forced-spill executor was not reported");
  Require(result.executed_actions.empty(),
          "executor preflight should prevent partial side effects");
  Require(counters.admission_control == 0 && counters.page_cache_shrink == 0 &&
              counters.query_cancel == 0 && counters.spill_preference == 0 &&
              counters.forced_spill == 0 && counters.agent_suspend == 0 &&
              counters.diagnostics == 0,
          "missing-executor preflight invoked an executor");
}

void RefusedPlannerDecisionIsNotExecutable(
    const std::filesystem::path& temp_root) {
  auto decision = PlannedPressureDecision();
  memory::MemoryPressureObservation unsafe;
  unsafe.route_label = "public.memory_pressure.executor";
  unsafe.operation_id = "PCR-013-unsafe";
  unsafe.current_bytes = 990;
  unsafe.soft_limit_bytes = 700;
  unsafe.hard_limit_bytes = 1000;
  unsafe.parser_or_reference_authority = true;
  decision = memory::PlanMemoryPressureResponse(memory::MemoryPressurePolicy{},
                                                unsafe);
  Require(!decision.ok() && decision.fail_closed,
          "unsafe planner decision should fail closed");

  ExecutionCounters counters;
  const auto result =
      memory::ExecuteMemoryPressureDecision(decision, Executors(&counters, temp_root));
  Require(!result.ok() && result.fail_closed,
          "refused planner decision should not execute");
  Require(counters.admission_control == 0 && counters.page_cache_shrink == 0 &&
              counters.query_cancel == 0 && counters.spill_preference == 0 &&
              counters.forced_spill == 0 && counters.agent_suspend == 0 &&
              counters.diagnostics == 0,
          "refused planner decision invoked an executor");
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 2) {
    std::cerr << "usage: public_memory_pressure_executor_gate <temp-root>\n";
    return EXIT_FAILURE;
  }
  const std::filesystem::path temp_root = argv[1];
  std::filesystem::remove_all(temp_root);
  std::filesystem::create_directories(temp_root);
  ExecutesEveryBoundExecutor(temp_root);
  MissingExecutorFailsClosedBeforeSideEffects(temp_root);
  RefusedPlannerDecisionIsNotExecutable(temp_root);
  std::filesystem::remove_all(temp_root);
  return EXIT_SUCCESS;
}

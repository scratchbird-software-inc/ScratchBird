// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "runtime_platform.hpp"

#include <functional>
#include <string>
#include <vector>

namespace scratchbird::core::memory {

using scratchbird::core::platform::DiagnosticRecord;
using scratchbird::core::platform::Status;
using scratchbird::core::platform::u64;

// SEARCH_KEY: MEMORY_GOVERNANCE_PRESSURE
// MMCH_ADAPTIVE_MEMORY_PRESSURE_RESPONSE
// CEIC-017_MEMORY_PRESSURE_STATE_MACHINE
enum class MemoryPressureState {
  normal,
  soft_pressure,
  high_pressure,
  emergency_pressure,
  recovery
};

enum class MemoryPressureTransitionTrigger {
  none,
  pressure_percent,
  soft_threshold,
  high_threshold,
  hard_limit_exceeded,
  emergency_threshold,
  host_memory_pressure,
  container_memory_pressure,
  linux_cgroup_memory_event,
  windows_job_object_pressure,
  recovery_pressure_drop,
  recovery_stability_window
};

enum class MemoryPressureActionKind {
  none,
  throttle,
  prefer_spill,
  shrink_page_cache,
  background_cleanup,
  cancel_query,
  refuse_allocation,
  emergency_reserve_release,
  emergency_diagnostics,
  emergency_admission_shutdown,
  suspend_noncritical_agents_jobs,
  block_large_grants,
  recovery_readmission_throttling,
  adaptive_batch_reduction,
  forced_spill,
  forced_cancel
};

struct MemoryPressurePolicy {
  u64 soft_pressure_percent = 70;
  u64 high_pressure_percent = 85;
  u64 spill_pressure_percent = 75;
  u64 page_cache_shrink_percent = 80;
  u64 cleanup_pressure_percent = 85;
  u64 cancel_pressure_percent = 95;
  u64 emergency_pressure_percent = 98;
  u64 refuse_pressure_percent = 100;
  u64 recovery_pressure_percent = 60;
  u64 recovery_required_stable_observations = 2;
  u64 recovery_readmission_per_tick = 1;
  u64 max_emergency_diagnostic_rows = 16;
  u64 max_emergency_top_contexts = 4;
  bool require_route_label = true;
  bool require_engine_mga_authority = true;
  bool require_mga_security_recheck_preservation = true;
  bool enable_emergency_admission_shutdown = true;
};

struct MemoryPressureTopContext {
  std::string scope_kind;
  std::string scope_id;
  u64 current_bytes = 0;
  u64 peak_bytes = 0;
  int priority = 0;
  bool low_priority = false;
  bool cancelable = false;
  bool mga_recheck_required = true;
  bool security_recheck_required = true;
};

struct HostMemoryPressureObservation {
  bool observed = false;
  bool pressure = false;
  u64 current_bytes = 0;
  u64 total_bytes = 0;
  u64 available_bytes = 0;
  u64 pressure_percent = 0;
  std::string source = "modeled";
};

struct ContainerMemoryPressureObservation {
  bool observed = false;
  bool pressure = false;
  u64 current_bytes = 0;
  u64 limit_bytes = 0;
  u64 pressure_percent = 0;
  std::string source = "modeled";
};

struct LinuxCgroupMemoryEventObservation {
  bool observed = false;
  u64 current_bytes = 0;
  u64 max_bytes = 0;
  u64 low_events = 0;
  u64 high_events = 0;
  u64 max_events = 0;
  u64 oom_events = 0;
  u64 oom_kill_events = 0;
  bool high_event = false;
  bool max_event = false;
  bool oom_event = false;
  bool oom_kill_event = false;
};

struct WindowsJobObjectMemoryObservation {
  bool observed = false;
  bool pressure = false;
  u64 process_memory_bytes = 0;
  u64 job_memory_bytes = 0;
  u64 job_memory_limit_bytes = 0;
  u64 peak_job_memory_bytes = 0;
  bool limit_violation = false;
  bool notification_pressure = false;
};

struct AllocationFreeEmergencyLoggerModel {
  bool modeled = true;
  bool allocation_free = true;
  bool bounded = true;
  bool redaction_before_buffering = true;
  bool protected_material_excluded = true;
  u64 preallocated_records = 16;
  u64 max_record_bytes = 256;
  u64 used_records = 0;
  u64 dropped_records = 0;
};

struct EmergencyMemoryReserveSnapshot {
  u64 configured_bytes = 0;
  u64 available_bytes = 0;
  u64 released_bytes = 0;
  bool allocated = false;
  bool released = false;
};

class EmergencyMemoryReserve {
 public:
  explicit EmergencyMemoryReserve(u64 configured_bytes = 0);

  void Reset(u64 configured_bytes);
  EmergencyMemoryReserveSnapshot Snapshot() const;
  u64 ReleaseForEmergencyDiagnostics();

 private:
  u64 configured_bytes_ = 0;
  u64 available_bytes_ = 0;
  u64 released_bytes_ = 0;
  bool allocated_ = false;
  bool released_ = false;
};

struct MemoryPressureObservation {
  MemoryPressureState previous_state = MemoryPressureState::normal;
  std::string route_label;
  std::string operation_id;
  u64 current_bytes = 0;
  u64 soft_limit_bytes = 0;
  u64 hard_limit_bytes = 0;
  u64 emergency_limit_bytes = 0;
  u64 unified_budget_bytes = 0;
  u64 unified_budget_limit_bytes = 0;
  u64 page_cache_resident_bytes = 0;
  u64 page_cache_target_bytes = 0;
  u64 active_spill_bytes = 0;
  u64 reclaimable_background_bytes = 0;
  u64 low_priority_query_count = 0;
  u64 low_priority_session_count = 0;
  u64 pending_readmission_count = 0;
  u64 stable_recovery_observation_count = 0;
  bool spill_supported = false;
  bool forced_spill_supported = false;
  bool page_cache_shrink_supported = false;
  bool background_cleanup_supported = false;
  bool cancellation_supported = false;
  bool low_priority_cancellation_supported = false;
  bool forced_cancel_supported = false;
  bool noncritical_agent_suspend_supported = false;
  bool emergency_diagnostics_supported = true;
  bool adaptive_batch_reduction_supported = true;
  bool engine_mga_authoritative = true;
  bool mga_recheck_preserved = true;
  bool security_recheck_preserved = true;
  bool parser_or_reference_authority = false;
  bool client_authority = false;
  bool provider_authority = false;
  bool wal_authority = false;
  bool transaction_finality_authority = false;
  bool visibility_authority = false;
  bool security_authority = false;
  bool recovery_authority = false;
  bool benchmark_authority = false;
  bool optimizer_plan_authority = false;
  bool index_finality_authority = false;
  bool agent_action_authority = false;
  HostMemoryPressureObservation host_pressure;
  ContainerMemoryPressureObservation container_pressure;
  LinuxCgroupMemoryEventObservation linux_cgroup;
  WindowsJobObjectMemoryObservation windows_job;
  AllocationFreeEmergencyLoggerModel emergency_logger;
  std::vector<MemoryPressureTopContext> top_contexts;
  std::vector<std::string> affected_scopes;
};

struct MemoryPressureEmergencyDiagnosticEvidence {
  bool emitted = false;
  bool bounded = false;
  bool allocation_free_logger = false;
  bool redaction_before_buffering = false;
  bool protected_material_excluded = false;
  bool full_support_bundle_deferred = true;
  u64 row_count = 0;
  u64 max_rows = 0;
  u64 top_context_count = 0;
};

struct MemoryPressureDecision {
  Status status;
  bool fail_closed = false;
  u64 pressure_percent = 0;
  MemoryPressureState previous_state = MemoryPressureState::normal;
  MemoryPressureState new_state = MemoryPressureState::normal;
  MemoryPressureTransitionTrigger trigger = MemoryPressureTransitionTrigger::none;
  u64 soft_threshold_bytes = 0;
  u64 hard_threshold_bytes = 0;
  u64 emergency_threshold_bytes = 0;
  bool ordinary_admission_allowed = true;
  bool recovery_readmission_throttled = false;
  u64 recovery_readmission_limit = 0;
  EmergencyMemoryReserveSnapshot emergency_reserve;
  bool emergency_reserve_released = false;
  u64 emergency_reserve_released_bytes = 0;
  MemoryPressureEmergencyDiagnosticEvidence emergency_diagnostics;
  std::vector<MemoryPressureTopContext> top_contexts;
  std::vector<std::string> affected_scopes;
  std::vector<MemoryPressureActionKind> actions;
  DiagnosticRecord diagnostic;
  std::vector<std::string> evidence;

  bool ok() const {
    return status.ok();
  }

  bool HasAction(MemoryPressureActionKind action) const;
};

struct MemoryPressureActionExecutionResult {
  Status status;
  bool fail_closed = true;
  MemoryPressureActionKind action = MemoryPressureActionKind::none;
  std::string executor_name;
  DiagnosticRecord diagnostic;
  std::vector<std::string> evidence;

  bool ok() const {
    return status.ok() && !fail_closed;
  }
};

using MemoryPressureActionExecutor =
    std::function<MemoryPressureActionExecutionResult(
        const MemoryPressureDecision&, MemoryPressureActionKind)>;

struct MemoryPressureActionExecutorSet {
  MemoryPressureActionExecutor admission_control;
  MemoryPressureActionExecutor spill_preference;
  MemoryPressureActionExecutor page_cache_shrink;
  MemoryPressureActionExecutor background_cleanup;
  MemoryPressureActionExecutor query_cancel;
  MemoryPressureActionExecutor emergency_reserve_release;
  MemoryPressureActionExecutor diagnostics;
  MemoryPressureActionExecutor agent_suspend;
  MemoryPressureActionExecutor adaptive_batch_reduction;
  MemoryPressureActionExecutor forced_spill;
  MemoryPressureActionExecutor forced_cancel;
};

struct MemoryPressureExecutorResult {
  Status status;
  bool fail_closed = true;
  MemoryPressureActionKind failed_action = MemoryPressureActionKind::none;
  DiagnosticRecord diagnostic;
  std::vector<MemoryPressureActionKind> executed_actions;
  std::vector<MemoryPressureActionKind> missing_executor_actions;
  std::vector<std::string> evidence;
  bool admission_control_executed = false;
  bool spill_preference_executed = false;
  bool page_cache_shrink_executed = false;
  bool query_cancel_executed = false;
  bool forced_spill_executed = false;
  bool agent_suspend_executed = false;
  bool diagnostics_executed = false;

  bool ok() const {
    return status.ok() && !fail_closed && missing_executor_actions.empty();
  }
};

const char* MemoryPressureStateName(MemoryPressureState state);
const char* MemoryPressureTransitionTriggerName(MemoryPressureTransitionTrigger trigger);
const char* MemoryPressureActionKindName(MemoryPressureActionKind action);
MemoryPressureActionExecutionResult MemoryPressureActionExecutorOk(
    MemoryPressureActionKind action,
    std::string executor_name,
    std::vector<std::string> evidence = {});
MemoryPressureActionExecutionResult MemoryPressureActionExecutorFailClosed(
    MemoryPressureActionKind action,
    std::string executor_name,
    std::string diagnostic_code,
    std::string detail,
    std::vector<std::string> evidence = {});
MemoryPressureExecutorResult ExecuteMemoryPressureDecision(
    const MemoryPressureDecision& decision,
    const MemoryPressureActionExecutorSet& executors);
MemoryPressureDecision PlanMemoryPressureResponse(const MemoryPressurePolicy& policy,
                                                  const MemoryPressureObservation& observation);
MemoryPressureDecision PlanMemoryPressureResponse(const MemoryPressurePolicy& policy,
                                                  const MemoryPressureObservation& observation,
                                                  EmergencyMemoryReserve* reserve);

}  // namespace scratchbird::core::memory

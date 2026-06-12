// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "memory_pressure_response.hpp"

#include <algorithm>
#include <cstddef>
#include <iterator>
#include <string>
#include <utility>

namespace scratchbird::core::memory {
namespace {

using scratchbird::core::platform::DiagnosticArgument;
using scratchbird::core::platform::MakeDiagnostic;
using scratchbird::core::platform::Severity;
using scratchbird::core::platform::StatusCode;
using scratchbird::core::platform::Subsystem;

constexpr const char* kCeic017EvidenceAnchor =
    "CEIC-017_MEMORY_PRESSURE_STATE_MACHINE";
constexpr const char* kLegacyAuthorityBoundary =
    "memory_pressure.authority_scope=evidence_only_not_transaction_finality_visibility_security_recovery_parser_reference_or_benchmark_authority";
constexpr const char* kExpandedAuthorityBoundary =
    "memory_pressure.authority_scope=evidence_only_not_transaction_finality_visibility_security_authorization_recovery_parser_reference_wal_benchmark_optimizer_plan_index_finality_or_agent_action_authority";

Status MemoryOkStatus() {
  return {StatusCode::ok, Severity::info, Subsystem::memory};
}

Status MemoryPressureErrorStatus() {
  return {StatusCode::memory_limit_exceeded, Severity::error, Subsystem::memory};
}

Status MemoryPressureExecutorErrorStatus() {
  return {StatusCode::memory_invalid_request, Severity::error, Subsystem::memory};
}

DiagnosticRecord MemoryPressureDiagnostic(Status status,
                                          std::string code,
                                          std::string message,
                                          std::vector<DiagnosticArgument> arguments = {}) {
  return MakeDiagnostic(status.code,
                        status.severity,
                        status.subsystem,
                        std::move(code),
                        std::move(message),
                        std::move(arguments),
                        {},
                        "core.memory.pressure_response",
                        "memory_pressure_requires_engine_route_evidence_and_mga_safe_authority");
}

std::string BoolString(bool value) {
  return value ? "true" : "false";
}

MemoryPressureDecision ErrorDecision(std::string code,
                                     std::string message,
                                     std::vector<DiagnosticArgument> arguments = {}) {
  MemoryPressureDecision decision;
  decision.status = MemoryPressureErrorStatus();
  decision.fail_closed = true;
  decision.ordinary_admission_allowed = false;
  decision.diagnostic =
      MemoryPressureDiagnostic(decision.status, std::move(code), std::move(message), std::move(arguments));
  decision.evidence.push_back("MMCH_ADAPTIVE_MEMORY_PRESSURE_RESPONSE");
  decision.evidence.push_back(kCeic017EvidenceAnchor);
  decision.evidence.push_back("memory_pressure.fail_closed=true");
  decision.evidence.push_back(kLegacyAuthorityBoundary);
  decision.evidence.push_back(kExpandedAuthorityBoundary);
  return decision;
}

u64 PercentOf(u64 numerator, u64 denominator) {
  if (denominator == 0) {
    return 0;
  }
  if (numerator >= denominator) {
    return 100;
  }
  return (numerator * 100) / denominator;
}

u64 BudgetPressurePercent(const MemoryPressureObservation& observation) {
  u64 pressure = 0;
  pressure = std::max(pressure,
                      PercentOf(observation.current_bytes,
                                observation.hard_limit_bytes));
  pressure = std::max(pressure,
                      PercentOf(observation.unified_budget_bytes,
                                observation.unified_budget_limit_bytes));
  pressure = std::max(pressure, observation.host_pressure.pressure_percent);
  pressure = std::max(pressure, observation.container_pressure.pressure_percent);
  pressure = std::max(pressure,
                      PercentOf(observation.linux_cgroup.current_bytes,
                                observation.linux_cgroup.max_bytes));
  pressure = std::max(pressure,
                      PercentOf(observation.windows_job.job_memory_bytes,
                                observation.windows_job.job_memory_limit_bytes));
  if (pressure == 0) {
    pressure = PercentOf(observation.current_bytes,
                         observation.soft_limit_bytes);
  }
  return pressure;
}

u64 SaturatingMaxPressure(const MemoryPressureObservation& observation) {
  u64 pressure = BudgetPressurePercent(observation);
  pressure = std::max(pressure,
                      PercentOf(observation.page_cache_resident_bytes,
                                observation.page_cache_target_bytes));
  return pressure;
}

u64 EmergencyThresholdBytes(const MemoryPressureObservation& observation) {
  if (observation.emergency_limit_bytes != 0) {
    return observation.emergency_limit_bytes;
  }
  return observation.hard_limit_bytes;
}

bool HardLimitExceeded(const MemoryPressureObservation& observation) {
  return (observation.hard_limit_bytes != 0 &&
          observation.current_bytes > observation.hard_limit_bytes) ||
         (observation.unified_budget_limit_bytes != 0 &&
          observation.unified_budget_bytes > observation.unified_budget_limit_bytes);
}

bool EmergencyThresholdReached(const MemoryPressureObservation& observation) {
  const u64 threshold = EmergencyThresholdBytes(observation);
  return threshold != 0 && observation.current_bytes >= threshold;
}

bool CgroupEmergency(const MemoryPressureObservation& observation) {
  return observation.linux_cgroup.observed &&
         (observation.linux_cgroup.max_event ||
          observation.linux_cgroup.oom_event ||
          observation.linux_cgroup.oom_kill_event ||
          observation.linux_cgroup.max_events != 0 ||
          observation.linux_cgroup.oom_events != 0 ||
          observation.linux_cgroup.oom_kill_events != 0);
}

bool CgroupPressure(const MemoryPressureObservation& observation) {
  return observation.linux_cgroup.observed &&
         (observation.linux_cgroup.high_event ||
          observation.linux_cgroup.high_events != 0 ||
          CgroupEmergency(observation));
}

bool WindowsJobEmergency(const MemoryPressureObservation& observation) {
  return observation.windows_job.observed &&
         (observation.windows_job.limit_violation ||
          (observation.windows_job.job_memory_limit_bytes != 0 &&
           observation.windows_job.job_memory_bytes >=
               observation.windows_job.job_memory_limit_bytes));
}

bool WindowsJobPressure(const MemoryPressureObservation& observation) {
  return observation.windows_job.observed &&
         (observation.windows_job.pressure ||
          observation.windows_job.notification_pressure ||
          WindowsJobEmergency(observation));
}

bool AtLeast(MemoryPressureState state, MemoryPressureState threshold) {
  auto rank = [](MemoryPressureState value) {
    switch (value) {
      case MemoryPressureState::normal: return 0;
      case MemoryPressureState::soft_pressure: return 1;
      case MemoryPressureState::high_pressure: return 2;
      case MemoryPressureState::emergency_pressure: return 3;
      case MemoryPressureState::recovery: return 1;
    }
    return 0;
  };
  return rank(state) >= rank(threshold);
}

MemoryPressureState SelectPressureState(const MemoryPressurePolicy& policy,
                                        const MemoryPressureObservation& observation,
                                        u64 budget_pressure_percent) {
  const bool page_cache_high =
      observation.page_cache_target_bytes != 0 &&
      observation.page_cache_resident_bytes > observation.page_cache_target_bytes &&
      PercentOf(observation.page_cache_resident_bytes,
                observation.page_cache_target_bytes) >= policy.high_pressure_percent;

  MemoryPressureState target = MemoryPressureState::normal;
  if (HardLimitExceeded(observation) ||
      EmergencyThresholdReached(observation) ||
      CgroupEmergency(observation) ||
      WindowsJobEmergency(observation) ||
      budget_pressure_percent >= policy.emergency_pressure_percent) {
    target = MemoryPressureState::emergency_pressure;
  } else if (budget_pressure_percent >= policy.high_pressure_percent ||
             page_cache_high ||
             CgroupPressure(observation) ||
             WindowsJobPressure(observation) ||
             (observation.host_pressure.observed && observation.host_pressure.pressure) ||
             (observation.container_pressure.observed &&
              observation.container_pressure.pressure)) {
    target = MemoryPressureState::high_pressure;
  } else if ((observation.soft_limit_bytes != 0 &&
              observation.current_bytes >= observation.soft_limit_bytes) ||
             budget_pressure_percent >= policy.soft_pressure_percent) {
    target = MemoryPressureState::soft_pressure;
  }

  if (target == MemoryPressureState::normal) {
    if (observation.previous_state == MemoryPressureState::soft_pressure ||
        observation.previous_state == MemoryPressureState::high_pressure ||
        observation.previous_state == MemoryPressureState::emergency_pressure) {
      return MemoryPressureState::recovery;
    }
    if (observation.previous_state == MemoryPressureState::recovery) {
      const bool stable =
          observation.stable_recovery_observation_count >=
          policy.recovery_required_stable_observations;
      const bool readmission_bounded =
          observation.pending_readmission_count <=
          policy.recovery_readmission_per_tick;
      const bool pressure_below_recovery =
          budget_pressure_percent <= policy.recovery_pressure_percent;
      return stable && readmission_bounded && pressure_below_recovery
                 ? MemoryPressureState::normal
                 : MemoryPressureState::recovery;
    }
  }
  return target;
}

MemoryPressureTransitionTrigger SelectTrigger(
    MemoryPressureState target_state,
    const MemoryPressurePolicy& policy,
    const MemoryPressureObservation& observation,
    u64 budget_pressure_percent) {
  if (target_state == MemoryPressureState::recovery) {
    return MemoryPressureTransitionTrigger::recovery_pressure_drop;
  }
  if (HardLimitExceeded(observation)) {
    return MemoryPressureTransitionTrigger::hard_limit_exceeded;
  }
  if (EmergencyThresholdReached(observation) ||
      budget_pressure_percent >= policy.emergency_pressure_percent) {
    return MemoryPressureTransitionTrigger::emergency_threshold;
  }
  if (CgroupPressure(observation)) {
    return MemoryPressureTransitionTrigger::linux_cgroup_memory_event;
  }
  if (WindowsJobPressure(observation)) {
    return MemoryPressureTransitionTrigger::windows_job_object_pressure;
  }
  if (observation.host_pressure.observed && observation.host_pressure.pressure) {
    return MemoryPressureTransitionTrigger::host_memory_pressure;
  }
  if (observation.container_pressure.observed &&
      observation.container_pressure.pressure) {
    return MemoryPressureTransitionTrigger::container_memory_pressure;
  }
  if (target_state == MemoryPressureState::high_pressure) {
    return MemoryPressureTransitionTrigger::high_threshold;
  }
  if (target_state == MemoryPressureState::soft_pressure) {
    return MemoryPressureTransitionTrigger::soft_threshold;
  }
  if (target_state == MemoryPressureState::normal &&
      observation.previous_state == MemoryPressureState::recovery) {
    return MemoryPressureTransitionTrigger::recovery_stability_window;
  }
  if (budget_pressure_percent != 0) {
    return MemoryPressureTransitionTrigger::pressure_percent;
  }
  return MemoryPressureTransitionTrigger::none;
}

void AddAction(MemoryPressureDecision* decision, MemoryPressureActionKind action) {
  if (decision == nullptr) {
    return;
  }
  if (decision->HasAction(action)) {
    return;
  }
  decision->actions.push_back(action);
}

void AddActionEvidence(MemoryPressureDecision* decision) {
  if (decision == nullptr) {
    return;
  }
  for (const auto action : decision->actions) {
    decision->evidence.push_back(std::string("memory_pressure.action=") +
                                 MemoryPressureActionKindName(action));
  }
}

const MemoryPressureActionExecutor* ExecutorForAction(
    const MemoryPressureActionExecutorSet& executors,
    MemoryPressureActionKind action) {
  switch (action) {
    case MemoryPressureActionKind::none:
      return nullptr;
    case MemoryPressureActionKind::throttle:
    case MemoryPressureActionKind::refuse_allocation:
    case MemoryPressureActionKind::emergency_admission_shutdown:
    case MemoryPressureActionKind::block_large_grants:
    case MemoryPressureActionKind::recovery_readmission_throttling:
      return &executors.admission_control;
    case MemoryPressureActionKind::prefer_spill:
      return &executors.spill_preference;
    case MemoryPressureActionKind::shrink_page_cache:
      return &executors.page_cache_shrink;
    case MemoryPressureActionKind::background_cleanup:
      return &executors.background_cleanup;
    case MemoryPressureActionKind::cancel_query:
      return &executors.query_cancel;
    case MemoryPressureActionKind::emergency_reserve_release:
      return &executors.emergency_reserve_release;
    case MemoryPressureActionKind::emergency_diagnostics:
      return &executors.diagnostics;
    case MemoryPressureActionKind::suspend_noncritical_agents_jobs:
      return &executors.agent_suspend;
    case MemoryPressureActionKind::adaptive_batch_reduction:
      return &executors.adaptive_batch_reduction;
    case MemoryPressureActionKind::forced_spill:
      return &executors.forced_spill;
    case MemoryPressureActionKind::forced_cancel:
      return &executors.forced_cancel;
  }
  return nullptr;
}

void AddExecutorBaseEvidence(MemoryPressureExecutorResult* result,
                             const MemoryPressureDecision& decision) {
  if (result == nullptr) {
    return;
  }
  result->evidence.push_back("PUBLIC_MEMORY_PRESSURE_ACTION_EXECUTOR");
  result->evidence.push_back("memory_pressure.executor.input_state=" +
                             std::string(MemoryPressureStateName(decision.new_state)));
  result->evidence.push_back("memory_pressure.executor.input_pressure_percent=" +
                             std::to_string(decision.pressure_percent));
  result->evidence.push_back("memory_pressure.executor.input_action_count=" +
                             std::to_string(decision.actions.size()));
  result->evidence.push_back(
      "memory_pressure.executor.plan_evidence_authority=false");
  result->evidence.push_back(
      "memory_pressure.executor.transaction_finality_authority=false");
  result->evidence.push_back(
      "memory_pressure.executor.visibility_authority=false");
  result->evidence.push_back(
      "memory_pressure.executor.recovery_authority=false");
  result->evidence.push_back(
      "memory_pressure.executor.parser_or_reference_authority=false");
  result->evidence.push_back(
      "memory_pressure.executor.cluster_production_execution=external_provider_only");
}

void MarkExecuted(MemoryPressureExecutorResult* result,
                  MemoryPressureActionKind action) {
  if (result == nullptr) {
    return;
  }
  result->executed_actions.push_back(action);
  result->evidence.push_back("memory_pressure.executor.executed_action=" +
                             std::string(MemoryPressureActionKindName(action)));
  switch (action) {
    case MemoryPressureActionKind::throttle:
    case MemoryPressureActionKind::refuse_allocation:
    case MemoryPressureActionKind::emergency_admission_shutdown:
    case MemoryPressureActionKind::block_large_grants:
    case MemoryPressureActionKind::recovery_readmission_throttling:
      result->admission_control_executed = true;
      break;
    case MemoryPressureActionKind::prefer_spill:
      result->spill_preference_executed = true;
      break;
    case MemoryPressureActionKind::shrink_page_cache:
      result->page_cache_shrink_executed = true;
      break;
    case MemoryPressureActionKind::cancel_query:
    case MemoryPressureActionKind::forced_cancel:
      result->query_cancel_executed = true;
      break;
    case MemoryPressureActionKind::forced_spill:
      result->forced_spill_executed = true;
      break;
    case MemoryPressureActionKind::suspend_noncritical_agents_jobs:
      result->agent_suspend_executed = true;
      break;
    case MemoryPressureActionKind::emergency_diagnostics:
      result->diagnostics_executed = true;
      break;
    case MemoryPressureActionKind::none:
    case MemoryPressureActionKind::background_cleanup:
    case MemoryPressureActionKind::emergency_reserve_release:
    case MemoryPressureActionKind::adaptive_batch_reduction:
      break;
  }
}

void AddExecutorSummaryEvidence(MemoryPressureExecutorResult* result) {
  if (result == nullptr) {
    return;
  }
  result->evidence.push_back("memory_pressure.executor.fail_closed=" +
                             BoolString(result->fail_closed));
  result->evidence.push_back("memory_pressure.executor.executed_action_count=" +
                             std::to_string(result->executed_actions.size()));
  result->evidence.push_back(
      "memory_pressure.executor.admission_control_executed=" +
      BoolString(result->admission_control_executed));
  result->evidence.push_back(
      "memory_pressure.executor.spill_preference_executed=" +
      BoolString(result->spill_preference_executed));
  result->evidence.push_back(
      "memory_pressure.executor.page_cache_shrink_executed=" +
      BoolString(result->page_cache_shrink_executed));
  result->evidence.push_back(
      "memory_pressure.executor.query_cancel_executed=" +
      BoolString(result->query_cancel_executed));
  result->evidence.push_back(
      "memory_pressure.executor.forced_spill_executed=" +
      BoolString(result->forced_spill_executed));
  result->evidence.push_back(
      "memory_pressure.executor.agent_suspend_executed=" +
      BoolString(result->agent_suspend_executed));
  result->evidence.push_back(
      "memory_pressure.executor.diagnostics_executed=" +
      BoolString(result->diagnostics_executed));
}

MemoryPressureExecutorResult ExecutorFailure(
    const MemoryPressureDecision& decision,
    std::string code,
    std::string message,
    std::vector<DiagnosticArgument> arguments = {}) {
  MemoryPressureExecutorResult result;
  result.status = MemoryPressureExecutorErrorStatus();
  result.fail_closed = true;
  result.diagnostic =
      MemoryPressureDiagnostic(result.status, std::move(code), std::move(message),
                               std::move(arguments));
  AddExecutorBaseEvidence(&result, decision);
  result.evidence.push_back("memory_pressure.executor.fail_closed=true");
  result.evidence.push_back("memory_pressure.executor.diagnostic_code=" +
                            result.diagnostic.diagnostic_code);
  return result;
}

std::vector<MemoryPressureTopContext> SortedTopContexts(
    const MemoryPressureObservation& observation,
    u64 max_contexts) {
  auto contexts = observation.top_contexts;
  std::sort(contexts.begin(), contexts.end(),
            [](const MemoryPressureTopContext& left,
               const MemoryPressureTopContext& right) {
              if (left.current_bytes != right.current_bytes) {
                return left.current_bytes > right.current_bytes;
              }
              return left.scope_id < right.scope_id;
            });
  if (max_contexts != 0 && contexts.size() > max_contexts) {
    contexts.resize(static_cast<std::size_t>(max_contexts));
  }
  return contexts;
}

void AddTopContextEvidence(MemoryPressureDecision* decision) {
  if (decision == nullptr) {
    return;
  }
  u64 index = 0;
  for (const auto& context : decision->top_contexts) {
    const std::string prefix =
        "memory_pressure.top_context." + std::to_string(index++);
    decision->evidence.push_back(prefix + ".scope_kind=" + context.scope_kind);
    decision->evidence.push_back(prefix + ".scope_id=" + context.scope_id);
    decision->evidence.push_back(prefix + ".current_bytes=" +
                                 std::to_string(context.current_bytes));
    decision->evidence.push_back(prefix + ".peak_bytes=" +
                                 std::to_string(context.peak_bytes));
    decision->evidence.push_back(prefix + ".low_priority=" +
                                 BoolString(context.low_priority));
    decision->evidence.push_back(prefix + ".cancelable=" +
                                 BoolString(context.cancelable));
    decision->evidence.push_back(prefix + ".mga_recheck_required=" +
                                 BoolString(context.mga_recheck_required));
    decision->evidence.push_back(prefix + ".security_recheck_required=" +
                                 BoolString(context.security_recheck_required));
  }
  decision->evidence.push_back("memory_pressure.top_context.count=" +
                               std::to_string(decision->top_contexts.size()));
}

void AddObservationEvidence(MemoryPressureDecision* decision,
                            const MemoryPressureObservation& observation) {
  if (decision == nullptr) {
    return;
  }
  decision->evidence.push_back("memory_pressure.current_bytes=" +
                               std::to_string(observation.current_bytes));
  decision->evidence.push_back("memory_pressure.threshold.soft_bytes=" +
                               std::to_string(decision->soft_threshold_bytes));
  decision->evidence.push_back("memory_pressure.threshold.hard_bytes=" +
                               std::to_string(decision->hard_threshold_bytes));
  decision->evidence.push_back("memory_pressure.threshold.emergency_bytes=" +
                               std::to_string(decision->emergency_threshold_bytes));
  decision->evidence.push_back("memory_pressure.host.observed=" +
                               BoolString(observation.host_pressure.observed));
  decision->evidence.push_back("memory_pressure.host.pressure=" +
                               BoolString(observation.host_pressure.pressure));
  decision->evidence.push_back("memory_pressure.host.current_bytes=" +
                               std::to_string(observation.host_pressure.current_bytes));
  decision->evidence.push_back("memory_pressure.host.total_bytes=" +
                               std::to_string(observation.host_pressure.total_bytes));
  decision->evidence.push_back("memory_pressure.host.available_bytes=" +
                               std::to_string(observation.host_pressure.available_bytes));
  decision->evidence.push_back("memory_pressure.host.pressure_percent=" +
                               std::to_string(observation.host_pressure.pressure_percent));
  decision->evidence.push_back("memory_pressure.container.observed=" +
                               BoolString(observation.container_pressure.observed));
  decision->evidence.push_back("memory_pressure.container.pressure=" +
                               BoolString(observation.container_pressure.pressure));
  decision->evidence.push_back("memory_pressure.container.current_bytes=" +
                               std::to_string(observation.container_pressure.current_bytes));
  decision->evidence.push_back("memory_pressure.container.limit_bytes=" +
                               std::to_string(observation.container_pressure.limit_bytes));
  decision->evidence.push_back("memory_pressure.container.pressure_percent=" +
                               std::to_string(observation.container_pressure.pressure_percent));
  decision->evidence.push_back("memory_pressure.linux_cgroup.observed=" +
                               BoolString(observation.linux_cgroup.observed));
  decision->evidence.push_back("memory_pressure.linux_cgroup.current_bytes=" +
                               std::to_string(observation.linux_cgroup.current_bytes));
  decision->evidence.push_back("memory_pressure.linux_cgroup.max_bytes=" +
                               std::to_string(observation.linux_cgroup.max_bytes));
  decision->evidence.push_back("memory_pressure.linux_cgroup.high_events=" +
                               std::to_string(observation.linux_cgroup.high_events));
  decision->evidence.push_back("memory_pressure.linux_cgroup.max_events=" +
                               std::to_string(observation.linux_cgroup.max_events));
  decision->evidence.push_back("memory_pressure.linux_cgroup.oom_events=" +
                               std::to_string(observation.linux_cgroup.oom_events));
  decision->evidence.push_back("memory_pressure.linux_cgroup.oom_kill_events=" +
                               std::to_string(observation.linux_cgroup.oom_kill_events));
  decision->evidence.push_back("memory_pressure.windows_job.observed=" +
                               BoolString(observation.windows_job.observed));
  decision->evidence.push_back("memory_pressure.windows_job.pressure=" +
                               BoolString(observation.windows_job.pressure));
  decision->evidence.push_back("memory_pressure.windows_job.process_memory_bytes=" +
                               std::to_string(observation.windows_job.process_memory_bytes));
  decision->evidence.push_back("memory_pressure.windows_job.job_memory_bytes=" +
                               std::to_string(observation.windows_job.job_memory_bytes));
  decision->evidence.push_back("memory_pressure.windows_job.job_memory_limit_bytes=" +
                               std::to_string(observation.windows_job.job_memory_limit_bytes));
  decision->evidence.push_back("memory_pressure.windows_job.limit_violation=" +
                               BoolString(observation.windows_job.limit_violation));
  for (const auto& scope : decision->affected_scopes) {
    decision->evidence.push_back("memory_pressure.affected_scope=" + scope);
  }
}

MemoryPressureEmergencyDiagnosticEvidence BuildEmergencyDiagnosticEvidence(
    const MemoryPressurePolicy& policy,
    const MemoryPressureObservation& observation,
    const MemoryPressureDecision& decision) {
  MemoryPressureEmergencyDiagnosticEvidence evidence;
  evidence.emitted = observation.emergency_diagnostics_supported;
  evidence.bounded = observation.emergency_logger.bounded;
  evidence.allocation_free_logger =
      observation.emergency_logger.modeled &&
      observation.emergency_logger.allocation_free;
  evidence.redaction_before_buffering =
      observation.emergency_logger.redaction_before_buffering;
  evidence.protected_material_excluded =
      observation.emergency_logger.protected_material_excluded;
  evidence.full_support_bundle_deferred = true;
  evidence.max_rows = policy.max_emergency_diagnostic_rows;
  evidence.top_context_count = decision.top_contexts.size();
  const u64 fixed_rows = 7;
  evidence.row_count =
      std::min(evidence.max_rows, fixed_rows + evidence.top_context_count);
  return evidence;
}

void AddEmergencyDiagnosticEvidence(MemoryPressureDecision* decision) {
  if (decision == nullptr) {
    return;
  }
  decision->evidence.push_back("memory_pressure.emergency_diagnostics.emitted=" +
                               BoolString(decision->emergency_diagnostics.emitted));
  decision->evidence.push_back("memory_pressure.emergency_diagnostics.bounded=" +
                               BoolString(decision->emergency_diagnostics.bounded));
  decision->evidence.push_back(
      "memory_pressure.emergency_diagnostics.allocation_free_logger=" +
      BoolString(decision->emergency_diagnostics.allocation_free_logger));
  decision->evidence.push_back(
      "memory_pressure.emergency_diagnostics.redaction_before_buffering=" +
      BoolString(decision->emergency_diagnostics.redaction_before_buffering));
  decision->evidence.push_back(
      "memory_pressure.emergency_diagnostics.protected_material_excluded=" +
      BoolString(decision->emergency_diagnostics.protected_material_excluded));
  decision->evidence.push_back(
      "memory_pressure.emergency_diagnostics.full_support_bundle_deferred_to_CEIC_023=" +
      BoolString(decision->emergency_diagnostics.full_support_bundle_deferred));
  decision->evidence.push_back("memory_pressure.emergency_diagnostics.row_count=" +
                               std::to_string(decision->emergency_diagnostics.row_count));
  decision->evidence.push_back("memory_pressure.emergency_diagnostics.max_rows=" +
                               std::to_string(decision->emergency_diagnostics.max_rows));
}

}  // namespace

EmergencyMemoryReserve::EmergencyMemoryReserve(u64 configured_bytes) {
  Reset(configured_bytes);
}

void EmergencyMemoryReserve::Reset(u64 configured_bytes) {
  configured_bytes_ = configured_bytes;
  available_bytes_ = configured_bytes;
  released_bytes_ = 0;
  allocated_ = configured_bytes != 0;
  released_ = false;
}

EmergencyMemoryReserveSnapshot EmergencyMemoryReserve::Snapshot() const {
  EmergencyMemoryReserveSnapshot snapshot;
  snapshot.configured_bytes = configured_bytes_;
  snapshot.available_bytes = available_bytes_;
  snapshot.released_bytes = released_bytes_;
  snapshot.allocated = allocated_;
  snapshot.released = released_;
  return snapshot;
}

u64 EmergencyMemoryReserve::ReleaseForEmergencyDiagnostics() {
  if (!allocated_ || released_ || available_bytes_ == 0) {
    return 0;
  }
  released_bytes_ = available_bytes_;
  available_bytes_ = 0;
  released_ = true;
  return released_bytes_;
}

bool MemoryPressureDecision::HasAction(MemoryPressureActionKind action) const {
  return std::find(actions.begin(), actions.end(), action) != actions.end();
}

const char* MemoryPressureStateName(MemoryPressureState state) {
  switch (state) {
    case MemoryPressureState::normal: return "NORMAL";
    case MemoryPressureState::soft_pressure: return "SOFT_PRESSURE";
    case MemoryPressureState::high_pressure: return "HIGH_PRESSURE";
    case MemoryPressureState::emergency_pressure: return "EMERGENCY_PRESSURE";
    case MemoryPressureState::recovery: return "RECOVERY";
  }
  return "UNKNOWN";
}

const char* MemoryPressureTransitionTriggerName(
    MemoryPressureTransitionTrigger trigger) {
  switch (trigger) {
    case MemoryPressureTransitionTrigger::none: return "none";
    case MemoryPressureTransitionTrigger::pressure_percent: return "pressure_percent";
    case MemoryPressureTransitionTrigger::soft_threshold: return "soft_threshold";
    case MemoryPressureTransitionTrigger::high_threshold: return "high_threshold";
    case MemoryPressureTransitionTrigger::hard_limit_exceeded: return "hard_limit_exceeded";
    case MemoryPressureTransitionTrigger::emergency_threshold: return "emergency_threshold";
    case MemoryPressureTransitionTrigger::host_memory_pressure: return "host_memory_pressure";
    case MemoryPressureTransitionTrigger::container_memory_pressure: return "container_memory_pressure";
    case MemoryPressureTransitionTrigger::linux_cgroup_memory_event: return "linux_cgroup_memory_event";
    case MemoryPressureTransitionTrigger::windows_job_object_pressure: return "windows_job_object_pressure";
    case MemoryPressureTransitionTrigger::recovery_pressure_drop: return "recovery_pressure_drop";
    case MemoryPressureTransitionTrigger::recovery_stability_window: return "recovery_stability_window";
  }
  return "unknown";
}

const char* MemoryPressureActionKindName(MemoryPressureActionKind action) {
  switch (action) {
    case MemoryPressureActionKind::none: return "none";
    case MemoryPressureActionKind::throttle: return "throttle";
    case MemoryPressureActionKind::prefer_spill: return "prefer_spill";
    case MemoryPressureActionKind::shrink_page_cache: return "shrink_page_cache";
    case MemoryPressureActionKind::background_cleanup: return "background_cleanup";
    case MemoryPressureActionKind::cancel_query: return "cancel_query";
    case MemoryPressureActionKind::refuse_allocation: return "refuse_allocation";
    case MemoryPressureActionKind::emergency_reserve_release: return "emergency_reserve_release";
    case MemoryPressureActionKind::emergency_diagnostics: return "emergency_diagnostics";
    case MemoryPressureActionKind::emergency_admission_shutdown: return "emergency_admission_shutdown";
    case MemoryPressureActionKind::suspend_noncritical_agents_jobs: return "suspend_noncritical_agents_jobs";
    case MemoryPressureActionKind::block_large_grants: return "block_large_grants";
    case MemoryPressureActionKind::recovery_readmission_throttling: return "recovery_readmission_throttling";
    case MemoryPressureActionKind::adaptive_batch_reduction: return "adaptive_batch_reduction";
    case MemoryPressureActionKind::forced_spill: return "forced_spill";
    case MemoryPressureActionKind::forced_cancel: return "forced_cancel";
  }
  return "unknown";
}

MemoryPressureActionExecutionResult MemoryPressureActionExecutorOk(
    MemoryPressureActionKind action,
    std::string executor_name,
    std::vector<std::string> evidence) {
  MemoryPressureActionExecutionResult result;
  result.status = MemoryOkStatus();
  result.fail_closed = false;
  result.action = action;
  result.executor_name = std::move(executor_name);
  result.diagnostic = MemoryPressureDiagnostic(
      result.status,
      "memory_pressure_action_executed",
      "memory.pressure.action_executed",
      {{"action", MemoryPressureActionKindName(action)},
       {"executor_name", result.executor_name}});
  result.evidence.push_back("memory_pressure.action_executor.action=" +
                            std::string(MemoryPressureActionKindName(action)));
  result.evidence.push_back("memory_pressure.action_executor.name=" +
                            result.executor_name);
  result.evidence.push_back("memory_pressure.action_executor.fail_closed=false");
  result.evidence.insert(result.evidence.end(),
                         std::make_move_iterator(evidence.begin()),
                         std::make_move_iterator(evidence.end()));
  return result;
}

MemoryPressureActionExecutionResult MemoryPressureActionExecutorFailClosed(
    MemoryPressureActionKind action,
    std::string executor_name,
    std::string diagnostic_code,
    std::string detail,
    std::vector<std::string> evidence) {
  MemoryPressureActionExecutionResult result;
  result.status = MemoryPressureExecutorErrorStatus();
  result.fail_closed = true;
  result.action = action;
  result.executor_name = std::move(executor_name);
  result.diagnostic = MemoryPressureDiagnostic(
      result.status,
      std::move(diagnostic_code),
      "memory.pressure.action_executor_failed",
      {{"action", MemoryPressureActionKindName(action)},
       {"executor_name", result.executor_name},
       {"detail", std::move(detail)}});
  result.evidence.push_back("memory_pressure.action_executor.action=" +
                            std::string(MemoryPressureActionKindName(action)));
  result.evidence.push_back("memory_pressure.action_executor.name=" +
                            result.executor_name);
  result.evidence.push_back("memory_pressure.action_executor.fail_closed=true");
  result.evidence.push_back("memory_pressure.action_executor.diagnostic_code=" +
                            result.diagnostic.diagnostic_code);
  result.evidence.insert(result.evidence.end(),
                         std::make_move_iterator(evidence.begin()),
                         std::make_move_iterator(evidence.end()));
  return result;
}

MemoryPressureExecutorResult ExecuteMemoryPressureDecision(
    const MemoryPressureDecision& decision,
    const MemoryPressureActionExecutorSet& executors) {
  if (!decision.ok() || decision.fail_closed) {
    auto result = ExecutorFailure(
        decision,
        "memory_pressure_executor_input_refused",
        "memory.pressure.executor_input_refused",
        {{"decision_diagnostic", decision.diagnostic.diagnostic_code},
         {"decision_fail_closed", decision.fail_closed ? "true" : "false"}});
    AddExecutorSummaryEvidence(&result);
    return result;
  }
  if (decision.actions.empty()) {
    auto result = ExecutorFailure(
        decision,
        "memory_pressure_executor_actions_required",
        "memory.pressure.executor_actions_required");
    AddExecutorSummaryEvidence(&result);
    return result;
  }

  MemoryPressureExecutorResult result;
  result.status = MemoryOkStatus();
  result.fail_closed = false;
  result.diagnostic = MemoryPressureDiagnostic(
      result.status,
      "memory_pressure_actions_executed",
      "memory.pressure.actions_executed",
      {{"action_count", std::to_string(decision.actions.size())}});
  AddExecutorBaseEvidence(&result, decision);

  for (const auto action : decision.actions) {
    if (action == MemoryPressureActionKind::none) {
      continue;
    }
    const MemoryPressureActionExecutor* executor =
        ExecutorForAction(executors, action);
    if (executor == nullptr || !*executor) {
      result.missing_executor_actions.push_back(action);
      result.evidence.push_back("memory_pressure.executor.missing_action=" +
                                std::string(MemoryPressureActionKindName(action)));
    }
  }

  if (!result.missing_executor_actions.empty()) {
    result.status = MemoryPressureExecutorErrorStatus();
    result.fail_closed = true;
    result.diagnostic = MemoryPressureDiagnostic(
        result.status,
        "memory_pressure_executor_missing",
        "memory.pressure.executor_missing",
        {{"missing_count",
          std::to_string(result.missing_executor_actions.size())}});
    AddExecutorSummaryEvidence(&result);
    return result;
  }

  for (const auto action : decision.actions) {
    if (action == MemoryPressureActionKind::none) {
      result.evidence.push_back("memory_pressure.executor.noop_action=none");
      continue;
    }
    const MemoryPressureActionExecutor* executor =
        ExecutorForAction(executors, action);
    MemoryPressureActionExecutionResult step = (*executor)(decision, action);
    result.evidence.insert(result.evidence.end(),
                           step.evidence.begin(),
                           step.evidence.end());
    if (!step.ok()) {
      result.status = step.status.ok() ? MemoryPressureExecutorErrorStatus()
                                       : step.status;
      result.fail_closed = true;
      result.failed_action = action;
      result.diagnostic = MemoryPressureDiagnostic(
          result.status,
          step.diagnostic.diagnostic_code.empty()
              ? "memory_pressure_executor_action_failed"
              : step.diagnostic.diagnostic_code,
          "memory.pressure.executor_action_failed",
          {{"action", MemoryPressureActionKindName(action)},
           {"executor_name", step.executor_name}});
      result.evidence.push_back("memory_pressure.executor.failed_action=" +
                                std::string(MemoryPressureActionKindName(action)));
      AddExecutorSummaryEvidence(&result);
      return result;
    }
    MarkExecuted(&result, action);
  }

  AddExecutorSummaryEvidence(&result);
  return result;
}

MemoryPressureDecision PlanMemoryPressureResponse(
    const MemoryPressurePolicy& policy,
    const MemoryPressureObservation& observation) {
  return PlanMemoryPressureResponse(policy, observation, nullptr);
}

MemoryPressureDecision PlanMemoryPressureResponse(
    const MemoryPressurePolicy& policy,
    const MemoryPressureObservation& observation,
    EmergencyMemoryReserve* reserve) {
  if (policy.require_route_label && observation.route_label.empty()) {
    return ErrorDecision(
        "memory_pressure_missing_route_label",
        "memory.pressure.missing_route_label",
        {{"operation_id", observation.operation_id}});
  }
  if (policy.require_engine_mga_authority &&
      !observation.engine_mga_authoritative) {
    return ErrorDecision(
        "memory_pressure_missing_engine_mga_authority",
        "memory.pressure.missing_engine_mga_authority",
        {{"route_label", observation.route_label}});
  }
  if (observation.parser_or_reference_authority ||
      observation.client_authority ||
      observation.provider_authority ||
      observation.wal_authority ||
      observation.transaction_finality_authority ||
      observation.visibility_authority ||
      observation.security_authority ||
      observation.recovery_authority ||
      observation.benchmark_authority ||
      observation.optimizer_plan_authority ||
      observation.index_finality_authority ||
      observation.agent_action_authority) {
    return ErrorDecision(
        "memory_pressure_unsafe_authority",
        "memory.pressure.unsafe_authority",
        {{"parser_or_reference_authority", observation.parser_or_reference_authority ? "true" : "false"},
         {"client_authority", observation.client_authority ? "true" : "false"},
         {"provider_authority", observation.provider_authority ? "true" : "false"},
         {"wal_authority", observation.wal_authority ? "true" : "false"},
         {"transaction_finality_authority", observation.transaction_finality_authority ? "true" : "false"},
         {"visibility_authority", observation.visibility_authority ? "true" : "false"},
         {"security_authority", observation.security_authority ? "true" : "false"},
         {"recovery_authority", observation.recovery_authority ? "true" : "false"},
         {"benchmark_authority", observation.benchmark_authority ? "true" : "false"},
         {"optimizer_plan_authority", observation.optimizer_plan_authority ? "true" : "false"},
         {"index_finality_authority", observation.index_finality_authority ? "true" : "false"},
         {"agent_action_authority", observation.agent_action_authority ? "true" : "false"}});
  }
  if (policy.require_mga_security_recheck_preservation &&
      (!observation.mga_recheck_preserved ||
       !observation.security_recheck_preserved)) {
    auto decision = ErrorDecision(
        "memory_pressure_recheck_not_preserved",
        "memory.pressure.recheck_not_preserved",
        {{"mga_recheck_preserved", observation.mga_recheck_preserved ? "true" : "false"},
         {"security_recheck_preserved", observation.security_recheck_preserved ? "true" : "false"}});
    decision.evidence.push_back("memory_pressure.mga_recheck_preserved=" +
                                BoolString(observation.mga_recheck_preserved));
    decision.evidence.push_back("memory_pressure.security_recheck_preserved=" +
                                BoolString(observation.security_recheck_preserved));
    return decision;
  }

  const u64 budget_pressure_percent = BudgetPressurePercent(observation);

  MemoryPressureDecision decision;
  decision.status = MemoryOkStatus();
  decision.previous_state = observation.previous_state;
  decision.new_state =
      SelectPressureState(policy, observation, budget_pressure_percent);
  decision.trigger =
      SelectTrigger(decision.new_state, policy, observation,
                    budget_pressure_percent);
  decision.pressure_percent = SaturatingMaxPressure(observation);
  decision.soft_threshold_bytes = observation.soft_limit_bytes;
  decision.hard_threshold_bytes = observation.hard_limit_bytes;
  decision.emergency_threshold_bytes = EmergencyThresholdBytes(observation);
  decision.recovery_readmission_limit = policy.recovery_readmission_per_tick;
  decision.top_contexts =
      SortedTopContexts(observation, policy.max_emergency_top_contexts);
  decision.affected_scopes = observation.affected_scopes;
  if (reserve != nullptr) {
    decision.emergency_reserve = reserve->Snapshot();
  }
  decision.diagnostic = MemoryPressureDiagnostic(
      decision.status,
      "memory_pressure_response_planned",
      "memory.pressure.response_planned",
      {{"route_label", observation.route_label},
       {"operation_id", observation.operation_id},
       {"pressure_percent", std::to_string(decision.pressure_percent)},
       {"previous_state", MemoryPressureStateName(decision.previous_state)},
       {"new_state", MemoryPressureStateName(decision.new_state)},
       {"trigger", MemoryPressureTransitionTriggerName(decision.trigger)}});
  decision.evidence.push_back("MMCH_ADAPTIVE_MEMORY_PRESSURE_RESPONSE");
  decision.evidence.push_back(kCeic017EvidenceAnchor);
  decision.evidence.push_back("memory_pressure.route_label=" + observation.route_label);
  decision.evidence.push_back("memory_pressure.operation_id=" + observation.operation_id);
  decision.evidence.push_back("memory_pressure.pressure_percent=" +
                              std::to_string(decision.pressure_percent));
  decision.evidence.push_back("memory_pressure.budget_pressure_percent=" +
                              std::to_string(budget_pressure_percent));
  decision.evidence.push_back("memory_pressure.previous_state=" +
                              std::string(MemoryPressureStateName(decision.previous_state)));
  decision.evidence.push_back("memory_pressure.new_state=" +
                              std::string(MemoryPressureStateName(decision.new_state)));
  decision.evidence.push_back("memory_pressure.trigger=" +
                              std::string(MemoryPressureTransitionTriggerName(decision.trigger)));
  decision.evidence.push_back(kLegacyAuthorityBoundary);
  decision.evidence.push_back(kExpandedAuthorityBoundary);
  decision.evidence.push_back("memory_pressure.engine_mga_authoritative=" +
                              BoolString(observation.engine_mga_authoritative));
  decision.evidence.push_back("memory_pressure.mga_recheck_preserved=" +
                              BoolString(observation.mga_recheck_preserved));
  decision.evidence.push_back("memory_pressure.security_recheck_preserved=" +
                              BoolString(observation.security_recheck_preserved));
  decision.evidence.push_back(
      "memory_pressure.security_authorization_remains_external_recheck=true");
  decision.evidence.push_back(
      "memory_pressure.cluster_pressure_coordination=out_of_scope_external_provider_fail_closed");

  AddObservationEvidence(&decision, observation);
  AddTopContextEvidence(&decision);

  if (decision.new_state == MemoryPressureState::emergency_pressure) {
    decision.ordinary_admission_allowed = false;
    if (policy.enable_emergency_admission_shutdown) {
      AddAction(&decision, MemoryPressureActionKind::emergency_admission_shutdown);
    }
    if (reserve != nullptr) {
      const u64 released_bytes = reserve->ReleaseForEmergencyDiagnostics();
      if (released_bytes != 0) {
        decision.emergency_reserve_released = true;
        decision.emergency_reserve_released_bytes = released_bytes;
        AddAction(&decision, MemoryPressureActionKind::emergency_reserve_release);
      }
      decision.emergency_reserve = reserve->Snapshot();
    }
    if (observation.emergency_diagnostics_supported) {
      AddAction(&decision, MemoryPressureActionKind::emergency_diagnostics);
      decision.emergency_diagnostics =
          BuildEmergencyDiagnosticEvidence(policy, observation, decision);
    }
    AddAction(&decision, MemoryPressureActionKind::refuse_allocation);
  }

  if (decision.new_state == MemoryPressureState::recovery) {
    decision.ordinary_admission_allowed = true;
    decision.recovery_readmission_throttled =
        observation.pending_readmission_count >
            policy.recovery_readmission_per_tick ||
        observation.stable_recovery_observation_count <
            policy.recovery_required_stable_observations;
    AddAction(&decision,
              MemoryPressureActionKind::recovery_readmission_throttling);
    decision.evidence.push_back("memory_pressure.recovery.pending_readmission_count=" +
                                std::to_string(observation.pending_readmission_count));
    decision.evidence.push_back("memory_pressure.recovery.readmission_limit=" +
                                std::to_string(decision.recovery_readmission_limit));
    decision.evidence.push_back("memory_pressure.recovery.stable_observation_count=" +
                                std::to_string(observation.stable_recovery_observation_count));
    decision.evidence.push_back("memory_pressure.recovery.throttled=" +
                                BoolString(decision.recovery_readmission_throttled));
  }

  if (AtLeast(decision.new_state, MemoryPressureState::soft_pressure) &&
      decision.new_state != MemoryPressureState::recovery) {
    if (observation.adaptive_batch_reduction_supported) {
      AddAction(&decision, MemoryPressureActionKind::adaptive_batch_reduction);
    }
    AddAction(&decision, MemoryPressureActionKind::throttle);
  }

  if (AtLeast(decision.new_state, MemoryPressureState::soft_pressure) &&
      decision.new_state != MemoryPressureState::recovery &&
      observation.background_cleanup_supported &&
      observation.reclaimable_background_bytes != 0) {
    AddAction(&decision, MemoryPressureActionKind::background_cleanup);
  }

  if (decision.pressure_percent >= policy.spill_pressure_percent &&
      observation.spill_supported &&
      decision.new_state != MemoryPressureState::recovery) {
    AddAction(&decision, MemoryPressureActionKind::prefer_spill);
  }
  if (AtLeast(decision.new_state, MemoryPressureState::high_pressure) &&
      decision.new_state != MemoryPressureState::recovery &&
      (observation.forced_spill_supported || observation.spill_supported)) {
    AddAction(&decision, MemoryPressureActionKind::block_large_grants);
    AddAction(&decision, MemoryPressureActionKind::forced_spill);
  }
  if (budget_pressure_percent >= policy.cancel_pressure_percent &&
      observation.cancellation_supported &&
      decision.new_state != MemoryPressureState::recovery) {
    AddAction(&decision, MemoryPressureActionKind::cancel_query);
  }
  if (AtLeast(decision.new_state, MemoryPressureState::high_pressure) &&
      decision.new_state != MemoryPressureState::recovery &&
      observation.low_priority_cancellation_supported &&
      (observation.low_priority_query_count != 0 ||
       observation.low_priority_session_count != 0)) {
    AddAction(&decision, MemoryPressureActionKind::cancel_query);
    if (observation.forced_cancel_supported ||
        decision.new_state == MemoryPressureState::emergency_pressure) {
      AddAction(&decision, MemoryPressureActionKind::forced_cancel);
    }
    decision.evidence.push_back("memory_pressure.low_priority_query_count=" +
                                std::to_string(observation.low_priority_query_count));
    decision.evidence.push_back("memory_pressure.low_priority_session_count=" +
                                std::to_string(observation.low_priority_session_count));
  }
  if (AtLeast(decision.new_state, MemoryPressureState::high_pressure) &&
      decision.new_state != MemoryPressureState::recovery &&
      observation.noncritical_agent_suspend_supported) {
    AddAction(&decision,
              MemoryPressureActionKind::suspend_noncritical_agents_jobs);
  }
  if (decision.pressure_percent >= policy.page_cache_shrink_percent &&
      observation.page_cache_shrink_supported &&
      observation.page_cache_target_bytes != 0 &&
      observation.page_cache_resident_bytes > observation.page_cache_target_bytes) {
    AddAction(&decision, MemoryPressureActionKind::shrink_page_cache);
  }
  if (decision.pressure_percent >= policy.cleanup_pressure_percent &&
      observation.background_cleanup_supported &&
      observation.reclaimable_background_bytes != 0) {
    AddAction(&decision, MemoryPressureActionKind::background_cleanup);
  }
  if (HardLimitExceeded(observation) ||
      budget_pressure_percent >= policy.refuse_pressure_percent ||
      decision.new_state == MemoryPressureState::emergency_pressure) {
    AddAction(&decision, MemoryPressureActionKind::refuse_allocation);
  }

  if (decision.actions.empty()) {
    AddAction(&decision, MemoryPressureActionKind::none);
  }

  decision.evidence.push_back("memory_pressure.ordinary_admission_allowed=" +
                              BoolString(decision.ordinary_admission_allowed));
  decision.evidence.push_back("memory_pressure.emergency_reserve.configured_bytes=" +
                              std::to_string(decision.emergency_reserve.configured_bytes));
  decision.evidence.push_back("memory_pressure.emergency_reserve.available_bytes=" +
                              std::to_string(decision.emergency_reserve.available_bytes));
  decision.evidence.push_back("memory_pressure.emergency_reserve.released_bytes=" +
                              std::to_string(decision.emergency_reserve.released_bytes));
  decision.evidence.push_back("memory_pressure.emergency_reserve.released=" +
                              BoolString(decision.emergency_reserve_released));
  if (decision.emergency_diagnostics.emitted) {
    AddEmergencyDiagnosticEvidence(&decision);
  }
  AddActionEvidence(&decision);
  return decision;
}

}  // namespace scratchbird::core::memory

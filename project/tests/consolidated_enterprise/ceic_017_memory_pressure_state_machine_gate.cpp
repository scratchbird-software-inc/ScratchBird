// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// CEIC-017 focused validation for memory pressure state transitions,
// hard-OOM survival modeling, and emergency diagnostic evidence.
#include "memory_pressure_response.hpp"

#include <cstdlib>
#include <iostream>
#include <string_view>
#include <vector>

namespace {

namespace memory = scratchbird::core::memory;

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view message) {
  if (!condition) {
    Fail(message);
  }
}

bool Contains(const std::vector<std::string>& evidence,
              std::string_view token) {
  for (const auto& row : evidence) {
    if (row.find(token) != std::string::npos) {
      return true;
    }
  }
  return false;
}

memory::MemoryPressurePolicy Policy() {
  memory::MemoryPressurePolicy policy;
  policy.soft_pressure_percent = 70;
  policy.high_pressure_percent = 85;
  policy.emergency_pressure_percent = 95;
  policy.refuse_pressure_percent = 100;
  policy.recovery_required_stable_observations = 2;
  policy.recovery_readmission_per_tick = 1;
  policy.max_emergency_diagnostic_rows = 10;
  policy.max_emergency_top_contexts = 3;
  return policy;
}

memory::MemoryPressureObservation BaseObservation() {
  memory::MemoryPressureObservation observation;
  observation.route_label = "engine.memory.ceic_017";
  observation.operation_id = "CEIC-017";
  observation.current_bytes = 256;
  observation.soft_limit_bytes = 700;
  observation.hard_limit_bytes = 1000;
  observation.emergency_limit_bytes = 950;
  observation.unified_budget_bytes = 256;
  observation.unified_budget_limit_bytes = 1000;
  observation.engine_mga_authoritative = true;
  observation.mga_recheck_preserved = true;
  observation.security_recheck_preserved = true;
  observation.top_contexts = {
      {"query", "q-low-priority", 384, 512, 5, true, true, true, true},
      {"page_cache", "clean-cache", 256, 768, 10, false, false, true, true},
      {"background", "stats-maintenance", 128, 256, 9, true, true, true, true},
      {"diagnostics", "bounded-emergency-log", 64, 64, 1, false, false, true, true}};
  observation.affected_scopes = {
      "process:ceic-017",
      "database:ceic-017-db",
      "session:ceic-017-low-priority"};
  return observation;
}

void RequireTransitionEvidence(const memory::MemoryPressureDecision& decision,
                               std::string_view new_state) {
  Require(Contains(decision.evidence,
                   "CEIC-017_MEMORY_PRESSURE_STATE_MACHINE"),
          "CEIC-017 evidence anchor missing");
  Require(Contains(decision.evidence, "memory_pressure.previous_state="),
          "CEIC-017 previous state evidence missing");
  Require(Contains(decision.evidence, std::string("memory_pressure.new_state=") +
                                      std::string(new_state)),
          "CEIC-017 new state evidence missing");
  Require(Contains(decision.evidence, "memory_pressure.trigger="),
          "CEIC-017 trigger evidence missing");
  Require(Contains(decision.evidence, "memory_pressure.current_bytes="),
          "CEIC-017 current byte evidence missing");
  Require(Contains(decision.evidence, "memory_pressure.threshold.soft_bytes="),
          "CEIC-017 soft threshold evidence missing");
  Require(Contains(decision.evidence, "memory_pressure.threshold.hard_bytes="),
          "CEIC-017 hard threshold evidence missing");
  Require(Contains(decision.evidence,
                   "memory_pressure.threshold.emergency_bytes="),
          "CEIC-017 emergency threshold evidence missing");
  Require(Contains(decision.evidence,
                   "memory_pressure.top_context.0.scope_id="),
          "CEIC-017 top context evidence missing");
  Require(Contains(decision.evidence,
                   "memory_pressure.affected_scope=process:ceic-017"),
          "CEIC-017 affected scope evidence missing");
  Require(Contains(decision.evidence,
                   "memory_pressure.authority_scope=evidence_only_not_transaction_finality_visibility_security_authorization_recovery_parser_donor_wal_benchmark_optimizer_plan_index_finality_or_agent_action_authority"),
          "CEIC-017 expanded authority boundary missing");
  Require(Contains(decision.evidence,
                   "memory_pressure.mga_recheck_preserved=true"),
          "CEIC-017 MGA recheck preservation missing");
  Require(Contains(decision.evidence,
                   "memory_pressure.security_recheck_preserved=true"),
          "CEIC-017 security recheck preservation missing");
}

void SyntheticStateTransitions() {
  auto observation = BaseObservation();
  auto decision =
      memory::PlanMemoryPressureResponse(Policy(), observation);
  Require(decision.ok(), "CEIC-017 normal transition failed");
  Require(decision.new_state == memory::MemoryPressureState::normal,
          "CEIC-017 normal state not selected");
  Require(decision.HasAction(memory::MemoryPressureActionKind::none),
          "CEIC-017 normal state should plan no action");
  RequireTransitionEvidence(decision, "NORMAL");

  observation = BaseObservation();
  observation.previous_state = memory::MemoryPressureState::normal;
  observation.current_bytes = 760;
  observation.unified_budget_bytes = 760;
  observation.spill_supported = true;
  observation.background_cleanup_supported = true;
  observation.reclaimable_background_bytes = 64;
  decision = memory::PlanMemoryPressureResponse(Policy(), observation);
  Require(decision.ok(), "CEIC-017 soft transition failed");
  Require(decision.new_state == memory::MemoryPressureState::soft_pressure,
          "CEIC-017 soft state not selected");
  Require(decision.trigger == memory::MemoryPressureTransitionTrigger::soft_threshold,
          "CEIC-017 soft trigger mismatch");
  RequireTransitionEvidence(decision, "SOFT_PRESSURE");

  observation.previous_state = decision.new_state;
  observation.current_bytes = 880;
  observation.unified_budget_bytes = 880;
  observation.page_cache_resident_bytes = 4096;
  observation.page_cache_target_bytes = 2048;
  observation.page_cache_shrink_supported = true;
  decision = memory::PlanMemoryPressureResponse(Policy(), observation);
  Require(decision.ok(), "CEIC-017 high transition failed");
  Require(decision.new_state == memory::MemoryPressureState::high_pressure,
          "CEIC-017 high state not selected");
  RequireTransitionEvidence(decision, "HIGH_PRESSURE");

  observation.previous_state = decision.new_state;
  observation.current_bytes = 980;
  observation.unified_budget_bytes = 980;
  memory::EmergencyMemoryReserve reserve(4096);
  decision = memory::PlanMemoryPressureResponse(Policy(), observation, &reserve);
  Require(decision.ok(), "CEIC-017 emergency transition failed");
  Require(decision.new_state ==
              memory::MemoryPressureState::emergency_pressure,
          "CEIC-017 emergency state not selected");
  RequireTransitionEvidence(decision, "EMERGENCY_PRESSURE");

  observation = BaseObservation();
  observation.previous_state = memory::MemoryPressureState::emergency_pressure;
  observation.current_bytes = 400;
  observation.unified_budget_bytes = 400;
  observation.pending_readmission_count = 5;
  decision = memory::PlanMemoryPressureResponse(Policy(), observation);
  Require(decision.ok(), "CEIC-017 recovery transition failed");
  Require(decision.new_state == memory::MemoryPressureState::recovery,
          "CEIC-017 recovery state not selected");
  RequireTransitionEvidence(decision, "RECOVERY");
}

void SoftPressurePlansSpillThrottleAndBackgroundAction() {
  auto observation = BaseObservation();
  observation.current_bytes = 760;
  observation.unified_budget_bytes = 760;
  observation.spill_supported = true;
  observation.background_cleanup_supported = true;
  observation.reclaimable_background_bytes = 256;

  const auto decision =
      memory::PlanMemoryPressureResponse(Policy(), observation);
  Require(decision.ok(), "CEIC-017 soft pressure decision failed");
  Require(decision.new_state == memory::MemoryPressureState::soft_pressure,
          "CEIC-017 soft pressure state mismatch");
  Require(decision.HasAction(memory::MemoryPressureActionKind::throttle),
          "CEIC-017 soft pressure missing throttle");
  Require(decision.HasAction(memory::MemoryPressureActionKind::prefer_spill),
          "CEIC-017 soft pressure missing spill preference");
  Require(decision.HasAction(
              memory::MemoryPressureActionKind::background_cleanup),
          "CEIC-017 soft pressure missing background cleanup");
  Require(decision.HasAction(
              memory::MemoryPressureActionKind::adaptive_batch_reduction),
          "CEIC-017 soft pressure missing adaptive batch reduction");
  Require(!decision.HasAction(
              memory::MemoryPressureActionKind::refuse_allocation),
          "CEIC-017 soft pressure refused allocation too early");
  RequireTransitionEvidence(decision, "SOFT_PRESSURE");
}

void HighPressureShrinksCacheAndCancelsLowPriorityWork() {
  auto observation = BaseObservation();
  observation.current_bytes = 880;
  observation.unified_budget_bytes = 880;
  observation.page_cache_resident_bytes = 8192;
  observation.page_cache_target_bytes = 2048;
  observation.spill_supported = true;
  observation.forced_spill_supported = true;
  observation.page_cache_shrink_supported = true;
  observation.low_priority_query_count = 2;
  observation.low_priority_session_count = 1;
  observation.low_priority_cancellation_supported = true;
  observation.forced_cancel_supported = true;
  observation.noncritical_agent_suspend_supported = true;

  const auto decision =
      memory::PlanMemoryPressureResponse(Policy(), observation);
  Require(decision.ok(), "CEIC-017 high pressure decision failed");
  Require(decision.new_state == memory::MemoryPressureState::high_pressure,
          "CEIC-017 high pressure state mismatch");
  Require(decision.HasAction(
              memory::MemoryPressureActionKind::shrink_page_cache),
          "CEIC-017 high pressure missing page-cache shrink");
  Require(decision.HasAction(memory::MemoryPressureActionKind::cancel_query),
          "CEIC-017 high pressure missing low-priority cancellation");
  Require(decision.HasAction(memory::MemoryPressureActionKind::forced_spill),
          "CEIC-017 high pressure missing forced spill");
  Require(decision.HasAction(
              memory::MemoryPressureActionKind::block_large_grants),
          "CEIC-017 high pressure missing large-grant block");
  Require(decision.HasAction(
              memory::MemoryPressureActionKind::suspend_noncritical_agents_jobs),
          "CEIC-017 high pressure missing noncritical agent/job suspension");
  Require(!decision.HasAction(
              memory::MemoryPressureActionKind::emergency_reserve_release),
          "CEIC-017 high pressure released emergency reserve too early");
  Require(decision.ordinary_admission_allowed,
          "CEIC-017 high pressure shut down admission too early");
  RequireTransitionEvidence(decision, "HIGH_PRESSURE");
}

void EmergencyReserveAndDiagnosticsAreBounded() {
  auto observation = BaseObservation();
  observation.current_bytes = 980;
  observation.unified_budget_bytes = 980;
  observation.host_pressure.observed = true;
  observation.host_pressure.pressure = true;
  observation.host_pressure.current_bytes = 980;
  observation.host_pressure.total_bytes = 1000;
  observation.host_pressure.available_bytes = 20;
  observation.host_pressure.pressure_percent = 98;
  observation.container_pressure.observed = true;
  observation.container_pressure.pressure = true;
  observation.container_pressure.current_bytes = 980;
  observation.container_pressure.limit_bytes = 1000;
  observation.container_pressure.pressure_percent = 98;
  observation.linux_cgroup.observed = true;
  observation.linux_cgroup.current_bytes = 980;
  observation.linux_cgroup.max_bytes = 1000;
  observation.linux_cgroup.high_events = 1;
  observation.linux_cgroup.max_events = 1;
  observation.linux_cgroup.oom_events = 1;
  observation.linux_cgroup.oom_event = true;
  observation.windows_job.observed = true;
  observation.windows_job.pressure = true;
  observation.windows_job.job_memory_bytes = 1000;
  observation.windows_job.job_memory_limit_bytes = 1000;
  observation.windows_job.limit_violation = true;
  observation.low_priority_query_count = 2;
  observation.low_priority_cancellation_supported = true;
  observation.forced_cancel_supported = true;
  observation.noncritical_agent_suspend_supported = true;

  memory::EmergencyMemoryReserve reserve(4096);
  const auto decision =
      memory::PlanMemoryPressureResponse(Policy(), observation, &reserve);

  Require(decision.ok(), "CEIC-017 emergency pressure decision failed");
  Require(decision.new_state ==
              memory::MemoryPressureState::emergency_pressure,
          "CEIC-017 emergency pressure state mismatch");
  Require(!decision.ordinary_admission_allowed,
          "CEIC-017 emergency pressure admitted ordinary work");
  Require(decision.HasAction(
              memory::MemoryPressureActionKind::emergency_admission_shutdown),
          "CEIC-017 emergency admission shutdown action missing");
  Require(decision.HasAction(
              memory::MemoryPressureActionKind::emergency_reserve_release),
          "CEIC-017 emergency reserve release action missing");
  Require(decision.HasAction(
              memory::MemoryPressureActionKind::emergency_diagnostics),
          "CEIC-017 emergency diagnostics action missing");
  Require(decision.HasAction(
              memory::MemoryPressureActionKind::refuse_allocation),
          "CEIC-017 emergency allocation refusal missing");
  Require(decision.emergency_reserve_released &&
              decision.emergency_reserve_released_bytes == 4096,
          "CEIC-017 emergency reserve was not released exactly once");
  Require(reserve.Snapshot().available_bytes == 0,
          "CEIC-017 emergency reserve still available after release");
  Require(decision.emergency_diagnostics.emitted &&
              decision.emergency_diagnostics.bounded &&
              decision.emergency_diagnostics.allocation_free_logger &&
              decision.emergency_diagnostics.row_count <=
                  decision.emergency_diagnostics.max_rows,
          "CEIC-017 emergency diagnostics were not bounded/allocation-free");
  Require(Contains(decision.evidence,
                   "memory_pressure.linux_cgroup.oom_events=1"),
          "CEIC-017 cgroup memory-event evidence missing");
  Require(Contains(decision.evidence,
                   "memory_pressure.windows_job.limit_violation=true"),
          "CEIC-017 Windows job-object evidence missing");
  Require(Contains(decision.evidence,
                   "full_support_bundle_deferred_to_CEIC_023=true"),
          "CEIC-017 overclaimed full support-bundle closure");
  RequireTransitionEvidence(decision, "EMERGENCY_PRESSURE");
}

void UnsafeAuthorityFailsClosed() {
  auto observation = BaseObservation();
  observation.security_authority = true;
  auto decision = memory::PlanMemoryPressureResponse(Policy(), observation);
  Require(!decision.ok() && decision.fail_closed,
          "CEIC-017 unsafe security authority did not fail closed");
  Require(decision.diagnostic.diagnostic_code ==
              "memory_pressure_unsafe_authority",
          "CEIC-017 unsafe authority diagnostic changed");

  observation = BaseObservation();
  observation.mga_recheck_preserved = false;
  decision = memory::PlanMemoryPressureResponse(Policy(), observation);
  Require(!decision.ok() && decision.fail_closed,
          "CEIC-017 missing MGA recheck did not fail closed");
  Require(decision.diagnostic.diagnostic_code ==
              "memory_pressure_recheck_not_preserved",
          "CEIC-017 missing recheck diagnostic changed");
}

void RecoveryDoesNotReadmitTooFast() {
  auto observation = BaseObservation();
  observation.previous_state = memory::MemoryPressureState::emergency_pressure;
  observation.current_bytes = 320;
  observation.unified_budget_bytes = 320;
  observation.pending_readmission_count = 8;
  observation.stable_recovery_observation_count = 0;

  auto decision = memory::PlanMemoryPressureResponse(Policy(), observation);
  Require(decision.ok(), "CEIC-017 recovery decision failed");
  Require(decision.new_state == memory::MemoryPressureState::recovery,
          "CEIC-017 recovery state not retained");
  Require(decision.HasAction(
              memory::MemoryPressureActionKind::recovery_readmission_throttling),
          "CEIC-017 recovery readmission throttle missing");
  Require(decision.recovery_readmission_throttled,
          "CEIC-017 recovery was not throttled under pending readmission surge");
  Require(decision.recovery_readmission_limit == 1,
          "CEIC-017 recovery readmission limit changed");

  observation.previous_state = memory::MemoryPressureState::recovery;
  observation.pending_readmission_count = 1;
  observation.stable_recovery_observation_count = 2;
  decision = memory::PlanMemoryPressureResponse(Policy(), observation);
  Require(decision.ok(), "CEIC-017 stable recovery decision failed");
  Require(decision.new_state == memory::MemoryPressureState::normal,
          "CEIC-017 stable recovery did not return to normal");
  Require(decision.HasAction(memory::MemoryPressureActionKind::none),
          "CEIC-017 stable recovery should not retain pressure actions");
}

}  // namespace

int main() {
  std::cout << "CEIC-017 authority_note=memory_pressure_evidence_only;"
               "not_transaction_finality_visibility_security_authorization_"
               "recovery_parser_donor_wal_benchmark_optimizer_plan_index_"
               "finality_or_agent_action_authority"
            << '\n';
  SyntheticStateTransitions();
  SoftPressurePlansSpillThrottleAndBackgroundAction();
  HighPressureShrinksCacheAndCancelsLowPriorityWork();
  EmergencyReserveAndDiagnosticsAreBounded();
  UnsafeAuthorityFailsClosed();
  RecoveryDoesNotReadmitTooFast();
  return EXIT_SUCCESS;
}

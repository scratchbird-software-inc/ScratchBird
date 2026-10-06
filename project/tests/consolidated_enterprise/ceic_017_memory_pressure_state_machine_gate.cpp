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
#include "metric_builtin_definitions.hpp"
#include "metric_label_key.hpp"
#include "metric_observation_queue.hpp"
#include "../support/binary_uuid_fixture.hpp"

#include <cstdlib>
#include <atomic>
#include <thread>
#include <iostream>
#include <limits>
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

void PressurePercentRetainsExactUint64Boundaries() {
  using memory::u64;
  constexpr u64 maximum = std::numeric_limits<u64>::max();
  // Exercise every byte-ratio source through the real planner. These are
  // numeric observations, not allocations of the represented byte counts.
  for (unsigned source = 0; source != 6; ++source) {
    auto check = [&](u64 current, u64 limit, u64 expected) {
      memory::MemoryPressureObservation observation;
      observation.route_label = "engine.memory.uint64_boundary";
      switch (source) {
        case 0: observation.current_bytes = current;
                observation.hard_limit_bytes = limit; break;
        case 1: observation.unified_budget_bytes = current;
                observation.unified_budget_limit_bytes = limit; break;
        case 2: observation.linux_cgroup.current_bytes = current;
                observation.linux_cgroup.max_bytes = limit; break;
        case 3: observation.windows_job.job_memory_bytes = current;
                observation.windows_job.job_memory_limit_bytes = limit; break;
        case 4: observation.page_cache_resident_bytes = current;
                observation.page_cache_target_bytes = limit; break;
        case 5: observation.current_bytes = current;
                observation.soft_limit_bytes = limit; break;
      }
      const auto decision = memory::PlanMemoryPressureResponse(Policy(), observation);
      Require(decision.ok() && decision.pressure_percent == expected,
              "uint64 pressure percentage lost exact boundary");
      // Cache occupancy feeds reclaim pressure rather than the process
      // emergency gate; all five budget inputs must preserve that gate.
      if (source != 4 && expected >= Policy().emergency_pressure_percent) {
        Require(decision.new_state == memory::MemoryPressureState::emergency_pressure &&
                    !decision.ordinary_admission_allowed &&
                    decision.HasAction(memory::MemoryPressureActionKind::refuse_allocation),
                "large byte count bypassed emergency admission denial");
      }
    };
    for (const u64 denominator : {u64{1}, u64{99}, u64{100}, u64{101},
                                  maximum / 100, maximum / 100 + 1,
                                  maximum - 99, maximum}) {
      check(0, denominator, 0);
      check(denominator, denominator, 100);
      check(maximum, denominator, 100);
      if (denominator < 100) continue;
      // Independent sequential threshold oracle; checks the exact byte
      // before and at every whole-percent transition, including overflow
      // edges in the original numerator-times-100 expression.
      u64 threshold = 0;
      u64 remainder = 0;
      for (u64 percent = 1; percent <= 99; ++percent) {
        threshold += denominator / 100;
        remainder += denominator % 100;
        threshold += remainder / 100;
        remainder %= 100;
        const auto ceiling = threshold + (remainder != 0);
        check(ceiling - 1, denominator, percent - 1);
        check(ceiling, denominator, percent);
      }
    }
    check(maximum, 0, 0);
  }
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
                   "memory_pressure.authority_scope=evidence_only_not_transaction_finality_visibility_security_authorization_recovery_parser_reference_wal_benchmark_optimizer_plan_index_finality_or_agent_action_authority"),
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
  Require(decision.emergency_diagnostics.requested &&
              !decision.emergency_diagnostics.emitted &&
              !decision.emergency_diagnostics.bounded &&
              !decision.emergency_diagnostics.allocation_free_logger &&
              !decision.emergency_diagnostics.redaction_before_buffering &&
              !decision.emergency_diagnostics.protected_material_excluded &&
              decision.emergency_diagnostics.row_count == 0 &&
              decision.emergency_diagnostics.planned_row_count <=
                  decision.emergency_diagnostics.max_rows,
          "CEIC-017 planning falsely claimed diagnostic emission or exceeded requested bound");
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

void AllDeclaredBinaryOwnershipScopesAreCharged() {
  memory::AllocationPolicy policy;
  policy.hard_limit_bytes = policy.byte_limit = 8192;
  policy.per_context_limit_bytes = 4096;
  memory::MemoryManager manager(policy);
  memory::MemoryTag tag;
  tag.category = memory::MemoryCategory::diagnostics;
  tag.lifetime = memory::MemoryLifetime::process;
  tag.purpose = "complete_binary_scope_conformance";
  constexpr auto count = static_cast<std::size_t>(memory::MemoryBinaryScopeKind::descriptor_snapshot) + 1;
  Require(tag.binary_ownership.scopes.size() == count,
          "binary ownership carrier omits declared process and downstream scopes");
  for (std::size_t i = 0; i < count; ++i) {
    auto& uuid = tag.binary_ownership[static_cast<memory::MemoryBinaryScopeKind>(i)];
    uuid[6] = 0x70;
    uuid[8] = 0x80;
    uuid[15] = static_cast<std::uint8_t>(i + 1);
  }
  {
    memory::EmergencyMemoryReserve reserve(manager, tag, 4096);
    const auto snapshot = manager.Snapshot();
    Require(snapshot.current_bytes == 4096,
            "scope attribution must not multiply physical backing charges");
    for (std::size_t i = 0; i < count; ++i) {
      const auto kind = static_cast<memory::MemoryBinaryScopeKind>(i);
      std::size_t matches = 0;
      for (const auto& context : snapshot.contexts) {
        if (context.binary_scope && context.binary_scope->kind == kind &&
            context.binary_scope->uuid == tag.binary_ownership[kind]) {
          ++matches;
          Require(context.current_bytes == 4096 && context.scope_id.empty(),
                  "each declared scope must retain exact binary charges");
        }
      }
      Require(matches == 1, "declared binary scope absent or duplicated in accounting");
    }
    auto sibling = tag;
    for (std::size_t i = 0; i < count; ++i) {
      const auto kind = static_cast<memory::MemoryBinaryScopeKind>(i);
      if (kind != memory::MemoryBinaryScopeKind::process)
        sibling.binary_ownership[kind][15] += 64;
    }
    Require(!manager.AllocateScoped(1, alignof(std::max_align_t), sibling).ok() &&
                manager.Snapshot().current_bytes == 4096,
            "shared process scope must enforce its limit across distinct children");
  }
  Require(manager.Snapshot().current_bytes == 0,
          "complete binary ownership teardown must release physical charges");
  auto malformed = tag;
  malformed.binary_ownership[memory::MemoryBinaryScopeKind::descriptor_snapshot][6] = 0x40;
  Require(!memory::MemoryBinaryOwnershipValid(malformed) &&
              !manager.AllocateScoped(4096, alignof(std::max_align_t), malformed).ok() &&
              manager.Snapshot().current_bytes == 0,
          "extended scope validation must refuse invalid UUIDs without a physical charge");
  auto mixed = tag;
  mixed.owner = "legacy-owner";
  Require(!memory::MemoryBinaryOwnershipValid(mixed),
          "extended binary scopes must not permit mixed legacy ownership");
}

void RegisterReserveMetricFixture() {
  namespace metrics = scratchbird::core::metrics;
  auto id = [](unsigned char ordinal) {
    auto value = scratchbird::tests::FixtureUuidLiteral("019f0000-0000-7000-8000-00000000ee00");
    value.bytes[15] = ordinal;
    return value;
  };
  auto queue = metrics::MetricObservationQueue::Create({id(1), id(2), {}}, {128, 65536});
  Require(queue.ok(), "reserve metric observation queue creation failed");
  auto& registry = metrics::DefaultMetricRegistry();
  Require(registry.BindObservationQueue(std::move(queue.queue)).ok, "reserve metric queue binding failed");
  const auto definitions = metrics::BuiltinMetricDescriptorDefinitions();
  const auto found = std::find_if(definitions.begin(), definitions.end(), [](const auto& d) {
    return d.family == "sb_memory_emergency_reserve_bytes";
  });
  Require(found != definitions.end(), "canonical reserve metric definition missing");
  metrics::MetricRetentionPolicy policy;
  policy.policy_uuid = id(3); policy.generation = 1;
  policy.policy_name = "reserve component observation fixture";
  metrics::MetricDescriptor descriptor;
  static_cast<metrics::MetricDescriptorDefinition&>(descriptor) = *found;
  descriptor.metric_uuid = id(4); descriptor.descriptor_generation = 1;
  descriptor.label_schema_uuid = id(5); descriptor.label_schema_generation = 1;
  descriptor.retention_policy_uuid = policy.policy_uuid;
  descriptor.retention_policy_generation = policy.generation;
  descriptor.visibility_policy_uuid = id(6); descriptor.visibility_policy_generation = 1;
  descriptor.readiness = metrics::MetricReadiness::implemented;
  Require(registry.RegisterDescriptor(descriptor).ok, "reserve metric descriptor registration failed");
  metrics::MetricHistoryBinding binding;
  static_cast<metrics::MetricDescriptorBinding&>(binding) = descriptor;
  binding.database_uuid = id(1); binding.node_uuid = id(2);
  auto series = metrics::MakeMetricSeriesIdentity(descriptor,
      {{"component", "core.memory"}, {"operation", "snapshot"}}, policy, binding, id(7), 1);
  Require(series.ok() && registry.RegisterSeries(*series.record, policy).ok,
          "reserve metric series registration failed");
}

void CheckPublishedReserve(memory::MemoryManager& manager, const memory::MemoryTag& tag,
                           std::uint64_t expected) {
  // Exercise the actual allocation-time producer, not a fabricated gauge.
  // This component fixture does not qualify periodic runtime collection.
  auto sample_trigger = manager.AllocateScoped(1, 0, tag);
  Require(sample_trigger.ok(), "reserve metric sampling allocation failed");
  bool observed = false;
  for (const auto& sample : scratchbird::core::metrics::DefaultMetricRegistry().SnapshotCurrent(false)) {
    if (sample.family != "sb_memory_emergency_reserve_bytes") continue;
    const auto* bytes = std::get_if<std::uint64_t>(&sample.value);
    Require(bytes && *bytes == expected, "reserve gauge must publish exact actual available backing");
    observed = true;
  }
  Require(observed, "actual reserve metric observation missing");
}

void EmergencyReserveOwnsGovernedBacking() {
  RegisterReserveMetricFixture();
  memory::AllocationPolicy policy;
  policy.hard_limit_bytes = policy.byte_limit = 8192;
  policy.zero_memory_on_allocate = false;
  memory::MemoryManager manager(policy);
  memory::MemoryTag tag;
  tag.category = memory::MemoryCategory::diagnostics;
  tag.lifetime = memory::MemoryLifetime::process;
  tag.purpose = "emergency_reserve_conformance";
  Require(manager.Snapshot().emergency_reserve_available_bytes == 0,
          "ordinary unused policy headroom is not emergency reserve");
  CheckPublishedReserve(manager, tag, 0);
  for (auto kind : {memory::MemoryBinaryScopeKind::context,
                    memory::MemoryBinaryScopeKind::owner}) {
    auto& uuid = tag.binary_ownership[kind];
    uuid[6] = 0x70;
    uuid[8] = 0x80;
    uuid[15] = kind == memory::MemoryBinaryScopeKind::context ? 1 : 2;
  }
  {
    memory::EmergencyMemoryReserve reserve(manager, tag, 4096);
    Require(reserve.Snapshot().allocated && manager.Snapshot().current_bytes == 4096,
            "reserve must retain real shared-governor backing");
    Require(manager.Snapshot().emergency_reserve_available_bytes == 4096,
            "initialized reserve must be attributed to its actual governor");
    CheckPublishedReserve(manager, tag, 4096);
    Require(!reserve.TryReset(8192) && manager.Snapshot().emergency_reserve_available_bytes == 4096,
            "failed replacement must preserve unconsumed reserve availability");
    {
      memory::EmergencyMemoryReserve second(manager, tag, 1024);
      Require(manager.Snapshot().emergency_reserve_available_bytes == 5120,
              "multiple reserves must aggregate actual distinct backing once");
    }
    Require(manager.Snapshot().emergency_reserve_available_bytes == 4096,
            "unconsumed reserve destruction must remove available backing");
    for (auto kind : {memory::MemoryBinaryScopeKind::context,
                      memory::MemoryBinaryScopeKind::owner}) {
      bool found = false;
      for (const auto& context : manager.Snapshot().contexts) {
        if (context.binary_scope && context.binary_scope->kind == kind &&
            context.binary_scope->uuid == tag.binary_ownership[kind]) {
          found = context.current_bytes == 4096 && context.scope_id.empty();
        }
      }
      Require(found, "reserve must charge exact binary identities without text UUID conversion");
    }
    Require(reserve.DiagnosticsBuffer().empty(), "unreleased reserve must not expose diagnostic use");
    auto refused = manager.AllocateScoped(8192, alignof(std::max_align_t), tag);
    Require(!refused.ok(), "ordinary allocation bypassed retained emergency charge");
    std::atomic<std::uint64_t> released{0};
    std::vector<std::thread> threads;
    for (int i = 0; i < 8; ++i) {
      threads.emplace_back([&] { released += reserve.ReleaseForEmergencyDiagnostics(); });
    }
    for (auto& thread : threads) thread.join();
    Require(released == 4096 && reserve.Snapshot().released_bytes == 4096,
            "concurrent diagnostic transfer must happen exactly once");
    Require(manager.Snapshot().emergency_reserve_available_bytes == 0,
            "transferred diagnostic storage must not remain available reserve");
    CheckPublishedReserve(manager, tag, 0);
    auto buffer = reserve.DiagnosticsBuffer();
    Require(buffer.size() == 4096 && manager.Snapshot().current_bytes == 4096,
            "diagnostic transfer must preserve the physical charge");
    Require(std::all_of(buffer.begin(), buffer.end(), [](std::byte value) {
              return value == std::byte{0};
            }), "emergency backing must be initialized before diagnostic transfer");
    buffer.front() = std::byte{0x35};
    buffer.back() = std::byte{0x79};
    memory::DiagnosticRecord failure;
    Require(!reserve.TryReset(8192, &failure) &&
                failure.diagnostic_code == "SB-MEMORY-ALLOC-LIMIT-EXCEEDED",
            "replacement must preserve the shared governor's exact failure diagnostic");
    Require(reserve.DiagnosticsBuffer().data() == buffer.data() &&
                reserve.DiagnosticsBuffer().front() == std::byte{0x35} &&
                reserve.DiagnosticsBuffer().back() == std::byte{0x79} &&
                reserve.Snapshot().released_bytes == 4096 && manager.Snapshot().current_bytes == 4096,
            "failed replacement must preserve bytes and exact prior ownership");
    Require(reserve.TryReset(2048) && manager.Snapshot().current_bytes == 2048 &&
                reserve.DiagnosticsBuffer().empty() && reserve.Snapshot().available_bytes == 2048,
            "successful replacement must retire old backing and restore withheld state");
    Require(manager.Snapshot().emergency_reserve_available_bytes == 2048,
            "replacement reserve accounting must reflect actual new backing");
    Require(reserve.ReleaseForEmergencyDiagnostics() == 2048,
            "replacement reserve transfer failed");
    auto replacement_buffer = reserve.DiagnosticsBuffer();
    Require(std::all_of(replacement_buffer.begin(), replacement_buffer.end(), [](std::byte value) {
              return value == std::byte{0};
            }), "replacement reserve must not expose previous allocation contents");
    Require(reserve.TryReset(0) && !reserve.Snapshot().allocated &&
                manager.Snapshot().current_bytes == 0,
            "zero reset must release actual charges");
    Require(reserve.TryReset(4096), "reserve reinitialization failed");
  }
  Require(manager.Snapshot().current_bytes == 0 && manager.Snapshot().active_allocation_count == 0,
          "reserve destruction leaked physical ownership");
  Require(manager.Snapshot().emergency_reserve_available_bytes == 0,
          "final reserve destruction leaked available-reserve accounting");
  CheckPublishedReserve(manager, tag, 0);
}

}  // namespace

int main() {
  memory::EmergencyMemoryReserve early_reserve;
  Require(!early_reserve.TryReset(4096) && !early_reserve.Snapshot().allocated,
          "unconfigured governor must not yield a synthetic reserve");
  Require(memory::ConfigureDefaultMemoryManager(memory::DefaultLocalEngineMemoryPolicy(),
                                                "ceic017_pressure_fixture").ok(),
          "CEIC-017 must initialize the shared governor before acquiring real reserves");
  Require(early_reserve.TryReset(4096) && early_reserve.TryReset(0),
          "failed early admission must permit retry after governor initialization");
  std::cout << "CEIC-017 authority_note=memory_pressure_evidence_only;"
               "not_transaction_finality_visibility_security_authorization_"
               "recovery_parser_reference_wal_benchmark_optimizer_plan_index_"
               "finality_or_agent_action_authority"
            << '\n';
  SyntheticStateTransitions();
  PressurePercentRetainsExactUint64Boundaries();
  SoftPressurePlansSpillThrottleAndBackgroundAction();
  HighPressureShrinksCacheAndCancelsLowPriorityWork();
  EmergencyReserveAndDiagnosticsAreBounded();
  UnsafeAuthorityFailsClosed();
  RecoveryDoesNotReadmitTooFast();
  EmergencyReserveOwnsGovernedBacking();
  AllDeclaredBinaryOwnershipScopesAreCharged();
  return EXIT_SUCCESS;
}

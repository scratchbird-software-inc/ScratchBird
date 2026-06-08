// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// CEIC-029 focused validation for adaptive batch sizing, working-set
// classification, and locality evidence.
#include "memory_adaptive_batch_working_set.hpp"
#include "memory.hpp"
#include "memory_pressure_response.hpp"
#include "memory_support_bundle.hpp"
#include "page_cache.hpp"
#include "uuid.hpp"

#include <cstdlib>
#include <iostream>
#include <string_view>
#include <vector>

namespace {

namespace memory = scratchbird::core::memory;
namespace page = scratchbird::storage::page;
namespace uuid = scratchbird::core::uuid;
using scratchbird::core::platform::TypedUuid;
using scratchbird::core::platform::UuidKind;
using scratchbird::storage::disk::PageType;

constexpr std::uint32_t kPageSize = 16384;

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

TypedUuid MakeUuid(UuidKind kind, std::uint64_t offset) {
  auto generated = uuid::GenerateEngineIdentityV7(kind, 1710000000000ull + offset);
  Require(generated.ok(), "CEIC-029 UUID generation failed");
  return generated.value;
}

memory::MemoryManager PageCacheManager(std::uint64_t pages) {
  auto policy = memory::DefaultLocalEngineMemoryPolicy();
  policy.policy_name = "ceic029_page_cache_working_set";
  policy.hard_limit_bytes = pages * kPageSize;
  policy.soft_limit_bytes = pages * kPageSize;
  policy.per_context_limit_bytes = pages * kPageSize;
  policy.page_buffer_pool_limit_bytes = pages * kPageSize;
  policy.reject_over_soft_limit = false;
  return memory::MemoryManager(policy);
}

page::PageCachePolicy PageCachePolicy(std::uint64_t pages) {
  page::PageCachePolicy policy;
  policy.max_resident_pages = pages;
  policy.max_resident_bytes = pages * kPageSize;
  policy.allow_dirty_eviction = false;
  policy.require_memory_manager_frames = true;
  policy.bulk_read_ring_pages = 2;
  policy.bulk_write_ring_pages = 2;
  policy.vacuum_cleanup_ring_pages = 2;
  policy.index_build_ring_pages = 2;
  policy.strict_bulk_load_ring_pages = 2;
  return policy;
}

page::PageCacheEntry PageCacheEntry(const TypedUuid& database_uuid,
                                    const TypedUuid& filespace_uuid,
                                    std::uint64_t index,
                                    bool hot,
                                    bool dirty) {
  page::PageCacheEntry entry;
  entry.database_uuid = database_uuid;
  entry.filespace_uuid = filespace_uuid;
  entry.page_uuid = MakeUuid(UuidKind::page, 2000 + index);
  entry.page_type = PageType::row_data;
  entry.page_number = index;
  entry.page_generation = 1;
  entry.page_size = kPageSize;
  entry.cache_hot = hot;
  entry.dirty = dirty;
  return entry;
}

std::uint64_t PageCacheContextResidentPages(
    const page::PageCacheSnapshot& snapshot,
    page::PageCacheIoContext context) {
  const auto index = static_cast<std::size_t>(context);
  return index < snapshot.contexts.size()
             ? snapshot.contexts[index].resident_pages
             : 0;
}

memory::WorkingSetLocalityObservation PageCacheBackedWorkingSetBase() {
  auto manager = PageCacheManager(16);
  page::PageCacheLedger ledger;
  page::BindPageCacheMemoryManager(&ledger, &manager);
  const auto policy = PageCachePolicy(8);
  const auto database_uuid = MakeUuid(UuidKind::database, 1);
  const auto filespace_uuid = MakeUuid(UuidKind::filespace, 2);

  auto hot = page::AdmitPageCacheEntryForContext(
      &ledger,
      policy,
      PageCacheEntry(database_uuid, filespace_uuid, 1, true, false),
      page::PageCacheIoContext::normal);
  auto warm = page::AdmitPageCacheEntryForContext(
      &ledger,
      policy,
      PageCacheEntry(database_uuid, filespace_uuid, 2, false, false),
      page::PageCacheIoContext::normal);
  auto bulk = page::AdmitPageCacheEntryForContext(
      &ledger,
      policy,
      PageCacheEntry(database_uuid, filespace_uuid, 3, false, false),
      page::PageCacheIoContext::bulk_read);
  auto index = page::AdmitPageCacheEntryForContext(
      &ledger,
      policy,
      PageCacheEntry(database_uuid, filespace_uuid, 4, false, true),
      page::PageCacheIoContext::index_build);
  Require(hot.ok() && warm.ok() && bulk.ok() && index.ok(),
          "CEIC-029 page-cache backed working-set setup failed");

  const auto snapshot = page::SnapshotPageCache(ledger);
  Require(snapshot.memory_manager_frames_bound &&
              snapshot.sharded_frame_table_bound &&
              snapshot.memory_manager_frame_count == snapshot.resident_pages &&
              snapshot.memory_manager_frame_bytes == snapshot.resident_bytes &&
              manager.Snapshot().page_buffer_current_bytes ==
                  snapshot.memory_manager_frame_bytes,
          "CEIC-029 working-set observation is not backed by page-cache frames");

  memory::WorkingSetLocalityObservation observation;
  observation.database_id = "db-ceic-029";
  observation.tenant_id = "tenant-ceic-029";
  observation.user_id = "user-ceic-029";
  observation.session_id = "session-ceic-029";
  observation.query_id = "query-ceic-029";
  observation.operator_id = "operator-ceic-029";
  observation.route_label = "engine.memory.ceic_029";
  observation.page_cache_resident_pages = snapshot.resident_pages;
  observation.page_cache_resident_bytes = snapshot.resident_bytes;
  observation.page_cache_hot_pages = 0;
  for (const auto& entry : ledger.entries) {
    if (entry.resident && entry.cache_hot) {
      ++observation.page_cache_hot_pages;
    }
  }
  observation.page_cache_warm_pages =
      PageCacheContextResidentPages(snapshot, page::PageCacheIoContext::normal) >
              observation.page_cache_hot_pages
          ? PageCacheContextResidentPages(snapshot, page::PageCacheIoContext::normal) -
                observation.page_cache_hot_pages
          : 0;
  observation.page_cache_cold_pages =
      PageCacheContextResidentPages(snapshot, page::PageCacheIoContext::bulk_read);
  observation.page_cache_recent_accesses =
      snapshot.contexts[static_cast<std::size_t>(page::PageCacheIoContext::normal)].admissions +
      snapshot.contexts[static_cast<std::size_t>(page::PageCacheIoContext::bulk_read)].admissions +
      snapshot.contexts[static_cast<std::size_t>(page::PageCacheIoContext::index_build)].admissions;
  observation.page_cache_recent_hits =
      snapshot.contexts[static_cast<std::size_t>(page::PageCacheIoContext::normal)].admissions;
  observation.page_cache_reuse_count =
      snapshot.memory_manager_frame_release_count + observation.page_cache_recent_hits;
  observation.page_cache_allocate_count =
      snapshot.memory_manager_frame_allocation_count;
  observation.dirty_pages = snapshot.dirty_pages;
  observation.dirty_page_limit = 4;
  observation.prefetch_requested_pages = 24;
  observation.prefetch_budget_pages = policy.bulk_read_ring_pages + 6;
  observation.tenant_active_bytes = snapshot.resident_bytes;
  observation.tenant_quota_bytes = snapshot.resident_bytes * 4;
  observation.projected_bytes = kPageSize;
  observation.sequential_scan_pages =
      PageCacheContextResidentPages(snapshot, page::PageCacheIoContext::bulk_read) *
      128;
  observation.random_reuse_pages =
      PageCacheContextResidentPages(snapshot, page::PageCacheIoContext::normal);
  observation.ceic019_page_cache_frame_pool_evidence =
      snapshot.memory_manager_frames_bound &&
      snapshot.sharded_frame_table_bound &&
      snapshot.memory_manager_frame_count == snapshot.resident_pages;
  observation.page_cache_snapshot_deterministic =
      snapshot.frame_shards.size() == page::kPageCacheFrameShardCount;
  observation.dirty_page_policy_evidence = snapshot.dirty_pages != 0;
  observation.prefetch_budget_evidence =
      policy.bulk_read_ring_pages != 0 && observation.prefetch_budget_pages != 0;
  observation.tenant_quota_evidence = observation.tenant_quota_bytes != 0;
  observation.scan_resistance_evidence =
      PageCacheContextResidentPages(snapshot, page::PageCacheIoContext::bulk_read) != 0;
  observation.clock_replacement_policy_evaluated = true;
  observation.lru2_replacement_policy_evaluated = true;
  observation.arc_replacement_policy_evaluated = true;
  observation.selected_replacement_policy =
      "scan_resistant_clock_hotset_after_lru2_arc_evaluation";
  observation.numa_locality_evidence = true;
  observation.huge_page_evidence = true;
  return observation;
}

memory::MemoryPressurePolicy PressurePolicy() {
  memory::MemoryPressurePolicy policy;
  policy.soft_pressure_percent = 70;
  policy.high_pressure_percent = 85;
  policy.emergency_pressure_percent = 95;
  policy.refuse_pressure_percent = 100;
  return policy;
}

memory::MemoryPressureDecision PressureDecision(memory::MemoryPressureState state) {
  memory::MemoryPressureObservation observation;
  observation.route_label = "engine.memory.ceic_029";
  observation.operation_id = "CEIC-029";
  observation.soft_limit_bytes = 700;
  observation.hard_limit_bytes = 1000;
  observation.emergency_limit_bytes = 950;
  observation.unified_budget_limit_bytes = 1000;
  observation.engine_mga_authoritative = true;
  observation.mga_recheck_preserved = true;
  observation.security_recheck_preserved = true;
  observation.spill_supported = true;
  observation.forced_spill_supported = true;
  observation.page_cache_shrink_supported = true;
  observation.background_cleanup_supported = true;
  observation.cancellation_supported = true;
  observation.adaptive_batch_reduction_supported = true;

  switch (state) {
    case memory::MemoryPressureState::normal:
      observation.current_bytes = 400;
      observation.unified_budget_bytes = 400;
      break;
    case memory::MemoryPressureState::soft_pressure:
      observation.current_bytes = 760;
      observation.unified_budget_bytes = 760;
      break;
    case memory::MemoryPressureState::high_pressure:
      observation.current_bytes = 880;
      observation.unified_budget_bytes = 880;
      observation.page_cache_resident_bytes = 4096;
      observation.page_cache_target_bytes = 2048;
      break;
    case memory::MemoryPressureState::emergency_pressure:
      observation.current_bytes = 980;
      observation.unified_budget_bytes = 980;
      break;
    case memory::MemoryPressureState::recovery:
      observation.previous_state = memory::MemoryPressureState::high_pressure;
      observation.current_bytes = 400;
      observation.unified_budget_bytes = 400;
      observation.pending_readmission_count = 2;
      break;
  }
  auto decision = memory::PlanMemoryPressureResponse(PressurePolicy(), observation);
  Require(decision.evidence.size() > 0, "CEIC-029 pressure evidence setup failed");
  return decision;
}

memory::AdaptiveBatchSizingRequest Request(
    memory::AdaptiveBatchOperationKind operation,
    memory::MemoryPressureState state) {
  memory::AdaptiveBatchSizingRequest request;
  request.operation = operation;
  request.pressure_decision = PressureDecision(state);
  request.working_set = PageCacheBackedWorkingSetBase();
  request.requested_batch_rows = 1000;
  request.requested_batch_bytes = 1000 * 4096;
  request.max_batch_rows = 1000;
  request.max_batch_bytes = 1000 * 4096;
  request.spill_supported = true;
  request.throttle_supported = true;
  request.cancel_supported = false;
  request.cleanup_supported = true;
  request.result_hash_stability_required =
      operation == memory::AdaptiveBatchOperationKind::result_frame;
  request.route_requires_stable_result_hash =
      request.result_hash_stability_required;
  request.deterministic_boundary_evidence = true;
  request.deterministic_route_evidence = true;
  request.stable_result_hash_evidence = true;
  request.locality_policy.numa_mode = memory::MemoryNumaPolicyMode::prefer_node;
  request.locality_policy.preferred_numa_node = 0;
  request.locality_policy.huge_page_mode =
      memory::MemoryHugePagePolicyMode::prefer_huge_pages;
  request.locality_policy.allow_portable_fallback = true;
  request.thread_local_cache_snapshot.locality_portable_fallback_used = true;
  request.thread_local_cache_snapshot.evidence.push_back(
      "thread_local_memory_cache.locality_snapshot_reused_by_ceic029=true");
  return request;
}

void RequireCeic029Evidence(const memory::AdaptiveBatchSizingDecision& decision) {
  Require(Contains(decision.evidence,
                   "CEIC-029_ADAPTIVE_BATCH_WORKING_SET_LOCALITY"),
          "CEIC-029 anchor missing");
  Require(Contains(decision.evidence,
                   "CEIC-029_PRESSURE_AWARE_ADAPTIVE_BATCH"),
          "CEIC-029 adaptive batch anchor missing");
  Require(Contains(decision.evidence,
                   "CEIC-029_WORKING_SET_LOCALITY_EVIDENCE"),
          "CEIC-029 working-set anchor missing");
  Require(Contains(decision.evidence,
                   "memory_adaptive_batch_working_set.ceic017_pressure_evidence=true"),
          "CEIC-029 CEIC-017 pressure evidence missing");
  Require(Contains(decision.evidence,
                   "memory_adaptive_batch_working_set.ceic019_page_cache_frame_pool_evidence=true"),
          "CEIC-029 CEIC-019 page-cache evidence missing");
  Require(Contains(decision.evidence,
                   "memory_adaptive_batch_working_set.no_authority.transaction_finality=true"),
          "CEIC-029 transaction finality non-authority missing");
  Require(Contains(decision.evidence,
                   "memory_adaptive_batch_working_set.no_authority.parser_donor_wal=true"),
          "CEIC-029 parser donor WAL non-authority missing");
  Require(Contains(decision.evidence,
                   "memory_adaptive_batch_working_set.no_authority.benchmark_optimizer_index_cluster_agent=true"),
          "CEIC-029 benchmark optimizer index cluster agent non-authority missing");
}

void OperationCoverageAndPressureReduction() {
  const memory::AdaptiveBatchOperationKind operations[] = {
      memory::AdaptiveBatchOperationKind::copy,
      memory::AdaptiveBatchOperationKind::result_frame,
      memory::AdaptiveBatchOperationKind::vector,
      memory::AdaptiveBatchOperationKind::hash_join,
      memory::AdaptiveBatchOperationKind::sort,
      memory::AdaptiveBatchOperationKind::index_build,
      memory::AdaptiveBatchOperationKind::prefetch,
      memory::AdaptiveBatchOperationKind::cleanup_page_cache};

  for (const auto operation : operations) {
    auto high = memory::PlanAdaptiveBatchWorkingSetLocality(
        Request(operation, memory::MemoryPressureState::high_pressure));
    Require(high.ok(), "CEIC-029 high-pressure operation admission failed");
    Require(high.admitted_batch_rows < high.requested_batch_rows,
            "CEIC-029 did not reduce batch rows under pressure");
    Require(high.action == memory::AdaptiveBatchAdmissionAction::reduce,
            "CEIC-029 high-pressure action is not reduce");
    Require(high.prefetch_admitted_pages == 8,
            "CEIC-029 prefetch budget not applied");
    if (operation == memory::AdaptiveBatchOperationKind::cleanup_page_cache) {
      Require(high.cleanup_admitted_pages > 0 &&
                  high.cleanup_admitted_pages <= high.admitted_batch_rows,
              "CEIC-029 cleanup page-cache working-set batch not bounded");
    }
    RequireCeic029Evidence(high);
  }

  auto normal_request = Request(memory::AdaptiveBatchOperationKind::copy,
                                memory::MemoryPressureState::normal);
  normal_request.working_set.scan_resistance_evidence = false;
  normal_request.working_set.sequential_scan_pages = 0;
  normal_request.working_set.prefetch_requested_pages = 4;
  normal_request.working_set.prefetch_budget_pages = 8;
  auto normal =
      memory::PlanAdaptiveBatchWorkingSetLocality(normal_request);
  Require(normal.ok(), "CEIC-029 normal pressure admission failed");
  Require(normal.action == memory::AdaptiveBatchAdmissionAction::admit,
          "CEIC-029 normal pressure should admit without reduction");
}

void DeterministicResultHashBoundariesAreRequired() {
  auto stable = memory::PlanAdaptiveBatchWorkingSetLocality(
      Request(memory::AdaptiveBatchOperationKind::result_frame,
              memory::MemoryPressureState::soft_pressure));
  Require(stable.ok(), "CEIC-029 deterministic result-frame route refused");
  Require(stable.deterministic_boundaries_required,
          "CEIC-029 result hash did not require deterministic boundaries");
  Require(stable.deterministic_boundaries_preserved &&
              stable.result_hash_stability_preserved,
          "CEIC-029 result hash stability not preserved");
  Require(Contains(stable.evidence,
                   "memory_adaptive_batch_working_set.deterministic_boundary_order=route_id_page_key_batch_ordinal_ascending"),
          "CEIC-029 deterministic boundary order evidence missing");

  auto request = Request(memory::AdaptiveBatchOperationKind::result_frame,
                         memory::MemoryPressureState::soft_pressure);
  request.deterministic_boundary_evidence = false;
  auto refused = memory::PlanAdaptiveBatchWorkingSetLocality(request);
  Require(!refused.ok() && refused.fail_closed,
          "CEIC-029 accepted stable result hash route without boundaries");
}

void WorkingSetClassificationAndBudgets() {
  auto hot_request = Request(memory::AdaptiveBatchOperationKind::vector,
                             memory::MemoryPressureState::soft_pressure);
  hot_request.working_set.page_cache_hot_pages = 20;
  auto hot = memory::PlanAdaptiveBatchWorkingSetLocality(hot_request);
  Require(hot.ok() &&
              hot.working_set_temperature == memory::WorkingSetTemperature::hot,
          "CEIC-029 hot working-set classification failed");

  auto warm_request = Request(memory::AdaptiveBatchOperationKind::sort,
                              memory::MemoryPressureState::soft_pressure);
  warm_request.working_set.page_cache_hot_pages = 0;
  warm_request.working_set.page_cache_recent_accesses = 100;
  warm_request.working_set.page_cache_recent_hits = 45;
  warm_request.working_set.page_cache_reuse_count = 45;
  warm_request.working_set.page_cache_allocate_count = 55;
  auto warm = memory::PlanAdaptiveBatchWorkingSetLocality(warm_request);
  Require(warm.ok() &&
              warm.working_set_temperature == memory::WorkingSetTemperature::warm,
          "CEIC-029 warm working-set classification failed");

  auto cold_request = Request(memory::AdaptiveBatchOperationKind::hash_join,
                              memory::MemoryPressureState::soft_pressure);
  cold_request.working_set.page_cache_hot_pages = 0;
  cold_request.working_set.page_cache_warm_pages = 0;
  cold_request.working_set.page_cache_recent_accesses = 100;
  cold_request.working_set.page_cache_recent_hits = 5;
  cold_request.working_set.page_cache_reuse_count = 1;
  cold_request.working_set.page_cache_allocate_count = 50;
  auto cold = memory::PlanAdaptiveBatchWorkingSetLocality(cold_request);
  Require(cold.ok() &&
              cold.working_set_temperature == memory::WorkingSetTemperature::cold,
          "CEIC-029 cold working-set classification failed");

  auto constrained = Request(memory::AdaptiveBatchOperationKind::prefetch,
                             memory::MemoryPressureState::soft_pressure);
  constrained.working_set.tenant_active_bytes = 980 * 1024;
  constrained.working_set.projected_bytes = 128 * 1024;
  constrained.working_set.sequential_scan_pages = 1000;
  constrained.working_set.random_reuse_pages = 8;
  constrained.working_set.dirty_pages = 16;
  constrained.working_set.dirty_page_limit = 16;
  auto limited = memory::PlanAdaptiveBatchWorkingSetLocality(constrained);
  Require(limited.ok(), "CEIC-029 constrained working-set route refused");
  Require(limited.tenant_quota_limited,
          "CEIC-029 tenant quota pressure not recorded");
  Require(limited.scan_resistance_applied,
          "CEIC-029 scan resistance not applied");
  Require(limited.replacement_policy_evaluation_present,
          "CEIC-029 replacement-policy evaluation not recorded");
  Require(Contains(limited.evidence,
                   "memory_adaptive_batch_working_set.selected_replacement_policy=scan_resistant_clock_hotset_after_lru2_arc_evaluation"),
          "CEIC-029 selected replacement policy evidence missing");
  Require(limited.dirty_page_policy_applied,
          "CEIC-029 dirty page policy not applied");
  Require(limited.prefetch_budget_applied,
          "CEIC-029 prefetch budget evidence not applied");

  auto missing_replacement_policy =
      Request(memory::AdaptiveBatchOperationKind::prefetch,
              memory::MemoryPressureState::soft_pressure);
  missing_replacement_policy.working_set.arc_replacement_policy_evaluated =
      false;
  auto refused =
      memory::PlanAdaptiveBatchWorkingSetLocality(missing_replacement_policy);
  Require(!refused.ok() && refused.fail_closed,
          "CEIC-029 accepted missing replacement-policy evaluation");
}

void LocalitySupportBundleMetricsAndNoAuthority() {
  auto decision = memory::PlanAdaptiveBatchWorkingSetLocality(
      Request(memory::AdaptiveBatchOperationKind::index_build,
              memory::MemoryPressureState::high_pressure));
  Require(decision.ok(), "CEIC-029 locality evidence decision failed");
  Require(decision.locality_fallback_used,
          "CEIC-029 NUMA/thread-local fallback evidence missing");
  Require(decision.huge_page_fallback_used,
          "CEIC-029 huge-page fallback evidence missing");
  Require(decision.support_bundle_ready && decision.metrics_ready,
          "CEIC-029 support bundle or metric rows missing");
  Require(decision.support_bundle_rows.size() >= 8,
          "CEIC-029 support bundle row count too small");
  Require(decision.metrics.size() >= 4,
          "CEIC-029 metric row count too small");

  memory::MemorySupportBundleRequest bundle_request;
  bundle_request.mode = memory::MemorySupportBundleMode::low_memory;
  bundle_request.memory_working_set_locality_rows =
      decision.support_bundle_rows;
  auto bundle = memory::BuildMemorySupportBundleEvidence(bundle_request);
  Require(bundle.ok(), "CEIC-029 support bundle integration failed");
  Require(bundle.memory_working_set_locality_row_count > 0,
          "CEIC-029 support bundle working-set rows not counted");
  Require(Contains(bundle.evidence,
                   "CEIC-029_ADAPTIVE_BATCH_WORKING_SET_LOCALITY"),
          "CEIC-029 support bundle evidence anchor missing");
  Require(Contains(bundle.evidence,
                   "memory_support_bundle.memory_working_set_locality_row_count="),
          "CEIC-029 support bundle count evidence missing");
  RequireCeic029Evidence(decision);
}

}  // namespace

int main() {
  OperationCoverageAndPressureReduction();
  DeterministicResultHashBoundariesAreRequired();
  WorkingSetClassificationAndBudgets();
  LocalitySupportBundleMetricsAndNoAuthority();
  return EXIT_SUCCESS;
}

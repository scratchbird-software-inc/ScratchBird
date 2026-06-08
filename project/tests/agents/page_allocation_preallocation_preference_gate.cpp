// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "agents/page_allocation_manager.hpp"
#include "page_allocation_lifecycle.hpp"
#include "page_filespace_handoff.hpp"
#include "uuid.hpp"

#include <cstddef>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace {

namespace agents = scratchbird::core::agents::implemented_agents;
namespace page = scratchbird::storage::page;
namespace platform = scratchbird::core::platform;
namespace uuid = scratchbird::core::uuid;

[[noreturn]] void Fail(const std::string& message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, const std::string& message) {
  if (!condition) { Fail(message); }
}

platform::TypedUuid MakeUuid(platform::UuidKind kind, platform::u64 seed) {
  const auto generated = uuid::GenerateEngineIdentityV7(kind, 1860000000000ull + seed);
  Require(generated.ok(), "UUID generation failed");
  return generated.value;
}

bool SameUuid(const platform::TypedUuid& left, const platform::TypedUuid& right) {
  return left.kind == right.kind && left.value == right.value;
}

struct FixtureIds {
  platform::TypedUuid database_uuid;
  platform::TypedUuid filespace_uuid;
  platform::TypedUuid policy_uuid;
  platform::TypedUuid capacity_evidence_uuid;
  platform::TypedUuid owner_object_uuid;
};

FixtureIds MakeIds(platform::u64 seed) {
  return {MakeUuid(platform::UuidKind::database, 100 + seed),
          MakeUuid(platform::UuidKind::filespace, 200 + seed),
          MakeUuid(platform::UuidKind::object, 300 + seed),
          MakeUuid(platform::UuidKind::object, 400 + seed),
          MakeUuid(platform::UuidKind::object, 500 + seed)};
}

page::PageAllocationLedger Ledger(const FixtureIds& ids) {
  page::PageAllocationLedger ledger;
  ledger.database_uuid = ids.database_uuid;
  ledger.filespace_uuid = ids.filespace_uuid;
  return ledger;
}

page::PagePreallocationRequest PreallocationRequest(const FixtureIds& ids,
                                                    const std::string& page_family,
                                                    platform::u64 page_count,
                                                    platform::u64 seed) {
  page::PagePreallocationRequest request;
  request.database_uuid = ids.database_uuid;
  request.filespace_uuid = ids.filespace_uuid;
  request.policy_uuid = ids.policy_uuid;
  request.capacity_evidence_uuid = ids.capacity_evidence_uuid;
  request.creator_transaction_uuid =
      MakeUuid(platform::UuidKind::transaction, 600 + seed);
  request.creator_local_transaction_id = 70 + seed;
  request.page_family = page_family;
  request.page_count = page_count;
  request.page_generation = 9 + seed;
  request.engine_authoritative = true;
  request.capacity_evidence_accepted = true;
  request.durability_fence_satisfied = true;
  return request;
}

page::PageAllocationRequest AllocationRequest(const FixtureIds& ids,
                                              const std::string& page_family,
                                              platform::u64 page_count,
                                              platform::u64 seed) {
  page::PageAllocationRequest request;
  request.database_uuid = ids.database_uuid;
  request.filespace_uuid = ids.filespace_uuid;
  request.owner_object_uuid = ids.owner_object_uuid;
  request.creator_transaction_uuid =
      MakeUuid(platform::UuidKind::transaction, 800 + seed);
  request.creator_local_transaction_id = 90 + seed;
  request.page_family = page_family;
  request.page_count = page_count;
  request.page_generation = 13 + seed;
  request.engine_authoritative = true;
  request.durability_fence_satisfied = true;
  return request;
}

page::PageAllocationResult PreallocatePool(page::PageAllocationLedger* ledger,
                                           const FixtureIds& ids,
                                           const std::string& page_family,
                                           platform::u64 start_page,
                                           platform::u64 page_count,
                                           platform::u64 seed) {
  ledger->free_extents.push_back({start_page, page_count});
  auto result = page::PreallocatePageFamilyPool(
      ledger, PreallocationRequest(ids, page_family, page_count, seed));
  Require(result.ok(), "preallocation setup failed: " + result.diagnostic.diagnostic_code);
  Require(result.allocation.start_page == start_page,
          "preallocation setup start page mismatch");
  return result;
}

platform::u64 FreeExtentPages(const page::PageAllocationLedger& ledger) {
  platform::u64 total = 0;
  for (const auto& extent : ledger.free_extents) {
    total += extent.page_count;
  }
  return total;
}

struct LedgerSnapshot {
  std::vector<page::PageAllocationEntry> allocations;
  std::vector<page::PageFreeExtent> free_extents;
  std::size_t evidence_count = 0;
  platform::u64 next_allocation_seed = 0;
};

LedgerSnapshot Capture(const page::PageAllocationLedger& ledger) {
  return {ledger.allocations,
          ledger.free_extents,
          ledger.evidence.size(),
          ledger.next_allocation_seed};
}

void RequireLedgerUnchanged(const page::PageAllocationLedger& ledger,
                            const LedgerSnapshot& before,
                            const std::string& label) {
  Require(ledger.allocations.size() == before.allocations.size(),
          label + " changed allocation count");
  for (std::size_t index = 0; index < before.allocations.size(); ++index) {
    const auto& left = ledger.allocations[index];
    const auto& right = before.allocations[index];
    Require(SameUuid(left.allocation_uuid, right.allocation_uuid),
            label + " changed allocation UUID");
    Require(left.state == right.state &&
                left.start_page == right.start_page &&
                left.page_count == right.page_count &&
                left.page_family == right.page_family,
            label + " changed allocation payload");
  }
  Require(ledger.free_extents.size() == before.free_extents.size(),
          label + " changed free extent count");
  for (std::size_t index = 0; index < before.free_extents.size(); ++index) {
    Require(ledger.free_extents[index].start_page == before.free_extents[index].start_page &&
                ledger.free_extents[index].page_count == before.free_extents[index].page_count,
            label + " changed free extent payload");
  }
  Require(ledger.evidence.size() == before.evidence_count,
          label + " changed evidence records");
  Require(ledger.next_allocation_seed == before.next_allocation_seed,
          label + " changed allocation identity seed");
}

const page::PageAllocationEntry* FindAllocation(
    const page::PageAllocationLedger& ledger,
    const platform::TypedUuid& allocation_uuid) {
  for (const auto& allocation : ledger.allocations) {
    if (SameUuid(allocation.allocation_uuid, allocation_uuid)) {
      return &allocation;
    }
  }
  return nullptr;
}

std::size_t CountPreallocatedPools(const page::PageAllocationLedger& ledger,
                                   const std::string& page_family) {
  std::size_t count = 0;
  for (const auto& allocation : ledger.allocations) {
    if (allocation.state == page::PageAllocationLifecycleState::preallocated &&
        allocation.page_family == page_family) {
      ++count;
    }
  }
  return count;
}

void RequireRetainedRecovery(const page::PageAllocationLedger& ledger,
                             const std::string& label) {
  const auto recovery = page::ClassifyPageAllocationLedgerForRecovery(ledger);
  Require(recovery.ok(), label + " recovery classification failed");
  for (const auto& classification : recovery.classifications) {
    Require(classification.action != page::PageAllocationRecoveryAction::release_to_free_map,
            label + " recovery released allocated/preallocated ownership");
    Require(!classification.fail_closed,
            label + " recovery failed closed on durable allocation state");
  }
}

void TestPreallocatedPoolPreferredOverEarlierFreeExtent() {
  const auto ids = MakeIds(1);
  auto ledger = Ledger(ids);
  const auto pool = PreallocatePool(&ledger, ids, "data", 500, 8, 1);
  ledger.free_extents.push_back({100, 16});
  const auto free_pages_before = FreeExtentPages(ledger);

  const auto result = page::ReservePageAllocation(
      &ledger, AllocationRequest(ids, "data", 4, 10));

  Require(result.ok(), "preallocated preferred allocation failed: " +
                           result.diagnostic.diagnostic_code);
  Require(result.allocation.start_page == 500,
          "preallocated pool was not preferred over earlier free extent");
  Require(result.evidence.action == "allocate_from_preallocated_pool",
          "preallocated hit evidence action mismatch");
  Require(result.evidence.diagnostic_code ==
              "SB-STORAGE-PAGE-ALLOCATION-PREALLOCATED-POOL-HIT",
          "preallocated hit diagnostic mismatch");
  Require(result.evidence.previous_state == page::PageAllocationLifecycleState::preallocated &&
              result.evidence.new_state == page::PageAllocationLifecycleState::allocated,
          "preallocated hit state transition mismatch");
  Require(result.evidence.capacity_evidence_accepted,
          "preallocated hit did not preserve capacity evidence");
  Require(FreeExtentPages(ledger) == free_pages_before,
          "preallocated hit consumed ordinary free extents");

  const auto* remaining_pool = FindAllocation(ledger, pool.allocation.allocation_uuid);
  Require(remaining_pool != nullptr, "partial consumption removed preallocated pool");
  Require(remaining_pool->state == page::PageAllocationLifecycleState::preallocated &&
              remaining_pool->start_page == 504 &&
              remaining_pool->page_count == 4,
          "partial preallocated pool remainder mismatch");
  RequireRetainedRecovery(ledger, "partial preallocated consumption");
}

void TestExactPreallocatedConsumptionRemovesPool() {
  const auto ids = MakeIds(2);
  auto ledger = Ledger(ids);
  const auto pool = PreallocatePool(&ledger, ids, "data", 700, 4, 2);
  ledger.free_extents.push_back({100, 16});

  const auto result = page::ReservePageAllocation(
      &ledger, AllocationRequest(ids, "data", 4, 20));

  Require(result.ok(), "exact preallocated allocation failed: " +
                           result.diagnostic.diagnostic_code);
  Require(result.allocation.start_page == 700 &&
              result.allocation.page_count == 4,
          "exact preallocated allocation range mismatch");
  Require(result.evidence.action == "allocate_from_preallocated_pool",
          "exact preallocated evidence action mismatch");
  Require(FindAllocation(ledger, pool.allocation.allocation_uuid) == nullptr,
          "exact preallocated consumption left the pool allocation behind");
  Require(CountPreallocatedPools(ledger, "data") == 0,
          "exact preallocated consumption left reusable preallocated pages");
  RequireRetainedRecovery(ledger, "exact preallocated consumption");
}

void TestPageFamilyMismatchFallsBackToFreeExtent() {
  const auto ids = MakeIds(3);
  auto ledger = Ledger(ids);
  const auto pool = PreallocatePool(&ledger, ids, "index", 800, 4, 3);
  ledger.free_extents.push_back({100, 8});

  const auto result = page::ReservePageAllocation(
      &ledger, AllocationRequest(ids, "data", 4, 30));

  Require(result.ok(), "page-family mismatch fallback failed: " +
                           result.diagnostic.diagnostic_code);
  Require(result.allocation.start_page == 100,
          "page-family mismatch did not fall back to free extent");
  Require(result.evidence.action == "allocate_from_free_extent",
          "free extent fallback evidence action mismatch");
  Require(result.evidence.diagnostic_code ==
              "SB-STORAGE-PAGE-ALLOCATION-FREE-EXTENT-FALLBACK",
          "free extent fallback diagnostic mismatch");
  const auto* remaining_pool = FindAllocation(ledger, pool.allocation.allocation_uuid);
  Require(remaining_pool != nullptr &&
              remaining_pool->state == page::PageAllocationLifecycleState::preallocated &&
              remaining_pool->start_page == 800 &&
              remaining_pool->page_count == 4,
          "page-family mismatch changed unrelated preallocated pool");
  Require(ledger.free_extents.size() == 1 &&
              ledger.free_extents.front().start_page == 104 &&
              ledger.free_extents.front().page_count == 4,
          "free extent fallback did not consume ordinary free extent correctly");
}

void TestInsufficientSourcesRefuseWithoutMutation() {
  const auto ids = MakeIds(4);
  auto ledger = Ledger(ids);
  PreallocatePool(&ledger, ids, "data", 900, 3, 4);
  ledger.free_extents.push_back({100, 2});
  const auto before = Capture(ledger);

  const auto result = page::ReservePageAllocation(
      &ledger, AllocationRequest(ids, "data", 4, 40));

  Require(!result.ok(), "insufficient sources unexpectedly allocated");
  Require(result.diagnostic.diagnostic_code ==
              "SB-STORAGE-PAGE-ALLOCATION-INSUFFICIENT-FREE-SPACE",
          "insufficient sources diagnostic mismatch: " +
              result.diagnostic.diagnostic_code);
  RequireLedgerUnchanged(ledger, before, "insufficient sources");
}

void TestInvalidReserveInputsRefuseWithoutMutation() {
  auto run_case = [](const std::string& label,
                     const std::string& diagnostic_code,
                     void (*mutate)(page::PageAllocationRequest*)) {
    const auto ids = MakeIds(50 + label.size());
    auto ledger = Ledger(ids);
    PreallocatePool(&ledger, ids, "data", 1000, 8, 50 + label.size());
    ledger.free_extents.push_back({100, 8});
    auto request = AllocationRequest(ids, "data", 4, 60 + label.size());
    mutate(&request);
    const auto before = Capture(ledger);

    const auto result = page::ReservePageAllocation(&ledger, request);
    Require(!result.ok(), label + " unexpectedly allocated");
    Require(result.diagnostic.diagnostic_code == diagnostic_code,
            label + " diagnostic mismatch: " + result.diagnostic.diagnostic_code);
    RequireLedgerUnchanged(ledger, before, label);
  };

  run_case("not engine authoritative",
           "SB-STORAGE-PAGE-ALLOCATION-NOT-ENGINE-AUTHORITATIVE",
           [](auto* request) { request->engine_authoritative = false; });
  run_case("cluster route requested",
           "SB-STORAGE-PAGE-ALLOCATION-CLUSTER-ROUTE-UNAVAILABLE",
           [](auto* request) { request->cluster_route_requested = true; });
  run_case("missing transaction id",
           "SB-STORAGE-PAGE-ALLOCATION-REQUEST-INVALID",
           [](auto* request) { request->creator_local_transaction_id = 0; });
  run_case("durability fence missing",
           "SB-STORAGE-PAGE-ALLOCATION-DURABILITY-FENCE-REQUIRED",
           [](auto* request) { request->durability_fence_satisfied = false; });
}

void TestUnsafePreallocatedPoolRefusesWithoutFreeFallback() {
  const auto ids = MakeIds(6);
  auto ledger = Ledger(ids);
  const auto pool = PreallocatePool(&ledger, ids, "data", 1200, 8, 6);
  auto* mutable_pool = const_cast<page::PageAllocationEntry*>(
      FindAllocation(ledger, pool.allocation.allocation_uuid));
  Require(mutable_pool != nullptr, "unsafe pool fixture missing");
  mutable_pool->durability_fence_satisfied = false;
  mutable_pool->durable_page_generation = 0;
  ledger.free_extents.push_back({100, 16});
  const auto before = Capture(ledger);

  const auto result = page::ReservePageAllocation(
      &ledger, AllocationRequest(ids, "data", 4, 60));

  Require(!result.ok(), "unsafe preallocated pool fell back to free extent");
  Require(result.diagnostic.diagnostic_code ==
              "SB-STORAGE-PAGE-ALLOCATION-PREALLOCATED-POOL-NOT-DURABLE",
          "unsafe preallocated pool diagnostic mismatch: " +
              result.diagnostic.diagnostic_code);
  RequireLedgerUnchanged(ledger, before, "unsafe preallocated pool");
}

agents::PageAllocationManagerPolicy LowReservePolicy(const FixtureIds& ids) {
  auto policy = agents::DefaultPageAllocationManagerPolicy();
  policy.database_uuid = ids.database_uuid;
  policy.filespace_uuid = ids.filespace_uuid;
  policy.policy_uuid = ids.policy_uuid;
  policy.minimum_free_pages = 4;
  policy.target_free_pages = 8;
  policy.low_water_notify_ratio = 0.50;
  policy.capacity_request_allowed = true;
  policy.capacity_request_policy_explicit = true;
  policy.live_preallocation_allowed = false;
  policy.live_preallocation_policy_explicit = false;
  return policy;
}

agents::PageAllocationManagerMetricSnapshot LowReserveSnapshot(const FixtureIds& ids) {
  agents::PageAllocationManagerMetricSnapshot snapshot;
  snapshot.database_uuid = ids.database_uuid;
  snapshot.filespace_uuid = ids.filespace_uuid;
  snapshot.policy_uuid = ids.policy_uuid;
  snapshot.page_family = "data";
  snapshot.free_pages = 1;
  snapshot.released_pages = 1;
  snapshot.reserved_pages = 2;
  snapshot.preallocated_pages = 1;
  snapshot.allocated_pages = 128;
  snapshot.target_free_deficit_pages = 6;
  snapshot.preallocation_target_pages = 1;
  snapshot.preallocation_deficit_pages = 0;
  snapshot.metrics_present = true;
  snapshot.metrics_fresh = true;
  snapshot.metrics_trusted = true;
  snapshot.scope_compatible = true;
  return snapshot;
}

void TestLowReserveCapacityRequestRemainsQueueBacked() {
  const auto ids = MakeIds(7);
  page::PageFilespaceAgentRequestQueue queue;

  const auto result = agents::EvaluatePageAllocationManagerTick(
      &queue, LowReserveSnapshot(ids), LowReservePolicy(ids));

  Require(result.ok(), "low reserve queue request failed: " +
                           result.diagnostic.diagnostic_code);
  Require(result.decision ==
              agents::PageAllocationManagerDecisionKind::capacity_request_queued,
          "low reserve did not queue a capacity request");
  Require(result.capacity_request_enqueued && result.accepted_evidence,
          "low reserve did not expose accepted durable evidence");
  Require(queue.records.size() == 1, "low reserve queue record count mismatch");
  const auto& record = queue.records.front();
  Require(record.request.kind == page::PageFilespaceAgentRequestKind::extend_filespace,
          "low reserve request kind mismatch");
  Require(record.request.state ==
              page::PageFilespaceAgentRequestState::waiting_filespace_agent,
          "low reserve request state mismatch");
  Require(record.request.requesting_agent == "page_allocation_manager" &&
              record.request.responding_agent == "filespace_capacity_manager",
          "low reserve request routing mismatch");
  Require(record.request.requested_pages == 6 &&
              record.request.released_free_pages == 2 &&
              record.request.target_reserve_pages == 8 &&
              record.request.threshold_pages == 4 &&
              record.request.allocated_pages == 128 &&
              record.request.reserved_pages == 2,
          "low reserve request counts mismatch");
  Require(record.explicit_evidence && record.evidence_id.valid(),
          "low reserve queue evidence missing");
  Require(result.handoff.queue_backed &&
              result.handoff.evidence.durable_state_changed &&
              SameUuid(result.handoff.evidence.request_uuid,
                       record.request.request_uuid),
          "low reserve handoff evidence mismatch");

  const auto restored =
      page::RestorePageFilespaceAgentRequestQueue(
          page::SerializePageFilespaceAgentRequestQueue(queue));
  Require(restored.ok(), "low reserve queue restore failed: " +
                             restored.diagnostic.diagnostic_code);
  const auto classified =
      page::ClassifyPageFilespaceAgentRequestQueueForRecovery(restored.queue);
  Require(classified.ok(), "low reserve queue recovery classification failed: " +
                               classified.diagnostic.diagnostic_code);
  Require(classified.classifications.size() == 1 &&
              classified.classifications.front().action ==
                  page::PageFilespaceAgentRequestRecoveryAction::resume_revalidate,
          "low reserve recovery did not resume/revalidate durable request");
}

}  // namespace

int main() {
  TestPreallocatedPoolPreferredOverEarlierFreeExtent();
  TestExactPreallocatedConsumptionRemovesPool();
  TestPageFamilyMismatchFallsBackToFreeExtent();
  TestInsufficientSourcesRefuseWithoutMutation();
  TestInvalidReserveInputsRefuseWithoutMutation();
  TestUnsafePreallocatedPoolRefusesWithoutFreeFallback();
  TestLowReserveCapacityRequestRemainsQueueBacked();
  return EXIT_SUCCESS;
}

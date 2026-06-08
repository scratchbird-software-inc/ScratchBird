// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "hierarchical_memory_budget_ledger.hpp"
#include "memory.hpp"
#include "reservation_backed_memory_resource.hpp"

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <map>
#include <memory_resource>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace memory = scratchbird::core::memory;

using scratchbird::core::platform::u64;

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view message) {
  if (!condition) {
    Fail(message);
  }
}

memory::AllocationPolicy Policy() {
  auto policy = memory::DefaultLocalEngineMemoryPolicy();
  policy.policy_name = "public_memory_pmr_call_path_gate";
  policy.hard_limit_bytes = 256 * 1024;
  policy.soft_limit_bytes = 256 * 1024;
  policy.per_context_limit_bytes = 256 * 1024;
  policy.page_buffer_pool_limit_bytes = 256 * 1024;
  policy.track_allocations = true;
  policy.zero_memory_on_release = true;
  policy.reject_over_soft_limit = false;
  return policy;
}

memory::HierarchicalMemoryBudgetProvenance RuntimeProvenance() {
  memory::HierarchicalMemoryBudgetProvenance provenance;
  provenance.source =
      memory::HierarchicalMemoryBudgetProvenanceSource::runtime_policy;
  provenance.source_label = "public_pcr016_runtime_policy";
  provenance.engine_mga_authoritative = true;
  provenance.memory_evidence_only = true;
  return provenance;
}

std::vector<memory::HierarchicalMemoryScopeRef> ScopeChain(
    memory::HierarchicalMemoryScopeKind leaf_kind,
    std::string leaf_id) {
  return {
      {memory::HierarchicalMemoryScopeKind::process, "public-pcr016-process"},
      {memory::HierarchicalMemoryScopeKind::database, "public-pcr016-database"},
      {memory::HierarchicalMemoryScopeKind::tenant, "public-pcr016-tenant"},
      {memory::HierarchicalMemoryScopeKind::session, "public-pcr016-session"},
      {memory::HierarchicalMemoryScopeKind::transaction,
       "public-pcr016-transaction"},
      {memory::HierarchicalMemoryScopeKind::statement, "public-pcr016-statement"},
      {memory::HierarchicalMemoryScopeKind::query, "public-pcr016-query"},
      {leaf_kind, std::move(leaf_id)},
  };
}

void SetBudget(memory::HierarchicalMemoryBudgetLedger* ledger,
               memory::HierarchicalMemoryScopeKind kind,
               std::string scope_id,
               u64 hard_limit) {
  memory::HierarchicalMemoryBudget budget;
  budget.scope = {kind, std::move(scope_id)};
  budget.hard_limit_bytes = hard_limit;
  budget.provenance = RuntimeProvenance();
  Require(ledger->SetBudget(std::move(budget)).ok(),
          "PCR-016 budget setup failed");
}

memory::ReservationBackedMemoryResourceAcquireResult AcquireResource(
    memory::HierarchicalMemoryBudgetLedger* ledger,
    memory::MemoryManager* manager,
    memory::ReservationBackedMemoryConsumerKind consumer,
    memory::HierarchicalMemoryScopeKind leaf_kind,
    memory::MemoryCategory category,
    std::string route,
    u64 bytes) {
  SetBudget(ledger, leaf_kind, route, bytes * 2);

  memory::ReservationBackedMemoryResourceRequest request;
  request.consumer_kind = consumer;
  request.reservation_ledger = ledger;
  request.memory_manager = manager;
  request.scope_chain = ScopeChain(leaf_kind, route);
  request.category = category;
  request.memory_class =
      "public_pcr016." +
      std::string(memory::ReservationBackedMemoryConsumerKindName(consumer));
  request.requested_bytes = bytes;
  request.owner_id = route;
  request.route_label = route;
  request.operation_id = route + ".operation";
  request.purpose = route + ".pmr_scratch";
  request.provenance = RuntimeProvenance();
  request.authority.engine_mga_authoritative = true;
  request.authority.transaction_inventory_authoritative = true;
  request.authority.security_or_policy_checked = true;
  return memory::AcquireReservationBackedMemoryResource(std::move(request));
}

std::string LongText(std::string prefix, int ordinal) {
  std::string value = std::move(prefix) + std::to_string(ordinal) + ":";
  while (value.size() < 256) {
    value += "reservation_backed_pmr_container_payload.";
  }
  return value;
}

void PmrContainersRoundTripThroughReservationResource() {
  memory::MemoryManager manager(Policy());
  memory::HierarchicalMemoryBudgetLedger ledger;
  SetBudget(&ledger,
            memory::HierarchicalMemoryScopeKind::process,
            "public-pcr016-process",
            256 * 1024);

  auto acquired = AcquireResource(
      &ledger,
      &manager,
      memory::ReservationBackedMemoryConsumerKind::planner_temporary,
      memory::HierarchicalMemoryScopeKind::query,
      memory::MemoryCategory::executor_query_reserved,
      "public-pcr016-planner-pmr",
      64 * 1024);
  Require(acquired.ok(), "PCR-016 PMR resource acquisition failed");
  Require(ledger.Snapshot().current_bytes == 64 * 1024,
          "PCR-016 reservation ledger did not commit requested bytes");

  memory::ReservationBackedPmrMemoryResource pmr(
      acquired.resource.get(), "public.pcr016.planner");

  {
    std::pmr::vector<std::pmr::string> labels(&pmr);
    labels.reserve(8);
    for (int i = 0; i < 8; ++i) {
      const std::string text = LongText("planner-label-", i);
      labels.push_back(std::pmr::string(text.c_str(), &pmr));
    }

    std::pmr::map<std::pmr::string, std::pmr::vector<std::uint64_t>>
        candidates(&pmr);
    for (int i = 0; i < 6; ++i) {
      const std::string text = LongText("candidate-key-", i);
      std::pmr::string key(text.c_str(), &pmr);
      std::pmr::vector<std::uint64_t> scores(&pmr);
      scores.reserve(24);
      for (int j = 0; j < 24; ++j) {
        scores.push_back(static_cast<std::uint64_t>(i * 100 + j));
      }
      candidates.emplace(std::move(key), std::move(scores));
    }

    const auto pmr_snapshot = pmr.Snapshot();
    Require(pmr_snapshot.bound_to_active_resource,
            "PCR-016 PMR adapter is not bound to active resource");
    Require(pmr_snapshot.allocation_count > 0,
            "PCR-016 PMR containers did not allocate through adapter");
    Require(pmr_snapshot.failed_allocation_count == 0,
            "PCR-016 PMR happy path recorded allocation failure");
    Require(pmr_snapshot.failed_deallocation_count == 0,
            "PCR-016 PMR happy path recorded deallocation failure");
    Require(pmr_snapshot.allocated_bytes > 0,
            "PCR-016 PMR live container bytes were not observed");
    Require(acquired.resource->Snapshot().allocated_bytes ==
                pmr_snapshot.allocated_bytes,
            "PCR-016 PMR adapter and resource allocated byte counts diverged");
    Require(manager.Snapshot().current_bytes == pmr_snapshot.allocated_bytes,
            "PCR-016 PMR manager bytes did not match resource bytes");
  }

  const auto after_container_scope = pmr.Snapshot();
  Require(after_container_scope.allocation_count ==
              after_container_scope.deallocation_count,
          "PCR-016 PMR containers did not return all allocations");
  Require(after_container_scope.allocated_bytes == 0,
          "PCR-016 PMR adapter retained bytes after containers were destroyed");
  Require(acquired.resource->Snapshot().allocated_bytes == 0,
          "PCR-016 reservation resource retained PMR bytes after destruction");
  Require(manager.Snapshot().current_bytes == 0,
          "PCR-016 memory manager retained PMR bytes after destruction");
  Require(ledger.Snapshot().current_bytes == 64 * 1024,
          "PCR-016 resource reservation was released before explicit release");

  int stack_object = 0;
  const auto unknown = acquired.resource->Deallocate(
      &stack_object, sizeof(stack_object), alignof(decltype(stack_object)));
  Require(!unknown.ok(),
          "PCR-016 resource accepted a pointer outside its active map");

  const auto released = acquired.resource->Release();
  Require(released.ok(), "PCR-016 PMR resource release failed");
  Require(ledger.Snapshot().current_bytes == 0,
          "PCR-016 ledger bytes leaked after PMR resource release");
}

void PmrGrowthFailureLeavesNoAllocationOrLedgerLeak() {
  memory::MemoryManager manager(Policy());
  memory::HierarchicalMemoryBudgetLedger ledger;
  SetBudget(&ledger,
            memory::HierarchicalMemoryScopeKind::process,
            "public-pcr016-process",
            256 * 1024);

  auto acquired = AcquireResource(
      &ledger,
      &manager,
      memory::ReservationBackedMemoryConsumerKind::executor_operator,
      memory::HierarchicalMemoryScopeKind::operator_scope,
      memory::MemoryCategory::executor_query_reserved,
      "public-pcr016-executor-pmr-failure",
      128);
  Require(acquired.ok(), "PCR-016 small PMR resource acquisition failed");

  memory::ReservationBackedPmrMemoryResource pmr(
      acquired.resource.get(), "public.pcr016.executor.failure");
  bool threw = false;
  try {
    std::pmr::vector<std::uint64_t> oversized(&pmr);
    oversized.resize(1024);
  } catch (const std::bad_alloc&) {
    threw = true;
  }
  Require(threw, "PCR-016 oversized PMR growth did not fail closed");

  const auto pmr_snapshot = pmr.Snapshot();
  Require(pmr_snapshot.failed_allocation_count > 0,
          "PCR-016 PMR failed growth was not observable");
  Require(pmr_snapshot.allocated_bytes == 0,
          "PCR-016 PMR failure leaked adapter bytes");
  Require(acquired.resource->Snapshot().allocated_bytes == 0,
          "PCR-016 PMR failure leaked resource bytes");
  Require(manager.Snapshot().current_bytes == 0,
          "PCR-016 PMR failure leaked allocator bytes");
  Require(ledger.Snapshot().current_bytes == 128,
          "PCR-016 failed PMR allocation changed committed reservation bytes");

  const auto released = acquired.resource->Release();
  Require(released.ok(), "PCR-016 failed PMR resource release failed");
  Require(ledger.Snapshot().current_bytes == 0,
          "PCR-016 failed PMR reservation leaked after release");
}

}  // namespace

int main() {
  PmrContainersRoundTripThroughReservationResource();
  PmrGrowthFailureLeavesNoAllocationOrLedgerLeak();
  std::cout << "PCR-016 PMR vectors, maps, and strings are reservation-backed\n";
  return EXIT_SUCCESS;
}

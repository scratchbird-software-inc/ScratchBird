// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// CEIC-014 focused validation for thread-local per-core optional-NUMA caches.
#include "thread_local_memory_cache.hpp"

#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

namespace memory = scratchbird::core::memory;
using scratchbird::core::platform::StatusCode;
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

bool Contains(const std::vector<std::string>& evidence, std::string_view needle) {
  for (const auto& entry : evidence) {
    if (entry.find(needle) != std::string::npos) {
      return true;
    }
  }
  return false;
}

memory::HierarchicalMemoryBudgetProvenance Provenance(
    std::string label = "ceic_014_thread_local_per_core_cache_gate") {
  memory::HierarchicalMemoryBudgetProvenance provenance;
  provenance.source = memory::HierarchicalMemoryBudgetProvenanceSource::runtime_policy;
  provenance.source_label = std::move(label);
  provenance.engine_mga_authoritative = true;
  provenance.memory_evidence_only = true;
  return provenance;
}

std::vector<memory::HierarchicalMemoryScopeRef> ScopeChain(
    const std::string& suffix) {
  return {{memory::HierarchicalMemoryScopeKind::process, "ceic-014-process"},
          {memory::HierarchicalMemoryScopeKind::database, "ceic-014-database"},
          {memory::HierarchicalMemoryScopeKind::session, "ceic-014-session-" + suffix},
          {memory::HierarchicalMemoryScopeKind::transaction, "ceic-014-transaction-" + suffix},
          {memory::HierarchicalMemoryScopeKind::statement, "ceic-014-statement-" + suffix},
          {memory::HierarchicalMemoryScopeKind::query, "ceic-014-query-" + suffix},
          {memory::HierarchicalMemoryScopeKind::operator_scope, "ceic-014-operator-" + suffix}};
}

void SetBudget(memory::HierarchicalMemoryBudgetLedger* ledger,
               memory::HierarchicalMemoryScopeKind kind,
               const std::string& scope_id,
               u64 hard_limit) {
  memory::HierarchicalMemoryBudget budget;
  budget.scope = {kind, scope_id};
  budget.hard_limit_bytes = hard_limit;
  budget.provenance = Provenance();
  Require(ledger->SetBudget(std::move(budget)).ok(), "CEIC-014 budget setup failed");
}

memory::ThreadLocalCacheRequest CacheRequest(
    memory::HierarchicalMemoryBudgetLedger* ledger,
    memory::MemoryManager* manager,
    std::string route,
    u64 reservation_bytes = 16384) {
  SetBudget(ledger,
            memory::HierarchicalMemoryScopeKind::operator_scope,
            "ceic-014-operator-" + route,
            reservation_bytes * 2);

  memory::ThreadLocalCacheRequest request;
  request.reservation_ledger = ledger;
  request.memory_manager = manager;
  request.scope_chain = ScopeChain(route);
  request.category = memory::MemoryCategory::executor_query_reserved;
  request.memory_class = "ceic_014.thread_local_cache";
  request.reservation_bytes = reservation_bytes;
  request.owner_id = route;
  request.route_label = route;
  request.operation_id = route + ".operation";
  request.purpose = route + ".thread_local_cache";
  request.object_kind = memory::TypedSlabPoolObjectKind::executor_frame;
  request.size_classes = {{64, 8}, {128, 8}, {256, 4}};
  request.refill_batch_count = 3;
  request.max_cached_slots_per_class_per_thread = 4;
  request.max_cached_bytes_per_thread = 2048;
  request.shard_count = 4;
  request.locality_policy.numa_mode = memory::MemoryNumaPolicyMode::prefer_node;
  request.locality_policy.preferred_numa_node = 0;
  request.locality_policy.allow_portable_fallback = true;
  request.provenance = Provenance();
  request.authority.engine_mga_authoritative = true;
  request.authority.transaction_inventory_authoritative = true;
  request.authority.security_or_policy_checked = true;
  return request;
}

memory::AllocationPolicy TestPolicy() {
  memory::AllocationPolicy policy;
  policy.policy_name = "ceic_014_thread_local_per_core_cache_gate";
  policy.byte_limit = 8ull * 1024ull * 1024ull;
  policy.hard_limit_bytes = 8ull * 1024ull * 1024ull;
  policy.track_allocations = true;
  policy.zero_memory_on_release = true;
  return policy;
}

void ValidateReservationBackedCreationReuseAndEvidence(
    memory::HierarchicalMemoryBudgetLedger* ledger,
    memory::MemoryManager* manager) {
  auto acquired = memory::CreateThreadLocalPerCoreMemoryCache(
      CacheRequest(ledger, manager, "reuse"));
  Require(acquired.ok(), "CEIC-014 cache acquisition failed");
  Require(Contains(acquired.evidence, "thread_local_memory_cache.reservation_first=true"),
          "CEIC-014 reservation-first evidence missing");
  Require(Contains(acquired.evidence, "thread_local_memory_cache.backing_pool=typed_slab_pool"),
          "CEIC-014 typed slab backing evidence missing");
  Require(Contains(acquired.evidence, "memory_locality.numa_provider=portable_fallback_no_libnuma_dependency"),
          "CEIC-014 NUMA unavailable fallback evidence missing");

  auto cache = std::move(acquired.cache);
  auto first = cache->Allocate({32, 0, "first"});
  Require(first.ok() && !first.cache_hit && first.refill_performed,
          "CEIC-014 first allocation did not refill from backing pool");
  std::memset(first.pointer, 0x42, first.requested_bytes);

  auto after_first = cache->Snapshot();
  Require(after_first.miss_count == 1, "CEIC-014 miss count missing");
  Require(after_first.refill_count == 1, "CEIC-014 refill count missing");
  Require(after_first.current_cached_bytes >= 128,
          "CEIC-014 refill did not retain per-thread cached bytes");
  Require(after_first.backing_pool_allocation_count >= 3,
          "CEIC-014 backing pool allocation evidence missing");
  const auto backing_allocations_after_first =
      after_first.backing_pool_allocation_count;

  auto freed = cache->Free(first.pointer);
  Require(freed.ok() && freed.cached_for_reuse,
          "CEIC-014 free did not cache block for same-thread reuse");

  auto second = cache->Allocate({32, 0, "second"});
  Require(second.ok() && second.cache_hit && !second.refill_performed,
          "CEIC-014 second allocation was not a thread-local hit");
  auto after_hit = cache->Snapshot();
  Require(after_hit.hit_count == 1, "CEIC-014 hit count missing");
  Require(after_hit.backing_pool_allocation_count == backing_allocations_after_first,
          "CEIC-014 cache hit touched backing allocator");
  Require(after_hit.threads.size() == 1, "CEIC-014 thread snapshot missing");
  Require(after_hit.threads.front().shard_index < after_hit.shard_count,
          "CEIC-014 per-core shard index evidence invalid");
  Require(after_hit.threads.front().category == memory::MemoryCategory::executor_query_reserved,
          "CEIC-014 thread category evidence mismatch");
  Require(Contains(after_hit.threads.front().evidence, "runtime_topology.current_core_provider="),
          "CEIC-014 current-core provider evidence missing");
  Require(after_hit.locality_portable_fallback_used,
          "CEIC-014 portable NUMA fallback was not reported");
  Require(after_hit.allocation_latency_p50_ns > 0 ||
              after_hit.allocation_latency_p95_ns > 0 ||
              after_hit.allocation_latency_p99_ns > 0,
          "CEIC-014 latency percentile fields missing");

  Require(cache->Free(second.pointer).ok(), "CEIC-014 second free failed");
  auto drained = cache->DrainCurrentThread();
  Require(drained.ok(), "CEIC-014 current-thread drain failed");
  auto after_drain = cache->Snapshot();
  Require(after_drain.current_cached_bytes == 0,
          "CEIC-014 drain left cached bytes behind");
  Require(after_drain.reconciliation_count >= 1,
          "CEIC-014 drain reconciliation count missing");

  auto release = cache->ReconcileAndRelease();
  Require(release.ok(), "CEIC-014 reconcile release failed");
}

void ValidateRemoteFreeHandoff(memory::HierarchicalMemoryBudgetLedger* ledger,
                               memory::MemoryManager* manager) {
  auto acquired = memory::CreateThreadLocalPerCoreMemoryCache(
      CacheRequest(ledger, manager, "remote-free"));
  Require(acquired.ok(), "CEIC-014 remote-free cache acquisition failed");
  auto cache = std::move(acquired.cache);
  auto allocated = cache->Allocate({48, 0, "remote-owner"});
  Require(allocated.ok(), "CEIC-014 remote-free allocation failed");

  bool remote_ok = false;
  std::thread remote([&]() {
    auto freed = cache->Free(allocated.pointer);
    remote_ok = freed.ok() && freed.remote_free_handoff;
  });
  remote.join();
  Require(remote_ok, "CEIC-014 remote free handoff failed");
  const auto snapshot = cache->Snapshot();
  Require(snapshot.remote_free_handoff_count == 1,
          "CEIC-014 remote-free handoff count missing");
  Require(cache->ReconcileAndRelease().ok(),
          "CEIC-014 remote-free reconcile release failed");
}

void ValidateBudgetRefusal(memory::MemoryManager* manager) {
  memory::HierarchicalMemoryBudgetLedger ledger;
  SetBudget(&ledger,
            memory::HierarchicalMemoryScopeKind::process,
            "ceic-014-process",
            128);
  auto request = CacheRequest(&ledger, manager, "budget-refusal", 1024);
  auto refused = memory::CreateThreadLocalPerCoreMemoryCache(std::move(request));
  Require(!refused.ok() && refused.fail_closed,
          "CEIC-014 budget refusal did not fail closed");
  Require(refused.status.code == StatusCode::memory_limit_exceeded,
          "CEIC-014 budget refusal status mismatch");
  Require(ledger.Snapshot().current_bytes == 0,
          "CEIC-014 budget refusal changed ledger current bytes");
}

void ValidateAuthorityFailClosed(memory::HierarchicalMemoryBudgetLedger* ledger,
                                 memory::MemoryManager* manager) {
  auto request = CacheRequest(ledger, manager, "authority-refusal");
  request.authority.memory_visibility_or_finality_authority = true;
  auto refused = memory::CreateThreadLocalPerCoreMemoryCache(std::move(request));
  Require(!refused.ok() && refused.fail_closed,
          "CEIC-014 unsafe authority did not fail closed");
  Require(refused.status.code == StatusCode::memory_invalid_request,
          "CEIC-014 unsafe authority status mismatch");
  Require(Contains(refused.evidence, "thread_local_memory_cache.fail_closed=true"),
          "CEIC-014 authority refusal evidence missing");
}

}  // namespace

int main() {
  memory::MemoryManager manager(TestPolicy());
  memory::HierarchicalMemoryBudgetLedger ledger;
  SetBudget(&ledger,
            memory::HierarchicalMemoryScopeKind::process,
            "ceic-014-process",
            8ull * 1024ull * 1024ull);

  ValidateReservationBackedCreationReuseAndEvidence(&ledger, &manager);
  ValidateRemoteFreeHandoff(&ledger, &manager);
  ValidateBudgetRefusal(&manager);
  ValidateAuthorityFailClosed(&ledger, &manager);

  const auto ledger_snapshot = ledger.Snapshot();
  Require(ledger_snapshot.current_bytes == 0,
          "CEIC-014 leaked ledger current bytes");
  Require(ledger_snapshot.active_allocation_count == 0,
          "CEIC-014 leaked active reservations");
  const auto memory_snapshot = manager.Snapshot();
  Require(memory_snapshot.current_bytes == 0,
          "CEIC-014 leaked MemoryManager bytes");
  Require(memory_snapshot.active_allocation_count == 0,
          "CEIC-014 leaked MemoryManager allocations");

  std::cout << "CEIC-014 thread-local per-core cache gate passed\n";
  return 0;
}

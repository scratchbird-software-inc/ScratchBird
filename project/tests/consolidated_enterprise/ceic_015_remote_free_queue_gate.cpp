// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// CEIC-015 focused validation for bounded remote-free queues and ownership reconciliation.
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
    std::string label = "ceic_015_remote_free_queue_gate") {
  memory::HierarchicalMemoryBudgetProvenance provenance;
  provenance.source = memory::HierarchicalMemoryBudgetProvenanceSource::runtime_policy;
  provenance.source_label = std::move(label);
  provenance.engine_mga_authoritative = true;
  provenance.memory_evidence_only = true;
  return provenance;
}

std::vector<memory::HierarchicalMemoryScopeRef> ScopeChain(
    const std::string& suffix) {
  return {{memory::HierarchicalMemoryScopeKind::process, "ceic-015-process"},
          {memory::HierarchicalMemoryScopeKind::database, "ceic-015-database"},
          {memory::HierarchicalMemoryScopeKind::session, "ceic-015-session-" + suffix},
          {memory::HierarchicalMemoryScopeKind::transaction, "ceic-015-transaction-" + suffix},
          {memory::HierarchicalMemoryScopeKind::statement, "ceic-015-statement-" + suffix},
          {memory::HierarchicalMemoryScopeKind::query, "ceic-015-query-" + suffix},
          {memory::HierarchicalMemoryScopeKind::operator_scope, "ceic-015-operator-" + suffix}};
}

void SetBudget(memory::HierarchicalMemoryBudgetLedger* ledger,
               memory::HierarchicalMemoryScopeKind kind,
               const std::string& scope_id,
               u64 hard_limit) {
  memory::HierarchicalMemoryBudget budget;
  budget.scope = {kind, scope_id};
  budget.hard_limit_bytes = hard_limit;
  budget.provenance = Provenance();
  Require(ledger->SetBudget(std::move(budget)).ok(), "CEIC-015 budget setup failed");
}

memory::ThreadLocalCacheRequest CacheRequest(
    memory::HierarchicalMemoryBudgetLedger* ledger,
    memory::MemoryManager* manager,
    std::string route,
    u64 reservation_bytes = 32768) {
  SetBudget(ledger,
            memory::HierarchicalMemoryScopeKind::operator_scope,
            "ceic-015-operator-" + route,
            reservation_bytes * 2);

  memory::ThreadLocalCacheRequest request;
  request.reservation_ledger = ledger;
  request.memory_manager = manager;
  request.scope_chain = ScopeChain(route);
  request.category = memory::MemoryCategory::executor_query_reserved;
  request.memory_class = "ceic_015.remote_free_queue";
  request.reservation_bytes = reservation_bytes;
  request.owner_id = route;
  request.route_label = route;
  request.operation_id = route + ".operation";
  request.purpose = route + ".thread_local_cache";
  request.object_kind = memory::TypedSlabPoolObjectKind::executor_frame;
  request.size_classes = {{64, 8}, {128, 8}, {256, 4}};
  request.refill_batch_count = 2;
  request.max_cached_slots_per_class_per_thread = 4;
  request.max_cached_bytes_per_thread = 4096;
  request.remote_free_queue_max_count = 4;
  request.remote_free_queue_max_bytes = 4096;
  request.remote_free_pressure_drain_count = 1;
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
  policy.policy_name = "ceic_015_remote_free_queue_gate";
  policy.byte_limit = 8ull * 1024ull * 1024ull;
  policy.hard_limit_bytes = 8ull * 1024ull * 1024ull;
  policy.track_allocations = true;
  policy.zero_memory_on_release = true;
  return policy;
}

void ValidateRemoteQueueEnqueueDrainAndReuse(
    memory::HierarchicalMemoryBudgetLedger* ledger,
    memory::MemoryManager* manager) {
  auto acquired = memory::CreateThreadLocalPerCoreMemoryCache(
      CacheRequest(ledger, manager, "queue-reuse"));
  Require(acquired.ok(), "CEIC-015 queue-reuse acquisition failed");
  Require(Contains(acquired.evidence, "CEIC-015_REMOTE_FREE_QUEUES_OWNERSHIP_RECONCILIATION"),
          "CEIC-015 remote-free anchor missing");
  auto cache = std::move(acquired.cache);

  auto allocated = cache->Allocate({48, 0, "owner-allocation"});
  Require(allocated.ok(), "CEIC-015 owner allocation failed");
  std::memset(allocated.pointer, 0x44, allocated.requested_bytes);

  memory::ThreadLocalCacheFreeResult remote_freed;
  std::thread remote([&]() {
    remote_freed = cache->Free(allocated.pointer);
  });
  remote.join();
  Require(remote_freed.ok(), "CEIC-015 remote free failed");
  Require(remote_freed.remote_free_handoff,
          "CEIC-015 remote free did not report handoff");
  Require(remote_freed.remote_free_enqueued,
          "CEIC-015 remote free was not enqueued");
  Require(remote_freed.remote_free_queue_current_count == 1,
          "CEIC-015 remote queue count after enqueue mismatch");
  Require(Contains(remote_freed.evidence, "thread_local_memory_cache.remote_free.enqueued=true"),
          "CEIC-015 remote enqueue evidence missing");

  auto after_remote = cache->Snapshot();
  Require(after_remote.remote_free_enqueued_count == 1,
          "CEIC-015 snapshot remote enqueue count missing");
  Require(after_remote.remote_free_queue_current_count == 1,
          "CEIC-015 snapshot queue current count missing");
  Require(after_remote.current_active_objects == 0,
          "CEIC-015 remote enqueue left active owner object");

  auto reused = cache->Allocate({48, 0, "owner-reuse"});
  Require(reused.ok() && reused.cache_hit,
          "CEIC-015 owner did not reuse remote-freed queued block");
  auto after_reuse = cache->Snapshot();
  Require(after_reuse.remote_free_dequeued_count == 1,
          "CEIC-015 snapshot remote dequeue count missing");
  Require(after_reuse.remote_free_drained_count == 1,
          "CEIC-015 snapshot remote drained count missing");
  Require(after_reuse.remote_free_queue_current_count == 0,
          "CEIC-015 remote queue did not drain to zero");
  Require(!after_reuse.threads.empty() &&
              !after_reuse.threads.front().remote_thread_hashes.empty(),
          "CEIC-015 remote thread hash evidence missing");
  Require(after_reuse.remote_free_enqueue_latency_p50_ns > 0 ||
              after_reuse.remote_free_enqueue_latency_p95_ns > 0 ||
              after_reuse.remote_free_enqueue_latency_p99_ns > 0,
          "CEIC-015 remote enqueue latency evidence missing");

  Require(cache->Free(reused.pointer).ok(), "CEIC-015 reused pointer free failed");
  Require(cache->ReconcileAndRelease().ok(), "CEIC-015 queue-reuse release failed");
}

void ValidateBackpressureAndDirectReconcile(
    memory::HierarchicalMemoryBudgetLedger* ledger,
    memory::MemoryManager* manager) {
  auto request = CacheRequest(ledger, manager, "backpressure");
  request.remote_free_queue_max_count = 1;
  request.remote_free_queue_max_bytes = 4096;
  request.remote_free_pressure_drain_count = 1;
  auto acquired = memory::CreateThreadLocalPerCoreMemoryCache(std::move(request));
  Require(acquired.ok(), "CEIC-015 backpressure acquisition failed");
  auto cache = std::move(acquired.cache);

  auto first = cache->Allocate({48, 0, "first"});
  auto second = cache->Allocate({48, 0, "second"});
  Require(first.ok() && second.ok(), "CEIC-015 backpressure allocations failed");

  memory::ThreadLocalCacheFreeResult first_free;
  std::thread remote_first([&]() {
    first_free = cache->Free(first.pointer);
  });
  remote_first.join();
  Require(first_free.ok() && first_free.remote_free_enqueued,
          "CEIC-015 first remote free did not enqueue");

  memory::ThreadLocalCacheFreeResult second_free;
  std::thread remote_second([&]() {
    second_free = cache->Free(second.pointer);
  });
  remote_second.join();
  Require(second_free.ok(), "CEIC-015 second remote free failed");
  Require(second_free.remote_free_backpressure,
          "CEIC-015 queue pressure was not reported");
  Require(second_free.remote_free_dequeued_count == 1,
          "CEIC-015 pressure drain count mismatch");

  auto snapshot = cache->Snapshot();
  Require(snapshot.remote_free_backpressure_count == 1,
          "CEIC-015 snapshot backpressure count missing");
  Require(snapshot.remote_free_dequeued_count >= 1,
          "CEIC-015 snapshot pressure dequeue missing");
  Require(cache->ReconcileAndRelease().ok(),
          "CEIC-015 backpressure release failed");

  auto direct_request = CacheRequest(ledger, manager, "direct-reconcile");
  direct_request.remote_free_queue_max_count = 1;
  direct_request.remote_free_queue_max_bytes = 32;
  auto direct_acquired =
      memory::CreateThreadLocalPerCoreMemoryCache(std::move(direct_request));
  Require(direct_acquired.ok(), "CEIC-015 direct acquisition failed");
  auto direct_cache = std::move(direct_acquired.cache);
  auto direct_allocated = direct_cache->Allocate({48, 0, "direct"});
  Require(direct_allocated.ok(), "CEIC-015 direct allocation failed");

  memory::ThreadLocalCacheFreeResult direct_free;
  std::thread remote_direct([&]() {
    direct_free = direct_cache->Free(direct_allocated.pointer);
  });
  remote_direct.join();
  Require(direct_free.ok(), "CEIC-015 direct remote free failed");
  Require(direct_free.direct_reconciled_to_backing_pool,
          "CEIC-015 direct reconcile did not occur");
  Require(direct_free.remote_free_rejected,
          "CEIC-015 direct reconcile did not report queue rejection");
  auto direct_snapshot = direct_cache->Snapshot();
  Require(direct_snapshot.remote_free_direct_reconcile_count >= 1,
          "CEIC-015 direct reconcile snapshot missing");
  Require(direct_cache->ReconcileAndRelease().ok(),
          "CEIC-015 direct release failed");
}

void ValidateStaleOwnerCleanup(memory::HierarchicalMemoryBudgetLedger* ledger,
                               memory::MemoryManager* manager) {
  auto acquired = memory::CreateThreadLocalPerCoreMemoryCache(
      CacheRequest(ledger, manager, "stale-owner"));
  Require(acquired.ok(), "CEIC-015 stale-owner acquisition failed");
  auto cache = std::move(acquired.cache);

  void* escaped_pointer = nullptr;
  std::thread owner([&]() {
    auto allocated = cache->Allocate({48, 0, "owner-thread"});
    Require(allocated.ok(), "CEIC-015 owner-thread allocation failed");
    escaped_pointer = allocated.pointer;
  });
  owner.join();
  Require(escaped_pointer != nullptr, "CEIC-015 owner pointer did not escape");

  auto before_free = cache->Snapshot();
  Require(before_free.thread_exit_cleanup_count >= 1,
          "CEIC-015 thread-exit cleanup count missing");
  Require(before_free.remote_free_stale_owner_pending_count >= 1,
          "CEIC-015 stale owner pending count missing");

  auto stale_free = cache->Free(escaped_pointer);
  Require(stale_free.ok(), "CEIC-015 stale owner free failed");
  Require(stale_free.stale_owner,
          "CEIC-015 stale owner result not reported");
  Require(stale_free.direct_reconciled_to_backing_pool,
          "CEIC-015 stale owner did not direct reconcile");
  Require(Contains(stale_free.evidence, "thread_local_memory_cache.remote_free.stale_owner=true"),
          "CEIC-015 stale owner evidence missing");
  auto after_free = cache->Snapshot();
  Require(after_free.remote_free_stale_owner_count >= 1,
          "CEIC-015 stale owner snapshot count missing");
  Require(cache->ReconcileAndRelease().ok(),
          "CEIC-015 stale owner release failed");
}

void ValidateAuthorityAndExactCleanup(memory::HierarchicalMemoryBudgetLedger* ledger,
                                      memory::MemoryManager* manager) {
  auto request = CacheRequest(ledger, manager, "authority-refusal");
  request.authority.agent_action_authority = true;
  auto refused = memory::CreateThreadLocalPerCoreMemoryCache(std::move(request));
  Require(!refused.ok() && refused.fail_closed,
          "CEIC-015 unsafe authority was accepted");
  Require(refused.status.code == StatusCode::memory_invalid_request,
          "CEIC-015 authority refusal status mismatch");
  Require(Contains(refused.evidence,
                   "thread_local_memory_cache.fail_closed=true"),
          "CEIC-015 authority fail-closed evidence missing");
}

}  // namespace

int main() {
  memory::MemoryManager manager(TestPolicy());
  memory::HierarchicalMemoryBudgetLedger ledger;
  SetBudget(&ledger,
            memory::HierarchicalMemoryScopeKind::process,
            "ceic-015-process",
            8ull * 1024ull * 1024ull);

  ValidateRemoteQueueEnqueueDrainAndReuse(&ledger, &manager);
  ValidateBackpressureAndDirectReconcile(&ledger, &manager);
  ValidateStaleOwnerCleanup(&ledger, &manager);
  ValidateAuthorityAndExactCleanup(&ledger, &manager);

  const auto ledger_snapshot = ledger.Snapshot();
  Require(ledger_snapshot.current_bytes == 0,
          "CEIC-015 leaked ledger current bytes");
  Require(ledger_snapshot.active_allocation_count == 0,
          "CEIC-015 leaked active reservations");
  const auto memory_snapshot = manager.Snapshot();
  Require(memory_snapshot.current_bytes == 0,
          "CEIC-015 leaked MemoryManager bytes");
  Require(memory_snapshot.active_allocation_count == 0,
          "CEIC-015 leaked MemoryManager allocations");

  std::cout << "CEIC-015 remote-free queue gate passed\n";
  return 0;
}

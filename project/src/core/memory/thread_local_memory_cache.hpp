// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// CEIC-014/CEIC-015: reservation-backed thread-local caches with remote-free
// ownership reconciliation.
#include "memory_locality_policy.hpp"
#include "typed_slab_pool.hpp"

#include <atomic>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace scratchbird::core::memory {

struct ThreadLocalCacheAuthority {
  bool engine_mga_authoritative = true;
  bool transaction_inventory_authoritative = true;
  bool security_or_policy_checked = true;
  bool parser_or_reference_finality_authority = false;
  bool memory_visibility_or_finality_authority = false;
  bool memory_recovery_authority = false;
  bool memory_authorization_authority = false;
  bool benchmark_authority = false;
  bool cluster_authority = false;
  bool debug_or_relaxed_path = false;
  bool optimizer_plan_authority = false;
  bool index_finality_authority = false;
  bool agent_action_authority = false;
};

struct ThreadLocalCacheRequest {
  HierarchicalMemoryBudgetLedger* reservation_ledger = nullptr;
  MemoryManager* memory_manager = nullptr;
  std::vector<HierarchicalMemoryScopeRef> scope_chain;
  MemoryCategory category = MemoryCategory::unknown;
  std::string memory_class = "thread_local_cache";
  u64 reservation_bytes = 0;
  std::string owner_id;
  std::string route_label;
  std::string operation_id;
  std::string purpose = "thread_local_per_core_cache";
  TypedSlabPoolObjectKind object_kind = TypedSlabPoolObjectKind::executor_frame;
  std::vector<SizeClassConfig> size_classes;
  usize refill_batch_count = 4;
  usize max_cached_slots_per_class_per_thread = 16;
  u64 max_cached_bytes_per_thread = 64ull * 1024ull;
  usize remote_free_queue_max_count = 64;
  u64 remote_free_queue_max_bytes = 64ull * 1024ull;
  usize remote_free_pressure_drain_count = 8;
  usize shard_count = 0;
  bool spillable = false;
  bool cancelable = true;
  int priority = 0;
  u64 weight = 1;
  u64 lease_expires_at_ms = 0;
  bool production_like = true;
  MemoryLocalityPolicy locality_policy;
  HierarchicalMemoryBudgetProvenance provenance;
  ThreadLocalCacheAuthority authority;
};

struct ThreadLocalCacheAllocationRequest {
  usize bytes = 0;
  usize alignment = 0;
  std::string purpose;
};

struct ThreadLocalCacheAllocationResult {
  Status status;
  bool fail_closed = false;
  void* pointer = nullptr;
  usize requested_bytes = 0;
  usize size_class_bytes = 0;
  usize size_class_index = 0;
  bool cache_hit = false;
  bool refill_performed = false;
  u64 latency_ns = 0;
  DiagnosticRecord diagnostic;
  std::vector<std::string> evidence;

  bool ok() const {
    return status.ok() && !fail_closed && pointer != nullptr;
  }
};

struct ThreadLocalCacheFreeResult {
  Status status;
  bool fail_closed = false;
  bool freed = false;
  bool cached_for_reuse = false;
  bool drained_to_backing_pool = false;
  bool remote_free_handoff = false;
  bool remote_free_enqueued = false;
  bool remote_free_dequeued = false;
  bool remote_free_rejected = false;
  bool remote_free_backpressure = false;
  bool direct_reconciled_to_backing_pool = false;
  bool stale_owner = false;
  u64 remote_free_enqueue_latency_ns = 0;
  u64 remote_free_reconcile_latency_ns = 0;
  u64 remote_free_enqueued_count = 0;
  u64 remote_free_dequeued_count = 0;
  u64 remote_free_drained_count = 0;
  u64 remote_free_rejected_count = 0;
  u64 remote_free_queue_current_count = 0;
  u64 remote_free_queue_current_bytes = 0;
  DiagnosticRecord diagnostic;
  std::vector<std::string> evidence;

  bool ok() const {
    return status.ok() && !fail_closed && freed;
  }
};

struct ThreadLocalCacheThreadSnapshot {
  u64 thread_id_hash = 0;
  usize shard_index = 0;
  bool current_core_available = false;
  usize core_id = 0;
  bool numa_node_available = false;
  int numa_node = -1;
  std::string owner_id;
  std::string context_id;
  MemoryCategory category = MemoryCategory::unknown;
  u64 hit_count = 0;
  u64 miss_count = 0;
  u64 refill_count = 0;
  u64 drain_count = 0;
  u64 remote_free_handoff_count = 0;
  u64 remote_free_enqueued_count = 0;
  u64 remote_free_dequeued_count = 0;
  u64 remote_free_drained_count = 0;
  u64 remote_free_rejected_count = 0;
  u64 remote_free_backpressure_count = 0;
  u64 remote_free_direct_reconcile_count = 0;
  u64 remote_free_stale_owner_count = 0;
  u64 remote_free_queue_current_count = 0;
  u64 remote_free_queue_peak_count = 0;
  u64 remote_free_queue_current_bytes = 0;
  u64 remote_free_queue_peak_bytes = 0;
  u64 allocation_count = 0;
  u64 free_count = 0;
  u64 reconciliation_count = 0;
  u64 current_cached_bytes = 0;
  u64 peak_cached_bytes = 0;
  u64 current_cached_objects = 0;
  u64 peak_cached_objects = 0;
  u64 current_active_bytes = 0;
  u64 current_active_objects = 0;
  u64 allocation_latency_p50_ns = 0;
  u64 allocation_latency_p95_ns = 0;
  u64 allocation_latency_p99_ns = 0;
  u64 remote_free_enqueue_latency_p50_ns = 0;
  u64 remote_free_enqueue_latency_p95_ns = 0;
  u64 remote_free_enqueue_latency_p99_ns = 0;
  u64 remote_free_reconcile_latency_p50_ns = 0;
  u64 remote_free_reconcile_latency_p95_ns = 0;
  u64 remote_free_reconcile_latency_p99_ns = 0;
  std::vector<u64> remote_thread_hashes;
  std::vector<std::string> evidence;
};

struct ThreadLocalCacheShardSnapshot {
  usize shard_index = 0;
  u64 hit_count = 0;
  u64 miss_count = 0;
  u64 refill_count = 0;
  u64 drain_count = 0;
  u64 remote_free_handoff_count = 0;
  u64 remote_free_enqueued_count = 0;
  u64 remote_free_dequeued_count = 0;
  u64 remote_free_drained_count = 0;
  u64 remote_free_rejected_count = 0;
  u64 remote_free_backpressure_count = 0;
  u64 remote_free_queue_current_count = 0;
  u64 remote_free_queue_peak_count = 0;
  u64 remote_free_queue_current_bytes = 0;
  u64 remote_free_queue_peak_bytes = 0;
  u64 current_cached_bytes = 0;
  u64 peak_cached_bytes = 0;
};

struct ThreadLocalCacheSnapshot {
  std::string owner_id;
  std::string route_label;
  std::string operation_id;
  std::string memory_class;
  MemoryCategory category = MemoryCategory::unknown;
  TypedSlabPoolObjectKind object_kind = TypedSlabPoolObjectKind::executor_frame;
  u64 reservation_bytes = 0;
  bool active = false;
  bool locality_portable_fallback_used = false;
  bool numa_hint_applied = false;
  bool numa_node_discovered = false;
  usize shard_count = 0;
  u64 hit_count = 0;
  u64 miss_count = 0;
  u64 refill_count = 0;
  u64 drain_count = 0;
  u64 remote_free_handoff_count = 0;
  u64 remote_free_enqueued_count = 0;
  u64 remote_free_dequeued_count = 0;
  u64 remote_free_drained_count = 0;
  u64 remote_free_rejected_count = 0;
  u64 remote_free_backpressure_count = 0;
  u64 remote_free_direct_reconcile_count = 0;
  u64 remote_free_stale_owner_count = 0;
  u64 remote_free_stale_owner_pending_count = 0;
  u64 remote_free_queue_current_count = 0;
  u64 remote_free_queue_peak_count = 0;
  u64 remote_free_queue_current_bytes = 0;
  u64 remote_free_queue_peak_bytes = 0;
  u64 thread_exit_cleanup_count = 0;
  u64 thread_exit_reconciled_count = 0;
  u64 thread_exit_reconciled_bytes = 0;
  u64 allocation_count = 0;
  u64 free_count = 0;
  u64 reconciliation_count = 0;
  u64 current_cached_bytes = 0;
  u64 peak_cached_bytes = 0;
  u64 current_cached_objects = 0;
  u64 peak_cached_objects = 0;
  u64 current_active_bytes = 0;
  u64 current_active_objects = 0;
  u64 allocation_latency_p50_ns = 0;
  u64 allocation_latency_p95_ns = 0;
  u64 allocation_latency_p99_ns = 0;
  u64 remote_free_enqueue_latency_p50_ns = 0;
  u64 remote_free_enqueue_latency_p95_ns = 0;
  u64 remote_free_enqueue_latency_p99_ns = 0;
  u64 remote_free_reconcile_latency_p50_ns = 0;
  u64 remote_free_reconcile_latency_p95_ns = 0;
  u64 remote_free_reconcile_latency_p99_ns = 0;
  u64 backing_pool_retained_bytes = 0;
  u64 backing_pool_allocation_count = 0;
  u64 backing_pool_reuse_count = 0;
  std::string authority_scope;
  std::vector<std::string> evidence;
  std::vector<ThreadLocalCacheThreadSnapshot> threads;
  std::vector<ThreadLocalCacheShardSnapshot> shards;
};

struct ThreadLocalCacheAcquireResult {
  Status status;
  bool fail_closed = false;
  std::unique_ptr<class ThreadLocalPerCoreMemoryCache> cache;
  DiagnosticRecord diagnostic;
  std::vector<std::string> evidence;

  bool ok() const {
    return status.ok() && !fail_closed && cache != nullptr;
  }
};

class ThreadLocalPerCoreMemoryCache {
 public:
  ThreadLocalPerCoreMemoryCache(const ThreadLocalPerCoreMemoryCache&) = delete;
  ThreadLocalPerCoreMemoryCache& operator=(const ThreadLocalPerCoreMemoryCache&) = delete;
  ~ThreadLocalPerCoreMemoryCache();

  ThreadLocalCacheAllocationResult Allocate(
      ThreadLocalCacheAllocationRequest request);
  ThreadLocalCacheFreeResult Free(void* pointer);
  ThreadLocalCacheFreeResult DrainCurrentThread();
  ThreadLocalCacheFreeResult ReconcileAndRelease();
  ThreadLocalCacheSnapshot Snapshot() const;

 private:
  struct CachedBlock {
    void* pointer = nullptr;
    usize requested_bytes = 0;
    usize size_class_bytes = 0;
    usize size_class_index = 0;
  };

  struct ActiveBlock {
    void* pointer = nullptr;
    usize requested_bytes = 0;
    usize size_class_bytes = 0;
    usize size_class_index = 0;
  };

  struct ThreadCacheState;

  struct RemoteFreeRecord {
    CachedBlock block;
    u64 owner_thread_hash = 0;
    usize owner_shard_index = 0;
    usize owner_core_id = 0;
    int owner_numa_node = -1;
    u64 remote_thread_hash = 0;
    u64 enqueue_time_ns = 0;
  };

  struct ActiveOwnerRecord {
    CachedBlock block;
    std::weak_ptr<ThreadCacheState> owner_state;
    std::thread::id owner_thread_id;
    u64 owner_thread_hash = 0;
    usize owner_shard_index = 0;
    usize owner_core_id = 0;
    int owner_numa_node = -1;
  };

  struct ThreadCacheState {
    ~ThreadCacheState();

    ThreadLocalPerCoreMemoryCache* owner = nullptr;
    std::thread::id thread_id;
    u64 thread_id_hash = 0;
    usize shard_index = 0;
    bool current_core_available = false;
    usize core_id = 0;
    bool numa_node_available = false;
    int numa_node = -1;
    mutable std::mutex mutex;
    bool thread_exiting = false;
    std::vector<std::vector<CachedBlock>> cached_by_class;
    std::unordered_map<void*, ActiveBlock> active;
    std::deque<RemoteFreeRecord> remote_free_queue;
    std::vector<u64> allocation_latencies_ns;
    std::vector<u64> remote_free_enqueue_latencies_ns;
    std::vector<u64> remote_free_reconcile_latencies_ns;
    std::vector<u64> remote_thread_hashes;
    u64 hit_count = 0;
    u64 miss_count = 0;
    u64 refill_count = 0;
    u64 drain_count = 0;
    u64 remote_free_handoff_count = 0;
    u64 remote_free_enqueued_count = 0;
    u64 remote_free_dequeued_count = 0;
    u64 remote_free_drained_count = 0;
    u64 remote_free_rejected_count = 0;
    u64 remote_free_backpressure_count = 0;
    u64 remote_free_direct_reconcile_count = 0;
    u64 remote_free_stale_owner_count = 0;
    u64 remote_free_queue_current_bytes = 0;
    u64 remote_free_queue_peak_count = 0;
    u64 remote_free_queue_peak_bytes = 0;
    u64 allocation_count = 0;
    u64 free_count = 0;
    u64 reconciliation_count = 0;
    u64 current_cached_bytes = 0;
    u64 peak_cached_bytes = 0;
    u64 current_cached_objects = 0;
    u64 peak_cached_objects = 0;
    u64 current_active_bytes = 0;
    u64 current_active_objects = 0;
    std::vector<std::string> topology_evidence;
  };

  struct CoreShard {
    std::atomic<u64> hit_count{0};
    std::atomic<u64> miss_count{0};
    std::atomic<u64> refill_count{0};
    std::atomic<u64> drain_count{0};
    std::atomic<u64> remote_free_handoff_count{0};
    std::atomic<u64> remote_free_enqueued_count{0};
    std::atomic<u64> remote_free_dequeued_count{0};
    std::atomic<u64> remote_free_drained_count{0};
    std::atomic<u64> remote_free_rejected_count{0};
    std::atomic<u64> remote_free_backpressure_count{0};
    std::atomic<u64> remote_free_queue_current_count{0};
    std::atomic<u64> remote_free_queue_peak_count{0};
    std::atomic<u64> remote_free_queue_current_bytes{0};
    std::atomic<u64> remote_free_queue_peak_bytes{0};
    std::atomic<u64> current_cached_bytes{0};
    std::atomic<u64> peak_cached_bytes{0};
  };

  ThreadLocalPerCoreMemoryCache(ThreadLocalCacheRequest request,
                                MemoryLocalityDecision locality_decision,
                                std::unique_ptr<SizeClassAllocator> allocator,
                                usize shard_count,
                                std::vector<std::string> topology_evidence);

  std::shared_ptr<ThreadCacheState> CurrentThreadState();
  std::shared_ptr<ThreadCacheState> CreateThreadState();
  usize FindSizeClass(usize bytes) const;
  ThreadLocalCacheAllocationResult RefuseAllocation(
      ThreadLocalCacheAllocationRequest request,
      std::string diagnostic_code,
      std::string message_key,
      std::string reason,
      scratchbird::core::platform::StatusCode code) const;
  ThreadLocalCacheFreeResult RefuseFree(std::string diagnostic_code,
                                        std::string message_key,
                                        std::string reason,
                                        scratchbird::core::platform::StatusCode code) const;
  bool StoreCachedBlockLocked(ThreadCacheState* state, CachedBlock block);
  bool RemoteQueueCanAcceptLocked(const ThreadCacheState& state,
                                  const CachedBlock& block) const;
  void RegisterRemoteQueueBytesLocked(ThreadCacheState* state,
                                      u64 bytes);
  void UnregisterRemoteQueueBytesLocked(ThreadCacheState* state,
                                        u64 bytes);
  void RecordRemoteThreadHashLocked(ThreadCacheState* state,
                                    u64 remote_thread_hash) const;
  void RecordRemoteEnqueueLatency(ThreadCacheState* state,
                                  u64 latency_ns) const;
  void RecordRemoteReconcileLatency(ThreadCacheState* state,
                                    u64 latency_ns) const;
  void RecordGlobalRemoteEnqueueLatency(u64 latency_ns);
  void RecordGlobalRemoteReconcileLatency(u64 latency_ns);
  ThreadLocalCacheFreeResult ReconcileRemoteQueue(
      const std::shared_ptr<ThreadCacheState>& state,
      const char* reason,
      bool cache_for_reuse,
      usize max_records = 0);
  ThreadLocalCacheFreeResult ThreadExitCleanupRaw(ThreadCacheState* state);
  ThreadLocalCacheFreeResult DrainState(const std::shared_ptr<ThreadCacheState>& state,
                                        bool include_active,
                                        const char* reason);
  ThreadLocalCacheFreeResult DrainStateRaw(ThreadCacheState* state,
                                           bool include_active,
                                           const char* reason);
  bool FreeBlockToBackingPool(CachedBlock block, ThreadLocalCacheFreeResult* result);
  void AppendBaseEvidence(std::vector<std::string>* evidence) const;
  ThreadLocalCacheThreadSnapshot SnapshotStateLocked(
      const ThreadCacheState& state) const;
  void RecordLatency(ThreadCacheState* state, u64 latency_ns) const;
  void RegisterCachedBytes(ThreadCacheState* state, u64 bytes);
  void UnregisterCachedBytes(ThreadCacheState* state, u64 bytes);
  void IncrementShardHit(usize shard_index);
  void IncrementShardMiss(usize shard_index);
  void IncrementShardRefill(usize shard_index);
  void IncrementShardDrain(usize shard_index);
  void IncrementShardRemoteFree(usize shard_index);
  void IncrementShardRemoteEnqueue(usize shard_index);
  void IncrementShardRemoteDequeue(usize shard_index);
  void IncrementShardRemoteDrain(usize shard_index);
  void IncrementShardRemoteReject(usize shard_index);
  void IncrementShardRemoteBackpressure(usize shard_index);
  std::vector<std::shared_ptr<ThreadCacheState>> LiveThreadStates() const;

  ThreadLocalCacheRequest request_;
  MemoryLocalityDecision locality_decision_;
  std::unique_ptr<SizeClassAllocator> allocator_;
  std::vector<SizeClassConfig> size_classes_;
  std::vector<std::unique_ptr<CoreShard>> shards_;
  std::vector<std::string> topology_evidence_;
  mutable std::mutex registry_mutex_;
  std::vector<std::weak_ptr<ThreadCacheState>> registry_;
  mutable std::mutex ownership_mutex_;
  std::unordered_map<void*, ActiveOwnerRecord> ownership_;
  mutable std::mutex allocator_mutex_;
  mutable std::mutex remote_latency_mutex_;
  std::vector<u64> remote_free_enqueue_latencies_ns_;
  std::vector<u64> remote_free_reconcile_latencies_ns_;
  std::atomic<u64> remote_free_direct_reconcile_count_{0};
  std::atomic<u64> remote_free_stale_owner_count_{0};
  std::atomic<u64> thread_exit_cleanup_count_{0};
  std::atomic<u64> thread_exit_reconciled_count_{0};
  std::atomic<u64> thread_exit_reconciled_bytes_{0};
  std::atomic<bool> released_{false};

  friend ThreadLocalCacheAcquireResult CreateThreadLocalPerCoreMemoryCache(
      ThreadLocalCacheRequest request);
};

ThreadLocalCacheAcquireResult CreateThreadLocalPerCoreMemoryCache(
    ThreadLocalCacheRequest request);

}  // namespace scratchbird::core::memory

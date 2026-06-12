// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// CEIC-014/CEIC-015: reservation-backed thread-local caches with bounded
// remote-free ownership reconciliation.
#include "thread_local_memory_cache.hpp"

#include "runtime_topology.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <limits>
#include <utility>

namespace scratchbird::core::memory {
namespace {

using scratchbird::core::platform::DiagnosticArgument;
using scratchbird::core::platform::MakeDiagnostic;
using scratchbird::core::platform::Severity;
using scratchbird::core::platform::StatusCode;
using scratchbird::core::platform::Subsystem;
using scratchbird::core::platform::RuntimeTopologySnapshot;

constexpr unsigned char kCacheFreePoison = 0xdc;
constexpr unsigned char kCacheAllocatedPoison = 0xa5;
constexpr usize kDefaultRefillBatchCount = 4;
constexpr usize kDefaultMaxCachedSlotsPerClass = 16;
constexpr u64 kDefaultMaxCachedBytesPerThread = 64ull * 1024ull;
constexpr usize kDefaultRemoteFreeQueueMaxCount = 64;
constexpr u64 kDefaultRemoteFreeQueueMaxBytes = 64ull * 1024ull;
constexpr usize kDefaultRemoteFreePressureDrainCount = 8;
constexpr usize kMaxLatencySamplesPerThread = 1024;
constexpr usize kMaxRemoteLatencySamples = 4096;
constexpr const char* kAnchor = "CEIC-014_THREAD_LOCAL_PER_CORE_NUMA_CACHE";
constexpr const char* kRemoteAnchor =
    "CEIC-015_REMOTE_FREE_QUEUES_OWNERSHIP_RECONCILIATION";
constexpr const char* kAuthorityScope =
    "thread_local_memory_cache.authority_scope=evidence_only_not_transaction_finality_visibility_recovery_parser_reference_benchmark_cluster_authorization_optimizer_plan_index_finality_or_agent_action_authority";

Status OkStatus() {
  return {StatusCode::ok, Severity::info, Subsystem::memory};
}

Status ErrorStatus(StatusCode code) {
  return {code, Severity::error, Subsystem::memory};
}

bool Blank(const std::string& value) {
  return value.find_first_not_of(" \t\r\n") == std::string::npos;
}

std::string BoolText(bool value) {
  return value ? "true" : "false";
}

u64 ThreadHash(std::thread::id id) {
  return static_cast<u64>(std::hash<std::thread::id>{}(id));
}

u64 NowNs() {
  return static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count());
}

u64 Percentile(std::vector<u64> values, unsigned percentile) {
  if (values.empty()) {
    return 0;
  }
  std::sort(values.begin(), values.end());
  const usize index = static_cast<usize>(
      ((static_cast<unsigned long long>(values.size() - 1) * percentile) + 99ull) / 100ull);
  return values[std::min(index, values.size() - 1)];
}

void UpdateAtomicPeak(std::atomic<u64>* peak, u64 value) {
  u64 observed = peak->load(std::memory_order_relaxed);
  while (observed < value &&
         !peak->compare_exchange_weak(observed, value, std::memory_order_relaxed)) {
  }
}

void SaturatingAtomicSubtract(std::atomic<u64>* value, u64 decrement) {
  u64 observed = value->load(std::memory_order_relaxed);
  for (;;) {
    const u64 next = observed >= decrement ? observed - decrement : 0;
    if (value->compare_exchange_weak(observed, next, std::memory_order_relaxed)) {
      return;
    }
  }
}

bool UnsafeAuthority(const ThreadLocalCacheRequest& request, std::string* reason) {
  const auto& authority = request.authority;
  if (!authority.engine_mga_authoritative) {
    *reason = "engine_mga_authority_required";
    return true;
  }
  if (request.production_like && !authority.transaction_inventory_authoritative) {
    *reason = "transaction_inventory_authority_required";
    return true;
  }
  if (!authority.security_or_policy_checked) {
    *reason = "security_or_policy_check_required";
    return true;
  }
  if (authority.parser_or_reference_finality_authority ||
      authority.memory_visibility_or_finality_authority ||
      authority.memory_recovery_authority ||
      authority.memory_authorization_authority ||
      authority.benchmark_authority ||
      authority.cluster_authority ||
      authority.debug_or_relaxed_path ||
      authority.optimizer_plan_authority ||
      authority.index_finality_authority ||
      authority.agent_action_authority) {
    *reason = "unsafe_authority_claim_refused";
    return true;
  }
  return false;
}

void AppendTopologyEvidence(std::vector<std::string>* evidence,
                            const RuntimeTopologySnapshot& topology) {
  evidence->insert(evidence->end(), topology.evidence.begin(), topology.evidence.end());
}

DiagnosticRecord MakeCacheDiagnostic(
    Status status,
    std::string diagnostic_code,
    std::string message_key,
    const ThreadLocalCacheRequest& request,
    std::vector<DiagnosticArgument> arguments = {}) {
  arguments.push_back({"slice", kAnchor});
  arguments.push_back({"authority_scope", kAuthorityScope});
  arguments.push_back({"owner_id", request.owner_id});
  arguments.push_back({"route_label", request.route_label});
  arguments.push_back({"operation_id", request.operation_id});
  arguments.push_back({"memory_class", request.memory_class});
  arguments.push_back({"category", MemoryCategoryName(request.category)});
  arguments.push_back({"object_kind", TypedSlabPoolObjectKindName(request.object_kind)});
  arguments.push_back({"reservation_bytes", std::to_string(request.reservation_bytes)});
  return MakeDiagnostic(status.code,
                        status.severity,
                        status.subsystem,
                        std::move(diagnostic_code),
                        std::move(message_key),
                        std::move(arguments),
                        {},
                        "core.memory.thread_local_per_core_cache",
                        status.ok()
                            ? std::string{}
                            : "Use CEIC-011 reservations and CEIC-013 typed slabs; memory cache evidence must not become transaction, parser, reference, optimizer, index, or agent authority.");
}

ThreadLocalCacheAcquireResult RefuseAcquire(
    ThreadLocalCacheRequest request,
    std::string diagnostic_code,
    std::string message_key,
    std::string reason,
    StatusCode code = StatusCode::memory_invalid_request) {
  ThreadLocalCacheAcquireResult result;
  result.status = ErrorStatus(code);
  result.fail_closed = true;
  result.diagnostic = MakeCacheDiagnostic(
      result.status,
      std::move(diagnostic_code),
      std::move(message_key),
      request,
      {{"reason", std::move(reason)}});
  result.evidence.push_back(kAnchor);
  result.evidence.push_back(kRemoteAnchor);
  result.evidence.push_back(kAuthorityScope);
  result.evidence.push_back("thread_local_memory_cache.fail_closed=true");
  result.evidence.push_back("thread_local_memory_cache.reservation_created=false");
  return result;
}

void NormalizeSizeClasses(std::vector<SizeClassConfig>* size_classes) {
  if (size_classes->empty()) {
    *size_classes = DefaultTypedSlabSizeClasses();
  }
  std::sort(size_classes->begin(),
            size_classes->end(),
            [](const SizeClassConfig& left, const SizeClassConfig& right) {
              return left.payload_bytes < right.payload_bytes;
            });
}

bool ValidateSizeClasses(const std::vector<SizeClassConfig>& size_classes,
                         std::string* reason) {
  usize previous = 0;
  for (const auto& config : size_classes) {
    if (config.payload_bytes == 0 || config.payload_bytes <= previous) {
      *reason = "size_classes_must_be_positive_and_strictly_increasing";
      return false;
    }
    previous = config.payload_bytes;
  }
  return true;
}

}  // namespace

ThreadLocalPerCoreMemoryCache::ThreadCacheState::~ThreadCacheState() {
  ThreadLocalPerCoreMemoryCache* cache = owner;
  if (cache != nullptr) {
    (void)cache->ThreadExitCleanupRaw(this);
  }
}

ThreadLocalPerCoreMemoryCache::ThreadLocalPerCoreMemoryCache(
    ThreadLocalCacheRequest request,
    MemoryLocalityDecision locality_decision,
    std::unique_ptr<SizeClassAllocator> allocator,
    usize shard_count,
    std::vector<std::string> topology_evidence)
    : request_(std::move(request)),
      locality_decision_(std::move(locality_decision)),
      allocator_(std::move(allocator)),
      size_classes_(request_.size_classes),
      topology_evidence_(std::move(topology_evidence)) {
  if (shard_count == 0) {
    shard_count = 1;
  }
  shards_.reserve(shard_count);
  for (usize index = 0; index < shard_count; ++index) {
    shards_.push_back(std::make_unique<CoreShard>());
  }
}

ThreadLocalPerCoreMemoryCache::~ThreadLocalPerCoreMemoryCache() {
  (void)ReconcileAndRelease();
}

void ThreadLocalPerCoreMemoryCache::AppendBaseEvidence(
    std::vector<std::string>* evidence) const {
  evidence->push_back(kAnchor);
  evidence->push_back(kRemoteAnchor);
  evidence->push_back(kAuthorityScope);
  evidence->push_back("thread_local_memory_cache.owner_id=" + request_.owner_id);
  evidence->push_back("thread_local_memory_cache.route_label=" + request_.route_label);
  evidence->push_back("thread_local_memory_cache.operation_id=" + request_.operation_id);
  evidence->push_back("thread_local_memory_cache.memory_class=" + request_.memory_class);
  evidence->push_back("thread_local_memory_cache.category=" +
                      std::string(MemoryCategoryName(request_.category)));
  evidence->push_back("thread_local_memory_cache.object_kind=" +
                      std::string(TypedSlabPoolObjectKindName(request_.object_kind)));
  evidence->push_back("thread_local_memory_cache.reservation_bytes=" +
                      std::to_string(request_.reservation_bytes));
  evidence->push_back("thread_local_memory_cache.remote_free_queue.bounded=true");
  evidence->push_back("thread_local_memory_cache.remote_free_queue.max_count=" +
                      std::to_string(request_.remote_free_queue_max_count));
  evidence->push_back("thread_local_memory_cache.remote_free_queue.max_bytes=" +
                      std::to_string(request_.remote_free_queue_max_bytes));
  evidence->push_back("thread_local_memory_cache.remote_free_queue.pressure_drain_count=" +
                      std::to_string(request_.remote_free_pressure_drain_count));
  evidence->push_back("thread_local_memory_cache.locality_portable_fallback_used=" +
                      BoolText(locality_decision_.portable_fallback_used));
  evidence->push_back("thread_local_memory_cache.numa_hint_applied=" +
                      BoolText(locality_decision_.numa_hint_applied));
  evidence->insert(evidence->end(),
                   locality_decision_.evidence.begin(),
                   locality_decision_.evidence.end());
}

std::shared_ptr<ThreadLocalPerCoreMemoryCache::ThreadCacheState>
ThreadLocalPerCoreMemoryCache::CreateThreadState() {
  auto state = std::make_shared<ThreadCacheState>();
  state->owner = this;
  state->thread_id = std::this_thread::get_id();
  state->thread_id_hash = ThreadHash(state->thread_id);
  state->cached_by_class.resize(size_classes_.size());

  const auto topology = scratchbird::core::platform::CurrentRuntimeTopology();
  state->current_core_available = topology.current_core_available;
  state->core_id = topology.current_core_id;
  state->numa_node_available = topology.current_numa_node_available;
  state->numa_node = topology.current_numa_node;
  state->topology_evidence = topology.evidence;
  if (!shards_.empty()) {
    state->shard_index = topology.current_core_available
                             ? topology.current_core_id % shards_.size()
                             : state->thread_id_hash % shards_.size();
  }

  {
    std::lock_guard<std::mutex> lock(registry_mutex_);
    registry_.push_back(state);
  }
  return state;
}

std::shared_ptr<ThreadLocalPerCoreMemoryCache::ThreadCacheState>
ThreadLocalPerCoreMemoryCache::CurrentThreadState() {
  thread_local std::vector<std::shared_ptr<ThreadCacheState>> thread_states;
  for (auto& state : thread_states) {
    if (state != nullptr && state->owner == this) {
      return state;
    }
  }
  auto state = CreateThreadState();
  thread_states.push_back(state);
  return state;
}

usize ThreadLocalPerCoreMemoryCache::FindSizeClass(usize bytes) const {
  for (usize i = 0; i < size_classes_.size(); ++i) {
    if (bytes <= size_classes_[i].payload_bytes) {
      return i;
    }
  }
  return size_classes_.size();
}

ThreadLocalCacheAllocationResult
ThreadLocalPerCoreMemoryCache::RefuseAllocation(
    ThreadLocalCacheAllocationRequest request,
    std::string diagnostic_code,
    std::string message_key,
    std::string reason,
    StatusCode code) const {
  ThreadLocalCacheAllocationResult result;
  result.status = ErrorStatus(code);
  result.fail_closed = true;
  result.requested_bytes = request.bytes;
  result.diagnostic = MakeCacheDiagnostic(
      result.status,
      std::move(diagnostic_code),
      std::move(message_key),
      request_,
      {{"reason", std::move(reason)},
       {"requested_bytes", std::to_string(request.bytes)},
       {"alignment", std::to_string(request.alignment)}});
  AppendBaseEvidence(&result.evidence);
  result.evidence.push_back("thread_local_memory_cache.allocation.fail_closed=true");
  return result;
}

ThreadLocalCacheFreeResult ThreadLocalPerCoreMemoryCache::RefuseFree(
    std::string diagnostic_code,
    std::string message_key,
    std::string reason,
    StatusCode code) const {
  ThreadLocalCacheFreeResult result;
  result.status = ErrorStatus(code);
  result.fail_closed = true;
  result.diagnostic = MakeCacheDiagnostic(
      result.status,
      std::move(diagnostic_code),
      std::move(message_key),
      request_,
      {{"reason", std::move(reason)}});
  AppendBaseEvidence(&result.evidence);
  result.evidence.push_back("thread_local_memory_cache.free.fail_closed=true");
  return result;
}

void ThreadLocalPerCoreMemoryCache::IncrementShardHit(usize shard_index) {
  if (shard_index < shards_.size()) {
    shards_[shard_index]->hit_count.fetch_add(1, std::memory_order_relaxed);
  }
}

void ThreadLocalPerCoreMemoryCache::IncrementShardMiss(usize shard_index) {
  if (shard_index < shards_.size()) {
    shards_[shard_index]->miss_count.fetch_add(1, std::memory_order_relaxed);
  }
}

void ThreadLocalPerCoreMemoryCache::IncrementShardRefill(usize shard_index) {
  if (shard_index < shards_.size()) {
    shards_[shard_index]->refill_count.fetch_add(1, std::memory_order_relaxed);
  }
}

void ThreadLocalPerCoreMemoryCache::IncrementShardDrain(usize shard_index) {
  if (shard_index < shards_.size()) {
    shards_[shard_index]->drain_count.fetch_add(1, std::memory_order_relaxed);
  }
}

void ThreadLocalPerCoreMemoryCache::IncrementShardRemoteFree(usize shard_index) {
  if (shard_index < shards_.size()) {
    shards_[shard_index]->remote_free_handoff_count.fetch_add(1, std::memory_order_relaxed);
  }
}

void ThreadLocalPerCoreMemoryCache::IncrementShardRemoteEnqueue(usize shard_index) {
  if (shard_index < shards_.size()) {
    shards_[shard_index]->remote_free_enqueued_count.fetch_add(1, std::memory_order_relaxed);
  }
}

void ThreadLocalPerCoreMemoryCache::IncrementShardRemoteDequeue(usize shard_index) {
  if (shard_index < shards_.size()) {
    shards_[shard_index]->remote_free_dequeued_count.fetch_add(1, std::memory_order_relaxed);
  }
}

void ThreadLocalPerCoreMemoryCache::IncrementShardRemoteDrain(usize shard_index) {
  if (shard_index < shards_.size()) {
    shards_[shard_index]->remote_free_drained_count.fetch_add(1, std::memory_order_relaxed);
  }
}

void ThreadLocalPerCoreMemoryCache::IncrementShardRemoteReject(usize shard_index) {
  if (shard_index < shards_.size()) {
    shards_[shard_index]->remote_free_rejected_count.fetch_add(1, std::memory_order_relaxed);
  }
}

void ThreadLocalPerCoreMemoryCache::IncrementShardRemoteBackpressure(usize shard_index) {
  if (shard_index < shards_.size()) {
    shards_[shard_index]->remote_free_backpressure_count.fetch_add(1, std::memory_order_relaxed);
  }
}

void ThreadLocalPerCoreMemoryCache::RegisterCachedBytes(ThreadCacheState* state,
                                                        u64 bytes) {
  state->current_cached_bytes += bytes;
  state->peak_cached_bytes = std::max(state->peak_cached_bytes, state->current_cached_bytes);
  ++state->current_cached_objects;
  state->peak_cached_objects = std::max(state->peak_cached_objects, state->current_cached_objects);
  if (state->shard_index < shards_.size()) {
    const u64 current =
        shards_[state->shard_index]->current_cached_bytes.fetch_add(bytes, std::memory_order_relaxed) + bytes;
    UpdateAtomicPeak(&shards_[state->shard_index]->peak_cached_bytes, current);
  }
}

void ThreadLocalPerCoreMemoryCache::UnregisterCachedBytes(ThreadCacheState* state,
                                                          u64 bytes) {
  state->current_cached_bytes =
      state->current_cached_bytes >= bytes ? state->current_cached_bytes - bytes : 0;
  if (state->current_cached_objects != 0) {
    --state->current_cached_objects;
  }
  if (state->shard_index < shards_.size()) {
    SaturatingAtomicSubtract(&shards_[state->shard_index]->current_cached_bytes, bytes);
  }
}

bool ThreadLocalPerCoreMemoryCache::StoreCachedBlockLocked(
    ThreadCacheState* state,
    CachedBlock block) {
  if (block.pointer == nullptr || block.size_class_index >= state->cached_by_class.size()) {
    return false;
  }
  auto& cached = state->cached_by_class[block.size_class_index];
  if (cached.size() >= request_.max_cached_slots_per_class_per_thread) {
    return false;
  }
  if (request_.max_cached_bytes_per_thread != 0 &&
      (block.size_class_bytes > request_.max_cached_bytes_per_thread ||
       state->current_cached_bytes >
           request_.max_cached_bytes_per_thread - block.size_class_bytes)) {
    return false;
  }
  cached.push_back(block);
  RegisterCachedBytes(state, block.size_class_bytes);
  return true;
}

bool ThreadLocalPerCoreMemoryCache::RemoteQueueCanAcceptLocked(
    const ThreadCacheState& state,
    const CachedBlock& block) const {
  if (block.pointer == nullptr) {
    return true;
  }
  if (request_.remote_free_queue_max_count == 0 ||
      request_.remote_free_queue_max_bytes == 0) {
    return false;
  }
  if (state.remote_free_queue.size() >= request_.remote_free_queue_max_count) {
    return false;
  }
  if (block.size_class_bytes > request_.remote_free_queue_max_bytes) {
    return false;
  }
  return state.remote_free_queue_current_bytes <=
         request_.remote_free_queue_max_bytes - block.size_class_bytes;
}

void ThreadLocalPerCoreMemoryCache::RegisterRemoteQueueBytesLocked(
    ThreadCacheState* state,
    u64 bytes) {
  state->remote_free_queue_current_bytes += bytes;
  state->remote_free_queue_peak_bytes =
      std::max(state->remote_free_queue_peak_bytes,
               state->remote_free_queue_current_bytes);
  state->remote_free_queue_peak_count =
      std::max<u64>(state->remote_free_queue_peak_count,
                    static_cast<u64>(state->remote_free_queue.size()));
  if (state->shard_index < shards_.size()) {
    const u64 current_count =
        shards_[state->shard_index]->remote_free_queue_current_count.fetch_add(
            1, std::memory_order_relaxed) +
        1;
    const u64 current_bytes =
        shards_[state->shard_index]->remote_free_queue_current_bytes.fetch_add(
            bytes, std::memory_order_relaxed) +
        bytes;
    UpdateAtomicPeak(&shards_[state->shard_index]->remote_free_queue_peak_count,
                     current_count);
    UpdateAtomicPeak(&shards_[state->shard_index]->remote_free_queue_peak_bytes,
                     current_bytes);
  }
}

void ThreadLocalPerCoreMemoryCache::UnregisterRemoteQueueBytesLocked(
    ThreadCacheState* state,
    u64 bytes) {
  state->remote_free_queue_current_bytes =
      state->remote_free_queue_current_bytes >= bytes
          ? state->remote_free_queue_current_bytes - bytes
          : 0;
  if (state->shard_index < shards_.size()) {
    SaturatingAtomicSubtract(
        &shards_[state->shard_index]->remote_free_queue_current_count, 1);
    SaturatingAtomicSubtract(
        &shards_[state->shard_index]->remote_free_queue_current_bytes, bytes);
  }
}

void ThreadLocalPerCoreMemoryCache::RecordRemoteThreadHashLocked(
    ThreadCacheState* state,
    u64 remote_thread_hash) const {
  if (std::find(state->remote_thread_hashes.begin(),
                state->remote_thread_hashes.end(),
                remote_thread_hash) == state->remote_thread_hashes.end()) {
    state->remote_thread_hashes.push_back(remote_thread_hash);
  }
}

void ThreadLocalPerCoreMemoryCache::RecordRemoteEnqueueLatency(
    ThreadCacheState* state,
    u64 latency_ns) const {
  if (state->remote_free_enqueue_latencies_ns.size() < kMaxLatencySamplesPerThread) {
    state->remote_free_enqueue_latencies_ns.push_back(latency_ns);
    return;
  }
  const usize index =
      state->remote_free_enqueued_count % kMaxLatencySamplesPerThread;
  state->remote_free_enqueue_latencies_ns[index] = latency_ns;
}

void ThreadLocalPerCoreMemoryCache::RecordRemoteReconcileLatency(
    ThreadCacheState* state,
    u64 latency_ns) const {
  if (state->remote_free_reconcile_latencies_ns.size() < kMaxLatencySamplesPerThread) {
    state->remote_free_reconcile_latencies_ns.push_back(latency_ns);
    return;
  }
  const usize index =
      state->remote_free_dequeued_count % kMaxLatencySamplesPerThread;
  state->remote_free_reconcile_latencies_ns[index] = latency_ns;
}

void ThreadLocalPerCoreMemoryCache::RecordGlobalRemoteEnqueueLatency(
    u64 latency_ns) {
  std::lock_guard<std::mutex> lock(remote_latency_mutex_);
  if (remote_free_enqueue_latencies_ns_.size() < kMaxRemoteLatencySamples) {
    remote_free_enqueue_latencies_ns_.push_back(latency_ns);
    return;
  }
  remote_free_enqueue_latencies_ns_[latency_ns % kMaxRemoteLatencySamples] =
      latency_ns;
}

void ThreadLocalPerCoreMemoryCache::RecordGlobalRemoteReconcileLatency(
    u64 latency_ns) {
  std::lock_guard<std::mutex> lock(remote_latency_mutex_);
  if (remote_free_reconcile_latencies_ns_.size() < kMaxRemoteLatencySamples) {
    remote_free_reconcile_latencies_ns_.push_back(latency_ns);
    return;
  }
  remote_free_reconcile_latencies_ns_[latency_ns % kMaxRemoteLatencySamples] =
      latency_ns;
}

void ThreadLocalPerCoreMemoryCache::RecordLatency(ThreadCacheState* state,
                                                  u64 latency_ns) const {
  if (state->allocation_latencies_ns.size() < kMaxLatencySamplesPerThread) {
    state->allocation_latencies_ns.push_back(latency_ns);
    return;
  }
  const usize index = state->allocation_count % kMaxLatencySamplesPerThread;
  state->allocation_latencies_ns[index] = latency_ns;
}

ThreadLocalCacheFreeResult ThreadLocalPerCoreMemoryCache::ReconcileRemoteQueue(
    const std::shared_ptr<ThreadCacheState>& state,
    const char* reason,
    bool cache_for_reuse,
    usize max_records) {
  ThreadLocalCacheFreeResult result;
  result.status = OkStatus();
  AppendBaseEvidence(&result.evidence);
  if (state == nullptr) {
    result.freed = true;
    return result;
  }

  std::vector<RemoteFreeRecord> records;
  if (max_records != 0) {
    records.reserve(max_records);
  }
  {
    std::lock_guard<std::mutex> lock(state->mutex);
    while (!state->remote_free_queue.empty() &&
           (max_records == 0 || records.size() < max_records)) {
      RemoteFreeRecord record = state->remote_free_queue.front();
      state->remote_free_queue.pop_front();
      UnregisterRemoteQueueBytesLocked(state.get(), record.block.size_class_bytes);
      ++state->remote_free_dequeued_count;
      ++state->remote_free_drained_count;
      ++state->reconciliation_count;
      IncrementShardRemoteDequeue(state->shard_index);
      IncrementShardRemoteDrain(state->shard_index);
      const u64 latency_ns =
          record.enqueue_time_ns == 0 ? 0 : NowNs() - record.enqueue_time_ns;
      RecordRemoteReconcileLatency(state.get(), latency_ns);
      RecordGlobalRemoteReconcileLatency(latency_ns);
      result.remote_free_reconcile_latency_ns =
          std::max(result.remote_free_reconcile_latency_ns, latency_ns);
      records.push_back(record);
    }
  }

  for (const auto& record : records) {
    bool cached = false;
    if (cache_for_reuse && !released_.load(std::memory_order_acquire)) {
      std::lock_guard<std::mutex> lock(state->mutex);
      if (!state->thread_exiting && StoreCachedBlockLocked(state.get(), record.block)) {
        cached = true;
        result.cached_for_reuse = true;
      }
    }
    if (!cached) {
      (void)FreeBlockToBackingPool(record.block, &result);
    }
  }

  result.freed = result.status.ok() && !result.fail_closed;
  result.remote_free_dequeued = !records.empty();
  result.remote_free_dequeued_count = static_cast<u64>(records.size());
  result.remote_free_drained_count = static_cast<u64>(records.size());
  {
    std::lock_guard<std::mutex> lock(state->mutex);
    result.remote_free_queue_current_count =
        static_cast<u64>(state->remote_free_queue.size());
    result.remote_free_queue_current_bytes =
        state->remote_free_queue_current_bytes;
  }
  result.evidence.push_back("thread_local_memory_cache.remote_free_queue.reconcile.reason=" +
                            std::string(reason));
  result.evidence.push_back("thread_local_memory_cache.remote_free_queue.dequeued_count=" +
                            std::to_string(result.remote_free_dequeued_count));
  result.evidence.push_back("thread_local_memory_cache.remote_free_queue.drained_count=" +
                            std::to_string(result.remote_free_drained_count));
  result.evidence.push_back("thread_local_memory_cache.remote_free_queue.current_count=" +
                            std::to_string(result.remote_free_queue_current_count));
  result.evidence.push_back("thread_local_memory_cache.remote_free_queue.current_bytes=" +
                            std::to_string(result.remote_free_queue_current_bytes));
  result.evidence.push_back("thread_local_memory_cache.remote_free_queue.cache_for_reuse=" +
                            BoolText(cache_for_reuse));
  return result;
}

ThreadLocalCacheFreeResult ThreadLocalPerCoreMemoryCache::ThreadExitCleanupRaw(
    ThreadCacheState* state) {
  ThreadLocalCacheFreeResult result;
  result.status = OkStatus();
  AppendBaseEvidence(&result.evidence);
  if (state == nullptr) {
    result.freed = true;
    return result;
  }

  std::vector<CachedBlock> blocks;
  u64 remote_record_count = 0;
  u64 remote_record_bytes = 0;
  {
    std::lock_guard<std::mutex> lock(state->mutex);
    state->thread_exiting = true;
    for (auto& cached : state->cached_by_class) {
      for (const auto& block : cached) {
        blocks.push_back(block);
        UnregisterCachedBytes(state, block.size_class_bytes);
      }
      cached.clear();
    }
    while (!state->remote_free_queue.empty()) {
      RemoteFreeRecord record = state->remote_free_queue.front();
      state->remote_free_queue.pop_front();
      UnregisterRemoteQueueBytesLocked(state, record.block.size_class_bytes);
      ++state->remote_free_dequeued_count;
      ++state->remote_free_drained_count;
      ++state->reconciliation_count;
      IncrementShardRemoteDequeue(state->shard_index);
      IncrementShardRemoteDrain(state->shard_index);
      const u64 latency_ns =
          record.enqueue_time_ns == 0 ? 0 : NowNs() - record.enqueue_time_ns;
      RecordRemoteReconcileLatency(state, latency_ns);
      RecordGlobalRemoteReconcileLatency(latency_ns);
      result.remote_free_reconcile_latency_ns =
          std::max(result.remote_free_reconcile_latency_ns, latency_ns);
      ++remote_record_count;
      remote_record_bytes += record.block.size_class_bytes;
      blocks.push_back(record.block);
    }
    ++state->drain_count;
    ++state->reconciliation_count;
    IncrementShardDrain(state->shard_index);
  }

  u64 reconciled_bytes = 0;
  for (const auto& block : blocks) {
    reconciled_bytes += block.size_class_bytes;
    (void)FreeBlockToBackingPool(block, &result);
  }
  thread_exit_cleanup_count_.fetch_add(1, std::memory_order_relaxed);
  thread_exit_reconciled_count_.fetch_add(static_cast<u64>(blocks.size()),
                                          std::memory_order_relaxed);
  thread_exit_reconciled_bytes_.fetch_add(reconciled_bytes,
                                          std::memory_order_relaxed);
  result.freed = result.status.ok() && !result.fail_closed;
  result.drained_to_backing_pool = !blocks.empty() || result.drained_to_backing_pool;
  result.remote_free_dequeued = remote_record_count != 0;
  result.remote_free_dequeued_count = remote_record_count;
  result.remote_free_drained_count = remote_record_count;
  result.evidence.push_back("thread_local_memory_cache.thread_exit_cleanup=true");
  result.evidence.push_back("thread_local_memory_cache.thread_exit_reconciled_count=" +
                            std::to_string(blocks.size()));
  result.evidence.push_back("thread_local_memory_cache.thread_exit_reconciled_bytes=" +
                            std::to_string(reconciled_bytes));
  result.evidence.push_back("thread_local_memory_cache.thread_exit_remote_records=" +
                            std::to_string(remote_record_count));
  result.evidence.push_back("thread_local_memory_cache.thread_exit_remote_bytes=" +
                            std::to_string(remote_record_bytes));
  result.evidence.push_back("thread_local_memory_cache.thread_exit_active_owner_records_stale=true");
  return result;
}

ThreadLocalCacheAllocationResult ThreadLocalPerCoreMemoryCache::Allocate(
    ThreadLocalCacheAllocationRequest request) {
  const auto start = std::chrono::steady_clock::now();
  if (released_.load(std::memory_order_acquire)) {
    return RefuseAllocation(std::move(request),
                            "SB_CEIC_014_THREAD_CACHE.RELEASED",
                            "memory.ceic_014.thread_cache.released",
                            "cache_released",
                            StatusCode::memory_invalid_request);
  }
  if (request.bytes == 0) {
    return RefuseAllocation(std::move(request),
                            "SB_CEIC_014_THREAD_CACHE.ZERO_SIZE",
                            "memory.ceic_014.thread_cache.zero_size",
                            "requested_bytes_required",
                            StatusCode::memory_invalid_request);
  }
  if (request.alignment == 0) {
    request.alignment = alignof(std::max_align_t);
  }
  if (!IsPowerOfTwo(request.alignment) ||
      request.alignment > alignof(std::max_align_t)) {
    return RefuseAllocation(std::move(request),
                            "SB_CEIC_014_THREAD_CACHE.ALIGNMENT_UNSUPPORTED",
                            "memory.ceic_014.thread_cache.alignment_unsupported",
                            "alignment_exceeds_cached_size_class_alignment",
                            StatusCode::memory_invalid_request);
  }

  const usize class_index = FindSizeClass(request.bytes);
  if (class_index == size_classes_.size()) {
    return RefuseAllocation(std::move(request),
                            "SB_CEIC_014_THREAD_CACHE.SIZE_CLASS_UNAVAILABLE",
                            "memory.ceic_014.thread_cache.size_class_unavailable",
                            "no_configured_size_class_can_hold_request",
                            StatusCode::memory_limit_exceeded);
  }

  auto state = CurrentThreadState();
  auto remote_reconciled =
      ReconcileRemoteQueue(state, "owner_allocation_reconcile", true);
  if (!remote_reconciled.ok()) {
    ThreadLocalCacheAllocationResult result;
    result.status = remote_reconciled.status;
    result.fail_closed = true;
    result.diagnostic = remote_reconciled.diagnostic;
    result.evidence = remote_reconciled.evidence;
    result.evidence.push_back("thread_local_memory_cache.allocation.fail_closed=true");
    result.evidence.push_back(
        "thread_local_memory_cache.remote_free_queue.reconcile_failed=true");
    return result;
  }

  CachedBlock cached_block;
  ThreadLocalCacheAllocationResult cached_result;
  bool served_from_cache = false;
  {
    std::lock_guard<std::mutex> lock(state->mutex);
    auto& cached = state->cached_by_class[class_index];
    if (!cached.empty()) {
      cached_block = cached.back();
      cached.pop_back();
      UnregisterCachedBytes(state.get(), cached_block.size_class_bytes);
      cached_block.requested_bytes = request.bytes;
      state->active[cached_block.pointer] =
          ActiveBlock{cached_block.pointer,
                      request.bytes,
                      cached_block.size_class_bytes,
                      class_index};
      ++state->hit_count;
      ++state->allocation_count;
      state->current_active_bytes += cached_block.size_class_bytes;
      ++state->current_active_objects;
      IncrementShardHit(state->shard_index);
      const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now() - start).count();
      RecordLatency(state.get(), static_cast<u64>(std::max<long long>(0, elapsed)));
      std::memset(cached_block.pointer, kCacheAllocatedPoison, cached_block.size_class_bytes);

      cached_result.status = OkStatus();
      cached_result.pointer = cached_block.pointer;
      cached_result.requested_bytes = request.bytes;
      cached_result.size_class_bytes = cached_block.size_class_bytes;
      cached_result.size_class_index = class_index;
      cached_result.cache_hit = true;
      cached_result.latency_ns = static_cast<u64>(std::max<long long>(0, elapsed));
      AppendBaseEvidence(&cached_result.evidence);
      cached_result.evidence.push_back("thread_local_memory_cache.allocation.ok=true");
      cached_result.evidence.push_back("thread_local_memory_cache.cache_hit=true");
      cached_result.evidence.push_back("thread_local_memory_cache.refill_performed=false");
      cached_result.evidence.push_back("thread_local_memory_cache.shard_index=" +
                                       std::to_string(state->shard_index));
      cached_result.evidence.push_back("thread_local_memory_cache.core_id=" +
                                       std::to_string(state->core_id));
      cached_result.evidence.push_back("thread_local_memory_cache.numa_node=" +
                                       std::to_string(state->numa_node));
      cached_result.evidence.push_back("thread_local_memory_cache.thread_id_hash=" +
                                       std::to_string(state->thread_id_hash));
      served_from_cache = true;
    }
    if (!served_from_cache) {
      ++state->miss_count;
      IncrementShardMiss(state->shard_index);
    }
  }

  if (served_from_cache) {
    ActiveOwnerRecord owner_record;
    owner_record.block = cached_block;
    owner_record.owner_state = state;
    owner_record.owner_thread_id = state->thread_id;
    owner_record.owner_thread_hash = state->thread_id_hash;
    owner_record.owner_shard_index = state->shard_index;
    owner_record.owner_core_id = state->core_id;
    owner_record.owner_numa_node = state->numa_node;
    std::lock_guard<std::mutex> owner_lock(ownership_mutex_);
    ownership_[cached_block.pointer] = owner_record;
    return cached_result;
  }

  CachedBlock returned;
  std::vector<CachedBlock> refill_blocks;
  const usize batch_count = std::max<usize>(1, request_.refill_batch_count);
  for (usize index = 0; index < batch_count; ++index) {
    SizeClassAllocationRequest backing_request;
    backing_request.bytes = size_classes_[class_index].payload_bytes;
    backing_request.alignment = request.alignment;
    backing_request.purpose = request.purpose.empty()
                                  ? request_.purpose + ".cache_refill"
                                  : request.purpose + ".cache_refill";
    SizeClassAllocationResult backing;
    {
      std::lock_guard<std::mutex> lock(allocator_mutex_);
      if (allocator_ == nullptr || released_.load(std::memory_order_acquire)) {
        return RefuseAllocation(std::move(request),
                                "SB_CEIC_014_THREAD_CACHE.BACKING_RELEASED",
                                "memory.ceic_014.thread_cache.backing_released",
                                "backing_pool_released",
                                StatusCode::memory_invalid_request);
      }
      backing = allocator_->Allocate(std::move(backing_request));
    }
    if (!backing.ok()) {
      if (index == 0) {
        ThreadLocalCacheAllocationResult result;
        result.status = backing.status;
        result.fail_closed = true;
        result.diagnostic = backing.diagnostic;
        result.evidence = backing.evidence;
        AppendBaseEvidence(&result.evidence);
        result.evidence.push_back("thread_local_memory_cache.cache_hit=false");
        result.evidence.push_back("thread_local_memory_cache.refill_performed=false");
        return result;
      }
      break;
    }
    CachedBlock block;
    block.pointer = backing.pointer;
    block.requested_bytes = request.bytes;
    block.size_class_bytes = backing.size_class_bytes;
    block.size_class_index = class_index;
    if (index == 0) {
      returned = block;
    } else {
      std::memset(block.pointer, kCacheFreePoison, block.size_class_bytes);
      refill_blocks.push_back(block);
    }
  }

  std::vector<CachedBlock> overflow_blocks;
  {
    std::lock_guard<std::mutex> lock(state->mutex);
    state->active[returned.pointer] =
        ActiveBlock{returned.pointer, request.bytes, returned.size_class_bytes, class_index};
    ++state->allocation_count;
    ++state->refill_count;
    state->current_active_bytes += returned.size_class_bytes;
    ++state->current_active_objects;
    IncrementShardRefill(state->shard_index);
    for (auto& block : refill_blocks) {
      if (!StoreCachedBlockLocked(state.get(), block)) {
        overflow_blocks.push_back(block);
      }
    }
    const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now() - start).count();
    RecordLatency(state.get(), static_cast<u64>(std::max<long long>(0, elapsed)));
  }

  {
    ActiveOwnerRecord owner_record;
    owner_record.block = returned;
    owner_record.owner_state = state;
    owner_record.owner_thread_id = state->thread_id;
    owner_record.owner_thread_hash = state->thread_id_hash;
    owner_record.owner_shard_index = state->shard_index;
    owner_record.owner_core_id = state->core_id;
    owner_record.owner_numa_node = state->numa_node;
    std::lock_guard<std::mutex> owner_lock(ownership_mutex_);
    ownership_[returned.pointer] = owner_record;
  }

  ThreadLocalCacheFreeResult overflow_result;
  overflow_result.status = OkStatus();
  for (const auto& block : overflow_blocks) {
    (void)FreeBlockToBackingPool(block, &overflow_result);
  }

  const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::steady_clock::now() - start).count();
  ThreadLocalCacheAllocationResult result;
  result.status = OkStatus();
  result.pointer = returned.pointer;
  result.requested_bytes = request.bytes;
  result.size_class_bytes = returned.size_class_bytes;
  result.size_class_index = class_index;
  result.cache_hit = false;
  result.refill_performed = true;
  result.latency_ns = static_cast<u64>(std::max<long long>(0, elapsed));
  AppendBaseEvidence(&result.evidence);
  result.evidence.push_back("thread_local_memory_cache.allocation.ok=true");
  result.evidence.push_back("thread_local_memory_cache.cache_hit=false");
  result.evidence.push_back("thread_local_memory_cache.refill_performed=true");
  result.evidence.push_back("thread_local_memory_cache.refill_batch_count=" +
                            std::to_string(batch_count));
  result.evidence.push_back("thread_local_memory_cache.refill_cached_blocks=" +
                            std::to_string(refill_blocks.size() - overflow_blocks.size()));
  result.evidence.push_back("thread_local_memory_cache.shard_index=" +
                            std::to_string(state->shard_index));
  result.evidence.push_back("thread_local_memory_cache.core_id=" +
                            std::to_string(state->core_id));
  result.evidence.push_back("thread_local_memory_cache.numa_node=" +
                            std::to_string(state->numa_node));
  result.evidence.push_back("thread_local_memory_cache.thread_id_hash=" +
                            std::to_string(state->thread_id_hash));
  return result;
}

bool ThreadLocalPerCoreMemoryCache::FreeBlockToBackingPool(
    CachedBlock block,
    ThreadLocalCacheFreeResult* result) {
  if (block.pointer == nullptr) {
    return true;
  }
  SizeClassFreeResult backing;
  {
    std::lock_guard<std::mutex> lock(allocator_mutex_);
    if (allocator_ == nullptr) {
      result->status = ErrorStatus(StatusCode::memory_invalid_request);
      result->fail_closed = true;
      result->diagnostic = MakeCacheDiagnostic(
          result->status,
          "SB_CEIC_014_THREAD_CACHE.BACKING_POOL_MISSING",
          "memory.ceic_014.thread_cache.backing_pool_missing",
          request_,
          {{"reason", "backing_pool_missing"}});
      return false;
    }
    backing = allocator_->Free(block.pointer);
  }
  if (!backing.ok()) {
    result->status = backing.status;
    result->fail_closed = true;
    result->diagnostic = backing.diagnostic;
    result->evidence.insert(result->evidence.end(),
                            backing.evidence.begin(),
                            backing.evidence.end());
    return false;
  }
  result->drained_to_backing_pool = true;
  result->evidence.insert(result->evidence.end(),
                          backing.evidence.begin(),
                          backing.evidence.end());
  return true;
}

ThreadLocalCacheFreeResult ThreadLocalPerCoreMemoryCache::Free(void* pointer) {
  const auto start = std::chrono::steady_clock::now();
  ThreadLocalCacheFreeResult result;
  result.status = OkStatus();
  AppendBaseEvidence(&result.evidence);
  if (pointer == nullptr) {
    result.freed = true;
    result.evidence.push_back("thread_local_memory_cache.free.null_pointer=true");
    return result;
  }
  if (released_.load(std::memory_order_acquire)) {
    return RefuseFree("SB_CEIC_014_THREAD_CACHE.FREE_AFTER_RELEASE",
                      "memory.ceic_014.thread_cache.free_after_release",
                      "cache_released",
                      StatusCode::memory_unknown_pointer);
  }

  auto current = CurrentThreadState();
  auto owner_reconciled =
      ReconcileRemoteQueue(current, "owner_free_reconcile", true);
  if (!owner_reconciled.ok()) {
    return owner_reconciled;
  }
  result.remote_free_dequeued = owner_reconciled.remote_free_dequeued;
  result.remote_free_dequeued_count = owner_reconciled.remote_free_dequeued_count;
  result.remote_free_drained_count = owner_reconciled.remote_free_drained_count;
  result.remote_free_reconcile_latency_ns =
      owner_reconciled.remote_free_reconcile_latency_ns;

  ActiveOwnerRecord owner_record;
  {
    std::lock_guard<std::mutex> owner_lock(ownership_mutex_);
    auto it = ownership_.find(pointer);
    if (it == ownership_.end()) {
      return RefuseFree("SB_CEIC_014_THREAD_CACHE.UNKNOWN_POINTER",
                        "memory.ceic_014.thread_cache.unknown_pointer",
                        "pointer_not_owned_by_cache",
                        StatusCode::memory_unknown_pointer);
    }
    owner_record = it->second;
    ownership_.erase(it);
  }

  auto direct_reconcile = [&](CachedBlock block,
                              bool stale_owner,
                              const char* reason) {
    if (block.pointer != nullptr) {
      std::memset(block.pointer, kCacheFreePoison, block.size_class_bytes);
    }
    (void)FreeBlockToBackingPool(block, &result);
    result.direct_reconciled_to_backing_pool = true;
    result.stale_owner = result.stale_owner || stale_owner;
    result.drained_to_backing_pool = true;
    remote_free_direct_reconcile_count_.fetch_add(1, std::memory_order_relaxed);
    if (stale_owner) {
      remote_free_stale_owner_count_.fetch_add(1, std::memory_order_relaxed);
    }
    result.evidence.push_back(
        "thread_local_memory_cache.remote_free.direct_reconcile=true");
    result.evidence.push_back("thread_local_memory_cache.remote_free.direct_reason=" +
                              std::string(reason));
    result.evidence.push_back("thread_local_memory_cache.remote_free.stale_owner=" +
                              BoolText(stale_owner));
  };

  CachedBlock block = owner_record.block;
  const bool same_thread = owner_record.owner_thread_id == current->thread_id;
  if (same_thread) {
    bool removed = false;
    CachedBlock drain_block;
    {
      std::lock_guard<std::mutex> lock(current->mutex);
      auto it = current->active.find(pointer);
      if (it != current->active.end()) {
        const ActiveBlock active = it->second;
        current->active.erase(it);
        removed = true;
        block = CachedBlock{active.pointer,
                            active.requested_bytes,
                            active.size_class_bytes,
                            active.size_class_index};
        if (current->current_active_bytes >= active.size_class_bytes) {
          current->current_active_bytes -= active.size_class_bytes;
        } else {
          current->current_active_bytes = 0;
        }
        if (current->current_active_objects != 0) {
          --current->current_active_objects;
        }
        ++current->free_count;
        std::memset(active.pointer, kCacheFreePoison, active.size_class_bytes);
        if (StoreCachedBlockLocked(current.get(), block)) {
          result.cached_for_reuse = true;
        } else {
          drain_block = block;
        }
      }
    }
    if (!removed) {
      direct_reconcile(block, true, "same_thread_owner_record_without_active_state");
    } else if (drain_block.pointer != nullptr) {
      (void)FreeBlockToBackingPool(drain_block, &result);
    }
    result.freed = result.status.ok() && !result.fail_closed;
    result.evidence.push_back("thread_local_memory_cache.free.ok=true");
    result.evidence.push_back("thread_local_memory_cache.free.same_thread=true");
    result.evidence.push_back("thread_local_memory_cache.free.cached_for_reuse=" +
                              BoolText(result.cached_for_reuse));
    return result;
  }

  auto owner_state = owner_record.owner_state.lock();
  if (owner_state == nullptr || owner_state->owner != this) {
    direct_reconcile(block, true, "owner_state_stale_or_released");
    result.freed = result.status.ok() && !result.fail_closed;
    result.remote_free_handoff = true;
    result.evidence.push_back("thread_local_memory_cache.free.ok=true");
    result.evidence.push_back("thread_local_memory_cache.free.remote_handoff=true");
    result.evidence.push_back("thread_local_memory_cache.remote_free.stale_owner=true");
    return result;
  }

  std::vector<RemoteFreeRecord> pressure_drained_records;
  bool owner_active_removed = false;
  bool enqueued = false;
  bool rejected = false;
  bool backpressure = false;
  {
    std::lock_guard<std::mutex> lock(owner_state->mutex);
    auto it = owner_state->active.find(pointer);
    if (it != owner_state->active.end() && !owner_state->thread_exiting) {
      const ActiveBlock active = it->second;
      owner_state->active.erase(it);
      owner_active_removed = true;
      block = CachedBlock{active.pointer,
                          active.requested_bytes,
                          active.size_class_bytes,
                          active.size_class_index};
      if (owner_state->current_active_bytes >= active.size_class_bytes) {
        owner_state->current_active_bytes -= active.size_class_bytes;
      } else {
        owner_state->current_active_bytes = 0;
      }
      if (owner_state->current_active_objects != 0) {
        --owner_state->current_active_objects;
      }
      ++owner_state->remote_free_handoff_count;
      ++owner_state->free_count;
      IncrementShardRemoteFree(owner_state->shard_index);
      RecordRemoteThreadHashLocked(owner_state.get(), current->thread_id_hash);
      std::memset(active.pointer, kCacheFreePoison, active.size_class_bytes);

      if (!RemoteQueueCanAcceptLocked(*owner_state, block)) {
        backpressure = true;
        ++owner_state->remote_free_backpressure_count;
        IncrementShardRemoteBackpressure(owner_state->shard_index);
        const usize drain_limit =
            std::max<usize>(1, request_.remote_free_pressure_drain_count);
        while (!owner_state->remote_free_queue.empty() &&
               pressure_drained_records.size() < drain_limit) {
          RemoteFreeRecord record = owner_state->remote_free_queue.front();
          owner_state->remote_free_queue.pop_front();
          UnregisterRemoteQueueBytesLocked(owner_state.get(),
                                           record.block.size_class_bytes);
          ++owner_state->remote_free_dequeued_count;
          ++owner_state->remote_free_drained_count;
          ++owner_state->reconciliation_count;
          IncrementShardRemoteDequeue(owner_state->shard_index);
          IncrementShardRemoteDrain(owner_state->shard_index);
          const u64 latency_ns =
              record.enqueue_time_ns == 0 ? 0 : NowNs() - record.enqueue_time_ns;
          RecordRemoteReconcileLatency(owner_state.get(), latency_ns);
          RecordGlobalRemoteReconcileLatency(latency_ns);
          result.remote_free_reconcile_latency_ns =
              std::max(result.remote_free_reconcile_latency_ns, latency_ns);
          pressure_drained_records.push_back(record);
        }
      }

      if (RemoteQueueCanAcceptLocked(*owner_state, block)) {
        RemoteFreeRecord record;
        record.block = block;
        record.owner_thread_hash = owner_record.owner_thread_hash;
        record.owner_shard_index = owner_record.owner_shard_index;
        record.owner_core_id = owner_record.owner_core_id;
        record.owner_numa_node = owner_record.owner_numa_node;
        record.remote_thread_hash = current->thread_id_hash;
        record.enqueue_time_ns = NowNs();
        owner_state->remote_free_queue.push_back(record);
        RegisterRemoteQueueBytesLocked(owner_state.get(), block.size_class_bytes);
        ++owner_state->remote_free_enqueued_count;
        IncrementShardRemoteEnqueue(owner_state->shard_index);
        const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - start).count();
        const u64 enqueue_latency =
            static_cast<u64>(std::max<long long>(0, elapsed));
        RecordRemoteEnqueueLatency(owner_state.get(), enqueue_latency);
        RecordGlobalRemoteEnqueueLatency(enqueue_latency);
        result.remote_free_enqueue_latency_ns = enqueue_latency;
        result.remote_free_queue_current_count =
            static_cast<u64>(owner_state->remote_free_queue.size());
        result.remote_free_queue_current_bytes =
            owner_state->remote_free_queue_current_bytes;
        enqueued = true;
      } else {
        rejected = true;
        ++owner_state->remote_free_rejected_count;
        IncrementShardRemoteReject(owner_state->shard_index);
        result.remote_free_queue_current_count =
            static_cast<u64>(owner_state->remote_free_queue.size());
        result.remote_free_queue_current_bytes =
            owner_state->remote_free_queue_current_bytes;
      }
    }
  }

  for (const auto& record : pressure_drained_records) {
    (void)FreeBlockToBackingPool(record.block, &result);
  }
  if (!pressure_drained_records.empty()) {
    result.remote_free_dequeued = true;
    result.remote_free_dequeued_count +=
        static_cast<u64>(pressure_drained_records.size());
    result.remote_free_drained_count +=
        static_cast<u64>(pressure_drained_records.size());
    result.drained_to_backing_pool = true;
  }

  if (!owner_active_removed) {
    direct_reconcile(block, true, "owner_active_state_missing_or_exiting");
  } else if (enqueued) {
    result.remote_free_enqueued = true;
    result.remote_free_enqueued_count = 1;
  } else {
    direct_reconcile(block, false, "remote_free_queue_refused_after_backpressure");
  }

  result.remote_free_handoff = true;
  result.remote_free_rejected = rejected;
  result.remote_free_backpressure = backpressure;
  result.remote_free_rejected_count = rejected ? 1 : 0;
  result.freed = result.status.ok() && !result.fail_closed;
  result.evidence.push_back("thread_local_memory_cache.free.ok=true");
  result.evidence.push_back("thread_local_memory_cache.free.remote_handoff=true");
  result.evidence.push_back("thread_local_memory_cache.remote_free.enqueued=" +
                            BoolText(result.remote_free_enqueued));
  result.evidence.push_back("thread_local_memory_cache.remote_free.rejected=" +
                            BoolText(result.remote_free_rejected));
  result.evidence.push_back("thread_local_memory_cache.remote_free.backpressure=" +
                            BoolText(result.remote_free_backpressure));
  result.evidence.push_back("thread_local_memory_cache.remote_free.direct_reconcile=" +
                            BoolText(result.direct_reconciled_to_backing_pool));
  result.evidence.push_back("thread_local_memory_cache.remote_free.owner_thread_hash=" +
                            std::to_string(owner_record.owner_thread_hash));
  result.evidence.push_back("thread_local_memory_cache.remote_free.owner_shard_index=" +
                            std::to_string(owner_record.owner_shard_index));
  result.evidence.push_back("thread_local_memory_cache.remote_free.owner_core_id=" +
                            std::to_string(owner_record.owner_core_id));
  result.evidence.push_back("thread_local_memory_cache.remote_free.owner_numa_node=" +
                            std::to_string(owner_record.owner_numa_node));
  result.evidence.push_back("thread_local_memory_cache.remote_free.remote_thread_hash=" +
                            std::to_string(current->thread_id_hash));
  result.evidence.push_back("thread_local_memory_cache.remote_free.queue_current_count=" +
                            std::to_string(result.remote_free_queue_current_count));
  result.evidence.push_back("thread_local_memory_cache.remote_free.queue_current_bytes=" +
                            std::to_string(result.remote_free_queue_current_bytes));
  result.evidence.push_back("thread_local_memory_cache.remote_free.enqueue_latency_ns=" +
                            std::to_string(result.remote_free_enqueue_latency_ns));
  result.evidence.push_back("thread_local_memory_cache.remote_free.reconcile_latency_ns=" +
                            std::to_string(result.remote_free_reconcile_latency_ns));
  return result;
}

ThreadLocalCacheFreeResult ThreadLocalPerCoreMemoryCache::DrainStateRaw(
    ThreadCacheState* state,
    bool include_active,
    const char* reason) {
  ThreadLocalCacheFreeResult result;
  result.status = OkStatus();
  AppendBaseEvidence(&result.evidence);
  if (state == nullptr) {
    result.freed = true;
    return result;
  }

  std::vector<CachedBlock> blocks;
  std::vector<void*> active_owned_pointers;
  u64 remote_record_count = 0;
  u64 remote_record_bytes = 0;
  {
    std::lock_guard<std::mutex> lock(state->mutex);
    for (auto& cached : state->cached_by_class) {
      for (const auto& block : cached) {
        blocks.push_back(block);
        UnregisterCachedBytes(state, block.size_class_bytes);
      }
      cached.clear();
    }
    while (!state->remote_free_queue.empty()) {
      RemoteFreeRecord record = state->remote_free_queue.front();
      state->remote_free_queue.pop_front();
      UnregisterRemoteQueueBytesLocked(state, record.block.size_class_bytes);
      ++state->remote_free_dequeued_count;
      ++state->remote_free_drained_count;
      ++state->reconciliation_count;
      IncrementShardRemoteDequeue(state->shard_index);
      IncrementShardRemoteDrain(state->shard_index);
      const u64 latency_ns =
          record.enqueue_time_ns == 0 ? 0 : NowNs() - record.enqueue_time_ns;
      RecordRemoteReconcileLatency(state, latency_ns);
      RecordGlobalRemoteReconcileLatency(latency_ns);
      result.remote_free_reconcile_latency_ns =
          std::max(result.remote_free_reconcile_latency_ns, latency_ns);
      ++remote_record_count;
      remote_record_bytes += record.block.size_class_bytes;
      blocks.push_back(record.block);
    }
    if (include_active) {
      for (const auto& entry : state->active) {
        const ActiveBlock& active = entry.second;
        active_owned_pointers.push_back(entry.first);
        blocks.push_back(CachedBlock{active.pointer,
                                     active.requested_bytes,
                                     active.size_class_bytes,
                                     active.size_class_index});
      }
      state->active.clear();
      state->current_active_bytes = 0;
      state->current_active_objects = 0;
    }
    ++state->drain_count;
    ++state->reconciliation_count;
    IncrementShardDrain(state->shard_index);
  }

  if (!active_owned_pointers.empty()) {
    std::lock_guard<std::mutex> owner_lock(ownership_mutex_);
    for (void* owned : active_owned_pointers) {
      ownership_.erase(owned);
    }
  }

  for (const auto& block : blocks) {
    (void)FreeBlockToBackingPool(block, &result);
  }
  result.freed = result.status.ok() && !result.fail_closed;
  result.remote_free_dequeued = remote_record_count != 0;
  result.remote_free_dequeued_count = remote_record_count;
  result.remote_free_drained_count = remote_record_count;
  result.evidence.push_back("thread_local_memory_cache.drain.reason=" + std::string(reason));
  result.evidence.push_back("thread_local_memory_cache.drain.block_count=" +
                            std::to_string(blocks.size()));
  result.evidence.push_back("thread_local_memory_cache.drain.include_active=" +
                            BoolText(include_active));
  result.evidence.push_back("thread_local_memory_cache.drain.remote_record_count=" +
                            std::to_string(remote_record_count));
  result.evidence.push_back("thread_local_memory_cache.drain.remote_record_bytes=" +
                            std::to_string(remote_record_bytes));
  return result;
}

ThreadLocalCacheFreeResult ThreadLocalPerCoreMemoryCache::DrainState(
    const std::shared_ptr<ThreadCacheState>& state,
    bool include_active,
    const char* reason) {
  return DrainStateRaw(state.get(), include_active, reason);
}

ThreadLocalCacheFreeResult ThreadLocalPerCoreMemoryCache::DrainCurrentThread() {
  auto state = CurrentThreadState();
  return DrainState(state, false, "manual_current_thread_drain");
}

std::vector<std::shared_ptr<ThreadLocalPerCoreMemoryCache::ThreadCacheState>>
ThreadLocalPerCoreMemoryCache::LiveThreadStates() const {
  std::vector<std::shared_ptr<ThreadCacheState>> states;
  std::lock_guard<std::mutex> lock(registry_mutex_);
  states.reserve(registry_.size());
  for (const auto& weak : registry_) {
    auto state = weak.lock();
    if (state != nullptr) {
      states.push_back(std::move(state));
    }
  }
  return states;
}

ThreadLocalCacheFreeResult ThreadLocalPerCoreMemoryCache::ReconcileAndRelease() {
  ThreadLocalCacheFreeResult result;
  result.status = OkStatus();
  AppendBaseEvidence(&result.evidence);
  bool expected = false;
  if (!released_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
    result.freed = true;
    result.evidence.push_back("thread_local_memory_cache.release.already_released=true");
    return result;
  }

  const auto states = LiveThreadStates();
  for (const auto& state : states) {
    auto drained = DrainState(state, true, "cache_release_reconcile");
    if (!drained.ok() && result.ok()) {
      result.status = drained.status;
      result.fail_closed = drained.fail_closed;
      result.diagnostic = drained.diagnostic;
    }
    result.evidence.insert(result.evidence.end(),
                           drained.evidence.begin(),
                           drained.evidence.end());
    std::lock_guard<std::mutex> lock(state->mutex);
    state->owner = nullptr;
  }

  std::vector<CachedBlock> stale_owner_blocks;
  {
    std::lock_guard<std::mutex> owner_lock(ownership_mutex_);
    stale_owner_blocks.reserve(ownership_.size());
    for (const auto& entry : ownership_) {
      stale_owner_blocks.push_back(entry.second.block);
    }
    ownership_.clear();
  }
  for (const auto& block : stale_owner_blocks) {
    remote_free_stale_owner_count_.fetch_add(1, std::memory_order_relaxed);
    remote_free_direct_reconcile_count_.fetch_add(1, std::memory_order_relaxed);
    (void)FreeBlockToBackingPool(block, &result);
  }

  {
    std::lock_guard<std::mutex> lock(allocator_mutex_);
    if (allocator_ != nullptr) {
      auto released = allocator_->Release();
      if (!released.ok() && result.ok()) {
        result.status = released.status;
        result.fail_closed = released.fail_closed;
        result.diagnostic = released.diagnostic;
      }
      result.evidence.insert(result.evidence.end(),
                             released.evidence.begin(),
                             released.evidence.end());
      allocator_.reset();
    }
  }
  result.freed = result.status.ok() && !result.fail_closed;
  result.drained_to_backing_pool = true;
  result.evidence.push_back("thread_local_memory_cache.release.reconciled=true");
  result.evidence.push_back("thread_local_memory_cache.release.state_count=" +
                            std::to_string(states.size()));
  result.evidence.push_back("thread_local_memory_cache.release.stale_owner_reconciled_count=" +
                            std::to_string(stale_owner_blocks.size()));
  return result;
}

ThreadLocalCacheThreadSnapshot
ThreadLocalPerCoreMemoryCache::SnapshotStateLocked(
    const ThreadCacheState& state) const {
  ThreadLocalCacheThreadSnapshot snapshot;
  snapshot.thread_id_hash = state.thread_id_hash;
  snapshot.shard_index = state.shard_index;
  snapshot.current_core_available = state.current_core_available;
  snapshot.core_id = state.core_id;
  snapshot.numa_node_available = state.numa_node_available;
  snapshot.numa_node = state.numa_node;
  snapshot.owner_id = request_.owner_id;
  snapshot.context_id = request_.route_label;
  snapshot.category = request_.category;
  snapshot.hit_count = state.hit_count;
  snapshot.miss_count = state.miss_count;
  snapshot.refill_count = state.refill_count;
  snapshot.drain_count = state.drain_count;
  snapshot.remote_free_handoff_count = state.remote_free_handoff_count;
  snapshot.remote_free_enqueued_count = state.remote_free_enqueued_count;
  snapshot.remote_free_dequeued_count = state.remote_free_dequeued_count;
  snapshot.remote_free_drained_count = state.remote_free_drained_count;
  snapshot.remote_free_rejected_count = state.remote_free_rejected_count;
  snapshot.remote_free_backpressure_count = state.remote_free_backpressure_count;
  snapshot.remote_free_direct_reconcile_count =
      state.remote_free_direct_reconcile_count;
  snapshot.remote_free_stale_owner_count = state.remote_free_stale_owner_count;
  snapshot.remote_free_queue_current_count =
      static_cast<u64>(state.remote_free_queue.size());
  snapshot.remote_free_queue_peak_count = state.remote_free_queue_peak_count;
  snapshot.remote_free_queue_current_bytes = state.remote_free_queue_current_bytes;
  snapshot.remote_free_queue_peak_bytes = state.remote_free_queue_peak_bytes;
  snapshot.allocation_count = state.allocation_count;
  snapshot.free_count = state.free_count;
  snapshot.reconciliation_count = state.reconciliation_count;
  snapshot.current_cached_bytes = state.current_cached_bytes;
  snapshot.peak_cached_bytes = state.peak_cached_bytes;
  snapshot.current_cached_objects = state.current_cached_objects;
  snapshot.peak_cached_objects = state.peak_cached_objects;
  snapshot.current_active_bytes = state.current_active_bytes;
  snapshot.current_active_objects = state.current_active_objects;
  snapshot.allocation_latency_p50_ns = Percentile(state.allocation_latencies_ns, 50);
  snapshot.allocation_latency_p95_ns = Percentile(state.allocation_latencies_ns, 95);
  snapshot.allocation_latency_p99_ns = Percentile(state.allocation_latencies_ns, 99);
  snapshot.remote_free_enqueue_latency_p50_ns =
      Percentile(state.remote_free_enqueue_latencies_ns, 50);
  snapshot.remote_free_enqueue_latency_p95_ns =
      Percentile(state.remote_free_enqueue_latencies_ns, 95);
  snapshot.remote_free_enqueue_latency_p99_ns =
      Percentile(state.remote_free_enqueue_latencies_ns, 99);
  snapshot.remote_free_reconcile_latency_p50_ns =
      Percentile(state.remote_free_reconcile_latencies_ns, 50);
  snapshot.remote_free_reconcile_latency_p95_ns =
      Percentile(state.remote_free_reconcile_latencies_ns, 95);
  snapshot.remote_free_reconcile_latency_p99_ns =
      Percentile(state.remote_free_reconcile_latencies_ns, 99);
  snapshot.remote_thread_hashes = state.remote_thread_hashes;
  snapshot.evidence = state.topology_evidence;
  snapshot.evidence.push_back("thread_local_memory_cache.thread_id_hash=" +
                              std::to_string(snapshot.thread_id_hash));
  snapshot.evidence.push_back("thread_local_memory_cache.shard_index=" +
                              std::to_string(snapshot.shard_index));
  snapshot.evidence.push_back("thread_local_memory_cache.owner_id=" + request_.owner_id);
  snapshot.evidence.push_back("thread_local_memory_cache.context_id=" + request_.route_label);
  snapshot.evidence.push_back("thread_local_memory_cache.category=" +
                              std::string(MemoryCategoryName(request_.category)));
  snapshot.evidence.push_back("thread_local_memory_cache.remote_free_queue.current_count=" +
                              std::to_string(snapshot.remote_free_queue_current_count));
  snapshot.evidence.push_back("thread_local_memory_cache.remote_free_queue.current_bytes=" +
                              std::to_string(snapshot.remote_free_queue_current_bytes));
  snapshot.evidence.push_back("thread_local_memory_cache.numa_node_available=" +
                              BoolText(snapshot.numa_node_available));
  return snapshot;
}

ThreadLocalCacheSnapshot ThreadLocalPerCoreMemoryCache::Snapshot() const {
  ThreadLocalCacheSnapshot snapshot;
  snapshot.owner_id = request_.owner_id;
  snapshot.route_label = request_.route_label;
  snapshot.operation_id = request_.operation_id;
  snapshot.memory_class = request_.memory_class;
  snapshot.category = request_.category;
  snapshot.object_kind = request_.object_kind;
  snapshot.reservation_bytes = request_.reservation_bytes;
  snapshot.active = !released_.load(std::memory_order_acquire) && allocator_ != nullptr;
  snapshot.locality_portable_fallback_used = locality_decision_.portable_fallback_used;
  snapshot.numa_hint_applied = locality_decision_.numa_hint_applied;
  snapshot.numa_node_discovered =
      locality_decision_.capabilities.numa_hint_supported ||
      locality_decision_.numa_hint_applied;
  snapshot.shard_count = shards_.size();
  snapshot.authority_scope = kAuthorityScope;
  AppendBaseEvidence(&snapshot.evidence);
  snapshot.evidence.insert(snapshot.evidence.end(),
                           topology_evidence_.begin(),
                           topology_evidence_.end());

  std::vector<u64> latencies;
  std::vector<u64> remote_enqueue_latencies;
  std::vector<u64> remote_reconcile_latencies;
  const auto states = LiveThreadStates();
  for (const auto& state : states) {
    std::lock_guard<std::mutex> lock(state->mutex);
    auto thread_snapshot = SnapshotStateLocked(*state);
    snapshot.hit_count += thread_snapshot.hit_count;
    snapshot.miss_count += thread_snapshot.miss_count;
    snapshot.refill_count += thread_snapshot.refill_count;
    snapshot.drain_count += thread_snapshot.drain_count;
    snapshot.remote_free_handoff_count += thread_snapshot.remote_free_handoff_count;
    snapshot.remote_free_enqueued_count += thread_snapshot.remote_free_enqueued_count;
    snapshot.remote_free_dequeued_count += thread_snapshot.remote_free_dequeued_count;
    snapshot.remote_free_drained_count += thread_snapshot.remote_free_drained_count;
    snapshot.remote_free_rejected_count += thread_snapshot.remote_free_rejected_count;
    snapshot.remote_free_backpressure_count +=
        thread_snapshot.remote_free_backpressure_count;
    snapshot.remote_free_direct_reconcile_count +=
        thread_snapshot.remote_free_direct_reconcile_count;
    snapshot.remote_free_stale_owner_count +=
        thread_snapshot.remote_free_stale_owner_count;
    snapshot.remote_free_queue_current_count +=
        thread_snapshot.remote_free_queue_current_count;
    snapshot.remote_free_queue_peak_count =
        std::max(snapshot.remote_free_queue_peak_count,
                 thread_snapshot.remote_free_queue_peak_count);
    snapshot.remote_free_queue_current_bytes +=
        thread_snapshot.remote_free_queue_current_bytes;
    snapshot.remote_free_queue_peak_bytes =
        std::max(snapshot.remote_free_queue_peak_bytes,
                 thread_snapshot.remote_free_queue_peak_bytes);
    snapshot.allocation_count += thread_snapshot.allocation_count;
    snapshot.free_count += thread_snapshot.free_count;
    snapshot.reconciliation_count += thread_snapshot.reconciliation_count;
    snapshot.current_cached_bytes += thread_snapshot.current_cached_bytes;
    snapshot.peak_cached_bytes = std::max(snapshot.peak_cached_bytes,
                                          thread_snapshot.peak_cached_bytes);
    snapshot.current_cached_objects += thread_snapshot.current_cached_objects;
    snapshot.peak_cached_objects = std::max(snapshot.peak_cached_objects,
                                            thread_snapshot.peak_cached_objects);
    snapshot.current_active_bytes += thread_snapshot.current_active_bytes;
    snapshot.current_active_objects += thread_snapshot.current_active_objects;
    latencies.insert(latencies.end(),
                     state->allocation_latencies_ns.begin(),
                     state->allocation_latencies_ns.end());
    remote_enqueue_latencies.insert(
        remote_enqueue_latencies.end(),
        state->remote_free_enqueue_latencies_ns.begin(),
        state->remote_free_enqueue_latencies_ns.end());
    remote_reconcile_latencies.insert(
        remote_reconcile_latencies.end(),
        state->remote_free_reconcile_latencies_ns.begin(),
        state->remote_free_reconcile_latencies_ns.end());
    snapshot.threads.push_back(std::move(thread_snapshot));
  }
  snapshot.allocation_latency_p50_ns = Percentile(latencies, 50);
  snapshot.allocation_latency_p95_ns = Percentile(latencies, 95);
  snapshot.allocation_latency_p99_ns = Percentile(latencies, 99);
  {
    std::lock_guard<std::mutex> lock(remote_latency_mutex_);
    remote_enqueue_latencies.insert(remote_enqueue_latencies.end(),
                                    remote_free_enqueue_latencies_ns_.begin(),
                                    remote_free_enqueue_latencies_ns_.end());
    remote_reconcile_latencies.insert(
        remote_reconcile_latencies.end(),
        remote_free_reconcile_latencies_ns_.begin(),
        remote_free_reconcile_latencies_ns_.end());
  }
  snapshot.remote_free_enqueue_latency_p50_ns =
      Percentile(remote_enqueue_latencies, 50);
  snapshot.remote_free_enqueue_latency_p95_ns =
      Percentile(remote_enqueue_latencies, 95);
  snapshot.remote_free_enqueue_latency_p99_ns =
      Percentile(remote_enqueue_latencies, 99);
  snapshot.remote_free_reconcile_latency_p50_ns =
      Percentile(remote_reconcile_latencies, 50);
  snapshot.remote_free_reconcile_latency_p95_ns =
      Percentile(remote_reconcile_latencies, 95);
  snapshot.remote_free_reconcile_latency_p99_ns =
      Percentile(remote_reconcile_latencies, 99);
  snapshot.remote_free_direct_reconcile_count +=
      remote_free_direct_reconcile_count_.load(std::memory_order_relaxed);
  snapshot.remote_free_stale_owner_count +=
      remote_free_stale_owner_count_.load(std::memory_order_relaxed);
  snapshot.thread_exit_cleanup_count =
      thread_exit_cleanup_count_.load(std::memory_order_relaxed);
  snapshot.thread_exit_reconciled_count =
      thread_exit_reconciled_count_.load(std::memory_order_relaxed);
  snapshot.thread_exit_reconciled_bytes =
      thread_exit_reconciled_bytes_.load(std::memory_order_relaxed);
  {
    std::lock_guard<std::mutex> owner_lock(ownership_mutex_);
    for (const auto& entry : ownership_) {
      if (entry.second.owner_state.expired()) {
        ++snapshot.remote_free_stale_owner_pending_count;
      }
    }
  }

  for (usize index = 0; index < shards_.size(); ++index) {
    ThreadLocalCacheShardSnapshot shard;
    shard.shard_index = index;
    shard.hit_count = shards_[index]->hit_count.load(std::memory_order_relaxed);
    shard.miss_count = shards_[index]->miss_count.load(std::memory_order_relaxed);
    shard.refill_count = shards_[index]->refill_count.load(std::memory_order_relaxed);
    shard.drain_count = shards_[index]->drain_count.load(std::memory_order_relaxed);
    shard.remote_free_handoff_count =
        shards_[index]->remote_free_handoff_count.load(std::memory_order_relaxed);
    shard.remote_free_enqueued_count =
        shards_[index]->remote_free_enqueued_count.load(std::memory_order_relaxed);
    shard.remote_free_dequeued_count =
        shards_[index]->remote_free_dequeued_count.load(std::memory_order_relaxed);
    shard.remote_free_drained_count =
        shards_[index]->remote_free_drained_count.load(std::memory_order_relaxed);
    shard.remote_free_rejected_count =
        shards_[index]->remote_free_rejected_count.load(std::memory_order_relaxed);
    shard.remote_free_backpressure_count =
        shards_[index]->remote_free_backpressure_count.load(std::memory_order_relaxed);
    shard.remote_free_queue_current_count =
        shards_[index]->remote_free_queue_current_count.load(std::memory_order_relaxed);
    shard.remote_free_queue_peak_count =
        shards_[index]->remote_free_queue_peak_count.load(std::memory_order_relaxed);
    shard.remote_free_queue_current_bytes =
        shards_[index]->remote_free_queue_current_bytes.load(std::memory_order_relaxed);
    shard.remote_free_queue_peak_bytes =
        shards_[index]->remote_free_queue_peak_bytes.load(std::memory_order_relaxed);
    shard.current_cached_bytes =
        shards_[index]->current_cached_bytes.load(std::memory_order_relaxed);
    shard.peak_cached_bytes =
        shards_[index]->peak_cached_bytes.load(std::memory_order_relaxed);
    snapshot.shards.push_back(shard);
  }

  {
    std::lock_guard<std::mutex> lock(allocator_mutex_);
    if (allocator_ != nullptr) {
      const auto backing = allocator_->Snapshot();
      snapshot.active = snapshot.active && backing.active;
      snapshot.backing_pool_retained_bytes = backing.retained_bytes;
      snapshot.backing_pool_allocation_count = backing.allocation_count;
      snapshot.backing_pool_reuse_count = backing.reuse_count;
    }
  }
  return snapshot;
}

ThreadLocalCacheAcquireResult CreateThreadLocalPerCoreMemoryCache(
    ThreadLocalCacheRequest request) {
  if (request.reservation_ledger == nullptr) {
    return RefuseAcquire(std::move(request),
                         "SB_CEIC_014_THREAD_CACHE.RESERVATION_LEDGER_REQUIRED",
                         "memory.ceic_014.thread_cache.reservation_ledger_required",
                         "hierarchical_memory_budget_ledger_required");
  }
  if (request.memory_manager == nullptr) {
    return RefuseAcquire(std::move(request),
                         "SB_CEIC_014_THREAD_CACHE.MEMORY_MANAGER_REQUIRED",
                         "memory.ceic_014.thread_cache.memory_manager_required",
                         "memory_manager_required");
  }
  if (request.scope_chain.empty()) {
    return RefuseAcquire(std::move(request),
                         "SB_CEIC_014_THREAD_CACHE.SCOPE_CHAIN_REQUIRED",
                         "memory.ceic_014.thread_cache.scope_chain_required",
                         "scope_chain_required");
  }
  if (Blank(request.owner_id) || Blank(request.route_label) ||
      Blank(request.operation_id)) {
    return RefuseAcquire(std::move(request),
                         "SB_CEIC_014_THREAD_CACHE.IDENTITY_REQUIRED",
                         "memory.ceic_014.thread_cache.identity_required",
                         "owner_route_and_operation_required");
  }
  if (request.reservation_bytes == 0) {
    return RefuseAcquire(std::move(request),
                         "SB_CEIC_014_THREAD_CACHE.ZERO_RESERVATION",
                         "memory.ceic_014.thread_cache.zero_reservation",
                         "reservation_bytes_required");
  }
  if (request.refill_batch_count == 0) {
    request.refill_batch_count = kDefaultRefillBatchCount;
  }
  if (request.max_cached_slots_per_class_per_thread == 0) {
    request.max_cached_slots_per_class_per_thread = kDefaultMaxCachedSlotsPerClass;
  }
  if (request.max_cached_bytes_per_thread == 0) {
    request.max_cached_bytes_per_thread = kDefaultMaxCachedBytesPerThread;
  }
  NormalizeSizeClasses(&request.size_classes);
  std::string invalid_size_classes;
  if (!ValidateSizeClasses(request.size_classes, &invalid_size_classes)) {
    return RefuseAcquire(std::move(request),
                         "SB_CEIC_014_THREAD_CACHE.SIZE_CLASS_INVALID",
                         "memory.ceic_014.thread_cache.size_class_invalid",
                         std::move(invalid_size_classes));
  }
  std::string unsafe_reason;
  if (UnsafeAuthority(request, &unsafe_reason)) {
    return RefuseAcquire(std::move(request),
                         "SB_CEIC_014_THREAD_CACHE.UNSAFE_AUTHORITY",
                         "memory.ceic_014.thread_cache.unsafe_authority",
                         std::move(unsafe_reason));
  }
  if (request.category == MemoryCategory::unknown) {
    request.category = TypedSlabPoolDefaultCategory(request.object_kind);
  }
  if (request.memory_class.empty()) {
    request.memory_class =
        std::string("ceic_014.") + TypedSlabPoolObjectKindName(request.object_kind);
  }
  if (request.purpose.empty()) {
    request.purpose =
        std::string("ceic_014.") + TypedSlabPoolObjectKindName(request.object_kind);
  }

  auto locality = EvaluateMemoryLocalityPolicy(request.locality_policy);
  if (!locality.ok()) {
    auto result = RefuseAcquire(
        request,
        "SB_CEIC_014_THREAD_CACHE.LOCALITY_POLICY_REFUSED",
        "memory.ceic_014.thread_cache.locality_policy_refused",
        locality.diagnostic.diagnostic_code.empty()
            ? "locality_policy_refused"
            : locality.diagnostic.diagnostic_code,
        locality.status.code);
    result.diagnostic = locality.diagnostic;
    result.evidence.insert(result.evidence.end(),
                           locality.evidence.begin(),
                           locality.evidence.end());
    return result;
  }

  const auto topology = scratchbird::core::platform::CurrentRuntimeTopology();
  usize shard_count = request.shard_count;
  if (shard_count == 0) {
    shard_count = topology.logical_core_count_available
                      ? std::max<usize>(1, topology.logical_core_count)
                      : 1;
  }

  SizeClassAllocatorRequest pool_request;
  pool_request.reservation_ledger = request.reservation_ledger;
  pool_request.memory_manager = request.memory_manager;
  pool_request.scope_chain = request.scope_chain;
  pool_request.category = request.category;
  pool_request.memory_class = request.memory_class;
  pool_request.reservation_bytes = request.reservation_bytes;
  pool_request.owner_id = request.owner_id;
  pool_request.route_label = request.route_label;
  pool_request.operation_id = request.operation_id;
  pool_request.purpose = request.purpose;
  pool_request.object_kind = request.object_kind;
  pool_request.size_classes = request.size_classes;
  pool_request.spillable = request.spillable;
  pool_request.cancelable = request.cancelable;
  pool_request.priority = request.priority;
  pool_request.weight = request.weight;
  pool_request.lease_expires_at_ms = request.lease_expires_at_ms;
  pool_request.production_like = request.production_like;
  pool_request.provenance = request.provenance;
  pool_request.authority.engine_mga_authoritative =
      request.authority.engine_mga_authoritative;
  pool_request.authority.transaction_inventory_authoritative =
      request.authority.transaction_inventory_authoritative;
  pool_request.authority.security_or_policy_checked =
      request.authority.security_or_policy_checked;
  pool_request.authority.parser_or_reference_finality_authority =
      request.authority.parser_or_reference_finality_authority;
  pool_request.authority.memory_visibility_or_finality_authority =
      request.authority.memory_visibility_or_finality_authority;
  pool_request.authority.memory_recovery_authority =
      request.authority.memory_recovery_authority;
  pool_request.authority.memory_authorization_authority =
      request.authority.memory_authorization_authority;
  pool_request.authority.benchmark_authority = request.authority.benchmark_authority;
  pool_request.authority.cluster_authority = request.authority.cluster_authority;
  pool_request.authority.debug_or_relaxed_path = request.authority.debug_or_relaxed_path;
  pool_request.authority.optimizer_plan_authority =
      request.authority.optimizer_plan_authority;
  pool_request.authority.index_finality_authority =
      request.authority.index_finality_authority;
  pool_request.authority.agent_action_authority =
      request.authority.agent_action_authority;

  auto pool = CreateSizeClassAllocator(std::move(pool_request));
  if (!pool.ok()) {
    ThreadLocalCacheAcquireResult result;
    result.status = pool.status;
    result.fail_closed = true;
    result.diagnostic = pool.diagnostic;
    result.evidence = pool.evidence;
    result.evidence.push_back(kAnchor);
    result.evidence.push_back("thread_local_memory_cache.reservation_created=false");
    return result;
  }

  ThreadLocalCacheAcquireResult result;
  result.status = OkStatus();
  result.evidence = pool.evidence;
  result.evidence.push_back(kAnchor);
  result.evidence.push_back(kRemoteAnchor);
  result.evidence.push_back(kAuthorityScope);
  result.evidence.push_back("thread_local_memory_cache.reservation_first=true");
  result.evidence.push_back("thread_local_memory_cache.backing_pool=typed_slab_pool");
  result.evidence.push_back("thread_local_memory_cache.shard_count=" +
                            std::to_string(shard_count));
  result.evidence.push_back("thread_local_memory_cache.locality_portable_fallback_used=" +
                            BoolText(locality.portable_fallback_used));
  result.evidence.push_back("thread_local_memory_cache.numa_hint_applied=" +
                            BoolText(locality.numa_hint_applied));
  result.evidence.insert(result.evidence.end(),
                         locality.evidence.begin(),
                         locality.evidence.end());
  AppendTopologyEvidence(&result.evidence, topology);
  result.diagnostic = MakeCacheDiagnostic(
      result.status,
      "SB_CEIC_014_THREAD_CACHE.OK",
      "memory.ceic_014.thread_cache.ok",
      request,
      {{"shard_count", std::to_string(shard_count)},
       {"size_class_count", std::to_string(request.size_classes.size())},
       {"locality_portable_fallback_used", BoolText(locality.portable_fallback_used)}});
  result.cache.reset(new ThreadLocalPerCoreMemoryCache(
      std::move(request),
      std::move(locality),
      std::move(pool.allocator),
      shard_count,
      topology.evidence));
  return result;
}

}  // namespace scratchbird::core::memory

// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "runtime_identity_records.hpp"
#include "resource_governance_admission.hpp"
#include <list>
#include <mutex>

namespace scratchbird::core::runtime {
struct NativeTaskQueueBinding {
  Uuid queue_uuid, scheduler_uuid, node_uuid, open_generation_uuid;
  RuntimeGeneration generation = 0;
  std::uint32_t maximum_depth = 0;
  agents::RuntimePermitAuthority authority;
  agents::RuntimePermitInstanceBinding capacity_pool;
};
struct NativeTaskQueueKey {
  RuntimeTaskIdentityRecord task;
  Uuid attempt_uuid;
  Uuid queue_uuid;
  RuntimeGeneration queue_generation = 0;
  bool operator==(const NativeTaskQueueKey&) const noexcept = default;
};
enum class NativeTaskQueueCode {
  bound, inserted, removed, observed, empty, closed, full, duplicate, conflict,
  invalid_binding, allocation_failed, synchronization_failed, release_failed,
  not_found, stale_identity, claimed, cancelled, timed_out
};
struct NativeTaskQueueResult {
  NativeTaskQueueCode code = NativeTaskQueueCode::invalid_binding;
  std::optional<NativeTaskQueueKey> key = std::nullopt;
  agents::RuntimePermitCode permit_code = agents::RuntimePermitCode::no_grant;
};

struct NativeTaskQueueClaimKey {
  NativeTaskQueueKey queued;
  RuntimeWorkerIdentityRecord worker;
  bool operator==(const NativeTaskQueueClaimKey&) const noexcept = default;
};
struct NativeTaskQueueClaimControl {
  std::stop_token cancellation;
  std::optional<std::chrono::steady_clock::time_point> deadline;
};

// Owns a real worker slot, not action authority or a task payload. The caller
// retains task/worker/manager and authority fences separately. Explicit release
// requires the exact claim key and prior worker quiescence. An abandoned active
// claim fails fast instead of silently releasing capacity under live execution.
// Calls, moves and borrowed key access on one claim are caller-serialized.
class NativeTaskQueueClaim {
 public:
  NativeTaskQueueClaim() = default;
  NativeTaskQueueClaim(const NativeTaskQueueClaim&) = delete;
  NativeTaskQueueClaim& operator=(const NativeTaskQueueClaim&) = delete;
  NativeTaskQueueClaim(NativeTaskQueueClaim&& other) noexcept
      : key_(other.key_), slot_(std::move(other.slot_)) {}
  NativeTaskQueueClaim& operator=(NativeTaskQueueClaim&&) = delete;
  ~NativeTaskQueueClaim() { if (slot_) std::terminate(); }
  explicit operator bool() const noexcept { return bool(slot_); }
  const NativeTaskQueueClaimKey* key() const noexcept { return slot_ ? &key_ : nullptr; }
  agents::RuntimePermitCode ReleaseAfterQuiescence(const NativeTaskQueueClaimKey& expected) noexcept {
    using C = agents::RuntimePermitCode;
    if (!slot_) return C::no_grant;
    if (key_ != expected) return C::invalid_binding;
    return slot_.Release(slot_.view()->binding);
  }
 private:
  NativeTaskQueueClaimKey key_;
  agents::RuntimePermitGrant slot_;
  friend class NativeRuntimeTaskQueue;
};

// SEARCH_KEY: NATIVE_RUNTIME_TASK_QUEUE_CREDIT_OWNERSHIP
// Bounded FIFO storage, not a scheduler or a private task catalog. The real
// governor owns capacity; maximum_depth is an additional selected queue bound.
// A copied key is only an observation. No method authorizes dispatch, changes
// task state, resolves targets, or acknowledges physical/durable completion.
//
// Trusted owning-kernel binding, not authenticated policy. The caller serializes
// task admission/cancellation/state with queue changes and owns all descriptor
// payloads. This queue stores only fixed binary keys, never borrowed pointers.
//
// Native lifetime contract: external owner retains this object, governor, issuer
// and governed metadata until all calls return. Close, remove all entries, join
// external users, then destroy. Close neither removes entries nor releases their
// credits. A stopped governor alone is not the queue's Close admission fence.
// Lock order: caller task predicate -> queue -> governor -> governed metadata.
// No external callbacks or blocking capacity waits occur under the queue lock.
class NativeRuntimeTaskQueue {
 public:
  NativeRuntimeTaskQueue(NativeTaskQueueBinding binding,
      agents::ResourceGovernanceReservationLedger& governor,
      memory::ReservationBackedPmrMemoryResource& metadata)
      : binding_(binding), governor_(governor), entries_(&metadata) {
    const auto pool = governor_.InspectRuntimePermitInstance(binding.authority,
        agents::RuntimePermitProfile::queued_task, binding.capacity_pool);
    valid_ = uuid::IsEngineIdentityUuid(binding.queue_uuid) &&
        uuid::IsEngineIdentityUuid(binding.scheduler_uuid) &&
        uuid::IsEngineIdentityUuid(binding.node_uuid) &&
        uuid::IsEngineIdentityUuid(binding.open_generation_uuid) &&
        pool.code == agents::RuntimePermitCode::bound && !pool.closed &&
        binding.maximum_depth <= pool.capacity;
    closed_ = !valid_;
  }
  NativeRuntimeTaskQueue(const NativeRuntimeTaskQueue&) = delete;
  NativeRuntimeTaskQueue& operator=(const NativeRuntimeTaskQueue&) = delete;
  ~NativeRuntimeTaskQueue() {
    // Never silently discard pending work or uncharge a still-owned queue entry.
    if (!closed_ || !entries_.empty()) std::terminate();
  }
  NativeTaskQueueCode BindStatus() const noexcept {
    return valid_ ? NativeTaskQueueCode::bound : NativeTaskQueueCode::invalid_binding;
  }
  NativeTaskQueueCode Close() noexcept {
    std::unique_lock lock(mutex_, std::defer_lock);
    try { lock.lock(); } catch (const std::system_error&) { return NativeTaskQueueCode::synchronization_failed; }
    if (!valid_) return NativeTaskQueueCode::invalid_binding;
    closed_ = true;
    return NativeTaskQueueCode::closed;
  }
  NativeTaskQueueResult Push(const NativeTaskQueueKey& key,
      agents::RuntimePermitGrant& credit) noexcept {
    using C = NativeTaskQueueCode;
    std::unique_lock lock(mutex_, std::defer_lock);
    try { lock.lock(); } catch (const std::system_error&) { return {C::synchronization_failed}; }
    if (!valid_) return {};
    if (closed_) return {C::closed};
    if (!KeyShape(key)) return {};
    // A duplicate notification may have no provisional credit: the real
    // governor refuses a second grant for the same task/attempt. This is only
    // an observation of the existing entry, never a new admission/ownership.
    for (const auto& entry : entries_) {
      if (entry.key.task.identity.runtime_object_uuid == key.task.identity.runtime_object_uuid)
        return {entry.key == key ? C::duplicate : C::conflict};
    }
    if (!credit.IssuedBy(governor_)) return {};
    const auto* view = credit.view();
    if (!view || view->binding.profile != agents::RuntimePermitProfile::queued_task ||
        view->binding.authority != binding_.authority ||
        view->binding.semaphore != binding_.capacity_pool.semaphore ||
        view->binding.semaphore_generation != binding_.capacity_pool.generation ||
        view->binding.task != key.task.identity.runtime_object_uuid ||
        view->binding.attempt != key.attempt_uuid || view->binding.worker || view->binding.quantity != 1)
      return {};
    if (entries_.size() >= binding_.maximum_depth) return {C::full};
    // Allocate before transferring ownership. Entry construction contains only
    // fixed values and cannot throw. No fallible work follows the grant move.
    try { entries_.emplace_back(key); }
    catch (const std::bad_alloc&) { return {C::allocation_failed}; }
    catch (const std::system_error&) { return {C::synchronization_failed}; }
    entries_.back().credit = std::move(credit);
    return {C::inserted, key, agents::RuntimePermitCode::granted};
  }
  NativeTaskQueueResult Front() const noexcept {
    std::unique_lock lock(mutex_, std::defer_lock);
    try { lock.lock(); } catch (const std::system_error&) { return {NativeTaskQueueCode::synchronization_failed}; }
    if (!valid_) return {};
    if (entries_.empty()) return {closed_ ? NativeTaskQueueCode::closed : NativeTaskQueueCode::empty};
    return {NativeTaskQueueCode::observed, entries_.front().key};
  }
  NativeTaskQueueResult Remove(const NativeTaskQueueKey& expected) noexcept {
    using C = NativeTaskQueueCode;
    std::unique_lock lock(mutex_, std::defer_lock);
    try { lock.lock(); } catch (const std::system_error&) { return {C::synchronization_failed}; }
    if (!valid_ || !KeyShape(expected)) return {};
    for (auto it = entries_.begin(); it != entries_.end(); ++it) {
      if (it->key.task.identity.runtime_object_uuid != expected.task.identity.runtime_object_uuid) continue;
      if (it->key != expected) return {C::stale_identity};
      // Queue users cannot retain an entry pointer. Holding this lock proves its
      // local quiescence. Release failure preserves both the node and capability.
      const auto released = it->credit.Release(it->credit.view()->binding);
      if (released != agents::RuntimePermitCode::released) return {C::release_failed, {}, released};
      entries_.erase(it);
      return {C::removed, expected, released};
    }
    return {C::not_found};
  }
  // SEARCH_KEY: NATIVE_TASK_QUEUE_WORKER_SLOT_TRANSFER
  // Acquire provisional capacity outside this predicate. Caller holds its
  // actual task-state/authority fence through this call and publication of
  // running. This resource transfer alone does not validate current policy,
  // worker/manager generation, fairness or L3 role. Optional cooperative stop
  // and monotonic deadline are rechecked after obtaining the queue predicate,
  // before ownership changes. They cannot interrupt a native mutex acquisition
  // or revoke an issued claim. External cancellation acknowledgment still owns
  // in-flight requests and cleanup of refused provisional slots.
  // No user callback or fallible result construction follows queue-credit
  // release. Close refuses fresh claims, unlike explicit pending-entry removal.
  NativeTaskQueueResult Claim(const NativeTaskQueueClaimKey& expected,
      agents::RuntimePermitGrant& provisional_worker, NativeTaskQueueClaim& output,
      const NativeTaskQueueClaimControl& control = {}) noexcept {
    using C = NativeTaskQueueCode;
    std::unique_lock lock(mutex_, std::defer_lock);
    try { lock.lock(); } catch (const std::system_error&) { return {C::synchronization_failed}; }
    if (!valid_) return {};
    if (closed_) return {C::closed};
    if (output || !KeyShape(expected.queued) ||
        CheckWorkerIdentityShape(expected.worker) != RuntimeIdentityShapeError::none ||
        expected.worker.task != expected.queued.task || !provisional_worker.IssuedBy(governor_)) return {};
    const auto* view = provisional_worker.view();
    if (!view || view->binding.profile != agents::RuntimePermitProfile::worker_slot ||
        view->binding.authority != binding_.authority || view->binding.quantity != 1 ||
        view->binding.task != expected.queued.task.identity.runtime_object_uuid ||
        view->binding.attempt != expected.queued.attempt_uuid ||
        view->binding.worker != expected.worker.identity.runtime_object_uuid) return {};
    for (auto it = entries_.begin(); it != entries_.end(); ++it) {
      if (it->key.task.identity.runtime_object_uuid != expected.queued.task.identity.runtime_object_uuid) continue;
      if (it->key != expected.queued) return {C::stale_identity};
      // Terminal selection for a valid pending claim precedes the irreversible
      // queue-credit release. Issued claims retain their existing ownership.
      if (control.cancellation.stop_requested()) return {C::cancelled};
      if (control.deadline && std::chrono::steady_clock::now() >= *control.deadline) return {C::timed_out};
      const auto released = it->credit.Release(it->credit.view()->binding);
      if (released != agents::RuntimePermitCode::released) return {C::release_failed, {}, released};
      entries_.erase(it);
      output.key_ = expected;
      output.slot_ = std::move(provisional_worker);
      return {C::claimed, expected.queued, agents::RuntimePermitCode::granted};
    }
    return {C::not_found};
  }
 private:
  bool KeyShape(const NativeTaskQueueKey& key) const noexcept {
    return CheckTaskIdentityShape(key.task) == RuntimeIdentityShapeError::none &&
        uuid::IsEngineIdentityUuid(key.attempt_uuid) &&
        key.queue_uuid == binding_.queue_uuid && key.queue_generation == binding_.generation &&
        key.task.scheduler_uuid == binding_.scheduler_uuid &&
        key.task.identity.owner_node_uuid == binding_.node_uuid &&
        key.task.identity.open_generation_uuid == binding_.open_generation_uuid;
  }
  struct Entry {
    explicit Entry(NativeTaskQueueKey value) noexcept : key(value) {}
    NativeTaskQueueKey key;
    agents::RuntimePermitGrant credit;
  };
  const NativeTaskQueueBinding binding_;
  agents::ResourceGovernanceReservationLedger& governor_;
  mutable std::mutex mutex_;
  std::pmr::list<Entry> entries_;
  bool valid_ = false, closed_ = true;
};
} // namespace scratchbird::core::runtime

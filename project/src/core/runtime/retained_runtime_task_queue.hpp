// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "native_runtime_task_queue.hpp"
#include "memory_safe_retirement.hpp"
#include "checked_condition.hpp"

namespace scratchbird::core::runtime {
struct RetainedTaskQueueLimits { std::uint32_t operation_references = 0; };
struct RetainedTaskQueueDrainResult {
  memory::SafeRetirementStatus status = memory::SafeRetirementStatus::invalid_request;
  bool drained = false;
  std::uint32_t operation_references = 0;
  std::optional<NativeTaskQueueKey> pending = std::nullopt;
};
struct RetainedTaskQueueSnapshot {
  bool initialized = false, closed = true, fenced = true, drain_observed = false;
  std::uint32_t operation_references = 0;
  NativeTaskQueueBinding binding;
  NativeTaskQueueResult front;
};
namespace detail {
struct RetainedTaskQueueState {
  const NativeTaskQueueBinding binding;
  const RetainedTaskQueueLimits limits;
  memory::SafeRetirementHandle handle;
  std::mutex lifetime;
  platform::CheckedCondition changed;
  std::uint32_t references = 0;
  bool closed = true, fenced = true, retired = false, drained = false;
  // Construct the native queue only after acquiring the actual owner hazard.
  // Failed Protect must leave an inert payload safe for domain retirement.
  std::optional<NativeRuntimeTaskQueue> queue;
  RetainedTaskQueueState(NativeTaskQueueBinding b, RetainedTaskQueueLimits l) : binding(b), limits(l) {}
  ~RetainedTaskQueueState() noexcept {
    if (references || !closed || !fenced) std::terminate();
  }
  void NotifyLocked() noexcept { if (!changed.NotifyAll()) std::terminate(); }
  void ReleaseReference() noexcept {
    std::lock_guard lock(lifetime);
    if (!references) std::terminate();
    --references; NotifyLocked();
  }
};
}

class RetainedRuntimeTaskQueueOwner;
// Moves and calls on one operation are caller-serialized. The real memory guard
// survives queue close/fencing and retains storage through complete result
// delivery. It neither retains an external task payload nor authorizes dispatch.
class RetainedRuntimeTaskQueueOperation {
 public:
  RetainedRuntimeTaskQueueOperation() = default;
  RetainedRuntimeTaskQueueOperation(const RetainedRuntimeTaskQueueOperation&) = delete;
  RetainedRuntimeTaskQueueOperation& operator=(const RetainedRuntimeTaskQueueOperation&) = delete;
  RetainedRuntimeTaskQueueOperation(RetainedRuntimeTaskQueueOperation&& other) noexcept
      : state_(std::exchange(other.state_, nullptr)), guard_(std::move(other.guard_)) {}
  RetainedRuntimeTaskQueueOperation& operator=(RetainedRuntimeTaskQueueOperation&& other) noexcept {
    if (this != &other) { Reset(); state_ = std::exchange(other.state_, nullptr); guard_ = std::move(other.guard_); }
    return *this;
  }
  ~RetainedRuntimeTaskQueueOperation() { Reset(); }
  explicit operator bool() const noexcept { return state_ != nullptr; }
  void Reset() noexcept {
    if (auto* state = std::exchange(state_, nullptr)) state->ReleaseReference();
    guard_.Reset();
  }
  NativeTaskQueueResult Push(const NativeTaskQueueKey& key, agents::RuntimePermitGrant& credit) noexcept {
    return state_ ? state_->queue->Push(key, credit) : NativeTaskQueueResult{};
  }
  NativeTaskQueueResult Front() const noexcept {
    return state_ ? state_->queue->Front() : NativeTaskQueueResult{};
  }
  NativeTaskQueueResult Remove(const NativeTaskQueueKey& key) noexcept {
    // This operation itself keeps Drain pending until Reset, which performs
    // the necessary notification. No fallible post-effect lifetime lock or
    // queue -> lifetime edge is needed to deliver the removal result.
    return state_ ? state_->queue->Remove(key) : NativeTaskQueueResult{};
  }
  NativeTaskQueueResult Claim(const NativeTaskQueueClaimKey& key,
      agents::RuntimePermitGrant& provisional_worker, NativeTaskQueueClaim& output,
      const NativeTaskQueueClaimControl& control = {}) noexcept {
    // The operation guard retains queue storage through delivery; the resulting
    // worker slot has its own lifetime and may outlive this queue after drain.
    return state_ ? state_->queue->Claim(key, provisional_worker, output, control) : NativeTaskQueueResult{};
  }
 private:
  detail::RetainedTaskQueueState* state_ = nullptr;
  memory::SafeRetirementGuard guard_;
  friend class RetainedRuntimeTaskQueueOwner;
};

// SEARCH_KEY: RETAINED_RUNTIME_TASK_QUEUE_OWNER
// Shared domain/governor/issuer/PMR backing must outlive owner, operations and
// final domain collection. Owner calls themselves have external lifetime
// coordination; a local drain result does not join an external caller thread.
// Lock order: lifetime -> native queue -> governor -> metadata; domain Protect
// and Retire run under lifetime, never while holding a native queue predicate.
class RetainedRuntimeTaskQueueOwner {
 public:
  explicit RetainedRuntimeTaskQueueOwner(memory::MemorySafeRetirement& domain) : domain_(domain) {}
  RetainedRuntimeTaskQueueOwner(const RetainedRuntimeTaskQueueOwner&) = delete;
  RetainedRuntimeTaskQueueOwner& operator=(const RetainedRuntimeTaskQueueOwner&) = delete;
  ~RetainedRuntimeTaskQueueOwner() {
    if (state_) {
      std::lock_guard lock(state_->lifetime);
      if (!state_->drained || !state_->closed || !state_->fenced || state_->references ||
          state_->queue->Front().code != NativeTaskQueueCode::closed) std::terminate();
    }
    owner_guard_.Reset();
  }
  memory::SafeRetirementStatus Initialize(NativeTaskQueueBinding binding, RetainedTaskQueueLimits limits,
      agents::ResourceGovernanceReservationLedger& governor,
      memory::ReservationBackedPmrMemoryResource& metadata, memory::SafeRetirementHazard owner) {
    using S = memory::SafeRetirementStatus;
    if (state_ || !limits.operation_references) return S::invalid_request;
    auto made = domain_.Emplace<detail::RetainedTaskQueueState>(binding.queue_uuid.bytes,
        memory::SafeRetirementObjectKind::temporary_descriptor, binding, limits);
    if (!made.ok()) return made.status;
    memory::SafeRetirementGuard guard;
    const auto protected_state = domain_.Protect(made.handle, owner, guard);
    if (protected_state != S::ok) { domain_.Retire(made.handle); return protected_state; }
    auto* state = static_cast<detail::RetainedTaskQueueState*>(guard.get());
    try { state->queue.emplace(binding, governor, metadata); }
    catch (...) { domain_.Retire(made.handle); throw; }
    if (state->queue->BindStatus() != NativeTaskQueueCode::bound) {
      domain_.Retire(made.handle); return S::invalid_request;
    }
    state->handle = made.handle; state->closed = state->fenced = false;
    state_ = state; owner_guard_ = std::move(guard);
    return S::ok;
  }
  memory::SafeRetirementStatus AcquireOperation(Uuid queue, Uuid open, RuntimeGeneration generation,
      memory::SafeRetirementHazard hazard, RetainedRuntimeTaskQueueOperation& output) {
    using S = memory::SafeRetirementStatus;
    if (!state_ || output || queue != state_->binding.queue_uuid || open != state_->binding.open_generation_uuid ||
        generation != state_->binding.generation) return S::invalid_request;
    auto& state = *state_;
    std::lock_guard lock(state.lifetime);
    if (state.fenced) return S::closed;
    if (state.references == state.limits.operation_references) return S::exhausted;
    const auto status = domain_.Protect(state.handle, hazard, output.guard_);
    if (status != S::ok) return status;
    ++state.references; output.state_ = &state;
    return S::ok;
  }
  bool Close() {
    if (!state_) return false;
    auto& state = *state_;
    std::lock_guard lock(state.lifetime);
    if (state.queue->Close() != NativeTaskQueueCode::closed) return false;
    state.closed = true; state.NotifyLocked();
    return true;
  }
  bool FenceAdmission() {
    if (!state_) return false;
    auto& state = *state_;
    std::lock_guard lock(state.lifetime);
    if (!state.closed) return false;
    state.fenced = true;
    state.retired = domain_.Retire(state.handle) == memory::SafeRetirementStatus::ok;
    return state.retired;
  }
  // Sole lifecycle owner's explicit pending-entry cleanup after admission close.
  // Needed when no ordinary operation remains after fencing. This does not
  // classify task cancellation, authorize retry or reconcile durable effects.
  NativeTaskQueueResult RemovePending(const NativeTaskQueueKey& key) {
    if (!state_) return {};
    auto& state = *state_;
    std::lock_guard lock(state.lifetime);
    if (!state.closed) return {};
    const auto result = state.queue->Remove(key);
    if (result.code == NativeTaskQueueCode::removed) state.NotifyLocked();
    return result;
  }
  RetainedTaskQueueDrainResult Drain(std::chrono::steady_clock::time_point deadline,
                                    std::stop_token cancellation = {}) {
    using S = memory::SafeRetirementStatus;
    RetainedTaskQueueDrainResult result;
    if (!state_) return result;
    auto& state = *state_;
    // Registration and callback join occur outside the predicate lock. Owner
    // hazard remains held even when waiting fails or cancellation is selected.
    std::stop_callback wake(cancellation, [&state] {
      std::lock_guard lock(state.lifetime); state.NotifyLocked();
    });
    std::unique_lock lock(state.lifetime);
    if (!state.closed || !state.fenced || !state.retired) return result;
    for (;;) {
      const auto front = state.queue->Front();
      result.operation_references = state.references; result.pending = front.key;
      if (front.code != NativeTaskQueueCode::observed && front.code != NativeTaskQueueCode::closed) {
        result.status = S::wait_failed; return result;
      }
      if (!state.references && front.code == NativeTaskQueueCode::closed) {
        state.drained = true; result.status = S::ok; result.drained = true; return result;
      }
      if (cancellation.stop_requested()) { result.status = S::cancelled; return result; }
      if (std::chrono::steady_clock::now() >= deadline) { result.status = S::timed_out; return result; }
      try {
        if (!state.changed.Wait(lock, deadline)) { result.status = S::wait_failed; return result; }
      } catch (...) {
        if (!lock.owns_lock()) std::terminate();
        result.status = S::wait_failed; return result;
      }
    }
  }
  RetainedTaskQueueSnapshot Snapshot() const {
    if (!state_) return {};
    auto& state = *state_;
    std::lock_guard lock(state.lifetime);
    return {true, state.closed, state.fenced, state.drained, state.references, state.binding, state.queue->Front()};
  }
 private:
  memory::MemorySafeRetirement& domain_;
  memory::SafeRetirementGuard owner_guard_;
  detail::RetainedTaskQueueState* state_ = nullptr;
};
} // namespace scratchbird::core::runtime

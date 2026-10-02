// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "checked_mode_latch.hpp"
#include "held_latch_order.hpp"
#include "memory_safe_retirement.hpp"

namespace scratchbird::core::concurrency {

// Retained native mechanism, NOT an admitted reader/writer or intent descriptor.
// Its owning adapter must validate class/mode, current execution
// authority and admitted arbitration rank. No conversion or bypass is exposed.
// Declared class order is checked against actual retained mutex and mode grants
// on this native thread; no unconverted primitive or exception is covered.
// Binary instance/generation and exact request bindings prevent stale/mismatched
// use; they do not manufacture authority. Domain/resource outlive all wrappers.
using ModeLatchUuid = memory::MemoryBinaryUuid;
using ModeLatchNative = platform::CheckedModeLatch;
using ModeLatchClock = ModeLatchNative::Clock;
struct ModeLatchIdentity {
  ModeLatchUuid primitive{};
  std::uint64_t generation = 0;
  LatchClass latch_class = LatchClass::unspecified;
  bool operator==(const ModeLatchIdentity&) const = default;
};
struct ModeLatchLimits { std::uint32_t holders = 0, waiters = 0, operations = 0; };
struct ModeLatchRequest {
  ModeLatchIdentity identity;
  ModeLatchUuid task{}, request{};
  ModeLatchNative::Mode mode = ModeLatchNative::Mode::none;
  std::uint32_t admitted_rank = 0;
  bool operator==(const ModeLatchRequest&) const = default;
};
// Two distinct actual guards: one retains the latch, the other its separately
// governed nonmoving grant record. Record UUID is a fresh memory-object identity,
// not a replacement for the original durable request/transaction identity.
struct ModeLatchGrantMemory {
  ModeLatchUuid record{};
  memory::SafeRetirementHazard latch_hazard, record_hazard;
};
enum class ModeLatchCode {
  acquired, released, drained, invalid, wrong_owner, no_grant, busy, recursive, exhausted,
  closed, cancelled, timed_out, memory_failed, synchronization_failed, order_violation
};
struct ModeLatchResult {
  ModeLatchCode code = ModeLatchCode::invalid;
  memory::SafeRetirementStatus memory_status = memory::SafeRetirementStatus::ok;
  std::optional<LatchOrderConflict> order_conflict{};
};
struct ModeLatchSnapshot {
  ModeLatchIdentity identity;
  bool initialized = false, fenced = false, retirement_admitted = false;
  std::uint32_t operations = 0, grants = 0;
  ModeLatchNative::Observation native{};
};
namespace detail {
inline bool ModeHazardValid(const memory::SafeRetirementHazard& h) noexcept {
  return memory::MemorySystemUuidValid(h.hazard_id) && memory::MemorySystemUuidValid(h.owner_task) &&
      h.release_required_by >= memory::SafeRetirementBoundary::operation_completion &&
      h.release_required_by <= memory::SafeRetirementBoundary::runtime_shutdown;
}
inline ModeLatchCode ModeNativeCode(ModeLatchNative::Result result) noexcept {
  using N = ModeLatchNative::Result;
  using C = ModeLatchCode;
  switch (result) {
    case N::acquired: return C::acquired;
    case N::busy: return C::busy;
    case N::invalid: return C::invalid;
    case N::recursive: return C::recursive;
    case N::exhausted: return C::exhausted;
    case N::closed: return C::closed;
    case N::cancelled: return C::cancelled;
    case N::timed_out: return C::timed_out;
    case N::failed: return C::synchronization_failed;
  }
  std::terminate();
}
struct ModeLatchRecord { ModeLatchNative::Grant native; HeldLatchNode held; };
struct ModeLatchState {
  const ModeLatchIdentity identity;
  const ModeLatchLimits limits;
  memory::MemorySafeRetirement& domain;
  memory::SafeRetirementHandle handle;
  // lifetime -> native; never retain lifetime while parking in native Acquire.
  std::mutex lifetime;
  platform::CheckedCondition changed;
  ModeLatchNative native;
  bool fenced = true, retirement_admitted = false;
  std::uint32_t operations = 0, grants = 0;
  ModeLatchState(ModeLatchIdentity i, ModeLatchLimits l, memory::MemorySafeRetirement& d)
      : identity(i), limits(l), domain(d), native(l.holders, l.waiters) {}
  ~ModeLatchState() noexcept {
    if (!fenced || operations || grants) std::terminate();
  }
  void Notify() noexcept { if (!changed.NotifyAll()) std::terminate(); }
  void ReleaseOperation() noexcept {
    std::lock_guard lock(lifetime);
    if (!operations) std::terminate();
    --operations; Notify();
  }
};
} // namespace detail

class ModeLatchOperation;
class ModeLatchGrant {
 public:
  ModeLatchGrant() = default;
  ModeLatchGrant(const ModeLatchGrant&) = delete;
  ModeLatchGrant& operator=(const ModeLatchGrant&) = delete;
  ModeLatchGrant(ModeLatchGrant&& other) noexcept
      : state_(std::exchange(other.state_, nullptr)), record_(std::exchange(other.record_, nullptr)),
        state_guard_(std::move(other.state_guard_)), record_guard_(std::move(other.record_guard_)),
        request_(other.request_) {}
  ModeLatchGrant& operator=(ModeLatchGrant&& other) noexcept {
    if (this != &other) {
      Reset();
      state_ = std::exchange(other.state_, nullptr);
      record_ = std::exchange(other.record_, nullptr);
      state_guard_ = std::move(other.state_guard_);
      record_guard_ = std::move(other.record_guard_);
      request_ = other.request_;
    }
    return *this;
  }
  ~ModeLatchGrant() { Reset(); }
  explicit operator bool() const noexcept { return state_ != nullptr; }
  const ModeLatchRequest& request() const noexcept { return request_; }
  ModeLatchResult Release(const ModeLatchRequest& request) noexcept {
    using C = ModeLatchCode;
    if (!state_) return {C::no_grant};
    if (request != request_) return {C::wrong_owner};
    try {
      std::lock_guard lock(state_->lifetime);
      if (!state_->native.Release(record_->native, request_.task)) return {C::wrong_owner};
      if (!state_->grants) std::terminate();
      record_->held.Unlink();
      --state_->grants; state_->Notify();
      state_ = nullptr; record_ = nullptr;
    } catch (const std::system_error&) { return {C::synchronization_failed}; }
    // Neither guard is dropped until the intrusive node has been unlinked.
    record_guard_.Reset(); state_guard_.Reset();
    return {C::released};
  }
 private:
  friend class ModeLatchOperation;
  void Reset() noexcept {
    if (state_ && Release(request_).code != ModeLatchCode::released) std::terminate();
  }
  detail::ModeLatchState* state_ = nullptr;
  detail::ModeLatchRecord* record_ = nullptr;
  memory::SafeRetirementGuard state_guard_, record_guard_;
  ModeLatchRequest request_;
};

class ModeLatchOwner;
class ModeLatchOperation {
 public:
  ModeLatchOperation() = default;
  ModeLatchOperation(const ModeLatchOperation&) = delete;
  ModeLatchOperation& operator=(const ModeLatchOperation&) = delete;
  ModeLatchOperation(ModeLatchOperation&& other) noexcept
      : state_(std::exchange(other.state_, nullptr)), guard_(std::move(other.guard_)), task_(other.task_) {}
  ModeLatchOperation& operator=(ModeLatchOperation&& other) noexcept {
    if (this != &other) {
      Reset(); state_ = std::exchange(other.state_, nullptr);
      guard_ = std::move(other.guard_); task_ = other.task_;
    }
    return *this;
  }
  ~ModeLatchOperation() { Reset(); }
  explicit operator bool() const noexcept { return state_ != nullptr; }
  void Reset() noexcept {
    if (auto* state = std::exchange(state_, nullptr)) state->ReleaseOperation();
    guard_.Reset();
  }
  // The caller exclusively borrows operation/output for the complete call.
  // Prepare both actual guards before any native grant. Moves of the returned
  // wrapper never move the intrusive native record in governed storage.
  ModeLatchResult Acquire(const ModeLatchRequest& request, const ModeLatchGrantMemory& storage,
      ModeLatchGrant& output, std::optional<ModeLatchClock::time_point> deadline,
      std::stop_token stop = {}, bool immediate = false) {
    using C = ModeLatchCode;
    using S = memory::SafeRetirementStatus;
    if (!state_ || output || request.identity != state_->identity || request.task != task_ ||
        !memory::MemorySystemUuidValid(request.request) || !ModeLatchNative::Valid(request.mode) ||
        !memory::MemorySystemUuidValid(storage.record) ||
        !detail::ModeHazardValid(storage.latch_hazard) || !detail::ModeHazardValid(storage.record_hazard) ||
        storage.latch_hazard.owner_task != task_ || storage.record_hazard.owner_task != task_ ||
        storage.latch_hazard.hazard_id == storage.record_hazard.hazard_id) return {C::invalid};
    auto& s = *state_;
    if (const auto conflict = detail::HeldOrderConflict(s.identity.latch_class))
      return {C::order_violation,S::ok,conflict};
    if (detail::held_latches.count == std::numeric_limits<std::uint32_t>::max())
      return {C::exhausted};
    memory::SafeRetirementGuard state_guard, record_guard;
    try {
      {
        std::lock_guard lock(s.lifetime);
        if (auto terminal = s.native.Preflight(deadline, stop)) return {detail::ModeNativeCode(*terminal)};
        if (s.fenced) return {C::closed};
        const auto status = s.domain.Protect(s.handle, storage.latch_hazard, state_guard);
        if (status != S::ok) return {C::memory_failed, status};
      }
      const auto made = s.domain.Emplace<detail::ModeLatchRecord>(storage.record,
          memory::SafeRetirementObjectKind::temporary_descriptor);
      if (!made.ok()) return {C::memory_failed, made.status};
      const auto protected_record = s.domain.Protect(made.handle, storage.record_hazard, record_guard);
      // Immediately retire private storage: only this call/returned grant can
      // retain it. A failed preparation is therefore collectable, never leaked
      // as a published grant object. Existing guards remain valid after retire.
      const auto retired = s.domain.Retire(made.handle);
      // Domain Close may retire and collect the still-unprotected private
      // record between Emplace and Protect. A stale retirement handle is then
      // harmless cleanup after refusal, not a committed ownership failure.
      if (protected_record != S::ok) return {C::memory_failed, protected_record};
      if (retired != S::ok) std::terminate(); // A real guard forbids reclamation.
      auto* record = static_cast<detail::ModeLatchRecord*>(record_guard.get());
      // Memory preparation may enter another retained latch on this thread.
      // Recheck before either a grant or registration in the native wait queue.
      if (const auto conflict = detail::HeldOrderConflict(s.identity.latch_class))
        return {C::order_violation,S::ok,conflict};
      if (detail::held_latches.count == std::numeric_limits<std::uint32_t>::max())
        return {C::exhausted};
      const auto native = immediate
          ? s.native.TryAcquire(record->native, request.mode, deadline, stop, task_)
          : s.native.Acquire(record->native, request.mode, request.admitted_rank, deadline, stop, task_);
      const auto code = detail::ModeNativeCode(native);
      if (code == C::acquired) {
        // After native commitment, failure is not retryable. Both guards stay
        // live until the complete move-only ownership result is installed.
        try {
          std::lock_guard lock(s.lifetime);
          if (s.grants == s.limits.holders) std::terminate();
          record->held.Link({s.identity.primitive,s.identity.generation,s.identity.latch_class});
          ++s.grants;
          output.request_ = request;
          output.state_guard_ = std::move(state_guard);
          output.record_guard_ = std::move(record_guard);
          output.record_ = record; output.state_ = &s;
        } catch (...) { std::terminate(); }
      }
      return {code};
    } catch (const std::system_error&) { return {C::synchronization_failed}; }
  }
 private:
  friend class ModeLatchOwner;
  detail::ModeLatchState* state_ = nullptr;
  memory::SafeRetirementGuard guard_;
  ModeLatchUuid task_{};
};

// Single lifecycle owner; join all owner calls before destruction. Drain proves
// local reference/holder quiescence only, not physical collection, caller joins
// or an engine/node clean-shutdown receipt. The domain performs reclamation.
class ModeLatchOwner {
 public:
  explicit ModeLatchOwner(memory::MemorySafeRetirement& domain) : domain_(domain) {}
  ModeLatchOwner(const ModeLatchOwner&) = delete;
  ModeLatchOwner& operator=(const ModeLatchOwner&) = delete;
  ~ModeLatchOwner() {
    if (state_ && !drained_) std::terminate();
    guard_.Reset();
  }
  memory::SafeRetirementStatus Initialize(ModeLatchIdentity identity, ModeLatchLimits limits,
                                         memory::SafeRetirementHazard hazard) {
    using S = memory::SafeRetirementStatus;
    if (state_ || !memory::MemorySystemUuidValid(identity.primitive) || !limits.holders ||
        identity.latch_class < LatchClass::engine_lifecycle ||
        identity.latch_class > LatchClass::metrics_evidence ||
        !limits.operations || !detail::ModeHazardValid(hazard)) return S::invalid_request;
    const auto made = domain_.Emplace<detail::ModeLatchState>(identity.primitive,
        memory::SafeRetirementObjectKind::temporary_descriptor, identity, limits, domain_);
    if (!made.ok()) return made.status;
    const auto status = domain_.Protect(made.handle, hazard, guard_);
    if (status != S::ok) { domain_.Retire(made.handle); return status; }
    state_ = static_cast<detail::ModeLatchState*>(guard_.get());
    state_->handle = made.handle; state_->fenced = false;
    return S::ok;
  }
  memory::SafeRetirementStatus AcquireOperation(ModeLatchIdentity identity,
      memory::SafeRetirementHazard hazard, ModeLatchOperation& output) {
    using S = memory::SafeRetirementStatus;
    if (!state_ || output || identity != state_->identity || !detail::ModeHazardValid(hazard))
      return S::invalid_request;
    std::lock_guard lock(state_->lifetime);
    if (state_->fenced || state_->native.Observe().closed) return S::closed;
    if (state_->operations == state_->limits.operations) return S::exhausted;
    const auto status = domain_.Protect(state_->handle, hazard, output.guard_);
    if (status != S::ok) return status;
    ++state_->operations;
    output.state_ = state_; output.task_ = hazard.owner_task;
    return S::ok;
  }
  bool Close() {
    if (!state_) return false;
    std::lock_guard lock(state_->lifetime);
    state_->native.Close(); state_->Notify(); return true;
  }
  memory::SafeRetirementStatus FenceAdmission() {
    using S = memory::SafeRetirementStatus;
    if (!state_) return S::invalid_request;
    std::lock_guard lock(state_->lifetime);
    if (!state_->native.Observe().closed) return S::invalid_request;
    state_->fenced = true;
    const auto result = domain_.Retire(state_->handle);
    if (result == S::ok) state_->retirement_admitted = true;
    return result;
  }
  ModeLatchResult Drain(ModeLatchClock::time_point deadline, std::stop_token stop = {}) {
    using C = ModeLatchCode;
    if (!state_) return {C::invalid};
    auto& s = *state_;
    std::stop_callback wake(stop, [&s] { std::lock_guard lock(s.lifetime); s.Notify(); });
    std::unique_lock lock(s.lifetime);
    if (!s.fenced || !s.retirement_admitted) return {C::invalid};
    try {
      for (;;) {
        if (stop.stop_requested()) return {C::cancelled};
        if (!s.operations && !s.grants) {
          const auto native = s.native.Observe();
          if (!native.closed || native.holders || native.waiters || native.calls)
            return {C::synchronization_failed};
          drained_ = true; return {C::drained};
        }
        if (ModeLatchClock::now() >= deadline) return {C::timed_out};
        if (!s.changed.Wait(lock, deadline)) return {C::synchronization_failed};
      }
    } catch (const std::system_error&) {
      if (!lock.owns_lock()) std::terminate();
      return {C::synchronization_failed};
    }
  }
  ModeLatchSnapshot Snapshot() const {
    if (!state_) return {};
    std::lock_guard lock(state_->lifetime);
    return {state_->identity, true, state_->fenced, state_->retirement_admitted,
            state_->operations, state_->grants, state_->native.Observe()};
  }
 private:
  memory::MemorySafeRetirement& domain_;
  detail::ModeLatchState* state_ = nullptr;
  memory::SafeRetirementGuard guard_;
  bool drained_ = false;
};
} // namespace scratchbird::core::concurrency

// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "checked_fifo_mutex.hpp"
#include "memory_safe_retirement.hpp"
#include <system_error>

namespace scratchbird::core::concurrency {

// Internal retained mechanism, not current runtime/security/lock-order admission.
// The caller supplies admitted binary identities and bounded memory grants. This
// component implements ordinary FIFO only; it cannot authorize emergency bypass.
using MutexUuid = memory::MemoryBinaryUuid;
using MutexClock = platform::CheckedFifoMutex::Clock;
enum class MutexOwnerScope {
  engine, database, attachment, transaction, statement, page, index, catalog,
  cluster, task, temporary, metrics, evidence
};
enum class MutexOwnershipProfile { owned, process_global };
enum class MutexLatchMode {
  none, shared_read, intent_read, intent_write, exclusive_write, upgrade,
  cleanup, recovery, flush, eviction, publication, verification
};
struct MutexLatchDescriptor {
  MutexUuid primitive_id{};
  std::optional<MutexUuid> owner_uuid;
  MutexOwnerScope owner_scope = MutexOwnerScope::engine;
  std::uint64_t generation = 0;
  std::uint64_t creation_epoch = 0;
  MutexUuid last_transition{};
  MutexOwnershipProfile ownership_profile = MutexOwnershipProfile::owned;
};
struct MutexLatchLimits {
  std::uint32_t waiters = 0;
  std::uint32_t operation_references = 0;
};
struct MutexLatchRequest {
  MutexUuid primitive_id{};
  std::uint64_t generation = 0;
  MutexUuid task_id{};
  MutexUuid request_id{};
  MutexLatchMode mode = MutexLatchMode::exclusive_write;
  bool operator==(const MutexLatchRequest&) const = default;
};
enum class MutexLatchCode {
  acquired, released, drained, closed, cancelled, timed_out, busy, recursive,
  exhausted, invalid_request, invalid_mode, wrong_owner, no_grant,
  synchronization_failed, memory_failed
};
struct MutexLatchResult {
  MutexLatchCode code = MutexLatchCode::invalid_request;
  memory::SafeRetirementStatus memory_status = memory::SafeRetirementStatus::ok;
};
struct MutexLatchSnapshot {
  MutexLatchDescriptor descriptor;
  bool initialized = false;
  bool closed = false;
  bool admission_fenced = false;
  bool retirement_admitted = false;
  std::uint32_t operation_references = 0;
  std::uint32_t retained_grants = 0;
  platform::CheckedFifoMutex::Observation native{};
};
struct MutexLatchWaitSnapshot {
  MutexLatchSnapshot latch;
  platform::CheckedFifoMutex::WaitObservation wait;
};
struct MutexLatchWaitSetObservation {
  MutexUuid primitive_id{};
  std::uint64_t generation = 0;
  platform::CheckedFifoMutex::WaitObservation wait;
  std::size_t offset = 0;
};

namespace detail {
inline bool MutexHazardValid(const memory::SafeRetirementHazard& hazard) noexcept {
  return memory::MemorySystemUuidValid(hazard.hazard_id) &&
      memory::MemorySystemUuidValid(hazard.owner_task) &&
      hazard.release_required_by >= memory::SafeRetirementBoundary::operation_completion &&
      hazard.release_required_by <= memory::SafeRetirementBoundary::runtime_shutdown;
}
struct MutexLatchState {
  MutexLatchDescriptor descriptor;
  const MutexLatchLimits limits;
  memory::MemorySafeRetirement& domain;
  memory::SafeRetirementHandle handle;
  // Lock order is lifetime -> native. Never hold lifetime while native Lock
  // parks. The domain does not call into these mutexes with its lock held.
  std::mutex lifetime;
  platform::CheckedCondition changed;
  platform::CheckedFifoMutex native;
  bool closed = true;
  bool fenced = true;
  bool retirement_admitted = false;
  std::uint32_t references = 0;
  std::uint32_t grants = 0;
  MutexLatchState(MutexLatchDescriptor d, MutexLatchLimits l,
                  memory::MemorySafeRetirement& memory_domain)
      : descriptor(d), limits(l), domain(memory_domain), native(l.waiters) {}
  ~MutexLatchState() noexcept {
    if (!closed || !fenced || references || grants) std::terminate();
  }
  void Notify() noexcept { if (!changed.NotifyAll()) std::terminate(); }
  void ReleaseReference() noexcept {
    std::lock_guard lock(lifetime);
    if (!references) std::terminate();
    --references; Notify();
  }
};
inline MutexLatchCode MutexNativeCode(platform::CheckedFifoMutex::Result result) noexcept {
  using N = platform::CheckedFifoMutex::Result;
  using C = MutexLatchCode;
  switch (result) {
    case N::acquired: return C::acquired;
    case N::busy: return C::busy;
    case N::recursive: return C::recursive;
    case N::exhausted: return C::exhausted;
    case N::timed_out: return C::timed_out;
    case N::cancelled: return C::cancelled;
    case N::closed: return C::closed;
    case N::failed: return C::synchronization_failed;
  }
  std::terminate();
}
} // namespace detail

class MutexLatchOperation;
// Move-only retained ownership. Release must execute on the granting native
// thread with the exact request. A failed release preserves ownership and its
// memory guard. Concurrent moves/use are forbidden, as for unique_lock.
class MutexLatchGrant {
 public:
  MutexLatchGrant() = default;
  MutexLatchGrant(const MutexLatchGrant&) = delete;
  MutexLatchGrant& operator=(const MutexLatchGrant&) = delete;
  MutexLatchGrant(MutexLatchGrant&& other) noexcept
      : state_(std::exchange(other.state_, nullptr)), guard_(std::move(other.guard_)),
        request_(other.request_) {}
  MutexLatchGrant& operator=(MutexLatchGrant&& other) noexcept {
    if (this != &other) {
      Reset(); state_ = std::exchange(other.state_, nullptr);
      guard_ = std::move(other.guard_); request_ = other.request_;
    }
    return *this;
  }
  ~MutexLatchGrant() { Reset(); }
  explicit operator bool() const noexcept { return state_ != nullptr; }
  const MutexLatchRequest& request() const noexcept { return request_; }
  MutexLatchResult Release(const MutexLatchRequest& request) noexcept {
    using C = MutexLatchCode;
    if (!state_) return {C::no_grant};
    if (request != request_) return {C::wrong_owner};
    try {
      std::lock_guard lock(state_->lifetime);
      if (!state_->native.Unlock(request_.task_id)) return {C::wrong_owner};
      if (state_->grants != 1) std::terminate();
      --state_->grants; state_->Notify();
      state_ = nullptr;
    } catch (const std::system_error&) {
      // Native unlock can throw only before the release effect. A committed
      // notification failure terminates inside the mechanism, never retries.
      return {C::synchronization_failed};
    }
    guard_.Reset();
    return {C::released};
  }
 private:
  friend class MutexLatchOperation;
  void Reset() noexcept {
    if (state_ && Release(request_).code != MutexLatchCode::released)
      std::terminate(); // Before member destruction could drop a live guard.
  }
  detail::MutexLatchState* state_ = nullptr;
  memory::SafeRetirementGuard guard_;
  MutexLatchRequest request_;
};

class MutexLatchOwner;
class MutexLatchOperation {
 public:
  // All operations must remain alive and must not be moved/reset concurrently.
  // Their existing real guards retain every native mutex throughout capture.
  // Caller-owned frames/output are bounded borrowed scratch, not a registry or
  // a resource grant. Binary descriptor association is produced here, never
  // supplied independently of the retained object. No raw native pointer remains
  // in a used frame after return. This still grants no task-phase authority.
  static platform::CheckedFifoMutex::WaitSetResult SnapshotWaitSet(
      std::span<MutexLatchOperation* const> operations,
      std::span<platform::CheckedFifoMutex::WaitSetEntry> frames,
      std::span<MutexLatchWaitSetObservation> observations,
      std::span<platform::CheckedFifoMutex::WaiterObservation> waiters,
      std::uint32_t max_latches) {
    using N = platform::CheckedFifoMutex;
    using R = N::WaitSetResult;
    if (!max_latches) return R::invalid;
    if (operations.size()>max_latches || frames.size()<operations.size() ||
        observations.size()<operations.size()) return R::exhausted;
    for (std::size_t i=0;i<operations.size();++i) {
      if (!operations[i] || !operations[i]->state_ || frames[i].lock.owns_lock())
        return R::invalid;
      const auto& current=operations[i]->state_->descriptor;
      for (std::size_t j=0;j<i;++j) {
        const auto& prior=operations[j]->state_->descriptor;
        if (current.primitive_id==prior.primitive_id && current.generation==prior.generation)
          return R::invalid;
      }
    }
    const auto used=frames.first(operations.size());
    struct Clear {
      std::span<N::WaitSetEntry> frames;
      ~Clear() { for (auto& frame:frames) frame.mutex=nullptr; }
    } clear{used};
    for (std::size_t i=0;i<operations.size();++i)
      used[i].mutex=&operations[i]->state_->native;
    const auto result=N::ObserveWaitSet(used,waiters,max_latches);
    if (result==R::captured || result==R::insufficient_capacity)
      for (std::size_t i=0;i<operations.size();++i) {
        // Identity/generation are immutable for this retained object's lifetime;
        // mutable lifecycle fields are deliberately not copied outside their lock.
        const auto& d=operations[i]->state_->descriptor;
        observations[i]={d.primitive_id,d.generation,used[i].observation,used[i].offset};
      }
    return result;
  }
  MutexLatchOperation() = default;
  ~MutexLatchOperation() { Reset(); }
  MutexLatchOperation(const MutexLatchOperation&) = delete;
  MutexLatchOperation& operator=(const MutexLatchOperation&) = delete;
  MutexLatchOperation(MutexLatchOperation&& other) noexcept
      : state_(std::exchange(other.state_, nullptr)), guard_(std::move(other.guard_)),
        task_(other.task_) {}
  MutexLatchOperation& operator=(MutexLatchOperation&& other) noexcept {
    if (this != &other) {
      Reset(); state_ = std::exchange(other.state_, nullptr);
      guard_ = std::move(other.guard_); task_ = other.task_;
    }
    return *this;
  }
  explicit operator bool() const noexcept { return state_ != nullptr; }
  void Reset() noexcept {
    if (auto* state = std::exchange(state_, nullptr)) state->ReleaseReference();
    guard_.Reset();
  }
  // Keep the operation through the complete call, including cancellation
  // callback join and committed-result delivery. Prepare a real distinct guard
  // before native grant commitment; memory failure cannot hide a committed grant.
  MutexLatchResult Acquire(const MutexLatchRequest& request,
      memory::SafeRetirementHazard grant_hazard, MutexLatchGrant& output,
      std::optional<MutexClock::time_point> deadline, std::stop_token stop = {},
      bool immediate = false) {
    using C = MutexLatchCode;
    using S = memory::SafeRetirementStatus;
    if (!state_ || output || request.primitive_id != state_->descriptor.primitive_id ||
        request.generation != state_->descriptor.generation || request.task_id != task_ ||
        !memory::MemorySystemUuidValid(request.request_id) ||
        grant_hazard.owner_task != task_ || !detail::MutexHazardValid(grant_hazard))
      return {C::invalid_request};
    if (request.mode != MutexLatchMode::exclusive_write) return {C::invalid_mode};
    auto& s = *state_;
    memory::SafeRetirementGuard grant_guard;
    try {
      std::lock_guard lock(s.lifetime);
      if (const auto terminal = s.native.Preflight(deadline, stop, task_))
        return {detail::MutexNativeCode(*terminal)};
      if (s.fenced) return {C::closed};
      const auto protected_state = s.domain.Protect(s.handle, grant_hazard, grant_guard);
      if (protected_state != S::ok) return {C::memory_failed, protected_state};
    } catch (const std::system_error&) { return {C::synchronization_failed}; }
    platform::CheckedFifoMutex::Result native;
    try {
      native = immediate ? s.native.TryLock(deadline, stop, task_)
                         : s.native.Lock(deadline, stop, task_);
    } catch (const std::system_error&) { return {C::synchronization_failed}; }
    const auto code = detail::MutexNativeCode(native);
    if (code == C::acquired) {
      // Failure here is after commitment: fail fast with both guards live.
      try {
        std::lock_guard lock(s.lifetime);
        if (s.grants) std::terminate();
        ++s.grants;
        output.request_ = request;
        output.guard_ = std::move(grant_guard);
        output.state_ = &s;
      } catch (...) { std::terminate(); }
    }
    return {code};
  }
 private:
  friend class MutexLatchOwner;
  detail::MutexLatchState* state_ = nullptr;
  memory::SafeRetirementGuard guard_;
  MutexUuid task_{};
};

// Sole lifecycle owner. Domain/resource outlive this owner, all operations and
// grants. Drain is a local reference/holder witness, not external caller join or
// physical reclamation. All owner calls must be joined before destruction.
class MutexLatchOwner {
 public:
  explicit MutexLatchOwner(memory::MemorySafeRetirement& domain) : domain_(domain) {}
  MutexLatchOwner(const MutexLatchOwner&) = delete;
  MutexLatchOwner& operator=(const MutexLatchOwner&) = delete;
  ~MutexLatchOwner() {
    if (state_ && !drain_observed_) std::terminate();
    owner_guard_.Reset();
  }
  memory::SafeRetirementStatus Initialize(const MutexLatchDescriptor& d,
      MutexLatchLimits limits, memory::SafeRetirementHazard hazard) {
    using S = memory::SafeRetirementStatus;
    if (state_ || !memory::MemorySystemUuidValid(d.primitive_id) ||
        !memory::MemorySystemUuidValid(d.last_transition) ||
        (d.owner_uuid && !memory::MemorySystemUuidValid(*d.owner_uuid)) ||
        d.owner_scope < MutexOwnerScope::engine || d.owner_scope > MutexOwnerScope::evidence ||
        (d.ownership_profile != MutexOwnershipProfile::owned &&
         d.ownership_profile != MutexOwnershipProfile::process_global) ||
        (!d.owner_uuid && d.ownership_profile != MutexOwnershipProfile::process_global) ||
        (d.ownership_profile == MutexOwnershipProfile::process_global &&
         d.owner_scope != MutexOwnerScope::engine) ||
        !limits.waiters || !limits.operation_references || !detail::MutexHazardValid(hazard))
      return S::invalid_request;
    const auto made = domain_.Emplace<detail::MutexLatchState>(d.primitive_id,
        memory::SafeRetirementObjectKind::temporary_descriptor, d, limits, domain_);
    if (!made.ok()) return made.status;
    const auto protected_state = domain_.Protect(made.handle, hazard, owner_guard_);
    if (protected_state != S::ok) {
      domain_.Retire(made.handle); // Unpublished closed/fenced state remains safe.
      return protected_state;
    }
    state_ = static_cast<detail::MutexLatchState*>(owner_guard_.get());
    state_->handle = made.handle;
    state_->closed = false; state_->fenced = false;
    return S::ok;
  }
  memory::SafeRetirementStatus AcquireOperation(const MutexUuid& primitive,
      std::uint64_t generation, memory::SafeRetirementHazard hazard,
      MutexLatchOperation& output) {
    using S = memory::SafeRetirementStatus;
    if (!state_ || output || !detail::MutexHazardValid(hazard) ||
        primitive != state_->descriptor.primitive_id ||
        generation != state_->descriptor.generation) return S::invalid_request;
    std::lock_guard lock(state_->lifetime);
    if (state_->fenced) return S::closed;
    if (state_->references == state_->limits.operation_references) return S::exhausted;
    const auto protected_state = domain_.Protect(state_->handle, hazard, output.guard_);
    if (protected_state != S::ok) return protected_state;
    ++state_->references;
    output.state_ = state_; output.task_ = hazard.owner_task;
    return S::ok;
  }
  bool Close(const MutexUuid& transition) {
    if (!state_ || !memory::MemorySystemUuidValid(transition)) return false;
    std::lock_guard lock(state_->lifetime);
    state_->native.Close();
    if (!state_->closed) {
      state_->closed = true; state_->descriptor.last_transition = transition;
    }
    state_->Notify();
    return true;
  }
  memory::SafeRetirementStatus FenceAdmission() {
    using S = memory::SafeRetirementStatus;
    if (!state_) return S::invalid_request;
    std::lock_guard lock(state_->lifetime);
    if (!state_->closed) return S::invalid_request;
    state_->fenced = true;
    const auto result = domain_.Retire(state_->handle);
    if (result == S::ok) state_->retirement_admitted = true;
    return result;
  }
  MutexLatchResult Drain(MutexClock::time_point deadline, std::stop_token stop = {}) {
    using C = MutexLatchCode;
    if (!state_) return {C::invalid_request};
    auto& s = *state_;
    std::stop_callback wake(stop, [&s] {
      std::lock_guard lock(s.lifetime); s.Notify();
    });
    std::unique_lock lock(s.lifetime);
    if (!s.fenced || !s.retirement_admitted) return {C::invalid_request};
    try {
      for (;;) {
        if (stop.stop_requested()) return {C::cancelled};
        if (!s.references && !s.grants) {
          const auto native = s.native.Observe();
          if (native.held || native.waiters || native.calls || !native.closed)
            return {C::synchronization_failed};
          drain_observed_ = true;
          return {C::drained};
        }
        if (MutexClock::now() >= deadline) return {C::timed_out};
        if (!s.changed.Wait(lock, deadline)) return {C::synchronization_failed};
      }
    } catch (const std::system_error&) {
      if (!lock.owns_lock()) std::terminate();
      return {C::synchronization_failed};
    }
  }
  MutexLatchSnapshot Snapshot() const {
    if (!state_) return {};
    std::lock_guard lock(state_->lifetime);
    return {state_->descriptor, true, state_->closed, state_->fenced,
        state_->retirement_admitted, state_->references, state_->grants,
        state_->native.Observe()};
  }
  // The owner's real guard retains storage across both locks and the copy.
  // Native ownership may precede retained-grant delivery: neither count is a
  // substitute for the other. This is a protected single-latch observation,
  // not admission, a global graph snapshot, or permission to cancel/reclaim.
  MutexLatchWaitSnapshot SnapshotWaiters(
      std::span<platform::CheckedFifoMutex::WaiterObservation> output) const {
    if (!state_) return {};
    std::lock_guard lock(state_->lifetime);
    const auto wait=state_->native.ObserveWaiters(output);
    return {{state_->descriptor, true, state_->closed, state_->fenced,
        state_->retirement_admitted, state_->references, state_->grants, wait.state}, wait};
  }
 private:
  memory::MemorySafeRetirement& domain_;
  detail::MutexLatchState* state_ = nullptr;
  memory::SafeRetirementGuard owner_guard_;
  bool drain_observed_ = false;
};
} // namespace scratchbird::core::concurrency

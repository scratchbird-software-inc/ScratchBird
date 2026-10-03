// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "checked_fifo_mutex.hpp"
#include "held_latch_order.hpp"
#include "memory_safe_retirement.hpp"
#include <algorithm>
#include <limits>
#include <system_error>

namespace scratchbird::core::concurrency {

// Internal retained mechanism, not current runtime/security admission.
// The caller supplies admitted binary identities and bounded memory grants. This
// component implements ordinary FIFO only; it cannot authorize emergency bypass.
// Declared global latch classes are enforced against this thread's actual held
// retained grants. This does not cover unconverted primitives or authorize any
// ordering exception. Same-class ordering still needs the owning cycle policy.
using MutexUuid = memory::MemoryBinaryUuid;
using MutexClock = platform::CheckedFifoMutex::Clock;
enum class MutexOwnerScope {
  engine, database, attachment, transaction, statement, page, index, catalog,
  cluster, task, temporary, metrics, evidence
};
enum class MutexOwnershipProfile { owned, process_global };
using MutexLatchClass = LatchClass;
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
  MutexLatchClass latch_class = MutexLatchClass::unspecified;
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
  synchronization_failed, memory_failed, order_violation
};
using MutexLatchOrderConflict = LatchOrderConflict;
struct MutexLatchResult {
  MutexLatchCode code = MutexLatchCode::invalid_request;
  memory::SafeRetirementStatus memory_status = memory::SafeRetirementStatus::ok;
  std::optional<MutexLatchOrderConflict> order_conflict{};
  platform::CheckedFifoMutex::WaitEvidence wait{};
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
struct MutexLatchCycleEdge {
  std::size_t latch_index = 0;
  std::size_t waiter_index = 0;
  std::size_t next = 0;
  std::size_t visit = 0;
};
enum class MutexLatchCycleStatus { capture_failed, exhausted, invalid_observation, execution_binding_required, no_cycle_in_set, cycle };
struct MutexLatchCycleResult {
  MutexLatchCycleStatus status = MutexLatchCycleStatus::capture_failed;
  platform::CheckedFifoMutex::WaitSetResult capture = platform::CheckedFifoMutex::WaitSetResult::invalid;
  std::size_t edge_count = 0;
  std::size_t cycle_size = 0;
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
  HeldLatchNode held;
  MutexLatchState(MutexLatchDescriptor d, MutexLatchLimits l,
                  memory::MemorySafeRetirement& memory_domain)
      : descriptor(d), limits(l), domain(memory_domain), native(l.waiters) {}
  ~MutexLatchState() noexcept {
    if (!closed || !fenced || references || grants) std::terminate();
  }
  void LinkHeld() noexcept {
    held.Link({descriptor.primitive_id,descriptor.generation,descriptor.latch_class});
  }
  void UnlinkHeld() noexcept {
    held.Unlink();
  }
  void Notify() noexcept { if (!changed.NotifyAll()) std::terminate(); }
  void ReleaseReference() noexcept {
    std::lock_guard lock(lifetime);
    if (!references) std::terminate();
    --references; Notify();
  }
};
inline std::optional<MutexLatchOrderConflict> MutexOrderConflict(MutexLatchClass requested) noexcept {
  return HeldOrderConflict(requested);
}
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
      // The lifetime lock keeps a successor's commit from linking this same
      // payload until the prior owner has removed its node after native release.
      state_->UnlinkHeld();
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
  // Always capture from real guarded objects; never consume caller-asserted
  // graph edges. Scratch spans are exclusively borrowed, admitted by the caller.
  // Nodes bind BOTH native thread and binary task: a task with concurrent
  // executions must not create a false cycle by collapsing those executions.
  // Closed/cancelled/expired waits cannot contribute a continuing wait edge.
  // O(W^2) bounded edge linking, O(W) functional-graph walk, no allocation,
  // recursion or polling. cycle[0..cycle_size) indexes edges from this call.
  // This is a supplied-set observation, not global absence proof, current phase
  // eligibility, a victim choice, cancellation acknowledgement or resolution.
  // The owner MUST freshly revalidate graph/execution/phase before any action.
  static MutexLatchCycleResult CaptureWaitCycle(
      std::span<MutexLatchOperation* const> operations,
      std::span<platform::CheckedFifoMutex::WaitSetEntry> frames,
      std::span<MutexLatchWaitSetObservation> observations,
      std::span<platform::CheckedFifoMutex::WaiterObservation> waiters,
      std::span<MutexLatchCycleEdge> edges, std::span<std::size_t> cycle,
      std::uint32_t max_latches, std::uint32_t max_waiters) {
    using C = MutexLatchCycleStatus;
    using R = platform::CheckedFifoMutex::WaitSetResult;
    if (!max_waiters) return {};
    const auto capture=SnapshotWaitSet(operations,frames,observations,
        waiters.first(std::min<std::size_t>(waiters.size(),max_waiters)),max_latches);
    if (capture!=R::captured) return {C::capture_failed,capture};
    std::size_t count=0;
    for (std::size_t i=0;i<operations.size();++i) count+=observations[i].wait.state.waiters;
    if (edges.size()<count || cycle.size()<count) return {C::exhausted,capture};
    const auto now=MutexClock::now();
    std::size_t used=0;
    for (std::size_t i=0;i<operations.size();++i) {
      const auto& latch=observations[i]; const auto& owner=latch.wait;
      if (owner.state.held && (owner.holder==std::thread::id{} || !owner.owner ||
          !memory::MemorySystemUuidValid(*owner.owner))) return {C::invalid_observation,capture};
      for (std::size_t j=0;j<owner.state.waiters;++j) {
        const auto index=latch.offset+j; const auto& wait=waiters[index];
        if (wait.thread==std::thread::id{} || !wait.owner ||
            !memory::MemorySystemUuidValid(*wait.owner)) return {C::invalid_observation,capture};
        if (!owner.state.held || owner.state.closed || wait.cancellation_requested ||
            (wait.deadline && now>=*wait.deadline)) continue;
        for (std::size_t k=0;k<used;++k)
          if (waiters[edges[k].waiter_index].thread==wait.thread)
            return {C::invalid_observation,capture};
        edges[used++]={i,index,count,0};
      }
    }
    // Each native execution can have at most one blocking outgoing wait.
    // Missing successors terminate a path; no unobserved edge is manufactured.
    for (std::size_t i=0;i<used;++i) {
      const auto& owner=observations[edges[i].latch_index].wait;
      for (std::size_t j=0;j<used;++j) {
        const auto& wait=waiters[edges[j].waiter_index];
        if (owner.holder==wait.thread) {
          // A native thread reused/rebound across different tasks is not proof
          // of either a bound cycle or its absence. Its owner must resolve the
          // execution binding; do not splice tasks or silently drop this edge.
          if (owner.owner!=wait.owner) return {C::execution_binding_required,capture,used,0};
          edges[i].next=j; break;
        }
      }
    }
    for (std::size_t start=0;start<used;++start) {
      if (edges[start].visit) continue;
      const auto mark=start+1;
      auto current=start;
      while (current<used && !edges[current].visit) {
        edges[current].visit=mark; current=edges[current].next;
      }
      if (current>=used || edges[current].visit!=mark) continue;
      const auto first=current;
      std::size_t length=0;
      do { cycle[length++]=current; current=edges[current].next; } while (current!=first);
      return {C::cycle,capture,used,length};
    }
    return {C::no_cycle_in_set,capture,used,0};
  }
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
    if (const auto conflict=detail::MutexOrderConflict(s.descriptor.latch_class))
      return {C::order_violation,S::ok,conflict};
    if (detail::held_latches.count==std::numeric_limits<std::uint32_t>::max())
      return {C::exhausted};
    memory::SafeRetirementGuard grant_guard;
    try {
      std::lock_guard lock(s.lifetime);
      if (const auto terminal = s.native.Preflight(deadline, stop, task_))
        return {detail::MutexNativeCode(*terminal)};
      if (s.fenced) return {C::closed};
      const auto protected_state = s.domain.Protect(s.handle, grant_hazard, grant_guard);
      if (protected_state != S::ok) return {C::memory_failed, protected_state};
    } catch (const std::system_error&) { return {C::synchronization_failed}; }
    // Memory preparation must not create an unchecked path if a future owning
    // allocator enters another retained latch before returning to this thread.
    if (const auto conflict=detail::MutexOrderConflict(s.descriptor.latch_class))
      return {C::order_violation,S::ok,conflict};
    if (detail::held_latches.count==std::numeric_limits<std::uint32_t>::max())
      return {C::exhausted};
    platform::CheckedFifoMutex::Result native;
    platform::CheckedFifoMutex::WaitEvidence wait;
    try {
      native = immediate ? s.native.TryLock(deadline, stop, task_, &wait)
                         : s.native.Lock(deadline, stop, task_, &wait);
    } catch (const std::system_error&) { return {C::synchronization_failed}; }
    const auto code = detail::MutexNativeCode(native);
    if (code == C::acquired) {
      // Failure here is after commitment: fail fast with both guards live.
      try {
        std::lock_guard lock(s.lifetime);
        if (s.grants) std::terminate();
        s.LinkHeld();
        ++s.grants;
        output.request_ = request;
        output.guard_ = std::move(grant_guard);
        output.state_ = &s;
      } catch (...) { std::terminate(); }
    }
    return {code, S::ok, {}, wait};
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
        d.latch_class<MutexLatchClass::engine_lifecycle ||
        d.latch_class>MutexLatchClass::metrics_evidence ||
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

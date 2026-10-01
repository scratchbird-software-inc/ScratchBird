// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "memory_safe_retirement.hpp"
#include "checked_condition.hpp"
#include "canonical_diagnostic_catalog.hpp"

#include <algorithm>
#include <optional>
#include <string_view>
#include <system_error>
#if defined(__linux__)
#include <cerrno>
#include <pthread.h>
#include <time.h>
#endif

namespace scratchbird::core::concurrency {

// SEARCH_KEY: SB_RETAINED_CONDITION_WAIT
// Internal synchronization only. The owning runtime admits the descriptor,
// resource profile, task and epoch; this component does not issue that authority.
using WaitUuid = memory::MemoryBinaryUuid;
using WaitClock = std::chrono::steady_clock;
enum class WaitOwnerScope {
  engine, database, attachment, transaction, statement, page, index, catalog,
  cluster, task, temporary, metrics, evidence
};
enum class WaitOutcome { satisfied, closed, cancelled, timed_out, failed };
enum class WaitOwnershipProfile { owned, process_global };
enum class WaitFailure { none, invalid_descriptor, fail_safe_release, timeout };

struct ConditionWaitDescriptor {
  WaitUuid primitive_id{};
  WaitUuid predicate_mutex_id{};
  std::optional<WaitUuid> owner_uuid;
  WaitOwnerScope owner_scope = WaitOwnerScope::engine;
  std::uint64_t generation = 0;
  std::uint64_t creation_epoch = 0;
  WaitUuid last_transition{};
  WaitOwnershipProfile ownership_profile = WaitOwnershipProfile::owned;
};
struct ConditionWaitLimits {
  std::uint32_t waiters = 0;
  std::uint32_t operation_references = 0;
};
// Allocation-free internal diagnostic content. Binary identities are owned;
// registration strings have immutable catalog lifetime. Not an emitted occurrence,
// disclosure grant, public rendering, retry admission or task-completion receipt.
struct ConditionWaitDiagnostic {
  diagnostics::DiagnosticCodeDefinition registration;
  std::optional<WaitUuid> primitive_id;
  std::string_view primitive_class = "condition_wait";
  std::optional<WaitOwnerScope> owner_scope;
  std::optional<WaitUuid> thread_or_task_id;
  std::string_view requested_mode = "none";
  std::string_view held_modes_summary = "none"; // Condition waits grant no latch modes.
  std::uint64_t wait_duration_us = 0;
  std::string_view required_action;
  bool protected_data = true;
};
struct ConditionWaitResult {
  WaitOutcome outcome = WaitOutcome::failed;
  WaitFailure failure = WaitFailure::invalid_descriptor;
  std::optional<WaitUuid> primitive_id;
  std::optional<WaitUuid> task_id;
  std::uint64_t wait_duration_us = 0;
  std::string_view required_action = "repair_descriptor";
  std::array<char, 96> uninterruptible_reason{};
  std::uint8_t reason_size = 0;
  std::optional<WaitOwnerScope> owner_scope;
  std::string_view reason() const noexcept { return {uninterruptible_reason.data(), reason_size}; }
  std::string_view code() const noexcept {
    switch (failure) {
      case WaitFailure::none: return {};
      case WaitFailure::invalid_descriptor:
        return "diag.mga.concurrency.invalid_primitive_descriptor";
      case WaitFailure::fail_safe_release:
        return "diag.mga.concurrency.fail_safe_release";
      case WaitFailure::timeout: return "diag.mga.concurrency.latch_timeout";
    }
    return "diag.mga.concurrency.invalid_primitive_descriptor";
  }
  std::optional<ConditionWaitDiagnostic> Diagnostic() const noexcept {
    if (failure == WaitFailure::none) return std::nullopt;
    const auto* registration = diagnostics::FindCanonicalDiagnosticCode(code());
    if (!registration) return std::nullopt; // Never invent missing catalog authority.
    return ConditionWaitDiagnostic{*registration, primitive_id, "condition_wait", owner_scope,
        task_id, "none", "none", wait_duration_us, required_action, true};
  }
};
struct ConditionWaitSnapshot {
  ConditionWaitDescriptor descriptor;
  bool closed = false;
  bool admission_fenced = false;
  std::uint32_t waiter_count = 0;
  std::uint32_t operation_ref_count = 0;
  // Local evidence only, not activated metric series or drain authority.
  std::uint64_t registered_waits = 0;
  std::uint64_t selected_timeouts = 0;
  bool initialized = false;
};

namespace detail {
using PredicateCondition = platform::CheckedCondition;
struct ConditionWaitState {
  ConditionWaitDescriptor descriptor;
  const ConditionWaitLimits limits;
  std::mutex predicate;
  PredicateCondition changed;
  // Lock order when both are needed: predicate -> lifetime. No memory-domain
  // lock is held while acquiring either mutex. Native waits release their mutex.
  std::mutex lifetime;
  PredicateCondition drained;
  // Unpublished construction is already safe to retire if owner protection
  // cannot be acquired. Only Initialize's retained owner opens the instance.
  bool closed = true;                  // predicate
  bool fenced = true;                  // lifetime
  std::uint32_t waiters = 0;            // predicate
  std::uint32_t references = 0;         // lifetime; excludes sole owner guard
  std::uint64_t registered_waits = 0;   // predicate
  std::uint64_t selected_timeouts = 0;  // predicate

  ConditionWaitState(ConditionWaitDescriptor d, ConditionWaitLimits l)
      : descriptor(d), limits(l) {}
  ~ConditionWaitState() noexcept {
    if (!closed || !fenced || waiters || references) std::terminate();
  }
  void ReleaseReference() noexcept {
    std::lock_guard lock(lifetime);
    if (references == 0) std::terminate();
    --references;
    if (!drained.NotifyAll()) std::terminate();
  }
  struct CancellationWake {
    ConditionWaitState* state;
    void operator()() const noexcept {
      // Failure to acquire a native mutex cannot be represented as a normal
      // wait return owning that mutex. Fail-fast preserves reachable storage.
      std::lock_guard lock(state->predicate);
      if (!state->changed.NotifyAll()) std::terminate();
    }
  };
};
} // namespace detail

class ConditionWaitOwner;
class ConditionWaitOperation {
 public:
  ConditionWaitOperation() = default;
  ~ConditionWaitOperation() { Reset(); }
  ConditionWaitOperation(const ConditionWaitOperation&) = delete;
  ConditionWaitOperation& operator=(const ConditionWaitOperation&) = delete;
  ConditionWaitOperation(ConditionWaitOperation&& other) noexcept
      : state_(std::exchange(other.state_, nullptr)), guard_(std::move(other.guard_)),
        task_(other.task_) {}
  ConditionWaitOperation& operator=(ConditionWaitOperation&& other) noexcept {
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
  // Retain this operation and its owner until all associated lock guards and
  // calls have ended. Like unique_lock, an operation is not concurrently movable.
  std::unique_lock<std::mutex> LockPredicate() {
    return state_ ? std::unique_lock(state_->predicate) : std::unique_lock<std::mutex>{};
  }
  bool NotifyAll() const noexcept {
    return state_ && state_->changed.NotifyAll();
  }

  template <class Predicate>
  ConditionWaitResult Wait(std::unique_lock<std::mutex>& lock, Predicate&& predicate,
      std::stop_token cancellation, std::optional<WaitClock::time_point> deadline,
      std::string_view uninterruptible_reason = {}) {
    ConditionWaitResult result;
    if (!state_) return result;
    result.task_id = task_;
    auto& s = *state_;
    result.primitive_id = s.descriptor.primitive_id;
    result.owner_scope = s.descriptor.owner_scope;
    if (!lock.owns_lock() || lock.mutex() != &s.predicate ||
        (!cancellation.stop_possible() && (!deadline || uninterruptible_reason.empty())) ||
        uninterruptible_reason.size() > result.uninterruptible_reason.size() ||
        uninterruptible_reason.find('\0') != std::string_view::npos)
      return result;
    std::copy(uninterruptible_reason.begin(), uninterruptible_reason.end(),
        result.uninterruptible_reason.begin());
    result.reason_size = static_cast<std::uint8_t>(uninterruptible_reason.size());
    const auto select = [&]() -> bool {
      result.failure = WaitFailure::none; result.required_action = {};
      if (s.closed) result.outcome = WaitOutcome::closed;
      else if (cancellation.stop_requested()) result.outcome = WaitOutcome::cancelled;
      else if (predicate()) result.outcome = WaitOutcome::satisfied;
      else if (deadline && WaitClock::now() >= *deadline) {
        result.outcome = WaitOutcome::timed_out; result.failure = WaitFailure::timeout;
        result.required_action = "abort_wait_preserve_operation_outcome";
      } else return false;
      return true;
    };
    const auto failed = [&]() {
      result.outcome = WaitOutcome::failed; result.failure = WaitFailure::fail_safe_release;
      result.required_action = "abort_wait_preserve_operation_outcome";
    };
    const auto count_timeout = [&]() {
      if (result.outcome == WaitOutcome::timed_out &&
          s.selected_timeouts != std::numeric_limits<std::uint64_t>::max())
        ++s.selected_timeouts;
    };
    try {
      if (select()) { count_timeout(); return result; }
    } catch (...) { failed(); return result; }
    // Counts are admission state. Refuse saturation before any registration.
    const auto refused = [&]() {
      result.outcome = WaitOutcome::failed; result.failure = WaitFailure::invalid_descriptor;
      result.required_action = "repair_descriptor";
      return result;
    };
    if (s.waiters == s.limits.waiters ||
        s.registered_waits == std::numeric_limits<std::uint64_t>::max())
      return refused();
    bool callback_reference = false;
    if (cancellation.stop_possible()) {
      std::lock_guard lifetime(s.lifetime);
      if (s.references == s.limits.operation_references)
        return refused();
      ++s.references; callback_reference = true;
    }
    ++s.waiters; ++s.registered_waits;
    const auto began = WaitClock::now();
    std::optional<std::stop_callback<detail::ConditionWaitState::CancellationWake>> wake;
    // An already-requested cancellation invokes inline. Never register while
    // holding the predicate mutex. Registration remains visible during this gap;
    // publication cannot be lost because selection runs again before parking.
    if (callback_reference) {
      lock.unlock();
      wake.emplace(cancellation, detail::ConditionWaitState::CancellationWake{&s});
      try { lock.lock(); } catch (...) { std::terminate(); }
    }
    try {
      while (!select()) {
        if (!s.changed.Wait(lock, deadline)) { failed(); break; }
      }
    } catch (...) { failed(); }
    --s.waiters;
    count_timeout();
    result.wait_duration_us = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(WaitClock::now() - began).count());
    // Result is immutable after selection. Joining a callback while holding the
    // predicate mutex would deadlock. The counted callback reference remains
    // live through unregister; the operation's memory guard retains all storage.
    if (callback_reference) {
      lock.unlock(); wake.reset(); s.ReleaseReference();
      try { lock.lock(); } catch (...) { std::terminate(); }
    }
    return result;
  }

 private:
  friend class ConditionWaitOwner;
  detail::ConditionWaitState* state_ = nullptr;
  memory::SafeRetirementGuard guard_;
  WaitUuid task_{};
};

// Sole lifecycle owner; external callers must keep it alive through all calls.
// No hidden allocation/reclamation service: payload and retained references use
// the supplied governed memory domain. Domain/resource outlive this owner.
class ConditionWaitOwner {
 public:
  explicit ConditionWaitOwner(memory::MemorySafeRetirement& domain) : domain_(domain) {}
  ConditionWaitOwner(const ConditionWaitOwner&) = delete;
  ConditionWaitOwner& operator=(const ConditionWaitOwner&) = delete;
  ~ConditionWaitOwner() {
    if (state_ && !drain_observed_) std::terminate();
    owner_guard_.Reset();
    // Collection remains with the owning memory service, including retry of
    // physical release failures. Destructor never forces a charged release.
  }
  memory::SafeRetirementStatus Initialize(const ConditionWaitDescriptor& descriptor,
      ConditionWaitLimits limits, memory::SafeRetirementHazard owner_hazard) {
    using S = memory::SafeRetirementStatus;
    if (state_ || !memory::MemorySystemUuidValid(descriptor.primitive_id) ||
        !memory::MemorySystemUuidValid(descriptor.predicate_mutex_id) ||
        (descriptor.owner_uuid && !memory::MemorySystemUuidValid(*descriptor.owner_uuid)) ||
        (descriptor.ownership_profile != WaitOwnershipProfile::owned &&
         descriptor.ownership_profile != WaitOwnershipProfile::process_global) ||
        (descriptor.ownership_profile == WaitOwnershipProfile::process_global &&
         descriptor.owner_scope != WaitOwnerScope::engine) ||
        (!descriptor.owner_uuid && descriptor.ownership_profile != WaitOwnershipProfile::process_global) ||
        !memory::MemorySystemUuidValid(descriptor.last_transition) ||
        descriptor.primitive_id == descriptor.predicate_mutex_id ||
        descriptor.owner_scope < WaitOwnerScope::engine || descriptor.owner_scope > WaitOwnerScope::evidence ||
        !limits.waiters || !limits.operation_references ||
        !memory::MemorySystemUuidValid(owner_hazard.hazard_id) ||
        !memory::MemorySystemUuidValid(owner_hazard.owner_task) ||
        owner_hazard.release_required_by < memory::SafeRetirementBoundary::operation_completion ||
        owner_hazard.release_required_by > memory::SafeRetirementBoundary::runtime_shutdown)
      return S::invalid_request;
    const auto made = domain_.Emplace<detail::ConditionWaitState>(descriptor.primitive_id,
        memory::SafeRetirementObjectKind::temporary_descriptor, descriptor, limits);
    if (!made.ok()) return made.status;
    handle_ = made.handle;
    const auto protected_state = domain_.Protect(handle_, owner_hazard, owner_guard_);
    if (protected_state != S::ok) {
      // Unpublished state stays closed/fenced, safe for the memory owner to
      // collect even if its reader table filled or the domain closed meanwhile.
      domain_.Retire(handle_);
      return protected_state;
    }
    state_ = static_cast<detail::ConditionWaitState*>(owner_guard_.get());
    owner_task_ = owner_hazard.owner_task;
    state_->closed = false; state_->fenced = false;
    return S::ok;
  }
  memory::SafeRetirementStatus Acquire(const WaitUuid& primitive, std::uint64_t generation,
      memory::SafeRetirementHazard hazard, ConditionWaitOperation& output) {
    using S = memory::SafeRetirementStatus;
    if (!state_ || output || primitive != state_->descriptor.primitive_id ||
        generation != state_->descriptor.generation) return S::invalid_request;
    auto& s = *state_;
    // Count even an in-flight protection attempt, so fence/drain cannot pass
    // between reference admission and its memory-service guard acquisition.
    std::lock_guard lifetime(s.lifetime);
    if (s.fenced) return S::closed;
    if (s.references == s.limits.operation_references) return S::exhausted;
    ++s.references;
    const auto protected_state = domain_.Protect(handle_, hazard, output.guard_);
    if (protected_state != S::ok) {
      --s.references;
      if (!s.drained.NotifyAll()) std::terminate();
      return protected_state;
    }
    output.state_ = state_; output.task_ = hazard.owner_task;
    return S::ok;
  }
  bool Close(const WaitUuid& transition) {
    if (!state_ || !memory::MemorySystemUuidValid(transition)) return false;
    std::lock_guard predicate(state_->predicate);
    if (!state_->closed) {
      state_->closed = true; state_->descriptor.last_transition = transition;
    }
    return state_->changed.NotifyAll();
  }
  bool FenceAdmission() {
    if (!state_) return false;
    std::lock_guard predicate(state_->predicate);
    if (!state_->closed) return false;
    std::lock_guard lifetime(state_->lifetime);
    state_->fenced = true;
    return domain_.Retire(handle_) == memory::SafeRetirementStatus::ok;
  }
  ConditionWaitResult Drain(WaitClock::time_point deadline, std::stop_token cancellation = {}) {
    ConditionWaitResult result;
    result.failure = WaitFailure::fail_safe_release;
    result.required_action = "retain_storage_complete_drain";
    if (!state_) return result;
    auto& s = *state_;
    result.primitive_id = s.descriptor.primitive_id;
    result.owner_scope = s.descriptor.owner_scope;
    result.task_id = owner_task_;
    // The lifecycle owner, not a counted operation, retains this observer and
    // its callback. All owner calls must return before owner destruction.
    // Register outside lifetime: already-requested stop invokes inline.
    std::stop_callback wake(cancellation, [&s] {
      std::lock_guard lifetime(s.lifetime);
      if (!s.drained.NotifyAll()) std::terminate();
    });
    std::unique_lock lifetime(s.lifetime);
    if (!s.fenced) return result;
    std::optional<WaitClock::time_point> began;
    const auto finish = [&]() {
      if (began) result.wait_duration_us = static_cast<std::uint64_t>(
          std::chrono::duration_cast<std::chrono::microseconds>(WaitClock::now() - *began).count());
      return result;
    };
    try {
      while (s.references != 0 && !cancellation.stop_requested()) {
        if (WaitClock::now() >= deadline) return finish();
        // Measure only a drain that actually attempts to park, through its
        // terminal selection. Spurious wakes do not restart this interval.
        if (!began) began = WaitClock::now();
        if (!s.drained.Wait(lifetime, deadline)) return finish();
      }
    } catch (...) {
      // An error is not a wake or drain receipt. Failed native reacquisition
      // cannot return normally while claiming ownership of the lifetime mutex.
      if (!lifetime.owns_lock()) std::terminate();
      return finish();
    }
    if (cancellation.stop_requested()) { result.outcome = WaitOutcome::cancelled; return finish(); }
    drain_observed_ = true;
    result.outcome = WaitOutcome::closed; result.failure = WaitFailure::none;
    result.required_action = {};
    return finish();
  }
  ConditionWaitSnapshot Snapshot() const {
    if (!state_) return {};
    std::lock_guard predicate(state_->predicate);
    std::lock_guard lifetime(state_->lifetime);
    return {state_->descriptor, state_->closed, state_->fenced, state_->waiters,
        state_->references, state_->registered_waits, state_->selected_timeouts, true};
  }
 private:
  memory::MemorySafeRetirement& domain_;
  memory::SafeRetirementHandle handle_;
  memory::SafeRetirementGuard owner_guard_;
  detail::ConditionWaitState* state_ = nullptr;
  bool drain_observed_ = false;
  WaitUuid owner_task_{};
};

} // namespace scratchbird::core::concurrency

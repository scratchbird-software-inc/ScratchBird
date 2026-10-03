// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "resource_governance_admission.hpp"
#include "retained_condition_wait.hpp"

namespace scratchbird::core::concurrency {
// Runtime-profile semaphore over the actual node governor. Slot ownership is
// not byte admission, security authority, transaction finality or clean shutdown.
struct RuntimeSemaphoreDescriptor {
  platform::Uuid primitive_id;
  std::optional<platform::Uuid> owner_uuid;
  WaitOwnerScope owner_scope = WaitOwnerScope::engine;
  std::uint64_t generation = 0, creation_epoch = 0;
  platform::Uuid last_transition;
  WaitOwnershipProfile ownership_profile = WaitOwnershipProfile::owned;
};
struct RuntimeSemaphoreLimits { std::uint32_t operation_references = 0; };
struct RuntimeSemaphoreSnapshot {
  RuntimeSemaphoreDescriptor descriptor;
  agents::RuntimePermitInstanceSnapshot governor;
  std::uint32_t operation_ref_count = 0;
  bool admission_fenced = false;
  bool initialized = false;
  std::uint64_t registered_waits = 0, selected_timeouts = 0;
  // Protected occurrence content, not an emitted MessageVector or disclosure
  // grant. The failure slot is part of the actual governed primitive payload.
  std::optional<ConditionWaitDiagnostic> last_failure;
  std::uint64_t fail_safe_releases = 0;
};
namespace detail {
inline std::optional<ConditionWaitDiagnostic> SemaphoreDiagnostic(
    agents::RuntimePermitCode code, std::optional<memory::MemoryBinaryUuid> primitive,
    std::optional<memory::MemoryBinaryUuid> task, std::optional<WaitOwnerScope> scope,
    std::uint64_t duration = 0, bool incomplete_drain = false) noexcept {
  using C = agents::RuntimePermitCode;
  std::string_view name, action;
  if (incomplete_drain) {
    name = "diag.mga.concurrency.fail_safe_release";
    action = "retain_storage_complete_drain";
  } else switch (code) {
    case C::granted: case C::released: case C::closed: case C::cancelled:
    case C::exhausted: return std::nullopt; // Actual quota refusal, not malformed primitive.
    case C::timed_out:
      name = "diag.mga.concurrency.latch_timeout";
      action = "abort_wait_preserve_operation_outcome"; break;
    case C::allocation_failed: case C::identity_failed: case C::synchronization_failed:
      name = "diag.mga.concurrency.fail_safe_release";
      action = "retain_storage_complete_actual_resource_release"; break;
    case C::sequence_exhausted:
      name = "diag.mga.concurrency.fail_safe_release";
      action = "retain_ownership_coordinate_fresh_incarnation_restart"; break;
    default:
      name = "diag.mga.concurrency.invalid_primitive_descriptor";
      action = "repair_descriptor"; break;
  }
  const auto* registration = diagnostics::FindCanonicalDiagnosticCode(name);
  if (!registration) return std::nullopt;
  return ConditionWaitDiagnostic{*registration,primitive,"semaphore_budget",scope,
      task,"none","none",duration,action,true};
}
struct RuntimeSemaphoreState {
  RuntimeSemaphoreDescriptor descriptor;
  const RuntimeSemaphoreLimits limits;
  memory::MemorySafeRetirement& domain;
  agents::ResourceGovernanceReservationLedger& governor;
  const agents::RuntimePermitAuthority authority;
  const agents::RuntimePermitProfile profile;
  const agents::RuntimePermitInstanceBinding instance;
  memory::SafeRetirementHandle handle;
  // lifetime -> governor -> governed metadata. No governor callback acquires
  // lifetime. A blocking governor wait never retains lifetime.
  std::mutex lifetime;
  platform::CheckedCondition changed;
  bool fenced = true, closed = true, retirement_admitted = false;
  std::uint32_t references = 0, retained_grants = 0;
  std::uint64_t registered_waits = 0, selected_timeouts = 0;
  memory::MemoryBinaryUuid owner_task{};
  std::optional<ConditionWaitDiagnostic> last_failure;
  std::uint64_t fail_safe_releases = 0;
  RuntimeSemaphoreState(RuntimeSemaphoreDescriptor d, RuntimeSemaphoreLimits l,
      memory::MemorySafeRetirement& memory, agents::ResourceGovernanceReservationLedger& g,
      agents::RuntimePermitAuthority a, agents::RuntimePermitProfile p)
      : descriptor(d), limits(l), domain(memory), governor(g), authority(a), profile(p),
        instance{d.primitive_id,d.generation} {}
  ~RuntimeSemaphoreState() noexcept {
    if (!closed || !fenced || references || retained_grants) std::terminate();
  }
  void RecordFailureLocked(std::optional<ConditionWaitDiagnostic> diagnostic) noexcept {
    if (!diagnostic) return;
    last_failure = diagnostic;
    if (diagnostic->registration.code == "diag.mga.concurrency.fail_safe_release" &&
        fail_safe_releases != UINT64_MAX) ++fail_safe_releases;
  }
  void Notify() noexcept { if (!changed.NotifyAll()) std::terminate(); }
  void ReleaseReference() noexcept {
    std::lock_guard lock(lifetime);
    if (!references) std::terminate();
    --references; Notify();
  }
};
}
class RuntimeSemaphoreOwner;
class RuntimeSemaphoreOperation;
// Release/destruction requires actual resource quiescence; RAII does not stop
// a live worker or remove a retained queue payload. Moves are caller-serialized.
class RuntimeSemaphoreGrant {
 public:
  RuntimeSemaphoreGrant() = default;
  RuntimeSemaphoreGrant(const RuntimeSemaphoreGrant&) = delete;
  RuntimeSemaphoreGrant& operator=(const RuntimeSemaphoreGrant&) = delete;
  RuntimeSemaphoreGrant(RuntimeSemaphoreGrant&& other) noexcept
      : state_(std::exchange(other.state_,nullptr)), permit_(std::move(other.permit_)),
        guard_(std::move(other.guard_)), primitive_(other.primitive_), task_(other.task_),
        scope_(other.scope_), last_code_(other.last_code_) {}
  RuntimeSemaphoreGrant& operator=(RuntimeSemaphoreGrant&& other) noexcept {
    if (this != &other) {
      Reset(); state_ = std::exchange(other.state_,nullptr);
      permit_ = std::move(other.permit_); guard_ = std::move(other.guard_);
      primitive_ = other.primitive_; task_ = other.task_; scope_ = other.scope_; last_code_ = other.last_code_;
    }
    return *this;
  }
  ~RuntimeSemaphoreGrant() { Reset(); }
  explicit operator bool() const noexcept { return state_ != nullptr; }
  const agents::RuntimePermitView* view() const noexcept { return permit_.view(); }
  bool QuiescenceRequested() const { return permit_.QuiescenceRequested(); }
  std::optional<ConditionWaitDiagnostic> Diagnostic() const noexcept {
    return detail::SemaphoreDiagnostic(last_code_,primitive_,task_,scope_);
  }
  // Caller first quiesces the actual queue entry/execution. A failed explicit
  // release keeps both the exact native capability and actual memory guard.
  agents::RuntimePermitCode Release(const agents::RuntimePermitRequest& expected) noexcept {
    using C = agents::RuntimePermitCode;
    if (!state_) { last_code_ = C::no_grant; return last_code_; }
    auto* state = state_;
    {
      std::unique_lock lock(state->lifetime,std::defer_lock);
      try { lock.lock(); }
      catch (const std::system_error&) {
        // No state was touched. Retain the real capability and memory guard;
        // the owning caller can inspect Diagnostic() and retry after recovery.
        // A mutex we could not acquire cannot protect an occurrence-slot write.
        last_code_ = C::synchronization_failed; return last_code_;
      }
      last_code_ = permit_.Release(expected);
      if (last_code_ != C::released) { state->RecordFailureLocked(Diagnostic()); return last_code_; }
      if (!state->retained_grants) std::terminate();
      --state->retained_grants;
      state_ = nullptr; state->Notify();
    }
    guard_.Reset();
    return C::released;
  }
 private:
  void Reset() noexcept {
    if (state_) {
      const auto expected = permit_.view()->binding;
      if (Release(expected) != agents::RuntimePermitCode::released) std::terminate();
    }
  }
  detail::RuntimeSemaphoreState* state_ = nullptr;
  agents::RuntimePermitGrant permit_;
  memory::SafeRetirementGuard guard_;
  // Immutable context remains available after a release/move for diagnostics;
  // these copied identities never own a grant or retain primitive storage.
  std::optional<memory::MemoryBinaryUuid> primitive_, task_;
  std::optional<WaitOwnerScope> scope_;
  agents::RuntimePermitCode last_code_ = agents::RuntimePermitCode::no_grant;
  friend class RuntimeSemaphoreOperation;
};
struct RuntimeSemaphoreAcquireResult {
  agents::RuntimePermitCode code = agents::RuntimePermitCode::invalid_binding;
  memory::SafeRetirementStatus memory_status = memory::SafeRetirementStatus::ok;
  RuntimeSemaphoreGrant permit;
  bool registered = false;
  std::uint64_t wait_duration_us = 0;
  std::array<char,96> uninterruptible_reason{};
  std::uint8_t reason_size = 0;
  std::optional<memory::MemoryBinaryUuid> primitive_id, task_id;
  std::optional<WaitOwnerScope> owner_scope;
  bool ok() const noexcept { return code == agents::RuntimePermitCode::granted && bool(permit); }
  std::string_view reason() const noexcept { return {uninterruptible_reason.data(),reason_size}; }
  std::optional<ConditionWaitDiagnostic> Diagnostic() const noexcept {
    return detail::SemaphoreDiagnostic(code,primitive_id,task_id,owner_scope,wait_duration_us);
  }
};
struct RuntimeSemaphoreDrainResult : agents::RuntimePermitDrainResult {
  std::optional<ConditionWaitDiagnostic> diagnostic;
  std::optional<ConditionWaitDiagnostic> Diagnostic() const noexcept { return diagnostic; }
};
class RuntimeSemaphoreOperation {
 public:
  RuntimeSemaphoreOperation() = default;
  RuntimeSemaphoreOperation(const RuntimeSemaphoreOperation&) = delete;
  RuntimeSemaphoreOperation& operator=(const RuntimeSemaphoreOperation&) = delete;
  RuntimeSemaphoreOperation(RuntimeSemaphoreOperation&& other) noexcept
      : state_(std::exchange(other.state_,nullptr)), guard_(std::move(other.guard_)), task_(other.task_) {}
  RuntimeSemaphoreOperation& operator=(RuntimeSemaphoreOperation&& other) noexcept {
    if (this != &other) {
      Reset(); state_ = std::exchange(other.state_,nullptr);
      guard_ = std::move(other.guard_); task_ = other.task_;
    }
    return *this;
  }
  ~RuntimeSemaphoreOperation() { Reset(); }
  explicit operator bool() const noexcept { return state_ != nullptr; }
  void Reset() noexcept {
    if (auto* s = std::exchange(state_,nullptr)) s->ReleaseReference();
    guard_.Reset();
  }
  // Operation/capability calls and moves are caller-serialized. Retain the
  // operation until result delivery finishes; the returned grant may outlive it.
  RuntimeSemaphoreAcquireResult Acquire(const agents::RuntimePermitRequest& request,
      memory::SafeRetirementHazard grant_hazard, agents::RuntimePermitAcquireControl control,
      std::string_view reason = {}) noexcept {
    return AcquireImpl(request,grant_hazard,control,reason,true);
  }
  // One actual governor attempt, never waiter registration/parking or a new
  // private budget. Optional terminal controls still precede grant commitment.
  RuntimeSemaphoreAcquireResult TryAcquire(const agents::RuntimePermitRequest& request,
      memory::SafeRetirementHazard grant_hazard,
      agents::RuntimePermitAcquireControl control = {}) noexcept {
    return AcquireImpl(request,grant_hazard,control,{},false);
  }
 private:
  RuntimeSemaphoreAcquireResult AcquireImpl(const agents::RuntimePermitRequest& request,
      memory::SafeRetirementHazard grant_hazard, agents::RuntimePermitAcquireControl control,
      std::string_view reason, bool waiting) noexcept {
    using C = agents::RuntimePermitCode;
    RuntimeSemaphoreAcquireResult result;
    if (!state_) return result;
    auto& s = *state_;
    result.primitive_id = s.descriptor.primitive_id.bytes;
    result.task_id = task_; result.owner_scope = s.descriptor.owner_scope;
    if (request.task.bytes != task_ || grant_hazard.owner_task != task_ ||
        !memory::MemorySystemUuidValid(grant_hazard.hazard_id) ||
        grant_hazard.release_required_by < memory::SafeRetirementBoundary::operation_completion ||
        grant_hazard.release_required_by > memory::SafeRetirementBoundary::runtime_shutdown ||
        request.quantity != 1 || !memory::MemorySystemUuidValid(request.attempt.bytes) ||
        (s.profile == agents::RuntimePermitProfile::worker_slot ?
          (!request.worker || !memory::MemorySystemUuidValid(request.worker->bytes)) : request.worker.has_value()) ||
        !(request.authority == s.authority) || request.profile != s.profile ||
        request.semaphore != s.instance.semaphore || request.semaphore_generation != s.instance.generation) {
      std::lock_guard lock(s.lifetime);
      s.RecordFailureLocked(result.Diagnostic());
      return result;
    }
    memory::SafeRetirementGuard grant_guard;
    const bool callback_reference = waiting && control.cancellation.stop_possible();
    {
      std::lock_guard lock(s.lifetime);
      const auto instance = s.governor.InspectRuntimePermitInstance(s.authority,s.profile,s.instance);
      if (s.fenced || instance.closed) { result.code = C::closed; return result; }
      if (instance.code != C::bound) {
        result.code = instance.code; s.RecordFailureLocked(result.Diagnostic()); return result;
      }
      if (callback_reference && s.references == s.limits.operation_references) {
        result.memory_status = memory::SafeRetirementStatus::exhausted;
        s.RecordFailureLocked(result.Diagnostic()); return result;
      }
      // Callback protection covers registration through join; the operation's
      // original reference also retains construction and delivery of the result.
      if (callback_reference) ++s.references;
      result.memory_status = s.domain.Protect(s.handle,grant_hazard,grant_guard);
      if (result.memory_status != memory::SafeRetirementStatus::ok) {
        if (callback_reference) --s.references;
        s.Notify(); result.code = C::allocation_failed;
        s.RecordFailureLocked(result.Diagnostic()); return result;
      }
    }
    agents::RuntimePermitWaitResult native;
    if (waiting) native = s.governor.WaitAcquireRuntimePermit(request,control,reason);
    else native.admission = s.governor.AcquireRuntimePermit(request,control);
    result.code = native.admission.code; result.registered = native.registered;
    result.wait_duration_us = native.wait_duration_us;
    result.uninterruptible_reason = native.uninterruptible_reason;
    result.reason_size = native.reason_size;
    {
      std::lock_guard lock(s.lifetime);
      if (native.admission.ok()) {
        // Native quantity/capacity bound this count. Releases serialize the
        // native credit and local retained-grant decrement under lifetime.
        if (s.retained_grants == UINT32_MAX) std::terminate();
        ++s.retained_grants;
        result.permit.state_ = &s;
        result.permit.permit_ = std::move(native.admission.permit);
        result.permit.guard_ = std::move(grant_guard);
        result.permit.primitive_ = result.primitive_id;
        result.permit.task_ = result.task_id; result.permit.scope_ = result.owner_scope;
        result.permit.last_code_ = C::granted;
      }
      if (native.registered && s.registered_waits != UINT64_MAX) ++s.registered_waits;
      if (native.admission.code == C::timed_out && s.selected_timeouts != UINT64_MAX) ++s.selected_timeouts;
      s.RecordFailureLocked(result.Diagnostic());
      if (callback_reference) --s.references;
      s.Notify();
    }
    return result;
  }
 private:
  detail::RuntimeSemaphoreState* state_ = nullptr;
  memory::SafeRetirementGuard guard_;
  memory::MemoryBinaryUuid task_{};
  friend class RuntimeSemaphoreOwner;
};

// Sole lifecycle owner. Domain, governor, issuer and actual governed backing
// outlive this owner and all grants. All owner calls must finish before owner
// destruction; a local drain receipt is not an external thread join.
class RuntimeSemaphoreOwner {
 public:
  explicit RuntimeSemaphoreOwner(memory::MemorySafeRetirement& domain) : domain_(domain) {}
  RuntimeSemaphoreOwner(const RuntimeSemaphoreOwner&) = delete;
  RuntimeSemaphoreOwner& operator=(const RuntimeSemaphoreOwner&) = delete;
  ~RuntimeSemaphoreOwner() {
    if (state_) {
      bool unsafe;
      {
        std::lock_guard lock(state_->lifetime);
        unsafe = !drain_observed_ || !state_->closed || !state_->fenced ||
                 state_->references || state_->retained_grants;
        if (unsafe) state_->RecordFailureLocked(detail::SemaphoreDiagnostic(
            agents::RuntimePermitCode::synchronization_failed,state_->descriptor.primitive_id.bytes,
            state_->owner_task,state_->descriptor.owner_scope,0,true));
      }
      // Record protected content before fail-fast. Never release the owner
      // guard or force resource credit because diagnostics cannot be delivered.
      if (unsafe) std::terminate();
    }
    owner_guard_.Reset();
  }
  memory::SafeRetirementStatus Initialize(RuntimeSemaphoreDescriptor descriptor,
      RuntimeSemaphoreLimits limits, agents::ResourceGovernanceReservationLedger& governor,
      agents::RuntimePermitAuthority authority, agents::RuntimePermitProfile profile,
      memory::SafeRetirementHazard owner_hazard) {
    using S = memory::SafeRetirementStatus;
    if (state_ || !limits.operation_references || !descriptor.generation ||
        !memory::MemorySystemUuidValid(descriptor.primitive_id.bytes) ||
        !memory::MemorySystemUuidValid(descriptor.last_transition.bytes) ||
        (descriptor.owner_uuid && !memory::MemorySystemUuidValid(descriptor.owner_uuid->bytes)) ||
        (descriptor.ownership_profile != WaitOwnershipProfile::owned &&
         descriptor.ownership_profile != WaitOwnershipProfile::process_global) ||
        (descriptor.ownership_profile == WaitOwnershipProfile::process_global && descriptor.owner_scope != WaitOwnerScope::engine) ||
        (!descriptor.owner_uuid && descriptor.ownership_profile != WaitOwnershipProfile::process_global) ||
        descriptor.owner_scope < WaitOwnerScope::engine || descriptor.owner_scope > WaitOwnerScope::evidence ||
        !memory::MemorySystemUuidValid(owner_hazard.hazard_id) ||
        !memory::MemorySystemUuidValid(owner_hazard.owner_task) ||
        owner_hazard.release_required_by < memory::SafeRetirementBoundary::operation_completion ||
        owner_hazard.release_required_by > memory::SafeRetirementBoundary::runtime_shutdown)
      return S::invalid_request;
    const auto instance = governor.InspectRuntimePermitInstance(authority,profile,
        {descriptor.primitive_id,descriptor.generation});
    if (instance.code != agents::RuntimePermitCode::bound) return S::invalid_request;
    auto made = domain_.Emplace<detail::RuntimeSemaphoreState>(descriptor.primitive_id.bytes,
        memory::SafeRetirementObjectKind::temporary_descriptor,descriptor,limits,domain_,governor,authority,profile);
    if (!made.ok()) return made.status;
    const auto protected_state = domain_.Protect(made.handle,owner_hazard,owner_guard_);
    if (protected_state != S::ok) { domain_.Retire(made.handle); return protected_state; }
    state_ = static_cast<detail::RuntimeSemaphoreState*>(owner_guard_.get());
    state_->owner_task = owner_hazard.owner_task;
    state_->handle = made.handle; state_->fenced = state_->closed = false;
    return S::ok;
  }
  memory::SafeRetirementStatus AcquireOperation(platform::Uuid primitive, std::uint64_t generation,
      memory::SafeRetirementHazard hazard, RuntimeSemaphoreOperation& output) {
    using S = memory::SafeRetirementStatus;
    if (!state_ || output || primitive != state_->descriptor.primitive_id || generation != state_->descriptor.generation)
      return S::invalid_request;
    auto& s = *state_;
    std::lock_guard lock(s.lifetime);
    if (s.fenced) return S::closed;
    if (s.references == s.limits.operation_references) return S::exhausted;
    ++s.references;
    const auto status = domain_.Protect(s.handle,hazard,output.guard_);
    if (status != S::ok) { --s.references; s.Notify(); return status; }
    output.state_ = &s; output.task_ = hazard.owner_task;
    return S::ok;
  }
  bool Close(platform::Uuid transition) {
    if (!state_ || !memory::MemorySystemUuidValid(transition.bytes)) return false;
    auto& s = *state_;
    std::lock_guard lock(s.lifetime);
    if (s.governor.CloseRuntimePermitInstance(s.authority,s.profile,s.instance) != agents::RuntimePermitCode::closed)
      return false;
    if (!s.closed) { s.closed = true; s.descriptor.last_transition = transition; }
    s.Notify(); return true;
  }
  bool FenceAdmission() {
    if (!state_) return false;
    auto& s = *state_;
    std::lock_guard lock(s.lifetime);
    if (!s.closed) return false;
    s.fenced = true;
    s.retirement_admitted = domain_.Retire(s.handle) == memory::SafeRetirementStatus::ok;
    return s.retirement_admitted;
  }
  RuntimeSemaphoreDrainResult Drain(WaitClock::time_point deadline, std::stop_token cancellation = {}) {
    RuntimeSemaphoreDrainResult result;
    constexpr std::string_view reason = "retain semaphore storage until actual grant and call drain";
    std::copy(reason.begin(),reason.end(),result.uninterruptible_reason.begin());
    result.reason_size = static_cast<std::uint8_t>(reason.size());
    if (!state_) return result;
    auto& s = *state_;
    std::optional<WaitClock::time_point> began;
    const auto finish = [&](bool lifetime_locked = false) {
      if (began) result.wait_duration_us = static_cast<std::uint64_t>(
          std::chrono::duration_cast<std::chrono::microseconds>(WaitClock::now() - *began).count());
      if (!result.drained) {
        result.diagnostic = detail::SemaphoreDiagnostic(result.code,s.descriptor.primitive_id.bytes,
            s.owner_task,s.descriptor.owner_scope,result.wait_duration_us,true);
        if (lifetime_locked) s.RecordFailureLocked(result.diagnostic);
        else { std::lock_guard lock(s.lifetime); s.RecordFailureLocked(result.diagnostic); }
      }
      return result;
    };
    std::stop_callback wake(cancellation,[&s] {
      std::lock_guard lock(s.lifetime); s.Notify();
    });
    {
      std::unique_lock lock(s.lifetime);
      if (!s.fenced || !s.retirement_admitted) return finish(true);
      while (s.references || s.retained_grants) {
        if (cancellation.stop_requested()) { result.code = agents::RuntimePermitCode::cancelled; return finish(true); }
        if (WaitClock::now() >= deadline) { result.code = agents::RuntimePermitCode::timed_out; return finish(true); }
        if (!began) began = WaitClock::now();
        try {
          if (!s.changed.Wait(lock,deadline)) { result.code = agents::RuntimePermitCode::synchronization_failed; return finish(true); }
        } catch (...) {
          if (!lock.owns_lock()) std::terminate();
          result.code = agents::RuntimePermitCode::synchronization_failed; return finish(true);
        }
      }
    }
    static_cast<agents::RuntimePermitDrainResult&>(result) = s.governor.DrainRuntimePermitInstance(s.authority,s.profile,s.instance,
        {cancellation,deadline},reason);
    if (result.drained) { std::lock_guard lock(s.lifetime); drain_observed_ = true; }
    return finish();
  }
  RuntimeSemaphoreSnapshot Snapshot() const {
    if (!state_) return {};
    auto& s = *state_;
    std::lock_guard lock(s.lifetime);
    return {s.descriptor,s.governor.InspectRuntimePermitInstance(s.authority,s.profile,s.instance),
        s.references,s.fenced,true,s.registered_waits,s.selected_timeouts,s.last_failure,s.fail_safe_releases};
  }
 private:
  memory::MemorySafeRetirement& domain_;
  memory::SafeRetirementGuard owner_guard_;
  detail::RuntimeSemaphoreState* state_ = nullptr;
  bool drain_observed_ = false;
};
} // namespace scratchbird::core::concurrency

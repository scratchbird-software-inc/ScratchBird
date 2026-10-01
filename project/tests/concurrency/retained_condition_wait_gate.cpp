// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "retained_condition_wait.hpp"

#include <atomic>
#include <future>
#include <iostream>
#include <latch>
#include <semaphore>
#include <stdexcept>
#include <thread>
#if defined(__unix__)
#include <sys/wait.h>
#include <unistd.h>
#endif

#if defined(SB_WAIT_NATIVE_FAULT_GATE)
thread_local bool fail_next_native_wait = false;
thread_local unsigned native_timed_waits = 0;
struct NativePause {
  std::binary_semaphore reached{0};
  std::binary_semaphore resume{0};
  pthread_mutex_t* mutex = nullptr;
};
thread_local NativePause* pause_before_park = nullptr;
thread_local NativePause* pause_after_unlock = nullptr;
thread_local NativePause* observe_mutex_attempt = nullptr;
extern "C" int __real_pthread_mutex_lock(pthread_mutex_t*);
extern "C" int __real_pthread_mutex_unlock(pthread_mutex_t*);
extern "C" int __wrap_pthread_mutex_lock(pthread_mutex_t* mutex) {
  if (observe_mutex_attempt && observe_mutex_attempt->mutex == mutex) {
    auto* probe = std::exchange(observe_mutex_attempt, nullptr);
    probe->reached.release();
  }
  return __real_pthread_mutex_lock(mutex);
}
extern "C" int __wrap_pthread_mutex_unlock(pthread_mutex_t* mutex) {
  auto* probe = pause_after_unlock && pause_after_unlock->mutex == mutex
      ? std::exchange(pause_after_unlock, nullptr) : nullptr;
  const int error = __real_pthread_mutex_unlock(mutex);
  if (probe) { probe->reached.release(); probe->resume.acquire(); }
  return error;
}
void ParkPause(pthread_mutex_t* mutex) {
  if (auto* probe = std::exchange(pause_before_park, nullptr)) {
    probe->mutex = mutex;
    probe->reached.release(); probe->resume.acquire();
  }
}
extern "C" int __real_pthread_cond_wait(pthread_cond_t*, pthread_mutex_t*);
extern "C" int __real_pthread_cond_timedwait(pthread_cond_t*, pthread_mutex_t*, const timespec*);
#if defined(_GLIBCXX_USE_PTHREAD_COND_CLOCKWAIT)
extern "C" int __real_pthread_cond_clockwait(pthread_cond_t*, pthread_mutex_t*, clockid_t, const timespec*);
extern "C" int __wrap_pthread_cond_clockwait(pthread_cond_t* c, pthread_mutex_t* m, clockid_t clock, const timespec* t) {
  ++native_timed_waits;
  if (std::exchange(fail_next_native_wait, false)) return EINVAL;
  ParkPause(m);
  return __real_pthread_cond_clockwait(c, m, clock, t);
}
#endif
extern "C" int __wrap_pthread_cond_wait(pthread_cond_t* c, pthread_mutex_t* m) {
  if (std::exchange(fail_next_native_wait, false)) return EINVAL;
  ParkPause(m);
  return __real_pthread_cond_wait(c, m);
}
extern "C" int __wrap_pthread_cond_timedwait(pthread_cond_t* c, pthread_mutex_t* m, const timespec* t) {
  ++native_timed_waits;
  if (std::exchange(fail_next_native_wait, false)) return EINVAL;
  ParkPause(m);
  return __real_pthread_cond_timedwait(c, m, t);
}
#endif

namespace {
namespace m = scratchbird::core::memory;
namespace c = scratchbird::core::concurrency;
using S = m::SafeRetirementStatus;
using O = c::WaitOutcome;
using namespace std::chrono_literals;
std::atomic<unsigned> checks{0};
void Check(bool condition, const char* why) {
  ++checks;
  if (!condition) throw std::runtime_error(why);
}
m::MemoryBinaryUuid Id(unsigned n) {
  m::MemoryBinaryUuid id{};
  id[6] = 0x70; id[8] = 0x80;
  id[14] = static_cast<unsigned char>(n >> 8); id[15] = static_cast<unsigned char>(n);
  return id;
}
m::SafeRetirementHazard Hazard(unsigned n) { return {Id(n), Id(99)}; }
c::ConditionWaitDescriptor Descriptor() {
  return {Id(20), Id(21), Id(22), c::WaitOwnerScope::database, 7, 11, Id(23)};
}
struct Fixture {
  m::MemoryManager manager;
  m::HierarchicalMemoryBudgetLedger ledger{3, 5};
  std::unique_ptr<m::ReservationBackedMemoryResource> resource;
  static auto Policy() {
    auto p = m::DefaultLocalEngineMemoryPolicy();
    p.hard_limit_bytes = p.per_context_limit_bytes = 131072;
    return p;
  }
  Fixture() : manager(Policy()) {
    m::ReservationBackedMemoryResourceRequest r;
    r.memory_manager = &manager; r.reservation_ledger = &ledger;
    r.consumer_kind = m::ReservationBackedMemoryConsumerKind::background_maintenance;
    r.requested_bytes = 65536; r.category = m::MemoryCategory::core_runtime;
    r.route_label = "runtime.condition_wait.conformance"; r.purpose = "governed wait lifetime";
    r.binary_operation_uuid = Id(1);
    r.binary_ownership[m::MemoryBinaryScopeKind::context] = Id(2);
    r.binary_ownership[m::MemoryBinaryScopeKind::owner] = Id(3);
    r.binary_ownership[m::MemoryBinaryScopeKind::database] = Id(4);
    r.scope_chain = {{m::HierarchicalMemoryScopeKind::process, {}, Id(5)},
                    {m::HierarchicalMemoryScopeKind::database, {}, Id(4)}};
    r.provenance.source = m::HierarchicalMemoryBudgetProvenanceSource::server_runtime_api;
    r.provenance.source_label = "condition wait component fixture";
    for (const auto& scope : r.scope_chain) {
      m::HierarchicalMemoryBudget budget;
      budget.scope = scope; budget.hard_limit_bytes = r.requested_bytes; budget.provenance = r.provenance;
      Check(ledger.SetBudget(budget).ok(), "actual parent budget");
    }
    auto acquired = m::AcquireReservationBackedMemoryResource(std::move(r));
    Check(acquired.ok(), "actual governed reservation"); resource = std::move(acquired.resource);
  }
  void Empty() {
    Check(manager.Snapshot().current_bytes == 0, "all native object/metadata allocations freed");
    Check(resource->ReleaseNoAlloc().ok(), "release actual reservation");
    Check(ledger.Snapshot().current_bytes == 0 && manager.Snapshot().reserved_capacity_bytes == 0,
          "all memory charges gone");
  }
};
c::ConditionWaitOperation Acquire(c::ConditionWaitOwner& owner, unsigned hazard) {
  c::ConditionWaitOperation op;
  Check(owner.Acquire(Id(20), 7, Hazard(hazard), op) == S::ok, "admit actual retained operation");
  return op;
}
void Finish(c::ConditionWaitOwner& owner) {
  Check(owner.Close(Id(24)) && owner.FenceAdmission(), "close then fence");
  Check(owner.Drain(c::WaitClock::now() + 5s).failure == c::WaitFailure::none, "actual reference drain");
}
template<class Function>
void Run(Function function, c::ConditionWaitLimits limits = {32, 64}) {
  Fixture f;
  {
    m::MemorySafeRetirement domain(*f.resource, Id(10), 4, 64);
    Check(domain.Initialize() == S::ok, "governed domain initialize");
    const auto metadata = f.manager.Snapshot().current_bytes;
    {
      c::ConditionWaitOwner owner(domain);
      Check(owner.Initialize(Descriptor(), limits, Hazard(30)) == S::ok, "publish wait object");
      Check(f.manager.Snapshot().current_bytes > metadata, "wait object physically charged");
      try { function(owner, domain); }
      catch (...) { Finish(owner); throw; }
      Finish(owner);
    }
    Check(domain.Collect() == S::ok && domain.Snapshot().retired == 0,
          "owning memory service reclaims closed drained wait");
  }
  f.Empty();
}

void EntryAndValidation() {
  Run([](auto& owner, auto&) {
    auto op = Acquire(owner, 31);
    std::mutex other; std::unique_lock wrong(other);
    auto result = op.Wait(wrong, [] { return true; }, {}, c::WaitClock::now(), "bounded test");
    Check(result.failure == c::WaitFailure::invalid_descriptor && wrong.owns_lock(), "wrong mutex rejected");
    wrong.unlock();
    auto lock = op.LockPredicate();
    result = op.Wait(lock, [] { return true; }, {}, std::nullopt, "reason");
    Check(result.failure == c::WaitFailure::invalid_descriptor, "uncancellable infinite wait rejected");
    result = op.Wait(lock, [] { return true; }, {}, c::WaitClock::now());
    Check(result.failure == c::WaitFailure::invalid_descriptor, "uncancellable reason required");
    op.NotifyAll();
    result = op.Wait(lock, [] { return false; }, {}, c::WaitClock::now(), "bounded test");
    Check(result.outcome == O::timed_out && result.code() == "diag.mga.concurrency.latch_timeout",
          "notification is not a retained credit");
    Check(result.reason() == "bounded test" && result.primitive_id == Id(20) && result.task_id == Id(99),
          "bounded reason and exact binary wait identities retained");
    result = op.Wait(lock, [] { return true; }, {}, c::WaitClock::now(), "bounded test");
    Check(result.outcome == O::satisfied && lock.owns_lock(), "truth precedes expired deadline");
    result = op.Wait(lock, []() -> bool { throw std::runtime_error("predicate"); }, {},
        c::WaitClock::now(), "bounded test");
    Check(result.failure == c::WaitFailure::fail_safe_release && lock.owns_lock(), "predicate exception unwinds");
    lock.unlock();
    auto snapshot = owner.Snapshot();
    Check(snapshot.waiter_count == 0 && snapshot.operation_ref_count == 1 &&
          snapshot.registered_waits == 0 && snapshot.selected_timeouts == 1,
          "immediate results never register waiters");
    c::ConditionWaitOperation invalid;
    Check(owner.Acquire(Id(20), 6, Hazard(32), invalid) == S::invalid_request && !invalid,
          "stale primitive generation refused");
    Check(owner.Acquire(Id(55), 7, Hazard(32), invalid) == S::invalid_request && !invalid,
          "foreign primitive identity refused");
    Check(!owner.FenceAdmission(), "cannot fence unclosed instance");
    Check(owner.Close(Id(24)) && owner.Close(Id(25)), "idempotent close");
    lock.lock(); unsigned invoked = 0;
    result = op.Wait(lock, [&] { ++invoked; return true; }, {}, c::WaitClock::now(), "bounded test");
    Check(result.outcome == O::closed && invoked == 0, "closed entry never invokes predicate");
    lock.unlock();
    Check(owner.Snapshot().descriptor.last_transition == Id(24), "repeat close preserves selected transition");
  });
}

void Precedence() {
  for (unsigned mask = 1; mask < 16; ++mask) Run([&](auto& owner, auto&) {
    std::stop_source stop;
    if (mask & 1) Check(owner.Close(Id(24)), "precedence close");
    if (mask & 2) stop.request_stop();
    auto op = Acquire(owner, 31); auto lock = op.LockPredicate(); unsigned calls = 0;
    const auto result = op.Wait(lock, [&] { ++calls; return (mask & 4) != 0; }, stop.get_token(),
        c::WaitClock::now() + ((mask & 8) ? -1s : 5s));
    const O expected = (mask & 1) ? O::closed : (mask & 2) ? O::cancelled :
        (mask & 4) ? O::satisfied : O::timed_out;
    Check(result.outcome == expected && lock.owns_lock(), "all terminal precedence combinations");
    Check(calls == ((mask & 3) ? 0U : 1U), "close/cancel precede predicate invocation");
    lock.unlock();
    Check(owner.Snapshot().registered_waits == 0, "precedence immediate result unregistered");
    Check(owner.Close(Id(25)), "late close"); stop.request_stop();
    Check(result.outcome == expected, "late close/cancel cannot rewrite selected result");
  });
}

void PublicationAndSpuriousWake() {
  Run([](auto& owner, auto&) {
    std::latch entered(1); std::counting_semaphore<> rechecked(0);
    unsigned calls = 0; bool ready = false; std::array<unsigned, 32> payload{};
    auto worker = std::async(std::launch::async, [&] {
      auto op = Acquire(owner, 31); auto lock = op.LockPredicate();
      const auto result = op.Wait(lock, [&] {
        if (++calls == 1) entered.count_down();
        if (calls >= 3 && !ready) rechecked.release();
        return ready;
      }, {}, c::WaitClock::now() + 10s, "bounded publication");
      Check(lock.owns_lock() && result.outcome == O::satisfied, "actual wait observes publication");
      for (unsigned i = 0; i < payload.size(); ++i) Check(payload[i] == i + 42, "full payload visible");
      return result;
    });
    entered.wait();
    auto producer = Acquire(owner, 32);
    { auto lock = producer.LockPredicate(); Check(calls >= 2, "registration reached native park"); }
    for (unsigned i = 0; i < 8; ++i) {
      producer.NotifyAll(); rechecked.acquire();
      { auto lock = producer.LockPredicate(); Check(!ready, "false predicate reparks"); }
      Check(worker.wait_for(0s) != std::future_status::ready, "spurious wake is not success");
      Check(owner.Snapshot().registered_waits == 1, "one registered call across repeated wakes");
    }
    {
      auto lock = producer.LockPredicate();
      for (unsigned i = 0; i < payload.size(); ++i) payload[i] = i + 42;
      ready = true;
    }
    producer.NotifyAll();
    Check(worker.get().failure == c::WaitFailure::none, "actual completion");
    Check(owner.Snapshot().waiter_count == 0, "registration removed exactly once");
  });
}

void CancellationAndCloseDrain() {
  Run([](auto& owner, auto& domain) {
    std::latch entered(1); std::stop_source stop;
    auto worker = std::async(std::launch::async, [&] {
      auto op = Acquire(owner, 31); auto lock = op.LockPredicate(); bool first = true;
      return op.Wait(lock, [&] { if (first) { first = false; entered.count_down(); } return false; },
          stop.get_token(), std::nullopt);
    });
    entered.wait();
    auto notifier = Acquire(owner, 32);
    { auto lock = notifier.LockPredicate(); }
    const auto live = owner.Snapshot();
    Check(live.waiter_count == 1 && live.operation_ref_count == 3,
          "wait operation callback and notifier references counted");
    stop.request_stop();
    Check(worker.get().outcome == O::cancelled, "cancellation wakes actual indefinite wait");
    Check(owner.Snapshot().operation_ref_count == 1, "callback joined and reference released");
    Check(owner.Close(Id(24)) && owner.FenceAdmission(), "close/fence with retained notifier");
    auto refused = c::ConditionWaitOperation{};
    Check(owner.Acquire(Id(20), 7, Hazard(33), refused) == S::closed, "fence rejects fresh reference");
    Check(owner.Drain(c::WaitClock::now()).failure == c::WaitFailure::fail_safe_release,
          "zero waiters plus retained notifier is not drain");
    std::stop_source drain_stop;
    auto drain = std::async(std::launch::async, [&] {
      return owner.Drain(c::WaitClock::now() + 10s, drain_stop.get_token());
    });
    drain_stop.request_stop();
    const auto cancelled_drain = drain.get();
    Check(cancelled_drain.outcome == O::cancelled &&
          cancelled_drain.failure == c::WaitFailure::fail_safe_release &&
          cancelled_drain.required_action == "retain_storage_complete_drain",
          "cancelled drain retains storage rather than reporting cleanup");
    Check(domain.Collect() == S::ok && domain.Snapshot().retired == 1,
          "pending drain retains actual charged object");
    notifier.NotifyAll(); notifier.Reset();
    Check(owner.Drain(c::WaitClock::now() + 5s).failure == c::WaitFailure::none,
          "last actual reference permits drain");
  });
  Run([](auto& owner, auto&) {
    constexpr unsigned n = 8;
    std::latch entered(n); std::array<std::future<c::ConditionWaitResult>, n> futures;
    for (unsigned i = 0; i < n; ++i) futures[i] = std::async(std::launch::async, [&, i] {
      auto op = Acquire(owner, 40 + i); auto lock = op.LockPredicate(); bool first = true;
      return op.Wait(lock, [&] { if (first) { first = false; entered.count_down(); } return false; },
          {}, c::WaitClock::now() + 10s, "bounded broadcast");
    });
    entered.wait();
    Check(owner.Snapshot().waiter_count == n, "every broadcast waiter registered");
    std::thread a([&] { Check(owner.Close(Id(24)), "first concurrent close"); });
    std::thread b([&] { Check(owner.Close(Id(25)), "second concurrent close"); });
    a.join(); b.join();
    for (auto& f : futures) Check(f.get().outcome == O::closed, "broadcast returns closed to every waiter");
    Check(owner.Snapshot().waiter_count == 0, "broadcast removes every registration");
  });
}

void AdmissionLimitsAndConstructionFailure() {
  Run([](auto& owner, auto&) {
    auto op = Acquire(owner, 31);
    c::ConditionWaitOperation extra;
    Check(owner.Acquire(Id(20), 7, Hazard(32), extra) == S::exhausted && !extra,
          "operation saturation before reference publication");
    std::stop_source stop; auto lock = op.LockPredicate();
    const auto r = op.Wait(lock, [] { return false; }, stop.get_token(), std::nullopt);
    Check(r.failure == c::WaitFailure::invalid_descriptor, "callback reference saturation before registration");
    lock.unlock();
    const auto s = owner.Snapshot();
    Check(s.waiter_count == 0 && s.registered_waits == 0 && s.operation_ref_count == 1,
          "no partial counter mutation at saturation");
  }, {1, 1});
  Fixture f;
  {
    m::MemorySafeRetirement domain(*f.resource, Id(10), 4, 1);
    Check(domain.Initialize() == S::ok, "bounded owner-reader domain");
    auto occupied = domain.Emplace<unsigned>(Id(11), m::SafeRetirementObjectKind::temporary_descriptor, 42);
    Check(occupied.ok(), "occupy actual reader slot");
    m::SafeRetirementGuard guard;
    Check(domain.Protect(occupied.handle, Hazard(12), guard) == S::ok, "retain reader slot");
    c::ConditionWaitOwner failed(domain);
    Check(failed.Initialize(Descriptor(), {1, 2}, Hazard(30)) == S::exhausted,
          "owner reference exhaustion fails initialization");
    Check(domain.Collect() == S::ok && domain.Snapshot().retired == 0,
          "unpublished condition state safely reclaimable");
    guard.Reset();
    Check(domain.Drain(c::WaitClock::now() + 5s) == S::ok, "failed initialization leaves no protected leak");
  }
  f.Empty();
}

void NativeAndRegisteredFailure() {
#if defined(SB_WAIT_NATIVE_FAULT_GATE)
  for (bool timed : {false, true}) Run([&](auto& owner, auto&) {
    auto op = Acquire(owner, 31); auto lock = op.LockPredicate(); std::stop_source stop;
    unsigned calls = 0;
    const auto r = op.Wait(lock, [&] {
      if (++calls == 2) fail_next_native_wait = true;
      return calls > 2; // Ignoring the actual native error would falsely satisfy.
    }, stop.get_token(), timed ? std::optional(c::WaitClock::now() + 1s) : std::nullopt);
    Check(r.outcome == O::failed && r.failure == c::WaitFailure::fail_safe_release &&
          lock.owns_lock() && !fail_next_native_wait, "native error cannot become predicate success");
    lock.unlock(); const auto s = owner.Snapshot();
    Check(s.waiter_count == 0 && s.operation_ref_count == 1 && s.registered_waits == 1,
          "native error unregisters waiter and joins callback");
  });
#endif
  Run([](auto& owner, auto&) {
    auto op = Acquire(owner, 31); auto lock = op.LockPredicate(); std::stop_source stop;
    unsigned calls = 0;
    const auto r = op.Wait(lock, [&]() -> bool {
      if (++calls == 2) throw std::runtime_error("registered predicate");
      return false;
    }, stop.get_token(), std::nullopt);
    Check(r.outcome == O::failed && lock.owns_lock(), "registered predicate failure");
    lock.unlock();
    const auto s = owner.Snapshot();
    Check(s.waiter_count == 0 && s.operation_ref_count == 1 && s.registered_waits == 1,
          "registered exception releases both registrations");
  });
}

void NativeDrainFailure() {
#if defined(SB_WAIT_NATIVE_FAULT_GATE)
  Run([](auto& owner, auto& domain) {
    auto op = Acquire(owner, 31);
    Check(owner.Close(Id(24)) && owner.FenceAdmission(), "native drain close and fence");
    native_timed_waits = 0;
    fail_next_native_wait = true;
    const auto result = owner.Drain(c::WaitClock::now() + 20ms);
    const auto calls = native_timed_waits;
    const bool injected = !fail_next_native_wait;
    fail_next_native_wait = false;
    Check(result.failure == c::WaitFailure::fail_safe_release &&
          result.required_action == "retain_storage_complete_drain",
          "native drain failure preserves fail-safe outcome");
    Check(owner.Snapshot().operation_ref_count == 1 && domain.Collect() == S::ok &&
          domain.Snapshot().retired == 1, "native drain failure retains reachable storage");
    op.Reset();
    Check(owner.Drain(c::WaitClock::now() + 1s).failure == c::WaitFailure::none,
          "actual reference release allows drain retry after native failure");
    Check(injected && calls == 1, "native drain error returns without retrying or parking again");
  });
#endif
}

void ControlledCancellationAndUnregister() {
#if defined(SB_WAIT_NATIVE_FAULT_GATE)
  Run([](auto& owner, auto&) {
    NativePause registering;
    std::stop_source stop;
    auto worker = std::async(std::launch::async, [&] {
      auto op = Acquire(owner, 31); auto lock = op.LockPredicate(); bool first = true;
      return op.Wait(lock, [&] {
        if (first) {
          first = false; registering.mutex = lock.mutex()->native_handle();
          pause_after_unlock = &registering;
        }
        return false;
      }, stop.get_token(), std::nullopt);
    });
    registering.reached.acquire(); // Counts reserved, callback not constructed yet.
    Check(owner.Snapshot().waiter_count == 1 && owner.Snapshot().operation_ref_count == 2,
          "registration gap retains both operation and callback capacity");
    Check(stop.request_stop(), "cancel before callback construction");
    registering.resume.release();
    Check(worker.get().outcome == O::cancelled, "inline already-requested callback cannot deadlock");
    Check(owner.Snapshot().operation_ref_count == 0, "registration-race references drained");
  });
  for (unsigned repeat = 0; repeat < 20; ++repeat) Run([](auto& owner, auto&) {
    NativePause parked, cancellation_attempt;
    std::stop_source stop;
    auto worker = std::async(std::launch::async, [&] {
      auto op = Acquire(owner, 31); auto lock = op.LockPredicate();
      pause_before_park = &parked;
      return op.Wait(lock, [] { return false; }, stop.get_token(), std::nullopt);
    });
    parked.reached.acquire(); // Real native park entry, predicate mutex still held.
    cancellation_attempt.mutex = parked.mutex;
    auto cancel = std::async(std::launch::async, [&] {
      observe_mutex_attempt = &cancellation_attempt;
      return stop.request_stop();
    });
    const bool participated = cancellation_attempt.reached.try_acquire_for(5s);
    const bool requested = stop.stop_requested();
    parked.resume.release(); // Actual pthread release/park races with callback lock.
    // A broken cancellation protocol must still leave a recoverable test. This
    // rescue close is used ONLY after the explicit callback-lock oracle failed.
    if (!participated) owner.Close(Id(24));
    const bool accepted = cancel.get();
    const auto outcome = worker.get().outcome;
    Check(participated && requested, "cancel published at actual locked park boundary");
    Check(accepted, "requester joins its callback");
    Check(outcome == O::cancelled, "no cancellation lost across native park");
    Check(owner.Snapshot().operation_ref_count == 0 && owner.Snapshot().waiter_count == 0,
          "controlled cancellation leaves no references");
  });
  Run([](auto& owner, auto& domain) {
    NativePause unregistering;
    std::stop_source stop;
    auto worker = std::async(std::launch::async, [&] {
      auto op = Acquire(owner, 31); auto lock = op.LockPredicate(); unsigned calls = 0;
      return op.Wait(lock, [&] {
        if (++calls == 2) {
          unregistering.mutex = lock.mutex()->native_handle();
          pause_after_unlock = &unregistering;
          return true;
        }
        return false;
      }, stop.get_token(), std::nullopt);
    });
    unregistering.reached.acquire(); // Terminal selected; callback not detached yet.
    const auto selected = owner.Snapshot();
    Check(selected.waiter_count == 0 && selected.operation_ref_count == 2,
          "selected wait still accounts its callback until unregister");
    Check(owner.Close(Id(24)) && owner.FenceAdmission(), "close after terminal selection");
    Check(owner.Drain(c::WaitClock::now()).failure == c::WaitFailure::fail_safe_release,
          "selected terminal is not callback drain");
    Check(domain.Collect() == S::ok && domain.Snapshot().retired == 1,
          "callback lifetime retains real allocation");
    Check(stop.request_stop(), "late callback executes on retained closed storage");
    unregistering.resume.release();
    Check(worker.get().outcome == O::satisfied, "late close and cancellation preserve selected satisfaction");
  });
#endif
}

template<class Result>
void CheckDiagnosticContext(const Result& result, const char* code, unsigned severity) {
  if constexpr (requires { result.Diagnostic(); }) {
    const auto d = result.Diagnostic();
    Check(d.has_value(), "failure has typed concurrency diagnostic");
    Check(d->registration.code == code && static_cast<unsigned>(d->registration.severity) == severity &&
          d->registration.is_failure && d->registration.sqlstate == "not_applicable" &&
          d->registration.numeric_binding == "not_applicable" &&
          d->registration.diagnostic_class == "MGA.CONCURRENCY", "exact canonical registration facts");
    Check(d->primitive_id == Id(20) && d->thread_or_task_id == Id(99) &&
          d->owner_scope == c::WaitOwnerScope::database && d->primitive_class == "condition_wait" &&
          d->requested_mode == "none" && d->held_modes_summary == "none" && d->protected_data,
          "protected binary primitive task and scope diagnostic context");
    Check(d->wait_duration_us == result.wait_duration_us && d->required_action == result.required_action,
          "diagnostic preserves selected duration and corrective action");
  } else Check(false, "missing typed concurrency diagnostic context");
}

void DiagnosticRetention() {
  c::ConditionWaitResult timeout, invalid, drain;
  Run([&](auto& owner, auto&) {
    auto op = Acquire(owner, 31); auto lock = op.LockPredicate();
    const auto satisfied = op.Wait(lock, [] { return true; }, {}, c::WaitClock::now(), "diagnostic success");
    Check(!satisfied.Diagnostic(), "satisfied wait invents no failure diagnostic");
    std::stop_source stop; stop.request_stop();
    const auto cancelled = op.Wait(lock, [] { return false; }, stop.get_token(), std::nullopt);
    Check(cancelled.outcome == O::cancelled && !cancelled.Diagnostic(),
          "ordinary wait cancellation invents no failure diagnostic");
    timeout = op.Wait(lock, [] { return false; }, {}, c::WaitClock::now(), "diagnostic timeout");
    lock.unlock();
    std::mutex wrong; std::unique_lock other(wrong);
    invalid = op.Wait(other, [] { return true; }, {}, c::WaitClock::now(), "invalid binding");
    other.unlock();
    Check(owner.Close(Id(24)) && owner.FenceAdmission(), "diagnostic drain close and fence");
    lock.lock();
    const auto closed = op.Wait(lock, [] { return true; }, {}, c::WaitClock::now(), "diagnostic close");
    Check(closed.outcome == O::closed && !closed.Diagnostic(), "closed wait invents no failure diagnostic");
    lock.unlock();
    drain = owner.Drain(c::WaitClock::now());
  });
  // All primitive/native storage is reclaimed before inspecting copied results.
  CheckDiagnosticContext(timeout, "diag.mga.concurrency.latch_timeout", 3);
  CheckDiagnosticContext(invalid, "diag.mga.concurrency.invalid_primitive_descriptor", 4);
  CheckDiagnosticContext(drain, "diag.mga.concurrency.fail_safe_release", 3);
}

void OwnershipProfiles() {
  Fixture f;
  {
    m::MemorySafeRetirement domain(*f.resource, Id(10), 4, 8);
    Check(domain.Initialize() == S::ok, "ownership domain initialized");
    const auto baseline = f.manager.Snapshot().current_bytes;
    for (int scope = 0; scope <= static_cast<int>(c::WaitOwnerScope::evidence); ++scope) {
      for (int profile = 0; profile < 3; ++profile) {
        for (int identity = 0; identity < 4; ++identity) {
          auto d = Descriptor();
          d.owner_scope = static_cast<c::WaitOwnerScope>(scope);
          d.ownership_profile = static_cast<c::WaitOwnershipProfile>(profile);
          if (identity == 0) d.owner_uuid.reset();
          if (identity == 2) d.owner_uuid = c::WaitUuid{};
          if (identity == 3) (*d.owner_uuid)[8] = 0;
          const bool global = profile == 1 && scope == 0;
          const bool admitted = (profile == 0 || global) &&
              (identity == 1 || (identity == 0 && global));
          {
            c::ConditionWaitOwner owner(domain);
            const auto result = owner.Initialize(d, {2, 4}, Hazard(30));
            // A regression accepting forbidden ownership must still drain its
            // real storage before the assertion unwinds the owner lifetime.
            if (!admitted && result == S::ok) Finish(owner);
            Check(result == (admitted ? S::ok : S::invalid_request), "scope/profile/identity admission matrix");
            if (admitted) {
              try {
                const auto snapshot = owner.Snapshot();
                Check(snapshot.descriptor.owner_uuid == d.owner_uuid &&
                      snapshot.descriptor.ownership_profile == d.ownership_profile,
                      "explicit presence and binary owner preserved");
                Check(f.manager.Snapshot().current_bytes > baseline, "global profile still charges storage");
                auto op = Acquire(owner, 31); auto lock = op.LockPredicate();
                Check(op.Wait(lock, [] { return true; }, {}, c::WaitClock::now(), "owner profile").outcome == O::satisfied,
                      "admitted ownership retains executable wait");
              } catch (...) { Finish(owner); throw; }
              Finish(owner);
            } else {
              Check(domain.Snapshot().published == 0 && f.manager.Snapshot().current_bytes == baseline,
                    "invalid ownership refuses before allocation or publication");
            }
          }
          Check(domain.Collect() == S::ok && f.manager.Snapshot().current_bytes == baseline,
                "ownership case reclaims actual storage");
        }
      }
    }
  }
  f.Empty();
}

void DescriptorAndWaiterBounds() {
  Fixture f;
  {
    m::MemorySafeRetirement domain(*f.resource, Id(10), 4, 8);
    Check(domain.Initialize() == S::ok, "descriptor validation domain");
    const auto before = f.manager.Snapshot().current_bytes;
    for (unsigned which = 0; which < 8; ++which) {
      c::ConditionWaitOwner owner(domain); auto d = Descriptor(); c::ConditionWaitLimits limits{2, 4};
      if (which == 0) d.primitive_id = {};
      if (which == 1) d.predicate_mutex_id[6] = 0;
      if (which == 2) (*d.owner_uuid)[8] = 0;
      if (which == 3) d.last_transition = {};
      if (which == 4) d.owner_scope = static_cast<c::WaitOwnerScope>(100);
      if (which == 5) limits.waiters = 0;
      if (which == 6) limits.operation_references = 0;
      if (which == 7) d.predicate_mutex_id = d.primitive_id;
      Check(owner.Initialize(d, limits, Hazard(30)) == S::invalid_request, "malformed descriptor refused");
      Check(f.manager.Snapshot().current_bytes == before && domain.Snapshot().published == 0,
            "invalid descriptor has no resource/publication effect");
    }
  }
  f.Empty();
  Run([](auto& owner, auto&) {
    std::latch entered(1);
    auto worker = std::async(std::launch::async, [&] {
      auto op = Acquire(owner, 31); auto lock = op.LockPredicate(); bool first = true;
      return op.Wait(lock, [&] { if (first) { first = false; entered.count_down(); } return false; },
          {}, c::WaitClock::now() + 10s, "bounded capacity");
    });
    entered.wait();
    auto extra = Acquire(owner, 32); auto lock = extra.LockPredicate();
    const auto refused = extra.Wait(lock, [] { return false; }, {}, c::WaitClock::now() + 5s,
        "bounded capacity");
    Check(refused.failure == c::WaitFailure::invalid_descriptor, "waiter limit refuses before mutation");
    CheckDiagnosticContext(refused, "diag.mga.concurrency.invalid_primitive_descriptor", 4);
    Check(refused.reason() == "bounded capacity", "admission refusal retains bounded reason");
    lock.unlock();
    Check(owner.Snapshot().waiter_count == 1 && owner.Snapshot().registered_waits == 1,
          "existing waiter survives saturation");
    Check(owner.Close(Id(24)), "close after saturation");
    Check(worker.get().outcome == O::closed, "existing saturated-table waiter completes");
  }, {1, 4});
}

void ActualDeadlineAndReplacement() {
  Run([](auto& owner, auto&) {
    auto op = Acquire(owner, 31); auto lock = op.LockPredicate();
    const auto deadline = c::WaitClock::now() + 2ms;
#if defined(SB_WAIT_NATIVE_FAULT_GATE)
    const auto before = native_timed_waits;
#endif
    const auto result = op.Wait(lock, [] { return false; }, {}, deadline, "finite native deadline");
    Check(result.outcome == O::timed_out && lock.owns_lock() &&
          c::WaitClock::now() >= deadline && result.wait_duration_us > 0,
          "real monotonic timed park expires without early completion");
#if defined(SB_WAIT_NATIVE_FAULT_GATE)
    Check(native_timed_waits > before, "timeout exercised actual pthread timed wait");
#endif
    lock.unlock(); const auto snapshot = owner.Snapshot();
    Check(snapshot.registered_waits == 1 && snapshot.selected_timeouts == 1,
          "native timeout counted once");
  });
  Fixture f;
  {
    m::MemorySafeRetirement domain(*f.resource, Id(10), 1, 8);
    Check(domain.Initialize() == S::ok, "replacement domain");
    {
      c::ConditionWaitOwner old(domain);
      Check(old.Initialize(Descriptor(), {2, 4}, Hazard(30)) == S::ok, "first incarnation");
      auto op = Acquire(old, 31); auto transferred = std::move(op);
      Check(!op && transferred && old.Snapshot().operation_ref_count == 1,
            "move transfers exactly one lifetime reference");
      transferred.Reset(); Finish(old);
    }
    Check(domain.Collect() == S::ok, "actual first incarnation reclaimed");
    {
      c::ConditionWaitOwner replacement(domain); auto descriptor = Descriptor();
      descriptor.primitive_id = Id(60); descriptor.generation = 8;
      Check(replacement.Initialize(descriptor, {2, 4}, Hazard(32)) == S::ok, "fresh incarnation reuses capacity");
      c::ConditionWaitOperation op;
      Check(replacement.Acquire(Id(20), 7, Hazard(33), op) == S::invalid_request && !op,
            "old identity cannot operate on replacement");
      Check(replacement.Acquire(Id(60), 7, Hazard(33), op) == S::invalid_request && !op,
            "old generation cannot operate on replacement");
      Check(replacement.Acquire(Id(60), 8, Hazard(33), op) == S::ok, "fresh exact identity admitted");
      op.Reset(); Finish(replacement);
    }
    Check(domain.Collect() == S::ok, "replacement reclaimed by memory owner");
  }
  f.Empty();
}

#if defined(__unix__)
m::MemoryManager* destruction_manager = nullptr;
void PrematureDestruction() {
  const pid_t child = fork();
  Check(child >= 0, "destruction child");
  if (child == 0) {
    std::set_terminate([] {
      // Fail before any reachable storage is physically released; no core dump.
      _exit(destruction_manager && destruction_manager->Snapshot().current_bytes > 0 ? 86 : 87);
    });
    Fixture f; destruction_manager = &f.manager;
    m::MemorySafeRetirement domain(*f.resource, Id(10), 4, 8);
    if (domain.Initialize() != S::ok) _exit(88);
    {
      c::ConditionWaitOwner owner(domain);
      if (owner.Initialize(Descriptor(), {2, 4}, Hazard(30)) != S::ok) _exit(89);
      // No close, fence or drain. The owning destruction boundary must refuse.
    }
    _exit(90);
  }
  int status = 0;
  Check(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 86,
        "premature destruction fails before freeing reachable storage");
}
#endif
} // namespace

int main(int argc, char** argv) {
  try {
    if (argc == 2 && std::string_view(argv[1]) == "--drain-native-error") {
      NativeDrainFailure(); return 0;
    }
    if (argc == 2 && std::string_view(argv[1]) == "--native-errors") {
      NativeAndRegisteredFailure(); return 0;
    }
    if (argc == 2 && std::string_view(argv[1]) == "--cancel-park") {
      ControlledCancellationAndUnregister(); return 0;
    }
    EntryAndValidation(); Precedence(); PublicationAndSpuriousWake();
    CancellationAndCloseDrain(); AdmissionLimitsAndConstructionFailure();
    NativeAndRegisteredFailure(); NativeDrainFailure();
    ControlledCancellationAndUnregister(); DescriptorAndWaiterBounds();
    ActualDeadlineAndReplacement(); OwnershipProfiles(); DiagnosticRetention();
#if defined(__unix__)
    if (argc != 2 || std::string_view(argv[1]) != "--no-destructor-child") PrematureDestruction();
#endif
    std::cout << "retained condition wait: " << checks << " checks PASS\n";
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "retained condition wait: " << e.what() << '\n'; return 1;
  }
}

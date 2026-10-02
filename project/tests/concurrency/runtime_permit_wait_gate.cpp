// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "runtime_permit_fixture.hpp"
#include "retained_runtime_semaphore.hpp"
#include <cerrno>
#include <semaphore>
#include <sys/wait.h>
#include <unistd.h>

namespace {
struct UnlockProbe {
  pthread_mutex_t* mutex = nullptr;
  unsigned remaining = 2;
  std::binary_semaphore reached{0}, resume{0};
};
thread_local UnlockProbe* unlock_probe = nullptr;
thread_local pthread_mutex_t* fail_lock_target = nullptr;
struct LockProbe {
  pthread_mutex_t* mutex = nullptr;
  std::binary_semaphore attempted{0};
};
thread_local LockProbe* lock_probe = nullptr;
struct WakeProbe {
  pthread_cond_t* condition = nullptr;
  std::atomic<bool> armed{false};
  std::binary_semaphore reached{0}, resume{0};
};
thread_local WakeProbe* wake_probe = nullptr;
thread_local WakeProbe* broadcast_probe = nullptr;
void AfterActualWake(int code) {
  if (code == 0 && wake_probe && wake_probe->armed.load()) {
    auto* pause = std::exchange(wake_probe,nullptr);
    pause->reached.release(); pause->resume.acquire();
  }
}
// Observe the actual native park, not a test implementation of admission.
struct ParkProbe {
  std::mutex mutex;
  std::condition_variable changed;
  unsigned parks = 0;
  bool hold_first = false;
  bool fail = false;
  std::binary_semaphore resume{0};
  pthread_mutex_t* native_mutex = nullptr;
  pthread_cond_t* native_condition = nullptr;
  void Enter(pthread_cond_t* condition, pthread_mutex_t* lock) {
    if (unlock_probe && !unlock_probe->mutex) unlock_probe->mutex = lock;
    unsigned count;
    {
      std::lock_guard guard(mutex);
      native_mutex = lock; native_condition = condition;
      count = ++parks;
      changed.notify_all();
    }
    if (hold_first && count == 1) resume.acquire();
  }
  void Await(unsigned count) {
    std::unique_lock lock(mutex);
    Check(changed.wait_for(lock, std::chrono::seconds(5), [&] { return parks >= count; }),
          "actual native wait reached");
  }
  void Spurious() {
    // Await has released the observation lock. Taking the real governor mutex
    // ensures the observed wait has atomically released it before broadcast.
    Check(pthread_mutex_lock(native_mutex) == 0, "lock actual park mutex");
    Check(pthread_cond_broadcast(native_condition) == 0, "actual spurious broadcast");
    Check(pthread_mutex_unlock(native_mutex) == 0, "unlock actual park mutex");
  }
};
thread_local ParkProbe* probe = nullptr;
thread_local bool fail_init = false;
}
extern "C" int __real_pthread_cond_wait(pthread_cond_t*, pthread_mutex_t*);
extern "C" int __real_pthread_cond_timedwait(pthread_cond_t*, pthread_mutex_t*, const timespec*);
extern "C" int __real_pthread_cond_init(pthread_cond_t*, const pthread_condattr_t*);
extern "C" int __real_pthread_mutex_unlock(pthread_mutex_t*);
extern "C" int __real_pthread_mutex_lock(pthread_mutex_t*);
extern "C" int __real_pthread_cond_broadcast(pthread_cond_t*);
extern "C" int __wrap_pthread_cond_broadcast(pthread_cond_t* condition) {
  if (broadcast_probe && broadcast_probe->condition == condition) broadcast_probe->armed.store(true);
  return __real_pthread_cond_broadcast(condition);
}
extern "C" int __wrap_pthread_mutex_lock(pthread_mutex_t* mutex) {
  if (lock_probe && lock_probe->mutex == mutex) {
    auto* observation = std::exchange(lock_probe,nullptr); observation->attempted.release();
  }
  if (fail_lock_target == mutex) { fail_lock_target = nullptr; return EINVAL; }
  return __real_pthread_mutex_lock(mutex);
}
extern "C" int __wrap_pthread_mutex_unlock(pthread_mutex_t* mutex) {
  auto* pause = unlock_probe && unlock_probe->mutex == mutex && --unlock_probe->remaining == 0
      ? std::exchange(unlock_probe,nullptr) : nullptr;
  const int code = __real_pthread_mutex_unlock(mutex);
  if (pause) { pause->reached.release(); pause->resume.acquire(); }
  return code;
}
extern "C" int __wrap_pthread_cond_wait(pthread_cond_t* c, pthread_mutex_t* m) {
  if (probe) { probe->Enter(c,m); if (probe->fail) return EINVAL; }
  const int code = __real_pthread_cond_wait(c,m); AfterActualWake(code); return code;
}
extern "C" int __wrap_pthread_cond_timedwait(pthread_cond_t* c, pthread_mutex_t* m,
                                            const timespec* deadline) {
  if (probe) { probe->Enter(c,m); if (probe->fail) return EINVAL; }
  const int code = __real_pthread_cond_timedwait(c,m,deadline); AfterActualWake(code); return code;
}
extern "C" int __wrap_pthread_cond_init(pthread_cond_t* c, const pthread_condattr_t* attr) {
  if (std::exchange(fail_init, false)) return ENOMEM;
  return __real_pthread_cond_init(c,attr);
}
namespace {
using Clock = std::chrono::steady_clock;
a::RuntimePermitAcquireControl Control(const std::stop_source& stop) {
  return {stop.get_token(), Clock::now() + std::chrono::seconds(5)};
}
void Configure(Fixture& f) {
  f.policy.worker_capacity = f.policy.queue_capacity = 1;
  f.policy.worker_waiter_limit = f.policy.queue_waiter_limit = 1;
  Check(f.Bind() == Code::bound, "bind selected wait limits");
}
void NoCalls(Fixture& f) {
  const auto s = f.governor.Snapshot();
  Check(!s.worker_waiters && !s.queue_waiters && !s.worker_wait_calls && !s.queue_wait_calls,
        "all wait registrations and joined calls retired");
}
void ReleaseWake(bool worker, bool spurious) {
  Fixture f(false); Configure(f);
  auto held = f.governor.AcquireRuntimePermit(f.Request(worker));
  Check(held.ok(), "seed real capacity ownership");
  ParkProbe p; p.hold_first = true;
  std::stop_source stop;
  a::RuntimePermitWaitResult result;
  std::thread waiter([&] { probe = &p;
    result = f.governor.WaitAcquireRuntimePermit(f.Request(worker,21), Control(stop));
    probe = nullptr;
  });
  p.Await(1);
  if (spurious) {
    p.resume.release();
    for (unsigned count = 2; count <= 9; ++count) { p.Spurious(); p.Await(count); }
    const auto s = f.governor.Snapshot();
    Check(s.retained_runtime_permits == 1 &&
          (worker ? s.worker_wait_calls : s.queue_wait_calls) == 1,
          "spurious wake neither grants nor retires holder");
    Check(held.permit.Release(f.Request(worker)) == Code::released, "actual release wakes");
  } else {
    // Releaser contends before the waiter atomically unlocks and parks.
    std::latch started{1};
    Code released = Code::no_grant;
    std::thread releaser([&] { started.count_down(); released = held.permit.Release(f.Request(worker)); });
    started.wait(); p.resume.release(); releaser.join();
    Check(released == Code::released, "release at park boundary");
  }
  waiter.join();
  Check(result.admission.ok(), "released real credit grants without rescue wake");
  Check(result.registered, "actual registration is explicit without duration inference");
  Check(result.admission.permit.Release(f.Request(worker,21)) == Code::released, "wait grant owns release");
  NoCalls(f); f.Empty();
}
void Terminal(unsigned mode) {
  Fixture f(false); Configure(f);
  auto held = f.governor.AcquireRuntimePermit(f.Request(true));
  Check(held.ok(), "terminal seed grant");
  ParkProbe p;
  std::stop_source stop;
  auto control = Control(stop);
  if (mode == 0) control.wait_deadline.reset(); // Cancellation must wake an indefinite park.
  if (mode == 2) control.wait_deadline = Clock::now() + std::chrono::milliseconds(100);
  if (mode == 3) p.fail = true;
  a::RuntimePermitWaitResult result;
  std::thread waiter([&] { probe = &p;
    result = f.governor.WaitAcquireRuntimePermit(f.Request(true,21), control);
    probe = nullptr;
  });
  p.Await(1);
  if (mode == 0) stop.request_stop();
  if (mode == 1) Check(f.governor.CloseRuntimePermitInstance(f.policy.authority,
      a::RuntimePermitProfile::worker_slot, f.workers) == Code::closed, "close selected pool");
  waiter.join();
  const Code expected[] = {Code::cancelled,Code::closed,Code::timed_out,Code::synchronization_failed};
  Check(result.admission.code == expected[mode] && !result.admission.permit, "typed wait terminal");
  Check(f.governor.Snapshot().retained_runtime_permits == 1, "terminal keeps previously owned permit");
  if (mode == 1) {
    auto queue = f.governor.AcquireRuntimePermit(f.Request(false,22));
    Check(queue.ok(), "other pool remains open");
    Check(queue.permit.Release(f.Request(false,22)) == Code::released, "other pool release");
  }
  Check(held.permit.Release(f.Request(true)) == Code::released, "terminal does not revoke release");
  NoCalls(f); f.Empty();
}
void BoundsAndValidation() {
  Fixture f(false); Configure(f);
  auto held = f.governor.AcquireRuntimePermit(f.Request(false));
  Check(held.ok(), "bound seed");
  ParkProbe p; std::stop_source stop;
  a::RuntimePermitWaitResult result;
  std::thread waiter([&] { probe = &p;
    result = f.governor.WaitAcquireRuntimePermit(f.Request(false,21), Control(stop)); probe = nullptr;
  });
  p.Await(1);
  f.policy.queue_waiter_limit = 100; // Caller mutation cannot change pinned policy.
  auto second = f.governor.WaitAcquireRuntimePermit(f.Request(false,22), Control(stop));
  Check(second.admission.code == Code::waiter_exhausted, "immutable whole-call bound");
  auto wrong = f.queue; ++wrong.generation;
  Check(f.governor.CloseRuntimePermitInstance(f.policy.authority,
      a::RuntimePermitProfile::queued_task, wrong) == Code::invalid_binding, "wrong close generation");
  Check(!f.governor.Snapshot().queue_closed, "invalid close no effects");
  stop.request_stop(); waiter.join();
  Check(result.admission.code == Code::cancelled, "bounded call cancelled");
  NoCalls(f);
  auto bad = f.governor.WaitAcquireRuntimePermit(f.Request(false,22), {});
  Check(bad.admission.code == Code::invalid_binding, "uncancellable unbounded wait rejected");
  auto expired = f.governor.WaitAcquireRuntimePermit(f.Request(false,22),
      {{}, Clock::now()}, "shutdown drain");
  Check(expired.admission.code == Code::timed_out && expired.reason() == "shutdown drain",
        "finite uninterruptible reason retained");
  Check(held.permit.Release(f.Request(false)) == Code::released, "bounds seed release");
  NoCalls(f); f.Empty();
}
void ConstructionAndUnbound() {
  Fixture f(false);
  f.policy.worker_waiter_limit = 1;
  fail_init = true;
  Check(f.Bind() == Code::synchronization_failed, "actual native condition init failure");
  Check(f.Bind() == Code::bound, "failed initialization publishes no binding");
  std::stop_source stop;
  auto result = f.governor.WaitAcquireRuntimePermit(f.Request(false), Control(stop));
  Check(result.admission.code == Code::wait_policy_unbound, "zero wait limit is not implicit default");
  NoCalls(f); f.Empty();
}
a::ResourceGovernanceReservationAcquireRequest Legacy(bool worker) {
  a::ResourceGovernanceReservationAcquireRequest r;
  r.owner_uuid = Id(15); r.admission.operation_id = "existing-operation-label";
  auto& d = r.admission.descriptor;
  d.descriptor_id = "existing-policy-label";
  d.family = r.admission.expected_family = a::ResourceGovernanceFamily::kBackgroundJob;
  d.source = a::ResourceGovernanceDescriptorSource::kRuntimePolicy;
  d.source_path_or_label = "runtime.test.policy";
  d.descriptor_generation = d.expected_generation = 1;
  d.limits = {64,64,64,64,64,64,64,64,64,64,64,64,64};
  d.benchmark_clean = d.runtime_dependency_present = true;
  if (worker) r.admission.requested.worker_threads = 1;
  else r.admission.requested.backlog_items = 1;
  r.lease_deadline_tick = 42;
  return r;
}
void LegacyCredit(bool worker, unsigned route, bool memory_exhausted = false) {
  Fixture f(false, memory_exhausted ? 1 : 65536);
  auto old = f.governor.Acquire(Legacy(worker));
  Check(old.ok, "actual legacy capacity reservation");
  Configure(f);
  ParkProbe p; std::stop_source stop;
  a::RuntimePermitWaitResult result;
  std::thread waiter([&] { probe = &p;
    result = f.governor.WaitAcquireRuntimePermit(f.Request(worker), Control(stop)); probe = nullptr;
  });
  p.Await(1);
  Check(f.governor.ReleaseNoAlloc("not-an-owning-token") == a::ResourceGovernanceReleaseCode::not_found,
        "non-owning release cannot create credit");
  Check(f.governor.Snapshot().active_reservation_count == 1, "missing release preserves actual charges");
  switch (route) {
    case 0: Check(f.governor.Release(old.reservation.token_id).ok, "legacy ordinary release"); break;
    case 1: Check(f.governor.ReleaseNoAlloc(old.reservation.token_id) ==
                 a::ResourceGovernanceReleaseCode::released, "legacy no allocation release"); break;
    case 2: Check(f.governor.ReleaseOwnerReservations(Id(15)).released_count == 1,
                 "legacy binary owner cleanup"); break;
    case 3: Check(f.governor.ExpireReservations(42).released_count == 1, "legacy lease expiry"); break;
  }
  waiter.join();
  if (memory_exhausted) {
    Check(result.admission.code == Code::allocation_failed && !result.admission.permit,
          "actual governed metadata exhaustion after capacity wake has no grant");
  } else {
    Check(result.admission.ok(), "every actual legacy credit route wakes native waiters");
    Check(result.admission.permit.Release(f.Request(worker)) == Code::released, "legacy-woken grant release");
  }
  NoCalls(f); f.Empty();
}
void AvailableTerminalPrecedence() {
  for (unsigned mode = 0; mode != 3; ++mode) {
    Fixture f(false); Configure(f);
    std::stop_source stop;
    auto control = Control(stop);
    control.wait_deadline = Clock::now();
    if (mode < 2) stop.request_stop();
    if (mode == 0) Check(f.governor.CloseRuntimePermits() == Code::closed, "close before terminal attempt");
    auto result = f.governor.WaitAcquireRuntimePermit(f.Request(true), control);
    const Code expected[] = {Code::closed, Code::cancelled, Code::timed_out};
    Check(result.admission.code == expected[mode], "closed cancelled timed out before available grant");
    Check(!result.registered, "terminal initial selection does not register a wait");
    NoCalls(f); f.Empty();
  }
  Fixture f(false); Configure(f);
  std::stop_source stop;
  auto result = f.governor.WaitAcquireRuntimePermit(f.Request(true), Control(stop));
  Check(result.admission.ok() && result.reason().empty(), "available bounded wait grants immediately");
  Check(result.admission.permit.Release(f.Request(true)) == Code::released, "immediate bounded grant release");
  NoCalls(f); f.Empty();
}
}
namespace {
void RetainedGrantLifetime(bool worker_profile) {
  namespace c = scratchbird::core::concurrency;
  using S = m::SafeRetirementStatus;
  Fixture f(false); Configure(f);
  {
    m::MemorySafeRetirement domain(*f.resource,Id(60).bytes,4,16);
    Check(domain.Initialize() == S::ok, "actual shared retirement domain initialized");
    const auto instance = worker_profile ? f.workers : f.queue;
    {
      c::RuntimeSemaphoreOwner owner(domain);
      c::RuntimeSemaphoreDescriptor descriptor;
      descriptor.primitive_id = instance.semaphore; descriptor.generation = instance.generation;
      descriptor.owner_uuid = Id(61); descriptor.last_transition = Id(62);
      descriptor.owner_scope = c::WaitOwnerScope::database;
      Check(owner.Initialize(descriptor,{8},f.governor,f.policy.authority,
          worker_profile ? a::RuntimePermitProfile::worker_slot : a::RuntimePermitProfile::queued_task,
          {Id(63).bytes,Id(64).bytes}) == S::ok, "retained primitive backed by actual memory grant");
      c::RuntimeSemaphoreOperation operation;
      Check(owner.AcquireOperation(instance.semaphore,instance.generation,
          {Id(65).bytes,Id(20).bytes},operation) == S::ok, "actual operation hazard");
      std::stop_source stop;
      auto expired = operation.Acquire(f.Request(worker_profile),{Id(69).bytes,Id(20).bytes},
          {stop.get_token(),Clock::now()});
      auto diagnostic = expired.Diagnostic();
      Check(expired.code == Code::timed_out && !expired.permit && diagnostic &&
            diagnostic->primitive_class == "semaphore_budget" && diagnostic->protected_data &&
            diagnostic->primitive_id == instance.semaphore.bytes && diagnostic->thread_or_task_id == Id(20).bytes,
            "protected canonical semaphore timeout vector has binary identities");
      auto result = operation.Acquire(f.Request(worker_profile),{Id(66).bytes,Id(20).bytes},Control(stop));
      Check(result.ok(), "actual governed grant has retained primitive guard");
      const auto live = owner.Snapshot();
      Check(live.governor.holders == 1 && live.operation_ref_count == 1, "real holder and delivery reference distinct");
      Check(owner.Close(Id(67)) && owner.FenceAdmission(), "close fences new lookup without revoking grant");
      operation.Reset();
      Check(!owner.Drain(Clock::now()).drained, "live grant prevents primitive destruction");
      Check(domain.Collect() == S::ok && domain.Snapshot().retained_payload_bytes > 0,
            "actual retired primitive remains physically retained");
      auto wrong = f.Request(worker_profile); ++wrong.semaphore_generation;
      Check(result.permit.Release(wrong) == Code::invalid_binding && bool(result.permit),
            "wrong retained release preserves exact owning guard");
      c::RuntimeSemaphoreOperation late;
      Check(owner.AcquireOperation(instance.semaphore,instance.generation,
          {Id(68).bytes,Id(20).bytes},late) == S::closed, "retired lookup cannot acquire new operation");
      Check(result.permit.Release(f.Request(worker_profile)) == Code::released, "owning grant releases after fence");
      Check(owner.Drain(Clock::now()).drained, "actual final release and fenced zeros allow drain");
    }
    Check(domain.Collect() == S::ok && !domain.Snapshot().retained_payload_bytes,
          "memory owner actually reclaims synchronization payload");
    domain.Close(); Check(domain.Drain(Clock::now()) == S::ok, "real memory domain drains");
  }
  NoCalls(f); f.Empty();
}
namespace c = scratchbird::core::concurrency;
using S = m::SafeRetirementStatus;
struct RetainedFixture {
  Fixture f{false};
  std::optional<m::MemorySafeRetirement> domain;
  std::optional<c::RuntimeSemaphoreOwner> owner;
  bool worker;
  a::RuntimePermitInstanceBinding instance;
  unsigned next_hazard = 1000;
  explicit RetainedFixture(bool worker_profile, std::uint32_t references = 16, m::usize readers = 32,
      std::uint32_t capacity = 1) : worker(worker_profile),
      instance(worker ? f.workers : f.queue) {
    f.policy.worker_capacity = f.policy.queue_capacity = capacity;
    f.policy.worker_waiter_limit = f.policy.queue_waiter_limit = 3;
    Check(f.Bind() == Code::bound, "selected bounded retained profile");
    domain.emplace(*f.resource,Id(80).bytes,4,readers);
    Check(domain->Initialize() == S::ok, "actual retained domain metadata");
    owner.emplace(*domain);
    c::RuntimeSemaphoreDescriptor descriptor;
    descriptor.primitive_id = instance.semaphore; descriptor.generation = instance.generation;
    descriptor.owner_uuid = Id(81); descriptor.last_transition = Id(82);
    descriptor.owner_scope = c::WaitOwnerScope::database;
    Check(owner->Initialize(descriptor,{references},f.governor,f.policy.authority,
        worker ? a::RuntimePermitProfile::worker_slot : a::RuntimePermitProfile::queued_task,
        Hazard(83)) == S::ok, "actual retained primitive publication");
  }
  m::SafeRetirementHazard Hazard(unsigned task) { return {Id(next_hazard++).bytes,Id(task).bytes}; }
  c::RuntimeSemaphoreOperation Operation(unsigned task) {
    c::RuntimeSemaphoreOperation operation;
    Check(owner->AcquireOperation(instance.semaphore,instance.generation,Hazard(task),operation) == S::ok,
          "actual operation admission");
    return operation;
  }
  c::RuntimeSemaphoreAcquireResult Grant(unsigned task) {
    auto operation = Operation(task); std::stop_source stop;
    auto result = operation.Acquire(f.Request(worker,task),Hazard(task),Control(stop));
    Check(result.ok(), "real retained resource grant");
    operation.Reset(); return result;
  }
  void Close() { Check(owner->Close(Id(84)) && owner->FenceAdmission(), "actual close and reference fence"); }
  void Finish() {
    if (owner) {
      Close(); Check(owner->Drain(Clock::now()).drained, "fenced actual final zeros"); owner.reset();
    }
    Check(domain->Collect() == S::ok && !domain->Snapshot().retained_payload_bytes,
          "actual primitive payload reclaimed");
    domain->Close(); Check(domain->Drain(Clock::now()) == S::ok, "actual metadata domain drained");
    domain.reset(); NoCalls(f); f.Empty();
  }
};
void RetainedPendingClose(bool worker) {
  RetainedFixture f(worker); auto seed = f.Grant(20);
  std::array<c::RuntimeSemaphoreOperation,3> operations;
  std::array<c::RuntimeSemaphoreAcquireResult,3> results;
  std::array<ParkProbe,3> parks;
  std::array<std::thread,3> threads;
  std::stop_source stop;
  for (unsigned i = 0; i != 3; ++i) {
    operations[i] = f.Operation(21+i); const auto hazard = f.Hazard(21+i);
    threads[i] = std::thread([&,i,hazard] { probe = &parks[i];
      results[i] = operations[i].Acquire(f.f.Request(worker,21+i),hazard,Control(stop)); probe = nullptr;
    });
    parks[i].Await(1);
  }
  Check(f.owner->Snapshot().governor.waiters == 3, "all actual pending registrations present");
  f.Close();
  for (auto& thread : threads) thread.join();
  for (unsigned i = 0; i != 3; ++i) {
    Check(results[i].code == Code::closed && !results[i].permit && results[i].registered,
          "semaphore_close_pending_acquirers");
    operations[i].Reset();
  }
  Check(f.owner->Snapshot().governor.holders == 1, "close preserves seed grant charge");
  Check(seed.permit.Release(f.f.Request(worker)) == Code::released, "semaphore_release_after_close");
  f.Finish();
}
void RetainedCloseAvailable(bool worker) {
  RetainedFixture f(worker); auto operation = f.Operation(20); std::stop_source stop;
  f.Close(); stop.request_stop();
  auto result = operation.Acquire(f.f.Request(worker),f.Hazard(20),{stop.get_token(),Clock::now()});
  Check(result.code == Code::closed && !result.permit && !result.registered &&
        !f.owner->Snapshot().governor.holders, "semaphore_close_available_capacity and terminal precedence");
  operation.Reset(); f.Finish();
}
void RetainedIdempotentClose(bool worker) {
  RetainedFixture f(worker); auto grant = f.Grant(20);
  Check(f.owner->Close(Id(90)), "first close transition");
  const auto bytes = f.f.manager.Snapshot().current_bytes;
  std::array<bool,4> outcomes{}; std::array<std::thread,4> threads;
  for (unsigned i = 0; i != 4; ++i) threads[i] = std::thread([&,i] { outcomes[i] = f.owner->Close(Id(91+i)); });
  for (auto& thread : threads) thread.join();
  for (bool outcome : outcomes) Check(outcome, "concurrent close acknowledgement");
  const auto snapshot = f.owner->Snapshot();
  Check(snapshot.descriptor.last_transition == Id(90) && snapshot.governor.holders == 1 &&
        f.f.manager.Snapshot().current_bytes == bytes, "semaphore_close_idempotent");
  Check(grant.permit.Release(f.f.Request(worker)) == Code::released, "idempotent close preserves release");
  f.Finish();
}
void RetainedLateDelivery(bool worker, bool grant_wins) {
  RetainedFixture f(worker); auto seed = f.Grant(20); auto operation = f.Operation(21);
  std::stop_source stop; auto control = Control(stop);
  control.wait_deadline = Clock::now() + std::chrono::seconds(2);
  const auto hazard = f.Hazard(21);
  ParkProbe park; UnlockProbe delivery;
  c::RuntimeSemaphoreAcquireResult result;
  std::thread thread([&] { probe = &park; unlock_probe = &delivery;
    result = operation.Acquire(f.f.Request(worker,21),hazard,control);
    probe = nullptr; unlock_probe = nullptr;
  });
  park.Await(1);
  if (grant_wins) Check(seed.permit.Release(f.f.Request(worker)) == Code::released, "real credit before commit");
  else f.Close();
  Check(delivery.reached.try_acquire_for(std::chrono::seconds(5)), "actual terminal delivery paused after callback join");
  if (grant_wins) f.Close();
  else Check(seed.permit.Release(f.f.Request(worker)) == Code::released, "real credit only after close winner");
  const auto pending = f.owner->Snapshot();
  Check(pending.governor.waiters == 0 && pending.governor.wait_calls == 0 && pending.operation_ref_count > 0,
        "delivery remains retained after native waiter and callback retirement");
  Check(pending.governor.holders == (grant_wins ? 1u : 0u), "actual serialized grant winner");
  Check(!f.owner->Drain(Clock::now()).drained && f.domain->Collect() == S::ok &&
        f.domain->Snapshot().retained_payload_bytes > 0, "semaphore_destroy_inflight_delivery retains actual storage");
  stop.request_stop();
  if (grant_wins) std::this_thread::sleep_until(*control.wait_deadline);
  delivery.resume.release(); thread.join();
  Check(result.code == (grant_wins ? Code::granted : Code::closed),
        "semaphore_grant_commit_before_close or close_before_grant_commit");
  if (grant_wins) {
    Check(result.ok(), "semaphore_grant_before_late_terminal keeps exact owning result");
    Check(result.permit.Release(f.f.Request(worker,21)) == Code::released, "late delivered owning release");
  } else Check(!result.permit, "close winner never publishes a phantom grant");
  operation.Reset(); f.Finish();
}
void RetainedDeliveryFailure(bool worker) {
  RetainedFixture f(worker); auto operation = f.Operation(20); std::stop_source stop;
  try {
    auto result = operation.Acquire(f.f.Request(worker),f.Hazard(20),Control(stop));
    Check(result.ok() && f.owner->Snapshot().governor.holders == 1, "real committed but unaccepted delivery");
    throw std::bad_alloc();
  } catch (const std::bad_alloc&) {}
  Check(!f.owner->Snapshot().governor.holders && f.f.governor.Snapshot().released_reservation_count == 1,
        "semaphore_grant_delivery_failure uses actual owning cleanup");
  operation.Reset(); f.Finish();
}
void RetainedReleaseFailure(bool worker) {
  RetainedFixture f(worker); auto grant = f.Grant(20); auto operation = f.Operation(21);
  ParkProbe park; std::stop_source stop; const auto hazard = f.Hazard(21);
  c::RuntimeSemaphoreAcquireResult pending;
  std::thread waiter([&] { probe = &park;
    pending = operation.Acquire(f.f.Request(worker,21),hazard,Control(stop)); probe = nullptr;
  });
  park.Await(1); stop.request_stop(); waiter.join(); operation.Reset();
  Check(pending.code == Code::cancelled, "actual waiter gives native mutex binding");
  const auto bytes = f.f.manager.Snapshot().current_bytes;
  fail_lock_target = park.native_mutex;
  Check(grant.permit.Release(f.f.Request(worker)) == Code::synchronization_failed && grant.permit,
        "failed actual governor release preserves owning capability");
  Check(f.owner->Snapshot().governor.holders == 1 && f.f.manager.Snapshot().current_bytes == bytes,
        "failed native release preserves real unit and physical backing");
  f.Close(); Check(!f.owner->Drain(Clock::now()).drained, "release failure is not completed drain");
  Check(grant.permit.Release(f.f.Request(worker)) == Code::released, "actual release retry succeeds");
  f.Finish();
}
void RetainedInvalidRelease(bool worker) {
  RetainedFixture f(worker); auto grant = f.Grant(20);
  const auto bytes = f.f.manager.Snapshot().current_bytes;
  const auto expected = f.f.Request(worker);
  for (unsigned field = 0; field != 12; ++field) {
    auto wrong = expected;
    switch (field) {
      case 0: wrong.authority.database = Id(901); break;
      case 1: wrong.authority.incarnation = Id(902); break;
      case 2: wrong.authority.policy = Id(903); break;
      case 3: ++wrong.authority.policy_generation; break;
      case 4: wrong.profile = worker ? a::RuntimePermitProfile::queued_task : a::RuntimePermitProfile::worker_slot; break;
      case 5: wrong.task = Id(904); break;
      case 6: wrong.attempt = Id(905); break;
      case 7: wrong.worker = Id(906); break;
      case 8: wrong.semaphore = Id(907); break;
      case 9: ++wrong.semaphore_generation; break;
      case 10: ++wrong.quantity; break;
      case 11: ++wrong.lease_deadline_tick; break;
    }
    Check(grant.permit.Release(wrong) != Code::released && grant.permit,
          "every wrong binary release binding preserves capability");
    const auto diagnostic = grant.permit.Diagnostic();
    Check(diagnostic && diagnostic->registration.code == "diag.mga.concurrency.invalid_primitive_descriptor",
          "wrong release has canonical protected descriptor diagnostic");
    Check(f.owner->Snapshot().governor.holders == 1 && f.f.manager.Snapshot().current_bytes == bytes,
          "wrong release preserves actual resource and storage charge");
  }
  f.Close();
  Check(grant.permit.Release(expected) == Code::released && !grant.permit.Diagnostic(), "exact release after close");
  Check(grant.permit.Release(expected) == Code::no_grant && grant.permit.Diagnostic(),
        "duplicate release has diagnostic without retaining unit or storage");
  f.Finish();
}
void RetainedReferenceBounds(bool worker) {
  RetainedFixture f(worker,2); auto first = f.Operation(20); auto second = f.Operation(21);
  c::RuntimeSemaphoreOperation rejected;
  const auto bytes = f.f.manager.Snapshot().current_bytes;
  Check(f.owner->AcquireOperation(f.instance.semaphore,f.instance.generation,f.Hazard(22),rejected) == S::exhausted && !rejected,
        "selected operation bound refuses before new hazard publication");
  std::stop_source stop;
  auto result = first.Acquire(f.f.Request(worker),f.Hazard(20),Control(stop));
  Check(!result.ok() && result.memory_status == S::exhausted && !result.registered && result.Diagnostic(),
        "callback reference bound rejects without native registration or partial grant");
  Check(f.owner->Snapshot().operation_ref_count == 2 && f.domain->Snapshot().readers == 3 &&
        f.f.manager.Snapshot().current_bytes == bytes, "reference exhaustion preserves exact prior hazards and storage");
  second.Reset();
  auto moved = std::move(first);
  Check(!first && moved && f.owner->Snapshot().operation_ref_count == 1, "operation move transfers one actual reference");
  result = moved.Acquire(f.f.Request(worker),f.Hazard(20),Control(stop));
  Check(result.ok(), "release of one real reference permits callback admission");
  auto grant = std::move(result.permit);
  Check(!result.permit && grant && f.owner->Snapshot().governor.holders == 1,
        "grant move transfers ownership without resource credit");
  moved.Reset(); f.Close();
  Check(grant.Release(f.f.Request(worker)) == Code::released, "moved actual grant release");
  f.Finish();
}
void RetainedHazardBounds(bool worker) {
  RetainedFixture f(worker,16,2); auto operation = f.Operation(20); std::stop_source stop;
  const auto bytes = f.f.manager.Snapshot().current_bytes;
  const auto result = operation.Acquire(f.f.Request(worker),f.Hazard(20),Control(stop));
  Check(result.code == Code::allocation_failed && result.memory_status == S::exhausted && !result.registered && !result.permit,
        "actual shared hazard limit refuses grant before native admission");
  const auto snapshot = f.owner->Snapshot();
  Check(snapshot.fail_safe_releases == 1 && snapshot.last_failure &&
        snapshot.last_failure->registration.code == "diag.mga.concurrency.fail_safe_release",
        "failed actual hazard admission records its protected fail-safe occurrence");
  Check(snapshot.operation_ref_count == 1 && !snapshot.governor.holders && !snapshot.governor.wait_calls &&
        f.domain->Snapshot().readers == 2 && f.f.manager.Snapshot().current_bytes == bytes,
        "failed grant protection unwinds callback reference and preserves actual prior hazards");
  operation.Reset(); f.Finish();
}
void RetainedInitializationFailure(bool worker) {
  Fixture f(false); f.policy.worker_waiter_limit = f.policy.queue_waiter_limit = 2;
  Check(f.Bind() == Code::bound, "initialization failure selected owning governor");
  {
    m::MemorySafeRetirement domain(*f.resource,Id(80).bytes,2,4);
    Check(domain.Initialize() == S::ok, "initialization failure real domain");
    const auto bytes = f.manager.Snapshot().current_bytes;
    c::RuntimeSemaphoreOwner owner(domain);
    const auto instance = worker ? f.workers : f.queue;
    c::RuntimeSemaphoreDescriptor descriptor;
    descriptor.primitive_id = instance.semaphore; descriptor.generation = instance.generation;
    descriptor.owner_uuid = Id(81); descriptor.last_transition = Id(82);
    const auto initialize = [&] {
      return owner.Initialize(descriptor,{4},f.governor,f.policy.authority,
          worker ? a::RuntimePermitProfile::worker_slot : a::RuntimePermitProfile::queued_task,
          {Id(83).bytes,Id(84).bytes});
    };
    fail_init = true;
    const auto failed = initialize(); fail_init = false;
    Check(failed == S::construction_failed && !owner.Snapshot().initialized,
          "actual native condition construction failure prevents primitive publication");
    Check(domain.Collect() == S::ok && !domain.Snapshot().retained_payload_bytes &&
          f.manager.Snapshot().current_bytes == bytes, "construction failure returns actual payload charge");
    Check(initialize() == S::ok, "clean initialization retry after real native failure");
    Check(owner.Close(Id(85)) && owner.FenceAdmission() && owner.Drain(Clock::now()).drained,
          "retried primitive actual closed drain");
  }
  NoCalls(f); f.Empty();
}
void RetainedAllocationFailures(bool worker) {
  unsigned failures = 0, telemetry_successes = 0; bool complete = false;
  for (long position = 0; position != 2048 && !complete; ++position) {
    RetainedFixture f(worker,16,32,2); auto seed = f.Grant(20); auto operation = f.Operation(21);
    const auto request = f.f.Request(worker,21); const auto hazard = f.Hazard(21);
    const auto before = f.f.governor.Snapshot(); const auto physical = f.f.manager.Snapshot();
    std::stop_source stop;
    fault::hit = false; fault::remaining = position;
    auto result = operation.Acquire(request,hazard,Control(stop));
    const bool hit = fault::hit; fault::remaining = -1;
    if (!result.ok()) {
      ++failures;
      Check(hit && result.code == Code::allocation_failed && result.Diagnostic(), "actual fallible retained admission refuses atomically");
      Check(Same(before,f.f.governor.Snapshot()) && f.f.manager.Snapshot().current_bytes == physical.current_bytes &&
            f.domain->Snapshot().readers == 3 && f.owner->Snapshot().operation_ref_count == 1,
            "every failed allocation retains prior grant and unwinds new grant guard and callback reference");
    } else {
      Check(f.owner->Snapshot().governor.holders == 2 && f.domain->Snapshot().readers == 4 &&
            f.f.governor.Snapshot().created_reservation_count == before.created_reservation_count + 1,
            "successful result owns real second grant and storage guard");
      if (hit) {
        ++telemetry_successes;
        Check(f.f.manager.Snapshot().telemetry_truncation_count > physical.telemetry_truncation_count,
              "handled allocator telemetry failure remains fully owned actual success");
      }
      fault::hit = false; fault::remaining = 0;
      const auto released = result.permit.Release(request);
      const bool release_allocated = fault::hit; fault::remaining = -1;
      Check(released == Code::released && !release_allocated, "actual retained owning release remains allocation free");
    }
    complete = !hit;
    operation.Reset();
    Check(seed.permit.Release(f.f.Request(worker)) == Code::released, "prior exact grant remains releasable after all allocation positions");
    f.Finish();
  }
  Check(complete && failures > 0 && telemetry_successes > 0, "exhausted every actual allocation position including admitted telemetry-loss success");
}
void RetainedDrainFailure(bool worker, unsigned mode) {
  RetainedFixture f(worker); auto grant = f.Grant(20); f.Close();
  const auto bytes = f.f.manager.Snapshot().current_bytes;
  std::stop_source stop;
  ParkProbe fault; fault.fail = true;
  if (mode == 1) stop.request_stop();
  if (mode == 2) probe = &fault;
  const auto drained = f.owner->Drain(Clock::now() + (mode == 2 ? std::chrono::seconds(5) : std::chrono::seconds(0)),stop.get_token());
  probe = nullptr;
  Check(!drained.drained && drained.code == (mode == 0 ? Code::timed_out : mode == 1 ? Code::cancelled : Code::synchronization_failed),
        "actual incomplete drain terminal selected");
  const auto diagnostic = drained.Diagnostic();
  Check(diagnostic && diagnostic->registration.code == "diag.mga.concurrency.fail_safe_release",
        "drain failure is fail safe release not acquisition timeout");
  const auto snapshot = f.owner->Snapshot();
  Check(snapshot.last_failure && snapshot.fail_safe_releases == 1 && snapshot.governor.holders == 1,
        "actual protected drain occurrence recorded once with retained holder");
  Check(f.owner->Snapshot().fail_safe_releases == 1 && f.domain->Collect() == S::ok &&
        f.f.manager.Snapshot().current_bytes == bytes, "inspection is not a new failure and collection cannot release storage");
  Check(grant.permit.Release(f.f.Request(worker)) == Code::released, "real release after drain failure");
  f.Finish();
}
void RetainedPhysicalReleaseFailure(bool worker) {
  RetainedFixture f(worker); auto grant = f.Grant(20); f.Close();
  Check(grant.permit.Release(f.f.Request(worker)) == Code::released && f.owner->Drain(Clock::now()).drained,
        "actual resource drain before physical memory retirement");
  f.owner.reset();
  const auto bytes = f.f.manager.Snapshot().current_bytes;
  m::MemoryFailureInjectionConfiguration config{m::MakeMemoryFailureInjectionTestGuard(),false,{},{},{}};
  config.fixture_enabled = true;
  config.fixture_name = "retained semaphore physical release";
  config.evidence_note = "real allocator failure after primitive destructor retains physical charge";
  m::MemoryFailureInjectionRule rule;
  rule.rule_id = "retained semaphore payload release";
  rule.purpose = "safe retirement payload";
  rule.callsite = "core.memory.reservation_backed_resource";
  config.rules.push_back(rule);
  Check(f.f.manager.allocator()->EnableAllocationFailureInjection(std::move(config)).ok(), "arm actual physical release fault");
  const auto collected = f.domain->Collect();
  Check(f.f.manager.allocator()->DisableAllocationFailureInjection().ok(), "disarm actual physical release fault");
  Check(collected == S::release_failed && f.domain->Snapshot().retired == 1 &&
        f.domain->Snapshot().retained_payload_bytes > 0 && f.f.manager.Snapshot().current_bytes == bytes,
        "local semaphore drain never fabricates physical release receipt");
  f.Finish();
}
// Fresh executable children test the actual owner destructor. The observer
// examines the protected occurrence and real charges before process exit;
// OS cleanup is deliberately not interpreted as an owning release receipt.
RetainedFixture* destruction_fixture = nullptr;
c::RuntimeSemaphoreOwner* destruction_owner = nullptr;
std::uint64_t destruction_bytes = 0;
bool destruction_delivery = false;
[[noreturn]] void RetainedDestructionChild(bool worker, bool delivery) {
  alarm(20);
  std::set_terminate([] {
    if (!destruction_fixture || !destruction_owner) _exit(87);
    const auto snapshot = destruction_owner->Snapshot();
    const auto& diagnostic = snapshot.last_failure;
    const bool ownership = destruction_delivery
        ? snapshot.governor.holders == 0 && snapshot.governor.waiters == 0 &&
          snapshot.governor.wait_calls == 0 && snapshot.operation_ref_count > 0
        : snapshot.governor.holders == 1;
    const bool retained = ownership && diagnostic && snapshot.fail_safe_releases == 1 &&
        diagnostic->registration.code == "diag.mga.concurrency.fail_safe_release" &&
        diagnostic->primitive_class == "semaphore_budget" && diagnostic->protected_data &&
        diagnostic->primitive_id == destruction_fixture->instance.semaphore.bytes &&
        diagnostic->thread_or_task_id == Id(83).bytes &&
        destruction_fixture->domain->Snapshot().retained_payload_bytes > 0 &&
        destruction_fixture->f.manager.Snapshot().current_bytes == destruction_bytes;
    _exit(retained ? 86 : 87);
  });
  RetainedFixture f(worker); auto grant = f.Grant(20);
  c::RuntimeSemaphoreOperation operation;
  c::RuntimeSemaphoreAcquireResult pending;
  ParkProbe park; UnlockProbe delivery_pause; std::stop_source stop;
  std::thread waiter;
  if (delivery) {
    operation = f.Operation(21); const auto hazard = f.Hazard(21);
    waiter = std::thread([&,hazard] {
      probe = &park; unlock_probe = &delivery_pause;
      pending = operation.Acquire(f.f.Request(worker,21),hazard,Control(stop));
      probe = nullptr; unlock_probe = nullptr;
    });
    park.Await(1); f.Close();
    Check(delivery_pause.reached.try_acquire_for(std::chrono::seconds(5)), "real in-flight result before unsafe destruction");
    Check(grant.permit.Release(f.f.Request(worker)) == Code::released, "no live grant can mask delivery reference");
  } else f.Close();
  destruction_fixture = &f;
  // optional may clear its engaged flag during reset; keep the actual object
  // address while its destructor body is still executing.
  destruction_owner = &*f.owner;
  destruction_bytes = f.f.manager.Snapshot().current_bytes;
  destruction_delivery = delivery;
  f.owner.reset();
  _exit(89); // Never allow a joinable-thread destructor to masquerade as owner failure.
}
void RetainedUnsafeDestruction() {
  for (const char* profile : {"queue","worker"}) for (const char* mode : {"grant","delivery"}) {
    const auto child = fork();
    Check(child >= 0, "launch fresh retained-owner destruction executable");
    if (child == 0) {
      execl("/proc/self/exe","runtime_permit_wait_gate","--unsafe-owner",profile,mode,static_cast<char*>(nullptr));
      _exit(90);
    }
    int status = 0; pid_t waited;
    do { waited = waitpid(child,&status,0); } while (waited < 0 && errno == EINTR);
    Check(waited == child && WIFEXITED(status) && WEXITSTATUS(status) == 86,
          "unsafe owner destruction records protected failure before retaining actual storage");
  }
}
void RetainedWakeTerminal(bool worker, bool cancellation) {
  RetainedFixture f(worker); auto seed = f.Grant(20); auto operation = f.Operation(21);
  ParkProbe park; WakeProbe wake; std::stop_source stop;
  auto control = Control(stop); control.wait_deadline = Clock::now() + std::chrono::seconds(2);
  const auto hazard = f.Hazard(21);
  c::RuntimeSemaphoreAcquireResult result;
  std::thread waiter([&] { probe = &park; wake_probe = &wake;
    result = operation.Acquire(f.f.Request(worker,21),hazard,control);
    probe = nullptr; wake_probe = nullptr;
  });
  park.Await(1); wake.condition = park.native_condition;
  // Only the actual owning credit's native broadcast arms the post-wake pause.
  // Earlier spurious wakes still execute the ordinary real predicate loop.
  broadcast_probe = &wake;
  Check(seed.permit.Release(f.f.Request(worker)) == Code::released, "real capacity returned before wake selection");
  broadcast_probe = nullptr;
  Check(wake.reached.try_acquire_for(std::chrono::seconds(5)), "actual credit wake reacquired governor mutex");
  std::thread canceller;
  LockProbe callback_attempt; callback_attempt.mutex = park.native_mutex;
  if (cancellation) {
    canceller = std::thread([&] { lock_probe = &callback_attempt; stop.request_stop(); lock_probe = nullptr; });
    Check(callback_attempt.attempted.try_acquire_for(std::chrono::seconds(5)) && stop.stop_requested(),
          "actual cancellation published before its blocked wake callback");
  } else std::this_thread::sleep_until(*control.wait_deadline);
  wake.resume.release(); waiter.join(); if (canceller.joinable()) canceller.join();
  Check(result.code == (cancellation ? Code::cancelled : Code::timed_out) && !result.permit && result.registered,
        "available credit cannot defeat observed cancellation or expired deadline after wake");
  const auto snapshot = f.owner->Snapshot();
  Check(!snapshot.governor.holders && !snapshot.governor.wait_calls &&
        f.f.governor.Snapshot().created_reservation_count == 1,
        "terminal retry publishes no second issuance or capacity debit");
  operation.Reset(); f.Finish();
}
void RetainedCloseParkRace(bool worker) {
  RetainedFixture f(worker); auto seed = f.Grant(20); auto operation = f.Operation(21);
  ParkProbe park; park.hold_first = true;
  std::stop_source stop; const auto hazard = f.Hazard(21);
  c::RuntimeSemaphoreAcquireResult result;
  std::thread waiter([&] { probe = &park;
    result = operation.Acquire(f.f.Request(worker,21),hazard,Control(stop)); probe = nullptr;
  });
  park.Await(1);
  LockProbe close_attempt; close_attempt.mutex = park.native_mutex;
  bool closed = false;
  std::thread closer([&] { lock_probe = &close_attempt; closed = f.owner->Close(Id(88)); lock_probe = nullptr; });
  Check(close_attempt.attempted.try_acquire_for(std::chrono::seconds(5)), "actual close contends at native park boundary");
  park.resume.release(); closer.join(); waiter.join();
  Check(closed && result.code == Code::closed && !result.permit, "semaphore_close_park_race has no lost wake");
  operation.Reset();
  Check(seed.permit.Release(f.f.Request(worker)) == Code::released, "park race preserves real seed release");
  f.Finish();
}
void NativeDrain(bool acquire_wait_enabled, unsigned terminal) {
  Fixture f(false);
  if (acquire_wait_enabled) Configure(f);
  else Check(f.Bind() == Code::bound, "immediate-only pool binding");
  auto worker = f.governor.AcquireRuntimePermit(f.Request(true));
  auto queue = f.governor.AcquireRuntimePermit(f.Request(false));
  Check(worker.ok() && queue.ok(), "drain retains both actual profile grants");
  std::stop_source stop;
  auto control = Control(stop);
  auto before = f.governor.InspectRuntimePermitInstance(f.policy.authority,
      a::RuntimePermitProfile::worker_slot, f.workers);
  Check(before.code == Code::bound && before.holders == 1 && !before.closed,
        "fixed inspector reads exact worker instance");
  auto open = f.governor.DrainRuntimePermitInstance(f.policy.authority,
      a::RuntimePermitProfile::worker_slot, f.workers, control);
  Check(!open.drained && open.code == Code::invalid_binding, "open pool cannot authorize final zeros");
  Check(f.governor.CloseRuntimePermitInstance(f.policy.authority,
      a::RuntimePermitProfile::worker_slot, f.workers) == Code::closed, "close drain profile");
  auto wrong = f.workers; ++wrong.generation;
  auto invalid = f.governor.DrainRuntimePermitInstance(f.policy.authority,
      a::RuntimePermitProfile::worker_slot, wrong, control);
  Check(!invalid.drained && invalid.code == Code::invalid_binding, "drain binding mismatch");
  auto expired = f.governor.DrainRuntimePermitInstance(f.policy.authority,
      a::RuntimePermitProfile::worker_slot, f.workers, {{}, Clock::now()}, "retain live holder");
  Check(!expired.drained && expired.code == Code::timed_out && expired.reason() == "retain live holder",
        "drain expiry preserves exact charge and reason");
  ParkProbe p; p.fail = terminal == 2;
  if (terminal == 1) control.wait_deadline.reset();
  a::RuntimePermitDrainResult result;
  std::thread observer([&] { probe = &p;
    result = f.governor.DrainRuntimePermitInstance(f.policy.authority,
        a::RuntimePermitProfile::worker_slot, f.workers, control);
    probe = nullptr;
  });
  p.Await(1);
  if (terminal == 0) Check(worker.permit.Release(f.Request(true)) == Code::released, "actual drain credit");
  else if (terminal == 1) stop.request_stop();
  observer.join();
  const Code expected[] = {Code::closed,Code::cancelled,Code::synchronization_failed};
  Check(result.code == expected[terminal] && result.drained == (terminal == 0), "exact local drain disposition");
  auto other = f.governor.InspectRuntimePermitInstance(f.policy.authority,
      a::RuntimePermitProfile::queued_task, f.queue);
  Check(other.code == Code::bound && other.holders == 1, "other profile unaffected by local drain");
  if (terminal != 0) {
    auto retained = f.governor.InspectRuntimePermitInstance(f.policy.authority,
        a::RuntimePermitProfile::worker_slot, f.workers);
    Check(retained.closed && retained.holders == 1, "failed drain cannot forgive capacity");
    Check(worker.permit.Release(f.Request(true)) == Code::released, "failed drain retains owning release");
  }
  auto done = f.governor.DrainRuntimePermitInstance(f.policy.authority,
      a::RuntimePermitProfile::worker_slot, f.workers, {{}, Clock::now()}, "already quiescent");
  Check(done.drained && done.code == Code::closed, "closed stable zero is immediately drained");
  Check(queue.permit.Release(f.Request(false)) == Code::released, "other profile owner release");
  NoCalls(f); f.Empty();
}
}
int main(int argc, char** argv) {
  if (argc == 4 && std::string_view(argv[1]) == "--unsafe-owner")
    RetainedDestructionChild(std::string_view(argv[2]) == "worker",std::string_view(argv[3]) == "delivery");
  static_assert(a::RuntimePermitSequenceAvailable(UINT64_MAX-1));
  static_assert(!a::RuntimePermitSequenceAvailable(UINT64_MAX));
  for (bool worker : {false,true}) for (bool spurious : {false,true}) ReleaseWake(worker,spurious);
  for (unsigned mode = 0; mode != 4; ++mode) Terminal(mode);
  BoundsAndValidation(); ConstructionAndUnbound();
  for (bool worker : {false,true}) for (unsigned route = 0; route != 4; ++route) LegacyCredit(worker,route);
  LegacyCredit(true,1,true); AvailableTerminalPrecedence();
  for (bool enabled : {false,true}) for (unsigned terminal = 0; terminal != 3; ++terminal)
    NativeDrain(enabled,terminal);
  RetainedGrantLifetime(false); RetainedGrantLifetime(true);
  RetainedUnsafeDestruction();
  for (bool worker : {false,true}) {
    RetainedPendingClose(worker); RetainedCloseAvailable(worker); RetainedIdempotentClose(worker);
    RetainedLateDelivery(worker,false); RetainedLateDelivery(worker,true);
    RetainedDeliveryFailure(worker); RetainedReleaseFailure(worker);
    RetainedInvalidRelease(worker); RetainedPhysicalReleaseFailure(worker);
    RetainedReferenceBounds(worker); RetainedHazardBounds(worker); RetainedInitializationFailure(worker);
    RetainedAllocationFailures(worker);
    for (unsigned mode = 0; mode != 3; ++mode) RetainedDrainFailure(worker,mode);
    RetainedWakeTerminal(worker,false); RetainedWakeTerminal(worker,true); RetainedCloseParkRace(worker);
  }
  std::cout << "runtime permit wait gate PASS " << checks << " checks\n";
}

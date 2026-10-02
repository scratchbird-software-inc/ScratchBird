// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "runtime_permit_fixture.hpp"
#include <cerrno>
#include <semaphore>

namespace {
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
extern "C" int __wrap_pthread_cond_wait(pthread_cond_t* c, pthread_mutex_t* m) {
  if (probe) { probe->Enter(c,m); if (probe->fail) return EINVAL; }
  return __real_pthread_cond_wait(c,m);
}
extern "C" int __wrap_pthread_cond_timedwait(pthread_cond_t* c, pthread_mutex_t* m,
                                            const timespec* deadline) {
  if (probe) { probe->Enter(c,m); if (probe->fail) return EINVAL; }
  return __real_pthread_cond_timedwait(c,m,deadline);
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
int main() {
  static_assert(a::RuntimePermitSequenceAvailable(UINT64_MAX-1));
  static_assert(!a::RuntimePermitSequenceAvailable(UINT64_MAX));
  for (bool worker : {false,true}) for (bool spurious : {false,true}) ReleaseWake(worker,spurious);
  for (unsigned mode = 0; mode != 4; ++mode) Terminal(mode);
  BoundsAndValidation(); ConstructionAndUnbound();
  for (bool worker : {false,true}) for (unsigned route = 0; route != 4; ++route) LegacyCredit(worker,route);
  LegacyCredit(true,1,true); AvailableTerminalPrecedence();
  std::cout << "runtime permit wait gate PASS " << checks << " checks\n";
}

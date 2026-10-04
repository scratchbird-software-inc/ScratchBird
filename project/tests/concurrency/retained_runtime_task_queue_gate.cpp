// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "retained_runtime_task_queue.hpp"
#include "runtime_permit_fixture.hpp"
#include <cerrno>

#if defined(SB_QUEUE_RETAINED_FAULTS)
thread_local bool fail_queue_drain_wait = false;
thread_local std::latch* queue_drain_park = nullptr;
extern "C" int __real_pthread_cond_timedwait(pthread_cond_t*, pthread_mutex_t*, const timespec*);
extern "C" int __wrap_pthread_cond_timedwait(pthread_cond_t* condition, pthread_mutex_t* mutex,
                                            const timespec* deadline) {
  if (std::exchange(fail_queue_drain_wait, false)) return EINVAL;
  if (auto* parked = std::exchange(queue_drain_park, nullptr)) parked->count_down();
  return __real_pthread_cond_timedwait(condition, mutex, deadline);
}
#endif
namespace r = scratchbird::core::runtime;
using Q = r::NativeTaskQueueCode;
using S = m::SafeRetirementStatus;
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;
r::NativeTaskQueueBinding Binding(const Fixture& f) {
  return {Id(200), Id(201), Id(1), Id(202), 17, 2, f.policy.authority, f.queue};
}
r::NativeTaskQueueKey Key(const Fixture& f, unsigned task = 20) {
  const auto request = f.Request(false, task);
  return {{{request.task, Id(1), Id(201), Id(202), 19}, Id(201)}, request.attempt, Id(200), 17};
}
m::SafeRetirementHazard Hazard(unsigned value) { return {Id(value).bytes, Id(999).bytes}; }
S Acquire(r::RetainedRuntimeTaskQueueOwner& owner, unsigned hazard, r::RetainedRuntimeTaskQueueOperation& op) {
  return owner.AcquireOperation(Id(200), Id(202), 17, Hazard(hazard), op);
}
void Collect(m::MemorySafeRetirement& domain) {
  Check(domain.Collect() == S::ok, "actual retired queue collected");
  const auto snapshot = domain.Snapshot();
  Check(!snapshot.readers && !snapshot.retired && !snapshot.published && !snapshot.retained_payload_bytes,
        "no retained queue payload or hazards");
  domain.Close();
  Check(domain.Drain(Clock::now() + 1s) == S::ok, "actual domain drain");
}

int main() {
#if defined(__linux__)
  for (unsigned stage = 0; stage != 3; ++stage) {
    const auto child = fork();
    Check(child >= 0, "retained lifetime child created");
    if (child == 0) {
      Fixture f;
      m::MemorySafeRetirement domain(*f.resource, Id(900).bytes, 4, 16);
      if (domain.Initialize() != S::ok) std::_Exit(74);
      {
        r::RetainedRuntimeTaskQueueOwner owner(domain);
        if (owner.Initialize(Binding(f), {2}, f.governor, *f.metadata, Hazard(901)) != S::ok)
          std::_Exit(74);
        if (stage > 0 && !owner.Close()) std::_Exit(74);
        if (stage > 1 && !owner.FenceAdmission()) std::_Exit(74);
        std::set_terminate([] { std::_Exit(73); });
      }
      std::_Exit(75);
    }
    int status = 0;
    Check(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 73,
          "owner destruction requires close, fence and observed drain");
  }
#endif
  {
    Fixture f;
    {
      m::MemorySafeRetirement domain(*f.resource, Id(900).bytes, 4, 16);
      Check(domain.Initialize() == S::ok, "move domain initialized");
      {
        r::RetainedRuntimeTaskQueueOwner owner(domain);
        Check(owner.Initialize(Binding(f), {2}, f.governor, *f.metadata, Hazard(901)) == S::ok,
              "move owner initialized");
        r::RetainedRuntimeTaskQueueOperation first, second;
        Check(Acquire(owner, 902, first) == S::ok && Acquire(owner, 903, second) == S::ok,
              "two independent actual hazards acquired");
        Check(Acquire(owner, 904, first) == S::invalid_request,
              "nonempty output cannot overwrite retained reference");
        first = std::move(second);
        Check(first && !second && owner.Snapshot().operation_references == 1 && domain.Snapshot().readers == 2,
              "move assignment releases previous hazard and transfers exactly one");
        Check(owner.Close() && owner.FenceAdmission(), "move owner fenced");
        first.Reset();
        first.Reset();
        Check(owner.Drain(Clock::now()).drained, "repeated reset does not underflow reference count");
      }
      Collect(domain);
    }
    f.Empty();
  }
  {
    Fixture f;
    {
      m::MemorySafeRetirement domain(*f.resource, Id(900).bytes, 4, 16);
      Check(domain.Initialize() == S::ok, "real governed retirement domain initialized");
      {
        r::RetainedRuntimeTaskQueueOwner owner(domain);
        Check(!owner.Close() && !owner.FenceAdmission() && !owner.Drain(Clock::now()).drained,
              "uninitialized owner cannot claim drain");
        Check(owner.Initialize(Binding(f), {1}, f.governor, *f.metadata, Hazard(901)) == S::ok,
              "actual retained queue initialized");
        Check(!owner.FenceAdmission(), "admission cannot retire before close");
        r::RetainedRuntimeTaskQueueOperation op, excess;
        Check(owner.AcquireOperation(Id(200), Id(203), 17, Hazard(902), op) == S::invalid_request,
              "old open incarnation cannot acquire operation");
        Check(owner.AcquireOperation(Id(200), Id(202), 18, Hazard(902), op) == S::invalid_request,
              "wrong queue generation cannot acquire operation");
        Check(Acquire(owner, 902, op) == S::ok && bool(op), "real operation guard acquired");
        Check(Acquire(owner, 903, excess) == S::exhausted && !excess, "selected operation reference bound");
        Check(domain.Snapshot().readers == 2, "owner and operation have real domain hazards");
        auto credit = f.governor.AcquireRuntimePermit(f.Request(false));
        Check(credit.ok() && op.Push(Key(f), credit.permit).code == Q::inserted && !credit.permit,
              "retained operation publishes real queue credit");
        auto provisional = f.governor.AcquireRuntimePermit(f.Request(false, 21));
        Check(provisional.ok(), "independent provisional credit remains externally owned");
        Check(owner.Close() && owner.FenceAdmission(), "close then retire admission");
        Check(Acquire(owner, 903, excess) == S::closed && !excess, "late acquisition refused after fence");
        Check(op.Push(Key(f, 21), provisional.permit).code == Q::closed && bool(provisional.permit),
              "issued operation cannot bypass queue close");
        const auto before = domain.Snapshot().retained_payload_bytes;
        const auto pending = owner.Drain(Clock::now());
        Check(!pending.drained && pending.status == S::timed_out && pending.pending == Key(f) &&
              pending.operation_references == 1, "timed-out drain reports exact retained work");
        Check(domain.Collect() == S::ok && domain.Snapshot().retained_payload_bytes == before,
              "collector cannot free queue through outstanding hazards");
        r::RetainedRuntimeTaskQueueOperation moved(std::move(op));
        Check(!op && bool(moved) && owner.Snapshot().operation_references == 1,
              "operation move transfers one reference without duplication");
        Check(moved.Remove(Key(f)).code == Q::removed, "existing retained operation removes after fence");
        Check(!owner.Drain(Clock::now()).drained, "empty entries do not discharge live operation reference");
        moved.Reset();
        const auto complete = owner.Drain(Clock::now());
        Check(complete.drained && complete.status == S::ok && !complete.pending,
              "actual local queue drain after operation release");
        Check(f.governor.Snapshot().active.backlog_items == 1, "local queue drain is not global governor drain");
        Check(provisional.permit.Release(f.Request(false, 21)) == Code::released,
              "external provisional grant released by its actual owner");
        Check(domain.Snapshot().readers == 1 && domain.Snapshot().retired == 1,
              "owner still retains retired payload until owner destruction");
      }
      Collect(domain);
    }
    f.Empty();
  }
  for (bool cancelled : {false, true}) {
    Fixture f;
    {
      m::MemorySafeRetirement domain(*f.resource, Id(900).bytes, 4, 16);
      Check(domain.Initialize() == S::ok, "drain domain initialized");
      {
        r::RetainedRuntimeTaskQueueOwner owner(domain);
        Check(owner.Initialize(Binding(f), {2}, f.governor, *f.metadata, Hazard(901)) == S::ok,
              "drain owner initialized");
        r::RetainedRuntimeTaskQueueOperation op;
        Check(Acquire(owner, 902, op) == S::ok, "drain operation acquired");
        auto credit = f.governor.AcquireRuntimePermit(f.Request(false));
        Check(credit.ok() && op.Push(Key(f), credit.permit).code == Q::inserted, "actual pending entry");
        op.Reset();
        Check(owner.RemovePending(Key(f)).code == Q::invalid_binding, "owner cleanup requires admission close");
        Check(owner.Close() && owner.FenceAdmission(), "pending entry fenced without operation");
        std::stop_source stop;
        if (cancelled) stop.request_stop();
#if defined(SB_QUEUE_RETAINED_FAULTS)
        fail_queue_drain_wait = !cancelled;
        const auto failed = owner.Drain(Clock::now() + 10s, stop.get_token());
        Check(!failed.drained && failed.status == (cancelled ? S::cancelled : S::wait_failed),
              "cancel or actual native wait failure does not claim drained");
#else
        const auto failed = owner.Drain(Clock::now(), stop.get_token());
        Check(!failed.drained && failed.status == (cancelled ? S::cancelled : S::timed_out), "failed drain retained");
#endif
        Check(failed.pending == Key(f) && f.governor.Snapshot().active.backlog_items == 1 &&
              domain.Snapshot().retained_payload_bytes > 0, "failed drain preserves queue memory and credit");
        Check(owner.RemovePending(Key(f)).code == Q::removed, "explicit owner cleanup works after operation fencing");
        Check(owner.Drain(Clock::now()).drained, "successful retry after actual cleanup");
      }
      Collect(domain);
    }
    f.Empty();
  }
#if defined(SB_QUEUE_RETAINED_FAULTS)
  for (bool cancel_parked : {false, true}) {
    Fixture f;
    {
      m::MemorySafeRetirement domain(*f.resource, Id(900).bytes, 4, 16);
      Check(domain.Initialize() == S::ok, "park domain initialized");
      {
        r::RetainedRuntimeTaskQueueOwner owner(domain);
        Check(owner.Initialize(Binding(f), {2}, f.governor, *f.metadata, Hazard(901)) == S::ok,
              "park owner initialized");
        r::RetainedRuntimeTaskQueueOperation op;
        Check(Acquire(owner, 902, op) == S::ok && owner.Close() && owner.FenceAdmission(), "live operation at drain fence");
        std::latch parked(1);
        std::stop_source stop;
        r::RetainedTaskQueueDrainResult result;
        std::thread waiter([&] {
          queue_drain_park = &parked;
          result = owner.Drain(Clock::now() + 10s, stop.get_token());
        });
        parked.wait();
        if (cancel_parked) stop.request_stop(); else op.Reset();
        waiter.join();
        Check(cancel_parked ? (!result.drained && result.status == S::cancelled) : result.drained,
              "actual parked drain wakes on cancellation or operation release");
        op.Reset();
        Check(owner.Drain(Clock::now()).drained, "joined waiter permits final successful drain");
      }
      Collect(domain);
    }
    f.Empty();
  }
#endif
  {
    Fixture f;
    {
      m::MemorySafeRetirement domain(*f.resource, Id(900).bytes, 4, 16);
      Check(domain.Initialize() == S::ok, "failed construction domain initialized");
      r::RetainedRuntimeTaskQueueOwner owner(domain);
      Check(owner.Initialize(Binding(f), {1}, f.governor, *f.metadata, {}) != S::ok,
            "invalid owner hazard never activates native queue");
      Check(!owner.Snapshot().initialized, "failed owner protection publishes no owner");
      Collect(domain);
    }
    f.Empty();
  }
  std::cout << "PASS retained runtime task queue " << checks << " checks; local lifetime only\n";
}

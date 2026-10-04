// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "native_runtime_task_queue.hpp"
#include "runtime_permit_fixture.hpp"
#include <atomic>
#include <cerrno>

#if defined(SB_QUEUE_NATIVE_FAULTS)
thread_local unsigned queue_fail_lock = 0;
thread_local std::latch* queue_lock_attempt = nullptr;
extern "C" int __real_pthread_mutex_lock(pthread_mutex_t*);
extern "C" int __wrap_pthread_mutex_lock(pthread_mutex_t* mutex) {
  if (auto* arrived = std::exchange(queue_lock_attempt, nullptr)) arrived->count_down();
  if (queue_fail_lock && --queue_fail_lock == 0) return EINVAL;
  return __real_pthread_mutex_lock(mutex);
}
#endif
namespace r = scratchbird::core::runtime;
using Q = r::NativeTaskQueueCode;
r::NativeTaskQueueBinding Binding(const Fixture& f, unsigned depth = 2) {
  return {Id(200), Id(201), Id(1), Id(202), 17, depth, f.policy.authority, f.queue};
}
r::NativeTaskQueueKey Key(const Fixture& f, unsigned task = 20) {
  const auto request = f.Request(false, task);
  return {{{request.task, Id(1), Id(201), Id(202), 19}, Id(201)},
          request.attempt, Id(200), 17};
}
void Release(a::RuntimePermitGrant& permit) {
  Check(bool(permit), "caller still owns provisional credit");
  const auto binding = permit.view()->binding;
  Check(permit.Release(binding) == Code::released, "real provisional credit release");
}

int main() {
#if defined(__linux__)
  // Intentional unsafe destruction must not silently discard queue ownership.
  // Child-only terminate handler is installed immediately before that destructor.
  for (bool populated : {false, true}) {
    const auto child = fork();
    Check(child >= 0, "native lifetime child created");
    if (child == 0) {
      Fixture f;
      {
        r::NativeRuntimeTaskQueue queue(Binding(f), f.governor, *f.metadata);
        if (populated) {
          auto credit = f.governor.AcquireRuntimePermit(f.Request(false));
          if (!credit.ok() || queue.Push(Key(f), credit.permit).code != Q::inserted ||
              queue.Close() != Q::closed) std::_Exit(74);
        }
        std::set_terminate([] { std::_Exit(73); });
      }
      std::_Exit(75);
    }
    int status = 0;
    Check(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 73,
          "unclosed or undrained native owner fails fast");
  }
#endif
  {
    Fixture f;
    r::NativeRuntimeTaskQueue queue(Binding(f), f.governor, *f.metadata);
    Check(queue.BindStatus() == Q::bound && queue.Front().code == Q::empty, "real queue initialized empty");
    auto one = f.governor.AcquireRuntimePermit(f.Request(false, 20));
    auto two = f.governor.AcquireRuntimePermit(f.Request(false, 21));
    Check(one.ok() && two.ok() && one.permit.IssuedBy(f.governor), "real queue credits and issuer provenance");
    const auto first = Key(f, 20), second = Key(f, 21);
    Check(queue.Push(first, one.permit).code == Q::inserted && !one.permit, "queue takes exact credit");
    Check(queue.Push(second, two.permit).code == Q::inserted && !two.permit, "second queue insertion");
    Check(queue.Front().key == first, "FIFO first observation");
    Check(f.governor.Snapshot().active.backlog_items == 2, "queued entries remain charged");
    auto stale = first; ++stale.task.identity.runtime_generation;
    Check(queue.Remove(stale).code == Q::stale_identity, "stale generation does not remove");
    stale = first; ++stale.queue_generation;
    Check(queue.Remove(stale).code == Q::invalid_binding, "stale queue incarnation does not remove");
#if defined(SB_QUEUE_NATIVE_FAULTS)
    queue_fail_lock = 2; // Queue predicate locks; actual governor release lock fails.
    const auto failed = queue.Remove(first);
    Check(failed.code == Q::release_failed && failed.permit_code == Code::synchronization_failed,
          "native release failure preserves entry and actual capability");
    Check(queue.Front().key == first && f.governor.Snapshot().active.backlog_items == 2,
          "failed release changes neither queue nor usage");
    queue_fail_lock = 1;
    Check(queue.Front().code == Q::synchronization_failed, "predicate failure is not empty queue");
#endif
    Check(queue.Close() == Q::closed && queue.Close() == Q::closed, "idempotent queue admission fence");
    Check(queue.Front().key == first && f.governor.Snapshot().active.backlog_items == 2,
          "close retains all existing ownership");
    Check(queue.Remove(first).code == Q::removed, "remove first and release once");
    Check(queue.Remove(first).code == Q::not_found, "duplicate removal cannot uncharge twice");
    Check(queue.Front().key == second && f.governor.Snapshot().active.backlog_items == 1, "FIFO advance");
    Check(queue.Remove(second).code == Q::removed && queue.Front().code == Q::closed, "closed queue drained");
    f.Empty();
  }
  {
    Fixture f;
    {
      r::NativeRuntimeTaskQueue queue(Binding(f, 1), f.governor, *f.metadata);
      auto one = f.governor.AcquireRuntimePermit(f.Request(false, 20));
      auto two = f.governor.AcquireRuntimePermit(f.Request(false, 21));
      Check(one.ok() && two.ok(), "two real credits for smaller per-queue bound");
      fault::remaining = 0; fault::hit = false;
      const auto failed = queue.Push(Key(f), one.permit);
      fault::remaining = -1;
      Check(fault::hit && failed.code == Q::allocation_failed && bool(one.permit), "failed allocation leaves credit with caller");
      Check(queue.Front().code == Q::empty && f.governor.Snapshot().active.backlog_items == 2,
            "failed allocation publishes no entry or uncharge");
      Check(queue.Push(Key(f), one.permit).code == Q::inserted, "same actual credit retries after allocation failure");
      Check(queue.Push(Key(f, 21), two.permit).code == Q::full && bool(two.permit), "selected depth bounds real queue");
      Check(queue.Close() == Q::closed, "close populated smaller queue");
      Check(queue.Push(Key(f, 21), two.permit).code == Q::closed && bool(two.permit), "closed queue retains rejected caller credit");
      Release(two.permit);
      Check(queue.Remove(Key(f)).code == Q::removed, "drain smaller queue");
    }
    f.Empty();
  }
  {
    Fixture f;
    {
      r::NativeRuntimeTaskQueue queue(Binding(f), f.governor, *f.metadata);
      auto one = f.governor.AcquireRuntimePermit(f.Request(false));
      const auto refused_duplicate = f.governor.AcquireRuntimePermit(f.Request(false));
      Check(one.ok() && refused_duplicate.code == Code::invalid_binding,
            "real governor prevents duplicate grant for same task attempt");
      Check(queue.Push(Key(f), one.permit).code == Q::inserted, "one canonical queue entry");
      Check(queue.Push(Key(f), one.permit).code == Q::duplicate && !one.permit,
            "duplicate notification observes existing entry without another credit");
      auto next_request = f.Request(false); next_request.attempt = Id(800);
      auto next = f.governor.AcquireRuntimePermit(next_request);
      Check(next.ok(), "different attempt has real provisional credit");
      auto changed = Key(f); changed.attempt_uuid = next_request.attempt;
      Check(queue.Push(changed, next.permit).code == Q::conflict, "new attempt cannot replace pending old attempt");
      ++changed.task.identity.runtime_generation;
      Check(queue.Push(changed, next.permit).code == Q::conflict, "conflicting task generation not silently replaced");
      Release(next.permit);
      Check(f.governor.Snapshot().active.backlog_items == 1, "conflicting provisional credit separately returned");
      Check(queue.Close() == Q::closed && queue.Remove(Key(f)).code == Q::removed, "duplicate case drained");
    }
    f.Empty();
  }
  {
    Fixture f, other; // Same binary governor label/bindings, different real ledger.
    {
      r::NativeRuntimeTaskQueue queue(Binding(f), f.governor, *f.metadata);
      auto foreign = other.governor.AcquireRuntimePermit(other.Request(false));
      Check(foreign.ok() && !foreign.permit.IssuedBy(f.governor), "UUID equality is not capability provenance");
      Check(queue.Push(Key(f), foreign.permit).code == Q::invalid_binding, "other ledger capability refused");
      Release(foreign.permit);
      auto worker = f.governor.AcquireRuntimePermit(f.Request(true));
      Check(worker.ok() && queue.Push(Key(f), worker.permit).code == Q::invalid_binding,
            "worker permit cannot fund a queue entry");
      Release(worker.permit);
      auto credit = f.governor.AcquireRuntimePermit(f.Request(false));
      Check(credit.ok(), "binding refusal cases actual grant");
      auto invalid = Key(f); invalid.queue_uuid = Id(500);
      Check(queue.Push(invalid, credit.permit).code == Q::invalid_binding, "foreign queue refused");
      invalid = Key(f); invalid.task.identity.open_generation_uuid = Id(500);
      Check(queue.Push(invalid, credit.permit).code == Q::invalid_binding, "foreign open refused");
      invalid = Key(f); invalid.attempt_uuid = Id(500);
      Check(queue.Push(invalid, credit.permit).code == Q::invalid_binding, "different grant attempt refused");
      Release(credit.permit);
      Check(queue.Close() == Q::closed, "empty refusal queue closed");
    }
    f.Empty(); other.Empty();
  }
  {
    Fixture f;
    {
      r::NativeRuntimeTaskQueue zero(Binding(f, 0), f.governor, *f.metadata);
      r::NativeRuntimeTaskQueue oversized(Binding(f, 3), f.governor, *f.metadata);
      Check(zero.BindStatus() == Q::bound && oversized.BindStatus() == Q::invalid_binding,
            "zero depth legal; selected depth cannot exceed actual capacity");
      auto credit = f.governor.AcquireRuntimePermit(f.Request(false));
      Check(credit.ok() && zero.Push(Key(f), credit.permit).code == Q::full, "zero depth admits no entries");
      Release(credit.permit);
      Check(zero.Close() == Q::closed, "zero depth closes");
    }
    f.Empty();
  }
  // Real concurrent close/submission. Bounded repetitions; barrier establishes
  // the race, queue mutex establishes order. No timing-based success oracle.
  for (unsigned run = 0; run != 32; ++run) {
    Fixture f;
    {
      r::NativeRuntimeTaskQueue queue(Binding(f), f.governor, *f.metadata);
      auto one = f.governor.AcquireRuntimePermit(f.Request(false, 20));
      auto two = f.governor.AcquireRuntimePermit(f.Request(false, 21));
      Check(one.ok() && two.ok(), "race actual provisional grants");
      std::barrier start(4);
      Q first{}, second{}, closed{};
      std::thread a([&] { start.arrive_and_wait(); first = queue.Push(Key(f, 20), one.permit).code; });
      std::thread b([&] { start.arrive_and_wait(); second = queue.Push(Key(f, 21), two.permit).code; });
      std::thread c([&] { start.arrive_and_wait(); closed = queue.Close(); });
      start.arrive_and_wait(); a.join(); b.join(); c.join();
      Check(closed == Q::closed, "race actual close observed");
      for (auto [result, permit, key] : {
          std::tuple{first, &one.permit, Key(f, 20)}, std::tuple{second, &two.permit, Key(f, 21)}}) {
        Check(result == Q::inserted || result == Q::closed, "submission wholly before or after fence");
        if (result == Q::inserted) {
          Check(!*permit && queue.Remove(key).code == Q::removed, "published entry owns credit until explicit removal");
        } else Release(*permit);
      }
      Check(queue.Front().code == Q::closed, "race queue fully drained after callers joined");
    }
    f.Empty();
  }
#if defined(SB_QUEUE_NATIVE_FAULTS)
  {
    Fixture f;
    {
      r::NativeRuntimeTaskQueue queue(Binding(f), f.governor, *f.metadata);
      auto credit = f.governor.AcquireRuntimePermit(f.Request(false));
      Check(credit.ok(), "paused publication owns real provisional credit");
      fault::AlignedPause pause;
      Q submitted{}, closed{};
      std::thread producer([&] {
        fault::aligned_pause = &pause;
        submitted = queue.Push(Key(f), credit.permit).code;
        fault::aligned_pause = nullptr;
        if (!pause.reached) pause.entered.count_down();
      });
      pause.entered.wait();
      Check(pause.reached, "publication paused inside actual governed allocation");
      std::latch close_attempt(1);
      std::atomic<bool> close_returned = false;
      std::thread closer([&] {
        queue_lock_attempt = &close_attempt;
        closed = queue.Close();
        close_returned = true;
      });
      close_attempt.wait();
      Check(!close_returned.load(), "close cannot overtake in-flight queue publication");
      pause.resume.count_down(); producer.join(); closer.join();
      Check(submitted == Q::inserted && closed == Q::closed && !credit.permit,
            "allocation through credit transfer and close serialized");
      Check(queue.Front().key == Key(f) && f.governor.Snapshot().active.backlog_items == 1,
            "close preserves just-published key and actual credit");
      Check(queue.Remove(Key(f)).code == Q::removed, "paused-publication entry drained after both calls join");
    }
    f.Empty();
  }
#endif
  std::cout << "PASS native runtime task queue " << checks << " checks; no dispatch authority\n";
}

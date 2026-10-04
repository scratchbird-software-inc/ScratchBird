// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "retained_runtime_task_queue.hpp"
#include "runtime_permit_fixture.hpp"
#include <cerrno>
#if defined(SB_QUEUE_CLAIM_FAULTS)
thread_local unsigned claim_fail_lock = 0;
struct ClaimPause { std::latch entered{1}, resume{1}; };
thread_local ClaimPause* claim_pause = nullptr;
extern "C" int __real_pthread_mutex_lock(pthread_mutex_t*);
extern "C" int __wrap_pthread_mutex_lock(pthread_mutex_t* mutex) {
  if (auto* pause = std::exchange(claim_pause, nullptr)) {
    pause->entered.count_down(); pause->resume.wait();
  }
  if (claim_fail_lock && --claim_fail_lock == 0) return EINVAL;
  return __real_pthread_mutex_lock(mutex);
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
r::NativeTaskQueueClaimKey Key(const Fixture& f, unsigned worker = 220) {
  const auto request = f.Request(false);
  r::NativeTaskQueueKey queued{{{request.task, Id(1), Id(201), Id(202), 19}, Id(201)},
                              request.attempt, Id(200), 17};
  return {queued, {{Id(worker), Id(1), Id(300), Id(202), 50},
                   {Id(300), Id(1), Id(301), Id(202), 40}, queued.task}};
}
void Publish(r::NativeRuntimeTaskQueue& queue, Fixture& f) {
  auto credit = f.governor.AcquireRuntimePermit(f.Request(false));
  Check(credit.ok() && queue.Push(Key(f).queued, credit.permit).code == Q::inserted && !credit.permit,
        "real queue entry and credit published");
}
void Release(r::NativeTaskQueueClaim& claim) {
  Check(bool(claim) && claim.key(), "actual live claim");
  const auto key = *claim.key();
  Check(claim.ReleaseAfterQuiescence(key) == Code::released && !claim && !claim.key(),
        "exact quiesced claim releases actual worker slot");
  Check(claim.ReleaseAfterQuiescence(key) == Code::no_grant, "repeat release cannot uncharge twice");
}
int main() {
#if defined(__linux__)
  const auto child = fork();
  Check(child >= 0, "active claim destruction child created");
  if (child == 0) {
    Fixture f;
    r::NativeRuntimeTaskQueue queue(Binding(f), f.governor, *f.metadata);
    Publish(queue, f);
    auto slot = f.governor.AcquireRuntimePermit(f.Request(true));
    {
      r::NativeTaskQueueClaim claim;
      if (!slot.ok() || queue.Claim(Key(f), slot.permit, claim).code != Q::claimed) std::_Exit(74);
      std::set_terminate([] { std::_Exit(73); });
    }
    std::_Exit(75);
  }
  int child_status = 0;
  Check(waitpid(child, &child_status, 0) == child && WIFEXITED(child_status) && WEXITSTATUS(child_status) == 73,
        "active claim cannot silently release a possibly live worker");
#endif
  for (bool closed : {false, true}) for (bool cancelled : {false, true}) for (bool expired : {false, true}) {
    Fixture f;
    {
      r::NativeRuntimeTaskQueue queue(Binding(f), f.governor, *f.metadata);
      Publish(queue, f);
      auto slot = f.governor.AcquireRuntimePermit(f.Request(true));
      Check(slot.ok(), "terminal-selection actual provisional slot");
      std::stop_source stop;
      if (cancelled) stop.request_stop();
      if (closed) Check(queue.Close() == Q::closed, "terminal-selection admission closed");
      r::NativeTaskQueueClaim claim;
      const r::NativeTaskQueueClaimControl control{stop.get_token(), expired ? Clock::now()-1s : Clock::now()+10s};
      const auto result = queue.Claim(Key(f), slot.permit, claim, control);
      const auto expected = closed ? Q::closed : cancelled ? Q::cancelled : expired ? Q::timed_out : Q::claimed;
      Check(result.code == expected, "valid pending claim selects closed then cancelled then timeout then transfer");
      if (expected == Q::claimed) {
        stop.request_stop();
        Check(claim && !slot.permit && f.governor.Snapshot().active.worker_threads == 1,
              "later cancellation does not revoke an issued worker claim");
        Release(claim);
      } else {
        Check(!claim && slot.permit && queue.Front().key == Key(f).queued &&
              f.governor.Snapshot().active.backlog_items == 1 && f.governor.Snapshot().active.worker_threads == 1,
              "terminal refusal preserves both original owners without cancellation acknowledgment");
        Check(slot.permit.Release(f.Request(true)) == Code::released && queue.Remove(Key(f).queued).code == Q::removed,
              "refused request owner performs actual cleanup");
      }
      Check(queue.Close() == Q::closed, "terminal-selection queue finally closed");
    }
    f.Empty();
  }
#if defined(SB_QUEUE_CLAIM_FAULTS)
  {
    Fixture f;
    {
      r::NativeRuntimeTaskQueue queue(Binding(f), f.governor, *f.metadata);
      Publish(queue, f);
      auto slot = f.governor.AcquireRuntimePermit(f.Request(true));
      Check(slot.ok(), "paused claim actual provisional slot");
      r::NativeTaskQueueClaim claim;
      std::stop_source stop;
      const r::NativeTaskQueueClaimControl control{stop.get_token(), Clock::now()+10s};
      ClaimPause pause;
      Q outcome{};
      std::thread worker([&] {
        claim_pause = &pause;
        outcome = queue.Claim(Key(f), slot.permit, claim, control).code;
      });
      pause.entered.wait();
      stop.request_stop(); pause.resume.count_down(); worker.join();
      Check(outcome == Q::cancelled && slot.permit && !claim && queue.Front().key == Key(f).queued,
            "cancellation arriving before actual queue predicate acquisition fences transfer");
      Check(queue.Close() == Q::closed && queue.Remove(Key(f).queued).code == Q::removed &&
            slot.permit.Release(f.Request(true)) == Code::released, "paused request joined before cleanup");
    }
    f.Empty();
  }
#endif
  {
    Fixture f;
    r::NativeTaskQueueClaim claim;
    {
      r::NativeRuntimeTaskQueue queue(Binding(f), f.governor, *f.metadata);
      Publish(queue, f);
      auto slot = f.governor.AcquireRuntimePermit(f.Request(true));
      Check(slot.ok(), "actual provisional worker slot acquired outside queue predicate");
      auto stale = Key(f); ++stale.queued.task.identity.runtime_generation; stale.worker.task = stale.queued.task;
      Check(queue.Claim(stale, slot.permit, claim).code == Q::stale_identity && slot.permit && !claim,
            "stale queue task generation cannot consume entry or provisional slot");
      auto wrong = Key(f); ++wrong.worker.task.identity.runtime_generation;
      Check(queue.Claim(wrong, slot.permit, claim).code == Q::invalid_binding && slot.permit && !claim,
            "worker task snapshot must equal exact queued task");
      wrong = Key(f); wrong.worker.identity.runtime_object_uuid = Id(221);
      Check(queue.Claim(wrong, slot.permit, claim).code == Q::invalid_binding, "worker UUID bound to actual slot");
#if defined(SB_QUEUE_CLAIM_FAULTS)
      claim_fail_lock = 2;
      const auto failure = queue.Claim(Key(f), slot.permit, claim);
      Check(failure.code == Q::release_failed && failure.permit_code == Code::synchronization_failed,
            "actual queue-credit release failure refuses claim");
      Check(slot.permit && !claim && queue.Front().key == Key(f).queued &&
            f.governor.Snapshot().active.backlog_items == 1 && f.governor.Snapshot().active.worker_threads == 1,
            "failed queue release retains entry credit and original provisional slot");
      claim_fail_lock = 1;
      Check(queue.Claim(Key(f), slot.permit, claim).code == Q::synchronization_failed && slot.permit && !claim,
            "actual queue predicate failure changes no ownership");
#endif
      const auto claimed = queue.Claim(Key(f), slot.permit, claim);
      Check(claimed.code == Q::claimed && claimed.key == Key(f).queued && !slot.permit && claim,
            "queue entry consumed and real worker slot transferred exactly once");
      Check(queue.Front().code == Q::empty && !f.governor.Snapshot().active.backlog_items &&
            f.governor.Snapshot().active.worker_threads == 1, "worker remains charged after queue consumption");
      auto other_request = f.Request(true); other_request.worker = Id(221);
      auto other = f.governor.AcquireRuntimePermit(other_request);
      Check(other.ok() && queue.Claim(Key(f, 221), other.permit, claim).code == Q::invalid_binding && other.permit &&
            *claim.key() == Key(f), "occupied output cannot replace an existing execution claim");
      Check(other.permit.Release(other_request) == Code::released, "unpublished extra provisional slot released");
      auto changed = *claim.key(); ++changed.worker.manager.runtime_generation;
      Check(claim.ReleaseAfterQuiescence(changed) == Code::invalid_binding && claim,
            "release requires original full worker and manager generations");
#if defined(SB_QUEUE_CLAIM_FAULTS)
      claim_fail_lock = 1;
      Check(claim.ReleaseAfterQuiescence(*claim.key()) == Code::synchronization_failed && claim,
            "actual worker release failure preserves claim for retry");
#endif
      Check(queue.Close() == Q::closed, "empty native queue closed while worker slot remains owned");
    }
    Check(f.governor.Snapshot().active.worker_threads == 1, "claim outlives queue without borrowing its memory");
    r::NativeTaskQueueClaim moved(std::move(claim));
    Check(!claim && moved && *moved.key() == Key(f), "claim move transfers exact ownership");
    Release(moved); f.Empty();
  }
  {
    Fixture f, foreign;
    {
      r::NativeRuntimeTaskQueue queue(Binding(f), f.governor, *f.metadata);
      Publish(queue, f);
      auto slot = foreign.governor.AcquireRuntimePermit(foreign.Request(true));
      r::NativeTaskQueueClaim claim;
      Check(slot.ok() && queue.Claim(Key(f), slot.permit, claim).code == Q::invalid_binding && slot.permit,
            "identical copied labels from foreign ledger cannot claim");
      Check(slot.permit.Release(foreign.Request(true)) == Code::released, "foreign owner releases its slot");
      auto wrong = f.governor.AcquireRuntimePermit(f.Request(false, 21));
      Check(wrong.ok() && queue.Claim(Key(f), wrong.permit, claim).code == Q::invalid_binding && wrong.permit,
            "queue credit cannot serve as worker slot");
      Check(wrong.permit.Release(f.Request(false, 21)) == Code::released, "wrong-profile provisional released");
      Check(queue.Close() == Q::closed && queue.Remove(Key(f).queued).code == Q::removed, "explicit refused-claim cleanup");
    }
    f.Empty(); foreign.Empty();
  }
  for (unsigned run = 0; run < 24; ++run) {
    Fixture f;
    {
      r::NativeRuntimeTaskQueue queue(Binding(f), f.governor, *f.metadata);
      Publish(queue, f);
      auto first_request = f.Request(true), second_request = first_request;
      second_request.worker = Id(221);
      auto first = f.governor.AcquireRuntimePermit(first_request);
      auto second = f.governor.AcquireRuntimePermit(second_request);
      Check(first.ok() && second.ok(), "two actual independent provisional worker slots");
      r::NativeTaskQueueClaim a, b;
      Q result_a{}, result_b{};
      std::barrier start(3);
      std::thread one([&] { start.arrive_and_wait(); result_a = queue.Claim(Key(f), first.permit, a).code; });
      std::thread two([&] { start.arrive_and_wait(); result_b = queue.Claim(Key(f,221), second.permit, b).code; });
      start.arrive_and_wait(); one.join(); two.join();
      Check((result_a == Q::claimed && result_b == Q::not_found) ||
            (result_b == Q::claimed && result_a == Q::not_found), "exactly one contender consumes actual queue entry");
      Check(bool(a) != bool(b) && bool(first.permit) != bool(second.permit), "one claimed and one provisional owner remain");
      if (a) Release(a); else Check(first.permit.Release(first_request) == Code::released, "loser releases provisional slot");
      if (b) Release(b); else Check(second.permit.Release(second_request) == Code::released, "loser releases provisional slot");
      Check(queue.Close() == Q::closed && queue.Front().code == Q::closed, "competing claim queue closed empty");
    }
    f.Empty();
  }
  for (unsigned run = 0; run < 24; ++run) {
    Fixture f;
    {
      r::NativeRuntimeTaskQueue queue(Binding(f), f.governor, *f.metadata);
      Publish(queue, f);
      auto slot = f.governor.AcquireRuntimePermit(f.Request(true));
      Check(slot.ok(), "close race actual provisional slot");
      r::NativeTaskQueueClaim claim;
      Q claimed{}, closed{};
      std::barrier start(3);
      std::thread claimant([&] { start.arrive_and_wait(); claimed = queue.Claim(Key(f), slot.permit, claim).code; });
      std::thread closer([&] { start.arrive_and_wait(); closed = queue.Close(); });
      start.arrive_and_wait(); claimant.join(); closer.join();
      Check(closed == Q::closed && (claimed == Q::claimed || claimed == Q::closed),
            "claim and close have one serialized ownership boundary");
      if (claimed == Q::claimed) {
        Check(claim && !slot.permit && queue.Front().code == Q::closed, "claim won before close and entry is consumed");
        Release(claim);
      } else {
        Check(!claim && slot.permit && queue.Front().key == Key(f).queued, "close won without either credit transfer");
        Check(slot.permit.Release(f.Request(true)) == Code::released && queue.Remove(Key(f).queued).code == Q::removed,
              "close winner leaves explicit cleanup to both original owners");
      }
    }
    f.Empty();
  }
  {
    Fixture f;
    r::NativeTaskQueueClaim claim;
    {
      m::MemorySafeRetirement domain(*f.resource, Id(900).bytes, 4, 8);
      Check(domain.Initialize() == S::ok, "retained claim domain");
      {
        r::RetainedRuntimeTaskQueueOwner owner(domain);
        Check(owner.Initialize(Binding(f), {1}, f.governor, *f.metadata,
                              {Id(901).bytes, Id(20).bytes}) == S::ok, "retained claim owner");
        r::RetainedRuntimeTaskQueueOperation operation;
        Check(owner.AcquireOperation(Id(200), Id(202), 17, {Id(902).bytes, Id(20).bytes}, operation) == S::ok,
              "real retained operation");
        auto credit = f.governor.AcquireRuntimePermit(f.Request(false));
        auto slot = f.governor.AcquireRuntimePermit(f.Request(true));
        Check(credit.ok() && slot.ok() && operation.Push(Key(f).queued, credit.permit).code == Q::inserted,
              "retained queue has actual entry and provisional slot");
        Check(operation.Claim(Key(f), slot.permit, claim, {{}, Clock::now()-1s}).code == Q::timed_out &&
              slot.permit && !claim, "retained operation forwards deadline without consuming provisional slot");
        Check(operation.Claim(Key(f), slot.permit, claim).code == Q::claimed && claim && !slot.permit,
              "retained wrapper transfers actual slot through result delivery");
        Check(owner.Close() && owner.FenceAdmission(), "retained queue admission closed");
        Check(!owner.Drain(Clock::now()).drained, "operation still retains delivery lifetime");
        operation.Reset();
        Check(owner.Drain(Clock::now()).drained && f.governor.Snapshot().active.worker_threads == 1,
              "local queue drain does not assert worker quiescence or release worker slot");
      }
      Check(domain.Collect() == S::ok && !domain.Snapshot().retained_payload_bytes, "queue payload really freed with claim alive");
      domain.Close(); Check(domain.Drain(Clock::now()+1s) == S::ok, "queue retirement domain drained");
    }
    Release(claim); f.Empty();
  }
  {
    Fixture f;
    {
      r::NativeRuntimeTaskQueue queue(Binding(f), f.governor, *f.metadata);
      Publish(queue, f);
      auto slot = f.governor.AcquireRuntimePermit(f.Request(true));
      r::NativeTaskQueueClaim claim;
      Check(slot.ok() && queue.Close() == Q::closed, "close precedes proposed claim");
      Check(queue.Claim(Key(f), slot.permit, claim).code == Q::closed && slot.permit && !claim &&
            queue.Front().key == Key(f).queued, "closed queue cannot publish a fresh worker claim");
      Check(slot.permit.Release(f.Request(true)) == Code::released && queue.Remove(Key(f).queued).code == Q::removed,
            "closed refusal retains both original owners for explicit cleanup");
    }
    f.Empty();
  }
  std::cout << "PASS actual queue worker-slot claims " << checks << " checks; no action authority\n";
}

// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "checked_fifo_mutex.hpp"
#include <array>
#include <atomic>
#include <cstdlib>
#include <iostream>
#include <semaphore>
#include <utility>
#include <vector>

using Mutex = scratchbird::core::platform::CheckedFifoMutex;
using Result = Mutex::Result;
using namespace std::chrono_literals;
namespace {
std::atomic<unsigned> checks{0};
void Check(bool value, const char* message) {
  ++checks;
  if (!value) { std::cerr << "FAIL " << message << '\n'; std::abort(); }
}
struct Park {
  std::binary_semaphore entered{0};
  std::binary_semaphore reparked{0};
  unsigned visits = 0;
  pthread_cond_t* condition = nullptr;
  bool fail = false;
  bool defer_return = false;
  std::binary_semaphore woke{0}, resume{0};
};
thread_local Park* park = nullptr;
struct DeliveryPause { std::binary_semaphore reached{0}, resume{0}; };
thread_local DeliveryPause* delivery_pause = nullptr;
thread_local unsigned fail_capture_lock_call=0, capture_lock_calls=0;
void Enter(pthread_cond_t* condition) {
  if (park && !park->condition) { park->condition = condition; park->entered.release(); }
  if (park && ++park->visits==2) park->reparked.release();
}
int FinishWait(int code, pthread_mutex_t* mutex) {
  if (park && park->defer_return) {
    park->defer_return=false;
    // Model a woken thread that has not yet reacquired the predicate mutex.
    // The wrapper still returns holding the real mutex, as native waits require.
    Check(pthread_mutex_unlock(mutex)==0,"defer native reacquisition");
    park->woke.release(); park->resume.acquire();
    Check(pthread_mutex_lock(mutex)==0,"resume native reacquisition");
  }
  return code;
}
void Idle(Mutex& mutex) {
  const auto state = mutex.Observe();
  Check(!state.held && state.waiters == 0 && state.calls == 0, "actual final ownership and callback counts zero");
}
}
extern "C" int __real_pthread_cond_wait(pthread_cond_t*, pthread_mutex_t*);
extern "C" int __real_pthread_cond_timedwait(pthread_cond_t*, pthread_mutex_t*, const timespec*);
extern "C" int __real_pthread_cond_broadcast(pthread_cond_t*);
extern "C" int __real_pthread_mutex_unlock(pthread_mutex_t*);
extern "C" int __real_pthread_mutex_lock(pthread_mutex_t*);
extern "C" int __wrap_pthread_mutex_lock(pthread_mutex_t* mutex) {
  if (fail_capture_lock_call && ++capture_lock_calls==fail_capture_lock_call) return EINVAL;
  return __real_pthread_mutex_lock(mutex);
}
extern "C" int __wrap_pthread_mutex_unlock(pthread_mutex_t* mutex) {
  const auto code=__real_pthread_mutex_unlock(mutex);
  if (code==0 && delivery_pause) {
    auto* pause=std::exchange(delivery_pause,nullptr);
    pause->reached.release(); pause->resume.acquire();
  }
  return code;
}
extern "C" int __wrap_pthread_cond_wait(pthread_cond_t* c, pthread_mutex_t* m) {
  Enter(c);
  if (park && park->fail) return EINVAL;
  return FinishWait(__real_pthread_cond_wait(c,m),m);
}
extern "C" int __wrap_pthread_cond_timedwait(pthread_cond_t* c, pthread_mutex_t* m, const timespec* t) {
  Enter(c);
  if (park && park->fail) return EINVAL;
  return FinishWait(__real_pthread_cond_timedwait(c,m,t),m);
}
extern "C" int __wrap_pthread_cond_broadcast(pthread_cond_t* c) {
  return __real_pthread_cond_broadcast(c);
}
int main() {
  {
    Mutex mutex(2);
    Mutex::WaitEvidence evidence{true, 123};
    Check(mutex.TryLock({}, {}, {}, &evidence)==Result::acquired &&
          !evidence.registered && evidence.duration_us==0 && mutex.Observe().registered_waits==0,
          "uncontended grant clears evidence without registering a wait");
    evidence={true,123};
    Check(mutex.Lock({}, {}, {}, &evidence)==Result::recursive &&
          !evidence.registered && evidence.duration_us==0,
          "recursive refusal cannot reuse prior wait sample");
    Check(mutex.Unlock(),"measurement initial release");
    evidence={true,123};
    Check(mutex.Lock(Mutex::Clock::now(), {}, {}, &evidence)==Result::timed_out &&
          !evidence.registered && evidence.duration_us==0 && mutex.Observe().registered_waits==0,
          "expired immediate refusal is not a registered wait");
  }
  for (unsigned terminal=0; terminal<5; ++terminal) {
    Mutex mutex(2); Park probe;
    Check(mutex.TryLock()==Result::acquired,"measurement holder acquired");
    std::stop_source stop;
    Mutex::WaitEvidence sample;
    Result outcome=Result::failed;
    Mutex::Clock::time_point begin,end;
    std::thread waiter([&] {
      park=&probe; probe.fail=terminal==4;
      begin=Mutex::Clock::now();
      outcome=mutex.Lock(begin+(terminal==3?1s:5s),stop.get_token(),{},&sample);
      end=Mutex::Clock::now();
      if (outcome==Result::acquired) Check(mutex.Unlock(),"measured holder release");
    });
    Check(probe.entered.try_acquire_for(5s),"measured registration actually parks");
    std::uint64_t lower=0;
    if (terminal!=4) {
      std::array<Mutex::WaiterObservation,1> rows;
      const auto snapshot=mutex.ObserveWaiters(rows);
      Check(snapshot.complete && snapshot.state.registered_waits==1 && snapshot.state.waiters==1,
            "live native registration counted once before completion");
      if (terminal==0) {
        Check(__real_pthread_cond_broadcast(probe.condition)==0,"real spurious notification for measurement");
        Check(probe.reparked.try_acquire_for(5s),"measurement waiter reparks after spurious wake");
        Check(mutex.Observe().registered_waits==1,"spurious wake cannot increment wait count");
      }
      const auto elapsed=std::chrono::duration_cast<std::chrono::microseconds>(
          Mutex::Clock::now()-rows[0].started).count();
      lower=elapsed>0?static_cast<std::uint64_t>(elapsed):0;
    }
    if (terminal==0) Check(mutex.Unlock(),"measured grant trigger");
    if (terminal==1) stop.request_stop();
    if (terminal==2) mutex.Close();
    waiter.join();
    const std::array expected{Result::acquired,Result::cancelled,Result::closed,Result::timed_out,Result::failed};
    const auto upper=std::chrono::duration_cast<std::chrono::microseconds>(end-begin).count();
    Check(outcome==expected[terminal] && sample.registered && sample.duration_us>=lower &&
          sample.duration_us<=static_cast<std::uint64_t>(upper),
          "actual selected outcome carries bounded monotonic registered-call duration");
    Check(mutex.Observe().registered_waits==1,"one cumulative registration for every selected outcome");
    if (terminal!=0) Check(mutex.Unlock(),"measured terminal preserves original holder");
    Idle(mutex);
  }
  {
    Mutex left(1), right(1); Park left_wait, right_wait;
    Mutex::OwnerIdentity a{}, b{}; a[0]=0xa1; b[0]=0xb2;
    std::counting_semaphore<2> ready(0), proceed(0);
    std::stop_source stop;
    std::thread one([&] {
      Check(left.TryLock({}, {}, a)==Result::acquired,"cycle left actual holder");
      ready.release(); proceed.acquire(); park=&left_wait;
      Check(right.Lock({},stop.get_token(),a)==Result::cancelled,"cycle left wait cooperatively cancelled");
      Check(left.Unlock(a),"cycle left holder releases itself");
    });
    std::thread two([&] {
      Check(right.TryLock({}, {}, b)==Result::acquired,"cycle right actual holder");
      ready.release(); proceed.acquire(); park=&right_wait;
      Check(left.Lock({},stop.get_token(),b)==Result::cancelled,"cycle right wait cooperatively cancelled");
      Check(right.Unlock(b),"cycle right holder releases itself");
    });
    Check(ready.try_acquire_for(5s) && ready.try_acquire_for(5s),"both real cycle holders ready");
    proceed.release(2);
    Check(left_wait.entered.try_acquire_for(5s) && right_wait.entered.try_acquire_for(5s),
          "real two-latch cycle is parked before capture");
    std::array<Mutex::WaitSetEntry,2> entries;
    entries[0].mutex=&right; entries[1].mutex=&left;
    std::array<Mutex::WaiterObservation,2> rows;
    rows[0].owner=a;
    for (unsigned failure : {1U,2U}) {
      capture_lock_calls=0; fail_capture_lock_call=failure;
      const auto failed=Mutex::ObserveWaitSet(entries,rows,2);
      fail_capture_lock_call=0;
      Check(failed==Mutex::WaitSetResult::synchronization_failed && rows[0].owner==a,
            "failed group lock publishes no partial wait records");
      Check(!entries[0].lock.owns_lock() && !entries[1].lock.owns_lock() &&
            left.Observe().held && right.Observe().held,"partial lock failure releases native locks not semantic holders");
    }
    std::mutex unrelated;
    entries[0].lock=std::unique_lock(unrelated);
    Check(Mutex::ObserveWaitSet(entries,rows,2)==Mutex::WaitSetResult::invalid &&
          entries[0].lock.owns_lock(),"invalid borrowed frame does not release caller-owned lock");
    entries[0].lock={};
    Check(Mutex::ObserveWaitSet(entries,rows,1)==Mutex::WaitSetResult::exhausted,
          "multi-latch capture enforces admitted latch bound");
    Check(Mutex::ObserveWaitSet(entries,std::span(rows).first(1),2)==Mutex::WaitSetResult::insufficient_capacity &&
          rows[0].owner==a && !entries[0].observation.complete && !entries[1].observation.complete,
          "multi-latch capacity failure exposes no partial edge output");
    auto capture=[&](bool reverse) {
      std::array<Mutex::WaitSetEntry,2> local;
      local[reverse?1:0].mutex=&right; local[reverse?0:1].mutex=&left;
      std::array<Mutex::WaiterObservation,2> waits;
      for (unsigned repeat=0;repeat<100;++repeat) {
        Check(Mutex::ObserveWaitSet(local,waits,2)==Mutex::WaitSetResult::captured,
              "opposite-order collectors capture without locking cycle");
        const auto& r=local[reverse?1:0]; const auto& l=local[reverse?0:1];
        Check(r.observation.complete && l.observation.complete && r.observation.owner==b &&
              l.observation.owner==a && waits[r.offset].owner==a && waits[l.offset].owner==b,
              "consistent capture preserves both actual wait-for edges");
        Check(!r.lock.owns_lock() && !l.lock.owns_lock(),"capture releases every native commit lock");
      }
    };
    std::thread collector([&]{capture(true);}); capture(false); collector.join();
    entries[1].mutex=&right;
    Check(Mutex::ObserveWaitSet(entries,rows,2)==Mutex::WaitSetResult::invalid,
          "duplicate latch rejected before recursive native locking");
    entries[1].mutex=nullptr;
    Check(Mutex::ObserveWaitSet(entries,rows,2)==Mutex::WaitSetResult::invalid,
          "missing latch rejected before capture");
    Check(Mutex::ObserveWaitSet({},rows,0)==Mutex::WaitSetResult::invalid &&
          Mutex::ObserveWaitSet({},rows,1)==Mutex::WaitSetResult::captured,
          "empty capture still requires a nonzero admitted bound");
    stop.request_stop(); one.join(); two.join(); Idle(left); Idle(right);
  }
  {
    // Observe actual stack-registered waits, not synthetic graph edges. A
    // too-small destination must not expose a deceptively complete prefix.
    Mutex mutex(2); Park first, second;
    Mutex::OwnerIdentity holder{}, a{}, b{};
    for (unsigned i=0;i<16;++i) { holder[i]=i+0x80; a[i]=i+0xa0; b[i]=i+0xe0; }
    first.defer_return=true;
    const auto main_thread=std::this_thread::get_id();
    Check(mutex.TryLock({}, {}, holder)==Result::acquired,"inspection holder granted");
    std::stop_source cancel;
    const auto deadline=Mutex::Clock::now()+10s;
    std::thread one([&] {
      park=&first;
      Check(mutex.Lock(deadline,cancel.get_token(),a)==Result::cancelled,"inspected waiter cancelled");
    });
    Check(first.entered.try_acquire_for(5s),"first inspected waiter actually parked");
    std::thread two([&] {
      park=&second;
      Check(mutex.Lock(deadline,{},b)==Result::closed,"inspected waiter closed");
    });
    Check(second.entered.try_acquire_for(5s),"second inspected waiter actually parked");
    std::array<Mutex::WaiterObservation,3> rows{};
    rows[0].owner=holder; rows[2].owner=holder;
    const auto small=mutex.ObserveWaiters(std::span(rows).first(1));
    Check(!small.complete && small.state.waiters==2 && small.state.calls==2,
          "insufficient inspection capacity reports exact requirement");
    Check(rows[0].owner==holder,"incomplete inspection leaves output untouched");
    const auto view=mutex.ObserveWaiters(rows);
    Check(view.complete && view.state.held && view.holder==main_thread && view.owner==holder,
          "inspection preserves real native holder and binary task");
    Check(rows[0].thread==one.get_id() && rows[1].thread==two.get_id() &&
          rows[0].owner==a && rows[1].owner==b,"inspection records real FIFO waiters");
    Check(rows[0].deadline==deadline && rows[1].deadline==deadline &&
          rows[0].started<=rows[1].started && rows[1].started<=Mutex::Clock::now(),
          "inspection captures native wait timing");
    Check(!rows[0].cancellation_requested && !rows[1].cancellation_requested &&
          rows[2].owner==holder,"inspection respects output extent");
    cancel.request_stop();
    Check(first.woke.try_acquire_for(5s),"cancelled native waiter paused before predicate reacquisition");
    const auto pending=mutex.ObserveWaiters(rows);
    Check(pending.complete && pending.state.waiters==2 && rows[0].owner==a &&
          rows[0].cancellation_requested && !rows[1].cancellation_requested,
          "cancellation request remains observed until real wait unlink");
    first.resume.release(); one.join();
    const auto after=mutex.ObserveWaiters(rows);
    Check(after.complete && after.state.waiters==1 && rows[0].owner==b,
          "cancelled wait edge disappears after actual unlink");
    mutex.Close(); two.join();
    const auto closed=mutex.ObserveWaiters({});
    Check(closed.complete && closed.state.closed && closed.state.held && closed.owner==holder &&
          closed.state.waiters==0 && closed.state.calls==0,"close observation does not erase live holder");
    Check(mutex.Unlock(holder),"inspected holder releases");
    const auto empty=mutex.ObserveWaiters({});
    Check(empty.complete && !empty.owner && empty.holder==std::thread::id{} && !empty.state.held,
          "release clears observed holder identity");
    Check(view.owner==holder && view.state.waiters==2,"captured state is a value not live authority");
  }
  for (bool immediate : {false,true}) for (bool held : {false,true})
    for (unsigned signals=0; signals<8; ++signals) {
      Mutex mutex(1);
      if (held) Check(mutex.TryLock()==Result::acquired,"matrix initial holder");
      std::stop_source stop;
      if (signals&1) mutex.Close();
      if (signals&2) stop.request_stop();
      const auto deadline=(signals&4) ? Mutex::Clock::now() : Mutex::Clock::now()+10s;
      // No terminal condition and a live holder is covered by the park/FIFO
      // fixtures below; avoid waiting for that holder in this initial matrix.
      if (held && signals==0 && !immediate) { Check(mutex.Unlock(),"matrix holder release"); continue; }
      std::thread request([&] {
        const auto result=immediate ? mutex.TryLock(deadline,stop.get_token()) : mutex.Lock(deadline,stop.get_token());
        const auto expected=(signals&1) ? Result::closed : (signals&2) ? Result::cancelled :
          (signals&4) ? Result::timed_out : held ? Result::busy : Result::acquired;
        Check(result==expected,"initial closed cancelled expired grant matrix");
        if (result==Result::acquired) Check(mutex.Unlock(),"matrix actual holder releases");
      }); request.join();
      Check(mutex.Observe().held==held,"terminal matrix preserves prior holder");
      if (held) Check(mutex.Unlock(),"matrix owning release survives close");
      Idle(mutex);
    }
  {
    Mutex mutex(0);
    Check(mutex.TryLock() == Result::acquired, "immediate grant");
    Check(mutex.TryLock() == Result::recursive, "recursive try rejected");
    Check(mutex.Lock(std::nullopt) == Result::recursive, "recursive wait rejected without park");
    std::thread other([&] {
      Check(!mutex.Unlock(), "foreign release preserves holder");
      Check(mutex.TryLock() == Result::busy, "exclusive conflict");
      Check(mutex.Lock(std::nullopt) == Result::exhausted, "zero waiter limit");
      std::stop_source stop; stop.request_stop();
      Check(mutex.Lock(Mutex::Clock::now(),stop.get_token()) == Result::cancelled, "mutex cancellation precedes expiry");
      Check(mutex.Lock(std::nullopt,stop.get_token()) == Result::cancelled, "cancelled conflict does not register");
    }); other.join();
    Check(mutex.Unlock(), "owning release");
    Check(!mutex.Unlock(), "duplicate release");
    Idle(mutex);
  }
  for (unsigned count : {1U, 2U, 8U}) {
    Mutex mutex(count);
    Check(mutex.TryLock() == Result::acquired, "FIFO initial holder");
    std::array<Park,8> probes;
    std::vector<std::thread> threads;
    unsigned payload = 0; // Deliberately non-atomic: real release/acquire publication.
    for (unsigned i=0; i<count; ++i) {
      threads.emplace_back([&,i] {
        park = &probes[i];
        Check(mutex.Lock(Mutex::Clock::now()+10s) == Result::acquired, "registered FIFO acquire");
        Check(payload == i, "ordinary FIFO and actual payload publication");
        payload = i+1;
        Check(mutex.Unlock(), "FIFO holder release");
        park = nullptr;
      });
      Check(probes[i].entered.try_acquire_for(5s), "actual native park observed");
      Check(mutex.Observe().waiters == i+1, "serialized registration count");
    }
    std::thread overflow([&] { Check(mutex.Lock(std::nullopt) == Result::exhausted, "selected waiter limit"); });
    overflow.join();
    Check(mutex.Unlock(), "release wakes actual native waiters");
    for(auto& thread:threads) thread.join();
    Check(payload == count, "every queued thread made progress");
    Idle(mutex);
  }
  // Remove each position from a three-node queue without disturbing survivors.
  for(unsigned removed=0; removed<3; ++removed) {
    Mutex mutex(3);
    Check(mutex.TryLock() == Result::acquired, "cancellation initial holder");
    std::array<Park,3> probes;
    std::array<std::stop_source,3> stops;
    std::array<std::thread,3> threads;
    unsigned next = 0;
    for(unsigned i=0; i<3; ++i) {
      threads[i]=std::thread([&,i] {
        park=&probes[i];
        const auto result=mutex.Lock(Mutex::Clock::now()+10s,stops[i].get_token());
        if(i==removed) Check(result==Result::cancelled,"selected waiter cancelled");
        else {
          Check(result==Result::acquired,"survivor acquired");
          if(next==removed) ++next;
          Check(next==i,"survivor order unchanged"); ++next;
          Check(mutex.Unlock(),"survivor release");
        }
        park=nullptr;
      });
      Check(probes[i].entered.try_acquire_for(5s),"cancellable native park observed");
      Check(mutex.Observe().waiters==i+1,"cancellable registration visible");
    }
    stops[removed].request_stop(); threads[removed].join();
    Check(mutex.Observe().waiters==2,"cancelled waiter removed before return");
    Check(mutex.Unlock(),"release after cancellation");
    for(auto& thread:threads) if(thread.joinable()) thread.join();
    Idle(mutex);
  }
  {
    Mutex mutex(1); Park probe; probe.defer_return=true;
    Check(mutex.TryLock()==Result::acquired,"handoff holder");
    std::thread waiter([&] {
      park=&probe;
      Check(mutex.Lock(Mutex::Clock::now()+10s)==Result::acquired,"delayed head acquire");
      Check(mutex.Unlock(),"delayed head release");
    });
    Check(probe.entered.try_acquire_for(5s),"handoff native park");
    Check(mutex.Observe().waiters==1,"handoff registered head");
    Check(mutex.Unlock(),"release with deliberately delayed reacquisition");
    Check(probe.woke.try_acquire_for(5s),"actual release wake held before reacquisition");
    Check(mutex.TryLock()==Result::busy,"immediate arrival cannot barge ahead of registered head");
    probe.resume.release();
    waiter.join(); Idle(mutex);
  }
  {
    Mutex mutex(1); Park probe; probe.fail=true;
    Check(mutex.TryLock()==Result::acquired,"fault holder");
    std::thread waiter([&] {
      park=&probe;
      Check(mutex.Lock(std::nullopt)==Result::failed,"native failure is not a grant");
    }); waiter.join();
    Check(mutex.Observe().held && mutex.Observe().waiters==0,"fault preserves holder and removes waiter");
    Check(mutex.Unlock(),"release after native failure"); Idle(mutex);
  }
  {
    Mutex mutex(1); Park probe;
    Check(mutex.TryLock()==Result::acquired,"spurious holder");
    std::thread waiter([&] {
      park=&probe;
      Check(mutex.Lock(std::nullopt)==Result::acquired,"real untimed park eventually acquires");
      Check(mutex.Unlock(),"untimed holder release");
    });
    Check(probe.entered.try_acquire_for(5s),"untimed native park");
    Check(mutex.Observe().held,"initial holder still owns");
    Check(__real_pthread_cond_broadcast(probe.condition)==0,"inject spurious wake");
    Check(probe.reparked.try_acquire_for(5s),"spurious wake reparks without a grant");
    Check(mutex.Observe().waiters==1,"spurious wake does not duplicate registration");
    Check(mutex.Unlock(),"real release after spurious wake");
    waiter.join(); Idle(mutex);
  }
  {
    Mutex mutex(1); Park probe;
    Check(mutex.TryLock()==Result::acquired,"deadline holder");
    std::thread waiter([&] {
      park=&probe;
      const auto deadline=Mutex::Clock::now()+20ms;
      Check(mutex.Lock(deadline)==Result::timed_out,"actual parked deadline expires");
      Check(Mutex::Clock::now()>=deadline,"timeout cannot be early");
    }); waiter.join();
    Check(mutex.Observe().held && mutex.Observe().waiters==0,"timeout preserves existing ownership");
    Check(mutex.Unlock(),"deadline holder release"); Idle(mutex);
  }
  for (unsigned signals=1; signals<8; ++signals) {
    Mutex mutex(1); Park probe; probe.defer_return=true;
    std::stop_source stop;
    Mutex::Clock::time_point deadline;
    Check(mutex.TryLock()==Result::acquired,"terminal wake holder");
    std::thread waiter([&] {
      park=&probe;
      deadline=Mutex::Clock::now()+200ms;
      const auto result=mutex.Lock((signals&4) ? deadline : Mutex::Clock::now()+10s,stop.get_token());
      const auto expected=(signals&1) ? Result::closed : (signals&2) ? Result::cancelled : Result::timed_out;
      Check(result==expected,"terminal selection after actual release wake wins over available grant");
    });
    Check(probe.entered.try_acquire_for(5s),"terminal waiter actually parks");
    Check(mutex.Observe().waiters==1,"terminal registration serialized");
    Check(mutex.Unlock(),"terminal release wakes waiter");
    Check(probe.woke.try_acquire_for(5s),"hold real wake before reacquisition");
    if (signals&1) mutex.Close();
    if (signals&2) stop.request_stop();
    if (signals&4) std::this_thread::sleep_until(deadline);
    probe.resume.release(); waiter.join();
    Idle(mutex);
    Check(mutex.TryLock()==((signals&1) ? Result::closed : Result::acquired),"terminal failure grants no ownership and does not poison retry");
    if (!(signals&1)) Check(mutex.Unlock(),"fresh retry release");
  }
  {
    Mutex mutex(1); Park probe;
    Check(mutex.TryLock()==Result::acquired,"close live holder");
    std::thread waiter([&] {
      park=&probe;
      Check(mutex.Lock(std::nullopt)==Result::closed,"close wakes real unbounded waiter");
    });
    Check(probe.entered.try_acquire_for(5s),"close waiter parked");
    Check(mutex.Observe().waiters==1,"close registration visible");
    mutex.Close(); mutex.Close(); waiter.join();
    Check(mutex.Observe().closed && mutex.Observe().held,"repeated close retains live holder");
    Check(mutex.Unlock(),"closed holder still owns valid release"); Idle(mutex);
  }
  {
    Mutex mutex(1); DeliveryPause pause; std::stop_source stop;
    Mutex::Clock::time_point deadline;
    std::thread holder([&] {
      deadline=Mutex::Clock::now()+100ms;
      delivery_pause=&pause;
      const auto result=mutex.TryLock(deadline,stop.get_token());
      Check(result==Result::acquired,"post-commit terminal signals cannot rewrite acquisition result");
      Check(mutex.Observe().held,"post-commit terminal signals preserve owning grant");
      Check(mutex.Unlock(),"post-commit holder release survives terminal signals");
    });
    Check(pause.reached.try_acquire_for(5s),"pause actual committed result delivery");
    mutex.Close(); stop.request_stop(); std::this_thread::sleep_until(deadline);
    Check(mutex.Observe().held,"close does not forgive in-flight committed grant");
    pause.resume.release(); holder.join(); Idle(mutex);
  }
  {
    Mutex mutex(2);
    Mutex::OwnerIdentity task{}; task[0]=1;
    auto different=task; different[15]=2;
    Check(mutex.TryLock({}, {}, task)==Result::acquired,"binary task holder");
    std::thread moved_task([&] {
      Check(mutex.TryLock({}, {}, task)==Result::recursive,"same binary task cannot recurse on another native thread");
      Check(mutex.Lock(Mutex::Clock::now()+1s, {}, task)==Result::recursive,"same binary task cannot park behind itself");
      Check(!mutex.Unlock(task),"binary task alone cannot release on foreign native thread");
    }); moved_task.join();
    Check(mutex.TryLock({}, {}, different)==Result::recursive,"different task does not permit native thread recursion");
    Check(!mutex.Unlock(different),"wrong binary task cannot release native holder");
    Check(!mutex.Unlock(),"unbound release cannot discard binary ownership");
    Check(mutex.Observe().held,"failed binary releases preserve ownership");
    Check(mutex.Unlock(task),"exact binary task and native thread release");
    Check(mutex.TryLock()==Result::acquired,"unbound native reuse clears old identity");
    Check(!mutex.Unlock(task),"bound release cannot release an unbound holder");
    Check(mutex.Unlock(),"unbound native release preserved"); Idle(mutex);
  }
  {
    Mutex mutex(2); Park first, second;
    Mutex::OwnerIdentity task{}; task[0]=1;
    std::binary_semaphore granted(0), release(0);
    Check(mutex.TryLock()==Result::acquired,"queued duplicate task initial holder");
    std::thread one([&] {
      park=&first;
      Check(mutex.Lock(Mutex::Clock::now()+5s, {}, task)==Result::acquired,"first queued binary task granted");
      granted.release(); release.acquire();
      Check(mutex.Unlock(task),"first queued task retains ownership");
    });
    Check(first.entered.try_acquire_for(5s),"first binary task parked");
    std::thread two([&] {
      park=&second;
      Check(mutex.Lock(Mutex::Clock::now()+5s, {}, task)==Result::recursive,"queued duplicate revalidates binary owner before repark");
    });
    Check(second.entered.try_acquire_for(5s),"second binary task parked");
    Check(mutex.Unlock(),"release to first binary task");
    Check(granted.try_acquire_for(5s),"first binary task commit visible");
    two.join(); release.release(); one.join(); Idle(mutex);
  }
  std::cout << "PASS " << checks << " native FIFO parking checks; not full engine latch acceptance\n";
}

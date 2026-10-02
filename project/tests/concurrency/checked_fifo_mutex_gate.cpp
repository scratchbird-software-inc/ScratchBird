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
  std::cout << "PASS " << checks << " native FIFO parking checks; not full engine latch acceptance\n";
}

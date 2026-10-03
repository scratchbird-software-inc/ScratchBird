// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "checked_mode_latch.hpp"
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <latch>
#include <semaphore>
#include <string_view>
#include <vector>

using L = scratchbird::core::platform::CheckedModeLatch;
using M = L::Mode;
using R = L::Result;
using namespace std::chrono_literals;
namespace {
std::atomic<unsigned> checks{0};
void Check(bool ok, const char* message = "check failed") {
  ++checks;
  if (!ok) { std::fprintf(stderr, "%s (%u)\n", message, checks.load()); std::abort(); }
}
// Independent literal Core matrix, not derived from implementation masks.
constexpr std::string_view matrix[]{
  "YYYNNNNYNNY", "YYYNNNNYNNY", "YYYNNNNNNNN", "NNNNNNNNNNN",
  "NNNNNNNNNNN", "NNNNNYNNNNN", "NNNNNNNNNNN", "YYNNNNNYNNY",
  "NNNNNNNNNNN", "NNNNNNNNNNN", "YYNNNNNYNNY"};
M Mode(unsigned i) { return static_cast<M>(i+1); }
thread_local std::binary_semaphore* parked = nullptr;
thread_local bool fail_wait = false;
thread_local std::binary_semaphore* woke = nullptr;
thread_local std::binary_semaphore* resume = nullptr;
struct PromotionGap {
  std::binary_semaphore entered{0}, proceed{0};
  bool after_unlock=false;
};
thread_local PromotionGap* promotion_gap=nullptr;
void WaitEntered() { if (auto* signal = parked) { parked = nullptr; signal->release(); } }
int AfterWake(int result, pthread_mutex_t* mutex) {
  if (woke && (result==0 || result==ETIMEDOUT)) {
    // Controlled wake/reacquire gap: another thread may close/cancel while the
    // waiter no longer sleeps but has not reselected under the commit mutex.
    auto* signal=woke; woke=nullptr;
    Check(pthread_mutex_unlock(mutex)==0);
    signal->release(); resume->acquire();
    Check(pthread_mutex_lock(mutex)==0);
  }
  return result;
}
void Empty(L& latch) {
  const auto state = latch.Observe();
  Check(state.holders == 0 && state.waiters == 0 && state.calls == 0);
}
void PromotionCloseBoundary() {
  for (const bool committed:{false,true}) for (unsigned state=0;state<8;++state) {
    L latch(1,2);
    PromotionGap gap; gap.after_unlock=committed;
    std::stop_source stop;
    std::binary_semaphore delivered{0},finish{0};
    std::atomic<bool> returned=false;
    L::Clock::time_point deadline;
    std::thread worker([&] {
      L::OwnerIdentity owner{}; owner[6]=0x70; owner[8]=0x80; owner[15]=1;
      L::Grant grant;
      Check(latch.TryAcquire(grant,M::upgrade,{},{},owner)==R::acquired);
      deadline=L::Clock::now()+500ms;
      promotion_gap=&gap;
      const auto result=latch.PromoteUpgradeToExclusive(grant,
          state&4 ? std::optional{deadline} : std::nullopt,stop.get_token(),owner);
      returned=true;
      Check(promotion_gap==nullptr,"promotion reached its actual native boundary");
      const auto expected=committed ? R::acquired :
          state&1 ? R::closed : state&2 ? R::cancelled : state&4 ? R::timed_out : R::acquired;
      Check(result==expected,"promotion commitment survives result-delivery signals");
      if (result==R::acquired)
        Check(latch.PromoteUpgradeToExclusive(grant,{},{},owner)==R::invalid,
              "delayed promotion result still owns exclusive mode");
      else
        Check(latch.PromoteUpgradeToExclusive(grant,{},{},owner)==(state&1?R::closed:R::acquired),
              "precommit refusal still owns upgrade for cleanup or retry");
      delivered.release(); finish.acquire();
      Check(latch.Release(grant,owner),"delayed promotion keeps actual native release ownership");
    });
    Check(gap.entered.try_acquire_for(2s),"controlled promotion boundary reached");
    Check(!returned,"promotion result not delivered at controlled boundary");
    if (state&1) latch.Close();
    if (state&2) stop.request_stop();
    if (state&4) {
      std::this_thread::sleep_until(deadline);
      Check(L::Clock::now()>=deadline,"real promotion deadline expired while delivery paused");
    }
    const auto paused=latch.Observe();
    Check(paused.holders==1 && paused.waiters==0 && paused.calls==0,
          "promotion close boundary never loses the real holder");
    gap.proceed.release();
    Check(delivered.try_acquire_for(2s),"promotion delivery completed");
    latch.Close();
    Check(latch.Observe().holders==1,"close after delivery cannot revoke promotion ownership");
    finish.release(); worker.join(); Empty(latch);
  }
}
void UpgradePromotion() {
  L::OwnerIdentity owner{}; owner[6]=0x70; owner[8]=0x80; owner[15]=1;
  auto wrong=owner; wrong[15]=2;
  for (unsigned mode=0;mode<11;++mode) for (unsigned state=0;state<8;++state) {
    L latch(1,2); L::Grant grant;
    Check(latch.TryAcquire(grant,Mode(mode),{},{},owner)==R::acquired);
    std::stop_source stop;
    if (state&1) latch.Close();
    if (state&2) stop.request_stop();
    const auto deadline=state&4 ? L::Clock::now() : L::Clock::now()+3s;
    const auto result=latch.PromoteUpgradeToExclusive(grant,deadline,stop.get_token(),owner);
    const auto expected=Mode(mode)!=M::upgrade ? R::invalid :
        state&1 ? R::closed : state&2 ? R::cancelled : state&4 ? R::timed_out : R::acquired;
    Check(result==expected,"promotion validates mode and terminal precedence");
    const auto snapshot=latch.Observe();
    Check(snapshot.holders==1 && snapshot.waiters==0 && snapshot.calls==0,
          "promotion never releases or registers another waiter");
    if (Mode(mode)==M::upgrade) {
      if (!state) {
        Check(latch.PromoteUpgradeToExclusive(grant,{},{},owner)==R::invalid,
              "successful promotion changes the actual native held mode");
        latch.Close(); stop.request_stop();
      } else {
        Check(latch.PromoteUpgradeToExclusive(grant,{},{},owner)==(state&1?R::closed:R::acquired),
              "refused promotion retains the original upgrade mode");
      }
    }
    Check(!latch.Release(grant,wrong),"promotion preserves exact binary owner");
    Check(latch.Release(grant,owner),"promotion retains original release path after terminals");
    Empty(latch);
  }
  {
    L latch(1,2),other(1,2); L::Grant grant,empty;
    Check(latch.PromoteUpgradeToExclusive(empty)==R::invalid);
    Check(latch.TryAcquire(grant,M::upgrade,{},{},owner)==R::acquired);
    Check(other.PromoteUpgradeToExclusive(grant,{},{},owner)==R::invalid);
    Check(latch.PromoteUpgradeToExclusive(grant,{},{},wrong)==R::invalid);
    Check(latch.PromoteUpgradeToExclusive(grant)==R::invalid);
    std::thread foreign([&] {
      Check(latch.PromoteUpgradeToExclusive(grant,{},{},owner)==R::invalid,
            "foreign execution cannot promote a borrowed upgrade record");
    });
    foreign.join();
    fail_wait=true;
    Check(latch.PromoteUpgradeToExclusive(grant,{},{},owner)==R::acquired,
          "promotion performs no native wait");
    fail_wait=false;
    std::thread contender([&] {
      for (unsigned mode=0;mode<11;++mode) {
        L::Grant refused;
        Check(latch.TryAcquire(refused,Mode(mode))==R::exhausted,
              "promoted holder still occupies the admitted holder capacity");
      }
    });
    contender.join();
    Check(latch.Release(grant,owner)); Empty(latch); Empty(other);
  }
  {
    L latch(2,2); L::Grant grant;
    Check(latch.TryAcquire(grant,M::upgrade,{},{},owner)==R::acquired);
    std::binary_semaphore entered_first{0},entered_second{0};
    std::atomic<unsigned> order{0};
    std::thread first([&] {
      L::Grant held; parked=&entered_first;
      Check(latch.Acquire(held,M::exclusive_write,9,L::Clock::now()+5s)==R::acquired);
      Check(order.fetch_add(1)==1,"promotion preserves admitted rank order");
      Check(latch.Release(held));
    });
    Check(entered_first.try_acquire_for(2s));
    std::thread second([&] {
      L::Grant held; parked=&entered_second;
      Check(latch.Acquire(held,M::shared_read,0,L::Clock::now()+5s)==R::acquired);
      Check(order.fetch_add(1)==0,"promotion does not rearrange queued contenders");
      Check(latch.Release(held));
    });
    Check(entered_second.try_acquire_for(2s));
    Check(latch.PromoteUpgradeToExclusive(grant,{},{},owner)==R::acquired);
    const auto snapshot=latch.Observe();
    Check(snapshot.holders==1 && snapshot.waiters==2 && snapshot.calls==2 && order==0,
          "queued contenders remain blocked through in-place promotion");
    Check(latch.Release(grant,owner)); first.join(); second.join(); Empty(latch);
  }
}
void Matrix() {
  L::OwnerIdentity shared_task{}; shared_task[0]=42;
  for (unsigned held=0; held<11; ++held) for (unsigned requested=0; requested<11; ++requested) {
    L latch(4, 4);
    L::Grant first;
    Check(latch.TryAcquire(first, Mode(held), {}, {}, shared_task) == R::acquired);
    std::thread contender([&] {
      L::Grant second;
      const auto result = latch.TryAcquire(second, Mode(requested), {}, {}, shared_task);
      Check(result == (matrix[requested][held] == 'Y' ? R::acquired : R::busy));
      if (result == R::acquired) Check(latch.Release(second, shared_task));
    });
    contender.join();
    Check(latch.Release(first, shared_task));
    Empty(latch);
  }
  // Every compatible pair of existing holders: the requested mode must agree
  // with BOTH, including a conflict hidden behind a compatible list head.
  for (unsigned a=0; a<11; ++a) for (unsigned b=0; b<11; ++b) {
    if (matrix[a][b] != 'Y') continue;
    for (unsigned request=0; request<11; ++request) {
      L latch(3, 3);
      L::Grant first;
      Check(latch.TryAcquire(first, Mode(a)) == R::acquired);
      std::latch ready(1), release(1);
      std::thread other([&] {
        L::Grant second;
        Check(latch.TryAcquire(second, Mode(b)) == R::acquired);
        ready.count_down(); release.wait();
        Check(latch.Release(second));
      });
      ready.wait();
      std::thread contender([&] {
        L::Grant third;
        auto result = latch.TryAcquire(third, Mode(request));
        Check(result == (matrix[request][a]=='Y' && matrix[request][b]=='Y' ? R::acquired : R::busy));
        if (result == R::acquired) Check(latch.Release(third));
      });
      contender.join(); release.count_down(); other.join();
      Check(latch.Release(first)); Empty(latch);
    }
  }
}
void Terminal() {
  for (unsigned mode=0; mode<11; ++mode) for (unsigned state=0; state<8; ++state)
    for (unsigned existing=0; existing<12; ++existing) for (bool immediate : {false, true}) {
      L latch(3, 3);
      L::Grant held;
      if (existing) Check(latch.TryAcquire(held, Mode(existing-1)) == R::acquired);
      std::stop_source stop;
      if (state&1) latch.Close();
      if (state&2) stop.request_stop();
      const auto deadline = state&4 ? L::Clock::now() : L::Clock::now()+2s;
      std::thread contender([&] {
        L::Grant grant;
        const bool compatible = !existing || matrix[mode][existing-1]=='Y';
        // An unblocked acquisition exercises both entry paths. Without a
        // terminal condition, contention uses TryAcquire rather than a delay.
        const auto result = immediate || (!state && !compatible)
            ? latch.TryAcquire(grant, Mode(mode), deadline, stop.get_token())
            : latch.Acquire(grant, Mode(mode), 0, deadline, stop.get_token());
        const auto expected = state&1 ? R::closed : state&2 ? R::cancelled :
            state&4 ? R::timed_out : compatible ? R::acquired : R::busy;
        Check(result == expected);
        if (result == R::acquired) Check(latch.Release(grant));
      });
      contender.join();
      if (existing) Check(latch.Release(held));
      Empty(latch);
    }
}
void Recursion() {
  for (unsigned held=0;held<11;++held) for (unsigned requested=0;requested<11;++requested)
    for (unsigned state=0;state<8;++state) for (bool rebound : {false,true}) {
      L latch(4,4);
      L::Grant grant, refused;
      L::OwnerIdentity owner{}, changed{}; owner[0]=1; changed[0]=2;
      Check(latch.TryAcquire(grant,Mode(held),{}, {},owner)==R::acquired);
      std::stop_source stop;
      if (state&1) latch.Close();
      if (state&2) stop.request_stop();
      const auto deadline=state&4 ? L::Clock::now() : L::Clock::now()+1s;
      Check(latch.Preflight(deadline,stop.get_token())==R::recursive);
      Check(latch.TryAcquire(refused,Mode(requested),deadline,stop.get_token(),rebound ? changed : owner)==R::recursive);
      Check(latch.Acquire(refused,Mode(requested),0,deadline,stop.get_token(),rebound ? changed : owner)==R::recursive);
      const auto snapshot=latch.Observe();
      Check(snapshot.holders==1 && !snapshot.waiters && !snapshot.calls);
      Check(latch.Release(grant,owner)); Empty(latch);
    }
}
void Queues() {
  // Priority rank chosen by the caller, FIFO ties, no immediate barging. Each
  // grant is held until the driver releases it, making the order observable.
  L latch(8, 8);
  L::Grant blocker;
  Check(latch.TryAcquire(blocker, M::exclusive_write) == R::acquired);
  std::array<std::binary_semaphore, 4> entered{
      std::binary_semaphore(0), std::binary_semaphore(0), std::binary_semaphore(0), std::binary_semaphore(0)};
  std::array<std::binary_semaphore, 4> granted{
      std::binary_semaphore(0), std::binary_semaphore(0), std::binary_semaphore(0), std::binary_semaphore(0)};
  std::array<std::binary_semaphore, 4> release{
      std::binary_semaphore(0), std::binary_semaphore(0), std::binary_semaphore(0), std::binary_semaphore(0)};
  constexpr unsigned ranks[]{5, 2, 2, 4};
  constexpr unsigned order[]{1, 2, 3, 0};
  std::array<unsigned, 4> actual{};
  std::atomic<unsigned> next{0};
  std::vector<std::thread> threads;
  for (unsigned i=0; i<4; ++i) {
    threads.emplace_back([&, i] {
      L::Grant grant;
      parked = &entered[i];
      Check(latch.Acquire(grant, M::exclusive_write, ranks[i], L::Clock::now()+5s) == R::acquired);
      actual[next.fetch_add(1)] = i;
      granted[i].release(); release[i].acquire();
      Check(latch.Release(grant));
    });
    Check(entered[i].try_acquire_for(2s));
    Check(latch.Observe().waiters == i+1);
  }
  L::Grant barger;
  std::thread immediate([&] { Check(latch.TryAcquire(barger, M::shared_read) == R::busy); });
  immediate.join();
  Check(latch.Release(blocker));
  for (auto i : order) { Check(granted[i].try_acquire_for(2s)); release[i].release(); }
  for (auto& thread : threads) thread.join();
  Check(actual == std::array<unsigned, 4>{1, 2, 3, 0});
  Empty(latch);
}
void ParkedOutcomes() {
  for (unsigned mode=0; mode<11; ++mode) for (unsigned ending=0; ending<4; ++ending) {
    L latch(2, 2);
    L::Grant holder;
    Check(latch.TryAcquire(holder, M::exclusive_write) == R::acquired);
    std::binary_semaphore entered(0);
    std::stop_source stop;
    std::thread waiter([&] {
      L::Grant grant;
      parked = &entered;
      const auto result = latch.Acquire(grant, Mode(mode), 0,
          L::Clock::now() + (ending==2 ? 50ms : 2s), stop.get_token());
      Check(result == (ending==0 ? R::closed : ending==1 ? R::cancelled :
                      ending==2 ? R::timed_out : R::acquired));
      if (result == R::acquired) Check(latch.Release(grant));
    });
    Check(entered.try_acquire_for(2s));
    if (ending==0) latch.Close();
    if (ending==1) stop.request_stop();
    if (ending==3) Check(latch.Release(holder));
    waiter.join();
    if (ending!=3) Check(latch.Release(holder));
    Empty(latch);
  }
}
void Boundaries() {
  L latch(1, 0), other(1, 1), zero(0, 1);
  L::Grant grant, extra;
  L::OwnerIdentity owner{}, wrong{};
  owner[0]=1; wrong[15]=1;
  Check(latch.TryAcquire(grant, M::none) == R::invalid);
  Check(latch.TryAcquire(grant, static_cast<M>(255)) == R::invalid);
  Check(zero.TryAcquire(grant, M::shared_read) == R::exhausted);
  Check(zero.Acquire(grant, M::shared_read, 0) == R::exhausted);
  Check(latch.TryAcquire(grant, M::shared_read, {}, {}, owner) == R::acquired);
  Check(latch.TryAcquire(grant, M::exclusive_write) == R::invalid);
  Check(other.TryAcquire(grant, M::shared_read) == R::invalid);
  Check(!other.Release(grant, owner));
  Check(!latch.Release(grant, wrong));
  std::thread nonowner([&] { Check(!latch.Release(grant, owner)); });
  nonowner.join();
  Check(latch.TryAcquire(extra, M::shared_read) == R::recursive);
  Check(latch.Acquire(extra, M::shared_read, 0) == R::recursive);
  std::thread capacity([&] {
    Check(latch.TryAcquire(extra, M::shared_read) == R::exhausted);
    Check(latch.Acquire(extra, M::shared_read, 0) == R::exhausted);
  });
  capacity.join();
  latch.Close();
  Check(latch.TryAcquire(extra, M::shared_read) == R::recursive);
  Check(latch.Release(grant, owner));
  Check(!latch.Release(grant, owner)); Empty(latch);
  // Native wait errors remove registrations and retain existing ownership.
  for (bool timed : {false, true}) {
    L failed(2, 2);
    Check(failed.TryAcquire(grant, M::exclusive_write) == R::acquired);
    std::thread waiter([&] {
      L::Grant waiting;
      fail_wait=true;
      Check(failed.Acquire(waiting, M::shared_read, 0,
            timed ? std::optional(L::Clock::now()+2s) : std::nullopt) == R::failed);
      fail_wait=false;
    });
    waiter.join();
    Check(failed.Observe().holders==1);
    Check(failed.Release(grant)); Empty(failed);
  }
}
void BatchAndHeadRemoval() {
  for (bool close_batch : {false, true}) {
    L latch(4, 4);
    L::Grant blocker;
    Check(latch.TryAcquire(blocker, M::exclusive_write) == R::acquired);
    std::latch all_granted(3), release(1);
    std::array<std::binary_semaphore, 3> entered{
        std::binary_semaphore(0), std::binary_semaphore(0), std::binary_semaphore(0)};
    std::vector<std::thread> readers;
    for (unsigned i=0; i<3; ++i) {
      readers.emplace_back([&, i] {
        L::Grant grant;
        parked=&entered[i];
        Check(latch.Acquire(grant, M::shared_read, 0, L::Clock::now()+2s)==R::acquired);
        all_granted.count_down(); release.wait();
        Check(latch.Release(grant));
      });
      Check(entered[i].try_acquire_for(2s));
      Check(latch.Observe().waiters==i+1);
    }
    Check(latch.Release(blocker));
    all_granted.wait();
    Check(latch.Observe().holders==3);
    if (close_batch) {
      latch.Close();
      L::Grant late;
      Check(latch.TryAcquire(late, M::shared_read)==R::closed);
      Check(latch.Observe().holders==3);
    }
    release.count_down();
    for (auto& reader:readers) reader.join();
    Empty(latch);
  }
  // Removing an incompatible head admits its compatible successor without
  // waiting for an unrelated existing reader to release.
  L latch(3, 2);
  L::Grant held;
  Check(latch.TryAcquire(held, M::shared_read)==R::acquired);
  std::stop_source stop;
  std::binary_semaphore first_entered(0), second_entered(0), second_granted(0);
  std::thread first([&] {
    L::Grant grant; parked=&first_entered;
    Check(latch.Acquire(grant, M::exclusive_write, 0, L::Clock::now()+2s,
                        stop.get_token())==R::cancelled);
  });
  Check(first_entered.try_acquire_for(2s));
  std::thread second([&] {
    L::Grant grant; parked=&second_entered;
    Check(latch.Acquire(grant, M::shared_read, 0, L::Clock::now()+2s)==R::acquired);
    second_granted.release();
    Check(latch.Release(grant));
  });
  Check(second_entered.try_acquire_for(2s));
  L::Grant overflow;
  std::thread capacity([&] { Check(latch.Acquire(overflow, M::shared_read, 0)==R::exhausted); });
  capacity.join();
  Check(latch.Observe().waiters==2);
  stop.request_stop();
  Check(second_granted.try_acquire_for(2s));
  first.join(); second.join();
  Check(latch.Observe().holders==1);
  Check(latch.Release(held)); Empty(latch);
}
void WakeSelection() {
  for (unsigned state=0; state<8; ++state) for (unsigned mode=0; mode<11; ++mode) {
    L latch(3, 3);
    L::Grant held;
    Check(latch.TryAcquire(held, M::exclusive_write)==R::acquired);
    std::binary_semaphore entered(0), awaken(0), proceed(0);
    std::stop_source stop;
    const auto deadline=L::Clock::now()+(state&4 ? 40ms : 3s);
    std::thread waiter([&] {
      L::Grant grant;
      parked=&entered; woke=&awaken; resume=&proceed;
      const auto result=latch.Acquire(grant, Mode(mode), 0, deadline, stop.get_token());
      Check(result==(state&1 ? R::closed : state&2 ? R::cancelled : state&4 ? R::timed_out : R::acquired));
      if (result==R::acquired) Check(latch.Release(grant));
    });
    Check(entered.try_acquire_for(2s));
    Check(latch.Release(held));
    Check(awaken.try_acquire_for(2s));
    if (state&1) latch.Close();
    if (state&2) stop.request_stop();
    if (state&4) std::this_thread::sleep_until(deadline);
    proceed.release(); waiter.join(); Empty(latch);
  }
  // First reader has committed; second compatible batch member is awake but
  // has not committed. Close must retain the first and reject the second.
  L latch(3, 3);
  L::Grant held;
  Check(latch.TryAcquire(held, M::exclusive_write)==R::acquired);
  std::binary_semaphore entered1(0), entered2(0), granted(0), release(0), awaken(0), proceed(0);
  std::thread first([&] {
    L::Grant grant; parked=&entered1;
    Check(latch.Acquire(grant, M::shared_read, 0, L::Clock::now()+3s)==R::acquired);
    granted.release(); release.acquire();
    Check(latch.Release(grant));
  });
  Check(entered1.try_acquire_for(2s));
  std::thread second([&] {
    L::Grant grant; parked=&entered2; woke=&awaken; resume=&proceed;
    Check(latch.Acquire(grant, M::shared_read, 0, L::Clock::now()+3s)==R::closed);
  });
  Check(entered2.try_acquire_for(2s));
  Check(latch.Release(held));
  Check(granted.try_acquire_for(2s)); Check(awaken.try_acquire_for(2s));
  latch.Close();
  Check(latch.Observe().holders==1);
  proceed.release(); second.join();
  Check(latch.Observe().holders==1);
  release.release(); first.join(); Empty(latch);
}
} // namespace
extern "C" int __real_pthread_cond_wait(pthread_cond_t*, pthread_mutex_t*);
extern "C" int __real_pthread_mutex_lock(pthread_mutex_t*);
extern "C" int __real_pthread_mutex_unlock(pthread_mutex_t*);
extern "C" int __wrap_pthread_mutex_lock(pthread_mutex_t* mutex) {
  if (promotion_gap && !promotion_gap->after_unlock) {
    auto* gap=promotion_gap; promotion_gap=nullptr;
    gap->entered.release(); gap->proceed.acquire();
  }
  return __real_pthread_mutex_lock(mutex);
}
extern "C" int __wrap_pthread_mutex_unlock(pthread_mutex_t* mutex) {
  auto* gap=promotion_gap && promotion_gap->after_unlock ? promotion_gap : nullptr;
  if (gap) promotion_gap=nullptr;
  const auto result=__real_pthread_mutex_unlock(mutex);
  if (gap) { gap->entered.release(); gap->proceed.acquire(); }
  return result;
}
extern "C" int __real_pthread_cond_timedwait(pthread_cond_t*, pthread_mutex_t*, const timespec*);
extern "C" int __wrap_pthread_cond_wait(pthread_cond_t* cond, pthread_mutex_t* mutex) {
  WaitEntered();
  return fail_wait ? EINVAL : AfterWake(__real_pthread_cond_wait(cond, mutex), mutex);
}
extern "C" int __wrap_pthread_cond_timedwait(pthread_cond_t* cond, pthread_mutex_t* mutex, const timespec* time) {
  WaitEntered();
  return fail_wait ? EINVAL : AfterWake(__real_pthread_cond_timedwait(cond, mutex, time), mutex);
}
int main() {
  UpgradePromotion();
  PromotionCloseBoundary();
  Matrix(); Terminal(); Recursion(); Queues(); ParkedOutcomes(); Boundaries(); BatchAndHeadRemoval(); WakeSelection();
  std::printf("PASS checked mode latch: %u checks\n", checks.load());
}

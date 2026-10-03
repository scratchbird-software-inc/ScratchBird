// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "checked_condition.hpp"
#include <array>
#include <cstdint>
#include <functional>
#include <span>
#include <stop_token>
#include <thread>

namespace scratchbird::core::platform {

// Native, thread-affine ordinary FIFO parking mechanism, not an engine latch
// descriptor or an authority grant. The owning latch layer supplies identity,
// memory retention, lock order, diagnostics and qualified exception handling.
// No dynamic waiter allocation: a registered call retains its stack node until
// unlink and cancellation callback join. The owner must join all calls and
// release the holder before destroying this object (Observe is observation only).
class CheckedFifoMutex {
 public:
  using Clock = std::chrono::steady_clock;
  // Opaque binary execution-owner token, not an authority or UUID issuer. The
  // owning layer validates it. Unbound use remains native-thread-affine only.
  using OwnerIdentity = std::array<std::uint8_t, 16>;
  enum class Result { acquired, busy, recursive, exhausted, timed_out, cancelled, closed, failed };
  // Caller-owned per-call sample; never read concurrently with the call. A
  // registered call counts once even after spurious wakes. Duration ends at
  // native outcome selection, excluding callback join and result delivery.
  // This is measurement input, not an activated metric or completion receipt.
  struct WaitEvidence {
    bool registered = false;
    std::uint64_t duration_us = 0;
  };
  explicit CheckedFifoMutex(std::uint32_t waiter_limit) : limit_(waiter_limit) {}
  CheckedFifoMutex(const CheckedFifoMutex&) = delete;
  CheckedFifoMutex& operator=(const CheckedFifoMutex&) = delete;
  ~CheckedFifoMutex() noexcept {
    std::lock_guard lock(mutex_);
    if (held_ || head_ || tail_ || waiters_ || calls_) std::terminate();
  }

  Result TryLock(std::optional<Clock::time_point> deadline = {}, std::stop_token stop = {},
                 std::optional<OwnerIdentity> owner = {}, WaitEvidence* evidence = nullptr) {
    if (evidence) *evidence = {};
    std::lock_guard lock(mutex_);
    if (Recursive(owner)) return Result::recursive;
    if (const auto terminal = Terminal(deadline, stop)) return *terminal;
    if (held_ || head_) return Result::busy;
    Grant(owner);
    return Result::acquired;
  }

  // For a valid ungranted request, select closed, cancelled, expired, then grant
  // under the commit mutex. The owning layer still validates identity/policy.
  Result Lock(std::optional<Clock::time_point> deadline, std::stop_token stop = {},
              std::optional<OwnerIdentity> owner = {}, WaitEvidence* evidence = nullptr) {
    if (evidence) *evidence = {};
    std::unique_lock lock(mutex_);
    if (Recursive(owner)) return Result::recursive;
    if (const auto terminal = Terminal(deadline, stop)) return *terminal;
    if (!held_ && !head_) { Grant(owner); return Result::acquired; }
    if (waiters_ == limit_ || calls_ == limit_)
      return Result::exhausted;
    Node node{tail_, nullptr, {std::this_thread::get_id(), owner, Clock::now(), deadline, false}, stop};
    if (tail_) tail_->next = &node; else head_ = &node;
    tail_ = &node;
    ++waiters_; ++calls_;
    if (registered_waits_ != UINT64_MAX) ++registered_waits_;

    // stop_callback may invoke inline. Registration and destruction must never
    // hold the mutex that the callback acquires. calls_ retains this whole gap.
    lock.unlock();
    std::optional<std::stop_callback<Wake>> callback;
    callback.emplace(stop, Wake{this});
    Relock(lock);
    Result result = Result::failed;
    try {
      for (;;) {
        if (Recursive(owner)) { result = Result::recursive; break; }
        if (const auto terminal = Terminal(deadline, stop)) { result = *terminal; break; }
        if (!held_ && head_ == &node) { Grant(owner); result = Result::acquired; break; }
        if (!changed_.Wait(lock, deadline)) break;
      }
    } catch (...) {
      if (!lock.owns_lock()) std::terminate();
      // Native failure is not a grant or permission to release another holder.
    }
    if (evidence) {
      const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
          Clock::now() - node.observation.started).count();
      *evidence = {true, elapsed > 0 ? static_cast<std::uint64_t>(elapsed) : 0};
    }
    if (node.previous) node.previous->next = node.next; else head_ = node.next;
    if (node.next) node.next->previous = node.previous; else tail_ = node.previous;
    --waiters_;
    Notify(); // Removing the head can make the next waiter eligible.
    lock.unlock();
    callback.reset();
    Relock(lock);
    --calls_;
    return result;
  }

  // False leaves ownership unchanged. Notification failure after committed
  // release cannot be reported as a retryable unlock: fail fast instead.
  bool Unlock(std::optional<OwnerIdentity> owner = {}) {
    std::lock_guard lock(mutex_);
    if (!held_ || holder_ != std::this_thread::get_id() || owner != owner_) return false;
    held_ = false; holder_ = {}; owner_.reset();
    Notify();
    return true;
  }
  // Serialized with grant commit. This acknowledges close publication and wake
  // initiation only, not holder release, callback join or safe destruction.
  void Close() {
    std::lock_guard lock(mutex_);
    closed_ = true;
    Notify();
  }
  struct Observation {
    bool held;
    std::uint32_t waiters;
    std::uint32_t calls;
    bool closed;
    std::uint64_t registered_waits = 0; // Saturates; never authority to reclaim.
  };
  // Select an existing rejection/terminal outcome before owning-layer memory
  // admission. No result is not a grant or queue position: Lock/TryLock must
  // revalidate under their commit synchronization after preparing storage.
  std::optional<Result> Preflight(std::optional<Clock::time_point> deadline,
      std::stop_token stop = {}, std::optional<OwnerIdentity> owner = {}) {
    std::lock_guard lock(mutex_);
    if (Recursive(owner)) return Result::recursive;
    return Terminal(deadline, stop);
  }
  Observation Observe() {
    std::lock_guard lock(mutex_);
    return {held_, waiters_, calls_, closed_, registered_waits_};
  }
  struct WaiterObservation {
    std::thread::id thread{};
    std::optional<OwnerIdentity> owner;
    Clock::time_point started{};
    std::optional<Clock::time_point> deadline;
    bool cancellation_requested = false;
  };
  struct WaitObservation {
    Observation state{};
    std::thread::id holder{};
    std::optional<OwnerIdentity> owner;
    bool complete = false;
  };
  // Protected owning-layer input, not a public diagnostic or deadlock verdict.
  // Holder and FIFO queue are copied under the actual grant/queue mutex. The
  // borrowed destination is never retained; no node pointers or stop tokens
  // escape. Capacity failure leaves it untouched and reports required waiters.
  // Only the first state.waiters entries are valid when complete. The caller
  // owns storage admission and disclosure. An observation of one latch cannot
  // prove a consistent cross-latch cycle or authorize victim cancellation.
  // Cancellation flags are individual atomic samples, not acknowledgements.
  WaitObservation ObserveWaiters(std::span<WaiterObservation> output) {
    std::lock_guard lock(mutex_);
    return CopyWaiters(output);
  }
  // Caller-owned bounded capture frame. The mutexes and all spans must remain
  // alive and exclusively borrowed for the whole call; entries must not own
  // locks at entry. No latch pointer or frame is retained by the implementation.
  struct WaitSetEntry {
    CheckedFifoMutex* mutex = nullptr;
    WaitObservation observation;
    std::size_t offset = 0;
    std::unique_lock<std::mutex> lock;
  };
  enum class WaitSetResult { invalid, exhausted, insufficient_capacity, captured, synchronization_failed };
  // A consistent capture of the supplied set, NOT evidence that it contains
  // all runtime latches or current task-phase authority. Locks are native commit
  // locks, not semantic latch grants. Acquire in a total pointer order, with
  // bounded O(n^2) local selection and blocking lock calls, never try-lock/yield
  // loops. Input order is preserved; output is packed by that order. All native
  // locks remain held until every copy finishes. Stop flags remain individual
  // atomic samples. The owning layer admits frame/output memory and disclosure.
  static WaitSetResult ObserveWaitSet(std::span<WaitSetEntry> entries,
      std::span<WaiterObservation> output, std::uint32_t max_latches) {
    using R = WaitSetResult;
    if (!max_latches) return R::invalid;
    if (entries.size()>max_latches) return R::exhausted;
    for (std::size_t i=0;i<entries.size();++i) {
      if (!entries[i].mutex || entries[i].lock.owns_lock()) return R::invalid;
      for (std::size_t j=0;j<i;++j)
        if (entries[j].mutex==entries[i].mutex) return R::invalid;
    }
    struct Release {
      std::span<WaitSetEntry> entries;
      ~Release() noexcept { for (auto& entry:entries) entry.lock={}; }
    } release{entries};
    CheckedFifoMutex* previous=nullptr;
    const std::less<CheckedFifoMutex*> less;
    try {
      for (std::size_t count=0;count<entries.size();++count) {
        WaitSetEntry* next=nullptr;
        for (auto& entry:entries)
          if ((!previous || less(previous,entry.mutex)) &&
              (!next || less(entry.mutex,next->mutex))) next=&entry;
        if (!next) std::terminate(); // Validated distinct, non-null input set.
        next->lock=std::unique_lock(next->mutex->mutex_);
        previous=next->mutex;
      }
    } catch (const std::system_error&) { return R::synchronization_failed; }
    auto remaining=output.size();
    bool complete=true;
    for (auto& entry:entries) {
      const auto& m=*entry.mutex;
      entry.observation={{m.held_,m.waiters_,m.calls_,m.closed_},m.holder_,m.owner_,false};
      entry.offset=0;
      if (m.waiters_>remaining) complete=false;
      else remaining-=m.waiters_;
    }
    if (!complete) return R::insufficient_capacity; // No output prefix written.
    std::size_t offset=0;
    for (auto& entry:entries) {
      entry.offset=offset;
      entry.observation=entry.mutex->CopyWaiters(output.subspan(offset));
      offset+=entry.observation.state.waiters;
    }
    return R::captured;
  }
 private:
  WaitObservation CopyWaiters(std::span<WaiterObservation> output) {
    WaitObservation result{{held_, waiters_, calls_, closed_, registered_waits_}, holder_, owner_,
                           output.size() >= waiters_};
    if (!result.complete) return result;
    std::size_t index=0;
    for (auto* node=head_; node; node=node->next) {
      output[index]=node->observation;
      output[index++].cancellation_requested=node->stop.stop_requested();
    }
    return result;
  }
  struct Node {
    Node* previous;
    Node* next;
    WaiterObservation observation;
    std::stop_token stop;
  };
  struct Wake {
    CheckedFifoMutex* owner;
    void operator()() const noexcept {
      std::lock_guard lock(owner->mutex_);
      owner->Notify();
    }
  };
  static void Relock(std::unique_lock<std::mutex>& lock) noexcept {
    try { lock.lock(); } catch (...) { std::terminate(); }
  }
  std::optional<Result> Terminal(std::optional<Clock::time_point> deadline,
                                  std::stop_token stop) const noexcept {
    if (closed_) return Result::closed;
    if (stop.stop_requested()) return Result::cancelled;
    if (deadline && Clock::now() >= *deadline) return Result::timed_out;
    return std::nullopt;
  }
  void Notify() noexcept { if (!changed_.NotifyAll()) std::terminate(); }
  bool Recursive(const std::optional<OwnerIdentity>& owner) const noexcept {
    return held_ && (holder_ == std::this_thread::get_id() || (owner && owner == owner_));
  }
  void Grant(const std::optional<OwnerIdentity>& owner) noexcept {
    held_ = true; holder_ = std::this_thread::get_id(); owner_ = owner;
  }
  const std::uint32_t limit_;
  std::mutex mutex_;
  CheckedCondition changed_;
  Node* head_ = nullptr;
  Node* tail_ = nullptr;
  std::thread::id holder_{};
  std::optional<OwnerIdentity> owner_;
  std::uint32_t waiters_ = 0;
  std::uint32_t calls_ = 0;
  std::uint64_t registered_waits_ = 0;
  bool held_ = false;
  bool closed_ = false;
};
} // namespace scratchbird::core::platform

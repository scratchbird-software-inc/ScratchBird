// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "checked_condition.hpp"
#include <array>
#include <cstdint>
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
  explicit CheckedFifoMutex(std::uint32_t waiter_limit) : limit_(waiter_limit) {}
  CheckedFifoMutex(const CheckedFifoMutex&) = delete;
  CheckedFifoMutex& operator=(const CheckedFifoMutex&) = delete;
  ~CheckedFifoMutex() noexcept {
    std::lock_guard lock(mutex_);
    if (held_ || head_ || tail_ || waiters_ || calls_) std::terminate();
  }

  Result TryLock(std::optional<Clock::time_point> deadline = {}, std::stop_token stop = {},
                 std::optional<OwnerIdentity> owner = {}) {
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
              std::optional<OwnerIdentity> owner = {}) {
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
  struct Observation { bool held; std::uint32_t waiters; std::uint32_t calls; bool closed; };
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
    return {held_, waiters_, calls_, closed_};
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
    WaitObservation result{{held_, waiters_, calls_, closed_}, holder_, owner_,
                           output.size() >= waiters_};
    if (!result.complete) return result;
    std::size_t index=0;
    for (auto* node=head_; node; node=node->next) {
      output[index]=node->observation;
      output[index++].cancellation_requested=node->stop.stop_requested();
    }
    return result;
  }
 private:
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
  bool held_ = false;
  bool closed_ = false;
};
} // namespace scratchbird::core::platform

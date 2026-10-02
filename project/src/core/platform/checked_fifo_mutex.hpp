// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "checked_condition.hpp"
#include <cstdint>
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
  enum class Result { acquired, busy, recursive, exhausted, timed_out, cancelled, closed, failed };
  explicit CheckedFifoMutex(std::uint32_t waiter_limit) : limit_(waiter_limit) {}
  CheckedFifoMutex(const CheckedFifoMutex&) = delete;
  CheckedFifoMutex& operator=(const CheckedFifoMutex&) = delete;
  ~CheckedFifoMutex() noexcept {
    std::lock_guard lock(mutex_);
    if (held_ || head_ || tail_ || waiters_ || calls_) std::terminate();
  }

  Result TryLock(std::optional<Clock::time_point> deadline = {}, std::stop_token stop = {}) {
    std::lock_guard lock(mutex_);
    if (held_ && holder_ == std::this_thread::get_id()) return Result::recursive;
    if (const auto terminal = Terminal(deadline, stop)) return *terminal;
    if (held_ || head_) return Result::busy;
    Grant();
    return Result::acquired;
  }

  // For a valid ungranted request, select closed, cancelled, expired, then grant
  // under the commit mutex. The owning layer still validates identity/policy.
  Result Lock(std::optional<Clock::time_point> deadline, std::stop_token stop = {}) {
    std::unique_lock lock(mutex_);
    if (held_ && holder_ == std::this_thread::get_id()) return Result::recursive;
    if (const auto terminal = Terminal(deadline, stop)) return *terminal;
    if (!held_ && !head_) { Grant(); return Result::acquired; }
    if (waiters_ == limit_ || calls_ == limit_)
      return Result::exhausted;
    Node node{tail_, nullptr};
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
        if (const auto terminal = Terminal(deadline, stop)) { result = *terminal; break; }
        if (!held_ && head_ == &node) { Grant(); result = Result::acquired; break; }
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
  bool Unlock() {
    std::lock_guard lock(mutex_);
    if (!held_ || holder_ != std::this_thread::get_id()) return false;
    held_ = false; holder_ = {};
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
  Observation Observe() {
    std::lock_guard lock(mutex_);
    return {held_, waiters_, calls_, closed_};
  }
 private:
  struct Node { Node* previous; Node* next; };
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
  void Grant() noexcept { held_ = true; holder_ = std::this_thread::get_id(); }
  const std::uint32_t limit_;
  std::mutex mutex_;
  CheckedCondition changed_;
  Node* head_ = nullptr;
  Node* tail_ = nullptr;
  std::thread::id holder_{};
  std::uint32_t waiters_ = 0;
  std::uint32_t calls_ = 0;
  bool held_ = false;
  bool closed_ = false;
};
} // namespace scratchbird::core::platform

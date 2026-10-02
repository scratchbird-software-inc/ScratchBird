// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "checked_condition.hpp"
#include <array>
#include <cstdint>
#include <stop_token>
#include <thread>

namespace scratchbird::core::platform {

// Native mechanism, not an engine latch descriptor or admission authority.
// The owner validates allowed modes, recursion, order, current execution and
// policy before entry, retains this object and every grant, and joins all calls
// before destruction. No conversion API: conversion barriers/generations belong
// to the owning layer. A rank is an already-admitted arbitration rank (smaller
// first), not an engine priority mapping. Equal ranks are FIFO; no bypass API.
class CheckedModeLatch {
 public:
  using Clock = std::chrono::steady_clock;
  using OwnerIdentity = std::array<std::uint8_t, 16>;
  enum class Mode : std::uint8_t {
    none, shared_read, intent_read, intent_write, exclusive_write, upgrade,
    cleanup, recovery, flush, eviction, publication, verification
  };
  enum class Result { acquired, busy, invalid, exhausted, timed_out, cancelled, closed, failed };

  // Intrusive, nonmoving caller-owned record. Exclusive borrow for Acquire and
  // Release; do not destroy or reuse until the call completes. An active record
  // cannot be reacquired, converted or silently revoked, even by Close.
  class Grant {
   public:
    Grant() = default;
    Grant(const Grant&) = delete;
    Grant& operator=(const Grant&) = delete;
    ~Grant() noexcept { if (latch_) std::terminate(); }
   private:
    friend class CheckedModeLatch;
    CheckedModeLatch* latch_ = nullptr;
    Grant* previous_ = nullptr;
    Grant* next_ = nullptr;
    Mode mode_ = Mode::none;
    std::thread::id thread_{};
    std::optional<OwnerIdentity> owner_;
  };

  CheckedModeLatch(std::uint32_t holder_limit, std::uint32_t waiter_limit)
      : holder_limit_(holder_limit), waiter_limit_(waiter_limit) {}
  CheckedModeLatch(const CheckedModeLatch&) = delete;
  CheckedModeLatch& operator=(const CheckedModeLatch&) = delete;
  ~CheckedModeLatch() noexcept {
    std::lock_guard lock(mutex_);
    if (holders_ || head_ || holder_count_ || waiters_ || calls_) std::terminate();
  }

  static constexpr bool Valid(Mode mode) noexcept {
    return mode >= Mode::shared_read && mode <= Mode::verification;
  }
  static constexpr bool Compatible(Mode requested, Mode held) noexcept {
    // Core baseline, requested row / held column. Invalid modes never grant.
    constexpr std::array<std::uint16_t, 11> rows{
      0x487, 0x487, 0x007, 0, 0, 0x020, 0, 0x483, 0, 0, 0x483};
    return Valid(requested) && Valid(held) &&
        (rows[static_cast<unsigned>(requested)-1] &
         (1U << (static_cast<unsigned>(held)-1))) != 0;
  }

  Result TryAcquire(Grant& grant, Mode mode,
      std::optional<Clock::time_point> deadline = {}, std::stop_token stop = {},
      std::optional<OwnerIdentity> owner = {}) {
    std::lock_guard lock(mutex_);
    if (!Valid(mode) || grant.latch_) return Result::invalid;
    if (auto result = Terminal(deadline, stop)) return *result;
    if (holder_count_ == holder_limit_) return Result::exhausted;
    if (head_ || !CanGrant(mode)) return Result::busy;
    Commit(grant, mode, owner);
    return Result::acquired;
  }

  Result Acquire(Grant& grant, Mode mode, std::uint32_t admitted_rank,
      std::optional<Clock::time_point> deadline = {}, std::stop_token stop = {},
      std::optional<OwnerIdentity> owner = {}) {
    std::unique_lock lock(mutex_);
    if (!Valid(mode) || grant.latch_) return Result::invalid;
    if (auto result = Terminal(deadline, stop)) return *result;
    if (!holder_limit_) return Result::exhausted;
    if (!head_ && holder_count_ < holder_limit_ && CanGrant(mode)) {
      Commit(grant, mode, owner);
      return Result::acquired;
    }
    if (waiters_ == waiter_limit_ || calls_ == waiter_limit_) return Result::exhausted;
    Node node{nullptr, nullptr, admitted_rank};
    // Stable priority insertion: FIFO within the supplied rank. Bounded local
    // traversal, not spinning for state changes. Every waiter parks natively.
    auto* next = head_;
    while (next && next->rank <= admitted_rank) next = next->next;
    node.next = next;
    node.previous = next ? next->previous : tail_;
    if (node.previous) node.previous->next = &node; else head_ = &node;
    if (next) next->previous = &node; else tail_ = &node;
    ++waiters_; ++calls_;

    lock.unlock(); // stop callback may execute inline and takes this mutex.
    std::optional<std::stop_callback<Wake>> callback;
    callback.emplace(stop, Wake{this});
    Relock(lock);
    Result result = Result::failed;
    try {
      for (;;) {
        if (auto terminal = Terminal(deadline, stop)) { result = *terminal; break; }
        if (head_ == &node && holder_count_ < holder_limit_ && CanGrant(mode)) {
          Commit(grant, mode, owner);
          result = Result::acquired;
          break;
        }
        if (!changed_.Wait(lock, deadline)) break;
      }
    } catch (...) {
      if (!lock.owns_lock()) std::terminate();
    }
    if (node.previous) node.previous->next = node.next; else head_ = node.next;
    if (node.next) node.next->previous = node.previous; else tail_ = node.previous;
    --waiters_;
    Notify(); // Next compatible member must perform its own terminal selection.
    lock.unlock();
    callback.reset(); // Join before releasing the whole-call lifetime count.
    Relock(lock);
    --calls_;
    return result;
  }

  bool Release(Grant& grant, std::optional<OwnerIdentity> owner = {}) {
    std::lock_guard lock(mutex_);
    if (grant.latch_ != this || grant.thread_ != std::this_thread::get_id() ||
        grant.owner_ != owner) return false;
    if (grant.previous_) grant.previous_->next_ = grant.next_; else holders_ = grant.next_;
    if (grant.next_) grant.next_->previous_ = grant.previous_;
    --holder_count_;
    grant.latch_ = nullptr;
    grant.previous_ = grant.next_ = nullptr;
    grant.mode_ = Mode::none;
    grant.thread_ = {};
    grant.owner_.reset();
    Notify();
    return true;
  }
  void Close() {
    std::lock_guard lock(mutex_);
    closed_ = true;
    Notify();
  }
  struct Observation { std::uint32_t holders, waiters, calls; bool closed; };
  // No grant or queue position is reserved. Owning memory preparation must
  // always re-enter Acquire/TryAcquire and reselect under commit synchronization.
  std::optional<Result> Preflight(std::optional<Clock::time_point> deadline,
                                   std::stop_token stop = {}) {
    std::lock_guard lock(mutex_);
    return Terminal(deadline, stop);
  }
  // Observation is not a lifetime fence, a drain receipt, or engine authority.
  Observation Observe() {
    std::lock_guard lock(mutex_);
    return {holder_count_, waiters_, calls_, closed_};
  }

 private:
  struct Node { Node* previous; Node* next; std::uint32_t rank; };
  struct Wake {
    CheckedModeLatch* latch;
    void operator()() const noexcept {
      std::lock_guard lock(latch->mutex_);
      latch->Notify();
    }
  };
  static void Relock(std::unique_lock<std::mutex>& lock) noexcept {
    try { lock.lock(); } catch (...) { std::terminate(); }
  }
  void Notify() noexcept { if (!changed_.NotifyAll()) std::terminate(); }
  std::optional<Result> Terminal(std::optional<Clock::time_point> deadline,
                                 std::stop_token stop) const noexcept {
    if (closed_) return Result::closed;
    if (stop.stop_requested()) return Result::cancelled;
    if (deadline && Clock::now() >= *deadline) return Result::timed_out;
    return {};
  }
  bool CanGrant(Mode mode) const noexcept {
    for (auto* holder = holders_; holder; holder = holder->next_)
      if (!Compatible(mode, holder->mode_)) return false;
    return true;
  }
  void Commit(Grant& grant, Mode mode, const std::optional<OwnerIdentity>& owner) noexcept {
    grant.latch_ = this;
    grant.mode_ = mode;
    grant.owner_ = owner;
    grant.thread_ = std::this_thread::get_id();
    grant.previous_ = nullptr;
    grant.next_ = holders_;
    if (holders_) holders_->previous_ = &grant;
    holders_ = &grant;
    ++holder_count_;
  }
  const std::uint32_t holder_limit_, waiter_limit_;
  std::mutex mutex_;
  CheckedCondition changed_;
  Grant* holders_ = nullptr;
  Node* head_ = nullptr;
  Node* tail_ = nullptr;
  std::uint32_t holder_count_ = 0, waiters_ = 0, calls_ = 0;
  bool closed_ = false;
};
} // namespace scratchbird::core::platform

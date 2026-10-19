// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <atomic>
#include <cstdint>
#include <exception>
#include <thread>
#include <utility>
#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif

namespace scratchbird::server {

enum class HostedProcessAdmissionError { none, opening, bound, wrong_process };

// One process-lifetime hosted-open reservation. It carries no database,
// storage, security or transaction authority. No mutex, allocation or callback
// is retained across host work. The owner must outlive every reservation.
class HostedProcessAdmission final {
  enum class State : std::uint8_t { unbound, opening, bound };
  static_assert(std::atomic<State>::is_always_lock_free);
  static std::uint64_t CurrentProcess() noexcept {
#if defined(_WIN32)
    return static_cast<std::uint64_t>(::_getpid());
#else
    return static_cast<std::uint64_t>(::getpid());
#endif
  }
 public:
  class Reservation final {
   public:
    Reservation() noexcept = default;
    Reservation(const Reservation&) = delete;
    Reservation& operator=(const Reservation&) = delete;
    Reservation(Reservation&& other) noexcept : owner_(Take(other)),
        process_(other.process_), thread_(other.thread_) {}
    Reservation& operator=(Reservation&&) = delete;
    ~Reservation() { Reset(); }
    explicit operator bool() const noexcept { CheckCustody(); return owner_ != nullptr; }
    void Reset() noexcept {
      CheckCustody();
      if (auto* owner = std::exchange(owner_, nullptr))
        owner->state_.store(State::unbound, std::memory_order_release);
    }
    // A successful hosted open permanently binds this process. Dropping its
    // copyable status/runtime handles must not allow a later database retarget.
    bool Commit() noexcept {
      CheckCustody();
      if (!owner_) return false;
      auto* owner = std::exchange(owner_, nullptr);
      owner->state_.store(State::bound, std::memory_order_release);
      return true;
    }
   private:
    friend class HostedProcessAdmission;
    explicit Reservation(HostedProcessAdmission& owner) noexcept : owner_(&owner) {}
    void CheckCustody() const noexcept {
      // Immutable even after reset/move: never inspect mutable ownership in
      // an inherited process or on a foreign thread before refusing access.
      if (process_ != CurrentProcess() || thread_ != std::this_thread::get_id())
        std::terminate();
    }
    static HostedProcessAdmission* Take(Reservation& other) noexcept {
      other.CheckCustody(); return std::exchange(other.owner_, nullptr);
    }
    HostedProcessAdmission* owner_ = nullptr;
    const std::uint64_t process_ = CurrentProcess();
    const std::thread::id thread_ = std::this_thread::get_id();
  };
  struct Result {
    HostedProcessAdmissionError error = HostedProcessAdmissionError::opening;
    Reservation reservation;
    bool ok() const noexcept { return error == HostedProcessAdmissionError::none && bool(reservation); }
  };
  HostedProcessAdmission() noexcept = default;
  HostedProcessAdmission(const HostedProcessAdmission&) = delete;
  HostedProcessAdmission& operator=(const HostedProcessAdmission&) = delete;
  bool InOwningProcess() const noexcept { return process_ == CurrentProcess(); }
  Result TryReserve() noexcept {
    Result out;
    if (!InOwningProcess()) { out.error = HostedProcessAdmissionError::wrong_process; return out; }
    State expected = State::unbound;
    if (!state_.compare_exchange_strong(expected, State::opening, std::memory_order_acq_rel)) {
      out.error = expected == State::bound ? HostedProcessAdmissionError::bound : HostedProcessAdmissionError::opening;
      return out;
    }
    out.error = HostedProcessAdmissionError::none;
    return {out.error, Reservation(*this)};
  }
 private:
  const std::uint64_t process_ = CurrentProcess();
  std::atomic<State> state_{State::unbound};
};
// The production legacy and native host paths share one actual process gate.
HostedProcessAdmission& ServerHostedProcessAdmission() noexcept;
} // namespace scratchbird::server

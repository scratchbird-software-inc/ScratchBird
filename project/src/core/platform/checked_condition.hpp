// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include <chrono>
#include <condition_variable>
#include <limits>
#include <mutex>
#include <optional>
#include <system_error>
#if defined(__linux__)
#include <cerrno>
#include <pthread.h>
#include <time.h>
#endif

namespace scratchbird::core::platform {
// std::condition_variable timed waits can discard a native error as an ordinary
// wake on some libraries. Keep explicit error reporting on the qualified Linux
// path; the interface remains portable and never substitutes a polling loop.
class CheckedCondition {
 public:
  CheckedCondition(const CheckedCondition&) = delete;
  CheckedCondition& operator=(const CheckedCondition&) = delete;
#if defined(__linux__)
  CheckedCondition() {
    pthread_condattr_t attributes;
    int error = pthread_condattr_init(&attributes);
    if (error) throw std::system_error(error, std::generic_category());
    error = pthread_condattr_setclock(&attributes, CLOCK_MONOTONIC);
    if (!error) error = pthread_cond_init(&condition_, &attributes);
    pthread_condattr_destroy(&attributes);
    if (error) throw std::system_error(error, std::generic_category());
  }
  ~CheckedCondition() noexcept {
    if (pthread_cond_destroy(&condition_) != 0) std::terminate();
  }
  bool NotifyAll() noexcept { return pthread_cond_broadcast(&condition_) == 0; }
  bool Wait(std::unique_lock<std::mutex>& lock, std::optional<std::chrono::steady_clock::time_point> deadline) {
    int error = 0;
    if (!deadline) error = pthread_cond_wait(&condition_, lock.mutex()->native_handle());
    else {
      const auto now = std::chrono::steady_clock::now();
      if (*deadline <= now) return true;
      const auto remaining = std::chrono::duration_cast<std::chrono::nanoseconds>(*deadline - now);
      timespec target{};
      if (clock_gettime(CLOCK_MONOTONIC, &target) != 0) return false;
      const auto seconds = remaining.count() / 1000000000;
      if (seconds >= std::numeric_limits<time_t>::max() - target.tv_sec) return false;
      target.tv_sec += static_cast<time_t>(seconds);
      target.tv_nsec += static_cast<long>(remaining.count() % 1000000000);
      if (target.tv_nsec >= 1000000000) { ++target.tv_sec; target.tv_nsec -= 1000000000; }
      error = pthread_cond_timedwait(&condition_, lock.mutex()->native_handle(), &target);
    }
    return error == 0 || error == ETIMEDOUT;
  }
 private:
  pthread_cond_t condition_;
#else
  CheckedCondition() = default;
  bool NotifyAll() noexcept { condition_.notify_all(); return true; }
  bool Wait(std::unique_lock<std::mutex>& lock, std::optional<std::chrono::steady_clock::time_point> deadline) {
    if (deadline) condition_.wait_until(lock, *deadline);
    else condition_.wait(lock);
    return true;
  }
 private:
  std::condition_variable condition_;
#endif
};

} // namespace scratchbird::core::platform


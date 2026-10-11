// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstdint>
#include <stdexcept>
#include <thread>

namespace scratchbird::server {

// Standalone product only. A separate native thread observes cooperative stop
// and enforces terminal process escalation even if a drain call is blocked.
// It never cancels an individual worker or runs engine/provider destructors.
// Keep this owner alive until all hosted resources have actually drained.
class StandaloneShutdownDeadline final {
 public:
  StandaloneShutdownDeadline(std::uint64_t timeout_ms, bool (*stop_requested)())
      : timeout_(timeout_ms), stop_requested_(stop_requested) {
    if (timeout_ms < 1000 || timeout_ms > 300000 || !stop_requested)
      throw std::invalid_argument("invalid standalone shutdown deadline");
    worker_ = std::thread([this] {
      auto deadline = std::chrono::steady_clock::time_point::max();
      while (!finished_.load(std::memory_order_acquire)) {
        const auto now = std::chrono::steady_clock::now();
        if (deadline == std::chrono::steady_clock::time_point::max() && stop_requested_())
          deadline = now + timeout_;
        if (now >= deadline) {
          // Deliberately no iostreams, provider allocation, flush, lock or
          // unwinding here: none can be relied upon after drain failure.
          // Missing clean engine receipt forces recovery on next open.
          std::_Exit(2);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
      }
    });
  }
  StandaloneShutdownDeadline(const StandaloneShutdownDeadline&) = delete;
  StandaloneShutdownDeadline& operator=(const StandaloneShutdownDeadline&) = delete;
  ~StandaloneShutdownDeadline() {
    finished_.store(true, std::memory_order_release);
    if (worker_.joinable()) worker_.join();
  }
 private:
  std::chrono::milliseconds timeout_;
  bool (*stop_requested_)();
  std::atomic<bool> finished_{false};
  std::thread worker_;
};

}  // namespace scratchbird::server

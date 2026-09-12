// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "uuid.hpp"
#include "time.hpp"

#include <chrono>
#include <limits>
#include <mutex>
#include <thread>
#if defined(_WIN32)
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace scratchbird::core::uuid {
namespace {
namespace time = scratchbird::core::time;

std::uint64_t ProcessIdentity() noexcept {
#if defined(_WIN32)
  return static_cast<std::uint64_t>(::GetCurrentProcessId());
#else
  return static_cast<std::uint64_t>(::getpid());
#endif
}

struct RuntimeGenerator {
  std::mutex mutex;
  std::uint64_t process = ProcessIdentity();
  time::LocalTimeAuthorityState clock;
  std::optional<std::uint64_t> last_millis;
  Uuid last_value;
};

// Increment exactly the 74 allocation bits. Work on an unpublished copy:
// exhaustion must neither wrap the retained value nor advance its timestamp.
bool IncrementAllocation(Uuid& value) noexcept {
  for (unsigned i = 15; i > 8; --i) {
    if (value.bytes[i] != 255) { ++value.bytes[i]; return true; }
    value.bytes[i] = 0;
  }
  if ((value.bytes[8] & 63) != 63) { ++value.bytes[8]; return true; }
  value.bytes[8] = 0x80;
  if (value.bytes[7] != 255) { ++value.bytes[7]; return true; }
  value.bytes[7] = 0;
  if ((value.bytes[6] & 15) != 15) { ++value.bytes[6]; return true; }
  return false;
}
} // namespace

std::optional<Uuid> IssueRuntimeIdentityV7() noexcept {
  try {
    // Each thread owns one generator instance. No mutex owned by a different
    // thread is inherited as this thread's generator when the process forks.
    // This bounded process-runtime state is not database/cluster time authority.
    thread_local RuntimeGenerator generator;
    const auto process = ProcessIdentity();
    if (generator.process != process) {
      // A child must not continue its parent's allocation suffix or clock
      // history, even when both read the same millisecond after fork.
      generator.process = process;
      generator.clock = {};
      generator.last_millis.reset();
      generator.last_value = {};
    }
    constexpr auto wait_limit = std::chrono::milliseconds(1000);
    std::optional<std::chrono::steady_clock::time_point> wait_started;
    for (;;) {
      if (wait_started && std::chrono::steady_clock::now() - *wait_started >= wait_limit)
        return std::nullopt;
      {
        std::lock_guard lock(generator.mutex);
        const auto snapshot = time::ReadLocalNodeClockSnapshot();
        if (!snapshot.ok() ||
            generator.clock.accepted_observations == (std::numeric_limits<u64>::max)())
          return std::nullopt;
        // High-security standalone runtime default: reject backward wall or
        // monotonic movement. No cluster state is fabricated from this clock.
        const auto observed = time::ObserveLocalNodeClock(
            generator.clock, snapshot.value, time::LocalTimeAuthorityPolicy{});
        if (!observed.ok()) return std::nullopt;
        const auto converted = time::WallClockToUuidV7Millis(snapshot.value.wall_clock);
        if (!converted.ok()) return std::nullopt;
        const auto millis = converted.unix_epoch_millis;
        if (generator.last_millis && millis < *generator.last_millis) return std::nullopt;

        Uuid candidate;
        bool available = true;
        if (!generator.last_millis || millis > *generator.last_millis) {
          const auto generated = GenerateCompatibilityUnixTimeV7(millis);
          if (!generated.ok()) return std::nullopt;
          candidate = generated.value;
        } else {
          candidate = generator.last_value;
          available = IncrementAllocation(candidate);
        }
        if (available) {
          if (wait_started && std::chrono::steady_clock::now() - *wait_started >= wait_limit)
            return std::nullopt;
          // Publish the complete binary allocation and its clock observation
          // together, only after every source and range check succeeded.
          generator.last_value = candidate;
          generator.last_millis = millis;
          generator.clock = observed.state;
          return candidate;
        }
      }
      // An exhausted suffix waits for an actual, strictly later accepted
      // physical millisecond. Never invent a future tick or reuse the maximum.
      const auto now = std::chrono::steady_clock::now();
      if (!wait_started) wait_started = now;
      if (now - *wait_started >= wait_limit) return std::nullopt;
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  } catch (...) {
    // The caller's optional failure channel also survives allocation, clock
    // and mutex failures. There is no diagnostic-UUID recursion or nil success.
    return std::nullopt;
  }
}
} // namespace scratchbird::core::uuid

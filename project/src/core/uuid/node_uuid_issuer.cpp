// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "node_uuid_issuer.hpp"

#include <chrono>
#include <limits>
#include <new>
#include <thread>
#include <type_traits>
#if defined(_WIN32)
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace scratchbird::core::uuid {
namespace {
using E = StandaloneUuidV7Error;
u64 ProcessIdentity() noexcept {
#if defined(_WIN32)
  return static_cast<u64>(::GetCurrentProcessId());
#else
  return static_cast<u64>(::getpid());
#endif
}
E Admit(const StandaloneUuidV7Binding& binding, const StandaloneUuidV7Policy& policy) {
  if (!IsEngineIdentityUuid(binding.database_uuid) || !IsEngineIdentityUuid(binding.policy_snapshot_uuid))
    return E::invalid_binding;
  if (!policy.same_ms_wait_timeout_ms ||
      policy.same_ms_wait_timeout_ms > (std::numeric_limits<u64>::max)() / 1000000 ||
      policy.max_uuid_regression_ms > 0x0000ffffffffffffULL)
    return E::invalid_policy;
  return E::none;
}
bool Increment(Uuid& value) noexcept {
  for (unsigned n = 15; n > 8; --n) {
    if (value.bytes[n] != 255) { ++value.bytes[n]; return true; }
    value.bytes[n] = 0;
  }
  if ((value.bytes[8] & 63) != 63) { ++value.bytes[8]; return true; }
  value.bytes[8] = 0x80;
  if (value.bytes[7] != 255) { ++value.bytes[7]; return true; }
  value.bytes[7] = 0;
  if ((value.bytes[6] & 15) != 15) { ++value.bytes[6]; return true; }
  return false;
}
StandaloneUuidV7Issue Failure(E error) noexcept { return {error, {}, {}}; }
} // namespace

StandaloneUuidV7Issuer::StandaloneUuidV7Issuer(StandaloneUuidV7Binding binding, StandaloneUuidV7Policy policy)
    : binding_(binding), policy_(policy), process_(ProcessIdentity()), admission_(Admit(binding, policy)) {}

StandaloneUuidV7Issue StandaloneUuidV7Issuer::Issue(UuidKind kind) noexcept {
  if (process_ != ProcessIdentity()) return Failure(E::wrong_process);
  if (admission_ != E::none) return Failure(admission_);
  if (!IsDurableEngineIdentityKind(kind)) return Failure(E::invalid_kind);
  try {
    std::optional<std::chrono::steady_clock::time_point> wait_started;
    const auto expired = [&] {
      if (!wait_started) return false;
      const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now() - *wait_started).count();
      if (elapsed < 0) throw E::clock_failure;
      return static_cast<u64>(elapsed) >= policy_.same_ms_wait_timeout_ms;
    };
    for (;;) {
      if (expired()) return Failure(E::sequence_exhausted);
      {
        std::unique_lock lock(mutex_, std::defer_lock);
        if (!wait_started) lock.lock();
        else if (!lock.try_lock()) {
          if (expired()) return Failure(E::sequence_exhausted);
          std::this_thread::sleep_for(std::chrono::milliseconds(1));
          continue;
        }
        const auto snapshot = time::ReadLocalNodeClockSnapshot();
        if (!snapshot.ok()) return Failure(E::clock_failure);
        const auto converted = time::WallClockToUuidV7Millis(snapshot.value.wall_clock);
        if (!converted.ok()) return Failure(E::timestamp_out_of_range);
        const auto observed = time::ObserveLocalNodeClock(clock_, snapshot.value, policy_.clock);
        if (!observed.ok() && observed.status.severity != core::platform::Severity::warning) {
          const auto error = observed.decision == time::LocalClockObservationDecision::monotonic_regression_detected ||
                         observed.decision == time::LocalClockObservationDecision::wall_clock_rollback_detected
                           ? E::time_regression : E::clock_failure;
          return {error, {}, {}, observed.decision};
        }
        const u64 millis = converted.unix_epoch_millis;
        if (last_millis_ && millis < *last_millis_ && *last_millis_ - millis > policy_.max_uuid_regression_ms)
          return Failure(E::time_regression);
        Uuid candidate;
        bool available = true;
        if (!last_millis_ || millis > *last_millis_) {
          const auto generated = GenerateCompatibilityUnixTimeV7(millis);
          if (!generated.ok()) return Failure(E::randomness_unavailable);
          candidate = generated.value;
        } else {
          candidate = last_value_;
          available = Increment(candidate);
        }
        if (available) {
          if (expired()) return Failure(E::sequence_exhausted);
          StandaloneUuidV7Issue result{E::none, TypedUuid{kind, candidate}, snapshot.value, observed.decision};
          static_assert(std::is_nothrow_move_constructible_v<StandaloneUuidV7Issue>);
          // No allocating or fallible work follows the publication of state.
          last_value_ = candidate;
          last_millis_ = last_millis_ && *last_millis_ > millis ? *last_millis_ : millis;
          clock_ = observed.state;
          return result;
        }
      }
      if (!wait_started) wait_started = std::chrono::steady_clock::now();
      if (expired()) return Failure(E::sequence_exhausted);
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  } catch (E error) {
    return Failure(error);
  } catch (const std::bad_alloc&) {
    return Failure(E::resource_exhausted);
  } catch (...) {
    return Failure(E::clock_failure);
  }
}
} // namespace scratchbird::core::uuid

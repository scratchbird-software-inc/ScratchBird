// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "time.hpp"

#include <chrono>
#include <ctime>
#include <iomanip>
#include <limits>
#include <sstream>
#include <utility>
#include <vector>

namespace scratchbird::core::time {
namespace {

using scratchbird::core::platform::DiagnosticArgument;
using scratchbird::core::platform::MakeDiagnostic;
using scratchbird::core::platform::Severity;
using scratchbird::core::platform::StatusCode;
using scratchbird::core::platform::Subsystem;

Status TimeOkStatus() {
  return {StatusCode::ok, Severity::info, Subsystem::time};
}

Status TimeErrorStatus() {
  return {StatusCode::time_source_unavailable, Severity::error, Subsystem::time};
}

Status TimeWarningStatus() {
  return {StatusCode::time_source_unavailable, Severity::warning, Subsystem::time};
}

__int128 WallClockToNanoseconds128(WallClockTime value) {
  return static_cast<__int128>(value.unix_seconds) * 1000000000 +
         static_cast<__int128>(value.nanoseconds);
}

DiagnosticRecord MakeClockObservationDiagnostic(Status status,
                                                std::string diagnostic_code,
                                                std::string message_key,
                                                LocalClockObservationDecision decision) {
  return MakeTimeDiagnostic(status,
                            std::move(diagnostic_code),
                            std::move(message_key),
                            LocalClockObservationDecisionName(decision));
}

}  // namespace

const char* TimeAuthorityModeName(TimeAuthorityMode mode) {
  switch (mode) {
    case TimeAuthorityMode::single_node_os: return "single_node_os";
    case TimeAuthorityMode::cluster_majority: return "cluster_majority";
  }
  return "unknown";
}

const char* LocalClockObservationDecisionName(LocalClockObservationDecision decision) {
  switch (decision) {
    case LocalClockObservationDecision::accepted: return "accepted";
    case LocalClockObservationDecision::invalid_wall_clock: return "invalid_wall_clock";
    case LocalClockObservationDecision::monotonic_regression_detected: return "monotonic_regression_detected";
    case LocalClockObservationDecision::wall_clock_rollback_detected: return "wall_clock_rollback_detected";
    case LocalClockObservationDecision::wall_clock_forward_jump_detected: return "wall_clock_forward_jump_detected";
  }
  return "unknown";
}

MonotonicTimeResult ReadMonotonicTime() {
  MonotonicTimeResult result;
  result.status = TimeOkStatus();
  const auto now = std::chrono::steady_clock::now().time_since_epoch();
  result.value.ticks = static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
  return result;
}

WallClockTimeResult ReadWallClockTime() {
  WallClockTimeResult result;
  result.status = TimeOkStatus();
  const auto now = std::chrono::system_clock::now().time_since_epoch();
  const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(now);
  const auto nanos = std::chrono::duration_cast<std::chrono::nanoseconds>(now - seconds);
  result.value.unix_seconds = static_cast<i64>(seconds.count());
  result.value.nanoseconds = static_cast<u32>(nanos.count());
  if (result.value.nanoseconds >= 1000000000u) {
    result.status = TimeErrorStatus();
    result.diagnostic = MakeTimeDiagnostic(result.status,
                                           "SB-TIME-WALL-CLOCK-NANOS-INVALID",
                                           "time.wall_clock.nanoseconds_invalid");
  }
  return result;
}

ClockSnapshotResult ReadLocalNodeClockSnapshot() {
  ClockSnapshotResult result;
  result.status = TimeOkStatus();
  const auto monotonic = ReadMonotonicTime();
  if (!monotonic.ok()) {
    result.status = monotonic.status;
    result.diagnostic = monotonic.diagnostic;
    return result;
  }
  const auto wall = ReadWallClockTime();
  if (!wall.ok()) {
    result.status = wall.status;
    result.diagnostic = wall.diagnostic;
    return result;
  }
  result.value = MakeClockSnapshot(monotonic.value, wall.value);
  return result;
}

UuidV7TimestampResult WallClockToUuidV7Millis(WallClockTime wall_clock) {
  UuidV7TimestampResult result;
  result.status = TimeOkStatus();
  if (wall_clock.unix_seconds < 0 || wall_clock.nanoseconds >= 1000000000u) {
    result.status = TimeErrorStatus();
    result.diagnostic = MakeTimeDiagnostic(result.status,
                                           "SB-TIME-UUIDV7-TIMESTAMP-INVALID",
                                           "time.uuidv7.timestamp_invalid");
    return result;
  }

  result.unix_epoch_millis = static_cast<u64>(wall_clock.unix_seconds) * 1000ull +
                             static_cast<u64>(wall_clock.nanoseconds / 1000000u);
  return result;
}

ClusterTimeResult MakeStandardizedClusterTime(WallClockTime observed_time,
                                               u64 uncertainty_nanoseconds,
                                               ClusterTimePolicy policy) {
  ClusterTimeResult result;
  result.status = TimeOkStatus();
  result.value.observed_time = observed_time;
  result.value.uncertainty_nanoseconds = uncertainty_nanoseconds;
  result.value.authoritative_for_visibility = false;

  if (observed_time.nanoseconds >= 1000000000u) {
    result.status = TimeErrorStatus();
    result.diagnostic = MakeTimeDiagnostic(result.status,
                                           "SB-TIME-CLUSTER-NANOS-INVALID",
                                           "time.cluster.nanoseconds_invalid");
    return result;
  }

  if (policy.max_uncertainty_nanoseconds != 0 && uncertainty_nanoseconds > policy.max_uncertainty_nanoseconds) {
    result.status = {StatusCode::time_source_unavailable, Severity::warning, Subsystem::time};
    result.diagnostic = MakeTimeDiagnostic(result.status,
                                           "SB-TIME-CLUSTER-UNCERTAINTY-EXCEEDED",
                                           "time.cluster.uncertainty_exceeded");
  }

  if (policy.allow_visibility_authority) {
    result.status = {StatusCode::time_source_unavailable, Severity::error, Subsystem::time};
    result.diagnostic = MakeTimeDiagnostic(result.status,
                                           "SB-TIME-CLUSTER-VISIBILITY-AUTHORITY-FORBIDDEN",
                                           "time.cluster.visibility_authority_forbidden");
  }

  return result;
}

ClockSnapshot MakeClockSnapshot(MonotonicTime monotonic, WallClockTime wall_clock) {
  return {monotonic, wall_clock};
}

LocalTimeAuthorityObservation ObserveLocalNodeClock(LocalTimeAuthorityState state,
                                                    ClockSnapshot snapshot,
                                                    LocalTimeAuthorityPolicy policy) {
  LocalTimeAuthorityObservation observation;
  observation.status = TimeOkStatus();
  observation.snapshot = snapshot;
  observation.state = state;
  observation.decision = LocalClockObservationDecision::accepted;

  if (!IsValidWallClockTime(snapshot.wall_clock)) {
    observation.status = TimeErrorStatus();
    observation.decision = LocalClockObservationDecision::invalid_wall_clock;
    observation.diagnostic = MakeClockObservationDiagnostic(observation.status,
                                                           "SB-TIME-LOCAL-WALL-CLOCK-INVALID",
                                                           "time.local.wall_clock.invalid",
                                                           observation.decision);
    return observation;
  }

  if (!state.initialized) {
    observation.state.initialized = true;
    observation.state.last_monotonic = snapshot.monotonic;
    observation.state.last_wall_clock = snapshot.wall_clock;
    observation.state.accepted_observations = 1;
    return observation;
  }

  if (snapshot.monotonic.ticks < state.last_monotonic.ticks) {
    observation.decision = LocalClockObservationDecision::monotonic_regression_detected;
    observation.status = policy.fail_closed_on_monotonic_regression ? TimeErrorStatus() : TimeWarningStatus();
    observation.diagnostic = MakeClockObservationDiagnostic(observation.status,
                                                           "SB-TIME-LOCAL-MONOTONIC-REGRESSION",
                                                           "time.local.monotonic_regression",
                                                           observation.decision);
    if (policy.fail_closed_on_monotonic_regression) { return observation; }
  }

  const int wall_compare = CompareWallClock(snapshot.wall_clock, state.last_wall_clock);
  if (wall_compare < 0) {
    const u64 rollback = WallClockDeltaNanos(snapshot.wall_clock, state.last_wall_clock);
    if (policy.max_wall_clock_backward_nanoseconds == 0 || rollback > policy.max_wall_clock_backward_nanoseconds) {
      observation.decision = LocalClockObservationDecision::wall_clock_rollback_detected;
      observation.status = policy.fail_closed_on_wall_clock_rollback ? TimeErrorStatus() : TimeWarningStatus();
      observation.diagnostic = MakeClockObservationDiagnostic(observation.status,
                                                             "SB-TIME-LOCAL-WALL-CLOCK-ROLLBACK",
                                                             "time.local.wall_clock_rollback",
                                                             observation.decision);
      if (policy.fail_closed_on_wall_clock_rollback) { return observation; }
    }
  } else if (wall_compare > 0 && policy.max_wall_clock_forward_jump_nanoseconds != 0) {
    const u64 forward_jump = WallClockDeltaNanos(state.last_wall_clock, snapshot.wall_clock);
    const u64 monotonic_elapsed = SaturatingDurationNanos(state.last_monotonic, snapshot.monotonic);
    if (forward_jump > monotonic_elapsed + policy.max_wall_clock_forward_jump_nanoseconds) {
      observation.decision = LocalClockObservationDecision::wall_clock_forward_jump_detected;
      observation.status = policy.fail_closed_on_wall_clock_forward_jump ? TimeErrorStatus() : TimeWarningStatus();
      observation.diagnostic = MakeClockObservationDiagnostic(observation.status,
                                                             "SB-TIME-LOCAL-WALL-CLOCK-FORWARD-JUMP",
                                                             "time.local.wall_clock_forward_jump",
                                                             observation.decision);
      if (policy.fail_closed_on_wall_clock_forward_jump) { return observation; }
    }
  }

  observation.state.initialized = true;
  observation.state.last_monotonic = snapshot.monotonic;
  observation.state.last_wall_clock = snapshot.wall_clock;
  observation.state.accepted_observations = state.accepted_observations + 1;
  return observation;
}

bool IsValidWallClockTime(WallClockTime wall_clock) {
  return wall_clock.nanoseconds < 1000000000u;
}

int CompareWallClock(WallClockTime left, WallClockTime right) {
  if (left.unix_seconds < right.unix_seconds) return -1;
  if (left.unix_seconds > right.unix_seconds) return 1;
  if (left.nanoseconds < right.nanoseconds) return -1;
  if (left.nanoseconds > right.nanoseconds) return 1;
  return 0;
}

u64 WallClockDeltaNanos(WallClockTime earlier, WallClockTime later) {
  const __int128 delta = WallClockToNanoseconds128(later) - WallClockToNanoseconds128(earlier);
  if (delta <= 0) { return 0; }
  if (delta > static_cast<__int128>(std::numeric_limits<u64>::max())) {
    return std::numeric_limits<u64>::max();
  }
  return static_cast<u64>(delta);
}

u64 SaturatingDurationNanos(MonotonicTime earlier, MonotonicTime later) {
  if (later.ticks < earlier.ticks) {
    return 0;
  }
  return later.ticks - earlier.ticks;
}

std::string WallClockToIso8601Utc(WallClockTime wall_clock) {
  std::time_t seconds = static_cast<std::time_t>(wall_clock.unix_seconds);
  std::tm utc{};
#if defined(_WIN32)
  gmtime_s(&utc, &seconds);
#else
  gmtime_r(&seconds, &utc);
#endif
  std::ostringstream out;
  out << std::put_time(&utc, "%Y-%m-%dT%H:%M:%S") << ".";
  out << std::setw(9) << std::setfill('0') << wall_clock.nanoseconds << "Z";
  return out.str();
}

DiagnosticRecord MakeTimeDiagnostic(Status status,
                                    std::string diagnostic_code,
                                    std::string message_key,
                                    std::string detail) {
  std::vector<DiagnosticArgument> arguments;
  if (!detail.empty()) {
    arguments.push_back({"detail", detail});
  }
  return MakeDiagnostic(status.code,
                        status.severity,
                        status.subsystem,
                        std::move(diagnostic_code),
                        std::move(message_key),
                        std::move(arguments),
                        {},
                        "core.time");
}

}  // namespace scratchbird::core::time

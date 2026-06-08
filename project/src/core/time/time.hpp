// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "runtime_platform.hpp"

#include <string>

namespace scratchbird::core::time {

using scratchbird::core::platform::DiagnosticRecord;
using scratchbird::core::platform::MonotonicTime;
using scratchbird::core::platform::StandardizedClusterTime;
using scratchbird::core::platform::Status;
using scratchbird::core::platform::WallClockTime;
using scratchbird::core::platform::u8;
using scratchbird::core::platform::i64;
using scratchbird::core::platform::u32;
using scratchbird::core::platform::u64;

struct TimeResult {
  Status status;
  DiagnosticRecord diagnostic;

  bool ok() const {
    return status.ok();
  }
};

struct MonotonicTimeResult {
  Status status;
  MonotonicTime value;
  DiagnosticRecord diagnostic;

  bool ok() const {
    return status.ok();
  }
};

struct WallClockTimeResult {
  Status status;
  WallClockTime value;
  DiagnosticRecord diagnostic;

  bool ok() const {
    return status.ok();
  }
};

struct ClusterTimeResult {
  Status status;
  StandardizedClusterTime value;
  DiagnosticRecord diagnostic;

  bool ok() const {
    return status.ok();
  }
};

struct UuidV7TimestampResult {
  Status status;
  u64 unix_epoch_millis = 0;
  DiagnosticRecord diagnostic;

  bool ok() const {
    return status.ok();
  }
};

struct ClockSnapshot {
  MonotonicTime monotonic;
  WallClockTime wall_clock;
};

struct ClockSnapshotResult {
  Status status;
  ClockSnapshot value;
  DiagnosticRecord diagnostic;

  bool ok() const {
    return status.ok();
  }
};

enum class TimeAuthorityMode : u8 {
  single_node_os,
  cluster_majority
};

enum class LocalClockObservationDecision : u8 {
  accepted,
  invalid_wall_clock,
  monotonic_regression_detected,
  wall_clock_rollback_detected,
  wall_clock_forward_jump_detected
};

struct LocalTimeAuthorityPolicy {
  u64 max_wall_clock_backward_nanoseconds = 0;
  u64 max_wall_clock_forward_jump_nanoseconds = 0;
  bool fail_closed_on_wall_clock_rollback = true;
  bool fail_closed_on_monotonic_regression = true;
  bool fail_closed_on_wall_clock_forward_jump = false;
};

struct LocalTimeAuthorityState {
  bool initialized = false;
  MonotonicTime last_monotonic;
  WallClockTime last_wall_clock;
  u64 accepted_observations = 0;
};

struct LocalTimeAuthorityObservation {
  Status status;
  ClockSnapshot snapshot;
  LocalTimeAuthorityState state;
  LocalClockObservationDecision decision = LocalClockObservationDecision::accepted;
  DiagnosticRecord diagnostic;

  bool ok() const {
    return status.ok();
  }
};

struct ClusterTimePolicy {
  u64 max_uncertainty_nanoseconds = 0;
  bool allow_visibility_authority = false;
};

const char* TimeAuthorityModeName(TimeAuthorityMode mode);
const char* LocalClockObservationDecisionName(LocalClockObservationDecision decision);
MonotonicTimeResult ReadMonotonicTime();
WallClockTimeResult ReadWallClockTime();
ClockSnapshotResult ReadLocalNodeClockSnapshot();
UuidV7TimestampResult WallClockToUuidV7Millis(WallClockTime wall_clock);
ClusterTimeResult MakeStandardizedClusterTime(WallClockTime observed_time,
                                               u64 uncertainty_nanoseconds,
                                               ClusterTimePolicy policy);
ClockSnapshot MakeClockSnapshot(MonotonicTime monotonic, WallClockTime wall_clock);
LocalTimeAuthorityObservation ObserveLocalNodeClock(LocalTimeAuthorityState state,
                                                    ClockSnapshot snapshot,
                                                    LocalTimeAuthorityPolicy policy);
bool IsValidWallClockTime(WallClockTime wall_clock);
int CompareWallClock(WallClockTime left, WallClockTime right);
u64 WallClockDeltaNanos(WallClockTime earlier, WallClockTime later);
u64 SaturatingDurationNanos(MonotonicTime earlier, MonotonicTime later);
std::string WallClockToIso8601Utc(WallClockTime wall_clock);
DiagnosticRecord MakeTimeDiagnostic(Status status,
                                    std::string diagnostic_code,
                                    std::string message_key,
                                    std::string detail = {});

}  // namespace scratchbird::core::time

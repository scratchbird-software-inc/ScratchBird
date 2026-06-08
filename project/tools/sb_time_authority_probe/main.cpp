// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// SEARCH_KEY: SB_TIME_AUTHORITY_PROBE_MAIN

#include "time.hpp"

#include <iostream>

namespace {

using namespace scratchbird::core::time;
using scratchbird::core::platform::MonotonicTime;
using scratchbird::core::platform::WallClockTime;

const char* Bool(bool value) { return value ? "true" : "false"; }

ClockSnapshot Snapshot(std::uint64_t monotonic_nanos, std::int64_t unix_seconds, std::uint32_t nanos) {
  return {MonotonicTime{monotonic_nanos}, WallClockTime{unix_seconds, nanos}};
}

}  // namespace

int main() {
  LocalTimeAuthorityPolicy policy;
  policy.max_wall_clock_backward_nanoseconds = 1000;
  policy.max_wall_clock_forward_jump_nanoseconds = 1000;
  policy.fail_closed_on_wall_clock_forward_jump = true;

  LocalTimeAuthorityState state;
  const auto first = ObserveLocalNodeClock(state, Snapshot(1000, 100, 0), policy);
  state = first.state;
  const auto second = ObserveLocalNodeClock(state, Snapshot(2000, 100, 500), policy);
  const auto rollback = ObserveLocalNodeClock(second.state, Snapshot(3000, 99, 0), policy);
  const auto monotonic_regression = ObserveLocalNodeClock(second.state, Snapshot(1000, 101, 0), policy);
  const auto forward_jump = ObserveLocalNodeClock(second.state, Snapshot(3000, 200, 0), policy);
  const auto invalid_wall = ObserveLocalNodeClock(second.state, Snapshot(4000, 200, 1000000000u), policy);
  const auto live_snapshot = ReadLocalNodeClockSnapshot();

  const bool accepted = first.ok() && second.ok() && first.decision == LocalClockObservationDecision::accepted &&
                        second.decision == LocalClockObservationDecision::accepted && second.state.accepted_observations == 2;
  const bool rollback_detected = !rollback.ok() && rollback.decision == LocalClockObservationDecision::wall_clock_rollback_detected;
  const bool monotonic_detected = !monotonic_regression.ok() &&
                                  monotonic_regression.decision == LocalClockObservationDecision::monotonic_regression_detected;
  const bool forward_detected = !forward_jump.ok() &&
                                forward_jump.decision == LocalClockObservationDecision::wall_clock_forward_jump_detected;
  const bool invalid_detected = !invalid_wall.ok() && invalid_wall.decision == LocalClockObservationDecision::invalid_wall_clock;
  const bool duration_ok = SaturatingDurationNanos(MonotonicTime{10}, MonotonicTime{15}) == 5 &&
                           SaturatingDurationNanos(MonotonicTime{15}, MonotonicTime{10}) == 0;
  const bool wall_delta_ok = WallClockDeltaNanos(WallClockTime{1, 0}, WallClockTime{1, 100}) == 100 &&
                             WallClockDeltaNanos(WallClockTime{2, 0}, WallClockTime{1, 0}) == 0;
  const bool live_ok = live_snapshot.ok() && IsValidWallClockTime(live_snapshot.value.wall_clock);
  const bool names_ok = TimeAuthorityModeName(TimeAuthorityMode::single_node_os) == std::string("single_node_os") &&
                        LocalClockObservationDecisionName(LocalClockObservationDecision::accepted) == std::string("accepted");
  const bool ok = accepted && rollback_detected && monotonic_detected && forward_detected && invalid_detected &&
                  duration_ok && wall_delta_ok && live_ok && names_ok;

  std::cout << "{\n";
  std::cout << "  \"ok\": " << Bool(ok) << ",\n";
  std::cout << "  \"accepted\": " << Bool(accepted) << ",\n";
  std::cout << "  \"rollback_detected\": " << Bool(rollback_detected) << ",\n";
  std::cout << "  \"monotonic_regression_detected\": " << Bool(monotonic_detected) << ",\n";
  std::cout << "  \"forward_jump_detected\": " << Bool(forward_detected) << ",\n";
  std::cout << "  \"invalid_wall_detected\": " << Bool(invalid_detected) << ",\n";
  std::cout << "  \"duration_ok\": " << Bool(duration_ok) << ",\n";
  std::cout << "  \"wall_delta_ok\": " << Bool(wall_delta_ok) << ",\n";
  std::cout << "  \"live_snapshot_ok\": " << Bool(live_ok) << "\n";
  std::cout << "}\n";
  return ok ? 0 : 1;
}

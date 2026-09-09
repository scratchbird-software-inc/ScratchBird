// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "canonical_query_model_family_composition_support.hpp"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>

namespace sblr = scratchbird::engine::sblr;
namespace opt = scratchbird::engine::optimizer;

int main() {
  int checks = 0;
  int failures = 0;
  const auto check = [&](bool condition, const char* name) {
    ++checks;
    if (!condition) {
      ++failures;
      std::cerr << "FAIL: " << name << '\n';
    }
  };
  using State = sblr::LiveCancellationProbeState;
  int calls = 0;
  const std::function<bool()> cancel = [&] { ++calls; return true; };
  check(sblr::PollLiveCancellationProbe(cancel, nullptr) && calls == 0,
        "null state refuses without polling");
  for (const auto terminal : {State::kCancelled, State::kProbeFailed}) {
    auto state = terminal;
    check(sblr::PollLiveCancellationProbe(cancel, &state) &&
              state == terminal && calls == 0,
          "terminal state remains terminal without polling");
  }
  auto state = State::kRunning;
  check(!sblr::PollLiveCancellationProbe({}, &state) && state == State::kRunning,
        "absent callback continues");
  const std::function<bool()> keep_running = [&] { ++calls; return false; };
  check(!sblr::PollLiveCancellationProbe(keep_running, &state) &&
            state == State::kRunning && calls == 1,
        "false callback continues");
  check(sblr::PollLiveCancellationProbe(cancel, &state) &&
            state == State::kCancelled && calls == 2,
        "true callback cancels");
  check(sblr::PollLiveCancellationProbe(cancel, &state) && calls == 2,
        "cancelled callback is not repeated");
  state = State::kRunning;
  const std::function<bool()> throws = [&]() -> bool {
    ++calls;
    throw std::runtime_error("probe failure");
  };
  check(sblr::PollLiveCancellationProbe(throws, &state) &&
            state == State::kProbeFailed && calls == 3,
        "callback exception fails closed");
  check(sblr::PollLiveCancellationProbe(throws, &state) && calls == 3,
        "failed callback is not repeated");

#if !defined(SCRATCHBIRD_QOW_QUERY_ROUTE_CONTRACT_ONLY)
  // Optimizer capability receipts, not fabricated MGA statement snapshots.
  for (const std::string family : {"vector", "search", "spatial", "document",
                                   "key_value", "time_series"}) {
    for (const std::string operation : {"SCAN", "FILTER"}) {
      for (const std::uint64_t units : {0ULL, 7ULL}) {
        for (const auto route : {opt::ModelFamilyAlternativeRouteClassV1::kNative,
                                opt::ModelFamilyAlternativeRouteClassV1::kExactCollectionFallback}) {
          opt::ModelFamilyCoordinatorRequestV1 planning;
          planning.family_id = family;
          planning.operation_id = operation;
          planning.statistics_snapshot_uuid = "statistics-test-identity";
          planning.statistics_generation = 13;
          const auto snapshot = sblr::MakeModelFamilyCapabilitySnapshotForCompositionV1(
              planning, "runtime-services-test", route, "provider", "capability",
              11, units != 0, units, 3, units);
          const auto& metrics = snapshot.metrics;
          const auto bounded = std::max<std::uint64_t>(1, units);
          check(snapshot.route_class == route && snapshot.provider_uuid == "provider" &&
                    snapshot.capability_uuid == "capability" &&
                    snapshot.provider_generation == 11 && snapshot.available == (units != 0),
                "capability identity and availability preserved");
          check(metrics.statistics_snapshot_uuid == planning.statistics_snapshot_uuid &&
                    metrics.statistics_generation == 13 && metrics.confidence_basis_points == 9000 &&
                    metrics.startup_events == 1 && metrics.sequential_pages == 3,
                "statistics identity and fixed cost fields preserved");
          check(metrics.estimated_rows == bounded && metrics.working_set_bytes == bounded &&
                    metrics.memory_grant_units == bounded && metrics.mga_rechecks == bounded,
                "zero estimates retain the existing minimum of one");
          check(metrics.predicate_evaluations == (operation == "FILTER" ? units : 0) &&
                    metrics.vector_distance_evaluations == (family == "vector" ? bounded : 0) &&
                    metrics.text_score_evaluations == (family == "search" ? bounded : 0) &&
                    metrics.spatial_evaluations == (family == "spatial" ? bounded : 0),
                "family and operation cost counters stay isolated");
          const auto repeated = sblr::MakeModelFamilyCapabilitySnapshotForCompositionV1(
              planning, "runtime-services-test", route, "provider", "capability",
              11, units != 0, units, 3, units);
          check(!metrics.property_snapshot_uuid.empty() &&
                    !metrics.calibration_profile_uuid.empty() &&
                    metrics.property_snapshot_uuid == repeated.metrics.property_snapshot_uuid &&
                    metrics.calibration_profile_uuid == repeated.metrics.calibration_profile_uuid,
                "derived optimizer receipt identities are deterministic");
        }
      }
    }
  }
#endif
  std::cout << "runtime services: " << checks << " checks, " << failures << " failures\n";
  return failures == 0 ? 0 : 1;
}

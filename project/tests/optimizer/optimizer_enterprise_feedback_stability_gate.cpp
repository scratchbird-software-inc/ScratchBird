// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "optimizer_feedback_stability.hpp"

#include <cstdlib>
#include <iostream>
#include <string>

namespace opt = scratchbird::engine::optimizer;

namespace {

bool Require(bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << "OEIC feedback stability gate failure: " << message << '\n';
    return false;
  }
  return true;
}

opt::OptimizerFeedbackPlanChoiceObservation Observation(std::string plan_hash,
                                                        std::uint64_t generation) {
  // SEARCH_KEY: OEIC_FEEDBACK_STABILITY_REGRESSION
  opt::OptimizerFeedbackPlanChoiceObservation observation;
  observation.route_label = "embedded:select:feedback";
  observation.plan_hash = std::move(plan_hash);
  observation.result_hash = "result:stable";
  observation.cache_key = "cache:key:" + std::to_string(generation);
  observation.cache_diagnostic_code = "SB_OPTIMIZER_PLAN_CACHE_HIT";
  observation.feedback_generation = generation;
  observation.memory_feedback_generation = 100 + generation;
  observation.stats_epoch = 200 + generation;
  observation.catalog_epoch = 300 + generation;
  observation.runtime_consumed = true;
  observation.exact_fallback_available = true;
  observation.benchmark_clean_claim = true;
  return observation;
}

bool StableFeedbackIsAccepted() {
  opt::OptimizerFeedbackStabilityRequest request;
  request.observations = {
      Observation("plan:a", 1),
      Observation("plan:a", 2),
      Observation("plan:b", 3),
  };
  request.latest_feedback_generation = 3;
  request.latest_memory_feedback_generation = 103;
  request.latest_stats_epoch = 203;
  request.latest_catalog_epoch = 303;
  request.max_plan_switches = 2;
  request.require_benchmark_clean = true;
  const auto result = opt::EvaluateOptimizerFeedbackStability(request);
  return Require(result.accepted && result.stable,
                 "stable feedback was refused: " + result.diagnostic_code) &&
         Require(result.plan_switches == 1, "plan switch count mismatch") &&
         Require(result.benchmark_clean_permitted,
                 "benchmark-clean feedback was not permitted");
}

bool OscillationAndStaleReuseAreRejected() {
  opt::OptimizerFeedbackStabilityRequest oscillating;
  oscillating.observations = {
      Observation("plan:a", 1),
      Observation("plan:b", 2),
      Observation("plan:a", 3),
      Observation("plan:b", 4),
  };
  oscillating.max_plan_switches = 2;
  auto oscillation = opt::EvaluateOptimizerFeedbackStability(oscillating);

  opt::OptimizerFeedbackStabilityRequest stale;
  stale.observations = {Observation("plan:a", 1)};
  stale.observations.front().stale_plan_reused = true;
  auto stale_result = opt::EvaluateOptimizerFeedbackStability(stale);

  return Require(!oscillation.accepted &&
                     oscillation.diagnostic_code ==
                         "SB_OPT_FEEDBACK_STABILITY_OSCILLATION",
                 "feedback oscillation was accepted") &&
         Require(!stale_result.accepted &&
                     stale_result.diagnostic_code ==
                         "SB_OPT_FEEDBACK_STABILITY_STALE_PLAN_REUSED",
                 "stale plan reuse was accepted");
}

bool UnsafeAndBenchmarkOverclaimAreRejected() {
  opt::OptimizerFeedbackStabilityRequest unsafe;
  unsafe.observations = {Observation("plan:a", 1)};
  unsafe.observations.front().parser_or_reference_authority = true;
  const auto unsafe_result = opt::EvaluateOptimizerFeedbackStability(unsafe);

  opt::OptimizerFeedbackStabilityRequest overclaim;
  overclaim.observations = {Observation("plan:a", 1)};
  overclaim.observations.front().benchmark_clean_claim = false;
  overclaim.require_benchmark_clean = true;
  const auto overclaim_result = opt::EvaluateOptimizerFeedbackStability(overclaim);

  opt::OptimizerFeedbackStabilityRequest stale_generation;
  stale_generation.observations = {Observation("plan:a", 1)};
  stale_generation.latest_feedback_generation = 2;
  const auto stale_generation_result =
      opt::EvaluateOptimizerFeedbackStability(stale_generation);

  return Require(!unsafe_result.accepted &&
                     unsafe_result.diagnostic_code ==
                         "SB_OPT_FEEDBACK_STABILITY_UNSAFE_AUTHORITY",
                 "unsafe feedback authority was accepted") &&
         Require(!overclaim_result.accepted &&
                     overclaim_result.diagnostic_code ==
                         "SB_OPT_FEEDBACK_STABILITY_BENCHMARK_CLEAN_OVERCLAIM",
                 "benchmark-clean feedback overclaim was accepted") &&
         Require(!stale_generation_result.accepted &&
                     stale_generation_result.diagnostic_code ==
                         "SB_OPT_FEEDBACK_STABILITY_STALE_FEEDBACK_GENERATION",
                 "stale feedback generation was accepted");
}

}  // namespace

int main() {
  if (!StableFeedbackIsAccepted()) return EXIT_FAILURE;
  if (!OscillationAndStaleReuseAreRejected()) return EXIT_FAILURE;
  if (!UnsafeAndBenchmarkOverclaimAreRejected()) return EXIT_FAILURE;
  return EXIT_SUCCESS;
}

// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "optimizer_feedback_stability.hpp"

#include <algorithm>
#include <utility>

namespace scratchbird::engine::optimizer {
namespace {

OptimizerFeedbackStabilityResult Refuse(std::string code, std::string evidence) {
  OptimizerFeedbackStabilityResult result;
  result.accepted = false;
  result.stable = false;
  result.benchmark_clean_permitted = false;
  result.diagnostic_code = std::move(code);
  result.evidence.push_back(std::move(evidence));
  result.evidence.push_back("feedback_stability.accepted=false");
  result.evidence.push_back("feedback_stability.benchmark_clean=false");
  return result;
}

bool UnsafeAuthority(const OptimizerFeedbackPlanChoiceObservation& observation) {
  return observation.parser_or_donor_authority ||
         observation.metric_finality_or_visibility_authority ||
         observation.cluster_stub_evidence;
}

bool BenchmarkCleanObservation(const OptimizerFeedbackPlanChoiceObservation& observation) {
  return observation.benchmark_clean_claim &&
         observation.runtime_consumed &&
         observation.exact_fallback_available &&
         !observation.unsafe_cache_admission &&
         !observation.stale_plan_reused &&
         !UnsafeAuthority(observation);
}

}  // namespace

OptimizerFeedbackStabilityResult EvaluateOptimizerFeedbackStability(
    const OptimizerFeedbackStabilityRequest& request) {
  if (request.observations.empty()) {
    return Refuse("SB_OPT_FEEDBACK_STABILITY_OBSERVATION_REQUIRED",
                  "plan_choice_observation_required");
  }
  std::uint64_t previous_generation = 0;
  std::uint64_t previous_memory_generation = 0;
  std::string previous_plan_hash;
  std::string expected_result_hash;
  std::uint64_t switches = 0;

  for (const auto& observation : request.observations) {
    if (observation.route_label.empty() || observation.plan_hash.empty() ||
        observation.result_hash.empty() || observation.cache_key.empty()) {
      return Refuse("SB_OPT_FEEDBACK_STABILITY_SCOPE_REQUIRED",
                    "route_plan_result_and_cache_key_required");
    }
    if (UnsafeAuthority(observation)) {
      return Refuse("SB_OPT_FEEDBACK_STABILITY_UNSAFE_AUTHORITY",
                    "parser_donor_metric_finality_or_cluster_stub_refused");
    }
    if (!observation.runtime_consumed ||
        !observation.exact_fallback_available) {
      return Refuse("SB_OPT_FEEDBACK_STABILITY_RUNTIME_PROOF_REQUIRED",
                    "runtime_consumption_and_exact_fallback_required");
    }
    if (observation.unsafe_cache_admission) {
      return Refuse("SB_OPT_FEEDBACK_STABILITY_UNSAFE_CACHE_ADMISSION",
                    "unsafe_cache_admission_refused");
    }
    if (observation.stale_plan_reused) {
      return Refuse("SB_OPT_FEEDBACK_STABILITY_STALE_PLAN_REUSED",
                    "stale_plan_reuse_refused");
    }
    if (previous_generation != 0 &&
        observation.feedback_generation <= previous_generation) {
      return Refuse("SB_OPT_FEEDBACK_STABILITY_NON_MONOTONIC_GENERATION",
                    "feedback_generation_must_increase");
    }
    if (previous_memory_generation != 0 &&
        observation.memory_feedback_generation < previous_memory_generation) {
      return Refuse("SB_OPT_FEEDBACK_STABILITY_MEMORY_GENERATION_REGRESSED",
                    "memory_feedback_generation_must_not_regress");
    }
    if (!expected_result_hash.empty() &&
        observation.result_hash != expected_result_hash) {
      return Refuse("SB_OPT_FEEDBACK_STABILITY_RESULT_MISMATCH",
                    "feedback_plan_choice_changed_results");
    }
    if (!previous_plan_hash.empty() &&
        observation.plan_hash != previous_plan_hash) {
      ++switches;
    }
    previous_plan_hash = observation.plan_hash;
    expected_result_hash = observation.result_hash;
    previous_generation = observation.feedback_generation;
    previous_memory_generation = observation.memory_feedback_generation;
  }

  const auto& last = request.observations.back();
  if (request.latest_feedback_generation != 0 &&
      last.feedback_generation < request.latest_feedback_generation) {
    return Refuse("SB_OPT_FEEDBACK_STABILITY_STALE_FEEDBACK_GENERATION",
                  "latest_feedback_generation_not_observed");
  }
  if (request.latest_memory_feedback_generation != 0 &&
      last.memory_feedback_generation < request.latest_memory_feedback_generation) {
    return Refuse("SB_OPT_FEEDBACK_STABILITY_STALE_MEMORY_GENERATION",
                  "latest_memory_feedback_generation_not_observed");
  }
  if (request.latest_stats_epoch != 0 && last.stats_epoch < request.latest_stats_epoch) {
    return Refuse("SB_OPT_FEEDBACK_STABILITY_STALE_STATS_EPOCH",
                  "latest_stats_epoch_not_observed");
  }
  if (request.latest_catalog_epoch != 0 &&
      last.catalog_epoch < request.latest_catalog_epoch) {
    return Refuse("SB_OPT_FEEDBACK_STABILITY_STALE_CATALOG_EPOCH",
                  "latest_catalog_epoch_not_observed");
  }
  if (switches > request.max_plan_switches) {
    OptimizerFeedbackStabilityResult result =
        Refuse("SB_OPT_FEEDBACK_STABILITY_OSCILLATION",
               "feedback_plan_choice_oscillation_detected");
    result.plan_switches = switches;
    return result;
  }

  const bool benchmark_clean =
      std::all_of(request.observations.begin(),
                  request.observations.end(),
                  BenchmarkCleanObservation);
  if (request.require_benchmark_clean && !benchmark_clean) {
    return Refuse("SB_OPT_FEEDBACK_STABILITY_BENCHMARK_CLEAN_OVERCLAIM",
                  "benchmark_clean_requires_runtime_exact_fallback_safe_cache");
  }

  OptimizerFeedbackStabilityResult result;
  result.accepted = true;
  result.stable = true;
  result.benchmark_clean_permitted = benchmark_clean;
  result.diagnostic_code = "SB_OPT_FEEDBACK_STABILITY_OK";
  result.plan_switches = switches;
  result.evidence.push_back("feedback_stability.accepted=true");
  result.evidence.push_back("feedback_stability.stable=true");
  result.evidence.push_back("feedback_stability.plan_switches=" +
                            std::to_string(switches));
  result.evidence.push_back("feedback_stability.benchmark_clean=" +
                            std::string(benchmark_clean ? "true" : "false"));
  result.evidence.push_back("feedback_stability.parser_or_donor_authority=false");
  result.evidence.push_back("feedback_stability.metric_finality_or_visibility_authority=false");
  result.evidence.push_back("feedback_stability.cluster_stub_evidence=false");
  return result;
}

}  // namespace scratchbird::engine::optimizer

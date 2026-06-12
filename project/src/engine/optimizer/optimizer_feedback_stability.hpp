// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace scratchbird::engine::optimizer {

// SEARCH_KEY: OEIC_FEEDBACK_STABILITY_REGRESSION
// Feedback stability guards are optimizer claim controls. They decide whether
// feedback-driven plan changes may be admitted, reused, or called
// benchmark-clean. They are not transaction finality, row visibility, parser,
// reference, security, or recovery authority.
struct OptimizerFeedbackPlanChoiceObservation {
  std::string route_label;
  std::string plan_hash;
  std::string result_hash;
  std::string cache_key;
  std::string cache_diagnostic_code;
  std::uint64_t feedback_generation = 0;
  std::uint64_t memory_feedback_generation = 0;
  std::uint64_t stats_epoch = 0;
  std::uint64_t catalog_epoch = 0;
  bool runtime_consumed = false;
  bool exact_fallback_available = false;
  bool benchmark_clean_claim = false;
  bool unsafe_cache_admission = false;
  bool stale_plan_reused = false;
  bool parser_or_reference_authority = false;
  bool metric_finality_or_visibility_authority = false;
  bool cluster_stub_evidence = false;
};

struct OptimizerFeedbackStabilityRequest {
  std::vector<OptimizerFeedbackPlanChoiceObservation> observations;
  std::uint64_t latest_feedback_generation = 0;
  std::uint64_t latest_memory_feedback_generation = 0;
  std::uint64_t latest_stats_epoch = 0;
  std::uint64_t latest_catalog_epoch = 0;
  std::uint64_t max_plan_switches = 2;
  bool require_benchmark_clean = false;
};

struct OptimizerFeedbackStabilityResult {
  bool accepted = false;
  bool stable = false;
  bool benchmark_clean_permitted = false;
  std::string diagnostic_code;
  std::vector<std::string> evidence;
  std::uint64_t plan_switches = 0;
};

OptimizerFeedbackStabilityResult EvaluateOptimizerFeedbackStability(
    const OptimizerFeedbackStabilityRequest& request);

}  // namespace scratchbird::engine::optimizer

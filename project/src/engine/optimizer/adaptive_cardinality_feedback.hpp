// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "optimizer_cost_full.hpp"
#include "optimizer_feedback.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace scratchbird::engine::optimizer {

// SEARCH_KEY: ORH_ADAPTIVE_CARDINALITY_PLAN_FEEDBACK
// Adaptive row feedback is optimizer-advisory only. It may admit
// bind-sensitive variants, quarantine a bad estimate, or request extended
// statistics after runtime proof; it is never parser, visibility, finality,
// recovery, authorization, or reference authority.
struct AdaptiveFeedbackAuthorityContext {
  bool engine_mga_snapshot_bound = false;
  bool transaction_inventory_authoritative = false;
  bool security_recheck_required = false;
  bool exact_recheck_required = false;
  bool parser_client_or_reference_feedback_authority = false;
  bool feedback_visibility_or_finality_authority = false;
  bool feedback_recovery_authority = false;
};

struct AdaptiveFeedbackEpochs {
  std::uint64_t feedback_generation = 0;
  std::uint64_t expected_feedback_generation = 0;
  std::uint64_t feedback_epoch = 0;
  std::uint64_t catalog_epoch = 0;
  std::uint64_t expected_catalog_epoch = 0;
  std::uint64_t security_epoch = 0;
  std::uint64_t expected_security_epoch = 0;
};

struct AdaptiveFeedbackPlanCapture {
  std::string route_label;
  std::string baseline_plan_hash;
  std::string variant_plan_hash;
  std::string fallback_plan_hash;
  std::string result_hash;
  std::string fallback_result_hash;
  bool runtime_consumed = false;
  bool exact_fallback_available = false;
  bool benchmark_or_reference_dominance_claim = false;
};

struct AdaptiveCardinalityFeedbackRequest {
  OptimizerRuntimeFeedback feedback;
  CostVector baseline_cost;
  AdaptiveFeedbackAuthorityContext authority;
  AdaptiveFeedbackEpochs epochs;
  AdaptiveFeedbackPlanCapture plan;
  bool bind_sensitive_variant_requested = false;
  bool misestimate_quarantine_requested = false;
  bool extended_stat_request_requested = false;
  bool extended_stat_source_authoritative = true;
};

struct AdaptiveCardinalityFeedbackResult {
  bool ok = false;
  bool benchmark_clean = false;
  bool fallback_used = false;
  bool fail_closed = false;
  bool bind_sensitive_variant_created = false;
  bool misestimate_quarantined = false;
  bool extended_stat_requested = false;
  std::string diagnostic_code;
  std::string fallback_reason;
  OptimizerFeedbackStatus feedback_status;
  CostVector adjusted_cost;
  std::vector<std::string> evidence;
};

AdaptiveCardinalityFeedbackResult EvaluateAdaptiveCardinalityFeedback(
    const AdaptiveCardinalityFeedbackRequest& request);

}  // namespace scratchbird::engine::optimizer

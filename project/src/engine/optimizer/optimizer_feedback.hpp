// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace scratchbird::engine::optimizer {

// SEARCH_KEY: SB_OPTIMIZER_METRIC_FEEDBACK_CONTRACTS
// Runtime feedback is optimizer-advisory only. It may calibrate relative cost
// and memory-grant estimates, but it is never transaction finality, visibility,
// parser execution, or donor authority.
struct OptimizerRuntimeFeedback {
  std::string operator_family;
  std::string plan_shape;
  std::string cost_profile_id = "local_default_v1";
  std::uint64_t estimated_rows = 0;
  std::uint64_t actual_rows = 0;
  std::uint64_t actual_rows_examined = 0;
  std::uint64_t actual_rows_filtered = 0;
  std::uint64_t loop_count = 0;
  std::uint64_t estimated_pages = 0;
  std::uint64_t actual_pages = 0;
  std::uint64_t estimated_io_operations = 0;
  std::uint64_t actual_io_operations = 0;
  std::uint64_t estimated_visibility_recheck_rows = 0;
  std::uint64_t actual_visibility_recheck_rows = 0;
  std::uint64_t estimated_spill_bytes = 0;
  std::uint64_t actual_spill_bytes = 0;
  std::uint64_t memory_grant_bytes = 0;
  std::uint64_t peak_memory_bytes = 0;
  std::uint64_t estimated_latency_microseconds = 0;
  std::uint64_t actual_latency_microseconds = 0;
  std::uint64_t estimated_resource_units = 0;
  std::uint64_t actual_resource_units = 0;
  std::uint64_t freshness_microseconds = 0;
  std::uint64_t max_freshness_microseconds = 60000000;
  bool policy_allowed = true;
  bool advisory_only = true;
  bool mga_visibility_recheck_preserved = true;
  bool parser_or_donor_authority = false;
  std::string transaction_finality_authority = "engine_transaction_inventory";
};

struct OptimizerCalibratedCostProfile {
  bool apply = false;
  double row_cost_multiplier = 1.0;
  double page_cost_multiplier = 1.0;
  double io_cost_multiplier = 1.0;
  double visibility_cost_multiplier = 1.0;
  double memory_cost_multiplier = 1.0;
  double latency_cost_multiplier = 1.0;
  std::uint64_t spill_penalty_pages = 0;
  std::uint64_t uncertainty_penalty = 0;
  std::string profile_id = "feedback_disabled";
};

struct OptimizerMemoryGrantFeedback {
  bool apply = false;
  std::uint64_t observed_grant_bytes = 0;
  std::uint64_t observed_peak_bytes = 0;
  std::uint64_t recommended_grant_bytes = 0;
  double grant_multiplier = 1.0;
  std::string diagnostic_code = "SB_OPTIMIZER_FEEDBACK.MEMORY_GRANT_OK";
};

struct OptimizerFeedbackStatus {
  bool ok = false;
  bool applied = false;
  double estimate_error_ratio = 0.0;
  double page_error_ratio = 0.0;
  double io_error_ratio = 0.0;
  std::string diagnostic_code;
  std::vector<std::string> evidence;
  OptimizerCalibratedCostProfile cost_profile;
  OptimizerMemoryGrantFeedback memory_grant;
};

// SEARCH_KEY: OPCH_ADAPTIVE_FEEDBACK_ACTUALS_PERSISTENCE
// Scoped runtime actuals and feedback persistence is optimizer-advisory only.
// Records carry policy/scope generations and can be invalidated; they never
// become transaction finality, visibility, security, recovery, parser, or donor
// authority.
struct OptimizerRuntimeFeedbackRecord {
  std::string feedback_uuid;
  std::string scope_uuid;
  std::string route_label;
  std::uint64_t feedback_generation = 0;
  std::uint64_t policy_generation = 0;
  std::uint64_t catalog_epoch = 0;
  std::uint64_t security_epoch = 0;
  OptimizerRuntimeFeedback feedback;
  OptimizerFeedbackStatus status;
  bool valid = true;
  std::string invalidation_reason;
  std::vector<std::string> evidence;
};

struct OptimizerRuntimeFeedbackInvalidation {
  std::string scope_uuid;
  std::uint64_t policy_generation = 0;
  std::uint64_t catalog_epoch = 0;
  std::uint64_t security_epoch = 0;
  std::string reason;
};

struct OptimizerRuntimeFeedbackSnapshot {
  std::uint64_t total_records = 0;
  std::uint64_t valid_records = 0;
  std::uint64_t invalidated_records = 0;
  std::vector<OptimizerRuntimeFeedbackRecord> records;
};

class OptimizerRuntimeFeedbackStore {
 public:
  OptimizerFeedbackStatus Record(OptimizerRuntimeFeedbackRecord record);
  std::uint64_t Invalidate(const OptimizerRuntimeFeedbackInvalidation& event);
  OptimizerRuntimeFeedbackSnapshot Snapshot() const;
  std::optional<OptimizerRuntimeFeedbackRecord> Find(const std::string& feedback_uuid) const;

 private:
  mutable std::mutex mutex_;
  std::vector<OptimizerRuntimeFeedbackRecord> records_;
};

OptimizerFeedbackStatus EvaluateOptimizerRuntimeFeedback(const OptimizerRuntimeFeedback& feedback);
OptimizerCalibratedCostProfile BuildOptimizerCalibratedCostProfile(const OptimizerRuntimeFeedback& feedback,
                                                                    const OptimizerFeedbackStatus& status);

}  // namespace scratchbird::engine::optimizer

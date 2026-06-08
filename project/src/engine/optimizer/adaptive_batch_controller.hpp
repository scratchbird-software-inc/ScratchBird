// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "adaptive_batch_policy_evidence.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace scratchbird::engine::optimizer {

// SEARCH_KEY: SB_OPTIMIZER_ADAPTIVE_BATCH_CONTROLLER_ODF_100
// Adaptive batch feedback is advisory/resource-governance only. It may tune
// bounded batch sizes, but it is never transaction finality, row visibility,
// parser execution, donor/provider/client authority, autocommit, or recovery
// authority.
enum class AdaptiveBatchFamily : std::uint32_t {
  kUnknown,
  kInsert,
  kCopy,
  kSegmentVector,
  kBucketTimeSeries,
  kGraphFrontier,
  kIndexMerge
};

enum class AdaptiveBatchActionClass : std::uint32_t {
  kRefuse,
  kHold,
  kIncrease,
  kDecrease
};

struct AdaptiveBatchHistory {
  std::uint64_t success_count = 0;
  std::uint64_t error_count = 0;
  std::uint64_t consecutive_success_count = 0;
  std::uint64_t consecutive_error_count = 0;
};

struct AdaptiveBatchControllerRequest {
  AdaptiveBatchFamily family = AdaptiveBatchFamily::kUnknown;
  std::string family_label;
  std::uint64_t lower_bound = 0;
  std::uint64_t upper_bound = 0;
  std::uint64_t current_batch_size = 0;
  std::uint64_t latency_budget_microseconds = 0;
  std::uint64_t observed_latency_microseconds = 0;
  std::uint64_t memory_budget_bytes = 0;
  std::uint64_t observed_memory_bytes = 0;
  AdaptiveBatchHistory history;
  scratchbird::core::agents::AdaptiveBatchPolicyEvidence agent_evidence;
  bool benchmark_clean_input_evidence = false;
};

struct AdaptiveBatchBudgetEvidence {
  std::uint64_t latency_budget_microseconds = 0;
  std::uint64_t observed_latency_microseconds = 0;
  std::uint64_t memory_budget_bytes = 0;
  std::uint64_t observed_memory_bytes = 0;
  std::uint64_t backlog_units = 0;
  std::uint64_t backlog_budget_units = 0;
  std::uint64_t worker_pressure_ppm = 0;
  std::uint64_t quota_pressure_ppm = 0;
  bool latency_over_budget = false;
  bool memory_over_budget = false;
  bool backlog_over_budget = false;
  bool worker_pressure_high = false;
  bool quota_pressure_high = false;
};

struct AdaptiveBatchControllerResult {
  bool ok = false;
  bool fail_closed = true;
  bool benchmark_clean_evidence = false;
  bool advisory_only = true;
  bool throttle_recommended = false;
  std::uint64_t selected_batch_size = 0;
  AdaptiveBatchActionClass action = AdaptiveBatchActionClass::kRefuse;
  AdaptiveBatchBudgetEvidence budget;
  std::string diagnostic_code;
  std::vector<std::string> diagnostics;
  std::vector<std::string> evidence;
};

const char* AdaptiveBatchFamilyName(AdaptiveBatchFamily family);
const char* AdaptiveBatchActionClassName(AdaptiveBatchActionClass action);

AdaptiveBatchControllerResult EvaluateAdaptiveBatchController(
    const AdaptiveBatchControllerRequest& request);

std::string SerializeAdaptiveBatchControllerEvidence(
    const AdaptiveBatchControllerResult& result);

}  // namespace scratchbird::engine::optimizer

// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "optimizer_statistics_full.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace scratchbird::engine::optimizer {

// SEARCH_KEY: SB_OPTIMIZER_STATISTICS_LIFECYCLE_ODF_028
// Deterministic statistics refresh admission and evidence planning. Lifecycle
// decisions are optimizer/catalog metadata only: they do not own transaction
// finality, row visibility, parser execution, or donor behavior.
enum class OptimizerStatisticsLifecycleTrigger {
  kManualAnalyze,
  kSampledRefresh,
  kStaleDetection,
  kPostBulkRefresh,
  kHistogramRebuild,
  kMcvRebuild,
  kAdvisorSafeRefresh,
  kAgentAutoMaintenance,
};

enum class OptimizerStatisticsLifecycleDecision {
  kAdmitted,
  kRefused,
  kNoRefreshNeeded,
};

struct OptimizerStatisticsLifecycleRebuildPlan {
  std::string column_uuid;
  std::string statistic_uuid;
  std::uint64_t target_entry_count = 0;
};

struct OptimizerStatisticsLifecycleRequest {
  OptimizerStatisticsLifecycleTrigger trigger =
      OptimizerStatisticsLifecycleTrigger::kManualAnalyze;
  std::string relation_uuid;
  std::vector<std::string> column_uuids;

  std::uint64_t current_stats_epoch = 0;
  std::uint64_t request_stats_epoch = 0;
  std::uint64_t catalog_epoch = 0;
  std::uint64_t security_epoch = 0;
  std::uint64_t policy_epoch = 0;
  std::uint64_t stats_visibility_epoch = 0;

  OptimizerStatsFreshnessState current_freshness =
      OptimizerStatsFreshnessState::kMissing;
  bool stats_compatible = true;
  bool require_fresh_current_stats = false;

  std::uint64_t sampled_rows = 0;
  std::uint64_t total_rows_estimate = 0;
  std::uint64_t page_count = 0;
  std::uint64_t average_row_bytes = 0;
  std::uint64_t rows_modified_since_stats = 0;
  std::uint64_t bulk_rows_written = 0;
  std::uint64_t stale_row_threshold = 1;
  std::uint64_t histogram_bucket_target = 0;
  std::uint64_t mcv_entry_target = 0;

  bool policy_enabled = true;
  bool security_context_present = true;
  bool grants_proven = true;
  bool mga_visibility_recheck_present = true;
  bool security_recheck_present = true;
  bool epoch_evidence_present = true;
  bool advisory_only = true;
  bool parser_or_donor_authority = false;
  bool agent_policy_safe = true;
  bool catalog_descriptor_present = true;
  bool catalog_write_admitted = true;
  bool agent_runtime_registered = true;
  bool agent_schedule_admitted = true;
};

struct OptimizerStatisticsLifecycleResult {
  OptimizerStatisticsLifecycleDecision decision =
      OptimizerStatisticsLifecycleDecision::kRefused;
  bool accepted = false;
  bool refresh_needed = false;
  bool histogram_rebuild = false;
  bool mcv_rebuild = false;
  bool advisor_metadata_only = false;
  bool agent_action_safe = false;
  bool catalog_update_planned = false;
  bool agent_schedule_planned = false;
  bool row_visibility_semantics_changed = false;
  bool transaction_finality_semantics_changed = false;
  std::uint64_t next_stats_epoch = 0;
  std::uint64_t next_catalog_epoch = 0;
  std::uint64_t next_stats_visibility_epoch = 0;
  std::string diagnostic_code;
  std::vector<std::string> evidence;
  TableCardinalityStats planned_table_stats;
  bool has_planned_table_stats = false;
  std::vector<OptimizerStatisticsLifecycleRebuildPlan> histogram_plans;
  std::vector<OptimizerStatisticsLifecycleRebuildPlan> mcv_plans;
};

const char* OptimizerStatisticsLifecycleTriggerName(
    OptimizerStatisticsLifecycleTrigger trigger);
OptimizerStatisticsLifecycleResult EvaluateOptimizerStatisticsLifecycle(
    const OptimizerStatisticsLifecycleRequest& request);
std::string SerializeOptimizerStatisticsLifecycleEvidence(
    const OptimizerStatisticsLifecycleResult& result);

}  // namespace scratchbird::engine::optimizer

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
#include <string_view>
#include <optional>
#include <vector>

namespace scratchbird::engine::optimizer {

// SEARCH_KEY: SB_OPTIMIZER_STATISTICS_LIFECYCLE_ODF_028
// Deterministic statistics refresh admission and evidence planning. Lifecycle
// decisions are optimizer/catalog metadata only: they do not own transaction
// finality, row visibility, parser execution, or reference behavior.
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
  planner::CanonicalPlannerUuid column_uuid;
  // The collector issues a statistic identity only when it creates a record.
  std::uint64_t target_entry_count = 0;
};

struct OptimizerStatisticsLifecycleRequest {
  OptimizerStatisticsLifecycleTrigger trigger =
      OptimizerStatisticsLifecycleTrigger::kManualAnalyze;
  planner::CanonicalPlannerUuid relation_uuid;
  std::vector<planner::CanonicalPlannerUuid> column_uuids;

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

  // Optional existing observation, never a forecast or a replacement record.
  // Absence and an observed empty table are distinct.
  std::optional<TableCardinalityStats> observed_table_stats;
  std::uint64_t rows_modified_since_stats = 0;
  std::uint64_t bulk_rows_written = 0;
  std::uint64_t stale_row_threshold = 1;
  std::uint64_t histogram_bucket_target = 0;
  std::uint64_t mcv_entry_target = 0;

  // Caller-supplied planning prerequisites, not proof of runtime authority.
  // Execution must resolve and recheck the owning security/catalog/agent context.
  bool policy_enabled = false;
  bool security_context_present = false;
  bool grants_proven = false;
  bool mga_visibility_recheck_present = false;
  bool security_recheck_present = false;
  bool epoch_evidence_present = false;
  bool advisory_only = true;
  bool parser_or_reference_authority = false;
  bool agent_policy_safe = false;
  bool catalog_descriptor_present = false;
  bool catalog_write_admitted = false;
  bool agent_runtime_registered = false;
  bool agent_schedule_admitted = false;
};

enum class OptimizerStatisticsLifecycleFailure {
  kNone, kInvalidRequest, kAllocationFailure, kInternalFailure,
};

struct OptimizerStatisticsLifecycleResult {
  OptimizerStatisticsLifecycleFailure failure =
      OptimizerStatisticsLifecycleFailure::kNone;
  OptimizerStatisticsLifecycleTrigger trigger =
      OptimizerStatisticsLifecycleTrigger::kManualAnalyze;
  planner::CanonicalPlannerUuid relation_uuid;
  std::vector<planner::CanonicalPlannerUuid> column_uuids;
  std::uint64_t security_epoch = 0;
  std::uint64_t policy_epoch = 0;
  OptimizerStatisticsLifecycleDecision decision =
      OptimizerStatisticsLifecycleDecision::kRefused;
  bool accepted = false;
  bool refresh_needed = false;
  bool histogram_rebuild = false;
  bool mcv_rebuild = false;
  bool advisor_metadata_only = false;
  bool catalog_update_planned = false;
  bool agent_schedule_planned = false;
  bool row_visibility_semantics_changed = false;
  bool transaction_finality_semantics_changed = false;
  // A proposed statistics epoch, NOT a committed generation. Catalog and MGA
  // visibility epochs are captured unchanged; this evaluator has no writer.
  std::uint64_t next_stats_epoch = 0;
  std::uint64_t next_catalog_epoch = 0;
  std::uint64_t next_stats_visibility_epoch = 0;
  std::string_view diagnostic_code;
  std::string_view reason;
  std::vector<std::string> evidence;
  std::optional<TableCardinalityStats> observed_table_stats;
  std::vector<OptimizerStatisticsLifecycleRebuildPlan> histogram_plans;
  std::vector<OptimizerStatisticsLifecycleRebuildPlan> mcv_plans;
};

const char* OptimizerStatisticsLifecycleTriggerName(
    OptimizerStatisticsLifecycleTrigger trigger);
OptimizerStatisticsLifecycleResult EvaluateOptimizerStatisticsLifecycle(
    const OptimizerStatisticsLifecycleRequest& request) noexcept;
std::string SerializeOptimizerStatisticsLifecycleEvidence(
    const OptimizerStatisticsLifecycleResult& result);

}  // namespace scratchbird::engine::optimizer

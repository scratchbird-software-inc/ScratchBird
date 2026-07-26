// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "canonical_candidate_legality.hpp"
#include "cost_model.hpp"
#include "logical_plan.hpp"
#ifndef SCRATCHBIRD_QOW_CANONICAL_CANDIDATE_LEGALITY_ONLY
#include "access_path.hpp"
#include "access_path_full.hpp"
#include "join_planner.hpp"
#include "optimizer_feedback.hpp"
#include "physical_plan.hpp"
#include "selectivity_model.hpp"
#include "statistics_catalog.hpp"
#endif

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace scratchbird::engine::optimizer {

enum class CanonicalOptimizerStatisticState : std::uint8_t {
  kKnown = 1,
  kUnknown,
  kNotApplicable,
};

enum class CanonicalOptimizerStatisticSource : std::uint8_t {
  kCatalogExact = 1,
  kCatalogSample,
  kUnavailable,
};

struct CanonicalOptimizerNodeEstimate {
  std::uint32_t logical_node_id{0};
  std::string object_uuid;
  CanonicalOptimizerStatisticState state{
      CanonicalOptimizerStatisticState::kUnknown};
  CanonicalOptimizerStatisticSource source{
      CanonicalOptimizerStatisticSource::kUnavailable};
  std::string catalog_epoch_uuid;
  std::string statistics_snapshot_uuid;
  std::uint64_t statistics_generation{0};
  std::uint64_t collected_at_monotonic_ns{0};
  std::uint64_t admitted_at_monotonic_ns{0};
  std::uint64_t maximum_age_ns{0};
  CostConfidence confidence{CostConfidence::kUnknown};
  bool row_count_present{false};
  std::uint64_t row_count{0};
  bool page_count_present{false};
  std::uint64_t page_count{0};
  bool derived_from_runtime_actuals{false};
  bool benchmark_clean_authority_claimed{false};
};

struct CanonicalOptimizerStatisticsSnapshot {
  std::uint16_t abi_version{1};
  std::string statistics_snapshot_uuid;
  std::string catalog_epoch_uuid;
  std::uint64_t statistics_generation{0};
  std::uint64_t admitted_at_monotonic_ns{0};
  std::vector<CanonicalOptimizerNodeEstimate> node_estimates;
  bool captured_before_data_access{false};
  bool data_access_observed{false};
  bool runtime_actuals_present{false};
  bool parser_statistics_authority_claimed{false};
};

struct CanonicalOptimizerStatisticsIssue {
  std::string diagnostic_id;
  std::uint32_t logical_node_id{0};
  std::string field_id;
};

struct CanonicalOptimizerStatisticsAdmissionResult {
  bool accepted{false};
  bool degraded_for_unknown_statistics{false};
  bool benchmark_clean_ready{false};
  bool data_access_allowed{false};
  std::size_t known_estimate_count{0};
  std::size_t unknown_estimate_count{0};
  std::size_t not_applicable_estimate_count{0};
  std::vector<CanonicalOptimizerStatisticsIssue> issues;
};

// SEARCH_KEY: QOW-SOURCE-OPT-005-V1
CanonicalOptimizerStatisticsAdmissionResult
AdmitCanonicalOptimizerStatisticsBeforeAccess(
    const scratchbird::engine::planner::CanonicalLogicalRelationalGraph& graph,
    const CanonicalOptimizerStatisticsSnapshot& snapshot);

#ifndef SCRATCHBIRD_QOW_CANONICAL_CANDIDATE_LEGALITY_ONLY
// SEARCH_KEY: SB_OPTIMIZER_CONTRACT
struct OptimizerEvidence {
  bool has_usable_index = false;
  bool point_predicate = false;
  bool range_predicate = false;
  bool reorder_safe_join = false;
  std::uint64_t left_cardinality = 0;
  std::uint64_t right_cardinality = 0;
  bool grouping_present = false;
  bool ordered_input = false;
  std::string specialized_kind;
  bool exact_fallback_available = false;
};

struct OptimizerDecision {
  bool ok = false;
  scratchbird::engine::planner::PhysicalAccessKind access_kind = scratchbird::engine::planner::PhysicalAccessKind::kNone;
  std::string rule;
  std::string diagnostic_code;
  bool llvm_eligible = false;
  bool gpu_eligible = false;
};

struct OptimizerCandidate {
  scratchbird::engine::planner::LogicalPlanNode node;
  PlanCandidate plan_candidate;
  CostVector cost;
  bool selected = false;
  bool selected_in_physical_tree = false;
  bool rejected = false;
  std::string rejection_reason;
  std::string statistics_version;
};

struct OptimizedPlan {
  bool ok = false;
  std::string optimizer_profile = "deterministic_first_cost_v1";
  std::vector<OptimizerCandidate> candidates;
  bool has_physical_plan = false;
  PhysicalPlanNode physical_root;
  std::string selected_primary_candidate_id;
  std::string selected_primary_operation_id;
  std::vector<std::string> diagnostics;
};

OptimizedPlan OptimizeLogicalPlan(const scratchbird::engine::planner::LogicalPlan& plan);
OptimizedPlan OptimizeLogicalPlanWithStatistics(const scratchbird::engine::planner::LogicalPlan& plan,
                                                const OptimizerStatisticsCatalog& statistics);
OptimizedPlan OptimizeLogicalPlanWithAccessPathRequest(
    const scratchbird::engine::planner::LogicalPlan& plan,
    const AccessPathPlanningRequest& access_request);
StatisticsContractStatus ValidateBenchmarkCleanOptimizedPlan(const OptimizedPlan& plan);
std::string SerializeOptimizedPlanToJson(const OptimizedPlan& plan);
OptimizerDecision ChooseIndexAccess(const OptimizerEvidence& evidence);
OptimizerDecision ChooseJoinOrder(const OptimizerEvidence& evidence);
OptimizerDecision ChooseAggregateStrategy(const OptimizerEvidence& evidence);
OptimizerDecision ChooseSpecializedWorkloadAccess(const OptimizerEvidence& evidence);
#endif

}  // namespace scratchbird::engine::optimizer

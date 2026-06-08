// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "access_path.hpp"
#include "access_path_full.hpp"
#include "cost_model.hpp"
#include "join_planner.hpp"
#include "logical_plan.hpp"
#include "optimizer_feedback.hpp"
#include "physical_plan.hpp"
#include "selectivity_model.hpp"
#include "statistics_catalog.hpp"

#include <string>
#include <vector>

namespace scratchbird::engine::optimizer {

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

}  // namespace scratchbird::engine::optimizer

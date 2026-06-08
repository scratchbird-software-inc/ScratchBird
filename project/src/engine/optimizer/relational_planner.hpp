// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "logical_plan.hpp"
#include "physical_plan.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace scratchbird::engine::optimizer {

// SEARCH_KEY: SB_OPTIMIZER_RELATIONAL_PLANNER
// SEARCH_KEY: OPCH_RELATIONAL_OPERATOR_PLANNING_DEPTH
struct RelationalPlannerProofMetadata {
  bool ordering_metadata_present = false;
  bool memory_budget_present = false;
  bool spill_allowed = false;
  bool visibility_proof_present = false;
  bool transaction_proof_present = false;
  bool mutation_visibility_proof_present = false;
  bool mutation_transaction_proof_present = false;
  bool parser_or_sql_authority_claimed = false;
};

struct AggregatePlanningInput {
  std::uint64_t input_rows = 0;
  std::uint64_t group_count = 0;
  std::uint64_t row_width_bytes = 0;
  std::uint64_t memory_budget_bytes = 0;
  bool grouping_present = false;
  bool distinct_present = false;
  bool input_ordered_by_group = false;
  std::vector<std::string> group_expression_ids;
  scratchbird::engine::planner::LogicalPlanPropertyMetadata property_metadata;
  RelationalPlannerProofMetadata proof;
};

struct WindowPlanningInput {
  std::uint64_t input_rows = 0;
  std::uint64_t partition_count = 0;
  bool input_ordered = false;
  bool frame_requires_materialization = false;
  std::vector<std::string> partition_expression_ids;
  std::vector<scratchbird::engine::planner::LogicalPlanOrderingTerm> ordering_terms;
  scratchbird::engine::planner::LogicalPlanPropertyMetadata property_metadata;
  RelationalPlannerProofMetadata proof;
};

struct SortPlanningInput {
  std::uint64_t input_rows = 0;
  std::uint64_t row_width_bytes = 0;
  std::uint64_t memory_budget_bytes = 0;
  bool input_already_ordered = false;
  bool limit_present = false;
  std::uint64_t limit_count = 0;
  std::vector<scratchbird::engine::planner::LogicalPlanOrderingTerm> required_ordering;
  scratchbird::engine::planner::LogicalPlanPropertyMetadata property_metadata;
  RelationalPlannerProofMetadata proof;
};

struct SetOperationPlanningInput {
  std::uint64_t left_rows = 0;
  std::uint64_t right_rows = 0;
  bool distinct = false;
  bool inputs_ordered_compatibly = false;
  std::uint64_t row_width_bytes = 0;
  std::uint64_t memory_budget_bytes = 0;
  scratchbird::engine::planner::LogicalPlanPropertyMetadata property_metadata;
  RelationalPlannerProofMetadata proof;
};

struct LocalMutationPlanningInput {
  std::string mutation_kind;
  RelationalPlannerProofMetadata proof;
};

struct RelationalPlanDecision {
  bool ok = false;
  scratchbird::engine::planner::PhysicalAccessKind access_kind = scratchbird::engine::planner::PhysicalAccessKind::kNone;
  CostVector cost;
  std::vector<std::string> diagnostics;
};

RelationalPlanDecision PlanAggregate(const AggregatePlanningInput& input);
RelationalPlanDecision PlanWindow(const WindowPlanningInput& input);
RelationalPlanDecision PlanSortLimit(const SortPlanningInput& input);
RelationalPlanDecision PlanSetOperation(std::uint64_t left_rows, std::uint64_t right_rows, bool distinct);
RelationalPlanDecision PlanSetOperation(const SetOperationPlanningInput& input);
RelationalPlanDecision PlanLocalMutation(const std::string& mutation_kind, bool transaction_context_present, bool visibility_proven);
RelationalPlanDecision PlanLocalMutation(const LocalMutationPlanningInput& input);

}  // namespace scratchbird::engine::optimizer

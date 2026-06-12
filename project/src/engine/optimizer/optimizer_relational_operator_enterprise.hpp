// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "optimizer_cost_full.hpp"
#include "relational_planner.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace scratchbird::engine::optimizer {

// SEARCH_KEY: OEIC_RELATIONAL_OPERATOR_ENTERPRISE_CLOSURE
enum class EnterpriseRelationalOperatorKind {
  kAggregate,
  kDistinctAggregate,
  kSort,
  kTopN,
  kWindow,
  kSetOperation,
  kCte,
  kMutation,
  kResultStreaming,
};

struct EnterpriseRelationalOperatorAuthority {
  bool engine_optimizer_authority = true;
  bool parser_or_sql_authority = false;
  bool reference_or_legacy_authority = false;
  bool client_finality_or_visibility_authority = false;
  bool metric_finality_or_visibility_authority = false;
  bool recovery_authority = false;
  bool cluster_authority = false;
  bool fixture_or_test_authority = false;
};

struct EnterpriseRelationalOperatorRequest {
  EnterpriseRelationalOperatorKind kind = EnterpriseRelationalOperatorKind::kAggregate;
  std::string route_label;
  std::string plan_node_id;
  std::string result_contract_hash;
  std::string mutation_kind;
  std::uint64_t input_rows = 0;
  std::uint64_t right_rows = 0;
  std::uint64_t group_count = 0;
  std::uint64_t partition_count = 0;
  std::uint64_t row_width_bytes = 0;
  std::uint64_t memory_budget_bytes = 0;
  bool grouping_present = false;
  bool input_ordered = false;
  bool limit_present = false;
  std::uint64_t limit_count = 0;
  bool set_distinct = false;
  bool cte_reused = false;
  bool volatile_or_side_effecting = false;
  bool result_streaming_enabled = false;
  bool client_backpressure = false;
  std::uint64_t result_window_rows = 0;
  std::vector<std::string> group_expression_ids;
  std::vector<scratchbird::engine::planner::LogicalPlanOrderingTerm> ordering_terms;
  scratchbird::engine::planner::LogicalPlanPropertyMetadata property_metadata;
  RelationalPlannerProofMetadata proof;
  bool feedback_present = false;
  OptimizerRuntimeFeedback runtime_feedback;
  EnterpriseRelationalOperatorAuthority authority;
};

struct EnterpriseRelationalOperatorResult {
  bool ok = false;
  bool fail_closed = true;
  bool feedback_applied = false;
  bool streaming_result = false;
  bool materializes = false;
  scratchbird::engine::planner::PhysicalAccessKind access_kind =
      scratchbird::engine::planner::PhysicalAccessKind::kNone;
  CostVector cost;
  std::string diagnostic_code;
  std::vector<std::string> diagnostics;
  std::vector<std::string> evidence;
};

const char* EnterpriseRelationalOperatorKindName(EnterpriseRelationalOperatorKind kind);

EnterpriseRelationalOperatorResult PlanEnterpriseRelationalOperator(
    const EnterpriseRelationalOperatorRequest& request);

}  // namespace scratchbird::engine::optimizer

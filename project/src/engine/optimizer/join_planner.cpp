// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "join_planner.hpp"

#include <algorithm>
#include <limits>
#include <utility>

namespace scratchbird::engine::optimizer {
namespace planner = scratchbird::engine::planner;
namespace {

constexpr std::uint64_t kMaxCost = std::numeric_limits<std::uint64_t>::max();

std::uint64_t SaturatingAdd(std::uint64_t lhs, std::uint64_t rhs) {
  if (kMaxCost - lhs < rhs) return kMaxCost;
  return lhs + rhs;
}

std::uint64_t SaturatingMul(std::uint64_t lhs, std::uint64_t rhs) {
  if (lhs == 0 || rhs == 0) return 0;
  if (lhs > kMaxCost / rhs) return kMaxCost;
  return lhs * rhs;
}

void FinishCost(CostVector* cost) {
  cost->total_cost = SaturatingAdd(
      SaturatingAdd(SaturatingAdd(cost->startup_cost, cost->row_cost),
                    SaturatingAdd(cost->io_cost, cost->memory_cost)),
      cost->uncertainty_cost);
}

PlanCandidate JoinCandidate(const char* id,
                            planner::PhysicalAccessKind access_kind,
                            CostVector cost,
                            std::uint64_t estimated_rows) {
  PlanCandidate candidate;
  candidate.candidate_id = id;
  candidate.access_kind = access_kind;
  candidate.scope = "local";
  candidate.estimated_rows = estimated_rows;
  candidate.cost = std::move(cost);
  return candidate;
}

}  // namespace

JoinPlanDecision PlanLocalJoin(const JoinPlanningInput& input) {
  JoinPlanDecision decision;
  const std::uint64_t left = std::max<std::uint64_t>(1, input.left_cardinality);
  const std::uint64_t right = std::max<std::uint64_t>(1, input.right_cardinality);
  const std::uint64_t output_rows = std::max<std::uint64_t>(1, std::min(left, right));

  auto nested_cost = EstimateNodeCost(planner::MakeLogicalPlanNode(planner::LogicalPlanNodeKind::kDmlRead, planner::PhysicalAccessKind::kJoinNestedLoop, "query.join", "join_nested_loop"));
  nested_cost.row_cost = std::max<std::uint64_t>(1, SaturatingMul(left, right));
  FinishCost(&nested_cost);
  decision.candidates.push_back(JoinCandidate("CAND-OPT-010", planner::PhysicalAccessKind::kJoinNestedLoop, nested_cost, output_rows));

  auto hash_cost = EstimateNodeCost(planner::MakeLogicalPlanNode(planner::LogicalPlanNodeKind::kDmlRead, planner::PhysicalAccessKind::kJoinHash, "query.join", "join_hash"));
  if (!input.equi_join || !input.hash_join_executor_available || input.memory_budget_bytes < 4096) {
    hash_cost = RejectedCost(!input.equi_join ? "join_predicate_not_supported" : (!input.hash_join_executor_available ? "executor_hash_join_unavailable" : "memory_budget_insufficient"), 1000000ULL);
  } else {
    hash_cost.row_cost = SaturatingAdd(hash_cost.row_cost, SaturatingAdd(left, right) / 20);
    FinishCost(&hash_cost);
  }
  decision.candidates.push_back(JoinCandidate("CAND-OPT-011", planner::PhysicalAccessKind::kJoinHash, hash_cost, output_rows));

  auto merge_cost = EstimateNodeCost(planner::MakeLogicalPlanNode(planner::LogicalPlanNodeKind::kDmlRead, planner::PhysicalAccessKind::kJoinMerge, "query.join", "join_merge"));
  if (!input.ordered_inputs || !input.merge_join_executor_available) {
    merge_cost = RejectedCost(!input.ordered_inputs ? "required_ordering_unavailable" : "executor_merge_join_unavailable", 1000000ULL);
  } else {
    merge_cost.row_cost = SaturatingAdd(merge_cost.row_cost, SaturatingAdd(left, right) / 15);
    FinishCost(&merge_cost);
  }
  decision.candidates.push_back(JoinCandidate("CAND-OPT-012", planner::PhysicalAccessKind::kJoinMerge, merge_cost, output_rows));

  const auto best = ChooseBestSelectableCandidate(decision.candidates);
  if (!best) {
    decision.diagnostics.push_back("SB_OPTIMIZER_JOIN.NO_SELECTABLE_METHOD");
    return decision;
  }
  decision.ok = true;
  decision.selected_method = best->access_kind;
  for (auto& candidate : decision.candidates) {
    candidate.selected = candidate.candidate_id == best->candidate_id;
  }
  if (!input.reorder_safe) decision.diagnostics.push_back("SB_OPTIMIZER_JOIN.PRESERVE_INPUT_ORDER");
  return decision;
}

}  // namespace scratchbird::engine::optimizer

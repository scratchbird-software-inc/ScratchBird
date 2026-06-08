// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "join_planner_full.hpp"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace opt = scratchbird::engine::optimizer;
namespace plan = scratchbird::engine::planner;

namespace {

bool Require(bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << "OPCH join gate failure: " << message << '\n';
    return false;
  }
  return true;
}

bool Contains(const std::vector<std::string>& values, const std::string& expected) {
  return std::find(values.begin(), values.end(), expected) != values.end();
}

opt::JoinPredicateEdge Edge(std::string left,
                            std::string right,
                            double selectivity = 0.1,
                            bool equality = true) {
  opt::JoinPredicateEdge edge;
  edge.left_relation_uuid = std::move(left);
  edge.right_relation_uuid = std::move(right);
  edge.predicate_kind = equality ? "eq" : "non_eq";
  edge.semantic_kind = opt::JoinSemanticKind::kInner;
  edge.equality = equality;
  edge.selectivity = selectivity;
  return edge;
}

opt::JoinRelationNode Relation(std::string uuid,
                               std::uint64_t rows,
                               bool ordered = false,
                               bool covering = false,
                               bool visible = true,
                               bool materialized = false,
                               bool parallel = true) {
  opt::JoinRelationNode relation;
  relation.relation_uuid = std::move(uuid);
  relation.estimated_rows = rows;
  relation.order_preserving_required = ordered;
  relation.covering_path_available = covering;
  relation.native_visibility_preserved = visible;
  relation.materialization_required = materialized;
  relation.parallel_eligible = parallel;
  return relation;
}

bool RetainsPropertyFrontierAlternatives() {
  // SEARCH_KEY: OPCH_JOIN_MEMO_FRONTIER_PROPERTY_RETENTION
  auto graph = opt::BuildJoinGraph(
      {Relation("rel.big", 100000, false, true),
       Relation("rel.medium", 1000, false, true),
       Relation("rel.small", 10, false, true)},
      {Edge("rel.big", "rel.medium", 0.01),
       Edge("rel.medium", "rel.small", 0.01),
       Edge("rel.big", "rel.small", 0.25)},
      false,
      false);

  opt::JoinSearchPolicy policy;
  policy.strategy = opt::JoinSearchStrategy::kBoundedDp;
  policy.memory_budget_bytes = 8 * 1024 * 1024;
  policy.frontier_width = 8;
  policy.preserve_property_frontier = true;
  const auto plan_result = opt::EnumerateJoinOrderWithPolicy(graph, policy);

  return Require(plan_result.ok, "bounded DP failed") &&
         Require(plan_result.selected_strategy == opt::JoinSearchStrategy::kBoundedDp,
                 "bounded DP strategy not selected") &&
         Require(plan_result.property_frontier_retained,
                 "property frontier was not retained") &&
         Require(plan_result.frontier_entries_retained > plan_result.enumerated_subsets,
                 "frontier entry count did not exceed subset count") &&
         Require(plan_result.max_frontier_width > 1,
                 "frontier width did not retain alternatives") &&
         Require(Contains(plan_result.diagnostics, "SB_OPT_JOIN_FRONTIER_PROPERTY_RETENTION"),
                 "frontier retention diagnostic missing");
}

bool SupportsMultipleStrategiesAndTelemetry() {
  // SEARCH_KEY: OPCH_MULTIPLE_JOIN_STRATEGIES_TELEMETRY
  auto graph = opt::BuildJoinGraph(
      {Relation("rel.a", 50), Relation("rel.b", 20), Relation("rel.c", 10)},
      {Edge("rel.a", "rel.b"), Edge("rel.b", "rel.c")},
      false,
      false);

  opt::JoinSearchPolicy exhaustive;
  exhaustive.strategy = opt::JoinSearchStrategy::kExhaustiveDp;
  exhaustive.memory_budget_bytes = 8 * 1024 * 1024;
  const auto exhaustive_plan = opt::EnumerateJoinOrderWithPolicy(graph, exhaustive);

  opt::JoinSearchPolicy greedy;
  greedy.strategy = opt::JoinSearchStrategy::kHypergraphGreedy;
  const auto greedy_plan = opt::EnumerateJoinOrderWithPolicy(graph, greedy);

  opt::JoinSearchPolicy input_order;
  input_order.strategy = opt::JoinSearchStrategy::kInputOrder;
  const auto input_plan = opt::EnumerateJoinOrderWithPolicy(graph, input_order);

  return Require(exhaustive_plan.ok, "exhaustive DP failed") &&
         Require(exhaustive_plan.selected_strategy == opt::JoinSearchStrategy::kExhaustiveDp,
                 "exhaustive DP strategy not selected") &&
         Require(Contains(exhaustive_plan.diagnostics, "SB_OPT_JOIN_EXHAUSTIVE_DP_SELECTED"),
                 "exhaustive DP diagnostic missing") &&
         Require(greedy_plan.ok, "hypergraph greedy failed") &&
         Require(greedy_plan.selected_strategy == opt::JoinSearchStrategy::kHypergraphGreedy,
                 "hypergraph greedy strategy not selected") &&
         Require(Contains(greedy_plan.diagnostics, "SB_OPT_JOIN_HYPERGRAPH_GREEDY_SELECTED"),
                 "hypergraph greedy diagnostic missing") &&
         Require(input_plan.ok, "input-order strategy failed") &&
         Require(input_plan.selected_strategy == opt::JoinSearchStrategy::kInputOrder,
                 "input-order strategy not selected");
}

bool PreservesSemanticBarriersAndLegalityDiagnostics() {
  // SEARCH_KEY: OPCH_JOIN_LEGALITY_BARRIER_PROPERTY_REGRESSION
  auto graph = opt::BuildJoinGraph(
      {Relation("rel.left", 1000), Relation("rel.right", 100)},
      [&]() {
        auto edge = Edge("rel.left", "rel.right");
        edge.semantic_kind = opt::JoinSemanticKind::kLeftOuter;
        edge.outer_join_sensitive = true;
        edge.nullable = true;
        return std::vector<opt::JoinPredicateEdge>{edge};
      }(),
      true,
      false);

  opt::JoinSearchPolicy policy;
  policy.strategy = opt::JoinSearchStrategy::kBoundedDp;
  policy.memory_budget_bytes = 8 * 1024 * 1024;
  const auto plan_result = opt::EnumerateJoinOrderWithPolicy(graph, policy);

  return Require(plan_result.ok, "semantic barrier plan failed") &&
         Require(plan_result.selected_strategy == opt::JoinSearchStrategy::kInputOrder,
                 "semantic barrier did not force input order") &&
         Require(plan_result.semantic_order_preserved,
                 "semantic order was not preserved") &&
         Require(Contains(plan_result.diagnostics, "SB_OPT_JOIN_ORDER_PRESERVED_OUTER_JOIN"),
                 "outer join diagnostic missing") &&
         Require(Contains(plan_result.diagnostics, "SB_OPT_JOIN_ORDER_PRESERVED_NULLABLE_EDGE"),
                 "nullable edge diagnostic missing") &&
         Require(!opt::JoinReorderAllowed(graph),
                 "semantic barrier still allowed reordering");
}

}  // namespace

int main() {
  if (!RetainsPropertyFrontierAlternatives()) return EXIT_FAILURE;
  if (!SupportsMultipleStrategiesAndTelemetry()) return EXIT_FAILURE;
  if (!PreservesSemanticBarriersAndLegalityDiagnostics()) return EXIT_FAILURE;
  return EXIT_SUCCESS;
}

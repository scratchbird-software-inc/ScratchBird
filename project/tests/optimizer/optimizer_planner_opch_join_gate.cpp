// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "join_planner_full.hpp"
#include "../sbsql_sblr_alignment/binary_uuid_fixture.hpp"
#include <limits>
#include <type_traits>

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace opt = scratchbird::engine::optimizer;
namespace plan = scratchbird::engine::planner;

namespace {

using scratchbird::tests::BinaryUuid;
std::size_t checks = 0;
static_assert(sizeof(plan::CanonicalPlannerUuid) == 16);
static_assert(std::is_same_v<decltype(opt::JoinRelationNode::relation_uuid), plan::CanonicalPlannerUuid>);

bool Require(bool condition, const std::string& message) {
  ++checks;
  if (!condition) {
    std::cerr << "OPCH join gate failure: " << message << '\n';
    return false;
  }
  return true;
}

bool Contains(const std::vector<std::string>& values, const std::string& expected) {
  return std::find(values.begin(), values.end(), expected) != values.end();
}

opt::JoinPredicateEdge Edge(plan::CanonicalPlannerUuid left,
                            plan::CanonicalPlannerUuid right,
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

opt::JoinRelationNode Relation(plan::CanonicalPlannerUuid uuid,
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
      {Relation(BinaryUuid("018fa120-0000-7000-8000-000000000001"), 100000, false, true),
       Relation(BinaryUuid("018fa120-0000-7000-8000-000000000002"), 1000, false, true),
       Relation(BinaryUuid("018fa120-0000-7000-8000-000000000003"), 10, false, true)},
      {Edge(BinaryUuid("018fa120-0000-7000-8000-000000000001"), BinaryUuid("018fa120-0000-7000-8000-000000000002"), 0.01),
       Edge(BinaryUuid("018fa120-0000-7000-8000-000000000002"), BinaryUuid("018fa120-0000-7000-8000-000000000003"), 0.01),
       Edge(BinaryUuid("018fa120-0000-7000-8000-000000000001"), BinaryUuid("018fa120-0000-7000-8000-000000000003"), 0.25)},
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
      {Relation(BinaryUuid("018fa120-0000-7000-8000-000000000004"), 50), Relation(BinaryUuid("018fa120-0000-7000-8000-000000000005"), 20), Relation(BinaryUuid("018fa120-0000-7000-8000-000000000006"), 10)},
      {Edge(BinaryUuid("018fa120-0000-7000-8000-000000000004"), BinaryUuid("018fa120-0000-7000-8000-000000000005")), Edge(BinaryUuid("018fa120-0000-7000-8000-000000000005"), BinaryUuid("018fa120-0000-7000-8000-000000000006"))},
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
      {Relation(BinaryUuid("018fa120-0000-7000-8000-000000000007"), 1000), Relation(BinaryUuid("018fa120-0000-7000-8000-000000000008"), 100)},
      [&]() {
        auto edge = Edge(BinaryUuid("018fa120-0000-7000-8000-000000000007"), BinaryUuid("018fa120-0000-7000-8000-000000000008"));
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

bool WideJoinSearch() {
  for (std::size_t count : {16u, 17u, 63u, 64u, 65u, 130u}) {
    std::vector<opt::JoinRelationNode> relations;
    std::vector<opt::JoinPredicateEdge> predicates;
    std::vector<plan::CanonicalPlannerUuid> expected;
    for (std::size_t i = 0; i != count; ++i) {
      auto id = BinaryUuid("018fa120-0000-7000-8000-000000000000");
      id.bytes[14] = static_cast<unsigned char>(i >> 8);
      id.bytes[15] = static_cast<unsigned char>(i);
      expected.push_back(id);
      relations.push_back(Relation(id, count - i));
      if (i) predicates.push_back(Edge(expected[i - 1], id, 1.0));
    }
    const auto graph = opt::BuildJoinGraph(relations, predicates, false, false);
    for (auto strategy : {opt::JoinSearchStrategy::kInputOrder,
                          opt::JoinSearchStrategy::kHeuristicGreedy,
                          opt::JoinSearchStrategy::kHypergraphGreedy,
                          opt::JoinSearchStrategy::kBoundedDp}) {
      opt::JoinSearchPolicy policy;
      policy.strategy = strategy;
      policy.bounded_relation_limit = 8;
      const auto result = opt::EnumerateJoinOrderWithPolicy(graph, policy);
      auto wanted = expected;
      if (strategy != opt::JoinSearchStrategy::kInputOrder)
        std::reverse(wanted.begin(), wanted.end());
      if (!Require(result.ok && result.ordered_relation_uuids == wanted,
                   "wide search retains every exact occurrence in the expected order")) return false;
      if (!Require(result.selected_strategy ==
                      (strategy == opt::JoinSearchStrategy::kBoundedDp ?
                       opt::JoinSearchStrategy::kHeuristicGreedy : strategy),
                   "reported fallback strategy matches executed algorithm")) return false;
    }
  }
  // A disconnected extension cannot reuse predicates internal to the prefix.
  const auto a = BinaryUuid("018fa120-0000-7000-8000-000000000301");
  const auto b = BinaryUuid("018fa120-0000-7000-8000-000000000302");
  const auto c = BinaryUuid("018fa120-0000-7000-8000-000000000303");
  const auto graph = opt::BuildJoinGraph({Relation(a, 100), Relation(b, 100), Relation(c, 10)},
                                       {Edge(a, b, 0.5)}, false, false);
  opt::JoinSearchPolicy policy;
  policy.strategy = opt::JoinSearchStrategy::kInputOrder;
  const auto result = opt::EnumerateJoinOrderWithPolicy(graph, policy);
  return Require(result.ok && result.estimated_rows == 50000,
                 "disconnected next relation has unit selectivity");
}

bool BinaryGraphAdmission() {
  const auto left = BinaryUuid("018fa120-0000-7000-8000-000000000101");
  const auto right = BinaryUuid("018fa120-0000-7000-8000-000000000202");
  const auto graph = opt::BuildJoinGraph({Relation(left, 10), Relation(right, 20)},
                                       {Edge(left, right)}, false, false);
  opt::JoinSearchPolicy policy;
  policy.strategy = opt::JoinSearchStrategy::kInputOrder;
  for (unsigned bit = 0; bit != 128; ++bit) {
    auto changed = graph;
    auto id = left;
    id.bytes[bit / 8] ^= 1u << (bit % 8);
    changed.relations[0].relation_uuid = id;
    changed.predicates[0].left_relation_uuid = id;
    const bool valid = (id.bytes[6] >> 4) == 7 && (id.bytes[8] & 0xc0) == 0x80;
    const auto result = opt::EnumerateJoinOrderWithPolicy(changed, policy);
    if (!Require(result.ok == valid, "all 128 identity bits use binary admission")) return false;
    if (valid && !Require(result.ordered_relation_uuids ==
                             std::vector<plan::CanonicalPlannerUuid>{id, right},
                         "plan retains exact binary identity bits")) return false;
    changed.predicates[0].left_relation_uuid = left;
    const auto dangling = opt::EnumerateJoinOrderWithPolicy(changed, policy);
    if (!Require(!dangling.ok && dangling.ordered_relation_uuids.empty(),
                 "changed relation cannot leave a silently dropped edge")) return false;
  }
  auto duplicate = graph;
  duplicate.relations[1].relation_uuid = left;
  if (!Require(!opt::EnumerateJoinOrderWithPolicy(duplicate, policy).ok &&
                   !opt::JoinReorderAllowed(duplicate), "duplicate bindings refused")) return false;
  auto nil = graph;
  nil.relations[0].relation_uuid = {};
  if (!Require(!opt::EnumerateJoinOrderWithPolicy(nil, policy).ok,
               "nil is not a relation binding")) return false;
  for (double value : {-1.0, 1.01, std::numeric_limits<double>::infinity(),
                       std::numeric_limits<double>::quiet_NaN()}) {
    auto invalid = graph;
    invalid.predicates[0].selectivity = value;
    if (!Require(!opt::EnumerateJoinOrderWithPolicy(invalid, policy).ok,
                 "invalid fraction cannot reach cost conversion")) return false;
  }
  auto invalid_kind = graph;
  invalid_kind.predicates[0].semantic_kind = static_cast<opt::JoinSemanticKind>(255);
  if (!Require(!opt::EnumerateJoinOrderWithPolicy(invalid_kind, policy).ok,
               "unknown semantics refused")) return false;
  auto self_edge = graph;
  self_edge.predicates[0].right_relation_uuid = left;
  if (!Require(!opt::EnumerateJoinOrderWithPolicy(self_edge, policy).ok,
               "same-occurrence edge is not a join edge")) return false;
  auto malformed = graph;
  malformed.predicates[0].predicate_count = 0;
  if (!Require(!opt::EnumerateJoinOrderWithPolicy(malformed, policy).ok,
               "mutable graph cannot bypass predicate shape validation")) return false;
  malformed = graph;
  malformed.predicates[0].semantic_kind = opt::JoinSemanticKind::kCross;
  if (!Require(!opt::EnumerateJoinOrderWithPolicy(malformed, policy).ok,
               "mutable cross edge cannot retain an equality predicate")) return false;
  const auto invalid_construction = opt::BuildJoinGraph(
      {Relation(left, 10)}, {Edge(left, right)}, false, false);
  return Require(!invalid_construction.valid, "builder validates endpoint membership");
}

}  // namespace

int main() {
  if (!RetainsPropertyFrontierAlternatives()) return EXIT_FAILURE;
  if (!SupportsMultipleStrategiesAndTelemetry()) return EXIT_FAILURE;
  if (!PreservesSemanticBarriersAndLegalityDiagnostics()) return EXIT_FAILURE;
  if (!BinaryGraphAdmission()) return EXIT_FAILURE;
  if (!WideJoinSearch()) return EXIT_FAILURE;
  std::cout << checks << " binary join checks passed\n";
  return EXIT_SUCCESS;
}

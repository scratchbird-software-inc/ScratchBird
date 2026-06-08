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

namespace {

bool Require(bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << "OEIC join memo gate failure: " << message << '\n';
    return false;
  }
  return true;
}

bool Contains(const std::vector<std::string>& values, const std::string& expected) {
  return std::find(values.begin(), values.end(), expected) != values.end();
}

bool ContainsText(const std::string& value, const std::string& expected) {
  return value.find(expected) != std::string::npos;
}

opt::JoinRelationNode Relation(std::string uuid,
                               std::uint64_t rows,
                               std::uint64_t memory_profile_bytes = 0,
                               bool ordered = false,
                               bool covering = true,
                               bool visibility = true,
                               bool exact = true,
                               bool materialized = false,
                               bool parallel = true,
                               bool correlated = false,
                               bool lateral = false) {
  opt::JoinRelationNode relation;
  relation.relation_uuid = std::move(uuid);
  relation.estimated_rows = rows;
  relation.memory_profile_bytes = memory_profile_bytes;
  relation.order_preserving_required = ordered;
  relation.covering_path_available = covering;
  relation.native_visibility_preserved = visibility;
  relation.exact_output_preserved = exact;
  relation.materialization_required = materialized;
  relation.parallel_eligible = parallel;
  relation.correlated_dependency = correlated;
  relation.lateral_dependency = lateral;
  return relation;
}

opt::JoinPredicateEdge Edge(std::string left,
                            std::string right,
                            double selectivity = 0.01,
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

bool RetainsMemoryProfileFrontierAlternatives() {
  // SEARCH_KEY: OEIC_ENTERPRISE_JOIN_MEMO_FRONTIER
  const auto graph = opt::BuildJoinGraph(
      {Relation("rel.build", 100000),
       Relation("rel.probe", 100000)},
      {Edge("rel.build", "rel.probe", 0.01, true)},
      false,
      false);

  opt::JoinSearchPolicy policy;
  policy.strategy = opt::JoinSearchStrategy::kBoundedDp;
  policy.memory_budget_bytes = 64 * 1024 * 1024;
  policy.frontier_width = 8;
  policy.preserve_property_frontier = true;

  const auto result = opt::EnumerateJoinOrderWithPolicy(graph, policy);
  return Require(result.ok, "bounded DP failed") &&
         Require(result.property_frontier_retained,
                 "memory-profile alternatives were collapsed") &&
         Require(result.frontier_entries_retained > result.enumerated_subsets,
                 "frontier did not retain non-dominated alternatives") &&
         Require(result.max_frontier_width > 1,
                 "frontier width did not retain memory alternatives") &&
         Require(result.memory_profile_kib > 0,
                 "selected plan did not report memory profile") &&
         Require(Contains(result.diagnostics, "SB_OPT_JOIN_FRONTIER_PROPERTY_RETENTION"),
                 "frontier retention diagnostic missing") &&
         Require(ContainsText(result.selected_property_signature, "memory="),
                 "memory profile missing from selected property signature") &&
         Require(ContainsText(result.selected_property_signature, "exact=1"),
                 "exactness missing from selected property signature");
}

bool PreservesExactnessAndCorrelationProperties() {
  const auto graph = opt::BuildJoinGraph(
      {Relation("rel.outer", 1000, 0, true, true, true, true, false, true, true),
       Relation("rel.lossy", 100, 256 * 1024, true, true, true, false)},
      {Edge("rel.outer", "rel.lossy", 0.05, true)},
      false,
      false);

  opt::JoinSearchPolicy policy;
  policy.strategy = opt::JoinSearchStrategy::kBoundedDp;
  policy.memory_budget_bytes = 8 * 1024 * 1024;
  policy.frontier_width = 8;
  policy.preserve_property_frontier = true;

  const auto result = opt::EnumerateJoinOrderWithPolicy(graph, policy);
  return Require(result.ok, "correlated exactness plan failed") &&
         Require(!result.exact_output_preserved,
                 "lossy base path did not propagate exactness loss") &&
         Require(result.semantic_order_preserved,
                 "correlation did not preserve semantic input order") &&
         Require(Contains(result.diagnostics, "SB_OPT_JOIN_ORDER_PRESERVED_CORRELATION"),
                 "correlation diagnostic missing") &&
         Require(ContainsText(result.selected_property_signature, "exact=0"),
                 "lossy exactness missing from selected property signature") &&
         Require(ContainsText(result.selected_property_signature, "correlated=1"),
                 "correlation missing from selected property signature") &&
         Require(ContainsText(result.selected_property_signature, "memory="),
                 "memory profile missing from correlated property signature");
}

bool CollapsesFrontierOnlyWhenPolicyDisablesRetention() {
  const auto graph = opt::BuildJoinGraph(
      {Relation("rel.a", 100000),
       Relation("rel.b", 100000)},
      {Edge("rel.a", "rel.b", 0.01, true)},
      false,
      false);

  opt::JoinSearchPolicy policy;
  policy.strategy = opt::JoinSearchStrategy::kBoundedDp;
  policy.memory_budget_bytes = 64 * 1024 * 1024;
  policy.frontier_width = 8;
  policy.preserve_property_frontier = false;

  const auto result = opt::EnumerateJoinOrderWithPolicy(graph, policy);
  return Require(result.ok, "single-state bounded DP failed") &&
         Require(!result.property_frontier_retained,
                 "property frontier retained despite policy disable") &&
         Require(result.max_frontier_width == 1,
                 "frontier width exceeded one when retention disabled");
}

}  // namespace

int main() {
  if (!RetainsMemoryProfileFrontierAlternatives()) return EXIT_FAILURE;
  if (!PreservesExactnessAndCorrelationProperties()) return EXIT_FAILURE;
  if (!CollapsesFrontierOnlyWhenPolicyDisablesRetention()) return EXIT_FAILURE;
  return EXIT_SUCCESS;
}

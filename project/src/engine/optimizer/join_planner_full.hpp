// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "access_path_full.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace scratchbird::engine::optimizer {

// SEARCH_KEY: SB_OPTIMIZER_JOIN_GRAPH_ENUMERATOR
struct JoinRelationNode {
  std::string relation_uuid;
  std::uint64_t estimated_rows = 0;
  std::uint64_t memory_profile_bytes = 0;
  bool order_preserving_required = false;
  bool covering_path_available = false;
  bool native_visibility_preserved = true;
  bool exact_output_preserved = true;
  bool materialization_required = false;
  bool parallel_eligible = true;
  bool semantic_order_barrier = false;
  bool correlated_dependency = false;
  bool lateral_dependency = false;
  bool volatile_dependency = false;
};

enum class JoinSemanticKind {
  kInner,
  kLeftOuter,
  kRightOuter,
  kFullOuter,
  kSemi,
  kAnti,
};

struct JoinPredicateEdge {
  std::string left_relation_uuid;
  std::string right_relation_uuid;
  std::string predicate_kind;
  JoinSemanticKind semantic_kind = JoinSemanticKind::kInner;
  bool equality = false;
  bool nullable = false;
  bool outer_join_sensitive = false;
  bool correlated = false;
  bool lateral = false;
  bool volatile_predicate = false;
  bool explicit_order_barrier = false;
  double selectivity = 0.10;
};

struct JoinGraph {
  std::vector<JoinRelationNode> relations;
  std::vector<JoinPredicateEdge> predicates;
  bool contains_outer_join = false;
  bool contains_semi_or_anti = false;
  bool contains_correlation = false;
  bool contains_lateral = false;
  bool contains_volatile = false;
  bool contains_explicit_barrier = false;
};

// SEARCH_KEY: OPCH_MULTIPLE_JOIN_STRATEGIES_TELEMETRY
enum class JoinSearchStrategy {
  kAuto,
  kExhaustiveDp,
  kBoundedDp,
  kHypergraphGreedy,
  kHeuristicGreedy,
  kInputOrder,
};

// SEARCH_KEY: OPCH_JOIN_MEMO_FRONTIER_PROPERTY_RETENTION
struct JoinSearchPolicy {
  JoinSearchStrategy strategy = JoinSearchStrategy::kAuto;
  std::uint64_t memory_budget_bytes = 0;
  std::uint64_t transition_budget = 0;
  std::size_t exhaustive_relation_limit = 10;
  std::size_t bounded_relation_limit = 16;
  std::size_t frontier_width = 8;
  bool preserve_property_frontier = true;
};

struct JoinOrderPlan {
  bool ok = false;
  std::vector<std::string> ordered_relation_uuids;
  scratchbird::engine::planner::PhysicalAccessKind method = scratchbird::engine::planner::PhysicalAccessKind::kJoinNestedLoop;
  CostVector cost;
  std::uint64_t estimated_rows = 0;
  std::uint64_t memory_profile_kib = 0;
  std::uint64_t enumerated_subsets = 0;
  std::uint64_t transitions_considered = 0;
  std::uint64_t pruned_alternatives = 0;
  std::uint64_t frontier_entries_retained = 0;
  std::uint64_t dominated_states = 0;
  std::size_t max_frontier_width = 0;
  bool reorder_applied = false;
  bool semantic_order_preserved = false;
  bool bounded_enumeration_applied = false;
  bool pruning_applied = false;
  bool property_frontier_retained = false;
  bool exact_output_preserved = true;
  JoinSearchStrategy requested_strategy = JoinSearchStrategy::kAuto;
  JoinSearchStrategy selected_strategy = JoinSearchStrategy::kAuto;
  std::string selected_property_signature;
  std::string fallback_reason;
  std::vector<std::string> diagnostics;
};

JoinGraph BuildJoinGraph(std::vector<JoinRelationNode> relations,
                         std::vector<JoinPredicateEdge> predicates,
                         bool contains_outer_join,
                         bool contains_semi_or_anti);
JoinOrderPlan EnumerateDeterministicJoinOrder(const JoinGraph& graph, std::uint64_t memory_budget_bytes);
JoinOrderPlan EnumerateJoinOrderWithPolicy(const JoinGraph& graph, const JoinSearchPolicy& policy);
CostVector CostNestedLoopJoin(std::uint64_t outer_rows, std::uint64_t inner_rows, double selectivity);
CostVector CostHashJoin(std::uint64_t build_rows, std::uint64_t probe_rows, std::uint64_t memory_budget_bytes, double selectivity);
CostVector CostMergeJoin(std::uint64_t left_rows, std::uint64_t right_rows, bool inputs_ordered, double selectivity);
bool JoinReorderAllowed(const JoinGraph& graph);
bool SemiAntiJoinCanDecorrelate(const JoinPredicateEdge& predicate);
const char* JoinSearchStrategyName(JoinSearchStrategy strategy);

}  // namespace scratchbird::engine::optimizer

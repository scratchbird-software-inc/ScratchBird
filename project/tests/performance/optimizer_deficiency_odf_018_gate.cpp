// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "join_planner.hpp"
#include "join_planner_full.hpp"
#include "logical_plan.hpp"
#include "optimizer_contract.hpp"
#include "statistics_catalog.hpp"

#include <algorithm>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

namespace opt = scratchbird::engine::optimizer;
namespace plan = scratchbird::engine::planner;

namespace {

bool Require(bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << message << '\n';
    return false;
  }
  return true;
}

bool HasDiagnostic(const std::vector<std::string>& diagnostics, const std::string& diagnostic) {
  return std::find(diagnostics.begin(), diagnostics.end(), diagnostic) != diagnostics.end();
}

bool HasEvidence(const opt::PhysicalPlanNode& node, const std::string& evidence) {
  return std::find(node.runtime_evidence.begin(), node.runtime_evidence.end(), evidence) !=
         node.runtime_evidence.end();
}

bool TreeContainsEvidence(const opt::PhysicalPlanNode& node, const std::string& evidence) {
  if (HasEvidence(node, evidence)) return true;
  return std::any_of(node.children.begin(), node.children.end(), [&](const opt::PhysicalPlanNode& child) {
    return TreeContainsEvidence(child, evidence);
  });
}

void AddRelationStats(opt::OptimizerStatisticsCatalog* catalog,
                      const std::string& relation_uuid,
                      double row_count) {
  const auto add = [&](const std::string& name, double value) {
    catalog->Add(opt::MakeStatistic(name,
                                    "relation",
                                    relation_uuid,
                                    value,
                                    opt::StatisticSource::kCatalogExact,
                                    18,
                                    0,
                                    opt::CostConfidence::kHigh));
  };
  add("row_count", row_count);
  add("visible_row_count", row_count);
  add("page_count", std::max(1.0, row_count / 100.0));
  add("average_row_bytes", 96.0);
  add("filespace_available_pages", std::max(4.0, row_count / 50.0));
  add("page_cache_hit_ratio", 0.80);
  add("page_cache_pressure_level", 0.10);
  add("page_family_read_latency_microseconds", 500.0);
}

opt::OptimizerStatisticsCatalog CardinalityCatalog() {
  opt::OptimizerStatisticsCatalog catalog;
  AddRelationStats(&catalog, "rel.big", 10000.0);
  AddRelationStats(&catalog, "rel.medium", 200.0);
  AddRelationStats(&catalog, "rel.small", 10.0);
  catalog.Add(opt::MakeStatistic("memory_grant_available_bytes",
                                 "session",
                                 "local.default",
                                 8.0 * 1024.0 * 1024.0,
                                 opt::StatisticSource::kCatalogExact,
                                 18,
                                 0,
                                 opt::CostConfidence::kHigh));
  catalog.Add(opt::MakeStatistic("join_selectivity",
                                 "query",
                                 "local.default",
                                 0.05,
                                 opt::StatisticSource::kCatalogExact,
                                 18,
                                 0,
                                 opt::CostConfidence::kHigh));
  return catalog;
}

opt::JoinGraph ReorderSafeInnerGraph() {
  std::vector<opt::JoinRelationNode> relations = {
      {"rel.big", 10000, false},
      {"rel.medium", 200, false},
      {"rel.small", 10, false},
  };
  std::vector<opt::JoinPredicateEdge> predicates;
  opt::JoinPredicateEdge big_medium;
  big_medium.left_relation_uuid = "rel.big";
  big_medium.right_relation_uuid = "rel.medium";
  big_medium.predicate_kind = "join.equi";
  big_medium.equality = true;
  big_medium.selectivity = 0.05;
  predicates.push_back(big_medium);
  opt::JoinPredicateEdge medium_small = big_medium;
  medium_small.left_relation_uuid = "rel.medium";
  medium_small.right_relation_uuid = "rel.small";
  predicates.push_back(medium_small);
  return opt::BuildJoinGraph(std::move(relations), std::move(predicates), false, false);
}

bool DirectBoundedDpReordersInnerJoins() {
  const auto graph = ReorderSafeInnerGraph();
  const auto order = opt::EnumerateDeterministicJoinOrder(graph, 8 * 1024 * 1024);
  const std::vector<std::string> expected = {"rel.small", "rel.medium", "rel.big"};
  return Require(order.ok, "bounded DP did not produce an inner-join order") &&
         Require(order.ordered_relation_uuids == expected,
                 "bounded DP did not choose the row-driven non-input order") &&
         Require(order.method == plan::PhysicalAccessKind::kJoinHash,
                 "bounded DP selected a method without an executable physical payload") &&
         Require(order.reorder_applied, "bounded DP did not report reorder_applied") &&
         Require(order.bounded_enumeration_applied, "bounded DP was not recorded as applied") &&
         Require(order.transitions_considered != 0, "bounded DP did not consider connected transitions") &&
         Require(HasDiagnostic(order.diagnostics, "SB_OPT_JOIN_DP_BOUNDED_ENUMERATION_APPLIED"),
                 "bounded DP diagnostic missing") &&
         Require(HasDiagnostic(order.diagnostics, "SB_OPT_JOIN_DP_CONNECTED_SUBSETS_ENUMERATED"),
                 "connected subset diagnostic missing");
}

bool DirectBoundedDpRecordsBudgetPruning() {
  std::vector<opt::JoinRelationNode> relations;
  for (std::size_t i = 0; i < 8; ++i) {
    opt::JoinRelationNode relation;
    relation.relation_uuid = "rel.dp" + std::to_string(i);
    relation.estimated_rows = 1000 + i;
    relations.push_back(std::move(relation));
  }
  std::vector<opt::JoinPredicateEdge> predicates;
  for (std::size_t i = 0; i < relations.size(); ++i) {
    for (std::size_t j = i + 1; j < relations.size(); ++j) {
      opt::JoinPredicateEdge edge;
      edge.left_relation_uuid = relations[i].relation_uuid;
      edge.right_relation_uuid = relations[j].relation_uuid;
      edge.predicate_kind = "join.equi";
      edge.equality = true;
      edge.selectivity = 0.10;
      predicates.push_back(std::move(edge));
    }
  }
  const auto graph = opt::BuildJoinGraph(std::move(relations), std::move(predicates), false, false);
  const auto order = opt::EnumerateDeterministicJoinOrder(graph, 1);
  return Require(order.ok, "bounded DP pruning fallback did not produce a deterministic plan") &&
         Require(order.bounded_enumeration_applied, "bounded DP did not report enumeration under budget") &&
         Require(order.pruning_applied, "bounded DP did not report budget pruning") &&
         Require(order.pruned_alternatives != 0, "bounded DP did not count pruned alternatives") &&
         Require(HasDiagnostic(order.diagnostics, "SB_OPT_JOIN_DP_PRUNED_BY_BUDGET"),
                 "budget pruning diagnostic missing");
}

bool DirectSemanticBarriersPreserveOrder() {
  struct Case {
    const char* name;
    opt::JoinSemanticKind kind;
    bool nullable = false;
    bool correlated = false;
    bool lateral = false;
    bool volatile_predicate = false;
    bool explicit_barrier = false;
    const char* diagnostic;
  };
  const std::vector<Case> cases = {
      {"outer", opt::JoinSemanticKind::kLeftOuter, false, false, false, false, false,
       "SB_OPT_JOIN_ORDER_PRESERVED_OUTER_JOIN"},
      {"semi", opt::JoinSemanticKind::kSemi, false, false, false, false, false,
       "SB_OPT_JOIN_ORDER_PRESERVED_SEMI_JOIN"},
      {"anti", opt::JoinSemanticKind::kAnti, false, false, false, false, false,
       "SB_OPT_JOIN_ORDER_PRESERVED_ANTI_JOIN"},
      {"nullable", opt::JoinSemanticKind::kInner, true, false, false, false, false,
       "SB_OPT_JOIN_ORDER_PRESERVED_NULLABLE_EDGE"},
      {"correlated", opt::JoinSemanticKind::kInner, false, true, false, false, false,
       "SB_OPT_JOIN_ORDER_PRESERVED_CORRELATION"},
      {"lateral", opt::JoinSemanticKind::kInner, false, false, true, false, false,
       "SB_OPT_JOIN_ORDER_PRESERVED_LATERAL"},
      {"volatile", opt::JoinSemanticKind::kInner, false, false, false, true, false,
       "SB_OPT_JOIN_ORDER_PRESERVED_VOLATILE"},
      {"barrier", opt::JoinSemanticKind::kInner, false, false, false, false, true,
       "SB_OPT_JOIN_ORDER_PRESERVED_EXPLICIT_BARRIER"},
  };

  for (const auto& entry : cases) {
    std::vector<opt::JoinRelationNode> relations = {
        {"rel.big", 10000, false},
        {"rel.small", 10, false},
    };
    std::vector<opt::JoinPredicateEdge> predicates;
    opt::JoinPredicateEdge edge;
    edge.left_relation_uuid = "rel.big";
    edge.right_relation_uuid = "rel.small";
    edge.predicate_kind = "join.equi";
    edge.semantic_kind = entry.kind;
    edge.equality = true;
    edge.nullable = entry.nullable;
    edge.outer_join_sensitive = entry.kind == opt::JoinSemanticKind::kLeftOuter ||
                                entry.kind == opt::JoinSemanticKind::kRightOuter ||
                                entry.kind == opt::JoinSemanticKind::kFullOuter;
    edge.correlated = entry.correlated;
    edge.lateral = entry.lateral;
    edge.volatile_predicate = entry.volatile_predicate;
    edge.explicit_order_barrier = entry.explicit_barrier;
    predicates.push_back(edge);
    const auto graph = opt::BuildJoinGraph(std::move(relations),
                                           std::move(predicates),
                                           edge.outer_join_sensitive,
                                           entry.kind == opt::JoinSemanticKind::kSemi ||
                                               entry.kind == opt::JoinSemanticKind::kAnti);
    const auto order = opt::EnumerateDeterministicJoinOrder(graph, 8 * 1024 * 1024);
    const std::vector<std::string> expected = {"rel.big", "rel.small"};
    if (!Require(order.ok, std::string(entry.name) + " barrier did not produce a plan") ||
        !Require(order.ordered_relation_uuids == expected,
                 std::string(entry.name) + " barrier did not preserve input order") ||
        !Require(order.semantic_order_preserved,
                 std::string(entry.name) + " barrier did not report semantic preservation") ||
        !Require(!opt::JoinReorderAllowed(graph),
                 std::string(entry.name) + " barrier still allowed reordering") ||
        !Require(HasDiagnostic(order.diagnostics, entry.diagnostic),
                 std::string(entry.name) + " exact diagnostic missing") ||
        !Require(HasDiagnostic(order.diagnostics, "SB_OPT_JOIN_ORDER_PRESERVED_BY_SEMANTIC_BARRIER"),
                 std::string(entry.name) + " general semantic barrier diagnostic missing")) {
      return false;
    }
  }
  return true;
}

bool SaturatingArithmeticIsDeterministic() {
  const auto near_max = std::numeric_limits<std::uint64_t>::max() - 4;
  const auto nested = opt::CostNestedLoopJoin(near_max, near_max, 1.0);
  if (!Require(nested.selectable, "saturated nested loop cost became unselectable") ||
      !Require(nested.row_cost == std::numeric_limits<std::uint64_t>::max(),
               "nested loop row cost did not saturate at uint64 max") ||
      !Require(nested.total_cost == std::numeric_limits<std::uint64_t>::max(),
               "nested loop total cost did not saturate at uint64 max")) {
    return false;
  }

  const auto hash = opt::CostHashJoin(near_max, near_max, 4096, 1.0);
  if (!Require(hash.selectable, "hash spill cost became unselectable") ||
      !Require(hash.total_cost == std::numeric_limits<std::uint64_t>::max(),
               "hash cost did not saturate deterministically") ||
      !Require(hash.reason == "hash_join_spill_expected",
               "hash spill diagnostic reason missing")) {
    return false;
  }

  opt::JoinPlanningInput no_hash;
  no_hash.left_cardinality = near_max;
  no_hash.right_cardinality = near_max;
  no_hash.equi_join = true;
  no_hash.reorder_safe = true;
  no_hash.memory_budget_bytes = 0;
  const auto rejected = opt::PlanLocalJoin(no_hash);
  if (!Require(rejected.ok, "local join did not fall back to selectable nested loop") ||
      !Require(rejected.selected_method == plan::PhysicalAccessKind::kJoinNestedLoop,
               "memory-rejected hash join was not deterministic") ||
      !Require(rejected.candidates[1].cost.rejection_reason == "memory_budget_insufficient",
               "hash rejection reason changed")) {
    return false;
  }

  opt::JoinPlanningInput hash_ok = no_hash;
  hash_ok.memory_budget_bytes = std::numeric_limits<std::uint64_t>::max();
  const auto selected = opt::PlanLocalJoin(hash_ok);
  return Require(selected.ok, "local join with large memory did not select") &&
         Require(selected.selected_method == plan::PhysicalAccessKind::kJoinHash,
                 "large-memory local join did not deterministically select hash");
}

plan::LogicalPlan InnerJoinLogicalPlan() {
  plan::LogicalPlan logical;
  logical.ok = true;
  logical.plan_id = "odf018.inner_join_dp";

  auto big = plan::MakeLogicalPlanNode(plan::LogicalPlanNodeKind::kDmlRead,
                                       plan::PhysicalAccessKind::kNone,
                                       "scan.big",
                                       "big_scan");
  big.required_object_uuids.push_back("rel.big");

  auto medium = plan::MakeLogicalPlanNode(plan::LogicalPlanNodeKind::kDmlRead,
                                          plan::PhysicalAccessKind::kNone,
                                          "scan.medium",
                                          "medium_scan");
  medium.required_object_uuids.push_back("rel.medium");

  auto small = plan::MakeLogicalPlanNode(plan::LogicalPlanNodeKind::kDmlRead,
                                         plan::PhysicalAccessKind::kNone,
                                         "scan.small",
                                         "small_scan");
  small.required_object_uuids.push_back("rel.small");

  auto join = plan::MakeLogicalPlanNode(plan::LogicalPlanNodeKind::kDmlRead,
                                        plan::PhysicalAccessKind::kJoinHash,
                                        "query.join",
                                        "big_medium_small_join");
  join.required_object_uuids = {"rel.big", "rel.medium", "rel.small"};
  join.required_descriptors = {"desc.join", "join.equi", "join.reorder_safe"};

  logical.nodes = {big, medium, small, join};
  return logical;
}

plan::LogicalPlan SemanticJoinLogicalPlan(const std::string& descriptor) {
  plan::LogicalPlan logical;
  logical.ok = true;
  logical.plan_id = "odf018.semantic_join";

  auto big = plan::MakeLogicalPlanNode(plan::LogicalPlanNodeKind::kDmlRead,
                                       plan::PhysicalAccessKind::kNone,
                                       "scan.big",
                                       "big_scan");
  big.required_object_uuids.push_back("rel.big");

  auto small = plan::MakeLogicalPlanNode(plan::LogicalPlanNodeKind::kDmlRead,
                                         plan::PhysicalAccessKind::kNone,
                                         "scan.small",
                                         "small_scan");
  small.required_object_uuids.push_back("rel.small");

  auto join = plan::MakeLogicalPlanNode(plan::LogicalPlanNodeKind::kDmlRead,
                                        plan::PhysicalAccessKind::kJoinHash,
                                        "query.join",
                                        "semantic_join");
  join.required_object_uuids = {"rel.big", "rel.small"};
  join.required_descriptors = {"desc.join", "join.equi", descriptor};

  logical.nodes = {big, small, join};
  return logical;
}

bool OptimizerIntegrationUsesDpPhysicalOrder() {
  const auto optimized = opt::OptimizeLogicalPlanWithStatistics(InnerJoinLogicalPlan(), CardinalityCatalog());
  return Require(optimized.ok, "optimizer did not produce inner DP plan") &&
         Require(optimized.has_physical_plan, "optimizer did not expose inner DP physical root") &&
         Require(optimized.physical_root.access_kind == plan::PhysicalAccessKind::kJoinHash,
                 "physical root did not use DP-selected hash join") &&
         Require(TreeContainsEvidence(optimized.physical_root, "join_order=rel.small,rel.medium,rel.big"),
                 "physical root tree did not expose DP-selected relation order") &&
         Require(TreeContainsEvidence(optimized.physical_root, "join_order_strategy=bounded_dp"),
                 "physical root tree did not expose bounded DP strategy") &&
         Require(TreeContainsEvidence(optimized.physical_root, "join_dp_bounded=true"),
                 "physical root tree did not record bounded enumeration") &&
         Require(TreeContainsEvidence(optimized.physical_root,
                                      "join_diagnostic=SB_OPT_JOIN_DP_BOUNDED_ENUMERATION_APPLIED"),
                 "physical root tree did not expose bounded DP diagnostic") &&
         Require(TreeContainsEvidence(optimized.physical_root,
                                      "mga_visibility_authority=engine_transaction_inventory"),
                 "MGA visibility authority evidence was not preserved") &&
         Require(opt::ValidatePhysicalPlanNode(optimized.physical_root).ok,
                 "DP physical tree failed validation");
}

bool OptimizerIntegrationPreservesSemanticOrder() {
  struct Case {
    const char* descriptor;
    const char* diagnostic;
  };
  const std::vector<Case> cases = {
      {"join.outer", "SB_OPT_JOIN_ORDER_PRESERVED_OUTER_JOIN"},
      {"join.semi", "SB_OPT_JOIN_ORDER_PRESERVED_SEMI_JOIN"},
      {"join.anti", "SB_OPT_JOIN_ORDER_PRESERVED_ANTI_JOIN"},
      {"join.barrier", "SB_OPT_JOIN_ORDER_PRESERVED_EXPLICIT_BARRIER"},
  };
  for (const auto& entry : cases) {
    const auto optimized = opt::OptimizeLogicalPlanWithStatistics(SemanticJoinLogicalPlan(entry.descriptor),
                                                                  CardinalityCatalog());
    if (!Require(optimized.ok, std::string(entry.descriptor) + " plan was not ok") ||
        !Require(optimized.has_physical_plan,
                 std::string(entry.descriptor) + " plan did not expose physical root") ||
        !Require(TreeContainsEvidence(optimized.physical_root, "join_order=rel.big,rel.small"),
                 std::string(entry.descriptor) + " did not preserve input order") ||
        !Require(TreeContainsEvidence(optimized.physical_root, "join_order_strategy=semantic_input_order"),
                 std::string(entry.descriptor) + " did not expose semantic strategy") ||
        !Require(TreeContainsEvidence(optimized.physical_root, "join_semantic_order_preserved=true"),
                 std::string(entry.descriptor) + " did not record semantic preservation") ||
        !Require(TreeContainsEvidence(optimized.physical_root, std::string("join_diagnostic=") + entry.diagnostic),
                 std::string(entry.descriptor) + " exact physical diagnostic missing")) {
      return false;
    }
  }
  return true;
}

}  // namespace

int main() {
  if (!DirectBoundedDpReordersInnerJoins()) return 1;
  if (!DirectBoundedDpRecordsBudgetPruning()) return 1;
  if (!DirectSemanticBarriersPreserveOrder()) return 1;
  if (!SaturatingArithmeticIsDeterministic()) return 1;
  if (!OptimizerIntegrationUsesDpPhysicalOrder()) return 1;
  if (!OptimizerIntegrationPreservesSemanticOrder()) return 1;
  return 0;
}

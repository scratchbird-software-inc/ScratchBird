// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "logical_plan.hpp"
#include "optimizer_contract.hpp"
#include "statistics_catalog.hpp"

#include <algorithm>
#include <iostream>
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

std::size_t CountSelectedTreeCandidates(const opt::OptimizedPlan& optimized,
                                        plan::PhysicalAccessKind access_kind) {
  return static_cast<std::size_t>(std::count_if(optimized.candidates.begin(),
                                                optimized.candidates.end(),
                                                [&](const opt::OptimizerCandidate& candidate) {
                                                  return candidate.selected_in_physical_tree &&
                                                         candidate.plan_candidate.access_kind == access_kind;
                                                }));
}

void AddRelationStats(opt::OptimizerStatisticsCatalog* catalog,
                      const std::string& relation_uuid,
                      double row_count,
                      double visible_rows,
                      double page_count,
                      double index_selectivity) {
  const auto add = [&](const std::string& name, double value) {
    catalog->Add(opt::MakeStatistic(name,
                                    "relation",
                                    relation_uuid,
                                    value,
                                    opt::StatisticSource::kCatalogExact,
                                    17,
                                    0,
                                    opt::CostConfidence::kHigh));
  };
  add("row_count", row_count);
  add("visible_row_count", visible_rows);
  add("page_count", page_count);
  add("average_row_bytes", 96.0);
  add("memory_grant_available_bytes", 4.0 * 1024.0 * 1024.0);
  add("filespace_available_pages", 8192.0);
  add("page_cache_hit_ratio", 0.80);
  add("page_cache_pressure_level", 0.25);
  add("page_family_read_latency_microseconds", 500.0);
  add("index_depth", 3.0);
  add("index_leaf_pages", 32.0);
  add("index_fragmentation_ratio", 0.01);
  add("index_distinct_keys", row_count);
  add("index_selectivity", index_selectivity);
}

opt::OptimizerStatisticsCatalog ExactCatalog() {
  opt::OptimizerStatisticsCatalog catalog;
  AddRelationStats(&catalog, "rel.customer", 1000.0, 900.0, 40.0, 0.01);
  AddRelationStats(&catalog, "rel.orders", 100.0, 96.0, 12.0, 0.05);
  catalog.Add(opt::MakeStatistic("memory_grant_available_bytes",
                                 "session",
                                 "local.default",
                                 8.0 * 1024.0 * 1024.0,
                                 opt::StatisticSource::kCatalogExact,
                                 17,
                                 0,
                                 opt::CostConfidence::kHigh));
  catalog.Add(opt::MakeStatistic("group_count",
                                 "query",
                                 "local.default",
                                 12.0,
                                 opt::StatisticSource::kCatalogExact,
                                 17,
                                 0,
                                 opt::CostConfidence::kHigh));
  catalog.Add(opt::MakeStatistic("limit_count",
                                 "query",
                                 "local.default",
                                 5.0,
                                 opt::StatisticSource::kCatalogExact,
                                 17,
                                 0,
                                 opt::CostConfidence::kHigh));
  return catalog;
}

plan::LogicalPlan FullTreeLogicalPlan() {
  plan::LogicalPlan logical;
  logical.ok = true;
  logical.plan_id = "odf017.full_physical_tree";

  auto customer = plan::MakeLogicalPlanNode(plan::LogicalPlanNodeKind::kDmlRead,
                                            plan::PhysicalAccessKind::kNone,
                                            "scan.customer",
                                            "customer_scan");
  customer.required_object_uuids.push_back("rel.customer");
  customer.required_descriptors = {"desc.customer", "predicate.scalar_eq"};

  auto orders = plan::MakeLogicalPlanNode(plan::LogicalPlanNodeKind::kDmlRead,
                                          plan::PhysicalAccessKind::kNone,
                                          "scan.orders",
                                          "orders_scan");
  orders.required_object_uuids.push_back("rel.orders");
  orders.required_descriptors = {"desc.orders", "predicate.scalar_range"};

  auto join = plan::MakeLogicalPlanNode(plan::LogicalPlanNodeKind::kDmlRead,
                                        plan::PhysicalAccessKind::kJoinHash,
                                        "query.join",
                                        "customer_orders_join");
  join.required_object_uuids = {"rel.customer", "rel.orders"};
  join.required_descriptors = {"desc.join", "join.equi", "join.reorder_safe"};

  auto aggregate = plan::MakeLogicalPlanNode(plan::LogicalPlanNodeKind::kDmlRead,
                                             plan::PhysicalAccessKind::kAggregateHash,
                                             "query.aggregate",
                                             "revenue_by_customer");
  aggregate.required_descriptors = {"desc.aggregate", "aggregate.grouping"};

  auto window = plan::MakeLogicalPlanNode(plan::LogicalPlanNodeKind::kDmlRead,
                                          plan::PhysicalAccessKind::kSortThenWindow,
                                          "query.window",
                                          "rank_by_revenue");
  window.required_descriptors = {"desc.window", "window.frame_materialization"};

  auto sort = plan::MakeLogicalPlanNode(plan::LogicalPlanNodeKind::kDmlRead,
                                        plan::PhysicalAccessKind::kSort,
                                        "query.sort",
                                        "sort_by_rank");
  sort.required_descriptors = {"desc.sort", "sort.required"};

  auto limit = plan::MakeLogicalPlanNode(plan::LogicalPlanNodeKind::kDmlRead,
                                         plan::PhysicalAccessKind::kTopN,
                                         "query.limit",
                                         "top_customers");
  limit.required_descriptors = {"desc.limit", "limit.present"};

  logical.nodes = {customer, orders, join, aggregate, window, sort, limit};
  return logical;
}

bool FullPhysicalPlanTreeIsSelected() {
  const auto optimized = opt::OptimizeLogicalPlanWithStatistics(FullTreeLogicalPlan(), ExactCatalog());
  if (!Require(optimized.ok, "optimizer did not produce an ODF-017 plan") ||
      !Require(optimized.has_physical_plan, "optimizer did not expose a physical root") ||
      !Require(optimized.selected_primary_candidate_id == "CAND-OPT-HASH",
               "flat compatibility selection was not the primary customer hash leaf: " +
                   optimized.selected_primary_candidate_id) ||
      !Require(std::count_if(optimized.candidates.begin(),
                             optimized.candidates.end(),
                             [](const opt::OptimizerCandidate& candidate) {
                               return candidate.selected;
                             }) == 1,
               "flat compatibility surface selected more than one global candidate")) {
    return false;
  }

  const auto selected_tree_count = std::count_if(optimized.candidates.begin(),
                                                 optimized.candidates.end(),
                                                 [](const opt::OptimizerCandidate& candidate) {
                                                   return candidate.selected_in_physical_tree;
                                                 });
  if (!Require(selected_tree_count == 7,
               "physical tree did not independently select two scans plus join and four upper operators") ||
      !Require(CountSelectedTreeCandidates(optimized, plan::PhysicalAccessKind::kScalarHashLookup) == 1,
               "customer hash lookup was not selected as one base scan") ||
      !Require(CountSelectedTreeCandidates(optimized, plan::PhysicalAccessKind::kScalarBtreeRange) == 1,
               "orders range scan was not selected as one base scan") ||
      !Require(CountSelectedTreeCandidates(optimized, plan::PhysicalAccessKind::kJoinHash) == 1,
               "hash join was not selected in the physical tree") ||
      !Require(CountSelectedTreeCandidates(optimized, plan::PhysicalAccessKind::kAggregateHash) == 1,
               "hash aggregate was not selected in the physical tree") ||
      !Require(CountSelectedTreeCandidates(optimized, plan::PhysicalAccessKind::kSortThenWindow) == 1,
               "window operator was not selected in the physical tree") ||
      !Require(CountSelectedTreeCandidates(optimized, plan::PhysicalAccessKind::kSort) == 1,
               "sort operator was not selected in the physical tree") ||
      !Require(CountSelectedTreeCandidates(optimized, plan::PhysicalAccessKind::kTopN) == 1,
               "limit/top-N operator was not selected in the physical tree")) {
    return false;
  }

  const auto& limit = optimized.physical_root;
  const auto& sort = limit.children.front();
  const auto& window = sort.children.front();
  const auto& aggregate = window.children.front();
  const auto& join = aggregate.children.front();
  const auto& orders = join.children.front();
  const auto& customer = join.children.back();

  return Require(limit.access_kind == plan::PhysicalAccessKind::kTopN, "root was not limit/top-N") &&
         Require(limit.executor_capability_id == "limit_offset", "limit executor capability mismatch") &&
         Require(limit.descriptor_digest == "desc.limit", "limit descriptor digest mismatch") &&
         Require(limit.materializes && limit.preserves_order && limit.preserves_visibility,
                 "limit flags did not preserve materialized order and visibility") &&
         Require(sort.access_kind == plan::PhysicalAccessKind::kSort, "sort node missing below limit") &&
         Require(sort.executor_capability_id == "sort", "sort executor capability mismatch") &&
         Require(sort.materializes && sort.preserves_order && sort.preserves_visibility,
                 "sort flags did not preserve materialized order and visibility") &&
         Require(window.access_kind == plan::PhysicalAccessKind::kSortThenWindow, "window node missing below sort") &&
         Require(window.executor_capability_id == "window", "window executor capability mismatch") &&
         Require(window.materializes && window.preserves_order && window.preserves_visibility,
                 "window flags did not preserve materialized order and visibility") &&
         Require(aggregate.access_kind == plan::PhysicalAccessKind::kAggregateHash,
                 "aggregate node missing below window") &&
         Require(aggregate.executor_capability_id == "hash_aggregate",
                 "aggregate executor capability mismatch") &&
         Require(aggregate.materializes && !aggregate.storage_backed && aggregate.preserves_visibility,
                 "aggregate flags did not preserve materialized visibility") &&
         Require(join.access_kind == plan::PhysicalAccessKind::kJoinHash, "join node was not hash join") &&
         Require(join.executor_capability_id == "hash_join", "join executor capability mismatch") &&
         Require(join.children.size() == 2, "join did not carry two selected base leaves") &&
         Require(join.materializes && !join.storage_backed && join.preserves_visibility,
                 "join flags did not preserve materialized visibility") &&
         Require(HasEvidence(join, "join_order=rel.orders,rel.customer"),
                 "join order evidence did not record selected cardinality order") &&
         Require(HasEvidence(join, "join_method=join_hash"), "join method evidence missing") &&
         Require(orders.access_kind == plan::PhysicalAccessKind::kScalarBtreeRange,
                 "orders base leaf did not select range access") &&
         Require(orders.executor_capability_id == "index_range_scan",
                 "orders executor capability mismatch") &&
         Require(orders.descriptor_digest == "desc.orders", "orders descriptor digest mismatch") &&
         Require(orders.storage_backed && orders.preserves_order && orders.preserves_visibility,
                 "orders leaf flags did not preserve ordered storage visibility") &&
         Require(HasEvidence(orders, "base_relation_uuid=rel.orders"), "orders relation evidence missing") &&
         Require(customer.access_kind == plan::PhysicalAccessKind::kScalarHashLookup,
                 "customer base leaf did not select hash lookup") &&
         Require(customer.executor_capability_id == "hash_index_lookup",
                 "customer executor capability mismatch") &&
         Require(customer.descriptor_digest == "desc.customer", "customer descriptor digest mismatch") &&
         Require(customer.storage_backed && !customer.materializes && customer.preserves_visibility,
                 "customer leaf flags did not preserve storage visibility") &&
         Require(HasEvidence(customer, "base_relation_uuid=rel.customer"), "customer relation evidence missing") &&
         Require(TreeContainsEvidence(limit, "mga_visibility_authority=engine_transaction_inventory"),
                 "runtime payload did not retain MGA visibility authority evidence") &&
         Require(TreeContainsEvidence(limit, "selected_candidate_id=CAND-OPT-HASH"),
                 "physical tree did not retain primary flat candidate evidence") &&
         Require(opt::ValidatePhysicalPlanNode(limit).ok, "physical tree validation failed") &&
         Require(opt::SerializeOptimizedPlanToJson(optimized).find("\"physical_root\"") != std::string::npos,
                 "optimized plan JSON did not serialize physical root evidence");
}

}  // namespace

int main() {
  if (!FullPhysicalPlanTreeIsSelected()) return 1;
  return 0;
}

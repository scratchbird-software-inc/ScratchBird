// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "access_path.hpp"
#include "access_path_full.hpp"
#include "logical_plan.hpp"
#include "optimizer_contract.hpp"
#include "optimizer_request.hpp"
#include "optimizer_statistics_full.hpp"
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

bool HasKind(const std::vector<opt::PlanCandidate>& candidates,
             plan::PhysicalAccessKind kind) {
  return std::any_of(candidates.begin(), candidates.end(), [&](const opt::PlanCandidate& candidate) {
    return candidate.access_kind == kind;
  });
}

bool HasOptimizedKind(const opt::OptimizedPlan& optimized,
                      plan::PhysicalAccessKind kind) {
  return std::any_of(optimized.candidates.begin(), optimized.candidates.end(), [&](const opt::OptimizerCandidate& candidate) {
    return candidate.plan_candidate.access_kind == kind;
  });
}

opt::OptimizerStatsIdentity FreshIdentity(const std::string& object_uuid,
                                          const std::string& statistic_uuid) {
  opt::OptimizerStatsIdentity identity;
  identity.object_uuid = object_uuid;
  identity.statistic_uuid = statistic_uuid;
  identity.stats_epoch = 7;
  identity.catalog_epoch = 3;
  identity.transaction_visibility_epoch = 7;
  identity.freshness = opt::OptimizerStatsFreshnessState::kFresh;
  identity.source = opt::StatisticSource::kCatalogExact;
  identity.confidence = opt::CostConfidence::kHigh;
  return identity;
}

opt::TableCardinalityStats TableStats(const std::string& relation_uuid) {
  opt::TableCardinalityStats stats;
  stats.identity = FreshIdentity(relation_uuid, relation_uuid + ":table");
  stats.row_count = 10000;
  stats.visible_row_count = 9600;
  stats.page_count = 512;
  stats.average_row_bytes = 96;
  return stats;
}

opt::IndexStats IndexStats(const std::string& relation_uuid,
                           const std::string& index_uuid,
                           const std::string& family,
                           bool unique,
                           bool covering) {
  opt::IndexStats stats;
  stats.identity = FreshIdentity(index_uuid, index_uuid + ":index");
  stats.index_uuid = index_uuid;
  stats.relation_uuid = relation_uuid;
  stats.index_family = family;
  stats.unique = unique;
  stats.covering = covering;
  stats.height = 3;
  stats.leaf_pages = 48;
  stats.distinct_keys = unique ? 10000 : 5000;
  stats.clustering_factor = 0.72;
  stats.fragmentation_ratio = 0.05;
  stats.visibility_coverage = 1.0;
  stats.predicate_coverage = 1.0;
  stats.route_benchmark_clean = true;
  if (family == "btree" || family == "unique_btree" ||
      family == "covering") {
    stats.equality_lookup_supported = true;
    stats.ordered_range_supported = true;
  } else if (family == "hash") {
    stats.equality_lookup_supported = true;
  } else if (family == "brin_zone") {
    stats.negative_prune_supported = true;
  } else {
    stats.candidate_set_producer = true;
  }
  return stats;
}

opt::OptimizerStatisticsCatalog ExactLocalCatalog(const std::string& relation_uuid,
                                                  bool include_memory_grant = true) {
  opt::OptimizerStatisticsCatalog catalog;
  const auto add = [&](const std::string& name, double value) {
    catalog.Add(opt::MakeStatistic(name, "relation", relation_uuid, value,
                                   opt::StatisticSource::kCatalogExact, 7, 0,
                                   opt::CostConfidence::kHigh));
  };
  add("row_count", 10000.0);
  add("visible_row_count", 9600.0);
  add("page_count", 512.0);
  add("average_row_bytes", 96.0);
  if (include_memory_grant) {
    add("memory_grant_available_bytes", 1048576.0);
  }
  add("filespace_available_pages", 4096.0);
  add("page_cache_hit_ratio", 0.80);
  add("page_cache_pressure_level", 1.0);
  add("page_family_read_latency_microseconds", 650.0);
  add("index_depth", 3.0);
  add("index_leaf_pages", 48.0);
  add("index_fragmentation_ratio", 0.05);
  add("index_distinct_keys", 10000.0);
  add("index_visibility_coverage", 1.0);
  add("index_selectivity", 0.001);
  return catalog;
}

bool LogicalPlannerStaysShapeOnly() {
  plan::QueryShapeEvidence evidence;
  evidence.shape = plan::QueryShapeKind::kPointLookup;
  evidence.has_usable_index = false;
  auto logical = plan::BuildQueryShapePlan(evidence);
  return Require(logical.ok, "shape plan not ok") &&
         Require(logical.nodes.size() == 1, "shape plan node count mismatch") &&
         Require(logical.nodes.front().access_kind == plan::PhysicalAccessKind::kNone,
                 "point lookup shape was converted to a final access path") &&
         Require(logical.diagnostics.empty(), "shape planner emitted deterministic fallback diagnostic");
}

bool LocalEnumerationIgnoresPreselectedAccessKind() {
  auto logical = plan::BuildQueryShapePlan({plan::QueryShapeKind::kPointLookup});
  logical.nodes.front().required_object_uuids.push_back("rel.local");
  const auto catalog = ExactLocalCatalog("rel.local");
  const auto candidates = opt::GenerateLocalAccessPathCandidates(logical.nodes.front(), catalog);
  return Require(HasKind(candidates, plan::PhysicalAccessKind::kTableScan), "local table scan candidate missing") &&
         Require(HasKind(candidates, plan::PhysicalAccessKind::kScalarBtreeLookup), "local btree candidate missing without preselected access kind") &&
         Require(HasKind(candidates, plan::PhysicalAccessKind::kScalarHashLookup), "local hash candidate missing without preselected access kind") &&
         Require(HasKind(candidates, plan::PhysicalAccessKind::kCoveringIndexScan), "local covering candidate missing without preselected access kind");
}

bool FullCatalogBackedEnumerationCoversRequiredFamilies() {
  const std::string relation_uuid = "rel.full";
  opt::AccessPathPlanningRequest equality;
  equality.relation_uuid = relation_uuid;
  equality.predicate_kind = "scalar_eq";
  equality.descriptor_digest = "desc:int64";
  equality.projected_column_uuids = {"col.full"};
  equality.visibility_proven = true;
  equality.grants_proven = true;
  equality.index_visibility_native = true;
  equality.table_stats = TableStats(relation_uuid);
  equality.candidate_indexes = {
      IndexStats(relation_uuid, "idx.full.btree", "btree", true, true),
      IndexStats(relation_uuid, "idx.full.hash", "hash", false, false),
  };
  const auto equality_candidates = opt::GenerateFullAccessPathCandidates(equality);

  opt::AccessPathPlanningRequest row_uuid = equality;
  row_uuid.predicate_kind = "row_uuid_eq";
  const auto row_uuid_candidates = opt::GenerateFullAccessPathCandidates(row_uuid);

  opt::AccessPathPlanningRequest range = equality;
  range.predicate_kind = "scalar_range";
  range.candidate_indexes = {IndexStats(relation_uuid, "idx.full.range", "btree", false, false)};
  const auto range_candidates = opt::GenerateFullAccessPathCandidates(range);

  opt::AccessPathPlanningRequest specialized = equality;
  specialized.predicate_kind = "full_text";
  specialized.candidate_indexes = {IndexStats(relation_uuid, "idx.full.text", "full_text", false, false)};
  const auto specialized_candidates = opt::GenerateFullAccessPathCandidates(specialized);

  auto logical = plan::BuildQueryShapePlan({plan::QueryShapeKind::kPointLookup});
  const auto optimized = opt::OptimizeLogicalPlanWithAccessPathRequest(logical, equality);

  return Require(HasKind(equality_candidates, plan::PhysicalAccessKind::kTableScan), "full table scan candidate missing") &&
         Require(HasKind(row_uuid_candidates, plan::PhysicalAccessKind::kRowUuidLookup), "full row UUID candidate missing") &&
         Require(HasKind(equality_candidates, plan::PhysicalAccessKind::kScalarBtreeLookup), "full btree candidate missing") &&
         Require(HasKind(equality_candidates, plan::PhysicalAccessKind::kScalarHashLookup), "full hash candidate missing") &&
         Require(HasKind(range_candidates, plan::PhysicalAccessKind::kScalarBtreeRange), "full range candidate missing") &&
         Require(HasKind(equality_candidates, plan::PhysicalAccessKind::kCoveringIndexScan), "full covering candidate missing") &&
         Require(HasKind(equality_candidates, plan::PhysicalAccessKind::kBitmapSummaryScan), "full bitmap summary candidate missing") &&
         Require(HasKind(specialized_candidates, plan::PhysicalAccessKind::kFullTextProbe), "full specialized candidate missing") &&
         Require(optimized.ok, "catalog-backed optimizer plan not ok") &&
         Require(HasOptimizedKind(optimized, plan::PhysicalAccessKind::kScalarBtreeLookup), "main optimizer did not expose btree candidate") &&
         Require(HasOptimizedKind(optimized, plan::PhysicalAccessKind::kScalarHashLookup), "main optimizer did not expose hash candidate");
}

bool BoundRequestUsesCatalogBackedMainPathAndBuildsPhysicalPlan() {
  const std::string relation_uuid = "rel.bound";
  auto logical = plan::BuildQueryShapePlan({plan::QueryShapeKind::kPointLookup});
  logical.nodes.front().required_object_uuids.push_back(relation_uuid);
  logical.nodes.front().required_descriptors.push_back("projection.covered");

  opt::AccessPathPlanningRequest access_request;
  access_request.relation_uuid = relation_uuid;
  access_request.predicate_kind = "scalar_eq";
  access_request.descriptor_digest = "desc:int64";
  access_request.projected_column_uuids = {"col.bound"};
  access_request.visibility_proven = true;
  access_request.grants_proven = true;
  access_request.index_visibility_native = true;
  access_request.table_stats = TableStats(relation_uuid);
  access_request.candidate_indexes = {
      IndexStats(relation_uuid, "idx.bound.btree", "btree", true, true),
      IndexStats(relation_uuid, "idx.bound.hash", "hash", false, false),
  };

  opt::BoundOptimizerRequest request;
  request.context.request_uuid = "request.bound";
  request.context.operation_id = "dml.select_rows";
  request.context.sblr_digest = "sblr.bound";
  request.context.descriptor_set_digest = "desc:int64";
  request.context.statistics_snapshot_id = "stats.bound";
  request.context.metric_snapshot_id = "metrics.bound";
  request.context.executor_capability_set_id = "executor.bound";
  request.context.catalog_epoch = 3;
  request.context.security_epoch = 4;
  request.context.policy_epoch = 5;
  request.context.security_context_present = true;
  request.context.transaction_context_present = true;
  request.logical_plan = logical;
  request.statistics = ExactLocalCatalog(relation_uuid);
  request.catalog_access_path_request = access_request;

  const auto result = opt::OptimizeBoundRequest(request);
  if (!Require(result.ok, "bound optimizer request did not produce an executable plan") ||
      !Require(result.optimizer_profile == "catalog_backed_access_path_v1",
               "bound optimizer request did not use catalog-backed main path") ||
      !Require(HasKind(result.candidates, plan::PhysicalAccessKind::kScalarBtreeLookup),
               "bound optimizer request did not expose btree candidate") ||
      !Require(HasKind(result.candidates, plan::PhysicalAccessKind::kScalarHashLookup),
               "bound optimizer request did not expose hash candidate")) {
    return false;
  }

  const auto selected = std::find_if(result.candidates.begin(), result.candidates.end(), [](const opt::PlanCandidate& candidate) {
    return candidate.selected;
  });
  if (!Require(selected != result.candidates.end(), "bound optimizer request did not mark a selected candidate")) {
    return false;
  }
  const auto physical = opt::PhysicalPlanNodeFromCandidate(*selected,
                                                           opt::RequiredExecutorCapabilityForAccessKind(selected->access_kind),
                                                           access_request.descriptor_digest);
  const auto validation = opt::ValidatePhysicalPlanNode(physical);
  return Require(validation.ok, "selected bound candidate did not validate as a physical plan node");
}

bool BenchmarkCleanPolicyDefaultStatsAreDiagnosed() {
  auto logical = plan::BuildQueryShapePlan({plan::QueryShapeKind::kPointLookup});
  const auto fallback_plan = opt::OptimizeLogicalPlan(logical);
  const auto fallback_status = opt::ValidateBenchmarkCleanOptimizedPlan(fallback_plan);
  if (!Require(!fallback_status.ok, "benchmark-clean accepted local.default policy stats")) {
    return false;
  }

  logical.nodes.front().required_object_uuids.push_back("rel.memory-default");
  auto memory_default_catalog = ExactLocalCatalog("rel.memory-default", false);
  memory_default_catalog.Add(opt::MakeStatistic("memory_grant_available_bytes",
                                                "session",
                                                "local.default",
                                                1048576.0,
                                                opt::StatisticSource::kPolicyDefault,
                                                7,
                                                0,
                                                opt::CostConfidence::kLow));
  const auto memory_default_plan = opt::OptimizeLogicalPlanWithStatistics(logical, memory_default_catalog);
  const auto memory_default_status = opt::ValidateBenchmarkCleanOptimizedPlan(memory_default_plan);
  if (!Require(!memory_default_status.ok,
               "benchmark-clean accepted local.default policy memory budget")) {
    return false;
  }

  logical.nodes.front().required_object_uuids.clear();
  logical.nodes.front().required_object_uuids.push_back("rel.clean");
  const auto clean_plan = opt::OptimizeLogicalPlanWithStatistics(logical, ExactLocalCatalog("rel.clean"));
  const auto clean_status = opt::ValidateBenchmarkCleanOptimizedPlan(clean_plan);
  return Require(clean_plan.ok, "exact-stats clean plan not ok") &&
         Require(!clean_status.ok, "benchmark-clean accepted statistics-only exact stats") &&
         Require(clean_status.diagnostic_code ==
                     "SB_ORH_CATALOG_BACKED_PUBLIC_SQL_ROUTE.STATISTICS_ONLY_NOT_BENCHMARK_CLEAN",
                 "statistics-only exact stats refusal diagnostic drifted");
}

bool ExpandedStatsCatalogFeedsCostingAndStaleDiagnostics() {
  const std::string relation_uuid = "rel.stats";
  opt::OptimizerStatisticsStore store;
  store.UpsertTable(TableStats(relation_uuid));

  opt::ColumnStats column;
  column.identity = FreshIdentity(relation_uuid, "stat.column");
  column.column_uuid = "col.stats";
  column.descriptor_digest = "desc:int64";
  column.null_count = 100;
  column.distinct_count = 9000;
  column.null_fraction = 0.01;
  column.correlation = 0.64;
  column.average_width_bytes = 8;
  store.UpsertColumn(column);

  opt::HistogramStats histogram;
  histogram.identity = FreshIdentity(relation_uuid, "stat.histogram");
  histogram.column_uuid = "col.stats";
  histogram.buckets.push_back({"1", "100", 0.30, 3000});
  histogram.buckets.push_back({"101", "1000", 0.70, 7000});
  store.UpsertHistogram(histogram);

  opt::MostCommonValueStats mcv;
  mcv.identity = FreshIdentity(relation_uuid, "stat.mcv");
  mcv.column_uuid = "col.stats";
  mcv.value_encoded = "42";
  mcv.frequency = 0.05;
  store.UpsertMcv(mcv);

  store.UpsertIndex(IndexStats(relation_uuid, "idx.stats", "btree", true, true));
  auto snapshot = store.Snapshot("stats.snapshot");
  const auto statuses = opt::ValidateOptimizerStatsSnapshot(snapshot);
  if (!Require(std::all_of(statuses.begin(), statuses.end(), [](const opt::StatisticsContractStatus& status) {
        return status.ok;
      }), "fresh expanded stats snapshot failed validation")) {
    return false;
  }

  const auto legacy = store.ToLegacyCatalog();
  if (!Require(legacy.Find("column_ndv", "col.stats").has_value(), "NDV not projected to costing catalog") ||
      !Require(legacy.Find("column_null_fraction", "col.stats").has_value(), "null fraction not projected to costing catalog") ||
      !Require(legacy.Find("histogram_bucket_count", "col.stats").has_value(), "histogram not projected to costing catalog") ||
      !Require(legacy.Find("mcv_frequency", "col.stats").has_value(), "MCV not projected to costing catalog") ||
      !Require(legacy.Find("index_depth", "idx.stats").has_value(), "index depth not projected to costing catalog") ||
      !Require(legacy.Find("index_leaf_pages", "idx.stats").has_value(), "index leaf pages not projected to costing catalog") ||
      !Require(legacy.Find("index_fragmentation_ratio", "idx.stats").has_value(), "index fragmentation not projected to costing catalog") ||
      !Require(legacy.Find("index_visibility_coverage", "idx.stats").has_value(), "index coverage not projected to costing catalog")) {
    return false;
  }

  store.MarkStaleByObject(relation_uuid, 9);
  const auto stale_statuses = opt::ValidateOptimizerStatsSnapshot(store.Snapshot("stats.stale"));
  return Require(std::any_of(stale_statuses.begin(), stale_statuses.end(), [](const opt::StatisticsContractStatus& status) {
           return !status.ok && status.diagnostic_code == "SB_OPT_STATS_NOT_USABLE";
         }), "stale stats did not produce stale-proof diagnostic");
}

}  // namespace

int main() {
  if (!LogicalPlannerStaysShapeOnly()) return 1;
  if (!LocalEnumerationIgnoresPreselectedAccessKind()) return 1;
  if (!FullCatalogBackedEnumerationCoversRequiredFamilies()) return 1;
  if (!BoundRequestUsesCatalogBackedMainPathAndBuildsPhysicalPlan()) return 1;
  if (!BenchmarkCleanPolicyDefaultStatsAreDiagnosed()) return 1;
  if (!ExpandedStatsCatalogFeedsCostingAndStaleDiagnostics()) return 1;
  return 0;
}

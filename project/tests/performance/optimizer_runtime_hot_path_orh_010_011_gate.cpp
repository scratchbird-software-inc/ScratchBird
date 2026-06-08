// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "access_path_full.hpp"
#include "logical_plan.hpp"
#include "optimizer_contract.hpp"
#include "optimizer_request.hpp"
#include "optimizer_statistics_full.hpp"
#include "runtime_consumption_evidence.hpp"
#include "statistics_catalog.hpp"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace opt = scratchbird::engine::optimizer;
namespace plan = scratchbird::engine::planner;

namespace {

constexpr const char* kCatalogBackedProfile = "catalog_backed_access_path_v1";
constexpr const char* kStatisticsOnlyNotBenchmarkClean =
    "SB_ORH_CATALOG_BACKED_PUBLIC_SQL_ROUTE.STATISTICS_ONLY_NOT_BENCHMARK_CLEAN";
constexpr const char* kCatalogFactsRequired =
    "SB_ORH_CATALOG_BACKED_PUBLIC_SQL_ROUTE.CATALOG_FACTS_REQUIRED";

void Require(bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << "ORH-010/011 gate failure: " << message << '\n';
    std::exit(1);
  }
}

bool Contains(const std::vector<std::string>& values,
              const std::string& expected) {
  return std::find(values.begin(), values.end(), expected) != values.end();
}

bool HasKind(const std::vector<opt::PlanCandidate>& candidates,
             plan::PhysicalAccessKind kind) {
  return std::any_of(candidates.begin(), candidates.end(), [&](const auto& candidate) {
    return candidate.access_kind == kind;
  });
}

bool HasSelectedKind(const opt::OptimizedPlan& optimized,
                     plan::PhysicalAccessKind kind) {
  return std::any_of(optimized.candidates.begin(), optimized.candidates.end(), [&](const auto& candidate) {
    return candidate.selected && candidate.plan_candidate.access_kind == kind;
  });
}

opt::OptimizerStatsIdentity FreshIdentity(const std::string& object_uuid,
                                          const std::string& statistic_uuid) {
  opt::OptimizerStatsIdentity identity;
  identity.object_uuid = object_uuid;
  identity.statistic_uuid = statistic_uuid;
  identity.stats_epoch = 17;
  identity.catalog_epoch = 23;
  identity.transaction_visibility_epoch = 29;
  identity.freshness = opt::OptimizerStatsFreshnessState::kFresh;
  identity.source = opt::StatisticSource::kCatalogExact;
  identity.confidence = opt::CostConfidence::kHigh;
  return identity;
}

opt::TableCardinalityStats TableStats(const std::string& relation_uuid) {
  opt::TableCardinalityStats stats;
  stats.identity = FreshIdentity(relation_uuid, relation_uuid + ":table");
  stats.row_count = 20000;
  stats.visible_row_count = 19500;
  stats.page_count = 768;
  stats.average_row_bytes = 112;
  return stats;
}

opt::IndexStats BtreeIndex(const std::string& relation_uuid,
                           const std::string& index_uuid) {
  opt::IndexStats stats;
  stats.identity = FreshIdentity(index_uuid, index_uuid + ":index");
  stats.index_uuid = index_uuid;
  stats.relation_uuid = relation_uuid;
  stats.index_family = "btree";
  stats.unique = true;
  stats.covering = true;
  stats.height = 3;
  stats.leaf_pages = 64;
  stats.distinct_keys = 20000;
  stats.clustering_factor = 0.84;
  stats.fragmentation_ratio = 0.02;
  stats.visibility_coverage = 1.0;
  stats.predicate_coverage = 1.0;
  stats.key_column_uuids = {"col.customer_id"};
  stats.covered_column_uuids = {"col.customer_id", "col.name"};
  return stats;
}

opt::OptimizerStatisticsCatalog ExactStatisticsCatalog(
    const std::string& relation_uuid) {
  opt::OptimizerStatisticsCatalog catalog;
  const auto add_relation = [&](const std::string& name, double value) {
    catalog.Add(opt::MakeStatistic(name,
                                   "relation",
                                   relation_uuid,
                                   value,
                                   opt::StatisticSource::kCatalogExact,
                                   17,
                                   0,
                                   opt::CostConfidence::kHigh));
  };
  add_relation("row_count", 20000.0);
  add_relation("visible_row_count", 19500.0);
  add_relation("page_count", 768.0);
  add_relation("average_row_bytes", 112.0);
  add_relation("memory_grant_available_bytes", 1048576.0);
  add_relation("filespace_available_pages", 4096.0);
  add_relation("page_cache_hit_ratio", 0.86);
  add_relation("page_cache_pressure_level", 1.0);
  add_relation("page_family_read_latency_microseconds", 500.0);
  add_relation("index_depth", 3.0);
  add_relation("index_leaf_pages", 64.0);
  add_relation("index_fragmentation_ratio", 0.02);
  add_relation("index_distinct_keys", 20000.0);
  add_relation("index_visibility_coverage", 1.0);
  return catalog;
}

plan::LogicalPlan PointLookupPlan(const std::string& relation_uuid) {
  auto logical = plan::BuildQueryShapePlan({plan::QueryShapeKind::kPointLookup});
  logical.nodes.front().required_object_uuids.push_back(relation_uuid);
  logical.nodes.front().required_descriptors.push_back("projection.covered");
  return logical;
}

opt::AccessPathPlanningRequest CatalogAccessRequest(
    const std::string& relation_uuid) {
  opt::AccessPathPlanningRequest request;
  request.relation_uuid = relation_uuid;
  request.predicate_kind = "scalar_eq";
  request.predicate_text = "col.customer_id = ?";
  request.descriptor_digest = "desc:customer_lookup";
  request.collation_identity = "binary";
  request.projected_column_uuids = {"col.customer_id", "col.name"};
  request.visibility_proven = true;
  request.grants_proven = true;
  request.base_row_mga_recheck_planned = true;
  request.base_row_security_recheck_planned = true;
  request.index_visibility_native = true;
  request.table_stats = TableStats(relation_uuid);
  request.candidate_indexes = {BtreeIndex(relation_uuid, "idx.customer_id")};
  return request;
}

opt::BoundOptimizerRequest PublicSqlRequest(const std::string& relation_uuid) {
  opt::BoundOptimizerRequest request;
  request.context.request_uuid = "orh010.request";
  request.context.operation_id = "public_sql.select.point_lookup";
  request.context.sblr_digest = "sblr:public-sql-point-lookup";
  request.context.descriptor_set_digest = "desc:customer_lookup";
  request.context.statistics_snapshot_id = "stats:orh010";
  request.context.metric_snapshot_id = "metrics:orh010";
  request.context.executor_capability_set_id = "executor:orh010";
  request.context.catalog_epoch = 23;
  request.context.security_epoch = 31;
  request.context.policy_epoch = 37;
  request.context.security_context_present = true;
  request.context.transaction_context_present = true;
  request.logical_plan = PointLookupPlan(relation_uuid);
  request.statistics = ExactStatisticsCatalog(relation_uuid);
  return request;
}

opt::RuntimeOptimizedPathEvidence RouteEvidenceFromResult(
    const opt::BoundOptimizerRequest& request,
    const opt::BoundOptimizerResult& result) {
  auto evidence = opt::MakeSelectionOnlyRuntimeEvidence(
      result.optimizer_profile,
      "embedded",
      "SB_ORH_CATALOG_BACKED_PUBLIC_SQL_ROUTE.CONSUMED",
      "optimizer selection awaiting executor consumption");
  evidence.transaction_snapshot_class = "engine.mga.snapshot";
  evidence.catalog_epoch = request.context.catalog_epoch;
  evidence.security_epoch = request.context.security_epoch;
  evidence.redaction_epoch = request.context.policy_epoch;
  evidence.provider_generation = 41;
  evidence.result_contract_hash = "hash:orh010-public-sql-result-contract";
  return opt::MarkRuntimeEvidenceConsumed(
      std::move(evidence),
      "engine.executor.public_sql.catalog_backed_access_path");
}

void CatalogBackedPublicSqlRouteIsBenchmarkCleanEligible() {
  const std::string relation_uuid = "rel.orh010.catalog";
  auto request = PublicSqlRequest(relation_uuid);
  request.catalog_access_path_request = CatalogAccessRequest(relation_uuid);

  const auto result = opt::OptimizeBoundRequest(request);
  Require(result.ok, "catalog-backed public SQL request was rejected");
  Require(result.optimizer_profile == kCatalogBackedProfile,
          "catalog-backed public SQL route did not select catalog profile");
  Require(!Contains(result.diagnostics, kStatisticsOnlyNotBenchmarkClean),
          "catalog-backed public SQL route emitted statistics-only blocker");
  Require(HasKind(result.candidates, plan::PhysicalAccessKind::kScalarBtreeLookup),
          "catalog-backed public SQL route did not enumerate btree lookup");

  const auto optimized = opt::OptimizeLogicalPlanWithAccessPathRequest(
      request.logical_plan,
      *request.catalog_access_path_request);
  const auto benchmark_clean = opt::ValidateBenchmarkCleanOptimizedPlan(optimized);
  Require(optimized.ok, "catalog-backed optimized plan was not ok");
  Require(optimized.optimizer_profile == kCatalogBackedProfile,
          "optimized plan did not preserve catalog-backed profile");
  Require(HasSelectedKind(optimized, plan::PhysicalAccessKind::kScalarBtreeLookup),
          "catalog-backed optimized plan did not select btree lookup");
  Require(benchmark_clean.ok,
          "catalog-backed optimized plan was not benchmark-clean eligible: " +
              benchmark_clean.diagnostic_code);

  const auto runtime_evidence = RouteEvidenceFromResult(request, result);
  const auto runtime_validation =
      opt::ValidateRuntimeOptimizedPathEvidence(runtime_evidence);
  Require(runtime_validation.ok,
          "catalog-backed public SQL runtime evidence rejected");
  Require(runtime_validation.state == opt::RuntimeConsumptionState::kRuntimeConsumed,
          "catalog-backed public SQL route did not record runtime consumption");
  Require(runtime_evidence.selected_path == kCatalogBackedProfile,
          "runtime evidence did not prove catalog-backed selected path");
  Require(runtime_evidence.catalog_epoch == request.context.catalog_epoch,
          "runtime evidence missing catalog_epoch");
  Require(runtime_evidence.security_epoch == request.context.security_epoch,
          "runtime evidence missing security_epoch");
  Require(runtime_evidence.redaction_epoch == request.context.policy_epoch,
          "runtime evidence missing redaction_epoch");
  Require(!runtime_evidence.result_contract_hash.empty(),
          "runtime evidence missing result_contract_hash");

  const opt::RouteCompletionClaim claim{
      .route_kind = "embedded",
      .benchmark_clean = true,
      .live_route = true,
      .mark_complete = true,
  };
  const auto guard = opt::EvaluateRouteCompletionClaim(claim, {runtime_evidence});
  Require(guard.can_mark_complete,
          "catalog-backed public SQL route with runtime evidence was blocked");
}

void StatisticsOnlyOptimizerIsQuarantinedFromBenchmarkCleanClaims() {
  const std::string relation_uuid = "rel.orh011.stats_only";
  const auto logical = PointLookupPlan(relation_uuid);
  const auto statistics = ExactStatisticsCatalog(relation_uuid);

  const auto optimized = opt::OptimizeLogicalPlanWithStatistics(logical, statistics);
  Require(optimized.ok, "statistics-only optimizer lost smoke compatibility");
  Require(Contains(optimized.diagnostics, kStatisticsOnlyNotBenchmarkClean),
          "statistics-only optimizer missing ORH not-benchmark-clean diagnostic");
  const auto benchmark_clean = opt::ValidateBenchmarkCleanOptimizedPlan(optimized);
  Require(!benchmark_clean.ok,
          "statistics-only optimized plan was allowed benchmark-clean status");
  Require(benchmark_clean.diagnostic_code == kStatisticsOnlyNotBenchmarkClean,
          "statistics-only benchmark-clean refusal diagnostic drifted");

  auto request = PublicSqlRequest(relation_uuid);
  const auto result = opt::OptimizeBoundRequest(request);
  Require(result.ok, "statistics-only public SQL route lost smoke compatibility");
  Require(result.optimizer_profile != kCatalogBackedProfile,
          "statistics-only route falsely reported catalog-backed profile");
  Require(Contains(result.diagnostics, kStatisticsOnlyNotBenchmarkClean),
          "statistics-only public SQL route missing exact blocker diagnostic");
}

void CatalogBackedRouteWithoutCatalogFactsIsNotBenchmarkClean() {
  const std::string relation_uuid = "rel.orh010.missing_catalog_facts";
  auto access_request = CatalogAccessRequest(relation_uuid);
  access_request.table_stats.reset();

  const auto optimized = opt::OptimizeLogicalPlanWithAccessPathRequest(
      PointLookupPlan(relation_uuid),
      access_request);
  Require(optimized.ok, "missing table stats should preserve smoke planning");
  Require(Contains(optimized.diagnostics, kCatalogFactsRequired),
          "missing table stats did not emit catalog-facts diagnostic");
  const auto benchmark_clean = opt::ValidateBenchmarkCleanOptimizedPlan(optimized);
  Require(!benchmark_clean.ok,
          "catalog-backed route without catalog facts was benchmark-clean");
  Require(benchmark_clean.diagnostic_code == kCatalogFactsRequired,
          "missing catalog facts refusal diagnostic drifted");
}

}  // namespace

int main() {
  CatalogBackedPublicSqlRouteIsBenchmarkCleanEligible();
  StatisticsOnlyOptimizerIsQuarantinedFromBenchmarkCleanClaims();
  CatalogBackedRouteWithoutCatalogFactsIsNotBenchmarkClean();
  return 0;
}

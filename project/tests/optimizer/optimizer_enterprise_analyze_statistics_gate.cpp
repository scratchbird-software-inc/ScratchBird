// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "optimizer_production_analyze.hpp"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace opt = scratchbird::engine::optimizer;

namespace {

bool Require(bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << "OEIC analyze statistics gate failure: " << message << '\n';
    return false;
  }
  return true;
}

bool HasStatus(const std::vector<opt::StatisticsContractStatus>& statuses,
               const std::string& code) {
  return std::any_of(statuses.begin(), statuses.end(), [&](const auto& status) {
    return status.diagnostic_code == code;
  });
}

opt::OptimizerStatsIdentity Identity(std::string object,
                                     std::string statistic,
                                     std::uint64_t stats_epoch = 20) {
  opt::OptimizerStatsIdentity identity;
  identity.object_uuid = std::move(object);
  identity.statistic_uuid = std::move(statistic);
  identity.stats_epoch = stats_epoch;
  identity.catalog_epoch = 30;
  identity.transaction_visibility_epoch = 40;
  identity.freshness = opt::OptimizerStatsFreshnessState::kFresh;
  identity.source = opt::StatisticSource::kCatalogSample;
  identity.confidence = opt::CostConfidence::kHigh;
  return identity;
}

opt::OptimizerProductionAnalyzeRequest BaseRequest() {
  // SEARCH_KEY: OEIC_PRODUCTION_ANALYZE_STATISTICS
  opt::OptimizerProductionAnalyzeRequest request;
  request.analyze_run_uuid = "analyze.run.1";
  request.relation_uuid = "relation.enterprise.analyze";
  request.descriptor_set_digest = "descriptor-set:enterprise-analyze";
  request.storage_scan_evidence_digest = "storage-scan:evidence:1";
  request.sample_provenance_digest = "sample:provenance:1";
  request.result_contract_hash = "result-contract:analyze:1";
  request.sample_method = opt::OptimizerProductionAnalyzeSampleMethod::kStratifiedSample;
  request.sampled_rows = 5000;
  request.row_count = 10000;
  request.visible_row_count = 9600;
  request.page_count = 128;
  request.average_row_bytes = 192;
  request.catalog_epoch = 30;
  request.stats_epoch = 20;
  request.transaction_visibility_epoch = 40;
  request.security_epoch = 50;
  request.redaction_epoch = 60;
  request.resource_epoch = 70;
  request.source_generation = 80;
  request.benchmark_clean_profile = true;

  request.authority.engine_runtime_scope = true;
  request.authority.catalog_descriptor_authority = true;
  request.authority.catalog_stats_write_authority = true;
  request.authority.storage_scan_authority = true;
  request.authority.page_filespace_authority = true;
  request.authority.index_generation_authority = true;
  request.authority.metric_generation_authority = true;
  request.authority.mga_snapshot_authority = true;
  request.authority.transaction_inventory_authority = true;
  request.authority.security_context_authority = true;
  request.authority.grants_proven = true;
  request.authority.redaction_policy_bound = true;
  request.authority.stats_epoch_authority = true;

  opt::OptimizerProductionAnalyzeColumnSample column;
  column.column_uuid = "column.enterprise.key";
  column.descriptor_digest = "descriptor:column.enterprise.key";
  column.sample_rows = 5000;
  column.null_count = 25;
  column.distinct_count = 4200;
  column.hyperloglog_register_count = 4096;
  column.hyperloglog_estimated_distinct = 4210;
  column.hyperloglog_relative_error = 0.018;
  column.null_fraction = 0.0025;
  column.correlation = 0.62;
  column.average_width_bytes = 16;
  column.min_encoded = "0001";
  column.max_encoded = "9999";
  column.histogram_buckets.push_back({"0001", "3000", 0.30, 3000});
  column.histogram_buckets.push_back({"3001", "7000", 0.40, 4000});
  column.histogram_buckets.push_back({"7001", "9999", 0.30, 3000});
  column.mcv_values.push_back({"0042", 0.08, 800});
  column.mcv_values.push_back({"0100", 0.04, 400});
  request.columns.push_back(column);

  opt::OptimizerProductionAnalyzeExpressionSample expression;
  expression.expression_digest = "expr:lower(column.enterprise.key)";
  expression.descriptor_digest = "descriptor:expression";
  expression.distinct_count = 3900;
  expression.null_fraction = 0.003;
  request.expressions.push_back(expression);

  opt::OptimizerProductionAnalyzeExtendedSample extended;
  extended.catalog_descriptor_proven = true;
  extended.stats.identity = Identity(request.relation_uuid,
                                     "relation.enterprise.analyze:extended:multi");
  extended.stats.kind = opt::ExtendedOptimizerStatisticKind::kMultiColumnNdv;
  extended.stats.relation_uuid = request.relation_uuid;
  extended.stats.column_uuids = {"column.enterprise.key", "column.enterprise.group"};
  extended.stats.multi_column_distinct_count = 6400;
  extended.stats.functional_dependency_strength = 0.35;
  extended.stats.correlation_coefficient = 0.47;
  extended.stats.histogram_selectivity = 0.28;
  extended.stats.sampled_dependency_selectivity = 0.31;
  extended.stats.observed_selectivity_error = 0.06;
  extended.stats.mga_visibility_recheck_required = true;
  extended.stats.security_recheck_required = true;
  request.extended_stats.push_back(extended);

  opt::OptimizerProductionAnalyzeIndexSample index;
  index.captured_from_index_provider = true;
  index.route_capability_proven = true;
  index.stats.identity = Identity(request.relation_uuid, "index.enterprise.key:stats");
  index.stats.index_uuid = "index.enterprise.key";
  index.stats.relation_uuid = request.relation_uuid;
  index.stats.index_family = "btree";
  index.stats.descriptor_digest = "descriptor:index.enterprise.key";
  index.stats.key_column_uuids = {"column.enterprise.key"};
  index.stats.height = 3;
  index.stats.leaf_pages = 48;
  index.stats.distinct_keys = 4200;
  index.stats.clustering_factor = 0.72;
  index.stats.fragmentation_ratio = 0.03;
  index.stats.visibility_coverage = 1.0;
  index.stats.predicate_coverage = 1.0;
  index.stats.equality_lookup_supported = true;
  index.stats.ordered_range_supported = true;
  index.stats.exact_recheck_required = true;
  index.stats.mga_recheck_required = true;
  index.stats.security_recheck_required = true;
  index.stats.route_benchmark_clean = true;
  request.indexes.push_back(index);

  opt::OptimizerProductionAnalyzePageFilespaceSample filespace;
  filespace.captured_from_page_manager = true;
  filespace.stats.identity = Identity(request.relation_uuid,
                                      "filespace.enterprise:relation:stats");
  filespace.stats.filespace_uuid = "filespace.enterprise";
  filespace.stats.page_family = "relation";
  filespace.stats.page_size_bytes = 8192;
  filespace.stats.free_pages = 250000;
  filespace.stats.sequential_latency_score = 0.80;
  filespace.stats.random_latency_score = 3.20;
  filespace.stats.health_score = 1.0;
  filespace.stats.degraded = false;
  request.page_filespaces.push_back(filespace);
  return request;
}

bool ProductionAnalyzePublishesAllStatsFamilies() {
  opt::OptimizerStatisticsStore store;
  const auto request = BaseRequest();
  const auto result = opt::RunOptimizerProductionAnalyze(request, &store);
  const auto persisted = store.Snapshot("persisted.after.analyze");
  const auto catalog = store.ToLegacyCatalog();

  return Require(result.accepted, "production analyze was refused: " + result.diagnostic_code) &&
         Require(result.catalog_stats_written, "catalog stats were not written") &&
         Require(result.snapshot_valid, "snapshot did not validate") &&
         Require(result.benchmark_clean_ready, "benchmark-clean catalog view was not ready") &&
         Require(result.table_stats_written == 1, "table stats count mismatch") &&
         Require(result.column_stats_written == 1, "column stats count mismatch") &&
         Require(result.histogram_stats_written == 1, "histogram stats count mismatch") &&
         Require(result.mcv_stats_written == 2, "MCV stats count mismatch") &&
         Require(result.expression_stats_written == 1, "expression stats count mismatch") &&
         Require(result.extended_stats_written == 1, "extended stats count mismatch") &&
         Require(result.index_stats_written == 1, "index stats count mismatch") &&
         Require(result.page_filespace_stats_written == 1, "page/filespace stats count mismatch") &&
         Require(persisted.tables.size() == 1 && persisted.columns.size() == 1 &&
                     persisted.histograms.size() == 1 && persisted.mcv.size() == 2 &&
                     persisted.expressions.size() == 1 &&
                     persisted.extended_stats.size() == 1 &&
                     persisted.indexes.size() == 1 &&
                     persisted.page_filespaces.size() == 1,
                 "persisted snapshot does not contain every stats family") &&
         Require(catalog.Find("column_hll_estimated_distinct",
                              "column.enterprise.key").has_value(),
                 "HLL distinct stat missing from catalog view") &&
         Require(catalog.Find("expression_distinct_count",
                              "expr:lower(column.enterprise.key)").has_value(),
                 "expression stat missing from catalog view") &&
         Require(catalog.Find("extended_multi_column_distinct_count",
                              "relation.enterprise.analyze:extended:multi").has_value(),
                 "extended stat missing from catalog view") &&
         Require(catalog.Find("index_route_benchmark_clean",
                              "index.enterprise.key").has_value(),
                 "index benchmark-clean stat missing") &&
         Require(catalog.Find("filespace_available_pages",
                              "filespace.enterprise").has_value(),
                 "page/filespace stat missing");
}

bool ProductionAnalyzeInvalidatesPinnedStats() {
  opt::OptimizerStatisticsStore store;
  auto request = BaseRequest();
  if (!opt::RunOptimizerProductionAnalyze(request, &store).accepted) {
    return Require(false, "initial analyze failed");
  }

  opt::OptimizerPinnedStatsDescriptorSnapshot pinned;
  pinned.key.catalog_epoch = request.catalog_epoch;
  pinned.key.security_epoch = request.security_epoch;
  pinned.key.resource_policy_epoch = request.resource_epoch;
  pinned.key.name_resolution_epoch = 90;
  pinned.key.stats_epoch = request.stats_epoch;
  pinned.key.descriptor_set_digest = request.descriptor_set_digest;
  pinned.key.object_uuids = {request.relation_uuid};
  pinned.key.index_uuids = {"index.enterprise.key"};
  pinned.key.security_policy_identity = "security.policy.enterprise";
  pinned.key.redaction_policy_identity = "redaction.policy.enterprise";
  pinned.stats_snapshot = store.Snapshot("pinned.before.refresh");
  const auto put = opt::GlobalOptimizerPinnedStatsDescriptorCache().Put(pinned);
  if (!Require(put.ok, "pinned stats cache put failed: " + put.diagnostic_code)) {
    return false;
  }

  request.analyze_run_uuid = "analyze.run.2";
  request.stats_epoch = 21;
  request.source_generation = 81;
  if (!opt::RunOptimizerProductionAnalyze(request, &store).accepted) {
    return Require(false, "refresh analyze failed");
  }
  const auto lookup = opt::GlobalOptimizerPinnedStatsDescriptorCache().Lookup(pinned.key);
  return Require(!lookup.ok &&
                     lookup.diagnostic_code == "SB_OPT_PINNED_STATS_CACHE_MISS",
                 "pinned stats cache was not invalidated");
}

bool ProductionAnalyzeRejectsUnsafeAuthorityAndBenchmarkOverclaim() {
  opt::OptimizerStatisticsStore store;
  auto unsafe = BaseRequest();
  unsafe.authority.parser_or_reference_authority = true;
  const auto unsafe_result = opt::RunOptimizerProductionAnalyze(unsafe, &store);

  auto overclaim = BaseRequest();
  overclaim.indexes.front().stats.route_benchmark_clean = false;
  const auto overclaim_result = opt::RunOptimizerProductionAnalyze(overclaim, &store);

  return Require(!unsafe_result.accepted,
                 "parser/reference authority was accepted") &&
         Require(HasStatus(unsafe_result.validation_statuses,
                           "SB_OPT_ANALYZE_UNSAFE_AUTHORITY"),
                 "unsafe authority diagnostic missing") &&
         Require(!overclaim_result.accepted,
                 "benchmark-clean index overclaim was accepted") &&
         Require(HasStatus(overclaim_result.benchmark_clean_statuses,
                           "SB_OPT_ANALYZE_INDEX_ROUTE_NOT_BENCHMARK_CLEAN"),
                 "benchmark-clean overclaim diagnostic missing");
}

}  // namespace

int main() {
  if (!ProductionAnalyzePublishesAllStatsFamilies()) return EXIT_FAILURE;
  if (!ProductionAnalyzeInvalidatesPinnedStats()) return EXIT_FAILURE;
  if (!ProductionAnalyzeRejectsUnsafeAuthorityAndBenchmarkOverclaim()) {
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}

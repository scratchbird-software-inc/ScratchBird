// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "optimizer_statistics_full.hpp"
#include "statistics_catalog.hpp"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace opt = scratchbird::engine::optimizer;

namespace {

bool Require(bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << "OPCH statistics/index gate failure: " << message << '\n';
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
                                     std::uint64_t epoch = 5) {
  opt::OptimizerStatsIdentity identity;
  identity.object_uuid = std::move(object);
  identity.statistic_uuid = std::move(statistic);
  identity.stats_epoch = epoch;
  identity.catalog_epoch = 7;
  identity.transaction_visibility_epoch = 5;
  identity.freshness = opt::OptimizerStatsFreshnessState::kFresh;
  identity.source = opt::StatisticSource::kCatalogSample;
  identity.confidence = opt::CostConfidence::kHigh;
  return identity;
}

bool StatisticsAnalyzeParityCoversSampleHllAndExtendedStats() {
  // SEARCH_KEY: OPCH_STATISTICS_ANALYZE_PARITY
  opt::OptimizerStatisticsStore store;
  opt::AnalyzeSampleInput input;
  input.relation_uuid = "rel.stats";
  input.sampled_rows = 1000;
  input.total_rows_estimate = 10000;
  input.page_count = 80;
  input.average_row_bytes = 128;
  input.stats_epoch = 5;
  input.catalog_epoch = 7;
  store.UpsertTable(opt::BuildTableStatsFromAnalyzeSample(input));

  opt::ColumnStats column;
  column.identity = Identity("rel.stats", "stats.column");
  column.column_uuid = "col.stats";
  column.descriptor_digest = "desc:col.stats";
  column.sample_method = "vitter_algorithm_s";
  column.sample_provenance_digest = "sample:stats:epoch5";
  column.sample_rows = 1000;
  column.hyperloglog_register_count = 4096;
  column.hyperloglog_estimated_distinct = 7500;
  column.hyperloglog_relative_error = 0.02;
  column.null_count = 10;
  column.distinct_count = 7500;
  column.null_fraction = 0.001;
  column.correlation = 0.25;
  column.average_width_bytes = 16;
  store.UpsertColumn(column);

  opt::HistogramStats histogram;
  histogram.identity = Identity("rel.stats", "stats.histogram");
  histogram.column_uuid = column.column_uuid;
  histogram.buckets.push_back({"a", "m", 0.5, 5000});
  histogram.buckets.push_back({"n", "z", 0.5, 5000});
  store.UpsertHistogram(histogram);

  opt::MostCommonValueStats mcv;
  mcv.identity = Identity("rel.stats", "stats.mcv");
  mcv.column_uuid = column.column_uuid;
  mcv.value_encoded = "value:hot";
  mcv.frequency = 0.12;
  store.UpsertMcv(mcv);

  opt::ExtendedOptimizerStatistic extended;
  extended.identity = Identity("rel.stats", "stats.extended");
  extended.kind = opt::ExtendedOptimizerStatisticKind::kMultiColumnNdv;
  extended.relation_uuid = "rel.stats";
  extended.column_uuids = {"col.stats", "col.other"};
  extended.multi_column_distinct_count = 8400;
  extended.functional_dependency_strength = 0.2;
  extended.correlation_coefficient = 0.4;
  extended.histogram_selectivity = 0.33;
  extended.sampled_dependency_selectivity = 0.31;
  extended.observed_selectivity_error = 0.05;
  extended.mga_visibility_recheck_required = true;
  extended.security_recheck_required = true;
  store.UpsertExtendedStatistic(extended);

  const auto snapshot = store.Snapshot("snapshot:stats");
  const auto statuses = opt::ValidateOptimizerStatsSnapshot(snapshot);
  const auto legacy = store.ToLegacyCatalog();
  return Require(HasStatus(statuses, "SB_OPT_STATS_OK"),
                 "valid stats snapshot was not accepted") &&
         Require(legacy.Find("column_hll_estimated_distinct", column.column_uuid).has_value(),
                 "HLL distinct estimate not exported") &&
         Require(legacy.Find("column_sample_rows", column.column_uuid).has_value(),
                 "sample row provenance not exported") &&
         Require(opt::EstimateRangeSelectivityFromHistogram(histogram) == 1.0,
                 "histogram selectivity changed");
}

bool BenchmarkCleanRejectsUnsafeStatistics() {
  // SEARCH_KEY: OPCH_STATISTICS_FRESHNESS_BENCHMARK_CLEAN_GATE
  opt::OptimizerStatisticsCatalog catalog;
  catalog.Add(opt::MakeStatistic("row_count", "relation", "rel.safe", 100.0,
                                 opt::StatisticSource::kCatalogExact, 9, 0,
                                 opt::CostConfidence::kExact));
  catalog.Add(opt::MakeStatistic("page_count", "relation", "rel.stale", 10.0,
                                 opt::StatisticSource::kCatalogSample, 9,
                                 120000000, opt::CostConfidence::kHigh));
  catalog.Add(opt::MakeStatistic("visible_row_count", "relation", "rel.policy",
                                 90.0, opt::StatisticSource::kPolicyDefault, 9,
                                 0, opt::CostConfidence::kLow));
  catalog.Add(opt::MakeStatistic("index_depth", "index", "rel.cluster", 3.0,
                                 opt::StatisticSource::kClusterMetric, 9, 0,
                                 opt::CostConfidence::kHigh, true, true));
  catalog.Add(opt::MakeStatistic("index_leaf_pages", "index", "local.default",
                                 8.0, opt::StatisticSource::kPolicyDefault, 1,
                                 0, opt::CostConfidence::kLow));

  const auto safe = catalog.ValidateBenchmarkCleanInputs({"row_count"}, "rel.safe");
  const auto stale = catalog.ValidateBenchmarkCleanInputs({"page_count"}, "rel.stale");
  const auto policy = catalog.ValidateBenchmarkCleanInputs({"visible_row_count"}, "rel.policy");
  const auto cluster = catalog.ValidateBenchmarkCleanInputs({"index_depth"}, "rel.cluster");
  const auto local = catalog.ValidateBenchmarkCleanInputs({"index_leaf_pages"}, "rel.missing");
  return Require(HasStatus(safe, "SB_OPTIMIZER_BENCHMARK_CLEAN.OK"),
                 "safe catalog stat rejected") &&
         Require(HasStatus(stale, "SB_OPTIMIZER_BENCHMARK_CLEAN.STALE_STATS"),
                 "stale stat accepted") &&
         Require(HasStatus(policy, "SB_OPTIMIZER_BENCHMARK_CLEAN.POLICY_DEFAULT_STATS"),
                 "policy default stat accepted") &&
         Require(HasStatus(cluster, "SB_OPTIMIZER_BENCHMARK_CLEAN.CLUSTER_ONLY_STATS"),
                 "cluster-only stat accepted") &&
         Require(HasStatus(local, "SB_OPTIMIZER_BENCHMARK_CLEAN.LOCAL_DEFAULT_STATS"),
                 "local default fallback accepted");
}

opt::IndexStats Index(std::string family, std::string uuid) {
  opt::IndexStats index;
  index.identity = Identity("rel.index", uuid + ":stats");
  index.index_family = std::move(family);
  index.index_uuid = std::move(uuid);
  index.relation_uuid = "rel.index";
  index.descriptor_digest = "desc:index";
  index.height = 2;
  index.leaf_pages = 8;
  index.distinct_keys = 1000;
  index.visibility_coverage = 1.0;
  index.predicate_coverage = 1.0;
  index.false_positive_ratio = 0.0;
  index.exact_recheck_required = true;
  index.mga_recheck_required = true;
  index.security_recheck_required = true;
  index.route_benchmark_clean = true;
  return index;
}

bool IndexFamilyCoverageRequiresRouteAndRecheckSemantics() {
  // SEARCH_KEY: OPCH_INDEX_FAMILY_COST_STATISTICS_COVERAGE
  auto btree = Index("btree", "idx.btree");
  btree.equality_lookup_supported = true;
  btree.ordered_range_supported = true;

  auto hash = Index("hash", "idx.hash");
  hash.equality_lookup_supported = true;

  auto bloom = Index("bloom", "idx.bloom");
  bloom.negative_prune_supported = true;
  bloom.candidate_set_producer = true;
  bloom.false_positive_ratio = 0.01;

  auto missing_recheck = Index("vector_hnsw", "idx.vector");
  missing_recheck.candidate_set_producer = true;
  missing_recheck.exact_recheck_required = false;

  const auto ok = opt::ValidateIndexFamilyCostCoverage({btree, hash, bloom});
  const auto bad = opt::ValidateIndexFamilyCostCoverage({missing_recheck});
  return Require(HasStatus(ok, "SB_OPT_INDEX_FAMILY_COST_COVERAGE_OK"),
                 "valid index family coverage rejected") &&
         Require(HasStatus(bad, "SB_OPT_INDEX_FAMILY_RECHECK_REQUIRED"),
                 "missing exact/MGA/security recheck accepted");
}

}  // namespace

int main() {
  if (!StatisticsAnalyzeParityCoversSampleHllAndExtendedStats()) return EXIT_FAILURE;
  if (!BenchmarkCleanRejectsUnsafeStatistics()) return EXIT_FAILURE;
  if (!IndexFamilyCoverageRequiresRouteAndRecheckSemantics()) return EXIT_FAILURE;
  return EXIT_SUCCESS;
}

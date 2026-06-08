// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "optimizer_statistics_full.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace scratchbird::engine::optimizer {

// SEARCH_KEY: OEIC_PRODUCTION_ANALYZE_STATISTICS
// Production ANALYZE collection is a catalog/statistics update path only. It
// consumes engine-owned scan evidence and writes optimizer statistics; it never
// becomes row visibility, transaction finality, security, parser, donor, or
// recovery authority.
enum class OptimizerProductionAnalyzeSampleMethod {
  kFullScan,
  kSystemSample,
  kStratifiedSample,
  kIndexAssistedSample,
};

struct OptimizerProductionAnalyzeAuthority {
  bool engine_runtime_scope = false;
  bool catalog_descriptor_authority = false;
  bool catalog_stats_write_authority = false;
  bool storage_scan_authority = false;
  bool page_filespace_authority = false;
  bool index_generation_authority = false;
  bool metric_generation_authority = false;
  bool mga_snapshot_authority = false;
  bool transaction_inventory_authority = false;
  bool security_context_authority = false;
  bool grants_proven = false;
  bool redaction_policy_bound = false;
  bool stats_epoch_authority = false;

  bool parser_or_donor_authority = false;
  bool client_finality_authority = false;
  bool client_visibility_authority = false;
  bool metric_finality_authority = false;
  bool metric_visibility_authority = false;
  bool external_recovery_authority = false;
  bool cluster_authority = false;
  bool fixture_or_synthetic_source = false;
};

struct OptimizerProductionAnalyzeMcvSample {
  std::string value_encoded;
  double frequency = 0.0;
  std::uint64_t row_count = 0;
};

struct OptimizerProductionAnalyzeColumnSample {
  std::string column_uuid;
  std::string descriptor_digest;
  std::uint64_t sample_rows = 0;
  std::uint64_t null_count = 0;
  std::uint64_t distinct_count = 0;
  std::uint64_t hyperloglog_register_count = 0;
  std::uint64_t hyperloglog_estimated_distinct = 0;
  double hyperloglog_relative_error = 0.0;
  double null_fraction = 0.0;
  double correlation = 0.0;
  std::uint64_t average_width_bytes = 0;
  std::string min_encoded;
  std::string max_encoded;
  std::vector<HistogramBucketStats> histogram_buckets;
  std::vector<OptimizerProductionAnalyzeMcvSample> mcv_values;
};

struct OptimizerProductionAnalyzeExpressionSample {
  std::string expression_digest;
  std::string descriptor_digest;
  std::uint64_t distinct_count = 0;
  double null_fraction = 0.0;
};

struct OptimizerProductionAnalyzeExtendedSample {
  ExtendedOptimizerStatistic stats;
  bool catalog_descriptor_proven = false;
};

struct OptimizerProductionAnalyzeIndexSample {
  IndexStats stats;
  bool captured_from_index_provider = false;
  bool route_capability_proven = false;
};

struct OptimizerProductionAnalyzePageFilespaceSample {
  PageFilespaceStats stats;
  bool captured_from_page_manager = false;
};

struct OptimizerProductionAnalyzeRequest {
  std::string analyze_run_uuid;
  std::string relation_uuid;
  std::string descriptor_set_digest;
  std::string storage_scan_evidence_digest;
  std::string sample_provenance_digest;
  std::string result_contract_hash;
  OptimizerProductionAnalyzeSampleMethod sample_method =
      OptimizerProductionAnalyzeSampleMethod::kSystemSample;
  OptimizerProductionAnalyzeAuthority authority;

  std::uint64_t sampled_rows = 0;
  std::uint64_t row_count = 0;
  std::uint64_t visible_row_count = 0;
  std::uint64_t page_count = 0;
  std::uint64_t average_row_bytes = 0;
  std::uint64_t catalog_epoch = 0;
  std::uint64_t stats_epoch = 0;
  std::uint64_t transaction_visibility_epoch = 0;
  std::uint64_t security_epoch = 0;
  std::uint64_t redaction_epoch = 0;
  std::uint64_t resource_epoch = 0;
  std::uint64_t source_generation = 0;

  bool benchmark_clean_profile = false;
  bool require_all_stat_families = true;

  std::vector<OptimizerProductionAnalyzeColumnSample> columns;
  std::vector<OptimizerProductionAnalyzeExpressionSample> expressions;
  std::vector<OptimizerProductionAnalyzeExtendedSample> extended_stats;
  std::vector<OptimizerProductionAnalyzeIndexSample> indexes;
  std::vector<OptimizerProductionAnalyzePageFilespaceSample> page_filespaces;
};

struct OptimizerProductionAnalyzeResult {
  bool accepted = false;
  bool catalog_stats_written = false;
  bool snapshot_valid = false;
  bool benchmark_clean_ready = false;
  bool pinned_stats_invalidated = false;
  std::string diagnostic_code;
  std::vector<std::string> evidence;
  std::vector<StatisticsContractStatus> validation_statuses;
  std::vector<StatisticsContractStatus> benchmark_clean_statuses;
  OptimizerStatsSnapshot snapshot;
  OptimizerStatisticsCatalog catalog_view;
  std::uint64_t table_stats_written = 0;
  std::uint64_t column_stats_written = 0;
  std::uint64_t histogram_stats_written = 0;
  std::uint64_t mcv_stats_written = 0;
  std::uint64_t expression_stats_written = 0;
  std::uint64_t extended_stats_written = 0;
  std::uint64_t index_stats_written = 0;
  std::uint64_t page_filespace_stats_written = 0;
};

const char* OptimizerProductionAnalyzeSampleMethodName(
    OptimizerProductionAnalyzeSampleMethod method);

OptimizerProductionAnalyzeResult RunOptimizerProductionAnalyze(
    const OptimizerProductionAnalyzeRequest& request,
    OptimizerStatisticsStore* store);

std::vector<StatisticsContractStatus> ValidateProductionAnalyzeBenchmarkCleanCatalog(
    const OptimizerStatisticsCatalog& catalog,
    const OptimizerProductionAnalyzeRequest& request);

}  // namespace scratchbird::engine::optimizer

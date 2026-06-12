// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "optimizer_production_analyze.hpp"

#include <algorithm>
#include <cmath>
#include <sstream>
#include <utility>

namespace scratchbird::engine::optimizer {
namespace {

OptimizerProductionAnalyzeResult Refuse(std::string code, std::string evidence) {
  OptimizerProductionAnalyzeResult result;
  result.accepted = false;
  result.diagnostic_code = std::move(code);
  result.evidence.push_back(std::move(evidence));
  return result;
}

StatisticsContractStatus Status(bool ok, std::string code, std::string detail) {
  StatisticsContractStatus status;
  status.ok = ok;
  status.diagnostic_code = std::move(code);
  status.detail = std::move(detail);
  return status;
}

bool HasFailure(const std::vector<StatisticsContractStatus>& statuses) {
  return std::any_of(statuses.begin(), statuses.end(), [](const auto& status) {
    return !status.ok;
  });
}

bool InRange01(double value) {
  return value >= 0.0 && value <= 1.0;
}

bool IsFullScan(const OptimizerProductionAnalyzeRequest& request) {
  return request.sample_method == OptimizerProductionAnalyzeSampleMethod::kFullScan &&
         request.sampled_rows == request.row_count;
}

StatisticSource AnalyzeSource(const OptimizerProductionAnalyzeRequest& request) {
  return IsFullScan(request) ? StatisticSource::kCatalogExact
                             : StatisticSource::kCatalogSample;
}

CostConfidence AnalyzeConfidence(const OptimizerProductionAnalyzeRequest& request) {
  if (IsFullScan(request)) return CostConfidence::kExact;
  if (request.row_count == 0 || request.sampled_rows == 0) return CostConfidence::kRejected;
  const double sample_fraction =
      static_cast<double>(request.sampled_rows) / static_cast<double>(request.row_count);
  if (sample_fraction >= 0.50) return CostConfidence::kHigh;
  if (sample_fraction >= 0.10) return CostConfidence::kMedium;
  return CostConfidence::kLow;
}

OptimizerStatsIdentity Identity(const OptimizerProductionAnalyzeRequest& request,
                                std::string object_uuid,
                                std::string statistic_uuid) {
  OptimizerStatsIdentity identity;
  identity.object_uuid = std::move(object_uuid);
  identity.statistic_uuid = std::move(statistic_uuid);
  identity.stats_epoch = request.stats_epoch;
  identity.catalog_epoch = request.catalog_epoch;
  identity.transaction_visibility_epoch = request.transaction_visibility_epoch;
  identity.freshness = OptimizerStatsFreshnessState::kFresh;
  identity.source = AnalyzeSource(request);
  identity.confidence = AnalyzeConfidence(request);
  return identity;
}

std::vector<StatisticsContractStatus> ValidateAnalyzeRequest(
    const OptimizerProductionAnalyzeRequest& request) {
  std::vector<StatisticsContractStatus> statuses;
  if (request.analyze_run_uuid.empty()) {
    statuses.push_back(Status(false, "SB_OPT_ANALYZE_RUN_UUID_REQUIRED", "analyze_run_uuid"));
  }
  if (request.relation_uuid.empty()) {
    statuses.push_back(Status(false, "SB_OPT_ANALYZE_RELATION_UUID_REQUIRED", "relation_uuid"));
  }
  if (request.descriptor_set_digest.empty()) {
    statuses.push_back(Status(false, "SB_OPT_ANALYZE_DESCRIPTOR_DIGEST_REQUIRED", request.relation_uuid));
  }
  if (request.storage_scan_evidence_digest.empty()) {
    statuses.push_back(Status(false, "SB_OPT_ANALYZE_STORAGE_EVIDENCE_REQUIRED", request.relation_uuid));
  }
  if (request.sample_provenance_digest.empty()) {
    statuses.push_back(Status(false, "SB_OPT_ANALYZE_SAMPLE_PROVENANCE_REQUIRED", request.relation_uuid));
  }
  if (request.result_contract_hash.empty()) {
    statuses.push_back(Status(false, "SB_OPT_ANALYZE_RESULT_CONTRACT_REQUIRED", request.relation_uuid));
  }
  if (request.catalog_epoch == 0 || request.stats_epoch == 0 ||
      request.transaction_visibility_epoch == 0 || request.security_epoch == 0 ||
      request.redaction_epoch == 0 || request.resource_epoch == 0 ||
      request.source_generation == 0) {
    statuses.push_back(Status(false, "SB_OPT_ANALYZE_EPOCH_EVIDENCE_REQUIRED", request.relation_uuid));
  }
  if (request.row_count == 0 || request.visible_row_count == 0 ||
      request.sampled_rows == 0 || request.page_count == 0 ||
      request.average_row_bytes == 0) {
    statuses.push_back(Status(false, "SB_OPT_ANALYZE_TABLE_SAMPLE_REQUIRED", request.relation_uuid));
  }
  if (request.sampled_rows > request.row_count ||
      request.visible_row_count > request.row_count) {
    statuses.push_back(Status(false, "SB_OPT_ANALYZE_TABLE_SAMPLE_INCONSISTENT", request.relation_uuid));
  }

  const auto& authority = request.authority;
  if (!authority.engine_runtime_scope || !authority.catalog_descriptor_authority ||
      !authority.catalog_stats_write_authority || !authority.storage_scan_authority ||
      !authority.page_filespace_authority || !authority.index_generation_authority ||
      !authority.metric_generation_authority || !authority.mga_snapshot_authority ||
      !authority.transaction_inventory_authority ||
      !authority.security_context_authority || !authority.grants_proven ||
      !authority.redaction_policy_bound || !authority.stats_epoch_authority) {
    statuses.push_back(Status(false, "SB_OPT_ANALYZE_AUTHORITY_REQUIRED", request.relation_uuid));
  }
  if (authority.parser_or_reference_authority || authority.client_finality_authority ||
      authority.client_visibility_authority || authority.metric_finality_authority ||
      authority.metric_visibility_authority || authority.external_recovery_authority ||
      authority.cluster_authority || authority.fixture_or_synthetic_source) {
    statuses.push_back(Status(false, "SB_OPT_ANALYZE_UNSAFE_AUTHORITY", request.relation_uuid));
  }

  if (request.require_all_stat_families) {
    if (request.columns.empty()) statuses.push_back(Status(false, "SB_OPT_ANALYZE_COLUMNS_REQUIRED", request.relation_uuid));
    if (request.expressions.empty()) statuses.push_back(Status(false, "SB_OPT_ANALYZE_EXPRESSIONS_REQUIRED", request.relation_uuid));
    if (request.extended_stats.empty()) statuses.push_back(Status(false, "SB_OPT_ANALYZE_EXTENDED_STATS_REQUIRED", request.relation_uuid));
    if (request.indexes.empty()) statuses.push_back(Status(false, "SB_OPT_ANALYZE_INDEX_STATS_REQUIRED", request.relation_uuid));
    if (request.page_filespaces.empty()) statuses.push_back(Status(false, "SB_OPT_ANALYZE_PAGE_FILESPACE_STATS_REQUIRED", request.relation_uuid));
  }

  for (const auto& column : request.columns) {
    if (column.column_uuid.empty() || column.descriptor_digest.empty()) {
      statuses.push_back(Status(false, "SB_OPT_ANALYZE_COLUMN_DESCRIPTOR_REQUIRED", request.relation_uuid));
    }
    if (column.sample_rows == 0 || column.sample_rows > request.sampled_rows) {
      statuses.push_back(Status(false, "SB_OPT_ANALYZE_COLUMN_SAMPLE_INVALID", column.column_uuid));
    }
    if (column.hyperloglog_register_count == 0 ||
        column.hyperloglog_estimated_distinct == 0 ||
        column.hyperloglog_relative_error < 0.0 ||
        column.hyperloglog_relative_error > 1.0) {
      statuses.push_back(Status(false, "SB_OPT_ANALYZE_COLUMN_HLL_REQUIRED", column.column_uuid));
    }
    if (!InRange01(column.null_fraction) ||
        column.correlation < -1.0 ||
        column.correlation > 1.0 ||
        column.average_width_bytes == 0 ||
        column.distinct_count == 0) {
      statuses.push_back(Status(false, "SB_OPT_ANALYZE_COLUMN_STATS_INVALID", column.column_uuid));
    }
    if (column.histogram_buckets.empty()) {
      statuses.push_back(Status(false, "SB_OPT_ANALYZE_HISTOGRAM_REQUIRED", column.column_uuid));
    }
    double histogram_fraction = 0.0;
    for (const auto& bucket : column.histogram_buckets) {
      if (bucket.lower_bound_encoded.empty() || bucket.upper_bound_encoded.empty() ||
          !InRange01(bucket.fraction) || bucket.row_count == 0) {
        statuses.push_back(Status(false, "SB_OPT_ANALYZE_HISTOGRAM_BUCKET_INVALID", column.column_uuid));
      }
      histogram_fraction += bucket.fraction;
    }
    if (!column.histogram_buckets.empty() &&
        (histogram_fraction <= 0.0 || histogram_fraction > 1.000001)) {
      statuses.push_back(Status(false, "SB_OPT_ANALYZE_HISTOGRAM_FRACTION_INVALID", column.column_uuid));
    }
    if (column.mcv_values.empty()) {
      statuses.push_back(Status(false, "SB_OPT_ANALYZE_MCV_REQUIRED", column.column_uuid));
    }
    double mcv_frequency = 0.0;
    for (const auto& mcv : column.mcv_values) {
      if (mcv.value_encoded.empty() || !InRange01(mcv.frequency)) {
        statuses.push_back(Status(false, "SB_OPT_ANALYZE_MCV_VALUE_INVALID", column.column_uuid));
      }
      mcv_frequency += mcv.frequency;
    }
    if (mcv_frequency > 1.000001) {
      statuses.push_back(Status(false, "SB_OPT_ANALYZE_MCV_FREQUENCY_INVALID", column.column_uuid));
    }
  }

  for (const auto& expression : request.expressions) {
    if (expression.expression_digest.empty() || expression.descriptor_digest.empty()) {
      statuses.push_back(Status(false, "SB_OPT_ANALYZE_EXPRESSION_DESCRIPTOR_REQUIRED", request.relation_uuid));
    }
    if (expression.distinct_count == 0 || !InRange01(expression.null_fraction)) {
      statuses.push_back(Status(false, "SB_OPT_ANALYZE_EXPRESSION_STATS_INVALID", expression.expression_digest));
    }
  }

  for (const auto& extended : request.extended_stats) {
    if (!extended.catalog_descriptor_proven) {
      statuses.push_back(Status(false, "SB_OPT_ANALYZE_EXTENDED_DESCRIPTOR_REQUIRED", extended.stats.identity.statistic_uuid));
    }
    if (extended.stats.finality_authority ||
        !extended.stats.mga_visibility_recheck_required ||
        !extended.stats.security_recheck_required) {
      statuses.push_back(Status(false, "SB_OPT_ANALYZE_EXTENDED_AUTHORITY_INVALID", extended.stats.identity.statistic_uuid));
    }
  }

  for (const auto& index : request.indexes) {
    if (!index.captured_from_index_provider || !index.route_capability_proven) {
      statuses.push_back(Status(false, "SB_OPT_ANALYZE_INDEX_PROVIDER_REQUIRED", index.stats.index_uuid));
    }
  }

  for (const auto& filespace : request.page_filespaces) {
    if (!filespace.captured_from_page_manager) {
      statuses.push_back(Status(false, "SB_OPT_ANALYZE_FILESPACE_PROVIDER_REQUIRED", filespace.stats.filespace_uuid));
    }
  }

  if (statuses.empty()) {
    statuses.push_back(Status(true, "SB_OPT_ANALYZE_REQUEST_OK", request.relation_uuid));
  }
  return statuses;
}

void PopulateStore(const OptimizerProductionAnalyzeRequest& request,
                   OptimizerStatisticsStore* store,
                   OptimizerProductionAnalyzeResult* result) {
  AnalyzeSampleInput table_input;
  table_input.relation_uuid = request.relation_uuid;
  table_input.sampled_rows = request.sampled_rows;
  table_input.total_rows_estimate = request.row_count;
  table_input.page_count = request.page_count;
  table_input.average_row_bytes = request.average_row_bytes;
  table_input.stats_epoch = request.stats_epoch;
  table_input.catalog_epoch = request.catalog_epoch;
  auto table = BuildTableStatsFromAnalyzeSample(table_input);
  table.visible_row_count = request.visible_row_count;
  table.identity.transaction_visibility_epoch = request.transaction_visibility_epoch;
  table.identity.source = AnalyzeSource(request);
  table.identity.confidence = AnalyzeConfidence(request);
  store->UpsertTable(std::move(table));
  if (result) ++result->table_stats_written;

  for (const auto& input : request.columns) {
    ColumnStats column;
    column.identity = Identity(request,
                               request.relation_uuid,
                               request.relation_uuid + ":" + input.column_uuid + ":column_stats");
    column.column_uuid = input.column_uuid;
    column.descriptor_digest = input.descriptor_digest;
    column.sample_method = OptimizerProductionAnalyzeSampleMethodName(request.sample_method);
    column.sample_provenance_digest = request.sample_provenance_digest;
    column.sample_rows = input.sample_rows;
    column.hyperloglog_register_count = input.hyperloglog_register_count;
    column.hyperloglog_estimated_distinct = input.hyperloglog_estimated_distinct;
    column.hyperloglog_relative_error = input.hyperloglog_relative_error;
    column.null_count = input.null_count;
    column.distinct_count = input.distinct_count;
    column.null_fraction = input.null_fraction;
    column.correlation = input.correlation;
    column.average_width_bytes = input.average_width_bytes;
    column.min_encoded = input.min_encoded;
    column.max_encoded = input.max_encoded;
    store->UpsertColumn(std::move(column));
    if (result) ++result->column_stats_written;

    HistogramStats histogram;
    histogram.identity = Identity(request,
                                  request.relation_uuid,
                                  request.relation_uuid + ":" + input.column_uuid + ":histogram");
    histogram.column_uuid = input.column_uuid;
    histogram.buckets = input.histogram_buckets;
    store->UpsertHistogram(std::move(histogram));
    if (result) ++result->histogram_stats_written;

    for (const auto& input_mcv : input.mcv_values) {
      MostCommonValueStats mcv;
      mcv.identity = Identity(request,
                              request.relation_uuid,
                              request.relation_uuid + ":" + input.column_uuid + ":mcv:" + input_mcv.value_encoded);
      mcv.column_uuid = input.column_uuid;
      mcv.value_encoded = input_mcv.value_encoded;
      mcv.frequency = input_mcv.frequency;
      store->UpsertMcv(std::move(mcv));
      if (result) ++result->mcv_stats_written;
    }
  }

  for (const auto& input : request.expressions) {
    ExpressionStats expression;
    expression.identity = Identity(request,
                                   request.relation_uuid,
                                   request.relation_uuid + ":" + input.expression_digest + ":expression_stats");
    expression.expression_digest = input.expression_digest;
    expression.descriptor_digest = input.descriptor_digest;
    expression.distinct_count = input.distinct_count;
    expression.null_fraction = input.null_fraction;
    store->UpsertExpression(std::move(expression));
    if (result) ++result->expression_stats_written;
  }

  for (auto input : request.extended_stats) {
    if (input.stats.identity.object_uuid.empty()) input.stats.identity.object_uuid = request.relation_uuid;
    if (input.stats.identity.statistic_uuid.empty()) {
      input.stats.identity.statistic_uuid =
          request.relation_uuid + ":extended:" +
          ExtendedOptimizerStatisticKindName(input.stats.kind);
    }
    input.stats.identity.stats_epoch = request.stats_epoch;
    input.stats.identity.catalog_epoch = request.catalog_epoch;
    input.stats.identity.transaction_visibility_epoch = request.transaction_visibility_epoch;
    input.stats.identity.freshness = OptimizerStatsFreshnessState::kFresh;
    input.stats.identity.source = AnalyzeSource(request);
    input.stats.identity.confidence = AnalyzeConfidence(request);
    if (input.stats.relation_uuid.empty()) input.stats.relation_uuid = request.relation_uuid;
    store->UpsertExtendedStatistic(std::move(input.stats));
    if (result) ++result->extended_stats_written;
  }

  for (auto input : request.indexes) {
    input.stats.identity.stats_epoch = request.stats_epoch;
    input.stats.identity.catalog_epoch = request.catalog_epoch;
    input.stats.identity.transaction_visibility_epoch = request.transaction_visibility_epoch;
    input.stats.identity.freshness = OptimizerStatsFreshnessState::kFresh;
    input.stats.identity.source = AnalyzeSource(request);
    input.stats.identity.confidence = AnalyzeConfidence(request);
    if (input.stats.identity.object_uuid.empty()) input.stats.identity.object_uuid = request.relation_uuid;
    if (input.stats.identity.statistic_uuid.empty()) {
      input.stats.identity.statistic_uuid = input.stats.index_uuid + ":index_stats";
    }
    if (input.stats.relation_uuid.empty()) input.stats.relation_uuid = request.relation_uuid;
    store->UpsertIndex(std::move(input.stats));
    if (result) ++result->index_stats_written;
  }

  for (auto input : request.page_filespaces) {
    input.stats.identity.stats_epoch = request.stats_epoch;
    input.stats.identity.catalog_epoch = request.catalog_epoch;
    input.stats.identity.transaction_visibility_epoch = request.transaction_visibility_epoch;
    input.stats.identity.freshness = OptimizerStatsFreshnessState::kFresh;
    input.stats.identity.source = AnalyzeSource(request);
    input.stats.identity.confidence = AnalyzeConfidence(request);
    if (input.stats.identity.object_uuid.empty()) input.stats.identity.object_uuid = request.relation_uuid;
    if (input.stats.identity.statistic_uuid.empty()) {
      input.stats.identity.statistic_uuid =
          input.stats.filespace_uuid + ":" + input.stats.page_family + ":page_filespace_stats";
    }
    store->UpsertPageFilespace(std::move(input.stats));
    if (result) ++result->page_filespace_stats_written;
  }
}

void AppendBenchmarkStatuses(std::vector<StatisticsContractStatus>* target,
                             std::vector<StatisticsContractStatus> statuses) {
  if (target == nullptr) return;
  target->insert(target->end(), statuses.begin(), statuses.end());
}

std::string ExtendedStatisticObjectKey(const OptimizerProductionAnalyzeRequest& request,
                                       const OptimizerProductionAnalyzeExtendedSample& extended) {
  if (!extended.stats.identity.statistic_uuid.empty()) {
    return extended.stats.identity.statistic_uuid;
  }
  return request.relation_uuid + ":extended:" +
         ExtendedOptimizerStatisticKindName(extended.stats.kind);
}

}  // namespace

const char* OptimizerProductionAnalyzeSampleMethodName(
    OptimizerProductionAnalyzeSampleMethod method) {
  switch (method) {
    case OptimizerProductionAnalyzeSampleMethod::kFullScan:
      return "full_scan";
    case OptimizerProductionAnalyzeSampleMethod::kSystemSample:
      return "system_sample";
    case OptimizerProductionAnalyzeSampleMethod::kStratifiedSample:
      return "stratified_sample";
    case OptimizerProductionAnalyzeSampleMethod::kIndexAssistedSample:
      return "index_assisted_sample";
  }
  return "system_sample";
}

std::vector<StatisticsContractStatus> ValidateProductionAnalyzeBenchmarkCleanCatalog(
    const OptimizerStatisticsCatalog& catalog,
    const OptimizerProductionAnalyzeRequest& request) {
  std::vector<StatisticsContractStatus> statuses;
  AppendBenchmarkStatuses(&statuses,
                          catalog.ValidateBenchmarkCleanInputs(
                              {"row_count", "visible_row_count", "page_count",
                               "average_row_bytes"},
                              request.relation_uuid));
  for (const auto& column : request.columns) {
    AppendBenchmarkStatuses(&statuses,
                            catalog.ValidateBenchmarkCleanInputs(
                                {"column_ndv", "column_sample_rows",
                                 "column_hll_estimated_distinct",
                                 "column_hll_relative_error_ppm",
                                 "column_correlation"},
                                column.column_uuid));
    AppendBenchmarkStatuses(&statuses,
                            catalog.ValidateBenchmarkCleanInputs(
                                {"histogram_bucket_count",
                                 "histogram_covered_fraction"},
                                column.column_uuid));
    AppendBenchmarkStatuses(&statuses,
                            catalog.ValidateBenchmarkCleanInputs(
                                {"mcv_frequency"},
                                column.column_uuid));
  }
  for (const auto& index : request.indexes) {
    if (!index.stats.route_benchmark_clean || index.stats.family_claim_removed) {
      statuses.push_back(Status(false,
                                "SB_OPT_ANALYZE_INDEX_ROUTE_NOT_BENCHMARK_CLEAN",
                                index.stats.index_uuid));
    }
    AppendBenchmarkStatuses(&statuses,
                            catalog.ValidateBenchmarkCleanInputs(
                                {"index_depth", "index_leaf_pages",
                                 "index_distinct_keys",
                                 "index_false_positive_ratio",
                                "index_route_benchmark_clean"},
                                index.stats.index_uuid));
  }
  for (const auto& expression : request.expressions) {
    AppendBenchmarkStatuses(&statuses,
                            catalog.ValidateBenchmarkCleanInputs(
                                {"expression_distinct_count",
                                 "expression_null_fraction"},
                                expression.expression_digest));
  }
  for (const auto& extended : request.extended_stats) {
    AppendBenchmarkStatuses(&statuses,
                            catalog.ValidateBenchmarkCleanInputs(
                                {"extended_multi_column_distinct_count",
                                 "extended_functional_dependency_strength",
                                 "extended_correlation_coefficient",
                                 "extended_histogram_selectivity",
                                 "extended_sampled_dependency_selectivity"},
                                ExtendedStatisticObjectKey(request, extended)));
  }
  for (const auto& filespace : request.page_filespaces) {
    AppendBenchmarkStatuses(&statuses,
                            catalog.ValidateBenchmarkCleanInputs(
                                {"filespace_available_pages",
                                 "page_family_read_latency_microseconds",
                                 "io_latency_multiplier"},
                                filespace.stats.filespace_uuid));
  }
  statuses.erase(std::remove_if(statuses.begin(), statuses.end(), [](const auto& status) {
                   return status.ok &&
                          status.diagnostic_code == "SB_OPTIMIZER_BENCHMARK_CLEAN.OK";
                 }),
                 statuses.end());
  if (statuses.empty()) {
    statuses.push_back(Status(true,
                              "SB_OPT_ANALYZE_BENCHMARK_CLEAN_OK",
                              request.relation_uuid));
  }
  return statuses;
}

OptimizerProductionAnalyzeResult RunOptimizerProductionAnalyze(
    const OptimizerProductionAnalyzeRequest& request,
    OptimizerStatisticsStore* store) {
  if (store == nullptr) {
    return Refuse("SB_OPT_ANALYZE_STORE_REQUIRED", "statistics_store_required");
  }

  OptimizerProductionAnalyzeResult result;
  result.validation_statuses = ValidateAnalyzeRequest(request);
  if (HasFailure(result.validation_statuses)) {
    result.diagnostic_code = "SB_OPT_ANALYZE_REQUEST_REFUSED";
    result.evidence.push_back("request_validation_failed=true");
    return result;
  }

  OptimizerStatisticsStore staged_store;
  PopulateStore(request, &staged_store, &result);
  result.snapshot = staged_store.Snapshot(request.analyze_run_uuid + ":snapshot");
  result.catalog_view = staged_store.ToLegacyCatalog();
  result.validation_statuses = ValidateOptimizerStatsSnapshot(result.snapshot);
  result.snapshot_valid = !HasFailure(result.validation_statuses);
  if (!result.snapshot_valid) {
    result.diagnostic_code = "SB_OPT_ANALYZE_SNAPSHOT_INVALID";
    result.evidence.push_back("snapshot_validation_failed=true");
    return result;
  }

  result.benchmark_clean_statuses =
      ValidateProductionAnalyzeBenchmarkCleanCatalog(result.catalog_view, request);
  result.benchmark_clean_ready = !HasFailure(result.benchmark_clean_statuses);
  if (request.benchmark_clean_profile && !result.benchmark_clean_ready) {
    result.diagnostic_code = "SB_OPT_ANALYZE_BENCHMARK_CLEAN_REFUSED";
    result.evidence.push_back("benchmark_clean_validation_failed=true");
    return result;
  }

  PopulateStore(request, store, nullptr);
  result.accepted = true;
  result.catalog_stats_written = true;
  result.pinned_stats_invalidated = true;
  result.diagnostic_code = "SB_OPT_ANALYZE_PRODUCTION_STATS_PUBLISHED";
  result.evidence.push_back("catalog_stats_write_authority=true");
  result.evidence.push_back("storage_scan_authority=true");
  result.evidence.push_back("mga_snapshot_authority=true");
  result.evidence.push_back("transaction_inventory_authority=true");
  result.evidence.push_back("security_redaction_bound=true");
  result.evidence.push_back("parser_or_reference_authority=false");
  result.evidence.push_back("cluster_authority=false");
  result.evidence.push_back("benchmark_clean_ready=" +
                            std::string(result.benchmark_clean_ready ? "true" : "false"));
  return result;
}

}  // namespace scratchbird::engine::optimizer

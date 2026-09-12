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
#include <limits>
#include <new>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace scratchbird::engine::optimizer {
namespace {

OptimizerProductionAnalyzeResult Refuse(std::string code, std::string evidence) {
  OptimizerProductionAnalyzeResult result;
  result.statistics_published = false;
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

StatisticsContractStatus Status(bool ok, std::string code,
                                const planner::CanonicalPlannerUuid& object) {
  auto status = Status(ok, std::move(code), std::string{});
  status.object_uuid = object;
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
  if (request.row_count == 0 || request.sampled_rows == 0) return CostConfidence::kLow;
  const double sample_fraction =
      static_cast<double>(request.sampled_rows) / static_cast<double>(request.row_count);
  if (sample_fraction >= 0.50) return CostConfidence::kHigh;
  if (sample_fraction >= 0.10) return CostConfidence::kMedium;
  return CostConfidence::kLow;
}

std::optional<OptimizerStatsIdentity> Identity(const OptimizerProductionAnalyzeRequest& request,
                                const planner::CanonicalPlannerUuid& object_uuid) {
  const auto issued = scratchbird::core::uuid::IssueRuntimeIdentityV7();
  if (!issued) return std::nullopt;
  OptimizerStatsIdentity identity;
  identity.object_uuid = object_uuid;
  identity.statistic_uuid = *issued;
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
  if (!scratchbird::core::uuid::IsEngineIdentityUuid(request.analyze_run_uuid)) {
    statuses.push_back(Status(false, "SB_OPT_ANALYZE_RUN_UUID_REQUIRED", "analyze_run_uuid"));
  }
  if (!scratchbird::core::uuid::IsEngineIdentityUuid(request.relation_uuid)) {
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
  if (request.sampled_rows > request.row_count || request.visible_row_count > request.row_count ||
      request.sample_method < OptimizerProductionAnalyzeSampleMethod::kFullScan ||
      request.sample_method > OptimizerProductionAnalyzeSampleMethod::kIndexAssistedSample ||
      (request.sample_method == OptimizerProductionAnalyzeSampleMethod::kFullScan &&
       request.sampled_rows != request.row_count)) {
    statuses.push_back(Status(false, "SB_OPT_ANALYZE_TABLE_SAMPLE_INCONSISTENT", request.relation_uuid));
  }

  const auto& authority = request.authority;
  if (!authority.engine_runtime_scope || !authority.catalog_descriptor_authority ||
      !authority.catalog_stats_write_authority || !authority.storage_scan_authority ||
      (!request.page_filespaces.empty() && !authority.page_filespace_authority) ||
      (!request.indexes.empty() && !authority.index_generation_authority) ||
      !authority.mga_snapshot_authority ||
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

  // Distribution counts describe the visible population. A sample may estimate
  // those counts; its raw sample size is retained separately. One row of
  // rounding is allowed when comparing an integer estimate to a fraction.
  const auto fraction_matches = [&](double fraction, std::uint64_t rows) {
    if (!InRange01(fraction) || rows > request.visible_row_count) return false;
    if (request.visible_row_count == 0) return fraction == 0.0 && rows == 0;
    const auto population = static_cast<double>(request.visible_row_count);
    return std::abs(fraction - static_cast<double>(rows) / population) <=
        1.0 / population + 1e-12;
  };
  for (const auto& column : request.columns) {
    if (!scratchbird::core::uuid::IsEngineIdentityUuid(column.column_uuid) ||
        column.descriptor_digest.empty()) {
      statuses.push_back(Status(false, "SB_OPT_ANALYZE_COLUMN_DESCRIPTOR_REQUIRED", request.relation_uuid));
    }
    if (column.sample_rows > request.sampled_rows || column.sample_rows > request.visible_row_count ||
        (request.visible_row_count != 0 && column.sample_rows == 0) ||
        (IsFullScan(request) && column.sample_rows != request.visible_row_count)) {
      statuses.push_back(Status(false, "SB_OPT_ANALYZE_COLUMN_SAMPLE_INVALID", column.column_uuid));
    }
    if (!InRange01(column.hyperloglog_relative_error) ||
        (column.hyperloglog_register_count == 0 && column.hyperloglog_estimated_distinct != 0) ||
        (column.hyperloglog_register_count != 0 && column.hyperloglog_estimated_distinct == 0 &&
         column.distinct_count != 0)) {
      statuses.push_back(Status(false, "SB_OPT_ANALYZE_COLUMN_HLL_REQUIRED", column.column_uuid));
    }
    if (!fraction_matches(column.null_fraction, column.null_count) ||
        !std::isfinite(column.correlation) || column.correlation < -1.0 || column.correlation > 1.0 ||
        column.null_count > request.visible_row_count ||
        column.distinct_count > request.visible_row_count - std::min(column.null_count, request.visible_row_count)) {
      statuses.push_back(Status(false, "SB_OPT_ANALYZE_COLUMN_STATS_INVALID", column.column_uuid));
    }
    const auto nonnull_rows = request.visible_row_count - std::min(column.null_count, request.visible_row_count);
    if (nonnull_rows == 0 && (!column.min_encoded.empty() || !column.max_encoded.empty() ||
        !column.histogram_buckets.empty() || !column.mcv_values.empty() ||
        column.correlation != 0.0 || column.hyperloglog_estimated_distinct != 0)) {
      statuses.push_back(Status(false, "SB_OPT_ANALYZE_COLUMN_STATS_INVALID", column.column_uuid));
    }
    std::uint64_t histogram_rows = 0, mcv_rows = 0;
    double histogram_fraction = 0.0, mcv_frequency = 0.0;
    for (const auto& bucket : column.histogram_buckets) {
      if (!fraction_matches(bucket.fraction, bucket.row_count) ||
          bucket.row_count > nonnull_rows - std::min(histogram_rows, nonnull_rows)) {
        statuses.push_back(Status(false, "SB_OPT_ANALYZE_HISTOGRAM_BUCKET_INVALID", column.column_uuid));
      } else histogram_rows += bucket.row_count;
      histogram_fraction += bucket.fraction;
    }
    if (!std::isfinite(histogram_fraction) || histogram_fraction > 1.0 - column.null_fraction + 1e-6) {
      statuses.push_back(Status(false, "SB_OPT_ANALYZE_HISTOGRAM_FRACTION_INVALID", column.column_uuid));
    }
    for (const auto& mcv : column.mcv_values) {
      if (!fraction_matches(mcv.frequency, mcv.row_count) ||
          mcv.row_count > nonnull_rows - std::min(mcv_rows, nonnull_rows)) {
        statuses.push_back(Status(false, "SB_OPT_ANALYZE_MCV_VALUE_INVALID", column.column_uuid));
      } else mcv_rows += mcv.row_count;
      mcv_frequency += mcv.frequency;
    }
    if (!std::isfinite(mcv_frequency) || mcv_frequency > 1.0 - column.null_fraction + 1e-6) {
      statuses.push_back(Status(false, "SB_OPT_ANALYZE_MCV_FREQUENCY_INVALID", column.column_uuid));
    }
  }
  for (const auto& expression : request.expressions) {
    if (expression.expression_digest.empty() || expression.descriptor_digest.empty()) {
      statuses.push_back(Status(false, "SB_OPT_ANALYZE_EXPRESSION_DESCRIPTOR_REQUIRED", request.relation_uuid));
    }
    if (expression.distinct_count > request.visible_row_count || !InRange01(expression.null_fraction) ||
        (request.visible_row_count == 0 && expression.null_fraction != 0.0)) {
      statuses.push_back(Status(false, "SB_OPT_ANALYZE_EXPRESSION_STATS_INVALID", expression.expression_digest));
    }
  }
  const auto valid_provider_identity = [&](const OptimizerStatsIdentity& id,
                                           const planner::CanonicalPlannerUuid& object) {
    return OptimizerStatsIdentityIsUsable(id) && id.object_uuid == object &&
        id.catalog_epoch == request.catalog_epoch && id.stats_epoch == request.stats_epoch &&
        id.transaction_visibility_epoch == request.transaction_visibility_epoch &&
        (id.source == StatisticSource::kCatalogExact || id.source == StatisticSource::kCatalogSample ||
         (id.source == StatisticSource::kRuntimeMetric && authority.metric_generation_authority));
  };

  for (const auto& extended : request.extended_stats) {
    if (!extended.catalog_descriptor_proven || extended.stats.relation_uuid != request.relation_uuid ||
        !valid_provider_identity(extended.stats.identity, request.relation_uuid)) {
      statuses.push_back(Status(false, "SB_OPT_ANALYZE_EXTENDED_DESCRIPTOR_REQUIRED", extended.stats.identity.statistic_uuid));
    }
    if (extended.stats.finality_authority ||
        !extended.stats.mga_visibility_recheck_required ||
        !extended.stats.security_recheck_required) {
      statuses.push_back(Status(false, "SB_OPT_ANALYZE_EXTENDED_AUTHORITY_INVALID", extended.stats.identity.statistic_uuid));
    }
  }

  for (const auto& index : request.indexes) {
    if (!index.captured_from_index_provider || !index.route_capability_proven ||
        index.stats.relation_uuid != request.relation_uuid ||
        !valid_provider_identity(index.stats.identity, request.relation_uuid)) {
      statuses.push_back(Status(false, "SB_OPT_ANALYZE_INDEX_PROVIDER_REQUIRED", index.stats.index_uuid));
    }
  }

  for (const auto& filespace : request.page_filespaces) {
    if (!filespace.captured_from_page_manager ||
        !valid_provider_identity(filespace.stats.identity, filespace.stats.filespace_uuid)) {
      statuses.push_back(Status(false, "SB_OPT_ANALYZE_FILESPACE_PROVIDER_REQUIRED", filespace.stats.filespace_uuid));
    }
  }

  if (statuses.empty()) {
    statuses.push_back(Status(true, "SB_OPT_ANALYZE_REQUEST_OK", request.relation_uuid));
  }
  return statuses;
}

std::optional<OptimizerStatsSnapshot> BuildSnapshot(
    const OptimizerProductionAnalyzeRequest& request) {
  OptimizerStatsSnapshot snapshot;
  const auto issued = scratchbird::core::uuid::IssueRuntimeIdentityV7();
  if (!issued) return std::nullopt;
  snapshot.snapshot_id = *issued;
  snapshot.catalog_epoch = request.catalog_epoch;
  snapshot.stats_epoch = request.stats_epoch;
  snapshot.collection = {request.analyze_run_uuid, request.descriptor_set_digest,
      request.storage_scan_evidence_digest, request.sample_provenance_digest,
      request.result_contract_hash, OptimizerProductionAnalyzeSampleMethodName(request.sample_method),
      request.sampled_rows, request.source_generation, request.transaction_visibility_epoch,
      request.security_epoch, request.redaction_epoch, request.resource_epoch};
  TableCardinalityStats table;
  auto identity = Identity(request, request.relation_uuid);
  if (!identity) return std::nullopt;
  table.identity = *identity;
  table.row_count = request.row_count;
  table.visible_row_count = request.visible_row_count;
  table.page_count = request.page_count;
  table.average_row_bytes = request.average_row_bytes;
  snapshot.tables.push_back(std::move(table));

  for (const auto& input : request.columns) {
    ColumnStats column;
    identity = Identity(request, request.relation_uuid);
    if (!identity) return std::nullopt;
    column.identity = *identity;
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
    snapshot.columns.push_back(std::move(column));
    if (!input.histogram_buckets.empty()) {
      HistogramStats histogram;
      identity = Identity(request, request.relation_uuid);
      if (!identity) return std::nullopt;
      histogram.identity = *identity;
      histogram.column_uuid = input.column_uuid;
      histogram.buckets = input.histogram_buckets;
      snapshot.histograms.push_back(std::move(histogram));
    }
    for (const auto& value : input.mcv_values) {
      MostCommonValueStats mcv;
      identity = Identity(request, request.relation_uuid);
      if (!identity) return std::nullopt;
      mcv.identity = *identity;
      mcv.column_uuid = input.column_uuid;
      mcv.value_encoded = value.value_encoded;
      mcv.frequency = value.frequency;
      mcv.row_count = value.row_count;
      snapshot.mcv.push_back(std::move(mcv));
    }
  }
  for (const auto& input : request.expressions) {
    ExpressionStats expression;
    identity = Identity(request, request.relation_uuid);
    if (!identity) return std::nullopt;
    expression.identity = *identity;
    expression.expression_digest = input.expression_digest;
    expression.descriptor_digest = input.descriptor_digest;
    expression.distinct_count = input.distinct_count;
    expression.null_fraction = input.null_fraction;
    snapshot.expressions.push_back(std::move(expression));
  }
  // Provider-owned measurements keep their identity, epochs, source and
  // uncertainty. Never relabel an old/default record as a fresh catalog scan.
  for (const auto& input : request.extended_stats) snapshot.extended_stats.push_back(input.stats);
  for (const auto& input : request.indexes) snapshot.indexes.push_back(input.stats);
  for (const auto& input : request.page_filespaces) snapshot.page_filespaces.push_back(input.stats);
  return snapshot;
}

void AppendBenchmarkStatuses(std::vector<StatisticsContractStatus>* target,
                             std::vector<StatisticsContractStatus> statuses) {
  if (target == nullptr) return;
  target->insert(target->end(), statuses.begin(), statuses.end());
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
  return "unknown";
}

std::vector<StatisticsContractStatus> ValidateProductionAnalyzeBenchmarkCleanCatalog(
    const OptimizerStatisticsCatalog& catalog,
    const OptimizerStatsSnapshot& snapshot) {
  std::vector<StatisticsContractStatus> statuses;
  if (snapshot.tables.size() != 1) return {Status(false, "SB_OPT_ANALYZE_SNAPSHOT_INVALID", "table snapshot required")};
  AppendBenchmarkStatuses(&statuses,
                          catalog.ValidateBenchmarkCleanInputs(
                              {"row_count", "visible_row_count", "page_count",
                               "average_row_bytes"},
                              OptimizerStatisticTarget::Object(snapshot.tables.front().identity.object_uuid)));
  for (const auto& column : snapshot.columns) {
    AppendBenchmarkStatuses(&statuses,
                            catalog.ValidateBenchmarkCleanInputs(
                                {"column_ndv", "column_sample_rows", "column_correlation"},
                                OptimizerStatisticTarget::Object(column.column_uuid)));
    if (column.hyperloglog_register_count != 0) {
      AppendBenchmarkStatuses(&statuses, catalog.ValidateBenchmarkCleanInputs(
          {"column_hll_estimated_distinct", "column_hll_relative_error_ppm"},
          OptimizerStatisticTarget::Object(column.column_uuid)));
    }
  }
  for (const auto& histogram : snapshot.histograms) {
    AppendBenchmarkStatuses(&statuses, catalog.ValidateBenchmarkCleanInputs(
        {"histogram_bucket_count", "histogram_covered_fraction"},
        OptimizerStatisticTarget::Object(histogram.column_uuid)));
  }
  for (const auto& mcv : snapshot.mcv) {
    AppendBenchmarkStatuses(&statuses, catalog.ValidateBenchmarkCleanInputs(
        {"mcv_frequency"}, OptimizerStatisticTarget::Object(mcv.identity.statistic_uuid)));
  }
  for (const auto& index : snapshot.indexes) {
    if (!index.route_benchmark_clean || index.family_claim_removed) {
      statuses.push_back(Status(false,
                                "SB_OPT_ANALYZE_INDEX_ROUTE_NOT_BENCHMARK_CLEAN",
                                index.index_uuid));
    }
    AppendBenchmarkStatuses(&statuses,
                            catalog.ValidateBenchmarkCleanInputs(
                                {"index_depth", "index_leaf_pages",
                                 "index_distinct_keys",
                                 "index_false_positive_ratio",
                                "index_route_benchmark_clean"},
                                OptimizerStatisticTarget::Object(index.index_uuid)));
  }
  for (const auto& expression : snapshot.expressions) {
    AppendBenchmarkStatuses(&statuses,
                            catalog.ValidateBenchmarkCleanInputs(
                                {"expression_distinct_count",
                                 "expression_null_fraction"},
                                OptimizerStatisticTarget::Object(expression.identity.statistic_uuid)));
  }
  for (const auto& extended : snapshot.extended_stats) {
    std::vector<std::string> names;
    switch (extended.kind) {
      case ExtendedOptimizerStatisticKind::kMultiColumnNdv:
      case ExtendedOptimizerStatisticKind::kJointMcv:
        names = {"extended_multi_column_distinct_count"}; break;
      case ExtendedOptimizerStatisticKind::kFunctionalDependency:
        names = {"extended_functional_dependency_strength"}; break;
      case ExtendedOptimizerStatisticKind::kCrossColumnCorrelation:
        names = {"extended_correlation_coefficient"}; break;
      case ExtendedOptimizerStatisticKind::kMultiColumnHistogram:
        names = {"extended_histogram_selectivity"}; break;
      case ExtendedOptimizerStatisticKind::kSampledDependency:
        names = {"extended_sampled_dependency_selectivity"}; break;
      case ExtendedOptimizerStatisticKind::kDocumentPathBridge:
        names = {"extended_multi_column_distinct_count", "extended_histogram_selectivity",
                 "extended_sampled_dependency_selectivity"}; break;
      case ExtendedOptimizerStatisticKind::kFkPkJoinCardinality:
        // Exact join cardinality lives in the typed snapshot, not an invented
        // scalar selectivity. Benchmark eligibility still requires provenance.
        if (extended.identity.source != StatisticSource::kCatalogExact &&
            extended.identity.source != StatisticSource::kCatalogSample)
          statuses.push_back(Status(false, "SB_OPT_ANALYZE_BENCHMARK_CLEAN_REFUSED", extended.identity.statistic_uuid));
        break;
    }
    if (!names.empty()) AppendBenchmarkStatuses(&statuses, catalog.ValidateBenchmarkCleanInputs(
        names, OptimizerStatisticTarget::Object(extended.identity.statistic_uuid)));
  }
  for (const auto& filespace : snapshot.page_filespaces) {
    AppendBenchmarkStatuses(&statuses,
                            catalog.ValidateBenchmarkCleanInputs(
                                {"filespace_available_pages",
                                 "page_family_read_latency_microseconds",
                                 "io_latency_multiplier"},
                                OptimizerStatisticTarget::Object(filespace.identity.statistic_uuid)));
  }
  statuses.erase(std::remove_if(statuses.begin(), statuses.end(), [](const auto& status) {
                   return status.ok &&
                          status.diagnostic_code == "SB_OPTIMIZER_BENCHMARK_CLEAN.OK";
                 }),
                 statuses.end());
  if (statuses.empty()) {
    statuses.push_back(Status(true,
                              "SB_OPT_ANALYZE_BENCHMARK_CLEAN_OK",
                              snapshot.tables.front().identity.object_uuid));
  }
  return statuses;
}

OptimizerProductionAnalyzeResult RunOptimizerProductionAnalyze(
    const OptimizerProductionAnalyzeRequest& request,
    OptimizerStatisticsStore* store) try {
  if (!store) return Refuse("SB_OPT_ANALYZE_STORE_REQUIRED", "statistics_store_required");
  OptimizerProductionAnalyzeResult result;
  result.validation_statuses = ValidateAnalyzeRequest(request);
  if (HasFailure(result.validation_statuses)) {
    result.diagnostic_code = "SB_OPT_ANALYZE_REQUEST_REFUSED";
    return result;
  }
  auto staged = BuildSnapshot(request);
  if (!staged) return Refuse("SB_OPT_ANALYZE_SNAPSHOT_INVALID", "Core identity issuance failed");
  result.validation_statuses = ValidateOptimizerStatsSnapshot(*staged);
  if (HasFailure(result.validation_statuses)) {
    result.diagnostic_code = "SB_OPT_ANALYZE_SNAPSHOT_INVALID";
    return result;
  }
  auto catalog = ProjectOptimizerStatsSnapshot(*staged);
  if (!catalog) return Refuse("SB_OPT_ANALYZE_SNAPSHOT_INVALID", "statistics projection unavailable");
  result.benchmark_clean_statuses = ValidateProductionAnalyzeBenchmarkCleanCatalog(*catalog, *staged);
  const bool benchmark_ready = !HasFailure(result.benchmark_clean_statuses);
  if (request.benchmark_clean_profile && !benchmark_ready) {
    result.diagnostic_code = "SB_OPT_ANALYZE_BENCHMARK_CLEAN_REFUSED";
    return result;
  }

  // Response allocations precede the only live publication. No durable catalog
  // write, row collection or SQL completion is asserted by this in-memory API.
  static_assert(std::is_nothrow_move_assignable_v<decltype(result.catalog_view)>);
  result.diagnostic_code = "SB_OPT_ANALYZE_PRODUCTION_STATS_PUBLISHED";
  result.evidence = {"in_memory_statistics_publication=true", "durable_catalog_write_performed=false",
      "storage_scan_performed=false", "mga_finality_authority=false", "cluster_authority=false"};
  auto publication = store->PublishRelationSnapshot(std::move(*staged));
  result.publication_status = publication.status;
  if (publication.status != OptimizerStatsPublicationStatus::kPublished) {
    result.evidence.clear();
    result.diagnostic_code.clear();
    return result;
  }
  result.statistics_published = true;
  result.snapshot_valid = true;
  result.benchmark_clean_ready = benchmark_ready;
  result.pinned_stats_invalidated = publication.invalidated_snapshots != 0;
  result.snapshot = std::move(publication.snapshot);
  result.catalog_view = std::move(catalog);
  result.table_stats_written = result.snapshot->tables.size();
  result.column_stats_written = result.snapshot->columns.size();
  result.histogram_stats_written = result.snapshot->histograms.size();
  result.mcv_stats_written = result.snapshot->mcv.size();
  result.expression_stats_written = result.snapshot->expressions.size();
  result.extended_stats_written = result.snapshot->extended_stats.size();
  result.index_stats_written = result.snapshot->indexes.size();
  result.page_filespace_stats_written = result.snapshot->page_filespaces.size();
  return result;
} catch (const std::bad_alloc&) {
  OptimizerProductionAnalyzeResult result;
  result.publication_status = OptimizerStatsPublicationStatus::kResourceExhausted;
  return result;
} catch (const std::length_error&) {
  OptimizerProductionAnalyzeResult result;
  result.publication_status = OptimizerStatsPublicationStatus::kResourceExhausted;
  return result;
}

}  // namespace scratchbird::engine::optimizer

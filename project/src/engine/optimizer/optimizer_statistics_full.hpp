// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "statistics_catalog.hpp"

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace scratchbird::engine::optimizer {

// SEARCH_KEY: SB_OPTIMIZER_STATISTICS_FULL_MODEL
// UUID-scoped, freshness-aware optimizer statistics. These structures are the
// implementation model used by the non-cluster optimizer; catalog persistence
// hooks serialize the same fields.
enum class OptimizerStatsFreshnessState {
  kFresh,
  kStale,
  kInvalid,
  kMissing,
};

struct OptimizerStatsIdentity {
  std::string object_uuid;
  std::string statistic_uuid;
  std::uint64_t stats_epoch = 0;
  std::uint64_t catalog_epoch = 0;
  std::uint64_t transaction_visibility_epoch = 0;
  OptimizerStatsFreshnessState freshness = OptimizerStatsFreshnessState::kMissing;
  StatisticSource source = StatisticSource::kUnavailable;
  CostConfidence confidence = CostConfidence::kUnknown;
};

struct TableCardinalityStats {
  OptimizerStatsIdentity identity;
  std::uint64_t row_count = 0;
  std::uint64_t visible_row_count = 0;
  std::uint64_t page_count = 0;
  std::uint64_t average_row_bytes = 0;
};

struct ColumnStats {
  OptimizerStatsIdentity identity;
  std::string column_uuid;
  std::string descriptor_digest;
  // SEARCH_KEY: OPCH_STATISTICS_ANALYZE_PARITY
  // Sampling and HLL evidence are optimizer estimates only and never replace
  // base-row MGA/security rechecks.
  std::string sample_method;
  std::string sample_provenance_digest;
  std::uint64_t sample_rows = 0;
  std::uint64_t hyperloglog_register_count = 0;
  std::uint64_t hyperloglog_estimated_distinct = 0;
  double hyperloglog_relative_error = 0.0;
  std::uint64_t null_count = 0;
  std::uint64_t distinct_count = 0;
  double null_fraction = 0.0;
  double correlation = 0.0;
  std::uint64_t average_width_bytes = 0;
  std::string min_encoded;
  std::string max_encoded;
};

struct HistogramBucketStats {
  std::string lower_bound_encoded;
  std::string upper_bound_encoded;
  double fraction = 0.0;
  std::uint64_t row_count = 0;
};

struct HistogramStats {
  OptimizerStatsIdentity identity;
  std::string column_uuid;
  std::vector<HistogramBucketStats> buckets;
};

struct MostCommonValueStats {
  OptimizerStatsIdentity identity;
  std::string column_uuid;
  std::string value_encoded;
  double frequency = 0.0;
};

// SEARCH_KEY: SB_OPTIMIZER_EXTENDED_STATS_ODFR_010
// Advisory multi-column and document-path optimizer statistics. These refine
// cost/selectivity estimates only; MGA visibility and security rechecks remain
// required and transaction finality remains engine-owned.
enum class ExtendedOptimizerStatisticKind {
  kMultiColumnNdv,
  kJointMcv,
  kFunctionalDependency,
  kCrossColumnCorrelation,
  kMultiColumnHistogram,
  kSampledDependency,
  kFkPkJoinCardinality,
  kDocumentPathBridge,
};

struct ExtendedOptimizerJointMcvEntry {
  std::vector<std::string> value_encodings;
  double frequency = 0.0;
};

struct ExtendedOptimizerStatistic {
  OptimizerStatsIdentity identity;
  ExtendedOptimizerStatisticKind kind = ExtendedOptimizerStatisticKind::kMultiColumnNdv;
  std::string relation_uuid;
  std::vector<std::string> column_uuids;
  std::vector<std::string> document_path_digests;
  std::vector<ExtendedOptimizerJointMcvEntry> joint_mcv;
  std::uint64_t multi_column_distinct_count = 0;
  double functional_dependency_strength = 0.0;
  double correlation_coefficient = 0.0;
  double histogram_selectivity = 0.0;
  double sampled_dependency_selectivity = 0.0;
  double observed_selectivity_error = -1.0;
  std::uint64_t fk_pk_estimated_rows = 0;
  bool fk_pk_shortcut = false;
  bool mga_visibility_recheck_required = true;
  bool security_recheck_required = true;
  bool finality_authority = false;
};

struct IndexStats {
  OptimizerStatsIdentity identity;
  std::string index_uuid;
  std::string relation_uuid;
  std::string index_family = "btree";
  std::string descriptor_digest;
  std::string collation_identity;
  std::vector<std::string> key_column_uuids;
  std::vector<std::string> key_expression_digests;
  std::vector<std::string> covered_column_uuids;
  std::string generated_column_expression_digest;
  std::string computed_expression_digest;
  std::string partial_predicate_text;
  std::string partial_predicate_digest;
  std::string function_volatility = "immutable";
  bool partial_predicate_immutable = true;
  bool partial_predicate_security_safe = true;
  bool descriptor_epoch_valid = true;
  bool resource_epoch_valid = true;
  bool collation_epoch_valid = true;
  bool function_epoch_valid = true;
  bool unique = false;
  bool covering = false;
  bool partial = false;
  bool expression_index = false;
  bool like_prefix_capable = false;
  bool collation_deterministic = true;
  std::uint64_t height = 0;
  std::uint64_t leaf_pages = 0;
  std::uint64_t distinct_keys = 0;
  double clustering_factor = 1.0;
  double fragmentation_ratio = 0.0;
  double visibility_coverage = 1.0;
  double predicate_coverage = 1.0;
  double contention_ratio = 0.0;
  bool rebuild_in_progress = false;
  // SEARCH_KEY: OPCH_INDEX_FAMILY_COST_STATISTICS_COVERAGE
  bool equality_lookup_supported = false;
  bool ordered_range_supported = false;
  bool negative_prune_supported = false;
  bool candidate_set_producer = false;
  bool exact_recheck_required = true;
  bool mga_recheck_required = true;
  bool security_recheck_required = true;
  double false_positive_ratio = 0.0;
  bool route_benchmark_clean = false;
  bool family_claim_removed = false;
};

struct ExpressionStats {
  OptimizerStatsIdentity identity;
  std::string expression_digest;
  std::string descriptor_digest;
  std::uint64_t distinct_count = 0;
  double null_fraction = 0.0;
};

struct PageFilespaceStats {
  OptimizerStatsIdentity identity;
  std::string filespace_uuid;
  std::string page_family;
  std::uint64_t page_size_bytes = 0;
  std::uint64_t free_pages = 0;
  double sequential_latency_score = 1.0;
  double random_latency_score = 4.0;
  double health_score = 1.0;
  bool degraded = false;
};

struct OptimizerStatsSnapshot {
  std::string snapshot_id;
  std::uint64_t stats_epoch = 0;
  std::uint64_t catalog_epoch = 0;
  std::vector<TableCardinalityStats> tables;
  std::vector<ColumnStats> columns;
  std::vector<HistogramStats> histograms;
  std::vector<MostCommonValueStats> mcv;
  std::vector<ExtendedOptimizerStatistic> extended_stats;
  std::vector<IndexStats> indexes;
  std::vector<ExpressionStats> expressions;
  std::vector<PageFilespaceStats> page_filespaces;
};

struct StatsInvalidationEvent {
  std::string event_kind;
  std::string object_uuid;
  std::string index_uuid;
  std::string security_policy_identity;
  std::string redaction_policy_identity;
  std::uint64_t new_catalog_epoch = 0;
  std::uint64_t new_stats_epoch = 0;
  std::string reason;
};

struct AnalyzeSampleInput {
  std::string relation_uuid;
  std::uint64_t sampled_rows = 0;
  std::uint64_t total_rows_estimate = 0;
  std::uint64_t page_count = 0;
  std::uint64_t average_row_bytes = 0;
  std::uint64_t stats_epoch = 0;
  std::uint64_t catalog_epoch = 0;
};

class OptimizerStatisticsStore {
 public:
  void UpsertTable(TableCardinalityStats stats);
  void UpsertColumn(ColumnStats stats);
  void UpsertHistogram(HistogramStats stats);
  void UpsertMcv(MostCommonValueStats stats);
  void UpsertExtendedStatistic(ExtendedOptimizerStatistic stats);
  void UpsertIndex(IndexStats stats);
  void UpsertExpression(ExpressionStats stats);
  void UpsertPageFilespace(PageFilespaceStats stats);

  std::optional<TableCardinalityStats> FindTable(const std::string& relation_uuid) const;
  std::optional<ColumnStats> FindColumn(const std::string& relation_uuid, const std::string& column_uuid) const;
  std::vector<ExtendedOptimizerStatistic> FindExtendedStatisticsForRelation(
      const std::string& relation_uuid) const;
  std::optional<IndexStats> FindIndex(const std::string& index_uuid) const;
  std::optional<PageFilespaceStats> FindFilespace(const std::string& filespace_uuid, const std::string& page_family) const;

  void MarkStaleByObject(const std::string& object_uuid, std::uint64_t catalog_epoch);
  OptimizerStatsSnapshot Snapshot(std::string snapshot_id) const;
  OptimizerStatisticsCatalog ToLegacyCatalog() const;

 private:
  std::vector<TableCardinalityStats> tables_;
  std::vector<ColumnStats> columns_;
  std::vector<HistogramStats> histograms_;
  std::vector<MostCommonValueStats> mcv_;
  std::vector<ExtendedOptimizerStatistic> extended_stats_;
  std::vector<IndexStats> indexes_;
  std::vector<ExpressionStats> expressions_;
  std::vector<PageFilespaceStats> page_filespaces_;
};

// SEARCH_KEY: SB_OPTIMIZER_PINNED_STATS_DESCRIPTOR_CACHE_ODF_021
// Read-only pinned statistics snapshots used by optimizer hot paths. They are
// cost metadata only and never become visibility/finality authority.
struct OptimizerPinnedStatsDescriptorKey {
  std::uint64_t catalog_epoch = 0;
  std::uint64_t security_epoch = 0;
  std::uint64_t resource_policy_epoch = 0;
  std::uint64_t name_resolution_epoch = 0;
  std::uint64_t stats_epoch = 0;
  std::string descriptor_set_digest;
  std::vector<std::string> object_uuids;
  std::vector<std::string> index_uuids;
  std::string security_policy_identity;
  std::string redaction_policy_identity;
};

struct OptimizerPinnedStatsDescriptorSnapshot {
  OptimizerPinnedStatsDescriptorKey key;
  OptimizerStatsSnapshot stats_snapshot;
  bool read_only_snapshot = true;
  bool mga_visibility_recheck_required = true;
  bool security_recheck_required = true;
  bool finality_authority_cached = false;
};

struct OptimizerPinnedStatsLookupResult {
  bool ok = false;
  bool cache_hit = false;
  std::string diagnostic_code;
  std::string detail;
  std::string cache_key;
  std::shared_ptr<const OptimizerPinnedStatsDescriptorSnapshot> snapshot;
};

struct OptimizerPinnedStatsInvalidatedEntry {
  std::string cache_key;
  std::string reason;
  std::vector<std::string> object_uuids;
  std::vector<std::string> index_uuids;
};

struct OptimizerPinnedStatsInvalidationResult {
  std::vector<OptimizerPinnedStatsInvalidatedEntry> invalidated_entries;
};

class OptimizerPinnedStatsDescriptorCache {
 public:
  OptimizerPinnedStatsLookupResult Put(OptimizerPinnedStatsDescriptorSnapshot snapshot);
  OptimizerPinnedStatsLookupResult Lookup(const OptimizerPinnedStatsDescriptorKey& key);
  OptimizerPinnedStatsInvalidationResult Invalidate(const StatsInvalidationEvent& event);
  void Clear();

 private:
  mutable std::mutex mutex_;
  std::map<std::string, std::shared_ptr<const OptimizerPinnedStatsDescriptorSnapshot>> snapshots_;
};

std::string OptimizerPinnedStatsDescriptorCacheKeyText(const OptimizerPinnedStatsDescriptorKey& key);
OptimizerPinnedStatsLookupResult ValidateOptimizerPinnedStatsDescriptorKey(
    const OptimizerPinnedStatsDescriptorKey& key);
OptimizerPinnedStatsDescriptorCache& GlobalOptimizerPinnedStatsDescriptorCache();

const char* OptimizerStatsFreshnessStateName(OptimizerStatsFreshnessState state);
bool OptimizerStatsIdentityIsUsable(const OptimizerStatsIdentity& identity);
const char* ExtendedOptimizerStatisticKindName(ExtendedOptimizerStatisticKind kind);
TableCardinalityStats BuildTableStatsFromAnalyzeSample(const AnalyzeSampleInput& input);
std::vector<StatisticsContractStatus> ValidateOptimizerStatsSnapshot(const OptimizerStatsSnapshot& snapshot);
double EstimateEqualitySelectivityFromColumnStats(const ColumnStats& stats, std::uint64_t table_rows);
double EstimateRangeSelectivityFromHistogram(const HistogramStats& stats);
CostVector ApplyIndexHealthCostAdjustment(CostVector cost, const IndexStats& stats);
std::vector<StatisticsContractStatus> ValidateIndexFamilyCostCoverage(
    const std::vector<IndexStats>& indexes);

}  // namespace scratchbird::engine::optimizer

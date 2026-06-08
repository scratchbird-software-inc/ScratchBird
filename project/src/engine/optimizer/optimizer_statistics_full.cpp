// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "optimizer_statistics_full.hpp"

#include <algorithm>
#include <sstream>
#include <utility>

namespace scratchbird::engine::optimizer {
namespace {

template <typename T, typename Pred>
void Upsert(std::vector<T>* values, T item, Pred pred) {
  auto it = std::find_if(values->begin(), values->end(), pred);
  if (it == values->end()) values->push_back(std::move(item));
  else *it = std::move(item);
}

StatisticsContractStatus Status(bool ok, std::string code, std::string detail) {
  StatisticsContractStatus status;
  status.ok = ok;
  status.diagnostic_code = std::move(code);
  status.detail = std::move(detail);
  return status;
}

void ValidateIdentity(const OptimizerStatsIdentity& identity,
                      const char* family,
                      std::vector<StatisticsContractStatus>* statuses) {
  if (identity.object_uuid.empty()) statuses->push_back(Status(false, "SB_OPT_STATS_OBJECT_UUID_REQUIRED", family));
  if (identity.statistic_uuid.empty()) statuses->push_back(Status(false, "SB_OPT_STATS_STATISTIC_UUID_REQUIRED", family));
  if (identity.stats_epoch == 0) statuses->push_back(Status(false, "SB_OPT_STATS_EPOCH_REQUIRED", family));
  if (!OptimizerStatsIdentityIsUsable(identity)) statuses->push_back(Status(false, "SB_OPT_STATS_NOT_USABLE", family));
}

std::vector<std::string> SortedUnique(std::vector<std::string> values) {
  std::sort(values.begin(), values.end());
  values.erase(std::unique(values.begin(), values.end()), values.end());
  return values;
}

void AppendSorted(std::ostringstream& out, const char* label, std::vector<std::string> values) {
  out << '|' << label << '=';
  for (const auto& value : SortedUnique(std::move(values))) out << value << ';';
}

OptimizerPinnedStatsLookupResult StatsRefusal(std::string code,
                                              std::string detail,
                                              std::string cache_key = {}) {
  OptimizerPinnedStatsLookupResult result;
  result.ok = false;
  result.cache_hit = false;
  result.diagnostic_code = std::move(code);
  result.detail = std::move(detail);
  result.cache_key = std::move(cache_key);
  return result;
}

bool Contains(const std::vector<std::string>& values, const std::string& value) {
  return !value.empty() &&
         std::find(values.begin(), values.end(), value) != values.end();
}

bool StatsEventInvalidatesAll(const StatsInvalidationEvent& event) {
  return event.event_kind == "catalog_epoch" ||
         event.event_kind == "security_epoch" ||
         event.event_kind == "policy_epoch" ||
         event.event_kind == "resource_epoch" ||
         event.event_kind == "name_resolution_epoch" ||
         event.event_kind == "stats_epoch" ||
         event.event_kind == "stats_refresh" ||
         event.event_kind == "statistics_refresh" ||
         event.event_kind == "storage_metric_generation" ||
         event.event_kind == "runtime_metric_generation" ||
         event.event_kind == "redaction_epoch" ||
         event.event_kind == "redaction_policy_epoch";
}

bool StatsEventInvalidatesSnapshot(const OptimizerPinnedStatsDescriptorSnapshot& snapshot,
                                   const StatsInvalidationEvent& event) {
  if (StatsEventInvalidatesAll(event)) return true;
  if (!event.object_uuid.empty() && Contains(snapshot.key.object_uuids, event.object_uuid)) return true;
  if (!event.index_uuid.empty() && Contains(snapshot.key.index_uuids, event.index_uuid)) return true;
  if (!event.security_policy_identity.empty() &&
      event.security_policy_identity == snapshot.key.security_policy_identity) {
    return true;
  }
  if (!event.redaction_policy_identity.empty() &&
      event.redaction_policy_identity == snapshot.key.redaction_policy_identity) {
    return true;
  }
  return (event.event_kind == "catalog_alter" ||
          event.event_kind == "catalog_drop" ||
          event.event_kind == "index_change" ||
          event.event_kind == "index_generation" ||
          event.event_kind == "analyze_generation" ||
          event.event_kind == "security_policy_change" ||
          event.event_kind == "redaction_policy_change" ||
          event.event_kind == "statistics_stale") &&
         event.object_uuid.empty() &&
         event.index_uuid.empty() &&
         event.security_policy_identity.empty() &&
         event.redaction_policy_identity.empty();
}

void InvalidateGlobalPinnedStatsCache(std::string event_kind,
                                      std::string object_uuid,
                                      std::string index_uuid,
                                      std::uint64_t new_catalog_epoch,
                                      std::uint64_t new_stats_epoch,
                                      std::string reason) {
  StatsInvalidationEvent event;
  event.event_kind = std::move(event_kind);
  event.object_uuid = std::move(object_uuid);
  event.index_uuid = std::move(index_uuid);
  event.new_catalog_epoch = new_catalog_epoch;
  event.new_stats_epoch = new_stats_epoch;
  event.reason = std::move(reason);
  (void)GlobalOptimizerPinnedStatsDescriptorCache().Invalidate(event);
}

}  // namespace

const char* OptimizerStatsFreshnessStateName(OptimizerStatsFreshnessState state) {
  switch (state) {
    case OptimizerStatsFreshnessState::kFresh: return "fresh";
    case OptimizerStatsFreshnessState::kStale: return "stale";
    case OptimizerStatsFreshnessState::kInvalid: return "invalid";
    case OptimizerStatsFreshnessState::kMissing: return "missing";
  }
  return "missing";
}

bool OptimizerStatsIdentityIsUsable(const OptimizerStatsIdentity& identity) {
  return !identity.object_uuid.empty() &&
         !identity.statistic_uuid.empty() &&
         identity.stats_epoch != 0 &&
         identity.freshness == OptimizerStatsFreshnessState::kFresh &&
         identity.source != StatisticSource::kUnavailable &&
         identity.confidence != CostConfidence::kRejected;
}

const char* ExtendedOptimizerStatisticKindName(ExtendedOptimizerStatisticKind kind) {
  switch (kind) {
    case ExtendedOptimizerStatisticKind::kMultiColumnNdv: return "multi_column_ndv";
    case ExtendedOptimizerStatisticKind::kJointMcv: return "joint_mcv";
    case ExtendedOptimizerStatisticKind::kFunctionalDependency: return "functional_dependency";
    case ExtendedOptimizerStatisticKind::kCrossColumnCorrelation: return "cross_column_correlation";
    case ExtendedOptimizerStatisticKind::kMultiColumnHistogram: return "multi_column_histogram";
    case ExtendedOptimizerStatisticKind::kSampledDependency: return "sampled_dependency";
    case ExtendedOptimizerStatisticKind::kFkPkJoinCardinality: return "fk_pk_join_cardinality";
    case ExtendedOptimizerStatisticKind::kDocumentPathBridge: return "document_path_bridge";
  }
  return "multi_column_ndv";
}

void OptimizerStatisticsStore::UpsertTable(TableCardinalityStats stats) {
  const auto object_uuid = stats.identity.object_uuid;
  const auto catalog_epoch = stats.identity.catalog_epoch;
  const auto stats_epoch = stats.identity.stats_epoch;
  Upsert(&tables_, std::move(stats), [&](const TableCardinalityStats& existing) { return existing.identity.object_uuid == object_uuid; });
  InvalidateGlobalPinnedStatsCache("stats_refresh", object_uuid, {}, catalog_epoch, stats_epoch, "stats_refresh");
}

void OptimizerStatisticsStore::UpsertColumn(ColumnStats stats) {
  const auto object_uuid = stats.identity.object_uuid;
  const auto column_uuid = stats.column_uuid;
  const auto catalog_epoch = stats.identity.catalog_epoch;
  const auto stats_epoch = stats.identity.stats_epoch;
  Upsert(&columns_, std::move(stats), [&](const ColumnStats& existing) { return existing.identity.object_uuid == object_uuid && existing.column_uuid == column_uuid; });
  InvalidateGlobalPinnedStatsCache("stats_refresh", object_uuid, {}, catalog_epoch, stats_epoch, "stats_refresh");
}

void OptimizerStatisticsStore::UpsertHistogram(HistogramStats stats) {
  const auto object_uuid = stats.identity.object_uuid;
  const auto column_uuid = stats.column_uuid;
  const auto catalog_epoch = stats.identity.catalog_epoch;
  const auto stats_epoch = stats.identity.stats_epoch;
  Upsert(&histograms_, std::move(stats), [&](const HistogramStats& existing) { return existing.identity.object_uuid == object_uuid && existing.column_uuid == column_uuid; });
  InvalidateGlobalPinnedStatsCache("stats_refresh", object_uuid, {}, catalog_epoch, stats_epoch, "stats_refresh");
}

void OptimizerStatisticsStore::UpsertMcv(MostCommonValueStats stats) {
  const auto object_uuid = stats.identity.object_uuid;
  const auto column_uuid = stats.column_uuid;
  const auto value_encoded = stats.value_encoded;
  const auto catalog_epoch = stats.identity.catalog_epoch;
  const auto stats_epoch = stats.identity.stats_epoch;
  Upsert(&mcv_, std::move(stats), [&](const MostCommonValueStats& existing) { return existing.identity.object_uuid == object_uuid && existing.column_uuid == column_uuid && existing.value_encoded == value_encoded; });
  InvalidateGlobalPinnedStatsCache("stats_refresh", object_uuid, {}, catalog_epoch, stats_epoch, "stats_refresh");
}

void OptimizerStatisticsStore::UpsertExtendedStatistic(ExtendedOptimizerStatistic stats) {
  const auto object_uuid = stats.identity.object_uuid;
  const auto statistic_uuid = stats.identity.statistic_uuid;
  const auto relation_uuid = stats.relation_uuid;
  const auto catalog_epoch = stats.identity.catalog_epoch;
  const auto stats_epoch = stats.identity.stats_epoch;
  Upsert(&extended_stats_, std::move(stats), [&](const ExtendedOptimizerStatistic& existing) {
    return existing.identity.statistic_uuid == statistic_uuid ||
           (existing.relation_uuid == relation_uuid && existing.identity.statistic_uuid == statistic_uuid);
  });
  InvalidateGlobalPinnedStatsCache("stats_refresh", object_uuid, {}, catalog_epoch, stats_epoch, "stats_refresh");
}

void OptimizerStatisticsStore::UpsertIndex(IndexStats stats) {
  const auto index_uuid = stats.index_uuid;
  const auto object_uuid = stats.identity.object_uuid;
  const auto catalog_epoch = stats.identity.catalog_epoch;
  const auto stats_epoch = stats.identity.stats_epoch;
  Upsert(&indexes_, std::move(stats), [&](const IndexStats& existing) { return existing.index_uuid == index_uuid; });
  InvalidateGlobalPinnedStatsCache("stats_refresh", object_uuid, index_uuid, catalog_epoch, stats_epoch, "stats_refresh");
}

void OptimizerStatisticsStore::UpsertExpression(ExpressionStats stats) {
  const auto object_uuid = stats.identity.object_uuid;
  const auto expression_digest = stats.expression_digest;
  const auto catalog_epoch = stats.identity.catalog_epoch;
  const auto stats_epoch = stats.identity.stats_epoch;
  Upsert(&expressions_, std::move(stats), [&](const ExpressionStats& existing) { return existing.identity.object_uuid == object_uuid && existing.expression_digest == expression_digest; });
  InvalidateGlobalPinnedStatsCache("stats_refresh", object_uuid, {}, catalog_epoch, stats_epoch, "stats_refresh");
}

void OptimizerStatisticsStore::UpsertPageFilespace(PageFilespaceStats stats) {
  const auto filespace_uuid = stats.filespace_uuid;
  const auto page_family = stats.page_family;
  const auto object_uuid = stats.identity.object_uuid;
  const auto catalog_epoch = stats.identity.catalog_epoch;
  const auto stats_epoch = stats.identity.stats_epoch;
  Upsert(&page_filespaces_, std::move(stats), [&](const PageFilespaceStats& existing) { return existing.filespace_uuid == filespace_uuid && existing.page_family == page_family; });
  InvalidateGlobalPinnedStatsCache("stats_refresh", object_uuid, filespace_uuid, catalog_epoch, stats_epoch, "stats_refresh");
}

std::optional<TableCardinalityStats> OptimizerStatisticsStore::FindTable(const std::string& relation_uuid) const {
  auto it = std::find_if(tables_.begin(), tables_.end(), [&](const TableCardinalityStats& stats) { return stats.identity.object_uuid == relation_uuid; });
  if (it == tables_.end()) return std::nullopt;
  return *it;
}

std::optional<ColumnStats> OptimizerStatisticsStore::FindColumn(const std::string& relation_uuid, const std::string& column_uuid) const {
  auto it = std::find_if(columns_.begin(), columns_.end(), [&](const ColumnStats& stats) { return stats.identity.object_uuid == relation_uuid && stats.column_uuid == column_uuid; });
  if (it == columns_.end()) return std::nullopt;
  return *it;
}

std::vector<ExtendedOptimizerStatistic> OptimizerStatisticsStore::FindExtendedStatisticsForRelation(
    const std::string& relation_uuid) const {
  std::vector<ExtendedOptimizerStatistic> out;
  for (const auto& stats : extended_stats_) {
    if (stats.relation_uuid == relation_uuid || stats.identity.object_uuid == relation_uuid) {
      out.push_back(stats);
    }
  }
  return out;
}

std::optional<IndexStats> OptimizerStatisticsStore::FindIndex(const std::string& index_uuid) const {
  auto it = std::find_if(indexes_.begin(), indexes_.end(), [&](const IndexStats& stats) { return stats.index_uuid == index_uuid; });
  if (it == indexes_.end()) return std::nullopt;
  return *it;
}

std::optional<PageFilespaceStats> OptimizerStatisticsStore::FindFilespace(const std::string& filespace_uuid, const std::string& page_family) const {
  auto it = std::find_if(page_filespaces_.begin(), page_filespaces_.end(), [&](const PageFilespaceStats& stats) { return stats.filespace_uuid == filespace_uuid && stats.page_family == page_family; });
  if (it == page_filespaces_.end()) return std::nullopt;
  return *it;
}

void OptimizerStatisticsStore::MarkStaleByObject(const std::string& object_uuid, std::uint64_t catalog_epoch) {
  auto mark = [&](OptimizerStatsIdentity* identity) {
    if (identity->object_uuid == object_uuid) {
      identity->freshness = OptimizerStatsFreshnessState::kStale;
      identity->catalog_epoch = catalog_epoch;
    }
  };
  for (auto& stats : tables_) mark(&stats.identity);
  for (auto& stats : columns_) mark(&stats.identity);
  for (auto& stats : histograms_) mark(&stats.identity);
  for (auto& stats : mcv_) mark(&stats.identity);
  for (auto& stats : extended_stats_) mark(&stats.identity);
  for (auto& stats : indexes_) {
    mark(&stats.identity);
    if (stats.index_uuid == object_uuid) stats.identity.freshness = OptimizerStatsFreshnessState::kStale;
  }
  for (auto& stats : expressions_) mark(&stats.identity);
  for (auto& stats : page_filespaces_) mark(&stats.identity);
  InvalidateGlobalPinnedStatsCache("statistics_stale", object_uuid, {}, catalog_epoch, 0, "statistics_stale");
}

OptimizerStatsSnapshot OptimizerStatisticsStore::Snapshot(std::string snapshot_id) const {
  OptimizerStatsSnapshot snapshot;
  snapshot.snapshot_id = std::move(snapshot_id);
  snapshot.tables = tables_;
  snapshot.columns = columns_;
  snapshot.histograms = histograms_;
  snapshot.mcv = mcv_;
  snapshot.extended_stats = extended_stats_;
  snapshot.indexes = indexes_;
  snapshot.expressions = expressions_;
  snapshot.page_filespaces = page_filespaces_;
  for (const auto& table : tables_) {
    snapshot.stats_epoch = std::max(snapshot.stats_epoch, table.identity.stats_epoch);
    snapshot.catalog_epoch = std::max(snapshot.catalog_epoch, table.identity.catalog_epoch);
  }
  for (const auto& column : columns_) {
    snapshot.stats_epoch = std::max(snapshot.stats_epoch, column.identity.stats_epoch);
    snapshot.catalog_epoch = std::max(snapshot.catalog_epoch, column.identity.catalog_epoch);
  }
  for (const auto& histogram : histograms_) {
    snapshot.stats_epoch = std::max(snapshot.stats_epoch, histogram.identity.stats_epoch);
    snapshot.catalog_epoch = std::max(snapshot.catalog_epoch, histogram.identity.catalog_epoch);
  }
  for (const auto& mcv_value : mcv_) {
    snapshot.stats_epoch = std::max(snapshot.stats_epoch, mcv_value.identity.stats_epoch);
    snapshot.catalog_epoch = std::max(snapshot.catalog_epoch, mcv_value.identity.catalog_epoch);
  }
  for (const auto& stats : extended_stats_) {
    snapshot.stats_epoch = std::max(snapshot.stats_epoch, stats.identity.stats_epoch);
    snapshot.catalog_epoch = std::max(snapshot.catalog_epoch, stats.identity.catalog_epoch);
  }
  for (const auto& index : indexes_) {
    snapshot.stats_epoch = std::max(snapshot.stats_epoch, index.identity.stats_epoch);
    snapshot.catalog_epoch = std::max(snapshot.catalog_epoch, index.identity.catalog_epoch);
  }
  for (const auto& expression : expressions_) {
    snapshot.stats_epoch = std::max(snapshot.stats_epoch, expression.identity.stats_epoch);
    snapshot.catalog_epoch = std::max(snapshot.catalog_epoch, expression.identity.catalog_epoch);
  }
  for (const auto& filespace : page_filespaces_) {
    snapshot.stats_epoch = std::max(snapshot.stats_epoch, filespace.identity.stats_epoch);
    snapshot.catalog_epoch = std::max(snapshot.catalog_epoch, filespace.identity.catalog_epoch);
  }
  return snapshot;
}

std::string OptimizerPinnedStatsDescriptorCacheKeyText(const OptimizerPinnedStatsDescriptorKey& key) {
  std::ostringstream out;
  out << "catalog_epoch=" << key.catalog_epoch
      << "|security_epoch=" << key.security_epoch
      << "|resource_policy_epoch=" << key.resource_policy_epoch
      << "|name_resolution_epoch=" << key.name_resolution_epoch
      << "|stats_epoch=" << key.stats_epoch
      << "|descriptor_set_digest=" << key.descriptor_set_digest
      << "|security_policy_identity=" << key.security_policy_identity
      << "|redaction_policy_identity=" << key.redaction_policy_identity;
  AppendSorted(out, "object_uuids", key.object_uuids);
  AppendSorted(out, "index_uuids", key.index_uuids);
  return out.str();
}

OptimizerPinnedStatsLookupResult ValidateOptimizerPinnedStatsDescriptorKey(
    const OptimizerPinnedStatsDescriptorKey& key) {
  const std::string cache_key = OptimizerPinnedStatsDescriptorCacheKeyText(key);
  if (key.catalog_epoch == 0) {
    return StatsRefusal("SB_OPT_PINNED_STATS_EPOCH_REQUIRED", "catalog_epoch is required", cache_key);
  }
  if (key.security_epoch == 0) {
    return StatsRefusal("SB_OPT_PINNED_STATS_EPOCH_REQUIRED", "security_epoch is required", cache_key);
  }
  if (key.resource_policy_epoch == 0) {
    return StatsRefusal("SB_OPT_PINNED_STATS_EPOCH_REQUIRED", "resource_policy_epoch is required", cache_key);
  }
  if (key.name_resolution_epoch == 0) {
    return StatsRefusal("SB_OPT_PINNED_STATS_EPOCH_REQUIRED", "name_resolution_epoch is required", cache_key);
  }
  if (key.stats_epoch == 0) {
    return StatsRefusal("SB_OPT_PINNED_STATS_EPOCH_REQUIRED", "stats_epoch is required", cache_key);
  }
  if (key.descriptor_set_digest.empty()) {
    return StatsRefusal("SB_OPT_PINNED_STATS_DIGEST_REQUIRED", "descriptor_set_digest is required", cache_key);
  }
  if (key.object_uuids.empty()) {
    return StatsRefusal("SB_OPT_PINNED_STATS_OBJECT_UUID_REQUIRED", "object UUID is required", cache_key);
  }
  if (key.security_policy_identity.empty()) {
    return StatsRefusal("SB_OPT_PINNED_STATS_SECURITY_POLICY_REQUIRED",
                        "security policy identity is required",
                        cache_key);
  }
  if (key.redaction_policy_identity.empty()) {
    return StatsRefusal("SB_OPT_PINNED_STATS_REDACTION_POLICY_REQUIRED",
                        "redaction policy identity is required",
                        cache_key);
  }
  OptimizerPinnedStatsLookupResult result;
  result.ok = true;
  result.diagnostic_code = "SB_OPT_PINNED_STATS_KEY_OK";
  result.cache_key = cache_key;
  return result;
}

OptimizerPinnedStatsLookupResult OptimizerPinnedStatsDescriptorCache::Put(
    OptimizerPinnedStatsDescriptorSnapshot snapshot) {
  auto validation = ValidateOptimizerPinnedStatsDescriptorKey(snapshot.key);
  if (!validation.ok) return validation;
  if (!snapshot.read_only_snapshot ||
      !snapshot.mga_visibility_recheck_required ||
      !snapshot.security_recheck_required ||
      snapshot.finality_authority_cached) {
    return StatsRefusal("SB_OPT_PINNED_STATS_UNSAFE_SNAPSHOT",
                        "pinned statistics snapshots must preserve MGA/security rechecks and cache no finality",
                        validation.cache_key);
  }
  auto stored = std::make_shared<const OptimizerPinnedStatsDescriptorSnapshot>(std::move(snapshot));
  {
    std::lock_guard<std::mutex> lock(mutex_);
    snapshots_[validation.cache_key] = stored;
  }
  validation.snapshot = std::move(stored);
  validation.diagnostic_code = "SB_OPT_PINNED_STATS_PUT";
  return validation;
}

OptimizerPinnedStatsLookupResult OptimizerPinnedStatsDescriptorCache::Lookup(
    const OptimizerPinnedStatsDescriptorKey& key) {
  auto validation = ValidateOptimizerPinnedStatsDescriptorKey(key);
  if (!validation.ok) return validation;
  std::lock_guard<std::mutex> lock(mutex_);
  const auto found = snapshots_.find(validation.cache_key);
  if (found == snapshots_.end()) {
    return StatsRefusal("SB_OPT_PINNED_STATS_CACHE_MISS",
                        "epoch-pinned statistics snapshot not found",
                        validation.cache_key);
  }
  validation.cache_hit = true;
  validation.diagnostic_code = "SB_OPT_PINNED_STATS_CACHE_HIT";
  validation.snapshot = found->second;
  return validation;
}

OptimizerPinnedStatsInvalidationResult OptimizerPinnedStatsDescriptorCache::Invalidate(
    const StatsInvalidationEvent& event) {
  OptimizerPinnedStatsInvalidationResult result;
  const std::string reason = event.reason.empty() ? event.event_kind : event.reason;
  std::lock_guard<std::mutex> lock(mutex_);
  for (auto it = snapshots_.begin(); it != snapshots_.end();) {
    if (!StatsEventInvalidatesSnapshot(*it->second, event)) {
      ++it;
      continue;
    }
    OptimizerPinnedStatsInvalidatedEntry entry;
    entry.cache_key = it->first;
    entry.reason = reason;
    entry.object_uuids = it->second->key.object_uuids;
    entry.index_uuids = it->second->key.index_uuids;
    result.invalidated_entries.push_back(std::move(entry));
    it = snapshots_.erase(it);
  }
  return result;
}

void OptimizerPinnedStatsDescriptorCache::Clear() {
  std::lock_guard<std::mutex> lock(mutex_);
  snapshots_.clear();
}

OptimizerPinnedStatsDescriptorCache& GlobalOptimizerPinnedStatsDescriptorCache() {
  static OptimizerPinnedStatsDescriptorCache cache;
  return cache;
}

OptimizerStatisticsCatalog OptimizerStatisticsStore::ToLegacyCatalog() const {
  OptimizerStatisticsCatalog catalog;
  for (const auto& table : tables_) {
    catalog.Add(MakeStatistic("row_count", "relation", table.identity.object_uuid, static_cast<double>(table.row_count), table.identity.source, table.identity.stats_epoch, 0, table.identity.confidence, OptimizerStatsIdentityIsUsable(table.identity)));
    catalog.Add(MakeStatistic("visible_row_count", "relation", table.identity.object_uuid, static_cast<double>(table.visible_row_count), table.identity.source, table.identity.stats_epoch, 0, table.identity.confidence, OptimizerStatsIdentityIsUsable(table.identity)));
    catalog.Add(MakeStatistic("relation_visible_version_count", "relation", table.identity.object_uuid, static_cast<double>(table.visible_row_count), table.identity.source, table.identity.stats_epoch, 0, table.identity.confidence, OptimizerStatsIdentityIsUsable(table.identity)));
    catalog.Add(MakeStatistic("page_count", "relation", table.identity.object_uuid, static_cast<double>(table.page_count), table.identity.source, table.identity.stats_epoch, 0, table.identity.confidence, OptimizerStatsIdentityIsUsable(table.identity)));
    catalog.Add(MakeStatistic("average_row_bytes", "relation", table.identity.object_uuid, static_cast<double>(table.average_row_bytes), table.identity.source, table.identity.stats_epoch, 0, table.identity.confidence, OptimizerStatsIdentityIsUsable(table.identity)));
  }
  for (const auto& column : columns_) {
    const bool usable = OptimizerStatsIdentityIsUsable(column.identity);
    catalog.Add(MakeStatistic("column_ndv", "column", column.column_uuid, static_cast<double>(column.distinct_count), column.identity.source, column.identity.stats_epoch, 0, column.identity.confidence, usable));
    catalog.Add(MakeStatistic("column_null_fraction", "column", column.column_uuid, column.null_fraction, column.identity.source, column.identity.stats_epoch, 0, column.identity.confidence, usable));
    catalog.Add(MakeStatistic("column_average_width_bytes", "column", column.column_uuid, static_cast<double>(column.average_width_bytes), column.identity.source, column.identity.stats_epoch, 0, column.identity.confidence, usable));
    catalog.Add(MakeStatistic("column_correlation", "column", column.column_uuid, column.correlation, column.identity.source, column.identity.stats_epoch, 0, column.identity.confidence, usable));
    catalog.Add(MakeStatistic("column_sample_rows", "column", column.column_uuid, static_cast<double>(column.sample_rows), column.identity.source, column.identity.stats_epoch, 0, column.identity.confidence, usable));
    catalog.Add(MakeStatistic("column_hll_estimated_distinct", "column", column.column_uuid, static_cast<double>(column.hyperloglog_estimated_distinct), column.identity.source, column.identity.stats_epoch, 0, column.identity.confidence, usable));
    catalog.Add(MakeStatistic("column_hll_relative_error_ppm", "column", column.column_uuid, column.hyperloglog_relative_error * 1000000.0, column.identity.source, column.identity.stats_epoch, 0, column.identity.confidence, usable));
  }
  for (const auto& histogram : histograms_) {
    const bool usable = OptimizerStatsIdentityIsUsable(histogram.identity);
    double fraction = 0.0;
    std::uint64_t rows = 0;
    for (const auto& bucket : histogram.buckets) {
      fraction += bucket.fraction;
      rows += bucket.row_count;
    }
    catalog.Add(MakeStatistic("histogram_bucket_count", "histogram", histogram.column_uuid, static_cast<double>(histogram.buckets.size()), histogram.identity.source, histogram.identity.stats_epoch, 0, histogram.identity.confidence, usable));
    catalog.Add(MakeStatistic("histogram_covered_fraction", "histogram", histogram.column_uuid, fraction, histogram.identity.source, histogram.identity.stats_epoch, 0, histogram.identity.confidence, usable));
    catalog.Add(MakeStatistic("histogram_row_count", "histogram", histogram.column_uuid, static_cast<double>(rows), histogram.identity.source, histogram.identity.stats_epoch, 0, histogram.identity.confidence, usable));
  }
  for (const auto& mcv_value : mcv_) {
    catalog.Add(MakeStatistic("mcv_frequency", "mcv", mcv_value.column_uuid, mcv_value.frequency, mcv_value.identity.source, mcv_value.identity.stats_epoch, 0, mcv_value.identity.confidence, OptimizerStatsIdentityIsUsable(mcv_value.identity)));
  }
  for (const auto& stats : extended_stats_) {
    const bool usable = OptimizerStatsIdentityIsUsable(stats.identity);
    catalog.Add(MakeStatistic("extended_multi_column_distinct_count", "extended_stats", stats.identity.statistic_uuid, static_cast<double>(stats.multi_column_distinct_count), stats.identity.source, stats.identity.stats_epoch, 0, stats.identity.confidence, usable));
    catalog.Add(MakeStatistic("extended_functional_dependency_strength", "extended_stats", stats.identity.statistic_uuid, stats.functional_dependency_strength, stats.identity.source, stats.identity.stats_epoch, 0, stats.identity.confidence, usable));
    catalog.Add(MakeStatistic("extended_correlation_coefficient", "extended_stats", stats.identity.statistic_uuid, stats.correlation_coefficient, stats.identity.source, stats.identity.stats_epoch, 0, stats.identity.confidence, usable));
    catalog.Add(MakeStatistic("extended_histogram_selectivity", "extended_stats", stats.identity.statistic_uuid, stats.histogram_selectivity, stats.identity.source, stats.identity.stats_epoch, 0, stats.identity.confidence, usable));
    catalog.Add(MakeStatistic("extended_sampled_dependency_selectivity", "extended_stats", stats.identity.statistic_uuid, stats.sampled_dependency_selectivity, stats.identity.source, stats.identity.stats_epoch, 0, stats.identity.confidence, usable));
  }
  for (const auto& index : indexes_) {
    catalog.Add(MakeStatistic("index_height", "index", index.index_uuid, static_cast<double>(index.height), index.identity.source, index.identity.stats_epoch, 0, index.identity.confidence, OptimizerStatsIdentityIsUsable(index.identity)));
    catalog.Add(MakeStatistic("index_depth", "index", index.index_uuid, static_cast<double>(index.height), index.identity.source, index.identity.stats_epoch, 0, index.identity.confidence, OptimizerStatsIdentityIsUsable(index.identity)));
    catalog.Add(MakeStatistic("index_leaf_pages", "index", index.index_uuid, static_cast<double>(index.leaf_pages), index.identity.source, index.identity.stats_epoch, 0, index.identity.confidence, OptimizerStatsIdentityIsUsable(index.identity)));
    catalog.Add(MakeStatistic("index_distinct_keys", "index", index.index_uuid, static_cast<double>(index.distinct_keys), index.identity.source, index.identity.stats_epoch, 0, index.identity.confidence, OptimizerStatsIdentityIsUsable(index.identity)));
    catalog.Add(MakeStatistic("index_clustering_factor", "index", index.index_uuid, index.clustering_factor, index.identity.source, index.identity.stats_epoch, 0, index.identity.confidence, OptimizerStatsIdentityIsUsable(index.identity)));
    catalog.Add(MakeStatistic("index_fragmentation_ratio", "index", index.index_uuid, index.fragmentation_ratio, index.identity.source, index.identity.stats_epoch, 0, index.identity.confidence, OptimizerStatsIdentityIsUsable(index.identity)));
    catalog.Add(MakeStatistic("index_visibility_coverage", "index", index.index_uuid, index.rebuild_in_progress ? 0.0 : index.visibility_coverage, index.identity.source, index.identity.stats_epoch, 0, index.identity.confidence, OptimizerStatsIdentityIsUsable(index.identity)));
    catalog.Add(MakeStatistic("index_predicate_coverage", "index", index.index_uuid, index.predicate_coverage, index.identity.source, index.identity.stats_epoch, 0, index.identity.confidence, OptimizerStatsIdentityIsUsable(index.identity)));
    catalog.Add(MakeStatistic("index_false_positive_ratio", "index", index.index_uuid, index.false_positive_ratio, index.identity.source, index.identity.stats_epoch, 0, index.identity.confidence, OptimizerStatsIdentityIsUsable(index.identity)));
    catalog.Add(MakeStatistic("index_route_benchmark_clean", "index", index.index_uuid, index.route_benchmark_clean ? 1.0 : 0.0, index.identity.source, index.identity.stats_epoch, 0, index.identity.confidence, OptimizerStatsIdentityIsUsable(index.identity)));
  }
  for (const auto& filespace : page_filespaces_) {
    const bool usable = OptimizerStatsIdentityIsUsable(filespace.identity);
    catalog.Add(MakeStatistic("filespace_available_pages", "filespace", filespace.filespace_uuid, static_cast<double>(filespace.free_pages), filespace.identity.source, filespace.identity.stats_epoch, 0, filespace.identity.confidence, usable));
    catalog.Add(MakeStatistic("page_family_read_latency_microseconds", "page_family", filespace.filespace_uuid, filespace.sequential_latency_score * 1000.0, filespace.identity.source, filespace.identity.stats_epoch, 0, filespace.identity.confidence, usable));
    catalog.Add(MakeStatistic("io_latency_multiplier", "page_family", filespace.filespace_uuid, filespace.sequential_latency_score, filespace.identity.source, filespace.identity.stats_epoch, 0, filespace.identity.confidence, usable));
  }
  for (const auto& expression : expressions_) {
    const bool usable = OptimizerStatsIdentityIsUsable(expression.identity);
    catalog.Add(MakeStatistic("expression_distinct_count", "expression", expression.expression_digest, static_cast<double>(expression.distinct_count), expression.identity.source, expression.identity.stats_epoch, 0, expression.identity.confidence, usable));
    catalog.Add(MakeStatistic("expression_null_fraction", "expression", expression.expression_digest, expression.null_fraction, expression.identity.source, expression.identity.stats_epoch, 0, expression.identity.confidence, usable));
  }
  return catalog;
}

TableCardinalityStats BuildTableStatsFromAnalyzeSample(const AnalyzeSampleInput& input) {
  TableCardinalityStats stats;
  stats.identity.object_uuid = input.relation_uuid;
  stats.identity.statistic_uuid = input.relation_uuid + ":table_stats";
  stats.identity.stats_epoch = input.stats_epoch;
  stats.identity.catalog_epoch = input.catalog_epoch;
  stats.identity.transaction_visibility_epoch = input.stats_epoch;
  stats.identity.freshness = OptimizerStatsFreshnessState::kFresh;
  stats.identity.source = input.sampled_rows == input.total_rows_estimate ? StatisticSource::kCatalogExact : StatisticSource::kCatalogSample;
  stats.identity.confidence = input.sampled_rows == input.total_rows_estimate ? CostConfidence::kExact : CostConfidence::kMedium;
  stats.row_count = input.total_rows_estimate;
  stats.visible_row_count = input.total_rows_estimate;
  stats.page_count = input.page_count;
  stats.average_row_bytes = input.average_row_bytes;
  return stats;
}

std::vector<StatisticsContractStatus> ValidateOptimizerStatsSnapshot(const OptimizerStatsSnapshot& snapshot) {
  std::vector<StatisticsContractStatus> statuses;
  if (snapshot.snapshot_id.empty()) statuses.push_back(Status(false, "SB_OPT_STATS_SNAPSHOT_ID_REQUIRED", "snapshot"));
  for (const auto& table : snapshot.tables) ValidateIdentity(table.identity, "table", &statuses);
  for (const auto& column : snapshot.columns) {
    ValidateIdentity(column.identity, "column", &statuses);
    if (column.column_uuid.empty()) statuses.push_back(Status(false, "SB_OPT_STATS_COLUMN_UUID_REQUIRED", column.identity.object_uuid));
    if (column.null_fraction < 0.0 || column.null_fraction > 1.0) statuses.push_back(Status(false, "SB_OPT_STATS_COLUMN_NULL_FRACTION_INVALID", column.column_uuid));
    if (column.correlation < -1.0 || column.correlation > 1.0) statuses.push_back(Status(false, "SB_OPT_STATS_COLUMN_CORRELATION_INVALID", column.column_uuid));
    if (column.sample_rows != 0 && column.sample_method.empty()) statuses.push_back(Status(false, "SB_OPT_STATS_SAMPLE_METHOD_REQUIRED", column.column_uuid));
    if (column.sample_rows != 0 && column.sample_provenance_digest.empty()) statuses.push_back(Status(false, "SB_OPT_STATS_SAMPLE_PROVENANCE_REQUIRED", column.column_uuid));
    if (column.hyperloglog_register_count != 0 && column.hyperloglog_estimated_distinct == 0) statuses.push_back(Status(false, "SB_OPT_STATS_HLL_NDV_REQUIRED", column.column_uuid));
    if (column.hyperloglog_relative_error < 0.0 || column.hyperloglog_relative_error > 1.0) statuses.push_back(Status(false, "SB_OPT_STATS_HLL_ERROR_INVALID", column.column_uuid));
  }
  for (const auto& histogram : snapshot.histograms) {
    ValidateIdentity(histogram.identity, "histogram", &statuses);
    if (histogram.column_uuid.empty()) statuses.push_back(Status(false, "SB_OPT_STATS_HISTOGRAM_COLUMN_UUID_REQUIRED", histogram.identity.object_uuid));
    if (histogram.buckets.empty()) statuses.push_back(Status(false, "SB_OPT_STATS_HISTOGRAM_BUCKET_REQUIRED", histogram.column_uuid));
  }
  for (const auto& mcv_value : snapshot.mcv) {
    ValidateIdentity(mcv_value.identity, "mcv", &statuses);
    if (mcv_value.column_uuid.empty()) statuses.push_back(Status(false, "SB_OPT_STATS_MCV_COLUMN_UUID_REQUIRED", mcv_value.identity.object_uuid));
    if (mcv_value.frequency < 0.0 || mcv_value.frequency > 1.0) statuses.push_back(Status(false, "SB_OPT_STATS_MCV_FREQUENCY_INVALID", mcv_value.column_uuid));
  }
  for (const auto& stats : snapshot.extended_stats) {
    ValidateIdentity(stats.identity, "extended_stats", &statuses);
    if (stats.relation_uuid.empty()) statuses.push_back(Status(false, "SB_OPT_EXTENDED_STATS_RELATION_UUID_REQUIRED", stats.identity.statistic_uuid));
    if (stats.column_uuids.empty() && stats.document_path_digests.empty()) statuses.push_back(Status(false, "SB_OPT_EXTENDED_STATS_SHAPE_REQUIRED", stats.identity.statistic_uuid));
    if (stats.functional_dependency_strength < 0.0 || stats.functional_dependency_strength > 1.0) statuses.push_back(Status(false, "SB_OPT_EXTENDED_STATS_DEPENDENCY_INVALID", stats.identity.statistic_uuid));
    if (stats.correlation_coefficient < -1.0 || stats.correlation_coefficient > 1.0) statuses.push_back(Status(false, "SB_OPT_EXTENDED_STATS_CORRELATION_INVALID", stats.identity.statistic_uuid));
    if (stats.histogram_selectivity < 0.0 || stats.histogram_selectivity > 1.0) statuses.push_back(Status(false, "SB_OPT_EXTENDED_STATS_HISTOGRAM_INVALID", stats.identity.statistic_uuid));
    if (stats.sampled_dependency_selectivity < 0.0 || stats.sampled_dependency_selectivity > 1.0) statuses.push_back(Status(false, "SB_OPT_EXTENDED_STATS_SAMPLE_INVALID", stats.identity.statistic_uuid));
    if (stats.observed_selectivity_error < -1.0 || stats.observed_selectivity_error > 1.0) statuses.push_back(Status(false, "SB_OPT_EXTENDED_STATS_OBSERVED_ERROR_INVALID", stats.identity.statistic_uuid));
    for (const auto& entry : stats.joint_mcv) {
      if (entry.frequency < 0.0 || entry.frequency > 1.0) statuses.push_back(Status(false, "SB_OPT_EXTENDED_STATS_JOINT_MCV_FREQUENCY_INVALID", stats.identity.statistic_uuid));
      if (!entry.value_encodings.empty() && !stats.column_uuids.empty() && entry.value_encodings.size() != stats.column_uuids.size()) statuses.push_back(Status(false, "SB_OPT_EXTENDED_STATS_JOINT_MCV_SHAPE_INVALID", stats.identity.statistic_uuid));
    }
    if (stats.finality_authority) statuses.push_back(Status(false, "SB_OPT_EXTENDED_STATS_FINALITY_FORBIDDEN", stats.identity.statistic_uuid));
    if (!stats.mga_visibility_recheck_required || !stats.security_recheck_required) statuses.push_back(Status(false, "SB_OPT_EXTENDED_STATS_RECHECK_REQUIRED", stats.identity.statistic_uuid));
  }
  for (const auto& index : snapshot.indexes) {
    ValidateIdentity(index.identity, "index", &statuses);
    if (index.index_uuid.empty()) statuses.push_back(Status(false, "SB_OPT_STATS_INDEX_UUID_REQUIRED", index.identity.object_uuid));
    if (index.index_family.empty()) statuses.push_back(Status(false, "SB_OPT_STATS_INDEX_FAMILY_REQUIRED", index.index_uuid));
    if (index.visibility_coverage < 0.0 || index.visibility_coverage > 1.0) statuses.push_back(Status(false, "SB_OPT_STATS_INDEX_VISIBILITY_COVERAGE_INVALID", index.index_uuid));
    if (index.predicate_coverage < 0.0 || index.predicate_coverage > 1.0) statuses.push_back(Status(false, "SB_OPT_STATS_INDEX_PREDICATE_COVERAGE_INVALID", index.index_uuid));
    if (index.false_positive_ratio < 0.0 || index.false_positive_ratio > 1.0) statuses.push_back(Status(false, "SB_OPT_STATS_INDEX_FALSE_POSITIVE_RATIO_INVALID", index.index_uuid));
    if (!index.exact_recheck_required || !index.mga_recheck_required || !index.security_recheck_required) statuses.push_back(Status(false, "SB_OPT_STATS_INDEX_RECHECK_REQUIRED", index.index_uuid));
    if (!index.family_claim_removed &&
        !index.equality_lookup_supported &&
        !index.ordered_range_supported &&
        !index.negative_prune_supported &&
        !index.candidate_set_producer) statuses.push_back(Status(false, "SB_OPT_STATS_INDEX_ROUTE_SEMANTICS_REQUIRED", index.index_uuid));
    if (index.rebuild_in_progress) statuses.push_back(Status(false, "SB_OPT_STATS_INDEX_REBUILD_IN_PROGRESS", index.index_uuid));
  }
  for (const auto& expression : snapshot.expressions) {
    ValidateIdentity(expression.identity, "expression", &statuses);
    if (expression.expression_digest.empty()) statuses.push_back(Status(false, "SB_OPT_STATS_EXPRESSION_DIGEST_REQUIRED", expression.identity.object_uuid));
  }
  for (const auto& filespace : snapshot.page_filespaces) {
    ValidateIdentity(filespace.identity, "page_filespace", &statuses);
    if (filespace.filespace_uuid.empty()) statuses.push_back(Status(false, "SB_OPT_STATS_FILESPACE_UUID_REQUIRED", filespace.identity.object_uuid));
    if (filespace.degraded) statuses.push_back(Status(false, "SB_OPT_STATS_FILESPACE_DEGRADED", filespace.filespace_uuid));
  }
  if (statuses.empty()) statuses.push_back(Status(true, "SB_OPT_STATS_OK", snapshot.snapshot_id));
  return statuses;
}

double EstimateEqualitySelectivityFromColumnStats(const ColumnStats& stats, std::uint64_t table_rows) {
  if (!OptimizerStatsIdentityIsUsable(stats.identity)) return 0.10;
  if (stats.distinct_count == 0 || table_rows == 0) return 1.0;
  const double selectivity = 1.0 / static_cast<double>(stats.distinct_count);
  return std::clamp(selectivity, 1.0 / static_cast<double>(table_rows), 1.0);
}

double EstimateRangeSelectivityFromHistogram(const HistogramStats& stats) {
  if (!OptimizerStatsIdentityIsUsable(stats.identity) || stats.buckets.empty()) return 0.25;
  double total = 0.0;
  for (const auto& bucket : stats.buckets) total += bucket.fraction;
  return std::clamp(total, 0.0, 1.0);
}

CostVector ApplyIndexHealthCostAdjustment(CostVector cost, const IndexStats& stats) {
  if (!OptimizerStatsIdentityIsUsable(stats.identity)) {
    cost.uncertainty_cost += 100;
  }
  if (stats.rebuild_in_progress) {
    cost.selectable = false;
    cost.confidence = CostConfidence::kRejected;
    cost.rejection_reason = "index_rebuild_in_progress";
  }
  if (stats.visibility_coverage < 1.0 || stats.predicate_coverage < 1.0) {
    cost.uncertainty_cost += static_cast<std::uint64_t>((2.0 - stats.visibility_coverage - stats.predicate_coverage) * 100.0);
  }
  const double penalty = 1.0 + std::clamp(stats.fragmentation_ratio, 0.0, 10.0) + std::clamp(stats.contention_ratio, 0.0, 10.0);
  cost.io_cost = static_cast<std::uint64_t>(static_cast<double>(cost.io_cost) * penalty);
  cost.total_cost = cost.startup_cost + cost.row_cost + cost.io_cost + cost.memory_cost + cost.uncertainty_cost;
  return cost;
}

std::vector<StatisticsContractStatus> ValidateIndexFamilyCostCoverage(
    const std::vector<IndexStats>& indexes) {
  std::vector<StatisticsContractStatus> statuses;
  for (const auto& index : indexes) {
    if (index.family_claim_removed) {
      statuses.push_back(Status(true, "SB_OPT_INDEX_FAMILY_CLAIM_REMOVED", index.index_family));
      continue;
    }
    if (index.index_family.empty() || index.index_uuid.empty()) {
      statuses.push_back(Status(false, "SB_OPT_INDEX_FAMILY_IDENTITY_REQUIRED", index.index_uuid));
      continue;
    }
    if (!OptimizerStatsIdentityIsUsable(index.identity)) {
      statuses.push_back(Status(false, "SB_OPT_INDEX_FAMILY_STATS_UNUSABLE", index.index_uuid));
    }
    if (!index.equality_lookup_supported &&
        !index.ordered_range_supported &&
        !index.negative_prune_supported &&
        !index.candidate_set_producer) {
      statuses.push_back(Status(false, "SB_OPT_INDEX_FAMILY_ROUTE_SEMANTICS_REQUIRED", index.index_uuid));
    }
    if (!index.exact_recheck_required || !index.mga_recheck_required ||
        !index.security_recheck_required) {
      statuses.push_back(Status(false, "SB_OPT_INDEX_FAMILY_RECHECK_REQUIRED", index.index_uuid));
    }
    if (index.false_positive_ratio < 0.0 || index.false_positive_ratio > 1.0) {
      statuses.push_back(Status(false, "SB_OPT_INDEX_FAMILY_FALSE_POSITIVE_INVALID", index.index_uuid));
    }
    if (!index.route_benchmark_clean) {
      statuses.push_back(Status(false, "SB_OPT_INDEX_FAMILY_ROUTE_NOT_BENCHMARK_CLEAN", index.index_uuid));
    }
  }
  if (statuses.empty()) statuses.push_back(Status(true, "SB_OPT_INDEX_FAMILY_COST_COVERAGE_OK", "indexes"));
  return statuses;
}

}  // namespace scratchbird::engine::optimizer

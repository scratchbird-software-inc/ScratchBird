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
#include <cmath>
#include <new>
#include <stdexcept>
#include <utility>
#include <set>

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

StatisticsContractStatus Status(bool ok, std::string code, const planner::CanonicalPlannerUuid& object) {
  auto status = Status(ok, std::move(code), std::string{});
  status.object_uuid = object;
  return status;
}

void ValidateIdentity(const OptimizerStatsIdentity& identity,
                      const char* family,
                      std::vector<StatisticsContractStatus>* statuses) {
  if (!scratchbird::core::uuid::IsEngineIdentityUuid(identity.object_uuid)) statuses->push_back(Status(false, "SB_OPT_STATS_OBJECT_UUID_REQUIRED", family));
  if (!scratchbird::core::uuid::IsEngineIdentityUuid(identity.statistic_uuid)) statuses->push_back(Status(false, "SB_OPT_STATS_STATISTIC_UUID_REQUIRED", family));
  if (identity.stats_epoch == 0) statuses->push_back(Status(false, "SB_OPT_STATS_EPOCH_REQUIRED", family));
  if (!OptimizerStatsIdentityIsUsable(identity)) statuses->push_back(Status(false, "SB_OPT_STATS_NOT_USABLE", family));
}

std::vector<planner::CanonicalPlannerUuid> SortedUnique(std::vector<planner::CanonicalPlannerUuid> values) {
  std::sort(values.begin(), values.end());
  values.erase(std::unique(values.begin(), values.end()), values.end());
  return values;
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

bool Contains(const std::vector<planner::CanonicalPlannerUuid>& values, const planner::CanonicalPlannerUuid& value) {
  return !value.is_nil() &&
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
  if (!event.object_uuid.is_nil() && Contains(snapshot.key.object_uuids, event.object_uuid)) return true;
  if (!event.index_uuid.is_nil() && Contains(snapshot.key.index_uuids, event.index_uuid)) return true;
  if (!event.security_policy_identity.is_nil() &&
      event.security_policy_identity == snapshot.key.security_policy_identity) {
    return true;
  }
  if (!event.redaction_policy_identity.is_nil() &&
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
         event.object_uuid.is_nil() &&
         event.index_uuid.is_nil() &&
         event.security_policy_identity.is_nil() &&
         event.redaction_policy_identity.is_nil();
}

void InvalidateGlobalPinnedStatsCache(std::string event_kind,
                                      planner::CanonicalPlannerUuid object_uuid,
                                      planner::CanonicalPlannerUuid index_uuid,
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
  return scratchbird::core::uuid::IsEngineIdentityUuid(identity.object_uuid) &&
         scratchbird::core::uuid::IsEngineIdentityUuid(identity.statistic_uuid) &&
         identity.stats_epoch != 0 && identity.catalog_epoch != 0 &&
         identity.transaction_visibility_epoch != 0 &&
         identity.freshness == OptimizerStatsFreshnessState::kFresh &&
         identity.source >= StatisticSource::kCatalogExact &&
         identity.source <= StatisticSource::kClusterMetric &&
         identity.confidence >= CostConfidence::kExact &&
         identity.confidence <= CostConfidence::kLow;
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

bool OptimizerTableStatsAreUsable(const TableCardinalityStats& stats) {
  return OptimizerStatsIdentityIsUsable(stats.identity) &&
      (stats.identity.source == StatisticSource::kCatalogExact ||
       stats.identity.source == StatisticSource::kCatalogSample) &&
      stats.visible_row_count <= stats.row_count &&
      (stats.row_count == 0 || (stats.page_count != 0 && stats.average_row_bytes != 0));
}

bool OptimizerIndexStatsAreUsable(const IndexStats& stats,
    const planner::CanonicalPlannerUuid& relation_uuid, std::string_view descriptor_digest) {
  return OptimizerStatsIdentityIsUsable(stats.identity) &&
      (stats.identity.source == StatisticSource::kCatalogExact ||
       stats.identity.source == StatisticSource::kCatalogSample) &&
      scratchbird::core::uuid::IsEngineIdentityUuid(stats.index_uuid) &&
      scratchbird::core::uuid::IsEngineIdentityUuid(relation_uuid) &&
      stats.relation_uuid == relation_uuid && !stats.descriptor_digest.empty() &&
      stats.descriptor_digest == descriptor_digest && stats.route_benchmark_clean &&
      stats.exact_recheck_required && stats.mga_recheck_required && stats.security_recheck_required &&
      !stats.rebuild_in_progress && !stats.family_claim_removed;
}

OptimizerStatsPublicationResult OptimizerStatisticsStore::PublishRelationSnapshot(
    OptimizerStatsSnapshot snapshot) try {
  using PublicationStatus = OptimizerStatsPublicationStatus;
  if (snapshot.tables.size() != 1 || snapshot.catalog_epoch == 0 || snapshot.stats_epoch == 0)
    return {};
  const auto validation = ValidateOptimizerStatsSnapshot(snapshot);
  if (std::ranges::any_of(validation, [](const auto& status) { return !status.ok; })) return {};
  const auto relation = snapshot.tables.front().identity.object_uuid;
  std::set<planner::CanonicalPlannerUuid> statistic_ids{snapshot.snapshot_id};
  const auto valid_family = [&](const auto& records) {
    for (const auto& record : records) {
      auto object = relation;
      if constexpr (requires { record.filespace_uuid; }) object = record.filespace_uuid;
      if (record.identity.object_uuid != object ||
          record.identity.catalog_epoch != snapshot.catalog_epoch ||
          record.identity.stats_epoch != snapshot.stats_epoch ||
          !statistic_ids.insert(record.identity.statistic_uuid).second) return false;
    }
    return true;
  };
  if (!valid_family(snapshot.tables) || !valid_family(snapshot.columns) ||
      !valid_family(snapshot.histograms) || !valid_family(snapshot.mcv) ||
      !valid_family(snapshot.extended_stats) || !valid_family(snapshot.indexes) ||
      !valid_family(snapshot.expressions) || !valid_family(snapshot.page_filespaces)) return {};

  std::set<planner::CanonicalPlannerUuid> columns, histograms, indexes;
  std::set<std::pair<planner::CanonicalPlannerUuid, std::string>> mcv, filespaces;
  std::set<std::string> expressions;
  for (const auto& record : snapshot.columns)
    if (!columns.insert(record.column_uuid).second) return {};
  for (const auto& record : snapshot.histograms)
    if (!columns.contains(record.column_uuid) || !histograms.insert(record.column_uuid).second) return {};
  for (const auto& record : snapshot.mcv)
    if (!columns.contains(record.column_uuid) ||
        !mcv.emplace(record.column_uuid, record.value_encoded).second) return {};
  for (const auto& record : snapshot.expressions)
    if (!expressions.insert(record.expression_digest).second) return {};
  for (const auto& record : snapshot.extended_stats) {
    if (record.relation_uuid != relation ||
        !std::ranges::all_of(record.column_uuids, [](const auto& id) {
          return scratchbird::core::uuid::IsEngineIdentityUuid(id);
        })) return {};
  }
  for (const auto& record : snapshot.indexes) {
    const auto valid_id = [](const auto& id) { return scratchbird::core::uuid::IsEngineIdentityUuid(id); };
    if (record.relation_uuid != relation || !indexes.insert(record.index_uuid).second ||
        !std::ranges::all_of(record.key_column_uuids, valid_id) ||
        !std::ranges::all_of(record.covered_column_uuids, valid_id)) return {};
  }
  for (const auto& record : snapshot.page_filespaces)
    if (!filespaces.emplace(record.filespace_uuid, record.page_family).second) return {};

  std::lock_guard store_lock(mutex_);
  const auto current_family = [&](const auto& records) {
    for (const auto& record : records) {
      if (record.identity.object_uuid == relation) {
        if (record.identity.stats_epoch >= snapshot.stats_epoch ||
            record.identity.catalog_epoch > snapshot.catalog_epoch) return false;
      } else if (statistic_ids.contains(record.identity.statistic_uuid)) {
        return false;
      }
    }
    return true;
  };
  if (!current_family(tables_) || !current_family(columns_) || !current_family(histograms_) ||
      !current_family(mcv_) || !current_family(extended_stats_) || !current_family(indexes_) ||
      !current_family(expressions_))
    return {PublicationStatus::kStaleEpoch};
  // An index cannot silently change its owning relation. Filespace statistics
  // are shared node metadata, not owned by whichever relation was analyzed.
  for (const auto& record : indexes_)
    if (record.identity.object_uuid != relation && indexes.contains(record.index_uuid)) return {};
  for (const auto& record : page_filespaces_) {
    if (filespaces.contains({record.filespace_uuid, record.page_family})) {
      if (record.identity.stats_epoch >= snapshot.stats_epoch ||
          record.identity.catalog_epoch > snapshot.catalog_epoch)
        return {PublicationStatus::kStaleEpoch};
    } else if (statistic_ids.contains(record.identity.statistic_uuid)) return {};
  }
  for (const auto& [object, owned] : published_)
    if (owned->snapshot_id == snapshot.snapshot_id ||
        (object != relation && statistic_ids.contains(owned->snapshot_id))) return {};

  const auto replace = [&](const auto& existing, const auto& replacement) {
    auto staged = existing;
    std::erase_if(staged, [&](const auto& record) { return record.identity.object_uuid == relation; });
    staged.insert(staged.end(), replacement.begin(), replacement.end());
    return staged;
  };
  auto tables = replace(tables_, snapshot.tables);
  auto column_records = replace(columns_, snapshot.columns);
  auto histogram_records = replace(histograms_, snapshot.histograms);
  auto mcv_records = replace(mcv_, snapshot.mcv);
  auto extended_records = replace(extended_stats_, snapshot.extended_stats);
  auto index_records = replace(indexes_, snapshot.indexes);
  auto expression_records = replace(expressions_, snapshot.expressions);
  auto filespace_records = page_filespaces_;
  for (const auto& record : snapshot.page_filespaces) {
    Upsert(&filespace_records, record, [&](const auto& prior) {
      return prior.filespace_uuid == record.filespace_uuid && prior.page_family == record.page_family;
    });
  }
  auto owned = std::make_shared<const OptimizerStatsSnapshot>(std::move(snapshot));
  auto published = published_;
  published.insert_or_assign(relation, owned);

  // Hold both locks through the commit. Readers see the old complete state or
  // the new complete state; no independently visible family prefix is exposed.
  auto& cache = GlobalOptimizerPinnedStatsDescriptorCache();
  std::lock_guard cache_lock(cache.mutex_);
  auto epochs = cache.publication_epochs_;
  const auto pin_epoch = [&](const auto& id) {
    auto& floor = epochs[id];
    floor.catalog = std::max(floor.catalog, owned->catalog_epoch);
    floor.stats = std::max(floor.stats, owned->stats_epoch);
  };
  std::set<planner::CanonicalPlannerUuid> affected{relation};
  for (const auto& record : indexes_)
    if (record.identity.object_uuid == relation) affected.insert(record.index_uuid);
  for (const auto& record : owned->indexes) affected.insert(record.index_uuid);
  for (const auto& record : owned->page_filespaces) affected.insert(record.filespace_uuid);
  for (const auto& id : affected) pin_epoch(id);
  std::vector<decltype(cache.snapshots_)::iterator> erased;
  for (auto it = cache.snapshots_.begin(); it != cache.snapshots_.end(); ++it) {
    const auto matches = [&](const auto& ids) {
      return std::ranges::any_of(ids, [&](const auto& id) { return affected.contains(id); });
    };
    if (matches(it->second->key.object_uuids) || matches(it->second->key.index_uuids)) erased.push_back(it);
  }
  OptimizerStatsPublicationResult result{PublicationStatus::kPublished, erased.size(), owned};
  // From this point onward, only noexcept swaps/erases/destruction are allowed.
  tables_.swap(tables);
  columns_.swap(column_records);
  histograms_.swap(histogram_records);
  mcv_.swap(mcv_records);
  extended_stats_.swap(extended_records);
  indexes_.swap(index_records);
  expressions_.swap(expression_records);
  page_filespaces_.swap(filespace_records);
  published_.swap(published);
  cache.publication_epochs_.swap(epochs);
  for (auto it : erased) cache.snapshots_.erase(it);
  return result;
} catch (const std::bad_alloc&) {
  return {OptimizerStatsPublicationStatus::kResourceExhausted};
} catch (const std::length_error&) {
  return {OptimizerStatsPublicationStatus::kResourceExhausted};
}

std::shared_ptr<const OptimizerStatsSnapshot> OptimizerStatisticsStore::PublishedRelationSnapshot(
    const planner::CanonicalPlannerUuid& relation_uuid) const {
  std::lock_guard lock(mutex_);
  const auto found = published_.find(relation_uuid);
  return found == published_.end() ? nullptr : found->second;
}

void OptimizerStatisticsStore::UpsertTable(TableCardinalityStats stats) {
  std::lock_guard lock(mutex_);
  auto staged = tables_;
  const auto object_uuid = stats.identity.object_uuid;
  const auto catalog_epoch = stats.identity.catalog_epoch;
  const auto stats_epoch = stats.identity.stats_epoch;
  Upsert(&staged, std::move(stats), [&](const TableCardinalityStats& existing) { return existing.identity.object_uuid == object_uuid; });
  InvalidateGlobalPinnedStatsCache("analyze_generation", object_uuid, {}, catalog_epoch, stats_epoch, "stats_refresh");
  tables_.swap(staged);
  published_.erase(object_uuid);
}

void OptimizerStatisticsStore::UpsertColumn(ColumnStats stats) {
  std::lock_guard lock(mutex_);
  auto staged = columns_;
  const auto object_uuid = stats.identity.object_uuid;
  const auto column_uuid = stats.column_uuid;
  const auto catalog_epoch = stats.identity.catalog_epoch;
  const auto stats_epoch = stats.identity.stats_epoch;
  Upsert(&staged, std::move(stats), [&](const ColumnStats& existing) { return existing.identity.object_uuid == object_uuid && existing.column_uuid == column_uuid; });
  InvalidateGlobalPinnedStatsCache("analyze_generation", object_uuid, {}, catalog_epoch, stats_epoch, "stats_refresh");
  columns_.swap(staged);
  published_.erase(object_uuid);
}

void OptimizerStatisticsStore::UpsertHistogram(HistogramStats stats) {
  std::lock_guard lock(mutex_);
  auto staged = histograms_;
  const auto object_uuid = stats.identity.object_uuid;
  const auto column_uuid = stats.column_uuid;
  const auto catalog_epoch = stats.identity.catalog_epoch;
  const auto stats_epoch = stats.identity.stats_epoch;
  Upsert(&staged, std::move(stats), [&](const HistogramStats& existing) { return existing.identity.object_uuid == object_uuid && existing.column_uuid == column_uuid; });
  InvalidateGlobalPinnedStatsCache("analyze_generation", object_uuid, {}, catalog_epoch, stats_epoch, "stats_refresh");
  histograms_.swap(staged);
  published_.erase(object_uuid);
}

void OptimizerStatisticsStore::UpsertMcv(MostCommonValueStats stats) {
  std::lock_guard lock(mutex_);
  auto staged = mcv_;
  const auto object_uuid = stats.identity.object_uuid;
  const auto column_uuid = stats.column_uuid;
  const auto value_encoded = stats.value_encoded;
  const auto catalog_epoch = stats.identity.catalog_epoch;
  const auto stats_epoch = stats.identity.stats_epoch;
  Upsert(&staged, std::move(stats), [&](const MostCommonValueStats& existing) { return existing.identity.object_uuid == object_uuid && existing.column_uuid == column_uuid && existing.value_encoded == value_encoded; });
  InvalidateGlobalPinnedStatsCache("analyze_generation", object_uuid, {}, catalog_epoch, stats_epoch, "stats_refresh");
  mcv_.swap(staged);
  published_.erase(object_uuid);
}

void OptimizerStatisticsStore::UpsertExtendedStatistic(ExtendedOptimizerStatistic stats) {
  std::lock_guard lock(mutex_);
  auto staged = extended_stats_;
  const auto object_uuid = stats.identity.object_uuid;
  const auto statistic_uuid = stats.identity.statistic_uuid;
  const auto relation_uuid = stats.relation_uuid;
  const auto catalog_epoch = stats.identity.catalog_epoch;
  const auto stats_epoch = stats.identity.stats_epoch;
  Upsert(&staged, std::move(stats), [&](const ExtendedOptimizerStatistic& existing) {
    return existing.identity.statistic_uuid == statistic_uuid ||
           (existing.relation_uuid == relation_uuid && existing.identity.statistic_uuid == statistic_uuid);
  });
  InvalidateGlobalPinnedStatsCache("analyze_generation", object_uuid, {}, catalog_epoch, stats_epoch, "stats_refresh");
  extended_stats_.swap(staged);
  published_.erase(object_uuid);
}

void OptimizerStatisticsStore::UpsertIndex(IndexStats stats) {
  std::lock_guard lock(mutex_);
  auto staged = indexes_;
  const auto index_uuid = stats.index_uuid;
  const auto object_uuid = stats.identity.object_uuid;
  const auto catalog_epoch = stats.identity.catalog_epoch;
  const auto stats_epoch = stats.identity.stats_epoch;
  Upsert(&staged, std::move(stats), [&](const IndexStats& existing) { return existing.index_uuid == index_uuid; });
  InvalidateGlobalPinnedStatsCache("analyze_generation", object_uuid, index_uuid, catalog_epoch, stats_epoch, "stats_refresh");
  indexes_.swap(staged);
  published_.erase(object_uuid);
}

void OptimizerStatisticsStore::UpsertExpression(ExpressionStats stats) {
  std::lock_guard lock(mutex_);
  auto staged = expressions_;
  const auto object_uuid = stats.identity.object_uuid;
  const auto expression_digest = stats.expression_digest;
  const auto catalog_epoch = stats.identity.catalog_epoch;
  const auto stats_epoch = stats.identity.stats_epoch;
  Upsert(&staged, std::move(stats), [&](const ExpressionStats& existing) { return existing.identity.object_uuid == object_uuid && existing.expression_digest == expression_digest; });
  InvalidateGlobalPinnedStatsCache("analyze_generation", object_uuid, {}, catalog_epoch, stats_epoch, "stats_refresh");
  expressions_.swap(staged);
  published_.erase(object_uuid);
}

void OptimizerStatisticsStore::UpsertPageFilespace(PageFilespaceStats stats) {
  std::lock_guard lock(mutex_);
  auto staged = page_filespaces_;
  const auto filespace_uuid = stats.filespace_uuid;
  const auto page_family = stats.page_family;
  const auto object_uuid = stats.identity.object_uuid;
  const auto catalog_epoch = stats.identity.catalog_epoch;
  const auto stats_epoch = stats.identity.stats_epoch;
  Upsert(&staged, std::move(stats), [&](const PageFilespaceStats& existing) { return existing.filespace_uuid == filespace_uuid && existing.page_family == page_family; });
  InvalidateGlobalPinnedStatsCache("analyze_generation", object_uuid, filespace_uuid, catalog_epoch, stats_epoch, "stats_refresh");
  page_filespaces_.swap(staged);
  published_.erase(object_uuid);
}

std::optional<TableCardinalityStats> OptimizerStatisticsStore::FindTable(const planner::CanonicalPlannerUuid& relation_uuid) const {
  std::lock_guard lock(mutex_);
  auto it = std::find_if(tables_.begin(), tables_.end(), [&](const TableCardinalityStats& stats) { return stats.identity.object_uuid == relation_uuid; });
  if (it == tables_.end()) return std::nullopt;
  return *it;
}

std::optional<ColumnStats> OptimizerStatisticsStore::FindColumn(const planner::CanonicalPlannerUuid& relation_uuid, const planner::CanonicalPlannerUuid& column_uuid) const {
  std::lock_guard lock(mutex_);
  auto it = std::find_if(columns_.begin(), columns_.end(), [&](const ColumnStats& stats) { return stats.identity.object_uuid == relation_uuid && stats.column_uuid == column_uuid; });
  if (it == columns_.end()) return std::nullopt;
  return *it;
}

std::vector<ExtendedOptimizerStatistic> OptimizerStatisticsStore::FindExtendedStatisticsForRelation(
    const planner::CanonicalPlannerUuid& relation_uuid) const {
  std::lock_guard lock(mutex_);
  std::vector<ExtendedOptimizerStatistic> out;
  for (const auto& stats : extended_stats_) {
    if (stats.relation_uuid == relation_uuid || stats.identity.object_uuid == relation_uuid) {
      out.push_back(stats);
    }
  }
  return out;
}

std::optional<IndexStats> OptimizerStatisticsStore::FindIndex(const planner::CanonicalPlannerUuid& index_uuid) const {
  std::lock_guard lock(mutex_);
  auto it = std::find_if(indexes_.begin(), indexes_.end(), [&](const IndexStats& stats) { return stats.index_uuid == index_uuid; });
  if (it == indexes_.end()) return std::nullopt;
  return *it;
}

std::optional<PageFilespaceStats> OptimizerStatisticsStore::FindFilespace(const planner::CanonicalPlannerUuid& filespace_uuid, const std::string& page_family) const {
  std::lock_guard lock(mutex_);
  auto it = std::find_if(page_filespaces_.begin(), page_filespaces_.end(), [&](const PageFilespaceStats& stats) { return stats.filespace_uuid == filespace_uuid && stats.page_family == page_family; });
  if (it == page_filespaces_.end()) return std::nullopt;
  return *it;
}

void OptimizerStatisticsStore::MarkStaleByObject(const planner::CanonicalPlannerUuid& object_uuid, std::uint64_t catalog_epoch) {
  std::lock_guard lock(mutex_);
  InvalidateGlobalPinnedStatsCache("statistics_stale", object_uuid, {}, catalog_epoch, 0, "statistics_stale");
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
  published_.clear();
}

OptimizerStatsSnapshot OptimizerStatisticsStore::Snapshot(planner::CanonicalPlannerUuid snapshot_id) const {
  std::lock_guard lock(mutex_);
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
  // Opaque content binding bytes, not a textual UUID or issued snapshot identity.
  planner::CanonicalPlannerBindingBytes out("optimizer-pinned-statistics-v2");
  out.Number(key.catalog_epoch); out.Number(key.security_epoch);
  out.Number(key.resource_policy_epoch); out.Number(key.name_resolution_epoch);
  out.Number(key.stats_epoch); out.Text(key.descriptor_set_digest);
  out.Identity(key.security_policy_identity); out.Identity(key.redaction_policy_identity);
  const auto objects = SortedUnique(key.object_uuids);
  const auto indexes = SortedUnique(key.index_uuids);
  out.Number(objects.size());
  for (const auto& uuid : objects) out.Identity(uuid);
  out.Number(indexes.size());
  for (const auto& uuid : indexes) out.Identity(uuid);
  return std::move(out).Take();
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
  const auto valid_uuid = [](const auto& uuid) { return scratchbird::core::uuid::IsEngineIdentityUuid(uuid); };
  if (!std::ranges::all_of(key.object_uuids, valid_uuid) ||
      !std::ranges::all_of(key.index_uuids, valid_uuid) ||
      SortedUnique(key.object_uuids).size() != key.object_uuids.size() ||
      SortedUnique(key.index_uuids).size() != key.index_uuids.size()) {
    return StatsRefusal("SB_OPT_PINNED_STATS_OBJECT_UUID_REQUIRED",
                        "invalid or duplicate binary object/index identity", cache_key);
  }
  if (key.object_uuids.empty()) {
    return StatsRefusal("SB_OPT_PINNED_STATS_OBJECT_UUID_REQUIRED", "object UUID is required", cache_key);
  }
  if (!scratchbird::core::uuid::IsEngineIdentityUuid(key.security_policy_identity)) {
    return StatsRefusal("SB_OPT_PINNED_STATS_SECURITY_POLICY_REQUIRED",
                        "security policy identity is required",
                        cache_key);
  }
  if (!scratchbird::core::uuid::IsEngineIdentityUuid(key.redaction_policy_identity)) {
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
    const auto stale = [&](const auto& ids) {
      return std::ranges::any_of(ids, [&](const auto& id) {
        const auto floor = publication_epochs_.find(id);
        return floor != publication_epochs_.end() &&
            (stored->key.catalog_epoch < floor->second.catalog ||
             stored->key.stats_epoch < floor->second.stats);
      });
    };
    const auto stale_records = [&](const auto& records) {
      return std::ranges::any_of(records, [&](const auto& record) {
        const auto floor = publication_epochs_.find(record.identity.object_uuid);
        return floor != publication_epochs_.end() &&
            (record.identity.catalog_epoch < floor->second.catalog ||
             record.identity.stats_epoch < floor->second.stats);
      });
    };
    if (stale(stored->key.object_uuids) || stale(stored->key.index_uuids) ||
        stale_records(stored->stats_snapshot.tables) || stale_records(stored->stats_snapshot.columns) ||
        stale_records(stored->stats_snapshot.histograms) || stale_records(stored->stats_snapshot.mcv) ||
        stale_records(stored->stats_snapshot.extended_stats) || stale_records(stored->stats_snapshot.indexes) ||
        stale_records(stored->stats_snapshot.expressions) || stale_records(stored->stats_snapshot.page_filespaces)) {
      return StatsRefusal("SB_OPT_PINNED_STATS_CACHE_MISS",
                          "snapshot predates published statistics", validation.cache_key);
    }
    // Prepare response storage before mutating the live map.
    validation.diagnostic_code = "SB_OPT_PINNED_STATS_PUT";
    snapshots_.insert_or_assign(validation.cache_key, stored);
  }
  validation.snapshot = std::move(stored);
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
  auto epochs = publication_epochs_;
  const auto advance = [&](const auto& id) {
    if (id.is_nil() || (event.new_catalog_epoch == 0 && event.new_stats_epoch == 0)) return;
    auto& floor = epochs[id];
    floor.catalog = std::max(floor.catalog, event.new_catalog_epoch);
    floor.stats = std::max(floor.stats, event.new_stats_epoch);
  };
  advance(event.object_uuid);
  advance(event.index_uuid);
  std::vector<decltype(snapshots_)::iterator> erased;
  for (auto it = snapshots_.begin(); it != snapshots_.end(); ++it) {
    if (!StatsEventInvalidatesSnapshot(*it->second, event)) {
      continue;
    }
    OptimizerPinnedStatsInvalidatedEntry entry;
    entry.cache_key = it->first;
    entry.reason = reason;
    entry.object_uuids = it->second->key.object_uuids;
    entry.index_uuids = it->second->key.index_uuids;
    result.invalidated_entries.push_back(std::move(entry));
    erased.push_back(it);
  }
  // No live entry is erased until every evidence/iterator allocation succeeds.
  publication_epochs_.swap(epochs);
  for (auto it : erased) snapshots_.erase(it);
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

std::optional<OptimizerStatisticsCatalog> OptimizerStatisticsStore::ToLegacyCatalog() const try {
  std::lock_guard lock(mutex_);
  OptimizerStatisticsCatalog catalog;
  for (const auto& table : tables_) {
    if (!catalog.Add(MakeUnsignedStatistic("row_count", "relation", OptimizerStatisticTarget::Object(table.identity.object_uuid), table.row_count, table.identity.source, table.identity.stats_epoch, 0, table.identity.confidence, OptimizerStatsIdentityIsUsable(table.identity)))) return std::nullopt;
    if (!catalog.Add(MakeUnsignedStatistic("visible_row_count", "relation", OptimizerStatisticTarget::Object(table.identity.object_uuid), table.visible_row_count, table.identity.source, table.identity.stats_epoch, 0, table.identity.confidence, OptimizerStatsIdentityIsUsable(table.identity)))) return std::nullopt;
    if (!catalog.Add(MakeUnsignedStatistic("relation_visible_version_count", "relation", OptimizerStatisticTarget::Object(table.identity.object_uuid), table.visible_row_count, table.identity.source, table.identity.stats_epoch, 0, table.identity.confidence, OptimizerStatsIdentityIsUsable(table.identity)))) return std::nullopt;
    if (!catalog.Add(MakeUnsignedStatistic("page_count", "relation", OptimizerStatisticTarget::Object(table.identity.object_uuid), table.page_count, table.identity.source, table.identity.stats_epoch, 0, table.identity.confidence, OptimizerStatsIdentityIsUsable(table.identity)))) return std::nullopt;
    if (!catalog.Add(MakeUnsignedStatistic("average_row_bytes", "relation", OptimizerStatisticTarget::Object(table.identity.object_uuid), table.average_row_bytes, table.identity.source, table.identity.stats_epoch, 0, table.identity.confidence, OptimizerStatsIdentityIsUsable(table.identity)))) return std::nullopt;
  }
  for (const auto& column : columns_) {
    const bool usable = OptimizerStatsIdentityIsUsable(column.identity);
    if (!catalog.Add(MakeUnsignedStatistic("column_ndv", "column", OptimizerStatisticTarget::Object(column.column_uuid), column.distinct_count, column.identity.source, column.identity.stats_epoch, 0, column.identity.confidence, usable))) return std::nullopt;
    if (!catalog.Add(MakeStatistic("column_null_fraction", "column", OptimizerStatisticTarget::Object(column.column_uuid), column.null_fraction, column.identity.source, column.identity.stats_epoch, 0, column.identity.confidence, usable))) return std::nullopt;
    if (!catalog.Add(MakeUnsignedStatistic("column_average_width_bytes", "column", OptimizerStatisticTarget::Object(column.column_uuid), column.average_width_bytes, column.identity.source, column.identity.stats_epoch, 0, column.identity.confidence, usable))) return std::nullopt;
    if (!catalog.Add(MakeStatistic("column_correlation", "column", OptimizerStatisticTarget::Object(column.column_uuid), column.correlation, column.identity.source, column.identity.stats_epoch, 0, column.identity.confidence, usable, false, OptimizerStatisticValueDomain::kSignedCorrelation))) return std::nullopt;
    if (!catalog.Add(MakeUnsignedStatistic("column_sample_rows", "column", OptimizerStatisticTarget::Object(column.column_uuid), column.sample_rows, column.identity.source, column.identity.stats_epoch, 0, column.identity.confidence, usable))) return std::nullopt;
    if (!catalog.Add(MakeUnsignedStatistic("column_hll_estimated_distinct", "column", OptimizerStatisticTarget::Object(column.column_uuid), column.hyperloglog_estimated_distinct, column.identity.source, column.identity.stats_epoch, 0, column.identity.confidence, usable))) return std::nullopt;
    if (!catalog.Add(MakeStatistic("column_hll_relative_error_ppm", "column", OptimizerStatisticTarget::Object(column.column_uuid), column.hyperloglog_relative_error * 1000000.0, column.identity.source, column.identity.stats_epoch, 0, column.identity.confidence, usable))) return std::nullopt;
  }
  for (const auto& histogram : histograms_) {
    const bool usable = OptimizerStatsIdentityIsUsable(histogram.identity);
    double fraction = 0.0;
    std::uint64_t rows = 0;
    for (const auto& bucket : histogram.buckets) {
      fraction += bucket.fraction;
      if (bucket.row_count > std::numeric_limits<std::uint64_t>::max() - rows) return std::nullopt;
      rows += bucket.row_count;
    }
    if (!catalog.Add(MakeUnsignedStatistic("histogram_bucket_count", "histogram", OptimizerStatisticTarget::Object(histogram.column_uuid), histogram.buckets.size(), histogram.identity.source, histogram.identity.stats_epoch, 0, histogram.identity.confidence, usable))) return std::nullopt;
    if (!catalog.Add(MakeStatistic("histogram_covered_fraction", "histogram", OptimizerStatisticTarget::Object(histogram.column_uuid), fraction, histogram.identity.source, histogram.identity.stats_epoch, 0, histogram.identity.confidence, usable))) return std::nullopt;
    if (!catalog.Add(MakeUnsignedStatistic("histogram_row_count", "histogram", OptimizerStatisticTarget::Object(histogram.column_uuid), rows, histogram.identity.source, histogram.identity.stats_epoch, 0, histogram.identity.confidence, usable))) return std::nullopt;
  }
  for (const auto& mcv_value : mcv_) {
    if (!catalog.Add(MakeStatistic("mcv_frequency", "mcv", OptimizerStatisticTarget::Object(mcv_value.column_uuid), mcv_value.frequency, mcv_value.identity.source, mcv_value.identity.stats_epoch, 0, mcv_value.identity.confidence, OptimizerStatsIdentityIsUsable(mcv_value.identity)))) return std::nullopt;
  }
  for (const auto& stats : extended_stats_) {
    const bool usable = OptimizerStatsIdentityIsUsable(stats.identity);
    if (!catalog.Add(MakeUnsignedStatistic("extended_multi_column_distinct_count", "extended_stats", OptimizerStatisticTarget::Object(stats.identity.statistic_uuid), stats.multi_column_distinct_count, stats.identity.source, stats.identity.stats_epoch, 0, stats.identity.confidence, usable))) return std::nullopt;
    if (!catalog.Add(MakeStatistic("extended_functional_dependency_strength", "extended_stats", OptimizerStatisticTarget::Object(stats.identity.statistic_uuid), stats.functional_dependency_strength, stats.identity.source, stats.identity.stats_epoch, 0, stats.identity.confidence, usable))) return std::nullopt;
    if (!catalog.Add(MakeStatistic("extended_correlation_coefficient", "extended_stats", OptimizerStatisticTarget::Object(stats.identity.statistic_uuid), stats.correlation_coefficient, stats.identity.source, stats.identity.stats_epoch, 0, stats.identity.confidence, usable, false, OptimizerStatisticValueDomain::kSignedCorrelation))) return std::nullopt;
    if (!catalog.Add(MakeStatistic("extended_histogram_selectivity", "extended_stats", OptimizerStatisticTarget::Object(stats.identity.statistic_uuid), stats.histogram_selectivity, stats.identity.source, stats.identity.stats_epoch, 0, stats.identity.confidence, usable))) return std::nullopt;
    if (!catalog.Add(MakeStatistic("extended_sampled_dependency_selectivity", "extended_stats", OptimizerStatisticTarget::Object(stats.identity.statistic_uuid), stats.sampled_dependency_selectivity, stats.identity.source, stats.identity.stats_epoch, 0, stats.identity.confidence, usable))) return std::nullopt;
  }
  for (const auto& index : indexes_) {
    if (!catalog.Add(MakeUnsignedStatistic("index_height", "index", OptimizerStatisticTarget::Object(index.index_uuid), index.height, index.identity.source, index.identity.stats_epoch, 0, index.identity.confidence, OptimizerStatsIdentityIsUsable(index.identity)))) return std::nullopt;
    if (!catalog.Add(MakeUnsignedStatistic("index_depth", "index", OptimizerStatisticTarget::Object(index.index_uuid), index.height, index.identity.source, index.identity.stats_epoch, 0, index.identity.confidence, OptimizerStatsIdentityIsUsable(index.identity)))) return std::nullopt;
    if (!catalog.Add(MakeUnsignedStatistic("index_leaf_pages", "index", OptimizerStatisticTarget::Object(index.index_uuid), index.leaf_pages, index.identity.source, index.identity.stats_epoch, 0, index.identity.confidence, OptimizerStatsIdentityIsUsable(index.identity)))) return std::nullopt;
    if (!catalog.Add(MakeUnsignedStatistic("index_distinct_keys", "index", OptimizerStatisticTarget::Object(index.index_uuid), index.distinct_keys, index.identity.source, index.identity.stats_epoch, 0, index.identity.confidence, OptimizerStatsIdentityIsUsable(index.identity)))) return std::nullopt;
    if (!catalog.Add(MakeStatistic("index_clustering_factor", "index", OptimizerStatisticTarget::Object(index.index_uuid), index.clustering_factor, index.identity.source, index.identity.stats_epoch, 0, index.identity.confidence, OptimizerStatsIdentityIsUsable(index.identity)))) return std::nullopt;
    if (!catalog.Add(MakeStatistic("index_fragmentation_ratio", "index", OptimizerStatisticTarget::Object(index.index_uuid), index.fragmentation_ratio, index.identity.source, index.identity.stats_epoch, 0, index.identity.confidence, OptimizerStatsIdentityIsUsable(index.identity)))) return std::nullopt;
    if (!catalog.Add(MakeStatistic("index_visibility_coverage", "index", OptimizerStatisticTarget::Object(index.index_uuid), index.rebuild_in_progress ? 0.0 : index.visibility_coverage, index.identity.source, index.identity.stats_epoch, 0, index.identity.confidence, OptimizerStatsIdentityIsUsable(index.identity)))) return std::nullopt;
    if (!catalog.Add(MakeStatistic("index_predicate_coverage", "index", OptimizerStatisticTarget::Object(index.index_uuid), index.predicate_coverage, index.identity.source, index.identity.stats_epoch, 0, index.identity.confidence, OptimizerStatsIdentityIsUsable(index.identity)))) return std::nullopt;
    if (!catalog.Add(MakeStatistic("index_false_positive_ratio", "index", OptimizerStatisticTarget::Object(index.index_uuid), index.false_positive_ratio, index.identity.source, index.identity.stats_epoch, 0, index.identity.confidence, OptimizerStatsIdentityIsUsable(index.identity)))) return std::nullopt;
    if (!catalog.Add(MakeStatistic("index_route_benchmark_clean", "index", OptimizerStatisticTarget::Object(index.index_uuid), index.route_benchmark_clean ? 1.0 : 0.0, index.identity.source, index.identity.stats_epoch, 0, index.identity.confidence, OptimizerStatsIdentityIsUsable(index.identity)))) return std::nullopt;
  }
  for (const auto& filespace : page_filespaces_) {
    const bool usable = OptimizerStatsIdentityIsUsable(filespace.identity);
    if (!catalog.Add(MakeUnsignedStatistic("filespace_available_pages", "filespace", OptimizerStatisticTarget::Object(filespace.filespace_uuid), filespace.free_pages, filespace.identity.source, filespace.identity.stats_epoch, 0, filespace.identity.confidence, usable))) return std::nullopt;
    if (!catalog.Add(MakeStatistic("page_family_read_latency_microseconds", "page_family", OptimizerStatisticTarget::Object(filespace.filespace_uuid), filespace.sequential_latency_score * 1000.0, filespace.identity.source, filespace.identity.stats_epoch, 0, filespace.identity.confidence, usable))) return std::nullopt;
    if (!catalog.Add(MakeStatistic("io_latency_multiplier", "page_family", OptimizerStatisticTarget::Object(filespace.filespace_uuid), filespace.sequential_latency_score, filespace.identity.source, filespace.identity.stats_epoch, 0, filespace.identity.confidence, usable))) return std::nullopt;
  }
  for (const auto& expression : expressions_) {
    const bool usable = OptimizerStatsIdentityIsUsable(expression.identity);
    if (!catalog.Add(MakeUnsignedStatistic("expression_distinct_count", "expression", OptimizerStatisticTarget::Object(expression.identity.statistic_uuid), expression.distinct_count, expression.identity.source, expression.identity.stats_epoch, 0, expression.identity.confidence, usable))) return std::nullopt;
    if (!catalog.Add(MakeStatistic("expression_null_fraction", "expression", OptimizerStatisticTarget::Object(expression.identity.statistic_uuid), expression.null_fraction, expression.identity.source, expression.identity.stats_epoch, 0, expression.identity.confidence, usable))) return std::nullopt;
  }
  return catalog;
} catch (const std::bad_alloc&) {
  return std::nullopt;
} catch (const std::length_error&) {
  return std::nullopt;
}

std::optional<TableCardinalityStats> BuildTableStatsFromAnalyzeSample(const AnalyzeSampleInput& input) {
  if (!scratchbird::core::uuid::IsEngineIdentityUuid(input.relation_uuid) ||
      input.stats_epoch == 0 || input.catalog_epoch == 0 ||
      input.sampled_rows > input.total_rows_estimate) return std::nullopt;
  const auto issued = scratchbird::core::uuid::IssueRuntimeIdentityV7();
  if (!issued) return std::nullopt;
  TableCardinalityStats stats;
  stats.identity.object_uuid = input.relation_uuid;
  stats.identity.statistic_uuid = *issued;
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
  if (!scratchbird::core::uuid::IsEngineIdentityUuid(snapshot.snapshot_id)) statuses.push_back(Status(false, "SB_OPT_STATS_SNAPSHOT_ID_REQUIRED", "snapshot"));
  for (const auto& table : snapshot.tables) {
    ValidateIdentity(table.identity, "table", &statuses);
    if (table.visible_row_count > table.row_count)
      statuses.push_back(Status(false, "SB_OPT_STATS_NOT_USABLE", table.identity.object_uuid));
  }
  for (const auto& column : snapshot.columns) {
    ValidateIdentity(column.identity, "column", &statuses);
    if (!scratchbird::core::uuid::IsEngineIdentityUuid(column.column_uuid)) statuses.push_back(Status(false, "SB_OPT_STATS_COLUMN_UUID_REQUIRED", column.identity.object_uuid));
    if (!std::isfinite(column.null_fraction) || column.null_fraction < 0.0 || column.null_fraction > 1.0) statuses.push_back(Status(false, "SB_OPT_STATS_COLUMN_NULL_FRACTION_INVALID", column.column_uuid));
    if (!std::isfinite(column.correlation) || column.correlation < -1.0 || column.correlation > 1.0) statuses.push_back(Status(false, "SB_OPT_STATS_COLUMN_CORRELATION_INVALID", column.column_uuid));
    if (column.sample_rows != 0 && column.sample_method.empty()) statuses.push_back(Status(false, "SB_OPT_STATS_SAMPLE_METHOD_REQUIRED", column.column_uuid));
    if (column.sample_rows != 0 && column.sample_provenance_digest.empty()) statuses.push_back(Status(false, "SB_OPT_STATS_SAMPLE_PROVENANCE_REQUIRED", column.column_uuid));
    if (column.hyperloglog_register_count != 0 && column.hyperloglog_estimated_distinct == 0 &&
        column.distinct_count != 0) statuses.push_back(Status(false, "SB_OPT_STATS_HLL_NDV_REQUIRED", column.column_uuid));
    if (!std::isfinite(column.hyperloglog_relative_error) || column.hyperloglog_relative_error < 0.0 || column.hyperloglog_relative_error > 1.0) statuses.push_back(Status(false, "SB_OPT_STATS_HLL_ERROR_INVALID", column.column_uuid));
  }
  for (const auto& histogram : snapshot.histograms) {
    ValidateIdentity(histogram.identity, "histogram", &statuses);
    if (!scratchbird::core::uuid::IsEngineIdentityUuid(histogram.column_uuid)) statuses.push_back(Status(false, "SB_OPT_STATS_HISTOGRAM_COLUMN_UUID_REQUIRED", histogram.identity.object_uuid));
    double covered_fraction = 0.0;
    for (const auto& bucket : histogram.buckets) {
      covered_fraction += bucket.fraction;
      if (!std::isfinite(bucket.fraction) || bucket.fraction < 0.0 || bucket.fraction > 1.0)
        statuses.push_back(Status(false, "SB_OPT_STATS_NOT_USABLE", histogram.column_uuid));
    }
    if (!std::isfinite(covered_fraction) || covered_fraction > 1.000001)
      statuses.push_back(Status(false, "SB_OPT_STATS_NOT_USABLE", histogram.column_uuid));
    if (histogram.buckets.empty()) statuses.push_back(Status(false, "SB_OPT_STATS_HISTOGRAM_BUCKET_REQUIRED", histogram.column_uuid));
  }
  for (const auto& mcv_value : snapshot.mcv) {
    ValidateIdentity(mcv_value.identity, "mcv", &statuses);
    if (!scratchbird::core::uuid::IsEngineIdentityUuid(mcv_value.column_uuid)) statuses.push_back(Status(false, "SB_OPT_STATS_MCV_COLUMN_UUID_REQUIRED", mcv_value.identity.object_uuid));
    if (!std::isfinite(mcv_value.frequency) || mcv_value.frequency < 0.0 || mcv_value.frequency > 1.0) statuses.push_back(Status(false, "SB_OPT_STATS_MCV_FREQUENCY_INVALID", mcv_value.column_uuid));
  }
  for (const auto& stats : snapshot.extended_stats) {
    ValidateIdentity(stats.identity, "extended_stats", &statuses);
    if (!scratchbird::core::uuid::IsEngineIdentityUuid(stats.relation_uuid)) statuses.push_back(Status(false, "SB_OPT_EXTENDED_STATS_RELATION_UUID_REQUIRED", stats.identity.statistic_uuid));
    if (stats.column_uuids.empty() && stats.document_path_digests.empty()) statuses.push_back(Status(false, "SB_OPT_EXTENDED_STATS_SHAPE_REQUIRED", stats.identity.statistic_uuid));
    if (!std::isfinite(stats.functional_dependency_strength) || stats.functional_dependency_strength < 0.0 || stats.functional_dependency_strength > 1.0) statuses.push_back(Status(false, "SB_OPT_EXTENDED_STATS_DEPENDENCY_INVALID", stats.identity.statistic_uuid));
    if (!std::isfinite(stats.correlation_coefficient) || stats.correlation_coefficient < -1.0 || stats.correlation_coefficient > 1.0) statuses.push_back(Status(false, "SB_OPT_EXTENDED_STATS_CORRELATION_INVALID", stats.identity.statistic_uuid));
    if (!std::isfinite(stats.histogram_selectivity) || stats.histogram_selectivity < 0.0 || stats.histogram_selectivity > 1.0) statuses.push_back(Status(false, "SB_OPT_EXTENDED_STATS_HISTOGRAM_INVALID", stats.identity.statistic_uuid));
    if (!std::isfinite(stats.sampled_dependency_selectivity) || stats.sampled_dependency_selectivity < 0.0 || stats.sampled_dependency_selectivity > 1.0) statuses.push_back(Status(false, "SB_OPT_EXTENDED_STATS_SAMPLE_INVALID", stats.identity.statistic_uuid));
    if (!std::isfinite(stats.observed_selectivity_error) || stats.observed_selectivity_error < -1.0 || stats.observed_selectivity_error > 1.0) statuses.push_back(Status(false, "SB_OPT_EXTENDED_STATS_OBSERVED_ERROR_INVALID", stats.identity.statistic_uuid));
    for (const auto& entry : stats.joint_mcv) {
      if (!std::isfinite(entry.frequency) || entry.frequency < 0.0 || entry.frequency > 1.0) statuses.push_back(Status(false, "SB_OPT_EXTENDED_STATS_JOINT_MCV_FREQUENCY_INVALID", stats.identity.statistic_uuid));
      if (!entry.value_encodings.empty() && !stats.column_uuids.empty() && entry.value_encodings.size() != stats.column_uuids.size()) statuses.push_back(Status(false, "SB_OPT_EXTENDED_STATS_JOINT_MCV_SHAPE_INVALID", stats.identity.statistic_uuid));
    }
    if (stats.finality_authority) statuses.push_back(Status(false, "SB_OPT_EXTENDED_STATS_FINALITY_FORBIDDEN", stats.identity.statistic_uuid));
    if (!stats.mga_visibility_recheck_required || !stats.security_recheck_required) statuses.push_back(Status(false, "SB_OPT_EXTENDED_STATS_RECHECK_REQUIRED", stats.identity.statistic_uuid));
  }
  for (const auto& index : snapshot.indexes) {
    ValidateIdentity(index.identity, "index", &statuses);
    if (!std::isfinite(index.clustering_factor) || !std::isfinite(index.fragmentation_ratio) ||
        !std::isfinite(index.contention_ratio) || index.clustering_factor < 0.0 ||
        index.fragmentation_ratio < 0.0 || index.contention_ratio < 0.0)
      statuses.push_back(Status(false, "SB_OPT_STATS_NOT_USABLE", index.index_uuid));
    if (!scratchbird::core::uuid::IsEngineIdentityUuid(index.index_uuid)) statuses.push_back(Status(false, "SB_OPT_STATS_INDEX_UUID_REQUIRED", index.identity.object_uuid));
    if (index.index_family.empty()) statuses.push_back(Status(false, "SB_OPT_STATS_INDEX_FAMILY_REQUIRED", index.index_uuid));
    if (!std::isfinite(index.visibility_coverage) || index.visibility_coverage < 0.0 || index.visibility_coverage > 1.0) statuses.push_back(Status(false, "SB_OPT_STATS_INDEX_VISIBILITY_COVERAGE_INVALID", index.index_uuid));
    if (!std::isfinite(index.predicate_coverage) || index.predicate_coverage < 0.0 || index.predicate_coverage > 1.0) statuses.push_back(Status(false, "SB_OPT_STATS_INDEX_PREDICATE_COVERAGE_INVALID", index.index_uuid));
    if (!std::isfinite(index.false_positive_ratio) || index.false_positive_ratio < 0.0 || index.false_positive_ratio > 1.0) statuses.push_back(Status(false, "SB_OPT_STATS_INDEX_FALSE_POSITIVE_RATIO_INVALID", index.index_uuid));
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
    if (!std::isfinite(expression.null_fraction) || expression.null_fraction < 0.0 ||
        expression.null_fraction > 1.0)
      statuses.push_back(Status(false, "SB_OPT_STATS_NOT_USABLE", expression.identity.object_uuid));
    if (expression.expression_digest.empty()) statuses.push_back(Status(false, "SB_OPT_STATS_EXPRESSION_DIGEST_REQUIRED", expression.identity.object_uuid));
  }
  for (const auto& filespace : snapshot.page_filespaces) {
    ValidateIdentity(filespace.identity, "page_filespace", &statuses);
    if (!std::isfinite(filespace.sequential_latency_score) || !std::isfinite(filespace.random_latency_score) ||
        !std::isfinite(filespace.health_score) || filespace.sequential_latency_score < 0.0 ||
        filespace.random_latency_score < 0.0 || filespace.health_score < 0.0)
      statuses.push_back(Status(false, "SB_OPT_STATS_NOT_USABLE", filespace.filespace_uuid));
    if (!scratchbird::core::uuid::IsEngineIdentityUuid(filespace.filespace_uuid)) statuses.push_back(Status(false, "SB_OPT_STATS_FILESPACE_UUID_REQUIRED", filespace.identity.object_uuid));
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
  FinalizeCostVector(&cost);
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
    if (index.index_family.empty() || index.index_uuid.is_nil()) {
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

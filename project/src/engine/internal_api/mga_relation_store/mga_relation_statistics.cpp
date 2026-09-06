// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "mga_relation_store/mga_relation_store.hpp"

#include "api_diagnostics.hpp"
#include "crud_support/crud_store.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace scratchbird::engine::internal_api {
namespace {

// SEARCH_KEY: SB_ENGINE_MGA_RELATION_STATISTICS_IMPLEMENTATION_AUTHORITY
// Owns derived relation-size estimates only. Estimates are observations over
// the canonical MGA read facade and never visibility or finality authority.

void AddBytes(std::uint64_t* total, std::uint64_t bytes) {
  if (total == nullptr) { return; }
  const std::uint64_t max = std::numeric_limits<std::uint64_t>::max();
  *total = bytes > max - *total ? max : *total + bytes;
}

std::uint64_t TextBytes(const std::string& value) {
  return static_cast<std::uint64_t>(value.size());
}

std::uint64_t PairBytes(
    const std::vector<std::pair<std::string, std::string>>& values) {
  std::uint64_t total = 0;
  for (const auto& [name, value] : values) {
    AddBytes(&total, 8 + TextBytes(name) + TextBytes(value));
  }
  return total;
}

std::uint64_t StringListBytes(const std::vector<std::string>& values) {
  std::uint64_t total = 0;
  for (const auto& value : values) {
    AddBytes(&total, 8 + TextBytes(value));
  }
  return total;
}

std::uint64_t TableMetadataEstimateBytes(const CrudTableRecord& table) {
  std::uint64_t total = 96;
  AddBytes(&total, TextBytes(table.table_uuid));
  AddBytes(&total, TextBytes(table.default_name));
  AddBytes(&total, PairBytes(table.columns));
  AddBytes(&total, TextBytes(table.temporary_scope));
  AddBytes(&total, TextBytes(table.temporary_session_uuid));
  AddBytes(&total, TextBytes(table.on_commit_action));
  return total;
}

std::uint64_t RowVersionEstimateBytes(const CrudRowVersionRecord& row) {
  std::uint64_t total = 128;
  AddBytes(&total, TextBytes(row.table_uuid));
  AddBytes(&total, TextBytes(row.row_uuid));
  AddBytes(&total, TextBytes(row.version_uuid));
  AddBytes(&total, TextBytes(row.previous_version_uuid));
  AddBytes(&total, PairBytes(row.values));
  return total;
}

std::uint64_t IndexMetadataEstimateBytes(const CrudIndexRecord& index) {
  std::uint64_t total = 128;
  AddBytes(&total, TextBytes(index.index_uuid));
  AddBytes(&total, TextBytes(index.table_uuid));
  AddBytes(&total, TextBytes(index.column_name));
  AddBytes(&total, TextBytes(index.family));
  AddBytes(&total, TextBytes(index.profile));
  AddBytes(&total, TextBytes(index.default_name));
  AddBytes(&total, StringListBytes(index.key_envelopes));
  AddBytes(&total, StringListBytes(index.include_columns));
  AddBytes(&total, TextBytes(index.predicate_kind));
  AddBytes(&total, TextBytes(index.predicate_column));
  AddBytes(&total, TextBytes(index.predicate_value));
  return total;
}

std::uint64_t IndexEntryEstimateBytes(const CrudIndexEntryRecord& entry) {
  std::uint64_t total = 112;
  AddBytes(&total, TextBytes(entry.index_uuid));
  AddBytes(&total, TextBytes(entry.table_uuid));
  AddBytes(&total, TextBytes(entry.column_name));
  AddBytes(&total, TextBytes(entry.family));
  AddBytes(&total, TextBytes(entry.entry_kind));
  AddBytes(&total, TextBytes(entry.key_value));
  AddBytes(&total, TextBytes(entry.payload_value));
  AddBytes(&total, TextBytes(entry.row_uuid));
  AddBytes(&total, TextBytes(entry.version_uuid));
  return total;
}

bool TableUuidSeen(const std::vector<std::string>& seen,
                   const std::string& table_uuid) {
  return std::find(seen.begin(), seen.end(), table_uuid) != seen.end();
}

MgaRelationStatistics EstimateRelationStatisticsFromState(
    const EngineRequestContext& context,
    const RelationReadSnapshot& state,
    const std::string& table_uuid,
    bool include_indexes) {
  MgaRelationStatistics statistics;
  const auto table =
      FindVisibleCrudTable(state, table_uuid, context.local_transaction_id);
  if (!table) { return statistics; }

  statistics.relation_found = true;
  statistics.visible_row_estimate = static_cast<std::uint64_t>(
      VisibleCrudRowsForContext(state, table_uuid, context).size());

  // Stable estimate from persisted MGA relation sidecar payload lengths. This
  // is not page-byte accounting; it is engine-owned row-version/catalog size.
  AddBytes(&statistics.row_store_bytes, TableMetadataEstimateBytes(*table));
  for (const auto& row : state.row_versions) {
    if (row.table_uuid != table_uuid) { continue; }
    ++statistics.retained_row_version_count;
    AddBytes(&statistics.row_store_bytes, RowVersionEstimateBytes(row));
  }
  statistics.table_size_bytes = statistics.row_store_bytes;

  if (include_indexes) {
    const auto indexes = VisibleCrudIndexesForTable(
        state, table_uuid, context.local_transaction_id);
    for (const auto& index : indexes) {
      AddBytes(&statistics.index_store_bytes,
               IndexMetadataEstimateBytes(index));
    }
    for (const auto& entry : state.index_entries) {
      if (entry.table_uuid != table_uuid) { continue; }
      AddBytes(&statistics.index_store_bytes, IndexEntryEstimateBytes(entry));
    }
    AddBytes(&statistics.table_size_bytes, statistics.index_store_bytes);
  }
  return statistics;
}

}  // namespace

MgaRelationStatisticsResult EstimateMgaRelationStatistics(
    const EngineRequestContext& context,
    const std::string& table_uuid,
    bool include_indexes) {
  MgaRelationStatisticsResult result;
  auto loaded = LoadMgaRelationStoreState(context);
  if (!loaded.ok) {
    result.diagnostic = loaded.diagnostic;
    return result;
  }
  const RelationReadSnapshot state =
      BuildCrudCompatibilityStateFromMga(loaded.state);
  result.statistics = EstimateRelationStatisticsFromState(
      context, state, table_uuid, include_indexes);
  result.ok = true;
  result.diagnostic =
      MakeEngineApiDiagnostic("SB_ENGINE_API_OK", "engine.api.ok", {}, false);
  return result;
}

MgaRelationStatisticsResult EstimateMgaCatalogStatistics(
    const EngineRequestContext& context,
    bool include_indexes) {
  MgaRelationStatisticsResult result;
  auto loaded = LoadMgaRelationStoreState(context);
  if (!loaded.ok) {
    result.diagnostic = loaded.diagnostic;
    return result;
  }
  const RelationReadSnapshot state =
      BuildCrudCompatibilityStateFromMga(loaded.state);
  std::vector<std::string> table_uuids;
  for (const auto& table : state.tables) {
    if (table.table_uuid.empty() ||
        TableUuidSeen(table_uuids, table.table_uuid)) {
      continue;
    }
    if (!FindVisibleCrudTable(
            state, table.table_uuid, context.local_transaction_id)) {
      continue;
    }
    table_uuids.push_back(table.table_uuid);
  }
  result.statistics.relation_found = true;
  for (const auto& table_uuid : table_uuids) {
    const auto table_stats = EstimateRelationStatisticsFromState(
        context, state, table_uuid, include_indexes);
    AddBytes(&result.statistics.visible_row_estimate,
             table_stats.visible_row_estimate);
    AddBytes(&result.statistics.retained_row_version_count,
             table_stats.retained_row_version_count);
    AddBytes(&result.statistics.row_store_bytes,
             table_stats.row_store_bytes);
    AddBytes(&result.statistics.index_store_bytes,
             table_stats.index_store_bytes);
    AddBytes(&result.statistics.table_size_bytes,
             table_stats.table_size_bytes);
  }
  result.ok = true;
  result.diagnostic =
      MakeEngineApiDiagnostic("SB_ENGINE_API_OK", "engine.api.ok", {}, false);
  return result;
}

}  // namespace scratchbird::engine::internal_api

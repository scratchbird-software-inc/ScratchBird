// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "direct_bulk_index_cache_projection.hpp"
namespace scratchbird::engine::internal_api::dml::detail {
// SEARCH_KEY: SB_ENGINE_DIRECT_BULK_INDEX_CACHE_PROJECTION_IMPLEMENTATION_AUTHORITY
// Pure advisory projections of supplied entries. No cache admission, durable
// writes, visibility decisions, or transaction finality are performed here.
std::map<EngineUuid, std::set<std::string>> DirectBuildIndexKeyCache(
    const std::vector<CrudIndexEntryRecord>& entries) {
  std::map<EngineUuid, std::set<std::string>> keys_by_index;
  for (const auto& entry : entries) {
    keys_by_index[entry.index_uuid].insert(entry.key_value);
  }
  return keys_by_index;
}

std::map<EngineUuid, std::map<std::string, CrudIndexEntryRecord>>
DirectBuildIndexEntryKeyCache(
    const std::vector<CrudIndexEntryRecord>& entries) {
  std::map<EngineUuid, std::map<std::string, CrudIndexEntryRecord>>
      entry_by_index_key;
  for (const auto& entry : entries) {
    entry_by_index_key[entry.index_uuid][entry.key_value] = entry;
  }
  return entry_by_index_key;
}

std::vector<CrudIndexEntryRecord> DirectIndexEntriesFromExactBatches(
    const EngineRequestContext& context,
    const std::vector<MgaExactIndexEntryAppendBatch>& batches) {
  std::vector<CrudIndexEntryRecord> entries;
  for (const auto& batch : batches) {
    const EngineUuid table_uuid =
        batch.index.table_uuid.is_nil() ? batch.table_uuid
                                       : batch.index.table_uuid;
    for (const auto& exact : batch.entries) {
      CrudIndexEntryRecord entry;
      entry.creator_tx = context.local_transaction_id;
      entry.index_uuid = batch.index.index_uuid;
      entry.table_uuid = table_uuid;
      entry.column_name = batch.index.column_name;
      entry.family = batch.index.family;
      entry.entry_kind = "exact";
      entry.key_value = exact.encoded_key;
      entry.payload_value = exact.payload_value;
      entry.row_uuid = exact.row_uuid;
      entry.version_uuid = exact.version_uuid;
      entries.push_back(std::move(entry));
    }
  }
  return entries;
}

std::vector<CrudIndexEntryRecord> DirectIndexEntriesFromRetailBatches(
    const EngineRequestContext& context,
    const std::vector<MgaIndexEntryAppendBatch>& batches) {
  std::vector<CrudIndexEntryRecord> entries;
  for (const auto& batch : batches) {
    const EngineUuid table_uuid =
        batch.index.table_uuid.is_nil() ? batch.table_uuid
                                       : batch.index.table_uuid;
    for (const auto& row : batch.rows) {
      for (const auto& key : CrudIndexKeysForValues(batch.index, row.values)) {
        CrudIndexEntryRecord entry;
        entry.creator_tx = context.local_transaction_id;
        entry.index_uuid = batch.index.index_uuid;
        entry.table_uuid = table_uuid;
        entry.column_name = batch.index.column_name;
        entry.family = batch.index.family;
        entry.entry_kind = "exact";
        entry.key_value = key;
        entry.payload_value = CrudFieldValue(row.values, batch.index.column_name);
        entry.row_uuid = row.row_uuid;
        entry.version_uuid = row.version_uuid;
        entries.push_back(std::move(entry));
      }
    }
  }
  return entries;
}

} // namespace scratchbird::engine::internal_api::dml::detail

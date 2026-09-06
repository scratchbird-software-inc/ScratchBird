// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "dml/direct_bulk_append_cache.hpp"

#include <algorithm>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace scratchbird::engine::internal_api::dml::detail {

// SEARCH_KEY: SB_ENGINE_DIRECT_BULK_APPEND_CACHE_AUTHORITY
// Advisory cache ownership only; durable MGA state remains authoritative.

namespace {

bool DirectIndexIsUnique(const CrudIndexRecord& index) {
  return index.unique ||
         std::find(index.key_envelopes.begin(),
                   index.key_envelopes.end(),
                   "unique") != index.key_envelopes.end();
}

}  // namespace

struct DirectAppendIndexEntryCacheRecord {
  std::uint64_t row_version_count = 0;
  std::uint64_t metadata_event_sequence = 0;
  std::uint64_t observer_local_transaction_id = 0;
  std::uint64_t savepoint_authority_generation = 0;
  std::vector<CrudIndexEntryRecord> entries;
  std::map<std::string, std::set<std::string>> keys_by_index;
  std::map<std::string, std::map<std::string, CrudIndexEntryRecord>>
      entry_by_index_key;
  bool entry_lookup_materialized = true;
};

std::mutex& DirectAppendIndexEntryCacheMutex() {
  static std::mutex mutex;
  return mutex;
}

std::map<std::string, DirectAppendIndexEntryCacheRecord>&
DirectAppendIndexEntryCache() {
  static std::map<std::string, DirectAppendIndexEntryCacheRecord> cache;
  return cache;
}

std::map<std::string, DirectBulkAppendContextCacheRecord>&
DirectBulkAppendContextCache() {
  static std::map<std::string, DirectBulkAppendContextCacheRecord> cache;
  return cache;
}

std::string DirectAppendIndexEntryCacheKey(const EngineRequestContext& context,
                                           const std::string& table_uuid) {
  return context.database_path + "\n" + table_uuid;
}

std::string DirectBulkAppendContextCacheKey(const EngineRequestContext& context,
                                            const std::string& table_uuid) {
  return context.database_path + "\n" +
         std::to_string(context.local_transaction_id) + "\n" +
         context.session_uuid.canonical + "\n" +
         context.principal_uuid.canonical + "\n" +
         context.current_role_uuid.canonical + "\n" +
         std::to_string(context.catalog_generation_id) + "\n" +
         std::to_string(context.security_epoch) + "\n" +
         std::to_string(CurrentMgaSavepointAuthorityGeneration(context)) +
         "\n" +
         table_uuid;
}

bool DirectAppendIndexCacheAuthorityMatches(
    const DirectAppendIndexEntryCacheRecord& record,
    const EngineRequestContext& context) {
  return record.observer_local_transaction_id ==
             context.local_transaction_id &&
         record.savepoint_authority_generation ==
             CurrentMgaSavepointAuthorityGeneration(context);
}

std::map<std::string, std::set<std::string>> DirectBuildIndexKeyCache(
    const std::vector<CrudIndexEntryRecord>& entries) {
  std::map<std::string, std::set<std::string>> keys_by_index;
  for (const auto& entry : entries) {
    keys_by_index[entry.index_uuid].insert(entry.key_value);
  }
  return keys_by_index;
}

std::map<std::string, std::map<std::string, CrudIndexEntryRecord>>
DirectBuildIndexEntryKeyCache(
    const std::vector<CrudIndexEntryRecord>& entries) {
  std::map<std::string, std::map<std::string, CrudIndexEntryRecord>>
      entry_by_index_key;
  for (const auto& entry : entries) {
    entry_by_index_key[entry.index_uuid][entry.key_value] = entry;
  }
  return entry_by_index_key;
}

bool DirectLookupAppendIndexEntryCache(const EngineRequestContext& context,
                                       const std::string& table_uuid,
                                       std::uint64_t row_version_count,
                                       std::vector<CrudIndexEntryRecord>* entries,
                                       std::map<std::string, std::set<std::string>>* keys_by_index,
                                       std::map<std::string, std::map<std::string, CrudIndexEntryRecord>>* entry_by_index_key) {
  if (entries == nullptr && keys_by_index == nullptr &&
      entry_by_index_key == nullptr) {
    return false;
  }
  const std::uint64_t metadata_event_sequence =
      CurrentMgaRelationMetadataEventSequence(context);
  const std::lock_guard<std::mutex> guard(DirectAppendIndexEntryCacheMutex());
  const auto found = DirectAppendIndexEntryCache().find(
      DirectAppendIndexEntryCacheKey(context, table_uuid));
  if (found == DirectAppendIndexEntryCache().end() ||
      found->second.row_version_count != row_version_count ||
      found->second.metadata_event_sequence != metadata_event_sequence ||
      !DirectAppendIndexCacheAuthorityMatches(found->second, context)) {
    return false;
  }
  if (entries != nullptr) {
    *entries = found->second.entries;
  }
  if (keys_by_index != nullptr) {
    *keys_by_index = found->second.keys_by_index;
  }
  if (entry_by_index_key != nullptr) {
    *entry_by_index_key = found->second.entry_by_index_key;
  }
  return true;
}

bool DirectAppendIndexEntryCacheAvailable(const EngineRequestContext& context,
                                          const std::string& table_uuid,
                                          std::uint64_t row_version_count,
                                          bool require_entry_lookup) {
  const std::uint64_t metadata_event_sequence =
      CurrentMgaRelationMetadataEventSequence(context);
  const std::lock_guard<std::mutex> guard(DirectAppendIndexEntryCacheMutex());
  const auto found = DirectAppendIndexEntryCache().find(
      DirectAppendIndexEntryCacheKey(context, table_uuid));
  return found != DirectAppendIndexEntryCache().end() &&
         found->second.row_version_count == row_version_count &&
         found->second.metadata_event_sequence == metadata_event_sequence &&
         DirectAppendIndexCacheAuthorityMatches(found->second, context) &&
         (!require_entry_lookup || found->second.entry_lookup_materialized);
}

void DirectBuildAppendIndexConflictCaches(
    const EngineRequestContext& context,
    const std::string& table_uuid,
    std::uint64_t row_version_count,
    const std::vector<CrudIndexRecord>& indexes,
    const std::vector<std::vector<std::pair<std::string, std::string>>>& logical_value_batch,
    std::map<std::string, std::set<std::string>>* keys_by_index,
    std::map<std::string, std::map<std::string, CrudIndexEntryRecord>>*
        entry_by_index_key) {
  if (keys_by_index == nullptr && entry_by_index_key == nullptr) {
    return;
  }
  const std::uint64_t metadata_event_sequence =
      CurrentMgaRelationMetadataEventSequence(context);
  const std::lock_guard<std::mutex> guard(DirectAppendIndexEntryCacheMutex());
  const auto found = DirectAppendIndexEntryCache().find(
      DirectAppendIndexEntryCacheKey(context, table_uuid));
  if (found == DirectAppendIndexEntryCache().end() ||
      found->second.row_version_count != row_version_count ||
      found->second.metadata_event_sequence != metadata_event_sequence ||
      !DirectAppendIndexCacheAuthorityMatches(found->second, context)) {
    return;
  }
  const auto& record = found->second;
  for (const auto& index : indexes) {
    if (!DirectIndexIsUnique(index)) {
      continue;
    }
    const auto cached_keys = record.keys_by_index.find(index.index_uuid);
    if (cached_keys == record.keys_by_index.end()) {
      continue;
    }
    const auto cached_entries = record.entry_by_index_key.find(index.index_uuid);
    const std::size_t keys_before = keys_by_index == nullptr
                                        ? 0
                                        : (*keys_by_index)[index.index_uuid].size();
    for (const auto& values : logical_value_batch) {
      for (const auto& key : CrudIndexKeysForValues(index, values)) {
        if (cached_keys->second.count(key) == 0) {
          continue;
        }
        if (keys_by_index != nullptr) {
          (*keys_by_index)[index.index_uuid].insert(key);
        }
        if (entry_by_index_key != nullptr &&
            cached_entries != record.entry_by_index_key.end()) {
          const auto entry = cached_entries->second.find(key);
          if (entry != cached_entries->second.end()) {
            (*entry_by_index_key)[index.index_uuid][key] = entry->second;
          }
        }
      }
    }
    if (keys_by_index != nullptr &&
        (*keys_by_index)[index.index_uuid].size() == keys_before &&
        !cached_keys->second.empty()) {
      for (const auto& key : cached_keys->second) {
        (*keys_by_index)[index.index_uuid].insert(key);
        if (entry_by_index_key != nullptr &&
            cached_entries != record.entry_by_index_key.end()) {
          const auto entry = cached_entries->second.find(key);
          if (entry != cached_entries->second.end()) {
            (*entry_by_index_key)[index.index_uuid][key] = entry->second;
          }
        }
      }
    }
  }
}

bool DirectLookupBulkAppendContextCache(
    const EngineRequestContext& context,
    const std::string& table_uuid,
    std::uint64_t row_version_count,
    DirectBulkAppendContextCacheRecord* record) {
  if (record == nullptr) return false;
  const std::uint64_t metadata_event_sequence =
      CurrentMgaRelationMetadataEventSequence(context);
  const std::lock_guard<std::mutex> guard(DirectAppendIndexEntryCacheMutex());
  const auto found = DirectBulkAppendContextCache().find(
      DirectBulkAppendContextCacheKey(context, table_uuid));
  if (found == DirectBulkAppendContextCache().end() ||
      found->second.row_version_count != row_version_count ||
      found->second.metadata_event_sequence != metadata_event_sequence ||
      !found->second.state) {
    return false;
  }
  *record = found->second;
  return true;
}

void DirectStoreBulkAppendContextCache(
    const EngineRequestContext& context,
    const std::string& table_uuid,
    std::uint64_t row_version_count,
    const MgaRelationReadView& state,
    const std::vector<CrudIndexRecord>& visible_indexes,
    const MgaRelationStorageDescriptor& relation_descriptor,
    bool index_entries_authoritative,
    bool append_index_cache_hit) {
  DirectBulkAppendContextCacheRecord record;
  record.row_version_count = row_version_count;
  record.metadata_event_sequence =
      CurrentMgaRelationMetadataEventSequence(context);
  record.state = std::make_shared<MgaRelationReadView>(state);
  record.visible_indexes = visible_indexes;
  record.relation_descriptor = relation_descriptor;
  record.index_entries_authoritative = index_entries_authoritative;
  record.append_index_cache_hit = append_index_cache_hit;
  const std::lock_guard<std::mutex> guard(DirectAppendIndexEntryCacheMutex());
  DirectBulkAppendContextCache()[DirectBulkAppendContextCacheKey(context,
                                                                 table_uuid)] =
      std::move(record);
}

bool DirectAdvanceBulkAppendContextCache(
    const EngineRequestContext& context,
    const std::string& table_uuid,
    std::uint64_t previous_row_version_count,
    std::uint64_t next_row_version_count,
    bool index_entries_authoritative,
    bool append_index_cache_hit) {
  const std::uint64_t metadata_event_sequence =
      CurrentMgaRelationMetadataEventSequence(context);
  const std::lock_guard<std::mutex> guard(DirectAppendIndexEntryCacheMutex());
  const auto found = DirectBulkAppendContextCache().find(
      DirectBulkAppendContextCacheKey(context, table_uuid));
  if (found == DirectBulkAppendContextCache().end() ||
      found->second.row_version_count != previous_row_version_count ||
      found->second.metadata_event_sequence != metadata_event_sequence ||
      !found->second.state) {
    return false;
  }
  found->second.row_version_count = next_row_version_count;
  found->second.index_entries_authoritative = index_entries_authoritative;
  found->second.append_index_cache_hit = append_index_cache_hit;
  return true;
}

void DirectStoreAppendIndexEntryCache(
    const EngineRequestContext& context,
    const std::string& table_uuid,
    std::uint64_t row_version_count,
    const MgaRelationReadView& state,
    const std::vector<CrudIndexEntryRecord>& entries) {
  std::vector<CrudIndexEntryRecord> visible_entries;
  visible_entries.reserve(entries.size());
  for (const auto& entry : entries) {
    if (MgaCreatorVisible(state,
                           entry.creator_tx,
                           entry.event_sequence,
                           context.local_transaction_id)) {
      visible_entries.push_back(entry);
    }
  }
  const std::lock_guard<std::mutex> guard(DirectAppendIndexEntryCacheMutex());
  auto& record =
      DirectAppendIndexEntryCache()[DirectAppendIndexEntryCacheKey(context,
                                                                  table_uuid)];
  record.row_version_count = row_version_count;
  record.metadata_event_sequence =
      CurrentMgaRelationMetadataEventSequence(context);
  record.observer_local_transaction_id = context.local_transaction_id;
  record.savepoint_authority_generation =
      CurrentMgaSavepointAuthorityGeneration(context);
  record.entries = std::move(visible_entries);
  record.keys_by_index = DirectBuildIndexKeyCache(record.entries);
  record.entry_by_index_key = DirectBuildIndexEntryKeyCache(record.entries);
  record.entry_lookup_materialized = true;
}

std::vector<CrudIndexEntryRecord> DirectIndexEntriesFromExactBatches(
    const EngineRequestContext& context,
    const std::vector<MgaExactIndexEntryAppendBatch>& batches) {
  std::vector<CrudIndexEntryRecord> entries;
  for (const auto& batch : batches) {
    const std::string table_uuid =
        batch.index.table_uuid.empty() ? batch.table_uuid
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
    const std::string table_uuid =
        batch.index.table_uuid.empty() ? batch.table_uuid
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

void DirectClearAppendIndexEntryCacheRecord(
    DirectAppendIndexEntryCacheRecord* record) {
  if (record == nullptr) { return; }
  record->entries.clear();
  record->keys_by_index.clear();
  record->entry_by_index_key.clear();
  record->entry_lookup_materialized = true;
}

void DirectAppendIndexEntryToCacheRecord(
    DirectAppendIndexEntryCacheRecord* record,
    CrudIndexEntryRecord entry,
    bool materialize_entry_lookup = true) {
  if (record == nullptr) { return; }
  record->entries.push_back(entry);
  record->keys_by_index[entry.index_uuid].insert(entry.key_value);
  if (materialize_entry_lookup) {
    record->entry_by_index_key[entry.index_uuid][entry.key_value] =
        std::move(entry);
  }
}

void DirectAppendIndexEntriesToCache(
    const EngineRequestContext& context,
    const std::string& table_uuid,
    std::uint64_t previous_row_version_count,
    std::uint64_t appended_row_count,
    const std::vector<CrudIndexEntryRecord>& appended_entries) {
  const std::lock_guard<std::mutex> guard(DirectAppendIndexEntryCacheMutex());
  auto& record =
      DirectAppendIndexEntryCache()[DirectAppendIndexEntryCacheKey(context,
                                                                  table_uuid)];
  const auto metadata_event_sequence =
      CurrentMgaRelationMetadataEventSequence(context);
  const auto savepoint_authority_generation =
      CurrentMgaSavepointAuthorityGeneration(context);
  if (record.row_version_count != previous_row_version_count ||
      record.metadata_event_sequence != metadata_event_sequence ||
      record.observer_local_transaction_id != context.local_transaction_id ||
      record.savepoint_authority_generation !=
          savepoint_authority_generation) {
    DirectClearAppendIndexEntryCacheRecord(&record);
  }
  for (const auto& entry : appended_entries) {
    DirectAppendIndexEntryToCacheRecord(&record, entry);
  }
  record.row_version_count = previous_row_version_count + appended_row_count;
  record.metadata_event_sequence = metadata_event_sequence;
  record.observer_local_transaction_id = context.local_transaction_id;
  record.savepoint_authority_generation =
      savepoint_authority_generation;
}

void DirectAppendIndexBatchesToCache(
    const EngineRequestContext& context,
    const std::string& table_uuid,
    std::uint64_t previous_row_version_count,
    std::uint64_t appended_row_count,
    const std::vector<MgaExactIndexEntryAppendBatch>& exact_batches,
    const std::vector<MgaIndexEntryAppendBatch>& retail_batches,
    bool materialize_entry_lookup) {
  const std::lock_guard<std::mutex> guard(DirectAppendIndexEntryCacheMutex());
  auto& record =
      DirectAppendIndexEntryCache()[DirectAppendIndexEntryCacheKey(context,
                                                                  table_uuid)];
  const auto metadata_event_sequence =
      CurrentMgaRelationMetadataEventSequence(context);
  const auto savepoint_authority_generation =
      CurrentMgaSavepointAuthorityGeneration(context);
  if (record.row_version_count != previous_row_version_count ||
      record.metadata_event_sequence != metadata_event_sequence ||
      record.observer_local_transaction_id != context.local_transaction_id ||
      record.savepoint_authority_generation !=
          savepoint_authority_generation) {
    DirectClearAppendIndexEntryCacheRecord(&record);
  }
  record.entry_lookup_materialized = materialize_entry_lookup;
  if (!materialize_entry_lookup) {
    record.entry_by_index_key.clear();
  }
  for (const auto& batch : exact_batches) {
    const std::string batch_table_uuid =
        batch.index.table_uuid.empty() ? batch.table_uuid
                                       : batch.index.table_uuid;
    for (const auto& exact : batch.entries) {
      CrudIndexEntryRecord entry;
      entry.creator_tx = context.local_transaction_id;
      entry.index_uuid = batch.index.index_uuid;
      entry.table_uuid = batch_table_uuid;
      entry.column_name = batch.index.column_name;
      entry.family = batch.index.family;
      entry.entry_kind = "exact";
      entry.key_value = exact.encoded_key;
      entry.payload_value = exact.payload_value;
      entry.row_uuid = exact.row_uuid;
      entry.version_uuid = exact.version_uuid;
      DirectAppendIndexEntryToCacheRecord(&record,
                                          std::move(entry),
                                          materialize_entry_lookup);
    }
  }
  for (const auto& batch : retail_batches) {
    const std::string batch_table_uuid =
        batch.index.table_uuid.empty() ? batch.table_uuid
                                       : batch.index.table_uuid;
    for (const auto& row : batch.rows) {
      for (const auto& key : CrudIndexKeysForValues(batch.index, row.values)) {
        CrudIndexEntryRecord entry;
        entry.creator_tx = context.local_transaction_id;
        entry.index_uuid = batch.index.index_uuid;
        entry.table_uuid = batch_table_uuid;
        entry.column_name = batch.index.column_name;
        entry.family = batch.index.family;
        entry.entry_kind = "exact";
        entry.key_value = key;
        entry.payload_value = CrudFieldValue(row.values, batch.index.column_name);
        entry.row_uuid = row.row_uuid;
        entry.version_uuid = row.version_uuid;
        DirectAppendIndexEntryToCacheRecord(&record,
                                            std::move(entry),
                                            materialize_entry_lookup);
      }
    }
  }
  record.row_version_count = previous_row_version_count + appended_row_count;
  record.metadata_event_sequence = metadata_event_sequence;
  record.observer_local_transaction_id = context.local_transaction_id;
  record.savepoint_authority_generation =
      savepoint_authority_generation;
}

}  // namespace scratchbird::engine::internal_api::dml::detail

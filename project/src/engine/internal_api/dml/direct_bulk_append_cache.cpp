// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "dml/direct_bulk_append_cache.hpp"
#include "direct_bulk_index_cache_projection.hpp"

#include <algorithm>
#include <limits>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <tuple>
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
  struct LogicalKeyProjection {
    std::size_t entries_processed = 0;
    std::map<std::string, std::set<std::string>> physical_keys;
  };
  std::uint64_t row_version_count = 0;
  std::uint64_t metadata_event_sequence = 0;
  std::uint64_t observer_local_transaction_id = 0;
  std::uint64_t savepoint_authority_generation = 0;
  std::vector<CrudIndexEntryRecord> entries;
  std::map<EngineUuid, std::set<std::string>> keys_by_index;
  std::map<EngineUuid, LogicalKeyProjection> logical_keys_by_index;
  std::map<EngineUuid, std::map<std::string, CrudIndexEntryRecord>>
      entry_by_index_key;
  bool entry_lookup_materialized = true;
};

std::mutex& DirectAppendIndexEntryCacheMutex() {
  static std::mutex mutex;
  return mutex;
}

// A structured key preserves raw identity bytes and separates the route path
// from identity/epoch fields. Neither UUID ordering nor cache membership grants
// visibility: the MGA generation and row-count checks below still apply.
using DirectBulkCacheKey = std::tuple<
    std::string, EngineUuid, EngineUuid, EngineUuid, EngineUuid, EngineUuid,
    EngineUuid, std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t>;

DirectBulkCacheKey DirectBulkAppendContextCacheKey(
    const EngineRequestContext& context, const EngineUuid& table_uuid) {
  return {context.database_path, context.database_uuid, table_uuid,
          context.transaction_uuid, context.session_uuid, context.principal_uuid,
          context.current_role_uuid, context.local_transaction_id,
          context.catalog_generation_id, context.security_epoch,
          CurrentMgaSavepointAuthorityGeneration(context)};
}

std::map<DirectBulkCacheKey, DirectAppendIndexEntryCacheRecord>&
DirectAppendIndexEntryCache() {
  static std::map<DirectBulkCacheKey, DirectAppendIndexEntryCacheRecord> cache;
  return cache;
}

std::map<DirectBulkCacheKey, DirectBulkAppendContextCacheRecord>&
DirectBulkAppendContextCache() {
  static std::map<DirectBulkCacheKey, DirectBulkAppendContextCacheRecord> cache;
  return cache;
}

DirectBulkCacheKey DirectAppendIndexEntryCacheKey(
    const EngineRequestContext& context, const EngineUuid& table_uuid) {
  return DirectBulkAppendContextCacheKey(context, table_uuid);
}

void DirectEvictAppendIndexEntryCache(const EngineRequestContext& context,
                                     const EngineUuid& table_uuid) {
  const std::lock_guard<std::mutex> guard(DirectAppendIndexEntryCacheMutex());
  auto& cache = DirectAppendIndexEntryCache();
  for (auto it = cache.begin(); it != cache.end();) {
    if (std::get<0>(it->first) == context.database_path &&
        std::get<1>(it->first) == context.database_uuid &&
        std::get<2>(it->first) == table_uuid) {
      it = cache.erase(it);
    } else {
      ++it;
    }
  }
}

bool DirectAppendIndexCacheAuthorityMatches(
    const DirectAppendIndexEntryCacheRecord& record,
    const EngineRequestContext& context) {
  return record.observer_local_transaction_id ==
             context.local_transaction_id &&
         record.savepoint_authority_generation ==
             CurrentMgaSavepointAuthorityGeneration(context);
}

bool DirectLookupAppendIndexEntryCache(const EngineRequestContext& context,
                                       const EngineUuid& table_uuid,
                                       std::uint64_t row_version_count,
                                       std::vector<CrudIndexEntryRecord>* entries,
                                       std::map<EngineUuid, std::set<std::string>>* keys_by_index,
                                       std::map<EngineUuid, std::map<std::string, CrudIndexEntryRecord>>* entry_by_index_key) {
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
  if (entry_by_index_key != nullptr && !found->second.entry_lookup_materialized)
    return false;
  // All copies precede publication; allocation failure cannot leave a caller
  // with a new key set paired with an old entry set.
  auto staged_entries = entries != nullptr ? found->second.entries
                                           : std::vector<CrudIndexEntryRecord>{};
  auto staged_keys = keys_by_index != nullptr ? found->second.keys_by_index
      : std::map<EngineUuid, std::set<std::string>>{};
  auto staged_lookup = entry_by_index_key != nullptr
      ? found->second.entry_by_index_key
      : std::map<EngineUuid, std::map<std::string, CrudIndexEntryRecord>>{};
  if (entries != nullptr) entries->swap(staged_entries);
  if (keys_by_index != nullptr) {
    keys_by_index->swap(staged_keys);
  }
  if (entry_by_index_key != nullptr) {
    entry_by_index_key->swap(staged_lookup);
  }
  return true;
}

bool DirectAppendIndexEntryCacheAvailable(const EngineRequestContext& context,
                                          const EngineUuid& table_uuid,
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

bool DirectBuildAppendIndexConflictCaches(
    const EngineRequestContext& context,
    const EngineUuid& table_uuid,
    std::uint64_t row_version_count,
    const std::vector<CrudIndexRecord>& indexes,
    const std::vector<std::vector<std::pair<std::string, std::string>>>& logical_value_batch,
    std::map<EngineUuid, std::set<std::string>>* keys_by_index,
    std::map<EngineUuid, std::map<std::string, CrudIndexEntryRecord>>*
        entry_by_index_key) {
  if (keys_by_index == nullptr && entry_by_index_key == nullptr) {
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
  auto& record = found->second;
  if (entry_by_index_key != nullptr && !record.entry_lookup_materialized) {
    return false;
  }
  // This API accumulates conflicts. Retain that contract while publishing both
  // outputs together only after every provider projection/copy succeeds.
  auto staged_keys = keys_by_index != nullptr ? *keys_by_index
      : std::map<EngineUuid, std::set<std::string>>{};
  auto staged_lookup = entry_by_index_key != nullptr ? *entry_by_index_key
      : std::map<EngineUuid, std::map<std::string, CrudIndexEntryRecord>>{};
  for (const auto& index : indexes) {
    if (!DirectIndexIsUnique(index)) {
      continue;
    }
    // Proofs compare provider logical keys. Physical scalar SBKOHEX entries
    // and ordinary logical entries can coexist in the same canonical index.
    // Incrementally project each validated cache record; a partial raw-key
    // match is never evidence that all incoming logical keys were checked.
    auto& projection = record.logical_keys_by_index[index.index_uuid];
    for (; projection.entries_processed < record.entries.size();
         ++projection.entries_processed) {
      const auto& entry = record.entries[projection.entries_processed];
      if (entry.index_uuid == index.index_uuid) {
        projection.physical_keys[CrudIndexEntryLogicalKey(index, entry)]
            .insert(entry.key_value);
      }
    }
    const auto cached_entries = record.entry_by_index_key.find(index.index_uuid);
    const auto append = [&](const auto& logical_key, const auto& physical_keys) {
      if (keys_by_index != nullptr) {
        staged_keys[index.index_uuid].insert(logical_key);
      }
      if (entry_by_index_key != nullptr &&
          cached_entries != record.entry_by_index_key.end()) {
        for (const auto& key : physical_keys) {
          const auto entry = cached_entries->second.find(key);
          if (entry != cached_entries->second.end()) {
            staged_lookup[index.index_uuid][key] = entry->second;
          }
        }
      }
    };
    if (logical_value_batch.empty()) {
      // Typed input may deliberately omit the logical row batch. The proof
      // filters these complete projected keys against its precomputed input.
      for (const auto& [key, physical_keys] : projection.physical_keys) {
        append(key, physical_keys);
      }
    } else {
      for (const auto& values : logical_value_batch) {
        for (const auto& key : CrudIndexKeysForValues(index, values)) {
          const auto match = projection.physical_keys.find(key);
          if (match != projection.physical_keys.end()) {
            append(match->first, match->second);
          }
        }
      }
    }
  }
  if (keys_by_index != nullptr) keys_by_index->swap(staged_keys);
  if (entry_by_index_key != nullptr) entry_by_index_key->swap(staged_lookup);
  return true;
}

bool DirectLookupBulkAppendContextCache(
    const EngineRequestContext& context,
    const EngineUuid& table_uuid,
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
  auto staged = found->second;
  *record = std::move(staged);
  return true;
}

void DirectStoreBulkAppendContextCache(
    const EngineRequestContext& context,
    const EngineUuid& table_uuid,
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
    const EngineUuid& table_uuid,
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
    const EngineUuid& table_uuid,
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
  DirectAppendIndexEntryCacheRecord record;
  record.row_version_count = row_version_count;
  record.metadata_event_sequence =
      CurrentMgaRelationMetadataEventSequence(context);
  record.observer_local_transaction_id = context.local_transaction_id;
  record.savepoint_authority_generation =
      CurrentMgaSavepointAuthorityGeneration(context);
  record.entries = std::move(visible_entries);
  record.logical_keys_by_index.clear();
  record.keys_by_index = DirectBuildIndexKeyCache(record.entries);
  record.entry_by_index_key = DirectBuildIndexEntryKeyCache(record.entries);
  record.entry_lookup_materialized = true;
  const auto key = DirectAppendIndexEntryCacheKey(context, table_uuid);
  const std::lock_guard<std::mutex> guard(DirectAppendIndexEntryCacheMutex());
  DirectAppendIndexEntryCache().insert_or_assign(key, std::move(record));
}

void DirectClearAppendIndexEntryCacheRecord(
    DirectAppendIndexEntryCacheRecord* record) {
  if (record == nullptr) { return; }
  record->entries.clear();
  record->logical_keys_by_index.clear();
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

// An append may allocate after modifying an entry vector or index map. If it
// does not finish, discard that advisory record rather than expose a partial
// proof cache at either the old or new row count. The cache mutex is held for
// the guard's whole lifetime; erasing this iterator does not allocate.
class DirectAppendCacheMutationGuard {
 public:
  explicit DirectAppendCacheMutationGuard(
      std::map<DirectBulkCacheKey, DirectAppendIndexEntryCacheRecord>::iterator it)
      : it_(it) {}
  ~DirectAppendCacheMutationGuard() {
    if (!finished_) DirectAppendIndexEntryCache().erase(it_);
  }
  DirectAppendCacheMutationGuard(const DirectAppendCacheMutationGuard&) = delete;
  DirectAppendCacheMutationGuard& operator=(const DirectAppendCacheMutationGuard&) = delete;
  void Finish() noexcept { finished_ = true; }

 private:
  std::map<DirectBulkCacheKey, DirectAppendIndexEntryCacheRecord>::iterator it_;
  bool finished_ = false;
};

void DirectAppendIndexEntriesToCache(
    const EngineRequestContext& context,
    const EngineUuid& table_uuid,
    std::uint64_t previous_row_version_count,
    std::uint64_t appended_row_count,
    const std::vector<CrudIndexEntryRecord>& appended_entries) {
  const std::lock_guard<std::mutex> guard(DirectAppendIndexEntryCacheMutex());
  const auto [it, inserted] = DirectAppendIndexEntryCache().try_emplace(
      DirectAppendIndexEntryCacheKey(context, table_uuid));
  DirectAppendCacheMutationGuard mutation(it);
  auto& record = it->second;
  if (appended_row_count >
      std::numeric_limits<std::uint64_t>::max() - previous_row_version_count) {
    return;
  }
  const auto metadata_event_sequence =
      CurrentMgaRelationMetadataEventSequence(context);
  const auto savepoint_authority_generation =
      CurrentMgaSavepointAuthorityGeneration(context);
  if (record.row_version_count != previous_row_version_count ||
      record.metadata_event_sequence != metadata_event_sequence ||
      record.observer_local_transaction_id != context.local_transaction_id ||
      record.savepoint_authority_generation !=
          savepoint_authority_generation) {
    if (previous_row_version_count != 0) {
      // A delta is not a complete cache rebuild. Missing prior entries must
      // force a scoped reload on the next request, not certify a partial set.
      return;
    }
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
  mutation.Finish();
}

void DirectAppendIndexBatchesToCache(
    const EngineRequestContext& context,
    const EngineUuid& table_uuid,
    std::uint64_t previous_row_version_count,
    std::uint64_t appended_row_count,
    const std::vector<MgaExactIndexEntryAppendBatch>& exact_batches,
    const std::vector<MgaIndexEntryAppendBatch>& retail_batches,
    bool materialize_entry_lookup) {
  const std::lock_guard<std::mutex> guard(DirectAppendIndexEntryCacheMutex());
  const auto [it, inserted] = DirectAppendIndexEntryCache().try_emplace(
      DirectAppendIndexEntryCacheKey(context, table_uuid));
  DirectAppendCacheMutationGuard mutation(it);
  auto& record = it->second;
  if (appended_row_count >
      std::numeric_limits<std::uint64_t>::max() - previous_row_version_count) {
    return;
  }
  const auto metadata_event_sequence =
      CurrentMgaRelationMetadataEventSequence(context);
  const auto savepoint_authority_generation =
      CurrentMgaSavepointAuthorityGeneration(context);
  if (record.row_version_count != previous_row_version_count ||
      record.metadata_event_sequence != metadata_event_sequence ||
      record.observer_local_transaction_id != context.local_transaction_id ||
      record.savepoint_authority_generation !=
          savepoint_authority_generation) {
    if (previous_row_version_count != 0) {
      return;
    }
    DirectClearAppendIndexEntryCacheRecord(&record);
  }
  if (materialize_entry_lookup && !record.entry_lookup_materialized) {
    // A previous key-only append deliberately omitted this projection. Build
    // it from every retained entry before claiming it is complete again.
    record.entry_by_index_key = DirectBuildIndexEntryKeyCache(record.entries);
  }
  record.entry_lookup_materialized = materialize_entry_lookup;
  if (!materialize_entry_lookup) {
    record.entry_by_index_key.clear();
  }
  for (const auto& batch : exact_batches) {
    const EngineUuid batch_table_uuid =
        batch.index.table_uuid.is_nil() ? batch.table_uuid
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
    const EngineUuid batch_table_uuid =
        batch.index.table_uuid.is_nil() ? batch.table_uuid
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
  mutation.Finish();
}

}  // namespace scratchbird::engine::internal_api::dml::detail

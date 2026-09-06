// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "crud_support/crud_store.hpp"
#include "dml/mga_relation_read_view.hpp"
#include "dml/transactional_index_provider.hpp"
#include "insert_physical_integration.hpp"
#include "mga_relation_store/mga_relation_store.hpp"

#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace scratchbird::engine::internal_api::dml::detail {

// Owns transaction-scoped direct-bulk context and index-entry caches. Cached
// data is advisory and never becomes visibility or finality authority.

struct DirectBulkAppendContextCacheRecord {
  std::uint64_t row_version_count = 0;
  std::uint64_t metadata_event_sequence = 0;
  std::shared_ptr<const MgaRelationReadView> state;
  std::vector<CrudIndexRecord> visible_indexes;
  MgaRelationStorageDescriptor relation_descriptor;
  bool index_entries_authoritative = false;
  bool append_index_cache_hit = false;
};

bool DirectAppendIndexEntryCacheAvailable(
    const EngineRequestContext& context,
    const std::string& table_uuid,
    std::uint64_t row_version_count,
    bool require_entry_lookup = false);

void DirectBuildAppendIndexConflictCaches(
    const EngineRequestContext& context,
    const std::string& table_uuid,
    std::uint64_t row_version_count,
    const std::vector<CrudIndexRecord>& indexes,
    const std::vector<std::vector<std::pair<std::string, std::string>>>&
        logical_value_batch,
    std::map<std::string, std::set<std::string>>* keys_by_index,
    std::map<std::string, std::map<std::string, CrudIndexEntryRecord>>*
        entry_by_index_key);

bool DirectLookupBulkAppendContextCache(
    const EngineRequestContext& context,
    const std::string& table_uuid,
    std::uint64_t row_version_count,
    DirectBulkAppendContextCacheRecord* record);

void DirectStoreBulkAppendContextCache(
    const EngineRequestContext& context,
    const std::string& table_uuid,
    std::uint64_t row_version_count,
    const MgaRelationReadView& state,
    const std::vector<CrudIndexRecord>& visible_indexes,
    const MgaRelationStorageDescriptor& relation_descriptor,
    bool index_entries_authoritative,
    bool append_index_cache_hit);

bool DirectAdvanceBulkAppendContextCache(
    const EngineRequestContext& context,
    const std::string& table_uuid,
    std::uint64_t previous_row_version_count,
    std::uint64_t next_row_version_count,
    bool index_entries_authoritative,
    bool append_index_cache_hit);

void DirectStoreAppendIndexEntryCache(
    const EngineRequestContext& context,
    const std::string& table_uuid,
    std::uint64_t row_version_count,
    const MgaRelationReadView& state,
    const std::vector<CrudIndexEntryRecord>& entries);

void DirectAppendIndexBatchesToCache(
    const EngineRequestContext& context,
    const std::string& table_uuid,
    std::uint64_t previous_row_version_count,
    std::uint64_t appended_row_count,
    const std::vector<MgaExactIndexEntryAppendBatch>& exact_batches,
    const std::vector<MgaIndexEntryAppendBatch>& retail_batches,
    bool materialize_entry_lookup = true);

}  // namespace scratchbird::engine::internal_api::dml::detail

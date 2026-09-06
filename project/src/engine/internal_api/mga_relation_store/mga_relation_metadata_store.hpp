// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "mga_relation_store/mga_relation_store.hpp"

#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace scratchbird::engine::internal_api {

// SEARCH_KEY: SB_ENGINE_MGA_RELATION_METADATA_STORE_INTERFACE
// Persisted relation metadata is decoded and cached here. Visibility remains
// a projection supplied by the canonical MGA transaction/savepoint authority.
using DescriptorFieldsByRelation =
    std::map<std::string,
             std::vector<std::pair<std::string, std::string>>>;

struct MgaMetadataCacheKey {
  std::string database_uuid;
  std::string metadata_path;
  std::uintmax_t metadata_file_size = 0;
  std::int64_t metadata_file_mtime_ticks = 0;
  std::string savepoint_path;
  std::uintmax_t savepoint_file_size = 0;
  std::int64_t savepoint_file_mtime_ticks = 0;
  std::uint64_t local_transaction_id = 0;

  bool operator<(const MgaMetadataCacheKey& other) const {
    return std::tie(database_uuid, metadata_path, metadata_file_size,
                    metadata_file_mtime_ticks, savepoint_path,
                    savepoint_file_size, savepoint_file_mtime_ticks,
                    local_transaction_id) <
           std::tie(other.database_uuid, other.metadata_path,
                    other.metadata_file_size,
                    other.metadata_file_mtime_ticks, other.savepoint_path,
                    other.savepoint_file_size,
                    other.savepoint_file_mtime_ticks,
                    other.local_transaction_id);
  }
};

struct MgaMetadataCacheEntry {
  std::vector<CrudTableRecord> tables;
  std::vector<CrudIndexRecord> indexes;
  std::vector<CrudSealedRelationDescriptorSnapshot>
      sealed_relation_descriptor_snapshots;
  std::set<std::string> known_temporary_relation_uuids;
  std::uint64_t max_event_sequence = 0;
};

struct MgaMetadataSnapshotLoadResult {
  EngineApiDiagnostic diagnostic;
  std::shared_ptr<const MgaMetadataCacheEntry> snapshot;
  MgaMetadataCacheKey key;

  bool ok() const { return snapshot != nullptr && !diagnostic.error; }
};

std::shared_ptr<const DescriptorFieldsByRelation>
LoadDescriptorFieldsSnapshot(
    const EngineRequestContext& context,
    std::string_view required_relation_uuid = {});
DescriptorFieldsByRelation LoadDescriptorFieldsByRelation(
    const EngineRequestContext& context,
    std::string_view required_relation_uuid = {});
EngineApiDiagnostic PersistDescriptorFields(
    const EngineRequestContext& context,
    const std::string& relation_uuid,
    const std::vector<std::pair<std::string, std::string>>& fields);
EngineApiDiagnostic LoadMgaMetadata(
    RelationReadSnapshot* state,
    const EngineRequestContext& context);
MgaMetadataSnapshotLoadResult LoadMgaMetadataSnapshot(
    const EngineRequestContext& context);
std::string CanonicalConstraintMutationBatchPayload(
    const MgaConstraintMutationBatch& batch,
    std::uint64_t creator_local_transaction_id,
    std::uint64_t metadata_event_sequence);
std::string ConstraintMutationBatchSha256(
    const MgaConstraintMutationBatch& batch,
    std::uint64_t creator_local_transaction_id,
    std::uint64_t metadata_event_sequence);
std::string CanonicalBigintMigrationPayload(
    const MgaBigintIdentityMigrationRequest& request,
    std::uint64_t creator_tx,
    std::uint64_t event_sequence,
    std::string_view transaction_uuid,
    const std::vector<CrudTableRecord>& tables,
    const std::vector<std::string>& decision_hashes);
std::string BigintMigrationDecisionHash(
    const MgaBigintIdentityMigrationRequest& request,
    const MgaBigintIdentityMigrationRow& row,
    std::uint64_t new_row_generation,
    std::string_view transaction_uuid);
std::string CanonicalInt32MigrationPayload(
    const MgaInt32IdentityMigrationRequest& request,
    std::uint64_t creator_tx,
    std::uint64_t event_sequence,
    std::string_view transaction_uuid,
    const std::vector<CrudTableRecord>& tables,
    const std::vector<std::string>& decision_hashes);
std::string Int32MigrationDecisionHash(
    const MgaInt32IdentityMigrationRequest& request,
    const MgaInt32IdentityMigrationRow& row,
    std::uint64_t new_row_generation,
    std::string_view transaction_uuid);
std::string CanonicalTextMigrationPayload(
    const MgaTextIdentityMigrationRequest& request,
    std::uint64_t creator_tx,
    std::uint64_t event_sequence,
    std::string_view transaction_uuid,
    std::string_view datatype_catalog_snapshot_uuid,
    std::uint64_t datatype_catalog_generation,
    std::uint64_t datatype_registry_generation,
    const std::vector<CrudTableRecord>& tables,
    const std::vector<CrudSealedRelationDescriptorSnapshot>&
        relation_descriptor_snapshots,
    const std::vector<std::string>& decision_hashes);
std::string TextMigrationDecisionHash(
    const MgaTextIdentityMigrationRequest& request,
    const MgaTextIdentityMigrationRow& row,
    std::uint64_t new_row_generation,
    std::string_view transaction_uuid,
    std::string_view datatype_catalog_snapshot_uuid,
    std::uint64_t datatype_catalog_generation,
    std::uint64_t datatype_registry_generation,
    const CrudSealedRelationDescriptorSnapshot& relation_snapshot);
std::string Sha256Tagged(std::string_view payload);
bool ValidConstraintBatchUuid(
    const std::string& value,
    scratchbird::core::platform::UuidKind kind);
std::vector<std::string> ConstraintMutationBatchLineFields(
    const MgaConstraintMutationBatch& batch,
    std::uint64_t creator_tx,
    std::uint64_t event_sequence);
std::size_t ConstraintMutationBatchFieldCount();

}  // namespace scratchbird::engine::internal_api

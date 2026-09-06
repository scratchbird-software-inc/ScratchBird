// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "api_types.hpp"
#include "api_diagnostics.hpp"

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace scratchbird::engine::internal_api {

// SEARCH_KEY: SB_ENGINE_API_CRUD_STORE_VERTICAL_SLICE
// SEARCH_KEY: SB_ENGINE_API_INDEX_VERTICAL_SLICE_STORE

inline constexpr std::size_t kCrudVerticalSliceMaxEncodedValueBytes = 4096;
inline constexpr const char* kCrudIndexProfileRowStoreScalarBtreeV1 = "rowstore_scalar_btree_v1";
inline constexpr const char* kCrudIndexFamilyBtree = "btree";
inline constexpr const char* kCrudIndexFamilyHash = "hash";
inline constexpr const char* kCrudIndexFamilyBitmap = "bitmap";
inline constexpr const char* kCrudIndexFamilyBrinZone = "brin_zone";
inline constexpr const char* kCrudIndexFamilyBloom = "bloom";
inline constexpr const char* kCrudIndexFamilyFullText = "full_text";
inline constexpr const char* kCrudIndexFamilyGin = "gin";
inline constexpr const char* kCrudIndexFamilyInverted = "inverted";
inline constexpr const char* kCrudIndexFamilyNgram = "ngram";
inline constexpr const char* kCrudIndexFamilySparseWand = "sparse_wand";
inline constexpr const char* kCrudIndexFamilySpatial = "spatial";
inline constexpr const char* kCrudIndexFamilyRtree = "rtree";
inline constexpr const char* kCrudIndexFamilyGist = "gist";
inline constexpr const char* kCrudIndexFamilySpgist = "spgist";
inline constexpr const char* kCrudIndexFamilyVectorExact = "vector_exact";
inline constexpr const char* kCrudIndexFamilyVectorHnsw = "vector_hnsw";
inline constexpr const char* kCrudIndexFamilyVectorIvf = "vector_ivf";
inline constexpr const char* kCrudIndexFamilyColumnarZone = "columnar_zone";
inline constexpr const char* kCrudIndexFamilyDocumentPath = "document_path";
inline constexpr const char* kCrudIndexFamilyGraphAdjacency = "graph_adjacency";
inline constexpr const char* kCrudIndexFamilyExpression = "expression";
inline constexpr const char* kCrudIndexFamilyPartial = "partial";
inline constexpr const char* kCrudIndexFamilyCovering = "covering";
inline constexpr const char* kCrudIndexFamilyTemporaryWork = "temporary_work";
inline constexpr const char* kCrudIndexFamilyInMemory = "in_memory";
inline constexpr const char* kCrudIndexFamilyReferenceEmulated = "reference_emulated";
inline constexpr const char* kCrudIndexFamilyPolicyBlocked = "policy_blocked";

struct CrudTableRecord {
  std::uint64_t creator_tx = 0;
  std::uint64_t event_sequence = 0;
  std::string table_uuid;
  // Migration/display cache only. SQL object name authority is SBNAME1 name registry.
  std::string default_name;
  std::vector<std::pair<std::string, std::string>> columns;
  bool temporary = false;
  std::string temporary_scope;
  std::string temporary_session_uuid;
  std::string on_commit_action;
};

struct CrudRowVersionRecord {
  std::uint64_t creator_tx = 0;
  std::uint64_t event_sequence = 0;
  std::uint64_t sequence = 0;
  std::string table_uuid;
  std::string row_uuid;
  std::string version_uuid;
  std::string temporary_session_uuid;
  std::string previous_version_uuid;
  std::uint64_t previous_sequence = 0;
  bool deleted = false;
  std::vector<std::pair<std::string, std::string>> values;
};

struct CrudIndexRecord {
  std::uint64_t creator_tx = 0;
  std::uint64_t event_sequence = 0;
  std::string index_uuid;
  std::string table_uuid;
  std::string column_name;
  std::string family;
  std::string profile;
  // Migration/display cache only. SQL object name authority is SBNAME1 name registry.
  std::string default_name;
  std::vector<std::string> key_envelopes;
  std::vector<std::string> include_columns;
  std::string predicate_kind;
  std::string predicate_column;
  std::string predicate_value;
  bool unique = false;
  bool approximate = false;
  bool exact_fallback = false;
};

struct CrudIndexEntryRecord {
  std::uint64_t creator_tx = 0;
  std::uint64_t event_sequence = 0;
  std::uint64_t sequence = 0;
  std::string index_uuid;
  std::string table_uuid;
  std::string column_name;
  std::string family;
  std::string entry_kind;
  std::string key_value;
  std::string payload_value;
  std::string row_uuid;
  std::string version_uuid;
};

struct CrudLargeValueChunkRecord {
  std::uint64_t creator_tx = 0;
  std::uint64_t event_sequence = 0;
  std::string overflow_uuid;
  std::uint64_t ordinal = 0;
  std::string payload_fragment;
  std::uint64_t checksum = 0;
};

struct CrudLargeValueRecord {
  std::uint64_t creator_tx = 0;
  std::uint64_t event_sequence = 0;
  std::string overflow_uuid;
  std::string table_uuid;
  std::string row_uuid;
  std::string version_uuid;
  std::string field_name;
  std::uint64_t total_bytes = 0;
  std::string content_hash;
  std::string state;
  std::vector<CrudLargeValueChunkRecord> chunks;
};

// A complete relation-descriptor replacement published inside a sealed MGA
// catalog migration event.  Visibility is determined from creator_tx and the
// exact event sequence; readers never rewrite or infer this snapshot from the
// migrated table spelling.
struct CrudSealedRelationDescriptorSnapshot {
  std::uint64_t creator_tx = 0;
  std::uint64_t event_sequence = 0;
  std::string relation_uuid;
  std::string relation_descriptor_uuid;
  std::uint64_t relation_descriptor_generation = 0;
  std::uint64_t descriptor_field_count = 0;
  std::uint64_t descriptor_field_bytes = 0;
  std::uint32_t contextual_sidecar_count = 0;
  std::vector<std::pair<std::string, std::string>> descriptor_fields;
};

// Read-only consumers share this neutral snapshot shape. CrudState is the
// legacy event-store materialization below; canonical MGA code uses its own
// derived view and cannot be passed to mutation APIs that require CrudState.
struct RelationReadSnapshot {
  std::map<std::uint64_t, std::string> transactions;
  std::vector<CrudTableRecord> tables;
  std::vector<CrudRowVersionRecord> row_versions;
  std::vector<CrudIndexRecord> indexes;
  std::vector<CrudIndexEntryRecord> index_entries;
  std::vector<CrudLargeValueRecord> large_values;
  std::vector<CrudSealedRelationDescriptorSnapshot>
      sealed_relation_descriptor_snapshots;
  std::uint64_t max_transaction_id = 0;
  std::uint64_t max_sequence = 0;
  std::uint64_t max_index_sequence = 0;
  std::uint64_t max_event_sequence = 0;
  std::map<std::uint64_t, std::map<std::string, std::uint64_t>> savepoints;
};

struct CrudState : RelationReadSnapshot {
  CrudState() = default;
  CrudState(const CrudState&) = default;
  CrudState(CrudState&&) noexcept = default;
  CrudState& operator=(const CrudState&) = default;
  CrudState& operator=(CrudState&&) noexcept = default;
  CrudState(const RelationReadSnapshot&) = delete;
  CrudState(RelationReadSnapshot&&) = delete;
};

struct CrudStoreResult {
  bool ok = false;
  EngineApiDiagnostic diagnostic;
  CrudState state;
};

std::string GenerateCrudEngineUuid(std::string kind, std::uint64_t unix_epoch_millis = 0);
bool IsEmptyUuid(const EngineUuid& uuid);
bool CrudCreatorVisible(const RelationReadSnapshot& state, std::uint64_t creator_tx, std::uint64_t event_sequence, std::uint64_t observer_tx);
std::string UuidStringOrGenerated(const EngineUuid& uuid, std::string kind);
CrudStoreResult LoadCrudState(const EngineRequestContext& context);
EngineApiDiagnostic AppendCrudEvent(const EngineRequestContext& context, const std::string& event);
EngineApiDiagnostic ValidateCrudDatabasePath(const EngineRequestContext& context, const std::string& operation_id);
EngineApiDiagnostic UnsupportedCrudFeatureDiagnostic(const std::string& operation_id, const std::string& feature);
std::string EncodeCrudText(const std::string& value);
std::string EncodeCrudPairs(const std::vector<std::pair<std::string, std::string>>& pairs);
std::vector<std::pair<std::string, std::string>> DecodeCrudPairs(const std::string& encoded);
std::string NormalizeCrudIndexProfile(const std::string& profile);
std::string CrudIndexFamilyForProfile(const std::string& profile);
bool IsSupportedCrudIndexProfile(const std::string& profile);
bool IsApproximateCrudIndexFamily(const std::string& family);
std::vector<std::pair<std::string, std::string>> RowValuePairs(const EngineRowValue& row);
std::size_t EncodedValueBytes(const std::vector<std::pair<std::string, std::string>>& values);
bool CrudValueIsLargeValueLocator(const std::string& value);
EngineApiDiagnostic PersistCrudLargeValuesForRow(const EngineRequestContext& context,
                                                 const std::string& table_uuid,
                                                 const std::string& row_uuid,
                                                 const std::string& version_uuid,
                                                 bool force_large_value,
                                                 std::vector<std::pair<std::string, std::string>>* values,
                                                 std::vector<EngineEvidenceReference>* evidence);
std::string CrudFieldValue(const std::vector<std::pair<std::string, std::string>>& values, const std::string& field);
std::string CrudColumnDescriptorForName(const std::vector<std::pair<std::string, std::string>>& columns,
                                        const std::string& column_name);
bool CrudColumnDescriptorIsOpaqueRenderOnly(const std::string& descriptor);
bool CrudPredicateTouchesOpaqueColumn(const CrudTableRecord& table, const EnginePredicateEnvelope& predicate);
bool CrudRowsTouchOpaqueColumn(const CrudTableRecord& table, std::span<const EngineRowValue> rows);
bool CrudAssignmentsTouchOpaqueColumn(const CrudTableRecord& table,
                                      const std::vector<std::pair<std::string, EngineTypedValue>>& assignments);
std::optional<CrudTableRecord> FindVisibleCrudTable(const RelationReadSnapshot& state,
                                                    const std::string& table_uuid,
                                                    std::uint64_t observer_tx);
std::vector<CrudRowVersionRecord> VisibleCrudRows(const RelationReadSnapshot& state,
                                                  const std::string& table_uuid,
                                                  std::uint64_t observer_tx);
std::vector<CrudRowVersionRecord> VisibleCrudRowsForContext(const RelationReadSnapshot& state,
                                                            const std::string& table_uuid,
                                                            const EngineRequestContext& context);
bool CrudRowVersionVisibleToContext(const RelationReadSnapshot& state,
                                    const CrudRowVersionRecord& row,
                                    const EngineRequestContext& context);
std::optional<CrudRowVersionRecord> FindVisibleCrudRow(const RelationReadSnapshot& state,
                                                       const std::string& table_uuid,
                                                       const std::string& row_uuid,
                                                       std::uint64_t observer_tx);
std::optional<CrudRowVersionRecord> FindVisibleCrudRowForContext(const RelationReadSnapshot& state,
                                                                 const std::string& table_uuid,
                                                                 const std::string& row_uuid,
                                                                 const EngineRequestContext& context);
std::vector<CrudIndexRecord> VisibleCrudIndexesForTable(const RelationReadSnapshot& state,
                                                        const std::string& table_uuid,
                                                        std::uint64_t observer_tx);
std::vector<CrudIndexRecord> VisibleCrudIndexesForTableColumn(const RelationReadSnapshot& state,
                                                              const std::string& table_uuid,
                                                              const std::string& column_name,
                                                              std::uint64_t observer_tx);
bool CrudIndexSupportsPredicate(const CrudIndexRecord& index, const EnginePredicateEnvelope& predicate);
bool CrudRowMatchesPredicate(const CrudRowVersionRecord& row, const EnginePredicateEnvelope& predicate);
std::vector<std::string> CrudIndexKeysForValues(const CrudIndexRecord& index,
                                                const std::vector<std::pair<std::string, std::string>>& values);
std::vector<CrudRowVersionRecord> IndexedCrudRows(const RelationReadSnapshot& state,
                                                  const std::string& table_uuid,
                                                  const std::string& column_name,
                                                  const std::string& key_value,
                                                  std::uint64_t observer_tx,
                                                  std::string* index_uuid_used);
std::vector<CrudRowVersionRecord> IndexedCrudRowsForPredicate(const RelationReadSnapshot& state,
                                                              const std::string& table_uuid,
                                                              const EnginePredicateEnvelope& predicate,
                                                              std::uint64_t observer_tx,
                                                              std::uint64_t limit,
                                                              std::string* index_evidence_id);
std::vector<CrudRowVersionRecord> IndexedCrudRowsForPredicateForContext(const RelationReadSnapshot& state,
                                                                        const std::string& table_uuid,
                                                                        const EnginePredicateEnvelope& predicate,
                                                                        const EngineRequestContext& context,
                                                                        std::uint64_t limit,
                                                                        std::string* index_evidence_id);
std::string MakeCrudRowVersionEvent(std::uint64_t creator_tx,
                                    const std::string& table_uuid,
                                    const std::string& row_uuid,
                                    const std::string& version_uuid,
                                    bool deleted,
                                    const std::string& previous_version_uuid,
                                    std::uint64_t previous_sequence,
                                    const std::vector<std::pair<std::string, std::string>>& values);
std::string CrudIndexEvidenceId(const CrudIndexRecord& index,
                                const EnginePredicateEnvelope& predicate,
                                std::size_t candidate_count,
                                std::size_t visible_count);
std::string MakeCrudIndexCreateEvent(std::uint64_t creator_tx,
                                     const std::string& index_uuid,
                                     const std::string& table_uuid,
                                     const std::string& column_name,
                                     const std::string& profile,
                                     const std::string& default_name);
std::string MakeCrudIndexCreateEventV2(std::uint64_t creator_tx, const CrudIndexRecord& index);
std::string MakeCrudIndexEntryEvent(std::uint64_t creator_tx,
                                    const std::string& index_uuid,
                                    const std::string& table_uuid,
                                    const std::string& column_name,
                                    const std::string& key_value,
                                    const std::string& row_uuid,
                                    const std::string& version_uuid);
std::string MakeCrudIndexEntryEventV2(std::uint64_t creator_tx,
                                      const CrudIndexRecord& index,
                                      const std::string& key_value,
                                      const std::string& payload_value,
                                      const std::string& row_uuid,
                                      const std::string& version_uuid);
EngineApiDiagnostic ValidateCrudUniqueIndexesForRow(const RelationReadSnapshot& state,
                                                    const std::string& table_uuid,
                                                    const std::string& row_uuid,
                                                    const std::vector<std::pair<std::string, std::string>>& values,
                                                    const EngineRequestContext& context);
EngineApiDiagnostic AppendCrudIndexEntriesForIndex(const EngineRequestContext& context,
                                                   const CrudIndexRecord& index,
                                                   const std::string& row_uuid,
                                                   const std::string& version_uuid,
                                                   const std::vector<std::pair<std::string, std::string>>& values);
EngineApiDiagnostic AppendCrudIndexEntriesForRow(const EngineRequestContext& context,
                                                 const RelationReadSnapshot& state,
                                                 const std::string& table_uuid,
                                                 const std::string& row_uuid,
                                                 const std::string& version_uuid,
                                                 const std::vector<std::pair<std::string, std::string>>& values);
EngineApiDiagnostic ApplyCrudTemporaryOnCommitActions(const EngineRequestContext& context,
                                                      std::uint64_t local_transaction_id,
                                                      std::uint64_t* deleted_row_count);
EngineResultShape CrudRowsToResultShape(const std::vector<CrudRowVersionRecord>& rows);
void AddEmbeddedTrustModeEvidence(const EngineRequestContext& context, EngineApiResult* result);

template <typename TResult>
TResult MakeCrudSuccessResult(const EngineRequestContext& context, std::string operation_id) {
  TResult result;
  result.ok = true;
  result.operation_id = std::move(operation_id);
  AddEmbeddedTrustModeEvidence(context, &result);
  return result;
}

template <typename TResult>
TResult MakeCrudDiagnosticResult(const EngineRequestContext& context,
                                 std::string operation_id,
                                 EngineApiDiagnostic diagnostic) {
  TResult result;
  result.ok = false;
  result.operation_id = std::move(operation_id);
  result.diagnostics.push_back(std::move(diagnostic));
  AddEmbeddedTrustModeEvidence(context, &result);
  return result;
}

}  // namespace scratchbird::engine::internal_api

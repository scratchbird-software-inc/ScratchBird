// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "api_types.hpp"
#include "crud_support/crud_store.hpp"
#include "mga_relation_store/mga_relation_descriptor.hpp"
#include "secondary_index_delta_ledger.hpp"
#include "transaction_cleanup.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace scratchbird::engine::internal_api {

// SEARCH_KEY: SB_ENGINE_MGA_RELATION_ROW_STORE_AUTHORITY
// Local MGA row-version authority for engine-internal DML. This layer is not a
// reference/parser API and does not accept SQL names.

struct MgaRelationStoreState {
  CrudState crud_metadata;
  std::vector<CrudRowVersionRecord> row_versions;
  std::vector<CrudIndexEntryRecord> index_entries;
  std::uint64_t max_row_event_sequence = 0;
  std::uint64_t max_index_event_sequence = 0;
};

struct MgaRelationStoreResult {
  bool ok = false;
  EngineApiDiagnostic diagnostic;
  MgaRelationStoreState state;
  bool full_state_load = false;
  bool scoped_state_load = false;
  std::uint64_t row_versions_scanned = 0;
  std::uint64_t row_versions_retained = 0;
  std::uint64_t index_entries_scanned = 0;
  std::uint64_t index_entries_retained = 0;
  bool scoped_physical_segments_used = false;
  bool scoped_physical_segments_fallback = false;
  std::vector<EngineEvidenceReference> evidence;
};

struct MgaTemporaryTableVisibilityResult {
  bool ok = false;
  EngineApiDiagnostic diagnostic;
  bool table_visible = false;
  bool known_temporary = false;
  bool visible_to_session = false;
  bool hidden_by_temporary_visibility = false;
  CrudTableRecord table;
};

struct MgaTemporaryTableDropResult {
  bool ok = false;
  EngineApiDiagnostic diagnostic;
  bool target_was_temporary = false;
  bool metadata_retired = false;
  std::string temporary_scope;
  std::uint64_t deleted_row_count = 0;
  std::uint64_t reclaimed_large_value_count = 0;
};

struct MgaEventSequenceRangeReservation {
  bool ok = false;
  EngineApiDiagnostic diagnostic;
  std::string allocator_path;
  std::string stream_kind;
  std::string stream_path;
  std::uint64_t first = 0;
  std::uint64_t count = 0;
  std::uint64_t next = 0;
  bool bootstrapped_from_store = false;
};

struct MgaRelationStatistics {
  bool relation_found = false;
  std::uint64_t visible_row_estimate = 0;
  std::uint64_t retained_row_version_count = 0;
  std::uint64_t row_store_bytes = 0;
  std::uint64_t index_store_bytes = 0;
  std::uint64_t table_size_bytes = 0;
};

struct MgaRelationStatisticsResult {
  bool ok = false;
  EngineApiDiagnostic diagnostic;
  MgaRelationStatistics statistics;
};

struct MgaTemporaryRecoveryClassificationResult {
  bool ok = false;
  EngineApiDiagnostic diagnostic;
  std::string classification;
  std::string action;
  bool recovery_required = false;
  bool write_admission_must_remain_fenced = false;
  bool silent_inconsistency_refused = true;
  std::uint64_t durable_global_metadata_count = 0;
  std::uint64_t orphaned_private_metadata_count = 0;
  std::uint64_t active_or_unresolved_event_count = 0;
  std::uint64_t fenced_event_count = 0;
  std::uint64_t rolled_back_event_count = 0;
  std::uint64_t orphaned_row_count = 0;
  std::uint64_t cleaned_row_count = 0;
  std::uint64_t orphaned_large_value_count = 0;
  std::uint64_t reclaimed_large_value_count = 0;
  std::uint64_t retired_private_metadata_count = 0;
  std::vector<EngineEvidenceReference> evidence;
};

struct MgaIndexEntryRowInput {
  std::string row_uuid;
  std::string version_uuid;
  std::vector<std::pair<std::string, std::string>> values;
};

struct MgaIndexEntryAppendBatch {
  CrudIndexRecord index;
  std::string table_uuid;
  std::vector<MgaIndexEntryRowInput> rows;
};

struct MgaExactIndexEntryInput {
  std::string encoded_key;
  std::string payload_value;
  std::string row_uuid;
  std::string version_uuid;
};

struct MgaExactIndexEntryAppendBatch {
  CrudIndexRecord index;
  std::string table_uuid;
  std::vector<MgaExactIndexEntryInput> entries;
};

struct MgaRelationHotAppendCounters {
  std::uint64_t row_stream_opens = 0;
  std::uint64_t row_stream_flushes = 0;
  std::uint64_t row_range_reservations = 0;
  std::uint64_t row_versions_appended = 0;
  std::uint64_t index_stream_opens = 0;
  std::uint64_t index_stream_flushes = 0;
  std::uint64_t index_range_reservations = 0;
  std::uint64_t index_entries_appended = 0;
};

struct MgaLargeValuePersistBatchRowInput {
  std::string table_uuid;
  std::string row_uuid;
  std::string version_uuid;
  bool force_large_value = false;
  std::vector<std::pair<std::string, std::string>>* values = nullptr;
};

struct MgaLargeValuePersistBatchCounters {
  std::uint64_t rows_seen = 0;
  std::uint64_t values_overflowed = 0;
  std::uint64_t chunks_appended = 0;
  std::uint64_t preallocated_chunk_slots = 0;
  std::uint64_t payload_bytes = 0;
  std::uint64_t store_lines_appended = 0;
  std::uint64_t stream_opens = 0;
  std::uint64_t stream_flushes = 0;
};

class MgaRelationHotAppendContext {
 public:
  explicit MgaRelationHotAppendContext(const EngineRequestContext& context);
  ~MgaRelationHotAppendContext();

  MgaRelationHotAppendContext(const MgaRelationHotAppendContext&) = delete;
  MgaRelationHotAppendContext& operator=(const MgaRelationHotAppendContext&) = delete;
  MgaRelationHotAppendContext(MgaRelationHotAppendContext&&) noexcept;
  MgaRelationHotAppendContext& operator=(MgaRelationHotAppendContext&&) noexcept;

  EngineApiDiagnostic AppendRowVersions(
      std::vector<CrudRowVersionRecord>* rows,
      std::vector<std::uint64_t>* written_event_sequences);
  EngineApiDiagnostic FlushRowVersions();

  EngineApiDiagnostic AppendIndexEntryBatches(
      const std::vector<MgaIndexEntryAppendBatch>& batches);
  EngineApiDiagnostic AppendExactIndexEntryBatches(
      const std::vector<MgaExactIndexEntryAppendBatch>& batches);
  EngineApiDiagnostic FlushIndexEntries();

  EngineApiDiagnostic Flush();
  const MgaRelationHotAppendCounters& counters() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

// DPC_DEFERRED_INDEX_WRITE_PATH
struct MgaSecondaryIndexDeltaLedgerEntryInput {
  CrudIndexRecord index;
  std::string table_uuid;
  std::string row_uuid;
  std::string version_uuid;
  std::vector<std::pair<std::string, std::string>> values;
  scratchbird::core::index::SecondaryIndexDeltaKind delta_kind =
      scratchbird::core::index::SecondaryIndexDeltaKind::insert;
  std::string cleanup_horizon_token;
  std::string source_evidence_reference;
};

struct MgaSecondaryIndexDeltaLedgerResult {
  bool ok = false;
  EngineApiDiagnostic diagnostic;
  scratchbird::core::index::PersistentSecondaryIndexDeltaLedger ledger;
};

// DPC_SECONDARY_INDEX_DELTA_MERGE_AGENT_GATE
struct MgaSecondaryIndexDeltaMergeAgentRequest {
  std::string index_uuid;
  std::string table_uuid;
  std::uint64_t authoritative_cleanup_horizon_local_transaction_id = 0;
  bool cleanup_horizon_authoritative = false;
  std::uint64_t max_records_to_scan = 1024;
  std::uint64_t max_records_to_merge = 256;
  bool merge_disabled = false;
  std::string ipar_fault_injection_point;
};

struct MgaSecondaryIndexDeltaMergeAgentResult {
  bool ok = false;
  bool throttled = false;
  EngineApiDiagnostic diagnostic;
  std::uint64_t merged_count = 0;
  std::uint64_t retained_count = 0;
  std::uint64_t cleaned_count = 0;
  std::uint64_t scanned_count = 0;
  std::uint64_t authoritative_cleanup_horizon_local_transaction_id = 0;
  std::string index_uuid;
  std::string table_uuid;
  std::string throttle_or_refusal_reason;
  std::vector<EngineEvidenceReference> evidence;
};

// DPC_SECONDARY_INDEX_DELTA_RECOVERY_REPAIR
struct MgaSecondaryIndexDeltaRecoveryRepairRequest {
  std::string index_uuid;
  std::string table_uuid;
  std::uint64_t max_records_to_scan = 1024;
  bool repair_enabled = false;
  bool require_authoritative_base = true;
};

struct MgaSecondaryIndexDeltaRecoveryRepairResult {
  bool ok = false;
  bool repaired = false;
  bool refused = false;
  bool fail_closed = false;
  EngineApiDiagnostic diagnostic;
  std::uint64_t scanned_count = 0;
  std::uint64_t retained_count = 0;
  std::uint64_t removed_count = 0;
  std::uint64_t promoted_count = 0;
  std::uint64_t committed_premerge_count = 0;
  std::uint64_t merged_cleaned_count = 0;
  std::string recovery_class;
  std::string recovery_action;
  std::vector<EngineEvidenceReference> evidence;
};

// DPC_SECONDARY_INDEX_GARBAGE_CLEANUP_AGENT
struct MgaSecondaryIndexGarbageCleanupRequest {
  std::string index_uuid;
  std::string table_uuid;
  std::uint64_t max_records_to_scan = 1024;
  std::uint64_t max_records_to_clean = 256;
  bool engine_mga_authoritative = true;
  bool inventory_authoritative = true;
  bool inventory_complete = true;
  bool active_snapshot_inventory_authoritative = true;
};

struct MgaSecondaryIndexGarbageCleanupResult {
  bool ok = false;
  bool refused = false;
  bool fail_closed = false;
  bool budget_exhausted = false;
  bool horizon_blocked = false;
  bool validation_before_ok = false;
  bool validation_after_ok = false;
  EngineApiDiagnostic diagnostic;
  std::uint64_t cleaned_count = 0;
  std::uint64_t retained_count = 0;
  std::uint64_t scanned_count = 0;
  std::uint64_t before_delta_ledger_records = 0;
  std::uint64_t after_delta_ledger_records = 0;
  std::uint64_t authoritative_cleanup_horizon_local_transaction_id = 0;
  std::string decision;
  std::vector<EngineEvidenceReference> evidence;
};

struct MgaRelationPhysicalSweepRequest {
  MgaRelationStoreState state;
  std::vector<scratchbird::transaction::mga::LocalCleanupReclaimEvidenceRecord>
      reclaim_evidence_records;
  bool engine_mga_authoritative = false;
  bool cleanup_horizon_authoritative = false;
  std::uint64_t authoritative_cleanup_horizon_local_transaction_id = 0;
  std::uint64_t max_row_versions_to_scan = 0;
  std::uint64_t max_index_entries_to_scan = 0;
};

struct MgaRelationPhysicalSweepResult {
  bool ok = false;
  bool physical_state_mutated = false;
  bool fail_closed = false;
  EngineApiDiagnostic diagnostic;
  MgaRelationStoreState state;
  std::uint64_t scanned_row_version_count = 0;
  std::uint64_t removed_row_version_count = 0;
  std::uint64_t retained_row_version_count = 0;
  std::uint64_t scanned_index_entry_count = 0;
  std::uint64_t removed_index_entry_count = 0;
  std::uint64_t retained_index_entry_count = 0;
  std::vector<EngineEvidenceReference> evidence;
};

// DPC_SECONDARY_INDEX_DELTA_OVERLAY_LOOKUP
struct MgaIndexedRowsLookupResult {
  bool ok = false;
  bool index_used = false;
  bool index_refused = false;
  EngineApiDiagnostic diagnostic;
  std::vector<CrudRowVersionRecord> rows;
  std::string index_evidence_id;
  std::vector<EngineEvidenceReference> evidence;
};

MgaRelationStoreResult LoadMgaRelationStoreState(const EngineRequestContext& context);
MgaRelationStoreResult LoadMgaRelationStoreStateForInsertTarget(
    const EngineRequestContext& context,
    const std::string& table_uuid);
CrudState BuildCrudCompatibilityStateFromMga(const MgaRelationStoreState& state);
MgaTemporaryTableVisibilityResult CheckMgaTemporaryTableVisibility(
    const EngineRequestContext& context,
    const std::string& table_uuid);
MgaTemporaryTableDropResult DropMgaTemporaryTable(
    const EngineRequestContext& context,
    const std::string& table_uuid);
MgaRelationStatisticsResult EstimateMgaRelationStatistics(const EngineRequestContext& context,
                                                          const std::string& table_uuid,
                                                          bool include_indexes);
MgaRelationStatisticsResult EstimateMgaCatalogStatistics(const EngineRequestContext& context,
                                                         bool include_indexes);
MgaTemporaryRecoveryClassificationResult ClassifyMgaTemporaryRecoveryState(
    const EngineRequestContext& context);

EngineApiDiagnostic EnsureMgaRelationStorageDescriptor(const EngineRequestContext& context,
                                                       const CrudTableRecord& table,
                                                       const std::vector<CrudIndexRecord>& indexes,
                                                       MgaRelationStorageDescriptor* descriptor);

EngineApiDiagnostic AppendMgaRowVersion(const EngineRequestContext& context,
                                         const CrudRowVersionRecord& row,
                                         std::uint64_t* written_event_sequence);
EngineApiDiagnostic AppendMgaRowVersions(const EngineRequestContext& context,
                                          std::vector<CrudRowVersionRecord>* rows,
                                          std::vector<std::uint64_t>* written_event_sequences);
EngineApiDiagnostic AppendMgaTableMetadata(const EngineRequestContext& context,
                                           const CrudTableRecord& table);
EngineApiDiagnostic AppendMgaIndexMetadata(const EngineRequestContext& context,
                                           const CrudIndexRecord& index);

EngineApiDiagnostic AppendMgaIndexEntriesForRow(const EngineRequestContext& context,
                                                const CrudState& state,
                                                const std::string& table_uuid,
                                                const std::string& row_uuid,
                                                const std::string& version_uuid,
                                                const std::vector<std::pair<std::string, std::string>>& values);
EngineApiDiagnostic AppendMgaIndexEntriesForRows(const EngineRequestContext& context,
                                                 const CrudState& state,
                                                 const std::string& table_uuid,
                                                 const std::vector<MgaIndexEntryRowInput>& rows);
EngineApiDiagnostic AppendMgaIndexEntriesForRowsWithIndexes(const EngineRequestContext& context,
                                                            const std::vector<CrudIndexRecord>& indexes,
                                                            const std::string& table_uuid,
                                                            const std::vector<MgaIndexEntryRowInput>& rows);
EngineApiDiagnostic AppendMgaExactIndexEntryBatches(
    const EngineRequestContext& context,
    const std::vector<MgaExactIndexEntryAppendBatch>& batches);
EngineApiDiagnostic AppendMgaIndexEntriesForIndex(const EngineRequestContext& context,
                                                  const CrudIndexRecord& index,
                                                  const std::string& row_uuid,
                                                  const std::string& version_uuid,
                                                  const std::vector<std::pair<std::string, std::string>>& values);

MgaSecondaryIndexDeltaLedgerResult LoadMgaSecondaryIndexDeltaLedger(
    const EngineRequestContext& context);
EngineApiDiagnostic AppendMgaSecondaryIndexDeltaLedgerEntries(
    const EngineRequestContext& context,
    const std::vector<MgaSecondaryIndexDeltaLedgerEntryInput>& entries,
    std::vector<EngineEvidenceReference>* evidence);
EngineApiDiagnostic CommitMgaSecondaryIndexDeltaLedgerTransaction(
    const EngineRequestContext& context,
    std::uint64_t local_transaction_id);
EngineApiDiagnostic RollbackMgaSecondaryIndexDeltaLedgerTransaction(
    const EngineRequestContext& context,
    std::uint64_t local_transaction_id);
MgaSecondaryIndexDeltaMergeAgentResult MergeMgaSecondaryIndexDeltasForIndex(
    const EngineRequestContext& context,
    const MgaSecondaryIndexDeltaMergeAgentRequest& request);
MgaSecondaryIndexDeltaRecoveryRepairResult ValidateAndRepairMgaSecondaryIndexDeltaLedgerForIndex(
    const EngineRequestContext& context,
    const MgaSecondaryIndexDeltaRecoveryRepairRequest& request);
MgaSecondaryIndexGarbageCleanupResult CleanupMgaSecondaryIndexGarbageForIndex(
    const EngineRequestContext& context,
    const MgaSecondaryIndexGarbageCleanupRequest& request);
MgaRelationPhysicalSweepResult ApplyMgaRelationPhysicalSweepToState(
    const MgaRelationPhysicalSweepRequest& request);
MgaIndexedRowsLookupResult IndexedMgaRowsForPredicateForContext(
    const CrudState& state,
    const std::string& table_uuid,
    const EnginePredicateEnvelope& predicate,
    const EngineRequestContext& context,
    std::uint64_t limit);

EngineApiDiagnostic PersistMgaLargeValuesForRow(const EngineRequestContext& context,
                                                const std::string& table_uuid,
                                                const std::string& row_uuid,
                                                const std::string& version_uuid,
                                                bool force_large_value,
                                                std::vector<std::pair<std::string, std::string>>* values,
                                                std::vector<EngineEvidenceReference>* evidence);
EngineApiDiagnostic PersistMgaLargeValuesForRows(
    const EngineRequestContext& context,
    const std::vector<MgaLargeValuePersistBatchRowInput>& rows,
    MgaLargeValuePersistBatchCounters* counters,
    std::vector<EngineEvidenceReference>* evidence);

EngineApiDiagnostic CreateMgaSavepointMarker(const EngineRequestContext& context, const std::string& savepoint_name);
EngineApiDiagnostic ReleaseMgaSavepointMarker(const EngineRequestContext& context, const std::string& savepoint_name);
EngineApiDiagnostic RollbackToMgaSavepointMarker(const EngineRequestContext& context, const std::string& savepoint_name);
EngineApiDiagnostic ValidateMgaSavepointExists(const EngineRequestContext& context,
                                               const std::string& savepoint_name,
                                               const std::string& operation_id);
std::vector<std::string> ActiveMgaSavepointNames(const EngineRequestContext& context);
EngineApiDiagnostic ApplyMgaTemporaryOnCommitActions(const EngineRequestContext& context,
                                                     std::uint64_t local_transaction_id,
                                                     std::uint64_t* deleted_row_count,
                                                     std::uint64_t* reclaimed_large_value_count);
EngineApiDiagnostic ApplyMgaTemporarySessionCleanupActions(
    const EngineRequestContext& context,
    std::uint64_t local_transaction_id,
    std::uint64_t* deleted_row_count,
    std::uint64_t* reclaimed_large_value_count,
    std::uint64_t* retired_private_metadata_count);

void ClearMgaEventSequenceRangeCacheForTesting();
MgaEventSequenceRangeReservation ReserveMgaRowEventSequenceRangeForTesting(
    const EngineRequestContext& context,
    std::uint64_t count);

}  // namespace scratchbird::engine::internal_api

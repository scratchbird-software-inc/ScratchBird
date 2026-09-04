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
#include "mga_relation_store/mga_contextual_text_sidecar_set_v2.hpp"
#include "mga_relation_store/mga_relation_descriptor.hpp"
#include "secondary_index_delta_ledger.hpp"
#include "transaction_cleanup.hpp"
#include "transaction_snapshot.hpp"

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace scratchbird::engine::internal_api {

struct EngineContextualTextPolicyRowSetV2;

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

// One prevalidated, single-record metadata/catalog mutation.  The `sealed`
// state is generated only by the neutral engine append path; callers cannot
// provide or persist a partially assembled constraint mutation.
struct MgaConstraintMutationBatch {
  std::string format_version{"neutral_fk_mutation_batch_v1"};
  std::string batch_uuid;
  std::string batch_hash;
  std::uint32_t mutation_count{0};
  std::string database_uuid;
  std::string constraint_uuid;
  std::string owner_table_uuid;
  std::string child_schema_uuid;
  std::string child_relation_descriptor_uuid;
  std::uint64_t child_relation_descriptor_generation{0};
  std::string child_column_uuid;
  std::string parent_table_uuid;
  std::string parent_schema_uuid;
  std::string parent_relation_descriptor_uuid;
  std::uint64_t parent_relation_descriptor_generation{0};
  std::string parent_column_uuid;
  std::string parent_candidate_key_constraint_uuid;
  std::string key_descriptor_uuid;
  std::string support_uuid;
  std::string support_family;
  std::string support_policy;
  std::string match_policy;
  std::string on_update_action;
  std::string on_delete_action;
  std::string enforcement_timing;
  // Generation of this constraint-metadata stream only.  The immutable MGA
  // relation storage descriptor generation is an exact base binding and is
  // not advanced by the bounded D1 constraint bridge.
  std::uint64_t constraint_metadata_generation{0};
  std::uint64_t base_table_event_sequence{0};
  std::uint64_t parent_base_table_event_sequence{0};
  std::string constraint_name;
  std::string constraint_kind;
  std::string canonical_constraint_envelope;
  CrudTableRecord updated_table;
};

std::string ComputeMgaConstraintMutationBatchHash(
    const MgaConstraintMutationBatch& batch,
    std::uint64_t creator_local_transaction_id,
    std::uint64_t metadata_event_sequence);

// DATATYPE-BIGINT-CANONICAL-IDENTITY-MIGRATION-V1.  This is an MGA catalog
// migration record, not a runtime alias. Each requested row is matched against
// an exact visible metadata generation before the single sealed record is
// appended.
struct MgaBigintIdentityMigrationRow {
  std::string object_uuid;
  std::string column_uuid;
  std::uint64_t old_row_generation{0};
};

struct MgaBigintIdentityMigrationRequest {
  std::string migration_id{"core.datatype.bigint.identity.v1"};
  std::string prior_catalog_snapshot_uuid;
  std::string new_catalog_snapshot_uuid;
  std::uint64_t prior_catalog_generation{0};
  std::uint64_t new_catalog_generation{0};
  std::vector<MgaBigintIdentityMigrationRow> rows;
};

struct MgaBigintIdentityMigrationResult {
  bool ok = false;
  EngineApiDiagnostic diagnostic;
  std::string decision_sha256;
  std::uint64_t migrated_row_count{0};
  std::vector<EngineEvidenceReference> evidence;
};

// DATATYPE-INT32-CANONICAL-IDENTITY-MIGRATION-V1.  Unlike the historical
// bigint repair, the provisional INT seed used one UUID for both the datatype
// descriptor and the type.  The sealed MGA record therefore replaces both
// identities as one catalog mutation; it is never interpreted as an alias.
struct MgaInt32IdentityMigrationRow {
  std::string object_uuid;
  std::string column_uuid;
  std::uint64_t old_row_generation{0};
};

struct MgaInt32IdentityMigrationRequest {
  std::string migration_id{"core.datatype.int32.identity.v1"};
  std::string prior_catalog_snapshot_uuid;
  std::string new_catalog_snapshot_uuid;
  std::uint64_t prior_catalog_generation{0};
  std::uint64_t new_catalog_generation{0};
  std::vector<MgaInt32IdentityMigrationRow> rows;
};

struct MgaInt32IdentityMigrationResult {
  bool ok = false;
  EngineApiDiagnostic diagnostic;
  std::string decision_sha256;
  std::uint64_t migrated_row_count{0};
  std::vector<EngineEvidenceReference> evidence;
};

// DATATYPE-TEXT-CANONICAL-IDENTITY-MIGRATION-V1.  The provisional character
// seed reused one UUID for descriptor and type and carried no exact codec
// authority.  The sealed MGA mutation replaces both identities and installs
// the complete immutable UTF-8 codec tuple as one catalog publication.  It is
// never invoked by query admission and never treats the provisional UUID as an
// alias.
struct MgaTextIdentityMigrationRow {
  std::string object_uuid;
  std::string column_uuid;
  std::uint64_t old_row_generation{0};
};

struct MgaTextIdentityMigrationRequest {
  std::string migration_id{"core.datatype.text.identity.v1"};
  std::string prior_catalog_snapshot_uuid;
  std::string new_catalog_snapshot_uuid;
  std::uint64_t prior_catalog_generation{0};
  std::uint64_t new_catalog_generation{0};
  std::vector<MgaTextIdentityMigrationRow> rows;
};

struct MgaTextIdentityMigrationResult {
  bool ok = false;
  EngineApiDiagnostic diagnostic;
  std::string decision_sha256;
  std::uint64_t migrated_row_count{0};
  std::vector<EngineEvidenceReference> evidence;
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

struct MgaRelationStorageDescriptorLoadResult {
  bool ok = false;
  EngineApiDiagnostic diagnostic;
  MgaRelationStorageDescriptor descriptor;
};

// Immutable statement-level authority shared by every relation scan in one
// optimizer/executor boundary.  Transaction visibility and catalog/security
// epochs are represented once; each relation adds only its exact descriptor
// and base generation.
struct PreparedMgaHeapStatementAuthority {
  scratchbird::transaction::mga::SnapshotVectorDescriptor snapshot_vector;
  std::shared_ptr<const std::map<std::uint64_t, std::string>>
      transaction_states;
  std::shared_ptr<const scratchbird::storage::database::
                            LocalTransactionInventorySnapshot>
      transaction_inventory_snapshot;
  std::string database_uuid;
  std::string statement_uuid;
  std::string transaction_uuid;
  std::string statement_snapshot_uuid;
  std::string statement_metadata_snapshot_uuid;
  std::string catalog_epoch_uuid;
  std::string authorization_authority_uuid;
  std::uint64_t catalog_generation{0};
  std::uint64_t security_epoch{0};
  std::uint64_t policy_epoch{0};
  std::uint64_t resource_epoch{0};
  std::uint64_t local_transaction_id{0};
  std::string metadata_path;
  std::uint64_t metadata_file_size{0};
  std::int64_t metadata_file_mtime_ticks{0};
  std::string savepoint_path;
  std::uint64_t savepoint_file_size{0};
  std::int64_t savepoint_file_mtime_ticks{0};
  std::string descriptor_path;
  std::uint64_t descriptor_file_size{0};
  std::int64_t descriptor_file_mtime_ticks{0};
};

struct PreparedMgaHeapReadAuthority {
  std::shared_ptr<const PreparedMgaHeapStatementAuthority> statement;
  MgaRelationStorageDescriptor descriptor;
  std::uint64_t current_relation_base_generation{0};
  std::string relation_uuid;
  bool temporary{false};
  std::string temporary_scope;
  std::string temporary_session_uuid;
};

struct PreparedMgaHeapReadAuthorityCohort {
  std::shared_ptr<const PreparedMgaHeapStatementAuthority> statement;
  std::map<std::string, std::shared_ptr<const PreparedMgaHeapReadAuthority>>
      relations;
};

struct PreparedMgaHeapReadAuthorityCohortResult {
  bool ok{false};
  EngineApiDiagnostic diagnostic;
  std::shared_ptr<const PreparedMgaHeapReadAuthorityCohort> cohort;
};

PreparedMgaHeapReadAuthorityCohortResult PrepareMgaHeapReadAuthorities(
    const EngineRequestContext& context,
    std::span<const std::string> relation_uuids);
PreparedMgaHeapReadAuthorityCohortResult PrepareMgaHeapReadAuthorities(
    const EngineRequestContext& context,
    std::span<const std::string> relation_uuids,
    const scratchbird::transaction::mga::SnapshotVectorDescriptor&
        resolved_statement_snapshot);

// Cheap TOCTOU fence: exact statement/catalog/security/resource identity plus
// inventory and metadata/savepoint file identity.  It never reloads or
// rehashes the statement's immutable authority.
EngineApiDiagnostic RevalidatePreparedMgaHeapReadAuthorityCohort(
    const EngineRequestContext& context,
    const PreparedMgaHeapReadAuthorityCohort& cohort);

// SEARCH_KEY: SB_ENGINE_MGA_VISIBLE_CONTEXTUAL_TEXT_SIDECAR_SNAPSHOT_V2
// Engine-only, read-only projection of the one complete sealed descriptor row
// selected under the caller's exact MGA visibility boundary.  This is
// persisted target material, not contextual policy authority.  A target
// resolver must independently resolve the live policy rows and reconstruct
// the expected projected-column descriptors before calling the pure full-set
// validator.
struct MgaVisibleContextualTextSidecarSnapshotV2 {
  CrudTableRecord table;
  MgaRelationStorageDescriptor relation_descriptor;
  MgaContextualTextSidecarSetOwnerV2 owner;
  std::vector<MgaContextualTextDescriptorFieldPairV2>
      base_descriptor_fields;
  MgaContextualTextSidecarSetV2 sealed_sidecar_set;
};

struct MgaVisibleContextualTextSidecarSnapshotLoadResultV2 {
  bool ok = false;
  EngineApiDiagnostic diagnostic;
  MgaVisibleContextualTextSidecarSnapshotV2 snapshot;
};

// Complete engine-owned material selected for one authenticated contextual
// TEXT demand.  The policy rows remain a separately resolved input: this
// result contains only the exact public relation projection and the one
// MGA-visible sealed descriptor snapshot that it authenticates.
struct MgaContextualTextTargetSelectionV2 {
  std::vector<std::uint8_t> exact_public_relation_projection_v3;
  MgaContextualTextSidecarSetOwnerV2 sidecar_owner;
  std::vector<MgaContextualTextDescriptorFieldPairV2>
      base_descriptor_fields;
  std::vector<MgaContextualTextProjectedColumnV2> projected_columns;
  MgaContextualTextSidecarSetV2 sealed_sidecar_set;
};

struct MgaContextualTextTargetSelectionResultV2 {
  bool ok = false;
  EngineApiDiagnostic diagnostic;
  MgaContextualTextTargetSelectionV2 selection;
};

struct MgaVisibleHeapRelationReadRequest {
  std::string relation_uuid;
  const std::string* borrowed_relation_uuid = nullptr;
  std::uint64_t maximum_scanned_row_versions = 0;
  std::uint64_t maximum_decoded_bytes = 0;
  std::uint64_t maximum_output_rows = 0;
  // Optional operator-local grant for the bounded row-carrier work performed
  // after engine snapshot/catalog authority has been resolved. A zero value
  // preserves the legacy caller contract and produces no complete receipt.
  std::uint64_t maximum_memory_bytes = 0;
  std::function<bool()> cancellation_requested;
  const std::function<bool()>* borrowed_cancellation_requested = nullptr;
};

enum class MgaHeapReadFailureCategoryV1 : std::uint8_t {
  kNone = 0,
  kInvalidRequest,
  kResource,
  kCancellation,
  kMgaContext,
  kCatalog,
  kStorage,
  kCorruptStorage,
};

struct MgaVisibleHeapRelationReadResult {
  bool ok = false;
  EngineApiDiagnostic diagnostic;
  MgaRelationStorageDescriptor descriptor;
  std::vector<CrudRowVersionRecord> visible_rows;
  // Conservative physical base-mutation generation for the exact bounded
  // relation read. This is not MGA visibility or transaction-finality
  // authority and remains zero on every refused or incomplete read.
  std::uint64_t current_relation_base_generation = 0;
  std::uint64_t scanned_row_version_count = 0;
  std::uint64_t decoded_byte_count = 0;
  std::uint64_t visibility_recheck_count = 0;
  std::uint64_t invisible_row_version_count = 0;
  std::uint64_t tombstone_row_count = 0;
  bool scoped_physical_segment_used = false;
  bool cancellation_observed = false;
  MgaHeapReadFailureCategoryV1 failure_category =
      MgaHeapReadFailureCategoryV1::kNone;
  std::uint64_t current_live_memory_bytes = 0;
  std::uint64_t peak_live_memory_bytes = 0;
  std::uint64_t memory_grant_bytes = 0;
  bool memory_receipt_complete = false;
  std::vector<EngineEvidenceReference> evidence;
};

// COUNT(*) has no value-column dependency.  This facade therefore streams the
// exact persisted row-version metadata required for MGA visibility and chain
// validation while skipping value payloads.  Its bounds are independent of an
// optimizer candidate/output-row estimate: physical segment size and the
// operator-local memory grant are the authoritative finite resources.
struct MgaVisibleHeapRelationCountRequest {
  std::string relation_uuid;
  const std::string* borrowed_relation_uuid = nullptr;
  std::uint64_t maximum_decoded_bytes = 0;
  std::uint64_t maximum_memory_bytes = 0;
  std::function<bool()> cancellation_requested;
  const std::function<bool()>* borrowed_cancellation_requested = nullptr;
};

struct MgaVisibleHeapRelationCountResult {
  bool ok = false;
  EngineApiDiagnostic diagnostic;
  MgaRelationStorageDescriptor descriptor;
  std::uint64_t visible_row_count = 0;
  std::uint64_t current_relation_base_generation = 0;
  std::uint64_t scanned_row_version_count = 0;
  std::uint64_t decoded_byte_count = 0;
  std::uint64_t storage_bytes_read = 0;
  std::uint64_t visibility_recheck_count = 0;
  std::uint64_t invisible_row_version_count = 0;
  std::uint64_t tombstone_row_count = 0;
  bool scoped_physical_segment_used = false;
  bool cancellation_observed = false;
  MgaHeapReadFailureCategoryV1 failure_category =
      MgaHeapReadFailureCategoryV1::kNone;
  std::uint64_t current_live_memory_bytes = 0;
  std::uint64_t peak_live_memory_bytes = 0;
  std::uint64_t memory_grant_bytes = 0;
  bool memory_receipt_complete = false;
};

// Two-pass, snapshot-stable visible-row stream.  The first pass validates the
// complete MGA version graph and selects the newest visible source ordinals
// without retaining value payloads.  The second pass revalidates the exact
// authorized segment extents and presents one selected row at a time.  The
// consumer publishes its retained-memory receipt so storage and operator state
// share one engine-owned grant.
struct MgaVisibleHeapRelationStreamRequest {
  std::string relation_uuid;
  const std::string* borrowed_relation_uuid = nullptr;
  std::uint64_t maximum_decoded_bytes_per_pass = 0;
  std::uint64_t maximum_memory_bytes = 0;
  // Absent requires complete value-row delivery.  A present bound limits only
  // the second/value pass; the first pass still validates the complete MGA
  // version graph and publishes the exact visible cardinality.  Zero is a
  // valid bound and performs no value-row decoding.
  std::optional<std::uint64_t> maximum_delivered_visible_rows;
  std::uint64_t maximum_consumer_growth_bytes_per_row = 0;
  std::function<bool()> cancellation_requested;
  const std::function<bool()>* borrowed_cancellation_requested = nullptr;
  // Invoked exactly once after the first pass has established the immutable
  // descriptor and exact visible cardinality, but before any value row is
  // decoded.  Consumers use this boundary to validate descriptor binding and
  // reserve only the bounded carrier storage admitted by their shared grant.
  // A false return refuses the stream before value delivery.
  std::function<bool(const MgaRelationStorageDescriptor&, std::uint64_t,
                     std::uint64_t*)>
      prepare_consumer_for_visible_rows;
  std::function<std::uint64_t()> consumer_retained_memory_bytes;
  std::function<bool(std::uint64_t, const CrudRowVersionRecord&)>
      consume_visible_row;
};

struct MgaVisibleHeapRelationStreamResult {
  bool ok = false;
  EngineApiDiagnostic diagnostic;
  MgaRelationStorageDescriptor descriptor;
  std::uint64_t visible_row_count = 0;
  std::uint64_t delivered_row_count = 0;
  // The first-pass counters below remain complete regardless of a delivery
  // bound.  These three counters describe only the bounded second/value pass.
  std::uint64_t second_pass_scanned_row_version_count = 0;
  std::uint64_t second_pass_decoded_byte_count = 0;
  std::uint64_t second_pass_storage_bytes_read = 0;
  std::uint64_t current_relation_base_generation = 0;
  std::uint64_t scanned_row_version_count = 0;
  std::uint64_t decoded_byte_count = 0;
  std::uint64_t storage_bytes_read = 0;
  std::uint64_t visibility_recheck_count = 0;
  std::uint64_t invisible_row_version_count = 0;
  std::uint64_t tombstone_row_count = 0;
  bool scoped_physical_segment_used = false;
  bool cancellation_observed = false;
  MgaHeapReadFailureCategoryV1 failure_category =
      MgaHeapReadFailureCategoryV1::kNone;
  std::uint64_t current_live_memory_bytes = 0;
  std::uint64_t peak_live_memory_bytes = 0;
  std::uint64_t memory_grant_bytes = 0;
  bool memory_receipt_complete = false;
  bool complete_mga_chain_validation = false;
  bool exact_segment_extent_revalidated = false;
  bool complete_value_delivery = false;
  bool delivery_stopped_by_bound = false;
};

struct MgaRelationIndexOnlyProofEligibilityResult {
  bool ok = false;
  EngineApiDiagnostic diagnostic;
  bool eligible = false;
  bool summary_trusted = false;
  std::string refusal_reason;
  std::uint64_t row_version_count = 0;
  std::uint64_t tombstone_count = 0;
  std::uint64_t update_count = 0;
  std::vector<EngineEvidenceReference> evidence;
};

struct MgaMetadataWorkPresenceResult {
  bool ok = false;
  EngineApiDiagnostic diagnostic;
  bool has_work = false;
  std::uint64_t metadata_tables_scanned = 0;
  std::uint64_t metadata_tables_matched = 0;
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
  std::uint64_t allocator_stream_opens = 0;
  std::uint64_t allocator_stream_flushes = 0;
  std::uint64_t allocator_range_records_appended = 0;
  std::uint64_t row_stream_opens = 0;
  std::uint64_t row_stream_flushes = 0;
  std::uint64_t row_range_reservations = 0;
  std::uint64_t row_versions_appended = 0;
  std::uint64_t scoped_row_stream_opens = 0;
  std::uint64_t scoped_row_stream_flushes = 0;
  std::uint64_t scoped_row_write_batches = 0;
  std::uint64_t scoped_row_write_tickets_issued = 0;
  std::uint64_t scoped_row_write_tickets_completed = 0;
  std::uint64_t scoped_row_write_worker_count = 0;
  std::uint64_t scoped_row_binary_batches = 0;
  std::uint64_t scoped_row_binary_rows = 0;
  std::uint64_t scoped_row_binary_bytes = 0;
  std::uint64_t index_stream_opens = 0;
  std::uint64_t index_stream_flushes = 0;
  std::uint64_t index_range_reservations = 0;
  std::uint64_t index_entries_appended = 0;
  std::uint64_t index_materialization_jobs_queued = 0;
  std::uint64_t index_materialization_jobs_completed = 0;
  std::uint64_t index_materialization_inline_jobs = 0;
  std::uint64_t index_materialization_worker_count = 0;
  std::uint64_t index_materialization_sort_batches = 0;
  std::uint64_t index_materialized_entries = 0;
  std::uint64_t scoped_index_stream_opens = 0;
  std::uint64_t scoped_index_stream_flushes = 0;
  std::uint64_t scoped_index_write_batches = 0;
  std::uint64_t scoped_index_write_tickets_issued = 0;
  std::uint64_t scoped_index_write_tickets_completed = 0;
  std::uint64_t scoped_index_write_worker_count = 0;
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
  EngineApiDiagnostic AppendRowVersions(
      std::vector<CrudRowVersionRecord>* rows,
      const std::vector<std::vector<std::pair<std::string, std::string>>>*
          value_batch,
      std::vector<std::uint64_t>* written_event_sequences);
  EngineApiDiagnostic AppendRowVersionsReadOnly(
      const std::vector<CrudRowVersionRecord>& rows);
  EngineApiDiagnostic AppendRowVersionsReadOnly(
      const std::vector<CrudRowVersionRecord>& rows,
      const std::vector<std::vector<std::pair<std::string, std::string>>>*
          value_batch);
  EngineApiDiagnostic AppendRowVersionsReadOnlyScopedOnly(
      const std::vector<CrudRowVersionRecord>& rows,
      const std::vector<std::vector<std::pair<std::string, std::string>>>*
          value_batch,
      bool shared_key_order_known = false);
  EngineApiDiagnostic AppendRowVersionsReadOnlyScopedOnlyTyped(
      const std::vector<CrudRowVersionRecord>& rows,
      std::span<const EngineRowValue> typed_rows,
      std::span<const std::string> shared_field_order);
  EngineApiDiagnostic AppendRowVersionIdentitiesReadOnlyScopedOnlyTyped(
      const std::vector<CrudRowVersionRecord>& row_identities,
      const std::string& table_uuid,
      const std::string& temporary_session_uuid,
      std::span<const EngineRowValue> typed_rows,
      std::span<const std::string> shared_field_order);
  EngineApiDiagnostic AppendRowVersionIdentitiesReadOnlyScopedOnlyNativePacket(
      const std::vector<CrudRowVersionRecord>& row_identities,
      const std::string& table_uuid,
      const std::string& temporary_session_uuid,
      const EngineNativeRowPacketFrame& frame);
  EngineApiDiagnostic FlushRowVersions();
  void SetDecodedRowCacheAutoWarm(bool enabled);

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
MgaRelationStoreResult LoadMgaRelationStoreIndexesOnlyForInsertTarget(
    const EngineRequestContext& context,
    const std::string& table_uuid);
MgaRelationStoreResult LoadMgaRelationStoreMetadataOnlyForInsertTarget(
    const EngineRequestContext& context,
    const std::string& table_uuid);
MgaRelationStoreResult LoadMgaRelationStoreStateForMutationTarget(
    const EngineRequestContext& context,
    const std::string& table_uuid);
MgaRelationStoreResult LoadMgaRelationStoreStateForMutationTargets(
    const EngineRequestContext& context,
    const std::vector<std::string>& table_uuids);
MgaRelationStoreResult LoadMgaRelationStoreRowsOnlyForMutationTarget(
    const EngineRequestContext& context,
    const std::string& table_uuid);
MgaRelationStoreResult LoadMgaRelationStoreRowsOnlyForMutationTargets(
    const EngineRequestContext& context,
    const std::vector<std::string>& table_uuids);
std::uint64_t CurrentMgaRelationMetadataEventSequence(
    const EngineRequestContext& context);
std::uint64_t CurrentMgaSavepointAuthorityGeneration(
    const EngineRequestContext& context);
MgaRelationIndexOnlyProofEligibilityResult
CanUseMgaRelationIndexOnlyProofForInsertTarget(
    const EngineRequestContext& context,
    const std::string& table_uuid);
MgaMetadataWorkPresenceResult HasVisibleMgaDeferredConstraintMetadata(
    const EngineRequestContext& context);
MgaMetadataWorkPresenceResult HasMgaTemporaryCleanupMetadataWork(
    const EngineRequestContext& context,
    bool include_delete_rows,
    bool include_preserve_rows,
    bool retire_private_metadata);
CrudState BuildCrudCompatibilityStateFromMga(const MgaRelationStoreState& state);
CrudState BuildCrudCompatibilityStateFromMga(MgaRelationStoreState&& state);
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

// Loads an already-persisted descriptor for a relation visible to the exact
// active MGA transaction. This path never synthesizes or persists a descriptor.
MgaRelationStorageDescriptorLoadResult LoadMgaRelationStorageDescriptor(
    const EngineRequestContext& context,
    const std::string& relation_uuid);

MgaVisibleContextualTextSidecarSnapshotLoadResultV2
LoadVisibleMgaContextualTextSidecarSnapshotV2(
    const EngineRequestContext& context,
    const std::string& relation_uuid,
    const std::string& relation_descriptor_uuid,
    std::uint64_t relation_descriptor_generation);

MgaContextualTextTargetSelectionResultV2
SelectVisibleMgaContextualTextTargetV2(
    const EngineRequestContext& context,
    const sblr::ContextualTextLiteralDemandV2& structural_claim,
    const EngineContextualTextPolicyRowSetV2& exact_policy_rows);

// QOW-SOURCE-QRY-004-HEAP-MGA-V1
// Reads one current persisted local heap descriptor and its visible row
// versions under the exact active MGA transaction/snapshot.  The facade is
// bounded before and during physical segment decoding and has no SQL, parser,
// candidate, transaction-finality, recovery, or WAL authority.
MgaVisibleHeapRelationReadResult ReadVisibleMgaHeapRelation(
    const EngineRequestContext& context,
    const MgaVisibleHeapRelationReadRequest& request);

// Exact, bounded MGA-visible COUNT(*) source.  No row/value payload is
// returned or retained and parser/optimizer candidate counts have no semantic
// authority over the result.
MgaVisibleHeapRelationCountResult CountVisibleMgaHeapRelation(
    const EngineRequestContext& context,
    const MgaVisibleHeapRelationCountRequest& request);

MgaVisibleHeapRelationStreamResult StreamVisibleMgaHeapRelation(
    const EngineRequestContext& context,
    const MgaVisibleHeapRelationStreamRequest& request);

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
EngineApiDiagnostic AppendMgaTableMetadataWithSealedContextualTextDescriptorV2(
    const EngineRequestContext& context,
    const CrudTableRecord& table,
    const std::vector<CrudIndexRecord>& indexes,
    MgaRelationStorageDescriptor* descriptor);
EngineApiDiagnostic AppendMgaConstraintMutationBatch(
    const EngineRequestContext& context,
    const MgaConstraintMutationBatch& batch);
MgaBigintIdentityMigrationResult AppendMgaBigintIdentityMigrationBatch(
    const EngineRequestContext& context,
    const MgaBigintIdentityMigrationRequest& request);

MgaInt32IdentityMigrationResult AppendMgaInt32IdentityMigrationBatch(
    const EngineRequestContext& context,
    const MgaInt32IdentityMigrationRequest& request);

MgaTextIdentityMigrationResult AppendMgaTextIdentityMigrationBatch(
    const EngineRequestContext& context,
    const MgaTextIdentityMigrationRequest& request);
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

// SEARCH_KEY: SB_ENGINE_MGA_DML_UPDATE_STATEMENT_SAVEPOINT_AUTHORITY_V1
// Durable, typed MGA authority for one dml.update_rows statement savepoint.
// The private marker label used by the append-only MGA store is intentionally
// absent: it routes storage mechanics but is never an authority or lookup key.
using MgaDmlUpdateStatementAuthoritySha256V1 =
    std::array<std::uint8_t, 32>;

// SEARCH_KEY: SB_ENGINE_MGA_DML_UPDATE_DURABLE_OPERATION_STORE_V1
// MGA-owned binary durability for one bound dml.update_rows operation.  These
// are opaque, already codec-validated carriers: the store preserves exact
// bytes and validates only authenticated ownership, fixed carrier extents,
// and journal compare-and-append metadata.  It never reconstructs authority
// from SQL text, names, hashes, or the retired descriptor-registry sidecar.
using MgaDmlUpdateDurableSha256V1 = std::array<std::uint8_t, 32>;

enum class MgaDmlUpdateDurableJournalStateV1 : std::uint8_t {
  bound = 1,
  intent = 2,
  prepared = 3,
  published = 4,
  aborted = 5,
};

enum class MgaDmlUpdateDurableOperationOutcomeV1 : std::uint8_t {
  committed = 1,
  already_exact = 2,
  access_denied = 3,
  stale = 4,
  conflict = 5,
  fork_or_terminal_conflict = 6,
  quarantined = 7,
  storage_failure = 8,
};

struct MgaDmlUpdateDurableOperationIdentityV1 {
  std::string database_uuid;
  std::string owning_transaction_uuid;
  std::uint64_t owning_local_transaction_id = 0;
  std::string authenticated_statement_receipt_uuid;
  std::string operation_uuid;
  std::uint64_t operation_generation = 0;
  std::string descriptor_uuid;
  std::uint64_t descriptor_generation = 0;
  std::string recovery_token_uuid;
  std::uint64_t recovery_generation = 0;
  // Exact private MGA durable-registry identity carried by DURC.  It is not
  // a parser/public handle and is never inferred from another UUID.
  std::string validated_durable_handle_uuid;
  std::uint64_t validated_durable_handle_generation = 0;
  std::string reserved_statement_barrier_uuid;
  std::uint64_t reserved_statement_barrier_generation = 0;

  bool operator==(
      const MgaDmlUpdateDurableOperationIdentityV1&) const = default;
};

struct MgaDmlUpdateDurableOperationLookupV1 {
  std::string descriptor_uuid;
  std::uint64_t descriptor_generation = 0;
  std::uint64_t structural_occurrence_id = 0;
};

struct MgaDmlUpdateDurableAuthoritySnapshotV1 {
  std::vector<std::uint8_t> assignment_vector_duav;
  std::vector<std::uint8_t> predicate_vector_duev;
  std::vector<std::uint8_t> row_policy_vector_dupv;
  std::vector<std::uint8_t> constraint_vector_ducv;
  std::vector<std::uint8_t> trigger_vector_dutv;
  std::vector<std::uint8_t> target_order_duor;
  std::vector<std::uint8_t> resource_budget_dubr;
  std::vector<std::uint8_t> recovery_token_durc;
  // Exact Core DUSV104 + N*DUSR256 and exact DUSP576.  The MGA store keeps
  // these byte-identical and never reconstructs source policy rows from DUPV.
  std::vector<std::uint8_t> source_policy_vector_dusv;
  std::vector<std::uint8_t> security_snapshot_proof_dusp;
  std::vector<std::uint8_t> descriptor_dudc;
  // Exact Core DUDV104+N*DUDR256 and DUOV104+N*DUOE288 vectors.  These are
  // engine-registry authority carriers, never length-prefixed host rows.
  std::vector<std::uint8_t> datatype_authority_vector_dudv;
  std::vector<std::uint8_t> builtin_operator_authority_vector_duov;

  bool operator==(
      const MgaDmlUpdateDurableAuthoritySnapshotV1&) const = default;
};

struct MgaDmlUpdateDurableJournalExtentV1 {
  std::uint64_t journal_sequence = 0;
  MgaDmlUpdateDurableJournalStateV1 lifecycle_state =
      MgaDmlUpdateDurableJournalStateV1::bound;
  MgaDmlUpdateDurableSha256V1 prior_record_sha256{};
  MgaDmlUpdateDurableSha256V1 record_evidence_sha256{};
  std::vector<std::uint8_t> exact_dujr_bytes;

  bool operator==(
      const MgaDmlUpdateDurableJournalExtentV1&) const = default;
};

struct MgaDmlUpdateDurablePublishBoundRequestV1 {
  MgaDmlUpdateDurableOperationIdentityV1 identity;
  MgaDmlUpdateDurableAuthoritySnapshotV1 authority_snapshot;
  MgaDmlUpdateDurableJournalExtentV1 bound_journal;
};

struct MgaDmlUpdateDurableAuthorityReservationRequestV1 {
  std::string operation_uuid;
  std::uint64_t operation_generation = 0;
  std::string descriptor_uuid;
  std::uint64_t descriptor_generation = 0;
  std::string recovery_token_uuid;
  std::uint64_t recovery_generation = 0;
};

struct MgaDmlUpdateDurableAuthorityReservationResultV1 {
  MgaDmlUpdateDurableOperationOutcomeV1 outcome =
      MgaDmlUpdateDurableOperationOutcomeV1::storage_failure;
  EngineApiDiagnostic diagnostic;
  MgaDmlUpdateDurableOperationIdentityV1 identity;

  bool ok() const {
    return outcome == MgaDmlUpdateDurableOperationOutcomeV1::committed ||
           outcome == MgaDmlUpdateDurableOperationOutcomeV1::already_exact;
  }
};

struct MgaDmlUpdateDurableAppendSuccessorRequestV1 {
  MgaDmlUpdateDurableOperationIdentityV1 identity;
  std::uint64_t expected_prior_sequence = 0;
  MgaDmlUpdateDurableJournalStateV1 expected_prior_state =
      MgaDmlUpdateDurableJournalStateV1::bound;
  MgaDmlUpdateDurableSha256V1 expected_prior_record_evidence_sha256{};
  MgaDmlUpdateDurableJournalExtentV1 successor;
};

struct MgaDmlUpdateDurableOperationMutationResultV1;
struct MgaDmlUpdateDurableOperationPrepareResultV1;

class MgaDmlUpdateDurablePreparedSuccessorV1 final {
 public:
  MgaDmlUpdateDurablePreparedSuccessorV1();
  ~MgaDmlUpdateDurablePreparedSuccessorV1();
  MgaDmlUpdateDurablePreparedSuccessorV1(
      MgaDmlUpdateDurablePreparedSuccessorV1&&) noexcept;
  MgaDmlUpdateDurablePreparedSuccessorV1& operator=(
      MgaDmlUpdateDurablePreparedSuccessorV1&&) noexcept;
  MgaDmlUpdateDurablePreparedSuccessorV1(
      const MgaDmlUpdateDurablePreparedSuccessorV1&) = delete;
  MgaDmlUpdateDurablePreparedSuccessorV1& operator=(
      const MgaDmlUpdateDurablePreparedSuccessorV1&) = delete;

  bool valid() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;

  friend struct MgaDmlUpdateDurableOperationPrepareResultV1;
  friend MgaDmlUpdateDurableOperationPrepareResultV1
  PrepareMgaDmlUpdateDurableOperationSuccessorV1(
      const EngineRequestContext&,
      const MgaDmlUpdateDurableAppendSuccessorRequestV1&);
  friend MgaDmlUpdateDurableOperationMutationResultV1
  CommitPreparedMgaDmlUpdateDurableOperationSuccessorV1(
      MgaDmlUpdateDurablePreparedSuccessorV1&&);
  friend MgaDmlUpdateDurableOperationMutationResultV1
  CancelPreparedMgaDmlUpdateDurableOperationSuccessorV1(
      MgaDmlUpdateDurablePreparedSuccessorV1&&);
};

struct MgaDmlUpdateDurableOperationMutationResultV1 {
  MgaDmlUpdateDurableOperationOutcomeV1 outcome =
      MgaDmlUpdateDurableOperationOutcomeV1::storage_failure;
  EngineApiDiagnostic diagnostic;

  bool ok() const {
    return outcome == MgaDmlUpdateDurableOperationOutcomeV1::committed ||
           outcome == MgaDmlUpdateDurableOperationOutcomeV1::already_exact;
  }
};

struct MgaDmlUpdateDurableOperationPrepareResultV1 {
  MgaDmlUpdateDurableOperationOutcomeV1 outcome =
      MgaDmlUpdateDurableOperationOutcomeV1::storage_failure;
  EngineApiDiagnostic diagnostic;
  MgaDmlUpdateDurablePreparedSuccessorV1 prepared;

  bool ok() const {
    return outcome == MgaDmlUpdateDurableOperationOutcomeV1::committed &&
           prepared.valid();
  }
};

struct MgaDmlUpdateDurableOperationInstrumentationV1 {
  std::uint64_t prepare_calls = 0;
  std::uint64_t frame_encode_calls = 0;
  std::uint64_t checksum_calls = 0;
  std::uint64_t commit_calls = 0;
  std::uint64_t commit_write_calls = 0;
  std::uint64_t commit_fsync_calls = 0;
  std::uint64_t recovery_calls = 0;
  std::uint64_t observation_encode_calls = 0;
};

struct EngineSecurityPolicySnapshotRecoveryResultV1;
struct MgaDmlUpdateDurableOperationRecoveryResultV1;

// Private proof that RecoverChain authenticated the exact MGA durable handle,
// immutable carrier snapshot, DUJR head and DUMO.  Construction is confined to
// the MGA store; the public API, parser and wire cannot forge one.
class MgaDmlUpdateValidatedDurableAuthorityHandleV1 final {
 public:
  MgaDmlUpdateValidatedDurableAuthorityHandleV1();
  ~MgaDmlUpdateValidatedDurableAuthorityHandleV1();
  MgaDmlUpdateValidatedDurableAuthorityHandleV1(
      MgaDmlUpdateValidatedDurableAuthorityHandleV1&&) noexcept;
  MgaDmlUpdateValidatedDurableAuthorityHandleV1& operator=(
      MgaDmlUpdateValidatedDurableAuthorityHandleV1&&) noexcept;
  MgaDmlUpdateValidatedDurableAuthorityHandleV1(
      const MgaDmlUpdateValidatedDurableAuthorityHandleV1&) = delete;
  MgaDmlUpdateValidatedDurableAuthorityHandleV1& operator=(
      const MgaDmlUpdateValidatedDurableAuthorityHandleV1&) = delete;

 bool valid() const;

 private:
  struct Impl {
    MgaDmlUpdateDurableOperationIdentityV1 identity;
    MgaDmlUpdateDurableAuthoritySnapshotV1 snapshot;
    std::vector<MgaDmlUpdateDurableJournalExtentV1> journal;
    bool staged_successor_present = false;
    MgaDmlUpdateDurableJournalExtentV1 staged_successor;
    std::vector<std::uint8_t> staged_encoded_journal_frame;
    std::uint64_t authenticated_store_extent_bytes = 0;
    std::vector<std::uint8_t> exact_dumo;
  };
  std::unique_ptr<Impl> impl_;

  friend struct MgaDmlUpdateDurableOperationRecoveryResultV1;
  friend MgaDmlUpdateDurableOperationRecoveryResultV1
  RecoverMgaDmlUpdateDurableOperationChainV1(
      const EngineRequestContext&,
      const MgaDmlUpdateDurableOperationLookupV1&);
  friend EngineSecurityPolicySnapshotRecoveryResultV1
  RecoverEngineSecurityPolicySnapshotFromValidatedDmlUpdateDurableAuthorityV1(
      const EngineRequestContext&,
      const MgaDmlUpdateValidatedDurableAuthorityHandleV1&,
      std::span<const std::uint8_t>);
  friend MgaDmlUpdateDurableOperationMutationResultV1
  RollbackMgaDmlUpdateStatementFromValidatedDurableAuthorityV1(
      const EngineRequestContext&,
      const MgaDmlUpdateValidatedDurableAuthorityHandleV1&);
  friend MgaDmlUpdateDurableOperationMutationResultV1
  CommitRecoveredMgaDmlUpdateDurableOperationStagedSuccessorV1(
      const EngineRequestContext&,
      const MgaDmlUpdateValidatedDurableAuthorityHandleV1&);
};

struct MgaDmlUpdateDurableOperationRecoveryResultV1 {
  MgaDmlUpdateDurableOperationOutcomeV1 outcome =
      MgaDmlUpdateDurableOperationOutcomeV1::storage_failure;
  EngineApiDiagnostic diagnostic;
  MgaDmlUpdateDurableOperationIdentityV1 identity;
  MgaDmlUpdateDurableAuthoritySnapshotV1 authority_snapshot;
  std::vector<MgaDmlUpdateDurableJournalExtentV1> journal;
  // Exact successor prepared and durably staged before the publication
  // barrier but not yet CAS-published into the DUJR chain.  Recovery may only
  // publish this byte-identical successor through the validated-handle API.
  bool staged_successor_present = false;
  MgaDmlUpdateDurableJournalExtentV1 staged_successor;
  // Exact provider-authenticated DUMO416.  All transaction/savepoint/barrier
  // decisions are decoded from this carrier by the UPDATE recovery consumer.
  std::vector<std::uint8_t> recovery_observation_dumo;
  MgaDmlUpdateValidatedDurableAuthorityHandleV1 validated_handle;
  bool quarantined = false;

  bool ok() const {
    return outcome == MgaDmlUpdateDurableOperationOutcomeV1::committed;
  }
};

MgaDmlUpdateDurableOperationMutationResultV1
PublishMgaDmlUpdateDurableOperationBoundV1(
    const EngineRequestContext& context,
    const MgaDmlUpdateDurablePublishBoundRequestV1& request);
MgaDmlUpdateDurableAuthorityReservationResultV1
ReserveMgaDmlUpdateDurableOperationAuthorityV1(
    const EngineRequestContext& context,
    const MgaDmlUpdateDurableAuthorityReservationRequestV1& request);
MgaDmlUpdateDurableOperationMutationResultV1
AbandonMgaDmlUpdateDurableOperationAuthorityReservationV1(
    const EngineRequestContext& context,
    const MgaDmlUpdateDurableOperationIdentityV1& identity);
MgaDmlUpdateDurableOperationMutationResultV1
AppendMgaDmlUpdateDurableOperationSuccessorV1(
    const EngineRequestContext& context,
    const MgaDmlUpdateDurableAppendSuccessorRequestV1& request);
MgaDmlUpdateDurableOperationPrepareResultV1
PrepareMgaDmlUpdateDurableOperationSuccessorV1(
    const EngineRequestContext& context,
    const MgaDmlUpdateDurableAppendSuccessorRequestV1& request);
MgaDmlUpdateDurableOperationMutationResultV1
CommitPreparedMgaDmlUpdateDurableOperationSuccessorV1(
    MgaDmlUpdateDurablePreparedSuccessorV1&& prepared);
// Pre-publication cancellation durably invalidates the staged successor and
// releases its descriptor lock.  Only after this succeeds may the caller
// prepare/append an aborted successor for the same chain head.
MgaDmlUpdateDurableOperationMutationResultV1
CancelPreparedMgaDmlUpdateDurableOperationSuccessorV1(
    MgaDmlUpdateDurablePreparedSuccessorV1&& prepared);
MgaDmlUpdateDurableOperationRecoveryResultV1
RecoverMgaDmlUpdateDurableOperationChainV1(
    const EngineRequestContext& context,
    const MgaDmlUpdateDurableOperationLookupV1& lookup);
MgaDmlUpdateDurableOperationMutationResultV1
RollbackMgaDmlUpdateStatementFromValidatedDurableAuthorityV1(
    const EngineRequestContext& context,
    const MgaDmlUpdateValidatedDurableAuthorityHandleV1& validated_handle);
MgaDmlUpdateDurableOperationMutationResultV1
CommitRecoveredMgaDmlUpdateDurableOperationStagedSuccessorV1(
    const EngineRequestContext& context,
    const MgaDmlUpdateValidatedDurableAuthorityHandleV1& validated_handle);
MgaDmlUpdateDurableOperationInstrumentationV1
ReadMgaDmlUpdateDurableOperationInstrumentationV1();
void ResetMgaDmlUpdateDurableOperationInstrumentationForTestingV1();

enum class MgaDmlUpdateDurableFaultCutpointV1 : std::uint8_t {
  none = 0,
  before_snapshot_write,
  after_snapshot_write_before_bound,
  before_successor_write,
  after_successor_write_before_fsync,
  after_successor_fsync_before_ack,
  before_observation_write,
  after_observation_write_before_ack,
};

struct MgaDmlUpdateDurableInspectionV1 {
  MgaDmlUpdateDurableOperationOutcomeV1 outcome =
      MgaDmlUpdateDurableOperationOutcomeV1::storage_failure;
  EngineApiDiagnostic diagnostic;
  MgaDmlUpdateDurableAuthoritySnapshotV1 authority_snapshot;
  std::vector<MgaDmlUpdateDurableJournalExtentV1> journal;
  std::vector<std::uint8_t> exact_dumo_bytes;
  bool quarantined = false;
};

// Engine-private deterministic test hooks.  They authenticate the same lookup
// as RecoverChain and never expose a backing path.
void SetMgaDmlUpdateDurableFaultCutpointForTestingV1(
    MgaDmlUpdateDurableFaultCutpointV1 cutpoint);
MgaDmlUpdateDurableInspectionV1 InspectMgaDmlUpdateDurableOperationForTestingV1(
    const EngineRequestContext& context,
    const MgaDmlUpdateDurableOperationLookupV1& lookup);
bool CorruptMgaDmlUpdateDurableExtentByteForTestingV1(
    const EngineRequestContext& context,
    const MgaDmlUpdateDurableOperationLookupV1& lookup,
    std::uint64_t exact_file_offset, std::uint8_t xor_mask);
bool TruncateMgaDmlUpdateDurableExtentForTestingV1(
    const EngineRequestContext& context,
    const MgaDmlUpdateDurableOperationLookupV1& lookup,
    std::uint64_t exact_file_bytes);
bool QuarantineMgaDmlUpdateDurableOperationForTestingV1(
    const EngineRequestContext& context,
    const MgaDmlUpdateDurableOperationLookupV1& lookup);
bool DeleteMgaDmlUpdateDurableOperationForTestingV1(
    const EngineRequestContext& context,
    const MgaDmlUpdateDurableOperationLookupV1& lookup);

enum class MgaDmlUpdateStatementSavepointLifecycleV1 : std::uint8_t {
  active = 1,
  rolled_back = 2,
  released = 3,
};

struct MgaDmlUpdateStatementSavepointBindingV1 {
  std::string database_uuid;
  std::string owning_transaction_uuid;
  std::uint64_t owning_local_transaction_id = 0;
  std::string authenticated_statement_receipt_uuid;
  std::string operation_uuid;
  std::string descriptor_uuid;
  std::uint64_t descriptor_generation = 0;
  std::string recovery_token_uuid;
  std::uint64_t recovery_generation = 0;

  bool operator==(
      const MgaDmlUpdateStatementSavepointBindingV1&) const = default;
};

struct MgaDmlUpdateStatementSavepointAuthorityV1 {
  MgaDmlUpdateStatementSavepointBindingV1 binding;
  std::string savepoint_uuid;
  std::uint64_t savepoint_generation = 0;
  MgaDmlUpdateStatementSavepointLifecycleV1 lifecycle =
      MgaDmlUpdateStatementSavepointLifecycleV1::active;
  std::string publication_barrier_uuid;
  std::uint64_t publication_barrier_generation = 0;
  // The identity is reserved durably at savepoint creation so a complete
  // DURS can be encoded before the statement barrier.  Presence becomes true
  // only when release durably publishes that exact reserved barrier.
  bool publication_barrier_present = false;
  MgaDmlUpdateStatementAuthoritySha256V1 durable_presence_sha256{};

  bool operator==(
      const MgaDmlUpdateStatementSavepointAuthorityV1&) const = default;
};

struct MgaDmlUpdateStatementSavepointAuthorityResultV1 {
  bool ok = false;
  EngineApiDiagnostic diagnostic;
  MgaDmlUpdateStatementSavepointAuthorityV1 authority;
};

MgaDmlUpdateStatementSavepointAuthorityResultV1
CreateMgaDmlUpdateStatementSavepointAuthorityV1(
    const EngineRequestContext& context,
    const MgaDmlUpdateStatementSavepointBindingV1& binding);
MgaDmlUpdateStatementSavepointAuthorityResultV1
CreateMgaDmlUpdateStatementSavepointAuthorityWithReservedBarrierV1(
    const EngineRequestContext& context,
    const MgaDmlUpdateStatementSavepointBindingV1& binding,
    const std::string& reserved_publication_barrier_uuid,
    std::uint64_t reserved_publication_barrier_generation);
MgaDmlUpdateStatementSavepointAuthorityResultV1
RecoverMgaDmlUpdateStatementSavepointAuthorityV1(
    const EngineRequestContext& context,
    const MgaDmlUpdateStatementSavepointBindingV1& binding,
    const std::string& savepoint_uuid,
    std::uint64_t savepoint_generation);
MgaDmlUpdateStatementSavepointAuthorityResultV1
RevalidateMgaDmlUpdateStatementSavepointAuthorityV1(
    const EngineRequestContext& context,
    const MgaDmlUpdateStatementSavepointAuthorityV1& admitted);
MgaDmlUpdateStatementSavepointAuthorityResultV1
RollbackMgaDmlUpdateStatementSavepointAuthorityV1(
    const EngineRequestContext& context,
    const MgaDmlUpdateStatementSavepointAuthorityV1& admitted);
MgaDmlUpdateStatementSavepointAuthorityResultV1
ReleaseMgaDmlUpdateStatementSavepointAuthorityV1(
    const EngineRequestContext& context,
    const MgaDmlUpdateStatementSavepointAuthorityV1& admitted);

EngineApiDiagnostic CreateMgaSavepointMarker(const EngineRequestContext& context, const std::string& savepoint_name);
EngineApiDiagnostic ReleaseMgaSavepointMarker(const EngineRequestContext& context, const std::string& savepoint_name);
EngineApiDiagnostic RollbackToMgaSavepointMarker(const EngineRequestContext& context, const std::string& savepoint_name);
EngineApiDiagnostic ValidateMgaSavepointExists(const EngineRequestContext& context,
                                               const std::string& savepoint_name,
                                               const std::string& operation_id);

// SEARCH_KEY: SB_MGA_BULK_IMPORT_PUBLICATION_V1
// Transaction-local durable publication authority for an opcode-775 mutation.
// This record proves statement publication inside the owning MGA transaction;
// it is not transaction commit or cross-session visibility evidence.
using MgaBulkImportSha256V1 = std::array<std::uint8_t, 32>;

enum class MgaBulkImportPublicationLifecycleV1 : std::uint8_t {
  prepared = 1,
  published_uncommitted = 2,
  aborted = 3,
};

struct MgaBulkImportPublicationRecordV1 {
  MgaBulkImportPublicationLifecycleV1 lifecycle =
      MgaBulkImportPublicationLifecycleV1::prepared;
  std::string durable_publication_uuid;
  std::uint64_t durable_publication_generation = 0;
  MgaBulkImportSha256V1 recovery_idempotency_key{};
  std::string stream_uuid;
  std::uint64_t stream_generation = 0;
  MgaBulkImportSha256V1 descriptor_evidence{};
  std::string target_relation_uuid;
  std::uint64_t target_relation_generation = 0;
  std::string owning_transaction_uuid;
  std::uint64_t owning_local_transaction_id = 0;
  std::string authenticated_receipt_uuid;
  std::string statement_uuid;
  std::uint64_t savepoint_ordinal = 0;
  std::string mutation_uuid;
  std::string bulk_batch_uuid;
  MgaBulkImportSha256V1 content_sha256{};
  std::uint64_t total_stream_bytes = 0;
  std::uint64_t chunk_count = 0;
  std::uint64_t input_row_count = 0;
  std::uint64_t affected_rows = 0;
  std::uint64_t rejected_rows = 0;
  std::uint64_t imported_row_postcondition_count = 0;
  MgaBulkImportSha256V1 imported_row_postcondition_sha256{};
  MgaBulkImportSha256V1 normalized_statement_effect_sha256{};
  MgaBulkImportSha256V1 column_descriptor_set_sha256{};
  MgaBulkImportSha256V1 import_policy_bundle_sha256{};
  MgaBulkImportSha256V1 default_descriptor_set_sha256{};
  MgaBulkImportSha256V1 constraint_set_sha256{};
  MgaBulkImportSha256V1 trigger_set_sha256{};
  MgaBulkImportSha256V1 index_set_sha256{};
  std::uint64_t executor_availability_generation = 0;
  MgaBulkImportSha256V1 record_evidence_sha256{};

  bool operator==(const MgaBulkImportPublicationRecordV1&) const = default;
};

struct MgaBulkImportImportedRowEventV1 {
  std::string durable_publication_uuid;
  std::uint64_t durable_publication_generation = 0;
  MgaBulkImportSha256V1 recovery_idempotency_key{};
  std::string mutation_uuid;
  std::string bulk_batch_uuid;
  std::string owning_transaction_uuid;
  std::uint64_t owning_local_transaction_id = 0;
  std::string statement_uuid;
  std::uint64_t savepoint_ordinal = 0;
  std::string target_relation_uuid;
  std::uint64_t target_relation_generation = 0;
  std::uint64_t import_ordinal = 0;
  std::string row_uuid;
  std::string row_version_uuid;
  std::string row_image_uuid;
  std::uint64_t row_image_metadata_generation = 0;
  MgaBulkImportSha256V1 row_image_domain_hash{};
  MgaBulkImportSha256V1 row_image_value_hash{};
  MgaBulkImportSha256V1 column_descriptor_set_sha256{};
  MgaBulkImportSha256V1 canonical_typed_field_vector_sha256{};
  MgaBulkImportSha256V1 event_evidence_sha256{};

  bool operator==(const MgaBulkImportImportedRowEventV1&) const = default;
};

struct MgaBulkImportImportedRowEventResultV1 {
  bool ok = false;
  bool replayed = false;
  EngineApiDiagnostic diagnostic;
  std::vector<MgaBulkImportImportedRowEventV1> events;
};

struct MgaBulkImportPublicationResultV1 {
  bool ok = false;
  bool found = false;
  bool replayed = false;
  EngineApiDiagnostic diagnostic;
  MgaBulkImportPublicationRecordV1 record;
};

struct MgaBulkImportRowLineageResultV1 {
  bool ok = false;
  EngineApiDiagnostic diagnostic;
  std::vector<CrudRowVersionRecord> versions;
};

MgaBulkImportPublicationResultV1 PrepareMgaBulkImportPublicationV1(
    const EngineRequestContext& context,
    const MgaBulkImportPublicationRecordV1& record);
MgaBulkImportPublicationResultV1 PublishMgaBulkImportPublicationV1(
    const EngineRequestContext& context,
    const MgaBulkImportPublicationRecordV1& prepared_record);
MgaBulkImportPublicationResultV1 AbortMgaBulkImportPublicationV1(
    const EngineRequestContext& context,
    const MgaBulkImportPublicationRecordV1& prepared_record);
MgaBulkImportPublicationResultV1 RecoverMgaBulkImportPublicationV1(
    const EngineRequestContext& context,
    const MgaBulkImportSha256V1& recovery_idempotency_key);
MgaBulkImportRowLineageResultV1 ProbeMgaBulkImportRowIdentityLineageV1(
    const EngineRequestContext& context, const std::string& table_uuid,
    const std::string& row_uuid);
MgaBulkImportImportedRowEventResultV1 StoreMgaBulkImportImportedRowEventsV1(
    const EngineRequestContext& context,
    const std::vector<MgaBulkImportImportedRowEventV1>& events);
MgaBulkImportImportedRowEventResultV1 RecoverMgaBulkImportImportedRowEventsV1(
    const EngineRequestContext& context,
    const MgaBulkImportSha256V1& recovery_idempotency_key);

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

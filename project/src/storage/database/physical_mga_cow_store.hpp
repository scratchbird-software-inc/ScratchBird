// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "native_index_btree_page.hpp"
#include "database_dirty_manifest.hpp"

// SB-PHYSICAL-MGA-COW-ANCHOR
#include "copy_on_write.hpp"
#include "catalog_record_codec.hpp"
#include "catalog_name_envelope.hpp"
#include "catalog_page.hpp"
#include "row_data_page.hpp"
#include "row_version.hpp"
#include "runtime_platform.hpp"
#include "transaction_inventory.hpp"
#include "transaction_snapshot.hpp"

#include <string>
#include <map>
#include <optional>
#include <vector>

namespace scratchbird::storage::disk {
class FileDevice;
}

namespace scratchbird::storage::database {

using scratchbird::core::platform::DiagnosticRecord;
using scratchbird::core::platform::Status;
using scratchbird::core::platform::TypedUuid;
using scratchbird::core::platform::u16;
using scratchbird::core::platform::u32;
using scratchbird::core::platform::u64;

enum class PhysicalMgaCowMutationKind : u16 {
  insert,
  update,
  delete_row
};

struct NativeCatalogLeafPage {
  scratchbird::storage::disk::NativeCommonPageHeader header;
  scratchbird::storage::page::RowDataPageBody body;
};
enum class NativeCatalogLeafError {
  none, invalid_header, invalid_body, invalid_metadata, invalid_integrity,
  hash_failure, resource_exhausted, invalid_filespace, binding_mismatch,
  io_failure, encrypted_requires_crypto_authority, cluster_requires_authority,
  header_policy_requires_authority
};
struct NativeCatalogLeafResult {
  NativeCatalogLeafError error = NativeCatalogLeafError::invalid_body;
  std::optional<NativeCatalogLeafPage> page;
  std::map<scratchbird::core::platform::Uuid,
           scratchbird::core::catalog::CatalogMetadataVersion> metadata;
  std::vector<scratchbird::core::platform::byte> bytes;
  bool ok() const noexcept { return error == NativeCatalogLeafError::none && page.has_value(); }
};
// Logical image and common metadata binding only. No inventory finality,
// family/name/security admission, allocation or publication receipt is implied.
NativeCatalogLeafResult EncodeNativeCatalogLeaf(const NativeCatalogLeafPage&) noexcept;
NativeCatalogLeafResult DecodeNativeCatalogLeaf(
    const std::vector<scratchbird::core::platform::byte>&) noexcept;
NativeCatalogLeafResult ReadNativeCatalogLeafFromOpenDevice(
    scratchbird::storage::disk::FileDevice&,
    const scratchbird::core::platform::Uuid& database_uuid,
    const scratchbird::storage::page::NativeCatalogRootReference&) noexcept;

struct NativeCatalogRelationBinding {
  scratchbird::core::platform::Uuid relation_uuid;
  std::optional<scratchbird::storage::page::NativeBtreeDependencies> index_dependencies;
};
struct NativeCatalogIndexEntryLocation {
  std::size_t page_index = 0;
  std::size_t cell_index = 0;
};
struct NativeCatalogRowImageBinding {
  std::optional<NativeCatalogIndexEntryLocation> index_entry;
  std::size_t catalog_page_index = 0;
  std::size_t catalog_row_index = 0;
};
enum class NativeCatalogRelationError {
  none, invalid_reference, invalid_filespace, tree_failure, leaf_failure,
  binding_mismatch, invalid_locator, duplicate_identity, resource_exhausted,
  hash_failure, io_failure
};
struct NativeCatalogRelationImageResult {
  NativeCatalogRelationError error = NativeCatalogRelationError::invalid_reference;
  scratchbird::storage::page::NativeBtreeError tree_error = scratchbird::storage::page::NativeBtreeError::none;
  NativeCatalogLeafError leaf_error = NativeCatalogLeafError::none;
  std::optional<scratchbird::storage::page::NativeBtreeTreeResult> index;
  std::vector<NativeCatalogLeafResult> catalogs;
  std::vector<NativeCatalogRowImageBinding> bindings;
  u64 retained_image_bytes = 0;
  bool ok() const noexcept { return error==NativeCatalogRelationError::none; }
};
// NATIVE-CATALOG-RELATION-IMAGE-BINDING-001. Native image/row identity join only,
// not typed key, dependency-map, family, MGA visibility or publication authority.
NativeCatalogRelationImageResult ReadNativeCatalogRelationImagesFromOpenDevices(
    const scratchbird::core::platform::Uuid& database_uuid,
    const std::vector<scratchbird::storage::disk::NativeFilespaceDevice>&,
    const scratchbird::storage::page::NativeCatalogRootReference&,
    const NativeCatalogRelationBinding&,u64 maximum_retained_image_bytes) noexcept;

struct NativeCatalogCreatorBinding {
  std::size_t catalog_page_index = 0;
  std::size_t catalog_row_index = 0;
  std::size_t inventory_entry_index = 0;
};
enum class NativeCheckpointCatalogRelationError {
  none, invalid_reference, invalid_filespace, checkpoint_failure, relation_failure,
  missing_relation, creator_mismatch, cluster_requires_authority,
  resource_exhausted, io_failure
};
struct NativeCheckpointCatalogRelationResult {
  NativeCheckpointCatalogRelationError error = NativeCheckpointCatalogRelationError::invalid_reference;
  NativeCheckpointCatalogResult checkpoint;
  NativeCatalogRelationImageResult relation;
  std::vector<std::size_t> navigation_creator_entries;
  std::vector<NativeCatalogCreatorBinding> row_creators;
  u64 retained_image_bytes = 0;
  bool ok() const noexcept { return error==NativeCheckpointCatalogRelationError::none && checkpoint.ok() && relation.ok(); }
};
// Actual checkpoint -> catalog/feature role -> relation images -> inventory
// creator identity. Not snapshot, family/key, root-selection or publication authority.
NativeCheckpointCatalogRelationResult ReadNativeCheckpointCatalogRelationFromOpenDevices(
    const scratchbird::core::platform::Uuid& database_uuid,
    const std::vector<scratchbird::storage::disk::NativeFilespaceDevice>&,
    const scratchbird::storage::disk::FilespaceRootReference& checkpoint,
    u16 catalog_selector, u16 relation_role, const NativeCatalogRelationBinding&,
    u64 maximum_retained_image_bytes) noexcept;

enum class PhysicalMgaCowFinalizeDecision : u16 {
  commit,
  rollback,
  invalid
};

// Trusted native catalog staging. It neither authorizes DDL nor resolves a
// family/name/dependency. Callers supply the owning catalog relation/page and
// actual metadata; receipts come from the native MGA mutation, never an event.
struct NativeCatalogVersionMutation {
  TypedUuid relation_uuid;
  u64 page_number = 0;
  scratchbird::transaction::mga::TransactionIdentity transaction;
  scratchbird::core::catalog::CatalogMetadataVersion metadata;
  // Nil means create; replacement requires the exact observed native version.
  scratchbird::core::platform::Uuid expected_version_uuid;
  // Kind5 supplies a typed definition and an empty metadata payload. The
  // physical owner supplies the actual resident envelope at version allocation.
  std::optional<scratchbird::core::catalog::CatalogNamePayload> name_payload;
};

struct NativeCatalogVersionRow {
  scratchbird::core::catalog::CatalogMetadataVersion metadata;
  scratchbird::core::platform::Uuid version_uuid;
  scratchbird::core::platform::Uuid previous_version_uuid;
  std::optional<scratchbird::core::catalog::CatalogNamePayload> name_payload;
  bool provisional = false;
  scratchbird::core::catalog::CatalogObjectLifecycle effective_lifecycle = scratchbird::core::catalog::CatalogObjectLifecycle::creating;
  scratchbird::core::catalog::CatalogObjectStatus effective_status = scratchbird::core::catalog::CatalogObjectStatus::proposed;
};

struct NativeCatalogVersionReadResult {
  Status status;
  DiagnosticRecord diagnostic;
  // Includes visible retirement records so owning inspection/history code
  // can retain their evidence. Normal object lookup must omit retired rows.
  std::vector<NativeCatalogVersionRow> rows;
  bool ok() const { return status.ok(); }
};

enum class NativePinnedCatalogReadError {
  none, invalid_reader, invalid_filespace, snapshot_failure, reader_mismatch,
  source_failure, invalid_chain, missing_version, visibility_failure,
  requires_recovery, resource_exhausted, io_failure, duplicate_identity
};
struct NativeCatalogVisibilityObservation {
  std::size_t retained_row_index = 0;
  scratchbird::transaction::mga::VisibilityDecision decision =
      scratchbird::transaction::mga::VisibilityDecision::unknown;
};
struct NativePinnedCatalogReadResult {
  NativePinnedCatalogReadError error = NativePinnedCatalogReadError::invalid_reader;
  NativeCheckpointCatalogRelationResult source;
  scratchbird::core::platform::Uuid snapshot_uuid;
  std::vector<NativeCatalogVersionRow> rows;
  std::vector<NativeCatalogVisibilityObservation> observations;
  DiagnosticRecord diagnostic;
  bool ok() const noexcept { return error==NativePinnedCatalogReadError::none && source.ok(); }
};
// Native pinned selection, not catalog membership/security or root publication.
NativePinnedCatalogReadResult ReadNativePinnedCatalogVersionsFromOpenDevices(
    const scratchbird::core::platform::Uuid& database_uuid,
    const std::vector<scratchbird::storage::disk::NativeFilespaceDevice>&,
    const scratchbird::storage::disk::FilespaceRootReference& checkpoint,
    u16 catalog_selector, u16 relation_role, const NativeCatalogRelationBinding&,
    const scratchbird::transaction::mga::TransactionIdentity& reader,
    const scratchbird::transaction::mga::PublishedSnapshotPin&,
    u64 maximum_retained_image_bytes) noexcept;

// Path-free mutation fields for an already-owned node device. A storage
// operation cannot select or open a second node through this payload.
struct NativeCatalogNameMaterialization {
  scratchbird::core::catalog::CatalogMetadataVersion metadata;
  scratchbird::core::catalog::CatalogNamePayload payload;
};
struct PhysicalMgaCowMutation {
  TypedUuid relation_uuid;
  TypedUuid row_uuid;
  TypedUuid transaction_uuid;
  scratchbird::transaction::mga::LocalTransactionId existing_local_transaction_id;
  bool use_existing_transaction = false;
  PhysicalMgaCowMutationKind kind = PhysicalMgaCowMutationKind::insert;
  u64 page_number = 0;
  u64 begin_unix_epoch_millis = 0;
  u32 stable_slot_id = 0;
  std::vector<scratchbird::storage::page::RowDataCell> cells;
  // Exact reverse-chain link installed only when this request creates a new
  // row-data page. Zero identifies the tail of the chain.
  u64 predecessor_page_number = 0;
  std::optional<NativeCatalogNameMaterialization> catalog_name;
};

struct PhysicalMgaCowMutationRequest : PhysicalMgaCowMutation {
  std::string database_path;
};

struct PhysicalMgaCowMutationBatch {
  std::vector<PhysicalMgaCowMutation> mutations;
  // Optional explicit page sync. False never skips the mandatory node sync
  // in native inventory publication before the batch commit fence is released.
  bool sync_after_batch = true;
  bool engine_generated_unique_insert_rows = false;
};

struct PhysicalMgaCowMutationBatchRequest {
  std::vector<PhysicalMgaCowMutationRequest> mutations;
  // Same durability rule as the retained-device batch above.
  bool sync_after_batch = true;
  bool engine_generated_unique_insert_rows = false;
};

struct PhysicalMgaCowFinalization {
  scratchbird::transaction::mga::TransactionIdentity transaction;
  PhysicalMgaCowFinalizeDecision decision = PhysicalMgaCowFinalizeDecision::invalid;
  u64 final_unix_epoch_millis = 0;
};

struct PhysicalMgaCowFinalizeRequest : PhysicalMgaCowFinalization {
  std::string database_path;
};

struct PhysicalMgaCowReadRequest {
  std::string database_path;
  TypedUuid relation_uuid;
  u64 page_number = 0;
  scratchbird::transaction::mga::VisibilitySnapshot visibility_snapshot;
  scratchbird::transaction::mga::TransactionIdentity reader_identity;
  bool use_latest_committed_snapshot = true;
  // Borrowed engine-owned pin. When present, raw visibility fields must be
  // default/empty and latest-committed override must be disabled. The pin,
  // bound to reader_identity in the native inventory, supplies visibility.
  const scratchbird::transaction::mga::PublishedSnapshotPin* snapshot_pin = nullptr;
};

struct PhysicalMgaCowReadRow {
  scratchbird::storage::page::RowDataRecord row;
  scratchbird::transaction::mga::RowVersionMetadata metadata;
  scratchbird::transaction::mga::VisibilityDecision decision =
      scratchbird::transaction::mga::VisibilityDecision::unknown;
  bool visible = false;
  bool visible_delete_marker = false;
};

struct PhysicalMgaCowMutationResult {
  Status status;
  scratchbird::transaction::mga::LocalTransactionInventory inventory;
  scratchbird::transaction::mga::TransactionInventoryEntry transaction_entry;
  scratchbird::transaction::mga::CopyOnWriteMutationState mutation;
  scratchbird::storage::page::RowDataPageBody row_page;
  scratchbird::storage::page::RowDataRecord row_version;
  DiagnosticRecord diagnostic;
  std::vector<std::string> evidence;
  // Exact outer-page identity generated and written for this mutation.
  TypedUuid page_uuid;
  u64 page_generation = 0;

  // Present only if rollback of a helper-owned transaction cannot be confirmed.
  // Exact recovery identity, not a transaction-state or publication receipt.
  scratchbird::transaction::mga::TransactionIdentity unresolved_owned_transaction;

  bool ok() const {
    return status.ok();
  }
};

// Exact staged native version location, issued only after batch publication.
// This is not transaction commit, catalog authorization or a user SQL receipt.
struct PhysicalMgaCowRowReceipt {
  TypedUuid database_uuid;
  TypedUuid filespace_uuid;
  TypedUuid relation_uuid;
  TypedUuid row_uuid;
  TypedUuid page_uuid;
  scratchbird::transaction::mga::TransactionIdentity creator;
  scratchbird::core::platform::Uuid version_uuid;
  scratchbird::core::platform::Uuid previous_version_uuid;
  u64 page_number = 0;
  u64 page_generation = 0;
  u64 row_version = 0;
  u64 storage_generation = 0;
  u32 stable_slot_id = 0;
  bool deleted = false;
};

struct PhysicalMgaCowMutationBatchResult {
  Status status;
  DiagnosticRecord diagnostic;
  std::vector<std::string> evidence;
  u64 written_rows = 0;
  u64 pages_written = 0;
  // Same order as input mutations. Failed batches return no row receipts.
  std::vector<PhysicalMgaCowRowReceipt> row_receipts;
  // Failed/uncertain native publication barrier. Caller must inspect durable
  // inventory and roll back/recover this exact transaction, never commit a prefix.
  scratchbird::transaction::mga::TransactionIdentity unresolved_mutation_transaction;

  bool ok() const {
    return status.ok();
  }
};

struct PhysicalMgaCowFinalizeResult {
  Status status;
  scratchbird::transaction::mga::LocalTransactionInventory inventory;
  scratchbird::transaction::mga::TransactionInventoryEntry transaction_entry;
  DiagnosticRecord diagnostic;
  std::vector<std::string> evidence;

  bool ok() const {
    return status.ok();
  }
};

struct PhysicalMgaCowReadResult {
  Status status;
  scratchbird::transaction::mga::LocalTransactionInventory inventory;
  scratchbird::storage::page::RowDataPageBody row_page;
  std::vector<PhysicalMgaCowReadRow> rows;
  std::vector<scratchbird::storage::page::RowDataRecord> visible_rows;
  u64 visible_delete_marker_count = 0;
  u64 wait_for_transaction_count = 0;
  u64 rolled_back_version_count = 0;
  u64 recovery_required_count = 0;
  DiagnosticRecord diagnostic;
  std::vector<std::string> evidence;
  // Complete page metadata bound to the same validated native page/inventory.
  // This low-level ownership result is not a user-visible row projection.
  std::vector<scratchbird::transaction::mga::RowVersionMetadata> version_metadata;

  bool ok() const {
    return status.ok();
  }
};

const char* PhysicalMgaCowMutationKindName(PhysicalMgaCowMutationKind kind);
const char* PhysicalMgaCowFinalizeDecisionName(PhysicalMgaCowFinalizeDecision decision);

PhysicalMgaCowMutationResult WritePhysicalMgaCowUnpublishedMutation(
    const PhysicalMgaCowMutationRequest& request);
PhysicalMgaCowMutationBatchResult WritePhysicalMgaCowUnpublishedMutationBatch(
    PhysicalMgaCowMutationBatchRequest request);
// The caller retains the device and its exclusive ownership through return,
// including every failure. These functions neither reopen nor close it and
// do not commit the transaction or publish catalog roots.
PhysicalMgaCowMutationResult WritePhysicalMgaCowUnpublishedMutationToOpenDevice(
    scratchbird::storage::disk::FileDevice& device,
    const PhysicalMgaCowMutation& mutation);
PhysicalMgaCowMutationBatchResult WritePhysicalMgaCowUnpublishedMutationBatchToOpenDevice(
    scratchbird::storage::disk::FileDevice& device,
    PhysicalMgaCowMutationBatch batch);
PhysicalMgaCowFinalizeResult FinalizePhysicalMgaCowTransaction(
    const PhysicalMgaCowFinalizeRequest& request);
// Exact identity is checked against this retained node's current inventory.
// The caller retains ownership on success, refusal, IO failure and exception.
PhysicalMgaCowFinalizeResult FinalizePhysicalMgaCowTransactionToOpenDevice(
    scratchbird::storage::disk::FileDevice& device,
    const PhysicalMgaCowFinalization& request);
PhysicalMgaCowReadResult ReadPhysicalMgaCowRows(
    const PhysicalMgaCowReadRequest& request);
// Borrow the caller's already-owned device without reopening or releasing it.
// The device, never an independently supplied path, is the storage authority.
// A nonzero snapshot reader requires its exact native inventory identity.
// Default/empty identity is allowed only for an anonymous (zero-number) reader.
PhysicalMgaCowReadResult ReadPhysicalMgaCowRowsFromOpenDevice(
    scratchbird::storage::disk::FileDevice& device,
    const TypedUuid& relation_uuid,
    u64 page_number,
    const scratchbird::transaction::mga::VisibilitySnapshot& visibility_snapshot,
    bool use_latest_committed_snapshot,
    const scratchbird::transaction::mga::TransactionIdentity& reader_identity = {},
    const scratchbird::transaction::mga::PublishedSnapshotPin* snapshot_pin = nullptr);

DiagnosticRecord MakePhysicalMgaCowDiagnostic(Status status,
                                              std::string diagnostic_code,
                                              std::string message_key,
                                              std::string detail = {});

PhysicalMgaCowMutationResult WriteNativeCatalogVersionToOpenDevice(
    scratchbird::storage::disk::FileDevice& device, const NativeCatalogVersionMutation& mutation);
// One complete successor per row in a single existing transaction. Owning
// catalog operations still validate family semantics and global placement/indexes.
PhysicalMgaCowMutationBatchResult WriteNativeCatalogVersionsToOpenDevice(
    scratchbird::storage::disk::FileDevice& device,
    const std::vector<NativeCatalogVersionMutation>& mutations);
NativeCatalogVersionReadResult ReadNativeCatalogVersionsFromOpenDevice(
    scratchbird::storage::disk::FileDevice& device, const TypedUuid& relation_uuid,
    u64 page_number, const scratchbird::transaction::mga::VisibilitySnapshot& snapshot,
    bool latest_committed,
    const scratchbird::transaction::mga::TransactionIdentity& reader_identity = {},
    const scratchbird::transaction::mga::PublishedSnapshotPin* snapshot_pin = nullptr);

}  // namespace scratchbird::storage::database

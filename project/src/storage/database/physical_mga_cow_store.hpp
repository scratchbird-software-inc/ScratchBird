// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// SB-PHYSICAL-MGA-COW-ANCHOR
#include "copy_on_write.hpp"
#include "catalog_record_codec.hpp"
#include "row_data_page.hpp"
#include "row_version.hpp"
#include "runtime_platform.hpp"
#include "transaction_inventory.hpp"
#include "transaction_snapshot.hpp"

#include <string>
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
};

struct NativeCatalogVersionRow {
  scratchbird::core::catalog::CatalogMetadataVersion metadata;
  scratchbird::core::platform::Uuid version_uuid;
  scratchbird::core::platform::Uuid previous_version_uuid;
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

// Path-free mutation fields for an already-owned node device. A storage
// operation cannot select or open a second node through this payload.
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
};

struct PhysicalMgaCowMutationRequest : PhysicalMgaCowMutation {
  std::string database_path;
};

struct PhysicalMgaCowMutationBatch {
  std::vector<PhysicalMgaCowMutation> mutations;
  bool sync_after_batch = true;
  bool engine_generated_unique_insert_rows = false;
};

struct PhysicalMgaCowMutationBatchRequest {
  std::vector<PhysicalMgaCowMutationRequest> mutations;
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

struct PhysicalMgaCowMutationBatchResult {
  Status status;
  DiagnosticRecord diagnostic;
  std::vector<std::string> evidence;
  u64 written_rows = 0;
  u64 pages_written = 0;

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
NativeCatalogVersionReadResult ReadNativeCatalogVersionsFromOpenDevice(
    scratchbird::storage::disk::FileDevice& device, const TypedUuid& relation_uuid,
    u64 page_number, const scratchbird::transaction::mga::VisibilitySnapshot& snapshot,
    bool latest_committed,
    const scratchbird::transaction::mga::TransactionIdentity& reader_identity = {},
    const scratchbird::transaction::mga::PublishedSnapshotPin* snapshot_pin = nullptr);

}  // namespace scratchbird::storage::database

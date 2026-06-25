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
#include "row_data_page.hpp"
#include "row_version.hpp"
#include "runtime_platform.hpp"
#include "transaction_inventory.hpp"

#include <string>
#include <vector>

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
  rollback
};

struct PhysicalMgaCowMutationRequest {
  std::string database_path;
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
};

struct PhysicalMgaCowMutationBatchRequest {
  std::vector<PhysicalMgaCowMutationRequest> mutations;
  bool sync_after_batch = true;
  bool engine_generated_unique_insert_rows = false;
};

struct PhysicalMgaCowFinalizeRequest {
  std::string database_path;
  scratchbird::transaction::mga::LocalTransactionId local_transaction_id;
  PhysicalMgaCowFinalizeDecision decision = PhysicalMgaCowFinalizeDecision::commit;
  u64 final_unix_epoch_millis = 0;
};

struct PhysicalMgaCowReadRequest {
  std::string database_path;
  TypedUuid relation_uuid;
  u64 page_number = 0;
  scratchbird::transaction::mga::VisibilitySnapshot visibility_snapshot;
  bool use_latest_committed_snapshot = true;
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
PhysicalMgaCowFinalizeResult FinalizePhysicalMgaCowTransaction(
    const PhysicalMgaCowFinalizeRequest& request);
PhysicalMgaCowReadResult ReadPhysicalMgaCowRows(
    const PhysicalMgaCowReadRequest& request);

DiagnosticRecord MakePhysicalMgaCowDiagnostic(Status status,
                                              std::string diagnostic_code,
                                              std::string message_key,
                                              std::string detail = {});

}  // namespace scratchbird::storage::database

// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// SB-TXN-INVENTORY-ANCHOR
#include "runtime_platform.hpp"
#include "transaction_state.hpp"

#include <string>
#include <vector>

namespace scratchbird::transaction::mga {

using scratchbird::core::platform::DiagnosticRecord;
using scratchbird::core::platform::Status;
using scratchbird::core::platform::TypedUuid;
using scratchbird::core::platform::u64;

struct TransactionInventoryEntry {
  TransactionIdentity identity;
  TransactionState state = TransactionState::none;
  u64 begin_unix_epoch_millis = 0;
  u64 final_unix_epoch_millis = 0;
  u64 begin_visible_through_local_transaction_id = kInvalidLocalTransactionId;
  bool evidence_record_required = true;
  bool evidence_record_written = false;
  bool rollback_only = false;
};

struct LocalTransactionInventory {
  u64 next_local_transaction_id = 1;
  std::vector<TransactionInventoryEntry> entries;
};

struct TransactionInventoryResult {
  Status status;
  LocalTransactionInventory inventory;
  TransactionInventoryEntry entry;
  DiagnosticRecord diagnostic;

  bool ok() const {
    return status.ok();
  }
};

struct TransactionInventoryLookupResult {
  Status status;
  TransactionInventoryEntry entry;
  DiagnosticRecord diagnostic;

  bool ok() const {
    return status.ok();
  }
};

struct TransactionInventoryCompactionRequest {
  LocalTransactionInventory inventory;
  bool inventory_authoritative = false;
  LocalTransactionId oldest_required_local_transaction_id;
  bool drop_archived_entries = true;
};

struct TransactionInventoryCompactionResult {
  Status status;
  LocalTransactionInventory inventory;
  u64 compacted_entry_count = 0;
  DiagnosticRecord diagnostic;

  bool ok() const {
    return status.ok();
  }
};

LocalTransactionInventory MakeEmptyLocalTransactionInventory();
TransactionInventoryResult BeginLocalTransaction(LocalTransactionInventory inventory,
                                                 TypedUuid transaction_uuid,
                                                 u64 begin_unix_epoch_millis);
TransactionInventoryResult BeginLocalReadOnlyTransaction(LocalTransactionInventory inventory,
                                                         TypedUuid transaction_uuid,
                                                         u64 begin_unix_epoch_millis);
TransactionInventoryResult PrepareLocalTransaction(LocalTransactionInventory inventory,
                                                   LocalTransactionId local_id);
TransactionInventoryResult CommitLocalTransaction(LocalTransactionInventory inventory,
                                                  LocalTransactionId local_id,
                                                  u64 final_unix_epoch_millis);
TransactionInventoryResult ArchiveLocalTransaction(LocalTransactionInventory inventory,
                                                   LocalTransactionId local_id);
TransactionInventoryResult RollbackLocalTransaction(LocalTransactionInventory inventory,
                                                    LocalTransactionId local_id,
                                                    u64 final_unix_epoch_millis);
TransactionInventoryResult AbortLocalTransaction(LocalTransactionInventory inventory,
                                                 LocalTransactionId local_id,
                                                 u64 final_unix_epoch_millis);
TransactionInventoryResult MarkLocalTransactionRollbackOnly(LocalTransactionInventory inventory,
                                                           LocalTransactionId local_id);
TransactionInventoryLookupResult LookupLocalTransaction(const LocalTransactionInventory& inventory,
                                                        LocalTransactionId local_id);
TransactionInventoryCompactionResult CompactLocalTransactionInventory(
    const TransactionInventoryCompactionRequest& request);
DiagnosticRecord MakeTransactionInventoryDiagnostic(Status status,
                                                   std::string diagnostic_code,
                                                   std::string message_key,
                                                   std::string detail = {});

}  // namespace scratchbird::transaction::mga

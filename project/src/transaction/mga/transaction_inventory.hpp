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

#include <array>
#include <optional>
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
  TransactionState archived_from_state = TransactionState::none;
  u64 begin_unix_epoch_millis = 0;
  u64 final_unix_epoch_millis = 0;
  u64 begin_visible_through_local_transaction_id = kInvalidLocalTransactionId;
  u64 begin_visible_through_commit_sequence = 0;
  u64 commit_sequence = 0;
  bool evidence_record_required = true;
  bool evidence_record_written = false;
  bool rollback_only = false;
  // Admitted at BEGIN and immutable until retirement; persisted in inventory flags.
  bool stable_snapshot = false;
};

// Trusted in-process optimistic-concurrency provenance, not durable state or
// authorization. Native loading/publication issues it; pure transforms retain it.
struct TransactionInventoryPublicationBase {
  scratchbird::core::platform::Uuid database_uuid;
  std::array<scratchbird::core::platform::byte, 32> inventory_sha256{};
  u64 generation = 0;
  bool operator==(const TransactionInventoryPublicationBase&) const = default;
};

struct LocalTransactionInventory {
  u64 next_local_transaction_id = 1;
  u64 next_commit_sequence = 1;
  std::vector<TransactionInventoryEntry> entries;
  std::optional<TransactionInventoryPublicationBase> publication_base;
};

// Derived visibility state, never an unqualified archived state. The native
// entry retains its lifecycle state and exact archived origin separately.
inline TransactionState InventoryVisibilityState(const TransactionInventoryEntry& entry) {
  if (entry.state != TransactionState::archived)
    return entry.archived_from_state == TransactionState::none ? entry.state : TransactionState::none;
  switch (entry.archived_from_state) {
    case TransactionState::committed:
    case TransactionState::rolled_back:
    case TransactionState::failed_terminal: return entry.archived_from_state;
    default: return TransactionState::none;
  }
}

inline bool HasCommittedInventoryOutcome(const TransactionInventoryEntry& entry) {
  return InventoryVisibilityState(entry) == TransactionState::committed;
}

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
// Pure candidate transforms, not durable BEGIN or concurrency admission.
// The owning publisher must persist created/starting before activation while
// retaining these exact identities, allocation and commit-order boundaries.
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

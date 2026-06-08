// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "transaction_inventory.hpp"

#include <utility>
#include <vector>

namespace scratchbird::transaction::mga {
namespace {

using scratchbird::core::platform::DiagnosticArgument;
using scratchbird::core::platform::MakeDiagnostic;
using scratchbird::core::platform::Severity;
using scratchbird::core::platform::StatusCode;
using scratchbird::core::platform::Subsystem;

Status InventoryOkStatus() {
  return {StatusCode::ok, Severity::info, Subsystem::transaction_mga};
}

Status InventoryErrorStatus() {
  return {StatusCode::platform_required_feature_missing, Severity::error, Subsystem::transaction_mga};
}

TransactionInventoryResult InventoryError(std::string diagnostic_code,
                                          std::string message_key,
                                          std::string detail = {}) {
  TransactionInventoryResult result;
  result.status = InventoryErrorStatus();
  result.diagnostic = MakeTransactionInventoryDiagnostic(result.status,
                                                        std::move(diagnostic_code),
                                                        std::move(message_key),
                                                        std::move(detail));
  return result;
}

TransactionInventoryLookupResult LookupError(std::string diagnostic_code,
                                             std::string message_key,
                                             std::string detail = {}) {
  TransactionInventoryLookupResult result;
  result.status = InventoryErrorStatus();
  result.diagnostic = MakeTransactionInventoryDiagnostic(result.status,
                                                        std::move(diagnostic_code),
                                                        std::move(message_key),
                                                        std::move(detail));
  return result;
}

TransactionInventoryCompactionResult CompactionError(std::string diagnostic_code,
                                                     std::string message_key,
                                                     std::string detail = {}) {
  TransactionInventoryCompactionResult result;
  result.status = InventoryErrorStatus();
  result.diagnostic = MakeTransactionInventoryDiagnostic(result.status,
                                                        std::move(diagnostic_code),
                                                        std::move(message_key),
                                                        std::move(detail));
  return result;
}

TransactionInventoryEntry* FindEntry(LocalTransactionInventory* inventory, LocalTransactionId local_id) {
  for (TransactionInventoryEntry& entry : inventory->entries) {
    if (entry.identity.local_id.value == local_id.value) {
      return &entry;
    }
  }
  return nullptr;
}

bool ContainsTransactionUuid(const LocalTransactionInventory& inventory, const TypedUuid& transaction_uuid) {
  for (const TransactionInventoryEntry& entry : inventory.entries) {
    if (entry.identity.transaction_uuid.value == transaction_uuid.value) {
      return true;
    }
  }
  return false;
}

u64 LatestCommittedLocalTransactionId(const LocalTransactionInventory& inventory) {
  u64 latest = kInvalidLocalTransactionId;
  for (const TransactionInventoryEntry& entry : inventory.entries) {
    if ((entry.state == TransactionState::committed ||
         entry.state == TransactionState::archived) &&
        entry.identity.local_id.valid() &&
        entry.identity.local_id.value > latest) {
      latest = entry.identity.local_id.value;
    }
  }
  return latest;
}

bool TransitionOrFail(TransactionInventoryEntry* entry, TransactionState next, DiagnosticRecord* diagnostic) {
  const auto transition = CheckTransactionStateTransition(entry->state, next, false);
  if (!transition.ok()) {
    *diagnostic = transition.diagnostic;
    return false;
  }
  entry->state = next;
  return true;
}

}  // namespace

LocalTransactionInventory MakeEmptyLocalTransactionInventory() {
  LocalTransactionInventory inventory;
  inventory.next_local_transaction_id = 1;
  return inventory;
}

TransactionInventoryResult BeginLocalTransactionWithState(LocalTransactionInventory inventory,
                                                          TypedUuid transaction_uuid,
                                                          u64 begin_unix_epoch_millis,
                                                          TransactionState active_state) {
  if (inventory.next_local_transaction_id == kInvalidLocalTransactionId) {
    return InventoryError("SB-TXN-INVENTORY-NEXT-ID-INVALID",
                          "transaction.inventory.next_id_invalid");
  }

  const LocalTransactionId local_id = MakeLocalTransactionId(inventory.next_local_transaction_id);
  if (ContainsTransactionUuid(inventory, transaction_uuid)) {
    return InventoryError("SB-TXN-DUPLICATE-TRANSACTION-UUID",
                          "transaction.inventory.duplicate_transaction_uuid");
  }
  const auto identity = MakeTransactionIdentity(local_id, transaction_uuid, TransactionScope::local_node);
  if (!identity.ok()) {
    TransactionInventoryResult result;
    result.status = identity.status;
    result.diagnostic = identity.diagnostic;
    return result;
  }

  TransactionInventoryEntry entry;
  entry.identity = identity.identity;
  entry.state = TransactionState::none;
  entry.begin_unix_epoch_millis = begin_unix_epoch_millis;
  entry.begin_visible_through_local_transaction_id =
      LatestCommittedLocalTransactionId(inventory);

  DiagnosticRecord diagnostic;
  if (!TransitionOrFail(&entry, TransactionState::created, &diagnostic) ||
      !TransitionOrFail(&entry, active_state, &diagnostic)) {
    TransactionInventoryResult result;
    result.status = InventoryErrorStatus();
    result.diagnostic = diagnostic;
    return result;
  }

  inventory.next_local_transaction_id += 1;
  inventory.entries.push_back(entry);

  TransactionInventoryResult result;
  result.status = InventoryOkStatus();
  result.inventory = std::move(inventory);
  result.entry = entry;
  return result;
}

TransactionInventoryResult BeginLocalTransaction(LocalTransactionInventory inventory,
                                                 TypedUuid transaction_uuid,
                                                 u64 begin_unix_epoch_millis) {
  return BeginLocalTransactionWithState(std::move(inventory),
                                        transaction_uuid,
                                        begin_unix_epoch_millis,
                                        TransactionState::active);
}

TransactionInventoryResult BeginLocalReadOnlyTransaction(LocalTransactionInventory inventory,
                                                         TypedUuid transaction_uuid,
                                                         u64 begin_unix_epoch_millis) {
  return BeginLocalTransactionWithState(std::move(inventory),
                                        transaction_uuid,
                                        begin_unix_epoch_millis,
                                        TransactionState::read_only_active);
}

TransactionInventoryResult PrepareLocalTransaction(LocalTransactionInventory inventory,
                                                   LocalTransactionId local_id) {
  if (!local_id.valid()) {
    return InventoryError("SB-TXN-LOCAL-ID-NOT-FOUND",
                          "transaction.inventory.prepare_id_invalid");
  }
  TransactionInventoryEntry* entry = FindEntry(&inventory, local_id);
  if (entry == nullptr) {
    return InventoryError("SB-TXN-LOCAL-ID-NOT-FOUND",
                          "transaction.inventory.prepare_id_not_found",
                          std::to_string(local_id.value));
  }
  DiagnosticRecord diagnostic;
  if (!TransitionOrFail(entry, TransactionState::preparing, &diagnostic) ||
      !TransitionOrFail(entry, TransactionState::prepared, &diagnostic)) {
    TransactionInventoryResult result;
    result.status = InventoryErrorStatus();
    result.diagnostic = diagnostic;
    return result;
  }
  const TransactionInventoryEntry prepared_entry = *entry;
  TransactionInventoryResult result;
  result.status = InventoryOkStatus();
  result.inventory = std::move(inventory);
  result.entry = prepared_entry;
  return result;
}

TransactionInventoryResult CommitLocalTransaction(LocalTransactionInventory inventory,
                                                  LocalTransactionId local_id,
                                                  u64 final_unix_epoch_millis) {
  if (!local_id.valid()) {
    return InventoryError("SB-TXN-LOCAL-ID-NOT-FOUND",
                          "transaction.inventory.commit_id_invalid");
  }

  TransactionInventoryEntry* entry = FindEntry(&inventory, local_id);
  if (entry == nullptr) {
    return InventoryError("SB-TXN-LOCAL-ID-NOT-FOUND",
                          "transaction.inventory.commit_id_not_found",
                          std::to_string(local_id.value));
  }
  if (entry->rollback_only) {
    return InventoryError("SB-MGA-ROLLBACK-ONLY-COMMIT-REFUSED",
                          "transaction.inventory.rollback_only_commit_refused",
                          std::to_string(local_id.value));
  }

  DiagnosticRecord diagnostic;
  if ((entry->state == TransactionState::active &&
       (!TransitionOrFail(entry, TransactionState::preparing, &diagnostic) ||
        !TransitionOrFail(entry, TransactionState::prepared, &diagnostic))) ||
      !TransitionOrFail(entry, TransactionState::committing, &diagnostic) ||
      !TransitionOrFail(entry, TransactionState::committed, &diagnostic)) {
    TransactionInventoryResult result;
    result.status = InventoryErrorStatus();
    result.diagnostic = diagnostic;
    return result;
  }

  entry->final_unix_epoch_millis = final_unix_epoch_millis;
  entry->evidence_record_written = true;
  const TransactionInventoryEntry committed_entry = *entry;

  TransactionInventoryResult result;
  result.status = InventoryOkStatus();
  result.inventory = std::move(inventory);
  result.entry = committed_entry;
  return result;
}

TransactionInventoryResult ArchiveLocalTransaction(LocalTransactionInventory inventory,
                                                   LocalTransactionId local_id) {
  if (!local_id.valid()) {
    return InventoryError("SB-TXN-LOCAL-ID-NOT-FOUND",
                          "transaction.inventory.archive_id_invalid");
  }

  TransactionInventoryEntry* entry = FindEntry(&inventory, local_id);
  if (entry == nullptr) {
    return InventoryError("SB-TXN-LOCAL-ID-NOT-FOUND",
                          "transaction.inventory.archive_id_not_found",
                          std::to_string(local_id.value));
  }
  if (!entry->evidence_record_written) {
    return InventoryError("SB-MGA-ARCHIVE-FINALITY-EVIDENCE-REQUIRED",
                          "transaction.inventory.archive_finality_evidence_required",
                          std::to_string(local_id.value));
  }

  DiagnosticRecord diagnostic;
  if (!TransitionOrFail(entry, TransactionState::archived, &diagnostic)) {
    TransactionInventoryResult result;
    result.status = InventoryErrorStatus();
    result.diagnostic = diagnostic;
    return result;
  }

  const TransactionInventoryEntry archived_entry = *entry;
  TransactionInventoryResult result;
  result.status = InventoryOkStatus();
  result.inventory = std::move(inventory);
  result.entry = archived_entry;
  return result;
}

TransactionInventoryResult RollbackLocalTransaction(LocalTransactionInventory inventory,
                                                    LocalTransactionId local_id,
                                                    u64 final_unix_epoch_millis) {
  if (!local_id.valid()) {
    return InventoryError("SB-TXN-LOCAL-ID-NOT-FOUND",
                          "transaction.inventory.rollback_id_invalid");
  }

  TransactionInventoryEntry* entry = FindEntry(&inventory, local_id);
  if (entry == nullptr) {
    return InventoryError("SB-TXN-LOCAL-ID-NOT-FOUND",
                          "transaction.inventory.rollback_id_not_found",
                          std::to_string(local_id.value));
  }

  DiagnosticRecord diagnostic;
  if (!TransitionOrFail(entry, TransactionState::rolling_back, &diagnostic) ||
      !TransitionOrFail(entry, TransactionState::rolled_back, &diagnostic)) {
    TransactionInventoryResult result;
    result.status = InventoryErrorStatus();
    result.diagnostic = diagnostic;
    return result;
  }

  entry->final_unix_epoch_millis = final_unix_epoch_millis;
  entry->evidence_record_written = true;
  const TransactionInventoryEntry rolled_back_entry = *entry;

  TransactionInventoryResult result;
  result.status = InventoryOkStatus();
  result.inventory = std::move(inventory);
  result.entry = rolled_back_entry;
  return result;
}

TransactionInventoryResult AbortLocalTransaction(LocalTransactionInventory inventory,
                                                 LocalTransactionId local_id,
                                                 u64 final_unix_epoch_millis) {
  return RollbackLocalTransaction(std::move(inventory), local_id, final_unix_epoch_millis);
}

TransactionInventoryResult MarkLocalTransactionRollbackOnly(LocalTransactionInventory inventory,
                                                           LocalTransactionId local_id) {
  if (!local_id.valid()) {
    return InventoryError("SB-TXN-LOCAL-ID-NOT-FOUND",
                          "transaction.inventory.rollback_only_id_invalid");
  }

  TransactionInventoryEntry* entry = FindEntry(&inventory, local_id);
  if (entry == nullptr) {
    return InventoryError("SB-TXN-LOCAL-ID-NOT-FOUND",
                          "transaction.inventory.rollback_only_id_not_found",
                          std::to_string(local_id.value));
  }
  if (entry->state == TransactionState::committed || entry->state == TransactionState::archived) {
    return InventoryError("SB-MGA-ROLLBACK-ONLY-MARK-REFUSED",
                          "transaction.inventory.rollback_only_mark_refused",
                          std::to_string(local_id.value));
  }

  entry->rollback_only = true;
  const TransactionInventoryEntry rollback_only_entry = *entry;
  TransactionInventoryResult result;
  result.status = InventoryOkStatus();
  result.inventory = std::move(inventory);
  result.entry = rollback_only_entry;
  return result;
}

TransactionInventoryLookupResult LookupLocalTransaction(const LocalTransactionInventory& inventory,
                                                        LocalTransactionId local_id) {
  if (!local_id.valid()) {
    return LookupError("SB-TXN-LOCAL-ID-NOT-FOUND",
                       "transaction.inventory.lookup_id_invalid");
  }

  for (const TransactionInventoryEntry& entry : inventory.entries) {
    if (entry.identity.local_id.value == local_id.value) {
      TransactionInventoryLookupResult result;
      result.status = InventoryOkStatus();
      result.entry = entry;
      return result;
    }
  }

  return LookupError("SB-TXN-LOCAL-ID-NOT-FOUND",
                     "transaction.inventory.lookup_id_not_found",
                     std::to_string(local_id.value));
}

TransactionInventoryCompactionResult CompactLocalTransactionInventory(
    const TransactionInventoryCompactionRequest& request) {
  if (!request.inventory_authoritative) {
    return CompactionError("SB-TXN-INVENTORY-COMPACTION-NOT-AUTHORITATIVE",
                           "transaction.inventory.compaction_not_authoritative");
  }
  if (!request.oldest_required_local_transaction_id.valid()) {
    return CompactionError("SB-TXN-INVENTORY-COMPACTION-HORIZON-INVALID",
                           "transaction.inventory.compaction_horizon_invalid");
  }
  TransactionInventoryCompactionResult result;
  result.status = InventoryOkStatus();
  result.inventory.next_local_transaction_id = request.inventory.next_local_transaction_id;
  for (const auto& entry : request.inventory.entries) {
    const bool can_drop = request.drop_archived_entries &&
                          entry.state == TransactionState::archived &&
                          entry.identity.local_id.valid() &&
                          entry.identity.local_id.value < request.oldest_required_local_transaction_id.value;
    if (can_drop) {
      ++result.compacted_entry_count;
      continue;
    }
    result.inventory.entries.push_back(entry);
  }
  return result;
}

DiagnosticRecord MakeTransactionInventoryDiagnostic(Status status,
                                                   std::string diagnostic_code,
                                                   std::string message_key,
                                                   std::string detail) {
  std::vector<DiagnosticArgument> arguments;
  if (!detail.empty()) {
    arguments.push_back({"detail", detail});
  }

  return MakeDiagnostic(status.code,
                        status.severity,
                        status.subsystem,
                        std::move(diagnostic_code),
                        std::move(message_key),
                        std::move(arguments),
                        {},
                        "transaction.mga.inventory");
}

}  // namespace scratchbird::transaction::mga

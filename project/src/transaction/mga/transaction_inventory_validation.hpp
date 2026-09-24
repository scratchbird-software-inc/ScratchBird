// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "transaction_inventory.hpp"
#include "uuid.hpp"

#include <array>
#include <map>
#include <set>

namespace scratchbird::transaction::mga {

// Structural admission only: never establishes durable inventory authority,
// transaction finality, cluster authorization or a live snapshot horizon.
// Empty means valid. Reasons are internal detail keys, not diagnostic codes.
inline const char* ValidateLocalTransactionInventoryStructure(
    const LocalTransactionInventory& inventory) {
  if (inventory.next_local_transaction_id == kInvalidLocalTransactionId)
    return "next_transaction_invalid";
  if (inventory.next_commit_sequence == 0) return "next_commit_sequence_invalid";
  std::set<u64> commit_sequences;
  std::set<u64> local_ids;
  using BinaryIdentity = std::array<scratchbird::core::platform::byte, 16>;
  static_assert(sizeof(BinaryIdentity) == 16);
  std::set<BinaryIdentity> transaction_ids;
  for (const auto& entry : inventory.entries) {
    if (entry.begin_visible_through_commit_sequence >= inventory.next_commit_sequence)
      return "begin_commit_sequence_invalid";
    if (entry.state == TransactionState::archived) {
      if (entry.archived_from_state != TransactionState::committed &&
          entry.archived_from_state != TransactionState::rolled_back &&
          entry.archived_from_state != TransactionState::failed_terminal)
        return "archive_origin_invalid";
    } else if (entry.archived_from_state != TransactionState::none) return "nonarchived_origin";
    const bool committed = HasCommittedInventoryOutcome(entry);
    if (committed) {
      if (entry.commit_sequence == 0 || entry.commit_sequence >= inventory.next_commit_sequence ||
          entry.commit_sequence <= entry.begin_visible_through_commit_sequence)
        return "commit_sequence_invalid";
      if (!commit_sequences.insert(entry.commit_sequence).second) return "duplicate_commit_sequence";
    } else if (entry.commit_sequence != 0) return "noncommitted_commit_sequence";
    if (!entry.identity.valid() ||
        !scratchbird::core::uuid::IsEngineIdentityUuid(entry.identity.transaction_uuid.value))
      return "invalid_transaction_identity";
    switch (entry.identity.scope) {
      case TransactionScope::local_node:
      case TransactionScope::cluster_global:
        break;
      default:
        return "invalid_transaction_scope";
    }
    if (entry.identity.local_id.value >= inventory.next_local_transaction_id)
      return "future_transaction_in_inventory";
    if (!local_ids.insert(entry.identity.local_id.value).second)
      return "duplicate_local_transaction_id";
    if (!transaction_ids.insert(entry.identity.transaction_uuid.value.bytes).second)
      return "duplicate_transaction_uuid";
    switch (entry.state) {
      case TransactionState::created:
      case TransactionState::active:
      case TransactionState::preparing:
      case TransactionState::prepared:
      case TransactionState::committing:
      case TransactionState::committed:
      case TransactionState::rolling_back:
      case TransactionState::rolled_back:
      case TransactionState::limbo:
      case TransactionState::recovering:
      case TransactionState::failed_terminal:
      case TransactionState::archived:
      case TransactionState::read_only_active:
        break;
      default:
        return "invalid_transaction_state";
    }
  }
  return "";
}

// Successor consistency only. Callers still own transition authorization,
// retention release, recovery authority and publication/CAS synchronization.
inline const char* ValidateLocalTransactionInventoryEvolution(
    const LocalTransactionInventory& before, const LocalTransactionInventory& after) {
  if (const auto* why = ValidateLocalTransactionInventoryStructure(before); *why) return why;
  if (const auto* why = ValidateLocalTransactionInventoryStructure(after); *why) return why;
  if (after.next_local_transaction_id < before.next_local_transaction_id ||
      after.next_commit_sequence < before.next_commit_sequence) return "counter_regression";
  constexpr auto state_count = static_cast<std::size_t>(TransactionState::read_only_active) + 1;
  static const auto reachable = [] {
    std::array<std::array<bool, state_count>, state_count> paths{};
    for (std::size_t i = 0; i < state_count; ++i) paths[i][i] = true;
    for (const auto& edge : BuiltinTransactionStateTransitions())
      paths[static_cast<std::size_t>(edge.from)][static_cast<std::size_t>(edge.to)] = true;
    for (std::size_t k = 0; k < state_count; ++k)
      for (std::size_t i = 0; i < state_count; ++i)
        for (std::size_t j = 0; j < state_count; ++j)
          paths[i][j] = paths[i][j] || (paths[i][k] && paths[k][j]);
    return paths;
  }();
  std::map<u64, const TransactionInventoryEntry*> old_entries;
  std::map<scratchbird::core::platform::Uuid, u64> old_uuids;
  for (const auto& entry : before.entries) {
    old_entries.emplace(entry.identity.local_id.value, &entry);
    old_uuids.emplace(entry.identity.transaction_uuid.value, entry.identity.local_id.value);
  }
  for (const auto& entry : after.entries) {
    const auto found = old_entries.find(entry.identity.local_id.value);
    if (found == old_entries.end()) {
      if (entry.identity.local_id.value < before.next_local_transaction_id) return "local_number_reused";
      if (old_uuids.contains(entry.identity.transaction_uuid.value)) return "transaction_uuid_reused";
      if (HasCommittedInventoryOutcome(entry) && entry.commit_sequence < before.next_commit_sequence)
        return "commit_sequence_reused";
      continue;
    }
    const auto& old = *found->second;
    if (old.identity.transaction_uuid.value != entry.identity.transaction_uuid.value ||
        old.identity.scope != entry.identity.scope) return "transaction_identity_changed";
    if (old.begin_unix_epoch_millis != entry.begin_unix_epoch_millis ||
        old.begin_visible_through_local_transaction_id != entry.begin_visible_through_local_transaction_id ||
        old.begin_visible_through_commit_sequence != entry.begin_visible_through_commit_sequence ||
        old.stable_snapshot != entry.stable_snapshot)
      return "transaction_begin_changed";
    if ((old.evidence_record_required && !entry.evidence_record_required) ||
        (old.evidence_record_written && !entry.evidence_record_written)) return "transaction_evidence_regressed";
    const auto old_outcome = InventoryVisibilityState(old);
    const auto new_outcome = InventoryVisibilityState(entry);
    if (IsTerminalTransactionState(old_outcome) &&
        (old_outcome != new_outcome || old.final_unix_epoch_millis != entry.final_unix_epoch_millis ||
         old.commit_sequence != entry.commit_sequence)) return "transaction_finality_changed";
    if (!reachable[static_cast<std::size_t>(old.state)][static_cast<std::size_t>(entry.state)] ||
        (entry.state == TransactionState::archived && old.state != TransactionState::archived &&
         !reachable[static_cast<std::size_t>(old.state)][static_cast<std::size_t>(new_outcome)]))
      return "transaction_state_regressed";
    if (!HasCommittedInventoryOutcome(old) && HasCommittedInventoryOutcome(entry) &&
        entry.commit_sequence < before.next_commit_sequence) return "commit_sequence_reused";
    old_entries.erase(found);
  }
  for (const auto& [local_id, entry] : old_entries) {
    const auto outcome = InventoryVisibilityState(*entry);
    if (outcome != TransactionState::committed && outcome != TransactionState::rolled_back)
      return "unresolved_transaction_removed";
  }
  return "";
}

}  // namespace scratchbird::transaction::mga

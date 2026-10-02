// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "transaction_horizon.hpp"
#include <algorithm>
#include <span>
namespace scratchbird::transaction::mga::detail {
inline bool IsInteresting(TransactionState state) {
  return state == TransactionState::created || state == TransactionState::active || state == TransactionState::read_only_active ||
         state == TransactionState::preparing || state == TransactionState::prepared || state == TransactionState::committing ||
         state == TransactionState::rolling_back || state == TransactionState::limbo || state == TransactionState::recovering ||
         state == TransactionState::failed_terminal;
}

inline bool IsActiveForOat(TransactionState state) {
  return state == TransactionState::active || state == TransactionState::read_only_active;
}

// Internal pure projection. The caller must first validate complete structure.
// No retention, finality, live snapshot, or cleanup authority is produced.
template<class Inventory> const char* ProjectValidatedLocalHorizons(
    const Inventory& inventory,std::span<const LocalTransactionId> snapshot_horizons,
    LocalTransactionHorizons& out) noexcept {
  u64 oit = inventory.next_local_transaction_id;
  u64 oat = inventory.next_local_transaction_id;
  u64 ost = inventory.next_local_transaction_id;

  for (const TransactionInventoryEntry& entry : inventory.entries) {
    if (IsInteresting(InventoryVisibilityState(entry))) {
      oit = std::min(oit, entry.identity.local_id.value);
    }
    if (IsActiveForOat(entry.state)) {
      oat = std::min(oat, entry.identity.local_id.value);
    }
  }

  // Without a retained snapshot, OST follows OAT, not the next transaction
  // counter. Otherwise an active reader can disappear from snapshot age.
  if (snapshot_horizons.empty()) ost = oat;
  for (const LocalTransactionId& snapshot_horizon : snapshot_horizons) {
    if (!snapshot_horizon.valid())return "transaction.horizon.invalid_snapshot_horizon";
    if (snapshot_horizon.value>inventory.next_local_transaction_id)return "transaction.horizon.future_snapshot_horizon";
    ost = std::min(ost, snapshot_horizon.value);
  }

  // Stable readers retain versions hidden by commits after their BEGIN,
  // including writers with lower local numbers. This applies between
  // statements as well as while a published statement snapshot is pinned.
  u64 stable_commit_boundary = inventory.next_commit_sequence;
  for (const auto& entry : inventory.entries)
    if (entry.stable_snapshot && IsActiveForOat(entry.state))
      stable_commit_boundary = std::min(stable_commit_boundary,
                                       entry.begin_visible_through_commit_sequence);
  if (stable_commit_boundary != inventory.next_commit_sequence) {
    ost = std::min(ost, oit);
    for (const auto& entry : inventory.entries)
      if (HasCommittedInventoryOutcome(entry) &&
          entry.commit_sequence > stable_commit_boundary)
        ost = std::min(ost, entry.identity.local_id.value);
  }

  out.next_transaction_id = MakeLocalTransactionId(inventory.next_local_transaction_id);
  out.oldest_interesting_transaction = MakeLocalTransactionId(oit);
  out.oldest_active_transaction = MakeLocalTransactionId(oat);
  out.oldest_snapshot_transaction = MakeLocalTransactionId(ost);
  out.valid = true;
  return nullptr;
}
} // namespace scratchbird::transaction::mga::detail

// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "transaction_inventory.hpp"
#include "uuid.hpp"

#include <array>
#include <set>

namespace scratchbird::transaction::mga {

// Structural admission only: never establishes durable inventory authority,
// transaction finality, cluster authorization or a live snapshot horizon.
// Empty means valid. Reasons are internal detail keys, not diagnostic codes.
inline const char* ValidateLocalTransactionInventoryStructure(
    const LocalTransactionInventory& inventory) {
  if (inventory.next_local_transaction_id == kInvalidLocalTransactionId)
    return "next_transaction_invalid";
  std::set<u64> local_ids;
  using BinaryIdentity = std::array<scratchbird::core::platform::byte, 16>;
  static_assert(sizeof(BinaryIdentity) == 16);
  std::set<BinaryIdentity> transaction_ids;
  for (const auto& entry : inventory.entries) {
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

}  // namespace scratchbird::transaction::mga

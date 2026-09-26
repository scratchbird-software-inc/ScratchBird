// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "../../src/transaction/mga/transaction_inventory.hpp"

#include <stdexcept>
#include <utility>
#include <vector>

namespace scratchbird::tests {

// Construct input to pure inventory/cleanup component tests. The actual
// transition API issues commit order; this helper does not establish durable
// inventory authority, publish a transaction, or provide SQL/IPC evidence.
inline transaction::mga::LocalTransactionInventory CommitInventoryModelFixture(
    transaction::mga::LocalTransactionInventory inventory) {
  namespace mga = transaction::mga;
  using Finalization = std::pair<mga::LocalTransactionId, core::platform::u64>;
  std::vector<Finalization> commits;
  if (inventory.next_commit_sequence != 1 || inventory.publication_base)
    throw std::runtime_error("inventory model fixture requires fresh unissued commit order");
  for (auto& entry : inventory.entries) {
    if (entry.commit_sequence != 0 || entry.begin_visible_through_commit_sequence != 0)
      throw std::runtime_error("inventory model fixture cannot rewrite issued commit order");
    if (entry.state != mga::TransactionState::committed) continue;
    commits.emplace_back(entry.identity.local_id, entry.final_unix_epoch_millis);
    entry.state = mga::TransactionState::active;
    entry.final_unix_epoch_millis = 0;
    entry.evidence_record_written = false;
  }
  for (const auto& [local_id, final_time] : commits) {
    auto committed = mga::CommitLocalTransaction(std::move(inventory), local_id, final_time);
    if (!committed.ok())
      throw std::runtime_error("inventory model commit failed: " + committed.diagnostic.message_key);
    inventory = std::move(committed.inventory);
  }
  return inventory;
}

}  // namespace scratchbird::tests

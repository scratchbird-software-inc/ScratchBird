// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
// Read-only test observer. It neither opens an attachment nor starts a txn.
#include "local_transaction_store.hpp"

#include <algorithm>
#include <iostream>

int main(int argc, char** argv) {
  if (argc != 2) return 2;
  auto loaded = scratchbird::storage::database::
      LoadLocalTransactionInventoryFromDatabase(argv[1]);
  if (!loaded.ok()) {
    std::cerr << "durable inventory load failed\n";
    return 1;
  }
  auto& entries = loaded.inventory.entries;
  std::sort(entries.begin(), entries.end(), [](const auto& a, const auto& b) {
    return a.identity.local_id.value < b.identity.local_id.value;
  });
  std::cout << "next_local_transaction_id\t"
            << loaded.inventory.next_local_transaction_id << '\n';
  for (const auto& entry : entries) {
    if (!entry.identity.valid()) return 1;
    // Retain the full inventory, including agent work. The differential
    // harness correlates observed INSERT transaction IDs with these records;
    // asynchronous agent work can change the intervening local ordinals.
    // Fresh UUID identities and timestamps are not equality criteria.
    std::cout << entry.identity.local_id.value << '\t'
              << scratchbird::transaction::mga::TransactionStateName(entry.state) << '\t'
              << static_cast<unsigned>(entry.identity.scope) << '\t'
              << entry.begin_visible_through_local_transaction_id << '\t'
              << entry.evidence_record_required << '\t'
              << entry.evidence_record_written << '\t'
              << entry.rollback_only << '\n';
  }
  return 0;
}

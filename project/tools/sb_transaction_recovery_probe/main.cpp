// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// SB-SNTXN-FAULT-PROBE-ANCHOR
#include "transaction_recovery.hpp"
#include "uuid.hpp"

#include <iostream>

namespace {

using scratchbird::core::platform::TypedUuid;
using scratchbird::core::platform::UuidKind;
using scratchbird::core::platform::u64;
using scratchbird::core::uuid::GenerateEngineIdentityV7;
using namespace scratchbird::transaction::mga;

TypedUuid GenerateTxn(u64 seed) {
  const auto generated = GenerateEngineIdentityV7(UuidKind::transaction, seed);
  return generated.ok() ? generated.value : TypedUuid{};
}

TransactionInventoryEntry Entry(u64 id, TransactionState state, bool force_missing_evidence = false) {
  TransactionInventoryEntry entry;
  entry.identity.local_id = MakeLocalTransactionId(id);
  entry.identity.transaction_uuid = GenerateTxn(1714500000000ull + id);
  entry.identity.scope = TransactionScope::local_node;
  entry.state = state;
  entry.evidence_record_required = true;
  entry.evidence_record_written = !force_missing_evidence &&
                                  (state == TransactionState::committed || state == TransactionState::rolled_back ||
                                   state == TransactionState::prepared || state == TransactionState::committing);
  return entry;
}

}  // namespace

int main() {
  LocalTransactionInventory inventory;
  inventory.next_local_transaction_id = 9;
  inventory.entries.push_back(Entry(1, TransactionState::active));
  inventory.entries.push_back(Entry(2, TransactionState::prepared));
  inventory.entries.push_back(Entry(3, TransactionState::committing));
  inventory.entries.push_back(Entry(4, TransactionState::rolling_back));
  inventory.entries.push_back(Entry(5, TransactionState::committed));
  inventory.entries.push_back(Entry(6, TransactionState::rolled_back));
  inventory.entries.push_back(Entry(7, TransactionState::limbo));
  inventory.entries.push_back(Entry(8, TransactionState::committing, true));

  const auto recovery = ClassifyLocalTransactionInventoryForRecovery(inventory);
  if (!recovery.ok()) {
    std::cerr << recovery.diagnostic.diagnostic_code << ":" << recovery.diagnostic.message_key << "\n";
    return 1;
  }

  u64 complete_rollback = 0;
  u64 complete_commit = 0;
  u64 prepared_waiting = 0;
  u64 no_action = 0;
  u64 fail_closed = 0;
  for (const auto& classification : recovery.classifications) {
    if (classification.action == TransactionRecoveryAction::complete_rollback) { ++complete_rollback; }
    if (classification.action == TransactionRecoveryAction::complete_commit) { ++complete_commit; }
    if (classification.action == TransactionRecoveryAction::prepared_waiting_local_decision) { ++prepared_waiting; }
    if (classification.action == TransactionRecoveryAction::no_action) { ++no_action; }
    if (classification.fail_closed) { ++fail_closed; }
  }
  const bool ok = complete_rollback == 2 && complete_commit == 1 && prepared_waiting == 1 && no_action == 2 && fail_closed == 3;
  std::cout << "{\n";
  std::cout << "  \"ok\": " << (ok ? "true" : "false") << ",\n";
  std::cout << "  \"classifications\": " << recovery.classifications.size() << ",\n";
  std::cout << "  \"complete_rollback\": " << complete_rollback << ",\n";
  std::cout << "  \"complete_commit\": " << complete_commit << ",\n";
  std::cout << "  \"prepared_waiting\": " << prepared_waiting << ",\n";
  std::cout << "  \"no_action\": " << no_action << ",\n";
  std::cout << "  \"fail_closed\": " << fail_closed << "\n";
  std::cout << "}\n";
  return ok ? 0 : 1;
}

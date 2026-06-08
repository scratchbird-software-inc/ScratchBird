// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "transaction_prepare.hpp"

#include <utility>

namespace scratchbird::transaction::mga {

TransactionInventoryResult PrepareLocalTransactionDurable(LocalTransactionInventory inventory,
                                                          LocalTransactionId local_id,
                                                          LocalPreparePolicy policy) {
  if (!policy.local_prepare_enabled) {
    return RollbackLocalTransaction(std::move(inventory), local_id, 0);
  }
  TransactionInventoryResult prepared = PrepareLocalTransaction(std::move(inventory), local_id);
  if (prepared.ok() && policy.require_evidence_before_prepared_success) {
    for (TransactionInventoryEntry& entry : prepared.inventory.entries) {
      if (entry.identity.local_id.value == local_id.value) {
        entry.evidence_record_required = true;
        entry.evidence_record_written = true;
        prepared.entry = entry;
        break;
      }
    }
  }
  return prepared;
}

TransactionInventoryResult CompletePreparedLocalTransactionCommit(LocalTransactionInventory inventory,
                                                                  LocalTransactionId local_id,
                                                                  u64 final_unix_epoch_millis) {
  return CommitLocalTransaction(std::move(inventory), local_id, final_unix_epoch_millis);
}

TransactionInventoryResult CompletePreparedLocalTransactionRollback(LocalTransactionInventory inventory,
                                                                    LocalTransactionId local_id,
                                                                    u64 final_unix_epoch_millis) {
  return RollbackLocalTransaction(std::move(inventory), local_id, final_unix_epoch_millis);
}

}  // namespace scratchbird::transaction::mga

// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// SB-SNTXN-PREPARE-ANCHOR
#include "transaction_inventory.hpp"

namespace scratchbird::transaction::mga {

struct LocalPreparePolicy {
  bool local_prepare_enabled = true;
  bool require_evidence_before_prepared_success = true;
};

TransactionInventoryResult PrepareLocalTransactionDurable(LocalTransactionInventory inventory,
                                                          LocalTransactionId local_id,
                                                          LocalPreparePolicy policy = {});
TransactionInventoryResult CompletePreparedLocalTransactionCommit(LocalTransactionInventory inventory,
                                                                  LocalTransactionId local_id,
                                                                  u64 final_unix_epoch_millis);
TransactionInventoryResult CompletePreparedLocalTransactionRollback(LocalTransactionInventory inventory,
                                                                    LocalTransactionId local_id,
                                                                    u64 final_unix_epoch_millis);

}  // namespace scratchbird::transaction::mga

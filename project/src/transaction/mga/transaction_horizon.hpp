// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// SB-TXN-HORIZON-ANCHOR
#include "transaction_inventory.hpp"

#include <vector>

namespace scratchbird::transaction::mga {

struct LocalTransactionHorizons {
  LocalTransactionId oldest_interesting_transaction;
  LocalTransactionId oldest_active_transaction;
  LocalTransactionId oldest_snapshot_transaction;
  LocalTransactionId next_transaction_id;
  bool valid = false;
};

struct LocalTransactionHorizonRequest {
  LocalTransactionInventory inventory;
  std::vector<LocalTransactionId> active_snapshot_horizons;
};

struct TransactionHorizonResult {
  Status status;
  LocalTransactionHorizons horizons;
  DiagnosticRecord diagnostic;

  bool ok() const {
    return status.ok() && horizons.valid;
  }
};

TransactionHorizonResult ComputeLocalTransactionHorizons(const LocalTransactionInventory& inventory);
TransactionHorizonResult ComputeLocalTransactionHorizons(const LocalTransactionHorizonRequest& request);
void PublishTransactionHorizonMetrics(const LocalTransactionInventory& inventory,
                                      const LocalTransactionHorizons& horizons,
                                      u64 observation_unix_epoch_millis);
DiagnosticRecord MakeTransactionHorizonDiagnostic(Status status,
                                                  std::string diagnostic_code,
                                                  std::string message_key,
                                                  std::string detail = {});

}  // namespace scratchbird::transaction::mga

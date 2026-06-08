// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// SB-TXN-MANAGER-ANCHOR
#include "memory.hpp"
#include "transaction_horizon.hpp"
#include "transaction_metrics.hpp"
#include "transaction_snapshot.hpp"

namespace scratchbird::transaction::mga {

struct LocalTransactionManagerResult {
  Status status;
  LocalTransactionInventory inventory;
  TransactionInventoryEntry entry;
  TransactionMetricsSnapshot metrics;
  DiagnosticRecord diagnostic;

  bool ok() const {
    return status.ok();
  }
};

class LocalTransactionManager {
 public:
  LocalTransactionManager();
  explicit LocalTransactionManager(LocalTransactionInventory inventory);

  LocalTransactionManagerResult Begin(TypedUuid transaction_uuid, u64 begin_unix_epoch_millis);
  LocalTransactionManagerResult Commit(LocalTransactionId local_id, u64 final_unix_epoch_millis);
  LocalTransactionManagerResult Rollback(LocalTransactionId local_id, u64 final_unix_epoch_millis);
  LocalTransactionManagerResult Abort(LocalTransactionId local_id, u64 final_unix_epoch_millis);
  TransactionHorizonResult Horizons() const;
  TransactionSnapshotResult Snapshot(LocalTransactionId reader_transaction) const;
  const LocalTransactionInventory& inventory() const;
  TransactionMetricsSnapshot Metrics() const;

 private:
  LocalTransactionManagerResult FromInventoryResult(TransactionInventoryResult result);

  LocalTransactionInventory inventory_;
  TransactionMetrics metrics_;
  scratchbird::core::memory::ArenaAllocator transaction_arena_;
};

}  // namespace scratchbird::transaction::mga

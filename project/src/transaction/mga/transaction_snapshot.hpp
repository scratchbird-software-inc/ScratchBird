// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// SB-TXN-SNAPSHOT-ANCHOR
#include "row_version.hpp"
#include "transaction_horizon.hpp"

namespace scratchbird::transaction::mga {

struct LocalTransactionSnapshot {
  LocalTransactionId reader_transaction;
  LocalTransactionId visible_through_local_transaction;
  LocalTransactionId transaction_start_visible_through_local_transaction;
  LocalTransactionId oldest_active_transaction;
  LocalTransactionId oldest_snapshot_transaction;
  bool allow_reader_own_uncommitted = true;
};

struct TransactionSnapshotResult {
  Status status;
  LocalTransactionSnapshot snapshot;
  VisibilitySnapshot visibility_snapshot;
  DiagnosticRecord diagnostic;

  bool ok() const {
    return status.ok() && snapshot.reader_transaction.valid();
  }
};

TransactionSnapshotResult CreateLocalTransactionSnapshot(const LocalTransactionInventory& inventory,
                                                         LocalTransactionId reader_transaction);
DiagnosticRecord MakeTransactionSnapshotDiagnostic(Status status,
                                                   std::string diagnostic_code,
                                                   std::string message_key,
                                                   std::string detail = {});

}  // namespace scratchbird::transaction::mga

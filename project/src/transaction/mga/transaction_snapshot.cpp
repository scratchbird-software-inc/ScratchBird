// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "transaction_snapshot.hpp"

#include <utility>
#include <vector>

namespace scratchbird::transaction::mga {
namespace {

using scratchbird::core::platform::DiagnosticArgument;
using scratchbird::core::platform::MakeDiagnostic;
using scratchbird::core::platform::Severity;
using scratchbird::core::platform::StatusCode;
using scratchbird::core::platform::Subsystem;

Status SnapshotOkStatus() {
  return {StatusCode::ok, Severity::info, Subsystem::transaction_mga};
}

Status SnapshotErrorStatus() {
  return {StatusCode::platform_required_feature_missing, Severity::error, Subsystem::transaction_mga};
}

u64 LatestCommittedLocalTransactionId(const LocalTransactionInventory& inventory) {
  u64 latest = kInvalidLocalTransactionId;
  for (const TransactionInventoryEntry& entry : inventory.entries) {
    if ((entry.state == TransactionState::committed ||
         entry.state == TransactionState::archived) &&
        entry.identity.local_id.valid() &&
        entry.identity.local_id.value > latest) {
      latest = entry.identity.local_id.value;
    }
  }
  return latest;
}

}  // namespace

TransactionSnapshotResult CreateLocalTransactionSnapshot(const LocalTransactionInventory& inventory,
                                                         LocalTransactionId reader_transaction) {
  TransactionSnapshotResult result;
  result.status = SnapshotOkStatus();
  const auto lookup = LookupLocalTransaction(inventory, reader_transaction);
  if (!lookup.ok()) {
    result.status = lookup.status;
    result.diagnostic = lookup.diagnostic;
    return result;
  }
  if (lookup.entry.state != TransactionState::active &&
      lookup.entry.state != TransactionState::read_only_active &&
      lookup.entry.state != TransactionState::preparing &&
      lookup.entry.state != TransactionState::prepared) {
    result.status = SnapshotErrorStatus();
    result.diagnostic = MakeTransactionSnapshotDiagnostic(result.status,
                                                          "SB-TXN-SNAPSHOT-UNSUPPORTED-STATE",
                                                          "transaction.snapshot.unsupported_state",
                                                          TransactionStateName(lookup.entry.state));
    return result;
  }
  const auto horizons = ComputeLocalTransactionHorizons(inventory);
  if (!horizons.ok()) {
    result.status = horizons.status;
    result.diagnostic = horizons.diagnostic;
    return result;
  }

  result.snapshot.reader_transaction = reader_transaction;
  result.snapshot.visible_through_local_transaction =
      MakeLocalTransactionId(LatestCommittedLocalTransactionId(inventory));
  result.snapshot.transaction_start_visible_through_local_transaction =
      MakeLocalTransactionId(lookup.entry.begin_visible_through_local_transaction_id);
  result.snapshot.oldest_active_transaction = horizons.horizons.oldest_active_transaction;
  result.snapshot.oldest_snapshot_transaction = horizons.horizons.oldest_snapshot_transaction;
  result.snapshot.allow_reader_own_uncommitted = true;
  result.visibility_snapshot.reader_transaction = reader_transaction;
  result.visibility_snapshot.visible_through_local_transaction_id =
      result.snapshot.visible_through_local_transaction.value;
  result.visibility_snapshot.visible_through_local_transaction_id_is_boundary = true;
  result.visibility_snapshot.allow_reader_own_uncommitted = true;
  result.visibility_snapshot.recovery_context = false;
  return result;
}

DiagnosticRecord MakeTransactionSnapshotDiagnostic(Status status,
                                                   std::string diagnostic_code,
                                                   std::string message_key,
                                                   std::string detail) {
  std::vector<DiagnosticArgument> arguments;
  if (!detail.empty()) {
    arguments.push_back({"detail", detail});
  }
  return MakeDiagnostic(status.code,
                        status.severity,
                        status.subsystem,
                        std::move(diagnostic_code),
                        std::move(message_key),
                        std::move(arguments),
                        {},
                        "transaction.mga.snapshot");
}

}  // namespace scratchbird::transaction::mga

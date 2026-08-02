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

#include <string>
#include <vector>

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

enum class SnapshotVectorKind : u16 {
  unknown = 0,
  statement_stable = 1,
};

// The sole engine-owned complete data-visibility snapshot descriptor.  Query,
// planner, optimizer, parser, server, and test carriers may copy these fields,
// but they do not create, reconstruct, or reinterpret this authority.
struct SnapshotVectorDescriptor {
  TypedUuid snapshot_uuid;
  TypedUuid owning_transaction_uuid;
  LocalTransactionId owning_transaction;
  u64 visible_committed_high_watermark = kInvalidLocalTransactionId;
  LocalTransactionId oldest_active_transaction;
  LocalTransactionId oldest_interesting_transaction;
  LocalTransactionId oldest_snapshot_transaction;
  LocalTransactionId retention_horizon_transaction;
  std::vector<u64> active_excluded_local_transaction_ids;
  std::vector<u64> in_doubt_excluded_local_transaction_ids;
  SnapshotVectorKind snapshot_kind = SnapshotVectorKind::unknown;
  u64 publication_inventory_next_local_transaction_id = 0;
  bool inventory_authoritative = false;
  bool complete = false;
};

struct SnapshotVectorResult {
  Status status;
  SnapshotVectorDescriptor descriptor;
  DiagnosticRecord diagnostic;

  bool ok() const {
    return status.ok() && descriptor.complete &&
           descriptor.inventory_authoritative &&
           descriptor.snapshot_uuid.valid();
  }
};

TransactionSnapshotResult CreateLocalTransactionSnapshot(const LocalTransactionInventory& inventory,
                                                         LocalTransactionId reader_transaction);
SnapshotVectorResult PublishStatementStableSnapshotVector(
    const LocalTransactionInventory& inventory,
    LocalTransactionId owning_transaction,
    u64 publication_unix_epoch_millis);
SnapshotVectorResult ResolvePublishedSnapshotVector(
    const TypedUuid& snapshot_uuid);
SnapshotVectorResult ResolveCurrentStatementStableSnapshotVector(
    const LocalTransactionInventory& inventory,
    const TypedUuid& snapshot_uuid,
    const TypedUuid& expected_owning_transaction_uuid,
    LocalTransactionId expected_owning_transaction);
void RevokePublishedSnapshotVectorsForTransaction(
    const TypedUuid& owning_transaction_uuid,
    LocalTransactionId owning_transaction);
void RevokePublishedSnapshotVector(const TypedUuid& snapshot_uuid);
bool SnapshotVectorDescriptorEqual(const SnapshotVectorDescriptor& left,
                                   const SnapshotVectorDescriptor& right);
const char* SnapshotVectorKindName(SnapshotVectorKind kind);
DiagnosticRecord MakeTransactionSnapshotDiagnostic(Status status,
                                                   std::string diagnostic_code,
                                                   std::string message_key,
                                                   std::string detail = {});

}  // namespace scratchbird::transaction::mga

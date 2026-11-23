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
#include <memory>
#include <utility>
#include <vector>

namespace scratchbird::transaction::mga {

struct LocalTransactionSnapshot {
  LocalTransactionId reader_transaction;
  LocalTransactionId visible_through_local_transaction;
  LocalTransactionId transaction_start_visible_through_local_transaction;
  LocalTransactionId oldest_active_transaction;
  LocalTransactionId oldest_snapshot_transaction;
  bool allow_reader_own_uncommitted = true;
  u64 visible_through_commit_sequence = 0;
  u64 transaction_start_visible_through_commit_sequence = 0;
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

// Engine-private ownership of an already published snapshot. A pin cannot be
// reconstructed from a descriptor or survive explicit transaction/recovery
// revocation. It retains the cleanup horizon until its last owner releases it.
enum class PublishedSnapshotObservationError { none, unknown, revoked, lock_failure };
struct PublishedSnapshotObservation {
  PublishedSnapshotObservationError error = PublishedSnapshotObservationError::unknown;
  const SnapshotVectorDescriptor* descriptor = nullptr;
  bool ok() const noexcept {
    return error == PublishedSnapshotObservationError::none && descriptor;
  }
  // Static message-vector identifiers; rendering a diagnostic may allocate and
  // belongs outside storage guards. No caller descriptor grants authority.
  const char* diagnostic_code() const noexcept {
    switch (error) {
      case PublishedSnapshotObservationError::none: return "";
      case PublishedSnapshotObservationError::unknown: return "SB-MGA-SNAPSHOT-VECTOR-UNKNOWN";
      case PublishedSnapshotObservationError::revoked: return "SB-MGA-SNAPSHOT-VECTOR-REVOKED";
      case PublishedSnapshotObservationError::lock_failure: return "SB-MGA-SNAPSHOT-VECTOR-LOCK-FAILURE";
    }
    return "SB-MGA-SNAPSHOT-VECTOR-UNKNOWN";
  }
  const char* message_key() const noexcept {
    switch (error) {
      case PublishedSnapshotObservationError::none: return "";
      case PublishedSnapshotObservationError::unknown: return "transaction.snapshot_vector.unknown";
      case PublishedSnapshotObservationError::revoked: return "transaction.snapshot_vector.revoked";
      case PublishedSnapshotObservationError::lock_failure: return "transaction.snapshot_vector.lock_failure";
    }
    return "transaction.snapshot_vector.unknown";
  }
};
class PublishedSnapshotPin final {
 public:
  PublishedSnapshotPin() noexcept = default;
  PublishedSnapshotPin(PublishedSnapshotPin&&) noexcept = default;
  PublishedSnapshotPin& operator=(PublishedSnapshotPin&&) noexcept = default;
  PublishedSnapshotPin(const PublishedSnapshotPin&) = delete;
  PublishedSnapshotPin& operator=(const PublishedSnapshotPin&) = delete;
  // Ownership only; Resolve also checks explicit revocation.
  bool valid() const noexcept { return lease_ != nullptr; }
  SnapshotVectorResult Resolve() const;
  // Allocation-free observation of immutable publication-owned backing. The
  // caller must retain this pin throughout use and observe again after work:
  // a previously returned view is not ongoing authorization after revocation.
  // Moving a pin transfers backing lifetime; Release/destruction ends it.
  // Concurrent Observe calls are supported; concurrent mutation of this SAME
  // pin object (move/Release/destruction) requires caller synchronization.
  PublishedSnapshotObservation Observe() const noexcept;
  void Release() noexcept { lease_.reset(); }
 private:
  struct Lease;
  explicit PublishedSnapshotPin(std::shared_ptr<Lease> lease) noexcept
      : lease_(std::move(lease)) {}
  std::shared_ptr<Lease> lease_;
  friend PublishedSnapshotPin RetainPublishedSnapshotVector(const TypedUuid&);
};

PublishedSnapshotPin RetainPublishedSnapshotVector(const TypedUuid& snapshot_uuid);
// End the publication owner's lifetime, without revoking previously retained
// pins. New pins cannot be acquired after this call. Explicit invalidation must
// continue to use RevokePublishedSnapshotVector(sForTransaction).
void ReleasePublishedSnapshotVector(const TypedUuid& snapshot_uuid);
// Runtime cleanup input only. Do not feed this into durable inventory/page
// validation: that pure calculation must be independent of live process state.
struct PublishedSnapshotHorizonResult {
  Status status;
  std::vector<LocalTransactionId> horizons;
  DiagnosticRecord diagnostic;
  bool ok() const { return status.ok(); }
};
PublishedSnapshotHorizonResult PublishedSnapshotRetentionHorizons(
    const LocalTransactionInventory& inventory);

TransactionSnapshotResult CreateLocalTransactionSnapshot(const LocalTransactionInventory& inventory,
                                                         LocalTransactionId reader_transaction);
// SB_SNAPSHOT_POINT_VIEW (owner 2026-11-21: the true snapshot-view reader entry). A visibility snapshot VALUE for a
// reader that is NOT a transaction (an in-memory snapshot point, like Firebird's system transaction 0 for the
// authentication read), built from an inventory image once, at the capture. Meaning of each field, by the native TIP
// state of every slot below Next at the capture:
//   visible_through_local_transaction_id = Next - 1, a boundary: a creator numbered above it began after the capture
//     (or is the capture's own successor) and is never visible.
//   active_excluded  = slots in state active or read_only_active at the capture: they may commit later and must stay
//     invisible.
//   in_doubt_excluded = slots in state created (starting), preparing, prepared, committing, rolling_back, limbo or
//     recovering at the capture: their outcome is not settled, so they stay invisible whatever happens next.
//   committed / rolled_back / failed_terminal (dead) / archived slots are in neither list: the row version's own creator
//     state decides (committed -> visible, rolled back or dead -> never visible), and a terminal state cannot change.
//   reader_transaction is NONE and allow_reader_own_uncommitted is false (the snapshot point owns no rows).
//   visible_through_commit_sequence is the last commit sequence at the capture, recorded for the caller but NOT a
//     boundary: commit sequences are rebuilt from commit times at every inventory read, so a later commit whose clock
//     stamp is earlier would shift them; the order boundary and the two lists above are complete without it.
struct SnapshotPointVisibilityResult {
  Status status;
  VisibilitySnapshot snapshot;
  DiagnosticRecord diagnostic;
  bool ok() const { return status.ok(); }
};
SnapshotPointVisibilityResult CreateSnapshotPointVisibility(const LocalTransactionInventory& inventory);
SnapshotVectorResult PublishStatementStableSnapshotVector(
    const LocalTransactionInventory& inventory,
    LocalTransactionId owning_transaction,
    u64 publication_unix_epoch_millis);
// SB_NATIVE_BEGIN_VIEW_SNAPSHOT (S4f): the same descriptor, published from a visibility snapshot captured when the owning
// transaction BEGAN (a native TIP does not record it; the database loop keeps it in memory). `inventory` is the live
// inventory (horizons and Next of the publication); `begin_view` is the value CreateSnapshotPointVisibility returned
// for the chains just before the begin (boundary = the begin's Next - 1; the exclusion lists as they stood).
SnapshotVectorResult PublishStatementStableSnapshotVectorFromBeginView(
    const LocalTransactionInventory& inventory,
    LocalTransactionId owning_transaction,
    const VisibilitySnapshot& begin_view,
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

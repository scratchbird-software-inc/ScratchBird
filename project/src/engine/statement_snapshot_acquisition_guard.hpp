// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "transaction_snapshot.hpp"

namespace scratchbird::engine {

// The actual acquisition owner's lease, not a wire/capability projection.
// Construction follows successful MGA publication and cannot allocate.
class StatementSnapshotAcquisitionGuard final {
 public:
  explicit StatementSnapshotAcquisitionGuard(
      core::platform::TypedUuid snapshot_uuid) noexcept
      : snapshot_uuid_(snapshot_uuid) {}
  StatementSnapshotAcquisitionGuard(const StatementSnapshotAcquisitionGuard&) = delete;
  StatementSnapshotAcquisitionGuard& operator=(const StatementSnapshotAcquisitionGuard&) = delete;
  StatementSnapshotAcquisitionGuard(StatementSnapshotAcquisitionGuard&&) = delete;
  StatementSnapshotAcquisitionGuard& operator=(StatementSnapshotAcquisitionGuard&&) = delete;
  ~StatementSnapshotAcquisitionGuard() {
    if (owned_) {
      transaction::mga::RevokePublishedSnapshotVector(snapshot_uuid_);
      transaction::mga::ReleasePublishedSnapshotVector(snapshot_uuid_);
    }
  }

  // Call only after live receipt/statement-binding insertion. That owner now
  // holds publication; this guard may no longer revoke or release it.
  void TransferToOwner() noexcept { owned_ = false; }

 private:
  core::platform::TypedUuid snapshot_uuid_;
  bool owned_ = true;
};

}  // namespace scratchbird::engine

// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// SB-SNTXN-DURABLE-MANAGER-ANCHOR
#include "database_lifecycle.hpp"
#include "disk_device.hpp"
#include "transaction_horizon.hpp"
#include "transaction_inventory.hpp"

#include <cstdint>
#include <memory>
#include <string>

namespace scratchbird::storage::database {

using scratchbird::core::platform::u32;

struct LocalTransactionStoreResult {
  Status status;
  scratchbird::transaction::mga::LocalTransactionInventory inventory;
  scratchbird::transaction::mga::LocalTransactionHorizons horizons;
  DiagnosticRecord diagnostic;

  bool ok() const {
    return status.ok();
  }
};

// Strong, immutable transaction-inventory authority acquired once at the
// statement boundary.  The two hashes authenticate the journal and inventory
// root bytes. Database size/mtime are attachment provenance only because the
// same file also contains unrelated catalog/relation pages; the publish-
// journal identity is retained for execution-time TOCTOU fencing without
// rehashing transaction bytes at every consumer.
struct LocalTransactionInventorySnapshot {
  std::string database_path;
  std::uint64_t database_file_size{0};
  std::int64_t database_write_time_count{0};
  bool publish_journal_present{false};
  std::uint64_t publish_journal_size{0};
  std::int64_t publish_journal_write_time_count{0};
  std::string publish_journal_sha256;
  std::string inventory_root_body_sha256;
  scratchbird::transaction::mga::LocalTransactionInventory inventory;
  scratchbird::transaction::mga::LocalTransactionHorizons horizons;
};

struct LocalTransactionInventorySnapshotResult {
  Status status;
  std::shared_ptr<const LocalTransactionInventorySnapshot> snapshot;
  DiagnosticRecord diagnostic;

  bool ok() const {
    return status.ok() && snapshot != nullptr;
  }
};

struct LocalTransactionInventoryFenceResult {
  Status status;
  bool unchanged{false};
  DiagnosticRecord diagnostic;

  bool ok() const {
    return status.ok() && unchanged;
  }
};

// Returns the current shared immutable inventory/horizon snapshot. A warm
// lookup uses only the retained file-identity fence; it does not rehash the
// journal and inventory root. Statement consumers should retain this carrier
// instead of calling the legacy copy-returning loader again.
LocalTransactionInventorySnapshotResult
AcquireLocalTransactionInventorySnapshot(std::string path);

// Statement-attachment boundary. This performs the expensive journal/root
// integrity signature even when a process snapshot is warm, then returns the
// exact immutable snapshot that every consumer of that statement must retain.
LocalTransactionInventorySnapshotResult
AcquireStrongLocalTransactionInventorySnapshot(std::string path);

// Compares only the retained database-wide inventory publication identity. It
// is deliberately a cheap execution boundary signal; a changed result does
// not by itself prove that one statement is stale because another transaction
// may have published. Statement consumers must then revalidate their exact
// owning transaction and published snapshot against current authority. A new
// statement attachment performs the full journal/root hash validation again.
LocalTransactionInventoryFenceResult RevalidateLocalTransactionInventorySnapshot(
    const LocalTransactionInventorySnapshot& snapshot);

LocalTransactionStoreResult LoadLocalTransactionInventoryFromDatabase(std::string path);
LocalTransactionStoreResult LoadLocalTransactionInventoryFromOpenDevice(
    scratchbird::storage::disk::FileDevice* device,
    u32 page_size);
LocalTransactionStoreResult PersistLocalTransactionInventoryToDatabase(
    std::string path,
    scratchbird::transaction::mga::LocalTransactionInventory inventory);
LocalTransactionStoreResult PersistLocalTransactionInventoryToOpenDevice(
    scratchbird::storage::disk::FileDevice* device,
    u32 page_size,
    scratchbird::transaction::mga::LocalTransactionInventory inventory);

}  // namespace scratchbird::storage::database

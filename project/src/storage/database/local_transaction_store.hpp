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

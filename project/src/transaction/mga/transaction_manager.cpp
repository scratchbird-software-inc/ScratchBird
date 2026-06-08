// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "transaction_manager.hpp"

#include "transaction_memory_hooks.hpp"

namespace scratchbird::transaction::mga {

using scratchbird::core::memory::DefaultMemoryManager;
using scratchbird::core::memory::MemoryCategory;
using scratchbird::core::memory::MemoryLifetime;

LocalTransactionManager::LocalTransactionManager()
    : LocalTransactionManager(MakeEmptyLocalTransactionInventory()) {}

LocalTransactionManager::LocalTransactionManager(LocalTransactionInventory inventory)
    : inventory_(std::move(inventory)),
      transaction_arena_(DefaultMemoryManager().CreateArena(
          MakeMgaMemoryTag(MemoryCategory::transaction_local,
                           "local_transaction_manager_arena",
                           MemoryLifetime::arena))) {}

LocalTransactionManagerResult LocalTransactionManager::Begin(TypedUuid transaction_uuid, u64 begin_unix_epoch_millis) {
  (void)transaction_arena_.Allocate(128, 64);
  TransactionInventoryResult result = BeginLocalTransaction(inventory_, transaction_uuid, begin_unix_epoch_millis);
  if (result.ok()) {
    metrics_.RecordBegin();
    const auto horizons = ComputeLocalTransactionHorizons(result.inventory);
    if (horizons.ok()) {
      PublishTransactionHorizonMetrics(result.inventory, horizons.horizons, begin_unix_epoch_millis);
    }
  } else {
    metrics_.RecordFailure();
  }
  return FromInventoryResult(std::move(result));
}

LocalTransactionManagerResult LocalTransactionManager::Commit(LocalTransactionId local_id, u64 final_unix_epoch_millis) {
  TransactionInventoryResult result = CommitLocalTransaction(inventory_, local_id, final_unix_epoch_millis);
  if (result.ok()) {
    metrics_.RecordCommit();
    const auto horizons = ComputeLocalTransactionHorizons(result.inventory);
    if (horizons.ok()) {
      PublishTransactionHorizonMetrics(result.inventory, horizons.horizons, final_unix_epoch_millis);
    }
  } else {
    metrics_.RecordFailure();
  }
  return FromInventoryResult(std::move(result));
}

LocalTransactionManagerResult LocalTransactionManager::Rollback(LocalTransactionId local_id, u64 final_unix_epoch_millis) {
  TransactionInventoryResult result = RollbackLocalTransaction(inventory_, local_id, final_unix_epoch_millis);
  if (result.ok()) {
    metrics_.RecordRollback();
    const auto horizons = ComputeLocalTransactionHorizons(result.inventory);
    if (horizons.ok()) {
      PublishTransactionHorizonMetrics(result.inventory, horizons.horizons, final_unix_epoch_millis);
    }
  } else {
    metrics_.RecordFailure();
  }
  return FromInventoryResult(std::move(result));
}

LocalTransactionManagerResult LocalTransactionManager::Abort(LocalTransactionId local_id, u64 final_unix_epoch_millis) {
  TransactionInventoryResult result = AbortLocalTransaction(inventory_, local_id, final_unix_epoch_millis);
  if (result.ok()) {
    metrics_.RecordAbort();
    const auto horizons = ComputeLocalTransactionHorizons(result.inventory);
    if (horizons.ok()) {
      PublishTransactionHorizonMetrics(result.inventory, horizons.horizons, final_unix_epoch_millis);
    }
  } else {
    metrics_.RecordFailure();
  }
  return FromInventoryResult(std::move(result));
}

TransactionHorizonResult LocalTransactionManager::Horizons() const {
  return ComputeLocalTransactionHorizons(inventory_);
}

TransactionSnapshotResult LocalTransactionManager::Snapshot(LocalTransactionId reader_transaction) const {
  return CreateLocalTransactionSnapshot(inventory_, reader_transaction);
}

const LocalTransactionInventory& LocalTransactionManager::inventory() const {
  return inventory_;
}

TransactionMetricsSnapshot LocalTransactionManager::Metrics() const {
  return metrics_.Snapshot();
}

LocalTransactionManagerResult LocalTransactionManager::FromInventoryResult(TransactionInventoryResult result) {
  LocalTransactionManagerResult manager_result;
  manager_result.status = result.status;
  manager_result.diagnostic = result.diagnostic;
  if (result.ok()) {
    inventory_ = result.inventory;
    manager_result.inventory = inventory_;
    manager_result.entry = result.entry;
  } else {
    manager_result.inventory = inventory_;
  }
  manager_result.metrics = metrics_.Snapshot();
  return manager_result;
}

}  // namespace scratchbird::transaction::mga

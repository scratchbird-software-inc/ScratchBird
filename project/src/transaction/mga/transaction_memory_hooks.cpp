// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "transaction_memory_hooks.hpp"

namespace scratchbird::transaction::mga {

const std::vector<MgaMemoryHook>& MgaMemoryHooks() {
  static const std::vector<MgaMemoryHook> hooks = {
      {MemoryCategory::transaction_local, MemoryLifetime::transaction, "local_transaction_inventory", true},
      {MemoryCategory::transaction_snapshot, MemoryLifetime::transaction, "snapshot_visibility_state", true},
      {MemoryCategory::version_chain, MemoryLifetime::deferred_epoch, "mga_version_chain_reclamation", true},
      {MemoryCategory::cleanup, MemoryLifetime::deferred_epoch, "cleanup_horizon_work", true},
      {MemoryCategory::archive, MemoryLifetime::database, "archive_retention_work", false},
  };
  return hooks;
}

MemoryTag MakeMgaMemoryTag(MemoryCategory category, const char* purpose, MemoryLifetime lifetime) {
  return MemoryTag{Subsystem::transaction_mga,
                   purpose == nullptr ? "mga_memory" : purpose,
                   category,
                   lifetime,
                   "transaction.mga",
                   "mga"};
}

}  // namespace scratchbird::transaction::mga

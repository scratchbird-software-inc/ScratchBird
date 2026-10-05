// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "native_btree_tree_backing.hpp"
#include "native_selected_checkpoint_memory_lease.hpp"

namespace scratchbird::storage::database {
// Navigation only: the caller's root/dependency tuple is not catalog authority.
// Admit backing before the source handle; release it after the handle. Reuse
// invalidates all previous borrowed results even when traversal fails.
class NativeBtreeTreeLeaseReader {
 public:
  static NativeBtreeTreeReadResult Read(const NativeSelectedCheckpointMemoryLease& lease,
      const disk::NativePageReference& root,const page::NativeBtreeDependencies& dependencies,
      NativeBtreeTreePreparedMemory& backing) noexcept {
    lease.CheckThread();
    if(!lease.selection_.checkpoint_inventory.checkpoint)return {};
    const auto& checkpoint=*lease.selection_.checkpoint_inventory.checkpoint;
    if(checkpoint.flags&4u){
      NativeBtreeTreeReadResult out;out.error=NativeBtreeTreeMemoryError::tree_failure;
      out.tree_error=page::NativeBtreeError::cluster_requires_authority;return out;
    }
    return btree_memory_detail::Reader::Run(checkpoint.header.database_uuid,lease.devices_,
      {lease.batches_,lease.batch_count_},root,dependencies,backing);
  }
};
} // namespace scratchbird::storage::database

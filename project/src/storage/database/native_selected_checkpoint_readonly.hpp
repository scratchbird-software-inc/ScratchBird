// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "native_checkpoint_selection.hpp"
#include "native_metadata_decode_scratch.hpp"
#include "native_page_chain_readonly.hpp"

namespace scratchbird::storage::database {
// Immutable projections only; no selection, publication or serving authority.
// The owning lease, its arena and every borrowed device must remain alive.
using NativeReadCheckpointRoot = NativeCheckpointRootData<std::span<const NativeCheckpointRootReference>>;
struct NativeReadCheckpointInventory {
  NativeCheckpointError error=NativeCheckpointError::invalid_reference;
  page::NativeInventoryError inventory_error=page::NativeInventoryError::none;
  std::optional<NativeReadCheckpointRoot> checkpoint;
  std::array<byte,32> checkpoint_sha256{};
  NativeReadTransactionInventory inventory;
  std::span<const NativeInventoryPageBinding> inventory_pages;
  u64 inventory_generation=0,retained_image_bytes=0;
  std::size_t backing_bytes_used=0;
  bool ok() const noexcept {return error==NativeCheckpointError::none&&checkpoint.has_value();}
};
struct NativeReadBoundCheckpointSelection {
  NativeCheckpointSelectionError error=NativeCheckpointSelectionError::invalid_pair;
  NativeCheckpointError checkpoint_error=NativeCheckpointError::none;
  page::NativeAllocationError allocation_error=page::NativeAllocationError::none;
  std::optional<NativeCheckpointSelection> selection;
  std::array<std::span<const byte>,2> slots;
  NativeReadCheckpointInventory checkpoint_inventory,predecessor;
  NativeReadAllocationChain allocation;
  u64 retained_image_bytes=0;
  std::size_t backing_bytes_used=0;
  bool ok() const noexcept{return error==NativeCheckpointSelectionError::none&&selection&&
    !slots[0].empty()&&!slots[1].empty()&&checkpoint_inventory.ok()&&allocation.ok()&&
    (selection->selection_generation==1?!predecessor.checkpoint.has_value():predecessor.ok());}
};
struct NativeReadCheckpointDirectory {
  NativeCheckpointError error=NativeCheckpointError::invalid_reference;
  page::NativeDirectoryError directory_error=page::NativeDirectoryError::none;
  NativeReadCheckpointInventory checkpoint_inventory;
  NativeReadDirectoryChain directory;
  u64 retained_image_bytes=0;
  std::size_t backing_bytes_used=0;
  bool ok() const noexcept{return error==NativeCheckpointError::none&&checkpoint_inventory.ok()&&directory.ok();}
};
namespace detail {
inline NativeReadCheckpointRoot ReadOnly(const NativeCheckpointRootView& r) noexcept {
  return {r.header,r.object_uuid,r.checkpoint_generation,r.root_set_generation,
    r.selected_local_transaction_id,r.stable_local_transaction_id,r.local_durable_transaction_id,
    r.cluster_quorum_transaction_id,r.timeline_uuid,r.creator_transaction_uuid,
    r.creator_local_transaction_id,r.flags,r.predecessor,r.predecessor_sha256,
    r.completed,r.roots,r.creator_operation_uuid};
}
inline NativeReadCheckpointInventory ReadOnly(const NativeCheckpointInventoryView& v) noexcept {
  NativeReadCheckpointInventory r;
  r.error=v.error;r.inventory_error=v.inventory_error;
  if(v.checkpoint)r.checkpoint=ReadOnly(*v.checkpoint);
  r.checkpoint_sha256=v.checkpoint_sha256;
  r.inventory={v.inventory.next_local_transaction_id,v.inventory.next_commit_sequence,v.inventory.entries};
  r.inventory_pages=v.inventory_pages;r.inventory_generation=v.inventory_generation;
  r.retained_image_bytes=v.retained_image_bytes;r.backing_bytes_used=v.backing_bytes_used;return r;
}
inline NativeReadBoundCheckpointSelection ReadOnly(const NativeBoundCheckpointSelectionView& v,NativeMetadataScratch& scratch) {
  return {v.error,v.checkpoint_error,v.allocation_error,v.selection,v.slots,
    ReadOnly(v.checkpoint_inventory),ReadOnly(v.predecessor),ReadOnly(v.allocation,scratch),
    v.retained_image_bytes,v.backing_bytes_used};
}
inline NativeReadCheckpointDirectory ReadOnly(const NativeCheckpointDirectoryView& v,NativeMetadataScratch& scratch) {
  return {v.error,v.directory_error,ReadOnly(v.checkpoint_inventory),ReadOnly(v.directory,scratch),
    v.retained_image_bytes,v.backing_bytes_used};
}
} // namespace detail
} // namespace scratchbird::storage::database

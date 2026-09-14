// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "native_common_page_header.hpp"
#include "database_dirty_manifest.hpp"
#include <array>
#include <optional>
#include <vector>

namespace scratchbird::storage::database {
using scratchbird::core::platform::Uuid;
using scratchbird::core::platform::byte;
using scratchbird::core::platform::u64;
struct NativeCheckpointSelection {
  disk::NativeCommonPageHeader header;
  Uuid object_uuid, bootstrap_uuid, publication_uuid;
  u64 selection_generation=0;
  disk::NativePageReference checkpoint;
  Uuid checkpoint_object_uuid;
  std::array<byte,32> checkpoint_sha256{};
  u64 checkpoint_generation=0, root_set_generation=0;
  Uuid timeline_uuid;
  u64 previous_selection_generation=0;
  std::optional<disk::NativePageReference> previous_checkpoint;
  Uuid previous_checkpoint_object_uuid;
  std::array<byte,32> previous_checkpoint_sha256{};
};
enum class NativeCheckpointSelectionError {
  none, invalid_header, invalid_family, invalid_identity, invalid_reference,
  invalid_integrity, hash_failure, resource_exhausted, invalid_pair, repair_required,
  invalid_filespace, bootstrap_failure, slot_binding_mismatch, checkpoint_failure,
  checkpoint_binding_mismatch, allocation_failure, allocation_binding_mismatch,
  creator_mismatch, io_failure
};
struct NativeCheckpointSelectionImage {
  NativeCheckpointSelectionError error=NativeCheckpointSelectionError::invalid_family;
  std::optional<NativeCheckpointSelection> selection;
  std::vector<byte> bytes;
  bool ok() const noexcept {return error==NativeCheckpointSelectionError::none&&selection.has_value();}
};
NativeCheckpointSelectionImage EncodeNativeCheckpointSelection(const NativeCheckpointSelection&) noexcept;
NativeCheckpointSelectionImage DecodeNativeCheckpointSelection(const std::vector<byte>&) noexcept;
// Image-level classification only. A stable pair does not prove allocation,
// bootstrap binding, actual checkpoint inventory, publication or serving.
struct NativeCheckpointSelectionPair {
  NativeCheckpointSelectionError error=NativeCheckpointSelectionError::invalid_pair;
  // Present only for a stable, internally agreeing pair; never a damaged prefix.
  std::optional<NativeCheckpointSelection> selection;
  bool ok() const noexcept {return error==NativeCheckpointSelectionError::none&&selection.has_value();}
};
NativeCheckpointSelectionPair ClassifyNativeCheckpointSelectionPair(
    const std::vector<byte>& first,const std::vector<byte>& second) noexcept;
struct NativeBoundCheckpointSelection {
  NativeCheckpointSelectionError error=NativeCheckpointSelectionError::invalid_pair;
  NativeCheckpointError checkpoint_error=NativeCheckpointError::none;
  page::NativeAllocationError allocation_error=page::NativeAllocationError::none;
  std::optional<NativeCheckpointSelection> selection;
  std::array<std::vector<byte>,2> slots;
  NativeCheckpointInventoryResult checkpoint_inventory;
  NativeCheckpointInventoryResult predecessor;
  page::NativeAllocationChainResult allocation;
  u64 retained_image_bytes=0;
  bool ok() const noexcept {return error==NativeCheckpointSelectionError::none&&selection.has_value()&&
    !slots[0].empty()&&!slots[1].empty()&&checkpoint_inventory.ok()&&allocation.ok()&&
    (selection->selection_generation==1?!predecessor.checkpoint.has_value():predecessor.ok());}
};
// Actual bootstrap-declared slots, selected checkpoint/inventory and allocated
// slot ownership. No complete-root serving, publication, repair or SQL receipt.
NativeBoundCheckpointSelection ReadNativeBoundCheckpointSelectionFromOpenDevices(
    const Uuid& database_uuid,const std::vector<disk::NativeFilespaceDevice>&,
    const Uuid& primary_filespace_uuid,u64 maximum_retained_image_bytes) noexcept;
}  // namespace scratchbird::storage::database

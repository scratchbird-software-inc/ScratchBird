// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "native_management_control_authority.hpp"

namespace scratchbird::storage::database {
enum class NativeFilespaceCapacityError {
  none, invalid_request, invalid_target, checkpoint_failure, bootstrap_failure,
  allocation_failure, root_mismatch, creator_mismatch, physical_capacity_mismatch,
  management_failure, resource_exhausted, hash_failure, io_failure,
  cluster_requires_authority, encrypted_requires_authority
};
struct NativeFilespaceCapacityObservation {
  Uuid database_uuid, filespace_uuid, locator_uuid, page_zero_uuid, page_size_profile_uuid;
  core::platform::u32 page_size_bytes=0;
  core::platform::u16 filespace_role=0, lifecycle_state=0;
  u64 page_zero_generation=0, filespace_root_set_generation=0, directory_generation=0;
  u64 checkpoint_generation=0, checkpoint_root_set_generation=0;
  disk::FilespaceRootReference checkpoint, allocation_root;
  std::array<byte,32> checkpoint_sha256{}, allocation_sha256{};
  u64 map_generation=0, capacity_generation=0, total_pages=0, physical_bytes=0;
  std::array<u64,8> state_counts{};
};
struct NativeFilespaceCapacityResult {
  NativeFilespaceCapacityError error=NativeFilespaceCapacityError::invalid_request;
  NativeCheckpointError checkpoint_error=NativeCheckpointError::none;
  page::NativeInventoryError inventory_error=page::NativeInventoryError::none;
  page::NativeDirectoryError directory_error=page::NativeDirectoryError::none;
  page::NativeAllocationError allocation_error=page::NativeAllocationError::none;
  disk::FilespacePageZeroError bootstrap_error=disk::FilespacePageZeroError::none;
  NativeManagementControlAuthorityError management_error=NativeManagementControlAuthorityError::none;
  std::optional<NativeFilespaceCapacityObservation> observation;
  u64 retained_image_bytes=0;
  bool ok() const noexcept {return error==NativeFilespaceCapacityError::none&&observation.has_value();}
};
// Actual current directory/allocation/inventory observation, not an allocation,
// policy/security grant, serving decision or durable operation receipt.
// Borrowed devices stay owned by the caller. Acquire no unordered subset of their
// guards before this call. No path opens, writes, syncs or repair are performed.
NativeFilespaceCapacityResult ReadNativeFilespaceCapacityFromOpenDevices(
  const Uuid& database_uuid,const std::vector<disk::NativeFilespaceDevice>&,
  const disk::FilespaceRootReference& expected_current_checkpoint,
  const Uuid& target_filespace_uuid,u64 maximum_retained_image_bytes) noexcept;
} // namespace scratchbird::storage::database

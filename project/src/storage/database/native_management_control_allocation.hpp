// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "native_publication_plan.hpp"

namespace scratchbird::storage::database {
enum class NativeManagementControlAllocationError {
  none, invalid_request, invalid_checkpoint, invalid_plan, invalid_extent,
  invalid_allocation, binding_mismatch, invalid_delta, resource_exhausted,
  hash_failure, cluster_requires_authority, encrypted_requires_authority, invalid_workspace
};
// Complete immutable primary-only metadata/inventory allocation delta. Directory
// publications require their complete base-directory delta, not this entry point.
// The base must separately
// come from the owning device lease. No I/O, selection, authentication or grant.
NativeManagementControlAllocationError ValidateNativeManagementControlAllocation(
  const std::vector<byte>& base_checkpoint, const std::vector<byte>& target_checkpoint,
  const std::vector<byte>& plan, const std::vector<std::vector<byte>>& extent,
  const std::vector<std::vector<byte>>& base_allocation,
  const std::vector<std::vector<byte>>& target_allocation, u64 maximum_input_image_bytes,
  const std::vector<std::vector<byte>>& control_bundle = {},
  const std::vector<std::vector<byte>>& base_inventory = {}) noexcept;
struct NativeManagementDirectoryBase {
  std::vector<std::vector<byte>> directory_images;
  // Exact original images for every filespace represented in base_allocation.
  // Immutable verification inputs, not caller assertions of live authority.
  std::vector<std::vector<byte>> page_zero_images;
};
// Complete directory-bearing inventory/preallocation/growth image transition.
// Requires actual base acquisition and separate physical/admission protocols.
NativeManagementControlAllocationError ValidateNativeManagementDirectoryControlAllocation(
  const std::vector<byte>& base_checkpoint, const std::vector<byte>& target_checkpoint,
  const std::vector<byte>& plan, const std::vector<std::vector<byte>>& extent,
  const std::vector<std::vector<byte>>& base_allocation,
  const std::vector<std::vector<byte>>& target_allocation,
  const NativeManagementDirectoryBase&, u64 maximum_input_image_bytes,
  const std::vector<std::vector<byte>>& control_bundle,
  const std::vector<std::vector<byte>>& base_inventory = {}) noexcept;

struct NativeManagementDirectoryBaseView {
  std::span<const std::span<const byte>> directory_images,page_zero_images;
};
struct NativeManagementControlAllocationInputs {
  std::span<const byte> base_checkpoint,target_checkpoint,plan;
  std::span<const std::span<const byte>> extent,base_allocation,target_allocation;
  std::span<const std::span<const byte>> control_bundle,base_inventory;
  const NativeManagementDirectoryBaseView* directory=nullptr;
  u64 maximum_input_image_bytes=0;
};
struct NativeManagementControlAllocationViewResult {
  NativeManagementControlAllocationError error=NativeManagementControlAllocationError::invalid_request;
  std::size_t backing_bytes_used=0;
  bool ok() const noexcept{return error==NativeManagementControlAllocationError::none;}
};
// Complete immutable control delta with shared source-independent validation.
// Backing excludes the whole request, all descriptor arrays and all input bytes.
// No I/O, memory grant, selected source, admission or publication authority.
NativeManagementControlAllocationViewResult ValidateNativeManagementControlAllocationInto(
  const NativeManagementControlAllocationInputs&,std::span<byte> backing) noexcept;
} // namespace scratchbird::storage::database

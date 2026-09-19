// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "native_publication_plan.hpp"

namespace scratchbird::storage::database {
enum class NativeManagementControlAllocationError {
  none, invalid_request, invalid_checkpoint, invalid_plan, invalid_extent,
  invalid_allocation, binding_mismatch, invalid_delta, resource_exhausted,
  hash_failure, cluster_requires_authority, encrypted_requires_authority
};
// Complete immutable metadata/inventory allocation delta. The base must separately
// come from the owning device lease. No I/O, selection, authentication or grant.
NativeManagementControlAllocationError ValidateNativeManagementControlAllocation(
  const std::vector<byte>& base_checkpoint, const std::vector<byte>& target_checkpoint,
  const std::vector<byte>& plan, const std::vector<std::vector<byte>>& extent,
  const std::vector<std::vector<byte>>& base_allocation,
  const std::vector<std::vector<byte>>& target_allocation, u64 maximum_input_image_bytes,
  const std::vector<std::vector<byte>>& control_bundle = {},
  const std::vector<std::vector<byte>>& base_inventory = {}) noexcept;
} // namespace scratchbird::storage::database

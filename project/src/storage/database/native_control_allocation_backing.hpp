// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "native_management_control_allocation.hpp"
#include <memory_resource>
namespace scratchbird::storage::database::detail {
// Internal composition using an already admitted input-disjoint source resource.
// Full canonical validation is shared; this is no independent grant or authority.
NativeManagementControlAllocationError ValidateNativeManagementControlAllocationBacked(
  const NativeManagementControlAllocationInputs&,std::pmr::memory_resource&) noexcept;
}

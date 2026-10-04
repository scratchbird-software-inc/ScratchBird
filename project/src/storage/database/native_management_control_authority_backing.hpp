// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "native_management_control_authority.hpp"
#include <memory_resource>
namespace scratchbird::storage::database::detail {
// Complete internally composed inspection, not a selected-source/memory grant.
// Batches and admitted input-disjoint backing survive all enclosing fences.
NativeManagementControlAuthorityDeviceRead ReadNativeManagementControlAuthorityBacked(
  const Uuid&,std::span<const disk::NativeFilespaceDevice>,const Uuid&,u64,
  NativeManagementHistoryReadContext,const NativeManagementCheckpointAnchor*,
  std::span<const NativeManagementHistoricalPageZero>,
  std::span<disk::FileDevice::ReadLatencyBatch* const>,std::pmr::memory_resource&) noexcept;
}

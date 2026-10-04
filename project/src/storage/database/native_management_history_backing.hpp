// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "native_management_history.hpp"
#include <memory_resource>
namespace scratchbird::storage::database::detail {
// Complete history, internally composing already admitted input-disjoint
// backing and exact-device batches that survive ALL enclosing fences.
// No independent source, memory, recovery or execution authority is granted.
NativeManagementHistoryDeviceRead ReadNativeManagementHistoryBacked(
  const Uuid&,std::span<const disk::NativeFilespaceDevice>,const Uuid&,u64,
  NativeManagementHistoryReadContext,const NativeManagementCheckpointAnchor*,
  std::span<const NativeManagementHistoricalPageZero>,
  std::span<disk::FileDevice::ReadLatencyBatch* const>,std::pmr::memory_resource&) noexcept;
} // namespace scratchbird::storage::database::detail

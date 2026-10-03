// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "transaction_inventory_page.hpp"
#include <memory_resource>
namespace scratchbird::storage::page::detail {
// Internal complete-source composition. All backing is already admitted and
// input-disjoint; exact-device batches outlive every enclosing device fence.
// This confers no memory, transaction, selection or retention authority.
NativeInventoryChainDeviceRead ReadNativeInventoryChainBacked(
    const core::platform::Uuid&,std::span<const disk::NativeFilespaceDevice>,
    const disk::FilespaceRootReference&,u64,NativeInventoryChainReadContext,
    const std::array<byte,32>*,std::span<const NativeInventoryHistoricalPageZero>,
    std::span<disk::FileDevice::ReadLatencyBatch* const>,std::pmr::memory_resource&) noexcept;
} // namespace scratchbird::storage::page::detail

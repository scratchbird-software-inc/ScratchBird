// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "native_allocation_map.hpp"
#include <memory_resource>

namespace scratchbird::storage::page::detail {
// Internal composition with an already admitted complete source resource. The
// caller excludes every input/device/batch from its backing and retains the
// exact-device batch beyond ALL enclosing fences. Reuses the entire chain
// reader, not a single-image shortcut. This supplies no memory or reuse grant.
NativeAllocationChainDeviceRead ReadNativeAllocationChainBacked(
    disk::FileDevice&,const disk::FilespaceBootstrapBinding&,u64 budget,
    NativeAllocationChainReadContext,const disk::FilespaceRootReference*,
    const std::array<byte,32>*,std::span<const byte> historical,
    disk::FileDevice::ReadLatencyBatch&,std::pmr::memory_resource&) noexcept;
} // namespace scratchbird::storage::page::detail

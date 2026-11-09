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
// The bootstrap read only (W4d-3b: the selected and historical reads were
// removed with the sealed family).
NativeAllocationChainDeviceRead ReadNativeAllocationChainBacked(
    disk::FileDevice&,const disk::FilespaceBootstrapBinding&,u64 budget,
    disk::FileDevice::ReadLatencyBatch&,std::pmr::memory_resource&) noexcept;
} // namespace scratchbird::storage::page::detail

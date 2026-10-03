// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "native_filespace_directory.hpp"
#include <memory_resource>
namespace scratchbird::storage::page::detail {
// Internal complete-source composition. Caller owns admitted input-disjoint
// backing and exact-device batches surviving all enclosing ordered fences.
// No runtime admission, attachment, selection or memory grant is conferred.
NativeDirectoryChainDeviceRead ReadNativeDirectoryChainBacked(
    const Uuid&,std::span<const disk::NativeFilespaceDevice>,const disk::FilespaceRootReference&,
    u64,NativeDirectoryChainReadContext,const std::array<byte,32>*,
    std::span<const NativeHistoricalFilespaceImageView>,
    std::span<disk::FileDevice::ReadLatencyBatch* const>,std::pmr::memory_resource&) noexcept;
} // namespace scratchbird::storage::page::detail

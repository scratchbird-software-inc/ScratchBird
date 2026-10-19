// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "database_dirty_manifest.hpp"
#include <memory_resource>
namespace scratchbird::storage::database::detail {
// Whole source backing and exact-device observations outlive every enclosing fence.
NativeCurrentCheckpointAllocationDeviceRead VerifyCurrentNativeCheckpointAllocationBacked(
 const core::platform::Uuid&,std::span<const disk::NativeFilespaceDevice>,const disk::FilespaceRootReference&,
 u64,std::span<disk::FileDevice::ReadLatencyBatch* const>,std::pmr::memory_resource&) noexcept;
NativeCurrentCheckpointDirectoryDeviceRead VerifyCurrentNativeCheckpointDirectoryBacked(
 const core::platform::Uuid&,std::span<const disk::NativeFilespaceDevice>,const disk::FilespaceRootReference&,
 u64,std::span<disk::FileDevice::ReadLatencyBatch* const>,std::pmr::memory_resource&) noexcept;
}

namespace scratchbird::storage::database {
struct NativeBoundCheckpointSelectionView;
namespace detail {
// Internal data validation under the same continuously held device guards as
// the real completed selection read. Reuses only that checked inventory;
// directory pages, creator proof and all bindings are still read/validated.
// Budget/result charge additional image work only; the caller retains and
// charges the selection once. This cannot construct a source lease or grant.
NativeCurrentCheckpointDirectoryDeviceRead VerifyRetainedNativeCheckpointDirectoryBacked(
 const core::platform::Uuid&,std::span<const disk::NativeFilespaceDevice>,const disk::FilespaceRootReference&,
 const NativeBoundCheckpointSelectionView&,u64,std::span<disk::FileDevice::ReadLatencyBatch* const>,std::pmr::memory_resource&) noexcept;
}
}

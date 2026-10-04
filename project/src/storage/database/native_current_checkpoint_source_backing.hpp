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

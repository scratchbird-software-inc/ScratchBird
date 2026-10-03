// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "native_checkpoint_selection.hpp"
#include <memory_resource>
namespace scratchbird::storage::database::detail {
// Complete shared source composition. The owner admits backing before every
// enclosing fence and retains exact-device batches beyond those fences.
NativeBoundCheckpointSelectionDeviceRead ReadNativeBoundCheckpointSelectionBacked(
 const Uuid&,std::span<const disk::NativeFilespaceDevice>,const Uuid&,u64,
 std::span<disk::FileDevice::ReadLatencyBatch* const>,std::pmr::memory_resource&) noexcept;
} // namespace scratchbird::storage::database::detail

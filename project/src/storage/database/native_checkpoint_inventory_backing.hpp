// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "database_dirty_manifest.hpp"
#include <memory_resource>
namespace scratchbird::storage::database::detail {
// Internal whole-source composition. Admitted input-disjoint backing and
// exact-device batches must survive all enclosing source fences.
NativeCheckpointRootDeviceRead ReadNativeCheckpointRootBacked(
  disk::FileDevice&,const core::platform::Uuid&,const disk::FilespaceRootReference&,
  disk::FileDevice::ReadLatencyBatch&,std::pmr::memory_resource&) noexcept;
NativeCheckpointInventoryDeviceRead VerifyNativeCheckpointInventoryBacked(
  const core::platform::Uuid&,std::span<const disk::NativeFilespaceDevice>,
  const disk::FilespaceRootReference&,u64,
  std::span<disk::FileDevice::ReadLatencyBatch* const>,std::pmr::memory_resource&) noexcept;
}

// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "native_inventory_publication_delta.hpp"
#include <memory_resource>

namespace scratchbird::storage::database::detail {
// Internal complete-image composition: the owning source has already admitted
// input-disjoint resource backing. No heap fallback or independent claimed grant.
NativeInventoryDeltaViewResult ValidateNativeInventoryPublicationDeltaBacked(
  const core::platform::Uuid&,const NativeCheckpointRootReference&,
  const NativeCheckpointRootReference&,core::platform::u64,
  const std::span<const std::span<const core::platform::byte>>&,
  const std::span<const std::span<const core::platform::byte>>&,
  core::platform::u64,std::pmr::memory_resource&) noexcept;
}

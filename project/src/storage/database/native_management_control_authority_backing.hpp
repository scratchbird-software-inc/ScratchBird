// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "native_management_control_authority.hpp"
#include <memory_resource>
namespace scratchbird::storage::database::detail {
// Complete internally composed inspection, not a selected-source/memory grant.
// Batches and admitted input-disjoint backing survive all enclosing fences.
NativeManagementControlAuthorityDeviceRead ReadNativeManagementControlAuthorityBacked(
  const Uuid&,std::span<const disk::NativeFilespaceDevice>,const Uuid&,u64,
  NativeManagementHistoryReadContext,const NativeManagementCheckpointAnchor*,
  std::span<const NativeManagementHistoricalPageZero>,
  std::span<disk::FileDevice::ReadLatencyBatch* const>,std::pmr::memory_resource&) noexcept;
// SEARCH_KEY: MEM-CONTROL-AUTHORITY-READ-ONCE (2026-11-04). One selected
// control authority of one filespace, shared by the checks of one bound
// selection. That selection holds every device operation guard for its whole
// read, so a second read of the same selected authority sees the same bytes
// and proves nothing more; the history replay is O(management entries) and
// was taken three times per selection (the checkpoint's creator, the
// predecessor's creator, the role-16 control proof). Memory only: every use
// still charges the view's verified image bytes against its own budget, and
// refuses when they no longer fit, as a fresh read would.
struct NativeSharedControlAuthority {
  Uuid filespace;
  std::optional<NativeManagementControlAuthorityView> view;
};
}

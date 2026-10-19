// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "native_selected_checkpoint_readonly.hpp"
#include "native_publication_plan.hpp"

namespace scratchbird::storage::database {
struct NativeInventoryGraphInputs {
  const NativeReadBoundCheckpointSelection& source;
  const NativeReadDirectoryChain& directory;
  const disk::FilespacePageZeroView& primary;
  const NativePublicationSnapshot& reservation;
  const NativeManagementOperationView& record;
  const page::NativeTransactionInventoryConstView& inventory;
  // Slots excluded by the owning writer's actual zero-page probes. This is
  // planning input, never proof that the remaining physical slots are empty.
  std::span<const u64> excluded_pages;
  u64 maximum_image_bytes=0;
};
struct NativeInventoryGraphView {
  NativePublicationPlan plan;
  std::span<const byte> checkpoint,plan_image;
  std::span<const std::span<const byte>> extent,bundle,allocation,inventory,directory;
};
struct NativeInventoryGraphBuildResult {
  NativePublicationError error=NativePublicationError::invalid_request;
  NativeInventoryGraphView graph;
  std::size_t backing_bytes_used=0;
  bool ok() const noexcept{return error==NativePublicationError::none;}
};
// Complete immutable profile-2 graph construction in caller backing. No heap
// fallback, file I/O, space reservation, history append permission, publication
// or transaction finality. The owner must independently bind the source and
// reservation, validate history, probe every destination, install and select.
// Backing must exclude every input descriptor and its borrowed field storage.
NativeInventoryGraphBuildResult BuildNativeInventoryGraphInto(
  const NativeInventoryGraphInputs&,core::uuid::StandaloneUuidV7Issuer&,
  std::span<byte> backing) noexcept;
} // namespace scratchbird::storage::database

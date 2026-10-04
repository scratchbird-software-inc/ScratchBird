// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "database_dirty_manifest.hpp"

namespace scratchbird::storage::database {
enum class NativeInventoryDeltaError {
  none, invalid_request, invalid_image, binding_mismatch, chain_mismatch,
  invalid_evolution, starting_allocation_required, encrypted_requires_authority,
  hash_failure, resource_exhausted, invalid_backing
};
struct NativeInventoryEntryDifference {
  std::optional<transaction::mga::TransactionInventoryEntry> before, after;
};
struct NativeInventoryPublicationDelta {
  transaction::mga::LocalTransactionInventory before, after;
  std::vector<NativeInventoryEntryDifference> differences;
  bool cluster_difference = false;
  core::platform::u64 verified_image_bytes = 0;
};
struct NativeInventoryDeltaResult {
  NativeInventoryDeltaError error = NativeInventoryDeltaError::invalid_request;
  std::optional<NativeInventoryPublicationDelta> delta;
  bool ok() const noexcept { return error == NativeInventoryDeltaError::none && delta.has_value(); }
};
// Complete image consistency only. Differences explicitly retain removed and
// changed entries for the owning execution/retention/cluster authority. This
// does not read selected files, allocate, publish, authenticate or begin a TX.
NativeInventoryDeltaResult ValidateNativeInventoryPublicationDelta(
  const core::platform::Uuid& database,
  const NativeCheckpointRootReference& before_root,
  const NativeCheckpointRootReference& after_root,
  core::platform::u64 reserved_publication_generation,
  const std::vector<std::vector<core::platform::byte>>& before_images,
  const std::vector<std::vector<core::platform::byte>>& after_images,
  core::platform::u64 maximum_verification_image_bytes) noexcept;

struct NativeInventoryPublicationDeltaView {
  page::NativeTransactionInventoryView before,after;
  std::span<const NativeInventoryEntryDifference> differences;
  bool cluster_difference=false;
  core::platform::u64 verified_image_bytes=0;
  std::size_t backing_bytes_used=0;
};
struct NativeInventoryDeltaViewResult {
  NativeInventoryDeltaError error=NativeInventoryDeltaError::invalid_request;
  std::optional<NativeInventoryPublicationDeltaView> delta;
  bool ok() const noexcept{return error==NativeInventoryDeltaError::none&&delta.has_value();}
};
// Complete immutable image/chain/evolution/difference validation in caller backing.
// Input descriptors and complete image ranges must be disjoint from backing.
// Result inventories and differences outlive inputs but not backing. No publication
// CAS base, live source, transition, cluster or retention authority is issued.
NativeInventoryDeltaViewResult ValidateNativeInventoryPublicationDeltaInto(
  const core::platform::Uuid& database,
  const NativeCheckpointRootReference& before_root,
  const NativeCheckpointRootReference& after_root,
  core::platform::u64 reserved_publication_generation,
  std::span<const std::span<const core::platform::byte>> before_images,
  std::span<const std::span<const core::platform::byte>> after_images,
  core::platform::u64 maximum_verification_image_bytes,
  std::span<core::platform::byte> backing) noexcept;
} // namespace scratchbird::storage::database

// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "database_dirty_manifest.hpp"

namespace scratchbird::storage::database {
enum class NativeInventoryDeltaError {
  none, invalid_request, invalid_image, binding_mismatch, chain_mismatch,
  invalid_evolution, starting_allocation_required, encrypted_requires_authority,
  hash_failure, resource_exhausted
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
} // namespace scratchbird::storage::database

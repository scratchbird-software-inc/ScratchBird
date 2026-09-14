// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "native_filespace_initialization.hpp"
#include "native_checkpoint_selection.hpp"

namespace scratchbird::storage::database {
enum class NativeCreationWorkspaceError {
  none, invalid_request, invalid_device, device_not_empty, invalid_capacity,
  resource_exhausted, identity_failure, encoding_failure, hash_failure,
  io_failure, readback_mismatch, graph_failure, cluster_requires_authority,
  encrypted_requires_crypto_authority
};
struct NativeCreationWorkspaceReceipt {
  core::platform::Uuid database_uuid, filespace_uuid, operation_uuid;
  core::platform::Uuid page_zero_uuid, timeline_uuid, publication_uuid;
  transaction::mga::TransactionIdentity construction_transaction;
  disk::FilespaceRootReference checkpoint;
  std::array<core::platform::byte,32> checkpoint_sha256{};
  std::array<page::NativeCatalogRootReference,6> relations;
  core::platform::u64 total_pages=0, free_pages=0, map_pages=0;
};
struct NativeCreationWorkspaceResult {
  NativeCreationWorkspaceError error=NativeCreationWorkspaceError::invalid_request;
  NativeCheckpointError checkpoint_error=NativeCheckpointError::none;
  NativeCheckpointSelectionError selection_error=NativeCheckpointSelectionError::none;
  std::optional<NativeCreationWorkspaceReceipt> receipt;
  bool ok() const noexcept {return error==NativeCreationWorkspaceError::none&&receipt.has_value();}
};
// Real local control-graph genesis on an empty owned primary. The only
// committed transaction certifies construction, not populated catalogs or
// CREATE DATABASE success. Remains initializing / creating / write-fenced.
// Never opens, closes, deletes or repairs a caller's device. Failure can leave
// owned partial bytes; a subsequent call refuses a nonempty device.
NativeCreationWorkspaceResult InitializeNativeCreationWorkspaceOnOpenDevice(
    disk::FileDevice&,const NativeFilespaceInitializationRequest&,
    core::platform::u64 maximum_retained_image_bytes) noexcept;
}  // namespace scratchbird::storage::database

// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "native_creation_workspace.hpp"

namespace scratchbird::storage::database {
enum class NativeCreationRecoveryError {
  none, invalid_request, invalid_device, resource_exhausted, graph_failure,
  selector_ambiguity, hash_failure, encoding_failure, io_failure,
  preimage_changed, readback_mismatch, admission_failure,
  cluster_requires_authority, encrypted_requires_crypto_authority
};
struct NativeCreationRecoveryResult {
  NativeCreationRecoveryError error=NativeCreationRecoveryError::invalid_request;
  std::optional<NativeCreationWorkspaceReceipt> receipt;
  unsigned repaired_slots=0;
  bool ok() const noexcept {return error==NativeCreationRecoveryError::none&&receipt.has_value();}
};
// Actual initial-creation recovery only, not general current-root selection or
// serving. Requires complete original genesis graph and a bound surviving slot.
// Never opens/closes/truncates/deletes the borrowed device or issues identities.
NativeCreationRecoveryResult RecoverNativeCreationWorkspaceSelectionOnOpenDevice(
    disk::FileDevice&,const disk::FilespaceBootstrapBinding&,
    const core::platform::Uuid& creation_operation_uuid,
    core::platform::u64 maximum_retained_image_bytes) noexcept;
}  // namespace scratchbird::storage::database

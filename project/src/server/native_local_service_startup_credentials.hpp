// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "native_local_service_startup_selection.hpp"

namespace scratchbird::server {

enum class NativeLocalServiceStartupCredentialsError {
  none, invalid_request, cancelled, credential_source, checkpoint_source,
  credential_verification, declaration_selection
};

struct NativeLocalServiceStartupCredentialsRequest {
  NativeRuntimePrincipalCredentialRequest credential;
  StartupCredentialSource::Request source;
  // Bounds of the read through page zero's fixed roots (named for the
  // checkpoint layer it replaced; renamed when that layer is deleted).
  storage::database::NativeSelectedCheckpointMemoryLimits checkpoint_limits;
};

struct NativeLocalServiceStartupCredentialsResult {
  NativeLocalServiceStartupCredentialsError error =
      NativeLocalServiceStartupCredentialsError::invalid_request;
  StartupCredentialSource::Outcome credential_source;
  storage::database::NativeOwnedSourceDiagnostics checkpoint_source;
  storage::database::NativeWriterError writer_error =
      storage::database::NativeWriterError::invalid_operation;
  // Destruction releases comparison/device guards before the enclosing writer
  // reservation. The original route owner remains pinned by both handles.
  NativeHostedDatabaseCustody::Operation operation;
  std::optional<NativeRuntimePrincipalCredentialResult> comparison;
  NativeLocalServiceStartupSelectionResult selection;

  bool ok() const noexcept {
    return error == NativeLocalServiceStartupCredentialsError::none &&
        comparison && comparison->ok() &&
        (bool(operation) || comparison->lease->has_retained_operation()) && selection.ok();
  }
};

// Compose the startup credential prerequisite against the actual hosted writer.
// The caller has already provisioned/published the protected source and admitted
// the catalog/checkpoint/derived backing. Managers, source and crypto scope must
// outlive this acquiring-thread result. This does not activate a provider,
// authenticate a session, admit BEGIN or publish readiness.
//
// Take exclusive custody of the operation before allocation/provider callbacks.
// Acquire protected material BEFORE taking any device guard: importing it while
// a selected checkpoint is held can call a governor under storage locks. The
// subsequent comparison binds the actual source frame to the actual committed
// catalog, retaining both fences on success. Every refusal returns the operation
// so the coordinator can reconcile/retire its original ownership explicitly.
inline NativeLocalServiceStartupCredentialsResult ReadNativeLocalServiceStartupCredentials(
    const NativeLocalServiceStartupCredentialsRequest request,
    NativeHostedDatabaseCustody::Operation operation,
    NativeRuntimePrincipalCredentialPreparedMemory& catalog_workspace,
    storage::database::NativeStorageMemory& checkpoint_memory,
    const storage::database::NativeStorageMemoryBinding checkpoint_binding,
    StartupCredentialSource& credential_source,
    core::memory::MemoryManager& manager,
    const core::memory::MemoryTag& credential_tag,
    const core::memory::ScopedProtectedBuffer& derived) {
  using E = NativeLocalServiceStartupCredentialsError;
  NativeLocalServiceStartupCredentialsResult out;
  out.operation = std::move(operation);
  const auto cancelled = [&] {
    return request.credential.stop.stop_requested() || request.source.stop.stop_requested();
  };
  if (cancelled()) { out.error = E::cancelled; return out; }
  if (!out.operation || !catalog_workspace || !request.source.deadline ||
      !request.source.generation ||
      !request.checkpoint_limits.maximum_verification_image_bytes ||
      !request.checkpoint_limits.maximum_backing_bytes ||
      request.credential.security.database_uuid != checkpoint_binding.database_uuid ||
      credential_tag.binary_ownership[core::memory::MemoryBinaryScopeKind::database] !=
          request.credential.security.database_uuid.bytes)
    return out;

  auto material = credential_source.Acquire(request.source, manager, credential_tag);
  out.credential_source = material.outcome;
  if (!material.outcome.ok() || !material.lease) {
    out.error = E::credential_source;
    return out;
  }
  if (cancelled()) { out.error = E::cancelled; return out; }

  const auto compare = [&](auto source) {
    out.writer_error = source.writer_error;
    static_cast<storage::database::NativeOwnedSourceDiagnostics&>(out.checkpoint_source) =
        std::move(static_cast<storage::database::NativeOwnedSourceDiagnostics&>(source));
    if (!source.ok()) { out.error = E::checkpoint_source; return; }
    if (cancelled()) { out.error = E::cancelled; return; }
    out.comparison.emplace(VerifyNativeRuntimePrincipalCredential(
        request.credential, source.lease, catalog_workspace, *material.lease, derived));
    if (!out.comparison->ok()) { out.error = E::credential_verification; return; }
    out.selection = SelectNativeLocalServiceStartupDeclarations(*out.comparison->lease);
    if (!out.selection.ok()) { out.error = E::declaration_selection; return; }
    out.error = E::none;
  };
  // SB_NATIVE_FIXED_ROOT_READ: page zero's fixed roots (drift removal C3c).
  compare(out.operation.ReadFixedRootWithMemory(
      {request.checkpoint_limits.maximum_verification_image_bytes, request.checkpoint_limits.maximum_backing_bytes},
      checkpoint_memory, checkpoint_binding));
  return out;
}

} // namespace scratchbird::server

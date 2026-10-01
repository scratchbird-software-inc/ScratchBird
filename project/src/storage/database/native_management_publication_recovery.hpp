// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "native_publication_coordinator.hpp"

namespace scratchbird::storage::database {
// Actual-file forward recovery of an installed management publication.
// Requires exact durable request, complete immutable graph and original slot
// allocations. No fabricated lease, survivor-based serving, new identity,
// authentication/effect completion or SQL receipt is granted.
// Growth profile 4 additionally requires already completed physical growth and
// the exact installed after page-zero body. This selector-only primitive never
// extends a device, reserves physical storage, or repairs a torn growth body.
NativePublicationInspection RecoverNativeManagementCheckpointPublicationOnOpenDevices(
  const Uuid& database,const std::vector<disk::NativeFilespaceDevice>&,
  const Uuid& primary,const Uuid& expected_attempt,
  const NativePublicationIntent& expected_intent,u64 maximum_verification_image_bytes) noexcept;

enum class NativeGrowthRecoveryPhase {
  request, anchor, reconstruction, base_graph, allocation_delta,
  installed_graph, physical, page_zero, selection, complete
};
struct NativeGrowthRecoveryResult {
  NativePublicationError error=NativePublicationError::invalid_request;
  NativeGrowthRecoveryPhase phase=NativeGrowthRecoveryPhase::request;
  Uuid request_uuid,operation_uuid,publication_attempt_uuid;
  bool original_graph_verified=false,already_selected=false;
  // Observation before this invocation, not attribution to a particular call.
  u64 observed_extent_bytes=0,original_extent_bytes=0,target_extent_bytes=0;
  bool physical_attempted=false,physical_sync_attempted=false;
  std::optional<disk::PreallocateExtentResult> physical;
  std::optional<disk::IoResult> physical_sync;
  std::optional<disk::FilespacePageZeroBodyResult> page_zero;
  NativePublicationInspection selection;
  bool ok() const noexcept {
    return error==NativePublicationError::none&&original_graph_verified&&selection.ok()&&
      (already_selected?(!physical_attempted&&!physical&&!physical_sync_attempted&&!physical_sync&&!page_zero):
        (physical_attempted&&physical&&physical->ok()&&physical_sync_attempted&&physical_sync&&physical_sync->ok()&&page_zero&&page_zero->ok()));
  }
};
// Reconcile the actual original anchor and immutable before/after graph before
// retrying its exact physical range or repairing its mutable page-zero body.
// Requires completely installed candidate metadata; pre-extension interrupted
// staging is resumed through the original lease, not replaced by this API.
// No new identity, security/policy grant or MGA operation completion is issued.
NativeGrowthRecoveryResult RecoverNativeFilespaceGrowthOnOpenDevices(
  const Uuid& database,const std::vector<disk::NativeFilespaceDevice>&,
  const Uuid& primary,const Uuid& expected_attempt,
  const NativeManagementOperation&,u64 maximum_verification_image_bytes) noexcept;
} // namespace scratchbird::storage::database

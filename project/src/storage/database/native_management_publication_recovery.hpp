// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "native_publication_coordinator.hpp"

namespace scratchbird::storage::database {
// Actual-file forward recovery of an installed V4 management publication.
// Requires exact durable request, complete immutable graph and original slot
// allocations. No fabricated lease, survivor-based serving, new identity,
// authentication/effect completion or SQL receipt is granted.
NativePublicationInspection RecoverNativeManagementCheckpointPublicationOnOpenDevices(
  const Uuid& database,const std::vector<disk::NativeFilespaceDevice>&,
  const Uuid& primary,const Uuid& expected_attempt,
  const NativePublicationIntent& expected_intent,u64 maximum_verification_image_bytes) noexcept;
} // namespace scratchbird::storage::database

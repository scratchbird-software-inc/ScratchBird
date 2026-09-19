// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "native_management_history.hpp"

namespace scratchbird::storage::database {
enum class NativeManagementControlAuthorityError {
  none,invalid_request,history_failure,checkpoint_failure,inventory_failure,
  allocation_failure,binding_mismatch,creator_mismatch,resource_exhausted,
  hash_failure,io_failure,encrypted_requires_authority,cluster_requires_authority
};
struct NativeManagementPublishedCheckpoint {
  disk::NativePageReference page;
  Uuid object_uuid;
  std::array<byte,32> sha256{};
};
struct NativeManagementControlAuthority {
  NativeManagementControlAuthorityError error=NativeManagementControlAuthorityError::invalid_request;
  std::optional<NativeCheckpointSelection> selection;
  std::map<Uuid,NativeManagementPublishedCheckpoint> publications;
  std::map<std::pair<Uuid,u64>,page::NativeAllocationRecord> allocations;
  u64 verified_image_bytes=0;
  bool ok() const noexcept {return error==NativeManagementControlAuthorityError::none&&selection.has_value();}
};
// Actual selected metadata ancestry and exact original control allocations.
// No authentication, user-operation completion, publication or SQL receipt.
NativeManagementControlAuthority ReadNativeManagementControlAuthorityFromOpenDevices(
  const Uuid& database,const std::vector<disk::NativeFilespaceDevice>&,
  const Uuid& primary,u64 maximum_verification_image_bytes) noexcept;
bool MatchesNativeManagementPublishedCheckpoint(const NativeManagementControlAuthority&,
  const NativeCheckpointRoot&,const std::array<byte,32>& stored_sha256) noexcept;
bool MatchesNativeManagementControlAllocation(const NativeManagementControlAuthority&,
  const Uuid& filespace,const page::NativeAllocationRecord&,page::NativeAllocationState) noexcept;
bool MatchesNativeManagementControlMap(const NativeManagementControlAuthority&,
  const page::NativeAllocationMap&) noexcept;
} // namespace scratchbird::storage::database

// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "filespace_page_zero.hpp"
#include "native_management_operation.hpp"
#include "native_filespace_capacity.hpp"

namespace scratchbird::storage::database {
enum class NativeStorageAction : u16 { physical_growth=1, page_preallocation=2 };
enum class NativeStorageIntentState : u16 { free=0, preallocated=7 };
// STORAGE-NATIVE-ACTION-INTENT-001. Immutable observation/intent, NOT authority.
// Dispatch lease/deadline/cancellation state belongs to the runtime separately.
struct NativeStorageActionIntent {
  NativeStorageAction action=NativeStorageAction::physical_growth;
  Uuid request_uuid, operation_uuid, database_uuid, filespace_uuid, locator_uuid;
  Uuid page_zero_uuid, page_size_profile_uuid, policy_snapshot_uuid;
  Uuid storage_profile_uuid, initiator_uuid, request_context_uuid;
  Uuid policy_uuid, policy_version_uuid, attachment_uuid, attachment_version_uuid;
  Uuid storage_profile_version_uuid;
  // Required only for preallocated pages, including growth into that state.
  // The owner/type are immutable request material, not an execution-time default.
  Uuid allocation_owner_uuid;
  u32 allocation_page_type=0;
  u64 attachment_generation=0, storage_profile_generation=0;
  disk::FilespaceRootReference checkpoint, allocation_root;
  std::array<byte,32> checkpoint_sha256{}, allocation_sha256{};
  u64 checkpoint_generation=0, checkpoint_root_set_generation=0, directory_generation=0;
  u64 filespace_root_set_generation=0, page_zero_generation=0, map_generation=0;
  u64 capacity_generation=0, catalog_generation=0, policy_generation=0, security_generation=0;
  // Configuration-root epoch is not the selected policy's own generation.
  u64 configuration_generation=0;
  u64 current_total_pages=0, first_page=0, page_count=0, maximum_total_pages=0;
  u64 maximum_work_bytes=0, maximum_retained_image_bytes=0;
  u32 page_size_bytes=0;
  NativeStorageIntentState intended_state=NativeStorageIntentState::free;
};
inline constexpr u64 kNativeStorageActionIntentBytes=768;
enum class NativeStorageIntentError {
  none, invalid_header, invalid_identity, invalid_profile, invalid_reference,
  invalid_range, invalid_integrity, operation_failure, binding_mismatch,
  resource_exhausted, hash_failure
};
struct NativeStorageIntentImage {
  NativeStorageIntentError error=NativeStorageIntentError::invalid_header;
  NativeManagementOperationError operation_error=NativeManagementOperationError::none;
  std::optional<NativeStorageActionIntent> intent;
  std::vector<byte> bytes;
  bool ok() const noexcept {return error==NativeStorageIntentError::none&&intent.has_value();}
};
NativeStorageIntentError ValidateNativeStorageActionIntent(const NativeStorageActionIntent&) noexcept;
NativeStorageIntentImage EncodeNativeStorageActionIntent(const NativeStorageActionIntent&,u64 maximum_encoded_bytes) noexcept;
NativeStorageIntentImage DecodeNativeStorageActionIntent(const std::vector<byte>&,u64 maximum_encoded_bytes) noexcept;
// Validates record shape/hash and exact overlapping identities/generations.
// Does NOT select/admit its descriptor, policy, resources, security or effects.
NativeStorageIntentImage ReadNativeStorageActionIntentFromOperation(
    const NativeManagementOperation&,u64 maximum_encoded_bytes) noexcept;

struct NativeStorageIntentViewImage {
  NativeStorageIntentError error=NativeStorageIntentError::invalid_header;
  NativeManagementOperationError operation_error=NativeManagementOperationError::none;
  std::optional<NativeStorageActionIntent> intent;
  std::span<const byte> bytes;
  bool ok() const noexcept{return error==NativeStorageIntentError::none&&intent.has_value();}
};
// Full immutable intent admission without an owned byte copy or rendered hash
// diagnostic. Intent is a fixed native value; bytes borrow the complete input.
// Operation form freshly validates the whole record using disjoint identity
// scratch and the same exact overlapping bindings. Neither grants execution,
// selected policy/security/resource authority or completion.
NativeStorageIntentViewImage DecodeNativeStorageActionIntentView(std::span<const byte>,u64) noexcept;
NativeStorageIntentViewImage ReadNativeStorageActionIntentFromOperationView(
  const NativeManagementOperationView&,u64,std::span<Uuid> identities) noexcept;

enum class NativeStorageCapacityCheckError {
  none, invalid_intent, capacity_failure, identity_mismatch, profile_mismatch,
  checkpoint_mismatch, allocation_mismatch, generation_mismatch, capacity_mismatch
};
struct NativeStorageCapacityCheck {
  NativeStorageCapacityCheckError error=NativeStorageCapacityCheckError::invalid_intent;
  NativeStorageIntentError intent_error=NativeStorageIntentError::none;
  // On failed current reads retain the original nested reader errors; on a
  // mismatch do not expose a successful observation as a matched proposal.
  NativeFilespaceCapacityResult capacity;
  bool ok() const noexcept {return error==NativeStorageCapacityCheckError::none&&capacity.ok();}
};
// Pure comparison only; callers cannot turn a constructed observation into a
// grant. Live consumers obtain the observation from the retained-device reader.
NativeStorageCapacityCheckError MatchNativeStorageIntentCapacityObservation(
    const NativeStorageActionIntent&,const NativeFilespaceCapacityObservation&) noexcept;
// Fresh actual native read and complete capacity-field matching only. No path
// opens, writes, policy/security checks, resource grant, free-range admission or
// execution lease. The result is an observation, not authority to mutate later.
NativeStorageCapacityCheck CheckNativeStorageIntentCapacityFromOpenDevices(
    const NativeStorageActionIntent&,const std::vector<disk::NativeFilespaceDevice>&,
    u64 maximum_retained_image_bytes) noexcept;
} // namespace scratchbird::storage::database

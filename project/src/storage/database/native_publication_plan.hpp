// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "native_publication_coordinator.hpp"
#include "native_management_extent.hpp"
#include "native_management_control_bundle.hpp"

namespace scratchbird::storage::database {
struct NativePublicationPlan {
  disk::NativeCommonPageHeader header;
  Uuid object_uuid, bootstrap_uuid, timeline_uuid, operation_uuid, security_snapshot_uuid;
  NativePublicationIntent intent;
  std::array<byte,32> reservation_state_sha256{}, base_checkpoint_sha256{}, target_graph_sha256{}, previous_plan_sha256{};
  u64 reserved_generation=0, base_checkpoint_generation=0, base_root_set_generation=0, target_root_set_generation=0;
  disk::NativePageReference base_checkpoint, target_checkpoint;
  Uuid base_checkpoint_object_uuid, target_checkpoint_object_uuid;
  std::optional<disk::NativePageReference> previous_plan;
  Uuid previous_plan_object_uuid;
  u64 catalog_generation=0, configuration_generation=0, security_generation=0;
  core::platform::u32 generation_guard_flags=7;
  std::optional<NativeManagementExtentRoot> management_extent;
  std::optional<NativeManagementControlBundleRoot> control_bundle;
  // Version4 binds the actual selector sequence, not the reserved checkpoint
  // watermark. Absence preserves the earlier plan image definitions.
  std::optional<u64> base_selection_generation;
  // A nonzero intent.recovery_profile selects version5 and requires the
  // complete metadata-only extent/bundle/sequence contract.
};
enum class NativePublicationPlanError {
  none, invalid_header, invalid_identity, invalid_family, invalid_reference,
  invalid_integrity, invalid_checkpoint, binding_mismatch, hash_failure, resource_exhausted,
  cluster_requires_authority
};
struct NativePublicationPlanImage {
  NativePublicationPlanError error=NativePublicationPlanError::invalid_family;
  std::optional<NativePublicationPlan> plan;
  // Reference digest of all stored bytes, including the embedded image seal.
  std::array<byte,32> sha256{};
  std::vector<byte> bytes;
  bool ok() const noexcept{return error==NativePublicationPlanError::none&&plan.has_value();}
};
struct NativePublicationGraphDigest {
  NativePublicationPlanError error=NativePublicationPlanError::invalid_checkpoint;
  std::array<byte,32> sha256{};
  bool ok() const noexcept{return error==NativePublicationPlanError::none;}
};
NativePublicationPlanImage EncodeNativePublicationPlan(const NativePublicationPlan&) noexcept;
NativePublicationPlanImage DecodeNativePublicationPlan(const std::vector<byte>&) noexcept;
NativePublicationGraphDigest ComputeNativePublicationTargetGraphDigest(const std::vector<byte>& checkpoint) noexcept;
// Structural preflight against a real retained reservation only. Does not
// authenticate, allocate, write, publish a graph or complete an operation.
NativePublicationPlanError BindNativePublicationPlanToLease(const NativePublicationPlan&,
    const NativePublicationLease&,const std::vector<byte>& checkpoint) noexcept;
NativePublicationPlanError BindNativePublicationPlanToManagementExtent(const NativePublicationPlan&,
    const std::vector<std::vector<byte>>&,u64 budget) noexcept;
// Complete canonical record digest plus provenance binding, not authorization.
NativePublicationPlanError BindNativePublicationPlanToManagementRecord(const NativePublicationPlan&,
    const NativeManagementOperation&) noexcept;
} // namespace scratchbird::storage::database

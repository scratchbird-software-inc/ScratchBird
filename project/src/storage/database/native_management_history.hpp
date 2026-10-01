// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "native_publication_plan.hpp"
#include <map>

namespace scratchbird::storage::database {
enum class NativeManagementHistoryError {
  none,invalid_request,bootstrap_failure,selection_failure,checkpoint_failure,
  plan_failure,extent_failure,history_mismatch,transition_failure,idempotency_conflict,
  resource_exhausted,hash_failure,io_failure,encrypted_requires_authority,
  cluster_requires_authority
};
struct NativeManagementHistoryEntry {
  NativePublicationPlan plan;
  NativeManagementOperation record;
  std::vector<disk::NativeCommonPageHeader> extent_pages;
  std::array<byte,32> plan_sha256{},checkpoint_sha256{};
  std::vector<disk::NativeCommonPageHeader> bundle_pages;
  std::vector<std::vector<byte>> control_allocation_images;
  std::vector<std::vector<byte>> control_inventory_images;
  std::vector<std::vector<byte>> control_directory_images;
  std::vector<std::vector<byte>> control_growth_images;
};
struct NativeManagementCheckpointAnchor {
  disk::NativePageReference checkpoint;
  Uuid checkpoint_object_uuid;
  std::array<byte,32> checkpoint_sha256{};
  u64 checkpoint_generation=0,root_set_generation=0;
  Uuid timeline_uuid;
  bool operator==(const NativeManagementCheckpointAnchor&) const = default;
};
struct NativeManagementGraphHistory {
  NativeManagementHistoryError error=NativeManagementHistoryError::invalid_request;
  std::optional<NativeManagementCheckpointAnchor> anchor;
  // Oldest first. Index values address this immutable successful sequence.
  std::vector<NativeManagementHistoryEntry> entries;
  std::map<Uuid,std::size_t> latest;
  std::map<std::pair<core::platform::u16,std::string>,Uuid> idempotency;
  u64 verified_image_bytes=0;
  bool ok() const noexcept{return error==NativeManagementHistoryError::none&&anchor.has_value();}
};
struct NativeManagementHistory : NativeManagementGraphHistory {
  std::optional<NativeCheckpointSelection> selection;
  bool ok() const noexcept{return NativeManagementGraphHistory::ok()&&selection.has_value();}
};
// Actual immutable graph only. The caller's anchor is not selection, a retained
// publication lease, recovery ownership, authentication or operation completion.
NativeManagementGraphHistory ReadNativeManagementGraphHistoryFromOpenDevices(
  const Uuid& database,const std::vector<disk::NativeFilespaceDevice>&,
  const Uuid& primary,const NativeManagementCheckpointAnchor&,u64 maximum_verification_image_bytes) noexcept;
// Explicit historical result contexts for older anchors or torn current bodies.
// The caller authenticates these binary image/root inputs against its original
// publication before any recovery effects. This reader grants no such authority.
NativeManagementGraphHistory ReadNativeManagementGraphHistoryAtHistoricalContextFromOpenDevices(
  const Uuid& database,const std::vector<disk::NativeFilespaceDevice>&,
  const Uuid& primary,const NativeManagementCheckpointAnchor&,
  const std::map<Uuid,std::vector<byte>>& result_page_zero_images,
  u64 maximum_verification_image_bytes) noexcept;
// Pure append validation using the same revision, semantic/idempotency and step
// identity rules as the history reader. The caller must acquire actual history;
// this grants no publication or physical effect authority.
NativeManagementHistoryError ValidateNativeManagementHistoryAppend(
  const NativeManagementGraphHistory&,const NativeManagementOperation&,
  u64 maximum_index_bytes,const std::optional<NativeStartupBinding>& startup_binding = {}) noexcept;
// Actual selected physical history and common evolution/uniqueness only.
// NOT allocation/creator outcome, kernel authentication, effects or serving.
NativeManagementHistory ReadNativeManagementHistoryFromOpenDevices(const Uuid& database,
  const std::vector<disk::NativeFilespaceDevice>&,const Uuid& primary,u64 maximum_verification_image_bytes) noexcept;
} // namespace scratchbird::storage::database

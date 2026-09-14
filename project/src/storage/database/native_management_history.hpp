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
};
struct NativeManagementHistory {
  NativeManagementHistoryError error=NativeManagementHistoryError::invalid_request;
  std::optional<NativeCheckpointSelection> selection;
  // Oldest first. Index values address this immutable successful sequence.
  std::vector<NativeManagementHistoryEntry> entries;
  std::map<Uuid,std::size_t> latest;
  std::map<std::pair<core::platform::u16,std::string>,Uuid> idempotency;
  u64 verified_image_bytes=0;
  bool ok() const noexcept{return error==NativeManagementHistoryError::none&&selection.has_value();}
};
// Actual selected physical history and common evolution/uniqueness only.
// NOT allocation/creator outcome, kernel authentication, effects or serving.
NativeManagementHistory ReadNativeManagementHistoryFromOpenDevices(const Uuid& database,
  const std::vector<disk::NativeFilespaceDevice>&,const Uuid& primary,u64 maximum_verification_image_bytes) noexcept;
} // namespace scratchbird::storage::database

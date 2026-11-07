// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "native_read_source_identity.hpp"
#include "native_system_state.hpp"
#include "engine/internal_api/transaction/native_startup_lifetime.hpp"

namespace scratchbird::engine::internal_api {
enum class NativeStartupIdentityError {
  none,not_ready,busy,cancelled,lifetime_refused,policy_refused,resource_exhausted,
  source_failure,source_mismatch,io_failure,invalid_identity,crypto_failure
};
// Actual first-stage scan observation. This is neither full verification nor
// admission: only the private startup owner can retain and consume the proof.
struct NativeStartupStorageIdentityObservation {
  storage::database::NativeReadSourceIdentity source;
  // SB_MGA_DATABASE_STATE: the database's administrative mode (SBSYS002).
  storage::database::NativeDatabaseState system_state;
  core::platform::Uuid transaction_uuid,snapshot_uuid;
  core::platform::Uuid primary_filespace_uuid,primary_page_uuid,primary_profile_uuid;
  core::platform::u64 primary_page_generation=0;
  std::array<storage::disk::FilespaceRootReference,32> primary_roots;
  std::size_t primary_root_count=0;
  core::platform::u64 filespaces=0,identity_pages=0,physical_bytes_read=0;
  std::size_t workspace_bytes=0;
};
struct NativeStartupStorageIdentityResult {
  NativeStartupIdentityError error=NativeStartupIdentityError::not_ready;
  NativeStartupLifetimeError lifetime_error=NativeStartupLifetimeError::none;
  std::optional<NativeStartupStorageIdentityObservation> observation;
  bool ok()const noexcept{return error==NativeStartupIdentityError::none&&observation.has_value();}
};
} // namespace scratchbird::engine::internal_api

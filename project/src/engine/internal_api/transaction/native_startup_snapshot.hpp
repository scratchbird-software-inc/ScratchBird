// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "engine/internal_api/security/native_runtime_security_observation.hpp"
#include "catalog_isolation_profile.hpp"
#include "native_checkpoint_selection.hpp"
#include <array>

namespace scratchbird::engine::internal_api {
enum class NativeStartupSnapshotError {
  none,not_ready,busy,cancelled,source_mismatch,policy_required,
  inventory_failure,identity_failure,crypto_failure,clock_failure,transaction_expired,snapshot_expired
};
// Descriptive copy of the actual private first-startup snapshot registration.
// It owns its small genesis exclusion arrays, so diagnostics remain safe after
// retirement. No observation or UUID can reconstruct a live visibility pin.
// This kind is maintenance_operation; its owner is the one first-open TX2.
struct NativeStartupSnapshotObservation {
  core::platform::Uuid snapshot_uuid,active_set_uuid,catalog_visibility_epoch_uuid;
  core::platform::Uuid database_uuid,engine_instance_uuid,attachment_uuid,session_uuid;
  core::platform::Uuid owning_transaction_uuid,operation_uuid,policy_snapshot_uuid,security_snapshot_uuid;
  core::platform::Uuid isolation_profile_uuid,isolation_version_uuid;
  core::platform::u64 isolation_generation=0,owning_transaction_number=0,created_at_transaction_number=0;
  core::platform::u64 created_at_micros=0,maximum_snapshot_microseconds=0;
  core::platform::u64 visible_committed_high_watermark=0,visible_commit_sequence=0;
  core::platform::u64 next_transaction_number=0,oldest_interesting=0,oldest_active=0,oldest_snapshot=0,retention_horizon=0;
  core::catalog::IsolationLimbo limbo_policy=core::catalog::IsolationLimbo::ordinary_hidden;
  std::array<core::platform::u64,2> active{},in_doubt{};
  std::size_t active_count=0,in_doubt_count=0;
  storage::database::NativeCheckpointSelection inventory_source;
  NativeRuntimeSelectedCatalogObservation catalog_source;
};
struct NativeStartupSnapshotResult {
  NativeStartupSnapshotError error=NativeStartupSnapshotError::not_ready;
  std::optional<NativeStartupSnapshotObservation> observation;
  bool ok()const noexcept{return error==NativeStartupSnapshotError::none&&observation.has_value();}
};
} // namespace scratchbird::engine::internal_api

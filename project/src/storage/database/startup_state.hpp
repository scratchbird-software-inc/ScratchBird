// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// SB-STARTUP-STATE-ANCHOR
#include "disk_device.hpp"
#include "runtime_platform.hpp"

#include <string>
#include <vector>

namespace scratchbird::storage::database {

using scratchbird::core::platform::DiagnosticRecord;
using scratchbird::core::platform::Status;
using scratchbird::core::platform::TypedUuid;
using scratchbird::core::platform::u16;
using scratchbird::core::platform::u32;
using scratchbird::core::platform::u64;

inline constexpr u64 kSystemStatePageNumber = 1;
inline constexpr u64 kCatalogPageNumber = 2;
inline constexpr u64 kAllocationMapPageNumber = 3;
inline constexpr u64 kTransactionInventoryPageNumber = 4;
inline constexpr u64 kBootstrapReservedPageNumber = 5;
inline constexpr u64 kCatalogOverflowFirstPageNumber = 6;
inline constexpr u32 kStartupStateFormatMajorCurrent = 1;
inline constexpr u32 kStartupStateFormatMajorMinSupported = 1;
inline constexpr u32 kStartupStateFormatMajorMaxSupported = 1;
inline constexpr u32 kStartupStateFormatMinorCurrent = 0;
inline constexpr u32 kStartupStateFormatMinorMinSupported = 0;
inline constexpr u32 kStartupStateFormatMinorMaxSupported = 0;

enum class StartupRecoveryClassification : u16 {
  clean_checkpoint_path,
  checkpoint_rebuild_required,
  repaired_recovery,
  fence_writes_until_safe,
  corruption_stop,
  restricted_open_required,
  operator_review_required
};

enum class StartupLifecycleDurablePhase : u16 {
  none,
  create_tx1_committed,
  open_dirty_marked,
  open_tx2_committed,
  open_ready,
  clean_shutdown,
  maintenance_entered,
  maintenance_exited,
  restricted_open_entered,
  restricted_open_exited,
  verify_completed,
  repair_completed,
  repair_refused,
  drop_evidence_recorded
};

enum class StartupStateFormatCompatibilityClass : u16 {
  supported_current,
  unsupported_old,
  unsupported_new,
  downgrade_refused,
  newer_than_supported_refused,
  missing_migration_plan_refused,
  migration_required_without_plan_refused
};

namespace StartupLifecycleEvidenceFlag {
inline constexpr u64 bootstrap_tx1_committed = 1ull << 0;
inline constexpr u64 first_open_tx2_committed = 1ull << 1;
inline constexpr u64 clean_shutdown_tx_committed = 1ull << 2;
inline constexpr u64 startup_owner_token_persisted = 1ull << 3;
inline constexpr u64 authorities_ready = 1ull << 4;
inline constexpr u64 maintenance_evidence_recorded = 1ull << 5;
inline constexpr u64 restricted_open_evidence_recorded = 1ull << 6;
inline constexpr u64 verify_evidence_recorded = 1ull << 7;
inline constexpr u64 repair_evidence_recorded = 1ull << 8;
inline constexpr u64 repair_refusal_evidence_recorded = 1ull << 9;
inline constexpr u64 drop_evidence_recorded = 1ull << 10;
inline constexpr u64 cache_preload_completed = 1ull << 11;
inline constexpr u64 cache_checkpoint_completed = 1ull << 12;
inline constexpr u64 cache_shutdown_flush_completed = 1ull << 13;
}  // namespace StartupLifecycleEvidenceFlag

struct StartupStateRecord {
  u32 format_major = kStartupStateFormatMajorCurrent;
  u32 format_minor = kStartupStateFormatMinorCurrent;
  TypedUuid database_uuid;
  TypedUuid first_filespace_uuid;
  u32 page_size = 0;
  bool clean_shutdown = false;
  bool startup_dirty = false;
  bool write_admission_fenced = true;
  bool config_authority_loaded = false;
  bool security_authority_loaded = false;
  bool i18n_authority_loaded = false;
  bool runtime_activation_complete = false;
  bool agent_runtime_started = false;
  bool cache_runtime_started = false;
  bool ipc_runtime_started = false;
  bool server_runtime_started = false;
  u64 startup_counter = 0;
  u64 restart_generation = 0;
  u64 checkpoint_generation = 0;
  u64 runtime_activation_generation = 0;
  u64 bootstrap_local_transaction_id = 0;
  u64 first_open_activation_local_transaction_id = 0;
  u64 clean_shutdown_local_transaction_id = 0;
  u64 last_lifecycle_local_transaction_id = 0;
  u64 last_lifecycle_event_unix_epoch_millis = 0;
  u64 lifecycle_generation = 0;
  StartupLifecycleDurablePhase durable_lifecycle_phase = StartupLifecycleDurablePhase::none;
  u64 durable_evidence_flags = 0;
  StartupRecoveryClassification recovery_classification = StartupRecoveryClassification::clean_checkpoint_path;
  std::string owner_token;
  std::vector<std::string> completed_phases;
};

struct StartupStateResult {
  Status status;
  StartupStateRecord state;
  DiagnosticRecord diagnostic;

  bool ok() const { return status.ok(); }
};

struct StartupWriteResult {
  Status status;
  DiagnosticRecord diagnostic;

  bool ok() const { return status.ok(); }
};

struct StartupStateFormatCompatibilityResult {
  Status status;
  StartupStateFormatCompatibilityClass compatibility_class =
      StartupStateFormatCompatibilityClass::unsupported_new;
  bool migration_required = false;
  DiagnosticRecord diagnostic;

  bool ok() const { return status.ok(); }
};

const char* StartupRecoveryClassificationName(StartupRecoveryClassification classification);
const char* StartupLifecycleDurablePhaseName(StartupLifecycleDurablePhase phase);
const char* StartupStateFormatCompatibilityClassName(
    StartupStateFormatCompatibilityClass compatibility_class);
StartupStateFormatCompatibilityResult ClassifyStartupStateFormatCompatibility(
    u32 format_major,
    u32 format_minor,
    const std::string& migration_plan_id = {},
    bool downgrade_requested = false,
    bool migration_plan_required = false);
StartupStateRecord MakeInitialStartupState(TypedUuid database_uuid,
                                           TypedUuid first_filespace_uuid,
                                           u32 page_size);
StartupStateRecord RecordStartupLifecycleEvidence(StartupStateRecord state,
                                                  StartupLifecycleDurablePhase phase,
                                                  u64 local_transaction_id,
                                                  u64 event_unix_epoch_millis,
                                                  u64 evidence_flags);
bool StartupLifecycleEvidencePresent(const StartupStateRecord& state, u64 required_flags);
StartupWriteResult WriteStartupStatePageBody(scratchbird::storage::disk::FileDevice* device,
                                             const StartupStateRecord& state);
StartupStateResult ReadStartupStatePageBody(scratchbird::storage::disk::FileDevice* device,
                                            u32 page_size);
StartupStateRecord MarkStartupDirty(StartupStateRecord state,
                                    std::string owner_token,
                                    StartupRecoveryClassification prior_state_classification);
StartupStateRecord MarkStartupClean(StartupStateRecord state);
DiagnosticRecord MakeStartupStateDiagnostic(Status status,
                                            std::string diagnostic_code,
                                            std::string message_key,
                                            std::string detail = {});

}  // namespace scratchbird::storage::database

// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "agent_runtime.hpp"

#include <string>
#include <vector>

namespace scratchbird::core::agents::implemented_agents {

using scratchbird::core::platform::DiagnosticRecord;
using scratchbird::core::platform::Status;
using scratchbird::core::platform::u32;
using scratchbird::core::platform::u64;

enum class CleanupArchiveManagerDecisionKind : u32 {
  no_action,
  advance_cleanup_lwm,
  request_archive_verify,
  refused
};

struct CleanupArchiveManagerPolicy {
  bool present = true;
  bool valid = true;
  bool scope_compatible = true;
  bool cleanup_lwm_allowed = true;
  bool archive_verify_allowed = true;
  u64 max_lwm_advance = 10000;
  u64 archive_lag_bytes_threshold = 104857600;
};

struct CleanupArchiveManagerSnapshot {
  u64 authoritative_cleanup_horizon = 0;
  u64 current_cleanup_lwm = 0;
  u64 blocked_cleanup_count = 0;
  u64 archive_lag_bytes = 0;
  u64 archive_slice_age_microseconds = 0;
  bool cleanup_horizon_authoritative = false;
  bool archive_metadata_authoritative = false;
  bool disposal_guard_requested = false;
  bool reader_hold_active = false;
  bool writer_hold_active = false;
  bool parser_snapshot_hold_active = false;
  bool backup_forward_hold_active = false;
  bool archive_hold_active = false;
  bool legal_hold_active = false;
  bool backup_hold_active = false;
  bool detached_filespace_hold_active = false;
  bool stable_checkpoint_hold_active = false;
  bool local_durable_hold_active = false;
  bool restore_reachability_hold_active = false;
  bool parser_authority = false;
  bool recovery_authority = false;
};

struct CleanupArchiveManagerEvidenceField {
  std::string key;
  std::string value;
};

struct CleanupArchiveManagerResult {
  Status status;
  DiagnosticRecord diagnostic;
  CleanupArchiveManagerDecisionKind decision =
      CleanupArchiveManagerDecisionKind::refused;
  std::vector<CleanupArchiveManagerEvidenceField> evidence;
  bool fail_closed = true;
  u64 proposed_cleanup_lwm = 0;

  bool ok() const { return status.ok() && !fail_closed; }
};

const char* CleanupArchiveManagerDecisionKindName(
    CleanupArchiveManagerDecisionKind decision);
CleanupArchiveManagerResult EvaluateCleanupArchiveManager(
    const CleanupArchiveManagerSnapshot& snapshot,
    const CleanupArchiveManagerPolicy& policy = {});
DiagnosticRecord MakeCleanupArchiveManagerDiagnostic(Status status,
                                                     std::string diagnostic_code,
                                                     std::string message_key,
                                                     std::string detail = {});

const char* cleanup_archive_manager_implementation_anchor();

}  // namespace scratchbird::core::agents::implemented_agents

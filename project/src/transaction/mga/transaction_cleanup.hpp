// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// SB-SNTXN-CLEANUP-ANCHOR
#include "copy_on_write.hpp"
#include "transaction_cleanup_horizon_service.hpp"
#include "transaction_horizon.hpp"

#include <string>
#include <vector>

namespace scratchbird::transaction::mga {

enum class LocalCleanupSweepFamily : u16 {
  cooperative,
  background,
  explicit_request,
  maintenance,
  emergency,
  unknown
};

struct TransactionCleanupDecisionResult {
  Status status;
  CleanupEligibilityDecision decision = CleanupEligibilityDecision::unknown;
  CleanupHoldKind blocking_hold = CleanupHoldKind::unknown;
  DiagnosticRecord diagnostic;

  bool ok() const {
    return status.ok();
  }
};

struct AuthoritativeCleanupHold {
  CleanupHoldKind hold_kind = CleanupHoldKind::unknown;
  LocalTransactionId horizon_transaction;
  bool authoritative = false;
  std::string stable_name;
};

struct HistoryRetentionHorizon {
  LocalTransactionId horizon_transaction;
  bool authoritative = false;
  std::string stable_name;
};

struct LocalCleanupReclaimEvidenceRecord {
  RowVersionIdentity row_version_identity;
  RowVersionState row_version_state = RowVersionState::unknown;
  LocalTransactionId creator_transaction;
  LocalTransactionId successor_transaction;
  u64 authoritative_cleanup_horizon_local_transaction_id = 0;
  std::string stable_evidence_id;
};

// SB_MGA_LOCAL_CLEANUP_WORKSET_AUTHORITY
struct LocalCleanupWorksetRequest {
  LocalTransactionInventory inventory;
  bool inventory_authoritative = false;
  bool inventory_complete = true;
  std::vector<LocalTransactionId> active_snapshot_horizons;
  bool active_snapshot_inventory_authoritative = true;
  bool always_in_transaction_policy = false;
  bool always_active_session_inventory_authoritative = true;
  std::vector<AlwaysActiveSessionTransactionBinding> always_active_sessions;
  std::vector<AuthoritativeCleanupHold> additional_holds;
  std::vector<HistoryRetentionHorizon> history_retention_horizons;
  std::vector<RowVersionMetadata> row_versions;
  u64 max_retained_row_versions = 0;
  bool retain_row_versions_in_result = true;
  bool emit_reclaim_evidence_records = false;
  u64 max_reclaim_evidence_records = 0;
  bool reclaim_rolled_back_versions = true;
  bool reclaim_delete_markers = true;
  bool reclaim_obsolete_committed_versions = true;
};

struct LocalCleanupWorksetResult {
  Status status;
  LocalTransactionHorizons horizons;
  std::vector<RowVersionMetadata> retained_row_versions;
  std::vector<LocalCleanupReclaimEvidenceRecord> reclaim_evidence_records;
  u64 reclaimed_row_version_count = 0;
  u64 retained_row_version_count = 0;
  u64 horizon_blocked_row_version_count = 0;
  u64 backup_archive_blocked_row_version_count = 0;
  u64 retention_blocked_row_version_count = 0;
  u64 limbo_or_unknown_outcome_blocked_row_version_count = 0;
  u64 oldest_retained_row_version_local_transaction_id = 0;
  u64 authoritative_cleanup_horizon_local_transaction_id = 0;
  bool cleanup_horizon_authoritative = false;
  bool bounded_memory_limit_hit = false;
  bool physical_storage_mutated = false;
  DiagnosticRecord diagnostic;

  bool ok() const {
    return status.ok() && cleanup_horizon_authoritative;
  }
};

struct LocalGarbageCollectionSweepRequest {
  LocalCleanupWorksetRequest workset;
  LocalCleanupSweepFamily family = LocalCleanupSweepFamily::unknown;
  bool engine_mga_authoritative = false;
  u64 max_candidate_row_versions = 0;
  u64 max_retained_row_versions = 0;
  bool retain_row_versions_in_result = false;
};

struct LocalGarbageCollectionSweepResult {
  Status status;
  LocalCleanupWorksetResult cleanup;
  LocalCleanupSweepFamily family = LocalCleanupSweepFamily::unknown;
  u64 scanned_row_version_count = 0;
  bool bounded_memory_enforced = false;
  DiagnosticRecord diagnostic;

  bool ok() const {
    return status.ok() && cleanup.ok() && bounded_memory_enforced;
  }
};

const char* LocalCleanupSweepFamilyName(LocalCleanupSweepFamily family);
TransactionCleanupDecisionResult EvaluateLocalCleanupWithHorizons(const RowVersionMetadata& metadata,
                                                                  const LocalTransactionHorizons& horizons);
TransactionCleanupDecisionResult EvaluateLocalCleanupWithAuthoritativeHolds(
    const RowVersionMetadata& metadata,
    const LocalTransactionHorizons& horizons,
    const std::vector<AuthoritativeCleanupHold>& additional_holds);
LocalCleanupWorksetResult ApplyLocalCleanupWithAuthoritativeInventory(
    const LocalCleanupWorksetRequest& request);
LocalGarbageCollectionSweepResult RunLocalGarbageCollectionSweep(
    const LocalGarbageCollectionSweepRequest& request);
DiagnosticRecord MakeTransactionCleanupDiagnostic(Status status,
                                                  std::string diagnostic_code,
                                                  std::string message_key,
                                                  std::string detail = {});

}  // namespace scratchbird::transaction::mga

// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// DPC_SECONDARY_INDEX_GARBAGE_CLEANUP_AGENT
#include "secondary_index_delta_ledger.hpp"

#include <string>
#include <vector>

namespace scratchbird::core::index {

inline constexpr const char* kSecondaryIndexGarbageCleanupSearchKey =
    "DPC_SECONDARY_INDEX_GARBAGE_CLEANUP_AGENT";

enum class SecondaryIndexGarbageCleanupDecisionKind : u32 {
  success,
  no_op,
  budget_exhausted,
  horizon_blocked,
  validation_refused,
  refused_non_authoritative,
  refused
};

struct SecondaryIndexTableSnapshotEntry {
  TypedUuid index_uuid;
  TypedUuid table_uuid;
  TypedUuid row_uuid;
  TypedUuid version_uuid;
  std::string key_payload;
  bool deleted = false;
};

struct SecondaryIndexGarbageCleanupMetrics {
  u64 table_snapshot_entries = 0;
  u64 base_index_entries = 0;
  u64 delta_ledger_records = 0;
  u64 relevant_delta_records = 0;
  u64 scanned_delta_records = 0;
  u64 eligible_garbage_records = 0;
  u64 cleaned_garbage_records = 0;
  u64 retained_garbage_records = 0;
  u64 retained_unmerged_delta_records = 0;
  u64 validation_expected_entries = 0;
  u64 validation_effective_entries = 0;
};

struct SecondaryIndexGarbageCleanupRequest {
  TypedUuid index_uuid;
  TypedUuid table_uuid;
  PersistentSecondaryIndexDeltaLedger ledger;
  std::vector<SecondaryIndexBaseEntry> base_entries;
  std::vector<SecondaryIndexTableSnapshotEntry> table_snapshot;
  u64 authoritative_cleanup_horizon_local_transaction_id = 0;
  bool cleanup_horizon_authoritative = false;
  SecondaryIndexKind index_kind = SecondaryIndexKind::non_unique;
  u64 max_records_to_scan = 1024;
  u64 max_records_to_clean = 256;
};

struct SecondaryIndexGarbageCleanupResult {
  Status status;
  SecondaryIndexGarbageCleanupDecisionKind decision =
      SecondaryIndexGarbageCleanupDecisionKind::refused;
  DiagnosticRecord diagnostic;
  PersistentSecondaryIndexDeltaLedger cleaned_ledger;
  SecondaryIndexGarbageCleanupMetrics before;
  SecondaryIndexGarbageCleanupMetrics after;
  bool fail_closed = true;
  bool bounded_batch = false;
  bool budget_exhausted = false;
  bool horizon_blocked = false;
  bool validation_before_ok = false;
  bool validation_after_ok = false;

  bool ok() const { return status.ok() && !fail_closed; }
};

const char* SecondaryIndexGarbageCleanupDecisionKindName(
    SecondaryIndexGarbageCleanupDecisionKind decision);

SecondaryIndexGarbageCleanupResult RunSecondaryIndexGarbageCleanupBatch(
    const SecondaryIndexGarbageCleanupRequest& request);

DiagnosticRecord MakeSecondaryIndexGarbageCleanupDiagnostic(
    Status status,
    std::string diagnostic_code,
    std::string message_key,
    std::string detail = {});

}  // namespace scratchbird::core::index

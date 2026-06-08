// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// SB-SECONDARY-INDEX-DELTA-MERGE-ANCHOR
// DPC_SECONDARY_INDEX_DELTA_MERGE_AGENT_GATE
#include "secondary_index_delta_overlay.hpp"

#include <string>
#include <vector>

namespace scratchbird::core::index {

enum class SecondaryIndexMergeState : u32 {
  absent,
  merged,
  cleanup_horizon_blocked,
  cleaned,
  refused,
  quarantine
};

enum class SecondaryIndexMergeRecoveryAction : u32 {
  no_action,
  complete_idempotent_merge,
  retain_until_horizon,
  fail_closed
};

struct SecondaryIndexMergeRequest {
  TypedUuid index_uuid;
  TypedUuid table_uuid;
  TypedUuid merge_id;
  u64 authoritative_cleanup_horizon_local_transaction_id = 0;
  bool cleanup_horizon_authoritative = false;
  SecondaryIndexKind index_kind = SecondaryIndexKind::non_unique;
  u64 max_records_to_scan = 1024;
  u64 max_records_to_merge = 256;
  bool merge_disabled = false;
};

struct SecondaryIndexMergeEvidenceRecord {
  u64 sequence = 0;
  TypedUuid evidence_id;
  TypedUuid merge_id;
  TypedUuid index_uuid;
  TypedUuid table_uuid;
  u64 ledger_generation_before = 0;
  u64 ledger_generation_after = 0;
  u64 merged_count = 0;
  u64 retained_delta_count = 0;
  u64 cleaned_delta_count = 0;
  u64 scanned_delta_count = 0;
  u64 max_records_to_scan = 0;
  u64 max_records_to_merge = 0;
  u64 authoritative_cleanup_horizon_local_transaction_id = 0;
  SecondaryIndexMergeState previous_state = SecondaryIndexMergeState::absent;
  SecondaryIndexMergeState new_state = SecondaryIndexMergeState::absent;
  std::string diagnostic_code;
  std::string throttle_or_refusal_reason;
  bool durable_state_changed = false;
};

struct SecondaryIndexDeltaMergeLedger {
  std::vector<SecondaryIndexMergeEvidenceRecord> evidence;
  u64 next_evidence_sequence = 1;
  u64 ledger_generation = 1;
};

struct SecondaryIndexMergeResult {
  Status status;
  bool merged = false;
  bool throttled = false;
  u64 merged_count = 0;
  u64 retained_count = 0;
  u64 cleaned_count = 0;
  u64 ledger_generation = 0;
  SecondaryIndexMergeEvidenceRecord evidence;
  DiagnosticRecord diagnostic;

  bool ok() const { return status.ok() && merged; }
};

struct SecondaryIndexCleanupRequest {
  TypedUuid index_uuid;
  TypedUuid table_uuid;
  u64 authoritative_cleanup_horizon_local_transaction_id = 0;
  bool cleanup_horizon_authoritative = false;
};

struct SecondaryIndexCleanupResult {
  Status status;
  bool cleaned = false;
  bool horizon_blocked = false;
  u64 cleaned_count = 0;
  u64 retained_count = 0;
  SecondaryIndexMergeEvidenceRecord evidence;
  DiagnosticRecord diagnostic;

  bool ok() const { return status.ok() && cleaned; }
};

struct SecondaryIndexMergeRecoveryClassification {
  TypedUuid evidence_id;
  SecondaryIndexMergeState observed_state = SecondaryIndexMergeState::absent;
  SecondaryIndexMergeRecoveryAction action = SecondaryIndexMergeRecoveryAction::fail_closed;
  bool fail_closed = false;
  std::string stable_reason;
};

struct SecondaryIndexMergeRecoveryResult {
  Status status;
  std::vector<SecondaryIndexMergeRecoveryClassification> classifications;
  DiagnosticRecord diagnostic;

  bool ok() const { return status.ok(); }
};

const char* SecondaryIndexMergeStateName(SecondaryIndexMergeState state);
const char* SecondaryIndexMergeRecoveryActionName(SecondaryIndexMergeRecoveryAction action);

SecondaryIndexMergeResult MergeSecondaryIndexDeltas(SecondaryIndexDeltaMergeLedger* merge_ledger,
                                                    std::vector<SecondaryIndexBaseEntry>* base_entries,
                                                    SecondaryIndexDeltaLedger* delta_ledger,
                                                    const SecondaryIndexMergeRequest& request);
SecondaryIndexCleanupResult CleanupSecondaryIndexDeltas(SecondaryIndexDeltaMergeLedger* merge_ledger,
                                                        SecondaryIndexDeltaLedger* delta_ledger,
                                                        const SecondaryIndexCleanupRequest& request);
SecondaryIndexMergeRecoveryResult ClassifySecondaryIndexMergeLedgerForRecovery(
    const SecondaryIndexDeltaMergeLedger& merge_ledger);
DiagnosticRecord MakeSecondaryIndexMergeDiagnostic(Status status,
                                                   std::string diagnostic_code,
                                                   std::string message_key,
                                                   std::string detail = {});

}  // namespace scratchbird::core::index

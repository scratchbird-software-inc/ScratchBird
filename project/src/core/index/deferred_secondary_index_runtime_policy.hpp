// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// DPC_DEFERRED_INDEX_FEATURE_FLAG
#include "page_finality_evidence.hpp"
#include "secondary_index_delta_ledger.hpp"

#include <string>
#include <vector>

namespace scratchbird::core::index {

constexpr const char* kDeferredSecondaryIndexRuntimeOption =
    "runtime.deferred_secondary_index=enabled";
constexpr const char* kSecondaryIndexDeltaLedgerFeatureOption =
    "feature.secondary_index_delta_ledger=enabled";
constexpr const char* kDeltaLedgerReaderOverlayOption =
    "delta_ledger.reader_overlay=enabled";
constexpr const char* kDeltaLedgerCleanupHorizonBoundOption =
    "delta_ledger.cleanup_horizon_bound=true";
constexpr const char* kDeltaLedgerRecoveryClassifiableOption =
    "delta_ledger.recovery_classifiable=true";

struct DeferredSecondaryIndexRuntimeDecision {
  bool runtime_enabled = false;
  bool feature_enabled = false;
  bool readers_overlay_committed_deltas = false;
  bool cleanup_horizon_bound = false;
  bool recovery_classifiable = false;
  bool enabled = false;
  bool synchronous_fallback_required = true;
  std::string fallback_reason = "runtime_deferred_secondary_index_disabled";
};

struct SecondaryChangeBufferEvidenceField {
  std::string name;
  std::string value;
};

struct PageAwareSecondaryChangeBufferCounters {
  u64 decisions = 0;
  u64 selected = 0;
  u64 refused = 0;
  u64 unique_refusals = 0;
  u64 hot_page_refusals = 0;
  u64 backlog_refusals = 0;
  u64 overlay_refusals = 0;
  u64 recovery_refusals = 0;
  u64 finality_authority_refusals = 0;
};

struct PageAwareSecondaryChangeBufferRequest {
  std::vector<std::string> option_envelopes;
  SecondaryIndexKind index_kind = SecondaryIndexKind::non_unique;
  bool unique_reservation_protocol_present = false;
  bool unique_reservation_protocol_proven = false;
  bool unique_reservation_protocol_enabled = false;
  std::string unique_reservation_protocol_gate_token;

  u64 target_page_random_io_score = 0;
  u64 cold_random_io_score_threshold = 70;
  u64 page_finality_advisory_score_credit = 10;
  bool target_page_cold = false;

  u64 pending_delta_count = 0;
  u64 incoming_delta_count = 1;
  u64 max_pending_delta_count = 1024;
  u64 pending_delta_bytes = 0;
  u64 incoming_delta_bytes = 0;
  u64 max_pending_delta_bytes = 1024 * 1024;

  bool delta_overlay_available = false;
  bool delta_overlay_read_safe = false;
  bool persisted_recovery_proof_available = false;
  PersistentSecondaryIndexDeltaLedger persistent_delta_ledger;
  SecondaryIndexDeltaLedgerLimits ledger_limits;

  bool durable_transaction_inventory_authoritative = true;
  bool page_finality_evidence_present = false;
  scratchbird::transaction::mga::PageFinalityEvidenceDecision page_finality;
};

struct PageAwareSecondaryChangeBufferDecision {
  Status status;
  DiagnosticRecord diagnostic;
  bool selected = false;
  bool synchronous_fallback_required = true;
  bool page_finality_used_as_advisory_acceleration = false;
  bool finality_map_transaction_authority = false;
  bool durable_mga_inventory_remains_authority = true;
  u64 effective_random_io_threshold = 0;
  std::string reason = "not_evaluated";
  std::string recovery_class = "not_classified";
  std::string recovery_action = "not_classified";
  PageAwareSecondaryChangeBufferCounters counters;
  std::vector<SecondaryChangeBufferEvidenceField> evidence;

  bool ok() const { return status.ok() && selected; }
};

bool DeferredIndexOptionEnabled(const std::vector<std::string>& option_envelopes,
                                const std::string& option);
DeferredSecondaryIndexRuntimeDecision ResolveDeferredSecondaryIndexRuntimePolicy(
    const std::vector<std::string>& option_envelopes);
PageAwareSecondaryChangeBufferDecision SelectPageAwareSecondaryChangeBufferV2(
    const PageAwareSecondaryChangeBufferRequest& request);

}  // namespace scratchbird::core::index

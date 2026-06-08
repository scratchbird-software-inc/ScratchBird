// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// SB-SECONDARY-INDEX-DELTA-OVERLAY-ANCHOR
#include "runtime_platform.hpp"
#include "uuid.hpp"

#include <string>
#include <vector>

namespace scratchbird::core::index {

using scratchbird::core::platform::DiagnosticRecord;
using scratchbird::core::platform::Status;
using scratchbird::core::platform::TypedUuid;
using scratchbird::core::platform::u32;
using scratchbird::core::platform::u64;

enum class SecondaryIndexKind : u32 {
  non_unique,
  unique
};

enum class SecondaryIndexDeltaKind : u32 {
  insert,
  delete_row,
  update_before,
  update_after
};

enum class SecondaryIndexDeltaVisibility : u32 {
  invisible,
  own_transaction,
  committed_visible
};

enum class SecondaryIndexOverlayEntryOrigin : u32 {
  base_index,
  delta_insert,
  delta_update_after
};

struct SecondaryIndexBaseEntry {
  TypedUuid index_uuid;
  TypedUuid table_uuid;
  TypedUuid row_uuid;
  TypedUuid version_uuid;
  std::string key_payload;
  u64 committed_local_transaction_id = 0;
  bool deleted = false;
};

struct SecondaryIndexDeltaEntry {
  TypedUuid delta_id;
  TypedUuid index_uuid;
  TypedUuid table_uuid;
  TypedUuid row_uuid;
  TypedUuid version_uuid;
  TypedUuid transaction_uuid;
  u64 local_transaction_id = 0;
  SecondaryIndexDeltaKind delta_kind = SecondaryIndexDeltaKind::insert;
  std::string key_payload;
  std::string cleanup_horizon_token;
  bool committed = false;
};

struct SecondaryIndexDeltaLedger {
  std::vector<SecondaryIndexDeltaEntry> deltas;
};

struct SecondaryIndexOverlayRequest {
  TypedUuid index_uuid;
  TypedUuid table_uuid;
  TypedUuid transaction_uuid;
  u64 local_transaction_id = 0;
  u64 snapshot_high_water_local_transaction_id = 0;
  SecondaryIndexKind index_kind = SecondaryIndexKind::non_unique;
  bool include_own_transaction = true;
  bool allow_deferred_unique_overlay = false;
  bool unique_reservation_protocol_present = false;
  bool unique_reservation_protocol_proven = false;
  bool unique_reservation_protocol_enabled = false;
  std::string unique_reservation_protocol_gate_token;
};

struct SecondaryIndexOverlayEntry {
  TypedUuid index_uuid;
  TypedUuid table_uuid;
  TypedUuid row_uuid;
  TypedUuid version_uuid;
  std::string key_payload;
  SecondaryIndexOverlayEntryOrigin origin = SecondaryIndexOverlayEntryOrigin::base_index;
};

struct SecondaryIndexOverlayEvidenceRecord {
  u64 sequence = 0;
  TypedUuid evidence_id;
  TypedUuid index_uuid;
  TypedUuid table_uuid;
  TypedUuid transaction_uuid;
  u64 local_transaction_id = 0;
  u64 snapshot_high_water_local_transaction_id = 0;
  u64 base_entries_scanned = 0;
  u64 delta_entries_scanned = 0;
  u64 visible_delta_entries = 0;
  u64 result_entries = 0;
  std::string diagnostic_code;
  bool durable_state_changed = false;
};

struct SecondaryIndexOverlayLedger {
  std::vector<SecondaryIndexOverlayEvidenceRecord> evidence;
  u64 next_evidence_sequence = 1;
};

struct SecondaryIndexOverlayResult {
  Status status;
  bool overlaid = false;
  std::vector<SecondaryIndexOverlayEntry> entries;
  SecondaryIndexOverlayEvidenceRecord evidence;
  DiagnosticRecord diagnostic;

  bool ok() const { return status.ok() && overlaid; }
};

const char* SecondaryIndexDeltaKindName(SecondaryIndexDeltaKind kind);
const char* SecondaryIndexDeltaVisibilityName(SecondaryIndexDeltaVisibility visibility);
const char* SecondaryIndexOverlayEntryOriginName(SecondaryIndexOverlayEntryOrigin origin);

SecondaryIndexDeltaVisibility ClassifySecondaryIndexDeltaVisibility(
    const SecondaryIndexOverlayRequest& request,
    const SecondaryIndexDeltaEntry& delta);
SecondaryIndexOverlayResult BuildSecondaryIndexDeltaOverlay(SecondaryIndexOverlayLedger* overlay_ledger,
                                                            const std::vector<SecondaryIndexBaseEntry>& base_entries,
                                                            const SecondaryIndexDeltaLedger& delta_ledger,
                                                            const SecondaryIndexOverlayRequest& request);
DiagnosticRecord MakeSecondaryIndexOverlayDiagnostic(Status status,
                                                     std::string diagnostic_code,
                                                     std::string message_key,
                                                     std::string detail = {});

}  // namespace scratchbird::core::index

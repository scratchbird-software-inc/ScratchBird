// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// SB-SECONDARY-INDEX-DELTA-OVERLAY-ANCHOR
#include "secondary_index_delta_overlay.hpp"

#include "unique_index_deferral_policy.hpp"

#include <algorithm>
#include <utility>

namespace scratchbird::core::index {
namespace {

using scratchbird::core::platform::Severity;
using scratchbird::core::platform::StatusCode;
using scratchbird::core::platform::Subsystem;
using scratchbird::core::platform::UuidKind;

Status OverlayOkStatus() {
  return {StatusCode::ok, Severity::info, Subsystem::engine};
}

Status OverlayErrorStatus() {
  return {StatusCode::platform_required_feature_missing, Severity::error, Subsystem::engine};
}

bool SameUuid(const TypedUuid& left, const TypedUuid& right) {
  return left.kind == right.kind && left.value == right.value;
}

bool SameIndexTable(const SecondaryIndexOverlayRequest& request,
                    const TypedUuid& index_uuid,
                    const TypedUuid& table_uuid) {
  return SameUuid(request.index_uuid, index_uuid) && SameUuid(request.table_uuid, table_uuid);
}

TypedUuid NewEvidenceId(const SecondaryIndexOverlayLedger* ledger) {
  const auto generated = scratchbird::core::uuid::GenerateEngineIdentityV7(
      UuidKind::object,
      ledger == nullptr ? 1 : ledger->next_evidence_sequence);
  return generated.ok() ? generated.value : TypedUuid{};
}

bool SameLogicalIndexEntry(const SecondaryIndexOverlayEntry& entry,
                           const SecondaryIndexDeltaEntry& delta) {
  return SameUuid(entry.row_uuid, delta.row_uuid) && entry.key_payload == delta.key_payload;
}

void RemoveLogicalIndexEntry(std::vector<SecondaryIndexOverlayEntry>* entries,
                             const SecondaryIndexDeltaEntry& delta) {
  if (entries == nullptr) {
    return;
  }
  entries->erase(std::remove_if(entries->begin(),
                                entries->end(),
                                [&](const SecondaryIndexOverlayEntry& entry) {
                                  return SameLogicalIndexEntry(entry, delta);
                                }),
                 entries->end());
}

void AddOrReplaceOverlayEntry(std::vector<SecondaryIndexOverlayEntry>* entries,
                              const SecondaryIndexDeltaEntry& delta,
                              SecondaryIndexOverlayEntryOrigin origin) {
  if (entries == nullptr) {
    return;
  }
  RemoveLogicalIndexEntry(entries, delta);
  SecondaryIndexOverlayEntry entry;
  entry.index_uuid = delta.index_uuid;
  entry.table_uuid = delta.table_uuid;
  entry.row_uuid = delta.row_uuid;
  entry.version_uuid = delta.version_uuid;
  entry.key_payload = delta.key_payload;
  entry.origin = origin;
  entries->push_back(entry);
}

SecondaryIndexOverlayEvidenceRecord BuildEvidence(SecondaryIndexOverlayLedger* ledger,
                                                  const SecondaryIndexOverlayRequest& request,
                                                  u64 base_entries_scanned,
                                                  u64 delta_entries_scanned,
                                                  u64 visible_delta_entries,
                                                  u64 result_entries,
                                                  std::string diagnostic_code) {
  SecondaryIndexOverlayEvidenceRecord evidence;
  evidence.sequence = ledger == nullptr ? 0 : ledger->next_evidence_sequence++;
  evidence.evidence_id = NewEvidenceId(ledger);
  evidence.index_uuid = request.index_uuid;
  evidence.table_uuid = request.table_uuid;
  evidence.transaction_uuid = request.transaction_uuid;
  evidence.local_transaction_id = request.local_transaction_id;
  evidence.snapshot_high_water_local_transaction_id = request.snapshot_high_water_local_transaction_id;
  evidence.base_entries_scanned = base_entries_scanned;
  evidence.delta_entries_scanned = delta_entries_scanned;
  evidence.visible_delta_entries = visible_delta_entries;
  evidence.result_entries = result_entries;
  evidence.diagnostic_code = std::move(diagnostic_code);
  evidence.durable_state_changed = false;
  return evidence;
}

SecondaryIndexOverlayResult Refuse(SecondaryIndexOverlayLedger* ledger,
                                   const SecondaryIndexOverlayRequest& request,
                                   std::string diagnostic_code,
                                   std::string message_key,
                                   std::string detail) {
  SecondaryIndexOverlayResult result;
  result.status = OverlayErrorStatus();
  result.overlaid = false;
  result.evidence = BuildEvidence(ledger, request, 0, 0, 0, 0, diagnostic_code);
  result.diagnostic = MakeSecondaryIndexOverlayDiagnostic(result.status,
                                                         std::move(diagnostic_code),
                                                         std::move(message_key),
                                                         std::move(detail));
  if (ledger != nullptr) {
    ledger->evidence.push_back(result.evidence);
  }
  return result;
}

}  // namespace

const char* SecondaryIndexDeltaKindName(SecondaryIndexDeltaKind kind) {
  switch (kind) {
    case SecondaryIndexDeltaKind::insert:
      return "insert";
    case SecondaryIndexDeltaKind::delete_row:
      return "delete";
    case SecondaryIndexDeltaKind::update_before:
      return "update_before";
    case SecondaryIndexDeltaKind::update_after:
      return "update_after";
  }
  return "unknown";
}

const char* SecondaryIndexDeltaVisibilityName(SecondaryIndexDeltaVisibility visibility) {
  switch (visibility) {
    case SecondaryIndexDeltaVisibility::invisible:
      return "invisible";
    case SecondaryIndexDeltaVisibility::own_transaction:
      return "own_transaction";
    case SecondaryIndexDeltaVisibility::committed_visible:
      return "committed_visible";
  }
  return "unknown";
}

const char* SecondaryIndexOverlayEntryOriginName(SecondaryIndexOverlayEntryOrigin origin) {
  switch (origin) {
    case SecondaryIndexOverlayEntryOrigin::base_index:
      return "base_index";
    case SecondaryIndexOverlayEntryOrigin::delta_insert:
      return "delta_insert";
    case SecondaryIndexOverlayEntryOrigin::delta_update_after:
      return "delta_update_after";
  }
  return "unknown";
}

SecondaryIndexDeltaVisibility ClassifySecondaryIndexDeltaVisibility(
    const SecondaryIndexOverlayRequest& request,
    const SecondaryIndexDeltaEntry& delta) {
  if (!SameIndexTable(request, delta.index_uuid, delta.table_uuid)) {
    return SecondaryIndexDeltaVisibility::invisible;
  }
  if (request.include_own_transaction &&
      SameUuid(request.transaction_uuid, delta.transaction_uuid) &&
      request.local_transaction_id == delta.local_transaction_id) {
    return SecondaryIndexDeltaVisibility::own_transaction;
  }
  if (delta.committed && delta.local_transaction_id <= request.snapshot_high_water_local_transaction_id) {
    return SecondaryIndexDeltaVisibility::committed_visible;
  }
  return SecondaryIndexDeltaVisibility::invisible;
}

SecondaryIndexOverlayResult BuildSecondaryIndexDeltaOverlay(SecondaryIndexOverlayLedger* overlay_ledger,
                                                            const std::vector<SecondaryIndexBaseEntry>& base_entries,
                                                            const SecondaryIndexDeltaLedger& delta_ledger,
                                                            const SecondaryIndexOverlayRequest& request) {
  if (overlay_ledger == nullptr) {
    return Refuse(nullptr,
                  request,
                  "secondary_index_overlay_missing_ledger",
                  "core.index.secondary_overlay.missing_ledger",
                  "secondary-index overlay ledger is required");
  }
  if (!request.index_uuid.valid() || !request.table_uuid.valid()) {
    return Refuse(overlay_ledger,
                  request,
                  "secondary_index_overlay_invalid_identity",
                  "core.index.secondary_overlay.invalid_identity",
                  "index_uuid and table_uuid must be valid engine UUIDs");
  }
  // DPC_UNIQUE_INDEX_DEFERRAL_POLICY: overlay is a deferred maintenance path,
  // so unique indexes must pass the standalone reservation gate before use.
  if (request.index_kind == SecondaryIndexKind::unique && !request.allow_deferred_unique_overlay) {
    UniqueIndexDeferralPolicyRequest policy_request;
    policy_request.uniqueness = IndexUniquenessClass::unique_secondary;
    policy_request.request_kind = IndexDeferralRequestKind::generic_deferred;
    const auto policy = EvaluateUniqueIndexDeferralPolicy(policy_request);
    return Refuse(overlay_ledger,
                  request,
                  policy.diagnostic.diagnostic_code,
                  policy.diagnostic.message_key,
                  "unique indexes must remain synchronous until unique reservation protocol exists");
  }
  if (request.index_kind == SecondaryIndexKind::unique && request.allow_deferred_unique_overlay) {
    UniqueIndexDeferralPolicyRequest policy_request;
    policy_request.uniqueness = IndexUniquenessClass::unique_secondary;
    policy_request.request_kind = IndexDeferralRequestKind::unique_deferred_with_reservation;
    policy_request.reservation_protocol_present = request.unique_reservation_protocol_present;
    policy_request.reservation_protocol_proven = request.unique_reservation_protocol_proven;
    policy_request.reservation_protocol_enabled = request.unique_reservation_protocol_enabled;
    policy_request.reservation_protocol_gate_token =
        request.unique_reservation_protocol_gate_token;
    const auto policy = EvaluateUniqueIndexDeferralPolicy(policy_request);
    if (!policy.ok()) {
      return Refuse(overlay_ledger,
                    request,
                    policy.diagnostic.diagnostic_code,
                    policy.diagnostic.message_key,
                    "unique indexes require a proven reservation protocol before deferred overlay");
    }
    return Refuse(overlay_ledger,
                  request,
                  "INDEX.UNIQUE_DEFERRAL.DML_ROUTE_INCOMPLETE",
                  "core.index.deferral.unique_dml_route_incomplete",
                  "unique deferred overlay still requires DML route closure to consume reservation ledger proofs");
  }

  std::vector<SecondaryIndexOverlayEntry> entries;
  u64 base_scanned = 0;
  for (const auto& base : base_entries) {
    if (!SameIndexTable(request, base.index_uuid, base.table_uuid)) {
      continue;
    }
    ++base_scanned;
    if (base.deleted || base.committed_local_transaction_id > request.snapshot_high_water_local_transaction_id) {
      continue;
    }
    SecondaryIndexOverlayEntry entry;
    entry.index_uuid = base.index_uuid;
    entry.table_uuid = base.table_uuid;
    entry.row_uuid = base.row_uuid;
    entry.version_uuid = base.version_uuid;
    entry.key_payload = base.key_payload;
    entry.origin = SecondaryIndexOverlayEntryOrigin::base_index;
    entries.push_back(entry);
  }

  u64 delta_scanned = 0;
  u64 visible_delta_count = 0;
  for (const auto& delta : delta_ledger.deltas) {
    if (!SameIndexTable(request, delta.index_uuid, delta.table_uuid)) {
      continue;
    }
    ++delta_scanned;
    if (delta.cleanup_horizon_token.empty()) {
      return Refuse(overlay_ledger,
                    request,
                    "secondary_index_overlay_missing_cleanup_horizon",
                    "core.index.secondary_overlay.missing_cleanup_horizon",
                    "visible delta ledger entries require cleanup horizon authority");
    }
    const auto visibility = ClassifySecondaryIndexDeltaVisibility(request, delta);
    if (visibility == SecondaryIndexDeltaVisibility::invisible) {
      continue;
    }
    ++visible_delta_count;
    switch (delta.delta_kind) {
      case SecondaryIndexDeltaKind::insert:
        AddOrReplaceOverlayEntry(&entries, delta, SecondaryIndexOverlayEntryOrigin::delta_insert);
        break;
      case SecondaryIndexDeltaKind::delete_row:
      case SecondaryIndexDeltaKind::update_before:
        RemoveLogicalIndexEntry(&entries, delta);
        break;
      case SecondaryIndexDeltaKind::update_after:
        AddOrReplaceOverlayEntry(&entries, delta, SecondaryIndexOverlayEntryOrigin::delta_update_after);
        break;
    }
  }

  SecondaryIndexOverlayResult result;
  result.status = OverlayOkStatus();
  result.overlaid = true;
  result.entries = std::move(entries);
  result.evidence = BuildEvidence(overlay_ledger,
                                  request,
                                  base_scanned,
                                  delta_scanned,
                                  visible_delta_count,
                                  result.entries.size(),
                                  "ok");
  result.diagnostic = MakeSecondaryIndexOverlayDiagnostic(result.status,
                                                         "ok",
                                                         "core.index.secondary_overlay.built",
                                                         "secondary-index overlay built");
  overlay_ledger->evidence.push_back(result.evidence);
  return result;
}

DiagnosticRecord MakeSecondaryIndexOverlayDiagnostic(Status status,
                                                     std::string diagnostic_code,
                                                     std::string message_key,
                                                     std::string detail) {
  std::vector<scratchbird::core::platform::DiagnosticArgument> arguments;
  if (!detail.empty()) {
    arguments.push_back({"detail", std::move(detail)});
  }
  return scratchbird::core::platform::MakeDiagnostic(status.code,
                                                     status.severity,
                                                     status.subsystem,
                                                     std::move(diagnostic_code),
                                                     std::move(message_key),
                                                     std::move(arguments),
                                                     {},
                                                     "core.index.secondary_delta_overlay",
                                                     status.ok() ? "" : "fall back to synchronous index maintenance or retry after index authority is available");
}

}  // namespace scratchbird::core::index

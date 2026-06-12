// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "historical_snapshot_locator.hpp"

#include <utility>
#include <vector>

namespace scratchbird::transaction::mga {
namespace {

using scratchbird::core::platform::DiagnosticArgument;
using scratchbird::core::platform::MakeDiagnostic;
using scratchbird::core::platform::Severity;
using scratchbird::core::platform::StatusCode;
using scratchbird::core::platform::Subsystem;

Status HistoricalSnapshotOkStatus() {
  return {StatusCode::ok, Severity::info, Subsystem::transaction_mga};
}

Status HistoricalSnapshotErrorStatus() {
  return {StatusCode::platform_required_feature_missing,
          Severity::error,
          Subsystem::transaction_mga};
}

HistoricalAuditSnapshotResult Refuse(HistoricalAuditLocationClass location_class,
                                     std::string diagnostic_code,
                                     std::string message_key,
                                     std::string detail = {}) {
  HistoricalAuditSnapshotResult result;
  result.status = HistoricalSnapshotErrorStatus();
  result.location_class = location_class;
  result.fail_closed = true;
  result.queryable = false;
  result.writes_refused = true;
  result.retired_history_exact =
      location_class == HistoricalAuditLocationClass::retired;
  result.no_cluster_remote_fail_closed =
      location_class == HistoricalAuditLocationClass::remote;
  result.diagnostic = MakeHistoricalAuditSnapshotDiagnostic(
      result.status,
      std::move(diagnostic_code),
      std::move(message_key),
      std::move(detail));
  return result;
}

const TransactionInventoryEntry* FindEntry(const LocalTransactionInventory& inventory,
                                           LocalTransactionId local_id) {
  if (!local_id.valid()) {
    return nullptr;
  }
  for (const auto& entry : inventory.entries) {
    if (entry.identity.local_id.value == local_id.value) {
      return &entry;
    }
  }
  return nullptr;
}

bool TargetStateQueryable(TransactionState state) {
  return state == TransactionState::committed ||
         state == TransactionState::archived;
}

HistoricalAuditLocationClass InferredLocationClass(TransactionState state) {
  return state == TransactionState::archived
      ? HistoricalAuditLocationClass::local_archive
      : HistoricalAuditLocationClass::local_hot;
}

bool CommonAuthorityEvidencePresent(const HistoricalAuditSnapshotEvidence& evidence) {
  return evidence.transaction_inventory_authoritative &&
         evidence.security_policy_authoritative &&
         evidence.catalog_epoch_authoritative &&
         evidence.cluster_epoch_authoritative &&
         evidence.security_epoch != 0 &&
         evidence.catalog_generation_id != 0 &&
         evidence.cluster_epoch != 0;
}

}  // namespace

const char* HistoricalAuditLocationClassName(HistoricalAuditLocationClass value) {
  switch (value) {
    case HistoricalAuditLocationClass::local_hot: return "local_hot";
    case HistoricalAuditLocationClass::local_archive: return "local_archive";
    case HistoricalAuditLocationClass::retired: return "retired";
    case HistoricalAuditLocationClass::remote: return "remote";
    case HistoricalAuditLocationClass::unknown: return "unknown";
  }
  return "unknown";
}

HistoricalAuditSnapshotResult CreateHistoricalAuditSnapshot(
    const HistoricalAuditSnapshotRequest& request) {
  if (request.write_intent) {
    return Refuse(HistoricalAuditLocationClass::unknown,
                  "SB-MGA-HISTORICAL-SNAPSHOT-WRITE-REFUSED",
                  "transaction.historical_snapshot.write_refused");
  }

  if (request.requested_location_class == HistoricalAuditLocationClass::remote) {
    return Refuse(
        HistoricalAuditLocationClass::remote,
        request.evidence.external_cluster_provider_available
            ? "SB-MGA-HISTORICAL-SNAPSHOT-REMOTE-EXTERNAL-PROVIDER-REQUIRED"
            : "SB-MGA-HISTORICAL-SNAPSHOT-REMOTE-NO-CLUSTER",
        "transaction.historical_snapshot.remote_fail_closed",
        request.evidence.external_cluster_provider_available
            ? "remote_snapshot_requires_external_cluster_provider"
            : "remote_snapshot_no_cluster_provider");
  }

  if (request.retired_history_evidence_present ||
      request.requested_location_class == HistoricalAuditLocationClass::retired) {
    return Refuse(HistoricalAuditLocationClass::retired,
                  "SB-MGA-HISTORICAL-SNAPSHOT-RETIRED-EXACT",
                  "transaction.historical_snapshot.retired_exact",
                  "retired_history_requires_restore_or_retention_proof");
  }

  if (!CommonAuthorityEvidencePresent(request.evidence)) {
    return Refuse(HistoricalAuditLocationClass::unknown,
                  "SB-MGA-HISTORICAL-SNAPSHOT-AUTHORITY-EVIDENCE-REQUIRED",
                  "transaction.historical_snapshot.authority_evidence_required");
  }

  const TransactionInventoryEntry* reader =
      FindEntry(request.inventory, request.audit_reader_transaction);
  if (reader == nullptr) {
    return Refuse(HistoricalAuditLocationClass::unknown,
                  "SB-MGA-HISTORICAL-SNAPSHOT-READER-NOT-FOUND",
                  "transaction.historical_snapshot.reader_not_found");
  }
  if (reader->state != TransactionState::read_only_active) {
    return Refuse(HistoricalAuditLocationClass::unknown,
                  "SB-MGA-HISTORICAL-SNAPSHOT-READER-NOT-READ-ONLY",
                  "transaction.historical_snapshot.reader_not_read_only",
                  TransactionStateName(reader->state));
  }

  const TransactionInventoryEntry* target =
      FindEntry(request.inventory, request.target_local_transaction_id);
  if (target == nullptr) {
    return Refuse(HistoricalAuditLocationClass::unknown,
                  "SB-MGA-HISTORICAL-SNAPSHOT-TARGET-UNKNOWN",
                  "transaction.historical_snapshot.target_unknown");
  }

  const HistoricalAuditLocationClass location_class =
      request.requested_location_class == HistoricalAuditLocationClass::unknown
          ? InferredLocationClass(target->state)
          : request.requested_location_class;

  if (location_class == HistoricalAuditLocationClass::local_archive &&
      !request.evidence.archive_manifest_authoritative) {
    auto refused = Refuse(location_class,
                          "SB-MGA-HISTORICAL-SNAPSHOT-ARCHIVE-EVIDENCE-REQUIRED",
                          "transaction.historical_snapshot.archive_evidence_required");
    refused.target_local_transaction_id = target->identity.local_id;
    refused.target_transaction_state = TransactionStateName(target->state);
    return refused;
  }

  if (!TargetStateQueryable(target->state)) {
    auto refused = Refuse(location_class,
                          "SB-MGA-HISTORICAL-SNAPSHOT-TARGET-NOT-QUERYABLE",
                          "transaction.historical_snapshot.target_not_queryable",
                          TransactionStateName(target->state));
    refused.target_local_transaction_id = target->identity.local_id;
    refused.target_transaction_state = TransactionStateName(target->state);
    return refused;
  }

  LocalTransactionHorizonRequest horizon_request;
  horizon_request.inventory = request.inventory;
  horizon_request.active_snapshot_horizons.push_back(
      request.target_local_transaction_id);
  const auto horizons = ComputeLocalTransactionHorizons(horizon_request);
  if (!horizons.ok()) {
    HistoricalAuditSnapshotResult result;
    result.status = horizons.status;
    result.diagnostic = horizons.diagnostic;
    result.location_class = location_class;
    result.target_local_transaction_id = target->identity.local_id;
    result.target_transaction_state = TransactionStateName(target->state);
    result.fail_closed = true;
    result.writes_refused = true;
    return result;
  }

  HistoricalAuditSnapshotResult result;
  result.status = HistoricalSnapshotOkStatus();
  result.location_class = location_class;
  result.target_local_transaction_id = target->identity.local_id;
  result.target_transaction_state = TransactionStateName(target->state);
  result.queryable = true;
  result.fail_closed = false;
  result.writes_refused = true;
  result.parser_finality_authority = false;
  result.reference_finality_authority = false;
  result.snapshot.reader_transaction = reader->identity.local_id;
  result.snapshot.visible_through_local_transaction = target->identity.local_id;
  result.snapshot.transaction_start_visible_through_local_transaction =
      target->identity.local_id;
  result.snapshot.oldest_active_transaction =
      horizons.horizons.oldest_active_transaction;
  result.snapshot.oldest_snapshot_transaction =
      horizons.horizons.oldest_snapshot_transaction;
  result.snapshot.allow_reader_own_uncommitted = false;
  result.visibility_snapshot.reader_transaction = reader->identity.local_id;
  result.visibility_snapshot.visible_through_local_transaction_id =
      target->identity.local_id.value;
  result.visibility_snapshot.visible_through_local_transaction_id_is_boundary = true;
  result.visibility_snapshot.allow_reader_own_uncommitted = false;
  result.visibility_snapshot.recovery_context = false;
  result.diagnostic = MakeHistoricalAuditSnapshotDiagnostic(
      result.status,
      "SB-MGA-HISTORICAL-SNAPSHOT-OK",
      "transaction.historical_snapshot.ok",
      HistoricalAuditLocationClassName(location_class));
  return result;
}

DiagnosticRecord MakeHistoricalAuditSnapshotDiagnostic(Status status,
                                                       std::string diagnostic_code,
                                                       std::string message_key,
                                                       std::string detail) {
  std::vector<DiagnosticArgument> arguments;
  if (!detail.empty()) {
    arguments.push_back({"detail", detail});
  }
  return MakeDiagnostic(status.code,
                        status.severity,
                        status.subsystem,
                        std::move(diagnostic_code),
                        std::move(message_key),
                        std::move(arguments),
                        {},
                        "transaction.mga.historical_snapshot");
}

}  // namespace scratchbird::transaction::mga

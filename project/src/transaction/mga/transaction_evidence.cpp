// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "transaction_evidence.hpp"

#include "uuid.hpp"

#include <utility>
#include <vector>

namespace scratchbird::transaction::mga {
namespace {

using scratchbird::core::platform::DiagnosticArgument;
using scratchbird::core::platform::MakeDiagnostic;
using scratchbird::core::platform::Severity;
using scratchbird::core::platform::StatusCode;
using scratchbird::core::platform::Subsystem;
using scratchbird::core::uuid::UuidToString;

Status EvidenceOkStatus() {
  return {StatusCode::ok, Severity::info, Subsystem::transaction_mga};
}

Status EvidenceErrorStatus() {
  return {StatusCode::platform_required_feature_missing, Severity::error, Subsystem::transaction_mga};
}

std::string EventClassForState(TransactionState state) {
  switch (state) {
    case TransactionState::created:
    case TransactionState::active:
    case TransactionState::preparing:
      return "begin";
    case TransactionState::read_only_active:
      return "read_only_begin";
    case TransactionState::prepared:
      return "prepare";
    case TransactionState::committing:
    case TransactionState::committed:
      return "commit";
    case TransactionState::rolling_back:
    case TransactionState::rolled_back:
      return "rollback";
    case TransactionState::limbo:
    case TransactionState::recovering:
      return "recovery_hold";
    case TransactionState::failed_terminal:
      return "failed_terminal";
    case TransactionState::archived:
      return "archived";
    case TransactionState::none:
      return "unknown";
  }
  return "unknown";
}

bool RestoreSafeTerminal(TransactionState state) {
  return state == TransactionState::committed || state == TransactionState::rolled_back ||
         state == TransactionState::archived;
}

std::string RestoreClassificationFor(const TransactionRecoveryClassification& classification) {
  if (classification.fail_closed) {
    return "refuse_fail_closed";
  }
  if (RestoreSafeTerminal(classification.observed_state)) {
    return "restore_terminal_evidence";
  }
  if (classification.action == TransactionRecoveryAction::complete_rollback) {
    return "restore_after_local_rollback_recovery";
  }
  if (classification.action == TransactionRecoveryAction::complete_commit) {
    return "restore_after_local_commit_recovery";
  }
  return "restore_requires_classification";
}

TransactionLineageEvidenceRecord BuildRecord(const TransactionInventoryEntry& entry,
                                             std::string schema_epoch,
                                             std::string snapshot_capsule) {
  const auto classification = ClassifyLocalTransactionForRecovery(entry);
  TransactionLineageEvidenceRecord record;
  record.local_id = entry.identity.local_id;
  record.transaction_uuid = UuidToString(entry.identity.transaction_uuid.value);
  record.event_class = EventClassForState(entry.state);
  record.observed_state = TransactionStateName(entry.state);
  record.terminal_state = IsTerminalTransactionState(entry.state) ? TransactionStateName(entry.state) : "";
  record.schema_epoch = std::move(schema_epoch);
  record.snapshot_capsule = std::move(snapshot_capsule);
  record.restore_classification = RestoreClassificationFor(classification);
  record.refusal_condition = classification.fail_closed ? classification.stable_reason : "";
  record.terminal = IsTerminalTransactionState(entry.state);
  record.evidence_written = entry.evidence_record_written;
  record.wal_required = false;
  return record;
}

}  // namespace

std::vector<TransactionLineageEvidenceRecord> BuildTransactionLineageEvidence(
    const LocalTransactionInventory& inventory,
    std::string schema_epoch,
    std::string snapshot_capsule) {
  std::vector<TransactionLineageEvidenceRecord> records;
  records.reserve(inventory.entries.size());
  for (const auto& entry : inventory.entries) {
    records.push_back(BuildRecord(entry, schema_epoch, snapshot_capsule));
  }
  return records;
}

TransactionRestoreClassificationResult ClassifyTransactionInventoryForRestore(
    const LocalTransactionInventory& inventory,
    std::string schema_epoch,
    std::string snapshot_capsule,
    bool caller_requires_wal) {
  TransactionRestoreClassificationResult result;
  result.status = EvidenceOkStatus();
  result.wal_required = false;
  if (caller_requires_wal) {
    result.status = EvidenceErrorStatus();
    result.restore_allowed = false;
    result.diagnostic = MakeTransactionEvidenceDiagnostic(result.status,
                                                         "SB-MGA-WAL-NOT-AUTHORITY",
                                                         "transaction.evidence.wal_not_authority",
                                                         "restore classification uses MGA inventory and lineage evidence");
    return result;
  }
  if (schema_epoch.empty() || snapshot_capsule.empty()) {
    result.status = EvidenceErrorStatus();
    result.restore_allowed = false;
    result.diagnostic = MakeTransactionEvidenceDiagnostic(result.status,
                                                         "SB-MGA-RESTORE-CONTEXT-MISSING",
                                                         "transaction.evidence.restore_context_missing",
                                                         "schema_epoch and snapshot_capsule are required");
    return result;
  }
  result.records = BuildTransactionLineageEvidence(inventory, std::move(schema_epoch), std::move(snapshot_capsule));
  for (const auto& record : result.records) {
    if (record.restore_classification == "refuse_fail_closed") {
      result.restore_allowed = false;
    }
  }
  if (!result.restore_allowed) {
    result.status = EvidenceErrorStatus();
    result.diagnostic = MakeTransactionEvidenceDiagnostic(result.status,
                                                         "SB-MGA-RESTORE-CLASSIFICATION-REFUSED",
                                                         "transaction.evidence.restore_classification_refused",
                                                         "inventory contains prepared, limbo, recovering, failed, or ambiguous state");
  }
  return result;
}

DiagnosticRecord MakeTransactionEvidenceDiagnostic(Status status,
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
                        "transaction.mga.evidence");
}

}  // namespace scratchbird::transaction::mga

// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "copy_on_write.hpp"

#include <utility>
#include <vector>

namespace scratchbird::transaction::mga {
namespace {

using scratchbird::core::platform::DiagnosticArgument;
using scratchbird::core::platform::MakeDiagnostic;
using scratchbird::core::platform::Severity;
using scratchbird::core::platform::StatusCode;
using scratchbird::core::platform::Subsystem;

Status CopyOnWriteOkStatus() {
  return {StatusCode::ok, Severity::info, Subsystem::transaction_mga};
}

Status CopyOnWriteWarningStatus() {
  return {StatusCode::platform_required_feature_missing, Severity::warning, Subsystem::transaction_mga};
}

Status CopyOnWriteErrorStatus() {
  return {StatusCode::platform_required_feature_missing, Severity::error, Subsystem::transaction_mga};
}

bool IsLegalPhaseTransition(CopyOnWriteMutationPhase from, CopyOnWriteMutationPhase to) {
  switch (from) {
    case CopyOnWriteMutationPhase::planned:
      return to == CopyOnWriteMutationPhase::base_version_locked ||
             to == CopyOnWriteMutationPhase::new_version_allocated ||
             to == CopyOnWriteMutationPhase::rollback_pending;
    case CopyOnWriteMutationPhase::base_version_locked:
      return to == CopyOnWriteMutationPhase::new_version_allocated ||
             to == CopyOnWriteMutationPhase::rollback_pending ||
             to == CopyOnWriteMutationPhase::recovery_required;
    case CopyOnWriteMutationPhase::new_version_allocated:
      return to == CopyOnWriteMutationPhase::payload_written_unpublished ||
             to == CopyOnWriteMutationPhase::rollback_pending ||
             to == CopyOnWriteMutationPhase::recovery_required;
    case CopyOnWriteMutationPhase::payload_written_unpublished:
      return to == CopyOnWriteMutationPhase::publish_pending_transaction ||
             to == CopyOnWriteMutationPhase::rollback_pending ||
             to == CopyOnWriteMutationPhase::recovery_required;
    case CopyOnWriteMutationPhase::publish_pending_transaction:
      return to == CopyOnWriteMutationPhase::published ||
             to == CopyOnWriteMutationPhase::rollback_pending ||
             to == CopyOnWriteMutationPhase::recovery_required;
    case CopyOnWriteMutationPhase::rollback_pending:
      return to == CopyOnWriteMutationPhase::rollback_complete ||
             to == CopyOnWriteMutationPhase::recovery_required;
    case CopyOnWriteMutationPhase::published:
    case CopyOnWriteMutationPhase::rollback_complete:
    case CopyOnWriteMutationPhase::recovery_required:
    case CopyOnWriteMutationPhase::unknown:
      return false;
  }
  return false;
}

RowVersionState ResultingStateForMutation(CopyOnWriteMutationKind kind) {
  switch (kind) {
    case CopyOnWriteMutationKind::insert:
    case CopyOnWriteMutationKind::update:
    case CopyOnWriteMutationKind::system_catalog_update:
      return RowVersionState::uncommitted;
    case CopyOnWriteMutationKind::delete_row:
      return RowVersionState::delete_marker;
    case CopyOnWriteMutationKind::unknown:
      return RowVersionState::unknown;
  }
  return RowVersionState::unknown;
}

CleanupEligibilityResult BlockedCleanup(CleanupEligibilityDecision decision,
                                        CleanupHoldKind hold_kind,
                                        std::string detail) {
  CleanupEligibilityResult result;
  result.status = CopyOnWriteWarningStatus();
  result.decision = decision;
  result.blocking_hold = hold_kind;
  result.diagnostic = MakeCopyOnWriteDiagnostic(result.status,
                                                "SB-COW-CLEANUP-BLOCKED",
                                                "copy_on_write.cleanup_blocked",
                                                std::move(detail));
  return result;
}

}  // namespace

const char* CopyOnWriteMutationKindName(CopyOnWriteMutationKind kind) {
  switch (kind) {
    case CopyOnWriteMutationKind::insert: return "insert";
    case CopyOnWriteMutationKind::update: return "update";
    case CopyOnWriteMutationKind::delete_row: return "delete_row";
    case CopyOnWriteMutationKind::system_catalog_update: return "system_catalog_update";
    case CopyOnWriteMutationKind::unknown: return "unknown";
  }
  return "unknown";
}

const char* CopyOnWriteMutationPhaseName(CopyOnWriteMutationPhase phase) {
  switch (phase) {
    case CopyOnWriteMutationPhase::planned: return "planned";
    case CopyOnWriteMutationPhase::base_version_locked: return "base_version_locked";
    case CopyOnWriteMutationPhase::new_version_allocated: return "new_version_allocated";
    case CopyOnWriteMutationPhase::payload_written_unpublished: return "payload_written_unpublished";
    case CopyOnWriteMutationPhase::publish_pending_transaction: return "publish_pending_transaction";
    case CopyOnWriteMutationPhase::published: return "published";
    case CopyOnWriteMutationPhase::rollback_pending: return "rollback_pending";
    case CopyOnWriteMutationPhase::rollback_complete: return "rollback_complete";
    case CopyOnWriteMutationPhase::recovery_required: return "recovery_required";
    case CopyOnWriteMutationPhase::unknown: return "unknown";
  }
  return "unknown";
}

const char* CleanupHoldKindName(CleanupHoldKind kind) {
  switch (kind) {
    case CleanupHoldKind::none: return "none";
    case CleanupHoldKind::oldest_interesting_transaction: return "oldest_interesting_transaction";
    case CleanupHoldKind::oldest_active_transaction: return "oldest_active_transaction";
    case CleanupHoldKind::oldest_snapshot_transaction: return "oldest_snapshot_transaction";
    case CleanupHoldKind::limbo_transaction: return "limbo_transaction";
    case CleanupHoldKind::recovery_required: return "recovery_required";
    case CleanupHoldKind::archive_required: return "archive_required";
    case CleanupHoldKind::backup_required: return "backup_required";
    case CleanupHoldKind::management_operation: return "management_operation";
    case CleanupHoldKind::legal_hold: return "legal_hold";
    case CleanupHoldKind::admin_hold: return "admin_hold";
    case CleanupHoldKind::unknown: return "unknown";
  }
  return "unknown";
}

const char* CleanupEligibilityDecisionName(CleanupEligibilityDecision decision) {
  switch (decision) {
    case CleanupEligibilityDecision::eligible_requires_authority: return "eligible_requires_authority";
    case CleanupEligibilityDecision::eligible_authoritative: return "eligible_authoritative";
    case CleanupEligibilityDecision::blocked_by_horizon: return "blocked_by_horizon";
    case CleanupEligibilityDecision::blocked_by_limbo: return "blocked_by_limbo";
    case CleanupEligibilityDecision::blocked_by_recovery: return "blocked_by_recovery";
    case CleanupEligibilityDecision::blocked_by_archive_or_backup: return "blocked_by_archive_or_backup";
    case CleanupEligibilityDecision::unknown: return "unknown";
  }
  return "unknown";
}

CopyOnWriteMutationResult PlanCopyOnWriteMutation(const CopyOnWriteMutationIntent& intent) {
  CopyOnWriteMutationState state;
  state.intent = intent;
  state.phase = CopyOnWriteMutationPhase::planned;
  state.resulting_row_state = ResultingStateForMutation(intent.kind);
  state.evidence_record_required = true;
  state.evidence_record_written = false;
  return ValidateCopyOnWriteMutationState(state);
}

CopyOnWriteMutationResult PlanLocalCopyOnWriteMutationForTransaction(const TransactionInventoryEntry& entry,
                                                                     RowIdentity row,
                                                                     CopyOnWriteMutationKind kind,
                                                                     u64 base_version_sequence,
                                                                     u64 new_version_sequence) {
  if (entry.state != TransactionState::active && entry.state != TransactionState::preparing &&
      entry.state != TransactionState::prepared) {
    CopyOnWriteMutationResult result;
    result.status = CopyOnWriteErrorStatus();
    result.diagnostic = MakeCopyOnWriteDiagnostic(result.status,
                                                  "SB-TXN-COW-UNSUPPORTED-TRANSACTION-STATE",
                                                  "copy_on_write.unsupported_transaction_state",
                                                  TransactionStateName(entry.state));
    return result;
  }
  CopyOnWriteMutationIntent intent;
  intent.kind = kind;
  intent.transaction = entry.identity;
  intent.row = row;
  intent.base_version_sequence = base_version_sequence;
  intent.new_version_sequence = new_version_sequence;
  intent.has_base_version = kind != CopyOnWriteMutationKind::insert;
  intent.payload_required = kind != CopyOnWriteMutationKind::delete_row;
  intent.system_catalog_mutation = kind == CopyOnWriteMutationKind::system_catalog_update;
  return PlanCopyOnWriteMutation(intent);
}

CopyOnWriteMutationResult ValidateCopyOnWriteMutationState(const CopyOnWriteMutationState& mutation) {
  CopyOnWriteMutationResult result;
  result.status = CopyOnWriteOkStatus();
  result.mutation = mutation;

  if (mutation.intent.kind == CopyOnWriteMutationKind::unknown) {
    result.status = CopyOnWriteErrorStatus();
    result.diagnostic = MakeCopyOnWriteDiagnostic(result.status,
                                                  "SB-COW-UNKNOWN-MUTATION-KIND",
                                                  "copy_on_write.unknown_mutation_kind");
    return result;
  }

  TransactionIdentityResult transaction_result = ValidateTransactionIdentity(mutation.intent.transaction);
  if (!transaction_result.ok()) {
    result.status = transaction_result.status;
    result.diagnostic = transaction_result.diagnostic;
    return result;
  }

  RowIdentityResult row_result = ValidateRowIdentity(mutation.intent.row);
  if (!row_result.ok()) {
    result.status = row_result.status;
    result.diagnostic = row_result.diagnostic;
    return result;
  }

  if (mutation.intent.kind == CopyOnWriteMutationKind::insert && mutation.intent.has_base_version) {
    result.status = CopyOnWriteErrorStatus();
    result.diagnostic = MakeCopyOnWriteDiagnostic(result.status,
                                                  "SB-COW-INSERT-MUST-NOT-HAVE-BASE-VERSION",
                                                  "copy_on_write.insert_must_not_have_base_version");
    return result;
  }

  if (mutation.intent.kind != CopyOnWriteMutationKind::insert && !mutation.intent.has_base_version) {
    result.status = CopyOnWriteErrorStatus();
    result.diagnostic = MakeCopyOnWriteDiagnostic(result.status,
                                                  "SB-COW-MUTATION-REQUIRES-BASE-VERSION",
                                                  "copy_on_write.mutation_requires_base_version",
                                                  CopyOnWriteMutationKindName(mutation.intent.kind));
    return result;
  }

  if (mutation.intent.has_base_version &&
      mutation.intent.base_version_sequence == kInvalidRowVersionSequence) {
    result.status = CopyOnWriteErrorStatus();
    result.diagnostic = MakeCopyOnWriteDiagnostic(result.status,
                                                  "SB-COW-INVALID-BASE-VERSION-SEQUENCE",
                                                  "copy_on_write.invalid_base_version_sequence");
    return result;
  }

  if (mutation.intent.new_version_sequence == kInvalidRowVersionSequence) {
    result.status = CopyOnWriteErrorStatus();
    result.diagnostic = MakeCopyOnWriteDiagnostic(result.status,
                                                  "SB-COW-INVALID-NEW-VERSION-SEQUENCE",
                                                  "copy_on_write.invalid_new_version_sequence");
    return result;
  }

  if (mutation.intent.has_base_version &&
      mutation.intent.new_version_sequence <= mutation.intent.base_version_sequence) {
    result.status = CopyOnWriteErrorStatus();
    result.diagnostic = MakeCopyOnWriteDiagnostic(result.status,
                                                  "SB-COW-NEW-VERSION-NOT-AFTER-BASE",
                                                  "copy_on_write.new_version_not_after_base");
    return result;
  }

  if (mutation.phase == CopyOnWriteMutationPhase::unknown) {
    result.status = CopyOnWriteErrorStatus();
    result.diagnostic = MakeCopyOnWriteDiagnostic(result.status,
                                                  "SB-COW-UNKNOWN-MUTATION-PHASE",
                                                  "copy_on_write.unknown_mutation_phase");
    return result;
  }

  if (mutation.resulting_row_state == RowVersionState::unknown) {
    result.status = CopyOnWriteErrorStatus();
    result.diagnostic = MakeCopyOnWriteDiagnostic(result.status,
                                                  "SB-COW-UNKNOWN-RESULTING-ROW-STATE",
                                                  "copy_on_write.unknown_resulting_row_state");
    return result;
  }

  if (mutation.evidence_record_required && mutation.phase == CopyOnWriteMutationPhase::published &&
      !mutation.evidence_record_written) {
    result.status = CopyOnWriteErrorStatus();
    result.diagnostic = MakeCopyOnWriteDiagnostic(result.status,
                                                  "SB-COW-EVIDENCE-REQUIRED-BEFORE-PUBLISH",
                                                  "copy_on_write.evidence_required_before_publish");
    return result;
  }

  return result;
}

CopyOnWriteMutationResult AdvanceCopyOnWriteMutationPhase(const CopyOnWriteMutationState& mutation,
                                                          CopyOnWriteMutationPhase next_phase) {
  CopyOnWriteMutationResult validation = ValidateCopyOnWriteMutationState(mutation);
  if (!validation.ok()) {
    return validation;
  }

  if (!IsLegalPhaseTransition(mutation.phase, next_phase)) {
    validation.status = CopyOnWriteErrorStatus();
    validation.diagnostic = MakeCopyOnWriteDiagnostic(validation.status,
                                                      "SB-COW-ILLEGAL-PHASE-TRANSITION",
                                                      "copy_on_write.illegal_phase_transition",
                                                      std::string(CopyOnWriteMutationPhaseName(mutation.phase)) +
                                                          "->" + CopyOnWriteMutationPhaseName(next_phase));
    return validation;
  }

  validation.status = CopyOnWriteOkStatus();
  validation.mutation.phase = next_phase;
  return validation;
}

CleanupEligibilityResult EvaluateCleanupEligibility(
    const RowVersionMetadata& metadata,
    const CleanupHorizonVector& horizons) {
  RowVersionMetadataResult metadata_result = ValidateRowVersionMetadata(metadata);
  if (!metadata_result.ok()) {
    CleanupEligibilityResult result;
    result.status = metadata_result.status;
    result.decision = CleanupEligibilityDecision::unknown;
    result.diagnostic = metadata_result.diagnostic;
    return result;
  }

  if (metadata.state == RowVersionState::limbo) {
    return BlockedCleanup(CleanupEligibilityDecision::blocked_by_limbo,
                          CleanupHoldKind::limbo_transaction,
                          RowVersionStateName(metadata.state));
  }

  if (metadata.state == RowVersionState::recovery_required) {
    return BlockedCleanup(CleanupEligibilityDecision::blocked_by_recovery,
                          CleanupHoldKind::recovery_required,
                          RowVersionStateName(metadata.state));
  }

  for (const CleanupHorizon& horizon : horizons.horizons) {
    if (!horizon.authoritative) {
      return BlockedCleanup(CleanupEligibilityDecision::blocked_by_horizon,
                            horizon.hold_kind,
                            "non_authoritative_horizon:" + horizon.stable_name);
    }

    if (!horizon.horizon_transaction.valid()) {
      return BlockedCleanup(CleanupEligibilityDecision::blocked_by_horizon,
                            horizon.hold_kind,
                            "invalid_horizon:" + horizon.stable_name);
    }

    if (horizon.horizon_transaction.value <= metadata.identity.creator_transaction.local_id.value) {
      switch (horizon.hold_kind) {
        case CleanupHoldKind::limbo_transaction:
          return BlockedCleanup(CleanupEligibilityDecision::blocked_by_limbo,
                                horizon.hold_kind,
                                horizon.stable_name);
        case CleanupHoldKind::recovery_required:
          return BlockedCleanup(CleanupEligibilityDecision::blocked_by_recovery,
                                horizon.hold_kind,
                                horizon.stable_name);
        case CleanupHoldKind::archive_required:
        case CleanupHoldKind::backup_required:
          return BlockedCleanup(CleanupEligibilityDecision::blocked_by_archive_or_backup,
                                horizon.hold_kind,
                                horizon.stable_name);
        case CleanupHoldKind::legal_hold:
        case CleanupHoldKind::admin_hold:
        case CleanupHoldKind::none:
        case CleanupHoldKind::oldest_interesting_transaction:
        case CleanupHoldKind::oldest_active_transaction:
        case CleanupHoldKind::oldest_snapshot_transaction:
        case CleanupHoldKind::management_operation:
        case CleanupHoldKind::unknown:
          return BlockedCleanup(CleanupEligibilityDecision::blocked_by_horizon,
                                horizon.hold_kind,
                                horizon.stable_name);
      }
    }
  }

  CleanupEligibilityResult result;
  result.status = CopyOnWriteWarningStatus();
  result.decision = CleanupEligibilityDecision::eligible_requires_authority;
  result.blocking_hold = CleanupHoldKind::none;
  result.diagnostic = MakeCopyOnWriteDiagnostic(result.status,
                                                "SB-COW-CLEANUP-ELIGIBLE-REQUIRES-AUTHORITY",
                                                "copy_on_write.cleanup_eligible_requires_authority");
  return result;
}

DiagnosticRecord MakeCopyOnWriteDiagnostic(Status status,
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
                        "transaction.mga.copy_on_write");
}

}  // namespace scratchbird::transaction::mga

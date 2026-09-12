// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "copy_on_write.hpp"

#include <array>
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

enum class CowCondition {
  invalid_kind,
  invalid_transaction_identity,
  invalid_row_identity,
  insert_has_base,
  base_required,
  invalid_base_sequence,
  invalid_new_sequence,
  nonincreasing_sequence,
  invalid_phase,
  invalid_row_state,
  evidence_required,
  illegal_transition,
  transaction_not_writable,
  invalid_row_metadata,
  read_only_transaction,
  cleanup_held,
  cleanup_authority_required,
};

struct CowDiagnosticDefinition {
  StatusCode status;
  const char* code;
  const char* message;
};
constexpr std::array<CowDiagnosticDefinition, 17> kCowDiagnostics{{
  {StatusCode::mga_cow_invalid_kind, "MGA.COW.INVALID_KIND", "copy_on_write.invalid_kind"},
  {StatusCode::mga_cow_invalid_transaction_identity, "MGA.COW.INVALID_TRANSACTION_IDENTITY", "copy_on_write.invalid_transaction_identity"},
  {StatusCode::mga_cow_invalid_row_identity, "MGA.COW.INVALID_ROW_IDENTITY", "copy_on_write.invalid_row_identity"},
  {StatusCode::mga_cow_insert_has_base, "MGA.COW.INSERT_HAS_BASE", "copy_on_write.insert_has_base"},
  {StatusCode::mga_cow_base_required, "MGA.COW.BASE_REQUIRED", "copy_on_write.base_required"},
  {StatusCode::mga_cow_invalid_base_sequence, "MGA.COW.INVALID_BASE_SEQUENCE", "copy_on_write.invalid_base_sequence"},
  {StatusCode::mga_cow_invalid_new_sequence, "MGA.COW.INVALID_NEW_SEQUENCE", "copy_on_write.invalid_new_sequence"},
  {StatusCode::mga_cow_nonincreasing_sequence, "MGA.COW.NONINCREASING_SEQUENCE", "copy_on_write.nonincreasing_sequence"},
  {StatusCode::mga_cow_invalid_phase, "MGA.COW.INVALID_PHASE", "copy_on_write.invalid_phase"},
  {StatusCode::mga_cow_invalid_row_state, "MGA.COW.INVALID_ROW_STATE", "copy_on_write.invalid_row_state"},
  {StatusCode::mga_cow_evidence_required, "MGA.COW.EVIDENCE_REQUIRED", "copy_on_write.evidence_required"},
  {StatusCode::mga_cow_illegal_transition, "MGA.COW.ILLEGAL_TRANSITION", "copy_on_write.illegal_transition"},
  {StatusCode::mga_cow_transaction_not_writable, "MGA.COW.TRANSACTION_NOT_WRITABLE", "copy_on_write.transaction_not_writable"},
  {StatusCode::mga_cow_invalid_row_metadata, "MGA.COW.INVALID_ROW_METADATA", "copy_on_write.invalid_row_metadata"},
  {StatusCode::mga_cow_read_only_transaction, "MGA.COW.READ_ONLY_TRANSACTION", "copy_on_write.read_only_transaction"},
  {StatusCode::ok, "MGA.COW.CLEANUP_HELD", "copy_on_write.cleanup_held"},
  {StatusCode::ok, "MGA.COW.CLEANUP_AUTHORITY_REQUIRED", "copy_on_write.cleanup_authority_required"},
}};

template <typename Result>
void SetCowDiagnostic(Result& result, CowCondition condition, std::string detail = {}) {
  const auto& definition = kCowDiagnostics.at(static_cast<std::size_t>(condition));
  std::vector<DiagnosticArgument> arguments;
  if (!detail.empty()) arguments.push_back({"detail", std::move(detail)});
  const Status status{definition.status, definition.status == StatusCode::ok ? Severity::info : Severity::error,
                      Subsystem::transaction_mga};
  auto diagnostic = MakeDiagnostic(status.code, status.severity, status.subsystem,
      definition.code, definition.message, std::move(arguments), {}, "transaction.mga.copy_on_write");
  result.diagnostic = std::move(diagnostic);
  result.status = status;
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
  result.decision = decision;
  result.blocking_hold = hold_kind;
  SetCowDiagnostic(result, CowCondition::cleanup_held,
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

CopyOnWriteTransactionStateResult ValidateCopyOnWriteTransactionState(const TransactionInventoryEntry& entry) {
  CopyOnWriteTransactionStateResult result;
  if (entry.state == TransactionState::read_only_active) {
    SetCowDiagnostic(result, CowCondition::read_only_transaction);
    return result;
  }
  if (entry.state != TransactionState::active || entry.rollback_only) {
    SetCowDiagnostic(result, CowCondition::transaction_not_writable,
                     TransactionStateName(entry.state));
    return result;
  }
  result.status = CopyOnWriteOkStatus();
  return result;
}

CopyOnWriteMutationResult PlanLocalCopyOnWriteMutationForTransaction(const TransactionInventoryEntry& entry,
                                                                     RowIdentity row,
                                                                     CopyOnWriteMutationKind kind,
                                                                     u64 base_version_sequence,
                                                                     u64 new_version_sequence) {
  const auto admission = ValidateCopyOnWriteTransactionState(entry);
  if (!admission.ok()) {
    CopyOnWriteMutationResult result;
    result.status = admission.status;
    result.diagnostic = admission.diagnostic;
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

  if (mutation.intent.kind >= CopyOnWriteMutationKind::unknown) {
    SetCowDiagnostic(result, CowCondition::invalid_kind);
    return result;
  }

  TransactionIdentityResult transaction_result = ValidateTransactionIdentity(mutation.intent.transaction);
  if (!transaction_result.ok() ||
      (mutation.intent.transaction.scope != TransactionScope::local_node &&
       mutation.intent.transaction.scope != TransactionScope::cluster_global)) {
    SetCowDiagnostic(result, CowCondition::invalid_transaction_identity);
    return result;
  }

  RowIdentityResult row_result = ValidateRowIdentity(mutation.intent.row);
  if (!row_result.ok()) {
    SetCowDiagnostic(result, CowCondition::invalid_row_identity);
    return result;
  }

  if (mutation.intent.kind == CopyOnWriteMutationKind::insert && mutation.intent.has_base_version) {
    SetCowDiagnostic(result, CowCondition::insert_has_base);
    return result;
  }

  if (mutation.intent.kind != CopyOnWriteMutationKind::insert && !mutation.intent.has_base_version) {
    SetCowDiagnostic(result, CowCondition::base_required,
                     CopyOnWriteMutationKindName(mutation.intent.kind));
    return result;
  }

  if (mutation.intent.has_base_version &&
      mutation.intent.base_version_sequence == kInvalidRowVersionSequence) {
    SetCowDiagnostic(result, CowCondition::invalid_base_sequence);
    return result;
  }

  if (mutation.intent.new_version_sequence == kInvalidRowVersionSequence) {
    SetCowDiagnostic(result, CowCondition::invalid_new_sequence);
    return result;
  }

  if (mutation.intent.has_base_version &&
      mutation.intent.new_version_sequence <= mutation.intent.base_version_sequence) {
    SetCowDiagnostic(result, CowCondition::nonincreasing_sequence);
    return result;
  }

  if (mutation.phase >= CopyOnWriteMutationPhase::unknown) {
    SetCowDiagnostic(result, CowCondition::invalid_phase);
    return result;
  }

  if (mutation.resulting_row_state == RowVersionState::unknown ||
      mutation.resulting_row_state > RowVersionState::recovery_required) {
    SetCowDiagnostic(result, CowCondition::invalid_row_state);
    return result;
  }

  if (mutation.evidence_record_required && mutation.phase == CopyOnWriteMutationPhase::published &&
      !mutation.evidence_record_written) {
    SetCowDiagnostic(result, CowCondition::evidence_required);
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
    SetCowDiagnostic(validation, CowCondition::illegal_transition,
                                                      std::string(CopyOnWriteMutationPhaseName(mutation.phase)) +
                                                          "->" + CopyOnWriteMutationPhaseName(next_phase));
    return validation;
  }

  // Edge legality does not imply candidate validity: publication has an
  // evidence prerequisite that is not required of the pending state. Validate
  // before returning the advanced state, and preserve the original on refusal.
  CopyOnWriteMutationState candidate = mutation;
  candidate.phase = next_phase;
  auto advanced = ValidateCopyOnWriteMutationState(candidate);
  if (!advanced.ok()) {
    validation.status = advanced.status;
    validation.diagnostic = std::move(advanced.diagnostic);
    return validation;
  }
  return advanced;
}

CleanupEligibilityResult EvaluateCleanupEligibility(
    const RowVersionMetadata& metadata,
    const CleanupHorizonVector& horizons) {
  RowVersionMetadataResult metadata_result = ValidateRowVersionMetadata(metadata);
  if (!metadata_result.ok() || metadata.state > RowVersionState::recovery_required ||
      metadata.creator_transaction_state > TransactionState::read_only_active ||
      (metadata.identity.creator_transaction.scope != TransactionScope::local_node &&
       metadata.identity.creator_transaction.scope != TransactionScope::cluster_global)) {
    CleanupEligibilityResult result;
    result.decision = CleanupEligibilityDecision::unknown;
    SetCowDiagnostic(result, CowCondition::invalid_row_metadata);
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
    if (horizon.hold_kind >= CleanupHoldKind::unknown) {
      return BlockedCleanup(CleanupEligibilityDecision::blocked_by_horizon,
                            CleanupHoldKind::unknown, "unknown_hold_kind");
    }
    if (!horizon.authoritative) {
      return BlockedCleanup(CleanupEligibilityDecision::blocked_by_horizon,
                            horizon.hold_kind,
                            "non_authoritative_horizon");
    }

    if (!horizon.horizon_transaction.valid()) {
      return BlockedCleanup(CleanupEligibilityDecision::blocked_by_horizon,
                            horizon.hold_kind,
                            "invalid_horizon");
    }

    if (horizon.horizon_transaction.value <= metadata.identity.creator_transaction.local_id.value) {
      switch (horizon.hold_kind) {
        case CleanupHoldKind::limbo_transaction:
          return BlockedCleanup(CleanupEligibilityDecision::blocked_by_limbo,
                                horizon.hold_kind,
                                CleanupHoldKindName(horizon.hold_kind));
        case CleanupHoldKind::recovery_required:
          return BlockedCleanup(CleanupEligibilityDecision::blocked_by_recovery,
                                horizon.hold_kind,
                                CleanupHoldKindName(horizon.hold_kind));
        case CleanupHoldKind::archive_required:
        case CleanupHoldKind::backup_required:
          return BlockedCleanup(CleanupEligibilityDecision::blocked_by_archive_or_backup,
                                horizon.hold_kind,
                                CleanupHoldKindName(horizon.hold_kind));
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
                                CleanupHoldKindName(horizon.hold_kind));
      }
    }
  }

  CleanupEligibilityResult result;
  result.decision = CleanupEligibilityDecision::eligible_requires_authority;
  result.blocking_hold = CleanupHoldKind::none;
  SetCowDiagnostic(result, CowCondition::cleanup_authority_required);
  return result;
}

}  // namespace scratchbird::transaction::mga

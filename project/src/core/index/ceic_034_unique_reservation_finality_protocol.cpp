// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "ceic_034_unique_reservation_finality_protocol.hpp"

// CEIC_034_UNIQUE_RESERVATION_FINALITY_PROTOCOL

#include <utility>

namespace scratchbird::core::index {
namespace {

using scratchbird::core::platform::Severity;
using scratchbird::core::platform::StatusCode;
using scratchbird::core::platform::Subsystem;
using scratchbird::core::platform::UuidKind;

Status OkStatus() {
  return Status{StatusCode::ok, Severity::info, Subsystem::engine};
}

Status RefuseStatus() {
  return Status{StatusCode::platform_required_feature_missing,
                Severity::error,
                Subsystem::engine};
}

const char* BoolText(bool value) { return value ? "true" : "false"; }

void AddEvidence(UniqueReservationFinalityProtocolResult* result,
                 std::string key,
                 std::string value) {
  result->evidence.push_back(std::move(key) + "=" + std::move(value));
}

void AddBoolEvidence(UniqueReservationFinalityProtocolResult* result,
                     const char* key,
                     bool value) {
  AddEvidence(result, key, BoolText(value));
}

void AddU64Evidence(UniqueReservationFinalityProtocolResult* result,
                    const char* key,
                    u64 value) {
  AddEvidence(result, key, std::to_string(value));
}

bool SameUuid(const TypedUuid& left, const TypedUuid& right) {
  return left.kind == right.kind && left.valid() && right.valid() &&
         left.value == right.value;
}

bool ValidUuidKind(const TypedUuid& uuid, UuidKind kind) {
  return uuid.kind == kind && uuid.valid();
}

bool EvidenceHasExact(const std::vector<std::string>& evidence,
                      const std::string& key,
                      const std::string& value) {
  const std::string expected = key + "=" + value;
  for (const auto& row : evidence) {
    if (row == expected) {
      return true;
    }
  }
  return false;
}

bool CEIC033ClosureAdmitted(
    const BtreeUniqueDurableProviderClosureResult& closure) {
  return closure.ok() &&
         closure.closure_status ==
             BtreeUniqueDurableProviderClosureStatus::
                 admitted_durable_provider_evidence &&
         closure.durable_provider_evidence &&
         closure.btree_unique_provider_closure_claimed &&
         !closure.enterprise_ready_claimed &&
         !closure.all_index_readiness_claimed &&
         EvidenceHasExact(
             closure.evidence,
             "ceic_search_key",
             "CEIC_033_BTREE_UNIQUE_DURABLE_PROVIDER_CLOSURE") &&
         EvidenceHasExact(closure.evidence, "family", "unique_btree");
}

bool DuplicatePreflightValid(
    const UniqueReservationDuplicatePreflightProof& proof) {
  return proof.duplicate_preflight_performed &&
         proof.duplicate_preflight_engine_mga_bound &&
         proof.duplicate_preflight_result_resolved &&
         proof.duplicate_preflight_before_reservation &&
         !proof.duplicate_preflight_evidence_id.empty();
}

bool ReservationIdentityValid(
    const UniqueReservationIdentityProof& identity,
    const UniqueReservationMGATransactionBindingProof& binding) {
  return ValidUuidKind(identity.index_uuid, UuidKind::object) &&
         ValidUuidKind(identity.table_uuid, UuidKind::object) &&
         ValidUuidKind(identity.constraint_uuid, UuidKind::object) &&
         ValidUuidKind(identity.row_uuid, UuidKind::row) &&
         ValidUuidKind(identity.version_uuid, UuidKind::row) &&
         ValidUuidKind(identity.transaction_uuid, UuidKind::transaction) &&
         identity.local_transaction_id != 0 &&
         !identity.encoded_key.empty() &&
         identity.reservation_identity_bound_to_duplicate_preflight &&
         identity.reservation_identity_bound_to_ledger &&
         identity.reservation_identity_bound_to_unique_btree &&
         !identity.reservation_evidence_id.empty() &&
         SameUuid(identity.transaction_uuid, binding.transaction_uuid) &&
         identity.local_transaction_id == binding.local_transaction_id;
}

bool MGATransactionStateCanOwnReservation(TransactionState state) {
  return state == TransactionState::active ||
         state == TransactionState::preparing ||
         state == TransactionState::prepared ||
         state == TransactionState::committing;
}

bool MGATransactionBindingValid(
    const UniqueReservationMGATransactionBindingProof& proof,
    const UniqueReservationIdentityProof& identity) {
  return ValidUuidKind(proof.transaction_uuid, UuidKind::transaction) &&
         proof.local_transaction_id != 0 &&
         SameUuid(proof.transaction_uuid, identity.transaction_uuid) &&
         proof.local_transaction_id == identity.local_transaction_id &&
         MGATransactionStateCanOwnReservation(proof.observed_state) &&
         proof.engine_transaction_handle_bound &&
         proof.engine_mga_inventory_present &&
         proof.engine_mga_inventory_authoritative &&
         proof.durable_transaction_inventory_authoritative &&
         proof.transaction_matches_reservation &&
         proof.cleanup_horizon_engine_bound &&
         !proof.inventory_evidence_id.empty() &&
         !proof.transaction_evidence_token.empty();
}

bool ReservationResultValid(
    const UniqueIndexReservationResult& result) {
  return result.ok() && result.reserved &&
         result.decision == UniqueIndexReservationDecision::reserved &&
         result.conflict_state == UniqueIndexReservationConflictState::none &&
         result.reservation_sequence != 0;
}

bool DuplicateConflictRefused(
    const UniqueIndexReservationResult& result,
    UniqueReservationConflictOutcome outcome) {
  return !result.ok() && result.conflict &&
         outcome == UniqueReservationConflictOutcome::duplicate_refused &&
         (result.conflict_state ==
              UniqueIndexReservationConflictState::committed_refused ||
          result.conflict_state ==
              UniqueIndexReservationConflictState::validated_refused ||
          result.conflict_state ==
              UniqueIndexReservationConflictState::
                  duplicate_in_transaction_refused ||
          result.conflict_state ==
              UniqueIndexReservationConflictState::refuse_candidate);
}

bool ActiveConflictRefused(
    const UniqueIndexReservationResult& result,
    UniqueReservationConflictOutcome outcome) {
  return !result.ok() && result.conflict &&
         outcome == UniqueReservationConflictOutcome::active_conflict_refused &&
         (result.conflict_state ==
              UniqueIndexReservationConflictState::refuse_candidate ||
          result.conflict_state ==
              UniqueIndexReservationConflictState::unsafe_refused ||
          result.conflict_state ==
              UniqueIndexReservationConflictState::wait_for_mga);
}

bool DeferredCommitProbeValid(
    const UniqueReservationCommitProbeProof& proof) {
  return proof.commit_probe_present &&
         proof.commit_probe_engine_mga_bound &&
         proof.commit_probe_scanned_reservation_ledger &&
         proof.commit_probe_result_resolved &&
         !proof.commit_probe_evidence_id.empty() &&
         proof.commit_probe_result.ok() &&
         proof.commit_probe_result.commit_validation_passed &&
         proof.commit_probe_result.decision ==
             UniqueIndexReservationDecision::commit_validated;
}

bool RollbackRetryCleanupProofValid(
    const UniqueReservationRollbackRetryCleanupProof& proof) {
  if (!proof.rollback_release_supported ||
      !proof.retry_release_supported ||
      !proof.cleanup_evidence_present ||
      !proof.cleanup_horizon_engine_bound ||
      !proof.reservation_ledger_cleanup_evidence_present ||
      proof.cleanup_evidence_id.empty()) {
    return false;
  }
  if (!proof.ledger_cleanup_result_present) {
    return true;
  }
  return proof.cleanup_result.ok() &&
         proof.cleanup_result.rollback_cleanup_performed &&
         proof.cleanup_result.decision ==
             UniqueIndexReservationDecision::rollback_cleaned;
}

bool RollbackRetryReleaseOutcomeConsistent(
    UniqueReservationConflictOutcome outcome,
    const UniqueReservationRollbackRetryCleanupProof& proof) {
  if (outcome != UniqueReservationConflictOutcome::rollback_retry_released) {
    return true;
  }
  return proof.ledger_cleanup_result_present &&
         proof.cleanup_result.ok() &&
         proof.cleanup_result.rollback_cleanup_performed &&
         proof.cleanup_result.decision ==
             UniqueIndexReservationDecision::rollback_cleaned;
}

bool CrashWindowValid(const UniqueReservationCrashWindowProof& proof) {
  return proof.classification_present &&
         proof.classification_engine_mga_bound &&
         proof.classification !=
             UniqueReservationCrashWindowClassification::unknown &&
         !proof.classification_evidence_id.empty();
}

DiagnosticRecord MakeDiagnostic(
    Status status,
    UniqueReservationFinalityProtocolStatus protocol_status,
    const UniqueReservationFinalityProtocolRequest& request,
    std::string detail) {
  DiagnosticRecord record;
  record.status = status;
  record.diagnostic_code =
      std::string("INDEX.UNIQUE_RESERVATION_PROTOCOL.") +
      UniqueReservationFinalityProtocolStatusName(protocol_status);
  record.message_key =
      std::string("index.unique_reservation_protocol.") +
      UniqueReservationFinalityProtocolStatusName(protocol_status);
  record.arguments.push_back({"family", IndexFamilyName(request.family)});
  record.arguments.push_back(
      {"mode", UniqueReservationFinalityProtocolModeName(request.mode)});
  record.arguments.push_back(
      {"conflict_outcome",
       UniqueReservationConflictOutcomeName(request.conflict_outcome)});
  if (!detail.empty()) {
    record.arguments.push_back({"detail", std::move(detail)});
  }
  record.source_component =
      "sb_core_index.ceic034_unique_reservation_finality_protocol";
  return record;
}

void AddBaseEvidence(UniqueReservationFinalityProtocolResult* result,
                     const UniqueReservationFinalityProtocolRequest& request) {
  AddEvidence(result, "ceic_search_key",
              "CEIC_034_UNIQUE_RESERVATION_FINALITY_PROTOCOL");
  AddEvidence(result, "family", IndexFamilyName(request.family));
  AddEvidence(result, "mode",
              UniqueReservationFinalityProtocolModeName(request.mode));
  AddEvidence(result, "ceic033_closure_status",
              BtreeUniqueDurableProviderClosureStatusName(
                  request.ceic033_closure.closure_status));
  AddBoolEvidence(result, "ceic033_closure_admitted",
                  CEIC033ClosureAdmitted(request.ceic033_closure));
  AddBoolEvidence(result, "duplicate_preflight_valid",
                  DuplicatePreflightValid(request.duplicate_preflight));
  AddBoolEvidence(result, "reservation_identity_valid",
                  ReservationIdentityValid(request.reservation_identity,
                                           request.mga_transaction_binding));
  AddBoolEvidence(result, "mga_transaction_binding_valid",
                  MGATransactionBindingValid(request.mga_transaction_binding,
                                             request.reservation_identity));
  AddBoolEvidence(result, "reservation_result_valid",
                  ReservationResultValid(request.reservation_result));
  AddBoolEvidence(result, "deferred_commit_probe_valid",
                  DeferredCommitProbeValid(request.commit_probe));
  AddBoolEvidence(result, "rollback_retry_cleanup_valid",
                  RollbackRetryCleanupProofValid(
                      request.rollback_retry_cleanup));
  AddEvidence(result, "conflict_outcome",
              UniqueReservationConflictOutcomeName(request.conflict_outcome));
  AddEvidence(result, "crash_window_classification",
              UniqueReservationCrashWindowClassificationName(
                  request.crash_window.classification));
  AddBoolEvidence(result, "crash_window_valid",
                  CrashWindowValid(request.crash_window));
  AddBoolEvidence(result, "cleanup_horizon_engine_bound",
                  request.rollback_retry_cleanup.cleanup_horizon_engine_bound &&
                      request.mga_transaction_binding.cleanup_horizon_engine_bound);
  AddBoolEvidence(result, "reference_local_participation",
                  request.reference_local_participation);
  AddBoolEvidence(result, "policy_local_participation",
                  request.policy_local_participation);
  AddBoolEvidence(result, "cluster_local_participation",
                  request.cluster_local_participation);
  AddBoolEvidence(result, "authority_boundary_clear",
                  UniqueReservationProtocolAuthorityBoundaryClear(
                      request.authority_boundary));
  AddBoolEvidence(result, "unique_protocol_evidence_claimed",
                  request.unique_protocol_evidence_claimed);
  AddBoolEvidence(result, "enterprise_ready_claimed",
                  request.enterprise_ready_claimed);
  AddBoolEvidence(result, "all_index_readiness_claimed",
                  request.all_index_readiness_claimed);
  AddBoolEvidence(result, "ceic_041_crash_matrix_claimed",
                  request.ceic_041_crash_matrix_claimed);
  AddBoolEvidence(result, "ceic_042_readiness_gate_claimed",
                  request.ceic_042_readiness_gate_claimed);
  AddU64Evidence(result, "local_transaction_id",
                 request.reservation_identity.local_transaction_id);
  AddEvidence(result, "mga_inventory_evidence_id",
              request.mga_transaction_binding.inventory_evidence_id);
  AddEvidence(result, "duplicate_preflight_evidence_id",
              request.duplicate_preflight.duplicate_preflight_evidence_id);
  AddEvidence(result, "reservation_evidence_id",
              request.reservation_identity.reservation_evidence_id);
  AddEvidence(result, "cleanup_evidence_id",
              request.rollback_retry_cleanup.cleanup_evidence_id);
  AddEvidence(result, "crash_window_evidence_id",
              request.crash_window.classification_evidence_id);
  for (const auto& evidence : request.evidence) {
    AddEvidence(result, "protocol_evidence", evidence);
  }
}

UniqueReservationFinalityProtocolResult BaseResult(
    const UniqueReservationFinalityProtocolRequest& request) {
  UniqueReservationFinalityProtocolResult result;
  result.status = OkStatus();
  result.conflict_outcome = request.conflict_outcome;
  AddBaseEvidence(&result, request);
  return result;
}

UniqueReservationFinalityProtocolResult RefuseProtocol(
    const UniqueReservationFinalityProtocolRequest& request,
    UniqueReservationFinalityProtocolStatus protocol_status,
    std::string detail) {
  auto result = BaseResult(request);
  result.status = RefuseStatus();
  result.admitted = false;
  result.fail_closed = true;
  result.protocol_status = protocol_status;
  result.enterprise_ready_claimed = false;
  result.all_index_readiness_claimed = false;
  result.ceic_041_crash_matrix_claimed = false;
  result.ceic_042_readiness_gate_claimed = false;
  result.diagnostic =
      MakeDiagnostic(result.status, protocol_status, request, std::move(detail));
  AddEvidence(&result, "protocol_status",
              UniqueReservationFinalityProtocolStatusName(protocol_status));
  AddBoolEvidence(&result, "result_unique_protocol_evidence",
                  result.unique_protocol_evidence);
  AddBoolEvidence(&result, "result_enterprise_ready_claimed",
                  result.enterprise_ready_claimed);
  AddBoolEvidence(&result, "result_all_index_readiness_claimed",
                  result.all_index_readiness_claimed);
  AddBoolEvidence(&result, "result_ceic_041_crash_matrix_claimed",
                  result.ceic_041_crash_matrix_claimed);
  AddBoolEvidence(&result, "result_ceic_042_readiness_gate_claimed",
                  result.ceic_042_readiness_gate_claimed);
  AddEvidence(&result, "diagnostic_code", result.diagnostic.diagnostic_code);
  return result;
}

}  // namespace

const char* UniqueReservationFinalityProtocolModeName(
    UniqueReservationFinalityProtocolMode mode) {
  switch (mode) {
    case UniqueReservationFinalityProtocolMode::immediate:
      return "IMMEDIATE";
    case UniqueReservationFinalityProtocolMode::deferred:
      return "DEFERRED";
  }
  return "UNKNOWN";
}

const char* UniqueReservationConflictOutcomeName(
    UniqueReservationConflictOutcome outcome) {
  switch (outcome) {
    case UniqueReservationConflictOutcome::none:
      return "NONE";
    case UniqueReservationConflictOutcome::duplicate_refused:
      return "DUPLICATE_REFUSED";
    case UniqueReservationConflictOutcome::active_conflict_refused:
      return "ACTIVE_CONFLICT_REFUSED";
    case UniqueReservationConflictOutcome::rollback_retry_released:
      return "ROLLBACK_RETRY_RELEASED";
    case UniqueReservationConflictOutcome::unresolved:
      return "UNRESOLVED";
  }
  return "UNKNOWN";
}

const char* UniqueReservationCrashWindowClassificationName(
    UniqueReservationCrashWindowClassification classification) {
  switch (classification) {
    case UniqueReservationCrashWindowClassification::clean_no_crash:
      return "CLEAN_NO_CRASH";
    case UniqueReservationCrashWindowClassification::
        crash_before_reservation_publish:
      return "CRASH_BEFORE_RESERVATION_PUBLISH";
    case UniqueReservationCrashWindowClassification::
        crash_after_reservation_before_commit_probe:
      return "CRASH_AFTER_RESERVATION_BEFORE_COMMIT_PROBE";
    case UniqueReservationCrashWindowClassification::
        crash_after_commit_probe_before_mga_commit:
      return "CRASH_AFTER_COMMIT_PROBE_BEFORE_MGA_COMMIT";
    case UniqueReservationCrashWindowClassification::
        crash_after_mga_commit_before_ledger_publish:
      return "CRASH_AFTER_MGA_COMMIT_BEFORE_LEDGER_PUBLISH";
    case UniqueReservationCrashWindowClassification::
        retry_after_rollback_cleanup:
      return "RETRY_AFTER_ROLLBACK_CLEANUP";
    case UniqueReservationCrashWindowClassification::unknown:
      return "UNKNOWN";
  }
  return "UNKNOWN";
}

const char* UniqueReservationFinalityProtocolStatusName(
    UniqueReservationFinalityProtocolStatus status) {
  switch (status) {
    case UniqueReservationFinalityProtocolStatus::
        admitted_immediate_protocol_evidence:
      return "ADMITTED_IMMEDIATE_PROTOCOL_EVIDENCE";
    case UniqueReservationFinalityProtocolStatus::
        admitted_deferred_protocol_evidence:
      return "ADMITTED_DEFERRED_PROTOCOL_EVIDENCE";
    case UniqueReservationFinalityProtocolStatus::unsupported_family:
      return "UNSUPPORTED_FAMILY";
    case UniqueReservationFinalityProtocolStatus::ceic033_closure_not_admitted:
      return "CEIC033_CLOSURE_NOT_ADMITTED";
    case UniqueReservationFinalityProtocolStatus::duplicate_preflight_missing:
      return "DUPLICATE_PREFLIGHT_MISSING";
    case UniqueReservationFinalityProtocolStatus::reservation_identity_missing:
      return "RESERVATION_IDENTITY_MISSING";
    case UniqueReservationFinalityProtocolStatus::
        mga_transaction_binding_missing:
      return "MGA_TRANSACTION_BINDING_MISSING";
    case UniqueReservationFinalityProtocolStatus::
        deferred_commit_probe_missing:
      return "DEFERRED_COMMIT_PROBE_MISSING";
    case UniqueReservationFinalityProtocolStatus::
        rollback_retry_cleanup_missing:
      return "ROLLBACK_RETRY_CLEANUP_MISSING";
    case UniqueReservationFinalityProtocolStatus::duplicate_conflict_refused:
      return "DUPLICATE_CONFLICT_REFUSED";
    case UniqueReservationFinalityProtocolStatus::conflict_outcome_unresolved:
      return "CONFLICT_OUTCOME_UNRESOLVED";
    case UniqueReservationFinalityProtocolStatus::
        crash_window_classification_missing:
      return "CRASH_WINDOW_CLASSIFICATION_MISSING";
    case UniqueReservationFinalityProtocolStatus::
        cleanup_horizon_not_engine_bound:
      return "CLEANUP_HORIZON_NOT_ENGINE_BOUND";
    case UniqueReservationFinalityProtocolStatus::
        reference_policy_cluster_participation:
      return "REFERENCE_POLICY_CLUSTER_PARTICIPATION";
    case UniqueReservationFinalityProtocolStatus::forbidden_authority_claim:
      return "FORBIDDEN_AUTHORITY_CLAIM";
    case UniqueReservationFinalityProtocolStatus::readiness_overclaim:
      return "READINESS_OVERCLAIM";
    case UniqueReservationFinalityProtocolStatus::
        reservation_ledger_result_missing:
      return "RESERVATION_LEDGER_RESULT_MISSING";
    case UniqueReservationFinalityProtocolStatus::
        unique_protocol_evidence_missing:
      return "UNIQUE_PROTOCOL_EVIDENCE_MISSING";
    case UniqueReservationFinalityProtocolStatus::active_conflict_refused:
      return "ACTIVE_CONFLICT_REFUSED";
  }
  return "UNKNOWN";
}

bool UniqueReservationProtocolAuthorityBoundaryClear(
    const UniqueReservationProtocolAuthorityBoundary& boundary) {
  return !boundary.transaction_finality_authority &&
         !boundary.visibility_authority &&
         !boundary.authorization_security_authority &&
         !boundary.security_authority &&
         !boundary.recovery_authority &&
         !boundary.parser_authority &&
         !boundary.reference_authority &&
         !boundary.wal_authority &&
         !boundary.benchmark_authority &&
         !boundary.optimizer_plan_authority &&
         !boundary.index_finality_authority &&
         !boundary.provider_finality_authority &&
         !boundary.cluster_authority &&
         !boundary.agent_action_authority;
}

UniqueReservationFinalityProtocolResult
AdmitUniqueReservationFinalityProtocol(
    const UniqueReservationFinalityProtocolRequest& request) {
  if (request.family != IndexFamily::unique_btree) {
    return RefuseProtocol(
        request,
        UniqueReservationFinalityProtocolStatus::unsupported_family,
        "CEIC-034 closes only unique_btree reservation protocol evidence");
  }
  if (request.reference_local_participation ||
      request.policy_local_participation ||
      request.cluster_local_participation) {
    return RefuseProtocol(
        request,
        UniqueReservationFinalityProtocolStatus::
            reference_policy_cluster_participation,
        "reference policy and cluster paths cannot participate in local unique reservation protocol evidence");
  }
  if (request.enterprise_ready_claimed ||
      request.all_index_readiness_claimed ||
      request.ceic_041_crash_matrix_claimed ||
      request.ceic_042_readiness_gate_claimed) {
    return RefuseProtocol(
        request,
        UniqueReservationFinalityProtocolStatus::readiness_overclaim,
        "CEIC-034 must not claim CEIC-041 crash matrix CEIC-042 readiness gate all-index or enterprise readiness");
  }
  if (!UniqueReservationProtocolAuthorityBoundaryClear(
          request.authority_boundary)) {
    return RefuseProtocol(
        request,
        UniqueReservationFinalityProtocolStatus::forbidden_authority_claim,
        "unique reservation protocol evidence must not claim transaction finality visibility security recovery parser reference WAL benchmark optimizer plan index finality cluster or agent-action authority");
  }
  if (!request.unique_protocol_evidence_claimed) {
    return RefuseProtocol(
        request,
        UniqueReservationFinalityProtocolStatus::
            unique_protocol_evidence_missing,
        "CEIC-034 unique reservation protocol evidence must be explicitly claimed");
  }
  if (!CEIC033ClosureAdmitted(request.ceic033_closure)) {
    return RefuseProtocol(
        request,
        UniqueReservationFinalityProtocolStatus::ceic033_closure_not_admitted,
        "CEIC-033 unique_btree durable provider closure must be admitted before CEIC-034 can admit protocol evidence");
  }
  if (!DuplicatePreflightValid(request.duplicate_preflight)) {
    return RefuseProtocol(
        request,
        UniqueReservationFinalityProtocolStatus::duplicate_preflight_missing,
        "duplicate preflight must be present engine-MGA-bound resolved and ordered before reservation");
  }
  if (!MGATransactionBindingValid(request.mga_transaction_binding,
                                  request.reservation_identity)) {
    return RefuseProtocol(
        request,
        UniqueReservationFinalityProtocolStatus::
            mga_transaction_binding_missing,
        "reservation must be bound to engine transaction handle and durable MGA transaction inventory evidence");
  }
  if (!ReservationIdentityValid(request.reservation_identity,
                                request.mga_transaction_binding)) {
    return RefuseProtocol(
        request,
        UniqueReservationFinalityProtocolStatus::reservation_identity_missing,
        "reservation identity must bind unique index table constraint row version key and transaction to the duplicate preflight and ledger");
  }
  if (request.conflict_outcome ==
      UniqueReservationConflictOutcome::unresolved) {
    return RefuseProtocol(
        request,
        UniqueReservationFinalityProtocolStatus::conflict_outcome_unresolved,
        "unique reservation conflict outcome must be resolved before protocol evidence is admitted");
  }
  if (!CrashWindowValid(request.crash_window)) {
    return RefuseProtocol(
        request,
        UniqueReservationFinalityProtocolStatus::
            crash_window_classification_missing,
        "unique reservation crash window classification must be present and engine-MGA-bound");
  }
  if (!request.mga_transaction_binding.cleanup_horizon_engine_bound ||
      !request.rollback_retry_cleanup.cleanup_horizon_engine_bound) {
    return RefuseProtocol(
        request,
        UniqueReservationFinalityProtocolStatus::
            cleanup_horizon_not_engine_bound,
        "unique reservation cleanup horizon must be engine-bound");
  }
  if (!RollbackRetryCleanupProofValid(request.rollback_retry_cleanup)) {
    return RefuseProtocol(
        request,
        UniqueReservationFinalityProtocolStatus::
            rollback_retry_cleanup_missing,
        "rollback retry release and reservation ledger cleanup evidence are required");
  }
  if (!RollbackRetryReleaseOutcomeConsistent(
          request.conflict_outcome,
          request.rollback_retry_cleanup)) {
    return RefuseProtocol(
        request,
        UniqueReservationFinalityProtocolStatus::
            rollback_retry_cleanup_missing,
        "rollback/retry release outcome requires a successful reservation ledger cleanup result");
  }
  if (DuplicateConflictRefused(request.reservation_result,
                               request.conflict_outcome)) {
    return RefuseProtocol(
        request,
        UniqueReservationFinalityProtocolStatus::duplicate_conflict_refused,
        "duplicate unique reservation conflict was deterministically refused");
  }
  if (ActiveConflictRefused(request.reservation_result,
                            request.conflict_outcome)) {
    return RefuseProtocol(
        request,
        UniqueReservationFinalityProtocolStatus::active_conflict_refused,
        "active unique reservation conflict was deterministically refused");
  }
  if (request.conflict_outcome != UniqueReservationConflictOutcome::none &&
      request.conflict_outcome !=
          UniqueReservationConflictOutcome::rollback_retry_released) {
    return RefuseProtocol(
        request,
        UniqueReservationFinalityProtocolStatus::conflict_outcome_unresolved,
        "conflict outcome must match the reservation ledger result");
  }
  if (!ReservationResultValid(request.reservation_result)) {
    return RefuseProtocol(
        request,
        UniqueReservationFinalityProtocolStatus::
            reservation_ledger_result_missing,
        "unique reservation ledger must record a resolved reservation result");
  }
  if (request.mode == UniqueReservationFinalityProtocolMode::deferred &&
      !DeferredCommitProbeValid(request.commit_probe)) {
    return RefuseProtocol(
        request,
        UniqueReservationFinalityProtocolStatus::
            deferred_commit_probe_missing,
        "deferred unique mode requires commit-time probe evidence from the reservation ledger");
  }

  auto result = BaseResult(request);
  result.status = OkStatus();
  result.admitted = true;
  result.fail_closed = false;
  result.unique_protocol_evidence = request.unique_protocol_evidence_claimed;
  result.immediate_mode_evidence =
      request.mode == UniqueReservationFinalityProtocolMode::immediate;
  result.deferred_mode_evidence =
      request.mode == UniqueReservationFinalityProtocolMode::deferred;
  result.enterprise_ready_claimed = false;
  result.all_index_readiness_claimed = false;
  result.ceic_041_crash_matrix_claimed = false;
  result.ceic_042_readiness_gate_claimed = false;
  result.protocol_status =
      request.mode == UniqueReservationFinalityProtocolMode::immediate
          ? UniqueReservationFinalityProtocolStatus::
                admitted_immediate_protocol_evidence
          : UniqueReservationFinalityProtocolStatus::
                admitted_deferred_protocol_evidence;
  result.proof_token = kCeic034UniqueReservationFinalityProtocolGateToken;
  result.diagnostic =
      MakeDiagnostic(result.status,
                     result.protocol_status,
                     request,
                     "unique_btree reservation protocol evidence admitted without finality or readiness overclaim");
  AddEvidence(&result, "protocol_status",
              UniqueReservationFinalityProtocolStatusName(
                  result.protocol_status));
  AddEvidence(&result, "proof_token", result.proof_token);
  AddBoolEvidence(&result, "result_unique_protocol_evidence",
                  result.unique_protocol_evidence);
  AddBoolEvidence(&result, "result_immediate_mode_evidence",
                  result.immediate_mode_evidence);
  AddBoolEvidence(&result, "result_deferred_mode_evidence",
                  result.deferred_mode_evidence);
  AddBoolEvidence(&result, "result_enterprise_ready_claimed",
                  result.enterprise_ready_claimed);
  AddBoolEvidence(&result, "result_all_index_readiness_claimed",
                  result.all_index_readiness_claimed);
  AddBoolEvidence(&result, "result_ceic_041_crash_matrix_claimed",
                  result.ceic_041_crash_matrix_claimed);
  AddBoolEvidence(&result, "result_ceic_042_readiness_gate_claimed",
                  result.ceic_042_readiness_gate_claimed);
  AddBoolEvidence(&result, "transaction_finality_authority", false);
  AddBoolEvidence(&result, "visibility_authority", false);
  AddBoolEvidence(&result, "recovery_authority", false);
  AddBoolEvidence(&result, "parser_authority", false);
  AddBoolEvidence(&result, "reference_authority", false);
  AddBoolEvidence(&result, "wal_authority", false);
  AddEvidence(&result, "diagnostic_code", result.diagnostic.diagnostic_code);
  return result;
}

}  // namespace scratchbird::core::index

// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// SB-UNIQUE-INDEX-RESERVATION-LEDGER-ANCHOR
#include "unique_index_reservation_ledger.hpp"

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

namespace scratchbird::core::index {
namespace {

using scratchbird::core::platform::DiagnosticArgument;
using scratchbird::core::platform::MakeDiagnostic;
using scratchbird::core::platform::Severity;
using scratchbird::core::platform::StatusCode;
using scratchbird::core::platform::Subsystem;
using scratchbird::core::platform::UuidKind;
using scratchbird::transaction::mga::TransactionStateName;

Status OkStatus() {
  return {StatusCode::ok, Severity::info, Subsystem::transaction_mga};
}

Status RefuseStatus() {
  return {StatusCode::platform_required_feature_missing,
          Severity::error,
          Subsystem::transaction_mga};
}

bool SameUuid(const TypedUuid& left, const TypedUuid& right) {
  return left.kind == right.kind && left.valid() && right.valid() &&
         left.value == right.value;
}

bool ValidUuidKind(const TypedUuid& uuid, UuidKind kind) {
  return uuid.kind == kind && uuid.valid();
}

bool SameTransaction(const TypedUuid& left_uuid,
                     u64 left_local_id,
                     const TypedUuid& right_uuid,
                     u64 right_local_id) {
  return left_local_id != 0 && right_local_id != 0 &&
         left_local_id == right_local_id && SameUuid(left_uuid, right_uuid);
}

bool SameReservationScope(const UniqueIndexReservationRecord& left,
                          const UniqueIndexReservationRequest& right) {
  return SameUuid(left.index_uuid, right.index_uuid) &&
         SameUuid(left.table_uuid, right.table_uuid) &&
         SameUuid(left.constraint_uuid, right.constraint_uuid);
}

bool SameReservationScope(const UniqueIndexReservationRecord& left,
                          const UniqueIndexReservationRecord& right) {
  return SameUuid(left.index_uuid, right.index_uuid) &&
         SameUuid(left.table_uuid, right.table_uuid) &&
         SameUuid(left.constraint_uuid, right.constraint_uuid);
}

bool SameKey(const UniqueIndexReservationRecord& left,
             const UniqueIndexReservationRequest& right) {
  return SameReservationScope(left, right) && left.encoded_key == right.encoded_key;
}

bool SameKey(const UniqueIndexReservationRecord& left,
             const UniqueIndexReservationRecord& right) {
  return SameReservationScope(left, right) && left.encoded_key == right.encoded_key;
}

bool SameRowProofValid(const UniqueIndexReservationRequest& request,
                       const UniqueIndexReservationRecord& existing) {
  return request.same_row_update_proven &&
         SameUuid(request.same_row_proof_uuid, request.row_uuid) &&
         SameUuid(existing.row_uuid, request.row_uuid);
}

bool RecordPairHasSameRowProof(const UniqueIndexReservationRecord& left,
                               const UniqueIndexReservationRecord& right) {
  if (!SameUuid(left.row_uuid, right.row_uuid)) {
    return false;
  }
  return (left.same_row_update_proven &&
          SameUuid(left.same_row_proof_uuid, left.row_uuid)) ||
         (right.same_row_update_proven &&
          SameUuid(right.same_row_proof_uuid, right.row_uuid));
}

bool IsActiveOrUnresolved(TransactionState state) {
  return state == TransactionState::created ||
         state == TransactionState::active ||
         state == TransactionState::read_only_active ||
         state == TransactionState::preparing ||
         state == TransactionState::prepared ||
         state == TransactionState::committing ||
         state == TransactionState::rolling_back ||
         state == TransactionState::limbo ||
         state == TransactionState::recovering;
}

bool IsCommittedState(TransactionState state) {
  return state == TransactionState::committed ||
         state == TransactionState::archived;
}

bool IsRollbackState(TransactionState state) {
  return state == TransactionState::rolled_back;
}

const UniqueIndexReservationTransactionProof* FindProof(
    const std::vector<UniqueIndexReservationTransactionProof>& proofs,
    const TypedUuid& transaction_uuid,
    u64 local_transaction_id) {
  for (const auto& proof : proofs) {
    if (SameTransaction(proof.transaction_uuid,
                        proof.local_transaction_id,
                        transaction_uuid,
                        local_transaction_id)) {
      return &proof;
    }
  }
  return nullptr;
}

bool ProofHasMGAAuthority(const UniqueIndexReservationTransactionProof& proof) {
  return proof.engine_mga_authority &&
         proof.durable_transaction_inventory_authoritative &&
         !proof.evidence_token.empty();
}

bool ProofMatchesRequest(const UniqueIndexReservationTransactionProof& proof,
                         const TypedUuid& transaction_uuid,
                         u64 local_transaction_id) {
  return SameTransaction(proof.transaction_uuid,
                         proof.local_transaction_id,
                         transaction_uuid,
                         local_transaction_id);
}

bool ActiveProofValidForCommitValidation(
    const UniqueIndexReservationTransactionProof& proof) {
  return ProofHasMGAAuthority(proof) && IsActiveOrUnresolved(proof.state);
}

bool DurableCommitProofValid(const UniqueIndexReservationTransactionProof& proof) {
  return ProofHasMGAAuthority(proof) && IsCommittedState(proof.state) &&
         proof.durable_commit_evidence;
}

bool DurableRollbackProofValid(const UniqueIndexReservationTransactionProof& proof) {
  return ProofHasMGAAuthority(proof) && IsRollbackState(proof.state) &&
         proof.durable_rollback_evidence;
}

void AddNonAuthorityEvidence(std::vector<std::string>* evidence) {
  evidence->push_back("visibility_authority=false");
  evidence->push_back("authorization_authority=false");
  evidence->push_back("transaction_finality_authority=false");
  evidence->push_back("cleanup_authority=false");
  evidence->push_back("recovery_authority=false");
  evidence->push_back("mga_transaction_state_input_required=true");
}

UniqueIndexReservationEvidenceRecord EvidenceRecord(
    UniqueIndexReservationLedger* ledger,
    std::string operation,
    const TypedUuid& index_uuid,
    const TypedUuid& table_uuid,
    const TypedUuid& constraint_uuid,
    const TypedUuid& transaction_uuid,
    u64 local_transaction_id,
    std::string diagnostic_code,
    bool ledger_state_changed) {
  UniqueIndexReservationEvidenceRecord evidence;
  evidence.sequence = ledger == nullptr ? 0 : ledger->next_evidence_sequence++;
  evidence.operation = std::move(operation);
  evidence.index_uuid = index_uuid;
  evidence.table_uuid = table_uuid;
  evidence.constraint_uuid = constraint_uuid;
  evidence.transaction_uuid = transaction_uuid;
  evidence.local_transaction_id = local_transaction_id;
  evidence.diagnostic_code = std::move(diagnostic_code);
  evidence.ledger_state_changed = ledger_state_changed;
  return evidence;
}

UniqueIndexReservationResult MakeResult(
    Status status,
    UniqueIndexReservationDecision decision,
    UniqueIndexReservationConflictState conflict_state,
    std::string diagnostic_code,
    std::string message_key,
    std::string detail = {}) {
  UniqueIndexReservationResult result;
  result.status = status;
  result.decision = decision;
  result.conflict_state = conflict_state;
  result.diagnostic = MakeUniqueIndexReservationDiagnostic(
      result.status,
      std::move(diagnostic_code),
      std::move(message_key),
      std::move(detail));
  result.evidence.push_back(std::string("decision=") +
                            UniqueIndexReservationDecisionName(decision));
  result.evidence.push_back(std::string("conflict_state=") +
                            UniqueIndexReservationConflictStateName(conflict_state));
  AddNonAuthorityEvidence(&result.evidence);
  return result;
}

UniqueIndexReservationResult Refuse(std::string diagnostic_code,
                                    std::string message_key,
                                    std::string detail = {},
                                    UniqueIndexReservationConflictState conflict_state =
                                        UniqueIndexReservationConflictState::unsafe_refused) {
  return MakeResult(RefuseStatus(),
                    UniqueIndexReservationDecision::refused,
                    conflict_state,
                    std::move(diagnostic_code),
                    std::move(message_key),
                    std::move(detail));
}

bool ValidateCommonReservationRequest(const UniqueIndexReservationRequest& request,
                                      std::string* detail) {
  if (!ValidUuidKind(request.index_uuid, UuidKind::object) ||
      !ValidUuidKind(request.table_uuid, UuidKind::object) ||
      !ValidUuidKind(request.constraint_uuid, UuidKind::object) ||
      !ValidUuidKind(request.row_uuid, UuidKind::row) ||
      !ValidUuidKind(request.version_uuid, UuidKind::row) ||
      !ValidUuidKind(request.transaction_uuid, UuidKind::transaction) ||
      request.local_transaction_id == 0) {
    *detail = "index table constraint row version and transaction identities are required";
    return false;
  }
  if (!request.partial_predicate_proven) {
    *detail = "partial predicate participation must be explicitly proven";
    return false;
  }
  if (!request.null_policy_proven) {
    *detail = "unique null policy must be explicitly proven";
    return false;
  }
  if (request.partial_predicate_participates && request.encoded_key.empty()) {
    *detail = "participating unique reservation requires an encoded key";
    return false;
  }
  return true;
}

UniqueIndexReservationConflict ConflictFromRecord(
    const UniqueIndexReservationRecord& record,
    UniqueIndexReservationConflictState conflict_state,
    TransactionState observed_state,
    bool proof_present,
    bool same_row) {
  UniqueIndexReservationConflict conflict;
  conflict.reservation_sequence = record.reservation_sequence;
  conflict.transaction_uuid = record.transaction_uuid;
  conflict.local_transaction_id = record.local_transaction_id;
  conflict.row_uuid = record.row_uuid;
  conflict.version_uuid = record.version_uuid;
  conflict.record_state = record.state;
  conflict.observed_transaction_state = observed_state;
  conflict.conflict_state = conflict_state;
  conflict.same_row = same_row;
  conflict.mga_state_proof_present = proof_present;
  return conflict;
}

void PushEvidence(UniqueIndexReservationLedger* ledger,
                  UniqueIndexReservationEvidenceRecord evidence) {
  if (ledger != nullptr) {
    ledger->evidence.push_back(std::move(evidence));
  }
}

void AddConflictEvidence(UniqueIndexReservationResult* result,
                         const UniqueIndexReservationConflict& conflict) {
  result->conflicts.push_back(conflict);
  result->conflict = true;
  result->evidence.push_back("unique_reservation_conflict=true");
  result->evidence.push_back("conflict_reservation_sequence=" +
                             std::to_string(conflict.reservation_sequence));
  result->evidence.push_back(std::string("conflict_record_state=") +
                             UniqueIndexReservationRecordStateName(conflict.record_state));
  result->evidence.push_back(std::string("conflict_observed_transaction_state=") +
                             TransactionStateName(conflict.observed_transaction_state));
}

}  // namespace

const char* UniqueIndexReservationNullPolicyName(
    UniqueIndexReservationNullPolicy policy) {
  switch (policy) {
    case UniqueIndexReservationNullPolicy::nulls_distinct:
      return "nulls_distinct";
    case UniqueIndexReservationNullPolicy::nulls_not_distinct:
      return "nulls_not_distinct";
  }
  return "unknown";
}

const char* UniqueIndexReservationActiveConflictPolicyName(
    UniqueIndexReservationActiveConflictPolicy policy) {
  switch (policy) {
    case UniqueIndexReservationActiveConflictPolicy::wait_for_mga:
      return "wait_for_mga";
    case UniqueIndexReservationActiveConflictPolicy::refuse_candidate:
      return "refuse_candidate";
  }
  return "unknown";
}

const char* UniqueIndexReservationConflictStateName(
    UniqueIndexReservationConflictState state) {
  switch (state) {
    case UniqueIndexReservationConflictState::none:
      return "none";
    case UniqueIndexReservationConflictState::wait_for_mga:
      return "wait_for_mga";
    case UniqueIndexReservationConflictState::refuse_candidate:
      return "refuse_candidate";
    case UniqueIndexReservationConflictState::committed_refused:
      return "committed_refused";
    case UniqueIndexReservationConflictState::validated_refused:
      return "validated_refused";
    case UniqueIndexReservationConflictState::same_row_proof_required:
      return "same_row_proof_required";
    case UniqueIndexReservationConflictState::unsafe_refused:
      return "unsafe_refused";
    case UniqueIndexReservationConflictState::duplicate_in_transaction_refused:
      return "duplicate_in_transaction_refused";
  }
  return "unknown";
}

const char* UniqueIndexReservationRecordStateName(
    UniqueIndexReservationRecordState state) {
  switch (state) {
    case UniqueIndexReservationRecordState::reserved:
      return "reserved";
    case UniqueIndexReservationRecordState::commit_validated:
      return "commit_validated";
    case UniqueIndexReservationRecordState::committed_published:
      return "committed_published";
  }
  return "unknown";
}

const char* UniqueIndexReservationDecisionName(
    UniqueIndexReservationDecision decision) {
  switch (decision) {
    case UniqueIndexReservationDecision::reserved:
      return "reserved";
    case UniqueIndexReservationDecision::bypassed_partial_predicate:
      return "bypassed_partial_predicate";
    case UniqueIndexReservationDecision::bypassed_nulls_distinct:
      return "bypassed_nulls_distinct";
    case UniqueIndexReservationDecision::wait_for_mga:
      return "wait_for_mga";
    case UniqueIndexReservationDecision::refused:
      return "refused";
    case UniqueIndexReservationDecision::rollback_cleaned:
      return "rollback_cleaned";
    case UniqueIndexReservationDecision::commit_validated:
      return "commit_validated";
    case UniqueIndexReservationDecision::commit_published:
      return "commit_published";
  }
  return "unknown";
}

UniqueIndexReservationProtocolGateResult EvaluateUniqueIndexReservationProtocolGate(
    const UniqueIndexReservationProtocolGateRequest& request) {
  UniqueIndexReservationProtocolGateResult result;
  result.protocol_present = request.reservation_api_present &&
                            request.commit_validation_api_present &&
                            request.rollback_cleanup_api_present;
  result.protocol_enabled = request.enabled;
  result.protocol_proven =
      result.protocol_present &&
      result.protocol_enabled &&
      request.commit_publication_requires_mga_durable_proof &&
      request.non_authority_evidence_flags_present &&
      request.standalone_lifecycle_tests_present;
  result.status = result.protocol_proven ? OkStatus() : RefuseStatus();
  result.proof_token = result.protocol_proven
                           ? kUniqueIndexReservationProtocolGateToken
                           : std::string{};
  result.diagnostic = MakeUniqueIndexReservationDiagnostic(
      result.status,
      result.protocol_proven
          ? "INDEX.UNIQUE_RESERVATION.PROTOCOL_PROVEN"
          : "INDEX.UNIQUE_RESERVATION.PROTOCOL_UNPROVEN",
      result.protocol_proven
          ? "core.index.unique_reservation.protocol_proven"
          : "core.index.unique_reservation.protocol_unproven",
      result.protocol_proven
          ? "unique reservation ledger protocol gate is proven"
          : "unique reservation ledger protocol gate is incomplete");
  result.evidence = {
      std::string("reservation_api_present=") +
          (request.reservation_api_present ? "true" : "false"),
      std::string("commit_validation_api_present=") +
          (request.commit_validation_api_present ? "true" : "false"),
      std::string("rollback_cleanup_api_present=") +
          (request.rollback_cleanup_api_present ? "true" : "false"),
      std::string("commit_publication_requires_mga_durable_proof=") +
          (request.commit_publication_requires_mga_durable_proof ? "true" : "false"),
      std::string("non_authority_evidence_flags_present=") +
          (request.non_authority_evidence_flags_present ? "true" : "false"),
      std::string("standalone_lifecycle_tests_present=") +
          (request.standalone_lifecycle_tests_present ? "true" : "false"),
      "visibility_authority=false",
      "authorization_authority=false",
      "transaction_finality_authority=false",
      "cleanup_authority=false",
      "recovery_authority=false"};
  return result;
}

UniqueIndexReservationResult ReserveUniqueIndexKey(
    UniqueIndexReservationLedger* ledger,
    const UniqueIndexReservationRequest& request) {
  if (ledger == nullptr) {
    return Refuse("INDEX.UNIQUE_RESERVATION.LEDGER_REQUIRED",
                  "core.index.unique_reservation.ledger_required",
                  "unique reservation ledger is required");
  }
  std::string detail;
  if (!ValidateCommonReservationRequest(request, &detail)) {
    return Refuse("INDEX.UNIQUE_RESERVATION.INVALID_REQUEST",
                  "core.index.unique_reservation.invalid_request",
                  std::move(detail));
  }

  if (!request.partial_predicate_participates) {
    auto result = MakeResult(OkStatus(),
                             UniqueIndexReservationDecision::
                                 bypassed_partial_predicate,
                             UniqueIndexReservationConflictState::none,
                             "INDEX.UNIQUE_RESERVATION.PARTIAL_BYPASS",
                             "core.index.unique_reservation.partial_bypass",
                             "partial predicate does not participate");
    result.bypassed = true;
    result.evidence.push_back("partial_predicate_participates=false");
    result.evidence.push_back("unique_reservation_created=false");
    PushEvidence(ledger,
                 EvidenceRecord(ledger,
                                "reserve",
                                request.index_uuid,
                                request.table_uuid,
                                request.constraint_uuid,
                                request.transaction_uuid,
                                request.local_transaction_id,
                                result.diagnostic.diagnostic_code,
                                false));
    return result;
  }

  const bool nulls_distinct =
      request.null_policy == UniqueIndexReservationNullPolicy::nulls_distinct;
  if (request.incoming_key_has_null && nulls_distinct) {
    auto result = MakeResult(OkStatus(),
                             UniqueIndexReservationDecision::
                                 bypassed_nulls_distinct,
                             UniqueIndexReservationConflictState::none,
                             "INDEX.UNIQUE_RESERVATION.NULLS_DISTINCT_BYPASS",
                             "core.index.unique_reservation.nulls_distinct_bypass",
                             "nulls-distinct unique key does not reserve a duplicate slot");
    result.bypassed = true;
    result.evidence.push_back("incoming_key_has_null=true");
    result.evidence.push_back("unique_null_policy=nulls_distinct");
    result.evidence.push_back("unique_reservation_created=false");
    PushEvidence(ledger,
                 EvidenceRecord(ledger,
                                "reserve",
                                request.index_uuid,
                                request.table_uuid,
                                request.constraint_uuid,
                                request.transaction_uuid,
                                request.local_transaction_id,
                                result.diagnostic.diagnostic_code,
                                false));
    return result;
  }

  bool same_row_allowed = false;
  for (const auto& existing : ledger->reservations) {
    if (!SameKey(existing, request)) {
      continue;
    }
    const bool same_row = SameUuid(existing.row_uuid, request.row_uuid);
    const bool same_transaction =
        SameTransaction(existing.transaction_uuid,
                        existing.local_transaction_id,
                        request.transaction_uuid,
                        request.local_transaction_id);

    if (same_transaction) {
      if (same_row) {
        if (!SameRowProofValid(request, existing)) {
          auto result = Refuse(
              "INDEX.UNIQUE_RESERVATION.SAME_ROW_PROOF_REQUIRED",
              "core.index.unique_reservation.same_row_proof_required",
              "same-row unique update requires an explicit same-row proof",
              UniqueIndexReservationConflictState::same_row_proof_required);
          AddConflictEvidence(
              &result,
              ConflictFromRecord(existing,
                                 UniqueIndexReservationConflictState::
                                     same_row_proof_required,
                                 TransactionState::active,
                                 false,
                                 true));
          PushEvidence(ledger,
                       EvidenceRecord(ledger,
                                      "reserve",
                                      request.index_uuid,
                                      request.table_uuid,
                                      request.constraint_uuid,
                                      request.transaction_uuid,
                                      request.local_transaction_id,
                                      result.diagnostic.diagnostic_code,
                                      false));
          return result;
        }
        same_row_allowed = true;
        continue;
      }
      continue;
    }

    if (same_row) {
      if (!SameRowProofValid(request, existing)) {
        auto result = Refuse(
            "INDEX.UNIQUE_RESERVATION.SAME_ROW_PROOF_REQUIRED",
            "core.index.unique_reservation.same_row_proof_required",
            "same-row unique update requires an explicit same-row proof",
            UniqueIndexReservationConflictState::same_row_proof_required);
        AddConflictEvidence(
            &result,
            ConflictFromRecord(existing,
                               UniqueIndexReservationConflictState::
                                   same_row_proof_required,
                               TransactionState::active,
                               false,
                               true));
        PushEvidence(ledger,
                     EvidenceRecord(ledger,
                                    "reserve",
                                    request.index_uuid,
                                    request.table_uuid,
                                    request.constraint_uuid,
                                    request.transaction_uuid,
                                    request.local_transaction_id,
                                    result.diagnostic.diagnostic_code,
                                    false));
        return result;
      }
      same_row_allowed = true;
      continue;
    }

    if (existing.state == UniqueIndexReservationRecordState::commit_validated) {
      auto result = Refuse(
          "INDEX.UNIQUE_RESERVATION.VALIDATED_CONFLICT_REFUSED",
          "core.index.unique_reservation.validated_conflict_refused",
          "validated unique reservation from a different row already owns this key",
          UniqueIndexReservationConflictState::validated_refused);
      AddConflictEvidence(
          &result,
          ConflictFromRecord(existing,
                             UniqueIndexReservationConflictState::
                                 validated_refused,
                             TransactionState::committing,
                             true,
                             false));
      PushEvidence(ledger,
                   EvidenceRecord(ledger,
                                  "reserve",
                                  request.index_uuid,
                                  request.table_uuid,
                                  request.constraint_uuid,
                                  request.transaction_uuid,
                                  request.local_transaction_id,
                                  result.diagnostic.diagnostic_code,
                                  false));
      return result;
    }

    if (existing.state == UniqueIndexReservationRecordState::committed_published) {
      auto result = Refuse(
          "INDEX.UNIQUE_RESERVATION.COMMITTED_CONFLICT_REFUSED",
          "core.index.unique_reservation.committed_conflict_refused",
          "committed unique reservation from a different row already owns this key",
          UniqueIndexReservationConflictState::committed_refused);
      AddConflictEvidence(
          &result,
          ConflictFromRecord(existing,
                             UniqueIndexReservationConflictState::
                                 committed_refused,
                             TransactionState::committed,
                             true,
                             false));
      PushEvidence(ledger,
                   EvidenceRecord(ledger,
                                  "reserve",
                                  request.index_uuid,
                                  request.table_uuid,
                                  request.constraint_uuid,
                                  request.transaction_uuid,
                                  request.local_transaction_id,
                                  result.diagnostic.diagnostic_code,
                                  false));
      return result;
    }

    const auto* proof = FindProof(request.transaction_state_proofs,
                                  existing.transaction_uuid,
                                  existing.local_transaction_id);
    if (proof == nullptr || !ProofHasMGAAuthority(*proof)) {
      auto result = Refuse(
          "INDEX.UNIQUE_RESERVATION.UNSAFE_CONFLICT_STATE",
          "core.index.unique_reservation.unsafe_conflict_state",
          "conflicting reservation requires explicit engine MGA transaction state proof",
          UniqueIndexReservationConflictState::unsafe_refused);
      AddConflictEvidence(
          &result,
          ConflictFromRecord(existing,
                             UniqueIndexReservationConflictState::unsafe_refused,
                             proof == nullptr ? TransactionState::none : proof->state,
                             proof != nullptr,
                             false));
      PushEvidence(ledger,
                   EvidenceRecord(ledger,
                                  "reserve",
                                  request.index_uuid,
                                  request.table_uuid,
                                  request.constraint_uuid,
                                  request.transaction_uuid,
                                  request.local_transaction_id,
                                  result.diagnostic.diagnostic_code,
                                  false));
      return result;
    }
    if (IsActiveOrUnresolved(proof->state)) {
      const bool wait =
          request.active_conflict_policy ==
          UniqueIndexReservationActiveConflictPolicy::wait_for_mga;
      auto result = MakeResult(
          wait ? OkStatus() : RefuseStatus(),
          wait ? UniqueIndexReservationDecision::wait_for_mga
               : UniqueIndexReservationDecision::refused,
          wait ? UniqueIndexReservationConflictState::wait_for_mga
               : UniqueIndexReservationConflictState::refuse_candidate,
          wait ? "INDEX.UNIQUE_RESERVATION.ACTIVE_CONFLICT_WAIT"
               : "INDEX.UNIQUE_RESERVATION.ACTIVE_CONFLICT_REFUSED",
          wait ? "core.index.unique_reservation.active_conflict_wait"
               : "core.index.unique_reservation.active_conflict_refused",
          "active conflicting transaction owns the same unique key");
      result.conflict = true;
      AddConflictEvidence(
          &result,
          ConflictFromRecord(existing,
                             result.conflict_state,
                             proof->state,
                             true,
                             false));
      result.evidence.push_back(std::string("active_conflict_policy=") +
                                UniqueIndexReservationActiveConflictPolicyName(
                                    request.active_conflict_policy));
      PushEvidence(ledger,
                   EvidenceRecord(ledger,
                                  "reserve",
                                  request.index_uuid,
                                  request.table_uuid,
                                  request.constraint_uuid,
                                  request.transaction_uuid,
                                  request.local_transaction_id,
                                  result.diagnostic.diagnostic_code,
                                  false));
      return result;
    }
    if (IsCommittedState(proof->state)) {
      auto result = Refuse(
          "INDEX.UNIQUE_RESERVATION.COMMITTED_CONFLICT_REFUSED",
          "core.index.unique_reservation.committed_conflict_refused",
          "committed transaction owns the same unique key",
          UniqueIndexReservationConflictState::committed_refused);
      AddConflictEvidence(
          &result,
          ConflictFromRecord(existing,
                             UniqueIndexReservationConflictState::
                                 committed_refused,
                             proof->state,
                             true,
                             false));
      PushEvidence(ledger,
                   EvidenceRecord(ledger,
                                  "reserve",
                                  request.index_uuid,
                                  request.table_uuid,
                                  request.constraint_uuid,
                                  request.transaction_uuid,
                                  request.local_transaction_id,
                                  result.diagnostic.diagnostic_code,
                                  false));
      return result;
    }
    auto result = Refuse(
        "INDEX.UNIQUE_RESERVATION.UNSAFE_CONFLICT_STATE",
        "core.index.unique_reservation.unsafe_conflict_state",
        "conflicting reservation requires rollback cleanup before reuse",
        UniqueIndexReservationConflictState::unsafe_refused);
    AddConflictEvidence(
        &result,
        ConflictFromRecord(existing,
                           UniqueIndexReservationConflictState::unsafe_refused,
                           proof->state,
                           true,
                           false));
    PushEvidence(ledger,
                 EvidenceRecord(ledger,
                                "reserve",
                                request.index_uuid,
                                request.table_uuid,
                                request.constraint_uuid,
                                request.transaction_uuid,
                                request.local_transaction_id,
                                result.diagnostic.diagnostic_code,
                                false));
    return result;
  }

  UniqueIndexReservationRecord record;
  record.reservation_sequence = ledger->next_reservation_sequence++;
  record.index_uuid = request.index_uuid;
  record.table_uuid = request.table_uuid;
  record.constraint_uuid = request.constraint_uuid;
  record.row_uuid = request.row_uuid;
  record.version_uuid = request.version_uuid;
  record.transaction_uuid = request.transaction_uuid;
  record.local_transaction_id = request.local_transaction_id;
  record.encoded_key = request.encoded_key;
  record.null_policy = request.null_policy;
  record.incoming_key_has_null = request.incoming_key_has_null;
  record.partial_predicate_participates = true;
  record.same_row_update_proven = request.same_row_update_proven;
  record.same_row_proof_uuid = request.same_row_proof_uuid;
  record.reservation_evidence_token =
      "unique_reservation:" + std::to_string(record.reservation_sequence);

  const u64 sequence = record.reservation_sequence;
  ledger->reservations.push_back(std::move(record));

  auto result = MakeResult(OkStatus(),
                           UniqueIndexReservationDecision::reserved,
                           UniqueIndexReservationConflictState::none,
                           "INDEX.UNIQUE_RESERVATION.RESERVED",
                           "core.index.unique_reservation.reserved",
                           "unique key reservation recorded");
  result.reserved = true;
  result.reservation_sequence = sequence;
  result.same_row_update_allowed = same_row_allowed;
  result.evidence.push_back("unique_reservation_created=true");
  result.evidence.push_back(std::string("unique_null_policy=") +
                            UniqueIndexReservationNullPolicyName(request.null_policy));
  result.evidence.push_back(std::string("incoming_key_has_null=") +
                            (request.incoming_key_has_null ? "true" : "false"));
  result.evidence.push_back("partial_predicate_participates=true");
  if (same_row_allowed) {
    result.evidence.push_back("same_row_update_proof=true");
    result.evidence.push_back("same_row_update_allowed=true");
  }
  PushEvidence(ledger,
               EvidenceRecord(ledger,
                              "reserve",
                              request.index_uuid,
                              request.table_uuid,
                              request.constraint_uuid,
                              request.transaction_uuid,
                              request.local_transaction_id,
                              result.diagnostic.diagnostic_code,
                              true));
  return result;
}

UniqueIndexReservationResult ValidateUniqueIndexCommitBatch(
    UniqueIndexReservationLedger* ledger,
    const UniqueIndexCommitValidationRequest& request) {
  if (ledger == nullptr) {
    return Refuse("INDEX.UNIQUE_RESERVATION.LEDGER_REQUIRED",
                  "core.index.unique_reservation.ledger_required",
                  "unique reservation ledger is required");
  }
  if (!ValidUuidKind(request.transaction_uuid, UuidKind::transaction) ||
      request.local_transaction_id == 0) {
    return Refuse("INDEX.UNIQUE_RESERVATION.INVALID_COMMIT_VALIDATION",
                  "core.index.unique_reservation.invalid_commit_validation",
                  "commit validation requires transaction identity");
  }
  const auto* own_proof = FindProof(request.transaction_state_proofs,
                                    request.transaction_uuid,
                                    request.local_transaction_id);
  if (own_proof == nullptr || !ActiveProofValidForCommitValidation(*own_proof)) {
    return Refuse("INDEX.UNIQUE_RESERVATION.COMMIT_VALIDATION_MGA_PROOF_REQUIRED",
                  "core.index.unique_reservation.commit_validation_mga_proof_required",
                  "commit validation requires explicit active engine MGA transaction proof");
  }

  std::vector<std::size_t> own_indexes;
  for (std::size_t i = 0; i < ledger->reservations.size(); ++i) {
    const auto& record = ledger->reservations[i];
    if (SameTransaction(record.transaction_uuid,
                        record.local_transaction_id,
                        request.transaction_uuid,
                        request.local_transaction_id)) {
      own_indexes.push_back(i);
    }
  }

  for (std::size_t left_pos = 0; left_pos < own_indexes.size(); ++left_pos) {
    for (std::size_t right_pos = left_pos + 1; right_pos < own_indexes.size();
         ++right_pos) {
      const auto& left = ledger->reservations[own_indexes[left_pos]];
      const auto& right = ledger->reservations[own_indexes[right_pos]];
      if (!SameKey(left, right)) {
        continue;
      }
      if (RecordPairHasSameRowProof(left, right)) {
        continue;
      }
      auto result = Refuse(
          "INDEX.UNIQUE_RESERVATION.COMMIT_DUPLICATE_REFUSED",
          "core.index.unique_reservation.commit_duplicate_refused",
          "commit validation found duplicate same-key reservations inside one transaction",
          UniqueIndexReservationConflictState::duplicate_in_transaction_refused);
      AddConflictEvidence(
          &result,
          ConflictFromRecord(left,
                             UniqueIndexReservationConflictState::
                                 duplicate_in_transaction_refused,
                             own_proof->state,
                             true,
                             SameUuid(left.row_uuid, right.row_uuid)));
      PushEvidence(ledger,
                   EvidenceRecord(ledger,
                                  "commit_validate",
                                  left.index_uuid,
                                  left.table_uuid,
                                  left.constraint_uuid,
                                  request.transaction_uuid,
                                  request.local_transaction_id,
                                  result.diagnostic.diagnostic_code,
                                  false));
      return result;
    }
  }

  for (const std::size_t own_index : own_indexes) {
    const auto& own = ledger->reservations[own_index];
    for (const auto& other : ledger->reservations) {
      if (SameTransaction(other.transaction_uuid,
                          other.local_transaction_id,
                          request.transaction_uuid,
                          request.local_transaction_id) ||
          !SameKey(own, other)) {
        continue;
      }
      if (SameUuid(own.row_uuid, other.row_uuid) &&
          own.same_row_update_proven &&
          SameUuid(own.same_row_proof_uuid, own.row_uuid)) {
        continue;
      }
      if (other.state == UniqueIndexReservationRecordState::commit_validated) {
        auto result = Refuse(
            "INDEX.UNIQUE_RESERVATION.COMMIT_VALIDATED_CONFLICT_REFUSED",
            "core.index.unique_reservation.commit_validated_conflict_refused",
            "commit validation found another validated unique reservation",
            UniqueIndexReservationConflictState::validated_refused);
        AddConflictEvidence(
            &result,
            ConflictFromRecord(other,
                               UniqueIndexReservationConflictState::
                                   validated_refused,
                               TransactionState::committing,
                               true,
                               SameUuid(own.row_uuid, other.row_uuid)));
        PushEvidence(ledger,
                     EvidenceRecord(ledger,
                                    "commit_validate",
                                    own.index_uuid,
                                    own.table_uuid,
                                    own.constraint_uuid,
                                    request.transaction_uuid,
                                    request.local_transaction_id,
                                    result.diagnostic.diagnostic_code,
                                    false));
        return result;
      }
      if (other.state == UniqueIndexReservationRecordState::committed_published) {
        auto result = Refuse(
            "INDEX.UNIQUE_RESERVATION.COMMIT_COMMITTED_CONFLICT_REFUSED",
            "core.index.unique_reservation.commit_committed_conflict_refused",
            "commit validation found another committed unique reservation",
            UniqueIndexReservationConflictState::committed_refused);
        AddConflictEvidence(
            &result,
            ConflictFromRecord(other,
                               UniqueIndexReservationConflictState::
                                   committed_refused,
                               TransactionState::committed,
                               true,
                               SameUuid(own.row_uuid, other.row_uuid)));
        PushEvidence(ledger,
                     EvidenceRecord(ledger,
                                    "commit_validate",
                                    own.index_uuid,
                                    own.table_uuid,
                                    own.constraint_uuid,
                                    request.transaction_uuid,
                                    request.local_transaction_id,
                                    result.diagnostic.diagnostic_code,
                                    false));
        return result;
      }
      const auto* proof = FindProof(request.transaction_state_proofs,
                                    other.transaction_uuid,
                                    other.local_transaction_id);
      if (proof == nullptr || !ProofHasMGAAuthority(*proof)) {
        auto result = Refuse(
            "INDEX.UNIQUE_RESERVATION.COMMIT_UNSAFE_CONFLICT_REFUSED",
            "core.index.unique_reservation.commit_unsafe_conflict_refused",
            "commit validation requires explicit MGA state for every conflicting reservation",
            UniqueIndexReservationConflictState::unsafe_refused);
        AddConflictEvidence(
            &result,
            ConflictFromRecord(other,
                               UniqueIndexReservationConflictState::
                                   unsafe_refused,
                               proof == nullptr ? TransactionState::none : proof->state,
                               proof != nullptr,
                               SameUuid(own.row_uuid, other.row_uuid)));
        PushEvidence(ledger,
                     EvidenceRecord(ledger,
                                    "commit_validate",
                                    own.index_uuid,
                                    own.table_uuid,
                                    own.constraint_uuid,
                                    request.transaction_uuid,
                                    request.local_transaction_id,
                                    result.diagnostic.diagnostic_code,
                                    false));
        return result;
      }
      if (IsActiveOrUnresolved(proof->state)) {
        auto result = Refuse(
            "INDEX.UNIQUE_RESERVATION.COMMIT_ACTIVE_CONFLICT_REFUSED",
            "core.index.unique_reservation.commit_active_conflict_refused",
            "commit validation found another active same-key transaction",
            UniqueIndexReservationConflictState::refuse_candidate);
        AddConflictEvidence(
            &result,
            ConflictFromRecord(other,
                               UniqueIndexReservationConflictState::
                                   refuse_candidate,
                               proof->state,
                               true,
                               SameUuid(own.row_uuid, other.row_uuid)));
        PushEvidence(ledger,
                     EvidenceRecord(ledger,
                                    "commit_validate",
                                    own.index_uuid,
                                    own.table_uuid,
                                    own.constraint_uuid,
                                    request.transaction_uuid,
                                    request.local_transaction_id,
                                    result.diagnostic.diagnostic_code,
                                    false));
        return result;
      }
      if (IsCommittedState(proof->state)) {
        auto result = Refuse(
            "INDEX.UNIQUE_RESERVATION.COMMIT_COMMITTED_CONFLICT_REFUSED",
            "core.index.unique_reservation.commit_committed_conflict_refused",
            "commit validation found committed same-key transaction",
            UniqueIndexReservationConflictState::committed_refused);
        AddConflictEvidence(
            &result,
            ConflictFromRecord(other,
                               UniqueIndexReservationConflictState::
                                   committed_refused,
                               proof->state,
                               true,
                               SameUuid(own.row_uuid, other.row_uuid)));
        PushEvidence(ledger,
                     EvidenceRecord(ledger,
                                    "commit_validate",
                                    own.index_uuid,
                                    own.table_uuid,
                                    own.constraint_uuid,
                                    request.transaction_uuid,
                                    request.local_transaction_id,
                                    result.diagnostic.diagnostic_code,
                                    false));
        return result;
      }
      auto result = Refuse(
          "INDEX.UNIQUE_RESERVATION.COMMIT_UNSAFE_CONFLICT_REFUSED",
          "core.index.unique_reservation.commit_unsafe_conflict_refused",
          "conflicting rolled-back reservation must be cleaned before commit validation",
          UniqueIndexReservationConflictState::unsafe_refused);
      AddConflictEvidence(
          &result,
          ConflictFromRecord(other,
                             UniqueIndexReservationConflictState::unsafe_refused,
                             proof->state,
                             true,
                             SameUuid(own.row_uuid, other.row_uuid)));
      PushEvidence(ledger,
                   EvidenceRecord(ledger,
                                  "commit_validate",
                                  own.index_uuid,
                                  own.table_uuid,
                                  own.constraint_uuid,
                                  request.transaction_uuid,
                                  request.local_transaction_id,
                                  result.diagnostic.diagnostic_code,
                                  false));
      return result;
    }
  }

  for (const std::size_t own_index : own_indexes) {
    ledger->reservations[own_index].state =
        UniqueIndexReservationRecordState::commit_validated;
    ledger->reservations[own_index].commit_validation_evidence_token =
        request.validation_evidence_token.empty()
            ? own_proof->evidence_token
            : request.validation_evidence_token;
  }

  auto result = MakeResult(OkStatus(),
                           UniqueIndexReservationDecision::commit_validated,
                           UniqueIndexReservationConflictState::none,
                           "INDEX.UNIQUE_RESERVATION.COMMIT_VALIDATED",
                           "core.index.unique_reservation.commit_validated",
                           "commit-time unique validation succeeded");
  result.commit_validation_passed = true;
  result.evidence.push_back("commit_validation_passed=true");
  result.evidence.push_back("commit_validation_requires_mga_state_inputs=true");
  result.evidence.push_back("commit_validation_does_not_publish_finality=true");
  result.evidence.push_back("validated_reservation_count=" +
                            std::to_string(own_indexes.size()));
  PushEvidence(ledger,
               EvidenceRecord(ledger,
                              "commit_validate",
                              TypedUuid{},
                              TypedUuid{},
                              TypedUuid{},
                              request.transaction_uuid,
                              request.local_transaction_id,
                              result.diagnostic.diagnostic_code,
                              !own_indexes.empty()));
  return result;
}

UniqueIndexReservationResult PublishUniqueIndexCommit(
    UniqueIndexReservationLedger* ledger,
    const UniqueIndexCommitPublicationRequest& request) {
  if (ledger == nullptr) {
    return Refuse("INDEX.UNIQUE_RESERVATION.LEDGER_REQUIRED",
                  "core.index.unique_reservation.ledger_required",
                  "unique reservation ledger is required");
  }
  if (!ValidUuidKind(request.transaction_uuid, UuidKind::transaction) ||
      request.local_transaction_id == 0 ||
      !ProofMatchesRequest(request.durable_commit_proof,
                           request.transaction_uuid,
                           request.local_transaction_id) ||
      !DurableCommitProofValid(request.durable_commit_proof)) {
    return Refuse("INDEX.UNIQUE_RESERVATION.COMMIT_PUBLICATION_PROOF_REQUIRED",
                  "core.index.unique_reservation.commit_publication_proof_required",
                  "commit publication requires explicit durable MGA commit proof");
  }

  std::vector<std::size_t> own_indexes;
  for (std::size_t i = 0; i < ledger->reservations.size(); ++i) {
    const auto& record = ledger->reservations[i];
    if (SameTransaction(record.transaction_uuid,
                        record.local_transaction_id,
                        request.transaction_uuid,
                        request.local_transaction_id)) {
      own_indexes.push_back(i);
      if (record.state != UniqueIndexReservationRecordState::commit_validated) {
        return Refuse(
            "INDEX.UNIQUE_RESERVATION.COMMIT_PUBLICATION_VALIDATION_REQUIRED",
            "core.index.unique_reservation.commit_publication_validation_required",
            "commit publication requires prior unique commit validation",
            UniqueIndexReservationConflictState::unsafe_refused);
      }
    }
  }

  for (const std::size_t own_index : own_indexes) {
    ledger->reservations[own_index].state =
        UniqueIndexReservationRecordState::committed_published;
    ledger->reservations[own_index].commit_publication_evidence_token =
        request.durable_commit_proof.evidence_token;
  }

  auto result = MakeResult(OkStatus(),
                           UniqueIndexReservationDecision::commit_published,
                           UniqueIndexReservationConflictState::none,
                           "INDEX.UNIQUE_RESERVATION.COMMIT_PUBLISHED",
                           "core.index.unique_reservation.commit_published",
                           "unique reservation commit publication marked from MGA proof");
  result.commit_publication_marked = true;
  result.evidence.push_back("mga_durable_commit_proof=true");
  result.evidence.push_back("commit_publication_marked=true");
  result.evidence.push_back("commit_publication_inferred=false");
  result.evidence.push_back("published_reservation_count=" +
                            std::to_string(own_indexes.size()));
  PushEvidence(ledger,
               EvidenceRecord(ledger,
                              "commit_publish",
                              TypedUuid{},
                              TypedUuid{},
                              TypedUuid{},
                              request.transaction_uuid,
                              request.local_transaction_id,
                              result.diagnostic.diagnostic_code,
                              !own_indexes.empty()));
  return result;
}

UniqueIndexReservationResult CleanupUniqueIndexReservationsForRollback(
    UniqueIndexReservationLedger* ledger,
    const UniqueIndexRollbackCleanupRequest& request) {
  if (ledger == nullptr) {
    return Refuse("INDEX.UNIQUE_RESERVATION.LEDGER_REQUIRED",
                  "core.index.unique_reservation.ledger_required",
                  "unique reservation ledger is required");
  }
  if (!ValidUuidKind(request.transaction_uuid, UuidKind::transaction) ||
      request.local_transaction_id == 0 ||
      !ProofMatchesRequest(request.rollback_proof,
                           request.transaction_uuid,
                           request.local_transaction_id) ||
      !DurableRollbackProofValid(request.rollback_proof)) {
    return Refuse("INDEX.UNIQUE_RESERVATION.ROLLBACK_PROOF_REQUIRED",
                  "core.index.unique_reservation.rollback_proof_required",
                  "rollback cleanup requires explicit durable MGA rollback proof");
  }

  const auto before = ledger->reservations.size();
  ledger->reservations.erase(
      std::remove_if(ledger->reservations.begin(),
                     ledger->reservations.end(),
                     [&](const UniqueIndexReservationRecord& record) {
                       return SameTransaction(record.transaction_uuid,
                                              record.local_transaction_id,
                                              request.transaction_uuid,
                                              request.local_transaction_id) &&
                              record.state != UniqueIndexReservationRecordState::
                                                  committed_published;
                     }),
      ledger->reservations.end());
  const auto removed = before - ledger->reservations.size();

  auto result = MakeResult(OkStatus(),
                           UniqueIndexReservationDecision::rollback_cleaned,
                           UniqueIndexReservationConflictState::none,
                           "INDEX.UNIQUE_RESERVATION.ROLLBACK_CLEANED",
                           "core.index.unique_reservation.rollback_cleaned",
                           "rolled-back transaction reservations removed");
  result.rollback_cleanup_performed = true;
  result.reservations_removed = static_cast<u64>(removed);
  result.evidence.push_back("mga_durable_rollback_proof=true");
  result.evidence.push_back("rollback_cleanup_removed_reservations=" +
                            std::to_string(removed));
  result.evidence.push_back("rollback_cleanup_inferred=false");
  PushEvidence(ledger,
               EvidenceRecord(ledger,
                              "rollback_cleanup",
                              TypedUuid{},
                              TypedUuid{},
                              TypedUuid{},
                              request.transaction_uuid,
                              request.local_transaction_id,
                              result.diagnostic.diagnostic_code,
                              removed != 0));
  return result;
}

DiagnosticRecord MakeUniqueIndexReservationDiagnostic(
    Status status,
    std::string diagnostic_code,
    std::string message_key,
    std::string detail) {
  std::vector<DiagnosticArgument> arguments;
  if (!detail.empty()) {
    arguments.push_back({"detail", std::move(detail)});
  }
  return MakeDiagnostic(
      status.code,
      status.severity,
      status.subsystem,
      std::move(diagnostic_code),
      std::move(message_key),
      std::move(arguments),
      {},
      "core.index.unique_index_reservation_ledger",
      status.ok()
          ? ""
          : "retry only after engine MGA transaction inventory supplies explicit state proof");
}

}  // namespace scratchbird::core::index

// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// SB-UNIQUE-INDEX-RESERVATION-LEDGER-ANCHOR
#include "runtime_platform.hpp"
#include "transaction_state.hpp"
#include "uuid.hpp"

#include <string>
#include <vector>

namespace scratchbird::core::index {

using scratchbird::core::platform::DiagnosticRecord;
using scratchbird::core::platform::Status;
using scratchbird::core::platform::TypedUuid;
using scratchbird::core::platform::byte;
using scratchbird::core::platform::u32;
using scratchbird::core::platform::u64;
using scratchbird::transaction::mga::TransactionState;

inline constexpr const char* kUniqueIndexReservationProtocolGateToken =
    "IRC-021.unique_reservation_ledger.protocol_gate";

enum class UniqueIndexReservationNullPolicy : u32 {
  nulls_distinct = 1,
  nulls_not_distinct = 2
};

enum class UniqueIndexReservationActiveConflictPolicy : u32 {
  wait_for_mga = 1,
  refuse_candidate = 2
};

enum class UniqueIndexReservationConflictState : u32 {
  none = 0,
  wait_for_mga = 1,
  refuse_candidate = 2,
  committed_refused = 3,
  validated_refused = 4,
  same_row_proof_required = 5,
  unsafe_refused = 6,
  duplicate_in_transaction_refused = 7
};

enum class UniqueIndexReservationRecordState : u32 {
  reserved = 1,
  commit_validated = 2,
  committed_published = 3
};

enum class UniqueIndexReservationDecision : u32 {
  reserved = 1,
  bypassed_partial_predicate = 2,
  bypassed_nulls_distinct = 3,
  wait_for_mga = 4,
  refused = 5,
  rollback_cleaned = 6,
  commit_validated = 7,
  commit_published = 8
};

struct UniqueIndexReservationTransactionProof {
  TypedUuid transaction_uuid;
  u64 local_transaction_id = 0;
  TransactionState state = TransactionState::none;
  bool engine_mga_authority = false;
  bool durable_transaction_inventory_authoritative = false;
  bool durable_commit_evidence = false;
  bool durable_rollback_evidence = false;
  std::string evidence_token;
};

struct UniqueIndexReservationRequest {
  TypedUuid index_uuid;
  TypedUuid table_uuid;
  TypedUuid constraint_uuid;
  TypedUuid row_uuid;
  TypedUuid version_uuid;
  TypedUuid transaction_uuid;
  u64 local_transaction_id = 0;
  std::vector<byte> encoded_key;
  UniqueIndexReservationNullPolicy null_policy =
      UniqueIndexReservationNullPolicy::nulls_distinct;
  bool null_policy_proven = false;
  bool incoming_key_has_null = false;
  bool partial_predicate_participates = true;
  bool partial_predicate_proven = false;
  bool same_row_update_proven = false;
  TypedUuid same_row_proof_uuid;
  UniqueIndexReservationActiveConflictPolicy active_conflict_policy =
      UniqueIndexReservationActiveConflictPolicy::wait_for_mga;
  std::vector<UniqueIndexReservationTransactionProof> transaction_state_proofs;
};

struct UniqueIndexReservationRecord {
  u64 reservation_sequence = 0;
  TypedUuid index_uuid;
  TypedUuid table_uuid;
  TypedUuid constraint_uuid;
  TypedUuid row_uuid;
  TypedUuid version_uuid;
  TypedUuid transaction_uuid;
  u64 local_transaction_id = 0;
  std::vector<byte> encoded_key;
  UniqueIndexReservationNullPolicy null_policy =
      UniqueIndexReservationNullPolicy::nulls_distinct;
  bool incoming_key_has_null = false;
  bool partial_predicate_participates = true;
  bool same_row_update_proven = false;
  TypedUuid same_row_proof_uuid;
  UniqueIndexReservationRecordState state =
      UniqueIndexReservationRecordState::reserved;
  std::string reservation_evidence_token;
  std::string commit_validation_evidence_token;
  std::string commit_publication_evidence_token;
};

struct UniqueIndexReservationConflict {
  u64 reservation_sequence = 0;
  TypedUuid transaction_uuid;
  u64 local_transaction_id = 0;
  TypedUuid row_uuid;
  TypedUuid version_uuid;
  UniqueIndexReservationRecordState record_state =
      UniqueIndexReservationRecordState::reserved;
  TransactionState observed_transaction_state = TransactionState::none;
  UniqueIndexReservationConflictState conflict_state =
      UniqueIndexReservationConflictState::none;
  bool same_row = false;
  bool same_key_identity = true;
  bool mga_state_proof_present = false;
  bool visibility_authority = false;
  bool authorization_authority = false;
  bool transaction_finality_authority = false;
  bool cleanup_authority = false;
  bool recovery_authority = false;
};

struct UniqueIndexReservationEvidenceRecord {
  u64 sequence = 0;
  std::string operation;
  TypedUuid index_uuid;
  TypedUuid table_uuid;
  TypedUuid constraint_uuid;
  TypedUuid transaction_uuid;
  u64 local_transaction_id = 0;
  std::string diagnostic_code;
  bool ledger_state_changed = false;
  bool visibility_authority = false;
  bool authorization_authority = false;
  bool transaction_finality_authority = false;
  bool cleanup_authority = false;
  bool recovery_authority = false;
};

struct UniqueIndexReservationLedger {
  std::vector<UniqueIndexReservationRecord> reservations;
  std::vector<UniqueIndexReservationEvidenceRecord> evidence;
  u64 next_reservation_sequence = 1;
  u64 next_evidence_sequence = 1;
};

struct UniqueIndexReservationResult {
  Status status;
  DiagnosticRecord diagnostic;
  UniqueIndexReservationDecision decision =
      UniqueIndexReservationDecision::refused;
  UniqueIndexReservationConflictState conflict_state =
      UniqueIndexReservationConflictState::none;
  bool reserved = false;
  bool bypassed = false;
  bool conflict = false;
  bool same_row_update_allowed = false;
  bool rollback_cleanup_performed = false;
  bool commit_validation_passed = false;
  bool commit_publication_marked = false;
  u64 reservation_sequence = 0;
  u64 reservations_removed = 0;
  std::vector<UniqueIndexReservationConflict> conflicts;
  std::vector<std::string> evidence;

  bool ok() const { return status.ok(); }
};

struct UniqueIndexCommitValidationRequest {
  TypedUuid transaction_uuid;
  u64 local_transaction_id = 0;
  std::vector<UniqueIndexReservationTransactionProof> transaction_state_proofs;
  std::string validation_evidence_token;
};

struct UniqueIndexCommitPublicationRequest {
  TypedUuid transaction_uuid;
  u64 local_transaction_id = 0;
  UniqueIndexReservationTransactionProof durable_commit_proof;
};

struct UniqueIndexRollbackCleanupRequest {
  TypedUuid transaction_uuid;
  u64 local_transaction_id = 0;
  UniqueIndexReservationTransactionProof rollback_proof;
};

struct UniqueIndexReservationProtocolGateRequest {
  bool enabled = false;
  bool reservation_api_present = false;
  bool commit_validation_api_present = false;
  bool rollback_cleanup_api_present = false;
  bool commit_publication_requires_mga_durable_proof = false;
  bool non_authority_evidence_flags_present = false;
  bool standalone_lifecycle_tests_present = false;
};

struct UniqueIndexReservationProtocolGateResult {
  Status status;
  DiagnosticRecord diagnostic;
  bool protocol_present = false;
  bool protocol_enabled = false;
  bool protocol_proven = false;
  std::string proof_token;
  std::vector<std::string> evidence;

  bool ok() const { return status.ok() && protocol_present &&
                           protocol_enabled && protocol_proven; }
};

const char* UniqueIndexReservationNullPolicyName(
    UniqueIndexReservationNullPolicy policy);
const char* UniqueIndexReservationActiveConflictPolicyName(
    UniqueIndexReservationActiveConflictPolicy policy);
const char* UniqueIndexReservationConflictStateName(
    UniqueIndexReservationConflictState state);
const char* UniqueIndexReservationRecordStateName(
    UniqueIndexReservationRecordState state);
const char* UniqueIndexReservationDecisionName(
    UniqueIndexReservationDecision decision);

UniqueIndexReservationProtocolGateResult EvaluateUniqueIndexReservationProtocolGate(
    const UniqueIndexReservationProtocolGateRequest& request);
UniqueIndexReservationResult ReserveUniqueIndexKey(
    UniqueIndexReservationLedger* ledger,
    const UniqueIndexReservationRequest& request);
UniqueIndexReservationResult ValidateUniqueIndexCommitBatch(
    UniqueIndexReservationLedger* ledger,
    const UniqueIndexCommitValidationRequest& request);
UniqueIndexReservationResult PublishUniqueIndexCommit(
    UniqueIndexReservationLedger* ledger,
    const UniqueIndexCommitPublicationRequest& request);
UniqueIndexReservationResult CleanupUniqueIndexReservationsForRollback(
    UniqueIndexReservationLedger* ledger,
    const UniqueIndexRollbackCleanupRequest& request);
DiagnosticRecord MakeUniqueIndexReservationDiagnostic(
    Status status,
    std::string diagnostic_code,
    std::string message_key,
    std::string detail = {});

}  // namespace scratchbird::core::index

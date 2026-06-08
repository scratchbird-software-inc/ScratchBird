// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "unique_index_deferral_policy.hpp"
#include "unique_index_reservation_ledger.hpp"
#include "uuid.hpp"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace idx = scratchbird::core::index;
namespace platform = scratchbird::core::platform;
namespace uuid = scratchbird::core::uuid;
namespace mga = scratchbird::transaction::mga;

namespace {

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << "unique_index_reservation_ledger_gate: " << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view message) {
  if (!condition) {
    Fail(message);
  }
}

bool HasEvidence(const std::vector<std::string>& evidence,
                 std::string_view needle) {
  return std::any_of(evidence.begin(),
                     evidence.end(),
                     [&](const std::string& value) {
                       return value == needle ||
                              value.find(needle) != std::string::npos;
                     });
}

platform::TypedUuid GeneratedUuid(platform::UuidKind kind,
                                  platform::u64 salt) {
  const auto generated = uuid::GenerateEngineIdentityV7(kind,
                                                        1700000000000ull + salt);
  Require(generated.ok(), "uuid generation failed");
  return generated.value;
}

std::vector<platform::byte> EncodedKey(platform::byte group,
                                       platform::byte suffix) {
  return {'S', 'B', 'K', 'O', 0x7f, group, suffix, 0x00};
}

idx::UniqueIndexReservationTransactionProof Proof(
    platform::TypedUuid transaction_uuid,
    platform::u64 local_transaction_id,
    mga::TransactionState state,
    std::string token,
    bool durable_commit = false,
    bool durable_rollback = false) {
  idx::UniqueIndexReservationTransactionProof proof;
  proof.transaction_uuid = transaction_uuid;
  proof.local_transaction_id = local_transaction_id;
  proof.state = state;
  proof.engine_mga_authority = true;
  proof.durable_transaction_inventory_authoritative = true;
  proof.durable_commit_evidence = durable_commit;
  proof.durable_rollback_evidence = durable_rollback;
  proof.evidence_token = std::move(token);
  return proof;
}

idx::UniqueIndexReservationRequest Request(
    platform::TypedUuid index_uuid,
    platform::TypedUuid table_uuid,
    platform::TypedUuid constraint_uuid,
    platform::TypedUuid transaction_uuid,
    platform::u64 local_transaction_id,
    platform::TypedUuid row_uuid,
    platform::TypedUuid version_uuid,
    std::vector<platform::byte> encoded_key) {
  idx::UniqueIndexReservationRequest request;
  request.index_uuid = index_uuid;
  request.table_uuid = table_uuid;
  request.constraint_uuid = constraint_uuid;
  request.transaction_uuid = transaction_uuid;
  request.local_transaction_id = local_transaction_id;
  request.row_uuid = row_uuid;
  request.version_uuid = version_uuid;
  request.encoded_key = std::move(encoded_key);
  request.null_policy = idx::UniqueIndexReservationNullPolicy::nulls_not_distinct;
  request.null_policy_proven = true;
  request.partial_predicate_participates = true;
  request.partial_predicate_proven = true;
  return request;
}

void RequireNonAuthority(const idx::UniqueIndexReservationResult& result,
                         std::string_view label) {
  Require(HasEvidence(result.evidence, "visibility_authority=false"),
          std::string(label) + " claimed visibility authority");
  Require(HasEvidence(result.evidence, "authorization_authority=false"),
          std::string(label) + " claimed authorization authority");
  Require(HasEvidence(result.evidence, "transaction_finality_authority=false"),
          std::string(label) + " claimed transaction finality authority");
  Require(HasEvidence(result.evidence, "cleanup_authority=false"),
          std::string(label) + " claimed cleanup authority");
  Require(HasEvidence(result.evidence, "recovery_authority=false"),
          std::string(label) + " claimed recovery authority");
  for (const auto& conflict : result.conflicts) {
    Require(!conflict.visibility_authority,
            std::string(label) + " conflict claimed visibility authority");
    Require(!conflict.authorization_authority,
            std::string(label) + " conflict claimed authorization authority");
    Require(!conflict.transaction_finality_authority,
            std::string(label) + " conflict claimed finality authority");
    Require(!conflict.cleanup_authority,
            std::string(label) + " conflict claimed cleanup authority");
    Require(!conflict.recovery_authority,
            std::string(label) + " conflict claimed recovery authority");
  }
}

void ProtocolGateAdmitsDeferralPolicy() {
  const auto default_gate = idx::EvaluateUniqueIndexReservationProtocolGate({});
  Require(!default_gate.ok(),
          "default unique reservation protocol gate unexpectedly passed");
  Require(default_gate.proof_token.empty(),
          "default unique reservation protocol gate returned a proof token");

  idx::UniqueIndexReservationProtocolGateRequest gate_request;
  gate_request.enabled = true;
  gate_request.reservation_api_present = true;
  gate_request.commit_validation_api_present = true;
  gate_request.rollback_cleanup_api_present = true;
  gate_request.commit_publication_requires_mga_durable_proof = true;
  gate_request.non_authority_evidence_flags_present = true;
  gate_request.standalone_lifecycle_tests_present = true;
  const auto gate = idx::EvaluateUniqueIndexReservationProtocolGate(gate_request);
  Require(gate.ok(), "IRC-021 protocol gate did not prove the ledger");
  Require(gate.proof_token == idx::kUniqueIndexReservationProtocolGateToken,
          "IRC-021 protocol gate token mismatch");
  Require(HasEvidence(gate.evidence, "transaction_finality_authority=false"),
          "IRC-021 protocol gate claimed finality authority");

  idx::UniqueIndexDeferralPolicyRequest policy;
  policy.uniqueness = idx::IndexUniquenessClass::unique_secondary;
  policy.request_kind =
      idx::IndexDeferralRequestKind::unique_deferred_with_reservation;
  policy.reservation_protocol_present = gate.protocol_present;
  policy.reservation_protocol_enabled = gate.protocol_enabled;
  policy.reservation_protocol_proven = gate.protocol_proven;
  policy.reservation_protocol_gate_token = gate.proof_token;
  const auto accepted = idx::EvaluateUniqueIndexDeferralPolicy(policy);
  Require(accepted.ok(), "proven reservation protocol did not admit deferral");
  Require(accepted.deferred_eligible,
          "proven reservation protocol was not marked deferred eligible");
  Require(!accepted.synchronous_required,
          "proven reservation protocol still required synchronous maintenance");
  Require(accepted.diagnostic.diagnostic_code ==
              "INDEX.UNIQUE_DEFERRAL.RESERVATION_ACCEPTED",
          "unique deferral accepted diagnostic mismatch");

  policy.reservation_protocol_gate_token.clear();
  const auto forged = idx::EvaluateUniqueIndexDeferralPolicy(policy);
  Require(!forged.ok(),
          "asserted reservation booleans were accepted without ledger proof token");
  Require(forged.diagnostic.diagnostic_code ==
              "INDEX.UNIQUE_DEFERRAL.RESERVATION_UNPROVEN",
          "forged reservation proof diagnostic mismatch");
}

void ReservationLifecycle() {
  const auto index_uuid = GeneratedUuid(platform::UuidKind::object, 10);
  const auto table_uuid = GeneratedUuid(platform::UuidKind::object, 11);
  const auto constraint_uuid = GeneratedUuid(platform::UuidKind::object, 12);
  const auto tx1 = GeneratedUuid(platform::UuidKind::transaction, 20);
  const auto tx2 = GeneratedUuid(platform::UuidKind::transaction, 21);
  const auto tx3 = GeneratedUuid(platform::UuidKind::transaction, 22);

  idx::UniqueIndexReservationLedger ledger;
  auto first = Request(index_uuid,
                       table_uuid,
                       constraint_uuid,
                       tx1,
                       101,
                       GeneratedUuid(platform::UuidKind::row, 30),
                       GeneratedUuid(platform::UuidKind::row, 31),
                       EncodedKey(0x10, 0x01));
  const auto first_result = idx::ReserveUniqueIndexKey(&ledger, first);
  Require(first_result.ok(), "first unique reservation failed");
  Require(first_result.reserved, "first unique reservation was not recorded");
  Require(ledger.reservations.size() == 1,
          "first unique reservation ledger count mismatch");
  RequireNonAuthority(first_result, "first reservation");

  auto same_tx_duplicate = Request(index_uuid,
                                   table_uuid,
                                   constraint_uuid,
                                   tx1,
                                   101,
                                   GeneratedUuid(platform::UuidKind::row, 32),
                                   GeneratedUuid(platform::UuidKind::row, 33),
                                   first.encoded_key);
  const auto same_tx_result =
      idx::ReserveUniqueIndexKey(&ledger, same_tx_duplicate);
  Require(same_tx_result.ok(),
          "same-transaction duplicate should defer to commit validation");
  Require(same_tx_result.reserved,
          "same-transaction duplicate was not recorded for commit validation");
  Require(ledger.reservations.size() == 2,
          "same-transaction duplicate ledger count mismatch");
  RequireNonAuthority(same_tx_result, "same transaction duplicate");

  auto wait_request = Request(index_uuid,
                              table_uuid,
                              constraint_uuid,
                              tx2,
                              202,
                              GeneratedUuid(platform::UuidKind::row, 34),
                              GeneratedUuid(platform::UuidKind::row, 35),
                              first.encoded_key);
  wait_request.transaction_state_proofs.push_back(
      Proof(tx1, 101, mga::TransactionState::active, "tx1-active"));
  const auto wait_result = idx::ReserveUniqueIndexKey(&ledger, wait_request);
  Require(wait_result.ok(), "active duplicate wait result should be reportable");
  Require(wait_result.conflict, "active duplicate wait did not report conflict");
  Require(wait_result.conflict_state ==
              idx::UniqueIndexReservationConflictState::wait_for_mga,
          "active duplicate did not return wait state");
  Require(ledger.reservations.size() == 2,
          "active wait conflict created a reservation");
  RequireNonAuthority(wait_result, "active wait conflict");

  wait_request.active_conflict_policy =
      idx::UniqueIndexReservationActiveConflictPolicy::refuse_candidate;
  const auto refuse_result = idx::ReserveUniqueIndexKey(&ledger, wait_request);
  Require(!refuse_result.ok(), "active duplicate refuse unexpectedly succeeded");
  Require(refuse_result.conflict_state ==
              idx::UniqueIndexReservationConflictState::refuse_candidate,
          "active duplicate did not return refuse state");
  RequireNonAuthority(refuse_result, "active refuse conflict");

  auto unsafe_request = wait_request;
  unsafe_request.transaction_state_proofs.clear();
  const auto unsafe_result = idx::ReserveUniqueIndexKey(&ledger, unsafe_request);
  Require(!unsafe_result.ok(), "missing MGA state proof was accepted");
  Require(unsafe_result.conflict_state ==
              idx::UniqueIndexReservationConflictState::unsafe_refused,
          "missing MGA state proof did not refuse as unsafe");

  idx::UniqueIndexReservationLedger same_row_ledger;
  auto original = Request(index_uuid,
                          table_uuid,
                          constraint_uuid,
                          tx1,
                          101,
                          GeneratedUuid(platform::UuidKind::row, 40),
                          GeneratedUuid(platform::UuidKind::row, 41),
                          EncodedKey(0x11, 0x01));
  Require(idx::ReserveUniqueIndexKey(&same_row_ledger, original).reserved,
          "same-row setup reservation failed");
  auto same_row_no_proof = original;
  same_row_no_proof.version_uuid = GeneratedUuid(platform::UuidKind::row, 42);
  const auto no_proof =
      idx::ReserveUniqueIndexKey(&same_row_ledger, same_row_no_proof);
  Require(!no_proof.ok(), "same-row update without proof was accepted");
  Require(no_proof.conflict_state ==
              idx::UniqueIndexReservationConflictState::same_row_proof_required,
          "same-row update without proof diagnostic mismatch");
  auto same_row_with_proof = same_row_no_proof;
  same_row_with_proof.same_row_update_proven = true;
  same_row_with_proof.same_row_proof_uuid = original.row_uuid;
  const auto with_proof =
      idx::ReserveUniqueIndexKey(&same_row_ledger, same_row_with_proof);
  Require(with_proof.ok(), "same-row update with proof was refused");
  Require(with_proof.same_row_update_allowed,
          "same-row update with proof was not marked allowed");

  auto partial = Request(index_uuid,
                         table_uuid,
                         constraint_uuid,
                         tx3,
                         303,
                         GeneratedUuid(platform::UuidKind::row, 50),
                         GeneratedUuid(platform::UuidKind::row, 51),
                         EncodedKey(0x12, 0x01));
  partial.partial_predicate_participates = false;
  const auto partial_bypass = idx::ReserveUniqueIndexKey(&ledger, partial);
  Require(partial_bypass.ok(), "partial predicate bypass failed");
  Require(partial_bypass.bypassed, "partial predicate bypass not marked");
  Require(ledger.reservations.size() == 2,
          "partial predicate bypass created a reservation");

  auto null_distinct = partial;
  null_distinct.partial_predicate_participates = true;
  null_distinct.incoming_key_has_null = true;
  null_distinct.null_policy =
      idx::UniqueIndexReservationNullPolicy::nulls_distinct;
  null_distinct.encoded_key = EncodedKey(0x13, 0x00);
  const auto null_bypass = idx::ReserveUniqueIndexKey(&ledger, null_distinct);
  Require(null_bypass.ok(), "nulls-distinct bypass failed");
  Require(null_bypass.bypassed, "nulls-distinct bypass not marked");
  Require(ledger.reservations.size() == 2,
          "nulls-distinct bypass created a reservation");

  idx::UniqueIndexReservationLedger null_not_distinct_ledger;
  auto null_not_distinct = null_distinct;
  null_not_distinct.null_policy =
      idx::UniqueIndexReservationNullPolicy::nulls_not_distinct;
  null_not_distinct.encoded_key = EncodedKey(0x13, 0x01);
  const auto null_not_distinct_first =
      idx::ReserveUniqueIndexKey(&null_not_distinct_ledger, null_not_distinct);
  Require(null_not_distinct_first.ok(),
          "nulls-not-distinct first reservation failed");
  Require(null_not_distinct_first.reserved,
          "nulls-not-distinct first reservation was not recorded");
  Require(HasEvidence(null_not_distinct_first.evidence,
                      "unique_null_policy=nulls_not_distinct"),
          "nulls-not-distinct policy evidence missing");
  auto null_not_distinct_duplicate = null_not_distinct;
  null_not_distinct_duplicate.transaction_uuid = tx2;
  null_not_distinct_duplicate.local_transaction_id = 202;
  null_not_distinct_duplicate.row_uuid =
      GeneratedUuid(platform::UuidKind::row, 56);
  null_not_distinct_duplicate.version_uuid =
      GeneratedUuid(platform::UuidKind::row, 57);
  null_not_distinct_duplicate.transaction_state_proofs.push_back(
      Proof(tx3, 303, mga::TransactionState::active, "null-tx3-active"));
  const auto null_not_distinct_wait =
      idx::ReserveUniqueIndexKey(&null_not_distinct_ledger,
                                 null_not_distinct_duplicate);
  Require(null_not_distinct_wait.ok(),
          "nulls-not-distinct active duplicate wait failed");
  Require(null_not_distinct_wait.conflict_state ==
              idx::UniqueIndexReservationConflictState::wait_for_mga,
          "nulls-not-distinct duplicate did not wait on active transaction");

  auto rollback_key = Request(index_uuid,
                              table_uuid,
                              constraint_uuid,
                              tx3,
                              303,
                              GeneratedUuid(platform::UuidKind::row, 52),
                              GeneratedUuid(platform::UuidKind::row, 53),
                              EncodedKey(0x14, 0x01));
  Require(idx::ReserveUniqueIndexKey(&ledger, rollback_key).reserved,
          "rollback setup reservation failed");
  idx::UniqueIndexRollbackCleanupRequest rollback;
  rollback.transaction_uuid = tx3;
  rollback.local_transaction_id = 303;
  rollback.rollback_proof =
      Proof(tx3, 303, mga::TransactionState::rolled_back, "tx3-rollback", false, true);
  const auto cleanup =
      idx::CleanupUniqueIndexReservationsForRollback(&ledger, rollback);
  Require(cleanup.ok(), "rollback cleanup failed");
  Require(cleanup.rollback_cleanup_performed,
          "rollback cleanup not marked performed");
  Require(cleanup.reservations_removed == 1,
          "rollback cleanup removed wrong reservation count");
  auto after_cleanup = rollback_key;
  after_cleanup.transaction_uuid = tx2;
  after_cleanup.local_transaction_id = 202;
  after_cleanup.row_uuid = GeneratedUuid(platform::UuidKind::row, 54);
  after_cleanup.version_uuid = GeneratedUuid(platform::UuidKind::row, 55);
  const auto after_cleanup_result =
      idx::ReserveUniqueIndexKey(&ledger, after_cleanup);
  Require(after_cleanup_result.ok(),
          "reservation after rollback cleanup was refused");
  Require(after_cleanup_result.reserved,
          "reservation after rollback cleanup was not recorded");
}

void CommitValidationAndPublication() {
  const auto index_uuid = GeneratedUuid(platform::UuidKind::object, 110);
  const auto table_uuid = GeneratedUuid(platform::UuidKind::object, 111);
  const auto constraint_uuid = GeneratedUuid(platform::UuidKind::object, 112);
  const auto tx1 = GeneratedUuid(platform::UuidKind::transaction, 120);
  const auto tx2 = GeneratedUuid(platform::UuidKind::transaction, 121);
  const auto tx3 = GeneratedUuid(platform::UuidKind::transaction, 122);

  idx::UniqueIndexReservationLedger ledger;
  auto reservation = Request(index_uuid,
                             table_uuid,
                             constraint_uuid,
                             tx1,
                             401,
                             GeneratedUuid(platform::UuidKind::row, 130),
                             GeneratedUuid(platform::UuidKind::row, 131),
                             EncodedKey(0x20, 0x01));
  Require(idx::ReserveUniqueIndexKey(&ledger, reservation).reserved,
          "commit validation setup reservation failed");

  idx::UniqueIndexCommitValidationRequest validation;
  validation.transaction_uuid = tx1;
  validation.local_transaction_id = 401;
  validation.validation_evidence_token = "tx1-unique-validation";
  validation.transaction_state_proofs.push_back(
      Proof(tx1, 401, mga::TransactionState::committing, "tx1-committing"));
  const auto validated =
      idx::ValidateUniqueIndexCommitBatch(&ledger, validation);
  Require(validated.ok(), "commit-time validation failed");
  Require(validated.commit_validation_passed,
          "commit-time validation not marked passed");
  Require(ledger.reservations.front().state ==
              idx::UniqueIndexReservationRecordState::commit_validated,
          "commit validation did not mark reservation as validated");
  Require(HasEvidence(validated.evidence,
                      "commit_validation_does_not_publish_finality=true"),
          "commit validation claimed publication/finality");
  RequireNonAuthority(validated, "commit validation");

  auto validated_conflict = Request(index_uuid,
                                    table_uuid,
                                    constraint_uuid,
                                    tx2,
                                    402,
                                    GeneratedUuid(platform::UuidKind::row, 132),
                                    GeneratedUuid(platform::UuidKind::row, 133),
                                    reservation.encoded_key);
  const auto validated_refusal =
      idx::ReserveUniqueIndexKey(&ledger, validated_conflict);
  Require(!validated_refusal.ok(),
          "validated reservation conflict was accepted");
  Require(validated_refusal.conflict_state ==
              idx::UniqueIndexReservationConflictState::validated_refused,
          "validated reservation conflict state mismatch");

  idx::UniqueIndexCommitPublicationRequest missing_publication;
  missing_publication.transaction_uuid = tx1;
  missing_publication.local_transaction_id = 401;
  const auto missing_publish =
      idx::PublishUniqueIndexCommit(&ledger, missing_publication);
  Require(!missing_publish.ok(),
          "commit publication without durable MGA proof was accepted");
  Require(missing_publish.diagnostic.diagnostic_code ==
              "INDEX.UNIQUE_RESERVATION.COMMIT_PUBLICATION_PROOF_REQUIRED",
          "missing commit proof diagnostic mismatch");

  idx::UniqueIndexCommitPublicationRequest publication;
  publication.transaction_uuid = tx1;
  publication.local_transaction_id = 401;
  publication.durable_commit_proof =
      Proof(tx1, 401, mga::TransactionState::committed, "tx1-commit", true);
  const auto published = idx::PublishUniqueIndexCommit(&ledger, publication);
  Require(published.ok(), "commit publication with durable MGA proof failed");
  Require(published.commit_publication_marked,
          "commit publication was not marked");
  Require(ledger.reservations.front().state ==
              idx::UniqueIndexReservationRecordState::committed_published,
          "commit publication did not mark reservation committed");
  Require(HasEvidence(published.evidence, "commit_publication_inferred=false"),
          "commit publication was inferred locally");
  RequireNonAuthority(published, "commit publication");

  auto committed_conflict = Request(index_uuid,
                                    table_uuid,
                                    constraint_uuid,
                                    tx3,
                                    403,
                                    GeneratedUuid(platform::UuidKind::row, 134),
                                    GeneratedUuid(platform::UuidKind::row, 135),
                                    reservation.encoded_key);
  const auto committed_refusal =
      idx::ReserveUniqueIndexKey(&ledger, committed_conflict);
  Require(!committed_refusal.ok(),
          "committed reservation conflict was accepted");
  Require(committed_refusal.conflict_state ==
              idx::UniqueIndexReservationConflictState::committed_refused,
          "committed reservation conflict state mismatch");

  idx::UniqueIndexReservationLedger duplicate_ledger;
  const auto dup_tx = GeneratedUuid(platform::UuidKind::transaction, 140);
  auto dup_a = Request(index_uuid,
                       table_uuid,
                       constraint_uuid,
                       dup_tx,
                       501,
                       GeneratedUuid(platform::UuidKind::row, 141),
                       GeneratedUuid(platform::UuidKind::row, 142),
                       EncodedKey(0x21, 0x01));
  auto dup_b = dup_a;
  dup_b.row_uuid = GeneratedUuid(platform::UuidKind::row, 143);
  dup_b.version_uuid = GeneratedUuid(platform::UuidKind::row, 144);
  Require(idx::ReserveUniqueIndexKey(&duplicate_ledger, dup_a).reserved,
          "duplicate validation setup first reservation failed");
  Require(idx::ReserveUniqueIndexKey(&duplicate_ledger, dup_b).reserved,
          "duplicate validation setup second reservation failed");
  idx::UniqueIndexCommitValidationRequest duplicate_validation;
  duplicate_validation.transaction_uuid = dup_tx;
  duplicate_validation.local_transaction_id = 501;
  duplicate_validation.transaction_state_proofs.push_back(
      Proof(dup_tx, 501, mga::TransactionState::committing, "dup-committing"));
  const auto duplicate_refusal =
      idx::ValidateUniqueIndexCommitBatch(&duplicate_ledger,
                                          duplicate_validation);
  Require(!duplicate_refusal.ok(),
          "commit-time duplicate validation was accepted");
  Require(duplicate_refusal.conflict_state ==
              idx::UniqueIndexReservationConflictState::
                  duplicate_in_transaction_refused,
          "commit-time duplicate refusal state mismatch");
  Require(duplicate_refusal.diagnostic.diagnostic_code ==
              "INDEX.UNIQUE_RESERVATION.COMMIT_DUPLICATE_REFUSED",
          "commit-time duplicate diagnostic mismatch");
  RequireNonAuthority(duplicate_refusal, "commit duplicate refusal");
}

}  // namespace

int main() {
  ProtocolGateAdmitsDeferralPolicy();
  ReservationLifecycle();
  CommitValidationAndPublication();
  std::cout << "unique_index_reservation_ledger_gate=passed\n";
  return EXIT_SUCCESS;
}

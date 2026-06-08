// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "bulk_constraint_proof.hpp"
#include "sorted_bulk_index_build.hpp"
#include "uuid.hpp"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace bulk = scratchbird::core::bulk_load;
namespace idx = scratchbird::core::index;
namespace platform = scratchbird::core::platform;
namespace uuid = scratchbird::core::uuid;
namespace mga = scratchbird::transaction::mga;

namespace {

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << "sorted_bulk_unique_proof_gate: " << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view message) {
  if (!condition) {
    Fail(message);
  }
}

platform::TypedUuid GeneratedUuid(platform::UuidKind kind,
                                  platform::u64 salt) {
  const auto generated = uuid::GenerateEngineIdentityV7(kind,
                                                        1800000000000ull + salt);
  Require(generated.ok(), "uuid generation failed");
  return generated.value;
}

std::string UuidText(platform::UuidKind kind, platform::u64 salt) {
  return uuid::UuidToString(GeneratedUuid(kind, salt).value);
}

std::string Key(char group, char suffix) {
  std::string key = "SBKO";
  key.push_back(static_cast<char>(0x7f));
  key.push_back(group);
  key.push_back(suffix);
  return key;
}

idx::SortedBulkIndexRowInput Row(char group,
                                 char suffix,
                                 platform::u64 salt,
                                 bool null_key = false) {
  idx::SortedBulkIndexRowInput row;
  row.encoded_key = Key(group, suffix);
  row.row_uuid = UuidText(platform::UuidKind::row, salt);
  row.version_uuid = UuidText(platform::UuidKind::row, salt + 1000);
  row.payload_value = std::string("payload-") + group + suffix;
  row.source_ordinal = salt;
  row.null_key = null_key;
  return row;
}

idx::SortedBulkIndexBuildRequest Request(std::vector<idx::SortedBulkIndexRowInput> rows) {
  idx::SortedBulkIndexBuildRequest request;
  request.metadata.index_uuid = GeneratedUuid(platform::UuidKind::object, 10);
  request.metadata.table_uuid = GeneratedUuid(platform::UuidKind::object, 11);
  request.metadata.family = idx::IndexFamily::btree;
  request.metadata.family_name = "btree";
  request.metadata.semantic_profile = "sorted_bulk_unique_proof_gate";
  request.metadata.unique = true;
  request.metadata.rebuild = false;
  request.metadata.leaf_entry_capacity = 2;
  request.rows = std::move(rows);
  return request;
}

bool HasEvidence(const std::vector<idx::SortedBulkIndexBuildEvidence>& evidence,
                 std::string_view kind,
                 std::string_view id) {
  return std::any_of(evidence.begin(),
                     evidence.end(),
                     [&](const auto& item) {
                       return item.evidence_kind == kind &&
                              item.evidence_id == id;
                     });
}

bool HasEvidenceContaining(
    const std::vector<idx::SortedBulkIndexBuildEvidence>& evidence,
    std::string_view token) {
  return std::any_of(evidence.begin(),
                     evidence.end(),
                     [&](const auto& item) {
                       return item.evidence_kind.find(token) !=
                                  std::string::npos ||
                              item.evidence_id.find(token) != std::string::npos;
                     });
}

bool HasBulkEvidence(const std::vector<bulk::BulkConstraintProofEvidence>& evidence,
                     std::string_view kind,
                     std::string_view id) {
  return std::any_of(evidence.begin(),
                     evidence.end(),
                     [&](const auto& item) {
                       return item.evidence_kind == kind &&
                              item.evidence_id == id;
                     });
}

void RequireNoDocRuntimeEvidence(
    const std::vector<idx::SortedBulkIndexBuildEvidence>& evidence) {
  for (const auto& item : evidence) {
    for (const auto token : {"docs" "/execution-plans",
                             "docs" "/findings",
                             "public_release_evidence",
                             "execution_plan",
                             "findings",
                             "contracts"}) {
      Require(item.evidence_kind.find(token) == std::string::npos &&
                  item.evidence_id.find(token) == std::string::npos,
              "runtime evidence leaked documentation path token");
    }
  }
}

void RequireNonAuthority(const idx::SortedBulkIndexBuildResult& result,
                         std::string_view label) {
  Require(HasEvidence(result.evidence,
                      "sorted_bulk_unique_proof_transaction_finality_authority",
                      "false"),
          std::string(label) + " claimed finality authority");
  Require(HasEvidence(result.evidence,
                      "sorted_bulk_unique_proof_visibility_authority",
                      "false"),
          std::string(label) + " claimed visibility authority");
  Require(HasEvidence(result.evidence,
                      "sorted_bulk_unique_proof_authorization_authority",
                      "false"),
          std::string(label) + " claimed authorization authority");
  Require(HasEvidence(result.evidence,
                      "sorted_bulk_unique_proof_recovery_authority",
                      "false"),
          std::string(label) + " claimed recovery authority");
  Require(HasEvidence(result.evidence,
                      "sorted_bulk_unique_proof_physical_append_authorized",
                      "false"),
          std::string(label) + " authorized physical append");
  Require(HasEvidence(result.evidence,
                      "sorted_bulk_unique_proof_root_publish_authorized",
                      "false"),
          std::string(label) + " authorized root publish");
}

idx::UniqueIndexReservationTransactionProof Proof(
    platform::TypedUuid transaction_uuid,
    platform::u64 local_transaction_id,
    mga::TransactionState state,
    std::string token) {
  idx::UniqueIndexReservationTransactionProof proof;
  proof.transaction_uuid = transaction_uuid;
  proof.local_transaction_id = local_transaction_id;
  proof.state = state;
  proof.engine_mga_authority = true;
  proof.durable_transaction_inventory_authoritative = true;
  proof.evidence_token = std::move(token);
  return proof;
}

void AcceptedSortedUniqueBatch() {
  auto request = Request({Row('b', '1', 101), Row('a', '1', 102), Row('c', '1', 103)});
  const auto result = idx::BuildSortedExactBulkIndex(request);
  Require(result.ok(), "accepted sorted unique batch was refused");
  Require(result.entries.size() == 3, "accepted batch entry count drifted");
  Require(result.entries[0].encoded_key == Key('a', '1') &&
              result.entries[1].encoded_key == Key('b', '1') &&
              result.entries[2].encoded_key == Key('c', '1'),
          "accepted batch was not sorted by encoded key");
  Require(result.uniqueness_proven, "accepted batch did not prove uniqueness");
  Require(result.sorted_key_run_count == 3,
          "accepted batch sorted key run count drifted");
  Require(result.unique_duplicate_run_count == 0,
          "accepted batch reported duplicate runs");
  Require(HasEvidence(result.evidence,
                      "sorted_bulk_unique_proof_duplicate_absent",
                      "true"),
          "accepted batch duplicate absence evidence missing");
  Require(HasEvidence(result.evidence,
                      "sorted_bulk_index_physical_append_authorized_by_proof",
                      "false"),
          "accepted batch authorized append through proof");
  RequireNonAuthority(result, "accepted sorted unique batch");
  RequireNoDocRuntimeEvidence(result.evidence);
}

void DuplicateIncomingBatchRefused() {
  auto request = Request({Row('a', '1', 201), Row('a', '1', 202)});
  const auto result = idx::BuildSortedExactBulkIndex(request);
  Require(!result.ok(), "duplicate incoming batch was accepted");
  Require(result.uniqueness_refused, "duplicate incoming batch not marked refused");
  Require(result.diagnostic.diagnostic_code ==
              "SB-INDEX-SORTED-BULK-UNIQUE-DUPLICATE-BATCH",
          "duplicate incoming diagnostic drifted");
  Require(HasEvidence(result.evidence,
                      "sorted_bulk_unique_proof_duplicate_run_count",
                      "1"),
          "duplicate incoming run evidence missing");
  Require(HasEvidence(result.evidence,
                      "sorted_bulk_unique_proof_before_physical_append",
                      "true"),
          "duplicate incoming did not prove pre-append refusal");
  Require(result.entries.empty(), "duplicate incoming retained append entries");
}

void VisiblePersistedConflictRefused() {
  auto request = Request({Row('a', '1', 301), Row('b', '1', 302)});
  request.visible_unique_keys.push_back(Row('b', '1', 399));
  const auto result = idx::BuildSortedExactBulkIndex(request);
  Require(!result.ok(), "visible persisted duplicate was accepted");
  Require(result.unique_visible_conflict_refused,
          "visible persisted conflict flag missing");
  Require(result.diagnostic.diagnostic_code ==
              "SB-INDEX-SORTED-BULK-UNIQUE-PERSISTED-CONFLICT",
          "visible persisted conflict diagnostic drifted");
  Require(HasEvidence(result.evidence,
                      "sorted_bulk_unique_proof_persisted_conflict_absent",
                      "false"),
          "visible persisted conflict evidence missing");
  Require(result.entries.empty(), "visible persisted conflict retained entries");
}

void NullPolicies() {
  auto distinct = Request({Row('n', '1', 401, true), Row('n', '1', 402, true)});
  distinct.metadata.unique_nulls_distinct = true;
  const auto distinct_result = idx::BuildSortedExactBulkIndex(distinct);
  Require(distinct_result.ok(), "nulls-distinct repeated null keys were refused");
  Require(distinct_result.unique_effective_key_count == 0,
          "nulls-distinct did not skip null unique keys");
  Require(HasEvidence(distinct_result.evidence,
                      "sorted_bulk_unique_proof_null_keys_skipped",
                      "2"),
          "nulls-distinct skip evidence missing");

  auto not_distinct = distinct;
  not_distinct.metadata.unique_nulls_distinct = false;
  const auto not_distinct_result = idx::BuildSortedExactBulkIndex(not_distinct);
  Require(!not_distinct_result.ok(),
          "nulls-not-distinct repeated null keys were accepted");
  Require(not_distinct_result.diagnostic.diagnostic_code ==
              "SB-INDEX-SORTED-BULK-UNIQUE-DUPLICATE-BATCH",
          "nulls-not-distinct diagnostic drifted");
}

void UnsafeLegacyKeyRefused() {
  auto request = Request({Row('a', '1', 501)});
  request.rows[0].encoded_key = "SBK1legacy";
  const auto result = idx::BuildSortedExactBulkIndex(request);
  Require(!result.ok(), "unsafe legacy key was accepted");
  Require(result.unsafe_key_refused, "unsafe key flag missing");
  Require(result.diagnostic.diagnostic_code ==
              "SB-INDEX-SORTED-BULK-UNSAFE-LEGACY-KEY",
          "unsafe legacy key diagnostic drifted");
  Require(HasEvidence(result.evidence,
                      "sorted_bulk_unique_proof_unsafe_key_encoding",
                      "SBK1"),
          "unsafe key evidence missing");
}

void InvalidOrderProofRefused() {
  auto request = Request({Row('b', '1', 601), Row('a', '1', 602)});
  request.metadata.input_presorted = true;
  request.metadata.order_proof_valid = true;
  const auto result = idx::BuildSortedExactBulkIndex(request);
  Require(!result.ok(), "invalid input_presorted proof was accepted");
  Require(result.diagnostic.diagnostic_code ==
              "SB-INDEX-SORTED-BULK-ORDER-PROOF-INVALID",
          "invalid order proof diagnostic drifted");
}

void ReservationLedgerValidationEvidence() {
  auto request = Request({Row('a', '1', 701), Row('b', '1', 702)});
  idx::UniqueIndexReservationLedger ledger;
  request.unique_reservation_ledger = &ledger;
  request.validate_unique_reservation_batch = true;
  request.unique_constraint_uuid = GeneratedUuid(platform::UuidKind::object, 703);
  request.transaction_uuid = GeneratedUuid(platform::UuidKind::transaction, 704);
  request.local_transaction_id = 704;
  request.unique_reservation_validation_evidence_token =
      "sorted-bulk-unique-validation";
  request.unique_transaction_state_proofs.push_back(
      Proof(request.transaction_uuid,
            request.local_transaction_id,
            mga::TransactionState::committing,
            "active-mga-proof"));

  const auto result = idx::BuildSortedExactBulkIndex(request);
  Require(result.ok(), "reservation-ledger sorted proof was refused");
  Require(result.unique_reservation_ledger_used,
          "reservation ledger use not reported");
  Require(result.unique_reservation_validation_passed,
          "reservation validation not marked passed");
  Require(ledger.reservations.size() == 2,
          "reservation ledger did not receive validated reservations");
  Require(HasEvidence(result.evidence,
                      "sorted_bulk_unique_proof_reservation_validation_passed",
                      "true"),
          "reservation validation success evidence missing");
  Require(HasEvidenceContaining(result.evidence,
                                "commit_validation_does_not_publish_finality=true"),
          "reservation validation finality evidence missing");
  RequireNonAuthority(result, "reservation-ledger sorted proof");

  idx::UniqueIndexReservationLedger missing_ledger;
  auto missing = Request({Row('c', '1', 801)});
  missing.unique_reservation_ledger = &missing_ledger;
  missing.validate_unique_reservation_batch = true;
  missing.unique_constraint_uuid = GeneratedUuid(platform::UuidKind::object, 803);
  missing.transaction_uuid = GeneratedUuid(platform::UuidKind::transaction, 804);
  missing.local_transaction_id = 804;
  const auto missing_result = idx::BuildSortedExactBulkIndex(missing);
  Require(!missing_result.ok(), "missing MGA proof validation was accepted");
  Require(missing_result.missing_mga_proof_refused,
          "missing MGA proof flag was not set");
  Require(missing_result.diagnostic.diagnostic_code ==
              "SB-INDEX-SORTED-BULK-MGA-PROOF-REQUIRED",
          "missing MGA proof diagnostic drifted");
  Require(missing_ledger.reservations.empty(),
          "failed reservation validation mutated caller ledger");
}

bulk::BulkConstraintProofKeyRef BulkRef(char group,
                                        char suffix,
                                        platform::u64 salt,
                                        bool null_key = false) {
  bulk::BulkConstraintProofKeyRef ref;
  ref.encoded_key = Key(group, suffix);
  ref.row_uuid = UuidText(platform::UuidKind::row, salt);
  ref.version_uuid = UuidText(platform::UuidKind::row, salt + 1000);
  ref.source_ordinal = salt;
  ref.null_key = null_key;
  return ref;
}

bulk::BulkConstraintProofRequest BulkRequest() {
  bulk::BulkConstraintProofRequest request;
  request.database_uuid = GeneratedUuid(platform::UuidKind::database, 900);
  request.object_uuid = GeneratedUuid(platform::UuidKind::object, 901);
  request.transaction_uuid = GeneratedUuid(platform::UuidKind::transaction, 902);
  request.local_transaction_id = 902;
  request.route = "direct_physical_bulk";
  request.direct_physical_bulk = true;
  bulk::BulkUniqueProofRequest unique;
  unique.constraint_uuid = "constraint-uuid";
  unique.index_uuid = "index-uuid";
  unique.table_uuid = "table-uuid";
  unique.column_name = "id";
  request.unique_proofs.push_back(std::move(unique));
  return request;
}

void BulkConstraintProofPathUsesSortedUniqueEvidence() {
  auto accepted = BulkRequest();
  accepted.unique_proofs[0].incoming_keys = {BulkRef('a', '1', 1001),
                                            BulkRef('b', '1', 1002)};
  const auto accepted_result = bulk::ProveBulkConstraints(accepted);
  Require(accepted_result.ok(), "bulk constraint proof accepted case failed");
  Require(HasBulkEvidence(accepted_result.evidence,
                          "bulk_unique_proof_order",
                          "encoded_key,row_uuid,version_uuid"),
          "bulk proof sorted order evidence missing");
  Require(HasBulkEvidence(accepted_result.evidence,
                          "bulk_unique_proof_sorted_duplicate_runs_absent",
                          "true"),
          "bulk proof duplicate absence evidence missing");
  Require(HasBulkEvidence(
              accepted_result.evidence,
              "bulk_unique_proof_physical_append_selected_before_proof",
              "false"),
          "bulk proof pre-append evidence missing");

  auto visible_conflict = BulkRequest();
  visible_conflict.unique_proofs[0].incoming_keys = {BulkRef('c', '1', 1101)};
  visible_conflict.unique_proofs[0].visible_keys = {BulkRef('c', '1', 1102)};
  const auto visible_result = bulk::ProveBulkConstraints(visible_conflict);
  Require(!visible_result.ok(), "bulk visible duplicate was accepted");
  Require(visible_result.diagnostic.diagnostic_code ==
              "SB-BULK-CONSTRAINT-UNIQUE-PERSISTED-CONFLICT",
          "bulk visible conflict diagnostic drifted");

  auto unsafe = BulkRequest();
  unsafe.unique_proofs[0].incoming_keys = {BulkRef('d', '1', 1201)};
  unsafe.unique_proofs[0].incoming_keys[0].encoded_key = "SBK1legacy";
  const auto unsafe_result = bulk::ProveBulkConstraints(unsafe);
  Require(!unsafe_result.ok(), "bulk unsafe key was accepted");
  Require(unsafe_result.diagnostic.diagnostic_code ==
              "SB-BULK-CONSTRAINT-UNIQUE-UNSAFE-KEY-ENCODING",
          "bulk unsafe key diagnostic drifted");
}

}  // namespace

int main() {
  AcceptedSortedUniqueBatch();
  DuplicateIncomingBatchRefused();
  VisiblePersistedConflictRefused();
  NullPolicies();
  UnsafeLegacyKeyRefused();
  InvalidOrderProofRefused();
  ReservationLedgerValidationEvidence();
  BulkConstraintProofPathUsesSortedUniqueEvidence();
  std::cout << "sorted_bulk_unique_proof_gate=passed\n";
  return EXIT_SUCCESS;
}

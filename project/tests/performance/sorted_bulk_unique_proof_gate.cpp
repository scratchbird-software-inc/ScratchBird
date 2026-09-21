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
#include <new>
#include <string>
#include <string_view>
#include <vector>

namespace bulk = scratchbird::core::bulk_load;
namespace idx = scratchbird::core::index;
namespace platform = scratchbird::core::platform;
namespace uuid = scratchbird::core::uuid;
namespace mga = scratchbird::transaction::mga;

// Test-only allocation faults exercise the actual builder and reservation
// ledger. They do not replace an authority source or simulate durable writes.
static thread_local long allocation_failure = -1;
static thread_local bool allocation_consumed = false;
static unsigned allocation_faults = 0;
void* operator new(std::size_t size) {
  if (allocation_failure == 0) {
    allocation_failure = -1;
    allocation_consumed = true;
    throw std::bad_alloc();
  }
  if (allocation_failure > 0) --allocation_failure;
  if (auto* value = std::malloc(size == 0 ? 1 : size)) return value;
  throw std::bad_alloc();
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void* operator new(std::size_t size, const std::nothrow_t&) noexcept {
  try { return ::operator new(size); } catch (...) { return nullptr; }
}
void* operator new[](std::size_t size, const std::nothrow_t&) noexcept {
  try { return ::operator new(size); } catch (...) { return nullptr; }
}
void operator delete(void* value) noexcept { std::free(value); }
void operator delete[](void* value) noexcept { std::free(value); }
void operator delete(void* value, std::size_t) noexcept { std::free(value); }
void operator delete[](void* value, std::size_t) noexcept { std::free(value); }
void operator delete(void* value, const std::nothrow_t&) noexcept { std::free(value); }
void operator delete[](void* value, const std::nothrow_t&) noexcept { std::free(value); }

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
  row.row_uuid = GeneratedUuid(platform::UuidKind::row, salt).value;
  row.version_uuid = GeneratedUuid(platform::UuidKind::row, salt + 1000).value;
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
                       const auto* text = std::get_if<std::string>(&item.evidence_id);
                       return item.evidence_kind == kind && text != nullptr && *text == id;
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

  for (const auto member : {&idx::SortedBulkIndexBuildRequest::unique_constraint_uuid,
                            &idx::SortedBulkIndexBuildRequest::transaction_uuid}) {
    auto invalid = request;
    (invalid.*member).value.bytes[6] = 0x40;
    Require(!idx::BuildSortedExactBulkIndex(invalid).ok() &&
                ledger.reservations.empty() && ledger.evidence.empty(),
            "non-v7 reservation authority accepted or changed ledger");
    invalid = request;
    (invalid.*member).kind = platform::UuidKind::row;
    Require(!idx::BuildSortedExactBulkIndex(invalid).ok() &&
                ledger.reservations.empty() && ledger.evidence.empty(),
            "wrong reservation authority kind accepted or changed ledger");
  }

  auto invalid_visible = request;
  invalid_visible.visible_unique_keys = {Row('z', '1', 710, true)};
  invalid_visible.visible_unique_keys[0].row_uuid.bytes[6] = 0x40;
  Require(!idx::BuildSortedExactBulkIndex(invalid_visible).ok() &&
              ledger.reservations.empty(),
          "NULL skipping hid invalid visible-row identity");

  // A physical failure happens after unique reservation validation. It must
  // not publish that provisional ledger into the caller's state.
  auto rejected = request;
  rejected.metadata.physical_page_size = 170;
  rejected.rows[0].encoded_key = std::string("SBKO") + std::string(512, 'x');
  const auto failed_physical = idx::BuildSortedExactBulkIndex(rejected);
  Require(!failed_physical.ok(), "invalid physical page build was accepted");
  Require(ledger.reservations.empty() && ledger.evidence.empty() &&
              ledger.next_reservation_sequence == 1 && ledger.next_evidence_sequence == 1,
          "failed physical candidate published uniqueness reservations");

  bool fault_matrix_complete = false;
  for (long fault = 0; fault < 20000; ++fault) {
    idx::UniqueIndexReservationLedger isolated;
    auto isolated_request = request;
    isolated_request.unique_reservation_ledger = &isolated;
    allocation_consumed = false;
    allocation_failure = fault;
    bool accepted = false;
    try {
      accepted = idx::BuildSortedExactBulkIndex(isolated_request).ok();
    } catch (const std::bad_alloc&) {
    }
    allocation_failure = -1;
    if (!allocation_consumed) {
      Require(accepted && isolated.reservations.size() == 2,
              "fault-free candidate did not publish complete reservations");
      fault_matrix_complete = true;
      break;
    }
    ++allocation_faults;
    if (!accepted) {
      Require(isolated.reservations.empty() && isolated.evidence.empty() &&
                  isolated.next_reservation_sequence == 1 &&
                  isolated.next_evidence_sequence == 1,
              "allocation failure published partial uniqueness state");
    } else {
      Require(isolated.reservations.size() == 2,
              "recovered allocation failure published partial reservations");
    }
  }
  Require(fault_matrix_complete && allocation_faults != 0,
          "allocation fault matrix did not cover the complete build");

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
  ref.row_uuid = GeneratedUuid(platform::UuidKind::row, salt).value;
  ref.version_uuid = GeneratedUuid(platform::UuidKind::row, salt + 1000).value;
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
  unique.constraint_uuid = GeneratedUuid(platform::UuidKind::object, 903).value;
  unique.index_uuid = GeneratedUuid(platform::UuidKind::object, 904).value;
  unique.table_uuid = request.object_uuid.value;
  unique.column_name = "id";
  request.unique_proofs.push_back(std::move(unique));
  return request;
}

void BulkConstraintProofPathUsesSortedUniqueEvidence() {
  auto accepted = BulkRequest();
  accepted.unique_proofs[0].incoming_keys = {BulkRef('a', '1', 1001),
                                            BulkRef('b', '1', 1002)};
  accepted.unique_proofs[0].incoming_keys_presorted = true;
  const auto accepted_result = bulk::ProveBulkConstraints(accepted);
  Require(accepted_result.ok(), "bulk constraint proof accepted case failed");
  Require(HasBulkEvidence(accepted_result.evidence,
                          "bulk_unique_proof_incoming_presorted",
                          "true"),
          "bulk proof presorted evidence missing");
  Require(HasBulkEvidence(accepted_result.evidence,
                          "bulk_unique_proof_presorted_order_valid",
                          "true"),
          "bulk proof presorted validation evidence missing");
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

  auto invalid_presorted = BulkRequest();
  invalid_presorted.unique_proofs[0].incoming_keys = {
      BulkRef('f', '1', 1301),
      BulkRef('e', '1', 1302)};
  invalid_presorted.unique_proofs[0].incoming_keys_presorted = true;
  const auto invalid_presorted_result =
      bulk::ProveBulkConstraints(invalid_presorted);
  Require(!invalid_presorted_result.ok(),
          "bulk invalid presorted order was accepted");
  Require(invalid_presorted_result.diagnostic.diagnostic_code ==
              "SB-BULK-CONSTRAINT-UNIQUE-PRESORTED-ORDER-INVALID",
          "bulk invalid presorted diagnostic drifted");
}

void BulkBinaryIdentityAdmission() {
  auto base = BulkRequest();
  base.unique_proofs[0].incoming_keys = {BulkRef('a', '1', 1501)};
  for (const auto member : {&bulk::BulkConstraintProofRequest::database_uuid,
                            &bulk::BulkConstraintProofRequest::object_uuid,
                            &bulk::BulkConstraintProofRequest::transaction_uuid}) {
    for (const bool no_constraints : {false, true}) {
      auto invalid = base;
      if (no_constraints) invalid.unique_proofs.clear();
      (invalid.*member).value.bytes[6] = 0x40;
      Require(!bulk::ProveBulkConstraints(invalid).ok(),
              "old UUID admitted as bulk authority");
      (invalid.*member) = {};
      Require(!bulk::ProveBulkConstraints(invalid).ok(),
              "missing identity admitted as bulk authority");
      (invalid.*member) = base.*member;
      (invalid.*member).kind = platform::UuidKind::row;
      Require(!bulk::ProveBulkConstraints(invalid).ok(),
              "wrong typed kind admitted as bulk authority");
    }
  }
  auto empty = base;
  empty.unique_proofs.clear();
  Require(bulk::ProveBulkConstraints(empty).ok(), "valid no-constraint request refused");
  empty.local_transaction_id = 0;
  Require(!bulk::ProveBulkConstraints(empty).ok(), "missing local transaction accepted");

  for (const std::size_t byte : {std::size_t{6}, std::size_t{8}}) {
    for (unsigned value = 0; value < 256; ++value) {
      auto candidate = base;
      candidate.unique_proofs[0].constraint_uuid.bytes[byte] =
          static_cast<platform::byte>(value);
      const bool valid = byte == 6 ? (value >> 4) == 7 : (value >> 6) == 2;
      Require(bulk::ProveBulkConstraints(candidate).ok() == valid,
              "constraint UUID version/variant admission drifted");
    }
  }
  for (const auto member : {&bulk::BulkUniqueProofRequest::constraint_uuid,
                            &bulk::BulkUniqueProofRequest::index_uuid,
                            &bulk::BulkUniqueProofRequest::table_uuid}) {
    auto invalid = base;
    (invalid.unique_proofs[0].*member) = {};
    Require(!bulk::ProveBulkConstraints(invalid).ok(), "nil unique identity accepted");
  }
  auto wrong_owner = base;
  wrong_owner.unique_proofs[0].table_uuid.bytes[15] ^= 1;
  Require(!bulk::ProveBulkConstraints(wrong_owner).ok(), "unbound proof table accepted");
  for (const bool visible : {false, true}) {
    for (const bool version : {false, true}) {
      auto invalid = base;
      auto bad = BulkRef('z', '1', 1502, true);
      (version ? bad.version_uuid : bad.row_uuid).bytes[6] = 0x40;
      (visible ? invalid.unique_proofs[0].visible_keys
               : invalid.unique_proofs[0].incoming_keys).push_back(bad);
      Require(!bulk::ProveBulkConstraints(invalid).ok(),
              "NULL filtering hid invalid unique row/version identity");
    }
  }

  auto conflict = base;
  conflict.unique_proofs[0].visible_keys = {BulkRef('a', '1', 1503)};
  const auto refused = bulk::ProveBulkConstraints(conflict);
  bool binary_constraint = false;
  for (const auto& item : refused.evidence) {
    if (item.evidence_kind == "bulk_unique_proof_conflict_constraint") {
      const auto* id = std::get_if<platform::Uuid>(&item.evidence_id);
      binary_constraint = id && *id == base.unique_proofs[0].constraint_uuid;
    }
  }
  Require(!refused.ok() && binary_constraint,
          "conflict lost binary constraint identity");

  auto foreign = base;
  foreign.unique_proofs.clear();
  bulk::BulkForeignKeyProofRequest fk;
  fk.constraint_uuid = GeneratedUuid(platform::UuidKind::object, 1600).value;
  fk.child_table_uuid = foreign.object_uuid.value;
  fk.parent_table_uuid = GeneratedUuid(platform::UuidKind::object, 1601).value;
  fk.parent_index_uuid = GeneratedUuid(platform::UuidKind::object, 1602).value;
  fk.child_keys = {BulkRef('b', '1', 1603)};
  fk.visible_parent_keys = {BulkRef('b', '1', 1604)};
  foreign.foreign_key_proofs = {fk};
  Require(bulk::ProveBulkConstraints(foreign).ok(), "valid visible FK parent refused");
  foreign.foreign_key_proofs[0].visible_parent_keys.clear();
  Require(!bulk::ProveBulkConstraints(foreign).ok(), "missing FK parent accepted");
  foreign.foreign_key_proofs[0].batch_parent_keys = fk.visible_parent_keys;
  Require(!bulk::ProveBulkConstraints(foreign).ok(), "cross-table batch parent fabricated");
  foreign.foreign_key_proofs[0].parent_table_uuid = fk.child_table_uuid;
  Require(bulk::ProveBulkConstraints(foreign).ok(), "same-table batch parent refused");
  foreign.foreign_key_proofs[0].batch_local_parent_allowed = false;
  Require(!bulk::ProveBulkConstraints(foreign).ok(), "unadmitted batch parent accepted");
  for (const auto member : {&bulk::BulkForeignKeyProofRequest::constraint_uuid,
                            &bulk::BulkForeignKeyProofRequest::child_table_uuid,
                            &bulk::BulkForeignKeyProofRequest::parent_table_uuid,
                            &bulk::BulkForeignKeyProofRequest::parent_index_uuid}) {
    foreign.foreign_key_proofs = {fk};
    (foreign.foreign_key_proofs[0].*member).bytes[8] = 0;
    Require(!bulk::ProveBulkConstraints(foreign).ok(), "invalid FK identity accepted");
  }
  for (const bool parent : {false, true}) {
    foreign.foreign_key_proofs = {fk};
    auto& keys = parent ? foreign.foreign_key_proofs[0].visible_parent_keys
                        : foreign.foreign_key_proofs[0].child_keys;
    keys[0].null_key = true;
    keys[0].version_uuid = {};
    Require(!bulk::ProveBulkConstraints(foreign).ok(), "NULL hid invalid FK row identity");
  }
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
  BulkBinaryIdentityAdmission();
  std::cout << "sorted_bulk_unique_proof_gate=passed allocation_faults="
            << allocation_faults << '\n';
  return EXIT_SUCCESS;
}

// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "repair_identity_rules.hpp"
#include "row_data_page.hpp"
#include "row_version.hpp"
#include "transaction_state.hpp"
#include "uuid.hpp"

#include <cstdlib>
#include <iostream>
#include <string_view>

namespace {

namespace page = scratchbird::storage::page;
namespace txn = scratchbird::transaction::mga;
namespace uuid = scratchbird::core::uuid;

using scratchbird::core::platform::TypedUuid;
using scratchbird::core::platform::UuidKind;
using scratchbird::core::platform::u32;
using scratchbird::core::platform::u64;

inline constexpr u64 kBaseMillis = 1771000000000ull;
inline constexpr u32 kPageSize = 8192;

bool Expect(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << message << '\n';
    return false;
  }
  return true;
}

TypedUuid MakeUuid(UuidKind kind, u64 offset) {
  const auto generated =
      uuid::GenerateEngineIdentityV7(kind, kBaseMillis + offset);
  return generated.ok() ? generated.value : TypedUuid{};
}

struct Fixture {
  TypedUuid relation_uuid = MakeUuid(UuidKind::object, 1);
  TypedUuid row_uuid = MakeUuid(UuidKind::row, 2);
  TypedUuid old_version_uuid = MakeUuid(UuidKind::row, 3);
  TypedUuid new_version_uuid = MakeUuid(UuidKind::row, 4);
  TypedUuid creator_transaction_uuid = MakeUuid(UuidKind::transaction, 5);
  TypedUuid correction_transaction_uuid = MakeUuid(UuidKind::transaction, 6);
};

txn::TransactionIdentity TransactionIdentity(TypedUuid transaction_uuid,
                                             u64 local_id) {
  txn::TransactionIdentity identity;
  identity.local_id = txn::MakeLocalTransactionId(local_id);
  identity.transaction_uuid = transaction_uuid;
  identity.scope = txn::TransactionScope::local_node;
  return identity;
}

txn::RowVersionMetadata Metadata(const Fixture& fixture,
                                 TypedUuid transaction_uuid,
                                 u64 local_id,
                                 u64 sequence,
                                 txn::RowVersionState row_state,
                                 txn::TransactionState transaction_state) {
  txn::RowVersionMetadata metadata;
  metadata.identity.row.row_uuid = fixture.row_uuid;
  metadata.identity.creator_transaction =
      TransactionIdentity(transaction_uuid, local_id);
  metadata.identity.version_sequence = sequence;
  metadata.state = row_state;
  metadata.creator_transaction_state = transaction_state;
  metadata.payload_present =
      row_state != txn::RowVersionState::rolled_back &&
      row_state != txn::RowVersionState::delete_marker;
  return metadata;
}

page::RowDataRecord RowFor(const Fixture& fixture,
                           TypedUuid transaction_uuid,
                           u64 local_id,
                           u32 version_sequence) {
  page::RowDataRecord row;
  row.row_uuid = fixture.row_uuid;
  row.transaction_uuid = transaction_uuid;
  row.local_transaction_id = local_id;
  row.row_version = version_sequence;
  row.stable_slot_id = version_sequence;
  return row;
}

page::RepairIdentityRequest BaseExactRequest(const Fixture& fixture) {
  page::RepairIdentityRequest request;
  request.action = page::RepairIdentityAction::exact_relocation;
  request.original_row =
      RowFor(fixture, fixture.creator_transaction_uuid, 10, 1);
  request.candidate_row = request.original_row;
  request.candidate_row.stable_slot_id = 9;
  request.original_metadata = Metadata(fixture,
                                       fixture.creator_transaction_uuid,
                                       10,
                                       1,
                                       txn::RowVersionState::committed,
                                       txn::TransactionState::committed);
  request.candidate_metadata = request.original_metadata;
  request.original_version_uuid = fixture.old_version_uuid;
  request.candidate_version_uuid = fixture.old_version_uuid;
  request.repair_event_digest = 0x7601;
  return request;
}

page::RepairIdentityRequest LogicalCorrectionRequest(const Fixture& fixture) {
  page::RepairIdentityRequest request;
  request.action = page::RepairIdentityAction::logical_correction;
  request.original_row =
      RowFor(fixture, fixture.creator_transaction_uuid, 10, 1);
  request.candidate_row =
      RowFor(fixture, fixture.correction_transaction_uuid, 11, 2);
  request.original_metadata = Metadata(fixture,
                                       fixture.creator_transaction_uuid,
                                       10,
                                       1,
                                       txn::RowVersionState::committed,
                                       txn::TransactionState::committed);
  request.candidate_metadata = Metadata(fixture,
                                        fixture.correction_transaction_uuid,
                                        11,
                                        2,
                                        txn::RowVersionState::uncommitted,
                                        txn::TransactionState::active);
  request.candidate_metadata.chain.previous_version_sequence = 1;
  request.candidate_metadata.chain.previous_version_uuid =
      fixture.old_version_uuid;
  request.original_metadata.chain.next_version_sequence = 2;
  request.original_metadata.chain.next_version_uuid = fixture.new_version_uuid;
  request.original_version_uuid = fixture.old_version_uuid;
  request.candidate_version_uuid = fixture.new_version_uuid;
  request.logical_payload_changed = true;
  request.authoritative_payload_proof = true;
  request.repair_event_digest = 0x7602;
  return request;
}

bool ExactRelocationProof(const Fixture& fixture) {
  bool ok = true;
  const auto decision = page::EvaluateRepairIdentityRule(
      BaseExactRequest(fixture));
  ok = Expect(decision.ok() && decision.mutation_allowed,
              "exact repair relocation should be admitted") && ok;
  ok = Expect(decision.exact_identity_preserved,
              "exact repair should preserve exact identity") && ok;
  ok = Expect(decision.row_uuid_preserved && decision.version_uuid_preserved,
              "exact repair should preserve row and version UUIDs") && ok;
  ok = Expect(decision.output_version_uuid.value ==
                  fixture.old_version_uuid.value,
              "exact repair output version UUID should match original") && ok;
  ok = Expect(!decision.repair_evidence_is_transaction_authority,
              "exact repair evidence must not become transaction authority") &&
       ok;

  auto changed_payload = BaseExactRequest(fixture);
  changed_payload.logical_payload_changed = true;
  const auto refused = page::EvaluateRepairIdentityRule(changed_payload);
  ok = Expect(!refused.ok(),
              "exact repair with logical payload change should fail closed") &&
       ok;
  ok = Expect(refused.diagnostic.diagnostic_code ==
                  "SB-REPAIR-IDENTITY-LOGICAL-CORRECTION-REQUIRED",
              "exact repair payload-change diagnostic should be stable") && ok;
  return ok;
}

bool PageRewriteRoundTripProof(const Fixture& fixture) {
  bool ok = true;
  page::RowDataPageBody body;
  body.relation_uuid = fixture.relation_uuid;
  body.segment_id = 1;
  body.segment_generation = 2;
  body.page_number = 55;
  body.page_generation = 3;
  body.compaction_generation = 3;
  body.rows.push_back(
      RowFor(fixture, fixture.creator_transaction_uuid, 10, 1));
  const auto built = page::BuildRowDataPageBody(body, kPageSize);
  ok = Expect(built.ok(), "row data page should build for rewrite proof") && ok;
  const auto parsed = page::ParseRowDataPageBody(built.serialized, 55);
  ok = Expect(parsed.ok(), "row data page should parse for rewrite proof") && ok;
  if (!ok) {
    return false;
  }

  auto request = BaseExactRequest(fixture);
  request.action = page::RepairIdentityAction::page_rewrite;
  request.candidate_row = parsed.body.rows.front();
  request.candidate_row.stable_slot_id = request.original_row.stable_slot_id;
  const auto accepted = page::EvaluateRepairIdentityRule(request);
  ok = Expect(accepted.ok(),
              "page rewrite should preserve row and version identities") && ok;

  auto changed_version = request;
  changed_version.candidate_version_uuid = fixture.new_version_uuid;
  const auto refused = page::EvaluateRepairIdentityRule(changed_version);
  ok = Expect(!refused.ok(),
              "page rewrite changing version UUID should fail closed") && ok;
  ok = Expect(refused.diagnostic.diagnostic_code ==
                  "SB-REPAIR-IDENTITY-EXACT-IDENTITY-CHANGED",
              "page rewrite identity-change diagnostic should be stable") && ok;
  return ok;
}

bool LogicalCorrectionProof(const Fixture& fixture) {
  bool ok = true;
  const auto accepted = page::EvaluateRepairIdentityRule(
      LogicalCorrectionRequest(fixture));
  ok = Expect(accepted.ok() && accepted.mutation_allowed,
              "logical correction should be admitted with payload proof") && ok;
  ok = Expect(accepted.logical_correction_created_new_version,
              "logical correction should create a new row version") && ok;
  ok = Expect(accepted.row_uuid_preserved && !accepted.version_uuid_preserved,
              "logical correction should preserve row UUID but use new version UUID") &&
       ok;
  ok = Expect(accepted.output_metadata.state ==
                  txn::RowVersionState::uncommitted,
              "logical correction output should be an uncommitted MGA version") &&
       ok;

  auto committed = LogicalCorrectionRequest(fixture);
  committed.candidate_metadata.state = txn::RowVersionState::committed;
  committed.candidate_metadata.creator_transaction_state =
      txn::TransactionState::committed;
  const auto refused = page::EvaluateRepairIdentityRule(committed);
  ok = Expect(!refused.ok(),
              "logical correction must not fabricate committed data") && ok;
  ok = Expect(refused.diagnostic.diagnostic_code ==
                  "SB-REPAIR-IDENTITY-NEW-VERSION-MUST-BE-ACTIVE",
              "logical correction committed-data diagnostic should be stable") &&
       ok;

  auto row_changed = LogicalCorrectionRequest(fixture);
  row_changed.candidate_row.row_uuid = MakeUuid(UuidKind::row, 20);
  row_changed.candidate_metadata.identity.row.row_uuid =
      row_changed.candidate_row.row_uuid;
  const auto row_refused = page::EvaluateRepairIdentityRule(row_changed);
  ok = Expect(!row_refused.ok(),
              "logical correction changing row UUID should fail closed") && ok;
  return ok;
}

bool SalvageRulesProof(const Fixture& fixture) {
  bool ok = true;
  auto review = BaseExactRequest(fixture);
  review.action = page::RepairIdentityAction::salvage_review;
  review.repair_event_persisted_before_mutation = false;
  review.repair_event_digest = 0;
  review.salvage_uncertain = true;
  review.salvage_restore_required = true;
  const auto review_decision = page::EvaluateRepairIdentityRule(review);
  ok = Expect(review_decision.ok() && !review_decision.mutation_allowed,
              "uncertain salvage review should remain non-mutating evidence") &&
       ok;
  ok = Expect(review_decision.salvage_remains_evidence &&
                  review_decision.restore_required,
              "uncertain salvage should require restore or review") && ok;

  auto no_proof = LogicalCorrectionRequest(fixture);
  no_proof.action = page::RepairIdentityAction::salvage_promote_with_authority;
  no_proof.salvage_uncertain = true;
  no_proof.salvage_payload_promoted_to_committed = true;
  no_proof.authoritative_payload_proof = false;
  const auto refused = page::EvaluateRepairIdentityRule(no_proof);
  ok = Expect(!refused.ok(),
              "salvage promotion without authority proof should fail closed") &&
       ok;
  ok = Expect(refused.diagnostic.diagnostic_code ==
                  "SB-REPAIR-IDENTITY-SALVAGE-PROOF-REQUIRED",
              "salvage proof diagnostic should be stable") && ok;

  auto with_proof = LogicalCorrectionRequest(fixture);
  with_proof.action =
      page::RepairIdentityAction::salvage_promote_with_authority;
  with_proof.salvage_uncertain = true;
  with_proof.salvage_restore_required = true;
  with_proof.salvage_payload_promoted_to_committed = true;
  const auto promoted = page::EvaluateRepairIdentityRule(with_proof);
  ok = Expect(promoted.ok() &&
                  promoted.logical_correction_created_new_version,
              "authorized salvage promotion should route through new MGA version") &&
       ok;
  ok = Expect(promoted.salvage_remains_evidence &&
                  !promoted.repair_evidence_is_transaction_authority,
              "authorized salvage evidence should still remain non-authority") &&
       ok;
  return ok;
}

bool AuthorityRefusalProof(const Fixture& fixture) {
  bool ok = true;
  auto drift = BaseExactRequest(fixture);
  drift.repair_evidence_is_transaction_authority = true;
  const auto refused = page::EvaluateRepairIdentityRule(drift);
  ok = Expect(!refused.ok(),
              "repair identity rules should reject repair authority drift") && ok;
  ok = Expect(refused.diagnostic.diagnostic_code ==
                  "SB-REPAIR-IDENTITY-AUTHORITY-REFUSED",
              "repair authority diagnostic should be stable") && ok;

  auto missing_event = BaseExactRequest(fixture);
  missing_event.repair_event_digest = 0;
  missing_event.repair_event_persisted_before_mutation = false;
  const auto event_refused = page::EvaluateRepairIdentityRule(missing_event);
  ok = Expect(!event_refused.ok(),
              "repair mutation should require a durable repair event") && ok;
  return ok;
}

}  // namespace

int main() {
  const Fixture fixture;
  bool ok = ExactRelocationProof(fixture);
  ok = PageRewriteRoundTripProof(fixture) && ok;
  ok = LogicalCorrectionProof(fixture) && ok;
  ok = SalvageRulesProof(fixture) && ok;
  ok = AuthorityRefusalProof(fixture) && ok;
  return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}

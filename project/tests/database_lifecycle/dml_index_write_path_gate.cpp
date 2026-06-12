// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "dml/dml_index_write_path.hpp"
#include "index_key_encoding.hpp"
#include "uuid.hpp"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace api = scratchbird::engine::internal_api;
namespace idx = scratchbird::core::index;
namespace page = scratchbird::storage::page;
namespace platform = scratchbird::core::platform;
namespace uuid = scratchbird::core::uuid;

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << "dml_index_write_path_gate: " << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view message) {
  if (!condition) {
    Fail(message);
  }
}

platform::TypedUuid GeneratedUuid(platform::UuidKind kind,
                                  platform::u64 millis,
                                  platform::byte suffix) {
  const auto generated = uuid::GenerateCompatibilityUnixTimeV7(millis);
  Require(generated.ok(), "uuidv7 generation failed");
  auto value = generated.value;
  value.bytes[15] = suffix;
  const auto typed = uuid::MakeTypedUuid(kind, value);
  Require(typed.ok(), "typed uuid creation failed");
  return typed.value;
}

std::string UuidText(platform::UuidKind kind,
                     platform::u64 millis,
                     platform::byte suffix) {
  return uuid::UuidToString(GeneratedUuid(kind, millis, suffix).value);
}

const std::string& TableUuid() {
  static const std::string uuid =
      UuidText(platform::UuidKind::object, 1700000999000ull, 0x30);
  return uuid;
}

std::vector<platform::byte> EncodedKey(const std::string& index_uuid,
                                       const std::string& key) {
  const auto descriptor_uuid =
      uuid::ParseDurableEngineIdentityUuid(platform::UuidKind::object,
                                           index_uuid);
  Require(descriptor_uuid.ok(), "index uuid parse for key encoding failed");
  idx::IndexKeyEncodingComponent component;
  component.kind = idx::IndexKeyComponentKind::scalar;
  component.ordinal = 0;
  component.type_descriptor_uuid = descriptor_uuid.value;
  component.payload.assign(key.begin(), key.end());
  const auto encoded = idx::EncodeIndexKey({component}, {});
  Require(encoded.ok(), "test key encoding failed");
  return encoded.encoded;
}

api::CrudIndexRecord Index(std::string uuid,
                           std::string family,
                           std::string column,
                           bool unique = false) {
  api::CrudIndexRecord index;
  index.creator_tx = 42;
  index.index_uuid = std::move(uuid);
  index.table_uuid = TableUuid();
  index.family = std::move(family);
  index.profile = api::kCrudIndexProfileRowStoreScalarBtreeV1;
  index.column_name = std::move(column);
  index.unique = unique;
  if (unique) {
    index.key_envelopes.push_back("unique");
  }
  return index;
}

api::DmlIndexWriteRowImage Row(std::string row_uuid,
                               std::string version_uuid,
                               std::string id,
                               std::string name,
                               std::string payload = "payload") {
  api::DmlIndexWriteRowImage row;
  row.row_uuid = std::move(row_uuid);
  row.version_uuid = std::move(version_uuid);
  row.values.push_back({"id", std::move(id)});
  row.values.push_back({"name", std::move(name)});
  row.values.push_back({"payload", std::move(payload)});
  return row;
}

api::DmlIndexWriteEvent BaseEvent(api::DmlIndexWriteOperation operation,
                                  const api::CrudIndexRecord& index) {
  api::DmlIndexWriteEvent event;
  event.operation = operation;
  event.index = index;
  event.table_uuid = TableUuid();
  event.transaction_uuid = UuidText(platform::UuidKind::transaction,
                                    1700001000000ull,
                                    0x31);
  event.local_transaction_id = 42;
  event.mga_transaction_identity_proof = true;
  event.mga_transaction_finality_authority_proof = true;
  event.rollback_evidence_token = "rollback-token-irc-050";
  event.index_descriptor_capability_proof = true;
  event.key_extraction_proof = true;
  event.partial_predicate_proof = true;
  event.covering_payload_proof = true;
  event.unique_preflight_proof = true;
  event.unique_reservation_preflight_proof = true;
  return event;
}

page::IndexBtreePhysicalTree MakeTree(const std::string& index_uuid) {
  const auto parsed =
      uuid::ParseDurableEngineIdentityUuid(platform::UuidKind::object,
                                           index_uuid);
  Require(parsed.ok(), "index uuid parse failed");
  auto initialized = page::InitializeIndexBtreePhysicalTree(parsed.value, 4096);
  Require(initialized.ok(), "physical btree init failed");
  return std::move(initialized.tree);
}

std::size_t CountKey(const page::IndexBtreePhysicalTree& tree,
                     const std::string& index_uuid,
                     const std::string& key) {
  const auto scan =
      page::PointLookupIndexBtreePhysicalTree(tree, EncodedKey(index_uuid, key));
  Require(scan.ok(), "point lookup failed");
  return scan.locators.size();
}

bool HasEvidence(const std::vector<api::EngineEvidenceReference>& evidence,
                 std::string_view kind,
                 std::string_view id) {
  return std::any_of(evidence.begin(),
                     evidence.end(),
                     [&](const auto& item) {
                       return item.evidence_kind == kind &&
                              item.evidence_id.find(id) != std::string::npos;
                     });
}

api::DmlIndexWritePathResult ApplyOne(api::DmlIndexWriteEvent event,
                                      page::IndexBtreePhysicalTree* tree) {
  api::DmlIndexWritePathRequest request;
  request.events.push_back(std::move(event));
  request.physical_trees.push_back({request.events.front().index.index_uuid, tree});
  return api::ApplyDmlIndexWritePath(request);
}

api::DmlIndexWritePathResult ApplyOneLedger(
    api::DmlIndexWriteEvent event,
    idx::PersistentSecondaryIndexDeltaLedger* ledger) {
  api::DmlIndexWritePathRequest request;
  request.events.push_back(std::move(event));
  request.secondary_delta_ledgers.push_back(
      {request.events.front().index.index_uuid, ledger, {}});
  request.deferred_secondary_index_options = {
      idx::kDeferredSecondaryIndexRuntimeOption,
      idx::kSecondaryIndexDeltaLedgerFeatureOption,
      idx::kDeltaLedgerReaderOverlayOption,
      idx::kDeltaLedgerCleanupHorizonBoundOption,
      idx::kDeltaLedgerRecoveryClassifiableOption};
  request.cleanup_horizon_token = "cleanup-horizon:irc-050";
  request.durable_mga_inventory_proof = true;
  request.delta_overlay_read_proof = true;
  request.recovery_classification_proof = true;
  request.unique_reservation_protocol_proof = true;
  request.unique_deferred_route_closure_proof = true;
  return api::ApplyDmlIndexWritePath(request);
}

void TestInsertUpdateDeleteMaintenance() {
  const std::string index_uuid =
      UuidText(platform::UuidKind::object, 1700002000000ull, 0x41);
  auto tree = MakeTree(index_uuid);
  const auto index = Index(index_uuid, api::kCrudIndexFamilyBtree, "name");
  const std::string row_uuid =
      UuidText(platform::UuidKind::row, 1700003000000ull, 0x51);
  const std::string v1 = UuidText(platform::UuidKind::row, 1700003001000ull, 0x52);
  const std::string v2 = UuidText(platform::UuidKind::row, 1700003002000ull, 0x53);

  auto insert = BaseEvent(api::DmlIndexWriteOperation::insert, index);
  insert.has_new_row = true;
  insert.new_row = Row(row_uuid, v1, "1", "alpha");
  auto result = ApplyOne(insert, &tree);
  Require(result.ok, "insert physical maintenance failed");
  Require(result.physical_inserts == 1, "insert count mismatch");
  Require(CountKey(tree, index_uuid, "alpha") == 1, "inserted key missing");

  auto update = BaseEvent(api::DmlIndexWriteOperation::update, index);
  update.has_old_row = true;
  update.old_row = Row(row_uuid, v1, "1", "alpha");
  update.has_new_row = true;
  update.new_row = Row(row_uuid, v2, "1", "bravo");
  result = ApplyOne(update, &tree);
  Require(result.ok, "update physical maintenance failed");
  Require(result.physical_deletes == 1 && result.physical_inserts == 1,
          "update did not delete old and insert new key");
  Require(CountKey(tree, index_uuid, "alpha") == 0,
          "old update key was not deleted");
  Require(CountKey(tree, index_uuid, "bravo") == 1, "new update key missing");

  auto unchanged = BaseEvent(api::DmlIndexWriteOperation::update, index);
  unchanged.has_old_row = true;
  unchanged.old_row = Row(row_uuid, v2, "1", "bravo");
  unchanged.has_new_row = true;
  unchanged.new_row = Row(row_uuid,
                          UuidText(platform::UuidKind::row,
                                   1700003003000ull,
                                   0x54),
                          "1",
                          "bravo",
                          "payload-changed");
  result = ApplyOne(unchanged, &tree);
  Require(result.ok, "unchanged update was refused");
  Require(result.unchanged_key_noops == 1, "unchanged-key no-op not recorded");
  Require(result.physical_deletes == 0 && result.physical_inserts == 0,
          "unchanged update performed index churn");
  Require(HasEvidence(result.evidence,
                      "dml_index_unchanged_key_noop",
                      index_uuid),
          "unchanged update evidence missing");

  auto delete_event = BaseEvent(api::DmlIndexWriteOperation::delete_row, index);
  delete_event.has_old_row = true;
  delete_event.old_row = Row(row_uuid, v2, "1", "bravo");
  result = ApplyOne(delete_event, &tree);
  Require(result.ok, "delete physical maintenance failed");
  Require(result.physical_deletes == 1, "delete count mismatch");
  Require(CountKey(tree, index_uuid, "bravo") == 0,
          "deleted key still visible");
}

void TestUniqueDuplicateRefusal() {
  const std::string index_uuid =
      UuidText(platform::UuidKind::object, 1700002100000ull, 0x61);
  auto tree = MakeTree(index_uuid);
  const auto index = Index(index_uuid, "unique_btree", "id", true);
  auto first = BaseEvent(api::DmlIndexWriteOperation::insert, index);
  first.has_new_row = true;
  first.new_row = Row(UuidText(platform::UuidKind::row, 1700003100000ull, 0x62),
                      UuidText(platform::UuidKind::row, 1700003101000ull, 0x63),
                      "1",
                      "alpha");
  auto result = ApplyOne(first, &tree);
  Require(result.ok, "unique seed insert failed");

  auto duplicate = BaseEvent(api::DmlIndexWriteOperation::insert, index);
  duplicate.has_new_row = true;
  duplicate.new_row = Row(UuidText(platform::UuidKind::row, 1700003102000ull, 0x64),
                          UuidText(platform::UuidKind::row, 1700003103000ull, 0x65),
                          "1",
                          "bravo");
  result = ApplyOne(duplicate, &tree);
  Require(!result.ok, "unique duplicate was admitted");
  Require(result.diagnostic.code == "SB-DML-INDEX-WRITE-UNIQUE-DUPLICATE",
          "unique duplicate diagnostic mismatch");
  Require(CountKey(tree, index_uuid, "1") == 1,
          "duplicate refusal changed physical unique tree");
}

void TestMergeBatchOrder() {
  const std::string index_uuid =
      UuidText(platform::UuidKind::object, 1700002200000ull, 0x71);
  auto tree = MakeTree(index_uuid);
  const auto index = Index(index_uuid, api::kCrudIndexFamilyBtree, "name");
  const std::string row_uuid =
      UuidText(platform::UuidKind::row, 1700003200000ull, 0x72);
  const std::string v1 = UuidText(platform::UuidKind::row, 1700003201000ull, 0x73);
  const std::string v2 = UuidText(platform::UuidKind::row, 1700003202000ull, 0x74);

  api::DmlIndexWritePathRequest request;
  auto insert = BaseEvent(api::DmlIndexWriteOperation::merge_insert, index);
  insert.source_ordinal = 0;
  insert.action_ordinal = 0;
  insert.has_new_row = true;
  insert.new_row = Row(row_uuid, v1, "1", "alpha");
  request.events.push_back(insert);

  auto update = BaseEvent(api::DmlIndexWriteOperation::merge_update, index);
  update.source_ordinal = 0;
  update.action_ordinal = 1;
  update.has_old_row = true;
  update.old_row = Row(row_uuid, v1, "1", "alpha");
  update.has_new_row = true;
  update.new_row = Row(row_uuid, v2, "1", "bravo");
  request.events.push_back(update);

  auto delete_event = BaseEvent(api::DmlIndexWriteOperation::merge_delete, index);
  delete_event.source_ordinal = 0;
  delete_event.action_ordinal = 2;
  delete_event.has_old_row = true;
  delete_event.old_row = Row(row_uuid, v2, "1", "bravo");
  request.events.push_back(delete_event);
  request.physical_trees.push_back({index_uuid, &tree});

  const auto result = api::ApplyDmlIndexWritePath(request);
  Require(result.ok, "merge write path batch failed");
  Require(result.merge_events == 3, "merge event count mismatch");
  Require(result.physical_inserts == 2 && result.physical_deletes == 2,
          "merge physical mutation count mismatch");
  Require(CountKey(tree, index_uuid, "alpha") == 0 &&
              CountKey(tree, index_uuid, "bravo") == 0,
          "merge delete did not remove final key");
  Require(HasEvidence(result.evidence,
                      "merge_index_write_source_action_order",
                      "0:merge_insert:0"),
          "merge insert order evidence missing");
  Require(HasEvidence(result.evidence,
                      "merge_index_write_source_action_order",
                      "0:merge_update:1"),
          "merge update order evidence missing");
  Require(HasEvidence(result.evidence,
                      "merge_index_write_source_action_order",
                      "0:merge_delete:2"),
          "merge delete order evidence missing");
}

void TestExpressionPartialCoveringProofsAndUnsupportedFamily() {
  const std::string expr_uuid =
      UuidText(platform::UuidKind::object, 1700002300000ull, 0x81);
  auto expr_tree = MakeTree(expr_uuid);
  auto expr = Index(expr_uuid, api::kCrudIndexFamilyExpression, "lower:name");
  auto expr_insert = BaseEvent(api::DmlIndexWriteOperation::insert, expr);
  expr_insert.has_new_row = true;
  expr_insert.new_row = Row(UuidText(platform::UuidKind::row, 1700003300000ull, 0x82),
                            UuidText(platform::UuidKind::row, 1700003301000ull, 0x83),
                            "1",
                            "MiXeD");
  auto result = ApplyOne(expr_insert, &expr_tree);
  Require(result.ok, "expression index insert with proof failed");
  Require(CountKey(expr_tree, expr_uuid, "mixed") == 1,
          "expression key missing");

  const std::string partial_uuid =
      UuidText(platform::UuidKind::object, 1700002301000ull, 0x84);
  auto partial_tree = MakeTree(partial_uuid);
  auto partial = Index(partial_uuid, api::kCrudIndexFamilyPartial, "name");
  partial.predicate_kind = "where_eq";
  partial.predicate_column = "payload";
  partial.predicate_value = "visible";
  auto partial_insert = BaseEvent(api::DmlIndexWriteOperation::insert, partial);
  partial_insert.has_new_row = true;
  partial_insert.new_row = Row(UuidText(platform::UuidKind::row, 1700003302000ull, 0x85),
                               UuidText(platform::UuidKind::row, 1700003303000ull, 0x86),
                               "2",
                               "partial",
                               "hidden");
  result = ApplyOne(partial_insert, &partial_tree);
  Require(result.ok, "partial excluded insert failed");
  Require(result.physical_inserts == 0, "partial excluded row was indexed");

  const std::string covering_uuid =
      UuidText(platform::UuidKind::object, 1700002302000ull, 0x87);
  auto covering_tree = MakeTree(covering_uuid);
  auto covering = Index(covering_uuid, api::kCrudIndexFamilyCovering, "name");
  covering.include_columns.push_back("payload");
  auto covering_insert = BaseEvent(api::DmlIndexWriteOperation::insert, covering);
  covering_insert.has_new_row = true;
  covering_insert.new_row = Row(UuidText(platform::UuidKind::row, 1700003304000ull, 0x88),
                                UuidText(platform::UuidKind::row, 1700003305000ull, 0x89),
                                "3",
                                "cover",
                                "payload");
  result = ApplyOne(covering_insert, &covering_tree);
  Require(result.ok, "covering index insert with payload proof failed");
  Require(CountKey(covering_tree, covering_uuid, "cover") == 1,
          "covering key missing");

  const std::string hash_uuid =
      UuidText(platform::UuidKind::object, 1700002303000ull, 0x8a);
  idx::PersistentSecondaryIndexDeltaLedger hash_ledger;
  auto hash = Index(hash_uuid, api::kCrudIndexFamilyHash, "name");
  auto hash_insert = BaseEvent(api::DmlIndexWriteOperation::insert, hash);
  hash_insert.has_new_row = true;
  hash_insert.new_row = Row(UuidText(platform::UuidKind::row, 1700003306000ull, 0x8b),
                            UuidText(platform::UuidKind::row, 1700003307000ull, 0x8c),
                            "4",
                            "hash");
  result = ApplyOneLedger(hash_insert, &hash_ledger);
  Require(result.ok && result.secondary_delta_ledger_appends == 1,
          "hash insert was not admitted to deferred DML ledger route");
  Require(hash_ledger.records.size() == 1 &&
              hash_ledger.records.back().delta.delta_kind ==
                  idx::SecondaryIndexDeltaKind::insert,
          "hash insert ledger record mismatch");
  Require(HasEvidence(result.evidence, "dml_index_route_capability", "complete"),
          "hash DML route capability evidence missing");

  auto hash_update = BaseEvent(api::DmlIndexWriteOperation::update, hash);
  hash_update.has_old_row = true;
  hash_update.old_row = hash_insert.new_row;
  hash_update.has_new_row = true;
  hash_update.new_row = Row(hash_insert.new_row.row_uuid,
                            UuidText(platform::UuidKind::row,
                                     1700003308000ull,
                                     0x8d),
                            "4",
                            "hash-next");
  result = ApplyOneLedger(hash_update, &hash_ledger);
  Require(result.ok && result.secondary_delta_ledger_appends == 2,
          "hash update did not append before/after ledger records");
  Require(hash_ledger.records.size() == 3 &&
              hash_ledger.records[1].delta.delta_kind ==
                  idx::SecondaryIndexDeltaKind::update_before &&
              hash_ledger.records[2].delta.delta_kind ==
                  idx::SecondaryIndexDeltaKind::update_after,
          "hash update ledger records mismatch");

  auto hash_delete = BaseEvent(api::DmlIndexWriteOperation::delete_row, hash);
  hash_delete.has_old_row = true;
  hash_delete.old_row = hash_update.new_row;
  result = ApplyOneLedger(hash_delete, &hash_ledger);
  Require(result.ok && result.secondary_delta_ledger_appends == 1,
          "hash delete did not append delete ledger record");
  Require(hash_ledger.records.size() == 4 &&
              hash_ledger.records.back().delta.delta_kind ==
                  idx::SecondaryIndexDeltaKind::delete_row,
          "hash delete ledger record mismatch");

  const std::string reference_uuid =
      UuidText(platform::UuidKind::object, 1700002304000ull, 0x8e);
  auto reference_tree = MakeTree(reference_uuid);
  auto unsupported = Index(reference_uuid, api::kCrudIndexFamilyReferenceEmulated, "name");
  auto reference_insert =
      BaseEvent(api::DmlIndexWriteOperation::insert, unsupported);
  reference_insert.has_new_row = true;
  reference_insert.new_row =
      Row(UuidText(platform::UuidKind::row, 1700003309000ull, 0x8f),
          UuidText(platform::UuidKind::row, 1700003310000ull, 0x90),
          "5",
          "reference");
  result = ApplyOne(reference_insert, &reference_tree);
  Require(!result.ok, "reference-emulated family was admitted to DML write route");
  Require(HasEvidence(result.evidence,
                      "dml_index_route_capability",
                      "refused"),
          "reference route-specific fail-closed evidence missing");
}

void TestRollbackAndNonAuthorityEvidence() {
  const std::string index_uuid =
      UuidText(platform::UuidKind::object, 1700002400000ull, 0x91);
  auto tree = MakeTree(index_uuid);
  const auto index = Index(index_uuid, api::kCrudIndexFamilyBtree, "name");
  auto event = BaseEvent(api::DmlIndexWriteOperation::insert, index);
  event.has_new_row = true;
  event.new_row = Row(UuidText(platform::UuidKind::row, 1700003400000ull, 0x92),
                      UuidText(platform::UuidKind::row, 1700003401000ull, 0x93),
                      "1",
                      "evidence");
  auto result = ApplyOne(event, &tree);
  Require(result.ok, "evidence insert failed");
  Require(HasEvidence(result.evidence,
                      "dml_index_write_rollback_safe",
                      "rollback-token-irc-050"),
          "rollback-safe evidence missing");
  Require(HasEvidence(result.evidence,
                      "mga_finality_authority",
                      "engine_transaction_inventory"),
          "MGA finality authority evidence missing");
  Require(HasEvidence(result.evidence,
                      "parser_or_reference_authority",
                      "false"),
          "parser/reference non-authority evidence missing");
  Require(HasEvidence(result.evidence,
                      "tree_mutation_finality_authority",
                      "false"),
          "tree mutation non-finality evidence missing");
  Require(HasEvidence(result.evidence,
                      "index_runtime_route_available",
                      "false"),
          "runtime route false evidence missing");
  Require(HasEvidence(result.evidence,
                      "index_benchmark_clean",
                      "false"),
          "benchmark-clean false evidence missing");
  Require(HasEvidence(result.evidence,
                      "dml_index_write_copy_on_success",
                      "true"),
          "copy-on-success rollback safety evidence missing");
}

void TestBatchFailureDoesNotCommitStagedMutations() {
  const std::string first_uuid =
      UuidText(platform::UuidKind::object, 1700002500000ull, 0xa1);
  const std::string second_uuid =
      UuidText(platform::UuidKind::object, 1700002501000ull, 0xa2);
  auto first_tree = MakeTree(first_uuid);
  auto second_tree = MakeTree(second_uuid);
  const auto first_index = Index(first_uuid, api::kCrudIndexFamilyBtree, "name");
  const auto second_index = Index(second_uuid, api::kCrudIndexFamilyBtree, "name");

  api::DmlIndexWritePathRequest request;
  auto insert = BaseEvent(api::DmlIndexWriteOperation::insert, first_index);
  insert.has_new_row = true;
  insert.new_row = Row(UuidText(platform::UuidKind::row, 1700003500000ull, 0xa3),
                       UuidText(platform::UuidKind::row, 1700003501000ull, 0xa4),
                       "1",
                       "staged");
  request.events.push_back(insert);

  auto missing_delete =
      BaseEvent(api::DmlIndexWriteOperation::delete_row, second_index);
  missing_delete.has_old_row = true;
  missing_delete.old_row =
      Row(UuidText(platform::UuidKind::row, 1700003502000ull, 0xa5),
          UuidText(platform::UuidKind::row, 1700003503000ull, 0xa6),
          "2",
          "missing");
  request.events.push_back(missing_delete);
  request.physical_trees.push_back({first_uuid, &first_tree});
  request.physical_trees.push_back({second_uuid, &second_tree});

  const auto result = api::ApplyDmlIndexWritePath(request);
  Require(!result.ok, "batch with missing delete was admitted");
  Require(CountKey(first_tree, first_uuid, "staged") == 0,
          "failed batch committed earlier staged insert");
}

}  // namespace

int main() {
  TestInsertUpdateDeleteMaintenance();
  TestUniqueDuplicateRefusal();
  TestMergeBatchOrder();
  TestExpressionPartialCoveringProofsAndUnsupportedFamily();
  TestRollbackAndNonAuthorityEvidence();
  TestBatchFailureDoesNotCommitStagedMutations();
  return EXIT_SUCCESS;
}

// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "dml/dml_index_write_path.hpp"
#include "index_key_encoding.hpp"
#include "secondary_index_delta_overlay.hpp"
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
  std::cerr << "dml_hot_delta_ledger_gate: " << message << '\n';
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

api::CrudIndexRecord Index(std::string index_uuid,
                           std::string table_uuid,
                           bool unique = false) {
  api::CrudIndexRecord index;
  index.creator_tx = 42;
  index.index_uuid = std::move(index_uuid);
  index.table_uuid = std::move(table_uuid);
  index.family = unique ? "unique_btree" : api::kCrudIndexFamilyBtree;
  index.profile = api::kCrudIndexProfileRowStoreScalarBtreeV1;
  index.column_name = unique ? "id" : "name";
  index.unique = unique;
  if (unique) {
    index.key_envelopes.push_back("unique");
  }
  return index;
}

api::CrudIndexRecord FullTextIndex(std::string index_uuid,
                                    std::string table_uuid) {
  auto index = Index(std::move(index_uuid), std::move(table_uuid));
  index.family = api::kCrudIndexFamilyFullText;
  index.profile = "native_full_text";
  index.column_name = "name";
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
                                  const api::CrudIndexRecord& index,
                                  const std::string& table_uuid) {
  api::DmlIndexWriteEvent event;
  event.operation = operation;
  event.index = index;
  event.table_uuid = table_uuid;
  event.transaction_uuid = UuidText(platform::UuidKind::transaction,
                                    1700100000000ull,
                                    0x31);
  event.local_transaction_id = 77;
  event.mga_transaction_identity_proof = true;
  event.mga_transaction_finality_authority_proof = true;
  event.rollback_evidence_token = "rollback-token-irc-051";
  event.index_descriptor_capability_proof = true;
  event.key_extraction_proof = true;
  event.partial_predicate_proof = true;
  event.covering_payload_proof = true;
  event.unique_preflight_proof = true;
  event.unique_reservation_preflight_proof = true;
  return event;
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

std::vector<std::string> DeferredOptions() {
  return {idx::kDeferredSecondaryIndexRuntimeOption,
          idx::kSecondaryIndexDeltaLedgerFeatureOption,
          idx::kDeltaLedgerReaderOverlayOption,
          idx::kDeltaLedgerCleanupHorizonBoundOption,
          idx::kDeltaLedgerRecoveryClassifiableOption};
}

api::DmlIndexWritePathResult Apply(api::DmlIndexWriteEvent event,
                                   page::IndexBtreePhysicalTree* tree = nullptr,
                                   idx::PersistentSecondaryIndexDeltaLedger* ledger = nullptr,
                                   bool deferred = false,
                                   bool cleanup_horizon = true,
                                   bool durable_mga = true,
                                   bool overlay = true,
                                   bool recovery = true) {
  api::DmlIndexWritePathRequest request;
  request.events.push_back(std::move(event));
  if (tree != nullptr) {
    request.physical_trees.push_back(
        {request.events.front().index.index_uuid, tree});
  }
  if (ledger != nullptr) {
    request.secondary_delta_ledgers.push_back(
        {request.events.front().index.index_uuid, ledger, {}});
  }
  if (deferred) {
    request.deferred_secondary_index_options = DeferredOptions();
  }
  if (cleanup_horizon) {
    request.cleanup_horizon_token = "cleanup-horizon-irc-051";
  }
  request.durable_mga_inventory_proof = durable_mga;
  request.delta_overlay_read_proof = overlay;
  request.recovery_classification_proof = recovery;
  return api::ApplyDmlIndexWritePath(request);
}

void SeedOldKey(page::IndexBtreePhysicalTree* tree,
                const api::CrudIndexRecord& index,
                const std::string& table_uuid,
                const std::string& row_uuid,
                const std::string& version_uuid,
                const std::string& key) {
  auto insert = BaseEvent(api::DmlIndexWriteOperation::insert, index, table_uuid);
  insert.has_new_row = true;
  insert.new_row = Row(row_uuid, version_uuid, "1", key);
  const auto result = Apply(std::move(insert), tree);
  Require(result.ok, "seed insert failed");
}

void TestHotLikeUnchangedUpdate() {
  const std::string table_uuid =
      UuidText(platform::UuidKind::object, 1700101000000ull, 0x41);
  const std::string index_uuid =
      UuidText(platform::UuidKind::object, 1700101001000ull, 0x42);
  const auto index = Index(index_uuid, table_uuid);
  auto tree = MakeTree(index_uuid);
  const std::string row_uuid =
      UuidText(platform::UuidKind::row, 1700101002000ull, 0x43);
  const std::string v1 = UuidText(platform::UuidKind::row, 1700101003000ull, 0x44);
  const std::string v2 = UuidText(platform::UuidKind::row, 1700101004000ull, 0x45);
  SeedOldKey(&tree, index, table_uuid, row_uuid, v1, "alpha");

  auto update = BaseEvent(api::DmlIndexWriteOperation::update, index, table_uuid);
  update.has_old_row = true;
  update.old_row = Row(row_uuid, v1, "1", "alpha");
  update.has_new_row = true;
  update.new_row = Row(row_uuid, v2, "1", "alpha", "payload-changed");
  const auto result = Apply(std::move(update), &tree);
  Require(result.ok, "HOT-like unchanged update failed");
  Require(result.hot_like_version_appends == 1, "HOT-like append count mismatch");
  Require(result.physical_inserts == 0 && result.physical_deletes == 0,
          "HOT-like update churned the physical index");
  Require(CountKey(tree, index_uuid, "alpha") == 1,
          "old index locator was not preserved");
  Require(HasEvidence(result.evidence, "hot_like_version_append_selected", "true"),
          "HOT-like selected evidence missing");
  Require(HasEvidence(result.evidence, "unchanged_index_churn_avoided", "true"),
          "unchanged churn avoided evidence missing");
  Require(HasEvidence(result.evidence,
                      "old_index_locator_remains_valid_through_mga_chain",
                      index_uuid),
          "MGA-chain locator evidence missing");
  Require(HasEvidence(result.evidence,
                      "durable_mga_inventory_remains_authority",
                      "true"),
          "MGA authority evidence missing");
  Require(HasEvidence(result.evidence, "transaction_finality_authority", "false"),
          "non-finality evidence missing");
}

void TestUpdateRowVersionContinuityFailClosed() {
  const std::string table_uuid =
      UuidText(platform::UuidKind::object, 1700101500000ull, 0x46);
  const std::string index_uuid =
      UuidText(platform::UuidKind::object, 1700101501000ull, 0x47);
  const auto index = Index(index_uuid, table_uuid);
  auto tree = MakeTree(index_uuid);
  const std::string row_uuid =
      UuidText(platform::UuidKind::row, 1700101502000ull, 0x48);
  const std::string other_row_uuid =
      UuidText(platform::UuidKind::row, 1700101503000ull, 0x49);
  const std::string v1 = UuidText(platform::UuidKind::row, 1700101504000ull, 0x4a);
  const std::string v2 = UuidText(platform::UuidKind::row, 1700101505000ull, 0x4b);
  SeedOldKey(&tree, index, table_uuid, row_uuid, v1, "alpha");

  auto row_mismatch = BaseEvent(api::DmlIndexWriteOperation::update,
                                index,
                                table_uuid);
  row_mismatch.has_old_row = true;
  row_mismatch.old_row = Row(row_uuid, v1, "1", "alpha");
  row_mismatch.has_new_row = true;
  row_mismatch.new_row = Row(other_row_uuid, v2, "1", "alpha");
  auto result = Apply(std::move(row_mismatch), &tree);
  Require(!result.ok, "row-uuid mismatched update was admitted");
  Require(result.diagnostic.code ==
              "SB-DML-INDEX-WRITE-UPDATE-ROW-UUID-MISMATCH",
          "row-uuid mismatch diagnostic mismatch");
  Require(CountKey(tree, index_uuid, "alpha") == 1,
          "row-uuid mismatch changed physical index state");

  auto version_unchanged = BaseEvent(api::DmlIndexWriteOperation::update,
                                     index,
                                     table_uuid);
  version_unchanged.has_old_row = true;
  version_unchanged.old_row = Row(row_uuid, v1, "1", "alpha");
  version_unchanged.has_new_row = true;
  version_unchanged.new_row = Row(row_uuid, v1, "1", "alpha");
  result = Apply(std::move(version_unchanged), &tree);
  Require(!result.ok, "same-version update was admitted");
  Require(result.diagnostic.code ==
              "SB-DML-INDEX-WRITE-UPDATE-VERSION-UUID-UNCHANGED",
          "same-version diagnostic mismatch");
  Require(CountKey(tree, index_uuid, "alpha") == 1,
          "same-version update changed physical index state");
}

void TestChangedNonUniqueDeferredDeltaOverlay() {
  const std::string table_uuid =
      UuidText(platform::UuidKind::object, 1700102000000ull, 0x51);
  const std::string index_uuid =
      UuidText(platform::UuidKind::object, 1700102001000ull, 0x52);
  const auto index = FullTextIndex(index_uuid, table_uuid);
  const std::string row_uuid =
      UuidText(platform::UuidKind::row, 1700102002000ull, 0x53);
  const std::string v1 = UuidText(platform::UuidKind::row, 1700102003000ull, 0x54);
  const std::string v2 = UuidText(platform::UuidKind::row, 1700102004000ull, 0x55);
  idx::PersistentSecondaryIndexDeltaLedger ledger;

  auto update = BaseEvent(api::DmlIndexWriteOperation::update, index, table_uuid);
  update.has_old_row = true;
  update.old_row = Row(row_uuid, v1, "1", "alpha");
  update.has_new_row = true;
  update.new_row = Row(row_uuid, v2, "1", "bravo");
  const auto result = Apply(std::move(update), nullptr, &ledger, true);
  Require(result.ok, "deferred non-unique delta update failed");
  Require(result.physical_inserts == 0 && result.physical_deletes == 0,
          "deferred update performed physical churn");
  Require(result.secondary_delta_ledger_appends == 2,
          "deferred update did not append before/after deltas");
  Require(result.secondary_delta_overlay_reads == 1,
          "deferred update did not exercise overlay read");
  Require(ledger.records.size() == 2, "ledger record count mismatch");
  Require(ledger.records[0].delta.delta_kind == idx::SecondaryIndexDeltaKind::update_before,
          "first delta was not update_before");
  Require(ledger.records[1].delta.delta_kind == idx::SecondaryIndexDeltaKind::update_after,
          "second delta was not update_after");
  Require(ledger.records[0].delta.cleanup_horizon_token ==
              "cleanup-horizon-irc-051",
          "cleanup horizon was not bound to before delta");
  Require(HasEvidence(result.evidence, "secondary_delta_ledger_appended", "true"),
          "delta append evidence missing");
  Require(HasEvidence(result.evidence, "delta_overlay_read_safe", "true"),
          "overlay read-safe evidence missing");
  Require(HasEvidence(result.evidence, "cleanup_horizon_bound", "true"),
          "cleanup horizon evidence missing");
  Require(HasEvidence(result.evidence, "runtime_route_capability", "true"),
          "runtime route capability evidence missing");
  Require(HasEvidence(result.evidence, "benchmark_clean", "true"),
          "benchmark clean evidence missing");
}

void TestUniqueOrderedDeferredFallsBackToPhysicalTree() {
  const std::string table_uuid =
      UuidText(platform::UuidKind::object, 1700103000000ull, 0x61);
  const std::string index_uuid =
      UuidText(platform::UuidKind::object, 1700103001000ull, 0x62);
  const auto index = Index(index_uuid, table_uuid, true);
  auto update = BaseEvent(api::DmlIndexWriteOperation::update, index, table_uuid);
  const std::string row_uuid =
      UuidText(platform::UuidKind::row, 1700103002000ull, 0x63);
  update.has_old_row = true;
  update.old_row = Row(row_uuid,
                       UuidText(platform::UuidKind::row, 1700103003000ull, 0x64),
                       "1",
                       "alpha");
  update.has_new_row = true;
  update.new_row = Row(row_uuid,
                       UuidText(platform::UuidKind::row, 1700103004000ull, 0x65),
                       "2",
                       "alpha");
  idx::PersistentSecondaryIndexDeltaLedger ledger;
  const auto result = Apply(std::move(update), nullptr, &ledger, true);
  Require(!result.ok, "unique ordered deferred update was admitted without tree");
  Require(result.diagnostic.code ==
              "SB-DML-INDEX-WRITE-PHYSICAL-TREE-REQUIRED",
          "unique ordered physical-tree diagnostic mismatch");
  Require(HasEvidence(result.evidence,
                      "dml_update_synchronous_fallback_reason",
                      "unique_ordered_index_synchronous_rewrite"),
          "unique ordered synchronous fallback evidence missing");
  Require(ledger.records.empty(), "unique ordered fallback appended deltas");
}

void TestDeferredProofDiagnostics() {
  api::DmlUpdateIndexMaintenanceRequest request;
  request.indexed_keys_changed = true;
  request.family = api::kCrudIndexFamilyFullText;
  request.option_envelopes = DeferredOptions();
  request.cleanup_horizon_present = false;
  request.durable_mga_inventory_proof = true;
  request.delta_overlay_read_proof = true;
  request.recovery_classification_proof = true;
  auto decision = api::DecideDmlUpdateIndexMaintenance(request);
  Require(!decision.ok &&
              decision.diagnostic.code ==
                  "SB-DML-HOT-DELTA-MISSING-CLEANUP-HORIZON",
          "missing cleanup horizon diagnostic mismatch");

  request.cleanup_horizon_present = true;
  request.durable_mga_inventory_proof = false;
  decision = api::DecideDmlUpdateIndexMaintenance(request);
  Require(!decision.ok &&
              decision.diagnostic.code ==
                  "SB-DML-HOT-DELTA-MISSING-DURABLE-MGA-PROOF",
          "missing durable MGA proof diagnostic mismatch");

  request.durable_mga_inventory_proof = true;
  request.delta_overlay_read_proof = false;
  decision = api::DecideDmlUpdateIndexMaintenance(request);
  Require(!decision.ok &&
              decision.diagnostic.code ==
                  "SB-DML-HOT-DELTA-MISSING-OVERLAY-PROOF",
          "missing overlay proof diagnostic mismatch");

  request.delta_overlay_read_proof = true;
  request.recovery_classification_proof = false;
  decision = api::DecideDmlUpdateIndexMaintenance(request);
  Require(!decision.ok &&
              decision.diagnostic.code ==
                  "SB-DML-HOT-DELTA-MISSING-RECOVERY-CLASSIFICATION-PROOF",
          "missing recovery classification proof diagnostic mismatch");
}

void TestSynchronousFallbackWhenPolicyDisabled() {
  const std::string table_uuid =
      UuidText(platform::UuidKind::object, 1700104000000ull, 0x71);
  const std::string index_uuid =
      UuidText(platform::UuidKind::object, 1700104001000ull, 0x72);
  const auto index = Index(index_uuid, table_uuid);
  auto tree = MakeTree(index_uuid);
  const std::string row_uuid =
      UuidText(platform::UuidKind::row, 1700104002000ull, 0x73);
  const std::string v1 = UuidText(platform::UuidKind::row, 1700104003000ull, 0x74);
  const std::string v2 = UuidText(platform::UuidKind::row, 1700104004000ull, 0x75);
  SeedOldKey(&tree, index, table_uuid, row_uuid, v1, "alpha");

  auto update = BaseEvent(api::DmlIndexWriteOperation::update, index, table_uuid);
  update.has_old_row = true;
  update.old_row = Row(row_uuid, v1, "1", "alpha");
  update.has_new_row = true;
  update.new_row = Row(row_uuid, v2, "1", "bravo");
  const auto result = Apply(std::move(update), &tree);
  Require(result.ok, "synchronous fallback update failed");
  Require(result.physical_deletes == 1 && result.physical_inserts == 1,
          "policy-disabled fallback did not rewrite synchronously");
  Require(CountKey(tree, index_uuid, "alpha") == 0,
          "synchronous fallback left old key");
  Require(CountKey(tree, index_uuid, "bravo") == 1,
          "synchronous fallback did not insert new key");
  Require(HasEvidence(result.evidence,
                      "dml_update_synchronous_fallback_reason",
                      "runtime_deferred_secondary_index_disabled"),
          "synchronous fallback reason missing");
}

}  // namespace

int main() {
  TestHotLikeUnchangedUpdate();
  TestUpdateRowVersionContinuityFailClosed();
  TestChangedNonUniqueDeferredDeltaOverlay();
  TestUniqueOrderedDeferredFallsBackToPhysicalTree();
  TestDeferredProofDiagnostics();
  TestSynchronousFallbackWhenPolicyDisabled();
  return EXIT_SUCCESS;
}

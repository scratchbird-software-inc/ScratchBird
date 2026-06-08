// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "database_lifecycle.hpp"
#include "dml/delete_api.hpp"
#include "dml/insert_api.hpp"
#include "dml/update_api.hpp"
#include "mga_relation_store/mga_relation_store.hpp"
#include "secondary_index_delta_ledger.hpp"
#include "transaction/transaction_api.hpp"
#include "uuid.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace api = scratchbird::engine::internal_api;
namespace db = scratchbird::storage::database;
namespace idx = scratchbird::core::index;
namespace platform = scratchbird::core::platform;
namespace uuid = scratchbird::core::uuid;

constexpr std::string_view kWritePathSearchKey = "DPC_DEFERRED_INDEX_WRITE_PATH";
constexpr std::string_view kGateSearchKey = "DPC_SECONDARY_INDEX_DELTA_WRITE_PATH_GATE";

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view message) {
  if (!condition) {
    Fail(message);
  }
}

template <typename TResult>
void RequireOk(const TResult& result, std::string_view message) {
  if (!result.ok) {
    if (!result.diagnostics.empty()) {
      std::cerr << result.diagnostics.front().code << ':'
                << result.diagnostics.front().detail << '\n';
    }
    Fail(message);
  }
}

void RequireDiagnosticOk(const api::EngineApiDiagnostic& diagnostic,
                         std::string_view message) {
  if (diagnostic.error) {
    std::cerr << diagnostic.code << ':' << diagnostic.detail << '\n';
    Fail(message);
  }
}

platform::u64 NowMillis() {
  return static_cast<platform::u64>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());
}

platform::TypedUuid NewUuid(platform::UuidKind kind, platform::u64 salt) {
  const auto generated = uuid::GenerateEngineIdentityV7(kind, NowMillis() + salt);
  Require(generated.ok(), "DPC-022 generated UUID creation failed");
  return generated.value;
}

std::string NewUuidText(platform::UuidKind kind, platform::u64 salt) {
  return uuid::UuidToString(NewUuid(kind, salt).value);
}

api::EngineTypedValue TextValue(std::string value) {
  api::EngineTypedValue typed;
  typed.descriptor.descriptor_kind = "scalar";
  typed.descriptor.canonical_type_name = "character";
  typed.descriptor.encoded_descriptor = "canonical=character";
  typed.encoded_value = std::move(value);
  return typed;
}

api::EngineRowValue Row(std::string id, std::string name, std::string note = "note") {
  api::EngineRowValue row;
  row.fields.push_back({"id", TextValue(std::move(id))});
  row.fields.push_back({"name", TextValue(std::move(name))});
  row.fields.push_back({"note", TextValue(std::move(note))});
  return row;
}

std::vector<std::string> DeferredOptions() {
  return {"runtime.deferred_secondary_index=enabled",
          "feature.secondary_index_delta_ledger=enabled",
          "delta_ledger.reader_overlay=enabled",
          "delta_ledger.cleanup_horizon_bound=true",
          "delta_ledger.recovery_classifiable=true"};
}

std::vector<std::string> ProofOnlyOptions() {
  return {"feature.secondary_index_delta_ledger=enabled",
          "delta_ledger.reader_overlay=enabled",
          "delta_ledger.cleanup_horizon_bound=true",
          "delta_ledger.recovery_classifiable=true"};
}

struct Fixture {
  std::filesystem::path dir;
  std::filesystem::path database_path;
  std::string database_uuid;
  std::string table_uuid;
  std::string non_unique_index_uuid;
  std::string unique_index_uuid;
  platform::u64 salt = 0;

  ~Fixture() {
    std::error_code ignored;
    if (!dir.empty()) {
      std::filesystem::remove_all(dir, ignored);
    }
  }
};

api::EngineRequestContext BaseContext(const Fixture& fixture, std::string request_id) {
  api::EngineRequestContext context;
  context.trust_mode = api::EngineTrustMode::server_isolated;
  context.request_id = std::move(request_id);
  context.database_path = fixture.database_path.string();
  context.database_uuid.canonical = fixture.database_uuid;
  context.principal_uuid.canonical = NewUuidText(platform::UuidKind::principal, fixture.salt + 100);
  context.session_uuid.canonical = NewUuidText(platform::UuidKind::object, fixture.salt + 101);
  context.security_context_present = true;
  context.identifier_profile_uuid = "sbsql_v3";
  context.language_context.language_tag = "en";
  context.language_context.default_language_tag = "en";
  context.catalog_generation_id = 1;
  context.security_epoch = 1;
  context.resource_epoch = 1;
  context.name_resolution_epoch = 1;
  return context;
}

api::EngineRequestContext Begin(const Fixture& fixture, std::string request_id) {
  api::EngineBeginTransactionRequest request;
  request.context = BaseContext(fixture, std::move(request_id));
  request.isolation_level = "read_committed";
  const auto begun = api::EngineBeginTransaction(request);
  RequireOk(begun, "DPC-022 begin transaction failed");
  auto context = request.context;
  context.local_transaction_id = begun.local_transaction_id;
  context.transaction_uuid = begun.transaction_uuid;
  context.snapshot_visible_through_local_transaction_id =
      begun.snapshot_visible_through_local_transaction_id;
  context.transaction_isolation_level = begun.isolation_level;
  return context;
}

void Commit(const api::EngineRequestContext& context) {
  api::EngineCommitTransactionRequest request;
  request.context = context;
  RequireOk(api::EngineCommitTransaction(request), "DPC-022 commit failed");
}

void Rollback(const api::EngineRequestContext& context) {
  api::EngineRollbackTransactionRequest request;
  request.context = context;
  RequireOk(api::EngineRollbackTransaction(request), "DPC-022 rollback failed");
}

api::CrudTableRecord Table(const Fixture& fixture,
                           const api::EngineRequestContext& context) {
  api::CrudTableRecord table;
  table.creator_tx = context.local_transaction_id;
  table.table_uuid = fixture.table_uuid;
  table.default_name = "dpc022_delta_write_path";
  table.columns.push_back({"id", "canonical=character"});
  table.columns.push_back({"name", "canonical=character"});
  table.columns.push_back({"note", "canonical=character"});
  return table;
}

api::CrudIndexRecord Index(const Fixture& fixture,
                           const api::EngineRequestContext& context,
                           std::string index_uuid,
                           std::string column,
                           bool unique) {
  api::CrudIndexRecord index;
  index.creator_tx = context.local_transaction_id;
  index.index_uuid = std::move(index_uuid);
  index.table_uuid = fixture.table_uuid;
  index.column_name = std::move(column);
  index.family = api::kCrudIndexFamilyBtree;
  index.profile = api::kCrudIndexProfileRowStoreScalarBtreeV1;
  index.unique = unique;
  index.key_envelopes.push_back(index.column_name);
  if (unique) {
    index.key_envelopes.push_back("unique");
  }
  return index;
}

Fixture MakeFixture(std::string name, platform::u64 salt) {
  Fixture fixture;
  fixture.salt = salt;
  fixture.dir = std::filesystem::temp_directory_path() /
                ("scratchbird_dpc022_" + name + "_" + std::to_string(NowMillis() + salt));
  std::filesystem::create_directories(fixture.dir);
  fixture.database_path = fixture.dir / "dpc022.sbdb";

  db::DatabaseCreateConfig create;
  create.path = fixture.database_path.string();
  create.database_uuid = NewUuid(platform::UuidKind::database, salt + 1);
  create.filespace_uuid = NewUuid(platform::UuidKind::filespace, salt + 2);
  create.creation_unix_epoch_millis = NowMillis() + salt + 3;
  create.require_resource_seed_pack = false;
  create.allow_minimal_resource_bootstrap = true;
  create.allow_overwrite = true;
  const auto created = db::CreateDatabaseFile(create);
  if (!created.ok()) {
    std::cerr << created.diagnostic.diagnostic_code << ':'
              << created.diagnostic.message_key << '\n';
  }
  Require(created.ok(), "DPC-022 database create failed");

  fixture.database_uuid = uuid::UuidToString(create.database_uuid.value);
  fixture.table_uuid = NewUuidText(platform::UuidKind::object, salt + 10);
  fixture.non_unique_index_uuid = NewUuidText(platform::UuidKind::object, salt + 11);
  fixture.unique_index_uuid = NewUuidText(platform::UuidKind::object, salt + 12);

  auto context = Begin(fixture, "dpc022-metadata");
  RequireDiagnosticOk(api::AppendMgaTableMetadata(context, Table(fixture, context)),
                      "DPC-022 table metadata append failed");
  RequireDiagnosticOk(api::AppendMgaIndexMetadata(
                          context,
                          Index(fixture, context, fixture.non_unique_index_uuid, "name", false)),
                      "DPC-022 non-unique index metadata append failed");
  RequireDiagnosticOk(api::AppendMgaIndexMetadata(
                          context,
                          Index(fixture, context, fixture.unique_index_uuid, "id", true)),
                      "DPC-022 unique index metadata append failed");
  Commit(context);
  return fixture;
}

api::EngineInsertRowsResult InsertRow(const Fixture& fixture,
                                      const api::EngineRequestContext& context,
                                      std::string id,
                                      std::string name,
                                      std::vector<std::string> options = {}) {
  api::EngineInsertRowsRequest request;
  request.context = context;
  request.target_table.uuid.canonical = fixture.table_uuid;
  request.target_table.object_kind = "table";
  request.input_rows.push_back(Row(std::move(id), std::move(name)));
  request.estimated_row_count = 1;
  request.option_envelopes = std::move(options);
  return api::EngineInsertRows(request);
}

api::EngineUpdateRowsResult UpdateName(const Fixture& fixture,
                                       const api::EngineRequestContext& context,
                                       std::string id,
                                       std::string name,
                                       std::vector<std::string> options = {}) {
  api::EngineUpdateRowsRequest request;
  request.context = context;
  request.target_table.uuid.canonical = fixture.table_uuid;
  request.target_table.object_kind = "table";
  request.update_predicate.predicate_kind = "column_equals";
  request.update_predicate.canonical_predicate_envelope = "id";
  request.update_predicate.bound_values.push_back(TextValue(std::move(id)));
  request.assignments.push_back({"name", TextValue(std::move(name))});
  request.option_envelopes = std::move(options);
  return api::EngineUpdateRows(request);
}

api::EngineDeleteRowsResult DeleteRow(const Fixture& fixture,
                                      const api::EngineRequestContext& context,
                                      std::string row_uuid,
                                      std::vector<std::string> options = {}) {
  api::EngineDeleteRowsRequest request;
  request.context = context;
  request.target_table.uuid.canonical = fixture.table_uuid;
  request.target_table.object_kind = "table";
  request.delete_predicate.predicate_kind = "row_uuid_match";
  request.delete_predicate.canonical_predicate_envelope = std::move(row_uuid);
  request.tombstone_only = true;
  request.option_envelopes = std::move(options);
  return api::EngineDeleteRows(request);
}

idx::PersistentSecondaryIndexDeltaLedger LoadLedger(const Fixture& fixture) {
  const auto loaded = api::LoadMgaSecondaryIndexDeltaLedger(BaseContext(fixture, "dpc022-ledger-load"));
  if (!loaded.ok) {
    std::cerr << loaded.diagnostic.code << ':' << loaded.diagnostic.detail << '\n';
  }
  Require(loaded.ok, "DPC-022 ledger load failed");
  return loaded.ledger;
}

std::vector<idx::SecondaryIndexDeltaLedgerRecord> RecordsForTx(
    const idx::PersistentSecondaryIndexDeltaLedger& ledger,
    platform::u64 local_transaction_id) {
  std::vector<idx::SecondaryIndexDeltaLedgerRecord> records;
  for (const auto& record : ledger.records) {
    if (record.delta.local_transaction_id == local_transaction_id) {
      records.push_back(record);
    }
  }
  return records;
}

std::string RecordIndexUuid(const idx::SecondaryIndexDeltaLedgerRecord& record) {
  return uuid::UuidToString(record.delta.index_uuid.value);
}

std::string PayloadField(const idx::SecondaryIndexDeltaLedgerRecord& record,
                         const std::string& field) {
  for (const auto& [key, value] : api::DecodeCrudPairs(record.delta.key_payload)) {
    if (key == field) {
      return value;
    }
  }
  return {};
}

std::size_t CountIndexEntries(const Fixture& fixture,
                              const api::EngineRequestContext& context,
                              const std::string& index_uuid) {
  const auto loaded = api::LoadMgaRelationStoreState(context);
  if (!loaded.ok) {
    std::cerr << loaded.diagnostic.code << ':' << loaded.diagnostic.detail << '\n';
  }
  Require(loaded.ok, "DPC-022 relation store load failed");
  std::size_t count = 0;
  for (const auto& entry : loaded.state.index_entries) {
    if (entry.table_uuid == fixture.table_uuid && entry.index_uuid == index_uuid) {
      ++count;
    }
  }
  return count;
}

bool HasEvidence(const std::vector<api::EngineEvidenceReference>& evidence,
                 std::string_view kind) {
  for (const auto& entry : evidence) {
    if (entry.evidence_kind == kind) {
      return true;
    }
  }
  return false;
}

std::string SeedCommittedRow(Fixture& fixture, std::string id, std::string name) {
  auto context = Begin(fixture, "dpc022-seed");
  const auto inserted = InsertRow(fixture, context, std::move(id), std::move(name));
  RequireOk(inserted, "DPC-022 seed insert failed");
  Require(inserted.inserted_count == 1 && inserted.row_uuids.size() == 1,
          "DPC-022 seed insert result shape changed");
  const std::string row_uuid = inserted.row_uuids.front().canonical;
  Commit(context);
  return row_uuid;
}

void ValidateDefaultAndProofOnlyStaySynchronous() {
  auto default_fixture = MakeFixture("default", 1000);
  auto default_context = Begin(default_fixture, "dpc022-default-insert");
  const auto inserted = InsertRow(default_fixture, default_context, "id-default", "alpha");
  RequireOk(inserted, "DPC-022 default insert failed");
  Require(LoadLedger(default_fixture).records.empty(),
          "DPC-022 default insert emitted deferred delta records");
  Require(CountIndexEntries(default_fixture,
                            default_context,
                            default_fixture.non_unique_index_uuid) == 1,
          "DPC-022 default insert did not synchronously maintain non-unique index");
  Require(CountIndexEntries(default_fixture,
                            default_context,
                            default_fixture.unique_index_uuid) == 1,
          "DPC-022 default insert did not synchronously maintain unique index");
  Rollback(default_context);

  auto proof_fixture = MakeFixture("proof_only", 2000);
  auto proof_context = Begin(proof_fixture, "dpc022-proof-insert");
  const auto proof_inserted = InsertRow(proof_fixture,
                                       proof_context,
                                       "id-proof",
                                       "alpha",
                                       ProofOnlyOptions());
  RequireOk(proof_inserted, "DPC-022 proof-only insert failed");
  Require(LoadLedger(proof_fixture).records.empty(),
          "DPC-022 proof-only insert emitted deferred delta records");
  Require(CountIndexEntries(proof_fixture,
                            proof_context,
                            proof_fixture.non_unique_index_uuid) == 1,
          "DPC-022 proof-only insert did not synchronously maintain non-unique index");
  Rollback(proof_context);
}

void ValidateDeferredInsertAndCommitState() {
  auto fixture = MakeFixture("insert", 3000);
  auto context = Begin(fixture, "dpc022-deferred-insert");
  const auto inserted = InsertRow(fixture, context, "id-insert", "alpha", DeferredOptions());
  RequireOk(inserted, "DPC-022 deferred insert failed");
  Require(inserted.inserted_count == 1, "DPC-022 deferred insert count changed");
  Require(HasEvidence(inserted.evidence, "mga_secondary_index_delta_ledger"),
          "DPC-022 deferred insert evidence omitted delta ledger record");

  auto ledger = LoadLedger(fixture);
  auto records = RecordsForTx(ledger, context.local_transaction_id);
  Require(records.size() == 1, "DPC-022 deferred insert did not write one non-unique delta");
  Require(RecordIndexUuid(records.front()) == fixture.non_unique_index_uuid,
          "DPC-022 deferred insert wrote delta for wrong index");
  Require(records.front().delta.delta_kind == idx::SecondaryIndexDeltaKind::insert,
          "DPC-022 deferred insert operation kind changed");
  Require(records.front().commit_state ==
              idx::SecondaryIndexDeltaLedgerCommitState::precommit_uncommitted &&
              !records.front().delta.committed,
          "DPC-022 deferred insert did not preserve precommit MGA state before commit");
  Require(PayloadField(records.front(), "key") == "alpha",
          "DPC-022 deferred insert key payload mismatch");
  Require(CountIndexEntries(fixture, context, fixture.non_unique_index_uuid) == 0,
          "DPC-022 deferred insert still wrote synchronous non-unique index entry");
  Require(CountIndexEntries(fixture, context, fixture.unique_index_uuid) == 1,
          "DPC-022 deferred insert did not preserve synchronous unique index entry");

  Commit(context);
  ledger = LoadLedger(fixture);
  records = RecordsForTx(ledger, context.local_transaction_id);
  Require(records.size() == 1, "DPC-022 committed deferred insert lost its delta");
  Require(records.front().commit_state ==
              idx::SecondaryIndexDeltaLedgerCommitState::committed_premerge &&
              records.front().delta.committed,
          "DPC-022 commit did not mark deferred insert delta committed_premerge");
}

void ValidateDeferredUpdateAndRollbackCleanup() {
  auto fixture = MakeFixture("update", 4000);
  (void)SeedCommittedRow(fixture, "id-update", "alpha");

  auto context = Begin(fixture, "dpc022-deferred-update");
  const auto updated = UpdateName(fixture, context, "id-update", "bravo", DeferredOptions());
  RequireOk(updated, "DPC-022 deferred update failed");
  Require(updated.updated_count == 1, "DPC-022 deferred update count changed");
  auto records = RecordsForTx(LoadLedger(fixture), context.local_transaction_id);
  Require(records.size() == 2, "DPC-022 deferred update did not write before/after deltas");
  Require(records[0].delta.delta_kind == idx::SecondaryIndexDeltaKind::update_before,
          "DPC-022 deferred update_before operation kind changed");
  Require(records[1].delta.delta_kind == idx::SecondaryIndexDeltaKind::update_after,
          "DPC-022 deferred update_after operation kind changed");
  Require(PayloadField(records[0], "key") == "alpha" &&
              PayloadField(records[1], "key") == "bravo",
          "DPC-022 deferred update key payloads changed");
  Require(RecordIndexUuid(records[0]) == fixture.non_unique_index_uuid &&
              RecordIndexUuid(records[1]) == fixture.non_unique_index_uuid,
          "DPC-022 deferred update wrote a unique or wrong-index delta");
  Rollback(context);
  records = RecordsForTx(LoadLedger(fixture), context.local_transaction_id);
  Require(records.empty(), "DPC-022 rollback left visible deferred update deltas");
}

void ValidateDeferredDeleteWritesTombstoneDelta() {
  auto fixture = MakeFixture("delete", 5000);
  const std::string row_uuid = SeedCommittedRow(fixture, "id-delete", "alpha");

  auto context = Begin(fixture, "dpc022-deferred-delete");
  const auto deleted = DeleteRow(fixture, context, row_uuid, DeferredOptions());
  RequireOk(deleted, "DPC-022 deferred delete failed");
  Require(deleted.deleted_count == 1, "DPC-022 deferred delete count changed");
  const auto records = RecordsForTx(LoadLedger(fixture), context.local_transaction_id);
  Require(records.size() == 1, "DPC-022 deferred delete did not write one tombstone delta");
  Require(records.front().delta.delta_kind == idx::SecondaryIndexDeltaKind::delete_row,
          "DPC-022 deferred delete operation kind changed");
  Require(PayloadField(records.front(), "key") == "alpha",
          "DPC-022 deferred delete key payload mismatch");
  Require(RecordIndexUuid(records.front()) == fixture.non_unique_index_uuid,
          "DPC-022 deferred delete wrote a unique or wrong-index delta");
  Rollback(context);
}

void ValidateErrorPathLeavesNoDelta() {
  auto fixture = MakeFixture("duplicate", 6000);
  (void)SeedCommittedRow(fixture, "id-duplicate", "alpha");

  auto context = Begin(fixture, "dpc022-duplicate-insert");
  const auto duplicate = InsertRow(fixture,
                                  context,
                                  "id-duplicate",
                                  "bravo",
                                  DeferredOptions());
  Require(!duplicate.ok, "DPC-022 duplicate insert unexpectedly succeeded");
  Require(LoadLedger(fixture).records.empty(),
          "DPC-022 validation error left a deferred delta record");
  Rollback(context);
}

}  // namespace

int main() {
  Require(kWritePathSearchKey == "DPC_DEFERRED_INDEX_WRITE_PATH",
          "DPC-022 write-path search key drifted");
  Require(kGateSearchKey == "DPC_SECONDARY_INDEX_DELTA_WRITE_PATH_GATE",
          "DPC-022 gate search key drifted");

  ValidateDefaultAndProofOnlyStaySynchronous();
  ValidateDeferredInsertAndCommitState();
  ValidateDeferredUpdateAndRollbackCleanup();
  ValidateDeferredDeleteWritesTombstoneDelta();
  ValidateErrorPathLeavesNoDelta();
  return EXIT_SUCCESS;
}

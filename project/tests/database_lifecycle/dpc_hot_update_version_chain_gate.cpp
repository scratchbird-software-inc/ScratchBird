// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "database_lifecycle.hpp"
#include "dml/insert_api.hpp"
#include "dml/select_api.hpp"
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

constexpr std::string_view kHotUpdateSearchKey = "DPC_HOT_UPDATE_SHAPE";
constexpr std::string_view kGateSearchKey = "DPC_HOT_UPDATE_VERSION_CHAIN_GATE";

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view message) {
  if (!condition) { Fail(message); }
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
  Require(generated.ok(), "DPC-026 generated UUID creation failed");
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

api::EngineRowValue Row(std::string id, std::string name, std::string note) {
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

std::vector<std::string> HotShapeDisabledOptions() {
  return {"runtime.hot_update_shape=disabled"};
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
    if (!dir.empty()) { std::filesystem::remove_all(dir, ignored); }
  }
};

api::EngineRequestContext BaseContext(const Fixture& fixture,
                                      std::string request_id) {
  api::EngineRequestContext context;
  context.trust_mode = api::EngineTrustMode::server_isolated;
  context.request_id = std::move(request_id);
  context.database_path = fixture.database_path.string();
  context.database_uuid.canonical = fixture.database_uuid;
  context.principal_uuid.canonical =
      NewUuidText(platform::UuidKind::principal, fixture.salt + 100);
  context.session_uuid.canonical =
      NewUuidText(platform::UuidKind::object, fixture.salt + 101);
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
  RequireOk(begun, "DPC-026 begin transaction failed");
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
  RequireOk(api::EngineCommitTransaction(request), "DPC-026 commit failed");
}

void Rollback(const api::EngineRequestContext& context) {
  api::EngineRollbackTransactionRequest request;
  request.context = context;
  RequireOk(api::EngineRollbackTransaction(request), "DPC-026 rollback failed");
}

api::CrudTableRecord Table(const Fixture& fixture,
                           const api::EngineRequestContext& context) {
  api::CrudTableRecord table;
  table.creator_tx = context.local_transaction_id;
  table.table_uuid = fixture.table_uuid;
  table.default_name = "dpc026_hot_update_version_chain";
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
  if (unique) { index.key_envelopes.push_back("unique"); }
  return index;
}

Fixture MakeFixture(std::string name, platform::u64 salt) {
  Fixture fixture;
  fixture.salt = salt;
  fixture.dir = std::filesystem::temp_directory_path() /
                ("scratchbird_dpc026_" + name + "_" +
                 std::to_string(NowMillis() + salt));
  std::filesystem::create_directories(fixture.dir);
  fixture.database_path = fixture.dir / "dpc026.sbdb";

  db::DatabaseCreateConfig create;
  create.path = fixture.database_path.string();
  create.database_uuid = NewUuid(platform::UuidKind::database, salt + 1);
  create.filespace_uuid = NewUuid(platform::UuidKind::filespace, salt + 2);
  create.creation_unix_epoch_millis = NowMillis() + salt + 3;
  create.require_resource_seed_pack = false;
  create.allow_minimal_resource_bootstrap = true;
  create.allow_overwrite = true;
  const auto created = db::CreateDatabaseFile(create);
  Require(created.ok(), "DPC-026 database create failed");

  fixture.database_uuid = uuid::UuidToString(create.database_uuid.value);
  fixture.table_uuid = NewUuidText(platform::UuidKind::object, salt + 10);
  fixture.non_unique_index_uuid = NewUuidText(platform::UuidKind::object, salt + 11);
  fixture.unique_index_uuid = NewUuidText(platform::UuidKind::object, salt + 12);

  auto context = Begin(fixture, "dpc026-metadata");
  RequireDiagnosticOk(api::AppendMgaTableMetadata(context, Table(fixture, context)),
                      "DPC-026 table metadata append failed");
  RequireDiagnosticOk(api::AppendMgaIndexMetadata(
                          context,
                          Index(fixture, context, fixture.non_unique_index_uuid,
                                "name", false)),
                      "DPC-026 non-unique index metadata append failed");
  RequireDiagnosticOk(api::AppendMgaIndexMetadata(
                          context,
                          Index(fixture, context, fixture.unique_index_uuid,
                                "id", true)),
                      "DPC-026 unique index metadata append failed");
  Commit(context);
  return fixture;
}

api::EngineInsertRowsResult InsertRow(const Fixture& fixture,
                                      const api::EngineRequestContext& context,
                                      std::string id,
                                      std::string name,
                                      std::string note) {
  api::EngineInsertRowsRequest request;
  request.context = context;
  request.target_table.uuid.canonical = fixture.table_uuid;
  request.target_table.object_kind = "table";
  request.input_rows.push_back(Row(std::move(id), std::move(name), std::move(note)));
  request.estimated_row_count = 1;
  return api::EngineInsertRows(request);
}

api::EngineUpdateRowsResult UpdateNameNote(
    const Fixture& fixture,
    const api::EngineRequestContext& context,
    std::string id,
    std::string name,
    std::string note,
    std::vector<std::string> options = {}) {
  api::EngineUpdateRowsRequest request;
  request.context = context;
  request.target_table.uuid.canonical = fixture.table_uuid;
  request.target_table.object_kind = "table";
  request.update_predicate.predicate_kind = "column_equals";
  request.update_predicate.canonical_predicate_envelope = "id";
  request.update_predicate.bound_values.push_back(TextValue(std::move(id)));
  request.assignments.push_back({"name", TextValue(std::move(name))});
  request.assignments.push_back({"note", TextValue(std::move(note))});
  request.option_envelopes = std::move(options);
  return api::EngineUpdateRows(request);
}

api::EngineUpdateRowsResult UpdateIdNameNote(
    const Fixture& fixture,
    const api::EngineRequestContext& context,
    std::string old_id,
    std::string new_id,
    std::string name,
    std::string note,
    std::vector<std::string> options = {}) {
  api::EngineUpdateRowsRequest request;
  request.context = context;
  request.target_table.uuid.canonical = fixture.table_uuid;
  request.target_table.object_kind = "table";
  request.update_predicate.predicate_kind = "column_equals";
  request.update_predicate.canonical_predicate_envelope = "id";
  request.update_predicate.bound_values.push_back(TextValue(std::move(old_id)));
  request.assignments.push_back({"id", TextValue(std::move(new_id))});
  request.assignments.push_back({"name", TextValue(std::move(name))});
  request.assignments.push_back({"note", TextValue(std::move(note))});
  request.option_envelopes = std::move(options);
  return api::EngineUpdateRows(request);
}

api::EnginePredicateEnvelope EqualsPredicate(std::string column,
                                             std::string value) {
  api::EnginePredicateEnvelope predicate;
  predicate.predicate_kind = "column_equals";
  predicate.canonical_predicate_envelope = std::move(column);
  predicate.bound_values.push_back(TextValue(std::move(value)));
  return predicate;
}

api::EngineSelectRowsResult SelectEquals(const Fixture& fixture,
                                         const api::EngineRequestContext& context,
                                         std::string column,
                                         std::string value) {
  api::EngineSelectRowsRequest request;
  request.context = context;
  request.source_object.uuid.canonical = fixture.table_uuid;
  request.source_object.object_kind = "table";
  request.select_predicate = EqualsPredicate(std::move(column), std::move(value));
  return api::EngineSelectRows(request);
}

bool HasEvidence(const std::vector<api::EngineEvidenceReference>& evidence,
                 std::string_view kind) {
  for (const auto& entry : evidence) {
    if (entry.evidence_kind == kind) { return true; }
  }
  return false;
}

std::uint64_t EvidenceCounter(const std::vector<api::EngineEvidenceReference>& evidence,
                              std::string_view kind) {
  for (const auto& entry : evidence) {
    if (entry.evidence_kind == kind) {
      try {
        return static_cast<std::uint64_t>(std::stoull(entry.evidence_id));
      } catch (...) {
        return 0;
      }
    }
  }
  return 0;
}

std::size_t CountRowsWithField(const api::EngineSelectRowsResult& result,
                               std::string_view field,
                               std::string_view value) {
  std::size_t count = 0;
  for (const auto& row : result.result_shape.rows) {
    for (const auto& candidate : row.fields) {
      if (candidate.first == field &&
          candidate.second.encoded_value == value) {
        ++count;
      }
    }
  }
  return count;
}

api::MgaRelationStoreState LoadState(const Fixture& fixture,
                                     const api::EngineRequestContext& context) {
  const auto loaded = api::LoadMgaRelationStoreState(context);
  if (!loaded.ok) {
    std::cerr << loaded.diagnostic.code << ':' << loaded.diagnostic.detail << '\n';
  }
  Require(loaded.ok, "DPC-026 relation store load failed");
  (void)fixture;
  return loaded.state;
}

std::size_t CountRowVersions(const Fixture& fixture,
                             const api::EngineRequestContext& context) {
  const auto loaded = LoadState(fixture, context);
  std::size_t count = 0;
  for (const auto& row : loaded.row_versions) {
    if (row.table_uuid == fixture.table_uuid) { ++count; }
  }
  return count;
}

std::size_t CountVisibleIndexEntries(const Fixture& fixture,
                                     const api::EngineRequestContext& context,
                                     const std::string& index_uuid) {
  const auto loaded = LoadState(fixture, context);
  const auto state = api::BuildCrudCompatibilityStateFromMga(loaded);
  std::size_t count = 0;
  for (const auto& entry : loaded.index_entries) {
    if (entry.table_uuid == fixture.table_uuid &&
        entry.index_uuid == index_uuid &&
        api::CrudCreatorVisible(state,
                                entry.creator_tx,
                                entry.event_sequence,
                                context.local_transaction_id)) {
      ++count;
    }
  }
  return count;
}

idx::PersistentSecondaryIndexDeltaLedger LoadLedger(const Fixture& fixture) {
  const auto loaded =
      api::LoadMgaSecondaryIndexDeltaLedger(BaseContext(fixture, "dpc026-ledger-load"));
  if (!loaded.ok) {
    std::cerr << loaded.diagnostic.code << ':' << loaded.diagnostic.detail << '\n';
  }
  Require(loaded.ok, "DPC-026 ledger load failed");
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
    if (key == field) { return value; }
  }
  return {};
}

void RequireLookupCount(const Fixture& fixture,
                        const api::EngineRequestContext& context,
                        std::string column,
                        std::string value,
                        std::size_t expected_count,
                        std::string_view message) {
  const auto selected = SelectEquals(fixture, context, std::move(column), std::move(value));
  RequireOk(selected, message);
  Require(selected.visible_count == expected_count, message);
}

void RequireLookupNote(const Fixture& fixture,
                       const api::EngineRequestContext& context,
                       std::string name,
                       std::string note,
                       std::string_view message) {
  const auto selected = SelectEquals(fixture, context, "name", std::move(name));
  RequireOk(selected, message);
  Require(selected.visible_count == 1 &&
              CountRowsWithField(selected, "note", note) == 1,
          message);
}

void RequireLookupField(const Fixture& fixture,
                        const api::EngineRequestContext& context,
                        std::string column,
                        std::string value,
                        std::string field,
                        std::string expected,
                        std::string_view message) {
  const auto selected = SelectEquals(fixture,
                                     context,
                                     std::move(column),
                                     std::move(value));
  RequireOk(selected, message);
  Require(selected.visible_count == 1 &&
              CountRowsWithField(selected, field, expected) == 1,
          message);
}

void SeedCommittedRow(Fixture& fixture, std::string id, std::string name) {
  auto context = Begin(fixture, "dpc026-seed");
  const auto inserted = InsertRow(fixture,
                                  context,
                                  std::move(id),
                                  std::move(name),
                                  "note0");
  RequireOk(inserted, "DPC-026 seed insert failed");
  Require(inserted.inserted_count == 1, "DPC-026 seed insert count changed");
  Commit(context);
}

void ValidateHotNonKeyUpdateAvoidsSynchronousChurn() {
  auto fixture = MakeFixture("sync_nonkey", 1000);
  SeedCommittedRow(fixture, "id-hot", "alpha");

  auto observer = Begin(fixture, "dpc026-sync-nonkey-observer");
  const auto before_index_entries =
      CountVisibleIndexEntries(fixture, observer, fixture.non_unique_index_uuid);
  const auto before_versions = CountRowVersions(fixture, observer);

  auto writer = Begin(fixture, "dpc026-sync-nonkey-writer");
  const auto updated =
      UpdateNameNote(fixture, writer, "id-hot", "alpha", "note1");
  RequireOk(updated, "DPC-026 hot non-key update failed");
  Require(updated.updated_count == 1, "DPC-026 hot non-key update count changed");
  Require(HasEvidence(updated.evidence, "DPC_HOT_UPDATE_SHAPE"),
          "DPC-026 hot update evidence anchor missing");
  Require(EvidenceCounter(updated.evidence,
                          "dpc_hot_update_shape_index_churn_avoided") >= 1,
          "DPC-026 hot update did not report avoided index churn");
  Require(EvidenceCounter(updated.evidence,
                          "dpc_hot_update_shape_synchronous_unchanged_key_skipped") >= 1,
          "DPC-026 hot update did not skip unchanged synchronous key");
  Require(EvidenceCounter(updated.evidence,
                          "dpc_hot_update_shape_synchronous_changed_key_maintained") == 0,
          "DPC-026 hot non-key update reported changed-key maintenance");
  Require(CountVisibleIndexEntries(fixture, writer, fixture.non_unique_index_uuid) ==
              before_index_entries,
          "DPC-026 hot non-key update appended an unchanged-key index entry");
  Require(CountRowVersions(fixture, writer) == before_versions + 1,
          "DPC-026 hot non-key update did not create a new MGA row version");

  RequireLookupNote(fixture, observer, "alpha", "note0",
                    "DPC-026 precommit reader lost old version");
  Commit(writer);
  RequireLookupNote(fixture, observer, "alpha", "note1",
                    "DPC-026 postcommit reader did not see new version");
  Rollback(observer);
}

void ValidateDisabledBaselineChurnAndRollback() {
  auto fixture = MakeFixture("disabled_baseline", 2000);
  SeedCommittedRow(fixture, "id-disabled", "alpha");

  auto reader = Begin(fixture, "dpc026-disabled-reader");
  const auto before_index_entries =
      CountVisibleIndexEntries(fixture, reader, fixture.non_unique_index_uuid);

  auto writer = Begin(fixture, "dpc026-disabled-writer");
  const auto updated = UpdateNameNote(fixture,
                                      writer,
                                      "id-disabled",
                                      "alpha",
                                      "note-disabled",
                                      HotShapeDisabledOptions());
  RequireOk(updated, "DPC-026 disabled baseline update failed");
  Require(EvidenceCounter(updated.evidence,
                          "dpc_hot_update_shape_disabled_baseline_churn_decisions") >= 1,
          "DPC-026 disabled baseline did not report normal index churn");
  Require(EvidenceCounter(updated.evidence,
                          "dpc_hot_update_shape_index_churn_avoided") == 0,
          "DPC-026 disabled baseline still reported avoided churn");
  Require(CountVisibleIndexEntries(fixture, writer, fixture.non_unique_index_uuid) ==
              before_index_entries + 1,
          "DPC-026 disabled baseline did not append the unchanged-key index entry");

  Rollback(writer);
  Require(CountVisibleIndexEntries(fixture, reader, fixture.non_unique_index_uuid) ==
              before_index_entries,
          "DPC-026 rollback left a visible disabled-baseline index entry");
  RequireLookupNote(fixture, reader, "alpha", "note0",
                    "DPC-026 rollback did not restore old visible version");
  Rollback(reader);
}

void ValidateSynchronousChangedKeyCommitAndRollback() {
  auto commit_fixture = MakeFixture("sync_changed_commit", 3000);
  SeedCommittedRow(commit_fixture, "id-sync", "alpha");

  auto reader = Begin(commit_fixture, "dpc026-sync-reader");
  auto writer = Begin(commit_fixture, "dpc026-sync-writer");
  const auto updated =
      UpdateNameNote(commit_fixture, writer, "id-sync", "bravo", "note1");
  RequireOk(updated, "DPC-026 synchronous changed-key update failed");
  Require(EvidenceCounter(updated.evidence,
                          "dpc_hot_update_shape_synchronous_changed_key_maintained") >= 1,
          "DPC-026 synchronous changed-key maintenance evidence missing");
  RequireLookupNote(commit_fixture, reader, "alpha", "note0",
                    "DPC-026 precommit reader did not retain old key");
  RequireLookupCount(commit_fixture, reader, "name", "bravo", 0,
                     "DPC-026 precommit reader saw uncommitted new key");
  Commit(writer);
  RequireLookupCount(commit_fixture, reader, "name", "alpha", 0,
                     "DPC-026 committed changed-key update left old-key lookup visible");
  RequireLookupNote(commit_fixture, reader, "bravo", "note1",
                    "DPC-026 committed changed-key update missed new-key lookup");
  Rollback(reader);

  auto unique_fixture = MakeFixture("sync_unique_changed_commit", 3500);
  SeedCommittedRow(unique_fixture, "id-unique", "alpha");
  auto unique_reader = Begin(unique_fixture, "dpc026-unique-reader");
  auto unique_writer = Begin(unique_fixture, "dpc026-unique-writer");
  const auto unique_updated = UpdateIdNameNote(unique_fixture,
                                               unique_writer,
                                               "id-unique",
                                               "id-unique-new",
                                               "alpha",
                                               "note-unique");
  RequireOk(unique_updated, "DPC-026 unique changed-key update failed");
  Require(EvidenceCounter(unique_updated.evidence,
                          "dpc_hot_update_shape_synchronous_changed_key_maintained") >= 1,
          "DPC-026 unique changed-key maintenance evidence missing");
  RequireLookupCount(unique_fixture, unique_reader, "id", "id-unique", 1,
                     "DPC-026 unique precommit reader lost old key");
  RequireLookupCount(unique_fixture, unique_reader, "id", "id-unique-new", 0,
                     "DPC-026 unique precommit reader saw new key");
  Commit(unique_writer);
  RequireLookupCount(unique_fixture, unique_reader, "id", "id-unique", 0,
                     "DPC-026 unique commit left old key visible");
  RequireLookupField(unique_fixture,
                     unique_reader,
                     "id",
                     "id-unique-new",
                     "note",
                     "note-unique",
                     "DPC-026 unique commit missed new key");
  Rollback(unique_reader);

  auto rollback_fixture = MakeFixture("sync_changed_rollback", 4000);
  SeedCommittedRow(rollback_fixture, "id-rollback", "alpha");
  auto rollback_reader = Begin(rollback_fixture, "dpc026-sync-rollback-reader");
  const auto before_index_entries =
      CountVisibleIndexEntries(rollback_fixture,
                               rollback_reader,
                               rollback_fixture.non_unique_index_uuid);
  auto rollback_writer = Begin(rollback_fixture, "dpc026-sync-rollback-writer");
  RequireOk(UpdateNameNote(rollback_fixture,
                           rollback_writer,
                           "id-rollback",
                           "charlie",
                           "note2"),
            "DPC-026 synchronous rollback update failed");
  Rollback(rollback_writer);
  Require(CountVisibleIndexEntries(rollback_fixture,
                                   rollback_reader,
                                   rollback_fixture.non_unique_index_uuid) ==
              before_index_entries,
          "DPC-026 rollback left a visible synchronous index entry");
  RequireLookupNote(rollback_fixture, rollback_reader, "alpha", "note0",
                    "DPC-026 synchronous rollback lost old key");
  RequireLookupCount(rollback_fixture, rollback_reader, "name", "charlie", 0,
                     "DPC-026 synchronous rollback left new key visible");
  Rollback(rollback_reader);

  auto unique_rollback_fixture = MakeFixture("sync_unique_changed_rollback", 4500);
  SeedCommittedRow(unique_rollback_fixture, "id-unique-rollback", "alpha");
  auto unique_rollback_reader =
      Begin(unique_rollback_fixture, "dpc026-unique-rollback-reader");
  const auto before_unique_entries =
      CountVisibleIndexEntries(unique_rollback_fixture,
                               unique_rollback_reader,
                               unique_rollback_fixture.unique_index_uuid);
  auto unique_rollback_writer =
      Begin(unique_rollback_fixture, "dpc026-unique-rollback-writer");
  RequireOk(UpdateIdNameNote(unique_rollback_fixture,
                             unique_rollback_writer,
                             "id-unique-rollback",
                             "id-unique-rollback-new",
                             "alpha",
                             "note-unique-rollback"),
            "DPC-026 unique rollback update failed");
  Rollback(unique_rollback_writer);
  Require(CountVisibleIndexEntries(unique_rollback_fixture,
                                   unique_rollback_reader,
                                   unique_rollback_fixture.unique_index_uuid) ==
              before_unique_entries,
          "DPC-026 unique rollback left a visible index entry");
  RequireLookupCount(unique_rollback_fixture,
                     unique_rollback_reader,
                     "id",
                     "id-unique-rollback",
                     1,
                     "DPC-026 unique rollback lost old key");
  RequireLookupCount(unique_rollback_fixture,
                     unique_rollback_reader,
                     "id",
                     "id-unique-rollback-new",
                     0,
                     "DPC-026 unique rollback left new key visible");
  Rollback(unique_rollback_reader);
}

void ValidateDeferredChangedKeyDeltasAndNonKeySkip() {
  auto commit_fixture = MakeFixture("deferred_changed_commit", 5000);
  SeedCommittedRow(commit_fixture, "id-deferred", "alpha");

  auto reader = Begin(commit_fixture, "dpc026-deferred-reader");
  auto writer = Begin(commit_fixture, "dpc026-deferred-writer");
  const auto updated = UpdateNameNote(commit_fixture,
                                      writer,
                                      "id-deferred",
                                      "bravo",
                                      "note1",
                                      DeferredOptions());
  RequireOk(updated, "DPC-026 deferred changed-key update failed");
  Require(EvidenceCounter(updated.evidence,
                          "dpc_hot_update_shape_deferred_changed_key_delta_pairs") >= 1,
          "DPC-026 deferred changed-key delta evidence missing");
  auto records = RecordsForTx(LoadLedger(commit_fixture),
                              writer.local_transaction_id);
  Require(records.size() == 2,
          "DPC-026 deferred changed-key update did not write before/after deltas");
  Require(records[0].delta.delta_kind == idx::SecondaryIndexDeltaKind::update_before &&
              records[1].delta.delta_kind == idx::SecondaryIndexDeltaKind::update_after,
          "DPC-026 deferred changed-key delta kinds changed");
  Require(PayloadField(records[0], "key") == "alpha" &&
              PayloadField(records[1], "key") == "bravo",
          "DPC-026 deferred changed-key delta payloads changed");
  Require(RecordIndexUuid(records[0]) == commit_fixture.non_unique_index_uuid &&
              RecordIndexUuid(records[1]) == commit_fixture.non_unique_index_uuid,
          "DPC-026 deferred changed-key deltas used the wrong index");
  RequireLookupNote(commit_fixture, reader, "alpha", "note0",
                    "DPC-026 deferred precommit reader lost old key");
  RequireLookupCount(commit_fixture, reader, "name", "bravo", 0,
                     "DPC-026 deferred precommit reader saw new key");
  Commit(writer);
  RequireLookupCount(commit_fixture, reader, "name", "alpha", 0,
                     "DPC-026 deferred commit left old key visible");
  RequireLookupNote(commit_fixture, reader, "bravo", "note1",
                    "DPC-026 deferred commit missed new key overlay");
  Rollback(reader);

  auto nonkey_fixture = MakeFixture("deferred_nonkey_skip", 6000);
  SeedCommittedRow(nonkey_fixture, "id-deferred-nonkey", "alpha");
  auto nonkey_reader = Begin(nonkey_fixture, "dpc026-deferred-nonkey-reader");
  const auto before_index_entries =
      CountVisibleIndexEntries(nonkey_fixture,
                               nonkey_reader,
                               nonkey_fixture.non_unique_index_uuid);
  auto nonkey_writer = Begin(nonkey_fixture, "dpc026-deferred-nonkey-writer");
  const auto nonkey_updated = UpdateNameNote(nonkey_fixture,
                                             nonkey_writer,
                                             "id-deferred-nonkey",
                                             "alpha",
                                             "note1",
                                             DeferredOptions());
  RequireOk(nonkey_updated, "DPC-026 deferred non-key update failed");
  Require(EvidenceCounter(nonkey_updated.evidence,
                          "dpc_hot_update_shape_deferred_unchanged_key_skipped") >= 1,
          "DPC-026 deferred non-key update did not skip unchanged key");
  Require(RecordsForTx(LoadLedger(nonkey_fixture),
                       nonkey_writer.local_transaction_id).empty(),
          "DPC-026 deferred non-key update wrote a stray delta");
  Require(CountVisibleIndexEntries(nonkey_fixture,
                                   nonkey_writer,
                                   nonkey_fixture.non_unique_index_uuid) ==
              before_index_entries,
          "DPC-026 deferred non-key update changed synchronous index entries");
  Commit(nonkey_writer);
  RequireLookupNote(nonkey_fixture, nonkey_reader, "alpha", "note1",
                    "DPC-026 deferred non-key commit did not publish new row version");
  Rollback(nonkey_reader);

  auto rollback_fixture = MakeFixture("deferred_changed_rollback", 7000);
  SeedCommittedRow(rollback_fixture, "id-deferred-rollback", "alpha");
  auto rollback_reader = Begin(rollback_fixture, "dpc026-deferred-rollback-reader");
  auto rollback_writer = Begin(rollback_fixture, "dpc026-deferred-rollback-writer");
  RequireOk(UpdateNameNote(rollback_fixture,
                           rollback_writer,
                           "id-deferred-rollback",
                           "charlie",
                           "note2",
                           DeferredOptions()),
            "DPC-026 deferred rollback update failed");
  Require(RecordsForTx(LoadLedger(rollback_fixture),
                       rollback_writer.local_transaction_id).size() == 2,
          "DPC-026 deferred rollback setup did not write deltas");
  Rollback(rollback_writer);
  Require(RecordsForTx(LoadLedger(rollback_fixture),
                       rollback_writer.local_transaction_id).empty(),
          "DPC-026 rollback left deferred delta records");
  RequireLookupNote(rollback_fixture, rollback_reader, "alpha", "note0",
                    "DPC-026 deferred rollback lost old key");
  RequireLookupCount(rollback_fixture, rollback_reader, "name", "charlie", 0,
                     "DPC-026 deferred rollback left new key visible");
  Rollback(rollback_reader);
}

}  // namespace

int main() {
  Require(kHotUpdateSearchKey == "DPC_HOT_UPDATE_SHAPE",
          "DPC-026 hot update search key drifted");
  Require(kGateSearchKey == "DPC_HOT_UPDATE_VERSION_CHAIN_GATE",
          "DPC-026 gate search key drifted");

  ValidateHotNonKeyUpdateAvoidsSynchronousChurn();
  ValidateDisabledBaselineChurnAndRollback();
  ValidateSynchronousChangedKeyCommitAndRollback();
  ValidateDeferredChangedKeyDeltasAndNonKeySkip();
  return EXIT_SUCCESS;
}

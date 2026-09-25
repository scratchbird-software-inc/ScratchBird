// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "../support/engine_evidence_fixture.hpp"
#include "../support/engine_statement_fixture.hpp"
#include "catalog/column_metadata_codec.hpp"
#include "memory.hpp"
#include "database_lifecycle.hpp"
#include "dml/delete_api.hpp"
#include "dml/import_execution_api.hpp"
#include "dml/insert_api.hpp"
#include "dml/update_api.hpp"
#include "mga_relation_store/mga_relation_store.hpp"
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
namespace platform = scratchbird::core::platform;
namespace uuid = scratchbird::core::uuid;

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
                << result.diagnostics.front().message_key << ':'
                << result.diagnostics.front().detail << '\n';
    }
    Fail(message);
  }
}

platform::u64 NowMillis() {
  return static_cast<platform::u64>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch()).count());
}

platform::TypedUuid NewUuid(platform::UuidKind kind, platform::u64 salt) {
  const auto generated = uuid::GenerateEngineIdentityV7(kind, NowMillis() + salt);
  Require(generated.ok(), "ODF-038 UUID generation failed");
  return generated.value;
}

platform::Uuid NewNativeUuid(platform::UuidKind kind, platform::u64 salt) {
  return NewUuid(kind, salt).value;
}

struct Fixture {
  std::filesystem::path dir;
  std::filesystem::path database_path;
  platform::Uuid database_uuid;
  api::EngineRequestContext owner_context;
  std::shared_ptr<scratchbird::tests::FixtureEngineSession> session;
  platform::Uuid table_uuid;
  platform::Uuid index_uuid;
  platform::Uuid primary_key_uuid;
  platform::u64 salt = 0;

  ~Fixture() {
    session.reset();
    if (!dir.empty()) {
      std::error_code ignored;
      std::filesystem::remove_all(dir, ignored);
    }
  }
};

api::EngineTypedValue TextValue(std::string value) {
  api::EngineTypedValue typed;
  typed.descriptor.descriptor_kind = "scalar";
  typed.descriptor.canonical_type_name = "character";
  typed.descriptor.encoded_descriptor = "canonical=character";
  typed.encoded_value = std::move(value);
  return typed;
}

api::EngineRowValue Row(std::string id, std::string note) {
  api::EngineRowValue row;
  row.fields.push_back({"id", TextValue(std::move(id))});
  row.fields.push_back({"note", TextValue(std::move(note))});
  return row;
}

bool HasEvidence(const std::vector<api::EngineEvidenceReference>& evidence,
                 std::string_view kind,
                 std::string_view id) {
  for (const auto& item : evidence) {
    if (item.evidence_kind == kind && scratchbird::tests::EvidenceTextFields(item.evidence_id) == id) {
      return true;
    }
  }
  return false;
}

std::string EvidenceValue(const std::vector<api::EngineEvidenceReference>& evidence,
                          std::string_view kind) {
  for (const auto& item : evidence) {
    if (item.evidence_kind == kind) {
      return scratchbird::tests::EvidenceTextFields(item.evidence_id);
    }
  }
  return {};
}

void RequireNoPathLeak(const std::vector<api::EngineEvidenceReference>& evidence) {
  for (const auto& item : evidence) {
    const std::string combined = item.evidence_kind + ":" + scratchbird::tests::EvidenceTextFields(item.evidence_id);
    for (const auto token :
         {"docs" "/execution-plans", "findings", "contracts", "references"}) {
      Require(combined.find(token) == std::string::npos,
              "ODF-038 runtime evidence leaked documentation token");
    }
  }
}

void RequireNoVerboseSummaryTrace(
    const std::vector<api::EngineEvidenceReference>& evidence) {
  for (const auto& item : evidence) {
    if (item.evidence_kind.rfind("dml_summary.", 0) != 0) {
      continue;
    }
    Require(item.evidence_kind.find("trace") == std::string::npos &&
                scratchbird::tests::EvidenceTextFields(item.evidence_id).find("trace") == std::string::npos,
            "ODF-038 summary evidence used trace formatting");
  }
}

void RequireSummaryEvidence(const api::EngineApiResult& result,
                            std::string_view operation) {
  Require(result.dml_summary.benchmark_clean,
          "ODF-038 DML summary was not benchmark-clean");
  Require(HasEvidence(result.evidence,
                      "dml_summary.schema_id",
                      "scratchbird.dml_summary_counters.v1"),
          "ODF-038 summary schema evidence missing");
  Require(HasEvidence(result.evidence, "dml_summary.operation", operation),
          "ODF-038 summary operation evidence missing: expected=" +
              std::string(operation) + " result_operation=" +
              result.operation_id + " evidence_operation=" +
              EvidenceValue(result.evidence, "dml_summary.operation"));
  Require(HasEvidence(result.evidence, "dml_summary.benchmark_clean", "true"),
          "ODF-038 summary benchmark-clean evidence missing");
  Require(EvidenceValue(result.evidence, "dml_summary.rows_changed") ==
              std::to_string(result.dml_summary.rows_changed),
          "ODF-038 rows changed summary evidence mismatch");
  Require(EvidenceValue(result.evidence, "dml_summary.visible_rows_scanned") ==
              std::to_string(result.dml_summary.visible_rows_scanned),
          "ODF-038 visible row scan summary evidence mismatch");
  Require(EvidenceValue(result.evidence, "dml_summary.index_probes") ==
              std::to_string(result.dml_summary.index_probes),
          "ODF-038 index probe summary evidence mismatch");
  Require(EvidenceValue(result.evidence, "dml_summary.append_calls") ==
              std::to_string(result.dml_summary.append_calls),
          "ODF-038 append call summary evidence mismatch");
  Require(EvidenceValue(result.evidence, "dml_summary.file_opens") ==
              std::to_string(result.dml_summary.file_opens),
          "ODF-038 file open summary evidence mismatch");
  Require(EvidenceValue(result.evidence, "dml_summary.flushes") ==
              std::to_string(result.dml_summary.flushes),
          "ODF-038 flush summary evidence mismatch");
  Require(EvidenceValue(result.evidence, "dml_summary.page_reservations") ==
              std::to_string(result.dml_summary.page_reservations),
          "ODF-038 page reservation summary evidence mismatch");
  Require(EvidenceValue(result.evidence, "dml_summary.fallback_reason_count") ==
              std::to_string(result.dml_summary.fallback_reasons.size()),
          "ODF-038 fallback reason count summary evidence mismatch");
  if (result.dml_summary.fallback_reasons.empty()) {
    Require(HasEvidence(result.evidence, "dml_summary.fallback_reason", "none"),
            "ODF-038 none fallback summary evidence missing");
  }
  RequireNoVerboseSummaryTrace(result.evidence);
  RequireNoPathLeak(result.evidence);
}

bool HasFallbackReason(const api::EngineDmlSummaryCounters& summary,
                       std::string_view reason) {
  for (const auto& observed : summary.fallback_reasons) {
    if (observed == reason) {
      return true;
    }
  }
  return false;
}

api::EngineRequestContext BaseContext(const Fixture& fixture,
                                      std::string request_id,
                                      bool security_context_present = true) {
  api::EngineRequestContext context = fixture.owner_context;
  context.trust_mode = api::EngineTrustMode::server_isolated;
  context.request_id = std::move(request_id);
  context.database_path = fixture.database_path.string();
  context.database_uuid = fixture.database_uuid;
  context.security_context_present = security_context_present;
  context.identifier_profile_uuid = "sbsql_v3";
  context.language_context.language_tag = "en";
  context.language_context.default_language_tag = "en";
  return context;
}

api::EngineRequestContext Begin(const Fixture& fixture, std::string request_id) {
  api::EngineBeginTransactionRequest request;
  request.context = BaseContext(fixture, std::move(request_id));
  request.isolation_level = "read_committed";
  const auto begun = api::EngineBeginTransaction(request);
  RequireOk(begun, "ODF-038 begin transaction failed");
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
  RequireOk(api::EngineCommitTransaction(request), "ODF-038 commit failed");
}

void Rollback(const api::EngineRequestContext& context) {
  api::EngineRollbackTransactionRequest request;
  request.context = context;
  RequireOk(api::EngineRollbackTransaction(request), "ODF-038 rollback failed");
}

api::CrudTableRecord Table(const Fixture& fixture,
                           const api::EngineRequestContext& context) {
  api::CrudTableRecord table;
  table.creator_tx = context.local_transaction_id;
  table.table_uuid = fixture.table_uuid;
  table.default_name = "optimizer_deficiency_odf_038";
  api::CatalogColumnMetadata key;
  key.text = {{"canonical", "character"}, {"primary_key", "true"}};
  key.identities = {{"candidate_key_constraint_uuid", fixture.primary_key_uuid},
                    {"support_uuid", fixture.index_uuid}};
  std::string key_metadata;
  Require(api::EncodeCatalogColumnMetadata(key, &key_metadata),
          "ODF-038 binary primary key metadata encoding failed");
  table.columns.push_back({"id", std::move(key_metadata)});
  table.columns.push_back({"note", "canonical=character"});
  return table;
}

api::CrudIndexRecord UniqueIdIndex(const Fixture& fixture,
                                   const api::EngineRequestContext& context) {
  api::CrudIndexRecord index;
  index.creator_tx = context.local_transaction_id;
  index.index_uuid = fixture.index_uuid;
  index.table_uuid = fixture.table_uuid;
  index.column_name = "id";
  index.family = api::kCrudIndexFamilyBtree;
  index.profile = api::kCrudIndexProfileRowStoreScalarBtreeV1;
  index.unique = true;
  index.key_envelopes.push_back("id");
  index.key_envelopes.push_back("unique");
  return index;
}

Fixture MakeFixture() {
  Fixture fixture;
  fixture.salt = 38000;
  fixture.dir = std::filesystem::temp_directory_path() /
                ("scratchbird_odf038_" +
                 std::to_string(NowMillis() + fixture.salt));
  std::filesystem::create_directories(fixture.dir);
  fixture.database_path = fixture.dir / "odf038.sbdb";

  db::DatabaseCreateConfig create;
  create.path = fixture.database_path.string();
  create.database_uuid = NewUuid(platform::UuidKind::database, fixture.salt + 1);
  create.filespace_uuid = NewUuid(platform::UuidKind::filespace, fixture.salt + 2);
  create.creation_unix_epoch_millis = NowMillis() + fixture.salt + 3;
  scratchbird::tests::ConfigureCredentialedFixtureBootstrap(create);
  create.allow_overwrite = true;
  const auto created = db::CreateDatabaseFile(create);
  Require(created.ok(), "ODF-038 database create failed");

  fixture.database_uuid = create.database_uuid.value;
  fixture.owner_context = scratchbird::tests::BootstrapFixtureOwnerContext(create);
  fixture.table_uuid = NewNativeUuid(platform::UuidKind::object, fixture.salt + 10);
  fixture.index_uuid = NewNativeUuid(platform::UuidKind::object, fixture.salt + 11);
  fixture.primary_key_uuid = NewNativeUuid(platform::UuidKind::object, fixture.salt + 12);

  auto metadata = Begin(fixture, "odf038-metadata");
  Require(!scratchbird::tests::PublishMgaTableFixture(
              metadata, Table(fixture, metadata), {"character", "character"},
              {UniqueIdIndex(fixture, metadata)}).error,
          "ODF-038 table metadata append failed");
  Require(!api::AppendMgaIndexMetadata(metadata, UniqueIdIndex(fixture, metadata)).error,
          "ODF-038 index metadata append failed");
  Commit(metadata);
  fixture.owner_context.current_schema_uuid = metadata.current_schema_uuid;
  fixture.session = std::make_shared<scratchbird::tests::FixtureEngineSession>(
      BaseContext(fixture, "odf038-session"));
  return fixture;
}

std::vector<std::string> BenchmarkOptions() {
  return {"page_allocation.runtime=enabled",
          "page_allocation.free_pages=64",
          "page_allocation.preallocate_data_pages=16",
          "page_allocation.preallocate_index_pages=16",
          "result_payload_policy=summary_only"};
}

api::EnginePredicateEnvelope EqualsPredicate(std::string column,
                                             std::string value) {
  api::EnginePredicateEnvelope predicate;
  predicate.predicate_kind = "column_equals";
  predicate.canonical_predicate_envelope = std::move(column);
  predicate.bound_values.push_back(TextValue(std::move(value)));
  return predicate;
}

api::EngineInsertRowsResult InsertRows(
    const Fixture& fixture,
    const api::EngineRequestContext& context,
    std::vector<api::EngineRowValue> rows) {
  scratchbird::tests::FixtureEngineRequest<api::EngineInsertRowsRequest> request(
      *fixture.session, context);
  request.target_table.uuid = fixture.table_uuid;
  request.target_table.object_kind = "table";
  request.input_rows = std::move(rows);
  request.estimated_row_count =
      static_cast<api::EngineApiU64>(request.input_rows.size());
  request.option_envelopes = BenchmarkOptions();
  return api::EngineInsertRows(request);
}

api::EngineInsertRowsResult RefusedInsertPageReservationDisabled(
    const Fixture& fixture,
    const api::EngineRequestContext& context) {
  scratchbird::tests::FixtureEngineRequest<api::EngineInsertRowsRequest> request(
      *fixture.session, context);
  request.target_table.uuid = fixture.table_uuid;
  request.target_table.object_kind = "table";
  request.input_rows.push_back(Row("refused-1", "should-not-write"));
  request.estimated_row_count = 1;
  request.option_envelopes = BenchmarkOptions();
  request.option_envelopes.push_back("feature.page_reservation=disabled");
  return api::EngineInsertRows(request);
}

api::EngineUpdateRowsResult UpdateNote(
    const Fixture& fixture,
    const api::EngineRequestContext& context,
    std::string id,
    std::string note) {
  scratchbird::tests::FixtureEngineRequest<api::EngineUpdateRowsRequest> request(
      *fixture.session, context);
  request.target_table.uuid = fixture.table_uuid;
  request.target_table.object_kind = "table";
  request.update_predicate = EqualsPredicate("id", std::move(id));
  request.assignments.push_back({"note", TextValue(std::move(note))});
  request.option_envelopes = BenchmarkOptions();
  return api::EngineUpdateRows(request);
}

api::EngineDeleteRowsResult DeleteById(
    const Fixture& fixture,
    const api::EngineRequestContext& context,
    std::string id) {
  scratchbird::tests::FixtureEngineRequest<api::EngineDeleteRowsRequest> request(
      *fixture.session, context);
  request.target_table.uuid = fixture.table_uuid;
  request.target_table.object_kind = "table";
  request.delete_predicate = EqualsPredicate("id", std::move(id));
  request.option_envelopes = BenchmarkOptions();
  return api::EngineDeleteRows(request);
}

api::EngineExecuteImportRowsResult ImportRows(
    const Fixture& fixture,
    const api::EngineRequestContext& context,
    std::vector<api::EngineRowValue> rows) {
  scratchbird::tests::FixtureEngineRequest<api::EngineExecuteImportRowsRequest> request(
      *fixture.session, context);
  request.target_table.uuid = fixture.table_uuid;
  request.target_table.object_kind = "table";
  request.source.source_kind = "csv_stream";
  request.source.source_position = "row:0";
  request.format.format_family = "csv";
  request.import_policy.reject_mode = "fail_fast";
  request.import_policy.reject_payload_policy = "diagnostic_only";
  request.import_policy.resume_policy = "fail_closed";
  request.canonical_rows = std::move(rows);
  request.estimated_row_count =
      static_cast<api::EngineApiU64>(request.canonical_rows.size());
  request.option_envelopes = BenchmarkOptions();
  request.option_envelopes.push_back("copy_append_batching=enabled");
  request.option_envelopes.push_back("copy_append_batch_rows=16");
  return api::EngineExecuteImportRows(request);
}

void TestDmlSummaryCounters() {
  auto fixture = MakeFixture();
  auto context = Begin(fixture, "odf038-dml");

  const auto inserted = InsertRows(
      fixture,
      context,
      {Row("insert-1", "seed-1"), Row("insert-2", "seed-2")});
  RequireOk(inserted, "ODF-038 insert failed");
  Require(inserted.inserted_count == 2, "ODF-038 insert count changed");
  Require(inserted.dml_summary.rows_changed == 2,
          "ODF-038 insert rows changed summary mismatch");
  Require(inserted.dml_summary.visible_rows_scanned == 0,
          "ODF-038 insert should expose zero visible row scans");
  Require(inserted.dml_summary.index_probes >= 2,
          "ODF-038 insert index probe summary missing");
  Require(inserted.dml_summary.append_calls >= 2,
          "ODF-038 insert append summary missing");
  Require(inserted.dml_summary.file_opens >= 2,
          "ODF-038 insert file open summary missing");
  Require(inserted.dml_summary.flushes >= 2,
          "ODF-038 insert flush summary missing");
  Require(inserted.dml_summary.page_reservations >= 1,
          "ODF-038 insert page reservation summary missing");
  RequireSummaryEvidence(inserted, "dml.insert_rows");

  const auto updated = UpdateNote(fixture, context, "insert-1", "updated");
  RequireOk(updated, "ODF-038 update failed");
  Require(updated.updated_count == 1, "ODF-038 update count changed");
  Require(updated.dml_summary.rows_changed == 1,
          "ODF-038 update rows changed summary mismatch");
  Require(updated.dml_summary.visible_rows_scanned == 1,
          "ODF-038 update visible rows scanned summary mismatch");
  Require(updated.dml_summary.index_probes >= 1,
          "ODF-038 update index probe summary missing");
  Require(updated.dml_summary.append_calls >= 1,
          "ODF-038 update append summary missing");
  Require(updated.dml_summary.file_opens >= 1,
          "ODF-038 update file open summary missing");
  Require(updated.dml_summary.flushes >= 1,
          "ODF-038 update flush summary missing");
  Require(updated.dml_summary.page_reservations >= 1,
          "ODF-038 update page reservation summary missing");
  RequireSummaryEvidence(updated, "dml.update_rows");

  const auto deleted = DeleteById(fixture, context, "insert-2");
  RequireOk(deleted, "ODF-038 delete failed");
  Require(deleted.deleted_count == 1, "ODF-038 delete count changed");
  Require(deleted.transaction_uuid == context.transaction_uuid &&
              deleted.local_transaction_id == context.local_transaction_id,
          "ODF-038 delete result lost its native transaction identity");
  Require(deleted.dml_summary.rows_changed == 1,
          "ODF-038 delete rows changed summary mismatch");
  Require(deleted.dml_summary.visible_rows_scanned == 1,
          "ODF-038 delete visible rows scanned summary mismatch");
  Require(deleted.dml_summary.index_probes >= 1,
          "ODF-038 delete index probe summary missing");
  Require(deleted.dml_summary.append_calls >= 1,
          "ODF-038 delete append summary missing");
  Require(deleted.dml_summary.file_opens >= 1,
          "ODF-038 delete file open summary missing");
  Require(deleted.dml_summary.flushes >= 1,
          "ODF-038 delete flush summary missing");
  RequireSummaryEvidence(deleted, "dml.delete_rows");

  const auto imported = ImportRows(
      fixture,
      context,
      {Row("import-1", "copy-1"), Row("import-2", "copy-2")});
  RequireOk(imported, "ODF-038 import failed");
  Require(imported.inserted_rows == 2, "ODF-038 import count changed");
  Require(imported.dml_summary.rows_changed == 2,
          "ODF-038 import rows changed summary mismatch");
  Require(imported.dml_summary.visible_rows_scanned == 0,
          "ODF-038 import should expose zero visible row scans");
  Require(imported.dml_summary.index_probes >= 2,
          "ODF-038 import index probe summary missing");
  Require(imported.dml_summary.append_calls >= 2,
          "ODF-038 import append summary missing");
  Require(imported.dml_summary.file_opens >= 2,
          "ODF-038 import file open summary missing");
  Require(imported.dml_summary.flushes >= 2,
          "ODF-038 import flush summary missing");
  Require(imported.dml_summary.page_reservations >= 1,
          "ODF-038 import page reservation summary missing");
  RequireSummaryEvidence(imported, "dml.execute_import_rows");

  const auto refused = RefusedInsertPageReservationDisabled(fixture, context);
  Require(!refused.ok, "ODF-038 refused insert unexpectedly succeeded");
  Require(refused.dml_summary.fallback_reasons.size() > 0,
          "ODF-038 refused insert fallback summary missing");
  Require(HasFallbackReason(refused.dml_summary, "page_reservation_disabled"),
          "ODF-038 refused insert exact fallback reason missing");
  Require(HasEvidence(refused.evidence,
                      "dml_summary.fallback_reason",
                      "page_reservation_disabled"),
          "ODF-038 refused insert fallback reason evidence missing");
  Require(refused.dml_summary.rows_changed == 0,
          "ODF-038 refused insert changed rows");
  Require(refused.dml_summary.visible_rows_scanned == 0,
          "ODF-038 refused insert scanned visible rows");
  Require(refused.dml_summary.index_probes == 0,
          "ODF-038 refused insert probed indexes");
  Require(refused.dml_summary.append_calls == 0,
          "ODF-038 refused insert appended storage");
  Require(refused.dml_summary.file_opens == 0,
          "ODF-038 refused insert opened files");
  Require(refused.dml_summary.flushes == 0,
          "ODF-038 refused insert flushed files");
  Require(refused.dml_summary.page_reservations == 0,
          "ODF-038 refused insert reserved pages");
  RequireSummaryEvidence(refused, "dml.insert_rows");

  Rollback(context);
}

void TestUpdateFromCommittedCreator() {
  auto fixture = MakeFixture();
  auto creator = Begin(fixture, "odf038-committed-creator");
  RequireOk(InsertRows(fixture, creator, {Row("committed", "before")}),
            "ODF-038 committed creator seed failed");
  Commit(creator);
  auto updater = Begin(fixture, "odf038-new-creator");
  Require(updater.transaction_uuid != creator.transaction_uuid,
          "ODF-038 update did not obtain a distinct transaction");
  const auto updated = UpdateNote(fixture, updater, "committed", "after");
  RequireOk(updated, "ODF-038 committed creator update failed");
  Require(updated.updated_count == 1 && updated.transaction_uuid == updater.transaction_uuid &&
              updated.local_transaction_id == updater.local_transaction_id,
          "ODF-038 update creator identity or row count changed");
  const auto loaded = api::LoadMgaRelationStoreStateForMutationTarget(
      updater, fixture.table_uuid);
  Require(loaded.ok, "ODF-038 updated versions could not be loaded");
  bool saw_creator = false, saw_updater = false;
  for (const auto& row : loaded.state.row_versions) {
    if (row.creator_tx == creator.local_transaction_id) {
      Require(row.creator_transaction_uuid == creator.transaction_uuid,
              "ODF-038 decoded committed creator identity changed");
      saw_creator = true;
    }
    if (row.creator_tx == updater.local_transaction_id) {
      Require(row.creator_transaction_uuid == updater.transaction_uuid &&
                  row.sequence > row.previous_sequence && row.previous_sequence != 0,
              "ODF-038 update lost its exact creator or allocated lineage sequence");
      saw_updater = true;
    }
  }
  Require(saw_creator && saw_updater, "ODF-038 creator lineage was not retained");
  Commit(updater);
}

void TestPreparedRowSequenceIdentity() {
  auto fixture = MakeFixture();
  const auto context = Begin(fixture, "odf038-prepared-row-sequence");
  std::vector<api::CrudRowVersionRecord> rows(2);
  for (std::size_t i = 0; i < rows.size(); ++i) {
    auto& row = rows[i];
    row.creator_tx = context.local_transaction_id;
    row.creator_transaction_uuid = context.transaction_uuid;
    row.table_uuid = fixture.table_uuid;
    row.row_uuid = NewNativeUuid(platform::UuidKind::row, 300 + i);
    row.version_uuid = NewNativeUuid(platform::UuidKind::row, 400 + i);
    row.values = {{"id", "prepared-" + std::to_string(i)}, {"note", "value"}};
  }
  api::MgaRelationHotAppendContext append(context);
  std::vector<api::CrudRowVersionRecord*> pointers{&rows[0], &rows[1]};
  Require(!append.PrepareRowVersionSequences(pointers).error,
          "ODF-038 exact sequence reservation failed");
  const auto first = rows[0].sequence;
  Require(first != 0 && rows[1].sequence == first + 1,
          "ODF-038 reservation did not allocate a contiguous range");
  Require(append.PrepareRowVersionSequences(pointers).error,
          "ODF-038 outstanding sequence reservation was replaced");
  std::vector<std::uint64_t> written;
  Require(append.AppendRowVersionsReadOnly(rows).error,
          "ODF-038 read-only append bypassed prepared version identity");
  std::vector<std::vector<std::pair<std::string, std::string>>> values;
  for (const auto& row : rows) values.push_back(row.values);
  Require(append.AppendRowVersions(&rows, &values, &written).error,
          "ODF-038 alternate value append bypassed prepared version identity");
  auto wrong_count = rows;
  wrong_count.pop_back();
  Require(append.AppendRowVersions(&wrong_count, &written).error,
          "ODF-038 partial reservation consumption was admitted");
  auto wrong_identity = rows;
  wrong_identity[0].version_uuid = NewNativeUuid(platform::UuidKind::row, 500);
  Require(append.AppendRowVersions(&wrong_identity, &written).error,
          "ODF-038 substituted version identity was admitted");
  auto wrong_sequence = rows;
  ++wrong_sequence[0].sequence;
  Require(append.AppendRowVersions(&wrong_sequence, &written).error,
          "ODF-038 substituted version sequence was admitted");
  Require(!append.AppendRowVersions(&rows, &written).error &&
              written == std::vector<std::uint64_t>{first, first + 1} &&
              rows[0].sequence == first && rows[1].sequence == first + 1,
          "ODF-038 append reassigned or lost reserved sequence identities");
  Require(!append.FlushRowVersions().error, "ODF-038 prepared row flush failed");
  Rollback(context);
}

}  // namespace

int main() {
  auto policy = scratchbird::core::memory::DefaultLocalEngineMemoryPolicy();
  policy.policy_name = "odf038_statement_fixture";
  Require(scratchbird::core::memory::ConfigureDefaultMemoryManagerForFixture(
              policy, "odf038_statement_fixture").ok(),
          "ODF-038 memory policy configuration failed");
  TestPreparedRowSequenceIdentity();
  TestDmlSummaryCounters();
  TestUpdateFromCommittedCreator();
  return EXIT_SUCCESS;
}

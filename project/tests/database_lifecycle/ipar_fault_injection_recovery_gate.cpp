// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "database_lifecycle.hpp"
#include "dml/import_execution_api.hpp"
#include "dml/insert_api.hpp"
#include "dml/select_api.hpp"
#include "mga_relation_store/mga_relation_store.hpp"
#include "secondary_index_delta_ledger.hpp"
#include "transaction/transaction_api.hpp"
#include "uuid.hpp"

#include <algorithm>
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

constexpr std::string_view kAuthority = "durable_mga_transaction_inventory";

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

platform::u64 NowMillis() {
  return static_cast<platform::u64>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());
}

platform::TypedUuid NewUuid(platform::UuidKind kind, platform::u64 salt) {
  const auto generated = uuid::GenerateEngineIdentityV7(kind, NowMillis() + salt);
  Require(generated.ok(), "IPAR-P7-06 UUID generation failed");
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
  typed.state = api::EngineValueState::value;
  return typed;
}

api::EngineRowValue Row(std::string id, std::string name = "alpha") {
  api::EngineRowValue row;
  row.fields.push_back({"id", TextValue(std::move(id))});
  row.fields.push_back({"name", TextValue(std::move(name))});
  row.fields.push_back({"note", TextValue("ipar-p7-06")});
  return row;
}

std::vector<api::EngineRowValue> Rows(std::string prefix, int count) {
  std::vector<api::EngineRowValue> rows;
  rows.reserve(static_cast<std::size_t>(count));
  for (int index = 0; index < count; ++index) {
    rows.push_back(Row(prefix + "-" + std::to_string(index + 1), "alpha"));
  }
  return rows;
}

bool HasEvidence(const std::vector<api::EngineEvidenceReference>& evidence,
                 std::string_view kind,
                 std::string_view id) {
  for (const auto& item : evidence) {
    if (item.evidence_kind == kind && item.evidence_id == id) {
      return true;
    }
  }
  return false;
}

std::string EvidenceValue(const std::vector<api::EngineEvidenceReference>& evidence,
                          std::string_view kind) {
  for (const auto& item : evidence) {
    if (item.evidence_kind == kind) {
      return item.evidence_id;
    }
  }
  return {};
}

std::string DiagnosticArgumentValue(const platform::DiagnosticRecord& diagnostic,
                                    std::string_view key) {
  for (const auto& argument : diagnostic.arguments) {
    if (argument.key == key) {
      return argument.value;
    }
  }
  return {};
}

template <typename TResult>
void RequireFault(const TResult& result,
                  std::string_view diagnostic_code,
                  std::string_view point) {
  Require(!result.ok, "IPAR-P7-06 injected fault unexpectedly succeeded");
  Require(!result.diagnostics.empty(), "IPAR-P7-06 fault diagnostic missing");
  Require(result.diagnostics.front().code == diagnostic_code,
          "IPAR-P7-06 fault diagnostic changed");
  Require(HasEvidence(result.evidence, "ipar_fault_injection_point", point),
          "IPAR-P7-06 fault point evidence missing");
  Require(HasEvidence(result.evidence, "ipar_fault_injection_authority", kAuthority),
          "IPAR-P7-06 durable MGA authority evidence missing");
  Require(HasEvidence(result.evidence, "ipar_fault_injection_wal_authority", "false"),
          "IPAR-P7-06 WAL non-authority evidence missing");
  Require(HasEvidence(result.evidence, "ipar_fault_injection_parser_finality", "false"),
          "IPAR-P7-06 parser non-finality evidence missing");
}

std::vector<std::string> DeferredOptions() {
  return {"runtime.deferred_secondary_index=enabled",
          "feature.secondary_index_delta_ledger=enabled",
          "delta_ledger.reader_overlay=enabled",
          "delta_ledger.cleanup_horizon_bound=true",
          "delta_ledger.recovery_classifiable=true"};
}

struct Fixture {
  std::filesystem::path dir;
  std::filesystem::path database_path;
  std::string database_uuid;
  std::string schema_uuid;
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
  context.current_schema_uuid.canonical = fixture.schema_uuid;
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
  RequireOk(begun, "IPAR-P7-06 begin transaction failed");
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
  RequireOk(api::EngineCommitTransaction(request), "IPAR-P7-06 commit failed");
}

void Rollback(const api::EngineRequestContext& context) {
  api::EngineRollbackTransactionRequest request;
  request.context = context;
  RequireOk(api::EngineRollbackTransaction(request), "IPAR-P7-06 rollback failed");
}

api::CrudTableRecord Table(const Fixture& fixture,
                           const api::EngineRequestContext& context) {
  api::CrudTableRecord table;
  table.creator_tx = context.local_transaction_id;
  table.table_uuid = fixture.table_uuid;
  table.default_name = "ipar_fault_injection_recovery";
  table.columns.push_back({"id", "canonical=character;not_null=true"});
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
  index.default_name = unique ? "ipar_p706_id_uq" : "ipar_p706_name_idx";
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
                ("scratchbird_ipar_p706_" + name + "_" +
                 std::to_string(NowMillis() + salt));
  std::filesystem::create_directories(fixture.dir);
  fixture.database_path = fixture.dir / "ipar_p706.sbdb";

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
  Require(created.ok(), "IPAR-P7-06 database create failed");

  fixture.database_uuid = uuid::UuidToString(create.database_uuid.value);
  fixture.schema_uuid = NewUuidText(platform::UuidKind::object, salt + 10);
  fixture.table_uuid = NewUuidText(platform::UuidKind::object, salt + 11);
  fixture.non_unique_index_uuid = NewUuidText(platform::UuidKind::object, salt + 12);
  fixture.unique_index_uuid = NewUuidText(platform::UuidKind::object, salt + 13);

  auto context = Begin(fixture, "ipar-p706-metadata");
  const auto table = api::AppendMgaTableMetadata(context, Table(fixture, context));
  Require(!table.error, "IPAR-P7-06 table metadata append failed");
  const auto non_unique = api::AppendMgaIndexMetadata(
      context,
      Index(fixture, context, fixture.non_unique_index_uuid, "name", false));
  Require(!non_unique.error, "IPAR-P7-06 non-unique index metadata append failed");
  const auto unique = api::AppendMgaIndexMetadata(
      context,
      Index(fixture, context, fixture.unique_index_uuid, "id", true));
  Require(!unique.error, "IPAR-P7-06 unique index metadata append failed");
  Commit(context);
  return fixture;
}

api::EngineInsertRowsRequest InsertRequest(const Fixture& fixture,
                                           const api::EngineRequestContext& context,
                                           std::vector<api::EngineRowValue> rows,
                                           std::vector<std::string> options = {}) {
  api::EngineInsertRowsRequest request;
  request.context = context;
  request.target_schema.uuid.canonical = fixture.schema_uuid;
  request.target_table.uuid.canonical = fixture.table_uuid;
  request.target_table.object_kind = "table";
  request.target_object.uuid.canonical = fixture.table_uuid;
  request.target_object.object_kind = "table";
  request.bound_object_identity.object_uuid = request.target_table.uuid;
  request.bound_object_identity.catalog_generation_id =
      request.context.catalog_generation_id;
  request.bound_object_identity.security_epoch = request.context.security_epoch;
  request.bound_object_identity.resource_epoch = request.context.resource_epoch;
  request.estimated_row_count = static_cast<api::EngineApiU64>(rows.size());
  request.input_rows = std::move(rows);
  request.option_envelopes = std::move(options);
  return request;
}

api::EngineExecuteImportRowsRequest ImportRequest(
    const Fixture& fixture,
    const api::EngineRequestContext& context,
    std::vector<api::EngineRowValue> rows,
    std::vector<std::string> options) {
  api::EngineExecuteImportRowsRequest request;
  request.context = context;
  request.target_table.uuid.canonical = fixture.table_uuid;
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
  request.option_envelopes = std::move(options);
  return request;
}

api::EngineApiU64 SelectCount(const Fixture& fixture) {
  auto context = Begin(fixture, "ipar-p706-select-count");
  api::EngineSelectRowsRequest request;
  request.context = context;
  request.source_object.uuid.canonical = fixture.table_uuid;
  request.source_object.object_kind = "table";
  request.select_projection.canonical_projection_envelopes.push_back("id");
  const auto selected = api::EngineSelectRows(request);
  RequireOk(selected, "IPAR-P7-06 select count failed");
  Rollback(context);
  return selected.visible_count;
}

api::EngineSelectRowsResult SelectEquals(const Fixture& fixture,
                                         std::string column,
                                         std::string value) {
  auto context = Begin(fixture, "ipar-p706-select-equals");
  api::EngineSelectRowsRequest request;
  request.context = context;
  request.source_object.uuid.canonical = fixture.table_uuid;
  request.source_object.object_kind = "table";
  request.select_predicate.predicate_kind = "column_equals";
  request.select_predicate.canonical_predicate_envelope = std::move(column);
  request.select_predicate.bound_values.push_back(TextValue(std::move(value)));
  const auto selected = api::EngineSelectRows(request);
  Rollback(context);
  return selected;
}

void VerifyRollbackLeavesNoVisibleRows(const Fixture& fixture) {
  Require(SelectCount(fixture) == 0,
          "IPAR-P7-06 rollback did not remove uncommitted rows from MGA visibility");
}

void TestRowAppendFault() {
  auto fixture = MakeFixture("row_append", 1000);
  auto context = Begin(fixture, "ipar-p706-row-append");
  const auto result = api::EngineInsertRows(InsertRequest(
      fixture,
      context,
      Rows("row-append", 1),
      {"ipar.fault_injection.point=row_append"}));
  RequireFault(result, "SB-IPAR-P7-06-ROW-APPEND-INJECTED", "row_append");
  Rollback(context);
  VerifyRollbackLeavesNoVisibleRows(fixture);
}

void TestIndexAppendFault() {
  auto fixture = MakeFixture("index_append", 2000);
  auto context = Begin(fixture, "ipar-p706-index-append");
  const auto result = api::EngineInsertRows(InsertRequest(
      fixture,
      context,
      Rows("index-append", 1),
      {"ipar.fault_injection.point=index_append"}));
  RequireFault(result, "SB-IPAR-P7-06-INDEX-APPEND-INJECTED", "index_append");
  Rollback(context);
  VerifyRollbackLeavesNoVisibleRows(fixture);
}

void TestCopyBatchFault() {
  auto fixture = MakeFixture("copy_batch", 3000);
  auto context = Begin(fixture, "ipar-p706-copy-batch");
  const auto result = api::EngineExecuteImportRows(ImportRequest(
      fixture,
      context,
      Rows("copy-batch", 4),
      {"copy_append_batching=enabled",
       "copy_append_adaptive_batching=true",
       "copy_append_batch_rows=2",
       "ipar.fault_injection.point=copy_batch",
       "ipar.fault_injection.after_batches=1"}));
  RequireFault(result, "SB-IPAR-P7-06-COPY-BATCH-INJECTED", "copy_batch");
  Require(result.inserted_rows != 0,
          "IPAR-P7-06 COPY batch fault did not occur after a published batch");
  Rollback(context);
  VerifyRollbackLeavesNoVisibleRows(fixture);
}

void TestDiskPreallocationFault() {
  auto fixture = MakeFixture("disk_preallocation", 4000);
  auto context = Begin(fixture, "ipar-p706-disk-preallocation");
  const auto result = api::EngineInsertRows(InsertRequest(
      fixture,
      context,
      Rows("disk-preallocation", 1),
      {"page_allocation.runtime=enabled",
       "ipar.fault_injection.point=disk_preallocation"}));
  RequireFault(result,
               "SB-IPAR-P7-06-DISK-PREALLOCATION-INJECTED",
               "disk_preallocation");
  Rollback(context);
  VerifyRollbackLeavesNoVisibleRows(fixture);
}

void TestCommitFenceFault() {
  auto fixture = MakeFixture("commit_fence", 5000);
  auto context = Begin(fixture, "ipar-p706-commit-fence");
  RequireOk(api::EngineInsertRows(InsertRequest(
                fixture,
                context,
                Rows("commit-fence", 1))),
            "IPAR-P7-06 commit-fence seed insert failed");
  api::EngineCommitTransactionRequest commit;
  commit.context = context;
  commit.option_envelopes.push_back("ipar.fault_injection.point=commit_fence");
  const auto result = api::EngineCommitTransaction(commit);
  RequireFault(result, "SB-IPAR-P7-06-COMMIT-FENCE-INJECTED", "commit_fence");
  Require(result.commit_finality_state == "refused_before_inventory_commit",
          "IPAR-P7-06 commit fence finality state changed");
  Rollback(context);
  VerifyRollbackLeavesNoVisibleRows(fixture);
}

idx::PersistentSecondaryIndexDeltaLedger LoadLedger(const Fixture& fixture) {
  const auto loaded = api::LoadMgaSecondaryIndexDeltaLedger(
      BaseContext(fixture, "ipar-p706-load-ledger"));
  Require(loaded.ok, "IPAR-P7-06 delta ledger load failed");
  return loaded.ledger;
}

platform::u64 MaxLedgerLocalTransactionId(
    const idx::PersistentSecondaryIndexDeltaLedger& ledger) {
  platform::u64 max_tx = 0;
  for (const auto& record : ledger.records) {
    max_tx = std::max(max_tx, record.delta.local_transaction_id);
  }
  return max_tx;
}

void TestSecondaryMergeFault() {
  auto fixture = MakeFixture("secondary_merge", 6000);
  auto writer = Begin(fixture, "ipar-p706-secondary-merge-seed");
  auto options = DeferredOptions();
  RequireOk(api::EngineInsertRows(InsertRequest(
                fixture,
                writer,
                {Row("secondary-merge-id", "merge-key")},
                options)),
            "IPAR-P7-06 deferred secondary seed insert failed");
  Commit(writer);

  const auto ledger = LoadLedger(fixture);
  Require(!ledger.records.empty(),
          "IPAR-P7-06 deferred secondary seed did not create a ledger record");
  api::MgaSecondaryIndexDeltaMergeAgentRequest request;
  request.index_uuid = fixture.non_unique_index_uuid;
  request.table_uuid = fixture.table_uuid;
  request.authoritative_cleanup_horizon_local_transaction_id =
      MaxLedgerLocalTransactionId(ledger);
  request.cleanup_horizon_authoritative = true;
  request.max_records_to_scan = 16;
  request.max_records_to_merge = 16;
  request.ipar_fault_injection_point = "secondary_merge";
  const auto result = api::MergeMgaSecondaryIndexDeltasForIndex(
      BaseContext(fixture, "ipar-p706-secondary-merge"),
      request);
  Require(!result.ok, "IPAR-P7-06 secondary merge fault unexpectedly succeeded");
  Require(result.diagnostic.code == "SB-IPAR-P7-06-SECONDARY-MERGE-INJECTED",
          "IPAR-P7-06 secondary merge diagnostic changed");
  Require(HasEvidence(result.evidence, "ipar_fault_injection_point", "secondary_merge"),
          "IPAR-P7-06 secondary merge fault point evidence missing");
  Require(HasEvidence(result.evidence, "ipar_fault_injection_authority", kAuthority),
          "IPAR-P7-06 secondary merge authority evidence missing");

  const auto selected = SelectEquals(fixture, "name", "merge-key");
  RequireOk(selected, "IPAR-P7-06 select after secondary merge refusal failed");
  Require(selected.visible_count == 1,
          "IPAR-P7-06 secondary merge refusal lost committed delta visibility");
}

void TestRestartClassificationFault() {
  auto fixture = MakeFixture("restart_classification", 7000);
  db::DatabaseOpenConfig open;
  open.path = fixture.database_path.string();
  open.ipar_fault_injection_point = "restart_classification";
  const auto result = db::OpenDatabaseFile(open);
  Require(!result.ok(),
          "IPAR-P7-06 restart classification fault unexpectedly succeeded");
  Require(result.diagnostic.diagnostic_code ==
              "SB-IPAR-P7-06-RESTART-CLASSIFICATION-INJECTED",
          "IPAR-P7-06 restart classification diagnostic changed");
  Require(DiagnosticArgumentValue(result.diagnostic, "detail")
                  .find("authority=durable_mga_transaction_inventory") !=
              std::string::npos,
          "IPAR-P7-06 restart classification authority detail missing");
}

}  // namespace

int main() {
  TestRowAppendFault();
  TestIndexAppendFault();
  TestCopyBatchFault();
  TestDiskPreallocationFault();
  TestCommitFenceFault();
  TestSecondaryMergeFault();
  TestRestartClassificationFault();
  return EXIT_SUCCESS;
}

// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "database_lifecycle.hpp"
#include "database_lifecycle_test_memory.hpp"
#include "dml/import_api.hpp"
#include "dml/import_execution_api.hpp"
#include "dml/insert_api.hpp"
#include "dml/select_api.hpp"
#include "mga_relation_store/mga_relation_store.hpp"
#include "sblr_dispatch.hpp"
#include "sblr_engine_envelope.hpp"
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
namespace dl_test = scratchbird::tests::database_lifecycle;
namespace platform = scratchbird::core::platform;
namespace sblr = scratchbird::engine::sblr;
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
                << result.diagnostics.front().detail << '\n';
    }
    Fail(message);
  }
}

platform::u64 MillisSeed() {
  return static_cast<platform::u64>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());
}

platform::TypedUuid NewUuid(platform::UuidKind kind, platform::u64 salt) {
  const auto generated = uuid::GenerateEngineIdentityV7(kind, MillisSeed() + salt);
  Require(generated.ok(), "CDP-011 UUID generation failed");
  return generated.value;
}

std::string NewUuidText(platform::UuidKind kind, platform::u64 salt) {
  return uuid::UuidToString(NewUuid(kind, salt).value);
}

struct Fixture {
  std::filesystem::path dir;
  std::filesystem::path database_path;
  std::string database_uuid;
  std::string table_uuid;
  std::string index_uuid;
  platform::u64 salt = 0;

  ~Fixture() {
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
    if (item.evidence_kind == kind && item.evidence_id == id) {
      return true;
    }
  }
  return false;
}

std::string FieldValue(const api::EngineResultShape& result,
                       std::size_t row_index,
                       std::string_view field_name) {
  Require(row_index < result.rows.size(), "CDP-011 result row index out of range");
  for (const auto& [name, value] : result.rows[row_index].fields) {
    if (name == field_name) {
      return value.encoded_value;
    }
  }
  return {};
}

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
  RequireOk(begun, "CDP-011 begin transaction failed");
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
  RequireOk(api::EngineCommitTransaction(request), "CDP-011 commit failed");
}

void Rollback(const api::EngineRequestContext& context) {
  api::EngineRollbackTransactionRequest request;
  request.context = context;
  RequireOk(api::EngineRollbackTransaction(request), "CDP-011 rollback failed");
}

api::CrudTableRecord Table(const Fixture& fixture,
                           const api::EngineRequestContext& context) {
  api::CrudTableRecord table;
  table.creator_tx = context.local_transaction_id;
  table.table_uuid = fixture.table_uuid;
  table.default_name = "cdp_copy_append_batching";
  table.columns.push_back({"id", "canonical=character;primary_key=true"});
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

Fixture MakeFixture(std::string name, platform::u64 salt) {
  Fixture fixture;
  fixture.salt = salt;
  fixture.dir = std::filesystem::temp_directory_path() /
                ("scratchbird_cdp011_" + name + "_" + std::to_string(MillisSeed() + salt));
  std::filesystem::create_directories(fixture.dir);
  fixture.database_path = fixture.dir / "cdp011.sbdb";

  db::DatabaseCreateConfig create;
  create.path = fixture.database_path.string();
  create.database_uuid = NewUuid(platform::UuidKind::database, salt + 1);
  create.filespace_uuid = NewUuid(platform::UuidKind::filespace, salt + 2);
  create.creation_unix_epoch_millis = MillisSeed() + salt + 3;
  create.require_resource_seed_pack = false;
  create.allow_minimal_resource_bootstrap = true;
  create.allow_overwrite = true;
  const auto created = db::CreateDatabaseFile(create);
  if (!created.ok()) {
    std::cerr << created.diagnostic.diagnostic_code << ':'
              << created.diagnostic.message_key << '\n';
  }
  Require(created.ok(), "CDP-011 database create failed");

  fixture.database_uuid = uuid::UuidToString(create.database_uuid.value);
  fixture.table_uuid = NewUuidText(platform::UuidKind::object, salt + 10);
  fixture.index_uuid = NewUuidText(platform::UuidKind::object, salt + 11);

  auto metadata = Begin(fixture, "cdp011-metadata");
  const auto table = api::AppendMgaTableMetadata(metadata, Table(fixture, metadata));
  Require(!table.error, "CDP-011 table metadata append failed");
  const auto index = api::AppendMgaIndexMetadata(metadata, UniqueIdIndex(fixture, metadata));
  Require(!index.error, "CDP-011 unique index metadata append failed");
  Commit(metadata);
  return fixture;
}

api::EngineExecuteImportRowsRequest ImportRequest(
    const Fixture& fixture,
    const api::EngineRequestContext& context,
    std::vector<api::EngineRowValue> rows,
    std::vector<std::string> options,
    std::string reject_mode = "fail_fast") {
  api::EngineExecuteImportRowsRequest request;
  request.context = context;
  request.target_table.uuid.canonical = fixture.table_uuid;
  request.target_table.object_kind = "table";
  request.source.source_kind = "csv_stream";
  request.source.source_position = "row:0";
  request.format.format_family = "csv";
  request.import_policy.reject_mode = std::move(reject_mode);
  request.import_policy.reject_payload_policy = "diagnostic_only";
  request.import_policy.resume_policy = "fail_closed";
  if (request.import_policy.reject_mode != "fail_fast") {
    request.import_policy.reject_limit_rows = 10;
  }
  request.canonical_rows = std::move(rows);
  request.estimated_row_count = static_cast<api::EngineApiU64>(request.canonical_rows.size());
  request.option_envelopes = std::move(options);
  return request;
}

api::EnginePlanImportRowsRequest PlanRequest(
    const Fixture& fixture,
    const api::EngineRequestContext& context) {
  api::EnginePlanImportRowsRequest request;
  request.context = context;
  request.target_table.uuid.canonical = fixture.table_uuid;
  request.target_table.object_kind = "table";
  request.source.source_kind = "csv_stream";
  request.source.source_position = "row:0";
  request.format.format_family = "csv";
  request.column_mappings.push_back({"id", "id", {}, true});
  request.column_mappings.push_back({"note", "note", {}, false});
  request.import_policy.reject_mode = "fail_fast";
  request.import_policy.reject_payload_policy = "diagnostic_only";
  request.import_policy.resume_policy = "fail_closed";
  return request;
}

std::vector<api::EngineRowValue> Rows(std::string prefix, int count) {
  std::vector<api::EngineRowValue> rows;
  rows.reserve(static_cast<std::size_t>(count));
  for (int index = 0; index < count; ++index) {
    rows.push_back(Row(prefix + "-id-" + std::to_string(index + 1),
                       prefix + "-note-" + std::to_string(index + 1)));
  }
  return rows;
}

api::EngineApiU64 SelectCount(const Fixture& fixture,
                              const api::EngineRequestContext& context) {
  api::EngineSelectRowsRequest request;
  request.context = context;
  request.source_object.uuid.canonical = fixture.table_uuid;
  request.source_object.object_kind = "table";
  request.select_projection.canonical_projection_envelopes.push_back("id");
  const auto selected = api::EngineSelectRows(request);
  RequireOk(selected, "CDP-011 select failed");
  return selected.visible_count;
}

sblr::SblrOperand Operand(std::string type, std::string name, std::string value) {
  sblr::SblrOperand operand;
  operand.type = std::move(type);
  operand.name = std::move(name);
  operand.value = std::move(value);
  return operand;
}

sblr::SblrOperationEnvelope ExecuteImportEnvelope(const Fixture& fixture) {
  auto envelope = sblr::MakeSblrEnvelope("dml.execute_import_rows",
                                         "SBLR_DML_EXECUTE_IMPORT_ROWS",
                                         "CDP-011-SBLR-EXECUTE-IMPORT");
  envelope.parser_package_uuid = NewUuidText(platform::UuidKind::object, 7000);
  envelope.registry_snapshot_uuid = NewUuidText(platform::UuidKind::object, 7001);
  envelope.parser_resolved_names_to_uuids = true;
  envelope.requires_security_context = true;
  envelope.requires_transaction_context = true;
  envelope.requires_cluster_authority = false;
  envelope.result_shape = "engine_api_result";
  envelope.diagnostic_shape = "engine_api_diagnostic_vector";
  envelope.operands.push_back(Operand("text", "target_object_uuid", fixture.table_uuid));
  envelope.operands.push_back(Operand("text", "target_object_kind", "table"));
  envelope.operands.push_back(Operand("text", "source_kind", "csv_stream"));
  envelope.operands.push_back(Operand("text", "format_family", "csv"));
  envelope.operands.push_back(Operand("text", "reject_mode", "fail_fast"));
  envelope.operands.push_back(Operand("text", "checkpoint_mode", "disabled"));
  envelope.operands.push_back(Operand("text", "estimated_row_count", "2"));
  return envelope;
}

api::EngineApiRequest SblrApiRequest(const Fixture& fixture,
                                     std::vector<api::EngineRowValue> rows) {
  api::EngineApiRequest request;
  request.target_object.uuid.canonical = fixture.table_uuid;
  request.target_object.object_kind = "table";
  request.rows = std::move(rows);
  return request;
}

void SeedCommittedRow(const Fixture& fixture, std::string id, std::string note) {
  auto context = Begin(fixture, "cdp011-seed");
  api::EngineInsertRowsRequest request;
  request.context = context;
  request.target_table.uuid.canonical = fixture.table_uuid;
  request.target_table.object_kind = "table";
  request.input_rows.push_back(Row(std::move(id), std::move(note)));
  request.estimated_row_count = 1;
  const auto inserted = api::EngineInsertRows(request);
  RequireOk(inserted, "CDP-011 seed insert failed");
  Commit(context);
}

void TestEnabledAndDisabledBatchingProduceSameRows() {
  auto enabled_fixture = MakeFixture("enabled", 1000);
  auto enabled_context = Begin(enabled_fixture, "cdp011-enabled");
  auto enabled = api::EngineExecuteImportRows(ImportRequest(
      enabled_fixture,
      enabled_context,
      Rows("enabled", 6),
      {"copy_append_batching=enabled", "copy_append_batch_rows=2"}));
  RequireOk(enabled, "CDP-011 enabled COPY append batch failed");
  Require(enabled.inserted_rows == 6 && enabled.accepted_rows == 6,
          "CDP-011 enabled COPY row count mismatch");
  Require(HasEvidence(enabled.evidence, "copy_append_batching", "enabled"),
          "CDP-011 enabled evidence missing");
  Require(HasEvidence(enabled.evidence, "copy_append_batch_count", "1"),
          "CDP-011 enabled path did not execute as one engine append batch");
  Require(HasEvidence(enabled.evidence, "copy_append_batch_rows", "6"),
          "CDP-011 enabled path did not report actual batch rows");
  Require(SelectCount(enabled_fixture, enabled_context) == 6,
          "CDP-011 enabled rows not visible inside writer transaction");
  Rollback(enabled_context);

  auto disabled_fixture = MakeFixture("disabled", 2000);
  auto disabled_context = Begin(disabled_fixture, "cdp011-disabled");
  auto disabled = api::EngineExecuteImportRows(ImportRequest(
      disabled_fixture,
      disabled_context,
      Rows("disabled", 6),
      {"copy_append_batching=disabled"}));
  RequireOk(disabled, "CDP-011 disabled COPY singleton baseline failed");
  Require(disabled.inserted_rows == enabled.inserted_rows &&
              disabled.accepted_rows == enabled.accepted_rows,
          "CDP-011 disabled baseline changed row counts");
  Require(HasEvidence(disabled.evidence, "copy_append_batching", "disabled"),
          "CDP-011 disabled evidence missing");
  Require(HasEvidence(disabled.evidence, "copy_append_batch_count", "6"),
          "CDP-011 disabled path did not execute singleton baseline");
  Require(HasEvidence(disabled.evidence, "copy_append_batch_rows", "1"),
          "CDP-011 disabled path did not report singleton batch rows");
  Require(SelectCount(disabled_fixture, disabled_context) == 6,
          "CDP-011 disabled rows not visible inside writer transaction");
  Rollback(disabled_context);
}

void TestPlanContractIsCompleteAndExecutionBound() {
  auto fixture = MakeFixture("plan_contract", 2500);
  auto context = Begin(fixture, "cdp011-plan-contract");
  const auto plan = api::EnginePlanImportRows(PlanRequest(fixture, context));
  RequireOk(plan, "CDP-011 import plan failed");
  Require(plan.surface_accepted, "CDP-011 import plan did not accept surface");
  Require(plan.planning_only, "CDP-011 import plan did not declare planning-only contract");
  Require(plan.execution_requires_execute_import_rows,
          "CDP-011 import plan did not bind execution entrypoint");
  Require(!plan.row_execution_completed,
          "CDP-011 import plan claimed row execution completion");
  Require(HasEvidence(plan.evidence, "import_plan_contract", "complete_planning_only"),
          "CDP-011 import plan contract evidence missing");
  Require(HasEvidence(plan.evidence, "import_execution_entrypoint", "dml.execute_import_rows"),
          "CDP-011 import plan entrypoint evidence missing");
  Require(HasEvidence(plan.evidence, "import_plan_requires_canonical_rows", "true"),
          "CDP-011 import plan canonical row evidence missing");
  Require(HasEvidence(plan.evidence, "import_plan_row_persistence_claimed", "false"),
          "CDP-011 import plan incorrectly claimed persistence");

  auto invalid = PlanRequest(fixture, context);
  invalid.localized_names.push_back({"en", "primary", "", "unsafe_name", true});
  const auto refused = api::EnginePlanImportRows(invalid);
  Require(!refused.ok, "CDP-011 import plan accepted localized names");
  Require(!refused.surface_accepted,
          "CDP-011 refused import plan still marked surface accepted");
  Require(!refused.execution_requires_execute_import_rows,
          "CDP-011 refused import plan still advertised execution binding");
  Rollback(context);
}

void TestRollbackInvisibilityAndCommittedReopenVisibility() {
  auto rollback_fixture = MakeFixture("rollback", 3000);
  auto rollback_context = Begin(rollback_fixture, "cdp011-rollback-writer");
  const auto rolled = api::EngineExecuteImportRows(ImportRequest(
      rollback_fixture,
      rollback_context,
      Rows("rollback", 4),
      {"copy_append_batching=enabled"}));
  RequireOk(rolled, "CDP-011 rollback import failed before rollback");
  Rollback(rollback_context);

  auto rollback_reader = Begin(rollback_fixture, "cdp011-rollback-reader");
  Require(SelectCount(rollback_fixture, rollback_reader) == 0,
          "CDP-011 rolled-back COPY rows became visible");
  Rollback(rollback_reader);

  auto commit_fixture = MakeFixture("commit_reopen", 4000);
  auto commit_context = Begin(commit_fixture, "cdp011-commit-writer");
  const auto committed = api::EngineExecuteImportRows(ImportRequest(
      commit_fixture,
      commit_context,
      Rows("commit", 5),
      {"copy_append_batching=enabled"}));
  RequireOk(committed, "CDP-011 committed import failed");
  Commit(commit_context);

  const auto opened = db::OpenDatabaseFile({commit_fixture.database_path.string(), false, false, false});
  Require(opened.ok(), "CDP-011 committed database did not reopen");

  auto reopen_reader = Begin(commit_fixture, "cdp011-reopen-reader");
  Require(SelectCount(commit_fixture, reopen_reader) == 5,
          "CDP-011 committed COPY rows were not visible after reopen");
  Rollback(reopen_reader);
}

void TestSblrExecuteImportRowsDispatchesToExecutor() {
  auto fixture = MakeFixture("sblr_execute", 4500);
  auto context = Begin(fixture, "cdp011-sblr-execute");
  sblr::SblrDispatchRequest dispatch;
  dispatch.context = context;
  dispatch.envelope = ExecuteImportEnvelope(fixture);
  dispatch.api_request = SblrApiRequest(fixture, Rows("sblr", 2));
  const auto result = sblr::DispatchSblrOperation(dispatch);
  if (!(result.accepted && result.envelope_validated && result.dispatched_to_api &&
        result.api_result.ok)) {
    if (!result.diagnostics.empty()) {
      std::cerr << result.diagnostics.front().code << ':'
                << result.diagnostics.front().message << '\n';
    }
    if (!result.api_result.diagnostics.empty()) {
      std::cerr << result.api_result.diagnostics.front().code << ':'
                << result.api_result.diagnostics.front().detail << '\n';
    }
    Fail("CDP-011 SBLR execute import dispatch failed");
  }
  Require(result.api_result.operation_id == "dml.execute_import_rows",
          "CDP-011 SBLR import dispatched to wrong operation");
  Require(HasEvidence(result.api_result.evidence, "import_plan_consumed", "true"),
          "CDP-011 SBLR import did not consume plan");
  Require(HasEvidence(result.api_result.evidence,
                      "import_execution_row_execution_completed",
                      "true"),
          "CDP-011 SBLR import did not complete execution");
  Require(HasEvidence(result.api_result.evidence, "parser_finality_authority", "false"),
          "CDP-011 SBLR import parser finality evidence missing");
  Require(SelectCount(fixture, context) == 2,
          "CDP-011 SBLR import rows were not visible in writer transaction");
  Rollback(context);
}

void TestRejectModeFallsBackWithoutLosingGoodRows() {
  auto fixture = MakeFixture("reject", 5000);
  SeedCommittedRow(fixture, "duplicate-id", "seed");

  auto context = Begin(fixture, "cdp011-reject");
  std::vector<api::EngineRowValue> rows;
  rows.push_back(Row("new-id-1", "accepted-a"));
  rows.push_back(Row("duplicate-id", "rejected-duplicate"));
  rows.push_back(Row("new-id-2", "accepted-b"));

  const auto result = api::EngineExecuteImportRows(ImportRequest(
      fixture,
      context,
      std::move(rows),
      {"copy_append_batching=enabled", "copy_append_batch_rows=8"},
      "reject_row"));
  RequireOk(result, "CDP-011 reject-row COPY import failed");
  Require(result.accepted_rows == 2 && result.inserted_rows == 2 && result.rejected_rows == 1,
          "CDP-011 reject-row counts mismatch");
  Require(HasEvidence(result.evidence, "copy_append_singleton_fallback_batches", "1"),
          "CDP-011 reject-row path did not record singleton fallback");
  Require(HasEvidence(result.evidence, "import_reject_materialization", "result_shape"),
          "CDP-011 reject-row path did not materialize reject diagnostic");
  Require(result.result_shape.rows.size() == 3,
          "CDP-011 reject-row result should include two accepted rows and one diagnostic row");
  const std::string duplicate_code = FieldValue(result.result_shape, 1, "diagnostic_code");
  Require(duplicate_code == "CLI.CONSTRAINT_PRIMARY_KEY_VIOLATION" ||
              duplicate_code == "SB_ENGINE_API_INVALID_REQUEST",
          "CDP-011 reject-row diagnostic code mismatch");
  const std::string duplicate_detail = FieldValue(result.result_shape, 1, "diagnostic_detail");
  Require(duplicate_detail.find("duplicate_key") != std::string::npos ||
              duplicate_detail.find("unique_index_duplicate") != std::string::npos,
          "CDP-011 reject-row diagnostic detail did not describe duplicate key");
  Rollback(context);
}

}  // namespace

int main() {
  dl_test::ConfigureLifecycleMemoryFixture("cdp_copy_append_batching_gate");
  TestEnabledAndDisabledBatchingProduceSameRows();
  TestPlanContractIsCompleteAndExecutionBound();
  TestRollbackInvisibilityAndCommittedReopenVisibility();
  TestSblrExecuteImportRowsDispatchesToExecutor();
  TestRejectModeFallsBackWithoutLosingGoodRows();
  return EXIT_SUCCESS;
}

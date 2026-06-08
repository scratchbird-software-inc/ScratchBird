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
#include "transaction/transaction_api.hpp"
#include "uuid.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <span>
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
  Require(generated.ok(), "ODF-036 UUID generation failed");
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

bool HasDiagnosticDetailContaining(const std::vector<api::EngineApiDiagnostic>& diagnostics,
                                   std::string_view detail) {
  for (const auto& diagnostic : diagnostics) {
    if (diagnostic.detail.find(detail) != std::string::npos) {
      return true;
    }
  }
  return false;
}

void RequireBorrowedWindowEvidence(const api::EngineExecuteImportRowsResult& result,
                                   std::string_view expected_windows) {
  Require(HasEvidence(result.evidence, "import_row_window_route", "borrowed_span"),
          "ODF-036 import did not use borrowed row window route");
  Require(HasEvidence(result.evidence, "import_row_vector_copies", "0"),
          "ODF-036 import reported canonical row vector copies");
  Require(HasEvidence(result.evidence, "import_row_window_count", expected_windows),
          "ODF-036 import row window count mismatch");
  for (const auto& item : result.evidence) {
    const std::string combined = item.evidence_kind + ":" + item.evidence_id;
    Require(combined.find("docs" "/execution-plans") == std::string::npos,
            "ODF-036 evidence leaked private execution_plan token");
    Require(combined.find("findings") == std::string::npos,
            "ODF-036 evidence leaked findings token");
    Require(combined.find("contracts") == std::string::npos,
            "ODF-036 evidence leaked contracts token");
    Require(combined.find("references") == std::string::npos,
            "ODF-036 evidence leaked references token");
  }
}

api::EngineRequestContext BaseContext(const Fixture& fixture, std::string request_id) {
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
  RequireOk(begun, "ODF-036 begin transaction failed");
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
  RequireOk(api::EngineCommitTransaction(request), "ODF-036 commit failed");
}

void Rollback(const api::EngineRequestContext& context) {
  api::EngineRollbackTransactionRequest request;
  request.context = context;
  RequireOk(api::EngineRollbackTransaction(request), "ODF-036 rollback failed");
}

api::CrudTableRecord Table(const Fixture& fixture,
                           const api::EngineRequestContext& context) {
  api::CrudTableRecord table;
  table.creator_tx = context.local_transaction_id;
  table.table_uuid = fixture.table_uuid;
  table.default_name = "optimizer_deficiency_odf_036";
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

Fixture MakeFixture(std::string_view name, platform::u64 salt) {
  Fixture fixture;
  fixture.salt = salt;
  fixture.dir = std::filesystem::temp_directory_path() /
                ("scratchbird_odf036_" + std::string(name) + "_" +
                 std::to_string(NowMillis() + salt));
  std::filesystem::create_directories(fixture.dir);
  fixture.database_path = fixture.dir / "odf036.sbdb";

  db::DatabaseCreateConfig create;
  create.path = fixture.database_path.string();
  create.database_uuid = NewUuid(platform::UuidKind::database, salt + 1);
  create.filespace_uuid = NewUuid(platform::UuidKind::filespace, salt + 2);
  create.creation_unix_epoch_millis = NowMillis() + salt + 3;
  create.require_resource_seed_pack = false;
  create.allow_minimal_resource_bootstrap = true;
  create.allow_overwrite = true;
  const auto created = db::CreateDatabaseFile(create);
  Require(created.ok(), "ODF-036 database create failed");

  fixture.database_uuid = uuid::UuidToString(create.database_uuid.value);
  fixture.table_uuid = NewUuidText(platform::UuidKind::object, salt + 10);
  fixture.index_uuid = NewUuidText(platform::UuidKind::object, salt + 11);

  auto metadata = Begin(fixture, "odf036-metadata");
  Require(!api::AppendMgaTableMetadata(metadata, Table(fixture, metadata)).error,
          "ODF-036 table metadata append failed");
  Require(!api::AppendMgaIndexMetadata(metadata, UniqueIdIndex(fixture, metadata)).error,
          "ODF-036 unique index metadata append failed");
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
  request.estimated_row_count =
      static_cast<api::EngineApiU64>(request.canonical_rows.size());
  request.option_envelopes = std::move(options);
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
  RequireOk(selected, "ODF-036 select failed");
  return selected.visible_count;
}

void SeedCommittedRow(const Fixture& fixture, std::string id, std::string note) {
  auto context = Begin(fixture, "odf036-seed");
  api::EngineInsertRowsRequest request;
  request.context = context;
  request.target_table.uuid.canonical = fixture.table_uuid;
  request.target_table.object_kind = "table";
  request.input_rows.push_back(Row(std::move(id), std::move(note)));
  request.estimated_row_count = 1;
  RequireOk(api::EngineInsertRows(request), "ODF-036 seed insert failed");
  Commit(context);
}

void TestFailFastBatchedImportUsesBorrowedSpan() {
  auto fixture = MakeFixture("batched", 36000);
  auto context = Begin(fixture, "odf036-batched");
  const auto result = api::EngineExecuteImportRows(ImportRequest(
      fixture,
      context,
      Rows("batched", 6),
      {"copy_append_batching=enabled", "copy_append_batch_rows=2"}));
  RequireOk(result, "ODF-036 batched import failed");
  Require(result.accepted_rows == 6 && result.inserted_rows == 6,
          "ODF-036 batched row counts changed");
  Require(HasEvidence(result.evidence, "copy_append_batching", "enabled"),
          "ODF-036 batched copy append evidence missing");
  Require(HasEvidence(result.evidence, "copy_append_batch_count", "1"),
          "ODF-036 batched copy append count changed");
  RequireBorrowedWindowEvidence(result, "1");
  Require(SelectCount(fixture, context) == 6,
          "ODF-036 batched rows not visible inside writer transaction");
  Rollback(context);
}

void TestDisabledBatchingSingletonsUseBorrowedSpans() {
  auto fixture = MakeFixture("singleton", 37000);
  auto context = Begin(fixture, "odf036-singleton");
  const auto result = api::EngineExecuteImportRows(ImportRequest(
      fixture,
      context,
      Rows("singleton", 4),
      {"copy_append_batching=disabled"}));
  RequireOk(result, "ODF-036 singleton import failed");
  Require(result.accepted_rows == 4 && result.inserted_rows == 4,
          "ODF-036 singleton row counts changed");
  Require(HasEvidence(result.evidence, "copy_append_batching", "disabled"),
          "ODF-036 singleton copy append evidence missing");
  Require(HasEvidence(result.evidence, "copy_append_batch_count", "4"),
          "ODF-036 singleton copy append count changed");
  Require(HasEvidence(result.evidence, "copy_append_batch_rows", "1"),
          "ODF-036 singleton batch row evidence changed");
  RequireBorrowedWindowEvidence(result, "4");
  Require(SelectCount(fixture, context) == 4,
          "ODF-036 singleton rows not visible inside writer transaction");
  Rollback(context);
}

void TestRejectFallbackSingletonsUseBorrowedSpans() {
  auto fixture = MakeFixture("reject", 38000);
  SeedCommittedRow(fixture, "duplicate-id", "seed");

  auto context = Begin(fixture, "odf036-reject");
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
  RequireOk(result, "ODF-036 reject-row import failed");
  Require(result.accepted_rows == 2 && result.inserted_rows == 2 &&
              result.rejected_rows == 1,
          "ODF-036 reject-row counts changed");
  Require(HasEvidence(result.evidence, "copy_append_batching", "enabled"),
          "ODF-036 reject-row copy append evidence missing");
  Require(HasEvidence(result.evidence, "copy_append_singleton_fallback_batches", "1"),
          "ODF-036 reject-row singleton fallback evidence missing");
  RequireBorrowedWindowEvidence(result, "5");
  Require(SelectCount(fixture, context) == 3,
          "ODF-036 reject-row visible count changed");
  Rollback(context);
}

void TestDirectBorrowedInsertUsesEffectiveRowsForEstimate() {
  auto fixture = MakeFixture("direct_borrowed", 39000);
  auto context = Begin(fixture, "odf036-direct-borrowed");
  auto rows = Rows("direct", 260);

  api::EngineInsertRowsRequest request;
  request.context = context;
  request.target_table.uuid.canonical = fixture.table_uuid;
  request.target_table.object_kind = "table";
  request.borrowed_input_rows =
      std::span<const api::EngineRowValue>(rows.data(), rows.size());
  request.estimated_row_count = 0;

  const auto result = api::EngineInsertRows(request);
  RequireOk(result, "ODF-036 direct borrowed insert failed");
  Require(request.input_rows.empty(),
          "ODF-036 direct borrowed insert copied rows into owned input_rows");
  Require(result.inserted_count == rows.size(),
          "ODF-036 direct borrowed insert did not insert all borrowed rows");
  Require(SelectCount(fixture, context) == rows.size(),
          "ODF-036 direct borrowed insert visible count mismatch");
  Require(HasEvidence(result.evidence,
                      "page_reservation",
                      "page_reservation:" + fixture.table_uuid + ":3"),
          "ODF-036 direct borrowed insert did not estimate borrowed row count");
  Rollback(context);
}

void TestOwnedAndBorrowedRowsAreRejected() {
  auto fixture = MakeFixture("ambiguous", 40000);
  auto context = Begin(fixture, "odf036-ambiguous");
  auto borrowed_rows = Rows("ambiguous-borrowed", 2);

  api::EngineInsertRowsRequest request;
  request.context = context;
  request.target_table.uuid.canonical = fixture.table_uuid;
  request.target_table.object_kind = "table";
  request.input_rows.push_back(Row("ambiguous-owned-id", "owned"));
  request.borrowed_input_rows =
      std::span<const api::EngineRowValue>(borrowed_rows.data(), borrowed_rows.size());

  const auto result = api::EngineInsertRows(request);
  Require(!result.ok, "ODF-036 ambiguous owned and borrowed rows were accepted");
  Require(HasDiagnosticDetailContaining(result.diagnostics,
                                        "input_rows_or_borrowed_rows_exclusive"),
          "ODF-036 ambiguous rows diagnostic mismatch");
  Rollback(context);
}

}  // namespace

int main() {
  TestFailFastBatchedImportUsesBorrowedSpan();
  TestDisabledBatchingSingletonsUseBorrowedSpans();
  TestRejectFallbackSingletonsUseBorrowedSpans();
  TestDirectBorrowedInsertUsesEffectiveRowsForEstimate();
  TestOwnedAndBorrowedRowsAreRejected();
  return EXIT_SUCCESS;
}

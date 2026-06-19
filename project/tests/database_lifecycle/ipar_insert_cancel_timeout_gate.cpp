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
  if (!condition) { Fail(message); }
}

template <typename TResult>
void RequireOk(const TResult& result, std::string_view message) {
  if (!result.ok) {
    for (const auto& diagnostic : result.diagnostics) {
      std::cerr << diagnostic.code << ':' << diagnostic.detail << '\n';
    }
    Fail(message);
  }
}

platform::u64 TimeSeed() {
  return static_cast<platform::u64>(
      std::chrono::steady_clock::now().time_since_epoch().count());
}

platform::TypedUuid NewUuid(platform::UuidKind kind, platform::u64 salt) {
  const auto generated =
      uuid::GenerateEngineIdentityV7(kind, 1900000000000ull + salt);
  Require(generated.ok(), "IPAR-P5-05 UUID generation failed");
  return generated.value;
}

std::string NewUuidText(platform::UuidKind kind, platform::u64 salt) {
  return uuid::UuidToString(NewUuid(kind, salt).value);
}

struct Fixture {
  std::filesystem::path dir;
  std::filesystem::path database_path;
  std::string database_uuid;
  std::string schema_uuid;
  std::string table_uuid;
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
  typed.state = api::EngineValueState::value;
  return typed;
}

api::EngineRowValue Row(std::string id, std::string payload) {
  api::EngineRowValue row;
  row.fields.push_back({"id", TextValue(std::move(id))});
  row.fields.push_back({"payload", TextValue(std::move(payload))});
  return row;
}

std::string LargePayload(std::size_t index) {
  std::string payload = "ipar-p5-05-large-payload-" + std::to_string(index) + "-";
  payload.append(10000, static_cast<char>('a' + (index % 20)));
  return payload;
}

std::vector<api::EngineRowValue> Rows(std::string prefix,
                                      std::size_t count,
                                      bool large_payload) {
  std::vector<api::EngineRowValue> rows;
  rows.reserve(count);
  for (std::size_t index = 0; index < count; ++index) {
    rows.push_back(Row(prefix + "-" + std::to_string(index),
                       large_payload ? LargePayload(index) : "payload"));
  }
  return rows;
}

api::CrudTableRecord Table(const Fixture& fixture,
                           const api::EngineRequestContext& context) {
  api::CrudTableRecord table;
  table.creator_tx = context.local_transaction_id;
  table.table_uuid = fixture.table_uuid;
  table.default_name = "ipar_insert_cancel_timeout";
  table.columns.push_back({"id", "canonical=character;not_null=true"});
  table.columns.push_back({"payload", "canonical=character"});
  return table;
}

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
  RequireOk(begun, "IPAR-P5-05 begin transaction failed");
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
  RequireOk(api::EngineCommitTransaction(request), "IPAR-P5-05 commit failed");
}

void Rollback(const api::EngineRequestContext& context) {
  api::EngineRollbackTransactionRequest request;
  request.context = context;
  RequireOk(api::EngineRollbackTransaction(request),
            "IPAR-P5-05 rollback failed");
}

Fixture MakeFixture(std::string_view label, platform::u64 salt) {
  Fixture fixture;
  fixture.salt = salt;
  fixture.dir = std::filesystem::temp_directory_path() /
                ("scratchbird_ipar_p5_05_" + std::string(label) + "_" +
                 std::to_string(salt));
  std::filesystem::create_directories(fixture.dir);
  fixture.database_path = fixture.dir / "ipar_insert_cancel_timeout.sbdb";

  db::DatabaseCreateConfig create;
  create.path = fixture.database_path.string();
  create.database_uuid = NewUuid(platform::UuidKind::database, salt + 1);
  create.filespace_uuid = NewUuid(platform::UuidKind::filespace, salt + 2);
  create.creation_unix_epoch_millis = 1900000000000ull + salt + 3;
  create.page_size = 8192;
  create.require_resource_seed_pack = false;
  create.allow_minimal_resource_bootstrap = true;
  create.allow_overwrite = true;
  const auto created = db::CreateDatabaseFile(create);
  if (!created.ok()) {
    std::cerr << created.diagnostic.diagnostic_code << ':'
              << created.diagnostic.message_key << '\n';
  }
  Require(created.ok(), "IPAR-P5-05 database create failed");

  fixture.database_uuid = uuid::UuidToString(create.database_uuid.value);
  fixture.schema_uuid = NewUuidText(platform::UuidKind::object, salt + 10);
  fixture.table_uuid = NewUuidText(platform::UuidKind::object, salt + 11);

  auto metadata = Begin(fixture, "ipar-p5-05-metadata");
  const auto table = api::AppendMgaTableMetadata(metadata, Table(fixture, metadata));
  Require(!table.error, "IPAR-P5-05 table metadata append failed");
  Commit(metadata);
  return fixture;
}

api::EngineInsertRowsRequest InsertRequest(
    const Fixture& fixture,
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
    std::vector<std::string> options = {}) {
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

api::EngineApiU64 SelectCount(const Fixture& fixture,
                              const api::EngineRequestContext& context) {
  api::EngineSelectRowsRequest request;
  request.context = context;
  request.source_object.uuid.canonical = fixture.table_uuid;
  request.source_object.object_kind = "table";
  request.select_projection.canonical_projection_envelopes.push_back("id");
  const auto selected = api::EngineSelectRows(request);
  RequireOk(selected, "IPAR-P5-05 select failed");
  return selected.visible_count;
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

template <typename TResult>
bool HasDiagnostic(const TResult& result, std::string_view code) {
  for (const auto& diagnostic : result.diagnostics) {
    if (diagnostic.code == code) { return true; }
  }
  return false;
}

void VerifyInsertCancelNoPublication(platform::u64 salt) {
  auto fixture = MakeFixture("insert_cancel", salt);
  auto context = Begin(fixture, "ipar-p5-05-insert-cancel");
  const auto cancelled = api::EngineInsertRows(InsertRequest(
      fixture,
      context,
      Rows("cancelled", 8, true),
      {"execution.cancel_requested=true",
       "execution.control_reason=ipar_p5_05_cancelled"}));
  Require(!cancelled.ok, "IPAR-P5-05 cancelled insert was admitted");
  Require(HasDiagnostic(cancelled, "SB-IPAR-INSERT-CANCELLED"),
          "IPAR-P5-05 cancelled insert diagnostic missing");
  Require(HasEvidence(cancelled.evidence,
                      "insert_execution_control_decision",
                      "refuse"),
          "IPAR-P5-05 cancelled insert refusal evidence missing");
  Require(HasEvidence(cancelled.evidence,
                      "insert_execution_control_rows_published",
                      "0"),
          "IPAR-P5-05 cancelled insert published rows evidence missing");
  Require(SelectCount(fixture, context) == 0,
          "IPAR-P5-05 cancelled insert published rows");

  RequireOk(api::EngineInsertRows(InsertRequest(
                fixture,
                context,
                Rows("after-cancel", 2, false))),
            "IPAR-P5-05 insert after cancel failed");
  Commit(context);

  auto reader = Begin(fixture, "ipar-p5-05-insert-cancel-reader");
  Require(SelectCount(fixture, reader) == 2,
          "IPAR-P5-05 post-cancel transaction boundary failed");
  Rollback(reader);
}

void VerifyInsertTimeoutNoPublication(platform::u64 salt) {
  auto fixture = MakeFixture("insert_timeout", salt);
  auto context = Begin(fixture, "ipar-p5-05-insert-timeout");
  const auto timed_out = api::EngineInsertRows(InsertRequest(
      fixture,
      context,
      Rows("timed-out", 8, true),
      {"execution.timeout_elapsed=true",
       "execution.control_reason=ipar_p5_05_timeout"}));
  Require(!timed_out.ok, "IPAR-P5-05 timed-out insert was admitted");
  Require(HasDiagnostic(timed_out, "SB-IPAR-INSERT-TIMEOUT"),
          "IPAR-P5-05 timed-out insert diagnostic missing");
  Require(HasEvidence(timed_out.evidence,
                      "insert_execution_control_retry_class",
                      "timeout"),
          "IPAR-P5-05 timed-out insert retry class missing");
  Require(SelectCount(fixture, context) == 0,
          "IPAR-P5-05 timed-out insert published rows");
  Rollback(context);
}

void VerifyCopyCancelNoPublication(platform::u64 salt) {
  auto fixture = MakeFixture("copy_cancel", salt);
  auto context = Begin(fixture, "ipar-p5-05-copy-cancel");
  const auto cancelled = api::EngineExecuteImportRows(ImportRequest(
      fixture,
      context,
      Rows("copy-cancelled", 6, true),
      {"copy.cancel_requested=true",
       "copy.control_reason=ipar_p5_05_copy_cancelled"}));
  Require(!cancelled.ok, "IPAR-P5-05 cancelled COPY was admitted");
  Require(HasDiagnostic(cancelled, "SB-IPAR-COPY-CANCELLED"),
          "IPAR-P5-05 cancelled COPY diagnostic missing");
  Require(HasEvidence(cancelled.evidence,
                      "copy_execution_control_decision",
                      "refuse"),
          "IPAR-P5-05 cancelled COPY refusal evidence missing");
  Require(HasEvidence(cancelled.evidence,
                      "copy_execution_control_rows_published",
                      "0"),
          "IPAR-P5-05 cancelled COPY published rows evidence missing");
  Require(SelectCount(fixture, context) == 0,
          "IPAR-P5-05 cancelled COPY published rows");

  RequireOk(api::EngineExecuteImportRows(ImportRequest(
                fixture,
                context,
                Rows("copy-after-cancel", 2, false))),
            "IPAR-P5-05 COPY after cancel failed");
  Commit(context);

  auto reader = Begin(fixture, "ipar-p5-05-copy-cancel-reader");
  Require(SelectCount(fixture, reader) == 2,
          "IPAR-P5-05 post-COPY-cancel transaction boundary failed");
  Rollback(reader);
}

void VerifyCopyTimeoutNoPublication(platform::u64 salt) {
  auto fixture = MakeFixture("copy_timeout", salt);
  auto context = Begin(fixture, "ipar-p5-05-copy-timeout");
  const auto timed_out = api::EngineExecuteImportRows(ImportRequest(
      fixture,
      context,
      Rows("copy-timed-out", 6, true),
      {"copy.timeout_elapsed=true",
       "copy.control_reason=ipar_p5_05_copy_timeout"}));
  Require(!timed_out.ok, "IPAR-P5-05 timed-out COPY was admitted");
  Require(HasDiagnostic(timed_out, "SB-IPAR-COPY-TIMEOUT"),
          "IPAR-P5-05 timed-out COPY diagnostic missing");
  Require(HasEvidence(timed_out.evidence,
                      "copy_execution_control_retry_class",
                      "timeout"),
          "IPAR-P5-05 timed-out COPY retry class missing");
  Require(SelectCount(fixture, context) == 0,
          "IPAR-P5-05 timed-out COPY published rows");
  Rollback(context);
}

}  // namespace

int main() {
  const auto salt = TimeSeed();
  VerifyInsertCancelNoPublication(salt + 100);
  VerifyInsertTimeoutNoPublication(salt + 200);
  VerifyCopyCancelNoPublication(salt + 300);
  VerifyCopyTimeoutNoPublication(salt + 400);
  return EXIT_SUCCESS;
}

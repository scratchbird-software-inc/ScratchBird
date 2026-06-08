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
  Require(generated.ok(), "ODF-037 UUID generation failed");
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

std::string EvidenceValue(const std::vector<api::EngineEvidenceReference>& evidence,
                          std::string_view kind) {
  for (const auto& item : evidence) {
    if (item.evidence_kind == kind) {
      return item.evidence_id;
    }
  }
  return {};
}

api::EngineApiU64 EvidenceU64(const std::vector<api::EngineEvidenceReference>& evidence,
                              std::string_view kind) {
  const std::string value = EvidenceValue(evidence, kind);
  Require(!value.empty(), "ODF-037 expected evidence value missing");
  api::EngineApiU64 parsed = 0;
  for (const unsigned char ch : value) {
    Require(ch >= '0' && ch <= '9', "ODF-037 evidence value was not numeric");
    parsed = parsed * 10 + static_cast<api::EngineApiU64>(ch - '0');
  }
  return parsed;
}

std::string FieldValue(const api::EngineResultShape& result,
                       std::size_t row_index,
                       std::string_view field_name) {
  Require(row_index < result.rows.size(), "ODF-037 result row index out of range");
  for (const auto& [name, value] : result.rows[row_index].fields) {
    if (name == field_name) {
      return value.encoded_value;
    }
  }
  return {};
}

void RequireNoPathLeak(const std::vector<api::EngineEvidenceReference>& evidence) {
  for (const auto& item : evidence) {
    const std::string combined = item.evidence_kind + ":" + item.evidence_id;
    Require(combined.find("docs" "/execution-plans") == std::string::npos,
            "ODF-037 evidence leaked private execution_plan token");
    Require(combined.find("findings") == std::string::npos,
            "ODF-037 evidence leaked findings token");
    Require(combined.find("contracts") == std::string::npos,
            "ODF-037 evidence leaked contracts token");
    Require(combined.find("references") == std::string::npos,
            "ODF-037 evidence leaked references token");
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
  RequireOk(begun, "ODF-037 begin transaction failed");
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
  RequireOk(api::EngineCommitTransaction(request), "ODF-037 commit failed");
}

void Rollback(const api::EngineRequestContext& context) {
  api::EngineRollbackTransactionRequest request;
  request.context = context;
  RequireOk(api::EngineRollbackTransaction(request), "ODF-037 rollback failed");
}

api::CrudTableRecord Table(const Fixture& fixture,
                           const api::EngineRequestContext& context) {
  api::CrudTableRecord table;
  table.creator_tx = context.local_transaction_id;
  table.table_uuid = fixture.table_uuid;
  table.default_name = "optimizer_deficiency_odf_037";
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

Fixture MakeFixture() {
  Fixture fixture;
  fixture.salt = 37000;
  fixture.dir = std::filesystem::temp_directory_path() /
                ("scratchbird_odf037_" + std::to_string(NowMillis() + fixture.salt));
  std::filesystem::create_directories(fixture.dir);
  fixture.database_path = fixture.dir / "odf037.sbdb";

  db::DatabaseCreateConfig create;
  create.path = fixture.database_path.string();
  create.database_uuid = NewUuid(platform::UuidKind::database, fixture.salt + 1);
  create.filespace_uuid = NewUuid(platform::UuidKind::filespace, fixture.salt + 2);
  create.creation_unix_epoch_millis = NowMillis() + fixture.salt + 3;
  create.require_resource_seed_pack = false;
  create.allow_minimal_resource_bootstrap = true;
  create.allow_overwrite = true;
  const auto created = db::CreateDatabaseFile(create);
  Require(created.ok(), "ODF-037 database create failed");

  fixture.database_uuid = uuid::UuidToString(create.database_uuid.value);
  fixture.table_uuid = NewUuidText(platform::UuidKind::object, fixture.salt + 10);
  fixture.index_uuid = NewUuidText(platform::UuidKind::object, fixture.salt + 11);

  auto metadata = Begin(fixture, "odf037-metadata");
  Require(!api::AppendMgaTableMetadata(metadata, Table(fixture, metadata)).error,
          "ODF-037 table metadata append failed");
  Require(!api::AppendMgaIndexMetadata(metadata, UniqueIdIndex(fixture, metadata)).error,
          "ODF-037 unique index metadata append failed");
  Commit(metadata);
  return fixture;
}

api::EngineExecuteImportRowsRequest ImportRequest(
    const Fixture& fixture,
    const api::EngineRequestContext& context,
    std::vector<api::EngineRowValue> rows) {
  api::EngineExecuteImportRowsRequest request;
  request.context = context;
  request.target_table.uuid.canonical = fixture.table_uuid;
  request.target_table.object_kind = "table";
  request.source.source_kind = "csv_stream";
  request.source.source_position = "row:0";
  request.format.format_family = "csv";
  request.import_policy.reject_mode = "reject_row";
  request.import_policy.reject_payload_policy = "diagnostic_only";
  request.import_policy.resume_policy = "fail_closed";
  request.import_policy.reject_limit_rows = 10;
  request.canonical_rows = std::move(rows);
  request.estimated_row_count =
      static_cast<api::EngineApiU64>(request.canonical_rows.size());
  request.option_envelopes = {"copy_append_batching=enabled", "copy_append_batch_rows=8"};
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
  RequireOk(selected, "ODF-037 select failed");
  return selected.visible_count;
}

void SeedCommittedRow(const Fixture& fixture) {
  auto context = Begin(fixture, "odf037-seed");
  api::EngineInsertRowsRequest request;
  request.context = context;
  request.target_table.uuid.canonical = fixture.table_uuid;
  request.target_table.object_kind = "table";
  request.input_rows.push_back(Row("duplicate-id", "seed"));
  request.estimated_row_count = 1;
  RequireOk(api::EngineInsertRows(request), "ODF-037 seed insert failed");
  Commit(context);
}

void TestRejectFallbackUsesBisection() {
  auto fixture = MakeFixture();
  SeedCommittedRow(fixture);

  std::vector<api::EngineRowValue> rows;
  rows.push_back(Row("new-id-1", "accepted-1"));
  rows.push_back(Row("new-id-2", "accepted-2"));
  rows.push_back(Row("new-id-3", "accepted-3"));
  rows.push_back(Row("new-id-4", "accepted-4"));
  rows.push_back(Row("duplicate-id", "rejected-duplicate"));
  rows.push_back(Row("new-id-6", "accepted-6"));
  rows.push_back(Row("new-id-7", "accepted-7"));
  rows.push_back(Row("new-id-8", "accepted-8"));

  auto context = Begin(fixture, "odf037-import");
  const auto result = api::EngineExecuteImportRows(
      ImportRequest(fixture, context, std::move(rows)));
  RequireOk(result, "ODF-037 reject-row import failed");
  Require(result.accepted_rows == 7 && result.inserted_rows == 7 &&
              result.rejected_rows == 1,
          "ODF-037 reject-row counts changed");
  Require(result.result_shape.rows.size() == 8,
          "ODF-037 result shape should include accepted rows and one diagnostic");

  Require(FieldValue(result.result_shape, 0, "id") == "new-id-1",
          "ODF-037 first accepted result row moved");
  Require(FieldValue(result.result_shape, 3, "id") == "new-id-4",
          "ODF-037 left accepted result ordering changed");
  Require(FieldValue(result.result_shape, 4, "source_row_number") == "5",
          "ODF-037 reject diagnostic source row changed");
  Require(FieldValue(result.result_shape, 5, "id") == "new-id-6",
          "ODF-037 right accepted result ordering changed");
  Require(FieldValue(result.result_shape, 7, "id") == "new-id-8",
          "ODF-037 final accepted result row moved");
  const std::string duplicate_detail =
      FieldValue(result.result_shape, 4, "diagnostic_detail");
  Require(duplicate_detail.find("duplicate_key") != std::string::npos ||
              duplicate_detail.find("unique_index_duplicate") != std::string::npos,
          "ODF-037 reject diagnostic did not describe duplicate key");

  Require(HasEvidence(result.evidence, "import_row_window_route", "borrowed_span"),
          "ODF-037 import did not use borrowed row windows");
  Require(HasEvidence(result.evidence, "import_row_vector_copies", "0"),
          "ODF-037 import copied canonical rows");
  Require(HasEvidence(result.evidence, "copy_append_reject_fallback", "bisection"),
          "ODF-037 bisection fallback evidence missing");
  Require(HasEvidence(result.evidence, "copy_append_singleton_fallback_batches", "1"),
          "ODF-037 compatibility singleton fallback evidence missing");
  Require(HasEvidence(result.evidence, "copy_append_bisection_split_count", "3"),
          "ODF-037 bisection split count changed");
  Require(HasEvidence(result.evidence,
                      "copy_append_bisection_terminal_singleton_count",
                      "1"),
          "ODF-037 bisection terminal singleton count changed");
  const api::EngineApiU64 attempts =
      EvidenceU64(result.evidence, "copy_append_bisection_batch_attempt_count");
  const api::EngineApiU64 windows =
      EvidenceU64(result.evidence, "import_row_window_count");
  Require(attempts == 7, "ODF-037 bisection attempt count changed");
  Require(windows == attempts,
          "ODF-037 row window count did not match bisection attempts");
  Require(attempts < 9,
          "ODF-037 bisection did not beat old full singleton replay attempts");
  Require(SelectCount(fixture, context) == 8,
          "ODF-037 accepted rows not visible inside writer transaction");
  RequireNoPathLeak(result.evidence);
  Rollback(context);
}

}  // namespace

int main() {
  TestRejectFallbackUsesBisection();
  return EXIT_SUCCESS;
}

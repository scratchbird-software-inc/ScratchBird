// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "database_lifecycle.hpp"
#include "dml/insert_api.hpp"
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

void DumpEvidence(const std::vector<api::EngineEvidenceReference>& evidence) {
  for (const auto& entry : evidence) {
    std::cerr << entry.evidence_kind << '=' << entry.evidence_id << '\n';
  }
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
  const auto generated = uuid::GenerateEngineIdentityV7(kind, 1900000000000ull + salt);
  Require(generated.ok(), "IPAR-P1-05 UUID generation failed");
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
  std::string schema_uuid;
  platform::u64 salt = 0;
  api::EngineRequestContext context;

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
  row.fields.push_back({"tag", TextValue("hot")});
  row.fields.push_back({"note", TextValue("adaptive batch proof")});
  return row;
}

std::vector<api::EngineRowValue> Rows(std::string prefix,
                                      std::size_t count,
                                      std::size_t payload_bytes = 16) {
  std::vector<api::EngineRowValue> rows;
  rows.reserve(count);
  for (std::size_t index = 0; index < count; ++index) {
    rows.push_back(Row(prefix + "-id-" + std::to_string(index),
                       std::string(payload_bytes, static_cast<char>('a' + (index % 26)))));
  }
  return rows;
}

api::CrudTableRecord Table(const Fixture& fixture) {
  api::CrudTableRecord table;
  table.creator_tx = fixture.context.local_transaction_id;
  table.table_uuid = fixture.table_uuid;
  table.default_name = "ipar_adaptive_batch_sizing";
  table.columns.push_back({"id", "canonical=character;not_null=true"});
  table.columns.push_back({"payload", "canonical=character"});
  table.columns.push_back({"tag", "canonical=character"});
  table.columns.push_back({"note", "canonical=character"});
  return table;
}

api::CrudIndexRecord Index(const Fixture& fixture,
                           std::string index_uuid,
                           std::string column_name) {
  api::CrudIndexRecord index;
  index.creator_tx = fixture.context.local_transaction_id;
  index.index_uuid = std::move(index_uuid);
  index.table_uuid = fixture.table_uuid;
  index.column_name = std::move(column_name);
  index.family = api::kCrudIndexFamilyBtree;
  index.profile = api::kCrudIndexProfileRowStoreScalarBtreeV1;
  index.default_name = "ipar_adaptive_batch_idx";
  index.key_envelopes.push_back(index.column_name);
  index.unique = false;
  return index;
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
  RequireOk(begun, "IPAR-P1-05 begin transaction failed");
  auto context = request.context;
  context.local_transaction_id = begun.local_transaction_id;
  context.transaction_uuid = begun.transaction_uuid;
  context.snapshot_visible_through_local_transaction_id =
      begun.snapshot_visible_through_local_transaction_id;
  context.transaction_isolation_level = begun.isolation_level;
  return context;
}

void Rollback(const api::EngineRequestContext& context) {
  api::EngineRollbackTransactionRequest request;
  request.context = context;
  const auto rolled_back = api::EngineRollbackTransaction(request);
  RequireOk(rolled_back, "IPAR-P1-05 rollback failed");
}

Fixture MakeFixture(std::string_view label,
                    platform::u64 salt,
                    std::size_t index_count) {
  Fixture fixture;
  fixture.salt = salt;
  fixture.dir = std::filesystem::temp_directory_path() /
                ("scratchbird_ipar_adaptive_batch_" +
                 std::string(label) + "_" + std::to_string(salt));
  std::filesystem::create_directories(fixture.dir);
  fixture.database_path = fixture.dir / "ipar_adaptive_batch.sbdb";

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
  Require(created.ok(), "IPAR-P1-05 database create failed");

  fixture.database_uuid = uuid::UuidToString(create.database_uuid.value);
  fixture.schema_uuid = NewUuidText(platform::UuidKind::object, salt + 10);
  fixture.table_uuid = NewUuidText(platform::UuidKind::object, salt + 11);
  fixture.context = Begin(fixture, "ipar-p1-05-metadata-" + std::string(label));

  const auto table = api::AppendMgaTableMetadata(fixture.context, Table(fixture));
  Require(!table.error, "IPAR-P1-05 table metadata append failed");
  for (std::size_t index = 0; index < index_count; ++index) {
    const std::string column = index % 2 == 0 ? "payload" : "note";
    const auto metadata = api::AppendMgaIndexMetadata(
        fixture.context,
        Index(fixture,
              NewUuidText(platform::UuidKind::object, salt + 1000 + index),
              column));
    Require(!metadata.error, "IPAR-P1-05 index metadata append failed");
  }
  return fixture;
}

api::EngineInsertRowsRequest InsertRequest(const Fixture& fixture,
                                           std::vector<api::EngineRowValue> rows,
                                           std::vector<std::string> options) {
  api::EngineInsertRowsRequest request;
  request.context = fixture.context;
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

api::EngineApiU64 EvidenceU64(const std::vector<api::EngineEvidenceReference>& evidence,
                              std::string_view kind) {
  for (const auto& item : evidence) {
    if (item.evidence_kind != kind) { continue; }
    try {
      return static_cast<api::EngineApiU64>(std::stoull(item.evidence_id));
    } catch (...) {
      return 0;
    }
  }
  return 0;
}

std::string EvidenceText(const std::vector<api::EngineEvidenceReference>& evidence,
                         std::string_view kind) {
  for (const auto& item : evidence) {
    if (item.evidence_kind == kind) { return item.evidence_id; }
  }
  return {};
}

void RequireAdaptiveEvidence(const api::EngineInsertRowsResult& result,
                             api::EngineApiU64 requested_rows,
                             api::EngineApiU64 expected_admitted_rows,
                             std::string_view expected_reason) {
  RequireOk(result, "IPAR-P1-05 adaptive insert failed");
  Require(result.inserted_count == requested_rows,
          "IPAR-P1-05 inserted row count mismatch");
  const auto requested = EvidenceU64(result.evidence, "insert_adaptive_batch_requested_rows");
  const auto admitted = EvidenceU64(result.evidence, "insert_adaptive_batch_admitted_rows");
  const auto reduced = EvidenceText(result.evidence, "insert_adaptive_batch_reduced");
  const auto reason = EvidenceText(result.evidence, "insert_adaptive_batch_reason");
  const auto window_count = EvidenceU64(result.evidence, "insert_adaptive_write_window_count");
  const auto window_max = EvidenceU64(result.evidence, "insert_adaptive_write_window_max_rows");
  const auto appended_rows = EvidenceU64(result.evidence, "insert_hot_append_row_versions");
  if (requested != requested_rows || admitted != expected_admitted_rows ||
      reduced != "true" || reason != expected_reason || window_count <= 1 ||
      window_max > expected_admitted_rows || appended_rows != requested_rows) {
    DumpEvidence(result.evidence);
  }
  Require(requested == requested_rows,
          "IPAR-P1-05 requested row evidence mismatch");
  Require(admitted == expected_admitted_rows,
          "IPAR-P1-05 admitted row evidence mismatch");
  Require(reduced == "true",
          "IPAR-P1-05 adaptive reduction evidence missing");
  Require(reason == expected_reason,
          "IPAR-P1-05 adaptive reason mismatch");
  Require(window_count > 1,
          "IPAR-P1-05 adaptive writer did not split into windows");
  Require(window_max <= expected_admitted_rows,
          "IPAR-P1-05 adaptive writer exceeded admitted window size");
  Require(appended_rows == requested_rows,
          "IPAR-P1-05 hot append row count mismatch");
}

void VerifyMemoryBudgetReduction(platform::u64 salt) {
  auto fixture = MakeFixture("memory", salt, 4);
  auto inserted = api::EngineInsertRows(
      InsertRequest(fixture,
                    Rows("memory", 7, 32),
                    {"memory.context_budget_bytes=640"}));
  RequireAdaptiveEvidence(inserted, 7, 1, "memory_budget");
  Rollback(fixture.context);
}

void VerifyIndexFanoutReduction(platform::u64 salt) {
  auto fixture = MakeFixture("index", salt, 8);
  auto inserted = api::EngineInsertRows(
      InsertRequest(fixture,
                    Rows("index", 513, 24),
                    {"memory.context_budget_bytes=16777216"}));
  RequireAdaptiveEvidence(inserted, 513, 512, "index_fanout");
  Require(EvidenceU64(inserted.evidence, "insert_adaptive_batch_index_count") == 8,
          "IPAR-P1-05 index fanout count evidence mismatch");
  Rollback(fixture.context);
}

void VerifyLargeValueReduction(platform::u64 salt) {
  auto fixture = MakeFixture("large", salt, 1);
  auto inserted = api::EngineInsertRows(
      InsertRequest(fixture,
                    Rows("large", 257, 5000),
                    {"memory.context_budget_bytes=16777216"}));
  RequireAdaptiveEvidence(inserted, 257, 256, "large_value_pressure");
  Require(EvidenceText(inserted.evidence,
                       "insert_memory_pressure") == "large_value_pressure",
          "IPAR-P1-05 large value memory pressure evidence mismatch");
  Rollback(fixture.context);
}

void VerifyPageSizeReduction(platform::u64 salt) {
  auto fixture = MakeFixture("page", salt, 1);
  auto inserted = api::EngineInsertRows(
      InsertRequest(fixture,
                    Rows("page", 3, 5000),
                    {"memory.context_budget_bytes=16777216",
                     "adaptive_batch.page_size_bytes=4096"}));
  RequireAdaptiveEvidence(inserted, 3, 1, "page_size");
  Require(EvidenceU64(inserted.evidence,
                      "insert_adaptive_batch_page_size_bytes") == 4096,
          "IPAR-P1-05 page-size evidence mismatch");
  Rollback(fixture.context);
}

void VerifyCommitPolicyReduction(platform::u64 salt) {
  auto fixture = MakeFixture("commit", salt, 1);
  auto inserted = api::EngineInsertRows(
      InsertRequest(fixture,
                    Rows("commit", 9, 24),
                    {"memory.context_budget_bytes=16777216",
                     "adaptive_batch.commit_window_rows=4"}));
  RequireAdaptiveEvidence(inserted, 9, 4, "commit_policy");
  Require(EvidenceU64(inserted.evidence,
                      "insert_adaptive_batch_commit_window_rows") == 4,
          "IPAR-P1-05 commit-window evidence mismatch");
  Rollback(fixture.context);
}

void VerifyContentionReduction(platform::u64 salt) {
  auto fixture = MakeFixture("contention", salt, 1);
  auto inserted = api::EngineInsertRows(
      InsertRequest(fixture,
                    Rows("contention", 9, 24),
                    {"memory.context_budget_bytes=16777216",
                     "adaptive_batch.contention_window_rows=3"}));
  RequireAdaptiveEvidence(inserted, 9, 3, "contention");
  Require(EvidenceU64(inserted.evidence,
                      "insert_adaptive_batch_contention_window_rows") == 3,
          "IPAR-P1-05 contention-window evidence mismatch");
  Rollback(fixture.context);
}

}  // namespace

int main() {
  const auto salt = TimeSeed();
  VerifyMemoryBudgetReduction(salt + 100);
  VerifyIndexFanoutReduction(salt + 200);
  VerifyLargeValueReduction(salt + 300);
  VerifyPageSizeReduction(salt + 400);
  VerifyCommitPolicyReduction(salt + 500);
  VerifyContentionReduction(salt + 600);
  return EXIT_SUCCESS;
}

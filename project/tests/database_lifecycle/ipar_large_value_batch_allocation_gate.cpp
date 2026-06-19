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
  Require(generated.ok(), "IPAR-P3-04 UUID generation failed");
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

api::EngineRowValue Row(std::size_t index, std::size_t payload_bytes) {
  api::EngineRowValue row;
  row.fields.push_back({"id", TextValue("large-id-" + std::to_string(index))});
  row.fields.push_back(
      {"payload",
       TextValue(std::string(payload_bytes,
                             static_cast<char>('A' + (index % 26))))});
  row.fields.push_back({"tag", TextValue("batch-large-value")});
  return row;
}

std::vector<api::EngineRowValue> Rows(std::size_t count,
                                      std::size_t payload_bytes) {
  std::vector<api::EngineRowValue> rows;
  rows.reserve(count);
  for (std::size_t index = 0; index < count; ++index) {
    rows.push_back(Row(index, payload_bytes));
  }
  return rows;
}

api::CrudTableRecord Table(const Fixture& fixture) {
  api::CrudTableRecord table;
  table.creator_tx = fixture.context.local_transaction_id;
  table.table_uuid = fixture.table_uuid;
  table.default_name = "ipar_large_value_batch";
  table.columns.push_back({"id", "canonical=character;not_null=true"});
  table.columns.push_back({"payload", "canonical=character"});
  table.columns.push_back({"tag", "canonical=character"});
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
  RequireOk(begun, "IPAR-P3-04 begin transaction failed");
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
  RequireOk(rolled_back, "IPAR-P3-04 rollback failed");
}

Fixture MakeFixture(platform::u64 salt) {
  Fixture fixture;
  fixture.salt = salt;
  fixture.dir = std::filesystem::temp_directory_path() /
                ("scratchbird_ipar_large_value_batch_" +
                 std::to_string(salt));
  std::filesystem::create_directories(fixture.dir);
  fixture.database_path = fixture.dir / "ipar_large_value_batch.sbdb";

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
  Require(created.ok(), "IPAR-P3-04 database create failed");

  fixture.database_uuid = uuid::UuidToString(create.database_uuid.value);
  fixture.schema_uuid = NewUuidText(platform::UuidKind::object, salt + 10);
  fixture.table_uuid = NewUuidText(platform::UuidKind::object, salt + 11);
  fixture.context = Begin(fixture, "ipar-p3-04-metadata");

  const auto table = api::AppendMgaTableMetadata(fixture.context, Table(fixture));
  Require(!table.error, "IPAR-P3-04 table metadata append failed");
  return fixture;
}

api::EngineInsertRowsRequest InsertRequest(const Fixture& fixture,
                                           std::vector<api::EngineRowValue> rows) {
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
  return request;
}

api::EngineApiU64 EvidenceU64(
    const std::vector<api::EngineEvidenceReference>& evidence,
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

bool EvidenceText(const std::vector<api::EngineEvidenceReference>& evidence,
                  std::string_view kind,
                  std::string_view value) {
  for (const auto& item : evidence) {
    if (item.evidence_kind == kind && item.evidence_id == value) {
      return true;
    }
  }
  return false;
}

void DumpEvidence(const std::vector<api::EngineEvidenceReference>& evidence) {
  for (const auto& item : evidence) {
    std::cerr << item.evidence_kind << '=' << item.evidence_id << '\n';
  }
}

void VerifyLargeValueBatchAllocation() {
  constexpr std::size_t kRowCount = 16;
  constexpr std::size_t kPayloadBytes = 10000;
  auto fixture = MakeFixture(TimeSeed());

  auto request = InsertRequest(fixture, Rows(kRowCount, kPayloadBytes));
  const auto inserted = api::EngineInsertRows(request);
  if (!inserted.ok) {
    DumpEvidence(inserted.evidence);
  }
  RequireOk(inserted, "IPAR-P3-04 large-value batch insert failed");
  Require(inserted.inserted_count == kRowCount,
          "IPAR-P3-04 inserted row count mismatch");
  Require(EvidenceText(inserted.evidence,
                       "mga_large_value_batch_writer",
                       "window"),
          "IPAR-P3-04 batch writer evidence missing");
  Require(EvidenceU64(inserted.evidence,
                      "insert_large_value_batch_windows") == 1,
          "IPAR-P3-04 expected one large-value batch window");
  Require(EvidenceU64(inserted.evidence,
                      "insert_large_value_batch_overflows") == kRowCount,
          "IPAR-P3-04 overflow count mismatch");
  Require(EvidenceU64(inserted.evidence,
                      "insert_large_value_batch_chunks") >= kRowCount * 4,
          "IPAR-P3-04 chunk count too small for large payloads");
  Require(EvidenceU64(inserted.evidence,
                      "insert_large_value_batch_preallocated_chunks") ==
              EvidenceU64(inserted.evidence,
                          "insert_large_value_batch_chunks"),
          "IPAR-P3-04 preallocated chunk evidence mismatch");
  Require(EvidenceU64(inserted.evidence,
                      "insert_large_value_batch_stream_opens") == 1,
          "IPAR-P3-04 large-value stream opened more than once");
  Require(EvidenceU64(inserted.evidence,
                      "insert_large_value_batch_stream_flushes") == 1,
          "IPAR-P3-04 large-value stream flushed more than once");

  const auto loaded = api::LoadMgaRelationStoreState(fixture.context);
  Require(loaded.ok, "IPAR-P3-04 load after large-value insert failed");
  std::size_t visible_payloads = 0;
  for (const auto& row : loaded.state.row_versions) {
    if (row.table_uuid != fixture.table_uuid || row.deleted) { continue; }
    for (const auto& value : row.values) {
      if (value.first == "payload" && value.second.size() == kPayloadBytes) {
        ++visible_payloads;
      }
    }
  }
  Require(visible_payloads == kRowCount,
          "IPAR-P3-04 large-value locators did not expand to payloads");
  Rollback(fixture.context);
}

}  // namespace

int main() {
  VerifyLargeValueBatchAllocation();
  return EXIT_SUCCESS;
}

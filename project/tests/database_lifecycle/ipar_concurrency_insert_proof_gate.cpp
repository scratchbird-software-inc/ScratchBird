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

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
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
  Require(generated.ok(), "IPAR-P7-04 UUID generation failed");
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
  typed.state = api::EngineValueState::value;
  return typed;
}

api::EngineRowValue Row(std::string id, std::string note) {
  api::EngineRowValue row;
  row.fields.push_back({"id", TextValue(std::move(id))});
  row.fields.push_back({"note", TextValue(std::move(note))});
  return row;
}

std::vector<api::EngineRowValue> Rows(std::string prefix, int count) {
  std::vector<api::EngineRowValue> rows;
  rows.reserve(static_cast<std::size_t>(count));
  for (int index = 0; index < count; ++index) {
    rows.push_back(Row(prefix + "-id-" + std::to_string(index),
                       prefix + "-note-" + std::to_string(index)));
  }
  return rows;
}

api::EngineRequestContext BaseContext(const Fixture& fixture,
                                      std::string request_id,
                                      platform::u64 lane) {
  api::EngineRequestContext context;
  context.trust_mode = api::EngineTrustMode::server_isolated;
  context.request_id = std::move(request_id);
  context.database_path = fixture.database_path.string();
  context.database_uuid.canonical = fixture.database_uuid;
  context.principal_uuid.canonical =
      NewUuidText(platform::UuidKind::principal, fixture.salt + 100 + lane);
  context.session_uuid.canonical =
      NewUuidText(platform::UuidKind::object, fixture.salt + 1000 + lane);
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

api::EngineRequestContext Begin(const Fixture& fixture,
                                std::string request_id,
                                platform::u64 lane) {
  api::EngineBeginTransactionRequest request;
  request.context = BaseContext(fixture, std::move(request_id), lane);
  request.isolation_level = "read_committed";
  const auto begun = api::EngineBeginTransaction(request);
  RequireOk(begun, "IPAR-P7-04 begin transaction failed");
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
  RequireOk(api::EngineCommitTransaction(request),
            "IPAR-P7-04 commit failed");
}

void Rollback(const api::EngineRequestContext& context) {
  api::EngineRollbackTransactionRequest request;
  request.context = context;
  RequireOk(api::EngineRollbackTransaction(request),
            "IPAR-P7-04 rollback failed");
}

api::CrudTableRecord Table(const Fixture& fixture,
                           const api::EngineRequestContext& context) {
  api::CrudTableRecord table;
  table.creator_tx = context.local_transaction_id;
  table.table_uuid = fixture.table_uuid;
  table.default_name = "ipar_concurrency_insert_proof";
  table.columns.push_back({"id", "canonical=character;primary_key=true;not_null=true"});
  table.columns.push_back({"note", "canonical=character"});
  return table;
}

api::CrudIndexRecord Index(const Fixture& fixture,
                           const api::EngineRequestContext& context) {
  api::CrudIndexRecord index;
  index.creator_tx = context.local_transaction_id;
  index.index_uuid = fixture.index_uuid;
  index.table_uuid = fixture.table_uuid;
  index.column_name = "id";
  index.family = api::kCrudIndexFamilyBtree;
  index.profile = api::kCrudIndexProfileRowStoreScalarBtreeV1;
  index.default_name = "ipar_concurrency_insert_proof_id_pk";
  index.unique = true;
  index.key_envelopes.push_back("id");
  index.key_envelopes.push_back("unique");
  return index;
}

Fixture MakeFixture(platform::u64 salt) {
  Fixture fixture;
  fixture.salt = salt;
  fixture.dir = std::filesystem::temp_directory_path() /
                ("scratchbird_ipar_p704_" + std::to_string(salt));
  std::filesystem::create_directories(fixture.dir);
  fixture.database_path = fixture.dir / "ipar_concurrency_insert_proof.sbdb";

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
  Require(created.ok(), "IPAR-P7-04 database create failed");

  fixture.database_uuid = uuid::UuidToString(create.database_uuid.value);
  fixture.schema_uuid = NewUuidText(platform::UuidKind::object, salt + 10);
  fixture.table_uuid = NewUuidText(platform::UuidKind::object, salt + 11);
  fixture.index_uuid = NewUuidText(platform::UuidKind::object, salt + 12);

  auto metadata = Begin(fixture, "ipar-p704-metadata", 0);
  const auto table = api::AppendMgaTableMetadata(metadata, Table(fixture, metadata));
  Require(!table.error, "IPAR-P7-04 table metadata append failed");
  const auto index = api::AppendMgaIndexMetadata(metadata, Index(fixture, metadata));
  Require(!index.error, "IPAR-P7-04 index metadata append failed");
  Commit(metadata);
  return fixture;
}

api::EngineInsertRowsRequest InsertRequest(const Fixture& fixture,
                                           const api::EngineRequestContext& context,
                                           std::vector<api::EngineRowValue> rows) {
  api::EngineInsertRowsRequest request;
  request.context = context;
  request.target_schema.uuid.canonical = fixture.schema_uuid;
  request.target_table.uuid.canonical = fixture.table_uuid;
  request.target_table.object_kind = "table";
  request.target_object.uuid.canonical = fixture.table_uuid;
  request.target_object.object_kind = "table";
  request.estimated_row_count = static_cast<api::EngineApiU64>(rows.size());
  request.input_rows = std::move(rows);
  request.option_envelopes.push_back("prepared_descriptor=enabled");
  return request;
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
  request.import_policy.reject_mode = "fail_fast";
  request.import_policy.reject_payload_policy = "diagnostic_only";
  request.import_policy.resume_policy = "fail_closed";
  request.canonical_rows = std::move(rows);
  request.estimated_row_count =
      static_cast<api::EngineApiU64>(request.canonical_rows.size());
  request.option_envelopes.push_back("copy_append_batching=enabled");
  request.option_envelopes.push_back("copy_append_batch_rows=4");
  request.option_envelopes.push_back("physical_mga_cow=required");
  request.option_envelopes.push_back("physical_mga_cow.page_number=3072");
  request.option_envelopes.push_back("physical_mga_cow.rows_per_page=8");
  return request;
}

api::EngineApiU64 SelectCount(const Fixture& fixture) {
  auto context = Begin(fixture, "ipar-p704-select", 9000);
  api::EngineSelectRowsRequest request;
  request.context = context;
  request.source_object.uuid.canonical = fixture.table_uuid;
  request.source_object.object_kind = "table";
  request.select_projection.canonical_projection_envelopes.push_back("id");
  const auto selected = api::EngineSelectRows(request);
  RequireOk(selected, "IPAR-P7-04 select failed");
  Rollback(context);
  return selected.visible_count;
}

void Reopen(const Fixture& fixture) {
  const auto opened =
      db::OpenDatabaseFile({fixture.database_path.string(), false, false, false});
  if (!opened.ok()) {
    std::cerr << opened.diagnostic.diagnostic_code << ':'
              << opened.diagnostic.message_key << '\n';
  }
  Require(opened.ok(), "IPAR-P7-04 database reopen failed");
}

void VerifyConcurrentWriters(const Fixture& fixture) {
  constexpr int kWorkers = 4;
  constexpr int kRowsPerWorker = 8;
  std::mutex mutex;
  std::condition_variable cv;
  int ready = 0;
  bool start = false;
  std::atomic<int> failures{0};
  std::vector<std::thread> threads;

  for (int worker = 0; worker < kWorkers; ++worker) {
    threads.emplace_back([&, worker]() {
      {
        std::unique_lock<std::mutex> lock(mutex);
        ++ready;
        cv.notify_all();
        cv.wait(lock, [&] { return start; });
      }
      try {
        auto context = Begin(fixture,
                             "ipar-p704-worker-" + std::to_string(worker),
                             static_cast<platform::u64>(worker + 1));
        const auto inserted = api::EngineInsertRows(InsertRequest(
            fixture,
            context,
            Rows("worker-" + std::to_string(worker), kRowsPerWorker)));
        RequireOk(inserted, "IPAR-P7-04 worker insert failed");
        Require(inserted.inserted_count == kRowsPerWorker,
                "IPAR-P7-04 worker insert count mismatch");
        Commit(context);
      } catch (...) {
        ++failures;
      }
    });
  }

  {
    std::unique_lock<std::mutex> lock(mutex);
    cv.wait(lock, [&] { return ready == kWorkers; });
    start = true;
  }
  cv.notify_all();
  for (auto& thread : threads) {
    thread.join();
  }
  Require(failures.load() == 0, "IPAR-P7-04 concurrent worker failed");
  Require(SelectCount(fixture) == kWorkers * kRowsPerWorker,
          "IPAR-P7-04 concurrent committed row count mismatch");
}

void VerifyCopyRollbackDuplicateAndRestart(Fixture& fixture) {
  auto copy = Begin(fixture, "ipar-p704-copy", 100);
  const auto imported = api::EngineExecuteImportRows(
      ImportRequest(fixture, copy, Rows("copy", 5)));
  RequireOk(imported, "IPAR-P7-04 COPY import failed");
  Require(imported.inserted_rows == 5,
          "IPAR-P7-04 COPY import count mismatch");
  Commit(copy);
  Require(SelectCount(fixture) == 37,
          "IPAR-P7-04 COPY committed row count mismatch");

  auto rollback = Begin(fixture, "ipar-p704-rollback", 101);
  const auto rollback_insert = api::EngineInsertRows(InsertRequest(
      fixture,
      rollback,
      Rows("rollback", 3)));
  RequireOk(rollback_insert, "IPAR-P7-04 rollback insert failed");
  Rollback(rollback);
  Require(SelectCount(fixture) == 37,
          "IPAR-P7-04 rolled-back rows became visible");

  auto duplicate = Begin(fixture, "ipar-p704-duplicate", 102);
  const auto duplicate_insert = api::EngineInsertRows(InsertRequest(
      fixture,
      duplicate,
      std::vector<api::EngineRowValue>{Row("worker-0-id-0", "duplicate")}));
  Require(!duplicate_insert.ok,
          "IPAR-P7-04 duplicate unique insert was accepted");
  Rollback(duplicate);

  Reopen(fixture);
  Require(SelectCount(fixture) == 37,
          "IPAR-P7-04 reopen row count mismatch");
}

}  // namespace

int main() {
  auto fixture = MakeFixture(TimeSeed());
  VerifyConcurrentWriters(fixture);
  VerifyCopyRollbackDuplicateAndRestart(fixture);
  return EXIT_SUCCESS;
}

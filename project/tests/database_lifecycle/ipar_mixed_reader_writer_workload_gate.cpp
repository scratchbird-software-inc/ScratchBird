// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "catalog/sys_information_projection.hpp"
#include "database_lifecycle.hpp"
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
namespace info = scratchbird::engine::internal_api;
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
      std::cerr << diagnostic.code << ':' << diagnostic.message_key << ':'
                << diagnostic.detail << '\n';
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
      uuid::GenerateEngineIdentityV7(kind, 1918000000000ull + salt);
  Require(generated.ok(), "IPAR-P7-10 UUID generation failed");
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

struct Fixture {
  std::filesystem::path dir;
  std::filesystem::path database_path;
  std::string database_uuid;
  std::string schema_uuid;
  std::string table_uuid;
  std::string id_index_uuid;
  std::string tag_index_uuid;
  platform::u64 salt = 0;

  ~Fixture() {
    if (!dir.empty()) {
      std::error_code ignored;
      std::filesystem::remove_all(dir, ignored);
    }
  }
};

api::EngineRequestContext BaseContext(const Fixture& fixture,
                                      std::string request_id,
                                      platform::u64 salt) {
  api::EngineRequestContext context;
  context.trust_mode = api::EngineTrustMode::server_isolated;
  context.request_id = std::move(request_id);
  context.database_path = fixture.database_path.string();
  context.database_uuid.canonical = fixture.database_uuid;
  context.principal_uuid.canonical =
      NewUuidText(platform::UuidKind::principal, fixture.salt + 100 + salt);
  context.session_uuid.canonical =
      NewUuidText(platform::UuidKind::object, fixture.salt + 200 + salt);
  context.current_schema_uuid.canonical = fixture.schema_uuid;
  context.security_context_present = true;
  context.catalog_generation_id = 1;
  context.security_epoch = 1;
  context.resource_epoch = 1;
  context.name_resolution_epoch = 1;
  return context;
}

api::EngineRequestContext Begin(const Fixture& fixture,
                                std::string request_id,
                                platform::u64 salt) {
  api::EngineBeginTransactionRequest request;
  request.context = BaseContext(fixture, std::move(request_id), salt);
  request.isolation_level = "read_committed";
  const auto begun = api::EngineBeginTransaction(request);
  RequireOk(begun, "IPAR-P7-10 begin transaction failed");
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
  const auto committed = api::EngineCommitTransaction(request);
  RequireOk(committed, "IPAR-P7-10 commit failed");
}

void Rollback(const api::EngineRequestContext& context) {
  api::EngineRollbackTransactionRequest request;
  request.context = context;
  const auto rolled_back = api::EngineRollbackTransaction(request);
  RequireOk(rolled_back, "IPAR-P7-10 rollback failed");
}

api::EngineRowValue Row(std::string id, std::string note, std::string tag) {
  api::EngineRowValue row;
  row.fields.push_back({"id", TextValue(std::move(id))});
  row.fields.push_back({"note", TextValue(std::move(note))});
  row.fields.push_back({"tag", TextValue(std::move(tag))});
  return row;
}

std::vector<api::EngineRowValue> Rows(std::string prefix,
                                      int worker,
                                      int count) {
  std::vector<api::EngineRowValue> rows;
  rows.reserve(static_cast<std::size_t>(count));
  for (int index = 0; index < count; ++index) {
    rows.push_back(Row(prefix + "-w" + std::to_string(worker) + "-r" +
                           std::to_string(index),
                       "mixed-reader-writer-note-" + std::to_string(index),
                       "hot-tag-" + std::to_string(index % 4)));
  }
  return rows;
}

api::CrudTableRecord Table(const Fixture& fixture,
                           const api::EngineRequestContext& context) {
  api::CrudTableRecord table;
  table.creator_tx = context.local_transaction_id;
  table.table_uuid = fixture.table_uuid;
  table.default_name = "ipar_mixed_reader_writer";
  table.columns.push_back({"id", "canonical=character;primary_key=true;not_null=true"});
  table.columns.push_back({"note", "canonical=character"});
  table.columns.push_back({"tag", "canonical=character"});
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
  index.default_name = "ipar_mixed_reader_writer_" + index.column_name;
  index.unique = unique;
  index.key_envelopes.push_back(index.column_name);
  if (unique) { index.key_envelopes.push_back("unique"); }
  return index;
}

Fixture MakeFixture(platform::u64 salt) {
  Fixture fixture;
  fixture.salt = salt;
  fixture.dir = std::filesystem::temp_directory_path() /
                ("scratchbird_ipar_mixed_reader_writer_" +
                 std::to_string(salt));
  std::filesystem::create_directories(fixture.dir);
  fixture.database_path = fixture.dir / "ipar_mixed_reader_writer.sbdb";

  db::DatabaseCreateConfig create;
  create.path = fixture.database_path.string();
  create.database_uuid = NewUuid(platform::UuidKind::database, salt + 1);
  create.filespace_uuid = NewUuid(platform::UuidKind::filespace, salt + 2);
  create.creation_unix_epoch_millis = 1918000000000ull + salt + 3;
  create.page_size = 8192;
  create.require_resource_seed_pack = false;
  create.allow_minimal_resource_bootstrap = true;
  create.allow_overwrite = true;
  const auto created = db::CreateDatabaseFile(create);
  if (!created.ok()) {
    std::cerr << created.diagnostic.diagnostic_code << ':'
              << created.diagnostic.message_key << '\n';
  }
  Require(created.ok(), "IPAR-P7-10 database create failed");

  fixture.database_uuid = uuid::UuidToString(create.database_uuid.value);
  fixture.schema_uuid = NewUuidText(platform::UuidKind::object, salt + 10);
  fixture.table_uuid = NewUuidText(platform::UuidKind::object, salt + 11);
  fixture.id_index_uuid = NewUuidText(platform::UuidKind::object, salt + 12);
  fixture.tag_index_uuid = NewUuidText(platform::UuidKind::object, salt + 13);

  auto metadata = Begin(fixture, "ipar-p7-10-metadata", 1);
  Require(!api::AppendMgaTableMetadata(metadata, Table(fixture, metadata)).error,
          "IPAR-P7-10 table metadata append failed");
  Require(!api::AppendMgaIndexMetadata(
               metadata,
               Index(fixture, metadata, fixture.id_index_uuid, "id", true))
               .error,
          "IPAR-P7-10 id index metadata append failed");
  Require(!api::AppendMgaIndexMetadata(
               metadata,
               Index(fixture, metadata, fixture.tag_index_uuid, "tag", false))
               .error,
          "IPAR-P7-10 tag index metadata append failed");
  Commit(metadata);
  return fixture;
}

std::vector<std::string> DemandOptions() {
  return {"page_allocation.runtime=enabled",
          "dml_demand_hints=enabled",
          "dml_demand_hints.max_pages=128",
          "dml_demand_hints.available_capacity_pages=128",
          "page_allocation.row_pages_per_mutation=32",
          "page_allocation.index_pages_per_mutation=32",
          "page_allocation.preallocate_index_pages=32"};
}

api::EngineInsertRowsRequest InsertRequest(
    const Fixture& fixture,
    const api::EngineRequestContext& context,
    std::vector<api::EngineRowValue> rows) {
  api::EngineInsertRowsRequest request;
  request.context = context;
  request.target_table.uuid.canonical = fixture.table_uuid;
  request.target_table.object_kind = "table";
  request.target_object.uuid.canonical = fixture.table_uuid;
  request.target_object.object_kind = "table";
  request.estimated_row_count = static_cast<api::EngineApiU64>(rows.size());
  request.input_rows = std::move(rows);
  request.option_envelopes = DemandOptions();
  return request;
}

api::EnginePredicateEnvelope TagPredicate(std::string value) {
  api::EnginePredicateEnvelope predicate;
  predicate.predicate_kind = "column_equals";
  predicate.canonical_predicate_envelope = "tag";
  predicate.bound_values.push_back(TextValue(std::move(value)));
  return predicate;
}

void RequireSelectAndIndexLookups(const Fixture& fixture,
                                  platform::u64 salt) {
  auto context = Begin(fixture, "ipar-p7-10-reader", salt);

  api::EngineSelectRowsRequest select;
  select.context = context;
  select.source_object.uuid.canonical = fixture.table_uuid;
  select.source_object.object_kind = "table";
  select.select_projection.canonical_projection_envelopes.push_back("id");
  select.select_predicate = TagPredicate("hot-tag-1");
  select.limit = 64;
  const auto selected = api::EngineSelectRows(select);
  RequireOk(selected, "IPAR-P7-10 select during mixed workload failed");

  const auto state = api::LoadMgaRelationStoreStateForInsertTarget(
      context,
      fixture.table_uuid);
  if (!state.ok) {
    std::cerr << state.diagnostic.code << ':' << state.diagnostic.detail << '\n';
  }
  Require(state.ok, "IPAR-P7-10 scoped state load failed");
  const auto indexed = api::IndexedMgaRowsForPredicateForContext(
      state.state.crud_metadata,
      fixture.table_uuid,
      TagPredicate("hot-tag-1"),
      context,
      64);
  Require(indexed.ok, "IPAR-P7-10 indexed row lookup failed");
  Require(indexed.index_used || selected.visible_count == 0,
          "IPAR-P7-10 indexed row lookup did not use index after rows were visible");

  const auto stats = api::EstimateMgaRelationStatistics(context, fixture.table_uuid, true);
  Require(stats.ok, "IPAR-P7-10 relation statistics lookup failed");
  Rollback(context);
}

info::SysInformationProjectionContext NavigatorContext() {
  info::SysInformationProjectionContext context;
  context.catalog_display_name = "MixedWorkloadDB";
  context.session_language = "en";
  context.default_language = "en";
  context.visible_catalog_generation_id = 10;
  return context;
}

void RequireNavigatorTreeProjection() {
  const std::vector<info::SysInformationCatalogObjectSource> objects = {
      {.object_uuid = "schema-users",
       .object_class = "schema",
       .catalog_generation_id = 1,
       .created_local_transaction_id = 1},
      {.object_uuid = "schema-public",
       .object_class = "schema",
       .parent_object_uuid = "schema-users",
       .catalog_generation_id = 1,
       .created_local_transaction_id = 1},
      {.object_uuid = "table-ipar-mixed",
       .object_class = "table",
       .schema_uuid = "schema-public",
       .parent_object_uuid = "schema-public",
       .table_type = "BASE TABLE",
       .catalog_generation_id = 2,
       .created_local_transaction_id = 2}};
  const std::vector<info::SysInformationResolverNameSource> names = {
      {.object_uuid = "schema-users",
       .object_class = "schema",
       .language_tag = "en",
       .name_class = "primary",
       .display_name = "users",
       .normalized_lookup_key = "USERS",
       .catalog_generation_id = 1},
      {.object_uuid = "schema-public",
       .object_class = "schema",
       .scope_uuid = "schema-users",
       .language_tag = "en",
       .name_class = "primary",
       .display_name = "public",
       .normalized_lookup_key = "PUBLIC",
       .catalog_generation_id = 1},
      {.object_uuid = "table-ipar-mixed",
       .object_class = "table",
       .scope_uuid = "schema-public",
       .language_tag = "en",
       .name_class = "primary",
       .display_name = "ipar_mixed_reader_writer",
       .normalized_lookup_key = "IPAR_MIXED_READER_WRITER",
       .catalog_generation_id = 2}};
  const std::vector<info::SysInformationColumnSource> columns = {
      {.relation_object_uuid = "table-ipar-mixed",
       .schema_uuid = "schema-public",
       .column_name = "id",
       .ordinal_position = 1,
       .datatype_name = "character",
       .is_nullable = "NO",
       .catalog_generation_id = 2}};
  const auto tree = info::BuildSysInformationProjection(
      "sys.catalog_readable.navigator_tree",
      NavigatorContext(),
      objects,
      names,
      {},
      {},
      columns);
  Require(tree.ok, "IPAR-P7-10 navigator tree projection failed");
  bool saw_database = false;
  bool saw_schema = false;
  bool saw_table = false;
  for (const auto& row : tree.rows) {
    for (const auto& field : row.fields) {
      saw_database = saw_database || field.second == "MixedWorkloadDB";
      saw_schema = saw_schema || field.second == "public";
      saw_table = saw_table || field.second == "ipar_mixed_reader_writer";
    }
  }
  Require(saw_database && saw_schema && saw_table,
          "IPAR-P7-10 navigator tree projection omitted expected nodes");
}

api::EngineApiU64 SelectCount(const Fixture& fixture) {
  auto context = Begin(fixture, "ipar-p7-10-final-select", 9000);
  api::EngineSelectRowsRequest request;
  request.context = context;
  request.source_object.uuid.canonical = fixture.table_uuid;
  request.source_object.object_kind = "table";
  request.select_projection.canonical_projection_envelopes.push_back("id");
  const auto selected = api::EngineSelectRows(request);
  RequireOk(selected, "IPAR-P7-10 final select failed");
  Rollback(context);
  return selected.visible_count;
}

void VerifyMixedReaderWriterWorkload() {
  auto fixture = MakeFixture(TimeSeed());
  constexpr int kWriters = 4;
  constexpr int kRowsPerWriter = 24;
  constexpr int kReaders = 3;
  constexpr int kReaderIterations = 12;

  std::mutex mutex;
  std::condition_variable cv;
  int ready = 0;
  bool start = false;
  std::atomic<int> failures{0};
  std::atomic<int> writer_commits{0};
  std::atomic<int> reader_iterations{0};
  std::vector<std::thread> threads;

  for (int writer = 0; writer < kWriters; ++writer) {
    threads.emplace_back([&, writer]() {
      {
        std::unique_lock<std::mutex> lock(mutex);
        ++ready;
        cv.notify_all();
        cv.wait(lock, [&] { return start; });
      }
      try {
        auto context = Begin(fixture,
                             "ipar-p7-10-writer-" + std::to_string(writer),
                             static_cast<platform::u64>(writer + 10));
        const auto inserted = api::EngineInsertRows(InsertRequest(
            fixture,
            context,
            Rows("mixed", writer, kRowsPerWriter)));
        RequireOk(inserted, "IPAR-P7-10 writer insert failed");
        Require(inserted.inserted_count == kRowsPerWriter,
                "IPAR-P7-10 writer insert count mismatch");
        Commit(context);
        ++writer_commits;
      } catch (...) {
        ++failures;
      }
    });
  }

  for (int reader = 0; reader < kReaders; ++reader) {
    threads.emplace_back([&, reader]() {
      {
        std::unique_lock<std::mutex> lock(mutex);
        ++ready;
        cv.notify_all();
        cv.wait(lock, [&] { return start; });
      }
      try {
        for (int iteration = 0; iteration < kReaderIterations; ++iteration) {
          RequireSelectAndIndexLookups(
              fixture,
              static_cast<platform::u64>(100 + reader * 100 + iteration));
          RequireNavigatorTreeProjection();
          ++reader_iterations;
          std::this_thread::yield();
        }
      } catch (...) {
        ++failures;
      }
    });
  }

  {
    std::unique_lock<std::mutex> lock(mutex);
    cv.wait(lock, [&] { return ready == kWriters + kReaders; });
    start = true;
    cv.notify_all();
  }
  for (auto& thread : threads) {
    thread.join();
  }

  Require(failures.load() == 0, "IPAR-P7-10 mixed workload thread failed");
  Require(writer_commits.load() == kWriters,
          "IPAR-P7-10 not all writers committed");
  Require(reader_iterations.load() == kReaders * kReaderIterations,
          "IPAR-P7-10 reader loop did not complete");
  Require(SelectCount(fixture) == kWriters * kRowsPerWriter,
          "IPAR-P7-10 final select count mismatch");
}

}  // namespace

int main() {
  VerifyMixedReaderWriterWorkload();
  std::cout << "ipar_mixed_reader_writer_workload_gate=passed\n";
  return EXIT_SUCCESS;
}

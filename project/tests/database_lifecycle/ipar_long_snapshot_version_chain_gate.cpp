// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "database_lifecycle.hpp"
#include "dml/insert_api.hpp"
#include "dml/select_api.hpp"
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
#include <unistd.h>
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

void RequireDiagnosticOk(const api::EngineApiDiagnostic& diagnostic,
                         std::string_view message) {
  if (diagnostic.error) {
    std::cerr << diagnostic.code << ':' << diagnostic.detail << '\n';
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
  Require(generated.ok(), "IPAR-P2-04 UUID generation failed");
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

api::EngineRowValue Row(std::string id, std::string payload) {
  api::EngineRowValue row;
  row.fields.push_back({"id", TextValue(std::move(id))});
  row.fields.push_back({"payload", TextValue(std::move(payload))});
  return row;
}

struct Fixture {
  std::filesystem::path dir;
  std::filesystem::path database_path;
  std::string database_uuid;
  std::string table_uuid;
  std::string id_index_uuid;
  std::string schema_uuid;
  platform::u64 salt = 0;

  ~Fixture() {
    std::error_code ignored;
    if (!dir.empty()) { std::filesystem::remove_all(dir, ignored); }
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

api::EngineRequestContext Begin(const Fixture& fixture,
                                std::string request_id,
                                std::string isolation_level) {
  api::EngineBeginTransactionRequest request;
  request.context = BaseContext(fixture, std::move(request_id));
  request.isolation_level = std::move(isolation_level);
  const auto begun = api::EngineBeginTransaction(request);
  RequireOk(begun, "IPAR-P2-04 begin transaction failed");

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
  RequireOk(api::EngineCommitTransaction(request), "IPAR-P2-04 commit failed");
}

void Rollback(const api::EngineRequestContext& context) {
  api::EngineRollbackTransactionRequest request;
  request.context = context;
  RequireOk(api::EngineRollbackTransaction(request), "IPAR-P2-04 rollback failed");
}

api::CrudTableRecord Table(const Fixture& fixture,
                           const api::EngineRequestContext& context) {
  api::CrudTableRecord table;
  table.creator_tx = context.local_transaction_id;
  table.table_uuid = fixture.table_uuid;
  table.default_name = "ipar_long_snapshot_version_chain";
  table.columns.push_back({"id", "canonical=character;primary_key=true"});
  table.columns.push_back({"payload", "canonical=character"});
  return table;
}

api::CrudIndexRecord IdIndex(const Fixture& fixture,
                             const api::EngineRequestContext& context) {
  api::CrudIndexRecord index;
  index.creator_tx = context.local_transaction_id;
  index.index_uuid = fixture.id_index_uuid;
  index.table_uuid = fixture.table_uuid;
  index.column_name = "id";
  index.family = api::kCrudIndexFamilyBtree;
  index.profile = api::kCrudIndexProfileRowStoreScalarBtreeV1;
  index.default_name = "ipar_long_snapshot_id_pk";
  index.unique = true;
  index.key_envelopes.push_back("id");
  index.key_envelopes.push_back("unique");
  return index;
}

Fixture MakeFixture() {
  Fixture fixture;
  fixture.salt = NowMillis();
  fixture.dir = std::filesystem::temp_directory_path() /
                ("scratchbird_ipar_p204_" + std::to_string(fixture.salt) +
                 "_" + std::to_string(static_cast<long long>(getpid())));
  std::filesystem::create_directories(fixture.dir);
  fixture.database_path = fixture.dir / "ipar_p204.sbdb";

  db::DatabaseCreateConfig create;
  create.path = fixture.database_path.string();
  create.database_uuid = NewUuid(platform::UuidKind::database, fixture.salt + 1);
  create.filespace_uuid = NewUuid(platform::UuidKind::filespace, fixture.salt + 2);
  create.creation_unix_epoch_millis = NowMillis() + fixture.salt + 3;
  create.require_resource_seed_pack = false;
  create.allow_minimal_resource_bootstrap = true;
  create.allow_overwrite = true;
  Require(db::CreateDatabaseFile(create).ok(), "IPAR-P2-04 database create failed");

  fixture.database_uuid = uuid::UuidToString(create.database_uuid.value);
  fixture.schema_uuid = NewUuidText(platform::UuidKind::schema, fixture.salt + 10);
  fixture.table_uuid = NewUuidText(platform::UuidKind::object, fixture.salt + 11);
  fixture.id_index_uuid = NewUuidText(platform::UuidKind::object, fixture.salt + 12);

  auto context = Begin(fixture, "ipar-p204-metadata", "read_committed");
  RequireDiagnosticOk(api::AppendMgaTableMetadata(context, Table(fixture, context)),
                      "IPAR-P2-04 table metadata append failed");
  RequireDiagnosticOk(api::AppendMgaIndexMetadata(context, IdIndex(fixture, context)),
                      "IPAR-P2-04 id index metadata append failed");
  Commit(context);
  return fixture;
}

api::EnginePredicateEnvelope IdEquals(std::string id) {
  api::EnginePredicateEnvelope predicate;
  predicate.predicate_kind = "column_equals";
  predicate.canonical_predicate_envelope = "id";
  predicate.bound_values.push_back(TextValue(std::move(id)));
  return predicate;
}

api::EngineInsertRowsResult InsertRow(const Fixture& fixture,
                                      const api::EngineRequestContext& context,
                                      std::string id,
                                      std::string payload) {
  api::EngineInsertRowsRequest request;
  request.context = context;
  request.target_table.uuid.canonical = fixture.table_uuid;
  request.target_table.object_kind = "table";
  request.input_rows.push_back(Row(std::move(id), std::move(payload)));
  request.estimated_row_count = 1;
  return api::EngineInsertRows(request);
}

api::EngineUpdateRowsResult UpdatePayload(const Fixture& fixture,
                                          const api::EngineRequestContext& context,
                                          std::string id,
                                          std::string payload) {
  api::EngineUpdateRowsRequest request;
  request.context = context;
  request.target_table.uuid.canonical = fixture.table_uuid;
  request.target_table.object_kind = "table";
  request.update_predicate = IdEquals(std::move(id));
  request.assignments.push_back({"payload", TextValue(std::move(payload))});
  return api::EngineUpdateRows(request);
}

api::EngineSelectRowsResult SelectById(const Fixture& fixture,
                                       const api::EngineRequestContext& context,
                                       std::string id) {
  api::EngineSelectRowsRequest request;
  request.context = context;
  request.source_object.uuid.canonical = fixture.table_uuid;
  request.source_object.object_kind = "table";
  request.select_predicate = IdEquals(std::move(id));
  return api::EngineSelectRows(request);
}

bool HasFieldValue(const api::EngineSelectRowsResult& result,
                   std::string_view field,
                   std::string_view value) {
  for (const auto& row : result.result_shape.rows) {
    for (const auto& candidate : row.fields) {
      if (candidate.first == field && candidate.second.encoded_value == value) {
        return true;
      }
    }
  }
  return false;
}

void RequirePayload(const Fixture& fixture,
                    const api::EngineRequestContext& context,
                    std::string expected,
                    std::string_view message) {
  const auto selected = SelectById(fixture, context, "row-1");
  RequireOk(selected, message);
  Require(selected.visible_count == 1, message);
  Require(HasFieldValue(selected, "payload", expected), message);
}

std::size_t CountRowVersions(const Fixture& fixture,
                             const api::EngineRequestContext& context) {
  const auto loaded = api::LoadMgaRelationStoreState(context);
  if (!loaded.ok) {
    std::cerr << loaded.diagnostic.code << ':' << loaded.diagnostic.detail << '\n';
  }
  Require(loaded.ok, "IPAR-P2-04 relation store load failed");

  std::size_t count = 0;
  for (const auto& row : loaded.state.row_versions) {
    if (row.table_uuid == fixture.table_uuid) { ++count; }
  }
  return count;
}

void ValidateLongSnapshotVersionChainPressure() {
  auto fixture = MakeFixture();

  auto seed = Begin(fixture, "ipar-p204-seed", "read_committed");
  const auto inserted = InsertRow(fixture, seed, "row-1", "payload-0");
  RequireOk(inserted, "IPAR-P2-04 seed insert failed");
  Require(inserted.inserted_count == 1, "IPAR-P2-04 seed insert count changed");
  Commit(seed);

  auto old_snapshot = Begin(fixture, "ipar-p204-old-snapshot", "snapshot");
  RequirePayload(fixture,
                 old_snapshot,
                 "payload-0",
                 "IPAR-P2-04 old snapshot did not see seeded payload");
  const auto before_versions = CountRowVersions(fixture, old_snapshot);
  Require(before_versions == 1, "IPAR-P2-04 seed version count changed");

  constexpr int kUpdateCount = 16;
  for (int i = 1; i <= kUpdateCount; ++i) {
    auto writer = Begin(fixture,
                        "ipar-p204-writer-" + std::to_string(i),
                        "read_committed");
    const auto updated =
        UpdatePayload(fixture, writer, "row-1", "payload-" + std::to_string(i));
    RequireOk(updated, "IPAR-P2-04 committed update failed");
    Require(updated.matched_count == 1 && updated.updated_count == 1,
            "IPAR-P2-04 committed update count changed");
    Commit(writer);

    RequirePayload(fixture,
                   old_snapshot,
                   "payload-0",
                   "IPAR-P2-04 old snapshot drifted under version-chain pressure");
  }

  auto latest_reader = Begin(fixture, "ipar-p204-latest-reader", "read_committed");
  RequirePayload(fixture,
                 latest_reader,
                 "payload-16",
                 "IPAR-P2-04 latest reader did not see latest committed version");
  const auto after_versions = CountRowVersions(fixture, latest_reader);
  Require(after_versions >= before_versions + kUpdateCount,
          "IPAR-P2-04 version chain did not retain committed versions");

  RequirePayload(fixture,
                 old_snapshot,
                 "payload-0",
                 "IPAR-P2-04 old snapshot lost retained version after latest read");

  Rollback(latest_reader);
  Rollback(old_snapshot);
}

}  // namespace

int main() {
  ValidateLongSnapshotVersionChainPressure();
  return EXIT_SUCCESS;
}

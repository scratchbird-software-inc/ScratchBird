// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "database_lifecycle.hpp"
#include "ddl/create_api.hpp"
#include "dml/import_execution_api.hpp"
#include "mga_relation_store/mga_relation_store.hpp"
#include "sorted_bulk_index_build.hpp"
#include "transaction/transaction_api.hpp"
#include "uuid.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace api = scratchbird::engine::internal_api;
namespace db = scratchbird::storage::database;
namespace idx = scratchbird::core::index;
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
    if (!result.diagnostics.empty()) {
      std::cerr << result.diagnostics.front().code << ':'
                << result.diagnostics.front().message_key << ':'
                << result.diagnostics.front().detail << '\n';
    }
    Fail(message);
  }
}

void RequireDiagnosticOk(const api::EngineApiDiagnostic& diagnostic,
                         std::string_view message) {
  if (diagnostic.error) {
    std::cerr << diagnostic.code << ':' << diagnostic.message_key << ':'
              << diagnostic.detail << '\n';
    Fail(message);
  }
}

platform::u64 NowMillis() {
  return static_cast<platform::u64>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch()).count());
}

platform::u64 UniqueSeed() {
  static platform::u64 counter = 0;
  return NowMillis() + (++counter * 1000);
}

platform::TypedUuid NewUuid(platform::UuidKind kind, platform::u64 salt) {
  const auto generated = uuid::GenerateEngineIdentityV7(kind, UniqueSeed() + salt);
  Require(generated.ok(), "ODF-044 UUID generation failed");
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
  std::string id_index_uuid;
  platform::u64 salt = 0;

  ~Fixture() {
    std::error_code ignored;
    if (!dir.empty()) { std::filesystem::remove_all(dir, ignored); }
  }
};

api::EngineTypedValue TextValue(std::string value) {
  api::EngineTypedValue typed;
  typed.descriptor.descriptor_kind = "scalar";
  typed.descriptor.canonical_type_name = "character";
  typed.descriptor.encoded_descriptor = "canonical=character";
  typed.encoded_value = std::move(value);
  typed.is_null = typed.encoded_value == "<NULL>";
  return typed;
}

api::EngineRowValue Row(std::string id, std::string city) {
  api::EngineRowValue row;
  row.fields.push_back({"id", TextValue(std::move(id))});
  row.fields.push_back({"city", TextValue(std::move(city))});
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

bool EvidenceKindContains(const std::vector<api::EngineEvidenceReference>& evidence,
                          std::string_view kind,
                          std::string_view token) {
  for (const auto& item : evidence) {
    if (item.evidence_kind == kind &&
        item.evidence_id.find(token) != std::string::npos) {
      return true;
    }
  }
  return false;
}

void AssertNoRuntimeDocLeaks(const std::vector<api::EngineEvidenceReference>& evidence) {
  const std::vector<std::string_view> forbidden = {
      "docs" "/execution-plans", "execution_plan", "findings", "contracts", "references"};
  for (const auto& item : evidence) {
    for (const auto token : forbidden) {
      Require(item.evidence_kind.find(token) == std::string::npos &&
                  item.evidence_id.find(token) == std::string::npos,
              "ODF-044 runtime evidence leaked documentation token");
    }
  }
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
  context.security_context_present = true;
  context.identifier_profile_uuid = "sbsql_v3";
  context.language_context.language_tag = "en";
  context.language_context.default_language_tag = "en";
  context.catalog_generation_id = 44;
  context.security_epoch = 45;
  context.resource_epoch = 46;
  context.name_resolution_epoch = 47;
  return context;
}

api::EngineRequestContext Begin(const Fixture& fixture, std::string request_id) {
  api::EngineBeginTransactionRequest request;
  request.context = BaseContext(fixture, std::move(request_id));
  request.isolation_level = "read_committed";
  const auto begun = api::EngineBeginTransaction(request);
  RequireOk(begun, "ODF-044 begin transaction failed");
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
  RequireOk(api::EngineCommitTransaction(request), "ODF-044 commit failed");
}

api::CrudTableRecord Table(const Fixture& fixture,
                           const api::EngineRequestContext& context) {
  api::CrudTableRecord table;
  table.creator_tx = context.local_transaction_id;
  table.table_uuid = fixture.table_uuid;
  table.default_name = "odf044_sorted_bulk";
  table.columns.push_back({"id", "canonical=character;not_null=true"});
  table.columns.push_back({"city", "canonical=character;not_null=true"});
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
  index.unique = true;
  index.key_envelopes.push_back("id");
  index.key_envelopes.push_back("unique");
  return index;
}

Fixture MakeFixture(std::string name, platform::u64 salt, bool with_index) {
  Fixture fixture;
  fixture.salt = salt;
  fixture.dir = std::filesystem::temp_directory_path() /
                ("scratchbird_odf044_" + name + "_" +
                 std::to_string(UniqueSeed()));
  std::filesystem::create_directories(fixture.dir);
  fixture.database_path = fixture.dir / "odf044.sbdb";

  db::DatabaseCreateConfig create;
  create.path = fixture.database_path.string();
  create.database_uuid = NewUuid(platform::UuidKind::database, salt + 1);
  create.filespace_uuid = NewUuid(platform::UuidKind::filespace, salt + 2);
  create.creation_unix_epoch_millis = UniqueSeed();
  create.require_resource_seed_pack = false;
  create.allow_minimal_resource_bootstrap = true;
  create.allow_overwrite = true;
  const auto created = db::CreateDatabaseFile(create);
  Require(created.ok(), "ODF-044 database create failed");

  fixture.database_uuid = uuid::UuidToString(create.database_uuid.value);
  fixture.table_uuid = NewUuidText(platform::UuidKind::object, salt + 10);
  fixture.id_index_uuid = NewUuidText(platform::UuidKind::object, salt + 11);

  auto metadata = Begin(fixture, "odf044-metadata");
  RequireDiagnosticOk(api::AppendMgaTableMetadata(metadata, Table(fixture, metadata)),
                      "ODF-044 table metadata append failed");
  if (with_index) {
    RequireDiagnosticOk(api::AppendMgaIndexMetadata(metadata, IdIndex(fixture, metadata)),
                        "ODF-044 index metadata append failed");
  }
  Commit(metadata);
  return fixture;
}

api::EngineExecuteImportRowsRequest ImportRequest(
    const Fixture& fixture,
    const api::EngineRequestContext& context,
    std::vector<api::EngineRowValue> rows,
    bool sorted_build) {
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
  request.checkpoint_policy.checkpoint_mode = "disabled";
  request.canonical_rows = std::move(rows);
  request.estimated_row_count =
      static_cast<api::EngineApiU64>(request.canonical_rows.size());
  request.option_envelopes.push_back("copy_append_batching=enabled");
  if (sorted_build) {
    request.option_envelopes.push_back("sorted_bulk_index_build=enabled");
  }
  return request;
}

void CoreBuilderSortsAndRefusesUniqueDuplicates() {
  auto index_uuid = NewUuid(platform::UuidKind::object, 44000);
  auto table_uuid = NewUuid(platform::UuidKind::object, 44001);
  idx::SortedBulkIndexBuildRequest request;
  request.metadata.index_uuid = index_uuid;
  request.metadata.table_uuid = table_uuid;
  request.metadata.family = idx::IndexFamily::btree;
  request.metadata.family_name = "btree";
  request.metadata.semantic_profile = "rowstore_scalar_btree_v1";
  request.metadata.unique = false;
  request.metadata.rebuild = true;
  request.rows.push_back({"b",
                          uuid::UuidToString(NewUuid(platform::UuidKind::row, 44002).value),
                          uuid::UuidToString(NewUuid(platform::UuidKind::row, 44003).value),
                          "b",
                          0});
  request.rows.push_back({"a",
                          uuid::UuidToString(NewUuid(platform::UuidKind::row, 44004).value),
                          uuid::UuidToString(NewUuid(platform::UuidKind::row, 44005).value),
                          "a",
                          1});
  const auto built = idx::BuildSortedExactBulkIndex(request);
  Require(built.ok(), "ODF-044 core builder refused valid input");
  Require(built.bottom_up_build_selected,
          "ODF-044 core builder did not select bottom-up build");
  Require(built.entries.size() == 2 &&
              built.entries[0].encoded_key == "a" &&
              built.entries[1].encoded_key == "b",
          "ODF-044 core builder did not sort exact entries");
  Require(built.sorted_key_run_count == 2,
          "ODF-044 sorted key run counter drifted");
  Require(built.leaf_generation_count == 1 &&
              built.root_generation_count == 1,
          "ODF-044 deterministic leaf/root evidence drifted");

  request.metadata.unique = true;
  request.rows[1].encoded_key = "b";
  const auto duplicate = idx::BuildSortedExactBulkIndex(request);
  Require(!duplicate.ok(), "ODF-044 unique duplicate run was accepted");
  Require(duplicate.diagnostic.diagnostic_code ==
              "SB-INDEX-SORTED-BULK-UNIQUE-DUPLICATE-BATCH",
          "ODF-044 duplicate diagnostic drifted");
}

void DirectBulkUsesSortedExactIndexBuild() {
  auto fixture = MakeFixture("direct", 44100, true);
  auto context = Begin(fixture, "odf044-direct");
  const auto imported = api::EngineExecuteImportRows(
      ImportRequest(fixture,
                    context,
                    {Row("004", "oslo"),
                     Row("001", "zurich"),
                     Row("003", "quito"),
                     Row("002", "berlin")},
                    true));
  RequireOk(imported, "ODF-044 direct sorted bulk import failed");
  Require(imported.inserted_rows == 4,
          "ODF-044 direct sorted row count drifted");
  Require(HasEvidence(imported.evidence,
                      "sorted_bulk_index_build_selected",
                      "true"),
          "ODF-044 sorted build selected evidence missing");
  Require(HasEvidence(imported.evidence,
                      "sorted_bulk_index_exact_append",
                      "mga_index_append_path"),
          "ODF-044 exact append path evidence missing");
  Require(HasEvidence(imported.evidence,
                      "sorted_bulk_index_uniqueness_proof",
                      "sorted_duplicate_runs_absent"),
          "ODF-044 uniqueness proof evidence missing");
  Require(HasEvidence(imported.evidence,
                      "mga_finality_authority",
                      "engine_transaction_inventory"),
          "ODF-044 MGA authority evidence missing");
  AssertNoRuntimeDocLeaks(imported.evidence);
  Commit(context);
}

void CreateIndexBackfillsWithSortedExactBuild() {
  auto fixture = MakeFixture("ddl", 44200, false);
  auto insert_context = Begin(fixture, "odf044-ddl-load");
  const auto imported = api::EngineExecuteImportRows(
      ImportRequest(fixture,
                    insert_context,
                    {Row("001", "zurich"),
                     Row("002", "oslo"),
                     Row("003", "berlin")},
                    false));
  RequireOk(imported, "ODF-044 preload import failed");
  Commit(insert_context);

  auto ddl_context = Begin(fixture, "odf044-create-index");
  const std::string city_index_uuid =
      NewUuidText(platform::UuidKind::object, 44250);
  api::EngineCreateIndexRequest request;
  request.context = ddl_context;
  request.target_object.uuid.canonical = fixture.table_uuid;
  request.target_object.object_kind = "table";
  api::EngineIndexDefinition definition;
  definition.requested_index_uuid.canonical = city_index_uuid;
  definition.physical_profile = api::kCrudIndexProfileRowStoreScalarBtreeV1;
  definition.key_envelopes.push_back("city");
  api::EngineLocalizedName name;
  name.language_tag = "en";
  name.name_class = "canonical";
  name.name = "odf044_city_idx";
  name.default_name = true;
  definition.names.push_back(name);
  request.indexes.push_back(definition);

  const auto created = api::EngineCreateIndex(request);
  RequireOk(created, "ODF-044 create index failed");
  Require(HasEvidence(created.evidence,
                      "sorted_bulk_index_build_route",
                      "ddl.create_index"),
          "ODF-044 DDL sorted route evidence missing");
  Require(HasEvidence(created.evidence,
                      "sorted_bulk_index_root_publish_fence",
                      "mga_index_append_path_after_bottom_up_root_generation"),
          "ODF-044 root publish fence evidence missing");
  Require(HasEvidence(created.evidence,
                      "sorted_bulk_index_retail_append_bypass",
                      "bottom_up_build_selected"),
          "ODF-044 retail append bypass evidence missing");
  AssertNoRuntimeDocLeaks(created.evidence);

  const auto loaded = api::LoadMgaRelationStoreState(ddl_context);
  Require(loaded.ok, "ODF-044 relation store reload failed");
  std::vector<std::string> keys;
  for (const auto& entry : loaded.state.index_entries) {
    if (entry.index_uuid == city_index_uuid) {
      keys.push_back(entry.key_value);
    }
  }
  Require(keys.size() == 3, "ODF-044 DDL exact index entry count drifted");
  Require(keys[0] == "berlin" && keys[1] == "oslo" && keys[2] == "zurich",
          "ODF-044 DDL exact index entries were not sorted");
  Commit(ddl_context);
}

}  // namespace

int main() {
  CoreBuilderSortsAndRefusesUniqueDuplicates();
  DirectBulkUsesSortedExactIndexBuild();
  CreateIndexBackfillsWithSortedExactBuild();
  return EXIT_SUCCESS;
}

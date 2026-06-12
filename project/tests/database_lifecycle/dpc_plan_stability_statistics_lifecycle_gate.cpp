// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "catalog/catalog_object_lifecycle.hpp"
#include "catalog/name_resolution_api.hpp"
#include "database_lifecycle.hpp"
#include "database_lifecycle_test_memory.hpp"
#include "dml/insert_api.hpp"
#include "mga_relation_store/mga_relation_store.hpp"
#include "observability/explain_api.hpp"
#include "observability/performance_optimization_surface.hpp"
#include "observability/show_api.hpp"
#include "optimizer_explain.hpp"
#include "optimizer_request.hpp"
#include "query/plan_api.hpp"
#include "transaction/transaction_api.hpp"
#include "uuid.hpp"

#include <algorithm>
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
namespace opt = scratchbird::engine::optimizer;
namespace plan = scratchbird::engine::planner;
namespace platform = scratchbird::core::platform;
namespace uuid = scratchbird::core::uuid;

// SEARCH_KEY: DPC_PLAN_STABILITY_GATE
constexpr std::string_view kSearchKey = "DPC_PLAN_STABILITY_GATE";

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view message) {
  if (!condition) Fail(message);
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

platform::u64 NowMillis() {
  return static_cast<platform::u64>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());
}

platform::u64 UniqueMillis() {
  static platform::u64 counter = 0;
  return NowMillis() + (++counter * 17);
}

platform::TypedUuid NewUuid(platform::UuidKind kind) {
  const auto generated = uuid::GenerateEngineIdentityV7(kind, UniqueMillis());
  Require(generated.ok(), "DPC-010 UUID generation failed");
  return generated.value;
}

std::string NewUuidText(platform::UuidKind kind) {
  return uuid::UuidToString(NewUuid(kind).value);
}

bool Contains(std::string_view haystack, std::string_view needle) {
  return haystack.find(needle) != std::string_view::npos;
}

bool HasEvidence(const api::EngineApiResult& result,
                 std::string_view kind,
                 std::string_view id = {}) {
  for (const auto& evidence : result.evidence) {
    if (evidence.evidence_kind == kind &&
        (id.empty() || evidence.evidence_id == id)) {
      return true;
    }
  }
  return false;
}

std::string EvidenceValue(const api::EngineApiResult& result,
                          std::string_view kind) {
  for (const auto& evidence : result.evidence) {
    if (evidence.evidence_kind == kind) {
      return evidence.evidence_id;
    }
  }
  return {};
}

std::string FieldValue(const api::EngineApiResult& result,
                       std::string_view field_name) {
  for (const auto& row : result.result_shape.rows) {
    for (const auto& field : row.fields) {
      if (field.first == field_name) {
        return field.second.encoded_value;
      }
    }
  }
  return {};
}

api::EngineLocalizedName Name(std::string value) {
  api::EngineLocalizedName name;
  name.language_tag = "en";
  name.name_class = "primary";
  name.name = value;
  name.raw_name_text = value;
  name.display_name = value;
  name.default_name = true;
  return name;
}

api::EngineTypedValue Int64Value(std::int64_t value) {
  api::EngineTypedValue typed;
  typed.descriptor.descriptor_kind = "scalar";
  typed.descriptor.canonical_type_name = "int64";
  typed.descriptor.encoded_descriptor = "canonical=int64";
  typed.encoded_value = std::to_string(value);
  return typed;
}

api::EngineTypedValue TextValue(std::string value) {
  api::EngineTypedValue typed;
  typed.descriptor.descriptor_kind = "scalar";
  typed.descriptor.canonical_type_name = "character";
  typed.descriptor.encoded_descriptor = "canonical=character";
  typed.encoded_value = std::move(value);
  return typed;
}

api::EngineRequestContext BaseContext(const std::filesystem::path& path,
                                      const std::string& database_uuid,
                                      const std::string& principal_uuid,
                                      std::string request_id) {
  api::EngineRequestContext context;
  context.trust_mode = api::EngineTrustMode::server_isolated;
  context.request_id = std::move(request_id);
  context.database_path = path.string();
  context.database_uuid.canonical = database_uuid;
  context.principal_uuid.canonical = principal_uuid;
  context.session_uuid.canonical = NewUuidText(platform::UuidKind::object);
  context.security_context_present = true;
  context.identifier_profile_uuid = "sbsql_v3";
  context.language_context.language_tag = "en";
  context.language_context.default_language_tag = "en";
  context.catalog_generation_id = 1;
  context.security_epoch = 17;
  context.resource_epoch = 19;
  context.name_resolution_epoch = 1;
  return context;
}

api::EngineRequestContext Begin(const std::filesystem::path& path,
                                const std::string& database_uuid,
                                const std::string& principal_uuid,
                                std::string request_id) {
  api::EngineBeginTransactionRequest request;
  request.context =
      BaseContext(path, database_uuid, principal_uuid, std::move(request_id));
  request.isolation_level = "read_committed";
  const auto begun = api::EngineBeginTransaction(request);
  RequireOk(begun, "DPC-010 begin transaction failed");
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
  RequireOk(api::EngineCommitTransaction(request), "DPC-010 commit failed");
}

void Rollback(const api::EngineRequestContext& context) {
  api::EngineRollbackTransactionRequest request;
  request.context = context;
  RequireOk(api::EngineRollbackTransaction(request), "DPC-010 rollback failed");
}

std::string CreateDatabase(const std::filesystem::path& path) {
  db::DatabaseCreateConfig create;
  create.path = path.string();
  create.database_uuid = NewUuid(platform::UuidKind::database);
  create.filespace_uuid = NewUuid(platform::UuidKind::filespace);
  create.page_size = 16384;
  create.creation_unix_epoch_millis = NowMillis();
  create.allow_minimal_resource_bootstrap = true;
  create.require_resource_seed_pack = false;
  create.allow_overwrite = true;
  const auto created = db::CreateDatabaseFile(create);
  if (!created.ok()) {
    std::cerr << created.diagnostic.diagnostic_code << ':'
              << created.diagnostic.message_key << '\n';
  }
  Require(created.ok(), "DPC-010 database create failed");
  return uuid::UuidToString(create.database_uuid.value);
}

api::EngineCatalogCreateObjectRequest CreateObjectRequest(
    const api::EngineRequestContext& context,
    const std::string& object_uuid,
    std::string object_kind,
    const std::string& schema_uuid,
    std::string name) {
  api::EngineCatalogCreateObjectRequest request;
  request.context = context;
  request.target_object.uuid.canonical = object_uuid;
  request.target_object.object_kind = std::move(object_kind);
  request.target_schema.uuid.canonical = schema_uuid;
  request.localized_names.push_back(Name(std::move(name)));
  return request;
}

api::EngineResolveNameRequest ResolveRequest(
    const api::EngineRequestContext& context,
    const std::string& schema_uuid,
    std::string object_kind,
    std::string name) {
  api::EngineResolveNameRequest request;
  request.context = context;
  request.target_schema.uuid.canonical = schema_uuid;
  request.target_object.object_kind = std::move(object_kind);
  request.localized_names.push_back(Name(std::move(name)));
  return request;
}

api::EngineQueryRelation InlineCountRelation(const std::string& table_uuid,
                                             const std::string& relation_name) {
  api::EngineQueryRelation relation;
  relation.relation_name = relation_name;
  relation.source_object.uuid.canonical = table_uuid;
  relation.source_object.object_kind = "table";
  relation.descriptor_digest = "generated-descriptor:" + table_uuid;
  for (int value : {1, 2, 3}) {
    api::EngineRowValue row;
    row.fields.push_back({"id", Int64Value(value)});
    relation.rows.push_back(std::move(row));
  }
  return relation;
}

api::EnginePlanOperationRequest CachedCountRequest(
    api::EngineRequestContext context,
    const std::string& table_uuid,
    const std::string& relation_name,
    const std::string& sblr_digest,
    const std::string& statistics_snapshot_id) {
  api::EnginePlanOperationRequest request;
  request.context = std::move(context);
  request.execute = true;
  request.query_operation = "count";
  request.target_object.uuid.canonical = table_uuid;
  request.target_object.object_kind = "table";
  request.relations.push_back(InlineCountRelation(table_uuid, relation_name));
  request.option_envelopes.push_back("optimizer_plan_cache:enabled");
  request.option_envelopes.push_back("sblr_digest:" + sblr_digest);
  request.option_envelopes.push_back("statistics_snapshot_id:" +
                                     statistics_snapshot_id);
  return request;
}

std::string CountValue(const api::EnginePlanOperationResult& result) {
  Require(!result.result_shape.rows.empty(), "DPC-010 count result had no row");
  Require(!result.result_shape.rows.front().fields.empty(),
          "DPC-010 count result had no field");
  return result.result_shape.rows.front().fields.front().second.encoded_value;
}

void RequireSameCountResult(const api::EnginePlanOperationResult& lhs,
                            const api::EnginePlanOperationResult& rhs,
                            std::string_view message) {
  Require(lhs.plan_kind == rhs.plan_kind, message);
  Require(lhs.output_row_count == rhs.output_row_count, message);
  Require(CountValue(lhs) == CountValue(rhs), message);
}

struct CatalogFixture {
  std::filesystem::path dir;
  std::filesystem::path database_path;
  std::string database_uuid;
  std::string principal_uuid;
  std::string schema_uuid;
  std::string table_uuid;
  std::string table_name;
  std::string renamed_table_name;
  api::EngineApiU64 table_catalog_epoch = 0;

  ~CatalogFixture() {
    std::error_code ignored;
    if (!dir.empty()) std::filesystem::remove_all(dir, ignored);
  }
};

CatalogFixture MakeCatalogFixture() {
  CatalogFixture fixture;
  fixture.dir = std::filesystem::temp_directory_path() /
                ("scratchbird_dpc010_catalog_" + std::to_string(UniqueMillis()));
  std::filesystem::create_directories(fixture.dir);
  fixture.database_path = fixture.dir / "dpc010_catalog.sbdb";
  fixture.database_uuid = CreateDatabase(fixture.database_path);
  fixture.principal_uuid = NewUuidText(platform::UuidKind::principal);
  fixture.schema_uuid = NewUuidText(platform::UuidKind::schema);
  fixture.table_uuid = NewUuidText(platform::UuidKind::object);
  fixture.table_name = "dpc010_plan_table_" + std::to_string(UniqueMillis());
  fixture.renamed_table_name = fixture.table_name + "_renamed";

  auto schema_context = Begin(fixture.database_path,
                              fixture.database_uuid,
                              fixture.principal_uuid,
                              "dpc010-create-schema");
  const auto created_schema =
      api::EngineCatalogCreateObject(CreateObjectRequest(schema_context,
                                                         fixture.schema_uuid,
                                                         "schema",
                                                         {},
                                                         "dpc010_plan_schema"));
  RequireOk(created_schema, "DPC-010 schema create failed");
  Commit(schema_context);

  auto table_context = Begin(fixture.database_path,
                             fixture.database_uuid,
                             fixture.principal_uuid,
                             "dpc010-create-table");
  table_context.catalog_generation_id = created_schema.metadata_cache_epoch;
  table_context.name_resolution_epoch = created_schema.metadata_cache_epoch;
  const auto created_table =
      api::EngineCatalogCreateObject(CreateObjectRequest(table_context,
                                                         fixture.table_uuid,
                                                         "table",
                                                         fixture.schema_uuid,
                                                         fixture.table_name));
  RequireOk(created_table, "DPC-010 table create failed");
  fixture.table_catalog_epoch = created_table.metadata_cache_epoch;
  Commit(table_context);

  return fixture;
}

void TestPlanCacheStabilityAndInvalidation() {
  auto fixture = MakeCatalogFixture();
  auto read_context = Begin(fixture.database_path,
                            fixture.database_uuid,
                            fixture.principal_uuid,
                            "dpc010-cache-read");
  read_context.catalog_generation_id = fixture.table_catalog_epoch;
  read_context.name_resolution_epoch = fixture.table_catalog_epoch;
  const auto resolved =
      api::EngineResolveName(ResolveRequest(read_context,
                                            fixture.schema_uuid,
                                            "table",
                                            fixture.table_name));
  RequireOk(resolved, "DPC-010 resolver lookup failed");
  Require(resolved.bound_object_identity.object_uuid.canonical ==
              fixture.table_uuid,
          "DPC-010 resolver did not bind generated table UUID");

  const std::string sblr_digest = std::string(kSearchKey) + ":sblr:" +
                                  NewUuidText(platform::UuidKind::object);
  const std::string stats_a = std::string(kSearchKey) + ":stats:" +
                              NewUuidText(platform::UuidKind::object);
  const std::string stats_b = std::string(kSearchKey) + ":stats:" +
                              NewUuidText(platform::UuidKind::object);

  const auto first = api::EnginePlanOperation(
      CachedCountRequest(read_context,
                         fixture.table_uuid,
                         fixture.table_name,
                         sblr_digest,
                         stats_a));
  RequireOk(first, "DPC-010 first cached plan failed");
  Require(HasEvidence(first, "optimizer_live_plan_cache", "miss"),
          "DPC-010 first cached plan did not miss");
  Require(HasEvidence(first,
                      "optimizer_live_plan_cache_binding",
                      "descriptor:" + fixture.table_uuid),
          "DPC-010 plan cache did not bind descriptor UUID");
  Require(CountValue(first) == "3", "DPC-010 first count result drifted");
  const auto first_key = EvidenceValue(first, "optimizer_live_plan_cache_key");
  Require(!first_key.empty(), "DPC-010 first cache key missing");

  const auto second = api::EnginePlanOperation(
      CachedCountRequest(read_context,
                         fixture.table_uuid,
                         fixture.table_name,
                         sblr_digest,
                         stats_a));
  RequireOk(second, "DPC-010 second cached plan failed");
  Require(HasEvidence(second, "optimizer_live_plan_cache", "hit"),
          "DPC-010 identical snapshots did not hit plan cache");
  Require(EvidenceValue(second, "optimizer_live_plan_cache_key") == first_key,
          "DPC-010 identical snapshots changed plan cache key");
  RequireSameCountResult(first,
                         second,
                         "DPC-010 cache hit was not result deterministic");

  const auto stats_changed = api::EnginePlanOperation(
      CachedCountRequest(read_context,
                         fixture.table_uuid,
                         fixture.table_name,
                         sblr_digest,
                         stats_b));
  RequireOk(stats_changed, "DPC-010 changed statistics plan failed");
  Require(HasEvidence(stats_changed, "optimizer_live_plan_cache", "miss"),
          "DPC-010 changed statistics snapshot did not miss plan cache");
  Require(EvidenceValue(stats_changed, "optimizer_live_plan_cache_key") !=
              first_key,
          "DPC-010 changed statistics snapshot reused plan cache key");
  RequireSameCountResult(first,
                         stats_changed,
                         "DPC-010 statistics miss changed query result");
  Rollback(read_context);

  auto rename_context = Begin(fixture.database_path,
                              fixture.database_uuid,
                              fixture.principal_uuid,
                              "dpc010-rename-table");
  rename_context.catalog_generation_id = fixture.table_catalog_epoch;
  rename_context.name_resolution_epoch = fixture.table_catalog_epoch;
  api::EngineCatalogRenameObjectRequest rename;
  rename.context = rename_context;
  rename.target_object.uuid.canonical = fixture.table_uuid;
  rename.target_object.object_kind = "table";
  rename.localized_names.push_back(Name(fixture.renamed_table_name));
  const auto renamed = api::EngineCatalogRenameObject(rename);
  RequireOk(renamed, "DPC-010 table rename failed");
  Require(renamed.metadata_cache_epoch > fixture.table_catalog_epoch,
          "DPC-010 rename did not advance catalog/name epoch");
  Commit(rename_context);

  auto after_ddl_context = Begin(fixture.database_path,
                                 fixture.database_uuid,
                                 fixture.principal_uuid,
                                 "dpc010-cache-after-ddl");
  after_ddl_context.catalog_generation_id = renamed.metadata_cache_epoch;
  after_ddl_context.name_resolution_epoch = renamed.metadata_cache_epoch;
  const auto after_ddl = api::EnginePlanOperation(
      CachedCountRequest(after_ddl_context,
                         fixture.table_uuid,
                         fixture.renamed_table_name,
                         sblr_digest,
                         stats_a));
  RequireOk(after_ddl, "DPC-010 DDL epoch plan failed");
  Require(HasEvidence(after_ddl, "optimizer_live_plan_cache", "miss"),
          "DPC-010 changed catalog/name epoch did not miss plan cache");
  Require(EvidenceValue(after_ddl, "optimizer_live_plan_cache_key") !=
              first_key,
          "DPC-010 changed catalog/name epoch reused plan cache key");
  RequireSameCountResult(first,
                         after_ddl,
                         "DPC-010 catalog/name miss changed query result");
  Rollback(after_ddl_context);
}

struct CrudFixture {
  std::filesystem::path dir;
  std::filesystem::path database_path;
  std::string database_uuid;
  std::string principal_uuid;
  std::string table_uuid;
  std::string index_uuid;
  api::EngineRequestContext context;

  ~CrudFixture() {
    std::error_code ignored;
    if (!dir.empty()) std::filesystem::remove_all(dir, ignored);
  }
};

api::CrudTableRecord TableRecord(const CrudFixture& fixture) {
  api::CrudTableRecord table;
  table.creator_tx = fixture.context.local_transaction_id;
  table.table_uuid = fixture.table_uuid;
  table.default_name = "dpc010_statistics_access_path";
  table.columns.push_back({"id", "canonical=character"});
  table.columns.push_back({"note", "canonical=character"});
  return table;
}

api::CrudIndexRecord CoveringIndexRecord(const CrudFixture& fixture) {
  api::CrudIndexRecord index;
  index.creator_tx = fixture.context.local_transaction_id;
  index.index_uuid = fixture.index_uuid;
  index.table_uuid = fixture.table_uuid;
  index.column_name = "id";
  index.family = api::kCrudIndexFamilyBtree;
  index.profile = api::kCrudIndexProfileRowStoreScalarBtreeV1;
  index.unique = true;
  index.key_envelopes.push_back("id");
  index.key_envelopes.push_back("unique");
  index.include_columns.push_back("note");
  return index;
}

api::EngineRowValue TextRow(std::string id, std::string note) {
  api::EngineRowValue row;
  row.fields.push_back({"id", TextValue(std::move(id))});
  row.fields.push_back({"note", TextValue(std::move(note))});
  return row;
}

CrudFixture MakeCrudFixture() {
  CrudFixture fixture;
  fixture.dir = std::filesystem::temp_directory_path() /
                ("scratchbird_dpc010_crud_" + std::to_string(UniqueMillis()));
  std::filesystem::create_directories(fixture.dir);
  fixture.database_path = fixture.dir / "dpc010_crud.sbdb";
  fixture.database_uuid = CreateDatabase(fixture.database_path);
  fixture.principal_uuid = NewUuidText(platform::UuidKind::principal);
  fixture.table_uuid = NewUuidText(platform::UuidKind::object);
  fixture.index_uuid = NewUuidText(platform::UuidKind::object);
  fixture.context = Begin(fixture.database_path,
                          fixture.database_uuid,
                          fixture.principal_uuid,
                          "dpc010-crud-metadata");

  const auto table = api::AppendMgaTableMetadata(fixture.context,
                                                 TableRecord(fixture));
  Require(!table.error, "DPC-010 table metadata append failed");
  const auto index = api::AppendMgaIndexMetadata(fixture.context,
                                                 CoveringIndexRecord(fixture));
  Require(!index.error, "DPC-010 index metadata append failed");

  std::vector<api::EngineRowValue> rows;
  rows.reserve(128);
  for (int i = 0; i < 128; ++i) {
    rows.push_back(TextRow("id-" + std::to_string(i),
                           "note-" + std::to_string(i)));
  }

  api::EngineInsertRowsRequest insert;
  insert.context = fixture.context;
  insert.context.request_id = "dpc010-insert-fixture";
  insert.target_table.uuid.canonical = fixture.table_uuid;
  insert.target_table.object_kind = "table";
  insert.input_rows = std::move(rows);
  insert.estimated_row_count = insert.input_rows.size();
  const auto inserted = api::EngineInsertRows(insert);
  RequireOk(inserted, "DPC-010 fixture insert failed");
  Require(inserted.inserted_count == 128, "DPC-010 insert count mismatch");
  return fixture;
}

api::EnginePredicateEnvelope Predicate(std::string kind,
                                       std::vector<std::string> values) {
  api::EnginePredicateEnvelope predicate;
  predicate.predicate_kind = std::move(kind);
  predicate.canonical_predicate_envelope = "id";
  for (auto& value : values) {
    predicate.bound_values.push_back(TextValue(std::move(value)));
  }
  return predicate;
}

api::EnginePlanOperationResult PlanCrud(CrudFixture& fixture,
                                        api::EnginePredicateEnvelope predicate,
                                        std::vector<std::string> options = {}) {
  api::EnginePlanOperationRequest request;
  request.context = fixture.context;
  request.context.request_id = "dpc010-crud-plan";
  request.target_object.uuid.canonical = fixture.table_uuid;
  request.target_object.object_kind = "table";
  request.predicate = std::move(predicate);
  request.option_envelopes = std::move(options);
  return api::EnginePlanOperation(request);
}

void RequireOptimizerSelectionEvidence(const api::EnginePlanOperationResult& result) {
  Require(HasEvidence(result, "optimizer_profile"),
          "DPC-010 optimizer_profile evidence missing");
  Require(HasEvidence(result, "logical_plan_id"),
          "DPC-010 logical_plan_id evidence missing");
  Require(HasEvidence(result, "optimizer_candidate"),
          "DPC-010 optimizer_candidate evidence missing");
  Require(HasEvidence(result, "optimizer_selected_candidate"),
          "DPC-010 optimizer_selected_candidate evidence missing");
  Require(HasEvidence(result, "optimizer_selected_access"),
          "DPC-010 optimizer_selected_access evidence missing");
  Require(HasEvidence(result, "optimizer_statistics_version"),
          "DPC-010 optimizer_statistics_version evidence missing");
}

void TestStatisticsFallbackAndSelectionEvidence(CrudFixture& fixture) {
  const auto lookup = PlanCrud(fixture, Predicate("column_equals", {"id-7"}));
  RequireOk(lookup, "DPC-010 equality lookup plan failed");
  Require(lookup.plan_kind == "scalar_btree_lookup",
          "DPC-010 equality predicate did not select scalar btree lookup");
  Require(HasEvidence(lookup, "optimizer_access_path_index", fixture.index_uuid),
          "DPC-010 lookup did not bind generated index UUID");
  Require(HasEvidence(lookup,
                      "optimizer_selected_access",
                      "scalar_btree_lookup"),
          "DPC-010 selected access evidence mismatch");
  RequireOptimizerSelectionEvidence(lookup);

  const auto stale = PlanCrud(fixture,
                              Predicate("column_equals", {"id-9"}),
                              {"statistics_stale:true"});
  RequireOk(stale, "DPC-010 stale-stat fallback plan failed");
  Require(stale.plan_kind == "table_scan",
          "DPC-010 stale statistics did not fail safe to table scan");
  Require(HasEvidence(stale,
                      "optimizer_access_path_fallback",
                      "stale_or_missing_relation_statistics_scan"),
          "DPC-010 stale-stat fallback evidence missing");

  const auto missing = PlanCrud(fixture,
                                Predicate("column_equals", {"id-10"}),
                                {"optimizer_statistics:disabled"});
  RequireOk(missing, "DPC-010 missing-stat fallback plan failed");
  Require(missing.plan_kind == "table_scan",
          "DPC-010 missing statistics did not fail safe to table scan");
  Require(HasEvidence(missing,
                      "optimizer_access_path_fallback",
                      "stale_or_missing_relation_statistics_scan"),
          "DPC-010 missing-stat fallback evidence missing");
}

void TestExplainAndOptimizerEvidence(CrudFixture& fixture) {
  api::EngineExplainOperationRequest explain;
  explain.context = fixture.context;
  explain.context.request_id = "dpc010-explain";
  explain.operation_id = "query.scan";
  explain.target_object.uuid.canonical = fixture.table_uuid;
  explain.target_object.object_kind = "table";
  explain.predicate = Predicate("column_equals", {"id-11"});
  const auto explained = api::EngineExplainOperation(explain);
  RequireOk(explained, "DPC-010 EXPLAIN operation failed");
  Require(HasEvidence(explained, "explain", "query.scan"),
          "DPC-010 EXPLAIN evidence missing");
  Require(HasEvidence(explained, "optimizer_selected_candidate"),
          "DPC-010 EXPLAIN did not expose selected candidate evidence");
  Require(HasEvidence(explained, "optimizer_candidate"),
          "DPC-010 EXPLAIN did not expose optimizer candidates");
  Require(HasEvidence(explained, "optimizer_statistics_version"),
          "DPC-010 EXPLAIN did not expose statistics version evidence");
  Require(!HasEvidence(explained, "parser_finality_authority", "true"),
          "DPC-010 EXPLAIN claimed parser finality authority");

  opt::BoundOptimizerRequest request;
  request.context.request_uuid = NewUuidText(platform::UuidKind::object);
  request.context.operation_id = "query.scan";
  request.context.sblr_digest =
      std::string(kSearchKey) + ":bound-sblr:" + fixture.table_uuid;
  request.context.descriptor_set_digest = "descriptor:" + fixture.table_uuid;
  request.context.statistics_snapshot_id =
      std::string(kSearchKey) + ":snapshot:" +
      NewUuidText(platform::UuidKind::object);
  request.context.metric_snapshot_id =
      std::string(kSearchKey) + ":metric:" +
      NewUuidText(platform::UuidKind::object);
  request.context.executor_capability_set_id = "local_noncluster_executor";
  request.context.catalog_epoch = 23;
  request.context.security_epoch = 29;
  request.context.policy_epoch = 31;
  request.context.security_context_present = true;
  request.context.transaction_context_present = true;
  request.authority_facts.push_back(opt::MakeAuthorityFact(
      "client_statement",
      opt::OptimizerAuthorityStatus::kRedacted,
      false,
      "client statement is not optimizer authority"));

  request.logical_plan.ok = true;
  request.logical_plan.plan_id =
      std::string(kSearchKey) + ":logical-plan:" + fixture.table_uuid;
  auto node = plan::MakeLogicalPlanNode(plan::LogicalPlanNodeKind::kDmlRead,
                                        plan::PhysicalAccessKind::kTableScan,
                                        "dpc_plan_stability_statistics_lifecycle_gate",
                                        "scan");
  node.required_object_uuids.push_back(fixture.table_uuid);
  node.required_descriptors.push_back("descriptor:" + fixture.table_uuid);
  request.logical_plan.nodes.push_back(std::move(node));
  request.statistics = opt::DefaultLocalStatisticsCatalog();

  const auto optimized = opt::OptimizeBoundRequest(request);
  Require(optimized.ok, "DPC-010 bound optimizer request failed");
  Require(!optimized.candidates.empty(),
          "DPC-010 optimizer result had no candidates");
  Require(std::any_of(optimized.candidates.begin(),
                      optimized.candidates.end(),
                      [](const opt::PlanCandidate& candidate) {
                        return candidate.selected;
                      }),
          "DPC-010 optimizer result had no selected candidate");

  auto document = opt::BuildOptimizerExplainDocument(request, optimized);
  document.redactions.push_back("client_statement_redacted");
  document.diagnostics.push_back(
      "DPC_PLAN_STABILITY_GATE_DIAGNOSTIC_VECTOR_PRESENT");
  const auto json = opt::RenderOptimizerExplainJson(document);
  Require(Contains(json, "\"selected_candidate_id\""),
          "DPC-010 optimizer explain selected candidate missing");
  Require(Contains(json, request.context.statistics_snapshot_id),
          "DPC-010 optimizer explain statistics snapshot id missing");
  Require(Contains(json, "\"candidates\""),
          "DPC-010 optimizer explain candidates missing");
  Require(Contains(json, "\"authority_facts\""),
          "DPC-010 optimizer explain authority facts missing");
  Require(Contains(json, "\"diagnostics\""),
          "DPC-010 optimizer explain diagnostics missing");
  Require(Contains(json, "\"redactions\""),
          "DPC-010 optimizer explain redactions missing");
  Require(!Contains(json, "SELECT ") && !Contains(json, "select "),
          "DPC-010 optimizer explain leaked SQL text");
  Require(!Contains(json, "parser_finality_authority"),
          "DPC-010 optimizer explain exposed parser finality authority");

  auto parser_claim_request = request;
  parser_claim_request.context.request_uuid =
      NewUuidText(platform::UuidKind::object);
  parser_claim_request.context.parser_owned_claims_present = true;
  const auto refused = opt::OptimizeBoundRequest(parser_claim_request);
  Require(!refused.ok, "DPC-010 parser-owned optimizer claim was accepted");
  auto refused_document =
      opt::BuildOptimizerExplainDocument(parser_claim_request, refused);
  const auto refused_json = opt::RenderOptimizerExplainJson(refused_document);
  Require(Contains(refused_json, "SB_OPT_AUTHORITY_REJECTED.parser_owned_claims"),
          "DPC-010 parser-owned claim refusal diagnostic missing");
}

api::PerformanceOptimizationSurfaceSnapshot RichSurfaceSnapshot() {
  auto snapshot = api::DefaultPerformanceOptimizationSurfaceSnapshot();
  snapshot.optimization_profile = "dpc010_plan_stability_statistics";
  snapshot.plan_cache_hits = 1;
  snapshot.plan_cache_misses = 2;
  snapshot.plan_cache_invalidations = 2;
  snapshot.plan_cache_last_invalidation_reason =
      "statistics_snapshot_or_catalog_name_epoch_changed";
  snapshot.statistics_epoch = 37;
  snapshot.stale_statistics_fail_safe_active = true;
  snapshot.stale_statistics_fail_safe_reason =
      "stale_or_missing_relation_statistics_scan";
  snapshot.selected_join_algorithm = "hash";
  snapshot.selected_join_plan_summary =
      "hash_join:left=dpc010_orders:right=dpc010_items";
  snapshot.selected_join_left_rows = 128;
  snapshot.selected_join_right_rows = 512;
  snapshot.selected_join_from_statistics = true;
  snapshot.selected_join_statistics_version =
      std::string(kSearchKey) + ":stats-version";
  snapshot.support_bundle_correlation_id =
      std::string(kSearchKey) + ":support-bundle";
  snapshot.request_correlation_id =
      std::string(kSearchKey) + ":management-request";
  snapshot.benchmark_correlation_id =
      std::string(kSearchKey) + ":benchmark-correlation";
  return snapshot;
}

void TestPerformanceOptimizationSurface() {
  for (const auto& field : api::PerformanceOptimizationSurfaceSchema()) {
    Require(!Contains(field.name, "uuid"),
            "DPC-010 user-facing schema leaked UUID field");
  }

  api::EngineRequestContext context;
  context.security_context_present = true;
  context.request_id = "dpc010-performance-surface";
  context.catalog_generation_id = 37;
  context.security_epoch = 41;
  context.resource_epoch = 43;
  context.trace_tags = {"DPC-010",
                        "DPC_TEST_PLAN_STABILITY",
                        "right:OBS_INDEX_PROFILE_READ",
                        std::string(kSearchKey)};
  scratchbird::tests::database_lifecycle::MaterializeAuthorizationRights(
      &context,
      "dpc_plan_stability_statistics_lifecycle_gate",
      {"OBS_INDEX_PROFILE_READ",
       "OBS_MANAGEMENT_INSPECT",
       "MGA_CLEANUP_INSPECT"});

  api::EngineInspectPerformanceOptimizationSurfaceRequest request;
  request.context = context;
  request.snapshot = RichSurfaceSnapshot();
  request.snapshot_present = true;
  const auto result = api::EngineInspectPerformanceOptimizationSurface(request);
  RequireOk(result, "DPC-010 performance optimization surface failed");
  Require(result.management_api_ready,
          "DPC-010 management API surface not ready");
  Require(result.support_bundle_ready,
          "DPC-010 support bundle surface not ready");
  Require(FieldValue(result, "plan_cache_hits") == "1",
          "DPC-010 plan cache hit counter missing");
  Require(FieldValue(result, "plan_cache_misses") == "2",
          "DPC-010 plan cache miss counter missing");
  Require(FieldValue(result, "plan_cache_invalidations") == "2",
          "DPC-010 plan cache invalidation counter missing");
  Require(FieldValue(result, "stale_statistics_fail_safe_active") == "true",
          "DPC-010 stale-stat fail-safe state missing");
  Require(FieldValue(result, "stale_statistics_fail_safe_reason") ==
              "stale_or_missing_relation_statistics_scan",
          "DPC-010 stale-stat fail-safe reason missing");
  Require(FieldValue(result, "selected_join_plan_summary") ==
              "hash_join:left=dpc010_orders:right=dpc010_items",
          "DPC-010 selected join plan summary missing");
  Require(Contains(result.management_api_json, "\"plan_cache_hits\":1"),
          "DPC-010 management JSON missing plan cache counters");
  Require(Contains(result.management_api_json,
                   "\"stale_statistics_fail_safe_active\":true"),
          "DPC-010 management JSON missing stale-stat state");
  Require(Contains(result.support_bundle_json,
                   "\"selected_join_plan_summary\""),
          "DPC-010 support bundle missing selected plan summary");
  Require(Contains(result.support_bundle_json,
                   "\"forbidden_fields_absent\":true"),
          "DPC-010 support bundle redaction proof missing");
  Require(!Contains(result.management_api_json, "docs" "/execution-plans"),
          "DPC-010 management surface depends on execution_plan path");
  Require(!Contains(result.support_bundle_json, "docs" "/execution-plans"),
          "DPC-010 support bundle depends on execution_plan path");
  Require(!result.parser_finality_authority && !result.reference_finality_authority,
          "DPC-010 surface claimed parser or reference finality");

  api::EngineShowManagementRequest show_request;
  show_request.context = context;
  const auto show = api::EngineShowManagement(show_request);
  RequireOk(show, "DPC-010 SHOW MANAGEMENT failed");
  Require(HasEvidence(show,
                      "management_performance_optimization_surface",
                      api::PerformanceOptimizationSurfaceSchemaId()),
          "DPC-010 SHOW MANAGEMENT surface evidence missing");
}

}  // namespace

int main() {
  scratchbird::tests::database_lifecycle::ConfigureLifecycleMemoryFixture(
      "dpc_plan_stability_statistics_lifecycle_gate");
  TestPlanCacheStabilityAndInvalidation();
  auto crud = MakeCrudFixture();
  TestStatisticsFallbackAndSelectionEvidence(crud);
  TestExplainAndOptimizerEvidence(crud);
  TestPerformanceOptimizationSurface();
  Rollback(crud.context);
  std::cout << "dpc_plan_stability_statistics_lifecycle_gate=passed\n";
  return EXIT_SUCCESS;
}

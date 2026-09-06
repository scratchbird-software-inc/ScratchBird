// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "catalog/name_registry.hpp"
#include "crud_support/crud_store.hpp"
#include "database_lifecycle.hpp"
#include "ddl/create_api.hpp"
#include "dml/insert_api.hpp"
#include "dml/select_api.hpp"
#include "mga_relation_store/mga_relation_store.hpp"
#include "dml/mga_relation_read_view.hpp"
#include "transaction/transaction_api.hpp"
#include "uuid.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <unistd.h>

namespace {

namespace api = scratchbird::engine::internal_api;
namespace db = scratchbird::storage::database;
namespace uuid = scratchbird::core::uuid;
using scratchbird::core::platform::UuidKind;

void Require(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << message << '\n';
    std::exit(EXIT_FAILURE);
  }
}

template <typename TResult>
void RequireOk(const TResult& result, std::string_view message) {
  if (!result.ok) {
    for (const auto& diagnostic : result.diagnostics) {
      std::cerr << diagnostic.code << ':' << diagnostic.detail << '\n';
    }
  }
  Require(result.ok, message);
}

template <typename TResult>
bool HasDiagnostic(const TResult& result,
                   std::string_view code,
                   std::string_view detail_prefix = {}) {
  for (const auto& diagnostic : result.diagnostics) {
    if (diagnostic.code != code) { continue; }
    if (detail_prefix.empty() ||
        diagnostic.detail.rfind(detail_prefix, 0) == 0) {
      return true;
    }
  }
  return false;
}

template <typename TResult>
bool HasDiagnosticDetail(const TResult& result, std::string_view detail_prefix) {
  for (const auto& diagnostic : result.diagnostics) {
    if (diagnostic.detail.rfind(detail_prefix, 0) == 0) { return true; }
  }
  return false;
}

std::uint64_t NowMillis() {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());
}

std::string GeneratedUuid(UuidKind kind, std::uint64_t offset) {
  const auto generated =
      uuid::GenerateEngineIdentityV7(kind, NowMillis() + offset);
  Require(generated.ok(), "test identity UUID generation failed");
  return uuid::UuidToString(generated.value.value);
}

struct DatabaseFixture {
  std::filesystem::path directory;
  std::filesystem::path path;
  std::string database_uuid;

  DatabaseFixture() = default;
  DatabaseFixture(const DatabaseFixture&) = delete;
  DatabaseFixture& operator=(const DatabaseFixture&) = delete;
  DatabaseFixture(DatabaseFixture&& other) noexcept
      : directory(std::move(other.directory)),
        path(std::move(other.path)),
        database_uuid(std::move(other.database_uuid)) {
    other.directory.clear();
    other.path.clear();
  }
  DatabaseFixture& operator=(DatabaseFixture&&) = delete;

  ~DatabaseFixture() {
    if (directory.empty()) { return; }
    std::error_code error;
    std::filesystem::remove_all(directory, error);
  }
};

DatabaseFixture CreateDatabase() {
  DatabaseFixture fixture;
  fixture.directory =
      std::filesystem::temp_directory_path() /
      ("sb_sblr_composite_key_" + std::to_string(NowMillis()) + "_" +
       std::to_string(static_cast<long long>(getpid())));
  std::filesystem::create_directories(fixture.directory);
  fixture.path = fixture.directory / "fixture.sbdb";
  db::DatabaseCreateConfig create;
  create.path = fixture.path.string();
  create.database_uuid =
      uuid::GenerateEngineIdentityV7(UuidKind::database, NowMillis() + 1).value;
  create.filespace_uuid =
      uuid::GenerateEngineIdentityV7(UuidKind::filespace, NowMillis() + 2).value;
  create.page_size = 16384;
  create.creation_unix_epoch_millis = NowMillis() + 3;
  create.allow_minimal_resource_bootstrap = true;
  create.require_resource_seed_pack = false;
  create.allow_overwrite = true;
  const auto created = db::CreateDatabaseFile(create);
  if (!created.ok()) {
    std::cerr << created.diagnostic.diagnostic_code << ':'
              << created.diagnostic.message_key << '\n';
  }
  Require(created.ok(), "composite-key database create failed");
  fixture.database_uuid = uuid::UuidToString(create.database_uuid.value);
  return fixture;
}

api::EngineRequestContext BaseContext(const DatabaseFixture& fixture,
                                      const std::string& schema_uuid,
                                      std::uint64_t session_ordinal) {
  api::EngineRequestContext context;
  context.trust_mode = api::EngineTrustMode::server_isolated;
  context.request_id = "sblr-composite-key-authority";
  context.database_path = fixture.path.string();
  context.database_uuid.canonical = fixture.database_uuid;
  context.principal_uuid.canonical = GeneratedUuid(UuidKind::object, 100);
  context.session_uuid.canonical =
      GeneratedUuid(UuidKind::object, 200 + session_ordinal);
  context.current_schema_uuid.canonical = schema_uuid;
  context.default_root_uuid.canonical = GeneratedUuid(UuidKind::object, 300);
  context.security_context_present = true;
  context.identifier_profile_uuid = "sbsql_v3";
  context.language_context.language_tag = "en";
  context.language_context.default_language_tag = "en";
  context.catalog_generation_id = 1;
  context.datatype_catalog_snapshot_uuid.canonical =
      "019d0000-0000-7000-8000-00000000d701";
  context.datatype_catalog_generation = 1;
  context.datatype_registry_generation = 1;
  context.security_epoch = 1;
  context.resource_epoch = 1;
  context.name_resolution_epoch = 1;
  context.trace_tags.push_back("SBLR_COMPOSITE_KEY_AUTHORITY");
  return context;
}

api::EngineRequestContext Begin(const DatabaseFixture& fixture,
                                const std::string& schema_uuid,
                                std::uint64_t session_ordinal) {
  api::EngineBeginTransactionRequest request;
  request.context = BaseContext(fixture, schema_uuid, session_ordinal);
  request.isolation_level = "read_committed";
  const auto begun = api::EngineBeginTransaction(request);
  RequireOk(begun, "composite-key transaction begin failed");
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
            "composite-key transaction commit failed");
}

void Rollback(const api::EngineRequestContext& context) {
  api::EngineRollbackTransactionRequest request;
  request.context = context;
  RequireOk(api::EngineRollbackTransaction(request),
            "composite-key transaction rollback failed");
}

api::EngineLocalizedName Name(std::string value) {
  return {"en", "primary", "", std::move(value), true};
}

api::EngineColumnDefinition Column(std::uint32_t ordinal,
                                   std::string name,
                                   bool nullable) {
  api::EngineColumnDefinition column;
  column.ordinal = ordinal;
  column.names.push_back(Name(std::move(name)));
  column.descriptor.descriptor_kind = "scalar";
  column.descriptor.canonical_type_name = "text";
  column.descriptor.encoded_descriptor =
      std::string("type=text;nullable=") + (nullable ? "true" : "false");
  column.nullable = nullable;
  return column;
}

api::EngineColumnDefinition PrimaryKeyColumn(std::uint32_t ordinal,
                                             std::string name,
                                             bool nullable) {
  auto column = Column(ordinal, std::move(name), nullable);
  column.descriptor.encoded_descriptor += ";primary_key=true";
  return column;
}

api::EngineCreateTableResult CreateTableComponent(
    const api::EngineRequestContext& context,
    api::EngineCreateTableRequest request) {
  request.operation_id = "engine.op.ddl_create_table";
  request.context = context;
  return api::EngineCreateTable(request);
}

api::EngineCreateTableRequest TableRequest(
    const std::string& schema_uuid,
    std::string table_name,
    std::vector<api::EngineColumnDefinition> columns,
    std::vector<api::EngineIndexDefinition> indexes) {
  api::EngineCreateTableRequest request;
  request.target_schema.uuid.canonical = schema_uuid;
  request.target_schema.object_kind = "schema";
  request.table_names.push_back(Name(std::move(table_name)));
  request.table_columns = std::move(columns);
  request.table_indexes = std::move(indexes);
  return request;
}

api::EngineIndexDefinition IndexDefinition(
    std::string constraint_kind,
    std::vector<std::string> keys,
    std::string index_name = {},
    std::string constraint_name = {}) {
  api::EngineIndexDefinition index;
  index.index_kind = "btree";
  index.key_envelopes = std::move(keys);
  index.key_envelopes.push_back(std::move(constraint_kind));
  if (!index_name.empty()) {
    index.names.push_back(Name(index_name));
  }
  if (!constraint_name.empty() && constraint_name != index_name) {
    auto name = Name(std::move(constraint_name));
    if (!index.names.empty()) {
      name.name_class = "constraint";
      name.default_name = false;
    }
    index.names.push_back(std::move(name));
  }
  return index;
}

std::vector<api::EngineIndexDefinition> CompositeIndexes() {
  std::vector<api::EngineIndexDefinition> indexes;
  indexes.push_back(IndexDefinition(
      "primary_key", {"tenant_id", "item_id"},
      "idx_orders_tenant_item", "pk_orders"));
  indexes.push_back(IndexDefinition(
      "unique", {"external_id", "region_id"}, {},
      "uq_orders_external_region"));
  return indexes;
}

std::vector<api::EngineIndexDefinition> OneIndex(
    std::string kind,
    std::string first_key,
    std::string second_key,
    std::string name) {
  std::vector<std::string> keys;
  keys.push_back(std::move(first_key));
  if (!second_key.empty()) keys.push_back(std::move(second_key));
  std::vector<api::EngineIndexDefinition> indexes;
  indexes.push_back(IndexDefinition(std::move(kind), std::move(keys), {},
                                    std::move(name)));
  return indexes;
}

api::EngineTypedValue TextValue(std::string value) {
  api::EngineTypedValue typed;
  typed.descriptor.descriptor_kind = "scalar";
  typed.descriptor.canonical_type_name = "text";
  typed.encoded_value = std::move(value);
  return typed;
}

api::EngineTypedValue NullTextValue() {
  auto typed = TextValue({});
  typed.setState(api::EngineValueState::sql_null);
  return typed;
}

api::EngineRowValue OrderRow(api::EngineTypedValue tenant,
                             std::string item,
                             std::string external,
                             std::string region) {
  api::EngineRowValue row;
  row.fields.push_back({"tenant_id", std::move(tenant)});
  row.fields.push_back({"item_id", TextValue(std::move(item))});
  row.fields.push_back({"external_id", TextValue(std::move(external))});
  row.fields.push_back({"region_id", TextValue(std::move(region))});
  return row;
}

api::EngineInsertRowsResult Insert(const api::EngineRequestContext& context,
                                   const std::string& table_uuid,
                                   api::EngineRowValue row) {
  api::EngineInsertRowsRequest request;
  request.context = context;
  request.target_table.uuid.canonical = table_uuid;
  request.target_table.object_kind = "table";
  request.input_rows.push_back(std::move(row));
  request.option_envelopes.push_back("direct_physical_insert=disabled");
  return api::EngineInsertRows(request);
}

api::EngineSelectRowsResult SelectAll(
    const api::EngineRequestContext& context,
    const std::string& table_uuid) {
  api::EngineSelectRowsRequest request;
  request.context = context;
  request.source_object.uuid.canonical = table_uuid;
  request.source_object.object_kind = "table";
  return api::EngineSelectRows(request);
}

bool HasRegistryName(const api::NameRegistryState& state,
                     std::string_view object_uuid,
                     std::string_view name) {
  return std::any_of(state.entries.begin(), state.entries.end(),
                     [&](const auto& entry) {
                       return entry.object_uuid == object_uuid &&
                              (entry.raw_name_text == name ||
                               entry.display_name == name);
                     });
}

std::vector<std::string> MetadataKeyColumns(const api::CrudIndexRecord& index) {
  std::vector<std::string> columns;
  for (const auto& envelope : index.key_envelopes) {
    if (envelope.empty() || envelope == "unique" ||
        envelope == "primary_key" || envelope == "where_true" ||
        envelope.rfind("include:", 0) == 0 ||
        envelope.rfind("where_eq:", 0) == 0 ||
        envelope.rfind("where_mod_eq:", 0) == 0) {
      continue;
    }
    columns.push_back(envelope);
  }
  if (columns.empty() && !index.column_name.empty()) {
    columns.push_back(index.column_name);
  }
  return columns;
}

}  // namespace

int main() {
  auto fixture = CreateDatabase();
  const std::string schema_uuid = GeneratedUuid(UuidKind::schema, 10);

  auto setup = Begin(fixture, schema_uuid, 1);
  api::EngineCreateSchemaRequest schema;
  schema.context = setup;
  schema.target_object.uuid.canonical = schema_uuid;
  schema.target_object.object_kind = "schema";
  schema.localized_names.push_back(Name("composite_key_schema"));
  RequireOk(api::EngineCreateSchema(schema),
            "composite-key schema create failed");

  const auto nullable_pk = CreateTableComponent(
      setup,
      TableRequest(schema_uuid,
                   "nullable_composite_pk",
                   {Column(0, "left_key", true), Column(1, "right_key", false)},
                   OneIndex("primary_key", "left_key", "right_key",
                            "pk_nullable_rejected")));
  if (nullable_pk.ok ||
      !HasDiagnostic(nullable_pk,
                     "SB_ENGINE_API_INVALID_REQUEST",
                     "ddl.create_table:primary_key_column_nullable:left_key")) {
    for (const auto& diagnostic : nullable_pk.diagnostics) {
      std::cerr << diagnostic.code << ':' << diagnostic.detail << '\n';
    }
  }
  Require(!nullable_pk.ok &&
              HasDiagnostic(nullable_pk,
                            "SB_ENGINE_API_INVALID_REQUEST",
                            "ddl.create_table:primary_key_column_nullable:left_key"),
          "nullable composite primary key did not fail closed");

  const auto nullable_unique = CreateTableComponent(
      setup,
      TableRequest(schema_uuid,
                   "nullable_composite_unique",
                   {Column(0, "left_key", true), Column(1, "right_key", false)},
                   OneIndex("unique", "left_key", "right_key",
                            "uq_nullable_rejected")));
  Require(!nullable_unique.ok &&
              HasDiagnostic(nullable_unique,
                            "SB_ENGINE_API_INVALID_REQUEST",
                            "ddl.create_table:composite_unique_nullable_policy_unsupported:left_key"),
          "nullable composite UNIQUE policy did not fail closed explicitly");

  api::EngineIndexDefinition unsupported_index;
  unsupported_index.index_kind = "invalid_inline_table_index_descriptor";
  unsupported_index.key_envelopes = {"key_value"};
  const auto unsupported_profile = CreateTableComponent(
      setup,
      TableRequest(schema_uuid,
                   "unsupported_index_profile",
                   {Column(0, "key_value", false)},
                   {std::move(unsupported_index)}));
  Require(!unsupported_profile.ok &&
              HasDiagnosticDetail(
                  unsupported_profile,
                  "ddl.create_table:unsupported_inline_index_profile"),
          "unsupported inline-index profile did not fail closed");

  const auto duplicate_key_descriptor = CreateTableComponent(
      setup,
      TableRequest(schema_uuid,
                   "duplicate_key_descriptor",
                   {Column(0, "duplicate_key", false)},
                   OneIndex("unique", "duplicate_key", "duplicate_key",
                            "uq_duplicate_descriptor")));
  Require(!duplicate_key_descriptor.ok &&
              HasDiagnostic(duplicate_key_descriptor,
                            "SB_ENGINE_API_INVALID_REQUEST",
                            "ddl.create_table:inline_index_duplicate_key_column:duplicate_key"),
          "duplicate inline key-column descriptor was not rejected");

  std::vector<api::EngineIndexDefinition> duplicate_primary_indexes;
  duplicate_primary_indexes.push_back(
      IndexDefinition("primary_key", {"first_key"}, {}, "pk_first"));
  duplicate_primary_indexes.push_back(
      IndexDefinition("primary_key", {"second_key"}, {}, "pk_second"));
  const auto multiple_primary_keys = CreateTableComponent(
      setup,
      TableRequest(schema_uuid,
                   "multiple_primary_keys",
                   {Column(0, "first_key", false),
                    Column(1, "second_key", false)},
                   std::move(duplicate_primary_indexes)));
  Require(!multiple_primary_keys.ok &&
              HasDiagnostic(multiple_primary_keys,
                            "SB_ENGINE_API_INVALID_REQUEST",
                            "ddl.create_table:multiple_primary_key_indexes_for_table"),
          "multiple inline primary-key indexes were not rejected");

  const auto multiple_column_primary_keys = CreateTableComponent(
      setup,
      TableRequest(schema_uuid,
                   "multiple_column_primary_keys",
                   {PrimaryKeyColumn(0, "first_key", false),
                    PrimaryKeyColumn(1, "second_key", false)},
                   {}));
  Require(!multiple_column_primary_keys.ok &&
              HasDiagnostic(multiple_column_primary_keys,
                            "SB_ENGINE_API_INVALID_REQUEST",
                            "ddl.create_table:multiple_primary_key_indexes_for_table"),
          "multiple column-level primary-key descriptors were not rejected");

  const auto column_and_inline_primary_keys = CreateTableComponent(
      setup,
      TableRequest(schema_uuid,
                   "column_and_inline_primary_keys",
                   {PrimaryKeyColumn(0, "first_key", false),
                    Column(1, "second_key", false)},
                   OneIndex("primary_key", "first_key", "second_key",
                            "pk_inline_conflict")));
  Require(!column_and_inline_primary_keys.ok &&
              HasDiagnostic(column_and_inline_primary_keys,
                            "SB_ENGINE_API_INVALID_REQUEST",
                            "ddl.create_table:multiple_primary_key_indexes_for_table"),
          "column-level and inline primary keys were not mutually exclusive");

  const auto nullable_column_primary_key = CreateTableComponent(
      setup,
      TableRequest(schema_uuid,
                   "nullable_column_primary_key",
                   {PrimaryKeyColumn(0, "key_value", true)},
                   {}));
  Require(!nullable_column_primary_key.ok &&
              HasDiagnostic(nullable_column_primary_key,
                            "SB_ENGINE_API_INVALID_REQUEST",
                            "ddl.create_table:primary_key_column_nullable:key_value"),
          "nullable column-level primary key did not fail closed");

  const auto expression_unique_constraint = CreateTableComponent(
      setup,
      TableRequest(schema_uuid,
                   "expression_unique_constraint",
                   {Column(0, "left_key", false), Column(1, "right_key", false)},
                   OneIndex("unique", "lower:left_key", "right_key",
                            "uq_expression_rejected")));
  Require(!expression_unique_constraint.ok &&
              HasDiagnosticDetail(expression_unique_constraint,
                                  "ddl.create_table:unique_constraint_requires_direct_columns"),
          "expression envelope in UNIQUE constraint was not rejected");

  auto expression_index_request = TableRequest(
      schema_uuid,
      "general_expression_index",
      {Column(0, "expression_source", false)},
      {});
  api::EngineIndexDefinition expression_index;
  expression_index.names.push_back(Name("idx_general_expression"));
  expression_index.index_kind = "expression";
  expression_index.key_envelopes = {"lower:expression_source",
                                    "upper:expression_source"};
  expression_index_request.table_indexes.push_back(std::move(expression_index));
  const auto expression_index_created =
      CreateTableComponent(setup, std::move(expression_index_request));
  Require(expression_index_created.ok,
          "general expression-index behavior was misclassified as duplicate constraint keys");

  const auto single_nullable_unique = CreateTableComponent(
      setup,
      TableRequest(schema_uuid,
                   "single_nullable_unique",
                   {Column(0, "single_key", true)},
                   OneIndex("unique", "single_key", {},
                            "uq_single_nullable")));
  Require(single_nullable_unique.ok,
          "existing single-column nullable UNIQUE create behavior regressed");

  const auto created = CreateTableComponent(
      setup,
      TableRequest(schema_uuid,
                   "orders",
                   {Column(0, "tenant_id", false),
                    Column(1, "item_id", false),
                    Column(2, "external_id", false),
                    Column(3, "region_id", false)},
                   CompositeIndexes()));
  Require(created.ok, "composite-key table create failed");
  const std::string table_uuid = created.table_object.uuid.canonical;
  Require(uuid::ParseDurableEngineIdentityUuid(UuidKind::object, table_uuid).ok(),
          "table UUID was not engine-generated durable object identity");
  Commit(setup);

  auto metadata_reader = Begin(fixture, schema_uuid, 2);
  const auto loaded_mga = api::LoadMgaRelationStoreState(metadata_reader);
  Require(loaded_mga.ok, "composite-key MGA metadata load failed");
  const auto loaded = api::BuildMgaRelationReadView(loaded_mga.state);
  const auto indexes = api::VisibleCrudIndexesForTable(
      loaded, table_uuid, metadata_reader.local_transaction_id);
  Require(indexes.size() == 2,
          "multiple inline table indexes were not persisted exactly once");
  const auto primary = std::find_if(indexes.begin(), indexes.end(),
                                    [](const auto& index) {
                                      return index.default_name ==
                                             "idx_orders_tenant_item";
                                    });
  const auto unique = std::find_if(indexes.begin(), indexes.end(),
                                   [](const auto& index) {
                                     return index.default_name ==
                                            "uq_orders_external_region";
                                   });
  Require(primary != indexes.end() && unique != indexes.end(),
          "presented inline index/constraint names were not retained");
  Require(primary->unique &&
              primary->key_envelopes ==
                  std::vector<std::string>({"tenant_id", "item_id",
                                            "primary_key"}),
          "primary-key marker or ordered metadata drifted");
  Require(MetadataKeyColumns(*primary) ==
              std::vector<std::string>({"tenant_id", "item_id"}),
          "primary-key directive leaked into the composite physical key");
  Require(unique->unique &&
              MetadataKeyColumns(*unique) ==
                  std::vector<std::string>({"external_id", "region_id"}),
          "ordered composite UNIQUE metadata drifted");
  Require(uuid::ParseDurableEngineIdentityUuid(UuidKind::object,
                                                primary->index_uuid)
              .ok() &&
              uuid::ParseDurableEngineIdentityUuid(UuidKind::object,
                                                    unique->index_uuid)
                  .ok() &&
              primary->index_uuid != unique->index_uuid &&
              primary->index_uuid != primary->default_name &&
              unique->index_uuid != unique->default_name,
          "inline index UUID authority escaped the engine");
  const auto names = api::LoadNameRegistryState(
      metadata_reader, metadata_reader.local_transaction_id);
  Require(names.ok &&
              HasRegistryName(names.state, primary->index_uuid,
                              "idx_orders_tenant_item") &&
              HasRegistryName(names.state, primary->index_uuid, "pk_orders") &&
              HasRegistryName(names.state, unique->index_uuid,
                              "uq_orders_external_region"),
          "presented index/constraint names were not name-registry inputs");
  Rollback(metadata_reader);

  auto writer = Begin(fixture, schema_uuid, 3);
  RequireOk(Insert(writer, table_uuid,
                   OrderRow(TextValue("tenant-a"), "item-1", "ext-1", "r1")),
            "first composite-key row insert failed");
  RequireOk(Insert(writer, table_uuid,
                   OrderRow(TextValue("tenant-a"), "item-2", "ext-2", "r2")),
            "same first/different second composite key was rejected");
  const auto null_pk = Insert(
      writer, table_uuid,
      OrderRow(NullTextValue(), "item-null", "ext-null", "rn"));
  Require(!null_pk.ok &&
              HasDiagnostic(null_pk, "CLI.CONSTRAINT_NOT_NULL_VIOLATION"),
          "SQL NULL primary-key component did not reject before mutation");
  const auto duplicate_pk = Insert(
      writer, table_uuid,
      OrderRow(TextValue("tenant-a"), "item-1", "ext-3", "r3"));
  Require(!duplicate_pk.ok &&
              HasDiagnostic(duplicate_pk,
                            "CLI.CONSTRAINT_PRIMARY_KEY_VIOLATION"),
          "duplicate composite primary-key pair was not rejected");
  const auto duplicate_unique = Insert(
      writer, table_uuid,
      OrderRow(TextValue("tenant-b"), "item-3", "ext-1", "r1"));
  Require(!duplicate_unique.ok &&
              HasDiagnostic(duplicate_unique,
                            "CLI.CONSTRAINT_UNIQUE_VIOLATION"),
          "duplicate composite UNIQUE pair was not rejected");
  Rollback(writer);

  auto after_rollback = Begin(fixture, schema_uuid, 4);
  const auto invisible = SelectAll(after_rollback, table_uuid);
  RequireOk(invisible, "post-rollback composite-key select failed");
  Require(invisible.result_shape.rows.empty(),
          "rolled-back composite-key rows became visible");
  Rollback(after_rollback);

  auto committed_writer = Begin(fixture, schema_uuid, 5);
  RequireOk(Insert(committed_writer, table_uuid,
                   OrderRow(TextValue("tenant-c"), "item-4", "ext-4", "r4")),
            "committed composite-key row insert failed");
  Commit(committed_writer);

  auto committed_reader = Begin(fixture, schema_uuid, 6);
  const auto visible = SelectAll(committed_reader, table_uuid);
  RequireOk(visible, "committed composite-key select failed");
  Require(visible.result_shape.rows.size() == 1,
          "committed composite-key row visibility drifted");
  Rollback(committed_reader);

  std::cout << "sblr composite key authority gate passed\n";
  return EXIT_SUCCESS;
}

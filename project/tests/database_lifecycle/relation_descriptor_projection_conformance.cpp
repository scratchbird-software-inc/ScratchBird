// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "catalog/relation_descriptor_projection.hpp"
#include "database_lifecycle.hpp"
#include "ddl/create_api.hpp"
#include "dml/select_api.hpp"
#include "memory.hpp"
#include "mga_relation_store/mga_relation_store.hpp"
#include "resource_seed_pack.hpp"
#include "transaction/transaction_api.hpp"
#include "uuid.hpp"

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace api = scratchbird::engine::internal_api;
namespace db = scratchbird::storage::database;
namespace memory = scratchbird::core::memory;
namespace resources = scratchbird::core::resources;
namespace uuid = scratchbird::core::uuid;
using scratchbird::core::platform::UuidKind;

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
    for (const auto& diagnostic : result.diagnostics) {
      std::cerr << diagnostic.code << ':' << diagnostic.detail << '\n';
    }
    Fail(message);
  }
}

std::uint64_t UniqueMillis() {
  static std::uint64_t counter = 0;
  return static_cast<std::uint64_t>(
             std::chrono::duration_cast<std::chrono::milliseconds>(
                 std::chrono::system_clock::now().time_since_epoch())
                 .count()) +
         (++counter * 1000);
}

std::string NewUuid(UuidKind kind, std::uint64_t salt) {
  const auto generated = uuid::GenerateEngineIdentityV7(kind, salt);
  Require(generated.ok(), "catalog projection UUID generation failed");
  return uuid::UuidToString(generated.value.value);
}

scratchbird::core::platform::TypedUuid NewTypedUuid(UuidKind kind,
                                                    std::uint64_t salt) {
  const auto generated = uuid::GenerateEngineIdentityV7(kind, salt);
  Require(generated.ok(), "catalog projection typed UUID generation failed");
  return generated.value;
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

struct Fixture {
  std::filesystem::path directory;
  std::filesystem::path database_path;
  db::DatabaseLifecycleResult created;
  std::string database_uuid;
  std::string principal_uuid;
  std::string session_uuid;
  std::string schema_uuid;
  std::string function_uuid;
  std::string view_uuid;
  std::string relation_uuid;
  std::uint64_t function_creator_tx = 0;
  std::uint64_t view_creator_tx = 0;
  std::uint64_t salt = 0;

  ~Fixture() {
    std::error_code ignored;
    std::filesystem::remove_all(directory, ignored);
  }
};

Fixture CreateFixture() {
  Fixture fixture;
  fixture.salt = UniqueMillis();
  fixture.directory = std::filesystem::temp_directory_path() /
                      ("scratchbird_relation_descriptor_projection_" +
                       std::to_string(fixture.salt));
  std::filesystem::create_directories(fixture.directory);
  fixture.database_path = fixture.directory / "projection.sbdb";

  db::DatabaseCreateConfig create;
  create.path = fixture.database_path.string();
  create.database_uuid =
      NewTypedUuid(UuidKind::database, fixture.salt + 1);
  create.filespace_uuid =
      NewTypedUuid(UuidKind::filespace, fixture.salt + 2);
  create.creation_unix_epoch_millis = fixture.salt + 3;
  create.page_size = 8192;
  create.resource_seed_pack_root = SB_BOOTSTRAP_SEED_PACK_ROOT;
  create.require_resource_seed_pack = true;
  create.allow_minimal_resource_bootstrap = false;
  create.allow_overwrite = true;
  fixture.created = db::CreateDatabaseFile(create);
  Require(fixture.created.ok(),
          "catalog projection database creation failed");

  fixture.database_uuid = uuid::UuidToString(create.database_uuid.value);
  fixture.principal_uuid =
      NewUuid(UuidKind::principal, fixture.salt + 4);
  fixture.session_uuid = NewUuid(UuidKind::object, fixture.salt + 5);
  fixture.schema_uuid = NewUuid(UuidKind::schema, fixture.salt + 6);
  fixture.function_uuid = NewUuid(UuidKind::object, fixture.salt + 7);
  fixture.view_uuid = NewUuid(UuidKind::object, fixture.salt + 8);
  fixture.relation_uuid = NewUuid(UuidKind::object, fixture.salt + 9);
  return fixture;
}

api::EngineRequestContext Begin(Fixture& fixture,
                                std::uint64_t ordinal,
                                bool read_only = false) {
  api::EngineBeginTransactionRequest begin;
  begin.context.trust_mode = api::EngineTrustMode::server_isolated;
  begin.context.request_id =
      "catalog-projection-begin-" + std::to_string(ordinal);
  begin.context.database_path = fixture.database_path.string();
  begin.context.database_uuid.canonical = fixture.database_uuid;
  begin.context.principal_uuid.canonical = fixture.principal_uuid;
  begin.context.session_uuid.canonical = fixture.session_uuid;
  begin.context.security_context_present = true;
  begin.context.catalog_generation_id = 1;
  begin.context.security_epoch = 1;
  begin.context.resource_epoch =
      fixture.created.state.resource_seed_catalog.resource_epoch;
  begin.context.name_resolution_epoch = 1;
  begin.isolation_level = "read_committed";
  if (read_only) {
    begin.transaction_policy_profile.encoded_profiles.push_back(
        "transaction_read_mode:read_only");
  }
  const auto begun = api::EngineBeginTransaction(begin);
  RequireOk(begun, "catalog projection transaction begin failed");
  auto context = begin.context;
  context.local_transaction_id = begun.local_transaction_id;
  context.transaction_uuid = begun.transaction_uuid;
  context.snapshot_visible_through_local_transaction_id =
      begun.snapshot_visible_through_local_transaction_id;
  context.transaction_isolation_level = begun.isolation_level;
  context.read_only_mode = begun.read_only;
  context.current_schema_uuid.canonical = fixture.schema_uuid;
  return context;
}

void Commit(const api::EngineRequestContext& context) {
  api::EngineCommitTransactionRequest commit;
  commit.context = context;
  RequireOk(api::EngineCommitTransaction(commit),
            "catalog projection commit failed");
}

void Rollback(const api::EngineRequestContext& context) {
  api::EngineRollbackTransactionRequest rollback;
  rollback.context = context;
  RequireOk(api::EngineRollbackTransaction(rollback),
            "catalog projection rollback failed");
}

void CreateSchemaAndFunction(Fixture& fixture,
                             const api::EngineRequestContext& context) {
  api::EngineCreateSchemaRequest schema;
  schema.context = context;
  schema.target_object.uuid.canonical = fixture.schema_uuid;
  schema.target_object.object_kind = "schema";
  schema.localized_names.push_back(Name("catalog_projection_schema"));
  RequireOk(api::EngineCreateSchema(schema),
            "catalog projection schema create failed");

  api::EngineCreateFunctionRequest function;
  function.context = context;
  function.target_schema.uuid.canonical = fixture.schema_uuid;
  function.target_schema.object_kind = "schema";
  function.target_object.uuid.canonical = fixture.function_uuid;
  function.target_object.object_kind = "function";
  function.localized_names.push_back(Name("generic_type_name_resolver"));
  function.option_envelopes = {
      "executor:metadata_only",
      "side_effect_class:none",
      std::string("compiled_body_descriptor:") +
          api::kRelationTypeNameFunctionDescriptorV1,
      "compiled_body_provenance:engine.catalog.neutral_projection.v1",
      "routine_parameter_count:2",
      "routine_parameter_0_mode:in",
      "routine_parameter_0_type:int16",
      "routine_parameter_1_mode:in",
      "routine_parameter_1_type:int16",
      "routine_return_count:1",
      "routine_return_0_type:text",
      "permission:manage_executable"};
  RequireOk(api::EngineCreateFunction(function),
            "catalog projection function create failed");
  fixture.function_creator_tx = context.local_transaction_id;
}

api::EngineCreateViewRequest ViewRequest(
    const Fixture& fixture,
    const api::EngineRequestContext& context,
    std::string view_uuid) {
  api::EngineCreateViewRequest view;
  view.context = context;
  view.target_schema.uuid.canonical = fixture.schema_uuid;
  view.target_schema.object_kind = "schema";
  view.target_object.uuid.canonical = std::move(view_uuid);
  view.target_object.object_kind = "view";
  view.localized_names.push_back(Name("generic_relation_inventory"));
  view.option_envelopes = {
      std::string("view_query_shape:") +
          api::kRelationDescriptorProjectionMarkerV1,
      "view_source_name:generic_catalog_source",
      "view_projection_count:2",
      std::string("view_projection_0:variant:") +
          api::kRelationDescriptorProjectionTypeInventoryVariantV1,
      "view_projection_1:function_uuid:" + fixture.function_uuid};
  return view;
}

void CreateViewAndRefusalProbes(
    Fixture& fixture,
    const api::EngineRequestContext& context) {
  auto raw_sql = ViewRequest(
      fixture,
      context,
      NewUuid(UuidKind::object, fixture.salt + 30));
  raw_sql.option_envelopes.push_back("raw_sql:select * from parser_state");
  const auto refused_raw = api::EngineCreateView(raw_sql);
  Require(!refused_raw.ok,
          "catalog projection accepted raw SQL view storage");

  auto unsupported = ViewRequest(
      fixture,
      context,
      NewUuid(UuidKind::object, fixture.salt + 31));
  unsupported.option_envelopes[3] =
      "view_projection_0:variant:dialect.private_inventory.v1";
  const auto refused_variant = api::EngineCreateView(unsupported);
  Require(!refused_variant.ok,
          "catalog projection accepted a dialect-private variant");

  auto view = ViewRequest(fixture, context, fixture.view_uuid);
  const auto created = api::EngineCreateView(view);
  RequireOk(created, "catalog projection view create failed");
  fixture.view_creator_tx = context.local_transaction_id;

  const auto described = api::DescribeEngineCatalogRelationProjectionView(
      context, fixture.view_uuid);
  Require(!described.diagnostic.error && described.present &&
              described.semantic_variant ==
                  api::kRelationDescriptorProjectionTypeInventoryVariantV1 &&
              described.source_relation_name == "generic_catalog_source" &&
              described.function_uuid == fixture.function_uuid,
          "catalog projection view descriptor was not engine-visible");
}

api::EngineColumnDefinition Column(std::uint32_t ordinal,
                                   std::string name,
                                   std::string canonical_type,
                                   std::string encoded_descriptor) {
  api::EngineColumnDefinition column;
  column.ordinal = ordinal;
  column.names.push_back(Name(std::move(name)));
  column.descriptor.descriptor_kind = "scalar";
  column.descriptor.canonical_type_name = std::move(canonical_type);
  column.descriptor.encoded_descriptor = std::move(encoded_descriptor);
  column.nullable = true;
  return column;
}

api::MgaRelationStorageDescriptor CreateSourceRelation(
    Fixture& fixture,
    const api::EngineRequestContext& context) {
  const auto* utf8 = resources::FindResourceSeedCharset(
      fixture.created.state.resource_seed_catalog, "UTF8");
  Require(utf8 != nullptr && !utf8->resource_uuid.empty() &&
              !utf8->default_collation_uuid.empty(),
          "catalog projection UTF8 resources are unavailable");

  api::EngineCreateTableRequest table;
  table.context = context;
  // The explicit target schema is authoritative even when the caller has no
  // session-default schema. This is the parser-server shape and guards the
  // persisted descriptor from silently losing its schema identity.
  table.context.current_schema_uuid.canonical.clear();
  table.target_schema.uuid.canonical = fixture.schema_uuid;
  table.target_schema.object_kind = "schema";
  table.requested_table_uuid.canonical = fixture.relation_uuid;
  table.table_names.push_back(Name("generic_catalog_source"));
  table.table_columns.push_back(
      Column(0, "integer_value", "integer", "type=integer"));
  table.table_columns.push_back(Column(
      1,
      "text_value",
      "varchar(20)",
      "type=varchar(20);character_length=20;charset_uuid=" +
          utf8->resource_uuid +
          ";collation_uuid=" + utf8->default_collation_uuid));
  table.table_columns.push_back(Column(
      2,
      "text_payload",
      "blob",
      "type=blob;text_resource_storage=large_object;charset_uuid=" +
          utf8->resource_uuid +
          ";collation_uuid=" + utf8->default_collation_uuid));
  RequireOk(api::EngineCreateTable(table),
            "catalog projection source table create failed");

  const auto descriptor =
      api::LoadMgaRelationStorageDescriptor(context, fixture.relation_uuid);
  Require(descriptor.ok,
          "catalog projection source descriptor load failed");
  Require(descriptor.descriptor.columns.size() == 3,
          "catalog projection source descriptor column count is invalid");
  Require(descriptor.descriptor.schema_uuid.canonical == fixture.schema_uuid,
          "explicit target schema was not persisted in the relation descriptor");
  return descriptor.descriptor;
}

const api::EngineTypedValue& Field(const api::EngineRowValue& row,
                                   std::string_view name) {
  for (const auto& field : row.fields) {
    if (field.first == name) return field.second;
  }
  Fail("catalog projection result field is missing");
}

bool HasEvidence(const api::EngineApiResult& result,
                 std::string_view kind,
                 std::string_view value) {
  for (const auto& evidence : result.evidence) {
    if (evidence.evidence_kind == kind && evidence.evidence_id == value) {
      return true;
    }
  }
  return false;
}

bool HasDiagnosticDetail(const api::EngineApiResult& result,
                         std::string_view detail) {
  for (const auto& diagnostic : result.diagnostics) {
    if (diagnostic.detail == detail) return true;
  }
  return false;
}

api::EngineSelectRowsRequest SelectRequest(
    const Fixture& fixture,
    const api::EngineRequestContext& context,
    const api::MgaRelationStorageDescriptor& descriptor) {
  api::EngineSelectRowsRequest select;
  select.context = context;
  select.target_object.uuid.canonical = fixture.view_uuid;
  select.target_object.object_kind = "view";
  select.option_envelopes = {
      std::string("source_kind:") +
          api::kRelationDescriptorProjectionSourceKind,
      std::string("result_projection:") +
          api::kRelationDescriptorProjectionMarkerV1,
      std::string("dml_surface_variant:") +
          api::kRelationDescriptorProjectionTypeInventoryVariantV1,
      "source_uuid:" + fixture.relation_uuid,
      "source_fingerprint:" + descriptor.descriptor_uuid.canonical,
      "source_position:" +
          std::to_string(descriptor.descriptor_generation)};
  return select;
}

void RequireProjectionRows(
    Fixture& fixture,
    const api::EngineRequestContext& context,
    const api::MgaRelationStorageDescriptor& descriptor) {
  const auto described = api::DescribeEngineCatalogRelationProjectionView(
      context, fixture.view_uuid);
  if (described.diagnostic.error) {
    std::cerr << described.diagnostic.code << ':'
              << described.diagnostic.detail << '\n';
  }
  Require(!described.diagnostic.error && described.present,
          "committed catalog projection view was not described");

  const auto selected =
      api::EngineSelectRows(SelectRequest(fixture, context, descriptor));
  RequireOk(selected, "catalog projection select failed");
  Require(selected.visible_count == 3 &&
              selected.result_shape.rows.size() == 3 &&
              selected.result_shape.columns.size() == 10,
          "catalog projection neutral result shape is invalid");
  Require(HasEvidence(selected,
                      "catalog_projection_marker",
                      api::kRelationDescriptorProjectionMarkerV1) &&
              HasEvidence(selected,
                          "catalog_projection_view_uuid",
                          fixture.view_uuid) &&
              HasEvidence(selected,
                          "catalog_projection_relation_uuid",
                          fixture.relation_uuid) &&
              HasEvidence(selected,
                          "catalog_projection_descriptor_uuid",
                          descriptor.descriptor_uuid.canonical) &&
              HasEvidence(selected,
                          "catalog_projection_mga_authority",
                          "durable_transaction_inventory") &&
              HasEvidence(selected,
                          "catalog_projection_parser_sql",
                          "false"),
          "catalog projection authority evidence is incomplete");

  const auto& integer = selected.result_shape.rows[0];
  const auto& character = selected.result_shape.rows[1];
  const auto& text_blob = selected.result_shape.rows[2];
  Require(Field(integer, "ordinal").encoded_value == "0" &&
              Field(character, "ordinal").encoded_value == "1" &&
              Field(text_blob, "ordinal").encoded_value == "2",
          "catalog projection rows are not in descriptor ordinal order");
  Require(Field(integer, "character_length").isSqlNull() &&
              Field(integer, "charset_uuid").isSqlNull() &&
              Field(integer, "collation_uuid").isSqlNull(),
          "non-character projection resources are not SQL NULL");
  Require(!Field(character, "character_length").isSqlNull() &&
              Field(character, "character_length").encoded_value == "20" &&
              !Field(character, "charset_canonical_name").isSqlNull() &&
              !Field(character, "collation_canonical_name").isSqlNull(),
          "character projection resources are incomplete");
  Require(Field(text_blob, "character_length").isSqlNull() &&
              Field(text_blob, "text_large_object").encoded_value == "true",
          "text large-object length/state is invalid");
  for (const auto& row : selected.result_shape.rows) {
    Require(row.fields.size() == 10,
            "catalog projection row field count is invalid");
    for (const auto& field : row.fields) {
      Require(field.first != "encoded_descriptor" &&
                  field.first != "sql_text" && field.first != "raw_sql",
              "catalog projection leaked encoded descriptor or SQL");
    }
  }

  auto wrong_fingerprint = SelectRequest(fixture, context, descriptor);
  wrong_fingerprint.option_envelopes[4] =
      "source_fingerprint:" +
      NewUuid(UuidKind::object, fixture.salt + 40);
  const auto wrong_fingerprint_result =
      api::EngineSelectRows(wrong_fingerprint);
  Require(!wrong_fingerprint_result.ok &&
              HasDiagnosticDetail(
                  wrong_fingerprint_result,
                  "catalog.relation_descriptor_projection:"
                  "projection_descriptor_uuid_mismatch"),
          "catalog projection did not identify a descriptor UUID mismatch");

  auto wrong_generation = SelectRequest(fixture, context, descriptor);
  wrong_generation.option_envelopes[5] =
      "source_position:" +
      std::to_string(descriptor.descriptor_generation + 1);
  const auto wrong_generation_result =
      api::EngineSelectRows(wrong_generation);
  Require(!wrong_generation_result.ok &&
              HasDiagnosticDetail(
                  wrong_generation_result,
                  "catalog.relation_descriptor_projection:"
                  "projection_descriptor_generation_mismatch"),
          "catalog projection did not identify a descriptor generation mismatch");

  auto malformed_position = SelectRequest(fixture, context, descriptor);
  malformed_position.option_envelopes[5] =
      "source_position:not-a-generation";
  Require(!api::EngineSelectRows(malformed_position).ok,
          "catalog projection accepted malformed descriptor detail");

  auto unknown_variant = SelectRequest(fixture, context, descriptor);
  unknown_variant.option_envelopes[2] =
      "dml_surface_variant:relation.unknown_inventory.v1";
  Require(!api::EngineSelectRows(unknown_variant).ok,
          "catalog projection accepted an unknown select variant");

  auto excluded_dependency = context;
  excluded_dependency.statement_metadata_snapshot_engine_owned = true;
  excluded_dependency.statement_metadata_snapshot_visible_through_local_transaction_id =
      context.local_transaction_id;
  excluded_dependency
      .statement_metadata_snapshot_active_excluded_local_transaction_ids
      .push_back(fixture.function_creator_tx);
  const auto excluded = api::DescribeEngineCatalogRelationProjectionView(
      excluded_dependency, fixture.view_uuid);
  Require(excluded.diagnostic.error && !excluded.present,
          "catalog projection accepted a metadata-excluded function");
}

}  // namespace

int main() {
  auto policy = memory::DefaultLocalEngineMemoryPolicy();
  policy.policy_name = "database_lifecycle_relation_descriptor_projection";
  const auto configured = memory::ConfigureDefaultMemoryManagerForFixture(
      policy, "database_lifecycle_relation_descriptor_projection");
  Require(configured.ok(),
          "catalog projection memory fixture configuration failed");

  Fixture fixture = CreateFixture();

  auto function_tx = Begin(fixture, 1);
  CreateSchemaAndFunction(fixture, function_tx);
  Commit(function_tx);

  auto view_tx = Begin(fixture, 2);
  CreateViewAndRefusalProbes(fixture, view_tx);
  Commit(view_tx);

  auto relation_tx = Begin(fixture, 3);
  const auto descriptor = CreateSourceRelation(fixture, relation_tx);
  Commit(relation_tx);

  auto read_tx = Begin(fixture, 4);
  RequireProjectionRows(fixture, read_tx, descriptor);
  Rollback(read_tx);

  std::cout << "database_lifecycle_relation_descriptor_projection_conformance: PASS\n";
  return EXIT_SUCCESS;
}

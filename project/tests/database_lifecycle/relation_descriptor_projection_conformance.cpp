#include "mga_relation_store/mga_metadata_record_codec.hpp"
#include "../support/engine_evidence_fixture.hpp"
// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "../support/binary_uuid_fixture.hpp"
#include "../support/engine_statement_fixture.hpp"
#include "../support/sb_test_temp_compat.hpp"
#include "catalog/relation_descriptor_projection.hpp"
#include "catalog/catalog_object_lifecycle.hpp"
#include "catalog/binary_view_options.hpp"
#include "database_lifecycle_test_memory.hpp"
#include "database_lifecycle.hpp"
#include "ddl/create_api.hpp"
#include "dml/select_api.hpp"
#include "memory.hpp"
#include "mga_relation_store/mga_relation_store.hpp"
#include "resource_seed_pack.hpp"
#include "transaction/transaction_api.hpp"
#include "uuid.hpp"
#include "catalog/column_metadata_codec.hpp"
#include "datatype_storage_identity.hpp"
#include "../../src/engine/public_abi_int64_payload.hpp"
#include "../../src/engine/public_abi_boolean_payload.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <future>
#include <iostream>
#include <string>
#include <string_view>
#include <stdexcept>
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
  throw std::runtime_error(std::string(message));
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

api::EngineUuid NewUuid(UuidKind kind, std::uint64_t salt) {
  const auto generated = uuid::GenerateEngineIdentityV7(kind, salt);
  Require(generated.ok(), "catalog projection UUID generation failed");
  return generated.value.value;
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

std::string ResourceMetadata(std::string_view attributes,
                             const api::EngineUuid& charset,
                             const api::EngineUuid& collation) {
  api::CatalogColumnMetadata fields;
  Require(api::AdmitCatalogColumnMetadata(attributes, &fields),
          "resource projection attributes invalid");
  fields.identities = {{"charset_uuid", charset}, {"collation_uuid", collation}};
  std::string bytes;
  Require(api::EncodeCatalogColumnMetadata(fields, &bytes),
          "resource projection metadata encoding failed");
  return bytes;
}

struct Fixture {
  std::filesystem::path directory;
  std::filesystem::path database_path;
  db::DatabaseLifecycleResult created;
  api::EngineRequestContext owner_context;
  api::EngineUuid database_uuid;
  api::EngineUuid principal_uuid;
  api::EngineUuid session_uuid;
  api::EngineUuid schema_uuid;
  api::EngineUuid function_uuid;
  api::EngineUuid view_uuid;
  api::EngineUuid relation_uuid;
  std::uint64_t function_creator_tx = 0;
  std::uint64_t function_successor_creator_tx = 0;
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
  auto pattern = (std::filesystem::temp_directory_path() /
                  "scratchbird_relation_descriptor_projection_XXXXXX").string();
  Require(::mkdtemp(pattern.data()) != nullptr,
          "catalog projection owned fixture directory creation failed");
  try {
    fixture.directory = pattern;
  } catch (...) {
    std::error_code ignored;
    std::filesystem::remove(pattern, ignored);
    throw;
  }
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
  scratchbird::tests::ConfigureCredentialedFixtureBootstrap(create);
  fixture.created = db::CreateDatabaseFile(create);
  Require(fixture.created.ok(),
          "catalog projection database creation failed");

  fixture.database_uuid = create.database_uuid.value;
  fixture.owner_context = scratchbird::tests::BootstrapFixtureOwnerContext(create);
  fixture.principal_uuid = fixture.owner_context.principal_uuid;
  fixture.session_uuid = NewUuid(UuidKind::object, fixture.salt + 5);
  fixture.schema_uuid = NewUuid(UuidKind::schema, fixture.salt + 6);
  fixture.function_uuid = NewUuid(UuidKind::object, fixture.salt + 7);
  fixture.view_uuid = NewUuid(UuidKind::object, fixture.salt + 8);
  fixture.relation_uuid = NewUuid(UuidKind::object, fixture.salt + 9);
  return fixture;
}

api::EngineRequestContext Begin(Fixture& fixture,
                                std::uint64_t ordinal,
                                bool read_only = false,
                                std::string isolation = "read_committed") {
  api::EngineBeginTransactionRequest begin;
  begin.context = fixture.owner_context;
  begin.context.trust_mode = api::EngineTrustMode::server_isolated;
  begin.context.request_id =
      "catalog-projection-begin-" + std::to_string(ordinal);
  begin.context.database_path = fixture.database_path.string();
  begin.context.database_uuid = fixture.database_uuid;
  begin.context.principal_uuid = fixture.principal_uuid;
  begin.context.session_uuid = fixture.session_uuid;
  begin.context.security_context_present = true;
  begin.context.catalog_generation_id = 1;
  begin.context.security_epoch = 1;
  begin.context.resource_epoch =
      fixture.created.state.resource_seed_catalog.resource_epoch;
  begin.context.name_resolution_epoch = 1;
  begin.isolation_level = std::move(isolation);
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
  context.current_schema_uuid = fixture.schema_uuid;
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

std::vector<std::string> TypeNameFunctionOptions() {
  return {
      "executor:metadata_only", "side_effect_class:none",
      std::string("compiled_body_descriptor:") + api::kRelationTypeNameFunctionDescriptorV1,
      "compiled_body_provenance:engine.catalog.neutral_projection.v1",
      "routine_parameter_count:2", "routine_parameter_0_mode:in", "routine_parameter_0_type:int16",
      "routine_parameter_1_mode:in", "routine_parameter_1_type:int16", "routine_return_count:1",
      "routine_return_0_type:text", "permission:manage_executable"};
}

void CreateSchemaAndFunction(Fixture& fixture,
                             const api::EngineRequestContext& context) {
  api::EngineCreateSchemaRequest schema;
  schema.context = context;
  schema.target_object.uuid = fixture.schema_uuid;
  schema.target_object.object_kind = "schema";
  schema.localized_names.push_back(Name("catalog_projection_schema"));
  RequireOk(api::EngineCreateSchema(schema),
            "catalog projection schema create failed");

  api::EngineCreateFunctionRequest function;
  function.context = context;
  function.target_schema.uuid = fixture.schema_uuid;
  function.target_schema.object_kind = "schema";
  function.target_object.uuid = fixture.function_uuid;
  function.target_object.object_kind = "function";
  function.localized_names.push_back(Name("generic_type_name_resolver"));
  function.option_envelopes = TypeNameFunctionOptions();
  RequireOk(api::EngineCreateFunction(function),
            "catalog projection function create failed");
  fixture.function_creator_tx = context.local_transaction_id;
}

void RequireFunctionSuccessorAdmission(Fixture& fixture) {
  auto reader = Begin(fixture, 110, true, "repeatable_read");
  const auto loaded = api::LoadExecutableObjectLifecycleState(reader);
  Require(loaded.ok, "function admission predecessor read failed");
  const auto found = std::find_if(loaded.state.objects.begin(), loaded.state.objects.end(),
      [&](const auto& row) { return row.object_uuid == fixture.function_uuid; });
  Require(found != loaded.state.objects.end(), "function admission predecessor missing");
  const auto predecessor = *found;
  auto writer = Begin(fixture, 111);
  api::EngineAlterExecutableObjectRequest alter;
  alter.context = writer;
  alter.target_object = {fixture.function_uuid, "function"};
  alter.option_envelopes = TypeNameFunctionOptions();
  RequireOk(api::EngineAlterExecutableObject(alter), "active sibling function ALTER failed");
  Require(api::RevalidateRelationDescriptorProjectionFunction(reader, predecessor).error,
      "VIEW admission accepted an unresolved sibling function successor");
  Rollback(writer);
  Require(!api::RevalidateRelationDescriptorProjectionFunction(reader, predecessor).error,
      "rolled-back function successor still blocks current admission");

  writer = Begin(fixture, 112);
  alter.context = writer;
  // The real executable writer must share the native VIEW publication lock.
  auto owner = api::AcquireTransactionInventoryGuard(writer.database_path);
  std::promise<void> started;
  auto ready = started.get_future();
  auto pending = std::async(std::launch::async, [&] {
    started.set_value();
    return api::EngineAlterExecutableObject(alter);
  });
  ready.wait();
  const bool blocked = pending.wait_for(std::chrono::milliseconds(50)) == std::future_status::timeout;
  owner.unlock();
  RequireOk(pending.get(), "serialized sibling function ALTER failed");
  Require(blocked, "executable ALTER escaped retained native publication guard");
  Commit(writer);
  fixture.function_successor_creator_tx = writer.local_transaction_id;
  Require(api::RevalidateRelationDescriptorProjectionFunction(reader, predecessor).error,
      "VIEW admission accepted a late-committed function successor hidden by its snapshot");
  Rollback(reader);

  reader = Begin(fixture, 113, true, "repeatable_read");
  const auto current = api::LoadExecutableObjectLifecycleState(reader);
  Require(current.ok, "current function read failed");
  const auto current_row = std::find_if(current.state.objects.begin(), current.state.objects.end(),
      [&](const auto& row) { return row.object_uuid == fixture.function_uuid; });
  Require(current_row != current.state.objects.end(), "current function missing");
  writer = Begin(fixture, 114);
  api::EngineDropExecutableObjectRequest drop;
  drop.context = writer;
  drop.target_object = {fixture.function_uuid, "function"};
  drop.option_envelopes = {"permission:manage_executable"};
  RequireOk(api::EngineDropExecutableObject(drop), "active sibling function DROP failed");
  Require(api::RevalidateRelationDescriptorProjectionFunction(reader, *current_row).error,
      "VIEW admission accepted an unresolved sibling function DROP");
  Rollback(writer);
  Require(!api::RevalidateRelationDescriptorProjectionFunction(reader, *current_row).error,
      "rolled-back DROP blocked valid function admission");
  Rollback(reader);
}

api::EngineCreateViewRequest ViewRequest(
    const Fixture& fixture,
    const api::EngineRequestContext& context,
    api::EngineUuid view_uuid) {
  api::EngineCreateViewRequest view;
  view.context = context;
  view.target_schema.uuid = fixture.schema_uuid;
  view.target_schema.object_kind = "schema";
  view.target_object.uuid = std::move(view_uuid);
  view.target_object.object_kind = "view";
  view.localized_names.push_back(Name("generic_relation_inventory"));
  view.option_envelopes = {
      std::string("view_query_shape:") +
          api::kRelationDescriptorProjectionMarkerV1,
      "view_source_name:generic_catalog_source",
      "view_projection_count:2",
      std::string("view_projection_0:variant:") +
          api::kRelationDescriptorProjectionTypeInventoryVariantV1,
      "view_projection_1:function_uuid:" + api::MetadataUuidBytes(fixture.function_uuid)};
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

  const auto catalog = api::LoadCatalogObjectLifecycleState(context);
  Require(catalog.ok, "inventory VIEW owning catalog read failed");
  const auto object = std::find_if(catalog.state.objects.begin(), catalog.state.objects.end(),
      [&](const auto& row) { return row.object_uuid == fixture.view_uuid; });
  Require(object != catalog.state.objects.end() && object->object_kind == "view" &&
              !object->catalog_row_uuid.is_nil() && object->definition_epoch == 1 &&
              object->creator_tx == context.local_transaction_id &&
              object->payload.starts_with("SBVIEW02"),
          "inventory VIEW lacks a real owning catalog row and binary definition");
  Require(std::any_of(catalog.state.dependencies.begin(), catalog.state.dependencies.end(),
      [&](const auto& edge) { return edge.source_uuid == fixture.view_uuid &&
          edge.dependency_uuid == fixture.function_uuid && edge.dependency_kind == "view_type_name_function"; }),
      "inventory VIEW omitted its actual function dependency");
  Require(std::none_of(catalog.state.dependencies.begin(), catalog.state.dependencies.end(),
      [&](const auto& edge) { return edge.source_uuid == fixture.view_uuid &&
          edge.dependency_kind == "view_source_column"; }),
      "inventory VIEW fabricated a source column before its table exists");

  const auto described = api::DescribeEngineCatalogRelationProjectionView(
      context, fixture.view_uuid);
  Require(!described.diagnostic.error && described.present &&
              described.semantic_variant ==
                  api::kRelationDescriptorProjectionTypeInventoryVariantV1 &&
              described.source_relation_name == "generic_catalog_source" &&
              described.function_uuid == fixture.function_uuid,
          "catalog projection view descriptor was not engine-visible");
}

api::EngineColumnDefinition Column(const api::EngineRequestContext& context,
                                   std::uint32_t ordinal,
                                   std::string name,
                                   std::string canonical_type,
                                   std::string encoded_descriptor) {
  namespace datatypes = scratchbird::core::datatypes;
  const auto manifest = datatypes::LoadCurrentCoreDatatypeCatalogManifest();
  Require(manifest.ok(), "projection fixture datatype catalog unavailable");
  const auto row = datatypes::LookupDatatypeCatalogRow(
      manifest.manifest, datatypes::CanonicalTypeIdFromStableName(
          canonical_type == "integer" ? "int32" : canonical_type));
  Require(row.ok() && row.manifest.descriptor_rows.size() == 1,
          "projection fixture datatype absent from current catalog");
  const auto& datatype = row.manifest.descriptor_rows.front();
  datatypes::DatatypeStorageIdentityV3 binding;
  const bool bound = datatypes::LookupDatatypeStorageIdentityV3(
      context.datatype_catalog_snapshot_uuid, context.datatype_catalog_generation,
      context.datatype_registry_generation, datatype.descriptor_uuid.value,
      datatype.descriptor_epoch, &binding);
  Require(bound && binding.type_id == datatype.type_id && !binding.type_uuid.is_nil(),
          "projection fixture datatype authority does not match its catalog");
  api::EngineColumnDefinition column;
  column.ordinal = ordinal;
  column.requested_column_uuid = api::GenerateCrudEngineUuid("object");
  column.names.push_back(Name(std::move(name)));
  column.descriptor.descriptor_uuid = api::GenerateCrudEngineUuid("object");
  column.descriptor.datatype_descriptor_uuid = datatype.descriptor_uuid.value;
  column.descriptor.datatype_descriptor_generation = datatype.descriptor_epoch;
  column.descriptor.type_uuid = binding.type_uuid;
  column.descriptor.datatype_cohort = {context.datatype_catalog_snapshot_uuid,
      context.datatype_catalog_generation, context.datatype_registry_generation};
  column.descriptor.descriptor_kind = "scalar";
  column.descriptor.canonical_type_name = std::move(canonical_type);
  column.descriptor.encoded_descriptor = std::move(encoded_descriptor);
  column.nullable = true;
  return column;
}

api::MgaRelationStorageDescriptor CreateSourceRelation(
    Fixture& fixture,
    const api::EngineRequestContext& context,
    bool include_text_lob = true) {
  const auto* utf8 = resources::FindResourceSeedCharset(
      fixture.created.state.resource_seed_catalog, "UTF8");
  Require(utf8 != nullptr && !utf8->resource_uuid.is_nil() &&
              !utf8->default_collation_uuid.is_nil(),
          "catalog projection UTF8 resources are unavailable");

  api::EngineCreateTableRequest table;
  table.context = context;
  // The explicit target schema is authoritative even when the caller has no
  // session-default schema. This is the parser-server shape and guards the
  // persisted descriptor from silently losing its schema identity.
  table.context.current_schema_uuid = {};
  table.target_schema.uuid = fixture.schema_uuid;
  table.target_schema.object_kind = "schema";
  table.requested_table_uuid = fixture.relation_uuid;
  table.table_names.push_back(Name("generic_catalog_source"));
  table.table_columns.push_back(
      Column(context, 0, "integer_value", "integer", "type=integer"));
  table.table_columns.push_back(Column(
      context,
      1,
      "text_value",
      "text",
      ResourceMetadata("type=text;character_length=20", utf8->resource_uuid, utf8->default_collation_uuid)));
  if (include_text_lob) {
  table.table_columns.push_back(Column(
      context,
      2,
      "text_payload",
      "blob",
      ResourceMetadata("type=blob;text_resource_storage=large_object", utf8->resource_uuid, utf8->default_collation_uuid)));
  auto wrong_blob_identity = table;
  Require(table.table_columns.back().descriptor.type_uuid != table.table_columns[1].descriptor.type_uuid,
          "projection BLOB fixture did not bind a distinct catalog type identity");
  wrong_blob_identity.table_columns.back().descriptor.type_uuid =
      table.table_columns[1].descriptor.type_uuid;
  const auto rejected_blob_identity = api::EngineCreateTable(wrong_blob_identity);
  Require(!rejected_blob_identity.ok &&
              std::any_of(rejected_blob_identity.diagnostics.begin(),
                          rejected_blob_identity.diagnostics.end(), [](const auto& diagnostic) {
                            return diagnostic.error &&
                                diagnostic.code == "DATATYPE.DESCRIPTOR.INVALID" &&
                                diagnostic.message_key == "datatype.native_blob.descriptor" &&
                                diagnostic.detail == "column_datatype_storage_identity_mismatch";
                          }),
          "catalog projection did not preserve the owning BLOB identity refusal");
  }
  RequireOk(api::EngineCreateTable(table),
            "catalog projection source table create failed");

  const auto descriptor =
      api::LoadMgaRelationStorageDescriptor(context, fixture.relation_uuid);
  Require(descriptor.ok,
          "catalog projection source descriptor load failed");
  Require(descriptor.descriptor.columns.size() == (include_text_lob ? 3 : 2),
          "catalog projection source descriptor column count is invalid");
  Require(descriptor.descriptor.schema_uuid == fixture.schema_uuid,
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
    if (evidence.evidence_kind == kind && scratchbird::tests::EvidenceTextEquals(evidence.evidence_id, value)) {
      return true;
    }
  }
  return false;
}

bool HasEvidence(const api::EngineApiResult& result, std::string_view kind,
                 const api::EngineUuid& identity) {
  for (const auto& evidence : result.evidence) {
    const auto* value = std::get_if<api::EngineUuid>(&evidence.evidence_id);
    if (evidence.evidence_kind == kind && value && *value == identity) return true;
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
  select.target_object.uuid = fixture.view_uuid;
  select.target_object.object_kind = "view";
  select.option_envelopes = {
      std::string("source_kind:") +
          api::kRelationDescriptorProjectionSourceKind,
      std::string("result_projection:") +
          api::kRelationDescriptorProjectionMarkerV1,
      std::string("dml_surface_variant:") +
          api::kRelationDescriptorProjectionTypeInventoryVariantV1,
      "source_uuid:" + api::MetadataUuidBytes(fixture.relation_uuid),
      "source_fingerprint:" + api::MetadataUuidBytes(descriptor.descriptor_uuid),
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
                          descriptor.descriptor_uuid) &&
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
  const auto unsigned_value = [](const api::EngineTypedValue& value) {
    std::string_view bytes;
    Require(scratchbird::engine::PublicUInt64ScalarPayloadV1(value, &bytes) &&
                bytes.size() == 8,
            "catalog UINT64 projection is not the actual native carrier");
    std::uint64_t result = 0;
    for (unsigned i = 0; i < 8; ++i)
      result |= std::uint64_t(static_cast<unsigned char>(bytes[i])) << (8 * i);
    return result;
  };
  Require(unsigned_value(Field(integer, "ordinal")) == 0 &&
              unsigned_value(Field(character, "ordinal")) == 1 &&
              unsigned_value(Field(text_blob, "ordinal")) == 2,
          "catalog projection rows are not in descriptor ordinal order");
  Require(Field(integer, "character_length").isSqlNull() &&
              Field(integer, "charset_uuid").isSqlNull() &&
              Field(integer, "collation_uuid").isSqlNull(),
          "non-character projection resources are not SQL NULL");
  Require(!Field(character, "character_length").isSqlNull() &&
              unsigned_value(Field(character, "character_length")) == 20 &&
              !Field(character, "charset_canonical_name").isSqlNull() &&
              !Field(character, "collation_canonical_name").isSqlNull(),
          "character projection resources are incomplete");
  std::string_view large_object;
  Require(Field(text_blob, "character_length").isSqlNull() &&
              scratchbird::engine::PublicBooleanScalarPayloadV1(
                  Field(text_blob, "text_large_object"), &large_object) &&
              large_object.size() == 1 && large_object[0] == 1,
          "text large-object length/state is invalid");
  for (std::size_t i = 0; i < selected.result_shape.rows.size(); ++i) {
    const auto& row = selected.result_shape.rows[i];
    const auto& column = descriptor.columns[i];
    for (const auto& [name, identity] :
         std::initializer_list<std::pair<std::string_view, api::EngineUuid>>{
             {"column_uuid", column.column_uuid},
             {"charset_uuid", column.charset_uuid},
             {"collation_uuid", column.collation_uuid}}) {
      const auto& value = Field(row, name);
      Require(value.encoded_value.empty(),
              "catalog projection rendered a system UUID as text");
      if (identity.is_nil()) {
        Require(value.isSqlNull() && value.binary_value.empty(),
                "absent catalog identity was not a payload-free SQL NULL");
      } else {
        Require(!value.isSqlNull() && value.binary_value.size() == 16 &&
                    std::equal(value.binary_value.begin(), value.binary_value.end(),
                               identity.bytes.begin()),
                "catalog projection changed a binary system identity");
      }
    }
  }
  for (const auto& row : selected.result_shape.rows) {
    std::string_view bytes;
    Require(scratchbird::engine::PublicUInt64ScalarPayloadV1(
                Field(row, "character_length"), &bytes),
            "catalog nullable length contains display bytes or invalid state");
    Require(scratchbird::engine::PublicBooleanScalarPayloadV1(
                Field(row, "text_large_object"), &bytes) && bytes.size() == 1,
            "catalog Boolean projection contains display bytes or invalid state");
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
      api::MetadataUuidBytes(NewUuid(UuidKind::object, fixture.salt + 40));
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

  auto retired_result_shape = SelectRequest(fixture, context, descriptor);
  retired_result_shape.option_envelopes[1] =
      "result_projection:any_legacy_projection";
  const auto retired_result_shape_result =
      api::EngineSelectRows(retired_result_shape);
  Require(!retired_result_shape_result.ok &&
              HasDiagnosticDetail(
                  retired_result_shape_result,
                  "dml.select_rows:retired_result_shape_not_supported"),
          "generic retired result projection was not refused");

  auto excluded_dependency = context;
  excluded_dependency.statement_metadata_snapshot_engine_owned = true;
  excluded_dependency.statement_metadata_snapshot_visible_through_local_transaction_id =
      context.local_transaction_id;
  excluded_dependency
      .statement_metadata_snapshot_active_excluded_local_transaction_ids
      .push_back(fixture.function_creator_tx);
  excluded_dependency.statement_metadata_snapshot_active_excluded_local_transaction_ids
      .push_back(fixture.function_successor_creator_tx);
  const auto excluded = api::DescribeEngineCatalogRelationProjectionView(
      excluded_dependency, fixture.view_uuid);
  Require(excluded.diagnostic.error && !excluded.present,
          "catalog projection accepted a metadata-excluded function");
}

void RequireNativeScalarProjection(
    const Fixture& fixture, const api::EngineRequestContext& context,
    const api::MgaRelationStorageDescriptor& descriptor) {
  const auto selected = api::EngineSelectRows(SelectRequest(fixture, context, descriptor));
  RequireOk(selected, "native catalog scalar projection failed");
  Require(selected.visible_count == 2 && selected.result_shape.rows.size() == 2 &&
              selected.result_shape.columns.size() == 10,
          "native catalog scalar projection shape invalid");
  for (std::size_t i = 0; i < 2; ++i) {
    const auto& row = selected.result_shape.rows[i];
    Require(row.requested_row_uuid == descriptor.columns[i].column_uuid,
            "native scalar projection lost actual persisted column identity");
    std::string_view bytes;
    Require(scratchbird::engine::PublicUInt64ScalarPayloadV1(Field(row, "ordinal"), &bytes) &&
                bytes.size() == 8,
            "live catalog ordinal not native UINT64");
    for (unsigned n = 0; n < 8; ++n)
      Require(static_cast<unsigned char>(bytes[n]) == ((std::uint64_t(i) >> (8 * n)) & 255),
              "live catalog ordinal byte significance changed");
    const auto& length = Field(row, "character_length");
    Require(scratchbird::engine::PublicUInt64ScalarPayloadV1(length, &bytes),
            "live catalog nullable length has malformed carrier");
    if (!i) Require(length.isSqlNull() && bytes.empty(),
                    "absent native length became zero or display text");
    else {
      Require(!length.isSqlNull() && bytes.size() == 8 &&
                  static_cast<unsigned char>(bytes[0]) == 20,
              "live catalog character length changed");
      for (unsigned n = 1; n < 8; ++n)
        Require(bytes[n] == 0, "live character length high bits changed");
    }
    Require(scratchbird::engine::PublicBooleanScalarPayloadV1(
                Field(row, "text_large_object"), &bytes) &&
                bytes.size() == 1 && bytes[0] == 0,
            "live catalog false flag is not native BOOLEAN");
    const auto& identity = Field(row, "column_uuid");
    Require(identity.descriptor.canonical_type_name == "uuid" && identity.encoded_value.empty() &&
                identity.binary_value.size() == 16 &&
                std::equal(identity.binary_value.begin(), identity.binary_value.end(),
                           descriptor.columns[i].column_uuid.bytes.begin()),
            "live catalog scalar row carries a textual or changed UUID");
  }
}

}  // namespace

int main(int argc, char** argv) {
  try {
  const bool native_scalars = argc == 2 && std::string_view(argv[1]) == "--native-scalars";
  Require(argc == 1 || native_scalars, "unexpected catalog projection test mode");
  auto policy = memory::DefaultLocalEngineMemoryPolicy();
  policy.policy_name = "database_lifecycle_relation_descriptor_projection";
  const auto configured = memory::ConfigureDefaultMemoryManagerForFixture(
      policy, "database_lifecycle_relation_descriptor_projection");
  Require(configured.ok(),
          "catalog projection memory fixture configuration failed");

  Fixture fixture = CreateFixture();

  auto function_tx = Begin(fixture, 1);
  CreateSchemaAndFunction(fixture, function_tx);
  auto stable_reader = Begin(fixture, 100, true, "repeatable_read");
  auto current_reader = Begin(fixture, 101, true);
  Commit(function_tx);
  const auto stable_functions = api::LoadExecutableObjectLifecycleState(stable_reader);
  Require(stable_functions.ok && std::none_of(stable_functions.state.objects.begin(),
      stable_functions.state.objects.end(), [&](const auto& row) { return row.object_uuid == fixture.function_uuid; }),
      "stable reader admitted an earlier-allocated function committed after its BEGIN");
  const auto current_functions = api::LoadExecutableObjectLifecycleState(current_reader);
  Require(current_functions.ok && std::any_of(current_functions.state.objects.begin(),
      current_functions.state.objects.end(), [&](const auto& row) { return row.object_uuid == fixture.function_uuid; }),
      "read-committed reader missed actual committed function");
  Rollback(stable_reader);
  Rollback(current_reader);
  RequireFunctionSuccessorAdmission(fixture);

  auto view_tx = Begin(fixture, 2);
  CreateViewAndRefusalProbes(fixture, view_tx);
  Commit(view_tx);

  auto relation_tx = Begin(fixture, 3);
  const auto descriptor = CreateSourceRelation(fixture, relation_tx, !native_scalars);
  Commit(relation_tx);

  auto read_tx = Begin(fixture, 4);
  if (native_scalars) RequireNativeScalarProjection(fixture, read_tx, descriptor);
  else RequireProjectionRows(fixture, read_tx, descriptor);
  Rollback(read_tx);

  std::error_code cleanup_error;
  std::filesystem::remove_all(fixture.directory, cleanup_error);
  Require(!cleanup_error, "catalog projection fixture cleanup failed");

  std::cout << "database_lifecycle_relation_descriptor_projection_conformance"
            << (native_scalars ? " native_scalars" : "") << ": PASS\n";
  return EXIT_SUCCESS;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return EXIT_FAILURE;
  }
}

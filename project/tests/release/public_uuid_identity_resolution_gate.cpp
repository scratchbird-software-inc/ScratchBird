// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "backup_archive/backup_archive_api.hpp"
#include "catalog/name_resolution_api.hpp"
#include "database_lifecycle.hpp"
#include "ddl/create_api.hpp"
#include "memory.hpp"
#include "public_release_authz_fixture.hpp"
#include "transaction/transaction_api.hpp"
#include "uuid.hpp"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>

namespace {

namespace api = scratchbird::engine::internal_api;
namespace db = scratchbird::storage::database;
namespace memory = scratchbird::core::memory;
namespace uuid = scratchbird::core::uuid;

using scratchbird::core::platform::TypedUuid;
using scratchbird::core::platform::UuidKind;
using scratchbird::core::platform::u32;
using scratchbird::core::platform::u64;

constexpr u64 kBaseMillis = 1771400000000ull;
constexpr u32 kPageSize = 16384;

struct CleanupDir {
  std::filesystem::path root;
  ~CleanupDir() {
    if (root.empty()) return;
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
  }
};

struct DatabaseFixture {
  std::filesystem::path root;
  std::filesystem::path path;
  TypedUuid database_uuid;
  TypedUuid filespace_uuid;
};

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view message) {
  if (!condition) {
    Fail(message);
  }
}

void RequireApiOk(const api::EngineApiResult& result, std::string_view message) {
  if (result.ok) {
    return;
  }
  std::cerr << message;
  if (!result.diagnostics.empty()) {
    const auto& diagnostic = result.diagnostics.front();
    std::cerr << ": " << diagnostic.code << ':' << diagnostic.message_key
              << ':' << diagnostic.detail;
  }
  std::cerr << '\n';
  std::exit(EXIT_FAILURE);
}

TypedUuid MakeUuid(UuidKind kind, u64 offset) {
  const auto generated = uuid::GenerateEngineIdentityV7(kind, kBaseMillis + offset);
  Require(generated.ok(), "PCR-008 UUID generation failed");
  return generated.value;
}

std::string UuidText(const TypedUuid& typed_uuid) {
  return uuid::UuidToString(typed_uuid.value);
}

bool IsGeneratedObjectUuid(const std::string& value) {
  return uuid::ParseTypedUuid(UuidKind::object, value).ok();
}

memory::AllocationPolicy MemoryPolicy() {
  memory::AllocationPolicy policy;
  policy.policy_name = "public_uuid_identity_resolution_gate";
  policy.hard_limit_bytes = 64ull * 1024ull * 1024ull;
  policy.soft_limit_bytes = 48ull * 1024ull * 1024ull;
  policy.per_context_limit_bytes = 32ull * 1024ull * 1024ull;
  policy.page_buffer_pool_limit_bytes = 16ull * 1024ull * 1024ull;
  policy.track_allocations = true;
  policy.zero_memory_on_release = true;
  return policy;
}

void ConfigureMemoryFixture() {
  const auto configured = memory::ConfigureDefaultMemoryManagerForFixture(
      MemoryPolicy(), "public_uuid_identity_resolution_gate");
  Require(configured.ok(), "PCR-008 memory manager fixture configure failed");
  Require(configured.fixture_mode, "PCR-008 memory manager did not use fixture mode");
}

api::EngineLocalizedName Name(std::string text) {
  api::EngineLocalizedName name;
  name.language_tag = "en";
  name.name_class = "primary";
  name.name = std::move(text);
  name.raw_name_text = name.name;
  name.display_name = name.name;
  name.default_name = true;
  return name;
}

DatabaseFixture CreateDatabaseFixture(const std::filesystem::path& root, u64 seed) {
  std::filesystem::create_directories(root);
  DatabaseFixture fixture;
  fixture.root = root;
  fixture.path = root / "public_uuid_identity_resolution.sbdb";
  fixture.database_uuid = MakeUuid(UuidKind::database, seed);
  fixture.filespace_uuid = MakeUuid(UuidKind::filespace, seed + 1);

  db::DatabaseCreateConfig create;
  create.path = fixture.path.string();
  create.database_uuid = fixture.database_uuid;
  create.filespace_uuid = fixture.filespace_uuid;
  create.page_size = kPageSize;
  create.creation_unix_epoch_millis = kBaseMillis + seed;
  create.require_resource_seed_pack = false;
  create.allow_minimal_resource_bootstrap = true;
  create.allow_overwrite = true;

  const auto created = db::CreateDatabaseFile(create);
  if (!created.ok()) {
    std::cerr << created.diagnostic.diagnostic_code << ':'
              << created.diagnostic.message_key << '\n';
  }
  Require(created.ok(), "PCR-008 database create failed");
  Require(created.state.local_transaction_inventory_present,
          "PCR-008 database create omitted transaction inventory");
  return fixture;
}

api::EngineRequestContext Context(const DatabaseFixture& fixture,
                                  std::string request_id) {
  api::EngineRequestContext context;
  context.trust_mode = api::EngineTrustMode::server_isolated;
  context.request_id = std::move(request_id);
  context.database_path = fixture.path.string();
  context.database_uuid.canonical = UuidText(fixture.database_uuid);
  context.principal_uuid.canonical = UuidText(MakeUuid(UuidKind::principal, 20));
  context.session_uuid.canonical = UuidText(MakeUuid(UuidKind::object, 21));
  context.security_context_present = true;
  context.identifier_profile_uuid = "sbsql_v3";
  context.language_context.language_tag = "en";
  context.language_context.default_language_tag = "en";
  context.catalog_generation_id = 1;
  context.security_epoch = 1;
  context.resource_epoch = 1;
  context.name_resolution_epoch = 1;
  context.trace_tags.push_back("right:CATALOG_MUTATE");
  scratchbird::tests::release::GrantMaterializedRight(
      &context, "CATALOG_MUTATE");
  return context;
}

api::EngineRequestContext BackupContext(const DatabaseFixture& fixture,
                                        std::string request_id) {
  auto context = Context(fixture, std::move(request_id));
  context.trace_tags.push_back("right:BACKUP_CREATE");
  scratchbird::tests::release::GrantMaterializedRight(
      &context, "BACKUP_CREATE");
  return context;
}

api::EngineRequestContext RestoreContext(const DatabaseFixture& fixture,
                                         std::string request_id) {
  auto context = Context(fixture, std::move(request_id));
  context.trace_tags.push_back("right:BACKUP_RESTORE");
  scratchbird::tests::release::GrantMaterializedRight(
      &context, "BACKUP_RESTORE");
  return context;
}

api::EngineRequestContext Begin(const DatabaseFixture& fixture,
                                std::string request_id) {
  api::EngineBeginTransactionRequest request;
  request.context = Context(fixture, std::move(request_id));
  request.isolation_level = "read_committed";
  const auto begun = api::EngineBeginTransaction(request);
  RequireApiOk(begun, "PCR-008 transaction begin failed");

  auto context = request.context;
  context.local_transaction_id = begun.local_transaction_id;
  context.transaction_uuid = begun.transaction_uuid;
  context.snapshot_visible_through_local_transaction_id =
      begun.snapshot_visible_through_local_transaction_id;
  context.transaction_isolation_level = begun.isolation_level;
  return context;
}

void Commit(api::EngineRequestContext* context) {
  api::EngineCommitTransactionRequest request;
  request.context = *context;
  const auto committed = api::EngineCommitTransaction(request);
  RequireApiOk(committed, "PCR-008 transaction commit failed");
  context->local_transaction_id = 0;
  context->transaction_uuid.canonical.clear();
}

void Rollback(const api::EngineRequestContext& context) {
  api::EngineRollbackTransactionRequest request;
  request.context = context;
  const auto rolled_back = api::EngineRollbackTransaction(request);
  RequireApiOk(rolled_back, "PCR-008 transaction rollback failed");
}

api::EngineCreateSchemaResult CreateGeneratedSchema(api::EngineRequestContext context) {
  api::EngineCreateSchemaRequest request;
  request.context = std::move(context);
  request.localized_names.push_back(Name("pcr008_schema"));
  Require(request.target_object.uuid.canonical.empty(),
          "PCR-008 schema request unexpectedly carried a literal object UUID");

  const auto created = api::EngineCreateSchema(request);
  RequireApiOk(created, "PCR-008 generated schema create failed");
  Require(IsGeneratedObjectUuid(created.primary_object.uuid.canonical),
          "PCR-008 schema create did not return a generated object UUID");
  Require(created.primary_object.object_kind == "schema",
          "PCR-008 schema create returned wrong object kind");
  return created;
}

api::EngineCreateTableResult CreateGeneratedTable(
    api::EngineRequestContext context,
    const api::EngineObjectReference& schema) {
  api::EngineCreateTableRequest request;
  request.context = std::move(context);
  request.target_schema = schema;
  request.table_names.push_back(Name("pcr008_customer"));
  api::EngineColumnDefinition id_column;
  id_column.names.push_back(Name("id"));
  id_column.descriptor.descriptor_kind = "datatype";
  id_column.descriptor.canonical_type_name = "int64";
  id_column.descriptor.encoded_descriptor = "type=int64";
  id_column.ordinal = 0;
  id_column.nullable = false;
  request.table_columns.push_back(std::move(id_column));
  Require(request.requested_table_uuid.canonical.empty(),
          "PCR-008 table request unexpectedly carried a literal table UUID");

  const auto created = api::EngineCreateTable(request);
  RequireApiOk(created, "PCR-008 generated table create failed");
  Require(IsGeneratedObjectUuid(created.table_object.uuid.canonical),
          "PCR-008 table create did not return a generated object UUID");
  Require(created.table_object.object_kind == "table",
          "PCR-008 table create returned wrong object kind");
  Require(created.primary_object.uuid.canonical == created.table_object.uuid.canonical,
          "PCR-008 table primary object and table object diverged");
  Require(!created.created_catalog_records.empty(),
          "PCR-008 table create omitted returned catalog record identity");
  Require(created.created_catalog_records.front().uuid.canonical ==
              created.table_object.uuid.canonical,
          "PCR-008 returned catalog record did not carry generated table UUID");
  return created;
}

api::EngineResolveNameRequest ResolveTableRequest(
    api::EngineRequestContext context,
    const api::EngineObjectReference& schema) {
  api::EngineResolveNameRequest request;
  request.context = std::move(context);
  request.target_schema = schema;
  request.target_object.object_kind = "table";
  request.localized_names.push_back(Name("pcr008_customer"));
  request.sql_object_reference.expected_object_type = "table";
  return request;
}

api::EngineMapUuidToNameRequest MapTableRequest(
    api::EngineRequestContext context,
    const api::EngineObjectReference& table) {
  api::EngineMapUuidToNameRequest request;
  request.context = std::move(context);
  request.target_object = table;
  return request;
}

void RequireCleanReopen(const DatabaseFixture& fixture,
                        std::string_view phase) {
  const auto clean = db::MarkDatabaseCleanShutdown(fixture.path.string());
  Require(clean.ok(), "PCR-009 clean shutdown mark failed before reopen");

  db::DatabaseOpenConfig open;
  open.path = fixture.path.string();
  open.read_only = false;
  const auto opened = db::OpenDatabaseFile(open);
  if (!opened.ok()) {
    std::cerr << opened.diagnostic.diagnostic_code << ':'
              << opened.diagnostic.message_key << '\n';
  }
  Require(opened.ok(), "PCR-009 database reopen failed");
  Require(opened.state.local_transaction_inventory_present,
          "PCR-009 reopen omitted transaction inventory");
  (void)phase;
}

void RequireResolveGeneratedTable(const DatabaseFixture& fixture,
                                  const api::EngineObjectReference& schema,
                                  const api::EngineObjectReference& table,
                                  std::string request_id,
                                  std::string_view phase) {
  auto read_context = Begin(fixture, std::move(request_id));
  if (!schema.uuid.canonical.empty()) {
    read_context.current_schema_uuid = schema.uuid;
  }
  const auto resolved =
      api::EngineResolveName(ResolveTableRequest(read_context, schema));
  const std::string resolve_message =
      "PCR-009 resolver failed for generated table during " +
      std::string(phase);
  RequireApiOk(resolved, resolve_message);
  Require(resolved.bound_object_identity.object_uuid.canonical ==
              table.uuid.canonical,
          "PCR-009 name resolver did not return generated table UUID");
  if (!schema.uuid.canonical.empty()) {
    Require(resolved.bound_object_identity.resolved_schema_uuid.canonical ==
                schema.uuid.canonical,
            "PCR-009 resolver did not bind the generated schema UUID");
  }

  const auto mapped = api::EngineMapUuidToName(MapTableRequest(read_context, table));
  const std::string map_message =
      "PCR-009 UUID-to-name map failed for generated table during " +
      std::string(phase);
  RequireApiOk(mapped, map_message);
  Require(mapped.bound_object_identity.object_uuid.canonical ==
              table.uuid.canonical,
          "PCR-009 UUID-to-name map returned the wrong object UUID");
  Rollback(read_context);
}

void AddRestoreInspectionOptions(std::vector<std::string>* options) {
  options->push_back("restore_inspection_open:true");
  options->push_back("recovery_classification_verified:true");
}

api::EngineStartLogicalBackupResult StartLogicalBackup(
    const DatabaseFixture& fixture,
    const std::filesystem::path& manifest_path) {
  api::EngineStartLogicalBackupRequest request;
  request.context = BackupContext(fixture, "pcr009-source-logical-backup");
  request.option_envelopes.push_back("target_uri:" + manifest_path.string());
  request.option_envelopes.push_back("filespace_uuid:" +
                                     UuidText(fixture.filespace_uuid));
  const auto backed_up = api::EngineStartLogicalBackup(request);
  RequireApiOk(backed_up, "PCR-009 logical backup failed");
  Require(backed_up.table_count == 1,
          "PCR-009 logical backup did not include generated table");
  Require(backed_up.row_count == 0,
          "PCR-009 logical backup unexpectedly included data rows");
  return backed_up;
}

void RestoreLogicalBackup(const DatabaseFixture& target,
                          const std::filesystem::path& manifest_path) {
  auto restore_context = Begin(target, "pcr009-target-logical-restore");
  restore_context.trace_tags.push_back("right:BACKUP_RESTORE");
  scratchbird::tests::release::GrantMaterializedRight(
      &restore_context, "BACKUP_RESTORE");
  api::EngineRestoreLogicalBackupRequest request;
  request.context = restore_context;
  request.option_envelopes.push_back("source_manifest_uri:" +
                                     manifest_path.string());
  AddRestoreInspectionOptions(&request.option_envelopes);
  const auto restored = api::EngineRestoreLogicalBackup(request);
  RequireApiOk(restored, "PCR-009 logical restore failed");
  Require(restored.restored_table_count == 1,
          "PCR-009 logical restore did not restore generated table");
  Commit(&restore_context);
}

void ProveGeneratedObjectUuidResolution(const std::filesystem::path& work_dir) {
  CleanupDir cleanup{work_dir};
  std::error_code ignored;
  std::filesystem::remove_all(work_dir, ignored);
  ConfigureMemoryFixture();

  const auto source = CreateDatabaseFixture(work_dir / "source", 1);
  const auto target = CreateDatabaseFixture(work_dir / "target", 101);
  const auto manifest_path = work_dir / "uuid_identity_resolution.sblbk";

  auto create_context = Begin(source, "pcr008-create-generated-objects");
  const auto created_schema = CreateGeneratedSchema(create_context);
  const auto created_table =
      CreateGeneratedTable(create_context, created_schema.primary_object);
  const std::string generated_table_uuid = created_table.table_object.uuid.canonical;
  Commit(&create_context);

  RequireResolveGeneratedTable(source,
                               created_schema.primary_object,
                               created_table.table_object,
                               "pcr008-resolve-generated-table",
                               "create");

  RequireCleanReopen(source, "source");
  RequireResolveGeneratedTable(source,
                               created_schema.primary_object,
                               created_table.table_object,
                               "pcr009-resolve-after-reopen",
                               "reopen");

  const auto backup = StartLogicalBackup(source, manifest_path);
  Require(!backup.backup_uuid.canonical.empty(),
          "PCR-009 logical backup did not return backup UUID");

  RestoreLogicalBackup(target, manifest_path);
  RequireCleanReopen(target, "target");
  RequireResolveGeneratedTable(target,
                               {},
                               created_table.table_object,
                               "pcr009-resolve-after-restore",
                               "restore");
  Require(IsGeneratedObjectUuid(generated_table_uuid),
          "PCR-009 generated table UUID no longer parses as object identity");
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 2) {
    std::cerr << "usage: public_uuid_identity_resolution_gate <work-dir>\n";
    return EXIT_FAILURE;
  }
  ProveGeneratedObjectUuidResolution(argv[1]);
  std::cout << "public_uuid_identity_resolution_gate=passed\n";
  return EXIT_SUCCESS;
}

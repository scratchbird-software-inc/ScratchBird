// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "catalog/catalog_object_lifecycle.hpp"
#include "database_lifecycle.hpp"
#include "ddl/create_api.hpp"
#include "ddl/drop_api.hpp"
#include "transaction/transaction_api.hpp"
#include "uuid.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>

namespace {

namespace catalog_api = scratchbird::engine::internal_api;
namespace db = scratchbird::storage::database;
namespace uuid = scratchbird::core::uuid;
using scratchbird::core::platform::UuidKind;

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view message) {
  if (!condition) { Fail(message); }
}

template <typename TResult>
std::string DiagnosticCode(const TResult& result) {
  if (result.diagnostics.empty()) { return {}; }
  return result.diagnostics.front().code;
}

template <typename TResult>
void RequireOk(const TResult& result, std::string_view message) {
  if (!result.ok) { std::cerr << DiagnosticCode(result) << '\n'; }
  Require(result.ok, message);
}

template <typename TResult>
void RequireDiagnostic(const TResult& result,
                       std::string_view expected,
                       std::string_view message) {
  Require(!result.ok, message);
  if (DiagnosticCode(result) != expected) {
    std::cerr << "expected=" << expected << " actual=" << DiagnosticCode(result) << '\n';
  }
  Require(DiagnosticCode(result) == expected, message);
}

std::uint64_t CurrentUnixMillis() {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());
}

std::filesystem::path TestPath() {
  return std::filesystem::temp_directory_path() /
         ("sb_dblc_013u_catalog_object_" + std::to_string(CurrentUnixMillis()) + ".sbdb");
}

std::string CreateDatabase(const std::filesystem::path& path) {
  db::DatabaseCreateConfig create;
  create.path = path.string();
  create.database_uuid = uuid::GenerateEngineIdentityV7(UuidKind::database, 1779810001000).value;
  create.filespace_uuid = uuid::GenerateEngineIdentityV7(UuidKind::filespace, 1779810001001).value;
  create.page_size = 16384;
  create.creation_unix_epoch_millis = 1779810001002;
  create.allow_minimal_resource_bootstrap = true;
  create.require_resource_seed_pack = false;
  create.allow_overwrite = true;
  const auto created = db::CreateDatabaseFile(create);
  if (!created.ok()) {
    std::cerr << created.diagnostic.diagnostic_code << ":" << created.diagnostic.message_key << '\n';
  }
  Require(created.ok(), "catalog object database create failed");
  return uuid::UuidToString(create.database_uuid.value);
}

catalog_api::EngineRequestContext BaseContext(const std::filesystem::path& path,
                                              const std::string& database_uuid,
                                              std::string principal) {
  catalog_api::EngineRequestContext context;
  context.trust_mode = catalog_api::EngineTrustMode::server_isolated;
  context.request_id = "catalog-object-conformance";
  context.database_path = path.string();
  context.database_uuid.canonical = database_uuid;
  context.principal_uuid.canonical = std::move(principal);
  context.session_uuid.canonical = "session-" + std::to_string(CurrentUnixMillis());
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

catalog_api::EngineRequestContext Begin(const std::filesystem::path& path,
                                        const std::string& database_uuid,
                                        std::string principal) {
  catalog_api::EngineBeginTransactionRequest request;
  request.context = BaseContext(path, database_uuid, std::move(principal));
  request.isolation_level = "read_committed";
  const auto begun = catalog_api::EngineBeginTransaction(request);
  if (!begun.ok) {
    for (const auto& diagnostic : begun.diagnostics) {
      std::cerr << diagnostic.code << ":" << diagnostic.detail << '\n';
    }
  }
  Require(begun.ok, "begin transaction failed");
  auto context = request.context;
  context.local_transaction_id = begun.local_transaction_id;
  context.transaction_uuid = begun.transaction_uuid;
  context.snapshot_visible_through_local_transaction_id = begun.snapshot_visible_through_local_transaction_id;
  context.transaction_isolation_level = begun.isolation_level;
  return context;
}

void Commit(const catalog_api::EngineRequestContext& context) {
  catalog_api::EngineCommitTransactionRequest request;
  request.context = context;
  const auto committed = catalog_api::EngineCommitTransaction(request);
  if (!committed.ok) {
    for (const auto& diagnostic : committed.diagnostics) {
      std::cerr << diagnostic.code << ":" << diagnostic.detail << '\n';
    }
  }
  Require(committed.ok, "commit transaction failed");
}

void Rollback(const catalog_api::EngineRequestContext& context) {
  catalog_api::EngineRollbackTransactionRequest request;
  request.context = context;
  const auto rolled_back = catalog_api::EngineRollbackTransaction(request);
  Require(rolled_back.ok, "rollback transaction failed");
}

catalog_api::EngineLocalizedName Name(std::string value) {
  catalog_api::EngineLocalizedName name;
  name.language_tag = "en";
  name.name_class = "primary";
  name.name = value;
  name.raw_name_text = value;
  name.display_name = value;
  name.default_name = true;
  return name;
}

catalog_api::EngineCatalogCreateObjectRequest CreateRequest(
    const catalog_api::EngineRequestContext& context,
    std::string object_uuid,
    std::string object_kind,
    std::string schema_uuid,
    std::string name) {
  catalog_api::EngineCatalogCreateObjectRequest request;
  request.context = context;
  request.target_object.uuid.canonical = std::move(object_uuid);
  request.target_object.object_kind = std::move(object_kind);
  request.target_schema.uuid.canonical = std::move(schema_uuid);
  request.localized_names.push_back(Name(std::move(name)));
  return request;
}

catalog_api::EngineCatalogResolveObjectNameRequest ResolveRequest(
    const catalog_api::EngineRequestContext& context,
    std::string object_kind,
    std::string schema_uuid,
    std::string name) {
  catalog_api::EngineCatalogResolveObjectNameRequest request;
  request.context = context;
  request.target_object.object_kind = std::move(object_kind);
  request.target_schema.uuid.canonical = std::move(schema_uuid);
  request.localized_names.push_back(Name(std::move(name)));
  return request;
}

catalog_api::EngineCatalogLookupObjectRequest LookupRequest(
    const catalog_api::EngineRequestContext& context,
    std::string object_uuid,
    std::string object_kind) {
  catalog_api::EngineCatalogLookupObjectRequest request;
  request.context = context;
  request.target_object.uuid.canonical = std::move(object_uuid);
  request.target_object.object_kind = std::move(object_kind);
  return request;
}

bool HasEvidence(const catalog_api::EngineApiResult& result,
                 std::string_view kind,
                 std::string_view value) {
  for (const auto& evidence : result.evidence) {
    if (evidence.evidence_kind == kind && evidence.evidence_id == value) { return true; }
  }
  return false;
}

void TestCreateAlterRenameDropAndDependencies(const std::filesystem::path& path,
                                              const std::string& database_uuid) {
  auto owner = Begin(path, database_uuid, "principal-owner");
  auto schema = CreateRequest(owner, "schema-app", "schema", "", "app");
  const auto created_schema = catalog_api::EngineCatalogCreateObject(schema);
  RequireOk(created_schema, "schema create failed");
  Require(created_schema.primary_object.uuid.canonical == "schema-app",
          "schema create was not UUID-first");
  Commit(owner);

  auto domain_context = Begin(path, database_uuid, "principal-owner");
  auto domain = CreateRequest(domain_context, "domain-customer-id", "domain", "schema-app", "customer_id");
  const auto created_domain = catalog_api::EngineCatalogCreateObject(domain);
  RequireOk(created_domain, "domain create failed");
  Commit(domain_context);

  auto table_context = Begin(path, database_uuid, "principal-owner");
  auto table = CreateRequest(table_context, "table-customers", "table", "schema-app", "customers");
  table.related_objects.push_back({{"domain-customer-id"}, "domain"});
  catalog_api::EngineColumnDefinition id_column;
  id_column.requested_column_uuid.canonical = "column-customers-id";
  id_column.names.push_back(Name("id"));
  id_column.descriptor.descriptor_kind = "scalar";
  id_column.descriptor.canonical_type_name = "text";
  id_column.ordinal = 1;
  id_column.nullable = false;
  table.columns.push_back(id_column);
  catalog_api::EngineConstraintDefinition primary_key;
  primary_key.requested_constraint_uuid.canonical = "constraint-customers-pk";
  primary_key.names.push_back(Name("customers_pk"));
  primary_key.constraint_kind = "primary_key";
  primary_key.canonical_constraint_envelope =
      "constraint_hash=customers_pk_hash;support_uuid=index-customers-pk;support_family=btree;"
      "key_descriptor_uuid=key-customers-pk;support_binding_uuid=support-customers-pk;"
      "subject_uuid=subject-customers-pk;subject_kind=column;subject_object_uuid=column-customers-id;"
      "subject_descriptor=id;dependency_uuid=dependency-customers-domain;"
      "dependency_object_uuid=domain-customer-id;dependency_kind=domain";
  table.constraints.push_back(primary_key);
  const auto created_table = catalog_api::EngineCatalogCreateObject(table);
  RequireOk(created_table, "table create with dependency failed");
  Commit(table_context);

  auto duplicate_context = Begin(path, database_uuid, "principal-owner");
  auto duplicate = CreateRequest(duplicate_context, "table-customers-2", "table", "schema-app", "customers");
  RequireDiagnostic(catalog_api::EngineCatalogCreateObject(duplicate),
                    catalog_api::kCatalogObjectDiagnosticDuplicateName,
                    "duplicate table name was accepted");
  Rollback(duplicate_context);

  auto blocked_context = Begin(path, database_uuid, "principal-owner");
  catalog_api::EngineCatalogDropObjectRequest blocked_drop;
  blocked_drop.context = blocked_context;
  blocked_drop.target_object.uuid.canonical = "domain-customer-id";
  blocked_drop.target_object.object_kind = "domain";
  RequireDiagnostic(catalog_api::EngineCatalogDropObject(blocked_drop),
                    catalog_api::kCatalogObjectDiagnosticDependencyBlockedDrop,
                    "dependency-blocked drop was accepted");
  Rollback(blocked_context);

  auto rename_context = Begin(path, database_uuid, "principal-owner");
  auto rename = catalog_api::EngineCatalogRenameObjectRequest{};
  rename.context = rename_context;
  rename.target_object.uuid.canonical = "table-customers";
  rename.target_object.object_kind = "table";
  rename.localized_names.push_back(Name("accounts"));
  const auto renamed = catalog_api::EngineCatalogRenameObject(rename);
  RequireOk(renamed, "table rename failed");
  Require(renamed.metadata_cache_epoch > created_table.metadata_cache_epoch,
          "rename did not advance metadata cache epoch");
  Commit(rename_context);

  auto old_name_context = Begin(path, database_uuid, "principal-owner");
  RequireDiagnostic(catalog_api::EngineCatalogResolveObjectName(
                        ResolveRequest(old_name_context, "table", "schema-app", "customers")),
                    catalog_api::kCatalogObjectDiagnosticNameNotFound,
                    "old table name still resolved after rename");
  Rollback(old_name_context);

  auto resolve_context = Begin(path, database_uuid, "principal-owner");
  const auto resolved = catalog_api::EngineCatalogResolveObjectName(
      ResolveRequest(resolve_context, "table", "schema-app", "accounts"));
  RequireOk(resolved, "renamed table did not resolve");
  Require(resolved.bound_object_identity.object_uuid.canonical == "table-customers",
          "resolver did not return the UUID identity");
  Rollback(resolve_context);

  auto alter_context = Begin(path, database_uuid, "principal-owner");
  catalog_api::EngineCatalogAlterObjectRequest alter;
  alter.context = alter_context;
  alter.target_object.uuid.canonical = "table-customers";
  alter.target_object.object_kind = "table";
  alter.option_envelopes.push_back("payload:shape=altered");
  const auto altered = catalog_api::EngineCatalogAlterObject(alter);
  RequireOk(altered, "table alter failed");
  Commit(alter_context);

  auto load_context = Begin(path, database_uuid, "principal-owner");
  const auto loaded = catalog_api::LoadCatalogObjectLifecycleState(load_context);
  Require(loaded.ok, "catalog lifecycle state did not load");
  bool saw_altered_table = false;
  bool saw_column_descriptor = false;
  bool saw_constraint_descriptor = false;
  bool saw_key_descriptor = false;
  bool saw_constraint_subject = false;
  bool saw_constraint_dependency = false;
  bool saw_constraint_support = false;
  for (const auto& object : loaded.state.objects) {
    if (object.object_uuid == "table-customers") {
      saw_altered_table = true;
      Require(object.definition_epoch == 3, "create/rename/alter definition epoch mismatch");
      Require(object.payload == "shape=altered", "alter payload was not persisted");
    }
  }
  for (const auto& column : loaded.state.columns) {
    if (column.column_uuid == "column-customers-id" &&
        column.owner_object_uuid == "table-customers" &&
        column.canonical_type_name == "text" &&
        !column.nullable) {
      saw_column_descriptor = true;
    }
  }
  for (const auto& constraint : loaded.state.constraints) {
    if (constraint.constraint_uuid == "constraint-customers-pk" &&
        constraint.constraint_class == "primary_key" &&
        constraint.owner_object_uuid == "table-customers" &&
        constraint.constraint_hash == "customers_pk_hash") {
      saw_constraint_descriptor = true;
    }
  }
  for (const auto& key : loaded.state.key_descriptors) {
    if (key.key_descriptor_uuid == "key-customers-pk" &&
        key.constraint_uuid == "constraint-customers-pk") {
      saw_key_descriptor = true;
    }
  }
  for (const auto& subject : loaded.state.constraint_subjects) {
    if (subject.subject_uuid == "subject-customers-pk" &&
        subject.constraint_uuid == "constraint-customers-pk" &&
        subject.subject_object_uuid == "column-customers-id") {
      saw_constraint_subject = true;
    }
  }
  for (const auto& dependency : loaded.state.constraint_dependencies) {
    if (dependency.dependency_uuid == "dependency-customers-domain" &&
        dependency.constraint_uuid == "constraint-customers-pk" &&
        dependency.dependency_object_uuid == "domain-customer-id") {
      saw_constraint_dependency = true;
    }
  }
  for (const auto& support : loaded.state.constraint_support_structures) {
    if (support.support_binding_uuid == "support-customers-pk" &&
        support.constraint_uuid == "constraint-customers-pk" &&
        support.support_uuid == "index-customers-pk") {
      saw_constraint_support = true;
    }
  }
  Require(saw_altered_table, "altered table missing from catalog state");
  Require(saw_column_descriptor, "column descriptor was not persisted in catalog lifecycle");
  Require(saw_constraint_descriptor, "constraint descriptor was not persisted in catalog lifecycle");
  Require(saw_key_descriptor, "key descriptor was not persisted in catalog lifecycle");
  Require(saw_constraint_subject, "constraint subject descriptor was not persisted in catalog lifecycle");
  Require(saw_constraint_dependency, "constraint dependency descriptor was not persisted in catalog lifecycle");
  Require(saw_constraint_support, "constraint support structure descriptor was not persisted in catalog lifecycle");
  Rollback(load_context);

  auto drop_table_context = Begin(path, database_uuid, "principal-owner");
  catalog_api::EngineCatalogDropObjectRequest drop_table;
  drop_table.context = drop_table_context;
  drop_table.target_object.uuid.canonical = "table-customers";
  drop_table.target_object.object_kind = "table";
  RequireOk(catalog_api::EngineCatalogDropObject(drop_table), "table drop failed");
  Commit(drop_table_context);

  auto drop_domain_context = Begin(path, database_uuid, "principal-owner");
  catalog_api::EngineCatalogDropObjectRequest drop_domain;
  drop_domain.context = drop_domain_context;
  drop_domain.target_object.uuid.canonical = "domain-customer-id";
  drop_domain.target_object.object_kind = "domain";
  RequireOk(catalog_api::EngineCatalogDropObject(drop_domain),
            "domain drop after dependent table removal failed");
  Commit(drop_domain_context);
}

void TestMissingUuidOwnershipCacheAndMga(const std::filesystem::path& path,
                                         const std::string& database_uuid) {
  auto missing_context = Begin(path, database_uuid, "principal-owner");
  auto missing_uuid = CreateRequest(missing_context, "", "table", "schema-app", "missing_uuid");
  RequireDiagnostic(catalog_api::EngineCatalogCreateObject(missing_uuid),
                    catalog_api::kCatalogObjectDiagnosticUuidRequired,
                    "create without UUID was accepted");
  Rollback(missing_context);

  auto denied_context = Begin(path, database_uuid, "principal-intruder");
  auto denied = CreateRequest(denied_context, "table-denied", "table", "schema-app", "denied");
  RequireDiagnostic(catalog_api::EngineCatalogCreateObject(denied),
                    catalog_api::kCatalogObjectDiagnosticSchemaOwnerDenied,
                    "schema ownership denial did not fire");
  Rollback(denied_context);

  auto create_context = Begin(path, database_uuid, "principal-owner");
  auto visible_object = CreateRequest(create_context, "table-epoch", "table", "schema-app", "epoch_probe");
  const auto created = catalog_api::EngineCatalogCreateObject(visible_object);
  RequireOk(created, "epoch probe create failed");
  const auto created_tx = create_context.local_transaction_id;
  Commit(create_context);

  auto stale_cache_context = Begin(path, database_uuid, "principal-owner");
  catalog_api::EngineCatalogValidateMetadataCacheRequest stale_cache;
  stale_cache.context = stale_cache_context;
  stale_cache.bound_object_identity.catalog_generation_id = created.metadata_cache_epoch - 1;
  RequireDiagnostic(catalog_api::EngineCatalogValidateMetadataCache(stale_cache),
                    catalog_api::kCatalogObjectDiagnosticCacheEpochStale,
                    "stale metadata cache epoch was accepted");
  Rollback(stale_cache_context);

  auto current_cache_context = Begin(path, database_uuid, "principal-owner");
  catalog_api::EngineCatalogValidateMetadataCacheRequest current_cache;
  current_cache.context = current_cache_context;
  current_cache.bound_object_identity.catalog_generation_id = created.metadata_cache_epoch;
  RequireOk(catalog_api::EngineCatalogValidateMetadataCache(current_cache),
            "current metadata cache epoch was rejected");
  Rollback(current_cache_context);

  auto hidden_context = Begin(path, database_uuid, "principal-owner");
  hidden_context.snapshot_visible_through_local_transaction_id = created_tx - 1;
  const auto hidden = catalog_api::EngineCatalogLookupObjectByUuid(
      LookupRequest(hidden_context, "table-epoch", "table"));
  RequireDiagnostic(hidden,
                    catalog_api::kCatalogObjectDiagnosticMgaVisibilityRefused,
                    "snapshot-stale MGA visibility lookup did not fail closed");
  Rollback(hidden_context);

  auto visible_context = Begin(path, database_uuid, "principal-owner");
  visible_context.snapshot_visible_through_local_transaction_id = created_tx;
  const auto visible = catalog_api::EngineCatalogLookupObjectByUuid(
      LookupRequest(visible_context, "table-epoch", "table"));
  RequireOk(visible, "MGA-visible object lookup failed");
  Rollback(visible_context);
}

void TestDdlSynonymCreateDropRoute(const std::filesystem::path& path,
                                   const std::string& database_uuid) {
  auto create_context = Begin(path, database_uuid, "principal-owner");
  catalog_api::EngineCreateSynonymRequest create;
  create.context = create_context;
  create.target_object.uuid.canonical = "syn-table-epoch";
  create.target_schema.uuid.canonical = "schema-app";
  create.localized_names.push_back(Name("epoch_alias"));
  create.related_objects.push_back({{"table-epoch"}, "table"});
  const auto created = catalog_api::EngineCreateSynonym(create);
  RequireOk(created, "DDL synonym create route failed");
  Require(created.bound_object_identity.object_uuid.canonical == "syn-table-epoch",
          "DDL synonym create did not preserve synonym UUID authority");
  Require(HasEvidence(created, "ddl_catalog_route", "sys.catalog.synonym"),
          "DDL synonym create did not route through sys.catalog.synonym");
  Commit(create_context);

  auto resolve_context = Begin(path, database_uuid, "principal-owner");
  const auto resolved = catalog_api::EngineCatalogResolveObjectName(
      ResolveRequest(resolve_context, "table", "schema-app", "epoch_alias"));
  RequireOk(resolved, "DDL-created synonym did not resolve");
  Require(resolved.bound_object_identity.object_uuid.canonical == "table-epoch",
          "DDL-created synonym did not dereference to target table UUID");
  Require(HasEvidence(resolved, "synonym_chain", "syn-table-epoch"),
          "DDL-created synonym did not retain synonym-chain evidence");
  Rollback(resolve_context);

  auto duplicate_context = Begin(path, database_uuid, "principal-owner");
  catalog_api::EngineCreateSynonymRequest duplicate = create;
  duplicate.context = duplicate_context;
  duplicate.target_object.uuid.canonical = "syn-table-epoch-duplicate";
  RequireDiagnostic(catalog_api::EngineCreateSynonym(duplicate),
                    catalog_api::kCatalogSynonymDiagnosticNameConflict,
                    "DDL synonym duplicate name did not use exact catalog diagnostic");
  Rollback(duplicate_context);

  auto drop_context = Begin(path, database_uuid, "principal-owner");
  catalog_api::EngineDropObjectRequest drop_synonym;
  drop_synonym.context = drop_context;
  drop_synonym.target_object.uuid.canonical = "syn-table-epoch";
  drop_synonym.target_object.object_kind = "synonym";
  const auto dropped = catalog_api::EngineDropObject(drop_synonym);
  RequireOk(dropped, "DDL synonym drop route failed");
  Require(HasEvidence(dropped, "ddl_catalog_route", "sys.catalog.synonym"),
          "DDL synonym drop did not route through sys.catalog.synonym");
  Commit(drop_context);

  auto missing_context = Begin(path, database_uuid, "principal-owner");
  RequireDiagnostic(catalog_api::EngineCatalogResolveObjectName(
                        ResolveRequest(missing_context, "table", "schema-app", "epoch_alias")),
                    catalog_api::kCatalogObjectDiagnosticNameNotFound,
                    "dropped DDL synonym still resolved by name");
  Rollback(missing_context);

  auto target_drop_context = Begin(path, database_uuid, "principal-owner");
  catalog_api::EngineCatalogDropObjectRequest drop_target;
  drop_target.context = target_drop_context;
  drop_target.target_object.uuid.canonical = "table-epoch";
  drop_target.target_object.object_kind = "table";
  RequireOk(catalog_api::EngineCatalogDropObject(drop_target),
            "synonym drop did not retire dependency blocking target drop");
  Commit(target_drop_context);
}

}  // namespace

int main() {
  const auto path = TestPath();
  const auto database_uuid = CreateDatabase(path);
  TestCreateAlterRenameDropAndDependencies(path, database_uuid);
  TestMissingUuidOwnershipCacheAndMga(path, database_uuid);
  TestDdlSynonymCreateDropRoute(path, database_uuid);

  std::error_code ignored;
  std::filesystem::remove(path, ignored);
  std::filesystem::remove(path.string() + ".sb.catalog_object_events", ignored);
  std::filesystem::remove(path.string() + ".sb.name_events", ignored);
  std::filesystem::remove(path.string() + ".sb.crud_events", ignored);
  return EXIT_SUCCESS;
}

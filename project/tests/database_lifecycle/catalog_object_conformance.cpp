#include "../support/engine_evidence_fixture.hpp"
// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "catalog/catalog_object_lifecycle.hpp"
#include "catalog/catalog_object_lifecycle_codec.hpp"
#include "catalog/name_registry_codec.hpp"
#include "mga_relation_store/mga_event_sequence_allocator.hpp"
#include "mga_relation_store/mga_savepoint_store.hpp"
#include "catalog/constraint_metadata_codec.hpp"
#include "../support/binary_uuid_fixture.hpp"
#include "../support/owned_temp_directory.hpp"
#include "database_lifecycle.hpp"
#include "ddl/create_api.hpp"
#include "ddl/drop_api.hpp"
#include "transaction/transaction_api.hpp"
#include "uuid.hpp"

#include <cstdlib>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

namespace catalog_api = scratchbird::engine::internal_api;
namespace db = scratchbird::storage::database;
namespace uuid = scratchbird::core::uuid;
using scratchbird::core::platform::UuidKind;

[[noreturn]] void Fail(std::string_view message) {
  throw std::runtime_error(std::string(message));
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

catalog_api::EngineUuid CreateDatabase(const std::filesystem::path& path) {
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
  return create.database_uuid.value;
}

catalog_api::EngineRequestContext BaseContext(const std::filesystem::path& path,
                                              const catalog_api::EngineUuid& database_uuid,
                                              catalog_api::EngineUuid principal) {
  catalog_api::EngineRequestContext context;
  context.trust_mode = catalog_api::EngineTrustMode::server_isolated;
  context.request_id = "catalog-object-conformance";
  context.database_path = path.string();
  context.database_uuid = database_uuid;
  context.principal_uuid = std::move(principal);
  context.session_uuid = scratchbird::tests::FixtureUuid(1378, 100);
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
                                        const catalog_api::EngineUuid& database_uuid,
                                        catalog_api::EngineUuid principal) {
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
    catalog_api::EngineUuid object_uuid,
    std::string object_kind,
    catalog_api::EngineUuid schema_uuid,
    std::string name) {
  catalog_api::EngineCatalogCreateObjectRequest request;
  request.context = context;
  request.target_object.uuid = std::move(object_uuid);
  request.target_object.object_kind = std::move(object_kind);
  request.target_schema.uuid = std::move(schema_uuid);
  request.localized_names.push_back(Name(std::move(name)));
  return request;
}

catalog_api::EngineCatalogResolveObjectNameRequest ResolveRequest(
    const catalog_api::EngineRequestContext& context,
    std::string object_kind,
    catalog_api::EngineUuid schema_uuid,
    std::string name) {
  catalog_api::EngineCatalogResolveObjectNameRequest request;
  request.context = context;
  request.target_object.object_kind = std::move(object_kind);
  request.target_schema.uuid = std::move(schema_uuid);
  request.localized_names.push_back(Name(std::move(name)));
  return request;
}

catalog_api::EngineCatalogLookupObjectRequest LookupRequest(
    const catalog_api::EngineRequestContext& context,
    catalog_api::EngineUuid object_uuid,
    std::string object_kind) {
  catalog_api::EngineCatalogLookupObjectRequest request;
  request.context = context;
  request.target_object.uuid = std::move(object_uuid);
  request.target_object.object_kind = std::move(object_kind);
  return request;
}

bool HasEvidence(const catalog_api::EngineApiResult& result,
                 std::string_view kind,
                 std::string_view value) {
  for (const auto& evidence : result.evidence) {
    if (evidence.evidence_kind == kind && scratchbird::tests::EvidenceTextEquals(evidence.evidence_id, value)) { return true; }
  }
  return false;
}

bool HasEvidence(const catalog_api::EngineApiResult& result,
                 std::string_view kind, const catalog_api::EngineUuid& identity) {
  for (const auto& evidence : result.evidence) {
    const auto* value = std::get_if<catalog_api::EngineUuid>(&evidence.evidence_id);
    if (evidence.evidence_kind == kind && value && *value == identity) return true;
  }
  return false;
}

void TestCreateAlterRenameDropAndDependencies(const std::filesystem::path& path,
                                              const catalog_api::EngineUuid& database_uuid) {
  auto owner = Begin(path, database_uuid, scratchbird::tests::FixtureUuid(1378, 8));
  auto schema = CreateRequest(owner, scratchbird::tests::FixtureUuid(1378, 9), "schema", {}, "app");
  const auto created_schema = catalog_api::EngineCatalogCreateObject(schema);
  RequireOk(created_schema, "schema create failed");
  Require(created_schema.primary_object.uuid == scratchbird::tests::FixtureUuid(1378, 9),
          "schema create was not UUID-first");
  Commit(owner);

  auto domain_context = Begin(path, database_uuid, scratchbird::tests::FixtureUuid(1378, 8));
  auto domain = CreateRequest(domain_context, scratchbird::tests::FixtureUuid(1378, 4), "domain", scratchbird::tests::FixtureUuid(1378, 9), "customer_id");
  const auto created_domain = catalog_api::EngineCatalogCreateObject(domain);
  RequireOk(created_domain, "domain create failed");
  Commit(domain_context);

  auto table_context = Begin(path, database_uuid, scratchbird::tests::FixtureUuid(1378, 8));
  auto table = CreateRequest(table_context, scratchbird::tests::FixtureUuid(1378, 14), "table", scratchbird::tests::FixtureUuid(1378, 9), "customers");
  table.related_objects.push_back({scratchbird::tests::FixtureUuid(1378, 4), "domain"});
  catalog_api::EngineColumnDefinition id_column;
  id_column.requested_column_uuid = scratchbird::tests::FixtureUuid(1378, 1);
  id_column.names.push_back(Name("id"));
  id_column.descriptor.descriptor_kind = "scalar";
  id_column.descriptor.canonical_type_name = "text";
  id_column.ordinal = 1;
  id_column.nullable = false;
  table.columns.push_back(id_column);
  catalog_api::EngineConstraintDefinition primary_key;
  primary_key.requested_constraint_uuid = scratchbird::tests::FixtureUuid(1378, 2);
  primary_key.names.push_back(Name("customers_pk"));
  primary_key.constraint_kind = "primary_key";
  catalog_api::CatalogConstraintMetadata constraint_metadata;
  constraint_metadata.text["constraint_hash"] = "customers_pk_hash";
  constraint_metadata.text["support_family"] = "btree";
  constraint_metadata.text["subject_kind"] = "column";
  constraint_metadata.text["subject_descriptor"] = "id";
  constraint_metadata.text["dependency_kind"] = "domain";
  constraint_metadata.identities["support_uuid"] = scratchbird::tests::FixtureUuid(1378, 5);
  constraint_metadata.identities["key_descriptor_uuid"] = scratchbird::tests::FixtureUuid(1378, 6);
  constraint_metadata.identities["support_binding_uuid"] = scratchbird::tests::FixtureUuid(1378, 11);
  constraint_metadata.identities["subject_uuid"] = scratchbird::tests::FixtureUuid(1378, 10);
  constraint_metadata.identities["subject_object_uuid"] = scratchbird::tests::FixtureUuid(1378, 1);
  constraint_metadata.identities["dependency_uuid"] = scratchbird::tests::FixtureUuid(1378, 3);
  constraint_metadata.identities["dependency_object_uuid"] = scratchbird::tests::FixtureUuid(1378, 4);
  Require(catalog_api::EncodeCatalogConstraintMetadata(constraint_metadata, &primary_key.canonical_constraint_envelope),
          "constraint binary metadata encoding failed");
  table.constraints.push_back(primary_key);
  const auto created_table = catalog_api::EngineCatalogCreateObject(table);
  RequireOk(created_table, "table create with dependency failed");
  Commit(table_context);

  auto duplicate_context = Begin(path, database_uuid, scratchbird::tests::FixtureUuid(1378, 8));
  auto duplicate = CreateRequest(duplicate_context, scratchbird::tests::FixtureUuid(1378, 15), "table", scratchbird::tests::FixtureUuid(1378, 9), "customers");
  RequireDiagnostic(catalog_api::EngineCatalogCreateObject(duplicate),
                    catalog_api::kCatalogObjectDiagnosticDuplicateName,
                    "duplicate table name was accepted");
  Rollback(duplicate_context);

  auto blocked_context = Begin(path, database_uuid, scratchbird::tests::FixtureUuid(1378, 8));
  catalog_api::EngineCatalogDropObjectRequest blocked_drop;
  blocked_drop.context = blocked_context;
  blocked_drop.target_object.uuid = scratchbird::tests::FixtureUuid(1378, 4);
  blocked_drop.target_object.object_kind = "domain";
  RequireDiagnostic(catalog_api::EngineCatalogDropObject(blocked_drop),
                    catalog_api::kCatalogObjectDiagnosticDependencyBlockedDrop,
                    "dependency-blocked drop was accepted");
  Rollback(blocked_context);

  auto rename_context = Begin(path, database_uuid, scratchbird::tests::FixtureUuid(1378, 8));
  auto rename = catalog_api::EngineCatalogRenameObjectRequest{};
  rename.context = rename_context;
  rename.target_object.uuid = scratchbird::tests::FixtureUuid(1378, 14);
  rename.target_object.object_kind = "table";
  rename.localized_names.push_back(Name("accounts"));
  const auto renamed = catalog_api::EngineCatalogRenameObject(rename);
  RequireOk(renamed, "table rename failed");
  Require(renamed.metadata_cache_epoch > created_table.metadata_cache_epoch,
          "rename did not advance metadata cache epoch");
  Commit(rename_context);

  auto old_name_context = Begin(path, database_uuid, scratchbird::tests::FixtureUuid(1378, 8));
  RequireDiagnostic(catalog_api::EngineCatalogResolveObjectName(
                        ResolveRequest(old_name_context, "table", scratchbird::tests::FixtureUuid(1378, 9), "customers")),
                    catalog_api::kCatalogObjectDiagnosticNameNotFound,
                    "old table name still resolved after rename");
  Rollback(old_name_context);

  auto resolve_context = Begin(path, database_uuid, scratchbird::tests::FixtureUuid(1378, 8));
  const auto resolved = catalog_api::EngineCatalogResolveObjectName(
      ResolveRequest(resolve_context, "table", scratchbird::tests::FixtureUuid(1378, 9), "accounts"));
  RequireOk(resolved, "renamed table did not resolve");
  Require(resolved.bound_object_identity.object_uuid == scratchbird::tests::FixtureUuid(1378, 14),
          "resolver did not return the UUID identity");
  Rollback(resolve_context);

  auto alter_context = Begin(path, database_uuid, scratchbird::tests::FixtureUuid(1378, 8));
  catalog_api::EngineCatalogAlterObjectRequest alter;
  alter.context = alter_context;
  alter.target_object.uuid = scratchbird::tests::FixtureUuid(1378, 14);
  alter.target_object.object_kind = "table";
  alter.option_envelopes.push_back("payload:shape=altered");
  const auto altered = catalog_api::EngineCatalogAlterObject(alter);
  RequireOk(altered, "table alter failed");
  Commit(alter_context);

  auto load_context = Begin(path, database_uuid, scratchbird::tests::FixtureUuid(1378, 8));
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
    if (object.object_uuid == scratchbird::tests::FixtureUuid(1378, 14)) {
      saw_altered_table = true;
      Require(object.definition_epoch == 3, "create/rename/alter definition epoch mismatch");
      Require(object.payload == "shape=altered", "alter payload was not persisted");
    }
  }
  for (const auto& column : loaded.state.columns) {
    if (column.column_uuid == scratchbird::tests::FixtureUuid(1378, 1) &&
        column.owner_object_uuid == scratchbird::tests::FixtureUuid(1378, 14) &&
        column.canonical_type_name == "text" &&
        !column.nullable) {
      saw_column_descriptor = true;
    }
  }
  for (const auto& constraint : loaded.state.constraints) {
    if (constraint.constraint_uuid == scratchbird::tests::FixtureUuid(1378, 2) &&
        constraint.constraint_class == "primary_key" &&
        constraint.owner_object_uuid == scratchbird::tests::FixtureUuid(1378, 14) &&
        constraint.constraint_hash == "customers_pk_hash") {
      saw_constraint_descriptor = true;
    }
  }
  for (const auto& key : loaded.state.key_descriptors) {
    if (key.key_descriptor_uuid == scratchbird::tests::FixtureUuid(1378, 6) &&
        key.constraint_uuid == scratchbird::tests::FixtureUuid(1378, 2)) {
      saw_key_descriptor = true;
    }
  }
  for (const auto& subject : loaded.state.constraint_subjects) {
    if (subject.subject_uuid == scratchbird::tests::FixtureUuid(1378, 10) &&
        subject.constraint_uuid == scratchbird::tests::FixtureUuid(1378, 2) &&
        subject.subject_object_uuid == scratchbird::tests::FixtureUuid(1378, 1)) {
      saw_constraint_subject = true;
    }
  }
  for (const auto& dependency : loaded.state.constraint_dependencies) {
    if (dependency.dependency_uuid == scratchbird::tests::FixtureUuid(1378, 3) &&
        dependency.constraint_uuid == scratchbird::tests::FixtureUuid(1378, 2) &&
        dependency.dependency_object_uuid == scratchbird::tests::FixtureUuid(1378, 4)) {
      saw_constraint_dependency = true;
    }
  }
  for (const auto& support : loaded.state.constraint_support_structures) {
    if (support.support_binding_uuid == scratchbird::tests::FixtureUuid(1378, 11) &&
        support.constraint_uuid == scratchbird::tests::FixtureUuid(1378, 2) &&
        support.support_uuid == scratchbird::tests::FixtureUuid(1378, 5)) {
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

  auto drop_table_context = Begin(path, database_uuid, scratchbird::tests::FixtureUuid(1378, 8));
  catalog_api::EngineCatalogDropObjectRequest drop_table;
  drop_table.context = drop_table_context;
  drop_table.target_object.uuid = scratchbird::tests::FixtureUuid(1378, 14);
  drop_table.target_object.object_kind = "table";
  RequireOk(catalog_api::EngineCatalogDropObject(drop_table), "table drop failed");
  Commit(drop_table_context);

  auto drop_domain_context = Begin(path, database_uuid, scratchbird::tests::FixtureUuid(1378, 8));
  catalog_api::EngineCatalogDropObjectRequest drop_domain;
  drop_domain.context = drop_domain_context;
  drop_domain.target_object.uuid = scratchbird::tests::FixtureUuid(1378, 4);
  drop_domain.target_object.object_kind = "domain";
  RequireOk(catalog_api::EngineCatalogDropObject(drop_domain),
            "domain drop after dependent table removal failed");
  Commit(drop_domain_context);
}

void TestMissingUuidOwnershipCacheAndMga(const std::filesystem::path& path,
                                         const catalog_api::EngineUuid& database_uuid) {
  auto missing_context = Begin(path, database_uuid, scratchbird::tests::FixtureUuid(1378, 8));
  auto missing_uuid = CreateRequest(missing_context, {}, "table", scratchbird::tests::FixtureUuid(1378, 9), "missing_uuid");
  RequireDiagnostic(catalog_api::EngineCatalogCreateObject(missing_uuid),
                    catalog_api::kCatalogObjectDiagnosticUuidRequired,
                    "create without UUID was accepted");
  Rollback(missing_context);

  auto denied_context = Begin(path, database_uuid, scratchbird::tests::FixtureUuid(1378, 7));
  auto denied = CreateRequest(denied_context, scratchbird::tests::FixtureUuid(1378, 16), "table", scratchbird::tests::FixtureUuid(1378, 9), "denied");
  RequireDiagnostic(catalog_api::EngineCatalogCreateObject(denied),
                    catalog_api::kCatalogObjectDiagnosticSchemaOwnerDenied,
                    "schema ownership denial did not fire");
  Rollback(denied_context);

  auto create_context = Begin(path, database_uuid, scratchbird::tests::FixtureUuid(1378, 8));
  auto visible_object = CreateRequest(create_context, scratchbird::tests::FixtureUuid(1378, 17), "table", scratchbird::tests::FixtureUuid(1378, 9), "epoch_probe");
  const auto created = catalog_api::EngineCatalogCreateObject(visible_object);
  RequireOk(created, "epoch probe create failed");
  const auto created_tx = create_context.local_transaction_id;
  // A real later committed transaction makes the issued observer bound cover
  // the still-active creator. Only that creator's outcome changes below; no
  // caller-manufactured snapshot bound or inventory status is used.
  auto barrier = Begin(path, database_uuid, scratchbird::tests::FixtureUuid(1378, 8));
  Commit(barrier);
  auto outcome_observer = Begin(path, database_uuid, scratchbird::tests::FixtureUuid(1378, 8));
  Require(outcome_observer.snapshot_visible_through_local_transaction_id >= created_tx,
          "issued observer boundary does not cover creator");
  const auto read_catalog_bytes = [&] {
    std::ifstream in(path.string() + ".sb.catalog_object_events", std::ios::binary);
    Require(in.is_open(), "catalog outcome fixture journal unavailable");
    std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    Require(!in.bad(), "catalog outcome fixture journal read failed");
    return bytes;
  };
  const auto catalog_before_commit = read_catalog_bytes();
  for (int repeat = 0; repeat != 2; ++repeat) {
    RequireDiagnostic(catalog_api::EngineCatalogLookupObjectByUuid(
        LookupRequest(outcome_observer, scratchbird::tests::FixtureUuid(1378, 17), "table")),
        catalog_api::kCatalogObjectDiagnosticMgaVisibilityRefused,
        "active sibling creator leaked through catalog cache");
  }
  Commit(create_context);
  Require(read_catalog_bytes() == catalog_before_commit,
          "creator commit unexpectedly changed the catalog journal");
  for (int repeat = 0; repeat != 2; ++repeat) {
    RequireOk(catalog_api::EngineCatalogLookupObjectByUuid(
        LookupRequest(outcome_observer, scratchbird::tests::FixtureUuid(1378, 17), "table")),
        "cached catalog hid creator after its actual commit");
  }
  Rollback(outcome_observer);

  auto stale_cache_context = Begin(path, database_uuid, scratchbird::tests::FixtureUuid(1378, 8));
  catalog_api::EngineCatalogValidateMetadataCacheRequest stale_cache;
  stale_cache.context = stale_cache_context;
  stale_cache.bound_object_identity.catalog_generation_id = created.metadata_cache_epoch - 1;
  RequireDiagnostic(catalog_api::EngineCatalogValidateMetadataCache(stale_cache),
                    catalog_api::kCatalogObjectDiagnosticCacheEpochStale,
                    "stale metadata cache epoch was accepted");
  Rollback(stale_cache_context);

  auto current_cache_context = Begin(path, database_uuid, scratchbird::tests::FixtureUuid(1378, 8));
  catalog_api::EngineCatalogValidateMetadataCacheRequest current_cache;
  current_cache.context = current_cache_context;
  current_cache.bound_object_identity.catalog_generation_id = created.metadata_cache_epoch;
  RequireOk(catalog_api::EngineCatalogValidateMetadataCache(current_cache),
            "current metadata cache epoch was rejected");
  Rollback(current_cache_context);

  auto hidden_context = Begin(path, database_uuid, scratchbird::tests::FixtureUuid(1378, 8));
  hidden_context.snapshot_visible_through_local_transaction_id = created_tx - 1;
  const auto hidden = catalog_api::EngineCatalogLookupObjectByUuid(
      LookupRequest(hidden_context, scratchbird::tests::FixtureUuid(1378, 17), "table"));
  RequireDiagnostic(hidden,
                    catalog_api::kCatalogObjectDiagnosticMgaVisibilityRefused,
                    "snapshot-stale MGA visibility lookup did not fail closed");
  Rollback(hidden_context);

  auto visible_context = Begin(path, database_uuid, scratchbird::tests::FixtureUuid(1378, 8));
  visible_context.snapshot_visible_through_local_transaction_id = created_tx;
  const auto visible = catalog_api::EngineCatalogLookupObjectByUuid(
      LookupRequest(visible_context, scratchbird::tests::FixtureUuid(1378, 17), "table"));
  RequireOk(visible, "MGA-visible object lookup failed");
  // Exercise the low-level catalog projection with the same observer and
  // unchanged journals. A cached wider snapshot must not supply visibility
  // to a narrower request, nor may the narrower cached result hide the row
  // when the original bound is read again.
  auto same_observer_hidden = visible_context;
  same_observer_hidden.snapshot_visible_through_local_transaction_id = created_tx - 1;
  for (int repeat = 0; repeat != 2; ++repeat) {
    RequireDiagnostic(catalog_api::EngineCatalogLookupObjectByUuid(
        LookupRequest(same_observer_hidden, scratchbird::tests::FixtureUuid(1378, 17), "table")),
        catalog_api::kCatalogObjectDiagnosticMgaVisibilityRefused,
        "cached catalog visibility crossed the requested snapshot boundary");
    RequireOk(catalog_api::EngineCatalogLookupObjectByUuid(
        LookupRequest(visible_context, scratchbird::tests::FixtureUuid(1378, 17), "table")),
        "narrow cached catalog snapshot hid an eligible object");
  }
  Rollback(visible_context);
}

void TestDdlSynonymCreateDropRoute(const std::filesystem::path& path,
                                   const catalog_api::EngineUuid& database_uuid) {
  auto create_context = Begin(path, database_uuid, scratchbird::tests::FixtureUuid(1378, 8));
  catalog_api::EngineCreateSynonymRequest create;
  create.context = create_context;
  create.target_object.uuid = scratchbird::tests::FixtureUuid(1378, 12);
  create.target_schema.uuid = scratchbird::tests::FixtureUuid(1378, 9);
  create.localized_names.push_back(Name("epoch_alias"));
  create.related_objects.push_back({scratchbird::tests::FixtureUuid(1378, 17), "table"});
  const auto created = catalog_api::EngineCreateSynonym(create);
  RequireOk(created, "DDL synonym create route failed");
  Require(created.bound_object_identity.object_uuid == scratchbird::tests::FixtureUuid(1378, 12),
          "DDL synonym create did not preserve synonym UUID authority");
  Require(HasEvidence(created, "ddl_catalog_route", "sys.catalog.synonym"),
          "DDL synonym create did not route through sys.catalog.synonym");
  Commit(create_context);

  auto resolve_context = Begin(path, database_uuid, scratchbird::tests::FixtureUuid(1378, 8));
  const auto resolved = catalog_api::EngineCatalogResolveObjectName(
      ResolveRequest(resolve_context, "table", scratchbird::tests::FixtureUuid(1378, 9), "epoch_alias"));
  RequireOk(resolved, "DDL-created synonym did not resolve");
  Require(resolved.bound_object_identity.object_uuid == scratchbird::tests::FixtureUuid(1378, 17),
          "DDL-created synonym did not dereference to target table UUID");
  Require(HasEvidence(resolved, "synonym_chain", scratchbird::tests::FixtureUuid(1378, 12)),
          "DDL-created synonym did not retain synonym-chain evidence");
  Rollback(resolve_context);

  auto duplicate_context = Begin(path, database_uuid, scratchbird::tests::FixtureUuid(1378, 8));
  catalog_api::EngineCreateSynonymRequest duplicate = create;
  duplicate.context = duplicate_context;
  duplicate.target_object.uuid = scratchbird::tests::FixtureUuid(1378, 13);
  RequireDiagnostic(catalog_api::EngineCreateSynonym(duplicate),
                    catalog_api::kCatalogSynonymDiagnosticNameConflict,
                    "DDL synonym duplicate name did not use exact catalog diagnostic");
  Rollback(duplicate_context);

  auto drop_context = Begin(path, database_uuid, scratchbird::tests::FixtureUuid(1378, 8));
  catalog_api::EngineDropObjectRequest drop_synonym;
  drop_synonym.context = drop_context;
  drop_synonym.target_object.uuid = scratchbird::tests::FixtureUuid(1378, 12);
  drop_synonym.target_object.object_kind = "synonym";
  const auto dropped = catalog_api::EngineDropObject(drop_synonym);
  RequireOk(dropped, "DDL synonym drop route failed");
  Require(HasEvidence(dropped, "ddl_catalog_route", "sys.catalog.synonym"),
          "DDL synonym drop did not route through sys.catalog.synonym");
  Commit(drop_context);

  auto missing_context = Begin(path, database_uuid, scratchbird::tests::FixtureUuid(1378, 8));
  RequireDiagnostic(catalog_api::EngineCatalogResolveObjectName(
                        ResolveRequest(missing_context, "table", scratchbird::tests::FixtureUuid(1378, 9), "epoch_alias")),
                    catalog_api::kCatalogObjectDiagnosticNameNotFound,
                    "dropped DDL synonym still resolved by name");
  Rollback(missing_context);

  auto target_drop_context = Begin(path, database_uuid, scratchbird::tests::FixtureUuid(1378, 8));
  catalog_api::EngineCatalogDropObjectRequest drop_target;
  drop_target.context = target_drop_context;
  drop_target.target_object.uuid = scratchbird::tests::FixtureUuid(1378, 17);
  drop_target.target_object.object_kind = "table";
  RequireOk(catalog_api::EngineCatalogDropObject(drop_target),
            "synonym drop did not retire dependency blocking target drop");
  Commit(target_drop_context);
}

// Storage replay fixture, not permission for the enclosing CREATE producer.
// Records use the real binary codecs and engine-reserved metadata sequences;
// transaction outcomes and rollback markers are issued by their real owners.
void TestSequencedCatalogReplay(const std::filesystem::path& path) {
  const auto database = CreateDatabase(path);
  auto context = Begin(path, database, scratchbird::tests::FixtureUuid(1378, 8));
  const auto id = [](unsigned n) { return scratchbird::tests::FixtureUuid(1390, n); };
  const auto append_bytes = [&](const char* suffix, const std::string& bytes) {
    std::ofstream out(path.string() + suffix, std::ios::binary | std::ios::app);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    out.close();
    Require(static_cast<bool>(out), "sequenced replay fixture append failed");
  };
  const auto reserve = [&] {
    const auto reservation = catalog_api::ReserveEventSequenceRange(context,
        "relation_metadata", path.string() + ".sb.mga_relation_metadata", 1,
        [&] { return catalog_api::ScanNextMetadataEventSequence(context); });
    Require(reservation.ok, "sequenced replay fixture reservation failed");
    return reservation.first;
  };
  const auto append = [&](const catalog_api::CatalogLifecycleRecord& record, bool legacy = false) {
    std::string bytes;
    const auto sequence = legacy ? 0 : reserve();
    Require(std::visit([&](const auto& value) {
      return catalog_api::EncodeCatalogLifecycleRecord(value, &bytes, sequence);
    }, record), "sequenced catalog fixture encoding failed");
    append_bytes(".sb.catalog_object_events", bytes);
  };
  catalog_api::EngineCatalogObjectRecord object;
  object.creator_tx = context.local_transaction_id;
  object.object_uuid = id(1); object.catalog_row_uuid = id(2);
  object.object_kind = "table"; object.metadata_epoch = 10;
  append(object, true); // Legacy remains readable but has no shared sequence.
  catalog_api::EngineCatalogNameRecord name;
  name.creator_tx = context.local_transaction_id;
  name.name_entry_uuid = id(3); name.object_uuid = id(1); name.object_kind = "table";
  name.raw_name_text = name.display_name = name.exact_lookup_key = "retained";
  name.normalized_lookup_key = "RETAINED"; name.metadata_epoch = 10;
  catalog_api::EngineCatalogDependencyRecord dependency;
  dependency.source_uuid = dependency.dependency_uuid = id(1);
  catalog_api::EngineCatalogColumnMetadataRecord column;
  column.column_uuid = id(4); column.owner_object_uuid = id(1);
  column.descriptor_kind = "scalar"; column.canonical_type_name = "int64";
  catalog_api::EngineCatalogConstraintDescriptorRecord constraint;
  constraint.constraint_uuid = id(5); constraint.owner_object_uuid = id(1);
  catalog_api::EngineCatalogKeyDescriptorRecord key;
  key.key_descriptor_uuid = id(6); key.constraint_uuid = id(5); key.owner_object_uuid = id(1);
  catalog_api::EngineCatalogConstraintSubjectRecord subject;
  subject.subject_uuid = id(7); subject.constraint_uuid = id(5); subject.subject_object_uuid = id(1);
  catalog_api::EngineCatalogConstraintDependencyRecord constraint_dependency;
  constraint_dependency.dependency_uuid = id(8); constraint_dependency.constraint_uuid = id(5);
  constraint_dependency.dependency_object_uuid = id(1);
  catalog_api::EngineCatalogConstraintSupportStructureRecord support;
  support.support_binding_uuid = id(9); support.constraint_uuid = id(5); support.support_uuid = id(10);
  std::vector<catalog_api::CatalogLifecycleRecord> records{
      name, dependency, column, constraint, key, subject, constraint_dependency, support};
  for (auto& record : records) {
    std::visit([&](auto& value) { value.creator_tx = context.local_transaction_id; value.metadata_epoch = 10; }, record);
    append(record);
  }
  const auto append_resolver = [&](bool retire, bool extra = false) {
    catalog_api::ApiBehaviorRecord frame;
    frame.creator_tx = context.local_transaction_id; frame.event_sequence = reserve();
    frame.operation_id = "catalog.replay.fixture"; frame.state = retire ? "retired" : "active";
    frame.object_kind = retire ? "name_registry.retire" : "name_registry.entry";
    frame.object_uuid = retire ? id(1) : id(extra ? 12 : 11); frame.target_object_uuid = id(1);
    if (!retire) {
      catalog_api::NameRegistryEntry entry;
      entry.creator_tx = context.local_transaction_id; entry.name_entry_uuid = frame.object_uuid;
      entry.object_uuid = id(1); entry.object_class = "table";
      entry.raw_name_text = entry.display_name = entry.exact_lookup_key = extra ? "extra" : "retained";
      entry.normalized_lookup_key = extra ? "EXTRA" : "RETAINED";
      Require(catalog_api::EncodeNameRegistryEntry(entry, &frame.payload), "resolver fixture encoding failed");
    }
    std::string bytes;
    Require(catalog_api::EncodeApiBehaviorRecord(frame, &bytes), "resolver frame encoding failed");
    append_bytes(".sb.name_events.v2", bytes);
  };
  append_resolver(false);
  const auto check = [&](const catalog_api::EngineRequestContext& reader, std::uint64_t epoch) {
    const auto loaded = catalog_api::LoadCatalogObjectLifecycleState(reader);
    Require(loaded.ok, "sequenced catalog replay refused");
    const auto epochs = catalog_api::LoadCatalogObjectLifecycleEpochState(reader);
    Require(epochs.ok && epochs.state.metadata_epoch == epoch && epochs.state.name_resolution_epoch == 10,
            "epoch-only catalog replay diverged after metadata rollback");
    const auto& s = loaded.state;
    Require(s.objects.size() == 1 && s.names.size() == 1 && s.dependencies.size() == 1 &&
                s.columns.size() == 1 && s.constraints.size() == 1 && s.key_descriptors.size() == 1 &&
                s.constraint_subjects.size() == 1 && s.constraint_dependencies.size() == 1 &&
                s.constraint_support_structures.size() == 1 && s.metadata_epoch == epoch &&
                s.name_resolution_epoch == 10 &&
                (epoch != 10 || s.objects.front().event_sequence == 0) &&
                s.names.front().event_sequence != 0,
            "catalog rollback did not restore every record family and epoch");
    const auto resolved = catalog_api::LoadNameRegistryState(reader, reader.local_transaction_id);
    if (!resolved.ok) std::cerr << resolved.diagnostic.code << ':' << resolved.diagnostic.detail << '\n';
    Require(resolved.ok && std::count_if(resolved.state.entries.begin(), resolved.state.entries.end(),
                [&](const auto& entry) { return entry.object_uuid == id(1); }) == 1,
            "resolver retirement survived metadata rollback");
  };
  check(context, 10);
  Require(!catalog_api::CreateMgaSavepointMarker(context, "catalog_outer").error, "catalog marker create failed");
  for (int repeat = 0; repeat != 2; ++repeat) {
    Require(!catalog_api::CreateMgaSavepointMarker(context, "catalog_inner").error, "nested catalog marker failed");
    auto deleted = object; deleted.deleted = true; deleted.metadata_epoch = 90;
    append(deleted);
    for (auto record : records) {
      std::visit([](auto& value) {
        if constexpr (requires { value.deleted; }) value.deleted = true;
        value.metadata_epoch = 90;
      }, record);
      append(record);
    }
    append(catalog_api::EngineCatalogRetireNamesRecord{context.local_transaction_id, id(1), 90});
    append(catalog_api::EngineCatalogCacheInvalidationRecord{context.local_transaction_id, id(1), "retire", 90, 90, 90});
    append_resolver(false, true);
    append_resolver(true);
    const auto deleted_state = catalog_api::LoadCatalogObjectLifecycleState(context);
    Require(deleted_state.ok && deleted_state.state.objects.empty(), "catalog successor was not applied");
    Require(!catalog_api::RollbackToMgaSavepointMarker(context, "catalog_outer").error, "catalog rollback failed");
    Require(catalog_api::ValidateMgaSavepointExists(context, "catalog_inner", "fixture").error,
            "outer catalog rollback retained nested marker");
    check(context, 10); check(context, 10); // Warm caches must honor new exclusions.
  }
  const auto marker_path = path.string() + ".sb.mga_savepoints";
  std::ifstream markers(marker_path, std::ios::binary);
  const std::string marker_bytes((std::istreambuf_iterator<char>(markers)), std::istreambuf_iterator<char>());
  Require(!markers.bad() && !marker_bytes.empty(), "marker fault fixture read failed");
  markers.close();
  const auto marker_time = std::filesystem::last_write_time(marker_path);
  const auto replace_markers = [&](const std::string& bytes) {
    std::ofstream out(marker_path, std::ios::binary | std::ios::trunc);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size())); out.close();
    Require(static_cast<bool>(out), "marker fault fixture replacement failed");
    std::filesystem::last_write_time(marker_path, marker_time);
  };
  auto corrupt = marker_bytes; corrupt.back() ^= 1; replace_markers(corrupt);
  const auto bad_catalog = catalog_api::LoadCatalogObjectLifecycleState(context);
  const auto bad_epoch = catalog_api::LoadCatalogObjectLifecycleEpochState(context);
  const auto bad_names = catalog_api::LoadNameRegistryState(context, context.local_transaction_id);
  Require(!bad_catalog.ok && bad_catalog.state.objects.empty() && bad_catalog.state.metadata_epoch == 0 &&
              !bad_epoch.ok && bad_epoch.state.metadata_epoch == 0 &&
              !bad_names.ok && bad_names.state.entries.empty(),
          "warm catalog/name cache bypassed corrupt rollback authority");
  replace_markers(marker_bytes); check(context, 10);
  Require(!catalog_api::ReleaseMgaSavepointMarker(context, "catalog_outer").error, "catalog release failed");
  object.metadata_epoch = 11; append(object);
  check(context, 11);
  Commit(context);
  auto reader = Begin(path, database, scratchbird::tests::FixtureUuid(1378, 8));
  check(reader, 11); // Committed-inventory fallback must not resurrect undone names.
  Rollback(reader);
}

}  // namespace

int main() try {
  // Exercise the same assertion-failure path with a real database, not just
  // an empty-directory helper test. No device handle escapes CreateDatabase.
  std::filesystem::path failed_directory;
  try {
    scratchbird::tests::OwnedTempDirectory failed_artifacts;
    failed_directory=failed_artifacts.path();
    (void)CreateDatabase(failed_directory / "failure.sbdb");
    Fail("intentional catalog fixture unwind");
  } catch (const std::runtime_error& error) {
    Require(std::string_view(error.what())=="intentional catalog fixture unwind",
            "unexpected catalog fixture setup failure");
  }
  Require(!failed_directory.empty() && !std::filesystem::exists(failed_directory),
          "assertion failure left catalog fixture artifacts");
  scratchbird::tests::OwnedTempDirectory artifacts;
  const auto path = artifacts.path() / "catalog.sbdb";
  const auto database_uuid = CreateDatabase(path);
  TestCreateAlterRenameDropAndDependencies(path, database_uuid);
  TestMissingUuidOwnershipCacheAndMga(path, database_uuid);
  TestDdlSynonymCreateDropRoute(path, database_uuid);
  TestSequencedCatalogReplay(artifacts.path() / "sequenced.sbdb");

  artifacts.Cleanup();
  return EXIT_SUCCESS;
} catch (const std::exception& error) {
  std::cerr << error.what() << '\n';
  return EXIT_FAILURE;
}

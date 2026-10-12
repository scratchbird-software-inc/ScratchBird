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
#include "catalog/catalog_journal_publication.hpp"
#include "catalog/name_registry_codec.hpp"
#include "mga_relation_store/mga_event_sequence_allocator.hpp"
#include "mga_relation_store/mga_savepoint_store.hpp"
#include "mga_relation_store/mga_relation_store_internal_support.hpp"
#include "crud_support/crud_store.hpp"
#include "catalog/constraint_metadata_codec.hpp"
#include "../support/binary_uuid_fixture.hpp"
#include "../support/owned_temp_directory.hpp"
#include "database_lifecycle.hpp"
#include "ddl/create_api.hpp"
#include "ddl/drop_api.hpp"
#include "transaction/transaction_api.hpp"
#include "uuid.hpp"
#include "memory.hpp"

#include <cstdlib>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <set>
#include <new>
#include <future>
#include <chrono>

namespace catalog_allocation_fault {
thread_local bool denied = false;
thread_local unsigned failures = 0;
}
void* operator new(std::size_t size) {
  if (catalog_allocation_fault::denied) {
    ++catalog_allocation_fault::failures;
    throw std::bad_alloc();
  }
  if (auto* memory = std::malloc(size == 0 ? 1 : size)) return memory;
  throw std::bad_alloc();
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete[](void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }
void operator delete[](void* memory, std::size_t) noexcept { std::free(memory); }

#if defined(SB_CATALOG_TEST_WRAP_SYNC)
#include <cerrno>
#include <sys/stat.h>
#include <sys/resource.h>
#include <csignal>
#include <sys/wait.h>
#include <unistd.h>
namespace catalog_publication_fault {
thread_local bool armed = false, hit = false;
thread_local unsigned skip_matches = 0;
thread_local struct stat target {};
struct Pause {
  struct stat target {};
  std::promise<void> reached;
  std::shared_future<void> release;
  bool hit = false;
};
thread_local Pause* pause = nullptr;
struct AfterSync {
  struct stat trigger {}, failure {};
  unsigned skip = 0;
  bool hit = false;
  bool deny_allocations = false;
};
thread_local AfterSync* after_sync = nullptr;
struct Crash {
  struct stat trigger {}, parent {};
  unsigned skip = 0;
  bool next_parent = false;
  int exit_code = 0;
};
thread_local Crash* crash = nullptr;
}
extern "C" int __real_fsync(int);
extern "C" int __wrap_fsync(int fd) {
  struct stat actual {};
  auto* pause = catalog_publication_fault::pause;
  if (pause && !pause->hit && ::fstat(fd, &actual) == 0 &&
      actual.st_dev == pause->target.st_dev && actual.st_ino == pause->target.st_ino) {
    pause->hit = true;
    pause->reached.set_value();
    pause->release.wait();
  }
  if (catalog_publication_fault::armed && ::fstat(fd, &actual) == 0 &&
      actual.st_dev == catalog_publication_fault::target.st_dev &&
      actual.st_ino == catalog_publication_fault::target.st_ino) {
    if (catalog_publication_fault::skip_matches != 0) --catalog_publication_fault::skip_matches;
    else {
      catalog_publication_fault::armed = false; catalog_publication_fault::hit = true;
      errno = EIO; return -1;
    }
  }
  const int result = __real_fsync(fd);
  auto* crash = catalog_publication_fault::crash;
  if (result == 0 && crash && ::fstat(fd, &actual) == 0 &&
      actual.st_dev == crash->trigger.st_dev && actual.st_ino == crash->trigger.st_ino) {
    if (crash->skip != 0) --crash->skip;
    else if (crash->next_parent) { crash->trigger = crash->parent; crash->next_parent = false; }
    else std::_Exit(crash->exit_code);
  }
  auto* after = catalog_publication_fault::after_sync;
  if (result == 0 && after && !after->hit && ::fstat(fd, &actual) == 0 &&
      actual.st_dev == after->trigger.st_dev && actual.st_ino == after->trigger.st_ino) {
    if (after->skip != 0) --after->skip;
    else {
      after->hit = true;
      if (after->deny_allocations) catalog_allocation_fault::denied = true;
      else {
        catalog_publication_fault::target = after->failure;
        catalog_publication_fault::skip_matches = 0;
        catalog_publication_fault::armed = true;
      }
    }
  }
  return result;
}
#endif

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

void RequireSequencedPublication(const std::filesystem::path& path) {
  std::set<std::uint64_t> sequences;
  for (const char* suffix : {".sb.catalog_object_events", ".sb.name_events.v2"}) {
    std::ifstream in(path.string() + suffix, std::ios::binary);
    Require(in.is_open(), "production catalog journal absent");
    unsigned count = 0;
    while (in.peek() != std::char_traits<char>::eof()) {
      catalog_api::ApiBehaviorRecord record;
      Require(catalog_api::ReadApiBehaviorRecord(in, &record) && record.event_sequence != 0 &&
                  sequences.insert(record.event_sequence).second,
              "production catalog/name record lacks a unique shared sequence");
      ++count;
    }
    Require(!in.bad() && count != 0, "production catalog/name journal read failed");
  }
}

void TestCatalogPublicationFailures(const std::filesystem::path& path) {
  const auto database = CreateDatabase(path);
  auto context = Begin(path, database, scratchbird::tests::FixtureUuid(1378, 8));
  catalog_api::EngineCatalogObjectRecord object;
  object.creator_tx = context.local_transaction_id;
  object.object_uuid = scratchbird::tests::FixtureUuid(1391, 1);
  object.catalog_row_uuid = scratchbird::tests::FixtureUuid(1391, 2);
  object.object_kind = "table"; object.metadata_epoch = 1;
  std::string bytes;
  Require(catalog_api::EncodeCatalogLifecycleRecord(object, &bytes), "publication fixture encoding failed");
  catalog_api::ApiBehaviorRecord frame;
  Require(catalog_api::DecodeApiBehaviorRecord(
      {reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size()}, &frame), "publication frame decode failed");
  const auto read = [](const std::string& file) {
    std::ifstream in(file, std::ios::binary);
    Require(in.is_open(), "publication evidence read failed");
    std::string result((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    Require(!in.bad(), "publication evidence read failed");
    return result;
  };
  const auto allocator = path.string() + ".sb.mga_event_sequence_allocator";
  // Use the exact producer guard and real frame encoder with sustained
  // allocation denial. Cleanup must neither allocate nor terminate while the
  // encoder is unwinding; only a never-attempted tail may be reused.
  {
    const auto next = catalog_api::NextMetadataEventSequence(context);
    std::vector<std::string> deferred;
    const auto reserve = [&] {
      return catalog_api::ReserveEventSequenceRange(context, "relation_metadata",
          context.database_path + ".sb.mga_relation_metadata", 1,
          [&] { return catalog_api::ScanNextMetadataEventSequence(context); }, &deferred);
    };
    const auto reservation = reserve();
    Require(reservation.ok && reservation.first == next, "allocation fault reservation failed");
    auto candidate = frame; candidate.event_sequence = reservation.first;
    unsigned caught = 0;
    struct Denial {
      Denial() { catalog_allocation_fault::failures = 0; catalog_allocation_fault::denied = true; }
      ~Denial() { catalog_allocation_fault::denied = false; }
    };
    {
      Denial denial;
      try {
        catalog_api::catalog_publication_detail::UnpublishedReservation unpublished{reservation};
        std::string encoded;
        (void)catalog_api::EncodeApiBehaviorRecord(candidate, &encoded);
      } catch (const std::bad_alloc&) { ++caught; }
      // Denial is still active, not a fail-once injector.
      try { std::string encoded; (void)catalog_api::EncodeApiBehaviorRecord(candidate, &encoded); }
      catch (const std::bad_alloc&) { ++caught; }
    }
    Require(caught == 2 && catalog_allocation_fault::failures == 2 &&
                catalog_api::NextMetadataEventSequence(context) == next &&
                !std::filesystem::exists(allocator),
            "allocation failure cleanup allocated, lost its tail, or published effects");
    deferred.clear();
    const auto first = reserve(), second = reserve();
    Require(first.ok && second.ok && first.first == next && second.first == next + 1,
            "post-allocation-failure reservations invalid");
    { Denial denial; catalog_api::AbandonDeferredEventSequenceReservation(first); }
    Require(catalog_api::NextMetadataEventSequence(context) == next + 2,
            "non-tail cleanup rewound a later reservation");
    { Denial denial;
      catalog_api::AbandonDeferredEventSequenceReservation(second);
      catalog_api::AbandonDeferredEventSequenceReservation(first);
    }
    Require(catalog_allocation_fault::failures == 0 &&
                catalog_api::NextMetadataEventSequence(context) == next,
            "sustained-denial tail abandonment failed");
  }
  for (const auto kind : {catalog_api::CatalogJournalKind::lifecycle,
                          catalog_api::CatalogJournalKind::resolver_names}) {
    auto record = frame;
    if (kind == catalog_api::CatalogJournalKind::resolver_names) {
      record.object_kind = "name_registry.retire";
      record.target_object_uuid = record.object_uuid;
      record.payload.clear(); record.state = "retired";
    }
    const auto journal = path.string() + (kind == catalog_api::CatalogJournalKind::lifecycle
        ? ".sb.catalog_object_events" : ".sb.name_events.v2");
    const auto publish = [&] { return catalog_api::AppendSequencedCatalogJournalRecord(context, record, kind); };
    Require(!publish().error, "initial real catalog publication failed");
    const auto before_invalid = read(journal), allocator_before_invalid = read(allocator);
    const auto next_before_invalid = catalog_api::NextMetadataEventSequence(context);
    auto invalid = record; invalid.object_uuid = {};
    Require(catalog_api::AppendSequencedCatalogJournalRecord(context, invalid, kind).error &&
                read(journal) == before_invalid && read(allocator) == allocator_before_invalid &&
                catalog_api::NextMetadataEventSequence(context) == next_before_invalid,
            "invalid encoding changed publication or consumed an unpublished tail");
    const auto refuses_without_effects = [&](const auto& owner, const auto& candidate, auto destination) {
      const auto journal_before = read(journal), ranges_before = read(allocator);
      const auto next_before = catalog_api::NextMetadataEventSequence(context);
      Require(catalog_api::AppendSequencedCatalogJournalRecord(owner, candidate, destination).error &&
                  read(journal) == journal_before && read(allocator) == ranges_before &&
                  catalog_api::NextMetadataEventSequence(context) == next_before,
              "invalid publication authority changed durable artifacts or reservation state");
    };
    auto foreign = context;
    foreign.transaction_uuid = scratchbird::tests::FixtureUuid(1391, 100);
    refuses_without_effects(foreign, record, kind);
    foreign = context; foreign.database_uuid = scratchbird::tests::FixtureUuid(1391, 101);
    refuses_without_effects(foreign, record, kind);
    foreign = context; foreign.local_transaction_id = 0;
    auto malformed = record; malformed.creator_tx = 0;
    refuses_without_effects(foreign, malformed, kind);
    malformed = record; malformed.event_sequence = 1;
    refuses_without_effects(context, malformed, kind);
    malformed = record; ++malformed.creator_tx;
    refuses_without_effects(context, malformed, kind);
    malformed = record;
    if (kind == catalog_api::CatalogJournalKind::lifecycle) malformed.payload.pop_back();
    else malformed.target_object_uuid = scratchbird::tests::FixtureUuid(1391, 102);
    refuses_without_effects(context, malformed, kind);
    refuses_without_effects(context, record, static_cast<catalog_api::CatalogJournalKind>(77));
#if defined(SB_CATALOG_TEST_WRAP_SYNC)
    for (unsigned fault = 0; fault != 4; ++fault) {
      const auto before = read(journal);
      const auto next = catalog_api::NextMetadataEventSequence(context);
      const std::string target = fault == 0 ? allocator : fault == 2 ? journal : path.parent_path().string();
      Require(::stat(target.c_str(), &catalog_publication_fault::target) == 0, "sync fault target unavailable");
      struct Fault {
        explicit Fault(unsigned skip) {
          catalog_publication_fault::hit = false; catalog_publication_fault::skip_matches = skip;
          catalog_publication_fault::armed = true;
        }
        ~Fault() { catalog_publication_fault::armed = false; }
      };
      catalog_api::EngineApiDiagnostic diagnostic;
      { Fault scope(fault == 3 ? 1 : 0); diagnostic = publish(); }
      const char* details[] = {"allocator_sync_failed:", "allocator_directory_sync_failed:",
                               "journal_sync_failed:", "journal_directory_sync_failed:"};
      Require(catalog_publication_fault::hit && diagnostic.error &&
                  diagnostic.detail.find(details[fault]) != std::string::npos,
              "catalog publication missed an actual sync failure");
      Require((fault < 2 ? read(journal) == before : read(journal).size() > before.size()),
              "catalog physical effects disagree with failure boundary");
      catalog_api::ClearMgaEventSequenceRangeCacheForTesting();
      Require(catalog_api::NextMetadataEventSequence(context) == next + 1,
              "cold allocator reused a possibly published reservation");
      Require(!publish().error, "real publication retry failed");
    }
    for (const bool allocator_failure : {true, false}) {
      const auto before = read(journal), allocator_before = read(allocator);
      const auto next = catalog_api::NextMetadataEventSequence(context);
      const auto target = allocator_failure ? allocator : journal;
      const auto mode = std::filesystem::status(target).permissions();
      struct Restore {
        std::string path; std::filesystem::perms mode;
        ~Restore() {
          std::error_code error; std::filesystem::permissions(path, mode, error);
          if (error) std::terminate();
        }
      };
      catalog_api::EngineApiDiagnostic diagnostic;
      {
        Restore restore{target, mode};
        std::filesystem::permissions(target, std::filesystem::perms::owner_read);
        Require(!std::ofstream(target, std::ios::app | std::ios::binary), "write fault needs unprivileged execution");
        diagnostic = publish();
      }
      Require(diagnostic.error && diagnostic.detail.find(allocator_failure ? "allocator_append_failed" : "journal_open_failed") != std::string::npos &&
                  read(journal) == before, "failed append falsely published catalog metadata");
      if (allocator_failure) Require(read(allocator) == allocator_before, "denied allocator write changed bytes");
      else {
        catalog_api::ClearMgaEventSequenceRangeCacheForTesting();
        Require(catalog_api::NextMetadataEventSequence(context) == next + 1,
                "denied record append lost its durable reservation");
      }
      Require(!publish().error, "real append retry failed");
    }
#endif
  }
#if defined(SB_CATALOG_TEST_WRAP_SYNC)
  // Suspend actual publication at allocator fsync while it owns the inventory
  // guard. Every marker writer must wait; its cutoff must include the completed
  // publication rather than observing half of it. No detached workers escape.
  for (const auto action : {catalog_api::CreateMgaSavepointMarker,
                            catalog_api::RollbackToMgaSavepointMarker,
                            catalog_api::ReleaseMgaSavepointMarker}) {
    if (action != catalog_api::CreateMgaSavepointMarker)
      Require(!catalog_api::CreateMgaSavepointMarker(context, "publication_serialized").error,
              "serialized marker setup failed");
    const auto next = catalog_api::NextMetadataEventSequence(context);
    catalog_publication_fault::Pause pause;
    Require(::stat(allocator.c_str(), &pause.target) == 0, "publication pause target unavailable");
    std::promise<void> release, starting;
    pause.release = release.get_future().share();
    auto reached = pause.reached.get_future(), started = starting.get_future();
    auto writer = std::async(std::launch::async, [&] {
      struct Hook {
        explicit Hook(catalog_publication_fault::Pause* value) { catalog_publication_fault::pause = value; }
        ~Hook() { catalog_publication_fault::pause = nullptr; }
      } hook(&pause);
      return catalog_api::AppendSequencedCatalogJournalRecord(context, frame, catalog_api::CatalogJournalKind::lifecycle);
    });
    std::future<catalog_api::EngineApiDiagnostic> marker;
    // Release before any throwing assertion or async-future destruction.
    struct Release {
      std::promise<void>& signal;
      bool sent = false;
      ~Release() { if (!sent) signal.set_value(); }
    } resume{release};
    const bool paused = reached.wait_for(std::chrono::seconds(5)) == std::future_status::ready;
    bool blocked = false;
    if (paused) {
      marker = std::async(std::launch::async, [&] {
        starting.set_value(); return action(context, "publication_serialized");
      });
      const bool running = started.wait_for(std::chrono::seconds(5)) == std::future_status::ready;
      blocked = running && marker.wait_for(std::chrono::milliseconds(100)) == std::future_status::timeout;
    }
    release.set_value(); resume.sent = true;
    const auto written = writer.get();
    const auto marked = marker.valid() ? marker.get() : catalog_api::EngineApiDiagnostic{};
    Require(paused && blocked && !written.error && !marked.error,
            "marker writer interleaved with an unfinished catalog publication");
    const auto markers = catalog_api::ParseSavepoints(context);
    Require(!markers.diagnostic.error, "serialized marker replay failed");
    if (action == catalog_api::CreateMgaSavepointMarker)
      Require(markers.active_savepoints.at(context.local_transaction_id).at("publication_serialized").metadata_event_sequence == next,
              "marker cutoff omitted the completed catalog publication");
    else if (action == catalog_api::RollbackToMgaSavepointMarker)
      Require(catalog_api::MetadataEventRolledBackBySavepoint(markers, context.local_transaction_id, next),
              "rollback upper bound omitted the completed catalog publication");
    else
      Require(!markers.active_savepoints.at(context.local_transaction_id).contains("publication_serialized"),
              "serialized marker release remained active");
  }
#endif
  Rollback(context);
  const auto lifecycle_path = path.string() + ".sb.catalog_object_events";
  const auto rolled_bytes = read(lifecycle_path), rolled_allocator = read(allocator);
  Require(catalog_api::AppendSequencedCatalogJournalRecord(context, frame,
              catalog_api::CatalogJournalKind::lifecycle).error &&
              read(lifecycle_path) == rolled_bytes && read(allocator) == rolled_allocator,
          "rolled-back writer appended already-visible catalog state");
  auto committed_writer = Begin(path, database, scratchbird::tests::FixtureUuid(1378, 8));
  object.creator_tx = committed_writer.local_transaction_id;
  Require(catalog_api::EncodeCatalogLifecycleRecord(object, &bytes) &&
              catalog_api::DecodeApiBehaviorRecord(
                  {reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size()}, &frame),
          "committed-writer fixture frame invalid");
  Commit(committed_writer);
  Require(catalog_api::AppendSequencedCatalogJournalRecord(committed_writer, frame,
              catalog_api::CatalogJournalKind::lifecycle).error &&
              read(lifecycle_path) == rolled_bytes && read(allocator) == rolled_allocator,
          "committed writer appended new catalog state after finality");
}

void TestImmediateReservationFailure(const std::filesystem::path& path) {
#if defined(SB_CATALOG_TEST_WRAP_SYNC)
  catalog_api::EngineRequestContext context;
  context.database_path = path.string();
  const auto stream = context.database_path + ".sb.mga_relation_metadata";
  const auto allocator = context.database_path + ".sb.mga_event_sequence_allocator";
  const auto reserve = [&](std::vector<std::string>* deferred = nullptr) {
    return catalog_api::ReserveEventSequenceRange(context, "relation_metadata", stream, 1,
        [] { return 1; }, deferred);
  };
  Require(reserve().ok, "immediate allocator fixture setup failed");
  for (const bool cold : {true, false}) {
    catalog_api::ClearMgaEventSequenceRangeCacheForTesting();
    const auto next = catalog_api::NextMetadataEventSequence(context);
    if (!cold) {
      std::vector<std::string> deferred;
      const auto warm = reserve(&deferred);
      Require(warm.ok && warm.first == next, "immediate allocator warm setup failed");
      catalog_api::AbandonDeferredEventSequenceReservation(warm);
    }
    const auto size = std::filesystem::file_size(allocator);
    // A real kernel short write, not a synthesized error return. Keep the
    // process limit/signal changes inside this single-threaded fixture scope.
    struct Limit {
      struct rlimit saved {};
      struct sigaction previous {};
      explicit Limit(rlim_t bytes) {
        Require(::getrlimit(RLIMIT_FSIZE, &saved) == 0, "read file-size limit failed");
        struct sigaction ignore {}; ignore.sa_handler = SIG_IGN; ::sigemptyset(&ignore.sa_mask);
        Require(::sigaction(SIGXFSZ, &ignore, &previous) == 0, "set file-size signal failed");
        auto limited = saved; limited.rlim_cur = bytes;
        if (::setrlimit(RLIMIT_FSIZE, &limited) != 0) {
          ::sigaction(SIGXFSZ, &previous, nullptr); Fail("set file-size limit failed");
        }
      }
      ~Limit() {
        if (::setrlimit(RLIMIT_FSIZE, &saved) != 0 ||
            ::sigaction(SIGXFSZ, &previous, nullptr) != 0) std::terminate();
      }
    };
    catalog_api::MgaEventSequenceRangeReservation failed;
    { Limit limit(size + 20); failed = reserve(); }
    if (failed.ok || failed.diagnostic.detail != "mga.event_sequence_allocator:durable_allocator_append_failed" ||
        std::filesystem::file_size(allocator) != size + 20 || catalog_api::NextMetadataEventSequence(context) != 0)
      std::cerr << "short-write cold=" << cold << " ok=" << failed.ok << " detail=" << failed.diagnostic.detail
                << " size=" << std::filesystem::file_size(allocator) << " expected=" << size + 20
                << " next=" << catalog_api::NextMetadataEventSequence(context) << '\n';
    Require(!failed.ok && failed.diagnostic.detail == "mga.event_sequence_allocator:durable_allocator_append_failed" &&
                std::filesystem::file_size(allocator) == size + 20 &&
                catalog_api::NextMetadataEventSequence(context) == 0,
            "partial allocator write was not refused as corrupt authority");
    // Restore only this deliberately torn fixture tail. This is not a runtime
    // recovery algorithm. Inspect the retained process reservation independently.
    std::filesystem::resize_file(allocator, size);
    std::vector<std::string> deferred;
    const auto after = reserve(&deferred);
    Require(after.ok && after.first == next + 1,
            "immediate partial append reused a possibly published reservation");
    Require(catalog_api::AppendDeferredEventSequenceAllocatorLines(context, &deferred, nullptr),
            "post-fault reservation publication failed");
    catalog_api::ClearMgaEventSequenceRangeCacheForTesting();
    Require(catalog_api::NextMetadataEventSequence(context) == next + 2,
            "post-fault reservation lost its high water on cold replay");
  }
#else
  (void)path;
#endif
}

void TestCatalogWholeOperationOwnership(const std::filesystem::path& path) {
  const auto database = CreateDatabase(path);
  auto context = Begin(path, database, scratchbird::tests::FixtureUuid(1378, 8));
  const auto must_wait = [&](auto operation) {
    std::promise<void> starting;
    auto started = starting.get_future();
    std::future<bool> completed;
    bool waited = false;
    {
      const auto owner = catalog_api::AcquireTransactionInventoryGuard(context.database_path);
      completed = std::async(std::launch::async, [&] {
        starting.set_value(); return operation();
      });
      const bool running = started.wait_for(std::chrono::seconds(5)) == std::future_status::ready;
      waited = running && completed.wait_for(std::chrono::milliseconds(100)) == std::future_status::timeout;
    }
    const auto ok = completed.get();
    Require(waited && !ok, "catalog preflight escaped whole-operation ownership");
  };
  // Refusals occur before the first record append. Per-record locking alone
  // cannot satisfy these controls; observation/preflight must own the guard.
  must_wait([&] { catalog_api::EngineCatalogCreateObjectRequest r; r.context = context;
    return catalog_api::EngineCatalogCreateObject(r).ok; });
  must_wait([&] { catalog_api::EngineCatalogApplyConstraintsRequest r; r.context = context;
    return catalog_api::EngineCatalogApplyConstraintsToObject(r).ok; });
  must_wait([&] { catalog_api::EngineCatalogAlterObjectRequest r; r.context = context;
    return catalog_api::EngineCatalogAlterObject(r).ok; });
  must_wait([&] { catalog_api::EngineCatalogRenameObjectRequest r; r.context = context;
    return catalog_api::EngineCatalogRenameObject(r).ok; });
  must_wait([&] { catalog_api::EngineCatalogDropObjectRequest r; r.context = context;
    return catalog_api::EngineCatalogDropObject(r).ok; });
  must_wait([&] { catalog_api::EngineCreateTableRequest r; r.context = context;
    return catalog_api::EngineCreateTable(r).ok; });

  constexpr unsigned count = 4;
  std::vector<catalog_api::EngineCatalogCreateObjectRequest> requests;
  for (unsigned n = 0; n < count; ++n)
    requests.push_back(CreateRequest(context, scratchbird::tests::FixtureUuid(1392, n + 1),
        "schema", {}, "serialized_" + std::to_string(n)));
  std::promise<void> start;
  auto released = start.get_future().share();
  std::vector<std::future<catalog_api::EngineCatalogCreateObjectResult>> writers;
  writers.reserve(count);
  struct Release {
    std::promise<void>& start; bool sent = false;
    ~Release() { if (!sent) start.set_value(); }
  } release{start};
  for (unsigned n = 0; n < count; ++n)
    writers.push_back(std::async(std::launch::async, [&, n] {
      released.wait(); return catalog_api::EngineCatalogCreateObject(requests[n]);
    }));
  start.set_value(); release.sent = true;
  std::set<std::uint64_t> epochs;
  for (auto& writer : writers) {
    const auto created = writer.get();
    RequireOk(created, "concurrent catalog creation failed");
    Require(epochs.insert(created.metadata_cache_epoch).second,
            "concurrent catalog operations reused a metadata publication epoch");
  }
  Commit(context);
  const auto reader = Begin(path, database, scratchbird::tests::FixtureUuid(1378, 8));
  for (const auto& request : requests)
    RequireOk(catalog_api::EngineCatalogLookupObjectByUuid(
        LookupRequest(reader, request.target_object.uuid, "schema")), "serialized object missing after commit");
  Rollback(reader);
}

void TestMutationExclusionAuthority(const std::filesystem::path& path) {
  namespace mga = scratchbird::transaction::mga;
  const auto database = CreateDatabase(path);
  auto context = Begin(path, database, scratchbird::tests::FixtureUuid(1378, 8));
  const auto validate = [&](const auto& candidate) {
    return catalog_api::ValidateMgaMutatingTransactionAuthorityForStoreModule(candidate, "mutation_authority_probe");
  };
  Require(!validate(context).error, "actual writable transaction refused");
  const auto snapshot = db::AcquireLocalTransactionInventorySnapshot(context.database_path);
  Require(snapshot.ok(), "mutation authority snapshot acquisition failed");
  context.statement_transaction_inventory_snapshot = snapshot.snapshot;
  const auto inventory = db::LoadLocalTransactionInventoryFromDatabase(context.database_path);
  Require(inventory.ok(), "mutation exclusion inventory unavailable");
  const auto marked = mga::MarkLocalTransactionRollbackOnly(inventory.inventory,
      mga::MakeLocalTransactionId(context.local_transaction_id));
  Require(marked.ok() && db::PersistLocalTransactionInventoryToDatabase(context.database_path, marked.inventory).ok(),
          "actual mutation exclusion publication failed");
  const auto denied = validate(context);
  Require(denied.error && denied.detail.find("rollback_only") != std::string::npos,
          "mutation admission ignored a durable rollback-only fence behind a retained statement snapshot");
  catalog_api::CrudIndexRecord index;
  index.creator_tx = context.local_transaction_id;
  index.index_uuid = scratchbird::tests::FixtureUuid(1393, 1);
  index.table_uuid = scratchbird::tests::FixtureUuid(1393, 2);
  index.column_name = "id"; index.default_name = "fenced_index";
  index.family = "btree"; index.profile = "row_store_scalar_btree_v1";
  const auto legacy = catalog_api::AppendCrudEvent(context,
      catalog_api::MakeCrudIndexCreateEventV2(context.local_transaction_id, index));
  Require(legacy.error && legacy.detail.find("rollback_only") != std::string::npos &&
              !std::filesystem::exists(context.database_path + ".sb.crud_events"),
          "legacy CRUD appended effects for a rollback-only owner");
  catalog_api::EngineCommitTransactionRequest commit; commit.context = context;
  Require(!catalog_api::EngineCommitTransaction(commit).ok, "fenced transaction committed");
  Rollback(context);
  Require(validate(context).error, "retained statement authorized an already rolled-back writer");
  auto writable = Begin(path, database, scratchbird::tests::FixtureUuid(1378, 8));
  for (unsigned fault = 0; fault != 4; ++fault) {
    auto foreign = writable;
    if (fault == 0) foreign.transaction_uuid = scratchbird::tests::FixtureUuid(1393, 3);
    else if (fault == 1) foreign.database_uuid = scratchbird::tests::FixtureUuid(1393, 4);
    else if (fault == 2) foreign.transaction_uuid = {};
    else foreign.local_transaction_id = 0;
    Require(validate(foreign).error, "mutation admission accepted a foreign/incomplete binary owner");
    Require(catalog_api::AppendCrudEvent(foreign,
                catalog_api::MakeCrudIndexCreateEventV2(writable.local_transaction_id, index)).error &&
                !std::filesystem::exists(context.database_path + ".sb.crud_events"),
            "legacy CRUD appended effects for a foreign/incomplete binary owner");
  }
  Require(!validate(writable).error, "fence from another transaction denied the actual writer");
  const auto retained = db::AcquireLocalTransactionInventorySnapshot(context.database_path);
  Require(retained.ok(), "writable snapshot acquisition failed");
  writable.statement_transaction_inventory_snapshot = retained.snapshot;
  index.creator_tx = writable.local_transaction_id;
  Require(!catalog_api::AppendCrudEvent(writable,
              catalog_api::MakeCrudIndexCreateEventV2(writable.local_transaction_id, index)).error &&
              std::filesystem::file_size(context.database_path + ".sb.crud_events") != 0,
          "actual unfenced legacy CRUD append failed");
  catalog_api::CrudTableRecord table;
  table.creator_tx = writable.local_transaction_id;
  table.table_uuid = index.table_uuid; table.default_name = "owned_metadata";
  // An actual standalone metadata writer must hold admission until effects.
  // Retain finality's guard while changing authority and forcing the waiting
  // append to reload it; the writer must not use its old statement snapshot.
  std::promise<void> starting;
  auto started = starting.get_future();
  std::future<catalog_api::EngineApiDiagnostic> append;
  bool waited = false;
  {
    const auto owner = catalog_api::AcquireTransactionInventoryGuard(context.database_path);
    append = std::async(std::launch::async, [&] {
      starting.set_value(); return catalog_api::AppendMgaTableMetadata(writable, table);
    });
    const bool running = started.wait_for(std::chrono::seconds(5)) == std::future_status::ready;
    waited = running && append.wait_for(std::chrono::milliseconds(100)) == std::future_status::timeout;
    Commit(writable);
  }
  const auto stale = append.get();
  Require(waited && stale.error && !std::filesystem::exists(context.database_path + ".sb.mga_relation_metadata"),
          "standalone metadata append escaped finality ownership or used stale active authority");
  Require(validate(writable).error, "retained statement authorized a committed writer");
  const auto final_writer = Begin(path, database, scratchbird::tests::FixtureUuid(1378, 8));
  table.creator_tx = final_writer.local_transaction_id;
  Require(!catalog_api::AppendMgaTableMetadata(final_writer, table).error &&
              std::filesystem::file_size(context.database_path + ".sb.mga_relation_metadata") != 0,
          "actual standalone unfenced metadata append failed");
  Rollback(final_writer);
}

void TestCatalogDefinitionPreflight(const std::filesystem::path& path) {
  const auto database_uuid = CreateDatabase(path);
  const auto principal = scratchbird::tests::FixtureUuid(1394, 1);
  auto context = Begin(path, database_uuid, principal);
  const auto schema_uuid = scratchbird::tests::FixtureUuid(1394, 2);
  RequireOk(catalog_api::EngineCatalogCreateObject(
      CreateRequest(context, schema_uuid, "schema", {}, "preflight")),
      "preflight schema setup failed");
  auto table = CreateRequest(context, scratchbird::tests::FixtureUuid(1394, 3),
      "table", schema_uuid, "preflight_table");
  catalog_api::EngineColumnDefinition column;
  column.requested_column_uuid = scratchbird::tests::FixtureUuid(1394, 4);
  column.descriptor.descriptor_kind = "scalar";
  column.descriptor.canonical_type_name = "text";
  column.names.push_back(Name("payload"));
  table.columns.push_back(column);
  catalog_api::EngineConstraintDefinition valid;
  valid.requested_constraint_uuid = scratchbird::tests::FixtureUuid(1394, 5);
  valid.constraint_kind = "check";
  valid.names.push_back(Name("first_check"));
  catalog_api::CatalogConstraintMetadata metadata;
  metadata.text["constraint_hash"] = "preflight_check_hash";
  Require(catalog_api::EncodeCatalogConstraintMetadata(metadata, &valid.canonical_constraint_envelope),
          "preflight check fixture encoding failed");
  auto invalid = valid;
  invalid.requested_constraint_uuid = scratchbird::tests::FixtureUuid(1394, 6);
  invalid.names = {Name("second_check")};
  invalid.canonical_constraint_envelope = "not_binary_constraint_metadata";
  table.constraints = {valid, invalid};
  const auto journals = [&] {
    std::vector<std::pair<bool, std::string>> captured;
    for (const auto suffix : {".sb.catalog_object_events", ".sb.name_events.v2",
                              ".sb.mga_event_sequence_allocator"}) {
      const auto file = path.string() + suffix;
      const bool present = std::filesystem::exists(file);
      std::string bytes;
      if (present) {
        std::ifstream in(file, std::ios::binary);
        Require(in.is_open(), "preflight journal open failed");
        bytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        Require(!in.bad(), "preflight journal read failed");
      }
      captured.emplace_back(present, std::move(bytes));
    }
    return captured;
  };
  const auto inventory_before = db::AcquireLocalTransactionInventorySnapshot(context.database_path);
  Require(inventory_before.ok(), "preflight inventory unavailable");
  const auto before = journals();
  const auto next = catalog_api::NextMetadataEventSequence(context);
  Require(!catalog_api::EngineCatalogCreateObject(table).ok,
          "late invalid constraint was admitted");
  Require(journals() == before && catalog_api::NextMetadataEventSequence(context) == next,
          "late invalid CREATE definition published an earlier catalog prefix");
  const auto inventory_after = db::AcquireLocalTransactionInventorySnapshot(context.database_path);
  Require(inventory_after.ok() && inventory_after.snapshot->inventory.publication_base ==
              inventory_before.snapshot->inventory.publication_base,
          "preflight refusal changed native transaction authority");
  {
    auto malformed_column = table;
    malformed_column.constraints.clear();
    auto later_column = column;
    later_column.ordinal = 1;
    later_column.requested_column_uuid.bytes[6] = 0x40; // user UUIDv4 is not a system identity
    malformed_column.columns.push_back(later_column);
    Require(!catalog_api::EngineCatalogCreateObject(malformed_column).ok && journals() == before &&
                catalog_api::NextMetadataEventSequence(context) == next,
            "late column frame encoding failure published a prefix");
  }
  for (unsigned collision = 0; collision != 3; ++collision) {
    auto repeated = table;
    auto second = valid;
    second.names = {Name("distinct_name_same_identity")};
    if (collision == 1) second.requested_constraint_uuid = repeated.target_object.uuid;
    if (collision == 2) second.requested_constraint_uuid = column.requested_column_uuid;
    repeated.constraints = {valid, second};
    Require(!catalog_api::EngineCatalogCreateObject(repeated).ok && journals() == before &&
                catalog_api::NextMetadataEventSequence(context) == next,
            "pending constraint identity collision was admitted or published effects");
  }
  // The same object and constraint identities can be retried: no hidden prefix
  // or rollback-only exclusion is allowed for pure input validation failure.
  table.constraints.clear();
  RequireOk(catalog_api::EngineCatalogCreateObject(table), "preflight table retry failed");
  catalog_api::EngineCatalogApplyConstraintsRequest apply;
  apply.context = context;
  apply.target_object = table.target_object;
  apply.columns = table.columns;
  apply.constraints = {valid, invalid};
  const auto before_apply = journals();
  const auto apply_next = catalog_api::NextMetadataEventSequence(context);
  Require(!catalog_api::EngineCatalogApplyConstraintsToObject(apply).ok,
          "late invalid constraint application was admitted");
  Require(journals() == before_apply && catalog_api::NextMetadataEventSequence(context) == apply_next,
          "late invalid constraint application published a valid prefix");
  for (unsigned collision = 0; collision != 3; ++collision) {
    auto second = valid;
    second.names = {Name("apply_distinct_name_same_identity")};
    if (collision == 1) second.requested_constraint_uuid = table.target_object.uuid;
    if (collision == 2) second.requested_constraint_uuid = column.requested_column_uuid;
    apply.constraints = {valid, second};
    Require(!catalog_api::EngineCatalogApplyConstraintsToObject(apply).ok &&
                journals() == before_apply && catalog_api::NextMetadataEventSequence(context) == apply_next,
            "constraint application ignored a pending binary identity collision");
  }
  {
    auto existing_column = apply;
    existing_column.columns.clear();
    auto constraint = valid;
    constraint.requested_constraint_uuid = column.requested_column_uuid;
    existing_column.constraints = {constraint};
    Require(!catalog_api::EngineCatalogApplyConstraintsToObject(existing_column).ok &&
                journals() == before_apply && catalog_api::NextMetadataEventSequence(context) == apply_next,
            "constraint creation reused an existing column identity omitted from the request");
    auto other_table = table;
    other_table.target_object.uuid = scratchbird::tests::FixtureUuid(1394, 7);
    other_table.localized_names = {Name("other_preflight_table")};
    Require(!catalog_api::EngineCatalogCreateObject(other_table).ok &&
                journals() == before_apply && catalog_api::NextMetadataEventSequence(context) == apply_next,
            "column identity was silently transferred to another table");
  }
  apply.constraints = {valid};
  RequireOk(catalog_api::EngineCatalogApplyConstraintsToObject(apply),
            "preflight constraint retry failed");
  Commit(context);
  auto reader = Begin(path, database_uuid, principal);
  const auto loaded = catalog_api::LoadCatalogObjectLifecycleState(reader);
  Require(loaded.ok && std::count_if(loaded.state.constraints.begin(), loaded.state.constraints.end(),
      [&](const auto& item) { return item.constraint_uuid == valid.requested_constraint_uuid; }) == 1 &&
      std::none_of(loaded.state.objects.begin(), loaded.state.objects.end(),
      [&](const auto& item) { return item.object_uuid == invalid.requested_constraint_uuid; }),
      "preflight retry did not persist exactly the valid constraint");
  Commit(reader);
}

void TestCatalogFailedPublicationCustody(const std::filesystem::path& path) {
  const auto database = CreateDatabase(path);
  const auto principal = scratchbird::tests::FixtureUuid(1395, 1);
  auto setup = Begin(path, database, principal);
  const auto schema = scratchbird::tests::FixtureUuid(1395, 2);
  RequireOk(catalog_api::EngineCatalogCreateObject(
      CreateRequest(setup, schema, "schema", {}, "custody")), "custody schema setup failed");
  Commit(setup);
  auto writer = Begin(path, database, principal);
  const auto object = scratchbird::tests::FixtureUuid(1395, 3);
  const auto request = CreateRequest(writer, object, "table", schema, "failed_publication");
  const auto names = path.string() + ".sb.name_events.v2";
  struct ReadOnly {
    std::filesystem::path path;
    std::filesystem::perms previous;
    explicit ReadOnly(std::filesystem::path file) : path(std::move(file)),
        previous(std::filesystem::status(path).permissions()) {
      std::filesystem::permissions(path, std::filesystem::perms::owner_write |
          std::filesystem::perms::group_write | std::filesystem::perms::others_write,
          std::filesystem::perm_options::remove);
    }
    ~ReadOnly() { std::error_code error; std::filesystem::permissions(path, previous, error);
      if (error) std::terminate(); }
  };
  {
    ReadOnly fault(names);
    Require(!std::ofstream(names, std::ios::binary | std::ios::app),
            "custody fixture must deny name append (run unprivileged)");
    Require(!catalog_api::EngineCatalogCreateObject(request).ok,
            "failed resolver publication reported success");
  }
  std::ifstream journal(path.string() + ".sb.catalog_object_events", std::ios::binary);
  Require(journal.is_open(), "custody journal open failed");
  bool found_prefix = false;
  while (journal.peek() != std::char_traits<char>::eof()) {
    catalog_api::ApiBehaviorRecord frame;
    Require(catalog_api::ReadApiBehaviorRecord(journal, &frame), "custody journal frame invalid");
    if (frame.creator_tx == writer.local_transaction_id && frame.object_uuid == object)
      found_prefix = true;
  }
  Require(found_prefix, "custody fixture did not retain an actual catalog prefix");
  Require(!catalog_api::LoadCatalogObjectLifecycleState(writer).ok &&
              !catalog_api::LoadCatalogObjectLifecycleEpochState(writer).ok &&
              !catalog_api::LoadNameRegistryState(writer, writer.local_transaction_id).ok,
          "ordinary catalog readers exposed a failed publication prefix");
  catalog_api::EngineCommitTransactionRequest commit;
  commit.context = writer;
  Require(!catalog_api::EngineCommitTransaction(commit).ok,
          "failed multi-record catalog prefix remained eligible for COMMIT");
  Require(catalog_api::ValidateMgaMutatingTransactionAuthorityForStoreModule(writer, "custody").error,
          "failed catalog publication allowed ordinary mutations");
  catalog_api::EngineRollbackTransactionRequest rollback;
  rollback.context = writer;
  Require(catalog_api::EngineRollbackTransaction(rollback).ok,
          "failed catalog publication prevented full rollback");
  auto reader = Begin(path, database, principal);
  const auto recovered = catalog_api::LoadCatalogObjectLifecycleState(reader);
  Require(recovered.ok && std::none_of(recovered.state.objects.begin(), recovered.state.objects.end(),
      [&](const auto& item) { return item.object_uuid == object; }),
      "failed catalog prefix became visible after full rollback");
  RequireOk(catalog_api::EngineCatalogCreateObject(
      CreateRequest(reader, object, "table", schema, "failed_publication")),
      "fresh transaction could not retry rolled-back catalog creation");
  Commit(reader);
}

void TestCatalogBatchBoundaries(const std::filesystem::path& path) {
  const auto database = CreateDatabase(path);
  const auto principal = scratchbird::tests::FixtureUuid(1396, 1);
  const auto schema = scratchbird::tests::FixtureUuid(1396, 2);
  auto setup = Begin(path, database, principal);
  RequireOk(catalog_api::EngineCatalogCreateObject(
      CreateRequest(setup, schema, "schema", {}, "batch_boundaries")), "batch boundary setup failed");
  Commit(setup);
  const auto read = [](const std::string& file) {
    std::ifstream input(file, std::ios::binary);
    Require(input.is_open(), "batch evidence file missing");
    std::string bytes(std::istreambuf_iterator<char>{input}, {});
    Require(!input.bad(), "batch evidence file read failed");
    return bytes;
  };
  const auto catalog = path.string() + ".sb.catalog_object_events";
  const auto names = path.string() + ".sb.name_events.v2";
  const auto allocator = path.string() + ".sb.mga_event_sequence_allocator";
  {
    auto context = Begin(path, database, principal);
    const auto before = db::AcquireStrongLocalTransactionInventorySnapshot(context.database_path);
    const auto catalog_before = read(catalog), allocator_before = read(allocator);
    const auto next = catalog_api::NextMetadataEventSequence(context);
    namespace memory = scratchbird::core::memory;
    memory::MemoryFailureInjectionConfiguration injection;
    injection.test_guard = memory::MakeMemoryFailureInjectionTestGuard();
    injection.fixture_enabled = true;
    injection.fixture_name = "catalog_batch_admission";
    memory::MemoryFailureInjectionRule rule;
    rule.rule_id = "catalog_batch_admission";
    rule.purpose = "catalog_journal_batch";
    rule.scope_kind = memory::MemoryFailureInjectionScopeKind::database;
    rule.binary_scope_uuid = context.database_uuid.bytes;
    injection.rules.push_back(rule);
    Require(memory::DefaultMemoryManager().EnableAllocationFailureInjection(injection).ok(),
            "batch memory fault setup failed");
    struct Cleanup { ~Cleanup() { if (!scratchbird::core::memory::DefaultMemoryManager().DisableAllocationFailureInjection().ok()) std::terminate(); } } cleanup;
    const auto failed = catalog_api::EngineCatalogCreateObject(
        CreateRequest(context, scratchbird::tests::FixtureUuid(1396, 3), "table", schema, "denied_batch"));
    const auto injected = memory::DefaultMemoryManager().FailureInjectionSnapshot();
    const auto after = db::AcquireStrongLocalTransactionInventorySnapshot(context.database_path);
    Require(!failed.ok && injected.rules.size() == 1 && injected.rules.front().failure_count == 1 &&
                before.ok() && after.ok() && before.snapshot->inventory.publication_base == after.snapshot->inventory.publication_base &&
                read(catalog) == catalog_before && read(allocator) == allocator_before &&
                catalog_api::NextMetadataEventSequence(context) == next,
            "batch memory denial published state or retained an unpublished reservation");
    Rollback(context);
  }
  // Actual legacy-file admission. Restore the native name journal even if an
  // assertion throws; this is one temporary fixture rename, not an archive.
  {
    auto context = Begin(path, database, principal);
    const auto before = db::AcquireStrongLocalTransactionInventorySnapshot(context.database_path);
    const auto catalog_before = read(catalog), allocator_before = read(allocator);
    const auto legacy = path.string() + ".sb.api_events";
    const bool had_legacy = std::filesystem::exists(legacy);
    if (!had_legacy) { std::ofstream out(legacy, std::ios::binary); Require(out.good(), "legacy fixture creation failed"); }
    const auto saved = names + ".fixture";
    std::filesystem::rename(names, saved);
    struct Restore {
      std::string names, saved, legacy; bool had_legacy;
      ~Restore() { std::error_code error; std::filesystem::rename(saved, names, error);
        if (error) std::terminate(); if (!had_legacy) { std::filesystem::remove(legacy, error); if (error) std::terminate(); } }
    } restore{names, saved, legacy, had_legacy};
    const auto failed = catalog_api::EngineCatalogCreateObject(
        CreateRequest(context, scratchbird::tests::FixtureUuid(1396, 4), "table", schema, "legacy_refusal"));
    const auto after = db::AcquireStrongLocalTransactionInventorySnapshot(context.database_path);
    Require(!failed.ok && !std::filesystem::exists(names) && before.ok() && after.ok() &&
                before.snapshot->inventory.publication_base == after.snapshot->inventory.publication_base &&
                read(catalog) == catalog_before && read(allocator) == allocator_before,
            "batch bypassed legacy name admission or changed state on preflight refusal");
    Rollback(context);
  }
#if defined(SB_CATALOG_TEST_WRAP_SYNC)
  for (unsigned phase = 0; phase != 10; ++phase) {
    auto context = Begin(path, database, principal);
    const auto object = scratchbird::tests::FixtureUuid(1396, 10 + phase);
    const auto request = CreateRequest(context, object, "table", schema, "sync_boundary_" + std::to_string(phase));
    Require(catalog_api::LoadCatalogObjectLifecycleState(context).ok &&
                catalog_api::LoadCatalogObjectLifecycleEpochState(context).ok &&
                catalog_api::LoadNameRegistryState(context, context.local_transaction_id).ok,
            "batch warm reader setup failed");
    const auto retained = db::AcquireLocalTransactionInventorySnapshot(context.database_path);
    Require(retained.ok(), "batch retained snapshot setup failed");
    context.statement_transaction_inventory_snapshot = retained.snapshot;
    const auto catalog_before = read(catalog), names_before = read(names);
    const std::string target = phase < 2 ? allocator : phase < 4 ? catalog : phase < 6 ? names : path.string();
    catalog_publication_fault::AfterSync after;
    Require(::stat(target.c_str(), &catalog_publication_fault::target) == 0,
            "batch sync target missing");
    after.trigger = catalog_publication_fault::target;
    Require(::stat(path.parent_path().c_str(), &after.failure) == 0,
            "batch directory target missing");
    const bool directory = phase < 6 ? phase % 2 != 0 : phase >= 8;
    after.skip = phase == 9 ? 1 : 0;
    struct FaultScope {
      FaultScope(bool directory, unsigned phase, catalog_publication_fault::AfterSync& after) {
        catalog_publication_fault::hit = false;
        catalog_publication_fault::skip_matches = phase == 7 ? 1 : 0;
        catalog_publication_fault::armed = !directory;
        catalog_publication_fault::after_sync = directory ? &after : nullptr;
      }
      ~FaultScope() { catalog_publication_fault::armed = false; catalog_publication_fault::after_sync = nullptr; }
    };
    catalog_api::EngineCatalogCreateObjectResult failed;
    { FaultScope fault(directory, phase, after); failed = catalog_api::EngineCatalogCreateObject(request); }
    Require(catalog_publication_fault::hit && !failed.ok && (!directory || after.hit),
            "batch ignored actual file/directory/inventory sync failure");
    const auto actual = db::AcquireStrongLocalTransactionInventorySnapshot(context.database_path);
    Require(actual.ok(), "batch failed acknowledgement inventory could not be reopened");
    const auto entry = scratchbird::transaction::mga::LookupLocalTransaction(actual.snapshot->inventory,
        scratchbird::transaction::mga::MakeLocalTransactionId(context.local_transaction_id));
    const bool excluded = phase != 6 && phase != 9;
    Require(entry.ok() && entry.entry.identity.transaction_uuid.value == context.transaction_uuid &&
                entry.entry.rollback_only == excluded,
            "batch failure was classified from acknowledgement instead of actual inventory");
    bool typed_owner = false, classified = false;
    for (const auto& diagnostic : failed.diagnostics) {
      for (const auto& [key, value] : diagnostic.identity_fields)
        if (key == "transaction_uuid" && value == context.transaction_uuid) typed_owner = true;
      for (const auto& field : diagnostic.fields)
        if (field.key == "observed_native_commit_exclusion" && field.value == (excluded ? "retained" : "absent")) classified = true;
    }
    Require(typed_owner && classified, "batch failure dropped its binary owner or actual inventory classification");
    if (phase == 6 || phase == 8)
      Require(read(catalog) == catalog_before && read(names) == names_before,
              "failed native marking attempted catalog publication");
    if (excluded) {
      Require(!catalog_api::LoadCatalogObjectLifecycleState(context).ok &&
                  !catalog_api::LoadCatalogObjectLifecycleEpochState(context).ok &&
                  !catalog_api::LoadNameRegistryState(context, context.local_transaction_id).ok,
              "warm/retained catalog reader bypassed current batch exclusion");
      catalog_api::EngineCommitTransactionRequest commit; commit.context = context;
      Require(!catalog_api::EngineCommitTransaction(commit).ok,
              "failed batch became committable");
    }
    if (phase == 9) {
      const auto complete = catalog_api::LoadCatalogObjectLifecycleState(context);
      Require(complete.ok && std::any_of(complete.state.objects.begin(), complete.state.objects.end(),
          [&](const auto& item) { return item.object_uuid == object; }),
          "lost release acknowledgement lost completed catalog object");
      RequireOk(catalog_api::EngineCatalogResolveObjectName(
          ResolveRequest(context, "table", schema, "sync_boundary_9")),
          "lost release acknowledgement lost completed resolver publication");
      Commit(context);
    } else Rollback(context);
  }
  {
    auto context = Begin(path, database, principal);
    catalog_publication_fault::Pause pause;
    Require(::stat(catalog.c_str(), &pause.target) == 0, "batch reader pause target missing");
    std::promise<void> resume;
    pause.release = resume.get_future().share();
    auto reached = pause.reached.get_future();
    std::promise<void> full_started, epoch_started, name_started;
    auto full_signal = full_started.get_future(), epoch_signal = epoch_started.get_future(), name_signal = name_started.get_future();
    std::future<catalog_api::EngineCatalogCreateObjectResult> publisher;
    std::future<bool> full_reader, epoch_reader, name_reader;
    struct Release {
      std::promise<void>& resume; bool released = false;
      void Now() { if (!released) { released = true; resume.set_value(); } }
      ~Release() { Now(); }
    } release{resume};
    publisher = std::async(std::launch::async, [&] {
      struct PauseScope { explicit PauseScope(catalog_publication_fault::Pause& pause) {
          catalog_publication_fault::pause = &pause;
        } ~PauseScope() { catalog_publication_fault::pause = nullptr; } } scope(pause);
      return catalog_api::EngineCatalogCreateObject(
          CreateRequest(context, scratchbird::tests::FixtureUuid(1396, 31), "table", schema, "concurrent_batch"));
    });
    Require(reached.wait_for(std::chrono::seconds(5)) == std::future_status::ready,
            "batch did not reach actual catalog synchronization");
    full_reader = std::async(std::launch::async, [&] { full_started.set_value(); return catalog_api::LoadCatalogObjectLifecycleState(context).ok; });
    epoch_reader = std::async(std::launch::async, [&] { epoch_started.set_value(); return catalog_api::LoadCatalogObjectLifecycleEpochState(context).ok; });
    name_reader = std::async(std::launch::async, [&] { name_started.set_value(); return catalog_api::LoadNameRegistryState(context, context.local_transaction_id).ok; });
    full_signal.wait(); epoch_signal.wait(); name_signal.wait();
    Require(full_reader.wait_for(std::chrono::milliseconds(50)) == std::future_status::timeout &&
                epoch_reader.wait_for(std::chrono::milliseconds(50)) == std::future_status::timeout &&
                name_reader.wait_for(std::chrono::milliseconds(50)) == std::future_status::timeout,
            "catalog reader consumed an in-flight publication prefix");
    release.Now();
    RequireOk(publisher.get(), "concurrent batch publication failed");
    Require(full_reader.get() && epoch_reader.get() && name_reader.get(),
            "readers did not resume after complete durable batch release");
    Commit(context);
  }
  {
    auto context = Begin(path, database, principal);
    catalog_publication_fault::AfterSync after;
    Require(::stat(catalog.c_str(), &after.trigger) == 0, "allocation fault trigger missing");
    after.deny_allocations = true;
    bool caught = false;
    {
      struct FaultScope {
        explicit FaultScope(catalog_publication_fault::AfterSync& after) {
          catalog_allocation_fault::failures = 0; catalog_publication_fault::after_sync = &after;
        }
        ~FaultScope() { catalog_publication_fault::after_sync = nullptr; catalog_allocation_fault::denied = false; }
      } fault(after);
      try { (void)catalog_api::EngineCatalogCreateObject(
          CreateRequest(context, scratchbird::tests::FixtureUuid(1396, 30), "table", schema, "allocation_unwind")); }
      catch (const std::bad_alloc&) { caught = true; }
    }
    Require(caught && after.hit && catalog_allocation_fault::failures != 0,
            "batch did not exercise actual allocation failure after catalog effects");
    const auto actual = db::AcquireStrongLocalTransactionInventorySnapshot(context.database_path);
    Require(actual.ok() && scratchbird::transaction::mga::LookupLocalTransaction(actual.snapshot->inventory,
        scratchbird::transaction::mga::MakeLocalTransactionId(context.local_transaction_id)).entry.rollback_only &&
        !catalog_api::LoadCatalogObjectLifecycleState(context).ok,
        "allocation unwinding cleared owned exclusion or exposed a prefix");
    Rollback(context);
  }
#endif
  {
    catalog_api::EngineBeginTransactionRequest request;
    request.context = BaseContext(path, database, principal);
    request.option_envelopes.push_back("transaction_read_mode:read_only");
    const auto begun = catalog_api::EngineBeginTransaction(request);
    RequireOk(begun, "real read-only catalog observer BEGIN failed");
    Require(begun.read_only && begun.read_mode == "read_only", "catalog observer was not actually read-only");
    auto context = request.context;
    context.local_transaction_id = begun.local_transaction_id;
    context.transaction_uuid = begun.transaction_uuid;
    context.snapshot_visible_through_local_transaction_id = begun.snapshot_visible_through_local_transaction_id;
    const auto full = catalog_api::LoadCatalogObjectLifecycleState(context);
    const auto epoch = catalog_api::LoadCatalogObjectLifecycleEpochState(context);
    const auto names = catalog_api::LoadNameRegistryState(context, context.local_transaction_id);
    if (!full.ok) std::cerr << "readonly full " << full.diagnostic.code << ':' << full.diagnostic.detail << '\n';
    if (!epoch.ok) std::cerr << "readonly epoch " << epoch.diagnostic.code << ':' << epoch.diagnostic.detail << '\n';
    if (!names.ok) std::cerr << "readonly names " << names.diagnostic.code << ':' << names.diagnostic.detail << '\n';
    Require(full.ok && epoch.ok && names.ok,
            "catalog commit-exclusion gate refused an unfenced read-only observer");
    const auto before = db::AcquireStrongLocalTransactionInventorySnapshot(context.database_path);
    const auto catalog_before = read(catalog), names_before = read(path.string() + ".sb.name_events.v2");
    const auto next = catalog_api::NextMetadataEventSequence(context);
    Require(!catalog_api::EngineCatalogCreateObject(
        CreateRequest(context, scratchbird::tests::FixtureUuid(1396, 32), "table", schema, "readonly_write")).ok,
        "read-only catalog support admitted a write");
    const auto after = db::AcquireStrongLocalTransactionInventorySnapshot(context.database_path);
    Require(before.ok() && after.ok() && before.snapshot->inventory.publication_base == after.snapshot->inventory.publication_base &&
                read(catalog) == catalog_before && read(path.string() + ".sb.name_events.v2") == names_before &&
                catalog_api::NextMetadataEventSequence(context) == next,
            "read-only writer refusal retained catalog, allocation or inventory effects");
    Commit(context);
  }
}

#if defined(SB_CATALOG_TEST_WRAP_SYNC)
int RunCatalogCrashChild(const std::filesystem::path& path, std::uint64_t local, unsigned phase) {
  const auto configured = scratchbird::core::memory::ConfigureDefaultMemoryManagerForFixture(
      scratchbird::core::memory::DefaultLocalEngineMemoryPolicy(), "catalog_crash_child");
  Require(configured.ok(), "crash child memory configuration failed");
  const auto current = db::AcquireStrongLocalTransactionInventorySnapshot(path.string());
  Require(current.ok() && current.snapshot->inventory.publication_base, "crash child inventory unavailable");
  auto context = BaseContext(path, current.snapshot->inventory.publication_base->database_uuid,
      scratchbird::tests::FixtureUuid(1397, 1));
  const auto entry = scratchbird::transaction::mga::LookupLocalTransaction(current.snapshot->inventory,
      scratchbird::transaction::mga::MakeLocalTransactionId(local));
  Require(entry.ok(), "crash child owner absent");
  context.local_transaction_id = local;
  context.transaction_uuid = entry.entry.identity.transaction_uuid.value;
  catalog_publication_fault::Crash crash;
  const std::string target = phase == 1 ? path.string() + ".sb.mga_event_sequence_allocator"
      : phase == 2 ? path.string() + ".sb.catalog_object_events"
      : phase == 3 ? path.string() + ".sb.name_events.v2" : path.string();
  Require(::stat(target.c_str(), &crash.trigger) == 0 &&
              ::stat(path.parent_path().c_str(), &crash.parent) == 0,
          "crash child boundary target missing");
  crash.skip = phase >= 4 ? 1 : 0;
  crash.next_parent = phase == 5;
  crash.exit_code = 80 + phase;
  catalog_publication_fault::crash = &crash;
  const auto result = catalog_api::EngineCatalogCreateObject(CreateRequest(context,
      scratchbird::tests::FixtureUuid(1397, 10 + phase), "table", scratchbird::tests::FixtureUuid(1397, 2),
      "crash_boundary_" + std::to_string(phase)));
  catalog_publication_fault::crash = nullptr;
  RequireOk(result, "crash child catalog operation failed before its boundary");
  Fail("crash child did not reach its actual synchronization boundary");
}

void TestCatalogBatchProcessLoss(const std::filesystem::path& path) {
  const auto database = CreateDatabase(path);
  const auto principal = scratchbird::tests::FixtureUuid(1397, 1);
  const auto schema = scratchbird::tests::FixtureUuid(1397, 2);
  auto setup = Begin(path, database, principal);
  RequireOk(catalog_api::EngineCatalogCreateObject(
      CreateRequest(setup, schema, "schema", {}, "crash_custody")), "crash schema setup failed");
  Commit(setup);
  for (unsigned phase = 0; phase != 6; ++phase) {
    auto context = Begin(path, database, principal);
    const auto local = std::to_string(context.local_transaction_id), mode = std::to_string(phase);
    const auto file = path.string();
    // The parent has no open node device while the child is running. Exec a
    // fresh process; no inherited C++ caches/mutexes authorize its operation.
    const auto child = ::fork();
    Require(child >= 0, "crash child fork failed");
    if (child == 0) {
      ::execl("/proc/self/exe", "catalog_object_conformance", "--crash-catalog", file.c_str(), local.c_str(), mode.c_str(), static_cast<char*>(nullptr));
      std::_Exit(125);
    }
    int status = 0;
    pid_t waited;
    do { waited = ::waitpid(child, &status, 0); } while (waited < 0 && errno == EINTR);
    Require(waited == child && WIFEXITED(status) && WEXITSTATUS(status) == 80 + static_cast<int>(phase),
            "catalog child did not stop at the requested real boundary");
    const auto actual = db::AcquireStrongLocalTransactionInventorySnapshot(context.database_path);
    Require(actual.ok(), "native inventory could not reopen after catalog process loss");
    const auto entry = scratchbird::transaction::mga::LookupLocalTransaction(actual.snapshot->inventory,
        scratchbird::transaction::mga::MakeLocalTransactionId(context.local_transaction_id));
    const bool excluded = phase != 0 && phase != 5;
    Require(entry.ok() && entry.entry.identity.transaction_uuid.value == context.transaction_uuid &&
                entry.entry.rollback_only == excluded,
            "catalog process loss recovered the wrong native exclusion state");
    if (excluded) {
      Require(!catalog_api::LoadCatalogObjectLifecycleState(context).ok &&
                  !catalog_api::LoadNameRegistryState(context, context.local_transaction_id).ok,
              "process-loss prefix became ordinary readable catalog");
      catalog_api::EngineCommitTransactionRequest commit; commit.context = context;
      Require(!catalog_api::EngineCommitTransaction(commit).ok,
              "process-loss catalog prefix became committable");
      Rollback(context);
    } else if (phase == 5) {
      RequireOk(catalog_api::EngineCatalogLookupObjectByUuid(
          LookupRequest(context, scratchbird::tests::FixtureUuid(1397, 15), "table")),
          "completed catalog release did not survive process loss");
      RequireOk(catalog_api::EngineCatalogResolveObjectName(
          ResolveRequest(context, "table", schema, "crash_boundary_5")),
          "completed resolver release did not survive process loss");
      Commit(context);
    } else Rollback(context);
  }
}
#endif

}  // namespace

int main(int argc, char** argv) try {
#if defined(SB_CATALOG_TEST_WRAP_SYNC)
  if (argc == 5 && std::string_view(argv[1]) == "--crash-catalog")
    return RunCatalogCrashChild(argv[2], std::stoull(argv[3]), static_cast<unsigned>(std::stoul(argv[4])));
#else
  (void)argc; (void)argv;
#endif
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
  RequireSequencedPublication(path);
  TestSequencedCatalogReplay(artifacts.path() / "sequenced.sbdb");
  TestCatalogPublicationFailures(artifacts.path() / "publication.sbdb");
  TestImmediateReservationFailure(artifacts.path() / "reservation.sbdb");
  TestCatalogWholeOperationOwnership(artifacts.path() / "ownership.sbdb");
  TestMutationExclusionAuthority(artifacts.path() / "mutation-authority.sbdb");
  TestCatalogDefinitionPreflight(artifacts.path() / "preflight.sbdb");
  TestCatalogFailedPublicationCustody(artifacts.path() / "custody.sbdb");
  TestCatalogBatchBoundaries(artifacts.path() / "batch-boundaries.sbdb");
#if defined(SB_CATALOG_TEST_WRAP_SYNC)
  TestCatalogBatchProcessLoss(artifacts.path() / "batch-process-loss.sbdb");
#endif

  artifacts.Cleanup();
  return EXIT_SUCCESS;
} catch (const std::exception& error) {
  std::cerr << error.what() << '\n';
  return EXIT_FAILURE;
}

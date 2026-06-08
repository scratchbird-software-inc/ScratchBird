// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// SB-DATABASE-PROBE-ANCHOR
#include "catalog_bootstrap.hpp"
#include "catalog_identity.hpp"
#include "catalog_persistence.hpp"
#include "database_lifecycle.hpp"
#include "engine_database_runtime.hpp"
#include "memory.hpp"
#include "resource_seed_pack.hpp"
#include "runtime_platform.hpp"
#include "transaction_inventory.hpp"
#include "uuid.hpp"

#include <cstdlib>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

namespace {

using scratchbird::catalog::bootstrap::BootstrapSchemaRootKind;
using scratchbird::catalog::bootstrap::CatalogObjectKind;
using scratchbird::catalog::bootstrap::CatalogPersistenceObjectSeed;
using scratchbird::catalog::bootstrap::CatalogPersistenceSeedConfig;
using scratchbird::catalog::bootstrap::BuildCatalogPersistenceImage;
using scratchbird::catalog::bootstrap::MakeDatabaseBootstrapIdentity;
using scratchbird::catalog::bootstrap::MakeLocalCatalogBootstrapManifest;
using scratchbird::core::platform::DiagnosticRecord;
using scratchbird::core::platform::TypedUuid;
using scratchbird::core::platform::UuidKind;
using scratchbird::core::platform::u32;
using scratchbird::core::platform::u64;
using scratchbird::core::uuid::GenerateEngineIdentityV7;
using scratchbird::core::uuid::ParseTypedUuid;
using scratchbird::core::uuid::TypedUuidResult;
using scratchbird::core::uuid::UuidToString;
using scratchbird::engine::internal_api::EngineContext;
using scratchbird::engine::internal_api::ExecuteShowDatabaseResourcesRuntime;
using scratchbird::engine::internal_api::ExecuteShowDatabaseRuntime;
using scratchbird::engine::internal_api::ExecuteShowVersionRuntime;
using scratchbird::engine::internal_api::MakeEngineDatabaseRuntimeState;
using scratchbird::core::resources::LoadResourceSeedPack;
using scratchbird::core::resources::ResolveResourceSeedAlias;
using scratchbird::core::resources::ResourceSeedFamily;
using scratchbird::core::resources::ResourceSeedCatalogImage;
using scratchbird::core::resources::ResourceSeedLoadConfig;
using scratchbird::storage::database::CreateDatabaseFile;
using scratchbird::storage::database::DatabaseCreateConfig;
using scratchbird::storage::database::DatabaseOpenConfig;
using scratchbird::storage::database::OpenDatabaseFile;
using scratchbird::transaction::mga::BeginLocalTransaction;
using scratchbird::transaction::mga::CommitLocalTransaction;
using scratchbird::transaction::mga::MakeEmptyLocalTransactionInventory;
using scratchbird::transaction::mga::TransactionStateName;

struct Args {
  std::string path;
  std::string database_uuid;
  std::string filespace_uuid;
  std::string session_uuid;
  std::string principal_uuid;
  std::string seed_pack_root;
  u64 creation_millis = 0;
  u32 page_size = 16384;
  bool overwrite = false;
  bool minimal_bootstrap_resources = false;
};

void Usage() {
  std::cerr << "usage: sb_database_probe --path PATH --database-uuid UUID --filespace-uuid UUID "
               "--session-uuid UUID --principal-uuid UUID --creation-ms MILLIS "
               "--seed-pack-root ROOT [--page-size BYTES] [--overwrite] [--minimal-bootstrap-resources]\n";
}

bool ParseU64(const std::string& text, u64* value) {
  char* end = nullptr;
  const unsigned long long parsed = std::strtoull(text.c_str(), &end, 10);
  if (end == nullptr || *end != '\0') {
    return false;
  }
  *value = static_cast<u64>(parsed);
  return true;
}

bool ParseU32(const std::string& text, u32* value) {
  u64 parsed = 0;
  if (!ParseU64(text, &parsed)) {
    return false;
  }
  *value = static_cast<u32>(parsed);
  return parsed <= 0xffffffffull;
}

bool ParseArgs(int argc, char** argv, Args* args) {
  for (int i = 1; i < argc; ++i) {
    const std::string key = argv[i];
    if (key == "--overwrite") {
      args->overwrite = true;
      continue;
    }
    if (key == "--minimal-bootstrap-resources") {
      args->minimal_bootstrap_resources = true;
      continue;
    }
    if (i + 1 >= argc) {
      return false;
    }
    const std::string value = argv[++i];
    if (key == "--path") {
      args->path = value;
    } else if (key == "--database-uuid") {
      args->database_uuid = value;
    } else if (key == "--filespace-uuid") {
      args->filespace_uuid = value;
    } else if (key == "--session-uuid") {
      args->session_uuid = value;
    } else if (key == "--principal-uuid") {
      args->principal_uuid = value;
    } else if (key == "--seed-pack-root") {
      args->seed_pack_root = value;
    } else if (key == "--creation-ms") {
      if (!ParseU64(value, &args->creation_millis)) {
        return false;
      }
    } else if (key == "--page-size") {
      if (!ParseU32(value, &args->page_size)) {
        return false;
      }
    } else {
      return false;
    }
  }
  return !args->path.empty() && !args->database_uuid.empty() && !args->filespace_uuid.empty() &&
         !args->session_uuid.empty() && !args->principal_uuid.empty() &&
         (args->minimal_bootstrap_resources || !args->seed_pack_root.empty()) &&
         args->creation_millis != 0;
}

void PrintDiagnostic(const DiagnosticRecord& diagnostic) {
  std::cerr << diagnostic.diagnostic_code << ":" << diagnostic.message_key << "\n";
}

TypedUuidResult GenerateTyped(UuidKind kind, u64 unix_epoch_millis) {
  return GenerateEngineIdentityV7(kind, unix_epoch_millis);
}

CatalogPersistenceObjectSeed Seed(TypedUuid row_uuid,
                                  TypedUuid object_uuid,
                                  CatalogObjectKind kind,
                                  TypedUuid parent,
                                  std::string path,
                                  std::string name,
                                  bool engine_owned) {
  CatalogPersistenceObjectSeed seed;
  seed.catalog_row_uuid = row_uuid;
  seed.object_uuid = object_uuid;
  seed.object_kind = kind;
  seed.parent_object_uuid = parent;
  seed.localized_path = std::move(path);
  seed.localized_name = std::move(name);
  seed.engine_owned = engine_owned;
  return seed;
}

}  // namespace

int main(int argc, char** argv) {
  Args args;
  if (!ParseArgs(argc, argv, &args)) {
    Usage();
    return 2;
  }

  const auto database_uuid = ParseTypedUuid(UuidKind::database, args.database_uuid);
  const auto filespace_uuid = ParseTypedUuid(UuidKind::filespace, args.filespace_uuid);
  const auto session_uuid = ParseTypedUuid(UuidKind::session, args.session_uuid);
  const auto principal_uuid = ParseTypedUuid(UuidKind::principal, args.principal_uuid);
  if (!database_uuid.ok()) { PrintDiagnostic(database_uuid.diagnostic); return 1; }
  if (!filespace_uuid.ok()) { PrintDiagnostic(filespace_uuid.diagnostic); return 1; }
  if (!session_uuid.ok()) { PrintDiagnostic(session_uuid.diagnostic); return 1; }
  if (!principal_uuid.ok()) { PrintDiagnostic(principal_uuid.diagnostic); return 1; }

  DatabaseCreateConfig create;
  create.path = args.path;
  create.database_uuid = database_uuid.value;
  create.filespace_uuid = filespace_uuid.value;
  create.page_size = args.page_size;
  create.creation_unix_epoch_millis = args.creation_millis;
  create.resource_seed_pack_root = args.seed_pack_root;
  create.allow_minimal_resource_bootstrap = args.minimal_bootstrap_resources;
  create.require_resource_seed_pack = !args.minimal_bootstrap_resources;
  create.allow_overwrite = args.overwrite;

  const auto created = CreateDatabaseFile(create);
  if (!created.ok()) { PrintDiagnostic(created.diagnostic); return 1; }

  DatabaseOpenConfig open;
  open.path = args.path;
  const auto opened = OpenDatabaseFile(open);
  if (!opened.ok()) { PrintDiagnostic(opened.diagnostic); return 1; }

  const auto runtime = MakeEngineDatabaseRuntimeState(opened.state);
  if (!runtime.ok()) { PrintDiagnostic(runtime.diagnostic); return 1; }

  EngineContext context;
  context.database_uuid = opened.state.database_uuid;
  context.session_uuid = session_uuid.value;
  context.principal_uuid = principal_uuid.value;
  context.trace_id = "sb_database_probe";

  const auto show_version = ExecuteShowVersionRuntime(runtime.state, context);
  if (!show_version.ok()) { PrintDiagnostic(show_version.diagnostic); return 1; }
  const auto show_database = ExecuteShowDatabaseRuntime(runtime.state, context);
  if (!show_database.ok()) { PrintDiagnostic(show_database.diagnostic); return 1; }
  const auto show_resources = ExecuteShowDatabaseResourcesRuntime(runtime.state, context);
  const bool show_resources_rejected_as_degraded =
      !show_resources.ok() && args.minimal_bootstrap_resources && !runtime.state.resources_active;
  if (!show_resources.ok() && !show_resources_rejected_as_degraded) {
    PrintDiagnostic(show_resources.diagnostic);
    return 1;
  }

  std::string charset_alias_result;
  std::string collation_alias_result;
  std::string timezone_alias_result;
  if (runtime.state.resources_active) {
    const auto charset_alias = ResolveResourceSeedAlias(runtime.state.resources,
                                                       ResourceSeedFamily::charset,
                                                       "US-ASCII");
    if (!charset_alias.ok()) { PrintDiagnostic(charset_alias.diagnostic); return 1; }
    const auto collation_alias = ResolveResourceSeedAlias(runtime.state.resources,
                                                         ResourceSeedFamily::collation,
                                                         "ascii_bin");
    if (!collation_alias.ok()) { PrintDiagnostic(collation_alias.diagnostic); return 1; }
    const auto timezone_alias = ResolveResourceSeedAlias(runtime.state.resources,
                                                        ResourceSeedFamily::timezone_tables,
                                                        "America/Toronto");
    if (!timezone_alias.ok()) { PrintDiagnostic(timezone_alias.diagnostic); return 1; }
    charset_alias_result = charset_alias.alias.canonical_name;
    collation_alias_result = collation_alias.alias.canonical_name;
    timezone_alias_result = timezone_alias.alias.canonical_name;
  }

  const auto db_identity = MakeDatabaseBootstrapIdentity(database_uuid.value, args.creation_millis);
  const auto sys_catalog_schema = GenerateTyped(UuidKind::schema, args.creation_millis + 100);
  const auto sys_metrics_schema = GenerateTyped(UuidKind::schema, args.creation_millis + 101);
  const auto local_user_schema = GenerateTyped(UuidKind::schema, args.creation_millis + 102);
  if (!db_identity.ok()) { PrintDiagnostic(db_identity.diagnostic); return 1; }
  if (!sys_catalog_schema.ok()) { PrintDiagnostic(sys_catalog_schema.diagnostic); return 1; }
  if (!sys_metrics_schema.ok()) { PrintDiagnostic(sys_metrics_schema.diagnostic); return 1; }
  if (!local_user_schema.ok()) { PrintDiagnostic(local_user_schema.diagnostic); return 1; }

  const auto bootstrap = MakeLocalCatalogBootstrapManifest(db_identity.identity,
                                                           sys_catalog_schema.value,
                                                           sys_metrics_schema.value,
                                                           local_user_schema.value);
  if (!bootstrap.ok()) { PrintDiagnostic(bootstrap.diagnostic); return 1; }

  const auto db_row = GenerateTyped(UuidKind::row, args.creation_millis + 200);
  const auto db_object = GenerateTyped(UuidKind::object, args.creation_millis + 201);
  const auto sys_catalog_row = GenerateTyped(UuidKind::row, args.creation_millis + 202);
  const auto sys_catalog_object = GenerateTyped(UuidKind::object, args.creation_millis + 203);
  const auto sys_metrics_row = GenerateTyped(UuidKind::row, args.creation_millis + 204);
  const auto sys_metrics_object = GenerateTyped(UuidKind::object, args.creation_millis + 205);
  const auto local_user_row = GenerateTyped(UuidKind::row, args.creation_millis + 206);
  const auto local_user_object = GenerateTyped(UuidKind::object, args.creation_millis + 207);
  if (!db_row.ok()) { PrintDiagnostic(db_row.diagnostic); return 1; }
  if (!db_object.ok()) { PrintDiagnostic(db_object.diagnostic); return 1; }
  if (!sys_catalog_row.ok()) { PrintDiagnostic(sys_catalog_row.diagnostic); return 1; }
  if (!sys_catalog_object.ok()) { PrintDiagnostic(sys_catalog_object.diagnostic); return 1; }
  if (!sys_metrics_row.ok()) { PrintDiagnostic(sys_metrics_row.diagnostic); return 1; }
  if (!sys_metrics_object.ok()) { PrintDiagnostic(sys_metrics_object.diagnostic); return 1; }
  if (!local_user_row.ok()) { PrintDiagnostic(local_user_row.diagnostic); return 1; }
  if (!local_user_object.ok()) { PrintDiagnostic(local_user_object.diagnostic); return 1; }

  CatalogPersistenceSeedConfig catalog_config;
  catalog_config.bootstrap_manifest = bootstrap.manifest;
  catalog_config.objects.push_back(Seed(db_row.value, db_object.value, CatalogObjectKind::database, {}, "database", "database", true));
  catalog_config.objects.push_back(Seed(sys_catalog_row.value, sys_catalog_object.value, CatalogObjectKind::schema, db_object.value, "sys.catalog", "catalog", true));
  catalog_config.objects.push_back(Seed(sys_metrics_row.value, sys_metrics_object.value, CatalogObjectKind::schema, db_object.value, "sys.metrics", "metrics", true));
  catalog_config.objects.push_back(Seed(local_user_row.value, local_user_object.value, CatalogObjectKind::schema, db_object.value, "local.user", "user", false));
  const auto catalog_image = BuildCatalogPersistenceImage(catalog_config);
  if (!catalog_image.ok()) { PrintDiagnostic(catalog_image.diagnostic); return 1; }

  const auto txn_uuid = GenerateTyped(UuidKind::transaction, args.creation_millis + 300);
  if (!txn_uuid.ok()) { PrintDiagnostic(txn_uuid.diagnostic); return 1; }
  auto inventory = MakeEmptyLocalTransactionInventory();
  const auto begin = BeginLocalTransaction(inventory, txn_uuid.value, args.creation_millis + 301);
  if (!begin.ok()) { PrintDiagnostic(begin.diagnostic); return 1; }
  const auto commit = CommitLocalTransaction(begin.inventory, begin.entry.identity.local_id, args.creation_millis + 302);
  if (!commit.ok()) { PrintDiagnostic(commit.diagnostic); return 1; }

  std::cout << "{\n";
  std::cout << "  \"ok\": true,\n";
  std::cout << "  \"database_uuid\": \"" << UuidToString(opened.state.database_uuid.value) << "\",\n";
  std::cout << "  \"page_size\": " << opened.state.header.page_size << ",\n";
  std::cout << "  \"show_version_rows\": " << show_version.rows.size() << ",\n";
  std::cout << "  \"show_database_rows\": " << show_database.rows.size() << ",\n";
  std::cout << "  \"show_database_resources_rows\": " << show_resources.rows.size() << ",\n";
  std::cout << "  \"show_database_resources_rejected_as_degraded\": "
            << (show_resources_rejected_as_degraded ? "true" : "false") << ",\n";
  std::cout << "  \"resource_seed_active\": " << (runtime.state.resources_active ? "true" : "false") << ",\n";
  std::cout << "  \"resource_seed_minimal_bootstrap\": "
            << (opened.state.resource_seed_catalog.minimal_bootstrap ? "true" : "false") << ",\n";
  std::cout << "  \"resource_artifacts\": " << runtime.state.resources.resource_artifact_records << ",\n";
  std::cout << "  \"resource_aliases\": " << runtime.state.resources.aliases.size() << ",\n";
  std::cout << "  \"charset_alias_US_ASCII\": \"" << charset_alias_result << "\",\n";
  std::cout << "  \"collation_alias_ascii_bin\": \"" << collation_alias_result << "\",\n";
  std::cout << "  \"timezone_alias_America_Toronto\": \"" << timezone_alias_result << "\",\n";
  std::cout << "  \"charset_records\": " << runtime.state.resources.charset_records << ",\n";
  std::cout << "  \"collation_records\": " << runtime.state.resources.collation_records << ",\n";
  std::cout << "  \"timezone_records\": " << runtime.state.resources.timezone_records << ",\n";
  std::cout << "  \"typed_catalog_records\": " << opened.state.typed_catalog_record_count << ",\n";
  std::cout << "  \"memory_current_bytes\": " << opened.state.memory_accounting.current_bytes << ",\n";
  std::cout << "  \"memory_peak_bytes\": " << opened.state.memory_accounting.peak_bytes << ",\n";
  std::cout << "  \"memory_allocations\": " << opened.state.memory_accounting.allocation_count << ",\n";
  std::cout << "  \"memory_failures\": " << opened.state.memory_accounting.failure_count << ",\n";
  std::cout << "  \"memory_page_buffer_peak_bytes\": " << opened.state.memory_accounting.page_buffer_peak_bytes << ",\n";
  std::cout << "  \"transaction_inventory_present\": "
            << (opened.state.local_transaction_inventory_present ? "true" : "false") << ",\n";
  std::cout << "  \"transaction_inventory_next_id\": "
            << opened.state.local_transaction_inventory.next_local_transaction_id << ",\n";
  std::cout << "  \"transaction_inventory_entries\": [";
  for (std::size_t index = 0;
       index < opened.state.local_transaction_inventory.entries.size();
       ++index) {
    const auto& entry = opened.state.local_transaction_inventory.entries[index];
    if (index != 0) std::cout << ", ";
    std::cout << "{\"local_transaction_id\": "
              << entry.identity.local_id.value
              << ", \"state\": \""
              << TransactionStateName(entry.state)
              << "\", \"transaction_uuid\": \""
              << UuidToString(entry.identity.transaction_uuid.value)
              << "\", \"begin_visible_through_local_transaction_id\": "
              << entry.begin_visible_through_local_transaction_id
              << ", \"evidence_record_written\": "
              << (entry.evidence_record_written ? "true" : "false")
              << "}";
  }
  std::cout << "],\n";
  std::cout << "  \"catalog_objects\": " << catalog_image.image.identity_manifest.objects.size() << ",\n";
  std::cout << "  \"transaction_state\": \"" << TransactionStateName(commit.entry.state) << "\"\n";
  std::cout << "}\n";
  return 0;
}

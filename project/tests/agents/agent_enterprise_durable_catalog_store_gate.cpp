// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "agents/agent_durable_catalog_store_api.hpp"

#include "agent_durable_catalog.hpp"
#include "crud_support/crud_store.hpp"
#include "database_lifecycle.hpp"
#include "local_transaction_store.hpp"
#include "mga_relation_store/mga_relation_store.hpp"
#include "transaction_inventory.hpp"
#include "uuid.hpp"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <utility>

namespace {

namespace agents = scratchbird::core::agents;
namespace api = scratchbird::engine::internal_api;
namespace db = scratchbird::storage::database;
namespace mga = scratchbird::transaction::mga;
namespace uuid = scratchbird::core::uuid;
using scratchbird::core::platform::UuidKind;

[[noreturn]] void Fail(const std::string& message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, const std::string& message) {
  if (!condition) { Fail(message); }
}

struct TestDatabase {
  std::filesystem::path path;
  std::string database_uuid;
  std::string transaction_uuid;
  std::uint64_t local_transaction_id = 0;
};

void Cleanup(const std::filesystem::path& path) {
  std::error_code ignored;
  std::filesystem::remove(path, ignored);
  for (const char* suffix : {".dirty.manifest",
                             ".sb.mga_event_sequence_allocator",
                             ".sb.mga_index_entries",
                             ".sb.mga_large_values",
                             ".sb.mga_relation_descriptors",
                             ".sb.mga_relation_metadata",
                             ".sb.mga_row_versions",
                             ".sb.mga_savepoints",
                             ".sb.mga_secondary_index_delta_ledger"}) {
    std::filesystem::remove(path.string() + suffix, ignored);
  }
}

TestDatabase CreateActiveDatabase() {
  const std::filesystem::path path =
      std::filesystem::temp_directory_path() /
      "scratchbird_aeic_agent_catalog_store.sbdb";
  Cleanup(path);

  const auto database_uuid = uuid::GenerateEngineIdentityV7(UuidKind::database,
                                                            1790000000001);
  const auto filespace_uuid = uuid::GenerateEngineIdentityV7(UuidKind::filespace,
                                                             1790000000002);
  Require(database_uuid.ok(), "database UUID generation failed");
  Require(filespace_uuid.ok(), "filespace UUID generation failed");

  db::DatabaseCreateConfig create;
  create.path = path.string();
  create.database_uuid = database_uuid.value;
  create.filespace_uuid = filespace_uuid.value;
  create.page_size = 16384;
  create.creation_unix_epoch_millis = 1790000000003;
  create.allow_minimal_resource_bootstrap = true;
  create.require_resource_seed_pack = false;
  create.allow_overwrite = true;
  const auto created = db::CreateDatabaseFile(create);
  if (!created.ok()) {
    std::cerr << created.diagnostic.diagnostic_code << ':'
              << created.diagnostic.message_key << '\n';
  }
  Require(created.ok(), "database creation failed");

  auto inventory = mga::MakeEmptyLocalTransactionInventory();
  const auto transaction_uuid = uuid::GenerateEngineIdentityV7(UuidKind::transaction,
                                                              1790000000004);
  Require(transaction_uuid.ok(), "transaction UUID generation failed");
  auto begun = mga::BeginLocalTransaction(std::move(inventory),
                                          transaction_uuid.value,
                                          1790000000005);
  Require(begun.ok(), "local transaction begin failed");
  const auto persisted =
      db::PersistLocalTransactionInventoryToDatabase(path.string(),
                                                     begun.inventory);
  if (!persisted.ok()) {
    std::cerr << persisted.diagnostic.diagnostic_code << ':'
              << persisted.diagnostic.message_key << '\n';
  }
  Require(persisted.ok(), "local transaction inventory persist failed");

  TestDatabase result;
  result.path = path;
  result.database_uuid = uuid::UuidToString(database_uuid.value.value);
  result.transaction_uuid = uuid::UuidToString(transaction_uuid.value.value);
  result.local_transaction_id = begun.entry.identity.local_id.value;
  return result;
}

api::EngineRequestContext Context(const TestDatabase& database) {
  api::EngineRequestContext context;
  context.request_id = "aeic-agent-catalog-store";
  context.database_path = database.path.string();
  context.database_uuid.canonical = database.database_uuid;
  context.transaction_uuid.canonical = database.transaction_uuid;
  context.local_transaction_id = database.local_transaction_id;
  context.snapshot_visible_through_local_transaction_id =
      database.local_transaction_id;
  context.security_context_present = true;
  context.principal_uuid.canonical =
      "018f0000-0000-7000-8000-00000000ae10";
  return context;
}

agents::DurableAgentCatalogImage CatalogImage() {
  agents::DurableAgentCatalogImage image;
  agents::AgentInstanceRecord instance;
  instance.instance_uuid = "018f0000-0000-7000-8000-00000000ae11";
  instance.agent_type_id = "node_resource_agent";
  instance.policy_uuid = "018f0000-0000-7000-8000-00000000ae12";
  instance.scope = "node";
  instance.state = agents::AgentLifecycleState::registered;
  instance.run_generation = 1;
  instance.policy_generation = 1;
  instance.instance_generation = 1;
  image.instances.push_back(instance);
  return image;
}

std::string RewriteHeaderSchemaVersion(std::string encoded,
                                        const std::string& replacement) {
  const std::string token = "schema_version=1";
  const auto pos = encoded.find(token);
  Require(pos != std::string::npos,
          "schema version token missing from durable catalog header");
  encoded.replace(pos, token.size(), "schema_version=" + replacement);
  return encoded;
}

std::string FieldValue(const std::vector<std::pair<std::string, std::string>>& values,
                       const std::string& field) {
  for (const auto& [name, value] : values) {
    if (name == field) { return value; }
  }
  return {};
}

api::CrudTableRecord FindCatalogTable(const api::CrudState& state) {
  for (const auto& table : state.tables) {
    if (table.default_name == api::kAgentDurableCatalogStoreTableName) {
      return table;
    }
  }
  Fail("agent durable catalog table not found");
}

api::CrudRowVersionRecord FindCatalogRootRow(const api::CrudState& state,
                                             const std::string& table_uuid) {
  api::CrudRowVersionRecord latest;
  for (const auto& row : state.row_versions) {
    if (row.deleted || row.table_uuid != table_uuid ||
        row.row_uuid != "agent-catalog-runtime-root") {
      continue;
    }
    if (FieldValue(row.values, "record_kind") != "agent_catalog_image") {
      continue;
    }
    if (latest.sequence == 0 || row.sequence > latest.sequence) {
      latest = row;
    }
  }
  Require(latest.sequence != 0, "agent durable catalog root row not found");
  return latest;
}

void TestPersistLoadRoundTrip() {
  auto database = CreateActiveDatabase();
  auto context = Context(database);

  api::AgentDurableCatalogStoreRequest request;
  request.context = context;
  request.image = CatalogImage();
  request.evidence_uuid = "018f0000-0000-7000-8000-00000000ae13";
  request.production_live_path = true;
  request.fsync_or_checkpoint_evidence = true;

  const auto persisted = api::PersistAgentDurableCatalogImage(request);
  Require(persisted.ok, "durable catalog MGA persist failed: " +
                            persisted.diagnostic.detail);
  Require(!persisted.table_uuid.empty(), "persisted table UUID missing");
  Require(persisted.row_event_sequence != 0, "row event sequence missing");
  Require(!persisted.storage_linkage_digest.empty(),
          "storage linkage digest missing");
  Require(agents::ValidateDurableAgentCatalogForProduction(persisted.image).ok,
          "persisted image does not validate as production durable catalog");

  const auto loaded = api::LoadAgentDurableCatalogImage(context, true);
  Require(loaded.ok, "durable catalog MGA load failed: " +
                         loaded.diagnostic.detail);
  Require(loaded.image.authority.catalog_root_digest ==
              persisted.image.authority.catalog_root_digest,
          "loaded root digest mismatch");
  Require(loaded.image.instances.size() == 1,
          "loaded catalog instance count mismatch");
  Require(loaded.image.authority.storage_catalog_record_evidence,
          "loaded catalog lacks storage record evidence");

  Cleanup(database.path);
}

void TestLoadMigratesAndPersistsOldSchemaImage() {
  auto database = CreateActiveDatabase();
  auto context = Context(database);

  api::AgentDurableCatalogStoreRequest request;
  request.context = context;
  request.image = CatalogImage();
  request.evidence_uuid = "018f0000-0000-7000-8000-00000000ae16";
  request.production_live_path = true;
  request.fsync_or_checkpoint_evidence = true;

  const auto persisted = api::PersistAgentDurableCatalogImage(request);
  Require(persisted.ok, "initial durable catalog persist failed: " +
                            persisted.diagnostic.detail);

  auto loaded_state = api::LoadMgaRelationStoreState(context);
  Require(loaded_state.ok, "MGA relation store load failed before migration seed");
  const auto crud_state =
      api::BuildCrudCompatibilityStateFromMga(loaded_state.state);
  const auto table = FindCatalogTable(crud_state);
  const auto current = FindCatalogRootRow(crud_state, table.table_uuid);

  const std::string old_encoded =
      RewriteHeaderSchemaVersion(
          agents::SerializeDurableAgentCatalogImage(persisted.image),
          "0");
  const auto old_validation =
      agents::ValidateDurableAgentCatalogImage(old_encoded, true);
  Require(old_validation.status.ok && old_validation.migrated,
          "old schema fixture did not migrate before store seed");

  api::CrudRowVersionRecord old_row;
  old_row.creator_tx = context.local_transaction_id;
  old_row.table_uuid = table.table_uuid;
  old_row.row_uuid = "agent-catalog-runtime-root";
  old_row.version_uuid = api::GenerateCrudEngineUuid("row");
  old_row.previous_version_uuid = current.version_uuid;
  old_row.previous_sequence = current.sequence;
  old_row.values = {{"record_kind", "agent_catalog_image"},
                    {"catalog_root_digest",
                     persisted.image.authority.catalog_root_digest},
                    {"encoded_catalog_image", old_encoded},
                    {"catalog_generation",
                     std::to_string(persisted.image.authority.catalog_generation)},
                    {"authority_evidence_uuid",
                     persisted.image.authority.evidence_uuid},
                    {"storage_commit_evidence_uuid",
                     persisted.image.authority.storage_commit_evidence_uuid},
                    {"storage_linkage_digest",
                     "legacy-schema-seed-bound-to-current-root"}};

  std::uint64_t old_event_sequence = 0;
  const auto appended =
      api::AppendMgaRowVersion(context, old_row, &old_event_sequence);
  Require(!appended.error, "old schema catalog row append failed: " +
                               appended.detail);
  Require(old_event_sequence > persisted.row_event_sequence,
          "old schema row did not become latest catalog image");

  api::AgentDurableCatalogLoadRequest load;
  load.context = context;
  load.production_live_path = true;
  load.persist_schema_migration = true;
  load.fsync_or_checkpoint_evidence = true;
  load.migration_evidence_uuid =
      "018f0000-0000-7000-8000-00000000ae17";
  const auto migrated = api::LoadAgentDurableCatalogImage(load);
  Require(migrated.ok, "durable catalog load/migrate/persist failed: " +
                           migrated.diagnostic.detail);
  Require(migrated.schema_migration_applied,
          "durable catalog store did not report schema migration");
  Require(migrated.schema_migration_persisted,
          "durable catalog store did not persist migrated schema");
  Require(migrated.image.schema_version == 1,
          "persisted migrated catalog schema is not current");
  Require(migrated.image.migrations.size() == 1,
          "persisted migrated catalog missing migration ledger row");
  Require(migrated.image.migrations.front().from_schema_version == 0,
          "persisted migration ledger source schema mismatch");
  Require(migrated.image.migrations.front().to_schema_version == 1,
          "persisted migration ledger target schema mismatch");

  const auto reloaded = api::LoadAgentDurableCatalogImage(context, true);
  Require(reloaded.ok, "reloaded migrated durable catalog failed: " +
                          reloaded.diagnostic.detail);
  Require(!reloaded.schema_migration_applied,
          "migrated catalog still required migration after persistence");
  Require(reloaded.image.schema_version == 1,
          "reloaded migrated catalog schema is not current");
  Require(reloaded.image.migrations.size() == 1,
          "migration ledger did not survive store reload");
  Require(reloaded.image.authority.catalog_root_digest ==
              migrated.image.authority.catalog_root_digest,
          "reloaded migrated catalog root digest mismatch");
  Require(agents::ValidateDurableAgentCatalogForProduction(reloaded.image).ok,
          "reloaded migrated catalog failed production validation");

  Cleanup(database.path);
}

void TestRefusals() {
  auto database = CreateActiveDatabase();
  auto context = Context(database);

  api::AgentDurableCatalogStoreRequest missing_fsync;
  missing_fsync.context = context;
  missing_fsync.image = CatalogImage();
  missing_fsync.evidence_uuid = "018f0000-0000-7000-8000-00000000ae14";
  missing_fsync.production_live_path = true;
  missing_fsync.fsync_or_checkpoint_evidence = false;
  Require(!api::PersistAgentDurableCatalogImage(missing_fsync).ok,
          "production persist accepted missing fsync/checkpoint evidence");

  api::AgentDurableCatalogStoreRequest missing_tx = missing_fsync;
  missing_tx.fsync_or_checkpoint_evidence = true;
  missing_tx.context.local_transaction_id = 0;
  Require(!api::PersistAgentDurableCatalogImage(missing_tx).ok,
          "production persist accepted missing MGA transaction context");

  Cleanup(database.path);
}

}  // namespace

int main() {
  TestPersistLoadRoundTrip();
  TestLoadMigratesAndPersistsOldSchemaImage();
  TestRefusals();
  return EXIT_SUCCESS;
}

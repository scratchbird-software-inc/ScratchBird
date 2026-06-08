// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "agents/agent_durable_catalog_store_api.hpp"
#include "agents/agent_local_workflow_store_api.hpp"

#include "agent_local_workflow.hpp"
#include "database_lifecycle.hpp"
#include "local_transaction_store.hpp"
#include "transaction_inventory.hpp"
#include "uuid.hpp"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>

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

TestDatabase CreateActiveDatabase(const char* basename) {
  const std::filesystem::path path =
      std::filesystem::temp_directory_path() / basename;
  Cleanup(path);

  const auto database_uuid = uuid::GenerateEngineIdentityV7(
      UuidKind::database, 1800000001001ull);
  const auto filespace_uuid = uuid::GenerateEngineIdentityV7(
      UuidKind::filespace, 1800000001002ull);
  Require(database_uuid.ok(), "database UUID generation failed");
  Require(filespace_uuid.ok(), "filespace UUID generation failed");

  db::DatabaseCreateConfig create;
  create.path = path.string();
  create.database_uuid = database_uuid.value;
  create.filespace_uuid = filespace_uuid.value;
  create.page_size = 16384;
  create.creation_unix_epoch_millis = 1800000001003ull;
  create.allow_minimal_resource_bootstrap = true;
  create.require_resource_seed_pack = false;
  create.allow_overwrite = true;
  Require(db::CreateDatabaseFile(create).ok(), "database creation failed");

  auto inventory = mga::MakeEmptyLocalTransactionInventory();
  const auto transaction_uuid = uuid::GenerateEngineIdentityV7(
      UuidKind::transaction, 1800000001004ull);
  Require(transaction_uuid.ok(), "transaction UUID generation failed");
  auto begun = mga::BeginLocalTransaction(std::move(inventory),
                                          transaction_uuid.value,
                                          1800000001005ull);
  Require(begun.ok(), "local transaction begin failed");
  Require(db::PersistLocalTransactionInventoryToDatabase(path.string(),
                                                         begun.inventory)
              .ok(),
          "local transaction inventory persist failed");

  TestDatabase result;
  result.path = path;
  result.database_uuid = uuid::UuidToString(database_uuid.value.value);
  result.transaction_uuid = uuid::UuidToString(transaction_uuid.value.value);
  result.local_transaction_id = begun.entry.identity.local_id.value;
  return result;
}

api::EngineRequestContext Context(const TestDatabase& database) {
  api::EngineRequestContext context;
  context.request_id = "aeic-local-workflow-store";
  context.database_path = database.path.string();
  context.database_uuid.canonical = database.database_uuid;
  context.transaction_uuid.canonical = database.transaction_uuid;
  context.local_transaction_id = database.local_transaction_id;
  context.snapshot_visible_through_local_transaction_id =
      database.local_transaction_id;
  context.security_context_present = true;
  context.principal_uuid.canonical =
      "018f0000-0000-7000-8000-00000000de10";
  return context;
}

void SeedCatalog(const api::EngineRequestContext& context) {
  agents::DurableAgentCatalogImage image;
  api::AgentDurableCatalogStoreRequest seed;
  seed.context = context;
  seed.image = image;
  seed.evidence_uuid = "018f0000-0000-7000-8000-00000000de11";
  seed.production_live_path = true;
  seed.fsync_or_checkpoint_evidence = true;
  Require(api::PersistAgentDurableCatalogImage(seed).ok,
          "initial workflow catalog seed failed");
}

agents::AgentLocalWorkflowRequest BackupWorkflow(
    const api::EngineRequestContext& context,
    std::string idempotency_key) {
  agents::AgentLocalWorkflowRequest request;
  request.domain = agents::AgentLocalWorkflowDomain::backup;
  request.operation_id = "start_backup";
  request.idempotency_key = std::move(idempotency_key);
  request.authority.database_uuid = context.database_uuid.canonical;
  request.authority.principal_uuid =
      "018f0000-0000-7000-8000-00000000de20";
  request.authority.subject_uuid = "018f0000-0000-7000-8000-00000000de21";
  request.authority.mga_transaction_uuid = context.transaction_uuid.canonical;
  request.authority.evidence_uuid = "018f0000-0000-7000-8000-00000000de22";
  request.authority.local_transaction_id = context.local_transaction_id;
  request.authority.catalog_generation = 3;
  request.authority.durable_catalog_bound = true;
  request.authority.transaction_inventory_bound = true;
  request.authority.storage_snapshot_authoritative = true;
  request.authority.metadata_authoritative = true;
  request.subsystem_precondition_satisfied = true;
  request.intended_state_observed = true;
  return request;
}

void TestLocalWorkflowPersistsAndReplaysFromStore() {
  const auto database = CreateActiveDatabase(
      "scratchbird_aeic_local_workflow_store.sbdb");
  const auto context = Context(database);
  SeedCatalog(context);

  api::AgentLocalWorkflowStoreRequest request;
  request.context = context;
  request.workflow = BackupWorkflow(context, "idem-workflow-store");
  request.production_live_path = true;
  request.fsync_or_checkpoint_evidence = true;

  const auto first = api::ApplyAgentLocalWorkflowWithDurableCatalogStore(request);
  Require(first.workflow.ok, "store-backed local workflow failed: " +
                                first.workflow.status.diagnostic_code);
  Require(first.loaded_from_store && first.persisted_to_store,
          "local workflow did not load/persist durable catalog");

  const auto loaded = api::LoadAgentDurableCatalogImage(context, true);
  Require(loaded.ok, "catalog reload after local workflow failed");
  Require(loaded.image.actions.size() == 1,
          "local workflow action was not persisted");
  Require(loaded.image.evidence.size() == 1,
          "local workflow evidence was not persisted");
  Require(loaded.image.actions.front().actuator_provider_id ==
              "agent_local_workflow:backup",
          "local workflow provider id mismatch");

  const auto duplicate = api::ApplyAgentLocalWorkflowWithDurableCatalogStore(request);
  Require(duplicate.workflow.ok && duplicate.workflow.idempotent,
          "local workflow did not replay from durable catalog action");
  Require(!duplicate.persisted_to_store,
          "idempotent local workflow replay wrote a new durable record");

  const auto replay_loaded = api::LoadAgentDurableCatalogImage(context, true);
  Require(replay_loaded.ok, "catalog reload after local workflow replay failed");
  Require(replay_loaded.image.actions.size() == 1,
          "local workflow replay duplicated durable action records");

  Cleanup(database.path);
}

void TestLocalWorkflowRequiresCheckpointEvidence() {
  const auto database = CreateActiveDatabase(
      "scratchbird_aeic_local_workflow_checkpoint.sbdb");
  const auto context = Context(database);
  SeedCatalog(context);

  api::AgentLocalWorkflowStoreRequest request;
  request.context = context;
  request.workflow = BackupWorkflow(context, "idem-missing-checkpoint");
  request.production_live_path = true;
  request.fsync_or_checkpoint_evidence = false;
  const auto result = api::ApplyAgentLocalWorkflowWithDurableCatalogStore(request);
  Require(!result.workflow.ok &&
              result.workflow.status.diagnostic_code ==
                  "SB_AGENT_LOCAL_WORKFLOW_STORE.PERSIST_FAILED",
          "local workflow store accepted missing checkpoint evidence");

  const auto loaded = api::LoadAgentDurableCatalogImage(context, true);
  Require(loaded.ok, "catalog reload after local workflow refusal failed");
  Require(loaded.image.actions.empty(),
          "failed local workflow persist mutated durable catalog");

  Cleanup(database.path);
}

}  // namespace

int main() {
  TestLocalWorkflowPersistsAndReplaysFromStore();
  TestLocalWorkflowRequiresCheckpointEvidence();
  return EXIT_SUCCESS;
}

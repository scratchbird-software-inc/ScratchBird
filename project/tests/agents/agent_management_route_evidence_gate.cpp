// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "agents/agent_management_api.hpp"
#include "agents/agent_durable_catalog_store_api.hpp"

#include "database_lifecycle.hpp"
#include "local_transaction_store.hpp"
#include "transaction_inventory.hpp"
#include "uuid.hpp"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace agents = scratchbird::core::agents;
namespace api = scratchbird::engine::internal_api;
namespace db = scratchbird::storage::database;
namespace mga = scratchbird::transaction::mga;
namespace uuid = scratchbird::core::uuid;

namespace {

void Require(bool condition, const std::string& message) {
  if (!condition) { throw std::runtime_error(message); }
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

TestDatabase CreateActiveDatabase(const char* basename,
                                  std::uint64_t timestamp_base) {
  const std::filesystem::path path =
      std::filesystem::temp_directory_path() / basename;
  Cleanup(path);

  const auto database_uuid = uuid::GenerateEngineIdentityV7(
      scratchbird::core::platform::UuidKind::database,
      timestamp_base + 1);
  const auto filespace_uuid = uuid::GenerateEngineIdentityV7(
      scratchbird::core::platform::UuidKind::filespace,
      timestamp_base + 2);
  Require(database_uuid.ok(), "database UUID generation failed");
  Require(filespace_uuid.ok(), "filespace UUID generation failed");

  db::DatabaseCreateConfig create;
  create.path = path.string();
  create.database_uuid = database_uuid.value;
  create.filespace_uuid = filespace_uuid.value;
  create.page_size = 16384;
  create.creation_unix_epoch_millis = timestamp_base + 3;
  create.allow_minimal_resource_bootstrap = true;
  create.require_resource_seed_pack = false;
  create.allow_overwrite = true;
  Require(db::CreateDatabaseFile(create).ok(), "database creation failed");

  auto inventory = mga::MakeEmptyLocalTransactionInventory();
  const auto transaction_uuid = uuid::GenerateEngineIdentityV7(
      scratchbird::core::platform::UuidKind::transaction,
      timestamp_base + 4);
  Require(transaction_uuid.ok(), "transaction UUID generation failed");
  auto begun = mga::BeginLocalTransaction(std::move(inventory),
                                          transaction_uuid.value,
                                          timestamp_base + 5);
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

api::EngineRequestContext Context(std::initializer_list<std::string_view> rights) {
  api::EngineRequestContext context;
  context.request_id = "arhc-092-agent-management-route-evidence";
  context.database_uuid.canonical = "019f0920-0000-7000-8000-000000000001";
  context.principal_uuid.canonical = "019f0920-0000-7000-8000-000000000002";
  context.transaction_uuid.canonical = "019f0920-0000-7000-8000-000000000003";
  context.session_uuid.canonical = "019f0920-0000-7000-8000-000000000004";
  context.node_uuid.canonical = "019f0920-0000-7000-8000-000000000005";
  context.local_transaction_id = 92092;
  context.security_context_present = true;
  context.catalog_generation_id = 92;
  context.security_epoch = 93;
  context.resource_epoch = 94;
  for (const auto right : rights) {
    context.trace_tags.push_back("right:" + std::string(right));
  }
  return context;
}

api::EngineRequestContext StoreContext(
    const TestDatabase& database,
    std::initializer_list<std::string_view> rights) {
  auto context = Context(rights);
  context.database_path = database.path.string();
  context.database_uuid.canonical = database.database_uuid;
  context.transaction_uuid.canonical = database.transaction_uuid;
  context.local_transaction_id = database.local_transaction_id;
  return context;
}

agents::DurableAgentCatalogImage DurableCatalog() {
  agents::DurableAgentCatalogImage image;
  image.source = agents::AgentCatalogStateSource::durable_catalog_image;
  image.schema_version = 1;
  image.authority.durable_catalog_authority = true;
  image.authority.mga_transaction_evidence = true;
  image.authority.mga_transaction_uuid =
      "019f0920-0000-7000-8000-000000000010";
  image.authority.transaction_generation = 92;
  image.authority.evidence_uuid = "019f0920-0000-7000-8000-000000000011";
  image.authority.database_uuid = "019f0920-0000-7000-8000-000000000012";
  image.authority.catalog_storage_uuid = "019f0920-0000-7000-8000-000000000013";
  image.authority.storage_commit_evidence_uuid = image.authority.evidence_uuid;
  image.authority.catalog_generation = 1;
  image.authority.local_transaction_id = 9292;
  image.authority.storage_catalog_record_evidence = true;
  image.authority.transaction_inventory_bound = true;
  image.authority.fsync_or_checkpoint_evidence = true;

  agents::AgentInstanceRecord instance;
  instance.instance_uuid = "019f0920-0000-7000-8000-000000000020";
  instance.agent_type_id = "storage_health_manager";
  instance.policy_uuid = "019f0920-0000-7000-8000-000000000021";
  instance.scope = "node/database/filespace";
  instance.state = agents::AgentLifecycleState::running;
  instance.policy_generation = 23;
  instance.instance_generation = 42;
  instance.crash_loop_count = 2;
  instance.supervision_failure_count = 3;
  instance.restart_not_before_microseconds = 444;
  instance.cooldown_until_microseconds = 555;
  image.instances.push_back(instance);

  auto policy = agents::BaselinePolicyForAgent(*agents::FindAgentType(instance.agent_type_id));
  policy.policy_uuid = instance.policy_uuid;
  policy.policy_name = "route evidence policy";
  policy.policy_generation = instance.policy_generation;
  image.policies.push_back(policy);

  agents::DurableAgentHealthRecord health;
  health.instance_uuid = instance.instance_uuid;
  health.health_state = "degraded";
  health.diagnostic_code = "SB_AGENT_ROUTE.TEST_DIAGNOSTIC";
  health.evidence_uuid = "019f0920-0000-7000-8000-000000000022";
  health.observed_at_microseconds = 100;
  image.health.push_back(health);

  agents::DurableAgentLeaseRecord lease;
  lease.lease_uuid = "019f0920-0000-7000-8000-000000000030";
  lease.instance_uuid = instance.instance_uuid;
  lease.owner_uuid = "019f0920-0000-7000-8000-000000000031";
  lease.state = agents::DurableAgentLeaseState::acquired;
  lease.evidence_uuid = "019f0920-0000-7000-8000-000000000032";
  image.leases.push_back(lease);

  agents::DurableAgentActionRecord pending;
  pending.action_uuid = "019f0920-0000-7000-8000-000000000040";
  pending.instance_uuid = instance.instance_uuid;
  pending.operation_id = "storage_health_summary";
  pending.state = agents::DurableAgentActionState::pending;
  pending.evidence_uuid = "019f0920-0000-7000-8000-000000000041";
  image.actions.push_back(pending);

  agents::DurableAgentActionRecord running;
  running.action_uuid = "019f0920-0000-7000-8000-000000000042";
  running.instance_uuid = instance.instance_uuid;
  running.operation_id = "update_storage_cost";
  running.state = agents::DurableAgentActionState::running;
  running.evidence_uuid = "019f0920-0000-7000-8000-000000000043";
  image.actions.push_back(running);

  agents::DurableAgentActionRecord quarantined;
  quarantined.action_uuid = "019f0920-0000-7000-8000-000000000044";
  quarantined.instance_uuid = instance.instance_uuid;
  quarantined.operation_id = "forbidden_security_update";
  quarantined.state = agents::DurableAgentActionState::quarantined;
  quarantined.evidence_uuid = "019f0920-0000-7000-8000-000000000045";
  image.actions.push_back(quarantined);
  const auto refresh =
      agents::RefreshDurableAgentCatalogAuthorityDigest(&image,
                                                        image.authority.evidence_uuid);
  Require(refresh.ok, "fixture durable catalog root digest failed");
  return image;
}

void SeedCatalog(const api::EngineRequestContext& context) {
  api::AgentDurableCatalogStoreRequest seed;
  seed.context = context;
  seed.image = DurableCatalog();
  seed.evidence_uuid = "019f0920-0000-7000-8000-000000000050";
  seed.production_live_path = true;
  seed.fsync_or_checkpoint_evidence = true;
  const auto persisted = api::PersistAgentDurableCatalogImage(seed);
  Require(persisted.ok,
          "route-evidence durable catalog seed failed: " +
              persisted.diagnostic.detail);
}

std::string FieldValue(const api::EngineApiResult& result,
                       std::string_view field) {
  for (const auto& row : result.result_shape.rows) {
    for (const auto& [name, value] : row.fields) {
      if (name == field) { return value.encoded_value; }
    }
  }
  return {};
}

bool HasEvidence(const api::EngineApiResult& result, std::string_view kind) {
  for (const auto& evidence : result.evidence) {
    if (evidence.evidence_kind == kind) { return true; }
  }
  return false;
}

void TestDurableSysAgentRouteEvidenceAndRedaction() {
  const auto database = CreateActiveDatabase(
      "scratchbird_aeic_agent_management_route_sys.sbdb",
      1800000005000ull);
  const auto context = StoreContext(database, {"OBS_AGENT_STATE_READ"});
  SeedCatalog(context);
  api::EngineSysAgentsRequest request;
  request.context = context;
  request.option_envelopes.push_back("agent_management_production_live:true");
  request.durable_runtime_state.production_live_path = true;

  const auto result = api::EngineSysAgents(request);
  Require(result.ok, "sys.agents durable route evidence failed");
  Require(FieldValue(result, "runtime_state_source") == "durable_runtime_catalog",
          "sys.agents did not use durable runtime state");
  Require(FieldValue(result, "health_state") == "degraded",
          "health state missing from durable projection");
  Require(FieldValue(result, "policy_generation") == "23",
          "policy generation missing from durable projection");
  Require(FieldValue(result, "queue_depth") == "3",
          "queue depth did not include lease plus action backlog");
  Require(FieldValue(result, "action_backlog") == "2",
          "action backlog did not count pending/running actions");
  Require(FieldValue(result, "failure_count") == "5",
          "failure count did not include crash and supervision failures");
  Require(FieldValue(result, "quarantine_count") == "1",
          "quarantine count did not include quarantined action");
  Require(FieldValue(result, "retry_not_before") == "555",
          "retry timer did not report max cooldown/restart timer");
  Require(FieldValue(result, "last_decision") ==
              "forbidden_security_update:quarantined",
          "last decision did not reflect durable action state");
  Require(FieldValue(result, "last_evidence_uuid") ==
              "019f0920-0000-7000-8000-000000000022",
          "last evidence UUID did not come from durable health evidence");
  Require(FieldValue(result, "last_diagnostic_code") == "redacted",
          "diagnostic was not redacted without evidence right");
  Require(FieldValue(result, "diagnostic_redaction_state") == "redacted",
          "diagnostic redaction state missing");
  Require(HasEvidence(result, "agent_durable_runtime_catalog"),
          "durable route evidence missing");
  Cleanup(database.path);
}

void TestDurableShowAgentDiagnosticWithEvidenceRight() {
  const auto database = CreateActiveDatabase(
      "scratchbird_aeic_agent_management_route_show.sbdb",
      1800000005100ull);
  const auto context = StoreContext(
      database,
      {"OBS_AGENT_STATE_READ", "OBS_AGENT_EVIDENCE_READ"});
  SeedCatalog(context);
  api::EngineShowAgentRequest request;
  request.context = context;
  request.target_object.object_kind = "storage_health_manager";
  request.option_envelopes.push_back("agent_management_production_live:true");
  request.durable_runtime_state.production_live_path = true;

  const auto result = api::EngineShowAgent(request);
  Require(result.ok, "agents.show durable route evidence failed");
  Require(FieldValue(result, "last_diagnostic_code") ==
              "SB_AGENT_ROUTE.TEST_DIAGNOSTIC",
          "diagnostic was not visible with evidence right");
  Require(FieldValue(result, "diagnostic_redaction_state") == "visible",
          "visible diagnostic state missing");
  Require(FieldValue(result, "overhead_budget_units") == "9",
          "overhead budget did not expose queue/failure/quarantine pressure");
  Cleanup(database.path);
}

}  // namespace

int main() {
  try {
    TestDurableSysAgentRouteEvidenceAndRedaction();
    TestDurableShowAgentDiagnosticWithEvidenceRight();
  } catch (const std::exception& e) {
    std::cerr << e.what() << '\n';
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}

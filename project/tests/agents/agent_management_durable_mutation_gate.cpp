// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "agents/agent_management_api.hpp"
#include "agents/agent_durable_catalog_store_api.hpp"
#include "agent_runtime.hpp"
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

namespace agents = scratchbird::core::agents;
namespace api = scratchbird::engine::internal_api;
namespace db = scratchbird::storage::database;
namespace mga = scratchbird::transaction::mga;
namespace uuid = scratchbird::core::uuid;
using scratchbird::core::platform::UuidKind;

void Require(bool condition, const std::string& message) {
  if (!condition) { throw std::runtime_error(message); }
}

api::EngineRequestContext Context(std::initializer_list<std::string_view> rights) {
  api::EngineRequestContext context;
  context.request_id = "arhc-042-agent-management-durable-mutation";
  context.database_uuid.canonical = "019f0300-0000-7000-8000-000000000001";
  context.principal_uuid.canonical = "019f0300-0000-7000-8000-000000000002";
  context.transaction_uuid.canonical = "019f0300-0000-7000-8000-000000000003";
  context.session_uuid.canonical = "019f0300-0000-7000-8000-000000000004";
  context.node_uuid.canonical = "019f0300-0000-7000-8000-000000000005";
  context.local_transaction_id = 42042;
  context.security_context_present = true;
  context.catalog_generation_id = 11;
  context.security_epoch = 12;
  context.resource_epoch = 13;
  for (const auto right : rights) {
    context.trace_tags.push_back("right:" + std::string(right));
  }
  return context;
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
      "scratchbird_aeic_agent_management_store.sbdb";
  Cleanup(path);

  const auto database_uuid = uuid::GenerateEngineIdentityV7(UuidKind::database,
                                                            1790000000101);
  const auto filespace_uuid = uuid::GenerateEngineIdentityV7(UuidKind::filespace,
                                                             1790000000102);
  Require(database_uuid.ok(), "database UUID generation failed");
  Require(filespace_uuid.ok(), "filespace UUID generation failed");

  db::DatabaseCreateConfig create;
  create.path = path.string();
  create.database_uuid = database_uuid.value;
  create.filespace_uuid = filespace_uuid.value;
  create.page_size = 16384;
  create.creation_unix_epoch_millis = 1790000000103;
  create.allow_minimal_resource_bootstrap = true;
  create.require_resource_seed_pack = false;
  create.allow_overwrite = true;
  const auto created = db::CreateDatabaseFile(create);
  Require(created.ok(), "database creation failed");

  auto inventory = mga::MakeEmptyLocalTransactionInventory();
  const auto transaction_uuid = uuid::GenerateEngineIdentityV7(UuidKind::transaction,
                                                              1790000000104);
  Require(transaction_uuid.ok(), "transaction UUID generation failed");
  auto begun = mga::BeginLocalTransaction(std::move(inventory),
                                          transaction_uuid.value,
                                          1790000000105);
  Require(begun.ok(), "local transaction begin failed");
  const auto persisted =
      db::PersistLocalTransactionInventoryToDatabase(path.string(),
                                                     begun.inventory);
  Require(persisted.ok(), "local transaction inventory persist failed");

  TestDatabase result;
  result.path = path;
  result.database_uuid = uuid::UuidToString(database_uuid.value.value);
  result.transaction_uuid = uuid::UuidToString(transaction_uuid.value.value);
  result.local_transaction_id = begun.entry.identity.local_id.value;
  return result;
}

api::EngineRequestContext StoreContext(
    const TestDatabase& database,
    std::initializer_list<std::string_view> rights) {
  auto context = Context(rights);
  context.database_path = database.path.string();
  context.database_uuid.canonical = database.database_uuid;
  context.transaction_uuid.canonical = database.transaction_uuid;
  context.local_transaction_id = database.local_transaction_id;
  context.snapshot_visible_through_local_transaction_id =
      database.local_transaction_id;
  return context;
}

agents::DurableAgentCatalogImage DurableCatalog(
    agents::AgentLifecycleState state = agents::AgentLifecycleState::registered) {
  agents::DurableAgentCatalogImage image;
  image.source = agents::AgentCatalogStateSource::durable_catalog_image;
  image.schema_version = 1;
  image.authority.durable_catalog_authority = true;
  image.authority.mga_transaction_evidence = true;
  image.authority.mga_transaction_uuid = "019f0300-0000-7000-8000-000000000010";
  image.authority.transaction_generation = 3;
  image.authority.evidence_uuid = "019f0300-0000-7000-8000-000000000011";
  image.authority.database_uuid = "019f0300-0000-7000-8000-000000000012";
  image.authority.catalog_storage_uuid = "019f0300-0000-7000-8000-000000000013";
  image.authority.storage_commit_evidence_uuid = image.authority.evidence_uuid;
  image.authority.catalog_generation = 1;
  image.authority.local_transaction_id = 3003;
  image.authority.storage_catalog_record_evidence = true;
  image.authority.transaction_inventory_bound = true;
  image.authority.fsync_or_checkpoint_evidence = true;

  const auto descriptor = agents::FindAgentType("storage_health_manager");
  Require(descriptor.has_value(), "storage_health_manager descriptor missing");

  agents::AgentInstanceRecord instance;
  instance.instance_uuid = "019f0300-0000-7000-8000-000000000020";
  instance.agent_type_id = descriptor->type_id;
  instance.policy_uuid = "019f0300-0000-7000-8000-000000000021";
  instance.scope = "database";
  instance.state = state;
  instance.policy_generation = 3;
  instance.instance_generation = 5;
  image.instances.push_back(instance);

  auto policy = agents::BaselinePolicyForAgent(*descriptor);
  policy.policy_uuid = instance.policy_uuid;
  policy.policy_name = "durable storage health policy";
  policy.policy_generation = instance.policy_generation;
  policy.scope = instance.scope;
  image.policies.push_back(policy);

  agents::DurableAgentHealthRecord health;
  health.instance_uuid = instance.instance_uuid;
  health.health_state = "degraded";
  health.diagnostic_code = "SB_AGENT_HEALTH.TEST_DURABLE_STATE";
  health.evidence_uuid = image.authority.evidence_uuid;
  health.observed_at_microseconds = 100;
  image.health.push_back(health);
  const auto refresh =
      agents::RefreshDurableAgentCatalogAuthorityDigest(&image,
                                                        image.authority.evidence_uuid);
  Require(refresh.ok, "fixture durable catalog root digest failed");
  return image;
}

bool HasDiagnostic(const api::EngineApiResult& result, std::string_view code) {
  for (const auto& diagnostic : result.diagnostics) {
    if (diagnostic.code == code) { return true; }
  }
  return false;
}

std::string DiagnosticCodes(const api::EngineApiResult& result) {
  std::string codes;
  for (const auto& diagnostic : result.diagnostics) {
    if (!codes.empty()) { codes += ","; }
    codes += diagnostic.code;
  }
  return codes;
}

bool HasEvidence(const api::EngineApiResult& result,
                 std::string_view kind,
                 std::string_view id = {}) {
  for (const auto& evidence : result.evidence) {
    if (evidence.evidence_kind == kind &&
        (id.empty() || evidence.evidence_id == id)) {
      return true;
    }
  }
  return false;
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

template <typename TRequest>
void AddObservedMetricSnapshotEvidence(TRequest* request) {
  request->option_envelopes.push_back("agent_metric_snapshot_observed:true");
  request->option_envelopes.push_back("agent_metric_snapshot_trusted:true");
  request->option_envelopes.push_back(
      "agent_metric_snapshot_source_quality:trusted");
  request->option_envelopes.push_back(
      "agent_metric_snapshot_digest:sha256:arhc042-management");
  request->option_envelopes.push_back(
      "agent_metric_snapshot_id:metric-snapshot-arhc042");
  request->option_envelopes.push_back(
      "agent_metric_snapshot_evidence_uuid:019f0300-0000-7000-8000-000000000090");
}

void TestDurableReadProjection() {
  auto catalog = DurableCatalog(agents::AgentLifecycleState::paused);
  api::EngineListAgentsRequest list;
  list.context = Context({"OBS_AGENT_STATE_READ"});
  list.target_object.object_kind = "storage_health_manager";
  list.durable_runtime_state.catalog = &catalog;

  const auto result = api::EngineListAgents(list);
  Require(result.ok, "durable list projection failed");
  Require(FieldValue(result, "agent_type_id") == "storage_health_manager",
          "durable list did not project storage_health_manager");
  Require(FieldValue(result, "state") == "paused",
          "durable list did not project instance state");
  Require(FieldValue(result, "health_state") == "degraded",
          "durable list did not project durable health");
  Require(FieldValue(result, "runtime_state_source") ==
              "durable_runtime_catalog",
          "durable list did not mark runtime state source");
  Require(FieldValue(result, "durable_catalog_authority") == "true",
          "durable list did not surface durable authority");
  Require(HasEvidence(result, "agent_durable_runtime_catalog"),
          "durable list evidence missing");
}

void TestProductionMutationRequiresDurableCatalog() {
  api::EngineStartAgentRequest start;
  start.context = Context({"OBS_AGENT_STATE_READ", "OBS_AGENT_CONTROL"});
  start.target_object.object_kind = "storage_health_manager";
  start.option_envelopes.push_back("agent_management_production_live:true");
  start.option_envelopes.push_back("wall_now_us:200");
  start.durable_runtime_state.production_live_path = true;

  const auto result = api::EngineStartAgent(start);
  Require(!result.ok, "production mutation without durable catalog accepted");
  Require(HasDiagnostic(result, "SB_AGENT_MANAGEMENT.DURABLE_CATALOG_STORE_REQUIRED"),
          "missing durable catalog diagnostic drifted: " + DiagnosticCodes(result));
}

void TestDurableMutationUpdatesCatalog() {
  auto catalog = DurableCatalog(agents::AgentLifecycleState::registered);
  api::EngineStartAgentRequest start;
  start.context = Context({"OBS_AGENT_STATE_READ", "OBS_AGENT_CONTROL"});
  start.target_object.object_kind = "storage_health_manager";
  start.option_envelopes.push_back("wall_now_us:300");
  AddObservedMetricSnapshotEvidence(&start);
  start.durable_runtime_state.mutable_catalog = &catalog;

  const auto before_generation = catalog.authority.catalog_generation;
  const auto result = api::EngineStartAgent(start);
  Require(result.ok, "durable start mutation failed");
  Require(FieldValue(result, "runtime_state_source") == "durable_runtime_catalog",
          "durable mutation did not report durable source");
  Require(FieldValue(result, "durable_catalog_mutated") == "true",
          "durable mutation flag missing");
  Require(FieldValue(result, "sidecar_event_used") == "false",
          "durable mutation fell back to sidecar event");
  Require(HasEvidence(result, "agent_durable_runtime_catalog",
                      "storage_health_manager"),
          "durable mutation evidence missing");
  Require(catalog.instances.front().state == agents::AgentLifecycleState::running,
          "durable instance state was not updated");
  Require(catalog.instances.front().instance_generation == 6,
          "durable instance generation was not advanced");
  Require(catalog.instances.front().run_generation == 1,
          "durable run generation was not advanced");
  Require(catalog.evidence.size() == 1, "durable mutation evidence missing");
  Require(catalog.health.front().diagnostic_code ==
              "SB_AGENT_MANAGEMENT.DURABLE_STATE_UPDATED",
          "durable health diagnostic not updated");
  Require(!catalog.retained_history.empty(),
          "durable mutation history not retained");
  Require(catalog.authority.catalog_generation > before_generation,
          "durable mutation did not advance catalog generation");
  Require(!catalog.authority.previous_catalog_root_digest.empty(),
          "durable mutation did not retain previous catalog root");
  Require(catalog.authority.catalog_root_digest ==
              agents::DurableAgentCatalogRootDigest(catalog),
          "durable mutation did not refresh catalog root");
  Require(catalog.resource_reservations.size() == 1,
          "durable mutation resource reservation missing");
  Require(catalog.resource_reservations.front().state ==
              agents::DurableAgentResourceReservationState::released,
          "durable mutation resource reservation was not released");
}

void TestProductionMutationPersistsThroughMGAStore() {
  const auto database = CreateActiveDatabase();
  const auto context =
      StoreContext(database, {"OBS_AGENT_STATE_READ", "OBS_AGENT_CONTROL"});

  api::AgentDurableCatalogStoreRequest initial_store;
  initial_store.context = context;
  initial_store.image = DurableCatalog(agents::AgentLifecycleState::registered);
  initial_store.evidence_uuid = "019f0300-0000-7000-8000-000000000111";
  initial_store.production_live_path = true;
  initial_store.fsync_or_checkpoint_evidence = true;
  const auto initial_persist =
      api::PersistAgentDurableCatalogImage(initial_store);
  Require(initial_persist.ok,
          "initial durable management catalog persist failed: " +
              initial_persist.diagnostic.detail);

  api::EngineStartAgentRequest start;
  start.context = context;
  start.target_object.object_kind = "storage_health_manager";
  start.option_envelopes.push_back("agent_management_production_live:true");
  start.option_envelopes.push_back(
      "agent_durable_catalog_fsync_or_checkpoint_evidence:true");
  start.option_envelopes.push_back("wall_now_us:325");
  AddObservedMetricSnapshotEvidence(&start);
  start.durable_runtime_state.production_live_path = true;

  const auto result = api::EngineStartAgent(start);
  Require(result.ok, "store-backed durable start mutation failed: " +
                         DiagnosticCodes(result));
  Require(FieldValue(result, "runtime_state_source") == "durable_runtime_catalog",
          "store-backed mutation did not report durable source");
  Require(FieldValue(result, "durable_catalog_store_mutated") == "true",
          "store-backed mutation did not persist through catalog store");
  Require(FieldValue(result, "durable_resource_reservation") == "true",
          "store-backed mutation did not acquire resource reservation");
  Require(FieldValue(result, "durable_resource_reservation_released") == "true",
          "store-backed mutation did not release resource reservation");
  Require(FieldValue(result, "sidecar_event_used") == "false",
          "store-backed mutation fell back to sidecar event");

  const auto loaded = api::LoadAgentDurableCatalogImage(context, true);
  Require(loaded.ok, "durable management catalog reload failed: " +
                         loaded.diagnostic.detail);
  Require(!loaded.image.instances.empty(), "reloaded catalog has no instances");
  Require(loaded.image.instances.front().state ==
              agents::AgentLifecycleState::running,
          "reloaded durable catalog did not persist running state");
  Require(!loaded.image.evidence.empty(),
          "reloaded durable catalog did not persist management evidence");
  Require(!loaded.image.retained_history.empty(),
          "reloaded durable catalog did not persist management history");
  Require(loaded.image.resource_reservations.size() == 1,
          "reloaded durable catalog did not persist resource reservation");
  Require(loaded.image.resource_reservations.front().state ==
              agents::DurableAgentResourceReservationState::released,
          "reloaded durable resource reservation was not released");

  api::EngineListAgentsRequest list;
  list.context = StoreContext(database, {"OBS_AGENT_STATE_READ"});
  list.target_object.object_kind = "storage_health_manager";
  list.option_envelopes.push_back("agent_management_production_live:true");
  list.durable_runtime_state.production_live_path = true;
  const auto listed = api::EngineListAgents(list);
  Require(listed.ok, "store-backed durable list failed after reload");
  Require(FieldValue(listed, "state") == "running",
          "store-backed durable list did not reload running state");
  Require(FieldValue(listed, "runtime_state_source") ==
              "durable_runtime_catalog",
          "store-backed durable list did not use catalog store");

  Cleanup(database.path);
}

void TestProductionMutationRequiresObservedMetricSnapshot() {
  const auto database = CreateActiveDatabase();
  const auto context =
      StoreContext(database, {"OBS_AGENT_STATE_READ", "OBS_AGENT_CONTROL"});
  api::AgentDurableCatalogStoreRequest initial_store;
  initial_store.context = context;
  initial_store.image = DurableCatalog(agents::AgentLifecycleState::registered);
  initial_store.evidence_uuid = "019f0300-0000-7000-8000-000000000121";
  initial_store.production_live_path = true;
  initial_store.fsync_or_checkpoint_evidence = true;
  Require(api::PersistAgentDurableCatalogImage(initial_store).ok,
          "initial missing-metric catalog persist failed");

  api::EngineStartAgentRequest start;
  start.context = context;
  start.target_object.object_kind = "storage_health_manager";
  start.option_envelopes.push_back("agent_management_production_live:true");
  start.option_envelopes.push_back(
      "agent_durable_catalog_fsync_or_checkpoint_evidence:true");
  start.option_envelopes.push_back("wall_now_us:350");
  start.durable_runtime_state.production_live_path = true;

  const auto result = api::EngineStartAgent(start);
  Require(!result.ok, "production mutation without observed metrics accepted");
  Require(HasDiagnostic(
              result,
              "SB_AGENT_METRIC_SNAPSHOT.PRODUCTION_OBSERVED_SNAPSHOT_REQUIRED"),
          "missing observed metric diagnostic drifted: " +
              DiagnosticCodes(result));
  Cleanup(database.path);
}

void TestProductionMutationRejectsFixtureMode() {
  const auto database = CreateActiveDatabase();
  const auto context =
      StoreContext(database, {"OBS_AGENT_STATE_READ", "OBS_AGENT_CONTROL"});
  api::AgentDurableCatalogStoreRequest initial_store;
  initial_store.context = context;
  initial_store.image = DurableCatalog(agents::AgentLifecycleState::registered);
  initial_store.evidence_uuid = "019f0300-0000-7000-8000-000000000122";
  initial_store.production_live_path = true;
  initial_store.fsync_or_checkpoint_evidence = true;
  Require(api::PersistAgentDurableCatalogImage(initial_store).ok,
          "initial fixture-mode catalog persist failed");

  api::EngineStartAgentRequest start;
  start.context = context;
  start.target_object.object_kind = "storage_health_manager";
  start.option_envelopes.push_back("agent_management_production_live:true");
  start.option_envelopes.push_back(
      "agent_durable_catalog_fsync_or_checkpoint_evidence:true");
  start.option_envelopes.push_back("agent_fixture_mode:true");
  start.option_envelopes.push_back("wall_now_us:375");
  AddObservedMetricSnapshotEvidence(&start);
  start.durable_runtime_state.production_live_path = true;

  const auto result = api::EngineStartAgent(start);
  Require(!result.ok, "production mutation accepted fixture mode");
  Require(HasDiagnostic(
              result,
          "SB_AGENT_PRODUCTION_FIXTURE.TEST_FIXTURE_MODE_REFUSED"),
          "fixture-mode diagnostic drifted: " + DiagnosticCodes(result));
  Cleanup(database.path);
}

void TestProductionMutationRejectsUntrustedObservedMetricSnapshot() {
  const auto database = CreateActiveDatabase();
  const auto context =
      StoreContext(database, {"OBS_AGENT_STATE_READ", "OBS_AGENT_CONTROL"});
  api::AgentDurableCatalogStoreRequest initial_store;
  initial_store.context = context;
  initial_store.image = DurableCatalog(agents::AgentLifecycleState::registered);
  initial_store.evidence_uuid = "019f0300-0000-7000-8000-000000000123";
  initial_store.production_live_path = true;
  initial_store.fsync_or_checkpoint_evidence = true;
  Require(api::PersistAgentDurableCatalogImage(initial_store).ok,
          "initial untrusted-metric catalog persist failed");

  api::EngineStartAgentRequest start;
  start.context = context;
  start.target_object.object_kind = "storage_health_manager";
  start.option_envelopes.push_back("agent_management_production_live:true");
  start.option_envelopes.push_back(
      "agent_durable_catalog_fsync_or_checkpoint_evidence:true");
  start.option_envelopes.push_back("wall_now_us:390");
  AddObservedMetricSnapshotEvidence(&start);
  start.option_envelopes.push_back(
      "agent_metric_snapshot_trust_provenance:fixture_untrusted");
  start.durable_runtime_state.production_live_path = true;

  const auto result = api::EngineStartAgent(start);
  Require(!result.ok, "production mutation accepted untrusted observed metrics");
  Require(HasDiagnostic(result, "SB_AGENT_METRIC_SNAPSHOT.UNTRUSTED"),
          "untrusted observed metric diagnostic drifted: " +
              DiagnosticCodes(result));
  Cleanup(database.path);
}

void TestNonProductionLegacyCompatibility() {
  api::EngineDryRunAgentRequest dry_run;
  dry_run.context = Context({"OBS_AGENT_STATE_READ", "OBS_AGENT_CONTROL"});
  dry_run.target_object.object_kind = "storage_health_manager";
  dry_run.option_envelopes.push_back("wall_now_us:400");

  const auto result = api::EngineDryRunAgent(dry_run);
  Require(result.ok, "non-production dry run compatibility broke");
  Require(FieldValue(result, "runtime_state_source") == "legacy_sidecar_event",
          "legacy compatibility path source changed");
  Require(FieldValue(result, "sidecar_event_used") == "false",
          "dry-run compatibility path reported sidecar persistence");
}

int main() {
  try {
    TestDurableReadProjection();
    TestProductionMutationRequiresDurableCatalog();
    TestDurableMutationUpdatesCatalog();
    TestProductionMutationPersistsThroughMGAStore();
    TestProductionMutationRequiresObservedMetricSnapshot();
    TestProductionMutationRejectsFixtureMode();
    TestProductionMutationRejectsUntrustedObservedMetricSnapshot();
    TestNonProductionLegacyCompatibility();
  } catch (const std::exception& ex) {
    std::cerr << ex.what() << '\n';
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}

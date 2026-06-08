// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "agents/agent_durable_catalog_store_api.hpp"
#include "agents/agent_runtime_service_store_api.hpp"
#include "metric_registry.hpp"
#include "transaction/transaction_api.hpp"

#include "agent_durable_catalog.hpp"
#include "agent_runtime_manifest.hpp"
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

TestDatabase CreateActiveDatabase() {
  const std::filesystem::path path =
      std::filesystem::temp_directory_path() /
      "scratchbird_aeic_agent_runtime_service_store.sbdb";
  Cleanup(path);

  const auto database_uuid = uuid::GenerateEngineIdentityV7(UuidKind::database,
                                                            1790000000201);
  const auto filespace_uuid = uuid::GenerateEngineIdentityV7(UuidKind::filespace,
                                                             1790000000202);
  Require(database_uuid.ok(), "database UUID generation failed");
  Require(filespace_uuid.ok(), "filespace UUID generation failed");

  db::DatabaseCreateConfig create;
  create.path = path.string();
  create.database_uuid = database_uuid.value;
  create.filespace_uuid = filespace_uuid.value;
  create.page_size = 16384;
  create.creation_unix_epoch_millis = 1790000000203;
  create.allow_minimal_resource_bootstrap = true;
  create.require_resource_seed_pack = false;
  create.allow_overwrite = true;
  Require(db::CreateDatabaseFile(create).ok(), "database creation failed");

  auto inventory = mga::MakeEmptyLocalTransactionInventory();
  const auto transaction_uuid = uuid::GenerateEngineIdentityV7(UuidKind::transaction,
                                                              1790000000204);
  Require(transaction_uuid.ok(), "transaction UUID generation failed");
  auto begun = mga::BeginLocalTransaction(std::move(inventory),
                                          transaction_uuid.value,
                                          1790000000205);
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
  context.request_id = "aeic-runtime-service-store";
  context.database_path = database.path.string();
  context.database_uuid.canonical = database.database_uuid;
  context.transaction_uuid.canonical = database.transaction_uuid;
  context.local_transaction_id = database.local_transaction_id;
  context.snapshot_visible_through_local_transaction_id =
      database.local_transaction_id;
  context.security_context_present = true;
  context.principal_uuid.canonical =
      "018f0000-0000-7000-8000-00000000be10";
  context.session_uuid.canonical =
      "018f0000-0000-7000-8000-00000000be14";
  context.catalog_generation_id = 1;
  context.security_epoch = 1;
  context.resource_epoch = 1;
  context.name_resolution_epoch = 1;
  return context;
}

api::EngineRequestContext BeginTransactionContext(const TestDatabase& database,
                                                  std::string request_id) {
  api::EngineBeginTransactionRequest begin;
  begin.context = Context(database);
  begin.context.request_id = std::move(request_id);
  begin.context.transaction_uuid.canonical.clear();
  begin.context.local_transaction_id = 0;
  begin.context.snapshot_visible_through_local_transaction_id = 0;
  begin.isolation_level = "read_committed";
  begin.transaction_policy_profile.encoded_profiles.push_back("fail_closed:true");
  begin.transaction_policy_profile.encoded_profiles.push_back(
      "transaction_read_only:false");
  begin.transaction_policy_profile.encoded_profiles.push_back(
      "transaction_read_mode:read_write");
  const auto begun = api::EngineBeginTransaction(begin);
  Require(begun.ok && begun.local_transaction_id != 0,
          "engine transaction begin failed for service store gate");
  auto context = begin.context;
  context.transaction_uuid = begun.transaction_uuid;
  context.local_transaction_id = begun.local_transaction_id;
  context.snapshot_visible_through_local_transaction_id =
      begun.snapshot_visible_through_local_transaction_id;
  return context;
}

void CommitContext(api::EngineRequestContext context) {
  api::EngineCommitTransactionRequest commit;
  commit.context = std::move(context);
  const auto committed = api::EngineCommitTransaction(commit);
  Require(committed.ok, "engine transaction commit failed for service store gate");
}

void RollbackContext(api::EngineRequestContext context) {
  api::EngineRollbackTransactionRequest rollback;
  rollback.context = std::move(context);
  const auto rolled_back = api::EngineRollbackTransaction(rollback);
  Require(rolled_back.ok,
          "engine transaction rollback failed for service store gate");
}

bool MetricHasLabel(const scratchbird::core::metrics::MetricValue& value,
                    const std::string& key,
                    const std::string& expected) {
  for (const auto& label : value.labels) {
    if (label.key == key && label.value == expected) { return true; }
  }
  return false;
}

double CurrentMetricValue(const std::string& family,
                          const std::string& label_key = {},
                          const std::string& label_value = {}) {
  const auto snapshot =
      scratchbird::core::metrics::DefaultMetricRegistry().SnapshotCurrent(false);
  for (const auto& value : snapshot) {
    if (value.family != family) { continue; }
    if (!label_key.empty() && !MetricHasLabel(value, label_key, label_value)) {
      continue;
    }
    return value.value;
  }
  return -1.0;
}

agents::DurableAgentCatalogImage CatalogImage() {
  agents::DurableAgentCatalogImage image;
  agents::AgentInstanceRecord instance;
  instance.instance_uuid = "018f0000-0000-7000-8000-00000000be11";
  instance.agent_type_id = "node_resource_agent";
  instance.policy_uuid = "018f0000-0000-7000-8000-00000000be12";
  instance.scope = "node";
  instance.state = agents::AgentLifecycleState::registered;
  instance.run_generation = 1;
  instance.policy_generation = 1;
  instance.instance_generation = 1;
  image.instances.push_back(instance);
  return image;
}

agents::AgentPolicy SupervisionPolicy() {
  agents::AgentPolicy policy;
  policy.policy_uuid = "018f0000-0000-7000-8000-00000000be12";
  policy.policy_name = "runtime service supervision policy";
  policy.policy_family = "runtime_service_supervision";
  policy.scope = "node";
  policy.enabled = true;
  policy.lease_microseconds = 5000;
  policy.cooldown_microseconds = 200;
  policy.max_runtime_microseconds = 1000;
  policy.max_restart_attempts = 2;
  policy.initial_backoff_microseconds = 100;
  policy.max_backoff_microseconds = 400;
  return policy;
}

agents::DurableAgentCatalogImage CatalogImageWithRunningInstance() {
  auto image = CatalogImage();
  image.instances.front().state = agents::AgentLifecycleState::running;
  image.instances.front().lease_until_microseconds = 5000;
  image.instances.front().last_run_start_microseconds = 1000;
  image.policies.push_back(SupervisionPolicy());
  return image;
}

agents::DurableAgentCatalogImage CatalogImageWithQueuedAction() {
  auto image = CatalogImage();
  auto add_action = [&](const std::string& suffix,
                        agents::DurableAgentActionState state,
                        const std::string& diagnostic) {
    agents::DurableAgentActionRecord action;
    action.action_uuid = "018f0000-0000-7000-8000-00000000c0" + suffix;
    action.instance_uuid = "018f0000-0000-7000-8000-00000000be11";
    action.owner_uuid = "018f0000-0000-7000-8000-00000000be21";
    action.operation_id = "agent.test.durable_replay." + suffix;
    action.actuator_provider_id = "test_live_provider";
    action.state = state;
    action.idempotency_key = "agent.test.durable_replay/" + suffix;
    action.input_evidence_digest = "input-digest-" + suffix;
    action.evidence_uuid = "018f0000-0000-7000-8000-00000000c1" + suffix;
    action.diagnostic_code = diagnostic;
    action.generation = 1;
    action.outcome_verified = state == agents::DurableAgentActionState::completed;
    image.actions.push_back(std::move(action));
  };
  add_action("50",
             agents::DurableAgentActionState::running,
             "SB_AGENT_ACTION.RUNNING_BEFORE_RECOVERY");
  add_action("51",
             agents::DurableAgentActionState::pending,
             "SB_AGENT_ACTION.PENDING_BEFORE_RECOVERY");
  add_action("52",
             agents::DurableAgentActionState::completed,
             "SB_AGENT_ACTION.COMPLETED_BEFORE_RECOVERY");
  add_action("53",
             agents::DurableAgentActionState::cancelled,
             "SB_AGENT_ACTION.CANCELLED_BEFORE_RECOVERY");
  return image;
}

agents::DurableLeaseRequest LeaseRequest() {
  agents::DurableLeaseRequest request;
  request.lease_uuid = "018f0000-0000-7000-8000-00000000be20";
  request.instance_uuid = "018f0000-0000-7000-8000-00000000be11";
  request.owner_uuid = "018f0000-0000-7000-8000-00000000be21";
  request.now_microseconds = 1000;
  request.lease_duration_microseconds = 5000;
  request.evidence_uuid = "018f0000-0000-7000-8000-00000000be22";
  return request;
}

void TestRuntimeServiceStoreRoundTrip() {
  const auto database = CreateActiveDatabase();
  const auto context = Context(database);

  api::AgentDurableCatalogStoreRequest seed;
  seed.context = context;
  seed.image = CatalogImage();
  seed.evidence_uuid = "018f0000-0000-7000-8000-00000000be13";
  seed.production_live_path = true;
  seed.fsync_or_checkpoint_evidence = true;
  Require(api::PersistAgentDurableCatalogImage(seed).ok,
          "initial service catalog seed persist failed");

  api::AgentRuntimeServiceStore service;
  api::AgentRuntimeServiceStoreOpenRequest open;
  open.context = context;
  open.manifest = agents::CanonicalAgentManifest();
  open.production_live_path = true;
  open.worker_foreground_protection_enabled = true;
  open.crash_recovery_mode = true;
  open.service_owner_uuid = "018f0000-0000-7000-8000-00000000be30";
  open.evidence_uuid = "018f0000-0000-7000-8000-00000000be31";
  open.fsync_or_checkpoint_evidence = true;
  auto result = service.Open(std::move(open));
  Require(result.status.ok, "store-backed runtime service open failed: " +
                                result.status.diagnostic_code);

  result = service.Start("018f0000-0000-7000-8000-00000000be32", true);
  Require(result.status.ok, "store-backed runtime service start failed");

  auto lease = LeaseRequest();
  result = service.AcquireLease(lease, true);
  Require(result.status.ok, "store-backed runtime service lease failed");

  lease.evidence_uuid = "018f0000-0000-7000-8000-00000000be23";
  lease.now_microseconds = 1500;
  result = service.HeartbeatLease(lease, true);
  Require(result.status.ok, "store-backed runtime service heartbeat failed");

  const auto loaded = api::LoadAgentDurableCatalogImage(context, true);
  Require(loaded.ok, "runtime service persisted catalog reload failed");
  Require(loaded.image.leases.size() == 1,
          "runtime service lease was not persisted");
  Require(loaded.image.leases.front().heartbeat_generation == 1,
          "runtime service heartbeat was not persisted");
  Require(!loaded.image.retained_history.empty(),
          "runtime service retained history was not persisted");

  api::AgentRuntimeServiceStore recovered;
  api::AgentRuntimeServiceStoreOpenRequest reopen;
  reopen.context = context;
  reopen.manifest = agents::CanonicalAgentManifest();
  reopen.production_live_path = true;
  reopen.worker_foreground_protection_enabled = true;
  reopen.crash_recovery_mode = true;
  reopen.service_owner_uuid = "018f0000-0000-7000-8000-00000000be30";
  reopen.evidence_uuid = "018f0000-0000-7000-8000-00000000be33";
  reopen.fsync_or_checkpoint_evidence = true;
  result = recovered.Open(std::move(reopen));
  Require(result.status.ok, "recovered service open from store failed");
  result = recovered.Start("018f0000-0000-7000-8000-00000000be34", true);
  Require(result.status.ok, "recovered service start failed");
  lease.evidence_uuid = "018f0000-0000-7000-8000-00000000be24";
  lease.now_microseconds = 2000;
  result = recovered.CancelLease(lease,
                                 agents::DurableAgentLeaseState::cancelled,
                                 true);
  Require(result.status.ok, "recovered service cancel failed");

  const auto cancelled = api::LoadAgentDurableCatalogImage(context, true);
  Require(cancelled.ok, "cancelled service catalog reload failed");
  Require(cancelled.image.leases.front().state ==
              agents::DurableAgentLeaseState::cancelled,
          "runtime service cancel state was not persisted");

  Cleanup(database.path);
}

void TestRuntimeServiceStoreRequiresCheckpointEvidence() {
  const auto database = CreateActiveDatabase();
  const auto context = Context(database);

  api::AgentDurableCatalogStoreRequest seed;
  seed.context = context;
  seed.image = CatalogImage();
  seed.evidence_uuid = "018f0000-0000-7000-8000-00000000be40";
  seed.production_live_path = true;
  seed.fsync_or_checkpoint_evidence = true;
  Require(api::PersistAgentDurableCatalogImage(seed).ok,
          "initial checkpoint refusal seed failed");

  api::AgentRuntimeServiceStore service;
  api::AgentRuntimeServiceStoreOpenRequest open;
  open.context = context;
  open.manifest = agents::CanonicalAgentManifest();
  open.production_live_path = true;
  open.worker_foreground_protection_enabled = true;
  open.service_owner_uuid = "018f0000-0000-7000-8000-00000000be41";
  open.evidence_uuid = "018f0000-0000-7000-8000-00000000be42";
  open.fsync_or_checkpoint_evidence = false;
  const auto result = service.Open(std::move(open));
  Require(!result.status.ok &&
              result.status.diagnostic_code ==
                  "SB_AGENT_SERVICE_STORE.PERSIST_FAILED",
          "runtime service store accepted missing checkpoint evidence");

  Cleanup(database.path);
}

void TestRuntimeServiceStoreAcceptsTransactionPerOperation() {
  const auto database = CreateActiveDatabase();
  const auto seed_context = Context(database);

  api::AgentDurableCatalogStoreRequest seed;
  seed.context = seed_context;
  seed.image = CatalogImage();
  seed.evidence_uuid = "018f0000-0000-7000-8000-00000000bf13";
  seed.production_live_path = true;
  seed.fsync_or_checkpoint_evidence = true;
  Require(api::PersistAgentDurableCatalogImage(seed).ok,
          "transaction-per-operation seed failed");
  CommitContext(seed_context);

  api::AgentRuntimeServiceStore service;
  auto open_context =
      BeginTransactionContext(database, "aeic-runtime-service-store-open");
  api::AgentRuntimeServiceStoreOpenRequest open;
  open.context = open_context;
  open.manifest = agents::CanonicalAgentManifest();
  open.production_live_path = true;
  open.worker_foreground_protection_enabled = true;
  open.crash_recovery_mode = true;
  open.service_owner_uuid = "018f0000-0000-7000-8000-00000000bf30";
  open.evidence_uuid = "018f0000-0000-7000-8000-00000000bf31";
  open.fsync_or_checkpoint_evidence = true;
  auto result = service.Open(std::move(open));
  Require(result.status.ok, "transaction-per-operation open failed");
  CommitContext(open_context);

  auto start_context =
      BeginTransactionContext(database, "aeic-runtime-service-store-start");
  service.SetContext(start_context);
  result = service.Start("018f0000-0000-7000-8000-00000000bf32", true);
  Require(result.status.ok, "transaction-per-operation start failed");
  CommitContext(start_context);

  auto lease_context =
      BeginTransactionContext(database, "aeic-runtime-service-store-lease");
  service.SetContext(lease_context);
  auto lease = LeaseRequest();
  lease.lease_uuid = "018f0000-0000-7000-8000-00000000bf20";
  lease.evidence_uuid = "018f0000-0000-7000-8000-00000000bf22";
  result = service.AcquireLease(lease, true);
  Require(result.status.ok, "transaction-per-operation lease failed");
  CommitContext(lease_context);

  const auto load_context =
      BeginTransactionContext(database, "aeic-runtime-service-store-load");
  const auto loaded = api::LoadAgentDurableCatalogImage(load_context, true);
  Require(loaded.ok, "transaction-per-operation persisted catalog load failed");
  Require(loaded.image.leases.size() == 1,
          "transaction-per-operation lease was not visible after commit");
  CommitContext(load_context);

  Cleanup(database.path);
}

void TestRuntimeServiceStoreDrainShutdownPersist() {
  const auto database = CreateActiveDatabase();
  const auto seed_context = Context(database);

  api::AgentDurableCatalogStoreRequest seed;
  seed.context = seed_context;
  seed.image = CatalogImage();
  seed.evidence_uuid = "018f0000-0000-7000-8000-00000000bfa0";
  seed.production_live_path = true;
  seed.fsync_or_checkpoint_evidence = true;
  Require(api::PersistAgentDurableCatalogImage(seed).ok,
          "drain shutdown seed failed");
  CommitContext(seed_context);

  api::AgentRuntimeServiceStore service;
  auto open_context =
      BeginTransactionContext(database, "aeic-runtime-service-drain-open");
  api::AgentRuntimeServiceStoreOpenRequest open;
  open.context = open_context;
  open.manifest = agents::CanonicalAgentManifest();
  open.production_live_path = true;
  open.worker_foreground_protection_enabled = true;
  open.crash_recovery_mode = true;
  open.service_owner_uuid = "018f0000-0000-7000-8000-00000000bfa1";
  open.evidence_uuid = "018f0000-0000-7000-8000-00000000bfa2";
  open.fsync_or_checkpoint_evidence = true;
  auto result = service.Open(std::move(open));
  Require(result.status.ok, "drain shutdown open failed");
  CommitContext(open_context);

  auto start_context =
      BeginTransactionContext(database, "aeic-runtime-service-drain-start");
  service.SetContext(start_context);
  result = service.Start("018f0000-0000-7000-8000-00000000bfa3", true);
  Require(result.status.ok, "drain shutdown start failed");
  CommitContext(start_context);

  auto lease_context =
      BeginTransactionContext(database, "aeic-runtime-service-drain-lease");
  service.SetContext(lease_context);
  auto lease = LeaseRequest();
  lease.lease_uuid = "018f0000-0000-7000-8000-00000000bfa4";
  lease.evidence_uuid = "018f0000-0000-7000-8000-00000000bfa5";
  result = service.AcquireLease(lease, true);
  Require(result.status.ok, "drain shutdown lease failed");
  CommitContext(lease_context);

  auto drain_context =
      BeginTransactionContext(database, "aeic-runtime-service-drain");
  service.SetContext(drain_context);
  result = service.Drain("018f0000-0000-7000-8000-00000000bfa6",
                         6000,
                         true);
  Require(result.status.ok, "runtime service drain failed");
  CommitContext(drain_context);

  auto drain_load_context =
      BeginTransactionContext(database, "aeic-runtime-service-drain-load");
  auto loaded = api::LoadAgentDurableCatalogImage(drain_load_context, true);
  Require(loaded.ok, "drained catalog load failed");
  Require(!loaded.image.leases.empty(), "drained lease missing");
  Require(loaded.image.leases.front().state ==
              agents::DurableAgentLeaseState::draining,
          "runtime service drain state was not persisted");
  CommitContext(drain_load_context);

  auto shutdown_context =
      BeginTransactionContext(database, "aeic-runtime-service-shutdown");
  service.SetContext(shutdown_context);
  result = service.Shutdown("018f0000-0000-7000-8000-00000000bfa7",
                            7000,
                            true);
  Require(result.status.ok, "runtime service shutdown failed");
  CommitContext(shutdown_context);

  auto shutdown_load_context =
      BeginTransactionContext(database, "aeic-runtime-service-shutdown-load");
  loaded = api::LoadAgentDurableCatalogImage(shutdown_load_context, true);
  Require(loaded.ok, "shutdown catalog load failed");
  Require(!loaded.image.leases.empty(), "shutdown lease missing");
  Require(loaded.image.leases.front().state ==
              agents::DurableAgentLeaseState::cancelled,
          "runtime service shutdown state was not persisted");
  bool saw_drain = false;
  bool saw_shutdown = false;
  for (const auto& history : loaded.image.retained_history) {
    saw_drain =
        saw_drain || history.event_kind == "agent_runtime_service.drain";
    saw_shutdown =
        saw_shutdown || history.event_kind == "agent_runtime_service.shutdown";
  }
  Require(saw_drain, "runtime service drain history missing");
  Require(saw_shutdown, "runtime service shutdown history missing");
  CommitContext(shutdown_load_context);

  Cleanup(database.path);
}

void TestRuntimeServiceStoreSupervisionTransitionsPersist() {
  const auto database = CreateActiveDatabase();
  const auto seed_context = Context(database);

  api::AgentDurableCatalogStoreRequest seed;
  seed.context = seed_context;
  seed.image = CatalogImageWithRunningInstance();
  seed.evidence_uuid = "018f0000-0000-7000-8000-00000000bfb0";
  seed.production_live_path = true;
  seed.fsync_or_checkpoint_evidence = true;
  Require(api::PersistAgentDurableCatalogImage(seed).ok,
          "supervision transition seed failed");
  CommitContext(seed_context);

  api::AgentRuntimeServiceStore service;
  auto open_context =
      BeginTransactionContext(database, "aeic-runtime-service-supervision-open");
  api::AgentRuntimeServiceStoreOpenRequest open;
  open.context = open_context;
  open.manifest = agents::CanonicalAgentManifest();
  open.production_live_path = true;
  open.worker_foreground_protection_enabled = true;
  open.crash_recovery_mode = true;
  open.service_owner_uuid = "018f0000-0000-7000-8000-00000000bfb1";
  open.evidence_uuid = "018f0000-0000-7000-8000-00000000bfb2";
  open.fsync_or_checkpoint_evidence = true;
  auto result = service.Open(std::move(open));
  Require(result.status.ok, "supervision transition open failed");
  CommitContext(open_context);

  const auto policy = SupervisionPolicy();
  auto failure_context =
      BeginTransactionContext(database, "aeic-runtime-service-supervision-failure");
  service.SetContext(failure_context);
  result = service.RecordSupervisionFailure(
      "018f0000-0000-7000-8000-00000000be11",
      policy,
      agents::AgentSupervisionFailureKind::runtime_timeout,
      2000,
      "max_runtime_exceeded",
      "018f0000-0000-7000-8000-00000000bfb3",
      true);
  Require(result.status.ok, "supervision failure was not persisted");
  CommitContext(failure_context);

  auto load_context =
      BeginTransactionContext(database, "aeic-runtime-service-supervision-load1");
  auto loaded = api::LoadAgentDurableCatalogImage(load_context, true);
  Require(loaded.ok, "supervision failure catalog load failed");
  const auto& failed = loaded.image.instances.front();
  Require(failed.state == agents::AgentLifecycleState::failed,
          "supervision failure did not persist failed state");
  Require(failed.lease_until_microseconds == 0,
          "supervision failure did not clear run lease");
  Require(failed.restart_not_before_microseconds == 2100,
          "supervision failure did not persist restart backoff");
  Require(failed.last_failure_diagnostic_code ==
              "SB_AGENT_SUPERVISION.RUNTIME_TIMEOUT",
          "supervision failure diagnostic was not persisted");
  CommitContext(load_context);

  auto early_restart_context =
      BeginTransactionContext(database, "aeic-runtime-service-supervision-early");
  service.SetContext(early_restart_context);
  result = service.RequestSupervisionRestart(
      "018f0000-0000-7000-8000-00000000be11",
      policy,
      2050,
      "018f0000-0000-7000-8000-00000000bfb4",
      true);
  Require(!result.status.ok &&
              result.status.diagnostic_code == "SB_AGENT_RESTART.BACKOFF_ACTIVE",
          "early supervision restart bypassed persisted backoff");
  RollbackContext(early_restart_context);

  auto restart_context =
      BeginTransactionContext(database, "aeic-runtime-service-supervision-restart");
  service.SetContext(restart_context);
  result = service.RequestSupervisionRestart(
      "018f0000-0000-7000-8000-00000000be11",
      policy,
      2200,
      "018f0000-0000-7000-8000-00000000bfb5",
      true);
  Require(result.status.ok, "supervision restart after backoff failed");
  CommitContext(restart_context);

  load_context =
      BeginTransactionContext(database, "aeic-runtime-service-supervision-load2");
  loaded = api::LoadAgentDurableCatalogImage(load_context, true);
  Require(loaded.ok, "supervision restart catalog load failed");
  Require(loaded.image.instances.front().state ==
              agents::AgentLifecycleState::registered,
          "supervision restart did not persist registered state");
  Require(loaded.image.instances.front().restart_not_before_microseconds == 0,
          "supervision restart did not clear restart backoff");
  CommitContext(load_context);

  auto cancel_context =
      BeginTransactionContext(database, "aeic-runtime-service-supervision-cancel");
  service.SetContext(cancel_context);
  result = service.CancelAgentExecution(
      "018f0000-0000-7000-8000-00000000be11",
      2300,
      "operator_cancel",
      "018f0000-0000-7000-8000-00000000bfb6",
      true);
  Require(result.status.ok, "supervision cancel failed");
  CommitContext(cancel_context);

  auto quarantine_context =
      BeginTransactionContext(database,
                              "aeic-runtime-service-supervision-quarantine");
  service.SetContext(quarantine_context);
  result = service.QuarantineAgentExecution(
      "018f0000-0000-7000-8000-00000000be11",
      2400,
      "queue_integrity_failure",
      "018f0000-0000-7000-8000-00000000bfb7",
      true);
  Require(result.status.ok, "supervision quarantine failed");
  CommitContext(quarantine_context);

  load_context =
      BeginTransactionContext(database, "aeic-runtime-service-supervision-load3");
  loaded = api::LoadAgentDurableCatalogImage(load_context, true);
  Require(loaded.ok, "supervision quarantine catalog load failed");
  Require(loaded.image.instances.front().state ==
              agents::AgentLifecycleState::quarantined,
          "supervision quarantine did not persist quarantined state");
  Require(loaded.image.instances.front().quarantined,
          "supervision quarantine flag was not persisted");
  bool saw_failure = false;
  bool saw_restart = false;
  bool saw_cancel = false;
  bool saw_quarantine = false;
  for (const auto& history : loaded.image.retained_history) {
    saw_failure = saw_failure ||
                  history.event_kind ==
                      "agent_runtime_service.supervision_failure";
    saw_restart = saw_restart ||
                  history.event_kind ==
                      "agent_runtime_service.supervision_restart";
    saw_cancel =
        saw_cancel || history.event_kind == "agent_runtime_service.agent_cancel";
    saw_quarantine = saw_quarantine ||
                     history.event_kind ==
                         "agent_runtime_service.agent_quarantine";
  }
  Require(saw_failure, "supervision failure history missing");
  Require(saw_restart, "supervision restart history missing");
  Require(saw_cancel, "supervision cancel history missing");
  Require(saw_quarantine, "supervision quarantine history missing");
  CommitContext(load_context);

  Cleanup(database.path);
}

void TestRuntimeServiceStoreCrashReplayMarksDurableWorkPending() {
  const auto database = CreateActiveDatabase();
  const auto seed_context = Context(database);

  api::AgentDurableCatalogStoreRequest seed;
  seed.context = seed_context;
  seed.image = CatalogImageWithQueuedAction();
  seed.evidence_uuid = "018f0000-0000-7000-8000-00000000c013";
  seed.production_live_path = true;
  seed.fsync_or_checkpoint_evidence = true;
  Require(api::PersistAgentDurableCatalogImage(seed).ok,
          "crash replay seed failed");
  CommitContext(seed_context);

  api::AgentRuntimeServiceStore service;
  auto open_context =
      BeginTransactionContext(database, "aeic-runtime-service-crash-open");
  api::AgentRuntimeServiceStoreOpenRequest open;
  open.context = open_context;
  open.manifest = agents::CanonicalAgentManifest();
  open.production_live_path = true;
  open.worker_foreground_protection_enabled = true;
  open.crash_recovery_mode = true;
  open.service_owner_uuid = "018f0000-0000-7000-8000-00000000c030";
  open.evidence_uuid = "018f0000-0000-7000-8000-00000000c031";
  open.fsync_or_checkpoint_evidence = true;
  auto result = service.Open(std::move(open));
  Require(result.status.ok, "crash replay open failed");
  CommitContext(open_context);

  auto start_context =
      BeginTransactionContext(database, "aeic-runtime-service-crash-start");
  service.SetContext(start_context);
  result = service.Start("018f0000-0000-7000-8000-00000000c032", true);
  Require(result.status.ok, "crash replay start failed");
  CommitContext(start_context);

  auto lease_context =
      BeginTransactionContext(database, "aeic-runtime-service-crash-lease");
  service.SetContext(lease_context);
  auto lease = LeaseRequest();
  lease.lease_uuid = "018f0000-0000-7000-8000-00000000c020";
  lease.evidence_uuid = "018f0000-0000-7000-8000-00000000c022";
  result = service.AcquireLease(lease, true);
  Require(result.status.ok, "crash replay live lease acquire failed");
  CommitContext(lease_context);

  api::AgentRuntimeServiceStore recovered;
  auto reopen_context =
      BeginTransactionContext(database, "aeic-runtime-service-crash-reopen");
  api::AgentRuntimeServiceStoreOpenRequest reopen;
  reopen.context = reopen_context;
  reopen.manifest = agents::CanonicalAgentManifest();
  reopen.production_live_path = true;
  reopen.worker_foreground_protection_enabled = true;
  reopen.crash_recovery_mode = true;
  reopen.service_owner_uuid = "018f0000-0000-7000-8000-00000000c030";
  reopen.evidence_uuid = "018f0000-0000-7000-8000-00000000c033";
  reopen.fsync_or_checkpoint_evidence = true;
  result = recovered.Open(std::move(reopen));
  Require(result.status.ok, "crash replay recovered open failed");
  CommitContext(reopen_context);

  auto recover_context =
      BeginTransactionContext(database, "aeic-runtime-service-crash-recover");
  recovered.SetContext(recover_context);
  result = recovered.Recover("018f0000-0000-7000-8000-00000000c034",
                             9000,
                             true);
  Require(result.status.ok, "crash replay recover failed: " +
                                result.status.diagnostic_code);
  CommitContext(recover_context);

  const auto load_context =
      BeginTransactionContext(database, "aeic-runtime-service-crash-load");
  const auto loaded = api::LoadAgentDurableCatalogImage(load_context, true);
  Require(loaded.ok, "crash replay persisted catalog load failed");
  Require(!loaded.image.leases.empty(), "crash replay lease missing");
  Require(loaded.image.leases.front().state ==
              agents::DurableAgentLeaseState::replay_pending,
          "crash replay did not mark live lease replay_pending");
  Require(!loaded.image.actions.empty(), "crash replay action missing");
  std::uint64_t replay_pending_actions = 0;
  bool completed_preserved = false;
  bool cancelled_preserved = false;
  for (const auto& action : loaded.image.actions) {
    if (action.state == agents::DurableAgentActionState::replay_pending) {
      ++replay_pending_actions;
    }
    if (action.operation_id == "agent.test.durable_replay.52") {
      completed_preserved =
          action.state == agents::DurableAgentActionState::completed;
    }
    if (action.operation_id == "agent.test.durable_replay.53") {
      cancelled_preserved =
          action.state == agents::DurableAgentActionState::cancelled;
    }
  }
  Require(replay_pending_actions == 2,
          "crash replay did not mark only pending/running actions replay_pending");
  Require(completed_preserved,
          "crash replay changed completed action state");
  Require(cancelled_preserved,
          "crash replay changed cancelled action state");
  bool saw_lease_replay = false;
  bool saw_action_replay = false;
  for (const auto& history : loaded.image.retained_history) {
    saw_lease_replay =
        saw_lease_replay || history.event_kind == "lease_replay_pending";
    saw_action_replay =
        saw_action_replay || history.event_kind == "action_replay_pending";
  }
  Require(saw_lease_replay, "crash replay lease history missing");
  Require(saw_action_replay, "crash replay action history missing");
  Require(CurrentMetricValue("sb_agent_runtime_service_leases",
                             "state",
                             "replay_pending") >= 1.0,
          "runtime service replay-pending lease metric missing");
  Require(CurrentMetricValue("sb_agent_runtime_service_actions",
                             "state",
                             "replay_pending") >= 2.0,
          "runtime service replay-pending action metric missing");
  Require(CurrentMetricValue("sb_agent_runtime_service_history_records") >=
              static_cast<double>(loaded.image.retained_history.size()),
          "runtime service retained-history metric missing");
  Require(CurrentMetricValue("sb_agent_runtime_service_catalog_generation") >=
              1.0,
          "runtime service catalog-generation metric missing");
  CommitContext(load_context);

  Cleanup(database.path);
}

void TestRuntimeServiceStoreRejectsDuplicateLiveLeaseOwner() {
  const auto database = CreateActiveDatabase();
  const auto seed_context = Context(database);

  api::AgentDurableCatalogStoreRequest seed;
  seed.context = seed_context;
  seed.image = CatalogImage();
  seed.evidence_uuid = "018f0000-0000-7000-8000-00000000d013";
  seed.production_live_path = true;
  seed.fsync_or_checkpoint_evidence = true;
  Require(api::PersistAgentDurableCatalogImage(seed).ok,
          "duplicate owner seed failed");
  CommitContext(seed_context);

  api::AgentRuntimeServiceStore owner;
  auto owner_open_context =
      BeginTransactionContext(database, "aeic-runtime-service-owner-open");
  api::AgentRuntimeServiceStoreOpenRequest owner_open;
  owner_open.context = owner_open_context;
  owner_open.manifest = agents::CanonicalAgentManifest();
  owner_open.production_live_path = true;
  owner_open.worker_foreground_protection_enabled = true;
  owner_open.crash_recovery_mode = true;
  owner_open.service_owner_uuid = "018f0000-0000-7000-8000-00000000d030";
  owner_open.evidence_uuid = "018f0000-0000-7000-8000-00000000d031";
  owner_open.fsync_or_checkpoint_evidence = true;
  auto result = owner.Open(std::move(owner_open));
  Require(result.status.ok, "duplicate owner initial open failed");
  CommitContext(owner_open_context);

  auto owner_start_context =
      BeginTransactionContext(database, "aeic-runtime-service-owner-start");
  owner.SetContext(owner_start_context);
  result = owner.Start("018f0000-0000-7000-8000-00000000d032", true);
  Require(result.status.ok, "duplicate owner initial start failed");
  CommitContext(owner_start_context);

  auto owner_lease_context =
      BeginTransactionContext(database, "aeic-runtime-service-owner-lease");
  owner.SetContext(owner_lease_context);
  auto lease = LeaseRequest();
  lease.lease_uuid = "018f0000-0000-7000-8000-00000000d020";
  lease.evidence_uuid = "018f0000-0000-7000-8000-00000000d022";
  lease.now_microseconds = 10000;
  lease.lease_duration_microseconds = 1000000;
  result = owner.AcquireLease(lease, true);
  Require(result.status.ok, "duplicate owner initial lease failed");
  CommitContext(owner_lease_context);

  api::AgentRuntimeServiceStore contender;
  auto contender_open_context =
      BeginTransactionContext(database, "aeic-runtime-service-contender-open");
  api::AgentRuntimeServiceStoreOpenRequest contender_open;
  contender_open.context = contender_open_context;
  contender_open.manifest = agents::CanonicalAgentManifest();
  contender_open.production_live_path = true;
  contender_open.worker_foreground_protection_enabled = true;
  contender_open.crash_recovery_mode = true;
  contender_open.service_owner_uuid = "018f0000-0000-7000-8000-00000000d040";
  contender_open.evidence_uuid = "018f0000-0000-7000-8000-00000000d041";
  contender_open.fsync_or_checkpoint_evidence = true;
  result = contender.Open(std::move(contender_open));
  Require(result.status.ok, "duplicate owner contender open failed");
  CommitContext(contender_open_context);

  auto contender_start_context =
      BeginTransactionContext(database, "aeic-runtime-service-contender-start");
  contender.SetContext(contender_start_context);
  result = contender.Start("018f0000-0000-7000-8000-00000000d042", true);
  Require(result.status.ok, "duplicate owner contender start failed");
  CommitContext(contender_start_context);

  auto duplicate_context =
      BeginTransactionContext(database, "aeic-runtime-service-duplicate-owner");
  contender.SetContext(duplicate_context);
  auto duplicate = lease;
  duplicate.owner_uuid = "018f0000-0000-7000-8000-00000000d099";
  duplicate.evidence_uuid = "018f0000-0000-7000-8000-00000000d043";
  duplicate.now_microseconds = 20000;
  result = contender.AcquireLease(duplicate, true);
  Require(!result.status.ok &&
              result.status.diagnostic_code ==
                  "SB_AGENT_LEASE.DUPLICATE_LIVE_OWNER_REFUSED",
          "duplicate live lease owner was not refused");
  RollbackContext(duplicate_context);

  Cleanup(database.path);
}

}  // namespace

int main() {
  TestRuntimeServiceStoreRoundTrip();
  TestRuntimeServiceStoreRequiresCheckpointEvidence();
  TestRuntimeServiceStoreAcceptsTransactionPerOperation();
  TestRuntimeServiceStoreDrainShutdownPersist();
  TestRuntimeServiceStoreSupervisionTransitionsPersist();
  TestRuntimeServiceStoreCrashReplayMarksDurableWorkPending();
  TestRuntimeServiceStoreRejectsDuplicateLiveLeaseOwner();
  return EXIT_SUCCESS;
}

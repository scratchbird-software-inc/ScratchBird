// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "database_lifecycle.hpp"
#include "database_lifecycle_test_memory.hpp"
#include "disk_device.hpp"
#include "maintenance_coordinator.hpp"
#include "sbps.hpp"
#include "startup_state.hpp"
#include "uuid.hpp"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <unistd.h>
#include <vector>

namespace {

namespace db = scratchbird::storage::database;
namespace disk = scratchbird::storage::disk;
namespace uuid = scratchbird::core::uuid;
namespace sbps = scratchbird::server::sbps;
using scratchbird::core::platform::UuidKind;
using scratchbird::server::ServerBootstrapConfig;
using scratchbird::server::ServerLifecycleArtifacts;
using scratchbird::server::ServerMaintenanceCoordinator;
using scratchbird::server::ServerMaintenanceOperationRequest;
using scratchbird::server::ServerMaintenanceOperationResult;
using scratchbird::server::ServerShutdownRuntimeSnapshot;

void Require(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << message << '\n';
    std::exit(EXIT_FAILURE);
  }
}

bool Contains(std::string_view haystack, std::string_view needle) {
  return haystack.find(needle) != std::string_view::npos;
}

bool HasDiagnostic(const ServerMaintenanceOperationResult& result, std::string_view code) {
  for (const auto& diagnostic : result.diagnostics) {
    if (diagnostic.code == code) return true;
  }
  return false;
}

std::filesystem::path MakeTempDir() {
  std::string tmpl = "/tmp/sb_dblc011_shutdown.XXXXXX";
  std::vector<char> writable(tmpl.begin(), tmpl.end());
  writable.push_back('\0');
  char* made = ::mkdtemp(writable.data());
  Require(made != nullptr, "mkdtemp failed for DBLC-011 shutdown test");
  return std::filesystem::path(made);
}

struct Fixture {
  std::filesystem::path path;
  std::string database_uuid;
  scratchbird::core::platform::u32 page_size = 0;
};

Fixture CreateActiveDatabase(const std::filesystem::path& path, std::uint64_t now_millis) {
  db::DatabaseCreateConfig create;
  create.path = path.string();
  create.database_uuid = uuid::GenerateEngineIdentityV7(UuidKind::database, now_millis).value;
  create.filespace_uuid = uuid::GenerateEngineIdentityV7(UuidKind::filespace, now_millis + 1).value;
  create.page_size = 16384;
  create.creation_unix_epoch_millis = now_millis + 2;
  create.allow_minimal_resource_bootstrap = true;
  create.require_resource_seed_pack = false;
  create.allow_overwrite = true;
  const auto created = db::CreateDatabaseFile(create);
  Require(created.ok(), "DBLC-011 database create failed");
  const auto opened = db::OpenDatabaseFile({path.string(), false, false, false});
  if (!opened.ok()) {
    std::cerr << opened.diagnostic.diagnostic_code << ":"
              << opened.diagnostic.message_key << '\n';
  }
  Require(opened.ok(), "DBLC-011 first open activation failed");

  Fixture fixture;
  fixture.path = path;
  fixture.database_uuid = uuid::UuidToString(create.database_uuid.value);
  fixture.page_size = created.state.header.page_size;
  return fixture;
}

db::StartupStateRecord ReadStartup(const Fixture& fixture) {
  disk::FileDevice device;
  const auto opened = device.Open(fixture.path.string(), disk::FileOpenMode::open_existing_read_only);
  Require(opened.ok(), "DBLC-011 startup read open failed");
  const auto startup = db::ReadStartupStatePageBody(&device, fixture.page_size);
  Require(startup.ok(), "DBLC-011 startup read failed");
  return startup.state;
}

ServerBootstrapConfig Config(const Fixture& fixture) {
  ServerBootstrapConfig config;
  config.database_default_path = fixture.path;
  config.sbps_enabled = true;
  return config;
}

ServerMaintenanceCoordinator Coordinator(const ServerBootstrapConfig& config) {
  ServerLifecycleArtifacts artifacts;
  artifacts.generation = 11;
  artifacts.state = "dblc011-test";
  return scratchbird::server::BuildMaintenanceCoordinator(config, artifacts);
}

ServerMaintenanceOperationRequest Request(std::string_view operation_key,
                                          std::string_view mode = {}) {
  ServerMaintenanceOperationRequest request;
  request.operation_key = std::string(operation_key);
  request.mode = std::string(mode);
  request.request_uuid = sbps::MakeUuidV7Bytes();
  request.session_uuid = sbps::MakeUuidV7Bytes();
  return request;
}

ServerShutdownRuntimeSnapshot Snapshot(const Fixture& fixture) {
  ServerShutdownRuntimeSnapshot snapshot;
  snapshot.database_path = fixture.path.string();
  snapshot.database_uuid = fixture.database_uuid;
  snapshot.association_scope_proven = true;
  snapshot.associated_manager_count = 1;
  snapshot.associated_listener_count = 1;
  snapshot.associated_parser_count = 1;
  snapshot.associated_ipc_endpoint_count = 1;
  snapshot.associated_session_count = 1;
  snapshot.associated_client_count = 1;
  snapshot.required_acknowledgement_count = 5;
  snapshot.acknowledged_component_count = 5;
  snapshot.drain_complete = true;
  return snapshot;
}

void TestGracefulShutdownCommitsCleanFinalTransaction(const std::filesystem::path& dir) {
  const auto fixture = CreateActiveDatabase(dir / "clean_shutdown.sbdb", 1779400001000);
  const auto startup_before = ReadStartup(fixture);
  Require(!startup_before.clean_shutdown, "active database unexpectedly started clean");
  Require(startup_before.first_open_activation_local_transaction_id != 0,
          "active database missing first-open activation transaction");

  const auto config = Config(fixture);
  auto coordinator = Coordinator(config);
  const auto result = scratchbird::server::ApplyDatabaseShutdownOperation(
      &coordinator,
      config,
      Request("shutdown_database", "acknowledgements_satisfied:true;drain_complete:true"),
      Snapshot(fixture));
  Require(result.ok, "graceful database shutdown was refused");
  Require(result.outcome == "shutdown_clean", "graceful shutdown outcome mismatch");
  Require(coordinator.state == "closed_clean", "coordinator did not enter closed_clean");
  Require(coordinator.attach_admission_fenced && coordinator.write_admission_fenced &&
              coordinator.sblr_admission_fenced && coordinator.event_admission_fenced,
          "shutdown did not fence all ordinary admission");
  Require(Contains(result.records_json, "\"clean_shutdown_marked\":true"),
          "shutdown records missing clean shutdown marker");
  Require(Contains(result.records_json, "\"durable_lifecycle_phase\":\"clean_shutdown\""),
          "shutdown records missing durable clean shutdown phase");

  const auto startup_after = ReadStartup(fixture);
  Require(startup_after.clean_shutdown, "clean shutdown did not mark startup clean");
  Require(startup_after.durable_lifecycle_phase == db::StartupLifecycleDurablePhase::clean_shutdown,
          "clean shutdown durable phase mismatch");
  Require(startup_after.clean_shutdown_local_transaction_id >
              startup_before.first_open_activation_local_transaction_id,
          "clean shutdown transaction did not advance beyond tx2");
}

void TestAcknowledgementTimeoutRefusesBeforeCleanFinalTransaction(const std::filesystem::path& dir) {
  const auto fixture = CreateActiveDatabase(dir / "ack_timeout.sbdb", 1779400002000);
  const auto config = Config(fixture);
  auto coordinator = Coordinator(config);
  auto snapshot = Snapshot(fixture);
  snapshot.required_acknowledgement_count = 5;
  snapshot.acknowledged_component_count = 4;
  const auto result = scratchbird::server::ApplyDatabaseShutdownOperation(
      &coordinator,
      config,
      Request("shutdown_database"),
      snapshot);
  Require(!result.ok, "shutdown succeeded before all acknowledgements arrived");
  Require(HasDiagnostic(result, "ENGINE.SHUTDOWN_ACK_TIMEOUT"),
          "ack timeout diagnostic mismatch");
  Require(coordinator.state == "shutdown_draining",
          "ack timeout did not leave database in shutdown_draining");
  Require(!ReadStartup(fixture).clean_shutdown,
          "ack timeout incorrectly persisted clean shutdown");
}

void TestDrainTimeoutPreservesActiveTransactionFinality(const std::filesystem::path& dir) {
  const auto fixture = CreateActiveDatabase(dir / "drain_timeout.sbdb", 1779400003000);
  const auto config = Config(fixture);
  auto coordinator = Coordinator(config);
  auto snapshot = Snapshot(fixture);
  snapshot.active_transaction_session_count = 1;
  snapshot.drain_complete = false;
  const auto result = scratchbird::server::ApplyDatabaseShutdownOperation(
      &coordinator,
      config,
      Request("shutdown_database", "acknowledgements_satisfied:true"),
      snapshot);
  Require(!result.ok, "graceful shutdown succeeded with active transactions");
  Require(HasDiagnostic(result, "ENGINE.SHUTDOWN_DRAIN_TIMEOUT"),
          "drain timeout diagnostic mismatch");
  Require(Contains(result.records_json, "\"active_transaction_session_count\":1"),
          "drain timeout records missing active transaction count");
  Require(!ReadStartup(fixture).clean_shutdown,
          "drain timeout incorrectly persisted clean shutdown");
}

void TestForceShutdownRequiresExplicitPolicyAndDoesNotMarkClean(const std::filesystem::path& dir) {
  const auto fixture = CreateActiveDatabase(dir / "force_shutdown.sbdb", 1779400004000);
  const auto config = Config(fixture);
  auto coordinator = Coordinator(config);
  auto snapshot = Snapshot(fixture);
  snapshot.active_transaction_session_count = 1;
  snapshot.drain_complete = false;

  const auto refused = scratchbird::server::ApplyDatabaseShutdownOperation(
      &coordinator,
      config,
      Request("shutdown_database_force", "acknowledgements_satisfied:true"),
      snapshot);
  Require(!refused.ok, "force shutdown without policy succeeded");
  Require(HasDiagnostic(refused, "ENGINE.SHUTDOWN_INPUT_INVALID"),
          "force policy refusal diagnostic mismatch");

  const auto result = scratchbird::server::ApplyDatabaseShutdownOperation(
      &coordinator,
      config,
      Request("shutdown_database_force",
              "force_termination_policy_uuid:019e1100-0000-7000-8000-000000000011;"
              "recovery_evidence_preserved:true;acknowledgements_satisfied:true"),
      snapshot);
  Require(result.ok, "explicit force shutdown was refused");
  Require(result.outcome == "shutdown_force_completed", "force shutdown outcome mismatch");
  Require(coordinator.state == "shutdown_force_completed",
          "coordinator did not record force shutdown completion");
  Require(Contains(result.records_json, "\"unknown_transaction_finality_preserved\":true"),
          "force shutdown did not preserve unknown transaction finality evidence");
  Require(!ReadStartup(fixture).clean_shutdown,
          "force shutdown incorrectly marked database clean");
}

}  // namespace

int main() {
  scratchbird::tests::database_lifecycle::ConfigureLifecycleMemoryFixture(
      "database_lifecycle_shutdown_conformance");
  const auto temp_dir = MakeTempDir();
  TestGracefulShutdownCommitsCleanFinalTransaction(temp_dir);
  TestAcknowledgementTimeoutRefusesBeforeCleanFinalTransaction(temp_dir);
  TestDrainTimeoutPreservesActiveTransactionFinality(temp_dir);
  TestForceShutdownRequiresExplicitPolicyAndDoesNotMarkClean(temp_dir);
  std::filesystem::remove_all(temp_dir);
  return EXIT_SUCCESS;
}

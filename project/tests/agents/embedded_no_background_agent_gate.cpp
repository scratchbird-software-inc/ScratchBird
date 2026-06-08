// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "database_lifecycle.hpp"
#include "engine_database_runtime.hpp"
#include "uuid.hpp"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>

namespace {

namespace api = scratchbird::engine::internal_api;
namespace db = scratchbird::storage::database;
namespace platform = scratchbird::core::platform;
namespace uuid = scratchbird::core::uuid;

[[noreturn]] void Fail(const std::string& message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, const std::string& message) {
  if (!condition) { Fail(message); }
}

platform::TypedUuid MakeUuid(platform::UuidKind kind, platform::u64 seed) {
  const auto generated = uuid::GenerateEngineIdentityV7(kind, 1917018000000ull + seed);
  Require(generated.ok(), "PFAR-017 UUID generation failed");
  return generated.value;
}

std::string UuidText(const platform::TypedUuid& value) {
  return uuid::UuidToString(value.value);
}

std::filesystem::path TempDatabasePath(std::string_view suffix) {
  static platform::u64 counter = 0;
  ++counter;
  return std::filesystem::temp_directory_path() /
         ("pfar017-" + std::string(suffix) + "-" + std::to_string(counter) + ".sbdb");
}

void RemoveDatabaseArtifacts(const std::filesystem::path& path) {
  std::error_code ignored;
  std::filesystem::remove(path, ignored);
  std::filesystem::remove(path.string() + ".dirty.manifest", ignored);
  std::filesystem::remove(path.string() + ".recovery.evidence", ignored);
  std::filesystem::remove(path.string() + ".sb.owner.lock", ignored);
  std::filesystem::remove(path.string() + ".sb.local_password_auth", ignored);
}

void CreateLifecycleDatabase(const std::filesystem::path& path, platform::u64 seed) {
  RemoveDatabaseArtifacts(path);
  db::DatabaseCreateConfig create;
  create.path = path.string();
  create.database_uuid = MakeUuid(platform::UuidKind::database, seed);
  create.filespace_uuid = MakeUuid(platform::UuidKind::filespace, seed + 1);
  create.page_size = 16384;
  create.creation_unix_epoch_millis = 1917019000000ull + seed;
  create.allow_minimal_resource_bootstrap = true;
  create.require_resource_seed_pack = false;
  create.allow_overwrite = true;
  const auto created = db::CreateDatabaseFile(create);
  Require(created.ok(), "PFAR-017 CreateDatabaseFile failed: " +
                            created.diagnostic.diagnostic_code);
}

bool HasPhase(const db::StartupStateRecord& startup_state, std::string_view phase) {
  for (const auto& completed : startup_state.completed_phases) {
    if (completed == phase) { return true; }
  }
  return false;
}

db::DatabaseLifecycleState RuntimeDatabase(db::DatabaseLifecyclePhase phase) {
  db::DatabaseLifecycleState database;
  database.path = "/tmp/pfar017-embedded-direct.sbdb";
  database.phase = phase;
  database.database_uuid = MakeUuid(platform::UuidKind::database, 1);
  database.filespace_uuid = MakeUuid(platform::UuidKind::filespace, 2);
  return database;
}

bool HasEvidence(const api::EngineDatabaseRuntimeState& state,
                 std::string_view key,
                 std::string_view value) {
  for (const auto& evidence : state.route_evidence) {
    if (evidence.first == key && evidence.second == value) { return true; }
  }
  return false;
}

void RequireNoLabelUuid(const api::EngineDatabaseRuntimeState& state) {
  const auto database_uuid = UuidText(state.database.database_uuid);
  const auto filespace_uuid = UuidText(state.database.filespace_uuid);
  Require(database_uuid.rfind("database.", 0) != 0 &&
              database_uuid.rfind("agent.", 0) != 0 &&
              database_uuid.rfind("policy.", 0) != 0 &&
              database_uuid.rfind("scope.", 0) != 0,
          "database UUID used label-prefixed authority");
  Require(filespace_uuid.rfind("filespace.", 0) != 0 &&
              filespace_uuid.rfind("agent.", 0) != 0 &&
              filespace_uuid.rfind("policy.", 0) != 0 &&
              filespace_uuid.rfind("scope.", 0) != 0,
          "filespace UUID used label-prefixed authority");
  Require(uuid::ParseDurableEngineIdentityUuid(platform::UuidKind::database, database_uuid).ok(),
          "database UUID is not a durable typed UUID");
  Require(uuid::ParseDurableEngineIdentityUuid(platform::UuidKind::filespace, filespace_uuid).ok(),
          "filespace UUID is not a durable typed UUID");
}

api::EngineDatabaseRuntimeRouteAdmissionOptions EmbeddedRoute(bool lock_owned = true) {
  api::EngineDatabaseRuntimeRouteAdmissionOptions route;
  route.route_mode = api::EngineDatabaseRuntimeRouteMode::embedded_direct;
  route.database_file_lock_owned = lock_owned;
  return route;
}

void TestEmbeddedDirectSuppressesBackgroundAgents() {
  auto route = EmbeddedRoute();
  route.explicit_maintenance_requested = true;
  route.maintenance_action = "validate_agent_runtime_schema";
  const auto result = api::MakeEngineDatabaseRuntimeState(
      RuntimeDatabase(db::DatabaseLifecyclePhase::opened),
      {},
      {},
      {},
      route);
  Require(result.ok(), "embedded direct runtime admission failed");
  const auto& state = result.state;
  Require(state.database_open, "embedded direct runtime did not open");
  Require(state.route_mode == "embedded_direct", "embedded route mode mismatch");
  Require(state.embedded_direct, "embedded direct flag missing");
  Require(!state.local_ipc_route && !state.server_inet_route,
          "embedded route also claimed IPC/server route");
  Require(state.embedded_sysarch_bypass_active,
          "embedded direct did not mark scoped sysarch bypass");
  Require(state.embedded_single_user_session,
          "embedded direct did not mark single-user session");
  Require(state.embedded_always_in_transaction,
          "embedded direct did not mark always-in-transaction");
  Require(state.database_file_lock_owned, "embedded direct did not require owned file lock");
  Require(state.background_agents_suppressed && state.listener_suppressed &&
              state.ipc_server_suppressed && state.manager_suppressed,
          "embedded direct did not suppress background agents/listener/ipc/manager");
  Require(!state.background_agents_launch_allowed,
          "embedded direct allowed background agent launch");
  Require(state.explicit_maintenance_synchronous,
          "embedded explicit maintenance was not synchronous");
  Require(state.explicit_maintenance_engine_owned,
          "embedded explicit maintenance was not engine-owned");
  Require(HasEvidence(state, "sysarch_bypass_scope", "direct_managed_file_only"),
          "embedded sysarch bypass scope evidence missing");
  Require(HasEvidence(state, "transaction_model", "always_in_transaction_mga_engine_owned"),
          "embedded MGA transaction evidence missing");
  Require(HasEvidence(state, "explicit_maintenance", "synchronous_engine_api"),
          "embedded synchronous maintenance evidence missing");
  RequireNoLabelUuid(state);
}

void TestServerAndIpcRoutesDoNotInheritBypass() {
  api::EngineDatabaseRuntimeRouteAdmissionOptions ipc;
  ipc.route_mode = api::EngineDatabaseRuntimeRouteMode::local_ipc;
  const auto ipc_result = api::MakeEngineDatabaseRuntimeState(
      RuntimeDatabase(db::DatabaseLifecyclePhase::opened),
      {},
      {},
      {},
      ipc);
  Require(ipc_result.ok(), "local IPC runtime admission failed");
  Require(ipc_result.state.local_ipc_route, "local IPC route flag missing");
  Require(!ipc_result.state.embedded_sysarch_bypass_active,
          "local IPC inherited embedded sysarch bypass");
  Require(ipc_result.state.background_agents_launch_allowed,
          "local IPC suppressed server-authoritative background agents");
  Require(HasEvidence(ipc_result.state, "security_authority", "server_or_ipc_protocol"),
          "local IPC security authority evidence missing");

  api::EngineDatabaseRuntimeRouteAdmissionOptions server;
  server.route_mode = api::EngineDatabaseRuntimeRouteMode::server_inet;
  const auto server_result = api::MakeEngineDatabaseRuntimeState(
      RuntimeDatabase(db::DatabaseLifecyclePhase::opened),
      {},
      {},
      {},
      server);
  Require(server_result.ok(), "server runtime admission failed");
  Require(server_result.state.server_inet_route, "server route flag missing");
  Require(!server_result.state.embedded_sysarch_bypass_active,
          "server route inherited embedded sysarch bypass");
  Require(server_result.state.background_agents_launch_allowed,
          "server route suppressed background agents");
  RequireNoLabelUuid(ipc_result.state);
  RequireNoLabelUuid(server_result.state);
}

void TestEmbeddedDirectRefusesServerOwnedLock() {
  auto route = EmbeddedRoute(false);
  route.database_file_locked_by_server = true;
  const auto result = api::MakeEngineDatabaseRuntimeState(
      RuntimeDatabase(db::DatabaseLifecyclePhase::opened),
      {},
      {},
      {},
      route);
  Require(!result.ok(), "embedded direct stole server-owned file lock");
  Require(result.diagnostic.diagnostic_code == "SB_EMBEDDED_DIRECT_OWNER_SERVER_LOCKED",
          "server-owned lock diagnostic mismatch: " + result.diagnostic.diagnostic_code);
  Require(result.state.route_diagnostic_detail == "connect_to_owning_server_via_local_ipc",
          "server-owned lock did not direct client to local IPC owner");
  Require(!result.state.database_open, "server-owned lock opened embedded runtime");
}

void TestEmbeddedDirectRequiresOwnedLock() {
  const auto result = api::MakeEngineDatabaseRuntimeState(
      RuntimeDatabase(db::DatabaseLifecyclePhase::opened),
      {},
      {},
      {},
      EmbeddedRoute(false));
  Require(!result.ok(), "embedded direct opened without file lock ownership");
  Require(result.diagnostic.diagnostic_code == "SB_EMBEDDED_DIRECT_FILE_LOCK_REQUIRED",
          "missing lock diagnostic mismatch: " + result.diagnostic.diagnostic_code);
  Require(!result.state.database_open, "missing lock opened embedded runtime");
}

void TestRealEmbeddedOpenSuppressesRuntimeLaunch() {
  const auto path = TempDatabasePath("embedded-direct");
  CreateLifecycleDatabase(path, 101);

  db::DatabaseOpenConfig open;
  open.path = path.string();
  open.suppress_background_agents = true;
  const auto opened = db::OpenDatabaseFile(open);
  RemoveDatabaseArtifacts(path);

  Require(opened.ok(), "embedded direct OpenDatabaseFile failed: " +
                           opened.diagnostic.diagnostic_code);
  const auto& startup = opened.state.startup_state;
  Require(startup.runtime_activation_complete,
          "embedded direct did not complete runtime activation");
  Require(startup.cache_runtime_started, "embedded direct did not start local cache runtime");
  Require(!startup.agent_runtime_started,
          "embedded direct incorrectly marked agent runtime started");
  Require(!startup.ipc_runtime_started,
          "embedded direct incorrectly marked IPC runtime started");
  Require(!startup.server_runtime_started,
          "embedded direct incorrectly marked server runtime started");
  Require(HasPhase(startup, "open.embedded_background_agents_suppressed"),
          "embedded direct suppression phase missing for agents");
  Require(HasPhase(startup, "open.embedded_ipc_runtime_suppressed"),
          "embedded direct suppression phase missing for IPC");
  Require(HasPhase(startup, "open.embedded_server_runtime_suppressed"),
          "embedded direct suppression phase missing for server runtime");
  Require(!HasPhase(startup, "open.runtime_agents_started"),
          "embedded direct recorded normal agent runtime launch phase");
  Require(!HasPhase(startup, "open.ipc_runtime_started"),
          "embedded direct recorded normal IPC launch phase");
  Require(!HasPhase(startup, "open.server_runtime_started"),
          "embedded direct recorded normal server launch phase");
  Require(opened.state.engine_agent_health_present,
          "embedded direct engine-agent health publication missing");
  Require(!opened.state.engine_agent_health.ordinary_admission_allowed,
          "embedded direct allowed background agent ordinary admission");
}

void TestNormalOpenStartsServerRuntime() {
  const auto path = TempDatabasePath("server-route");
  CreateLifecycleDatabase(path, 201);

  db::DatabaseOpenConfig open;
  open.path = path.string();
  const auto opened = db::OpenDatabaseFile(open);
  RemoveDatabaseArtifacts(path);

  Require(opened.ok(), "normal OpenDatabaseFile failed: " +
                           opened.diagnostic.diagnostic_code);
  const auto& startup = opened.state.startup_state;
  Require(startup.runtime_activation_complete,
          "normal open did not complete runtime activation");
  Require(startup.agent_runtime_started,
          "normal open did not mark agent runtime started");
  Require(startup.cache_runtime_started, "normal open did not mark cache runtime started");
  Require(startup.ipc_runtime_started, "normal open did not mark IPC runtime started");
  Require(startup.server_runtime_started,
          "normal open did not mark server runtime started");
  Require(HasPhase(startup, "open.runtime_agents_started"),
          "normal open missing agent runtime launch phase");
  Require(HasPhase(startup, "open.ipc_runtime_started"),
          "normal open missing IPC runtime launch phase");
  Require(HasPhase(startup, "open.server_runtime_started"),
          "normal open missing server runtime launch phase");
  Require(!HasPhase(startup, "open.embedded_background_agents_suppressed"),
          "normal open recorded embedded suppression phase");
}

}  // namespace

int main() {
  TestEmbeddedDirectSuppressesBackgroundAgents();
  TestServerAndIpcRoutesDoNotInheritBypass();
  TestEmbeddedDirectRefusesServerOwnedLock();
  TestEmbeddedDirectRequiresOwnedLock();
  TestRealEmbeddedOpenSuppressesRuntimeLaunch();
  TestNormalOpenStartsServerRuntime();
  return EXIT_SUCCESS;
}

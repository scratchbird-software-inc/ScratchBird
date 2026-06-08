// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "database_lifecycle.hpp"
#include "manager_control.hpp"
#include "sbps.hpp"
#include "uuid.hpp"

#include <array>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <unistd.h>
#include <vector>

namespace {

namespace db = scratchbird::storage::database;
namespace uuid = scratchbird::core::uuid;
namespace sbps = scratchbird::server::sbps;
using scratchbird::core::platform::UuidKind;
using scratchbird::server::HostedDatabaseSnapshot;
using scratchbird::server::HostedDatabaseState;
using scratchbird::server::HostedEngineState;
using scratchbird::server::ParserPackageRegistry;
using scratchbird::server::ServerBootstrapConfig;
using scratchbird::server::ServerLifecycleArtifacts;
using scratchbird::server::ServerListenerOrchestrator;
using scratchbird::server::ServerListenerProfileRuntime;
using scratchbird::server::ServerMaintenanceCoordinator;
using scratchbird::server::ServerManagementContext;
using scratchbird::server::ServerManagementRequest;
using scratchbird::server::ServerManagementResponse;
using scratchbird::server::ServerSessionRecord;
using scratchbird::server::ServerSessionRegistry;

void Require(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << message << '\n';
    std::exit(EXIT_FAILURE);
  }
}

bool Contains(std::string_view haystack, std::string_view needle) {
  return haystack.find(needle) != std::string_view::npos;
}

bool HasDiagnostic(const ServerManagementResponse& result, std::string_view code) {
  for (const auto& diagnostic : result.diagnostics) {
    if (diagnostic.code == code) return true;
  }
  return false;
}

std::filesystem::path MakeTempDir() {
  std::string tmpl = "/tmp/sb_dblc011_shutdown_notify.XXXXXX";
  std::vector<char> writable(tmpl.begin(), tmpl.end());
  writable.push_back('\0');
  char* made = ::mkdtemp(writable.data());
  Require(made != nullptr, "mkdtemp failed for DBLC-011 shutdown notification test");
  return std::filesystem::path(made);
}

struct Fixture {
  std::filesystem::path path;
  std::string database_uuid;
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
  Require(created.ok(), "DBLC-011 notification database create failed");
  const auto opened = db::OpenDatabaseFile({path.string(), false, false, false});
  Require(opened.ok(), "DBLC-011 notification database open failed");
  return Fixture{path, uuid::UuidToString(create.database_uuid.value)};
}

ServerBootstrapConfig Config(const Fixture& fixture) {
  ServerBootstrapConfig config;
  config.database_default_path = fixture.path;
  config.sbps_enabled = true;
  return config;
}

HostedEngineState EngineState(const Fixture& fixture) {
  HostedEngineState state;
  state.engine_context_active = true;
  HostedDatabaseSnapshot database;
  database.state = HostedDatabaseState::kOpen;
  database.database_path = fixture.path.string();
  database.database_uuid = fixture.database_uuid;
  database.database_open = true;
  database.write_admission_fenced = false;
  state.databases.push_back(std::move(database));
  return state;
}

ServerLifecycleArtifacts Artifacts() {
  ServerLifecycleArtifacts artifacts;
  artifacts.generation = 11;
  artifacts.state = "dblc011-notification-test";
  return artifacts;
}

ServerMaintenanceCoordinator Coordinator(const ServerBootstrapConfig& config,
                                         const ServerLifecycleArtifacts& artifacts) {
  return scratchbird::server::BuildMaintenanceCoordinator(config, artifacts);
}

ServerListenerProfileRuntime Listener(const Fixture& fixture,
                                      std::string_view listener_uuid,
                                      std::string_view state) {
  ServerListenerProfileRuntime profile;
  profile.listener_uuid = std::string(listener_uuid);
  profile.profile_name = "native";
  profile.state = std::string(state);
  profile.enabled = state != "stopped";
  profile.pid = -1;
  profile.database_selector = fixture.path.string();
  profile.parser_package_ref = "sbp_native";
  profile.management_socket_path = "/tmp/sb_dblc011_listener.sock";
  profile.last_transition = "associated";
  if (state == "failed") profile.diagnostic_code = "LISTENER.MANAGEMENT_TIMEOUT";
  return profile;
}

ServerListenerOrchestrator Listeners(const Fixture& fixture, bool failed_listener) {
  ServerListenerOrchestrator listeners;
  listeners.generation = 11;
  listeners.engine_endpoint = "/tmp/sb_dblc011_engine.sock";
  listeners.profiles.push_back(
      Listener(fixture, "019e1100-0000-7000-8000-000000000101", failed_listener ? "failed" : "running"));
  ServerListenerProfileRuntime unrelated;
  unrelated.listener_uuid = "019e1100-0000-7000-8000-000000000202";
  unrelated.state = "running";
  unrelated.enabled = true;
  unrelated.pid = -1;
  unrelated.database_selector = "unrelated-database-uuid";
  unrelated.parser_package_ref = "sbp_native";
  unrelated.last_transition = "associated";
  listeners.profiles.push_back(std::move(unrelated));
  return listeners;
}

std::array<std::uint8_t, 16> AddSession(ServerSessionRegistry* registry,
                                        const Fixture& fixture,
                                        std::string_view principal,
                                        std::uint64_t local_transaction_id = 0) {
  ServerSessionRecord session;
  session.connection_uuid = sbps::MakeUuidV7Bytes();
  session.session_uuid = sbps::MakeUuidV7Bytes();
  session.auth_context_uuid = sbps::MakeUuidV7Bytes();
  session.principal_claim = std::string(principal);
  session.database_path = fixture.path.string();
  session.database_uuid = fixture.database_uuid;
  session.effective_user_uuid = sbps::MakeUuidV7Bytes();
  session.local_transaction_id = local_transaction_id;
  registry->sessions_by_uuid[scratchbird::server::UuidBytesToText(session.session_uuid)] = session;
  registry->auth_contexts_by_uuid[scratchbird::server::UuidBytesToText(session.auth_context_uuid)] = session;
  return session.session_uuid;
}

sbps::Frame ManagementFrame(const std::array<std::uint8_t, 16>& session_uuid,
                            std::string_view operation_key,
                            std::string_view mode = {}) {
  ServerManagementRequest request;
  request.operation_key = std::string(operation_key);
  request.mode = std::string(mode);
  sbps::Frame frame;
  frame.header.message_type = static_cast<std::uint16_t>(sbps::MessageType::kManagementRequest);
  frame.header.payload_schema_id = sbps::kSchemaManagementRequestV1;
  frame.header.request_uuid = sbps::MakeUuidV7Bytes();
  frame.header.session_uuid = session_uuid;
  frame.payload = scratchbird::server::EncodeServerManagementRequestForTest(request);
  return frame;
}

ServerManagementContext Context(ServerBootstrapConfig* config,
                                ServerLifecycleArtifacts* artifacts,
                                HostedEngineState* engine_state,
                                ServerSessionRegistry* registry,
                                ParserPackageRegistry* parser_registry,
                                ServerListenerOrchestrator* listeners,
                                ServerMaintenanceCoordinator* coordinator) {
  ServerManagementContext context;
  context.config = config;
  context.artifacts = artifacts;
  context.engine_state = engine_state;
  context.session_registry = registry;
  context.parser_registry = parser_registry;
  context.listener_orchestrator = listeners;
  context.maintenance_coordinator = coordinator;
  return context;
}

void TestManagementShutdownNotifiesAndStopsOnlyTargetDatabase(const std::filesystem::path& dir) {
  const auto fixture = CreateActiveDatabase(dir / "notify_route.sbdb", 1779410001000);
  auto config = Config(fixture);
  auto artifacts = Artifacts();
  auto engine_state = EngineState(fixture);
  auto listeners = Listeners(fixture, false);
  auto coordinator = Coordinator(config, artifacts);
  ParserPackageRegistry parser_registry;
  ServerSessionRegistry registry;
  const auto admin_uuid = AddSession(&registry, fixture, "admin");
  (void)AddSession(&registry, fixture, "alice");

  auto context = Context(&config,
                         &artifacts,
                         &engine_state,
                         &registry,
                         &parser_registry,
                         &listeners,
                         &coordinator);
  const auto response = scratchbird::server::HandleServerManagementRequest(
      context,
      ManagementFrame(admin_uuid,
                      "shutdown_database",
                      "acknowledgements_satisfied:true;drain_complete:true"));
  Require(response.accepted && !response.error, "management shutdown route failed");
  const std::string payload(response.payload.begin(), response.payload.end());
  Require(Contains(payload, "\"outcome\":\"shutdown_clean\""),
          "shutdown route payload missing shutdown_clean outcome");
  Require(Contains(payload, "\"shutdown_notification_count\":6"),
          "shutdown route payload missing target notification count");
  Require(coordinator.state == "closed_clean", "coordinator did not close target database cleanly");
  Require(registry.sessions_by_uuid.empty(), "shutdown did not close target database sessions");
  Require(listeners.profiles[0].state == "stopped", "target listener was not stopped");
  Require(listeners.profiles[1].state == "running", "unrelated listener was incorrectly stopped");
}

void TestParserFallbackRefusesMissingAssociation(const std::filesystem::path& dir) {
  const auto fixture = CreateActiveDatabase(dir / "fallback_refusal.sbdb", 1779410002000);
  auto config = Config(fixture);
  auto artifacts = Artifacts();
  auto engine_state = EngineState(fixture);
  ParserPackageRegistry parser_registry;

  {
    auto listeners = Listeners(fixture, true);
    listeners.profiles[0].parser_package_ref.clear();
    auto coordinator = Coordinator(config, artifacts);
    ServerSessionRegistry registry;
    const auto admin_uuid = AddSession(&registry, fixture, "admin");
    auto context = Context(&config,
                           &artifacts,
                           &engine_state,
                           &registry,
                           &parser_registry,
                           &listeners,
                           &coordinator);
    const auto missing = scratchbird::server::HandleServerManagementRequest(
        context,
        ManagementFrame(admin_uuid,
                        "shutdown_database",
                        "acknowledgements_satisfied:true;drain_complete:true"));
    Require(missing.error, "missing parser association fallback was admitted");
    Require(HasDiagnostic(missing, "ENGINE.SHUTDOWN_PARSER_ASSOCIATION_MISSING"),
            "missing parser association diagnostic mismatch");
  }
}

void TestParserFallbackAvailableClosesThroughEngineVisibleAssociation(const std::filesystem::path& dir) {
  const auto fixture = CreateActiveDatabase(dir / "fallback_success.sbdb", 1779410003000);
  auto config = Config(fixture);
  auto artifacts = Artifacts();
  auto engine_state = EngineState(fixture);
  auto listeners = Listeners(fixture, true);
  auto coordinator = Coordinator(config, artifacts);
  ParserPackageRegistry parser_registry;
  ServerSessionRegistry registry;
  const auto admin_uuid = AddSession(&registry, fixture, "admin");

  auto context = Context(&config,
                         &artifacts,
                         &engine_state,
                         &registry,
                         &parser_registry,
                         &listeners,
                         &coordinator);
  const auto response = scratchbird::server::HandleServerManagementRequest(
      context,
      ManagementFrame(admin_uuid,
                      "shutdown_database",
                      "acknowledgements_satisfied:true;drain_complete:true"));
  Require(response.accepted && !response.error, "available parser fallback was refused");
  const std::string payload(response.payload.begin(), response.payload.end());
  Require(Contains(payload, "\"parser_fallback_used\":true"),
          "available parser fallback was not recorded");
  Require(coordinator.shutdown_parser_fallback_used,
          "coordinator did not record parser fallback");
  Require(registry.sessions_by_uuid.empty(), "parser fallback did not close target sessions");
  Require(listeners.profiles[1].state == "running",
          "parser fallback stopped unrelated listener");
}

}  // namespace

int main() {
  const auto temp_dir = MakeTempDir();
  TestManagementShutdownNotifiesAndStopsOnlyTargetDatabase(temp_dir);
  TestParserFallbackRefusesMissingAssociation(temp_dir);
  TestParserFallbackAvailableClosesThroughEngineVisibleAssociation(temp_dir);
  std::filesystem::remove_all(temp_dir);
  return EXIT_SUCCESS;
}

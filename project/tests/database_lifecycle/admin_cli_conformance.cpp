// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "cli.hpp"
#include "database_lifecycle.hpp"
#include "database_lifecycle_test_memory.hpp"
#include "manager_control.hpp"
#include "maintenance_coordinator.hpp"
#include "res_lifecycle_parity.h"
#include "sbps.hpp"
#include "server_observability.hpp"
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
namespace parity = scratchbird::cli::parity;
namespace sbps = scratchbird::server::sbps;
namespace uuid = scratchbird::core::uuid;
using scratchbird::core::platform::UuidKind;
using scratchbird::server::HostedDatabaseSnapshot;
using scratchbird::server::HostedDatabaseState;
using scratchbird::server::HostedEngineState;
using scratchbird::server::ParserPackageRegistry;
using scratchbird::server::ServerBootstrapConfig;
using scratchbird::server::ServerLifecycleArtifacts;
using scratchbird::server::ServerListenerOrchestrator;
using scratchbird::server::ServerMaintenanceCoordinator;
using scratchbird::server::ServerManagementContext;
using scratchbird::server::ServerManagementRequest;
using scratchbird::server::ServerManagementResponse;
using scratchbird::server::ServerObservabilityState;
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
  std::string tmpl = "/tmp/sb_dblc013p_admin_cli.XXXXXX";
  std::vector<char> writable(tmpl.begin(), tmpl.end());
  writable.push_back('\0');
  char* made = ::mkdtemp(writable.data());
  Require(made != nullptr, "mkdtemp failed for DBLC-013P admin CLI test");
  return std::filesystem::path(made);
}

struct Fixture {
  std::filesystem::path path;
  std::string database_uuid;
  std::string filespace_uuid;
};

Fixture CreateCleanDatabase(const std::filesystem::path& path, std::uint64_t now_millis) {
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
  Require(created.ok(), "DBLC-013P database create failed");
  const auto opened = db::OpenDatabaseFile({path.string(), false, false, false});
  Require(opened.ok(), "DBLC-013P first open failed");
  const auto clean = db::MarkDatabaseCleanShutdown(path.string());
  Require(clean.ok(), "DBLC-013P clean shutdown failed");
  return {path,
          uuid::UuidToString(create.database_uuid.value),
          uuid::UuidToString(create.filespace_uuid.value)};
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

std::array<std::uint8_t, 16> AddSession(ServerSessionRegistry* registry,
                                        const std::filesystem::path& path,
                                        std::string database_uuid,
                                        std::string_view principal,
                                        std::uint64_t local_transaction_id = 0) {
  ServerSessionRecord session;
  session.connection_uuid = sbps::MakeUuidV7Bytes();
  session.session_uuid = sbps::MakeUuidV7Bytes();
  session.auth_context_uuid = sbps::MakeUuidV7Bytes();
  session.principal_claim = std::string(principal);
  session.database_path = path.string();
  session.database_uuid = std::move(database_uuid);
  session.effective_user_uuid = sbps::MakeUuidV7Bytes();
  session.local_transaction_id = local_transaction_id;
  if (principal == "admin") {
    session.engine_authorization_trace_tags = {
        "right:OBS_MANAGEMENT_CONTROL",
        "right:OBS_MANAGEMENT_INSPECT",
        "right:OBS_CONFIG_CONTROL",
        "right:OBS_METRICS_READ_ALL",
        "right:SUPPORT_EXPORT"};
  } else if (principal == "operator") {
    session.engine_authorization_trace_tags = {
        "right:OBS_MANAGEMENT_INSPECT",
        "right:OBS_CONFIG_INSPECT",
        "right:OBS_METRICS_READ_ALL"};
  } else if (principal == "auditor") {
    session.engine_authorization_trace_tags = {
        "right:OBS_CONFIG_INSPECT",
        "right:OBS_METRICS_READ_ALL"};
  }
  registry->sessions_by_uuid[scratchbird::server::UuidBytesToText(session.session_uuid)] = session;
  registry->auth_contexts_by_uuid[scratchbird::server::UuidBytesToText(session.auth_context_uuid)] = session;
  return session.session_uuid;
}

sbps::Frame ManagementFrame(const std::array<std::uint8_t, 16>& session_uuid,
                            std::string_view operation_key,
                            std::string_view mode = {},
                            std::string_view target_uuid = {}) {
  ServerManagementRequest request;
  request.operation_key = std::string(operation_key);
  request.mode = std::string(mode);
  request.target_uuid = std::string(target_uuid);
  request.audit_reason = "dblc013p_admin_cli_conformance";
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
                                ServerMaintenanceCoordinator* coordinator,
                                ServerObservabilityState* observability) {
  ServerManagementContext context;
  context.config = config;
  context.artifacts = artifacts;
  context.engine_state = engine_state;
  context.session_registry = registry;
  context.parser_registry = parser_registry;
  context.listener_orchestrator = listeners;
  context.maintenance_coordinator = coordinator;
  context.observability = observability;
  return context;
}

void TestRouteRegistryCoverage() {
  std::vector<std::string> missing;
  Require(parity::hasCompleteAdminLifecycleRouteCoverage(&missing),
          "DBLC-013P admin lifecycle route registry is incomplete");
  Require(parity::adminLifecycleRoutes().size() >= 12,
          "DBLC-013P route registry did not cover all lifecycle operations");
}

void TestServerCliLifecycleBuilder() {
  const char* argv[] = {"sb_server",
                        "--lifecycle-command",
                        "shutdown-force",
                        "--lifecycle-target-uuid",
                        "019e1300-0000-7000-8000-000000000013",
                        "--lifecycle-mode",
                        "force_termination_policy_uuid:019e1300-0000-7000-8000-000000000014;"
                        "recovery_evidence_preserved:true"};
  auto parsed = scratchbird::server::ParseServerCli(7, const_cast<char**>(argv));
  Require(parsed.ok(), "server CLI lifecycle parse failed");
  Require(scratchbird::server::HasServerCliLifecycleRequest(parsed.options),
          "server CLI lifecycle request flag missing");
  const auto request = scratchbird::server::BuildServerCliLifecycleRequest(parsed.options);
  Require(request.operation_key == "shutdown_database_force",
          "server CLI lifecycle operation did not canonicalize shutdown-force");
  Require(Contains(request.mode, "recovery_evidence_preserved:true"),
          "server CLI lifecycle mode evidence missing");
}

void TestManagementAuthDiagnosticsAndAudit(const Fixture& fixture) {
  ServerBootstrapConfig config;
  config.database_default_path = fixture.path;
  config.sbps_enabled = true;
  config.control_dir = fixture.path.parent_path();
  ServerLifecycleArtifacts artifacts;
  artifacts.generation = 13;
  artifacts.state = "dblc013p-test";
  auto engine_state = EngineState(fixture);
  ParserPackageRegistry parser_registry;
  ServerListenerOrchestrator listeners;
  ServerSessionRegistry registry;
  auto coordinator = scratchbird::server::BuildMaintenanceCoordinator(config, artifacts);
  ServerObservabilityState observability;
  const auto admin = AddSession(&registry, fixture.path, fixture.database_uuid, "admin");
  const auto auditor = AddSession(&registry, fixture.path, fixture.database_uuid, "auditor");
  const auto operator_uuid = AddSession(&registry, fixture.path, fixture.database_uuid, "operator");
  auto context = Context(&config,
                         &artifacts,
                         &engine_state,
                         &registry,
                         &parser_registry,
                         &listeners,
                         &coordinator,
                         &observability);

  auto missing_session = scratchbird::server::HandleServerManagementRequest(
      context, ManagementFrame({}, "verify_database"));
  Require(missing_session.error, "missing session management request was admitted");
  Require(HasDiagnostic(missing_session, "PARSER_SERVER_IPC.SESSION_NOT_BOUND"),
          "missing session diagnostic mismatch");

  auto denied = scratchbird::server::HandleServerManagementRequest(
      context, ManagementFrame(auditor, "verify_database"));
  Require(denied.error, "auditor unexpectedly admitted to verify");
  Require(HasDiagnostic(denied, "SECURITY.ACCESS_DENIED"),
          "auditor denial diagnostic mismatch");

  auto status = scratchbird::server::HandleServerManagementRequest(
      context, ManagementFrame(operator_uuid, "status"));
  Require(status.accepted && !status.error, "operator status route failed");
  const std::string status_payload(status.payload.begin(), status.payload.end());
  Require(Contains(status_payload, "DBLC_STATIC_ADMIN_AUTH_AUDIT_ROUTE"),
          "status route missing audit marker");

  auto verify = scratchbird::server::HandleServerManagementRequest(
      context, ManagementFrame(admin, "verify"));
  Require(verify.accepted && !verify.error, "admin verify route failed");

  auto repair_refused = scratchbird::server::HandleServerManagementRequest(
      context, ManagementFrame(admin, "repair"));
  Require(repair_refused.error, "repair without plan was admitted");
  Require(HasDiagnostic(repair_refused, "ENGINE.DBLC_REPAIR_REFUSED"),
          "repair refusal diagnostic mismatch");

  auto force_refused = scratchbird::server::HandleServerManagementRequest(
      context, ManagementFrame(admin, "shutdown-force", "acknowledgements_satisfied:true"));
  Require(force_refused.error, "force shutdown without explicit policy was admitted");
  Require(HasDiagnostic(force_refused, "ENGINE.SHUTDOWN_INPUT_INVALID"),
          "force shutdown policy diagnostic mismatch");

  auto drop_refused = scratchbird::server::HandleServerManagementRequest(
      context, ManagementFrame(admin, "drop", "retention_policy_satisfied:true"));
  Require(drop_refused.error, "drop without backup/legal proof was admitted");
  Require(HasDiagnostic(drop_refused, "ENGINE.DBLC_DROP_UNSAFE"),
          "drop refusal diagnostic mismatch");

  Require(!observability.audit_events.empty(), "admin routes did not produce audit evidence");
}

void TestCreateOpenAttachDetachManagementRoutes(const std::filesystem::path& path) {
  ServerBootstrapConfig config;
  config.database_default_path = path;
  config.sbps_enabled = true;
  ServerLifecycleArtifacts artifacts;
  artifacts.generation = 130;
  artifacts.state = "dblc013p-create-test";
  HostedEngineState engine_state;
  engine_state.engine_context_active = true;
  ParserPackageRegistry parser_registry;
  ServerListenerOrchestrator listeners;
  ServerSessionRegistry registry;
  auto admin = AddSession(&registry, path, {}, "admin");
  auto coordinator = scratchbird::server::BuildMaintenanceCoordinator(config, artifacts);
  ServerObservabilityState observability;
  auto context = Context(&config,
                         &artifacts,
                         &engine_state,
                         &registry,
                         &parser_registry,
                         &listeners,
                         &coordinator,
                         &observability);

  auto created = scratchbird::server::HandleServerManagementRequest(
      context,
      ManagementFrame(admin, "create", "allow_minimal_resource_bootstrap:true"));
  Require(created.accepted && !created.error, "create database management route failed");
  Require(std::filesystem::exists(path), "create database route did not create database file");

  auto opened = scratchbird::server::HandleServerManagementRequest(
      context, ManagementFrame(admin, "open"));
  Require(opened.accepted && !opened.error, "open database management route failed");

  auto attached = scratchbird::server::HandleServerManagementRequest(
      context, ManagementFrame(admin, "attach"));
  Require(attached.accepted && !attached.error, "attach database management route failed");

  auto detached = scratchbird::server::HandleServerManagementRequest(
      context, ManagementFrame(admin, "detach"));
  Require(detached.accepted && !detached.error, "detach database management route failed");
}

}  // namespace

int main() {
  scratchbird::tests::database_lifecycle::ConfigureLifecycleMemoryFixture(
      "database_lifecycle_admin_cli_conformance");
  const auto temp_dir = MakeTempDir();
  TestRouteRegistryCoverage();
  TestServerCliLifecycleBuilder();
  const auto fixture = CreateCleanDatabase(temp_dir / "admin_cli.sbdb", 1779600001000);
  TestManagementAuthDiagnosticsAndAudit(fixture);
  TestCreateOpenAttachDetachManagementRoutes(temp_dir / "created_by_admin_cli.sbdb");
  std::filesystem::remove_all(temp_dir);
  std::cout << "DBLC_P13P_ADMIN_CLI_COMPLETE=passed\n";
  return EXIT_SUCCESS;
}

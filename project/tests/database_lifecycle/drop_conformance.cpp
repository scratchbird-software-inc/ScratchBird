// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "database_lifecycle.hpp"
#include "disk_device.hpp"
#include "lifecycle/engine_lifecycle_api.hpp"
#include "manager_control.hpp"
#include "sbps.hpp"
#include "startup_state.hpp"
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

namespace api = scratchbird::engine::internal_api;
namespace db = scratchbird::storage::database;
namespace disk = scratchbird::storage::disk;
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

bool HasDiagnostic(const api::EngineApiResult& result, std::string_view code) {
  for (const auto& diagnostic : result.diagnostics) {
    if (diagnostic.code == code) return true;
  }
  return false;
}

bool HasDiagnostic(const ServerManagementResponse& result, std::string_view code) {
  for (const auto& diagnostic : result.diagnostics) {
    if (diagnostic.code == code) return true;
  }
  return false;
}

std::filesystem::path MakeTempDir() {
  std::string tmpl = "/tmp/sb_dblc012_drop.XXXXXX";
  std::vector<char> writable(tmpl.begin(), tmpl.end());
  writable.push_back('\0');
  char* made = ::mkdtemp(writable.data());
  Require(made != nullptr, "mkdtemp failed for DBLC-012 drop test");
  return std::filesystem::path(made);
}

struct Fixture {
  std::filesystem::path path;
  std::string database_uuid;
  std::string filespace_uuid;
  scratchbird::core::platform::u32 page_size = 0;
};

Fixture CreateDatabase(const std::filesystem::path& path, std::uint64_t now_millis) {
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
  Require(created.ok(), "DBLC-012 database create failed");
  const auto opened = db::OpenDatabaseFile({path.string(), false, false, false});
  Require(opened.ok(), "DBLC-012 first open activation failed");

  Fixture fixture;
  fixture.path = path;
  fixture.database_uuid = uuid::UuidToString(create.database_uuid.value);
  fixture.filespace_uuid = uuid::UuidToString(create.filespace_uuid.value);
  fixture.page_size = created.state.header.page_size;
  return fixture;
}

Fixture CreateCleanDatabase(const std::filesystem::path& path, std::uint64_t now_millis) {
  auto fixture = CreateDatabase(path, now_millis);
  const auto clean = db::MarkDatabaseCleanShutdown(path.string());
  Require(clean.ok(), "DBLC-012 clean shutdown failed");
  return fixture;
}

db::StartupStateRecord ReadStartup(const Fixture& fixture) {
  disk::FileDevice device;
  const auto opened = device.Open(fixture.path.string(), disk::FileOpenMode::open_existing_read_only);
  Require(opened.ok(), "DBLC-012 startup read open failed");
  const auto startup = db::ReadStartupStatePageBody(&device, fixture.page_size);
  Require(startup.ok(), "DBLC-012 startup read failed");
  return startup.state;
}

bool HasDropEvidenceFlag(const db::StartupStateRecord& startup) {
  return (startup.durable_evidence_flags & db::StartupLifecycleEvidenceFlag::drop_evidence_recorded) != 0;
}

db::DatabaseDropConfig DropConfig(const Fixture& fixture, std::string_view mode = "logical") {
  db::DatabaseDropConfig config;
  config.path = fixture.path.string();
  config.operation_uuid = "dblc012-drop-operation";
  config.actor_uuid = "dblc012-actor";
  config.drop_mode = std::string(mode);
  config.expected_database_uuid = fixture.database_uuid;
  config.expected_filespace_uuid = fixture.filespace_uuid;
  config.drop_safety_preconditions = true;
  config.session_drain_complete = true;
  config.ownership_release_verified = true;
  config.retention_policy_satisfied = true;
  config.backup_coverage_verified = true;
  config.legal_hold_clear = true;
  return config;
}

api::EngineRequestContext EngineContext(const Fixture& fixture) {
  api::EngineRequestContext context;
  context.trust_mode = api::EngineTrustMode::server_isolated;
  context.request_id = "dblc012-engine-drop";
  context.database_path = fixture.path.string();
  context.database_uuid.canonical = fixture.database_uuid;
  context.principal_uuid.canonical = "019e1200-a100-7000-8000-000000000101";
  context.session_uuid.canonical = "019e1200-a100-7000-8000-000000000102";
  context.security_context_present = true;
  context.catalog_generation_id = 1;
  context.security_epoch = 1;
  context.resource_epoch = 1;
  context.name_resolution_epoch = 1;
  return context;
}

void AddDropPreconditionOptions(api::EngineDropLifecycleRequest* request,
                                const Fixture& fixture,
                                std::string_view mode = "logical") {
  request->option_envelopes.push_back(std::string("drop_mode:") + std::string(mode));
  request->option_envelopes.push_back("drop_safety_preconditions:true");
  request->option_envelopes.push_back("session_drain_complete:true");
  request->option_envelopes.push_back("ownership_release_verified:true");
  request->option_envelopes.push_back("retention_policy_satisfied:true");
  request->option_envelopes.push_back("backup_coverage_verified:true");
  request->option_envelopes.push_back("legal_hold_clear:true");
  request->option_envelopes.push_back("expected_filespace_uuid:" + fixture.filespace_uuid);
}

void TestStorageDropRefusalsAndLogicalEvidence(const std::filesystem::path& dir) {
  const auto active = CreateDatabase(dir / "drop_active.sbdb", 1779500001000);
  const auto active_drop = db::DropDatabaseLifecycle(DropConfig(active));
  Require(!active_drop.ok(), "drop admitted database without clean shutdown");
  Require(active_drop.diagnostic.diagnostic_code == "ENGINE.DBLC_DROP_UNSAFE",
          "active drop refusal diagnostic mismatch");

  const auto fixture = CreateCleanDatabase(dir / "drop_logical.sbdb", 1779500002000);
  auto missing_policy = DropConfig(fixture);
  missing_policy.legal_hold_clear = false;
  const auto refused = db::DropDatabaseLifecycle(missing_policy);
  Require(!refused.ok(), "drop admitted missing legal hold proof");
  Require(refused.diagnostic.diagnostic_code == "ENGINE.DBLC_DROP_UNSAFE",
          "drop unsafe policy diagnostic mismatch");

  const auto dropped = db::DropDatabaseLifecycle(DropConfig(fixture));
  Require(dropped.ok(), "logical drop failed");
  Require(dropped.state.phase == db::DatabaseLifecyclePhase::dropped,
          "logical drop phase mismatch");
  Require(std::filesystem::exists(fixture.path), "logical drop removed database file");
  Require(std::filesystem::exists(fixture.path.string() + ".sb.drop_evidence"),
          "logical drop evidence sidecar missing");
  const auto startup = ReadStartup(fixture);
  Require(startup.durable_lifecycle_phase == db::StartupLifecycleDurablePhase::drop_evidence_recorded,
          "drop durable phase missing");
  Require(startup.write_admission_fenced, "drop did not fence write admission");
  Require(HasDropEvidenceFlag(startup), "drop evidence flag missing");
  Require(startup.last_lifecycle_local_transaction_id > startup.clean_shutdown_local_transaction_id,
          "drop transaction did not advance beyond clean shutdown transaction");
}

void TestStorageQuarantineAndPhysicalDeletePolicies(const std::filesystem::path& dir) {
  const auto quarantine_fixture = CreateCleanDatabase(dir / "drop_quarantine.sbdb", 1779500003000);
  auto quarantine_missing_policy = DropConfig(quarantine_fixture, "quarantine");
  const auto refused_quarantine = db::DropDatabaseLifecycle(quarantine_missing_policy);
  Require(!refused_quarantine.ok(), "quarantine drop admitted without policy");
  quarantine_missing_policy.allow_quarantine = true;
  const auto quarantined = db::DropDatabaseLifecycle(quarantine_missing_policy);
  Require(quarantined.ok(), "quarantine drop failed");
  Require(quarantined.state.phase == db::DatabaseLifecyclePhase::quarantined,
          "quarantine drop phase mismatch");
  Require(!std::filesystem::exists(quarantine_fixture.path),
          "quarantine drop left original database file in place");
  Require(std::filesystem::exists(quarantined.state.path),
          "quarantine drop did not preserve quarantined database file");

  const auto delete_fixture = CreateCleanDatabase(dir / "drop_delete.sbdb", 1779500004000);
  auto delete_missing_policy = DropConfig(delete_fixture, "physical_delete");
  const auto refused_delete = db::DropDatabaseLifecycle(delete_missing_policy);
  Require(!refused_delete.ok(), "physical delete admitted without policy");
  delete_missing_policy.allow_physical_delete = true;
  const auto deleted = db::DropDatabaseLifecycle(delete_missing_policy);
  Require(deleted.ok(), "physical delete drop failed");
  Require(deleted.state.phase == db::DatabaseLifecyclePhase::dropped,
          "physical delete phase mismatch");
  Require(!std::filesystem::exists(delete_fixture.path),
          "physical delete left database file in place");
  Require(std::filesystem::exists(delete_fixture.path.string() + ".sb.drop_evidence"),
          "physical delete evidence sidecar missing");
}

void TestEngineDropLifecycle(const std::filesystem::path& dir) {
  const auto fixture = CreateCleanDatabase(dir / "drop_engine.sbdb", 1779500005000);
  api::EngineDropLifecycleRequest missing;
  missing.context = EngineContext(fixture);
  const auto refused = api::EngineDropLifecycle(missing);
  Require(!refused.ok, "engine drop admitted missing preconditions");
  Require(HasDiagnostic(refused, "SB_ENGINE_API_LIFECYCLE_DROP_PRECONDITIONS_NOT_SATISFIED"),
          "engine drop precondition diagnostic mismatch");

  api::EngineDropLifecycleRequest request;
  request.context = EngineContext(fixture);
  AddDropPreconditionOptions(&request, fixture);
  const auto dropped = api::EngineDropLifecycle(request);
  Require(dropped.ok, "engine logical drop failed");
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
  artifacts.generation = 12;
  artifacts.state = "dblc012-drop-test";
  return artifacts;
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

void TestManagementDropRoute(const std::filesystem::path& dir) {
  const auto active_fixture = CreateCleanDatabase(dir / "drop_route_active.sbdb", 1779500006000);
  auto active_config = Config(active_fixture);
  auto active_artifacts = Artifacts();
  auto active_engine_state = EngineState(active_fixture);
  ParserPackageRegistry parser_registry;
  ServerListenerOrchestrator listeners;
  ServerSessionRegistry active_registry;
  const auto active_admin = AddSession(&active_registry, active_fixture, "admin", 44);
  auto active_coordinator =
      scratchbird::server::BuildMaintenanceCoordinator(active_config, active_artifacts);
  auto active_context = Context(&active_config,
                                &active_artifacts,
                                &active_engine_state,
                                &active_registry,
                                &parser_registry,
                                &listeners,
                                &active_coordinator);
  const auto active_refused = scratchbird::server::HandleServerManagementRequest(
      active_context,
      ManagementFrame(active_admin,
                      "drop_database",
                      "retention_policy_satisfied:true;backup_coverage_verified:true;legal_hold_clear:true"));
  Require(active_refused.error, "management drop admitted active transaction");
  Require(HasDiagnostic(active_refused, "ENGINE.DBLC_DROP_UNSAFE"),
          "management active drop diagnostic mismatch");

  const auto fixture = CreateCleanDatabase(dir / "drop_route.sbdb", 1779500007000);
  auto config = Config(fixture);
  auto artifacts = Artifacts();
  auto engine_state = EngineState(fixture);
  ServerSessionRegistry registry;
  const auto admin_uuid = AddSession(&registry, fixture, "admin");
  (void)AddSession(&registry, fixture, "alice");
  auto coordinator = scratchbird::server::BuildMaintenanceCoordinator(config, artifacts);
  auto context = Context(&config,
                         &artifacts,
                         &engine_state,
                         &registry,
                         &parser_registry,
                         &listeners,
                         &coordinator);
  const std::string mode =
      "retention_policy_satisfied:true;backup_coverage_verified:true;legal_hold_clear:true;"
      "expected_filespace_uuid:" + fixture.filespace_uuid;
  const auto response = scratchbird::server::HandleServerManagementRequest(
      context,
      ManagementFrame(admin_uuid, "drop_database", mode));
  Require(response.accepted && !response.error, "management drop route failed");
  const std::string payload(response.payload.begin(), response.payload.end());
  Require(Contains(payload, "\"outcome\":\"dropped\""),
          "management drop payload missing outcome");
  Require(Contains(payload, "\"durable_lifecycle_phase\":\"drop_evidence_recorded\""),
          "management drop payload missing durable drop phase");
  Require(registry.sessions_by_uuid.empty(), "management drop did not close target sessions");
  Require(coordinator.state == "database_dropped", "management drop coordinator state mismatch");
}

}  // namespace

int main() {
  const auto temp_dir = MakeTempDir();
  TestStorageDropRefusalsAndLogicalEvidence(temp_dir);
  TestStorageQuarantineAndPhysicalDeletePolicies(temp_dir);
  TestEngineDropLifecycle(temp_dir);
  TestManagementDropRoute(temp_dir);
  std::filesystem::remove_all(temp_dir);
  return EXIT_SUCCESS;
}

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
#include "lifecycle/engine_lifecycle_api.hpp"
#include "maintenance_coordinator.hpp"
#include "manager_control.hpp"
#include "sblr_dispatch.hpp"
#include "session_registry.hpp"
#include "startup_state.hpp"
#include "uuid.hpp"

#include <array>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>
#include <unistd.h>
#include <vector>

namespace {

namespace api = scratchbird::engine::internal_api;
namespace db = scratchbird::storage::database;
namespace disk = scratchbird::storage::disk;
namespace sblr = scratchbird::engine::sblr;
namespace uuid = scratchbird::core::uuid;
namespace sbps = scratchbird::server::sbps;
using scratchbird::core::platform::UuidKind;
using scratchbird::server::HostedEngineState;
using scratchbird::server::ServerBootstrapConfig;
using scratchbird::server::ServerLifecycleArtifacts;
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

bool HasDiagnostic(const sblr::SblrDispatchResult& result, std::string_view code) {
  for (const auto& diagnostic : result.diagnostics) {
    if (diagnostic.code == code) return true;
  }
  for (const auto& diagnostic : result.api_result.diagnostics) {
    if (diagnostic.code == code) return true;
  }
  return false;
}

std::filesystem::path MakeTempDir() {
  std::string tmpl = "/tmp/sb_dblc010_maintenance_repair.XXXXXX";
  std::vector<char> writable(tmpl.begin(), tmpl.end());
  writable.push_back('\0');
  char* made = ::mkdtemp(writable.data());
  Require(made != nullptr, "mkdtemp failed for DBLC-010 maintenance repair test");
  return std::filesystem::path(made);
}

std::string UuidText(const scratchbird::core::platform::TypedUuid& value) {
  return uuid::UuidToString(value.value);
}

struct Fixture {
  std::filesystem::path path;
  std::string database_uuid;
  std::string filespace_uuid;
  scratchbird::core::platform::u32 page_size = 0;
};

Fixture CreateOpenCleanDatabase(const std::filesystem::path& path,
                                std::uint64_t now_millis) {
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
  if (!created.ok()) {
    std::cerr << created.diagnostic.diagnostic_code << ":"
              << created.diagnostic.message_key << '\n';
  }
  Require(created.ok(), "DBLC-010 database create failed");

  const auto opened = db::OpenDatabaseFile({path.string(), false, false, false});
  if (!opened.ok()) {
    std::cerr << opened.diagnostic.diagnostic_code << ":"
              << opened.diagnostic.message_key << '\n';
  }
  Require(opened.ok(), "DBLC-010 first open activation failed");
  const auto clean = db::MarkDatabaseCleanShutdown(path.string());
  Require(clean.ok(), "DBLC-010 clean shutdown failed");

  Fixture fixture;
  fixture.path = path;
  fixture.database_uuid = UuidText(create.database_uuid);
  fixture.filespace_uuid = UuidText(create.filespace_uuid);
  fixture.page_size = created.state.header.page_size;
  return fixture;
}

db::StartupStateRecord ReadStartup(const Fixture& fixture) {
  disk::FileDevice device;
  const auto opened = device.Open(fixture.path.string(), disk::FileOpenMode::open_existing_read_only);
  Require(opened.ok(), "startup read open failed");
  const auto startup = db::ReadStartupStatePageBody(&device, fixture.page_size);
  if (!startup.ok()) {
    std::cerr << startup.diagnostic.diagnostic_code << '\n';
  }
  Require(startup.ok(), "startup read failed");
  return startup.state;
}

template <typename Mutator>
void MutateStartup(const Fixture& fixture, Mutator mutator) {
  disk::FileDevice device;
  const auto opened = device.Open(fixture.path.string(), disk::FileOpenMode::open_existing);
  Require(opened.ok(), "startup mutation open failed");
  auto startup = db::ReadStartupStatePageBody(&device, fixture.page_size);
  Require(startup.ok(), "startup mutation read failed");
  mutator(&startup.state);
  const auto written = db::WriteStartupStatePageBody(&device, startup.state);
  Require(written.ok(), "startup mutation write failed");
  const auto synced = device.Sync();
  Require(synced.ok(), "startup mutation sync failed");
}

db::DatabaseLifecycleOperationConfig OperationConfig(const Fixture& fixture) {
  db::DatabaseLifecycleOperationConfig config;
  config.path = fixture.path.string();
  config.operation_uuid = "dblc010-operation";
  config.actor_uuid = "dblc010-actor";
  config.write_evidence = true;
  return config;
}

db::DatabaseLifecycleRepairConfig RepairConfig(const Fixture& fixture,
                                               std::string_view plan,
                                               bool admitted) {
  db::DatabaseLifecycleRepairConfig config;
  config.path = fixture.path.string();
  config.operation_uuid = "dblc010-repair";
  config.actor_uuid = "dblc010-actor";
  config.repair_plan_id = std::string(plan);
  config.expected_database_uuid = fixture.database_uuid;
  config.expected_filespace_uuid = fixture.filespace_uuid;
  config.repair_admission_proven = admitted;
  config.allow_mutation = admitted;
  return config;
}

api::EngineRequestContext EngineContext(const Fixture& fixture) {
  api::EngineRequestContext context;
  context.trust_mode = api::EngineTrustMode::server_isolated;
  context.request_id = "dblc010-engine-request";
  context.database_path = fixture.path.string();
  context.database_uuid.canonical = fixture.database_uuid;
  context.principal_uuid.canonical = "019e1080-a100-7000-8000-000000000101";
  context.session_uuid.canonical = "019e1080-a100-7000-8000-000000000102";
  context.security_context_present = true;
  context.catalog_generation_id = 1;
  context.security_epoch = 1;
  context.resource_epoch = 1;
  context.name_resolution_epoch = 1;
  return context;
}

sblr::SblrOperationEnvelope LifecycleEnvelope(std::string operation_id,
                                              std::string opcode) {
  auto envelope = sblr::MakeSblrEnvelope(std::move(operation_id),
                                         std::move(opcode),
                                         "trace.dblc010.lifecycle.repair");
  envelope.requires_security_context = true;
  envelope.requires_transaction_context = false;
  envelope.requires_cluster_authority = false;
  envelope.contains_sql_text = false;
  envelope.parser_resolved_names_to_uuids = true;
  return envelope;
}

sblr::SblrDispatchResult DispatchLifecycleSblr(
    const Fixture& fixture,
    std::string operation_id,
    std::string opcode,
    std::vector<std::string> options = {}) {
  api::EngineApiRequest api_request;
  api_request.option_envelopes = std::move(options);
  const sblr::SblrDispatchRequest request{
      EngineContext(fixture),
      LifecycleEnvelope(std::move(operation_id), std::move(opcode)),
      std::move(api_request)};
  return sblr::DispatchSblrOperation(request);
}

void RequireSblrLifecycleSuccess(const Fixture& fixture,
                                 std::string operation_id,
                                 std::string opcode,
                                 std::vector<std::string> options = {}) {
  const std::string expected_operation = operation_id;
  const auto result = DispatchLifecycleSblr(fixture,
                                           std::move(operation_id),
                                           std::move(opcode),
                                           std::move(options));
  Require(result.envelope_validated, "SBLR lifecycle envelope did not validate");
  Require(result.accepted, "SBLR lifecycle dispatch did not accept operation");
  Require(result.dispatched_to_api, "SBLR lifecycle dispatch did not reach API");
  if (!result.api_result.ok) {
    for (const auto& diagnostic : result.api_result.diagnostics) {
      std::cerr << diagnostic.code << ':' << diagnostic.detail << '\n';
    }
  }
  Require(result.api_result.ok, "SBLR lifecycle API operation failed");
  Require(result.api_result.operation_id == expected_operation,
          "SBLR lifecycle API operation id mismatch");
}

void TestStorageMaintenanceRepair(const Fixture& fixture) {
  const auto entered = db::EnterDatabaseRestrictedOpenMode(OperationConfig(fixture));
  Require(entered.ok(), "restricted-open entry failed");
  Require(entered.state.phase == db::DatabaseLifecyclePhase::restricted_open,
          "restricted-open phase mismatch");
  auto startup = ReadStartup(fixture);
  Require(startup.write_admission_fenced, "restricted-open did not fence write admission");
  Require(startup.durable_lifecycle_phase == db::StartupLifecycleDurablePhase::restricted_open_entered,
          "restricted-open durable phase missing");
  Require(startup.last_lifecycle_local_transaction_id != 0,
          "restricted-open did not record MGA lifecycle transaction");

  const auto ordinary_open = db::OpenDatabaseFile({fixture.path.string(), false, false, false});
  Require(!ordinary_open.ok(), "ordinary open succeeded during restricted-open");
  Require(ordinary_open.diagnostic.diagnostic_code == "SB-DB-LIFECYCLE-RESTRICTED-OPEN-REQUIRED",
          "ordinary open did not report restricted-open required");

  const auto verified = db::VerifyDatabaseLifecycle(OperationConfig(fixture));
  Require(verified.ok(), "verify failed in restricted-open");
  startup = ReadStartup(fixture);
  Require(startup.write_admission_fenced, "verify cleared restricted-open write fence");
  Require(startup.durable_lifecycle_phase == db::StartupLifecycleDurablePhase::verify_completed,
          "verify durable phase missing");

  const auto refused = db::RepairDatabaseLifecycle(RepairConfig(fixture, "", false));
  Require(!refused.ok(), "repair without plan/admission succeeded");
  Require(refused.diagnostic.diagnostic_code == "ENGINE.DBLC_REPAIR_REFUSED",
          "repair refusal did not use DBLC diagnostic");

  const auto repaired = db::RepairDatabaseLifecycle(
      RepairConfig(fixture, "clear_verified_write_fence", true));
  Require(repaired.ok(), "accepted repair plan failed");
  startup = ReadStartup(fixture);
  Require(!startup.write_admission_fenced, "repair did not clear verified write fence");
  Require(startup.durable_lifecycle_phase == db::StartupLifecycleDurablePhase::repair_completed,
          "repair durable phase missing");
  Require(startup.last_lifecycle_local_transaction_id != 0,
          "repair did not record MGA lifecycle transaction");

  const auto open_after_repair = db::OpenDatabaseFile({fixture.path.string(), false, false, false});
  Require(open_after_repair.ok(), "ordinary open after accepted repair failed");
  const auto clean = db::MarkDatabaseCleanShutdown(fixture.path.string());
  Require(clean.ok(), "clean shutdown after repair failed");

  const auto maintenance = db::EnterDatabaseMaintenanceMode(OperationConfig(fixture));
  Require(maintenance.ok(), "maintenance entry failed");
  startup = ReadStartup(fixture);
  Require(startup.write_admission_fenced, "maintenance entry did not fence writes");
  Require(startup.durable_lifecycle_phase == db::StartupLifecycleDurablePhase::maintenance_entered,
          "maintenance durable phase missing");
  const auto ordinary_during_maintenance =
      db::OpenDatabaseFile({fixture.path.string(), false, false, false});
  Require(!ordinary_during_maintenance.ok(),
          "ordinary open succeeded during maintenance");
  Require(ordinary_during_maintenance.diagnostic.diagnostic_code ==
              "SB-DB-LIFECYCLE-RESTRICTED-OPEN-REQUIRED",
          "ordinary maintenance open did not report restricted-open required");
  const auto exited = db::ExitDatabaseMaintenanceMode(OperationConfig(fixture));
  Require(exited.ok(), "maintenance exit failed");
  startup = ReadStartup(fixture);
  Require(!startup.write_admission_fenced, "maintenance exit did not clear write fence");
  Require(startup.durable_lifecycle_phase == db::StartupLifecycleDurablePhase::maintenance_exited,
          "maintenance exit durable phase missing");
}

void TestEngineLifecycleApi(const Fixture& fixture, const Fixture& corrupt_fixture) {
  api::EngineVerifyLifecycleRequest verify;
  verify.context = EngineContext(fixture);
  const auto verified = api::EngineVerifyLifecycle(verify);
  Require(verified.ok, "engine verify rejected valid database");

  api::EngineRepairLifecycleRequest refused;
  refused.context = EngineContext(fixture);
  const auto refused_result = api::EngineRepairLifecycle(refused);
  Require(!refused_result.ok, "engine repair without plan succeeded");
  Require(HasDiagnostic(refused_result, "ENGINE.DBLC_REPAIR_REFUSED"),
          "engine repair refusal missing DBLC diagnostic");

  api::EngineEnterRestrictedOpenLifecycleRequest enter_restricted;
  enter_restricted.context = EngineContext(fixture);
  const auto restricted = api::EngineEnterRestrictedOpenLifecycle(enter_restricted);
  Require(restricted.ok, "engine restricted-open entry failed");

  api::EngineRepairLifecycleRequest repair;
  repair.context = EngineContext(fixture);
  repair.option_envelopes.push_back("repair_plan_id:clear_verified_write_fence");
  repair.option_envelopes.push_back("expected_filespace_uuid:" + fixture.filespace_uuid);
  repair.option_envelopes.push_back("repair_admission_proven:true");
  repair.option_envelopes.push_back("allow_repair:true");
  const auto repaired = api::EngineRepairLifecycle(repair);
  Require(repaired.ok, "engine accepted repair plan failed");

  api::EngineVerifyLifecycleRequest corrupt_verify;
  corrupt_verify.context = EngineContext(corrupt_fixture);
  const auto corrupt_result = api::EngineVerifyLifecycle(corrupt_verify);
  Require(!corrupt_result.ok, "engine verify accepted corrupted startup identity");
  Require(HasDiagnostic(corrupt_result, "ENGINE.DBLC_VERIFY_FAILED"),
          "engine verify failure missing DBLC diagnostic");

  api::EngineVerifyLifecycleRequest missing_security;
  missing_security.context = EngineContext(fixture);
  missing_security.context.security_context_present = false;
  const auto denied = api::EngineVerifyLifecycle(missing_security);
  Require(!denied.ok, "engine verify admitted missing security context");
  Require(HasDiagnostic(denied, "SB_ENGINE_API_SECURITY_CONTEXT_REQUIRED"),
          "engine verify missing-security diagnostic mismatch");
}

void TestSblrLifecycleRoute(const Fixture& fixture) {
  RequireSblrLifecycleSuccess(fixture,
                              "lifecycle.enter_maintenance",
                              "SBLR_LIFECYCLE_ENTER_MAINTENANCE");
  RequireSblrLifecycleSuccess(fixture,
                              "lifecycle.verify_database",
                              "SBLR_LIFECYCLE_VERIFY_DATABASE");

  const auto repair_refused = DispatchLifecycleSblr(
      fixture,
      "lifecycle.repair_database",
      "SBLR_LIFECYCLE_REPAIR_DATABASE");
  Require(repair_refused.envelope_validated, "SBLR repair-refusal envelope did not validate");
  Require(repair_refused.accepted, "SBLR repair-refusal dispatch did not accept operation");
  Require(repair_refused.dispatched_to_api, "SBLR repair-refusal did not reach API");
  Require(!repair_refused.api_result.ok, "SBLR repair without admission unexpectedly succeeded");
  Require(HasDiagnostic(repair_refused, "ENGINE.DBLC_REPAIR_REFUSED"),
          "SBLR repair refusal missing exact DBLC diagnostic");

  RequireSblrLifecycleSuccess(fixture,
                              "lifecycle.exit_maintenance",
                              "SBLR_LIFECYCLE_EXIT_MAINTENANCE");
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

ServerSessionRegistry RegistryWithPrincipal(const Fixture& fixture,
                                            std::string_view principal,
                                            std::array<std::uint8_t, 16>* session_uuid) {
  ServerSessionRegistry registry;
  ServerSessionRecord session;
  session.session_uuid = sbps::MakeUuidV7Bytes();
  session.principal_claim = std::string(principal);
  session.database_path = fixture.path.string();
  session.database_uuid = fixture.database_uuid;
  session.effective_user_uuid = sbps::MakeUuidV7Bytes();
  if (principal == "admin") {
    session.engine_authorization_trace_tags = {
        "right:OBS_MANAGEMENT_CONTROL",
        "right:OBS_MANAGEMENT_INSPECT",
        "right:OBS_CONFIG_CONTROL",
        "right:SUPPORT_EXPORT"};
  } else if (principal == "auditor") {
    session.engine_authorization_trace_tags = {
        "right:OBS_CONFIG_INSPECT",
        "right:OBS_METRICS_READ_ALL"};
  }
  *session_uuid = session.session_uuid;
  registry.sessions_by_uuid[scratchbird::server::UuidBytesToText(session.session_uuid)] = session;
  return registry;
}

ServerManagementContext ManagementContext(ServerBootstrapConfig* config,
                                          ServerLifecycleArtifacts* artifacts,
                                          HostedEngineState* engine_state,
                                          ServerSessionRegistry* registry,
                                          ServerMaintenanceCoordinator* coordinator) {
  ServerManagementContext context;
  context.config = config;
  context.artifacts = artifacts;
  context.engine_state = engine_state;
  context.session_registry = registry;
  context.maintenance_coordinator = coordinator;
  return context;
}

void TestManagementRoute(const Fixture& fixture) {
  ServerBootstrapConfig config;
  config.database_default_path = fixture.path;
  ServerLifecycleArtifacts artifacts;
  artifacts.generation = 7;
  artifacts.state = "dblc010-test";
  HostedEngineState engine_state;
  engine_state.engine_context_active = true;
  ServerMaintenanceCoordinator coordinator =
      scratchbird::server::BuildMaintenanceCoordinator(config, artifacts);

  std::array<std::uint8_t, 16> admin_uuid{};
  auto registry = RegistryWithPrincipal(fixture, "admin", &admin_uuid);
  auto context = ManagementContext(&config, &artifacts, &engine_state, &registry, &coordinator);

  auto enter = scratchbird::server::HandleServerManagementRequest(
      context, ManagementFrame(admin_uuid, "enter_restricted_open"));
  Require(enter.accepted && !enter.error, "management restricted-open entry failed");
  const std::string enter_payload(enter.payload.begin(), enter.payload.end());
  Require(Contains(enter_payload, "restricted_open_enabled"),
          "management restricted-open payload missing outcome");
  Require(coordinator.restricted_open_mode, "coordinator did not enter restricted-open");
  Require(!scratchbird::server::MaintenanceAllowsAttach(coordinator),
          "restricted-open did not fence ordinary attach");

  auto verify = scratchbird::server::HandleServerManagementRequest(
      context, ManagementFrame(admin_uuid, "verify_database"));
  Require(verify.accepted && !verify.error, "management verify failed");

  auto repair_refused = scratchbird::server::HandleServerManagementRequest(
      context, ManagementFrame(admin_uuid, "repair_database"));
  Require(repair_refused.error, "management repair without plan succeeded");
  Require(HasDiagnostic(repair_refused, "ENGINE.DBLC_REPAIR_REFUSED"),
          "management repair refusal missing DBLC diagnostic");

  std::string repair_mode = "repair_plan_id:clear_verified_write_fence;";
  repair_mode += "expected_database_uuid:" + fixture.database_uuid + ";";
  repair_mode += "expected_filespace_uuid:" + fixture.filespace_uuid + ";";
  repair_mode += "repair_admission_proven:true;allow_repair:true";
  auto repair = scratchbird::server::HandleServerManagementRequest(
      context, ManagementFrame(admin_uuid, "repair_database", repair_mode));
  Require(repair.accepted && !repair.error, "management accepted repair failed");
  Require(!coordinator.restricted_open_mode, "repair did not clear restricted-open coordinator state");

  std::array<std::uint8_t, 16> auditor_uuid{};
  auto auditor_registry = RegistryWithPrincipal(fixture, "auditor", &auditor_uuid);
  auto auditor_context =
      ManagementContext(&config, &artifacts, &engine_state, &auditor_registry, &coordinator);
  auto denied = scratchbird::server::HandleServerManagementRequest(
      auditor_context, ManagementFrame(auditor_uuid, "verify_database"));
  Require(denied.error, "auditor unexpectedly admitted to verify management control");
  Require(HasDiagnostic(denied, "SECURITY.ACCESS_DENIED"),
          "auditor denial diagnostic mismatch");
}

}  // namespace

int main() {
  scratchbird::tests::database_lifecycle::ConfigureLifecycleMemoryFixture(
      "database_lifecycle_maintenance_repair_conformance");
  const auto temp_dir = MakeTempDir();
  const auto fixture = CreateOpenCleanDatabase(temp_dir / "dblc010.sbdb", 1779300001000);
  const auto corrupt_fixture = CreateOpenCleanDatabase(temp_dir / "dblc010_corrupt.sbdb", 1779300002000);
  MutateStartup(corrupt_fixture, [](db::StartupStateRecord* state) {
    state->database_uuid = uuid::GenerateEngineIdentityV7(UuidKind::database, 1779300003000).value;
  });

  TestStorageMaintenanceRepair(fixture);
  TestEngineLifecycleApi(fixture, corrupt_fixture);
  TestSblrLifecycleRoute(fixture);
  TestManagementRoute(fixture);

  std::filesystem::remove_all(temp_dir);
  return EXIT_SUCCESS;
}

// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "agent_engine_lifecycle.hpp"
#include "database_lifecycle.hpp"
#include "database_lifecycle_test_memory.hpp"
#include "engine_host.hpp"
#include "session_registry.hpp"
#include "sbps.hpp"
#include "uuid.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>

namespace {

namespace agents = scratchbird::core::agents;
namespace db = scratchbird::storage::database;
namespace uuid = scratchbird::core::uuid;
namespace sbps = scratchbird::server::sbps;
using scratchbird::core::platform::UuidKind;

constexpr std::string_view kVerifier =
    "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
constexpr std::string_view kAlicePrincipalUuid =
    "019e108d-1700-7000-8000-0000000013aa";

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view message) {
  if (!condition) {
    Fail(message);
  }
}

bool Contains(std::string_view haystack, std::string_view needle) {
  return haystack.find(needle) != std::string_view::npos;
}

bool ContainsAgent(const std::vector<std::string>& agents, std::string_view type_id) {
  for (const auto& agent : agents) {
    if (agent == type_id) return true;
  }
  return false;
}

void PutU16(std::vector<std::uint8_t>* out, std::uint16_t value) {
  out->push_back(static_cast<std::uint8_t>(value & 0xffu));
  out->push_back(static_cast<std::uint8_t>((value >> 8u) & 0xffu));
}

void PutUuid(std::vector<std::uint8_t>* out,
             const std::array<std::uint8_t, 16>& uuid_bytes) {
  out->insert(out->end(), uuid_bytes.begin(), uuid_bytes.end());
}

void PutString(std::vector<std::uint8_t>* out, std::string_view value) {
  PutU16(out, static_cast<std::uint16_t>(value.size()));
  out->insert(out->end(), value.begin(), value.end());
}

std::uint64_t CurrentUnixMillis() {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());
}

std::filesystem::path TestDatabasePath() {
  return std::filesystem::temp_directory_path() /
         ("sb_dblc013h_engine_agent_" + std::to_string(CurrentUnixMillis()) + ".sbdb");
}

agents::DatabaseEngineAgentInput ValidAgentInput() {
  agents::DatabaseEngineAgentInput input;
  input.database_uuid = "019e0f2a-0000-7000-8000-000000000013";
  input.engine_instance_uuid = "engine-instance:019e0f2a-0000-7000-8000-000000000013";
  input.database_lifecycle_state = "opened";
  input.lifecycle_mode = agents::AgentLifecycleMode::database_open;
  input.policy_generation = 7;
  input.catalog_generation = 7;
  input.security_generation = 7;
  input.filespace_generation = 3;
  input.agent_set_generation = 2;
  input.health_generation = 2;
  input.tx1_bootstrap_visible = true;
  input.tx2_activation_committed = true;
  input.startup_admitted = true;
  input.health_publication_allowed = true;
  input.health_publication_persisted = true;
  input.allow_degraded_service = true;
  return input;
}

void TestDirectAgentLifecycle() {
  const auto result = agents::StartDatabaseEngineLifecycleAgent(ValidAgentInput());
  Require(result.ok(), "valid engine lifecycle agent start failed");
  Require(result.health.agent_state == agents::DatabaseEngineAgentLifecycleState::active,
          "valid engine lifecycle agent did not become active");
  Require(result.health.ordinary_admission_allowed,
          "active lifecycle agent did not admit ordinary work");
  Require(!result.health.selected_agent_type_ids.empty(),
          "engine lifecycle agent selected no database-local agents");
  Require(ContainsAgent(result.health.selected_agent_type_ids, "storage_health_manager"),
          "engine lifecycle agent did not select storage health manager");
  Require(!ContainsAgent(result.health.selected_agent_type_ids, "cluster_autoscale_manager"),
          "engine lifecycle agent selected a cluster-only agent in standalone mode");
  Require(result.health.cluster_paths_failed_closed,
          "engine lifecycle agent did not record standalone cluster fail-closed behavior");
  Require(agents::DatabaseEngineAgentAuthorityBoundaryValid(result.health.authority_boundary),
          "engine lifecycle agent authority boundary is invalid");
  const auto json = agents::SerializeDatabaseEngineAgentHealthJson(result.health, false);
  Require(Contains(json, "\"agent_state\":\"active\""),
          "engine lifecycle health JSON missing active state");
  Require(Contains(json, "\"authority_boundary_valid\":true"),
          "engine lifecycle health JSON missing authority boundary");
}

void TestTx2RequiredBeforeStart() {
  auto input = ValidAgentInput();
  input.tx2_activation_committed = false;
  const auto result = agents::StartDatabaseEngineLifecycleAgent(input);
  Require(!result.ok(), "engine lifecycle agent started before tx2 evidence");
  Require(result.status.diagnostic_code == "ENGINE.AGENT_LIFECYCLE_INPUT_INVALID",
          "tx2 refusal diagnostic mismatch");
  Require(result.health.agent_state == agents::DatabaseEngineAgentLifecycleState::not_started,
          "tx2 refusal did not leave lifecycle agent not_started");
}

void TestDegradedSafeModeAndQuarantine() {
  auto degraded = ValidAgentInput();
  degraded.unhealthy_agent_type_ids.push_back("support_bundle_triage_agent");
  const auto degraded_result = agents::StartDatabaseEngineLifecycleAgent(degraded);
  Require(degraded_result.ok(), "noncritical unhealthy agent failed health publication");
  Require(degraded_result.health.agent_state == agents::DatabaseEngineAgentLifecycleState::degraded,
          "noncritical unhealthy agent did not produce degraded state");
  Require(degraded_result.health.ordinary_admission_allowed,
          "degraded policy did not allow ordinary admission");
  Require(Contains(agents::SerializeDatabaseEngineAgentHealthJson(degraded_result.health, false),
                   "ENGINE.AGENT_HEALTH_DEGRADED"),
          "degraded diagnostic was not published");

  auto safe = ValidAgentInput();
  safe.unhealthy_agent_type_ids.push_back("storage_health_manager");
  const auto safe_result = agents::StartDatabaseEngineLifecycleAgent(safe);
  Require(safe_result.ok(), "critical unhealthy agent failed to publish safe-mode health");
  Require(safe_result.health.agent_state == agents::DatabaseEngineAgentLifecycleState::safe_mode,
          "critical unhealthy agent did not force safe mode");
  Require(!safe_result.health.ordinary_admission_allowed,
          "safe mode incorrectly admitted ordinary work");

  auto quarantine = ValidAgentInput();
  quarantine.quarantine_required = true;
  const auto quarantine_result = agents::StartDatabaseEngineLifecycleAgent(quarantine);
  Require(quarantine_result.ok(), "quarantine health publication failed");
  Require(quarantine_result.health.agent_state == agents::DatabaseEngineAgentLifecycleState::quarantined,
          "quarantine policy did not produce quarantined state");
  Require(!quarantine_result.health.ordinary_admission_allowed,
          "quarantined agent incorrectly admitted ordinary work");
}

void TestShutdownStopAndAuthorityDenial() {
  const auto started = agents::StartDatabaseEngineLifecycleAgent(ValidAgentInput());
  Require(started.ok(), "shutdown precondition start failed");
  auto stop_input = ValidAgentInput();
  stop_input.database_lifecycle_state = "closed";
  stop_input.lifecycle_mode = agents::AgentLifecycleMode::shutdown;
  stop_input.shutdown_requested = true;
  stop_input.health_generation = started.health.health_generation + 1;
  const auto stopped = agents::StopDatabaseEngineLifecycleAgent(stop_input, started.health);
  Require(stopped.ok(), "engine lifecycle agent stop failed");
  Require(stopped.health.agent_state == agents::DatabaseEngineAgentLifecycleState::stopped,
          "engine lifecycle agent did not stop during shutdown");
  Require(stopped.health.shutdown_coordination_complete,
          "engine lifecycle agent did not report shutdown coordination complete");
  Require(!stopped.health.ordinary_admission_allowed,
          "stopped engine lifecycle agent admitted ordinary work");

  agents::DatabaseEngineAgentAuthorityBoundary invalid;
  invalid.transaction_finality_authority = true;
  const auto authority = agents::ValidateDatabaseEngineAgentAuthorityBoundary(invalid);
  Require(!authority.ok, "invalid authority boundary was accepted");
  Require(authority.diagnostic_code == "ENGINE.AGENT_LIFECYCLE_AUTHORITY_DENIED",
          "authority boundary diagnostic mismatch");
}

std::string CreateOpenCleanDatabase(const std::filesystem::path& path) {
  const auto now = CurrentUnixMillis();
  auto database_uuid = uuid::GenerateEngineIdentityV7(UuidKind::database, now);
  auto filespace_uuid = uuid::GenerateEngineIdentityV7(UuidKind::filespace, now + 1);
  Require(database_uuid.ok() && filespace_uuid.ok(), "DBLC-013H UUID generation failed");

  db::DatabaseCreateConfig create;
  create.path = path.string();
  create.database_uuid = database_uuid.value;
  create.filespace_uuid = filespace_uuid.value;
  create.page_size = 16384;
  create.creation_unix_epoch_millis = now + 2;
  create.allow_minimal_resource_bootstrap = true;
  create.require_resource_seed_pack = false;
  create.allow_overwrite = true;
  const auto created = db::CreateDatabaseFile(create);
  Require(created.ok(), "DBLC-013H create failed");
  Require(created.state.engine_agent_health_present,
          "create did not return lifecycle agent health publication");
  Require(created.state.engine_agent_health.agent_state ==
              agents::DatabaseEngineAgentLifecycleState::not_started,
          "create started engine lifecycle agent before tx2");

  const auto opened = db::OpenDatabaseFile({path.string(), false, false, false});
  Require(opened.ok(), "DBLC-013H first open failed");
  Require(opened.state.engine_agent_health_present,
          "first open did not return lifecycle agent health publication");
  Require(opened.state.startup_state.first_open_activation_local_transaction_id == 2,
          "first open did not commit tx2 before agent startup");
  Require(opened.state.engine_agent_health.agent_state ==
              agents::DatabaseEngineAgentLifecycleState::active,
          "first open did not start active lifecycle agent");
  Require(opened.state.engine_agent_health.ordinary_admission_allowed,
          "active database lifecycle agent did not admit ordinary work");
  Require(Contains(opened.state.engine_agent_health_json, "\"database_engine_agent\""),
          "open result did not serialize lifecycle agent health");

  const auto clean = db::MarkDatabaseCleanShutdown(path.string());
  Require(clean.ok(), "DBLC-013H clean shutdown failed");
  const auto inspected = db::InspectDatabaseLifecycle({path.string(), false, false, {}, {}, false});
  Require(inspected.ok(), "DBLC-013H inspect after shutdown failed");
  Require(inspected.state.engine_agent_health.agent_state ==
              agents::DatabaseEngineAgentLifecycleState::not_started,
          "clean shutdown did not stop lifecycle agent runtime");
  return uuid::UuidToString(create.database_uuid.value);
}

void WriteAuthStore(const std::filesystem::path& database_path,
                    const std::string& database_uuid) {
  scratchbird::tests::database_lifecycle::CreateDurableLocalPasswordPrincipal(
      database_path,
      database_uuid,
      kAlicePrincipalUuid,
      "alice",
      kVerifier,
      17,
      "DBLC-013H");
}

std::string Evidence() {
  return scratchbird::tests::database_lifecycle::DurableLocalPasswordEvidence(
      "alice",
      kAlicePrincipalUuid,
      kVerifier,
      "right:CONNECT,right:OBS_RUNTIME_ALL");
}

std::vector<std::uint8_t> AuthPayload() {
  std::vector<std::uint8_t> out;
  PutUuid(&out, sbps::MakeUuidV7Bytes());
  out.push_back(1);
  out.push_back(0);
  out.push_back(0);
  out.push_back(0);
  PutString(&out, "local_password");
  PutString(&out, "alice");
  PutString(&out, "default");
  PutString(&out, "en");
  PutString(&out, Evidence());
  return out;
}

sbps::Frame Frame(sbps::MessageType type, std::vector<std::uint8_t> payload) {
  sbps::Frame frame;
  frame.header.message_type = static_cast<std::uint16_t>(type);
  frame.header.request_uuid = sbps::MakeUuidV7Bytes();
  frame.header.payload_schema_id = sbps::kSchemaNone;
  frame.payload = std::move(payload);
  return frame;
}

void TestHostedEngineAndSessionHealthPublication(const std::filesystem::path& path) {
  const auto database_uuid = CreateOpenCleanDatabase(path);
  WriteAuthStore(path, database_uuid);

  scratchbird::server::ServerBootstrapConfig config;
  config.database_default_path = path;
  config.database_auto_create = false;
  config.sbps_enabled = true;
  config.security_default_policy_installed = true;
  config.security_provider_family = "local_password";
  config.security_provider_state = "healthy";
  const auto hosted = scratchbird::server::StartHostedEngine(config);
  Require(hosted.ok(), "hosted engine start failed");
  Require(hosted.state.databases.size() == 1, "hosted engine did not expose one database");
  const auto& database = hosted.state.databases.front();
  Require(database.database_engine_agent_state == "active",
          "hosted engine did not expose active lifecycle agent state");
  Require(database.database_engine_agent_ordinary_admission_allowed,
          "hosted engine did not expose ordinary admission health");
  Require(Contains(database.database_engine_agent_health_json, "\"database_engine_agent\""),
          "hosted engine did not expose lifecycle health JSON");
  Require(Contains(scratchbird::server::HostedEngineStatusJson(hosted.state),
                   "\"database_engine_agent_health\""),
          "hosted engine status JSON omitted lifecycle health");

  scratchbird::server::ServerSessionRegistry registry;
  auto auth = scratchbird::server::HandleAuthHandoff(
      &registry,
      hosted.state,
      Frame(sbps::MessageType::kAuthHandoff, AuthPayload()));
  Require(auth.accepted, "DBLC-013H auth handoff failed");
  const auto auth_context =
      scratchbird::server::DecodeAuthContextUuidForTest(auth.payload);
  Require(auth_context.has_value(), "DBLC-013H auth context decode failed");
  Require(Contains(std::string(auth.payload.begin(), auth.payload.end()),
                   "\"database_engine_agent\""),
          "auth result did not carry lifecycle health publication");

  auto attach = scratchbird::server::HandleAttachDatabase(
      &registry,
      hosted.state,
      Frame(sbps::MessageType::kAttachDatabase,
            scratchbird::server::EncodeAttachPayloadForTest(*auth_context, "read_write")));
  Require(attach.accepted, "DBLC-013H attach failed");
  Require(Contains(std::string(attach.payload.begin(), attach.payload.end()),
                   "\"database_engine_agent\""),
          "attach result did not carry lifecycle health publication");
  Require(registry.sessions_by_uuid.size() == 1,
          "DBLC-013H attach did not create one session");
  const auto& session = registry.sessions_by_uuid.begin()->second;
  Require(session.database_engine_agent_state == "active",
          "session did not retain lifecycle agent state");
  Require(session.database_engine_agent_ordinary_admission_allowed,
          "session did not retain lifecycle ordinary-admission health");
  Require(Contains(scratchbird::server::SessionRegistryStatusJson(registry),
                   "\"database_engine_agent_health\""),
          "session registry status omitted lifecycle health");
}

}  // namespace

int main() {
  scratchbird::tests::database_lifecycle::ConfigureLifecycleMemoryFixture(
      "database_lifecycle_engine_agent_conformance");
  TestDirectAgentLifecycle();
  TestTx2RequiredBeforeStart();
  TestDegradedSafeModeAndQuarantine();
  TestShutdownStopAndAuthorityDenial();

  const auto database_path = TestDatabasePath();
  struct Cleanup {
    std::filesystem::path path;
    ~Cleanup() {
      if (!path.empty()) {
        std::error_code ignored;
        std::filesystem::remove(path, ignored);
        std::filesystem::remove(path.string() + ".sb.local_password_auth", ignored);
      }
    }
  } cleanup{database_path};
  TestHostedEngineAndSessionHealthPublication(database_path);
  return EXIT_SUCCESS;
}

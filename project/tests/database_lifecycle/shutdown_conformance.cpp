#include "../support/binary_uuid_fixture.hpp"
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
#include "wire/management_request_codec.hpp"
#include "../../drivers/tool/cli/binary_status_display.hpp"
#include "sbps.hpp"
#include "startup_state.hpp"
#include "uuid.hpp"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <type_traits>
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
  scratchbird::core::platform::Uuid database_uuid;
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
  fixture.database_uuid = create.database_uuid.value;
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
  std::vector<scratchbird::wire::public_result::Field> fields;
  Require(scratchbird::wire::binary_status::Decode(result.records_json, &fields),
          "joined shutdown records are not a valid binary status packet");
  std::size_t database_atoms = 0;
  std::size_t uuid_atoms = 0;
  for (const auto& field : fields) {
    if (field.kind != scratchbird::wire::public_result::Kind::uuid) continue;
    ++uuid_atoms;
    Require(field.value.size() == 16, "shutdown UUID atom is not binary16");
    if (field.value == scratchbird::wire::ManagementTargetBytes(fixture.database_uuid))
      ++database_atoms;
  }
  Require(database_atoms == 4 && uuid_atoms == 7,
          "coordinator/runtime/storage array joining changed UUID atoms");
  const auto display = scratchbird::cli::RenderBinaryStatus(result.records_json);
  Require(display && display->starts_with("[{") && display->ends_with("}]") &&
          Contains(*display, "},{"), "client could not render joined shutdown records");

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
              "force_termination_policy_uuid:" + scratchbird::wire::ManagementTargetBytes(
                  scratchbird::tests::FixtureUuidLiteral("019e1100-0000-7000-8000-000000000011")) + ";" +
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

void TestNativeShutdownScopeRefusesBeforeMutation(const std::filesystem::path& dir) {
  using NativeUuid = scratchbird::core::platform::Uuid;
  static_assert(!std::is_assignable_v<decltype(ServerShutdownRuntimeSnapshot::database_uuid)&,
                                      std::string>);
  static_assert(!std::is_assignable_v<decltype(ServerMaintenanceCoordinator::database_uuid)&,
                                      std::string>);
  static_assert(!std::is_assignable_v<decltype(ServerMaintenanceCoordinator::shutdown_database_uuid)&,
                                      std::string>);
  const auto fixture = CreateActiveDatabase(dir / "identity_refusals.sbdb", 1779400005000);
  const auto config = Config(fixture);
  const auto before = ReadStartup(fixture);
  NativeUuid v4 = fixture.database_uuid;
  v4.bytes[6] = static_cast<std::uint8_t>((v4.bytes[6] & 15) | 0x40);
  NativeUuid bad_variant = fixture.database_uuid;
  bad_variant.bytes[8] &= 0x3f;
  for (const auto& invalid : {NativeUuid{}, v4, bad_variant}) {
    auto coordinator = Coordinator(config);
    auto snapshot = Snapshot(fixture);
    snapshot.database_uuid = invalid;
    const auto result = scratchbird::server::ApplyDatabaseShutdownOperation(
        &coordinator, config, Request("shutdown_database"), snapshot);
    Require(!result.ok && HasDiagnostic(result, "ENGINE.SHUTDOWN_INPUT_INVALID"),
            "invalid snapshot node identity was not refused");
    Require(coordinator.state == "running" && !coordinator.attach_admission_fenced &&
                coordinator.database_uuid.is_nil() && coordinator.shutdown_database_uuid.is_nil(),
            "invalid snapshot changed node scope or shutdown fences");
  }
  auto coordinator = Coordinator(config);
  auto foreign = fixture.database_uuid;
  foreign.bytes[15] ^= 0x80;
  auto request = Request("shutdown_database");
  request.target_uuid = scratchbird::wire::ManagementTargetBytes(foreign);
  const auto result = scratchbird::server::ApplyDatabaseShutdownOperation(
      &coordinator, config, request, Snapshot(fixture));
  Require(!result.ok && HasDiagnostic(result, "ENGINE.SHUTDOWN_SCOPE_INVALID"),
          "equal path rescued a different requested node identity");
  Require(coordinator.state == "running" && !coordinator.attach_admission_fenced,
          "foreign requested node mutated shutdown state");
  for (const bool shutdown_binding : {false, true}) {
    coordinator = Coordinator(config);
    if (shutdown_binding) coordinator.shutdown_database_uuid = foreign;
    else coordinator.database_uuid = foreign;
    const auto bound_refusal = scratchbird::server::ApplyDatabaseShutdownOperation(
        &coordinator, config, Request("shutdown_database"), Snapshot(fixture));
    Require(!bound_refusal.ok && HasDiagnostic(bound_refusal, "ENGINE.SHUTDOWN_SCOPE_INVALID"),
            "shutdown rebound an existing coordinator to a different node");
    Require(coordinator.state == "running" && !coordinator.attach_admission_fenced &&
                (shutdown_binding ? coordinator.shutdown_database_uuid : coordinator.database_uuid) == foreign,
            "shutdown refusal changed retained coordinator identity");
  }
  const auto after = ReadStartup(fixture);
  Require(!after.clean_shutdown &&
              after.clean_shutdown_local_transaction_id == before.clean_shutdown_local_transaction_id &&
              after.durable_lifecycle_phase == before.durable_lifecycle_phase,
          "identity refusal mutated durable lifecycle state");
}

void TestDurableShutdownChecksTheOpenedNode(const std::filesystem::path& dir) {
  const auto fixture = CreateActiveDatabase(dir / "durable_identity.sbdb", 1779400006000);
  const auto bytes = [&] {
    disk::FileDevice device;
    Require(device.Open(fixture.path.string(), disk::FileOpenMode::open_existing_read_only).ok(),
            "cannot inspect durable shutdown identity fixture");
    std::vector<std::uint8_t> data(std::filesystem::file_size(fixture.path));
    Require(device.ReadAt(0, data.data(), data.size()).ok(), "cannot read identity fixture bytes");
    return data;
  };
  const auto before = bytes();
  auto foreign = fixture.database_uuid;
  foreign.bytes[15] ^= 0x80;
  auto v4 = fixture.database_uuid;
  v4.bytes[6] = static_cast<std::uint8_t>((v4.bytes[6] & 15) | 0x40);
  auto invalid_variant = fixture.database_uuid;
  invalid_variant.bytes[8] &= 0x3f;
  for (const auto& invalid : {scratchbird::core::platform::Uuid{}, v4, invalid_variant, foreign}) {
    const auto refused = db::MarkDatabaseCleanShutdown(fixture.path.string(), invalid);
    Require(!refused.ok(), "shutdown accepted an invalid or foreign expected node");
    Require(refused.diagnostic.diagnostic_code ==
                (invalid == foreign ? "SB-DB-LIFECYCLE-SHUTDOWN-IDENTITY-MISMATCH"
                                    : "SB-DB-LIFECYCLE-SHUTDOWN-IDENTITY-INVALID"),
            "durable shutdown identity refusal diagnostic mismatch");
    Require(bytes() == before, "refused shutdown modified bytes of the opened database");
  }
  // The server must pass its claimed native identity all the way to the opened
  // file. An otherwise complete snapshot cannot bless a different node's path.
  auto snapshot = Snapshot(fixture);
  snapshot.database_uuid = foreign;
  const auto config = Config(fixture);
  auto coordinator = Coordinator(config);
  const auto refused = scratchbird::server::ApplyDatabaseShutdownOperation(
      &coordinator, config, Request("shutdown_database"), snapshot);
  Require(!refused.ok && HasDiagnostic(refused, "SB-DB-LIFECYCLE-SHUTDOWN-IDENTITY-MISMATCH"),
          "server shutdown did not retain expected node at the storage boundary");
  Require(bytes() == before, "server shutdown marked a foreign file clean");
  Require(db::MarkDatabaseCleanShutdown(fixture.path.string(), fixture.database_uuid).ok(),
          "exact native owner could not mark database clean");
  Require(ReadStartup(fixture).clean_shutdown, "exact-owner shutdown did not persist clean state");
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
  TestNativeShutdownScopeRefusesBeforeMutation(temp_dir);
  TestDurableShutdownChecksTheOpenedNode(temp_dir);
  std::filesystem::remove_all(temp_dir);
  return EXIT_SUCCESS;
}

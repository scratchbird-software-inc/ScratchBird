// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "config.hpp"
#include "database_lifecycle.hpp"
#include "server_daemon_lifecycle.hpp"
#include "uuid.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <unistd.h>
#include <vector>

namespace {

using scratchbird::server::EvaluateServerDaemonLifecycle;
using scratchbird::server::HostedDatabaseSnapshot;
using scratchbird::server::HostedDatabaseState;
using scratchbird::server::HostedEngineState;
using scratchbird::server::ResolveServerBootstrapConfig;
using scratchbird::server::ServerBootstrapConfig;
using scratchbird::server::ServerCliOptions;
using scratchbird::server::ServerDaemonLifecycleSnapshot;
using scratchbird::server::ServerDaemonShouldStopForDatabaseShutdown;
using scratchbird::server::ServerLifecycleArtifacts;
namespace db = scratchbird::storage::database;
namespace uuid = scratchbird::core::uuid;

void Require(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << message << '\n';
    std::exit(EXIT_FAILURE);
  }
}

bool Contains(std::string_view haystack, std::string_view needle) {
  return haystack.find(needle) != std::string_view::npos;
}

bool HasDiagnostic(const ServerDaemonLifecycleSnapshot& snapshot,
                   std::string_view code) {
  for (const auto& diagnostic : snapshot.diagnostics) {
    if (diagnostic.code == code) return true;
  }
  return false;
}

std::filesystem::path MakeTempDir() {
  std::string tmpl = "/tmp/sb_dblc013e_server_daemon.XXXXXX";
  std::vector<char> writable(tmpl.begin(), tmpl.end());
  writable.push_back('\0');
  char* made = ::mkdtemp(writable.data());
  Require(made != nullptr, "mkdtemp failed for DBLC-013E server daemon test");
  return std::filesystem::path(made);
}

ServerLifecycleArtifacts Artifacts() {
  ServerLifecycleArtifacts artifacts;
  artifacts.generation = 1305;
  artifacts.state = "config_lifecycle_ready";
  return artifacts;
}

ServerBootstrapConfig Config(std::string_view scope = "shared") {
  ServerBootstrapConfig config;
  config.database_default_path = "/tmp/sb_dblc013e_target.sbdb";
  config.database_daemon_scope = std::string(scope);
  config.sbps_enabled = true;
  return config;
}

HostedDatabaseSnapshot Database(std::string_view uuid,
                                std::string_view path,
                                HostedDatabaseState state,
                                bool open) {
  HostedDatabaseSnapshot database;
  database.state = state;
  database.database_uuid = std::string(uuid);
  database.database_path = std::string(path);
  database.database_open = open;
  database.write_admission_fenced = !open;
  if (state == HostedDatabaseState::kQuarantined) {
    database.diagnostic_code = "SERVER.DAEMON.TEST_QUARANTINE";
  }
  if (state == HostedDatabaseState::kFailed) {
    database.diagnostic_code = "SERVER.DAEMON.TEST_FAILED";
  }
  return database;
}

HostedEngineState Engine(std::initializer_list<HostedDatabaseSnapshot> databases) {
  HostedEngineState state;
  state.engine_context_active = true;
  state.databases.assign(databases.begin(), databases.end());
  return state;
}

void TestSharedDaemonServiceReadyAndIsolation() {
  auto config = Config("shared");
  const auto engine = Engine({
      Database("019e1305-0000-7000-8000-000000000001",
               "/tmp/sb_dblc013e_target.sbdb",
               HostedDatabaseState::kOpen,
               true),
      Database("019e1305-0000-7000-8000-000000000002",
               "/tmp/sb_dblc013e_other.sbdb",
               HostedDatabaseState::kOpen,
               true),
  });
  const auto snapshot = EvaluateServerDaemonLifecycle(config, Artifacts(), engine);
  Require(snapshot.service_ready, "shared daemon did not become service-ready");
  Require(snapshot.shared_daemon_has_other_databases,
          "shared daemon did not record unrelated database association");
  Require(!ServerDaemonShouldStopForDatabaseShutdown(snapshot,
                                                     "019e1305-0000-7000-8000-000000000001"),
          "shared daemon would stop for one target database shutdown");
  const auto status = scratchbird::server::ServerDaemonLifecycleStatusJson(snapshot);
  Require(Contains(status, "\"daemon_scope\":\"shared\""),
          "server daemon status missing shared scope");
  Require(Contains(status, "\"service_ready\":true"),
          "server daemon status missing service-ready state");
}

void TestDedicatedDaemonExclusiveStopDecision() {
  auto config = Config("dedicated");
  const auto engine = Engine({
      Database("019e1305-0000-7000-8000-000000000101",
               "/tmp/sb_dblc013e_dedicated.sbdb",
               HostedDatabaseState::kOpen,
               true),
  });
  const auto snapshot = EvaluateServerDaemonLifecycle(config, Artifacts(), engine);
  Require(snapshot.service_ready, "dedicated daemon did not become service-ready");
  Require(snapshot.daemon_exclusive_to_database,
          "dedicated daemon did not record exclusive database association");
  Require(ServerDaemonShouldStopForDatabaseShutdown(snapshot,
                                                   "019e1305-0000-7000-8000-000000000101"),
          "dedicated daemon would not stop for its exclusive database shutdown");
  Require(!ServerDaemonShouldStopForDatabaseShutdown(snapshot,
                                                    "019e1305-0000-7000-8000-000000000202"),
          "dedicated daemon would stop for unrelated database shutdown");
}

void TestDedicatedDaemonRefusesAmbiguousScope() {
  auto config = Config("dedicated");
  const auto engine = Engine({
      Database("019e1305-0000-7000-8000-000000000201",
               "/tmp/sb_dblc013e_a.sbdb",
               HostedDatabaseState::kOpen,
               true),
      Database("019e1305-0000-7000-8000-000000000202",
               "/tmp/sb_dblc013e_b.sbdb",
               HostedDatabaseState::kOpen,
               true),
  });
  const auto snapshot = EvaluateServerDaemonLifecycle(config, Artifacts(), engine);
  Require(!snapshot.service_ready, "ambiguous dedicated daemon became service-ready");
  Require(snapshot.scope_ambiguous, "ambiguous dedicated daemon scope was not recorded");
  Require(HasDiagnostic(snapshot, "SERVER.DAEMON.SCOPE_AMBIGUOUS"),
          "ambiguous dedicated daemon diagnostic mismatch");
}

void TestHostedDatabaseFailureRequiresQuarantine() {
  auto config = Config("shared");
  const auto engine = Engine({
      Database("019e1305-0000-7000-8000-000000000301",
               "/tmp/sb_dblc013e_failed.sbdb",
               HostedDatabaseState::kFailed,
               false),
  });
  const auto snapshot = EvaluateServerDaemonLifecycle(config, Artifacts(), engine);
  Require(!snapshot.service_ready, "failed hosted database became service-ready");
  Require(snapshot.quarantine_required, "failed hosted database did not require quarantine");
  Require(HasDiagnostic(snapshot, "SERVER.DAEMON.HOSTED_DATABASE_FAILED"),
          "failed hosted database diagnostic mismatch");
}

void TestDaemonScopeConfigurationValidation(const std::filesystem::path& dir) {
  const auto valid_path = dir / "valid.conf";
  {
    std::ofstream out(valid_path);
    out << "[config]\nformat = SBCD1\n"
        << "[server.database]\n"
        << "default_path = \"" << (dir / "valid.sbdb").generic_string() << "\"\n"
        << "daemon_scope = dedicated\n";
  }
  ServerCliOptions valid_cli;
  valid_cli.config_path = valid_path.string();
  const auto valid = ResolveServerBootstrapConfig(valid_cli);
  Require(valid.ok(), "valid daemon scope config was rejected");
  Require(valid.config.database_daemon_scope == "dedicated",
          "valid daemon scope config was not applied");

  const auto invalid_path = dir / "invalid.conf";
  {
    std::ofstream out(invalid_path);
    out << "[config]\nformat = SBCD1\n"
        << "[server.database]\n"
        << "default_path = \"" << (dir / "invalid.sbdb").generic_string() << "\"\n"
        << "daemon_scope = crosswired\n";
  }
  ServerCliOptions invalid_cli;
  invalid_cli.config_path = invalid_path.string();
  const auto invalid = ResolveServerBootstrapConfig(invalid_cli);
  Require(!invalid.ok(), "invalid daemon scope config was accepted");
  bool found = false;
  for (const auto& diagnostic : invalid.diagnostics) {
    if (diagnostic.code == "CONFIG.VALUE_INVALID_ENUM") found = true;
  }
  Require(found, "invalid daemon scope diagnostic mismatch");
}

void TestHostedEnginePublishesDurableDatabaseUuid(const std::filesystem::path& dir) {
  const auto path = dir / "hosted_identity.sbdb";
  const auto database_uuid = uuid::GenerateEngineIdentityV7(
      scratchbird::core::platform::UuidKind::database,
      1779420001000);
  const auto filespace_uuid = uuid::GenerateEngineIdentityV7(
      scratchbird::core::platform::UuidKind::filespace,
      1779420001001);
  Require(database_uuid.ok() && filespace_uuid.ok(),
          "DBLC-013E identity UUID generation failed");

  db::DatabaseCreateConfig create;
  create.path = path.string();
  create.database_uuid = database_uuid.value;
  create.filespace_uuid = filespace_uuid.value;
  create.page_size = 16384;
  create.creation_unix_epoch_millis = 1779420001002;
  create.allow_minimal_resource_bootstrap = true;
  create.require_resource_seed_pack = false;
  create.allow_overwrite = true;
  const auto created = db::CreateDatabaseFile(create);
  Require(created.ok(), "DBLC-013E hosted identity database create failed");

  auto config = Config("shared");
  config.database_default_path = path;
  config.database_auto_create = false;
  const auto hosted = scratchbird::server::StartHostedEngine(config);
  if (!hosted.ok()) {
    for (const auto& diagnostic : hosted.diagnostics) {
      std::cerr << diagnostic.code << ':' << diagnostic.safe_message << '\n';
    }
  }
  Require(hosted.ok(), "DBLC-013E hosted engine open failed");
  Require(hosted.state.databases.size() == 1,
          "DBLC-013E hosted engine database count mismatch");
  const auto expected = uuid::UuidToString(database_uuid.value.value);
  Require(hosted.state.databases.front().database_uuid == expected,
          "hosted engine did not publish durable database UUID");
  Require(!Contains(hosted.state.databases.front().database_uuid, "engine-public-abi:"),
          "hosted engine published synthetic database UUID");
}

}  // namespace

int main() {
  const auto dir = MakeTempDir();
  TestSharedDaemonServiceReadyAndIsolation();
  TestDedicatedDaemonExclusiveStopDecision();
  TestDedicatedDaemonRefusesAmbiguousScope();
  TestHostedDatabaseFailureRequiresQuarantine();
  TestDaemonScopeConfigurationValidation(dir);
  TestHostedEnginePublishesDurableDatabaseUuid(dir);
  std::filesystem::remove_all(dir);
  return EXIT_SUCCESS;
}

// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "config.hpp"
#include "database_lifecycle.hpp"
#include "memory.hpp"
#include "server_daemon_lifecycle.hpp"
#include "uuid.hpp"

#include <cstdlib>
#include <cerrno>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <thread>
#include <string>
#include <string_view>
#include <unistd.h>
#include <sys/wait.h>
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

ServerBootstrapConfig Config(std::string_view scope = "dedicated") {
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

void TestSharedDaemonRefused() {
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
  Require(!snapshot.service_ready && snapshot.state == "failed",
          "forbidden shared multi-database daemon became service-ready");
  Require(HasDiagnostic(snapshot, "SERVER.DAEMON.SCOPE_INVALID"),
          "shared daemon scope was not explicitly refused");
  Require(!ServerDaemonShouldStopForDatabaseShutdown(snapshot,
                                                     "019e1305-0000-7000-8000-000000000001"),
          "shared daemon would stop for one target database shutdown");
  const auto status = scratchbird::server::ServerDaemonLifecycleStatusJson(snapshot);
  Require(Contains(status, "\"daemon_scope\":\"shared\""),
          "server daemon status missing shared scope");
  Require(Contains(status, "\"service_ready\":false"),
          "server daemon reported false readiness");
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
  auto config = Config();
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
  Require(ServerBootstrapConfig{}.database_daemon_scope == "dedicated",
          "default server configuration permits shared database hosting");
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
  Require(valid.config.control_dir.filename() == valid.config.database_runtime_scope_id &&
              valid.config.data_dir.filename() == valid.config.database_runtime_scope_id,
          "compiled runtime defaults were not scoped to the database instance");
  valid_cli.control_dir = (dir / "explicit-control").string();
  valid_cli.runtime_dir = (dir / "explicit-runtime").string();
  valid_cli.sbps_endpoint = (dir / "explicit-control" / "s.sock").string();
  const auto explicit_paths = ResolveServerBootstrapConfig(valid_cli);
  Require(explicit_paths.ok() && explicit_paths.config.control_dir == valid_cli.control_dir &&
              explicit_paths.config.data_dir == valid_cli.runtime_dir &&
              explicit_paths.config.sbps_endpoint == valid_cli.sbps_endpoint,
          "dedicated scope rewrote explicit CLI instance directories/endpoints");

  const auto scoped_path = dir / "explicit-paths.conf";
  {
    std::ofstream out(scoped_path);
    out << "[config]\nformat = SBCD1\n[server.database]\ndefault_path = \""
        << (dir / "file-paths.sbdb").generic_string() << "\"\n[server.runtime]\ncontrol_dir = \""
        << (dir / "file-control").generic_string() << "\"\ndata_dir = \""
        << (dir / "file-runtime").generic_string() << "\"\n";
  }
  ServerCliOptions file_paths_cli;
  file_paths_cli.config_path = scoped_path.string();
  const auto file_paths = ResolveServerBootstrapConfig(file_paths_cli);
  Require(file_paths.ok() && file_paths.config.control_dir == dir / "file-control" &&
              file_paths.config.data_dir == dir / "file-runtime" &&
              file_paths.config.sbps_endpoint.parent_path() == file_paths.config.control_dir,
          "dedicated scope rewrote file-configured instance directories");

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

  const auto shared_path = dir / "shared.conf";
  {
    std::ofstream out(shared_path);
    out << "[config]\nformat = SBCD1\n[server.database]\ndaemon_scope = shared\n";
  }
  invalid_cli.config_path = shared_path.string();
  const auto shared = ResolveServerBootstrapConfig(invalid_cli);
  Require(!shared.ok(), "explicit shared daemon config was accepted");
}

std::string ReadBytes(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  Require(input.is_open(), "database fixture missing");
  const std::string bytes{std::istreambuf_iterator<char>(input), {}};
  Require(!input.bad(), "database fixture read failed");
  return bytes;
}

void TestHostedEnginePublishesDurableDatabaseUuid(const std::filesystem::path& dir,
                                                const char* executable) {
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

  auto second_create = create;
  const auto second_path = dir / "second_identity.sbdb";
  second_create.path = second_path.string();
  second_create.database_uuid = uuid::GenerateEngineIdentityV7(
      scratchbird::core::platform::UuidKind::database, 1779420001003).value;
  second_create.filespace_uuid = uuid::GenerateEngineIdentityV7(
      scratchbird::core::platform::UuidKind::filespace, 1779420001004).value;
  Require(db::CreateDatabaseFile(second_create).ok(), "second real database fixture failed");

  auto config = Config();
  config.database_default_path = path;
  config.database_auto_create = false;
  const auto refused = scratchbird::server::StartHostedEngine(config);
  Require(!refused.ok() && !refused.diagnostics.empty() &&
              refused.diagnostics.front().code ==
                  "BOOTSTRAP.SECURITY_DATABASE_UNAVAILABLE",
          "DBLC-013E public server accepted an uncredentialed fixture database");

  config.allow_uncredentialed_fixture_database = true;
  auto shared_config = config;
  shared_config.database_daemon_scope = "shared";
  const auto shared = scratchbird::server::StartHostedEngine(shared_config);
  Require(!shared.ok() && shared.diagnostics.front().code == "SERVER.DAEMON.SCOPE_INVALID",
          "direct hosted open bypassed shared-scope rejection");
  auto hosted = scratchbird::server::StartHostedEngine(config);
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

  const auto status_before = scratchbird::server::HostedEngineStatusJson(hosted.state);
  const auto first_bytes = ReadBytes(path), second_bytes = ReadBytes(second_path);
  auto second_config = config;
  second_config.database_default_path = second_path;
  std::vector<scratchbird::server::HostedEngineResult> rejected(8);
  std::vector<std::thread> contenders;
  for (std::size_t i = 0; i != rejected.size(); ++i) {
    contenders.emplace_back([&, i] { rejected[i] = scratchbird::server::StartHostedEngine(second_config); });
  }
  for (auto& contender : contenders) contender.join();
  for (const auto& result : rejected) {
    Require(!result.ok() && result.diagnostics.front().code == "SERVER.STARTUP.DATABASE_OPEN_FAILED",
            "concurrent second-database attempt escaped the process owner guard");
  }
  Require(ReadBytes(path) == first_bytes && ReadBytes(second_path) == second_bytes &&
              !std::filesystem::exists(second_path.string() + ".sb.route.owner.lock"),
          "second-database attempts changed data or published route ownership");
  second_config.database_default_path = dir / "must-not-be-created" / "second.sbdb";
  const auto second = scratchbird::server::StartHostedEngine(second_config);
  Require(!second.ok() && second.diagnostics.front().code == "SERVER.STARTUP.DATABASE_OPEN_FAILED",
          "one process admitted a second hosted database attempt");
  Require(!std::filesystem::exists(second_config.database_default_path.parent_path()),
          "second hosted open touched another database directory before refusing");
  Require(scratchbird::server::HostedEngineStatusJson(hosted.state) == status_before &&
              hosted.state.database_ownership_locks.size() == 1 &&
              hosted.state.database_ownership_locks.front()->valid(),
          "second database refusal changed the first hosted runtime/ownership");

  // Exec a fresh process: it must refuse the parent's database but may host the
  // second real database. This is real host/lock evidence, not IPC/security E2E.
  const auto first_text = path.string(), second_text = second_path.string();
  const char* first_arg = first_text.c_str();
  const char* second_arg = second_text.c_str();
  const auto child = ::fork();
  Require(child >= 0, "fork failed for independent server-host probe");
  if (child == 0) {
    ::execl(executable, executable, "--child-probe", first_arg, second_arg,
            static_cast<char*>(nullptr));
    ::_exit(127);
  }
  int child_status = 0;
  pid_t waited;
  do { waited = ::waitpid(child, &child_status, 0); } while (waited < 0 && errno == EINTR);
  Require(waited == child && WIFEXITED(child_status) && WEXITSTATUS(child_status) == 0,
          "independent process did not enforce exclusive first/separate second ownership");
  Require(ReadBytes(path) == first_bytes && hosted.state.database_ownership_locks.front()->valid(),
          "separate process host changed the parent's database/owner");
  hosted.state = {};
  second_config.database_default_path = second_path;
  const auto after_child = ReadBytes(second_path);
  const auto retarget = scratchbird::server::StartHostedEngine(second_config);
  Require(!retarget.ok() && retarget.diagnostics.front().code == "SERVER.STARTUP.DATABASE_OPEN_FAILED" &&
              ReadBytes(second_path) == after_child,
          "dropping a hosted snapshot permitted process-lifetime database retargeting");
}

}  // namespace

int main(int argc, char** argv) {
  if (argc == 4 && std::string_view(argv[1]) == "--child-probe") {
    // exec discarded the parent's configured allocator. A real server installs
    // startup memory policy before hosting; this component installs its explicit
    // fixture policy rather than inheriting parent process runtime state.
    const auto memory = scratchbird::core::memory::ConfigureDefaultMemoryManagerForFixture(
        scratchbird::core::memory::DefaultLocalEngineMemoryPolicy(),
        "server_instance_ownership_child");
    Require(memory.ok() && memory.fixture_mode, "child memory policy installation failed");
    auto config = Config();
    config.allow_uncredentialed_fixture_database = true;
    config.database_default_path = argv[2];
    const auto conflict = scratchbird::server::StartHostedEngine(config);
    Require(!conflict.ok() && conflict.diagnostics.front().code == "ARCH.DATABASE_MULTI_OWNER",
            "second process opened an already hosted database");
    config.database_default_path = argv[3];
    std::vector<scratchbird::server::HostedEngineResult> outcomes(8);
    std::vector<std::thread> racers;
    for (std::size_t i = 0; i != outcomes.size(); ++i) {
      racers.emplace_back([&, i] { outcomes[i] = scratchbird::server::StartHostedEngine(config); });
    }
    for (auto& racer : racers) racer.join();
    unsigned admitted = 0;
    for (const auto& own : outcomes) {
      if (own.ok()) {
        Require(own.state.databases.size() == 1 && own.state.databases.front().database_open,
                "fresh process did not host a real database");
        ++admitted;
      } else {
        if (own.diagnostics.front().code != "SERVER.STARTUP.DATABASE_OPEN_FAILED") {
          for (const auto& diagnostic : own.diagnostics)
            std::cerr << diagnostic.code << ':' << diagnostic.safe_message << '\n';
        }
        Require(own.diagnostics.front().code == "SERVER.STARTUP.DATABASE_OPEN_FAILED",
                "concurrent host opening bypassed process reservation");
      }
    }
    Require(admitted == 1, "fresh process must admit exactly one concurrent hosted open");
    return EXIT_SUCCESS;
  }
  Require(argc == 1, "unexpected server ownership test arguments");
  const auto dir = MakeTempDir();
  TestSharedDaemonRefused();
  TestDedicatedDaemonExclusiveStopDecision();
  TestDedicatedDaemonRefusesAmbiguousScope();
  TestHostedDatabaseFailureRequiresQuarantine();
  TestDaemonScopeConfigurationValidation(dir);
  TestHostedEnginePublishesDurableDatabaseUuid(dir, argv[0]);
  std::filesystem::remove_all(dir);
  return EXIT_SUCCESS;
}

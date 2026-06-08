// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "config.hpp"
#include "lifecycle.hpp"
#include "manager_runtime.hpp"
#include "server_daemon_lifecycle.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <unistd.h>
#include <vector>

#ifndef _WIN32
#include <sys/stat.h>
#endif

namespace {

using scratchbird::manager::node::CleanupManagerRuntimeArtifacts;
using scratchbird::manager::node::ManagerConfig;
using scratchbird::manager::node::ManagerRuntimeCleanupOperation;
using scratchbird::manager::node::ManagerOwnerDatabaseRuntimeScopeId;
using scratchbird::manager::node::ResolveManagerRuntimePaths;
using scratchbird::manager::node::ValidateManagerRuntimeArtifacts;
using scratchbird::server::CleanupServerRuntimeArtifacts;
using scratchbird::server::EvaluateServerDaemonLifecycle;
using scratchbird::server::HostedDatabaseSnapshot;
using scratchbird::server::HostedDatabaseState;
using scratchbird::server::HostedEngineState;
using scratchbird::server::ResolveServerBootstrapConfig;
using scratchbird::server::ServerBootstrapConfig;
using scratchbird::server::ServerCliOptions;
using scratchbird::server::ServerDatabaseRuntimeScopeId;
using scratchbird::server::ServerLifecycleArtifacts;
using scratchbird::server::ServerRuntimeCleanupOperation;
using scratchbird::server::ValidateServerRuntimeArtifacts;
using scratchbird::server::WriteStartupLifecycleArtifacts;

constexpr std::string_view kGate = "DBLC_P13Q_PACKAGING_SERVICE_COMPLETE";
constexpr std::string_view kLabel = "database_lifecycle_packaging_service";
constexpr std::string_view kStaticLabel = "DBLC_STATIC_RUNTIME_CLEANUP";

void Require(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << message << '\n';
    std::exit(EXIT_FAILURE);
  }
}

bool Contains(std::string_view haystack, std::string_view needle) {
  return haystack.find(needle) != std::string_view::npos;
}

std::filesystem::path MakeTempDir() {
  std::string tmpl = "/tmp/sb_dblc013q_packaging.XXXXXX";
  std::vector<char> writable(tmpl.begin(), tmpl.end());
  writable.push_back('\0');
  char* made = ::mkdtemp(writable.data());
  Require(made != nullptr, "mkdtemp failed for DBLC-013Q packaging service test");
  return std::filesystem::absolute(std::filesystem::path(made)).lexically_normal();
}

std::string ReadFile(const std::filesystem::path& path) {
  std::ifstream in(path);
  Require(static_cast<bool>(in), "failed to read DBLC-013Q artifact");
  return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

void WriteFile(const std::filesystem::path& path, const std::string& text) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream out(path, std::ios::trunc);
  Require(static_cast<bool>(out), "failed to write DBLC-013Q artifact");
  out << text;
  out.close();
#ifndef _WIN32
  ::chmod(path.c_str(), 0600);
#endif
}

void RequirePrivateFile(const std::filesystem::path& path) {
#ifndef _WIN32
  struct stat st {};
  Require(::stat(path.c_str(), &st) == 0, "DBLC-013Q stat failed");
  Require((st.st_mode & 0777u) == 0600u, "DBLC-013Q runtime file is not 0600");
#else
  (void)path;
#endif
}

void RequirePrivateDir(const std::filesystem::path& path) {
#ifndef _WIN32
  struct stat st {};
  Require(::stat(path.c_str(), &st) == 0, "DBLC-013Q dir stat failed");
  Require((st.st_mode & 0777u) == 0700u, "DBLC-013Q runtime directory is not 0700");
#else
  (void)path;
#endif
}

ServerBootstrapConfig DedicatedConfig(const std::filesystem::path& root,
                                      std::string_view database_name) {
  ServerBootstrapConfig config;
  config.control_dir = root / "run";
  config.data_dir = root / "data";
  config.database_default_path = root / std::string(database_name);
  config.database_daemon_scope = "dedicated";
  config.database_runtime_scope_id = ServerDatabaseRuntimeScopeId(config.database_default_path);
  const auto scope = std::filesystem::path("databases") / config.database_runtime_scope_id;
  config.control_dir /= scope;
  config.data_dir /= scope;
  config.pid_file = config.control_dir / "sb_server.pid";
  config.lifecycle_state_file = config.control_dir / "sb_server.lifecycle.state";
  config.lifecycle_journal_file = config.control_dir / "sb_server.lifecycle.journal";
  config.sbps_endpoint = config.control_dir / "sb_server.sbps.sock";
  return config;
}

HostedEngineState EngineFor(const ServerBootstrapConfig& config,
                            bool cluster_required = false) {
  HostedDatabaseSnapshot database;
  database.state = HostedDatabaseState::kOpen;
  database.database_open = true;
  database.database_path = config.database_default_path.string();
  database.database_uuid = "019e130q-0000-7000-8000-000000000013";
  database.cluster_authority_required = cluster_required;
  HostedEngineState engine;
  engine.engine_context_active = true;
  engine.databases.push_back(database);
  return engine;
}

bool HasDiagnostic(const std::vector<scratchbird::server::ServerDiagnostic>& diagnostics,
                   std::string_view code) {
  for (const auto& diagnostic : diagnostics) {
    if (diagnostic.code == code) return true;
  }
  return false;
}

void TestDedicatedConfigDerivesDatabaseScopedRuntime(const std::filesystem::path& root) {
  const auto config_path = root / "dedicated.conf";
  const auto control = root / "configured-run";
  const auto data = root / "configured-data";
  const auto database = root / "configured.sbdb";
  {
    std::ofstream out(config_path);
    out << "[config]\nformat = SBCD1\n"
        << "[server.runtime]\n"
        << "control_dir = \"" << control.generic_string() << "\"\n"
        << "data_dir = \"" << data.generic_string() << "\"\n"
        << "[server.database]\n"
        << "default_path = \"" << database.generic_string() << "\"\n"
        << "daemon_scope = dedicated\n";
  }
  ServerCliOptions cli;
  cli.config_path = config_path.string();
  const auto loaded = ResolveServerBootstrapConfig(cli);
  Require(loaded.ok(), "DBLC-013Q dedicated config failed to load");
  const auto scope = ServerDatabaseRuntimeScopeId(loaded.config.database_default_path);
  Require(loaded.config.database_runtime_scope_id == scope,
          "DBLC-013Q database runtime scope id not derived");
  Require(Contains(loaded.config.control_dir.string(), (std::filesystem::path("databases") / scope).string()),
          "DBLC-013Q control directory is not database scoped");
  Require(Contains(loaded.config.pid_file.string(), scope),
          "DBLC-013Q PID file is not database scoped");
}

void TestPackagedServiceDefaultsDeriveRuntimeAndLogPaths(const std::filesystem::path& root) {
  ServerCliOptions cli;
  cli.service = true;
  cli.database_ref = (root / "packaged_default.sbdb").string();
  const auto loaded = ResolveServerBootstrapConfig(cli);
  Require(loaded.ok(), "DBLC-013Q packaged service defaults failed to resolve");
  Require(!loaded.config.allow_current_directory,
          "DBLC-013Q service mode did not disable current-directory authority");
  Require(loaded.config.control_dir == std::filesystem::path("/run/scratchbird"),
          "DBLC-013Q service control directory default mismatch");
  Require(loaded.config.data_dir == std::filesystem::path("/var/lib/scratchbird"),
          "DBLC-013Q service data directory default mismatch");
  Require(loaded.config.pid_file == loaded.config.control_dir / "sb_server.pid",
          "DBLC-013Q service PID file default mismatch");
  Require(loaded.config.lifecycle_state_file ==
              loaded.config.control_dir / "sb_server.lifecycle.state",
          "DBLC-013Q service lifecycle state default mismatch");
  Require(loaded.config.lifecycle_journal_file ==
              loaded.config.control_dir / "sb_server.lifecycle.journal",
          "DBLC-013Q service lifecycle journal default mismatch");
  Require(loaded.config.sbps_endpoint == loaded.config.control_dir / "sb_server.sbps.sock",
          "DBLC-013Q service SBPS socket default mismatch");
  Require(loaded.config.listener_native_control_dir ==
              loaded.config.control_dir / "listeners",
          "DBLC-013Q listener control default mismatch");
  Require(loaded.config.listener_native_runtime_dir ==
              loaded.config.data_dir / "listeners",
          "DBLC-013Q listener runtime default mismatch");
  Require(loaded.config.log_file == "/var/log/scratchbird/sb_server.log",
          "DBLC-013Q service log file default mismatch");
  Require(!loaded.config.database_runtime_scope_id.empty(),
          "DBLC-013Q service database runtime scope id was not derived");
}

void TestStartupArtifactsAreScopedPrivateAndServiceReady(const std::filesystem::path& root) {
  auto config = DedicatedConfig(root, "owned.sbdb");
  const auto lifecycle = WriteStartupLifecycleArtifacts(config, "config_lifecycle_ready");
  Require(lifecycle.ok(), "DBLC-013Q startup lifecycle artifacts failed");
  RequirePrivateDir(config.control_dir);
  RequirePrivateDir(config.data_dir);
  RequirePrivateFile(config.pid_file);
  RequirePrivateFile(config.control_dir / "sb_server.owner");
  RequirePrivateFile(config.lifecycle_state_file);

  const auto owner = ReadFile(config.control_dir / "sb_server.owner");
  Require(Contains(owner, "daemon_scope=dedicated"), "DBLC-013Q owner missing daemon scope");
  Require(Contains(owner, "database_runtime_scope_id=" + config.database_runtime_scope_id),
          "DBLC-013Q owner missing database scope");
  Require(Contains(owner, "database_path=" + config.database_default_path.string()),
          "DBLC-013Q owner missing database path");

  const auto validation = ValidateServerRuntimeArtifacts(config, lifecycle.artifacts, true);
  if (!validation.ok()) {
    for (const auto& diagnostic : validation.diagnostics) {
      std::cerr << diagnostic.code << ':' << diagnostic.safe_message << '\n';
    }
  }
  Require(validation.ok(), "DBLC-013Q runtime artifact validation failed");
  Require(validation.directories_valid && validation.pid_file_valid &&
              validation.owner_token_valid && validation.lifecycle_state_valid &&
              validation.endpoint_descriptor_valid && validation.database_association_valid,
          "DBLC-013Q runtime validation did not prove service-ready inputs");

  const auto daemon = EvaluateServerDaemonLifecycle(config, lifecycle.artifacts, EngineFor(config));
  Require(daemon.service_ready, "DBLC-013Q daemon did not become service-ready");
  Require(daemon.runtime_directories_valid && daemon.pid_owner_state_valid &&
              daemon.endpoint_descriptors_valid && daemon.database_association_valid,
          "DBLC-013Q daemon service-ready publication missed required evidence");
}

void TestCrossDatabaseOwnerTokenFailsClosed(const std::filesystem::path& root) {
  auto config = DedicatedConfig(root, "cross_target.sbdb");
  std::filesystem::create_directories(config.control_dir);
  std::filesystem::create_directories(config.data_dir);
#ifndef _WIN32
  ::chmod(config.control_dir.c_str(), 0700);
  ::chmod(config.data_dir.c_str(), 0700);
#endif
  WriteFile(config.pid_file, std::to_string(::getpid()) + "\n");
  WriteFile(config.lifecycle_state_file,
            "format=SB_SERVER_LIFECYCLE_STATE_V1\nstate=stopped\n");
  WriteFile(config.control_dir / "sb_server.owner",
            "format=SB_SERVER_OWNER_TOKEN_V1\npid=" + std::to_string(::getpid()) +
                "\ndaemon_scope=dedicated\n"
                "database_runtime_scope_id=db-wrong\n"
                "database_path=/tmp/other.sbdb\n");
  const auto lifecycle = WriteStartupLifecycleArtifacts(config, "config_lifecycle_ready");
  Require(!lifecycle.ok(), "DBLC-013Q cross-database owner token was accepted");
  Require(HasDiagnostic(lifecycle.diagnostics, "SERVER.RUNTIME.OWNER_TOKEN_CROSS_DATABASE"),
          "DBLC-013Q cross-database owner diagnostic mismatch");
}

void TestClusterPrivateStandalonePathFailsClosed(const std::filesystem::path& root) {
  auto config = DedicatedConfig(root, "cluster_private.sbdb");
  const auto lifecycle = WriteStartupLifecycleArtifacts(config, "config_lifecycle_ready");
  Require(lifecycle.ok(), "DBLC-013Q cluster test startup artifacts failed");
  const auto daemon = EvaluateServerDaemonLifecycle(config, lifecycle.artifacts,
                                                    EngineFor(config, true));
  Require(!daemon.service_ready, "DBLC-013Q standalone cluster-private path became ready");
  Require(daemon.standalone_cluster_path_refused,
          "DBLC-013Q cluster-private refusal flag missing");
  Require(HasDiagnostic(daemon.diagnostics, "SERVER.DAEMON.CLUSTER_AUTHORITY_REQUIRED"),
          "DBLC-013Q cluster-private diagnostic mismatch");
}

void TestCleanupPreservesUnrelatedRuntimeArtifacts(const std::filesystem::path& root) {
  auto config = DedicatedConfig(root, "cleanup.sbdb");
  const auto lifecycle = WriteStartupLifecycleArtifacts(config, "config_lifecycle_ready");
  Require(lifecycle.ok(), "DBLC-013Q cleanup startup artifacts failed");
  const auto unrelated_scope = root / "run" / "databases" / "db-unrelated";
  std::filesystem::create_directories(unrelated_scope);
  WriteFile(unrelated_scope / "sb_server.owner", "format=other\npid=1\n");

  const auto cleanup = CleanupServerRuntimeArtifacts(config,
                                                     lifecycle.artifacts,
                                                     ServerRuntimeCleanupOperation::kUninstall);
  Require(cleanup.ok(), "DBLC-013Q scoped cleanup failed");
  Require(std::filesystem::exists(unrelated_scope / "sb_server.owner"),
          "DBLC-013Q cleanup removed unrelated database runtime artifact");
}

void TestManagerRuntimeOwnerScopeAndCleanup(const std::filesystem::path& root) {
  ManagerConfig config;
  config.runtime_dir = root / "manager-runtime";
  config.control_dir = config.runtime_dir / "manager" / "databases";
  config.owner_database_name = "main";
  config.owner_database_runtime_scope_id = ManagerOwnerDatabaseRuntimeScopeId(config);
  config.control_dir /= config.owner_database_runtime_scope_id;
  std::filesystem::create_directories(config.runtime_dir);
  std::filesystem::create_directories(config.control_dir);
#ifndef _WIN32
  ::chmod(config.runtime_dir.c_str(), 0700);
  ::chmod(config.control_dir.c_str(), 0700);
#endif
  const auto paths = ResolveManagerRuntimePaths(config);
  WriteFile(paths.pid_file, std::to_string(::getpid()) + "\n");
  WriteFile(paths.state_file, "format=SBMN_MANAGER_LIFECYCLE_STATE_V1\nstate=stopped\n");
  WriteFile(paths.owner_file,
            "format=SBMN_MANAGER_OWNER_V1\npid=" + std::to_string(::getpid()) +
                "\nowner_database_name=main\nowner_database_runtime_scope_id=" +
                config.owner_database_runtime_scope_id + "\n");

  const auto validation = ValidateManagerRuntimeArtifacts(config, true);
  Require(validation.ok(), "DBLC-013Q manager runtime validation failed");
  Require(validation.directories_valid && validation.pid_owner_valid &&
              validation.database_association_valid,
          "DBLC-013Q manager runtime validation missed owner database scope");

  const auto unrelated = config.runtime_dir / "manager" / "databases" / "db-other";
  std::filesystem::create_directories(unrelated);
  WriteFile(unrelated / "sbmn_manager.owner", "format=other\npid=1\n");
  const auto cleanup = CleanupManagerRuntimeArtifacts(config,
                                                      ManagerRuntimeCleanupOperation::kUninstall);
  Require(cleanup.exit_code == 0, "DBLC-013Q manager scoped cleanup failed");
  Require(std::filesystem::exists(unrelated / "sbmn_manager.owner"),
          "DBLC-013Q manager cleanup removed unrelated database runtime artifact");
}

}  // namespace

int main() {
  Require(!kGate.empty() && !kLabel.empty() && !kStaticLabel.empty(),
          "DBLC-013Q labels missing");
  const auto root = MakeTempDir();
  TestDedicatedConfigDerivesDatabaseScopedRuntime(root);
  TestPackagedServiceDefaultsDeriveRuntimeAndLogPaths(root);
  TestStartupArtifactsAreScopedPrivateAndServiceReady(root);
  TestCrossDatabaseOwnerTokenFailsClosed(root);
  TestClusterPrivateStandalonePathFailsClosed(root);
  TestCleanupPreservesUnrelatedRuntimeArtifacts(root);
  TestManagerRuntimeOwnerScopeAndCleanup(root);
  return EXIT_SUCCESS;
}

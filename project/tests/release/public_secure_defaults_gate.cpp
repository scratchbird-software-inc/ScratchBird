// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// PUBLIC_SECURE_DEFAULTS_GATE

#include "config.hpp"
#include "lifecycle.hpp"
#include "memory.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

#ifndef _WIN32
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace {

namespace memory = scratchbird::core::memory;
namespace server = scratchbird::server;

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view message) {
  if (!condition) {
    Fail(message);
  }
}

bool HasDiagnostic(const std::vector<server::ServerDiagnostic>& diagnostics,
                   std::string_view code) {
  for (const auto& diagnostic : diagnostics) {
    if (diagnostic.code == code) {
      return true;
    }
  }
  return false;
}

std::filesystem::path MakeTempRoot() {
#ifndef _WIN32
  std::string tmpl = "/tmp/sb_pcr123_secure_defaults.XXXXXX";
  std::vector<char> writable(tmpl.begin(), tmpl.end());
  writable.push_back('\0');
  char* made = ::mkdtemp(writable.data());
  Require(made != nullptr, "PCR-123 mkdtemp failed");
  return std::filesystem::path(made);
#else
  const auto root = std::filesystem::temp_directory_path() /
                    "sb_pcr123_secure_defaults";
  std::filesystem::remove_all(root);
  std::filesystem::create_directories(root);
  return root;
#endif
}

void WriteFile(const std::filesystem::path& path, const std::string& text) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream out(path, std::ios::trunc);
  Require(static_cast<bool>(out), "PCR-123 file write failed");
  out << text;
  out.close();
  Require(static_cast<bool>(out), "PCR-123 file flush failed");
}

std::string SecureServiceConfig(const std::filesystem::path& root) {
  return "[config]\n"
         "format = SBCD1\n"
         "[server]\n"
         "mode = service\n"
         "[server.runtime]\n"
         "control_dir = \"" + (root / "run").generic_string() + "\"\n"
         "data_dir = \"" + (root / "data").generic_string() + "\"\n"
         "[server.security]\n"
         "provider_family = local_password\n"
         "provider_state = healthy\n"
         "default_policy_installed = true\n"
         "[server.memory]\n"
         "enable_platform_memory_probe = false\n";
}

server::ServerConfigLoadResult LoadConfig(const std::filesystem::path& path) {
  server::ServerCliOptions cli;
  cli.config_path = path.string();
  return server::ResolveServerBootstrapConfig(cli);
}

#ifndef _WIN32
void RequireMode(const std::filesystem::path& path,
                 mode_t expected,
                 std::string_view message) {
  struct stat st {};
  Require(::stat(path.c_str(), &st) == 0, "PCR-123 stat failed");
  Require((st.st_mode & 0777u) == expected, message);
}
#else
void RequireMode(const std::filesystem::path&, int, std::string_view) {}
#endif

void TestCompiledDefaults() {
  server::ServerBootstrapConfig defaults;
  Require(defaults.security_authority_mode == "database_local",
          "PCR-123 security authority default drifted");
  Require(defaults.security_provider_family == "local_password",
          "PCR-123 security provider default is not local_password");
  Require(defaults.security_default_policy_installed,
          "PCR-123 default security policy is not installed");
  Require(!defaults.database_auto_create,
          "PCR-123 database auto-create default is not fail-closed");
  Require(defaults.database_open_mode == "normal",
          "PCR-123 database open mode default drifted");
  Require(defaults.database_daemon_scope == "shared",
          "PCR-123 daemon scope default drifted");
  Require(!defaults.embedded_direct_mode,
          "PCR-123 embedded direct mode default must be disabled");
  Require(!defaults.listener_native_enabled,
          "PCR-123 native listener default must be disabled");
  Require(defaults.listener_native_bind_host == "127.0.0.1",
          "PCR-123 native listener bind default is not loopback");
  Require(defaults.listener_native_tls_required,
          "PCR-123 native listener TLS default must be required");
  Require(defaults.log_level == "info",
          "PCR-123 log level default must not be debug");
  Require(defaults.log_file == "stderr",
          "PCR-123 foreground log default drifted");
  Require(defaults.memory_policy_name == "server_production_default",
          "PCR-123 memory policy default drifted");
  Require(defaults.memory_failure_mode == memory::AllocationFailureMode::return_error,
          "PCR-123 memory failure mode default is not fail-closed");
  Require(defaults.memory_zero_memory_on_release,
          "PCR-123 memory release zeroing default is disabled");

  defaults.memory_enable_platform_memory_probe = false;
  const auto resolved = server::ResolveServerMemoryAllocationPolicy(defaults);
  Require(resolved.ok(), "PCR-123 compiled memory defaults do not resolve");
  Require(!resolved.policy.refuse_all_allocations,
          "PCR-123 resolved production memory policy refuses all allocations");
  Require(resolved.policy.policy_name == "server_production_default",
          "PCR-123 resolved memory policy name drifted");
}

void TestServiceProfileResolution(const std::filesystem::path& root) {
  const auto config_path = root / "service.conf";
  WriteFile(config_path, SecureServiceConfig(root));
  const auto loaded = LoadConfig(config_path);
  Require(loaded.ok(), "PCR-123 secure service config did not load");
  Require(loaded.config.mode == server::ServerMode::kService,
          "PCR-123 service mode did not apply");
  Require(!loaded.config.allow_current_directory,
          "PCR-123 service profile left current-directory authority enabled");
  Require(loaded.config.log_file == "/var/log/scratchbird/sb_server.log",
          "PCR-123 service log default did not move away from stderr");
  Require(loaded.config.security_provider_family == "local_password",
          "PCR-123 service security provider did not canonicalize");
  Require(loaded.config.memory_policy_name == "server_production_default",
          "PCR-123 service memory policy default drifted");
  Require(!loaded.config.memory_enable_platform_memory_probe,
          "PCR-123 explicit deterministic memory probe setting was not applied");
}

void TestPermissiveProviderConfigFailsClosed(const std::filesystem::path& root) {
  for (std::string provider : {"fixture_auth", "test_provider", "unsupported_provider"}) {
    const auto config_path = root / ("provider_" + provider + ".conf");
    WriteFile(config_path,
              "[config]\n"
              "format = SBCD1\n"
              "[server.security]\n"
              "provider_family = " + provider + "\n"
              "[server.memory]\n"
              "enable_platform_memory_probe = false\n");
    const auto loaded = LoadConfig(config_path);
    Require(!loaded.ok(), "PCR-123 permissive/test provider config was accepted");
    Require(HasDiagnostic(loaded.diagnostics, "CONFIG.VALUE_INVALID_ENUM"),
            "PCR-123 permissive/test provider diagnostic mismatch");
  }
}

void TestRuntimeArtifactsArePrivate(const std::filesystem::path& root) {
  const auto config_path = root / "runtime.conf";
  WriteFile(config_path, SecureServiceConfig(root));
  const auto loaded = LoadConfig(config_path);
  Require(loaded.ok(), "PCR-123 runtime config did not load");

  const auto lifecycle =
      server::WriteStartupLifecycleArtifacts(loaded.config, "config_lifecycle_ready");
  Require(lifecycle.ok(), "PCR-123 startup lifecycle artifacts failed");
  RequireMode(loaded.config.control_dir, 0700, "PCR-123 control dir is not 0700");
  RequireMode(loaded.config.data_dir, 0700, "PCR-123 data dir is not 0700");
  RequireMode(loaded.config.pid_file, 0600, "PCR-123 pid file is not 0600");
  RequireMode(loaded.config.control_dir / "sb_server.owner",
              0600,
              "PCR-123 owner token is not 0600");
  RequireMode(loaded.config.lifecycle_state_file,
              0600,
              "PCR-123 lifecycle state file is not 0600");
  RequireMode(loaded.config.lifecycle_journal_file,
              0600,
              "PCR-123 lifecycle journal file is not 0600");

  const auto valid =
      server::ValidateServerRuntimeArtifacts(loaded.config, lifecycle.artifacts, true);
  Require(valid.ok(), "PCR-123 private runtime artifacts did not validate");

#ifndef _WIN32
  ::chmod(loaded.config.control_dir.c_str(), 0777);
  const auto unsafe_dir =
      server::ValidateServerRuntimeArtifacts(loaded.config, lifecycle.artifacts, true);
  Require(!unsafe_dir.ok(), "PCR-123 world-writable control dir was accepted");
  Require(HasDiagnostic(unsafe_dir.diagnostics, "SERVER.RUNTIME.DIRECTORY_VALIDATION_FAILED"),
          "PCR-123 world-writable dir diagnostic mismatch");

  ::chmod(loaded.config.control_dir.c_str(), 0700);
  ::chmod(loaded.config.pid_file.c_str(), 0666);
  const auto unsafe_pid =
      server::ValidateServerRuntimeArtifacts(loaded.config, lifecycle.artifacts, true);
  Require(!unsafe_pid.ok(), "PCR-123 world-writable pid file was accepted");
  Require(HasDiagnostic(unsafe_pid.diagnostics, "SERVER.RUNTIME.PID_FILE_INVALID"),
          "PCR-123 world-writable pid diagnostic mismatch");
#endif
}

}  // namespace

int main() {
  const auto root = MakeTempRoot();
  TestCompiledDefaults();
  TestServiceProfileResolution(root);
  TestPermissiveProviderConfigFailsClosed(root);
  TestRuntimeArtifactsArePrivate(root);
  return EXIT_SUCCESS;
}

// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// SEARCH_KEY: SB_SERVER_CONFIG_LIFECYCLE_ARTIFACTS

#include "lifecycle.hpp"

#include "config.hpp"

#include <chrono>
#include <csignal>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <sstream>
#include <utility>

#ifndef _WIN32
#include <cerrno>
#include <sys/stat.h>
#include <unistd.h>
#else
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace scratchbird::server {

namespace {

std::uint64_t NowMicros() {
  const auto now = std::chrono::system_clock::now().time_since_epoch();
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::microseconds>(now).count());
}

std::uint64_t NowMonotonicMicros() {
  const auto now = std::chrono::steady_clock::now().time_since_epoch();
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::microseconds>(now).count());
}

int CurrentPid() {
#ifndef _WIN32
  return static_cast<int>(::getpid());
#else
  return static_cast<int>(::GetCurrentProcessId());
#endif
}

bool ProcessAppearsAlive(int pid) {
  if (pid <= 0) {
    return false;
  }
#ifndef _WIN32
  if (::kill(pid, 0) == 0) {
    return true;
  }
  return errno == EPERM;
#else
  HANDLE process = ::OpenProcess(SYNCHRONIZE, FALSE, static_cast<DWORD>(pid));
  if (process == nullptr) {
    return ::GetLastError() == ERROR_ACCESS_DENIED;
  }
  const DWORD wait = ::WaitForSingleObject(process, 0);
  ::CloseHandle(process);
  return wait == WAIT_TIMEOUT;
#endif
}

bool IsDedicatedScope(const ServerBootstrapConfig& config) {
  return config.database_daemon_scope == "dedicated";
}

std::filesystem::path OwnerFilePath(const ServerBootstrapConfig& config) {
  return config.control_dir / "sb_server.owner";
}

std::string ExpectedScopeId(const ServerBootstrapConfig& config) {
  if (!config.database_runtime_scope_id.empty()) {
    return config.database_runtime_scope_id;
  }
  return ServerDatabaseRuntimeScopeId(config.database_default_path);
}

std::map<std::string, std::string> ReadKeyValueFile(const std::filesystem::path& path) {
  std::ifstream input(path);
  std::map<std::string, std::string> values;
  std::string line;
  while (std::getline(input, line)) {
    const auto eq = line.find('=');
    if (eq == std::string::npos) {
      continue;
    }
    values[line.substr(0, eq)] = line.substr(eq + 1);
  }
  return values;
}

bool SafeFinalState(const std::string& state) {
  return state == "stopped" || state == "startup_failed" ||
         state == "failed_terminal" || state == "quarantined";
}

std::optional<int> ReadPidFromKeyValues(const std::map<std::string, std::string>& values) {
  const auto it = values.find("pid");
  if (it == values.end()) return std::nullopt;
  try {
    return std::stoi(it->second);
  } catch (...) {
    return std::nullopt;
  }
}

std::optional<std::string> ReadStateFileState(const std::filesystem::path& path) {
  const auto values = ReadKeyValueFile(path);
  const auto it = values.find("state");
  if (it == values.end()) return std::nullopt;
  return it->second;
}

bool IsOwnedByCurrentUser(const std::filesystem::path& path) {
#ifndef _WIN32
  struct stat st {};
  if (::lstat(path.c_str(), &st) != 0) return false;
  return st.st_uid == ::geteuid() && !S_ISLNK(st.st_mode);
#else
  (void)path;
  return true;
#endif
}

bool HasPrivatePermissions(const std::filesystem::path& path, bool directory) {
#ifndef _WIN32
  struct stat st {};
  if (::stat(path.c_str(), &st) != 0) return false;
  if (directory && !S_ISDIR(st.st_mode)) return false;
  if (!directory && !S_ISREG(st.st_mode)) return false;
  return (st.st_mode & 0077u) == 0;
#else
  (void)path;
  (void)directory;
  return true;
#endif
}

void ApplyPrivatePermissions(const std::filesystem::path& path, bool directory) {
#ifndef _WIN32
  ::chmod(path.c_str(), directory ? 0700 : 0600);
#else
  (void)path;
  (void)directory;
#endif
}

std::optional<int> ReadPidFile(const std::filesystem::path& path) {
  std::ifstream input(path);
  if (!input) {
    return std::nullopt;
  }
  int pid = 0;
  input >> pid;
  if (!input) {
    return std::nullopt;
  }
  return pid;
}

ServerDiagnostic RuntimeDiagnostic(std::string code,
                                   std::string key,
                                   std::string safe,
                                   std::vector<ServerDiagnosticField> fields = {}) {
  return {std::move(code),
          std::move(key),
          ServerDiagnosticSeverity::kError,
          std::move(safe),
          std::move(fields)};
}

bool WriteTextFile(const std::filesystem::path& path,
                   const std::string& contents,
                   std::vector<ServerDiagnostic>* diagnostics,
                   const std::string& diagnostic_code) {
  std::ofstream output(path, std::ios::trunc);
  if (!output) {
    diagnostics->push_back(RuntimeDiagnostic(diagnostic_code,
                                            "server.runtime.file_write_failed",
                                            "A server runtime artifact could not be written.",
                                            {{"path_redacted", path.string()}}));
    return false;
  }
  output << contents;
  output.flush();
  if (!output) {
    diagnostics->push_back(RuntimeDiagnostic(diagnostic_code,
                                            "server.runtime.file_write_failed",
                                            "A server runtime artifact could not be flushed.",
                                            {{"path_redacted", path.string()}}));
    return false;
  }
  ApplyPrivatePermissions(path, false);
  return true;
}

bool AppendTextFile(const std::filesystem::path& path,
                    const std::string& contents,
                    std::vector<ServerDiagnostic>* diagnostics) {
  std::ofstream output(path, std::ios::app);
  if (!output) {
    diagnostics->push_back(RuntimeDiagnostic("SERVER.RUNTIME.LIFECYCLE_JOURNAL_INVALID",
                                            "server.runtime.lifecycle_journal_invalid",
                                            "The server lifecycle journal could not be opened.",
                                            {{"path_redacted", path.string()}}));
    return false;
  }
  output << contents;
  output.flush();
  if (!output) {
    diagnostics->push_back(RuntimeDiagnostic("SERVER.RUNTIME.LIFECYCLE_JOURNAL_INVALID",
                                            "server.runtime.lifecycle_journal_invalid",
                                            "The server lifecycle journal could not be flushed.",
                                            {{"path_redacted", path.string()}}));
    return false;
  }
  ApplyPrivatePermissions(path, false);
  return true;
}

bool PreparePrivateDirectory(const std::filesystem::path& path,
                             std::vector<ServerDiagnostic>* diagnostics,
                             const std::string& diagnostic_code,
                             const std::string& message_key) {
  std::error_code ec;
  std::filesystem::create_directories(path, ec);
  if (ec) {
    diagnostics->push_back(RuntimeDiagnostic(diagnostic_code,
                                            message_key,
                                            "A server runtime directory could not be created.",
                                            {{"path_redacted", path.string()},
                                             {"os_error", ec.message()}}));
    return false;
  }
  ApplyPrivatePermissions(path, true);
  if (!IsOwnedByCurrentUser(path) || !HasPrivatePermissions(path, true)) {
    diagnostics->push_back(RuntimeDiagnostic(diagnostic_code,
                                            message_key,
                                            "A server runtime directory has unsafe ownership or permissions.",
                                            {{"path_redacted", path.string()}}));
    return false;
  }
  return true;
}

bool ExistingRuntimeArtifactsAreSafeForReplacement(
    const ServerBootstrapConfig& config,
    std::vector<ServerDiagnostic>* diagnostics) {
  const auto owner_file = OwnerFilePath(config);
  const bool owner_exists = std::filesystem::exists(owner_file);
  const bool pid_exists = std::filesystem::exists(config.pid_file);
  if (!owner_exists && !pid_exists) {
    return true;
  }

  if (owner_exists && (!IsOwnedByCurrentUser(owner_file) || !HasPrivatePermissions(owner_file, false))) {
    diagnostics->push_back(RuntimeDiagnostic("SERVER.RUNTIME.OWNER_TOKEN_INVALID",
                                            "server.runtime.owner_token_invalid",
                                            "The existing server owner token has unsafe ownership or permissions.",
                                            {{"owner_token_file", owner_file.string()}}));
    return false;
  }
  if (pid_exists && (!IsOwnedByCurrentUser(config.pid_file) || !HasPrivatePermissions(config.pid_file, false))) {
    diagnostics->push_back(RuntimeDiagnostic("SERVER.RUNTIME.PID_FILE_INVALID",
                                            "server.runtime.pid_file_invalid",
                                            "The existing server PID file has unsafe ownership or permissions.",
                                            {{"pid_file", config.pid_file.string()}}));
    return false;
  }

  const auto owner_values = owner_exists ? ReadKeyValueFile(owner_file) : std::map<std::string, std::string>{};
  const auto owner_pid = ReadPidFromKeyValues(owner_values);
  const auto pid_file_pid = ReadPidFile(config.pid_file);
  const auto live_pid = owner_pid ? owner_pid : pid_file_pid;
  if (live_pid && ProcessAppearsAlive(*live_pid) && *live_pid != CurrentPid()) {
    diagnostics->push_back(RuntimeDiagnostic("SERVER.RUNTIME.OWNER_TOKEN_BUSY",
                                            "server.runtime.owner_token_busy",
                                            "A live server owner already controls the runtime directory.",
                                            {{"pid", std::to_string(*live_pid)}}));
    return false;
  }

  if (owner_exists) {
    const auto expected_scope = ExpectedScopeId(config);
    const auto scope_it = owner_values.find("database_runtime_scope_id");
    const auto path_it = owner_values.find("database_path");
    const auto daemon_it = owner_values.find("daemon_scope");
    if (IsDedicatedScope(config) &&
        (scope_it == owner_values.end() || scope_it->second != expected_scope ||
         path_it == owner_values.end() ||
         std::filesystem::path(path_it->second).lexically_normal() !=
             config.database_default_path.lexically_normal())) {
      diagnostics->push_back(RuntimeDiagnostic("SERVER.RUNTIME.OWNER_TOKEN_CROSS_DATABASE",
                                              "server.runtime.owner_token_cross_database",
                                              "The existing owner token belongs to a different database scope.",
                                              {{"owner_token_file", owner_file.string()}}));
      return false;
    }
    if (daemon_it != owner_values.end() && daemon_it->second != config.database_daemon_scope) {
      diagnostics->push_back(RuntimeDiagnostic("SERVER.RUNTIME.OWNER_TOKEN_AMBIGUOUS",
                                              "server.runtime.owner_token_ambiguous",
                                              "The existing owner token daemon scope does not match.",
                                              {{"owner_token_file", owner_file.string()}}));
      return false;
    }
  }

  const auto last_state = ReadStateFileState(config.lifecycle_state_file);
  if (!last_state || !SafeFinalState(*last_state)) {
    diagnostics->push_back(RuntimeDiagnostic("SERVER.RUNTIME.OWNER_TOKEN_AMBIGUOUS",
                                            "server.runtime.owner_token_ambiguous",
                                            "Stale server runtime artifacts cannot be proven safe.",
                                            {{"lifecycle_state_file", config.lifecycle_state_file.string()}}));
    return false;
  }
  return true;
}

std::string StateFileContents(const ServerBootstrapConfig& config,
                              const std::string& state,
                              std::uint64_t generation) {
  std::ostringstream out;
  out << "format=SB_SERVER_LIFECYCLE_STATE_V1\n";
  out << "pid=" << CurrentPid() << "\n";
  out << "state=" << state << "\n";
  out << "state_generation=" << generation << "\n";
  out << "started_at_wall_micros=" << NowMicros() << "\n";
  out << "started_at_monotonic_micros=" << NowMonotonicMicros() << "\n";
  out << "mode=" << ServerModeName(config.mode) << "\n";
  out << "config_source=" << config.selected_config_source << "\n";
  if (config.selected_config_path) {
    out << "config_path=" << config.selected_config_path->string() << "\n";
  }
  out << "control_dir=" << config.control_dir.string() << "\n";
  out << "data_dir=" << config.data_dir.string() << "\n";
  out << "database_open_mode=" << config.database_open_mode << "\n";
  out << "daemon_scope=" << config.database_daemon_scope << "\n";
  out << "database_runtime_scope_id=" << ExpectedScopeId(config) << "\n";
  out << "database_path=" << config.database_default_path.string() << "\n";
  out << "sbps_endpoint=" << config.sbps_endpoint.string() << "\n";
  return out.str();
}

std::string OwnerFileContents(const ServerBootstrapConfig& config, std::uint64_t generation) {
  std::ostringstream out;
  out << "format=SB_SERVER_OWNER_TOKEN_V1\n";
  out << "pid=" << CurrentPid() << "\n";
  out << "state_generation=" << generation << "\n";
  out << "owner_started_wall_micros=" << NowMicros() << "\n";
  out << "owner_started_monotonic_micros=" << NowMonotonicMicros() << "\n";
  out << "daemon_scope=" << config.database_daemon_scope << "\n";
  out << "database_runtime_scope_id=" << ExpectedScopeId(config) << "\n";
  out << "database_path=" << config.database_default_path.string() << "\n";
  out << "control_dir=" << config.control_dir.string() << "\n";
  out << "data_dir=" << config.data_dir.string() << "\n";
  out << "sbps_endpoint=" << config.sbps_endpoint.string() << "\n";
  return out.str();
}

std::string JournalRecord(const std::string& event,
                          const std::string& state,
                          std::uint64_t generation) {
  std::ostringstream out;
  out << "{\"format\":\"SB_SERVER_LIFECYCLE_JOURNAL_V1\","
      << "\"event\":\"" << event << "\","
      << "\"pid\":" << CurrentPid() << ","
      << "\"state\":\"" << state << "\","
      << "\"state_generation\":" << generation << ","
      << "\"wall_micros\":" << NowMicros() << ","
      << "\"monotonic_micros\":" << NowMonotonicMicros() << "}\n";
  return out.str();
}

}  // namespace

ServerLifecycleResult WriteStartupLifecycleArtifacts(const ServerBootstrapConfig& config,
                                                     const std::string& target_state) {
  ServerLifecycleResult result;
  result.artifacts.generation = NowMicros();
  result.artifacts.state = target_state;
  result.artifacts.pid_file = config.pid_file.string();
  result.artifacts.owner_token_file = OwnerFilePath(config).string();
  result.artifacts.lifecycle_state_file = config.lifecycle_state_file.string();
  result.artifacts.lifecycle_journal_file = config.lifecycle_journal_file.string();
  result.artifacts.database_runtime_scope_id = ExpectedScopeId(config);
  result.artifacts.database_path = config.database_default_path.string();
  result.artifacts.daemon_scope = config.database_daemon_scope;
  result.artifacts.sbps_endpoint = config.sbps_endpoint.string();

  if (!PreparePrivateDirectory(config.data_dir,
                               &result.diagnostics,
                               "SERVER.RUNTIME.RUNTIME_DIR_INVALID",
                               "server.runtime.runtime_dir_invalid")) {
    return result;
  }
  if (!PreparePrivateDirectory(config.control_dir,
                               &result.diagnostics,
                               "SERVER.RUNTIME.CONTROL_DIR_INVALID",
                               "server.runtime.control_dir_invalid")) {
    return result;
  }

  if (!ExistingRuntimeArtifactsAreSafeForReplacement(config, &result.diagnostics)) {
    return result;
  }

  if (!WriteTextFile(config.pid_file,
                     std::to_string(CurrentPid()) + "\n",
                     &result.diagnostics,
                     "SERVER.RUNTIME.PID_FILE_INVALID")) {
    return result;
  }
  const auto owner_file = OwnerFilePath(config);
  if (!WriteTextFile(owner_file,
                     OwnerFileContents(config, result.artifacts.generation),
                     &result.diagnostics,
                     "SERVER.RUNTIME.OWNER_TOKEN_INVALID")) {
    return result;
  }
  if (!WriteTextFile(config.lifecycle_state_file,
                     StateFileContents(config, target_state, result.artifacts.generation),
                     &result.diagnostics,
                     "SERVER.RUNTIME.LIFECYCLE_STATE_INVALID")) {
    return result;
  }
  AppendTextFile(config.lifecycle_journal_file,
                 JournalRecord("startup_lifecycle_ready", target_state, result.artifacts.generation),
                 &result.diagnostics);
  return result;
}

ServerLifecycleResult WriteStoppedLifecycleArtifacts(const ServerBootstrapConfig& config,
                                                     std::uint64_t generation) {
  ServerLifecycleResult result;
  result.artifacts.generation = generation == 0 ? NowMicros() : generation;
  result.artifacts.state = "stopped";
  result.artifacts.pid_file = config.pid_file.string();
  result.artifacts.owner_token_file = OwnerFilePath(config).string();
  result.artifacts.lifecycle_state_file = config.lifecycle_state_file.string();
  result.artifacts.lifecycle_journal_file = config.lifecycle_journal_file.string();
  result.artifacts.database_runtime_scope_id = ExpectedScopeId(config);
  result.artifacts.database_path = config.database_default_path.string();
  result.artifacts.daemon_scope = config.database_daemon_scope;
  result.artifacts.sbps_endpoint = config.sbps_endpoint.string();
  WriteTextFile(config.lifecycle_state_file,
                StateFileContents(config, "stopped", result.artifacts.generation),
                &result.diagnostics,
                "SERVER.RUNTIME.LIFECYCLE_STATE_INVALID");
  AppendTextFile(config.lifecycle_journal_file,
                 JournalRecord("stopped", "stopped", result.artifacts.generation),
                 &result.diagnostics);
  return result;
}

ServerRuntimeArtifactValidation ValidateServerRuntimeArtifacts(
    const ServerBootstrapConfig& config,
    const ServerLifecycleArtifacts& artifacts,
    bool require_existing_files) {
  ServerRuntimeArtifactValidation validation;

  const auto owner_file = artifacts.owner_token_file.empty()
      ? OwnerFilePath(config)
      : std::filesystem::path(artifacts.owner_token_file);
  const auto pid_file = artifacts.pid_file.empty()
      ? config.pid_file
      : std::filesystem::path(artifacts.pid_file);
  const auto state_file = artifacts.lifecycle_state_file.empty()
      ? config.lifecycle_state_file
      : std::filesystem::path(artifacts.lifecycle_state_file);
  const auto endpoint = artifacts.sbps_endpoint.empty()
      ? config.sbps_endpoint
      : std::filesystem::path(artifacts.sbps_endpoint);

  auto add = [&](std::string code, std::string key, std::string message,
                 std::vector<ServerDiagnosticField> fields = {}) {
    validation.diagnostics.push_back(RuntimeDiagnostic(std::move(code),
                                                       std::move(key),
                                                       std::move(message),
                                                       std::move(fields)));
  };

  validation.directories_valid =
      std::filesystem::is_directory(config.control_dir) &&
      std::filesystem::is_directory(config.data_dir) &&
      IsOwnedByCurrentUser(config.control_dir) &&
      IsOwnedByCurrentUser(config.data_dir) &&
      HasPrivatePermissions(config.control_dir, true) &&
      HasPrivatePermissions(config.data_dir, true);
  if (!validation.directories_valid) {
    add("SERVER.RUNTIME.DIRECTORY_VALIDATION_FAILED",
        "server.runtime.directory_validation_failed",
        "The server runtime directories are missing or unsafe.",
        {{"control_dir", config.control_dir.string()}, {"data_dir", config.data_dir.string()}});
  }

  const bool pid_exists = std::filesystem::exists(pid_file);
  const bool owner_exists = std::filesystem::exists(owner_file);
  const bool state_exists = std::filesystem::exists(state_file);
  if (require_existing_files && (!pid_exists || !owner_exists || !state_exists)) {
    add("SERVER.RUNTIME.ARTIFACT_MISSING",
        "server.runtime.artifact_missing",
        "Required server runtime artifacts are missing.",
        {{"pid_file", pid_file.string()},
         {"owner_token_file", owner_file.string()},
         {"lifecycle_state_file", state_file.string()}});
  }

  validation.pid_file_valid =
      pid_exists && IsOwnedByCurrentUser(pid_file) && HasPrivatePermissions(pid_file, false);
  if (pid_exists && !validation.pid_file_valid) {
    add("SERVER.RUNTIME.PID_FILE_INVALID",
        "server.runtime.pid_file_invalid",
        "The server PID file has unsafe ownership or permissions.",
        {{"pid_file", pid_file.string()}});
  }

  validation.owner_token_valid =
      owner_exists && IsOwnedByCurrentUser(owner_file) && HasPrivatePermissions(owner_file, false);
  std::map<std::string, std::string> owner_values;
  if (owner_exists && validation.owner_token_valid) {
    owner_values = ReadKeyValueFile(owner_file);
    const auto owner_pid = ReadPidFromKeyValues(owner_values);
    const auto pid_file_pid = ReadPidFile(pid_file);
    if (!owner_pid || !pid_file_pid || *owner_pid != *pid_file_pid) {
      validation.owner_token_valid = false;
      add("SERVER.RUNTIME.OWNER_TOKEN_AMBIGUOUS",
          "server.runtime.owner_token_ambiguous",
          "The server owner token and PID file do not identify the same process.",
          {{"owner_token_file", owner_file.string()}});
    }
  } else if (owner_exists) {
    add("SERVER.RUNTIME.OWNER_TOKEN_INVALID",
        "server.runtime.owner_token_invalid",
        "The server owner token has unsafe ownership or permissions.",
        {{"owner_token_file", owner_file.string()}});
  }

  validation.lifecycle_state_valid =
      state_exists && IsOwnedByCurrentUser(state_file) && HasPrivatePermissions(state_file, false);
  if (state_exists && validation.lifecycle_state_valid) {
    const auto state_values = ReadKeyValueFile(state_file);
    const auto format = state_values.find("format");
    const auto state = state_values.find("state");
    if (format == state_values.end() || format->second != "SB_SERVER_LIFECYCLE_STATE_V1" ||
        state == state_values.end() || state->second.empty()) {
      validation.lifecycle_state_valid = false;
      add("SERVER.RUNTIME.LIFECYCLE_STATE_INVALID",
          "server.runtime.lifecycle_state_invalid",
          "The server lifecycle state artifact is malformed.",
          {{"lifecycle_state_file", state_file.string()}});
    }
  } else if (state_exists) {
    add("SERVER.RUNTIME.LIFECYCLE_STATE_INVALID",
        "server.runtime.lifecycle_state_invalid",
        "The server lifecycle state artifact has unsafe ownership or permissions.",
        {{"lifecycle_state_file", state_file.string()}});
  }

  const auto expected_scope = ExpectedScopeId(config);
  const auto owner_scope = owner_values.find("database_runtime_scope_id");
  const auto owner_database = owner_values.find("database_path");
  const auto owner_daemon = owner_values.find("daemon_scope");
  validation.database_association_valid =
      !IsDedicatedScope(config) ||
      (owner_scope != owner_values.end() && owner_scope->second == expected_scope &&
       owner_database != owner_values.end() &&
       std::filesystem::path(owner_database->second).lexically_normal() ==
           config.database_default_path.lexically_normal() &&
       owner_daemon != owner_values.end() && owner_daemon->second == config.database_daemon_scope);
  if (!validation.database_association_valid) {
    add("SERVER.RUNTIME.DATABASE_SCOPE_INVALID",
        "server.runtime.database_scope_invalid",
        "The server runtime artifacts do not prove the expected database association.",
        {{"database_runtime_scope_id", expected_scope}});
  }

  validation.endpoint_descriptor_valid =
      !endpoint.empty() && endpoint.is_absolute() &&
      endpoint.lexically_normal().string().rfind(config.control_dir.lexically_normal().string(), 0) == 0;
  if (!validation.endpoint_descriptor_valid) {
    add("SERVER.RUNTIME.ENDPOINT_DESCRIPTOR_INVALID",
        "server.runtime.endpoint_descriptor_invalid",
        "The server endpoint descriptor is not scoped to the validated control directory.",
        {{"endpoint", endpoint.string()}});
  }

  validation.cleanup_safe = validation.directories_valid &&
                            (!owner_exists || validation.owner_token_valid) &&
                            (!pid_exists || validation.pid_file_valid) &&
                            validation.database_association_valid;
  return validation;
}

ServerLifecycleResult CleanupServerRuntimeArtifacts(const ServerBootstrapConfig& config,
                                                    const ServerLifecycleArtifacts& artifacts,
                                                    ServerRuntimeCleanupOperation operation) {
  ServerLifecycleResult result;
  result.artifacts = artifacts;
  if (result.artifacts.generation == 0) {
    result.artifacts.generation = NowMicros();
  }
  result.artifacts.state = operation == ServerRuntimeCleanupOperation::kRestart
      ? "restart_cleanup"
      : (operation == ServerRuntimeCleanupOperation::kUninstall ? "uninstall_cleanup" : "stopped");
  result.artifacts.pid_file = config.pid_file.string();
  result.artifacts.owner_token_file = OwnerFilePath(config).string();
  result.artifacts.lifecycle_state_file = config.lifecycle_state_file.string();
  result.artifacts.lifecycle_journal_file = config.lifecycle_journal_file.string();
  result.artifacts.database_runtime_scope_id = ExpectedScopeId(config);
  result.artifacts.database_path = config.database_default_path.string();
  result.artifacts.daemon_scope = config.database_daemon_scope;
  result.artifacts.sbps_endpoint = config.sbps_endpoint.string();

  const auto validation = ValidateServerRuntimeArtifacts(config, result.artifacts, false);
  if (!validation.cleanup_safe) {
    result.diagnostics = validation.diagnostics;
    if (result.diagnostics.empty()) {
      result.diagnostics.push_back(RuntimeDiagnostic("SERVER.RUNTIME.CLEANUP_UNSAFE",
                                                    "server.runtime.cleanup_unsafe",
                                                    "Server runtime cleanup was refused because scope was not proven."));
    }
    return result;
  }

  std::error_code ec;
  std::filesystem::remove(config.sbps_endpoint, ec);
  std::filesystem::remove(config.pid_file, ec);
  std::filesystem::remove(OwnerFilePath(config), ec);
  if (operation != ServerRuntimeCleanupOperation::kRestart) {
    WriteStoppedLifecycleArtifacts(config, result.artifacts.generation);
  }
  if (operation == ServerRuntimeCleanupOperation::kUninstall && IsDedicatedScope(config)) {
    std::filesystem::remove(config.lifecycle_state_file, ec);
    std::filesystem::remove(config.lifecycle_journal_file, ec);
    std::filesystem::remove(config.control_dir, ec);
    std::filesystem::remove(config.data_dir, ec);
  }
  return result;
}

}  // namespace scratchbird::server

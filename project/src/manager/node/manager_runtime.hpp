// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// SEARCH_KEY: SBMI_NODE_MANAGER_RUNTIME

#pragma once

#include "manager_protocol.hpp"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace scratchbird::manager::node {

namespace proto = scratchbird::manager::protocol;

struct ManagerConfig {
  bool help = false;
  bool version = false;
  bool foreground = false;
  bool service = false;
  bool validate_config = false;
  bool proxy_enabled = true;
  bool restart_enabled = false;
  bool third_party_management_enabled = false;
  bool no_spin_required = true;
  std::string release_profile = "enterprise";
  std::string bind_address = "0.0.0.0";
  std::uint16_t proxy_port = 3090;
  std::uint32_t proxy_backlog = 128;
  std::uint32_t proxy_max_clients = 1024;
  std::uint64_t proxy_client_idle_timeout_ms = 300000;
  std::uint64_t proxy_backend_connect_timeout_ms = 5000;
  std::uint64_t proxy_io_timeout_ms = 30000;
  std::uint32_t management_backlog = 64;
  std::uint32_t management_max_clients = 128;
  std::uint32_t management_max_payload_bytes = 1024 * 1024;
  std::uint64_t management_idle_timeout_ms = 30000;
  std::string native_bind = "127.0.0.1";
  std::uint16_t native_port = 3392;
  std::string owner_database_name = "main";
  std::filesystem::path owner_database_path;
  proto::UuidBytes owner_database_uuid{};
  bool owner_database_uuid_set = false;
  std::string owner_database_runtime_scope_id;
  std::filesystem::path security_token_store_path;
  std::uint32_t listener_id = 1;
  std::filesystem::path listener_control_socket_dir;
  std::uint64_t dbbt_ttl_ms = 30000;
  std::uint64_t dbbt_clock_skew_ms = 2000;
  std::uint32_t dbbt_replay_cache_entries = 4096;
  std::filesystem::path dbbt_keyring_path;
  std::string mcp_secret_ref;
  std::string mcp_secret_rights;
  std::uint64_t heartbeat_interval_ms = 5000;
  std::uint64_t heartbeat_timeout_ms = 2000;
  std::uint32_t missed_heartbeat_threshold = 3;
  std::uint32_t restart_max_attempts = 3;
  std::uint64_t restart_window_ms = 600000;
  std::uint64_t restart_initial_backoff_ms = 1000;
  std::uint64_t restart_max_backoff_ms = 60000;
  std::filesystem::path restart_executable;
  std::string restart_arguments;
  std::filesystem::path config_path;
  std::filesystem::path runtime_dir;
  std::filesystem::path control_dir;
  std::filesystem::path log_path;
  std::string log_level = "info";
};

struct ParseResult {
  ManagerConfig config;
  std::vector<proto::Diagnostic> diagnostics;
  bool ok() const { return diagnostics.empty(); }
};

struct RuntimeResult {
  int exit_code = 0;
  std::vector<proto::Diagnostic> diagnostics;
};

struct ManagerRuntimePaths {
  std::filesystem::path runtime_dir;
  std::filesystem::path control_dir;
  std::filesystem::path pid_file;
  std::filesystem::path owner_file;
  std::filesystem::path state_file;
  std::filesystem::path control_socket;
  std::filesystem::path audit_file;
  std::filesystem::path metrics_file;
  std::string owner_database_runtime_scope_id;
};

struct ManagerRuntimeValidation {
  bool directories_valid = false;
  bool pid_owner_valid = false;
  bool database_association_valid = false;
  bool cleanup_safe = false;
  std::vector<proto::Diagnostic> diagnostics;
  bool ok() const { return diagnostics.empty(); }
};

enum class ManagerRuntimeCleanupOperation {
  kStop,
  kRestart,
  kUninstall,
};

std::string ProductVersionLine();
std::string HelpText();
std::string ManagerOwnerDatabaseRuntimeScopeId(const ManagerConfig& config);
ManagerRuntimePaths ResolveManagerRuntimePaths(const ManagerConfig& config);
ManagerRuntimeValidation ValidateManagerRuntimeArtifacts(const ManagerConfig& config,
                                                         bool require_existing_files);
RuntimeResult CleanupManagerRuntimeArtifacts(const ManagerConfig& config,
                                             ManagerRuntimeCleanupOperation operation);
ParseResult ParseManagerCli(int argc, char** argv);
RuntimeResult RunManager(const ManagerConfig& config);

}  // namespace scratchbird::manager::node

// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// SEARCH_KEY: SB_SERVER_CONFIG_LIFECYCLE_CONFIG

#pragma once

#include "cli.hpp"
#include "diagnostics.hpp"
#include "memory_policy_config.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace scratchbird::server {

inline constexpr const char* kServerConfigFormatCurrent = "SBCD1";
inline constexpr std::uint32_t kServerConfigFormatVersionCurrent = 1;
inline constexpr std::uint32_t kServerConfigFormatVersionMinSupported = 1;
inline constexpr std::uint32_t kServerConfigFormatVersionMaxSupported = 1;

enum class ServerMode {
  kForeground,
  kService,
  kValidationOnly,
  kMaintenance,
  kReadOnly,
};

enum class ServerConfigCompatibilityClass {
  kSupportedCurrent,
  kUnsupportedOld,
  kUnsupportedNew,
  kDowngradeRefused,
  kNewerThanSupportedRefused,
  kMissingMigrationPlanRefused,
  kMigrationRequiredWithoutPlanRefused,
};

struct ServerBootstrapConfig {
  ServerMode mode = ServerMode::kForeground;
  bool allow_current_directory = true;
  std::filesystem::path data_dir = "/var/lib/scratchbird";
  std::filesystem::path control_dir = "/run/scratchbird";
  std::filesystem::path pid_file;
  std::filesystem::path lifecycle_state_file;
  std::filesystem::path lifecycle_journal_file;
  std::string log_file = "stderr";
  std::string log_level = "info";
  std::uint64_t config_source_epoch = 1;
  std::uint64_t config_reload_generation = 1;
  std::uint64_t cache_invalidation_epoch = 1;
  std::uint64_t capability_policy_generation = 1;
  std::string security_authority_mode = "database_local";
  std::filesystem::path security_database_path;
  std::uint64_t security_policy_generation = 1;
  std::uint64_t security_epoch = 1;
  std::string security_provider_family = "local_password";
  std::uint64_t security_provider_generation = 1;
  std::string security_provider_state = "healthy";
  bool security_default_policy_installed = true;
  std::filesystem::path database_default_path;
  std::filesystem::path database_resource_seed_pack_root;
  std::filesystem::path database_policy_seed_pack_root;
  std::string database_runtime_scope_id;
  bool database_auto_create = false;
  std::uint64_t database_create_page_size_bytes = 16384;
  std::string database_open_mode = "normal";
  std::string database_daemon_scope = "shared";
  bool database_ownership_prelocked = false;
  std::string database_ownership_owner_kind = "server";
  bool embedded_direct_mode = false;
  bool listener_native_enabled = false;
  std::string listener_native_bind_host = "127.0.0.1";
  std::uint64_t listener_native_port = 3050;
  std::filesystem::path listener_native_executable_path;
  std::filesystem::path listener_native_parser_executable_path;
  std::filesystem::path listener_native_control_dir;
  std::filesystem::path listener_native_runtime_dir;
  bool listener_native_tls_required = true;
  std::filesystem::path listener_native_tls_cert_file;
  std::filesystem::path listener_native_tls_key_file;
  std::filesystem::path listener_native_tls_ca_file;
  std::uint64_t listener_native_ready_timeout_ms = 5000;
  bool metrics_enabled = true;
  std::uint64_t metrics_flush_interval_ms = 5000;
  std::string memory_policy_name = "default_local_server_memory_cache_v1";
  std::uint64_t memory_hard_limit_bytes = 1024ull * 1024ull * 1024ull;
  std::uint64_t memory_soft_limit_bytes = 768ull * 1024ull * 1024ull;
  std::uint64_t memory_per_context_limit_bytes = 256ull * 1024ull * 1024ull;
  std::uint64_t memory_page_buffer_pool_limit_bytes = 512ull * 1024ull * 1024ull;
  std::uint64_t memory_min_startup_available_bytes = 1024ull * 1024ull * 1024ull;
  scratchbird::core::memory::AllocationFailureMode memory_failure_mode =
      scratchbird::core::memory::AllocationFailureMode::return_error;
  bool memory_track_allocations = true;
  bool memory_zero_memory_on_allocate = false;
  bool memory_zero_memory_on_release = true;
  bool memory_reject_over_soft_limit = false;
  bool memory_adaptive_page_cache_enabled = true;
  bool memory_index_read_cache_enabled = true;
  bool memory_trim_heap_on_disconnect = false;
  std::string memory_policy_provenance =
      "default_policy_pack:default-local-password:server_memory_cache_policy";
  std::uint64_t memory_policy_generation = 1;
  bool memory_enable_platform_memory_probe = true;
  bool memory_require_platform_memory_ceiling = false;
  std::filesystem::path parser_registry_path;
  std::uint64_t parser_worker_restart_max = 3;
  std::uint64_t parser_worker_restart_window_ms = 60000;
  bool sbps_enabled = true;
  std::filesystem::path sbps_endpoint;
  std::uint64_t sbps_max_frame_bytes = 1048576;
  std::uint64_t sbps_max_streams = 16;
  std::uint64_t sbps_hello_timeout_ms = 5000;
  std::vector<std::string> server_agent_runtime_test_options;
  std::optional<std::filesystem::path> selected_config_path;
  std::string selected_config_source = "compiled_defaults";
};

struct ServerConfigLoadResult {
  ServerBootstrapConfig config;
  std::vector<ServerDiagnostic> diagnostics;

  bool ok() const { return diagnostics.empty(); }
};

struct ServerConfigCompatibilityResult {
  bool accepted = false;
  bool migration_required = false;
  ServerConfigCompatibilityClass compatibility_class =
      ServerConfigCompatibilityClass::kUnsupportedNew;
  ServerDiagnostic diagnostic;
};

const char* ServerModeName(ServerMode mode);
const char* ServerConfigCompatibilityClassName(ServerConfigCompatibilityClass compatibility_class);
ServerConfigCompatibilityResult ClassifyServerConfigFormat(
    std::string_view format,
    std::string_view migration_plan_id = {},
    bool downgrade_requested = false,
    bool migration_plan_required = false);
std::string ServerDatabaseRuntimeScopeId(const std::filesystem::path& database_path);
scratchbird::core::memory::MemoryPolicyConfigResolveResult ResolveServerMemoryAllocationPolicy(
    const ServerBootstrapConfig& config);
ServerConfigLoadResult ResolveServerBootstrapConfig(const ServerCliOptions& cli);

}  // namespace scratchbird::server

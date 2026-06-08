// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// SEARCH_KEY: SB_SERVER_CONFIG_LIFECYCLE_STARTUP

#include "startup.hpp"

#include "memory.hpp"

#include <sstream>

namespace scratchbird::server {

namespace {

std::string JsonEscape(const std::string& value) {
  return EscapeMessageVectorText(value);
}

std::string ValidationSummaryJson(const ServerBootstrapConfig& config) {
  std::ostringstream out;
  out << "{\"server_config_validation\":{\"accepted\":true,"
      << "\"mode\":\"" << ServerModeName(config.mode) << "\","
      << "\"config_source\":\"" << config.selected_config_source << "\","
      << "\"control_dir\":\"" << JsonEscape(config.control_dir.string()) << "\","
      << "\"data_dir\":\"" << JsonEscape(config.data_dir.string()) << "\"";
  if (config.selected_config_path) {
    out << ",\"config_path\":\"" << JsonEscape(config.selected_config_path->string()) << "\"";
  }
  out << "}}\n";
  return out.str();
}

std::string StartupSummaryJson(const ServerBootstrapConfig& config,
                               const ServerLifecycleArtifacts& artifacts) {
  std::ostringstream out;
  out << "{\"server_startup\":{\"accepted\":true,"
      << "\"serving\":" << (config.sbps_enabled ? "true" : "false") << ","
      << "\"mode\":\"" << ServerModeName(config.mode) << "\","
      << "\"state\":\"" << artifacts.state << "\","
      << "\"state_generation\":" << artifacts.generation << ","
      << "\"sbps_endpoint\":\"" << JsonEscape(config.sbps_endpoint.string()) << "\","
      << "\"pid_file\":\"" << JsonEscape(artifacts.pid_file) << "\","
      << "\"owner_token_file\":\"" << JsonEscape(artifacts.owner_token_file) << "\","
      << "\"daemon_scope\":\"" << JsonEscape(artifacts.daemon_scope) << "\","
      << "\"database_runtime_scope_id\":\""
      << JsonEscape(artifacts.database_runtime_scope_id) << "\","
      << "\"lifecycle_state_file\":\"" << JsonEscape(artifacts.lifecycle_state_file) << "\","
      << "\"lifecycle_journal_file\":\"" << JsonEscape(artifacts.lifecycle_journal_file) << "\"}}\n";
  return out.str();
}

std::string TargetLifecycleState(const ServerBootstrapConfig& config) {
  if (config.mode == ServerMode::kMaintenance || config.database_open_mode == "maintenance" ||
      config.database_open_mode == "restricted" || config.database_open_mode == "restricted_open") {
    return "restricted_lifecycle_ready";
  }
  if (config.mode == ServerMode::kReadOnly || config.database_open_mode == "read_only") {
    return "read_only_lifecycle_ready";
  }
  return "config_lifecycle_ready";
}

ServerDiagnostic MemoryConfigInstallDiagnostic(
    const scratchbird::core::platform::DiagnosticRecord& diagnostic) {
  std::vector<ServerDiagnosticField> fields;
  fields.push_back({"source_component", diagnostic.source_component});
  for (const auto& argument : diagnostic.arguments) {
    fields.push_back({argument.key, argument.value});
  }
  return {diagnostic.diagnostic_code,
          diagnostic.message_key,
          ServerDiagnosticSeverity::kError,
          "The server memory policy could not be installed.",
          std::move(fields)};
}

}  // namespace

ServerStartupResult RunServerStartup(const ServerCliOptions& cli) {
  ServerStartupResult result;
  auto config = ResolveServerBootstrapConfig(cli);
  result.effective_config = config.config;
  if (!config.ok()) {
    result.exit_code = 2;
    result.diagnostics = std::move(config.diagnostics);
    return result;
  }

  auto memory_policy = ResolveServerMemoryAllocationPolicy(config.config);
  if (!memory_policy.ok()) {
    for (const auto& diagnostic : memory_policy.diagnostics) {
      result.diagnostics.push_back(MemoryConfigInstallDiagnostic(diagnostic));
    }
    result.exit_code = 2;
    return result;
  }
  auto memory_install = scratchbird::core::memory::ConfigureDefaultMemoryManager(
      memory_policy.policy,
      config.config.memory_policy_provenance);
  if (!memory_install.ok()) {
    result.diagnostics.push_back(MemoryConfigInstallDiagnostic(memory_install.diagnostic));
    result.exit_code = 2;
    return result;
  }

  if (config.config.mode == ServerMode::kValidationOnly) {
    result.exit_code = 0;
    result.stdout_text = ValidationSummaryJson(config.config);
    return result;
  }

  auto lifecycle = WriteStartupLifecycleArtifacts(config.config, TargetLifecycleState(config.config));
  result.lifecycle_artifacts = lifecycle.artifacts;
  if (!lifecycle.ok()) {
    result.exit_code = 2;
    result.diagnostics = std::move(lifecycle.diagnostics);
    return result;
  }

  result.exit_code = 0;
  result.serving_requested = config.config.sbps_enabled;
  result.stdout_text = StartupSummaryJson(config.config, lifecycle.artifacts);
  return result;
}

}  // namespace scratchbird::server

// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// SEARCH_KEY: SB_SERVER_CONFIG_LIFECYCLE_STARTUP

#include "startup.hpp"
#include "host_platform_admission.hpp"

#include "memory.hpp"
#include "uuid.hpp"

#include <algorithm>
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
    if (const auto* text = argument.text()) fields.push_back({argument.key, *text});
  }
  ServerDiagnostic result{diagnostic.diagnostic_code,
          diagnostic.message_key,
          ServerDiagnosticSeverity::kError,
          "The server memory policy could not be installed.",
          std::move(fields)};
  result.native_platform_source = diagnostic;
  return result;
}

ServerStartupResult CompleteServerStartup(ServerConfigLoadResult config) {
  ServerStartupResult result;
  result.effective_config = config.config;
  if (!config.ok()) {
    result.exit_code = 2;
    result.diagnostics = std::move(config.diagnostics);
    return result;
  }

  if (config.config.mode != ServerMode::kValidationOnly) {
    const auto host = ProbeServerHostPlatform();
    if (!host.supported) {
      result.exit_code = 2;
      result.diagnostics.push_back({
          "PROFILE.BUILTIN_PROFILE_UNAVAILABLE",
          "PROFILE.BUILTIN_PROFILE_UNAVAILABLE",
          ServerDiagnosticSeverity::kError,
          "The server host lacks a required platform capability.",
          {{"platform", host.platform},
           {"kernel_release", host.kernel_release},
           {"minimum_linux_kernel", "6.6"},
           {"failed_capability", host.failed_capability},
           {"native_error", std::to_string(host.native_error)},
           {"operator_action", "Use a qualified maintained kernel and permit required process/IPC APIs."}}});
      return result;
    }
  }

  auto memory_policy = ResolveServerMemoryAllocationPolicy(config.config);
  if (!memory_policy.ok()) {
    for (const auto& diagnostic : memory_policy.diagnostics) {
      result.diagnostics.push_back(MemoryConfigInstallDiagnostic(diagnostic));
    }
    result.exit_code = 2;
    return result;
  }
  namespace memory = scratchbird::core::memory;
  const auto reserve_bytes = memory::DefaultBootstrapEmergencyReserveBytes(
      memory_policy.effective_hard_limit_bytes);
  auto reserve_failure = [&](const char* reason) {
    result.exit_code = 2;
    result.diagnostics.push_back({
        "MEMORY.EMERGENCY_RESERVE_INVALID",
        "memory.emergency_reserve_invalid", ServerDiagnosticSeverity::kError,
        "The bootstrap emergency reserve could not be admitted.",
        {{"reason", reason}, {"emergency_reserve_bytes", std::to_string(reserve_bytes)},
         {"effective_hard_limit_bytes", std::to_string(memory_policy.effective_hard_limit_bytes)},
         {"hard_limit_bytes", std::to_string(config.config.memory_hard_limit_bytes)},
         {"min_startup_available_bytes", std::to_string(config.config.memory_min_startup_available_bytes)}}});
  };
  if (reserve_bytes >= memory_policy.effective_hard_limit_bytes) {
    reserve_failure("emergency_reserve_leaves_no_ordinary_capacity");
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

  const auto owner = scratchbird::core::uuid::IssueRuntimeIdentityV7();
  const auto context = scratchbird::core::uuid::IssueRuntimeIdentityV7();
  if (!owner || !context) {
    result.exit_code = 2;
    result.diagnostics.push_back({"SERVER.RUNTIME.OWNER_TOKEN_INVALID",
        "server.runtime.owner_token_invalid", ServerDiagnosticSeverity::kError,
        "A native bootstrap memory owner identity could not be issued.", {}});
    return result;
  }
  memory::MemoryTag tag;
  tag.category = memory::MemoryCategory::diagnostics;
  tag.lifetime = memory::MemoryLifetime::process;
  tag.purpose = "server_bootstrap_emergency_reserve";
  tag.binary_ownership[memory::MemoryBinaryScopeKind::owner] = owner->bytes;
  tag.binary_ownership[memory::MemoryBinaryScopeKind::context] = context->bytes;
  try {
    auto reserve = std::make_unique<memory::EmergencyMemoryReserve>(
        memory::DefaultMemoryManager(), std::move(tag));
    scratchbird::core::platform::DiagnosticRecord failure;
    if (!reserve->TryReset(reserve_bytes, &failure)) {
      if (!failure.diagnostic_code.empty()) {
        result.diagnostics.push_back(MemoryConfigInstallDiagnostic(failure));
        result.exit_code = 2;
      } else {
        reserve_failure("emergency_backing_unavailable");
      }
      return result;
    }
    result.emergency_reserve = std::move(reserve);
  } catch (const std::bad_alloc&) {
    // Allocation failure cannot yield an admission grant, including failure
    // to construct the diagnostic itself. Bounded emergency logging is a
    // separate consumer; do not recurse into it before backing exists.
    result.exit_code = 2;
    try { reserve_failure("emergency_backing_allocation_failed"); }
    catch (const std::bad_alloc&) {}
    return result;
  }
  // Allocating and initializing the reserve can take time. Re-observe before
  // publishing admission; never silently keep an installed stale envelope.
  // This is a conservative observation check, not an atomic OS-limit fence.
  const auto activation_policy = ResolveServerMemoryAllocationPolicy(config.config);
  if (!activation_policy.ok()) {
    result.emergency_reserve.reset();
    result.exit_code = 2;
    for (const auto& diagnostic : activation_policy.diagnostics) {
      result.diagnostics.push_back(MemoryConfigInstallDiagnostic(diagnostic));
    }
    return result;
  }
  const auto& before = memory_policy.ceiling_evidence;
  const auto& after = activation_policy.ceiling_evidence;
  const bool unchanged =
      memory_policy.effective_hard_limit_bytes == activation_policy.effective_hard_limit_bytes &&
      memory_policy.degraded_container_limit == activation_policy.degraded_container_limit &&
      before.platform_name == after.platform_name &&
      before.platform_supported == after.platform_supported &&
      before.container_limit_incomplete == after.container_limit_incomplete &&
      before.available_ceiling_bytes == after.available_ceiling_bytes &&
      std::equal(before.signals.begin(), before.signals.end(),
                 after.signals.begin(), after.signals.end(), [](const auto& a, const auto& b) {
        return a.kind == b.kind && a.source == b.source && a.raw_value == b.raw_value &&
               a.available == b.available && a.valid == b.valid &&
               a.finite == b.finite && a.bytes == b.bytes && a.evidence == b.evidence;
      });
  if (!unchanged) {
    result.emergency_reserve.reset();
    reserve_failure("memory_envelope_changed_during_bootstrap");
    result.diagnostics.back().fields.push_back({"activation_effective_hard_limit_bytes",
        std::to_string(activation_policy.effective_hard_limit_bytes)});
    return result;
  }
  for (const auto& warning : memory_policy.warnings) {
    auto diagnostic = MemoryConfigInstallDiagnostic(warning);
    diagnostic.severity = ServerDiagnosticSeverity::kWarning;
    diagnostic.safe_message =
        "Starting with an explicitly capped degraded memory policy; the container limit remains unverified.";
    // Preserve canonical private identity; expose only numeric generation.
    diagnostic.fields.push_back({"memory_generation", std::to_string(config.config.memory_policy_generation)});
    result.diagnostics.push_back(std::move(diagnostic));
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

}  // namespace

ServerStartupResult RunServerStartup(const ServerCliOptions& cli) {
  return CompleteServerStartup(ResolveServerBootstrapConfig(cli));
}

ServerStartupResult RunServerStartup(const ServerCliOptions& cli,
                                    const ServerConfigResolutionContext& context) {
  return CompleteServerStartup(ResolveServerBootstrapConfig(cli, context));
}

bool WriteServerStartupDiagnostics(const ServerStartupResult& startup,
                                   std::ostream& channel) noexcept {
  try {
    for (const auto& diagnostic : startup.diagnostics) {
      channel << ToMessageVectorJsonLine(diagnostic) << '\n';
      if (!channel) return false;
    }
    channel.flush();
    return static_cast<bool>(channel);
  } catch (...) {
    // No recursive diagnostics on the same failed channel, and no grant.
    return false;
  }
}

}  // namespace scratchbird::server

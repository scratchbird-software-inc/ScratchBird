// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// SEARCH_KEY: SB_SERVER_CONFIG_POLICY_SECURITY_LIFECYCLE

#include "config_policy_security_lifecycle.hpp"

#include "security/auth_provider_model.hpp"

#include <algorithm>
#include <array>
#include <sstream>
#include <string_view>

namespace scratchbird::server {
namespace {

namespace engine_api = scratchbird::engine::internal_api;

std::string JsonEscape(const std::string& value) {
  return EscapeMessageVectorText(value);
}

ServerDiagnostic LifecycleDiagnostic(std::string code,
                                     std::string detail,
                                     std::vector<ServerDiagnosticField> fields = {}) {
  if (!detail.empty()) fields.push_back({"detail", detail});
  return ServerDiagnostic{std::move(code),
                          "server.config_policy_security_lifecycle",
                          ServerDiagnosticSeverity::kError,
                          "Configuration, policy, or security-provider lifecycle admission failed.",
                          std::move(fields)};
}

ConfigPolicySecurityLifecycleResult Failure(std::string code,
                                            std::string detail,
                                            std::vector<ServerDiagnosticField> fields = {}) {
  ConfigPolicySecurityLifecycleResult result;
  result.accepted = false;
  result.diagnostic = LifecycleDiagnostic(std::move(code), std::move(detail), std::move(fields));
  result.diagnostics.push_back(result.diagnostic);
  return result;
}

DatabaseLifecycleThreatModelResult ThreatFailure(const DatabaseLifecycleThreatModelInput& input,
                                                 std::string code,
                                                 std::string detail,
                                                 std::vector<ServerDiagnosticField> fields = {}) {
  fields.push_back({"surface", DatabaseLifecycleThreatSurfaceName(input.surface)});
  fields.push_back({"operation_key", input.operation_key});
  fields.push_back({"authority_actor", input.authority_actor});
  DatabaseLifecycleThreatModelResult result;
  result.accepted = false;
  result.covered_surfaces.push_back(DatabaseLifecycleThreatSurfaceName(input.surface));
  result.evidence.push_back("gate:database_lifecycle_threat_model");
  result.evidence.push_back("completion_gate:DBLC_P13T_THREAT_MODEL_COMPLETE");
  result.diagnostics.push_back(
      ServerDiagnostic{std::move(code),
                       "database_lifecycle.threat_model",
                       ServerDiagnosticSeverity::kError,
                       std::move(detail),
                       std::move(fields)});
  return result;
}

DatabaseLifecycleThreatModelResult ThreatAccepted(
    const DatabaseLifecycleThreatModelInput& input) {
  DatabaseLifecycleThreatModelResult result;
  result.accepted = true;
  result.covered_surfaces.push_back(DatabaseLifecycleThreatSurfaceName(input.surface));
  result.evidence.push_back("gate:database_lifecycle_threat_model");
  result.evidence.push_back("static_gate:DBLC_STATIC_THREAT_MODEL_ABUSE_CASES");
  result.evidence.push_back("completion_gate:DBLC_P13T_THREAT_MODEL_COMPLETE");
  result.evidence.push_back(std::string("surface:") +
                            DatabaseLifecycleThreatSurfaceName(input.surface));
  result.evidence.push_back("authority:engine_owned");
  result.evidence.push_back("decision:fail_closed_when_unproven");
  return result;
}

std::uint64_t RequiredCacheEpoch(const ConfigPolicySecurityLifecycle& lifecycle) {
  return std::max({lifecycle.config_source_epoch,
                   lifecycle.config_reload_generation,
                   lifecycle.capability_policy_generation,
                   lifecycle.policy_generation,
                   lifecycle.security_epoch,
                   lifecycle.provider_generation});
}

std::array<std::string_view, 9> CacheTargets() {
  return {"sessions",
          "prepared_statements",
          "parser_pools",
          "capability_policy_cache",
          "listener_pools",
          "manager_routes",
          "ipc_channels",
          "metrics_descriptors",
          "security_assertion_caches"};
}

void PopulateCacheTargets(ConfigPolicySecurityLifecycle* lifecycle) {
  lifecycle->invalidated_cache_targets.clear();
  for (const std::string_view target : CacheTargets()) {
    lifecycle->invalidated_cache_targets.emplace_back(target);
  }
}

bool ProviderStateAdmitsAuthentication(SecurityProviderLifecycleState state) {
  return state == SecurityProviderLifecycleState::kHealthy ||
         state == SecurityProviderLifecycleState::kStarted;
}

std::string CanonicalProviderFamily(const std::string& provider_family) {
  auto family = engine_api::CanonicalAuthProviderFamily(provider_family);
  return family.empty() ? "local_password" : family;
}

}  // namespace

const char* SecurityProviderLifecycleStateName(SecurityProviderLifecycleState state) {
  switch (state) {
    case SecurityProviderLifecycleState::kLoaded:
      return "loaded";
    case SecurityProviderLifecycleState::kStarted:
      return "started";
    case SecurityProviderLifecycleState::kHealthy:
      return "healthy";
    case SecurityProviderLifecycleState::kDegraded:
      return "degraded";
    case SecurityProviderLifecycleState::kQuiescing:
      return "quiescing";
    case SecurityProviderLifecycleState::kQuiesced:
      return "quiesced";
    case SecurityProviderLifecycleState::kDisabled:
      return "disabled";
    case SecurityProviderLifecycleState::kQuarantined:
      return "quarantined";
  }
  return "quarantined";
}

const char* DatabaseLifecycleThreatSurfaceName(DatabaseLifecycleThreatSurface surface) {
  switch (surface) {
    case DatabaseLifecycleThreatSurface::kForceShutdown:
      return "force_shutdown";
    case DatabaseLifecycleThreatSurface::kIpcAuthentication:
      return "ipc_authentication";
    case DatabaseLifecycleThreatSurface::kIpcAuthorization:
      return "ipc_authorization";
    case DatabaseLifecycleThreatSurface::kManagerSupervision:
      return "manager_supervision";
    case DatabaseLifecycleThreatSurface::kListenerSupervision:
      return "listener_supervision";
    case DatabaseLifecycleThreatSurface::kParserSupervision:
      return "parser_supervision";
    case DatabaseLifecycleThreatSurface::kUdrLoading:
      return "udr_loading";
    case DatabaseLifecycleThreatSurface::kHealthReporting:
      return "health_reporting";
    case DatabaseLifecycleThreatSurface::kBackupRestore:
      return "backup_restore";
    case DatabaseLifecycleThreatSurface::kSupportBundle:
      return "support_bundle";
    case DatabaseLifecycleThreatSurface::kServiceFiles:
      return "service_files";
    case DatabaseLifecycleThreatSurface::kResourceQuota:
      return "resource_quota";
  }
  return "unknown";
}

SecurityProviderLifecycleState ParseSecurityProviderLifecycleState(const std::string& value) {
  if (value == "loaded") return SecurityProviderLifecycleState::kLoaded;
  if (value == "started") return SecurityProviderLifecycleState::kStarted;
  if (value == "healthy") return SecurityProviderLifecycleState::kHealthy;
  if (value == "degraded") return SecurityProviderLifecycleState::kDegraded;
  if (value == "quiescing") return SecurityProviderLifecycleState::kQuiescing;
  if (value == "quiesced") return SecurityProviderLifecycleState::kQuiesced;
  if (value == "disabled") return SecurityProviderLifecycleState::kDisabled;
  return SecurityProviderLifecycleState::kQuarantined;
}

ConfigPolicySecurityLifecycleInput BuildConfigPolicySecurityLifecycleInput(
    const ServerBootstrapConfig& config,
    std::string database_path,
    std::string database_uuid,
    bool database_open,
    bool cluster_authority_required) {
  ConfigPolicySecurityLifecycleInput input;
  input.database_path = std::move(database_path);
  input.database_uuid = std::move(database_uuid);
  input.database_open = database_open;
  input.cluster_authority_required =
      cluster_authority_required || config.security_authority_mode == "cluster";
  input.config_source = config.selected_config_source;
  input.config_source_epoch = config.config_source_epoch;
  input.config_reload_generation = config.config_reload_generation;
  input.capability_policy_generation = config.capability_policy_generation;
  input.policy_generation = config.security_policy_generation;
  input.security_epoch = config.security_epoch;
  input.provider_family = config.security_provider_family;
  input.provider_generation = config.security_provider_generation;
  input.provider_state = ParseSecurityProviderLifecycleState(config.security_provider_state);
  input.default_policy_installed = config.security_default_policy_installed;
  input.cache_invalidation_epoch = config.cache_invalidation_epoch;
  return input;
}

ConfigPolicySecurityLifecycleResult StartConfigPolicySecurityLifecycle(
    const ConfigPolicySecurityLifecycleInput& input) {
  if (!input.database_open || input.database_uuid.empty() || input.database_path.empty()) {
    return Failure("ENGINE.DBLC_CONFIG_POLICY_SECURITY_NOT_READY", "database_not_open");
  }
  if (input.descriptor_version < kConfigPolicySecurityLifecycleDescriptorMinSupported) {
    return Failure("ENGINE.DBLC_PROTOCOL_VERSION_TOO_OLD",
                   "config_policy_security_descriptor_too_old");
  }
  if (input.descriptor_version > kConfigPolicySecurityLifecycleDescriptorMaxSupported) {
    return Failure("ENGINE.DBLC_PROTOCOL_VERSION_FUTURE_REFUSED",
                   "config_policy_security_descriptor_future");
  }
  if (input.cluster_authority_required) {
    return Failure("ENGINE.DBLC_STANDALONE_CLUSTER_FAIL_CLOSED",
                   "cluster_security_authority_unavailable");
  }
  if (input.config_source_epoch == 0 || input.config_reload_generation == 0 ||
      input.capability_policy_generation == 0 || input.policy_generation == 0 ||
      input.security_epoch == 0 ||
      input.provider_generation == 0 || input.cache_invalidation_epoch == 0) {
    return Failure("ENGINE.DBLC_CONFIG_POLICY_SECURITY_NOT_READY",
                   "generation_zero_refused");
  }
  if (!input.default_policy_installed) {
    return Failure("ENGINE.DBLC_CONFIG_POLICY_SECURITY_NOT_READY",
                   "default_policy_not_installed");
  }

  ConfigPolicySecurityLifecycle lifecycle;
  lifecycle.descriptor_version = input.descriptor_version;
  lifecycle.database_path = input.database_path;
  lifecycle.database_uuid = input.database_uuid;
  lifecycle.config_source = input.config_source.empty() ? "compiled_defaults" : input.config_source;
  lifecycle.config_source_epoch = input.config_source_epoch;
  lifecycle.config_reload_generation = input.config_reload_generation;
  lifecycle.capability_policy_generation = input.capability_policy_generation;
  lifecycle.policy_generation = input.policy_generation;
  lifecycle.security_epoch = input.security_epoch;
  lifecycle.provider_family = CanonicalProviderFamily(input.provider_family);
  lifecycle.provider_generation = input.provider_generation;
  lifecycle.provider_state = input.provider_state;
  lifecycle.default_policy_installed = input.default_policy_installed;
  lifecycle.cluster_authority_required = input.cluster_authority_required;
  lifecycle.provider_plugin_loaded =
      lifecycle.provider_state != SecurityProviderLifecycleState::kDisabled &&
      lifecycle.provider_state != SecurityProviderLifecycleState::kQuarantined;
  lifecycle.provider_started = lifecycle.provider_state != SecurityProviderLifecycleState::kLoaded &&
                               lifecycle.provider_plugin_loaded;
  lifecycle.provider_healthy = ProviderStateAdmitsAuthentication(lifecycle.provider_state);
  lifecycle.cache_invalidation_epoch =
      std::max(input.cache_invalidation_epoch, RequiredCacheEpoch(lifecycle));
  lifecycle.provider_lifecycle_trace = {"load", "start", SecurityProviderLifecycleStateName(input.provider_state)};
  PopulateCacheTargets(&lifecycle);

  if (!engine_api::IsKnownAuthProviderFamily(lifecycle.provider_family) ||
      !engine_api::AuthProviderFamilySupportsAuthn(lifecycle.provider_family)) {
    return Failure("ENGINE.DBLC_SECURITY_PROVIDER_UNAVAILABLE",
                   "provider_family_not_authn_capable:" + lifecycle.provider_family);
  }
  if (lifecycle.provider_family == "cluster_security") {
    return Failure("ENGINE.DBLC_STANDALONE_CLUSTER_FAIL_CLOSED",
                   "cluster_security_provider_requires_cluster_authority");
  }
  if (!lifecycle.provider_healthy) {
    return Failure("ENGINE.DBLC_SECURITY_PROVIDER_UNAVAILABLE",
                   std::string("provider_state_not_admitting_authentication:") +
                       SecurityProviderLifecycleStateName(lifecycle.provider_state));
  }

  ConfigPolicySecurityLifecycleResult result;
  result.accepted = true;
  result.lifecycle = std::move(lifecycle);
  return result;
}

ConfigPolicySecurityLifecycleResult ReloadConfigPolicySecurityLifecycle(
    const ConfigPolicySecurityLifecycle& current,
    std::uint64_t new_config_source_epoch,
    std::uint64_t new_capability_policy_generation,
    std::uint64_t new_policy_generation,
    std::uint64_t new_security_epoch) {
  if (new_config_source_epoch <= current.config_source_epoch ||
      new_capability_policy_generation <= current.capability_policy_generation ||
      new_policy_generation <= current.policy_generation ||
      new_security_epoch <= current.security_epoch) {
    return Failure("ENGINE.DBLC_STALE_POLICY_REFUSED", "reload_generation_not_monotonic");
  }
  auto next = current;
  next.config_source_epoch = new_config_source_epoch;
  next.config_reload_generation = current.config_reload_generation + 1;
  next.capability_policy_generation = new_capability_policy_generation;
  next.policy_generation = new_policy_generation;
  next.security_epoch = new_security_epoch;
  next.provider_generation = current.provider_generation + 1;
  next.provider_state = SecurityProviderLifecycleState::kHealthy;
  next.provider_plugin_loaded = true;
  next.provider_started = true;
  next.provider_healthy = true;
  next.cache_invalidation_epoch = RequiredCacheEpoch(next);
  next.provider_lifecycle_trace = {"quiesce", "reload_policy", "restart", "healthy"};
  PopulateCacheTargets(&next);

  ConfigPolicySecurityLifecycleResult result;
  result.accepted = true;
  result.lifecycle = std::move(next);
  return result;
}

ConfigPolicySecurityLifecycleResult ValidateConfigPolicySecurityAdmission(
    const ConfigPolicySecurityLifecycle& lifecycle,
    std::uint64_t observed_capability_policy_generation,
    std::uint64_t observed_policy_generation,
    std::uint64_t observed_security_epoch,
    std::uint64_t observed_provider_generation,
    const std::string& provider_family,
    const std::string& authority_actor) {
  if (authority_actor != "engine") {
    return Failure("ENGINE.DBLC_AUTHORITY_BYPASS_REFUSED",
                   "auth_authority_must_be_engine",
                   {{"authority_actor", authority_actor}});
  }
  if (lifecycle.descriptor_version < kConfigPolicySecurityLifecycleDescriptorMinSupported) {
    return Failure("ENGINE.DBLC_PROTOCOL_VERSION_TOO_OLD",
                   "config_policy_security_descriptor_too_old");
  }
  if (lifecycle.descriptor_version > kConfigPolicySecurityLifecycleDescriptorMaxSupported) {
    return Failure("ENGINE.DBLC_PROTOCOL_VERSION_FUTURE_REFUSED",
                   "config_policy_security_descriptor_future");
  }
  if (lifecycle.cluster_authority_required) {
    return Failure("ENGINE.DBLC_STANDALONE_CLUSTER_FAIL_CLOSED",
                   "cluster_security_authority_unavailable");
  }
  if (!lifecycle.default_policy_installed) {
    return Failure("ENGINE.DBLC_CONFIG_POLICY_SECURITY_NOT_READY",
                   "default_policy_not_installed");
  }
  if (!lifecycle.provider_plugin_loaded || !lifecycle.provider_started ||
      !lifecycle.provider_healthy) {
    return Failure("ENGINE.DBLC_SECURITY_PROVIDER_UNAVAILABLE",
                   std::string("provider_state_not_admitting_authentication:") +
                       SecurityProviderLifecycleStateName(lifecycle.provider_state));
  }
  const auto requested_provider = CanonicalProviderFamily(provider_family);
  const bool temporary_token_for_configured_authority =
      requested_provider == "security_database_temporary_token" &&
      (lifecycle.provider_family == "local_password" ||
       lifecycle.provider_family == "remote_security_database" ||
       lifecycle.provider_family == "cluster_security");
  if (requested_provider != lifecycle.provider_family &&
      !temporary_token_for_configured_authority) {
    return Failure("ENGINE.DBLC_SECURITY_PROVIDER_UNAVAILABLE",
                   "provider_family_mismatch:" + requested_provider);
  }
  if (observed_capability_policy_generation != lifecycle.capability_policy_generation ||
      observed_policy_generation != lifecycle.policy_generation ||
      observed_security_epoch != lifecycle.security_epoch ||
      observed_provider_generation != lifecycle.provider_generation ||
      lifecycle.cache_invalidation_epoch < RequiredCacheEpoch(lifecycle)) {
    return Failure("ENGINE.DBLC_STALE_POLICY_REFUSED",
                   "stale_policy_or_security_epoch_refused",
                   {{"observed_capability_policy_generation", std::to_string(observed_capability_policy_generation)},
                    {"current_capability_policy_generation", std::to_string(lifecycle.capability_policy_generation)},
                    {"observed_policy_generation", std::to_string(observed_policy_generation)},
                    {"current_policy_generation", std::to_string(lifecycle.policy_generation)},
                    {"observed_security_epoch", std::to_string(observed_security_epoch)},
                    {"current_security_epoch", std::to_string(lifecycle.security_epoch)},
                    {"observed_provider_generation", std::to_string(observed_provider_generation)},
                    {"current_provider_generation", std::to_string(lifecycle.provider_generation)}});
  }

  ConfigPolicySecurityLifecycleResult result;
  result.accepted = true;
  result.lifecycle = lifecycle;
  return result;
}

DatabaseLifecycleThreatModelResult EvaluateDatabaseLifecycleThreatModelGate(
    const DatabaseLifecycleThreatModelInput& input) {
  if (!input.session_context_present) {
    return ThreatFailure(input,
                         "DBLC.THREAT.SESSION_REQUIRED",
                         "Lifecycle security-sensitive operations require a bound session context.");
  }
  if (!input.security_context_present) {
    return ThreatFailure(input,
                         "DBLC.THREAT.SECURITY_CONTEXT_REQUIRED",
                         "Lifecycle security-sensitive operations require engine security context.");
  }
  if (input.authority_actor != "engine" ||
      input.parser_listener_driver_claims_authority) {
    return ThreatFailure(
        input,
        "DBLC.THREAT.ENGINE_AUTHORITY_REQUIRED",
        "Parser, listener, driver, and manager evidence cannot grant security or finality authority.");
  }
  if (!input.engine_authorization_granted || !input.required_role_present) {
    return ThreatFailure(input,
                         "SECURITY.AUTHORIZATION.DENIED",
                         "The engine authorization decision denied the required lifecycle right.",
                         {{"required_right", input.required_right}});
  }

  switch (input.surface) {
    case DatabaseLifecycleThreatSurface::kForceShutdown:
      if (!input.process_association_exact ||
          input.ambiguous_process_association) {
        return ThreatFailure(input,
                             "ENGINE.DBLC_ASSOCIATION_SCOPE_AMBIGUOUS",
                             "Force shutdown requires exact manager, listener, parser, IPC, and session association.");
      }
      if (input.force_shutdown_requested &&
          (!input.force_termination_policy_present ||
           !input.mga_recovery_evidence_preserved ||
           !input.active_transaction_finality_preserved)) {
        return ThreatFailure(
            input,
            "ENGINE.SHUTDOWN_FORCE_UNSAFE_REFUSED",
            "Unsafe force shutdown is refused unless policy, MGA recovery evidence, and unknown finality preservation are present.");
      }
      break;
    case DatabaseLifecycleThreatSurface::kIpcAuthentication:
      break;
    case DatabaseLifecycleThreatSurface::kIpcAuthorization:
      if (input.required_right.empty()) {
        return ThreatFailure(input,
                             "SECURITY.AUTHORIZATION.DENIED",
                             "IPC authorization requires a concrete engine right.");
      }
      break;
    case DatabaseLifecycleThreatSurface::kManagerSupervision:
    case DatabaseLifecycleThreatSurface::kListenerSupervision:
    case DatabaseLifecycleThreatSurface::kParserSupervision:
      if (!input.process_association_exact ||
          input.ambiguous_process_association ||
          input.ambiguous_supervision_owner) {
        return ThreatFailure(input,
                             "ENGINE.DBLC_ASSOCIATION_SCOPE_AMBIGUOUS",
                             "Supervision operations require an unambiguous process association owner.");
      }
      if (!input.supervisor_heartbeat_current ||
          !input.supervisor_restart_policy_present) {
        return ThreatFailure(input,
                             "SERVER.SUPERVISION.FAIL_CLOSED",
                             "Manager, listener, and parser supervision require current heartbeat and restart policy evidence.");
      }
      break;
    case DatabaseLifecycleThreatSurface::kUdrLoading:
      if (input.udr_or_plugin_load_requested &&
          (!input.package_policy_present ||
           !input.trusted_package_signature_present ||
           !input.parser_package_authorized)) {
        return ThreatFailure(
            input,
            "SB_ENGINE_API_UDR_TRUST_REQUIRED",
            "UDR, plugin, and parser package loading requires policy, trusted signature, and engine-authorized package admission.");
      }
      break;
    case DatabaseLifecycleThreatSurface::kHealthReporting:
      if (!input.health_redaction_applied || input.protected_material_requested) {
        return ThreatFailure(input,
                             "OPS.HEALTH.REDACTION_REQUIRED",
                             "Health reporting must redact protected material and local path detail.");
      }
      break;
    case DatabaseLifecycleThreatSurface::kBackupRestore:
      if (!input.backup_restore_policy_present) {
        return ThreatFailure(input,
                             "BACKUP_POLICY_REQUIRED",
                             "Backup and restore operations require explicit engine policy evidence.");
      }
      if (!input.backup_engine_owned_path) {
        return ThreatFailure(input,
                             "BACKUP_ENGINE_OWNED_PATH_REQUIRED",
                             "Backup and restore must use engine-owned paths and evidence.");
      }
      if (!input.restore_inspection_open) {
        return ThreatFailure(input,
                             "RESTORE_INSPECTION_OPEN_REQUIRED",
                             "Restore requires inspection-open recovery classification before mutation.");
      }
      break;
    case DatabaseLifecycleThreatSurface::kSupportBundle:
      if (!input.support_bundle_policy_present) {
        return ThreatFailure(input,
                             "OPS.SUPPORT_BUNDLE.POLICY_REQUIRED",
                             "Support bundles require retention, disposition, and redaction policy evidence.");
      }
      if (!input.support_bundle_redaction_applied ||
          input.protected_material_requested) {
        return ThreatFailure(input,
                             "OPS.SUPPORT_BUNDLE.REDACTION_REQUIRED",
                             "Support bundles must redact protected material.");
      }
      if (!input.supportability_flush_succeeded) {
        return ThreatFailure(input,
                             "OPS.SUPPORT_BUNDLE.FLUSH_REQUIRED",
                             "Support bundle export requires flushed supportability evidence before success.");
      }
      break;
    case DatabaseLifecycleThreatSurface::kServiceFiles:
      if (!input.service_file_private_mode) {
        return ThreatFailure(input,
                             "SERVER.SERVICE_FILE.PRIVATE_MODE_REQUIRED",
                             "Lifecycle service files require private file permissions.");
      }
      if (!input.service_file_owner_verified) {
        return ThreatFailure(input,
                             "SERVER.SERVICE_FILE.OWNER_REQUIRED",
                             "Lifecycle service files require verified engine owner evidence.");
      }
      if (!input.service_file_protected_material_absent) {
        return ThreatFailure(input,
                             "SERVER.SERVICE_FILE.PROTECTED_MATERIAL_FORBIDDEN",
                             "Lifecycle service files must not contain protected material.");
      }
      break;
    case DatabaseLifecycleThreatSurface::kResourceQuota:
      if (input.quota_requested && !input.quota_reservation_present) {
        return ThreatFailure(input,
                             "WORKLOAD_RESOURCE.EMPTY_RESERVATION_REFUSED",
                             "Routed work must hold an engine-owned quota reservation before work starts.");
      }
      if (input.quota_hard_limit_exceeded) {
        return ThreatFailure(input,
                             "WORKLOAD_RESOURCE.HARD_DENIED",
                             "Hard resource quota limits cannot be bypassed.");
      }
      if (input.quota_bypass_requested) {
        return ThreatFailure(input,
                             "WORKLOAD_RESOURCE.BYPASS_REFUSED",
                             "Parser, listener, manager, UDR, and plugin routes cannot bypass resource quota lifecycle.");
      }
      if (!input.quota_release_accounted) {
        return ThreatFailure(input,
                             "WORKLOAD_RESOURCE.RELEASE_REQUIRED",
                             "Quota reservations must be released exactly once on success, failure, cancellation, or shutdown.");
      }
      break;
  }

  return ThreatAccepted(input);
}

std::string SerializeDatabaseLifecycleThreatModelResultJson(
    const DatabaseLifecycleThreatModelResult& result) {
  std::ostringstream out;
  out << "{\"database_lifecycle_threat_model\":{\"accepted\":"
      << (result.accepted ? "true" : "false")
      << ",\"gate_label\":\"" << JsonEscape(result.gate_label)
      << "\",\"static_gate_label\":\"" << JsonEscape(result.static_gate_label)
      << "\",\"completion_gate_label\":\"" << JsonEscape(result.completion_gate_label)
      << "\",\"covered_surfaces\":[";
  for (std::size_t i = 0; i < result.covered_surfaces.size(); ++i) {
    if (i != 0) out << ',';
    out << "\"" << JsonEscape(result.covered_surfaces[i]) << "\"";
  }
  out << "],\"diagnostics\":[";
  for (std::size_t i = 0; i < result.diagnostics.size(); ++i) {
    if (i != 0) out << ',';
    out << "{\"code\":\"" << JsonEscape(result.diagnostics[i].code)
        << "\",\"message\":\"" << JsonEscape(result.diagnostics[i].safe_message)
        << "\"}";
  }
  out << "],\"evidence\":[";
  for (std::size_t i = 0; i < result.evidence.size(); ++i) {
    if (i != 0) out << ',';
    out << "\"" << JsonEscape(result.evidence[i]) << "\"";
  }
  out << "]}}";
  return out.str();
}

std::string SerializeConfigPolicySecurityLifecycleJson(
    const ConfigPolicySecurityLifecycle& lifecycle) {
  std::ostringstream out;
  out << "{\"config_policy_security_lifecycle\":{"
      << "\"descriptor_version\":" << lifecycle.descriptor_version << ","
      << "\"database_uuid\":\"" << JsonEscape(lifecycle.database_uuid) << "\","
      << "\"config_source\":\"" << JsonEscape(lifecycle.config_source) << "\","
      << "\"config_source_epoch\":" << lifecycle.config_source_epoch << ","
      << "\"config_reload_generation\":" << lifecycle.config_reload_generation << ","
      << "\"capability_policy_generation\":" << lifecycle.capability_policy_generation << ","
      << "\"policy_generation\":" << lifecycle.policy_generation << ","
      << "\"security_epoch\":" << lifecycle.security_epoch << ","
      << "\"provider_family\":\"" << JsonEscape(lifecycle.provider_family) << "\","
      << "\"provider_generation\":" << lifecycle.provider_generation << ","
      << "\"provider_state\":\""
      << SecurityProviderLifecycleStateName(lifecycle.provider_state) << "\","
      << "\"provider_plugin_loaded\":"
      << (lifecycle.provider_plugin_loaded ? "true" : "false") << ","
      << "\"provider_started\":" << (lifecycle.provider_started ? "true" : "false") << ","
      << "\"provider_healthy\":" << (lifecycle.provider_healthy ? "true" : "false") << ","
      << "\"default_policy_installed\":"
      << (lifecycle.default_policy_installed ? "true" : "false") << ","
      << "\"password_hash_verification_engine_owned\":"
      << (lifecycle.password_hash_verification_engine_owned ? "true" : "false") << ","
      << "\"cleartext_password_storage_allowed\":"
      << (lifecycle.cleartext_password_storage_allowed ? "true" : "false") << ","
      << "\"cache_invalidation_epoch\":" << lifecycle.cache_invalidation_epoch << ","
      << "\"invalidated_cache_targets\":[";
  for (std::size_t i = 0; i < lifecycle.invalidated_cache_targets.size(); ++i) {
    if (i != 0) out << ',';
    out << "\"" << JsonEscape(lifecycle.invalidated_cache_targets[i]) << "\"";
  }
  out << "]}}";
  return out.str();
}

}  // namespace scratchbird::server

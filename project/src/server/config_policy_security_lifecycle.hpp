// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// SEARCH_KEY: SB_SERVER_CONFIG_POLICY_SECURITY_LIFECYCLE

#pragma once

#include "config.hpp"
#include "diagnostics.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace scratchbird::server {

inline constexpr std::uint32_t kConfigPolicySecurityLifecycleDescriptorCurrent = 1;
inline constexpr std::uint32_t kConfigPolicySecurityLifecycleDescriptorMinSupported = 1;
inline constexpr std::uint32_t kConfigPolicySecurityLifecycleDescriptorMaxSupported = 1;

enum class SecurityProviderLifecycleState {
  kLoaded,
  kStarted,
  kHealthy,
  kDegraded,
  kQuiescing,
  kQuiesced,
  kDisabled,
  kQuarantined,
};

enum class DatabaseLifecycleThreatSurface {
  kForceShutdown,
  kIpcAuthentication,
  kIpcAuthorization,
  kManagerSupervision,
  kListenerSupervision,
  kParserSupervision,
  kUdrLoading,
  kHealthReporting,
  kBackupRestore,
  kSupportBundle,
  kServiceFiles,
  kResourceQuota,
};

struct ConfigPolicySecurityLifecycleInput {
  std::uint32_t descriptor_version = kConfigPolicySecurityLifecycleDescriptorCurrent;
  std::string database_path;
  std::string database_uuid;
  bool database_open = false;
  bool cluster_authority_required = false;
  std::string config_source = "compiled_defaults";
  std::uint64_t config_source_epoch = 1;
  std::uint64_t config_reload_generation = 1;
  std::uint64_t capability_policy_generation = 1;
  std::uint64_t policy_generation = 1;
  std::uint64_t security_epoch = 1;
  std::string provider_family = "local_password";
  std::uint64_t provider_generation = 1;
  SecurityProviderLifecycleState provider_state = SecurityProviderLifecycleState::kHealthy;
  bool default_policy_installed = true;
  std::uint64_t cache_invalidation_epoch = 1;
};

struct ConfigPolicySecurityLifecycle {
  std::uint32_t descriptor_version = kConfigPolicySecurityLifecycleDescriptorCurrent;
  std::string database_path;
  std::string database_uuid;
  std::string config_source;
  std::uint64_t config_source_epoch = 1;
  std::uint64_t config_reload_generation = 1;
  std::uint64_t capability_policy_generation = 1;
  std::uint64_t policy_generation = 1;
  std::uint64_t security_epoch = 1;
  std::string provider_family = "local_password";
  std::uint64_t provider_generation = 1;
  SecurityProviderLifecycleState provider_state = SecurityProviderLifecycleState::kHealthy;
  bool provider_plugin_loaded = true;
  bool provider_started = true;
  bool provider_healthy = true;
  bool default_policy_installed = true;
  bool password_hash_verification_engine_owned = true;
  bool cleartext_password_storage_allowed = false;
  bool cluster_authority_required = false;
  std::uint64_t cache_invalidation_epoch = 1;
  std::vector<std::string> invalidated_cache_targets;
  std::vector<std::string> provider_lifecycle_trace;
};

struct DatabaseLifecycleThreatModelInput {
  DatabaseLifecycleThreatSurface surface = DatabaseLifecycleThreatSurface::kIpcAuthentication;
  std::string operation_key;
  std::string authority_actor = "engine";
  std::string required_right;
  bool session_context_present = true;
  bool security_context_present = true;
  bool engine_authorization_granted = true;
  bool required_role_present = true;
  bool parser_listener_driver_claims_authority = false;
  bool process_association_exact = true;
  bool ambiguous_process_association = false;
  bool force_shutdown_requested = false;
  bool force_termination_policy_present = true;
  bool mga_recovery_evidence_preserved = true;
  bool active_transaction_finality_preserved = true;
  bool supervisor_heartbeat_current = true;
  bool supervisor_restart_policy_present = true;
  bool ambiguous_supervision_owner = false;
  bool udr_or_plugin_load_requested = false;
  bool package_policy_present = true;
  bool trusted_package_signature_present = true;
  bool parser_package_authorized = true;
  bool health_redaction_applied = true;
  bool protected_material_requested = false;
  bool backup_restore_policy_present = true;
  bool backup_engine_owned_path = true;
  bool restore_inspection_open = true;
  bool support_bundle_policy_present = true;
  bool support_bundle_redaction_applied = true;
  bool supportability_flush_succeeded = true;
  bool service_file_private_mode = true;
  bool service_file_owner_verified = true;
  bool service_file_protected_material_absent = true;
  bool quota_requested = false;
  bool quota_reservation_present = true;
  bool quota_hard_limit_exceeded = false;
  bool quota_bypass_requested = false;
  bool quota_release_accounted = true;
};

struct DatabaseLifecycleThreatModelResult {
  bool accepted = false;
  std::string gate_label = "database_lifecycle_threat_model";
  std::string static_gate_label = "DBLC_STATIC_THREAT_MODEL_ABUSE_CASES";
  std::string completion_gate_label = "DBLC_P13T_THREAT_MODEL_COMPLETE";
  std::vector<std::string> covered_surfaces;
  std::vector<std::string> evidence;
  std::vector<ServerDiagnostic> diagnostics;

  bool ok() const { return accepted && diagnostics.empty(); }
};

struct ConfigPolicySecurityLifecycleResult {
  bool accepted = false;
  ConfigPolicySecurityLifecycle lifecycle;
  ServerDiagnostic diagnostic;
  std::vector<ServerDiagnostic> diagnostics;

  bool ok() const { return accepted && diagnostics.empty(); }
};

const char* SecurityProviderLifecycleStateName(SecurityProviderLifecycleState state);
const char* DatabaseLifecycleThreatSurfaceName(DatabaseLifecycleThreatSurface surface);
SecurityProviderLifecycleState ParseSecurityProviderLifecycleState(const std::string& value);
ConfigPolicySecurityLifecycleInput BuildConfigPolicySecurityLifecycleInput(
    const ServerBootstrapConfig& config,
    std::string database_path,
    std::string database_uuid,
    bool database_open,
    bool cluster_authority_required);
ConfigPolicySecurityLifecycleResult StartConfigPolicySecurityLifecycle(
    const ConfigPolicySecurityLifecycleInput& input);
ConfigPolicySecurityLifecycleResult ReloadConfigPolicySecurityLifecycle(
    const ConfigPolicySecurityLifecycle& current,
    std::uint64_t new_config_source_epoch,
    std::uint64_t new_capability_policy_generation,
    std::uint64_t new_policy_generation,
    std::uint64_t new_security_epoch);
ConfigPolicySecurityLifecycleResult ValidateConfigPolicySecurityAdmission(
    const ConfigPolicySecurityLifecycle& lifecycle,
    std::uint64_t observed_capability_policy_generation,
    std::uint64_t observed_policy_generation,
    std::uint64_t observed_security_epoch,
    std::uint64_t observed_provider_generation,
    const std::string& provider_family,
    const std::string& authority_actor);
DatabaseLifecycleThreatModelResult EvaluateDatabaseLifecycleThreatModelGate(
    const DatabaseLifecycleThreatModelInput& input);
std::string SerializeDatabaseLifecycleThreatModelResultJson(
    const DatabaseLifecycleThreatModelResult& result);
std::string SerializeConfigPolicySecurityLifecycleJson(
    const ConfigPolicySecurityLifecycle& lifecycle);

}  // namespace scratchbird::server

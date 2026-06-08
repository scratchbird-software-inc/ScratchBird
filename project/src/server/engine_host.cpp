// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// SEARCH_KEY: SB_SERVER_ENGINE_HOST_DATABASE_SUPERVISION

#include "engine_host.hpp"

#include "config_policy_security_lifecycle.hpp"
#include "database_ownership.hpp"
#include "database_lifecycle.hpp"
#include "scratchbird/engine/engine.h"
#include "uuid.hpp"

#include <chrono>
#include <filesystem>
#include <sstream>

namespace scratchbird::server {

namespace {

std::string JsonEscape(const std::string& value) {
  return EscapeMessageVectorText(value);
}

ServerDiagnostic EngineHostDiagnostic(std::string code,
                                      std::string key,
                                      std::string message,
                                      const std::string& database_path) {
  return ServerDiagnostic{std::move(code),
                          std::move(key),
                          ServerDiagnosticSeverity::kError,
                          std::move(message),
                          {{"database_path", database_path}}};
}

sb_engine_open_mode_t OpenModeForConfig(const ServerBootstrapConfig& config) {
  if (config.mode == ServerMode::kMaintenance || config.database_open_mode == "maintenance") {
    return SB_ENGINE_OPEN_MAINTENANCE;
  }
  if (config.mode == ServerMode::kReadOnly || config.database_open_mode == "read_only") {
    return SB_ENGINE_OPEN_READ_ONLY;
  }
  if (config.database_open_mode == "restricted" || config.database_open_mode == "restricted_open") {
    return SB_ENGINE_OPEN_MAINTENANCE;
  }
  return SB_ENGINE_OPEN_NORMAL;
}

HostedDatabaseState StateForConfig(const ServerBootstrapConfig& config) {
  if (config.database_open_mode == "restricted" || config.database_open_mode == "restricted_open") {
    return HostedDatabaseState::kRestrictedOpen;
  }
  if (config.mode == ServerMode::kMaintenance || config.database_open_mode == "maintenance") {
    return HostedDatabaseState::kMaintenance;
  }
  if (config.mode == ServerMode::kReadOnly || config.database_open_mode == "read_only") {
    return HostedDatabaseState::kReadOnly;
  }
  return HostedDatabaseState::kOpen;
}

std::uint64_t CurrentUnixMillis() {
  const auto now = std::chrono::system_clock::now().time_since_epoch();
  return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(now).count());
}

}  // namespace

const char* HostedDatabaseStateName(HostedDatabaseState state) {
  switch (state) {
    case HostedDatabaseState::kNotConfigured:
      return "not_configured";
    case HostedDatabaseState::kOpening:
      return "opening";
    case HostedDatabaseState::kOpen:
      return "open";
    case HostedDatabaseState::kReadOnly:
      return "read_only";
    case HostedDatabaseState::kRestrictedOpen:
      return "restricted_open";
    case HostedDatabaseState::kMaintenance:
      return "maintenance";
    case HostedDatabaseState::kFailed:
      return "failed";
    case HostedDatabaseState::kDetached:
      return "detached";
    case HostedDatabaseState::kQuarantined:
      return "quarantined";
  }
  return "failed";
}

HostedEngineResult StartHostedEngine(const ServerBootstrapConfig& config) {
  HostedEngineResult result;
  result.state.engine_context_active = true;

  HostedDatabaseSnapshot snapshot;
  snapshot.state = HostedDatabaseState::kOpening;
  snapshot.database_path = config.database_default_path.string();
  snapshot.lifecycle_mode = config.database_open_mode;

  if (snapshot.database_path.empty()) {
    snapshot.state = HostedDatabaseState::kNotConfigured;
    result.state.databases.push_back(snapshot);
    return result;
  }

  std::shared_ptr<DatabaseOwnershipLock> database_ownership_lock;
  if (!config.database_ownership_prelocked) {
    DatabaseOwnershipRequest ownership_request;
    ownership_request.database_path = config.database_default_path;
    ownership_request.owner_kind = config.database_ownership_owner_kind.empty()
                                       ? "server"
                                       : config.database_ownership_owner_kind;
    ownership_request.sbps_endpoint = config.sbps_endpoint;
    auto ownership = AcquireDatabaseOwnership(ownership_request);
    if (!ownership.acquired) {
      snapshot.state = HostedDatabaseState::kFailed;
      snapshot.diagnostic_code =
          ownership.diagnostic_code.empty() ? "ARCH.DATABASE_MULTI_OWNER" : ownership.diagnostic_code;
      snapshot.diagnostic_message_key = "arch.database_multi_owner";
      result.diagnostics.push_back(ServerDiagnostic{
          snapshot.diagnostic_code,
          snapshot.diagnostic_code,
          ServerDiagnosticSeverity::kError,
          "The configured database is already owned by another engine instance.",
          {{"database_path", snapshot.database_path},
           {"lock_path", ownership.lock_path.string()},
           {"owner_kind", ownership.incumbent.owner_kind},
           {"owner_pid", ownership.incumbent.pid},
           {"owner_sbps_endpoint", ownership.incumbent.sbps_endpoint},
           {"detail", ownership.diagnostic_detail}}});
      result.state.databases.push_back(snapshot);
      return result;
    }
    database_ownership_lock = std::move(ownership.lock);
  }

  bool exists = std::filesystem::exists(config.database_default_path);
  if (!exists && !config.database_auto_create) {
    snapshot.state = HostedDatabaseState::kFailed;
    snapshot.diagnostic_code = "SERVER.ENGINE_HOST.DATABASE_NOT_FOUND";
    snapshot.diagnostic_message_key = "server.engine_host.database_not_found";
    result.diagnostics.push_back(EngineHostDiagnostic(snapshot.diagnostic_code,
                                                      snapshot.diagnostic_message_key,
                                                      "The configured database does not exist and auto-create is disabled.",
                                                      snapshot.database_path));
    result.state.databases.push_back(snapshot);
    return result;
  }

  bool created_database = false;
  if (!exists && config.database_auto_create) {
    namespace db = scratchbird::storage::database;
    namespace uuid = scratchbird::core::uuid;
    using scratchbird::core::platform::UuidKind;

    const auto now = CurrentUnixMillis();
    const auto database_uuid = uuid::GenerateEngineIdentityV7(UuidKind::database, now);
    const auto filespace_uuid = uuid::GenerateEngineIdentityV7(UuidKind::filespace, now + 1);
    if (!database_uuid.ok() || !filespace_uuid.ok()) {
      snapshot.state = HostedDatabaseState::kFailed;
      snapshot.diagnostic_code = "SERVER.ENGINE_HOST.DATABASE_CREATE_UUID_FAILED";
      snapshot.diagnostic_message_key = "server.engine_host.database_create_uuid_failed";
      result.diagnostics.push_back(EngineHostDiagnostic(snapshot.diagnostic_code,
                                                        snapshot.diagnostic_message_key,
                                                        "The configured database could not allocate durable bootstrap UUIDs.",
                                                        snapshot.database_path));
      result.state.databases.push_back(snapshot);
      return result;
    }

    db::DatabaseCreateConfig create;
    create.path = snapshot.database_path;
    create.database_uuid = database_uuid.value;
    create.filespace_uuid = filespace_uuid.value;
    create.page_size = 16384;
    create.creation_unix_epoch_millis = now;
    create.allow_minimal_resource_bootstrap = true;
    create.require_resource_seed_pack = false;
    create.allow_overwrite = false;
    const auto created = db::CreateDatabaseFile(create);
    if (!created.ok()) {
      snapshot.state = HostedDatabaseState::kFailed;
      snapshot.diagnostic_code = created.diagnostic.diagnostic_code.empty()
          ? "SERVER.ENGINE_HOST.DATABASE_CREATE_FAILED"
          : created.diagnostic.diagnostic_code;
      snapshot.diagnostic_message_key = created.diagnostic.message_key.empty()
          ? "server.engine_host.database_create_failed"
          : created.diagnostic.message_key;
      result.diagnostics.push_back(EngineHostDiagnostic(snapshot.diagnostic_code,
                                                        snapshot.diagnostic_message_key,
                                                        "The configured database could not be created by storage lifecycle.",
                                                        snapshot.database_path));
      result.state.databases.push_back(snapshot);
      return result;
    }
    exists = true;
    created_database = true;
  }

  namespace db = scratchbird::storage::database;
  db::DatabaseOpenConfig lifecycle_open_config;
  lifecycle_open_config.path = snapshot.database_path;
  lifecycle_open_config.cluster_authority_available = false;
  lifecycle_open_config.decryption_available = false;
  lifecycle_open_config.read_only = OpenModeForConfig(config) != SB_ENGINE_OPEN_NORMAL;
  lifecycle_open_config.suppress_background_agents = config.embedded_direct_mode;
  const auto lifecycle_open = db::OpenDatabaseFile(lifecycle_open_config);
  if (!lifecycle_open.ok()) {
    snapshot.state = HostedDatabaseState::kFailed;
    snapshot.diagnostic_code = lifecycle_open.diagnostic.diagnostic_code.empty()
        ? "SERVER.ENGINE_HOST.DATABASE_LIFECYCLE_OPEN_FAILED"
        : lifecycle_open.diagnostic.diagnostic_code;
    snapshot.diagnostic_message_key = lifecycle_open.diagnostic.message_key.empty()
        ? "server.engine_host.database_lifecycle_open_failed"
        : lifecycle_open.diagnostic.message_key;
    result.diagnostics.push_back(EngineHostDiagnostic(
        snapshot.diagnostic_code,
        snapshot.diagnostic_message_key,
        "The configured database could not enter the storage lifecycle open path.",
        snapshot.database_path));
    result.state.databases.push_back(snapshot);
    return result;
  }

  sb_engine_open_params_v1_t open_params{};
  open_params.struct_size = sizeof(open_params);
  open_params.abi_version = SB_ENGINE_ABI_VERSION_PACKED;
  open_params.database_path_utf8 = snapshot.database_path.data();
  open_params.database_path_size = static_cast<std::uint64_t>(snapshot.database_path.size());
  open_params.mode = OpenModeForConfig(config);

  sb_engine_handle_t engine = nullptr;
  sb_engine_result_t open_result = nullptr;
  const auto status = sb_engine_open(&open_params, &engine, &open_result);
  if (open_result != nullptr) {
    (void)sb_engine_result_release(open_result);
  }
  if (status != SB_ENGINE_STATUS_OK) {
    snapshot.state = HostedDatabaseState::kFailed;
    snapshot.diagnostic_code = "SERVER.ENGINE_HOST.OPEN_FAILED";
    snapshot.diagnostic_message_key = "server.engine_host.open_failed";
    result.diagnostics.push_back(EngineHostDiagnostic(snapshot.diagnostic_code,
                                                      snapshot.diagnostic_message_key,
                                                      "The configured database could not be opened by the hosted engine public ABI.",
                                                      snapshot.database_path));
    result.state.databases.push_back(snapshot);
    return result;
  }
  snapshot.database_created = created_database;
  snapshot.database_open = true;
  snapshot.database_uuid =
      scratchbird::core::uuid::UuidToString(lifecycle_open.state.database_uuid.value);
  snapshot.filespace_uuid =
      scratchbird::core::uuid::UuidToString(lifecycle_open.state.filespace_uuid.value);
  snapshot.state = StateForConfig(config);
  snapshot.startup_recovery_classification =
      lifecycle_open.state.startup_recovery_classification.empty()
          ? "public_abi_hosted_open"
          : lifecycle_open.state.startup_recovery_classification;
  if (lifecycle_open.state.engine_agent_health_present) {
    snapshot.database_engine_agent_instance_uuid =
        lifecycle_open.state.engine_agent_health.engine_instance_uuid;
    snapshot.selected_agent_type_ids =
        lifecycle_open.state.engine_agent_health.selected_agent_type_ids;
    snapshot.database_engine_agent_state =
        scratchbird::core::agents::DatabaseEngineAgentLifecycleStateName(
            lifecycle_open.state.engine_agent_health.agent_state);
    snapshot.database_engine_agent_health_generation =
        lifecycle_open.state.engine_agent_health.health_generation;
    snapshot.database_engine_agent_ordinary_admission_allowed =
        lifecycle_open.state.engine_agent_health.ordinary_admission_allowed;
    snapshot.database_engine_agent_health_json =
        lifecycle_open.state.engine_agent_health_json;
  }
  const auto config_policy_security = StartConfigPolicySecurityLifecycle(
      BuildConfigPolicySecurityLifecycleInput(config,
                                              snapshot.database_path,
                                              snapshot.database_uuid,
                                              snapshot.database_open,
                                              snapshot.cluster_authority_required));
  if (!config_policy_security.ok()) {
    snapshot.state = HostedDatabaseState::kFailed;
    snapshot.diagnostic_code = config_policy_security.diagnostic.code.empty()
        ? "SERVER.ENGINE_HOST.CONFIG_POLICY_SECURITY_FAILED"
        : config_policy_security.diagnostic.code;
    snapshot.diagnostic_message_key = config_policy_security.diagnostic.message_key.empty()
        ? "server.engine_host.config_policy_security_failed"
        : config_policy_security.diagnostic.message_key;
    result.diagnostics.push_back(config_policy_security.diagnostic);
    result.state.databases.push_back(snapshot);
    return result;
  }
  snapshot.config_policy_security_lifecycle_present = true;
  snapshot.config_source_epoch = config_policy_security.lifecycle.config_source_epoch;
  snapshot.config_reload_generation = config_policy_security.lifecycle.config_reload_generation;
  snapshot.capability_policy_generation =
      config_policy_security.lifecycle.capability_policy_generation;
  snapshot.policy_generation = config_policy_security.lifecycle.policy_generation;
  snapshot.security_epoch = config_policy_security.lifecycle.security_epoch;
  snapshot.security_provider_family = config_policy_security.lifecycle.provider_family;
  snapshot.security_provider_generation = config_policy_security.lifecycle.provider_generation;
  snapshot.security_provider_state =
      SecurityProviderLifecycleStateName(config_policy_security.lifecycle.provider_state);
  snapshot.default_policy_installed = config_policy_security.lifecycle.default_policy_installed;
  snapshot.cache_invalidation_epoch = config_policy_security.lifecycle.cache_invalidation_epoch;
  snapshot.config_policy_security_lifecycle_json =
      SerializeConfigPolicySecurityLifecycleJson(config_policy_security.lifecycle);
  if (snapshot.state == HostedDatabaseState::kReadOnly ||
      snapshot.state == HostedDatabaseState::kRestrictedOpen ||
      snapshot.state == HostedDatabaseState::kMaintenance) {
    snapshot.read_only = true;
    snapshot.write_admission_fenced = true;
  } else {
    snapshot.write_admission_fenced = false;
  }
  if (engine != nullptr) {
    (void)sb_engine_close(engine, nullptr);
  }
  if (database_ownership_lock) {
    result.state.database_ownership_locks.push_back(std::move(database_ownership_lock));
  }
  result.state.databases.push_back(snapshot);
  return result;
}

std::string HostedEngineStatusJson(const HostedEngineState& state) {
  std::ostringstream out;
  out << "{\"engine_host\":{\"active\":" << (state.engine_context_active ? "true" : "false")
      << ",\"databases\":[";
  for (std::size_t i = 0; i < state.databases.size(); ++i) {
    const auto& database = state.databases[i];
    if (i != 0) out << ',';
    out << "{\"state\":\"" << HostedDatabaseStateName(database.state) << "\","
        << "\"database_path\":\"" << JsonEscape(database.database_path) << "\","
        << "\"database_uuid\":\"" << JsonEscape(database.database_uuid) << "\","
        << "\"filespace_uuid\":\"" << JsonEscape(database.filespace_uuid) << "\","
        << "\"database_created\":" << (database.database_created ? "true" : "false") << ","
        << "\"database_open\":" << (database.database_open ? "true" : "false") << ","
        << "\"read_only\":" << (database.read_only ? "true" : "false") << ","
        << "\"write_admission_fenced\":" << (database.write_admission_fenced ? "true" : "false") << ","
        << "\"cluster_authority_required\":"
        << (database.cluster_authority_required ? "true" : "false") << ","
        << "\"database_engine_agent_state\":\""
        << JsonEscape(database.database_engine_agent_state) << "\","
        << "\"database_engine_agent_instance_uuid\":\""
        << JsonEscape(database.database_engine_agent_instance_uuid) << "\","
        << "\"database_engine_agent_health_generation\":"
        << database.database_engine_agent_health_generation << ","
        << "\"database_engine_agent_ordinary_admission_allowed\":"
        << (database.database_engine_agent_ordinary_admission_allowed ? "true" : "false") << ","
        << "\"database_engine_agent_health\":"
        << (database.database_engine_agent_health_json.empty()
                ? "{\"database_engine_agent\":{\"agent_state\":\"not_started\"}}"
                : database.database_engine_agent_health_json)
        << ","
        << "\"config_policy_security_lifecycle\":"
        << (database.config_policy_security_lifecycle_json.empty()
                ? "{\"config_policy_security_lifecycle\":{\"present\":false}}"
                : database.config_policy_security_lifecycle_json)
        << ","
        << "\"startup_recovery_classification\":\""
        << JsonEscape(database.startup_recovery_classification) << "\","
        << "\"diagnostic_code\":\"" << JsonEscape(database.diagnostic_code) << "\"}";
  }
  out << "]}}\n";
  return out.str();
}

}  // namespace scratchbird::server

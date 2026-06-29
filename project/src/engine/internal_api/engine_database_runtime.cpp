// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "engine_database_runtime.hpp"

#include <string>
#include <utility>
#include <vector>

namespace scratchbird::engine::internal_api {
namespace {

using scratchbird::core::platform::DiagnosticArgument;
using scratchbird::core::platform::MakeDiagnostic;
using scratchbird::core::platform::Severity;
using scratchbird::core::platform::StatusCode;
using scratchbird::core::platform::Subsystem;
using scratchbird::storage::database::DatabaseLifecyclePhase;

Status EngineRuntimeOkStatus() {
  return {StatusCode::ok, Severity::info, Subsystem::engine};
}

Status EngineRuntimeErrorStatus() {
  return {StatusCode::platform_required_feature_missing, Severity::error, Subsystem::engine};
}

EngineDatabaseRuntimeStateResult RuntimeError(std::string diagnostic_code,
                                             std::string message_key,
                                             std::string detail = {}) {
  EngineDatabaseRuntimeStateResult result;
  result.status = EngineRuntimeErrorStatus();
  result.diagnostic = MakeEngineDatabaseRuntimeDiagnostic(result.status,
                                                         std::move(diagnostic_code),
                                                         std::move(message_key),
                                                         std::move(detail));
  return result;
}

AgentCatalogRuntimeSchemaOpenMode RuntimeSchemaOpenMode(
    const DatabaseLifecycleState& database,
    const EngineDatabaseRuntimeSchemaAdmissionOptions& options) {
  if (options.open_mode_override_present) { return options.open_mode; }
  if (database.read_only_open) {
    return AgentCatalogRuntimeSchemaOpenMode::read_only;
  }
  return AgentCatalogRuntimeSchemaOpenMode::read_write;
}

AgentCatalogRuntimeSchemaValidationRequest RuntimeSchemaValidationRequest(
    const DatabaseLifecycleState& database,
    const EngineDatabaseRuntimeSchemaAdmissionOptions& options) {
  AgentCatalogRuntimeSchemaValidationRequest request;
  request.open_mode = RuntimeSchemaOpenMode(database, options);
  request.fresh_install = database.phase == DatabaseLifecyclePhase::created;
  request.migration_requested = options.migration_requested;
  if (!request.fresh_install) {
    request.observed_surfaces = options.observed_schema_surfaces_present
                                    ? options.observed_schema_surfaces
                                    : CurrentAgentCatalogRuntimeSchemaObservations();
  }
  return request;
}

EngineDatabaseRuntimeStateResult RuntimeSchemaError(
    EngineDatabaseRuntimeState state,
    const AgentCatalogRuntimeSchemaValidationResult& validation) {
  EngineDatabaseRuntimeStateResult result;
  result.status = EngineRuntimeErrorStatus();
  state.agent_catalog_schema_validation = validation;
  state.agent_catalog_schema_validated = true;
  state.database_open = false;
  result.state = std::move(state);
  result.diagnostic = MakeEngineDatabaseRuntimeDiagnostic(
      result.status,
      validation.diagnostic_code,
      "engine.database_runtime.agent_catalog_schema_validation_failed",
      validation.diagnostic_detail);
  return result;
}

std::string RouteModeName(EngineDatabaseRuntimeRouteMode mode) {
  switch (mode) {
    case EngineDatabaseRuntimeRouteMode::embedded_direct: return "embedded_direct";
    case EngineDatabaseRuntimeRouteMode::local_ipc: return "local_ipc";
    case EngineDatabaseRuntimeRouteMode::server_inet: return "server_inet";
  }
  return "server_inet";
}

void AddRouteEvidence(EngineDatabaseRuntimeState* state,
                      std::string key,
                      std::string value) {
  state->route_evidence.push_back({std::move(key), std::move(value)});
}

bool RuntimeDatabasePhaseAdmissible(DatabaseLifecyclePhase phase) {
  return phase == DatabaseLifecyclePhase::opened ||
         phase == DatabaseLifecyclePhase::created ||
         phase == DatabaseLifecyclePhase::maintenance ||
         phase == DatabaseLifecyclePhase::restricted_open ||
         phase == DatabaseLifecyclePhase::inspected ||
         phase == DatabaseLifecyclePhase::verified ||
         phase == DatabaseLifecyclePhase::repaired;
}

void SetOpenState(EngineDatabaseRuntimeState* state,
                  std::string mode,
                  std::string diagnostic_code,
                  std::string detail,
                  bool mutation_allowed,
                  bool drain_allowed,
                  bool safe_maintenance_allowed) {
  state->open_state_mode = std::move(mode);
  state->open_state_diagnostic_code = std::move(diagnostic_code);
  state->open_state_diagnostic_detail = std::move(detail);
  state->agent_inspect_allowed = true;
  state->agent_mutation_allowed = mutation_allowed;
  state->agent_drain_allowed = drain_allowed;
  state->safe_maintenance_allowed = safe_maintenance_allowed;
  state->normal_background_action_loops_allowed = mutation_allowed;
  if (!mutation_allowed) {
    state->background_agents_launch_allowed = false;
  }
  AddRouteEvidence(state, "agent_open_state_mode", state->open_state_mode);
  AddRouteEvidence(state, "agent_open_state_diagnostic", state->open_state_diagnostic_code);
  AddRouteEvidence(state,
                   "agent_mutation_allowed",
                   state->agent_mutation_allowed ? "true" : "false");
  AddRouteEvidence(state,
                   "normal_background_action_loops",
                   state->normal_background_action_loops_allowed ? "allowed" : "suppressed");
}

// SEARCH_KEY: ApplyAgentOpenStateAdmission
void ApplyAgentOpenStateAdmission(
    EngineDatabaseRuntimeState* state,
    const EngineDatabaseRuntimeRouteAdmissionOptions& options) {
  if (options.shutdown_in_progress) {
    SetOpenState(state,
                 "shutdown_in_progress",
                 "SB_ENGINE_OPEN_STATE_SHUTDOWN_DRAIN_ONLY",
                 "shutdown_drains_agents_and_refuses_new_mutation",
                 false,
                 true,
                 false);
    return;
  }
  if (state->database.read_only_open) {
    SetOpenState(state,
                 "read_only",
                 "SB_ENGINE_OPEN_STATE_READ_ONLY_INSPECT_ONLY",
                 "read_only_agent_mutation_refused",
                 false,
                 false,
                 false);
    return;
  }
  if (state->database.phase == DatabaseLifecyclePhase::restricted_open) {
    SetOpenState(state,
                 "restricted_open",
                 "SB_ENGINE_OPEN_STATE_RESTRICTED_SAFE_ONLY",
                 "restricted_open_allows_inspect_and_safe_maintenance",
                 false,
                 false,
                 true);
    return;
  }
  if (state->database.phase == DatabaseLifecyclePhase::maintenance) {
    SetOpenState(state,
                 "maintenance",
                 "SB_ENGINE_OPEN_STATE_MAINTENANCE_SAFE_ONLY",
                 "maintenance_allows_safe_engine_owned_maintenance",
                 false,
                 false,
                 true);
    return;
  }
  if (options.repair_mode_active || state->database.phase == DatabaseLifecyclePhase::repaired) {
    SetOpenState(state,
                 "repair",
                 "SB_ENGINE_OPEN_STATE_REPAIR_SAFE_ONLY",
                 "repair_mode_allows_inspect_and_safe_repair",
                 false,
                 false,
                 true);
    return;
  }
  if (options.backup_hold_active) {
    SetOpenState(state,
                 "backup_hold",
                 "SB_ENGINE_OPEN_STATE_BACKUP_HOLD_INSPECT_ONLY",
                 "backup_hold_refuses_agent_mutation",
                 false,
                 false,
                 false);
    return;
  }
  if (options.archive_hold_active) {
    SetOpenState(state,
                 "archive_hold",
                 "SB_ENGINE_OPEN_STATE_ARCHIVE_HOLD_INSPECT_ONLY",
                 "archive_hold_refuses_agent_mutation",
                 false,
                 false,
                 false);
    return;
  }
  SetOpenState(state,
               "read_write",
               "SB_ENGINE_OPEN_STATE_READ_WRITE",
               "normal_mutation_allowed",
               true,
               false,
               false);
}

void ApplyRuntimeRouteAdmission(EngineDatabaseRuntimeState* state,
                                const EngineDatabaseRuntimeRouteAdmissionOptions& options) {
  state->route_mode = RouteModeName(options.route_mode);
  state->route_diagnostic_code = "SB_ENGINE_RUNTIME_ROUTE_ADMITTED";
  state->route_diagnostic_detail = state->route_mode;
  state->database_file_lock_owned = options.database_file_lock_owned;
  state->database_file_locked_by_server = options.database_file_locked_by_server;
  state->explicit_maintenance_engine_owned = true;

  state->embedded_direct = options.route_mode == EngineDatabaseRuntimeRouteMode::embedded_direct;
  state->local_ipc_route = options.route_mode == EngineDatabaseRuntimeRouteMode::local_ipc;
  state->server_inet_route = options.route_mode == EngineDatabaseRuntimeRouteMode::server_inet;

  if (state->embedded_direct) {
    state->embedded_sysarch_bypass_active = true;
    state->embedded_single_user_session = true;
    state->embedded_always_in_transaction = true;
    state->background_agents_suppressed = true;
    state->listener_suppressed = true;
    state->ipc_server_suppressed = true;
    state->manager_suppressed = true;
    state->background_agents_launch_allowed = false;
    state->explicit_maintenance_synchronous = options.explicit_maintenance_requested;
    AddRouteEvidence(state, "route_mode", "embedded_direct");
    AddRouteEvidence(state, "sysarch_bypass_scope", "direct_managed_file_only");
    AddRouteEvidence(state, "background_agents", "suppressed");
    AddRouteEvidence(state, "listener", "suppressed");
    AddRouteEvidence(state, "ipc_server", "suppressed");
    AddRouteEvidence(state, "manager", "suppressed");
    AddRouteEvidence(state, "session_model", "single_user_single_session");
    AddRouteEvidence(state, "transaction_model", "always_in_transaction_mga_engine_owned");
    AddRouteEvidence(state,
                     "explicit_maintenance",
                     options.explicit_maintenance_requested
                         ? "synchronous_engine_api"
                         : "operator_required");
    if (!options.maintenance_action.empty()) {
      AddRouteEvidence(state, "maintenance_action", options.maintenance_action);
    }
    return;
  }

  state->embedded_sysarch_bypass_active = false;
  state->embedded_single_user_session = false;
  state->embedded_always_in_transaction = false;
  state->background_agents_suppressed = false;
  state->listener_suppressed = false;
  state->ipc_server_suppressed = false;
  state->manager_suppressed = false;
  state->background_agents_launch_allowed = true;
  state->explicit_maintenance_synchronous = false;
  AddRouteEvidence(state, "route_mode", state->route_mode);
  AddRouteEvidence(state, "sysarch_bypass_scope", "none");
  AddRouteEvidence(state, "security_authority", "server_or_ipc_protocol");
  AddRouteEvidence(state, "background_agents", "server_authoritative");
}

EngineDatabaseRuntimeStateResult RuntimeRouteError(
    EngineDatabaseRuntimeState state,
    std::string diagnostic_code,
    std::string message_key,
    std::string detail) {
  EngineDatabaseRuntimeStateResult result;
  result.status = EngineRuntimeErrorStatus();
  state.route_diagnostic_code = diagnostic_code;
  state.route_diagnostic_detail = detail;
  state.database_open = false;
  result.state = std::move(state);
  result.diagnostic = MakeEngineDatabaseRuntimeDiagnostic(result.status,
                                                         std::move(diagnostic_code),
                                                         std::move(message_key),
                                                         std::move(detail));
  return result;
}

EngineOperationResult OperationError(EngineOperationCode operation_code,
                                     std::string diagnostic_code,
                                     std::string message_key,
                                     std::string detail = {}) {
  EngineOperationResult result;
  result.status = EngineRuntimeErrorStatus();
  result.operation_code = operation_code;
  result.diagnostic = MakeEngineDatabaseRuntimeDiagnostic(result.status,
                                                         std::move(diagnostic_code),
                                                         std::move(message_key),
                                                         std::move(detail));
  result.diagnostics.push_back(result.diagnostic);
  return result;
}

}  // namespace

EngineDatabaseRuntimeStateResult MakeEngineDatabaseRuntimeState(DatabaseLifecycleState database,
                                                               EngineVersionInfo version,
                                                               scratchbird::core::resources::ResourceSeedCatalogImage resources,
                                                               EngineDatabaseRuntimeSchemaAdmissionOptions schema_options,
                                                               EngineDatabaseRuntimeRouteAdmissionOptions route_options) {
  if (!RuntimeDatabasePhaseAdmissible(database.phase)) {
    return RuntimeError("SB-ENGINE-RUNTIME-DATABASE-NOT-OPEN",
                        "engine.database_runtime.database_not_open");
  }
  if (!database.database_uuid.valid()) {
    return RuntimeError("SB-ENGINE-RUNTIME-DATABASE-UUID-INVALID",
                        "engine.database_runtime.database_uuid_invalid");
  }

  EngineDatabaseRuntimeStateResult result;
  result.status = EngineRuntimeOkStatus();
  result.state.database = std::move(database);
  result.state.version = std::move(version);
  ApplyRuntimeRouteAdmission(&result.state, route_options);
  ApplyAgentOpenStateAdmission(&result.state, route_options);
  if (result.state.embedded_direct && route_options.database_file_locked_by_server &&
      !route_options.database_file_lock_owned) {
    return RuntimeRouteError(std::move(result.state),
                             "SB_EMBEDDED_DIRECT_OWNER_SERVER_LOCKED",
                             "engine.database_runtime.embedded_direct_owner_server_locked",
                             "connect_to_owning_server_via_local_ipc");
  }
  if (result.state.embedded_direct && !route_options.database_file_lock_owned) {
    return RuntimeRouteError(std::move(result.state),
                             "SB_EMBEDDED_DIRECT_FILE_LOCK_REQUIRED",
                             "engine.database_runtime.embedded_direct_file_lock_required",
                             "exclusive_embedded_file_lock_required");
  }
  const auto schema_validation = ValidateAgentCatalogRuntimeSchema(
      RuntimeSchemaValidationRequest(result.state.database, schema_options));
  result.state.agent_catalog_schema_validation = schema_validation;
  result.state.agent_catalog_schema_validated = true;
  if (!schema_validation.ok) {
    return RuntimeSchemaError(std::move(result.state), schema_validation);
  }
  if (!resources.active && result.state.database.resource_seed_catalog_present) {
    resources = result.state.database.resource_seed_catalog;
  }
  result.state.resources = std::move(resources);
  result.state.database_open = true;
  result.state.resources_active = result.state.resources.active;
  return result;
}

EngineOperationResult ExecuteShowVersionRuntime(const EngineDatabaseRuntimeState& runtime,
                                                const EngineContext& context) {
  if (!runtime.database_open) {
    return OperationError(EngineOperationCode::show_version,
                          "SB-ENGINE-RUNTIME-SHOW-VERSION-DATABASE-NOT-OPEN",
                          "engine.database_runtime.show_version_database_not_open");
  }
  return ExecuteShowVersionOperation(context, runtime.version);
}

EngineOperationResult ExecuteShowDatabaseRuntime(const EngineDatabaseRuntimeState& runtime,
                                                 const EngineContext& context) {
  if (!runtime.database_open) {
    return OperationError(EngineOperationCode::show_database,
                          "SB-ENGINE-RUNTIME-SHOW-DATABASE-NOT-OPEN",
                          "engine.database_runtime.show_database_not_open");
  }

  EngineDatabaseInfo database;
  database.database_uuid = runtime.database.database_uuid;
  database.database_label = runtime.database.path.empty() ? "unnamed" : runtime.database.path;
  database.page_size_bytes = runtime.database.header.page_size;
  database.cluster_authority_active = runtime.database.cluster_authority_active;
  return ExecuteShowDatabaseOperation(context, database);
}

EngineOperationResult ExecuteShowDatabaseResourcesRuntime(const EngineDatabaseRuntimeState& runtime,
                                                         const EngineContext& context) {
  if (!runtime.database_open) {
    return OperationError(EngineOperationCode::show_database_resources,
                          "SB-ENGINE-RUNTIME-SHOW-DATABASE-RESOURCES-DATABASE-NOT-OPEN",
                          "engine.database_runtime.show_database_resources_database_not_open");
  }
  if (!runtime.resources_active) {
    return OperationError(EngineOperationCode::show_database_resources,
                          "SB_RESOURCE_SEED_MISSING",
                          "engine.database_runtime.resource_seed_missing");
  }
  return ExecuteShowDatabaseResourcesOperation(context, runtime.resources);
}

DiagnosticRecord MakeEngineDatabaseRuntimeDiagnostic(Status status,
                                                    std::string diagnostic_code,
                                                    std::string message_key,
                                                    std::string detail) {
  std::vector<DiagnosticArgument> arguments;
  if (!detail.empty()) {
    arguments.push_back({"detail", detail});
  }

  return MakeDiagnostic(status.code,
                        status.severity,
                        status.subsystem,
                        std::move(diagnostic_code),
                        std::move(message_key),
                        std::move(arguments),
                        {},
                        "engine.database.runtime");
}

}  // namespace scratchbird::engine::internal_api

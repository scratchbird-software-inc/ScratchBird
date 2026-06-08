// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// SB-ENGINE-DATABASE-RUNTIME-ANCHOR
#include "database_lifecycle.hpp"
#include "engine_builtin_operations.hpp"
#include "catalog/agent_catalog_runtime_schema_versioning.hpp"
#include "resource_seed_pack.hpp"
#include "runtime_platform.hpp"

#include <string>
#include <vector>

namespace scratchbird::engine::internal_api {

using scratchbird::core::platform::DiagnosticRecord;
using scratchbird::core::platform::Status;
using scratchbird::storage::database::DatabaseLifecycleState;

struct EngineDatabaseRuntimeState {
  DatabaseLifecycleState database;
  EngineVersionInfo version;
  scratchbird::core::resources::ResourceSeedCatalogImage resources;
  AgentCatalogRuntimeSchemaValidationResult agent_catalog_schema_validation;
  std::string route_mode = "server_inet";
  std::string route_diagnostic_code;
  std::string route_diagnostic_detail;
  std::vector<std::pair<std::string, std::string>> route_evidence;
  bool database_open = false;
  bool resources_active = false;
  bool agent_catalog_schema_validated = false;
  bool embedded_direct = false;
  bool local_ipc_route = false;
  bool server_inet_route = true;
  bool embedded_sysarch_bypass_active = false;
  bool embedded_single_user_session = false;
  bool embedded_always_in_transaction = false;
  bool database_file_lock_owned = false;
  bool database_file_locked_by_server = false;
  bool background_agents_suppressed = false;
  bool listener_suppressed = false;
  bool ipc_server_suppressed = false;
  bool manager_suppressed = false;
  bool background_agents_launch_allowed = true;
  bool explicit_maintenance_synchronous = false;
  bool explicit_maintenance_engine_owned = true;
  std::string open_state_mode = "read_write";
  std::string open_state_diagnostic_code = "SB_ENGINE_OPEN_STATE_READ_WRITE";
  std::string open_state_diagnostic_detail = "normal_mutation_allowed";
  bool agent_inspect_allowed = true;
  bool agent_mutation_allowed = true;
  bool agent_drain_allowed = false;
  bool safe_maintenance_allowed = false;
  bool normal_background_action_loops_allowed = true;
};

struct EngineDatabaseRuntimeStateResult {
  Status status;
  EngineDatabaseRuntimeState state;
  DiagnosticRecord diagnostic;

  bool ok() const {
    return status.ok();
  }
};

struct EngineDatabaseRuntimeSchemaAdmissionOptions {
  bool observed_schema_surfaces_present = false;
  std::vector<AgentCatalogRuntimeSchemaObservation> observed_schema_surfaces;
  bool migration_requested = false;
  bool open_mode_override_present = false;
  AgentCatalogRuntimeSchemaOpenMode open_mode =
      AgentCatalogRuntimeSchemaOpenMode::read_write;
};

enum class EngineDatabaseRuntimeRouteMode {
  server_inet,
  local_ipc,
  embedded_direct,
};

struct EngineDatabaseRuntimeRouteAdmissionOptions {
  EngineDatabaseRuntimeRouteMode route_mode = EngineDatabaseRuntimeRouteMode::server_inet;
  bool database_file_lock_owned = false;
  bool database_file_locked_by_server = false;
  bool explicit_maintenance_requested = false;
  std::string maintenance_action;
  bool shutdown_in_progress = false;
  bool repair_mode_active = false;
  bool backup_hold_active = false;
  bool archive_hold_active = false;
};

EngineDatabaseRuntimeStateResult MakeEngineDatabaseRuntimeState(DatabaseLifecycleState database,
                                                               EngineVersionInfo version = {},
                                                               scratchbird::core::resources::ResourceSeedCatalogImage resources = {},
                                                               EngineDatabaseRuntimeSchemaAdmissionOptions schema_options = {},
                                                               EngineDatabaseRuntimeRouteAdmissionOptions route_options = {});
EngineOperationResult ExecuteShowVersionRuntime(const EngineDatabaseRuntimeState& runtime,
                                                const EngineContext& context);
EngineOperationResult ExecuteShowDatabaseRuntime(const EngineDatabaseRuntimeState& runtime,
                                                 const EngineContext& context);
EngineOperationResult ExecuteShowDatabaseResourcesRuntime(const EngineDatabaseRuntimeState& runtime,
                                                         const EngineContext& context);
DiagnosticRecord MakeEngineDatabaseRuntimeDiagnostic(Status status,
                                                    std::string diagnostic_code,
                                                    std::string message_key,
                                                    std::string detail = {});

}  // namespace scratchbird::engine::internal_api

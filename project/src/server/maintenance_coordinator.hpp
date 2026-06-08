// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// SEARCH_KEY: SB_SERVER_MAINTENANCE_RELOAD_SHUTDOWN

#pragma once

#include "config.hpp"
#include "diagnostics.hpp"
#include "lifecycle.hpp"
#include "session_registry.hpp"

#include <array>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace scratchbird::server {

struct ServerMaintenanceCoordinator {
  std::uint64_t generation = 1;
  std::uint64_t reload_generation = 1;
  std::string state = "running";
  bool maintenance_mode = false;
  bool restricted_open_mode = false;
  bool draining = false;
  bool stopping = false;
  bool restart_pending = false;
  bool attach_admission_fenced = false;
  bool write_admission_fenced = false;
  bool sblr_admission_fenced = false;
  bool event_admission_fenced = false;
  bool backup_fence_active = false;
  bool restore_fence_active = false;
  bool shutdown_requested = false;
  bool database_shutdown_requested = false;
  bool shutdown_parser_fallback_used = false;
  std::uint64_t shutdown_generation = 0;
  std::uint64_t shutdown_notification_count = 0;
  std::uint64_t shutdown_required_ack_count = 0;
  std::uint64_t shutdown_acknowledged_count = 0;
  std::uint64_t shutdown_active_transaction_session_count = 0;
  std::string shutdown_mode;
  std::string shutdown_database_uuid;
  std::string database_uuid;
  std::string permitted_maintenance_operations;
  std::string last_operation;
  std::string last_outcome;
  std::string last_finality_token;
  std::map<std::string, ServerFinalityRecord> finality_by_token_uuid;
};

struct ServerMaintenanceOperationRequest {
  std::string operation_key;
  std::string target_uuid;
  std::string mode;
  std::string audit_reason;
  std::uint64_t timeout_ms = 30000;
  std::array<std::uint8_t, 16> request_uuid{};
  std::array<std::uint8_t, 16> session_uuid{};
};

struct ServerMaintenanceOperationResult {
  bool ok = true;
  bool request_shutdown = false;
  std::string outcome = "completed";
  std::string state_before;
  std::string state_after;
  std::string records_json = "[]";
  std::string finality_token_uuid;
  std::vector<ServerDiagnostic> diagnostics;
};

struct ServerShutdownRuntimeSnapshot {
  std::string database_path;
  std::string database_uuid;
  std::uint64_t associated_manager_count = 1;
  std::uint64_t associated_listener_count = 0;
  std::uint64_t associated_parser_count = 0;
  std::uint64_t associated_ipc_endpoint_count = 0;
  std::uint64_t associated_session_count = 0;
  std::uint64_t associated_client_count = 0;
  std::uint64_t active_transaction_session_count = 0;
  std::uint64_t required_acknowledgement_count = 0;
  std::uint64_t acknowledged_component_count = 0;
  bool association_scope_proven = false;
  bool listener_unavailable = false;
  bool parser_association_registry_available = false;
  bool parser_association_registry_stale = false;
  bool drain_complete = false;
  std::string association_diagnostic_code;
  std::string association_diagnostic_detail;
};

ServerMaintenanceCoordinator BuildMaintenanceCoordinator(const ServerBootstrapConfig& config,
                                                         const ServerLifecycleArtifacts& artifacts);

bool MaintenanceAllowsAttach(const ServerMaintenanceCoordinator& coordinator);
bool MaintenanceAllowsSblr(const ServerMaintenanceCoordinator& coordinator);
bool MaintenanceAllowsEvents(const ServerMaintenanceCoordinator& coordinator);

ServerDiagnostic MaintenanceAdmissionDiagnostic(const ServerMaintenanceCoordinator& coordinator,
                                                std::string operation_key,
                                                std::string reason);

std::string MaintenanceCoordinatorRecordsJson(const ServerMaintenanceCoordinator& coordinator);
std::string MaintenanceFinalityRecordsJson(const ServerMaintenanceCoordinator& coordinator);

ServerMaintenanceOperationResult ApplyServerMaintenanceOperation(
    ServerMaintenanceCoordinator* coordinator,
    const ServerBootstrapConfig& config,
    const ServerMaintenanceOperationRequest& request);

ServerMaintenanceOperationResult ApplyDatabaseShutdownOperation(
    ServerMaintenanceCoordinator* coordinator,
    const ServerBootstrapConfig& config,
    const ServerMaintenanceOperationRequest& request,
    const ServerShutdownRuntimeSnapshot& snapshot);

}  // namespace scratchbird::server

// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "agent_dependency_lifecycle.hpp"
#include "agent_runtime.hpp"
#include "agent_runtime_manager.hpp"

// SEARCH_KEY: SB_AGENT_RUNTIME_AGENT_ENGINE_LIFECYCLE_HEADER
// Engine-owned database lifecycle supervisor. This agent publishes health and
// coordinates database-local operational agents; it is never transaction,
// storage, catalog, security, policy, parser, SBLR, or recovery authority.

#include <string>
#include <vector>

namespace scratchbird::core::agents {

// SEARCH_KEY: DATABASE_ENGINE_AGENT_LIFECYCLE_CLOSURE
enum class DatabaseEngineAgentLifecycleState {
  not_started,
  starting,
  active,
  degraded,
  safe_mode,
  draining,
  stopped,
  failed,
  quarantined
};

struct DatabaseEngineAgentAuthorityBoundary {
  bool transaction_finality_authority = false;
  bool catalog_truth_authority = false;
  bool storage_identity_authority = false;
  bool authentication_authority = false;
  bool authorization_authority = false;
  bool policy_truth_authority = false;
  bool sblr_execution_authority = false;
  bool parser_admission_authority = false;
  bool recovery_finality_authority = false;
  bool engine_lifecycle_request_only = true;
};

struct DatabaseEngineAgentInput {
  std::string database_uuid;
  std::string engine_instance_uuid;
  std::string database_lifecycle_state = "opened";
  AgentLifecycleMode lifecycle_mode = AgentLifecycleMode::database_open;
  u64 policy_generation = 1;
  u64 catalog_generation = 1;
  u64 security_generation = 1;
  u64 filespace_generation = 1;
  u64 agent_set_generation = 1;
  u64 health_generation = 1;
  bool tx1_bootstrap_visible = false;
  bool tx2_activation_committed = false;
  bool startup_admitted = false;
  bool shutdown_requested = false;
  bool graceful_shutdown = true;
  bool read_only_mode = false;
  bool restricted_open_mode = false;
  bool repair_mode = false;
  bool maintenance_mode = false;
  bool backup_hold_mode = false;
  bool archive_hold_mode = false;
  bool cluster_authority_available = false;
  bool dependency_graph_consistent = true;
  bool health_publication_allowed = true;
  bool health_publication_persisted = true;
  bool allow_degraded_service = true;
  bool diagnostic_role = false;
  bool safe_mode_required = false;
  bool quarantine_required = false;
  bool enforce_dependency_lifecycle = false;
  AgentDependencyLifecycleRequest dependency_lifecycle_request;
  std::vector<std::string> unhealthy_agent_type_ids;
  std::vector<std::string> quarantined_agent_type_ids;
  std::vector<std::string> dependency_edges;
};

struct DatabaseEngineAgentHealthPublication {
  std::string database_uuid;
  std::string engine_instance_uuid;
  std::string database_lifecycle_state;
  DatabaseEngineAgentLifecycleState agent_state =
      DatabaseEngineAgentLifecycleState::not_started;
  u64 health_generation = 0;
  u64 policy_generation = 0;
  u64 catalog_generation = 0;
  u64 security_generation = 0;
  u64 filespace_generation = 0;
  u64 agent_set_generation = 0;
  bool ordinary_admission_allowed = false;
  bool shutdown_coordination_complete = false;
  bool cluster_paths_failed_closed = true;
  bool health_publication_redacted = true;
  std::string degraded_reason;
  std::string safe_mode_reason;
  std::vector<std::string> selected_agent_type_ids;
  std::vector<AgentRuntimeSelectionDecision> selection_decisions;
  std::vector<AgentTickHealthRecord> noncluster_tick_health_records;
  std::vector<std::string> redacted_diagnostics;
  DatabaseEngineAgentAuthorityBoundary authority_boundary;
};

struct DatabaseEngineAgentLifecycleResult {
  AgentRuntimeStatus status;
  DatabaseEngineAgentHealthPublication health;
  std::vector<AgentInstanceRecord> supervised_agents;

  bool ok() const { return status.ok; }
};

const char* DatabaseEngineAgentLifecycleStateName(DatabaseEngineAgentLifecycleState state);
bool DatabaseEngineAgentAuthorityBoundaryValid(const DatabaseEngineAgentAuthorityBoundary& boundary);
AgentRuntimeStatus ValidateDatabaseEngineAgentAuthorityBoundary(
    const DatabaseEngineAgentAuthorityBoundary& boundary);

DatabaseEngineAgentLifecycleResult StartDatabaseEngineLifecycleAgent(
    const DatabaseEngineAgentInput& input);
DatabaseEngineAgentLifecycleResult EvaluateDatabaseEngineLifecycleAgentHealth(
    const DatabaseEngineAgentInput& input,
    const std::vector<AgentInstanceRecord>& supervised_agents);
DatabaseEngineAgentLifecycleResult StopDatabaseEngineLifecycleAgent(
    const DatabaseEngineAgentInput& input,
    const DatabaseEngineAgentHealthPublication& current_health);

std::string SerializeDatabaseEngineAgentHealthJson(
    const DatabaseEngineAgentHealthPublication& health,
    bool diagnostic_role);

std::vector<std::string> DatabaseEngineAgentDiagnosticCodes();

}  // namespace scratchbird::core::agents

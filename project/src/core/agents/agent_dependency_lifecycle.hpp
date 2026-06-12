// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// SEARCH_KEY: CEIC_078_AGENT_DEPENDENCY_LIFECYCLE
// Agent dependency lifecycle admission for database-local DR, restore, clone,
// role-change, and restricted operating modes. This is an admission/evidence
// validator only; it is not transaction finality, visibility, security,
// recovery, parser, reference, benchmark, optimizer-plan, index-finality,
// provider-finality, cluster, memory, or agent-action authority.

#include "agent_runtime.hpp"

#include <string>
#include <vector>

namespace scratchbird::core::agents {

enum class AgentDependencyLifecycleMode {
  normal,
  read_only,
  restricted_open,
  maintenance,
  repair,
  backup,
  restore,
  archive_hold,
  shutdown,
  crash_recovery,
  pitr,
  clone,
  role_change
};

struct AgentDependencyLifecycleGenerationWindow {
  u64 required_policy_generation = 1;
  u64 required_metric_generation = 1;
  u64 required_catalog_generation = 1;
  u64 required_security_generation = 1;
  u64 required_filespace_generation = 1;
  u64 required_agent_set_generation = 1;

  u64 observed_policy_generation = 0;
  u64 observed_metric_generation = 0;
  u64 observed_catalog_generation = 0;
  u64 observed_security_generation = 0;
  u64 observed_filespace_generation = 0;
  u64 observed_agent_set_generation = 0;
};

struct AgentDependencyLifecycleAuthorityBoundary {
  bool transaction_finality_authority = false;
  bool visibility_authority = false;
  bool recovery_authority = false;
  bool security_authority = false;
  bool parser_authority = false;
  bool reference_authority = false;
  bool write_ahead_log_authority = false;
  bool benchmark_authority = false;
  bool optimizer_plan_authority = false;
  bool index_finality_authority = false;
  bool provider_finality_authority = false;
  bool cluster_authority = false;
  bool memory_authority = false;
  bool agent_action_authority = false;
};

struct AgentDependencyLifecycleRequest {
  AgentDependencyLifecycleMode mode = AgentDependencyLifecycleMode::normal;
  std::string database_uuid;
  std::string agent_type_id;
  std::string dependency_graph_digest;
  std::string lifecycle_evidence_uuid;
  AgentDependencyLifecycleGenerationWindow generations;
  AgentDependencyLifecycleAuthorityBoundary authority_boundary;

  bool dependency_graph_consistent = true;
  bool production_profile = true;

  std::string restricted_maintenance_evidence_uuid;
  std::string repair_plan_uuid;
  std::string backup_manifest_uuid;
  std::string restore_plan_uuid;
  std::string archive_hold_evidence_uuid;
  std::string shutdown_drain_evidence_uuid;
  std::string crash_recovery_evidence_uuid;
  std::string pitr_recovery_point_uuid;
  std::string clone_manifest_uuid;
  std::string clone_source_database_uuid;
  std::string clone_target_database_uuid;
  std::string role_change_evidence_uuid;
  std::string target_role;

  bool local_cluster_path_requested = false;
  bool external_cluster_provider_proof_present = false;
  std::string external_cluster_provider_id;
  std::string external_cluster_provider_evidence_uuid;
};

struct AgentDependencyLifecycleDecision {
  AgentRuntimeStatus status;
  AgentDependencyLifecycleMode mode = AgentDependencyLifecycleMode::normal;
  AgentActivationProfile maximum_activation = AgentActivationProfile::observe_only;
  bool ordinary_work_admitted = false;
  bool mutable_action_admitted = false;
  bool inspect_only = false;
  bool safe_maintenance_only = false;
  bool drain_only = false;
  bool recovery_only = false;
  bool restore_or_clone_mode = false;
  bool generation_window_valid = false;
  bool dependency_graph_valid = false;
  bool cluster_path_failed_closed = true;
  bool external_provider_only = false;
  bool evidence_non_authoritative = true;
  AgentDependencyLifecycleAuthorityBoundary evidence_authority_boundary;
  std::vector<std::string> diagnostics;
};

const char* AgentDependencyLifecycleModeName(AgentDependencyLifecycleMode mode);
AgentDependencyLifecycleMode AgentDependencyLifecycleModeFromRuntime(
    AgentLifecycleMode mode);
AgentRuntimeStatus ValidateAgentDependencyLifecycleAuthorityBoundary(
    const AgentDependencyLifecycleAuthorityBoundary& boundary);
AgentDependencyLifecycleDecision EvaluateAgentDependencyLifecycle(
    const AgentDependencyLifecycleRequest& request);
std::vector<std::string> AgentDependencyLifecycleDiagnosticCodes();

}  // namespace scratchbird::core::agents

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

// SEARCH_KEY: SB_AGENT_DATABASE_LOCAL_RUNTIME_MANAGER
// Deterministic database-local agent runtime manager. This object coordinates
// selection, state, health publication, and drain/shutdown evidence only. It is
// not storage, catalog, security, transaction, parser, SBLR, or recovery
// authority.

#include <string>
#include <vector>

namespace scratchbird::core::agents {

enum class AgentRuntimeManagerState {
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

struct AgentRuntimeActivationEvidence {
  std::string database_uuid;
  std::string engine_instance_uuid;
  AgentLifecycleMode lifecycle_mode = AgentLifecycleMode::database_open;
  u64 policy_generation = 0;
  u64 catalog_generation = 0;
  u64 security_generation = 0;
  u64 filespace_generation = 0;
  u64 agent_set_generation = 0;
  u64 health_generation = 0;
  bool tx1_bootstrap_visible = false;
  bool tx2_activation_committed = false;
  bool startup_admitted = false;
  bool health_publication_allowed = false;
  bool health_publication_persisted = false;
  bool dependency_graph_consistent = true;
};

struct AgentRuntimeManagerConfig {
  bool standalone_edition = true;
  bool cluster_authority_available = false;
  bool allow_degraded_service = true;
  bool safe_mode_required = false;
  bool quarantine_required = false;
  bool shutdown_requested = false;
  bool graceful_shutdown = true;
  bool read_only_mode = false;
  bool restricted_open_mode = false;
  bool repair_mode = false;
  bool maintenance_mode = false;
  bool backup_hold_mode = false;
  bool archive_hold_mode = false;
  std::vector<std::string> unhealthy_agent_type_ids;
  std::vector<std::string> quarantined_agent_type_ids;
  std::vector<std::string> dependency_edges;
  bool use_explicit_policy_state = false;
  std::vector<AgentPolicy> policy_records;
  std::vector<AgentPolicyAttachmentRecord> policy_attachments;
  std::vector<AgentInstanceRecord> persisted_instances;
  bool enforce_dependency_lifecycle = false;
  AgentDependencyLifecycleRequest dependency_lifecycle_request;
};

struct AgentRuntimeSelectionDecision {
  std::string agent_type_id;
  bool database_applicable = false;
  bool selected = false;
  bool policy_disabled = false;
  bool failed_closed = false;
  bool cluster_path_failed_closed = false;
  std::string diagnostic_code;
  std::string detail;
};

struct AgentRuntimeManagerSnapshot {
  AgentRuntimeManagerState state = AgentRuntimeManagerState::not_started;
  AgentRuntimeStatus status;
  std::string database_uuid;
  std::string engine_instance_uuid;
  u64 manager_generation = 0;
  u64 health_generation = 0;
  u64 policy_generation = 0;
  u64 catalog_generation = 0;
  u64 security_generation = 0;
  u64 filespace_generation = 0;
  u64 agent_set_generation = 0;
  bool activation_evidence_accepted = false;
  bool ordinary_admission_allowed = false;
  bool shutdown_coordination_complete = false;
  bool cluster_paths_failed_closed = true;
  std::vector<AgentInstanceRecord> supervised_agents;
  std::vector<AgentPolicyBootstrapRecord> policy_bootstrap_records;
  std::vector<AgentPolicyAttachmentRecord> policy_attachments;
  std::vector<AgentRuntimeSelectionDecision> selection_decisions;
  std::vector<std::string> diagnostics;
};

const char* AgentRuntimeManagerStateName(AgentRuntimeManagerState state);

AgentLifecycleState AgentLifecycleStateForActivation(AgentActivationProfile activation);
AgentRuntimeStatus ApplyAgentLifecycleTransition(AgentInstanceRecord* instance,
                                                 AgentLifecycleState to,
                                                 std::string reason);
AgentRuntimeStatus RecoverAgentLifecycleOnOpen(AgentInstanceRecord* instance);

class DatabaseLocalAgentRuntimeManager {
 public:
  const AgentRuntimeManagerSnapshot& snapshot() const { return snapshot_; }

  AgentRuntimeManagerSnapshot Start(const AgentRuntimeActivationEvidence& evidence,
                                    const AgentRuntimeManagerConfig& config);
  AgentRuntimeManagerSnapshot PublishHealth(const AgentRuntimeManagerConfig& config);
  AgentRuntimeManagerSnapshot Drain(const AgentRuntimeActivationEvidence& evidence,
                                    const AgentRuntimeManagerConfig& config);
  AgentRuntimeManagerSnapshot RecoverOpen(const AgentRuntimeActivationEvidence& evidence,
                                          const AgentRuntimeManagerConfig& config);

 private:
  AgentRuntimeManagerSnapshot snapshot_;
};

AgentRuntimeManagerSnapshot SelectStandaloneDatabaseLocalAgents(
    const AgentRuntimeActivationEvidence& evidence,
    const AgentRuntimeManagerConfig& config);

}  // namespace scratchbird::core::agents

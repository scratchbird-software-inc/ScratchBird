// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "agent_engine_lifecycle.hpp"

#include "agent_runtime_manager.hpp"

#include <algorithm>
#include <sstream>
#include <utility>

namespace scratchbird::core::agents {
namespace {

bool Contains(const std::vector<std::string>& values, const std::string& value) {
  return std::find(values.begin(), values.end(), value) != values.end();
}

bool ScopeContainsDatabase(const std::string& scope) {
  return scope.find("database") != std::string::npos;
}

bool HasClusterMetricDependency(const AgentTypeDescriptor& descriptor) {
  for (const auto& dependency : descriptor.metric_dependencies) {
    if (dependency.cluster_only) {
      return true;
    }
  }
  return false;
}

bool IsCriticalDatabaseAgent(const std::string& type_id) {
  return type_id == "metrics_registry_manager" ||
         type_id == "storage_health_manager" ||
         type_id == "transaction_pressure_manager" ||
         type_id == "cleanup_archive_manager" ||
         type_id == "identity_manager" ||
         type_id == "session_control_manager";
}

std::string JsonEscape(const std::string& value) {
  std::string out;
  out.reserve(value.size());
  for (const char ch : value) {
    switch (ch) {
      case '\\': out += "\\\\"; break;
      case '"': out += "\\\""; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default: out.push_back(ch); break;
    }
  }
  return out;
}

DatabaseEngineAgentHealthPublication BaseHealth(const DatabaseEngineAgentInput& input) {
  DatabaseEngineAgentHealthPublication health;
  health.database_uuid = input.database_uuid;
  health.engine_instance_uuid = input.engine_instance_uuid;
  health.database_lifecycle_state = input.database_lifecycle_state;
  health.health_generation = input.health_generation == 0 ? 1 : input.health_generation;
  health.policy_generation = input.policy_generation;
  health.catalog_generation = input.catalog_generation;
  health.security_generation = input.security_generation;
  health.filespace_generation = input.filespace_generation;
  health.agent_set_generation = input.agent_set_generation == 0 ? 1 : input.agent_set_generation;
  health.cluster_paths_failed_closed = !input.cluster_authority_available;
  return health;
}

AgentRuntimeContext RuntimeContextForInput(const DatabaseEngineAgentInput& input) {
  AgentRuntimeContext context;
  context.security_context_present = true;
  context.cluster_authority_available = input.cluster_authority_available;
  context.private_features_available = true;
  context.standalone_edition = !input.cluster_authority_available;
  context.shutdown_requested = input.shutdown_requested;
  context.read_only_mode = input.read_only_mode ||
                           input.shutdown_requested ||
                           input.lifecycle_mode == AgentLifecycleMode::database_close ||
                           input.lifecycle_mode == AgentLifecycleMode::shutdown ||
                           input.lifecycle_mode == AgentLifecycleMode::read_only ||
                           input.lifecycle_mode == AgentLifecycleMode::pitr ||
                           input.lifecycle_mode == AgentLifecycleMode::clone ||
                           input.lifecycle_mode == AgentLifecycleMode::role_change;
  context.maintenance_mode = input.maintenance_mode ||
                             input.lifecycle_mode == AgentLifecycleMode::maintenance;
  context.restricted_open_mode = input.restricted_open_mode ||
                                 input.lifecycle_mode == AgentLifecycleMode::restricted_open;
  context.repair_mode = input.repair_mode ||
                        input.lifecycle_mode == AgentLifecycleMode::repair;
  context.backup_hold_mode = input.backup_hold_mode ||
                             input.lifecycle_mode == AgentLifecycleMode::backup ||
                             input.lifecycle_mode == AgentLifecycleMode::restore ||
                             input.lifecycle_mode == AgentLifecycleMode::pitr ||
                             input.lifecycle_mode == AgentLifecycleMode::clone;
  context.archive_hold_mode = input.archive_hold_mode ||
                              input.lifecycle_mode == AgentLifecycleMode::archive_hold;
  context.principal_uuid = DeterministicAgentRuntimePrincipalUuidFromKey(
      input.database_uuid + "|database_lifecycle_agent|principal|" +
      std::to_string(input.policy_generation));
  context.database_uuid = input.database_uuid;
  context.trace_tags.push_back("database_engine_lifecycle_agent");
  context.groups.push_back("OPS");
  context.rights.push_back("OBS_AGENT_STATE_READ");
  context.rights.push_back("OBS_AGENT_EVIDENCE_READ");
  context.rights.push_back("OBS_AGENT_CONTROL");
  return context;
}

bool InputHasRequiredActivationEvidence(const DatabaseEngineAgentInput& input) {
  return !input.database_uuid.empty() &&
         !input.engine_instance_uuid.empty() &&
         input.policy_generation != 0 &&
         input.catalog_generation != 0 &&
         input.security_generation != 0 &&
         input.filespace_generation != 0 &&
         input.tx1_bootstrap_visible &&
         input.tx2_activation_committed &&
         input.startup_admitted;
}

AgentRuntimeActivationEvidence ActivationEvidenceForInput(const DatabaseEngineAgentInput& input) {
  AgentRuntimeActivationEvidence evidence;
  evidence.database_uuid = input.database_uuid;
  evidence.engine_instance_uuid = input.engine_instance_uuid;
  evidence.lifecycle_mode = input.lifecycle_mode;
  evidence.policy_generation = input.policy_generation;
  evidence.catalog_generation = input.catalog_generation;
  evidence.security_generation = input.security_generation;
  evidence.filespace_generation = input.filespace_generation;
  evidence.agent_set_generation = input.agent_set_generation;
  evidence.health_generation = input.health_generation;
  evidence.tx1_bootstrap_visible = input.tx1_bootstrap_visible;
  evidence.tx2_activation_committed = input.tx2_activation_committed;
  evidence.startup_admitted = input.startup_admitted;
  evidence.health_publication_allowed = input.health_publication_allowed;
  evidence.health_publication_persisted = input.health_publication_persisted;
  evidence.dependency_graph_consistent = input.dependency_graph_consistent;
  return evidence;
}

AgentRuntimeManagerConfig RuntimeManagerConfigForInput(const DatabaseEngineAgentInput& input) {
  AgentRuntimeManagerConfig config;
  config.standalone_edition = !input.cluster_authority_available;
  config.cluster_authority_available = input.cluster_authority_available;
  config.allow_degraded_service = input.allow_degraded_service;
  config.safe_mode_required = input.safe_mode_required;
  config.quarantine_required = input.quarantine_required;
  config.shutdown_requested = input.shutdown_requested;
  config.graceful_shutdown = input.graceful_shutdown;
  config.read_only_mode = input.read_only_mode;
  config.restricted_open_mode = input.restricted_open_mode;
  config.repair_mode = input.repair_mode;
  config.maintenance_mode = input.maintenance_mode;
  config.backup_hold_mode = input.backup_hold_mode;
  config.archive_hold_mode = input.archive_hold_mode;
  config.unhealthy_agent_type_ids = input.unhealthy_agent_type_ids;
  config.quarantined_agent_type_ids = input.quarantined_agent_type_ids;
  config.dependency_edges = input.dependency_edges;
  config.enforce_dependency_lifecycle = input.enforce_dependency_lifecycle;
  config.dependency_lifecycle_request = input.dependency_lifecycle_request;
  return config;
}

DatabaseEngineAgentLifecycleState LifecycleStateForRuntimeManager(
    AgentRuntimeManagerState state) {
  switch (state) {
    case AgentRuntimeManagerState::not_started: return DatabaseEngineAgentLifecycleState::not_started;
    case AgentRuntimeManagerState::starting: return DatabaseEngineAgentLifecycleState::starting;
    case AgentRuntimeManagerState::active: return DatabaseEngineAgentLifecycleState::active;
    case AgentRuntimeManagerState::degraded: return DatabaseEngineAgentLifecycleState::degraded;
    case AgentRuntimeManagerState::safe_mode: return DatabaseEngineAgentLifecycleState::safe_mode;
    case AgentRuntimeManagerState::draining: return DatabaseEngineAgentLifecycleState::draining;
    case AgentRuntimeManagerState::stopped: return DatabaseEngineAgentLifecycleState::stopped;
    case AgentRuntimeManagerState::failed: return DatabaseEngineAgentLifecycleState::failed;
    case AgentRuntimeManagerState::quarantined: return DatabaseEngineAgentLifecycleState::quarantined;
  }
  return DatabaseEngineAgentLifecycleState::failed;
}

void ApplyRuntimeManagerSnapshot(const AgentRuntimeManagerSnapshot& snapshot,
                                 DatabaseEngineAgentLifecycleResult* result) {
  if (result == nullptr) {
    return;
  }
  result->status = snapshot.status;
  result->health.database_uuid = snapshot.database_uuid;
  result->health.engine_instance_uuid = snapshot.engine_instance_uuid;
  result->health.health_generation = snapshot.health_generation;
  result->health.policy_generation = snapshot.policy_generation;
  result->health.catalog_generation = snapshot.catalog_generation;
  result->health.security_generation = snapshot.security_generation;
  result->health.filespace_generation = snapshot.filespace_generation;
  result->health.agent_set_generation = snapshot.agent_set_generation;
  result->health.agent_state = LifecycleStateForRuntimeManager(snapshot.state);
  result->health.ordinary_admission_allowed = snapshot.ordinary_admission_allowed;
  result->health.shutdown_coordination_complete = snapshot.shutdown_coordination_complete;
  result->health.cluster_paths_failed_closed = snapshot.cluster_paths_failed_closed;
  result->health.selected_agent_type_ids.clear();
  result->health.selection_decisions = snapshot.selection_decisions;
  result->health.noncluster_tick_health_records.clear();
  result->health.redacted_diagnostics = snapshot.diagnostics;
  result->supervised_agents = snapshot.supervised_agents;
  for (const auto& instance : snapshot.supervised_agents) {
    result->health.selected_agent_type_ids.push_back(instance.agent_type_id);
  }
  AgentTickHealthRequest tick_request;
  tick_request.context.security_context_present = true;
  tick_request.context.cluster_authority_available = false;
  tick_request.context.private_features_available = true;
  tick_request.context.standalone_edition = true;
  tick_request.context.principal_uuid = DeterministicAgentRuntimePrincipalUuidFromKey(
      snapshot.database_uuid + "|database_engine_agent_tick_health");
  tick_request.context.database_uuid = snapshot.database_uuid;
  tick_request.context.rights.push_back("OBS_AGENT_STATE_READ");
  const u64 time_reference = snapshot.health_generation == 0
      ? (snapshot.manager_generation == 0 ? 1 : snapshot.manager_generation)
      : snapshot.health_generation;
  tick_request.context.wall_now_microseconds = time_reference;
  tick_request.context.monotonic_now_microseconds = time_reference;
  tick_request.policy_generation = snapshot.policy_generation == 0
      ? 1
      : snapshot.policy_generation;
  const auto tick_result = BuildNonClusterAgentTickHealthSnapshot(tick_request);
  if (tick_result.status.ok) {
    result->health.noncluster_tick_health_records = tick_result.records;
  } else {
    result->health.redacted_diagnostics.push_back(tick_result.status.diagnostic_code);
  }
}

void AddDiagnostic(DatabaseEngineAgentHealthPublication* health,
                   const std::string& diagnostic) {
  if (health != nullptr) {
    health->redacted_diagnostics.push_back(diagnostic);
  }
}

std::vector<AgentInstanceRecord> SelectDatabaseLocalAgents(
    const DatabaseEngineAgentInput& input,
    DatabaseEngineAgentHealthPublication* health,
    AgentRuntimeStatus* status) {
  std::vector<AgentInstanceRecord> selected;
  const auto registry_status = ValidateCanonicalAgentRegistry();
  if (!registry_status.ok) {
    if (status != nullptr) {
      *status = AgentError("ENGINE.AGENT_LIFECYCLE_INPUT_INVALID",
                           registry_status.diagnostic_code + ":" + registry_status.detail);
    }
    return selected;
  }
  if (!AgentPersistenceUsesScratchBirdStorageAuthority()) {
    if (status != nullptr) {
      *status = AgentError("ENGINE.AGENT_LIFECYCLE_AUTHORITY_DENIED",
                           "agent_persistence_not_scratchbird_storage_authority");
    }
    return selected;
  }

  const auto boundary_status =
      ValidateDatabaseEngineAgentAuthorityBoundary(DatabaseEngineAgentAuthorityBoundary{});
  if (!boundary_status.ok) {
    if (status != nullptr) {
      *status = boundary_status;
    }
    return selected;
  }

  const auto context = RuntimeContextForInput(input);
  for (const auto& descriptor : CanonicalAgentRegistry()) {
    if (!ScopeContainsDatabase(descriptor.scope)) {
      continue;
    }
    const auto availability = EvaluateAgentFeatureAvailability(descriptor, context);
    if (descriptor.cluster_only ||
        descriptor.deployment == AgentDeployment::cluster ||
        (!input.cluster_authority_available && HasClusterMetricDependency(descriptor)) ||
        availability == AgentFeatureAvailability::unavailable_cluster_authority ||
        availability == AgentFeatureAvailability::unavailable_edition) {
      AddDiagnostic(health, "cluster_agent_skipped_fail_closed:" + descriptor.type_id);
      continue;
    }
    if (availability != AgentFeatureAvailability::available) {
      if (status != nullptr) {
        *status = AgentError("ENGINE.AGENT_LIFECYCLE_INPUT_INVALID",
                             descriptor.type_id + ":" +
                                 AgentFeatureAvailabilityName(availability));
      }
      selected.clear();
      return selected;
    }

    auto policy = BaselinePolicyForAgent(descriptor);
    policy.activation = EffectiveActivationForLifecycle(policy.activation, input.lifecycle_mode);
    const auto policy_status = ValidateAgentPolicy(policy, descriptor);
    if (!policy_status.ok) {
      if (status != nullptr) {
        *status = AgentError("ENGINE.AGENT_LIFECYCLE_INPUT_INVALID",
                             policy_status.diagnostic_code + ":" + policy_status.detail);
      }
      selected.clear();
      return selected;
    }
    if (!policy.enabled || policy.activation == AgentActivationProfile::disabled) {
      continue;
    }

    AgentInstanceRecord instance;
    instance.instance_uuid = DeterministicAgentInstanceUuid(
        input.database_uuid,
        descriptor.type_id,
        descriptor.scope,
        input.agent_set_generation == 0 ? 1 : input.agent_set_generation);
    instance.agent_type_id = descriptor.type_id;
    instance.policy_uuid = policy.policy_uuid;
    instance.scope = descriptor.scope;
    if (policy.activation == AgentActivationProfile::observe_only) {
      instance.state = AgentLifecycleState::observe_only;
    } else if (policy.activation == AgentActivationProfile::recommend_only) {
      instance.state = AgentLifecycleState::recommend_only;
    } else if (policy.activation == AgentActivationProfile::dry_run) {
      instance.state = AgentLifecycleState::dry_run;
    } else {
      instance.state = AgentLifecycleState::running;
    }
    if (Contains(input.quarantined_agent_type_ids, descriptor.type_id)) {
      instance.state = AgentLifecycleState::quarantined;
      instance.quarantined = true;
    } else if (Contains(input.unhealthy_agent_type_ids, descriptor.type_id)) {
      instance.state = AgentLifecycleState::failed;
      ++instance.crash_loop_count;
    }
    selected.push_back(std::move(instance));
    if (health != nullptr) {
      health->selected_agent_type_ids.push_back(descriptor.type_id);
    }
  }
  if (status != nullptr) {
    *status = AgentOk();
  }
  return selected;
}

}  // namespace

const char* DatabaseEngineAgentLifecycleStateName(DatabaseEngineAgentLifecycleState state) {
  switch (state) {
    case DatabaseEngineAgentLifecycleState::not_started: return "not_started";
    case DatabaseEngineAgentLifecycleState::starting: return "starting";
    case DatabaseEngineAgentLifecycleState::active: return "active";
    case DatabaseEngineAgentLifecycleState::degraded: return "degraded";
    case DatabaseEngineAgentLifecycleState::safe_mode: return "safe_mode";
    case DatabaseEngineAgentLifecycleState::draining: return "draining";
    case DatabaseEngineAgentLifecycleState::stopped: return "stopped";
    case DatabaseEngineAgentLifecycleState::failed: return "failed";
    case DatabaseEngineAgentLifecycleState::quarantined: return "quarantined";
  }
  return "failed";
}

bool DatabaseEngineAgentAuthorityBoundaryValid(const DatabaseEngineAgentAuthorityBoundary& boundary) {
  return !boundary.transaction_finality_authority &&
         !boundary.catalog_truth_authority &&
         !boundary.storage_identity_authority &&
         !boundary.authentication_authority &&
         !boundary.authorization_authority &&
         !boundary.policy_truth_authority &&
         !boundary.sblr_execution_authority &&
         !boundary.parser_admission_authority &&
         !boundary.recovery_finality_authority &&
         boundary.engine_lifecycle_request_only;
}

AgentRuntimeStatus ValidateDatabaseEngineAgentAuthorityBoundary(
    const DatabaseEngineAgentAuthorityBoundary& boundary) {
  if (DatabaseEngineAgentAuthorityBoundaryValid(boundary)) {
    return AgentOk();
  }
  return AgentError("ENGINE.AGENT_LIFECYCLE_AUTHORITY_DENIED",
                    "database_engine_lifecycle_agent_authority_boundary_invalid");
}

// SEARCH_KEY: SB_AGENT_RUNTIME_ENGINE_LIFECYCLE_START
DatabaseEngineAgentLifecycleResult StartDatabaseEngineLifecycleAgent(
    const DatabaseEngineAgentInput& input) {
  DatabaseEngineAgentLifecycleResult result;
  result.health = BaseHealth(input);
  result.health.agent_state = DatabaseEngineAgentLifecycleState::starting;

  if (!InputHasRequiredActivationEvidence(input) ||
      !input.health_publication_allowed ||
      !input.health_publication_persisted) {
    result.health.agent_state = DatabaseEngineAgentLifecycleState::not_started;
    AddDiagnostic(&result.health, "ENGINE.AGENT_LIFECYCLE_INPUT_INVALID");
    result.status = AgentError("ENGINE.AGENT_LIFECYCLE_INPUT_INVALID",
                               "tx1_tx2_policy_generation_and_health_publication_required");
    return result;
  }
  if (!input.dependency_graph_consistent) {
    result.health.agent_state = DatabaseEngineAgentLifecycleState::failed;
    AddDiagnostic(&result.health, "ENGINE.AGENT_LIFECYCLE_INPUT_INVALID");
    result.status = AgentError("ENGINE.AGENT_LIFECYCLE_INPUT_INVALID",
                               "agent_dependency_graph_inconsistent");
    return result;
  }
  if (!input.dependency_edges.empty()) {
    const auto graph = ValidateAgentDependencyGraph(input.dependency_edges);
    if (!graph.ok) {
      result.health.agent_state = DatabaseEngineAgentLifecycleState::failed;
      AddDiagnostic(&result.health, graph.diagnostic_code);
      result.status = AgentError("ENGINE.AGENT_LIFECYCLE_INPUT_INVALID",
                                 graph.diagnostic_code + ":" + graph.detail);
      return result;
    }
  }

  DatabaseLocalAgentRuntimeManager manager;
  const auto snapshot = manager.Start(ActivationEvidenceForInput(input),
                                      RuntimeManagerConfigForInput(input));
  ApplyRuntimeManagerSnapshot(snapshot, &result);
  result.health.database_lifecycle_state = input.database_lifecycle_state;
  if (!result.status.ok) {
    result.health.agent_state = DatabaseEngineAgentLifecycleState::failed;
    return result;
  }
  if (result.health.agent_state == DatabaseEngineAgentLifecycleState::degraded) {
    result.health.degraded_reason = "noncritical_database_local_agent_unhealthy";
  } else if (result.health.agent_state == DatabaseEngineAgentLifecycleState::safe_mode) {
    result.health.safe_mode_reason = input.safe_mode_required
        ? "safe_mode_required_by_policy"
        : "critical_database_local_agent_unhealthy";
  } else if (result.health.agent_state == DatabaseEngineAgentLifecycleState::quarantined) {
    result.health.safe_mode_reason = "quarantine_required_by_policy";
  }
  return result;
}

DatabaseEngineAgentLifecycleResult EvaluateDatabaseEngineLifecycleAgentHealth(
    const DatabaseEngineAgentInput& input,
    const std::vector<AgentInstanceRecord>& supervised_agents) {
  DatabaseEngineAgentLifecycleResult result;
  result.health = BaseHealth(input);
  result.supervised_agents = supervised_agents;
  for (const auto& instance : supervised_agents) {
    result.health.selected_agent_type_ids.push_back(instance.agent_type_id);
  }

  if (input.quarantine_required) {
    result.health.agent_state = DatabaseEngineAgentLifecycleState::quarantined;
    result.health.safe_mode_reason = "quarantine_required_by_policy";
    result.health.ordinary_admission_allowed = false;
    AddDiagnostic(&result.health, "ENGINE.AGENT_SAFE_MODE_REQUIRED");
    result.status = AgentOk();
    return result;
  }

  bool critical_unhealthy = false;
  bool noncritical_unhealthy = false;
  for (const auto& instance : supervised_agents) {
    const bool unhealthy = instance.state == AgentLifecycleState::failed ||
                           instance.state == AgentLifecycleState::quarantined ||
                           instance.quarantined;
    if (!unhealthy) {
      continue;
    }
    if (IsCriticalDatabaseAgent(instance.agent_type_id)) {
      critical_unhealthy = true;
    } else {
      noncritical_unhealthy = true;
    }
  }

  if (input.safe_mode_required || critical_unhealthy) {
    result.health.agent_state = DatabaseEngineAgentLifecycleState::safe_mode;
    result.health.safe_mode_reason = input.safe_mode_required
        ? "safe_mode_required_by_policy"
        : "critical_database_local_agent_unhealthy";
    result.health.ordinary_admission_allowed = false;
    AddDiagnostic(&result.health, "ENGINE.AGENT_SAFE_MODE_REQUIRED");
    result.status = AgentOk();
    return result;
  }

  if (noncritical_unhealthy) {
    result.health.agent_state = input.allow_degraded_service
        ? DatabaseEngineAgentLifecycleState::degraded
        : DatabaseEngineAgentLifecycleState::safe_mode;
    result.health.degraded_reason = "noncritical_database_local_agent_unhealthy";
    result.health.ordinary_admission_allowed = input.allow_degraded_service;
    AddDiagnostic(&result.health, "ENGINE.AGENT_HEALTH_DEGRADED");
    result.status = AgentOk();
    return result;
  }

  result.health.agent_state = DatabaseEngineAgentLifecycleState::active;
  result.health.ordinary_admission_allowed = true;
  AddDiagnostic(&result.health, "ENGINE.AGENT_LIFECYCLE_HEALTHY");
  result.status = AgentOk();
  return result;
}

DatabaseEngineAgentLifecycleResult StopDatabaseEngineLifecycleAgent(
    const DatabaseEngineAgentInput& input,
    const DatabaseEngineAgentHealthPublication& current_health) {
  DatabaseEngineAgentLifecycleResult result;
  result.health = current_health;
  result.health.database_lifecycle_state = input.database_lifecycle_state.empty()
      ? current_health.database_lifecycle_state
      : input.database_lifecycle_state;
  result.health.health_generation =
      input.health_generation == 0 ? current_health.health_generation + 1 : input.health_generation;
  result.health.agent_state = input.shutdown_requested
      ? DatabaseEngineAgentLifecycleState::stopped
      : DatabaseEngineAgentLifecycleState::draining;
  result.health.ordinary_admission_allowed = false;
  result.health.shutdown_coordination_complete = input.shutdown_requested &&
                                                input.graceful_shutdown;
  result.health.redacted_diagnostics.clear();
  AddDiagnostic(&result.health, result.health.shutdown_coordination_complete
                                  ? "ENGINE.AGENT_LIFECYCLE_STOPPED"
                                  : "ENGINE.AGENT_LIFECYCLE_DRAINING");
  result.status = AgentOk();
  return result;
}

void WriteSelectionDecisionJson(std::ostringstream* out,
                                const AgentRuntimeSelectionDecision& decision) {
  if (out == nullptr) {
    return;
  }
  bool operator_only = false;
  for (const auto& descriptor : CanonicalAgentRegistry()) {
    if (descriptor.type_id == decision.agent_type_id) {
      const auto baseline = BaselinePolicyForAgent(descriptor);
      operator_only = decision.policy_disabled &&
          baseline.enabled &&
          baseline.action_mode == "operator_only";
      break;
    }
  }
  *out << "{\"agent_type_id\":\"" << JsonEscape(decision.agent_type_id) << "\","
       << "\"database_applicable\":" << (decision.database_applicable ? "true" : "false") << ","
       << "\"selected\":" << (decision.selected ? "true" : "false") << ","
       << "\"policy_disabled\":" << (decision.policy_disabled ? "true" : "false") << ","
       << "\"operator_only\":" << (operator_only ? "true" : "false") << ","
       << "\"failed_closed\":" << (decision.failed_closed ? "true" : "false") << ","
       << "\"cluster_path_failed_closed\":"
       << (decision.cluster_path_failed_closed ? "true" : "false") << ","
       << "\"diagnostic_code\":\"" << JsonEscape(decision.diagnostic_code) << "\","
       << "\"detail\":\"" << JsonEscape(decision.detail) << "\"}";
}

void WriteTickHealthRecordJson(std::ostringstream* out,
                               const AgentTickHealthRecord& record) {
  if (out == nullptr) {
    return;
  }
  const bool action_evidence_required =
      record.action_class != AgentActionClass::none ||
      record.action_result_class != AgentActionResultClass::accepted;
  const bool operator_only =
      record.tick_class == AgentTickHealthClass::manual_approval_operator_only ||
      record.manual_approval_required;
  *out << "{\"agent_type_id\":\"" << JsonEscape(record.agent_type_id) << "\","
       << "\"deployment\":\"" << AgentDeploymentName(record.deployment) << "\","
       << "\"policy_uuid\":\"" << JsonEscape(record.policy_uuid) << "\","
       << "\"tick_class\":\"" << AgentTickHealthClassName(record.tick_class) << "\","
       << "\"lifecycle_state\":\"" << AgentLifecycleStateName(record.lifecycle_state) << "\","
       << "\"action_class\":\"" << AgentActionClassName(record.action_class) << "\","
       << "\"action_result_class\":\""
       << AgentActionResultClassName(record.action_result_class) << "\","
       << "\"selected\":" << (record.selected ? "true" : "false") << ","
       << "\"runnable\":" << (record.runnable ? "true" : "false") << ","
       << "\"tick_produced\":" << (record.tick_produced ? "true" : "false") << ","
       << "\"health_published\":" << (record.health_published ? "true" : "false") << ","
       << "\"action_evidence_required\":"
       << (action_evidence_required ? "true" : "false") << ","
       << "\"action_evidence_published\":"
       << (record.action_evidence_published ? "true" : "false") << ","
       << "\"policy_disabled\":" << (record.policy_disabled ? "true" : "false") << ","
       << "\"suppressed\":" << (record.suppressed ? "true" : "false") << ","
       << "\"manual_approval_required\":"
       << (record.manual_approval_required ? "true" : "false") << ","
       << "\"operator_only\":" << (operator_only ? "true" : "false") << ","
       << "\"failed_closed\":" << (record.failed_closed ? "true" : "false") << ","
       << "\"cluster_path_failed_closed\":"
       << (record.cluster_path_failed_closed ? "true" : "false") << ","
       << "\"resource_budget_limited\":"
       << (record.resource_budget_limited ? "true" : "false") << ","
       << "\"diagnostic_code\":\"" << JsonEscape(record.diagnostic_code) << "\","
       << "\"detail\":\"" << JsonEscape(record.detail) << "\","
       << "\"health_evidence_uuid\":\"" << JsonEscape(record.health_evidence_uuid) << "\","
       << "\"action_evidence_uuid\":\"" << JsonEscape(record.action_evidence_uuid) << "\"}";
}

std::string SerializeDatabaseEngineAgentHealthJson(
    const DatabaseEngineAgentHealthPublication& health,
    bool diagnostic_role) {
  std::ostringstream out;
  out << "{\"database_engine_agent\":{"
      << "\"database_uuid\":\"" << JsonEscape(health.database_uuid) << "\","
      << "\"engine_instance_uuid\":\"" << JsonEscape(health.engine_instance_uuid) << "\","
      << "\"database_lifecycle_state\":\"" << JsonEscape(health.database_lifecycle_state) << "\","
      << "\"agent_state\":\"" << DatabaseEngineAgentLifecycleStateName(health.agent_state) << "\","
      << "\"health_generation\":" << health.health_generation << ","
      << "\"policy_generation\":" << health.policy_generation << ","
      << "\"catalog_generation\":" << health.catalog_generation << ","
      << "\"security_generation\":" << health.security_generation << ","
      << "\"filespace_generation\":" << health.filespace_generation << ","
      << "\"agent_set_generation\":" << health.agent_set_generation << ","
      << "\"ordinary_admission_allowed\":"
      << (health.ordinary_admission_allowed ? "true" : "false") << ","
      << "\"shutdown_coordination_complete\":"
      << (health.shutdown_coordination_complete ? "true" : "false") << ","
      << "\"cluster_paths_failed_closed\":"
      << (health.cluster_paths_failed_closed ? "true" : "false") << ","
      << "\"authority_boundary_valid\":"
      << (DatabaseEngineAgentAuthorityBoundaryValid(health.authority_boundary) ? "true" : "false") << ","
      << "\"degraded_reason\":\"" << JsonEscape(health.degraded_reason) << "\","
      << "\"safe_mode_reason\":\"" << JsonEscape(health.safe_mode_reason) << "\","
      << "\"selected_agents\":[";
  for (std::size_t i = 0; i < health.selected_agent_type_ids.size(); ++i) {
    if (i != 0) {
      out << ',';
    }
    out << "\"" << JsonEscape(health.selected_agent_type_ids[i]) << "\"";
  }
  out << "],\"selection_evidence\":[";
  for (std::size_t i = 0; i < health.selection_decisions.size(); ++i) {
    if (i != 0) {
      out << ',';
    }
    WriteSelectionDecisionJson(&out, health.selection_decisions[i]);
  }
  out << "],\"noncluster_tick_health_records\":[";
  for (std::size_t i = 0; i < health.noncluster_tick_health_records.size(); ++i) {
    if (i != 0) {
      out << ',';
    }
    WriteTickHealthRecordJson(&out, health.noncluster_tick_health_records[i]);
  }
  out << "],\"diagnostics\":[";
  for (std::size_t i = 0; i < health.redacted_diagnostics.size(); ++i) {
    if (i != 0) {
      out << ',';
    }
    const std::string diagnostic = diagnostic_role
        ? health.redacted_diagnostics[i]
        : health.redacted_diagnostics[i].substr(0, health.redacted_diagnostics[i].find(':'));
    out << "\"" << JsonEscape(diagnostic) << "\"";
  }
  out << "]}}";
  return out.str();
}

std::vector<std::string> DatabaseEngineAgentDiagnosticCodes() {
  return {"ENGINE.AGENT_LIFECYCLE_INPUT_INVALID",
          "ENGINE.AGENT_LIFECYCLE_AUTHORITY_DENIED",
          "ENGINE.AGENT_HEALTH_DEGRADED",
          "ENGINE.AGENT_SAFE_MODE_REQUIRED",
          "ENGINE.AGENT_LIFECYCLE_HEALTHY",
          "ENGINE.AGENT_LIFECYCLE_DRAINING",
          "ENGINE.AGENT_LIFECYCLE_STOPPED"};
}

}  // namespace scratchbird::core::agents

// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "agent_runtime_manager.hpp"

#include <algorithm>
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

bool ScopeContainsCluster(const std::string& scope) {
  return scope.find("cluster") != std::string::npos;
}

bool IsCriticalDatabaseAgent(const std::string& type_id) {
  return type_id == "metrics_registry_manager" ||
         type_id == "storage_health_manager" ||
         type_id == "transaction_pressure_manager" ||
         type_id == "cleanup_archive_manager" ||
         type_id == "identity_manager" ||
         type_id == "session_control_manager";
}

bool LifecycleModeRequiresSafeAdmission(AgentLifecycleMode mode) {
  return mode == AgentLifecycleMode::read_only ||
         mode == AgentLifecycleMode::restricted_open ||
         mode == AgentLifecycleMode::repair ||
         mode == AgentLifecycleMode::maintenance ||
         mode == AgentLifecycleMode::backup ||
         mode == AgentLifecycleMode::restore ||
         mode == AgentLifecycleMode::archive_hold ||
         mode == AgentLifecycleMode::crash_recovery ||
         mode == AgentLifecycleMode::pitr ||
         mode == AgentLifecycleMode::clone ||
         mode == AgentLifecycleMode::role_change;
}

// SEARCH_KEY: OpenStateModeDiagnostic
std::string OpenStateModeDiagnostic(const AgentRuntimeActivationEvidence& evidence,
                                    const AgentRuntimeManagerConfig& config) {
  if (config.shutdown_requested || evidence.lifecycle_mode == AgentLifecycleMode::shutdown ||
      evidence.lifecycle_mode == AgentLifecycleMode::database_close) {
    return "ENGINE.AGENT_OPEN_STATE.SHUTDOWN_DRAIN_ONLY";
  }
  if (config.read_only_mode || evidence.lifecycle_mode == AgentLifecycleMode::read_only) {
    return "ENGINE.AGENT_OPEN_STATE.READ_ONLY_INSPECT_ONLY";
  }
  if (config.restricted_open_mode || evidence.lifecycle_mode == AgentLifecycleMode::restricted_open) {
    return "ENGINE.AGENT_OPEN_STATE.RESTRICTED_SAFE_MAINTENANCE_ONLY";
  }
  if (config.repair_mode || evidence.lifecycle_mode == AgentLifecycleMode::repair) {
    return "ENGINE.AGENT_OPEN_STATE.REPAIR_SAFE_MAINTENANCE_ONLY";
  }
  if (config.maintenance_mode || evidence.lifecycle_mode == AgentLifecycleMode::maintenance) {
    return "ENGINE.AGENT_OPEN_STATE.MAINTENANCE_SAFE_ONLY";
  }
  if (config.backup_hold_mode || evidence.lifecycle_mode == AgentLifecycleMode::backup ||
      evidence.lifecycle_mode == AgentLifecycleMode::restore) {
    return "ENGINE.AGENT_OPEN_STATE.BACKUP_HOLD_INSPECT_ONLY";
  }
  if (config.archive_hold_mode || evidence.lifecycle_mode == AgentLifecycleMode::archive_hold) {
    return "ENGINE.AGENT_OPEN_STATE.ARCHIVE_HOLD_INSPECT_ONLY";
  }
  if (evidence.lifecycle_mode == AgentLifecycleMode::crash_recovery) {
    return "ENGINE.AGENT_OPEN_STATE.RECOVERY_SAFE_ONLY";
  }
  if (evidence.lifecycle_mode == AgentLifecycleMode::pitr) {
    return "ENGINE.AGENT_OPEN_STATE.PITR_INSPECT_ONLY";
  }
  if (evidence.lifecycle_mode == AgentLifecycleMode::clone) {
    return "ENGINE.AGENT_OPEN_STATE.CLONE_INSPECT_ONLY";
  }
  if (evidence.lifecycle_mode == AgentLifecycleMode::role_change) {
    return "ENGINE.AGENT_OPEN_STATE.ROLE_CHANGE_INSPECT_ONLY";
  }
  return {};
}

bool OpenStateSuppressesNormalLoops(const AgentRuntimeActivationEvidence& evidence,
                                    const AgentRuntimeManagerConfig& config) {
  return config.read_only_mode ||
         config.restricted_open_mode ||
         config.repair_mode ||
         config.maintenance_mode ||
         config.backup_hold_mode ||
         config.archive_hold_mode ||
         LifecycleModeRequiresSafeAdmission(evidence.lifecycle_mode);
}

AgentPolicyBootstrapRecord BootstrapRecordFromPolicy(const std::string& agent_type_id,
                                                     const AgentPolicy& policy) {
  AgentPolicyBootstrapRecord record;
  record.agent_type_id = agent_type_id;
  record.policy_family = policy.policy_family;
  record.policy_uuid = policy.policy_uuid;
  record.scope = policy.scope;
  record.action_mode = policy.action_mode;
  record.invalid_policy_behavior = policy.invalid_policy_behavior;
  record.enabled = policy.enabled;
  record.activation = policy.activation;
  record.policy_generation = policy.policy_generation;
  record.run_interval_microseconds = policy.run_interval_microseconds;
  record.cooldown_microseconds = policy.cooldown_microseconds;
  record.required_fields = RequiredPolicyConfigFieldsForFamily(policy.policy_family);
  record.config_fields = policy.config_fields;
  return record;
}

const AgentPolicy* FindPolicy(const std::vector<AgentPolicy>& policies,
                              const std::string& agent_type_id,
                              const std::string& policy_family) {
  const auto descriptor = FindAgentType(agent_type_id);
  for (const auto& policy : policies) {
    if (policy.policy_family == policy_family &&
        (!descriptor.has_value() || policy.scope == descriptor->scope)) {
      return &policy;
    }
  }
  return nullptr;
}

std::string AgentTypeIdForPolicy(
    const AgentPolicy& policy,
    const std::vector<AgentPolicyAttachmentRecord>& attachments) {
  for (const auto& attachment : attachments) {
    if (attachment.policy_uuid == policy.policy_uuid &&
        attachment.policy_family == policy.policy_family &&
        !attachment.agent_type_id.empty()) {
      return attachment.agent_type_id;
    }
  }
  for (const auto& descriptor : CanonicalAgentRegistry()) {
    if (descriptor.scope != policy.scope) { continue; }
    if (Contains(RequiredPolicyFamiliesForAgent(descriptor), policy.policy_family)) {
      return descriptor.type_id;
    }
  }
  return {};
}

const AgentPolicyAttachmentRecord* FindAttachment(
    const std::vector<AgentPolicyAttachmentRecord>& attachments,
    const std::string& agent_type_id,
    const std::string& policy_family) {
  for (const auto& attachment : attachments) {
    if (attachment.agent_type_id == agent_type_id &&
        attachment.policy_family == policy_family &&
        attachment.active) {
      return &attachment;
    }
  }
  return nullptr;
}

const AgentInstanceRecord* FindPersistedInstance(
    const std::vector<AgentInstanceRecord>& instances,
    const std::string& instance_uuid) {
  for (const auto& instance : instances) {
    if (instance.instance_uuid == instance_uuid) { return &instance; }
  }
  return nullptr;
}

AgentRuntimeStatus PreparePersistedInstanceForOpen(
    AgentInstanceRecord* instance,
    AgentLifecycleMode lifecycle_mode) {
  if (instance == nullptr || lifecycle_mode != AgentLifecycleMode::database_open) {
    return AgentOk();
  }
  return RecoverAgentLifecycleOnOpen(instance);
}

bool ActivationEvidenceValid(const AgentRuntimeActivationEvidence& evidence) {
  return !evidence.database_uuid.empty() &&
         !evidence.engine_instance_uuid.empty() &&
         evidence.policy_generation != 0 &&
         evidence.catalog_generation != 0 &&
         evidence.security_generation != 0 &&
         evidence.filespace_generation != 0 &&
         evidence.tx1_bootstrap_visible &&
         evidence.tx2_activation_committed &&
         evidence.startup_admitted &&
         evidence.health_publication_allowed &&
         evidence.health_publication_persisted;
}

AgentRuntimeContext RuntimeContextFor(const AgentRuntimeActivationEvidence& evidence,
                                      const AgentRuntimeManagerConfig& config) {
  AgentRuntimeContext context;
  context.security_context_present = true;
  context.cluster_authority_available = config.cluster_authority_available;
  context.private_features_available = true;
  context.standalone_edition = config.standalone_edition;
  context.shutdown_requested = config.shutdown_requested;
  context.read_only_mode = config.read_only_mode ||
                           config.shutdown_requested ||
                           evidence.lifecycle_mode == AgentLifecycleMode::database_close ||
                           evidence.lifecycle_mode == AgentLifecycleMode::shutdown ||
                           evidence.lifecycle_mode == AgentLifecycleMode::read_only ||
                           evidence.lifecycle_mode == AgentLifecycleMode::pitr ||
                           evidence.lifecycle_mode == AgentLifecycleMode::clone ||
                           evidence.lifecycle_mode == AgentLifecycleMode::role_change;
  context.maintenance_mode = config.maintenance_mode ||
                             evidence.lifecycle_mode == AgentLifecycleMode::maintenance;
  context.restricted_open_mode = config.restricted_open_mode ||
                                 evidence.lifecycle_mode == AgentLifecycleMode::restricted_open;
  context.repair_mode = config.repair_mode ||
                        evidence.lifecycle_mode == AgentLifecycleMode::repair;
  context.backup_hold_mode = config.backup_hold_mode ||
                             evidence.lifecycle_mode == AgentLifecycleMode::backup ||
                             evidence.lifecycle_mode == AgentLifecycleMode::restore ||
                             evidence.lifecycle_mode == AgentLifecycleMode::pitr ||
                             evidence.lifecycle_mode == AgentLifecycleMode::clone;
  context.archive_hold_mode = config.archive_hold_mode ||
                              evidence.lifecycle_mode == AgentLifecycleMode::archive_hold;
  context.principal_uuid = DeterministicAgentRuntimePrincipalUuidFromKey(
      evidence.database_uuid + "|database_local_agent_runtime_manager|principal|" +
      std::to_string(evidence.policy_generation));
  context.database_uuid = evidence.database_uuid;
  context.trace_tags.push_back("database_local_agent_runtime_manager");
  context.groups.push_back("OPS");
  context.rights.push_back("OBS_AGENT_STATE_READ");
  context.rights.push_back("OBS_AGENT_EVIDENCE_READ");
  context.rights.push_back("OBS_AGENT_CONTROL");
  return context;
}

AgentRuntimeManagerSnapshot BaseSnapshot(const AgentRuntimeActivationEvidence& evidence,
                                         const AgentRuntimeManagerConfig& config) {
  AgentRuntimeManagerSnapshot snapshot;
  snapshot.database_uuid = evidence.database_uuid;
  snapshot.engine_instance_uuid = evidence.engine_instance_uuid;
  snapshot.health_generation = evidence.health_generation == 0 ? 1 : evidence.health_generation;
  snapshot.policy_generation = evidence.policy_generation;
  snapshot.catalog_generation = evidence.catalog_generation;
  snapshot.security_generation = evidence.security_generation;
  snapshot.filespace_generation = evidence.filespace_generation;
  snapshot.agent_set_generation = evidence.agent_set_generation == 0 ? 1 : evidence.agent_set_generation;
  snapshot.cluster_paths_failed_closed = config.standalone_edition || !config.cluster_authority_available;
  if (config.use_explicit_policy_state) {
    for (const auto& policy : config.policy_records) {
      const std::string agent_type_id =
          AgentTypeIdForPolicy(policy, config.policy_attachments);
      snapshot.policy_bootstrap_records.push_back(BootstrapRecordFromPolicy(agent_type_id, policy));
    }
    snapshot.policy_attachments = config.policy_attachments;
  } else {
    InMemoryAgentRuntimeCatalog catalog;
    catalog.BootstrapDatabasePolicies(evidence.database_uuid, snapshot.policy_generation);
    for (const auto& policy : catalog.policies()) {
      const std::string agent_type_id =
          AgentTypeIdForPolicy(policy, catalog.attachments());
      snapshot.policy_bootstrap_records.push_back(BootstrapRecordFromPolicy(agent_type_id, policy));
    }
    snapshot.policy_attachments = catalog.attachments();
  }
  snapshot.status = AgentOk();
  return snapshot;
}

void AddDiagnostic(AgentRuntimeManagerSnapshot* snapshot, std::string diagnostic) {
  if (snapshot != nullptr) {
    snapshot->diagnostics.push_back(std::move(diagnostic));
  }
}

void AddDecision(AgentRuntimeManagerSnapshot* snapshot,
                 std::string agent_type_id,
                 bool database_applicable,
                 bool selected,
                 bool policy_disabled,
                 bool failed_closed,
                 bool cluster_path_failed_closed,
                 std::string diagnostic_code,
                 std::string detail) {
  if (snapshot == nullptr) {
    return;
  }
  AgentRuntimeSelectionDecision decision;
  decision.agent_type_id = std::move(agent_type_id);
  decision.database_applicable = database_applicable;
  decision.selected = selected;
  decision.policy_disabled = policy_disabled;
  decision.failed_closed = failed_closed;
  decision.cluster_path_failed_closed = cluster_path_failed_closed;
  decision.diagnostic_code = std::move(diagnostic_code);
  decision.detail = std::move(detail);
  snapshot->selection_decisions.push_back(std::move(decision));
}

void PublishSupervisionHealth(AgentRuntimeManagerSnapshot* snapshot,
                              const AgentRuntimeManagerConfig& config) {
  if (snapshot == nullptr || !snapshot->status.ok) {
    return;
  }
  for (auto& instance : snapshot->supervised_agents) {
    (void)EnforceAgentSupervisionSafety(&instance);
  }
  if (config.quarantine_required) {
    snapshot->state = AgentRuntimeManagerState::quarantined;
    snapshot->ordinary_admission_allowed = false;
    AddDiagnostic(snapshot, "ENGINE.AGENT_SAFE_MODE_REQUIRED:quarantine_required_by_policy");
    return;
  }

  bool critical_unhealthy = false;
  bool noncritical_unhealthy = false;
  for (const auto& instance : snapshot->supervised_agents) {
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

  if (config.safe_mode_required || critical_unhealthy) {
    snapshot->state = AgentRuntimeManagerState::safe_mode;
    snapshot->ordinary_admission_allowed = false;
    AddDiagnostic(snapshot, config.safe_mode_required
                                ? "ENGINE.AGENT_SAFE_MODE_REQUIRED:safe_mode_required_by_policy"
                                : "ENGINE.AGENT_SAFE_MODE_REQUIRED:critical_database_local_agent_unhealthy");
    return;
  }
  if (noncritical_unhealthy) {
    snapshot->state = config.allow_degraded_service
        ? AgentRuntimeManagerState::degraded
        : AgentRuntimeManagerState::safe_mode;
    snapshot->ordinary_admission_allowed = config.allow_degraded_service;
    AddDiagnostic(snapshot, "ENGINE.AGENT_HEALTH_DEGRADED:noncritical_database_local_agent_unhealthy");
    return;
  }

  snapshot->state = AgentRuntimeManagerState::active;
  snapshot->ordinary_admission_allowed = true;
  AddDiagnostic(snapshot, "ENGINE.AGENT_LIFECYCLE_HEALTHY");
}

}  // namespace

const char* AgentRuntimeManagerStateName(AgentRuntimeManagerState state) {
  switch (state) {
    case AgentRuntimeManagerState::not_started: return "not_started";
    case AgentRuntimeManagerState::starting: return "starting";
    case AgentRuntimeManagerState::active: return "active";
    case AgentRuntimeManagerState::degraded: return "degraded";
    case AgentRuntimeManagerState::safe_mode: return "safe_mode";
    case AgentRuntimeManagerState::draining: return "draining";
    case AgentRuntimeManagerState::stopped: return "stopped";
    case AgentRuntimeManagerState::failed: return "failed";
    case AgentRuntimeManagerState::quarantined: return "quarantined";
  }
  return "failed";
}

AgentLifecycleState AgentLifecycleStateForActivation(AgentActivationProfile activation) {
  switch (activation) {
    case AgentActivationProfile::disabled: return AgentLifecycleState::disabled;
    case AgentActivationProfile::observe_only: return AgentLifecycleState::observe_only;
    case AgentActivationProfile::recommend_only: return AgentLifecycleState::recommend_only;
    case AgentActivationProfile::dry_run: return AgentLifecycleState::dry_run;
    case AgentActivationProfile::live_action: return AgentLifecycleState::running;
  }
  return AgentLifecycleState::failed;
}

AgentRuntimeStatus ApplyAgentLifecycleTransition(AgentInstanceRecord* instance,
                                                 AgentLifecycleState to,
                                                 std::string reason) {
  if (instance == nullptr) {
    return AgentError("SB_AGENT_LIFECYCLE.INSTANCE_REQUIRED", std::move(reason));
  }
  const auto status = ValidateAgentLifecycleTransition(instance->state, to);
  if (!status.ok) {
    return status;
  }
  instance->state = to;
  instance->disabled_by_operator = to == AgentLifecycleState::disabled;
  instance->safe_mode = to == AgentLifecycleState::safe_mode;
  instance->quarantined = to == AgentLifecycleState::quarantined;
  (void)EnforceAgentSupervisionSafety(instance);
  (void)reason;
  return AgentOk();
}

AgentRuntimeStatus RecoverAgentLifecycleOnOpen(AgentInstanceRecord* instance) {
  if (instance == nullptr) {
    return AgentError("SB_AGENT_LIFECYCLE.INSTANCE_REQUIRED", "recover_open");
  }
  switch (instance->state) {
    case AgentLifecycleState::stopping:
    case AgentLifecycleState::stopped:
    case AgentLifecycleState::failed:
      return ApplyAgentLifecycleTransition(instance, AgentLifecycleState::registered, "recover_open");
    case AgentLifecycleState::running:
    case AgentLifecycleState::observe_only:
    case AgentLifecycleState::recommend_only:
    case AgentLifecycleState::dry_run:
      return ApplyAgentLifecycleTransition(instance, AgentLifecycleState::paused, "recover_open");
    case AgentLifecycleState::safe_mode:
    case AgentLifecycleState::quarantined:
    case AgentLifecycleState::disabled:
    case AgentLifecycleState::retired:
    case AgentLifecycleState::registered:
    case AgentLifecycleState::created:
    case AgentLifecycleState::paused:
      return AgentOk();
  }
  return AgentError("SB_AGENT_LIFECYCLE.INVALID_RECOVERY_STATE", instance->instance_uuid);
}

AgentRuntimeManagerSnapshot SelectStandaloneDatabaseLocalAgents(
    const AgentRuntimeActivationEvidence& evidence,
    const AgentRuntimeManagerConfig& config) {
  auto snapshot = BaseSnapshot(evidence, config);
  const auto registry_status = ValidateCanonicalAgentRegistry();
  if (!registry_status.ok) {
    snapshot.status = AgentError("ENGINE.AGENT_RUNTIME_MANAGER_REGISTRY_INVALID",
                                 registry_status.diagnostic_code + ":" + registry_status.detail);
    snapshot.state = AgentRuntimeManagerState::failed;
    AddDiagnostic(&snapshot, snapshot.status.diagnostic_code);
    return snapshot;
  }
  if (!AgentPersistenceUsesScratchBirdStorageAuthority()) {
    snapshot.status = AgentError("ENGINE.AGENT_RUNTIME_MANAGER_AUTHORITY_DENIED",
                                 "agent_persistence_not_scratchbird_storage_authority");
    snapshot.state = AgentRuntimeManagerState::failed;
    AddDiagnostic(&snapshot, snapshot.status.diagnostic_code);
    return snapshot;
  }
  if (!config.dependency_edges.empty()) {
    const auto graph = ValidateAgentDependencyGraph(config.dependency_edges);
    if (!graph.ok) {
      snapshot.status = AgentError("ENGINE.AGENT_RUNTIME_MANAGER_DEPENDENCY_INVALID",
                                   graph.diagnostic_code + ":" + graph.detail);
      snapshot.state = AgentRuntimeManagerState::failed;
      AddDiagnostic(&snapshot, snapshot.status.diagnostic_code);
      return snapshot;
    }
  }

  const auto context = RuntimeContextFor(evidence, config);
  std::vector<AgentPolicy> policies = config.policy_records;
  std::vector<AgentPolicyAttachmentRecord> attachments = config.policy_attachments;
  if (!config.use_explicit_policy_state) {
    InMemoryAgentRuntimeCatalog catalog;
    catalog.BootstrapDatabasePolicies(evidence.database_uuid, snapshot.policy_generation);
    policies = catalog.policies();
    attachments = catalog.attachments();
  }
  for (const auto& descriptor : CanonicalAgentRegistry()) {
    const bool database_applicable = ScopeContainsDatabase(descriptor.scope);
    const bool cluster_only_agent = descriptor.cluster_only ||
                                    descriptor.deployment == AgentDeployment::cluster;
    if (!database_applicable) {
      if (cluster_only_agent) {
        AddDecision(&snapshot, descriptor.type_id, false, false, false, true, true,
                    "ENGINE.AGENT_RUNTIME_MANAGER.CLUSTER_FAIL_CLOSED",
                    "cluster_only_agent_not_database_applicable");
        AddDiagnostic(&snapshot, "ENGINE.AGENT_RUNTIME_MANAGER.CLUSTER_FAIL_CLOSED:" + descriptor.type_id);
        continue;
      }
      AddDecision(&snapshot, descriptor.type_id, false, false, false, false, false,
                  "ENGINE.AGENT_RUNTIME_MANAGER.NOT_DATABASE_APPLICABLE", descriptor.scope);
      continue;
    }

    const bool mixed_cluster_path_agent =
        descriptor.deployment == AgentDeployment::both &&
        config.standalone_edition &&
        (HasClusterMetricDependency(descriptor) || ScopeContainsCluster(descriptor.scope));
    bool has_local_metric_dependency = false;
    for (const auto& dependency : descriptor.metric_dependencies) {
      if (!dependency.cluster_only) {
        has_local_metric_dependency = true;
      }
    }
    const auto availability = EvaluateAgentFeatureAvailability(descriptor, context);
    if (cluster_only_agent ||
        availability == AgentFeatureAvailability::unavailable_cluster_authority ||
        availability == AgentFeatureAvailability::unavailable_edition) {
      AddDecision(&snapshot, descriptor.type_id, true, false, false, true, true,
                  "ENGINE.AGENT_RUNTIME_MANAGER.CLUSTER_FAIL_CLOSED",
                  "cluster_only_agent");
      AddDiagnostic(&snapshot, "ENGINE.AGENT_RUNTIME_MANAGER.CLUSTER_FAIL_CLOSED:" + descriptor.type_id);
      continue;
    }
    if (mixed_cluster_path_agent && !has_local_metric_dependency) {
      AddDecision(&snapshot, descriptor.type_id, true, false, false, true, true,
                  "ENGINE.AGENT_RUNTIME_MANAGER.CLUSTER_FAIL_CLOSED",
                  "cluster_metric_dependency_without_local_projection");
      AddDiagnostic(&snapshot, "ENGINE.AGENT_RUNTIME_MANAGER.CLUSTER_FAIL_CLOSED:" + descriptor.type_id);
      continue;
    }
    if (availability != AgentFeatureAvailability::available) {
      snapshot.status = AgentError("ENGINE.AGENT_RUNTIME_MANAGER_AGENT_UNAVAILABLE",
                                   descriptor.type_id + ":" +
                                       AgentFeatureAvailabilityName(availability));
      snapshot.state = AgentRuntimeManagerState::failed;
      AddDecision(&snapshot, descriptor.type_id, true, false, false, true, false,
                  snapshot.status.diagnostic_code, snapshot.status.detail);
      AddDiagnostic(&snapshot, snapshot.status.diagnostic_code + ":" + descriptor.type_id);
      return snapshot;
    }

    const auto required_families = RequiredPolicyFamiliesForAgent(descriptor);
    std::vector<AgentPolicy> agent_policies;
    bool policy_state_ok = true;
    std::string policy_failure_code;
    std::string policy_failure_detail;
    for (const auto& family : required_families) {
      const AgentPolicy* policy_ptr = FindPolicy(policies, descriptor.type_id, family);
      const AgentPolicyAttachmentRecord* attachment_ptr =
          FindAttachment(attachments, descriptor.type_id, family);
      const auto policy_state = ValidateAgentPolicyStateForMutation(
          policy_ptr, attachment_ptr, descriptor, snapshot.policy_generation);
      if (!policy_state.ok) {
        policy_state_ok = false;
        policy_failure_code = policy_state.diagnostic_code;
        policy_failure_detail = policy_state.detail;
        break;
      }
      agent_policies.push_back(*policy_ptr);
    }
    if (!policy_state_ok || agent_policies.empty()) {
      AddDecision(&snapshot, descriptor.type_id, true, false, false, true, false,
                  policy_failure_code.empty() ? "SB_AGENT_POLICY.MISSING" : policy_failure_code,
                  policy_failure_detail);
      AddDiagnostic(&snapshot,
                    (policy_failure_code.empty() ? "SB_AGENT_POLICY.MISSING" : policy_failure_code) +
                        ":" + descriptor.type_id);
      continue;
    }

    auto policy = agent_policies.front();
    policy.activation = EffectiveActivationForLifecycle(policy.activation, evidence.lifecycle_mode);
    if (!policy.enabled || policy.activation == AgentActivationProfile::disabled) {
      AddDecision(&snapshot, descriptor.type_id, true, false, true, false,
                  mixed_cluster_path_agent,
                  "ENGINE.AGENT_RUNTIME_MANAGER.POLICY_DISABLED", policy.policy_uuid);
      continue;
    }
    const auto policy_status = ValidateAgentPolicy(policy, descriptor);
    if (!policy_status.ok) {
      AddDecision(&snapshot, descriptor.type_id, true, false, false, true, false,
                  policy_status.diagnostic_code, policy_status.detail);
      AddDiagnostic(&snapshot, policy_status.diagnostic_code + ":" + descriptor.type_id);
      continue;
    }

    const std::string instance_uuid = DeterministicAgentInstanceUuid(
        evidence.database_uuid, descriptor.type_id, descriptor.scope, snapshot.policy_generation);
    const AgentInstanceRecord* persisted = FindPersistedInstance(config.persisted_instances, instance_uuid);
    if (persisted != nullptr && persisted->state == AgentLifecycleState::retired) {
      if (persisted->retirement_evidence_uuid.empty()) {
        AddDecision(&snapshot, descriptor.type_id, true, false, false, true, false,
                    "SB_AGENT_INSTANCE.RETIREMENT_EVIDENCE_REQUIRED", instance_uuid);
        AddDiagnostic(&snapshot,
                      "SB_AGENT_INSTANCE.RETIREMENT_EVIDENCE_REQUIRED:" + descriptor.type_id);
      } else {
        snapshot.supervised_agents.push_back(*persisted);
        AddDecision(&snapshot, descriptor.type_id, true, false, true, false,
                    mixed_cluster_path_agent,
                    "ENGINE.AGENT_RUNTIME_MANAGER.INSTANCE_RETIRED",
                    persisted->retirement_evidence_uuid);
      }
      continue;
    }

    AgentInstanceRecord instance;
    instance.instance_uuid = instance_uuid;
    instance.agent_type_id = descriptor.type_id;
    instance.policy_uuid = policy.policy_uuid;
    instance.scope = descriptor.scope;
    instance.state = AgentLifecycleStateForActivation(policy.activation);
    instance.policy_generation = policy.policy_generation;
    instance.instance_generation = snapshot.agent_set_generation;
    instance.run_generation = snapshot.manager_generation;
    if (persisted != nullptr) {
      instance = *persisted;
      instance.policy_uuid = policy.policy_uuid;
      instance.policy_generation = policy.policy_generation;
      const auto recover_status = PreparePersistedInstanceForOpen(&instance, evidence.lifecycle_mode);
      if (!recover_status.ok) {
        snapshot.status = recover_status;
        snapshot.state = AgentRuntimeManagerState::failed;
        AddDecision(&snapshot, descriptor.type_id, true, false, false, true, false,
                    recover_status.diagnostic_code, recover_status.detail);
        AddDiagnostic(&snapshot, recover_status.diagnostic_code + ":" + descriptor.type_id);
        return snapshot;
      }
    }
    if (Contains(config.quarantined_agent_type_ids, descriptor.type_id)) {
      instance.state = AgentLifecycleState::quarantined;
      instance.quarantined = true;
    } else if (Contains(config.unhealthy_agent_type_ids, descriptor.type_id)) {
      instance.state = AgentLifecycleState::failed;
      ++instance.crash_loop_count;
    }
    snapshot.supervised_agents.push_back(std::move(instance));
    AddDecision(&snapshot, descriptor.type_id, true, true, false, false,
                mixed_cluster_path_agent,
                mixed_cluster_path_agent
                    ? "ENGINE.AGENT_RUNTIME_MANAGER.LOCAL_PROJECTION_SELECTED_CLUSTER_PATH_FAIL_CLOSED"
                    : "ENGINE.AGENT_RUNTIME_MANAGER.SELECTED",
                mixed_cluster_path_agent
                    ? policy.policy_uuid + ":cluster_metric_dependency_fail_closed"
                    : policy.policy_uuid);
    if (mixed_cluster_path_agent) {
      AddDiagnostic(&snapshot,
                    "ENGINE.AGENT_RUNTIME_MANAGER.LOCAL_PROJECTION_SELECTED_CLUSTER_PATH_FAIL_CLOSED:" +
                        descriptor.type_id);
    }
  }

  snapshot.status = AgentOk();
  return snapshot;
}

AgentRuntimeManagerSnapshot DatabaseLocalAgentRuntimeManager::Start(
    const AgentRuntimeActivationEvidence& evidence,
    const AgentRuntimeManagerConfig& config) {
  snapshot_ = BaseSnapshot(evidence, config);
  snapshot_.state = AgentRuntimeManagerState::starting;
  snapshot_.manager_generation += 1;

  if (!ActivationEvidenceValid(evidence)) {
    snapshot_.state = AgentRuntimeManagerState::not_started;
    snapshot_.status = AgentError("ENGINE.AGENT_RUNTIME_MANAGER_ACTIVATION_REQUIRED",
                                  "tx1_tx2_startup_generations_and_health_publication_required");
    AddDiagnostic(&snapshot_, snapshot_.status.diagnostic_code);
    return snapshot_;
  }
  if (!evidence.dependency_graph_consistent) {
    snapshot_.state = AgentRuntimeManagerState::failed;
    snapshot_.status = AgentError("ENGINE.AGENT_RUNTIME_MANAGER_DEPENDENCY_INVALID",
                                  "agent_dependency_graph_inconsistent");
    AddDiagnostic(&snapshot_, snapshot_.status.diagnostic_code);
    return snapshot_;
  }
  if (config.enforce_dependency_lifecycle) {
    const auto dependency_decision =
        EvaluateAgentDependencyLifecycle(config.dependency_lifecycle_request);
    if (!dependency_decision.status.ok) {
      snapshot_.state = AgentRuntimeManagerState::failed;
      snapshot_.status = AgentError(
          "ENGINE.AGENT_RUNTIME_MANAGER_DEPENDENCY_LIFECYCLE_REFUSED",
          dependency_decision.status.diagnostic_code + ":" +
              dependency_decision.status.detail);
      AddDiagnostic(&snapshot_, snapshot_.status.diagnostic_code);
      for (const auto& diagnostic : dependency_decision.diagnostics) {
        AddDiagnostic(&snapshot_, diagnostic);
      }
      return snapshot_;
    }
    for (const auto& diagnostic : dependency_decision.diagnostics) {
      AddDiagnostic(&snapshot_, diagnostic);
    }
  }

  const auto open_state_diagnostic = OpenStateModeDiagnostic(evidence, config);
  if (!open_state_diagnostic.empty()) {
    snapshot_.activation_evidence_accepted = true;
    snapshot_.ordinary_admission_allowed = false;
    snapshot_.shutdown_coordination_complete =
        open_state_diagnostic == "ENGINE.AGENT_OPEN_STATE.SHUTDOWN_DRAIN_ONLY" &&
        config.graceful_shutdown;
    snapshot_.state = snapshot_.shutdown_coordination_complete
        ? AgentRuntimeManagerState::stopped
        : AgentRuntimeManagerState::safe_mode;
    snapshot_.status = AgentOk();
    AddDiagnostic(&snapshot_, open_state_diagnostic);
    return snapshot_;
  }

  snapshot_ = SelectStandaloneDatabaseLocalAgents(evidence, config);
  snapshot_.manager_generation += 1;
  snapshot_.activation_evidence_accepted = true;
  PublishSupervisionHealth(&snapshot_, config);
  if (OpenStateSuppressesNormalLoops(evidence, config)) {
    snapshot_.state = AgentRuntimeManagerState::safe_mode;
    snapshot_.ordinary_admission_allowed = false;
    AddDiagnostic(&snapshot_, "ENGINE.AGENT_OPEN_STATE.SAFE_ADMISSION_ONLY");
  }
  return snapshot_;
}

AgentRuntimeManagerSnapshot DatabaseLocalAgentRuntimeManager::PublishHealth(
    const AgentRuntimeManagerConfig& config) {
  snapshot_.health_generation += 1;
  PublishSupervisionHealth(&snapshot_, config);
  return snapshot_;
}

AgentRuntimeManagerSnapshot DatabaseLocalAgentRuntimeManager::Drain(
    const AgentRuntimeActivationEvidence& evidence,
    const AgentRuntimeManagerConfig& config) {
  snapshot_.database_uuid = evidence.database_uuid.empty() ? snapshot_.database_uuid : evidence.database_uuid;
  snapshot_.engine_instance_uuid = evidence.engine_instance_uuid.empty()
      ? snapshot_.engine_instance_uuid
      : evidence.engine_instance_uuid;
  snapshot_.health_generation = evidence.health_generation == 0
      ? snapshot_.health_generation + 1
      : evidence.health_generation;
  snapshot_.ordinary_admission_allowed = false;
  snapshot_.shutdown_coordination_complete = config.shutdown_requested && config.graceful_shutdown;
  snapshot_.state = snapshot_.shutdown_coordination_complete
      ? AgentRuntimeManagerState::stopped
      : AgentRuntimeManagerState::draining;
  for (auto& instance : snapshot_.supervised_agents) {
    if (AgentLifecycleTransitionAllowed(instance.state, AgentLifecycleState::stopping)) {
      (void)ApplyAgentLifecycleTransition(&instance, AgentLifecycleState::stopping, "drain");
    }
    if (snapshot_.shutdown_coordination_complete &&
        AgentLifecycleTransitionAllowed(instance.state, AgentLifecycleState::stopped)) {
      (void)ApplyAgentLifecycleTransition(&instance, AgentLifecycleState::stopped, "drain_complete");
    }
  }
  snapshot_.status = AgentOk();
  snapshot_.diagnostics.clear();
  AddDiagnostic(&snapshot_, snapshot_.shutdown_coordination_complete
                                ? "ENGINE.AGENT_LIFECYCLE_STOPPED"
                                : "ENGINE.AGENT_LIFECYCLE_DRAINING");
  return snapshot_;
}

AgentRuntimeManagerSnapshot DatabaseLocalAgentRuntimeManager::RecoverOpen(
    const AgentRuntimeActivationEvidence& evidence,
    const AgentRuntimeManagerConfig& config) {
  for (auto& instance : snapshot_.supervised_agents) {
    const auto status = RecoverAgentLifecycleOnOpen(&instance);
    if (!status.ok) {
      snapshot_.state = AgentRuntimeManagerState::failed;
      snapshot_.status = status;
      AddDiagnostic(&snapshot_, status.diagnostic_code + ":" + instance.agent_type_id);
      return snapshot_;
    }
  }
  return Start(evidence, config);
}

}  // namespace scratchbird::core::agents

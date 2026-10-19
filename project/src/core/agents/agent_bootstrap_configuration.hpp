// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "agent_engine_lifecycle.hpp"
#include "../catalog/catalog_agent_bootstrap.hpp"
#include <string>
#include <utility>
#include <vector>

namespace scratchbird::core::agents {
// Installation declarations only. The construction owner issues all object
// identities and publishes them in its creation transaction. These defaults
// cannot create a service session, activate a worker or authorize engine work.
struct AgentBootstrapDeclaration {
  catalog::AgentBootstrapDefinitionKind kind;
  std::string agent_type, properties;
  std::size_t primary_policy_index;
};
inline std::string AgentBootstrapProperties(
    const std::vector<std::pair<std::string, std::string>>& fields) {
  std::string result;
  for (const auto& [key, value] : fields) {
    result += key; result += '=';
    for (const char byte : value) result += (byte == '\n' || byte == '\r' || byte == '|') ? ' ' : byte;
    result += '\n';
  }
  return result;
}
// Shared by embedded creation and native catalog construction. Required
// families come from the canonical registry; cluster entries remain installed
// and disabled until separately admitted enrollment.
inline std::vector<AgentBootstrapDeclaration> DefaultAgentBootstrapDeclarations() {
  std::vector<AgentBootstrapDeclaration> result;
  auto configured_agents = CanonicalAgentRegistry();
  const auto lifecycle_agents = RuntimeLifecycleAgentDescriptors();
  configured_agents.insert(configured_agents.end(), lifecycle_agents.begin(), lifecycle_agents.end());
  for (const auto& agent : configured_agents) {
    const auto lifecycle_family = RuntimeLifecycleAgentPolicyFamily(agent.type_id);
    const bool lifecycle = !lifecycle_family.empty();
    const auto families = lifecycle ? std::vector<std::string>{lifecycle_family}
                                   : RequiredPolicyFamiliesForAgent(agent);
    std::string policy_families;
    std::size_t primary_policy_index = result.size();
    bool first_policy = true;
    bool enabled = false;
    for (const auto& family : families) {
      auto policy = lifecycle ? BaselineRuntimeLifecycleAgentPolicy(agent)
                              : BaselinePolicyForAgentFamily(agent, family, 1);
      // Cluster configuration is present for later enrollment, but cannot run
      // on a newly created standalone database.
      if (agent.cluster_only) {
        policy.enabled = false;
        policy.activation = AgentActivationProfile::disabled;
        policy.action_mode = "disabled";
      }
      const auto policy_index = result.size();
      if (first_policy) {
        primary_policy_index = policy_index;
        enabled = policy.enabled;
        first_policy = false;
      }
      if (!policy_families.empty()) policy_families += ",";
      policy_families += family;
      std::string metrics;
      for (const auto& metric : policy.required_metric_families) {
        if (!metrics.empty()) metrics += ",";
        metrics += metric;
      }
      std::string dependencies;
      for (const auto& dependency : policy.policy_dependencies) {
        if (!dependencies.empty()) dependencies += ",";
        dependencies += dependency;
      }
      std::vector<std::pair<std::string, std::string>> fields = {
          {"policy_name", policy.policy_name},
          {"policy_class", lifecycle ? "runtime_lifecycle_agent" : "operational_agent"},
          {"policy_family", policy.policy_family},
          {"policy_generation", std::to_string(policy.policy_generation)},
          {"agent_type", agent.type_id},
          {"layer", AgentRuntimeLayerName(agent.layer)},
          {"deployment", AgentDeploymentName(agent.deployment)},
          {"scope", agent.scope},
          {"authority", AgentAuthorityClassName(agent.authority)},
          {"enabled", policy.enabled ? "1" : "0"},
          {"activation", AgentActivationProfileName(policy.activation)},
          {"action_mode", policy.action_mode},
          {"invalid_policy_behavior", policy.invalid_policy_behavior},
          {"allow_live_action", policy.allow_live_action ? "1" : "0"},
          {"require_manual_approval", policy.require_manual_approval ? "1" : "0"},
          {"require_dry_run_before_live", policy.require_dry_run_before_live ? "1" : "0"},
          {"evidence_required", policy.evidence_required ? "1" : "0"},
          {"explainability_required", policy.explainability_required ? "1" : "0"},
          {"run_interval_microseconds", std::to_string(policy.run_interval_microseconds)},
          {"jitter_microseconds", std::to_string(policy.jitter_microseconds)},
          {"lease_microseconds", std::to_string(policy.lease_microseconds)},
          {"cooldown_microseconds", std::to_string(policy.cooldown_microseconds)},
          {"max_runtime_microseconds", std::to_string(policy.max_runtime_microseconds)},
          {"max_restart_attempts", std::to_string(policy.max_restart_attempts)},
          {"initial_backoff_microseconds", std::to_string(policy.initial_backoff_microseconds)},
          {"max_backoff_microseconds", std::to_string(policy.max_backoff_microseconds)},
          {"max_history_query_rows", std::to_string(policy.max_history_query_rows)},
          {"max_evidence_fanout", std::to_string(policy.max_evidence_fanout)},
          {"max_label_cardinality", std::to_string(policy.max_label_cardinality)},
          {"action_budget_per_window", std::to_string(policy.action_budget_per_window)},
          {"required_metric_families", metrics},
          {"policy_dependencies", dependencies},
          {"editable_seed_policy", "1"}};
      for (const auto& [key, value] : policy.config_fields)
        fields.emplace_back("config." + key, value);
      result.push_back({catalog::AgentBootstrapDefinitionKind::policy, agent.type_id,
                        AgentBootstrapProperties(fields), policy_index});
    }
    const std::string trigger = agent.type_id == "agent_coordinator" ? "server_open"
        : agent.type_id == "startup_agent" ? "coordinator_startup"
        : agent.type_id == "shutdown_agent" ? "coordinator_shutdown" : "policy_schedule";
    result.push_back({catalog::AgentBootstrapDefinitionKind::registration, agent.type_id,
        AgentBootstrapProperties({
            {"profile_name", agent.type_id + "_registration"},
            {"profile_class", "agent_registration"},
            {"agent_type", agent.type_id},
            {"policy_families", policy_families},
            {"state", enabled ? "registered" : "disabled"},
            {"managed_by", agent.type_id == "agent_coordinator" ? "ipc_server" : "agent_coordinator"},
            {"launch_trigger", trigger},
            {"starts_during_create", "0"}}), primary_policy_index});
  }
  return result;
}
} // namespace scratchbird::core::agents

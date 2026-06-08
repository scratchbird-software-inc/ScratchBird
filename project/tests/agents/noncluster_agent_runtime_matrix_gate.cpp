// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "agent_runtime_manager.hpp"

#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>

namespace {

namespace agents = scratchbird::core::agents;

[[noreturn]] void Fail(const std::string& message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, const std::string& message) {
  if (!condition) {
    Fail(message);
  }
}

bool ScopeContainsDatabase(const std::string& scope) {
  return scope.find("database") != std::string::npos;
}

bool HasClusterMetricDependency(const agents::AgentTypeDescriptor& descriptor) {
  for (const auto& dependency : descriptor.metric_dependencies) {
    if (dependency.cluster_only) {
      return true;
    }
  }
  return false;
}

bool HasLocalMetricDependency(const agents::AgentTypeDescriptor& descriptor) {
  for (const auto& dependency : descriptor.metric_dependencies) {
    if (!dependency.cluster_only) {
      return true;
    }
  }
  return false;
}

const agents::AgentRuntimeSelectionDecision* FindDecision(
    const agents::AgentRuntimeManagerSnapshot& snapshot,
    std::string_view type_id) {
  for (const auto& decision : snapshot.selection_decisions) {
    if (decision.agent_type_id == type_id) {
      return &decision;
    }
  }
  return nullptr;
}

agents::AgentRuntimeActivationEvidence ValidEvidence() {
  agents::AgentRuntimeActivationEvidence evidence;
  evidence.database_uuid = "019e0f2a-002a-7000-8000-000000000002";
  evidence.engine_instance_uuid = "engine-instance:019e0f2a-002a";
  evidence.lifecycle_mode = agents::AgentLifecycleMode::database_open;
  evidence.policy_generation = 1;
  evidence.catalog_generation = 1;
  evidence.security_generation = 1;
  evidence.filespace_generation = 1;
  evidence.agent_set_generation = 1;
  evidence.health_generation = 1;
  evidence.tx1_bootstrap_visible = true;
  evidence.tx2_activation_committed = true;
  evidence.startup_admitted = true;
  evidence.health_publication_allowed = true;
  evidence.health_publication_persisted = true;
  return evidence;
}

void TestStandaloneSelectionMatrix() {
  agents::AgentRuntimeManagerConfig config;
  config.standalone_edition = true;
  config.cluster_authority_available = false;

  const auto snapshot = agents::SelectStandaloneDatabaseLocalAgents(ValidEvidence(), config);
  Require(snapshot.status.ok, "standalone runtime selection failed: " + snapshot.status.diagnostic_code);
  Require(!snapshot.selection_decisions.empty(), "standalone runtime selection published no decisions");

  int selected_database_agents = 0;
  int failed_closed_cluster_agents = 0;
  for (const auto& descriptor : agents::CanonicalAgentRegistry()) {
    const auto* decision = FindDecision(snapshot, descriptor.type_id);
    Require(decision != nullptr, "missing selection decision for " + descriptor.type_id);

    const bool database_applicable = ScopeContainsDatabase(descriptor.scope);
    Require(decision->database_applicable == database_applicable,
            "database applicability mismatch for " + descriptor.type_id);
    if (!database_applicable) {
      Require(!decision->selected, "non-database agent was selected: " + descriptor.type_id);
      if (descriptor.cluster_only || descriptor.deployment == agents::AgentDeployment::cluster) {
        Require(decision->failed_closed,
                "cluster-only non-database agent did not fail closed: " + descriptor.type_id);
        Require(decision->cluster_path_failed_closed,
                "cluster-only non-database agent did not mark cluster path failed closed: " +
                    descriptor.type_id);
        Require(decision->diagnostic_code == "ENGINE.AGENT_RUNTIME_MANAGER.CLUSTER_FAIL_CLOSED",
                "cluster-only non-database diagnostic mismatch for " + descriptor.type_id);
        ++failed_closed_cluster_agents;
      }
      continue;
    }

    const bool cluster_dependent = descriptor.cluster_only ||
                                   descriptor.deployment == agents::AgentDeployment::cluster;
    const bool mixed_both_with_local_projection =
        descriptor.deployment == agents::AgentDeployment::both &&
        HasClusterMetricDependency(descriptor) &&
        HasLocalMetricDependency(descriptor);
    if (cluster_dependent) {
      Require(!decision->selected, "cluster-dependent agent selected in standalone mode: " + descriptor.type_id);
      Require(decision->failed_closed,
              "cluster-dependent agent did not fail closed in standalone mode: " + descriptor.type_id);
      Require(decision->cluster_path_failed_closed,
              "cluster-dependent agent did not mark cluster path failed closed: " + descriptor.type_id);
      Require(decision->diagnostic_code == "ENGINE.AGENT_RUNTIME_MANAGER.CLUSTER_FAIL_CLOSED",
              "cluster-dependent diagnostic mismatch for " + descriptor.type_id);
      ++failed_closed_cluster_agents;
      continue;
    }
    if (mixed_both_with_local_projection) {
      Require(decision->selected,
              "mixed both-agent local projection was not selected: " + descriptor.type_id);
      Require(!decision->failed_closed,
              "mixed both-agent was globally failed closed instead of local-projected: " + descriptor.type_id);
      Require(decision->cluster_path_failed_closed,
              "mixed both-agent did not mark cluster path failed closed: " + descriptor.type_id);
      Require(decision->diagnostic_code ==
                  "ENGINE.AGENT_RUNTIME_MANAGER.LOCAL_PROJECTION_SELECTED_CLUSTER_PATH_FAIL_CLOSED",
              "mixed both-agent diagnostic mismatch for " + descriptor.type_id);
      ++selected_database_agents;
      continue;
    }
    if (descriptor.deployment == agents::AgentDeployment::both &&
        HasClusterMetricDependency(descriptor) &&
        !HasLocalMetricDependency(descriptor)) {
      Require(!decision->selected,
              "mixed both-agent without local metrics was selected: " + descriptor.type_id);
      Require(decision->failed_closed,
              "mixed both-agent without local metrics did not fail closed: " + descriptor.type_id);
      ++failed_closed_cluster_agents;
      continue;
    }

    Require(decision->selected || decision->policy_disabled,
            "database-local non-cluster agent neither selected nor policy-disabled: " + descriptor.type_id);
    if (decision->selected) {
      ++selected_database_agents;
    }
  }

  Require(selected_database_agents > 0, "no database-local non-cluster agents were selected");
  Require(failed_closed_cluster_agents > 0, "no cluster-dependent agents were proven fail-closed");

  const auto* admission = FindDecision(snapshot, "admission_control_manager");
  Require(admission != nullptr, "missing admission_control_manager selection decision");
  Require(admission->selected, "admission_control_manager local projection was not selected");
  Require(!admission->failed_closed,
          "admission_control_manager was globally failed closed instead of local-projected");
  Require(admission->cluster_path_failed_closed,
          "admission_control_manager cluster path was not failed closed");
}

}  // namespace

int main() {
  TestStandaloneSelectionMatrix();
  return EXIT_SUCCESS;
}

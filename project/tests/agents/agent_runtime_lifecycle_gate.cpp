// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "agent_engine_lifecycle.hpp"
#include "agent_runtime_manager.hpp"

#include <cstdlib>
#include <iostream>
#include <string_view>

namespace {

namespace agents = scratchbird::core::agents;

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view message) {
  if (!condition) {
    Fail(message);
  }
}

agents::AgentRuntimeActivationEvidence ValidEvidence() {
  agents::AgentRuntimeActivationEvidence evidence;
  evidence.database_uuid = "019e0f2a-0020-7000-8000-000000000002";
  evidence.engine_instance_uuid = "engine-instance:019e0f2a-0020";
  evidence.lifecycle_mode = agents::AgentLifecycleMode::database_open;
  evidence.policy_generation = 2;
  evidence.catalog_generation = 3;
  evidence.security_generation = 4;
  evidence.filespace_generation = 5;
  evidence.agent_set_generation = 6;
  evidence.health_generation = 7;
  evidence.tx1_bootstrap_visible = true;
  evidence.tx2_activation_committed = true;
  evidence.startup_admitted = true;
  evidence.health_publication_allowed = true;
  evidence.health_publication_persisted = true;
  evidence.dependency_graph_consistent = true;
  return evidence;
}

bool HasAgent(const agents::AgentRuntimeManagerSnapshot& snapshot, std::string_view type_id) {
  for (const auto& instance : snapshot.supervised_agents) {
    if (instance.agent_type_id == type_id) {
      return true;
    }
  }
  return false;
}

bool HasDiagnostic(const agents::AgentRuntimeManagerSnapshot& snapshot, std::string_view diagnostic) {
  for (const auto& item : snapshot.diagnostics) {
    if (item.find(diagnostic) != std::string::npos) {
      return true;
    }
  }
  return false;
}

void TestRuntimeStartsOnlyAfterTx2Evidence() {
  agents::DatabaseLocalAgentRuntimeManager manager;
  auto evidence = ValidEvidence();
  evidence.tx2_activation_committed = false;
  const auto snapshot = manager.Start(evidence, {});
  Require(!snapshot.status.ok, "runtime manager accepted missing TX2 activation evidence");
  Require(snapshot.state == agents::AgentRuntimeManagerState::not_started,
          "runtime manager did not remain not_started without TX2");
  Require(snapshot.supervised_agents.empty(),
          "runtime manager supervised agents before activation evidence");
}

void TestRuntimeStartHealthAndDrain() {
  agents::DatabaseLocalAgentRuntimeManager manager;
  agents::AgentRuntimeManagerConfig config;
  config.standalone_edition = true;
  config.cluster_authority_available = false;

  const auto started = manager.Start(ValidEvidence(), config);
  Require(started.status.ok, "runtime manager start failed");
  Require(started.activation_evidence_accepted,
          "runtime manager did not record accepted activation evidence");
  Require(started.state == agents::AgentRuntimeManagerState::active,
          "runtime manager did not become active");
  Require(started.ordinary_admission_allowed,
          "active runtime manager did not allow ordinary admission");
  Require(started.manager_generation > 0, "runtime manager did not advance generation");
  Require(started.health_generation == 7, "runtime manager did not publish health generation");
  Require(HasAgent(started, "storage_health_manager"),
          "runtime manager did not supervise storage_health_manager");
  Require(!HasAgent(started, "cluster_autoscale_manager"),
          "runtime manager supervised cluster-only agent in standalone mode");
  Require(started.cluster_paths_failed_closed,
          "runtime manager did not record cluster paths failed closed");
  Require(HasDiagnostic(started, "ENGINE.AGENT_LIFECYCLE_HEALTHY"),
          "runtime manager did not publish healthy diagnostic");

  auto drain_evidence = ValidEvidence();
  drain_evidence.health_generation = started.health_generation + 1;
  config.shutdown_requested = false;
  const auto draining = manager.Drain(drain_evidence, config);
  Require(draining.state == agents::AgentRuntimeManagerState::draining,
          "runtime manager did not enter draining state");
  Require(!draining.ordinary_admission_allowed,
          "draining runtime manager admitted ordinary work");

  config.shutdown_requested = true;
  const auto stopped = manager.Drain(drain_evidence, config);
  Require(stopped.state == agents::AgentRuntimeManagerState::stopped,
          "runtime manager did not stop after graceful shutdown");
  Require(stopped.shutdown_coordination_complete,
          "runtime manager did not publish shutdown coordination complete");
}

void TestLifecycleWrapperUsesRuntimeManager() {
  agents::DatabaseEngineAgentInput input;
  input.database_uuid = ValidEvidence().database_uuid;
  input.engine_instance_uuid = ValidEvidence().engine_instance_uuid;
  input.database_lifecycle_state = "opened";
  input.lifecycle_mode = agents::AgentLifecycleMode::database_open;
  input.policy_generation = 2;
  input.catalog_generation = 3;
  input.security_generation = 4;
  input.filespace_generation = 5;
  input.agent_set_generation = 6;
  input.health_generation = 7;
  input.tx1_bootstrap_visible = true;
  input.tx2_activation_committed = true;
  input.startup_admitted = true;
  input.health_publication_allowed = true;
  input.health_publication_persisted = true;

  const auto result = agents::StartDatabaseEngineLifecycleAgent(input);
  Require(result.ok(), "database lifecycle wrapper failed runtime manager start");
  Require(result.health.agent_state == agents::DatabaseEngineAgentLifecycleState::active,
          "database lifecycle wrapper did not publish active runtime state");
  Require(result.health.ordinary_admission_allowed,
          "database lifecycle wrapper did not publish ordinary admission");
}

}  // namespace

int main() {
  TestRuntimeStartsOnlyAfterTx2Evidence();
  TestRuntimeStartHealthAndDrain();
  TestLifecycleWrapperUsesRuntimeManager();
  return EXIT_SUCCESS;
}

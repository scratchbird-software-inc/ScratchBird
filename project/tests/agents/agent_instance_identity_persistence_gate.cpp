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

namespace {

namespace agents = scratchbird::core::agents;

[[noreturn]] void Fail(const std::string& message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, const std::string& message) {
  if (!condition) { Fail(message); }
}

const agents::AgentInstanceRecord* FindInstance(
    const std::vector<agents::AgentInstanceRecord>& instances,
    const std::string& type_id) {
  for (const auto& instance : instances) {
    if (instance.agent_type_id == type_id) { return &instance; }
  }
  return nullptr;
}

const agents::AgentRuntimeSelectionDecision* FindDecision(
    const agents::AgentRuntimeManagerSnapshot& snapshot,
    const std::string& type_id) {
  for (const auto& decision : snapshot.selection_decisions) {
    if (decision.agent_type_id == type_id) { return &decision; }
  }
  return nullptr;
}

agents::AgentRuntimeActivationEvidence Evidence(unsigned generation) {
  agents::AgentRuntimeActivationEvidence evidence;
  evidence.database_uuid = "019e0f2a-003a-7000-8000-000000000002";
  evidence.engine_instance_uuid = "engine-instance:019e0f2a-003a";
  evidence.lifecycle_mode = agents::AgentLifecycleMode::database_open;
  evidence.policy_generation = generation;
  evidence.catalog_generation = 1;
  evidence.security_generation = 1;
  evidence.filespace_generation = 1;
  evidence.agent_set_generation = generation;
  evidence.health_generation = 1;
  evidence.tx1_bootstrap_visible = true;
  evidence.tx2_activation_committed = true;
  evidence.startup_admitted = true;
  evidence.health_publication_allowed = true;
  evidence.health_publication_persisted = true;
  return evidence;
}

agents::AgentRuntimeManagerConfig ConfigWithCatalog(unsigned generation) {
  agents::InMemoryAgentRuntimeCatalog catalog;
  catalog.BootstrapDatabasePolicies(Evidence(generation).database_uuid, generation);

  agents::AgentRuntimeManagerConfig config;
  config.standalone_edition = true;
  config.cluster_authority_available = false;
  config.use_explicit_policy_state = true;
  config.policy_records = catalog.policies();
  config.policy_attachments = catalog.attachments();
  return config;
}

void TestDeterministicIdentityStableByDatabaseAgentScopeGeneration() {
  const auto descriptor = agents::FindAgentType("page_allocation_manager");
  Require(descriptor.has_value(), "page_allocation_manager descriptor missing");
  const auto first = agents::DeterministicAgentInstanceUuid(
      Evidence(11).database_uuid, descriptor->type_id, descriptor->scope, 11);
  const auto second = agents::DeterministicAgentInstanceUuid(
      Evidence(11).database_uuid, descriptor->type_id, descriptor->scope, 11);
  const auto next_generation = agents::DeterministicAgentInstanceUuid(
      Evidence(11).database_uuid, descriptor->type_id, descriptor->scope, 12);
  Require(first == second, "deterministic instance UUID changed for same identity tuple");
  Require(first != next_generation, "instance UUID did not change across policy generation");
  Require(first.size() == 36 && first[8] == '-' && first[13] == '-' &&
              first[18] == '-' && first[23] == '-',
          "deterministic instance ID is not UUID-shaped");
}

void TestStateAndPolicyGenerationSerializeRestore() {
  agents::AgentInstanceRecord instance;
  instance.instance_uuid = "019e0f2a-003a-7000-8000-0000000000aa";
  instance.agent_type_id = "storage_health_manager";
  instance.policy_uuid = "policy:storage_health_manager:storage_health_policy:baseline";
  instance.scope = "node/database/filespace";
  instance.state = agents::AgentLifecycleState::recommend_only;
  instance.policy_generation = 11;
  instance.instance_generation = 11;
  instance.run_generation = 3;
  instance.lease_until_microseconds = 44;

  const auto encoded = agents::SerializeAgentInstanceRecord(instance);
  agents::AgentInstanceRecord restored;
  const auto status = agents::RestoreAgentInstanceRecord(encoded, &restored);
  Require(status.ok, "instance restore failed: " + status.diagnostic_code);
  Require(restored.instance_uuid == instance.instance_uuid, "instance UUID did not restore");
  Require(restored.state == instance.state, "instance state did not restore");
  Require(restored.policy_generation == 11, "policy generation did not restore");
  Require(restored.instance_generation == 11, "instance generation did not restore");
}

void TestManagerReusesPersistedIdentityAndState() {
  auto config = ConfigWithCatalog(11);
  agents::DatabaseLocalAgentRuntimeManager manager;
  const auto first = manager.Start(Evidence(11), config);
  Require(first.status.ok, "initial manager start failed");
  const auto* first_page = FindInstance(first.supervised_agents, "page_allocation_manager");
  Require(first_page != nullptr, "page_allocation_manager instance missing on first start");

  agents::InMemoryAgentRuntimeCatalog persistence;
  auto save_status = persistence.SaveInstances(first.supervised_agents);
  Require(save_status.ok, "initial instance persistence failed");

  auto persisted_instances = persistence.instances();
  for (auto& instance : persisted_instances) {
    if (instance.agent_type_id == "page_allocation_manager") {
      instance.state = agents::AgentLifecycleState::paused;
      instance.run_generation = 99;
    }
  }
  config.persisted_instances = persisted_instances;

  agents::DatabaseLocalAgentRuntimeManager reopened;
  const auto second = reopened.Start(Evidence(11), config);
  Require(second.status.ok, "manager reopen with persisted instances failed");
  const auto* second_page = FindInstance(second.supervised_agents, "page_allocation_manager");
  Require(second_page != nullptr, "page_allocation_manager instance missing after reopen");
  Require(second_page->instance_uuid == first_page->instance_uuid,
          "persisted instance UUID was not reused");
  Require(second_page->state == agents::AgentLifecycleState::paused,
          "persisted instance state did not survive reopen");
  Require(second_page->policy_generation == 11,
          "persisted instance policy generation did not survive reopen");
  Require(second_page->run_generation == 99,
          "persisted run generation did not survive reopen");
}

void TestRetiredInstanceRequiresEvidenceAndDoesNotRestartNormally() {
  auto config = ConfigWithCatalog(11);
  agents::DatabaseLocalAgentRuntimeManager manager;
  const auto first = manager.Start(Evidence(11), config);
  Require(first.status.ok, "initial manager start failed");
  const auto* first_page = FindInstance(first.supervised_agents, "page_allocation_manager");
  Require(first_page != nullptr, "page_allocation_manager instance missing on first start");

  agents::InMemoryAgentRuntimeCatalog persistence;
  auto save_status = persistence.SaveInstances(first.supervised_agents);
  Require(save_status.ok, "instance persistence failed");

  auto retire_status = persistence.RetireInstance(
      first_page->instance_uuid, "agent-evidence:page-allocation-retired", 12);
  Require(retire_status.ok, "retirement with evidence failed: " + retire_status.diagnostic_code);

  config.persisted_instances = persistence.instances();
  agents::DatabaseLocalAgentRuntimeManager reopened;
  const auto retired_snapshot = reopened.Start(Evidence(11), config);
  Require(retired_snapshot.status.ok, "reopen with retired instance failed");
  const auto* retired = FindInstance(retired_snapshot.supervised_agents, "page_allocation_manager");
  Require(retired != nullptr, "retired page_allocation_manager was not inspectable");
  Require(retired->state == agents::AgentLifecycleState::retired,
          "retired instance re-entered normal state");
  Require(retired->retirement_evidence_uuid == "agent-evidence:page-allocation-retired",
          "retirement evidence did not survive reopen");

  const auto* decision = FindDecision(retired_snapshot, "page_allocation_manager");
  Require(decision != nullptr, "retired instance decision missing");
  Require(!decision->selected && decision->policy_disabled,
          "retired instance was treated as normally selected");
  Require(decision->diagnostic_code == "ENGINE.AGENT_RUNTIME_MANAGER.INSTANCE_RETIRED",
          "retired instance diagnostic mismatch: " + decision->diagnostic_code);

  auto no_evidence = *first_page;
  no_evidence.state = agents::AgentLifecycleState::retired;
  no_evidence.retirement_evidence_uuid.clear();
  const auto encoded = agents::SerializeAgentInstanceRecord(no_evidence);
  agents::AgentInstanceRecord restored;
  const auto restore_status = agents::RestoreAgentInstanceRecord(encoded, &restored);
  Require(!restore_status.ok &&
              restore_status.diagnostic_code == "SB_AGENT_INSTANCE.RETIREMENT_EVIDENCE_REQUIRED",
          "retired instance without evidence restored successfully");
}

}  // namespace

int main() {
  TestDeterministicIdentityStableByDatabaseAgentScopeGeneration();
  TestStateAndPolicyGenerationSerializeRestore();
  TestManagerReusesPersistedIdentityAndState();
  TestRetiredInstanceRequiresEvidenceAndDoesNotRestartNormally();
  return EXIT_SUCCESS;
}

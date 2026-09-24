// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "agent_runtime_manager.hpp"
#include "uuid.hpp"
#include <algorithm>

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

std::string Identity(unsigned salt) {
  scratchbird::core::platform::Uuid value;
  value.bytes = {0x01, 0x9e, 0x0f, 0x2a, 0, 0x3a, 0x70, 0, 0x80, 0, 0, 0, 0, 0, 0, 0};
  value.bytes[15] = static_cast<unsigned char>(salt);
  return {reinterpret_cast<const char*>(value.bytes.data()), value.bytes.size()};
}

agents::AgentRuntimeActivationEvidence Evidence(unsigned generation) {
  agents::AgentRuntimeActivationEvidence evidence;
  evidence.database_uuid = Identity(2);
  evidence.engine_instance_uuid = Identity(3);
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
  Require(first.size() == 16, "deterministic instance ID is not binary16");
  scratchbird::core::platform::Uuid identity;
  std::copy(first.begin(), first.end(), identity.bytes.begin());
  Require(scratchbird::core::uuid::IsEngineIdentityUuid(identity),
          "deterministic instance identity is not an engine UUID");
}

void TestStateAndPolicyGenerationSerializeRestore() {
  agents::AgentInstanceRecord instance;
  instance.instance_uuid = Identity(0xaa);
  instance.agent_type_id = "storage_health_manager";
  instance.policy_uuid = Identity(4);
  instance.scope = "node/database/filespace";
  instance.state = agents::AgentLifecycleState::recommend_only;
  instance.policy_generation = 11;
  instance.instance_generation = 11;
  instance.instance_uuid[10] = '|';
  instance.instance_uuid[11] = '\n';
  instance.policy_uuid[10] = '\0';
  instance.scope = std::string("node|database\n\0filespace", 24);
  instance.last_supervision_detail = "detail|with\nseparator";
  instance.last_failure_diagnostic_code = "SB_AGENT_TEST";
  instance.last_run_start_microseconds = 19;
  instance.last_run_end_microseconds = 23;
  instance.crash_loop_count = 2;
  instance.supervision_failure_count = 7;
  instance.restart_attempts = 8;
  instance.restart_not_before_microseconds = 29;
  instance.cooldown_until_microseconds = 31;
  instance.disabled_by_operator = true;
  instance.safe_mode = true;
  instance.quarantined = true;
  instance.cancellation_requested = true;
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
  Require(restored.policy_uuid == instance.policy_uuid && restored.scope == instance.scope &&
              restored.last_supervision_detail == instance.last_supervision_detail &&
              restored.last_failure_diagnostic_code == instance.last_failure_diagnostic_code &&
              restored.last_run_start_microseconds == 19 && restored.last_run_end_microseconds == 23 &&
              restored.crash_loop_count == 2 && restored.supervision_failure_count == 7 &&
              restored.restart_attempts == 8 && restored.restart_not_before_microseconds == 29 &&
              restored.cooldown_until_microseconds == 31 && restored.run_generation == 3 &&
              restored.lease_until_microseconds == 44 && restored.disabled_by_operator &&
              restored.safe_mode && restored.quarantined && restored.cancellation_requested,
          "binary snapshot lost supervision state or delimiter-containing values");
  Require(agents::SerializeAgentInstanceRecord(restored) == encoded,
          "binary snapshot lost runtime fields");
  Require(encoded.substr(8, 16) == instance.instance_uuid &&
              encoded.substr(24, 16) == instance.policy_uuid,
          "snapshot did not retain exact native UUID bytes");
  const auto reject = [&](const std::string& malformed) {
    auto target = restored;
    const auto before = agents::SerializeAgentInstanceRecord(target);
    Require(!agents::RestoreAgentInstanceRecord(malformed, &target).ok,
            "malformed snapshot accepted");
    Require(agents::SerializeAgentInstanceRecord(target) == before,
            "malformed snapshot partially mutated restore target");
  };
  for (std::size_t size = 0; size < encoded.size(); ++size) reject(encoded.substr(0, size));
  reject(encoded + "trailing");
  reject("legacy|agent|policy|scope|running|0|0|0|0|0");
  auto bad = encoded; bad[0] = 'X'; reject(bad);
  bad = encoded; bad[8 + 6] = 0; reject(bad);
  bad = encoded; bad[8 + 48 + 96] = static_cast<char>(255); reject(bad);
  bad = encoded; bad[8 + 48 + 96 + 1] = static_cast<char>(128); reject(bad);
  bad = encoded; bad.replace(8 + 48 + 96 + 2, 4, 4, static_cast<char>(255)); reject(bad);
  auto invalid = instance; invalid.instance_uuid = "019e0f2a-003a-7000-8000-0000000000aa";
  Require(agents::SerializeAgentInstanceRecord(invalid).empty(), "text UUID snapshot accepted");
  invalid = instance; invalid.scope.assign(65537, 'x');
  Require(agents::SerializeAgentInstanceRecord(invalid).empty(), "oversize snapshot accepted");

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
      first_page->instance_uuid, Identity(5), 12);
  Require(retire_status.ok, "retirement with evidence failed: " + retire_status.diagnostic_code);

  config.persisted_instances = persistence.instances();
  agents::DatabaseLocalAgentRuntimeManager reopened;
  const auto retired_snapshot = reopened.Start(Evidence(11), config);
  Require(retired_snapshot.status.ok, "reopen with retired instance failed");
  const auto* retired = FindInstance(retired_snapshot.supervised_agents, "page_allocation_manager");
  Require(retired != nullptr, "retired page_allocation_manager was not inspectable");
  Require(retired->state == agents::AgentLifecycleState::retired,
          "retired instance re-entered normal state");
  Require(retired->retirement_evidence_uuid == Identity(5),
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

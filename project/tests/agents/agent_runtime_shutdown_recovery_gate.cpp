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
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace agents = scratchbird::core::agents;

[[noreturn]] void Fail(const std::string& message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, const std::string& message) {
  if (!condition) { Fail(message); }
}

bool Contains(std::string_view haystack, std::string_view needle) {
  return haystack.find(needle) != std::string_view::npos;
}

bool HasDiagnostic(const std::vector<std::string>& diagnostics, std::string_view needle) {
  for (const auto& diagnostic : diagnostics) {
    if (Contains(diagnostic, needle)) { return true; }
  }
  return false;
}

const agents::AgentInstanceRecord* FindInstance(
    const std::vector<agents::AgentInstanceRecord>& instances,
    std::string_view type_id) {
  for (const auto& instance : instances) {
    if (instance.agent_type_id == type_id) { return &instance; }
  }
  return nullptr;
}

agents::AgentRuntimeActivationEvidence Evidence(unsigned generation = 17) {
  agents::AgentRuntimeActivationEvidence evidence;
  evidence.database_uuid = "019e0f2a-003c-7000-8000-000000000003";
  evidence.engine_instance_uuid = "engine-instance:019e0f2a-003c";
  evidence.lifecycle_mode = agents::AgentLifecycleMode::database_open;
  evidence.policy_generation = generation;
  evidence.catalog_generation = generation + 1;
  evidence.security_generation = generation + 2;
  evidence.filespace_generation = generation + 3;
  evidence.agent_set_generation = generation + 4;
  evidence.health_generation = generation + 5;
  evidence.tx1_bootstrap_visible = true;
  evidence.tx2_activation_committed = true;
  evidence.startup_admitted = true;
  evidence.health_publication_allowed = true;
  evidence.health_publication_persisted = true;
  evidence.dependency_graph_consistent = true;
  return evidence;
}

agents::AgentRuntimeManagerConfig ConfigWithCatalog(unsigned generation = 17) {
  agents::InMemoryAgentRuntimeCatalog catalog;
  catalog.BootstrapDatabasePolicies(Evidence(generation).database_uuid, generation);

  agents::AgentRuntimeManagerConfig config;
  config.standalone_edition = true;
  config.cluster_authority_available = false;
  config.allow_degraded_service = true;
  config.use_explicit_policy_state = true;
  config.policy_records = catalog.policies();
  config.policy_attachments = catalog.attachments();
  return config;
}

agents::DatabaseEngineAgentInput EngineInput() {
  const auto evidence = Evidence();
  agents::DatabaseEngineAgentInput input;
  input.database_uuid = evidence.database_uuid;
  input.engine_instance_uuid = evidence.engine_instance_uuid;
  input.database_lifecycle_state = "opened";
  input.lifecycle_mode = evidence.lifecycle_mode;
  input.policy_generation = evidence.policy_generation;
  input.catalog_generation = evidence.catalog_generation;
  input.security_generation = evidence.security_generation;
  input.filespace_generation = evidence.filespace_generation;
  input.agent_set_generation = evidence.agent_set_generation;
  input.health_generation = evidence.health_generation;
  input.tx1_bootstrap_visible = evidence.tx1_bootstrap_visible;
  input.tx2_activation_committed = evidence.tx2_activation_committed;
  input.startup_admitted = evidence.startup_admitted;
  input.health_publication_allowed = evidence.health_publication_allowed;
  input.health_publication_persisted = evidence.health_publication_persisted;
  input.allow_degraded_service = true;
  return input;
}

void RequireActivationRefused(const agents::AgentRuntimeActivationEvidence& evidence,
                              const agents::AgentRuntimeManagerConfig& config,
                              std::string_view label) {
  agents::DatabaseLocalAgentRuntimeManager manager;
  const auto snapshot = manager.Start(evidence, config);
  Require(!snapshot.status.ok, std::string(label) + " was accepted");
  Require(snapshot.state == agents::AgentRuntimeManagerState::not_started,
          std::string(label) + " did not leave runtime not_started");
  Require(snapshot.supervised_agents.empty(),
          std::string(label) + " supervised agents without complete evidence");
  Require(!snapshot.activation_evidence_accepted,
          std::string(label) + " recorded activation evidence as accepted");
}

void TestActivationEvidenceRequiredBeforeRuntimeStart() {
  const auto config = ConfigWithCatalog();
  auto missing_tx1 = Evidence();
  missing_tx1.tx1_bootstrap_visible = false;
  RequireActivationRefused(missing_tx1, config, "missing TX1 bootstrap visibility");

  auto missing_tx2 = Evidence();
  missing_tx2.tx2_activation_committed = false;
  RequireActivationRefused(missing_tx2, config, "missing TX2 activation commit");

  auto missing_policy = Evidence();
  missing_policy.policy_generation = 0;
  RequireActivationRefused(missing_policy, config, "missing policy generation");

  auto missing_catalog = Evidence();
  missing_catalog.catalog_generation = 0;
  RequireActivationRefused(missing_catalog, config, "missing catalog generation");

  auto missing_security = Evidence();
  missing_security.security_generation = 0;
  RequireActivationRefused(missing_security, config, "missing security generation");

  auto missing_filespace = Evidence();
  missing_filespace.filespace_generation = 0;
  RequireActivationRefused(missing_filespace, config, "missing filespace generation");

  auto missing_startup_admission = Evidence();
  missing_startup_admission.startup_admitted = false;
  RequireActivationRefused(missing_startup_admission, config, "missing startup admission");

  auto missing_health_permission = Evidence();
  missing_health_permission.health_publication_allowed = false;
  RequireActivationRefused(missing_health_permission, config, "missing health publication permission");

  auto missing_health_persistence = Evidence();
  missing_health_persistence.health_publication_persisted = false;
  RequireActivationRefused(missing_health_persistence, config, "missing health publication persistence");
}

void TestRuntimePublishesActiveHealthAndDatabaseLocalAgents() {
  agents::DatabaseLocalAgentRuntimeManager manager;
  const auto snapshot = manager.Start(Evidence(), ConfigWithCatalog());
  Require(snapshot.status.ok, "runtime start with valid evidence failed");
  Require(snapshot.activation_evidence_accepted, "activation evidence was not accepted");
  Require(snapshot.state == agents::AgentRuntimeManagerState::active,
          "runtime did not publish active health");
  Require(snapshot.ordinary_admission_allowed, "active runtime did not allow ordinary admission");
  Require(snapshot.health_generation == Evidence().health_generation,
          "runtime health generation did not come from evidence");
  Require(snapshot.policy_generation == Evidence().policy_generation,
          "runtime policy generation did not come from evidence");
  Require(snapshot.catalog_generation == Evidence().catalog_generation,
          "runtime catalog generation did not come from evidence");
  Require(snapshot.security_generation == Evidence().security_generation,
          "runtime security generation did not come from evidence");
  Require(snapshot.filespace_generation == Evidence().filespace_generation,
          "runtime filespace generation did not come from evidence");
  Require(FindInstance(snapshot.supervised_agents, "storage_health_manager") != nullptr,
          "runtime did not select storage_health_manager");
  Require(FindInstance(snapshot.supervised_agents, "transaction_pressure_manager") != nullptr,
          "runtime did not select transaction_pressure_manager");
  Require(FindInstance(snapshot.supervised_agents, "cluster_autoscale_manager") == nullptr,
          "runtime selected cluster-only agent in standalone mode");
  Require(snapshot.cluster_paths_failed_closed,
          "runtime did not record standalone cluster paths failed closed");
  Require(HasDiagnostic(snapshot.diagnostics, "ENGINE.AGENT_LIFECYCLE_HEALTHY"),
          "runtime did not publish healthy lifecycle diagnostic");
}

void TestShutdownDrainCoordinationRequiresGracefulShutdownRequest() {
  agents::DatabaseLocalAgentRuntimeManager manager;
  auto config = ConfigWithCatalog();
  const auto started = manager.Start(Evidence(), config);
  Require(started.status.ok, "shutdown test precondition start failed");

  auto drain_evidence = Evidence();
  drain_evidence.lifecycle_mode = agents::AgentLifecycleMode::shutdown;
  drain_evidence.health_generation = started.health_generation + 1;

  config.shutdown_requested = false;
  config.graceful_shutdown = true;
  const auto draining = manager.Drain(drain_evidence, config);
  Require(draining.state == agents::AgentRuntimeManagerState::draining,
          "ordinary drain did not enter draining state");
  Require(!draining.ordinary_admission_allowed,
          "ordinary drain admitted work");
  Require(!draining.shutdown_coordination_complete,
          "ordinary drain completed shutdown coordination without shutdown request");
  Require(HasDiagnostic(draining.diagnostics, "ENGINE.AGENT_LIFECYCLE_DRAINING"),
          "ordinary drain diagnostic missing");

  config.shutdown_requested = true;
  config.graceful_shutdown = false;
  const auto ungraceful = manager.Drain(drain_evidence, config);
  Require(ungraceful.state == agents::AgentRuntimeManagerState::draining,
          "ungraceful shutdown should remain draining");
  Require(!ungraceful.ordinary_admission_allowed,
          "ungraceful shutdown admitted work");
  Require(!ungraceful.shutdown_coordination_complete,
          "ungraceful shutdown completed coordination");

  config.shutdown_requested = true;
  config.graceful_shutdown = true;
  const auto stopped = manager.Drain(drain_evidence, config);
  Require(stopped.state == agents::AgentRuntimeManagerState::stopped,
          "graceful shutdown did not stop runtime");
  Require(!stopped.ordinary_admission_allowed,
          "stopped runtime admitted work");
  Require(stopped.shutdown_coordination_complete,
          "graceful shutdown request did not complete coordination");
  Require(HasDiagnostic(stopped.diagnostics, "ENGINE.AGENT_LIFECYCLE_STOPPED"),
          "graceful shutdown stopped diagnostic missing");
}

void TestRecoveryConvertsUnsafePersistedStatesBeforeReopen() {
  auto config = ConfigWithCatalog();
  agents::DatabaseLocalAgentRuntimeManager first_manager;
  const auto first = first_manager.Start(Evidence(), config);
  Require(first.status.ok, "recovery test precondition start failed");

  config.persisted_instances = first.supervised_agents;
  bool saw_running_seed = false;
  bool saw_failed_seed = false;
  bool saw_stopping_seed = false;
  for (auto& instance : config.persisted_instances) {
    if (instance.agent_type_id == "page_allocation_manager") {
      instance.state = agents::AgentLifecycleState::running;
      saw_running_seed = true;
    } else if (instance.agent_type_id == "storage_health_manager") {
      instance.state = agents::AgentLifecycleState::failed;
      ++instance.crash_loop_count;
      saw_failed_seed = true;
    } else if (instance.agent_type_id == "transaction_pressure_manager") {
      instance.state = agents::AgentLifecycleState::stopping;
      saw_stopping_seed = true;
    }
  }
  Require(saw_running_seed && saw_failed_seed && saw_stopping_seed,
          "recovery test did not seed all required persisted states");

  agents::DatabaseLocalAgentRuntimeManager reopened_manager;
  const auto reopened = reopened_manager.Start(Evidence(), config);
  Require(reopened.status.ok, "runtime reopen with persisted instances failed");
  Require(reopened.state == agents::AgentRuntimeManagerState::active,
          "runtime reopen did not publish active manager state");

  const auto* page = FindInstance(reopened.supervised_agents, "page_allocation_manager");
  Require(page != nullptr, "page_allocation_manager missing after recovery");
  Require(page->state == agents::AgentLifecycleState::paused,
          "running persisted instance was not paused on reopen");

  const auto* storage = FindInstance(reopened.supervised_agents, "storage_health_manager");
  Require(storage != nullptr, "storage_health_manager missing after recovery");
  Require(storage->state == agents::AgentLifecycleState::registered,
          "failed persisted instance was not registered on reopen");
  Require(!storage->safe_mode && !storage->quarantined,
          "failed persisted instance kept stale safety flags on reopen");

  const auto* transaction = FindInstance(reopened.supervised_agents, "transaction_pressure_manager");
  Require(transaction != nullptr, "transaction_pressure_manager missing after recovery");
  Require(transaction->state == agents::AgentLifecycleState::registered,
          "stopping persisted instance was not registered on reopen");
}

void TestEngineLifecycleWrapperAuthorityAndHealthJson() {
  const auto result = agents::StartDatabaseEngineLifecycleAgent(EngineInput());
  Require(result.ok(), "engine lifecycle wrapper start failed");
  Require(result.health.agent_state == agents::DatabaseEngineAgentLifecycleState::active,
          "engine lifecycle wrapper did not publish active state");
  Require(result.health.ordinary_admission_allowed,
          "engine lifecycle wrapper did not publish ordinary admission");
  Require(agents::DatabaseEngineAgentAuthorityBoundaryValid(result.health.authority_boundary),
          "engine lifecycle wrapper authority boundary invalid");
  Require(!result.health.authority_boundary.transaction_finality_authority,
          "engine lifecycle wrapper claimed transaction finality");
  Require(!result.health.authority_boundary.storage_identity_authority,
          "engine lifecycle wrapper claimed storage identity authority");
  Require(!result.health.authority_boundary.catalog_truth_authority,
          "engine lifecycle wrapper claimed catalog truth authority");
  Require(!result.health.authority_boundary.authentication_authority,
          "engine lifecycle wrapper claimed authentication authority");
  Require(!result.health.authority_boundary.authorization_authority,
          "engine lifecycle wrapper claimed authorization authority");
  Require(!result.health.authority_boundary.policy_truth_authority,
          "engine lifecycle wrapper claimed policy truth authority");
  Require(!result.health.authority_boundary.parser_admission_authority,
          "engine lifecycle wrapper claimed parser admission authority");
  Require(!result.health.authority_boundary.sblr_execution_authority,
          "engine lifecycle wrapper claimed SBLR execution authority");
  Require(!result.health.authority_boundary.recovery_finality_authority,
          "engine lifecycle wrapper claimed recovery finality");
  Require(result.health.authority_boundary.engine_lifecycle_request_only,
          "engine lifecycle wrapper did not preserve request-only boundary");

  const auto json = agents::SerializeDatabaseEngineAgentHealthJson(result.health, false);
  Require(Contains(json, "\"database_engine_agent\""),
          "health JSON missing database_engine_agent object");
  Require(Contains(json, "\"agent_state\":\"active\""),
          "health JSON missing active state");
  Require(Contains(json, "\"ordinary_admission_allowed\":true"),
          "health JSON missing ordinary admission");
  Require(Contains(json, "\"authority_boundary_valid\":true"),
          "health JSON missing valid authority boundary");
  Require(Contains(json, "\"selected_agents\":["),
          "health JSON missing selected agent array");
  Require(Contains(json, "storage_health_manager"),
          "health JSON missing selected storage health manager");

  auto shutdown = EngineInput();
  shutdown.database_lifecycle_state = "closed";
  shutdown.lifecycle_mode = agents::AgentLifecycleMode::shutdown;
  shutdown.shutdown_requested = true;
  shutdown.graceful_shutdown = true;
  shutdown.health_generation = result.health.health_generation + 1;
  const auto stopped = agents::StopDatabaseEngineLifecycleAgent(shutdown, result.health);
  Require(stopped.ok(), "engine lifecycle wrapper stop failed");
  Require(!stopped.health.ordinary_admission_allowed,
          "engine lifecycle wrapper stop admitted ordinary work");
  Require(stopped.health.shutdown_coordination_complete,
          "engine lifecycle wrapper graceful shutdown did not complete coordination");

  auto ungraceful = shutdown;
  ungraceful.graceful_shutdown = false;
  const auto not_complete = agents::StopDatabaseEngineLifecycleAgent(ungraceful, result.health);
  Require(not_complete.ok(), "engine lifecycle wrapper ungraceful stop failed");
  Require(!not_complete.health.ordinary_admission_allowed,
          "engine lifecycle wrapper ungraceful stop admitted ordinary work");
  Require(!not_complete.health.shutdown_coordination_complete,
          "engine lifecycle wrapper ungraceful stop completed coordination");
}

}  // namespace

int main() {
  TestActivationEvidenceRequiredBeforeRuntimeStart();
  TestRuntimePublishesActiveHealthAndDatabaseLocalAgents();
  TestShutdownDrainCoordinationRequiresGracefulShutdownRequest();
  TestRecoveryConvertsUnsafePersistedStatesBeforeReopen();
  TestEngineLifecycleWrapperAuthorityAndHealthJson();
  return EXIT_SUCCESS;
}

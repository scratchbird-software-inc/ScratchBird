// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "agent_layer_pipeline.hpp"
#include "agent_runtime.hpp"
#include "agent_runtime_manifest.hpp"

#include <cstdlib>
#include <iostream>
#include <set>
#include <string>
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

agents::AgentTypeDescriptor FindDescriptor(const std::string& type_id) {
  const auto descriptor = agents::FindAgentType(type_id);
  Require(descriptor.has_value(), "missing descriptor: " + type_id);
  return *descriptor;
}

agents::AgentTypeDescriptor RuntimeCoordinatorDescriptor() {
  agents::AgentTypeDescriptor descriptor;
  descriptor.type_id = "node_runtime_agent_coordinator";
  descriptor.layer = agents::AgentRuntimeLayer::l5_coordinator;
  descriptor.deployment = agents::AgentDeployment::local;
  descriptor.scope = "node/agent_runtime";
  descriptor.authority = agents::AgentAuthorityClass::direct_bounded_action;
  descriptor.default_activation = agents::AgentActivationProfile::dry_run;
  descriptor.required_rights = {"OBS_AGENT_STATE_READ", "OBS_AGENT_CONTROL"};
  return descriptor;
}

agents::AgentTypeDescriptor RuntimeWorkerDescriptor() {
  agents::AgentTypeDescriptor descriptor;
  descriptor.type_id = "runtime_l4_worker";
  descriptor.layer = agents::AgentRuntimeLayer::l4_worker;
  descriptor.deployment = agents::AgentDeployment::local;
  descriptor.scope = "database/runtime_worker";
  descriptor.authority = agents::AgentAuthorityClass::direct_bounded_action;
  descriptor.default_activation = agents::AgentActivationProfile::disabled;
  descriptor.required_rights = {"OBS_AGENT_STATE_READ", "OBS_AGENT_CONTROL"};
  return descriptor;
}

void TestCanonicalLayerInventory() {
  const auto manifest = agents::CanonicalAgentManifest();
  const auto registry = agents::CanonicalAgentRegistry();
  Require(manifest.size() == 29, "canonical manifest count changed");
  Require(registry.size() == 29, "canonical registry count changed");

  std::set<std::string> l1;
  std::set<std::string> l2;
  std::set<std::string> l3;
  std::set<std::string> l4;
  std::set<std::string> l5;
  for (const auto& descriptor : registry) {
    switch (descriptor.layer) {
      case agents::AgentRuntimeLayer::l1_observation:
        l1.insert(descriptor.type_id);
        break;
      case agents::AgentRuntimeLayer::l2_recorder:
        l2.insert(descriptor.type_id);
        break;
      case agents::AgentRuntimeLayer::l3_dispatcher:
        l3.insert(descriptor.type_id);
        break;
      case agents::AgentRuntimeLayer::l4_worker:
        l4.insert(descriptor.type_id);
        break;
      case agents::AgentRuntimeLayer::l5_coordinator:
        l5.insert(descriptor.type_id);
        break;
    }
  }

  Require(l1.count("node_resource_agent") == 1,
          "node_resource_agent must be L1");
  Require(l1.count("distributed_query_metrics_agent") == 1,
          "distributed_query_metrics_agent must be L1");
  Require(l2.count("metrics_registry_manager") == 1,
          "metrics_registry_manager must be L2");
  Require(l3.size() == 26, "expected remaining 26 canonical agents to be L3");
  Require(l4.empty(), "canonical manifest must not contain L4 worker entries");
  Require(l5.empty(), "canonical manifest must not contain L5 coordinator entry");
  Require(agents::ValidateCanonicalAgentRegistry().ok,
          "canonical registry validation failed");
}

void TestLayerPipelineContracts() {
  agents::AgentLayerPipelineState state;
  state.observation_queue.capacity = 1;

  auto l1 = FindDescriptor("node_resource_agent");
  agents::AgentObservationRecord observation;
  observation.entity_type = "node";
  observation.entity_uuid = "node-uuid";
  observation.record_type = "NODE_PRESSURE";
  observation.payload = "cpu=42";
  observation.monotonic_timestamp_microseconds = 100;
  Require(agents::PushLayer1Observation(l1, observation, &state).ok,
          "L1 push failed");
  Require(state.observation_queue.records.size() == 1,
          "L1 push did not enqueue record");

  agents::AgentObservationRecord dropped = observation;
  dropped.payload = "cpu=43";
  const auto drop = agents::PushLayer1Observation(l1, dropped, &state);
  Require(drop.ok, "L1 full queue should drop without failing");
  Require(state.observation_queue.dropped_records == 1,
          "L1 did not record dropped observation");

  auto l2 = FindDescriptor("metrics_registry_manager");
  Require(agents::DrainLayer2Observations(
              l2, "findings.node_pressure", "tx-001", 16, &state).ok,
          "L2 drain failed");
  Require(state.findings.size() == 1, "L2 did not commit finding");
  Require(state.findings.front().mga_committed,
          "L2 finding was not marked MGA committed");

  auto l3 = FindDescriptor("admission_control_manager");
  auto l4 = RuntimeWorkerDescriptor();
  agents::AgentLayer3DispatchRequest dispatch;
  dispatch.findings_table_ids = {"findings.node_pressure"};
  dispatch.target_worker_type_id = l4.type_id;
  dispatch.dispatch_reason = "pressure finding";
  const auto dispatch_decision =
      agents::DispatchLayer3FromFindings(l3, l4, dispatch, &state);
  Require(dispatch_decision.status.ok, "L3 dispatch failed");
  Require(dispatch_decision.findings_read == 1,
          "L3 did not read committed L2 finding");
  Require(dispatch_decision.launched_worker_type_ids.size() == 1,
          "L3 did not launch L4 worker");

  dispatch.attempted_raw_queue_read = true;
  const auto bad_dispatch =
      agents::DispatchLayer3FromFindings(l3, l4, dispatch, &state);
  Require(!bad_dispatch.status.ok &&
              bad_dispatch.status.diagnostic_code ==
                  "SB_AGENT_LAYER.L3_RAW_QUEUE_FORBIDDEN",
          "L3 raw queue read was not refused");
}

void TestLayer5RuntimeCoordinatorPolicy() {
  agents::AgentLayerPipelineState state;
  const auto coordinator = RuntimeCoordinatorDescriptor();
  const auto l1 = FindDescriptor("node_resource_agent");
  const auto l2 = FindDescriptor("metrics_registry_manager");
  const auto l3 = FindDescriptor("admission_control_manager");
  const auto l4 = RuntimeWorkerDescriptor();

  agents::AgentLayerCoordinatorPolicy policy;
  policy.max_agents_per_cycle = 8;
  const auto decision = agents::RunLayer5CoordinatorCycle(
      coordinator, policy, {l1, l2, l3, l4}, &state);
  Require(decision.status.ok, "L5 coordinator cycle failed");
  Require(decision.scheduled_agent_type_ids.size() == 3,
          "L5 did not schedule exactly L1-L3 candidates");
  Require(decision.refused_agent_type_ids.size() == 1 &&
              decision.refused_agent_type_ids.front() == l4.type_id,
          "L5 did not refuse L4 worker launch");

  agents::AgentLayerCoordinatorPolicy refused_policy = policy;
  refused_policy.go_no_go = false;
  const auto refused = agents::RunLayer5CoordinatorCycle(
      coordinator, refused_policy, {l1, l2, l3}, &state);
  Require(!refused.status.ok &&
              refused.status.diagnostic_code ==
                  "SB_AGENT_LAYER.L5_GO_NO_GO_REFUSED",
          "L5 go/no-go refusal failed");
  Require(refused.refused_agent_type_ids.size() == 3,
          "L5 go/no-go did not refuse all candidates");
}

}  // namespace

int main() {
  TestCanonicalLayerInventory();
  TestLayerPipelineContracts();
  TestLayer5RuntimeCoordinatorPolicy();
  return EXIT_SUCCESS;
}

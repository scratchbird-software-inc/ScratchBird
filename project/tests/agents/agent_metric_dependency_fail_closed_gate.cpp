// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "agent_runtime.hpp"

#include <cstdlib>
#include <iostream>
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

agents::AgentRuntimeContext NonClusterContext() {
  agents::AgentRuntimeContext context;
  context.security_context_present = true;
  context.private_features_available = true;
  context.standalone_edition = true;
  context.cluster_authority_available = false;
  context.database_uuid = "019f006c-0000-7000-8000-000000000001";
  context.principal_uuid = "019f006c-0000-7000-8000-000000000002";
  context.groups = {"ROOT", "OPS", "DBA"};
  context.rights = {
      "OBS_AGENT_STATE_READ",
      "OBS_AGENT_CONTROL",
      "OBS_AGENT_POLICY_CONTROL",
      "OBS_AGENT_EVIDENCE_READ"};
  context.wall_now_microseconds = 1700000000000060ull;
  context.monotonic_now_microseconds = 6000;
  return context;
}

bool IsNonClusterRuntimeAgent(const agents::AgentTypeDescriptor& descriptor) {
  return !descriptor.cluster_only &&
         descriptor.deployment != agents::AgentDeployment::cluster &&
         (descriptor.deployment == agents::AgentDeployment::local ||
          descriptor.deployment == agents::AgentDeployment::both);
}

agents::AgentMetricObservation FreshObservation(
    const agents::AgentMetricDependency& dep) {
  agents::AgentMetricObservation observation;
  observation.metric_family = dep.metric_family;
  observation.namespace_path = dep.namespace_prefix;
  observation.age_microseconds = dep.max_freshness_microseconds == 0
      ? 1
      : dep.max_freshness_microseconds / 2;
  observation.trusted = true;
  observation.schema_compatible = true;
  observation.scope_compatible = true;
  observation.evidence_uuid = "evidence:" + dep.metric_family;
  observation.snapshot_id = "snapshot:" + dep.metric_family;
  return observation;
}

std::vector<agents::AgentMetricObservation> ObservationsForDescriptor(
    const agents::AgentTypeDescriptor& descriptor,
    bool include_optional = false) {
  std::vector<agents::AgentMetricObservation> observations;
  for (const auto& dep : descriptor.metric_dependencies) {
    if (dep.cluster_only || (!dep.required && !include_optional)) { continue; }
    observations.push_back(FreshObservation(dep));
  }
  return observations;
}

std::vector<agents::AgentPolicyDependencyState> PolicyStateForDescriptor(
    const agents::AgentTypeDescriptor& descriptor) {
  std::vector<agents::AgentPolicyDependencyState> states;
  for (const auto& family : agents::RequiredPolicyFamiliesForAgent(descriptor)) {
    agents::AgentPolicyDependencyState state;
    state.policy_family = family;
    state.policy_uuid = "policy:" + descriptor.type_id + ":" + family;
    state.scope = descriptor.scope;
    state.present = true;
    state.valid = true;
    state.scope_compatible = true;
    state.evidence_uuid = "policy-evidence:" + family;
    states.push_back(std::move(state));
  }
  return states;
}

const agents::AgentMetricDependency& RequiredDep(
    const agents::AgentTypeDescriptor& descriptor,
    const std::string& family) {
  for (const auto& dep : descriptor.metric_dependencies) {
    if (dep.metric_family == family) { return dep; }
  }
  Fail("missing dependency " + descriptor.type_id + ":" + family);
}

const agents::AgentDependencyDiagnostic& FirstDiagnostic(
    const agents::AgentDependencyEvaluation& evaluation) {
  Require(!evaluation.diagnostics.empty(), "dependency diagnostic missing");
  return evaluation.diagnostics.front();
}

void RequireFailsWith(const agents::AgentDependencyEvaluation& evaluation,
                      const std::string& code,
                      const std::string& evidence_uuid) {
  Require(!evaluation.status.ok, "dependency evaluation unexpectedly passed");
  Require(evaluation.failed_closed, "dependency evaluation did not fail closed");
  const auto& diagnostic = FirstDiagnostic(evaluation);
  Require(diagnostic.diagnostic_code == code,
          "diagnostic mismatch: " + diagnostic.diagnostic_code);
  if (!evidence_uuid.empty()) {
    Require(diagnostic.evidence_uuid == evidence_uuid,
            "evidence UUID mismatch: " + diagnostic.evidence_uuid);
  } else {
    Require(!diagnostic.evidence_uuid.empty(), "missing generated evidence UUID");
  }
}

void TestAllNonClusterRequiredLocalMetricsValidate() {
  int checked = 0;
  int local_projection = 0;
  for (const auto& descriptor : agents::CanonicalAgentRegistry()) {
    if (!IsNonClusterRuntimeAgent(descriptor)) { continue; }
    const auto evaluation = agents::EvaluateAgentMetricDependencies(
        descriptor,
        NonClusterContext(),
        ObservationsForDescriptor(descriptor),
        PolicyStateForDescriptor(descriptor),
        true);
    Require(evaluation.status.ok,
            "fresh required dependencies failed for " + descriptor.type_id +
                ": " + evaluation.status.diagnostic_code);
    if (evaluation.cluster_path_failed_closed) {
      ++local_projection;
      Require(!evaluation.diagnostics.empty(),
              "mixed cluster/local projection lacked diagnostic evidence");
    }
    ++checked;
  }
  Require(checked > 0, "no non-cluster agents checked");
  Require(local_projection > 0,
          "no mixed cluster/local non-cluster projection was checked");
}

void TestRequiredMetricFailureDiagnostics() {
  const auto descriptor = agents::FindAgentType("page_allocation_manager");
  Require(descriptor.has_value(), "page_allocation_manager descriptor missing");
  const auto& dep = RequiredDep(*descriptor, "sb_page_free_count");

  {
    auto observations = ObservationsForDescriptor(*descriptor);
    observations.erase(observations.begin());
    const auto evaluation = agents::EvaluateAgentMetricDependencies(
        *descriptor, NonClusterContext(), observations, PolicyStateForDescriptor(*descriptor), true);
    RequireFailsWith(evaluation, "SB_AGENT_METRICS.REQUIRED_METRIC_MISSING", {});
  }
  {
    auto observations = ObservationsForDescriptor(*descriptor);
    observations.front().age_microseconds = dep.max_freshness_microseconds + 1;
    observations.front().evidence_uuid = "metric-evidence:stale-page-free";
    const auto evaluation = agents::EvaluateAgentMetricDependencies(
        *descriptor, NonClusterContext(), observations, PolicyStateForDescriptor(*descriptor), true);
    RequireFailsWith(evaluation, "SB_AGENT_METRICS.REQUIRED_METRIC_STALE",
                     "metric-evidence:stale-page-free");
  }
  {
    auto observations = ObservationsForDescriptor(*descriptor);
    observations.front().trusted = false;
    observations.front().evidence_uuid = "metric-evidence:untrusted-page-free";
    const auto evaluation = agents::EvaluateAgentMetricDependencies(
        *descriptor, NonClusterContext(), observations, PolicyStateForDescriptor(*descriptor), true);
    RequireFailsWith(evaluation, "SB_AGENT_METRICS.REQUIRED_METRIC_UNTRUSTED",
                     "metric-evidence:untrusted-page-free");
  }
  {
    auto observations = ObservationsForDescriptor(*descriptor);
    observations.front().namespace_path = "sys.metrics.storage.filespaces";
    observations.front().evidence_uuid = "metric-evidence:namespace-page-free";
    const auto evaluation = agents::EvaluateAgentMetricDependencies(
        *descriptor, NonClusterContext(), observations, PolicyStateForDescriptor(*descriptor), true);
    RequireFailsWith(evaluation, "SB_AGENT_METRICS.NAMESPACE_SCHEMA_INCOMPATIBLE",
                     "metric-evidence:namespace-page-free");
  }
  {
    auto observations = ObservationsForDescriptor(*descriptor);
    observations.front().schema_compatible = false;
    observations.front().evidence_uuid = "metric-evidence:schema-page-free";
    const auto evaluation = agents::EvaluateAgentMetricDependencies(
        *descriptor, NonClusterContext(), observations, PolicyStateForDescriptor(*descriptor), true);
    RequireFailsWith(evaluation, "SB_AGENT_METRICS.NAMESPACE_SCHEMA_INCOMPATIBLE",
                     "metric-evidence:schema-page-free");
  }
  {
    auto observations = ObservationsForDescriptor(*descriptor);
    observations.front().scope_compatible = false;
    observations.front().evidence_uuid = "metric-evidence:scope-page-free";
    const auto evaluation = agents::EvaluateAgentMetricDependencies(
        *descriptor, NonClusterContext(), observations, PolicyStateForDescriptor(*descriptor), true);
    RequireFailsWith(evaluation, "SB_AGENT_METRICS.SCOPE_INCOMPATIBLE",
                     "metric-evidence:scope-page-free");
  }
}

void TestOptionalMetricSuppressionDoesNotBlock() {
  const auto descriptor = agents::FindAgentType("page_allocation_manager");
  Require(descriptor.has_value(), "page_allocation_manager descriptor missing");
  const auto evaluation = agents::EvaluateAgentMetricDependencies(
      *descriptor,
      NonClusterContext(),
      ObservationsForDescriptor(*descriptor, false),
      PolicyStateForDescriptor(*descriptor),
      true);
  Require(evaluation.status.ok, "optional absence blocked page allocation manager");
  Require(evaluation.optional_suppressed, "optional absence did not suppress exactly");
  bool found = false;
  for (const auto& diagnostic : evaluation.diagnostics) {
    if (diagnostic.diagnostic_code == "SB_AGENT_METRICS.OPTIONAL_METRIC_SUPPRESSED" &&
        diagnostic.subject == "sb_page_fragmentation_ratio") {
      Require(diagnostic.optional_suppressed, "optional diagnostic flag missing");
      Require(!diagnostic.evidence_uuid.empty(), "optional diagnostic lacked evidence");
      found = true;
    }
  }
  Require(found, "optional suppression diagnostic missing");
}

void TestPolicyDependencyFailures() {
  const auto descriptor = agents::FindAgentType("filespace_capacity_manager");
  Require(descriptor.has_value(), "filespace_capacity_manager descriptor missing");
  const auto observations = ObservationsForDescriptor(*descriptor);

  RequireFailsWith(
      agents::EvaluateAgentMetricDependencies(
          *descriptor, NonClusterContext(), observations, {}, true),
      "SB_AGENT_POLICY_DEPENDENCY.MISSING",
      {});

  auto invalid = PolicyStateForDescriptor(*descriptor);
  invalid.front().valid = false;
  invalid.front().evidence_uuid = "policy-evidence:invalid-filespace";
  RequireFailsWith(
      agents::EvaluateAgentMetricDependencies(
          *descriptor, NonClusterContext(), observations, invalid, true),
      "SB_AGENT_POLICY_DEPENDENCY.INVALID",
      "policy-evidence:invalid-filespace");

  auto scope = PolicyStateForDescriptor(*descriptor);
  scope.front().scope_compatible = false;
  scope.front().evidence_uuid = "policy-evidence:scope-filespace";
  RequireFailsWith(
      agents::EvaluateAgentMetricDependencies(
          *descriptor, NonClusterContext(), observations, scope, true),
      "SB_AGENT_POLICY_DEPENDENCY.SCOPE_INCOMPATIBLE",
      "policy-evidence:scope-filespace");
}

void TestTickAndActionIntegrationsFailClosed() {
  const auto descriptor = agents::FindAgentType("page_allocation_manager");
  Require(descriptor.has_value(), "page_allocation_manager descriptor missing");

  auto request = agents::AgentTickHealthRequest{};
  request.context = NonClusterContext();
  request.policy_generation = 6;
  request.enforce_metric_observation_dependencies = true;
  request.enforce_policy_dependency_state = true;
  request.metric_observations = ObservationsForDescriptor(*descriptor);
  request.policy_dependency_states = PolicyStateForDescriptor(*descriptor);
  request.metric_observations.front().trusted = false;
  request.metric_observations.front().evidence_uuid = "tick-evidence:untrusted-page-free";
  const auto tick = agents::BuildNonClusterAgentTickHealthSnapshot(request);
  Require(tick.status.ok, "strict tick snapshot did not return records");
  bool saw_page_failure = false;
  for (const auto& record : tick.records) {
    if (record.agent_type_id != "page_allocation_manager") { continue; }
    saw_page_failure = true;
    Require(record.tick_class == agents::AgentTickHealthClass::failed_closed,
            "untrusted tick metric did not fail closed");
    Require(record.diagnostic_code == "SB_AGENT_METRICS.REQUIRED_METRIC_UNTRUSTED",
            "tick metric diagnostic mismatch: " + record.diagnostic_code);
    Require(!record.dependency_diagnostics.empty(),
            "tick failure lacked dependency evidence");
    Require(record.dependency_diagnostics.front().evidence_uuid ==
                "tick-evidence:untrusted-page-free",
            "tick dependency evidence mismatch");
  }
  Require(saw_page_failure, "page_allocation_manager tick record missing");

  const auto contract = agents::FindAgentActionContract(
      "page_allocation_manager", "preallocate_page_family");
  Require(contract.has_value(), "preallocate_page_family contract missing");
  auto policy = agents::BaselinePolicyForAgentFamily(
      *descriptor, "page_preallocation_policy", 1);
  auto action = agents::AgentActionContractEvaluationRequest{};
  action.context = NonClusterContext();
  action.policy = &policy;
  action.policy_present = true;
  action.policy_gate_present = true;
  action.evidence_store_available = true;
  action.enforce_metric_observation_dependencies = true;
  action.enforce_policy_dependency_state = true;
  action.policy_dependency_states = PolicyStateForDescriptor(*descriptor);
  for (const auto& family : contract->metric_families) {
    agents::AgentMetricDependency dep;
    dep.metric_family = family;
    dep.max_freshness_microseconds = 300000000;
    dep.namespace_prefix = "sys.metrics.storage.pages";
    action.metric_observations.push_back(FreshObservation(dep));
  }
  action.metric_observations.front().age_microseconds = 300000001;
  action.metric_observations.front().evidence_uuid = "action-evidence:stale-page";
  const auto decision = agents::EvaluateAgentActionContract(*contract, action);
  Require(decision.result_class == agents::AgentActionResultClass::failed_closed,
          "stale action metric did not fail closed");
  Require(decision.diagnostic_code == "SB_AGENT_METRICS.REQUIRED_METRIC_STALE",
          "action stale diagnostic mismatch: " + decision.diagnostic_code);
  Require(decision.evidence_uuid == "action-evidence:stale-page",
          "action dependency evidence UUID mismatch: " + decision.evidence_uuid);
  Require(!decision.mutates_state, "failed-closed dependency decision mutated state");
}

void TestMixedClusterLocalProjectionEvidence() {
  const auto descriptor = agents::FindAgentType("transaction_pressure_manager");
  Require(descriptor.has_value(), "transaction_pressure_manager descriptor missing");
  const auto evaluation = agents::EvaluateAgentMetricDependencies(
      *descriptor,
      NonClusterContext(),
      ObservationsForDescriptor(*descriptor),
      PolicyStateForDescriptor(*descriptor),
      true);
  Require(evaluation.status.ok, "mixed local projection did not remain valid");
  Require(evaluation.cluster_path_failed_closed,
          "cluster path was not marked failed closed");
  bool found = false;
  for (const auto& diagnostic : evaluation.diagnostics) {
    if (diagnostic.diagnostic_code ==
        "SB_AGENT_METRICS.CLUSTER_AUTHORITY_REQUIRED") {
      Require(!diagnostic.evidence_uuid.empty(),
              "cluster authority diagnostic lacked evidence");
      found = true;
    }
  }
  Require(found, "cluster authority diagnostic missing");
}

}  // namespace

int main() {
  TestAllNonClusterRequiredLocalMetricsValidate();
  TestRequiredMetricFailureDiagnostics();
  TestOptionalMetricSuppressionDoesNotBlock();
  TestPolicyDependencyFailures();
  TestTickAndActionIntegrationsFailClosed();
  TestMixedClusterLocalProjectionEvidence();
  return EXIT_SUCCESS;
}

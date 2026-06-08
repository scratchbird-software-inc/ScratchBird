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
#include <map>
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

agents::AgentRuntimeContext Context() {
  agents::AgentRuntimeContext context;
  context.security_context_present = true;
  context.private_features_available = true;
  context.standalone_edition = true;
  context.cluster_authority_available = false;
  context.database_uuid = "019f006g-0000-7000-8000-000000000001";
  context.principal_uuid = "019f006g-0000-7000-8000-000000000002";
  context.groups = {"ROOT", "OPS", "DBA"};
  context.rights = {
      "OBS_AGENT_STATE_READ",
      "OBS_AGENT_CONTROL",
      "OBS_AGENT_POLICY_CONTROL",
      "OBS_AGENT_EVIDENCE_READ"};
  context.wall_now_microseconds = 1700000000000000ull;
  context.monotonic_now_microseconds = 7000;
  return context;
}

bool IsNonClusterRuntimeAgent(const agents::AgentTypeDescriptor& descriptor) {
  return !descriptor.cluster_only &&
         descriptor.deployment != agents::AgentDeployment::cluster &&
         (descriptor.deployment == agents::AgentDeployment::local ||
          descriptor.deployment == agents::AgentDeployment::both);
}

const agents::AgentTypeDescriptor& FindDescriptor(
    const std::vector<agents::AgentTypeDescriptor>& descriptors,
    const std::string& agent) {
  for (const auto& descriptor : descriptors) {
    if (descriptor.type_id == agent) { return descriptor; }
  }
  Fail("missing descriptor " + agent);
}

const agents::AgentMetricDependency& FindDep(
    const agents::AgentTypeDescriptor& descriptor,
    const std::string& family) {
  for (const auto& dep : descriptor.metric_dependencies) {
    if (dep.metric_family == family) { return dep; }
  }
  Fail("missing dependency " + descriptor.type_id + ":" + family);
}

agents::AgentMetricObservation ObservationFor(
    const agents::AgentMetricDependency& dep) {
  agents::AgentMetricObservation observation;
  observation.metric_family = dep.metric_family;
  observation.namespace_path = dep.namespace_prefix;
  observation.age_microseconds = dep.max_freshness_microseconds > 1
      ? dep.max_freshness_microseconds / 2
      : 1;
  observation.trusted = true;
  observation.source_quality = dep.required_source_quality;
  if (observation.source_quality == agents::AgentMetricSourceQuality::unknown) {
    observation.source_quality = agents::AgentMetricSourceQuality::trusted;
  }
  observation.schema_compatible = true;
  observation.scope_compatible = true;
  observation.evidence_uuid = "metric-evidence:" + dep.metric_family;
  observation.snapshot_id = "metric-snapshot:" + dep.metric_family;
  return observation;
}

std::vector<agents::AgentMetricObservation> ObservationsFor(
    const agents::AgentTypeDescriptor& descriptor,
    bool include_optional = false) {
  std::vector<agents::AgentMetricObservation> observations;
  for (const auto& dep : descriptor.metric_dependencies) {
    if (dep.cluster_only || (!dep.required && !include_optional)) { continue; }
    observations.push_back(ObservationFor(dep));
  }
  return observations;
}

std::vector<agents::AgentPolicyDependencyState> PolicyStateFor(
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

void RequireFailsWith(const agents::AgentDependencyEvaluation& evaluation,
                      const std::string& code,
                      const std::string& evidence_uuid,
                      const std::string& policy_field,
                      const std::string& fail_behavior,
                      const std::string& evidence_field) {
  Require(!evaluation.status.ok, "dependency evaluation unexpectedly passed");
  Require(evaluation.failed_closed, "dependency evaluation did not fail closed");
  Require(!evaluation.diagnostics.empty(), "dependency diagnostic missing");
  const auto& diagnostic = evaluation.diagnostics.front();
  Require(diagnostic.diagnostic_code == code,
          "diagnostic mismatch: " + diagnostic.diagnostic_code);
  Require(diagnostic.evidence_uuid == evidence_uuid,
          "evidence UUID mismatch: " + diagnostic.evidence_uuid);
  Require(diagnostic.policy_field == policy_field,
          "policy field mismatch: " + diagnostic.policy_field);
  Require(diagnostic.fail_behavior == fail_behavior,
          "fail behavior mismatch: " + diagnostic.fail_behavior);
  Require(diagnostic.dependency_evidence_field == evidence_field,
          "evidence field mismatch: " + diagnostic.dependency_evidence_field);
}

void TestRegistryCoverageAndExactRepresentativeRows() {
  const auto status = agents::ValidateCanonicalAgentRegistry();
  Require(status.ok, "canonical registry validation failed: " + status.diagnostic_code);

  const auto descriptors = agents::CanonicalAgentRegistry();
  int noncluster_count = 0;
  for (const auto& descriptor : descriptors) {
    if (!IsNonClusterRuntimeAgent(descriptor)) { continue; }
    const auto contract_deps = agents::MetricDependenciesForAgent(descriptor.type_id);
    Require(!contract_deps.empty(), "contract missing for " + descriptor.type_id);
    Require(contract_deps.size() == descriptor.metric_dependencies.size(),
            "descriptor dependency count drift for " + descriptor.type_id);
    for (const auto& dep : descriptor.metric_dependencies) {
      const auto contract =
          agents::FindAgentMetricDependencyContract(descriptor.type_id, dep.metric_family);
      Require(contract.has_value(), "missing contract row for " + descriptor.type_id + ":" + dep.metric_family);
      Require(contract->namespace_prefix == dep.namespace_prefix, "namespace drift");
      Require(contract->policy_field == dep.policy_field, "policy field drift");
      Require(contract->decision_use == dep.decision_use, "decision use drift");
      Require(contract->fail_behavior == dep.fail_behavior, "fail behavior drift");
      Require(contract->evidence_field == dep.evidence_field, "evidence field drift");
      Require(!dep.required_quality.empty(), "quality missing");
      Require(!dep.aggregation.empty(), "aggregation missing");
    }
    ++noncluster_count;
  }
  Require(noncluster_count >= 20, "non-cluster registry coverage too small");

  const auto& page = FindDescriptor(descriptors, "page_allocation_manager");
  const auto& page_free = FindDep(page, "sb_page_free_count");
  Require(page_free.namespace_prefix == "sys.metrics.storage.pages", "page namespace mismatch");
  Require(page_free.max_freshness_microseconds == 15000000ull, "page freshness mismatch");
  Require(page_free.policy_field == "preallocation_allowed", "page policy field mismatch");
  Require(page_free.decision_use == "allocation decision", "page decision use mismatch");
  Require(page_free.fail_behavior == "preallocation denied", "page fail behavior mismatch");
  Require(page_free.evidence_field == "page_allocation_snapshot", "page evidence field mismatch");

  const auto& filespace = FindDescriptor(descriptors, "filespace_capacity_manager");
  const auto& shrink = FindDep(filespace, "sb_filespace_truncate_ready_bytes");
  Require(shrink.dependency_kind == agents::AgentMetricDependencyKind::required_for_shrink,
          "filespace shrink classification mismatch");
  Require(shrink.evidence_field == "filespace_shrink_proof", "filespace shrink evidence mismatch");

  const auto& storage = FindDescriptor(descriptors, "storage_health_manager");
  Require(!FindDep(storage, "sb_page_allocation_failures_total").required,
          "storage optional page allocator input was not optional");
  Require(FindDep(FindDescriptor(descriptors, "memory_governor"),
                  "sb_memory_emergency_reserve_bytes").fail_behavior ==
              "deny large grants",
          "memory fail behavior mismatch");
  Require(FindDep(FindDescriptor(descriptors, "admission_control_manager"),
                  "sb_scheduler_queue_depth").policy_field ==
              "scheduler_queue_threshold",
          "admission policy field mismatch");
  Require(FindDep(FindDescriptor(descriptors, "job_control_manager"),
                  "sb_job_control_actions_total").evidence_field ==
              "job_action_counter",
          "job evidence field mismatch");
  Require(FindDep(FindDescriptor(descriptors, "export_adapter_manager"),
                  "sb_export_adapter_queue_depth").namespace_prefix ==
              "sys.metrics.export",
          "export namespace mismatch");
  Require(FindDep(FindDescriptor(descriptors, "support_bundle_triage_agent"),
                  "sb_support_bundle_completeness_ratio").decision_use ==
              "bundle readiness",
          "support decision use mismatch");
}

void TestRequiredDependencyDiagnosticsCarryContractFields() {
  const auto descriptor = agents::FindAgentType("page_allocation_manager");
  Require(descriptor.has_value(), "page_allocation_manager missing");
  const auto& dep = FindDep(*descriptor, "sb_page_free_count");

  {
    auto observations = ObservationsFor(*descriptor);
    observations.erase(observations.begin());
    const auto evaluation = agents::EvaluateAgentMetricDependencies(
        *descriptor, Context(), observations, PolicyStateFor(*descriptor), true);
    Require(!evaluation.status.ok, "missing required metric passed");
    Require(evaluation.diagnostics.front().policy_field == dep.policy_field,
            "missing diagnostic policy field not preserved");
    Require(evaluation.diagnostics.front().dependency_evidence_field == dep.evidence_field,
            "missing diagnostic evidence field not preserved");
  }
  {
    auto observations = ObservationsFor(*descriptor);
    observations.front().age_microseconds = dep.max_freshness_microseconds + 1;
    observations.front().evidence_uuid = "metric-evidence:stale-page-free";
    RequireFailsWith(
        agents::EvaluateAgentMetricDependencies(
            *descriptor, Context(), observations, PolicyStateFor(*descriptor), true),
        "SB_AGENT_METRICS.REQUIRED_METRIC_STALE",
        "metric-evidence:stale-page-free",
        dep.policy_field,
        dep.fail_behavior,
        dep.evidence_field);
  }
  {
    auto observations = ObservationsFor(*descriptor);
    observations.front().trusted = false;
    observations.front().evidence_uuid = "metric-evidence:untrusted-page-free";
    RequireFailsWith(
        agents::EvaluateAgentMetricDependencies(
            *descriptor, Context(), observations, PolicyStateFor(*descriptor), true),
        "SB_AGENT_METRICS.REQUIRED_METRIC_UNTRUSTED",
        "metric-evidence:untrusted-page-free",
        dep.policy_field,
        dep.fail_behavior,
        dep.evidence_field);
  }
  {
    auto observations = ObservationsFor(*descriptor);
    observations.front().source_quality = agents::AgentMetricSourceQuality::unknown;
    observations.front().evidence_uuid = "metric-evidence:unknown-quality-page-free";
    RequireFailsWith(
        agents::EvaluateAgentMetricDependencies(
            *descriptor, Context(), observations, PolicyStateFor(*descriptor), true),
        "SB_AGENT_METRICS.REQUIRED_METRIC_QUALITY_INSUFFICIENT",
        "metric-evidence:unknown-quality-page-free",
        dep.policy_field,
        dep.fail_behavior,
        dep.evidence_field);
  }
  {
    auto observations = ObservationsFor(*descriptor);
    observations.front().namespace_path = "sys.metrics.storage.filespaces";
    observations.front().evidence_uuid = "metric-evidence:namespace-page-free";
    RequireFailsWith(
        agents::EvaluateAgentMetricDependencies(
            *descriptor, Context(), observations, PolicyStateFor(*descriptor), true),
        "SB_AGENT_METRICS.NAMESPACE_SCHEMA_INCOMPATIBLE",
        "metric-evidence:namespace-page-free",
        dep.policy_field,
        dep.fail_behavior,
        dep.evidence_field);
  }
  {
    auto observations = ObservationsFor(*descriptor);
    observations.front().schema_compatible = false;
    observations.front().evidence_uuid = "metric-evidence:schema-page-free";
    RequireFailsWith(
        agents::EvaluateAgentMetricDependencies(
            *descriptor, Context(), observations, PolicyStateFor(*descriptor), true),
        "SB_AGENT_METRICS.NAMESPACE_SCHEMA_INCOMPATIBLE",
        "metric-evidence:schema-page-free",
        dep.policy_field,
        dep.fail_behavior,
        dep.evidence_field);
  }
  {
    auto observations = ObservationsFor(*descriptor);
    observations.front().scope_compatible = false;
    observations.front().evidence_uuid = "metric-evidence:scope-page-free";
    RequireFailsWith(
        agents::EvaluateAgentMetricDependencies(
            *descriptor, Context(), observations, PolicyStateFor(*descriptor), true),
        "SB_AGENT_METRICS.SCOPE_INCOMPATIBLE",
        "metric-evidence:scope-page-free",
        dep.policy_field,
        dep.fail_behavior,
        dep.evidence_field);
  }
}

void TestOptionalSuppressionAndClusterProjection() {
  const auto page = agents::FindAgentType("page_allocation_manager");
  Require(page.has_value(), "page_allocation_manager missing");
  const auto optional = agents::EvaluateAgentMetricDependencies(
      *page, Context(), ObservationsFor(*page, false), PolicyStateFor(*page), true);
  Require(optional.status.ok, "optional absence blocked local work");
  Require(optional.optional_suppressed, "optional absence was not suppressed");
  bool saw_fragmentation = false;
  for (const auto& diagnostic : optional.diagnostics) {
    if (diagnostic.subject == "sb_page_fragmentation_ratio") {
      saw_fragmentation = true;
      Require(diagnostic.optional_suppressed, "optional diagnostic flag missing");
      Require(diagnostic.policy_field == "fragmentation_threshold",
              "optional policy field mismatch");
      Require(diagnostic.failed_closed == false, "optional diagnostic failed closed");
    }
  }
  Require(saw_fragmentation, "optional fragmentation diagnostic missing");

  const auto cluster = agents::FindAgentType("cluster_autoscale_manager");
  Require(cluster.has_value(), "cluster_autoscale_manager missing");
  const auto cluster_eval = agents::EvaluateAgentMetricDependencies(
      *cluster, Context(), {}, {}, false);
  Require(!cluster_eval.status.ok, "cluster-only agent ran without cluster authority");
  Require(cluster_eval.status.diagnostic_code ==
              "SB_AGENT_METRICS.CLUSTER_AUTHORITY_REQUIRED",
          "cluster-only refusal diagnostic mismatch");
  Require(cluster_eval.cluster_path_failed_closed, "cluster path did not fail closed");

  const auto mixed = agents::FindAgentType("transaction_pressure_manager");
  Require(mixed.has_value(), "transaction_pressure_manager missing");
  const auto mixed_eval = agents::EvaluateAgentMetricDependencies(
      *mixed, Context(), ObservationsFor(*mixed), PolicyStateFor(*mixed), true);
  Require(mixed_eval.status.ok, "mixed local projection failed local work");
  Require(mixed_eval.local_projection_valid, "mixed local projection not marked valid");
  Require(mixed_eval.cluster_path_failed_closed, "mixed cluster path not refused");
  bool saw_limbo = false;
  for (const auto& diagnostic : mixed_eval.diagnostics) {
    if (diagnostic.subject == "sb_cluster_limbo_transactions") {
      saw_limbo = true;
      Require(diagnostic.policy_field == "limbo_pressure_threshold",
              "cluster policy field mismatch");
      Require(diagnostic.dependency_evidence_field == "limbo_count",
              "cluster evidence field mismatch");
    }
  }
  Require(saw_limbo, "cluster projection diagnostic missing");
}

void TestActionContractUsesDependencyContractFreshness() {
  const auto descriptor = agents::FindAgentType("page_allocation_manager");
  Require(descriptor.has_value(), "page_allocation_manager missing");
  const auto contract = agents::FindAgentActionContract(
      "page_allocation_manager", "preallocate_page_family");
  Require(contract.has_value(), "preallocate_page_family contract missing");

  auto policy = agents::BaselinePolicyForAgentFamily(
      *descriptor, "page_preallocation_policy", 1);
  auto request = agents::AgentActionContractEvaluationRequest{};
  request.context = Context();
  request.policy = &policy;
  request.policy_present = true;
  request.policy_gate_present = true;
  request.evidence_store_available = true;
  request.enforce_metric_observation_dependencies = true;
  request.enforce_policy_dependency_state = true;
  request.policy_dependency_states = PolicyStateFor(*descriptor);
  for (const auto& family : contract->metric_families) {
    const auto dep = agents::FindAgentMetricDependencyContract(
        contract->owning_agent, family);
    Require(dep.has_value(), "action metric missing dependency contract: " + family);
    request.metric_observations.push_back(ObservationFor(*dep));
  }
  request.metric_observations.front().age_microseconds = 16000000ull;
  request.metric_observations.front().evidence_uuid =
      "action-evidence:contract-freshness";

  const auto decision = agents::EvaluateAgentActionContract(*contract, request);
  Require(decision.result_class == agents::AgentActionResultClass::failed_closed,
          "action contract did not inherit dependency freshness");
  Require(decision.diagnostic_code == "SB_AGENT_METRICS.REQUIRED_METRIC_STALE",
          "action freshness diagnostic mismatch: " + decision.diagnostic_code);
  Require(decision.evidence_uuid == "action-evidence:contract-freshness",
          "action evidence did not propagate");
  Require(!decision.mutates_state, "failed dependency action mutated state");
}

}  // namespace

int main() {
  TestRegistryCoverageAndExactRepresentativeRows();
  TestRequiredDependencyDiagnosticsCarryContractFields();
  TestOptionalSuppressionAndClusterProjection();
  TestActionContractUsesDependencyContractFreshness();
  return EXIT_SUCCESS;
}

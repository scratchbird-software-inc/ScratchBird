// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "agent_cluster_boundary.hpp"
#include "agent_runtime.hpp"

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

bool Contains(const std::string& value, const std::string& needle) {
  return value.find(needle) != std::string::npos;
}

std::string RequireRow(const std::vector<std::string>& rows,
                       std::string_view category) {
  const std::string prefix = "explain.row=" + std::string(category) + ";";
  for (const auto& row : rows) {
    if (row.rfind(prefix, 0) == 0) { return row; }
  }
  Fail("missing explain row category " + std::string(category));
}

bool HasDiagnostic(const agents::AgentClusterBoundaryResult& result,
                   std::string_view code) {
  for (const auto& diagnostic_code : result.diagnostic_codes) {
    if (diagnostic_code == code) { return true; }
  }
  return false;
}

bool HasEvidence(const agents::AgentClusterBoundaryResult& result,
                 std::string_view kind,
                 std::string_view id) {
  for (const auto& evidence : result.evidence) {
    if (evidence.evidence_kind == kind && evidence.evidence_id == id) {
      return true;
    }
  }
  return false;
}

agents::AgentRuntimeContext ClusterContext() {
  agents::AgentRuntimeContext context;
  context.security_context_present = true;
  context.cluster_authority_available = true;
  context.standalone_edition = false;
  context.principal_uuid = "principal:public-agent-pcr082";
  context.database_uuid = "database:public-agent-pcr082";
  context.cluster_uuid = "cluster:public-agent-pcr082";
  context.trace_tags.push_back("public_agent_operator_explain_cluster_boundary_gate");
  return context;
}

agents::AgentActionDecision DecisionFor(const agents::AgentTypeDescriptor& descriptor) {
  agents::AgentActionDecision decision;
  decision.result_class = descriptor.cluster_only
                              ? agents::AgentActionResultClass::failed_closed
                              : agents::AgentActionResultClass::accepted;
  decision.diagnostic_code = descriptor.cluster_only
                                 ? agents::kAgentClusterExternalProviderRequiredCode
                                 : "SB_AGENT_ACTION.ACCEPTED";
  decision.detail = descriptor.cluster_only
                        ? "cluster execution requires external provider"
                        : "public explain row proof";
  decision.evidence_uuid = "evidence:public-agent-pcr082:" + descriptor.type_id;
  decision.mutates_state = false;
  return decision;
}

void RequireExplainCategories(const agents::AgentTypeDescriptor& descriptor,
                              const agents::AgentPolicy& policy,
                              const agents::AgentActionDecision& decision) {
  const auto rows = agents::ExplainAgentDecision(descriptor, policy, decision);
  const auto metrics = RequireRow(rows, "metrics");
  const auto policy_row = RequireRow(rows, "policy");
  const auto dependency = RequireRow(rows, "dependency");
  const auto decision_row = RequireRow(rows, "decision");
  const auto refusal = RequireRow(rows, "refusal");
  const auto approval = RequireRow(rows, "approval");
  const auto actuator = RequireRow(rows, "actuator_outcome");
  const auto evidence = RequireRow(rows, "evidence");

  Require(Contains(metrics, "metrics_are_authority=false"),
          descriptor.type_id + " metrics row claimed authority");
  Require(Contains(policy_row, "policy_uuid=" + policy.policy_uuid),
          descriptor.type_id + " policy row lost policy UUID");
  Require(Contains(policy_row, "explainability_required=true"),
          descriptor.type_id + " policy row lost explainability flag");
  Require(Contains(dependency, "dependencies_are_authority=false"),
          descriptor.type_id + " dependency row claimed authority");
  Require(Contains(decision_row, "diagnostic=" + decision.diagnostic_code),
          descriptor.type_id + " decision row lost diagnostic");
  Require(Contains(refusal, "diagnostic=" + decision.diagnostic_code),
          descriptor.type_id + " refusal row lost diagnostic");
  Require(Contains(approval, "approval_evidence_only=true"),
          descriptor.type_id + " approval row claimed engine authority");
  Require(Contains(actuator, "provider_authority_required_for_live=true"),
          descriptor.type_id + " actuator row lost provider authority guard");
  Require(Contains(evidence, "authority_scope=evidence_only"),
          descriptor.type_id + " evidence row claimed engine authority");
  Require(Contains(evidence, "parser_authority=false"),
          descriptor.type_id + " evidence row claimed parser authority");
  Require(Contains(evidence, "reference_authority=false"),
          descriptor.type_id + " evidence row claimed reference authority");
  Require(Contains(evidence, "sidecar_authority=false"),
          descriptor.type_id + " evidence row claimed sidecar authority");
  Require(Contains(evidence,
                   "transaction_visibility_recovery_authority=false"),
          descriptor.type_id + " evidence row claimed MGA/recovery authority");
}

void TestEveryCanonicalAgentHasRequiredExplainRows() {
  const auto descriptors = agents::CanonicalAgentRegistry();
  Require(!descriptors.empty(), "canonical agent registry is empty");
  for (const auto& descriptor : descriptors) {
    auto policy = agents::BaselinePolicyForAgent(descriptor);
    policy.policy_uuid = "policy:public-agent-pcr082:" + descriptor.type_id;
    policy.policy_generation = 82;
    policy.explainability_required = true;
    policy.policy_dependencies.push_back("dependency:" + descriptor.type_id);
    policy.required_metric_families.push_back("metric:" + descriptor.type_id);
    RequireExplainCategories(descriptor, policy, DecisionFor(descriptor));
  }
}

void TestOperatorApprovalExplainRowsAreExplicit() {
  const auto descriptor = agents::FindAgentType("page_allocation_manager");
  Require(descriptor.has_value(), "page_allocation_manager descriptor missing");
  auto policy = agents::BaselinePolicyForAgent(*descriptor);
  policy.policy_uuid = "policy:public-agent-pcr082:operator-approval";
  policy.policy_generation = 82;
  policy.require_manual_approval = true;
  policy.explainability_required = true;
  policy.policy_dependencies.push_back("dependency:operator-approval");
  policy.required_metric_families.push_back("metric:operator-approval");

  agents::AgentActionDecision decision;
  decision.result_class = agents::AgentActionResultClass::approval_required;
  decision.diagnostic_code = "SB_AGENT_APPROVAL.REQUIRED";
  decision.detail = "operator approval required before action dispatch";
  decision.evidence_uuid = "evidence:public-agent-pcr082:operator-approval";

  const auto rows = agents::ExplainAgentDecision(*descriptor, policy, decision);
  const auto refusal = RequireRow(rows, "refusal");
  const auto approval = RequireRow(rows, "approval");
  Require(Contains(refusal, "refused=true"),
          "approval-required decision did not publish refusal=true");
  Require(Contains(refusal, "failed_closed=false"),
          "approval-required decision was mislabeled failed_closed");
  Require(Contains(approval, "manual_approval_required=true"),
          "approval row did not require manual approval");
  Require(Contains(approval, "operator_approval_required=true"),
          "approval row did not require operator approval");
}

void TestDefaultNoClusterProviderFailsClosedWithoutMutation() {
  const auto route = agents::RouteAgentClusterProviderBoundary(
      ClusterContext(), "agent.cluster.public_pcr082", true);
  Require(route.provider_called, "cluster boundary did not call provider");
  Require(route.provider_type == "no_cluster",
          "default public release cluster provider is not no_cluster");
  Require(!route.ok, "no_cluster provider accepted production live route");
  Require(route.cluster_path_failed_closed,
          "no_cluster production route did not fail closed");
  Require(route.external_provider_required,
          "no_cluster production route did not require external provider");
  Require(HasDiagnostic(route, agents::kAgentClusterExternalProviderRequiredCode),
          "external-provider-required diagnostic missing");
  Require(HasEvidence(route, "agent_cluster_external_provider_required", "true"),
          "external-provider-required evidence missing");

  agents::AgentClusterLeaseState state;
  agents::AgentClusterLeaseRequest request;
  request.surface = agents::AgentClusterLeaseSurface::acquire_lease;
  request.agent_type_id = "cluster_scheduler_manager";
  request.instance_uuid = "cluster-agent-instance";
  request.now_microseconds = 1000;
  request.lease_duration_microseconds = 1000;
  request.production_live_path = true;
  const auto lease = agents::ApplyAgentClusterLeaseSurface(
      ClusterContext(), request, &state);
  Require(lease.provider_type == "no_cluster",
          "lease surface did not use no_cluster provider");
  Require(!lease.ok, "no_cluster provider accepted production live lease");
  Require(lease.cluster_path_failed_closed,
          "no_cluster production lease did not fail closed");
  Require(lease.external_provider_required,
          "no_cluster production lease did not require external provider");
  Require(state.state == agents::AgentClusterLeadershipState::follower,
          "no_cluster production lease mutated local state");
}

}  // namespace

int main() {
  TestEveryCanonicalAgentHasRequiredExplainRows();
  TestOperatorApprovalExplainRowsAreExplicit();
  TestDefaultNoClusterProviderFailsClosedWithoutMutation();
  return EXIT_SUCCESS;
}

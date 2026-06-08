// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "agent_cluster_boundary.hpp"
#include "agent_runtime_manager.hpp"
#include "cluster_provider/cluster_provider.hpp"

#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>

namespace {

namespace agents = scratchbird::core::agents;
namespace cluster_provider = scratchbird::engine::cluster_provider;

[[noreturn]] void Fail(const std::string& message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, const std::string& message) {
  if (!condition) {
    Fail(message);
  }
}

bool HasDiagnostic(const agents::AgentClusterBoundaryResult& result,
                   std::string_view code) {
  for (const auto& diagnostic_code : result.diagnostic_codes) {
    if (diagnostic_code == code) {
      return true;
    }
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

agents::AgentRuntimeContext ClusterContext() {
  agents::AgentRuntimeContext context;
  context.security_context_present = true;
  context.cluster_authority_available = true;
  context.standalone_edition = false;
  context.principal_uuid = "principal:agent-cluster-boundary";
  context.database_uuid = "database:agent-cluster-boundary";
  context.cluster_uuid = "cluster:agent-cluster-boundary";
  context.trace_tags.push_back("agent_cluster_provider_boundary_gate");
  return context;
}

agents::AgentRuntimeActivationEvidence ValidEvidence() {
  agents::AgentRuntimeActivationEvidence evidence;
  evidence.database_uuid = "019e0f2b-002b-7000-8000-000000000002";
  evidence.engine_instance_uuid = "engine-instance:019e0f2b-002b";
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

void RequireNoClusterVector(const agents::AgentClusterBoundaryResult& result) {
  Require(!result.ok, "no-cluster provider unexpectedly accepted cluster operation");
  Require(result.cluster_path_failed_closed, "no-cluster cluster path did not fail closed");
  Require(result.cluster_authority_required, "no-cluster result did not require cluster authority");
  Require(result.provider_name == "scratchbird.cluster.no_cluster_provider",
          "no-cluster provider name mismatch");
  Require(result.provider_type == "no_cluster", "no-cluster provider type mismatch");
  Require(result.provider_support_status == "not_enabled",
          "no-cluster provider support mismatch");
  Require(HasDiagnostic(result, agents::kAgentClusterSupportNotEnabledCode),
          "no-cluster diagnostic code missing");
  Require(HasEvidence(result, "cluster_provider", "no_cluster"),
          "no-cluster provider evidence missing");
  Require(HasEvidence(result, "cluster_provider_name",
                      "scratchbird.cluster.no_cluster_provider"),
          "no-cluster provider-name evidence missing");
  Require(HasEvidence(result, "cluster_provider_support", "not_enabled"),
          "no-cluster support evidence missing");
  Require(HasEvidence(result, "unsupported_feature", "cluster.provider"),
          "no-cluster unsupported feature evidence missing");
}

void RequireCompileLinkStubVector(const agents::AgentClusterBoundaryResult& result) {
  Require(!result.ok, "compile-link stub accepted cluster operation");
  Require(result.cluster_path_failed_closed,
          "compile-link stub cluster path did not fail closed");
  Require(result.cluster_authority_required,
          "compile-link stub result did not require cluster authority");
  Require(result.provider_name == "scratchbird.cluster.compile_link_stub_provider",
          "compile-link stub provider name mismatch");
  Require(result.provider_type == "compile_link_stub",
          "compile-link stub provider type mismatch");
  Require(result.provider_support_status == "compile_link_only",
          "compile-link stub provider support mismatch");
  Require(HasDiagnostic(
              result,
              cluster_provider::kClusterHandshakeStubCompileLinkOnlyCode),
          "compile-link stub diagnostic missing");
  Require(HasEvidence(result, "cluster_provider_type", "compile_link_stub"),
          "compile-link stub provider-type evidence missing");
  Require(HasEvidence(result, "cluster_provider_name",
                      "scratchbird.cluster.compile_link_stub_provider"),
          "compile-link stub provider-name evidence missing");
  Require(HasEvidence(result, "cluster_provider_support", "compile_link_only"),
          "compile-link stub support evidence missing");
  Require(HasEvidence(result, "unsupported_feature", "cluster.provider.stub"),
          "compile-link stub unsupported feature evidence missing");
}

void TestProviderBoundary() {
  const auto result = agents::RouteAgentClusterProviderBoundary(
      ClusterContext(), "agent.cluster.provider_boundary_gate");
  Require(result.provider_called, "agent boundary did not call cluster provider");
  if (result.provider_type == "no_cluster") {
    RequireNoClusterVector(result);
  } else if (result.provider_type == "compile_link_stub") {
    RequireCompileLinkStubVector(result);
  } else {
    Require(!result.provider_name.empty(), "external provider name missing");
    Require(!result.provider_support_status.empty(), "external provider support status missing");
  }

  const auto production_live = agents::RouteAgentClusterProviderBoundary(
      ClusterContext(), "agent.cluster.provider_boundary_gate.production", true);
  if (production_live.provider_type == "no_cluster" ||
      production_live.provider_type == "compile_link_stub") {
    Require(!production_live.ok,
            "in-tree provider satisfied production live cluster route");
    Require(production_live.cluster_path_failed_closed,
            "production live cluster route did not fail closed");
    Require(production_live.external_provider_required,
            "production live cluster route did not require external provider");
    Require(HasDiagnostic(production_live,
                          agents::kAgentClusterExternalProviderRequiredCode),
            "external provider required diagnostic missing");
    Require(HasEvidence(production_live,
                        "agent_cluster_external_provider_required", "true"),
            "external provider required evidence missing");
  }
}

void TestStandaloneSelectionHasExplicitClusterPathBoundary() {
  agents::AgentRuntimeManagerConfig config;
  config.standalone_edition = true;
  config.cluster_authority_available = false;
  const auto snapshot = agents::SelectStandaloneDatabaseLocalAgents(ValidEvidence(), config);
  Require(snapshot.status.ok, "standalone selection failed");

  const auto* cluster_only = FindDecision(snapshot, "cluster_autoscale_manager");
  Require(cluster_only != nullptr, "missing cluster_autoscale_manager decision");
  Require(!cluster_only->selected, "cluster-only agent selected in standalone build");
  Require(cluster_only->failed_closed, "cluster-only agent did not fail closed");
  Require(cluster_only->cluster_path_failed_closed,
          "cluster-only agent did not mark cluster path failed closed");

  const auto* admission = FindDecision(snapshot, "admission_control_manager");
  Require(admission != nullptr, "missing admission_control_manager decision");
  Require(admission->selected, "mixed both-deployment local projection not selected");
  Require(!admission->failed_closed, "mixed local projection was globally failed closed");
  Require(admission->cluster_path_failed_closed,
          "mixed local projection did not mark cluster path failed closed");

  const auto boundary = agents::RouteAgentClusterProviderBoundary(
      ClusterContext(), "agent.cluster.selection_cluster_path");
  if (boundary.provider_type == "no_cluster") {
    RequireNoClusterVector(boundary);
  } else if (boundary.provider_type == "compile_link_stub") {
    RequireCompileLinkStubVector(boundary);
  }

  agents::AgentClusterLeaseState lease_state;
  agents::AgentClusterLeaseRequest lease_request;
  lease_request.surface = agents::AgentClusterLeaseSurface::acquire_lease;
  lease_request.agent_type_id = "cluster_scheduler_manager";
  lease_request.instance_uuid = "cluster-agent-instance";
  lease_request.now_microseconds = 1000;
  lease_request.lease_duration_microseconds = 1000;
  lease_request.production_live_path = true;
  const auto lease_result = agents::ApplyAgentClusterLeaseSurface(
      ClusterContext(), lease_request, &lease_state);
  if (lease_result.provider_type == "no_cluster" ||
      lease_result.provider_type == "compile_link_stub") {
    Require(!lease_result.ok,
            "in-tree provider satisfied production live lease route");
    Require(lease_result.cluster_path_failed_closed,
            "production live lease route did not fail closed");
    Require(lease_result.external_provider_required,
            "production live lease route did not require external provider");
    Require(lease_state.state == agents::AgentClusterLeadershipState::follower,
            "production live stub/no-cluster lease mutated core state");
  }
}

}  // namespace

int main() {
  TestProviderBoundary();
  TestStandaloneSelectionHasExplicitClusterPathBoundary();
  return EXIT_SUCCESS;
}

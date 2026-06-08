// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "agent_cluster_boundary.hpp"
#include "cluster_provider/cluster_provider.hpp"

#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

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

agents::AgentRuntimeContext ClusterContext() {
  agents::AgentRuntimeContext context;
  context.security_context_present = true;
  context.cluster_authority_available = true;
  context.standalone_edition = false;
  context.principal_uuid = "principal:agent-cluster-leadership";
  context.database_uuid = "database:agent-cluster-leadership";
  context.cluster_uuid = "cluster:agent-cluster-leadership";
  context.trace_tags.push_back("agent_cluster_leadership_boundary_gate");
  return context;
}

agents::AgentClusterLeaseRequest Request(agents::AgentClusterLeaseSurface surface) {
  agents::AgentClusterLeaseRequest request;
  request.surface = surface;
  request.agent_type_id = "cluster_scheduler_manager";
  request.instance_uuid = "agent-instance:leader-a";
  request.now_microseconds = 1000;
  request.lease_duration_microseconds = 5000;
  return request;
}

void TestLeadershipStateNames() {
  Require(std::string(agents::AgentClusterLeadershipStateName(
              agents::AgentClusterLeadershipState::follower)) == "follower",
          "follower state name mismatch");
  Require(std::string(agents::AgentClusterLeadershipStateName(
              agents::AgentClusterLeadershipState::candidate)) == "candidate",
          "candidate state name mismatch");
  Require(std::string(agents::AgentClusterLeadershipStateName(
              agents::AgentClusterLeadershipState::leader_pending_fence)) ==
              "leader_pending_fence",
          "leader_pending_fence state name mismatch");
  Require(std::string(agents::AgentClusterLeadershipStateName(
              agents::AgentClusterLeadershipState::leader_active)) ==
              "leader_active",
          "leader_active state name mismatch");
  Require(std::string(agents::AgentClusterLeadershipStateName(
              agents::AgentClusterLeadershipState::leader_draining)) ==
              "leader_draining",
          "leader_draining state name mismatch");
  Require(std::string(agents::AgentClusterLeadershipStateName(
              agents::AgentClusterLeadershipState::lease_expired)) ==
              "lease_expired",
          "lease_expired state name mismatch");
  Require(std::string(agents::AgentClusterLeadershipStateName(
              agents::AgentClusterLeadershipState::quarantined)) ==
              "quarantined",
          "quarantined state name mismatch");
}

void TestNoClusterLeaseSurfacesFailClosed() {
  const auto probe = agents::RouteAgentClusterProviderBoundary(
      ClusterContext(), "agent.cluster.leadership_probe");
  if (probe.provider_type != "no_cluster") {
    return;
  }

  for (const auto surface : {agents::AgentClusterLeaseSurface::inspect,
                            agents::AgentClusterLeaseSurface::acquire_lease,
                            agents::AgentClusterLeaseSurface::renew_lease,
                            agents::AgentClusterLeaseSurface::failover,
                            agents::AgentClusterLeaseSurface::authorize_action}) {
    agents::AgentClusterLeaseState state;
    auto request = Request(surface);
    request.destructive_or_control_action = surface == agents::AgentClusterLeaseSurface::authorize_action;
    const auto result = agents::ApplyAgentClusterLeaseSurface(ClusterContext(), request, &state);
    Require(result.provider_called, "no-cluster lease surface did not call provider");
    Require(!result.ok, "no-cluster lease surface unexpectedly succeeded");
    Require(result.cluster_path_failed_closed,
            "no-cluster lease surface did not fail closed");
    Require(HasDiagnostic(result, agents::kAgentClusterSupportNotEnabledCode),
            "no-cluster lease surface exact diagnostic missing");
    Require(HasEvidence(result, "cluster_provider", "no_cluster"),
            "no-cluster lease surface provider evidence missing");
    Require(state.state == agents::AgentClusterLeadershipState::follower,
            "no-cluster lease surface mutated local proof state");
  }
}

void TestCompileLinkStubLeaseSurfacesFailClosed() {
  const auto probe = agents::RouteAgentClusterProviderBoundary(
      ClusterContext(), "agent.cluster.leadership_probe");
  if (probe.provider_type != "compile_link_stub") {
    return;
  }

  for (const auto surface : {agents::AgentClusterLeaseSurface::inspect,
                            agents::AgentClusterLeaseSurface::acquire_lease,
                            agents::AgentClusterLeaseSurface::renew_lease,
                            agents::AgentClusterLeaseSurface::failover,
                            agents::AgentClusterLeaseSurface::authorize_action}) {
    agents::AgentClusterLeaseState state;
    auto request = Request(surface);
    request.destructive_or_control_action =
        surface == agents::AgentClusterLeaseSurface::authorize_action;
    const auto result =
        agents::ApplyAgentClusterLeaseSurface(ClusterContext(), request, &state);
    Require(result.provider_called,
            "compile-link stub lease surface did not call provider");
    Require(!result.ok,
            "compile-link stub lease surface unexpectedly succeeded");
    Require(result.cluster_path_failed_closed,
            "compile-link stub lease surface did not fail closed");
    Require(HasDiagnostic(
                result,
                cluster_provider::kClusterHandshakeStubCompileLinkOnlyCode),
            "compile-link stub lease surface exact diagnostic missing");
    Require(HasEvidence(result, "cluster_provider_type", "compile_link_stub"),
            "compile-link stub lease surface provider evidence missing");
    Require(state.state == agents::AgentClusterLeadershipState::follower,
            "compile-link stub lease surface mutated local proof state");
  }
}

}  // namespace

int main() {
  TestLeadershipStateNames();
  TestNoClusterLeaseSurfacesFailClosed();
  TestCompileLinkStubLeaseSurfacesFailClosed();
  return EXIT_SUCCESS;
}

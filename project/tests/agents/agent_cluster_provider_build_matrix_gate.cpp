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

agents::AgentRuntimeContext ClusterContext() {
  agents::AgentRuntimeContext context;
  context.security_context_present = true;
  context.cluster_authority_available = true;
  context.standalone_edition = false;
  context.principal_uuid = "principal:agent-cluster-build-matrix";
  context.database_uuid = "database:agent-cluster-build-matrix";
  context.cluster_uuid = "cluster:agent-cluster-build-matrix";
  return context;
}

void TestCompileTimeProviderMatrix() {
  const auto info = cluster_provider::DescribeClusterProvider();
  Require(cluster_provider::ClusterProviderSupportsExecution() == info.supports_execution,
          "provider support helper disagrees with provider info");

#if defined(SCRATCHBIRD_CLUSTER_PROVIDER_NO_CLUSTER)
  Require(info.provider_name == std::string_view("scratchbird.cluster.no_cluster_provider"),
          "default build did not link no-cluster provider");
  Require(info.provider_type == std::string_view("no_cluster"),
          "default build provider type mismatch");
  Require(info.support_status == std::string_view("not_enabled"),
          "default build support status mismatch");
  Require(!info.supports_execution,
          "default build unexpectedly supports cluster execution");
  const auto result = agents::RouteAgentClusterProviderBoundary(
      ClusterContext(), "agent.cluster.build_matrix");
  Require(!result.ok, "default no-cluster provider accepted cluster operation");
  Require(HasDiagnostic(result, agents::kAgentClusterSupportNotEnabledCode),
          "default no-cluster exact diagnostic missing");
#elif defined(SCRATCHBIRD_CLUSTER_PROVIDER_STUB)
  Require(info.provider_name ==
              std::string_view("scratchbird.cluster.compile_link_stub_provider"),
          "stub build did not link compile-link provider");
  Require(info.provider_type == std::string_view("compile_link_stub"),
          "stub build provider type mismatch");
  Require(info.support_status == std::string_view("compile_link_only"),
          "stub build support status mismatch");
  Require(!info.external_provider,
          "public stub build claimed external-provider authority");
  Require(info.compile_link_only,
          "public stub build did not mark compile-link-only scope");
  Require(!info.supports_execution,
          "public stub build exposed execution support");
  Require(!info.supports_route_admission,
          "public stub build exposed route admission support");
  const auto result = agents::RouteAgentClusterProviderBoundary(
      ClusterContext(), "agent.cluster.build_matrix");
  Require(!result.ok, "compile-link stub provider accepted cluster operation");
  Require(result.cluster_path_failed_closed,
          "compile-link stub provider did not fail closed");
  Require(HasDiagnostic(
              result,
              cluster_provider::kClusterHandshakeStubCompileLinkOnlyCode),
          "compile-link stub exact diagnostic missing");
#elif defined(SCRATCHBIRD_CLUSTER_PROVIDER_EXTERNAL)
  Require(!info.provider_name.empty(), "external provider name missing");
  Require(!info.provider_type.empty(), "external provider type missing");
  Require(!info.support_status.empty(), "external provider support status missing");
#else
  Fail("cluster provider compile-time selection macro missing");
#endif
}

}  // namespace

int main() {
  TestCompileTimeProviderMatrix();
  return EXIT_SUCCESS;
}

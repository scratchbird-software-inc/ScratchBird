// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// SPDX-License-Identifier: Apache-2.0
// Copyright (c) ScratchBird contributors
//
// PUBLIC_CLUSTER_BOUNDARY_CLEANUP_GATE

#include "agent_cluster_boundary.hpp"
#include "cluster_provider/cluster_provider.hpp"

#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>

namespace {

namespace agents = scratchbird::core::agents;
namespace api = scratchbird::engine::internal_api;
namespace cluster_provider = scratchbird::engine::cluster_provider;

[[noreturn]] void Fail(const std::string& message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, const std::string& message) {
  if (!condition) { Fail(message); }
}

bool HasDiagnostic(const api::EngineApiResult& result, std::string_view code) {
  for (const auto& diagnostic : result.diagnostics) {
    if (diagnostic.code == code) { return true; }
  }
  return false;
}

bool HasAgentDiagnostic(const agents::AgentClusterBoundaryResult& result,
                        std::string_view code) {
  for (const auto& diagnostic : result.diagnostic_codes) {
    if (diagnostic == code) { return true; }
  }
  return false;
}

bool HasEvidence(const api::EngineApiResult& result,
                 std::string_view kind,
                 std::string_view id) {
  for (const auto& evidence : result.evidence) {
    if (evidence.evidence_kind == kind && evidence.evidence_id == id) {
      return true;
    }
  }
  return false;
}

bool HasAgentEvidence(const agents::AgentClusterBoundaryResult& result,
                      std::string_view kind,
                      std::string_view id) {
  for (const auto& evidence : result.evidence) {
    if (evidence.evidence_kind == kind && evidence.evidence_id == id) {
      return true;
    }
  }
  return false;
}

bool HasUnsupportedFeature(const api::EngineApiResult& result,
                           std::string_view feature) {
  for (const auto& unsupported : result.unsupported_features) {
    if (unsupported.feature == feature && !unsupported.reason.empty()) {
      return true;
    }
  }
  return false;
}

std::string FieldValue(const api::EngineRowValue& row, std::string_view name) {
  for (const auto& [field_name, value] : row.fields) {
    if (field_name == name) { return value.encoded_value; }
  }
  Fail("missing provider info field " + std::string(name));
}

api::EngineRequestContext EngineContext() {
  api::EngineRequestContext context;
  context.security_context_present = true;
  context.cluster_authority_available = true;
  context.database_uuid.canonical = "database:public-cluster-boundary-pcr097";
  context.cluster_uuid.canonical = "cluster:public-cluster-boundary-pcr097";
  context.principal_uuid.canonical = "principal:public-cluster-boundary-pcr097";
  context.trace_tags.push_back("public_cluster_provider_boundary_cleanup_gate");
  return context;
}

cluster_provider::ClusterProviderRequest ProviderRequest(std::string operation_id) {
  cluster_provider::ClusterProviderRequest request;
  request.context = EngineContext();
  request.envelope.operation_id = std::move(operation_id);
  request.envelope.opcode = "SBLR_PUBLIC_CLUSTER_BOUNDARY_CLEANUP_GATE";
  request.envelope.trace_key = "public-cluster-boundary-cleanup-gate";
  request.envelope.requires_security_context = true;
  request.envelope.requires_cluster_authority = true;
  request.envelope.contains_sql_text = false;
  request.api_request.context = request.context;
  request.api_request.operation_id = request.envelope.operation_id;
  return request;
}

agents::AgentRuntimeContext AgentContext() {
  agents::AgentRuntimeContext context;
  context.security_context_present = true;
  context.cluster_authority_available = true;
  context.standalone_edition = false;
  context.principal_uuid = "principal:public-cluster-boundary-pcr097";
  context.database_uuid = "database:public-cluster-boundary-pcr097";
  context.cluster_uuid = "cluster:public-cluster-boundary-pcr097";
  context.trace_tags.push_back("public_cluster_provider_boundary_cleanup_gate");
  return context;
}

void TestProviderInfoAndFailClosedExecution() {
  const auto info = cluster_provider::DescribeClusterProvider();
  const std::string provider_type(info.provider_type);
  const std::string provider_support(info.support_status);
  Require(provider_type == "no_cluster" || provider_type == "compile_link_stub",
          "public build linked an unexpected in-tree cluster provider: " +
              provider_type);
  Require(provider_support == "not_enabled" ||
              provider_support == "compile_link_only",
          "public cluster provider reported executable support: " +
              provider_support);
  Require(!info.supports_execution,
          "in-tree cluster provider reported execution support");
  Require(!cluster_provider::ClusterProviderSupportsExecution(),
          "provider support helper reported execution support");
  Require(std::string(cluster_provider::ClusterProviderMode()) == provider_type,
          "provider mode did not match provider type");

  auto inspect_request =
      ProviderRequest(std::string(cluster_provider::kClusterProviderInfoOperationId));
  const auto inspect = cluster_provider::InspectClusterProvider(inspect_request);
  Require(inspect.ok, "provider inspect route failed");
  Require(inspect.result_shape.result_kind ==
              std::string(cluster_provider::kClusterProviderInfoResultKind),
          "provider inspect route used unexpected result kind");
  Require(inspect.result_shape.rows.size() == 1,
          "provider inspect route should return exactly one info row");
  const auto& row = inspect.result_shape.rows.front();
  Require(FieldValue(row, "provider_type") == provider_type,
          "provider inspect row lost provider type");
  Require(FieldValue(row, "support_status") == provider_support,
          "provider inspect row lost support status");
  Require(FieldValue(row, "supports_execution") == "false",
          "provider inspect row claimed execution support");

  auto execute_request =
      ProviderRequest("cluster.public_release_boundary_cleanup");
  const auto executed = cluster_provider::ExecuteClusterOperation(execute_request);
  Require(!executed.ok, "in-tree cluster provider accepted execution");
  Require(executed.cluster_authority_required,
          "in-tree cluster provider did not require cluster authority");
  Require(executed.result_shape.rows.empty(),
          "in-tree cluster provider emitted mutable result rows");
  Require(HasEvidence(executed, "cluster_provider_type", provider_type),
          "provider evidence lost provider type");
  Require(HasEvidence(executed, "cluster_provider_support", provider_support),
          "provider evidence lost support status");
  Require(HasEvidence(executed, "cluster_operation",
                      "cluster.public_release_boundary_cleanup"),
          "provider evidence lost operation id");

  if (provider_type == "no_cluster") {
    Require(HasDiagnostic(executed,
                          cluster_provider::kClusterSupportNotEnabledCode),
            "no_cluster provider did not publish support-not-enabled diagnostic");
    Require(HasUnsupportedFeature(executed, "cluster.provider"),
            "no_cluster provider did not publish unsupported feature");
  } else {
    Require(HasDiagnostic(
                executed,
                cluster_provider::kClusterHandshakeStubCompileLinkOnlyCode),
            "compile-link stub did not publish compile-link-only diagnostic");
    Require(HasUnsupportedFeature(executed, "cluster.provider.stub"),
            "compile-link stub did not publish unsupported feature");
  }
}

void TestAgentBoundaryStillRequiresExternalProvider() {
  const auto info = cluster_provider::DescribeClusterProvider();
  const std::string provider_type(info.provider_type);
  const auto route = agents::RouteAgentClusterProviderBoundary(
      AgentContext(), "agent.cluster.public_pcr097", true);
  Require(route.provider_called, "agent boundary did not call provider");
  Require(route.provider_type == provider_type,
          "agent boundary observed a different provider type");
  Require(!route.ok, "agent cluster boundary accepted in-tree provider");
  Require(route.cluster_path_failed_closed,
          "agent cluster boundary did not fail closed");
  Require(route.cluster_authority_required,
          "agent cluster boundary did not require cluster authority");
  Require(route.external_provider_required,
          "agent cluster boundary did not require external provider");
  Require(HasAgentDiagnostic(route,
                             agents::kAgentClusterExternalProviderRequiredCode),
          "agent external-provider-required diagnostic missing");
  Require(HasAgentEvidence(route, "agent_cluster_external_provider_required",
                           "true"),
          "agent external-provider-required evidence missing");

  agents::AgentClusterLeaseState state;
  agents::AgentClusterLeaseRequest request;
  request.surface = agents::AgentClusterLeaseSurface::acquire_lease;
  request.agent_type_id = "cluster_scheduler_manager";
  request.instance_uuid = "public-cluster-boundary-instance";
  request.now_microseconds = 100;
  request.lease_duration_microseconds = 500;
  request.production_live_path = true;
  const auto lease = agents::ApplyAgentClusterLeaseSurface(
      AgentContext(), request, &state);
  Require(!lease.ok, "agent lease surface accepted in-tree provider");
  Require(lease.cluster_path_failed_closed,
          "agent lease surface did not fail closed");
  Require(lease.external_provider_required,
          "agent lease surface did not require external provider");
  Require(state.state == agents::AgentClusterLeadershipState::follower,
          "agent lease surface mutated local state");
}

}  // namespace

int main() {
  TestProviderInfoAndFailClosedExecution();
  TestAgentBoundaryStillRequiresExternalProvider();
  return EXIT_SUCCESS;
}

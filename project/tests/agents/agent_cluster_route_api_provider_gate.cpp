// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "agents/agent_management_api.hpp"
#include "cluster_provider/cluster_provider.hpp"
#include "sblr_dispatch.hpp"
#include "sblr_engine_envelope.hpp"

#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>

namespace {

namespace api = scratchbird::engine::internal_api;
namespace cluster_provider = scratchbird::engine::cluster_provider;
namespace sblr = scratchbird::engine::sblr;

[[noreturn]] void Fail(const std::string& message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, const std::string& message) {
  if (!condition) {
    Fail(message);
  }
}

bool HasApiDiagnostic(const api::EngineApiResult& result, std::string_view code) {
  for (const auto& diagnostic : result.diagnostics) {
    if (diagnostic.code == code) {
      return true;
    }
  }
  return false;
}

bool HasDispatchDiagnostic(const sblr::SblrDispatchResult& result,
                           std::string_view code) {
  for (const auto& diagnostic : result.diagnostics) {
    if (diagnostic.code == code) {
      return true;
    }
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

bool HasUnsupportedFeature(const api::EngineApiResult& result,
                           std::string_view feature) {
  for (const auto& unsupported : result.unsupported_features) {
    if (unsupported.feature == feature) {
      return true;
    }
  }
  return false;
}

bool HasRowFieldValue(const api::EngineApiResult& result,
                      std::string_view field_name,
                      std::string_view field_value) {
  for (const auto& row : result.result_shape.rows) {
    for (const auto& field : row.fields) {
      if (field.first == field_name &&
          field.second.encoded_value == field_value) {
        return true;
      }
    }
  }
  return false;
}

api::EngineRequestContext EngineContext(bool security_context_present = true) {
  api::EngineRequestContext context;
  context.request_id = "pfar-013a-agent-cluster-route";
  context.security_context_present = security_context_present;
  context.cluster_authority_available = cluster_provider::ClusterProviderSupportsExecution();
  context.database_uuid.canonical = "019f013a-0000-7000-8000-000000000001";
  context.cluster_uuid.canonical = "019f013a-0000-7000-8000-000000000002";
  context.node_uuid.canonical = "019f013a-0000-7000-8000-000000000003";
  context.principal_uuid.canonical = "019f013a-0000-7000-8000-000000000004";
  context.session_uuid.canonical = "019f013a-0000-7000-8000-000000000005";
  context.statement_uuid.canonical = "019f013a-0000-7000-8000-000000000006";
  context.trace_tags = {
      "right:OBS_AGENT_STATE_READ",
      "right:OBS_CLUSTER_HEALTH_INSPECT",
      "agent_cluster_route_api_provider_gate",
  };
  return context;
}

sblr::SblrDispatchRequest ClusterSysAgentsDispatchRequest(
    bool security_context_present = true) {
  sblr::SblrDispatchRequest request;
  request.context = EngineContext(security_context_present);
  request.envelope = sblr::MakeSblrEnvelope(
      "cluster.sys.agents",
      "SBLR_CLUSTER_SYS_AGENTS",
      "trace.pfar013a.cluster_sys_agents");
  request.envelope.result_shape = "cluster.provider.stub.v1";
  request.envelope.diagnostic_shape = "diagnostic.canonical_message_vector";
  request.envelope.requires_security_context = true;
  request.envelope.requires_cluster_authority = true;
  request.envelope.contains_sql_text = false;
  request.api_request.context = request.context;
  request.api_request.operation_id = "cluster.sys.agents";
  return request;
}

void RequireProviderVector(const api::EngineApiResult& result,
                           bool expect_api_route_evidence) {
  Require(result.operation_id == "cluster.sys.agents",
          "cluster sys agents operation id changed");
  const auto provider = cluster_provider::DescribeClusterProvider();
  if (provider.provider_type == std::string_view("compile_link_stub")) {
    Require(!result.ok, "compile-link stub accepted cluster.sys.agents");
    Require(result.cluster_authority_required,
            "compile-link stub result did not require cluster authority");
    Require(result.result_shape.rows.empty(),
            "compile-link stub emitted mutable result rows");
    Require(HasApiDiagnostic(
                result,
                cluster_provider::kClusterHandshakeStubCompileLinkOnlyCode),
            "compile-link stub diagnostic missing");
    Require(HasEvidence(result, "cluster_provider_type", "compile_link_stub"),
            "compile-link stub provider-type evidence missing");
    Require(HasEvidence(result, "cluster_provider_name",
                        "scratchbird.cluster.compile_link_stub_provider"),
            "compile-link stub provider name evidence missing");
    Require(HasEvidence(result, "cluster_provider_support", "compile_link_only"),
            "compile-link stub support evidence missing");
  } else if (provider.supports_execution) {
    Require(result.ok, "external provider did not accept cluster.sys.agents");
    Require(HasEvidence(result, "cluster_provider_boundary", "provider_invoked") ||
                HasEvidence(result, "agent_cluster_api_route", "provider_boundary"),
            "external provider route evidence missing");
  } else {
    Require(!result.ok, "no-cluster build accepted cluster.sys.agents");
    Require(result.cluster_authority_required,
            "no-cluster result did not require cluster authority");
    Require(HasApiDiagnostic(result, "SBLR.CLUSTER.SUPPORT_NOT_ENABLED"),
            "no-cluster diagnostic missing");
    Require(!HasApiDiagnostic(
                result,
                cluster_provider::kClusterHandshakeStubCompileLinkOnlyCode),
            "no-cluster build returned compile-link stub diagnostic");
    Require(HasEvidence(result, "cluster_provider", "no_cluster"),
            "no-cluster provider evidence missing");
    Require(HasEvidence(result, "cluster_provider_name",
                        "scratchbird.cluster.no_cluster_provider"),
            "no-cluster provider name evidence missing");
    Require(HasEvidence(result, "cluster_provider_support", "not_enabled"),
            "no-cluster support evidence missing");
    Require(HasUnsupportedFeature(result, "cluster.provider"),
            "no-cluster unsupported-feature vector missing");
  }
  if (expect_api_route_evidence) {
    Require(HasEvidence(result, "agent_cluster_api_route", "provider_boundary"),
            "direct API did not mark provider-boundary evidence");
  }
}

void TestDirectApiProviderBoundary() {
  api::EngineClusterSysAgentsRequest request;
  request.context = EngineContext();
  request.operation_id = "cluster.sys.agents";
  const auto result = api::EngineClusterSysAgents(request);
  RequireProviderVector(result, true);
}

void TestSblrProviderBoundary() {
  const auto dispatch = sblr::DispatchSblrOperation(
      ClusterSysAgentsDispatchRequest());
  Require(dispatch.envelope_validated,
          "SBLR cluster.sys.agents envelope was not validated");
  Require(dispatch.accepted && dispatch.dispatched_to_api,
          "SBLR cluster.sys.agents did not dispatch");
  RequireProviderVector(dispatch.api_result, false);
  const auto provider = cluster_provider::DescribeClusterProvider();
  if (provider.provider_type == std::string_view("compile_link_stub")) {
    Require(HasDispatchDiagnostic(
                dispatch,
                cluster_provider::kClusterHandshakeStubCompileLinkOnlyCode),
            "SBLR compile-link stub dispatch diagnostic missing");
  } else if (!provider.supports_execution) {
    Require(HasDispatchDiagnostic(dispatch, "SBLR.CLUSTER.SUPPORT_NOT_ENABLED"),
            "SBLR no-cluster dispatch diagnostic missing");
  }
}

void TestSecurityRefusesBeforeProvider() {
  api::EngineClusterSysAgentsRequest request;
  request.context = EngineContext(false);
  request.operation_id = "cluster.sys.agents";
  const auto direct = api::EngineClusterSysAgents(request);
  Require(!direct.ok, "direct API accepted missing security context");
  Require(HasApiDiagnostic(direct, "SB_AGENT_SECURITY.RIGHT_REQUIRED"),
          "direct API missing-security diagnostic changed");
  Require(!HasEvidence(direct, "cluster_provider", "no_cluster") &&
              !HasEvidence(direct, "cluster_provider_type", "compile_link_stub"),
          "direct API called provider before security validation");

  const auto dispatch = sblr::DispatchSblrOperation(
      ClusterSysAgentsDispatchRequest(false));
  Require(dispatch.envelope_validated,
          "missing-security SBLR envelope was not validated");
  Require(!dispatch.accepted && !dispatch.dispatched_to_api,
          "missing-security SBLR dispatch reached provider");
  Require(HasDispatchDiagnostic(dispatch,
                                "SB_SBLR_DISPATCH_SECURITY_CONTEXT_REQUIRED"),
          "SBLR missing-security diagnostic changed");
  Require(!HasEvidence(dispatch.api_result, "cluster_provider", "no_cluster") &&
              !HasEvidence(dispatch.api_result, "cluster_provider_type",
                           "compile_link_stub"),
          "SBLR missing-security path produced provider evidence");
}

void TestLocalSysAgentsStillWorksWithoutClusterProvider() {
  api::EngineSysAgentsRequest request;
  request.context = EngineContext();
  request.context.cluster_authority_available = false;
  request.operation_id = "sys.agents";
  const auto result = api::EngineSysAgents(request);
  Require(result.ok, "local sys.agents projection failed");
  Require(!result.result_shape.rows.empty(),
          "local sys.agents projection returned no rows");
  Require(HasEvidence(result, "sys_surface", "sys.agents"),
          "local sys.agents evidence missing");
  Require(!HasEvidence(result, "cluster_provider", "no_cluster") &&
              !HasEvidence(result, "cluster_provider_type", "compile_link_stub"),
          "local sys.agents projection entered cluster provider");
  Require(!HasRowFieldValue(result, "agent_type", "cluster_autoscale_manager"),
          "local sys.agents exposed cluster-only agent");
  Require(HasRowFieldValue(result, "agent_type", "admission_control_manager"),
          "local sys.agents suppressed valid local/both agent projection");
}

}  // namespace

int main() {
  TestDirectApiProviderBoundary();
  TestSblrProviderBoundary();
  TestSecurityRefusesBeforeProvider();
  TestLocalSysAgentsStillWorksWithoutClusterProvider();
  return EXIT_SUCCESS;
}

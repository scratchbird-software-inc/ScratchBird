// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "cluster_provider/cluster_provider.hpp"
#include "sblr_dispatch.hpp"

#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <unordered_set>

namespace {

namespace api = scratchbird::engine::internal_api;
namespace cluster_provider = scratchbird::engine::cluster_provider;
namespace sblr = scratchbird::engine::sblr;

void Require(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << message << '\n';
    std::exit(EXIT_FAILURE);
  }
}

bool HasApiDiagnostic(const api::EngineApiResult& result, std::string_view code) {
  for (const auto& diagnostic : result.diagnostics) {
    if (diagnostic.code == code) return true;
  }
  return false;
}

bool HasDispatchDiagnostic(const sblr::SblrDispatchResult& result, std::string_view code) {
  for (const auto& diagnostic : result.diagnostics) {
    if (diagnostic.code == code) return true;
  }
  return false;
}

bool HasEvidence(const api::EngineApiResult& result,
                 std::string_view kind,
                 std::string_view id) {
  for (const auto& evidence : result.evidence) {
    if (evidence.evidence_kind == kind && evidence.evidence_id == id) return true;
  }
  return false;
}

bool RowFieldEquals(const api::EngineApiResult& result,
                    std::string_view field_name,
                    std::string_view expected_value) {
  if (result.result_shape.rows.empty()) return false;
  for (const auto& field : result.result_shape.rows.front().fields) {
    if (field.first == field_name && field.second.encoded_value == expected_value) return true;
  }
  return false;
}

bool HasUnsupportedFeature(const api::EngineApiResult& result,
                           std::string_view feature) {
  for (const auto& unsupported : result.unsupported_features) {
    if (unsupported.feature == feature) return true;
  }
  return false;
}

void VerifyNormalizedCommandBoundaryManifest() {
  Require(cluster_provider::RequiredClusterProviderCommandBoundarySet().size() == 59,
          "normalized cluster command boundary manifest size mismatch");
  Require(cluster_provider::RequiredClusterProviderPreAdmissionRefusalSet().size() == 3,
          "cluster provider exact refusal set size mismatch");

  std::unordered_set<std::string_view> normalized_commands;
  std::unordered_set<std::string_view> expected_operation_ids;
  std::size_t routed_count = 0;
  std::size_t refusal_count = 0;
  for (const auto& command :
       cluster_provider::RequiredClusterProviderCommandBoundarySet()) {
    Require(normalized_commands.insert(command.normalized_command).second,
            "duplicate normalized cluster command boundary entry");
    if (command.provider_routed) {
      ++routed_count;
      Require(cluster_provider::ContainsProviderToken(
                  cluster_provider::RequiredClusterProviderOperationSet(),
                  command.normalized_command),
              "provider-routed normalized command missing from operation admission set");
      Require(command.provider_operation_id != "exact_refusal_no_provider_call",
              "provider-routed command used exact-refusal operation id");
      expected_operation_ids.insert(command.normalized_command);
      expected_operation_ids.insert(command.provider_operation_id);
      Require(cluster_provider::ContainsProviderToken(
                  cluster_provider::RequiredClusterProviderOperationSet(),
                  command.provider_operation_id),
              "provider-routed compatibility operation id missing from admission set");
    } else {
      ++refusal_count;
      Require(command.provider_operation_id == "exact_refusal_no_provider_call",
              "exact refusal command was mapped to provider operation");
      Require(cluster_provider::ContainsProviderToken(
                  cluster_provider::RequiredClusterProviderPreAdmissionRefusalSet(),
                  command.normalized_command),
              "exact refusal command missing from refusal set");
    }
  }
  Require(routed_count == 56, "provider-routed command boundary count mismatch");
  Require(refusal_count == 3, "pre-provider refusal boundary count mismatch");
  Require(cluster_provider::RequiredClusterProviderOperationSet().size() ==
              expected_operation_ids.size(),
          "cluster provider route operation set does not match derived command boundary");
  for (const auto operation_id :
       cluster_provider::RequiredClusterProviderOperationSet()) {
    Require(expected_operation_ids.count(operation_id) == 1,
            "cluster provider operation admission set contains an unexpected token");
  }

  for (const std::string_view operation :
       {"cluster.query.plan_distributed",
        "cluster.query.admit_cross_node",
        "cluster.query.route_shard_read",
        "cluster.query.execute_fragment",
        "cluster.query.fanout_search",
        "cluster.query.merge_results",
        "cluster.query.aggregate_partial",
        "cluster.query.validate_safe_read"}) {
    Require(cluster_provider::ContainsProviderToken(
                cluster_provider::RequiredClusterProviderOperationSet(),
                operation),
            "distributed query normalized command missing from provider admission set");
  }
  Require(cluster_provider::ContainsProviderToken(
              cluster_provider::RequiredClusterProviderOperationSet(),
              "cluster.topology.define_region"),
          "topology normalized command missing from provider admission set");
  Require(cluster_provider::ContainsProviderToken(
              cluster_provider::RequiredClusterProviderOperationSet(),
              "cluster.tx.begin_distributed"),
          "transaction normalized command missing from provider admission set");
  Require(cluster_provider::ContainsProviderToken(
              cluster_provider::RequiredClusterProviderOperationSet(),
              "cluster.inspect_state"),
          "current inspect-state operation id missing from provider admission set");
  Require(cluster_provider::ContainsProviderToken(
              cluster_provider::RequiredClusterProviderOperationSet(),
              "cluster.inspect_routing_plan"),
          "current routing-plan operation id missing from provider admission set");
  Require(cluster_provider::ContainsProviderToken(
              cluster_provider::RequiredClusterProviderOperationSet(),
              "cluster.prepare_remote_participant_insert"),
          "current remote-participant operation id missing from provider admission set");
  Require(cluster_provider::ContainsProviderToken(
              cluster_provider::RequiredClusterProviderOperationSet(),
              "cluster.place_object"),
          "current placement operation id missing from provider admission set");
  Require(cluster_provider::ContainsProviderToken(
              cluster_provider::RequiredClusterProviderOperationSet(),
              "cluster.inspect_replication"),
          "current replication inspection operation id missing from provider admission set");
  Require(cluster_provider::ContainsProviderToken(
              cluster_provider::RequiredClusterProviderOperationSet(),
              "cluster.validate_insert_route_fence"),
          "current insert-route-fence operation id missing from provider admission set");
  Require(cluster_provider::ContainsProviderToken(
              cluster_provider::RequiredClusterProviderOperationSet(),
              "cluster.inspect_provider"),
          "current provider-inspection operation id missing from provider admission set");
  Require(!cluster_provider::ContainsProviderToken(
              cluster_provider::RequiredClusterProviderOperationSet(),
              "cluster.query.refuse_local_query_as_cluster_authority"),
          "exact refusal command leaked into provider admission set");
  Require(!cluster_provider::ContainsProviderToken(
              cluster_provider::RequiredClusterProviderOperationSet(),
              "query.plan_operation"),
          "local query operation leaked into cluster provider route set");
  Require(cluster_provider::ContainsProviderToken(
              cluster_provider::RequiredClusterProviderPreAdmissionRefusalSet(),
              "cluster.query.refuse_local_query_as_cluster_authority"),
          "local query authority refusal missing from pre-provider refusal set");
}

void VerifyProviderInfoResult(const api::EngineApiResult& result,
                              const cluster_provider::ClusterProviderInfo& info) {
  Require(result.ok, "cluster provider info command failed");
  Require(result.operation_id == cluster_provider::kClusterProviderInfoOperationId,
          "cluster provider info returned the wrong operation id");
  Require(result.result_shape.result_kind == cluster_provider::kClusterProviderInfoResultKind,
          "cluster provider info returned the wrong result kind");
  Require(result.result_shape.rows.size() == 1,
          "cluster provider info returned the wrong row count");
  Require(RowFieldEquals(result, "provider_name", info.provider_name),
          "cluster provider info returned the wrong provider name");
  Require(RowFieldEquals(result, "provider_type", info.provider_type),
          "cluster provider info returned the wrong provider type");
  Require(RowFieldEquals(result, "provider_version", info.provider_version),
          "cluster provider info returned the wrong provider version");
  Require(RowFieldEquals(result, "support_status", info.support_status),
          "cluster provider info returned the wrong support status");
  Require(RowFieldEquals(result, "supports_execution", info.supports_execution ? "true" : "false"),
          "cluster provider info returned the wrong support flag");
  Require(HasEvidence(result, "cluster_provider_name", info.provider_name),
          "cluster provider info evidence omitted provider name");
  Require(HasEvidence(result, "cluster_provider_type", info.provider_type),
          "cluster provider info evidence omitted provider type");
  Require(HasEvidence(result, "cluster_provider_version", info.provider_version),
          "cluster provider info evidence omitted provider version");
  Require(HasEvidence(result, "cluster_provider_support", info.support_status),
          "cluster provider info evidence omitted support status");
}

void VerifyClusterProviderProfile() {
  api::EngineRequestContext context;
  context.security_context_present = true;
  context.database_uuid.canonical = "cluster-provider-conformance-database";
  context.session_uuid.canonical = "cluster-provider-conformance-session";
  context.principal_uuid.canonical = "cluster-provider-conformance-user";

  auto envelope = sblr::MakeSblrEnvelope("cluster.topology.inspect",
                                         "SBLR_CLUSTER_TOPOLOGY_INSPECT",
                                         "cluster-provider-conformance");
  envelope.requires_security_context = true;
  envelope.requires_cluster_authority = true;

  sblr::SblrDispatchRequest request;
  request.context = context;
  request.envelope = envelope;

  const auto info = cluster_provider::DescribeClusterProvider();
  const auto mode = cluster_provider::ClusterProviderMode();
  Require((!info.supports_execution && mode == info.provider_type) ||
              (info.supports_execution && mode != "no_cluster"),
          "cluster provider mode and provider info disagree");
  Require(cluster_provider::ClusterProviderSupportsExecution() == info.supports_execution,
          "cluster provider support flag and provider info disagree");

  auto info_envelope = sblr::MakeSblrEnvelope(std::string(cluster_provider::kClusterProviderInfoOperationId),
                                             std::string(cluster_provider::kClusterProviderInfoOpcode),
                                             "cluster-provider-conformance");
  info_envelope.requires_security_context = true;
  sblr::SblrDispatchRequest info_request;
  info_request.context = context;
  info_request.envelope = info_envelope;
  const auto info_result = sblr::DispatchSblrOperation(info_request);
  Require(info_result.envelope_validated,
          "cluster provider info envelope was not validated");
  Require(info_result.accepted,
          "cluster provider info dispatch was not accepted by SBLR");
  Require(info_result.dispatched_to_api,
          "cluster provider info did not reach the provider boundary");
  VerifyProviderInfoResult(info_result.api_result, info);

  const auto result = sblr::DispatchSblrOperation(request);
  Require(result.envelope_validated, "cluster provider envelope was not validated");
  Require(result.accepted, "cluster provider dispatch was not accepted by SBLR");
  Require(result.dispatched_to_api, "cluster provider dispatch did not reach an API boundary");

  if (cluster_provider::ClusterProviderSupportsExecution()) {
    Require(result.api_result.ok, "cluster stub provider did not return a successful result");
    if (info.provider_type == "dummy") {
      Require(cluster_provider::ClusterProviderMode() == "stub",
              "cluster execution provider is not the expected public stub provider");
      Require(result.api_result.result_shape.result_kind == "cluster.provider.stub.v1",
              "cluster stub provider returned the wrong result kind");
      Require(result.api_result.result_shape.rows.size() == 1,
              "cluster stub provider returned the wrong row count");
      Require(HasEvidence(result.api_result, "cluster_provider", "stub"),
              "cluster stub provider evidence is missing");
    }
    Require(HasEvidence(result.api_result, "cluster_provider_name", info.provider_name),
            "cluster execution provider name evidence is missing");
    Require(HasEvidence(result.api_result, "cluster_provider_type", info.provider_type),
            "cluster execution provider type evidence is missing");
    Require(HasEvidence(result.api_result, "cluster_provider_support", info.support_status),
            "cluster execution provider support evidence is missing");
    if (info.provider_type == "dummy") {
      Require(HasApiDiagnostic(
                  result.api_result,
                  cluster_provider::kClusterHandshakeStubCompileLinkOnlyCode),
              "cluster stub provider diagnostic is missing");
    }
  } else if (info.provider_type == "compile_link_stub") {
    Require(cluster_provider::ClusterProviderMode() == "compile_link_stub",
            "stub build did not link the compile-link stub provider");
    Require(!result.api_result.ok,
            "compile-link stub unexpectedly executed cluster SBLR");
    Require(result.api_result.cluster_authority_required,
            "compile-link stub did not mark cluster authority as required");
    Require(HasUnsupportedFeature(result.api_result, "cluster.provider.stub"),
            "compile-link stub returned the wrong unsupported feature");
    Require(HasApiDiagnostic(result.api_result,
                             cluster_provider::kClusterHandshakeStubCompileLinkOnlyCode),
            "compile-link stub returned the wrong API diagnostic");
    Require(HasDispatchDiagnostic(result,
                                  cluster_provider::kClusterHandshakeStubCompileLinkOnlyCode),
            "compile-link stub returned the wrong dispatch diagnostic");
    Require(HasEvidence(result.api_result, "cluster_provider", "stub"),
            "compile-link stub provider evidence is missing");
    Require(HasEvidence(result.api_result, "cluster_provider_type", "compile_link_stub"),
            "compile-link stub provider type evidence is missing");
    Require(HasEvidence(result.api_result,
                        "cluster_provider_handshake_diagnostic",
                        cluster_provider::kClusterHandshakeStubCompileLinkOnlyCode),
            "compile-link stub diagnostic evidence is missing");
  } else {
    Require(cluster_provider::ClusterProviderMode() == "no_cluster",
            "non-cluster build did not link the no-cluster provider");
    Require(!result.api_result.ok, "no-cluster provider unexpectedly executed cluster SBLR");
    Require(result.api_result.cluster_authority_required,
            "no-cluster provider did not mark cluster authority as required");
    Require(result.api_result.unsupported_features.size() == 1,
            "no-cluster provider returned the wrong unsupported-feature vector size");
    Require(result.api_result.unsupported_features.front().feature == "cluster.provider",
            "no-cluster provider returned the wrong unsupported feature");
    Require(result.api_result.unsupported_features.front().reason ==
                "cluster support is not enabled in this build",
            "no-cluster provider returned the wrong unsupported reason");
    Require(HasApiDiagnostic(result.api_result, cluster_provider::kClusterSupportNotEnabledCode),
            "no-cluster provider returned the wrong API diagnostic");
    Require(HasDispatchDiagnostic(result, cluster_provider::kClusterSupportNotEnabledCode),
            "no-cluster provider returned the wrong dispatch diagnostic");
    Require(HasEvidence(result.api_result, "cluster_provider", "no_cluster"),
            "no-cluster provider evidence is missing");
    Require(HasEvidence(result.api_result, "cluster_provider_name", info.provider_name),
            "no-cluster provider name evidence is missing");
    Require(HasEvidence(result.api_result, "cluster_provider_type", info.provider_type),
            "no-cluster provider type evidence is missing");
    Require(HasEvidence(result.api_result, "cluster_provider_support", info.support_status),
            "no-cluster provider support evidence is missing");
  }
}

}  // namespace

int main() {
  VerifyNormalizedCommandBoundaryManifest();
  VerifyClusterProviderProfile();
  return EXIT_SUCCESS;
}

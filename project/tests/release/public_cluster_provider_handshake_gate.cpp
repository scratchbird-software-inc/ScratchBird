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
// PUBLIC_CLUSTER_PROVIDER_HANDSHAKE_GATE

#include "cluster_provider/cluster_provider.hpp"

#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace {

namespace api = scratchbird::engine::internal_api;
namespace cluster_provider = scratchbird::engine::cluster_provider;

[[noreturn]] void Fail(const std::string& message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, const std::string& message) {
  if (!condition) { Fail(message); }
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

bool HasHandshakeIssue(const cluster_provider::ClusterProviderHandshakeResult& result,
                       std::string_view code) {
  for (const auto& issue : result.issues) {
    if (issue.diagnostic_code == code) { return true; }
  }
  return false;
}

bool HasApiDiagnostic(const api::EngineApiResult& result, std::string_view code) {
  for (const auto& diagnostic : result.diagnostics) {
    if (diagnostic.code == code) { return true; }
  }
  return false;
}

bool HasUnsupportedFeature(const api::EngineApiResult& result,
                           std::string_view feature) {
  for (const auto& unsupported : result.unsupported_features) {
    if (unsupported.feature == feature) { return true; }
  }
  return false;
}

bool HasBoundaryCommand(std::string_view normalized_command) {
  for (const auto& command :
       cluster_provider::RequiredClusterProviderCommandBoundarySet()) {
    if (command.normalized_command == normalized_command) { return true; }
  }
  return false;
}

bool HasRoutedBoundaryCommand(std::string_view normalized_command,
                              std::string_view provider_operation_id) {
  for (const auto& command :
       cluster_provider::RequiredClusterProviderCommandBoundarySet()) {
    if (command.normalized_command == normalized_command &&
        command.provider_operation_id == provider_operation_id &&
        command.provider_routed) {
      return true;
    }
  }
  return false;
}

bool HasPreAdmissionRefusalCommand(std::string_view normalized_command) {
  return cluster_provider::ContainsProviderToken(
      cluster_provider::RequiredClusterProviderPreAdmissionRefusalSet(),
      normalized_command);
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
  context.database_uuid.canonical = "database:public-cluster-handshake-pcr102";
  context.cluster_uuid.canonical = "cluster:public-cluster-handshake-pcr102";
  context.principal_uuid.canonical = "principal:public-cluster-handshake-pcr102";
  context.trace_tags.push_back("public_cluster_provider_handshake_gate");
  return context;
}

cluster_provider::ClusterProviderRequest ProviderRequest(std::string operation_id) {
  cluster_provider::ClusterProviderRequest request;
  request.context = EngineContext();
  request.envelope.operation_id = std::move(operation_id);
  request.envelope.opcode = "SBLR_PUBLIC_CLUSTER_PROVIDER_HANDSHAKE_GATE";
  request.envelope.trace_key = "public-cluster-provider-handshake-gate";
  request.envelope.requires_security_context = true;
  request.envelope.requires_cluster_authority = true;
  request.envelope.contains_sql_text = false;
  request.api_request.context = request.context;
  request.api_request.operation_id = request.envelope.operation_id;
  return request;
}

cluster_provider::ClusterProviderInfo ValidExternalProvider() {
  cluster_provider::ClusterProviderInfo info;
  info.provider_name = "scratchbird.cluster.external_provider.contract_test";
  info.provider_type = "external_cluster_provider";
  info.provider_version = "1.0.0-test";
  info.support_status = "enabled";
  info.provider_abi_version = cluster_provider::kClusterProviderAbiVersionCurrent;
  info.provider_contract_id = cluster_provider::kClusterProviderContractId;
  info.catalog_manifest_id = cluster_provider::kClusterProviderCatalogManifestId;
  info.catalog_manifest_version =
      cluster_provider::kClusterProviderCatalogManifestVersionCurrent;
  info.catalog_record_codec_version =
      cluster_provider::kClusterProviderCatalogRecordCodecVersionCurrent;
  info.catalog_compatibility_digest =
      cluster_provider::kClusterProviderCatalogCompatibilityDigest;
  info.operation_ids = cluster_provider::RequiredClusterProviderOperationSet();
  info.feature_flags = cluster_provider::RequiredClusterProviderFeatureFlags();
  info.authority_domains = cluster_provider::RequiredClusterProviderAuthorityDomains();
  info.external_provider = true;
  info.compile_link_only = false;
  info.supports_execution = true;
  info.supports_route_admission = true;
  info.local_runtime_execution_enabled = false;
  info.mutable_by_local_core = false;
  return info;
}

void TestRequiredStaticContractCoverage() {
  Require(cluster_provider::kClusterProviderAbiVersionCurrent == 1,
          "unexpected provider ABI version");
  Require(cluster_provider::kClusterProviderCatalogManifestVersionCurrent == 1,
          "unexpected catalog manifest version");
  Require(cluster_provider::kClusterProviderCatalogRecordCodecVersionCurrent == 1,
          "unexpected catalog codec version");
  Require(std::string(cluster_provider::kClusterProviderCatalogCompatibilityDigest)
              .rfind("sha256:", 0) == 0,
          "provider catalog digest is not a sha256 digest");
  Require(std::string(cluster_provider::kClusterProviderCatalogCompatibilityDigest)
              .size() == 71,
          "provider catalog digest has an unexpected length");
  Require(cluster_provider::RequiredClusterProviderCommandBoundarySet().size() == 59,
          "normalized cluster command boundary set does not match FSC-P1");
  Require(cluster_provider::RequiredClusterProviderPreAdmissionRefusalSet().size() == 3,
          "pre-provider refusal set is incomplete");
  Require(cluster_provider::RequiredClusterProviderFeatureFlags().size() >= 7,
          "provider feature flag set is incomplete");
  Require(cluster_provider::RequiredClusterProviderAuthorityDomains().size() >= 8,
          "provider authority domain set is incomplete");
  Require(cluster_provider::ContainsProviderToken(
              cluster_provider::RequiredClusterProviderAuthorityDomains(),
              "mga.transaction_inventory.finality"),
          "MGA transaction inventory finality domain missing");

  std::unordered_set<std::string_view> normalized_commands;
  std::unordered_set<std::string_view> expected_operation_ids;
  std::size_t routed_count = 0;
  std::size_t refusal_count = 0;
  for (const auto& command :
       cluster_provider::RequiredClusterProviderCommandBoundarySet()) {
    Require(normalized_commands.insert(command.normalized_command).second,
            "duplicate normalized cluster command in provider boundary set");
    if (command.provider_routed) {
      ++routed_count;
      Require(cluster_provider::ContainsProviderToken(
                  cluster_provider::RequiredClusterProviderOperationSet(),
                  command.normalized_command),
              "provider-routed normalized command omitted from operation set");
      Require(command.provider_operation_id != "exact_refusal_no_provider_call",
              "provider-routed command used exact-refusal operation id");
      expected_operation_ids.insert(command.normalized_command);
      expected_operation_ids.insert(command.provider_operation_id);
      Require(cluster_provider::ContainsProviderToken(
                  cluster_provider::RequiredClusterProviderOperationSet(),
                  command.provider_operation_id),
              "provider-routed compatibility operation id omitted from operation set");
    } else {
      ++refusal_count;
      Require(command.provider_operation_id == "exact_refusal_no_provider_call",
              "pre-provider refusal command used a routed provider operation");
      Require(HasPreAdmissionRefusalCommand(command.normalized_command),
              "pre-provider refusal command omitted from refusal set");
    }
  }
  Require(routed_count == 56, "provider-routed normalized command count mismatch");
  Require(refusal_count == 3, "pre-provider refusal command count mismatch");
  Require(cluster_provider::RequiredClusterProviderOperationSet().size() ==
              expected_operation_ids.size(),
          "provider operation set did not match derived normalized-plus-alias set");
  for (const auto operation_id :
       cluster_provider::RequiredClusterProviderOperationSet()) {
    Require(expected_operation_ids.count(operation_id) == 1,
            "provider operation set contains an unexpected admission token");
  }

  Require(HasRoutedBoundaryCommand("cluster.query.plan_distributed",
                                   "cluster.query.plan_distributed"),
          "distributed query plan command missing from provider route set");
  Require(HasRoutedBoundaryCommand("cluster.query.execute_fragment",
                                   "cluster.query.execute_fragment"),
          "distributed query fragment command missing from provider route set");
  Require(HasRoutedBoundaryCommand("cluster.query.fanout_search",
                                   "cluster.query.fanout_search"),
          "distributed query fanout command missing from provider route set");
  Require(HasRoutedBoundaryCommand("cluster.query.merge_results",
                                   "cluster.query.merge_results"),
          "distributed query merge command missing from provider route set");
  Require(HasRoutedBoundaryCommand("cluster.query.validate_safe_read",
                                   "cluster.query.validate_safe_read"),
          "distributed safe-read validation command missing from provider route set");
  Require(HasBoundaryCommand("cluster.query.refuse_local_query_as_cluster_authority"),
          "local-query-as-cluster-authority refusal command missing");
  Require(HasPreAdmissionRefusalCommand(
              "cluster.query.refuse_local_query_as_cluster_authority"),
          "local query authority refusal was not classified as pre-provider");
  Require(cluster_provider::ContainsProviderToken(
              cluster_provider::RequiredClusterProviderOperationSet(),
              "cluster.topology.define_region"),
          "topology normalized command missing from route admission set");
  Require(cluster_provider::ContainsProviderToken(
              cluster_provider::RequiredClusterProviderOperationSet(),
              "cluster.tx.begin_distributed"),
          "transaction normalized command missing from route admission set");
  Require(cluster_provider::ContainsProviderToken(
              cluster_provider::RequiredClusterProviderOperationSet(),
              "cluster.inspect_state"),
          "current inspect-state operation id missing from route admission set");
  Require(cluster_provider::ContainsProviderToken(
              cluster_provider::RequiredClusterProviderOperationSet(),
              "cluster.inspect_routing_plan"),
          "current routing-plan operation id missing from route admission set");
  Require(cluster_provider::ContainsProviderToken(
              cluster_provider::RequiredClusterProviderOperationSet(),
              "cluster.prepare_remote_participant_insert"),
          "current remote-participant operation id missing from route admission set");
  Require(cluster_provider::ContainsProviderToken(
              cluster_provider::RequiredClusterProviderOperationSet(),
              "cluster.place_object"),
          "current placement operation id missing from route admission set");
  Require(cluster_provider::ContainsProviderToken(
              cluster_provider::RequiredClusterProviderOperationSet(),
              "cluster.inspect_replication"),
          "current replication inspection operation id missing from route admission set");
  Require(cluster_provider::ContainsProviderToken(
              cluster_provider::RequiredClusterProviderOperationSet(),
              "cluster.validate_insert_route_fence"),
          "current insert-route-fence operation id missing from route admission set");
  Require(cluster_provider::ContainsProviderToken(
              cluster_provider::RequiredClusterProviderOperationSet(),
              "cluster.inspect_provider"),
          "current provider-inspection operation id missing from route admission set");
  Require(!cluster_provider::ContainsProviderToken(
              cluster_provider::RequiredClusterProviderOperationSet(),
              "cluster.query.refuse_local_query_as_cluster_authority"),
          "exact-refusal normalized command leaked into route admission set");
  Require(!cluster_provider::ContainsProviderToken(
              cluster_provider::RequiredClusterProviderOperationSet(),
              "query.plan_operation"),
          "local query SBLR operation leaked into cluster provider route set");
}

void TestValidExternalProviderAdmitsRoute() {
  const auto info = ValidExternalProvider();
  const auto handshake = cluster_provider::ValidateClusterProviderHandshake(info);
  Require(handshake.ok, "valid external provider handshake was rejected");
  Require(handshake.route_admission_allowed,
          "valid external provider handshake did not admit routes");
  Require(!handshake.external_provider_required,
          "valid external provider still required an external provider");
  Require(handshake.diagnostic_code ==
              std::string(cluster_provider::kClusterHandshakeAcceptedCode),
          "valid external provider did not publish accepted diagnostic");

  for (const std::string_view operation :
       {"cluster.topology.define_region",
        "cluster.inspect_state",
        "cluster.query.plan_distributed",
        "cluster.tx.prepare_remote_participant",
        "cluster.prepare_remote_participant_insert",
        "cluster.query.validate_safe_read"}) {
    const auto admitted = cluster_provider::EvaluateClusterProviderRouteAdmission(
        info, operation);
    Require(admitted.ok && admitted.route_admitted,
            "valid external provider did not admit cluster route");
    Require(admitted.handshake_ok,
            "cluster route admission lost handshake-ok state");
  }
}

void TestHandshakeRefusesIncompleteOrIncompatibleProviders() {
  auto info = ValidExternalProvider();
  info.external_provider = false;
  auto result = cluster_provider::ValidateClusterProviderHandshake(info);
  Require(!result.ok, "non-external provider handshake was accepted");
  Require(result.external_provider_required,
          "non-external provider did not require external provider");
  Require(HasHandshakeIssue(
              result,
              cluster_provider::kClusterHandshakeExternalProviderRequiredCode),
          "non-external provider diagnostic missing");

  info = ValidExternalProvider();
  info.compile_link_only = true;
  result = cluster_provider::ValidateClusterProviderHandshake(info);
  Require(!result.ok, "compile-link-only provider handshake was accepted");
  Require(HasHandshakeIssue(
              result,
              cluster_provider::kClusterHandshakeStubCompileLinkOnlyCode),
          "compile-link-only diagnostic missing");

  info = ValidExternalProvider();
  ++info.provider_abi_version;
  result = cluster_provider::ValidateClusterProviderHandshake(info);
  Require(!result.ok, "ABI-mismatched provider handshake was accepted");
  Require(HasHandshakeIssue(result,
                            cluster_provider::kClusterHandshakeAbiMismatchCode),
          "ABI mismatch diagnostic missing");

  info = ValidExternalProvider();
  ++info.catalog_manifest_version;
  result = cluster_provider::ValidateClusterProviderHandshake(info);
  Require(!result.ok, "manifest-version mismatched handshake was accepted");
  Require(HasHandshakeIssue(
              result,
              cluster_provider::kClusterHandshakeCatalogMismatchCode),
          "catalog manifest mismatch diagnostic missing");

  info = ValidExternalProvider();
  info.catalog_compatibility_digest = "sha256:0000000000000000000000000000000000000000000000000000000000000000";
  result = cluster_provider::ValidateClusterProviderHandshake(info);
  Require(!result.ok, "digest-mismatched provider handshake was accepted");
  Require(HasHandshakeIssue(
              result,
              cluster_provider::kClusterHandshakeDigestMismatchCode),
          "digest mismatch diagnostic missing");

  info = ValidExternalProvider();
  info.operation_ids.pop_back();
  result = cluster_provider::ValidateClusterProviderHandshake(info);
  Require(!result.ok, "operation-incomplete provider handshake was accepted");
  Require(HasHandshakeIssue(
              result,
              cluster_provider::kClusterHandshakeOperationSetIncompleteCode),
          "operation set diagnostic missing");

  info = ValidExternalProvider();
  info.feature_flags.pop_back();
  result = cluster_provider::ValidateClusterProviderHandshake(info);
  Require(!result.ok, "feature-flag-incomplete provider handshake was accepted");
  Require(HasHandshakeIssue(
              result,
              cluster_provider::kClusterHandshakeFeatureFlagsIncompleteCode),
          "feature flag diagnostic missing");

  info = ValidExternalProvider();
  info.authority_domains.pop_back();
  result = cluster_provider::ValidateClusterProviderHandshake(info);
  Require(!result.ok, "authority-domain-incomplete provider handshake was accepted");
  Require(HasHandshakeIssue(
              result,
              cluster_provider::kClusterHandshakeAuthorityDomainsIncompleteCode),
          "authority domain diagnostic missing");

  info = ValidExternalProvider();
  info.local_runtime_execution_enabled = true;
  result = cluster_provider::ValidateClusterProviderHandshake(info);
  Require(!result.ok, "local runtime cluster execution was accepted");
  Require(HasHandshakeIssue(
              result,
              cluster_provider::kClusterHandshakeLocalRuntimeRefusedCode),
          "local runtime refusal diagnostic missing");

  info = ValidExternalProvider();
  info.mutable_by_local_core = true;
  result = cluster_provider::ValidateClusterProviderHandshake(info);
  Require(!result.ok, "local core mutation through provider was accepted");
  Require(HasHandshakeIssue(
              result,
              cluster_provider::kClusterHandshakeLocalMutationRefusedCode),
          "local mutation refusal diagnostic missing");
}

void TestRouteAdmissionRequiresHandshakeAndOperation() {
  auto info = ValidExternalProvider();
  auto admission = cluster_provider::EvaluateClusterProviderRouteAdmission(
      info, "cluster.operation.not_supported");
  Require(!admission.ok, "unsupported provider operation was admitted");
  Require(!admission.route_admitted,
          "unsupported provider operation reported route admission");
  Require(admission.handshake_ok,
          "unsupported operation should still have passed handshake");
  Require(admission.diagnostic_code ==
              std::string(
                  cluster_provider::kClusterRouteAdmissionUnsupportedOperationCode),
          "unsupported operation diagnostic mismatch");

  admission = cluster_provider::EvaluateClusterProviderRouteAdmission(
      info, "cluster.query.refuse_local_query_as_cluster_authority");
  Require(!admission.ok, "exact pre-provider refusal command was admitted");
  Require(!admission.route_admitted,
          "exact pre-provider refusal reported route admission");
  Require(admission.handshake_ok,
          "exact pre-provider refusal should still have passed handshake");
  Require(admission.diagnostic_code ==
              std::string(
                  cluster_provider::kClusterRouteAdmissionUnsupportedOperationCode),
          "exact pre-provider refusal diagnostic mismatch");

  admission = cluster_provider::EvaluateClusterProviderRouteAdmission(
      info, "query.plan_operation");
  Require(!admission.ok, "local query operation was admitted as cluster route");
  Require(!admission.route_admitted,
          "local query operation reported cluster route admission");
  Require(admission.handshake_ok,
          "local query rejection should still have passed provider handshake");

  info = ValidExternalProvider();
  info.catalog_compatibility_digest = "sha256:ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff";
  admission = cluster_provider::EvaluateClusterProviderRouteAdmission(
      info, "cluster.topology.inspect");
  Require(!admission.ok, "digest-mismatched route was admitted");
  Require(!admission.handshake_ok,
          "digest-mismatched route reported handshake success");
  Require(admission.diagnostic_code ==
              std::string(cluster_provider::kClusterHandshakeDigestMismatchCode),
          "route admission did not surface handshake digest mismatch");
}

void VerifyInTreeExecutionMessageVector(std::string_view provider_type,
                                        const api::EngineApiResult& executed,
                                        std::string_view operation_id) {
  Require(!executed.ok, "in-tree provider executed a cluster operation");
  Require(executed.operation_id == operation_id,
          "in-tree provider returned the wrong operation id");
  Require(executed.cluster_authority_required,
          "in-tree provider did not preserve cluster authority requirement");
  Require(executed.result_shape.rows.empty(),
          "in-tree provider emitted mutable result rows");
  Require(HasEvidence(executed, "cluster_operation", operation_id),
          "execution evidence lost cluster operation id");
  Require(HasEvidence(executed, "cluster_provider_handshake", "failed_closed"),
          "execution evidence lost handshake failed-closed status");
  Require(HasEvidence(executed, "cluster_provider_route_admission", "false"),
          "execution evidence lost route-admission refusal");
  if (provider_type == "no_cluster") {
    Require(HasEvidence(executed,
                        "cluster_provider_route_admission_diagnostic",
                        cluster_provider::kClusterHandshakeExternalProviderRequiredCode),
            "no-cluster execution evidence lost route-admission diagnostic");
    Require(HasUnsupportedFeature(executed, "cluster.provider"),
            "no-cluster provider returned the wrong unsupported feature");
    Require(HasApiDiagnostic(executed,
                             cluster_provider::kClusterSupportNotEnabledCode),
            "no-cluster provider returned the wrong API diagnostic");
    Require(HasEvidence(executed, "cluster_provider", "no_cluster"),
            "no-cluster provider evidence is missing");
    Require(HasEvidence(executed, "cluster_provider_type", "no_cluster"),
            "no-cluster provider type evidence is missing");
    Require(HasEvidence(executed,
                        "cluster_provider_handshake_diagnostic",
                        cluster_provider::kClusterHandshakeExternalProviderRequiredCode),
            "no-cluster handshake diagnostic evidence is missing");
  } else if (provider_type == "compile_link_stub") {
    Require(HasEvidence(executed,
                        "cluster_provider_route_admission_diagnostic",
                        cluster_provider::kClusterHandshakeStubCompileLinkOnlyCode),
            "compile-link stub execution evidence lost route-admission diagnostic");
    Require(HasUnsupportedFeature(executed, "cluster.provider.stub"),
            "compile-link stub returned the wrong unsupported feature");
    Require(HasApiDiagnostic(executed,
                             cluster_provider::kClusterHandshakeStubCompileLinkOnlyCode),
            "compile-link stub returned the wrong API diagnostic");
    Require(HasEvidence(executed, "cluster_provider", "stub"),
            "compile-link stub provider evidence is missing");
    Require(HasEvidence(executed, "cluster_provider_type", "compile_link_stub"),
            "compile-link stub provider type evidence is missing");
    Require(HasEvidence(executed,
                        "cluster_provider_handshake_diagnostic",
                        cluster_provider::kClusterHandshakeStubCompileLinkOnlyCode),
            "compile-link stub diagnostic evidence is missing");
  } else {
    Fail("unexpected in-tree provider type " + std::string(provider_type));
  }
}

void TestInTreeProviderFailsClosedBeforeRouteAdmission() {
  const auto info = cluster_provider::DescribeClusterProvider();
  const std::string provider_type(info.provider_type);
  Require(provider_type == "no_cluster" || provider_type == "compile_link_stub",
          "public release linked unexpected provider type: " + provider_type);
  Require(!info.external_provider,
          "in-tree provider claimed to be an external provider");
  Require(!info.supports_execution,
          "in-tree provider claimed execution support");
  Require(!info.supports_route_admission,
          "in-tree provider claimed route admission support");
  Require(!info.local_runtime_execution_enabled,
          "in-tree provider enabled local runtime execution");
  Require(!info.mutable_by_local_core,
          "in-tree provider enabled local core mutation");

  const auto handshake = cluster_provider::ValidateCurrentClusterProviderHandshake();
  Require(!handshake.ok, "in-tree provider handshake was accepted");
  Require(handshake.external_provider_required,
          "in-tree provider did not require an external provider");
  Require(!handshake.route_admission_allowed,
          "in-tree provider admitted cluster routes");

  const auto admitted =
      cluster_provider::AdmitCurrentClusterProviderRoute("cluster.topology.inspect");
  Require(!admitted.ok, "current in-tree provider admitted a cluster route");
  Require(!admitted.route_admitted,
          "current in-tree provider reported route admitted");
  Require(!admitted.handshake_ok,
          "current in-tree provider reported handshake success");

  auto inspect_request =
      ProviderRequest(std::string(cluster_provider::kClusterProviderInfoOperationId));
  const auto inspect = cluster_provider::InspectClusterProvider(inspect_request);
  Require(inspect.ok, "provider inspection failed");
  Require(inspect.result_shape.rows.size() == 1,
          "provider inspection should return one row");
  const auto& row = inspect.result_shape.rows.front();
  Require(FieldValue(row, "handshake_status") == "failed_closed",
          "provider inspection did not publish failed-closed handshake status");
  Require(FieldValue(row, "route_admission_allowed") == "false",
          "provider inspection claimed route admission");
  Require(FieldValue(row, "catalog_compatibility_digest") ==
              std::string(cluster_provider::kClusterProviderCatalogCompatibilityDigest),
          "provider inspection lost catalog compatibility digest");

  for (const auto operation_id :
       cluster_provider::RequiredClusterProviderOperationSet()) {
    auto execute_request = ProviderRequest(std::string(operation_id));
    const auto executed = cluster_provider::ExecuteClusterOperation(execute_request);
    VerifyInTreeExecutionMessageVector(info.provider_type, executed, operation_id);
  }

  const auto external_info = ValidExternalProvider();
  for (const auto refusal : cluster_provider::RequiredClusterProviderPreAdmissionRefusalSet()) {
    Require(!cluster_provider::ContainsProviderToken(
                cluster_provider::RequiredClusterProviderOperationSet(),
                refusal),
            "exact pre-provider refusal leaked into operation admission set");
    const auto refusal_admission =
        cluster_provider::EvaluateClusterProviderRouteAdmission(external_info, refusal);
    Require(!refusal_admission.ok,
            "exact pre-provider refusal was admitted to external provider route");
    Require(!refusal_admission.route_admitted,
            "exact pre-provider refusal reported route admission");
    Require(refusal_admission.handshake_ok,
            "exact pre-provider refusal should pass handshake before local refusal");
    Require(refusal_admission.diagnostic_code ==
                std::string(cluster_provider::kClusterRouteAdmissionUnsupportedOperationCode),
            "exact pre-provider refusal returned wrong route diagnostic");
  }
}

}  // namespace

int main() {
  TestRequiredStaticContractCoverage();
  TestValidExternalProviderAdmitsRoute();
  TestHandshakeRefusesIncompleteOrIncompatibleProviders();
  TestRouteAdmissionRequiresHandshakeAndOperation();
  TestInTreeProviderFailsClosedBeforeRouteAdmission();
  return EXIT_SUCCESS;
}

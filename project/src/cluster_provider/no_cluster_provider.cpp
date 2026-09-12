// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "cluster_provider/cluster_provider.hpp"

#include "api_diagnostics.hpp"

#include <string>

namespace scratchbird::engine::cluster_provider {
namespace {

void AddProviderEvidence(internal_api::EngineApiResult* result,
                         const ClusterProviderInfo& info) {
  const auto handshake = ValidateClusterProviderHandshake(info);
  result->evidence.push_back({"cluster_provider", std::string(info.provider_type)});
  result->evidence.push_back({"cluster_provider_name", std::string(info.provider_name)});
  result->evidence.push_back({"cluster_provider_type", std::string(info.provider_type)});
  result->evidence.push_back({"cluster_provider_version", std::string(info.provider_version)});
  result->evidence.push_back({"cluster_provider_support", std::string(info.support_status)});
  result->evidence.push_back({"cluster_provider_abi_version",
                              std::to_string(info.provider_abi_version)});
  result->evidence.push_back({"cluster_provider_contract_id",
                              std::string(info.provider_contract_id)});
  result->evidence.push_back({"cluster_catalog_manifest_id",
                              std::string(info.catalog_manifest_id)});
  result->evidence.push_back({"cluster_catalog_manifest_version",
                              std::to_string(info.catalog_manifest_version)});
  result->evidence.push_back({"cluster_catalog_record_codec_version",
                              std::to_string(info.catalog_record_codec_version)});
  result->evidence.push_back({"cluster_catalog_compatibility_digest",
                              std::string(info.catalog_compatibility_digest)});
  result->evidence.push_back({"cluster_provider_handshake",
                              handshake.ok ? "accepted" : "failed_closed"});
  result->evidence.push_back({"cluster_provider_route_admission",
                              handshake.route_admission_allowed ? "true" : "false"});
  if (!handshake.diagnostic_code.empty()) {
    result->evidence.push_back({"cluster_provider_handshake_diagnostic",
                                handshake.diagnostic_code});
  }
}

}  // namespace

ClusterProviderInfo DescribeClusterProvider() {
  ClusterProviderInfo info;
  info.provider_name = "scratchbird.cluster.no_cluster_provider";
  info.provider_type = "no_cluster";
  info.provider_version = "1.0.0";
  info.support_status = "not_enabled";
  info.provider_abi_version = kClusterProviderAbiVersionCurrent;
  info.provider_contract_id = kClusterProviderContractId;
  info.catalog_manifest_id = kClusterProviderCatalogManifestId;
  info.catalog_manifest_version = kClusterProviderCatalogManifestVersionCurrent;
  info.catalog_record_codec_version =
      kClusterProviderCatalogRecordCodecVersionCurrent;
  info.catalog_compatibility_digest =
      kClusterProviderCatalogCompatibilityDigest;
  info.external_provider = false;
  info.compile_link_only = false;
  info.supports_execution = false;
  info.supports_route_admission = false;
  info.local_runtime_execution_enabled = false;
  info.mutable_by_local_core = false;
  return info;
}

std::string_view ClusterProviderMode() {
  return DescribeClusterProvider().provider_type;
}

bool ClusterProviderSupportsExecution() {
  return DescribeClusterProvider().supports_execution;
}

internal_api::EngineApiResult InspectClusterProvider(const ClusterProviderRequest& request) {
  // SBLR_CLUSTER_INSPECT_PROVIDER is cluster-required. DescribeClusterProvider
  // remains the host-only descriptor query; an absent provider cannot publish a
  // successful SBLR inspection result (or synthesize row/descriptor identities).
  return ExecuteClusterOperation(request);
}

internal_api::EngineApiResult ExecuteClusterOperation(const ClusterProviderRequest& request) {
  const auto info = DescribeClusterProvider();
  const auto route_admission =
      EvaluateClusterProviderRouteAdmission(info, request.envelope.operation_id);
  internal_api::EngineApiResult result;
  result.ok = false;
  result.operation_id = request.envelope.operation_id;
  result.embedded_trust_mode_observed =
      request.context.trust_mode == internal_api::EngineTrustMode::embedded_in_process;
  result.cluster_authority_required = true;
  result.diagnostics.push_back(internal_api::MakeEngineApiDiagnostic(
      std::string(kClusterPathAbsentCode),
      "engine.cluster.path_absent",
      "cluster_provider=no_cluster;cluster_support=not_enabled",
      true));
  result.unsupported_features.push_back(
      {"cluster.provider", "cluster support is not enabled in this build"});
  AddProviderEvidence(&result, info);
  result.evidence.push_back({"cluster_operation", request.envelope.operation_id});
  result.evidence.push_back({"cluster_provider_route_admission_diagnostic",
                             route_admission.diagnostic_code});
  return result;
}

}  // namespace scratchbird::engine::cluster_provider

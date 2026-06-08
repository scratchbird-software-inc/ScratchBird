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

internal_api::EngineDescriptor TextColumn(std::string name) {
  internal_api::EngineDescriptor descriptor;
  descriptor.descriptor_uuid.canonical = std::move(name);
  descriptor.descriptor_kind = "scalar";
  descriptor.canonical_type_name = "text";
  descriptor.encoded_descriptor = "type=text";
  return descriptor;
}

internal_api::EngineTypedValue TextValue(std::string value) {
  internal_api::EngineTypedValue typed;
  typed.descriptor.descriptor_kind = "scalar";
  typed.descriptor.canonical_type_name = "text";
  typed.descriptor.encoded_descriptor = "type=text";
  typed.encoded_value = std::move(value);
  typed.is_null = false;
  return typed;
}

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
  const auto info = DescribeClusterProvider();
  internal_api::EngineApiResult result;
  result.ok = true;
  result.operation_id = request.envelope.operation_id.empty()
                            ? std::string(kClusterProviderInfoOperationId)
                            : request.envelope.operation_id;
  result.embedded_trust_mode_observed =
      request.context.trust_mode == internal_api::EngineTrustMode::embedded_in_process;
  result.result_shape.result_kind = std::string(kClusterProviderInfoResultKind);
  result.result_shape.columns.push_back(TextColumn("provider_name"));
  result.result_shape.columns.push_back(TextColumn("provider_type"));
  result.result_shape.columns.push_back(TextColumn("provider_version"));
  result.result_shape.columns.push_back(TextColumn("support_status"));
  result.result_shape.columns.push_back(TextColumn("supports_execution"));
  result.result_shape.columns.push_back(TextColumn("provider_abi_version"));
  result.result_shape.columns.push_back(TextColumn("catalog_manifest_id"));
  result.result_shape.columns.push_back(TextColumn("catalog_manifest_version"));
  result.result_shape.columns.push_back(TextColumn("catalog_record_codec_version"));
  result.result_shape.columns.push_back(TextColumn("catalog_compatibility_digest"));
  result.result_shape.columns.push_back(TextColumn("handshake_status"));
  result.result_shape.columns.push_back(TextColumn("route_admission_allowed"));

  const auto handshake = ValidateClusterProviderHandshake(info);
  internal_api::EngineRowValue row;
  row.requested_row_uuid.canonical = "cluster-provider-info-row-0";
  row.fields.push_back({"provider_name", TextValue(std::string(info.provider_name))});
  row.fields.push_back({"provider_type", TextValue(std::string(info.provider_type))});
  row.fields.push_back({"provider_version", TextValue(std::string(info.provider_version))});
  row.fields.push_back({"support_status", TextValue(std::string(info.support_status))});
  row.fields.push_back({"supports_execution",
                        TextValue(info.supports_execution ? "true" : "false")});
  row.fields.push_back({"provider_abi_version",
                        TextValue(std::to_string(info.provider_abi_version))});
  row.fields.push_back({"catalog_manifest_id",
                        TextValue(std::string(info.catalog_manifest_id))});
  row.fields.push_back({"catalog_manifest_version",
                        TextValue(std::to_string(info.catalog_manifest_version))});
  row.fields.push_back(
      {"catalog_record_codec_version",
       TextValue(std::to_string(info.catalog_record_codec_version))});
  row.fields.push_back({"catalog_compatibility_digest",
                        TextValue(std::string(info.catalog_compatibility_digest))});
  row.fields.push_back({"handshake_status",
                        TextValue(handshake.ok ? "accepted" : "failed_closed")});
  row.fields.push_back(
      {"route_admission_allowed",
       TextValue(handshake.route_admission_allowed ? "true" : "false")});
  result.result_shape.rows.push_back(std::move(row));

  AddProviderEvidence(&result, info);
  result.evidence.push_back({"cluster_operation", result.operation_id});
  result.diagnostics.push_back(internal_api::MakeEngineApiDiagnostic(
      "SBLR.CLUSTER.PROVIDER_INFO",
      "engine.cluster.provider_info",
      "cluster_provider=no_cluster;cluster_support=not_enabled",
      false));
  return result;
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
      std::string(kClusterSupportNotEnabledCode),
      "engine.cluster.support_not_enabled",
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

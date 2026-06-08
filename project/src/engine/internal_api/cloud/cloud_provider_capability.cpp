// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "cloud/cloud_provider_capability.hpp"

#include <algorithm>
#include <sstream>
#include <utility>

namespace scratchbird::engine::internal_api {
namespace {

bool Empty(std::string_view value) {
  return value.empty();
}

template <typename T>
bool Contains(const std::vector<T>& values, T value) {
  return std::find(values.begin(), values.end(), value) != values.end();
}

template <typename T, typename NameFn>
std::string JoinNames(const std::vector<T>& values, NameFn name_fn) {
  std::ostringstream out;
  for (std::size_t i = 0; i < values.size(); ++i) {
    if (i != 0) {
      out << ",";
    }
    out << name_fn(values[i]);
  }
  return out.str();
}

std::string JoinRouteNamesForScope(const std::vector<CloudRouteMode>& values,
                                   CloudBuildScope build_scope) {
  std::ostringstream out;
  bool first = true;
  for (const auto value : values) {
    if (build_scope == CloudBuildScope::public_single_node &&
        value == CloudRouteMode::cluster_route_refused) {
      continue;
    }
    if (!first) {
      out << ",";
    }
    first = false;
    out << CloudRouteModeName(value);
  }
  return out.str();
}

bool KnownProviderKind(CloudProviderKind kind) {
  switch (kind) {
    case CloudProviderKind::local_emulator:
    case CloudProviderKind::aws:
    case CloudProviderKind::azure:
    case CloudProviderKind::gcp:
    case CloudProviderKind::kubernetes_generic:
    case CloudProviderKind::openstack:
    case CloudProviderKind::vmware:
    case CloudProviderKind::bare_metal:
    case CloudProviderKind::hybrid:
    case CloudProviderKind::managed_scratchbird_cloud:
      return true;
  }
  return false;
}

CloudProviderDiagnostic MakeCloudDiagnostic(std::string code,
                                            std::string message_key,
                                            std::string field,
                                            std::string requested_value,
                                            std::string provider_profile_uuid,
                                            std::string detail,
                                            bool retryable = false) {
  CloudProviderDiagnostic diagnostic;
  diagnostic.code = std::move(code);
  diagnostic.message_key = std::move(message_key);
  diagnostic.field = std::move(field);
  diagnostic.requested_value = std::move(requested_value);
  diagnostic.provider_profile_uuid = std::move(provider_profile_uuid);
  diagnostic.detail = std::move(detail);
  diagnostic.error = true;
  diagnostic.retryable = retryable;
  diagnostic.audit_required = true;
  diagnostic.side_effect_free = true;
  return diagnostic;
}

void AddCloudDiagnostic(CloudProviderCapabilityResult* result,
                        CloudProviderDiagnostic diagnostic) {
  result->cloud_diagnostics.push_back(diagnostic);
  result->diagnostics.push_back(ToEngineApiDiagnostic(diagnostic));
  result->ok = false;
}

void AddProjectionDiagnostic(CloudProviderCapabilityProjectionResult* result,
                             CloudProviderDiagnostic diagnostic) {
  result->cloud_diagnostics.push_back(diagnostic);
  result->diagnostics.push_back(ToEngineApiDiagnostic(diagnostic));
  result->ok = false;
}

template <typename T, typename NameFn>
bool RequireNonEmptySet(CloudProviderCapabilityResult* result,
                        const CloudProviderCapabilityProfile& profile,
                        const std::vector<T>& values,
                        std::string field,
                        NameFn) {
  if (!values.empty()) {
    return true;
  }
  AddCloudDiagnostic(
      result,
      MakeCloudDiagnostic(kCloudProviderDiagnosticCapabilityMissing,
                          "cloud.provider.capability_missing",
                          std::move(field),
                          "",
                          profile.provider_profile_uuid,
                          "capability set is required"));
  return false;
}

void AddField(CloudProviderCapabilityProjectionRow* row,
              std::string name,
              std::string value) {
  row->fields.push_back({std::move(name), std::move(value)});
}

CloudProviderCapabilityResult ShapeFailure(std::string operation_id,
                                           CloudProviderDiagnostic diagnostic) {
  CloudProviderCapabilityResult result;
  result.operation_id = std::move(operation_id);
  AddCloudDiagnostic(&result, std::move(diagnostic));
  result.side_effects_performed = false;
  return result;
}

}  // namespace

const char* CloudProviderKindName(CloudProviderKind kind) {
  switch (kind) {
    case CloudProviderKind::local_emulator:
      return "local_emulator";
    case CloudProviderKind::aws:
      return "aws";
    case CloudProviderKind::azure:
      return "azure";
    case CloudProviderKind::gcp:
      return "gcp";
    case CloudProviderKind::kubernetes_generic:
      return "kubernetes_generic";
    case CloudProviderKind::openstack:
      return "openstack";
    case CloudProviderKind::vmware:
      return "vmware";
    case CloudProviderKind::bare_metal:
      return "bare_metal";
    case CloudProviderKind::hybrid:
      return "hybrid";
    case CloudProviderKind::managed_scratchbird_cloud:
      return "managed_scratchbird_cloud";
  }
  return "unknown";
}

const char* CloudRegionModelName(CloudRegionModel model) {
  switch (model) {
    case CloudRegionModel::region_zone:
      return "region_zone";
    case CloudRegionModel::region_availability_zone:
      return "region_availability_zone";
    case CloudRegionModel::datacenter_rack:
      return "datacenter_rack";
    case CloudRegionModel::custom_failure_domain:
      return "custom_failure_domain";
  }
  return "unknown";
}

const char* CloudProviderStatusName(CloudProviderStatus status) {
  switch (status) {
    case CloudProviderStatus::draft:
      return "draft";
    case CloudProviderStatus::supported:
      return "supported";
    case CloudProviderStatus::supported_with_limitations:
      return "supported_with_limitations";
    case CloudProviderStatus::deprecated:
      return "deprecated";
    case CloudProviderStatus::disabled:
      return "disabled";
  }
  return "unknown";
}

const char* CloudIdentityModeName(CloudIdentityMode mode) {
  switch (mode) {
    case CloudIdentityMode::workload_identity:
      return "workload_identity";
    case CloudIdentityMode::oidc_federation:
      return "oidc_federation";
    case CloudIdentityMode::managed_identity:
      return "managed_identity";
    case CloudIdentityMode::iam_role:
      return "iam_role";
    case CloudIdentityMode::service_account:
      return "service_account";
    case CloudIdentityMode::static_secret_forbidden:
      return "static_secret_forbidden";
    case CloudIdentityMode::static_secret_allowed_by_policy:
      return "static_secret_allowed_by_policy";
  }
  return "unknown";
}

const char* CloudKmsModeName(CloudKmsMode mode) {
  switch (mode) {
    case CloudKmsMode::cloud_kms:
      return "cloud_kms";
    case CloudKmsMode::managed_hsm:
      return "managed_hsm";
    case CloudKmsMode::external_hsm:
      return "external_hsm";
    case CloudKmsMode::kmip:
      return "kmip";
    case CloudKmsMode::local_key_agent:
      return "local_key_agent";
    case CloudKmsMode::manual_key_entry:
      return "manual_key_entry";
  }
  return "unknown";
}

const char* CloudBlockStorageModeName(CloudBlockStorageMode mode) {
  switch (mode) {
    case CloudBlockStorageMode::single_attach:
      return "single_attach";
    case CloudBlockStorageMode::multi_attach_fenced:
      return "multi_attach_fenced";
    case CloudBlockStorageMode::local_nvme:
      return "local_nvme";
    case CloudBlockStorageMode::network_block:
      return "network_block";
    case CloudBlockStorageMode::ephemeral:
      return "ephemeral";
  }
  return "unknown";
}

const char* CloudObjectStorageModeName(CloudObjectStorageMode mode) {
  switch (mode) {
    case CloudObjectStorageMode::standard:
      return "standard";
    case CloudObjectStorageMode::archive:
      return "archive";
    case CloudObjectStorageMode::immutable_lock:
      return "immutable_lock";
    case CloudObjectStorageMode::cross_region_replication:
      return "cross_region_replication";
    case CloudObjectStorageMode::versioned:
      return "versioned";
    case CloudObjectStorageMode::none:
      return "none";
  }
  return "unknown";
}

const char* CloudSnapshotModeName(CloudSnapshotMode mode) {
  switch (mode) {
    case CloudSnapshotMode::csi_snapshot:
      return "csi_snapshot";
    case CloudSnapshotMode::provider_disk_snapshot:
      return "provider_disk_snapshot";
    case CloudSnapshotMode::application_consistent_snapshot:
      return "application_consistent_snapshot";
    case CloudSnapshotMode::crash_consistent_snapshot:
      return "crash_consistent_snapshot";
    case CloudSnapshotMode::none:
      return "none";
  }
  return "unknown";
}

const char* CloudRouteModeName(CloudRouteMode mode) {
  switch (mode) {
    case CloudRouteMode::single_node_local:
      return "single_node_local";
    case CloudRouteMode::tcp_service:
      return "tcp_service";
    case CloudRouteMode::udp_service:
      return "udp_service";
    case CloudRouteMode::http_service:
      return "http_service";
    case CloudRouteMode::private_service:
      return "private_service";
    case CloudRouteMode::internal_lb:
      return "internal_lb";
    case CloudRouteMode::external_lb:
      return "external_lb";
    case CloudRouteMode::private_endpoint:
      return "private_endpoint";
    case CloudRouteMode::private_link:
      return "private_link";
    case CloudRouteMode::vpc_peering:
      return "vpc_peering";
    case CloudRouteMode::vpn:
      return "vpn";
    case CloudRouteMode::direct_connect:
      return "direct_connect";
    case CloudRouteMode::interconnect:
      return "interconnect";
    case CloudRouteMode::cluster_route_refused:
      return "cluster_route_refused";
    case CloudRouteMode::none:
      return "none";
  }
  return "unknown";
}

const char* CloudObservabilityModeName(CloudObservabilityMode mode) {
  switch (mode) {
    case CloudObservabilityMode::opentelemetry:
      return "opentelemetry";
    case CloudObservabilityMode::cloud_metrics:
      return "cloud_metrics";
    case CloudObservabilityMode::cloud_logs:
      return "cloud_logs";
    case CloudObservabilityMode::cloud_traces:
      return "cloud_traces";
    case CloudObservabilityMode::support_bundle:
      return "support_bundle";
    case CloudObservabilityMode::none:
      return "none";
  }
  return "unknown";
}

const char* CloudOperatorModeName(CloudOperatorMode mode) {
  switch (mode) {
    case CloudOperatorMode::none:
      return "none";
    case CloudOperatorMode::local_process:
      return "local_process";
    case CloudOperatorMode::kubernetes_operator:
      return "kubernetes_operator";
    case CloudOperatorMode::dry_run_reconciler:
      return "dry_run_reconciler";
    case CloudOperatorMode::managed_control_plane:
      return "managed_control_plane";
  }
  return "unknown";
}

const char* CloudEdgeCacheModeName(CloudEdgeCacheMode mode) {
  switch (mode) {
    case CloudEdgeCacheMode::none:
      return "none";
    case CloudEdgeCacheMode::local_sink:
      return "local_sink";
    case CloudEdgeCacheMode::signed_invalidation:
      return "signed_invalidation";
    case CloudEdgeCacheMode::provider_cdn:
      return "provider_cdn";
    case CloudEdgeCacheMode::managed_edge:
      return "managed_edge";
  }
  return "unknown";
}

const char* CloudAutoscalingModeName(CloudAutoscalingMode mode) {
  switch (mode) {
    case CloudAutoscalingMode::manual:
      return "manual";
    case CloudAutoscalingMode::horizontal:
      return "horizontal";
    case CloudAutoscalingMode::vertical:
      return "vertical";
    case CloudAutoscalingMode::storage_expansion:
      return "storage_expansion";
    case CloudAutoscalingMode::elastic_oltp:
      return "elastic_oltp";
    case CloudAutoscalingMode::serverless_refused:
      return "serverless_refused";
  }
  return "unknown";
}

const char* CloudBillingMeteringModeName(CloudBillingMeteringMode mode) {
  switch (mode) {
    case CloudBillingMeteringMode::none:
      return "none";
    case CloudBillingMeteringMode::tenant_usage:
      return "tenant_usage";
    case CloudBillingMeteringMode::node_hours:
      return "node_hours";
    case CloudBillingMeteringMode::storage_bytes:
      return "storage_bytes";
    case CloudBillingMeteringMode::iops:
      return "iops";
    case CloudBillingMeteringMode::egress_bytes:
      return "egress_bytes";
    case CloudBillingMeteringMode::support_tier:
      return "support_tier";
  }
  return "unknown";
}

EngineApiDiagnostic ToEngineApiDiagnostic(const CloudProviderDiagnostic& diagnostic) {
  EngineApiDiagnostic out;
  out.code = diagnostic.code;
  out.message_key = diagnostic.message_key;
  out.error = diagnostic.error;
  out.detail = "field=" + diagnostic.field +
               ";requested_value=" + diagnostic.requested_value +
               ";provider_profile_uuid=" + diagnostic.provider_profile_uuid +
               ";retryable=" + (diagnostic.retryable ? std::string("true") : std::string("false")) +
               ";audit_required=" + (diagnostic.audit_required ? std::string("true") : std::string("false")) +
               ";side_effect_free=" + (diagnostic.side_effect_free ? std::string("true") : std::string("false")) +
               ";detail=" + diagnostic.detail;
  return out;
}

CloudProviderCapabilityProfile MakeLocalEmulatorCloudProviderCapabilityProfile() {
  CloudProviderCapabilityProfile profile;
  profile.provider_profile_uuid = "00000000-0000-7000-8000-000000000020";
  profile.provider_kind = CloudProviderKind::local_emulator;
  profile.region_model = CloudRegionModel::custom_failure_domain;
  profile.status = CloudProviderStatus::supported;
  profile.profile_version = 1;
  profile.display_name = "local_emulator";
  profile.profile_class = "public_single_node";
  profile.identity_modes = {
      CloudIdentityMode::workload_identity,
      CloudIdentityMode::oidc_federation,
      CloudIdentityMode::service_account,
      CloudIdentityMode::static_secret_forbidden,
  };
  profile.kms_modes = {
      CloudKmsMode::local_key_agent,
      CloudKmsMode::kmip,
  };
  profile.block_storage_modes = {
      CloudBlockStorageMode::single_attach,
      CloudBlockStorageMode::local_nvme,
      CloudBlockStorageMode::ephemeral,
  };
  profile.object_storage_modes = {
      CloudObjectStorageMode::standard,
      CloudObjectStorageMode::versioned,
      CloudObjectStorageMode::immutable_lock,
      CloudObjectStorageMode::none,
  };
  profile.snapshot_modes = {
      CloudSnapshotMode::application_consistent_snapshot,
      CloudSnapshotMode::crash_consistent_snapshot,
      CloudSnapshotMode::none,
  };
  profile.route_modes = {
      CloudRouteMode::single_node_local,
      CloudRouteMode::tcp_service,
      CloudRouteMode::http_service,
      CloudRouteMode::private_service,
      CloudRouteMode::internal_lb,
      CloudRouteMode::none,
  };
  profile.observability_modes = {
      CloudObservabilityMode::opentelemetry,
      CloudObservabilityMode::support_bundle,
      CloudObservabilityMode::none,
  };
  profile.operator_modes = {
      CloudOperatorMode::none,
      CloudOperatorMode::local_process,
      CloudOperatorMode::dry_run_reconciler,
  };
  profile.edge_cache_modes = {
      CloudEdgeCacheMode::none,
      CloudEdgeCacheMode::local_sink,
      CloudEdgeCacheMode::signed_invalidation,
  };
  profile.autoscaling_modes = {
      CloudAutoscalingMode::manual,
      CloudAutoscalingMode::storage_expansion,
  };
  profile.billing_metering_modes = {
      CloudBillingMeteringMode::none,
  };
  profile.public_single_node_supported = true;
  profile.local_emulator = true;
  profile.profile_version_auditable = true;
  profile.sys_information_projectable = true;
  profile.provider_state_finality_authority = false;
  profile.provider_recovery_authority = false;
  return profile;
}

CloudProviderCapabilityRegistry MakeCloudProviderCapabilityRegistryWithLocalEmulator() {
  CloudProviderCapabilityRegistry registry;
  registry.profiles.push_back(MakeLocalEmulatorCloudProviderCapabilityProfile());
  return registry;
}

CloudProviderCapabilityResult RegisterCloudProviderCapabilityProfile(
    CloudProviderCapabilityRegistry* registry,
    CloudProviderCapabilityProfile profile) {
  constexpr const char* operation_id = "cloud.provider.register";
  if (registry == nullptr) {
    return ShapeFailure(
        operation_id,
        MakeCloudDiagnostic(kCloudProviderDiagnosticCapabilityMissing,
                            "cloud.provider.registry_missing",
                            "registry",
                            "",
                            profile.provider_profile_uuid,
                            "registry pointer is required"));
  }
  if (Empty(profile.provider_profile_uuid)) {
    return ShapeFailure(
        operation_id,
        MakeCloudDiagnostic(kCloudProviderDiagnosticCapabilityMissing,
                            "cloud.provider.capability_missing",
                            "provider_profile_uuid",
                            "",
                            "",
                            "provider profile UUID is required"));
  }
  if (!KnownProviderKind(profile.provider_kind)) {
    return ShapeFailure(
        operation_id,
        MakeCloudDiagnostic(kCloudProviderDiagnosticProviderUnsupported,
                            "cloud.provider.unsupported",
                            "provider_kind",
                            CloudProviderKindName(profile.provider_kind),
                            profile.provider_profile_uuid,
                            "provider kind is outside the provider capability registry"));
  }
  if (FindCloudProviderCapabilityProfile(*registry, profile.provider_profile_uuid) != nullptr) {
    return ShapeFailure(
        operation_id,
        MakeCloudDiagnostic(kCloudProviderDiagnosticCapabilityUnsupported,
                            "cloud.provider.duplicate_uuid",
                            "provider_profile_uuid",
                            profile.provider_profile_uuid,
                            profile.provider_profile_uuid,
                            "provider profile UUID already exists in registry"));
  }

  CloudProviderCapabilityResult shape;
  shape.operation_id = operation_id;
  shape.ok = true;
  RequireNonEmptySet(&shape, profile, profile.identity_modes, "identity_modes", CloudIdentityModeName);
  RequireNonEmptySet(&shape, profile, profile.kms_modes, "kms_modes", CloudKmsModeName);
  RequireNonEmptySet(&shape, profile, profile.block_storage_modes, "block_storage_modes", CloudBlockStorageModeName);
  RequireNonEmptySet(&shape, profile, profile.object_storage_modes, "object_storage_modes", CloudObjectStorageModeName);
  RequireNonEmptySet(&shape, profile, profile.snapshot_modes, "snapshot_modes", CloudSnapshotModeName);
  RequireNonEmptySet(&shape, profile, profile.route_modes, "route_modes", CloudRouteModeName);
  RequireNonEmptySet(&shape, profile, profile.observability_modes, "observability_modes", CloudObservabilityModeName);
  RequireNonEmptySet(&shape, profile, profile.operator_modes, "operator_modes", CloudOperatorModeName);
  RequireNonEmptySet(&shape, profile, profile.edge_cache_modes, "edge_cache_modes", CloudEdgeCacheModeName);
  RequireNonEmptySet(&shape, profile, profile.autoscaling_modes, "autoscaling_modes", CloudAutoscalingModeName);
  RequireNonEmptySet(&shape, profile, profile.billing_metering_modes, "billing_metering_modes", CloudBillingMeteringModeName);
  if (profile.profile_version == 0) {
    AddCloudDiagnostic(
        &shape,
        MakeCloudDiagnostic(kCloudProviderDiagnosticCapabilityMissing,
                            "cloud.provider.profile_version_missing",
                            "profile_version",
                            "0",
                            profile.provider_profile_uuid,
                            "profile version must be non-zero"));
  }
  if (profile.provider_state_finality_authority || profile.provider_recovery_authority) {
    AddCloudDiagnostic(
        &shape,
        MakeCloudDiagnostic(kCloudProviderDiagnosticCapabilityUnsupported,
                            "cloud.provider.authority_boundary",
                            "provider_authority",
                            "true",
                            profile.provider_profile_uuid,
                            "provider state cannot be transaction or recovery authority"));
  }
  if (!shape.ok) {
    shape.side_effects_performed = false;
    return shape;
  }

  registry->profiles.push_back(profile);
  CloudProviderCapabilityResult result;
  result.ok = true;
  result.operation_id = operation_id;
  result.profile = std::move(profile);
  result.profile_registered = true;
  result.profile_found = true;
  result.side_effects_performed = false;
  return result;
}

const CloudProviderCapabilityProfile* FindCloudProviderCapabilityProfile(
    const CloudProviderCapabilityRegistry& registry,
    std::string_view provider_profile_uuid) {
  for (const auto& profile : registry.profiles) {
    if (profile.provider_profile_uuid == provider_profile_uuid) {
      return &profile;
    }
  }
  return nullptr;
}

CloudProviderCapabilityResult ValidateCloudProviderCapabilityProfile(
    const CloudProviderCapabilityProfile& profile,
    CloudBuildScope build_scope) {
  constexpr const char* operation_id = "cloud.provider.validate";
  CloudProviderCapabilityResult result;
  result.operation_id = operation_id;
  result.profile = profile;
  result.profile_found = !profile.provider_profile_uuid.empty();
  result.side_effects_performed = false;
  result.ok = true;

  if (profile.provider_profile_uuid.empty()) {
    AddCloudDiagnostic(
        &result,
        MakeCloudDiagnostic(kCloudProviderDiagnosticProviderNotFound,
                            "cloud.provider.not_found",
                            "provider_profile_uuid",
                            "",
                            "",
                            "provider profile UUID is required"));
  }
  if (!KnownProviderKind(profile.provider_kind)) {
    AddCloudDiagnostic(
        &result,
        MakeCloudDiagnostic(kCloudProviderDiagnosticProviderUnsupported,
                            "cloud.provider.unsupported",
                            "provider_kind",
                            CloudProviderKindName(profile.provider_kind),
                            profile.provider_profile_uuid,
                            "provider kind is outside the provider capability registry"));
  }
  if (profile.status == CloudProviderStatus::disabled) {
    AddCloudDiagnostic(
        &result,
        MakeCloudDiagnostic(kCloudProviderDiagnosticProviderDisabled,
                            "cloud.provider.disabled",
                            "status",
                            CloudProviderStatusName(profile.status),
                            profile.provider_profile_uuid,
                            "provider profile is disabled"));
  }
  if (build_scope == CloudBuildScope::public_single_node &&
      !profile.public_single_node_supported) {
    AddCloudDiagnostic(
        &result,
        MakeCloudDiagnostic(kCloudProviderDiagnosticClusterFieldRefused,
                            "cloud.provider.public_scope_refused",
                            "public_single_node_support",
                            "unsupported",
                            profile.provider_profile_uuid,
                            "provider profile is private cluster scope only"));
  }
  if (profile.provider_state_finality_authority || profile.provider_recovery_authority) {
    AddCloudDiagnostic(
        &result,
        MakeCloudDiagnostic(kCloudProviderDiagnosticCapabilityUnsupported,
                            "cloud.provider.authority_boundary",
                            "provider_authority",
                            "true",
                            profile.provider_profile_uuid,
                            "provider state cannot be transaction or recovery authority"));
  }

  return result;
}

CloudProviderCapabilityProjectionResult BuildCloudProviderCapabilitySysInformationProjection(
    const CloudProviderCapabilityRegistry& registry,
    const CloudProviderCapabilityProjectionContext& context) {
  CloudProviderCapabilityProjectionResult result;
  result.ok = true;

  if (context.build_scope == CloudBuildScope::public_single_node &&
      context.include_private_cluster_fields) {
    AddProjectionDiagnostic(
        &result,
        MakeCloudDiagnostic(kCloudProviderDiagnosticClusterFieldRefused,
                            "cloud.provider.projection.cluster_field_refused",
                            "include_private_cluster_fields",
                            "true",
                            "",
                            "public sys.information projection cannot expose private cluster-only cloud fields"));
    return result;
  }

  for (const auto& profile : registry.profiles) {
    if (!profile.sys_information_projectable) {
      continue;
    }
    CloudProviderCapabilityProjectionRow row;
    AddField(&row, "provider_profile_uuid", profile.provider_profile_uuid);
    AddField(&row, "provider_kind", CloudProviderKindName(profile.provider_kind));
    AddField(&row, "region_model", CloudRegionModelName(profile.region_model));
    AddField(&row, "profile_status", CloudProviderStatusName(profile.status));
    AddField(&row,
             "public_single_node_support",
             profile.public_single_node_supported ? "supported" : "unsupported");
    AddField(&row, "identity_modes", JoinNames(profile.identity_modes, CloudIdentityModeName));
    AddField(&row, "kms_modes", JoinNames(profile.kms_modes, CloudKmsModeName));
    AddField(&row, "block_storage_modes", JoinNames(profile.block_storage_modes, CloudBlockStorageModeName));
    AddField(&row, "object_storage_modes", JoinNames(profile.object_storage_modes, CloudObjectStorageModeName));
    AddField(&row, "snapshot_modes", JoinNames(profile.snapshot_modes, CloudSnapshotModeName));
    AddField(&row, "route_modes", JoinRouteNamesForScope(profile.route_modes, context.build_scope));
    AddField(&row, "observability_modes", JoinNames(profile.observability_modes, CloudObservabilityModeName));
    AddField(&row, "operator_modes", JoinNames(profile.operator_modes, CloudOperatorModeName));
    AddField(&row, "edge_cache_modes", JoinNames(profile.edge_cache_modes, CloudEdgeCacheModeName));
    AddField(&row, "diagnostic_policy",
             "SB-CLOUD-PROVIDER-NOT-FOUND,SB-CLOUD-PROVIDER-DISABLED,"
             "SB-CLOUD-CAPABILITY-UNSUPPORTED,SB-CLOUD-KMS-MODE-UNSUPPORTED,"
             "SB-CLOUD-STATIC-SECRET-FORBIDDEN,SB-CLOUD-CLUSTER-FIELD-REFUSED");
    if (context.build_scope == CloudBuildScope::private_cluster &&
        context.include_private_cluster_fields) {
      AddField(&row,
               "private_cluster_only_fields",
               JoinNames(profile.private_cluster_only_fields,
                         [](const std::string& value) { return value.c_str(); }));
    }
    result.rows.push_back(std::move(row));
  }

  return result;
}

}  // namespace scratchbird::engine::internal_api

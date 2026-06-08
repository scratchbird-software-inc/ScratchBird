// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// SEARCH_KEY: SB_ENGINE_INTERNAL_API_CLOUD_PROVIDER_CAPABILITY
// Engine-owned cloud provider capability registry. Provider records describe
// admissible configuration only; they do not provide transaction or recovery
// authority.

#include "api_types.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace scratchbird::engine::internal_api {

inline constexpr const char* kCloudProviderDiagnosticOk = "SB-CLOUD-OK";
inline constexpr const char* kCloudProviderDiagnosticProviderNotFound =
    "SB-CLOUD-PROVIDER-NOT-FOUND";
inline constexpr const char* kCloudProviderDiagnosticProviderDisabled =
    "SB-CLOUD-PROVIDER-DISABLED";
inline constexpr const char* kCloudProviderDiagnosticProviderUnsupported =
    "SB-CLOUD-PROVIDER-UNSUPPORTED";
inline constexpr const char* kCloudProviderDiagnosticCapabilityMissing =
    "SB-CLOUD-CAPABILITY-MISSING";
inline constexpr const char* kCloudProviderDiagnosticCapabilityUnsupported =
    "SB-CLOUD-CAPABILITY-UNSUPPORTED";
inline constexpr const char* kCloudProviderDiagnosticKmsModeUnsupported =
    "SB-CLOUD-KMS-MODE-UNSUPPORTED";
inline constexpr const char* kCloudProviderDiagnosticStaticSecretForbidden =
    "SB-CLOUD-STATIC-SECRET-FORBIDDEN";
inline constexpr const char* kCloudProviderDiagnosticClusterFieldRefused =
    "SB-CLOUD-CLUSTER-FIELD-REFUSED";
inline constexpr const char* kCloudProviderDiagnosticServerlessProfileRefused =
    "SB-CLOUD-SERVERLESS-PROFILE-REFUSED";

enum class CloudBuildScope : std::uint16_t {
  public_single_node,
  private_cluster,
};

enum class CloudProviderKind : std::uint16_t {
  local_emulator,
  aws,
  azure,
  gcp,
  kubernetes_generic,
  openstack,
  vmware,
  bare_metal,
  hybrid,
  managed_scratchbird_cloud,
};

enum class CloudRegionModel : std::uint16_t {
  region_zone,
  region_availability_zone,
  datacenter_rack,
  custom_failure_domain,
};

enum class CloudProviderStatus : std::uint16_t {
  draft,
  supported,
  supported_with_limitations,
  deprecated,
  disabled,
};

enum class CloudIdentityMode : std::uint16_t {
  workload_identity,
  oidc_federation,
  managed_identity,
  iam_role,
  service_account,
  static_secret_forbidden,
  static_secret_allowed_by_policy,
};

enum class CloudKmsMode : std::uint16_t {
  cloud_kms,
  managed_hsm,
  external_hsm,
  kmip,
  local_key_agent,
  manual_key_entry,
};

enum class CloudBlockStorageMode : std::uint16_t {
  single_attach,
  multi_attach_fenced,
  local_nvme,
  network_block,
  ephemeral,
};

enum class CloudObjectStorageMode : std::uint16_t {
  standard,
  archive,
  immutable_lock,
  cross_region_replication,
  versioned,
  none,
};

enum class CloudSnapshotMode : std::uint16_t {
  csi_snapshot,
  provider_disk_snapshot,
  application_consistent_snapshot,
  crash_consistent_snapshot,
  none,
};

enum class CloudRouteMode : std::uint16_t {
  single_node_local,
  tcp_service,
  udp_service,
  http_service,
  private_service,
  internal_lb,
  external_lb,
  private_endpoint,
  private_link,
  vpc_peering,
  vpn,
  direct_connect,
  interconnect,
  cluster_route_refused,
  none,
};

enum class CloudObservabilityMode : std::uint16_t {
  opentelemetry,
  cloud_metrics,
  cloud_logs,
  cloud_traces,
  support_bundle,
  none,
};

enum class CloudOperatorMode : std::uint16_t {
  none,
  local_process,
  kubernetes_operator,
  dry_run_reconciler,
  managed_control_plane,
};

enum class CloudEdgeCacheMode : std::uint16_t {
  none,
  local_sink,
  signed_invalidation,
  provider_cdn,
  managed_edge,
};

enum class CloudAutoscalingMode : std::uint16_t {
  manual,
  horizontal,
  vertical,
  storage_expansion,
  elastic_oltp,
  serverless_refused,
};

enum class CloudBillingMeteringMode : std::uint16_t {
  none,
  tenant_usage,
  node_hours,
  storage_bytes,
  iops,
  egress_bytes,
  support_tier,
};

struct CloudProviderDiagnostic {
  std::string code = kCloudProviderDiagnosticOk;
  std::string message_key = "cloud.provider.ok";
  std::string field;
  std::string requested_value;
  std::string provider_profile_uuid;
  std::string detail;
  bool error = false;
  bool retryable = false;
  bool audit_required = false;
  bool side_effect_free = true;
};

struct CloudProviderCapabilityProfile {
  std::string provider_profile_uuid;
  CloudProviderKind provider_kind = CloudProviderKind::local_emulator;
  CloudRegionModel region_model = CloudRegionModel::custom_failure_domain;
  CloudProviderStatus status = CloudProviderStatus::draft;
  std::uint64_t profile_version = 1;
  std::string display_name;
  std::string profile_class = "public_single_node";
  std::vector<CloudIdentityMode> identity_modes;
  std::vector<CloudKmsMode> kms_modes;
  std::vector<CloudBlockStorageMode> block_storage_modes;
  std::vector<CloudObjectStorageMode> object_storage_modes;
  std::vector<CloudSnapshotMode> snapshot_modes;
  std::vector<CloudRouteMode> route_modes;
  std::vector<CloudObservabilityMode> observability_modes;
  std::vector<CloudOperatorMode> operator_modes;
  std::vector<CloudEdgeCacheMode> edge_cache_modes;
  std::vector<CloudAutoscalingMode> autoscaling_modes;
  std::vector<CloudBillingMeteringMode> billing_metering_modes;
  std::vector<std::string> private_cluster_only_fields;
  bool public_single_node_supported = true;
  bool local_emulator = false;
  bool profile_version_auditable = true;
  bool sys_information_projectable = true;
  bool provider_state_finality_authority = false;
  bool provider_recovery_authority = false;
};

struct CloudProviderCapabilityRegistry {
  std::vector<CloudProviderCapabilityProfile> profiles;
};

struct CloudProviderCapabilityResult {
  bool ok = false;
  std::string operation_id;
  std::vector<CloudProviderDiagnostic> cloud_diagnostics;
  std::vector<EngineApiDiagnostic> diagnostics;
  CloudProviderCapabilityProfile profile;
  bool profile_registered = false;
  bool profile_found = false;
  bool side_effects_performed = false;
};

struct CloudProviderCapabilityProjectionContext {
  CloudBuildScope build_scope = CloudBuildScope::public_single_node;
  bool include_private_cluster_fields = false;
  bool cluster_authority_available = false;
};

struct CloudProviderCapabilityProjectionRow {
  std::vector<std::pair<std::string, std::string>> fields;
};

struct CloudProviderCapabilityProjectionResult {
  bool ok = false;
  std::vector<CloudProviderDiagnostic> cloud_diagnostics;
  std::vector<EngineApiDiagnostic> diagnostics;
  std::vector<CloudProviderCapabilityProjectionRow> rows;
};

const char* CloudProviderKindName(CloudProviderKind kind);
const char* CloudRegionModelName(CloudRegionModel model);
const char* CloudProviderStatusName(CloudProviderStatus status);
const char* CloudIdentityModeName(CloudIdentityMode mode);
const char* CloudKmsModeName(CloudKmsMode mode);
const char* CloudBlockStorageModeName(CloudBlockStorageMode mode);
const char* CloudObjectStorageModeName(CloudObjectStorageMode mode);
const char* CloudSnapshotModeName(CloudSnapshotMode mode);
const char* CloudRouteModeName(CloudRouteMode mode);
const char* CloudObservabilityModeName(CloudObservabilityMode mode);
const char* CloudOperatorModeName(CloudOperatorMode mode);
const char* CloudEdgeCacheModeName(CloudEdgeCacheMode mode);
const char* CloudAutoscalingModeName(CloudAutoscalingMode mode);
const char* CloudBillingMeteringModeName(CloudBillingMeteringMode mode);

EngineApiDiagnostic ToEngineApiDiagnostic(const CloudProviderDiagnostic& diagnostic);

CloudProviderCapabilityProfile MakeLocalEmulatorCloudProviderCapabilityProfile();
CloudProviderCapabilityRegistry MakeCloudProviderCapabilityRegistryWithLocalEmulator();

CloudProviderCapabilityResult RegisterCloudProviderCapabilityProfile(
    CloudProviderCapabilityRegistry* registry,
    CloudProviderCapabilityProfile profile);

const CloudProviderCapabilityProfile* FindCloudProviderCapabilityProfile(
    const CloudProviderCapabilityRegistry& registry,
    std::string_view provider_profile_uuid);

CloudProviderCapabilityResult ValidateCloudProviderCapabilityProfile(
    const CloudProviderCapabilityProfile& profile,
    CloudBuildScope build_scope);

CloudProviderCapabilityProjectionResult BuildCloudProviderCapabilitySysInformationProjection(
    const CloudProviderCapabilityRegistry& registry,
    const CloudProviderCapabilityProjectionContext& context);

}  // namespace scratchbird::engine::internal_api

// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// SEARCH_KEY: SB_ENGINE_INTERNAL_API_CLOUD_DEPLOYMENT_PROFILE
// Deployment profile selector and fail-closed capability validation.

#include "cloud/cloud_provider_capability.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace scratchbird::engine::internal_api {

enum class CloudDeploymentKind : std::uint16_t {
  self_managed_vm,
  self_managed_container,
  self_managed_kubernetes,
  hybrid_cloud_on_prem,
  managed_scratchbird_cloud_dedicated,
  managed_scratchbird_cloud_shared_control_plane,
  managed_scratchbird_cloud_serverless_edge,
};

struct CloudDeploymentProfile {
  std::string deployment_profile_uuid;
  CloudDeploymentKind deployment_kind = CloudDeploymentKind::self_managed_vm;
  std::string provider_profile_uuid;
  CloudBuildScope build_scope = CloudBuildScope::public_single_node;
  std::vector<CloudIdentityMode> requested_identity_modes;
  std::vector<CloudKmsMode> requested_kms_modes;
  std::vector<CloudBlockStorageMode> requested_block_storage_modes;
  std::vector<CloudObjectStorageMode> requested_object_storage_modes;
  std::vector<CloudSnapshotMode> requested_snapshot_modes;
  std::vector<CloudRouteMode> requested_route_modes;
  std::vector<CloudObservabilityMode> requested_observability_modes;
  std::vector<CloudOperatorMode> requested_operator_modes;
  std::vector<CloudEdgeCacheMode> requested_edge_cache_modes;
  std::vector<CloudBillingMeteringMode> requested_billing_metering_modes;
  std::vector<std::string> private_cluster_only_fields;
  bool static_secret_policy_authorized = false;
  bool serverless_state_contract_active = false;
};

struct CloudDeploymentProfileValidationResult {
  bool ok = false;
  std::string operation_id = "cloud.deployment.validate";
  std::vector<CloudProviderDiagnostic> cloud_diagnostics;
  std::vector<EngineApiDiagnostic> diagnostics;
  CloudProviderCapabilityProfile selected_provider;
  bool selected_provider_found = false;
  bool profile_selector_valid = false;
  bool side_effects_performed = false;
  bool mga_authority_preserved = true;
  bool provider_state_finality_authority = false;
  bool provider_recovery_authority = false;
  bool parser_finality_authority = false;
};

const char* CloudDeploymentKindName(CloudDeploymentKind kind);

CloudDeploymentProfile MakeLocalEmulatorCloudDeploymentProfile(
    std::string provider_profile_uuid =
        MakeLocalEmulatorCloudProviderCapabilityProfile().provider_profile_uuid);

CloudDeploymentProfileValidationResult ValidateCloudDeploymentProfile(
    const CloudProviderCapabilityRegistry& registry,
    const CloudDeploymentProfile& deployment);

}  // namespace scratchbird::engine::internal_api

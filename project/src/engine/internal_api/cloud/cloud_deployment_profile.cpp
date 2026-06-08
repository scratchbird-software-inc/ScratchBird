// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "cloud/cloud_deployment_profile.hpp"

#include <algorithm>
#include <string_view>
#include <utility>

namespace scratchbird::engine::internal_api {
namespace {

template <typename T>
bool Contains(const std::vector<T>& values, T value) {
  return std::find(values.begin(), values.end(), value) != values.end();
}

CloudProviderDiagnostic MakeDeploymentDiagnostic(std::string code,
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

void AddDeploymentDiagnostic(CloudDeploymentProfileValidationResult* result,
                             CloudProviderDiagnostic diagnostic) {
  result->cloud_diagnostics.push_back(diagnostic);
  result->diagnostics.push_back(ToEngineApiDiagnostic(diagnostic));
  result->ok = false;
}

bool StartsWith(std::string_view value, std::string_view prefix) {
  return value.size() >= prefix.size() && value.substr(0, prefix.size()) == prefix;
}

bool IsClusterOnlyFieldName(std::string_view field) {
  return StartsWith(field, "cluster.") ||
         field == "route_epoch" ||
         field == "fence_token" ||
         field == "cluster_route_epoch" ||
         field == "cluster_fence_token" ||
         field == "cluster_membership" ||
         field == "cluster_metrics_root" ||
         field == "private_kms_partition_policy";
}

bool ManagedCloudDeploymentKind(CloudDeploymentKind kind) {
  return kind == CloudDeploymentKind::managed_scratchbird_cloud_dedicated ||
         kind == CloudDeploymentKind::managed_scratchbird_cloud_shared_control_plane ||
         kind == CloudDeploymentKind::managed_scratchbird_cloud_serverless_edge;
}

template <typename T, typename NameFn>
void ValidateRequestedModes(CloudDeploymentProfileValidationResult* result,
                            const std::vector<T>& provider_modes,
                            const std::vector<T>& requested_modes,
                            const CloudDeploymentProfile& deployment,
                            const char* field,
                            const char* diagnostic_code,
                            const char* message_key,
                            NameFn name_fn) {
  for (const auto requested : requested_modes) {
    if (Contains(provider_modes, requested)) {
      continue;
    }
    AddDeploymentDiagnostic(
        result,
        MakeDeploymentDiagnostic(diagnostic_code,
                                 message_key,
                                 field,
                                 name_fn(requested),
                                 deployment.provider_profile_uuid,
                                 "requested capability is absent from provider profile"));
  }
}

void CopyProviderValidationDiagnostics(CloudDeploymentProfileValidationResult* result,
                                       const CloudProviderCapabilityResult& provider_result) {
  for (const auto& diagnostic : provider_result.cloud_diagnostics) {
    AddDeploymentDiagnostic(result, diagnostic);
  }
}

}  // namespace

const char* CloudDeploymentKindName(CloudDeploymentKind kind) {
  switch (kind) {
    case CloudDeploymentKind::self_managed_vm:
      return "self_managed_vm";
    case CloudDeploymentKind::self_managed_container:
      return "self_managed_container";
    case CloudDeploymentKind::self_managed_kubernetes:
      return "self_managed_kubernetes";
    case CloudDeploymentKind::hybrid_cloud_on_prem:
      return "hybrid_cloud_on_prem";
    case CloudDeploymentKind::managed_scratchbird_cloud_dedicated:
      return "managed_scratchbird_cloud_dedicated";
    case CloudDeploymentKind::managed_scratchbird_cloud_shared_control_plane:
      return "managed_scratchbird_cloud_shared_control_plane";
    case CloudDeploymentKind::managed_scratchbird_cloud_serverless_edge:
      return "managed_scratchbird_cloud_serverless_edge";
  }
  return "unknown";
}

CloudDeploymentProfile MakeLocalEmulatorCloudDeploymentProfile(
    std::string provider_profile_uuid) {
  CloudDeploymentProfile deployment;
  deployment.deployment_profile_uuid = "00000000-0000-7000-8000-000000000021";
  deployment.deployment_kind = CloudDeploymentKind::self_managed_vm;
  deployment.provider_profile_uuid = std::move(provider_profile_uuid);
  deployment.build_scope = CloudBuildScope::public_single_node;
  deployment.requested_identity_modes = {
      CloudIdentityMode::workload_identity,
      CloudIdentityMode::oidc_federation,
      CloudIdentityMode::service_account,
  };
  deployment.requested_kms_modes = {
      CloudKmsMode::local_key_agent,
  };
  deployment.requested_block_storage_modes = {
      CloudBlockStorageMode::single_attach,
  };
  deployment.requested_object_storage_modes = {
      CloudObjectStorageMode::standard,
      CloudObjectStorageMode::versioned,
  };
  deployment.requested_snapshot_modes = {
      CloudSnapshotMode::application_consistent_snapshot,
  };
  deployment.requested_route_modes = {
      CloudRouteMode::single_node_local,
      CloudRouteMode::tcp_service,
  };
  deployment.requested_observability_modes = {
      CloudObservabilityMode::opentelemetry,
      CloudObservabilityMode::support_bundle,
  };
  deployment.requested_operator_modes = {
      CloudOperatorMode::local_process,
      CloudOperatorMode::dry_run_reconciler,
  };
  deployment.requested_edge_cache_modes = {
      CloudEdgeCacheMode::local_sink,
      CloudEdgeCacheMode::signed_invalidation,
  };
  deployment.requested_billing_metering_modes = {
      CloudBillingMeteringMode::none,
  };
  return deployment;
}

CloudDeploymentProfileValidationResult ValidateCloudDeploymentProfile(
    const CloudProviderCapabilityRegistry& registry,
    const CloudDeploymentProfile& deployment) {
  CloudDeploymentProfileValidationResult result;
  result.ok = true;
  result.operation_id = "cloud.deployment.validate";
  result.side_effects_performed = false;
  result.mga_authority_preserved = true;
  result.provider_state_finality_authority = false;
  result.provider_recovery_authority = false;
  result.parser_finality_authority = false;

  if (deployment.deployment_profile_uuid.empty()) {
    AddDeploymentDiagnostic(
        &result,
        MakeDeploymentDiagnostic(kCloudProviderDiagnosticCapabilityMissing,
                                 "cloud.deployment.profile_uuid_missing",
                                 "deployment_profile_uuid",
                                 "",
                                 deployment.provider_profile_uuid,
                                 "deployment profile UUID is required"));
  }
  if (deployment.provider_profile_uuid.empty()) {
    AddDeploymentDiagnostic(
        &result,
        MakeDeploymentDiagnostic(kCloudProviderDiagnosticProviderNotFound,
                                 "cloud.provider.not_found",
                                 "provider_profile_uuid",
                                 "",
                                 "",
                                 "deployment profile must select a provider profile"));
    return result;
  }

  if (deployment.build_scope == CloudBuildScope::public_single_node) {
    for (const auto& field : deployment.private_cluster_only_fields) {
      AddDeploymentDiagnostic(
          &result,
          MakeDeploymentDiagnostic(kCloudProviderDiagnosticClusterFieldRefused,
                                   "cloud.deployment.cluster_field_refused",
                                   field,
                                   field,
                                   deployment.provider_profile_uuid,
                                   "public build refuses private cluster-only cloud field"));
    }
  }

  if (deployment.deployment_kind ==
          CloudDeploymentKind::managed_scratchbird_cloud_serverless_edge &&
      !deployment.serverless_state_contract_active) {
    AddDeploymentDiagnostic(
        &result,
        MakeDeploymentDiagnostic(kCloudProviderDiagnosticServerlessProfileRefused,
                                 "cloud.deployment.serverless_profile_refused",
                                 "deployment_kind",
                                 CloudDeploymentKindName(deployment.deployment_kind),
                                 deployment.provider_profile_uuid,
                                 "serverless deployment requires an active serverless state and finality contract"));
  }

  const CloudProviderCapabilityProfile* selected =
      FindCloudProviderCapabilityProfile(registry, deployment.provider_profile_uuid);
  if (selected == nullptr) {
    AddDeploymentDiagnostic(
        &result,
        MakeDeploymentDiagnostic(kCloudProviderDiagnosticProviderNotFound,
                                 "cloud.provider.not_found",
                                 "provider_profile_uuid",
                                 deployment.provider_profile_uuid,
                                 deployment.provider_profile_uuid,
                                 "provider profile UUID is not registered"));
    return result;
  }

  result.selected_provider = *selected;
  result.selected_provider_found = true;
  result.provider_state_finality_authority = selected->provider_state_finality_authority;
  result.provider_recovery_authority = selected->provider_recovery_authority;

  const auto provider_validation =
      ValidateCloudProviderCapabilityProfile(*selected, deployment.build_scope);
  if (!provider_validation.ok) {
    CopyProviderValidationDiagnostics(&result, provider_validation);
  }

  ValidateRequestedModes(&result,
                         selected->identity_modes,
                         deployment.requested_identity_modes,
                         deployment,
                         "requested_identity_modes",
                         kCloudProviderDiagnosticCapabilityUnsupported,
                         "cloud.deployment.identity_unsupported",
                         CloudIdentityModeName);
  ValidateRequestedModes(&result,
                         selected->kms_modes,
                         deployment.requested_kms_modes,
                         deployment,
                         "requested_kms_modes",
                         kCloudProviderDiagnosticKmsModeUnsupported,
                         "cloud.deployment.kms_unsupported",
                         CloudKmsModeName);
  ValidateRequestedModes(&result,
                         selected->block_storage_modes,
                         deployment.requested_block_storage_modes,
                         deployment,
                         "requested_block_storage_modes",
                         kCloudProviderDiagnosticCapabilityUnsupported,
                         "cloud.deployment.block_storage_unsupported",
                         CloudBlockStorageModeName);
  ValidateRequestedModes(&result,
                         selected->object_storage_modes,
                         deployment.requested_object_storage_modes,
                         deployment,
                         "requested_object_storage_modes",
                         kCloudProviderDiagnosticCapabilityUnsupported,
                         "cloud.deployment.object_storage_unsupported",
                         CloudObjectStorageModeName);
  ValidateRequestedModes(&result,
                         selected->snapshot_modes,
                         deployment.requested_snapshot_modes,
                         deployment,
                         "requested_snapshot_modes",
                         kCloudProviderDiagnosticCapabilityUnsupported,
                         "cloud.deployment.snapshot_unsupported",
                         CloudSnapshotModeName);
  ValidateRequestedModes(&result,
                         selected->route_modes,
                         deployment.requested_route_modes,
                         deployment,
                         "requested_route_modes",
                         kCloudProviderDiagnosticCapabilityUnsupported,
                         "cloud.deployment.route_unsupported",
                         CloudRouteModeName);
  ValidateRequestedModes(&result,
                         selected->observability_modes,
                         deployment.requested_observability_modes,
                         deployment,
                         "requested_observability_modes",
                         kCloudProviderDiagnosticCapabilityUnsupported,
                         "cloud.deployment.observability_unsupported",
                         CloudObservabilityModeName);
  ValidateRequestedModes(&result,
                         selected->operator_modes,
                         deployment.requested_operator_modes,
                         deployment,
                         "requested_operator_modes",
                         kCloudProviderDiagnosticCapabilityUnsupported,
                         "cloud.deployment.operator_unsupported",
                         CloudOperatorModeName);
  ValidateRequestedModes(&result,
                         selected->edge_cache_modes,
                         deployment.requested_edge_cache_modes,
                         deployment,
                         "requested_edge_cache_modes",
                         kCloudProviderDiagnosticCapabilityUnsupported,
                         "cloud.deployment.edge_cache_unsupported",
                         CloudEdgeCacheModeName);
  ValidateRequestedModes(&result,
                         selected->billing_metering_modes,
                         deployment.requested_billing_metering_modes,
                         deployment,
                         "requested_billing_metering_modes",
                         kCloudProviderDiagnosticCapabilityUnsupported,
                         "cloud.deployment.billing_unsupported",
                         CloudBillingMeteringModeName);

  for (const auto requested : deployment.requested_identity_modes) {
    if (requested == CloudIdentityMode::static_secret_allowed_by_policy &&
        (!deployment.static_secret_policy_authorized ||
         !Contains(selected->identity_modes, CloudIdentityMode::static_secret_allowed_by_policy))) {
      AddDeploymentDiagnostic(
          &result,
          MakeDeploymentDiagnostic(kCloudProviderDiagnosticStaticSecretForbidden,
                                   "cloud.deployment.static_secret_forbidden",
                                   "requested_identity_modes",
                                   CloudIdentityModeName(requested),
                                   deployment.provider_profile_uuid,
                                   "static secret mode requires provider capability and explicit policy authorization"));
    }
  }

  if (deployment.build_scope == CloudBuildScope::public_single_node) {
    for (const auto requested : deployment.requested_route_modes) {
      if (requested == CloudRouteMode::cluster_route_refused) {
        AddDeploymentDiagnostic(
            &result,
            MakeDeploymentDiagnostic(kCloudProviderDiagnosticClusterFieldRefused,
                                     "cloud.deployment.cluster_route_refused",
                                     "requested_route_modes",
                                     CloudRouteModeName(requested),
                                     deployment.provider_profile_uuid,
                                     "public build refuses cluster route fields"));
      }
    }
    for (const auto& field : selected->private_cluster_only_fields) {
      if (IsClusterOnlyFieldName(field) &&
          Contains(deployment.private_cluster_only_fields, field)) {
        AddDeploymentDiagnostic(
            &result,
            MakeDeploymentDiagnostic(kCloudProviderDiagnosticClusterFieldRefused,
                                     "cloud.deployment.provider_cluster_field_refused",
                                     field,
                                     field,
                                     deployment.provider_profile_uuid,
                                     "public build refuses provider private cluster-only field"));
      }
    }
  }

  if (ManagedCloudDeploymentKind(deployment.deployment_kind) &&
      deployment.requested_billing_metering_modes.empty()) {
    AddDeploymentDiagnostic(
        &result,
        MakeDeploymentDiagnostic(kCloudProviderDiagnosticCapabilityMissing,
                                 "cloud.deployment.billing_required",
                                 "requested_billing_metering_modes",
                                 "",
                                 deployment.provider_profile_uuid,
                                 "managed cloud deployment requires billing and metering capability"));
  }

  result.profile_selector_valid = result.ok;
  result.side_effects_performed = false;
  return result;
}

}  // namespace scratchbird::engine::internal_api

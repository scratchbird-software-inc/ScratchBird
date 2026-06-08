// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "security/external_group_api.hpp"

#include "security/auth_provider_model.hpp"
#include "security/security_model.hpp"

namespace scratchbird::engine::internal_api {

// SEARCH_KEY: SB_ENGINE_INTERNAL_API_SECURITY_EXTERNAL_GROUP_API_BEHAVIOR
EngineSyncExternalGroupsResult EngineSyncExternalGroups(const EngineSyncExternalGroupsRequest& request) {
  const std::string provider = CanonicalAuthProviderFamily(
      !request.provider_family.empty() ? request.provider_family : SecurityOptionValue(request, "provider:"));
  if (provider.empty() || !IsKnownAuthProviderFamily(provider)) {
    return SecurityFailure<EngineSyncExternalGroupsResult>(
        request.context,
        "security.sync_external_groups",
        MakeSecurityDiagnostic("SECURITY.AUTHORITY.INVALID", "known_provider_family_required:" + provider));
  }
  if (provider == "cluster_security" && !request.context.cluster_authority_available &&
      !SecurityOptionBool(request, "cluster_authority_available:", false)) {
    auto result = SecurityFailure<EngineSyncExternalGroupsResult>(
        request.context,
        "security.sync_external_groups",
        MakeSecurityDiagnostic("PROCESS.CLUSTER_PATH_ABSENT", "cluster_security_authority_unavailable"));
    result.cluster_authority_required = true;
    return result;
  }
  const auto caps = SecurityProviderCapabilitiesFor(provider);
  if (!caps.supports_group_query && !caps.supports_authz_claims) {
    return SecurityFailure<EngineSyncExternalGroupsResult>(
        request.context,
        "security.sync_external_groups",
        MakeSecurityDiagnostic("SECURITY.GROUP.EXTERNAL_UNSYNCED", "provider_has_no_group_or_claim_capability:" + provider));
  }
  const std::string external_group = SecurityOptionValue(request, "external_group:");
  const std::string internal_group_uuid = SecurityOptionValue(request, "internal_group_uuid:");
  if (external_group.empty() || internal_group_uuid.empty()) {
    return SecurityFailure<EngineSyncExternalGroupsResult>(
        request.context,
        "security.sync_external_groups",
        MakeSecurityDiagnostic("SECURITY.GROUP.EXTERNAL_UNSYNCED", "external_and_internal_group_required"));
  }
  auto result = SecuritySuccess<EngineSyncExternalGroupsResult>(request.context, "security.sync_external_groups");
  result.materialized = true;
  AddSecurityEvidence(&result, "external_group_materialized", internal_group_uuid);
  AddSecurityRow(&result, {{"provider_family", provider},
                           {"external_group", external_group},
                           {"internal_group_uuid", internal_group_uuid},
                           {"ordinary_authz_live_lookup", "false"}});
  return result;
}

EngineExplainMembershipResult EngineExplainMembership(const EngineExplainMembershipRequest& request) {
  const std::string provider = CanonicalAuthProviderFamily(
      !request.provider_family.empty() ? request.provider_family : SecurityOptionValue(request, "provider:"));
  if (provider.empty() || !IsKnownAuthProviderFamily(provider)) {
    return SecurityFailure<EngineExplainMembershipResult>(
        request.context,
        "security.explain_membership",
        MakeSecurityDiagnostic("SECURITY.AUTHORITY.INVALID", "known_provider_family_required:" + provider));
  }
  const auto caps = SecurityProviderCapabilitiesFor(provider);
  if (!caps.supports_membership_path_explain && !SecurityOptionBool(request, "synchronized_graph_evidence:", false)) {
    return SecurityFailure<EngineExplainMembershipResult>(
        request.context,
        "security.explain_membership",
        MakeSecurityDiagnostic("SECURITY.GROUP.EXTERNAL_UNSYNCED", "membership_path_not_explainable:" + provider));
  }
  auto result = SecuritySuccess<EngineExplainMembershipResult>(request.context, "security.explain_membership");
  result.explainable = true;
  AddSecurityEvidence(&result, "membership_path_explain", provider);
  AddSecurityRow(&result, {{"provider_family", provider}, {"explainable", "true"}});
  return result;
}

}  // namespace scratchbird::engine::internal_api

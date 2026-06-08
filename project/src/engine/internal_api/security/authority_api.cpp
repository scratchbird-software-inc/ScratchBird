// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "security/authority_api.hpp"

#include "api_diagnostics.hpp"

namespace scratchbird::engine::internal_api {

// SEARCH_KEY: SB_ENGINE_INTERNAL_API_SECURITY_AUTHORITY_API_BEHAVIOR
EngineResolveSecurityAuthorityResult EngineResolveSecurityAuthority(
    const EngineResolveSecurityAuthorityRequest& request) {
  SecurityAuthorityDescriptor descriptor = request.candidate;
  if (descriptor.authority_class.empty()) { descriptor = SecurityAuthorityDescriptorFromRequest(request); }
  if (descriptor.authority_uuid.canonical.empty()) {
    descriptor.authority_uuid.canonical = request.context.database_uuid.canonical.empty()
        ? "00000000-0000-7000-8000-0000000sec01"
        : request.context.database_uuid.canonical;
  }
  if (!IsSupportedSecurityAuthorityClass(descriptor.authority_class)) {
    return SecurityFailure<EngineResolveSecurityAuthorityResult>(
        request.context,
        "security.resolve_authority",
        MakeSecurityDiagnostic("SECURITY.AUTHORITY.INVALID", "unsupported_authority_class:" + descriptor.authority_class));
  }
  if (IsClusterSecurityAuthorityClass(descriptor.authority_class) && !request.context.cluster_authority_available) {
    auto result = SecurityFailure<EngineResolveSecurityAuthorityResult>(
        request.context,
        "security.resolve_authority",
        MakeSecurityDiagnostic("SECURITY.CLUSTER.AUTHORITY_REQUIRED", "cluster_security_authority_unavailable"));
    result.cluster_authority_required = true;
    return result;
  }
  if (request.require_admitted && request.context.database_path.empty() &&
      descriptor.authority_class != "internal_default_bootstrap") {
    return SecurityFailure<EngineResolveSecurityAuthorityResult>(
        request.context,
        "security.resolve_authority",
        MakeSecurityDiagnostic("SECURITY.AUTHORITY.UNAVAILABLE", "database_path_required"));
  }

  auto result = SecuritySuccess<EngineResolveSecurityAuthorityResult>(request.context, "security.resolve_authority");
  result.descriptor = descriptor;
  result.admitted = true;
  result.primary_object.uuid = descriptor.authority_uuid;
  result.primary_object.object_kind = "security_authority";
  AddSecurityEvidence(&result, "security_authority", descriptor.authority_class);
  AddSecurityRow(&result, {{"authority_uuid", descriptor.authority_uuid.canonical},
                           {"authority_class", descriptor.authority_class},
                           {"policy_epoch", std::to_string(descriptor.policy_epoch)},
                           {"offline_behavior", descriptor.offline_behavior},
                           {"admitted", "true"}});
  return result;
}

}  // namespace scratchbird::engine::internal_api

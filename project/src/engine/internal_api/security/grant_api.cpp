// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "security/grant_api.hpp"

#include "behavior_support/api_behavior_store.hpp"
#include "security/authorization_api.hpp"
#include "security/security_model.hpp"

#include <string>

namespace scratchbird::engine::internal_api {
namespace {

bool HasOptionContaining(const EngineGrantRightRequest& request, const std::string& needle) {
  for (const auto& option : request.option_envelopes) {
    if (option.find(needle) != std::string::npos) { return true; }
  }
  return false;
}

template <typename TRequest, typename TResult>
bool RequireGrantAdmin(const TRequest& request, TResult* result) {
  EngineAuthorizeRequest authorize;
  authorize.context = request.context;
  authorize.target_object = request.target_object;
  authorize.required_right = "SEC_GRANT_ADMIN";
  const auto authorized = EngineAuthorize(authorize);
  if (authorized.ok && authorized.authorized) { return true; }
  *result = SecurityFailure<TResult>(
      request.context,
      request.operation_id.empty() ? "security.grant_admin" : request.operation_id,
      authorized.diagnostics.empty()
          ? MakeSecurityDiagnostic("SECURITY.AUTHORIZATION.DENIED", "SEC_GRANT_ADMIN")
          : authorized.diagnostics.front());
  return false;
}

}  // namespace

// SEARCH_KEY: SB_ENGINE_INTERNAL_API_SECURITY_GRANT_API_BEHAVIOR
EngineGrantRightResult EngineGrantRight(const EngineGrantRightRequest& request) {
  if (!request.context.security_context_present) {
    return SecurityFailure<EngineGrantRightResult>(
        request.context,
        "security.grant_right",
        MakeSecurityDiagnostic("SECURITY.AUTHENTICATION.REQUEST_INVALID", "security_context_required"));
  }
  EngineGrantRightResult authorization_failure;
  if (!RequireGrantAdmin(request, &authorization_failure)) { return authorization_failure; }
  const std::string right = SecurityOptionValue(request, "right:");
  if (!right.empty() && !IsKnownSecurityRight(right)) {
    return SecurityFailure<EngineGrantRightResult>(
        request.context,
        "security.grant_right",
        MakeSecurityDiagnostic("SECURITY.AUTHORIZATION.DENIED", "unknown_right:" + right));
  }
  auto result = PersistedRecordResult<EngineGrantRightResult>(request, "security.grant_right", "grant", true, "granted");
  if (HasOptionContaining(request, "OBS_INDEX_PROFILE_READ") && HasOptionContaining(request, "DEV")) {
    result.diagnostics.push_back(MakeEngineApiDiagnostic("SB_ENGINE_API_DEV_ONLY_RIGHT_WARNING",
                                                         "engine.api.dev_only_right_warning",
                                                         "OBS_INDEX_PROFILE_READ:DEV",
                                                         false));
  }
  if (HasOptionContaining(request, "DOMAIN_UNMASK") && HasOptionContaining(request, "DEV")) {
    result.diagnostics.push_back(MakeEngineApiDiagnostic("SB_ENGINE_API_DEV_ONLY_RIGHT_WARNING",
                                                         "engine.api.dev_only_right_warning",
                                                         "DOMAIN_UNMASK:DEV",
                                                         false));
  }
  if (result.ok) {
    AddSecurityEvidence(&result, "security_evidence_before_success", "grant_right");
    AddSecurityEvidence(&result, "security_grant_admin", request.context.principal_uuid.canonical);
  }
  return result;
}

EngineRevokeRightResult EngineRevokeRight(const EngineRevokeRightRequest& request) {
  if (!request.context.security_context_present) {
    return SecurityFailure<EngineRevokeRightResult>(
        request.context,
        "security.revoke_right",
        MakeSecurityDiagnostic("SECURITY.AUTHENTICATION.REQUEST_INVALID", "security_context_required"));
  }
  EngineRevokeRightResult authorization_failure;
  if (!RequireGrantAdmin(request, &authorization_failure)) { return authorization_failure; }
  auto result = PersistedRecordResult<EngineRevokeRightResult>(request, "security.revoke_right", "grant", true, "revoked", true);
  if (result.ok) {
    AddSecurityEvidence(&result, "security_evidence_before_success", "revoke_right");
    AddSecurityEvidence(&result, "security_grant_admin", request.context.principal_uuid.canonical);
  }
  return result;
}

}  // namespace scratchbird::engine::internal_api

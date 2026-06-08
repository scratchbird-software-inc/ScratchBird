// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "security/auth_provider_plugin_api.hpp"

#include "behavior_support/api_behavior_store.hpp"

namespace scratchbird::engine::internal_api {

// SEARCH_KEY: SB_ENGINE_INTERNAL_API_AUTH_PROVIDER_PLUGIN_API_BEHAVIOR
EngineRegisterAuthProviderResult EngineRegisterAuthProvider(const EngineRegisterAuthProviderRequest& request) {
  auto decision = AdmitAuthProvider(request);
  if (!decision.ok) {
    auto result = SecurityFailure<EngineRegisterAuthProviderResult>(request.context, "security.register_auth_provider", decision.diagnostic);
    ApplyAuthProviderDecision(&result, decision);
    return result;
  }
  auto persisted = PersistedRecordResult<EngineRegisterAuthProviderResult>(request, "security.register_auth_provider", "security_auth_provider", true, "admitted");
  if (!persisted.ok) { return persisted; }
  persisted.provider = AuthProviderDescriptorFromRequest(request);
  persisted.admitted = true;
  AddSecurityEvidence(&persisted, "auth_provider_admitted", persisted.provider.provider_uuid.canonical);
  AddSecurityRow(&persisted, {{"provider_uuid", persisted.provider.provider_uuid.canonical},
                              {"provider_family", persisted.provider.provider_family},
                              {"trust_state", persisted.provider.trust_state},
                              {"rollout_state", persisted.provider.rollout_state}});
  return persisted;
}

EngineInspectAuthProviderResult EngineInspectAuthProvider(const EngineInspectAuthProviderRequest& request) {
  if (!SecurityContextHasRight(request.context, "AUTH_PROVIDER_ADMIN") &&
      !SecurityContextHasRight(request.context, "OBS_CONFIG_INSPECT")) {
    return SecurityFailure<EngineInspectAuthProviderResult>(request.context,
                                                            "security.inspect_auth_provider",
                                                            MakeSecurityDiagnostic("SECURITY.AUTHORIZATION.DENIED", "OBS_CONFIG_INSPECT"));
  }
  auto result = SecuritySuccess<EngineInspectAuthProviderResult>(request.context, "security.inspect_auth_provider");
  result.provider = AuthProviderDescriptorFromRequest(request);
  result.visible = true;
  AddSecurityEvidence(&result, "auth_provider_inspect", result.provider.provider_family);
  AddSecurityRow(&result, {{"provider_uuid", result.provider.provider_uuid.canonical},
                           {"provider_family", result.provider.provider_family},
                           {"authn", result.provider.capabilities.supports_authn ? "true" : "false"},
                           {"group_query", result.provider.capabilities.supports_group_query ? "true" : "false"}});
  return result;
}

EngineDisableAuthProviderResult EngineDisableAuthProvider(const EngineDisableAuthProviderRequest& request) {
  if (!SecurityContextHasRight(request.context, "AUTH_PROVIDER_ADMIN")) {
    return SecurityFailure<EngineDisableAuthProviderResult>(request.context,
                                                            "security.disable_auth_provider",
                                                            MakeSecurityDiagnostic("SECURITY.AUTHORIZATION.DENIED", "AUTH_PROVIDER_ADMIN"));
  }
  auto result = PersistedRecordResult<EngineDisableAuthProviderResult>(request, "security.disable_auth_provider", "security_auth_provider", true, "disabled", true);
  if (result.ok) {
    result.disabled = true;
    AddSecurityEvidence(&result, "auth_provider_disabled", result.primary_object.uuid.canonical);
    AddSecurityRow(&result, {{"disabled", "true"}});
  }
  return result;
}

EngineAuthenticateProviderResult EngineAuthenticateProvider(const EngineAuthenticateProviderRequest& request) {
  auto decision = AuthenticateWithProvider(request);
  if (!decision.ok) {
    auto result = SecurityFailure<EngineAuthenticateProviderResult>(request.context, "security.authenticate_provider", decision.diagnostic);
    ApplyAuthProviderDecision(&result, decision);
    return result;
  }
  EngineApiRequest context_request = request;
  context_request.option_envelopes.push_back("external_evidence:provider=" + decision.provider_family);
  auto result = SecuritySuccess<EngineAuthenticateProviderResult>(request.context, "security.authenticate_provider");
  result.authenticated = true;
  result.connection_security_context = ConnectionSecurityContextFromRequest(context_request);
  if (result.connection_security_context.effective_user_uuid.canonical.empty()) {
    result.connection_security_context.effective_user_uuid.canonical = GenerateCrudEngineUuid("principal");
  }
  if (result.connection_security_context.connection_uuid.canonical.empty()) {
    result.connection_security_context.connection_uuid.canonical = GenerateCrudEngineUuid("session");
  }
  result.primary_object.uuid = result.connection_security_context.effective_user_uuid;
  result.primary_object.object_kind = "principal";
  ApplyAuthProviderDecision(&result, decision);
  result.ok = true;
  AddSecurityEvidence(&result, "connection_security_context", result.connection_security_context.connection_uuid.canonical);
  return result;
}

}  // namespace scratchbird::engine::internal_api

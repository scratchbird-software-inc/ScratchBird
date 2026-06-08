// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "security/auth_provider_policy_api.hpp"

#include "behavior_support/api_behavior_store.hpp"

namespace scratchbird::engine::internal_api {

// SEARCH_KEY: SB_ENGINE_INTERNAL_API_AUTH_PROVIDER_POLICY_API_BEHAVIOR
EngineReloadAuthProviderPolicyResult EngineReloadAuthProviderPolicy(const EngineReloadAuthProviderPolicyRequest& request) {
  auto decision = EvaluateAuthProviderPolicy(request);
  if (!decision.ok) {
    auto result = SecurityFailure<EngineReloadAuthProviderPolicyResult>(request.context, "security.reload_auth_provider_policy", decision.diagnostic);
    ApplyAuthProviderDecision(&result, decision);
    return result;
  }
  auto result = PersistedRecordResult<EngineReloadAuthProviderPolicyResult>(request, "security.reload_auth_provider_policy", "security_auth_provider_policy", true, "active");
  if (!result.ok) { return result; }
  result.policy = AuthProviderPolicyFromRequest(request);
  result.reloaded = true;
  AddSecurityEvidence(&result, "auth_provider_policy_reloaded", result.policy.policy_uuid.canonical);
  AddSecurityRow(&result, {{"policy_uuid", result.policy.policy_uuid.canonical},
                           {"provider_family", result.policy.provider_family},
                           {"stale_behavior", result.policy.stale_behavior},
                           {"cache_bounds", result.policy.cache_bounds}});
  return result;
}

}  // namespace scratchbird::engine::internal_api

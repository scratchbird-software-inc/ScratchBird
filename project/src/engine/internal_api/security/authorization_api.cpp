// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "security/authorization_api.hpp"

#include "security/security_model.hpp"

#include <utility>

namespace scratchbird::engine::internal_api {

// SEARCH_KEY: SB_ENGINE_INTERNAL_API_SECURITY_AUTHORIZATION_API_BEHAVIOR
EngineAuthorizeResult EngineAuthorize(const EngineAuthorizeRequest& request) {
  const std::string right = !request.required_right.empty()
      ? request.required_right
      : SecurityOptionValue(request, "right:");
  if (right.empty() || !IsKnownSecurityRight(right)) {
    return SecurityFailure<EngineAuthorizeResult>(
        request.context,
        "security.authorize",
        MakeSecurityDiagnostic("SECURITY.AUTHORIZATION.DENIED", "unknown_or_missing_right:" + right));
  }
  const bool cluster_required = request.require_cluster_authority ||
                                SecurityOptionBool(request, "requires_cluster_authority:", false) ||
                                right.rfind("OBS_CLUSTER_", 0) == 0;
  if (cluster_required && !request.context.cluster_authority_available) {
    auto result = SecurityFailure<EngineAuthorizeResult>(
        request.context,
        "security.authorize",
        MakeSecurityDiagnostic("SECURITY.CLUSTER.AUTHORITY_REQUIRED", right));
    result.cluster_authority_required = true;
    return result;
  }
  auto decision = EvaluateMaterializedAuthorization(request.context,
                                                   request.context.authorization_context,
                                                   right,
                                                   request.target_object.uuid.canonical);
  if (!decision.authorized) {
    auto result = SecurityFailure<EngineAuthorizeResult>(
        request.context,
        "security.authorize",
        decision.diagnostics.empty()
            ? MakeSecurityDiagnostic("SECURITY.AUTHORIZATION.DENIED", right)
            : decision.diagnostics.front());
    result.decision = decision.decision;
    result.policy_recheck_required = decision.policy_recheck_required;
    result.policy_recheck_reasons = std::move(decision.policy_recheck_reasons);
    AddSecurityRow(&result, {{"decision", result.decision},
                             {"right", right},
                             {"target_uuid", request.target_object.uuid.canonical},
                             {"authority", "materialized_authorization_context"}});
    return result;
  }
  auto result = SecuritySuccess<EngineAuthorizeResult>(request.context, "security.authorize");
  result.authorized = true;
  result.policy_recheck_required = decision.policy_recheck_required;
  result.policy_recheck_reasons = std::move(decision.policy_recheck_reasons);
  result.decision = decision.decision;
  AddSecurityEvidence(&result, "authorization_decision", "allow:" + right);
  AddSecurityEvidence(&result, "authorization_authority", "materialized_authorization_context");
  if (result.policy_recheck_required) {
    AddSecurityEvidence(&result, "authorization_policy_recheck", right);
  }
  AddSecurityRow(&result, {{"decision", result.decision},
                           {"right", right},
                           {"target_uuid", request.target_object.uuid.canonical},
                           {"policy_recheck_required", result.policy_recheck_required ? "true" : "false"},
                           {"authority", "materialized_authorization_context"}});
  return result;
}

}  // namespace scratchbird::engine::internal_api

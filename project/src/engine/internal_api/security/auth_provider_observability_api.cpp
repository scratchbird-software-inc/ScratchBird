// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "security/auth_provider_observability_api.hpp"

#include "security/auth_provider_model.hpp"

namespace scratchbird::engine::internal_api {

// SEARCH_KEY: SB_ENGINE_INTERNAL_API_AUTH_PROVIDER_OBSERVABILITY_API_BEHAVIOR
EngineInspectAuthProviderMetricsResult EngineInspectAuthProviderMetrics(const EngineInspectAuthProviderMetricsRequest& request) {
  auto decision = InspectAuthProviderMetrics(request);
  if (!decision.ok) {
    auto result = SecurityFailure<EngineInspectAuthProviderMetricsResult>(request.context, "security.inspect_auth_provider_metrics", decision.diagnostic);
    ApplyAuthProviderDecision(&result, decision);
    return result;
  }
  auto result = SecuritySuccess<EngineInspectAuthProviderMetricsResult>(request.context, "security.inspect_auth_provider_metrics");
  result.metrics_available = true;
  ApplyAuthProviderDecision(&result, decision);
  result.ok = true;
  return result;
}

}  // namespace scratchbird::engine::internal_api

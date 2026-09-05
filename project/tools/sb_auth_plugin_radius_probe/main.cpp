// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "../auth_provider_probe_common/probe_common.hpp"

int main() {
  using namespace sb_auth_probe;
  auto authentication = Request<EngineAuthenticateProviderRequest>("radius");
  authentication.option_envelopes.push_back("radius_policy_enabled:true");
  authentication.option_envelopes.push_back(
      "dependency:radius_client:available");
  const auto authentication_r = EngineAuthenticateProvider(authentication);
  auto explain = Request<EngineExplainMembershipRequest>("radius");
  const auto explain_r = EngineExplainMembership(explain);
  return Finish({
      {"radius_unavailable",
       HasDiagnostic(authentication_r, "SECURITY.AUTH_SOURCE_UNAVAILABLE",
                     "radius_policy_and_dependency_required")},
      {"radius_no_path",
       HasDiagnostic(explain_r, "SECURITY.GROUP.EXTERNAL_UNSYNCED",
                     "membership_path_not_explainable:radius")},
  });
}

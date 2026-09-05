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
  auto unavailable = Request<EngineAuthenticateProviderRequest>("oidc_jwt");
  unavailable.option_envelopes.push_back(
      "dependency:oidc_jwt_client:available");
  const auto unavailable_r = EngineAuthenticateProvider(unavailable);

  auto overage = unavailable;
  RemoveOption(&overage, "groups:materialized");
  overage.option_envelopes.push_back("groups_overage:true");
  EngineAuthenticateProviderResult overage_r;
  {
    ScopedTrustedProviderResult trusted_result;
    overage_r = EngineAuthenticateProvider(overage);
  }

  auto validator =
      Request<EngineAuthenticateProviderRequest>("oauth_validator");
  const auto validator_r = EngineAuthenticateProvider(validator);
  return Finish({
      {"oidc_unavailable",
       HasDiagnostic(unavailable_r, "SECURITY.AUTH_SOURCE_UNAVAILABLE",
                     "oidc_jwt_validator_required")},
      {"overage_requires_sync",
       HasDiagnostic(overage_r, "SECURITY.GROUP.EXTERNAL_UNSYNCED",
                     "internal_group_materialization_required")},
      {"oauth_not_login",
       HasDiagnostic(validator_r, "SECURITY.AUTH_PROVIDER_UNSUPPORTED",
                     "oauth_validator_is_validator_only_not_login")},
  });
}

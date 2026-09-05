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
  auto authentication =
      Request<EngineAuthenticateProviderRequest>("token_api_key");
  const auto authentication_r = EngineAuthenticateProvider(authentication);

  auto revoke = Request<EngineRevokeTokenRequest>("token_api_key");
  const auto revoke_r = EngineRevokeToken(revoke);

  auto unsynchronized =
      Request<EngineAuthenticateProviderRequest>("token_api_key");
  RemoveOption(&unsynchronized, "groups:materialized");
  EngineAuthenticateProviderResult unsynchronized_r;
  {
    ScopedTrustedProviderResult trusted_result;
    unsynchronized_r = EngineAuthenticateProvider(unsynchronized);
  }

  return Finish({
      {"token_unavailable",
       HasDiagnostic(authentication_r, "SECURITY.CREDENTIAL_INVALID",
                     "token_api_key_evidence_required")},
      {"revoked", revoke_r.ok && revoke_r.revoked},
      {"unsynced_rejected",
       HasDiagnostic(unsynchronized_r,
                     "SECURITY.GROUP.EXTERNAL_UNSYNCED",
                     "internal_group_materialization_required")},
  });
}

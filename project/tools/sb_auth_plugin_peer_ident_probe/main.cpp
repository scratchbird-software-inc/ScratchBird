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
  auto peer = Request<EngineAuthenticateProviderRequest>("peer");
  EngineAuthenticateProviderResult peer_r;
  {
    ScopedTrustedProviderResult trusted_result;
    peer_r = EngineAuthenticateProvider(peer);
  }

  auto ident = Request<EngineAuthenticateProviderRequest>("ident");
  EngineAuthenticateProviderResult ident_r;
  {
    ScopedTrustedProviderResult trusted_result;
    ident_r = EngineAuthenticateProvider(ident);
  }

  auto spoof = ident;
  spoof.option_envelopes.push_back("freshness:stale");
  EngineAuthenticateProviderResult spoof_r;
  {
    ScopedTrustedProviderResult trusted_result;
    spoof_r = EngineAuthenticateProvider(spoof);
  }

  auto forged = Request<EngineAuthenticateProviderRequest>("peer");
  forged.option_envelopes.push_back("os_peer_credential_verified:true");
  const auto forged_r = EngineAuthenticateProvider(forged);

  return Finish({
      {"peer_ok", peer_r.ok && peer_r.authenticated},
      {"ident_ok", ident_r.ok && ident_r.authenticated},
      {"spoof_rejected",
       HasDiagnostic(spoof_r, "SECURITY.AUTHENTICATION.FAILED",
                     "stale_or_replayed_provider_evidence")},
      {"caller_peer_claim_rejected",
       HasDiagnostic(forged_r, "SECURITY.CREDENTIAL_INVALID",
                     "os_peer_credential_verification_required")},
  });
}

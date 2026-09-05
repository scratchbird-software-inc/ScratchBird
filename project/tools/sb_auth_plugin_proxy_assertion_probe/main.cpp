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
  auto unavailable =
      Request<EngineAuthenticateProviderRequest>("proxy_assertion");
  unavailable.option_envelopes.push_back(
      "proxy_assertion_policy_enabled:true");
  unavailable.option_envelopes.push_back(
      "dependency:proxy_assertion_verifier:available");
  unavailable.option_envelopes.push_back("channel_binding:verified");
  const auto unavailable_r = EngineAuthenticateProvider(unavailable);

  auto replay = unavailable;
  replay.option_envelopes.push_back("replayed:true");
  EngineAuthenticateProviderResult replay_r;
  {
    ScopedTrustedProviderResult trusted_result;
    replay_r = EngineAuthenticateProvider(replay);
  }
  return Finish({
      {"proxy_unavailable",
       HasDiagnostic(unavailable_r, "SECURITY.CHANNEL_BINDING_REQUIRED",
                     "proxy_assertion_policy_and_channel_binding_required")},
      {"replay_rejected",
       HasDiagnostic(replay_r, "SECURITY.AUTHENTICATION.FAILED",
                     "stale_or_replayed_provider_evidence")},
  });
}

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
  auto unavailable = Request<EngineAuthenticateProviderRequest>("saml");
  unavailable.option_envelopes.push_back("dependency:saml_xmlsig:available");
  const auto unavailable_r = EngineAuthenticateProvider(unavailable);
  auto stale = unavailable;
  stale.option_envelopes.push_back("freshness:stale");
  EngineAuthenticateProviderResult stale_r;
  {
    ScopedTrustedProviderResult trusted_result;
    stale_r = EngineAuthenticateProvider(stale);
  }
  return Finish({
      {"saml_unavailable",
       HasDiagnostic(unavailable_r, "SECURITY.AUTH_SOURCE_UNAVAILABLE",
                     "saml_validator_required")},
      {"stale_assertion_rejected",
       HasDiagnostic(stale_r, "SECURITY.AUTHENTICATION.FAILED",
                     "stale_or_replayed_provider_evidence")},
  });
}

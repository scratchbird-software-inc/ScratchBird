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
  auto signature = Request<EngineRegisterAuthProviderRequest>("oidc_jwt");
  signature.option_envelopes.push_back(
      "dependency:oidc_jwt_client:available");
  signature.option_envelopes.push_back("signature_valid:false");
  const auto signature_r = EngineRegisterAuthProvider(signature);

  auto dependency = Request<EngineRegisterAuthProviderRequest>("ldap_ad");
  dependency.option_envelopes.push_back("missing_dependency:true");
  const auto dependency_r = EngineRegisterAuthProvider(dependency);

  auto abi = Request<EngineRegisterAuthProviderRequest>("saml");
  abi.option_envelopes.push_back("dependency:saml_xmlsig:available");
  abi.option_envelopes.push_back("abi_supported:false");
  const auto abi_r = EngineRegisterAuthProvider(abi);

  auto stale = Request<EngineRegisterAuthProviderRequest>("radius");
  stale.option_envelopes.push_back("stale_implementation:true");
  const auto stale_r = EngineRegisterAuthProvider(stale);

  return Finish({
      {"signature_rejected",
       HasDiagnostic(signature_r, "SECURITY.UDR.TRUST_DENIED",
                     "signature_or_provenance_failed")},
      {"dependency_rejected",
       HasDiagnostic(dependency_r, "SECURITY.AUTH_SOURCE_UNAVAILABLE",
                     "provider_dependency_missing:ldap_client")},
      {"abi_rejected",
       HasDiagnostic(abi_r, "SECURITY.AUTHORITY.INVALID",
                     "unsupported_provider_abi")},
      {"stale_rejected",
       HasDiagnostic(stale_r, "SECURITY.AUTHORITY.INVALID",
                     "stale_provider_implementation")},
  });
}

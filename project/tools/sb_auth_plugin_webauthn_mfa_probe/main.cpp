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
  auto unavailable = Request<EngineAuthenticateProviderRequest>("webauthn");
  unavailable.option_envelopes.push_back("webauthn_policy_enabled:true");
  unavailable.option_envelopes.push_back(
      "dependency:webauthn_fido2:available");
  unavailable.option_envelopes.push_back("mfa:present");
  const auto unavailable_r = EngineAuthenticateProvider(unavailable);

  auto missing_mfa = unavailable;
  RemoveOption(&missing_mfa, "mfa:present");
  EngineAuthenticateProviderResult missing_mfa_r;
  {
    ScopedTrustedProviderResult trusted_result;
    missing_mfa_r = EngineAuthenticateProvider(missing_mfa);
  }

  auto factor = Request<EngineAuthenticateProviderRequest>("factor_chain");
  factor.option_envelopes.push_back("factor_chain_policy_enabled:true");
  factor.option_envelopes.push_back(
      "dependency:webauthn_fido2:available");
  const auto factor_r = EngineAuthenticateProvider(factor);
  return Finish({
      {"webauthn_unavailable",
       HasDiagnostic(unavailable_r, "SECURITY.MFA_REQUIRED",
                     "webauthn_policy_and_mfa_required")},
      {"missing_mfa_rejected",
       HasDiagnostic(missing_mfa_r, "SECURITY.MFA_REQUIRED",
                     "mfa_evidence_required")},
      {"factor_unavailable",
       HasDiagnostic(factor_r, "SECURITY.MFA_REQUIRED",
                     "factor_chain_policy_required")},
  });
}

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
  auto scram = Request<EngineAuthenticateProviderRequest>("scram_sha256");
  scram.option_envelopes.push_back("channel_binding:verified");
  const auto scram_r = EngineAuthenticateProvider(scram);

  auto compat = Request<EngineAuthenticateProviderRequest>("password_compat");
  const auto compat_r = EngineAuthenticateProvider(compat);

  auto compat_allowed = compat;
  compat_allowed.option_envelopes.push_back("allow_password_compat:true");
  compat_allowed.option_envelopes.push_back("channel_binding:verified");
  const auto compat_allowed_r = EngineAuthenticateProvider(compat_allowed);

  auto downgrade = Request<EngineAuthenticateProviderRequest>("scram_sha512");
  downgrade.option_envelopes.push_back(
      "scram_sha512_verifier_storage:available");
  downgrade.option_envelopes.push_back("scram_sha512_tests:present");
  downgrade.option_envelopes.push_back("channel_binding:verified");
  downgrade.option_envelopes.push_back("downgrade_attempt:true");
  const auto downgrade_r = EngineAuthenticateProvider(downgrade);

  return Finish({
      {"scram_ok", scram_r.ok && scram_r.authenticated},
      {"compat_default_rejected",
       HasDiagnostic(compat_r, "SECURITY.AUTH_PROVIDER_UNSUPPORTED",
                     "password_compat_policy_required")},
      {"compat_allowed",
       compat_allowed_r.ok && compat_allowed_r.authenticated},
      {"downgrade_rejected",
       HasDiagnostic(downgrade_r, "SECURITY.AUTHENTICATION.FAILED",
                     "scram_downgrade_denied")},
  });
}

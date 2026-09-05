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
  auto good = Request<EngineRegisterAuthProviderRequest>("ldap_ad");
  good.option_envelopes.push_back("dependency:ldap_client:available");
  const auto good_r = EngineRegisterAuthProvider(good);

  auto duplicate = good;
  duplicate.option_envelopes.push_back("duplicate_provider_uuid:true");
  const auto duplicate_r = EngineRegisterAuthProvider(duplicate);

  auto unknown = Request<EngineRegisterAuthProviderRequest>("unknown");
  const auto unknown_r = EngineRegisterAuthProvider(unknown);

  auto disabled = good;
  disabled.option_envelopes.push_back("provider:disabled");
  const auto disabled_r = EngineRegisterAuthProvider(disabled);

  return Finish({
      {"registered", good_r.ok && good_r.admitted},
      {"duplicate_rejected",
       HasDiagnostic(duplicate_r, "SECURITY.AUTHORITY.INVALID",
                     "duplicate_provider_uuid")},
      {"unknown_rejected",
       HasDiagnostic(unknown_r, "SECURITY.AUTHORITY.INVALID",
                     "unknown_provider_family:unknown")},
      {"disabled_rejected",
       HasDiagnostic(disabled_r, "SECURITY.UDR.TRUST_DENIED",
                     "provider_disabled_or_trust_reject")},
  });
}

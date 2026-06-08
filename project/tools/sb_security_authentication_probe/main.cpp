// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "security/authentication_api.hpp"

#include <iostream>

using namespace scratchbird::engine::internal_api;

namespace {
EngineRequestContext Context() {
  EngineRequestContext context;
  context.database_uuid.canonical = "018f0000-0000-7000-8000-00000000b001";
  context.session_uuid.canonical = "018f0000-0000-7000-8000-00000000b002";
  return context;
}
}

int main() {
  EngineAuthenticateRequest ok_request;
  ok_request.context = Context();
  ok_request.provider_family = "ldap_ad";
  ok_request.principal_claim = "alice";
  ok_request.credential_evidence_present = true;
  const auto authenticated = EngineAuthenticate(ok_request);

  EngineAuthenticateRequest bad_provider;
  bad_provider.context = Context();
  bad_provider.provider_family = "unknown";
  bad_provider.principal_claim = "alice";
  bad_provider.credential_evidence_present = true;
  const auto provider_rejected = EngineAuthenticate(bad_provider);

  EngineAuthenticateRequest missing_mfa;
  missing_mfa.context = Context();
  missing_mfa.provider_family = "local_password";
  missing_mfa.principal_claim = "bob";
  missing_mfa.credential_evidence_present = true;
  missing_mfa.option_envelopes.push_back("mfa_required:true");
  const auto mfa_rejected = EngineAuthenticate(missing_mfa);

  const bool ok = authenticated.ok && authenticated.authenticated && !provider_rejected.ok && !mfa_rejected.ok;
  std::cout << "{\"ok\":" << (ok ? "true" : "false")
            << ",\"authenticated\":" << (authenticated.authenticated ? "true" : "false")
            << ",\"provider_rejected\":" << (!provider_rejected.ok ? "true" : "false")
            << ",\"mfa_rejected\":" << (!mfa_rejected.ok ? "true" : "false") << "}\n";
  return ok ? 0 : 1;
}

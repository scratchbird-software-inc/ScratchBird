// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "security/external_group_api.hpp"

#include <iostream>

using namespace scratchbird::engine::internal_api;

int main() {
  EngineRequestContext context;
  context.security_context_present = true;

  EngineSyncExternalGroupsRequest sync;
  sync.context = context;
  sync.provider_family = "ldap_ad";
  sync.option_envelopes.push_back("external_group:CN=DBA,DC=example,DC=org");
  sync.option_envelopes.push_back("internal_group_uuid:018f0000-0000-7000-8000-00000000d001");
  const auto sync_result = EngineSyncExternalGroups(sync);

  EngineExplainMembershipRequest explain;
  explain.context = context;
  explain.provider_family = "ldap_ad";
  const auto explain_result = EngineExplainMembership(explain);

  EngineExplainMembershipRequest token_explain;
  token_explain.context = context;
  token_explain.provider_family = "oidc_jwt";
  const auto token_result = EngineExplainMembership(token_explain);

  const bool ok = sync_result.ok && sync_result.materialized && explain_result.ok && explain_result.explainable && !token_result.ok;
  std::cout << "{\"ok\":" << (ok ? "true" : "false")
            << ",\"sync\":" << (sync_result.materialized ? "true" : "false")
            << ",\"ldap_explain\":" << (explain_result.explainable ? "true" : "false")
            << ",\"token_explain_rejected\":" << (!token_result.ok ? "true" : "false") << "}\n";
  return ok ? 0 : 1;
}

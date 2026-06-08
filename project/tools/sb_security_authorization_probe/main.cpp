// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "security/authorization_api.hpp"
#include "security/grant_api.hpp"
#include "security/visibility_api.hpp"

#include <iostream>

using namespace scratchbird::engine::internal_api;

namespace {
EngineRequestContext Context(std::initializer_list<const char*> tags) {
  EngineRequestContext context;
  context.security_context_present = true;
  context.database_path = "/tmp/sb_security_authorization_probe.sbdb";
  context.local_transaction_id = 1;
  for (const char* tag : tags) { context.trace_tags.emplace_back(tag); }
  return context;
}
}

int main() {
  EngineAuthorizeRequest allow;
  allow.context = Context({"group:DBA"});
  allow.required_right = "SELECT";
  const auto allow_result = EngineAuthorize(allow);

  EngineAuthorizeRequest deny;
  deny.context = Context({"group:APP"});
  deny.required_right = "OBS_RUNTIME_ALL";
  const auto deny_result = EngineAuthorize(deny);

  EngineGrantRightRequest grant;
  grant.context = Context({"right:SEC_GRANT_ADMIN"});
  grant.option_envelopes.push_back("right:OBS_INDEX_PROFILE_READ");
  grant.option_envelopes.push_back("grantee:DEV");
  const auto grant_result = EngineGrantRight(grant);

  EngineEvaluateVisibilityRequest visibility;
  visibility.context = Context({"right:VISIBLE"});
  visibility.target_object.uuid.canonical = "018f0000-0000-7000-8000-00000000c001";
  const auto visibility_result = EngineEvaluateVisibility(visibility);

  const bool ok = allow_result.ok && allow_result.authorized && !deny_result.ok && grant_result.ok && visibility_result.ok;
  std::cout << "{\"ok\":" << (ok ? "true" : "false")
            << ",\"allow\":" << (allow_result.authorized ? "true" : "false")
            << ",\"deny\":" << (!deny_result.ok ? "true" : "false")
            << ",\"grant\":" << (grant_result.ok ? "true" : "false")
            << ",\"visibility\":" << (visibility_result.ok ? "true" : "false") << "}\n";
  return ok ? 0 : 1;
}

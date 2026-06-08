// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "sblr_dispatch.hpp"

#include <iostream>

namespace api = scratchbird::engine::internal_api;
namespace sblr = scratchbird::engine::sblr;

namespace {
api::EngineRequestContext Context() {
  api::EngineRequestContext context;
  context.security_context_present = true;
  context.database_path = "/tmp/sb_security_sblr_api_probe.sbdb";
  context.local_transaction_id = 11;
  context.database_uuid.canonical = "018f0000-0000-7000-8000-00000000f001";
  context.trace_tags.push_back("security.bootstrap");
  return context;
}
}

int main() {
  sblr::SblrDispatchRequest request;
  request.context = Context();

  request.envelope = sblr::MakeSblrEnvelope("security.resolve_authority", "security.resolve_authority", "TRACE-SEC-AUTHORITY");
  request.api_request.option_envelopes.push_back("authority_class:database_local");
  const auto authority = sblr::DispatchSblrOperation(request);

  request.api_request = api::EngineApiRequest{};
  request.envelope = sblr::MakeSblrEnvelope("security.authorize", "security.authorize", "TRACE-SEC-AUTHZ");
  request.api_request.option_envelopes.push_back("right:SEC_GRANT_ADMIN");
  const auto authz = sblr::DispatchSblrOperation(request);

  request.api_request = api::EngineApiRequest{};
  request.envelope = sblr::MakeSblrEnvelope("security.seed_standard_bundles", "security.seed_standard_bundles", "TRACE-SEC-BUNDLE");
  const auto seed = sblr::DispatchSblrOperation(request);

  request.api_request = api::EngineApiRequest{};
  request.envelope = sblr::MakeSblrEnvelope("security.missing", "security.missing", "TRACE-SEC-MISSING");
  const auto missing = sblr::DispatchSblrOperation(request);

  const bool ok = authority.accepted && authority.api_result.ok && authz.accepted && authz.api_result.ok && seed.accepted && seed.api_result.ok && !missing.accepted;
  std::cout << "{\"ok\":" << (ok ? "true" : "false")
            << ",\"authority\":" << (authority.api_result.ok ? "true" : "false")
            << ",\"authorize\":" << (authz.api_result.ok ? "true" : "false")
            << ",\"seed\":" << (seed.api_result.ok ? "true" : "false")
            << ",\"missing_rejected\":" << (!missing.accepted ? "true" : "false") << "}\n";
  return ok ? 0 : 1;
}

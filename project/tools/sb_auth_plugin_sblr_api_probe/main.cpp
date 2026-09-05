// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "../auth_provider_probe_common/probe_common.hpp"
#include "sblr_dispatch.hpp"
#include "sblr_opcode_registry.hpp"

#include <iostream>

namespace api = scratchbird::engine::internal_api;
namespace sblr = scratchbird::engine::sblr;

void PrintFailure(const char* label, const sblr::SblrDispatchResult& result) {
  if (result.api_result.ok) { return; }
  for (const auto& diagnostic : result.api_result.diagnostics) {
    std::cerr << label << ':' << diagnostic.code << ':' << diagnostic.detail
              << '\n';
  }
  for (const auto& diagnostic : result.diagnostics) {
    std::cerr << label << ':' << diagnostic.code << ':'
              << diagnostic.message << '\n';
  }
}

bool HasDispatchDiagnostic(const sblr::SblrDispatchResult& result,
                           const std::string& code) {
  for (const auto& diagnostic : result.diagnostics) {
    if (diagnostic.code == code) { return true; }
  }
  return false;
}

int main() {
  api::EngineRequestContext context = sb_auth_probe::Context();
  context.database_path = "/tmp/sb_auth_provider_sblr_probe.sbdb";
  context.local_transaction_id = 500;

  sblr::SblrDispatchRequest request;
  request.context = context;
  request.envelope = sblr::MakeSblrEnvelope(
      "security.register_auth_provider", "SBLR_SECURITY_REGISTER_AUTH_PROVIDER",
      "TRACE-AUTHP-REGISTER");
  request.api_request.operation_id = "security.register_auth_provider";
  request.api_request.option_envelopes = {
      "provider:ldap_ad", "provider_enabled:true", "signature_valid:true",
      "provenance_valid:true", "dependency:ldap_client:available"};
  const auto registered = sblr::DispatchSblrOperation(request);

  request.api_request = api::EngineApiRequest{};
  request.envelope = sblr::MakeSblrEnvelope(
      "security.authenticate_provider", "SBLR_SECURITY_AUTHENTICATE_PROVIDER",
      "TRACE-AUTHP-AUTH");
  request.api_request.operation_id = "security.authenticate_provider";
  request.api_request.option_envelopes = {
      "provider:ldap_ad", "provider_enabled:true", "auth_flow:login",
      "dependency:ldap_client:available", "credential:valid",
      "fixture:success", "principal:alice", "groups:materialized"};
  const auto authenticated = sblr::DispatchSblrOperation(request);

  request.api_request = api::EngineApiRequest{};
  request.envelope = sblr::MakeSblrEnvelope(
      "security.revoke_token", "SBLR_SECURITY_REVOKE_TOKEN",
      "TRACE-AUTHP-REVOKE");
  request.api_request.operation_id = "security.revoke_token";
  request.api_request.option_envelopes = {
      "provider:token_api_key",
      "token_uuid:018f0000-0000-7000-8000-0000000b0001"};
  const auto revoked = sblr::DispatchSblrOperation(request);

  PrintFailure("register", registered);
  PrintFailure("authenticate", authenticated);
  PrintFailure("revoke", revoked);

  const bool routes_absent =
      sblr::LookupSblrOperation("security.register_auth_provider") == nullptr &&
      sblr::LookupSblrOperation("security.authenticate_provider") == nullptr &&
      sblr::LookupSblrOperation("security.revoke_token") == nullptr;
  const bool routes_refused =
      !registered.accepted && !authenticated.accepted && !revoked.accepted &&
      HasDispatchDiagnostic(registered, "SBLR.OPERATION.HEADER_INVALID") &&
      HasDispatchDiagnostic(authenticated, "SBLR.OPERATION.HEADER_INVALID") &&
      HasDispatchDiagnostic(revoked, "SBLR.OPERATION.HEADER_INVALID");
  const bool ok = routes_absent && routes_refused;
  std::cout << "{\"routes_absent\":" << (routes_absent ? "true" : "false")
            << ",\"unregistered_routes_refused\":"
            << (routes_refused ? "true" : "false")
            << ",\"ok\":" << (ok ? "true" : "false") << "}\n";
  return ok ? 0 : 1;
}

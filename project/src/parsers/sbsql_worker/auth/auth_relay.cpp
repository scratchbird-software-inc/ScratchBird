// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "auth/auth_relay.hpp"

namespace scratchbird::parser::sbsql {

AuthRelayResult FailClosedAuthRelay(const AuthRelayRequest& request, const ParserConfig& config) {
  AuthRelayResult result;
  result.messages.diagnostics.push_back(MakeDiagnostic(
      config.server_endpoint.empty() ? "SBSQL.AUTH.SERVER_ENDPOINT_REQUIRED" : "SBSQL.AUTH.SERVER_RELAY_UNAVAILABLE",
      "ERROR", "authentication must be relayed to sb_server; parser cannot authenticate locally",
      "sbp_sbsql.auth", {{"provider_id", request.provider_id}}));
  return result;
}

AuthRelayResult ProbeAuthRelay(const AuthRelayRequest& request, const ParserConfig& config) {
  (void)config;
  AuthRelayResult result;
  result.messages.diagnostics.push_back(MakeDiagnostic(
      "SBSQL.AUTH.ENGINE_AUTHORITY_REQUIRED",
      "ERROR",
      "authentication must be decided by the engine; parser probe authentication is forbidden",
      "sbp_sbsql.auth",
      {{"provider_id", request.provider_id}}));
  return result;
}

} // namespace scratchbird::parser::sbsql

// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// SEARCH_KEY: AEIC_SESSION_CONTROL_SERVER_ROUTE_BRIDGE
// Server-owned bridge for session-control agent decisions. The bridge mutates
// only the server session registry; it is not transaction finality, visibility,
// security, parser, SBLR, recovery, or cluster authority.

#include "session_registry.hpp"

#include "agents/session_control_manager.hpp"

#include <string>
#include <vector>

namespace scratchbird::server {

struct AgentSessionControlRouteResult {
  scratchbird::core::agents::implemented_agents::SessionControlManagerResult
      manager_result;
  bool registry_mutated = false;
  bool session_removed = false;
  bool reauth_required = false;
  bool token_revoked = false;
  std::string diagnostic_code;
  std::vector<std::string> evidence;

  bool ok() const {
    // A successful manager decision is only a plan. In particular, a later
    // missing-target/error diagnostic must not be treated as actuator success.
    // This checks result consistency; the actuator still owns proving effects.
    if (!manager_result.ok() || !registry_mutated) return false;
    using Decision = scratchbird::core::agents::implemented_agents::
        SessionControlManagerDecisionKind;
    switch (manager_result.decision) {
      case Decision::force_disconnect:
        return session_removed && !reauth_required && !token_revoked &&
               diagnostic_code == "SB_AGENT_SESSION_CONTROL_ROUTE.DISCONNECTED";
      case Decision::require_reauth:
        return reauth_required && !session_removed && !token_revoked &&
               diagnostic_code == "SB_AGENT_SESSION_CONTROL_ROUTE.REAUTH_REQUIRED";
      case Decision::revoke_session:
        return token_revoked && !session_removed && !reauth_required &&
               diagnostic_code == "SB_AGENT_SESSION_CONTROL_ROUTE.SESSION_REVOKED";
      case Decision::refused:
        return false;
    }
    return false;
  }
};

AgentSessionControlRouteResult ApplySessionControlAgentRoute(
    ServerSessionRegistry* registry,
    scratchbird::core::agents::AgentLocalWorkflowLedger* ledger,
    scratchbird::core::agents::implemented_agents::SessionControlManagerRequest request);

}  // namespace scratchbird::server

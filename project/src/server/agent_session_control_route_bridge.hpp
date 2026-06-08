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
    return manager_result.ok() && !diagnostic_code.empty() &&
           diagnostic_code != "SB_AGENT_SESSION_CONTROL_ROUTE.REFUSED";
  }
};

AgentSessionControlRouteResult ApplySessionControlAgentRoute(
    ServerSessionRegistry* registry,
    scratchbird::core::agents::AgentLocalWorkflowLedger* ledger,
    scratchbird::core::agents::implemented_agents::SessionControlManagerRequest request);

}  // namespace scratchbird::server

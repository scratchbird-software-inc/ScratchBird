// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "agent_session_control_route_bridge.hpp"

// SEARCH_KEY: AEIC_SESSION_CONTROL_SERVER_ROUTE_BRIDGE

namespace scratchbird::server {

namespace agents = scratchbird::core::agents;
namespace implemented = scratchbird::core::agents::implemented_agents;

AgentSessionControlRouteResult ApplySessionControlAgentRoute(
    ServerSessionRegistry* registry,
    agents::AgentLocalWorkflowLedger* ledger,
    implemented::SessionControlManagerRequest request) {
  AgentSessionControlRouteResult result;
  if (registry == nullptr) {
    result.manager_result =
        implemented::EvaluateSessionControlManagerRequest(nullptr, request);
    result.diagnostic_code =
        "SB_AGENT_SESSION_CONTROL_ROUTE.REGISTRY_REQUIRED";
    result.evidence.push_back("server_session_registry_required");
    return result;
  }

  result.manager_result =
      implemented::EvaluateSessionControlManagerRequest(ledger, request);
  if (!result.manager_result.ok()) {
    result.diagnostic_code = "SB_AGENT_SESSION_CONTROL_ROUTE.REFUSED";
    result.evidence.push_back(result.manager_result.diagnostic.diagnostic_code);
    return result;
  }

  const auto found = registry->sessions_by_uuid.find(request.session_uuid);
  if (found == registry->sessions_by_uuid.end()) {
    result.diagnostic_code =
        "SB_AGENT_SESSION_CONTROL_ROUTE.SESSION_NOT_FOUND";
    result.evidence.push_back("session_not_found");
    return result;
  }

  switch (result.manager_result.decision) {
    case implemented::SessionControlManagerDecisionKind::force_disconnect:
      registry->sessions_by_uuid.erase(found);
      result.registry_mutated = true;
      result.session_removed = true;
      result.diagnostic_code =
          "SB_AGENT_SESSION_CONTROL_ROUTE.DISCONNECTED";
      result.evidence.push_back("server_session_registry_removed=true");
      break;
    case implemented::SessionControlManagerDecisionKind::require_reauth:
      found->second.engine_authorization_trace_tags.push_back(
          "agent.session_control.require_reauth");
      result.registry_mutated = true;
      result.reauth_required = true;
      result.diagnostic_code =
          "SB_AGENT_SESSION_CONTROL_ROUTE.REAUTH_REQUIRED";
      result.evidence.push_back("server_session_reauth_required=true");
      break;
    case implemented::SessionControlManagerDecisionKind::revoke_session:
      found->second.engine_authorization_trace_tags.push_back(
          "agent.session_control.revoke_session");
      result.registry_mutated = true;
      result.token_revoked = true;
      result.diagnostic_code =
          "SB_AGENT_SESSION_CONTROL_ROUTE.SESSION_REVOKED";
      result.evidence.push_back("server_session_revoked=true");
      break;
    case implemented::SessionControlManagerDecisionKind::refused:
      result.diagnostic_code = "SB_AGENT_SESSION_CONTROL_ROUTE.REFUSED";
      result.evidence.push_back("session_decision_refused");
      break;
  }
  result.evidence.push_back("transaction_finality_authority=false");
  result.evidence.push_back("parser_authority=false");
  result.evidence.push_back("cluster_authority=false");
  return result;
}

}  // namespace scratchbird::server

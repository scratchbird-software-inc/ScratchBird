// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "server/agent_session_control_route_bridge.hpp"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <string_view>

int main() {
  using Result = scratchbird::server::AgentSessionControlRouteResult;
  using Decision = scratchbird::core::agents::implemented_agents::
      SessionControlManagerDecisionKind;
  using Code = scratchbird::core::platform::StatusCode;
  constexpr std::array decisions{Decision::force_disconnect,
      Decision::require_reauth, Decision::revoke_session, Decision::refused,
      static_cast<Decision>(255)};
  constexpr std::array<std::string_view, 9> diagnostics{
      "SB_AGENT_SESSION_CONTROL_ROUTE.DISCONNECTED",
      "SB_AGENT_SESSION_CONTROL_ROUTE.REAUTH_REQUIRED",
      "SB_AGENT_SESSION_CONTROL_ROUTE.SESSION_REVOKED",
      "SB_AGENT_SESSION_CONTROL_ROUTE.REGISTRY_REQUIRED",
      "SB_AGENT_SESSION_CONTROL_ROUTE.REFUSED",
      "SB_AGENT_SESSION_CONTROL_ROUTE.SESSION_NOT_FOUND", "", "unknown",
      "SB_AGENT_SESSION_CONTROL_ROUTE.DISCONNECTED_extra"};
  std::size_t checks = 0;
  // Independent complete truth table for the actual production predicate.
  // No workflow, security actuator, persistence or live IPC is substituted.
  for (std::size_t decision = 0; decision != decisions.size(); ++decision) {
    for (unsigned effects = 0; effects != 16; ++effects) {
      for (std::size_t diagnostic = 0; diagnostic != diagnostics.size(); ++diagnostic) {
        for (unsigned state = 0; state != 4; ++state) {
          Result result;
          result.manager_result.decision = decisions[decision];
          result.manager_result.status.code =
              (state & 1) ? Code::memory_invalid_request : Code::ok;
          result.manager_result.fail_closed = (state & 2) != 0;
          result.registry_mutated = (effects & 1) != 0;
          result.session_removed = (effects & 2) != 0;
          result.reauth_required = (effects & 4) != 0;
          result.token_revoked = (effects & 8) != 0;
          result.diagnostic_code = diagnostics[diagnostic];
          const bool expected = state == 0 && decision < 3 &&
              diagnostic == decision &&
              effects == (1u | (2u << decision));
          ++checks;
          if (result.ok() != expected) {
            std::fprintf(stderr, "agent session result mismatch: decision=%zu effects=%u diagnostic=%zu state=%u\n",
                         decision, effects, diagnostic, state);
            return EXIT_FAILURE;
          }
        }
      }
    }
  }
  std::printf("agent session-control result predicate: PASS %zu checks; not actuator acceptance\n", checks);
}

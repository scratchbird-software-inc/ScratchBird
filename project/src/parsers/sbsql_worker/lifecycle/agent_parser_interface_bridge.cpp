// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "lifecycle/agent_parser_interface_bridge.hpp"

#include <utility>

namespace scratchbird::parser::sbsql {
// SEARCH_KEY: AEIC_PARSER_INTERFACE_LIFECYCLE_BRIDGE
namespace {

namespace impl = scratchbird::core::agents::implemented_agents;

ParserInterfaceAgentLifecycleRouteResult Refuse(ParserLifecycle* lifecycle,
                                                std::string code,
                                                std::string detail) {
  ParserInterfaceAgentLifecycleRouteResult result;
  result.ok = false;
  result.fail_closed = true;
  result.state = lifecycle == nullptr ? ParserLifecycleState::kConstructed
                                      : lifecycle->state();
  result.diagnostic_code = std::move(code);
  result.evidence.push_back("diagnostic_code=" + result.diagnostic_code);
  result.evidence.push_back("detail=" + std::move(detail));
  result.evidence.push_back("parser_execution_authority=false");
  result.evidence.push_back("parser_finality_authority=false");
  result.evidence.push_back("parser_sblr_authority=false");
  result.evidence.push_back("transaction_finality_authority=false");
  result.evidence.push_back("visibility_authority=false");
  result.evidence.push_back("recovery_authority=false");
  return result;
}

ParserInterfaceAgentLifecycleRouteResult Accept(ParserLifecycle* lifecycle,
                                                std::string code) {
  ParserInterfaceAgentLifecycleRouteResult result;
  result.ok = true;
  result.fail_closed = false;
  result.state = lifecycle == nullptr ? ParserLifecycleState::kConstructed
                                      : lifecycle->state();
  result.diagnostic_code = std::move(code);
  result.evidence.push_back("diagnostic_code=" + result.diagnostic_code);
  result.evidence.push_back("agent_route_authority=engine_parser_supervisor");
  result.evidence.push_back("durable_runtime_store_authority=true");
  result.evidence.push_back("parser_execution_authority=false");
  result.evidence.push_back("parser_finality_authority=false");
  result.evidence.push_back("parser_sblr_authority=false");
  result.evidence.push_back("transaction_finality_authority=false");
  result.evidence.push_back("visibility_authority=false");
  result.evidence.push_back("recovery_authority=false");
  return result;
}

bool CanRecycleForDrain(ParserLifecycleState state) {
  return state != ParserLifecycleState::kTerminated &&
         state != ParserLifecycleState::kQuarantined &&
         state != ParserLifecycleState::kFailed;
}

}  // namespace

ParserInterfaceAgentLifecycleRouteResult
ApplyParserInterfaceAgentLifecycleRoute(
    const ParserInterfaceAgentLifecycleRouteRequest& request) {
  if (request.lifecycle == nullptr || request.decision == nullptr) {
    return Refuse(request.lifecycle,
                  "SB_AGENT_PARSER_INTERFACE_ROUTE.MISSING_INPUT",
                  "parser lifecycle and agent decision are required");
  }
  if (!request.engine_supervisor_authority ||
      !request.durable_runtime_store_authority ||
      !request.parser_metrics_authoritative ||
      request.parser_execution_authority ||
      request.parser_finality_authority ||
      request.parser_sblr_authority) {
    return Refuse(request.lifecycle,
                  "SB_AGENT_PARSER_INTERFACE_ROUTE.UNSAFE_AUTHORITY",
                  "route requires engine supervisor durable store and metric authority only");
  }
  if (!request.decision->status.ok() || request.decision->fail_closed ||
      request.decision->decision == impl::ParserInterfaceManagerDecisionKind::refused ||
      !request.decision->parser_authority_preserved) {
    return Refuse(request.lifecycle,
                  "SB_AGENT_PARSER_INTERFACE_ROUTE.DECISION_REFUSED",
                  request.decision->diagnostic.diagnostic_code);
  }

  switch (request.decision->decision) {
    case impl::ParserInterfaceManagerDecisionKind::no_action: {
      auto result = Accept(request.lifecycle,
                           "SB_AGENT_PARSER_INTERFACE_ROUTE.NO_ACTION");
      result.no_action = true;
      return result;
    }
    case impl::ParserInterfaceManagerDecisionKind::drain_parser_family: {
      if (!CanRecycleForDrain(request.lifecycle->state())) {
        return Refuse(request.lifecycle,
                      "SB_AGENT_PARSER_INTERFACE_ROUTE.DRAIN_STATE_REFUSED",
                      "parser lifecycle state cannot enter drain/recycle");
      }
      ParserLifecycleResult transition;
      if (request.lifecycle->state() == ParserLifecycleState::kActive) {
        transition = request.lifecycle->RecordCancelRequested();
      } else {
        transition = request.lifecycle->RecordRecycleRequested();
      }
      if (!transition.accepted) {
        const std::string code = transition.diagnostics.empty()
                                     ? "PARSER.LIFECYCLE.INVALID_TRANSITION"
                                     : transition.diagnostics.front().code;
        return Refuse(request.lifecycle,
                      "SB_AGENT_PARSER_INTERFACE_ROUTE.DRAIN_FAILED",
                      code);
      }
      auto result = Accept(request.lifecycle,
                           "SB_AGENT_PARSER_INTERFACE_ROUTE.DRAIN_APPLIED");
      result.drain_applied = true;
      result.evidence.push_back("lifecycle_state_after=" +
                                std::string(ParserLifecycleStateName(result.state)));
      return result;
    }
    case impl::ParserInterfaceManagerDecisionKind::quarantine_parser_package: {
      const auto failed =
          request.lifecycle->RecordFailure("parser_interface_manager_quarantine");
      if (!failed.accepted) {
        return Refuse(request.lifecycle,
                      "SB_AGENT_PARSER_INTERFACE_ROUTE.QUARANTINE_FAILURE_REFUSED",
                      failed.diagnostics.empty() ? "unknown"
                                                 : failed.diagnostics.front().code);
      }
      ParserFailurePolicy policy;
      policy.quarantine_failures_10m = 1;
      policy.quarantine_failures_1h = 1;
      const auto quarantined = request.lifecycle->ApplyFailurePolicy(policy);
      if (!quarantined.accepted ||
          request.lifecycle->state() != ParserLifecycleState::kQuarantined) {
        return Refuse(request.lifecycle,
                      "SB_AGENT_PARSER_INTERFACE_ROUTE.QUARANTINE_FAILED",
                      quarantined.diagnostics.empty()
                          ? "quarantine policy did not accept"
                          : quarantined.diagnostics.front().code);
      }
      auto result = Accept(request.lifecycle,
                           "SB_AGENT_PARSER_INTERFACE_ROUTE.QUARANTINE_APPLIED");
      result.quarantine_applied = true;
      result.evidence.push_back("lifecycle_state_after=quarantined");
      return result;
    }
    case impl::ParserInterfaceManagerDecisionKind::refused:
      break;
  }
  return Refuse(request.lifecycle,
                "SB_AGENT_PARSER_INTERFACE_ROUTE.DECISION_REFUSED",
                "refused parser-interface decision");
}

}  // namespace scratchbird::parser::sbsql

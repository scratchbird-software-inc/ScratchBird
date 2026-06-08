// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "agents/parser_interface_manager.hpp"

// CanonicalAgentRegistry/CanonicalAgentManifest owns production exposure;
// this file provides the parser-interface non-authority handler.

#include <utility>

namespace scratchbird::core::agents::implemented_agents {
namespace {

using scratchbird::core::platform::Severity;
using scratchbird::core::platform::StatusCode;
using scratchbird::core::platform::Subsystem;

Status OkStatus() { return {StatusCode::ok, Severity::info, Subsystem::engine}; }
Status ErrorStatus() {
  return {StatusCode::memory_invalid_request, Severity::error, Subsystem::engine};
}

void AddEvidence(ParserInterfaceManagerResult* result,
                 std::string key,
                 std::string value) {
  result->evidence.push_back({std::move(key), std::move(value)});
}

ParserInterfaceManagerResult Finish(ParserInterfaceManagerDecisionKind decision,
                                    Status status,
                                    std::string code,
                                    std::string key,
                                    std::string detail,
                                    bool fail_closed) {
  ParserInterfaceManagerResult result;
  result.status = status;
  result.decision = decision;
  result.fail_closed = fail_closed;
  result.parser_authority_preserved = true;
  result.diagnostic = MakeParserInterfaceManagerDiagnostic(
      result.status, std::move(code), std::move(key), std::move(detail));
  AddEvidence(&result, "decision",
              ParserInterfaceManagerDecisionKindName(result.decision));
  AddEvidence(&result, "parser_execution_authority", "false");
  AddEvidence(&result, "parser_finality_authority", "false");
  return result;
}

}  // namespace

const char* ParserInterfaceManagerDecisionKindName(
    ParserInterfaceManagerDecisionKind decision) {
  switch (decision) {
    case ParserInterfaceManagerDecisionKind::no_action: return "no_action";
    case ParserInterfaceManagerDecisionKind::drain_parser_family:
      return "drain_parser_family";
    case ParserInterfaceManagerDecisionKind::quarantine_parser_package:
      return "quarantine_parser_package";
    case ParserInterfaceManagerDecisionKind::refused: return "refused";
  }
  return "refused";
}

DiagnosticRecord MakeParserInterfaceManagerDiagnostic(
    Status status,
    std::string diagnostic_code,
    std::string message_key,
    std::string detail) {
  return scratchbird::core::platform::MakeDiagnostic(
      status.code, status.severity, status.subsystem,
      std::move(diagnostic_code), std::move(message_key),
      {{"detail", std::move(detail)}}, {}, "parser_interface_manager", {});
}

ParserInterfaceManagerResult EvaluateParserInterfaceManager(
    const ParserInterfaceManagerSnapshot& snapshot,
    const ParserInterfaceManagerPolicy& policy) {
  if (!policy.present || !policy.valid || !policy.scope_compatible ||
      snapshot.parser_family.empty()) {
    return Finish(ParserInterfaceManagerDecisionKind::refused, ErrorStatus(),
                  "SB_AGENT_PARSER_INTERFACE_POLICY_INVALID",
                  "agents.parser_interface.policy_invalid",
                  "policy and parser family are required", true);
  }
  if (!snapshot.parser_metrics_authoritative ||
      snapshot.parser_execution_authority ||
      snapshot.parser_finality_authority) {
    return Finish(ParserInterfaceManagerDecisionKind::refused, ErrorStatus(),
                  "SB_AGENT_PARSER_INTERFACE_AUTHORITY_UNTRUSTED",
                  "agents.parser_interface.untrusted_authority",
                  "parser manager cannot accept parser execution/finality authority",
                  true);
  }
  if ((!snapshot.package_signature_valid || snapshot.security_event_present) &&
      policy.quarantine_allowed) {
    return Finish(ParserInterfaceManagerDecisionKind::quarantine_parser_package,
                  OkStatus(),
                  "SB_AGENT_PARSER_INTERFACE_QUARANTINE",
                  "agents.parser_interface.quarantine",
                  "signature or security evidence requires package quarantine",
                  false);
  }
  if (snapshot.parser_crashes_total >= policy.crash_threshold &&
      snapshot.parser_sessions_active == 0 && policy.drain_allowed) {
    return Finish(ParserInterfaceManagerDecisionKind::drain_parser_family,
                  OkStatus(),
                  "SB_AGENT_PARSER_INTERFACE_DRAIN",
                  "agents.parser_interface.drain",
                  "parser crash count exceeds policy threshold", false);
  }
  return Finish(ParserInterfaceManagerDecisionKind::no_action, OkStatus(),
                "SB_AGENT_PARSER_INTERFACE_NO_ACTION",
                "agents.parser_interface.no_action",
                "parser interface state within policy", false);
}

const char* parser_interface_manager_implementation_anchor() {
  return "parser_interface_manager";
}

}  // namespace scratchbird::core::agents::implemented_agents

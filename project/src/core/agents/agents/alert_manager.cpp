// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "agents/alert_manager.hpp"

// CanonicalAgentRegistry/CanonicalAgentManifest owns production exposure;
// this file provides the local alert-evaluation handler.

#include <utility>

namespace scratchbird::core::agents::implemented_agents {
namespace {

using scratchbird::core::platform::Severity;
using scratchbird::core::platform::StatusCode;
using scratchbird::core::platform::Subsystem;

Status OkStatus() {
  return {StatusCode::ok, Severity::info, Subsystem::engine};
}

Status ErrorStatus() {
  return {StatusCode::memory_invalid_request, Severity::error, Subsystem::engine};
}

void AddEvidence(AlertManagerResult* result,
                 std::string key,
                 std::string value) {
  result->evidence.push_back({std::move(key), std::move(value)});
}

AlertManagerResult Finish(AlertManagerDecisionKind decision,
                          Status status,
                          std::string code,
                          std::string key,
                          std::string detail,
                          bool fail_closed) {
  AlertManagerResult result;
  result.status = status;
  result.decision = decision;
  result.fail_closed = fail_closed;
  result.alert_fired = decision == AlertManagerDecisionKind::fire_alert;
  result.alert_silenced = decision == AlertManagerDecisionKind::silence_alert;
  result.alert_cleared = decision == AlertManagerDecisionKind::clear_alert;
  result.diagnostic = MakeAlertManagerDiagnostic(result.status,
                                                 std::move(code),
                                                 std::move(key),
                                                 std::move(detail));
  AddEvidence(&result, "decision", AlertManagerDecisionKindName(result.decision));
  AddEvidence(&result, "failed_closed", fail_closed ? "true" : "false");
  return result;
}

}  // namespace

const char* AlertManagerDecisionKindName(AlertManagerDecisionKind decision) {
  switch (decision) {
    case AlertManagerDecisionKind::no_action: return "no_action";
    case AlertManagerDecisionKind::fire_alert: return "fire_alert";
    case AlertManagerDecisionKind::silence_alert: return "silence_alert";
    case AlertManagerDecisionKind::clear_alert: return "clear_alert";
    case AlertManagerDecisionKind::refused: return "refused";
  }
  return "refused";
}

const char* AlertSeverityName(AlertSeverity severity) {
  switch (severity) {
    case AlertSeverity::info: return "info";
    case AlertSeverity::warning: return "warning";
    case AlertSeverity::critical: return "critical";
  }
  return "warning";
}

DiagnosticRecord MakeAlertManagerDiagnostic(Status status,
                                            std::string diagnostic_code,
                                            std::string message_key,
                                            std::string detail) {
  return scratchbird::core::platform::MakeDiagnostic(
      status.code,
      status.severity,
      status.subsystem,
      std::move(diagnostic_code),
      std::move(message_key),
      {{"detail", std::move(detail)}},
      {},
      "alert_manager",
      {});
}

AlertManagerResult EvaluateAlertManagerRequest(
    const AlertManagerRequest& request,
    const AlertManagerPolicy& policy) {
  if (!policy.present || !policy.valid || !policy.scope_compatible) {
    return Finish(AlertManagerDecisionKind::refused, ErrorStatus(),
                  "SB_AGENT_ALERT_POLICY_INVALID",
                  "agents.alert.policy_invalid",
                  "policy missing invalid or outside scope",
                  true);
  }
  if (request.alert_key.empty() || request.now_microseconds == 0) {
    return Finish(AlertManagerDecisionKind::refused, ErrorStatus(),
                  "SB_AGENT_ALERT_REQUEST_INVALID",
                  "agents.alert.request_invalid",
                  "alert key and observation time are required",
                  true);
  }
  if (!request.trusted_evidence_present ||
      !request.redaction_policy_valid ||
      request.parser_authority || request.sidecar_authority) {
    return Finish(AlertManagerDecisionKind::refused, ErrorStatus(),
                  "SB_AGENT_ALERT_AUTHORITY_UNTRUSTED",
                  "agents.alert.untrusted_authority",
                  "alert routing requires trusted evidence and redaction policy",
                  true);
  }
  if (request.silence_requested) {
    if (!policy.silence_allowed ||
        request.requested_silence_microseconds == 0 ||
        request.requested_silence_microseconds >
            policy.max_silence_microseconds) {
      return Finish(AlertManagerDecisionKind::refused, ErrorStatus(),
                    "SB_AGENT_ALERT_SILENCE_REFUSED",
                    "agents.alert.silence_refused",
                    "silence duration outside policy",
                    true);
    }
    auto result = Finish(AlertManagerDecisionKind::silence_alert,
                         OkStatus(),
                         "SB_AGENT_ALERT_SILENCED",
                         "agents.alert.silenced",
                         "operator silence accepted under policy",
                         false);
    result.silence_until_microseconds =
        request.now_microseconds + request.requested_silence_microseconds;
    AddEvidence(&result, "silence_until_microseconds",
                std::to_string(result.silence_until_microseconds));
    return result;
  }
  if (request.clear_condition) {
    if (!policy.clear_allowed) {
      return Finish(AlertManagerDecisionKind::refused, ErrorStatus(),
                    "SB_AGENT_ALERT_CLEAR_REFUSED",
                    "agents.alert.clear_refused",
                    "clear operation disabled by policy",
                    true);
    }
    return Finish(AlertManagerDecisionKind::clear_alert,
                  OkStatus(),
                  "SB_AGENT_ALERT_CLEARED",
                  "agents.alert.cleared",
                  "clear condition observed",
                  false);
  }
  if (!request.condition_active) {
    return Finish(AlertManagerDecisionKind::no_action,
                  OkStatus(),
                  "SB_AGENT_ALERT_NO_ACTION",
                  "agents.alert.no_action",
                  "alert condition inactive",
                  false);
  }
  if (!policy.fire_allowed) {
    return Finish(AlertManagerDecisionKind::refused, ErrorStatus(),
                  "SB_AGENT_ALERT_FIRE_REFUSED",
                  "agents.alert.fire_refused",
                  "fire operation disabled by policy",
                  true);
  }
  if (request.last_fired_microseconds != 0 &&
      request.now_microseconds > request.last_fired_microseconds &&
      request.now_microseconds - request.last_fired_microseconds <
          policy.dedupe_window_microseconds) {
    auto result = Finish(AlertManagerDecisionKind::no_action,
                         OkStatus(),
                         "SB_AGENT_ALERT_DEDUPED",
                         "agents.alert.deduped",
                         "alert suppressed within dedupe window",
                         false);
    result.deduped = true;
    return result;
  }
  auto result = Finish(AlertManagerDecisionKind::fire_alert,
                       OkStatus(),
                       "SB_AGENT_ALERT_FIRED",
                       "agents.alert.fired",
                       "trusted alert condition fired",
                       false);
  AddEvidence(&result, "alert_key", request.alert_key);
  AddEvidence(&result, "severity", AlertSeverityName(request.severity));
  return result;
}

const char* alert_manager_implementation_anchor() {
  return "alert_manager";
}

}  // namespace scratchbird::core::agents::implemented_agents

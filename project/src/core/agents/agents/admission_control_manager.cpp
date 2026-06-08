// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "agents/admission_control_manager.hpp"

// CanonicalAgentRegistry/CanonicalAgentManifest owns production exposure;
// this file provides the local admission-control handler.

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

void AddEvidence(AdmissionControlResult* result,
                 std::string key,
                 std::string value) {
  result->evidence.push_back({std::move(key), std::move(value)});
}

AdmissionControlResult Finish(AdmissionControlDecisionKind decision,
                              Status status,
                              std::string code,
                              std::string key,
                              std::string detail,
                              bool fail_closed) {
  AdmissionControlResult result;
  result.status = status;
  result.decision = decision;
  result.fail_closed = fail_closed;
  result.request_allowed = decision == AdmissionControlDecisionKind::allow ||
                           decision == AdmissionControlDecisionKind::throttle_admission ||
                           decision == AdmissionControlDecisionKind::downgrade_admission;
  result.throttled = decision == AdmissionControlDecisionKind::throttle_admission;
  result.denied = decision == AdmissionControlDecisionKind::deny_admission;
  result.downgraded = decision == AdmissionControlDecisionKind::downgrade_admission;
  result.diagnostic = MakeAdmissionControlDiagnostic(result.status,
                                                     std::move(code),
                                                     std::move(key),
                                                     std::move(detail));
  AddEvidence(&result, "decision",
              AdmissionControlDecisionKindName(result.decision));
  AddEvidence(&result, "failed_closed", fail_closed ? "true" : "false");
  return result;
}

}  // namespace

const char* AdmissionControlDecisionKindName(
    AdmissionControlDecisionKind decision) {
  switch (decision) {
    case AdmissionControlDecisionKind::allow: return "allow";
    case AdmissionControlDecisionKind::throttle_admission:
      return "throttle_admission";
    case AdmissionControlDecisionKind::deny_admission:
      return "deny_admission";
    case AdmissionControlDecisionKind::downgrade_admission:
      return "downgrade_admission";
    case AdmissionControlDecisionKind::refused: return "refused";
  }
  return "refused";
}

DiagnosticRecord MakeAdmissionControlDiagnostic(Status status,
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
      "admission_control_manager",
      {});
}

AdmissionControlResult EvaluateAdmissionControlRequest(
    const AdmissionControlSnapshot& snapshot,
    const AdmissionControlPolicy& policy) {
  if (!policy.present || !policy.valid || !policy.scope_compatible) {
    return Finish(AdmissionControlDecisionKind::refused, ErrorStatus(),
                  "SB_AGENT_ADMISSION_POLICY_INVALID",
                  "agents.admission_control.policy_invalid",
                  "policy missing invalid or outside scope",
                  true);
  }
  if (!snapshot.pressure_metrics_authoritative ||
      !snapshot.resource_ledger_authoritative ||
      snapshot.parser_authority || snapshot.client_authority) {
    return Finish(AdmissionControlDecisionKind::refused, ErrorStatus(),
                  "SB_AGENT_ADMISSION_AUTHORITY_UNTRUSTED",
                  "agents.admission_control.untrusted_authority",
                  "admission decisions require trusted pressure metrics and ledger evidence",
                  true);
  }
  if (snapshot.emergency_reserve_bytes < policy.min_emergency_reserve_bytes &&
      !snapshot.request_is_admin && policy.deny_allowed) {
    auto result = Finish(AdmissionControlDecisionKind::deny_admission,
                         OkStatus(),
                         "SB_AGENT_ADMISSION_DENIED",
                         "agents.admission_control.deny",
                         "emergency reserve below policy floor",
                         false);
    result.foreground_protected = snapshot.foreground_database_work_active;
    AddEvidence(&result, "emergency_reserve_bytes",
                std::to_string(snapshot.emergency_reserve_bytes));
    return result;
  }
  if (snapshot.scheduler_queue_depth >= policy.deny_scheduler_queue_depth &&
      policy.deny_allowed) {
    auto result = Finish(AdmissionControlDecisionKind::deny_admission,
                         OkStatus(),
                         "SB_AGENT_ADMISSION_SCHEDULER_DENIED",
                         "agents.admission_control.scheduler_deny",
                         "scheduler queue exceeds deny threshold",
                         false);
    AddEvidence(&result, "scheduler_queue_depth",
                std::to_string(snapshot.scheduler_queue_depth));
    return result;
  }
  if (snapshot.slo_burn_rate_per_mille >=
          policy.downgrade_slo_burn_rate_per_mille &&
      policy.downgrade_allowed) {
    auto result = Finish(AdmissionControlDecisionKind::downgrade_admission,
                         OkStatus(),
                         "SB_AGENT_ADMISSION_DOWNGRADED",
                         "agents.admission_control.downgrade",
                         "workload SLO burn rate exceeds downgrade threshold",
                         false);
    AddEvidence(&result, "slo_burn_rate_per_mille",
                std::to_string(snapshot.slo_burn_rate_per_mille));
    return result;
  }
  if (snapshot.listener_queue_depth >= policy.throttle_listener_queue_depth &&
      policy.throttle_allowed) {
    auto result = Finish(AdmissionControlDecisionKind::throttle_admission,
                         OkStatus(),
                         "SB_AGENT_ADMISSION_THROTTLED",
                         "agents.admission_control.throttle",
                         "listener queue exceeds throttle threshold",
                         false);
    AddEvidence(&result, "listener_queue_depth",
                std::to_string(snapshot.listener_queue_depth));
    return result;
  }
  auto result = Finish(AdmissionControlDecisionKind::allow,
                       OkStatus(),
                       "SB_AGENT_ADMISSION_ALLOWED",
                       "agents.admission_control.allow",
                       "admission pressure remains within policy",
                       false);
  result.foreground_protected = snapshot.foreground_database_work_active;
  AddEvidence(&result, "request_is_admin",
              snapshot.request_is_admin ? "true" : "false");
  return result;
}

const char* admission_control_manager_implementation_anchor() {
  return "admission_control_manager";
}

}  // namespace scratchbird::core::agents::implemented_agents

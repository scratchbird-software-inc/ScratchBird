// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "agents/support_bundle_triage_agent.hpp"

// CanonicalAgentRegistry/CanonicalAgentManifest owns production exposure;
// this file provides the support-bundle triage handler.

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

void AddEvidence(SupportBundleTriageResult* result,
                 std::string key,
                 std::string value) {
  result->evidence.push_back({std::move(key), std::move(value)});
}

SupportBundleTriageResult Finish(SupportBundleTriageDecisionKind decision,
                                 Status status,
                                 std::string code,
                                 std::string key,
                                 std::string detail,
                                 bool fail_closed) {
  SupportBundleTriageResult result;
  result.status = status;
  result.decision = decision;
  result.fail_closed = fail_closed;
  result.diagnostic = MakeSupportBundleTriageDiagnostic(
      result.status, std::move(code), std::move(key), std::move(detail));
  AddEvidence(&result, "decision",
              SupportBundleTriageDecisionKindName(result.decision));
  AddEvidence(&result, "sidecar_authority", "false");
  return result;
}

}  // namespace

const char* SupportBundleTriageDecisionKindName(
    SupportBundleTriageDecisionKind decision) {
  switch (decision) {
    case SupportBundleTriageDecisionKind::no_action: return "no_action";
    case SupportBundleTriageDecisionKind::recommend_support_bundle:
      return "recommend_support_bundle";
    case SupportBundleTriageDecisionKind::prepare_redacted_bundle:
      return "prepare_redacted_bundle";
    case SupportBundleTriageDecisionKind::refused: return "refused";
  }
  return "refused";
}

DiagnosticRecord MakeSupportBundleTriageDiagnostic(
    Status status,
    std::string diagnostic_code,
    std::string message_key,
    std::string detail) {
  return scratchbird::core::platform::MakeDiagnostic(
      status.code, status.severity, status.subsystem,
      std::move(diagnostic_code), std::move(message_key),
      {{"detail", std::move(detail)}}, {}, "support_bundle_triage_agent", {});
}

SupportBundleTriageResult EvaluateSupportBundleTriage(
    const SupportBundleTriageSnapshot& snapshot,
    const SupportBundleTriagePolicy& policy) {
  if (!policy.present || !policy.valid || !policy.scope_compatible) {
    return Finish(SupportBundleTriageDecisionKind::refused, ErrorStatus(),
                  "SB_AGENT_SUPPORT_TRIAGE_POLICY_INVALID",
                  "agents.support_triage.policy_invalid",
                  "policy missing invalid or outside scope", true);
  }
  if (!snapshot.evidence_catalog_authoritative ||
      !snapshot.tamper_evidence_valid ||
      !snapshot.redaction_policy_valid ||
      snapshot.sidecar_authority) {
    return Finish(SupportBundleTriageDecisionKind::refused, ErrorStatus(),
                  "SB_AGENT_SUPPORT_TRIAGE_AUTHORITY_UNTRUSTED",
                  "agents.support_triage.untrusted_authority",
                  "support bundle triage requires durable evidence and redaction proof",
                  true);
  }
  if (snapshot.completeness_ratio_per_mille <
          policy.completeness_threshold_per_mille &&
      policy.recommendation_allowed) {
    return Finish(SupportBundleTriageDecisionKind::recommend_support_bundle,
                  OkStatus(),
                  "SB_AGENT_SUPPORT_TRIAGE_RECOMMENDED",
                  "agents.support_triage.recommended",
                  "support evidence completeness below policy threshold",
                  false);
  }
  if (snapshot.support_bundle_sink_available &&
      policy.redacted_bundle_allowed) {
    auto result = Finish(SupportBundleTriageDecisionKind::prepare_redacted_bundle,
                         OkStatus(),
                         "SB_AGENT_SUPPORT_TRIAGE_REDACTED_BUNDLE",
                         "agents.support_triage.redacted_bundle",
                         "durable evidence can be projected into support bundle",
                         false);
    result.protected_material_suppressed = snapshot.protected_material_present;
    AddEvidence(&result, "protected_material_suppressed",
                result.protected_material_suppressed ? "true" : "false");
    return result;
  }
  return Finish(SupportBundleTriageDecisionKind::no_action, OkStatus(),
                "SB_AGENT_SUPPORT_TRIAGE_NO_ACTION",
                "agents.support_triage.no_action",
                "support bundle evidence within policy", false);
}

const char* support_bundle_triage_agent_implementation_anchor() {
  return "support_bundle_triage_agent";
}

}  // namespace scratchbird::core::agents::implemented_agents

// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "agents/agent_support_bundle_triage_route_api.hpp"

#include <utility>

namespace scratchbird::engine::internal_api {
// SEARCH_KEY: AEIC_SUPPORT_BUNDLE_TRIAGE_ROUTE_API
namespace {

namespace impl = scratchbird::core::agents::implemented_agents;

AgentSupportBundleTriageRouteResult Refuse(std::string code,
                                           std::string detail) {
  AgentSupportBundleTriageRouteResult result;
  result.ok = false;
  result.fail_closed = true;
  result.diagnostic_code = std::move(code);
  result.evidence.push_back({"diagnostic_code", result.diagnostic_code});
  result.evidence.push_back({"detail", std::move(detail)});
  result.evidence.push_back({"sidecar_authority", "false"});
  result.evidence.push_back({"parser_authority", "false"});
  result.evidence.push_back({"transaction_finality_authority", "false"});
  result.evidence.push_back({"visibility_authority", "false"});
  result.evidence.push_back({"recovery_authority", "false"});
  return result;
}

bool HasOption(const std::vector<std::string>& options,
               const std::string& expected) {
  for (const auto& option : options) {
    if (option == expected) {
      return true;
    }
  }
  return false;
}

std::string FirstDiagnosticCode(const EnginePrepareSupportBundleResult& result) {
  if (!result.diagnostics.empty()) {
    return result.diagnostics.front().code;
  }
  return "OPS.SUPPORT_BUNDLE.UNKNOWN_REFUSAL";
}

}  // namespace

AgentSupportBundleTriageRouteResult ApplySupportBundleTriageAgentRoute(
    const AgentSupportBundleTriageRouteRequest& request) {
  if (!request.triage_result.status.ok() || request.triage_result.fail_closed ||
      request.triage_result.decision ==
          impl::SupportBundleTriageDecisionKind::refused) {
    return Refuse("SB_AGENT_SUPPORT_TRIAGE_ROUTE.DECISION_REFUSED",
                  request.triage_result.diagnostic.diagnostic_code);
  }
  if (!request.durable_evidence_store_authority ||
      !request.tamper_chain_verified ||
      !request.redaction_profile_authoritative ||
      !request.support_export_authorized_by_engine ||
      request.sidecar_authority ||
      request.agent_uuid.empty() ||
      request.evidence_uuid.empty()) {
    return Refuse("SB_AGENT_SUPPORT_TRIAGE_ROUTE.UNSAFE_AUTHORITY",
                  "durable evidence tamper chain redaction and engine support-export authority are required");
  }
  if (request.triage_result.decision ==
      impl::SupportBundleTriageDecisionKind::recommend_support_bundle) {
    AgentSupportBundleTriageRouteResult result;
    result.ok = true;
    result.fail_closed = false;
    result.diagnostic_code = "SB_AGENT_SUPPORT_TRIAGE_ROUTE.RECOMMENDED";
    result.evidence.push_back({"diagnostic_code", result.diagnostic_code});
    result.evidence.push_back({"support_bundle_route", "recommendation_only"});
    result.evidence.push_back({"sidecar_authority", "false"});
    return result;
  }
  if (request.triage_result.decision !=
      impl::SupportBundleTriageDecisionKind::prepare_redacted_bundle) {
    AgentSupportBundleTriageRouteResult result;
    result.ok = true;
    result.fail_closed = false;
    result.diagnostic_code = "SB_AGENT_SUPPORT_TRIAGE_ROUTE.NO_ACTION";
    result.evidence.push_back({"diagnostic_code", result.diagnostic_code});
    result.evidence.push_back({"support_bundle_route", "no_action"});
    return result;
  }

  auto support_request = request.support_request;
  if (!support_request.context.security_context_present ||
      !HasOption(support_request.option_envelopes,
                 "engine_authorized_support_export:true")) {
    return Refuse("SB_AGENT_SUPPORT_TRIAGE_ROUTE.ENGINE_AUTH_REQUIRED",
                  "support-bundle API must receive engine authorization and security context");
  }
  EngineSupportBundleAgentEvidenceSource source;
  source.agent_type_id = "support_bundle_triage_agent";
  source.agent_uuid = request.agent_uuid;
  source.evidence_uuid = request.evidence_uuid;
  source.evidence_kind = "agent_support_bundle_triage";
  source.result_state = "success";
  source.diagnostic_code = request.triage_result.diagnostic.diagnostic_code;
  source.payload_digest = "sha256:support_bundle_triage_agent_route";
  source.retention_class = "support_bundle_evidence";
  source.retention_policy_ref = "support.bundle.default_retention.v1";
  source.payload_redacted = true;
  support_request.agent_runtime_evidence.push_back(std::move(source));

  AgentSupportBundleTriageRouteResult result;
  result.support_result = EnginePrepareSupportBundle(support_request);
  if (!result.support_result.ok ||
      !result.support_result.redaction_applied ||
      !result.support_result.forbidden_fields_absent ||
      !result.support_result.flush_required_before_export ||
      !result.support_result.agent_runtime_evidence_collected) {
    return Refuse("SB_AGENT_SUPPORT_TRIAGE_ROUTE.SUPPORT_API_REFUSED",
                  FirstDiagnosticCode(result.support_result));
  }
  result.ok = true;
  result.fail_closed = false;
  result.support_bundle_prepared = true;
  result.protected_material_suppressed =
      request.triage_result.protected_material_suppressed;
  result.diagnostic_code = "SB_AGENT_SUPPORT_TRIAGE_ROUTE.PREPARED";
  result.evidence.push_back({"diagnostic_code", result.diagnostic_code});
  result.evidence.push_back({"support_bundle_api", "management.prepare_support_bundle"});
  result.evidence.push_back({"redaction_applied", "true"});
  result.evidence.push_back({"forbidden_fields_absent", "true"});
  result.evidence.push_back({"flush_required_before_export", "true"});
  result.evidence.push_back({"sidecar_authority", "false"});
  result.evidence.push_back({"parser_authority", "false"});
  result.evidence.push_back({"transaction_finality_authority", "false"});
  result.evidence.push_back({"visibility_authority", "false"});
  result.evidence.push_back({"recovery_authority", "false"});
  return result;
}

}  // namespace scratchbird::engine::internal_api

// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "security/deep_enforcement_api.hpp"

#include "security/security_model.hpp"

#include <utility>

namespace scratchbird::engine::internal_api {
namespace {

std::string RequestValueOr(const EngineApiRequest& request,
                           const std::string& prefix,
                           const std::string& fallback) {
  const auto value = SecurityOptionValue(request, prefix);
  return value.empty() ? fallback : value;
}

void AddDecisionRow(EngineEvaluateDeepSecurityResult* result,
                    const std::string& phase,
                    const std::string& right,
                    const std::string& decision,
                    const std::string& reason) {
  AddSecurityRow(result,
                 {{"phase", phase},
                  {"required_right", right},
                  {"decision", decision},
                  {"reason", reason},
                  {"admitted", result->admitted ? "true" : "false"},
                  {"authorized", result->authorized ? "true" : "false"},
                  {"visible", result->visible ? "true" : "false"},
                  {"masked", result->masked ? "true" : "false"},
                  {"rls_applied", result->rls_applied ? "true" : "false"},
                  {"audit_written", result->audit_written ? "true" : "false"},
                  {"side_effect_permitted", result->side_effect_permitted ? "true" : "false"}});
}

EngineEvaluateDeepSecurityResult Refuse(const EngineEvaluateDeepSecurityRequest& request,
                                        std::string code,
                                        std::string reason,
                                        bool hidden_as_missing = false) {
  EngineEvaluateDeepSecurityResult result =
      SecurityFailure<EngineEvaluateDeepSecurityResult>(request.context,
                                                        "security.evaluate_deep_enforcement",
                                                        MakeSecurityDiagnostic(std::move(code), reason));
  result.decision = hidden_as_missing ? "hidden_as_missing" : "refused";
  result.side_effect_permitted = false;
  AddDecisionRow(&result, request.phase, request.required_right, result.decision, reason);
  AddSecurityEvidence(&result, "security_deep_enforcement_refusal", reason);
  return result;
}

bool MutationPhase(const std::string& phase) {
  return phase == "storage" || phase == "mutation" || phase == "udr";
}

}  // namespace

EngineEvaluateDeepSecurityResult EngineEvaluateDeepSecurity(const EngineEvaluateDeepSecurityRequest& input) {
  EngineEvaluateDeepSecurityRequest request = input;
  request.operation_id = "security.evaluate_deep_enforcement";
  request.phase = RequestValueOr(request, "phase:", request.phase.empty() ? "executor" : request.phase);
  request.required_right = RequestValueOr(request, "required_right:", request.required_right.empty() ? "SELECT" : request.required_right);
  request.mutation = SecurityOptionBool(request, "mutation:", request.mutation);
  request.require_audit_before_success =
      SecurityOptionBool(request, "require_audit_before_success:", request.require_audit_before_success);

  if (!request.context.security_context_present) {
    return Refuse(request,
                  "SECURITY.AUTHENTICATION.REQUEST_INVALID",
                  "security_context_present=false");
  }
  if (!IsKnownSecurityRight(request.required_right)) {
    return Refuse(request,
                  "SECURITY.RIGHT.UNKNOWN",
                  request.required_right);
  }

  const std::string target_uuid = request.target_object.uuid.canonical;
  const bool authorized = SecurityContextHasRight(request.context, request.required_right, target_uuid);
  if (!authorized) {
    const bool discovery = request.phase == "catalog_discovery" || request.phase == "name_resolution";
    return Refuse(request,
                  discovery ? "SECURITY.OBJECT.NOT_FOUND_OR_NOT_VISIBLE" : "SECURITY.AUTHORIZATION.DENIED",
                  discovery ? "hidden_as_missing" : request.required_right,
                  discovery);
  }

  const std::string rls_policy = SecurityLower(RequestValueOr(request, "rls_policy:", "allow"));
  if (rls_policy == "deny") {
    return Refuse(request, "SECURITY.RLS.DENIED", "rls_policy=deny");
  }

  EngineEvaluateDeepSecurityResult result =
      SecuritySuccess<EngineEvaluateDeepSecurityResult>(request.context, request.operation_id);
  result.admitted = true;
  result.authorized = true;
  result.visible = true;
  result.rls_applied = rls_policy == "filter";
  result.decision = "admitted";
  result.side_effect_permitted = request.mutation || MutationPhase(request.phase);

  const std::string masking_policy = SecurityLower(RequestValueOr(request, "masking_policy:", "none"));
  if (masking_policy == "mask") {
    result.masked = true;
  } else if (masking_policy == "unmask") {
    result.masked = !SecurityContextHasRight(request.context, "UNMASK", target_uuid) &&
                    !SecurityContextHasRight(request.context, "DOMAIN_UNMASK", target_uuid);
  }

  if ((request.require_audit_before_success || result.side_effect_permitted) && request.mutation) {
    const auto audit = AppendSecurityEvidenceEvent(request.context,
                                                  request.operation_id,
                                                  "security_deep_enforcement",
                                                  request.phase + ":" + request.required_right);
    if (audit.error) {
      return Refuse(request, audit.code, audit.detail);
    }
    result.audit_written = true;
    AddSecurityEvidence(&result, "audit_before_success", request.phase + ":" + request.required_right);
  }

  AddSecurityEvidence(&result, "security_deep_enforcement", request.phase + ":" + request.required_right);
  if (result.masked) { AddSecurityEvidence(&result, "masking_applied", target_uuid); }
  if (result.rls_applied) { AddSecurityEvidence(&result, "rls_filter_applied", target_uuid); }
  AddDecisionRow(&result, request.phase, request.required_right, result.decision, "authorized");
  return result;
}

}  // namespace scratchbird::engine::internal_api

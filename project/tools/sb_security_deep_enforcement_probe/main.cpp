// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "security/deep_enforcement_api.hpp"

#include <filesystem>
#include <iostream>

using namespace scratchbird::engine::internal_api;

namespace {

EngineRequestContext Context(std::initializer_list<const char*> tags) {
  EngineRequestContext context;
  context.security_context_present = true;
  context.database_path = "/tmp/sb_security_deep_enforcement_probe.sbdb";
  context.local_transaction_id = 41;
  context.principal_uuid.canonical = "018f0000-0000-7000-8000-00000000d001";
  context.session_uuid.canonical = "018f0000-0000-7000-8000-00000000d002";
  for (const char* tag : tags) { context.trace_tags.emplace_back(tag); }
  return context;
}

EngineEvaluateDeepSecurityRequest Request(EngineRequestContext context,
                                          std::string right,
                                          std::string phase) {
  EngineEvaluateDeepSecurityRequest request;
  request.context = std::move(context);
  request.required_right = std::move(right);
  request.phase = std::move(phase);
  request.target_object.uuid.canonical = "018f0000-0000-7000-8000-00000000d100";
  request.target_object.object_kind = "table";
  return request;
}

bool HasDiagnostic(const EngineApiResult& result, const std::string& code) {
  for (const auto& diagnostic : result.diagnostics) {
    if (diagnostic.code == code) { return true; }
  }
  return false;
}

}  // namespace

int main() {
  std::filesystem::remove("/tmp/sb_security_deep_enforcement_probe.sbdb");

  auto allow = Request(Context({"right:SELECT"}), "SELECT", "executor");
  const auto allow_result = EngineEvaluateDeepSecurity(allow);

  auto missing_context = Request({}, "SELECT", "executor");
  const auto missing_context_result = EngineEvaluateDeepSecurity(missing_context);

  auto denied = Request(Context({}), "UPDATE", "mutation");
  denied.mutation = true;
  denied.require_audit_before_success = true;
  const auto denied_result = EngineEvaluateDeepSecurity(denied);

  auto discovery = Request(Context({}), "DISCOVER", "catalog_discovery");
  const auto discovery_result = EngineEvaluateDeepSecurity(discovery);

  auto mask = Request(Context({"right:SELECT"}), "SELECT", "policy_masking_rls");
  mask.option_envelopes.push_back("masking_policy:mask");
  const auto mask_result = EngineEvaluateDeepSecurity(mask);

  auto unmask = Request(Context({"right:SELECT", "right:UNMASK"}), "SELECT", "policy_masking_rls");
  unmask.option_envelopes.push_back("masking_policy:unmask");
  const auto unmask_result = EngineEvaluateDeepSecurity(unmask);

  auto rls_deny = Request(Context({"right:SELECT"}), "SELECT", "policy_masking_rls");
  rls_deny.option_envelopes.push_back("rls_policy:deny");
  const auto rls_deny_result = EngineEvaluateDeepSecurity(rls_deny);

  auto rls_filter = Request(Context({"right:SELECT"}), "SELECT", "policy_masking_rls");
  rls_filter.option_envelopes.push_back("rls_policy:filter");
  const auto rls_filter_result = EngineEvaluateDeepSecurity(rls_filter);

  auto audit = Request(Context({"right:INSERT"}), "INSERT", "mutation");
  audit.mutation = true;
  audit.require_audit_before_success = true;
  const auto audit_result = EngineEvaluateDeepSecurity(audit);

  auto udr = Request(Context({"right:UDR_INVOKE"}), "UDR_INVOKE", "udr");
  udr.mutation = true;
  const auto udr_result = EngineEvaluateDeepSecurity(udr);

  const bool ok = allow_result.ok && allow_result.admitted &&
                  !missing_context_result.ok &&
                  HasDiagnostic(missing_context_result, "SECURITY.AUTHENTICATION.REQUEST_INVALID") &&
                  !denied_result.ok && !denied_result.audit_written && !denied_result.side_effect_permitted &&
                  !discovery_result.ok && HasDiagnostic(discovery_result, "SECURITY.OBJECT.NOT_FOUND_OR_NOT_VISIBLE") &&
                  mask_result.ok && mask_result.masked &&
                  unmask_result.ok && !unmask_result.masked &&
                  !rls_deny_result.ok && HasDiagnostic(rls_deny_result, "SECURITY.RLS.DENIED") &&
                  rls_filter_result.ok && rls_filter_result.rls_applied &&
                  audit_result.ok && audit_result.audit_written && audit_result.side_effect_permitted &&
                  udr_result.ok && udr_result.side_effect_permitted;

  std::cout << "{\"ok\":" << (ok ? "true" : "false")
            << ",\"allow\":" << (allow_result.ok ? "true" : "false")
            << ",\"missing_context_denied\":" << (!missing_context_result.ok ? "true" : "false")
            << ",\"denied_no_audit\":" << (!denied_result.audit_written ? "true" : "false")
            << ",\"discovery_hidden\":" << (!discovery_result.ok ? "true" : "false")
            << ",\"mask_applied\":" << (mask_result.masked ? "true" : "false")
            << ",\"rls_filter\":" << (rls_filter_result.rls_applied ? "true" : "false")
            << ",\"audit_before_success\":" << (audit_result.audit_written ? "true" : "false")
            << ",\"udr_admitted\":" << (udr_result.ok ? "true" : "false") << "}\n";
  return ok ? 0 : 1;
}

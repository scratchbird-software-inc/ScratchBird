// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "security/plugin_trust_api.hpp"

#include "security/security_model.hpp"

namespace scratchbird::engine::internal_api {

// SEARCH_KEY: SB_ENGINE_INTERNAL_API_SECURITY_PLUGIN_TRUST_API_BEHAVIOR
EngineEvaluateUdrTrustResult EngineEvaluateUdrTrust(const EngineEvaluateUdrTrustRequest& request) {
  const bool signature_valid = SecurityOptionBool(request, "signature_valid:", false);
  const bool provenance_valid = SecurityOptionBool(request, "provenance_valid:", false);
  if (!signature_valid || !provenance_valid || !SecurityContextHasRight(request.context, "UDR_TRUST_ADMIN")) {
    return SecurityFailure<EngineEvaluateUdrTrustResult>(
        request.context,
        "security.evaluate_udr_trust",
        MakeSecurityDiagnostic("SECURITY.UDR.TRUST_DENIED", "signature_or_provenance_or_admin_denied"));
  }
  auto result = SecuritySuccess<EngineEvaluateUdrTrustResult>(request.context, "security.evaluate_udr_trust");
  result.admitted = true;
  AddSecurityEvidence(&result, "udr_trust", "admitted");
  AddSecurityRow(&result, {{"admitted", "true"}, {"signature_valid", "true"}, {"provenance_valid", "true"}});
  return result;
}

EngineEvaluateManagerAdmissionResult EngineEvaluateManagerAdmission(
    const EngineEvaluateManagerAdmissionRequest& request) {
  const bool identity_valid = SecurityOptionBool(request, "identity_valid:", false);
  const bool key_valid = SecurityOptionBool(request, "key_valid:", false);
  const bool expected_member = SecurityOptionBool(request, "expected_member:", false);
  if (!identity_valid || !key_valid || !expected_member ||
      !SecurityContextHasRight(request.context, "MANAGER_ADMISSION_ADMIN")) {
    return SecurityFailure<EngineEvaluateManagerAdmissionResult>(
        request.context,
        "security.evaluate_manager_admission",
        MakeSecurityDiagnostic("SECURITY.MANAGER.ADMISSION_DENIED", "manager_admission_denied"));
  }
  auto result = SecuritySuccess<EngineEvaluateManagerAdmissionResult>(request.context, "security.evaluate_manager_admission");
  result.admitted = true;
  AddSecurityEvidence(&result, "manager_admission", "admitted");
  AddSecurityRow(&result, {{"admitted", "true"}, {"identity_valid", "true"}, {"key_valid", "true"}});
  return result;
}

}  // namespace scratchbird::engine::internal_api

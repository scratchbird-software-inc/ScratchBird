// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "cloud/cloud_identity_kms.hpp"

#include <cassert>
#include <string>
#include <vector>

namespace api = scratchbird::engine::internal_api;

namespace {

api::EngineApiRequest BaseRequest() {
  api::EngineApiRequest request;
  request.operation_id = "cloud.identity_kms.test";
  request.context.security_context_present = true;
  request.context.database_uuid.canonical = "db-test";
  request.context.principal_uuid.canonical = "principal-test";
  request.option_envelopes = {
      "provider_profile_uuid:provider-1",
      "external_subject_ref:spiffe://tenant/ns/default/sa/scratchbird",
      "internal_subject_uuid:subject-1",
      "identity_evidence:verified",
      "assertion_signature_valid:true",
      "assertion_expiry_ms:4102444800000",
      "evidence_observed_ms:1780000000000",
      "kms_profile_uuid:kms-profile-1",
      "kms_mode:cloud_kms",
      "kms_key_reference:projects/example/locations/local/keyRings/test/cryptoKeys/main",
      "protected_material_uuid:pm-1",
      "protected_material_version_uuid:pmv-1",
      "rotation_policy_uuid:rotation-1",
      "audit_policy_uuid:audit-1",
      "kms_version_current:7",
      "kms_version_observed:7",
  };
  return request;
}

void Add(api::EngineApiRequest* request, std::string option) {
  request->option_envelopes.push_back(std::move(option));
}

bool RowValueEquals(const api::EngineApiResult& result,
                    const std::string& field_name,
                    const std::string& expected) {
  for (const auto& row : result.result_shape.rows) {
    for (const auto& field : row.fields) {
      if (field.first == field_name && field.second.encoded_value == expected) { return true; }
    }
  }
  return false;
}

bool AnyReturnedValueContains(const api::EngineApiResult& result, const std::string& needle) {
  for (const auto& row : result.result_shape.rows) {
    for (const auto& field : row.fields) {
      if (field.second.encoded_value.find(needle) != std::string::npos) { return true; }
    }
  }
  for (const auto& evidence : result.evidence) {
    if (evidence.evidence_id.find(needle) != std::string::npos) { return true; }
  }
  for (const auto& diagnostic : result.diagnostics) {
    if (diagnostic.detail.find(needle) != std::string::npos) { return true; }
  }
  return false;
}

void WorkloadIdentityCloudKmsProducesRedactedEnvelope() {
  auto request = BaseRequest();
  Add(&request, "identity_mode:workload_identity");
  Add(&request, "workload_trust_ref:trust-bundle-1");
  const auto result = api::ValidateCloudIdentityKmsPolicyApi(request);
  assert(result.ok);
  assert(RowValueEquals(result, "identity_mode", "workload_identity"));
  assert(RowValueEquals(result, "kms_mode", "cloud_kms"));
  assert(RowValueEquals(result, "plaintext_material_persisted", "false"));
  assert(RowValueEquals(result, "plaintext_material_returned", "false"));
  assert(RowValueEquals(result, "transaction_finality_authority", "scratchbird_mga_not_kms"));
  assert(!AnyReturnedValueContains(result, "projects/example/locations/local"));
}

void StaticSecretFailsClosedWithoutPolicy() {
  auto request = BaseRequest();
  Add(&request, "identity_mode:static_secret");
  const auto result = api::ValidateCloudIdentityKmsPolicyApi(request);
  assert(!result.ok);
  assert(!result.diagnostics.empty());
  assert(result.diagnostics.front().code == "SB_DIAG_CLOUD_STATIC_SECRET_FORBIDDEN");
  assert(!result.evidence.empty());
}

void StaticSecretAllowedOnlyAsProtectedAuditedReference() {
  auto request = BaseRequest();
  Add(&request, "identity_mode:static_secret");
  Add(&request, "static_secret_explicitly_allowed:true");
  Add(&request, "static_secret_break_glass:true");
  Add(&request, "static_secret_policy_uuid:static-policy-1");
  Add(&request, "static_secret_audit_evidence_uuid:static-audit-1");
  Add(&request, "static_secret_rotation_policy_uuid:static-rotation-1");
  Add(&request, "static_secret_protected_material_version_uuid:pmv-static-1");
  const auto result = api::ValidateCloudIdentityKmsPolicyApi(request);
  assert(result.ok);
  assert(RowValueEquals(result, "static_secret_policy_exception", "true"));
  assert(RowValueEquals(result, "plaintext_material_persisted", "false"));
  assert(!AnyReturnedValueContains(result, "secret-value"));
}

void PlaintextMaterialIsRejected() {
  auto request = BaseRequest();
  Add(&request, "identity_mode:oidc_federation");
  Add(&request, "oidc_issuer_ref:issuer-1");
  Add(&request, "oidc_audience_ref:audience-1");
  Add(&request, "secret_value:do-not-store");
  const auto result = api::ValidateCloudIdentityKmsPolicyApi(request);
  assert(!result.ok);
  assert(!result.diagnostics.empty());
  assert(result.diagnostics.front().code == "SB_DIAG_CLOUD_PLAINTEXT_MATERIAL_FORBIDDEN");
}

void StaleKmsVersionCannotAuthorizeEnvelope() {
  auto request = BaseRequest();
  Add(&request, "identity_mode:iam_role");
  Add(&request, "iam_role_ref:role-ref-1");
  for (auto& option : request.option_envelopes) {
    if (option == "kms_version_observed:7") { option = "kms_version_observed:6"; }
  }
  const auto result = api::ValidateCloudIdentityKmsPolicyApi(request);
  assert(!result.ok);
  assert(!result.diagnostics.empty());
  assert(result.diagnostics.front().code == "SB_DIAG_CLOUD_KMS_VERSION_STALE");
}

void LocalEmulatorFixtureDoesNotRequireExternalKms() {
  api::EngineApiRequest request;
  request.operation_id = "cloud.identity_kms.emulator_test";
  request.option_envelopes = {
      "identity_mode:local_emulator",
      "identity_emulator_evidence:verified",
      "local_emulator_fixture:true",
      "kms_profile_uuid:kms-emulator-profile",
      "kms_mode:local_emulator",
      "emulator_key_ref:fixture-key-1",
      "protected_material_uuid:pm-emulator",
      "protected_material_version_uuid:pmv-emulator",
      "rotation_policy_uuid:rotation-emulator",
      "audit_policy_uuid:audit-emulator",
      "kms_emulator_evidence:verified",
  };
  const auto result = api::ValidateCloudIdentityKmsPolicyApi(request);
  assert(result.ok);
  assert(RowValueEquals(result, "local_emulator_fixture", "true"));
  assert(RowValueEquals(result, "external_kms_dependency", "not_required"));
}

}  // namespace

int main() {
  WorkloadIdentityCloudKmsProducesRedactedEnvelope();
  StaticSecretFailsClosedWithoutPolicy();
  StaticSecretAllowedOnlyAsProtectedAuditedReference();
  PlaintextMaterialIsRejected();
  StaleKmsVersionCannotAuthorizeEnvelope();
  LocalEmulatorFixtureDoesNotRequireExternalKms();
  return 0;
}

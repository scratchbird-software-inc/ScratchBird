#include "../support/binary_uuid_fixture.hpp"
#include "../support/engine_evidence_fixture.hpp"
// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "cloud/cloud_identity_kms.hpp"

#include <cassert>
#include <array>
#include <string>
#include <vector>

namespace api = scratchbird::engine::internal_api;

namespace {

std::string UuidOption(std::string_view key, std::uint32_t ordinal) {
  const auto uuid = scratchbird::tests::FixtureUuid(1208, ordinal);
  std::string result(key);
  result.append(reinterpret_cast<const char*>(uuid.bytes.data()), uuid.bytes.size());
  return result;
}

api::EngineApiRequest BaseRequest() {
  api::EngineApiRequest request;
  request.operation_id = "cloud.identity_kms.test";
  request.context.security_context_present = true;
  request.context.database_uuid = scratchbird::tests::FixtureUuid(1208, 801);
  request.context.principal_uuid = scratchbird::tests::FixtureUuid(1208, 802);
  request.option_envelopes = {
      UuidOption("provider_profile_uuid:", 1),
      "external_subject_ref:spiffe://tenant/ns/default/sa/scratchbird",
      UuidOption("internal_subject_uuid:", 2),
      "identity_evidence:verified",
      "assertion_signature_valid:true",
      "assertion_expiry_ms:4102444800000",
      "evidence_observed_ms:1780000000000",
      UuidOption("kms_profile_uuid:", 3),
      "kms_mode:cloud_kms",
      "kms_key_reference:projects/example/locations/local/keyRings/test/cryptoKeys/main",
      UuidOption("protected_material_uuid:", 4),
      UuidOption("protected_material_version_uuid:", 5),
      UuidOption("rotation_policy_uuid:", 6),
      UuidOption("audit_policy_uuid:", 7),
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
    if (scratchbird::tests::EvidenceTextFind(evidence.evidence_id, needle) != std::string::npos) { return true; }
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
  Add(&request, UuidOption("static_secret_policy_uuid:", 8));
  Add(&request, UuidOption("static_secret_audit_evidence_uuid:", 9));
  Add(&request, UuidOption("static_secret_rotation_policy_uuid:", 10));
  Add(&request, UuidOption("static_secret_protected_material_version_uuid:", 11));
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
      UuidOption("kms_profile_uuid:", 12),
      "kms_mode:local_emulator",
      "emulator_key_ref:fixture-key-1",
      UuidOption("protected_material_uuid:", 13),
      UuidOption("protected_material_version_uuid:", 14),
      UuidOption("rotation_policy_uuid:", 15),
      UuidOption("audit_policy_uuid:", 16),
      "kms_emulator_evidence:verified",
  };
  const auto result = api::ValidateCloudIdentityKmsPolicyApi(request);
  assert(result.ok);
  assert(RowValueEquals(result, "local_emulator_fixture", "true"));
  assert(RowValueEquals(result, "external_kms_dependency", "not_required"));
}

void RequiredIdentitiesRemainBinaryAndUnambiguous() {
  const std::array<std::string_view, 7> keys = {
      "provider_profile_uuid:", "internal_subject_uuid:", "kms_profile_uuid:",
      "protected_material_uuid:", "protected_material_version_uuid:",
      "rotation_policy_uuid:", "audit_policy_uuid:"};
  for (const auto key : keys) {
    auto valid = BaseRequest();
    Add(&valid, "identity_mode:workload_identity");
    Add(&valid, "workload_trust_ref:trust-bundle-1");
    assert(api::ValidateCloudIdentityKmsPolicyApi(valid).ok);
    for (unsigned mutation = 0; mutation < 10; ++mutation) {
      auto bad = valid;
      for (auto& option : bad.option_envelopes) {
        if (!option.starts_with(key)) continue;
        auto value = option.substr(key.size());
        switch (mutation) {
          case 0: value = "019d0000-04b8-7000-8000-000000000001"; break;
          case 1: value.assign(16, '\0'); break;
          case 2: value[6] = 0x40; break;
          case 3: value[8] = 0; break;
          case 4: value.pop_back(); break;
          case 5: value.push_back('\0'); break;
          case 6: value.clear(); break;
          default: break;
        }
        option = std::string(key) + value;
      }
      if (mutation >= 7) {
        // Both identical and conflicting duplicate fields are ambiguous,
        // including a malformed duplicate after an otherwise valid value.
        Add(&bad, mutation == 9 ? std::string(key) + "text-alias" :
            UuidOption(key, 99));
        if (mutation == 7) {
          for (const auto& option : valid.option_envelopes) {
            if (option.starts_with(key)) bad.option_envelopes.back() = option;
          }
        }
      }
      const auto result = api::ValidateCloudIdentityKmsPolicyApi(bad);
      assert(!result.ok);
      assert(result.primary_object.uuid.is_nil());
      assert(!result.diagnostics.empty());
      assert(result.diagnostics.front().code == "SB_DIAG_CLOUD_IDENTITY_MAPPING_MISSING");
      assert(result.diagnostics.front().detail ==
          (mutation >= 7 ? "duplicate_uuid_option" : "binary_uuid_option_required"));
      assert(RowValueEquals(result, "decision", "deny"));
    }
    const auto admitted = api::ValidateCloudIdentityKmsPolicyApi(valid);
    assert(admitted.ok);
    for (const auto& row : admitted.result_shape.rows) {
      for (const auto& field : row.fields) {
        if (!field.first.ends_with("_uuid")) continue;
        const auto& bytes = field.second.binary_value;
        assert(field.second.descriptor.canonical_type_name == "uuid");
        assert(field.second.encoded_value.empty());
        assert(bytes.size() == 16);
        assert((static_cast<unsigned char>(bytes[6]) & 0xf0) == 0x70);
        assert((static_cast<unsigned char>(bytes[8]) & 0xc0) == 0x80);
      }
    }
  }
}

}  // namespace

int main() {
  WorkloadIdentityCloudKmsProducesRedactedEnvelope();
  StaticSecretFailsClosedWithoutPolicy();
  StaticSecretAllowedOnlyAsProtectedAuditedReference();
  PlaintextMaterialIsRejected();
  StaleKmsVersionCannotAuthorizeEnvelope();
  LocalEmulatorFixtureDoesNotRequireExternalKms();
  RequiredIdentitiesRemainBinaryAndUnambiguous();
  return 0;
}

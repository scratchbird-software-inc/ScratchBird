// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "cloud/cloud_identity_kms.hpp"

#include "api_diagnostics.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <iomanip>
#include <sstream>
#include <string_view>

namespace scratchbird::engine::internal_api {
namespace {

constexpr std::array<const char*, 18> kPlaintextPrefixes = {{
    "plaintext:",
    "plaintext_secret:",
    "secret:",
    "secret_value:",
    "static_secret_value:",
    "password:",
    "token_value:",
    "access_token:",
    "refresh_token:",
    "raw_token:",
    "private_key:",
    "raw_key:",
    "key_material:",
    "hsm_key_material:",
    "data_key_plaintext:",
    "wrapped_key_plaintext:",
    "credential_plaintext:",
    "provider_secret:",
}};

bool StartsWith(const std::string& value, const std::string& prefix) {
  return value.rfind(prefix, 0) == 0;
}

std::string Lower(std::string value) {
  for (char& c : value) { c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); }
  return value;
}

std::string OptionValue(const EngineApiRequest& request, const std::string& prefix) {
  for (const auto& option : request.option_envelopes) {
    if (StartsWith(option, prefix)) { return option.substr(prefix.size()); }
  }
  return {};
}

bool OptionPresent(const EngineApiRequest& request, const std::string& exact_value) {
  for (const auto& option : request.option_envelopes) {
    if (option == exact_value) { return true; }
  }
  return false;
}

bool OptionBool(const EngineApiRequest& request, const std::string& prefix, bool fallback = false) {
  const std::string value = Lower(OptionValue(request, prefix));
  if (value.empty()) { return fallback; }
  if (value == "1" || value == "true" || value == "yes" || value == "on") { return true; }
  if (value == "0" || value == "false" || value == "no" || value == "off") { return false; }
  return fallback;
}

bool ParseU64(const std::string& value, std::uint64_t* out) {
  if (!out || value.empty()) { return false; }
  std::uint64_t parsed = 0;
  for (const char c : value) {
    if (c < '0' || c > '9') { return false; }
    const std::uint64_t next = (parsed * 10u) + static_cast<std::uint64_t>(c - '0');
    if (next < parsed) { return false; }
    parsed = next;
  }
  *out = parsed;
  return true;
}

std::uint64_t Fnv1a(std::string_view value) {
  std::uint64_t hash = 1469598103934665603ull;
  for (const unsigned char c : value) {
    hash ^= static_cast<std::uint64_t>(c);
    hash *= 1099511628211ull;
  }
  return hash;
}

std::string Hex64(const std::uint64_t value) {
  std::ostringstream out;
  out << std::hex << std::setfill('0') << std::setw(16) << value;
  return out.str();
}

std::string SyntheticUuid(const std::string& kind, const std::string& seed) {
  const std::string hex = Hex64(Fnv1a(kind + ":a:" + seed)) + Hex64(Fnv1a(kind + ":b:" + seed));
  return hex.substr(0, 8) + "-" + hex.substr(8, 4) + "-" + hex.substr(12, 4) + "-" +
         hex.substr(16, 4) + "-" + hex.substr(20, 12);
}

std::string RedactedReference(const std::string& reference) {
  if (reference.empty()) { return {}; }
  return "redacted-ref:" + Hex64(Fnv1a(reference)).substr(0, 16);
}

std::string FirstPresentValue(const EngineApiRequest& request, std::initializer_list<const char*> prefixes) {
  for (const char* prefix : prefixes) {
    std::string value = OptionValue(request, prefix);
    if (!value.empty()) { return value; }
  }
  return {};
}

CloudIdentityKmsValidation Fail(const EngineApiRequest& request,
                                std::string code,
                                std::string detail,
                                bool add_denial_audit = true) {
  CloudIdentityKmsValidation validation;
  validation.ok = false;
  validation.identity_mode = CanonicalCloudIdentityMode(OptionValue(request, "identity_mode:"));
  validation.kms_mode = CanonicalCloudKmsMode(OptionValue(request, "kms_mode:"));
  validation.diagnostic = MakeEngineApiDiagnostic(std::move(code), "cloud.identity_kms", std::move(detail), true);
  validation.rows.push_back({"decision", "deny"});
  validation.rows.push_back({"plaintext_material_persisted", "false"});
  validation.rows.push_back({"plaintext_material_returned", "false"});
  if (add_denial_audit) {
    std::string audit = FirstPresentValue(request, {"audit_evidence_uuid:", "static_secret_audit_evidence_uuid:", "audit_policy_uuid:"});
    if (audit.empty()) { audit = SyntheticUuid("cloud_identity_kms_audit", validation.diagnostic.code + ":" + validation.diagnostic.detail); }
    validation.evidence.push_back({"cloud_identity_kms_denial_audit", audit});
  }
  return validation;
}

bool ContainsPlaintextOption(const EngineApiRequest& request, std::string* offending_prefix) {
  for (const auto& option : request.option_envelopes) {
    const std::string lowered = Lower(option);
    for (const char* prefix : kPlaintextPrefixes) {
      if (StartsWith(lowered, prefix)) {
        if (offending_prefix) { *offending_prefix = prefix; }
        return true;
      }
    }
  }
  return false;
}

bool ModeEvidenceVerified(const EngineApiRequest& request, const std::string& mode) {
  return OptionPresent(request, "identity_evidence:verified") ||
         OptionPresent(request, mode + "_evidence:verified") ||
         OptionBool(request, "identity_evidence_verified:", false) ||
         OptionBool(request, mode + "_evidence_verified:", false) ||
         OptionBool(request, "assertion_signature_valid:", false) ||
         OptionBool(request, "signature_valid:", false);
}

bool AssertionFresh(const EngineApiRequest& request, const std::string& mode, std::string* detail) {
  std::string expiry_text = FirstPresentValue(request, {
      "assertion_expiry_ms:",
      "expires_at_ms:",
      "token_expiry_ms:",
      "session_expiry_ms:",
  });
  if (expiry_text.empty()) {
    expiry_text = OptionValue(request, mode + "_expiry_ms:");
  }
  std::uint64_t expiry = 0;
  if (!ParseU64(expiry_text, &expiry) || expiry == 0) {
    if (detail) { *detail = "identity_assertion_expiry_required"; }
    return false;
  }
  std::uint64_t observed = 0;
  const std::string observed_text = FirstPresentValue(request, {"evidence_observed_ms:", "observed_at_ms:", "now_ms:"});
  if (!ParseU64(observed_text, &observed) || observed == 0) {
    if (detail) { *detail = "identity_evidence_observed_time_required"; }
    return false;
  }
  if (expiry <= observed) {
    if (detail) { *detail = "identity_assertion_expired"; }
    return false;
  }
  return true;
}

CloudIdentityKmsValidation ValidateStaticSecretException(const EngineApiRequest& request) {
  if (!OptionBool(request, "static_secret_explicitly_allowed:", false)) {
    return Fail(request, "SB_DIAG_CLOUD_STATIC_SECRET_FORBIDDEN", "static_secret_policy_exception_required");
  }
  if (!OptionBool(request, "static_secret_break_glass:", false) &&
      !OptionBool(request, "legacy_static_secret_policy:", false)) {
    return Fail(request, "SB_DIAG_CLOUD_STATIC_SECRET_FORBIDDEN", "static_secret_break_glass_or_legacy_scope_required");
  }
  if (OptionValue(request, "static_secret_policy_uuid:").empty()) {
    return Fail(request, "SB_DIAG_CLOUD_STATIC_SECRET_FORBIDDEN", "static_secret_policy_uuid_required");
  }
  if (OptionValue(request, "static_secret_audit_evidence_uuid:").empty()) {
    return Fail(request, "SB_DIAG_CLOUD_STATIC_SECRET_FORBIDDEN", "static_secret_audit_evidence_required");
  }
  if (OptionValue(request, "static_secret_rotation_policy_uuid:").empty()) {
    return Fail(request, "SB_DIAG_CLOUD_STATIC_SECRET_FORBIDDEN", "static_secret_rotation_policy_required");
  }
  if (FirstPresentValue(request, {"static_secret_protected_material_version_uuid:", "protected_material_version_uuid:"}).empty()) {
    return Fail(request, "SB_DIAG_CLOUD_STATIC_SECRET_FORBIDDEN", "static_secret_protected_material_reference_required");
  }
  CloudIdentityKmsValidation ok;
  ok.ok = true;
  return ok;
}

CloudIdentityKmsValidation ValidateSecretlessIdentity(const EngineApiRequest& request, const std::string& mode) {
  if (mode == "local_emulator_identity") {
    if (!OptionBool(request, "local_emulator_fixture:", false)) {
      return Fail(request, "SB_DIAG_CLOUD_IDENTITY_MODE_UNSUPPORTED", "local_emulator_fixture_policy_required");
    }
    if (!OptionPresent(request, "identity_emulator_evidence:verified") &&
        !OptionBool(request, "identity_emulator_evidence_verified:", false)) {
      return Fail(request, "SB_DIAG_CLOUD_IDENTITY_TOKEN_INVALID", "identity_emulator_evidence_required");
    }
    CloudIdentityKmsValidation ok;
    ok.ok = true;
    return ok;
  }

  if (OptionValue(request, "provider_profile_uuid:").empty()) {
    return Fail(request, "SB_DIAG_CLOUD_IDENTITY_MAPPING_MISSING", "provider_profile_uuid_required");
  }
  if (FirstPresentValue(request, {"external_subject_ref:", "subject_ref:", "provider_subject_ref:"}).empty()) {
    return Fail(request, "SB_DIAG_CLOUD_IDENTITY_MAPPING_MISSING", "external_subject_ref_required");
  }
  if (OptionValue(request, "internal_subject_uuid:").empty()) {
    return Fail(request, "SB_DIAG_CLOUD_IDENTITY_MAPPING_MISSING", "internal_subject_uuid_required");
  }
  if (!ModeEvidenceVerified(request, mode)) {
    return Fail(request, "SB_DIAG_CLOUD_IDENTITY_TOKEN_INVALID", "identity_evidence_verification_required");
  }
  std::string freshness_detail;
  if (!AssertionFresh(request, mode, &freshness_detail)) {
    return Fail(request, "SB_DIAG_CLOUD_IDENTITY_TOKEN_INVALID", freshness_detail);
  }

  if (mode == "workload_identity" &&
      FirstPresentValue(request, {"workload_trust_ref:", "trust_bundle_ref:", "spiffe_trust_domain_ref:"}).empty()) {
    return Fail(request, "SB_DIAG_CLOUD_IDENTITY_TOKEN_INVALID", "workload_trust_ref_required");
  }
  if (mode == "oidc_federation" &&
      (OptionValue(request, "oidc_issuer_ref:").empty() || OptionValue(request, "oidc_audience_ref:").empty())) {
    return Fail(request, "SB_DIAG_CLOUD_IDENTITY_TOKEN_INVALID", "oidc_issuer_and_audience_required");
  }
  if (mode == "managed_identity" &&
      FirstPresentValue(request, {"managed_identity_resource_ref:", "identity_resource_ref:"}).empty()) {
    return Fail(request, "SB_DIAG_CLOUD_IDENTITY_TOKEN_INVALID", "managed_identity_resource_ref_required");
  }
  if (mode == "iam_role" && FirstPresentValue(request, {"iam_role_ref:", "role_arn_ref:"}).empty()) {
    return Fail(request, "SB_DIAG_CLOUD_IDENTITY_TOKEN_INVALID", "iam_role_ref_required");
  }
  if (mode == "service_account_token" &&
      (OptionValue(request, "service_account_ref:").empty() || OptionValue(request, "token_audience_ref:").empty())) {
    return Fail(request, "SB_DIAG_CLOUD_IDENTITY_TOKEN_INVALID", "service_account_ref_and_audience_required");
  }

  CloudIdentityKmsValidation ok;
  ok.ok = true;
  return ok;
}

CloudIdentityKmsValidation ValidateKmsPolicy(const EngineApiRequest& request, const std::string& kms_mode) {
  if (kms_mode.empty()) {
    return Fail(request, "SB_DIAG_CLOUD_KMS_PROFILE_INVALID", "kms_mode_required");
  }
  if (kms_mode == "unsupported") {
    return Fail(request, "SB_DIAG_CLOUD_KMS_PROFILE_INVALID", "kms_mode_unsupported");
  }
  if (OptionValue(request, "kms_profile_uuid:").empty()) {
    return Fail(request, "SB_DIAG_CLOUD_KMS_PROFILE_INVALID", "kms_profile_uuid_required");
  }
  if (OptionValue(request, "rotation_policy_uuid:").empty()) {
    return Fail(request, "SB_DIAG_CLOUD_KEY_ROTATION_BLOCKED", "rotation_policy_uuid_required");
  }
  if (OptionValue(request, "audit_policy_uuid:").empty()) {
    return Fail(request, "SB_DIAG_CLOUD_KMS_PROFILE_INVALID", "audit_policy_uuid_required");
  }
  if (!OptionBool(request, "envelope_encryption_flag:", true)) {
    return Fail(request, "SB_DIAG_CLOUD_KMS_PROFILE_INVALID", "envelope_encryption_required");
  }
  if (OptionBool(request, "external_kms_dependency_required:", false)) {
    return Fail(request, "SB_DIAG_CLOUD_KMS_PROFILE_INVALID", "runtime_external_kms_dependency_forbidden");
  }
  if (OptionBool(request, "kms_outage:", false)) {
    return Fail(request, "SB_DIAG_CLOUD_KMS_UNAVAILABLE", "kms_outage_fail_closed");
  }

  const std::string current_generation = FirstPresentValue(request, {"kms_version_current:", "key_version_current:"});
  const std::string observed_generation = FirstPresentValue(request, {"kms_version_observed:", "key_version_observed:"});
  if (!current_generation.empty() && !observed_generation.empty() && current_generation != observed_generation) {
    return Fail(request, "SB_DIAG_CLOUD_KMS_VERSION_STALE", "kms_version_observed_is_stale");
  }

  if (kms_mode == "local_emulator" &&
      (!OptionBool(request, "local_emulator_fixture:", false) ||
       (!OptionPresent(request, "kms_emulator_evidence:verified") &&
        !OptionBool(request, "kms_emulator_evidence_verified:", false)))) {
    return Fail(request, "SB_DIAG_CLOUD_KMS_PROFILE_INVALID", "local_kms_emulator_evidence_required");
  }
  if (kms_mode == "manual_recovery_key" && OptionValue(request, "manual_recovery_approval_uuid:").empty()) {
    return Fail(request, "SB_DIAG_CLOUD_KMS_PROFILE_INVALID", "manual_recovery_approval_uuid_required");
  }

  if (FirstPresentValue(request, {
          "key_reference:",
          "kms_key_reference:",
          "hsm_key_reference:",
          "kmip_key_reference:",
          "local_key_reference:",
          "manual_recovery_key_ref:",
          "emulator_key_ref:",
      }).empty()) {
    return Fail(request, "SB_DIAG_CLOUD_KMS_PROFILE_INVALID", "key_reference_required");
  }
  if (FirstPresentValue(request, {"protected_material_uuid:", "kms_protected_material_uuid:"}).empty()) {
    return Fail(request, "SB_DIAG_CLOUD_KMS_PROFILE_INVALID", "protected_material_uuid_required");
  }
  if (FirstPresentValue(request, {"protected_material_version_uuid:", "kms_protected_material_version_uuid:"}).empty()) {
    return Fail(request, "SB_DIAG_CLOUD_KMS_PROFILE_INVALID", "protected_material_version_uuid_required");
  }

  CloudIdentityKmsValidation ok;
  ok.ok = true;
  return ok;
}

CloudProtectedReference MakeIdentityReference(const EngineApiRequest& request, const std::string& mode) {
  CloudProtectedReference ref;
  const std::string subject = FirstPresentValue(request, {"external_subject_ref:", "subject_ref:", "provider_subject_ref:"});
  const std::string version = FirstPresentValue(request, {"identity_protected_material_version_uuid:", "static_secret_protected_material_version_uuid:", "protected_material_version_uuid:"});
  ref.reference_kind = mode == "static_secret" ? "static_secret_protected_reference" : "secretless_identity_binding";
  ref.provider_profile_uuid = OptionValue(request, "provider_profile_uuid:");
  ref.protected_material_uuid = FirstPresentValue(request, {"identity_protected_material_uuid:", "protected_material_uuid:"});
  ref.protected_material_version_uuid = version;
  ref.redacted_external_reference = RedactedReference(subject.empty() ? OptionValue(request, "static_secret_policy_uuid:") : subject);
  ref.reference_uuid.canonical = SyntheticUuid(ref.reference_kind, mode + ":" + ref.provider_profile_uuid + ":" +
                                                                  ref.protected_material_version_uuid + ":" +
                                                                  ref.redacted_external_reference);
  return ref;
}

CloudProtectedReference MakeKmsReference(const EngineApiRequest& request, const std::string& kms_mode) {
  const std::string key_ref = FirstPresentValue(request, {
      "key_reference:",
      "kms_key_reference:",
      "hsm_key_reference:",
      "kmip_key_reference:",
      "local_key_reference:",
      "manual_recovery_key_ref:",
      "emulator_key_ref:",
  });
  CloudProtectedReference ref;
  ref.reference_kind = "kms_wrapping_key_reference";
  ref.provider_profile_uuid = OptionValue(request, "provider_profile_uuid:");
  ref.protected_material_uuid = FirstPresentValue(request, {"protected_material_uuid:", "kms_protected_material_uuid:"});
  ref.protected_material_version_uuid = FirstPresentValue(request, {"protected_material_version_uuid:", "kms_protected_material_version_uuid:"});
  ref.redacted_external_reference = RedactedReference(kms_mode + ":" + key_ref);
  ref.reference_uuid.canonical = SyntheticUuid(ref.reference_kind, kms_mode + ":" + OptionValue(request, "kms_profile_uuid:") +
                                                                  ":" + ref.protected_material_version_uuid + ":" +
                                                                  ref.redacted_external_reference);
  return ref;
}

CloudKmsEnvelopeMetadata MakeEnvelope(const EngineApiRequest& request,
                                      const std::string& kms_mode,
                                      const CloudProtectedReference& kms_reference) {
  CloudKmsEnvelopeMetadata envelope;
  envelope.kms_profile_uuid = OptionValue(request, "kms_profile_uuid:");
  envelope.kms_mode = kms_mode;
  envelope.rotation_policy_uuid = OptionValue(request, "rotation_policy_uuid:");
  envelope.audit_policy_uuid = OptionValue(request, "audit_policy_uuid:");
  envelope.envelope_version = OptionValue(request, "envelope_version:");
  if (envelope.envelope_version.empty()) { envelope.envelope_version = "1"; }
  envelope.wrapping_reference_uuid = kms_reference.reference_uuid;
  envelope.envelope_uuid.canonical = SyntheticUuid("cloud_kms_envelope",
                                                   envelope.kms_profile_uuid + ":" +
                                                       kms_reference.protected_material_version_uuid + ":" +
                                                       envelope.envelope_version);
  return envelope;
}

EngineTypedValue RowValue(std::string value) {
  EngineTypedValue typed;
  typed.descriptor.descriptor_kind = "scalar";
  typed.descriptor.canonical_type_name = "text";
  typed.encoded_value = std::move(value);
  return typed;
}

void AddResultRow(EngineApiResult* result, std::vector<std::pair<std::string, std::string>> fields) {
  EngineRowValue row;
  row.requested_row_uuid.canonical = SyntheticUuid("cloud_identity_kms_row", std::to_string(result->result_shape.rows.size()));
  for (auto& field : fields) { row.fields.push_back({std::move(field.first), RowValue(std::move(field.second))}); }
  result->result_shape.result_kind = "cloud_identity_kms_policy_rows";
  result->result_shape.rows.push_back(std::move(row));
}

}  // namespace

std::string CanonicalCloudIdentityMode(std::string mode) {
  mode = Lower(std::move(mode));
  if (mode == "workload.identity" || mode == "kubernetes_workload_identity" ||
      mode == "spiffe" || mode == "spiffe_svid" || mode == "workload_spiffe_svid") {
    return "workload_identity";
  }
  if (mode == "oidc" || mode == "oidc.jwt" || mode == "oidc_jwt" ||
      mode == "web_identity" || mode == "federation.oidc" || mode == "federated_oidc") {
    return "oidc_federation";
  }
  if (mode == "managed.identity" || mode == "provider_managed_identity" ||
      mode == "azure_managed_identity" || mode == "instance_metadata_identity") {
    return "managed_identity";
  }
  if (mode == "iam.role" || mode == "aws_iam_role" || mode == "instance_profile_role") {
    return "iam_role";
  }
  if (mode == "service_account" || mode == "service_account.jwt" ||
      mode == "kubernetes_service_account_token") {
    return "service_account_token";
  }
  if (mode == "static.secret" || mode == "static_credential" || mode == "static_credentials" ||
      mode == "long_lived_secret") {
    return "static_secret";
  }
  if (mode == "local_emulator" || mode == "local_identity_emulator") {
    return "local_emulator_identity";
  }
  return mode;
}

std::string CanonicalCloudKmsMode(std::string mode) {
  mode = Lower(std::move(mode));
  if (mode == "cloud.kms" || mode == "provider_kms") { return "cloud_kms"; }
  if (mode == "managed.hsm" || mode == "provider_managed_hsm") { return "managed_hsm"; }
  if (mode == "external.hsm" || mode == "external_hardware_hsm") { return "external_hsm"; }
  if (mode == "kmip_server") { return "kmip"; }
  if (mode == "local.key_agent" || mode == "local_operator_key_agent") { return "local_key_agent"; }
  if (mode == "manual_key_entry" || mode == "manual.recovery_key") { return "manual_recovery_key"; }
  if (mode == "local_kms_emulator" || mode == "kms_emulator") { return "local_emulator"; }
  if (mode == "cloud_kms" || mode == "managed_hsm" || mode == "external_hsm" ||
      mode == "kmip" || mode == "local_key_agent" || mode == "manual_recovery_key" ||
      mode == "local_emulator") {
    return mode;
  }
  return mode.empty() ? mode : "unsupported";
}

bool CloudIdentityModeIsSecretless(const std::string& mode) {
  const std::string canonical = CanonicalCloudIdentityMode(mode);
  return canonical == "workload_identity" || canonical == "oidc_federation" ||
         canonical == "managed_identity" || canonical == "iam_role" ||
         canonical == "service_account_token" || canonical == "local_emulator_identity";
}

CloudIdentityKmsValidation ValidateCloudIdentityKmsPolicy(const EngineApiRequest& request) {
  std::string offending_prefix;
  if (ContainsPlaintextOption(request, &offending_prefix)) {
    return Fail(request, "SB_DIAG_CLOUD_PLAINTEXT_MATERIAL_FORBIDDEN",
                "plaintext_option_forbidden:" + offending_prefix);
  }

  const std::string identity_mode = CanonicalCloudIdentityMode(FirstPresentValue(request, {"identity_mode:", "cloud_identity_mode:"}));
  if (identity_mode.empty()) {
    return Fail(request, "SB_DIAG_CLOUD_IDENTITY_MODE_UNSUPPORTED", "identity_mode_required");
  }
  const bool known_identity_mode =
      identity_mode == "workload_identity" || identity_mode == "oidc_federation" ||
      identity_mode == "managed_identity" || identity_mode == "iam_role" ||
      identity_mode == "service_account_token" || identity_mode == "static_secret" ||
      identity_mode == "local_emulator_identity";
  if (!known_identity_mode) {
    return Fail(request, "SB_DIAG_CLOUD_IDENTITY_MODE_UNSUPPORTED", "identity_mode_unsupported:" + identity_mode);
  }

  CloudIdentityKmsValidation identity_check;
  if (identity_mode == "static_secret") {
    identity_check = ValidateStaticSecretException(request);
  } else {
    identity_check = ValidateSecretlessIdentity(request, identity_mode);
  }
  if (!identity_check.ok) { return identity_check; }

  const std::string kms_mode = CanonicalCloudKmsMode(FirstPresentValue(request, {"kms_mode:", "cloud_kms_mode:"}));
  auto kms_check = ValidateKmsPolicy(request, kms_mode);
  if (!kms_check.ok) { return kms_check; }

  CloudIdentityKmsValidation validation;
  validation.ok = true;
  validation.identity_mode = identity_mode;
  validation.kms_mode = kms_mode;
  validation.diagnostic = MakeEngineApiDiagnostic("SB_ENGINE_API_OK", "engine.api.ok", {}, false);
  validation.identity_reference = MakeIdentityReference(request, identity_mode);
  validation.kms_reference = MakeKmsReference(request, kms_mode);
  validation.envelope = MakeEnvelope(request, kms_mode, validation.kms_reference);

  const bool static_exception = identity_mode == "static_secret";
  const bool local_emulator = identity_mode == "local_emulator_identity" || kms_mode == "local_emulator";
  validation.evidence.push_back({"cloud_identity_kms_policy_validated", validation.envelope.envelope_uuid.canonical});
  validation.evidence.push_back({"cloud_identity_binding_reference", validation.identity_reference.reference_uuid.canonical});
  validation.evidence.push_back({"cloud_kms_wrapping_reference", validation.kms_reference.reference_uuid.canonical});
  validation.evidence.push_back({"cloud_identity_kms_audit", OptionValue(request, "audit_policy_uuid:")});
  if (static_exception) {
    validation.evidence.push_back({"cloud_static_secret_policy_exception", OptionValue(request, "static_secret_audit_evidence_uuid:")});
  }
  if (local_emulator) {
    validation.evidence.push_back({"cloud_local_emulator_fixture", validation.envelope.envelope_uuid.canonical});
  }

  validation.rows.push_back({"decision", "allow"});
  validation.rows.push_back({"identity_mode", identity_mode});
  validation.rows.push_back({"identity_secretless", CloudIdentityModeIsSecretless(identity_mode) ? "true" : "false"});
  validation.rows.push_back({"static_secret_policy_exception", static_exception ? "true" : "false"});
  validation.rows.push_back({"kms_mode", kms_mode});
  validation.rows.push_back({"provider_profile_uuid", OptionValue(request, "provider_profile_uuid:")});
  validation.rows.push_back({"identity_reference_uuid", validation.identity_reference.reference_uuid.canonical});
  validation.rows.push_back({"identity_external_subject_ref", validation.identity_reference.redacted_external_reference});
  validation.rows.push_back({"kms_reference_uuid", validation.kms_reference.reference_uuid.canonical});
  validation.rows.push_back({"kms_key_reference", validation.kms_reference.redacted_external_reference});
  validation.rows.push_back({"protected_material_uuid", validation.kms_reference.protected_material_uuid});
  validation.rows.push_back({"protected_material_version_uuid", validation.kms_reference.protected_material_version_uuid});
  validation.rows.push_back({"envelope_uuid", validation.envelope.envelope_uuid.canonical});
  validation.rows.push_back({"wrapping_reference_uuid", validation.envelope.wrapping_reference_uuid.canonical});
  validation.rows.push_back({"envelope_version", validation.envelope.envelope_version});
  validation.rows.push_back({"rotation_policy_uuid", validation.envelope.rotation_policy_uuid});
  validation.rows.push_back({"audit_policy_uuid", validation.envelope.audit_policy_uuid});
  validation.rows.push_back({"plaintext_material_persisted", "false"});
  validation.rows.push_back({"plaintext_material_returned", "false"});
  validation.rows.push_back({"external_kms_dependency", "not_required"});
  validation.rows.push_back({"local_emulator_fixture", local_emulator ? "true" : "false"});
  validation.rows.push_back({"transaction_finality_authority", "scratchbird_mga_not_kms"});
  return validation;
}

EngineApiResult ValidateCloudIdentityKmsPolicyApi(const EngineApiRequest& request) {
  const auto validation = ValidateCloudIdentityKmsPolicy(request);
  EngineApiResult result;
  result.ok = validation.ok;
  result.operation_id = request.operation_id.empty() ? "cloud.identity_kms.validate_policy" : request.operation_id;
  result.embedded_trust_mode_observed = request.context.trust_mode == EngineTrustMode::embedded_in_process;
  result.primary_object.uuid = validation.envelope.envelope_uuid;
  result.primary_object.object_kind = "cloud_kms_envelope_metadata";
  if (validation.diagnostic.error) { result.diagnostics.push_back(validation.diagnostic); }
  for (const auto& evidence : validation.evidence) { result.evidence.push_back(evidence); }
  for (const auto& row : validation.rows) { AddResultRow(&result, {{row.first, row.second}}); }
  return result;
}

}  // namespace scratchbird::engine::internal_api

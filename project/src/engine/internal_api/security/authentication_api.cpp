// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "security/authentication_api.hpp"

#include "behavior_support/api_behavior_store.hpp"
#include "metric_contracts.hpp"
#include "metric_producer.hpp"
#include "security/auth_provider_model.hpp"
#include "security/security_crypto_policy.hpp"
#include "security/security_principal_lifecycle.hpp"
#include "uuid.hpp"

#include <openssl/crypto.h>
#include <openssl/evp.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <map>
#include <sstream>
#include <utility>
#include <vector>

namespace scratchbird::engine::internal_api {
namespace {

std::string AuthMetricProvider(const EngineAuthenticateRequest& request) {
  const std::string provider = !request.provider_family.empty()
      ? request.provider_family
      : SecurityOptionValue(request, "provider:");
  return provider.empty() ? "local_password" : provider;
}

void RecordAuthenticationAttempt(const EngineAuthenticateRequest& request) {
  (void)scratchbird::core::metrics::IncrementCounter(
      "sb_identity_auth_attempts_total",
      scratchbird::core::metrics::Labels({{"component", "security.authenticate"},
                                          {"provider_family", AuthMetricProvider(request)}}),
      1.0,
      "security_auth");
}

void RecordAuthenticationFailure(const EngineAuthenticateRequest& request, const std::string& reason) {
  (void)scratchbird::core::metrics::IncrementCounter(
      "sb_auth_failures_total",
      scratchbird::core::metrics::Labels({{"component", "security.authenticate"},
                                          {"provider_family", AuthMetricProvider(request)},
                                          {"reason", reason.empty() ? "unknown" : reason}}),
      1.0,
      "security_auth");
}

EngineAuthenticateResult AuthenticationFailureResult(const EngineAuthenticateRequest& request,
                                                     std::string code,
                                                     std::string detail) {
  RecordAuthenticationFailure(request, detail);
  return SecurityFailure<EngineAuthenticateResult>(
      request.context,
      "security.authenticate",
      MakeSecurityDiagnostic(std::move(code), std::move(detail)));
}

bool IsHexVerifier(std::string_view value) {
  if (value.size() != 64) return false;
  for (const unsigned char ch : value) {
    if (!std::isxdigit(ch)) return false;
  }
  return true;
}

std::map<std::string, std::string> ParseEvidenceFields(std::string_view evidence) {
  std::map<std::string, std::string> fields;
  std::size_t cursor = 0;
  while (cursor < evidence.size()) {
    const std::size_t end = evidence.find(';', cursor);
    const std::string_view part = evidence.substr(cursor, end == std::string_view::npos ? evidence.size() - cursor
                                                                                        : end - cursor);
    const std::size_t eq = part.find('=');
    if (eq != std::string_view::npos && eq != 0 && eq + 1 < part.size()) {
      fields.emplace(std::string(part.substr(0, eq)), std::string(part.substr(eq + 1)));
    }
    if (end == std::string_view::npos) break;
    cursor = end + 1;
  }
  return fields;
}

bool IsDurableSecurityStateAuthority(std::string_view value) {
  return value == "durable_security_catalog" ||
         value == "engine_durable_security_state" ||
         value == "mga_security_principal_lifecycle";
}

std::string FieldOrOption(const EngineAuthenticateRequest& request,
                          const std::map<std::string, std::string>& fields,
                          const std::string& field,
                          const std::string& option_prefix) {
  const auto found = fields.find(field);
  if (found != fields.end()) { return found->second; }
  return SecurityOptionValue(request, option_prefix);
}

bool IsDurablePrincipalUuid(std::string_view value) {
  if (value.empty()) { return false; }
  const auto parsed =
      scratchbird::core::uuid::ParseDurableEngineIdentityUuid(
          scratchbird::core::platform::UuidKind::principal,
          std::string(value));
  return parsed.ok();
}

std::string DurablePrincipalUuid(
    const EngineAuthenticateRequest& request,
    const std::map<std::string, std::string>& fields) {
  return FieldOrOption(request, fields, "principal_uuid", "durable_principal_uuid:");
}

void AddUniqueAuthorizationTag(std::vector<std::string>* tags, std::string tag) {
  if (tags == nullptr || tag.empty()) { return; }
  if (std::find(tags->begin(), tags->end(), tag) == tags->end()) {
    tags->push_back(std::move(tag));
  }
}

std::vector<std::string> DurableAuthorizationTagsForPrincipal(
    const EngineAuthenticateRequest& request,
    const std::string& principal_uuid,
    bool include_connect_fallback,
    const EngineSecurityPrincipalLifecycleState* retained_state) {
  std::vector<std::string> tags;
  if (include_connect_fallback) {
    AddUniqueAuthorizationTag(&tags, "right:CONNECT");
  }
  if (!IsDurablePrincipalUuid(principal_uuid)) { return tags; }
  EngineLoadSecurityPrincipalLifecycleStateResult loaded;
  const EngineSecurityPrincipalLifecycleState* state = retained_state;
  if (state == nullptr) {
    loaded = LoadSecurityPrincipalLifecycleState(request.context);
    if (!loaded.ok) { return tags; }
    state = &loaded.state;
  }
  for (const auto& grant : state->grants) {
    if (grant.grantee_kind != "principal" ||
        grant.grantee_uuid != principal_uuid ||
        !grant.target_object_uuid.empty() ||
        grant.grant_effect == "deny" ||
        !IsKnownSecurityRight(grant.privilege)) {
      continue;
    }
    AddUniqueAuthorizationTag(&tags, "right:" + grant.privilege);
  }
  return tags;
}

int LowerHexValue(char value) {
  if (value >= '0' && value <= '9') return value - '0';
  if (value >= 'a' && value <= 'f') return 10 + value - 'a';
  return -1;
}

template <std::size_t N>
bool DecodeLowerHexExact(std::string_view value,
                         std::array<unsigned char, N>* output) {
  if (output == nullptr || value.size() != N * 2) return false;
  for (std::size_t index = 0; index < N; ++index) {
    const int high = LowerHexValue(value[index * 2]);
    const int low = LowerHexValue(value[index * 2 + 1]);
    if (high < 0 || low < 0) return false;
    (*output)[index] = static_cast<unsigned char>((high << 4) | low);
  }
  return true;
}

bool VerifyLocalPasswordCredentialFingerprint(std::string_view stored,
                                              std::string_view password) {
  static constexpr std::string_view kPrefix =
      "local-password-pbkdf2-sha256:v1:iterations=";
  static constexpr std::string_view kSaltMarker = ":salt=";
  static constexpr std::string_view kVerifierMarker = ":verifier=";
  if (password.empty() || password.size() > 1024 ||
      password.find('\0') != std::string_view::npos ||
      stored.rfind(kPrefix, 0) != 0) {
    return false;
  }
  const std::size_t salt_marker = stored.find(kSaltMarker, kPrefix.size());
  if (salt_marker == std::string_view::npos) return false;
  const std::string_view iteration_text =
      stored.substr(kPrefix.size(), salt_marker - kPrefix.size());
  if (iteration_text.empty() || iteration_text.size() > 8 ||
      (iteration_text.size() > 1 && iteration_text.front() == '0')) {
    return false;
  }
  std::uint64_t iterations = 0;
  for (const char value : iteration_text) {
    if (value < '0' || value > '9') return false;
    iterations = iterations * 10 + static_cast<std::uint64_t>(value - '0');
  }
  if (iterations < 600000 || iterations > 10000000) return false;
  const std::size_t salt_begin = salt_marker + kSaltMarker.size();
  const std::size_t verifier_marker = stored.find(kVerifierMarker, salt_begin);
  if (verifier_marker == std::string_view::npos ||
      verifier_marker - salt_begin != 32) {
    return false;
  }
  const std::size_t verifier_begin = verifier_marker + kVerifierMarker.size();
  if (stored.size() - verifier_begin != 64) return false;

  std::array<unsigned char, 16> salt{};
  std::array<unsigned char, 32> expected{};
  std::array<unsigned char, 32> computed{};
  const bool decoded = DecodeLowerHexExact(
                           stored.substr(salt_begin, 32), &salt) &&
                       DecodeLowerHexExact(
                           stored.substr(verifier_begin, 64), &expected);
  const bool derived = decoded &&
      PKCS5_PBKDF2_HMAC(
          password.data(), static_cast<int>(password.size()), salt.data(),
          static_cast<int>(salt.size()), static_cast<int>(iterations),
          EVP_sha256(), static_cast<int>(computed.size()), computed.data()) == 1;
  const bool matches = derived &&
      CRYPTO_memcmp(expected.data(), computed.data(), expected.size()) == 0;
  OPENSSL_cleanse(salt.data(), salt.size());
  OPENSSL_cleanse(expected.data(), expected.size());
  OPENSSL_cleanse(computed.data(), computed.size());
  return matches;
}

std::string TemporaryTokenCredentialFingerprint(std::string_view token_handle,
                                                std::string_view token_digest,
                                                std::string_view state,
                                                std::string_view expires_at_ms) {
  std::string payload;
  payload.reserve(token_digest.size() + state.size() + expires_at_ms.size() + 2);
  payload += token_digest;
  payload += '|';
  payload += state.empty() ? "active" : state;
  payload += '|';
  payload += expires_at_ms.empty() ? "0" : expires_at_ms;
  return "security-temporary-token:v1:hmac-sha256:" +
         SecurityHmacSha256Hex(token_handle, payload);
}

EngineApiDiagnostic DurableCredentialFingerprintForPrincipal(
    const EngineAuthenticateRequest& request,
    const std::string& principal,
    const std::string& durable_principal_uuid,
    std::string* credential_fingerprint,
    const EngineSecurityPrincipalLifecycleState* retained_state) {
  if (credential_fingerprint == nullptr) {
    return MakeSecurityDiagnostic("SECURITY.AUTHENTICATION.FAILED",
                                  "credential_fingerprint_output_required");
  }
  if (request.context.database_path.empty()) {
    return MakeSecurityDiagnostic("SECURITY.AUTH_SOURCE_UNAVAILABLE",
                                  "durable_security_state_database_path_required");
  }
  EngineLoadSecurityPrincipalLifecycleStateResult loaded;
  const EngineSecurityPrincipalLifecycleState* state = retained_state;
  if (state == nullptr) {
    loaded = LoadSecurityPrincipalLifecycleState(request.context);
    if (!loaded.ok) {
      return MakeSecurityDiagnostic("SECURITY.AUTH_SOURCE_UNAVAILABLE",
                                    loaded.diagnostic.detail.empty()
                                        ? "durable_security_state_unavailable"
                                        : loaded.diagnostic.detail);
    }
    state = &loaded.state;
  }
  for (const auto& record : state->principals) {
    if (record.principal_uuid != durable_principal_uuid) { continue; }
    if (record.principal_name != principal) {
      return MakeSecurityDiagnostic("SECURITY.AUTHENTICATION.FAILED",
                                    "durable_principal_name_mismatch");
    }
    if (record.lifecycle_state != "active") {
      return MakeSecurityDiagnostic("SECURITY.AUTHENTICATION.FAILED",
                                    "durable_principal_disabled");
    }
    if (record.credential_fingerprint.empty()) {
      return MakeSecurityDiagnostic("SECURITY.AUTHENTICATION.FAILED",
                                    "durable_credential_fingerprint_required");
    }
    *credential_fingerprint = record.credential_fingerprint;
    return EngineApiDiagnostic{"SB_ENGINE_API_OK", "engine.api.ok", {}, false};
  }
  return MakeSecurityDiagnostic("SECURITY.AUTHENTICATION.FAILED",
                                "durable_principal_credential_missing");
}

EngineApiDiagnostic DurableCredentialFingerprintForPrincipalName(
    const EngineAuthenticateRequest& request,
    const std::string& principal,
    std::string* durable_principal_uuid,
    std::string* credential_fingerprint,
    const EngineSecurityPrincipalLifecycleState* retained_state) {
  if (durable_principal_uuid == nullptr || credential_fingerprint == nullptr) {
    return MakeSecurityDiagnostic("SECURITY.AUTHENTICATION.FAILED",
                                  "credential_fingerprint_output_required");
  }
  if (principal.empty()) {
    return MakeSecurityDiagnostic("SECURITY.AUTHENTICATION.REQUEST_INVALID",
                                  "principal_claim_required");
  }
  if (request.context.database_path.empty()) {
    return MakeSecurityDiagnostic("SECURITY.AUTH_SOURCE_UNAVAILABLE",
                                  "durable_security_state_database_path_required");
  }
  EngineLoadSecurityPrincipalLifecycleStateResult loaded;
  const EngineSecurityPrincipalLifecycleState* state = retained_state;
  if (state == nullptr) {
    loaded = LoadSecurityPrincipalLifecycleState(request.context);
    if (!loaded.ok) {
      return MakeSecurityDiagnostic("SECURITY.AUTH_SOURCE_UNAVAILABLE",
                                    loaded.diagnostic.detail.empty()
                                        ? "durable_security_state_unavailable"
                                        : loaded.diagnostic.detail);
    }
    state = &loaded.state;
  }
  const EngineSecurityPrincipalRecord* matched = nullptr;
  for (const auto& record : state->principals) {
    if (record.principal_name != principal) { continue; }
    if (matched != nullptr) {
      return MakeSecurityDiagnostic("SECURITY.AUTHENTICATION.FAILED",
                                    "durable_principal_name_ambiguous");
    }
    matched = &record;
  }
  if (matched == nullptr) {
    return MakeSecurityDiagnostic("SECURITY.AUTHENTICATION.FAILED",
                                  "durable_principal_credential_missing");
  }
  if (matched->lifecycle_state != "active") {
    return MakeSecurityDiagnostic("SECURITY.AUTHENTICATION.FAILED",
                                  "durable_principal_disabled");
  }
  if (matched->credential_fingerprint.empty()) {
    return MakeSecurityDiagnostic("SECURITY.AUTHENTICATION.FAILED",
                                  "durable_credential_fingerprint_required");
  }
  *durable_principal_uuid = matched->principal_uuid;
  *credential_fingerprint = matched->credential_fingerprint;
  return EngineApiDiagnostic{"SB_ENGINE_API_OK", "engine.api.ok", {}, false};
}

bool LooksLikeStructuredAuthEvidence(std::string_view evidence) {
  return evidence.find("scheme=") != std::string_view::npos ||
         evidence.find(';') != std::string_view::npos;
}

EngineApiDiagnostic VerifyLocalPasswordEvidence(const EngineAuthenticateRequest& request,
                                                const std::string& principal,
                                                std::string* resolved_principal_uuid,
                                                bool* server_derived_connect_right,
                                                const EngineSecurityPrincipalLifecycleState*
                                                    retained_state) {
  if (request.credential_invalid_claim ||
      SecurityOptionPresent(request, "credential:invalid") ||
      SecurityOptionBool(request, "fixture_fail:", false)) {
    return MakeSecurityDiagnostic("SECURITY.AUTHENTICATION.FAILED", "credential_evidence_failed");
  }
  const auto fields = ParseEvidenceFields(request.credential_evidence);
  const auto scheme = fields.find("scheme");
  const auto evidence_principal = fields.find("principal");
  const auto verifier = fields.find("verifier");
  const std::string durable_principal_uuid = DurablePrincipalUuid(request, fields);
  const std::string storage_authority =
      FieldOrOption(request, fields, "storage_authority", "security_storage_authority:");
  if (scheme == fields.end() && !LooksLikeStructuredAuthEvidence(request.credential_evidence)) {
    std::string resolved_uuid;
    std::string durable_fingerprint;
    const auto durable = DurableCredentialFingerprintForPrincipalName(
        request, principal, &resolved_uuid, &durable_fingerprint,
        retained_state);
    if (durable.error) { return durable; }
    if (!VerifyLocalPasswordCredentialFingerprint(
            durable_fingerprint, request.credential_evidence)) {
      return MakeSecurityDiagnostic("SECURITY.AUTHENTICATION.FAILED",
                                    "credential_verifier_mismatch");
    }
    if (resolved_principal_uuid != nullptr) { *resolved_principal_uuid = resolved_uuid; }
    if (server_derived_connect_right != nullptr) { *server_derived_connect_right = true; }
    return EngineApiDiagnostic{"SB_ENGINE_API_OK", "engine.api.ok", {}, false};
  }
  if (scheme == fields.end() || scheme->second != "local_password_v1" ||
      evidence_principal == fields.end() || evidence_principal->second != principal ||
      verifier == fields.end() || !IsHexVerifier(verifier->second)) {
    return MakeSecurityDiagnostic("SECURITY.AUTHENTICATION.FAILED", "credential_verifier_evidence_required");
  }
  if (!IsDurablePrincipalUuid(durable_principal_uuid)) {
    return MakeSecurityDiagnostic("SECURITY.AUTHENTICATION.FAILED", "durable_principal_uuid_required");
  }
  if (!IsDurableSecurityStateAuthority(storage_authority)) {
    return MakeSecurityDiagnostic("SECURITY.AUTHENTICATION.FAILED",
                                  "durable_security_state_authority_required");
  }
  std::string durable_fingerprint;
  const auto durable = DurableCredentialFingerprintForPrincipal(
      request, principal, durable_principal_uuid, &durable_fingerprint,
      retained_state);
  if (durable.error) { return durable; }
  return MakeSecurityDiagnostic("SECURITY.AUTHENTICATION.FAILED",
                                "password_secret_required_for_pbkdf2_verification");
}

std::uint64_t CurrentEpochMilliseconds() {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch()).count());
}

bool TemporaryTokenTextValid(std::string_view token) {
  if (token.empty() || token.size() > 1024) return false;
  for (const unsigned char ch : token) {
    const bool ok = std::isalnum(ch) || ch == '-' || ch == '_' || ch == '.' || ch == ':';
    if (!ok) return false;
  }
  return true;
}

EngineApiDiagnostic VerifySecurityDatabaseTemporaryTokenEvidence(
    const EngineAuthenticateRequest& request,
    const std::string& principal) {
  if (principal.empty()) {
    return MakeSecurityDiagnostic("SECURITY.AUTHENTICATION.REQUEST_INVALID", "principal_claim_required");
  }
  if (request.credential_invalid_claim ||
      SecurityOptionPresent(request, "credential:invalid") ||
      SecurityOptionBool(request, "fixture_fail:", false)) {
    return MakeSecurityDiagnostic("SECURITY.AUTHENTICATION.FAILED", "credential_evidence_failed");
  }
  const auto fields = ParseEvidenceFields(request.credential_evidence);
  const auto scheme = fields.find("scheme");
  const auto evidence_principal = fields.find("principal");
  const auto token = fields.find("token");
  const std::string durable_principal_uuid = DurablePrincipalUuid(request, fields);
  const std::string storage_authority =
      FieldOrOption(request, fields, "storage_authority", "security_storage_authority:");
  const std::string token_handle =
      FieldOrOption(request, fields, "token_handle", "durable_token_handle:");
  const auto token_digest = fields.find("token_digest");
  if (scheme == fields.end() || scheme->second != "security_database_temporary_token_v1" ||
      evidence_principal == fields.end() || evidence_principal->second != principal ||
      token == fields.end() || !TemporaryTokenTextValid(token->second)) {
    return MakeSecurityDiagnostic("SECURITY.AUTHENTICATION.FAILED",
                                  "security_database_temporary_token_evidence_required");
  }
  if (!IsDurablePrincipalUuid(durable_principal_uuid)) {
    return MakeSecurityDiagnostic("SECURITY.AUTHENTICATION.FAILED", "durable_principal_uuid_required");
  }
  if (!IsDurableSecurityStateAuthority(storage_authority)) {
    return MakeSecurityDiagnostic("SECURITY.AUTHENTICATION.FAILED",
                                  "durable_security_state_authority_required");
  }
  if (token_handle.empty()) {
    return MakeSecurityDiagnostic("SECURITY.AUTHENTICATION.FAILED",
                                  "security_database_temporary_token_handle_required");
  }
  if (token_digest == fields.end() || !IsHexVerifier(token_digest->second)) {
    return MakeSecurityDiagnostic("SECURITY.AUTHENTICATION.FAILED",
                                  "security_database_temporary_token_digest_required");
  }
  const std::string computed_digest = SecuritySha256Hex(token->second);
  if (!IsHexVerifier(computed_digest) ||
      !SecurityConstantTimeEqual(token_digest->second, computed_digest)) {
    return MakeSecurityDiagnostic("SECURITY.AUTHENTICATION.FAILED",
                                  "security_database_temporary_token_digest_mismatch");
  }
  const std::string row_state = FieldOrOption(request, fields, "state", "durable_token_state:");
  if (!row_state.empty() && row_state != "active" && row_state != "valid") {
    return MakeSecurityDiagnostic("SECURITY.AUTHENTICATION.FAILED",
                                  "security_database_temporary_token_revoked");
  }
  const std::string row_expires =
      FieldOrOption(request, fields, "expires_at_ms", "durable_token_expires_at_ms:");
  if (!row_expires.empty()) {
    std::uint64_t expires_at_ms = 0;
    try {
      expires_at_ms = static_cast<std::uint64_t>(std::stoull(row_expires));
    } catch (...) {
      return MakeSecurityDiagnostic("SECURITY.AUTHENTICATION.FAILED",
                                    "security_database_temporary_token_expiry_invalid");
    }
    if (expires_at_ms != 0 && expires_at_ms < CurrentEpochMilliseconds()) {
      return MakeSecurityDiagnostic("SECURITY.AUTHENTICATION.FAILED",
                                    "security_database_temporary_token_expired");
    }
  }
  std::string durable_fingerprint;
  const auto durable = DurableCredentialFingerprintForPrincipal(
      request, principal, durable_principal_uuid, &durable_fingerprint,
      nullptr);
  if (durable.error) { return durable; }
  const std::string expected = TemporaryTokenCredentialFingerprint(
      token_handle,
      token_digest->second,
      row_state.empty() ? "active" : row_state,
      row_expires.empty() ? "0" : row_expires);
  if (expected.empty() || !SecurityConstantTimeEqual(durable_fingerprint, expected)) {
    return MakeSecurityDiagnostic("SECURITY.AUTHENTICATION.FAILED",
                                  "security_database_temporary_token_not_found");
  }
  return EngineApiDiagnostic{"SB_ENGINE_API_OK", "engine.api.ok", {}, false};
}

}  // namespace

// SEARCH_KEY: SB_ENGINE_INTERNAL_API_SECURITY_AUTHENTICATION_API_BEHAVIOR
EngineAuthenticateResult EngineAuthenticate(const EngineAuthenticateRequest& request) {
  RecordAuthenticationAttempt(request);
  const std::string provider = !request.provider_family.empty()
      ? request.provider_family
      : SecurityOptionValue(request, "provider:");
  const std::string principal = !request.principal_claim.empty()
      ? request.principal_claim
      : SecurityOptionValue(request, "principal:");

  EngineApiRequest provider_request = request;
  provider_request.option_envelopes.push_back("auth_flow:login");
  provider_request.option_envelopes.push_back("provider:" + (provider.empty() ? "local_password" : provider));
  if (!principal.empty()) {
    provider_request.option_envelopes.push_back("principal:" + principal);
  }
  const bool credential_present = request.credential_evidence_present ||
                                  !request.credential_evidence.empty() ||
                                  SecurityOptionBool(request, "credential_evidence_present:", false) ||
                                  SecurityOptionPresent(request, "credential:valid");
  const std::string canonical_provider = CanonicalAuthProviderFamily(provider.empty() ? "local_password" : provider);
  auto credential_fields = ParseEvidenceFields(request.credential_evidence);
  std::vector<std::string> engine_authorization_tags;
  std::shared_ptr<const EngineSecurityPrincipalLifecycleState>
      authentication_durable_security_state;
  if (canonical_provider == "security_database_temporary_token") {
    const auto temporary_token = VerifySecurityDatabaseTemporaryTokenEvidence(request, principal);
    if (temporary_token.error) {
      return AuthenticationFailureResult(request, temporary_token.code, temporary_token.detail);
    }
    provider_request.option_envelopes.push_back("credential:valid");
    provider_request.option_envelopes.push_back("credential_evidence_present:true");
    provider_request.option_envelopes.push_back("protected_material:available");
  } else if ((provider.empty() || provider == "local_password") && credential_present) {
    auto loaded = LoadSecurityPrincipalLifecycleState(request.context);
    if (!loaded.ok) {
      return AuthenticationFailureResult(
          request,
          "SECURITY.AUTH_SOURCE_UNAVAILABLE",
          loaded.diagnostic.detail.empty()
              ? "durable_security_state_unavailable"
              : loaded.diagnostic.detail);
    }
    authentication_durable_security_state =
        std::make_shared<const EngineSecurityPrincipalLifecycleState>(
            std::move(loaded.state));
    std::string resolved_principal_uuid;
    bool server_derived_connect_right = false;
    const auto local_password = VerifyLocalPasswordEvidence(
        request, principal, &resolved_principal_uuid,
        &server_derived_connect_right,
        authentication_durable_security_state.get());
    if (local_password.error) {
      return AuthenticationFailureResult(request, local_password.code, local_password.detail);
    }
    if (!resolved_principal_uuid.empty()) {
      credential_fields.emplace("principal_uuid", resolved_principal_uuid);
      credential_fields.emplace("storage_authority", "mga_security_principal_lifecycle");
    }
    engine_authorization_tags = DurableAuthorizationTagsForPrincipal(
        request, resolved_principal_uuid, server_derived_connect_right,
        authentication_durable_security_state.get());
    if (server_derived_connect_right) {
      provider_request.option_envelopes.push_back("credential_password_transport:raw");
    }
    provider_request.option_envelopes.push_back("credential_verifier_match:true");
    provider_request.option_envelopes.push_back("protected_material:available");
  } else if (credential_present) {
    provider_request.option_envelopes.push_back("credential_evidence_present:true");
  }
  const bool mfa_required = SecurityOptionBool(request, "mfa_required:", false);
  const bool mfa_present = request.mfa_evidence_present || SecurityOptionBool(request, "mfa_evidence_present:", false) ||
                           SecurityOptionPresent(request, "mfa:present");
  if (mfa_required) {
    provider_request.option_envelopes.push_back("mfa_required:true");
  }
  if (mfa_present) {
    provider_request.option_envelopes.push_back("mfa_evidence_present:true");
    provider_request.option_envelopes.push_back("mfa:present");
  }

  const auto provider_decision = AuthenticateWithProvider(provider_request);
  if (!provider_decision.ok || !provider_decision.authenticated) {
    return AuthenticationFailureResult(request,
                                       provider_decision.diagnostic.code.empty()
                                           ? "SECURITY.AUTHENTICATION.FAILED"
                                           : provider_decision.diagnostic.code,
                                       provider_decision.diagnostic.detail.empty()
                                           ? "provider_denied"
                                           : provider_decision.diagnostic.detail);
  }

  EngineApiRequest context_request = request;
  context_request.target_object.uuid = request.target_object.uuid;
  auto context = ConnectionSecurityContextFromRequest(context_request);
  const std::string durable_principal_uuid =
      DurablePrincipalUuid(request, credential_fields);
  if (IsDurablePrincipalUuid(durable_principal_uuid)) {
    context.effective_user_uuid.canonical = durable_principal_uuid;
  } else if (context.effective_user_uuid.canonical.empty()) {
    context.effective_user_uuid.canonical = GenerateCrudEngineUuid("principal");
  }
  if (context.connection_uuid.canonical.empty()) { context.connection_uuid.canonical = GenerateCrudEngineUuid("session"); }
  if (context.authority_uuid.canonical.empty()) { context.authority_uuid.canonical = request.context.database_uuid.canonical; }
  context.authorization_trace_tags = std::move(engine_authorization_tags);

  auto result = SecuritySuccess<EngineAuthenticateResult>(request.context, "security.authenticate");
  result.authenticated = true;
  result.connection_security_context = context;
  result.durable_security_state =
      std::move(authentication_durable_security_state);
  result.primary_object.uuid = context.effective_user_uuid;
  result.primary_object.object_kind = "principal";
  (void)scratchbird::core::metrics::PublishIdentitySessionsActive(
      1.0,
      provider_decision.provider_family.empty() ? "local_password" : provider_decision.provider_family,
      "self",
      scratchbird::core::metrics::Labels({{"session_uuid", context.connection_uuid.canonical},
                                          {"principal_uuid", context.effective_user_uuid.canonical}}));
  (void)scratchbird::core::metrics::PublishIdentityUsersOnline(
      1.0,
      provider_decision.provider_family.empty() ? "local_password" : provider_decision.provider_family,
      scratchbird::core::metrics::Labels({{"principal_uuid", context.effective_user_uuid.canonical}}));
  ApplyAuthProviderDecision(&result, provider_decision);
  result.ok = true;
  result.authenticated = true;
  result.operation_id = "security.authenticate";
  AddSecurityEvidence(&result,
                      "authentication_provider",
                      provider_decision.provider_family.empty() ? "local_password" : provider_decision.provider_family);
  if (IsDurablePrincipalUuid(durable_principal_uuid)) {
    AddSecurityEvidence(&result, "durable_principal_uuid", durable_principal_uuid);
  }
  const std::string storage_authority =
      FieldOrOption(request, credential_fields, "storage_authority", "security_storage_authority:");
  if (IsDurableSecurityStateAuthority(storage_authority)) {
    AddSecurityEvidence(&result, "security_state_authority", storage_authority);
  }
  AddSecurityEvidence(&result, "connection_security_context", context.connection_uuid.canonical);
  for (const auto& tag : context.authorization_trace_tags) {
    if (tag.rfind("group:", 0) == 0) {
      AddSecurityEvidence(&result, "authorized_group", tag.substr(6));
    }
  }
  AddSecurityRow(&result, {{"authenticated", "true"},
                           {"provider", provider_decision.provider_family.empty()
                                            ? "local_password"
                                            : provider_decision.provider_family},
                           {"principal", provider_decision.principal.empty()
                                             ? principal
                                             : provider_decision.principal},
                           {"connection_uuid", context.connection_uuid.canonical},
                           {"effective_user_uuid", context.effective_user_uuid.canonical}});
  return result;
}

EngineRefreshSecurityContextResult EngineRefreshSecurityContext(
    const EngineRefreshSecurityContextRequest& request) {
  if (!request.context.security_context_present) {
    return SecurityFailure<EngineRefreshSecurityContextResult>(
        request.context,
        "security.refresh_context",
        MakeSecurityDiagnostic("SECURITY.AUTHENTICATION.REQUEST_INVALID", "security_context_required"));
  }
  if (SecurityContextHasTag(request.context, "security_context:expired") &&
      !SecurityContextHasRight(request.context, "SEC_IDENTITY_ADMIN")) {
    return SecurityFailure<EngineRefreshSecurityContextResult>(
        request.context,
        "security.refresh_context",
        MakeSecurityDiagnostic("SECURITY.CONTEXT.EXPIRED", "refresh_requires_authority"));
  }
  auto result = SecuritySuccess<EngineRefreshSecurityContextResult>(request.context, "security.refresh_context");
  result.refreshed = true;
  AddSecurityEvidence(&result, "security_context", "refreshed");
  AddSecurityRow(&result, {{"refreshed", "true"}});
  return result;
}

}  // namespace scratchbird::engine::internal_api

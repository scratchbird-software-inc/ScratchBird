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

#include <algorithm>
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

bool IsBootstrapAuthorizationTagAllowed(
    const EngineAuthenticateRequest& request,
    const std::map<std::string, std::string>& fields) {
  const std::string bootstrap_only =
      FieldOrOption(request, fields, "bootstrap_only", "bootstrap_only:");
  const std::string bootstrap_evidence =
      FieldOrOption(request, fields, "bootstrap_evidence_uuid", "bootstrap_evidence_uuid:");
  return SecurityOptionPresent(request, "bootstrap_security_state:present") &&
         (bootstrap_only == "true" || bootstrap_only == "1") &&
         !bootstrap_evidence.empty();
}

bool IsAllowedDurableAuthorizationTag(
    const EngineAuthenticateRequest& request,
    const std::map<std::string, std::string>& fields,
    std::string_view tag) {
  if (tag.rfind("group:", 0) == 0 || tag.rfind("role:", 0) == 0 ||
      tag.rfind("right:", 0) == 0) {
    return true;
  }
  return tag == "security.bootstrap" &&
         IsBootstrapAuthorizationTagAllowed(request, fields);
}

std::vector<std::string> SplitCsv(std::string_view value) {
  std::vector<std::string> out;
  std::size_t cursor = 0;
  while (cursor < value.size()) {
    const std::size_t end = value.find(',', cursor);
    const std::string_view part =
        value.substr(cursor,
                     end == std::string_view::npos ? value.size() - cursor
                                                   : end - cursor);
    if (!part.empty()) { out.emplace_back(part); }
    if (end == std::string_view::npos) { break; }
    cursor = end + 1;
  }
  return out;
}

std::vector<std::string> AuthorizationTraceTagsForDurableSecurityState(
    const EngineAuthenticateRequest& request,
    const std::map<std::string, std::string>& fields) {
  std::vector<std::string> tags;
  const std::string evidence_tags =
      FieldOrOption(request, fields, "authorization_tags", "durable_authorization_tags:");
  for (const auto& tag : SplitCsv(evidence_tags)) {
    if (IsAllowedDurableAuthorizationTag(request, fields, tag)) {
      tags.push_back(tag);
    }
  }
  for (const auto& option : request.option_envelopes) {
    constexpr std::string_view kPrefix = "durable_authorization_tag:";
    if (option.rfind(std::string(kPrefix), 0) == 0) {
      const std::string tag = option.substr(kPrefix.size());
      if (IsAllowedDurableAuthorizationTag(request, fields, tag)) {
        tags.push_back(tag);
      }
    }
  }
  return tags;
}

std::string LocalPasswordCredentialFingerprint(std::string_view verifier) {
  return "local-password-verifier:v1:sha256:" + SecuritySha256Hex(verifier);
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
    std::string* credential_fingerprint) {
  if (credential_fingerprint == nullptr) {
    return MakeSecurityDiagnostic("SECURITY.AUTHENTICATION.FAILED",
                                  "credential_fingerprint_output_required");
  }
  if (request.context.database_path.empty()) {
    return MakeSecurityDiagnostic("SECURITY.AUTH_SOURCE_UNAVAILABLE",
                                  "durable_security_state_database_path_required");
  }
  const auto loaded = LoadSecurityPrincipalLifecycleState(request.context);
  if (!loaded.ok) {
    return MakeSecurityDiagnostic("SECURITY.AUTH_SOURCE_UNAVAILABLE",
                                  loaded.diagnostic.detail.empty()
                                      ? "durable_security_state_unavailable"
                                      : loaded.diagnostic.detail);
  }
  for (const auto& record : loaded.state.principals) {
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

EngineApiDiagnostic VerifyLocalPasswordEvidence(const EngineAuthenticateRequest& request,
                                                const std::string& principal) {
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
      request, principal, durable_principal_uuid, &durable_fingerprint);
  if (durable.error) { return durable; }
  const std::string expected = LocalPasswordCredentialFingerprint(verifier->second);
  if (expected.empty() || !SecurityConstantTimeEqual(durable_fingerprint, expected)) {
    return MakeSecurityDiagnostic("SECURITY.AUTHENTICATION.FAILED",
                                  "credential_verifier_mismatch");
  }
  return EngineApiDiagnostic{"SB_ENGINE_API_OK", "engine.api.ok", {}, false};
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
      request, principal, durable_principal_uuid, &durable_fingerprint);
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
  const auto credential_fields = ParseEvidenceFields(request.credential_evidence);
  if (canonical_provider == "security_database_temporary_token") {
    const auto temporary_token = VerifySecurityDatabaseTemporaryTokenEvidence(request, principal);
    if (temporary_token.error) {
      return AuthenticationFailureResult(request, temporary_token.code, temporary_token.detail);
    }
    provider_request.option_envelopes.push_back("credential:valid");
    provider_request.option_envelopes.push_back("credential_evidence_present:true");
    provider_request.option_envelopes.push_back("protected_material:available");
  } else if ((provider.empty() || provider == "local_password") && credential_present) {
    const auto local_password = VerifyLocalPasswordEvidence(request, principal);
    if (local_password.error) {
      return AuthenticationFailureResult(request, local_password.code, local_password.detail);
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
  context.authorization_trace_tags =
      AuthorizationTraceTagsForDurableSecurityState(request, credential_fields);

  auto result = SecuritySuccess<EngineAuthenticateResult>(request.context, "security.authenticate");
  result.authenticated = true;
  result.connection_security_context = context;
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
    } else if (tag == "security.bootstrap") {
      AddSecurityEvidence(&result, "bootstrap_authority", "true");
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

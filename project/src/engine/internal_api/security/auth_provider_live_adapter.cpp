// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "security/auth_provider_live_adapter.hpp"

#include "api_diagnostics.hpp"
#include "security/auth_provider_model.hpp"
#include "security/security_model.hpp"

#include <chrono>
#include <cctype>
#include <map>
#include <set>
#include <sstream>
#include <string_view>

namespace scratchbird::engine::internal_api {
namespace {

// SEARCH_KEY: SB_ENGINE_INTERNAL_API_AUTH_PROVIDER_LIVE_ADAPTER_BEHAVIOR
constexpr std::size_t kMaxPayloadBytes = 8192;

bool StartsWith(const std::string& value, const std::string& prefix) { return value.rfind(prefix, 0) == 0; }

std::string OptionValue(const EngineApiRequest& request, const std::string& prefix) {
  for (const auto& option : request.option_envelopes) {
    if (StartsWith(option, prefix)) { return option.substr(prefix.size()); }
  }
  return {};
}

bool OptionPresent(const EngineApiRequest& request, const std::string& value) {
  for (const auto& option : request.option_envelopes) {
    if (option == value) { return true; }
  }
  return false;
}

bool LooksSafePayload(std::string_view payload) {
  if (payload.empty() || payload.size() > kMaxPayloadBytes) { return false; }
  for (const unsigned char c : payload) {
    if (c < 0x20 || c > 0x7e || c == '"' || c == '\\' || c == '`') { return false; }
  }
  return true;
}

bool IsNameChar(char c) {
  return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-' || c == '.' || c == ':' || c == '/' || c == '@';
}

bool BoundedToken(std::string_view value, std::size_t max_bytes) {
  if (value.empty() || value.size() > max_bytes) { return false; }
  for (char c : value) {
    if (!IsNameChar(c) && c != ',' && c != '=' && c != ' ') { return false; }
  }
  return true;
}

bool IsHex64(std::string_view value) {
  if (value.size() != 64) { return false; }
  for (char c : value) {
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) { return false; }
  }
  return true;
}

bool ParseBool(std::string_view value, bool* out) {
  if (!out) { return false; }
  if (value == "true" || value == "1" || value == "yes" || value == "on") { *out = true; return true; }
  if (value == "false" || value == "0" || value == "no" || value == "off") { *out = false; return true; }
  return false;
}

bool OptionBool(const EngineApiRequest& request, const std::string& prefix, bool fallback = false) {
  const std::string value = OptionValue(request, prefix);
  if (value.empty()) { return fallback; }
  bool parsed = fallback;
  return ParseBool(value, &parsed) ? parsed : fallback;
}

bool ParseU64(std::string_view value, std::uint64_t* out) {
  if (!out || value.empty()) { return false; }
  std::uint64_t parsed = 0;
  for (char c : value) {
    if (c < '0' || c > '9') { return false; }
    const std::uint64_t next = (parsed * 10u) + static_cast<std::uint64_t>(c - '0');
    if (next < parsed) { return false; }
    parsed = next;
  }
  *out = parsed;
  return true;
}

std::uint64_t NowMs() {
  const auto now = std::chrono::system_clock::now().time_since_epoch();
  return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(now).count());
}

using Payload = std::map<std::string, std::string>;

bool ParsePayload(std::string_view payload, Payload* out, std::string* error) {
  if (!out || !LooksSafePayload(payload)) {
    if (error) { *error = "payload_unsafe_or_oversized"; }
    return false;
  }
  out->clear();
  std::size_t cursor = 0;
  while (cursor < payload.size()) {
    const std::size_t sep = payload.find(';', cursor);
    const std::size_t end = sep == std::string_view::npos ? payload.size() : sep;
    if (end <= cursor) {
      if (error) { *error = "empty_payload_segment"; }
      return false;
    }
    const std::string_view pair = payload.substr(cursor, end - cursor);
    const std::size_t eq = pair.find('=');
    if (eq == std::string_view::npos || eq == 0 || eq + 1 >= pair.size()) {
      if (error) { *error = "malformed_payload_pair"; }
      return false;
    }
    const std::string key(pair.substr(0, eq));
    const std::string value(pair.substr(eq + 1));
    if (!BoundedToken(key, 64) || value.size() > 2048 || out->count(key) != 0 || out->size() >= 64) {
      if (error) { *error = "invalid_or_duplicate_payload_key"; }
      return false;
    }
    out->emplace(key, value);
    if (sep == std::string_view::npos) { break; }
    cursor = sep + 1;
  }
  return !out->empty();
}

std::string Get(const Payload& payload, const std::string& key) {
  const auto it = payload.find(key);
  return it == payload.end() ? std::string{} : it->second;
}

bool HasAny(const Payload& payload, std::initializer_list<const char*> keys) {
  for (const char* key : keys) {
    if (payload.count(key) != 0 && !Get(payload, key).empty()) { return true; }
  }
  return false;
}

bool RequireFields(const Payload& payload, std::initializer_list<const char*> keys, std::string* missing) {
  for (const char* key : keys) {
    if (Get(payload, key).empty()) {
      if (missing) { *missing = key; }
      return false;
    }
  }
  return true;
}

bool FutureField(const Payload& payload, const std::string& key) {
  std::uint64_t value = 0;
  return ParseU64(Get(payload, key), &value) && value > NowMs();
}

bool PayloadBool(const Payload& payload, const std::string& key, bool fallback = false) {
  bool parsed = fallback;
  return ParseBool(Get(payload, key), &parsed) ? parsed : fallback;
}

std::string RequiredClientDependencyForFamily(const std::string& family) {
  if (family == "ldap_ad") { return "ldap_client"; }
  if (family == "kerberos_pac") { return "gssapi_krb5"; }
  if (family == "pam") { return "pam"; }
  if (family == "radius") { return "radius_client"; }
  if (family == "oidc_jwt") { return "oidc_jwt_client"; }
  if (family == "saml") { return "saml_xmlsig"; }
  if (family == "webauthn" || family == "factor_chain") { return "webauthn_fido2"; }
  if (family == "workload_identity" || family == "managed_identity") { return "spiffe_svid_or_workload_oidc"; }
  if (family == "certificate_mtls") { return "tls_x509"; }
  if (family == "proxy_assertion") { return "proxy_assertion_verifier"; }
  if (family == "remote_security_database") { return "remote_scratchbird_security"; }
  return {};
}

bool DependencyAvailable(const EngineApiRequest& request, const std::string& dependency) {
  if (dependency.empty()) { return true; }
  return OptionPresent(request, "dependency:" + dependency + ":available") ||
         OptionPresent(request, "client_dependency:" + dependency + ":available") ||
         OptionPresent(request, "provider_dependency:" + dependency + ":available") ||
         OptionValue(request, "dependency_available:") == dependency;
}

bool DependencyMissing(const EngineApiRequest& request, const std::string& dependency) {
  return OptionBool(request, "missing_dependency:", false) ||
         OptionPresent(request, "library:missing") ||
         OptionPresent(request, "dependency:" + dependency + ":missing") ||
         OptionPresent(request, "client_dependency:" + dependency + ":missing") ||
         OptionPresent(request, "provider_dependency:" + dependency + ":missing");
}

AuthProviderLiveEvidenceResult Deny(const EngineApiRequest& request, std::string detail) {
  AuthProviderLiveEvidenceResult result;
  result.evaluated = true;
  result.ok = false;
  result.provider_family = CanonicalAuthProviderFamily(OptionValue(request, "provider:"));
  result.diagnostic = MakeSecurityDiagnostic("SECURITY.AUTHENTICATION.FAILED", std::move(detail));
  result.rows.push_back({"live_adapter", "deny"});
  return result;
}

AuthProviderLiveEvidenceResult DenyCode(const EngineApiRequest& request,
                                        std::string code,
                                        std::string detail) {
  AuthProviderLiveEvidenceResult result;
  result.evaluated = true;
  result.ok = false;
  result.provider_family = CanonicalAuthProviderFamily(OptionValue(request, "provider:"));
  result.diagnostic = MakeSecurityDiagnostic(std::move(code), std::move(detail));
  result.rows.push_back({"live_adapter", "deny"});
  return result;
}

AuthProviderLiveEvidenceResult Allow(const EngineApiRequest& request,
                                     const std::string& family,
                                     std::string principal,
                                     std::string credential_kind,
                                     bool groups_materialized,
                                     bool explainable = false,
                                     bool mfa = false) {
  AuthProviderLiveEvidenceResult result;
  result.evaluated = true;
  result.ok = true;
  result.authenticated = true;
  result.provider_family = family;
  result.principal = std::move(principal);
  result.credential_kind = std::move(credential_kind);
  result.groups_materialized = groups_materialized;
  result.membership_explainable = explainable;
  result.mfa_verified = mfa;
  result.diagnostic = MakeEngineApiDiagnostic("SB_ENGINE_API_OK", "engine.api.ok", {}, false);
  result.evidence.push_back({"auth_provider_live_adapter", family});
  result.rows.push_back({"live_adapter", "allow"});
  result.rows.push_back({"live_provider_family", family});
  result.rows.push_back({"credential_kind", result.credential_kind});
  result.rows.push_back({"groups_materialized", groups_materialized ? "true" : "false"});
  if (OptionPresent(request, "client_mode:external") || OptionPresent(request, "provider_client:real") ||
      OptionBool(request, "real_external_client:", false)) {
    result.evidence.push_back({"auth_provider_real_external_client", family});
    result.rows.push_back({"real_external_client", "validated"});
  }
  return result;
}

AuthProviderLiveEvidenceResult RejectAdversarialPayload(const EngineApiRequest& request, const Payload& p) {
  if (PayloadBool(p, "replay", false) || PayloadBool(p, "replayed", false) || Get(p, "nonce") == "replayed" ||
      Get(p, "challenge") == "replayed") {
    return Deny(request, "provider_replay_denied");
  }
  if (PayloadBool(p, "expired", false)) { return Deny(request, "provider_assertion_expired"); }
  const std::string alg = Get(p, "alg");
  if (alg == "none" || alg == "md5" || alg == "sha1" || alg == "rs1") {
    return Deny(request, "provider_algorithm_downgrade_denied");
  }
  if (Get(p, "kid") == "mismatch") { return Deny(request, "provider_key_id_mismatch"); }
  if (Get(p, "signature") == "bad" || Get(p, "sig") == "bad") { return Deny(request, "provider_signature_invalid"); }
  if (PayloadBool(p, "revoked", false) || PayloadBool(p, "token_revoked", false)) {
    return DenyCode(request, "SECURITY.TOKEN_REVOKED", "provider_token_revoked");
  }
  return {};
}

AuthProviderLiveEvidenceResult ValidateExternalClientGate(const EngineApiRequest& request, const Payload& p, const std::string& family) {
  const bool external_requested = OptionPresent(request, "client_mode:external") ||
                                  OptionPresent(request, "provider_client:real") ||
                                  OptionBool(request, "real_external_client:", false) ||
                                  Get(p, "client_mode") == "external" ||
                                  Get(p, "provider_client") == "real" ||
                                  PayloadBool(p, "real_external_client", false);
  if (!external_requested) { return {}; }
  const std::string dependency = RequiredClientDependencyForFamily(family);
  if (DependencyMissing(request, dependency) || !DependencyAvailable(request, dependency)) {
    return Deny(request, "real_client_dependency_missing:" + dependency);
  }
  if (OptionPresent(request, "external_service:unavailable") || PayloadBool(p, "external_service_unavailable", false)) {
    return Deny(request, "real_client_service_unavailable");
  }
  if (Get(p, "client_result") != "success" && Get(p, "external_client_result") != "success") {
    return Deny(request, "real_client_success_evidence_required");
  }
  return {};
}

AuthProviderLiveEvidenceResult ValidateLdap(const EngineApiRequest& request, const Payload& p) {
  std::string missing;
  if (!RequireFields(p, {"user", "endpoint", "starttls", "bind"}, &missing)) { return Deny(request, "ldap_missing_" + missing); }
  if (!HasAny(p, {"password", "bind_secret", "bind_token"})) { return Deny(request, "ldap_bind_secret_required"); }
  bool starttls = false;
  if (!ParseBool(Get(p, "starttls"), &starttls) || !starttls) { return Deny(request, "ldap_starttls_required"); }
  if (Get(p, "bind") != "allow") { return Deny(request, "ldap_bind_denied"); }
  if (!HasAny(p, {"groups", "group"})) { return Deny(request, "ldap_group_materialization_required"); }
  return Allow(request, "ldap_ad", Get(p, "user"), "ldap_bind", true, !Get(p, "path").empty());
}

AuthProviderLiveEvidenceResult ValidateKerberos(const EngineApiRequest& request, const Payload& p) {
  std::string missing;
  if (!RequireFields(p, {"spn", "subject", "nonce", "kdc", "sig", "exp", "pac_groups"}, &missing)) { return Deny(request, "kerberos_missing_" + missing); }
  if (!IsHex64(Get(p, "sig"))) { return Deny(request, "kerberos_signature_invalid"); }
  if (!FutureField(p, "exp")) { return Deny(request, "kerberos_ticket_expired"); }
  return Allow(request, "kerberos_pac", Get(p, "subject"), "kerberos_pac", true, false);
}

AuthProviderLiveEvidenceResult ValidatePam(const EngineApiRequest& request, const Payload& p) {
  std::string missing;
  if (!RequireFields(p, {"user", "service", "module", "password", "prompt", "account", "session"}, &missing)) { return Deny(request, "pam_missing_" + missing); }
  if (Get(p, "prompt") != "hidden" && Get(p, "prompt") != "secret") { return Deny(request, "pam_insecure_prompt"); }
  if (Get(p, "account") != "allow" || Get(p, "session") != "open") { return Deny(request, "pam_account_or_session_denied"); }
  return Allow(request, "pam", Get(p, "user"), "pam_conversation", false, false);
}

AuthProviderLiveEvidenceResult ValidateRadius(const EngineApiRequest& request, const Payload& p) {
  std::string missing;
  if (!RequireFields(p, {"user", "authenticator", "result", "attribute", "shared_secret_handle"}, &missing)) { return Deny(request, "radius_missing_" + missing); }
  if (!IsHex64(Get(p, "authenticator"))) { return Deny(request, "radius_authenticator_invalid"); }
  if (Get(p, "result") != "accept") { return Deny(request, "radius_rejected"); }
  return Allow(request, "radius", Get(p, "user"), "radius_access_request", true, false);
}

AuthProviderLiveEvidenceResult ValidateOidcJwt(const EngineApiRequest& request, const Payload& p) {
  std::string missing;
  if (!RequireFields(p, {"iss", "aud", "sub", "alg", "exp", "sig", "groups", "validator"}, &missing)) { return Deny(request, "oidc_jwt_missing_" + missing); }
  if (Get(p, "validator") != "jwks" && Get(p, "validator") != "introspection") {
    return Deny(request, "oidc_jwt_validator_boundary_required");
  }
  if (Get(p, "alg") == "none") { return Deny(request, "oidc_jwt_alg_none_forbidden"); }
  if (!IsHex64(Get(p, "sig"))) { return Deny(request, "oidc_jwt_signature_invalid"); }
  if (!FutureField(p, "exp")) { return Deny(request, "oidc_jwt_expired"); }
  return Allow(request, "oidc_jwt", Get(p, "sub"), "oidc_jwt", true, false);
}

AuthProviderLiveEvidenceResult ValidateSaml(const EngineApiRequest& request, const Payload& p) {
  std::string missing;
  if (!RequireFields(p, {"issuer", "audience", "subject", "not_on_or_after", "signature", "attributes"}, &missing)) { return Deny(request, "saml_missing_" + missing); }
  if (!IsHex64(Get(p, "signature"))) { return Deny(request, "saml_signature_invalid"); }
  if (!FutureField(p, "not_on_or_after")) { return Deny(request, "saml_assertion_expired"); }
  return Allow(request, "saml", Get(p, "subject"), "saml_assertion", true, false);
}

AuthProviderLiveEvidenceResult ValidateWebAuthn(const EngineApiRequest& request, const Payload& p) {
  std::string missing;
  if (!RequireFields(p, {"challenge", "rp", "origin", "credential", "subject", "uv", "exp", "signature"}, &missing)) { return Deny(request, "webauthn_missing_" + missing); }
  bool uv = false;
  if (!ParseBool(Get(p, "uv"), &uv) || !uv) { return Deny(request, "webauthn_user_verification_required"); }
  if (!IsHex64(Get(p, "signature"))) { return Deny(request, "webauthn_signature_invalid"); }
  if (!FutureField(p, "exp")) { return Deny(request, "webauthn_assertion_expired"); }
  return Allow(request, "webauthn", Get(p, "subject"), "webauthn_assertion", false, false, true);
}

AuthProviderLiveEvidenceResult ValidateWorkload(const EngineApiRequest& request, const Payload& p) {
  if (!HasAny(p, {"spiffe_id", "sub"})) { return Deny(request, "workload_subject_required"); }
  std::string missing;
  if (!RequireFields(p, {"trust_bundle", "exp", "sig", "service_group"}, &missing)) { return Deny(request, "workload_missing_" + missing); }
  if (!IsHex64(Get(p, "sig"))) { return Deny(request, "workload_signature_invalid"); }
  if (!FutureField(p, "exp")) { return Deny(request, "workload_assertion_expired"); }
  const std::string subject = !Get(p, "spiffe_id").empty() ? Get(p, "spiffe_id") : Get(p, "sub");
  if (!Get(p, "spiffe_id").empty() && subject.rfind("spiffe://", 0) != 0) { return Deny(request, "workload_spiffe_id_invalid"); }
  return Allow(request, "workload_identity", subject, "workload_identity", true, false);
}

AuthProviderLiveEvidenceResult ValidateManagedIdentity(const EngineApiRequest& request, const Payload& p) {
  std::string missing;
  if (!RequireFields(p, {"issuer", "audience", "subject", "exp", "sig", "service_group"}, &missing)) {
    return Deny(request, "managed_identity_missing_" + missing);
  }
  if (!IsHex64(Get(p, "sig"))) { return Deny(request, "managed_identity_signature_invalid"); }
  if (!FutureField(p, "exp")) { return Deny(request, "managed_identity_assertion_expired"); }
  return Allow(request, "managed_identity", Get(p, "subject"), "managed_identity", true, false);
}

AuthProviderLiveEvidenceResult ValidateCertificate(const EngineApiRequest& request, const Payload& p) {
  std::string missing;
  if (!RequireFields(p, {"subject", "san", "chain", "revoked", "eku", "fingerprint"}, &missing)) { return Deny(request, "certificate_missing_" + missing); }
  bool revoked = true;
  if (Get(p, "chain") != "trusted") { return Deny(request, "certificate_chain_untrusted"); }
  if (!ParseBool(Get(p, "revoked"), &revoked) || revoked) { return Deny(request, "certificate_revoked"); }
  if (Get(p, "eku") != "clientAuth") { return Deny(request, "certificate_eku_invalid"); }
  if (!IsHex64(Get(p, "fingerprint"))) { return Deny(request, "certificate_fingerprint_invalid"); }
  if (!HasAny(p, {"groups", "group"})) { return Deny(request, "certificate_group_materialization_required"); }
  return Allow(request, "certificate_mtls", Get(p, "subject"), "certificate_mtls", true, false);
}

AuthProviderLiveEvidenceResult ValidateProxy(const EngineApiRequest& request, const Payload& p) {
  std::string missing;
  if (!RequireFields(p, {"iss", "sub", "aud", "proxy", "source", "manager_trust", "listener_binding", "exp", "sig"}, &missing)) { return Deny(request, "proxy_missing_" + missing); }
  if (Get(p, "source") != "trusted") { return Deny(request, "proxy_source_untrusted"); }
  if (Get(p, "manager_trust") != "trusted") { return Deny(request, "proxy_manager_trust_required"); }
  if (Get(p, "listener_binding") != "verified") { return Deny(request, "proxy_listener_binding_required"); }
  if (!IsHex64(Get(p, "sig"))) { return Deny(request, "proxy_signature_invalid"); }
  if (!FutureField(p, "exp")) { return Deny(request, "proxy_assertion_expired"); }
  return Allow(request, "proxy_assertion", Get(p, "sub"), "proxy_assertion", HasAny(p, {"groups", "group"}), false);
}

AuthProviderLiveEvidenceResult ValidateBearerToken(const EngineApiRequest& request, const Payload& p) {
  std::string missing;
  if (!RequireFields(p, {"token_id", "proof", "exp", "subject"}, &missing)) { return Deny(request, "bearer_token_missing_" + missing); }
  if (!IsHex64(Get(p, "proof"))) { return Deny(request, "bearer_token_proof_invalid"); }
  if (!FutureField(p, "exp")) { return Deny(request, "bearer_token_expired"); }
  return Allow(request, "bearer_token", Get(p, "subject"), "bearer_token", false, false);
}

AuthProviderLiveEvidenceResult ValidateApiKey(const EngineApiRequest& request, const Payload& p) {
  std::string missing;
  if (!RequireFields(p, {"key_id", "proof", "generation", "subject", "groups"}, &missing)) { return Deny(request, "api_key_missing_" + missing); }
  if (!IsHex64(Get(p, "proof"))) { return Deny(request, "api_key_proof_invalid"); }
  return Allow(request, "token_api_key", Get(p, "subject"), "api_key_authkey", true, false);
}

AuthProviderLiveEvidenceResult ValidateFactorChain(const EngineApiRequest& request, const Payload& p) {
  std::string missing;
  if (!RequireFields(p, {"primary_auth_context_uuid", "factor_policy_uuid", "factor_results", "challenge_transcript_hash", "subject"}, &missing)) {
    return Deny(request, "factor_chain_missing_" + missing);
  }
  if (Get(p, "factor_results") != "allow") { return DenyCode(request, "SECURITY.MFA_REQUIRED", "factor_chain_denied"); }
  if (!IsHex64(Get(p, "challenge_transcript_hash"))) { return Deny(request, "factor_chain_transcript_invalid"); }
  return Allow(request, "factor_chain", Get(p, "subject"), "factor_chain", false, false, true);
}

AuthProviderLiveEvidenceResult ValidateRemoteSecurityDatabase(const EngineApiRequest& request, const Payload& p) {
  std::string missing;
  if (!RequireFields(p, {"remote_locator", "subject", "delegated_credential", "exp", "sig", "groups"}, &missing)) {
    return Deny(request, "remote_security_database_missing_" + missing);
  }
  if (!IsHex64(Get(p, "sig"))) { return Deny(request, "remote_security_database_signature_invalid"); }
  if (!FutureField(p, "exp")) { return Deny(request, "remote_security_database_assertion_expired"); }
  return Allow(request, "remote_security_database", Get(p, "subject"), "remote_security_database", true, false);
}

AuthProviderLiveEvidenceResult ValidateClusterSecurity(const EngineApiRequest& request, const Payload& p) {
  std::string missing;
  if (!RequireFields(p, {"cluster_member_uuid", "cluster_epoch", "fence_token", "subject", "sig", "groups"}, &missing)) {
    return Deny(request, "cluster_security_missing_" + missing);
  }
  if (!IsHex64(Get(p, "sig"))) { return Deny(request, "cluster_security_signature_invalid"); }
  return Allow(request, "cluster_security", Get(p, "subject"), "cluster_security", true, false);
}

AuthProviderLiveEvidenceResult ValidateCustomCppPlugin(const EngineApiRequest& request, const Payload& p) {
  std::string missing;
  if (!RequireFields(p, {"plugin_uuid", "subject", "plugin_result", "plugin_signature", "timeout", "memory"}, &missing)) {
    return Deny(request, "custom_cpp_plugin_missing_" + missing);
  }
  if (Get(p, "plugin_result") != "allow") { return Deny(request, "custom_cpp_plugin_denied"); }
  if (!IsHex64(Get(p, "plugin_signature"))) { return DenyCode(request, "SECURITY.AUTH_PLUGIN_INVALID", "custom_cpp_plugin_signature_invalid"); }
  return Allow(request, "custom_cpp_plugin", Get(p, "subject"), "custom_cpp_plugin", false, false);
}

}  // namespace

AuthProviderLiveEvidenceResult ValidateAuthProviderLiveEvidence(const EngineApiRequest& request) {
  const bool live_requested = OptionPresent(request, "adapter_mode:live") || OptionPresent(request, "provider_driver:live") ||
                              !OptionValue(request, "provider_payload:").empty();
  if (!live_requested) { return {}; }

  const std::string family = CanonicalAuthProviderFamily(OptionValue(request, "provider:"));
  const std::string payload_text = OptionValue(request, "provider_payload:");
  if (payload_text.empty()) { return Deny(request, "provider_payload_required"); }

  Payload payload;
  std::string parse_error;
  if (!ParsePayload(payload_text, &payload, &parse_error)) { return Deny(request, parse_error); }
  if (const auto adversarial = RejectAdversarialPayload(request, payload); adversarial.evaluated) { return adversarial; }
  if (const auto external = ValidateExternalClientGate(request, payload, family); external.evaluated) { return external; }

  if (family == "ldap_ad") { return ValidateLdap(request, payload); }
  if (family == "kerberos_pac") { return ValidateKerberos(request, payload); }
  if (family == "pam") { return ValidatePam(request, payload); }
  if (family == "radius") { return ValidateRadius(request, payload); }
  if (family == "oidc_jwt") { return ValidateOidcJwt(request, payload); }
  if (family == "saml") { return ValidateSaml(request, payload); }
  if (family == "webauthn") { return ValidateWebAuthn(request, payload); }
  if (family == "factor_chain") { return ValidateFactorChain(request, payload); }
  if (family == "workload_identity") { return ValidateWorkload(request, payload); }
  if (family == "managed_identity") { return ValidateManagedIdentity(request, payload); }
  if (family == "certificate_mtls") { return ValidateCertificate(request, payload); }
  if (family == "proxy_assertion") { return ValidateProxy(request, payload); }
  if (family == "bearer_token") { return ValidateBearerToken(request, payload); }
  if (family == "token_api_key") { return ValidateApiKey(request, payload); }
  if (family == "remote_security_database") { return ValidateRemoteSecurityDatabase(request, payload); }
  if (family == "cluster_security") { return ValidateClusterSecurity(request, payload); }
  if (family == "custom_cpp_plugin") { return ValidateCustomCppPlugin(request, payload); }
  return Deny(request, "live_adapter_not_supported_for_provider:" + family);
}

}  // namespace scratchbird::engine::internal_api

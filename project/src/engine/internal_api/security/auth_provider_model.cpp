// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "security/auth_provider_model.hpp"

#include "behavior_support/api_behavior_store.hpp"
#include "metric_producer.hpp"
#include "security/auth_provider_live_adapter.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <set>
#include <sstream>
#include <utility>

namespace scratchbird::engine::internal_api {
namespace {

// SEARCH_KEY: SB_ENGINE_INTERNAL_API_AUTH_PROVIDER_MODEL_BEHAVIOR
bool StartsWith(const std::string& value, const std::string& prefix) { return value.rfind(prefix, 0) == 0; }

std::string Lower(std::string value) {
  for (char& c : value) { c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); }
  return value;
}

struct AuthFamilyRegistryEntry {
  const char* family;
  bool supports_authn;
  bool validator_only;
  bool forbidden;
  bool policy_gated;
  bool guarded;
  bool channel_binding_required;
  bool live_evidence_required;
  bool group_materialization_required;
  bool dependency_required;
  const char* dependency;
  const char* policy_option;
  const char* fail_closed_code;
  const char* fail_closed_detail;
};

constexpr std::array<AuthFamilyRegistryEntry, 29> kAuthFamilyRegistry = {{
    {"local_password", true, false, false, false, false, false, false, false, false, "", "", "SECURITY.CREDENTIAL_INVALID", "credential_invalid"},
    {"password_compat", true, false, false, true, false, true, false, false, false, "", "allow_password_compat", "SECURITY.AUTH_PROVIDER_UNSUPPORTED", "password_compat_policy_required"},
    {"scram", true, false, false, false, false, true, false, false, false, "", "", "SECURITY.CHANNEL_BINDING_REQUIRED", "scram_channel_binding_required"},
    {"scram_sha256", true, false, false, false, false, true, false, false, false, "", "", "SECURITY.CHANNEL_BINDING_REQUIRED", "scram_channel_binding_required"},
    {"scram_sha512", true, false, false, false, true, true, false, false, false, "", "", "SECURITY.AUTH_METHOD_UNSUPPORTED", "scram_sha512_guarded_until_verifier_storage_and_tests"},
    {"internal_server_authority", true, false, false, false, false, false, false, false, false, "", "", "SECURITY.NO_AUTHORITY", "server_authority_evidence_required"},
    {"remote_security_database", true, false, false, false, false, true, true, true, true, "remote_scratchbird_security", "", "SECURITY.AUTH_SOURCE_UNAVAILABLE", "remote_security_database_unavailable"},
    {"cluster_security", true, false, false, false, false, true, true, true, false, "", "", "PROCESS.CLUSTER_PATH_ABSENT", "cluster_security_authority_unavailable"},
    {"peer", true, false, false, false, false, false, false, false, false, "", "", "SECURITY.CREDENTIAL_INVALID", "os_peer_credential_verification_required"},
    {"ident", true, false, false, false, false, false, false, false, false, "", "", "SECURITY.CREDENTIAL_INVALID", "os_peer_credential_verification_required"},
    {"certificate_mtls", true, false, false, false, false, true, true, true, true, "tls_x509", "", "SECURITY.CREDENTIAL_INVALID", "certificate_evidence_required"},
    {"ldap_ad", true, false, false, false, false, false, true, true, true, "ldap_client", "", "SECURITY.AUTH_SOURCE_UNAVAILABLE", "ldap_starttls_verifier_required"},
    {"kerberos_pac", true, false, false, false, false, true, true, true, true, "gssapi_krb5", "", "SECURITY.AUTH_SOURCE_UNAVAILABLE", "kerberos_gssapi_verifier_required"},
    {"pam", true, false, false, true, false, false, true, false, true, "pam", "pam_policy_enabled", "SECURITY.AUTH_SOURCE_UNAVAILABLE", "pam_policy_and_dependency_required"},
    {"oidc_jwt", true, false, false, false, false, false, true, true, true, "oidc_jwt_client", "", "SECURITY.AUTH_SOURCE_UNAVAILABLE", "oidc_jwt_validator_required"},
    {"oauth_validator", false, true, false, false, false, false, true, true, true, "oidc_jwt_client", "", "SECURITY.AUTH_PROVIDER_UNSUPPORTED", "oauth_validator_is_validator_only_not_login"},
    {"saml", true, false, false, false, false, false, true, true, true, "saml_xmlsig", "", "SECURITY.AUTH_SOURCE_UNAVAILABLE", "saml_validator_required"},
    {"webauthn", true, false, false, true, false, false, true, false, true, "webauthn_fido2", "webauthn_policy_enabled", "SECURITY.MFA_REQUIRED", "webauthn_policy_and_mfa_required"},
    {"factor_chain", true, false, false, true, false, false, true, false, true, "webauthn_fido2", "factor_chain_policy_enabled", "SECURITY.MFA_REQUIRED", "factor_chain_policy_required"},
    {"radius", true, false, false, true, false, false, true, true, true, "radius_client", "radius_policy_enabled", "SECURITY.AUTH_SOURCE_UNAVAILABLE", "radius_policy_and_dependency_required"},
    {"proxy_assertion", true, false, false, true, false, true, true, true, true, "proxy_assertion_verifier", "proxy_assertion_policy_enabled", "SECURITY.CHANNEL_BINDING_REQUIRED", "proxy_assertion_policy_and_channel_binding_required"},
    {"bearer_token", true, false, false, false, false, false, true, false, false, "", "", "SECURITY.CREDENTIAL_INVALID", "bearer_token_evidence_required"},
    {"token_api_key", true, false, false, false, false, false, true, true, false, "", "", "SECURITY.CREDENTIAL_INVALID", "token_api_key_evidence_required"},
    {"security_database_temporary_token", true, false, false, false, false, false, false, false, false, "", "", "SECURITY.CREDENTIAL_INVALID", "security_database_temporary_token_required"},
    {"token_refresh_reauth", true, false, false, true, false, true, false, false, false, "", "token_refresh_policy_enabled", "SECURITY.REAUTH_REQUIRED", "engine_reauth_envelope_required"},
    {"workload_identity", true, false, false, false, false, false, true, true, true, "spiffe_svid_or_workload_oidc", "", "SECURITY.AUTH_SOURCE_UNAVAILABLE", "workload_identity_validator_required"},
    {"managed_identity", true, false, false, false, false, false, true, true, true, "spiffe_svid_or_workload_oidc", "", "SECURITY.AUTH_SOURCE_UNAVAILABLE", "managed_identity_validator_required"},
    {"custom_cpp_plugin", true, false, false, true, true, false, true, false, false, "", "custom_cpp_plugin_policy_enabled", "SECURITY.AUTH_PLUGIN_INVALID", "custom_cpp_plugin_registration_required"},
    {"trust_reject", false, false, true, false, false, false, false, false, false, "", "", "SECURITY.AUTH_PROVIDER_UNSUPPORTED", "trust_reject_forbidden"},
}};

const AuthFamilyRegistryEntry* RegistryEntryForFamily(const std::string& provider_family);

std::set<std::string> KnownFamilies() {
  std::set<std::string> families;
  for (const auto& entry : kAuthFamilyRegistry) { families.insert(entry.family); }
  return families;
}

std::string BaseFamily(const std::string& family) {
  if (family == "password_compat") { return "local_password"; }
  if (family == "scram_sha256" || family == "scram_sha512") { return "scram"; }
  if (family == "ident") { return "peer"; }
  if (family == "oauth_validator") { return "oidc_jwt"; }
  if (family == "factor_chain") { return "webauthn"; }
  if (family == "managed_identity") { return "workload_identity"; }
  if (family == "token_refresh_reauth") { return "token_api_key"; }
  return family;
}

const AuthFamilyRegistryEntry* RegistryEntryForFamily(const std::string& provider_family) {
  const std::string family = CanonicalAuthProviderFamily(provider_family);
  for (const auto& entry : kAuthFamilyRegistry) {
    if (family == entry.family) { return &entry; }
  }
  return nullptr;
}

bool HasAdmin(const EngineRequestContext& context) {
  return SecurityContextHasRight(context, "AUTH_PROVIDER_ADMIN") || SecurityContextHasTag(context, "security.bootstrap");
}

AuthProviderDecision Fail(const EngineApiRequest& request, std::string code, std::string detail) {
  AuthProviderDecision decision;
  decision.ok = false;
  decision.provider_family = CanonicalAuthProviderFamily(AuthProviderOptionValue(request, "provider:"));
  decision.decision = "deny";
  decision.diagnostic = MakeSecurityDiagnostic(std::move(code), std::move(detail));
  if (decision.diagnostic.code.rfind("SECURITY.AUTHENTICATION", 0) == 0) {
    (void)scratchbird::core::metrics::IncrementCounter(
        "sb_auth_failures_total",
        scratchbird::core::metrics::Labels({{"component", "security.auth"}, {"provider_family", decision.provider_family.empty() ? "unknown" : decision.provider_family},
                                            {"reason", decision.diagnostic.detail.empty() ? decision.diagnostic.code : decision.diagnostic.detail}}),
        1.0,
        "security_auth");
  }
  decision.rows.push_back({"decision", "deny"});
  return decision;
}

AuthProviderDecision Ok(const EngineApiRequest& request, std::string decision_name) {
  AuthProviderDecision decision;
  decision.ok = true;
  decision.provider_family = CanonicalAuthProviderFamily(AuthProviderOptionValue(request, "provider:"));
  if (decision.provider_family.empty()) { decision.provider_family = CanonicalAuthProviderFamily(AuthProviderOptionValue(request, "provider_family:")); }
  decision.decision = std::move(decision_name);
  decision.diagnostic = MakeEngineApiDiagnostic("SB_ENGINE_API_OK", "engine.api.ok", {}, false);
  return decision;
}

bool HasFixtureSuccess(const EngineApiRequest& request) {
  return AuthProviderOptionPresent(request, "fixture:success") ||
         AuthProviderOptionPresent(request, "credential:valid") ||
         AuthProviderOptionBool(request, "credential_verifier_match:", false) ||
         AuthProviderOptionBool(request, "fixture_success:", false);
}

bool HasFreshness(const EngineApiRequest& request) {
  return !AuthProviderOptionPresent(request, "freshness:stale") &&
         !AuthProviderOptionBool(request, "expired:", false) &&
         !AuthProviderOptionBool(request, "replayed:", false);
}

bool HasVerifiedOsPeerCredential(const EngineApiRequest& request) {
  return AuthProviderOptionBool(request, "os_peer_credential_verified:", false) ||
         AuthProviderOptionBool(request, "so_peercred_verified:", false) ||
         AuthProviderOptionBool(request, "getpeereid_verified:", false) ||
         AuthProviderOptionBool(request, "ucred_verified:", false);
}

bool HasVerifiedChannelBinding(const EngineApiRequest& request) {
  const std::string tls_status = Lower(AuthProviderOptionValue(request, "tls_channel_binding_status:"));
  return AuthProviderOptionBool(request, "channel_binding_verified:", false) ||
         AuthProviderOptionBool(request, "tls_channel_binding_verified:", false) ||
         AuthProviderOptionPresent(request, "channel_binding:verified") ||
         AuthProviderOptionPresent(request, "tls_channel_binding:verified") ||
         !AuthProviderOptionValue(request, "channel_binding_hash:").empty() ||
         tls_status == "verified" || tls_status == "ok" || tls_status == "matched";
}

bool HasEngineReauthEnvelope(const EngineApiRequest& request) {
  const std::string envelope = Lower(AuthProviderOptionValue(request, "reauth_envelope:"));
  return envelope == "engine" || envelope == "engine_owned" ||
         AuthProviderOptionBool(request, "engine_reauth_envelope:", false) ||
         AuthProviderOptionPresent(request, "engine_reauth_envelope:present") ||
         AuthProviderOptionPresent(request, "reauth_envelope:engine_owned");
}

bool MaterializationRequired(const std::string& family) {
  const auto* entry = RegistryEntryForFamily(family);
  return entry != nullptr && entry->group_materialization_required;
}

void AddRow(AuthProviderDecision* decision, std::string key, std::string value) {
  decision->rows.push_back({std::move(key), std::move(value)});
}

std::string RequiredClientDependencyForFamily(const std::string& family) {
  if (family == "ldap_ad") { return "ldap_client"; }
  if (family == "kerberos_pac") { return "gssapi_krb5"; }
  if (family == "pam") { return "pam"; }
  if (family == "radius") { return "radius_client"; }
  if (family == "oidc_jwt" || family == "oauth_validator") { return "oidc_jwt_client"; }
  if (family == "saml") { return "saml_xmlsig"; }
  if (family == "webauthn" || family == "factor_chain") { return "webauthn_fido2"; }
  if (family == "workload_identity" || family == "managed_identity") { return "spiffe_svid_or_workload_oidc"; }
  if (family == "certificate_mtls") { return "tls_x509"; }
  if (family == "proxy_assertion") { return "proxy_assertion_verifier"; }
  if (family == "remote_security_database") { return "remote_scratchbird_security"; }
  return {};
}

std::string ParserPolicyRequiredProvider(const EngineApiRequest& request) {
  std::string provider = AuthProviderOptionValue(request, "parser_policy_requires_provider:");
  if (provider.empty()) { provider = AuthProviderOptionValue(request, "parser_policy_provider:"); }
  if (provider.empty()) { provider = AuthProviderOptionValue(request, "parser_auth_provider:"); }
  return CanonicalAuthProviderFamily(provider);
}

bool ParserProviderAttachmentAttempted(const EngineApiRequest& request) {
  return AuthProviderOptionBool(request, "parser_attach_provider:", false) ||
         AuthProviderOptionBool(request, "parser_connection_auth:", false) ||
         AuthProviderOptionPresent(request, "parser_attachment:attempt") ||
         AuthProviderOptionPresent(request, "parser_provider:attach");
}

bool DependencyAvailable(const EngineApiRequest& request, const std::string& dependency) {
  if (dependency.empty()) { return true; }
  return AuthProviderOptionPresent(request, "dependency:" + dependency + ":available") ||
         AuthProviderOptionPresent(request, "client_dependency:" + dependency + ":available") ||
         AuthProviderOptionPresent(request, "provider_dependency:" + dependency + ":available") ||
         AuthProviderOptionValue(request, "dependency_available:") == dependency;
}

bool DependencyMissing(const EngineApiRequest& request, const std::string& dependency) {
  return AuthProviderOptionBool(request, "missing_dependency:", false) ||
         AuthProviderOptionPresent(request, "library:missing") ||
         AuthProviderOptionPresent(request, "dependency:" + dependency + ":missing") ||
         AuthProviderOptionPresent(request, "client_dependency:" + dependency + ":missing") ||
         AuthProviderOptionPresent(request, "provider_dependency:" + dependency + ":missing");
}

bool PolicyGateSatisfied(const EngineApiRequest& request, const AuthFamilyRegistryEntry& entry) {
  if (!entry.policy_gated) { return true; }
  const std::string family(entry.family);
  if (AuthProviderOptionBool(request, "provider_policy_enabled:", false) ||
      AuthProviderOptionBool(request, "policy_admits_provider:", false) ||
      AuthProviderOptionBool(request, "policy_enabled:", false) ||
      AuthProviderOptionBool(request, "policy_enabled:" + family + ":", false) ||
      AuthProviderOptionPresent(request, "policy:" + family + ":enabled")) {
    return true;
  }
  if (entry.policy_option[0] != '\0' &&
      AuthProviderOptionBool(request, std::string(entry.policy_option) + ":", false)) {
    return true;
  }
  return false;
}

bool GuardedMethodReady(const EngineApiRequest& request, const std::string& family) {
  if (family == "scram_sha512") {
    return (AuthProviderOptionPresent(request, "scram_sha512_verifier_storage:available") ||
            AuthProviderOptionBool(request, "scram_sha512_verifier_storage_available:", false)) &&
           (AuthProviderOptionPresent(request, "scram_sha512_tests:present") ||
            AuthProviderOptionBool(request, "scram_sha512_tests_present:", false));
  }
  if (family == "custom_cpp_plugin") {
    return AuthProviderOptionBool(request, "custom_cpp_plugin_registered:", false) &&
           AuthProviderOptionBool(request, "plugin_signature_valid:", true) &&
           AuthProviderOptionBool(request, "plugin_trusted:", false) &&
           AuthProviderOptionPresent(request, "plugin_timeout_policy:present") &&
           AuthProviderOptionPresent(request, "plugin_memory_policy:present");
  }
  return true;
}

std::uint64_t AuthProviderOptionU64(const EngineApiRequest& request,
                                    const std::string& prefix,
                                    std::uint64_t fallback) {
  const std::string value = AuthProviderOptionValue(request, prefix);
  if (value.empty()) { return fallback; }
  try {
    return static_cast<std::uint64_t>(std::stoull(value));
  } catch (...) {
    return fallback;
  }
}

AuthProviderDecision ValidateParserPolicyAttachment(const EngineApiRequest& request, const std::string& family) {
  const std::string parser_required = ParserPolicyRequiredProvider(request);
  const bool attachment_attempted = ParserProviderAttachmentAttempted(request);
  if (!attachment_attempted && parser_required.empty()) { return Ok(request, "parser_policy_not_applicable"); }
  if (parser_required.empty()) {
    return Fail(request, "SECURITY.AUTHENTICATION.REQUEST_INVALID", "parser_policy_provider_required_for_attachment");
  }
  if (parser_required != family) {
    return Fail(request, "SECURITY.AUTHENTICATION.REQUEST_INVALID",
                "parser_policy_provider_mismatch:" + parser_required + "!=" + family);
  }
  const std::string dependency = RequiredClientDependencyForFamily(family);
  if (DependencyMissing(request, dependency) || !DependencyAvailable(request, dependency)) {
    return Fail(request, "SECURITY.AUTHORITY.INVALID", "provider_dependency_missing:" + dependency);
  }
  auto decision = Ok(request, "parser_policy_attachment_allowed");
  decision.admitted = true;
  decision.provider_family = family;
  decision.evidence.push_back({"parser_policy_auth_provider", family});
  AddRow(&decision, "parser_policy_provider", family);
  AddRow(&decision, "provider_dependency", dependency);
  AddRow(&decision, "provider_attachment", "policy_selected");
  return decision;
}

}  // namespace

std::string CanonicalAuthProviderFamily(std::string provider_family) {
  provider_family = Lower(std::move(provider_family));
  if (provider_family == "password" || provider_family == "cleartext_startup_password" ||
      provider_family == "internal.password_verifier" ||
      provider_family == "builtin.internal_password") {
    return "local_password";
  }
  if (provider_family == "internal.password_compat" || provider_family == "md5_password_compat" ||
      provider_family == "reference_password_compat" || provider_family == "builtin.password_compat") {
    return "password_compat";
  }
  if (provider_family == "internal.scram_sha256" || provider_family == "scram-sha-256" ||
      provider_family == "scram_sha256") {
    return "scram_sha256";
  }
  if (provider_family == "internal.scram_sha512" || provider_family == "scram-sha-512" ||
      provider_family == "scram_sha512") {
    return "scram_sha512";
  }
  if (provider_family == "builtin.scram") { return "scram"; }
  if (provider_family == "internal.server_authority" ||
      provider_family == "builtin.internal_server_authority") {
    return "internal_server_authority";
  }
  if (provider_family == "remote_scratchbird.security_database" ||
      provider_family == "scratchbird.remote_security_database") {
    return "remote_security_database";
  }
  if (provider_family == "cluster.security_authority" ||
      provider_family == "cluster.security_provider") {
    return "cluster_security";
  }
  if (provider_family == "os.peer_credential" || provider_family == "os_identity" ||
      provider_family == "os.identity") {
    return "peer";
  }
  if (provider_family == "ldap" || provider_family == "ldap.bind" ||
      provider_family == "ldap.starttls_bind" ||
      provider_family == "directory.ldap" ||
      provider_family == "directory.ldap_ad_starttls") {
    return "ldap_ad";
  }
  if (provider_family == "kerberos" || provider_family == "kerberos.ticket" ||
      provider_family == "kerberos.gssapi_ticket" ||
      provider_family == "directory.kerberos_gssapi" ||
      provider_family == "gssapi") {
    return "kerberos_pac";
  }
  if (provider_family == "jwt" || provider_family == "oidc" ||
      provider_family == "oauth_bearer" || provider_family == "bearer.jwt" ||
      provider_family == "bearer.opaque_token" || provider_family == "oauth.oidc" ||
      provider_family == "token.oidc_jwt" ||
      provider_family == "federation.oidc_jwt_validator") {
    return "oidc_jwt";
  }
  if (provider_family == "federation.oauth_validator") { return "oauth_validator"; }
  if (provider_family == "api_key" || provider_family == "token" ||
      provider_family == "token_authkey" || provider_family == "authkey" ||
      provider_family == "token.api_key" || provider_family == "token.authkey" ||
      provider_family == "token.api_key_authkey" ||
      provider_family == "token.api_key_authkey_store") {
    return "token_api_key";
  }
  if (provider_family == "bearer_token" || provider_family == "token.bearer" ||
      provider_family == "token.local_bearer_store") {
    return "bearer_token";
  }
  if (provider_family == "security_database_temporary_token" ||
      provider_family == "security_database.temporary_token" ||
      provider_family == "security.temporary_token" ||
      provider_family == "manager_security_token" ||
      provider_family == "manager.temporary_security_token") {
    return "security_database_temporary_token";
  }
  if (provider_family == "token_refresh" || provider_family == "reauth_token_rotation" ||
      provider_family == "token.refresh_reauth") {
    return "token_refresh_reauth";
  }
  if (provider_family == "mtls" || provider_family == "certificate" ||
      provider_family == "peer_certificate" ||
      provider_family == "tls.client_certificate" ||
      provider_family == "tls.client_certificate_store") {
    return "certificate_mtls";
  }
  if (provider_family == "saml.assertion" || provider_family == "saml.signed_assertion" ||
      provider_family == "federation.saml_validator") {
    return "saml";
  }
  if (provider_family == "radius.access_request") { return "radius"; }
  if (provider_family == "fido2" || provider_family == "webauthn.credential" ||
      provider_family == "webauthn.fido2_assertion" ||
      provider_family == "webauthn.fido2_provider") {
    return "webauthn";
  }
  if (provider_family == "mfa" || provider_family == "mfa.factor_chain" ||
      provider_family == "mfa.factor_chain_provider") {
    return "factor_chain";
  }
  if (provider_family == "spiffe" || provider_family == "spiffe_svid" ||
      provider_family == "workload.identity" ||
      provider_family == "workload.spiffe_svid" ||
      provider_family == "workload.spiffe_svid_validator") {
    return "workload_identity";
  }
  if (provider_family == "managed_identity" || provider_family == "workload_oidc" ||
      provider_family == "workload.managed_identity" ||
      provider_family == "workload.managed_identity_validator") {
    return "managed_identity";
  }
  if (provider_family == "proxy.assertion" || provider_family == "manager_bootstrap_token" ||
      provider_family == "proxy.manager_assertion" ||
      provider_family == "proxy.manager_assertion_provider") {
    return "proxy_assertion";
  }
  if (provider_family == "pam.account" || provider_family == "pam.service_conversation" ||
      provider_family == "pam.service_provider") {
    return "pam";
  }
  if (provider_family == "custom.cpp_plugin" ||
      provider_family == "plugin.custom_cpp_auth_provider") {
    return "custom_cpp_plugin";
  }
  if (provider_family == "policy.unsupported_provider_refusal" ||
      provider_family == "policy.trust_reject" ||
      provider_family == "unsupported_provider") {
    return "trust_reject";
  }
  return provider_family;
}

bool IsKnownAuthProviderFamily(const std::string& provider_family) {
  return KnownFamilies().count(CanonicalAuthProviderFamily(provider_family)) != 0;
}

bool AuthProviderFamilySupportsAuthn(const std::string& provider_family) {
  const auto* entry = RegistryEntryForFamily(provider_family);
  return entry != nullptr && entry->supports_authn && !entry->validator_only && !entry->forbidden;
}

std::string AuthProviderOptionValue(const EngineApiRequest& request, const std::string& prefix) {
  return SecurityOptionValue(request, prefix);
}

bool AuthProviderOptionBool(const EngineApiRequest& request, const std::string& prefix, bool fallback) {
  return SecurityOptionBool(request, prefix, fallback);
}

bool AuthProviderOptionPresent(const EngineApiRequest& request, const std::string& exact_value) {
  return SecurityOptionPresent(request, exact_value);
}

AuthProviderDescriptor AuthProviderDescriptorFromRequest(const EngineApiRequest& request) {
  AuthProviderDescriptor descriptor;
  descriptor.provider_uuid = request.target_object.uuid;
  descriptor.provider_family = CanonicalAuthProviderFamily(AuthProviderOptionValue(request, "provider:"));
  if (descriptor.provider_family.empty()) { descriptor.provider_family = CanonicalAuthProviderFamily(AuthProviderOptionValue(request, "provider_family:")); }
  if (descriptor.provider_family.empty()) { descriptor.provider_family = "local_password"; }
  if (descriptor.provider_uuid.canonical.empty()) { descriptor.provider_uuid.canonical = GenerateCrudEngineUuid("auth_provider"); }
  descriptor.provider_version = AuthProviderOptionValue(request, "provider_version:");
  if (descriptor.provider_version.empty()) { descriptor.provider_version = "1"; }
  descriptor.implementation_version = AuthProviderOptionValue(request, "implementation_version:");
  if (descriptor.implementation_version.empty()) { descriptor.implementation_version = "fixture_v1"; }
  descriptor.capabilities = SecurityProviderCapabilitiesFor(BaseFamily(descriptor.provider_family));
  descriptor.capabilities.provider_family = descriptor.provider_family;
  descriptor.policy_epoch = 1;
  const auto epoch = AuthProviderOptionValue(request, "policy_epoch:");
  if (!epoch.empty()) { try { descriptor.policy_epoch = static_cast<std::uint64_t>(std::stoull(epoch)); } catch (...) {} }
  descriptor.trust_state = AuthProviderOptionValue(request, "trust_state:");
  if (descriptor.trust_state.empty()) { descriptor.trust_state = "trusted"; }
  descriptor.rollout_state = AuthProviderOptionValue(request, "rollout_state:");
  if (descriptor.rollout_state.empty()) { descriptor.rollout_state = "enabled"; }
  for (const auto& option : request.option_envelopes) {
    if (StartsWith(option, "required_library:")) { descriptor.required_libraries.push_back(option.substr(17)); }
    if (StartsWith(option, "allowed_scope:")) { descriptor.allowed_policy_scopes.push_back(option.substr(14)); }
  }
  return descriptor;
}

AuthProviderPolicy AuthProviderPolicyFromRequest(const EngineApiRequest& request) {
  AuthProviderPolicy policy;
  policy.provider_family = CanonicalAuthProviderFamily(AuthProviderOptionValue(request, "provider:"));
  if (policy.provider_family.empty()) { policy.provider_family = CanonicalAuthProviderFamily(AuthProviderOptionValue(request, "provider_family:")); }
  policy.provider_uuid = request.target_object.uuid;
  if (policy.provider_uuid.canonical.empty()) { policy.provider_uuid.canonical = AuthProviderOptionValue(request, "provider_uuid:"); }
  policy.policy_uuid.canonical = AuthProviderOptionValue(request, "policy_uuid:");
  if (policy.policy_uuid.canonical.empty()) { policy.policy_uuid.canonical = GenerateCrudEngineUuid("policy"); }
  policy.enabled = AuthProviderOptionBool(request, "provider_enabled:", true) && !AuthProviderOptionPresent(request, "provider:disabled");
  policy.allow_password_compat = AuthProviderOptionBool(request, "allow_password_compat:", false);
  policy.require_mfa = AuthProviderOptionBool(request, "mfa_required:", false);
  policy.require_group_sync = AuthProviderOptionBool(request, "require_group_sync:", MaterializationRequired(policy.provider_family));
  policy.allow_cache_stale = AuthProviderOptionBool(request, "allow_cache_stale:", false);
  policy.allow_fixture = AuthProviderOptionBool(request, "allow_fixture:", false);
  policy.stale_behavior = AuthProviderOptionValue(request, "stale_behavior:");
  if (policy.stale_behavior.empty()) { policy.stale_behavior = policy.allow_cache_stale ? "allow_within_cache_bounds" : "deny"; }
  policy.group_behavior = AuthProviderOptionValue(request, "group_behavior:");
  if (policy.group_behavior.empty()) { policy.group_behavior = MaterializationRequired(policy.provider_family) ? "materialize_internal_groups" : "none"; }
  policy.cache_bounds = AuthProviderOptionValue(request, "cache_bounds:");
  if (policy.cache_bounds.empty()) { policy.cache_bounds = "deny_when_expired"; }
  policy.audit_policy_ref = AuthProviderOptionValue(request, "audit_policy_ref:");
  policy.redaction_policy_ref = AuthProviderOptionValue(request, "redaction_policy_ref:");
  return policy;
}

EngineApiDiagnostic AuthProviderNoPlaintextDiagnostic(const EngineApiRequest& request) {
  if (AuthProviderOptionPresent(request, "credential_plaintext:persist") ||
      AuthProviderOptionPresent(request, "store_plaintext:true") ||
      AuthProviderOptionBool(request, "store_reusable_plaintext:", false)) {
    return MakeSecurityDiagnostic("SECURITY.PROTECTED_MATERIAL.DENIED", "reusable_plaintext_forbidden");
  }
  return MakeEngineApiDiagnostic("SB_ENGINE_API_OK", "engine.api.ok", {}, false);
}

AuthProviderDecision AdmitAuthProvider(const EngineApiRequest& request) {
  const auto descriptor = AuthProviderDescriptorFromRequest(request);
  if (!HasAdmin(request.context)) { return Fail(request, "SECURITY.AUTHORIZATION.DENIED", "AUTH_PROVIDER_ADMIN"); }
  if (!IsKnownAuthProviderFamily(descriptor.provider_family)) {
    return Fail(request, "SECURITY.AUTHORITY.INVALID", "unknown_provider_family:" + descriptor.provider_family);
  }
  const auto* registry_entry = RegistryEntryForFamily(descriptor.provider_family);
  if (registry_entry == nullptr) {
    return Fail(request, "SECURITY.AUTH_PROVIDER_UNSUPPORTED", "provider_not_registered:" + descriptor.provider_family);
  }
  if (registry_entry->forbidden) {
    return Fail(request, registry_entry->fail_closed_code, registry_entry->fail_closed_detail);
  }
  if (descriptor.provider_family == "trust_reject" || descriptor.rollout_state == "disabled" ||
      descriptor.trust_state == "disabled" || AuthProviderOptionPresent(request, "provider:disabled")) {
    return Fail(request, "SECURITY.UDR.TRUST_DENIED", "provider_disabled_or_trust_reject");
  }
  if (AuthProviderOptionBool(request, "duplicate_provider_uuid:", false)) {
    return Fail(request, "SECURITY.AUTHORITY.INVALID", "duplicate_provider_uuid");
  }
  if (AuthProviderOptionBool(request, "stale_implementation:", false)) {
    return Fail(request, "SECURITY.AUTHORITY.INVALID", "stale_provider_implementation");
  }
  const std::string required_capability = AuthProviderOptionValue(request, "required_capability:");
  if (registry_entry->validator_only && required_capability == "authn") {
    return Fail(request, registry_entry->fail_closed_code, registry_entry->fail_closed_detail);
  }
  if (registry_entry->policy_gated && !PolicyGateSatisfied(request, *registry_entry)) {
    return Fail(request, registry_entry->fail_closed_code, registry_entry->fail_closed_detail);
  }
  if (registry_entry->guarded && !GuardedMethodReady(request, descriptor.provider_family)) {
    return Fail(request, registry_entry->fail_closed_code, registry_entry->fail_closed_detail);
  }
  if (registry_entry->dependency_required &&
      (DependencyMissing(request, registry_entry->dependency) ||
       !DependencyAvailable(request, registry_entry->dependency))) {
    return Fail(request, registry_entry->fail_closed_code,
                std::string("provider_dependency_missing:") + registry_entry->dependency);
  }
  if ((required_capability == "authn" && !descriptor.capabilities.supports_authn) ||
      (required_capability == "authz_claims" && !descriptor.capabilities.supports_authz_claims) ||
      (required_capability == "group_query" && !descriptor.capabilities.supports_group_query) ||
      (required_capability == "membership_path_explain" && !descriptor.capabilities.supports_membership_path_explain) ||
      (required_capability == "mfa" && !descriptor.capabilities.supports_mfa) ||
      (required_capability == "token_introspection" && !descriptor.capabilities.supports_token_introspection) ||
      (required_capability == "credential_rotation" && !descriptor.capabilities.supports_credential_rotation)) {
    return Fail(request, "SECURITY.AUTHORITY.INVALID", "required_capability_missing:" + required_capability);
  }
  if (AuthProviderOptionBool(request, "missing_dependency:", false) || AuthProviderOptionPresent(request, "library:missing")) {
    return Fail(request, "SECURITY.AUTHORITY.INVALID", "provider_dependency_missing");
  }
  if (AuthProviderOptionBool(request, "signature_valid:", true) == false ||
      AuthProviderOptionBool(request, "provenance_valid:", true) == false) {
    return Fail(request, "SECURITY.UDR.TRUST_DENIED", "signature_or_provenance_failed");
  }
  if (AuthProviderOptionBool(request, "abi_supported:", true) == false) {
    return Fail(request, "SECURITY.AUTHORITY.INVALID", "unsupported_provider_abi");
  }
  auto decision = Ok(request, "admit_provider");
  decision.admitted = true;
  decision.provider_family = descriptor.provider_family;
  decision.evidence.push_back({"auth_provider_admitted", descriptor.provider_uuid.canonical});
  AddRow(&decision, "provider_uuid", descriptor.provider_uuid.canonical);
  AddRow(&decision, "provider_family", descriptor.provider_family);
  AddRow(&decision, "trust_state", descriptor.trust_state);
  AddRow(&decision, "rollout_state", descriptor.rollout_state);
  return decision;
}

AuthProviderDecision EvaluateAuthProviderPolicy(const EngineApiRequest& request) {
  (void)scratchbird::core::metrics::IncrementCounter(
      "sb_policy_evaluations_total",
      scratchbird::core::metrics::Labels({{"component", "security.auth_provider"}, {"policy_family", "auth_provider"}}),
      1.0,
      "policy_runtime");
  const auto policy = AuthProviderPolicyFromRequest(request);
  const bool login_flow = AuthProviderOptionPresent(request, "auth_flow:login");
  if (!login_flow && !HasAdmin(request.context) && !SecurityContextHasRight(request.context, "CONNECT")) {
    return Fail(request, "SECURITY.AUTHORIZATION.DENIED", "provider_policy_requires_authority");
  }
  if (!policy.enabled) { return Fail(request, "SECURITY.AUTHORITY.INVALID", "provider_policy_disabled"); }
  const std::string auth_authority = AuthProviderOptionValue(request, "auth_authority:");
  if (auth_authority == "parser" || auth_authority == "driver" || auth_authority == "listener") {
    return Fail(request, "SECURITY.AUTHORITY.INVALID", "auth_authority_must_be_engine");
  }
  if (AuthProviderOptionPresent(request, "default_policy_installed:false") ||
      AuthProviderOptionBool(request, "default_policy_installed:", true) == false) {
    return Fail(request, "SECURITY.AUTHORITY.INVALID", "default_policy_not_installed");
  }
  const auto current_policy_generation =
      AuthProviderOptionU64(request, "policy_generation_current:", 1);
  const auto observed_policy_generation =
      AuthProviderOptionU64(request, "policy_generation_observed:", current_policy_generation);
  const auto current_security_epoch =
      AuthProviderOptionU64(request, "security_epoch_current:", 1);
  const auto observed_security_epoch =
      AuthProviderOptionU64(request, "security_epoch_observed:", current_security_epoch);
  const auto current_provider_generation =
      AuthProviderOptionU64(request, "provider_generation_current:", 1);
  const auto observed_provider_generation =
      AuthProviderOptionU64(request, "provider_generation_observed:", current_provider_generation);
  if (observed_policy_generation != current_policy_generation ||
      observed_security_epoch != current_security_epoch ||
      observed_provider_generation != current_provider_generation) {
    return Fail(request, "SECURITY.POLICY.STALE", "stale_policy_or_security_epoch_refused");
  }
  const std::string provider_state = AuthProviderOptionValue(request, "provider_lifecycle_state:");
  if (!provider_state.empty() && provider_state != "healthy" && provider_state != "started") {
    return Fail(request, "SECURITY.AUTHORITY.INVALID",
                "provider_state_not_admitting_authentication:" + provider_state);
  }
  if (AuthProviderOptionBool(request, "cluster_policy:", false) && !request.context.cluster_authority_available) {
    auto decision = Fail(request, "SECURITY.CLUSTER.AUTHORITY_REQUIRED", "cluster_provider_policy_unavailable");
    decision.cluster_authority_required = true;
    return decision;
  }
  auto decision = Ok(request, "allow_provider_policy");
  decision.admitted = true;
  decision.provider_family = policy.provider_family;
  decision.evidence.push_back({"auth_provider_policy", policy.policy_uuid.canonical});
  AddRow(&decision, "provider_family", policy.provider_family);
  AddRow(&decision, "stale_behavior", policy.stale_behavior);
  AddRow(&decision, "group_behavior", policy.group_behavior);
  AddRow(&decision, "cache_bounds", policy.cache_bounds);
  return decision;
}

AuthProviderDecision AuthenticateWithProvider(const EngineApiRequest& request) {
  (void)scratchbird::core::metrics::IncrementCounter(
      "sb_identity_auth_attempts_total",
      scratchbird::core::metrics::Labels({{"component", "security.auth"}, {"provider_family", CanonicalAuthProviderFamily(AuthProviderOptionValue(request, "provider:"))}}),
      1.0,
      "security_auth");
  auto policy = EvaluateAuthProviderPolicy(request);
  if (!policy.ok) { return policy; }
  const auto family = CanonicalAuthProviderFamily(AuthProviderOptionValue(request, "provider:"));
  const auto* registry_entry = RegistryEntryForFamily(family);
  if (registry_entry == nullptr) {
    return Fail(request, "SECURITY.AUTH_PROVIDER_UNSUPPORTED", "provider_not_registered:" + family);
  }
  if (registry_entry->forbidden || registry_entry->validator_only) {
    return Fail(request, registry_entry->fail_closed_code, registry_entry->fail_closed_detail);
  }
  if (!AuthProviderFamilySupportsAuthn(family)) {
    return Fail(request, "SECURITY.AUTH_PROVIDER_UNSUPPORTED", "provider_is_not_login_provider:" + family);
  }
  if (registry_entry->policy_gated && !PolicyGateSatisfied(request, *registry_entry)) {
    return Fail(request, registry_entry->fail_closed_code, registry_entry->fail_closed_detail);
  }
  if (registry_entry->guarded && !GuardedMethodReady(request, family)) {
    return Fail(request, registry_entry->fail_closed_code, registry_entry->fail_closed_detail);
  }
  if (registry_entry->dependency_required &&
      (DependencyMissing(request, registry_entry->dependency) ||
       !DependencyAvailable(request, registry_entry->dependency))) {
    return Fail(request, registry_entry->fail_closed_code,
                std::string("provider_dependency_missing:") + registry_entry->dependency);
  }
  if (family == "cluster_security" && !request.context.cluster_authority_available &&
      !AuthProviderOptionBool(request, "cluster_authority_available:", false)) {
    return Fail(request, "PROCESS.CLUSTER_PATH_ABSENT", "cluster_security_authority_unavailable");
  }
  if (registry_entry->channel_binding_required && !HasVerifiedChannelBinding(request)) {
    return Fail(request, "SECURITY.CHANNEL_BINDING_REQUIRED", "channel_binding_required:" + family);
  }
  if (family == "token_refresh_reauth" && !HasEngineReauthEnvelope(request)) {
    return Fail(request, "SECURITY.REAUTH_REQUIRED", "engine_reauth_envelope_required");
  }
  if (family == "token_refresh_reauth" &&
      !AuthProviderOptionPresent(request, "refresh_token_proof:present") &&
      !AuthProviderOptionBool(request, "refresh_token_proof_present:", false)) {
    return Fail(request, "SECURITY.CREDENTIAL_INVALID", "refresh_token_proof_required");
  }
  const auto parser_gate = ValidateParserPolicyAttachment(request, family);
  if (!parser_gate.ok) { return parser_gate; }
  const auto live = ValidateAuthProviderLiveEvidence(request);
  if (live.evaluated && !live.ok) { return Fail(request, live.diagnostic.code, live.diagnostic.detail); }
  if (registry_entry->live_evidence_required && !live.evaluated &&
      !AuthProviderOptionBool(request, "allow_fixture:", false)) {
    return Fail(request, registry_entry->fail_closed_code, registry_entry->fail_closed_detail);
  }
  std::string principal = AuthProviderOptionValue(request, "principal:");
  if (principal.empty() && live.authenticated) { principal = live.principal; }
  if (principal.empty()) { return Fail(request, "SECURITY.AUTHENTICATION.REQUEST_INVALID", "principal_claim_required"); }
  const auto plaintext = AuthProviderNoPlaintextDiagnostic(request);
  if (plaintext.error) { return Fail(request, plaintext.code, plaintext.detail); }
  if (!HasFreshness(request)) { return Fail(request, "SECURITY.AUTHENTICATION.FAILED", "stale_or_replayed_provider_evidence"); }
  if (AuthProviderOptionPresent(request, "credential:invalid") || AuthProviderOptionBool(request, "fixture_fail:", false)) {
    return Fail(request, "SECURITY.AUTHENTICATION.FAILED", "provider_fixture_failed");
  }
  if ((BaseFamily(family) == "peer") && !HasVerifiedOsPeerCredential(request)) {
    return Fail(request, "SECURITY.AUTHENTICATION.FAILED", "os_peer_credential_verification_required");
  }
  const bool fixture_success =
      HasFixtureSuccess(request) &&
      (!registry_entry->live_evidence_required ||
       AuthProviderOptionBool(request, "allow_fixture:", false));
  if (!fixture_success && !live.authenticated && BaseFamily(family) != "peer" &&
      family != "token_refresh_reauth" && family != "internal_server_authority") {
    return Fail(request, "SECURITY.AUTHENTICATION.FAILED", "credential_evidence_required");
  }
  if ((family == "scram" || family == "scram_sha256" || family == "scram_sha512") &&
      AuthProviderOptionBool(request, "downgrade_attempt:", false)) {
    return Fail(request, "SECURITY.AUTHENTICATION.FAILED", "scram_downgrade_denied");
  }
  if ((family == "webauthn" || family == "factor_chain") && !AuthProviderOptionPresent(request, "mfa:present") &&
      !live.mfa_verified) {
    return Fail(request, "SECURITY.MFA_REQUIRED", "mfa_evidence_required");
  }
  if (MaterializationRequired(family) &&
      AuthProviderOptionBool(request, "require_group_sync:", true) &&
      !AuthProviderOptionPresent(request, "groups:materialized") &&
      !AuthProviderOptionBool(request, "groups_materialized:", false) &&
      !live.groups_materialized) {
    return Fail(request, "SECURITY.GROUP.EXTERNAL_UNSYNCED", "internal_group_materialization_required");
  }
  auto decision = Ok(request, "authenticated");
  decision.authenticated = true;
  decision.provider_family = family;
  decision.principal = principal;
  decision.evidence.push_back({"auth_provider_authenticated", family});
  AddRow(&decision, "authenticated", "true");
  AddRow(&decision, "provider_family", family);
  AddRow(&decision, "principal", principal);
  AddRow(&decision, "group_authority", (family == "ldap_ad") ? "explainable" : (MaterializationRequired(family) ? "effective_materialized" : "none"));
  if (live.evaluated) {
    decision.evidence.insert(decision.evidence.end(), live.evidence.begin(), live.evidence.end());
    for (const auto& row : live.rows) { AddRow(&decision, row.first, row.second); }
  }
  return decision;
}

AuthProviderDecision ContinueAuthChallenge(const EngineApiRequest& request) {
  if (AuthProviderOptionBool(request, "challenge_expired:", false)) { return Fail(request, "SECURITY.AUTHENTICATION.FAILED", "challenge_expired"); }
  if (AuthProviderOptionBool(request, "challenge_replayed:", false)) { return Fail(request, "SECURITY.AUTHENTICATION.FAILED", "challenge_replay_denied"); }
  if (AuthProviderOptionBool(request, "attempt_limit_exceeded:", false)) { return Fail(request, "SECURITY.AUTHENTICATION.FAILED", "challenge_attempt_limit_exceeded"); }
  if (AuthProviderOptionBool(request, "wrong_connection:", false)) { return Fail(request, "SECURITY.AUTHENTICATION.FAILED", "challenge_connection_mismatch"); }
  auto decision = Ok(request, "challenge_accepted");
  decision.challenge_accepted = true;
  decision.evidence.push_back({"auth_challenge", AuthProviderOptionValue(request, "challenge_uuid:")});
  AddRow(&decision, "challenge", "accepted");
  AddRow(&decision, "ttl_enforced", "true");
  return decision;
}

AuthProviderDecision RotateAuthCredential(const EngineApiRequest& request) {
  if (!HasAdmin(request.context)) { return Fail(request, "SECURITY.AUTHORIZATION.DENIED", "AUTH_PROVIDER_ADMIN"); }
  std::string family = CanonicalAuthProviderFamily(AuthProviderOptionValue(request, "provider:"));
  if (family.empty()) { family = "local_password"; }
  if (!IsKnownAuthProviderFamily(family)) {
    return Fail(request, "SECURITY.AUTHORITY.INVALID", "unknown_provider_family:" + family);
  }
  const auto* registry_entry = RegistryEntryForFamily(family);
  if (registry_entry == nullptr || registry_entry->forbidden || registry_entry->validator_only) {
    return Fail(request,
                registry_entry == nullptr ? "SECURITY.AUTH_PROVIDER_UNSUPPORTED" : registry_entry->fail_closed_code,
                registry_entry == nullptr ? "provider_not_registered:" + family : registry_entry->fail_closed_detail);
  }
  const auto caps = SecurityProviderCapabilitiesFor(family);
  if (!caps.supports_credential_rotation) {
    return Fail(request, "SECURITY.AUTH_PROVIDER_UNSUPPORTED",
                "credential_rotation_not_supported:" + family);
  }
  if (registry_entry->policy_gated && !PolicyGateSatisfied(request, *registry_entry)) {
    return Fail(request, registry_entry->fail_closed_code, registry_entry->fail_closed_detail);
  }
  if (registry_entry->dependency_required &&
      (DependencyMissing(request, registry_entry->dependency) ||
       !DependencyAvailable(request, registry_entry->dependency))) {
    return Fail(request, registry_entry->fail_closed_code,
                std::string("provider_dependency_missing:") + registry_entry->dependency);
  }
  const auto plaintext = AuthProviderNoPlaintextDiagnostic(request);
  if (plaintext.error) { return Fail(request, plaintext.code, plaintext.detail); }
  if (!HasFreshness(request)) {
    return Fail(request, "SECURITY.AUTHENTICATION.FAILED",
                "stale_or_replayed_provider_evidence");
  }
  if (!AuthProviderOptionPresent(request, "protected_material:available") &&
      !AuthProviderOptionBool(request, "protected_material_available:", false)) {
    return Fail(request, "SECURITY.KEY.UNAVAILABLE", "protected_material_required_for_rotation");
  }
  const auto current_material_generation =
      AuthProviderOptionU64(request, "protected_material_generation_current:", 1);
  const auto observed_material_generation =
      AuthProviderOptionU64(request, "protected_material_generation_observed:",
                            current_material_generation);
  if (observed_material_generation != current_material_generation) {
    return Fail(request, "SECURITY.POLICY.STALE",
                "stale_protected_material_generation_refused");
  }
  if (AuthProviderOptionBool(request, "rotation_provider_failed:", false) ||
      AuthProviderOptionPresent(request, "rotation_result:failed")) {
    return Fail(request, "SECURITY.KEY.UNAVAILABLE",
                "credential_rotation_provider_failure");
  }
  auto decision = Ok(request, "credential_rotated");
  decision.credential_rotated = true;
  decision.provider_family = family;
  const std::string event_uuid = GenerateCrudEngineUuid("security_event");
  decision.evidence.push_back({"credential_rotation", event_uuid});
  decision.evidence.push_back({"credential_rotation_audit", event_uuid});
  AddRow(&decision, "provider_family", family);
  AddRow(&decision, "credential_rotated", "true");
  AddRow(&decision, "plaintext_persisted", "false");
  AddRow(&decision, "protected_material_generation",
         std::to_string(current_material_generation));
  AddRow(&decision, "rotation_failure_audited", "true");
  return decision;
}

AuthProviderDecision RevokeAuthToken(const EngineApiRequest& request) {
  if (!HasAdmin(request.context)) { return Fail(request, "SECURITY.AUTHORIZATION.DENIED", "AUTH_PROVIDER_ADMIN"); }
  const std::string token_uuid = AuthProviderOptionValue(request, "token_uuid:");
  if (token_uuid.empty()) { return Fail(request, "SECURITY.AUTHENTICATION.REQUEST_INVALID", "token_uuid_required"); }
  auto decision = Ok(request, "token_revoked");
  decision.token_revoked = true;
  decision.evidence.push_back({"token_revoked", token_uuid});
  AddRow(&decision, "token_uuid", token_uuid);
  AddRow(&decision, "revoked", "true");
  return decision;
}

AuthProviderDecision SyncAuthProviderGroups(const EngineApiRequest& request) {
  const std::string family = CanonicalAuthProviderFamily(AuthProviderOptionValue(request, "provider:"));
  const auto caps = SecurityProviderCapabilitiesFor(BaseFamily(family));
  const bool has_group_or_claim = caps.supports_group_query || caps.supports_authz_claims || MaterializationRequired(family);
  if (!has_group_or_claim) { return Fail(request, "SECURITY.GROUP.EXTERNAL_UNSYNCED", "provider_has_no_group_or_claim_capability:" + family); }
  const std::string external_group = AuthProviderOptionValue(request, "external_group:");
  const std::string internal_group = AuthProviderOptionValue(request, "internal_group_uuid:");
  if (external_group.empty() || internal_group.empty()) { return Fail(request, "SECURITY.GROUP.EXTERNAL_UNSYNCED", "external_group_and_internal_group_uuid_required"); }
  auto decision = Ok(request, "groups_materialized");
  decision.materialized = true;
  decision.provider_family = family;
  decision.evidence.push_back({"external_group_materialized", internal_group});
  AddRow(&decision, "provider_family", family);
  AddRow(&decision, "external_group", external_group);
  AddRow(&decision, "internal_group_uuid", internal_group);
  AddRow(&decision, "ordinary_authz_live_lookup", "false");
  return decision;
}

AuthProviderDecision ExplainAuthProviderMembership(const EngineApiRequest& request) {
  const std::string family = CanonicalAuthProviderFamily(AuthProviderOptionValue(request, "provider:"));
  const auto caps = SecurityProviderCapabilitiesFor(BaseFamily(family));
  if (!caps.supports_membership_path_explain && !AuthProviderOptionBool(request, "synchronized_graph_evidence:", false)) {
    auto decision = Fail(request, "SECURITY.GROUP.EXTERNAL_UNSYNCED", "membership_path_not_explainable:" + family);
    AddRow(&decision, "effective_only", "true");
    return decision;
  }
  auto decision = Ok(request, "membership_explainable");
  decision.explainable = true;
  decision.provider_family = family;
  decision.evidence.push_back({"membership_path_explain", family});
  AddRow(&decision, "provider_family", family);
  AddRow(&decision, "path", "principal->external_group->internal_group");
  AddRow(&decision, "redacted", AuthProviderOptionBool(request, "redact:", false) ? "true" : "false");
  return decision;
}

AuthProviderDecision InspectAuthProviderMetrics(const EngineApiRequest& request) {
  if (!SecurityContextHasRight(request.context, "OBS_METRICS_READ_FAMILY") && !HasAdmin(request.context)) {
    return Fail(request, "SECURITY.AUTHORIZATION.DENIED", "OBS_METRICS_READ_FAMILY");
  }
  auto decision = Ok(request, "metrics_ready");
  decision.evidence.push_back({"auth_provider_metrics", "fixture"});
  AddRow(&decision, "authentication_attempts", "1");
  AddRow(&decision, "authentication_failures", AuthProviderOptionBool(request, "include_failure:", false) ? "1" : "0");
  AddRow(&decision, "group_sync_attempts", "1");
  AddRow(&decision, "redaction_applied", AuthProviderOptionBool(request, "redact:", true) ? "true" : "false");
  return decision;
}

void ApplyAuthProviderDecision(EngineApiResult* result, const AuthProviderDecision& decision) {
  result->ok = decision.ok;
  result->cluster_authority_required = decision.cluster_authority_required;
  if (decision.diagnostic.code != "SB_ENGINE_API_OK" && !decision.diagnostic.code.empty()) { result->diagnostics.push_back(decision.diagnostic); }
  for (const auto& evidence : decision.evidence) { result->evidence.push_back(evidence); }
  for (const auto& row : decision.rows) { AddSecurityRow(result, {{row.first, row.second}}); }
}

}  // namespace scratchbird::engine::internal_api

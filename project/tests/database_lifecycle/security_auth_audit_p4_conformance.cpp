// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "security/audit_api.hpp"
#include "security/auth_provider_plugin_api.hpp"
#include "security/auth_provider_policy_api.hpp"
#include "security/authentication_api.hpp"
#include "security/policy_api.hpp"
#include "security/protected_material_api.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <unistd.h>
#include <vector>

namespace {

namespace api = scratchbird::engine::internal_api;

constexpr std::string_view kDatabaseUuid = "019e1d7e-7000-7000-8000-0000000000a4";
constexpr std::string_view kFilespaceUuid = "019e1d7e-7001-7000-8000-0000000000b4";
constexpr std::string_view kVerifier =
    "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
constexpr std::string_view kWrongVerifier =
    "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
constexpr std::string_view kPlaintextSecret = "CorrectHorseBatteryStaple-P4";
constexpr std::string_view kHexProof =
    "cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc";
constexpr std::string_view kFutureExpiryMs = "4102444800000";

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view message) {
  if (!condition) { Fail(message); }
}

std::filesystem::path MakeTempDir() {
  std::string tmpl = "/tmp/sb_p4_security_auth_audit.XXXXXX";
  std::vector<char> writable(tmpl.begin(), tmpl.end());
  writable.push_back('\0');
  char* made = ::mkdtemp(writable.data());
  Require(made != nullptr, "P4 security temp directory create failed");
  return std::filesystem::path(made);
}

api::EngineRequestContext Context(const std::filesystem::path& database_path, bool admin = true) {
  api::EngineRequestContext context;
  context.trust_mode = api::EngineTrustMode::server_isolated;
  context.database_path = database_path.string();
  context.database_uuid.canonical = std::string(kDatabaseUuid);
  context.security_context_present = true;
  context.local_transaction_id = 1;
  context.transaction_uuid.canonical = "txn-p4-security";
  context.principal_uuid.canonical = admin ? "principal-p4-admin" : "principal-p4-user";
  context.resource_epoch = 1000;
  context.catalog_generation_id = 1;
  context.security_epoch = 2;
  if (admin) {
    context.trace_tags.push_back("security.bootstrap");
    context.trace_tags.push_back("group:SEC");
    context.trace_tags.push_back("group:AUD");
  }
  std::ofstream touch(database_path, std::ios::app);
  Require(static_cast<bool>(touch), "P4 database fixture create failed");
  return context;
}

bool HasDiagnostic(const api::EngineApiResult& result, std::string_view code) {
  for (const auto& diagnostic : result.diagnostics) {
    if (diagnostic.code == code || diagnostic.detail == code) { return true; }
  }
  return false;
}

bool HasEvidence(const api::EngineApiResult& result, std::string_view kind) {
  for (const auto& evidence : result.evidence) {
    if (evidence.evidence_kind == kind) { return true; }
  }
  return false;
}

void ReplaceOption(std::vector<std::string>* options,
                   std::string_view prefix,
                   std::string value) {
  for (auto& option : *options) {
    if (option.rfind(std::string(prefix), 0) == 0) {
      option = std::move(value);
      return;
    }
  }
  options->push_back(std::move(value));
}

std::string FlattenResult(const api::EngineApiResult& result) {
  std::ostringstream out;
  out << result.operation_id << '\n';
  for (const auto& diagnostic : result.diagnostics) {
    out << diagnostic.code << '\n' << diagnostic.detail << '\n';
  }
  for (const auto& evidence : result.evidence) {
    out << evidence.evidence_kind << '\n' << evidence.evidence_id << '\n';
  }
  for (const auto& row : result.result_shape.rows) {
    for (const auto& field : row.fields) {
      out << field.first << '=' << field.second.encoded_value << '\n';
    }
  }
  return out.str();
}

void RequireNoLeak(const api::EngineApiResult& result, std::string_view secret) {
  Require(FlattenResult(result).find(secret) == std::string::npos,
          "P4 protected material leaked through diagnostic/result evidence");
}

void WriteAuthStore(const std::filesystem::path& database_path) {
  std::ofstream out(database_path.string() + ".sb.local_password_auth", std::ios::trunc);
  out << "alice\tlocal_password\t" << kVerifier << '\n';
  Require(static_cast<bool>(out), "P4 local password verifier store write failed");
}

void WriteTemporaryTokenStore(const std::filesystem::path& database_path,
                              std::string_view token,
                              std::string_view principal,
                              std::uint64_t expires_at_ms,
                              std::string_view state = "active") {
  std::ofstream out(database_path.string() + ".sb.temporary_auth_tokens", std::ios::trunc);
  out << token << '\t' << principal << '\t' << expires_at_ms << '\t' << state << '\n';
  Require(static_cast<bool>(out), "P4 temporary auth token store write failed");
}

api::EngineAuthenticateRequest AuthRequest(const std::filesystem::path& database_path,
                                           std::string_view verifier = kVerifier) {
  api::EngineAuthenticateRequest request;
  request.context = Context(database_path);
  request.provider_family = "local_password";
  request.principal_claim = "alice";
  request.credential_evidence = std::string("scheme=local_password_v1;principal=alice;verifier=") +
                                std::string(verifier);
  request.credential_evidence_present = true;
  request.target_database.uuid.canonical = std::string(kDatabaseUuid);
  request.option_envelopes.push_back("auth_authority:engine");
  request.option_envelopes.push_back("policy_generation_current:2");
  request.option_envelopes.push_back("policy_generation_observed:2");
  request.option_envelopes.push_back("security_epoch_current:2");
  request.option_envelopes.push_back("security_epoch_observed:2");
  request.option_envelopes.push_back("provider_generation_current:2");
  request.option_envelopes.push_back("provider_generation_observed:2");
  request.option_envelopes.push_back("provider_lifecycle_state:healthy");
  request.option_envelopes.push_back("default_policy_installed:true");
  return request;
}

api::EngineAuthenticateRequest TemporaryTokenAuthRequest(
    const std::filesystem::path& database_path,
    std::string_view token,
    std::string_view principal = "alice") {
  api::EngineAuthenticateRequest request;
  request.context = Context(database_path);
  request.provider_family = "security_database_temporary_token";
  request.principal_claim = std::string(principal);
  request.credential_evidence =
      std::string("scheme=security_database_temporary_token_v1;principal=") +
      std::string(principal) + ";token=" + std::string(token) + ";issuer=manager";
  request.credential_evidence_present = true;
  request.target_database.uuid.canonical = std::string(kDatabaseUuid);
  request.option_envelopes.push_back("auth_authority:engine");
  request.option_envelopes.push_back("policy_generation_current:2");
  request.option_envelopes.push_back("policy_generation_observed:2");
  request.option_envelopes.push_back("security_epoch_current:2");
  request.option_envelopes.push_back("security_epoch_observed:2");
  request.option_envelopes.push_back("provider_generation_current:2");
  request.option_envelopes.push_back("provider_generation_observed:2");
  request.option_envelopes.push_back("provider_lifecycle_state:healthy");
  request.option_envelopes.push_back("default_policy_installed:true");
  return request;
}

api::EngineAuthenticateProviderRequest ProviderAuthRequest(
    const std::filesystem::path& database_path,
    std::string provider_family,
    std::string principal = "alice") {
  api::EngineAuthenticateProviderRequest request;
  request.context = Context(database_path);
  request.option_envelopes.push_back("provider:" + std::move(provider_family));
  request.option_envelopes.push_back("principal:" + std::move(principal));
  request.option_envelopes.push_back("auth_authority:engine");
  request.option_envelopes.push_back("provider_enabled:true");
  request.option_envelopes.push_back("default_policy_installed:true");
  request.option_envelopes.push_back("policy_generation_current:2");
  request.option_envelopes.push_back("policy_generation_observed:2");
  request.option_envelopes.push_back("security_epoch_current:2");
  request.option_envelopes.push_back("security_epoch_observed:2");
  request.option_envelopes.push_back("provider_generation_current:2");
  request.option_envelopes.push_back("provider_generation_observed:2");
  request.option_envelopes.push_back("provider_lifecycle_state:healthy");
  return request;
}

void AddProviderPayload(api::EngineApiRequest* request, std::string payload) {
  request->option_envelopes.push_back("adapter_mode:live");
  request->option_envelopes.push_back("provider_payload:" + std::move(payload));
}

void AddDependency(api::EngineApiRequest* request, std::string dependency) {
  request->option_envelopes.push_back("dependency:" + std::move(dependency) + ":available");
}

void AddChannelBinding(api::EngineApiRequest* request) {
  request->option_envelopes.push_back("channel_binding_verified:true");
  request->option_envelopes.push_back("channel_binding_hash:" + std::string(kHexProof));
}

void TestAuthProviderManifestAndPolicy(const std::filesystem::path& database_path) {
  api::EngineRegisterAuthProviderRequest register_request;
  register_request.context = Context(database_path);
  register_request.target_object.uuid.canonical = "auth-provider-p4-local";
  register_request.option_envelopes.push_back("provider:local_password");
  register_request.option_envelopes.push_back("provider_version:1");
  register_request.option_envelopes.push_back("implementation_version:p4-conformance");
  register_request.option_envelopes.push_back("required_capability:authn");
  register_request.option_envelopes.push_back("required_library:engine_local_hash_compare");
  register_request.option_envelopes.push_back("allowed_scope:database_local");
  const auto registered = api::EngineRegisterAuthProvider(register_request);
  Require(registered.ok && registered.admitted, "P4 auth provider manifest admission failed");
  Require(registered.provider.provider_family == "local_password",
          "P4 auth provider family was not canonicalized");
  Require(HasEvidence(registered, "auth_provider_admitted"),
          "P4 auth provider admission evidence missing");

  api::EngineReloadAuthProviderPolicyRequest policy_request;
  policy_request.context = Context(database_path);
  policy_request.target_object.uuid.canonical = "auth-provider-p4-local";
  policy_request.option_envelopes.push_back("provider:local_password");
  policy_request.option_envelopes.push_back("policy_uuid:auth-provider-policy-p4");
  policy_request.option_envelopes.push_back("provider_enabled:true");
  policy_request.option_envelopes.push_back("auth_authority:engine");
  policy_request.option_envelopes.push_back("default_policy_installed:true");
  const auto policy = api::EngineReloadAuthProviderPolicy(policy_request);
  Require(policy.ok && policy.reloaded, "P4 auth provider policy reload failed");
  Require(policy.policy.enabled && policy.policy.stale_behavior == "deny",
          "P4 auth provider default policy did not fail closed");

  api::EngineRegisterAuthProviderRequest unknown = register_request;
  unknown.option_envelopes.clear();
  unknown.option_envelopes.push_back("provider:unknown_provider");
  const auto unknown_result = api::EngineRegisterAuthProvider(unknown);
  Require(!unknown_result.ok && HasDiagnostic(unknown_result, "SECURITY.AUTHORITY.INVALID"),
          "P4 unknown auth provider was admitted");

  api::EngineRegisterAuthProviderRequest no_admin = register_request;
  no_admin.context = Context(database_path, false);
  const auto no_admin_result = api::EngineRegisterAuthProvider(no_admin);
  Require(!no_admin_result.ok && HasDiagnostic(no_admin_result, "SECURITY.AUTHORIZATION.DENIED"),
          "P4 auth provider registration without admin was admitted");
}

void TestEngineAuthenticationAndPolicy(const std::filesystem::path& database_path) {
  WriteAuthStore(database_path);
  const auto good = api::EngineAuthenticate(AuthRequest(database_path));
  Require(good.ok && good.authenticated, "P4 engine authentication rejected valid verifier");
  Require(HasEvidence(good, "authentication_provider"),
          "P4 engine authentication evidence missing");

  const auto wrong = api::EngineAuthenticate(AuthRequest(database_path, kWrongVerifier));
  Require(!wrong.ok && HasDiagnostic(wrong, "SECURITY.AUTHENTICATION.FAILED"),
          "P4 engine authentication accepted wrong verifier");

  auto parser = AuthRequest(database_path);
  ReplaceOption(&parser.option_envelopes, "auth_authority:", "auth_authority:parser");
  const auto parser_result = api::EngineAuthenticate(parser);
  Require(!parser_result.ok && HasDiagnostic(parser_result, "SECURITY.AUTHORITY.INVALID"),
          "P4 parser-side authentication authority was accepted");

  auto listener = AuthRequest(database_path);
  ReplaceOption(&listener.option_envelopes, "auth_authority:", "auth_authority:listener");
  const auto listener_result = api::EngineAuthenticate(listener);
  Require(!listener_result.ok && HasDiagnostic(listener_result, "SECURITY.AUTHORITY.INVALID"),
          "P4 listener-side authentication authority was accepted");

  auto plaintext = AuthRequest(database_path);
  plaintext.option_envelopes.push_back("credential_plaintext:persist");
  const auto plaintext_result = api::EngineAuthenticate(plaintext);
  Require(!plaintext_result.ok &&
              HasDiagnostic(plaintext_result, "SECURITY.PROTECTED_MATERIAL.DENIED"),
          "P4 reusable plaintext credential persistence was accepted");
  RequireNoLeak(plaintext_result, kPlaintextSecret);

  const std::string temporary_token = "p4-temporary-token-1";
  WriteTemporaryTokenStore(database_path, temporary_token, "alice", 0);
  const auto token_ok = api::EngineAuthenticate(
      TemporaryTokenAuthRequest(database_path, temporary_token));
  Require(token_ok.ok && token_ok.authenticated,
          "P4 engine rejected valid security database temporary token");
  Require(HasEvidence(token_ok, "authentication_provider"),
          "P4 temporary token authentication evidence missing");

  const auto token_wrong = api::EngineAuthenticate(
      TemporaryTokenAuthRequest(database_path, "p4-wrong-token"));
  Require(!token_wrong.ok && HasDiagnostic(token_wrong, "SECURITY.AUTHENTICATION.FAILED"),
          "P4 engine accepted wrong security database temporary token");

  const auto token_principal_mismatch = api::EngineAuthenticate(
      TemporaryTokenAuthRequest(database_path, temporary_token, "bob"));
  Require(!token_principal_mismatch.ok &&
              HasDiagnostic(token_principal_mismatch, "SECURITY.AUTHENTICATION.FAILED"),
          "P4 engine accepted temporary token for wrong principal");

  WriteTemporaryTokenStore(database_path, temporary_token, "alice", 1);
  const auto token_expired = api::EngineAuthenticate(
      TemporaryTokenAuthRequest(database_path, temporary_token));
  Require(!token_expired.ok && HasDiagnostic(token_expired, "SECURITY.AUTHENTICATION.FAILED"),
          "P4 engine accepted expired security database temporary token");

  api::EngineAuthenticateProviderRequest peer;
  peer.context = Context(database_path);
  peer.option_envelopes.push_back("provider:peer");
  peer.option_envelopes.push_back("principal:local-peer-user");
  peer.option_envelopes.push_back("auth_authority:engine");
  peer.option_envelopes.push_back("provider_enabled:true");
  peer.option_envelopes.push_back("default_policy_installed:true");
  peer.option_envelopes.push_back("provider_lifecycle_state:healthy");
  const auto peer_without_os_evidence = api::EngineAuthenticateProvider(peer);
  Require(!peer_without_os_evidence.ok &&
              HasDiagnostic(peer_without_os_evidence, "SECURITY.AUTHENTICATION.FAILED"),
          "P4 PEER authentication without OS credential verification was accepted");

  peer.option_envelopes.push_back("so_peercred_verified:true");
  const auto peer_with_os_evidence = api::EngineAuthenticateProvider(peer);
  Require(peer_with_os_evidence.ok && peer_with_os_evidence.authenticated,
          "P4 PEER authentication with verified OS credential evidence was rejected");

  auto ident = ProviderAuthRequest(database_path, "ident", "local-peer-user");
  const auto ident_without_os_evidence = api::EngineAuthenticateProvider(ident);
  Require(!ident_without_os_evidence.ok &&
              HasDiagnostic(ident_without_os_evidence, "SECURITY.AUTHENTICATION.FAILED"),
          "P4 ident authentication without OS credential verification was accepted");
  ident.option_envelopes.push_back("ucred_verified:true");
  const auto ident_with_os_evidence = api::EngineAuthenticateProvider(ident);
  Require(ident_with_os_evidence.ok && ident_with_os_evidence.authenticated,
          "P4 ident authentication with verified OS credential evidence was rejected");

  api::EngineEvaluatePolicyRequest policy;
  policy.context = Context(database_path);
  policy.target_object.uuid.canonical = "security-policy-p4";
  policy.policy_profile.encoded_profiles.push_back("allow:policy-admin");
  const auto policy_ok = api::EngineEvaluatePolicy(policy);
  Require(policy_ok.ok && HasEvidence(policy_ok, "policy_decision"),
          "P4 engine policy evaluation failed");

  api::EngineEvaluatePolicyRequest invalid_policy = policy;
  invalid_policy.policy_profile.encoded_profiles.push_back("unsafe:policy");
  const auto denied = api::EngineEvaluatePolicy(invalid_policy);
  Require(!denied.ok && HasDiagnostic(denied, "SECURITY.AUTHORIZATION.DENIED"),
          "P4 unsafe policy profile was accepted");
}

void TestAuthRegistryMethodPosture(const std::filesystem::path& database_path) {
  Require(api::CanonicalAuthProviderFamily("SCRAM-SHA-256") == "scram_sha256",
          "P4 SCRAM-SHA-256 alias was not canonicalized");
  Require(api::CanonicalAuthProviderFamily("spiffe") == "workload_identity",
          "P4 SPIFFE alias was not canonicalized");
  Require(api::CanonicalAuthProviderFamily("fido2") == "webauthn",
          "P4 FIDO2 alias was not canonicalized");
  Require(api::CanonicalAuthProviderFamily("mfa") == "factor_chain",
          "P4 MFA alias was not canonicalized");
  Require(api::AuthProviderFamilySupportsAuthn("factor_chain"),
          "P4 factor_chain provider was not admitted as authn-capable when policy-gated");
  Require(!api::AuthProviderFamilySupportsAuthn("oauth_validator"),
          "P4 OAuth validator-only provider was exposed as a login provider");

  auto oauth = ProviderAuthRequest(database_path, "oauth_validator");
  AddDependency(&oauth, "oidc_jwt_client");
  AddProviderPayload(&oauth,
                     "iss=idp;aud=sb;sub=alice;alg=rs256;exp=" +
                         std::string(kFutureExpiryMs) + ";sig=" +
                         std::string(kHexProof) + ";groups=APP;validator=introspection");
  const auto oauth_result = api::EngineAuthenticateProvider(oauth);
  Require(!oauth_result.ok && HasDiagnostic(oauth_result, "SECURITY.AUTH_PROVIDER_UNSUPPORTED"),
          "P4 OAuth validator-only provider was accepted as login");

  auto password_compat = ProviderAuthRequest(database_path, "password_compat");
  AddChannelBinding(&password_compat);
  password_compat.option_envelopes.push_back("credential:valid");
  const auto password_compat_denied = api::EngineAuthenticateProvider(password_compat);
  Require(!password_compat_denied.ok &&
              HasDiagnostic(password_compat_denied, "SECURITY.AUTH_PROVIDER_UNSUPPORTED"),
          "P4 password compatibility auth was accepted without policy");
  password_compat.option_envelopes.push_back("allow_password_compat:true");
  const auto password_compat_ok = api::EngineAuthenticateProvider(password_compat);
  Require(password_compat_ok.ok && password_compat_ok.authenticated,
          "P4 policy-enabled password compatibility auth was rejected");

  auto scram = ProviderAuthRequest(database_path, "scram_sha256");
  scram.option_envelopes.push_back("credential:valid");
  const auto scram_no_binding = api::EngineAuthenticateProvider(scram);
  Require(!scram_no_binding.ok &&
              HasDiagnostic(scram_no_binding, "SECURITY.CHANNEL_BINDING_REQUIRED"),
          "P4 SCRAM-SHA-256 without channel binding was accepted");
  AddChannelBinding(&scram);
  const auto scram_ok = api::EngineAuthenticateProvider(scram);
  Require(scram_ok.ok && scram_ok.authenticated,
          "P4 SCRAM-SHA-256 with channel binding was rejected");

  auto scram512 = ProviderAuthRequest(database_path, "scram_sha512");
  AddChannelBinding(&scram512);
  scram512.option_envelopes.push_back("credential:valid");
  const auto scram512_guarded = api::EngineAuthenticateProvider(scram512);
  Require(!scram512_guarded.ok &&
              HasDiagnostic(scram512_guarded, "SECURITY.AUTH_METHOD_UNSUPPORTED"),
          "P4 guarded SCRAM-SHA-512 was accepted before verifier storage/tests");
  scram512.option_envelopes.push_back("scram_sha512_verifier_storage:available");
  scram512.option_envelopes.push_back("scram_sha512_tests:present");
  const auto scram512_ok = api::EngineAuthenticateProvider(scram512);
  Require(scram512_ok.ok && scram512_ok.authenticated,
          "P4 guarded-ready SCRAM-SHA-512 was rejected");
}

void TestFederatedDirectoryAndMfaProviders(const std::filesystem::path& database_path) {
  auto ldap = ProviderAuthRequest(database_path, "ldap");
  AddDependency(&ldap, "ldap_client");
  AddProviderPayload(&ldap,
                     "user=alice;password=ldapSecretP4;endpoint=ldaps.example;starttls=true;bind=allow;groups=APP;path=cn-alice");
  const auto ldap_result = api::EngineAuthenticateProvider(ldap);
  Require(ldap_result.ok && ldap_result.authenticated,
          "P4 LDAP StartTLS provider evidence was rejected");
  RequireNoLeak(ldap_result, "ldapSecretP4");

  auto kerberos = ProviderAuthRequest(database_path, "kerberos");
  AddDependency(&kerberos, "gssapi_krb5");
  AddChannelBinding(&kerberos);
  AddProviderPayload(&kerberos,
                     "spn=svc-sb;subject=alice;nonce=n1;kdc=kdc1;sig=" +
                         std::string(kHexProof) + ";exp=" +
                         std::string(kFutureExpiryMs) + ";pac_groups=APP");
  const auto kerberos_result = api::EngineAuthenticateProvider(kerberos);
  Require(kerberos_result.ok && kerberos_result.authenticated,
          "P4 Kerberos/GSSAPI provider evidence was rejected");

  auto oidc = ProviderAuthRequest(database_path, "oidc");
  AddDependency(&oidc, "oidc_jwt_client");
  AddProviderPayload(&oidc,
                     "iss=idp;aud=sb;sub=alice;alg=rs256;exp=" +
                         std::string(kFutureExpiryMs) + ";sig=" +
                         std::string(kHexProof) + ";groups=APP;validator=jwks;assertion=oidcSecretAssertion");
  const auto oidc_result = api::EngineAuthenticateProvider(oidc);
  Require(oidc_result.ok && oidc_result.authenticated,
          "P4 OIDC/JWT validator evidence was rejected");
  RequireNoLeak(oidc_result, "oidcSecretAssertion");

  auto oidc_missing_validator = ProviderAuthRequest(database_path, "oidc");
  AddDependency(&oidc_missing_validator, "oidc_jwt_client");
  AddProviderPayload(&oidc_missing_validator,
                     "iss=idp;aud=sb;sub=alice;alg=rs256;exp=" +
                         std::string(kFutureExpiryMs) + ";sig=" +
                         std::string(kHexProof) + ";groups=APP");
  const auto oidc_denied = api::EngineAuthenticateProvider(oidc_missing_validator);
  Require(!oidc_denied.ok && HasDiagnostic(oidc_denied, "SECURITY.AUTHENTICATION.FAILED"),
          "P4 OIDC/JWT without validator boundary was accepted");

  auto saml = ProviderAuthRequest(database_path, "saml");
  AddDependency(&saml, "saml_xmlsig");
  AddProviderPayload(&saml,
                     "issuer=idp;audience=sb;subject=alice;not_on_or_after=" +
                         std::string(kFutureExpiryMs) + ";signature=" +
                         std::string(kHexProof) + ";attributes=groups;assertion=samlSecretAssertion");
  const auto saml_result = api::EngineAuthenticateProvider(saml);
  Require(saml_result.ok && saml_result.authenticated,
          "P4 SAML validator evidence was rejected");
  RequireNoLeak(saml_result, "samlSecretAssertion");

  auto radius = ProviderAuthRequest(database_path, "radius");
  radius.option_envelopes.push_back("radius_policy_enabled:true");
  AddDependency(&radius, "radius_client");
  AddProviderPayload(&radius,
                     "user=alice;authenticator=" + std::string(kHexProof) +
                         ";result=accept;attribute=group;shared_secret_handle=secretRef");
  const auto radius_result = api::EngineAuthenticateProvider(radius);
  Require(radius_result.ok && radius_result.authenticated,
          "P4 policy-enabled RADIUS Access-Request evidence was rejected");

  auto webauthn = ProviderAuthRequest(database_path, "fido2");
  webauthn.option_envelopes.push_back("webauthn_policy_enabled:true");
  AddDependency(&webauthn, "webauthn_fido2");
  AddProviderPayload(&webauthn,
                     "challenge=chal1;rp=scratchbird;origin=https://db.example;credential=cred1;subject=alice;uv=true;exp=" +
                         std::string(kFutureExpiryMs) + ";signature=" +
                         std::string(kHexProof));
  const auto webauthn_result = api::EngineAuthenticateProvider(webauthn);
  Require(webauthn_result.ok && webauthn_result.authenticated,
          "P4 WebAuthn/FIDO2 provider evidence was rejected");

  auto factor_chain = ProviderAuthRequest(database_path, "mfa");
  factor_chain.option_envelopes.push_back("factor_chain_policy_enabled:true");
  AddDependency(&factor_chain, "webauthn_fido2");
  AddProviderPayload(&factor_chain,
                     "primary_auth_context_uuid=authctx;factor_policy_uuid=policy;factor_results=allow;challenge_transcript_hash=" +
                         std::string(kHexProof) + ";subject=alice");
  const auto factor_chain_result = api::EngineAuthenticateProvider(factor_chain);
  Require(factor_chain_result.ok && factor_chain_result.authenticated,
          "P4 MFA factor_chain provider evidence was rejected");
}

void TestWorkloadProxyTokenAndRefreshProviders(const std::filesystem::path& database_path) {
  auto certificate = ProviderAuthRequest(database_path, "mtls");
  AddDependency(&certificate, "tls_x509");
  AddChannelBinding(&certificate);
  AddProviderPayload(&certificate,
                     "subject=alice;san=alice.example;chain=trusted;revoked=false;eku=clientAuth;fingerprint=" +
                         std::string(kHexProof) + ";groups=APP");
  const auto certificate_result = api::EngineAuthenticateProvider(certificate);
  Require(certificate_result.ok && certificate_result.authenticated,
          "P4 mTLS certificate provider evidence was rejected");

  auto pam = ProviderAuthRequest(database_path, "pam");
  pam.option_envelopes.push_back("pam_policy_enabled:true");
  AddDependency(&pam, "pam");
  AddProviderPayload(&pam,
                     "user=alice;service=scratchbird;module=unix;password=pamSecretP4;prompt=hidden;account=allow;session=open");
  const auto pam_result = api::EngineAuthenticateProvider(pam);
  Require(pam_result.ok && pam_result.authenticated,
          "P4 policy-enabled PAM provider evidence was rejected");
  RequireNoLeak(pam_result, "pamSecretP4");

  auto remote = ProviderAuthRequest(database_path, "remote_security_database");
  AddDependency(&remote, "remote_scratchbird_security");
  AddChannelBinding(&remote);
  AddProviderPayload(&remote,
                     "remote_locator=remote1;subject=alice;delegated_credential=remoteSecretP4;exp=" +
                         std::string(kFutureExpiryMs) + ";sig=" +
                         std::string(kHexProof) + ";groups=APP");
  const auto remote_result = api::EngineAuthenticateProvider(remote);
  Require(remote_result.ok && remote_result.authenticated,
          "P4 remote ScratchBird security database evidence was rejected");
  RequireNoLeak(remote_result, "remoteSecretP4");

  auto cluster = ProviderAuthRequest(database_path, "cluster_security");
  AddChannelBinding(&cluster);
  AddProviderPayload(&cluster,
                     "cluster_member_uuid=member1;cluster_epoch=2;fence_token=fence;subject=alice;sig=" +
                         std::string(kHexProof) + ";groups=APP");
  const auto cluster_no_authority = api::EngineAuthenticateProvider(cluster);
  Require(!cluster_no_authority.ok &&
              HasDiagnostic(cluster_no_authority, "PROCESS.CLUSTER_PATH_ABSENT"),
          "P4 cluster security provider was accepted without cluster authority");
  cluster.context.cluster_authority_available = true;
  const auto cluster_result = api::EngineAuthenticateProvider(cluster);
  Require(cluster_result.ok && cluster_result.authenticated,
          "P4 cluster security provider with cluster authority evidence was rejected");

  auto spiffe = ProviderAuthRequest(database_path, "spiffe");
  AddDependency(&spiffe, "spiffe_svid_or_workload_oidc");
  AddProviderPayload(&spiffe,
                     "spiffe_id=spiffe://tenant/workload;trust_bundle=bundle1;exp=" +
                         std::string(kFutureExpiryMs) + ";sig=" +
                         std::string(kHexProof) + ";service_group=APP");
  const auto spiffe_result = api::EngineAuthenticateProvider(spiffe);
  Require(spiffe_result.ok && spiffe_result.authenticated,
          "P4 workload SPIFFE identity evidence was rejected");

  auto managed = ProviderAuthRequest(database_path, "managed_identity");
  AddDependency(&managed, "spiffe_svid_or_workload_oidc");
  AddProviderPayload(&managed,
                     "issuer=cloud;audience=sb;subject=vm1;exp=" +
                         std::string(kFutureExpiryMs) + ";sig=" +
                         std::string(kHexProof) + ";service_group=APP");
  const auto managed_result = api::EngineAuthenticateProvider(managed);
  Require(managed_result.ok && managed_result.authenticated,
          "P4 managed workload identity evidence was rejected");

  auto bearer = ProviderAuthRequest(database_path, "bearer_token");
  AddProviderPayload(&bearer,
                     "token_id=tok1;proof=" + std::string(kHexProof) +
                         ";exp=" + std::string(kFutureExpiryMs) + ";subject=alice;token=topSecretBearer");
  const auto bearer_result = api::EngineAuthenticateProvider(bearer);
  Require(bearer_result.ok && bearer_result.authenticated,
          "P4 bearer token evidence was rejected");
  RequireNoLeak(bearer_result, "topSecretBearer");

  auto revoked = bearer;
  ReplaceOption(&revoked.option_envelopes,
                "provider_payload:",
                "provider_payload:token_id=tok1;proof=" + std::string(kHexProof) +
                    ";exp=" + std::string(kFutureExpiryMs) +
                    ";subject=alice;revoked=true;token=topSecretBearer");
  const auto revoked_result = api::EngineAuthenticateProvider(revoked);
  Require(!revoked_result.ok && HasDiagnostic(revoked_result, "SECURITY.TOKEN_REVOKED"),
          "P4 revoked bearer token was accepted");
  RequireNoLeak(revoked_result, "topSecretBearer");

  auto api_key = ProviderAuthRequest(database_path, "authkey");
  AddProviderPayload(&api_key,
                     "key_id=key1;proof=" + std::string(kHexProof) +
                         ";generation=2;subject=alice;groups=APP;api_secret=topSecretApiKey");
  const auto api_key_result = api::EngineAuthenticateProvider(api_key);
  Require(api_key_result.ok && api_key_result.authenticated,
          "P4 API key/authkey token evidence was rejected");
  RequireNoLeak(api_key_result, "topSecretApiKey");

  auto proxy = ProviderAuthRequest(database_path, "proxy_assertion");
  proxy.option_envelopes.push_back("proxy_assertion_policy_enabled:true");
  AddDependency(&proxy, "proxy_assertion_verifier");
  AddChannelBinding(&proxy);
  AddProviderPayload(&proxy,
                     "iss=manager;sub=alice;aud=engine;proxy=listener;source=trusted;manager_trust=trusted;listener_binding=verified;exp=" +
                         std::string(kFutureExpiryMs) + ";sig=" +
                         std::string(kHexProof) + ";groups=APP;assertion=topSecretProxyAssertion");
  const auto proxy_result = api::EngineAuthenticateProvider(proxy);
  Require(proxy_result.ok && proxy_result.authenticated,
          "P4 proxy assertion provider evidence was rejected");
  RequireNoLeak(proxy_result, "topSecretProxyAssertion");

  auto token_refresh = ProviderAuthRequest(database_path, "token_refresh");
  token_refresh.option_envelopes.push_back("token_refresh_policy_enabled:true");
  AddChannelBinding(&token_refresh);
  token_refresh.option_envelopes.push_back("refresh_token_proof:present");
  const auto refresh_without_reauth = api::EngineAuthenticateProvider(token_refresh);
  Require(!refresh_without_reauth.ok &&
              HasDiagnostic(refresh_without_reauth, "SECURITY.REAUTH_REQUIRED"),
          "P4 token refresh without engine reauth envelope was accepted");
  token_refresh.option_envelopes.push_back("reauth_envelope:engine_owned");
  const auto refresh_with_reauth = api::EngineAuthenticateProvider(token_refresh);
  Require(refresh_with_reauth.ok && refresh_with_reauth.authenticated,
          "P4 token refresh with engine reauth envelope was rejected");

  auto custom = ProviderAuthRequest(database_path, "custom_cpp_plugin");
  custom.option_envelopes.push_back("custom_cpp_plugin_policy_enabled:true");
  AddProviderPayload(&custom,
                     "plugin_uuid=plugin1;subject=alice;plugin_result=allow;plugin_signature=" +
                         std::string(kHexProof) + ";timeout=bounded;memory=bounded;plugin_secret=customSecretP4");
  const auto custom_guarded = api::EngineAuthenticateProvider(custom);
  Require(!custom_guarded.ok && HasDiagnostic(custom_guarded, "SECURITY.AUTH_PLUGIN_INVALID"),
          "P4 unregistered custom C++ auth plugin was accepted");
  custom.option_envelopes.push_back("custom_cpp_plugin_registered:true");
  custom.option_envelopes.push_back("plugin_trusted:true");
  custom.option_envelopes.push_back("plugin_signature_valid:true");
  custom.option_envelopes.push_back("plugin_timeout_policy:present");
  custom.option_envelopes.push_back("plugin_memory_policy:present");
  const auto custom_result = api::EngineAuthenticateProvider(custom);
  Require(custom_result.ok && custom_result.authenticated,
          "P4 registered custom C++ auth plugin evidence was rejected");
  RequireNoLeak(custom_result, "customSecretP4");
}

void TestAuditAndProtectedMaterial(const std::filesystem::path& database_path) {
  api::EngineEmitAuditEventRequest missing;
  missing.context = Context(database_path);
  const auto missing_result = api::EngineEmitAuditEvent(missing);
  Require(!missing_result.ok && HasDiagnostic(missing_result, "SECURITY.AUDIT.EVIDENCE_REQUIRED"),
          "P4 audit event without class was accepted");

  api::EngineEmitAuditEventRequest audit;
  audit.context = Context(database_path);
  audit.event_class = "security.authentication";
  audit.outcome = "accepted";
  audit.option_envelopes.push_back("redact:true");
  const auto emitted = api::EngineEmitAuditEvent(audit);
  Require(emitted.ok && emitted.emitted && emitted.redacted,
          "P4 audit event did not persist redacted evidence");
  Require(HasEvidence(emitted, "audit_event"), "P4 audit event evidence missing");

  api::EngineEmitLifecycleAuditEventRequest lifecycle;
  lifecycle.context = Context(database_path);
  lifecycle.operation_key = "security.policy.reload";
  lifecycle.outcome = "accepted";
  lifecycle.cache_marker_uuid = "cache-marker-p4";
  lifecycle.cache_invalidation_recorded = true;
  const auto lifecycle_emitted = api::EngineEmitLifecycleAuditEvent(lifecycle);
  Require(lifecycle_emitted.ok && lifecycle_emitted.cache_marker_linked,
          "P4 lifecycle audit did not link cache invalidation evidence");

  api::EnginePurgeProtectedMaterialRequest purge;
  purge.context = Context(database_path);
  purge.option_envelopes.push_back("key_authority:engine");
  (void)api::EnginePurgeProtectedMaterial(purge);

  api::EngineAdmitEncryptionKeyRequest key;
  key.context = Context(database_path);
  key.key_uuid = "key-p4";
  key.filespace_uuid = std::string(kFilespaceUuid);
  key.secret_evidence = "kms-ref:v1:p4-proof";
  key.option_envelopes.push_back("key_authority:engine");
  const auto admitted = api::EngineAdmitEncryptionKey(key);
  Require(admitted.ok && admitted.key_admitted && !admitted.plaintext_material_returned,
          "P4 encrypted key admission failed");

  api::EngineOpenEncryptedFilespaceRequest open;
  open.context = Context(database_path);
  open.database_uuid = std::string(kDatabaseUuid);
  open.filespace_uuid = std::string(kFilespaceUuid);
  open.key_uuid = "key-p4";
  open.key_handle = admitted.key_handle;
  open.option_envelopes.push_back("filespace_open_authority:engine");
  const auto opened = api::EngineOpenEncryptedFilespace(open);
  Require(opened.ok && opened.open_admitted && opened.key_cache_hit,
          "P4 encrypted filespace open did not use active protected-material key");

  api::EngineAdmitEncryptionKeyRequest plaintext = key;
  plaintext.key_uuid = "key-p4-plaintext";
  plaintext.secret_evidence = std::string("password=") + std::string(kPlaintextSecret);
  const auto plaintext_key = api::EngineAdmitEncryptionKey(plaintext);
  Require(!plaintext_key.ok && HasDiagnostic(plaintext_key, "SECURITY.KEY.PLAINTEXT_REFUSED"),
          "P4 plaintext key material was accepted");
  RequireNoLeak(plaintext_key, kPlaintextSecret);
}

}  // namespace

int main() {
  const auto temp_dir = MakeTempDir();
  const auto database_path = temp_dir / "p4_security_auth_audit.sbdb";
  TestAuthProviderManifestAndPolicy(database_path);
  TestEngineAuthenticationAndPolicy(database_path);
  TestAuthRegistryMethodPosture(database_path);
  TestFederatedDirectoryAndMfaProviders(database_path);
  TestWorkloadProxyTokenAndRefreshProviders(database_path);
  TestAuditAndProtectedMaterial(database_path);
  std::filesystem::remove_all(temp_dir);
  return EXIT_SUCCESS;
}

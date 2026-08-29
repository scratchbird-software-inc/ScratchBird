// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "security/authentication_api.hpp"
#include "security/security_crypto_policy.hpp"
#include "security/security_principal_lifecycle.hpp"
#include "uuid.hpp"

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>

#include <array>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace api = scratchbird::engine::internal_api;
namespace uuid = scratchbird::core::uuid;

using scratchbird::core::platform::UuidKind;
using scratchbird::core::platform::u64;

constexpr u64 kBaseMillis = 1771200000000ull;
constexpr std::string_view kVerifier =
    "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
constexpr std::string_view kWrongVerifier =
    "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
constexpr std::string_view kInitialPassword = "scratchbird-initial-password";
constexpr std::string_view kPassword = "scratchbird-rotated-password";
constexpr std::string_view kWrongPassword = "scratchbird-wrong-password";
constexpr int kPasswordPbkdf2Iterations = 600000;

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view message) {
  if (!condition) { Fail(message); }
}

template <std::size_t N>
std::string HexEncode(const std::array<unsigned char, N>& bytes) {
  static constexpr char kHex[] = "0123456789abcdef";
  std::string output;
  output.reserve(bytes.size() * 2);
  for (const unsigned char byte : bytes) {
    output.push_back(kHex[(byte >> 4) & 0x0f]);
    output.push_back(kHex[byte & 0x0f]);
  }
  return output;
}

std::string DeriveLocalPasswordFingerprint(std::string_view password) {
  Require(!password.empty() &&
              password.size() <=
                  static_cast<std::size_t>((std::numeric_limits<int>::max)()) &&
              password.find('\0') == std::string_view::npos,
          "PBKDF2 password fixture is invalid");
  std::array<unsigned char, 16> salt{};
  std::array<unsigned char, 32> verifier{};
  const bool derived =
      RAND_priv_bytes(salt.data(), static_cast<int>(salt.size())) == 1 &&
      PKCS5_PBKDF2_HMAC(password.data(),
                        static_cast<int>(password.size()),
                        salt.data(),
                        static_cast<int>(salt.size()),
                        kPasswordPbkdf2Iterations,
                        EVP_sha256(),
                        static_cast<int>(verifier.size()),
                        verifier.data()) == 1;
  std::string fingerprint;
  if (derived) {
    fingerprint = "local-password-pbkdf2-sha256:v1:iterations=" +
                  std::to_string(kPasswordPbkdf2Iterations) +
                  ":salt=" + HexEncode(salt) +
                  ":verifier=" + HexEncode(verifier);
  }
  OPENSSL_cleanse(salt.data(), salt.size());
  OPENSSL_cleanse(verifier.data(), verifier.size());
  Require(derived, "PBKDF2-HMAC-SHA-256 password fingerprint derivation failed");
  return fingerprint;
}

api::EngineUuid MakeUuid(UuidKind kind, u64 offset) {
  const auto generated = uuid::GenerateEngineIdentityV7(kind, kBaseMillis + offset);
  api::EngineUuid out;
  Require(generated.ok(), "engine UUID generation failed");
  out.canonical = uuid::UuidToString(generated.value.value);
  return out;
}

struct Fixture {
  api::EngineUuid database = MakeUuid(UuidKind::database, 1);
  api::EngineUuid principal = MakeUuid(UuidKind::principal, 2);
  api::EngineUuid admin_principal = MakeUuid(UuidKind::principal, 3);
  api::EngineUuid session = MakeUuid(UuidKind::object, 4);
  api::EngineUuid transaction = MakeUuid(UuidKind::transaction, 5);
  api::EngineUuid unused_principal = MakeUuid(UuidKind::principal, 6);
  api::EngineUuid lifecycle_principal = MakeUuid(UuidKind::principal, 7);
  api::EngineUuid global_connect_grant = MakeUuid(UuidKind::object, 8);
  std::filesystem::path work_dir;
  std::filesystem::path database_path;
};

api::EngineRequestContext Context(const Fixture& fixture) {
  api::EngineRequestContext context;
  context.trust_mode = api::EngineTrustMode::server_isolated;
  context.request_id = "pcr091-security-durable-crypto";
  context.database_path = fixture.database_path.string();
  context.database_uuid = fixture.database;
  context.session_uuid = fixture.session;
  context.transaction_uuid = fixture.transaction;
  context.catalog_generation_id = 7;
  context.security_epoch = 7;
  return context;
}

bool HasDiagnosticDetail(const api::EngineApiResult& result, std::string_view detail) {
  for (const auto& diagnostic : result.diagnostics) {
    if (diagnostic.detail == detail) { return true; }
  }
  return false;
}

bool HasEvidence(const api::EngineApiResult& result,
                 std::string_view kind,
                 std::string_view id) {
  for (const auto& evidence : result.evidence) {
    if (evidence.evidence_kind == kind && evidence.evidence_id == id) {
      return true;
    }
  }
  return false;
}

bool HasAuthorizationTag(const api::EngineAuthenticateResult& result,
                         std::string_view tag) {
  for (const auto& candidate :
       result.connection_security_context.authorization_trace_tags) {
    if (candidate == tag) { return true; }
  }
  return false;
}

std::string FieldValue(const api::EngineApiResult& result, std::string_view key) {
  for (const auto& row : result.result_shape.rows) {
    for (const auto& field : row.fields) {
      if (field.first == key) { return field.second.encoded_value; }
    }
  }
  return {};
}

api::EngineRequestContext SecurityAdminContext(const Fixture& fixture,
                                               std::uint64_t local_tx) {
  auto context = Context(fixture);
  context.trust_mode = api::EngineTrustMode::embedded_in_process;
  context.security_context_present = true;
  context.principal_uuid = fixture.admin_principal;
  context.trace_tags.push_back("security.fixture_trace_authority");
  context.trace_tags.push_back("right:SEC_IDENTITY_ADMIN");
  context.trace_tags.push_back("right:SEC_GRANT_ADMIN");
  context.local_transaction_id = local_tx;
  context.snapshot_visible_through_local_transaction_id = local_tx;
  return context;
}

std::string TemporaryTokenFingerprint(std::string_view token_handle,
                                      std::string_view token_digest,
                                      std::string_view state = "active",
                                      std::string_view expires_at_ms = "0") {
  std::string payload;
  payload += token_digest;
  payload += '|';
  payload += state.empty() ? "active" : state;
  payload += '|';
  payload += expires_at_ms.empty() ? "0" : expires_at_ms;
  return "security-temporary-token:v1:hmac-sha256:" +
         api::SecurityHmacSha256Hex(token_handle, payload);
}

void CreatePrincipalCredential(const Fixture& fixture,
                               const api::EngineUuid& principal_uuid,
                               std::string_view principal_name,
                               std::string credential_fingerprint,
                               std::uint64_t local_tx) {
  api::EngineSecurityCreatePrincipalRequest request;
  request.context = SecurityAdminContext(fixture, local_tx);
  request.target_object.uuid = principal_uuid;
  request.target_object.object_kind = "security_principal";
  request.principal_uuid = principal_uuid.canonical;
  request.principal_name = std::string(principal_name);
  request.credential_fingerprint = std::move(credential_fingerprint);
  const auto created = api::EngineSecurityCreatePrincipal(request);
  Require(created.ok && created.principal_created,
          "failed to create durable principal credential state");
}

void AlterPrincipalCredential(const Fixture& fixture,
                              const api::EngineUuid& principal_uuid,
                              std::string_view principal_name,
                              std::string credential_fingerprint,
                              std::uint64_t local_tx) {
  api::EngineSecurityAlterPrincipalRequest request;
  request.context = SecurityAdminContext(fixture, local_tx);
  request.target_object.uuid = principal_uuid;
  request.target_object.object_kind = "security_principal";
  request.principal_uuid = principal_uuid.canonical;
  request.principal_name = std::string(principal_name);
  request.credential_fingerprint = std::move(credential_fingerprint);
  const auto altered = api::EngineSecurityAlterPrincipal(request);
  Require(altered.ok && altered.principal_altered,
          "failed to alter durable principal credential state");
}

void GrantGlobalConnectPrivilege(const Fixture& fixture,
                                 const api::EngineUuid& principal_uuid,
                                 std::uint64_t local_tx) {
  api::EngineSecurityGrantPrivilegeRequest request;
  request.context = SecurityAdminContext(fixture, local_tx);
  request.grant_uuid = fixture.global_connect_grant.canonical;
  request.grantee_uuid = principal_uuid.canonical;
  request.grantee_kind = "principal";
  request.target_object_uuid.clear();
  request.target_object_kind.clear();
  request.privilege = "CONNECT";
  request.grant_effect = "allow";
  request.option_envelopes.push_back("grant_authority:engine");

  const auto granted = api::EngineSecurityGrantPrivilege(request);
  Require(granted.ok && granted.privilege_granted,
          "failed to grant durable global CONNECT privilege");
  Require(granted.primary_object.uuid.canonical ==
              fixture.global_connect_grant.canonical &&
              granted.primary_object.object_kind == "security_privilege_grant" &&
              granted.security_generation != 0,
          "global CONNECT grant result did not preserve exact durable identity");

  const auto loaded = api::LoadSecurityPrincipalLifecycleState(request.context);
  Require(loaded.ok, "global CONNECT grant durable state did not reload");
  const api::EngineSecurityPrivilegeGrantRecord* exact_grant = nullptr;
  for (const auto& grant : loaded.state.grants) {
    if (grant.grant_uuid == fixture.global_connect_grant.canonical) {
      Require(exact_grant == nullptr,
              "global CONNECT durable grant identity was duplicated");
      exact_grant = &grant;
    }
  }
  Require(exact_grant != nullptr && !exact_grant->revoked,
          "active global CONNECT durable grant was not reloaded");
  Require(exact_grant->grantee_uuid == principal_uuid.canonical &&
              exact_grant->grantee_kind == "principal" &&
              exact_grant->target_object_uuid.empty() &&
              exact_grant->target_object_kind.empty() &&
              exact_grant->privilege == "CONNECT" &&
              exact_grant->grant_effect == "allow" &&
              exact_grant->grantor_principal_uuid ==
                  request.context.principal_uuid.canonical &&
              exact_grant->creator_tx == local_tx &&
              exact_grant->security_generation != 0,
          "reloaded global CONNECT grant did not preserve exact authority fields");
}

std::string StructuredVerifierClaim(std::string_view principal,
                                    const api::EngineUuid& principal_uuid,
                                    std::string_view verifier) {
  std::string evidence = "scheme=local_password_v1;principal=";
  evidence += principal;
  evidence += ";principal_uuid=";
  evidence += principal_uuid.canonical;
  evidence += ";storage_authority=durable_security_catalog;verifier=";
  evidence += verifier;
  return evidence;
}

api::EngineAuthenticateRequest LocalPasswordRequest(
    const Fixture& fixture,
    std::string principal,
    api::EngineUuid principal_uuid,
    std::string evidence,
    bool include_durable_hint = true) {
  api::EngineAuthenticateRequest request;
  request.context = Context(fixture);
  request.provider_family = "local_password";
  request.principal_claim = std::move(principal);
  request.credential_evidence = std::move(evidence);
  request.credential_evidence_present = true;
  request.target_database.uuid = fixture.database;
  request.target_database.object_kind = "database";
  request.target_object.uuid = fixture.database;
  request.target_object.object_kind = "security_authority";
  request.option_envelopes.push_back("auth_authority:engine");
  request.option_envelopes.push_back("policy_generation_current:7");
  request.option_envelopes.push_back("policy_generation_observed:7");
  request.option_envelopes.push_back("security_epoch_current:7");
  request.option_envelopes.push_back("security_epoch_observed:7");
  request.option_envelopes.push_back("provider_generation_current:7");
  request.option_envelopes.push_back("provider_generation_observed:7");
  request.option_envelopes.push_back("provider_lifecycle_state:healthy");
  request.option_envelopes.push_back("default_policy_installed:true");
  if (include_durable_hint && !principal_uuid.canonical.empty()) {
    request.option_envelopes.push_back("durable_principal_uuid:" + principal_uuid.canonical);
  }
  return request;
}

std::string TokenEvidence(std::string_view principal,
                          const api::EngineUuid& principal_uuid,
                          std::string_view token,
                          std::string_view state = "active",
                          std::string_view expires_at_ms = "0") {
  const std::string digest = api::SecuritySha256Hex(token);
  std::string evidence =
      "scheme=security_database_temporary_token_v1;principal=";
  evidence += principal;
  evidence += ";principal_uuid=";
  evidence += principal_uuid.canonical;
  evidence += ";storage_authority=durable_security_catalog;token=";
  evidence += token;
  evidence += ";token_handle=token-handle-pcr091;token_digest=";
  evidence += digest;
  evidence += ";state=";
  evidence += state;
  evidence += ";expires_at_ms=";
  evidence += expires_at_ms;
  return evidence;
}

api::EngineAuthenticateRequest TokenRequest(const Fixture& fixture,
                                            std::string evidence) {
  api::EngineAuthenticateRequest request;
  request.context = Context(fixture);
  request.provider_family = "security_database_temporary_token";
  request.principal_claim = "alice";
  request.credential_evidence = std::move(evidence);
  request.credential_evidence_present = true;
  request.target_database.uuid = fixture.database;
  request.target_database.object_kind = "database";
  request.target_object.uuid = fixture.database;
  request.target_object.object_kind = "security_authority";
  request.option_envelopes.push_back("auth_authority:engine");
  request.option_envelopes.push_back("policy_generation_current:7");
  request.option_envelopes.push_back("policy_generation_observed:7");
  request.option_envelopes.push_back("security_epoch_current:7");
  request.option_envelopes.push_back("security_epoch_observed:7");
  request.option_envelopes.push_back("provider_generation_current:7");
  request.option_envelopes.push_back("provider_generation_observed:7");
  request.option_envelopes.push_back("provider_lifecycle_state:healthy");
  request.option_envelopes.push_back("default_policy_installed:true");
  return request;
}

void TestCryptoPrimitives() {
  Require(api::SecurityConstantTimeEqual(kVerifier, kVerifier),
          "constant-time equality rejected identical verifier");
  Require(!api::SecurityConstantTimeEqual(kVerifier, kWrongVerifier),
          "constant-time equality accepted different verifier");
  Require(api::SecuritySha256Hex("abc") ==
              "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
          "SHA-256 known answer mismatch");
  Require(api::SecurityHmacSha256Hex(
              "key", "The quick brown fox jumps over the lazy dog") ==
              "f7bc83f430538424b13298e6aa6fb143ef4d59a14946175997479dbc2d1a3cd8",
          "HMAC-SHA-256 known answer mismatch");
}

void TestLocalPasswordDurableState(const Fixture& fixture) {
  {
    std::ofstream sidecar(fixture.database_path.string() + ".sb.local_password_auth",
                          std::ios::trunc);
    sidecar << "alice\tlocal_password\t" << kVerifier << '\n';
    Require(static_cast<bool>(sidecar), "sidecar fixture write failed");
  }

  const auto sidecar_only = api::EngineAuthenticate(LocalPasswordRequest(
      fixture,
      "alice",
      fixture.principal,
      std::string(kInitialPassword),
      false));
  Require(!sidecar_only.ok &&
              HasDiagnosticDetail(sidecar_only,
                                  "durable_principal_credential_missing"),
          "sidecar local-password state or caller evidence was accepted as authority");

  const std::string initial_fingerprint =
      DeriveLocalPasswordFingerprint(kInitialPassword);
  CreatePrincipalCredential(fixture,
                            fixture.principal,
                            "alice",
                            initial_fingerprint,
                            91);
  GrantGlobalConnectPrivilege(fixture, fixture.principal, 92);

  const auto structured_verifier_claim = api::EngineAuthenticate(LocalPasswordRequest(
      fixture,
      "alice",
      fixture.principal,
      StructuredVerifierClaim("alice", fixture.principal, kVerifier)));
  Require(!structured_verifier_claim.ok &&
              HasDiagnosticDetail(structured_verifier_claim,
                                  "password_secret_required_for_pbkdf2_verification"),
          "caller-supplied verifier claim bypassed PBKDF2 password verification");

  auto initial_good_request = LocalPasswordRequest(
      fixture,
      "alice",
      fixture.principal,
      std::string(kInitialPassword),
      false);
  initial_good_request.context.trace_tags.push_back("group:APP");
  initial_good_request.context.trace_tags.push_back("right:OBS_MANAGEMENT_CONTROL");
  const auto good = api::EngineAuthenticate(initial_good_request);
  Require(good.ok && good.authenticated,
          "durable PBKDF2 local-password verifier was rejected");
  Require(good.connection_security_context.effective_user_uuid.canonical ==
              fixture.principal.canonical,
          "authenticated context did not use durable principal UUID");
  Require(HasEvidence(good,
                      "security_state_authority",
                      "mga_security_principal_lifecycle"),
          "MGA durable security authority evidence missing");
  Require(!HasEvidence(good, "authorized_group", "APP"),
          "client-supplied authorization group was trusted");
  Require(!HasAuthorizationTag(good, "right:OBS_MANAGEMENT_CONTROL"),
          "client-supplied management right was trusted");
  Require(HasAuthorizationTag(good, "right:CONNECT"),
          "server-side durable CONNECT grant was not materialized");

  const std::string rotated_fingerprint =
      DeriveLocalPasswordFingerprint(kPassword);
  Require(rotated_fingerprint != initial_fingerprint,
          "password rotation reused a PBKDF2 salt");
  AlterPrincipalCredential(fixture,
                           fixture.principal,
                           "alice",
                           rotated_fingerprint,
                           191);

  const auto previous_password = api::EngineAuthenticate(LocalPasswordRequest(
      fixture,
      "alice",
      fixture.principal,
      std::string(kInitialPassword),
      false));
  Require(!previous_password.ok &&
              HasDiagnosticDetail(previous_password, "credential_verifier_mismatch"),
          "rotated local password remained valid");

  const auto raw_mismatch = api::EngineAuthenticate(LocalPasswordRequest(
      fixture,
      "alice",
      fixture.principal,
      std::string(kWrongPassword),
      false));
  Require(!raw_mismatch.ok &&
              HasDiagnosticDetail(raw_mismatch, "credential_verifier_mismatch"),
          "raw local-password mismatch was accepted");

  const auto raw_good = api::EngineAuthenticate(LocalPasswordRequest(
      fixture,
      "alice",
      fixture.principal,
      std::string(kPassword),
      false));
  Require(raw_good.ok && raw_good.authenticated,
          "raw local-password credential was rejected");
  Require(raw_good.connection_security_context.effective_user_uuid.canonical ==
              fixture.principal.canonical,
          "raw password auth did not resolve the durable principal UUID from server state");
  Require(HasEvidence(raw_good,
                      "security_state_authority",
                      "mga_security_principal_lifecycle"),
          "raw password auth did not publish server-derived security authority evidence");
  Require(HasAuthorizationTag(raw_good, "right:CONNECT"),
          "raw password auth did not materialize server-derived CONNECT authority");
}

void TestPrincipalNameDoesNotEscalate(const Fixture& fixture) {
  CreatePrincipalCredential(fixture,
                            fixture.admin_principal,
                            "admin",
                            DeriveLocalPasswordFingerprint(kPassword),
                            92);
  const auto admin = api::EngineAuthenticate(LocalPasswordRequest(
      fixture,
      "admin",
      fixture.admin_principal,
      std::string(kPassword),
      false));
  Require(admin.ok && admin.authenticated,
          "durable admin-named principal PBKDF2 verifier was rejected");
  Require(!HasEvidence(admin, "bootstrap_authority", "true"),
          "admin name synthesized bootstrap authority");
  Require(!HasEvidence(admin, "authorized_group", "ROOT") &&
              !HasEvidence(admin, "authorized_group", "DBA") &&
              !HasEvidence(admin, "authorized_group", "SEC") &&
              !HasEvidence(admin, "authorized_group", "OPS"),
          "admin name synthesized privileged group evidence");
  Require(!HasAuthorizationTag(admin, "security.bootstrap") &&
              !HasAuthorizationTag(admin, "group:ROOT"),
          "admin name synthesized privileged authorization tags");
}

void TestTemporaryTokenDurableDigest(const Fixture& fixture) {
  const std::string token = "pcr091-token";
  const std::string digest = api::SecuritySha256Hex(token);
  AlterPrincipalCredential(fixture,
                           fixture.principal,
                           "alice",
                           TemporaryTokenFingerprint("token-handle-pcr091", digest),
                           93);
  const auto good = api::EngineAuthenticate(
      TokenRequest(fixture, TokenEvidence("alice", fixture.principal, token)));
  Require(good.ok && good.authenticated,
          "durable temporary token digest was rejected");
  Require(HasEvidence(good, "security_state_authority", "durable_security_catalog"),
          "temporary-token durable state authority evidence missing");

  const auto missing_durable_state = api::EngineAuthenticate(TokenRequest(
      fixture,
      TokenEvidence("alice", fixture.unused_principal, token)));
  Require(!missing_durable_state.ok &&
              HasDiagnosticDetail(missing_durable_state,
                                  "durable_principal_credential_missing"),
          "temporary-token auth accepted missing durable credential state");

  const auto wrong_token = api::EngineAuthenticate(TokenRequest(
      fixture,
      TokenEvidence("alice", fixture.principal, "pcr091-wrong-token")));
  Require(!wrong_token.ok &&
              HasDiagnosticDetail(wrong_token,
                                  "security_database_temporary_token_not_found"),
          "temporary-token auth accepted mismatched durable digest");

  const auto revoked = api::EngineAuthenticate(TokenRequest(
      fixture,
      TokenEvidence("alice", fixture.principal, token, "revoked")));
  Require(!revoked.ok &&
              HasDiagnosticDetail(revoked,
                                  "security_database_temporary_token_revoked"),
          "temporary-token auth accepted revoked durable token state");
}

void TestSecurityLifecycleStableTokenUsesSha256(const Fixture& fixture) {
  api::EngineSecurityCreatePrincipalRequest request;
  request.context = SecurityAdminContext(fixture, 94);
  request.target_object.uuid = fixture.lifecycle_principal;
  request.target_object.object_kind = "security_principal";
  request.principal_uuid = fixture.lifecycle_principal.canonical;
  request.principal_name = "lifecycle-proof";
  request.credential_protected_material_ref =
      "protected-material-ref:v1:sha256:pcr091-local-password-verifier";
  const auto created = api::EngineSecurityCreatePrincipal(request);
  Require(created.ok && created.principal_created,
          "security principal lifecycle did not create principal");
  Require(FieldValue(created, "credential_fingerprint").empty(),
          "security lifecycle response exposed the credential fingerprint");
  const auto loaded = api::LoadSecurityPrincipalLifecycleState(request.context);
  Require(loaded.ok, "security lifecycle durable state did not reload");
  std::string fingerprint;
  for (const auto& principal : loaded.state.principals) {
    if (principal.principal_uuid == fixture.lifecycle_principal.canonical) {
      fingerprint = principal.credential_fingerprint;
      break;
    }
  }
  Require(fingerprint.rfind("credential-fingerprint:v1:sha256:", 0) == 0,
          "durable security lifecycle credential token did not use SHA-256");
  Require(fingerprint.find("fnv") == std::string::npos,
          "security lifecycle credential token exposed legacy FNV marker");
}

}  // namespace

int main(int argc, char** argv) {
  Require(argc == 2, "usage: public_security_durable_crypto_hardening_gate <tmp-dir>");
  Fixture fixture;
  fixture.work_dir = argv[1];
  std::error_code ignored;
  std::filesystem::remove_all(fixture.work_dir, ignored);
  Require(std::filesystem::create_directories(fixture.work_dir),
          "failed to create PCR-091 work directory");
  fixture.database_path = fixture.work_dir / "pcr091.sbdb";
  Require(!std::filesystem::exists(fixture.database_path),
          "PCR-091 must exercise the nonexistent-path security lifecycle");

  TestCryptoPrimitives();
  TestLocalPasswordDurableState(fixture);
  TestPrincipalNameDoesNotEscalate(fixture);
  TestTemporaryTokenDurableDigest(fixture);
  TestSecurityLifecycleStableTokenUsesSha256(fixture);

  std::filesystem::remove_all(fixture.work_dir, ignored);
  std::cout << "public_security_durable_crypto_hardening_gate=passed\n";
  return EXIT_SUCCESS;
}

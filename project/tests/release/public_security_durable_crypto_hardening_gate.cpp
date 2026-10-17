#include "../support/engine_evidence_fixture.hpp"
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
#include "transaction/transaction_api.hpp"
#include "../support/engine_statement_fixture.hpp"
#include "../support/owned_temp_directory.hpp"
#include "memory.hpp"
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
#include <memory>
#include <stdexcept>
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
  throw std::runtime_error(std::string(message));
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
  out = generated.value.value;
  return out;
}

struct Fixture {
  scratchbird::tests::OwnedTempDirectory directory;
  api::EngineUuid database = MakeUuid(UuidKind::database, 1);
  api::EngineUuid principal = MakeUuid(UuidKind::principal, 2);
  api::EngineUuid admin_principal = MakeUuid(UuidKind::principal, 3);
  api::EngineUuid unused_principal = MakeUuid(UuidKind::principal, 6);
  api::EngineUuid lifecycle_principal = MakeUuid(UuidKind::principal, 7);
  api::EngineUuid global_connect_grant = MakeUuid(UuidKind::object, 8);
  std::filesystem::path work_dir = directory.path();
  std::filesystem::path database_path = work_dir / "pcr091.sbdb";
  scratchbird::storage::database::DatabaseCreateConfig create;
  Fixture() {
    namespace db = scratchbird::storage::database;
    create.path = database_path.string();
    create.database_uuid = {UuidKind::database, database};
    create.filespace_uuid = {UuidKind::filespace, MakeUuid(UuidKind::filespace, 9)};
    scratchbird::tests::ConfigureCredentialedFixtureBootstrap(create);
    const auto made = db::CreateDatabaseFile(create);
    if (!made.ok()) std::cerr << made.diagnostic.diagnostic_code << ':' << made.diagnostic.message_key << '\n';
    Require(made.ok() && made.create_finality == db::DatabaseCreateFinalityClass::committed,
            "PCR-091 real credentialed node creation did not durably complete");
    Require(scratchbird::tests::BootstrapFixtureOwnerContext(create).principal_uuid != admin_principal,
            "unprivileged admin-named test principal must not be the bootstrap owner");
  }
};

api::EngineRequestContext Context(const Fixture& fixture) {
  api::EngineRequestContext context;
  context.trust_mode = api::EngineTrustMode::server_isolated;
  context.request_id = "pcr091-security-durable-crypto";
  context.database_path = fixture.database_path.string();
  context.database_uuid = fixture.database;
  return context;
}

void RequireOk(const api::EngineApiResult& result, std::string_view message) {
  if (!result.ok) for (const auto& diagnostic : result.diagnostics)
    std::cerr << diagnostic.code << ':' << diagnostic.detail << '\n';
  Require(result.ok, message);
}

class AdminTransaction final {
 public:
  explicit AdminTransaction(const Fixture& fixture)
      : context(scratchbird::tests::BootstrapFixtureOwnerContext(fixture.create)),
        session(std::make_unique<scratchbird::tests::FixtureEngineSession>(context)) {
    api::EngineBeginTransactionRequest begin;
    begin.context = context;
    begin.isolation_level = "read_committed";
    auto started = api::EngineBeginTransaction(begin);
    RequireOk(started, "PCR-091 real administrative BEGIN failed");
    context.transaction_uuid = started.transaction_uuid;
    context.local_transaction_id = started.local_transaction_id;
    context.snapshot_visible_through_local_transaction_id = started.snapshot_visible_through_local_transaction_id;
    context.transaction_isolation_level = std::move(started.isolation_level);
    active = true;
  }
  ~AdminTransaction() noexcept {
    if (!active) return;
    api::EngineRollbackTransactionRequest rollback;
    rollback.context = context;
    if (!api::EngineRollbackTransaction(rollback).ok) std::terminate();
  }
  AdminTransaction(const AdminTransaction&) = delete;
  AdminTransaction& operator=(const AdminTransaction&) = delete;
  void Finish(bool commit = true) {
    if (commit) {
      api::EngineCommitTransactionRequest request;
      request.context = context;
      RequireOk(api::EngineCommitTransaction(request), "PCR-091 administrative COMMIT failed");
    } else {
      api::EngineRollbackTransactionRequest request;
      request.context = context;
      RequireOk(api::EngineRollbackTransaction(request), "PCR-091 administrative ROLLBACK failed");
    }
    active = false;
    session->Close();
    session.reset();
  }
  api::EngineRequestContext context;
  std::unique_ptr<scratchbird::tests::FixtureEngineSession> session;
 private:
  bool active = false;
};

void RequireStoredCredential(const Fixture& fixture, const api::EngineUuid& identity,
    std::string_view name, std::string_view fingerprint, std::uint64_t creator_tx) {
  // This source has no active writer/session. Reload actual committed native
  // catalog state independently, not the mutation result or an event sidecar.
  const auto loaded = api::LoadSecurityPrincipalLifecycleState(Context(fixture));
  Require(loaded.ok, "committed credential did not independently reload");
  const api::EngineSecurityPrincipalRecord* exact = nullptr;
  for (const auto& record : loaded.state.principals) if (record.principal_uuid == identity) {
    Require(exact == nullptr, "committed credential principal identity duplicated");
    exact = &record;
  }
  Require(exact && !exact->deleted && exact->lifecycle_state == "active" &&
              exact->principal_name == name && exact->credential_fingerprint == fingerprint &&
              exact->creator_tx == creator_tx && exact->security_generation != 0,
          "committed native principal changed identity, credential or transaction ownership");
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
    if (evidence.evidence_kind == kind && scratchbird::tests::EvidenceTextEquals(evidence.evidence_id, id)) {
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
                               std::string credential_fingerprint) {
  AdminTransaction transaction(fixture);
  {
  scratchbird::tests::FixtureEngineRequest<api::EngineSecurityCreatePrincipalRequest> request(
      *transaction.session, transaction.context);
  request.target_object.uuid = principal_uuid;
  request.target_object.object_kind = "security_principal";
  request.principal_uuid = principal_uuid;
  request.principal_name = std::string(principal_name);
  request.credential_fingerprint = credential_fingerprint;
  const auto created = api::EngineSecurityCreatePrincipal(request);
  RequireOk(created, "real principal credential creation failed");
  Require(created.ok && created.principal_created,
          "failed to create durable principal credential state");
  }
  transaction.Finish();
  RequireStoredCredential(fixture, principal_uuid, principal_name, credential_fingerprint,
                          transaction.context.local_transaction_id);
}

void AlterPrincipalCredential(const Fixture& fixture,
                              const api::EngineUuid& principal_uuid,
                              std::string_view principal_name,
                              std::string credential_fingerprint) {
  AdminTransaction transaction(fixture);
  {
  scratchbird::tests::FixtureEngineRequest<api::EngineSecurityAlterPrincipalRequest> request(
      *transaction.session, transaction.context);
  request.target_object.uuid = principal_uuid;
  request.target_object.object_kind = "security_principal";
  request.principal_uuid = principal_uuid;
  request.principal_name = std::string(principal_name);
  request.credential_fingerprint = credential_fingerprint;
  const auto altered = api::EngineSecurityAlterPrincipal(request);
  RequireOk(altered, "real principal credential alteration failed");
  Require(altered.ok && altered.principal_altered,
          "failed to alter durable principal credential state");
  }
  transaction.Finish();
  RequireStoredCredential(fixture, principal_uuid, principal_name, credential_fingerprint,
                          transaction.context.local_transaction_id);
}

void GrantGlobalConnectPrivilege(const Fixture& fixture,
                                 const api::EngineUuid& principal_uuid) {
  AdminTransaction transaction(fixture);
  const auto grant_uuid = principal_uuid == fixture.principal
      ? fixture.global_connect_grant : MakeUuid(UuidKind::object, 10);
  {
  scratchbird::tests::FixtureEngineRequest<api::EngineSecurityGrantPrivilegeRequest> request(
      *transaction.session, transaction.context);
  request.grant_uuid = grant_uuid;
  request.grantee_uuid = principal_uuid;
  request.grantee_kind = "principal";
  request.target_object_uuid = {};
  request.target_object_kind.clear();
  request.privilege = "CONNECT";
  request.grant_effect = "allow";
  const auto granted = api::EngineSecurityGrantPrivilege(request);
  RequireOk(granted, "real CONNECT grant failed");
  Require(granted.ok && granted.privilege_granted,
          "failed to grant durable global CONNECT privilege");
  Require(granted.primary_object.uuid ==
              grant_uuid &&
              granted.primary_object.object_kind == "security_privilege_grant" &&
              granted.security_generation != 0,
          "global CONNECT grant result did not preserve exact durable identity");

  }
  transaction.Finish();
  const auto loaded = api::LoadSecurityPrincipalLifecycleState(Context(fixture));
  Require(loaded.ok, "global CONNECT grant durable state did not reload");
  const api::EngineSecurityPrivilegeGrantRecord* exact_grant = nullptr;
  for (const auto& grant : loaded.state.grants) {
    if (grant.grant_uuid == grant_uuid) {
      Require(exact_grant == nullptr,
              "global CONNECT durable grant identity was duplicated");
      exact_grant = &grant;
    }
  }
  Require(exact_grant != nullptr && !exact_grant->revoked,
          "active global CONNECT durable grant was not reloaded");
  Require(exact_grant->grantee_uuid == principal_uuid &&
              exact_grant->grantee_kind == "principal" &&
              exact_grant->target_object_uuid.is_nil() &&
              exact_grant->target_object_kind.empty() &&
              exact_grant->privilege == "CONNECT" &&
              exact_grant->grant_effect == "allow" &&
              exact_grant->grantor_principal_uuid ==
                  transaction.context.principal_uuid &&
              exact_grant->creator_tx == transaction.context.local_transaction_id &&
              exact_grant->security_generation != 0,
          "reloaded global CONNECT grant did not preserve exact authority fields");
}

std::string StructuredVerifierClaim(std::string_view principal,
                                    std::string_view verifier) {
  std::string evidence = "scheme=local_password_v1;principal=";
  evidence += principal;
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
  if (include_durable_hint && !principal_uuid.is_nil()) {
    request.durable_principal_uuid = principal_uuid;
  }
  return request;
}

struct TokenCredentialEvidence {
  api::EngineUuid principal_uuid;
  std::string payload;
};

TokenCredentialEvidence TokenEvidence(std::string_view principal,
                          const api::EngineUuid& principal_uuid,
                          std::string_view token,
                          std::string_view state = "active",
                          std::string_view expires_at_ms = "0") {
  const std::string digest = api::SecuritySha256Hex(token);
  std::string evidence =
      "scheme=security_database_temporary_token_v1;principal=";
  evidence += principal;
  evidence += ";storage_authority=durable_security_catalog;token=";
  evidence += token;
  evidence += ";token_handle=token-handle-pcr091;token_digest=";
  evidence += digest;
  evidence += ";state=";
  evidence += state;
  evidence += ";expires_at_ms=";
  evidence += expires_at_ms;
  return {principal_uuid, std::move(evidence)};
}

api::EngineAuthenticateRequest TokenRequest(const Fixture& fixture,
                                            TokenCredentialEvidence evidence) {
  api::EngineAuthenticateRequest request;
  request.context = Context(fixture);
  request.provider_family = "security_database_temporary_token";
  request.principal_claim = "alice";
  request.durable_principal_uuid = evidence.principal_uuid;
  request.credential_evidence = std::move(evidence.payload);
  request.credential_evidence_present = true;
  request.target_database.uuid = fixture.database;
  request.target_database.object_kind = "database";
  request.target_object.uuid = fixture.database;
  request.target_object.object_kind = "security_authority";
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
  const auto sidecar_path = fixture.database_path.string() + ".sb.local_password_auth";
  {
    std::ofstream sidecar(sidecar_path, std::ios::trunc);
    sidecar << "alice\tlocal_password\t" << kVerifier << '\n';
    Require(static_cast<bool>(sidecar), "sidecar fixture write failed");
  }

  const auto quarantined = scratchbird::storage::database::ReadDatabaseBootstrapSecurityCatalog(
      fixture.database_path.string());
  Require(!quarantined.ok() && quarantined.diagnostic.diagnostic_code ==
              "SB-DB-BOOTSTRAP-SECURITY-SIDECAR-FORBIDDEN" &&
              quarantined.diagnostic.message_key ==
              "storage.database_lifecycle.bootstrap_security_sidecar_forbidden",
          "legacy password sidecar did not produce the exact native security quarantine vector");
  const auto sidecar_only = api::EngineAuthenticate(LocalPasswordRequest(
      fixture,
      "alice",
      fixture.principal,
      std::string(kInitialPassword),
      false));
  const bool refused_sidecar = !sidecar_only.ok && !sidecar_only.authenticated &&
      !sidecar_only.durable_security_state && sidecar_only.primary_object.uuid.is_nil() &&
      sidecar_only.connection_security_context.effective_user_uuid.is_nil() &&
      sidecar_only.result_shape.rows.empty() && sidecar_only.diagnostics.size() == 1 &&
      sidecar_only.diagnostics.front().code == "SECURITY.CATALOG.PRIVATE_LIFECYCLE_CORRUPT" &&
      sidecar_only.diagnostics.front().message_key == quarantined.diagnostic.message_key &&
      sidecar_only.diagnostics.front().detail == "bootstrap_security_catalog_invalid" &&
      sidecar_only.diagnostics.front().error && sidecar_only.diagnostics.front().native_source.has_value();
  if (!refused_sidecar) {
    std::cerr << "unexpected sidecar-only authentication outcome: ok=" << sidecar_only.ok << '\n';
    for (const auto& diagnostic : sidecar_only.diagnostics)
      std::cerr << diagnostic.code << ':' << diagnostic.message_key << ':' << diagnostic.detail << '\n';
  }
  Require(refused_sidecar,
          "sidecar local-password state or caller evidence was accepted as authority");
  const auto& wrapper = sidecar_only.diagnostics.front();
  const auto& source = wrapper.native_source->record;
  const auto& expected = quarantined.diagnostic;
  Require(source.diagnostic_code == expected.diagnostic_code && source.message_key == expected.message_key &&
              source.status.code == expected.status.code && source.status.severity == expected.status.severity &&
              source.status.subsystem == expected.status.subsystem &&
              source.source_component == expected.source_component && source.remediation_hint == expected.remediation_hint &&
              source.arguments.size() == expected.arguments.size(),
          "authentication discarded the exact native sidecar quarantine cause");
  for (std::size_t i = 0; i < source.arguments.size(); ++i)
    Require(source.arguments[i].key == expected.arguments[i].key &&
                source.arguments[i].value == expected.arguments[i].value,
            "authentication changed private native quarantine arguments");
  const auto same_metadata = [](const auto& actual, const auto& registered) {
    if (actual.has_value() != registered.has_value()) return false;
    return !actual || (actual->code == registered->code && actual->severity == registered->severity &&
        actual->is_failure == registered->is_failure && actual->sqlstate == registered->sqlstate &&
        actual->numeric_binding == registered->numeric_binding && actual->retry_class == registered->retry_class &&
        actual->required_outcome == registered->required_outcome && actual->diagnostic_class == registered->diagnostic_class);
  };
  Require(same_metadata(wrapper.canonical_metadata,
              scratchbird::core::diagnostics::CaptureCanonicalDiagnosticMetadata(wrapper.code)) &&
              same_metadata(wrapper.native_source->canonical_metadata,
                  scratchbird::core::diagnostics::CaptureCanonicalDiagnosticMetadata(expected.diagnostic_code)) &&
              wrapper.fields.empty() && wrapper.identity_fields.empty(),
          "authentication lost canonical disclosure metadata or flattened private native cause into public fields");
  Require(std::filesystem::remove(sidecar_path) && !std::filesystem::exists(sidecar_path),
          "fixture password sidecar quarantine cleanup failed");
  const auto admitted = scratchbird::storage::database::ReadDatabaseBootstrapSecurityCatalog(
      fixture.database_path.string());
  Require(admitted.ok(), "valid native security catalog did not reopen after owned sidecar removal");

  // An absent credential on a valid admitted node is a different refusal from
  // quarantined legacy authority. Preserve the exact missing-principal control.
  const auto missing_alice = api::EngineAuthenticate(LocalPasswordRequest(
      fixture, "alice", fixture.principal, std::string(kInitialPassword), false));
  if (missing_alice.ok || !HasDiagnosticDetail(missing_alice, "durable_principal_credential_missing"))
    for (const auto& diagnostic : missing_alice.diagnostics)
      std::cerr << diagnostic.code << ':' << diagnostic.message_key << ':' << diagnostic.detail << '\n';
  Require(!missing_alice.ok && !missing_alice.authenticated && !missing_alice.durable_security_state &&
              HasDiagnosticDetail(missing_alice, "durable_principal_credential_missing"),
          "missing Alice credential was not refused by the valid native security catalog");

  const std::string initial_fingerprint =
      DeriveLocalPasswordFingerprint(kInitialPassword);
  CreatePrincipalCredential(fixture,
                            fixture.principal,
                            "alice",
                            initial_fingerprint);
  GrantGlobalConnectPrivilege(fixture, fixture.principal);

  const auto structured_verifier_claim = api::EngineAuthenticate(LocalPasswordRequest(
      fixture,
      "alice",
      fixture.principal,
      StructuredVerifierClaim("alice", kVerifier)));
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
  Require(good.connection_security_context.effective_user_uuid ==
              fixture.principal,
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
                           rotated_fingerprint);

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
  Require(raw_good.connection_security_context.effective_user_uuid ==
              fixture.principal,
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
                            DeriveLocalPasswordFingerprint(kPassword));
  GrantGlobalConnectPrivilege(fixture, fixture.admin_principal);
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
                           TemporaryTokenFingerprint("token-handle-pcr091", digest));
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
  AdminTransaction transaction(fixture);
  const std::string protected_reference = "protected-material-ref:v1:sha256:pcr091-local-password-verifier";
  {
  scratchbird::tests::FixtureEngineRequest<api::EngineSecurityCreatePrincipalRequest> request(
      *transaction.session, transaction.context);
  request.target_object.uuid = fixture.lifecycle_principal;
  request.target_object.object_kind = "security_principal";
  request.principal_uuid = fixture.lifecycle_principal;
  request.principal_name = "lifecycle-proof";
  request.credential_protected_material_ref = protected_reference;
  const auto created = api::EngineSecurityCreatePrincipal(request);
  RequireOk(created, "real stable-token principal creation failed");
  Require(created.ok && created.principal_created,
          "security principal lifecycle did not create principal");
  Require(FieldValue(created, "credential_fingerprint").empty(),
          "security lifecycle response exposed the credential fingerprint");
  }
  transaction.Finish();
  const auto loaded = api::LoadSecurityPrincipalLifecycleState(Context(fixture));
  Require(loaded.ok, "security lifecycle durable state did not reload");
  std::string fingerprint;
  for (const auto& principal : loaded.state.principals) {
    if (principal.principal_uuid == fixture.lifecycle_principal) {
      fingerprint = principal.credential_fingerprint;
      break;
    }
  }
  Require(fingerprint.rfind("credential-fingerprint:v1:sha256:", 0) == 0,
          "durable security lifecycle credential token did not use SHA-256");
  Require(fingerprint.find("fnv") == std::string::npos,
          "security lifecycle credential token exposed legacy FNV marker");
  const std::string identity_bytes(reinterpret_cast<const char*>(fixture.lifecycle_principal.bytes.data()),
                                    fixture.lifecycle_principal.bytes.size());
  Require(fingerprint == "credential-fingerprint:v1:sha256:" +
              api::SecuritySha256Hex(identity_bytes + protected_reference),
          "durable security lifecycle token did not hash the exact binary identity and protected reference");
  RequireStoredCredential(fixture, fixture.lifecycle_principal, "lifecycle-proof", fingerprint,
                          transaction.context.local_transaction_id);
}

void TestCredentialRollback(const Fixture& fixture) {
  AdminTransaction transaction(fixture);
  {
    scratchbird::tests::FixtureEngineRequest<api::EngineSecurityCreatePrincipalRequest> request(
        *transaction.session, transaction.context);
    request.principal_uuid = fixture.unused_principal;
    request.target_object.uuid = fixture.unused_principal;
    request.target_object.object_kind = "security_principal";
    request.principal_name = "rollback-only";
    request.credential_fingerprint = DeriveLocalPasswordFingerprint(kPassword);
    const auto created = api::EngineSecurityCreatePrincipal(request);
    RequireOk(created, "credential rollback positive staging failed");
    Require(created.principal_created, "credential rollback did not stage a real principal");
    const auto staged = api::LoadSecurityPrincipalLifecycleState(request.context);
    Require(staged.ok, "credential rollback staged state did not load");
    bool seen = false;
    for (const auto& principal : staged.state.principals)
      seen = seen || principal.principal_uuid == fixture.unused_principal;
    Require(seen, "credential rollback fixture had no actual staged principal");
  }
  transaction.Finish(false);
  const auto reopened = api::LoadSecurityPrincipalLifecycleState(Context(fixture));
  Require(reopened.ok, "post-rollback independent native security reopen failed");
  for (const auto& principal : reopened.state.principals)
    Require(principal.principal_uuid != fixture.unused_principal,
            "rolled-back principal was visible after independent reopen");
}

}  // namespace

int main(int argc, char** argv) try {
  Require(argc == 2, "usage: public_security_durable_crypto_hardening_gate <tmp-dir>");
  Require(argv[1] != nullptr && argv[1][0] != '\0', "PCR-091 legacy temporary-path hint is empty");
  // The historical path argument is not cleanup authority. A private owned
  // directory is created under the admitted test runtime's temporary root.
  const auto configured = scratchbird::core::memory::ConfigureDefaultMemoryManagerForFixture(
      scratchbird::core::memory::DefaultLocalEngineMemoryPolicy(), "pcr091-durable-crypto");
  Require(configured.ok() && configured.fixture_mode, "PCR-091 explicit memory fixture setup failed");
  Fixture fixture;
  Require(std::filesystem::exists(fixture.database_path),
          "PCR-091 must exercise actual durable native node authority");

  TestCryptoPrimitives();
  TestLocalPasswordDurableState(fixture);
  TestPrincipalNameDoesNotEscalate(fixture);
  TestCredentialRollback(fixture);
  TestTemporaryTokenDurableDigest(fixture);
  TestSecurityLifecycleStableTokenUsesSha256(fixture);

  fixture.directory.Cleanup();
  std::cout << "public_security_durable_crypto_hardening_gate=passed\n";
  return EXIT_SUCCESS;
} catch (const std::exception& error) {
  std::cerr << error.what() << '\n';
  return EXIT_FAILURE;
}

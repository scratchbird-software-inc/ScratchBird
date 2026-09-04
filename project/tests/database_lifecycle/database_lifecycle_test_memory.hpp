// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "api_types.hpp"
#include "database_lifecycle.hpp"
#include "memory.hpp"
#include "security/security_crypto_policy.hpp"
#include "security/security_principal_lifecycle.hpp"
#include "local_transaction_store.hpp"
#include "transaction_inventory.hpp"

#include <openssl/crypto.h>
#include <openssl/evp.h>

#include <array>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <initializer_list>
#include <string>
#include <string_view>

namespace scratchbird::tests::database_lifecycle {

inline void ConfigureLifecycleMemoryFixture(std::string_view provenance) {
  const auto configured =
      core::memory::ConfigureDefaultMemoryManagerForFixture(
          core::memory::DefaultLocalEngineMemoryPolicy(),
          std::string(provenance));
  if (!configured.ok()) {
    std::cerr << configured.diagnostic.diagnostic_code << ':'
              << configured.diagnostic.message_key << '\n';
    std::exit(EXIT_FAILURE);
  }
  if (!configured.fixture_mode) {
    std::cerr << "database_lifecycle memory fixture mode was not active\n";
    std::exit(EXIT_FAILURE);
  }
}

inline std::string CanonicalTestUuid(std::string_view provenance,
                                     std::string_view suffix) {
  return std::string(provenance) + ":" + std::string(suffix);
}

inline engine::internal_api::EngineUuid DurableTestUuid(std::string_view uuid) {
  engine::internal_api::EngineUuid value;
  value.canonical = std::string(uuid);
  return value;
}

struct DurableBootstrapTransaction {
  std::filesystem::path database_path;
  engine::internal_api::EngineUuid transaction_uuid;
  std::uint64_t local_transaction_id = 0;
  std::uint64_t snapshot_visible_through_local_transaction_id = 0;
};

inline thread_local std::uint64_t g_bootstrap_security_context_generation = 1;

inline DurableBootstrapTransaction BeginDurableBootstrapTransaction(
    const std::filesystem::path& database_path, std::string_view provenance) {
  namespace db = scratchbird::storage::database;
  namespace mga = scratchbird::transaction::mga;
  const auto loaded = db::LoadLocalTransactionInventoryFromDatabase(
      database_path.string());
  if (!loaded.ok()) {
    std::cerr << loaded.diagnostic.diagnostic_code << ':'
              << loaded.diagnostic.message_key << '\n';
    std::exit(EXIT_FAILURE);
  }
  const auto generated = scratchbird::core::uuid::GenerateEngineIdentityV7(
      scratchbird::core::platform::UuidKind::transaction, 1780700000100ull);
  if (!generated.ok()) {
    std::cerr << generated.diagnostic.diagnostic_code << ':'
              << generated.diagnostic.message_key << '\n';
    std::exit(EXIT_FAILURE);
  }
  const auto begun = mga::BeginLocalTransaction(
      loaded.inventory, generated.value, 1780700000100ull);
  if (!begun.ok()) {
    std::cerr << begun.diagnostic.diagnostic_code << ':'
              << begun.diagnostic.message_key << '\n';
    std::exit(EXIT_FAILURE);
  }
  const auto persisted = db::PersistLocalTransactionInventoryToDatabase(
      database_path.string(), begun.inventory);
  if (!persisted.ok()) {
    std::cerr << persisted.diagnostic.diagnostic_code << ':'
              << persisted.diagnostic.message_key << '\n';
    std::exit(EXIT_FAILURE);
  }
  DurableBootstrapTransaction result;
  result.database_path = database_path;
  result.transaction_uuid.canonical = scratchbird::core::uuid::UuidToString(
      generated.value.value);
  result.local_transaction_id = begun.entry.identity.local_id.value;
  result.snapshot_visible_through_local_transaction_id =
      begun.entry.begin_visible_through_local_transaction_id;
  g_bootstrap_security_context_generation = 1;
  (void)provenance;
  return result;
}

inline void CommitDurableBootstrapTransaction(
    const DurableBootstrapTransaction& transaction) {
  namespace db = scratchbird::storage::database;
  namespace mga = scratchbird::transaction::mga;
  const auto loaded = db::LoadLocalTransactionInventoryFromDatabase(
      transaction.database_path.string());
  if (!loaded.ok()) {
    std::cerr << loaded.diagnostic.diagnostic_code << ':'
              << loaded.diagnostic.message_key << '\n';
    std::exit(EXIT_FAILURE);
  }
  const auto committed = mga::CommitLocalTransaction(
      loaded.inventory,
      mga::MakeLocalTransactionId(transaction.local_transaction_id),
      1780700000200ull);
  if (!committed.ok()) {
    std::cerr << committed.diagnostic.diagnostic_code << ':'
              << committed.diagnostic.message_key << '\n';
    std::exit(EXIT_FAILURE);
  }
  const auto persisted = db::PersistLocalTransactionInventoryToDatabase(
      transaction.database_path.string(), committed.inventory);
  if (!persisted.ok()) {
    std::cerr << persisted.diagnostic.diagnostic_code << ':'
              << persisted.diagnostic.message_key << '\n';
    std::exit(EXIT_FAILURE);
  }
}

inline std::string LocalPasswordVerifierFingerprint(std::string_view verifier) {
  constexpr int kIterations = 600000;
  const std::array<unsigned char, 16> salt = {
      0x53, 0x63, 0x72, 0x61, 0x74, 0x63, 0x68, 0x42,
      0x69, 0x72, 0x64, 0x54, 0x65, 0x73, 0x74, 0x31};
  std::array<unsigned char, 32> derived{};
  const bool ok = !verifier.empty() && verifier.size() <= 1024 &&
                  verifier.find('\0') == std::string_view::npos &&
                  PKCS5_PBKDF2_HMAC(
                      verifier.data(), static_cast<int>(verifier.size()),
                      salt.data(), static_cast<int>(salt.size()), kIterations,
                      EVP_sha256(), static_cast<int>(derived.size()),
                      derived.data()) == 1;
  if (!ok) {
    std::cerr << "database_lifecycle PBKDF2 password fixture derivation failed\n";
    std::exit(EXIT_FAILURE);
  }
  const auto lower_hex = [](const auto& bytes) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string encoded;
    encoded.reserve(bytes.size() * 2);
    for (const auto byte : bytes) {
      encoded.push_back(kHex[(byte >> 4) & 0x0f]);
      encoded.push_back(kHex[byte & 0x0f]);
    }
    return encoded;
  };
  const std::string fingerprint =
      "local-password-pbkdf2-sha256:v1:iterations=" +
      std::to_string(kIterations) + ":salt=" + lower_hex(salt) +
      ":verifier=" + lower_hex(derived);
  OPENSSL_cleanse(derived.data(), derived.size());
  return fingerprint;
}

inline void MaterializeAuthorizationRights(
    engine::internal_api::EngineRequestContext* context,
    std::string_view provenance,
    std::initializer_list<std::string_view> rights);

inline void CreateDurableLocalPasswordPrincipal(
    const std::filesystem::path& database_path,
    std::string_view database_uuid,
    std::string_view principal_uuid,
    std::string_view principal_name,
    std::string_view verifier,
    std::uint64_t local_transaction_id,
    std::string_view provenance,
    std::string_view transaction_uuid =
        "019e108d-1700-7000-8000-00000000d002",
    std::string_view credential_fingerprint = {}) {
  engine::internal_api::EngineSecurityCreatePrincipalRequest request;
  request.context.trust_mode = engine::internal_api::EngineTrustMode::embedded_in_process;
  request.context.database_path = database_path.string();
  request.context.database_uuid = DurableTestUuid(database_uuid);
  request.context.principal_uuid = DurableTestUuid(principal_uuid);
  request.context.session_uuid =
      DurableTestUuid("019e108d-1700-7000-8000-00000000d001");
  request.context.transaction_uuid = DurableTestUuid(transaction_uuid);
  request.context.security_context_present = true;
  request.context.trace_tags.push_back("security.bootstrap");
  request.context.local_transaction_id = local_transaction_id;
  request.context.snapshot_visible_through_local_transaction_id =
      local_transaction_id;
  request.context.catalog_generation_id = 1;
  request.context.security_epoch = 1;
  request.target_object.uuid = DurableTestUuid(principal_uuid);
  request.target_object.object_kind = "security_principal";
  request.principal_uuid = std::string(principal_uuid);
  request.principal_name = std::string(principal_name);
  request.credential_fingerprint = credential_fingerprint.empty()
      ? LocalPasswordVerifierFingerprint(verifier)
      : std::string(credential_fingerprint);
  request.option_envelopes.push_back("principal_authority:engine");
  MaterializeAuthorizationRights(&request.context, provenance,
                                 {"SEC_IDENTITY_ADMIN"});
  const auto created =
      engine::internal_api::EngineSecurityCreatePrincipal(request);
  if (!created.ok || !created.principal_created) {
    for (const auto& diagnostic : created.diagnostics) {
      std::cerr << diagnostic.code << ':' << diagnostic.detail << '\n';
    }
    std::cerr << provenance
              << " durable local-password principal creation failed\n";
    std::exit(EXIT_FAILURE);
  }
  ++g_bootstrap_security_context_generation;
}

inline void GrantDurablePrincipalPrivilege(
    const std::filesystem::path& database_path,
    std::string_view database_uuid,
    std::string_view principal_uuid,
    std::string_view target_object_uuid,
    std::string_view target_object_kind,
    std::string_view privilege,
    std::uint64_t local_transaction_id,
    std::string_view provenance,
    std::string_view transaction_uuid =
        "019e108d-1700-7000-8000-00000000d002") {
  engine::internal_api::EngineSecurityGrantPrivilegeRequest request;
  request.context.trust_mode = engine::internal_api::EngineTrustMode::embedded_in_process;
  request.context.database_path = database_path.string();
  request.context.database_uuid = DurableTestUuid(database_uuid);
  request.context.principal_uuid = DurableTestUuid(principal_uuid);
  request.context.session_uuid =
      DurableTestUuid("019e108d-1700-7000-8000-00000000d101");
  request.context.transaction_uuid = DurableTestUuid(transaction_uuid);
  request.context.security_context_present = true;
  request.context.trace_tags.push_back("security.bootstrap");
  request.context.local_transaction_id = local_transaction_id;
  request.context.snapshot_visible_through_local_transaction_id =
      local_transaction_id;
  request.context.catalog_generation_id = 1;
  request.context.security_epoch = 1;
  request.target_object.uuid = DurableTestUuid(target_object_uuid);
  request.target_object.object_kind = std::string(target_object_kind);
  request.grantee_uuid = std::string(principal_uuid);
  request.grantee_kind = "principal";
  request.target_object_uuid = std::string(target_object_uuid);
  request.target_object_kind = std::string(target_object_kind);
  request.privilege = std::string(privilege);
  request.grant_effect = "allow";
  request.option_envelopes.push_back("grant_authority:engine");
  MaterializeAuthorizationRights(&request.context, provenance,
                                 {"SEC_GRANT_ADMIN"});
  const auto granted =
      engine::internal_api::EngineSecurityGrantPrivilege(request);
  if (!granted.ok || !granted.privilege_granted) {
    for (const auto& diagnostic : granted.diagnostics) {
      std::cerr << diagnostic.code << ':' << diagnostic.detail << '\n';
    }
    std::cerr << provenance
              << " durable privilege grant failed: " << privilege << '\n';
    std::exit(EXIT_FAILURE);
  }
  ++g_bootstrap_security_context_generation;
}

inline std::string DurableLocalPasswordEvidence(
    std::string_view principal_name,
    std::string_view principal_uuid,
    std::string_view verifier,
    std::string_view authorization_tags = {}) {
  (void)principal_name;
  (void)principal_uuid;
  (void)authorization_tags;
  return std::string(verifier);
}

inline void MaterializeAuthorizationRights(
    engine::internal_api::EngineRequestContext* context,
    std::string_view provenance,
    std::initializer_list<std::string_view> rights) {
  if (context->principal_uuid.canonical.empty()) {
    context->principal_uuid.canonical = CanonicalTestUuid(provenance, "principal");
  }
  if (context->security_epoch == 0) {
    context->security_epoch = 1;
  }
  if (context->catalog_generation_id == 0) {
    context->catalog_generation_id = 1;
  }
  context->security_context_present = true;

  auto& authorization = context->authorization_context;
  authorization.present = true;
  authorization.authority_uuid.canonical =
      context->database_uuid.canonical.empty()
          ? CanonicalTestUuid(provenance, "authority")
          : context->database_uuid.canonical + ":authority";
  // CreateDatabaseFile publishes the bootstrap security context at generation
  // one.  Every lifecycle mutation in this fixture is staged under the one
  // durable bootstrap transaction and must carry that exact generation until
  // the first successor is durably published.
  authorization.security_context_generation =
      g_bootstrap_security_context_generation;
  authorization.principal_uuid = context->principal_uuid;
  authorization.security_epoch = context->security_epoch;
  authorization.policy_epoch = context->security_epoch;
  authorization.catalog_generation_id = context->catalog_generation_id;
  authorization.effective_subjects.clear();
  authorization.grants.clear();
  authorization.policies.clear();
  authorization.effective_subjects.push_back(
      {context->principal_uuid, "principal"});
  std::uint64_t index = 0;
  for (std::string_view right : rights) {
    engine::internal_api::EngineMaterializedAuthorizationGrant grant;
    grant.grant_uuid.canonical =
        CanonicalTestUuid(provenance, "grant-" + std::to_string(++index));
    grant.subject_uuid = context->principal_uuid;
    grant.subject_kind = "principal";
    grant.right = std::string(right);
    grant.security_epoch = context->security_epoch;
    authorization.grants.push_back(std::move(grant));
  }
  authorization.evidence_tags = {
      "database_lifecycle_operational_test_materialized_authorization",
      "grants:" + std::to_string(authorization.grants.size())};
}

}  // namespace scratchbird::tests::database_lifecycle

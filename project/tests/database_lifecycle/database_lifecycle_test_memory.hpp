// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "api_types.hpp"
#include "memory.hpp"
#include "security/security_crypto_policy.hpp"
#include "security/security_principal_lifecycle.hpp"

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

inline std::string LocalPasswordVerifierFingerprint(std::string_view verifier) {
  return "local-password-verifier:v1:sha256:" +
         engine::internal_api::SecuritySha256Hex(verifier);
}

inline void CreateDurableLocalPasswordPrincipal(
    const std::filesystem::path& database_path,
    std::string_view database_uuid,
    std::string_view principal_uuid,
    std::string_view principal_name,
    std::string_view verifier,
    std::uint64_t local_transaction_id,
    std::string_view provenance) {
  engine::internal_api::EngineSecurityCreatePrincipalRequest request;
  request.context.trust_mode = engine::internal_api::EngineTrustMode::server_isolated;
  request.context.database_path = database_path.string();
  request.context.database_uuid = DurableTestUuid(database_uuid);
  request.context.principal_uuid = DurableTestUuid(principal_uuid);
  request.context.session_uuid =
      DurableTestUuid("019e108d-1700-7000-8000-00000000d001");
  request.context.transaction_uuid =
      DurableTestUuid("019e108d-1700-7000-8000-00000000d002");
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
  request.credential_fingerprint = LocalPasswordVerifierFingerprint(verifier);
  request.option_envelopes.push_back("principal_authority:engine");
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
}

inline std::string DurableLocalPasswordEvidence(
    std::string_view principal_name,
    std::string_view principal_uuid,
    std::string_view verifier,
    std::string_view authorization_tags = {}) {
  std::string evidence = "scheme=local_password_v1;principal=";
  evidence += principal_name;
  evidence += ";principal_uuid=";
  evidence += principal_uuid;
  evidence += ";storage_authority=mga_security_principal_lifecycle";
  if (!authorization_tags.empty()) {
    evidence += ";authorization_tags=";
    evidence += authorization_tags;
  }
  evidence += ";verifier=";
  evidence += verifier;
  return evidence;
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

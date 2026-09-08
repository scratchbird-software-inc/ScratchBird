// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "database_lifecycle.hpp"
#include "security/security_principal_lifecycle.hpp"
#include "transaction/transaction_api.hpp"
#include "uuid.hpp"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <unistd.h>
#include <vector>

namespace {

namespace api = scratchbird::engine::internal_api;
namespace database = scratchbird::storage::database;
namespace platform = scratchbird::core::platform;
namespace uuid = scratchbird::core::uuid;

constexpr std::uint64_t kBaseMillis = 1970000000000ull;

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view message) {
  if (!condition) Fail(message);
}

api::EngineUuid MakeUuid(platform::UuidKind kind, std::uint64_t offset) {
  const auto generated =
      uuid::GenerateEngineIdentityV7(kind, kBaseMillis + offset);
  Require(generated.ok(), "privilege-template UUID generation failed");
  api::EngineUuid result;
  result.canonical = uuid::UuidToString(generated.value.value);
  return result;
}

struct Fixture {
  std::filesystem::path root;
  std::filesystem::path database_path;
  api::EngineUuid database_uuid = MakeUuid(platform::UuidKind::database, 1);
  api::EngineUuid filespace_uuid = MakeUuid(platform::UuidKind::filespace, 2);
  api::EngineUuid admin_uuid = MakeUuid(platform::UuidKind::principal, 3);

  ~Fixture() {
    std::error_code error;
    if (!root.empty()) std::filesystem::remove_all(root, error);
  }
};

Fixture CreateFixture() {
  std::string pattern = "/tmp/sb_privilege_template_catalog.XXXXXX";
  std::vector<char> writable(pattern.begin(), pattern.end());
  writable.push_back('\0');
  char* created_root = ::mkdtemp(writable.data());
  Require(created_root != nullptr,
          "privilege-template temporary directory creation failed");

  Fixture fixture;
  fixture.root = created_root;
  fixture.database_path = fixture.root / "privilege-template.sbdb";
  const auto database_uuid = uuid::ParseUuid(fixture.database_uuid.canonical);
  const auto filespace_uuid = uuid::ParseUuid(fixture.filespace_uuid.canonical);
  Require(database_uuid.ok() && filespace_uuid.ok(),
          "privilege-template database identity parse failed");

  database::DatabaseCreateConfig config;
  config.path = fixture.database_path.string();
  config.database_uuid = {platform::UuidKind::database, database_uuid.value};
  config.filespace_uuid = {platform::UuidKind::filespace, filespace_uuid.value};
  config.page_size = 16384;
  config.creation_unix_epoch_millis = kBaseMillis;
  config.allow_minimal_resource_bootstrap = true;
  config.require_resource_seed_pack = false;
  config.allow_overwrite = false;
  const auto created = database::CreateDatabaseFile(config);
  if (!created.ok()) {
    std::cerr << created.diagnostic.diagnostic_code << ':'
              << created.diagnostic.message_key << '\n';
  }
  Require(created.ok(), "privilege-template database creation failed");
  return fixture;
}

api::EngineRequestContext BaseContext(const Fixture& fixture,
                                      std::uint64_t session_ordinal) {
  api::EngineRequestContext context;
  context.trust_mode = api::EngineTrustMode::embedded_in_process;
  context.request_id = "ia09-privilege-template-catalog";
  context.database_path = fixture.database_path.string();
  context.database_uuid = fixture.database_uuid;
  context.principal_uuid = fixture.admin_uuid;
  context.session_uuid = MakeUuid(platform::UuidKind::object,
                                  100 + session_ordinal);
  context.catalog_generation_id = 1;
  context.security_epoch = 1;
  context.resource_epoch = 1;
  context.name_resolution_epoch = 1;
  context.security_context_present = true;
  context.trace_tags = {"security.fixture_trace_authority",
                        "right:POLICY_ADMIN",
                        "right:SEC_GRANT_ADMIN",
                        "right:OBS_POLICY_READ"};
  return context;
}

api::EngineRequestContext Begin(const Fixture& fixture,
                                std::uint64_t session_ordinal) {
  api::EngineBeginTransactionRequest request;
  request.context = BaseContext(fixture, session_ordinal);
  request.isolation_level = "read_committed";
  const auto begun = api::EngineBeginTransaction(request);
  if (!begun.ok) {
    for (const auto& diagnostic : begun.diagnostics) {
      std::cerr << diagnostic.code << ':' << diagnostic.detail << '\n';
    }
  }
  Require(begun.ok, "privilege-template transaction begin failed");
  auto context = request.context;
  context.transaction_uuid = begun.transaction_uuid;
  context.local_transaction_id = begun.local_transaction_id;
  context.snapshot_visible_through_local_transaction_id =
      begun.snapshot_visible_through_local_transaction_id;
  context.transaction_isolation_level = begun.isolation_level;
  return context;
}

void Commit(const api::EngineRequestContext& context) {
  api::EngineCommitTransactionRequest request;
  request.context = context;
  const auto committed = api::EngineCommitTransaction(request);
  Require(committed.ok && committed.engine_finality_known,
          "privilege-template transaction commit failed");
}

void Rollback(const api::EngineRequestContext& context) {
  api::EngineRollbackTransactionRequest request;
  request.context = context;
  const auto rolled_back = api::EngineRollbackTransaction(request);
  Require(rolled_back.ok && rolled_back.engine_finality_known,
          "privilege-template transaction rollback failed");
}

const api::EngineSecurityPrivilegeTemplateRecord* FindTemplate(
    const api::EngineSecurityPrincipalLifecycleState& state,
    std::string_view template_uuid) {
  const api::EngineSecurityPrivilegeTemplateRecord* found = nullptr;
  for (const auto& candidate : state.privilege_templates) {
    if (candidate.template_uuid != template_uuid) continue;
    Require(found == nullptr,
            "privilege-template durable identity was duplicated");
    found = &candidate;
  }
  return found;
}

api::EngineSecurityCreatePrivilegeTemplateRequest CreateRequest(
    const api::EngineRequestContext& context,
    const api::EngineUuid& template_uuid,
    std::string name,
    std::string grantee_uuid,
    std::string idempotency_key) {
  api::EngineSecurityCreatePrivilegeTemplateRequest request;
  request.context = context;
  request.template_uuid = template_uuid.canonical;
  request.template_name = std::move(name);
  request.object_kinds = {"TABLES"};
  request.grantee_uuids = {std::move(grantee_uuid)};
  request.privileges = {"select"};
  request.grant_option_privileges = {"SELECT"};
  request.enabled = true;
  request.idempotency_key = std::move(idempotency_key);
  return request;
}

void RequireDiagnostic(const api::EngineApiResult& result,
                       std::string_view code,
                       std::string_view message) {
  Require(!result.ok && !result.diagnostics.empty() &&
              result.diagnostics.front().code == code,
          message);
}

}  // namespace

int main() {
  auto fixture = CreateFixture();
  const auto template_uuid = MakeUuid(platform::UuidKind::object, 200);
  const auto conflicting_uuid = MakeUuid(platform::UuidKind::object, 201);
  const auto rolled_back_uuid = MakeUuid(platform::UuidKind::object, 202);
  const auto cancelled_uuid = MakeUuid(platform::UuidKind::object, 203);
  const auto invalid_uuid = MakeUuid(platform::UuidKind::object, 204);

  auto writer = Begin(fixture, 1);
  const auto initial = api::LoadSecurityPrincipalLifecycleState(writer);
  Require(initial.ok && !initial.state.roles.empty(),
          "privilege-template bootstrap grantee authority missing");
  const std::string grantee_uuid = initial.state.roles.front().role_uuid;

  auto create = CreateRequest(writer, template_uuid, "future_readers",
                              grantee_uuid, "create-future-readers-v1");
  const auto created = api::EngineSecurityCreatePrivilegeTemplate(create);
  if (!created.ok) {
    for (const auto& diagnostic : created.diagnostics) {
      std::cerr << diagnostic.code << ':' << diagnostic.detail << '\n';
    }
  }
  Require(created.ok && created.template_created &&
              !created.exact_idempotent_replay &&
              created.template_generation != 0 &&
              created.privilege_template.object_kinds ==
                  std::vector<std::string>{"table"} &&
              created.privilege_template.privileges ==
                  std::vector<std::string>{"SELECT"} &&
              created.privilege_template.grant_option_privileges ==
                  std::vector<std::string>{"SELECT"},
          "privilege-template durable create failed");

  const auto replayed = api::EngineSecurityCreatePrivilegeTemplate(create);
  Require(replayed.ok && replayed.template_created &&
              replayed.exact_idempotent_replay &&
              replayed.template_generation == created.template_generation,
          "privilege-template exact idempotent replay failed");
  const auto writer_state = api::LoadSecurityPrincipalLifecycleState(writer);
  Require(writer_state.ok &&
              FindTemplate(writer_state.state, template_uuid.canonical) !=
                  nullptr &&
              writer_state.state.privilege_templates.size() == 1,
          "privilege-template writer did not observe one durable row");

  auto conflicting = CreateRequest(
      writer, conflicting_uuid, "future_readers", grantee_uuid,
      "conflicting-template-name-v1");
  const auto conflict =
      api::EngineSecurityCreatePrivilegeTemplate(conflicting);
  RequireDiagnostic(conflict,
                    api::kSecurityPrivilegeTemplateDiagnosticInvalidScope,
                    "privilege-template name conflict was admitted");

  auto unauthorized = CreateRequest(
      writer, invalid_uuid, "unauthorized_template", grantee_uuid,
      "unauthorized-template-v1");
  unauthorized.context.trace_tags = {"security.fixture_trace_authority",
                                     "right:POLICY_ADMIN"};
  const auto unauthorized_result =
      api::EngineSecurityCreatePrivilegeTemplate(unauthorized);
  RequireDiagnostic(unauthorized_result,
                    api::kSecurityPrivilegeTemplateDiagnosticUnauthorized,
                    "privilege-template missing grant authority was admitted");

  auto invalid_grantee = CreateRequest(
      writer, invalid_uuid, "invalid_grantee_template",
      MakeUuid(platform::UuidKind::principal, 999).canonical,
      "invalid-grantee-template-v1");
  const auto invalid_grantee_result =
      api::EngineSecurityCreatePrivilegeTemplate(invalid_grantee);
  RequireDiagnostic(invalid_grantee_result,
                    api::kSecurityPrivilegeTemplateDiagnosticGrantForbidden,
                    "privilege-template invisible grantee was admitted");

  auto cancelled = CreateRequest(writer, cancelled_uuid,
                                 "cancelled_template", grantee_uuid,
                                 "cancelled-template-v1");
  cancelled.context.query_cancellation_requested = [] { return true; };
  const auto cancelled_result =
      api::EngineSecurityCreatePrivilegeTemplate(cancelled);
  RequireDiagnostic(cancelled_result, "PROCESS.CANCELLED",
                    "privilege-template cancellation was ignored");

  auto cluster = CreateRequest(writer, invalid_uuid, "cluster_template",
                               grantee_uuid, "cluster-template-v1");
  cluster.context.cluster_transaction_active = true;
  const auto cluster_result =
      api::EngineSecurityCreatePrivilegeTemplate(cluster);
  RequireDiagnostic(cluster_result,
                    "CLUSTER.GATEWAY_CLUSTER_FALLTHROUGH_FORBIDDEN",
                    "privilege-template cluster fallthrough was admitted");

  auto concurrent_observer = Begin(fixture, 2);
  const auto before_commit =
      api::LoadSecurityPrincipalLifecycleState(concurrent_observer);
  Require(before_commit.ok &&
              FindTemplate(before_commit.state, template_uuid.canonical) ==
                  nullptr,
          "uncommitted privilege-template leaked to a concurrent session");
  Commit(writer);
  Rollback(concurrent_observer);

  auto committed_observer = Begin(fixture, 3);
  const auto after_commit =
      api::LoadSecurityPrincipalLifecycleState(committed_observer);
  const auto* durable =
      FindTemplate(after_commit.state, template_uuid.canonical);
  Require(after_commit.ok && durable != nullptr && durable->enabled &&
              durable->template_name == "future_readers" &&
              durable->grantee_uuids ==
                  std::vector<std::string>{grantee_uuid} &&
              durable->creation_transaction_uuid ==
                  created.privilege_template.creation_transaction_uuid &&
              durable->source_catalog_generation == 1 &&
              durable->source_policy_generation != 0 &&
              durable->source_security_generation != 0,
          "committed privilege-template did not reload exactly");
  Rollback(committed_observer);

  auto rollback_writer = Begin(fixture, 4);
  auto rollback_create = CreateRequest(
      rollback_writer, rolled_back_uuid, "rolled_back_template",
      grantee_uuid, "rolled-back-template-v1");
  const auto rollback_created =
      api::EngineSecurityCreatePrivilegeTemplate(rollback_create);
  Require(rollback_created.ok && rollback_created.template_created,
          "rollback privilege-template setup failed");
  const auto own_uncommitted =
      api::LoadSecurityPrincipalLifecycleState(rollback_writer);
  Require(own_uncommitted.ok &&
              FindTemplate(own_uncommitted.state,
                           rolled_back_uuid.canonical) != nullptr,
          "writer did not observe its rollback candidate");
  Rollback(rollback_writer);

  auto restart_observer = Begin(fixture, 5);
  const auto restarted =
      api::LoadSecurityPrincipalLifecycleState(restart_observer);
  Require(restarted.ok &&
              FindTemplate(restarted.state, template_uuid.canonical) !=
                  nullptr &&
              FindTemplate(restarted.state, rolled_back_uuid.canonical) ==
                  nullptr &&
              FindTemplate(restarted.state, cancelled_uuid.canonical) ==
                  nullptr &&
              FindTemplate(restarted.state, invalid_uuid.canonical) ==
                  nullptr &&
              restarted.state.privilege_templates.size() == 1,
          "privilege-template restart/rollback/no-mutation observation failed");
  Rollback(restart_observer);

  std::cout << "CSC-TEST-005829 privilege-template durable catalog passed\n";
  return EXIT_SUCCESS;
}

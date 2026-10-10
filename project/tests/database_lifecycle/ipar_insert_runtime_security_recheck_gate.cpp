// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "../support/binary_uuid_fixture.hpp"
#include "../support/engine_evidence_fixture.hpp"
#include "../support/published_mga_table_fixture.hpp"
#include "../support/catalog_column_binding_fixture.hpp"
#include "../support/engine_statement_fixture.hpp"
#include "../support/owned_temp_directory.hpp"
#include "ddl/create_api.hpp"
#include "security/security_principal_lifecycle.hpp"
#include "database_lifecycle.hpp"
#include "memory.hpp"
#include "dml/insert_api.hpp"
#include "mga_relation_store/mga_relation_store.hpp"
#include "transaction/transaction_api.hpp"
#include "uuid.hpp"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <stdexcept>
#include <vector>

namespace {

namespace api = scratchbird::engine::internal_api;
namespace db = scratchbird::storage::database;
namespace uuid = scratchbird::core::uuid;
using scratchbird::core::platform::UuidKind;

constexpr auto kSchemaUuid = scratchbird::tests::FixtureUuidLiteral("019f2000-0000-7000-8000-000000000001");
constexpr auto kTableUuid = scratchbird::tests::FixtureUuidLiteral("019f2000-0000-7000-8000-000000000101");
constexpr auto kPrincipalUuid = scratchbird::tests::FixtureUuidLiteral("019f2000-0000-7000-8000-000000000201");
constexpr auto kGroupUuid = scratchbird::tests::FixtureUuidLiteral("019f2000-0000-7000-8000-000000000202");

void Require(bool condition, std::string_view message) {
  if (!condition) {
    throw std::runtime_error(std::string(message));
  }
}

template <typename TResult>
bool HasDiagnostic(const TResult& result, std::string_view code) {
  for (const auto& diagnostic : result.diagnostics) {
    if (diagnostic.code == code) {
      return true;
    }
  }
  return false;
}

bool HasEvidence(const api::EngineApiResult& result,
                 std::string_view kind,
                 std::string_view needle = {}) {
  for (const auto& evidence : result.evidence) {
    if (evidence.evidence_kind != kind) {
      continue;
    }
    if (needle.empty() ||
        scratchbird::tests::EvidenceTextFind(evidence.evidence_id, needle) != std::string::npos) {
      return true;
    }
  }
  return false;
}

api::EngineRequestContext CreateDatabase(const std::filesystem::path& path) {
  db::DatabaseCreateConfig create;
  create.path = path.string();
  create.database_uuid =
      uuid::GenerateEngineIdentityV7(UuidKind::database, 1779800200000).value;
  create.filespace_uuid =
      uuid::GenerateEngineIdentityV7(UuidKind::filespace, 1779800200001).value;
  create.page_size = 16384;
  create.creation_unix_epoch_millis = 1779800200002;
  scratchbird::tests::ConfigureCredentialedFixtureBootstrap(create);
  const auto created = db::CreateDatabaseFile(create);
  if (!created.ok()) {
    std::cerr << created.diagnostic.diagnostic_code << ":"
              << created.diagnostic.message_key << '\n';
  }
  Require(created.ok(), "IPAR runtime security database create failed");
  auto owner = scratchbird::tests::BootstrapFixtureOwnerContext(create);
  owner.current_schema_uuid = kSchemaUuid;
  return owner;
}

void RefreshOwner(api::EngineRequestContext& context) {
  scratchbird::tests::MaterializeBootstrapFixtureAuthorization(context);
  context.resource_epoch = context.authorization_context.policy_epoch;
}

api::EngineRequestContext Begin(api::EngineRequestContext context) {
  api::EngineBeginTransactionRequest request;
  request.context = context;
  request.isolation_level = "read_committed";
  const auto begun = api::EngineBeginTransaction(request);
  if (!begun.ok) {
    for (const auto& diagnostic : begun.diagnostics) {
      std::cerr << diagnostic.code << ":" << diagnostic.detail << '\n';
    }
  }
  Require(begun.ok, "IPAR runtime security begin failed");
  context.local_transaction_id = begun.local_transaction_id;
  context.transaction_uuid = begun.transaction_uuid;
  context.snapshot_visible_through_local_transaction_id =
      begun.snapshot_visible_through_local_transaction_id;
  context.transaction_isolation_level = begun.isolation_level;
  return context;
}

void Commit(const api::EngineRequestContext& context) {
  api::EngineCommitTransactionRequest request;
  request.context = context;
  const auto committed = api::EngineCommitTransaction(request);
  Require(committed.ok, "IPAR runtime security commit failed");
}

api::EngineTypedValue TextValue(std::string value) {
  api::EngineTypedValue typed;
  typed.descriptor.descriptor_kind = "scalar";
  typed.descriptor.canonical_type_name = "text";
  typed.descriptor.encoded_descriptor = "type=text";
  typed.encoded_value = std::move(value);
  typed.state = api::EngineValueState::value;
  return typed;
}

api::EngineRowValue Row(api::EngineUuid row_uuid, std::string tenant) {
  api::EngineRowValue row;
  row.requested_row_uuid = std::move(row_uuid);
  row.fields.push_back({"payload", TextValue("payload")});
  row.fields.push_back({"tenant", TextValue(std::move(tenant))});
  return row;
}

void SeedMetadata(api::EngineRequestContext& context) {
  api::EngineCatalogCreateObjectRequest schema;
  schema.context = context;
  schema.target_object.uuid = kSchemaUuid;
  schema.target_object.object_kind = "schema";
  schema.localized_names.push_back({"en", "primary", "", "runtime_security", true});
  Require(api::EngineCatalogCreateObject(schema).ok, "fixture schema publication failed");
  api::CrudTableRecord table;
  table.table_uuid = scratchbird::tests::FixtureUuidLiteral("019f2000-0000-7000-8000-000000000101");
  table.default_name = "ipar_runtime_security";
  table.columns.push_back({"tenant", "type=text;nullable=false"});
  table.columns.push_back({"payload", "type=text"});
  table.creator_tx = context.local_transaction_id;
  Require(!scratchbird::tests::PublishMgaTableFixture(
              context, table, {"text", "text"}).error,
          "IPAR runtime security bound table publication failed");
}

template<class Result>
void RequireOk(const Result& result, std::string_view message) {
  if (!result.ok) for (const auto& d : result.diagnostics)
    std::cerr << d.code << ':' << d.detail << '\n';
  Require(result.ok, message);
}

void SeedSecurity(api::EngineRequestContext& owner) {
  api::EngineSecurityCreatePrincipalRequest principal;
  principal.context = owner;
  principal.principal_uuid = kPrincipalUuid;
  principal.principal_name = "tenant_writer";
  principal.credential_protected_material_ref = "fixture:tenant_writer:credential";
  RequireOk(api::EngineSecurityCreatePrincipal(principal), "durable writer publication failed");
  RefreshOwner(owner);
  api::EngineSecurityCreateGroupRequest group;
  group.context = owner;
  group.group_uuid = kGroupUuid;
  group.group_name = "tenant_writers";
  RequireOk(api::EngineSecurityCreateGroup(group), "durable group publication failed");
  RefreshOwner(owner);
  api::EngineSecurityGrantMembershipRequest membership;
  membership.context = owner;
  membership.membership_uuid = scratchbird::tests::FixtureUuid(1559, 1);
  membership.member_principal_uuid = kPrincipalUuid;
  membership.container_uuid = kGroupUuid;
  membership.container_kind = "group";
  RequireOk(api::EngineSecurityGrantMembership(membership), "durable membership publication failed");
  RefreshOwner(owner);
  api::EngineSecurityGrantPrivilegeRequest grant;
  grant.context = owner;
  grant.grant_uuid = scratchbird::tests::FixtureUuid(1559, 2);
  grant.grantee_uuid = kGroupUuid;
  grant.grantee_kind = "group";
  grant.target_object_uuid = kTableUuid;
  grant.target_object_kind = "table";
  grant.privilege = "INSERT";
  RequireOk(api::EngineSecurityGrantPrivilege(grant), "durable group INSERT grant failed");
  RefreshOwner(owner);
  api::EngineSecurityPutRowPolicyRequest policy;
  policy.context = owner;
  policy.policy_uuid = scratchbird::tests::FixtureUuid(1559, 3);
  policy.target_object_uuid = kTableUuid;
  policy.target_object_kind = "table";
  policy.policy_effect = "row_filter";
  policy.predicate_envelope = "sblr_predicate:column_equals:tenant:tenant_a";
  policy.definer_principal_uuid = owner.principal_uuid;
  const auto published = api::EngineSecurityPutRowPolicy(policy);
  RequireOk(published, "durable row policy publication failed");
  Require(published.policy_persisted, "row policy must be persisted");
  RefreshOwner(owner);
}

api::EngineRequestContext WriterContext(const api::EngineRequestContext& owner,
                                       bool member = true) {
  auto context = owner;
  context.principal_uuid = kPrincipalUuid;
  context.session_uuid = api::GenerateCrudEngineUuid("object");
  context.request_id = "ipar-runtime-security-writer";
  // Construct the fixture's server-side authorization observation solely from
  // committed durable rows. The writer has no SYSARCH or direct INSERT grant.
  const auto loaded = api::LoadSecurityPrincipalLifecycleState(context);
  Require(loaded.ok, "committed security read failed");
  const auto& state = loaded.state;
  api::DurableAuthorizationState authority;
  authority.authority_uuid = context.database_uuid;
  authority.security_context_generation = state.security_context_generation;
  authority.security_epoch = state.security_generation;
  authority.policy_epoch = state.policy_generation;
  authority.catalog_generation_id = context.catalog_generation_id;
  for (const auto& p : state.principals) if (!p.deleted && p.lifecycle_state == "active")
    authority.principals.push_back({p.principal_uuid, "principal", true, state.security_generation});
  for (const auto& r : state.roles) if (!r.deleted && r.lifecycle_state == "active")
    authority.roles.push_back({r.role_uuid, true, state.security_generation});
  for (const auto& g : state.groups) if (!g.deleted && g.lifecycle_state == "active")
    authority.groups.push_back({g.group_uuid, true, state.security_generation});
  for (const auto& m : state.memberships) if (!m.revoked)
    authority.memberships.push_back({m.member_principal_uuid, "principal", m.container_uuid,
                                    m.container_kind, true, state.security_generation});
  for (const auto& g : state.grants) if (!g.revoked)
    authority.grants.push_back({g.grant_uuid, g.grantee_uuid, g.grantee_kind, g.target_object_uuid,
                               g.privilege, g.grant_effect == "deny", true, state.security_generation});
  for (const auto& p : state.row_policies) if (!p.deleted && p.target_object_uuid == kTableUuid) {
    api::DurableAuthorizationPolicyRecord policy;
    policy.policy_uuid = p.policy_uuid;
    policy.subject_uuid = context.principal_uuid;
    policy.subject_kind = "principal";
    policy.target_uuid = p.target_object_uuid;
    policy.right = "INSERT";
    policy.policy_kind = "rls_filter";
    policy.requires_runtime_recheck = true;
    policy.source_policy_generation = p.policy_generation;
    policy.policy_epoch = state.policy_generation;
    policy.canonical_policy_envelope = p.predicate_envelope;
    authority.policies.push_back(std::move(policy));
  }
  const auto materialized = api::MaterializeDurableAuthorizationContext(authority,
      {context.principal_uuid, authority.security_epoch, authority.policy_epoch,
       authority.catalog_generation_id});
  RequireOk(materialized, "durable writer authorization failed");
  context.authorization_context = materialized.context;
  context.security_epoch = authority.security_epoch;
  context.resource_epoch = authority.policy_epoch;
  Require((member ? (context.authorization_context.grants.size() == 1 &&
          context.authorization_context.grants.front().subject_uuid == kGroupUuid &&
          context.authorization_context.grants.front().subject_kind == "group" &&
          context.authorization_context.grants.front().right == "INSERT") :
          context.authorization_context.grants.empty()) &&
          context.authorization_context.policies.size() == 1,
          "writer must obtain INSERT only through its persisted group membership");
  return context;
}

api::EngineInsertRowsResult Insert(const scratchbird::tests::FixtureEngineSession& session,
                                   const api::EngineRequestContext& context,
                                   api::EngineRowValue row) {
  scratchbird::tests::FixtureEngineRequest<api::EngineInsertRowsRequest> request(session, context);
  request.target_schema.uuid = kSchemaUuid;
  request.target_table.uuid = scratchbird::tests::FixtureUuidLiteral("019f2000-0000-7000-8000-000000000101");
  request.target_table.object_kind = "table";
  request.target_object.uuid = scratchbird::tests::FixtureUuidLiteral("019f2000-0000-7000-8000-000000000101");
  request.target_object.object_kind = "table";
  request.bound_object_identity.object_uuid = kTableUuid;
  request.bound_object_identity.catalog_generation_id = context.catalog_generation_id;
  request.bound_object_identity.security_epoch = context.security_epoch;
  request.bound_object_identity.resource_epoch = context.resource_epoch;
  request.input_rows.push_back(std::move(row));
  return api::EngineInsertRows(request);
}

void VerifyRuntimeSecurityRecheck() {
  scratchbird::tests::OwnedTempDirectory directory;
  const auto owner = CreateDatabase(directory.path() / "runtime_security.sbdb");
  auto setup = Begin(owner);
  SeedMetadata(setup);
  SeedSecurity(setup);
  Commit(setup);

  auto allowed_context = Begin(WriterContext(owner));
  {
  scratchbird::tests::FixtureEngineSession session(allowed_context);
  const auto allowed = Insert(
      session, allowed_context,
      Row(scratchbird::tests::FixtureUuidLiteral("019f2000-0000-7000-8000-000000000501"), "tenant_a"));
  if (!allowed.ok) {
    for (const auto& diagnostic : allowed.diagnostics) {
      std::cerr << diagnostic.code << ":" << diagnostic.detail << '\n';
    }
  }
  Require(allowed.ok, "IPAR runtime security allowed insert failed");
  Require(HasEvidence(allowed, "insert_runtime_security_policy_evaluated"),
          "IPAR runtime security policy evaluation evidence missing");
  Require(HasEvidence(allowed, "insert_runtime_security_recheck", "rls=filter"),
          "IPAR runtime security filter recheck evidence missing");
  Commit(allowed_context);
  }

  auto denied_context = Begin(WriterContext(owner));
  {
  scratchbird::tests::FixtureEngineSession session(denied_context);
  const auto denied = Insert(
      session, denied_context,
      Row(scratchbird::tests::FixtureUuidLiteral("019f2000-0000-7000-8000-000000000502"), "tenant_b"));
  Require(!denied.ok, "IPAR runtime security denied insert was admitted");
  Require(HasDiagnostic(denied, "SECURITY.RLS.DENIED"),
          "IPAR runtime security denied insert diagnostic mismatch");
  Require(HasEvidence(denied, "insert_runtime_security_policy_result",
                      "column_equals:tenant:deny"),
          "IPAR runtime security deny evidence missing");
  Require(HasEvidence(denied, "security_deep_enforcement_refusal",
                      "rls_policy=deny"),
          "IPAR runtime security deep refusal evidence missing");
  api::EngineRollbackTransactionRequest rollback;
  rollback.context = denied_context;
  RequireOk(api::EngineRollbackTransaction(rollback), "denied transaction rollback failed");
  }
  // A matching tenant value is not sufficient: remove the durable membership,
  // acquire a new statement, and require authorization failure before effects.
  auto admin = owner;
  admin.session_uuid = api::GenerateCrudEngineUuid("object");
  RefreshOwner(admin);
  admin = Begin(admin);
  api::EngineSecurityRevokeMembershipRequest revoke;
  revoke.context = admin;
  revoke.member_principal_uuid = kPrincipalUuid;
  revoke.container_uuid = kGroupUuid;
  revoke.container_kind = "group";
  RequireOk(api::EngineSecurityRevokeMembership(revoke), "durable membership revocation failed");
  Commit(admin);
  auto revoked_context = Begin(WriterContext(owner, false));
  {
    scratchbird::tests::FixtureEngineSession session(revoked_context);
    const auto refused = Insert(session, revoked_context,
        Row(scratchbird::tests::FixtureUuid(1559, 4), "tenant_a"));
    Require(!refused.ok && HasDiagnostic(refused, "SECURITY.AUTHORIZATION.DENIED"),
            "revoked group membership still authorizes INSERT");
    Require(HasEvidence(refused, "security_deep_enforcement_refusal", "INSERT"),
            "revoked membership must fail at the deep INSERT authorization boundary");
    api::EngineRollbackTransactionRequest rollback;
    rollback.context = revoked_context;
    RequireOk(api::EngineRollbackTransaction(rollback), "revoked transaction rollback failed");
  }
  auto observer = owner;
  observer.session_uuid = api::GenerateCrudEngineUuid("object");
  RefreshOwner(observer);
  observer = Begin(observer);
  const auto stored = api::LoadMgaRelationStoreStateForRelationScans(observer, {kTableUuid});
  Require(stored.ok, "fresh committed row readback failed");
  std::size_t rows = 0;
  for (const auto& row : stored.state.row_versions) if (row.table_uuid == kTableUuid) {
    ++rows;
    Require(!row.deleted && row.creator_tx == allowed_context.local_transaction_id &&
            row.row_uuid == scratchbird::tests::FixtureUuidLiteral("019f2000-0000-7000-8000-000000000501"),
            "only the allowed row may be persisted");
    Require(row.values.size() == 2, "committed row lost fields");
    unsigned tenant_fields = 0, payload_fields = 0;
    for (const auto& [name, value] : row.values) {
      if (name == "tenant") ++tenant_fields;
      else if (name == "payload") ++payload_fields;
      else Require(false, "committed row contains an unexpected field");
      Require(value.isPresent() && value.bytes == (name == "tenant" ? "tenant_a" : "payload"),
              "committed row payload changed");
    }
    Require(tenant_fields == 1 && payload_fields == 1, "committed row field names are not exact");
  }
  Require(rows == 1, "denied INSERT published a row or allowed INSERT was lost");
  Commit(observer);
  directory.Cleanup();
}

}  // namespace

int main() try {
  namespace memory = scratchbird::core::memory;
  auto policy = memory::DefaultLocalEngineMemoryPolicy();
  policy.policy_name = "ipar_insert_runtime_security_recheck_gate";
  const auto configured = memory::ConfigureDefaultMemoryManagerForFixture(
      policy, "ipar_insert_runtime_security_recheck_gate");
  Require(configured.ok(), "IPAR runtime security memory policy admission failed");
  VerifyRuntimeSecurityRecheck();
  return EXIT_SUCCESS;
} catch (const std::exception& error) {
  std::cerr << error.what() << '\n';
  return EXIT_FAILURE;
}

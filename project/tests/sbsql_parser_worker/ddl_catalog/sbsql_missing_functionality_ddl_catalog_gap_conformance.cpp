// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "../../support/database_fixture_cleanup.hpp"
#include "../../database_lifecycle/credentialed_database_fixture.hpp"
#include "../../support/binary_uuid_fixture.hpp"
#include "catalog/catalog_object_lifecycle.hpp"
#include "catalog/schema_tree_api.hpp"
#include "catalog/schema_tree_codec.hpp"
#include "cst/cst.hpp"
#include "database_lifecycle.hpp"
#include "ddl/alter_api.hpp"
#include "ddl/create_api.hpp"
#include "ddl/drop_api.hpp"
#include "local_transaction_store.hpp"
#include "sblr_admission.hpp"
#include "sblr_opcode_registry.hpp"
#include "security/security_model.hpp"
#include "security/security_principal_lifecycle.hpp"
#include "transaction_inventory.hpp"
#include "uuid.hpp"

#include "../canonical_sblr_admission_test_helper.hpp"
#include "engine/sblr/sblr_ddl_create_schema_runtime.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

#include <stdexcept>
#include <utility>

namespace scratchbird::engine::internal_api {
template <typename... Args>
auto CheckedFindSchemaTreeRecord(Args&&... args) {
  EngineApiDiagnostic diagnostic;
  auto result = FindVisibleSchemaTreeRecord(std::forward<Args>(args)..., diagnostic);
  if (diagnostic.error) throw std::runtime_error(diagnostic.code + ":" + diagnostic.detail);
  return result;
}
}  // namespace scratchbird::engine::internal_api

namespace {

namespace api = scratchbird::engine::internal_api;
namespace db = scratchbird::storage::database;
namespace parser = scratchbird::parser::sbsql;
namespace server = scratchbird::server;
namespace sblr = scratchbird::engine::sblr;
namespace txn = scratchbird::transaction::mga;
namespace uuid = scratchbird::core::uuid;
using scratchbird::core::platform::UuidKind;

constexpr auto kSchemaUuid = scratchbird::tests::FixtureUuidLiteral("019f0700-0000-7000-8000-000000000101");
constexpr auto kLifecycleSchemaUuid = scratchbird::tests::FixtureUuidLiteral("019f0700-0000-7000-8000-000000000102");
constexpr auto kUnionMemberUuid = scratchbird::tests::FixtureUuidLiteral("019f0700-0000-7000-8000-000000000103");
constexpr auto kUnionMember2Uuid = scratchbird::tests::FixtureUuidLiteral("019f0700-0000-7000-8000-000000000104");
constexpr auto kUdrUuid = scratchbird::tests::FixtureUuidLiteral("019f0700-0000-7000-8000-000000000201");
constexpr auto kConstraintUuid = scratchbird::tests::FixtureUuidLiteral("019f0700-0000-7000-8000-000000000202");
constexpr auto kColumnUuid = scratchbird::tests::FixtureUuidLiteral("019f0700-0000-7000-8000-000000000203");
constexpr auto kGroupUuid = scratchbird::tests::FixtureUuidLiteral("019f0700-0000-7000-8000-000000000204");
constexpr auto kFilespaceUuid = scratchbird::tests::FixtureUuidLiteral("019f0700-0000-7000-8000-000000000205");
constexpr auto kClusterUuid = scratchbird::tests::FixtureUuidLiteral("019f0700-0000-7000-8000-000000000206");
constexpr auto kNodeUuid = scratchbird::tests::FixtureUuidLiteral("019f0700-0000-7000-8000-000000000207");

constexpr auto kAuditUuid = scratchbird::tests::FixtureUuid(1578, 1);

struct Fixture {
  std::filesystem::path path;
  api::EngineUuid database_uuid;
  api::EngineUuid transaction_uuid;
  std::uint64_t local_transaction_id = 0;
  std::uint64_t resource_epoch = 0;
  txn::LocalTransactionId typed_local_transaction_id;
  txn::LocalTransactionInventory inventory;
};

void Require(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << message << '\n';
    std::exit(EXIT_FAILURE);
  }
}

bool Contains(std::string_view haystack, std::string_view needle) {
  return haystack.find(needle) != std::string_view::npos;
}

void Cleanup(const std::filesystem::path& path) {
  scratchbird::tests::RemoveDatabaseFixtureArtifacts(path);
}

bool HasEvidence(const api::EngineApiResult& result,
                 std::string_view kind,
                 std::string_view value = {}) {
  for (const auto& evidence : result.evidence) {
    if (evidence.evidence_kind == kind &&
        (value.empty() || (std::holds_alternative<std::string>(evidence.evidence_id) && std::get<std::string>(evidence.evidence_id) == value))) {
      return true;
    }
  }
  return false;
}

std::string MetadataUuidBytes(const api::EngineUuid& identity) {
  return {reinterpret_cast<const char*>(identity.bytes.data()), identity.bytes.size()};
}

bool HasEvidence(const api::EngineApiResult& result, std::string_view kind,
                 const api::EngineUuid& identity) {
  for (const auto& evidence : result.evidence) {
    const auto* value = std::get_if<api::EngineUuid>(&evidence.evidence_id);
    if (evidence.evidence_kind == kind && value && *value == identity) return true;
  }
  return false;
}

bool SchemaHasIdentity(std::string_view payload, const std::string& key,
                       const api::EngineUuid& identity) {
  std::vector<api::EngineLocalizedName> names;
  std::vector<std::pair<std::string, std::string>> comments;
  api::BinaryCatalogMetadata metadata;
  if (!api::DecodeSchemaTreeMetadata(payload, &names, &comments, &metadata)) return false;
  const auto it = metadata.identities.find(key);
  return it != metadata.identities.end() && it->second == identity;
}

bool AnySchemaIdentity(const api::EngineApiResult& result, const std::string& key,
                       const api::EngineUuid& identity) {
  for (const auto& row : result.result_shape.rows) {
    for (const auto& [name, value] : row.fields) {
      if (name != "payload" || value.is_null) continue;
      // The behavior-row carrier currently stores serialized metadata in its
      // byte-preserving string member. Decode the framed metadata, never UUID text.
      if (!value.binary_value.empty() && !value.encoded_value.empty()) continue;
      const std::string_view bytes = value.binary_value.empty()
          ? std::string_view(value.encoded_value)
          : std::string_view(reinterpret_cast<const char*>(value.binary_value.data()),
                             value.binary_value.size());
      if (SchemaHasIdentity(bytes, key, identity)) return true;
    }
  }
  return false;
}

bool HasDiagnostic(const api::EngineApiResult& result, std::string_view code) {
  for (const auto& diagnostic : result.diagnostics) {
    if (diagnostic.code == code) return true;
  }
  return false;
}

std::string FieldValue(const api::EngineApiResult& result,
                       std::string_view field,
                       std::size_t row_index = 0) {
  if (row_index >= result.result_shape.rows.size()) return {};
  for (const auto& [name, value] : result.result_shape.rows[row_index].fields) {
    if (name == field) return value.encoded_value;
  }
  return {};
}

bool AnyFieldContains(const api::EngineApiResult& result,
                      std::string_view field,
                      std::string_view expected) {
  for (std::size_t i = 0; i < result.result_shape.rows.size(); ++i) {
    if (Contains(FieldValue(result, field, i), expected)) return true;
  }
  return false;
}

api::EngineLocalizedName Name(std::string path, std::string name) {
  api::EngineLocalizedName localized;
  localized.language_tag = "en";
  localized.name_class = "canonical";
  localized.path = std::move(path);
  localized.name = std::move(name);
  localized.default_name = true;
  return localized;
}

api::EngineRequestContext Context(const Fixture& fixture, bool grant_catalog_mutate) {
  api::EngineRequestContext context;
  context.database_path = fixture.path.string();
  context.database_uuid = fixture.database_uuid;
  const auto bootstrap = db::ReadDatabaseBootstrapSecurityCatalog(fixture.path.string());
  Require(bootstrap.ok() && bootstrap.state.present && bootstrap.state.committed_by_inventory,
          "durable bootstrap principal unavailable");
  context.principal_uuid = bootstrap.state.principal_uuid.value;
  context.session_uuid = scratchbird::tests::FixtureUuidLiteral("019f0700-0000-7000-8000-000000000010");
  context.transaction_uuid = fixture.transaction_uuid;
  context.local_transaction_id = fixture.local_transaction_id;
  context.snapshot_visible_through_local_transaction_id =
      fixture.local_transaction_id;
  const auto catalog = api::LoadCatalogObjectLifecycleState(context);
  Require(catalog.ok, "durable catalog epoch unavailable");
  // A verified fresh bootstrap precedes the first catalog mutation event.
  // Catalog consumers use generation one for that initial namespace.
  context.catalog_generation_id = std::max<std::uint64_t>(1, catalog.state.metadata_epoch);
  context.resource_epoch = fixture.resource_epoch;
  context.security_context_present = true;
  const auto loaded = api::LoadSecurityPrincipalLifecycleState(context);
  if (!loaded.ok) throw std::runtime_error("driver fixture durable security catalog unavailable");
  const auto& lifecycle = loaded.state;
  if (!lifecycle.row_policies.empty())
    throw std::runtime_error("driver fixture requires a fresh bootstrap security catalog");
  api::DurableAuthorizationState authority;
  authority.authority_uuid = context.database_uuid;
  authority.security_context_generation = lifecycle.security_context_generation;
  authority.security_epoch = lifecycle.security_generation;
  authority.policy_epoch = lifecycle.policy_generation;
  authority.catalog_generation_id = context.catalog_generation_id;
  authority.engine_owned_sysarch_role_uuid = bootstrap.state.sysarch_role_uuid.value;
  context.security_epoch = authority.security_epoch;
  for (const auto& principal : lifecycle.principals)
    if (!principal.deleted && principal.lifecycle_state == "active")
      authority.principals.push_back({principal.principal_uuid, "principal", true,
                                     authority.security_epoch});
  for (const auto& role : lifecycle.roles)
    if (!role.deleted && role.lifecycle_state == "active")
      authority.roles.push_back({role.role_uuid, true, authority.security_epoch});
  for (const auto& membership : lifecycle.memberships)
    if (!membership.revoked)
      authority.memberships.push_back({membership.member_principal_uuid, "principal",
          membership.container_uuid, membership.container_kind, true, authority.security_epoch});
  for (const auto& grant : lifecycle.grants)
    if (!grant.revoked)
      authority.grants.push_back({grant.grant_uuid, grant.grantee_uuid, grant.grantee_kind,
          grant.target_object_uuid, grant.privilege, grant.grant_effect == "deny", true,
          authority.security_epoch});
  const auto materialized = api::MaterializeDurableAuthorizationContext(authority,
      {context.principal_uuid, authority.security_epoch, authority.policy_epoch,
       authority.catalog_generation_id});
  if (!materialized.ok) throw std::runtime_error("driver fixture durable authorization materialization failed");
  context.authorization_context = materialized.context;
  // The negative fixture has no admitted authorization context.
  if (!grant_catalog_mutate) context.authorization_context = {};
  return context;
}

Fixture CreateFixture() {
  Fixture fixture;
  fixture.path = std::filesystem::temp_directory_path() /
                 "scratchbird_sbsql_miss007_ddl_catalog.sbdb";
  Cleanup(fixture.path);

  const auto database_uuid =
      uuid::GenerateEngineIdentityV7(UuidKind::database, 1790700000000);
  Require(database_uuid.ok(), "database UUID generation failed");
  const auto filespace_uuid =
      uuid::GenerateEngineIdentityV7(UuidKind::filespace, 1790700000001);
  Require(filespace_uuid.ok(), "filespace UUID generation failed");

  db::DatabaseCreateConfig create;
  create.path = fixture.path.string();
  create.database_uuid = database_uuid.value;
  create.filespace_uuid = filespace_uuid.value;
  create.page_size = 16384;
  create.creation_unix_epoch_millis = 1790700000002;
  create.allow_minimal_resource_bootstrap = true;
  create.require_resource_seed_pack = false;
  create.allow_overwrite = true;
  create.bootstrap_principal_name = std::string(scratchbird::tests::database_lifecycle::kCredentialedFixturePrincipal);
  create.bootstrap_credential_fingerprint = std::string(scratchbird::tests::database_lifecycle::kCredentialedFixtureFingerprint);
  create.require_bootstrap_principal = true;
  create.allow_uncredentialed_bootstrap = false;
  const auto created = db::CreateDatabaseFile(create);
  Require(created.ok(), "database creation failed");
  fixture.resource_epoch = created.state.resource_seed_catalog.resource_epoch;

  auto initial_inventory = db::LoadLocalTransactionInventoryFromDatabase(fixture.path.string());
  Require(initial_inventory.ok() && initial_inventory.inventory.publication_base.has_value(),
          "lifecycle-published transaction inventory unavailable");
  fixture.inventory = std::move(initial_inventory.inventory);
  const auto transaction_uuid =
      uuid::GenerateEngineIdentityV7(UuidKind::transaction, 1790700000003);
  Require(transaction_uuid.ok(), "transaction UUID generation failed");
  auto begun = txn::BeginLocalTransaction(std::move(fixture.inventory),
                                          transaction_uuid.value,
                                          1790700000004);
  Require(begun.ok(), "local transaction begin failed");
  fixture.inventory = std::move(begun.inventory);
  fixture.database_uuid = database_uuid.value.value;
  fixture.transaction_uuid = transaction_uuid.value.value;
  fixture.local_transaction_id = begun.entry.identity.local_id.value;
  fixture.typed_local_transaction_id = begun.entry.identity.local_id;
  Require(db::PersistLocalTransactionInventoryToDatabase(fixture.path.string(),
                                                         fixture.inventory)
              .ok(),
          "active transaction inventory persist failed");
  return fixture;
}

void CommitFixture(Fixture* fixture) {
  auto published = db::LoadLocalTransactionInventoryFromDatabase(fixture->path.string());
  Require(published.ok() && published.inventory.publication_base.has_value(),
          "current transaction inventory unavailable before commit");
  fixture->inventory = std::move(published.inventory);
  auto committed = txn::CommitLocalTransaction(std::move(fixture->inventory),
                                               fixture->typed_local_transaction_id,
                                               1790700000100);
  Require(committed.ok(), "local transaction commit failed");
  const auto reopen_transaction_uuid =
      uuid::GenerateEngineIdentityV7(UuidKind::transaction, 1790700000101);
  Require(reopen_transaction_uuid.ok(),
          "reopen transaction UUID generation failed");
  auto reopened = txn::BeginLocalTransaction(std::move(committed.inventory),
                                             reopen_transaction_uuid.value,
                                             1790700000102);
  Require(reopened.ok(), "reopen transaction begin failed");
  fixture->inventory = std::move(reopened.inventory);
  fixture->transaction_uuid =
      reopen_transaction_uuid.value.value;
  fixture->local_transaction_id = reopened.entry.identity.local_id.value;
  fixture->typed_local_transaction_id = reopened.entry.identity.local_id;
  Require(db::PersistLocalTransactionInventoryToDatabase(fixture->path.string(),
                                                         fixture->inventory)
              .ok(),
          "reopen transaction inventory persist failed");
}

std::string AdmissionFrame(std::string operation_id, std::string family) {
  return "envelope=SBLRExecutionEnvelope.v3\n"
         "envelope_major=3\n"
         "sblr_version=sblr_v3\n"
         "operation_id=" + operation_id + "\n"
         "sblr_operation_family=" + family + "\n"
         "result_shape=rs.ddl.catalog.gap.v1\n"
         "diagnostic_shape=diag.ddl.catalog.gap.v1\n"
         "parser_resolved_names_to_uuids=true\n"
         "contains_sql_text=false\n"
         "engine_api_command_route=true\n"
         "public_sbsql_exact_command=true\n";
}

void RequireParserUnionGrammar() {
  const auto cst = parser::BuildCst(
      "SELECT 1 AS sample_value UNION SELECT 2 AS sample_value;");
  for (const auto& diagnostic : cst.messages.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.message << '\n';
  }
  Require(!cst.messages.has_errors(), "UNION grammar CST parse failed");
}

void RequireAdmissionAndOpcodeRegistry() {
  constexpr std::string_view kCreateSchemaOperation =
      "engine.op.ddl_create_schema";
  constexpr std::string_view kCreateSchemaOpcode = "SBLR_DDL_CREATE_SCHEMA";
  const auto* create_schema =
      sblr::LookupSblrOperation(kCreateSchemaOperation);
  Require(create_schema != nullptr,
          "CREATE SCHEMA opcode registry row missing");
  Require(create_schema->opcode == kCreateSchemaOpcode &&
              create_schema->code == 1536 &&
              create_schema->operand_contract == "create_schema_descriptor" &&
              create_schema->result_contract == "ddl_result" &&
              create_schema->requires_security_context,
          "CREATE SCHEMA canonical registry tuple drifted");

  sblr::SblrDdlCreateSchemaDescriptorV1 descriptor;
  descriptor.receipt[15] = 1;
  descriptor.occurrence = 1;
  descriptor.schema_occurrence = 1;
  descriptor.schema_uuid[15] = 2;
  descriptor.schema_generation = 1;
  descriptor.database_uuid[15] = 3;
  descriptor.owning_transaction_uuid[15] = 4;
  descriptor.owning_local_transaction_id = 1;
  descriptor.statement_snapshot_uuid[15] = 5;
  descriptor.catalog_epoch_uuid[15] = 6;
  descriptor.catalog_generation = 1;
  descriptor.security_context_uuid[15] = 7;
  descriptor.security_epoch = 1;
  descriptor.policy_snapshot_uuid[15] = 8;
  descriptor.policy_generation = 1;
  descriptor.resource_grant_uuid[15] = 9;
  descriptor.resource_generation = 1;
  descriptor.owner_principal_uuid[15] = 10;
  descriptor.binding_uuid[15] = 11;
  descriptor.recovery_uuid[15] = 12;
  descriptor.binding_generation = 1;
  descriptor.recovery_generation = 1;
  descriptor.normalized_path_sha256[31] = 1;
  descriptor.syntax_demand_sha256[31] = 2;
  descriptor.authorization_evidence_sha256[31] = 3;
  descriptor.availability = 1;
  auto operation = sblr::MakeSblrEnvelope(
      std::string(kCreateSchemaOperation), std::string(kCreateSchemaOpcode),
      "ddl-catalog-gap-create-schema");
  operation.result_shape = "ddl_result";
  operation.diagnostic_shape = "diagnostic_vector";
  sblr::SblrOperand operand;
  operand.ordinal = 1;
  operand.type = "create_schema_descriptor";
  operand.name = "schema";
  operand.value_kind = sblr::SblrValueKind::create_schema_descriptor;
  operand.value_body =
      sblr::EncodeSblrDdlCreateSchemaDescriptorV1(descriptor, true);
  Require(!operand.value_body.empty(),
          "CREATE SCHEMA canonical descriptor construction failed");
  operation.operands.push_back(std::move(operand));
  const auto admitted = server::AdmitServerSblrEnvelope(
      scratchbird::test::sbsql::BuildCanonicalSblrAdmissionRequest(
          std::move(operation)));
  Require(admitted.admitted,
          "server admission rejected exact CREATE SCHEMA carrier");
  Require(admitted.requires_public_abi_dispatch,
          "CREATE SCHEMA did not require public ABI dispatch");
  Require(admitted.operation_id == kCreateSchemaOperation,
          "CREATE SCHEMA admission operation id drifted");

  // These symbolic rows have no Core numeric opcode, operand layout, result
  // layout, or executor-evidence profile.  They may remain parser/catalog
  // classifications, but must never be promoted into executable SBOPs.
  for (const std::string_view symbolic_operation : {
           "ddl.alter_object", "ddl.drop_object", "ddl.constraint.drop"}) {
    const auto* entry = sblr::LookupSblrOperation(symbolic_operation);
    Require(entry == nullptr || entry->code == 0,
            "unallocated symbolic DDL route became executable");
  }

  const auto* drop_database =
      sblr::LookupSblrOperation("lifecycle.drop_database");
  Require(drop_database != nullptr &&
              drop_database->opcode == "SBLR_LIFECYCLE_DROP_DATABASE" &&
              drop_database->code == 5142 &&
              drop_database->operand_contract ==
                  "lifecycle_drop_database_descriptor" &&
              drop_database->result_contract == "lifecycle_drop_result" &&
              drop_database->executor_evidence_required &&
              !drop_database->executor_evidence_accepted,
          "DROP DATABASE fail-closed registry tuple drifted");
}

void RequireCreateAndAlterSchema(api::EngineRequestContext context) {
  api::EngineCreateSchemaRequest create;
  create.context = context;
  create.target_object.uuid = scratchbird::tests::FixtureUuidLiteral("019f0700-0000-7000-8000-000000000101");
  create.target_object.object_kind = "schema";
  create.localized_names.push_back(
      Name("users.public.ddl_gap_union", "ddl_gap_union"));
  create.option_envelopes = {
      "schema_union_member:" + MetadataUuidBytes(kUnionMemberUuid),
      "schema_union_member:" + MetadataUuidBytes(kUnionMember2Uuid),
      "schema_union_policy:ordered_overlay",
      "schema_union_root:true",
      "lifecycle_transition:create_schema_descriptor",
      "mga_root_mutation_registry:local_node_catalog",
      "implementation_flavour:engine_internal_api",
      "filespace_diagnostic:default_filespace_ready",
      "catalog_ddl_mutation_audit:" + MetadataUuidBytes(kAuditUuid)};
  const auto created = api::EngineCreateSchema(create);
  for (const auto& diagnostic : created.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.detail << '\n';
  }
  Require(created.ok, "EngineCreateSchema rejected schema metadata options");
  Require(HasEvidence(created, "schema_union_member.0", kUnionMemberUuid) &&
              HasEvidence(created, "schema_union_member.1", kUnionMember2Uuid),
          "create schema missing ordered binary schema-union member evidence");
  Require(HasEvidence(created, "schema_union_policy", "ordered_overlay"),
          "create schema missing schema-union policy evidence");
  Require(HasEvidence(created, "catalog_ddl_mutation_audit",
                      kAuditUuid),
          "create schema missing catalog DDL audit evidence");
  Require(HasEvidence(created, "ddl_mga_finality_authority",
                      "durable_transaction_inventory"),
          "create schema missing MGA finality evidence");
  Require(AnySchemaIdentity(created, "schema_union_member.0", kUnionMemberUuid),
          "create schema payload omitted schema-union member");

  api::EngineAlterObjectRequest alter;
  alter.context = context;
  alter.target_object.uuid = scratchbird::tests::FixtureUuidLiteral("019f0700-0000-7000-8000-000000000101");
  alter.target_object.object_kind = "schema";
  alter.option_envelopes = {"schema_union_policy:replace_on_match",
                            "implementation_flavour:engine_internal_api_alter",
                            "catalog_ddl_mutation_audit:" + MetadataUuidBytes(kAuditUuid)};
  const auto altered = api::EngineAlterObject(alter);
  for (const auto& diagnostic : altered.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.detail << '\n';
  }
  Require(altered.ok, "EngineAlterObject rejected schema metadata options");
  Require(HasEvidence(altered, "schema_union_policy", "replace_on_match"),
          "alter schema missing schema-union policy evidence");
  Require(HasEvidence(altered, "ddl_catalog_mutation_audit"),
          "alter schema missing DDL mutation audit publication");
}

api::EngineCatalogCreateObjectRequest CatalogCreateRequest(
    const api::EngineRequestContext& context,
    api::EngineUuid identity,
    std::string kind,
    std::string path,
    std::string name,
    api::EngineUuid schema_uuid = {}) {
  api::EngineCatalogCreateObjectRequest request;
  request.context = context;
  request.target_object.uuid = identity;
  request.target_object.object_kind = std::move(kind);
  request.target_schema.uuid = std::move(schema_uuid);
  request.localized_names.push_back(Name(std::move(path), std::move(name)));
  request.option_envelopes.push_back(
      "payload:implementation_flavour=engine_internal_api");
  request.option_envelopes.push_back(
      "payload:catalog_ddl_mutation_audit=SBSQL-MISS-GATE-007");
  return request;
}

void RequireCatalogLifecycleExpansion(api::EngineRequestContext context) {
  const auto schema = api::EngineCatalogCreateObject(
      CatalogCreateRequest(context,
                           kLifecycleSchemaUuid,
                           "schema",
                           "users.public.ddl_catalog_lifecycle",
                           "ddl_catalog_lifecycle"));
  for (const auto& diagnostic : schema.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.detail << '\n';
  }
  Require(schema.ok, "catalog lifecycle schema create failed");
  Require(HasEvidence(schema, "ddl_catalog_mutation_audit"),
          "catalog lifecycle create missing DDL mutation audit");

  const auto udr = api::EngineCatalogCreateObject(
      CatalogCreateRequest(context,
                           kUdrUuid,
                           "udr",
                           "users.public.ddl_catalog_lifecycle.udr_gap",
                           "udr_gap",
                           kLifecycleSchemaUuid));
  Require(udr.ok, "generic catalog CREATE UDR failed");
  Require(HasEvidence(udr, "ddl_lifecycle_transition_registry"),
          "generic CREATE UDR missing lifecycle registry evidence");

  api::EngineCatalogAlterObjectRequest alter;
  alter.context = context;
  alter.target_object.uuid = scratchbird::tests::FixtureUuidLiteral("019f0700-0000-7000-8000-000000000201");
  alter.target_object.object_kind = "udr";
  alter.option_envelopes.push_back(
      "payload:implementation_flavour=engine_internal_api_alter");
  const auto altered = api::EngineCatalogAlterObject(alter);
  Require(altered.ok, "generic catalog ALTER UDR failed");
  Require(HasEvidence(altered, "ddl_implementation_flavour_registry"),
          "generic ALTER missing implementation-flavour registry evidence");

  const auto constraint = api::EngineCatalogCreateObject(
      CatalogCreateRequest(context,
                           kConstraintUuid,
                           "constraint",
                           "users.public.ddl_catalog_lifecycle.ck_gap",
                           "ck_gap",
                           kLifecycleSchemaUuid));
  Require(constraint.ok, "catalog constraint create failed");

  api::EngineDropConstraintRequest drop_constraint;
  drop_constraint.context = context;
  drop_constraint.target_object.uuid = scratchbird::tests::FixtureUuidLiteral("019f0700-0000-7000-8000-000000000202");
  drop_constraint.target_object.object_kind = "constraint";
  const auto constraint_dropped = api::EngineDropConstraint(drop_constraint);
  for (const auto& diagnostic : constraint_dropped.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.detail << '\n';
  }
  Require(constraint_dropped.ok, "DROP CONSTRAINT runtime failed");
  Require(HasEvidence(constraint_dropped, "ddl_catalog_mutation_audit"),
          "DROP CONSTRAINT missing catalog mutation audit evidence");

  api::EngineCatalogDropObjectRequest drop_udr;
  drop_udr.context = context;
  drop_udr.target_object.uuid = scratchbird::tests::FixtureUuidLiteral("019f0700-0000-7000-8000-000000000201");
  drop_udr.target_object.object_kind = "udr";
  const auto udr_dropped = api::EngineCatalogDropObject(drop_udr);
  Require(udr_dropped.ok, "generic catalog DROP UDR failed");
  Require(HasEvidence(udr_dropped, "ddl_mga_root_mutation_registry"),
          "generic DROP UDR missing MGA-root mutation evidence");
}

void RequireTopLevelDropForms(api::EngineRequestContext context) {
  struct DropCase {
    std::string_view kind;
    api::EngineUuid identity;
    bool expect_ok;
  };
  const DropCase cases[] = {
      {"udr", kUdrUuid, true},
      {"group", kGroupUuid, true},
      {"column", kColumnUuid, true},
      {"filespace", kFilespaceUuid, true},
      {"cluster", kClusterUuid, false},
      {"node", kNodeUuid, false},
  };
  for (const auto& drop_case : cases) {
    api::EngineDropObjectRequest request;
    request.context = context;
    request.target_object.uuid = drop_case.identity;
    request.target_object.object_kind = std::string(drop_case.kind);
    const auto dropped = api::EngineDropObject(request);
    if (drop_case.expect_ok) {
      for (const auto& diagnostic : dropped.diagnostics) {
        std::cerr << diagnostic.code << ':' << diagnostic.detail << '\n';
      }
      Require(dropped.ok, "top-level DROP form failed");
      Require(HasEvidence(dropped, "ddl_catalog_mutation_audit"),
              "top-level DROP form missing DDL mutation audit");
      if (drop_case.kind == std::string_view("filespace")) {
        Require(HasEvidence(dropped,
                            "ddl_filespace_diagnostic",
                            kFilespaceUuid),
                "DROP FILESPACE missing filespace diagnostic evidence");
      }
    } else {
      Require(!dropped.ok, "cluster/node DROP must fail closed without cluster authority");
      Require(dropped.cluster_authority_required,
              "cluster/node DROP did not report cluster authority requirement");
      Require(HasDiagnostic(dropped, "SB_ENGINE_API_CLUSTER_AUTHORITY_UNAVAILABLE"),
              "cluster/node DROP missing cluster authority diagnostic");
      Require(HasEvidence(dropped, "cluster_provider_dispatch", "false"),
              "cluster/node DROP missing provider-dispatch=false evidence");
    }
  }
}

void RequireRightsMaterialization(const api::EngineRequestContext& authorized,
                                  const api::EngineRequestContext& unauthorized) {
  Require(api::SecurityContextHasRight(authorized, "CATALOG_MUTATE"),
          "authorized context did not materialize CATALOG_MUTATE");
  Require(!api::SecurityContextHasRight(unauthorized, "CATALOG_MUTATE"),
          "unauthorized context unexpectedly materialized CATALOG_MUTATE");
}

}  // namespace

int main() {
  RequireParserUnionGrammar();
  RequireAdmissionAndOpcodeRegistry();

  auto fixture = CreateFixture();
  auto authorized = Context(fixture, true);
  auto unauthorized = Context(fixture, false);
  RequireRightsMaterialization(authorized, unauthorized);
  RequireCreateAndAlterSchema(authorized);
  RequireCatalogLifecycleExpansion(authorized);
  RequireTopLevelDropForms(authorized);

  CommitFixture(&fixture);
  authorized = Context(fixture, true);
  const auto schema = api::CheckedFindSchemaTreeRecord(
      authorized,
      kSchemaUuid,
      fixture.local_transaction_id);
  Require(schema.has_value(), "committed schema metadata was not reopen-visible");
  Require(SchemaHasIdentity(schema->payload, "catalog_ddl_mutation_audit", kAuditUuid),
          "committed schema payload omitted catalog DDL audit marker");
  Cleanup(fixture.path);
  return EXIT_SUCCESS;
}

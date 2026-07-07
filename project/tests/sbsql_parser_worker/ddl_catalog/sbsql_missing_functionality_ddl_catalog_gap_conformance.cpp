// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "catalog/catalog_object_lifecycle.hpp"
#include "catalog/schema_tree_api.hpp"
#include "cst/cst.hpp"
#include "database_lifecycle.hpp"
#include "ddl/alter_api.hpp"
#include "ddl/create_api.hpp"
#include "ddl/drop_api.hpp"
#include "local_transaction_store.hpp"
#include "sblr_admission.hpp"
#include "sblr_opcode_registry.hpp"
#include "security/security_model.hpp"
#include "transaction_inventory.hpp"
#include "uuid.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace api = scratchbird::engine::internal_api;
namespace db = scratchbird::storage::database;
namespace parser = scratchbird::parser::sbsql;
namespace server = scratchbird::server;
namespace sblr = scratchbird::engine::sblr;
namespace txn = scratchbird::transaction::mga;
namespace uuid = scratchbird::core::uuid;
using scratchbird::core::platform::UuidKind;

constexpr std::string_view kPrincipalUuid =
    "019f0700-0000-7000-8000-000000000001";
constexpr std::string_view kSchemaUuid =
    "019f0700-0000-7000-8000-000000000101";
constexpr std::string_view kLifecycleSchemaUuid =
    "019f0700-0000-7000-8000-000000000102";
constexpr std::string_view kUnionMemberUuid =
    "019f0700-0000-7000-8000-000000000103";
constexpr std::string_view kUnionMember2Uuid =
    "019f0700-0000-7000-8000-000000000104";
constexpr std::string_view kUdrUuid =
    "019f0700-0000-7000-8000-000000000201";
constexpr std::string_view kConstraintUuid =
    "019f0700-0000-7000-8000-000000000202";
constexpr std::string_view kColumnUuid =
    "019f0700-0000-7000-8000-000000000203";
constexpr std::string_view kGroupUuid =
    "019f0700-0000-7000-8000-000000000204";
constexpr std::string_view kFilespaceUuid =
    "019f0700-0000-7000-8000-000000000205";
constexpr std::string_view kClusterUuid =
    "019f0700-0000-7000-8000-000000000206";
constexpr std::string_view kNodeUuid =
    "019f0700-0000-7000-8000-000000000207";

struct Fixture {
  std::filesystem::path path;
  std::string database_uuid;
  std::string transaction_uuid;
  std::uint64_t local_transaction_id = 0;
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
  std::error_code ignored;
  std::filesystem::remove(path, ignored);
  for (const char* suffix : {".dirty.manifest",
                             ".recovery.evidence",
                             ".sb.api_events",
                             ".sb.catalog_object_events",
                             ".sb.name_events",
                             ".sb.mga_event_sequence_allocator",
                             ".sb.mga_index_entries",
                             ".sb.mga_large_values",
                             ".sb.mga_relation_descriptors",
                             ".sb.mga_relation_metadata",
                             ".sb.mga_row_versions",
                             ".sb.mga_savepoints",
                             ".sb.mga_secondary_index_delta_ledger"}) {
    std::filesystem::remove(path.string() + suffix, ignored);
  }
}

bool HasEvidence(const api::EngineApiResult& result,
                 std::string_view kind,
                 std::string_view value = {}) {
  for (const auto& evidence : result.evidence) {
    if (evidence.evidence_kind == kind &&
        (value.empty() || evidence.evidence_id == value)) {
      return true;
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

void Grant(api::EngineRequestContext* context,
           std::string right,
           std::string target_uuid = {}) {
  api::EngineAuthorizationSubject subject;
  subject.subject_uuid.canonical = std::string(kPrincipalUuid);
  subject.subject_kind = "user";
  context->authorization_context.effective_subjects.push_back(subject);

  api::EngineMaterializedAuthorizationGrant grant;
  grant.subject_uuid.canonical = std::string(kPrincipalUuid);
  grant.subject_kind = "user";
  grant.target_uuid.canonical = std::move(target_uuid);
  grant.right = std::move(right);
  context->authorization_context.grants.push_back(std::move(grant));
}

api::EngineRequestContext Context(const Fixture& fixture, bool grant_catalog_mutate) {
  api::EngineRequestContext context;
  context.database_path = fixture.path.string();
  context.database_uuid.canonical = fixture.database_uuid;
  context.principal_uuid.canonical = std::string(kPrincipalUuid);
  context.session_uuid.canonical = "019f0700-0000-7000-8000-000000000010";
  context.transaction_uuid.canonical = fixture.transaction_uuid;
  context.local_transaction_id = fixture.local_transaction_id;
  context.snapshot_visible_through_local_transaction_id =
      fixture.local_transaction_id;
  context.catalog_generation_id = 77;
  context.security_epoch = 78;
  context.resource_epoch = 79;
  context.security_context_present = true;
  context.authorization_context.present = true;
  context.authorization_context.principal_uuid.canonical =
      std::string(kPrincipalUuid);
  context.authorization_context.authority_uuid.canonical =
      "019f0700-0000-7000-8000-000000000020";
  context.authorization_context.security_epoch = context.security_epoch;
  context.authorization_context.policy_epoch = 80;
  context.authorization_context.catalog_generation_id =
      context.catalog_generation_id;
  context.authorization_context.evidence_tags.push_back("sbsql_miss_007");
  if (grant_catalog_mutate) Grant(&context, "CATALOG_MUTATE");
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
  Require(db::CreateDatabaseFile(create).ok(), "database creation failed");

  fixture.inventory = txn::MakeEmptyLocalTransactionInventory();
  const auto transaction_uuid =
      uuid::GenerateEngineIdentityV7(UuidKind::transaction, 1790700000003);
  Require(transaction_uuid.ok(), "transaction UUID generation failed");
  auto begun = txn::BeginLocalTransaction(std::move(fixture.inventory),
                                          transaction_uuid.value,
                                          1790700000004);
  Require(begun.ok(), "local transaction begin failed");
  fixture.inventory = std::move(begun.inventory);
  fixture.database_uuid = uuid::UuidToString(database_uuid.value.value);
  fixture.transaction_uuid = uuid::UuidToString(transaction_uuid.value.value);
  fixture.local_transaction_id = begun.entry.identity.local_id.value;
  fixture.typed_local_transaction_id = begun.entry.identity.local_id;
  Require(db::PersistLocalTransactionInventoryToDatabase(fixture.path.string(),
                                                         fixture.inventory)
              .ok(),
          "active transaction inventory persist failed");
  return fixture;
}

void CommitFixture(Fixture* fixture) {
  auto committed = txn::CommitLocalTransaction(std::move(fixture->inventory),
                                               fixture->typed_local_transaction_id,
                                               1790700000100);
  Require(committed.ok(), "local transaction commit failed");
  fixture->inventory = std::move(committed.inventory);
  Require(db::PersistLocalTransactionInventoryToDatabase(fixture->path.string(),
                                                         fixture->inventory)
              .ok(),
          "committed transaction inventory persist failed");
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
  struct Route {
    std::string operation_id;
    std::string family;
    std::string opcode;
  };
  const Route routes[] = {
      {"ddl.create_schema", "sblr.catalog.mutation.v3",
       "SBLR_DDL_CREATE_SCHEMA"},
      {"ddl.alter_object", "sblr.catalog.mutation.v3",
       "SBLR_DDL_ALTER_OBJECT"},
      {"ddl.drop_object", "sblr.catalog.mutation.v3",
       "SBLR_DDL_DROP_OBJECT"},
      {"ddl.constraint.drop", "sblr.catalog.mutation.v3",
       "SBLR_DDL_CONSTRAINT_DROP"},
      {"lifecycle.drop_database", "sblr.database.management.v3",
       "SBLR_LIFECYCLE_DROP_DATABASE"},
  };
  for (const auto& route : routes) {
    const auto* entry = sblr::LookupSblrOperation(route.operation_id);
    Require(entry != nullptr, "DDL/catalog opcode registry row missing");
    Require(entry->opcode == route.opcode, "DDL/catalog opcode drifted");
    Require(entry->requires_security_context,
            "DDL/catalog route must require security context");
    const auto admitted = server::AdmitServerSblrEnvelope(
        server::ServerSblrAdmissionRequest{
            AdmissionFrame(route.operation_id, route.family), false});
    Require(admitted.admitted, "server admission rejected DDL/catalog route");
    Require(admitted.requires_public_abi_dispatch,
            "DDL/catalog route did not require public ABI dispatch");
    Require(admitted.operation_id == route.operation_id,
            "server admission operation id drifted");
  }
}

void RequireCreateAndAlterSchema(api::EngineRequestContext context) {
  api::EngineCreateSchemaRequest create;
  create.context = context;
  create.target_object.uuid.canonical = std::string(kSchemaUuid);
  create.target_object.object_kind = "schema";
  create.localized_names.push_back(
      Name("users.public.ddl_gap_union", "ddl_gap_union"));
  create.option_envelopes = {
      "schema_union_member:" + std::string(kUnionMemberUuid),
      "schema_union_member:" + std::string(kUnionMember2Uuid),
      "schema_union_policy:ordered_overlay",
      "schema_union_root:true",
      "lifecycle_transition:create_schema_descriptor",
      "mga_root_mutation_registry:local_node_catalog",
      "implementation_flavour:engine_internal_api",
      "filespace_diagnostic:default_filespace_ready",
      "catalog_ddl_mutation_audit:SBSQL-MISS-GATE-007"};
  const auto created = api::EngineCreateSchema(create);
  for (const auto& diagnostic : created.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.detail << '\n';
  }
  Require(created.ok, "EngineCreateSchema rejected schema metadata options");
  Require(HasEvidence(created, "schema_union_member", kUnionMemberUuid),
          "create schema missing schema-union member evidence");
  Require(HasEvidence(created, "schema_union_policy", "ordered_overlay"),
          "create schema missing schema-union policy evidence");
  Require(HasEvidence(created, "catalog_ddl_mutation_audit",
                      "SBSQL-MISS-GATE-007"),
          "create schema missing catalog DDL audit evidence");
  Require(HasEvidence(created, "ddl_mga_finality_authority",
                      "durable_transaction_inventory"),
          "create schema missing MGA finality evidence");
  Require(AnyFieldContains(created, "payload",
                           "schema_union_member=" + std::string(kUnionMemberUuid)),
          "create schema payload omitted schema-union member");

  api::EngineAlterObjectRequest alter;
  alter.context = context;
  alter.target_object.uuid.canonical = std::string(kSchemaUuid);
  alter.target_object.object_kind = "schema";
  alter.option_envelopes = {"schema_union_policy:replace_on_match",
                            "implementation_flavour:engine_internal_api_alter",
                            "catalog_ddl_mutation_audit:SBSQL-MISS-GATE-007"};
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
    std::string_view uuid_text,
    std::string kind,
    std::string path,
    std::string name,
    std::string schema_uuid = {}) {
  api::EngineCatalogCreateObjectRequest request;
  request.context = context;
  request.target_object.uuid.canonical = std::string(uuid_text);
  request.target_object.object_kind = std::move(kind);
  request.target_schema.uuid.canonical = std::move(schema_uuid);
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
                           std::string(kLifecycleSchemaUuid)));
  Require(udr.ok, "generic catalog CREATE UDR failed");
  Require(HasEvidence(udr, "ddl_lifecycle_transition_registry"),
          "generic CREATE UDR missing lifecycle registry evidence");

  api::EngineCatalogAlterObjectRequest alter;
  alter.context = context;
  alter.target_object.uuid.canonical = std::string(kUdrUuid);
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
                           std::string(kLifecycleSchemaUuid)));
  Require(constraint.ok, "catalog constraint create failed");

  api::EngineDropConstraintRequest drop_constraint;
  drop_constraint.context = context;
  drop_constraint.target_object.uuid.canonical = std::string(kConstraintUuid);
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
  drop_udr.target_object.uuid.canonical = std::string(kUdrUuid);
  drop_udr.target_object.object_kind = "udr";
  const auto udr_dropped = api::EngineCatalogDropObject(drop_udr);
  Require(udr_dropped.ok, "generic catalog DROP UDR failed");
  Require(HasEvidence(udr_dropped, "ddl_mga_root_mutation_registry"),
          "generic DROP UDR missing MGA-root mutation evidence");
}

void RequireTopLevelDropForms(api::EngineRequestContext context) {
  struct DropCase {
    std::string_view kind;
    std::string_view uuid_text;
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
    request.target_object.uuid.canonical = std::string(drop_case.uuid_text);
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
                            "filespace_catalog_mutation_recorded:" +
                                std::string(kFilespaceUuid)),
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
  authorized.snapshot_visible_through_local_transaction_id =
      fixture.local_transaction_id;
  const auto schema = api::FindVisibleSchemaTreeRecord(
      authorized,
      std::string(kSchemaUuid),
      fixture.local_transaction_id);
  Require(schema.has_value(), "committed schema metadata was not reopen-visible");
  Require(Contains(schema->payload, "catalog_ddl_mutation_audit=SBSQL-MISS-GATE-007"),
          "committed schema payload omitted catalog DDL audit marker");
  Cleanup(fixture.path);
  return EXIT_SUCCESS;
}

// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "ast/ast.hpp"
#include "binder/binder.hpp"
#include "catalog/catalog_object_lifecycle.hpp"
#include "cst/cst.hpp"
#include "database_lifecycle.hpp"
#include "lowering/lowering.hpp"
#include "registry/generated/sbsql_generated_registry.hpp"
#include "ddl/create_api.hpp"
#include "transaction/transaction_api.hpp"
#include "uuid.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>

namespace {

using namespace scratchbird::parser::sbsql;
namespace api = scratchbird::engine::internal_api;
namespace db = scratchbird::storage::database;
namespace uuid = scratchbird::core::uuid;
using scratchbird::core::platform::UuidKind;

constexpr std::string_view kColumnConstraintSurfaceId = "SBSQL-A57CFDE0BBA9";
constexpr std::string_view kColumnConstraintName = "column_constraint";
constexpr std::string_view kColumnConstraintSql = "CREATE TABLE customer (id int NOT NULL);";
constexpr std::string_view kTableConstraintSurfaceId = "SBSQL-28F16A4C7DD0";
constexpr std::string_view kConstraintNameSurfaceId = "SBSQL-B1816929AD45";
constexpr std::string_view kConstraintBodySurfaceId = "SBSQL-5CC9FDFFE6F7";
constexpr std::string_view kTableConstraintSql =
    "CREATE TABLE customer (id int, CONSTRAINT customer_pk PRIMARY KEY (id));";
constexpr std::string_view kRefusalOperationId = "engine.op.diagnostic_refusal";
constexpr std::string_view kRefusalOpcode = "SBLR_DIAGNOSTIC_REFUSAL";
constexpr std::string_view kCreateTableOperationId = "engine.op.ddl_create_table";
constexpr std::string_view kCreateTableOpcode = "SBLR_DDL_CREATE_TABLE";
constexpr std::string_view kInternalConstraintOperationId = "ddl.constraint.create";
constexpr std::string_view kFamily = "sblr.catalog.mutation.v3";

void Require(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << message << '\n';
    std::exit(EXIT_FAILURE);
  }
}

bool Contains(std::string_view haystack, std::string_view needle) {
  return haystack.find(needle) != std::string_view::npos;
}

bool HasValue(const std::vector<std::string>& values, std::string_view expected) {
  return std::find(values.begin(), values.end(), expected) != values.end();
}

bool HasEvidence(const api::EngineApiResult& result,
                 std::string_view kind,
                 std::string_view id) {
  for (const auto& evidence : result.evidence) {
    if (evidence.evidence_kind == kind && evidence.evidence_id == id) return true;
  }
  return false;
}

void DumpDiagnostics(const MessageVectorSet& messages) {
  for (const auto& diagnostic : messages.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.message << '\n';
  }
}

SessionContext ParserSession() {
  SessionContext session;
  session.authenticated = true;
  session.session_uuid = "019f0000-0000-7000-8000-000000021101";
  session.connection_uuid = "019f0000-0000-7000-8000-000000021102";
  session.database_uuid = "019f0000-0000-7000-8000-000000021103";
  session.dialect_profile_uuid = "sbsql_v3";
  session.catalog_epoch = 21;
  session.security_policy_epoch = 22;
  session.descriptor_epoch = 23;
  return session;
}

ParserConfig ParserConfigForTest() {
  ParserConfig config;
  config.probe_mode = true;
  config.server_endpoint = "sb_server_name_resolver";
  config.parser_uuid = "019f0000-0000-7000-8000-000000021104";
  config.bundle_contract_id = "sbp_sbsql@column-constraint-route-test";
  config.build_id = "sbsql-column-constraint-route-test";
  return config;
}

struct PipelineArtifacts {
  CstDocument cst;
  AstDocument ast;
  BoundStatement bound;
  SblrEnvelope envelope;
  SblrVerifierResult verifier;
};

PipelineArtifacts RunPipeline(std::string_view sql) {
  PipelineArtifacts artifacts;
  const auto session = ParserSession();
  artifacts.cst = BuildCst(sql);
  artifacts.ast = BuildAst(artifacts.cst);
  artifacts.bound = BindAst(artifacts.ast, artifacts.cst, ParserConfigForTest(), session);
  artifacts.envelope = LowerToSblr(artifacts.bound, artifacts.cst, session);
  artifacts.verifier = VerifySblrEnvelope(artifacts.envelope);
  return artifacts;
}

void RequireGeneratedRegistryRow(std::string_view surface_id,
                                 std::string_view canonical_name,
                                 std::string_view family,
                                 std::string_view sblr_family,
                                 std::string_view parser_handler,
                                 std::string_view lowering_handler) {
  const auto* registry_row = FindGeneratedSurfaceRegistryRowById(surface_id);
  Require(registry_row != nullptr, "constraint generated registry row missing");
  Require(registry_row->canonical_name == canonical_name,
          "constraint generated registry canonical name drifted");
  Require(registry_row->surface_kind == "grammar_production",
          "constraint generated registry kind drifted");
  Require(registry_row->family == family,
          "constraint generated registry family drifted");
  Require(registry_row->source_status == "native_now",
          "constraint generated registry status drifted");
  Require(registry_row->cluster_scope == "noncluster_or_profile_scoped",
          "constraint generated registry cluster scope drifted");
  Require(registry_row->sblr_operation_family == sblr_family,
          "constraint generated registry canonical SBLR family drifted");
  Require(registry_row->parser_handler_key == parser_handler,
          "constraint generated registry parser handler drifted");
  Require(registry_row->lowering_handler_key == lowering_handler,
          "constraint generated registry lowering handler drifted");
}

void RequireRegistryEvidence() {
  RequireGeneratedRegistryRow(kColumnConstraintSurfaceId,
                              kColumnConstraintName,
                              "general",
                              "sblr.general.operation.v3",
                              "parser.grammar_ast",
                              "lowering.sblr_family.sblr_general_operation_v3");
  RequireGeneratedRegistryRow(kTableConstraintSurfaceId,
                              "table_constraint",
                              "ddl_catalog",
                              "sblr.catalog.mutation.v3",
                              "parser.statement_family.ddl_catalog",
                              "lowering.sblr_family.sblr_catalog_mutation_v3");
  RequireGeneratedRegistryRow(kConstraintNameSurfaceId,
                              "constraint_name",
                              "general",
                              "sblr.general.operation.v3",
                              "parser.grammar_ast",
                              "lowering.sblr_family.sblr_general_operation_v3");
  RequireGeneratedRegistryRow(kConstraintBodySurfaceId,
                              "constraint_body",
                              "general",
                              "sblr.general.operation.v3",
                              "parser.grammar_ast",
                              "lowering.sblr_family.sblr_general_operation_v3");
}

void RequireParserLowering(const PipelineArtifacts& artifacts,
                           std::string_view sql,
                           std::string_view label,
                           std::initializer_list<std::string_view> expected_surface_ids) {
  if (artifacts.cst.messages.has_errors()) DumpDiagnostics(artifacts.cst.messages);
  if (artifacts.ast.messages.has_errors()) DumpDiagnostics(artifacts.ast.messages);
  if (artifacts.bound.messages.has_errors()) DumpDiagnostics(artifacts.bound.messages);
  Require(!artifacts.cst.messages.has_errors(), "constraint CST failed");
  Require(!artifacts.ast.messages.has_errors(), "constraint AST failed");
  Require(artifacts.bound.bound, "constraint bind failed");
  Require(!artifacts.verifier.admitted,
          "constraint child was admitted without an engine-bound CREATE TABLE descriptor");
  Require(artifacts.envelope.operation_id == kRefusalOperationId &&
              artifacts.envelope.sblr_opcode == kRefusalOpcode &&
              artifacts.envelope.engine_api_operation_id == "not_admitted",
          "constraint child did not use the exact non-executable refusal tuple");
  Require(artifacts.envelope.operation_family == kFamily,
          "constraint route family mismatch");
  Require(artifacts.envelope.sblr_operation_key == kFamily,
          "constraint operation key mismatch");
  Require(HasValue(artifacts.envelope.required_authority_steps,
                   "authority.parser.no_executable_sblr") &&
              HasValue(artifacts.envelope.required_authority_steps,
                   "authority.parser.no_sql_text_execution"),
          "constraint parser non-authority evidence missing");
  Require(!artifacts.envelope.parser_executes_sql,
          "constraint lowering allowed parser SQL execution");
  Require(!artifacts.envelope.real_file_effects,
          "constraint lowering allowed file effects");
  Require(artifacts.envelope.payload.empty() &&
              artifacts.envelope.operands.empty() &&
              artifacts.envelope.resolved_object_uuids.empty() &&
              artifacts.envelope.required_rights.empty(),
          "constraint refusal retained executable or resolved authority");
  Require(artifacts.envelope.messages.diagnostics.size() == 1,
          "constraint refusal did not emit one exact diagnostic");
  const auto& diagnostic = artifacts.envelope.messages.diagnostics.front();
  Require(diagnostic.code == "SBSQL.IMPL.NOT_AVAILABLE",
          "constraint refusal diagnostic drifted");
  auto diagnostic_field = [&](std::string_view field_name) {
    for (const auto& field : diagnostic.fields) {
      if (field.name == field_name) return field.value;
    }
    return std::string{};
  };
  Require(diagnostic_field("canonical_parent_operation_id") ==
              kCreateTableOperationId &&
              diagnostic_field("canonical_parent_sblr_opcode") ==
                  kCreateTableOpcode &&
              diagnostic_field("executable_sblr_emitted") == "false",
          "constraint refusal did not retain the canonical CREATE TABLE parent");
  std::string expected_surface_csv;
  for (const auto surface_id : expected_surface_ids) {
    if (!expected_surface_csv.empty()) expected_surface_csv.push_back(',');
    expected_surface_csv += surface_id;
  }
  Require(diagnostic_field("recognized_surface_ids") == expected_surface_csv,
          "constraint refusal surface identity set drifted");
  Require(!Contains(artifacts.envelope.payload, sql),
          "constraint refusal embedded source SQL text");
  Require(!std::string(label).empty(), "constraint route label missing");
}

std::uint64_t CurrentUnixMillis() {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());
}

std::filesystem::path TestDatabasePath() {
  return std::filesystem::temp_directory_path() /
         ("sbsql_column_constraint_exact_route_" + std::to_string(CurrentUnixMillis()) + ".sbdb");
}

void RemoveDatabaseArtifacts(const std::filesystem::path& path) {
  std::error_code ignored;
  std::filesystem::remove(path, ignored);
  for (const auto suffix : {".sb.catalog_object_events",
                            ".sb.name_events",
                            ".sb.crud_events",
                            ".sb.transaction_inventory",
                            ".dirty.manifest",
                            ".recovery.evidence",
                            ".sb.owner.lock"}) {
    std::filesystem::remove(path.string() + suffix, ignored);
  }
}

std::string CreateMinimalDatabase(const std::filesystem::path& path) {
  db::DatabaseCreateConfig create;
  create.path = path.string();
  create.database_uuid =
      uuid::GenerateEngineIdentityV7(UuidKind::database, 1779810211000).value;
  create.filespace_uuid =
      uuid::GenerateEngineIdentityV7(UuidKind::filespace, 1779810211001).value;
  create.page_size = 16384;
  create.creation_unix_epoch_millis = 1779810211002;
  create.allow_minimal_resource_bootstrap = true;
  create.require_resource_seed_pack = false;
  create.allow_overwrite = true;
  const auto created = db::CreateDatabaseFile(create);
  if (!created.ok()) {
    std::cerr << created.diagnostic.diagnostic_code << ':' << created.diagnostic.message_key
              << '\n';
  }
  Require(created.ok(), "column_constraint test database create failed");
  return uuid::UuidToString(create.database_uuid.value);
}

api::EngineRequestContext BaseContext(const std::filesystem::path& path,
                                      const std::string& database_uuid) {
  api::EngineRequestContext context;
  context.request_id = "sbsql-column-constraint-exact-route";
  context.database_path = path.string();
  context.database_uuid.canonical = database_uuid;
  context.session_uuid.canonical = "019f0000-0000-7000-8000-000000021202";
  context.principal_uuid.canonical = "019f0000-0000-7000-8000-000000021203";
  context.current_schema_uuid.canonical = "019f0000-0000-7000-8000-000000021205";
  context.security_context_present = true;
  context.catalog_generation_id = 1;
  context.security_epoch = 1;
  context.resource_epoch = 1;
  context.name_resolution_epoch = 1;
  context.trace_tags.push_back("right:CATALOG_MUTATE");
  context.trace_tags.push_back("sbsql_surface_id:SBSQL-A57CFDE0BBA9");
  context.trace_tags.push_back("sbsql_surface_id:SBSQL-28F16A4C7DD0");
  return context;
}

api::EngineRequestContext BeginTransaction(const std::filesystem::path& path,
                                           const std::string& database_uuid) {
  api::EngineBeginTransactionRequest begin;
  begin.context = BaseContext(path, database_uuid);
  begin.isolation_level = "read_committed";
  const auto begun = api::EngineBeginTransaction(begin);
  if (!begun.ok) {
    for (const auto& diagnostic : begun.diagnostics) {
      std::cerr << diagnostic.code << ':' << diagnostic.detail << '\n';
    }
  }
  Require(begun.ok, "column_constraint begin transaction failed");
  auto context = begin.context;
  context.local_transaction_id = begun.local_transaction_id;
  context.transaction_uuid = begun.transaction_uuid;
  context.snapshot_visible_through_local_transaction_id =
      begun.snapshot_visible_through_local_transaction_id;
  context.transaction_isolation_level = begun.isolation_level;
  return context;
}

api::EngineLocalizedName Name(std::string value) {
  api::EngineLocalizedName name;
  name.language_tag = "en";
  name.name_class = "primary";
  name.name = value;
  name.raw_name_text = value;
  name.display_name = value;
  name.default_name = true;
  return name;
}

api::EngineCatalogCreateObjectRequest CreateObjectRequest(
    const api::EngineRequestContext& context,
    std::string object_uuid,
    std::string object_kind,
    std::string schema_uuid,
    std::string name) {
  api::EngineCatalogCreateObjectRequest request;
  request.context = context;
  request.target_object.uuid.canonical = std::move(object_uuid);
  request.target_object.object_kind = std::move(object_kind);
  request.target_schema.uuid.canonical = std::move(schema_uuid);
  request.localized_names.push_back(Name(std::move(name)));
  return request;
}

void SeedCatalogTarget(const api::EngineRequestContext& context) {
  auto schema = CreateObjectRequest(context, "schema-app", "schema", "", "app");
  const auto created_schema = api::EngineCatalogCreateObject(schema);
  for (const auto& diagnostic : created_schema.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.detail << '\n';
  }
  Require(created_schema.ok, "column_constraint schema seed failed");

  auto table = CreateObjectRequest(context, "table-customer", "table", "schema-app", "customer");
  api::EngineColumnDefinition id_column;
  id_column.requested_column_uuid.canonical = "column-customer-id";
  id_column.names.push_back(Name("id"));
  id_column.descriptor.descriptor_kind = "scalar";
  id_column.descriptor.canonical_type_name = "int";
  id_column.descriptor.encoded_descriptor = "type=int";
  id_column.ordinal = 0;
  id_column.nullable = true;
  table.columns.push_back(std::move(id_column));
  const auto created_table = api::EngineCatalogCreateObject(table);
  for (const auto& diagnostic : created_table.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.detail << '\n';
  }
  Require(created_table.ok, "column_constraint table seed failed");
}

api::EngineApiRequest EngineConstraintRequest() {
  api::EngineApiRequest request;
  request.target_object.uuid.canonical = "table-customer";
  request.target_object.object_kind = "table";
  api::EngineConstraintDefinition constraint;
  constraint.requested_constraint_uuid.canonical = "constraint-customer-id-not-null";
  constraint.names.push_back(Name("customer_id_not_null"));
  constraint.constraint_kind = "not_null_constraint";
  constraint.canonical_constraint_envelope =
      "constraint_hash=customer_id_not_null_hash;"
      "constraint_policy_version_uuid=policy-local-column-constraint;"
      "enforcement_timing=immediate;validation_state=validated;trust_state=trusted;"
      "support_requirement=optional;subject_uuid=subject-customer-id-not-null;"
      "subject_kind=column;subject_object_uuid=column-customer-id;subject_descriptor=id;"
      "diagnostic_profile_uuid=diag-column-constraint;"
      "metrics_profile_uuid=metrics-column-constraint;"
      "conformance_profile_uuid=conformance-column-constraint";
  request.constraints.push_back(std::move(constraint));
  return request;
}

api::EngineApiRequest EngineTableConstraintRequest() {
  api::EngineApiRequest request;
  request.target_object.uuid.canonical = "table-customer";
  request.target_object.object_kind = "table";
  api::EngineConstraintDefinition constraint;
  constraint.requested_constraint_uuid.canonical = "constraint-customer-pk";
  constraint.names.push_back(Name("customer_pk"));
  constraint.constraint_kind = "primary_key";
  constraint.canonical_constraint_envelope =
      "constraint_hash=customer_pk_hash;"
      "constraint_policy_version_uuid=policy-local-table-constraint;"
      "enforcement_timing=immediate;validation_state=validated;trust_state=trusted;"
      "support_requirement=required;support_uuid=index-customer-pk;"
      "support_family=rowstore_scalar_btree_v1;"
      "support_binding_uuid=support-customer-pk;"
      "key_descriptor_uuid=key-customer-pk;key_class=primary_key;"
      "component_order_hash=id;comparison_profile_hash=int64;"
      "null_policy=not_null;canonical_encoding_uuid=encoding-int64;"
      "subject_uuid=subject-customer-pk;subject_kind=owner_object;"
      "subject_object_uuid=table-customer;subject_descriptor=primary_key(id);"
      "diagnostic_profile_uuid=diag-table-constraint;"
      "metrics_profile_uuid=metrics-table-constraint;"
      "conformance_profile_uuid=conformance-table-constraint";
  request.constraints.push_back(std::move(constraint));
  return request;
}

void RequireEngineDispatch() {
  const auto path = TestDatabasePath();
  RemoveDatabaseArtifacts(path);
  const auto database_uuid = CreateMinimalDatabase(path);
  auto context = BeginTransaction(path, database_uuid);
  SeedCatalogTarget(context);

  api::EngineCreateConstraintRequest request;
  static_cast<api::EngineApiRequest&>(request) = EngineConstraintRequest();
  request.context = context;
  const auto result = api::EngineCreateConstraint(request);
  Require(result.ok, "EngineCreateConstraint component did not return success");
  Require(result.operation_id == kInternalConstraintOperationId,
          "EngineCreateConstraint operation id mismatch");
  Require(result.primary_object.uuid.canonical == "table-customer",
          "EngineCreateConstraint target object UUID mismatch");
  Require(HasEvidence(result, "constraint_catalog_route", "sys.constraint_descriptor"),
          "EngineCreateConstraint missing constraint catalog evidence");
  Require(HasEvidence(result, "ddl_catalog_route", "sys.constraint_descriptor"),
          "EngineCreateConstraint missing DDL catalog route evidence");

  const auto loaded = api::LoadCatalogObjectLifecycleState(context);
  Require(loaded.ok, "column_constraint catalog state load failed");
  bool saw_constraint = false;
  bool saw_subject = false;
  for (const auto& constraint : loaded.state.constraints) {
    if (constraint.constraint_uuid == "constraint-customer-id-not-null" &&
        constraint.constraint_class == "not_null_constraint" &&
        constraint.owner_object_uuid == "table-customer" &&
        constraint.constraint_hash == "customer_id_not_null_hash" &&
        constraint.enforcement_timing == "immediate") {
      saw_constraint = true;
    }
  }
  for (const auto& subject : loaded.state.constraint_subjects) {
    if (subject.subject_uuid == "subject-customer-id-not-null" &&
        subject.constraint_uuid == "constraint-customer-id-not-null" &&
        subject.subject_kind == "column" &&
        subject.subject_object_uuid == "column-customer-id") {
      saw_subject = true;
    }
  }
  Require(saw_constraint, "column_constraint descriptor was not persisted");
  Require(saw_subject, "column_constraint subject was not persisted");
  RemoveDatabaseArtifacts(path);
}

void RequireTableConstraintEngineDispatch() {
  const auto path = TestDatabasePath();
  RemoveDatabaseArtifacts(path);
  const auto database_uuid = CreateMinimalDatabase(path);
  auto context = BeginTransaction(path, database_uuid);
  SeedCatalogTarget(context);

  api::EngineCreateConstraintRequest request;
  static_cast<api::EngineApiRequest&>(request) = EngineTableConstraintRequest();
  request.context = context;
  const auto result = api::EngineCreateConstraint(request);
  Require(result.ok,
          "EngineCreateConstraint table component did not return success");
  Require(result.operation_id == kInternalConstraintOperationId,
          "EngineCreateConstraint table operation id mismatch");
  Require(HasEvidence(result, "constraint_catalog_route", "sys.constraint_descriptor"),
          "EngineCreateConstraint table missing constraint catalog evidence");

  const auto loaded = api::LoadCatalogObjectLifecycleState(context);
  Require(loaded.ok, "table_constraint catalog state load failed");
  bool saw_constraint = false;
  bool saw_subject = false;
  bool saw_key = false;
  bool saw_support = false;
  bool saw_name = false;
  for (const auto& constraint : loaded.state.constraints) {
    if (constraint.constraint_uuid == "constraint-customer-pk" &&
        constraint.constraint_class == "primary_key" &&
        constraint.owner_object_uuid == "table-customer" &&
        constraint.constraint_hash == "customer_pk_hash" &&
        constraint.support_requirement == "required") {
      saw_constraint = true;
    }
  }
  for (const auto& subject : loaded.state.constraint_subjects) {
    if (subject.subject_uuid == "subject-customer-pk" &&
        subject.constraint_uuid == "constraint-customer-pk" &&
        subject.subject_kind == "owner_object" &&
        subject.subject_object_uuid == "table-customer" &&
        subject.subject_descriptor == "primary_key(id)") {
      saw_subject = true;
    }
  }
  for (const auto& key : loaded.state.key_descriptors) {
    if (key.key_descriptor_uuid == "key-customer-pk" &&
        key.constraint_uuid == "constraint-customer-pk" &&
        key.key_class == "primary_key" &&
        key.owner_object_uuid == "table-customer") {
      saw_key = true;
    }
  }
  for (const auto& support : loaded.state.constraint_support_structures) {
    if (support.support_binding_uuid == "support-customer-pk" &&
        support.constraint_uuid == "constraint-customer-pk" &&
        support.support_uuid == "index-customer-pk" &&
        support.support_family == "rowstore_scalar_btree_v1") {
      saw_support = true;
    }
  }
  for (const auto& name : loaded.state.names) {
    if (name.object_uuid == "constraint-customer-pk" &&
        name.object_kind == "constraint" &&
        name.display_name == "customer_pk") {
      saw_name = true;
    }
  }
  Require(saw_constraint, "table_constraint descriptor was not persisted");
  Require(saw_subject, "table_constraint subject was not persisted");
  Require(saw_key, "table_constraint key descriptor was not persisted");
  Require(saw_support, "table_constraint support binding was not persisted");
  Require(saw_name, "table_constraint name was not persisted");
  RemoveDatabaseArtifacts(path);
}

}  // namespace

int main() {
  RequireRegistryEvidence();
  const auto column_artifacts = RunPipeline(kColumnConstraintSql);
  RequireParserLowering(column_artifacts,
                        kColumnConstraintSql,
                        "column_constraint",
                        {kColumnConstraintSurfaceId});
  RequireEngineDispatch();

  const auto table_artifacts = RunPipeline(kTableConstraintSql);
  RequireParserLowering(table_artifacts,
                        kTableConstraintSql,
                        "table_constraint",
                        {kTableConstraintSurfaceId, kConstraintNameSurfaceId,
                         kConstraintBodySurfaceId});
  RequireTableConstraintEngineDispatch();
  std::cout << "sbsql_create_table_constraint_child_refusal_and_internal_api_component=passed\n";
  return EXIT_SUCCESS;
}

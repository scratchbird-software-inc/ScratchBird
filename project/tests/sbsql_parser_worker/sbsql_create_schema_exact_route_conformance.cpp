// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "ast/ast.hpp"
#include "canonical_sblr_admission_test_helper.hpp"
#include "binder/binder.hpp"
#include "cst/cst.hpp"
#include "database_lifecycle.hpp"
#include "lowering/lowering.hpp"
#include "memory.hpp"
#include "registry/generated/sbsql_generated_registry.hpp"
#include "sblr_admission.hpp"
#include "sblr_dispatch.hpp"
#include "sblr_engine_envelope.hpp"
#include "sblr_opcode_registry.hpp"
#include "catalog/schema_tree_api.hpp"
#include "ddl/create_api.hpp"
#include "observability/show_api.hpp"
#include "transaction/transaction_api.hpp"
#include "uuid.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace scratchbird::parser::sbsql;
namespace api = scratchbird::engine::internal_api;
namespace db = scratchbird::storage::database;
namespace memory = scratchbird::core::memory;
namespace sblr = scratchbird::engine::sblr;
namespace uuid = scratchbird::core::uuid;
using scratchbird::core::platform::UuidKind;

constexpr std::string_view kSql = "CREATE SCHEMA qa_schema;";
constexpr std::string_view kOperationId = "engine.op.ddl_create_schema";
constexpr std::string_view kApiOperationId = "ddl.create_schema";
constexpr std::string_view kOpcode = "SBLR_DDL_CREATE_SCHEMA";
constexpr std::string_view kFamily = "sblr.catalog.mutation.v3";
constexpr std::string_view kSchemaUuid = "019f0000-0000-7000-8000-000000023306";
constexpr std::string_view kLiveRouteSchemaUuid = "019f0000-0000-7000-8000-000000023307";

struct CreateSchemaRowEvidence {
  std::string_view surface_id;
  std::string_view canonical_name;
  std::string_view validation_fixture_id;
};

constexpr std::array<CreateSchemaRowEvidence, 2> kCreateSchemaRows{{
    {"SBSQL-DE4B8AAF6326", "create_schema_stmt", "SBSQL-SURFACE-4F9512A05B14"},
    {"SBSQL-7BA0B928798B", "schema_name", "SBSQL-SURFACE-DF3A68E8CA6C"},
}};

void Require(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << message << '\n';
    std::exit(EXIT_FAILURE);
  }
}

memory::AllocationPolicy MemoryPolicy() {
  memory::AllocationPolicy policy;
  policy.policy_name = "sbsql_create_schema_exact_route_conformance";
  policy.hard_limit_bytes = 64ull * 1024ull * 1024ull;
  policy.soft_limit_bytes = 48ull * 1024ull * 1024ull;
  policy.per_context_limit_bytes = 32ull * 1024ull * 1024ull;
  policy.page_buffer_pool_limit_bytes = 16ull * 1024ull * 1024ull;
  policy.track_allocations = true;
  policy.zero_memory_on_release = true;
  return policy;
}

void ConfigureMemoryFixture() {
  const auto configured = memory::ConfigureDefaultMemoryManagerForFixture(
      MemoryPolicy(), "sbsql_create_schema_exact_route_conformance");
  Require(configured.ok(), "CREATE SCHEMA memory fixture configuration failed");
  Require(configured.fixture_mode,
          "CREATE SCHEMA memory fixture mode was not active");
}

bool HasEvidence(const api::EngineApiResult& result,
                 std::string_view kind,
                 std::string_view id) {
  for (const auto& evidence : result.evidence) {
    if (evidence.evidence_kind == kind && evidence.evidence_id == id) return true;
  }
  return false;
}

std::string DiagnosticField(const Diagnostic& diagnostic,
                            std::string_view name) {
  for (const auto& field : diagnostic.fields) {
    if (field.name == name) return field.value;
  }
  return {};
}

bool HasAuthorityStep(const SblrEnvelope& envelope,
                      std::string_view expected) {
  return std::find(envelope.required_authority_steps.begin(),
                   envelope.required_authority_steps.end(), expected) !=
         envelope.required_authority_steps.end();
}

bool RowHasFieldValue(const api::EngineRowValue& row,
                      std::string_view field_name,
                      std::string_view field_value) {
  for (const auto& field : row.fields) {
    if (field.first == field_name && field.second.encoded_value == field_value) {
      return true;
    }
  }
  return false;
}

bool ResultHasRowWithFieldValue(const api::EngineApiResult& result,
                                std::string_view field_name,
                                std::string_view field_value) {
  for (const auto& row : result.result_shape.rows) {
    if (RowHasFieldValue(row, field_name, field_value)) { return true; }
  }
  return false;
}

SessionContext ParserSession() {
  SessionContext session;
  session.authenticated = true;
  session.session_uuid = "019f0000-0000-7000-8000-000000023101";
  session.connection_uuid = "019f0000-0000-7000-8000-000000023102";
  session.database_uuid = "019f0000-0000-7000-8000-000000023103";
  session.dialect_profile_uuid = "sbsql_v3";
  session.catalog_epoch = 23;
  session.security_policy_epoch = 24;
  session.descriptor_epoch = 25;
  return session;
}

ParserConfig ParserConfigForTest() {
  ParserConfig config;
  config.probe_mode = true;
  config.server_endpoint = "sb_server_name_resolver";
  config.parser_uuid = "019f0000-0000-7000-8000-000000023104";
  config.bundle_contract_id = "sbp_sbsql@create-schema-route-test";
  config.build_id = "sbsql-create-schema-route-test";
  return config;
}

struct PipelineArtifacts {
  CstDocument cst;
  AstDocument ast;
  BoundStatement bound;
  SblrEnvelope envelope;
  SblrVerifierResult verifier;
};

PipelineArtifacts RunPipeline() {
  PipelineArtifacts artifacts;
  const auto session = ParserSession();
  artifacts.cst = BuildCst(kSql);
  artifacts.ast = BuildAst(artifacts.cst);
  artifacts.bound = BindAst(artifacts.ast, artifacts.cst, ParserConfigForTest(), session);
  artifacts.envelope = LowerToSblr(artifacts.bound, artifacts.cst, session);
  artifacts.verifier = VerifySblrEnvelope(artifacts.envelope);
  return artifacts;
}

void RequireRegistryEvidence() {
  for (const auto& row : kCreateSchemaRows) {
    const auto* registry_row = FindGeneratedSurfaceRegistryRowById(row.surface_id);
    Require(registry_row != nullptr, "CREATE SCHEMA generated registry row missing");
    Require(registry_row->canonical_name == row.canonical_name,
            "CREATE SCHEMA generated registry canonical name drifted");
    Require(registry_row->surface_kind == "grammar_production",
            "CREATE SCHEMA generated registry kind drifted");
    Require(registry_row->family == "ddl_catalog",
            "CREATE SCHEMA generated registry family drifted");
    Require(registry_row->source_status == "native_now",
            "CREATE SCHEMA generated registry status drifted");
    Require(registry_row->cluster_scope == "noncluster_or_profile_scoped",
            "CREATE SCHEMA generated registry cluster scope drifted");
    Require(registry_row->sblr_operation_family == kFamily,
            "CREATE SCHEMA generated registry SBLR family drifted");
    Require(registry_row->parser_handler_key == "parser.statement_family.ddl_catalog",
            "CREATE SCHEMA generated registry parser handler drifted");
    Require(registry_row->lowering_handler_key ==
                "lowering.sblr_family.sblr_catalog_mutation_v3",
            "CREATE SCHEMA generated registry lowering handler drifted");
    Require(registry_row->server_admission_key == "server.admission.sblr_catalog_mutation_v3",
            "CREATE SCHEMA generated registry server admission drifted");
    Require(registry_row->engine_rule_key == "engine.rule.sblr_catalog_mutation_v3",
            "CREATE SCHEMA generated registry engine rule drifted");
    Require(registry_row->validation_fixture_id == row.validation_fixture_id,
            "CREATE SCHEMA generated registry fixture id drifted");
  }
}

void RequireExactRefusal(const PipelineArtifacts& artifacts) {
  Require(!artifacts.cst.messages.has_errors(), "CREATE SCHEMA CST failed");
  Require(!artifacts.ast.messages.has_errors(), "CREATE SCHEMA AST failed");
  Require(artifacts.bound.bound, "CREATE SCHEMA bind failed");
  Require(!artifacts.verifier.admitted,
          "CREATE SCHEMA was admitted without the exact engine-bound CSDO carrier");
  Require(artifacts.envelope.operation_family == kFamily,
          "CREATE SCHEMA operation family mismatch");
  Require(artifacts.envelope.sblr_operation_key == kFamily,
          "CREATE SCHEMA SBLR operation key mismatch");
  Require(artifacts.envelope.operation_id == "engine.op.diagnostic_refusal" &&
              artifacts.envelope.sblr_opcode == "SBLR_DIAGNOSTIC_REFUSAL" &&
              artifacts.envelope.engine_api_operation_id == "not_admitted",
          "CREATE SCHEMA did not use the exact non-executable refusal tuple");
  Require(artifacts.envelope.result_shape_key == "diagnostic_vector.v1" &&
              artifacts.envelope.diagnostic_shape_key ==
                  "diagnostic_vector.v1" &&
              artifacts.envelope.resource_contract_key ==
                  "sbsql.command.no_execution.v1",
          "CREATE SCHEMA refusal contract metadata drifted");
  Require(artifacts.envelope.payload.empty() &&
              artifacts.envelope.operands.empty() &&
              artifacts.envelope.resolved_object_uuids.empty() &&
              !artifacts.envelope.parser_executes_sql &&
              !artifacts.envelope.real_file_effects,
          "CREATE SCHEMA refusal emitted executable or parser-owned authority");
  Require(HasAuthorityStep(artifacts.envelope,
                           "authority.parser.syntax_evidence_only") &&
              HasAuthorityStep(artifacts.envelope,
                               "authority.parser.no_executable_sblr") &&
              HasAuthorityStep(artifacts.envelope,
                               "authority.parser.no_sql_text_execution") &&
              HasAuthorityStep(artifacts.envelope,
                               "authority.parser.no_storage_or_finality"),
          "CREATE SCHEMA refusal omitted parser non-authority evidence");
  Require(artifacts.envelope.messages.diagnostics.size() == 1,
          "CREATE SCHEMA did not emit one exact refusal diagnostic");
  const auto& diagnostic = artifacts.envelope.messages.diagnostics.front();
  Require(diagnostic.code == "SBSQL.IMPL.NOT_AVAILABLE" &&
              diagnostic.severity == "ERROR" &&
              DiagnosticField(diagnostic, "canonical_parent_operation_id") ==
                  kOperationId &&
              DiagnosticField(diagnostic, "canonical_parent_sblr_opcode") ==
                  kOpcode &&
              DiagnosticField(diagnostic, "recognized_surface_ids") ==
                  "SBSQL-DE4B8AAF6326,SBSQL-7BA0B928798B" &&
              DiagnosticField(diagnostic, "executable_sblr_emitted") ==
                  "false",
          "CREATE SCHEMA refusal identity or parent mapping drifted");
}

void RequireOpcodeRegistryContract() {
  const auto* opcode_entry = sblr::LookupSblrOperation(kOperationId);
  Require(opcode_entry != nullptr, "CREATE SCHEMA opcode registry row missing");
  Require(opcode_entry->opcode == kOpcode, "CREATE SCHEMA opcode registry opcode drifted");
  Require(opcode_entry->code == 1536 &&
              opcode_entry->operand_contract == "create_schema_descriptor" &&
              opcode_entry->result_contract == "ddl_result" &&
              opcode_entry->executor_id == kOperationId,
          "CREATE SCHEMA exact registry tuple drifted");
  Require(opcode_entry->requires_security_context,
          "CREATE SCHEMA opcode registry security context drifted");
  Require(opcode_entry->requires_transaction_context,
          "CREATE SCHEMA opcode registry transaction context drifted");
}

std::uint64_t CurrentUnixMillis() {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());
}

std::filesystem::path TestDatabasePath() {
  return std::filesystem::temp_directory_path() /
         ("sbsql_create_schema_exact_route_" + std::to_string(CurrentUnixMillis()) +
          ".sbdb");
}

void RemoveDatabaseArtifacts(const std::filesystem::path& path) {
  std::error_code ignored;
  std::filesystem::remove(path, ignored);
  for (const auto suffix : {".sb.api_events",
                            ".sb.name_events",
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
      uuid::GenerateEngineIdentityV7(UuidKind::database, 1779810233000).value;
  create.filespace_uuid =
      uuid::GenerateEngineIdentityV7(UuidKind::filespace, 1779810233001).value;
  create.page_size = 16384;
  create.creation_unix_epoch_millis = 1779810233002;
  create.allow_minimal_resource_bootstrap = true;
  create.require_resource_seed_pack = false;
  create.allow_overwrite = true;
  const auto created = db::CreateDatabaseFile(create);
  if (!created.ok()) {
    std::cerr << created.diagnostic.diagnostic_code << ':'
              << created.diagnostic.message_key << '\n';
  }
  Require(created.ok(), "CREATE SCHEMA engine dispatch test database create failed");
  return uuid::UuidToString(create.database_uuid.value);
}

api::EngineRequestContext EngineContext(const std::filesystem::path& path,
                                        const std::string& database_uuid) {
  api::EngineRequestContext context;
  context.request_id = "sbsql-create-schema-exact-route";
  context.database_path = path.string();
  context.database_uuid.canonical = database_uuid;
  context.session_uuid.canonical = "019f0000-0000-7000-8000-000000023202";
  context.principal_uuid.canonical = "019f0000-0000-7000-8000-000000023203";
  context.security_context_present = true;
  context.catalog_generation_id = 1;
  context.security_epoch = 1;
  context.resource_epoch = 1;
  context.name_resolution_epoch = 1;
  context.trace_tags.push_back("right:CATALOG_MUTATE");
  context.trace_tags.push_back("sbsql_surface_id:SBSQL-DE4B8AAF6326");
  context.trace_tags.push_back("sbsql_surface_id:SBSQL-7BA0B928798B");
  return context;
}

api::EngineRequestContext BeginEngineTransaction(const std::filesystem::path& path,
                                                 const std::string& database_uuid) {
  auto context = EngineContext(path, database_uuid);
  api::EngineBeginTransactionRequest request;
  request.context = context;
  request.isolation_level = "read_committed";
  const auto result = api::EngineBeginTransaction(request);
  for (const auto& diagnostic : result.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.detail << '\n';
  }
  Require(result.ok, "internal API transaction begin did not return success");
  Require(result.local_transaction_id != 0,
          "transaction begin did not return local transaction id");
  context.local_transaction_id = result.local_transaction_id;
  context.transaction_uuid = result.transaction_uuid;
  context.snapshot_visible_through_local_transaction_id =
      result.snapshot_visible_through_local_transaction_id;
  context.transaction_isolation_level = result.isolation_level;
  return context;
}

void CommitEngineTransaction(api::EngineRequestContext context) {
  api::EngineCommitTransactionRequest request;
  request.context = std::move(context);
  const auto result = api::EngineCommitTransaction(request);
  for (const auto& diagnostic : result.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.detail << '\n';
  }
  Require(result.ok, "internal API transaction commit did not return success");
  Require(result.engine_finality_known &&
              result.commit_finality_state == "committed_by_engine_inventory",
          "internal API transaction commit did not reach durable finality");
}

std::string SchemaUuidForPath(const api::EngineRequestContext& context,
                              const std::string& path) {
  for (const auto& schema : api::VisibleSchemaTreeRecords(context,
                                                          context.local_transaction_id)) {
    for (const auto& name : schema.localized_names) {
      if (name.path == path) { return schema.schema_uuid; }
    }
  }
  return {};
}

void RequireSchemaPath(const api::EngineRequestContext& context,
                       std::string_view schema_uuid,
                       std::string_view expected_parent_uuid,
                       std::string_view expected_path,
                       std::string_view expected_name) {
  const auto schema = api::FindVisibleSchemaTreeRecord(context,
                                                       std::string(schema_uuid),
                                                       context.local_transaction_id);
  Require(schema.has_value(), "created schema was not visible in schema tree");
  Require(schema->parent_schema_uuid == expected_parent_uuid,
          "created schema parent UUID was not persisted");
  bool saw_name = false;
  for (const auto& name : schema->localized_names) {
    if (name.path == expected_path && name.name == expected_name) {
      saw_name = true;
      break;
    }
  }
  Require(saw_name, "created schema full path was not persisted in name metadata");
}

void RequireCatalogReadableNavigatorPath(const api::EngineRequestContext& context,
                                         std::string_view expected_object_path) {
  api::EngineShowCatalogRequest request;
  request.context = context;
  request.option_envelopes.push_back("projection:sys.catalog_readable.navigator_tree");
  const auto result = api::EngineShowCatalog(request);
  for (const auto& diagnostic : result.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.detail << '\n';
  }
  Require(result.ok, "EngineShowCatalog navigator projection did not return success");
  Require(ResultHasRowWithFieldValue(result, "object_path", expected_object_path),
          "catalog-readable navigator projection omitted created schema path");
}

api::EngineCreateSchemaRequest EngineCreateSchemaApiRequest(
    const api::EngineRequestContext& context,
    std::string_view schema_uuid,
    std::string_view schema_name,
    std::string_view parent_schema_uuid = {},
    std::string_view full_path = {}) {
  api::EngineCreateSchemaRequest request;
  request.context = context;
  request.target_schema.uuid.canonical = std::string(parent_schema_uuid);
  request.target_schema.object_kind = "schema";
  request.target_object.uuid.canonical = std::string(schema_uuid);
  request.target_object.object_kind = "schema";
  request.localized_names.push_back(
      {"en", "primary", std::string(full_path), std::string(schema_name), true});
  return request;
}

void RequireEngineApiPersistence() {
  const auto path = TestDatabasePath();
  RemoveDatabaseArtifacts(path);
  const auto database_uuid = CreateMinimalDatabase(path);
  auto context = BeginEngineTransaction(path, database_uuid);
  const std::string public_schema_uuid = SchemaUuidForPath(context, "users.public");
  Require(!public_schema_uuid.empty(), "users.public bootstrap schema was not visible");
  const auto result = api::EngineCreateSchema(EngineCreateSchemaApiRequest(
      context, kSchemaUuid, "qa_schema"));
  for (const auto& diagnostic : result.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.detail << '\n';
  }
  Require(result.ok, "EngineCreateSchema internal API did not return success");
  Require(result.operation_id == kApiOperationId,
          "EngineCreateSchema returned wrong operation id");
  Require(result.primary_object.object_kind == "schema",
          "EngineCreateSchema did not return schema primary object");
  Require(result.primary_object.uuid.canonical == kSchemaUuid,
          "EngineCreateSchema returned wrong schema UUID");
  Require(HasEvidence(result, "api_behavior_event", kApiOperationId),
          "EngineCreateSchema missing API behavior event evidence");
  Require(HasEvidence(result, "schema", kSchemaUuid),
          "EngineCreateSchema missing schema UUID evidence");

  const auto live_route_result = api::EngineCreateSchema(
      EngineCreateSchemaApiRequest(context, kLiveRouteSchemaUuid,
                                   "qa_live_schema", public_schema_uuid,
                                   "users.public.qa_live_schema"));
  for (const auto& diagnostic : live_route_result.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.detail << '\n';
  }
  Require(live_route_result.ok,
          "EngineCreateSchema component request did not return success");
  Require(live_route_result.primary_object.uuid.canonical ==
              kLiveRouteSchemaUuid,
          "EngineCreateSchema component returned wrong schema UUID");
  Require(HasEvidence(live_route_result, "schema", kLiveRouteSchemaUuid),
          "EngineCreateSchema component missing schema UUID evidence");
  RequireSchemaPath(context,
                    kLiveRouteSchemaUuid,
                    public_schema_uuid,
                    "users.public.qa_live_schema",
                    "qa_live_schema");
  CommitEngineTransaction(context);
  auto read_context = BeginEngineTransaction(path, database_uuid);
  RequireSchemaPath(read_context,
                    kLiveRouteSchemaUuid,
                    public_schema_uuid,
                    "users.public.qa_live_schema",
                    "qa_live_schema");
  RequireCatalogReadableNavigatorPath(read_context,
                                      "users.public.qa_live_schema");
  CommitEngineTransaction(read_context);
  RemoveDatabaseArtifacts(path);
}

}  // namespace

int main() {
  ConfigureMemoryFixture();
  RequireRegistryEvidence();
  const auto artifacts = RunPipeline();
  RequireExactRefusal(artifacts);
  RequireOpcodeRegistryContract();
  RequireEngineApiPersistence();
  std::cout << "sbsql_create_schema_exact_route_conformance=passed\n";
  return EXIT_SUCCESS;
}

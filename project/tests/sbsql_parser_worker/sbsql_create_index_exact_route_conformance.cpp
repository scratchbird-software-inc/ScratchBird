// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "ast/ast.hpp"
#include "binder/binder.hpp"
#include "cst/cst.hpp"
#include "database_lifecycle.hpp"
#include "lowering/lowering.hpp"
#include "registry/generated/sbsql_generated_registry.hpp"
#include "sblr_admission.hpp"
#include "sblr_dispatch.hpp"
#include "sblr_engine_envelope.hpp"
#include "sblr_opcode_registry.hpp"
#include "uuid.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using namespace scratchbird::parser::sbsql;
namespace api = scratchbird::engine::internal_api;
namespace db = scratchbird::storage::database;
namespace sblr = scratchbird::engine::sblr;
namespace uuid = scratchbird::core::uuid;
using scratchbird::core::platform::UuidKind;

constexpr std::string_view kSql =
    "CREATE INDEX replay_target_id_idx ON replay_target (id);";
constexpr std::string_view kOperationId = "ddl.create_index";
constexpr std::string_view kOpcode = "SBLR_DDL_CREATE_INDEX";
constexpr std::string_view kFamily = "sblr.catalog.mutation.v3";
constexpr std::string_view kSurfaceId = "SBSQL-D09825658F68";
constexpr std::string_view kSurfaceName = "create_index_stmt";
constexpr std::string_view kFixtureId = "SBSQL-SURFACE-F247E52B8B35";
constexpr std::string_view kTableUuid = "019f0000-0000-7000-8000-000000d09801";
constexpr std::string_view kIndexUuid = "019f0000-0000-7000-8000-000000d09802";
constexpr std::string_view kSchemaUuid = "019f0000-0000-7000-8000-000000d09803";

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

bool HasDiagnosticDetailContaining(const api::EngineApiResult& result, std::string_view detail) {
  for (const auto& diagnostic : result.diagnostics) {
    if (Contains(diagnostic.detail, detail)) return true;
  }
  return false;
}

void PrintMessages(const MessageVectorSet& messages) {
  for (const auto& diagnostic : messages.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.message << '\n';
    for (const auto& field : diagnostic.fields) {
      std::cerr << "  " << field.name << '=' << field.value << '\n';
    }
  }
}

SessionContext ParserSession() {
  SessionContext session;
  session.authenticated = true;
  session.session_uuid = "019f0000-0000-7000-8000-000000d09901";
  session.connection_uuid = "019f0000-0000-7000-8000-000000d09902";
  session.database_uuid = "019f0000-0000-7000-8000-000000d09903";
  session.dialect_profile_uuid = "sbsql_v3";
  session.catalog_epoch = 57;
  session.security_policy_epoch = 58;
  session.descriptor_epoch = 59;
  return session;
}

ParserConfig ParserConfigForTest() {
  ParserConfig config;
  config.probe_mode = true;
  config.server_endpoint = "sb_server_name_resolver";
  config.parser_uuid = "019f0000-0000-7000-8000-000000d09904";
  config.bundle_contract_id = "sbp_sbsql@create-index-route-test";
  config.build_id = "sbsql-create-index-route-test";
  return config;
}

struct PipelineArtifacts {
  CstDocument cst;
  AstDocument ast;
  BoundStatement bound;
  SblrEnvelope envelope;
  SblrVerifierResult verifier;
};

PipelineArtifacts RunPipeline(std::string_view sql = kSql) {
  PipelineArtifacts artifacts;
  const auto session = ParserSession();
  artifacts.cst = BuildCst(sql);
  artifacts.ast = BuildAst(artifacts.cst);
  artifacts.bound =
      BindAst(artifacts.ast, artifacts.cst, ParserConfigForTest(), session,
              {std::string(kTableUuid)});
  if (artifacts.bound.resolved_object_uuids.empty()) {
    artifacts.bound.resolved_object_uuids.push_back(std::string(kTableUuid));
  }
  artifacts.envelope = LowerToSblr(artifacts.bound, artifacts.cst, session);
  artifacts.verifier = VerifySblrEnvelope(artifacts.envelope);
  return artifacts;
}

void RequireRegistryEvidence() {
  const auto* registry_row = FindGeneratedSurfaceRegistryRowById(kSurfaceId);
  Require(registry_row != nullptr, "CREATE INDEX generated registry row missing");
  Require(registry_row->canonical_name == kSurfaceName,
          "CREATE INDEX generated registry canonical name drifted");
  Require(registry_row->surface_kind == "grammar_production",
          "CREATE INDEX generated registry kind drifted");
  Require(registry_row->family == "ddl_catalog",
          "CREATE INDEX generated registry family drifted");
  Require(registry_row->source_status == "native_now",
          "CREATE INDEX generated registry status drifted");
  Require(registry_row->cluster_scope == "noncluster_or_profile_scoped",
          "CREATE INDEX generated registry cluster scope drifted");
  Require(registry_row->sblr_operation_family == kFamily,
          "CREATE INDEX generated registry SBLR family drifted");
  Require(registry_row->validation_fixture_id == kFixtureId,
          "CREATE INDEX generated registry fixture id drifted");
}

void RequireExactLowering(const PipelineArtifacts& artifacts) {
  PrintMessages(artifacts.cst.messages);
  PrintMessages(artifacts.ast.messages);
  PrintMessages(artifacts.bound.messages);
  PrintMessages(artifacts.envelope.messages);
  PrintMessages(artifacts.verifier.messages);
  Require(!artifacts.cst.messages.has_errors(), "CREATE INDEX CST failed");
  Require(!artifacts.ast.messages.has_errors(), "CREATE INDEX AST failed");
  Require(artifacts.bound.bound, "CREATE INDEX bind failed");
  Require(artifacts.verifier.admitted, "CREATE INDEX verifier rejected exact route");
  Require(artifacts.envelope.operation_family == kFamily,
          "CREATE INDEX operation family mismatch");
  Require(artifacts.envelope.operation_id == kOperationId,
          "CREATE INDEX operation id mismatch");
  Require(artifacts.envelope.engine_api_operation_id == kOperationId,
          "CREATE INDEX engine API operation id mismatch");
  Require(artifacts.envelope.sblr_opcode == kOpcode,
          "CREATE INDEX SBLR opcode mismatch");
  Require(HasValue(artifacts.envelope.required_rights, "right.catalog_mutate"),
          "CREATE INDEX catalog mutation right missing");
  Require(HasValue(artifacts.envelope.required_authority_steps,
                   "authority.engine.ddl_create_index_api_required"),
          "CREATE INDEX engine DDL authority step missing");
  Require(HasValue(artifacts.envelope.required_authority_steps,
                   "authority.engine.mga_catalog_commit_required"),
          "CREATE INDEX MGA catalog authority step missing");
  Require(HasValue(artifacts.envelope.required_authority_steps,
                   "authority.engine.mga_table_visibility_required"),
          "CREATE INDEX MGA table visibility authority step missing");
  Require(HasValue(artifacts.envelope.required_authority_steps,
                   "authority.parser.no_storage_or_finality"),
          "CREATE INDEX parser no-storage authority step missing");
  Require(HasValue(artifacts.envelope.required_authority_steps,
                   "authority.parser.no_sql_text_execution"),
          "CREATE INDEX parser no-SQL-execution authority step missing");
  Require(!artifacts.envelope.parser_executes_sql,
          "CREATE INDEX lowering allowed parser SQL execution");
  Require(!artifacts.envelope.real_file_effects,
          "CREATE INDEX lowering allowed reference/file effects");
  Require(Contains(artifacts.envelope.payload,
                   "\"catalog_envelope_kind\":\"create_index_ddl\""),
          "CREATE INDEX payload missing catalog envelope kind");
  Require(Contains(artifacts.envelope.payload,
                   "\"catalog_authority\":\"sys.catalog.index\""),
          "CREATE INDEX payload missing catalog authority");
  Require(Contains(artifacts.envelope.payload, "\"target_object_kind\":\"index\""),
          "CREATE INDEX payload missing target object kind");
  Require(Contains(artifacts.envelope.payload,
                   "\"index_target_uuid\":\"019f0000-0000-7000-8000-000000d09801\""),
          "CREATE INDEX payload missing target table UUID evidence");
  Require(Contains(artifacts.envelope.payload, "\"index_key_count\":1"),
          "CREATE INDEX payload missing single-key evidence");
  Require(Contains(artifacts.envelope.payload, "\"index_profile\":\"btree\""),
          "CREATE INDEX payload missing btree profile evidence");
  Require(Contains(artifacts.envelope.payload, "\"index_unique\":false"),
          "CREATE INDEX payload missing non-unique evidence");
  Require(Contains(artifacts.envelope.payload, kSurfaceId),
          "CREATE INDEX payload missing row-identifiable surface evidence");
  Require(Contains(artifacts.envelope.payload, "\"name_text_included\":true"),
          "CREATE INDEX payload did not carry create-object catalog name data");
  Require(Contains(artifacts.envelope.payload, "\"name_text_authority\":false"),
          "CREATE INDEX payload did not prove name text is non-authoritative");
  Require(Contains(artifacts.envelope.payload, "\"sql_text_included\":false"),
          "CREATE INDEX payload did not prove no SQL text authority");
  Require(Contains(artifacts.envelope.payload, "\"parser_executes_sql\":false"),
          "CREATE INDEX payload did not prove parser_executes_sql=false");
  Require(!Contains(artifacts.envelope.payload, std::string(kSql)),
          "CREATE INDEX payload embedded SQL text as authority");
  Require(!Contains(artifacts.envelope.payload, "reference"),
          "CREATE INDEX payload carried reference authority");
  Require(!Contains(artifacts.envelope.payload, "WAL") &&
              !Contains(artifacts.envelope.payload, "wal") &&
              !Contains(artifacts.envelope.payload, "recovery"),
          "CREATE INDEX payload carried WAL/recovery authority");
}

void RequireGeneratedIndexForm(std::string_view sql,
                               std::string_view expected_profile,
                               std::string_view expected_key_fragment,
                               std::string_view expected_flag) {
  const auto artifacts = RunPipeline(sql);
  PrintMessages(artifacts.cst.messages);
  PrintMessages(artifacts.ast.messages);
  PrintMessages(artifacts.bound.messages);
  PrintMessages(artifacts.envelope.messages);
  PrintMessages(artifacts.verifier.messages);
  Require(!artifacts.cst.messages.has_errors(), "generated CREATE INDEX CST failed");
  Require(!artifacts.ast.messages.has_errors(), "generated CREATE INDEX AST failed");
  Require(artifacts.bound.bound, "generated CREATE INDEX bind failed");
  Require(artifacts.verifier.admitted, "generated CREATE INDEX verifier rejected exact route");
  Require(Contains(artifacts.envelope.payload, "\"catalog_envelope_kind\":\"create_index_ddl\""),
          "generated CREATE INDEX missing create-index envelope");
  Require(Contains(artifacts.envelope.payload, expected_profile),
          "generated CREATE INDEX profile was not lowered");
  Require(Contains(artifacts.envelope.payload, expected_key_fragment),
          "generated CREATE INDEX key envelope was not lowered");
  Require(Contains(artifacts.envelope.payload, expected_flag),
          "generated CREATE INDEX feature flag was not lowered");
  Require(Contains(artifacts.envelope.payload, "\"sql_text_included\":false"),
          "generated CREATE INDEX carried SQL text authority");
  Require(Contains(artifacts.envelope.payload, "\"name_text_authority\":false"),
          "generated CREATE INDEX carried authoritative name text");
  Require(!artifacts.envelope.parser_executes_sql,
          "generated CREATE INDEX lowering allowed parser SQL execution");
}

void RequireGeneratedIndexForms() {
  RequireGeneratedIndexForm(
      "CREATE INDEX idx_unique ON replay_target USING UNIQUE_BTREE (id);",
      "\"index_profile\":\"btree_unique\"",
      "\"index_key_envelope\":\"id\"",
      "\"index_predicate_included\":false");
  RequireGeneratedIndexForm(
      "CREATE INDEX idx_partial ON replay_target USING BTREE (id) WHERE id % 2 = 0;",
      "\"index_profile\":\"partial\"",
      "where_mod_eq:id:2=0",
      "\"index_predicate_included\":true");
  RequireGeneratedIndexForm(
      "CREATE INDEX idx_cast ON replay_target USING BTREE (CAST(id AS VARCHAR(512)));",
      "\"index_profile\":\"expression\"",
      "cast:id:varchar",
      "\"index_expression_keys_included\":true");
  RequireGeneratedIndexForm(
      "CREATE INDEX idx_desc ON replay_target USING BTREE (id DESC);",
      "\"index_profile\":\"btree\"",
      "desc:id",
      "\"index_expression_keys_included\":false");
  RequireGeneratedIndexForm(
      "CREATE INDEX idx_covering ON replay_target USING BTREE (id) INCLUDE (id);",
      "\"index_profile\":\"covering\"",
      "include:id",
      "\"index_include_columns_included\":true");
}

void RequireServerAdmission(const SblrEnvelope& envelope) {
  const auto admission = scratchbird::server::AdmitServerSblrEnvelope(
      scratchbird::server::ServerSblrAdmissionRequest{envelope.payload, false});
  Require(admission.admitted, "server admission rejected CREATE INDEX exact route");
  Require(admission.requires_public_abi_dispatch,
          "server admission did not require public ABI dispatch for CREATE INDEX");
  Require(admission.operation_id == kOperationId,
          "server admission CREATE INDEX operation id mismatch");
  Require(admission.operation_family == kFamily,
          "server admission CREATE INDEX operation family mismatch");
  const auto* opcode_entry = sblr::LookupSblrOperation(kOperationId);
  Require(opcode_entry != nullptr, "CREATE INDEX opcode registry row missing");
  Require(opcode_entry->opcode == kOpcode,
          "CREATE INDEX opcode registry opcode drifted");
  Require(opcode_entry->requires_security_context,
          "CREATE INDEX opcode registry security context drifted");
  Require(opcode_entry->requires_transaction_context,
          "CREATE INDEX opcode registry transaction context drifted");
}

std::uint64_t CurrentUnixMillis() {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());
}

std::filesystem::path TestDatabasePath() {
  return std::filesystem::temp_directory_path() /
         ("sbsql_create_index_exact_route_" + std::to_string(CurrentUnixMillis()) +
          ".sbdb");
}

void RemoveDatabaseArtifacts(const std::filesystem::path& path) {
  std::error_code ignored;
  std::filesystem::remove(path, ignored);
  for (const auto suffix : {".sb.api_events",
                            ".sb.crud_events",
                            ".sb.name_events",
                            ".sb.mga_relation_metadata",
                            ".sb.mga_relation_descriptors",
                            ".sb.mga_row_versions",
                            ".sb.mga_index_entries",
                            ".sb.mga_savepoints",
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
      uuid::GenerateEngineIdentityV7(UuidKind::database, 1779810982000).value;
  create.filespace_uuid =
      uuid::GenerateEngineIdentityV7(UuidKind::filespace, 1779810982001).value;
  create.page_size = 16384;
  create.creation_unix_epoch_millis = 1779810982002;
  create.allow_minimal_resource_bootstrap = true;
  create.require_resource_seed_pack = false;
  create.allow_overwrite = true;
  const auto created = db::CreateDatabaseFile(create);
  if (!created.ok()) {
    std::cerr << created.diagnostic.diagnostic_code << ':'
              << created.diagnostic.message_key << '\n';
  }
  Require(created.ok(), "CREATE INDEX engine dispatch test database create failed");
  return uuid::UuidToString(create.database_uuid.value);
}

api::EngineRequestContext EngineContext(const std::filesystem::path& path,
                                        const std::string& database_uuid) {
  api::EngineRequestContext context;
  context.request_id = "sbsql-create-index-exact-route";
  context.database_path = path.string();
  context.database_uuid.canonical = database_uuid;
  context.session_uuid.canonical = "019f0000-0000-7000-8000-000000d09a01";
  context.principal_uuid.canonical = "019f0000-0000-7000-8000-000000d09a02";
  context.current_schema_uuid.canonical = std::string(kSchemaUuid);
  context.security_context_present = true;
  context.catalog_generation_id = 1;
  context.security_epoch = 1;
  context.resource_epoch = 1;
  context.name_resolution_epoch = 1;
  context.trace_tags.push_back("right:CATALOG_MUTATE");
  context.trace_tags.push_back("sbsql_surface_id:SBSQL-D09825658F68");
  return context;
}

api::EngineRequestContext BeginEngineTransaction(const std::filesystem::path& path,
                                                 const std::string& database_uuid) {
  auto context = EngineContext(path, database_uuid);
  auto envelope = sblr::MakeSblrEnvelope("transaction.begin",
                                         "SBLR_TRANSACTION_BEGIN",
                                         "trace.create_index.exact_route.transaction.begin");
  envelope.requires_security_context = true;
  envelope.requires_transaction_context = false;
  envelope.contains_sql_text = false;
  const sblr::SblrDispatchRequest request{context, envelope, api::EngineApiRequest{}};
  const auto result = sblr::DispatchSblrOperation(request);
  for (const auto& diagnostic : result.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.message << '\n';
  }
  for (const auto& diagnostic : result.api_result.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.detail << '\n';
  }
  Require(result.envelope_validated, "transaction begin envelope did not validate");
  Require(result.accepted, "transaction begin dispatch did not accept");
  Require(result.api_result.ok, "transaction begin did not return success");
  Require(result.api_result.local_transaction_id != 0,
          "transaction begin did not return local transaction id");
  context.local_transaction_id = result.api_result.local_transaction_id;
  context.transaction_uuid = result.api_result.transaction_uuid;
  return context;
}

api::EngineApiRequest EngineCreateTableApiRequest() {
  api::EngineApiRequest request;
  request.target_schema.uuid.canonical = std::string(kSchemaUuid);
  request.target_schema.object_kind = "schema";
  request.target_object.uuid.canonical = std::string(kTableUuid);
  request.target_object.object_kind = "table";
  request.localized_names.push_back({"en", "primary", "", "replay_target", true});
  api::EngineColumnDefinition column;
  column.requested_column_uuid.canonical = "019f0000-0000-7000-8000-000000d09804";
  column.names.push_back({"en", "primary", "", "id", true});
  column.descriptor.descriptor_kind = "scalar";
  column.descriptor.canonical_type_name = "int";
  column.descriptor.encoded_descriptor = "type=int";
  column.ordinal = 0;
  column.nullable = true;
  request.columns.push_back(std::move(column));
  return request;
}

api::EngineApiRequest EngineCreateSchemaApiRequest() {
  api::EngineApiRequest request;
  request.target_object.uuid.canonical = std::string(kSchemaUuid);
  request.target_object.object_kind = "schema";
  request.localized_names.push_back({"en", "primary", "", "create_index_exact_route", true});
  return request;
}

api::EngineApiRequest EngineCreateIndexApiRequest(
    std::string index_uuid = std::string(kIndexUuid),
    std::string index_name = "replay_target_id_idx",
    std::string index_kind = "btree",
    std::vector<std::string> key_envelopes = {"id"}) {
  api::EngineApiRequest request;
  request.target_object.uuid.canonical = std::string(kTableUuid);
  request.target_object.object_kind = "table";
  api::EngineIndexDefinition index;
  index.requested_index_uuid.canonical = std::move(index_uuid);
  index.names.push_back({"en", "primary", "", std::move(index_name), true});
  index.index_kind = std::move(index_kind);
  index.key_envelopes = std::move(key_envelopes);
  request.indexes.push_back(std::move(index));
  return request;
}

sblr::SblrOperationEnvelope EngineEnvelope(std::string_view operation_id,
                                           std::string_view opcode,
                                           std::string_view trace_key) {
  auto envelope = sblr::MakeSblrEnvelope(std::string(operation_id),
                                         std::string(opcode),
                                         std::string(trace_key));
  envelope.requires_security_context = true;
  envelope.requires_transaction_context = true;
  envelope.requires_cluster_authority = false;
  envelope.contains_sql_text = false;
  envelope.parser_resolved_names_to_uuids = true;
  return envelope;
}

void RequireCreateIndexDispatch(const api::EngineRequestContext& context,
                                api::EngineApiRequest request,
                                std::string_view expected_family,
                                std::string_view failure_message) {
  const sblr::SblrDispatchRequest create_index_request{
      context,
      EngineEnvelope(kOperationId,
                     kOpcode,
                     "trace.create_index.exact_route.generated_variant"),
      std::move(request)};
  const auto result = sblr::DispatchSblrOperation(create_index_request);
  for (const auto& diagnostic : result.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.message << '\n';
  }
  for (const auto& diagnostic : result.api_result.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.detail << '\n';
  }
  Require(result.envelope_validated, "generated CREATE INDEX SBLR envelope did not validate");
  Require(result.accepted, "generated CREATE INDEX SBLR dispatch did not accept");
  Require(result.dispatched_to_api, "generated CREATE INDEX SBLR dispatch did not route to internal API");
  Require(result.api_result.ok, failure_message);
  Require(HasEvidence(result.api_result, "mga_relation_metadata", "index_create"),
          "generated CREATE INDEX missing index-create MGA evidence");
  Require(HasEvidence(result.api_result, "index_family", expected_family),
          "generated CREATE INDEX missing expected index family evidence");
}

std::string EncodedCreateIndexOperationEnvelope(std::string_view index_uuid,
                                                std::string_view index_name,
                                                std::string_view index_profile,
                                                std::string_view index_key_envelope,
                                                bool unique) {
  std::string envelope;
  envelope += "operation_id=ddl.create_index\n";
  envelope += "opcode=SBLR_DDL_CREATE_INDEX\n";
  envelope += "sblr_operation_family=sblr.catalog.mutation.v3\n";
  envelope += "result_shape=engine.api.result.v1\n";
  envelope += "diagnostic_shape=engine.diagnostic.v1\n";
  envelope += "trace_key=sbsql.create_index.typed_dispatch\n";
  envelope += "contains_sql_text=false\n";
  envelope += "parser_resolved_names_to_uuids=true\n";
  envelope += "requires_security_context=true\n";
  envelope += "requires_transaction_context=true\n";
  envelope += "requires_cluster_authority=false\n";
  envelope += "operand=text\tindex_object_uuid\t";
  envelope += index_uuid;
  envelope += "\n";
  envelope += "operand=text\tindex_name\t";
  envelope += index_name;
  envelope += "\n";
  envelope += "operand=text\tindex_target_uuid\t";
  envelope += kTableUuid;
  envelope += "\n";
  envelope += "operand=text\tindex_target_kind\ttable\n";
  envelope += "operand=text\tindex_profile\t";
  envelope += index_profile;
  envelope += "\n";
  envelope += "operand=text\tindex_key_envelope\t";
  envelope += index_key_envelope;
  envelope += "\n";
  envelope += "operand=text\tindex_unique\t";
  envelope += unique ? "true\n" : "false\n";
  return envelope;
}

void RequireCreateIndexDecodeDispatch(const api::EngineRequestContext& context,
                                      std::string encoded_envelope,
                                      std::string_view expected_family,
                                      std::string_view failure_message) {
  const auto result = sblr::DecodeAndDispatchSblrOperation(encoded_envelope, context);
  for (const auto& diagnostic : result.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.message << '\n';
  }
  for (const auto& diagnostic : result.api_result.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.detail << '\n';
  }
  Require(result.envelope_validated, "decoded CREATE INDEX SBLR envelope did not validate");
  Require(result.accepted, "decoded CREATE INDEX SBLR dispatch did not accept");
  Require(result.dispatched_to_api, "decoded CREATE INDEX SBLR dispatch did not route to internal API");
  Require(result.api_result.ok, failure_message);
  Require(HasEvidence(result.api_result, "index_family", expected_family),
          "decoded CREATE INDEX missing expected index family evidence");
}

void RequireEngineDispatch() {
  const auto path = TestDatabasePath();
  RemoveDatabaseArtifacts(path);
  const auto database_uuid = CreateMinimalDatabase(path);
  auto context = BeginEngineTransaction(path, database_uuid);

  const sblr::SblrDispatchRequest create_schema_request{
      context,
      EngineEnvelope("ddl.create_schema",
                     "SBLR_DDL_CREATE_SCHEMA",
                     "trace.create_index.exact_route.create_schema"),
      EngineCreateSchemaApiRequest()};
  const auto schema_result = sblr::DispatchSblrOperation(create_schema_request);
  for (const auto& diagnostic : schema_result.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.message << '\n';
  }
  for (const auto& diagnostic : schema_result.api_result.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.detail << '\n';
  }
  Require(schema_result.envelope_validated, "CREATE SCHEMA setup envelope did not validate");
  Require(schema_result.accepted, "CREATE SCHEMA setup dispatch did not accept");
  Require(schema_result.api_result.ok, "CREATE SCHEMA setup did not return success");

  const sblr::SblrDispatchRequest missing_target_index_request{
      context,
      EngineEnvelope(kOperationId,
                     kOpcode,
                     "trace.create_index.exact_route.missing_table"),
      EngineCreateIndexApiRequest()};
  const auto missing_target = sblr::DispatchSblrOperation(missing_target_index_request);
  Require(missing_target.envelope_validated,
          "CREATE INDEX missing target envelope did not validate");
  Require(missing_target.accepted,
          "CREATE INDEX missing target dispatch did not accept");
  Require(!missing_target.api_result.ok,
          "CREATE INDEX missing target unexpectedly succeeded");
  Require(HasDiagnosticDetailContaining(missing_target.api_result,
                                        "target_table_not_visible"),
          "CREATE INDEX missing target did not prove MGA table visibility check");

  const sblr::SblrDispatchRequest create_table_request{
      context,
      EngineEnvelope("ddl.create_table",
                     "SBLR_DDL_CREATE_TABLE",
                     "trace.create_index.exact_route.create_table"),
      EngineCreateTableApiRequest()};
  const auto table_result = sblr::DispatchSblrOperation(create_table_request);
  for (const auto& diagnostic : table_result.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.message << '\n';
  }
  for (const auto& diagnostic : table_result.api_result.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.detail << '\n';
  }
  Require(table_result.envelope_validated, "CREATE TABLE setup envelope did not validate");
  Require(table_result.accepted, "CREATE TABLE setup dispatch did not accept");
  Require(table_result.api_result.ok, "CREATE TABLE setup did not return success");

  RequireCreateIndexDispatch(context,
                             EngineCreateIndexApiRequest(),
                             "btree",
                             "EngineCreateIndex did not return success");
  RequireCreateIndexDispatch(context,
                             EngineCreateIndexApiRequest(
                                 "019f0000-0000-7000-8000-000000d09812",
                                 "replay_target_unique_idx",
                                 "btree_unique",
                                 {"id", "unique"}),
                             "btree",
                             "EngineCreateIndex btree_unique route failed");
  RequireCreateIndexDispatch(context,
                             EngineCreateIndexApiRequest(
                                 "019f0000-0000-7000-8000-000000d09813",
                                 "replay_target_expression_idx",
                                 "expression",
                                 {"cast:id:varchar"}),
                             "expression",
                             "EngineCreateIndex expression route failed");
  RequireCreateIndexDispatch(context,
                             EngineCreateIndexApiRequest(
                                 "019f0000-0000-7000-8000-000000d09814",
                                 "replay_target_desc_idx",
                                 "btree",
                                 {"desc:id"}),
                             "btree",
                             "EngineCreateIndex descending btree route failed");
  RequireCreateIndexDispatch(context,
                             EngineCreateIndexApiRequest(
                                 "019f0000-0000-7000-8000-000000d09815",
                                 "replay_target_partial_idx",
                                 "partial",
                                 {"id", "where_mod_eq:id:2=0"}),
                             "partial",
                             "EngineCreateIndex partial route failed");
  RequireCreateIndexDispatch(context,
                             EngineCreateIndexApiRequest(
                                 "019f0000-0000-7000-8000-000000d09816",
                                 "replay_target_covering_idx",
                                 "covering",
                                 {"id", "include:id"}),
                             "covering",
                             "EngineCreateIndex covering route failed");
  RequireCreateIndexDispatch(context,
                             EngineCreateIndexApiRequest(
                                 "019f0000-0000-7000-8000-000000d09817",
                                 "replay_target_in_memory_idx",
                                 "in_memory",
                                 {"id"}),
                             "in_memory",
                             "EngineCreateIndex in-memory route failed");
  RequireCreateIndexDispatch(context,
                             EngineCreateIndexApiRequest(
                                 "019f0000-0000-7000-8000-000000d09818",
                                 "replay_target_reference_emulated_idx",
                                 "reference_emulated",
                                 {"id"}),
                             "reference_emulated",
                             "EngineCreateIndex reference-emulated route failed");
  RequireCreateIndexDecodeDispatch(
      context,
      EncodedCreateIndexOperationEnvelope("019f0000-0000-7000-8000-000000d09822",
                                          "replay_target_typed_cast_idx",
                                          "expression",
                                          "cast:id:varchar",
                                          false),
      "expression",
      "typed CREATE INDEX expression route failed");
  RequireCreateIndexDecodeDispatch(
      context,
      EncodedCreateIndexOperationEnvelope("019f0000-0000-7000-8000-000000d09823",
                                          "replay_target_typed_desc_idx",
                                          "btree",
                                          "desc:id",
                                          false),
      "btree",
      "typed CREATE INDEX descending route failed");
  RequireCreateIndexDecodeDispatch(
      context,
      EncodedCreateIndexOperationEnvelope("019f0000-0000-7000-8000-000000d09824",
                                          "replay_target_typed_unique_idx",
                                          "btree_unique",
                                          "id",
                                          true),
      "btree",
      "typed CREATE INDEX unique route failed");
  RequireCreateIndexDecodeDispatch(
      context,
      EncodedCreateIndexOperationEnvelope("019f0000-0000-7000-8000-000000d09825",
                                          "replay_target_typed_partial_idx",
                                          "partial",
                                          "id,where_mod_eq:id:2=0",
                                          false),
      "partial",
      "typed CREATE INDEX partial route failed");
  RequireCreateIndexDecodeDispatch(
      context,
      EncodedCreateIndexOperationEnvelope("019f0000-0000-7000-8000-000000d09826",
                                          "replay_target_typed_in_memory_idx",
                                          "in_memory",
                                          "id,include:id",
                                          false),
      "in_memory",
      "typed CREATE INDEX in-memory route failed");
  RequireCreateIndexDecodeDispatch(
      context,
      EncodedCreateIndexOperationEnvelope("019f0000-0000-7000-8000-000000d09827",
                                          "replay_target_typed_reference_idx",
                                          "reference_emulated",
                                          "id,where_mod_eq:id:2=0",
                                          false),
      "reference_emulated",
      "typed CREATE INDEX reference-emulated route failed");
  RemoveDatabaseArtifacts(path);
}

}  // namespace

int main() {
  RequireRegistryEvidence();
  const auto artifacts = RunPipeline();
  RequireExactLowering(artifacts);
  RequireGeneratedIndexForms();
  RequireServerAdmission(artifacts.envelope);
  RequireEngineDispatch();
  std::cout << "sbsql_create_index_exact_route_conformance=passed\n";
  return EXIT_SUCCESS;
}

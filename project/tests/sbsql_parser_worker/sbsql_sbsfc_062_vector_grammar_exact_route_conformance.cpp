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
#include "catalog/schema_tree_api.hpp"
#include "registry/generated/sbsql_generated_registry.hpp"
#include "rendering/rendering.hpp"
#include "sblr_admission.hpp"
#include "sblr_dispatch.hpp"
#include "sblr_engine_envelope.hpp"
#include "uuid.hpp"

#include <algorithm>
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
namespace sblr = scratchbird::engine::sblr;
namespace uuid = scratchbird::core::uuid;
using scratchbird::core::platform::UuidKind;

constexpr std::string_view kCollectionUuid = "019f0000-0000-7000-8000-000000062001";
constexpr std::string_view kCreatedCollectionUuid = "019f0000-0000-7000-8000-000000062002";
constexpr std::string_view kVectorColumnUuid = "019f0000-0000-7000-8000-000000062003";
constexpr std::string_view kIdColumnUuid = "019f0000-0000-7000-8000-000000062004";

struct GrammarRow {
  std::string_view surface_id;
  std::string_view canonical_name;
  std::string_view canonical_sblr_operation_family;
};

constexpr GrammarRow kRows[] = {
    {"SBSQL-094A8BF58B40", "vector_search_body", "sblr.query.multimodel_or_ddl.v3"},
    {"SBSQL-0EEEB7598628", "vector_op", "sblr.query.multimodel_or_ddl.v3"},
    {"SBSQL-1DBECA8A20A7", "vector_metric", "sblr.query.multimodel_or_ddl.v3"},
    {"SBSQL-3C937B646A90", "vector_rerank_clause", "sblr.query.multimodel_or_ddl.v3"},
    {"SBSQL-3DA278E4B3B5", "vector_filter", "sblr.query.multimodel_or_ddl.v3"},
    {"SBSQL-43D593BC6E94", "vector_op_stmt", "sblr.query.multimodel_or_ddl.v3"},
    {"SBSQL-5FFAE2D1CBA9", "vector_search_target", "sblr.query.multimodel_or_ddl.v3"},
    {"SBSQL-77751BFB5DC8", "vector_search_query", "sblr.query.relational.v3"},
    {"SBSQL-78EAD21925B4", "vector_modifier", "sblr.query.multimodel_or_ddl.v3"},
    {"SBSQL-860D952A1FD1", "vector_search_expr", "sblr.query.multimodel_or_ddl.v3"},
    {"SBSQL-8AA6DA462354", "vector_search_paging", "sblr.query.multimodel_or_ddl.v3"},
    {"SBSQL-B251723259F5", "vector_value", "sblr.query.multimodel_or_ddl.v3"},
    {"SBSQL-BD714DD6A2E8", "vector_field_modifier", "sblr.query.multimodel_or_ddl.v3"},
    {"SBSQL-F4564DE055AE", "vector_collection_clause", "sblr.query.multimodel_or_ddl.v3"},
    {"SBSQL-F4FC81A6CD2F", "vector_search_arg", "sblr.query.multimodel_or_ddl.v3"},
    {"SBSQL-F71131A52D48", "create_vector_collection", "sblr.catalog.mutation.v3"},
};

struct VectorCase {
  std::string_view sql;
  std::string_view operation_id;
  std::string_view opcode;
  std::string_view operation_family;
  std::vector<std::string_view> surface_ids;
  std::vector<std::string> resolved_uuids;
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

SessionContext ParserSession() {
  SessionContext session;
  session.authenticated = true;
  session.session_uuid = "019f0000-0000-7000-8000-000000062101";
  session.connection_uuid = "019f0000-0000-7000-8000-000000062102";
  session.database_uuid = "019f0000-0000-7000-8000-000000062103";
  session.dialect_profile_uuid = "sbsql_v3";
  session.catalog_epoch = 262;
  session.security_policy_epoch = 263;
  session.descriptor_epoch = 264;
  return session;
}

ParserConfig ParserConfigForTest() {
  ParserConfig config;
  config.probe_mode = true;
  config.server_endpoint = "sb_server_sbsfc_062_vector_grammar_route";
  config.parser_uuid = "019f0000-0000-7000-8000-000000062104";
  config.bundle_contract_id = "sbp_sbsql@sbsfc-062-vector-grammar-route-test";
  config.build_id = "sbsql-sbsfc-062-vector-grammar-route-test";
  return config;
}

struct PipelineArtifacts {
  CstDocument cst;
  AstDocument ast;
  BoundStatement bound;
  SblrEnvelope envelope;
  SblrVerifierResult verifier;
};

PipelineArtifacts RunPipeline(const VectorCase& test_case) {
  PipelineArtifacts artifacts;
  const auto session = ParserSession();
  artifacts.cst = BuildCst(std::string(test_case.sql));
  artifacts.ast = BuildAst(artifacts.cst);
  artifacts.bound = BindAst(artifacts.ast,
                            artifacts.cst,
                            ParserConfigForTest(),
                            session,
                            test_case.resolved_uuids);
  artifacts.envelope = LowerToSblr(artifacts.bound, artifacts.cst, session);
  artifacts.verifier = VerifySblrEnvelope(artifacts.envelope);
  return artifacts;
}

void RequireRegistryEvidence() {
  for (const auto& row : kRows) {
    const auto* registry_row = FindGeneratedSurfaceRegistryRowById(row.surface_id);
    Require(registry_row != nullptr, "SBSFC-062 generated registry row missing");
    Require(registry_row->canonical_name == row.canonical_name,
            "SBSFC-062 generated registry canonical name drifted");
    Require(registry_row->surface_kind == "grammar_production",
            "SBSFC-062 generated registry kind drifted");
    Require(registry_row->source_status == "native_now",
            "SBSFC-062 generated registry status drifted");
    Require(registry_row->cluster_scope == "noncluster_or_profile_scoped",
            "SBSFC-062 generated registry cluster scope drifted");
    Require(registry_row->sblr_operation_family == row.canonical_sblr_operation_family,
            "SBSFC-062 generated registry SBLR family drifted");
  }
}

void RequireExactLowering(const VectorCase& test_case,
                          const PipelineArtifacts& artifacts) {
  if (artifacts.cst.messages.has_errors()) {
    std::cerr << RenderMessageVectorSet(artifacts.cst.messages);
  }
  if (artifacts.ast.messages.has_errors()) {
    std::cerr << RenderMessageVectorSet(artifacts.ast.messages);
  }
  if (!artifacts.bound.bound) {
    std::cerr << RenderMessageVectorSet(artifacts.bound.messages);
  }
  if (!artifacts.verifier.admitted) {
    std::cerr << RenderMessageVectorSet(artifacts.verifier.messages);
  }
  Require(!artifacts.cst.messages.has_errors(), "SBSFC-062 CST failed");
  Require(!artifacts.ast.messages.has_errors(), "SBSFC-062 AST failed");
  Require(artifacts.bound.bound, "SBSFC-062 bind failed");
  Require(artifacts.verifier.admitted, "SBSFC-062 verifier rejected exact route");
  Require(artifacts.envelope.operation_id == test_case.operation_id,
          "SBSFC-062 operation id mismatch");
  Require(artifacts.envelope.sblr_opcode == test_case.opcode,
          "SBSFC-062 SBLR opcode mismatch");
  Require(artifacts.envelope.operation_family == test_case.operation_family,
          "SBSFC-062 operation family mismatch");
  Require(HasValue(artifacts.envelope.required_authority_steps,
                   "authority.parser.no_sql_text_execution"),
          "SBSFC-062 no-SQL-execution authority missing");
  Require(HasValue(artifacts.envelope.required_authority_steps,
                   "authority.parser.no_storage_or_finality"),
          "SBSFC-062 no-storage-finality authority missing");
  Require(!artifacts.envelope.parser_executes_sql,
          "SBSFC-062 lowering allowed parser SQL execution");
  Require(Contains(artifacts.envelope.payload, "\"sql_text_included\":false"),
          "SBSFC-062 payload did not prove no SQL text authority");
  Require(!Contains(artifacts.envelope.payload, test_case.sql),
          "SBSFC-062 payload embedded source SQL text");
  Require(!Contains(artifacts.envelope.payload, "SBSQL_SURFACE_REPLAY") &&
              !Contains(artifacts.envelope.payload, "replay") &&
              !Contains(artifacts.envelope.payload, "refusal"),
          "SBSFC-062 payload used forbidden replay/refusal evidence");
  Require(!Contains(artifacts.envelope.payload, "WAL") &&
              !Contains(artifacts.envelope.payload, "wal") &&
              !Contains(artifacts.envelope.payload, "recovery"),
          "SBSFC-062 payload carried WAL/recovery authority");
  for (const auto surface_id : test_case.surface_ids) {
    Require(Contains(artifacts.envelope.payload, surface_id),
            std::string("SBSFC-062 payload missing row marker ") +
                std::string(surface_id));
  }
}

void RequireServerAdmission(const VectorCase& test_case,
                            const SblrEnvelope& envelope) {
  const auto admission = scratchbird::server::AdmitServerSblrEnvelope(
      scratchbird::server::ServerSblrAdmissionRequest{envelope.payload, false});
  Require(admission.admitted, "server admission rejected SBSFC-062 exact route");
  Require(admission.requires_public_abi_dispatch,
          "server admission did not require public ABI dispatch for SBSFC-062");
  Require(admission.operation_id == test_case.operation_id,
          "server admission SBSFC-062 operation id mismatch");
}

api::EngineRequestContext EngineContext() {
  api::EngineRequestContext context;
  context.request_id = "sbsql-sbsfc-062-vector-grammar-exact-route";
  context.database_uuid.canonical = "019f0000-0000-7000-8000-000000062201";
  context.node_uuid.canonical = "019f0000-0000-7000-8000-000000062202";
  context.session_uuid.canonical = "019f0000-0000-7000-8000-000000062203";
  context.principal_uuid.canonical = "019f0000-0000-7000-8000-000000062204";
  context.transaction_uuid.canonical = "019f0000-0000-7000-8000-000000062205";
  context.statement_uuid.canonical = "019f0000-0000-7000-8000-000000062206";
  context.current_schema_uuid.canonical = "019f0000-0000-7000-8000-000000062207";
  context.current_role_uuid.canonical = "019f0000-0000-7000-8000-000000062208";
  context.local_transaction_id = 262;
  context.security_context_present = true;
  context.catalog_generation_id = 1;
  context.security_epoch = 1;
  context.resource_epoch = 1;
  context.name_resolution_epoch = 1;
  context.trace_tags.push_back("right:VECTOR_SURFACE_TEST");
  for (const auto& row : kRows) {
    context.trace_tags.push_back(std::string("sbsql_surface_id:") +
                                 std::string(row.surface_id));
  }
  return context;
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
  envelope.operands.push_back({"text", "target_object_uuid", std::string(kCollectionUuid)});
  envelope.operands.push_back({"text", "target_object_kind", "vector_collection"});
  return envelope;
}

void RequireVectorSearchDispatch() {
  const sblr::SblrDispatchRequest request{
      EngineContext(),
      EngineEnvelope("nosql.vector_search",
                     "SBLR_NOSQL_VECTOR_SEARCH",
                     "trace.sbsfc062.vector.search"),
      api::EngineApiRequest{}};
  const auto result = sblr::DispatchSblrOperation(request);
  Require(result.envelope_validated, "SBSFC-062 vector search envelope invalid");
  Require(result.accepted, "SBSFC-062 vector search dispatch rejected");
  Require(result.dispatched_to_api, "SBSFC-062 vector search did not dispatch to API");
  Require(result.api_result.ok, "EngineVectorSearch did not return success");
  Require(HasEvidence(result.api_result, "vector_search", "exact_fallback_available"),
          "SBSFC-062 vector search runtime evidence missing");
  Require(HasEvidence(result.api_result, "nosql_surface", "vector"),
          "SBSFC-062 vector search surface evidence missing");
}

void RequireVectorCollectionOperationDispatch() {
  const sblr::SblrDispatchRequest request{
      EngineContext(),
      EngineEnvelope("nosql.vector_collection_op",
                     "SBLR_NOSQL_VECTOR_COLLECTION_OP",
                     "trace.sbsfc062.vector.collection_op"),
      api::EngineApiRequest{}};
  const auto result = sblr::DispatchSblrOperation(request);
  Require(result.envelope_validated, "SBSFC-062 vector op envelope invalid");
  Require(result.accepted, "SBSFC-062 vector op dispatch rejected");
  Require(result.dispatched_to_api, "SBSFC-062 vector op did not dispatch to API");
  Require(result.api_result.ok, "EngineVectorCollectionOperation did not return success");
  Require(HasEvidence(result.api_result,
                      "vector_collection_operation",
                      "local_operation_admitted"),
          "SBSFC-062 vector op runtime evidence missing");
}

std::uint64_t CurrentUnixMillis() {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());
}

std::filesystem::path TestDatabasePath() {
  return std::filesystem::temp_directory_path() /
         ("sbsql_sbsfc_062_vector_exact_route_" +
          std::to_string(CurrentUnixMillis()) + ".sbdb");
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
                            ".sb.owner.lock"}) {
    std::filesystem::remove(path.string() + suffix, ignored);
  }
}

std::string CreateMinimalDatabase(const std::filesystem::path& path) {
  db::DatabaseCreateConfig create;
  create.path = path.string();
  create.database_uuid =
      uuid::GenerateEngineIdentityV7(UuidKind::database, 1779810622000).value;
  create.filespace_uuid =
      uuid::GenerateEngineIdentityV7(UuidKind::filespace, 1779810622001).value;
  create.page_size = 16384;
  create.creation_unix_epoch_millis = 1779810622002;
  create.allow_minimal_resource_bootstrap = true;
  create.require_resource_seed_pack = false;
  create.allow_overwrite = true;
  const auto created = db::CreateDatabaseFile(create);
  Require(created.ok(), "SBSFC-062 test database create failed");
  return uuid::UuidToString(create.database_uuid.value);
}

api::EngineRequestContext EngineContextForDatabase(const std::filesystem::path& path,
                                                   const std::string& database_uuid) {
  auto context = EngineContext();
  context.database_path = path.string();
  context.database_uuid.canonical = database_uuid;
  context.current_schema_uuid.canonical.clear();
  context.local_transaction_id = 0;
  context.transaction_uuid.canonical.clear();
  return context;
}

std::string SchemaUuidForPath(const api::EngineRequestContext& context,
                              const std::string& path) {
  for (const auto& schema : api::VisibleSchemaTreeRecords(context,
                                                          context.local_transaction_id)) {
    for (const auto& name : schema.localized_names) {
      if (name.path == path) return schema.schema_uuid;
    }
  }
  return {};
}

api::EngineRequestContext BeginEngineTransaction(const std::filesystem::path& path,
                                                 const std::string& database_uuid) {
  auto context = EngineContextForDatabase(path, database_uuid);
  auto envelope = sblr::MakeSblrEnvelope("transaction.begin",
                                         "SBLR_TRANSACTION_BEGIN",
                                         "trace.sbsfc062.vector.transaction.begin");
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
  Require(result.envelope_validated, "SBSFC-062 transaction begin envelope invalid");
  Require(result.accepted, "SBSFC-062 transaction begin dispatch rejected");
  Require(result.api_result.ok, "SBSFC-062 transaction begin failed");
  context.local_transaction_id = result.api_result.local_transaction_id;
  context.transaction_uuid = result.api_result.transaction_uuid;
  context.snapshot_visible_through_local_transaction_id = context.local_transaction_id;
  return context;
}

api::EngineApiRequest EngineCreateVectorCollectionApiRequest(std::string_view schema_uuid) {
  api::EngineApiRequest request;
  request.target_schema.uuid.canonical = std::string(schema_uuid);
  request.target_schema.object_kind = "schema";
  request.target_object.uuid.canonical = std::string(kCreatedCollectionUuid);
  request.target_object.object_kind = "table";
  request.localized_names.push_back({"en", "primary", "", "vec_docs", true});
  request.physical_profile.encoded_profiles.push_back(
      "vector_collection:dimension=3;metric=cosine;index=hnsw");

  api::EngineColumnDefinition id_column;
  id_column.requested_column_uuid.canonical = std::string(kIdColumnUuid);
  id_column.names.push_back({"en", "primary", "", "id", true});
  id_column.descriptor.descriptor_kind = "scalar";
  id_column.descriptor.canonical_type_name = "int";
  id_column.descriptor.encoded_descriptor = "type=int";
  id_column.ordinal = 0;
  id_column.nullable = false;
  request.columns.push_back(std::move(id_column));

  api::EngineColumnDefinition vector_column;
  vector_column.requested_column_uuid.canonical = std::string(kVectorColumnUuid);
  vector_column.names.push_back({"en", "primary", "", "embedding", true});
  vector_column.descriptor.descriptor_kind = "scalar";
  vector_column.descriptor.canonical_type_name = "dense_vector";
  vector_column.descriptor.encoded_descriptor = "type=dense_vector;dimension=3";
  vector_column.ordinal = 1;
  vector_column.nullable = false;
  request.columns.push_back(std::move(vector_column));
  return request;
}

void RequireCreateVectorCollectionDispatch() {
  const auto path = TestDatabasePath();
  RemoveDatabaseArtifacts(path);
  const auto database_uuid = CreateMinimalDatabase(path);
  auto context = BeginEngineTransaction(path, database_uuid);
  context.current_schema_uuid.canonical = SchemaUuidForPath(context, "users.public");
  Require(!context.current_schema_uuid.canonical.empty(),
          "SBSFC-062 bootstrapped users.public schema missing");
  const sblr::SblrDispatchRequest request{
      context,
      EngineEnvelope("ddl.create_table",
                     "SBLR_DDL_CREATE_TABLE",
                     "trace.sbsfc062.vector.create_collection"),
      EngineCreateVectorCollectionApiRequest(context.current_schema_uuid.canonical)};
  const auto result = sblr::DispatchSblrOperation(request);
  for (const auto& diagnostic : result.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.message << '\n';
  }
  for (const auto& diagnostic : result.api_result.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.detail << '\n';
  }
  Require(result.envelope_validated, "SBSFC-062 create vector collection envelope invalid");
  Require(result.accepted, "SBSFC-062 create vector collection dispatch rejected");
  Require(result.dispatched_to_api, "SBSFC-062 create vector collection did not dispatch");
  Require(result.api_result.ok, "EngineCreateTable vector collection route failed");
  Require(HasEvidence(result.api_result, "mga_relation_metadata", "table_create"),
          "SBSFC-062 create vector collection metadata evidence missing");
  RemoveDatabaseArtifacts(path);
}

}  // namespace

int main() {
  RequireRegistryEvidence();
  const std::vector<VectorCase> cases = {
      {"SEARCH vec_docs v WITH (VECTOR = VECTOR '[1,0,0]', METRIC = COSINE, LIMIT = 5, OFFSET = 1, EF_SEARCH = 16, NPROBE = 4, RADIUS = 0.75, OUTPUT_FIELDS = (doc_id, title)) WHERE tag = 'native' BATCH 2 OFFSET 0 RERANK BY RRF(60);",
       "nosql.vector_search",
       "SBLR_NOSQL_VECTOR_SEARCH",
       "sblr.query.multimodel_or_ddl.v3",
       {"SBSQL-77751BFB5DC8",
        "SBSQL-5FFAE2D1CBA9",
        "SBSQL-094A8BF58B40",
        "SBSQL-F4FC81A6CD2F",
        "SBSQL-1DBECA8A20A7",
        "SBSQL-B251723259F5",
        "SBSQL-3DA278E4B3B5",
        "SBSQL-8AA6DA462354",
        "SBSQL-3C937B646A90"},
       {std::string(kCollectionUuid)}},
      {"SEARCH vec_docs embedding <-> VECTOR '[1,0,0]' TOP 3;",
       "nosql.vector_search",
       "SBLR_NOSQL_VECTOR_SEARCH",
       "sblr.query.multimodel_or_ddl.v3",
       {"SBSQL-77751BFB5DC8",
        "SBSQL-5FFAE2D1CBA9",
        "SBSQL-094A8BF58B40",
        "SBSQL-860D952A1FD1",
        "SBSQL-0EEEB7598628",
        "SBSQL-78EAD21925B4",
        "SBSQL-B251723259F5"},
       {std::string(kCollectionUuid)}},
      {"CREATE VECTOR COLLECTION IF NOT EXISTS vec_docs DIMENSION 3 METRIC COSINE INDEX METHOD HNSW PAYLOAD (doc_id BIGINT, embedding VECTOR INDEX METHOD HNSW METRIC COSINE) WITH (profile = 'local');",
       "ddl.create_table",
       "SBLR_DDL_CREATE_TABLE",
       "sblr.catalog.mutation.v3",
       {"SBSQL-F71131A52D48",
        "SBSQL-F4564DE055AE",
        "SBSQL-1DBECA8A20A7",
        "SBSQL-BD714DD6A2E8"},
       {}},
      {"REINDEX VECTOR COLLECTION vec_docs WITH (METHOD = HNSW);",
       "nosql.vector_collection_op",
       "SBLR_NOSQL_VECTOR_COLLECTION_OP",
       "sblr.query.multimodel_or_ddl.v3",
       {"SBSQL-43D593BC6E94"},
       {std::string(kCollectionUuid)}},
  };
  for (const auto& test_case : cases) {
    const auto artifacts = RunPipeline(test_case);
    RequireExactLowering(test_case, artifacts);
    RequireServerAdmission(test_case, artifacts.envelope);
  }
  RequireVectorSearchDispatch();
  RequireVectorCollectionOperationDispatch();
  RequireCreateVectorCollectionDispatch();
  std::cout << "sbsql_sbsfc_062_vector_grammar_exact_route_conformance=passed\n";
  return EXIT_SUCCESS;
}

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
#include "rendering/rendering.hpp"
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
#include <vector>

namespace {

using namespace scratchbird::parser::sbsql;
namespace api = scratchbird::engine::internal_api;
namespace db = scratchbird::storage::database;
namespace sblr = scratchbird::engine::sblr;
namespace uuid = scratchbird::core::uuid;
using scratchbird::core::platform::UuidKind;

constexpr std::string_view kTargetUuid = "019f0000-0000-7000-8000-000000079001";

struct CaseRow {
  std::string_view surface_id;
  std::string_view canonical_name;
  std::string_view sql;
  std::string_view operation_id;
  std::string_view opcode;
  std::string_view engine_api_function;
  std::string_view runtime_evidence_kind;
  std::string_view runtime_evidence_id;
  std::string_view target_object_kind;
  bool mutation;
};

const CaseRow kCases[] = {
    {"SBSQL-05E754B16D54", "keyvalue_op_stmt", "KEYVALUE PUT session VALUE 'v';", "nosql.key_value_put", "SBLR_NOSQL_KEY_VALUE_PUT", "EngineKeyValuePut", "nosql_behavior", "physical_provider_put", "kv_key", true},
    {"SBSQL-0684D68A5C1E", "cypher_node_pattern", "CYPHER NODE PATTERN n;", "nosql.graph_query", "SBLR_NOSQL_GRAPH_QUERY", "EngineGraphQuery", "nosql_behavior", "local_descriptor_scan", "graph", false},
    {"SBSQL-0E501E2836E3", "cypher_return_list", "CYPHER RETURN LIST n;", "nosql.graph_query", "SBLR_NOSQL_GRAPH_QUERY", "EngineGraphQuery", "nosql_behavior", "local_descriptor_scan", "graph", false},
    {"SBSQL-1312C775BE58", "property_match", "PROPERTY MATCH status;", "nosql.graph_query", "SBLR_NOSQL_GRAPH_QUERY", "EngineGraphQuery", "nosql_behavior", "local_descriptor_scan", "graph", false},
    {"SBSQL-1BEB384F9390", "cypher_with_pipeline_clause", "CYPHER WITH PIPELINE n;", "nosql.graph_query", "SBLR_NOSQL_GRAPH_QUERY", "EngineGraphQuery", "nosql_behavior", "local_descriptor_scan", "graph", false},
    {"SBSQL-2070E4830DA6", "cypher_constraint_predicate", "CYPHER CONSTRAINT PREDICATE unique;", "nosql.graph_query", "SBLR_NOSQL_GRAPH_QUERY", "EngineGraphQuery", "nosql_behavior", "local_descriptor_scan", "graph", false},
    {"SBSQL-22B237EEC450", "doc_write_options", "DOC WRITE OPTIONS UPSERT;", "nosql.document_update", "SBLR_NOSQL_DOCUMENT_UPDATE", "EngineDocumentUpdate", "nosql_behavior", "persisted_document_update", "document_collection", true},
    {"SBSQL-2449778EBEFF", "node_match", "NODE MATCH person;", "nosql.graph_query", "SBLR_NOSQL_GRAPH_QUERY", "EngineGraphQuery", "nosql_behavior", "local_descriptor_scan", "graph", false},
    {"SBSQL-291BCF06C348", "fulltext_filter", "FULLTEXT FILTER status;", "nosql.search_query", "SBLR_NOSQL_SEARCH_QUERY", "EngineSearchQuery", "nosql_behavior", "specialized_descriptor_fallback", "search_index", false},
    {"SBSQL-2A100566FFA5", "cypher_relationship_pattern", "CYPHER RELATIONSHIP PATTERN knows;", "nosql.graph_query", "SBLR_NOSQL_GRAPH_QUERY", "EngineGraphQuery", "nosql_behavior", "local_descriptor_scan", "graph", false},
    {"SBSQL-2E68B4999498", "cypher_remove_clause", "CYPHER REMOVE CLAUSE n.flag;", "nosql.graph_query", "SBLR_NOSQL_GRAPH_QUERY", "EngineGraphQuery", "nosql_behavior", "local_descriptor_scan", "graph", false},
    {"SBSQL-2EB813F67044", "edge_pattern", "EDGE PATTERN knows;", "nosql.graph_query", "SBLR_NOSQL_GRAPH_QUERY", "EngineGraphQuery", "nosql_behavior", "local_descriptor_scan", "graph", false},
    {"SBSQL-2F241A19F456", "cypher_stmt", "CYPHER STMT MATCH n;", "nosql.graph_query", "SBLR_NOSQL_GRAPH_QUERY", "EngineGraphQuery", "nosql_behavior", "local_descriptor_scan", "graph", false},
    {"SBSQL-2F2E87E657F5", "multi_model_op_stmt", "MULTIMODEL OP GRAPH;", "nosql.graph_query", "SBLR_NOSQL_GRAPH_QUERY", "EngineGraphQuery", "nosql_behavior", "local_descriptor_scan", "graph", false},
    {"SBSQL-303A7136FED8", "recursive_path_expr", "RECURSIVE PATH EXPR p;", "nosql.graph_query", "SBLR_NOSQL_GRAPH_QUERY", "EngineGraphQuery", "nosql_behavior", "local_descriptor_scan", "graph", false},
    {"SBSQL-325299EE7C73", "cypher_call_clause", "CYPHER CALL CLAUSE proc;", "nosql.graph_query", "SBLR_NOSQL_GRAPH_QUERY", "EngineGraphQuery", "nosql_behavior", "local_descriptor_scan", "graph", false},
    {"SBSQL-36AA57B12B71", "fulltext_aggs", "FULLTEXT AGGS COUNT;", "nosql.search_query", "SBLR_NOSQL_SEARCH_QUERY", "EngineSearchQuery", "nosql_behavior", "specialized_descriptor_fallback", "search_index", false},
    {"SBSQL-3AD0ED09F1FF", "cypher_set_item", "CYPHER SET ITEM n.flag;", "nosql.graph_query", "SBLR_NOSQL_GRAPH_QUERY", "EngineGraphQuery", "nosql_behavior", "local_descriptor_scan", "graph", false},
    {"SBSQL-445D9B87C50B", "cypher_range_spec", "CYPHER RANGE SPEC 1 3;", "nosql.graph_query", "SBLR_NOSQL_GRAPH_QUERY", "EngineGraphQuery", "nosql_behavior", "local_descriptor_scan", "graph", false},
    {"SBSQL-4C27542D83FF", "traversal_strategy", "TRAVERSAL STRATEGY BREADTH FIRST;", "nosql.graph_query", "SBLR_NOSQL_GRAPH_QUERY", "EngineGraphQuery", "nosql_behavior", "local_descriptor_scan", "graph", false},
    {"SBSQL-5125E2546D11", "neo4j_node_constraint", "NEO4J NODE CONSTRAINT unique;", "nosql.graph_query", "SBLR_NOSQL_GRAPH_QUERY", "EngineGraphQuery", "nosql_behavior", "local_descriptor_scan", "graph", false},
    {"SBSQL-525F59FB9EEE", "doc_pipeline_options", "DOC PIPELINE OPTIONS LIMIT 10;", "nosql.document_find", "SBLR_NOSQL_DOCUMENT_FIND", "EngineDocumentFind", "nosql_behavior", "local_descriptor_scan", "document_collection", false},
    {"SBSQL-5A8AC604250A", "map_constructor", "MAP CONSTRUCTOR key value;", "nosql.document_find", "SBLR_NOSQL_DOCUMENT_FIND", "EngineDocumentFind", "nosql_behavior", "local_descriptor_scan", "document_collection", false},
    {"SBSQL-5A9F91459388", "cypher_foreach_clause", "CYPHER FOREACH CLAUSE n;", "nosql.graph_query", "SBLR_NOSQL_GRAPH_QUERY", "EngineGraphQuery", "nosql_behavior", "local_descriptor_scan", "graph", false},
    {"SBSQL-5AA5DF7E6D13", "multi_model_method", "MULTIMODEL METHOD graph.query;", "nosql.graph_query", "SBLR_NOSQL_GRAPH_QUERY", "EngineGraphQuery", "nosql_behavior", "local_descriptor_scan", "graph", false},
    {"SBSQL-5B83FF7701AD", "doc_crud_stmt", "DOC CRUD UPDATE docs;", "nosql.document_update", "SBLR_NOSQL_DOCUMENT_UPDATE", "EngineDocumentUpdate", "nosql_behavior", "persisted_document_update", "document_collection", true},
    {"SBSQL-5D4BADBF355D", "map_entry", "MAP ENTRY key value;", "nosql.document_find", "SBLR_NOSQL_DOCUMENT_FIND", "EngineDocumentFind", "nosql_behavior", "local_descriptor_scan", "document_collection", false},
    {"SBSQL-630BA8179CD6", "doc_pipeline_stage", "DOC PIPELINE STAGE MATCH;", "nosql.document_find", "SBLR_NOSQL_DOCUMENT_FIND", "EngineDocumentFind", "nosql_behavior", "local_descriptor_scan", "document_collection", false},
    {"SBSQL-7AD416BAC2DE", "cypher_optional_match", "CYPHER OPTIONAL MATCH n;", "nosql.graph_query", "SBLR_NOSQL_GRAPH_QUERY", "EngineGraphQuery", "nosql_behavior", "local_descriptor_scan", "graph", false},
    {"SBSQL-7B17AA8608A0", "doc_pipeline_stmt", "DOC PIPELINE STMT docs;", "nosql.document_find", "SBLR_NOSQL_DOCUMENT_FIND", "EngineDocumentFind", "nosql_behavior", "local_descriptor_scan", "document_collection", false},
    {"SBSQL-816FDCD9486B", "cypher_clause", "CYPHER CLAUSE MATCH;", "nosql.graph_query", "SBLR_NOSQL_GRAPH_QUERY", "EngineGraphQuery", "nosql_behavior", "local_descriptor_scan", "graph", false},
    {"SBSQL-8D3089BE1CA9", "doc_path_mutation", "DOC PATH MUTATION $.status;", "nosql.document_update", "SBLR_NOSQL_DOCUMENT_UPDATE", "EngineDocumentUpdate", "nosql_behavior", "persisted_document_update", "document_collection", true},
    {"SBSQL-9D1418D2B716", "cypher_label_list", "CYPHER LABEL LIST Person;", "nosql.graph_query", "SBLR_NOSQL_GRAPH_QUERY", "EngineGraphQuery", "nosql_behavior", "local_descriptor_scan", "graph", false},
    {"SBSQL-9D88B076B01B", "cypher_pattern", "CYPHER PATTERN n;", "nosql.graph_query", "SBLR_NOSQL_GRAPH_QUERY", "EngineGraphQuery", "nosql_behavior", "local_descriptor_scan", "graph", false},
    {"SBSQL-9E50E2ABCF21", "cypher_return_item", "CYPHER RETURN ITEM n;", "nosql.graph_query", "SBLR_NOSQL_GRAPH_QUERY", "EngineGraphQuery", "nosql_behavior", "local_descriptor_scan", "graph", false},
    {"SBSQL-AB1430CA9AFD", "doc_read_options", "DOC READ OPTIONS CONSISTENT;", "nosql.document_find", "SBLR_NOSQL_DOCUMENT_FIND", "EngineDocumentFind", "nosql_behavior", "local_descriptor_scan", "document_collection", false},
    {"SBSQL-AEC92F727FDD", "cypher_remove_item", "CYPHER REMOVE ITEM n.flag;", "nosql.graph_query", "SBLR_NOSQL_GRAPH_QUERY", "EngineGraphQuery", "nosql_behavior", "local_descriptor_scan", "graph", false},
    {"SBSQL-B22EFC23798F", "cypher_yield_item", "CYPHER YIELD ITEM name;", "nosql.graph_query", "SBLR_NOSQL_GRAPH_QUERY", "EngineGraphQuery", "nosql_behavior", "local_descriptor_scan", "graph", false},
    {"SBSQL-BD4D6DEB65AD", "fulltext_agg_def", "FULLTEXT AGG DEF terms;", "nosql.search_query", "SBLR_NOSQL_SEARCH_QUERY", "EngineSearchQuery", "nosql_behavior", "specialized_descriptor_fallback", "search_index", false},
    {"SBSQL-C583E0E27E5F", "node_pattern", "NODE PATTERN person;", "nosql.graph_query", "SBLR_NOSQL_GRAPH_QUERY", "EngineGraphQuery", "nosql_behavior", "local_descriptor_scan", "graph", false},
    {"SBSQL-C9583BC8D444", "doc_verifiable_stmt", "DOC VERIFIABLE STMT proof;", "nosql.document_find", "SBLR_NOSQL_DOCUMENT_FIND", "EngineDocumentFind", "nosql_behavior", "local_descriptor_scan", "document_collection", false},
    {"SBSQL-CC8AF838251B", "cypher_property_match", "CYPHER PROPERTY MATCH n.name;", "nosql.graph_query", "SBLR_NOSQL_GRAPH_QUERY", "EngineGraphQuery", "nosql_behavior", "local_descriptor_scan", "graph", false},
    {"SBSQL-D12B674CD242", "cypher_use_clause", "CYPHER USE CLAUSE graph;", "nosql.graph_query", "SBLR_NOSQL_GRAPH_QUERY", "EngineGraphQuery", "nosql_behavior", "local_descriptor_scan", "graph", false},
    {"SBSQL-D4A1D5000B4F", "cypher_rel_detail", "CYPHER REL DETAIL knows;", "nosql.graph_query", "SBLR_NOSQL_GRAPH_QUERY", "EngineGraphQuery", "nosql_behavior", "local_descriptor_scan", "graph", false},
    {"SBSQL-DCA33E721330", "cypher_constraint_target", "CYPHER CONSTRAINT TARGET node;", "nosql.graph_query", "SBLR_NOSQL_GRAPH_QUERY", "EngineGraphQuery", "nosql_behavior", "local_descriptor_scan", "graph", false},
    {"SBSQL-DCB7F12CB677", "doc_pipeline_verb", "DOC PIPELINE VERB PROJECT;", "nosql.document_find", "SBLR_NOSQL_DOCUMENT_FIND", "EngineDocumentFind", "nosql_behavior", "local_descriptor_scan", "document_collection", false},
    {"SBSQL-E154D66C475D", "fulltext_paging", "FULLTEXT PAGING LIMIT 10;", "nosql.search_query", "SBLR_NOSQL_SEARCH_QUERY", "EngineSearchQuery", "nosql_behavior", "specialized_descriptor_fallback", "search_index", false},
    {"SBSQL-EEC5DB9F1399", "cypher_unwind_clause", "CYPHER UNWIND CLAUSE list;", "nosql.graph_query", "SBLR_NOSQL_GRAPH_QUERY", "EngineGraphQuery", "nosql_behavior", "local_descriptor_scan", "graph", false},
    {"SBSQL-FED13166EA9C", "cypher_match_clause", "CYPHER MATCH CLAUSE n;", "nosql.graph_query", "SBLR_NOSQL_GRAPH_QUERY", "EngineGraphQuery", "nosql_behavior", "local_descriptor_scan", "graph", false},
    {"SBSQL-FEECBD281F72", "cypher_set_clause", "CYPHER SET CLAUSE n.flag;", "nosql.graph_query", "SBLR_NOSQL_GRAPH_QUERY", "EngineGraphQuery", "nosql_behavior", "local_descriptor_scan", "graph", false},
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
  session.session_uuid = "019f0000-0000-7000-8000-000000079101";
  session.connection_uuid = "019f0000-0000-7000-8000-000000079102";
  session.database_uuid = "019f0000-0000-7000-8000-000000079103";
  session.dialect_profile_uuid = "sbsql_v3";
  session.catalog_epoch = 79;
  session.security_policy_epoch = 80;
  session.descriptor_epoch = 81;
  return session;
}

ParserConfig ParserConfigForTest() {
  ParserConfig config;
  config.probe_mode = true;
  config.server_endpoint = "sb_server_sbsfc_079_multimodel_general_residual";
  config.parser_uuid = "019f0000-0000-7000-8000-000000079104";
  config.bundle_contract_id = "sbp_sbsql@sbsfc-079-multimodel-general-residual";
  config.build_id = "sbsql-sbsfc-079-multimodel-general-residual";
  return config;
}

struct PipelineArtifacts {
  CstDocument cst;
  AstDocument ast;
  BoundStatement bound;
  SblrEnvelope envelope;
  SblrVerifierResult verifier;
};

PipelineArtifacts RunPipeline(const CaseRow& row) {
  PipelineArtifacts artifacts;
  const auto session = ParserSession();
  artifacts.cst = BuildCst(std::string(row.sql));
  artifacts.ast = BuildAst(artifacts.cst);
  artifacts.bound = BindAst(artifacts.ast,
                            artifacts.cst,
                            ParserConfigForTest(),
                            session,
                            {std::string(kTargetUuid)});
  artifacts.envelope = LowerToSblr(artifacts.bound, artifacts.cst, session);
  artifacts.verifier = VerifySblrEnvelope(artifacts.envelope);
  return artifacts;
}

void RequireRegistryEvidence(const CaseRow& row) {
  const auto* registry_row = FindGeneratedSurfaceRegistryRowById(row.surface_id);
  Require(registry_row != nullptr, "SBSFC-079 generated registry row missing");
  Require(registry_row->canonical_name == row.canonical_name,
          "SBSFC-079 generated registry canonical name drifted");
  Require(registry_row->surface_kind == "grammar_production",
          "SBSFC-079 generated registry surface kind drifted");
  Require(registry_row->source_status == "native_now",
          "SBSFC-079 generated registry status drifted");
  Require(registry_row->cluster_scope == "noncluster_or_profile_scoped",
          "SBSFC-079 generated registry cluster scope drifted");
  Require(registry_row->sblr_operation_family == "sblr.general.operation.v3",
          "SBSFC-079 generated registry canonical family drifted");
}

void RequireExactLowering(const CaseRow& row, const PipelineArtifacts& artifacts) {
  if (artifacts.cst.messages.has_errors()) std::cerr << RenderMessageVectorSet(artifacts.cst.messages);
  if (artifacts.ast.messages.has_errors()) std::cerr << RenderMessageVectorSet(artifacts.ast.messages);
  if (!artifacts.bound.bound) std::cerr << RenderMessageVectorSet(artifacts.bound.messages);
  if (!artifacts.verifier.admitted) std::cerr << RenderMessageVectorSet(artifacts.verifier.messages);
  Require(!artifacts.cst.messages.has_errors(), "SBSFC-079 CST failed");
  Require(!artifacts.ast.messages.has_errors(), "SBSFC-079 AST failed");
  Require(artifacts.ast.statement_surface_id == row.surface_id,
          "SBSFC-079 AST row surface id mismatch");
  Require(artifacts.ast.statement_surface_name == row.canonical_name,
          "SBSFC-079 AST canonical name mismatch");
  Require(artifacts.ast.registry_family == "sbsql.general.operation.v3",
          "SBSFC-079 AST registry family mismatch");
  Require(artifacts.ast.operation_family == "sblr.general.operation.v3",
          "SBSFC-079 AST canonical operation family mismatch");
  Require(artifacts.bound.bound, "SBSFC-079 bind failed");
  Require(artifacts.verifier.admitted, "SBSFC-079 verifier rejected exact route");
  Require(artifacts.envelope.operation_family == "sblr.query.multimodel_or_ddl.v3",
          "SBSFC-079 route operation family mismatch");
  Require(artifacts.envelope.sblr_operation_key == "sblr.query.multimodel_or_ddl.v3",
          "SBSFC-079 route operation key mismatch");
  Require(artifacts.envelope.operation_id == row.operation_id,
          "SBSFC-079 operation id mismatch");
  Require(artifacts.envelope.engine_api_operation_id == row.operation_id,
          "SBSFC-079 engine API operation id mismatch");
  Require(artifacts.envelope.sblr_opcode == row.opcode, "SBSFC-079 opcode mismatch");
  Require(artifacts.envelope.engine_api_function == row.engine_api_function,
          "SBSFC-079 engine API function mismatch");
  Require(HasValue(artifacts.envelope.required_authority_steps,
                   "authority.parser.no_sql_text_execution"),
          "SBSFC-079 parser no-SQL-execution authority missing");
  Require(HasValue(artifacts.envelope.required_authority_steps,
                   "authority.parser.no_storage_or_finality"),
          "SBSFC-079 parser no-finality authority missing");
  Require(HasValue(artifacts.envelope.required_authority_steps,
                   "authority.cluster.provider_dispatch_not_required"),
          "SBSFC-079 cluster provider exclusion authority missing");
  Require(Contains(artifacts.envelope.payload, row.surface_id),
          "SBSFC-079 payload missing row surface id");
  Require(Contains(artifacts.envelope.payload, row.runtime_evidence_kind),
          "SBSFC-079 payload missing runtime evidence kind");
  Require(Contains(artifacts.envelope.payload, row.runtime_evidence_id),
          "SBSFC-079 payload missing runtime evidence id");
  Require(!artifacts.envelope.parser_executes_sql,
          "SBSFC-079 lowering allowed parser SQL execution");
  Require(!Contains(artifacts.envelope.payload, row.sql),
          "SBSFC-079 payload embedded source SQL text");
  Require(!Contains(artifacts.envelope.payload, "SBSQL_SURFACE_REPLAY") &&
              !Contains(artifacts.envelope.payload, "exact_refusal") &&
              !Contains(artifacts.envelope.payload, "cluster_support_not_enabled"),
          "SBSFC-079 payload used replay, refusal, or cluster-provider error evidence");
  Require(Contains(artifacts.envelope.payload, "\"cluster_provider_dispatch\":false"),
          "SBSFC-079 payload did not prove cluster provider dispatch false");
  Require(!Contains(artifacts.envelope.payload, "WAL") &&
              !Contains(artifacts.envelope.payload, "wal_recovery_authority\":true") &&
              !Contains(artifacts.envelope.payload, "recovery_authority\":true"),
          "SBSFC-079 payload carried WAL/recovery authority");

  const auto admission = scratchbird::server::AdmitServerSblrEnvelope(
      scratchbird::server::ServerSblrAdmissionRequest{artifacts.envelope.payload, false});
  Require(admission.admitted, "SBSFC-079 server admission rejected exact route");
  Require(admission.requires_public_abi_dispatch,
          "SBSFC-079 server admission did not require public ABI dispatch");
  Require(admission.operation_id == row.operation_id,
          "SBSFC-079 server admission operation id mismatch");
  Require(admission.operation_family == "sblr.query.multimodel_or_ddl.v3",
          "SBSFC-079 server admission operation family mismatch");

  const auto* opcode = sblr::LookupSblrOperation(std::string(row.operation_id));
  Require(opcode != nullptr, "SBSFC-079 opcode registry row missing");
  Require(opcode->opcode == row.opcode, "SBSFC-079 opcode registry drifted");
  Require(opcode->requires_cluster_authority == false,
          "SBSFC-079 opcode claimed cluster authority");
}

std::uint64_t CurrentUnixMillis() {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch()).count());
}

std::filesystem::path TestDatabasePath() {
  return std::filesystem::temp_directory_path() /
         ("sbsql_sbsfc_079_multimodel_general_" + std::to_string(CurrentUnixMillis()) + ".sbdb");
}

void RemoveDatabaseArtifacts(const std::filesystem::path& path) {
  std::error_code ignored;
  std::filesystem::remove(path, ignored);
  for (const auto suffix : {".sb.api_events", ".sb.crud_events", ".sb.name_events",
                            ".sb.mga_relation_metadata", ".sb.mga_relation_descriptors",
                            ".sb.mga_row_versions", ".sb.mga_index_entries",
                            ".sb.mga_savepoints", ".sb.transaction_inventory",
                            ".dirty.manifest", ".sb.owner.lock"}) {
    std::filesystem::remove(path.string() + suffix, ignored);
  }
}

std::string CreateMinimalDatabase(const std::filesystem::path& path) {
  db::DatabaseCreateConfig create;
  create.path = path.string();
  create.database_uuid =
      uuid::GenerateEngineIdentityV7(UuidKind::database, 1779810790000).value;
  create.filespace_uuid =
      uuid::GenerateEngineIdentityV7(UuidKind::filespace, 1779810790001).value;
  create.page_size = 16384;
  create.creation_unix_epoch_millis = 1779810790002;
  create.allow_minimal_resource_bootstrap = true;
  create.require_resource_seed_pack = false;
  create.allow_overwrite = true;
  const auto created = db::CreateDatabaseFile(create);
  Require(created.ok(), "SBSFC-079 database create failed");
  return uuid::UuidToString(create.database_uuid.value);
}

api::EngineRequestContext EngineContext(const std::filesystem::path& path,
                                        const std::string& database_uuid) {
  api::EngineRequestContext context;
  context.request_id = "sbsql-sbsfc-079-multimodel-general-residual";
  context.database_path = path.string();
  context.database_uuid.canonical = database_uuid;
  context.node_uuid.canonical = "019f0000-0000-7000-8000-000000079201";
  context.session_uuid.canonical = "019f0000-0000-7000-8000-000000079202";
  context.principal_uuid.canonical = "019f0000-0000-7000-8000-000000079203";
  context.statement_uuid.canonical = "019f0000-0000-7000-8000-000000079204";
  context.current_schema_uuid.canonical = "019f0000-0000-7000-8000-000000079205";
  context.security_context_present = true;
  context.catalog_generation_id = 1;
  context.security_epoch = 1;
  context.resource_epoch = 1;
  context.name_resolution_epoch = 1;
  context.trace_tags.push_back("right:MULTIMODEL_GENERAL_RESIDUAL_TEST");
  return context;
}

api::EngineRequestContext BeginEngineTransaction(const std::filesystem::path& path,
                                                 const std::string& database_uuid) {
  auto context = EngineContext(path, database_uuid);
  auto envelope = sblr::MakeSblrEnvelope("transaction.begin",
                                         "SBLR_TRANSACTION_BEGIN",
                                         "trace.sbsfc079.transaction.begin");
  envelope.requires_security_context = true;
  envelope.requires_transaction_context = false;
  envelope.contains_sql_text = false;
  const auto result = sblr::DispatchSblrOperation({context, envelope, api::EngineApiRequest{}});
  Require(result.envelope_validated, "SBSFC-079 transaction begin envelope rejected");
  Require(result.accepted, "SBSFC-079 transaction begin not accepted");
  Require(result.api_result.ok, "SBSFC-079 transaction begin failed");
  context.local_transaction_id = result.api_result.local_transaction_id;
  context.transaction_uuid = result.api_result.transaction_uuid;
  context.snapshot_visible_through_local_transaction_id = context.local_transaction_id;
  return context;
}

void PrintDispatchDiagnostics(const sblr::SblrDispatchResult& result) {
  for (const auto& diagnostic : result.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.message << '\n';
  }
  for (const auto& diagnostic : result.api_result.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.detail << '\n';
  }
}

sblr::SblrOperationEnvelope EngineEnvelope(const CaseRow& row) {
  auto envelope = sblr::MakeSblrEnvelope(std::string(row.operation_id),
                                         std::string(row.opcode),
                                         "trace.sbsfc079.multimodel_general");
  envelope.requires_security_context = true;
  envelope.requires_transaction_context = true;
  envelope.requires_cluster_authority = false;
  envelope.contains_sql_text = false;
  envelope.parser_resolved_names_to_uuids = true;
  envelope.operands.push_back({"text", "target_object_uuid", std::string(kTargetUuid)});
  envelope.operands.push_back({"text", "target_object_kind", std::string(row.target_object_kind)});
  envelope.operands.push_back({"text", "sbsfc079_surface_id", std::string(row.surface_id)});
  return envelope;
}

api::EngineApiRequest ApiRequestFor(const CaseRow& row) {
  api::EngineApiRequest request;
  request.target_object.uuid.canonical = std::string(kTargetUuid);
  request.target_object.object_kind = std::string(row.target_object_kind);
  request.localized_names.push_back({"en", "primary", "", std::string(row.canonical_name), true});
  request.option_envelopes.push_back(std::string("sbsfc079_surface_id:") + std::string(row.surface_id));
  request.option_envelopes.push_back(std::string("surface_kind:") + std::string(row.canonical_name));
  request.option_envelopes.push_back("parser_executes_sql:false");
  return request;
}

void RequireEngineDispatch(const api::EngineRequestContext& context, const CaseRow& row) {
  const auto result = sblr::DispatchSblrOperation({context, EngineEnvelope(row), ApiRequestFor(row)});
  PrintDispatchDiagnostics(result);
  Require(result.envelope_validated, "SBSFC-079 engine envelope rejected");
  Require(result.accepted, "SBSFC-079 engine dispatch did not accept route");
  Require(result.dispatched_to_api, "SBSFC-079 engine did not dispatch to API");
  Require(result.api_result.operation_id == row.operation_id,
          "SBSFC-079 runtime operation id drifted");
  Require(result.api_result.ok, "SBSFC-079 runtime API did not complete");
  Require(HasEvidence(result.api_result, row.runtime_evidence_kind, row.runtime_evidence_id),
          "SBSFC-079 runtime evidence missing");
  Require(HasEvidence(result.api_result, "cluster_provider_dispatch", "false"),
          "SBSFC-079 runtime claimed cluster provider dispatch");
  Require(HasEvidence(result.api_result, "parser_executes_sql", "false"),
          "SBSFC-079 runtime allowed parser SQL execution");
  Require(HasEvidence(result.api_result, "wal_recovery_authority", "false"),
          "SBSFC-079 runtime carried WAL/recovery authority");
}

}  // namespace

int main() {
  static_assert(sizeof(kCases) / sizeof(kCases[0]) == 50);
  for (const auto& row : kCases) {
    RequireRegistryEvidence(row);
    RequireExactLowering(row, RunPipeline(row));
  }

  const auto path = TestDatabasePath();
  RemoveDatabaseArtifacts(path);
  const auto database_uuid = CreateMinimalDatabase(path);
  const auto context = BeginEngineTransaction(path, database_uuid);
  for (const auto& row : kCases) {
    RequireEngineDispatch(context, row);
  }
  RemoveDatabaseArtifacts(path);

  std::cout << "sbsql_sbsfc_079_multimodel_general_residual_exact_route_conformance=passed\n";
  return EXIT_SUCCESS;
}

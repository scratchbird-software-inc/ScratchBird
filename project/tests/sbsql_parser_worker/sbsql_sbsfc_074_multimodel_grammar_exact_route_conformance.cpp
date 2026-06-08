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

constexpr std::string_view kTargetUuid = "019f0000-0000-7000-8000-000000074001";

struct GrammarRow {
  std::string_view surface_id;
  std::string_view canonical_name;
};

constexpr GrammarRow kRows[] = {
    {"SBSQL-0754AA11F0F2", "graph_pattern"},
    {"SBSQL-07D7A3903706", "kv_hash_verb"},
    {"SBSQL-0FCC33E4D785", "graph_constraint_stmt"},
    {"SBSQL-107A755454C8", "kv_geo_verb"},
    {"SBSQL-1652C483571E", "graph_traversal_stmt"},
    {"SBSQL-1961DE9339F1", "opensearch_mapping_clause"},
    {"SBSQL-2178E91249AB", "timeseries_clause"},
    {"SBSQL-287F16013E79", "kv_verifiable_op"},
    {"SBSQL-2F3129D519B2", "time_series_op_stmt"},
    {"SBSQL-2F6045F8F5F2", "fulltext_search_body"},
    {"SBSQL-32A1DEAB05A0", "kv_stream_blob_op"},
    {"SBSQL-33A29F15AA8A", "kv_reference_op"},
    {"SBSQL-33DA03C79DF2", "kv_geo_op"},
    {"SBSQL-4C133DC91E03", "kv_iter_op"},
    {"SBSQL-4C62BAB2476B", "kv_sorted_set_op"},
    {"SBSQL-4E61D43F9BB6", "kv_hash_op"},
    {"SBSQL-52BB2D269DD1", "kv_value_clause"},
    {"SBSQL-54CB03E3346C", "kv_legacy_op"},
    {"SBSQL-5BAB6D8DDFFF", "cte_search_clause"},
    {"SBSQL-60227B8E9964", "kv_modifier"},
    {"SBSQL-6D1F0E5B0CB7", "graph_pattern_named"},
    {"SBSQL-6F29C4B955DE", "document_field"},
    {"SBSQL-70714D771561", "change_stream_options"},
    {"SBSQL-77833871F12E", "kv_set_op"},
    {"SBSQL-77E8E05E5CFD", "timeseries_option"},
    {"SBSQL-8564858075D8", "document_op_stmt"},
    {"SBSQL-86C00D52D838", "graph_node_def"},
    {"SBSQL-8BF2E92FC180", "kv_atomic_op"},
    {"SBSQL-8E06FA7352B1", "kv_hyperloglog_op"},
    {"SBSQL-9618AE139D74", "graph_op_stmt"},
    {"SBSQL-A6B1FB14927E", "graph_path_expr"},
    {"SBSQL-ACA7CDB46BEE", "document_collection_clause"},
    {"SBSQL-B7EBE41498AD", "graph_name"},
    {"SBSQL-BDC0BC347EAE", "kv_admin_op"},
    {"SBSQL-C8C89D4BB273", "kv_versionstamp_op"},
    {"SBSQL-C968039FC515", "kv_atomic_verb"},
    {"SBSQL-CF608F8411B5", "graph_edge_def"},
    {"SBSQL-D1766BD7AF1B", "document_path_expr"},
    {"SBSQL-D97C1EA0B5FC", "kv_list_verb"},
    {"SBSQL-DFED6F273B2E", "kv_bitmap_verb"},
    {"SBSQL-E4D8E62EB41E", "kv_zset_verb"},
    {"SBSQL-E4F3A349B8CE", "kv_ttl_op"},
    {"SBSQL-E5363707B058", "kv_string_op"},
    {"SBSQL-E9688436FF59", "change_stream_stmt"},
    {"SBSQL-EA468C08F372", "kv_set_verb"},
    {"SBSQL-EC6DC4CA9DE6", "kv_list_op"},
    {"SBSQL-EFAC7C699F99", "kv_string_verb"},
    {"SBSQL-F09967132630", "kv_bitmap_op"},
    {"SBSQL-FC4CD6147DB2", "kv_range_op"},
};

struct MultiModelCase {
  std::string_view sql;
  std::string_view operation_id;
  std::string_view opcode;
  std::string_view api;
  std::vector<std::string_view> surface_ids;
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
  session.session_uuid = "019f0000-0000-7000-8000-000000074101";
  session.connection_uuid = "019f0000-0000-7000-8000-000000074102";
  session.database_uuid = "019f0000-0000-7000-8000-000000074103";
  session.dialect_profile_uuid = "sbsql_v3";
  session.catalog_epoch = 274;
  session.security_policy_epoch = 275;
  session.descriptor_epoch = 276;
  return session;
}

ParserConfig ParserConfigForTest() {
  ParserConfig config;
  config.probe_mode = true;
  config.server_endpoint = "sb_server_sbsfc_074_multimodel_route";
  config.parser_uuid = "019f0000-0000-7000-8000-000000074104";
  config.bundle_contract_id = "sbp_sbsql@sbsfc-074-multimodel-route-test";
  config.build_id = "sbsql-sbsfc-074-multimodel-route-test";
  return config;
}

struct PipelineArtifacts {
  CstDocument cst;
  AstDocument ast;
  BoundStatement bound;
  SblrEnvelope envelope;
  SblrVerifierResult verifier;
};

PipelineArtifacts RunPipeline(const MultiModelCase& test_case) {
  PipelineArtifacts artifacts;
  const auto session = ParserSession();
  artifacts.cst = BuildCst(std::string(test_case.sql));
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

void RequireRegistryEvidence() {
  for (const auto& row : kRows) {
    const auto* registry_row = FindGeneratedSurfaceRegistryRowById(row.surface_id);
    Require(registry_row != nullptr, "SBSFC-074 generated registry row missing");
    Require(registry_row->canonical_name == row.canonical_name,
            "SBSFC-074 generated registry canonical name drifted");
    Require(registry_row->surface_kind == "grammar_production",
            "SBSFC-074 generated registry kind drifted");
    Require(registry_row->source_status == "native_now",
            "SBSFC-074 generated registry status drifted");
    Require(registry_row->cluster_scope == "noncluster_or_profile_scoped",
            "SBSFC-074 generated registry cluster scope drifted");
    Require(registry_row->sblr_operation_family == "sblr.query.multimodel_or_ddl.v3",
            "SBSFC-074 generated registry SBLR family drifted");
  }
}

void RequireExactLowering(const MultiModelCase& test_case,
                          const PipelineArtifacts& artifacts) {
  if (artifacts.cst.messages.has_errors()) std::cerr << RenderMessageVectorSet(artifacts.cst.messages);
  if (artifacts.ast.messages.has_errors()) std::cerr << RenderMessageVectorSet(artifacts.ast.messages);
  if (!artifacts.bound.bound) std::cerr << RenderMessageVectorSet(artifacts.bound.messages);
  if (!artifacts.verifier.admitted) std::cerr << RenderMessageVectorSet(artifacts.verifier.messages);
  Require(!artifacts.cst.messages.has_errors(), "SBSFC-074 CST failed");
  Require(!artifacts.ast.messages.has_errors(), "SBSFC-074 AST failed");
  Require(artifacts.bound.bound, "SBSFC-074 bind failed");
  Require(artifacts.verifier.admitted, "SBSFC-074 verifier rejected exact route");
  Require(artifacts.envelope.operation_id == test_case.operation_id,
          "SBSFC-074 operation id mismatch");
  Require(artifacts.envelope.sblr_opcode == test_case.opcode,
          "SBSFC-074 SBLR opcode mismatch");
  Require(artifacts.envelope.operation_family == "sblr.query.multimodel_or_ddl.v3",
          "SBSFC-074 operation family mismatch");
  Require(HasValue(artifacts.envelope.required_authority_steps,
                   "authority.engine.nosql_multimodel_api_required"),
          "SBSFC-074 missing engine NoSQL authority");
  Require(HasValue(artifacts.envelope.required_authority_steps,
                   "authority.parser.no_sql_text_execution"),
          "SBSFC-074 missing parser no-SQL-execution authority");
  Require(HasValue(artifacts.envelope.required_authority_steps,
                   "authority.parser.no_storage_or_finality"),
          "SBSFC-074 missing parser no-finality authority");
  Require(!artifacts.envelope.parser_executes_sql,
          "SBSFC-074 lowering allowed parser SQL execution");
  Require(Contains(artifacts.envelope.payload, "\"sql_text_included\":false"),
          "SBSFC-074 payload did not prove no SQL text authority");
  Require(!Contains(artifacts.envelope.payload, test_case.sql),
          "SBSFC-074 payload embedded source SQL text");
  Require(!Contains(artifacts.envelope.payload, "SBSQL_SURFACE_REPLAY") &&
              !Contains(artifacts.envelope.payload, "replay") &&
              !Contains(artifacts.envelope.payload, "refusal"),
          "SBSFC-074 payload used forbidden replay/refusal evidence");
  Require(!Contains(artifacts.envelope.payload, "WAL") &&
              !Contains(artifacts.envelope.payload, "wal") &&
              !Contains(artifacts.envelope.payload, "recovery"),
          "SBSFC-074 payload carried WAL/recovery authority");
  for (const auto surface_id : test_case.surface_ids) {
    Require(Contains(artifacts.envelope.payload, surface_id),
            std::string("SBSFC-074 payload missing row marker ") +
                std::string(surface_id));
  }
}

void RequireServerAdmission(const MultiModelCase& test_case,
                            const SblrEnvelope& envelope) {
  const auto admission = scratchbird::server::AdmitServerSblrEnvelope(
      scratchbird::server::ServerSblrAdmissionRequest{envelope.payload, false});
  Require(admission.admitted, "server admission rejected SBSFC-074 exact route");
  Require(admission.requires_public_abi_dispatch,
          "server admission did not require public ABI dispatch for SBSFC-074");
  Require(admission.operation_id == test_case.operation_id,
          "server admission SBSFC-074 operation id mismatch");
}

std::uint64_t CurrentUnixMillis() {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());
}

std::filesystem::path TestDatabasePath() {
  return std::filesystem::temp_directory_path() /
         ("sbsql_sbsfc_074_multimodel_exact_route_" +
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
      uuid::GenerateEngineIdentityV7(UuidKind::database, 1779810742000).value;
  create.filespace_uuid =
      uuid::GenerateEngineIdentityV7(UuidKind::filespace, 1779810742001).value;
  create.page_size = 16384;
  create.creation_unix_epoch_millis = 1779810742002;
  create.allow_minimal_resource_bootstrap = true;
  create.require_resource_seed_pack = false;
  create.allow_overwrite = true;
  const auto created = db::CreateDatabaseFile(create);
  Require(created.ok(), "SBSFC-074 test database create failed");
  return uuid::UuidToString(create.database_uuid.value);
}

api::EngineRequestContext EngineContextForDatabase(const std::filesystem::path& path,
                                                   const std::string& database_uuid) {
  api::EngineRequestContext context;
  context.request_id = "sbsql-sbsfc-074-multimodel-exact-route";
  context.database_path = path.string();
  context.database_uuid.canonical = database_uuid;
  context.node_uuid.canonical = "019f0000-0000-7000-8000-000000074202";
  context.session_uuid.canonical = "019f0000-0000-7000-8000-000000074203";
  context.principal_uuid.canonical = "019f0000-0000-7000-8000-000000074204";
  context.statement_uuid.canonical = "019f0000-0000-7000-8000-000000074206";
  context.current_schema_uuid.canonical = "019f0000-0000-7000-8000-000000074207";
  context.current_role_uuid.canonical = "019f0000-0000-7000-8000-000000074208";
  context.security_context_present = true;
  context.catalog_generation_id = 1;
  context.security_epoch = 1;
  context.resource_epoch = 1;
  context.name_resolution_epoch = 1;
  context.trace_tags.push_back("right:MULTIMODEL_SURFACE_TEST");
  for (const auto& row : kRows) {
    context.trace_tags.push_back(std::string("sbsql_surface_id:") +
                                 std::string(row.surface_id));
  }
  return context;
}

api::EngineRequestContext BeginEngineTransaction(const std::filesystem::path& path,
                                                 const std::string& database_uuid) {
  auto context = EngineContextForDatabase(path, database_uuid);
  auto envelope = sblr::MakeSblrEnvelope("transaction.begin",
                                         "SBLR_TRANSACTION_BEGIN",
                                         "trace.sbsfc074.multimodel.transaction.begin");
  envelope.requires_security_context = true;
  envelope.requires_transaction_context = false;
  envelope.contains_sql_text = false;
  const sblr::SblrDispatchRequest request{context, envelope, api::EngineApiRequest{}};
  const auto result = sblr::DispatchSblrOperation(request);
  Require(result.envelope_validated, "SBSFC-074 transaction begin envelope invalid");
  Require(result.accepted, "SBSFC-074 transaction begin dispatch rejected");
  Require(result.api_result.ok, "SBSFC-074 transaction begin failed");
  context.local_transaction_id = result.api_result.local_transaction_id;
  context.transaction_uuid = result.api_result.transaction_uuid;
  context.snapshot_visible_through_local_transaction_id = context.local_transaction_id;
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
  envelope.operands.push_back({"text", "target_object_uuid", std::string(kTargetUuid)});
  envelope.operands.push_back({"text", "target_object_kind", "multimodel_target"});
  return envelope;
}

api::EngineApiRequest ApiRequestFor(std::string_view name, std::string_view option) {
  api::EngineApiRequest request;
  request.target_object.uuid.canonical = std::string(kTargetUuid);
  request.target_object.object_kind = "multimodel_target";
  request.localized_names.push_back({"en", "primary", "", std::string(name), true});
  request.option_envelopes.push_back(std::string(option));
  return request;
}

void RequireNoSqlDispatches(const api::EngineRequestContext& context) {
  struct DispatchCase {
    std::string_view operation_id;
    std::string_view opcode;
    std::string_view api_name;
    std::string_view evidence_kind;
    std::string_view evidence_id;
  };
  const DispatchCase cases[] = {
      {"nosql.graph_query", "SBLR_NOSQL_GRAPH_QUERY", "EngineGraphQuery", "graph_query", "local_descriptor_scan"},
      {"nosql.document_find", "SBLR_NOSQL_DOCUMENT_FIND", "EngineDocumentFind", "document_find", "0"},
      {"nosql.key_value_get", "SBLR_NOSQL_KEY_VALUE_GET", "EngineKeyValueGet", "kv_physical_access", "exact_key_index_probe"},
      {"nosql.key_value_put", "SBLR_NOSQL_KEY_VALUE_PUT", "EngineKeyValuePut", "api_behavior_event", "nosql.key_value_put"},
      {"nosql.time_series_append", "SBLR_NOSQL_TIME_SERIES_APPEND", "EngineTimeSeriesAppend", "api_behavior_event", "nosql.time_series_append"},
      {"nosql.search_query", "SBLR_NOSQL_SEARCH_QUERY", "EngineSearchQuery", "search_query", "full_text_descriptor_query"},
  };
  for (const auto& row : cases) {
    const sblr::SblrDispatchRequest request{
        context,
        EngineEnvelope(row.operation_id,
                       row.opcode,
                       std::string("trace.sbsfc074.multimodel.") + std::string(row.operation_id)),
        ApiRequestFor(row.api_name, "sbsfc074_multimodel")};
    const auto result = sblr::DispatchSblrOperation(request);
    Require(result.envelope_validated, "SBSFC-074 NoSQL envelope invalid");
    Require(result.accepted, "SBSFC-074 NoSQL dispatch rejected");
    Require(result.dispatched_to_api, "SBSFC-074 NoSQL route did not dispatch to API");
    Require(result.api_result.ok, "SBSFC-074 NoSQL API returned failure");
    Require(HasEvidence(result.api_result, row.evidence_kind, row.evidence_id),
            "SBSFC-074 NoSQL runtime evidence missing");
  }
}

}  // namespace

int main() {
  RequireRegistryEvidence();
  const std::vector<MultiModelCase> cases = {
      {"GRAPH social PATTERN NAMED friend_path NODE person EDGE knows PATH p CONSTRAINT unique_node TRAVERSE FROM person SEARCH BREADTH FIRST;",
       "nosql.graph_query",
       "SBLR_NOSQL_GRAPH_QUERY",
       "EngineGraphQuery",
       {"SBSQL-9618AE139D74", "SBSQL-B7EBE41498AD", "SBSQL-0754AA11F0F2",
        "SBSQL-6D1F0E5B0CB7", "SBSQL-86C00D52D838", "SBSQL-CF608F8411B5",
        "SBSQL-A6B1FB14927E", "SBSQL-0FCC33E4D785", "SBSQL-1652C483571E",
        "SBSQL-5BAB6D8DDFFF"}},
      {"DOCUMENT orders FIELD status PATH $.status IN COLLECTION docs;",
       "nosql.document_find",
       "SBLR_NOSQL_DOCUMENT_FIND",
       "EngineDocumentFind",
       {"SBSQL-8564858075D8", "SBSQL-ACA7CDB46BEE", "SBSQL-6F29C4B955DE",
        "SBSQL-D1766BD7AF1B"}},
      {"CHANGE STREAM orders WITH RESUME AFTER 'token';",
       "nosql.document_find",
       "SBLR_NOSQL_DOCUMENT_FIND",
       "EngineDocumentFind",
       {"SBSQL-E9688436FF59", "SBSQL-70714D771561"}},
      {"FULLTEXT docs MATCH 'native search' WITH HIGHLIGHT;",
       "nosql.search_query",
       "SBLR_NOSQL_SEARCH_QUERY",
       "EngineSearchQuery",
       {"SBSQL-2F6045F8F5F2"}},
      {"OPENSEARCH docs MAPPING WITH FIELD title TEXT;",
       "nosql.search_query",
       "SBLR_NOSQL_SEARCH_QUERY",
       "EngineSearchQuery",
       {"SBSQL-1961DE9339F1"}},
      {"TIMESERIES cpu APPEND VALUE 42 WITH BUCKET 60 RETENTION 3600;",
       "nosql.time_series_append",
       "SBLR_NOSQL_TIME_SERIES_APPEND",
       "EngineTimeSeriesAppend",
       {"SBSQL-2F3129D519B2", "SBSQL-2178E91249AB", "SBSQL-77E8E05E5CFD"}},
      {"KV STRING SET user:1 VALUE 'ada' WITH NX;",
       "nosql.key_value_put",
       "SBLR_NOSQL_KEY_VALUE_PUT",
       "EngineKeyValuePut",
       {"SBSQL-E5363707B058", "SBSQL-EFAC7C699F99", "SBSQL-52BB2D269DD1",
        "SBSQL-60227B8E9964"}},
      {"KV LIST LPUSH queue VALUE 'a';",
       "nosql.key_value_put",
       "SBLR_NOSQL_KEY_VALUE_PUT",
       "EngineKeyValuePut",
       {"SBSQL-EC6DC4CA9DE6", "SBSQL-D97C1EA0B5FC", "SBSQL-52BB2D269DD1"}},
      {"KV SET SADD tags VALUE 'blue';",
       "nosql.key_value_put",
       "SBLR_NOSQL_KEY_VALUE_PUT",
       "EngineKeyValuePut",
       {"SBSQL-77833871F12E", "SBSQL-EA468C08F372", "SBSQL-52BB2D269DD1"}},
      {"KV HASH HSET profile VALUE 'name=ada';",
       "nosql.key_value_put",
       "SBLR_NOSQL_KEY_VALUE_PUT",
       "EngineKeyValuePut",
       {"SBSQL-4E61D43F9BB6", "SBSQL-07D7A3903706", "SBSQL-52BB2D269DD1"}},
      {"KV ZSET ZADD ranks VALUE 'ada:1';",
       "nosql.key_value_put",
       "SBLR_NOSQL_KEY_VALUE_PUT",
       "EngineKeyValuePut",
       {"SBSQL-4C62BAB2476B", "SBSQL-E4D8E62EB41E", "SBSQL-52BB2D269DD1"}},
      {"KV BITMAP SETBIT flags VALUE 1;",
       "nosql.key_value_put",
       "SBLR_NOSQL_KEY_VALUE_PUT",
       "EngineKeyValuePut",
       {"SBSQL-F09967132630", "SBSQL-DFED6F273B2E", "SBSQL-52BB2D269DD1"}},
      {"KV HLL PFADD seen VALUE 'u1';",
       "nosql.key_value_put",
       "SBLR_NOSQL_KEY_VALUE_PUT",
       "EngineKeyValuePut",
       {"SBSQL-8E06FA7352B1", "SBSQL-52BB2D269DD1"}},
      {"KV TTL EXPIRE user:1 WITH EX 60;",
       "nosql.key_value_put",
       "SBLR_NOSQL_KEY_VALUE_PUT",
       "EngineKeyValuePut",
       {"SBSQL-E4F3A349B8CE", "SBSQL-60227B8E9964"}},
      {"KV ATOMIC INCR counter VALUE 1;",
       "nosql.key_value_put",
       "SBLR_NOSQL_KEY_VALUE_PUT",
       "EngineKeyValuePut",
       {"SBSQL-8BF2E92FC180", "SBSQL-C968039FC515", "SBSQL-52BB2D269DD1"}},
      {"KV VERSIONSTAMP GET version;",
       "nosql.key_value_put",
       "SBLR_NOSQL_KEY_VALUE_PUT",
       "EngineKeyValuePut",
       {"SBSQL-C8C89D4BB273"}},
      {"KV GEO ADD places VALUE '1,2';",
       "nosql.key_value_put",
       "SBLR_NOSQL_KEY_VALUE_PUT",
       "EngineKeyValuePut",
       {"SBSQL-33DA03C79DF2", "SBSQL-107A755454C8", "SBSQL-52BB2D269DD1"}},
      {"KV STREAM APPEND audit VALUE 'blob';",
       "nosql.key_value_put",
       "SBLR_NOSQL_KEY_VALUE_PUT",
       "EngineKeyValuePut",
       {"SBSQL-32A1DEAB05A0", "SBSQL-52BB2D269DD1"}},
      {"KV LEGACY GET oldkey;",
       "nosql.key_value_put",
       "SBLR_NOSQL_KEY_VALUE_PUT",
       "EngineKeyValuePut",
       {"SBSQL-54CB03E3346C"}},
      {"KV ADMIN COMPACT namespace;",
       "nosql.key_value_put",
       "SBLR_NOSQL_KEY_VALUE_PUT",
       "EngineKeyValuePut",
       {"SBSQL-BDC0BC347EAE"}},
      {"KV RANGE users FROM a TO z;",
       "nosql.key_value_get",
       "SBLR_NOSQL_KEY_VALUE_GET",
       "EngineKeyValueGet",
       {"SBSQL-FC4CD6147DB2", "SBSQL-52BB2D269DD1"}},
      {"KV ITER users PREFIX user;",
       "nosql.key_value_get",
       "SBLR_NOSQL_KEY_VALUE_GET",
       "EngineKeyValueGet",
       {"SBSQL-4C133DC91E03", "SBSQL-52BB2D269DD1"}},
      {"KV REFERENCE GET refkey;",
       "nosql.key_value_get",
       "SBLR_NOSQL_KEY_VALUE_GET",
       "EngineKeyValueGet",
       {"SBSQL-33A29F15AA8A", "SBSQL-52BB2D269DD1"}},
      {"KV VERIFIABLE PROOF user:1;",
       "nosql.key_value_put",
       "SBLR_NOSQL_KEY_VALUE_PUT",
       "EngineKeyValuePut",
       {"SBSQL-287F16013E79", "SBSQL-52BB2D269DD1"}},
  };
  for (const auto& test_case : cases) {
    const auto artifacts = RunPipeline(test_case);
    RequireExactLowering(test_case, artifacts);
    RequireServerAdmission(test_case, artifacts.envelope);
  }

  const auto path = TestDatabasePath();
  RemoveDatabaseArtifacts(path);
  const auto database_uuid = CreateMinimalDatabase(path);
  const auto context = BeginEngineTransaction(path, database_uuid);
  RequireNoSqlDispatches(context);
  RemoveDatabaseArtifacts(path);

  std::cout << "sbsql_sbsfc_074_multimodel_grammar_exact_route_conformance=passed\n";
  return EXIT_SUCCESS;
}

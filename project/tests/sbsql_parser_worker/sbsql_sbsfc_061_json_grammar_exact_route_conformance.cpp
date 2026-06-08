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
#include "lowering/lowering.hpp"
#include "rendering/rendering.hpp"
#include "registry/generated/sbsql_generated_registry.hpp"
#include "sblr_admission.hpp"
#include "sblr_dispatch.hpp"
#include "sblr_engine_envelope.hpp"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace scratchbird::parser::sbsql;
namespace api = scratchbird::engine::internal_api;
namespace sblr = scratchbird::engine::sblr;

constexpr std::string_view kOperationId = "query.evaluate_projection";
constexpr std::string_view kOpcode = "SBLR_QUERY_EVALUATE_PROJECTION";
constexpr std::string_view kRouteFamily = "sblr.query.relational.v3";

struct GrammarRow {
  std::string_view surface_id;
  std::string_view canonical_name;
  std::string_view canonical_sblr_operation_family;
};

constexpr GrammarRow kRows[] = {
    {"SBSQL-08CD0B33AAE5", "json_passing_clause", "sblr.general.operation.v3"},
    {"SBSQL-18B3E6720DDA", "json_get_expr", "sblr.general.operation.v3"},
    {"SBSQL-247E72F74164", "json_wrapper_clause", "sblr.general.operation.v3"},
    {"SBSQL-288EFD5075B2", "json_kv_pair", "sblr.query.multimodel_or_ddl.v3"},
    {"SBSQL-4146FCAB1413", "json_predicate_modifier", "sblr.general.operation.v3"},
    {"SBSQL-547EB20F2F61", "json_query_form", "sblr.query.relational.v3"},
    {"SBSQL-58634A438204", "json_exists_form", "sblr.general.operation.v3"},
    {"SBSQL-59407DC640A0", "json_on_error_clause", "sblr.general.operation.v3"},
    {"SBSQL-5F143056E7C8", "json_quotes_clause", "sblr.general.operation.v3"},
    {"SBSQL-B48FA9BE99B6", "json_value", "sblr.general.operation.v3"},
    {"SBSQL-C58C9A48AB7D", "json_on_empty_clause", "sblr.general.operation.v3"},
    {"SBSQL-DC03B51C015C", "json_value_form", "sblr.general.operation.v3"},
    {"SBSQL-F933EEDD8711", "json_get_op", "sblr.general.operation.v3"},
};

struct FunctionArg {
  std::string name;
  std::string type;
  std::string value;
};

struct JsonCase {
  std::vector<std::string_view> surface_ids;
  std::string_view sql;
  std::string_view projection_name;
  std::string_view expression_kind;
  std::string_view function_id;
  std::string_view operator_id;
  std::string_view canonical_operator_id;
  std::string_view expected_type;
  std::string_view expected_value;
  std::vector<FunctionArg> args;
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

std::string EscapeJson(std::string_view value) {
  std::string escaped;
  for (const char ch : value) {
    switch (ch) {
      case '\\':
        escaped += "\\\\";
        break;
      case '"':
        escaped += "\\\"";
        break;
      default:
        escaped.push_back(ch);
        break;
    }
  }
  return escaped;
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
  session.session_uuid = "019f0000-0000-7000-8000-000000061101";
  session.connection_uuid = "019f0000-0000-7000-8000-000000061102";
  session.database_uuid = "019f0000-0000-7000-8000-000000061103";
  session.dialect_profile_uuid = "sbsql_v3";
  session.catalog_epoch = 261;
  session.security_policy_epoch = 262;
  session.descriptor_epoch = 263;
  return session;
}

ParserConfig ParserConfigForTest() {
  ParserConfig config;
  config.probe_mode = true;
  config.server_endpoint = "sb_server_sbsfc_061_json_grammar_route";
  config.parser_uuid = "019f0000-0000-7000-8000-000000061104";
  config.bundle_contract_id = "sbp_sbsql@sbsfc-061-json-grammar-route-test";
  config.build_id = "sbsql-sbsfc-061-json-grammar-route-test";
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
  artifacts.cst = BuildCst(std::string(sql));
  artifacts.ast = BuildAst(artifacts.cst);
  artifacts.bound = BindAst(artifacts.ast, artifacts.cst, ParserConfigForTest(), session);
  artifacts.envelope = LowerToSblr(artifacts.bound, artifacts.cst, session);
  artifacts.verifier = VerifySblrEnvelope(artifacts.envelope);
  return artifacts;
}

void RequireRegistryEvidence() {
  for (const auto& row : kRows) {
    const auto* registry_row = FindGeneratedSurfaceRegistryRowById(row.surface_id);
    Require(registry_row != nullptr, "SBSFC-061 generated registry row missing");
    Require(registry_row->canonical_name == row.canonical_name,
            "SBSFC-061 generated registry canonical name drifted");
    Require(registry_row->surface_kind == "grammar_production",
            "SBSFC-061 generated registry kind drifted");
    Require(registry_row->source_status == "native_now",
            "SBSFC-061 generated registry status drifted");
    Require(registry_row->cluster_scope == "noncluster_or_profile_scoped",
            "SBSFC-061 generated registry cluster scope drifted");
    Require(registry_row->sblr_operation_family == row.canonical_sblr_operation_family,
            "SBSFC-061 generated registry SBLR family drifted");
  }
}

void RequireExactLowering(const JsonCase& test_case,
                          const PipelineArtifacts& artifacts) {
  Require(!artifacts.cst.messages.has_errors(), "SBSFC-061 CST failed");
  Require(!artifacts.ast.messages.has_errors(), "SBSFC-061 AST failed");
  Require(artifacts.bound.bound, "SBSFC-061 bind failed");
  if (!artifacts.verifier.admitted) {
    std::cerr << RenderMessageVectorSet(artifacts.verifier.messages);
  }
  Require(artifacts.verifier.admitted, "SBSFC-061 verifier rejected exact route");
  Require(artifacts.envelope.operation_family == kRouteFamily,
          "SBSFC-061 operation family mismatch");
  Require(artifacts.envelope.operation_id == kOperationId,
          "SBSFC-061 operation id mismatch");
  Require(artifacts.envelope.sblr_opcode == kOpcode,
          "SBSFC-061 SBLR opcode mismatch");
  Require(HasValue(artifacts.envelope.required_authority_steps,
                   "authority.parser.no_sql_text_execution"),
          "SBSFC-061 parser no-SQL-execution authority step missing");
  Require(!artifacts.envelope.parser_executes_sql,
          "SBSFC-061 lowering allowed parser SQL execution");
  Require(Contains(artifacts.envelope.payload, "\"projection_0_syntax_surface_ids\""),
          "SBSFC-061 payload missing projection syntax surface list");
  for (const auto surface_id : test_case.surface_ids) {
    Require(Contains(artifacts.envelope.payload, surface_id),
            std::string("SBSFC-061 payload missing JSON grammar row marker ") +
                std::string(surface_id));
  }
  Require(Contains(artifacts.envelope.payload,
                   std::string("\"projection_0_expr_kind\":\"") +
                       std::string(test_case.expression_kind) + "\""),
          "SBSFC-061 payload expression kind mismatch");
  if (!test_case.function_id.empty()) {
    Require(Contains(artifacts.envelope.payload, test_case.function_id),
            "SBSFC-061 payload missing JSON function id");
  }
  if (!test_case.operator_id.empty()) {
    Require(Contains(artifacts.envelope.payload, test_case.operator_id),
            "SBSFC-061 payload missing JSON operator id");
  }
  Require(Contains(artifacts.envelope.payload,
                   std::string("\"projection_0_name\":\"") +
                       std::string(test_case.projection_name) + "\""),
          "SBSFC-061 payload missing projection alias");
  Require(Contains(artifacts.envelope.payload, "\"sql_text_included\":false"),
          "SBSFC-061 payload did not prove no SQL text authority");
  Require(!Contains(artifacts.envelope.payload, test_case.sql),
          "SBSFC-061 payload embedded source SQL text");
  Require(!Contains(artifacts.envelope.payload, "replay") &&
              !Contains(artifacts.envelope.payload, "refusal"),
          "SBSFC-061 payload used a forbidden non-e2e route");
  Require(!Contains(artifacts.envelope.payload, "WAL") &&
              !Contains(artifacts.envelope.payload, "wal") &&
              !Contains(artifacts.envelope.payload, "recovery"),
          "SBSFC-061 payload carried WAL/recovery authority");
}

void RequireServerAdmission(const SblrEnvelope& envelope) {
  const auto admission = scratchbird::server::AdmitServerSblrEnvelope(
      scratchbird::server::ServerSblrAdmissionRequest{envelope.payload, false});
  Require(admission.admitted, "server admission rejected SBSFC-061 exact route");
  Require(admission.requires_public_abi_dispatch,
          "server admission did not require public ABI dispatch for SBSFC-061");
  Require(admission.operation_id == kOperationId,
          "server admission SBSFC-061 operation id mismatch");
}

api::EngineRequestContext EngineContext() {
  api::EngineRequestContext context;
  context.request_id = "sbsql-sbsfc-061-json-grammar-exact-route";
  context.database_uuid.canonical = "019f0000-0000-7000-8000-000000061201";
  context.node_uuid.canonical = "019f0000-0000-7000-8000-000000061202";
  context.session_uuid.canonical = "019f0000-0000-7000-8000-000000061203";
  context.principal_uuid.canonical = "019f0000-0000-7000-8000-000000061204";
  context.transaction_uuid.canonical = "019f0000-0000-7000-8000-000000061205";
  context.statement_uuid.canonical = "019f0000-0000-7000-8000-000000061206";
  context.current_schema_uuid.canonical = "019f0000-0000-7000-8000-000000061207";
  context.current_role_uuid.canonical = "019f0000-0000-7000-8000-000000061208";
  context.local_transaction_id = 261;
  context.security_context_present = true;
  context.trace_tags.push_back("right:QUERY_PROJECTION_TEST");
  for (const auto& row : kRows) {
    context.trace_tags.push_back(std::string("sbsql_surface_id:") +
                                 std::string(row.surface_id));
  }
  return context;
}

void AppendLiteralProjectionExpression(sblr::SblrOperationEnvelope& envelope,
                                       std::string_view prefix,
                                       std::string_view type,
                                       std::string_view value) {
  envelope.operands.push_back({"text", std::string(prefix) + "expr_kind", "literal"});
  envelope.operands.push_back({"text", std::string(prefix) + "type", std::string(type)});
  envelope.operands.push_back({"text", std::string(prefix) + "value", std::string(value)});
  envelope.operands.push_back({"text", std::string(prefix) + "is_null", "false"});
}

sblr::SblrOperationEnvelope EngineEnvelope(const JsonCase& test_case) {
  auto envelope = sblr::MakeSblrEnvelope(std::string(kOperationId),
                                         std::string(kOpcode),
                                         "trace.sbsfc061.json_grammar.exact_route");
  envelope.requires_security_context = true;
  envelope.requires_transaction_context = true;
  envelope.requires_cluster_authority = false;
  envelope.contains_sql_text = false;
  envelope.parser_resolved_names_to_uuids = true;
  envelope.operands.push_back({"text", "projection_count", "1"});
  envelope.operands.push_back({"text", "projection_0_name", std::string(test_case.projection_name)});
  envelope.operands.push_back({"text", "projection_0_expr_kind", std::string(test_case.expression_kind)});
  envelope.operands.push_back({"text", "projection_0_type", std::string(test_case.expected_type)});
  envelope.operands.push_back({"text", "projection_0_value", ""});
  envelope.operands.push_back({"text", "projection_0_is_null", "false"});
  if (test_case.expression_kind == "function") {
    envelope.operands.push_back({"text", "projection_0_function_id", std::string(test_case.function_id)});
    envelope.operands.push_back({"text", "projection_0_function_arg_count",
                                 std::to_string(test_case.args.size())});
    for (std::size_t index = 0; index < test_case.args.size(); ++index) {
      const auto& arg = test_case.args[index];
      const auto prefix = "projection_0_arg_" + std::to_string(index) + "_";
      envelope.operands.push_back({"text", prefix + "name", arg.name});
      AppendLiteralProjectionExpression(envelope, prefix, arg.type, arg.value);
    }
  } else {
    envelope.operands.push_back({"text", "projection_0_operator_id", std::string(test_case.operator_id)});
    envelope.operands.push_back({"text", "projection_0_canonical_operator_id",
                                 std::string(test_case.canonical_operator_id)});
    envelope.operands.push_back({"text", "projection_0_operator_arg_count",
                                 std::to_string(test_case.args.size())});
    for (std::size_t index = 0; index < test_case.args.size(); ++index) {
      const auto& arg = test_case.args[index];
      AppendLiteralProjectionExpression(envelope,
                                        "projection_0_arg_" + std::to_string(index) + "_",
                                        arg.type,
                                        arg.value);
    }
  }
  return envelope;
}

void RequireEngineDispatch(const JsonCase& test_case) {
  const sblr::SblrDispatchRequest request{
      EngineContext(),
      EngineEnvelope(test_case),
      api::EngineApiRequest{}};
  const auto result = sblr::DispatchSblrOperation(request);
  for (const auto& diagnostic : result.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.message << '\n';
  }
  for (const auto& diagnostic : result.api_result.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.detail << '\n';
  }
  Require(result.envelope_validated, "SBSFC-061 engine SBLR envelope did not validate");
  Require(result.accepted, "SBSFC-061 engine SBLR dispatch did not accept projection");
  Require(result.dispatched_to_api, "SBSFC-061 engine SBLR dispatch did not route to internal API");
  Require(result.api_result.ok, "EngineEvaluateProjection did not return SBSFC-061 success");
  Require(result.api_result.result_shape.rows.size() == 1,
          "SBSFC-061 result row count mismatch");
  const auto& row = result.api_result.result_shape.rows.front();
  Require(row.fields.size() == 1, "SBSFC-061 result field count mismatch");
  const auto& field = row.fields.front();
  Require(field.first == test_case.projection_name, "SBSFC-061 result field name mismatch");
  Require(field.second.descriptor.canonical_type_name == test_case.expected_type,
          "SBSFC-061 result descriptor mismatch");
  Require(!field.second.is_null, "SBSFC-061 result unexpectedly null");
  Require(field.second.encoded_value == test_case.expected_value,
          "SBSFC-061 result value mismatch");
  if (!test_case.function_id.empty()) {
    Require(HasEvidence(result.api_result, "function_runtime", test_case.function_id),
            "SBSFC-061 result missing function runtime evidence");
  }
  Require(HasEvidence(result.api_result,
                      "query_projection",
                      "constant_projection_engine_evaluated"),
          "SBSFC-061 result missing query projection runtime evidence");
}

}  // namespace

int main() {
  RequireRegistryEvidence();
  const std::vector<JsonCase> cases = {
      {{"SBSQL-B48FA9BE99B6", "SBSQL-DC03B51C015C", "SBSQL-08CD0B33AAE5",
        "SBSQL-C58C9A48AB7D", "SBSQL-59407DC640A0", "SBSQL-5F143056E7C8"},
       "SELECT JSON_VALUE('{\"item\":\"bird\"}', '$.item' PASSING 'ignored' AS extra RETURNING character OMIT QUOTES NULL ON EMPTY NULL ON ERROR) AS json_value_result;",
       "json_value_result",
       "function",
       "sb.json.value",
       "",
       "",
       "character",
       "bird",
       {{"document", "json_document", "{\"item\":\"bird\"}"},
        {"path", "text", "$.item"},
        {"extra", "text", "ignored"},
        {"returning", "text", "character"},
        {"quotes", "text", "omit"},
        {"on_empty", "text", "null"},
        {"on_error", "text", "null"}}},
      {{"SBSQL-547EB20F2F61", "SBSQL-247E72F74164", "SBSQL-5F143056E7C8"},
       "SELECT JSON_QUERY('{\"item\":{\"name\":\"bird\"}}', '$.item' WITH WRAPPER KEEP QUOTES) AS json_query_result;",
       "json_query_result",
       "function",
       "sb.json.query",
       "",
       "",
       "json_document",
       "[{\"name\":\"bird\"}]",
       {{"document", "json_document", "{\"item\":{\"name\":\"bird\"}}"},
        {"path", "text", "$.item"},
        {"wrapper", "text", "with_wrapper"},
        {"quotes", "text", "keep"}}},
      {{"SBSQL-58634A438204", "SBSQL-08CD0B33AAE5", "SBSQL-4146FCAB1413",
        "SBSQL-59407DC640A0"},
       "SELECT JSON_EXISTS('{\"item\":\"bird\"}', '$.item' PASSING 'bird' AS expected TRUE ON ERROR) AS json_exists_result;",
       "json_exists_result",
       "function",
       "sb.json.exists",
       "",
       "",
       "boolean",
       "1",
       {{"document", "json_document", "{\"item\":\"bird\"}"},
        {"path", "text", "$.item"},
        {"expected", "text", "bird"},
        {"predicate_modifier", "text", "true"}}},
      {{"SBSQL-288EFD5075B2"},
       "SELECT JSON_OBJECT('item' VALUE 'bird') AS json_object_result;",
       "json_object_result",
       "function",
       "sb.json.object",
       "",
       "",
       "json_document",
       "{\"item\":\"bird\"}",
       {{"key", "text", "item"}, {"value", "text", "bird"}}},
      {{"SBSQL-18B3E6720DDA", "SBSQL-F933EEDD8711"},
       "SELECT '{\"item\":\"bird\"}' -> '$.item' AS json_get_result;",
       "json_get_result",
       "operator",
       "",
       "op_json_get",
       "sb.operator.json_get",
       "json_document",
       "\"bird\"",
       {{"document", "json_document", "{\"item\":\"bird\"}"}, {"path", "text", "$.item"}}},
  };
  for (const auto& test_case : cases) {
    const auto artifacts = RunPipeline(test_case.sql);
    RequireExactLowering(test_case, artifacts);
    RequireServerAdmission(artifacts.envelope);
    RequireEngineDispatch(test_case);
  }
  std::cout << "sbsql_sbsfc_061_json_grammar_exact_route_conformance=passed\n";
  return EXIT_SUCCESS;
}

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
    {"SBSQL-04294772B704", "xml_forest_element", "sblr.general.operation.v3"},
    {"SBSQL-0D763364A741", "xml_root_form", "sblr.general.operation.v3"},
    {"SBSQL-1EB5AFA89C92", "xml_text_form", "sblr.general.operation.v3"},
    {"SBSQL-377E419D39E7", "xml_query_arg", "sblr.query.relational.v3"},
    {"SBSQL-472218EF5162", "xml_serialize_form", "sblr.general.operation.v3"},
    {"SBSQL-47746E67E9A9", "xml_cast_form", "sblr.general.operation.v3"},
    {"SBSQL-4F01326C8EDC", "xml_attributes_form", "sblr.general.operation.v3"},
    {"SBSQL-5C277CC8C23B", "xml_namespace_decl", "sblr.general.operation.v3"},
    {"SBSQL-5F6F3888603D", "xml_pi_form", "sblr.general.operation.v3"},
    {"SBSQL-7814247BD85C", "xml_query_form", "sblr.query.relational.v3"},
    {"SBSQL-79275B05E28B", "xml_forest_form", "sblr.general.operation.v3"},
    {"SBSQL-7D729966B640", "xml_concat_form", "sblr.general.operation.v3"},
    {"SBSQL-9421E36B42D7", "xml_name", "sblr.general.operation.v3"},
    {"SBSQL-9B179F0E2530", "xml_parse_form", "sblr.general.operation.v3"},
    {"SBSQL-A679E455207F", "xml_special_form", "sblr.general.operation.v3"},
    {"SBSQL-B3CBDBCA5C65", "xml_namespaces_form", "sblr.general.operation.v3"},
    {"SBSQL-B547F4BBBB33", "xml_document_form", "sblr.query.multimodel_or_ddl.v3"},
    {"SBSQL-B7E217B24869", "xml_exists_form", "sblr.general.operation.v3"},
    {"SBSQL-C072208F4207", "xml_element_form", "sblr.general.operation.v3"},
    {"SBSQL-D6C4289D4CB2", "xml_validate_form", "sblr.general.operation.v3"},
    {"SBSQL-F16F6CCCDF7C", "xml_agg_form", "sblr.general.operation.v3"},
    {"SBSQL-F9798A94FC50", "xml_attribute_def", "sblr.general.operation.v3"},
    {"SBSQL-FD154EAC4FA6", "xml_comment_form", "sblr.catalog.mutation.v3"},
};

struct FunctionArg {
  std::string name;
  std::string type;
  std::string value;
};

struct FunctionCase {
  std::vector<std::string_view> surface_ids;
  std::string_view sql;
  std::string_view projection_name;
  std::string_view function_id;
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
      case '\n':
        escaped += "\\n";
        break;
      case '\r':
        escaped += "\\r";
        break;
      case '\t':
        escaped += "\\t";
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
  session.session_uuid = "019f0000-0000-7000-8000-000000060101";
  session.connection_uuid = "019f0000-0000-7000-8000-000000060102";
  session.database_uuid = "019f0000-0000-7000-8000-000000060103";
  session.dialect_profile_uuid = "sbsql_v3";
  session.catalog_epoch = 260;
  session.security_policy_epoch = 261;
  session.descriptor_epoch = 262;
  return session;
}

ParserConfig ParserConfigForTest() {
  ParserConfig config;
  config.probe_mode = true;
  config.server_endpoint = "sb_server_sbsfc_060r_xml_grammar_route";
  config.parser_uuid = "019f0000-0000-7000-8000-000000060104";
  config.bundle_contract_id = "sbp_sbsql@sbsfc-060r-xml-grammar-route-test";
  config.build_id = "sbsql-sbsfc-060r-xml-grammar-route-test";
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
    Require(registry_row != nullptr, "SBSFC-060R generated registry row missing");
    Require(registry_row->canonical_name == row.canonical_name,
            "SBSFC-060R generated registry canonical name drifted");
    Require(registry_row->surface_kind == "grammar_production",
            "SBSFC-060R generated registry kind drifted");
    Require(registry_row->source_status == "native_now",
            "SBSFC-060R generated registry status drifted");
    Require(registry_row->cluster_scope == "noncluster_or_profile_scoped",
            "SBSFC-060R generated registry cluster scope drifted");
    Require(registry_row->sblr_operation_family == row.canonical_sblr_operation_family,
            "SBSFC-060R generated registry SBLR family drifted");
  }
}

void RequireExactLowering(const FunctionCase& test_case,
                          const PipelineArtifacts& artifacts) {
  Require(!artifacts.cst.messages.has_errors(), "SBSFC-060R CST failed");
  Require(!artifacts.ast.messages.has_errors(), "SBSFC-060R AST failed");
  Require(artifacts.bound.bound, "SBSFC-060R bind failed");
  if (!artifacts.verifier.admitted) {
    std::cerr << RenderMessageVectorSet(artifacts.verifier.messages);
  }
  Require(artifacts.verifier.admitted, "SBSFC-060R verifier rejected exact route");
  Require(artifacts.envelope.operation_family == kRouteFamily,
          "SBSFC-060R operation family mismatch");
  Require(artifacts.envelope.operation_id == kOperationId,
          "SBSFC-060R operation id mismatch");
  Require(artifacts.envelope.sblr_opcode == kOpcode,
          "SBSFC-060R SBLR opcode mismatch");
  Require(HasValue(artifacts.envelope.required_authority_steps,
                   "authority.parser.no_sql_text_execution"),
          "SBSFC-060R parser no-SQL-execution authority step missing");
  Require(!artifacts.envelope.parser_executes_sql,
          "SBSFC-060R lowering allowed parser SQL execution");
  Require(Contains(artifacts.envelope.payload, "\"projection_0_syntax_surface_ids\""),
          "SBSFC-060R payload missing projection syntax surface list");
  for (const auto surface_id : test_case.surface_ids) {
    Require(Contains(artifacts.envelope.payload, surface_id),
            std::string("SBSFC-060R payload missing target grammar row marker for ") +
                std::string(test_case.projection_name) + " surface=" +
                std::string(surface_id));
  }
  Require(Contains(artifacts.envelope.payload, test_case.function_id),
          "SBSFC-060R payload missing XML function id");
  Require(Contains(artifacts.envelope.payload,
                   std::string("\"projection_0_name\":\"") +
                       std::string(test_case.projection_name) + "\""),
          "SBSFC-060R payload missing projection alias");
  Require(Contains(artifacts.envelope.payload, "\"sql_text_included\":false"),
          "SBSFC-060R payload did not prove no SQL text authority");
  Require(Contains(artifacts.envelope.payload,
                   std::string("\"projection_0_function_arg_count\":\"") +
                       std::to_string(test_case.args.size()) + "\""),
          "SBSFC-060R payload missing XML argument count");
  for (std::size_t index = 0; index < test_case.args.size(); ++index) {
    const auto& arg = test_case.args[index];
    const auto prefix = std::string("\"projection_0_arg_") +
                        std::to_string(index) + "_";
    Require(Contains(artifacts.envelope.payload, prefix + "type\":\"" + EscapeJson(arg.type) + "\""),
            "SBSFC-060R payload missing XML argument type");
    Require(Contains(artifacts.envelope.payload, prefix + "value\":\"" + EscapeJson(arg.value) + "\""),
            "SBSFC-060R payload missing XML argument value");
    if (!arg.name.empty() && arg.name.rfind("arg", 0) != 0) {
      Require(Contains(artifacts.envelope.payload,
                       prefix + "name\":\"" + EscapeJson(arg.name) + "\""),
              std::string("SBSFC-060R payload missing XML argument name for ") +
                  std::string(test_case.projection_name) + " arg=" + arg.name);
    }
  }
  Require(!Contains(artifacts.envelope.payload, test_case.sql),
          "SBSFC-060R payload embedded source SQL text");
  Require(!Contains(artifacts.envelope.payload, "replay") &&
              !Contains(artifacts.envelope.payload, "refusal"),
          "SBSFC-060R payload used a forbidden non-e2e route");
  Require(!Contains(artifacts.envelope.payload, "WAL") &&
              !Contains(artifacts.envelope.payload, "wal") &&
              !Contains(artifacts.envelope.payload, "recovery"),
          "SBSFC-060R payload carried WAL/recovery authority");
}

void RequireServerAdmission(const SblrEnvelope& envelope) {
  const auto admission = scratchbird::server::AdmitServerSblrEnvelope(
      scratchbird::server::ServerSblrAdmissionRequest{envelope.payload, false});
  Require(admission.admitted, "server admission rejected SBSFC-060R exact route");
  Require(admission.requires_public_abi_dispatch,
          "server admission did not require public ABI dispatch for SBSFC-060R");
  Require(admission.operation_id == kOperationId,
          "server admission SBSFC-060R operation id mismatch");
}

api::EngineRequestContext EngineContext() {
  api::EngineRequestContext context;
  context.request_id = "sbsql-sbsfc-060r-xml-grammar-exact-route";
  context.database_uuid.canonical = "019f0000-0000-7000-8000-000000060201";
  context.node_uuid.canonical = "019f0000-0000-7000-8000-000000060202";
  context.session_uuid.canonical = "019f0000-0000-7000-8000-000000060203";
  context.principal_uuid.canonical = "019f0000-0000-7000-8000-000000060204";
  context.transaction_uuid.canonical = "019f0000-0000-7000-8000-000000060205";
  context.statement_uuid.canonical = "019f0000-0000-7000-8000-000000060206";
  context.current_schema_uuid.canonical = "019f0000-0000-7000-8000-000000060207";
  context.current_role_uuid.canonical = "019f0000-0000-7000-8000-000000060208";
  context.local_transaction_id = 260;
  context.security_context_present = true;
  context.trace_tags.push_back("right:QUERY_PROJECTION_TEST");
  for (const auto& row : kRows) {
    context.trace_tags.push_back(std::string("sbsql_surface_id:") +
                                 std::string(row.surface_id));
  }
  return context;
}

sblr::SblrOperationEnvelope EngineEnvelope(const FunctionCase& test_case) {
  auto envelope = sblr::MakeSblrEnvelope(std::string(kOperationId),
                                         std::string(kOpcode),
                                         "trace.sbsfc060r.xml_grammar.exact_route");
  envelope.requires_security_context = true;
  envelope.requires_transaction_context = true;
  envelope.requires_cluster_authority = false;
  envelope.contains_sql_text = false;
  envelope.parser_resolved_names_to_uuids = true;
  envelope.operands.push_back({"text", "projection_count", "1"});
  envelope.operands.push_back({"text", "projection_0_name", std::string(test_case.projection_name)});
  envelope.operands.push_back({"text", "projection_0_expr_kind", "function"});
  envelope.operands.push_back({"text", "projection_0_function_id", std::string(test_case.function_id)});
  envelope.operands.push_back({"text", "projection_0_function_arg_count",
                               std::to_string(test_case.args.size())});
  for (std::size_t index = 0; index < test_case.args.size(); ++index) {
    const auto& arg = test_case.args[index];
    const auto prefix = "projection_0_arg_" + std::to_string(index) + "_";
    envelope.operands.push_back({"text", prefix + "name", arg.name});
    envelope.operands.push_back({"text", prefix + "type", arg.type});
    envelope.operands.push_back({"text", prefix + "value", arg.value});
    envelope.operands.push_back({"text", prefix + "is_null", "false"});
  }
  return envelope;
}

void RequireEngineDispatch(const FunctionCase& test_case) {
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
  Require(result.envelope_validated, "SBSFC-060R engine SBLR envelope did not validate");
  Require(result.accepted, "SBSFC-060R engine SBLR dispatch did not accept projection");
  Require(result.dispatched_to_api, "SBSFC-060R engine SBLR dispatch did not route to internal API");
  Require(result.api_result.ok, "EngineEvaluateProjection did not return SBSFC-060R success");
  Require(result.api_result.result_shape.rows.size() == 1,
          "SBSFC-060R result row count mismatch");
  const auto& row = result.api_result.result_shape.rows.front();
  Require(row.fields.size() == 1, "SBSFC-060R result field count mismatch");
  const auto& field = row.fields.front();
  Require(field.first == test_case.projection_name, "SBSFC-060R result field name mismatch");
  Require(field.second.descriptor.canonical_type_name == test_case.expected_type,
          "SBSFC-060R result descriptor mismatch");
  Require(!field.second.is_null, "SBSFC-060R result unexpectedly null");
  Require(field.second.encoded_value == test_case.expected_value,
          "SBSFC-060R result value mismatch");
  Require(HasEvidence(result.api_result, "function_runtime", test_case.function_id),
          "SBSFC-060R result missing function runtime evidence");
  Require(HasEvidence(result.api_result,
                      "query_projection",
                      "constant_projection_engine_evaluated"),
          "SBSFC-060R result missing query projection runtime evidence");
}

}  // namespace

int main() {
  RequireRegistryEvidence();
  const std::vector<FunctionCase> cases = {
      {{"SBSQL-04294772B704"},
       "SELECT XMLFOREST('bird' AS item) AS xmlforest_result;",
       "xmlforest_result",
       "sb.xml.forest",
       "xml_document",
       "<item>bird</item>",
       {{"item", "text", "bird"}}},
      {{"SBSQL-0D763364A741"},
       "SELECT XMLROOT('<r/>', VERSION '1.1', STANDALONE yes) AS xmlroot_result;",
       "xmlroot_result",
       "sb.xml.root",
       "xml_document",
       "<?xml version=\"1.1\" standalone=\"yes\"?><r/>",
       {{"arg0", "xml_document", "<r/>"},
        {"version", "text", "1.1"},
        {"standalone", "text", "yes"}}},
      {{"SBSQL-1EB5AFA89C92"},
       "SELECT XMLTEXT('a & b') AS xmltext_result;",
       "xmltext_result",
       "sb.xml.text",
       "xml",
       "a &amp; b",
       {{"arg0", "text", "a & b"}}},
      {{"SBSQL-377E419D39E7", "SBSQL-7814247BD85C"},
       "SELECT XMLQUERY('item' PASSING '<root><item>bird</item></root>' AS doc) AS xmlquery_result;",
       "xmlquery_result",
       "sb.xml.query",
       "xml_document",
       "<item>bird</item>",
       {{"query", "text", "item"},
        {"doc", "xml_document", "<root><item>bird</item></root>"}}},
      {{"SBSQL-472218EF5162"},
       "SELECT XMLSERIALIZE(CONTENT '<r>bird</r>' AS character) AS xmlserialize_result;",
       "xmlserialize_result",
       "sb.xml.serialize",
       "character",
       "<r>bird</r>",
       {{"mode", "text", "CONTENT"},
        {"arg1", "xml_document", "<r>bird</r>"},
        {"type", "text", "character"}}},
      {{"SBSQL-47746E67E9A9"},
       "SELECT XMLCAST('<r>bird</r>' AS xml) AS xmlcast_result;",
       "xmlcast_result",
       "sb.xml.cast",
       "xml_document",
       "<r>bird</r>",
       {{"arg0", "text", "<r>bird</r>"},
        {"arg1", "text", "xml"}}},
      {{"SBSQL-4F01326C8EDC", "SBSQL-F9798A94FC50"},
       "SELECT XMLATTRIBUTES('blue' AS color) AS xmlattributes_result;",
       "xmlattributes_result",
       "sb.xml.attributes",
       "xml",
       " color=\"blue\"",
       {{"color", "text", "blue"}}},
      {{"SBSQL-5C277CC8C23B", "SBSQL-B3CBDBCA5C65"},
       "SELECT XMLNAMESPACES(DEFAULT 'urn:default', 'urn:item' AS item) AS xmlnamespaces_result;",
       "xmlnamespaces_result",
       "sb.xml.namespaces",
       "xml",
       " xmlns=\"urn:default\" xmlns:item=\"urn:item\"",
       {{"prefix", "text", "default"},
        {"uri", "text", "urn:default"},
        {"prefix", "text", "item"},
        {"uri", "text", "urn:item"}}},
      {{"SBSQL-5F6F3888603D"},
       "SELECT XMLPI(NAME target, 'href=\"bird\"') AS xmlpi_result;",
       "xmlpi_result",
       "sb.xml.pi",
       "xml",
       "<?target href=\"bird\"?>",
       {{"target", "text", "target"},
        {"content", "text", "href=\"bird\""}}},
      {{"SBSQL-79275B05E28B"},
       "SELECT XMLFOREST('bird' AS item) AS xmlforest_form_result;",
       "xmlforest_form_result",
       "sb.xml.forest",
       "xml_document",
       "<item>bird</item>",
       {{"item", "text", "bird"}}},
      {{"SBSQL-7D729966B640"},
       "SELECT XMLCONCAT('<a/>', '<b/>') AS xmlconcat_result;",
       "xmlconcat_result",
       "sb.xml.concat",
       "xml_document",
       "<a/><b/>",
       {{"arg0", "xml_document", "<a/>"},
        {"arg1", "xml_document", "<b/>"}}},
      {{"SBSQL-9421E36B42D7", "SBSQL-C072208F4207"},
       "SELECT XMLELEMENT(NAME item, 'bird') AS xmlelement_result;",
       "xmlelement_result",
       "sb.xml.element",
       "xml_document",
       "<item>bird</item>",
       {{"name", "text", "item"},
        {"arg1", "text", "bird"}}},
      {{"SBSQL-9B179F0E2530", "SBSQL-A679E455207F"},
       "SELECT XMLPARSE(DOCUMENT '<r/>') AS xmlparse_result;",
       "xmlparse_result",
       "sb.xml.parse",
       "xml_document",
       "<r/>",
       {{"mode", "text", "DOCUMENT"},
        {"arg1", "xml_document", "<r/>"}}},
      {{"SBSQL-B547F4BBBB33"},
       "SELECT XMLDOCUMENT('<r/>') AS xmldocument_result;",
       "xmldocument_result",
       "sb.xml.document",
       "xml_document",
       "<r/>",
       {{"arg0", "xml_document", "<r/>"}}},
      {{"SBSQL-B7E217B24869"},
       "SELECT XMLEXISTS('item', '<root><item>bird</item></root>') AS xmlexists_result;",
       "xmlexists_result",
       "sb.xml.exists",
       "boolean",
       "1",
       {{"arg0", "text", "item"},
        {"arg1", "xml_document", "<root><item>bird</item></root>"}}},
      {{"SBSQL-D6C4289D4CB2"},
       "SELECT XMLVALIDATE(DOCUMENT '<r/>') AS xmlvalidate_result;",
       "xmlvalidate_result",
       "sb.xml.validate",
       "xml_document",
       "<r/>",
       {{"mode", "text", "DOCUMENT"},
        {"arg1", "xml_document", "<r/>"}}},
      {{"SBSQL-F16F6CCCDF7C"},
       "SELECT XMLAGG('<a/>', '<b/>') AS xmlagg_result;",
       "xmlagg_result",
       "sb.xml.agg",
       "xml_document",
       "<a/><b/>",
       {{"arg0", "xml_document", "<a/>"},
        {"arg1", "xml_document", "<b/>"}}},
      {{"SBSQL-FD154EAC4FA6"},
       "SELECT XMLCOMMENT('bird') AS xmlcomment_result;",
       "xmlcomment_result",
       "sb.xml.comment",
       "xml",
       "<!--bird-->",
       {{"arg0", "text", "bird"}}},
  };
  for (const auto& test_case : cases) {
    const auto artifacts = RunPipeline(test_case.sql);
    RequireExactLowering(test_case, artifacts);
    RequireServerAdmission(artifacts.envelope);
    RequireEngineDispatch(test_case);
  }
  std::cout << "sbsql_sbsfc_060r_xml_grammar_exact_route_conformance=passed\n";
  return EXIT_SUCCESS;
}

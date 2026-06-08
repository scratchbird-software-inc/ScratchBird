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
#include "registry/generated/sbsql_generated_registry.hpp"
#include "rendering/rendering.hpp"
#include "sblr_admission.hpp"
#include "sblr_dispatch.hpp"
#include "sblr_engine_envelope.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>
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
    {"SBSQL-4B0E85EDA340", "psql_simple_case_stmt", "sblr.general.operation.v3"},
    {"SBSQL-A2EAC47B5A98", "psql_searched_case_stmt", "sblr.query.multimodel_or_ddl.v3"},
    {"SBSQL-ED8343236128", "psql_case_stmt", "sblr.general.operation.v3"},
};

struct CaseRoute {
  std::string_view sql;
  std::string_view projection_name;
  std::vector<std::string_view> surface_ids;
  std::vector<std::string_view> payload_markers;
};

struct PipelineArtifacts {
  CstDocument cst;
  AstDocument ast;
  BoundStatement bound;
  SblrEnvelope envelope;
  SblrVerifierResult verifier;
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

std::size_t FindJsonStringEnd(std::string_view text, std::size_t start) {
  bool escaped = false;
  for (std::size_t index = start; index < text.size(); ++index) {
    const char ch = text[index];
    if (escaped) {
      escaped = false;
      continue;
    }
    if (ch == '\\') {
      escaped = true;
      continue;
    }
    if (ch == '"') return index;
  }
  return std::string_view::npos;
}

std::string UnescapeJsonString(std::string_view text) {
  std::string out;
  out.reserve(text.size());
  bool escaped = false;
  for (const char ch : text) {
    if (escaped) {
      switch (ch) {
        case 'n': out.push_back('\n'); break;
        case 'r': out.push_back('\r'); break;
        case 't': out.push_back('\t'); break;
        case '\\': out.push_back('\\'); break;
        case '"': out.push_back('"'); break;
        default: out.push_back(ch); break;
      }
      escaped = false;
      continue;
    }
    if (ch == '\\') {
      escaped = true;
      continue;
    }
    out.push_back(ch);
  }
  if (escaped) out.push_back('\\');
  return out;
}

sblr::SblrOperationEnvelope EngineEnvelopeFromParserEnvelope(
    const SblrEnvelope& parser_envelope) {
  auto envelope = sblr::MakeSblrEnvelope(
      parser_envelope.engine_api_operation_id.empty() ? parser_envelope.operation_id
                                                      : parser_envelope.engine_api_operation_id,
      parser_envelope.sblr_opcode,
      parser_envelope.trace_key);
  envelope.contains_sql_text = false;
  envelope.parser_resolved_names_to_uuids = true;
  envelope.requires_security_context = true;
  envelope.requires_transaction_context = true;
  envelope.requires_cluster_authority = false;

  std::size_t index = 0;
  while (index < parser_envelope.payload.size()) {
    const std::size_t key_start_quote = parser_envelope.payload.find('"', index);
    if (key_start_quote == std::string_view::npos) break;
    const std::size_t key_end_quote =
        FindJsonStringEnd(parser_envelope.payload, key_start_quote + 1);
    if (key_end_quote == std::string_view::npos) break;
    std::size_t cursor = key_end_quote + 1;
    while (cursor < parser_envelope.payload.size() &&
           std::isspace(static_cast<unsigned char>(parser_envelope.payload[cursor]))) {
      ++cursor;
    }
    if (cursor >= parser_envelope.payload.size() || parser_envelope.payload[cursor] != ':') {
      index = key_end_quote + 1;
      continue;
    }
    ++cursor;
    while (cursor < parser_envelope.payload.size() &&
           std::isspace(static_cast<unsigned char>(parser_envelope.payload[cursor]))) {
      ++cursor;
    }
    if (cursor >= parser_envelope.payload.size() || parser_envelope.payload[cursor] != '"') {
      index = cursor + 1;
      continue;
    }
    const std::size_t value_end_quote =
        FindJsonStringEnd(parser_envelope.payload, cursor + 1);
    if (value_end_quote == std::string_view::npos) break;
    envelope.operands.push_back(
        {"text",
         UnescapeJsonString(parser_envelope.payload.substr(
             key_start_quote + 1, key_end_quote - key_start_quote - 1)),
         UnescapeJsonString(parser_envelope.payload.substr(
             cursor + 1, value_end_quote - cursor - 1))});
    index = value_end_quote + 1;
  }
  return envelope;
}

bool HasEvidence(const api::EngineApiResult& result,
                 std::string_view kind,
                 std::string_view id) {
  for (const auto& evidence : result.evidence) {
    if (evidence.evidence_kind == kind && evidence.evidence_id == id) return true;
  }
  return false;
}

std::string FieldValue(const api::EngineApiResult& result, std::string_view field) {
  if (result.result_shape.rows.empty()) return {};
  for (const auto& [name, value] : result.result_shape.rows.front().fields) {
    if (name == field) return value.encoded_value;
  }
  return {};
}

void PrintMessages(const MessageVectorSet& messages) {
  for (const auto& diagnostic : messages.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.message << '\n';
  }
}

SessionContext ParserSession() {
  SessionContext session;
  session.authenticated = true;
  session.session_uuid = "019f0000-0000-7000-8000-000000066101";
  session.connection_uuid = "019f0000-0000-7000-8000-000000066102";
  session.database_uuid = "019f0000-0000-7000-8000-000000066103";
  session.dialect_profile_uuid = "sbsql_v3";
  session.catalog_epoch = 566;
  session.security_policy_epoch = 567;
  session.descriptor_epoch = 568;
  return session;
}

ParserConfig ParserConfigForTest() {
  ParserConfig config;
  config.probe_mode = true;
  config.server_endpoint = "sb_server_sbsfc_066_case_grammar_route";
  config.parser_uuid = "019f0000-0000-7000-8000-000000066104";
  config.bundle_contract_id = "sbp_sbsql@sbsfc-066-case-grammar-route-test";
  config.build_id = "sbsql-sbsfc-066-case-grammar-route-test";
  return config;
}

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
    Require(registry_row != nullptr, "SBSFC-066 generated registry row missing");
    Require(registry_row->canonical_name == row.canonical_name,
            "SBSFC-066 generated registry canonical name drifted");
    Require(registry_row->surface_kind == "grammar_production",
            "SBSFC-066 generated registry kind drifted");
    Require(registry_row->source_status == "native_now",
            "SBSFC-066 generated registry status drifted");
    Require(registry_row->cluster_scope == "noncluster_or_profile_scoped",
            "SBSFC-066 generated registry cluster scope drifted");
    Require(registry_row->sblr_operation_family == row.canonical_sblr_operation_family,
            "SBSFC-066 generated registry SBLR family drifted");
  }
}

void RequireExactLowering(const CaseRoute& test_case,
                          const PipelineArtifacts& artifacts) {
  PrintMessages(artifacts.cst.messages);
  PrintMessages(artifacts.ast.messages);
  PrintMessages(artifacts.bound.messages);
  PrintMessages(artifacts.envelope.messages);
  PrintMessages(artifacts.verifier.messages);
  Require(!artifacts.cst.messages.has_errors(), "SBSFC-066 CST failed");
  Require(!artifacts.ast.messages.has_errors(), "SBSFC-066 AST failed");
  Require(artifacts.bound.bound, "SBSFC-066 bind failed");
  Require(artifacts.verifier.admitted, "SBSFC-066 verifier rejected exact route");
  Require(artifacts.envelope.operation_family == kRouteFamily,
          "SBSFC-066 operation family mismatch");
  Require(artifacts.envelope.operation_id == kOperationId,
          "SBSFC-066 operation id mismatch");
  Require(artifacts.envelope.sblr_opcode == kOpcode,
          "SBSFC-066 SBLR opcode mismatch");
  Require(HasValue(artifacts.envelope.required_authority_steps,
                   "authority.parser.no_sql_text_execution"),
          "SBSFC-066 no-SQL-execution authority missing");
  Require(HasValue(artifacts.envelope.required_authority_steps,
                   "authority.parser.no_storage_or_finality"),
          "SBSFC-066 no-storage-finality authority missing");
  Require(!artifacts.envelope.parser_executes_sql,
          "SBSFC-066 lowering allowed parser SQL execution");
  Require(Contains(artifacts.envelope.payload, "\"query_envelope_kind\":\"scalar_projection\""),
          "SBSFC-066 scalar projection route missing");
  Require(Contains(artifacts.envelope.payload,
                   "\"projection_0_special_form_id\":\"sb.special.case\""),
          "SBSFC-066 CASE special form missing");
  Require(Contains(artifacts.envelope.payload, "\"sql_text_included\":false"),
          "SBSFC-066 payload did not prove no SQL text authority");
  Require(Contains(artifacts.envelope.payload,
                   std::string("\"projection_0_name\":\"") +
                       std::string(test_case.projection_name) + "\""),
          "SBSFC-066 projection alias missing");
  Require(!Contains(artifacts.envelope.payload, test_case.sql),
          "SBSFC-066 payload embedded source SQL text");
  Require(!Contains(artifacts.envelope.payload, "SBSQL_SURFACE_REPLAY") &&
              !Contains(artifacts.envelope.payload, "refusal"),
          "SBSFC-066 payload used forbidden replay/refusal evidence");
  Require(!Contains(artifacts.envelope.payload, "WAL") &&
              !Contains(artifacts.envelope.payload, "wal") &&
              !Contains(artifacts.envelope.payload, "recovery"),
          "SBSFC-066 payload carried WAL/recovery authority");
  for (const auto surface_id : test_case.surface_ids) {
    Require(Contains(artifacts.envelope.payload, surface_id),
            std::string("SBSFC-066 payload missing row marker ") +
                std::string(surface_id));
  }
  for (const auto marker : test_case.payload_markers) {
    Require(Contains(artifacts.envelope.payload, marker),
            std::string("SBSFC-066 payload missing marker ") + std::string(marker));
  }
}

void RequireServerAdmission(const SblrEnvelope& envelope) {
  const auto admission = scratchbird::server::AdmitServerSblrEnvelope(
      scratchbird::server::ServerSblrAdmissionRequest{envelope.payload, false});
  Require(admission.admitted, "server admission rejected SBSFC-066 exact route");
  Require(admission.requires_public_abi_dispatch,
          "server admission did not require public ABI dispatch for SBSFC-066");
  Require(admission.operation_id == kOperationId,
          "server admission SBSFC-066 operation id mismatch");
}

api::EngineRequestContext EngineContext();

void RequireParsedEnvelopeRuntime(std::string_view sql,
                                  std::string_view projection_name,
                                  std::string_view expected_value,
                                  std::string_view condition_op) {
  const auto artifacts = RunPipeline(sql);
  RequireExactLowering(CaseRoute{sql,
                                 projection_name,
                                 {"SBSQL-ED8343236128"},
                                 {"\"projection_0_special_form_id\":\"sb.special.case\""}},
                       artifacts);
  RequireServerAdmission(artifacts.envelope);

  const sblr::SblrDispatchRequest request{
      EngineContext(),
      EngineEnvelopeFromParserEnvelope(artifacts.envelope),
      api::EngineApiRequest{}};
  const auto result = sblr::DispatchSblrOperation(request);
  for (const auto& diagnostic : result.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.message << '\n';
  }
  for (const auto& diagnostic : result.api_result.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.detail << '\n';
  }
  Require(result.envelope_validated, "SBSFC-066 parsed runtime envelope invalid");
  Require(result.accepted, "SBSFC-066 parsed runtime dispatch rejected");
  Require(result.dispatched_to_api, "SBSFC-066 parsed runtime did not dispatch to API");
  Require(result.api_result.ok, "SBSFC-066 parsed CASE route failed");
  Require(FieldValue(result.api_result, projection_name) == expected_value,
          "SBSFC-066 parsed CASE deterministic result mismatch");
  Require(HasEvidence(result.api_result, "special_form_runtime", "special_case"),
          "SBSFC-066 parsed CASE runtime evidence missing");
  Require(HasEvidence(result.api_result, "operator_runtime", condition_op),
          "SBSFC-066 parsed CASE condition operator runtime evidence missing");
}

api::EngineRequestContext EngineContext() {
  api::EngineRequestContext context;
  context.request_id = "sbsql-sbsfc-066-case-grammar-exact-route";
  context.database_uuid.canonical = "019f0000-0000-7000-8000-000000066201";
  context.node_uuid.canonical = "019f0000-0000-7000-8000-000000066202";
  context.session_uuid.canonical = "019f0000-0000-7000-8000-000000066203";
  context.principal_uuid.canonical = "019f0000-0000-7000-8000-000000066204";
  context.transaction_uuid.canonical = "019f0000-0000-7000-8000-000000066205";
  context.statement_uuid.canonical = "019f0000-0000-7000-8000-000000066206";
  context.local_transaction_id = 66;
  context.security_context_present = true;
  context.trace_tags.push_back("right:QUERY_PROJECTION_TEST");
  for (const auto& row : kRows) {
    context.trace_tags.push_back(std::string("sbsql_surface_id:") +
                                 std::string(row.surface_id));
  }
  return context;
}

void AddTextOperand(sblr::SblrOperationEnvelope* envelope,
                    std::string name,
                    std::string value) {
  envelope->operands.push_back({"text", std::move(name), std::move(value)});
}

void AddLiteral(sblr::SblrOperationEnvelope* envelope,
                std::string prefix,
                std::string type,
                std::string value) {
  AddTextOperand(envelope, prefix + "expr_kind", "literal");
  AddTextOperand(envelope, prefix + "type", std::move(type));
  AddTextOperand(envelope, prefix + "value", std::move(value));
  AddTextOperand(envelope, prefix + "is_null", "false");
}

sblr::SblrOperationEnvelope EngineEnvelope(std::string_view trace_key) {
  auto envelope = sblr::MakeSblrEnvelope(std::string(kOperationId),
                                         std::string(kOpcode),
                                         std::string(trace_key));
  envelope.requires_security_context = true;
  envelope.requires_transaction_context = true;
  envelope.requires_cluster_authority = false;
  envelope.contains_sql_text = false;
  envelope.parser_resolved_names_to_uuids = true;
  AddTextOperand(&envelope, "projection_count", "1");
  AddTextOperand(&envelope, "projection_0_expr_kind", "special_form");
  AddTextOperand(&envelope, "projection_0_function_id", "sb.special.case");
  AddTextOperand(&envelope, "projection_0_special_form_id", "sb.special.case");
  AddTextOperand(&envelope, "projection_0_sblr_binding", "sblr.expr.special_case.v3");
  AddTextOperand(&envelope, "projection_0_type", "text");
  AddTextOperand(&envelope, "projection_0_is_null", "false");
  AddTextOperand(&envelope, "projection_0_special_form_arg_count", "3");
  return envelope;
}

void AddComparisonCondition(sblr::SblrOperationEnvelope* envelope,
                            std::string_view op,
                            std::string_view left,
                            std::string_view right) {
  AddTextOperand(envelope, "projection_0_arg_0_expr_kind", "operator");
  AddTextOperand(envelope, "projection_0_arg_0_operator_id", std::string(op));
  AddTextOperand(envelope, "projection_0_arg_0_canonical_operator_id",
                 op == std::string_view("op_gt") ? "sb.operator.greater"
                                                  : "sb.operator.equal");
  AddTextOperand(envelope, "projection_0_arg_0_type", "boolean");
  AddTextOperand(envelope, "projection_0_arg_0_is_null", "false");
  AddTextOperand(envelope, "projection_0_arg_0_operator_arg_count", "2");
  const std::string value_type = op == std::string_view("op_gt") ? "int64" : "text";
  AddLiteral(envelope, "projection_0_arg_0_arg_0_", value_type, std::string(left));
  AddLiteral(envelope, "projection_0_arg_0_arg_1_", value_type, std::string(right));
}

void RequireCaseRuntime(std::string_view trace_key,
                        std::string_view projection_name,
                        std::string_view condition_op,
                        std::string_view left,
                        std::string_view right,
                        std::string_view selected_value,
                        std::string_view fallback_value) {
  auto envelope = EngineEnvelope(trace_key);
  AddTextOperand(&envelope, "projection_0_name", std::string(projection_name));
  AddComparisonCondition(&envelope, condition_op, left, right);
  AddLiteral(&envelope, "projection_0_arg_1_", "text", std::string(selected_value));
  AddLiteral(&envelope, "projection_0_arg_2_", "text", std::string(fallback_value));

  const sblr::SblrDispatchRequest request{
      EngineContext(),
      std::move(envelope),
      api::EngineApiRequest{}};
  const auto result = sblr::DispatchSblrOperation(request);
  for (const auto& diagnostic : result.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.message << '\n';
  }
  for (const auto& diagnostic : result.api_result.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.detail << '\n';
  }
  Require(result.envelope_validated, "SBSFC-066 runtime envelope invalid");
  Require(result.accepted, "SBSFC-066 runtime dispatch rejected");
  Require(result.dispatched_to_api, "SBSFC-066 runtime did not dispatch to API");
  Require(result.api_result.ok, "EngineEvaluateProjection CASE route failed");
  Require(FieldValue(result.api_result, projection_name) == selected_value,
          "SBSFC-066 CASE deterministic result mismatch");
  Require(HasEvidence(result.api_result, "special_form_runtime", "special_case"),
          "SBSFC-066 CASE runtime evidence missing");
  Require(HasEvidence(result.api_result, "operator_runtime", condition_op),
          "SBSFC-066 CASE condition operator runtime evidence missing");
  Require(HasEvidence(result.api_result, "query_projection",
                      "constant_projection_engine_evaluated"),
          "SBSFC-066 projection runtime evidence missing");
}

}  // namespace

int main() {
  RequireRegistryEvidence();
  const std::vector<CaseRoute> cases = {
      {"SELECT CASE status WHEN 'A' THEN 'active' ELSE 'other' END AS status_label",
       "status_label",
       {"SBSQL-4B0E85EDA340", "SBSQL-ED8343236128"},
       {"reference_name\":\"status\"",
        "operator_id\":\"op_eq\"",
        "\"projection_0_special_form_id\":\"sb.special.case\""}},
      {"SELECT CASE WHEN amount > 10 THEN 'large' ELSE 'small' END AS amount_bucket",
       "amount_bucket",
       {"SBSQL-A2EAC47B5A98", "SBSQL-ED8343236128"},
       {"reference_name\":\"amount\"",
        "operator_id\":\"op_gt\"",
        "\"projection_0_special_form_id\":\"sb.special.case\""}},
      {"SELECT CASE status WHEN 'A' THEN 'active' ELSE 'other' END AS case_stmt_label",
       "case_stmt_label",
       {"SBSQL-ED8343236128"},
       {"\"projection_0_special_form_id\":\"sb.special.case\""}},
  };

  for (const auto& test_case : cases) {
    const auto artifacts = RunPipeline(test_case.sql);
    RequireExactLowering(test_case, artifacts);
    RequireServerAdmission(artifacts.envelope);
  }
  RequireParsedEnvelopeRuntime(
      "SELECT CASE 'A' WHEN 'A' THEN 'active' ELSE 'other' END AS status_label",
      "status_label",
      "active",
      "op_eq");
  RequireParsedEnvelopeRuntime(
      "SELECT CASE WHEN 12 > 10 THEN 'large' ELSE 'small' END AS amount_bucket",
      "amount_bucket",
      "large",
      "op_gt");
  RequireParsedEnvelopeRuntime(
      "SELECT CASE 'A' WHEN 'A' THEN 'active' ELSE 'other' END AS case_stmt_label",
      "case_stmt_label",
      "active",
      "op_eq");
  RequireCaseRuntime("trace.sbsfc066.simple_case.runtime",
                     "status_label",
                     "op_eq",
                     "A",
                     "A",
                     "active",
                     "other");
  RequireCaseRuntime("trace.sbsfc066.searched_case.runtime",
                     "amount_bucket",
                     "op_gt",
                     "12",
                     "10",
                     "large",
                     "small");
  std::cout << "sbsql_sbsfc_066_case_grammar_exact_route_conformance=passed\n";
  return EXIT_SUCCESS;
}

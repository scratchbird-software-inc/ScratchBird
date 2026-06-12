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
#include "sblr_admission.hpp"
#include "sblr_dispatch.hpp"
#include "sblr_engine_envelope.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace scratchbird::parser::sbsql;
namespace api = scratchbird::engine::internal_api;
namespace sblr = scratchbird::engine::sblr;

constexpr std::string_view kArraySql = "SELECT ARRAY[1, 2] AS array_value;";
constexpr std::string_view kRowSql = "SELECT ROW(1, 'alpha') AS row_value;";
constexpr std::string_view kOperationId = "query.evaluate_projection";
constexpr std::string_view kOpcode = "SBLR_QUERY_EVALUATE_PROJECTION";
constexpr std::string_view kQueryFamily = "sblr.query.relational.v3";
constexpr std::string_view kExpressionFamily = "sblr.expression.runtime.v3";
constexpr std::string_view kGeneralFamily = "sblr.general.operation.v3";

struct ConstructorRowEvidence {
  std::string_view surface_id;
  std::string_view canonical_name;
  std::string_view surface_kind;
  std::string_view family;
  std::string_view sblr_operation_family;
  std::string_view parser_handler_key;
  std::string_view lowering_handler_key;
  std::string_view server_admission_key;
  std::string_view engine_rule_key;
};

constexpr std::array<ConstructorRowEvidence, 7> kConstructorRows{{
    {"SBSQL-B4128030D7CC",
     "array_constructor",
     "grammar_production",
     "general",
     kGeneralFamily,
     "parser.grammar_ast",
     "lowering.sblr_family.sblr_general_operation_v3",
     "server.admission.sblr_general_operation_v3",
     "engine.rule.sblr_general_operation_v3"},
    {"SBSQL-6F7E1DB52AB4",
     "ARRAY",
     "function",
     "expression_runtime",
     kExpressionFamily,
     "parser.expression_runtime.function",
     "lowering.expression_runtime.function",
     "server.admission.sblr_expression_runtime_v3",
     "engine.rule.sblr_expression_runtime_v3"},
    {"SBSQL-88AEC458F6D4",
     "sb.special.array_constructor",
     "function",
     "expression_runtime",
     kExpressionFamily,
     "parser.expression_runtime.function",
     "lowering.expression_runtime.function",
     "server.admission.sblr_expression_runtime_v3",
     "engine.rule.sblr_expression_runtime_v3"},
    {"SBSQL-05C0C615EBD0",
     "row_constructor",
     "grammar_production",
     "general",
     kGeneralFamily,
     "parser.grammar_ast",
     "lowering.sblr_family.sblr_general_operation_v3",
     "server.admission.sblr_general_operation_v3",
     "engine.rule.sblr_general_operation_v3"},
    {"SBSQL-765F2091034E",
     "ROW",
     "function",
     "expression_runtime",
     kExpressionFamily,
     "parser.expression_runtime.function",
     "lowering.expression_runtime.function",
     "server.admission.sblr_expression_runtime_v3",
     "engine.rule.sblr_expression_runtime_v3"},
    {"SBSQL-4F87DEFF9A1A",
     "ROW(expr,...)",
     "function",
     "expression_runtime",
     kExpressionFamily,
     "parser.expression_runtime.function",
     "lowering.expression_runtime.function",
     "server.admission.sblr_expression_runtime_v3",
     "engine.rule.sblr_expression_runtime_v3"},
    {"SBSQL-C328F7FE54C8",
     "sb.special.row_constructor",
     "function",
     "expression_runtime",
     kExpressionFamily,
     "parser.expression_runtime.function",
     "lowering.expression_runtime.function",
     "server.admission.sblr_expression_runtime_v3",
     "engine.rule.sblr_expression_runtime_v3"},
}};

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

void PrintMessages(const MessageVectorSet& messages) {
  for (const auto& diagnostic : messages.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.message << '\n';
  }
}

SessionContext ParserSession() {
  SessionContext session;
  session.authenticated = true;
  session.session_uuid = "019f0000-0000-7000-8000-000000031101";
  session.connection_uuid = "019f0000-0000-7000-8000-000000031102";
  session.database_uuid = "019f0000-0000-7000-8000-000000031103";
  session.dialect_profile_uuid = "sbsql_v3";
  session.catalog_epoch = 311;
  session.security_policy_epoch = 312;
  session.descriptor_epoch = 313;
  return session;
}

ParserConfig ParserConfigForTest() {
  ParserConfig config;
  config.probe_mode = true;
  config.server_endpoint = "sb_server_constructor_route";
  config.parser_uuid = "019f0000-0000-7000-8000-000000031104";
  config.bundle_contract_id = "sbp_sbsql@constructor-route-test";
  config.build_id = "sbsql-constructor-route-test";
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

void RequireRegistryEvidence() {
  for (const auto& row : kConstructorRows) {
    const auto* registry_row = FindGeneratedSurfaceRegistryRowById(row.surface_id);
    Require(registry_row != nullptr, "constructor generated registry row missing");
    Require(registry_row->canonical_name == row.canonical_name,
            "constructor generated registry canonical name drifted");
    Require(registry_row->surface_kind == row.surface_kind,
            "constructor generated registry kind drifted");
    Require(registry_row->family == row.family,
            "constructor generated registry family drifted");
    Require(registry_row->source_status == "native_now",
            "constructor generated registry status drifted");
    Require(registry_row->cluster_scope == "noncluster_or_profile_scoped",
            "constructor generated registry cluster scope drifted");
    Require(registry_row->sblr_operation_family == row.sblr_operation_family,
            "constructor generated registry SBLR family drifted");
    Require(registry_row->parser_handler_key == row.parser_handler_key,
            "constructor generated registry parser handler drifted");
    Require(registry_row->lowering_handler_key == row.lowering_handler_key,
            "constructor generated registry lowering handler drifted");
    Require(registry_row->server_admission_key == row.server_admission_key,
            "constructor generated registry server admission drifted");
    Require(registry_row->engine_rule_key == row.engine_rule_key,
            "constructor generated registry engine rule drifted");
  }
}

void RequireExactLowering(const PipelineArtifacts& artifacts,
                          std::string_view label,
                          std::string_view projection_name,
                          std::string_view type_name,
                          std::string_view function_id,
                          std::string_view sblr_binding,
                          std::string_view engine_entrypoint,
                          std::initializer_list<std::string_view> surface_ids) {
  Require(!artifacts.cst.messages.has_errors(), "constructor CST failed");
  Require(!artifacts.ast.messages.has_errors(), "constructor AST failed");
  Require(artifacts.bound.bound, "constructor bind failed");
  PrintMessages(artifacts.verifier.messages);
  if (!artifacts.verifier.admitted) {
    std::cerr << artifacts.envelope.payload << '\n';
  }
  Require(artifacts.verifier.admitted, "constructor verifier rejected exact route");
  Require(artifacts.envelope.operation_family == kQueryFamily,
          "constructor query operation family mismatch");
  Require(artifacts.envelope.sblr_operation_key == kQueryFamily,
          "constructor query SBLR operation key mismatch");
  Require(artifacts.envelope.operation_id == kOperationId,
          "constructor operation id mismatch");
  Require(artifacts.envelope.engine_api_operation_id == kOperationId,
          "constructor engine API operation id mismatch");
  Require(artifacts.envelope.sblr_opcode == kOpcode,
          "constructor SBLR opcode mismatch");
  Require(HasValue(artifacts.envelope.required_authority_steps,
                   "authority.engine.query_projection_api_required"),
          "constructor engine projection authority step missing");
  Require(HasValue(artifacts.envelope.required_authority_steps,
                   "authority.server.transaction_context_required"),
          "constructor transaction context authority step missing");
  Require(HasValue(artifacts.envelope.required_authority_steps,
                   "authority.parser.no_sql_text_execution"),
          "constructor parser no-SQL-execution authority step missing");
  Require(!artifacts.envelope.parser_executes_sql,
          "constructor lowering allowed parser SQL execution");
  Require(!artifacts.envelope.real_file_effects,
          "constructor lowering allowed reference/file effects");
  Require(Contains(artifacts.envelope.payload, "\"query_envelope_kind\":\"scalar_projection\""),
          "constructor payload missing scalar projection envelope kind");
  Require(Contains(artifacts.envelope.payload, "\"projection_0_name\":\"") &&
              Contains(artifacts.envelope.payload, projection_name),
          "constructor payload missing projection name");
  Require(Contains(artifacts.envelope.payload, "\"projection_0_expr_kind\":\"special_form\""),
          "constructor payload missing special-form expression kind");
  Require(Contains(artifacts.envelope.payload, "\"projection_0_expr_opcode\":\"SBLR_SPECIAL_FORM_CALL\""),
          "constructor payload missing special-form SBLR opcode");
  Require(Contains(artifacts.envelope.payload, std::string("\"projection_0_type\":\"") +
                                                   std::string(type_name) + "\""),
          "constructor payload missing result type");
  Require(Contains(artifacts.envelope.payload, std::string("\"projection_0_function_id\":\"") +
                                                   std::string(function_id) + "\""),
          "constructor payload missing canonical function id");
  Require(Contains(artifacts.envelope.payload, std::string("\"projection_0_special_form_id\":\"") +
                                                   std::string(function_id) + "\""),
          "constructor payload missing special form id");
  Require(Contains(artifacts.envelope.payload, std::string("\"projection_0_sblr_binding\":\"") +
                                                   std::string(sblr_binding) + "\""),
          "constructor payload missing SBLR binding id");
  Require(Contains(artifacts.envelope.payload, std::string("\"projection_0_engine_entrypoint\":\"") +
                                                   std::string(engine_entrypoint) + "\""),
          "constructor payload missing engine entrypoint id");
  for (const auto surface_id : surface_ids) {
    Require(Contains(artifacts.envelope.payload, surface_id),
            "constructor payload missing row-identifiable surface evidence");
  }
  Require(Contains(artifacts.envelope.payload, "\"sql_text_included\":false"),
          "constructor payload did not prove no SQL text authority");
  Require(!Contains(artifacts.envelope.payload, "reference"),
          "constructor payload carried reference authority");
  Require(!Contains(artifacts.envelope.payload, "WAL") &&
              !Contains(artifacts.envelope.payload, "wal") &&
              !Contains(artifacts.envelope.payload, "recovery"),
          "constructor payload carried WAL/recovery authority");
  (void)label;
}

void RequireServerAdmission(const SblrEnvelope& envelope) {
  const auto admission = scratchbird::server::AdmitServerSblrEnvelope(
      scratchbird::server::ServerSblrAdmissionRequest{envelope.payload, false});
  Require(admission.admitted, "server admission rejected constructor exact route");
  Require(admission.requires_public_abi_dispatch,
          "server admission did not require public ABI dispatch for constructor");
  Require(admission.operation_id == kOperationId,
          "server admission constructor operation id mismatch");
  Require(admission.operation_family == kQueryFamily,
          "server admission constructor operation family mismatch");
}

api::EngineRequestContext EngineContext(std::string_view request_id) {
  api::EngineRequestContext context;
  context.request_id = std::string(request_id);
  context.database_uuid.canonical = "019f0000-0000-7000-8000-000000031201";
  context.node_uuid.canonical = "019f0000-0000-7000-8000-000000031202";
  context.session_uuid.canonical = "019f0000-0000-7000-8000-000000031203";
  context.principal_uuid.canonical = "019f0000-0000-7000-8000-000000031204";
  context.transaction_uuid.canonical = "019f0000-0000-7000-8000-000000031205";
  context.statement_uuid.canonical = "019f0000-0000-7000-8000-000000031206";
  context.current_schema_uuid.canonical = "019f0000-0000-7000-8000-000000031207";
  context.current_role_uuid.canonical = "019f0000-0000-7000-8000-000000031208";
  context.local_transaction_id = 93;
  context.security_context_present = true;
  context.trace_tags.push_back("right:QUERY_PROJECTION_TEST");
  return context;
}

void AddLiteralArg(sblr::SblrOperationEnvelope* envelope,
                   std::string_view prefix,
                   std::string_view type_name,
                   std::string_view value) {
  envelope->operands.push_back({"text", std::string(prefix) + "expr_kind", "literal"});
  envelope->operands.push_back({"text", std::string(prefix) + "type", std::string(type_name)});
  envelope->operands.push_back({"text", std::string(prefix) + "value", std::string(value)});
  envelope->operands.push_back({"text", std::string(prefix) + "is_null", "false"});
}

sblr::SblrOperationEnvelope EngineEnvelope(std::string_view projection_name,
                                           std::string_view result_type,
                                           std::string_view function_id,
                                           std::string_view sblr_binding,
                                           std::string_view trace_id) {
  auto envelope = sblr::MakeSblrEnvelope(std::string(kOperationId),
                                         std::string(kOpcode),
                                         std::string(trace_id));
  envelope.requires_security_context = true;
  envelope.requires_transaction_context = true;
  envelope.requires_cluster_authority = false;
  envelope.contains_sql_text = false;
  envelope.parser_resolved_names_to_uuids = true;
  envelope.operands.push_back({"text", "projection_count", "1"});
  envelope.operands.push_back({"text", "projection_0_name", std::string(projection_name)});
  envelope.operands.push_back({"text", "projection_0_expr_kind", "special_form"});
  envelope.operands.push_back({"text", "projection_0_type", std::string(result_type)});
  envelope.operands.push_back({"text", "projection_0_function_id", std::string(function_id)});
  envelope.operands.push_back({"text", "projection_0_special_form_id", std::string(function_id)});
  envelope.operands.push_back({"text", "projection_0_sblr_binding", std::string(sblr_binding)});
  envelope.operands.push_back({"text", "projection_0_special_form_arg_count", "2"});
  AddLiteralArg(&envelope, "projection_0_arg_0_", "bigint", "1");
  return envelope;
}

void RequireEngineDispatch(std::string_view request_id,
                           sblr::SblrOperationEnvelope envelope,
                           std::string_view projection_name,
                           std::string_view expected_type,
                           std::string_view expected_value,
                           std::string_view evidence_id) {
  const sblr::SblrDispatchRequest request{
      EngineContext(request_id),
      std::move(envelope),
      api::EngineApiRequest{}};
  const auto result = sblr::DispatchSblrOperation(request);
  for (const auto& diagnostic : result.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.message << '\n';
  }
  for (const auto& diagnostic : result.api_result.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.detail << '\n';
  }
  Require(result.envelope_validated, "engine SBLR envelope did not validate");
  Require(result.accepted, "engine SBLR dispatch did not accept constructor projection");
  Require(result.dispatched_to_api, "engine SBLR dispatch did not route to internal API");
  Require(result.api_result.ok, "EngineEvaluateProjection did not return constructor success");
  Require(result.api_result.operation_id == kOperationId,
          "EngineEvaluateProjection returned wrong operation id");
  Require(result.api_result.result_shape.rows.size() == 1,
          "constructor result row count mismatch");
  Require(result.api_result.result_shape.rows.front().fields.size() == 1,
          "constructor result field count mismatch");
  const auto& field = result.api_result.result_shape.rows.front().fields.front();
  Require(field.first == projection_name, "constructor result field name mismatch");
  Require(field.second.descriptor.canonical_type_name == expected_type,
          "constructor result descriptor mismatch");
  Require(!field.second.is_null, "constructor result unexpectedly null");
  Require(field.second.encoded_value == expected_value,
          "constructor result encoded value mismatch");
  Require(HasEvidence(result.api_result, "special_form_runtime", evidence_id),
          "constructor result missing special form runtime evidence");
}

}  // namespace

int main() {
  RequireRegistryEvidence();

  const auto array_artifacts = RunPipeline(kArraySql);
  RequireExactLowering(array_artifacts,
                       "ARRAY",
                       "array_value",
                       "array",
                       "sb.special.array_constructor",
                       "sblr.expr.special_array_constructor.v3",
                       "special_array_constructor",
                       {"SBSQL-B4128030D7CC",
                        "SBSQL-6F7E1DB52AB4",
                        "SBSQL-88AEC458F6D4"});
  RequireServerAdmission(array_artifacts.envelope);
  auto array_envelope = EngineEnvelope("array_value",
                                       "array",
                                       "sb.special.array_constructor",
                                       "sblr.expr.special_array_constructor.v3",
                                       "trace.array_constructor.exact_route.SBSQL-B4128030D7CC");
  AddLiteralArg(&array_envelope, "projection_0_arg_1_", "bigint", "2");
  RequireEngineDispatch("sbsql-array-constructor-exact-route",
                        std::move(array_envelope),
                        "array_value",
                        "array",
                        "array[bigint:1,bigint:2]",
                        "special_array_constructor");

  const auto row_artifacts = RunPipeline(kRowSql);
  RequireExactLowering(row_artifacts,
                       "ROW",
                       "row_value",
                       "row",
                       "sb.special.row_constructor",
                       "sblr.expr.special_row_constructor.v3",
                       "special_row_constructor",
                       {"SBSQL-05C0C615EBD0",
                        "SBSQL-765F2091034E",
                        "SBSQL-4F87DEFF9A1A",
                        "SBSQL-C328F7FE54C8"});
  RequireServerAdmission(row_artifacts.envelope);
  auto row_envelope = EngineEnvelope("row_value",
                                     "row",
                                     "sb.special.row_constructor",
                                     "sblr.expr.special_row_constructor.v3",
                                     "trace.row_constructor.exact_route.SBSQL-05C0C615EBD0");
  AddLiteralArg(&row_envelope, "projection_0_arg_1_", "text", "alpha");
  RequireEngineDispatch("sbsql-row-constructor-exact-route",
                        std::move(row_envelope),
                        "row_value",
                        "row",
                        "row(bigint:1,text:alpha)",
                        "special_row_constructor");

  std::cout << "sbsql_constructor_exact_route_conformance=passed\n";
  return EXIT_SUCCESS;
}

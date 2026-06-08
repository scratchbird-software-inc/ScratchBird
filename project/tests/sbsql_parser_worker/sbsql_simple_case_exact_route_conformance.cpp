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

#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace scratchbird::parser::sbsql;
namespace api = scratchbird::engine::internal_api;
namespace sblr = scratchbird::engine::sblr;

constexpr std::string_view kSql =
    "SELECT CASE 1 WHEN 1 THEN 'one' ELSE 'other' END AS simple_case_value;";
constexpr std::string_view kOperationId = "query.evaluate_projection";
constexpr std::string_view kOpcode = "SBLR_QUERY_EVALUATE_PROJECTION";
constexpr std::string_view kQueryFamily = "sblr.query.relational.v3";

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
  for (const auto& value : values) {
    if (value == expected) return true;
  }
  return false;
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
  session.session_uuid = "019f0000-0000-7000-8000-000000028101";
  session.connection_uuid = "019f0000-0000-7000-8000-000000028102";
  session.database_uuid = "019f0000-0000-7000-8000-000000028103";
  session.dialect_profile_uuid = "sbsql_v3";
  session.catalog_epoch = 81;
  session.security_policy_epoch = 82;
  session.descriptor_epoch = 83;
  return session;
}

ParserConfig ParserConfigForTest() {
  ParserConfig config;
  config.probe_mode = true;
  config.server_endpoint = "sb_server_simple_case_route";
  config.parser_uuid = "019f0000-0000-7000-8000-000000028104";
  config.bundle_contract_id = "sbp_sbsql@simple-case-route-test";
  config.build_id = "sbsql-simple-case-route-test";
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
  const auto* row = FindGeneratedSurfaceRegistryRowById("SBSQL-00BFC4EDD817");
  Require(row != nullptr, "simple_case_expr generated registry row missing");
  Require(row->canonical_name == "simple_case_expr",
          "simple_case_expr generated registry canonical name drifted");
  Require(row->surface_kind == "grammar_production",
          "simple_case_expr generated registry kind drifted");
  Require(row->family == "general", "simple_case_expr generated registry family drifted");
  Require(row->source_status == "native_now",
          "simple_case_expr generated registry status drifted");
  Require(row->cluster_scope == "noncluster_or_profile_scoped",
          "simple_case_expr generated registry cluster scope drifted");
  Require(row->sblr_operation_family == "sblr.general.operation.v3",
          "simple_case_expr generated registry SBLR family drifted");
  Require(row->parser_handler_key == "parser.grammar_ast",
          "simple_case_expr generated registry parser handler drifted");
  Require(row->lowering_handler_key == "lowering.sblr_family.sblr_general_operation_v3",
          "simple_case_expr generated registry lowering handler drifted");
  Require(row->server_admission_key == "server.admission.sblr_general_operation_v3",
          "simple_case_expr generated registry server admission drifted");
  Require(row->engine_rule_key == "engine.rule.sblr_general_operation_v3",
          "simple_case_expr generated registry engine rule drifted");

  const auto* operator_token = FindGeneratedSurfaceRegistryRowById("SBSQL-062A23531925");
  Require(operator_token != nullptr, "operator_token generated registry row missing");
  Require(operator_token->canonical_name == "operator_token",
          "operator_token generated registry canonical name drifted");
  Require(operator_token->surface_kind == "function",
          "operator_token generated registry kind drifted");
  Require(operator_token->family == "expression_runtime",
          "operator_token generated registry family drifted");
  Require(operator_token->source_status == "native_now",
          "operator_token generated registry status drifted");
  Require(operator_token->cluster_scope == "noncluster_or_profile_scoped",
          "operator_token generated registry cluster scope drifted");
  Require(operator_token->sblr_operation_family == "sblr.expression.runtime.v3",
          "operator_token generated registry SBLR family drifted");
}

void RequireExactLowering(const PipelineArtifacts& artifacts) {
  Require(!artifacts.cst.messages.has_errors(), "simple CASE CST failed");
  Require(!artifacts.ast.messages.has_errors(), "simple CASE AST failed");
  Require(artifacts.bound.bound, "simple CASE bind failed");
  Require(artifacts.verifier.admitted, "simple CASE verifier rejected exact route");
  Require(artifacts.envelope.operation_family == kQueryFamily,
          "simple CASE query operation family mismatch");
  Require(artifacts.envelope.operation_id == kOperationId,
          "simple CASE operation id mismatch");
  Require(artifacts.envelope.sblr_opcode == kOpcode,
          "simple CASE SBLR opcode mismatch");
  Require(HasValue(artifacts.envelope.required_authority_steps,
                   "authority.parser.no_sql_text_execution"),
          "simple CASE parser no-SQL-execution authority step missing");
  Require(!artifacts.envelope.parser_executes_sql,
          "simple CASE lowering allowed parser SQL execution");
  Require(Contains(artifacts.envelope.payload, "\"projection_0_expr_kind\":\"special_form\""),
          "simple CASE payload missing special-form expression kind");
  Require(Contains(artifacts.envelope.payload, "\"projection_0_function_id\":\"sb.special.case\""),
          "simple CASE payload missing canonical function id");
  Require(Contains(artifacts.envelope.payload, "\"projection_0_special_form_id\":\"sb.special.case\""),
          "simple CASE payload missing special form id");
  Require(Contains(artifacts.envelope.payload, "\"projection_0_sblr_binding\":\"sblr.expr.special_case.v3\""),
          "simple CASE payload missing SBLR binding id");
  Require(Contains(artifacts.envelope.payload, "SBSQL-00BFC4EDD817"),
          "simple CASE payload missing row-identifiable surface evidence");
  Require(Contains(artifacts.envelope.payload, "\"projection_0_arg_0_expr_kind\":\"operator\""),
          "simple CASE payload missing equality condition operator");
  Require(Contains(artifacts.envelope.payload, "\"projection_0_arg_0_operator_id\":\"op_eq\""),
          "simple CASE payload missing equality operator id");
  Require(Contains(artifacts.envelope.payload, "SBSQL-062A23531925"),
          "simple CASE payload missing operator_token surface evidence");
  Require(Contains(artifacts.envelope.payload, "\"projection_0_arg_1_value\":\"one\""),
          "simple CASE payload missing THEN operand");
  Require(Contains(artifacts.envelope.payload, "\"projection_0_arg_2_value\":\"other\""),
          "simple CASE payload missing ELSE operand");
  Require(Contains(artifacts.envelope.payload, "\"sql_text_included\":false"),
          "simple CASE payload did not prove no SQL text authority");
  Require(!Contains(artifacts.envelope.payload, "WAL") &&
              !Contains(artifacts.envelope.payload, "wal") &&
              !Contains(artifacts.envelope.payload, "recovery"),
          "simple CASE payload carried WAL/recovery authority");
}

void RequireServerAdmission(const SblrEnvelope& envelope) {
  const auto admission = scratchbird::server::AdmitServerSblrEnvelope(
      scratchbird::server::ServerSblrAdmissionRequest{envelope.payload, false});
  Require(admission.admitted, "server admission rejected simple CASE exact route");
  Require(admission.requires_public_abi_dispatch,
          "server admission did not require public ABI dispatch for simple CASE");
  Require(admission.operation_id == kOperationId,
          "server admission simple CASE operation id mismatch");
}

api::EngineRequestContext EngineContext() {
  api::EngineRequestContext context;
  context.request_id = "sbsql-simple-case-exact-route";
  context.database_uuid.canonical = "019f0000-0000-7000-8000-000000028201";
  context.node_uuid.canonical = "019f0000-0000-7000-8000-000000028202";
  context.session_uuid.canonical = "019f0000-0000-7000-8000-000000028203";
  context.principal_uuid.canonical = "019f0000-0000-7000-8000-000000028204";
  context.transaction_uuid.canonical = "019f0000-0000-7000-8000-000000028205";
  context.statement_uuid.canonical = "019f0000-0000-7000-8000-000000028206";
  context.current_schema_uuid.canonical = "019f0000-0000-7000-8000-000000028207";
  context.current_role_uuid.canonical = "019f0000-0000-7000-8000-000000028208";
  context.local_transaction_id = 94;
  context.security_context_present = true;
  context.trace_tags.push_back("right:QUERY_PROJECTION_TEST");
  context.trace_tags.push_back("sbsql_surface_id:SBSQL-00BFC4EDD817");
  context.trace_tags.push_back("sbsql_surface_id:SBSQL-062A23531925");
  return context;
}

sblr::SblrOperationEnvelope EngineEnvelope() {
  auto envelope = sblr::MakeSblrEnvelope(std::string(kOperationId),
                                         std::string(kOpcode),
                                         "trace.simple_case.exact_route.SBSQL-00BFC4EDD817");
  envelope.requires_security_context = true;
  envelope.requires_transaction_context = true;
  envelope.requires_cluster_authority = false;
  envelope.contains_sql_text = false;
  envelope.parser_resolved_names_to_uuids = true;
  envelope.operands.push_back({"text", "projection_count", "1"});
  envelope.operands.push_back({"text", "projection_0_name", "simple_case_value"});
  envelope.operands.push_back({"text", "projection_0_expr_kind", "special_form"});
  envelope.operands.push_back({"text", "projection_0_type", "text"});
  envelope.operands.push_back({"text", "projection_0_function_id", "sb.special.case"});
  envelope.operands.push_back({"text", "projection_0_special_form_id", "sb.special.case"});
  envelope.operands.push_back({"text", "projection_0_sblr_binding", "sblr.expr.special_case.v3"});
  envelope.operands.push_back({"text", "projection_0_special_form_arg_count", "3"});
  envelope.operands.push_back({"text", "projection_0_arg_0_expr_kind", "operator"});
  envelope.operands.push_back({"text", "projection_0_arg_0_type", "boolean"});
  envelope.operands.push_back({"text", "projection_0_arg_0_operator_id", "op_eq"});
  envelope.operands.push_back({"text", "projection_0_arg_0_canonical_operator_id", "sb.operator.equal"});
  envelope.operands.push_back({"text", "projection_0_arg_0_operator_arg_count", "2"});
  envelope.operands.push_back({"text", "projection_0_arg_0_arg_0_expr_kind", "literal"});
  envelope.operands.push_back({"text", "projection_0_arg_0_arg_0_type", "bigint"});
  envelope.operands.push_back({"text", "projection_0_arg_0_arg_0_value", "1"});
  envelope.operands.push_back({"text", "projection_0_arg_0_arg_0_is_null", "false"});
  envelope.operands.push_back({"text", "projection_0_arg_0_arg_1_expr_kind", "literal"});
  envelope.operands.push_back({"text", "projection_0_arg_0_arg_1_type", "bigint"});
  envelope.operands.push_back({"text", "projection_0_arg_0_arg_1_value", "1"});
  envelope.operands.push_back({"text", "projection_0_arg_0_arg_1_is_null", "false"});
  envelope.operands.push_back({"text", "projection_0_arg_1_expr_kind", "literal"});
  envelope.operands.push_back({"text", "projection_0_arg_1_type", "text"});
  envelope.operands.push_back({"text", "projection_0_arg_1_value", "one"});
  envelope.operands.push_back({"text", "projection_0_arg_1_is_null", "false"});
  envelope.operands.push_back({"text", "projection_0_arg_2_expr_kind", "literal"});
  envelope.operands.push_back({"text", "projection_0_arg_2_type", "text"});
  envelope.operands.push_back({"text", "projection_0_arg_2_value", "other"});
  envelope.operands.push_back({"text", "projection_0_arg_2_is_null", "false"});
  return envelope;
}

void RequireEngineDispatch() {
  const sblr::SblrDispatchRequest request{
      EngineContext(),
      EngineEnvelope(),
      api::EngineApiRequest{}};
  const auto result = sblr::DispatchSblrOperation(request);
  for (const auto& diagnostic : result.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.message << '\n';
  }
  for (const auto& diagnostic : result.api_result.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.detail << '\n';
  }
  Require(result.envelope_validated, "engine SBLR envelope did not validate");
  Require(result.accepted, "engine SBLR dispatch did not accept simple CASE projection");
  Require(result.dispatched_to_api, "engine SBLR dispatch did not route to internal API");
  Require(result.api_result.ok, "EngineEvaluateProjection did not return success for simple CASE");
  Require(result.api_result.result_shape.rows.size() == 1,
          "simple CASE result row count mismatch");
  const auto& field = result.api_result.result_shape.rows.front().fields.front();
  Require(field.first == "simple_case_value", "simple CASE result field name mismatch");
  Require(field.second.descriptor.canonical_type_name == "text",
          "simple CASE result descriptor mismatch");
  Require(!field.second.is_null, "simple CASE result unexpectedly null");
  Require(field.second.encoded_value == "one", "simple CASE result value mismatch");
  Require(HasEvidence(result.api_result, "special_form_runtime", "special_case"),
          "simple CASE result missing special form runtime evidence");
}

}  // namespace

int main() {
  RequireRegistryEvidence();
  const auto artifacts = RunPipeline();
  RequireExactLowering(artifacts);
  RequireServerAdmission(artifacts.envelope);
  RequireEngineDispatch();
  std::cout << "sbsql_simple_case_exact_route_conformance=passed\n";
  return EXIT_SUCCESS;
}

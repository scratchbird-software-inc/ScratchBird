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

constexpr std::string_view kSql = "SELECT 5 IN (1, 5, 9) AS in_value;";
constexpr std::string_view kOperationId = "query.evaluate_projection";
constexpr std::string_view kOpcode = "SBLR_QUERY_EVALUATE_PROJECTION";
constexpr std::string_view kQueryFamily = "sblr.query.relational.v3";
constexpr std::string_view kExpressionFamily = "sblr.expression.runtime.v3";

struct InRowEvidence {
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

constexpr std::array<InRowEvidence, 3> kInRows{{
    {"SBSQL-1287554A5489",
     "special_form",
     "grammar_production",
     "general",
     "sblr.general.operation.v3",
     "parser.grammar_ast",
     "lowering.sblr_family.sblr_general_operation_v3",
     "server.admission.sblr_general_operation_v3",
     "engine.rule.sblr_general_operation_v3"},
    {"SBSQL-AF51A9A07F10",
     "IN",
     "function",
     "expression_runtime",
     kExpressionFamily,
     "parser.expression_runtime.function",
     "lowering.expression_runtime.function",
     "server.admission.sblr_expression_runtime_v3",
     "engine.rule.sblr_expression_runtime_v3"},
    {"SBSQL-28270EB632EF",
     "sb.special.in",
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

SessionContext ParserSession() {
  SessionContext session;
  session.authenticated = true;
  session.session_uuid = "019f0000-0000-7000-8000-000000026101";
  session.connection_uuid = "019f0000-0000-7000-8000-000000026102";
  session.database_uuid = "019f0000-0000-7000-8000-000000026103";
  session.dialect_profile_uuid = "sbsql_v3";
  session.catalog_epoch = 61;
  session.security_policy_epoch = 62;
  session.descriptor_epoch = 63;
  return session;
}

ParserConfig ParserConfigForTest() {
  ParserConfig config;
  config.probe_mode = true;
  config.server_endpoint = "sb_server_in_route";
  config.parser_uuid = "019f0000-0000-7000-8000-000000026104";
  config.bundle_contract_id = "sbp_sbsql@in-route-test";
  config.build_id = "sbsql-in-route-test";
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
  for (const auto& row : kInRows) {
    const auto* registry_row = FindGeneratedSurfaceRegistryRowById(row.surface_id);
    Require(registry_row != nullptr, "IN generated registry row missing");
    Require(registry_row->canonical_name == row.canonical_name,
            "IN generated registry canonical name drifted");
    Require(registry_row->surface_kind == row.surface_kind,
            "IN generated registry kind drifted");
    Require(registry_row->family == row.family,
            "IN generated registry family drifted");
    Require(registry_row->source_status == "native_now",
            "IN generated registry status drifted");
    Require(registry_row->cluster_scope == "noncluster_or_profile_scoped",
            "IN generated registry cluster scope drifted");
    Require(registry_row->sblr_operation_family == row.sblr_operation_family,
            "IN generated registry SBLR family drifted");
    Require(registry_row->parser_handler_key == row.parser_handler_key,
            "IN generated registry parser handler drifted");
    Require(registry_row->lowering_handler_key == row.lowering_handler_key,
            "IN generated registry lowering handler drifted");
    Require(registry_row->server_admission_key == row.server_admission_key,
            "IN generated registry server admission drifted");
    Require(registry_row->engine_rule_key == row.engine_rule_key,
            "IN generated registry engine rule drifted");
  }
}

void RequireExactLowering(const PipelineArtifacts& artifacts) {
  Require(!artifacts.cst.messages.has_errors(), "IN CST failed");
  Require(!artifacts.ast.messages.has_errors(), "IN AST failed");
  Require(artifacts.bound.bound, "IN bind failed");
  Require(artifacts.verifier.admitted, "IN verifier rejected exact route");
  Require(artifacts.envelope.operation_family == kQueryFamily,
          "IN query operation family mismatch");
  Require(artifacts.envelope.sblr_operation_key == kQueryFamily,
          "IN query SBLR operation key mismatch");
  Require(artifacts.envelope.operation_id == kOperationId,
          "IN operation id mismatch");
  Require(artifacts.envelope.engine_api_operation_id == kOperationId,
          "IN engine API operation id mismatch");
  Require(artifacts.envelope.sblr_opcode == kOpcode,
          "IN SBLR opcode mismatch");
  Require(HasValue(artifacts.envelope.required_authority_steps,
                   "authority.engine.query_projection_api_required"),
          "IN engine projection authority step missing");
  Require(HasValue(artifacts.envelope.required_authority_steps,
                   "authority.server.transaction_context_required"),
          "IN route must require transaction context through projection authority");
  Require(HasValue(artifacts.envelope.required_authority_steps,
                   "authority.parser.no_sql_text_execution"),
          "IN parser no-SQL-execution authority step missing");
  Require(!artifacts.envelope.parser_executes_sql,
          "IN lowering allowed parser SQL execution");
  Require(!artifacts.envelope.real_file_effects,
          "IN lowering allowed reference/file effects");
  Require(Contains(artifacts.envelope.payload, "\"projection_0_expr_kind\":\"special_form\""),
          "IN payload missing special-form expression kind");
  Require(Contains(artifacts.envelope.payload, "\"projection_0_function_id\":\"sb.special.in\""),
          "IN payload missing canonical function id");
  Require(Contains(artifacts.envelope.payload, "\"projection_0_special_form_id\":\"sb.special.in\""),
          "IN payload missing special form id");
  Require(Contains(artifacts.envelope.payload, "\"projection_0_sblr_binding\":\"sblr.expr.special_in.v3\""),
          "IN payload missing SBLR binding id");
  Require(Contains(artifacts.envelope.payload, "\"projection_0_engine_entrypoint\":\"special_in\""),
          "IN payload missing engine entrypoint id");
  Require(Contains(artifacts.envelope.payload, "\"projection_0_special_form_arg_count\":\"4\""),
          "IN payload missing argument count");
  for (const auto& row : kInRows) {
    Require(Contains(artifacts.envelope.payload, row.surface_id),
            "IN payload missing row-identifiable surface evidence");
  }
  Require(Contains(artifacts.envelope.payload, "\"projection_0_arg_0_value\":\"5\""),
          "IN payload missing value operand");
  Require(Contains(artifacts.envelope.payload, "\"projection_0_arg_1_value\":\"1\""),
          "IN payload missing first list operand");
  Require(Contains(artifacts.envelope.payload, "\"projection_0_arg_2_value\":\"5\""),
          "IN payload missing matching list operand");
  Require(Contains(artifacts.envelope.payload, "\"projection_0_arg_3_value\":\"9\""),
          "IN payload missing third list operand");
  Require(Contains(artifacts.envelope.payload, "\"sql_text_included\":false"),
          "IN payload did not prove no SQL text authority");
  Require(!Contains(artifacts.envelope.payload, "reference"),
          "IN payload carried reference authority");
  Require(!Contains(artifacts.envelope.payload, "WAL") &&
              !Contains(artifacts.envelope.payload, "wal") &&
              !Contains(artifacts.envelope.payload, "recovery"),
          "IN payload carried WAL/recovery authority");
}

void RequireServerAdmission(const SblrEnvelope& envelope) {
  const auto admission = scratchbird::server::AdmitServerSblrEnvelope(
      scratchbird::server::ServerSblrAdmissionRequest{envelope.payload, false});
  Require(admission.admitted, "server admission rejected IN exact route");
  Require(admission.requires_public_abi_dispatch,
          "server admission did not require public ABI dispatch for IN");
  Require(admission.operation_id == kOperationId,
          "server admission IN operation id mismatch");
  Require(admission.operation_family == kQueryFamily,
          "server admission IN operation family mismatch");
}

api::EngineRequestContext EngineContext() {
  api::EngineRequestContext context;
  context.request_id = "sbsql-in-exact-route";
  context.database_uuid.canonical = "019f0000-0000-7000-8000-000000026201";
  context.node_uuid.canonical = "019f0000-0000-7000-8000-000000026202";
  context.session_uuid.canonical = "019f0000-0000-7000-8000-000000026203";
  context.principal_uuid.canonical = "019f0000-0000-7000-8000-000000026204";
  context.transaction_uuid.canonical = "019f0000-0000-7000-8000-000000026205";
  context.statement_uuid.canonical = "019f0000-0000-7000-8000-000000026206";
  context.current_schema_uuid.canonical = "019f0000-0000-7000-8000-000000026207";
  context.current_role_uuid.canonical = "019f0000-0000-7000-8000-000000026208";
  context.local_transaction_id = 92;
  context.security_context_present = true;
  context.trace_tags.push_back("right:QUERY_PROJECTION_TEST");
  context.trace_tags.push_back("sbsql_surface_id:SBSQL-AF51A9A07F10");
  context.trace_tags.push_back("sbsql_surface_id:SBSQL-28270EB632EF");
  return context;
}

sblr::SblrOperationEnvelope EngineEnvelope() {
  auto envelope = sblr::MakeSblrEnvelope(std::string(kOperationId),
                                         std::string(kOpcode),
                                         "trace.in.exact_route.SBSQL-AF51A9A07F10");
  envelope.requires_security_context = true;
  envelope.requires_transaction_context = true;
  envelope.requires_cluster_authority = false;
  envelope.contains_sql_text = false;
  envelope.parser_resolved_names_to_uuids = true;
  envelope.operands.push_back({"text", "projection_count", "1"});
  envelope.operands.push_back({"text", "projection_0_name", "in_value"});
  envelope.operands.push_back({"text", "projection_0_expr_kind", "special_form"});
  envelope.operands.push_back({"text", "projection_0_type", "boolean"});
  envelope.operands.push_back({"text", "projection_0_function_id", "sb.special.in"});
  envelope.operands.push_back({"text", "projection_0_special_form_id", "sb.special.in"});
  envelope.operands.push_back({"text", "projection_0_sblr_binding", "sblr.expr.special_in.v3"});
  envelope.operands.push_back({"text", "projection_0_special_form_arg_count", "4"});
  envelope.operands.push_back({"text", "projection_0_arg_0_expr_kind", "literal"});
  envelope.operands.push_back({"text", "projection_0_arg_0_type", "bigint"});
  envelope.operands.push_back({"text", "projection_0_arg_0_value", "5"});
  envelope.operands.push_back({"text", "projection_0_arg_0_is_null", "false"});
  envelope.operands.push_back({"text", "projection_0_arg_1_expr_kind", "literal"});
  envelope.operands.push_back({"text", "projection_0_arg_1_type", "bigint"});
  envelope.operands.push_back({"text", "projection_0_arg_1_value", "1"});
  envelope.operands.push_back({"text", "projection_0_arg_1_is_null", "false"});
  envelope.operands.push_back({"text", "projection_0_arg_2_expr_kind", "literal"});
  envelope.operands.push_back({"text", "projection_0_arg_2_type", "bigint"});
  envelope.operands.push_back({"text", "projection_0_arg_2_value", "5"});
  envelope.operands.push_back({"text", "projection_0_arg_2_is_null", "false"});
  envelope.operands.push_back({"text", "projection_0_arg_3_expr_kind", "literal"});
  envelope.operands.push_back({"text", "projection_0_arg_3_type", "bigint"});
  envelope.operands.push_back({"text", "projection_0_arg_3_value", "9"});
  envelope.operands.push_back({"text", "projection_0_arg_3_is_null", "false"});
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
  Require(result.accepted, "engine SBLR dispatch did not accept IN projection");
  Require(result.dispatched_to_api, "engine SBLR dispatch did not route to internal API");
  Require(result.api_result.ok, "EngineEvaluateProjection did not return success for IN");
  Require(result.api_result.operation_id == kOperationId,
          "EngineEvaluateProjection returned wrong operation id");
  Require(result.api_result.result_shape.rows.size() == 1,
          "IN result row count mismatch");
  Require(result.api_result.result_shape.rows.front().fields.size() == 1,
          "IN result field count mismatch");
  const auto& field = result.api_result.result_shape.rows.front().fields.front();
  Require(field.first == "in_value", "IN result field name mismatch");
  Require(field.second.descriptor.canonical_type_name == "boolean",
          "IN result descriptor mismatch");
  Require(!field.second.is_null, "IN result unexpectedly null");
  Require(field.second.encoded_value == "true" || field.second.encoded_value == "TRUE" ||
              field.second.encoded_value == "1",
          "IN result value mismatch");
  Require(HasEvidence(result.api_result, "special_form_runtime", "special_in"),
          "IN result missing special form runtime evidence");
  Require(HasEvidence(result.api_result, "operator_runtime", "op_eq"),
          "IN result missing equality comparison evidence");
}

}  // namespace

int main() {
  RequireRegistryEvidence();
  const auto artifacts = RunPipeline();
  RequireExactLowering(artifacts);
  RequireServerAdmission(artifacts.envelope);
  RequireEngineDispatch();
  std::cout << "sbsql_in_exact_route_conformance=passed\n";
  return EXIT_SUCCESS;
}

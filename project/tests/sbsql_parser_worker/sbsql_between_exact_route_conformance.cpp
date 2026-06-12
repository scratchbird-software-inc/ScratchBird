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

constexpr std::string_view kSql = "SELECT 5 BETWEEN 1 AND 9 AS between_value;";
constexpr std::string_view kOperationId = "query.evaluate_projection";
constexpr std::string_view kOpcode = "SBLR_QUERY_EVALUATE_PROJECTION";
constexpr std::string_view kQueryFamily = "sblr.query.relational.v3";
constexpr std::string_view kExpressionFamily = "sblr.expression.runtime.v3";

struct BetweenRowEvidence {
  std::string_view surface_id;
  std::string_view canonical_name;
};

constexpr std::array<BetweenRowEvidence, 2> kBetweenRows{{
    {"SBSQL-47FC806CA045", "BETWEEN"},
    {"SBSQL-CF2DB952D802", "sb.special.between"},
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
  session.session_uuid = "019f0000-0000-7000-8000-000000025101";
  session.connection_uuid = "019f0000-0000-7000-8000-000000025102";
  session.database_uuid = "019f0000-0000-7000-8000-000000025103";
  session.dialect_profile_uuid = "sbsql_v3";
  session.catalog_epoch = 51;
  session.security_policy_epoch = 52;
  session.descriptor_epoch = 53;
  return session;
}

ParserConfig ParserConfigForTest() {
  ParserConfig config;
  config.probe_mode = true;
  config.server_endpoint = "sb_server_between_route";
  config.parser_uuid = "019f0000-0000-7000-8000-000000025104";
  config.bundle_contract_id = "sbp_sbsql@between-route-test";
  config.build_id = "sbsql-between-route-test";
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
  for (const auto& row : kBetweenRows) {
    const auto* registry_row = FindGeneratedSurfaceRegistryRowById(row.surface_id);
    Require(registry_row != nullptr, "BETWEEN generated registry row missing");
    Require(registry_row->canonical_name == row.canonical_name,
            "BETWEEN generated registry canonical name drifted");
    Require(registry_row->surface_kind == "function",
            "BETWEEN generated registry kind drifted");
    Require(registry_row->family == "expression_runtime",
            "BETWEEN generated registry family drifted");
    Require(registry_row->source_status == "native_now",
            "BETWEEN generated registry status drifted");
    Require(registry_row->cluster_scope == "noncluster_or_profile_scoped",
            "BETWEEN generated registry cluster scope drifted");
    Require(registry_row->sblr_operation_family == kExpressionFamily,
            "BETWEEN generated registry SBLR family drifted");
    Require(registry_row->parser_handler_key == "parser.expression_runtime.function",
            "BETWEEN generated registry parser handler drifted");
    Require(registry_row->lowering_handler_key == "lowering.expression_runtime.function",
            "BETWEEN generated registry lowering handler drifted");
    Require(registry_row->server_admission_key == "server.admission.sblr_expression_runtime_v3",
            "BETWEEN generated registry server admission drifted");
    Require(registry_row->engine_rule_key == "engine.rule.sblr_expression_runtime_v3",
            "BETWEEN generated registry engine rule drifted");
  }
}

void RequireExactLowering(const PipelineArtifacts& artifacts) {
  Require(!artifacts.cst.messages.has_errors(), "BETWEEN CST failed");
  Require(!artifacts.ast.messages.has_errors(), "BETWEEN AST failed");
  Require(artifacts.bound.bound, "BETWEEN bind failed");
  Require(artifacts.verifier.admitted, "BETWEEN verifier rejected exact route");
  Require(artifacts.envelope.operation_family == kQueryFamily,
          "BETWEEN query operation family mismatch");
  Require(artifacts.envelope.sblr_operation_key == kQueryFamily,
          "BETWEEN query SBLR operation key mismatch");
  Require(artifacts.envelope.operation_id == kOperationId,
          "BETWEEN operation id mismatch");
  Require(artifacts.envelope.engine_api_operation_id == kOperationId,
          "BETWEEN engine API operation id mismatch");
  Require(artifacts.envelope.sblr_opcode == kOpcode,
          "BETWEEN SBLR opcode mismatch");
  Require(HasValue(artifacts.envelope.required_authority_steps,
                   "authority.engine.query_projection_api_required"),
          "BETWEEN engine projection authority step missing");
  Require(HasValue(artifacts.envelope.required_authority_steps,
                   "authority.server.transaction_context_required"),
          "BETWEEN route must require transaction context through projection authority");
  Require(HasValue(artifacts.envelope.required_authority_steps,
                   "authority.parser.no_sql_text_execution"),
          "BETWEEN parser no-SQL-execution authority step missing");
  Require(!artifacts.envelope.parser_executes_sql,
          "BETWEEN lowering allowed parser SQL execution");
  Require(!artifacts.envelope.real_file_effects,
          "BETWEEN lowering allowed reference/file effects");
  Require(Contains(artifacts.envelope.payload, "\"query_envelope_kind\":\"scalar_projection\""),
          "BETWEEN payload missing scalar projection envelope kind");
  Require(Contains(artifacts.envelope.payload, "\"projection_0_expr_kind\":\"special_form\""),
          "BETWEEN payload missing special-form expression kind");
  Require(Contains(artifacts.envelope.payload, "\"projection_0_expr_opcode\":\"SBLR_SPECIAL_FORM_CALL\""),
          "BETWEEN payload missing special-form SBLR opcode");
  Require(Contains(artifacts.envelope.payload, "\"projection_0_function_id\":\"sb.special.between\""),
          "BETWEEN payload missing canonical function id");
  Require(Contains(artifacts.envelope.payload, "\"projection_0_special_form_id\":\"sb.special.between\""),
          "BETWEEN payload missing special form id");
  Require(Contains(artifacts.envelope.payload, "\"projection_0_sblr_binding\":\"sblr.expr.special_between.v3\""),
          "BETWEEN payload missing SBLR binding id");
  Require(Contains(artifacts.envelope.payload, "\"projection_0_engine_entrypoint\":\"special_between\""),
          "BETWEEN payload missing engine entrypoint id");
  for (const auto& row : kBetweenRows) {
    Require(Contains(artifacts.envelope.payload, row.surface_id),
            "BETWEEN payload missing row-identifiable surface evidence");
  }
  Require(Contains(artifacts.envelope.payload, "\"projection_0_arg_0_value\":\"5\""),
          "BETWEEN payload missing value operand");
  Require(Contains(artifacts.envelope.payload, "\"projection_0_arg_1_value\":\"1\""),
          "BETWEEN payload missing lower operand");
  Require(Contains(artifacts.envelope.payload, "\"projection_0_arg_2_value\":\"9\""),
          "BETWEEN payload missing upper operand");
  Require(Contains(artifacts.envelope.payload, "\"sql_text_included\":false"),
          "BETWEEN payload did not prove no SQL text authority");
  Require(!Contains(artifacts.envelope.payload, "reference"),
          "BETWEEN payload carried reference authority");
  Require(!Contains(artifacts.envelope.payload, "WAL") &&
              !Contains(artifacts.envelope.payload, "wal") &&
              !Contains(artifacts.envelope.payload, "recovery"),
          "BETWEEN payload carried WAL/recovery authority");
}

void RequireServerAdmission(const SblrEnvelope& envelope) {
  const auto admission = scratchbird::server::AdmitServerSblrEnvelope(
      scratchbird::server::ServerSblrAdmissionRequest{envelope.payload, false});
  Require(admission.admitted, "server admission rejected BETWEEN exact route");
  Require(admission.requires_public_abi_dispatch,
          "server admission did not require public ABI dispatch for BETWEEN");
  Require(admission.operation_id == kOperationId,
          "server admission BETWEEN operation id mismatch");
  Require(admission.operation_family == kQueryFamily,
          "server admission BETWEEN operation family mismatch");
}

api::EngineRequestContext EngineContext() {
  api::EngineRequestContext context;
  context.request_id = "sbsql-between-exact-route";
  context.database_uuid.canonical = "019f0000-0000-7000-8000-000000025201";
  context.node_uuid.canonical = "019f0000-0000-7000-8000-000000025202";
  context.session_uuid.canonical = "019f0000-0000-7000-8000-000000025203";
  context.principal_uuid.canonical = "019f0000-0000-7000-8000-000000025204";
  context.transaction_uuid.canonical = "019f0000-0000-7000-8000-000000025205";
  context.statement_uuid.canonical = "019f0000-0000-7000-8000-000000025206";
  context.current_schema_uuid.canonical = "019f0000-0000-7000-8000-000000025207";
  context.current_role_uuid.canonical = "019f0000-0000-7000-8000-000000025208";
  context.local_transaction_id = 91;
  context.security_context_present = true;
  context.trace_tags.push_back("right:QUERY_PROJECTION_TEST");
  context.trace_tags.push_back("sbsql_surface_id:SBSQL-47FC806CA045");
  context.trace_tags.push_back("sbsql_surface_id:SBSQL-CF2DB952D802");
  return context;
}

sblr::SblrOperationEnvelope EngineEnvelope() {
  auto envelope = sblr::MakeSblrEnvelope(std::string(kOperationId),
                                         std::string(kOpcode),
                                         "trace.between.exact_route.SBSQL-47FC806CA045");
  envelope.requires_security_context = true;
  envelope.requires_transaction_context = true;
  envelope.requires_cluster_authority = false;
  envelope.contains_sql_text = false;
  envelope.parser_resolved_names_to_uuids = true;
  envelope.operands.push_back({"text", "projection_count", "1"});
  envelope.operands.push_back({"text", "projection_0_name", "between_value"});
  envelope.operands.push_back({"text", "projection_0_expr_kind", "special_form"});
  envelope.operands.push_back({"text", "projection_0_type", "boolean"});
  envelope.operands.push_back({"text", "projection_0_function_id", "sb.special.between"});
  envelope.operands.push_back({"text", "projection_0_special_form_id", "sb.special.between"});
  envelope.operands.push_back({"text", "projection_0_sblr_binding", "sblr.expr.special_between.v3"});
  envelope.operands.push_back({"text", "projection_0_special_form_arg_count", "3"});
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
  envelope.operands.push_back({"text", "projection_0_arg_2_value", "9"});
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
  Require(result.accepted, "engine SBLR dispatch did not accept BETWEEN projection");
  Require(result.dispatched_to_api, "engine SBLR dispatch did not route to internal API");
  Require(result.api_result.ok, "EngineEvaluateProjection did not return success for BETWEEN");
  Require(result.api_result.operation_id == kOperationId,
          "EngineEvaluateProjection returned wrong operation id");
  Require(result.api_result.result_shape.rows.size() == 1,
          "BETWEEN result row count mismatch");
  Require(result.api_result.result_shape.rows.front().fields.size() == 1,
          "BETWEEN result field count mismatch");
  const auto& field = result.api_result.result_shape.rows.front().fields.front();
  Require(field.first == "between_value", "BETWEEN result field name mismatch");
  Require(field.second.descriptor.canonical_type_name == "boolean",
          "BETWEEN result descriptor mismatch");
  Require(!field.second.is_null, "BETWEEN result unexpectedly null");
  Require(field.second.encoded_value == "true" || field.second.encoded_value == "TRUE" ||
              field.second.encoded_value == "1",
          "BETWEEN result value mismatch");
  Require(HasEvidence(result.api_result, "special_form_runtime", "special_between"),
          "BETWEEN result missing special form runtime evidence");
  Require(HasEvidence(result.api_result, "operator_runtime", "op_ge"),
          "BETWEEN result missing lower-bound comparison evidence");
  Require(HasEvidence(result.api_result, "operator_runtime", "op_le"),
          "BETWEEN result missing upper-bound comparison evidence");
}

}  // namespace

int main() {
  RequireRegistryEvidence();
  const auto artifacts = RunPipeline();
  RequireExactLowering(artifacts);
  RequireServerAdmission(artifacts.envelope);
  RequireEngineDispatch();
  std::cout << "sbsql_between_exact_route_conformance=passed\n";
  return EXIT_SUCCESS;
}

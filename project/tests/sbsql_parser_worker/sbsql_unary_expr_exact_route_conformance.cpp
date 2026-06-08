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

#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace scratchbird::parser::sbsql;
namespace api = scratchbird::engine::internal_api;
namespace sblr = scratchbird::engine::sblr;

constexpr std::string_view kSql = "SELECT -5 AS negative_value;";
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
  session.session_uuid = "019f0000-0000-7000-8000-000000029501";
  session.connection_uuid = "019f0000-0000-7000-8000-000000029502";
  session.database_uuid = "019f0000-0000-7000-8000-000000029503";
  session.dialect_profile_uuid = "sbsql_v3";
  session.catalog_epoch = 99;
  session.security_policy_epoch = 100;
  session.descriptor_epoch = 101;
  return session;
}

ParserConfig ParserConfigForTest() {
  ParserConfig config;
  config.probe_mode = true;
  config.server_endpoint = "sb_server_unary_expr_route";
  config.parser_uuid = "019f0000-0000-7000-8000-000000029504";
  config.bundle_contract_id = "sbp_sbsql@unary-expr-route-test";
  config.build_id = "sbsql-unary-expr-route-test";
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
  const auto* row = FindGeneratedSurfaceRegistryRowById("SBSQL-7872FB756AE7");
  Require(row != nullptr, "unary_expr generated registry row missing");
  Require(row->canonical_name == "unary_expr",
          "unary_expr generated registry canonical name drifted");
  Require(row->surface_kind == "grammar_production",
          "unary_expr generated registry kind drifted");
  Require(row->family == "general", "unary_expr generated registry family drifted");
  Require(row->source_status == "native_now",
          "unary_expr generated registry status drifted");
  Require(row->cluster_scope == "noncluster_or_profile_scoped",
          "unary_expr generated registry cluster scope drifted");
  Require(row->sblr_operation_family == "sblr.general.operation.v3",
          "unary_expr generated registry SBLR family drifted");

  const auto* operator_token = FindGeneratedSurfaceRegistryRowById("SBSQL-062A23531925");
  Require(operator_token != nullptr, "operator_token generated registry row missing");
  Require(operator_token->canonical_name == "operator_token",
          "operator_token generated registry canonical name drifted");
  Require(operator_token->source_status == "native_now",
          "operator_token generated registry status drifted");

  const auto* unary_minus = FindGeneratedSurfaceRegistryRowById("SBSQL-276FACC8D892");
  Require(unary_minus != nullptr, "sb.operator.unary_minus generated registry row missing");
  Require(unary_minus->canonical_name == "sb.operator.unary_minus",
          "sb.operator.unary_minus generated registry canonical name drifted");
  Require(unary_minus->surface_kind == "function",
          "sb.operator.unary_minus generated registry kind drifted");
  Require(unary_minus->source_status == "native_now",
          "sb.operator.unary_minus generated registry status drifted");
  Require(unary_minus->sblr_operation_family == "sblr.expression.runtime.v3",
          "sb.operator.unary_minus generated registry SBLR family drifted");
}

void RequireExactLowering(const PipelineArtifacts& artifacts) {
  Require(!artifacts.cst.messages.has_errors(), "unary_expr CST failed");
  Require(!artifacts.ast.messages.has_errors(), "unary_expr AST failed");
  Require(artifacts.bound.bound, "unary_expr bind failed");
  if (!artifacts.verifier.admitted) {
    std::cerr << RenderMessageVectorSet(artifacts.verifier.messages);
  }
  Require(artifacts.verifier.admitted, "unary_expr verifier rejected exact route");
  Require(artifacts.envelope.operation_family == kQueryFamily,
          "unary_expr query operation family mismatch");
  Require(artifacts.envelope.operation_id == kOperationId,
          "unary_expr operation id mismatch");
  Require(artifacts.envelope.sblr_opcode == kOpcode,
          "unary_expr SBLR opcode mismatch");
  Require(HasValue(artifacts.envelope.required_authority_steps,
                   "authority.parser.no_sql_text_execution"),
          "unary_expr parser no-SQL-execution authority step missing");
  Require(!artifacts.envelope.parser_executes_sql,
          "unary_expr lowering allowed parser SQL execution");
  Require(Contains(artifacts.envelope.payload, "\"projection_0_expr_kind\":\"operator\""),
          "unary_expr payload missing operator expression kind");
  Require(Contains(artifacts.envelope.payload, "\"projection_0_expr_opcode\":\"SBLR_OPERATOR_CALL\""),
          "unary_expr payload missing operator opcode");
  Require(Contains(artifacts.envelope.payload, "\"projection_0_operator_id\":\"op_unary_minus\""),
          "unary_expr payload missing unary-minus operator id");
  Require(Contains(artifacts.envelope.payload,
                   "\"projection_0_canonical_operator_id\":\"sb.operator.unary_minus\""),
          "unary_expr payload missing canonical unary-minus operator id");
  Require(Contains(artifacts.envelope.payload, "SBSQL-7872FB756AE7"),
          "unary_expr payload missing row-identifiable surface evidence");
  Require(Contains(artifacts.envelope.payload, "SBSQL-062A23531925"),
          "unary_expr payload missing operator_token surface evidence");
  Require(Contains(artifacts.envelope.payload, "\"projection_0_operator_arg_count\":\"1\""),
          "unary_expr payload missing unary operand count");
  Require(Contains(artifacts.envelope.payload, "\"projection_0_arg_0_value\":\"5\""),
          "unary_expr payload missing positive literal operand");
  Require(Contains(artifacts.envelope.payload, "\"sql_text_included\":false"),
          "unary_expr payload did not prove no SQL text authority");
  Require(!Contains(artifacts.envelope.payload, "WAL") &&
              !Contains(artifacts.envelope.payload, "wal") &&
              !Contains(artifacts.envelope.payload, "recovery"),
          "unary_expr payload carried WAL/recovery authority");
}

void RequireServerAdmission(const SblrEnvelope& envelope) {
  const auto admission = scratchbird::server::AdmitServerSblrEnvelope(
      scratchbird::server::ServerSblrAdmissionRequest{envelope.payload, false});
  Require(admission.admitted, "server admission rejected unary_expr exact route");
  Require(admission.requires_public_abi_dispatch,
          "server admission did not require public ABI dispatch for unary_expr");
  Require(admission.operation_id == kOperationId,
          "server admission unary_expr operation id mismatch");
}

api::EngineRequestContext EngineContext() {
  api::EngineRequestContext context;
  context.request_id = "sbsql-unary-expr-exact-route";
  context.database_uuid.canonical = "019f0000-0000-7000-8000-000000029601";
  context.node_uuid.canonical = "019f0000-0000-7000-8000-000000029602";
  context.session_uuid.canonical = "019f0000-0000-7000-8000-000000029603";
  context.principal_uuid.canonical = "019f0000-0000-7000-8000-000000029604";
  context.transaction_uuid.canonical = "019f0000-0000-7000-8000-000000029605";
  context.statement_uuid.canonical = "019f0000-0000-7000-8000-000000029606";
  context.current_schema_uuid.canonical = "019f0000-0000-7000-8000-000000029607";
  context.current_role_uuid.canonical = "019f0000-0000-7000-8000-000000029608";
  context.local_transaction_id = 99;
  context.security_context_present = true;
  context.trace_tags.push_back("right:QUERY_PROJECTION_TEST");
  context.trace_tags.push_back("sbsql_surface_id:SBSQL-7872FB756AE7");
  context.trace_tags.push_back("sbsql_surface_id:SBSQL-062A23531925");
  context.trace_tags.push_back("sbsql_surface_id:SBSQL-276FACC8D892");
  return context;
}

sblr::SblrOperationEnvelope EngineEnvelope() {
  auto envelope = sblr::MakeSblrEnvelope(std::string(kOperationId),
                                         std::string(kOpcode),
                                         "trace.unary_expr.exact_route.SBSQL-7872FB756AE7");
  envelope.requires_security_context = true;
  envelope.requires_transaction_context = true;
  envelope.requires_cluster_authority = false;
  envelope.contains_sql_text = false;
  envelope.parser_resolved_names_to_uuids = true;
  envelope.operands.push_back({"text", "projection_count", "1"});
  envelope.operands.push_back({"text", "projection_0_name", "negative_value"});
  envelope.operands.push_back({"text", "projection_0_expr_kind", "operator"});
  envelope.operands.push_back({"text", "projection_0_type", "int64"});
  envelope.operands.push_back({"text", "projection_0_operator_id", "op_unary_minus"});
  envelope.operands.push_back({"text", "projection_0_canonical_operator_id",
                               "sb.operator.unary_minus"});
  envelope.operands.push_back({"text", "projection_0_operator_arg_count", "1"});
  envelope.operands.push_back({"text", "projection_0_arg_0_expr_kind", "literal"});
  envelope.operands.push_back({"text", "projection_0_arg_0_type", "bigint"});
  envelope.operands.push_back({"text", "projection_0_arg_0_value", "5"});
  envelope.operands.push_back({"text", "projection_0_arg_0_is_null", "false"});
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
  Require(result.accepted, "engine SBLR dispatch did not accept unary_expr projection");
  Require(result.dispatched_to_api, "engine SBLR dispatch did not route to internal API");
  Require(result.api_result.ok, "EngineEvaluateProjection did not return success for unary_expr");
  Require(result.api_result.result_shape.rows.size() == 1,
          "unary_expr result row count mismatch");
  const auto& field = result.api_result.result_shape.rows.front().fields.front();
  Require(field.first == "negative_value", "unary_expr result field name mismatch");
  Require(field.second.descriptor.canonical_type_name == "int64",
          "unary_expr result descriptor mismatch");
  Require(!field.second.is_null, "unary_expr result unexpectedly null");
  Require(field.second.encoded_value == "-5", "unary_expr result value mismatch");
  Require(HasEvidence(result.api_result, "operator_runtime", "op_unary_minus"),
          "unary_expr result missing op_unary_minus runtime evidence");
}

}  // namespace

int main() {
  RequireRegistryEvidence();
  const auto artifacts = RunPipeline();
  RequireExactLowering(artifacts);
  RequireServerAdmission(artifacts.envelope);
  RequireEngineDispatch();
  std::cout << "sbsql_unary_expr_exact_route_conformance=passed\n";
  return EXIT_SUCCESS;
}

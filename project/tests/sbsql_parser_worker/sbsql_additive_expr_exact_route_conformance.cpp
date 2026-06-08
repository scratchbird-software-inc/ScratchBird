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

constexpr std::string_view kSql = "SELECT 10 + 5 AS sum_value;";
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
  session.session_uuid = "019f0000-0000-7000-8000-000000029301";
  session.connection_uuid = "019f0000-0000-7000-8000-000000029302";
  session.database_uuid = "019f0000-0000-7000-8000-000000029303";
  session.dialect_profile_uuid = "sbsql_v3";
  session.catalog_epoch = 96;
  session.security_policy_epoch = 97;
  session.descriptor_epoch = 98;
  return session;
}

ParserConfig ParserConfigForTest() {
  ParserConfig config;
  config.probe_mode = true;
  config.server_endpoint = "sb_server_additive_expr_route";
  config.parser_uuid = "019f0000-0000-7000-8000-000000029304";
  config.bundle_contract_id = "sbp_sbsql@additive-expr-route-test";
  config.build_id = "sbsql-additive-expr-route-test";
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
  const auto* row = FindGeneratedSurfaceRegistryRowById("SBSQL-B1DCA7AA5204");
  Require(row != nullptr, "additive_expr generated registry row missing");
  Require(row->canonical_name == "additive_expr",
          "additive_expr generated registry canonical name drifted");
  Require(row->surface_kind == "grammar_production",
          "additive_expr generated registry kind drifted");
  Require(row->family == "general", "additive_expr generated registry family drifted");
  Require(row->source_status == "native_now",
          "additive_expr generated registry status drifted");
  Require(row->cluster_scope == "noncluster_or_profile_scoped",
          "additive_expr generated registry cluster scope drifted");
  Require(row->sblr_operation_family == "sblr.general.operation.v3",
          "additive_expr generated registry SBLR family drifted");
  Require(row->parser_handler_key == "parser.grammar_ast",
          "additive_expr generated registry parser handler drifted");
  Require(row->lowering_handler_key == "lowering.sblr_family.sblr_general_operation_v3",
          "additive_expr generated registry lowering handler drifted");
  Require(row->server_admission_key == "server.admission.sblr_general_operation_v3",
          "additive_expr generated registry server admission drifted");
  Require(row->engine_rule_key == "engine.rule.sblr_general_operation_v3",
          "additive_expr generated registry engine rule drifted");

  const auto* operator_token = FindGeneratedSurfaceRegistryRowById("SBSQL-062A23531925");
  Require(operator_token != nullptr, "operator_token generated registry row missing");
  Require(operator_token->canonical_name == "operator_token",
          "operator_token generated registry canonical name drifted");
  Require(operator_token->source_status == "native_now",
          "operator_token generated registry status drifted");

  const auto* add = FindGeneratedSurfaceRegistryRowById("SBSQL-FFE9CCC014E1");
  Require(add != nullptr, "sb.operator.add generated registry row missing");
  Require(add->canonical_name == "sb.operator.add",
          "sb.operator.add generated registry canonical name drifted");
  Require(add->surface_kind == "function", "sb.operator.add generated registry kind drifted");
  Require(add->source_status == "native_now", "sb.operator.add generated registry status drifted");
  Require(add->sblr_operation_family == "sblr.expression.runtime.v3",
          "sb.operator.add generated registry SBLR family drifted");
}

void RequireExactLowering(const PipelineArtifacts& artifacts) {
  Require(!artifacts.cst.messages.has_errors(), "additive_expr CST failed");
  Require(!artifacts.ast.messages.has_errors(), "additive_expr AST failed");
  Require(artifacts.bound.bound, "additive_expr bind failed");
  if (!artifacts.verifier.admitted) {
    std::cerr << RenderMessageVectorSet(artifacts.verifier.messages);
  }
  Require(artifacts.verifier.admitted, "additive_expr verifier rejected exact route");
  Require(artifacts.envelope.operation_family == kQueryFamily,
          "additive_expr query operation family mismatch");
  Require(artifacts.envelope.operation_id == kOperationId,
          "additive_expr operation id mismatch");
  Require(artifacts.envelope.sblr_opcode == kOpcode,
          "additive_expr SBLR opcode mismatch");
  Require(HasValue(artifacts.envelope.required_authority_steps,
                   "authority.parser.no_sql_text_execution"),
          "additive_expr parser no-SQL-execution authority step missing");
  Require(!artifacts.envelope.parser_executes_sql,
          "additive_expr lowering allowed parser SQL execution");
  Require(Contains(artifacts.envelope.payload, "\"projection_0_expr_kind\":\"operator\""),
          "additive_expr payload missing operator expression kind");
  Require(Contains(artifacts.envelope.payload, "\"projection_0_expr_opcode\":\"SBLR_OPERATOR_CALL\""),
          "additive_expr payload missing operator opcode");
  Require(Contains(artifacts.envelope.payload, "\"projection_0_operator_id\":\"op_add\""),
          "additive_expr payload missing add operator id");
  Require(Contains(artifacts.envelope.payload,
                   "\"projection_0_canonical_operator_id\":\"sb.operator.add\""),
          "additive_expr payload missing canonical add operator id");
  Require(Contains(artifacts.envelope.payload, "SBSQL-B1DCA7AA5204"),
          "additive_expr payload missing row-identifiable surface evidence");
  Require(Contains(artifacts.envelope.payload, "SBSQL-062A23531925"),
          "additive_expr payload missing operator_token surface evidence");
  Require(Contains(artifacts.envelope.payload, "\"projection_0_arg_0_value\":\"10\""),
          "additive_expr payload missing left literal operand");
  Require(Contains(artifacts.envelope.payload, "\"projection_0_arg_1_value\":\"5\""),
          "additive_expr payload missing right literal operand");
  Require(Contains(artifacts.envelope.payload, "\"sql_text_included\":false"),
          "additive_expr payload did not prove no SQL text authority");
  Require(!Contains(artifacts.envelope.payload, "WAL") &&
              !Contains(artifacts.envelope.payload, "wal") &&
              !Contains(artifacts.envelope.payload, "recovery"),
          "additive_expr payload carried WAL/recovery authority");
}

void RequireServerAdmission(const SblrEnvelope& envelope) {
  const auto admission = scratchbird::server::AdmitServerSblrEnvelope(
      scratchbird::server::ServerSblrAdmissionRequest{envelope.payload, false});
  Require(admission.admitted, "server admission rejected additive_expr exact route");
  Require(admission.requires_public_abi_dispatch,
          "server admission did not require public ABI dispatch for additive_expr");
  Require(admission.operation_id == kOperationId,
          "server admission additive_expr operation id mismatch");
}

api::EngineRequestContext EngineContext() {
  api::EngineRequestContext context;
  context.request_id = "sbsql-additive-expr-exact-route";
  context.database_uuid.canonical = "019f0000-0000-7000-8000-000000029401";
  context.node_uuid.canonical = "019f0000-0000-7000-8000-000000029402";
  context.session_uuid.canonical = "019f0000-0000-7000-8000-000000029403";
  context.principal_uuid.canonical = "019f0000-0000-7000-8000-000000029404";
  context.transaction_uuid.canonical = "019f0000-0000-7000-8000-000000029405";
  context.statement_uuid.canonical = "019f0000-0000-7000-8000-000000029406";
  context.current_schema_uuid.canonical = "019f0000-0000-7000-8000-000000029407";
  context.current_role_uuid.canonical = "019f0000-0000-7000-8000-000000029408";
  context.local_transaction_id = 96;
  context.security_context_present = true;
  context.trace_tags.push_back("right:QUERY_PROJECTION_TEST");
  context.trace_tags.push_back("sbsql_surface_id:SBSQL-B1DCA7AA5204");
  context.trace_tags.push_back("sbsql_surface_id:SBSQL-062A23531925");
  context.trace_tags.push_back("sbsql_surface_id:SBSQL-FFE9CCC014E1");
  return context;
}

sblr::SblrOperationEnvelope EngineEnvelope() {
  auto envelope = sblr::MakeSblrEnvelope(std::string(kOperationId),
                                         std::string(kOpcode),
                                         "trace.additive_expr.exact_route.SBSQL-B1DCA7AA5204");
  envelope.requires_security_context = true;
  envelope.requires_transaction_context = true;
  envelope.requires_cluster_authority = false;
  envelope.contains_sql_text = false;
  envelope.parser_resolved_names_to_uuids = true;
  envelope.operands.push_back({"text", "projection_count", "1"});
  envelope.operands.push_back({"text", "projection_0_name", "sum_value"});
  envelope.operands.push_back({"text", "projection_0_expr_kind", "operator"});
  envelope.operands.push_back({"text", "projection_0_type", "bigint"});
  envelope.operands.push_back({"text", "projection_0_operator_id", "op_add"});
  envelope.operands.push_back({"text", "projection_0_canonical_operator_id", "sb.operator.add"});
  envelope.operands.push_back({"text", "projection_0_operator_arg_count", "2"});
  envelope.operands.push_back({"text", "projection_0_arg_0_expr_kind", "literal"});
  envelope.operands.push_back({"text", "projection_0_arg_0_type", "bigint"});
  envelope.operands.push_back({"text", "projection_0_arg_0_value", "10"});
  envelope.operands.push_back({"text", "projection_0_arg_0_is_null", "false"});
  envelope.operands.push_back({"text", "projection_0_arg_1_expr_kind", "literal"});
  envelope.operands.push_back({"text", "projection_0_arg_1_type", "bigint"});
  envelope.operands.push_back({"text", "projection_0_arg_1_value", "5"});
  envelope.operands.push_back({"text", "projection_0_arg_1_is_null", "false"});
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
  Require(result.accepted, "engine SBLR dispatch did not accept additive_expr projection");
  Require(result.dispatched_to_api, "engine SBLR dispatch did not route to internal API");
  Require(result.api_result.ok, "EngineEvaluateProjection did not return success for additive_expr");
  Require(result.api_result.result_shape.rows.size() == 1,
          "additive_expr result row count mismatch");
  const auto& field = result.api_result.result_shape.rows.front().fields.front();
  Require(field.first == "sum_value", "additive_expr result field name mismatch");
  Require(field.second.descriptor.canonical_type_name == "int64",
          "additive_expr result descriptor mismatch");
  Require(!field.second.is_null, "additive_expr result unexpectedly null");
  Require(field.second.encoded_value == "15", "additive_expr result value mismatch");
  Require(HasEvidence(result.api_result, "operator_runtime", "op_add"),
          "additive_expr result missing op_add runtime evidence");
}

}  // namespace

int main() {
  RequireRegistryEvidence();
  const auto artifacts = RunPipeline();
  RequireExactLowering(artifacts);
  RequireServerAdmission(artifacts.envelope);
  RequireEngineDispatch();
  std::cout << "sbsql_additive_expr_exact_route_conformance=passed\n";
  return EXIT_SUCCESS;
}

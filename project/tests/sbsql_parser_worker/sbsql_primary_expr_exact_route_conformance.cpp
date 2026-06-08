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

constexpr std::string_view kSql = "SELECT 1 AS primary_value;";
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
  session.session_uuid = "019f0000-0000-7000-8000-000000029701";
  session.connection_uuid = "019f0000-0000-7000-8000-000000029702";
  session.database_uuid = "019f0000-0000-7000-8000-000000029703";
  session.dialect_profile_uuid = "sbsql_v3";
  session.catalog_epoch = 102;
  session.security_policy_epoch = 103;
  session.descriptor_epoch = 104;
  return session;
}

ParserConfig ParserConfigForTest() {
  ParserConfig config;
  config.probe_mode = true;
  config.server_endpoint = "sb_server_primary_expr_route";
  config.parser_uuid = "019f0000-0000-7000-8000-000000029704";
  config.bundle_contract_id = "sbp_sbsql@primary-expr-route-test";
  config.build_id = "sbsql-primary-expr-route-test";
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
  const auto* row = FindGeneratedSurfaceRegistryRowById("SBSQL-53D45E6D9459");
  Require(row != nullptr, "primary_expr generated registry row missing");
  Require(row->canonical_name == "primary_expr",
          "primary_expr generated registry canonical name drifted");
  Require(row->surface_kind == "grammar_production",
          "primary_expr generated registry kind drifted");
  Require(row->family == "general", "primary_expr generated registry family drifted");
  Require(row->source_status == "native_now",
          "primary_expr generated registry status drifted");
  Require(row->cluster_scope == "noncluster_or_profile_scoped",
          "primary_expr generated registry cluster scope drifted");
  Require(row->sblr_operation_family == "sblr.general.operation.v3",
          "primary_expr generated registry SBLR family drifted");
}

void RequireExactLowering(const PipelineArtifacts& artifacts) {
  Require(!artifacts.cst.messages.has_errors(), "primary_expr CST failed");
  Require(!artifacts.ast.messages.has_errors(), "primary_expr AST failed");
  Require(artifacts.bound.bound, "primary_expr bind failed");
  if (!artifacts.verifier.admitted) {
    std::cerr << RenderMessageVectorSet(artifacts.verifier.messages);
  }
  Require(artifacts.verifier.admitted, "primary_expr verifier rejected exact route");
  Require(artifacts.envelope.operation_family == kQueryFamily,
          "primary_expr query operation family mismatch");
  Require(artifacts.envelope.operation_id == kOperationId,
          "primary_expr operation id mismatch");
  Require(artifacts.envelope.sblr_opcode == kOpcode,
          "primary_expr SBLR opcode mismatch");
  Require(HasValue(artifacts.envelope.required_authority_steps,
                   "authority.parser.no_sql_text_execution"),
          "primary_expr parser no-SQL-execution authority step missing");
  Require(!artifacts.envelope.parser_executes_sql,
          "primary_expr lowering allowed parser SQL execution");
  Require(Contains(artifacts.envelope.payload, "\"projection_0_expr_kind\":\"literal\""),
          "primary_expr payload missing literal expression kind");
  Require(Contains(artifacts.envelope.payload, "\"projection_0_type\":\"bigint\""),
          "primary_expr payload missing literal type");
  Require(Contains(artifacts.envelope.payload, "\"projection_0_value\":\"1\""),
          "primary_expr payload missing literal value");
  Require(Contains(artifacts.envelope.payload, "SBSQL-53D45E6D9459"),
          "primary_expr payload missing row-identifiable surface evidence");
  Require(Contains(artifacts.envelope.payload, "\"sql_text_included\":false"),
          "primary_expr payload did not prove no SQL text authority");
  Require(!Contains(artifacts.envelope.payload, "WAL") &&
              !Contains(artifacts.envelope.payload, "wal") &&
              !Contains(artifacts.envelope.payload, "recovery"),
          "primary_expr payload carried WAL/recovery authority");
}

void RequireServerAdmission(const SblrEnvelope& envelope) {
  const auto admission = scratchbird::server::AdmitServerSblrEnvelope(
      scratchbird::server::ServerSblrAdmissionRequest{envelope.payload, false});
  Require(admission.admitted, "server admission rejected primary_expr exact route");
  Require(admission.requires_public_abi_dispatch,
          "server admission did not require public ABI dispatch for primary_expr");
  Require(admission.operation_id == kOperationId,
          "server admission primary_expr operation id mismatch");
}

api::EngineRequestContext EngineContext() {
  api::EngineRequestContext context;
  context.request_id = "sbsql-primary-expr-exact-route";
  context.database_uuid.canonical = "019f0000-0000-7000-8000-000000029801";
  context.node_uuid.canonical = "019f0000-0000-7000-8000-000000029802";
  context.session_uuid.canonical = "019f0000-0000-7000-8000-000000029803";
  context.principal_uuid.canonical = "019f0000-0000-7000-8000-000000029804";
  context.transaction_uuid.canonical = "019f0000-0000-7000-8000-000000029805";
  context.statement_uuid.canonical = "019f0000-0000-7000-8000-000000029806";
  context.current_schema_uuid.canonical = "019f0000-0000-7000-8000-000000029807";
  context.current_role_uuid.canonical = "019f0000-0000-7000-8000-000000029808";
  context.local_transaction_id = 102;
  context.security_context_present = true;
  context.trace_tags.push_back("right:QUERY_PROJECTION_TEST");
  context.trace_tags.push_back("sbsql_surface_id:SBSQL-53D45E6D9459");
  return context;
}

sblr::SblrOperationEnvelope EngineEnvelope() {
  auto envelope = sblr::MakeSblrEnvelope(std::string(kOperationId),
                                         std::string(kOpcode),
                                         "trace.primary_expr.exact_route.SBSQL-53D45E6D9459");
  envelope.requires_security_context = true;
  envelope.requires_transaction_context = true;
  envelope.requires_cluster_authority = false;
  envelope.contains_sql_text = false;
  envelope.parser_resolved_names_to_uuids = true;
  envelope.operands.push_back({"text", "projection_count", "1"});
  envelope.operands.push_back({"text", "projection_0_name", "primary_value"});
  envelope.operands.push_back({"text", "projection_0_expr_kind", "literal"});
  envelope.operands.push_back({"text", "projection_0_type", "bigint"});
  envelope.operands.push_back({"text", "projection_0_value", "1"});
  envelope.operands.push_back({"text", "projection_0_is_null", "false"});
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
  Require(result.accepted, "engine SBLR dispatch did not accept primary_expr projection");
  Require(result.dispatched_to_api, "engine SBLR dispatch did not route to internal API");
  Require(result.api_result.ok, "EngineEvaluateProjection did not return success for primary_expr");
  Require(result.api_result.result_shape.rows.size() == 1,
          "primary_expr result row count mismatch");
  const auto& field = result.api_result.result_shape.rows.front().fields.front();
  Require(field.first == "primary_value", "primary_expr result field name mismatch");
  Require(field.second.descriptor.canonical_type_name == "bigint",
          "primary_expr result descriptor mismatch");
  Require(!field.second.is_null, "primary_expr result unexpectedly null");
  Require(field.second.encoded_value == "1", "primary_expr result value mismatch");
  Require(HasEvidence(result.api_result,
                      "query_projection",
                      "constant_projection_engine_evaluated"),
          "primary_expr result missing literal projection runtime evidence");
}

}  // namespace

int main() {
  RequireRegistryEvidence();
  const auto artifacts = RunPipeline();
  RequireExactLowering(artifacts);
  RequireServerAdmission(artifacts.envelope);
  RequireEngineDispatch();
  std::cout << "sbsql_primary_expr_exact_route_conformance=passed\n";
  return EXIT_SUCCESS;
}

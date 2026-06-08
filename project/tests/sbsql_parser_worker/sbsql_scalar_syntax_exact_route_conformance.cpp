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
#include <iterator>
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

struct SyntaxRow {
  std::string_view surface_id;
  std::string_view canonical_name;
  std::string_view surface_kind;
  std::string_view sblr_operation_family;
  std::string_view payload_marker;
};

constexpr SyntaxRow kRows[] = {
    {"SBSQL-C02360549248",
     "optional_expression_list",
     "grammar_production",
     "sblr.general.operation.v3",
     "SBSQL-C02360549248"},
    {"SBSQL-70470A026B74",
     "expression_list",
     "grammar_production",
     "sblr.general.operation.v3",
     "SBSQL-70470A026B74"},
    {"SBSQL-0CA86A310A96",
     "postfix_expr",
     "grammar_production",
     "sblr.general.operation.v3",
     "SBSQL-0CA86A310A96"},
    {"SBSQL-A71BB92503D1",
     "alias",
     "grammar_production",
     "sblr.general.operation.v3",
     "SBSQL-A71BB92503D1"},
    {"SBSQL-8FCBFAE9BB6E",
     "identifier",
     "grammar_production",
     "sblr.general.operation.v3",
     "SBSQL-8FCBFAE9BB6E"},
    {"SBSQL-12D6A4C483F0",
     "identifier_bare",
     "grammar_production",
     "sblr.general.operation.v3",
     "SBSQL-12D6A4C483F0"},
};

constexpr SyntaxRow kReservedKeywordIdentifierRows[] = {
    {"SBSQL-16C870E47238",
     "SBSQL.RESERVED_KEYWORD_AS_IDENTIFIER",
     "function",
     "sblr.expression.runtime.v3",
     "SBSQL-16C870E47238"},
};

constexpr SyntaxRow kDelimitedIdentifierRows[] = {
    {"SBSQL-52966F033E44",
     "identifier_delimited",
     "grammar_production",
     "sblr.query.relational.v3",
     "SBSQL-52966F033E44"},
    {"SBSQL-F2EFBBE3E8CF",
     "identifier_delimited",
     "function",
     "sblr.expression.runtime.v3",
     "SBSQL-F2EFBBE3E8CF"},
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

bool HasEvidence(const api::EngineApiResult& result,
                 std::string_view kind,
                 std::string_view id) {
  for (const auto& evidence : result.evidence) {
    if (evidence.evidence_kind == kind && evidence.evidence_id == id) return true;
  }
  return false;
}

std::vector<SyntaxRow> BaseRows() {
  return std::vector<SyntaxRow>(std::begin(kRows), std::end(kRows));
}

std::vector<SyntaxRow> AliasIdentifierRows() {
  return std::vector<SyntaxRow>(std::begin(kRows), std::begin(kRows) + 5);
}

SessionContext ParserSession() {
  SessionContext session;
  session.authenticated = true;
  session.session_uuid = "019f0000-0000-7000-8000-000000031101";
  session.connection_uuid = "019f0000-0000-7000-8000-000000031102";
  session.database_uuid = "019f0000-0000-7000-8000-000000031103";
  session.dialect_profile_uuid = "sbsql_v3";
  session.catalog_epoch = 205;
  session.security_policy_epoch = 206;
  session.descriptor_epoch = 207;
  return session;
}

ParserConfig ParserConfigForTest() {
  ParserConfig config;
  config.probe_mode = true;
  config.server_endpoint = "sb_server_scalar_syntax_route";
  config.parser_uuid = "019f0000-0000-7000-8000-000000031104";
  config.bundle_contract_id = "sbp_sbsql@scalar-syntax-route-test";
  config.build_id = "sbsql-scalar-syntax-route-test";
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

void RequireRegistryRowEvidence(const SyntaxRow& row) {
  const auto* registry_row = FindGeneratedSurfaceRegistryRowById(row.surface_id);
  Require(registry_row != nullptr, "scalar syntax generated registry row missing");
  Require(registry_row->canonical_name == row.canonical_name,
          "scalar syntax generated registry canonical name drifted");
  Require(registry_row->surface_kind == row.surface_kind,
          "scalar syntax generated registry kind drifted");
  Require(registry_row->source_status == "native_now",
          "scalar syntax generated registry status drifted");
  Require(registry_row->cluster_scope == "noncluster_or_profile_scoped",
          "scalar syntax generated registry cluster scope drifted");
  Require(registry_row->sblr_operation_family == row.sblr_operation_family,
          "scalar syntax generated registry SBLR family drifted");
}

void RequireRegistryEvidence() {
  for (const auto& row : kRows) RequireRegistryRowEvidence(row);
  for (const auto& row : kReservedKeywordIdentifierRows) RequireRegistryRowEvidence(row);
  for (const auto& row : kDelimitedIdentifierRows) RequireRegistryRowEvidence(row);
}

void RequireExactLowering(const PipelineArtifacts& artifacts,
                          std::string_view sql,
                          std::string_view expected_projection_name,
                          const std::vector<SyntaxRow>& required_rows) {
  Require(!artifacts.cst.messages.has_errors(), "scalar syntax CST failed");
  Require(!artifacts.ast.messages.has_errors(), "scalar syntax AST failed");
  Require(artifacts.bound.bound, "scalar syntax bind failed");
  if (!artifacts.verifier.admitted) {
    std::cerr << RenderMessageVectorSet(artifacts.verifier.messages);
  }
  Require(artifacts.verifier.admitted, "scalar syntax verifier rejected exact route");
  Require(artifacts.envelope.operation_family == kRouteFamily,
          "scalar syntax route operation family mismatch");
  if (artifacts.envelope.operation_id != kOperationId) {
    std::cerr << "scalar syntax operation id mismatch for SQL: " << sql
              << " observed=" << artifacts.envelope.operation_id
              << " payload=" << artifacts.envelope.payload << '\n';
  }
  Require(artifacts.envelope.operation_id == kOperationId,
          "scalar syntax operation id mismatch");
  Require(artifacts.envelope.sblr_opcode == kOpcode,
          "scalar syntax SBLR opcode mismatch");
  Require(HasValue(artifacts.envelope.required_authority_steps,
                   "authority.parser.no_sql_text_execution"),
          "scalar syntax parser no-SQL-execution authority step missing");
  Require(!artifacts.envelope.parser_executes_sql,
          "scalar syntax lowering allowed parser SQL execution");
  Require(Contains(artifacts.envelope.payload, "\"query_syntax_surface_ids\""),
          "scalar syntax payload missing query syntax surface list");
  Require(Contains(artifacts.envelope.payload, "\"projection_0_syntax_surface_ids\""),
          "scalar syntax payload missing projection syntax surface list");
  Require(Contains(artifacts.envelope.payload, "\"projection_0_expression_surface_ids\""),
          "scalar syntax payload missing expression surface list");
  Require(Contains(artifacts.envelope.payload,
                   std::string("\"projection_0_name\":\"") +
                       std::string(expected_projection_name) + "\""),
          "scalar syntax payload missing alias result label");
  Require(Contains(artifacts.envelope.payload, "\"projection_0_expr_kind\":\"literal\""),
          "scalar syntax payload missing literal expression kind");
  Require(Contains(artifacts.envelope.payload, "\"projection_0_value\":\"1\""),
          "scalar syntax payload missing literal value");
  for (const auto& row : required_rows) {
    Require(Contains(artifacts.envelope.payload, row.payload_marker),
            "scalar syntax payload missing row-identifiable marker");
  }
  Require(Contains(artifacts.envelope.payload, "\"sql_text_included\":false"),
          "scalar syntax payload did not prove no SQL text authority");
  Require(!Contains(artifacts.envelope.payload, "WAL") &&
              !Contains(artifacts.envelope.payload, "wal") &&
              !Contains(artifacts.envelope.payload, "recovery"),
          "scalar syntax payload carried WAL/recovery authority");
}

void RequireServerAdmission(const SblrEnvelope& envelope) {
  const auto admission = scratchbird::server::AdmitServerSblrEnvelope(
      scratchbird::server::ServerSblrAdmissionRequest{envelope.payload, false});
  Require(admission.admitted, "server admission rejected scalar syntax exact route");
  Require(admission.requires_public_abi_dispatch,
          "server admission did not require public ABI dispatch for scalar syntax");
  Require(admission.operation_id == kOperationId,
          "server admission scalar syntax operation id mismatch");
}

api::EngineRequestContext EngineContext() {
  api::EngineRequestContext context;
  context.request_id = "sbsql-scalar-syntax-exact-route";
  context.database_uuid.canonical = "019f0000-0000-7000-8000-000000031201";
  context.node_uuid.canonical = "019f0000-0000-7000-8000-000000031202";
  context.session_uuid.canonical = "019f0000-0000-7000-8000-000000031203";
  context.principal_uuid.canonical = "019f0000-0000-7000-8000-000000031204";
  context.transaction_uuid.canonical = "019f0000-0000-7000-8000-000000031205";
  context.statement_uuid.canonical = "019f0000-0000-7000-8000-000000031206";
  context.current_schema_uuid.canonical = "019f0000-0000-7000-8000-000000031207";
  context.current_role_uuid.canonical = "019f0000-0000-7000-8000-000000031208";
  context.local_transaction_id = 205;
  context.security_context_present = true;
  context.trace_tags.push_back("right:QUERY_PROJECTION_TEST");
  context.trace_tags.push_back("sbsql_surface_id:SBSQL-C02360549248");
  context.trace_tags.push_back("sbsql_surface_id:SBSQL-70470A026B74");
  context.trace_tags.push_back("sbsql_surface_id:SBSQL-0CA86A310A96");
  context.trace_tags.push_back("sbsql_surface_id:SBSQL-A71BB92503D1");
  context.trace_tags.push_back("sbsql_surface_id:SBSQL-8FCBFAE9BB6E");
  context.trace_tags.push_back("sbsql_surface_id:SBSQL-12D6A4C483F0");
  context.trace_tags.push_back("sbsql_surface_id:SBSQL-16C870E47238");
  context.trace_tags.push_back("sbsql_surface_id:SBSQL-52966F033E44");
  context.trace_tags.push_back("sbsql_surface_id:SBSQL-F2EFBBE3E8CF");
  return context;
}

sblr::SblrOperationEnvelope EngineEnvelope(std::string_view projection_name) {
  auto envelope = sblr::MakeSblrEnvelope(std::string(kOperationId),
                                         std::string(kOpcode),
                                         "trace.scalar_syntax.exact_route");
  envelope.requires_security_context = true;
  envelope.requires_transaction_context = true;
  envelope.requires_cluster_authority = false;
  envelope.contains_sql_text = false;
  envelope.parser_resolved_names_to_uuids = true;
  envelope.operands.push_back({"text", "projection_count", "1"});
  envelope.operands.push_back({"text", "projection_0_name", std::string(projection_name)});
  envelope.operands.push_back({"text", "projection_0_expr_kind", "literal"});
  envelope.operands.push_back({"text", "projection_0_type", "bigint"});
  envelope.operands.push_back({"text", "projection_0_value", "1"});
  envelope.operands.push_back({"text", "projection_0_is_null", "false"});
  return envelope;
}

void RequireEngineDispatch(std::string_view expected_projection_name) {
  const sblr::SblrDispatchRequest request{
      EngineContext(),
      EngineEnvelope(expected_projection_name),
      api::EngineApiRequest{}};
  const auto result = sblr::DispatchSblrOperation(request);
  for (const auto& diagnostic : result.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.message << '\n';
  }
  for (const auto& diagnostic : result.api_result.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.detail << '\n';
  }
  Require(result.envelope_validated, "engine SBLR envelope did not validate");
  Require(result.accepted, "engine SBLR dispatch did not accept scalar syntax projection");
  Require(result.dispatched_to_api, "engine SBLR dispatch did not route to internal API");
  Require(result.api_result.ok, "EngineEvaluateProjection did not return success for scalar syntax");
  Require(result.api_result.result_shape.rows.size() == 1,
          "scalar syntax result row count mismatch");
  const auto& field = result.api_result.result_shape.rows.front().fields.front();
  Require(field.first == expected_projection_name, "scalar syntax result field name mismatch");
  Require(field.second.descriptor.canonical_type_name == "bigint",
          "scalar syntax result descriptor mismatch");
  Require(!field.second.is_null, "scalar syntax result unexpectedly null");
  Require(field.second.encoded_value == "1", "scalar syntax result value mismatch");
  Require(HasEvidence(result.api_result,
                      "query_projection",
                      "constant_projection_engine_evaluated"),
          "scalar syntax result missing literal projection runtime evidence");
}

}  // namespace

int main() {
  RequireRegistryEvidence();
  {
    const auto artifacts = RunPipeline("SELECT 1 AS syntax_alias;");
    RequireExactLowering(artifacts, "SELECT 1 AS syntax_alias;", "syntax_alias", BaseRows());
    RequireServerAdmission(artifacts.envelope);
    RequireEngineDispatch("syntax_alias");
  }
  {
    const auto artifacts = RunPipeline("SELECT 1 AS WHERE;");
    auto rows = AliasIdentifierRows();
    rows.insert(rows.end(),
                std::begin(kReservedKeywordIdentifierRows),
                std::end(kReservedKeywordIdentifierRows));
    RequireExactLowering(artifacts,
                         "SELECT 1 AS WHERE;",
                         "WHERE",
                         rows);
    RequireServerAdmission(artifacts.envelope);
    RequireEngineDispatch("WHERE");
  }
  {
    const auto artifacts = RunPipeline("SELECT 1 AS \"DelimitedAlias\";");
    auto rows = AliasIdentifierRows();
    rows.insert(rows.end(),
                std::begin(kDelimitedIdentifierRows),
                std::end(kDelimitedIdentifierRows));
    RequireExactLowering(artifacts,
                         "SELECT 1 AS \"DelimitedAlias\";",
                         "DelimitedAlias",
                         rows);
    RequireServerAdmission(artifacts.envelope);
    RequireEngineDispatch("DelimitedAlias");
  }
  std::cout << "sbsql_scalar_syntax_exact_route_conformance=passed\n";
  return EXIT_SUCCESS;
}

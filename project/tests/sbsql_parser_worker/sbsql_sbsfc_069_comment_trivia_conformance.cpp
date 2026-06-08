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

constexpr std::string_view kSql =
    "--! doc grammar row\n"
    "-- line grammar row\n"
    "SELECT /* block grammar row */ /** doc block grammar row */ 1 AS comment_value;";
constexpr std::string_view kOperationId = "query.evaluate_projection";
constexpr std::string_view kOpcode = "SBLR_QUERY_EVALUATE_PROJECTION";
constexpr std::string_view kQueryFamily = "sblr.query.relational.v3";
constexpr std::string_view kCanonicalFamily = "sblr.catalog.mutation.v3";

struct CommentRowEvidence {
  std::string_view surface_id;
  std::string_view canonical_name;
};

constexpr std::array<CommentRowEvidence, 4> kCommentRows{{
    {"SBSQL-8EA1C10BB2CE", "comment"},
    {"SBSQL-F02BC9B3AD76", "comment_block"},
    {"SBSQL-167095E06F30", "comment_doc"},
    {"SBSQL-4E6D7E3DECC7", "comment_line"},
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

bool CstHasTriviaNodeForToken(const CstDocument& cst, std::size_t token_index) {
  for (const auto& node : cst.nodes) {
    if (node.token_index == token_index && node.trivia && node.kind == "comment") {
      return true;
    }
  }
  return false;
}

SessionContext ParserSession() {
  SessionContext session;
  session.authenticated = true;
  session.session_uuid = "019f0000-0000-7000-8000-000000069101";
  session.connection_uuid = "019f0000-0000-7000-8000-000000069102";
  session.database_uuid = "019f0000-0000-7000-8000-000000069103";
  session.dialect_profile_uuid = "sbsql_v3";
  session.catalog_epoch = 169;
  session.security_policy_epoch = 170;
  session.descriptor_epoch = 171;
  return session;
}

ParserConfig ParserConfigForTest() {
  ParserConfig config;
  config.probe_mode = true;
  config.server_endpoint = "sb_server_comment_trivia_route";
  config.parser_uuid = "019f0000-0000-7000-8000-000000069104";
  config.bundle_contract_id = "sbp_sbsql@comment-trivia-route-test";
  config.build_id = "sbsql-comment-trivia-route-test";
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
  for (const auto& row : kCommentRows) {
    const auto* registry_row = FindGeneratedSurfaceRegistryRowById(row.surface_id);
    Require(registry_row != nullptr, "comment generated registry row missing");
    Require(registry_row->canonical_name == row.canonical_name,
            "comment generated registry canonical name drifted");
    Require(registry_row->surface_kind == "grammar_production",
            "comment generated registry kind drifted");
    Require(registry_row->family == "ddl_catalog",
            "comment generated registry family drifted");
    Require(registry_row->source_status == "native_now",
            "comment generated registry status drifted");
    Require(registry_row->cluster_scope == "noncluster_or_profile_scoped",
            "comment generated registry cluster scope drifted");
    Require(registry_row->sblr_operation_family == kCanonicalFamily,
            "comment generated registry SBLR family drifted");
    Require(registry_row->parser_handler_key == "parser.statement_family.ddl_catalog",
            "comment generated registry parser handler drifted");
    Require(registry_row->lowering_handler_key == "lowering.sblr_family.sblr_catalog_mutation_v3",
            "comment generated registry lowering handler drifted");
    Require(registry_row->server_admission_key == "server.admission.sblr_catalog_mutation_v3",
            "comment generated registry server admission drifted");
    Require(registry_row->engine_rule_key == "engine.rule.sblr_catalog_mutation_v3",
            "comment generated registry engine rule drifted");
  }
}

void RequireCommentLexingAndCst(const CstDocument& cst) {
  Require(cst.trivia_preserved, "comment CST did not preserve trivia");
  Require(ReconstructSourceFromTokens(cst) == kSql,
          "comment CST did not reconstruct source exactly");
  bool saw_generic_comment = false;
  bool saw_line = false;
  bool saw_block = false;
  bool saw_doc_line = false;
  bool saw_doc_block = false;
  for (std::size_t index = 0; index < cst.tokens.size(); ++index) {
    const auto& token = cst.tokens[index];
    if (token.kind != TokenKind::kComment) continue;
    saw_generic_comment = true;
    Require(token.render_hint == "preserve_comment",
            "comment token did not preserve comment render hint");
    Require(CstHasTriviaNodeForToken(cst, index),
            "comment token missing CST trivia node");
    if (token.literal_family == "line_comment" &&
        Contains(token.raw_text, "line grammar row")) {
      saw_line = true;
    }
    if (token.literal_family == "block_comment" &&
        Contains(token.raw_text, "block grammar row")) {
      saw_block = true;
    }
    if (token.literal_family == "doc_comment" &&
        Contains(token.raw_text, "doc grammar row")) {
      saw_doc_line = true;
    }
    if (token.literal_family == "doc_comment" &&
        Contains(token.raw_text, "doc block grammar row")) {
      saw_doc_block = true;
    }
  }
  Require(saw_generic_comment, "comment grammar row had no comment token evidence");
  Require(saw_line, "comment_line grammar row had no line comment evidence");
  Require(saw_block, "comment_block grammar row had no block comment evidence");
  Require(saw_doc_line && saw_doc_block,
          "comment_doc grammar row had no documentation comment evidence");
}

void RequireCommentLexingEdges() {
  const auto nested = Lex("/* outer /* inner */ SELECT 1");
  Require(!nested.messages.has_errors(), "non-nesting block comment emitted errors");
  Require(!nested.tokens.empty() && nested.tokens.front().kind == TokenKind::kComment,
          "non-nesting block comment did not begin with comment token");
  Require(nested.tokens.front().literal_family == "block_comment",
          "non-nesting block comment family drifted");
  Require(nested.tokens.front().raw_text == "/* outer /* inner */",
          "block comment incorrectly nested inner opener");
  bool saw_select = false;
  for (const auto& token : nested.tokens) {
    if (token.kind == TokenKind::kKeyword && token.text == "SELECT") saw_select = true;
  }
  Require(saw_select, "non-nesting block comment swallowed following SELECT");

  const auto unterminated = Lex("/** open");
  Require(unterminated.messages.has_errors(),
          "unterminated documentation comment did not emit diagnostic");
  Require(!unterminated.tokens.empty() &&
              unterminated.tokens.front().literal_family == "doc_comment",
          "unterminated documentation comment family drifted");
  bool saw_unterminated = false;
  for (const auto& diagnostic : unterminated.messages.diagnostics) {
    if (diagnostic.code == "SBSQL.LEX.UNTERMINATED_COMMENT") {
      saw_unterminated = true;
    }
  }
  Require(saw_unterminated,
          "unterminated documentation comment diagnostic code drifted");
}

void RequireExactLowering(const PipelineArtifacts& artifacts) {
  Require(!artifacts.cst.messages.has_errors(), "comment CST failed");
  Require(!artifacts.ast.messages.has_errors(), "comment AST failed");
  Require(artifacts.bound.bound, "comment bind failed");
  Require(artifacts.verifier.admitted, "comment verifier rejected exact route");
  Require(artifacts.envelope.operation_family == kQueryFamily,
          "comment-bearing query operation family mismatch");
  Require(artifacts.envelope.sblr_operation_key == kQueryFamily,
          "comment-bearing query SBLR operation key mismatch");
  Require(artifacts.envelope.operation_id == kOperationId,
          "comment-bearing query operation id mismatch");
  Require(artifacts.envelope.engine_api_operation_id == kOperationId,
          "comment-bearing query engine API operation id mismatch");
  Require(artifacts.envelope.sblr_opcode == kOpcode,
          "comment-bearing query SBLR opcode mismatch");
  Require(HasValue(artifacts.envelope.required_authority_steps,
                   "authority.parser.no_sql_text_execution"),
          "comment-bearing query parser no-SQL-execution authority step missing");
  Require(!artifacts.envelope.parser_executes_sql,
          "comment-bearing query lowering allowed parser SQL execution");
  Require(Contains(artifacts.envelope.payload, "\"projection_0_name\":\"comment_value\""),
          "comment-bearing query payload missing projection alias");
  Require(Contains(artifacts.envelope.payload, "\"projection_0_expr_kind\":\"literal\""),
          "comment-bearing query payload missing literal expression kind");
  Require(Contains(artifacts.envelope.payload, "\"projection_0_type\":\"bigint\""),
          "comment-bearing query payload missing literal type");
  Require(Contains(artifacts.envelope.payload, "\"projection_0_value\":\"1\""),
          "comment-bearing query payload missing literal value");
  Require(Contains(artifacts.envelope.payload, "\"sql_text_included\":false"),
          "comment-bearing query payload did not prove no SQL text authority");
  Require(!Contains(artifacts.envelope.payload, "line grammar row"),
          "line comment text leaked into SBLR payload");
  Require(!Contains(artifacts.envelope.payload, "block grammar row"),
          "block comment text leaked into SBLR payload");
  Require(!Contains(artifacts.envelope.payload, "doc grammar row"),
          "documentation comment text leaked into SBLR payload");
  Require(!Contains(artifacts.envelope.payload, "WAL") &&
              !Contains(artifacts.envelope.payload, "wal") &&
              !Contains(artifacts.envelope.payload, "recovery"),
          "comment-bearing query payload carried WAL/recovery authority");
}

void RequireServerAdmission(const SblrEnvelope& envelope) {
  const auto admission = scratchbird::server::AdmitServerSblrEnvelope(
      scratchbird::server::ServerSblrAdmissionRequest{envelope.payload, false});
  Require(admission.admitted, "server admission rejected comment-bearing query");
  Require(admission.requires_public_abi_dispatch,
          "server admission did not require public ABI dispatch");
  Require(admission.operation_id == kOperationId,
          "server admission comment-bearing query operation id mismatch");
  Require(admission.operation_family == kQueryFamily,
          "server admission comment-bearing query operation family mismatch");
}

api::EngineRequestContext EngineContext() {
  api::EngineRequestContext context;
  context.request_id = "sbsql-comment-trivia-exact-route";
  context.database_uuid.canonical = "019f0000-0000-7000-8000-000000069201";
  context.node_uuid.canonical = "019f0000-0000-7000-8000-000000069202";
  context.session_uuid.canonical = "019f0000-0000-7000-8000-000000069203";
  context.principal_uuid.canonical = "019f0000-0000-7000-8000-000000069204";
  context.transaction_uuid.canonical = "019f0000-0000-7000-8000-000000069205";
  context.statement_uuid.canonical = "019f0000-0000-7000-8000-000000069206";
  context.current_schema_uuid.canonical = "019f0000-0000-7000-8000-000000069207";
  context.current_role_uuid.canonical = "019f0000-0000-7000-8000-000000069208";
  context.local_transaction_id = 169;
  context.security_context_present = true;
  context.trace_tags.push_back("right:QUERY_PROJECTION_TEST");
  context.trace_tags.push_back("sbsql_surface_id:SBSQL-8EA1C10BB2CE");
  context.trace_tags.push_back("sbsql_surface_id:SBSQL-F02BC9B3AD76");
  context.trace_tags.push_back("sbsql_surface_id:SBSQL-167095E06F30");
  context.trace_tags.push_back("sbsql_surface_id:SBSQL-4E6D7E3DECC7");
  return context;
}

sblr::SblrOperationEnvelope EngineEnvelope() {
  auto envelope = sblr::MakeSblrEnvelope(
      std::string(kOperationId),
      std::string(kOpcode),
      "trace.comment_trivia.exact_route.SBSQL-8EA1C10BB2CE");
  envelope.requires_security_context = true;
  envelope.requires_transaction_context = true;
  envelope.requires_cluster_authority = false;
  envelope.contains_sql_text = false;
  envelope.parser_resolved_names_to_uuids = true;
  envelope.operands.push_back({"text", "projection_count", "1"});
  envelope.operands.push_back({"text", "projection_0_name", "comment_value"});
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
  Require(result.accepted, "engine SBLR dispatch did not accept comment projection");
  Require(result.dispatched_to_api, "engine SBLR dispatch did not route to internal API");
  Require(result.api_result.ok,
          "EngineEvaluateProjection did not return success for comment route");
  Require(result.api_result.operation_id == kOperationId,
          "EngineEvaluateProjection operation id mismatch");
  Require(result.api_result.result_shape.rows.size() == 1,
          "comment result row count mismatch");
  const auto& field = result.api_result.result_shape.rows.front().fields.front();
  Require(field.first == "comment_value", "comment result field name mismatch");
  Require(field.second.descriptor.canonical_type_name == "bigint",
          "comment result descriptor mismatch");
  Require(!field.second.is_null, "comment result unexpectedly null");
  Require(field.second.encoded_value == "1", "comment result value mismatch");
  Require(HasEvidence(result.api_result,
                      "query_projection",
                      "constant_projection_engine_evaluated"),
          "comment result missing literal projection runtime evidence");
}

}  // namespace

int main() {
  RequireRegistryEvidence();
  RequireCommentLexingEdges();
  const auto artifacts = RunPipeline();
  RequireCommentLexingAndCst(artifacts.cst);
  RequireExactLowering(artifacts);
  RequireServerAdmission(artifacts.envelope);
  RequireEngineDispatch();
  std::cout << "sbsql_sbsfc_069_comment_trivia_conformance=passed\n";
  return EXIT_SUCCESS;
}

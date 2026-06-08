// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "ast/ast.hpp"
#include "cst/cst.hpp"

#include <iostream>
#include <string_view>
#include <vector>

namespace sbsql = scratchbird::parser::sbsql;

namespace {

bool Require(bool condition, std::string_view message) {
  if (condition) return true;
  std::cerr << message << "\n";
  return false;
}

std::size_t NonEndTokenCount(const sbsql::CstDocument& cst) {
  std::size_t count = 0;
  for (const auto& token : cst.tokens) {
    if (token.kind != sbsql::TokenKind::kEnd) ++count;
  }
  return count;
}

bool HasAstChildKind(const sbsql::AstDocument& ast, std::string_view kind) {
  if (ast.nodes.empty()) return false;
  for (const auto child_index : ast.nodes[ast.root_node_index].children) {
    if (child_index < ast.nodes.size() && ast.nodes[child_index].kind == kind) {
      return true;
    }
  }
  return false;
}

bool ValidateLosslessCstAstArtifacts() {
  const std::string source =
      "-- lead\nSELECT \"col\", DATE '2026-05-07' FROM \"schema\".\"table\"; /* tail */";
  const auto cst = sbsql::BuildCst(source);
  bool ok = true;
  ok &= Require(!cst.messages.has_errors(), "CST build produced errors");
  ok &= Require(sbsql::ReconstructSourceFromTokens(cst) == source,
                "CST token reconstruction is not lossless");
  ok &= Require(!cst.nodes.empty(), "CST root node missing");
  ok &= Require(cst.nodes[cst.root_node_index].children.size() == NonEndTokenCount(cst),
                "CST token node coverage mismatch");
  ok &= Require(cst.nodes[cst.root_node_index].range.length == source.size(),
                "CST root range does not cover source");

  bool saw_comment = false;
  bool saw_whitespace = false;
  bool saw_quoted_identifier = false;
  bool saw_temporal_literal = false;
  for (const auto& token : cst.tokens) {
    saw_comment = saw_comment || token.kind == sbsql::TokenKind::kComment;
    saw_whitespace = saw_whitespace || token.kind == sbsql::TokenKind::kWhitespace;
    saw_quoted_identifier =
        saw_quoted_identifier || (token.kind == sbsql::TokenKind::kIdentifier && token.quoted);
    saw_temporal_literal = saw_temporal_literal ||
                           token.kind == sbsql::TokenKind::kTemporalLiteral;
  }
  ok &= Require(saw_comment, "comment artifact was not preserved");
  ok &= Require(saw_whitespace, "whitespace artifact was not preserved");
  ok &= Require(saw_quoted_identifier, "quoted identifier artifact missing");
  ok &= Require(saw_temporal_literal, "temporal literal artifact missing");

  const auto ast = sbsql::BuildAst(cst);
  ok &= Require(!ast.messages.has_errors(), "AST build produced errors");
  ok &= Require(ast.family == sbsql::StatementFamily::kQuery, "AST family mismatch");
  ok &= Require(ast.statement_kind == "query", "AST statement kind mismatch");
  ok &= Require(ast.source_text == source, "AST source artifact mismatch");
  ok &= Require(ast.canonical_render == source, "AST render artifact mismatch");
  ok &= Require(!ast.nodes.empty(), "AST root node missing");
  ok &= Require(ast.nodes[ast.root_node_index].text == "query", "AST root text mismatch");
  ok &= Require(ast.nodes[ast.root_node_index].range.offset == source.find("SELECT"),
                "AST root range did not skip leading trivia");
  ok &= Require(HasAstChildKind(ast, "keyword"), "AST keyword child missing");
  ok &= Require(HasAstChildKind(ast, "delimited_identifier"),
                "AST delimited identifier child missing");
  ok &= Require(HasAstChildKind(ast, "literal"), "AST literal child missing");
  ok &= Require(HasAstChildKind(ast, "statement_terminator"),
                "AST terminator child missing");
  return ok;
}

bool ValidateStatementFamilies() {
  struct Case {
    std::string_view sql;
    sbsql::StatementFamily family;
    std::string_view statement_kind;
  };
  const std::vector<Case> cases = {
      {"SELECT 1", sbsql::StatementFamily::kQuery, "query"},
      {"VALUES (1)", sbsql::StatementFamily::kValues, "values"},
      {"INSERT INTO t VALUES (1)", sbsql::StatementFamily::kInsert, "insert"},
      {"UPDATE t SET a = 1", sbsql::StatementFamily::kUpdate, "update"},
      {"DELETE FROM t", sbsql::StatementFamily::kDelete, "delete"},
      {"MERGE INTO t USING s ON t.id = s.id", sbsql::StatementFamily::kMerge, "merge"},
      {"UPSERT INTO t VALUES (1)", sbsql::StatementFamily::kUpsert, "upsert"},
      {"CREATE TABLE t (id int)", sbsql::StatementFamily::kCatalog, "catalog"},
      {"ALTER TABLE t", sbsql::StatementFamily::kCatalog, "catalog"},
      {"DROP TABLE t", sbsql::StatementFamily::kCatalog, "catalog"},
      {"SHOW METRICS", sbsql::StatementFamily::kShow, "show"},
      {"SET x = 1", sbsql::StatementFamily::kSession, "session"},
      {"BEGIN", sbsql::StatementFamily::kTransaction, "transaction"},
      {"COMMIT", sbsql::StatementFamily::kTransaction, "transaction"},
      {"ROLLBACK", sbsql::StatementFamily::kTransaction, "transaction"},
      {"SAVEPOINT s", sbsql::StatementFamily::kTransaction, "transaction"},
      {"EXECUTE p", sbsql::StatementFamily::kExecute, "execute"},
      {"CALL p()", sbsql::StatementFamily::kCall, "call"},
  };

  bool ok = true;
  for (const auto& item : cases) {
    const auto cst = sbsql::BuildCst(item.sql);
    const auto ast = sbsql::BuildAst(cst);
    if (ast.messages.has_errors()) {
      std::cerr << "AST produced error for " << item.sql << "\n";
      ok = false;
      continue;
    }
    ok &= Require(ast.family == item.family,
                  std::string("statement family mismatch for ") + std::string(item.sql) +
                      ": got " + ast.statement_kind + " expected " +
                      std::string(item.statement_kind));
    ok &= Require(ast.statement_kind == item.statement_kind,
                  std::string("statement kind mismatch for ") + std::string(item.sql) +
                      ": got " + ast.statement_kind + " expected " +
                      std::string(item.statement_kind));
    ok &= Require(!ast.nodes.empty(), "statement AST node missing");
  }
  return ok;
}

bool ValidateRecoveryArtifacts() {
  bool ok = true;
  const auto empty = sbsql::BuildAst(sbsql::BuildCst("  -- only trivia\n"));
  ok &= Require(empty.messages.has_errors(), "empty trivia-only statement did not fail");
  ok &= Require(empty.source_text == "  -- only trivia\n",
                "empty statement source artifact was not preserved");

  const auto unknown = sbsql::BuildAst(sbsql::BuildCst("WAL CHECKPOINT"));
  ok &= Require(unknown.messages.has_errors(), "unknown/refusal statement did not fail");
  ok &= Require(!unknown.nodes.empty(), "unknown statement AST artifact missing");
  ok &= Require(unknown.nodes[unknown.root_node_index].range.offset == 0,
                "unknown statement source range missing");
  return ok;
}

} // namespace

int main() {
  bool ok = true;
  ok &= ValidateLosslessCstAstArtifacts();
  ok &= ValidateStatementFamilies();
  ok &= ValidateRecoveryArtifacts();
  if (!ok) return 1;
  std::cout << "SBSQL CST/AST conformance passed\n";
  return 0;
}

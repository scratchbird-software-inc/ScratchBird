// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "ast/ast.hpp"
#include "cst/cst.hpp"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <string_view>
#include <vector>

namespace sbsql = scratchbird::parser::sbsql;

namespace {

bool Require(const bool condition, const std::string_view message) {
  if (!condition) std::cerr << message << '\n';
  return condition;
}

bool HasExpressionKind(const sbsql::NativeRelationalAstDocument& document,
                       const sbsql::NativeExpressionAstKind kind) {
  return std::ranges::any_of(document.expressions, [kind](const auto& expression) {
    return expression.expression_kind == kind;
  });
}

bool HasDiagnostic(const sbsql::MessageVectorSet& messages,
                   const std::string_view diagnostic_id) {
  return std::ranges::any_of(messages.diagnostics, [diagnostic_id](const auto& diagnostic) {
    return diagnostic.code == diagnostic_id;
  });
}

bool ValidateTypedValuesFamily() {
  constexpr std::string_view sql =
      "VALUES (1, 'a', TRUE, NULL, :p), "
      "(2 + 3, -4, schema.value, abs(5), (?));";
  const auto cst = sbsql::BuildCst(sql);
  const auto native = sbsql::ParseNativeRelationalAst(cst);

  bool passed = true;
  passed &= Require(native.recognized(), "VALUES was not recognized as a native relation");
  passed &= Require(native.accepted(), "well-formed VALUES relation was refused");
  passed &= Require(native.root_relation_id == 1, "native relation root ID is not one");
  passed &= Require(native.relations.size() == 1, "VALUES did not create one relation node");
  passed &= Require(native.values_rows.size() == 2, "VALUES row count differs");
  passed &= Require(native.values_rows[0].expression_ids.size() == 5,
                    "first VALUES row arity differs");
  passed &= Require(native.values_rows[1].expression_ids.size() == 5,
                    "second VALUES row arity differs");
  passed &= Require(native.relations[0].relation_kind ==
                        sbsql::NativeRelationAstKind::kValues,
                    "relation kind is not the closed VALUES kind");
  passed &= Require(native.relations[0].input_relation_ids.empty(),
                    "VALUES relation acquired an input relation");
  passed &= Require(native.relations[0].values_row_ids ==
                        std::vector<std::uint32_t>({1, 2}),
                    "VALUES relation row handles differ");
  passed &= Require(HasExpressionKind(native, sbsql::NativeExpressionAstKind::kLiteral),
                    "literal expression node is missing");
  passed &= Require(HasExpressionKind(native, sbsql::NativeExpressionAstKind::kParameter),
                    "parameter expression node is missing");
  passed &= Require(HasExpressionKind(native, sbsql::NativeExpressionAstKind::kIdentifier),
                    "identifier expression node is missing");
  passed &= Require(HasExpressionKind(native, sbsql::NativeExpressionAstKind::kFunctionCall),
                    "function-call expression node is missing");
  passed &= Require(HasExpressionKind(native, sbsql::NativeExpressionAstKind::kUnary),
                    "unary expression node is missing");
  passed &= Require(HasExpressionKind(native, sbsql::NativeExpressionAstKind::kBinary),
                    "binary expression node is missing");
  passed &= Require(HasExpressionKind(native,
                                      sbsql::NativeExpressionAstKind::kParenthesized),
                    "parenthesized expression node is missing");

  for (std::size_t index = 0; index < native.expressions.size(); ++index) {
    const auto& expression = native.expressions[index];
    passed &= Require(expression.expression_id == index + 1,
                      "expression IDs are not stable one-based handles");
    passed &= Require(expression.range.length != 0,
                      "expression source range is empty");
    passed &= Require(expression.literal_kind.has_value() ==
                          (expression.expression_kind ==
                           sbsql::NativeExpressionAstKind::kLiteral),
                      "literal subtype leaked into a non-literal expression");
  }

  const auto ast = sbsql::BuildAst(cst);
  passed &= Require(!ast.messages.has_errors(), "BuildAst refused typed VALUES input");
  passed &= Require(ast.family == sbsql::StatementFamily::kValues,
                    "BuildAst did not use the typed VALUES family");
  passed &= Require(ast.native_relational.accepted(),
                    "BuildAst did not retain the typed relation document");
  passed &= Require(ast.produces_sblr,
                    "accepted typed VALUES input was not marked for later lowering");
  return passed;
}

bool ValidateMalformedRefusal() {
  constexpr std::string_view malformed[] = {
      "VALUES;",
      "VALUES ();",
      "VALUES (1,);",
      "VALUES (1),;",
      "VALUES (1), (2, 3);",
      "VALUES (1) trailing;",
      "VALUES ((1);",
  };

  bool passed = true;
  for (const auto sql : malformed) {
    const auto cst = sbsql::BuildCst(sql);
    const auto native = sbsql::ParseNativeRelationalAst(cst);
    passed &= Require(native.recognized(), "malformed VALUES was not recognized");
    passed &= Require(!native.accepted(), "malformed VALUES was accepted");
    passed &= Require(native.messages.has_errors(),
                      "malformed VALUES did not emit an error");
    passed &= Require(
        HasDiagnostic(native.messages, "QOW-DIAG-QRY-001-AST-MALFORMED"),
        "malformed VALUES did not emit the stable QOW diagnostic");
    passed &= Require(native.root_relation_id == 0 && native.relations.empty() &&
                          native.values_rows.empty() && native.expressions.empty(),
                      "refused VALUES retained a usable partial typed AST");

    const auto ast = sbsql::BuildAst(cst);
    passed &= Require(ast.messages.has_errors(),
                      "BuildAst did not refuse malformed typed input");
    passed &= Require(!ast.produces_sblr,
                      "malformed typed input remained eligible for lowering");
  }
  return passed;
}

bool ValidateFamilyBoundary() {
  const auto cst = sbsql::BuildCst("SELECT 1;");
  const auto native = sbsql::ParseNativeRelationalAst(cst);
  bool passed = true;
  passed &= Require(!native.recognized(),
                    "the bounded VALUES parser claimed a SELECT family");
  passed &= Require(native.root_relation_id == 0 && native.relations.empty(),
                    "unrecognized syntax produced native relation nodes");

  const auto ast = sbsql::BuildAst(cst);
  passed &= Require(!ast.messages.has_errors(), "legacy SELECT classification regressed");
  passed &= Require(ast.family == sbsql::StatementFamily::kQuery,
                    "legacy SELECT family classification regressed");
  passed &= Require(!ast.native_relational.recognized(),
                    "BuildAst fabricated a typed VALUES document for SELECT");
  return passed;
}

} // namespace

// QOW-TEST-QRY-001-AST-V1
int main() {
  bool passed = true;
  passed &= ValidateTypedValuesFamily();
  passed &= ValidateMalformedRefusal();
  passed &= ValidateFamilyBoundary();
  return passed ? EXIT_SUCCESS : EXIT_FAILURE;
}

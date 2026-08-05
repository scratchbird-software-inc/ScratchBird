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

static_assert(static_cast<int>(sbsql::NativeRelationAstKind::kValues) == 0);
static_assert(static_cast<int>(sbsql::NativeRelationAstKind::kAggregate) == 1);
static_assert(static_cast<int>(sbsql::NativeRelationAstKind::kFilter) == 2);
static_assert(
    static_cast<int>(sbsql::NativeRelationAstKind::kCatalogSource) == 3);
static_assert(static_cast<int>(sbsql::NativeExpressionAstKind::kLiteral) == 0);
static_assert(static_cast<int>(sbsql::NativeExpressionAstKind::kParameter) == 1);
static_assert(static_cast<int>(sbsql::NativeExpressionAstKind::kIdentifier) == 2);
static_assert(
    static_cast<int>(sbsql::NativeExpressionAstKind::kFunctionCall) == 3);
static_assert(static_cast<int>(sbsql::NativeExpressionAstKind::kUnary) == 4);
static_assert(static_cast<int>(sbsql::NativeExpressionAstKind::kBinary) == 5);
static_assert(
    static_cast<int>(sbsql::NativeExpressionAstKind::kParenthesized) == 6);
static_assert(static_cast<int>(sbsql::NativeExpressionAstKind::kWildcard) == 7);

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

std::string_view SourceForRange(const std::string_view source,
                                const sbsql::SourceRange& range) {
  if (range.offset > source.size() ||
      range.length > source.size() - range.offset) {
    return {};
  }
  return source.substr(range.offset, range.length);
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

bool ValidateCatalogRelationSourceFamily() {
  constexpr std::string_view sql =
      "SELECT * FROM tenant.sales.orders AS o;";
  const auto cst = sbsql::BuildCst(sql);
  const auto native = sbsql::ParseNativeRelationalAst(cst);

  bool passed = true;
  passed &= Require(native.recognized(),
                    "catalog SELECT was not recognized as a native relation");
  passed &= Require(native.accepted(), "catalog SELECT was refused");
  passed &= Require(native.root_relation_id == 1,
                    "catalog SELECT root ID is not one");
  passed &= Require(native.relations.size() == 1,
                    "catalog SELECT did not create one relation node");
  passed &= Require(native.catalog_relation_sources.size() == 1,
                    "catalog SELECT did not create one source node");
  passed &= Require(native.expressions.size() == 1,
                    "catalog SELECT did not retain its wildcard projection");
  if (native.relations.size() == 1) {
    const auto& relation = native.relations.front();
    passed &= Require(relation.relation_kind ==
                          sbsql::NativeRelationAstKind::kCatalogSource,
                      "catalog SELECT relation kind differs");
    passed &= Require(relation.relation_source_ids ==
                          std::vector<std::uint32_t>({1}),
                      "catalog SELECT source handle differs");
    passed &= Require(relation.input_relation_ids.empty(),
                      "catalog source acquired a relational input");
    passed &= Require(relation.output_expression_ids ==
                          std::vector<std::uint32_t>({1}),
                      "catalog SELECT projection handle differs");
    passed &= Require(SourceForRange(sql, relation.range) ==
                          "SELECT * FROM tenant.sales.orders AS o",
                      "catalog SELECT relation range differs");
  }
  if (native.catalog_relation_sources.size() == 1) {
    const auto& source = native.catalog_relation_sources.front();
    passed &= Require(source.source_id == 1,
                      "catalog source ID is not stable and one-based");
    passed &= Require(source.source_kind ==
                          sbsql::NativeRelationSourceAstKind::kCatalogRelation,
                      "catalog source kind differs");
    passed &= Require(source.qualified_name.size() == 3,
                      "qualified catalog source part count differs");
    if (source.qualified_name.size() == 3) {
      passed &= Require(source.qualified_name[0].spelling == "tenant" &&
                            !source.qualified_name[0].quoted &&
                            SourceForRange(sql, source.qualified_name[0].range) ==
                                "tenant",
                        "catalog source tenant component differs");
      passed &= Require(source.qualified_name[1].spelling == "sales" &&
                            !source.qualified_name[1].quoted &&
                            SourceForRange(sql, source.qualified_name[1].range) ==
                                "sales",
                        "catalog source schema component differs");
      passed &= Require(source.qualified_name[2].spelling == "orders" &&
                            !source.qualified_name[2].quoted &&
                            SourceForRange(sql, source.qualified_name[2].range) ==
                                "orders",
                        "catalog source relation component differs");
    }
    passed &= Require(SourceForRange(sql, source.qualified_name_range) ==
                          "tenant.sales.orders",
                      "qualified catalog source range differs");
    passed &= Require(source.alias.has_value(),
                      "explicit catalog alias is missing");
    if (source.alias.has_value()) {
      passed &= Require(source.alias->spelling == "o" &&
                            !source.alias->quoted &&
                            SourceForRange(sql, source.alias->range) == "o",
                        "explicit catalog alias differs");
    }
    passed &= Require(source.alias_is_explicit,
                      "AS alias was not recorded as explicit");
    passed &= Require(SourceForRange(sql, source.range) ==
                          "tenant.sales.orders AS o",
                      "catalog source range differs");
  }
  if (native.expressions.size() == 1) {
    const auto& wildcard = native.expressions.front();
    passed &= Require(wildcard.expression_id == 1 &&
                          wildcard.expression_kind ==
                              sbsql::NativeExpressionAstKind::kWildcard &&
                          wildcard.spelling == "*" &&
                          SourceForRange(sql, wildcard.range) == "*",
                      "catalog wildcard projection differs");
  }
  passed &= Require(
      sbsql::NativeRelationAstKindName(
          sbsql::NativeRelationAstKind::kCatalogSource) == "catalog_source",
      "catalog relation kind name differs");
  passed &= Require(
      sbsql::NativeRelationSourceAstKindName(
          sbsql::NativeRelationSourceAstKind::kCatalogRelation) ==
          "catalog_relation",
      "catalog source kind name differs");
  passed &= Require(
      sbsql::NativeExpressionAstKindName(
          sbsql::NativeExpressionAstKind::kWildcard) == "wildcard",
      "wildcard expression kind name differs");

  const auto ast = sbsql::BuildAst(cst);
  passed &= Require(!ast.messages.has_errors(),
                    "BuildAst refused the catalog relation source");
  passed &= Require(ast.family == sbsql::StatementFamily::kQuery,
                    "catalog source did not retain the query family");
  passed &= Require(ast.requires_name_resolution,
                    "catalog source did not require later name resolution");
  passed &= Require(!ast.produces_sblr,
                    "parser-only catalog source became eligible for SBLR lowering");

  constexpr std::string_view implicit_alias_sql =
      "SELECT * FROM app.\"Order Ledger\" ledger;";
  const auto implicit = sbsql::ParseNativeRelationalAst(
      sbsql::BuildCst(implicit_alias_sql));
  passed &= Require(implicit.accepted(),
                    "quoted catalog source with implicit alias was refused");
  if (implicit.catalog_relation_sources.size() == 1) {
    const auto& source = implicit.catalog_relation_sources.front();
    passed &= Require(source.qualified_name.size() == 2,
                      "quoted catalog source part count differs");
    if (source.qualified_name.size() == 2) {
      passed &= Require(source.qualified_name[1].spelling == "Order Ledger" &&
                            source.qualified_name[1].quoted &&
                            SourceForRange(implicit_alias_sql,
                                           source.qualified_name[1].range) ==
                                "\"Order Ledger\"",
                        "quoted catalog source component differs");
    }
    passed &= Require(source.alias.has_value() &&
                          source.alias->spelling == "ledger" &&
                          !source.alias_is_explicit,
                      "implicit catalog alias differs");
  }

  constexpr std::string_view no_alias_sql =
      "SELECT * FROM only_relation;";
  const auto no_alias =
      sbsql::ParseNativeRelationalAst(sbsql::BuildCst(no_alias_sql));
  passed &= Require(no_alias.accepted(),
                    "unaliased catalog source was refused");
  if (no_alias.catalog_relation_sources.size() == 1) {
    passed &= Require(!no_alias.catalog_relation_sources.front().alias.has_value(),
                      "unaliased catalog source acquired an alias");
  }
  return passed;
}

bool ValidateCatalogLimitComposition() {
  constexpr std::string_view sql =
      "SELECT * FROM app.orders LIMIT 7 OFFSET 2;";
  const auto native =
      sbsql::ParseNativeRelationalAst(sbsql::BuildCst(sql));
  bool passed = true;
  passed &= Require(native.accepted(),
                    "catalog LIMIT/OFFSET composition was refused");
  passed &= Require(native.root_relation_id == 2 &&
                        native.relations.size() == 2 &&
                        native.expressions.size() == 3,
                    "catalog LIMIT/OFFSET graph cardinality differs");
  if (native.relations.size() == 2) {
    const auto& source = native.relations[0];
    const auto& limit = native.relations[1];
    passed &= Require(
        source.relation_kind ==
                sbsql::NativeRelationAstKind::kCatalogSource &&
            limit.relation_kind == sbsql::NativeRelationAstKind::kLimit &&
            limit.input_relation_ids == std::vector<std::uint32_t>({1}) &&
            limit.output_expression_ids ==
                source.output_expression_ids &&
            limit.limit_expression_ids ==
                std::vector<std::uint32_t>({2, 3}),
        "catalog LIMIT/OFFSET relation linkage differs");
    passed &= Require(SourceForRange(sql, source.range) ==
                          "SELECT * FROM app.orders" &&
                          SourceForRange(sql, limit.range) ==
                              "SELECT * FROM app.orders LIMIT 7 OFFSET 2",
                      "catalog LIMIT/OFFSET relation ranges differ");
  }
  if (native.expressions.size() == 3) {
    passed &= Require(
        native.expressions[1].expression_kind ==
                sbsql::NativeExpressionAstKind::kLiteral &&
            native.expressions[1].literal_kind ==
                sbsql::NativeLiteralAstKind::kNumeric &&
            native.expressions[1].spelling == "7" &&
            native.expressions[2].expression_kind ==
                sbsql::NativeExpressionAstKind::kLiteral &&
            native.expressions[2].literal_kind ==
                sbsql::NativeLiteralAstKind::kNumeric &&
            native.expressions[2].spelling == "2",
        "catalog LIMIT/OFFSET literal expressions differ");
  }
  passed &= Require(
      sbsql::NativeRelationAstKindName(
          sbsql::NativeRelationAstKind::kLimit) == "limit",
      "catalog LIMIT relation kind name differs");
  return passed;
}

bool ValidateCatalogRelationRefusal() {
  constexpr std::string_view malformed[] = {
      "SELECT * FROM;",
      "SELECT * FROM .orders;",
      "SELECT * FROM app.;",
      "SELECT * FROM app..orders;",
      "SELECT * FROM app.orders AS;",
      "SELECT * FROM app.orders AS WHERE;",
      "SELECT * FROM app.orders WHERE TRUE;",
      "SELECT * FROM app.orders, app.customers;",
      "SELECT * FROM app.orders JOIN app.customers ON TRUE;",
      "SELECT * FROM app.orders alias trailing;",
      "SELECT * FROM app.orders LIMIT;",
      "SELECT * FROM app.orders LIMIT value;",
      "SELECT * FROM app.orders LIMIT 1 OFFSET;",
      "SELECT * FROM app.orders LIMIT 1 OFFSET -1;",
      "SELECT * FROM app.orders OFFSET 1;",
  };

  bool passed = true;
  for (const auto sql : malformed) {
    const auto cst = sbsql::BuildCst(sql);
    const auto native = sbsql::ParseNativeRelationalAst(cst);
    passed &= Require(native.recognized(),
                      "malformed catalog SELECT was not recognized");
    passed &= Require(!native.accepted(),
                      "malformed catalog SELECT was accepted");
    passed &= Require(native.messages.has_errors(),
                      "malformed catalog SELECT did not emit an error");
    passed &= Require(
        HasDiagnostic(native.messages, "QOW-DIAG-QRY-001-AST-MALFORMED"),
        "malformed catalog SELECT did not emit the stable QOW diagnostic");
    passed &= Require(native.root_relation_id == 0 &&
                          native.relations.empty() &&
                          native.catalog_relation_sources.empty() &&
                          native.values_rows.empty() &&
                          native.expressions.empty(),
                      "refused catalog SELECT retained a usable partial AST");

    const auto ast = sbsql::BuildAst(cst);
    passed &= Require(ast.messages.has_errors(),
                      "BuildAst did not refuse malformed catalog SELECT");
    passed &= Require(!ast.produces_sblr,
                      "malformed catalog SELECT became eligible for lowering");
  }

  constexpr std::string_view out_of_slice_sql =
      "SELECT name FROM app.orders;";
  const auto out_of_slice = sbsql::ParseNativeRelationalAst(
      sbsql::BuildCst(out_of_slice_sql));
  passed &= Require(!out_of_slice.recognized() &&
                        out_of_slice.root_relation_id == 0 &&
                        out_of_slice.relations.empty() &&
                        out_of_slice.catalog_relation_sources.empty() &&
                        out_of_slice.expressions.empty(),
                    "non-wildcard catalog projection entered the bounded route");
  return passed;
}

bool ValidateFamilyBoundary() {
  const auto cst = sbsql::BuildCst("SELECT 1;");
  const auto native = sbsql::ParseNativeRelationalAst(cst);
  bool passed = true;
  passed &= Require(!native.recognized(),
                    "the bounded native parser claimed an object-free SELECT");
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
  passed &= ValidateCatalogRelationSourceFamily();
  passed &= ValidateCatalogLimitComposition();
  passed &= ValidateCatalogRelationRefusal();
  passed &= ValidateFamilyBoundary();
  return passed ? EXIT_SUCCESS : EXIT_FAILURE;
}

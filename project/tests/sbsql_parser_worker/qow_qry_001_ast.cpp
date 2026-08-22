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
static_assert(static_cast<int>(sbsql::NativeRelationAstKind::kLimit) == 4);
static_assert(static_cast<int>(sbsql::NativeRelationAstKind::kProject) == 5);
static_assert(static_cast<int>(sbsql::NativeRelationAstKind::kSort) == 6);
static_assert(static_cast<int>(sbsql::NativeRelationAstKind::kJoin) == 7);
static_assert(static_cast<int>(sbsql::NativeRelationAstKind::kWindow) == 8);
static_assert(static_cast<int>(sbsql::NativeRelationAstKind::kQualify) == 9);
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

bool ValidateCatalogFilterComposition() {
  constexpr std::string_view sql =
      "SELECT * FROM app.orders WHERE amount >= 10 LIMIT 2;";
  const auto native =
      sbsql::ParseNativeRelationalAst(sbsql::BuildCst(sql));
  bool passed = true;
  passed &= Require(native.accepted(),
                    "catalog WHERE/LIMIT composition was refused");
  passed &= Require(native.root_relation_id == 3,
                    "catalog WHERE/LIMIT root identity differs");
  passed &= Require(native.relations.size() == 3,
                    "catalog WHERE/LIMIT relation cardinality differs");
  passed &= Require(native.expressions.size() == 5,
                    "catalog WHERE/LIMIT expression cardinality differs");
  if (native.relations.size() == 3) {
    const auto& source = native.relations[0];
    const auto& filter = native.relations[1];
    const auto& limit = native.relations[2];
    passed &= Require(
        source.relation_kind ==
                sbsql::NativeRelationAstKind::kCatalogSource &&
            filter.relation_kind == sbsql::NativeRelationAstKind::kFilter &&
            filter.input_relation_ids == std::vector<std::uint32_t>({1}) &&
            filter.predicate_expression_ids ==
                std::vector<std::uint32_t>({4}) &&
            limit.relation_kind == sbsql::NativeRelationAstKind::kLimit &&
            limit.input_relation_ids == std::vector<std::uint32_t>({2}) &&
            limit.limit_expression_ids ==
                std::vector<std::uint32_t>({5}),
        "catalog WHERE/LIMIT relation linkage differs");
    passed &= Require(
        SourceForRange(sql, source.range) == "SELECT * FROM app.orders" &&
            SourceForRange(sql, filter.range) ==
                "SELECT * FROM app.orders WHERE amount >= 10" &&
            SourceForRange(sql, limit.range) ==
                "SELECT * FROM app.orders WHERE amount >= 10 LIMIT 2",
        "catalog WHERE/LIMIT relation ranges differ");
  }
  if (native.expressions.size() == 5) {
    const auto& identifier = native.expressions[1];
    const auto& literal = native.expressions[2];
    const auto& predicate = native.expressions[3];
    passed &= Require(
        identifier.expression_kind ==
                sbsql::NativeExpressionAstKind::kIdentifier &&
            identifier.spelling == "amount" &&
            literal.expression_kind ==
                sbsql::NativeExpressionAstKind::kLiteral &&
            literal.literal_kind == sbsql::NativeLiteralAstKind::kNumeric &&
            literal.spelling == "10" &&
            predicate.expression_kind ==
                sbsql::NativeExpressionAstKind::kBinary &&
            predicate.operator_name == ">=" &&
            predicate.child_expression_ids ==
                std::vector<std::uint32_t>({2, 3}),
        "catalog WHERE predicate expression differs");
  }
  return passed;
}

bool ValidateCatalogProjectionComposition() {
  constexpr std::string_view sql =
      "SELECT amount, order_id FROM app.orders WHERE amount >= 10 LIMIT 2;";
  const auto native =
      sbsql::ParseNativeRelationalAst(sbsql::BuildCst(sql));
  bool passed = true;
  passed &= Require(native.accepted(),
                    "catalog projection composition was refused");
  passed &= Require(native.root_relation_id == 3 &&
                        native.relations.size() == 3 &&
                        native.expressions.size() == 6,
                    "catalog projection composition shape differs");
  if (native.relations.size() == 3) {
    passed &= Require(
        native.relations[0].output_expression_ids ==
                std::vector<std::uint32_t>({1, 2}) &&
            native.relations[1].output_expression_ids ==
                std::vector<std::uint32_t>({1, 2}) &&
            native.relations[2].output_expression_ids ==
                std::vector<std::uint32_t>({1, 2}),
        "catalog projection identifiers did not cross the operator chain");
  }
  if (native.expressions.size() == 6) {
    passed &= Require(
        native.expressions[0].expression_kind ==
                sbsql::NativeExpressionAstKind::kIdentifier &&
            native.expressions[0].spelling == "amount" &&
            native.expressions[1].expression_kind ==
                sbsql::NativeExpressionAstKind::kIdentifier &&
            native.expressions[1].spelling == "order_id" &&
            native.expressions[4].child_expression_ids ==
                std::vector<std::uint32_t>({3, 4}),
        "catalog projection expression identities differ");
  }
  constexpr std::string_view hidden_sql =
      "SELECT amount FROM app.orders WHERE order_id >= 10 LIMIT 2;";
  const auto hidden =
      sbsql::ParseNativeRelationalAst(sbsql::BuildCst(hidden_sql));
  passed &= Require(hidden.accepted() && hidden.root_relation_id == 4 &&
                        hidden.relations.size() == 4 &&
                        hidden.expressions.size() == 5,
                    "hidden predicate-column projection shape differs");
  if (hidden.relations.size() == 4) {
    passed &= Require(
        hidden.relations[0].output_expression_ids ==
                std::vector<std::uint32_t>({1, 2}) &&
            hidden.relations[1].relation_kind ==
                sbsql::NativeRelationAstKind::kFilter &&
            hidden.relations[1].output_expression_ids ==
                std::vector<std::uint32_t>({1, 2}) &&
            hidden.relations[2].relation_kind ==
                sbsql::NativeRelationAstKind::kProject &&
            hidden.relations[2].input_relation_ids ==
                std::vector<std::uint32_t>({2}) &&
            hidden.relations[2].output_expression_ids ==
                std::vector<std::uint32_t>({1}) &&
            hidden.relations[3].input_relation_ids ==
                std::vector<std::uint32_t>({3}) &&
            hidden.relations[3].output_expression_ids ==
                std::vector<std::uint32_t>({1}),
        "hidden predicate column did not terminate at the Project node");
  }
  return passed;
}

bool ValidateCatalogJoinTailComposition() {
  constexpr std::string_view mixed_sql =
      "SELECT l.integer_value FROM app.left_relation AS l CROSS JOIN "
      "app.right_relation AS r WHERE r.join_value >= 2 LIMIT ?;";
  const auto mixed =
      sbsql::ParseNativeRelationalAst(sbsql::BuildCst(mixed_sql));
  bool passed = true;
  passed &= Require(mixed.accepted(),
                    "catalog JOIN literal-filter/parameter-LIMIT was refused");
  passed &= Require(mixed.root_relation_id == 6 &&
                        mixed.relations.size() == 6 &&
                        mixed.expressions.size() == 7,
                    "catalog JOIN mixed tail graph cardinality differs");
  if (mixed.relations.size() == 6) {
    const auto& join = mixed.relations[2];
    const auto& filter = mixed.relations[3];
    const auto& project = mixed.relations[4];
    const auto& limit = mixed.relations[5];
    passed &= Require(
        join.relation_kind == sbsql::NativeRelationAstKind::kJoin &&
            join.join_kind == sbsql::NativeJoinAstKind::kCross &&
            join.input_relation_ids == std::vector<std::uint32_t>({1, 2}) &&
            join.output_expression_ids ==
                std::vector<std::uint32_t>({6, 7}) &&
            filter.relation_kind == sbsql::NativeRelationAstKind::kFilter &&
            filter.input_relation_ids == std::vector<std::uint32_t>({3}) &&
            filter.output_expression_ids == join.output_expression_ids &&
            filter.predicate_expression_ids ==
                std::vector<std::uint32_t>({4}) &&
            project.relation_kind ==
                sbsql::NativeRelationAstKind::kProject &&
            project.input_relation_ids == std::vector<std::uint32_t>({4}) &&
            project.output_expression_ids ==
                std::vector<std::uint32_t>({1}) &&
            limit.relation_kind == sbsql::NativeRelationAstKind::kLimit &&
            limit.input_relation_ids == std::vector<std::uint32_t>({5}) &&
            limit.output_expression_ids == project.output_expression_ids &&
            limit.limit_expression_ids ==
                std::vector<std::uint32_t>({5}),
        "catalog JOIN FILTER/PROJECT/LIMIT topology or lineage differs");
  }
  if (mixed.expressions.size() == 7) {
    const auto& projection = mixed.expressions[0];
    const auto& filter_identifier = mixed.expressions[1];
    const auto& filter_literal = mixed.expressions[2];
    const auto& predicate = mixed.expressions[3];
    const auto& limit_parameter = mixed.expressions[4];
    passed &= Require(
        projection.expression_kind ==
                sbsql::NativeExpressionAstKind::kIdentifier &&
            projection.qualified_identifier.size() == 2 &&
            projection.qualified_identifier[0].spelling == "l" &&
            projection.qualified_identifier[1].spelling == "integer_value" &&
            filter_identifier.expression_kind ==
                sbsql::NativeExpressionAstKind::kIdentifier &&
            filter_identifier.qualified_identifier.size() == 2 &&
            filter_identifier.qualified_identifier[0].spelling == "r" &&
            filter_identifier.qualified_identifier[1].spelling ==
                "join_value" &&
            filter_literal.expression_kind ==
                sbsql::NativeExpressionAstKind::kLiteral &&
            filter_literal.literal_kind ==
                sbsql::NativeLiteralAstKind::kNumeric &&
            filter_literal.structural_literal_occurrence_id == 1 &&
            filter_literal.structural_parameter_occurrence_id == 0 &&
            predicate.expression_kind ==
                sbsql::NativeExpressionAstKind::kBinary &&
            predicate.operator_name == ">=" &&
            predicate.child_expression_ids ==
                std::vector<std::uint32_t>({2, 3}) &&
            limit_parameter.expression_kind ==
                sbsql::NativeExpressionAstKind::kParameter &&
            limit_parameter.structural_literal_occurrence_id == 0 &&
            limit_parameter.structural_parameter_occurrence_id == 1 &&
            limit_parameter.structural_variable_occurrence_id == 0,
        "catalog JOIN mixed tail expression or occurrence identity differs");
  }

  constexpr std::string_view unsupported_pairings[] = {
      "SELECT * FROM app.a AS a CROSS JOIN app.b AS b WHERE a.value >= 2 "
      "LIMIT 1;",
      "SELECT * FROM app.a AS a CROSS JOIN app.b AS b WHERE a.value >= ? "
      "LIMIT 1;",
      "SELECT * FROM app.a AS a CROSS JOIN app.b AS b WHERE a.value >= ? "
      "LIMIT ?;",
  };
  for (const auto sql : unsupported_pairings) {
    const auto parsed =
        sbsql::ParseNativeRelationalAst(sbsql::BuildCst(sql));
    passed &= Require(!parsed.accepted() && parsed.root_relation_id == 0 &&
                          parsed.relations.empty(),
                      "unsupported catalog JOIN FILTER/LIMIT pairing was "
                      "admitted");
  }

  constexpr std::string_view three_way_sql =
      "SELECT * FROM app.a AS a CROSS JOIN app.b AS b CROSS JOIN app.c AS c "
      "WHERE c.value >= 2 LIMIT ?;";
  const auto three_way =
      sbsql::ParseNativeRelationalAst(sbsql::BuildCst(three_way_sql));
  passed &= Require(three_way.accepted() &&
                        three_way.root_relation_id == 7 &&
                        three_way.relations.size() == 7 &&
                        three_way.relations[3].input_relation_ids ==
                            std::vector<std::uint32_t>({1, 2}) &&
                        three_way.relations[4].input_relation_ids ==
                            std::vector<std::uint32_t>({4, 3}) &&
                        three_way.relations[5].input_relation_ids ==
                            std::vector<std::uint32_t>({5}) &&
                        three_way.relations[6].input_relation_ids ==
                            std::vector<std::uint32_t>({6}),
                    "three-way catalog CROSS JOIN FILTER/LIMIT topology differs");

  constexpr std::string_view refused[] = {
      "SELECT * FROM app.a AS a CROSS JOIN app.b AS b WHERE a.value >= @v "
      "LIMIT ?;",
      "SELECT * FROM app.a AS a CROSS JOIN app.b AS b LIMIT @v;",
      "SELECT * FROM app.a AS a CROSS JOIN app.b AS b LIMIT 01;",
      "SELECT * FROM app.a AS a CROSS JOIN app.b AS b LIMIT "
      "9223372036854775808;",
  };
  for (const auto sql : refused) {
    const auto parsed = sbsql::ParseNativeRelationalAst(sbsql::BuildCst(sql));
    passed &= Require(!parsed.accepted() && parsed.root_relation_id == 0 &&
                          parsed.relations.empty(),
                      "catalog JOIN tail refusal retained an executable graph");
  }
  return passed;
}

bool ValidateCatalogOrderingComposition() {
  constexpr std::string_view sql =
      "SELECT amount FROM app.orders WHERE order_id >= 10 "
      "ORDER BY order_id DESC NULLS LAST, amount ASC LIMIT 2;";
  const auto native =
      sbsql::ParseNativeRelationalAst(sbsql::BuildCst(sql));
  bool passed = true;
  passed &= Require(native.accepted() && native.root_relation_id == 5 &&
                        native.relations.size() == 5,
                    "catalog ORDER BY composition shape differs");
  if (native.relations.size() == 5) {
    const auto& sort = native.relations[2];
    passed &= Require(
        sort.relation_kind == sbsql::NativeRelationAstKind::kSort &&
            sort.input_relation_ids == std::vector<std::uint32_t>({2}) &&
            sort.output_expression_ids ==
                std::vector<std::uint32_t>({1, 2}) &&
            sort.ordering_terms.size() == 2 &&
            sort.ordering_terms[0].expression_id == 2 &&
            sort.ordering_terms[0].direction ==
                sbsql::NativeSortDirection::kDescending &&
            sort.ordering_terms[0].null_placement ==
                sbsql::NativeNullPlacement::kNullsLast &&
            sort.ordering_terms[1].expression_id == 1 &&
            sort.ordering_terms[1].direction ==
                sbsql::NativeSortDirection::kAscending &&
            sort.ordering_terms[1].null_placement ==
                sbsql::NativeNullPlacement::kNullsLast &&
            native.relations[3].relation_kind ==
                sbsql::NativeRelationAstKind::kProject &&
            native.relations[3].input_relation_ids ==
                std::vector<std::uint32_t>({3}) &&
            native.relations[4].input_relation_ids ==
                std::vector<std::uint32_t>({4}),
        "catalog ORDER BY terms or hidden-key placement differ");
  }

  constexpr std::string_view wildcard_sql =
      "SELECT * FROM app.orders ORDER BY order_id DESC;";
  const auto wildcard =
      sbsql::ParseNativeRelationalAst(sbsql::BuildCst(wildcard_sql));
  passed &= Require(
      wildcard.accepted() && wildcard.relations.size() == 2 &&
          wildcard.relations[0].output_expression_ids ==
              std::vector<std::uint32_t>({1}) &&
          wildcard.relations[1].relation_kind ==
              sbsql::NativeRelationAstKind::kSort &&
          wildcard.relations[1].ordering_terms.size() == 1 &&
          wildcard.relations[1].ordering_terms[0].null_placement ==
              sbsql::NativeNullPlacement::kNullsFirst,
      "catalog wildcard ORDER BY default null placement differs");
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
      "SELECT * FROM app.orders WHERE;",
      "SELECT * FROM app.orders WHERE amount;",
      "SELECT * FROM app.orders WHERE amount = 'ten';",
      "SELECT * FROM app.orders WHERE 10 < amount;",
      "SELECT * FROM app.orders WHERE amount > 1 AND amount < 3;",
      "SELECT * FROM app.orders ORDER;",
      "SELECT * FROM app.orders ORDER amount;",
      "SELECT * FROM app.orders ORDER BY amount NULLS;",
      "SELECT * FROM app.orders ORDER BY amount, amount;",
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
      "SELECT name || 'suffix' FROM app.orders;";
  const auto out_of_slice = sbsql::ParseNativeRelationalAst(
      sbsql::BuildCst(out_of_slice_sql));
  passed &= Require(!out_of_slice.recognized() &&
                        out_of_slice.root_relation_id == 0 &&
                        out_of_slice.relations.empty() &&
                        out_of_slice.catalog_relation_sources.empty() &&
                        out_of_slice.expressions.empty(),
                    "computed catalog projection entered the bounded route");
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

bool ValidateTypedWindowVocabulary() {
  sbsql::NativeWindowDefinitionAstNode definition;
  definition.window_id = 1;
  definition.name = sbsql::NativeIdentifierAstNode{"ranked", false, {}};
  definition.base_name =
      sbsql::NativeIdentifierAstNode{"partitioned", false, {}};
  definition.partition_expression_ids = {4, 5};
  definition.ordering_terms = {
      {6, sbsql::NativeSortDirection::kDescending,
       sbsql::NativeNullPlacement::kNullsFirst, {}}};
  definition.frame_unit = sbsql::NativeWindowFrameUnit::kGroups;
  definition.frame_start = {sbsql::NativeWindowFrameBoundKind::kPreceding,
                            7, {}};
  definition.frame_end = {
      sbsql::NativeWindowFrameBoundKind::kCurrentRow, std::nullopt, {}};
  definition.exclusion = sbsql::NativeWindowFrameExclusion::kTies;

  sbsql::NativeWindowInvocationAstNode invocation;
  invocation.invocation_id = 1;
  invocation.function_expression_id = 8;
  invocation.window_definition_id = 1;
  sbsql::NativeRelationAstNode window;
  window.relation_id = 2;
  window.relation_kind = sbsql::NativeRelationAstKind::kWindow;
  window.input_relation_ids = {1};
  window.window_invocation_ids = {invocation.invocation_id};

  bool passed = true;
  passed &= Require(
      definition.name->spelling == "ranked" &&
          definition.base_name->spelling == "partitioned" &&
          definition.partition_expression_ids ==
              std::vector<std::uint32_t>({4, 5}) &&
          definition.frame_start->offset_expression_id == 7 &&
          definition.exclusion == sbsql::NativeWindowFrameExclusion::kTies,
      "typed window definition lost name, inheritance, partition, or frame state");
  passed &= Require(
      invocation.function_expression_id == 8 &&
          invocation.window_definition_id == definition.window_id &&
          window.window_invocation_ids ==
              std::vector<std::uint32_t>({invocation.invocation_id}),
      "typed window invocation is not independently addressable");
  passed &= Require(
      sbsql::NativeRelationAstKindName(
          sbsql::NativeRelationAstKind::kWindow) == "window" &&
          sbsql::NativeRelationAstKindName(
              sbsql::NativeRelationAstKind::kQualify) == "qualify" &&
          sbsql::NativeWindowFrameUnitName(
              sbsql::NativeWindowFrameUnit::kGroups) == "groups" &&
          sbsql::NativeWindowFrameBoundKindName(
              sbsql::NativeWindowFrameBoundKind::kUnboundedFollowing) ==
              "unbounded_following" &&
          sbsql::NativeWindowFrameExclusionName(
              sbsql::NativeWindowFrameExclusion::kTies) == "ties",
      "typed window vocabulary names are not canonical");
  return passed;
}

bool ValidateTypedWindowParse() {
  constexpr std::string_view sql =
      "SELECT ROW_NUMBER() OVER (PARTITION BY account_id ORDER BY created_at "
      "DESC NULLS FIRST GROUPS BETWEEN 2 PRECEDING AND CURRENT ROW EXCLUDE "
      "TIES) AS sequence_no FROM app.events AS e;";
  const auto native =
      sbsql::ParseNativeRelationalAst(sbsql::BuildCst(sql));
  bool passed = true;
  passed &= Require(native.accepted(),
                    "typed ROW_NUMBER OVER query was refused");
  passed &= Require(native.root_relation_id == 2 &&
                        native.relations.size() == 2 &&
                        native.relations[0].relation_kind ==
                            sbsql::NativeRelationAstKind::kCatalogSource &&
                        native.relations[1].relation_kind ==
                            sbsql::NativeRelationAstKind::kWindow &&
                        native.relations[1].input_relation_ids ==
                            std::vector<std::uint32_t>({1}) &&
                        native.relations[1].window_invocation_ids ==
                            std::vector<std::uint32_t>({1}),
                    "typed window relational chain differs");
  passed &= Require(native.window_definitions.size() == 1 &&
                        native.window_invocations.size() == 1,
                    "typed window definition/invocation cardinality differs");
  if (native.window_definitions.size() == 1) {
    const auto& definition = native.window_definitions.front();
    passed &= Require(
        definition.partition_expression_ids ==
                std::vector<std::uint32_t>({2}) &&
            definition.ordering_terms.size() == 1 &&
            definition.ordering_terms.front().expression_id == 3 &&
            definition.ordering_terms.front().direction ==
                sbsql::NativeSortDirection::kDescending &&
            definition.ordering_terms.front().null_placement ==
                sbsql::NativeNullPlacement::kNullsFirst &&
            definition.frame_unit == sbsql::NativeWindowFrameUnit::kGroups &&
            definition.frame_start.has_value() &&
            definition.frame_start->bound_kind ==
                sbsql::NativeWindowFrameBoundKind::kPreceding &&
            definition.frame_start->offset_expression_id == 4 &&
            definition.frame_end.has_value() &&
            definition.frame_end->bound_kind ==
                sbsql::NativeWindowFrameBoundKind::kCurrentRow &&
            definition.exclusion ==
                sbsql::NativeWindowFrameExclusion::kTies,
        "typed window partition/order/frame/exclusion state differs");
  }
  if (native.window_invocations.size() == 1) {
    const auto& invocation = native.window_invocations.front();
    passed &= Require(invocation.function_expression_id == 1 &&
                          invocation.window_definition_id == 1 &&
                          invocation.output_alias.has_value() &&
                          invocation.output_alias->spelling == "sequence_no",
                      "typed window invocation binding handles differ");
  }
  passed &= Require(native.expressions.size() == 4 &&
                        native.expressions[0].expression_kind ==
                            sbsql::NativeExpressionAstKind::kFunctionCall &&
                        native.expressions[0].operator_name == "ROW_NUMBER" &&
                        native.expressions[1].spelling == "account_id" &&
                        native.expressions[2].spelling == "created_at" &&
                        native.expressions[3].literal_kind ==
                            sbsql::NativeLiteralAstKind::kNumeric,
                    "typed window expression records differ");
  passed &= Require(native.catalog_relation_sources.size() == 1 &&
                        native.catalog_relation_sources.front().qualified_name
                                .size() == 2 &&
                        native.catalog_relation_sources.front().alias.has_value() &&
                        native.catalog_relation_sources.front().alias->spelling ==
                            "e",
                    "typed window catalog source differs");

  const auto reused = sbsql::ParseNativeRelationalAst(sbsql::BuildCst(
      "SELECT ROW_NUMBER() OVER (PARTITION BY account_id ORDER BY account_id) "
      "FROM app.events;"));
  passed &= Require(
      reused.accepted() && reused.expressions.size() == 2 &&
          reused.relations.front().output_expression_ids ==
              std::vector<std::uint32_t>({2}) &&
          reused.window_definitions.front().partition_expression_ids ==
              std::vector<std::uint32_t>({2}) &&
          reused.window_definitions.front().ordering_terms.front().expression_id ==
              2,
      "reused partition/order column was not interned to one typed expression");

  constexpr std::string_view named_sql =
      "SELECT ROW_NUMBER() OVER framed AS sequence_no FROM app.events AS e "
      "WINDOW partitioned AS (PARTITION BY account_id), "
      "ordered AS (partitioned ORDER BY created_at DESC NULLS FIRST), "
      "framed AS (ordered GROUPS BETWEEN 2 PRECEDING AND CURRENT ROW "
      "EXCLUDE TIES);";
  const auto named =
      sbsql::ParseNativeRelationalAst(sbsql::BuildCst(named_sql));
  passed &= Require(
      named.accepted() && named.window_definitions.size() == 3 &&
          named.window_invocations.size() == 1 &&
          named.window_invocations.front().window_definition_id == 3 &&
          named.window_definitions[0].name->spelling == "partitioned" &&
          !named.window_definitions[0].base_name.has_value() &&
          named.window_definitions[0].partition_expression_ids ==
              std::vector<std::uint32_t>({2}) &&
          named.window_definitions[1].name->spelling == "ordered" &&
          named.window_definitions[1].base_name->spelling == "partitioned" &&
          named.window_definitions[1].ordering_terms.front().expression_id == 3 &&
          named.window_definitions[2].name->spelling == "framed" &&
          named.window_definitions[2].base_name->spelling == "ordered" &&
          named.window_definitions[2].frame_start->offset_expression_id == 4 &&
          named.relations.front().output_expression_ids ==
              std::vector<std::uint32_t>({2, 3}),
      "named-window declaration/reference/inheritance AST differs");

  const auto named_refused = [&](const std::string_view candidate) {
    const auto result =
        sbsql::ParseNativeRelationalAst(sbsql::BuildCst(candidate));
    return !result.accepted() && result.root_relation_id == 0 &&
           result.window_definitions.empty() &&
           result.window_invocations.empty() && result.relations.empty();
  };
  passed &= Require(
      named_refused(
          "SELECT ROW_NUMBER() OVER derived FROM app.events WINDOW derived AS "
          "(later ORDER BY created_at), later AS (PARTITION BY account_id);"),
      "forward named-window inheritance was admitted");
  passed &= Require(
      named_refused(
          "SELECT ROW_NUMBER() OVER base FROM app.events WINDOW base AS "
          "(PARTITION BY account_id), base AS (ORDER BY created_at);"),
      "duplicate named-window declaration was admitted");
  passed &= Require(
      named_refused(
          "SELECT ROW_NUMBER() OVER derived FROM app.events WINDOW base AS "
          "(PARTITION BY account_id), derived AS (base PARTITION BY created_at);"),
      "named-window PARTITION override was admitted");
  passed &= Require(
      named_refused(
          "SELECT ROW_NUMBER() OVER missing FROM app.events WINDOW base AS "
          "(PARTITION BY account_id);"),
      "unknown named-window reference was admitted");

  const auto qualified = sbsql::ParseNativeRelationalAst(sbsql::BuildCst(
      "SELECT ROW_NUMBER() OVER framed AS sequence_no FROM app.events AS e "
      "WINDOW partitioned AS (PARTITION BY account_id), "
      "ordered AS (partitioned ORDER BY created_at), "
      "framed AS (ordered GROUPS BETWEEN 2 PRECEDING AND CURRENT ROW) "
      "QUALIFY sequence_no <= 2;"));
  passed &= Require(
      qualified.accepted() && qualified.root_relation_id == 3 &&
          qualified.relations.size() == 3 &&
          qualified.relations.back().relation_kind ==
              sbsql::NativeRelationAstKind::kQualify &&
          qualified.relations.back().input_relation_ids ==
              std::vector<std::uint32_t>({2}) &&
          qualified.relations.back().output_expression_ids ==
              std::vector<std::uint32_t>({1}) &&
          qualified.relations.back().predicate_expression_ids ==
              std::vector<std::uint32_t>({6}) &&
          qualified.expressions[4].literal_kind ==
              sbsql::NativeLiteralAstKind::kNumeric &&
          qualified.expressions[5].expression_kind ==
              sbsql::NativeExpressionAstKind::kBinary &&
          qualified.expressions[5].operator_name == "<=" &&
          qualified.expressions[5].child_expression_ids ==
              std::vector<std::uint32_t>({1, 5}),
      "typed QUALIFY relation or window-result predicate AST differs");
  for (const std::string_view comparison : {"=", "<>", "!=", "<", "<=",
                                             ">", ">="}) {
    const auto comparison_sql =
        "SELECT ROW_NUMBER() OVER (ORDER BY created_at) FROM app.events "
        "QUALIFY row_number " +
        std::string(comparison) + " 1;";
    const auto comparison_ast =
        sbsql::ParseNativeRelationalAst(sbsql::BuildCst(comparison_sql));
    if (!comparison_ast.accepted()) {
      for (const auto& diagnostic : comparison_ast.messages.diagnostics) {
        std::cerr << "QUALIFY comparison diagnostic: " << diagnostic.code
                  << " " << diagnostic.message << '\n';
      }
    }
    passed &= Require(
        comparison_ast.accepted() &&
            comparison_ast.relations.back().relation_kind ==
                sbsql::NativeRelationAstKind::kQualify &&
            comparison_ast.expressions.back().operator_name == comparison,
        "QUALIFY canonical comparison operator matrix differs");
  }
  passed &= Require(
      named_refused(
          "SELECT ROW_NUMBER() OVER (ORDER BY created_at) AS sequence_no "
          "FROM app.events QUALIFY missing <= 2;"),
      "QUALIFY unknown window-result name was admitted");
  passed &= Require(
      named_refused(
          "SELECT ROW_NUMBER() OVER (ORDER BY created_at) AS sequence_no "
          "FROM app.events QUALIFY sequence_no <= account_id;"),
      "QUALIFY nonnumeric bound was admitted");
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
  passed &= ValidateCatalogFilterComposition();
  passed &= ValidateCatalogProjectionComposition();
  passed &= ValidateCatalogJoinTailComposition();
  passed &= ValidateCatalogOrderingComposition();
  passed &= ValidateCatalogRelationRefusal();
  passed &= ValidateFamilyBoundary();
  passed &= ValidateTypedWindowVocabulary();
  passed &= ValidateTypedWindowParse();
  return passed ? EXIT_SUCCESS : EXIT_FAILURE;
}

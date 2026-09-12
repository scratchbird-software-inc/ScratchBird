// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "ast/ast.hpp"
#include "parser/cte_clause.hpp"

#include <array>
#include <iostream>
#include <string>

namespace sql = scratchbird::parser::sbsql;
namespace {
std::size_t checks = 0;
std::size_t failures = 0;
void Check(bool ok, const std::string& label) {
  ++checks;
  if (!ok) { ++failures; std::cerr << "FAIL " << label << '\n'; }
}
struct Input {
  sql::CstDocument cst;
  std::vector<const sql::Token*> tokens;
  explicit Input(const std::string& source) : cst(sql::BuildCst(source)) {
    for (const auto& token : cst.tokens)
      if (token.kind != sql::TokenKind::kEnd && !sql::IsTriviaToken(token))
        tokens.push_back(&token);
  }
  sql::WithClauseSyntax Parse(std::size_t depth = 256) const {
    return sql::ParseWithClauseSyntax(tokens, 131072, depth);
  }
  std::string Text(sql::CteTokenRange range) const {
    if (range.empty() || range.end > tokens.size()) return {};
    const auto& first = *tokens[range.begin];
    const auto& last = *tokens[range.end - 1];
    return cst.source.substr(first.offset, last.offset + last.length - first.offset);
  }
};
}

int main() {
  // Framing tuples, not an assertion that every combination is semantically
  // legal SQL. Key type/nullability, recursive references, child expressions,
  // and SEARCH/CYCLE binding belong to subsequent parser/binder stages.
  const std::array<std::string, 4> names{"r", "\"R\"", "\"recursive\"", "\"résumé\""};
  const std::array<std::string, 3> material{"", "MATERIALIZED ", "NOT MATERIALIZED "};
  const std::array<std::string, 3> unions{"UNION", "UNION ALL", "UNION DISTINCT"};
  std::size_t tuples = 0;
  for (const auto& name : names)
  for (bool recursive : {false, true})
  for (std::size_t hint = 0; hint < material.size(); ++hint)
  for (bool columns : {false, true})
  for (bool keyed : {false, true})
  for (int search = 0; search < 3; ++search)
  for (int cycle = 0; cycle < 3; ++cycle)
  for (std::size_t duplicate = 0; duplicate < unions.size(); ++duplicate) {
    ++tuples;
    const std::string body = "VALUES (1, 2) " + unions[duplicate] +
        " SELECT n + 1, m + 2 FROM " + name + " WHERE n < 3";
    const std::string outer = "SELECT n, m FROM " + name + " ORDER BY n";
    const std::string source = "WITH " + std::string(recursive ? "RECURSIVE " : "") + name +
        (columns ? " (n, m)" : "") + (keyed ? " USING KEY (n, (m + 1))" : "") +
        " AS " + material[hint] + "(" + body + ")" +
        (search == 0 ? "" : search == 1 ? " SEARCH DEPTH FIRST BY n, m SET seq" :
                                               " SEARCH BREADTH FIRST BY n, m SET seq") +
        (cycle == 0 ? "" : cycle == 1 ? " CYCLE n, m SET mark USING path" :
                                      " CYCLE n, m SET mark TO TRUE DEFAULT FALSE USING path") +
        " " + outer + ";";
    const Input input(source);
    const auto parsed = input.Parse();
    const auto label = "tuple=" + std::to_string(tuples);
    Check(!input.cst.messages.has_errors(), label + " lex/CST");
    Check(parsed.status == sql::WithClauseSyntaxStatus::kValid, label + " framing " + parsed.detail);
    Check(parsed.definitions.size() == 1, label + " definition count");
    if (parsed.definitions.size() != 1) continue;
    const auto& d = parsed.definitions.front();
    Check(parsed.recursive_keyword == recursive, label + " recursive flag");
    Check(input.Text({d.name, d.name + 1}) == name, label + " exact name");
    Check(d.columns.size() == (columns ? 2U : 0U), label + " columns");
    Check(d.key_expressions.size() == (keyed ? 2U : 0U), label + " keys");
    Check(d.materialization == (hint == 0 ? sql::CteMaterialization::kDefault :
        hint == 1 ? sql::CteMaterialization::kMaterialized : sql::CteMaterialization::kNotMaterialized),
        label + " materialization");
    Check(input.Text(d.body) == body, label + " complete actual body");
    Check(input.Text(parsed.query) == outer, label + " complete outer query");
    Check(d.union_keywords.size() == 1, label + " UNION candidates");
    if (d.union_keywords.size() == 1) {
      const auto& u = d.union_keywords.front();
      Check(input.Text({u.token, u.right_begin}) == unions[duplicate], label + " UNION tokens");
      Check(u.duplicates == (duplicate == 1 ? sql::CteUnionDuplicates::kAll :
                                             sql::CteUnionDuplicates::kDistinct), label + " duplicates");
      Check(input.Text({u.right_begin, d.body.end}) ==
          "SELECT n + 1, m + 2 FROM " + name + " WHERE n < 3", label + " actual recursive arm");
    }
    if (keyed && d.key_expressions.size() == 2) {
      Check(input.Text(d.key_expressions[0]) == "n", label + " key one");
      Check(input.Text(d.key_expressions[1]) == "(m + 1)", label + " key two");
    }
    Check(d.search.has_value() == (search != 0), label + " search presence");
    if (d.search) {
      Check(d.search->depth_first == (search == 1), label + " traversal order");
      Check(d.search->ordering_terms.size() == 2, label + " order terms");
      Check(input.Text({d.search->sequence_name, d.search->sequence_name + 1}) == "seq", label + " sequence");
    }
    Check(d.cycle.has_value() == (cycle != 0), label + " cycle presence");
    if (d.cycle) {
      Check(d.cycle->columns.size() == 2, label + " cycle columns");
      Check(input.Text({d.cycle->mark_name, d.cycle->mark_name + 1}) == "mark", label + " mark");
      Check(input.Text({d.cycle->path_name, d.cycle->path_name + 1}) == "path", label + " path");
      Check(d.cycle->mark_value.has_value() == (cycle == 2), label + " explicit mark");
      Check(d.cycle->default_value.has_value() == (cycle == 2), label + " explicit default");
      if (d.cycle->mark_value) Check(input.Text(*d.cycle->mark_value) == "TRUE", label + " mark value");
      if (d.cycle->default_value) Check(input.Text(*d.cycle->default_value) == "FALSE", label + " default value");
    }
  }
  Check(tuples == 2592, "fixed expected framing tuple count");

  const std::array<std::string, 28> malformed{
      "WITH", "WITH RECURSIVE", "WITH r", "WITH r AS", "WITH r AS () SELECT 1",
      "WITH r () AS (VALUES(1)) SELECT 1", "WITH r (n,) AS (VALUES(1)) SELECT 1",
      "WITH r (n m) AS (VALUES(1)) SELECT 1", "WITH r AS (VALUES(1))",
      "WITH r AS (VALUES(1));", "WITH r AS VALUES(1) SELECT 1",
      "WITH r AS NOT (VALUES(1)) SELECT 1", "WITH r USING (n) AS (VALUES(1)) SELECT 1",
      "WITH r USING KEY () AS (VALUES(1)) SELECT 1",
      "WITH r USING KEY (n,) AS (VALUES(1)) SELECT 1",
      "WITH r AS (VALUES(1);) SELECT 1", "WITH r AS (VALUES(1)) SELECT 1; SELECT 2",
      "WITH r AS (VALUES(1)) SELECT (1", "WITH r AS (VALUES(1)) SELECT 1)",
      "WITH r AS (VALUES(1)) SEARCH FIRST BY n SET seq SELECT 1",
      "WITH r AS (VALUES(1)) SEARCH DEPTH BY n SET seq SELECT 1",
      "WITH r AS (VALUES(1)) SEARCH DEPTH FIRST BY SET seq SELECT 1",
      "WITH r AS (VALUES(1)) CYCLE SET mark USING path SELECT 1",
      "WITH r AS (VALUES(1)) CYCLE n SET mark TO DEFAULT FALSE USING path SELECT 1",
      "WITH r AS (VALUES(1)) CYCLE n SET mark TO TRUE DEFAULT USING path SELECT 1",
      "WITH r AS (VALUES(1)) CYCLE n SET mark USING",
      "WITH r AS (VALUES(1)),", "WITH r AS (VALUES(1) SELECT 1"};
  for (const auto& source : malformed) {
    const auto parsed = Input(source).Parse();
    Check(parsed.status == sql::WithClauseSyntaxStatus::kMalformed, "malformed " + source);
    Check(parsed.definitions.empty() && parsed.query.empty(), "no partial result " + source);
  }
  const Input nested("WITH RECURSIVE r(n) AS (SELECT 'UNION ) ; ,', \"UNION\" FROM "
      "(VALUES(1) UNION ALL VALUES(2)) s UNION ALL SELECT n FROM r UNION SELECT n FROM r), "
      "t AS (WITH x AS (VALUES(3)) SELECT * FROM x) SELECT * FROM t;");
  const auto nested_result = nested.Parse();
  Check(nested_result.status == sql::WithClauseSyntaxStatus::kValid, "nested framing");
  Check(nested_result.definitions.size() == 2, "multiple definitions retained");
  if (nested_result.definitions.size() == 2) {
    Check(nested_result.definitions[0].union_keywords.size() == 2, "no nested/string/quoted UNION pollution");
    Check(nested_result.definitions[1].union_keywords.empty(), "nested WITH scope remains child");
    Check(nested.Text(nested_result.definitions[1].body) ==
          "WITH x AS (VALUES(3)) SELECT * FROM x", "nested WITH source retained");
  }
  for (std::size_t depth = 0; depth <= 257; ++depth) {
    const Input input("WITH r AS (SELECT " + std::string(depth, '(') + "1" +
                      std::string(depth, ')') + ") SELECT * FROM r");
    const auto parsed = input.Parse();
    Check(parsed.status == (depth <= 256 ? sql::WithClauseSyntaxStatus::kValid :
                                          sql::WithClauseSyntaxStatus::kLimit), "nesting " + std::to_string(depth));
  }
  const Input plain("WITH r AS (SELECT * FROM public.items) SELECT * FROM r;");
  Check(sql::ParseWithClauseSyntax(plain.tokens, plain.tokens.size(), 256).status ==
        sql::WithClauseSyntaxStatus::kValid, "exact token budget");
  Check(sql::ParseWithClauseSyntax(plain.tokens, plain.tokens.size() - 1, 256).status ==
        sql::WithClauseSyntaxStatus::kLimit, "token budget overflow");
  Check(plain.Parse(0).status == sql::WithClauseSyntaxStatus::kLimit, "zero depth budget");
  const std::array<const sql::Token*, 1> null_token{nullptr};
  Check(sql::ParseWithClauseSyntax(null_token, 1, 1).status ==
        sql::WithClauseSyntaxStatus::kMalformed, "null token");
  Check(Input("SELECT 1").Parse().status == sql::WithClauseSyntaxStatus::kNotRecognized, "non WITH");
  for (const std::string source : {
      "WITH r AS (SELECT * FROM public.items) SELECT * FROM r",
      "WITH r AS (SELECT * FROM public.items) SELECT * FROM r;",
      "WITH /*before name*/ r AS (SELECT * FROM public.items) /*body*/ SELECT * FROM r;"}) {
    const Input input(source);
    const auto ast = sql::ParseNativeRelationalAst(input.cst);
    Check(ast.accepted() && sql::IsNativeHeapCteIdentity(ast), "real heap parser preserves identity route");
    Check(ast.with_clause && ast.with_clause->status == sql::WithClauseSyntaxStatus::kValid,
          "real heap parser retains WITH structure");
  }
  const Input recursive("WITH RECURSIVE r(n) AS (VALUES(1) UNION ALL SELECT n + 2 FROM r WHERE n < 7) SELECT * FROM r");
  for (const std::string body : {
      "VALUES(2),(9)", "SELECT 7", "SELECT id FROM public.items WHERE id = 2",
      "SELECT COUNT(*) FROM public.items", "SELECT * FROM public.items LIMIT 1",
      "WITH inner_cte AS (VALUES(2),(9)) SELECT * FROM inner_cte"}) {
    for (const std::string prefix : {"WITH ", "WITH RECURSIVE "}) {
      const auto wrapped = sql::ParseNativeRelationalAst(
          Input(prefix + "r AS (" + body + ") SELECT * FROM r").cst);
      Check(wrapped.accepted(), "actual CTE child DAG: " + prefix + body);
      const auto child = sql::ParseNativeRelationalAst(Input(body).cst);
      Check(sql::NativeCteProducerRoot(wrapped) ==
                std::optional<std::uint32_t>{child.root_relation_id},
            "wrapper retains actual child root: " + body);
    }
  }
  const auto valid_wrapper = sql::ParseNativeRelationalAst(
      Input("WITH r AS (SELECT * FROM public.items LIMIT 1) SELECT * FROM r").cst);
  // Independently enumerate forbidden wrapper fields/handles. These checks
  // qualify wrapper structure only; the ordinary child validators still run.
  for (int mutation = 0; mutation < 24; ++mutation) {
    auto invalid = valid_wrapper;
    auto& cte = invalid.relations.back();
    switch (mutation) {
      case 0: cte.input_relation_ids.clear(); break;
      case 1: cte.input_relation_ids.push_back(cte.input_relation_ids[0]); break;
      case 2: cte.input_relation_ids[0] = cte.relation_id; break;
      case 3: cte.input_relation_ids[0] = 999; break;
      case 4: invalid.root_relation_id = 1; break;
      case 5: invalid.relations.front().relation_id = 0; break;
      case 6: invalid.relations.front().relation_id = cte.relation_id; break;
      case 7: invalid.relations.front().relation_id = cte.relation_id + 1; break;
      case 8: cte.relation_kind = sql::NativeRelationAstKind::kValues; break;
      case 9: cte.relation_source_ids = {1}; break;
      case 10: cte.values_row_ids = {1}; break;
      case 11: cte.output_expression_ids = {1}; break;
      case 12: cte.grouping_key_expression_ids = {1}; break;
      case 13: cte.aggregate_expression_ids = {1}; break;
      case 14: cte.predicate_expression_ids = {1}; break;
      case 15: cte.limit_expression_ids = {1}; break;
      case 16: cte.table_function_name.emplace_back(); break;
      case 17: cte.table_function_argument_expression_ids = {1}; break;
      case 18: cte.window_invocation_ids = {1}; break;
      case 19: cte.ordering_terms.emplace_back(); break;
      case 20: cte.aggregate_grouping_form = static_cast<sql::NativeAggregateGroupingForm>(255); break;
      case 21: cte.aggregate_projection_form = static_cast<sql::NativeAggregateProjectionForm>(255); break;
      case 22: cte.join_kind = static_cast<sql::NativeJoinAstKind>(255); break;
      case 23: invalid.status = sql::NativeRelationalParseStatus::kRefused; break;
    }
    Check(!sql::NativeCteProducerRoot(invalid), "malformed wrapper " + std::to_string(mutation));
  }
  const auto ast = sql::ParseNativeRelationalAst(recursive.cst);
  Check(!ast.accepted(), "unfinished recursive transport is not falsely admitted");
  Check(ast.with_clause && ast.with_clause->status == sql::WithClauseSyntaxStatus::kValid &&
        ast.with_clause->recursive_keyword && ast.with_clause->definitions.size() == 1,
        "actual recursive arms survive child-route refusal");
  if (ast.with_clause && ast.with_clause->definitions.size() == 1)
    Check(recursive.Text(ast.with_clause->definitions[0].body) ==
          "VALUES(1) UNION ALL SELECT n + 2 FROM r WHERE n < 7", "not a counter-profile substitution");
  for (const auto& source : {"WITH r AS (SELECT * FROM r) SELECT * FROM r",
                             "WITH r AS (SELECT * FROM public.items) SELECT * FROM other"})
    Check(!sql::ParseNativeRelationalAst(Input(source).cst).accepted(), "scope mismatch not admitted");
  if (checks != 60000) {
    ++failures;
    std::cerr << "FAIL fixed expected check count 60000; observed " << checks << '\n';
  }
  std::cout << "framing_tuples=" << tuples << " checks=" << checks << " failures=" << failures << '\n';
  return failures == 0 ? 0 : 1;
}

// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
// Syntax provenance only. These are actual parser-produced AST occurrences,
// never datatype identities, runtime receipts or execution permissions.
#include "lowering/lowering.hpp"
#include "cst/cst.hpp"

#include <algorithm>
#include <iostream>
#include <stdexcept>
#include <string>

namespace p = scratchbird::parser::sbsql;
using Error = p::ScalarProjectionLiteralDemandErrorV1;
namespace {
unsigned checks = 0;
void Check(bool value, const char* reason) {
  ++checks;
  if (!value) throw std::runtime_error(reason);
}
struct Fixture {
  p::CstDocument cst;
  p::NativeRelationalAstDocument native;
  // aliased_values: the wire pipeline's option (SB_PARSER_ALIASED_LITERAL_PROJECTION, 2026-12-25): `SELECT <literal> AS <name>` is the VALUES relation; the default is the standalone pipeline
  explicit Fixture(std::string sql, bool aliased_values = false)
      : cst(p::BuildCst(sql)), native(p::ParseNativeRelationalAst(cst, p::AstBuildOptions{aliased_values})) {}
  auto describe(p::ScalarProjectionLiteralDemandLimitsV1 limits = {4096, 256}) const {
    return p::DescribeScalarProjectionLiteralDemandsV1(cst, native, limits);
  }
};
void Origins(const Fixture& f, const p::ScalarProjectionLiteralDemandDescriptionV1& result) {
  Check(result.applicable && result.valid && result.error == Error::none, "valid scalar demand refused");
  for (const auto& demand : result.entries) {
    const p::NativeExpressionAstNode* origin = nullptr;
    unsigned matches = 0;
    for (const auto& expression : f.native.expressions)
      if (expression.expression_kind == p::NativeExpressionAstKind::kLiteral &&
          expression.literal_kind == demand.literal_kind &&
          expression.range.offset == demand.range.offset && expression.range.length == demand.range.length) {
        ++matches; origin = &expression;
      }
    Check(matches == 1 && origin && origin->expression_id && origin->structural_literal_occurrence_id,
          "demand lacks one exact existing native occurrence");
    Check(demand.expression_id == origin->expression_id &&
          demand.structural_literal_occurrence_id == origin->structural_literal_occurrence_id,
          "export reminted existing native identity");
    Check(demand.is_null == (demand.literal_kind == p::NativeLiteralAstKind::kNull), "NULL source kind changed");
    Check(!demand.canonical_type_name.empty() && demand.canonical_type_name != "null", "unresolved result exposed");
  }
}
void NoPartial(const p::ScalarProjectionLiteralDemandDescriptionV1& result, Error error) {
  Check(!result.valid && result.entries.empty() && result.error == error, "failed demand exposed partial entries/wrong failure");
}
void NumericAndIndependentOrigins() {
  Fixture f("SELECT 7;");
  const auto result = f.describe(); Origins(f, result);
  Check(result.entries.size() == 1 && result.entries[0].canonical_type_name == "int64" &&
        result.entries[0].structural_path == "projection_0_", "numeric literal provenance/result path");
  auto reordered = f;
  std::reverse(reordered.native.expressions.begin(), reordered.native.expressions.end());
  const auto again = reordered.describe(); Origins(reordered, again);
  Check(again.entries[0].expression_id == result.entries[0].expression_id, "native record ordering changed identity");
  auto removed = f; removed.native.expressions.clear();
  NoPartial(removed.describe(), Error::missing_literal_mapping);
  auto zero = f; zero.native.expressions[0].expression_id = 0;
  NoPartial(zero.describe(), Error::missing_literal_mapping);
  zero = f; zero.native.expressions[0].structural_literal_occurrence_id = 0;
  NoPartial(zero.describe(), Error::missing_literal_mapping);
  auto duplicate = f; duplicate.native.expressions.push_back(duplicate.native.expressions[0]);
  NoPartial(duplicate.describe(), Error::ambiguous_literal_mapping);
  auto shifted = f; ++shifted.native.expressions[0].range.offset;
  NoPartial(shifted.describe(), Error::missing_literal_mapping);
  auto changed = f; changed.native.expressions[0].literal_kind = p::NativeLiteralAstKind::kString;
  NoPartial(changed.describe(), Error::missing_literal_mapping);
}
void ContextualBooleanOrigins() {
  for (const std::string sql : {
       "SELECT IIF(TRUE,NULL,FALSE);", "SELECT COALESCE(NULL,TRUE);",
       // OLD (before 2026-12-24): "SELECT TRUE,FALSE,TRUE;" was a scalar-syntax inventory. NEW: a bare list of boolean literals is the typed literal VALUES relation (owner 2026-11-26
       // item 5), no longer a scalar inventory; the aliased form stays one and exercises the same machinery (three Boolean occurrences, in source order)
       "SELECT CAST(NULL AS BOOLEAN);", "SELECT TRUE AS a,FALSE AS b,TRUE AS c;",
       "SELECT IIF(TRUE,CAST(NULL AS BOOLEAN),FALSE);"}) {
    Fixture f(sql);
    const auto result = f.describe(); Origins(f, result);
    Check(!result.entries.empty(), "Boolean scalar lacks actual literal occurrences");
    for (const auto& entry : result.entries)
      Check(entry.canonical_type_name == "boolean", "Boolean context was not propagated through selector/CAST");
    const auto literals = std::count_if(f.native.expressions.begin(), f.native.expressions.end(), [](const auto& node) {
      return node.expression_kind == p::NativeExpressionAstKind::kLiteral;
    });
    Check(result.entries.size() == static_cast<std::size_t>(literals), "CAST metadata became a literal occurrence");
  }
  Fixture strings("SELECT IIF(TRUE,NULL,'other');");
  const auto result = strings.describe(); Origins(strings, result);
  Check(result.entries.size() == 3, "mixed family selector lost condition/result origins");
  Check(result.entries[0].canonical_type_name == "boolean" &&
        result.entries[0].structural_path == "projection_0_arg_0_" &&
        result.entries[1].canonical_type_name == "character" && result.entries[1].is_null &&
        result.entries[1].structural_path == "projection_0_arg_1_" &&
        result.entries[2].canonical_type_name == "character", "condition/result contextual types or paths wrong");
  Fixture repeated("SELECT TRUE AS a,FALSE AS b,TRUE AS c;");  // OLD: "SELECT TRUE,FALSE,TRUE;" (see above)
  const auto repeated_result = repeated.describe(); Origins(repeated, repeated_result);
  Check(repeated_result.entries.size() == 3 &&
        repeated_result.entries[0].structural_literal_occurrence_id !=
            repeated_result.entries[2].structural_literal_occurrence_id,
        "equal Boolean spelling was treated as one occurrence");
}
void FailureAndLimits() {
  Fixture f("SELECT 7;");
  NoPartial(f.describe({0, 1}), Error::invalid_limits);
  NoPartial(f.describe({1, 0}), Error::invalid_limits);
  const auto exact = f.describe({1, 1}); Origins(f, exact);
  Fixture other("VALUES (7);");
  const auto not_applicable = other.describe();
  Check(!not_applicable.applicable, "non-scalar statement marked applicable");
  NoPartial(not_applicable, Error::not_applicable);
  for (std::string sql : {"SELECT NULL;", "SELECT COALESCE(NULL,NULL);", "SELECT IIF(TRUE,NULL,NULL);"}) {
    Fixture invalid(sql); NoPartial(invalid.describe(), Error::invalid_scalar);
  }
  Fixture multiple("SELECT TRUE AS a,FALSE AS b;");  // OLD: "SELECT TRUE,FALSE;" (a bare boolean list is now the VALUES relation, not a scalar inventory)
  NoPartial(multiple.describe({1, 256}), Error::resource_limit);
  Fixture deep("SELECT IIF(TRUE,NULL,FALSE);");
  NoPartial(deep.describe({16, 1}), Error::resource_limit);
  Fixture cast_metadata("SELECT CAST(3 AS DECIMAL(18,2));");
  const auto cast_demand = cast_metadata.describe({1, 16});
  Origins(cast_metadata, cast_demand);
  Check(cast_demand.entries.size() == 1,
        "numeric CAST metadata incorrectly consumes literal entry allowance");
  Check(cast_demand.entries[0].canonical_type_name == "int64" &&
        cast_demand.entries[0].literal_kind == p::NativeLiteralAstKind::kNumeric &&
        cast_demand.entries[0].structural_path == "projection_0_arg_0_",
        "CAST target metadata changed source type/kind/provenance path");
  Fixture null_cast_metadata("SELECT CAST(NULL AS BOOLEAN);");
  const auto null_cast_demand = null_cast_metadata.describe({1, 2});
  Origins(null_cast_metadata, null_cast_demand);
  Check(null_cast_demand.entries.size() == 1 && null_cast_demand.entries[0].is_null &&
        null_cast_demand.entries[0].canonical_type_name == "boolean",
        "synthetic CAST label consumed the only contextual NULL occurrence slot");
  NoPartial(null_cast_metadata.describe({1, 1}), Error::resource_limit);
  Fixture sibling_casts("SELECT CAST(3 AS DECIMAL(18,2)),CAST(4 AS DECIMAL(18,2));");
  const auto sibling_demand = sibling_casts.describe({2, 16});
  Origins(sibling_casts, sibling_demand);
  Check(sibling_demand.entries.size() == 2 &&
        sibling_demand.entries[0].structural_path == "projection_0_arg_0_" &&
        sibling_demand.entries[1].structural_path == "projection_1_arg_0_" &&
        sibling_demand.entries[0].structural_literal_occurrence_id !=
            sibling_demand.entries[1].structural_literal_occurrence_id,
        "sibling CAST metadata altered actual occurrence capacity/order");
  NoPartial(sibling_casts.describe({1, 16}), Error::resource_limit);
  auto cyclic = f;
  cyclic.native.expressions[0].child_expression_ids = {cyclic.native.expressions[0].expression_id};
  NoPartial(cyclic.describe(), Error::invalid_scalar);
  auto missing_child = f;
  missing_child.native.expressions[0].child_expression_ids = {0};
  NoPartial(missing_child.describe(), Error::invalid_scalar);
}
void ActualParserInventory() {
  // OLD (before 2026-12-24): "SELECT TRUE;" was in the list below (a scalar inventory that must not become an executable graph). NEW (owner 2026-11-26 item 5): a bare projection of
  // boolean (and exact-decimal) LITERALS is the typed literal VALUES relation, exactly like the numeric one; the checks after the loop pin that, and the aliased boolean forms stay inventories.
  for (const std::string sql : {"SELECT IIF(NULL,TRUE,FALSE);",
       "SELECT CAST(NULL AS BOOLEAN);", "SELECT CAST(3 AS DECIMAL(18,2));",
       "SELECT CAST(3 AS DOUBLE PRECISION);", "SELECT TRUE AS flag,FALSE other;",
       "SELECT 'FROM',\"FROM\";", "SELECT 'DOCUMENT_SOURCE';",
       "SELECT IIF(TRUE,NULL,'TIME_RANGE');", "SELECT TRUE AS date;"}) {
    Fixture f(sql);
    Check(f.native.scalar_syntax_accepted() && !f.native.recognized() &&
          !f.native.accepted() && !f.native.root_relation_id &&
          f.native.relations.empty(), "scalar inventory falsely became executable native graph");
    std::uint64_t last_occurrence = 0;
    std::size_t last_offset = 0;
    for (const auto& node : f.native.expressions) {
      Check(node.expression_id && node.range.length &&
            node.range.offset + node.range.length <= sql.size(), "real scalar source extent missing");
      if (node.expression_kind == p::NativeExpressionAstKind::kLiteral) {
        Check(node.structural_literal_occurrence_id == ++last_occurrence &&
              node.range.offset >= last_offset, "literal identities must retain actual source order");
        last_offset = node.range.offset;
      }
      if (node.cast_target_range) {
        Check(node.operator_name == "CAST" && node.child_expression_ids.size() == 1 &&
              node.cast_target_range->offset > node.range.offset, "CAST target lost non-value provenance");
      }
    }
    if (sql.find("DECIMAL") != std::string::npos)
      Check(last_occurrence == 1, "CAST precision and scale manufactured value occurrences");
  }
  for (const std::string sql : {"SELECT;", "SELECT TRUE,;", "SELECT TRUE AS;",
       "SELECT CAST(NULL AS);", "SELECT CAST(NULL AS x.);",
       "SELECT CAST(3 AS DECIMAL(1,));", "SELECT IIF(TRUE,FALSE,);",
       "SELECT CAST(NULL AS BOOLEAN garbage);", "SELECT CAST(NULL AS BOOLEAN AS alias);"}) {
    Fixture f(sql);
    Check(!f.native.scalar_syntax_accepted() && !f.native.accepted() &&
          f.native.expressions.empty() && f.native.messages.has_errors(),
          "invalid scalar syntax retained partial occurrence inventory");
  }
  // SQL '--' is a comment, so exercise actual unary tokens separated by spaces.
  std::string prefix = "SELECT ";
  for (unsigned i = 0; i != 300; ++i) prefix += "NOT ";
  Fixture deep(prefix + "TRUE;");
  Check(deep.native.messages.has_errors() && deep.native.expressions.empty(),
        "unary chain escaped parser's bounded stack depth");
  Fixture numeric("SELECT 7;");
  Check(numeric.native.accepted() && numeric.native.recognized() &&
        !numeric.native.scalar_syntax_accepted() && numeric.native.root_relation_id,
        "existing executable single numeric SELECT route changed");
  for (const std::string sql : {"SELECT TRUE;", "SELECT FALSE", "select true;", "SELECT TRUE,FALSE;", "SELECT TRUE,1;", "SELECT 1,TRUE;", "SELECT 1.5;", "SELECT 1.5,2.5;", "SELECT 1,2.5;",
                                "SELECT 9223372036854775808;", "SELECT -1.5;"}) {
    Fixture bare(sql);
    Check(bare.native.accepted() && bare.native.recognized() && !bare.native.scalar_syntax_accepted() && bare.native.root_relation_id &&
              bare.native.relations.size() == 1 && bare.native.relations[0].relation_kind == p::NativeRelationAstKind::kValues,
          ("a bare projection of boolean/decimal literals is the typed literal VALUES relation: " + sql).c_str());
  }
  // OLD (before 2026-12-25): "an all-integer bare list keeps the scalar-projection route": plain_list("SELECT 1,2;").native.scalar_syntax_accepted() && !recognized().
  // NEW (owner 2026-11-28 Q5): a pure-integer list is the VALUES relation like every other bare literal list.
  for (const std::string sql : {"SELECT 1,2;", "SELECT 1,-2,3", "select 7 , 8 ;"}) {
    Fixture plain_list(sql);
    Check(plain_list.native.accepted() && plain_list.native.recognized() && !plain_list.native.scalar_syntax_accepted() && plain_list.native.relations.size() == 1 &&
              plain_list.native.relations[0].relation_kind == p::NativeRelationAstKind::kValues && plain_list.native.relations[0].column_aliases.empty(),
          ("an all-integer bare list is the typed literal VALUES relation: " + sql).c_str());
  }
  // ALIASED bare literals (owner 2026-11-28 Q3): with the wire pipeline's option the VALUES relation, the alias kept as written in relation.column_aliases (the parser's display name; an
  // unaliased item is column<N>). WITHOUT the option (the standalone pipeline) the aliased form keeps its scalar-inventory route, so the standalone conformance tests are unchanged.
  {
    struct Aliased { const char* sql; std::vector<std::string> names; };
    for (const auto& item : {Aliased{"SELECT TRUE AS date;", {"date"}}, Aliased{"SELECT 1.5 AS x", {"x"}}, Aliased{"SELECT 1 AS one", {"one"}},
                             Aliased{"SELECT TRUE AS t, 1.5 AS d, 2 AS i", {"t", "d", "i"}}, Aliased{"SELECT 1 AS one, 2", {"one", "column2"}}, Aliased{"SELECT 1 AS \"Q c\"", {"Q c"}}}) {
      Fixture aliased(item.sql, true);
      Check(aliased.native.accepted() && aliased.native.recognized() && !aliased.native.scalar_syntax_accepted() && aliased.native.relations.size() == 1 &&
                aliased.native.relations[0].relation_kind == p::NativeRelationAstKind::kValues && aliased.native.relations[0].column_aliases.size() == item.names.size(),
            (std::string("an aliased bare literal list is the VALUES relation with display aliases (wire option): ") + item.sql).c_str());
      for (std::size_t i = 0; i < item.names.size() && i < aliased.native.relations[0].column_aliases.size(); ++i)
        Check(aliased.native.relations[0].column_aliases[i].spelling == item.names[i], (std::string("alias ") + std::to_string(i) + " is kept as written: " + item.sql).c_str());
      Fixture standalone(item.sql, false);
      Check(!(standalone.native.recognized() && standalone.native.accepted() && !standalone.native.scalar_syntax_accepted()),
            (std::string("without the wire option the aliased form is NOT the VALUES relation: ") + item.sql).c_str());
    }
    // an alias WITHOUT AS and an aliased parameter keep the scalar-projection route even with the option
    Fixture no_as("SELECT 1 one;", true);
    Check(no_as.native.scalar_syntax_accepted() && !no_as.native.recognized(), "an alias written without AS keeps the scalar-projection route");
  }
  // an expression or a parameter beside a boolean is not the literal list
  for (const std::string sql : {"SELECT TRUE,?;", "SELECT NOT TRUE;", "SELECT TRUE AND FALSE;"}) {
    Fixture not_list(sql);
    Check(!(not_list.native.recognized() && not_list.native.accepted()), ("not a bare literal list: " + sql).c_str());
  }
  Fixture parameter("SELECT ?;");
  Check(parameter.native.accepted() && parameter.native.recognized(),
        "existing executable parameter SELECT route changed");
}
}  // namespace
int main() {
  try {
    NumericAndIndependentOrigins();
    ContextualBooleanOrigins();
    FailureAndLimits();
    ActualParserInventory();
    std::cout << "scalar_projection_literal_demand=passed checks=" << checks << '\n';
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "scalar_projection_literal_demand=failed checks=" << checks << " reason=" << error.what() << '\n';
    return 1;
  }
}

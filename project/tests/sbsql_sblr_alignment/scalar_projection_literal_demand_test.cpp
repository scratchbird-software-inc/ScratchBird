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
  explicit Fixture(std::string sql) : cst(p::BuildCst(sql)), native(p::ParseNativeRelationalAst(cst)) {}
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
       "SELECT CAST(NULL AS BOOLEAN);", "SELECT TRUE,FALSE,TRUE;",
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
  Fixture repeated("SELECT TRUE,FALSE,TRUE;");
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
  Fixture multiple("SELECT TRUE,FALSE;");
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
  for (const std::string sql : {"SELECT TRUE;", "SELECT IIF(NULL,TRUE,FALSE);",
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

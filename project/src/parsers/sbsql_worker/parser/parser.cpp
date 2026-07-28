// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "ast/ast.hpp"

#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace scratchbird::parser::sbsql {
namespace {

constexpr std::size_t kMaximumNativeRelationalTokens = 131072;
constexpr std::size_t kMaximumNativeExpressionDepth = 256;

std::string CanonicalTokenText(const Token& token) {
  if (!token.canonical_text.empty()) {
    return ToUpperAscii(token.canonical_text);
  }
  return ToUpperAscii(token.text);
}

SourceRange Span(const Token& first, const Token& last) {
  SourceRange range = TokenSourceRange(first);
  range.length = last.offset + last.length - first.offset;
  range.end_line = last.end_line;
  range.end_column = last.end_column;
  return range;
}

std::optional<NativeLiteralAstKind> LiteralKindFor(const TokenKind kind) {
  switch (kind) {
    case TokenKind::kNumericLiteral: return NativeLiteralAstKind::kNumeric;
    case TokenKind::kStringLiteral: return NativeLiteralAstKind::kString;
    case TokenKind::kBinaryLiteral: return NativeLiteralAstKind::kBinary;
    case TokenKind::kTemporalLiteral: return NativeLiteralAstKind::kTemporal;
    case TokenKind::kUuidLiteral: return NativeLiteralAstKind::kUuid;
    case TokenKind::kBooleanLiteral: return NativeLiteralAstKind::kBoolean;
    case TokenKind::kNullLiteral: return NativeLiteralAstKind::kNull;
    case TokenKind::kDefaultLiteral: return NativeLiteralAstKind::kDefault;
    case TokenKind::kDocumentLiteral: return NativeLiteralAstKind::kDocument;
    case TokenKind::kVectorLiteral: return NativeLiteralAstKind::kVector;
    case TokenKind::kRegexLiteral: return NativeLiteralAstKind::kRegex;
    case TokenKind::kRangeLiteral: return NativeLiteralAstKind::kRange;
    default: return std::nullopt;
  }
}

struct BinaryOperator {
  int precedence{0};
  std::string canonical_name;
};

std::optional<BinaryOperator> BinaryOperatorFor(const Token& token) {
  const auto word = CanonicalTokenText(token);
  if (word == "OR") return BinaryOperator{1, word};
  if (word == "AND") return BinaryOperator{2, word};
  if (word == "=" || word == "<>" || word == "!=" || word == "<" ||
      word == "<=" || word == ">" || word == ">=" || word == "LIKE" ||
      word == "ILIKE" || word == "IS") {
    return BinaryOperator{3, word};
  }
  if (word == "||") return BinaryOperator{4, word};
  if (word == "+" || word == "-") return BinaryOperator{5, word};
  if (word == "*" || word == "/" || word == "%") {
    return BinaryOperator{6, word};
  }
  return std::nullopt;
}

class NativeRelationalParser final {
 public:
  explicit NativeRelationalParser(const CstDocument& cst) : cst_(cst) {
    tokens_.reserve(cst.tokens.size());
    for (const auto& token : cst.tokens) {
      if (token.kind == TokenKind::kEnd || IsTriviaToken(token)) continue;
      tokens_.push_back(&token);
    }
  }

  NativeRelationalAstDocument Parse() {
    // QOW-SOURCE-QRY-006-TEMPORAL-REFUSAL-V1
    // Temporal table sources are a closed, unapproved native-query profile.
    // Recognize their tokenized grammar only to publish the canonical refusal;
    // never translate them through a donor parser or a generic query route.
    if (!tokens_.empty() &&
        (IsWord(*tokens_.front(), "SELECT") ||
         IsWord(*tokens_.front(), "WITH"))) {
      if (const auto temporal = FindTemporalTableSource();
          temporal.has_value()) {
        document_.status = NativeRelationalParseStatus::kRefused;
        document_.temporal_table_source_refusal = *temporal;
        document_.messages.diagnostics.push_back(MakeDiagnostic(
            "QOW-DIAG-QRY-006-TEMPORAL-REFUSAL-V1", "ERROR",
            "unapproved temporal table source is outside the native relational profile",
            "sbp_sbsql.native_relational_parser",
            {{"axis", TemporalAxisName(temporal->axis)},
             {"form", TemporalFormName(temporal->form)},
             {"authority", "QOW-AUTH-QRY-006-TEMPORAL-REFUSAL-V1"}}));
        return FinishRefusal();
      }
    }
    if (!tokens_.empty() && IsWord(*tokens_.front(), "SELECT") &&
        LooksLikeSupportedGroupingQuery()) {
      return ParseGroupedAggregateSelect();
    }
    if (tokens_.empty() || !IsWord(*tokens_.front(), "VALUES")) {
      return std::move(document_);
    }

    document_.status = NativeRelationalParseStatus::kRefused;
    if (cst_.messages.has_errors()) {
      document_.messages = cst_.messages;
      return FinishRefusal();
    }
    if (tokens_.size() > kMaximumNativeRelationalTokens) {
      Refuse("token_limit_exceeded", "native relational token limit exceeded");
      return FinishRefusal();
    }

    const Token& values_token = Consume();
    std::optional<std::size_t> row_arity;
    while (!AtEnd() && !AtSymbol(";")) {
      const auto row_id = ParseValuesRow();
      if (!row_id.has_value()) return FinishRefusal();
      const auto& row = document_.values_rows[*row_id - 1];
      if (!row_arity.has_value()) {
        row_arity = row.expression_ids.size();
      } else if (*row_arity != row.expression_ids.size()) {
        Refuse("row_arity_mismatch",
               "VALUES rows must contain the same number of expressions");
        return FinishRefusal();
      }
      if (!AtSymbol(",")) break;
      Consume();
      if (AtEnd() || AtSymbol(";") || AtSymbol(")")) {
        Refuse("missing_row_after_separator",
               "VALUES row separator must be followed by a row constructor");
        return FinishRefusal();
      }
    }

    if (document_.values_rows.empty()) {
      Refuse("missing_values_row", "VALUES requires at least one row constructor");
      return FinishRefusal();
    }

    const Token& relation_end = Previous();
    if (AtSymbol(";")) Consume();
    if (!AtEnd()) {
      Refuse("trailing_input", "unexpected input follows the VALUES relation");
      return FinishRefusal();
    }

    NativeRelationAstNode relation;
    relation.relation_id = 1;
    relation.relation_kind = NativeRelationAstKind::kValues;
    relation.range = Span(values_token, relation_end);
    relation.values_row_ids.reserve(document_.values_rows.size());
    for (const auto& row : document_.values_rows) {
      relation.values_row_ids.push_back(row.row_id);
    }
    relation.output_expression_ids =
        document_.values_rows.front().expression_ids;
    document_.relations.push_back(std::move(relation));
    document_.root_relation_id = 1;
    document_.status = NativeRelationalParseStatus::kAccepted;
    return std::move(document_);
  }

 private:
  static std::string TemporalAxisName(const NativeTemporalTableAxis axis) {
    switch (axis) {
      case NativeTemporalTableAxis::kSystemTime: return "system_time";
      case NativeTemporalTableAxis::kValidTime: return "valid_time";
    }
    return "unknown";
  }

  static std::string TemporalFormName(const NativeTemporalTableForm form) {
    switch (form) {
      case NativeTemporalTableForm::kUnspecified: return "unspecified";
      case NativeTemporalTableForm::kAsOf: return "as_of";
      case NativeTemporalTableForm::kAll: return "all";
      case NativeTemporalTableForm::kBetween: return "between";
      case NativeTemporalTableForm::kFromTo: return "from_to";
    }
    return "unknown";
  }

  std::optional<NativeTemporalTableSourceRefusal>
  FindTemporalTableSource() const {
    bool table_source_domain = false;
    int depth = 0;
    for (std::size_t index = 0; index < tokens_.size(); ++index) {
      const auto& token = *tokens_[index];
      if (token.text == "(") {
        ++depth;
        continue;
      }
      if (token.text == ")") {
        if (depth > 0) --depth;
        continue;
      }
      const auto word = CanonicalTokenText(token);
      if (word == "FROM" || word == "JOIN") {
        table_source_domain = true;
        continue;
      }
      if (!table_source_domain || word != "FOR") continue;

      std::size_t axis_index = index + 1;
      bool leading_all = false;
      if (axis_index < tokens_.size() &&
          IsWord(*tokens_[axis_index], "ALL")) {
        leading_all = true;
        ++axis_index;
      }
      if (axis_index >= tokens_.size()) continue;

      NativeTemporalTableAxis axis;
      std::size_t after_axis = axis_index + 1;
      if (IsWord(*tokens_[axis_index], "SYSTEM_TIME")) {
        axis = NativeTemporalTableAxis::kSystemTime;
      } else if (IsWord(*tokens_[axis_index], "VALID_TIME")) {
        axis = NativeTemporalTableAxis::kValidTime;
      } else if (after_axis < tokens_.size() &&
                 IsWord(*tokens_[axis_index], "SYSTEM") &&
                 IsWord(*tokens_[after_axis], "TIME")) {
        axis = NativeTemporalTableAxis::kSystemTime;
        ++after_axis;
      } else if (after_axis < tokens_.size() &&
                 IsWord(*tokens_[axis_index], "VALID") &&
                 IsWord(*tokens_[after_axis], "TIME")) {
        axis = NativeTemporalTableAxis::kValidTime;
        ++after_axis;
      } else {
        continue;
      }

      NativeTemporalTableForm form = leading_all
                                         ? NativeTemporalTableForm::kAll
                                         : NativeTemporalTableForm::kUnspecified;
      std::size_t range_end = after_axis - 1;
      if (after_axis < tokens_.size()) {
        if (IsWord(*tokens_[after_axis], "ALL")) {
          form = NativeTemporalTableForm::kAll;
          range_end = after_axis;
        } else if (IsWord(*tokens_[after_axis], "BETWEEN")) {
          form = NativeTemporalTableForm::kBetween;
          range_end = after_axis;
        } else if (IsWord(*tokens_[after_axis], "FROM")) {
          form = NativeTemporalTableForm::kFromTo;
          range_end = after_axis;
        } else if (after_axis + 1 < tokens_.size() &&
                   IsWord(*tokens_[after_axis], "AS") &&
                   IsWord(*tokens_[after_axis + 1], "OF")) {
          form = NativeTemporalTableForm::kAsOf;
          range_end = after_axis + 1;
        }
      }
      return NativeTemporalTableSourceRefusal{
          axis, form, Span(*tokens_[index], *tokens_[range_end])};
    }
    return std::nullopt;
  }

  bool AtEnd() const { return cursor_ >= tokens_.size(); }

  const Token& Current() const { return *tokens_[cursor_]; }

  const Token& Previous() const { return *tokens_[cursor_ - 1]; }

  const Token& Consume() { return *tokens_[cursor_++]; }

  bool AtSymbol(const std::string_view symbol) const {
    return !AtEnd() && Current().text == symbol;
  }

  static bool IsWord(const Token& token, const std::string_view word) {
    return CanonicalTokenText(token) == word;
  }

  static bool IsNameToken(const Token& token) {
    return token.kind == TokenKind::kIdentifier ||
           token.kind == TokenKind::kKeyword;
  }

  bool LooksLikeSupportedGroupingQuery() const {
    bool has_count = false;
    bool has_sum = false;
    bool has_inline_values_source = false;
    for (std::size_t index = 0; index < tokens_.size(); ++index) {
      has_count = has_count || IsWord(*tokens_[index], "COUNT");
      has_sum = has_sum || IsWord(*tokens_[index], "SUM");
      if (index + 2 < tokens_.size() &&
          IsWord(*tokens_[index], "FROM") &&
          tokens_[index + 1]->text == "(" &&
          IsWord(*tokens_[index + 2], "VALUES")) {
        has_inline_values_source = true;
      }
    }
    for (std::size_t index = 0; index + 2 < tokens_.size(); ++index) {
      if (IsWord(*tokens_[index], "GROUP") &&
          IsWord(*tokens_[index + 1], "BY")) {
        const bool fixed_two_key_form =
            IsWord(*tokens_[index + 2], "ROLLUP") ||
            IsWord(*tokens_[index + 2], "CUBE") ||
            (index + 3 < tokens_.size() &&
             IsWord(*tokens_[index + 2], "GROUPING") &&
             IsWord(*tokens_[index + 3], "SETS"));
        if (fixed_two_key_form ||
            (has_count && has_sum && has_inline_values_source &&
             IsNameToken(*tokens_[index + 2]))) {
          return true;
        }
      }
    }
    return false;
  }

  bool RequireWord(const std::string_view word,
                   const std::string_view reason,
                   const std::string_view message) {
    if (AtEnd() || !IsWord(Current(), word)) {
      Refuse(reason, message);
      return false;
    }
    Consume();
    return true;
  }

  bool RequireSymbol(const std::string_view symbol,
                     const std::string_view reason,
                     const std::string_view message) {
    if (!AtSymbol(symbol)) {
      Refuse(reason, message);
      return false;
    }
    Consume();
    return true;
  }

  std::optional<std::uint32_t> ParseCountStar() {
    if (AtEnd() || !IsWord(Current(), "COUNT")) {
      Refuse("count_star_required",
             "native grouped aggregate profile requires COUNT(*)");
      return std::nullopt;
    }
    const Token& first = Consume();
    if (!RequireSymbol("(", "count_star_open_required",
                       "COUNT(*) requires an opening parenthesis") ||
        !RequireSymbol("*", "count_star_wildcard_required",
                       "native grouped aggregate profile requires COUNT(*)") ||
        !AtSymbol(")")) {
      if (!document_.messages.has_errors()) {
        Refuse("count_star_close_required",
               "COUNT(*) requires a closing parenthesis");
      }
      return std::nullopt;
    }
    const Token& last = Consume();
    NativeExpressionAstNode count;
    count.expression_id = NextExpressionId();
    count.expression_kind = NativeExpressionAstKind::kFunctionCall;
    count.operator_name = "COUNT";
    count.spelling = SourceSpelling(first, last);
    count.range = Span(first, last);
    document_.expressions.push_back(std::move(count));
    return document_.expressions.back().expression_id;
  }

  std::optional<std::uint32_t> ParseGroupingSpecialForm(
      const std::string_view surface_name,
      const std::string_view canonical_operator,
      const NativeExpressionAstKind expression_kind,
      const std::vector<std::uint32_t>& child_expression_ids,
      const std::vector<std::string>& expected_argument_names) {
    if (child_expression_ids.empty() ||
        child_expression_ids.size() != expected_argument_names.size() ||
        (expression_kind != NativeExpressionAstKind::kUnary &&
         expression_kind != NativeExpressionAstKind::kBinary) ||
        AtEnd() || !IsWord(Current(), surface_name)) {
      Refuse("grouping_metadata_function_required",
             "native grouping metadata projection is incomplete or out of order");
      return std::nullopt;
    }
    const Token& first = Consume();
    if (!RequireSymbol("(", "grouping_metadata_open_required",
                       "grouping metadata special form requires an opening parenthesis")) {
      return std::nullopt;
    }
    for (std::size_t index = 0; index < expected_argument_names.size(); ++index) {
      if (AtEnd() || !IsNameToken(Current()) ||
          CanonicalTokenText(Current()) != expected_argument_names[index]) {
        Refuse("grouping_metadata_argument_invalid",
               "grouping metadata arguments must be the projected keys in exact order");
        return std::nullopt;
      }
      Consume();
      if (index + 1 < expected_argument_names.size() &&
          !RequireSymbol(",", "grouping_metadata_separator_required",
                         "GROUPING_ID keys must be comma separated")) {
        return std::nullopt;
      }
    }
    if (!AtSymbol(")")) {
      Refuse("grouping_metadata_close_required",
             "grouping metadata special form has unexpected arguments");
      return std::nullopt;
    }
    const Token& last = Consume();
    NativeExpressionAstNode expression;
    expression.expression_id = NextExpressionId();
    expression.expression_kind = expression_kind;
    expression.child_expression_ids = child_expression_ids;
    expression.operator_name = std::string(canonical_operator);
    expression.spelling = SourceSpelling(first, last);
    expression.range = Span(first, last);
    document_.expressions.push_back(std::move(expression));
    return document_.expressions.back().expression_id;
  }

  NativeRelationalAstDocument ParseGroupedAggregateSelect() {
    // QOW-SOURCE-QRY-001-GROUPING-SETS-V1
    document_.status = NativeRelationalParseStatus::kRefused;
    if (cst_.messages.has_errors()) {
      document_.messages = cst_.messages;
      return FinishRefusal();
    }
    if (tokens_.size() > kMaximumNativeRelationalTokens) {
      Refuse("token_limit_exceeded", "native relational token limit exceeded");
      return FinishRefusal();
    }

    const Token& select_token = Consume();
    const auto key_a = ParseExpression(0, 0);
    if (!key_a.has_value() ||
        !RequireSymbol(",", "select_separator_required",
                       "native grouped aggregate projection requires aggregate expressions")) {
      return FinishRefusal();
    }
    const bool one_key_grouping_profile =
        !AtEnd() && IsWord(Current(), "COUNT");
    std::optional<std::uint32_t> key_b;
    if (!one_key_grouping_profile) {
      key_b = ParseExpression(0, 0);
      if (!key_b.has_value() ||
          !RequireSymbol(",", "select_separator_required",
                         "two-key grouped aggregate projection requires COUNT(*)")) {
        return FinishRefusal();
      }
    }
    const auto count = ParseCountStar();
    if (!count.has_value() ||
        !RequireSymbol(",", "select_separator_required",
                       "native grouped aggregate projection requires SUM after COUNT(*)")) {
      return FinishRefusal();
    }
    const auto sum = ParseExpression(0, 0);
    if (!sum.has_value()) {
      return FinishRefusal();
    }

    const auto& key_a_expression = document_.expressions[*key_a - 1];
    const NativeExpressionAstNode* key_b_expression =
        key_b.has_value() ? &document_.expressions[*key_b - 1] : nullptr;
    const auto& sum_expression = document_.expressions[*sum - 1];
    if (key_a_expression.expression_kind != NativeExpressionAstKind::kIdentifier ||
        (!one_key_grouping_profile &&
         (key_b_expression == nullptr ||
          key_b_expression->expression_kind !=
              NativeExpressionAstKind::kIdentifier)) ||
        sum_expression.expression_kind != NativeExpressionAstKind::kFunctionCall ||
        ToUpperAscii(sum_expression.operator_name) != "SUM" ||
        sum_expression.child_expression_ids.size() != 1) {
      Refuse("grouping_projection_shape_invalid",
             "native grouped aggregate profile requires one or two keys, COUNT(*), and SUM(value)");
      return FinishRefusal();
    }
    const auto key_a_spelling = key_a_expression.spelling;
    const auto key_b_spelling =
        key_b_expression == nullptr ? std::string{} : key_b_expression->spelling;
    const auto sum_child = sum_expression.child_expression_ids.front();

    NativeAggregateProjectionForm projection_form =
        one_key_grouping_profile
            ? NativeAggregateProjectionForm::kKeyCountSum
            : NativeAggregateProjectionForm::kKeysCountSum;
    std::vector<std::uint32_t> grouping_projection_ids;
    if (!one_key_grouping_profile && AtSymbol(",")) {
      // QOW-SOURCE-QRY-001-GROUPING-METADATA-V1
      projection_form =
          NativeAggregateProjectionForm::kKeysCountSumGrouping;
      Consume();
      const auto grouping_a = ParseGroupingSpecialForm(
          "GROUPING", "grouping", NativeExpressionAstKind::kUnary,
          {*key_a}, {ToUpperAscii(key_a_spelling)});
      if (!grouping_a.has_value() ||
          !RequireSymbol(",", "grouping_metadata_separator_required",
                         "GROUPING(key_a) must be followed by GROUPING(key_b)")) {
        return FinishRefusal();
      }
      const auto grouping_b = ParseGroupingSpecialForm(
          "GROUPING", "grouping", NativeExpressionAstKind::kUnary,
          {*key_b}, {ToUpperAscii(key_b_spelling)});
      if (!grouping_b.has_value() ||
          !RequireSymbol(",", "grouping_metadata_separator_required",
                         "GROUPING(key_b) must be followed by GROUPING_ID")) {
        return FinishRefusal();
      }
      const auto grouping_id = ParseGroupingSpecialForm(
          "GROUPING_ID", "grouping_id", NativeExpressionAstKind::kBinary,
          {*key_a, *key_b},
          {ToUpperAscii(key_a_spelling), ToUpperAscii(key_b_spelling)});
      if (!grouping_id.has_value()) return FinishRefusal();
      grouping_projection_ids = {*grouping_a, *grouping_b, *grouping_id};
    }
    if (!RequireWord("FROM", "inline_values_source_required",
                     "native grouped aggregate profile requires an inline VALUES source")) {
      return FinishRefusal();
    }

    if (!RequireSymbol("(", "inline_values_open_required",
                       "inline VALUES source requires an opening parenthesis") ||
        !RequireWord("VALUES", "inline_values_source_required",
                     "native grouped aggregate profile requires VALUES")) {
      return FinishRefusal();
    }
    const Token& values_token = Previous();
    std::optional<std::size_t> row_arity;
    while (!AtEnd()) {
      const auto row_id = ParseValuesRow();
      if (!row_id.has_value()) return FinishRefusal();
      const auto& row = document_.values_rows[*row_id - 1];
      if (!row_arity.has_value()) {
        row_arity = row.expression_ids.size();
      } else if (*row_arity != row.expression_ids.size()) {
        Refuse("row_arity_mismatch",
               "VALUES rows must contain the same number of expressions");
        return FinishRefusal();
      }
      if (!AtSymbol(",")) break;
      Consume();
      if (!AtSymbol("(")) {
        Refuse("row_constructor_expected",
               "VALUES row separator must be followed by a row constructor");
        return FinishRefusal();
      }
    }
    const std::size_t expected_source_arity =
        one_key_grouping_profile ? 2 : 3;
    if (document_.values_rows.empty() ||
        row_arity != expected_source_arity) {
      Refuse("inline_values_arity_invalid",
             "native grouped aggregate source arity does not match its grouping profile");
      return FinishRefusal();
    }
    const Token& values_end = Previous();
    if (!RequireSymbol(")", "inline_values_not_closed",
                       "inline VALUES source is not closed") ||
        !RequireWord("AS", "inline_values_alias_required",
                     "inline VALUES source requires an explicit alias") ||
        AtEnd() || !IsNameToken(Current())) {
      if (!document_.messages.has_errors()) {
        Refuse("inline_values_alias_required",
               "inline VALUES source requires an explicit alias");
      }
      return FinishRefusal();
    }
    Consume();  // relation alias; binding resolves column UUIDs, not this spelling.
    if (!RequireSymbol("(", "inline_values_columns_required",
                       "inline VALUES alias requires three column names")) {
      return FinishRefusal();
    }
    std::vector<std::string> column_names;
    while (!AtEnd() && !AtSymbol(")")) {
      if (!IsNameToken(Current())) {
        Refuse("inline_values_column_invalid",
               "inline VALUES column alias must be an identifier");
        return FinishRefusal();
      }
      column_names.push_back(CanonicalTokenText(Consume()));
      if (AtSymbol(")")) break;
      if (!RequireSymbol(",", "inline_values_column_separator_required",
                         "inline VALUES column aliases must be comma separated")) {
        return FinishRefusal();
      }
    }
    const bool duplicate_column_name =
        std::unordered_set<std::string>(column_names.begin(),
                                        column_names.end()).size() !=
        column_names.size();
    if (!RequireSymbol(")", "inline_values_columns_not_closed",
                       "inline VALUES column alias list is not closed") ||
        column_names.size() != expected_source_arity ||
        duplicate_column_name) {
      if (!document_.messages.has_errors()) {
        Refuse("inline_values_columns_invalid",
               "inline VALUES source requires the exact unique column aliases for its grouping profile");
      }
      return FinishRefusal();
    }
    const auto& sum_argument = document_.expressions[sum_child - 1];
    if (ToUpperAscii(key_a_spelling) != column_names[0] ||
        (!one_key_grouping_profile &&
         ToUpperAscii(key_b_spelling) != column_names[1]) ||
        sum_argument.expression_kind != NativeExpressionAstKind::kIdentifier ||
        ToUpperAscii(sum_argument.spelling) !=
            column_names[expected_source_arity - 1]) {
      Refuse("inline_values_projection_mismatch",
             "grouped aggregate projection must bind the declared inline VALUES columns");
      return FinishRefusal();
    }

    if (!RequireWord("GROUP", "grouping_clause_required",
                     "native aggregate profile requires GROUP BY") ||
        !RequireWord("BY", "grouping_clause_required",
                     "native aggregate profile requires GROUP BY")) {
      return FinishRefusal();
    }

    NativeAggregateGroupingForm grouping_form =
        NativeAggregateGroupingForm::kNone;
    const Token* query_end = nullptr;
    if (one_key_grouping_profile) {
      // QOW-SOURCE-QRY-001-SIMPLE-GROUP-BY-V1
      grouping_form = NativeAggregateGroupingForm::kSimple;
      if (AtEnd() || !IsNameToken(Current()) ||
          CanonicalTokenText(Current()) != column_names[0]) {
        Refuse("simple_grouping_key_invalid",
               "ordinary GROUP BY must name the projected grouping key exactly");
        return FinishRefusal();
      }
      query_end = &Consume();
    } else if (!AtEnd() && IsNameToken(Current()) &&
               CanonicalTokenText(Current()) == column_names[0]) {
      // QOW-SOURCE-QRY-001-SIMPLE-TWO-KEY-GROUP-BY-V1
      if (projection_form != NativeAggregateProjectionForm::kKeysCountSum) {
        Refuse("simple_grouping_projection_invalid",
               "ordinary two-key GROUP BY does not admit grouping metadata "
               "projections");
        return FinishRefusal();
      }
      grouping_form = NativeAggregateGroupingForm::kSimple;
      Consume();
      if (!RequireSymbol(",", "simple_grouping_key_separator_required",
                         "ordinary two-key GROUP BY requires both projected keys") ||
          AtEnd() || !IsNameToken(Current()) ||
          CanonicalTokenText(Current()) != column_names[1]) {
        if (!document_.messages.has_errors()) {
          Refuse("simple_grouping_second_key_invalid",
                 "ordinary two-key GROUP BY must name the second projected key "
                 "exactly");
        }
        return FinishRefusal();
      }
      query_end = &Consume();
    } else if (!AtEnd() && IsWord(Current(), "GROUPING")) {
      grouping_form = NativeAggregateGroupingForm::kGroupingSets;
      Consume();
      if (!RequireWord("SETS", "grouping_sets_required",
                       "native aggregate profile requires GROUPING SETS") ||
          !RequireSymbol("(", "grouping_sets_open_required",
                         "GROUPING SETS requires an opening parenthesis")) {
        return FinishRefusal();
      }

      std::uint32_t ordinal = 0;
      while (!AtEnd() && !AtSymbol(")")) {
        if (!AtSymbol("(")) {
          Refuse("grouping_set_open_required",
                 "each GROUPING SETS item must be parenthesized");
          return FinishRefusal();
        }
        const Token& set_open = Consume();
        NativeGroupingSetAstNode grouping_set;
        grouping_set.relation_id = 2;
        grouping_set.ordinal = ordinal++;
        while (!AtEnd() && !AtSymbol(")")) {
          if (!IsNameToken(Current())) {
            Refuse("grouping_set_member_invalid",
                   "native grouping-set members must be projected key identifiers");
            return FinishRefusal();
          }
          const auto member = CanonicalTokenText(Consume());
          if (member == column_names[0]) {
            grouping_set.expression_ids.push_back(*key_a);
          } else if (member == column_names[1]) {
            grouping_set.expression_ids.push_back(*key_b);
          } else {
            Refuse("grouping_set_member_unbound",
                   "grouping-set member is not a projected grouping key");
            return FinishRefusal();
          }
          if (AtSymbol(")")) break;
          if (!RequireSymbol(",", "grouping_set_member_separator_required",
                             "grouping-set members must be comma separated")) {
            return FinishRefusal();
          }
        }
        if (!AtSymbol(")")) {
          Refuse("grouping_set_not_closed", "grouping set is not closed");
          return FinishRefusal();
        }
        const Token& set_close = Consume();
        grouping_set.range = Span(set_open, set_close);
        document_.grouping_sets.push_back(std::move(grouping_set));
        if (AtSymbol(")")) break;
        if (!RequireSymbol(",", "grouping_sets_separator_required",
                           "GROUPING SETS items must be comma separated")) {
          return FinishRefusal();
        }
        if (AtSymbol(")")) {
          Refuse("grouping_set_missing_after_separator",
                 "GROUPING SETS separator must be followed by a grouping set");
          return FinishRefusal();
        }
      }
      if (document_.grouping_sets.empty() || !AtSymbol(")")) {
        Refuse("grouping_sets_empty_or_unclosed",
               "GROUPING SETS requires at least one closed grouping set");
        return FinishRefusal();
      }
      query_end = &Consume();
    } else if (!AtEnd() && IsWord(Current(), "ROLLUP")) {
      // QOW-SOURCE-QRY-001-ROLLUP-V1
      grouping_form = NativeAggregateGroupingForm::kRollup;
      Consume();
      if (!RequireSymbol("(", "rollup_open_required",
                         "ROLLUP requires an opening parenthesis") ||
          AtEnd() || !IsNameToken(Current()) ||
          CanonicalTokenText(Current()) != column_names[0]) {
        if (!document_.messages.has_errors()) {
          Refuse("rollup_first_key_invalid",
                 "native ROLLUP profile requires the first projected grouping key");
        }
        return FinishRefusal();
      }
      Consume();
      if (!RequireSymbol(",", "rollup_key_separator_required",
                         "native ROLLUP profile requires two grouping keys") ||
          AtEnd() || !IsNameToken(Current()) ||
          CanonicalTokenText(Current()) != column_names[1]) {
        if (!document_.messages.has_errors()) {
          Refuse("rollup_second_key_invalid",
                 "native ROLLUP profile requires the second projected grouping key");
        }
        return FinishRefusal();
      }
      Consume();
      if (!AtSymbol(")")) {
        Refuse("rollup_close_required",
               "native ROLLUP profile requires exactly two grouping keys");
        return FinishRefusal();
      }
      query_end = &Consume();
    } else if (!AtEnd() && IsWord(Current(), "CUBE")) {
      // QOW-SOURCE-QRY-001-CUBE-V1
      grouping_form = NativeAggregateGroupingForm::kCube;
      Consume();
      if (!RequireSymbol("(", "cube_open_required",
                         "CUBE requires an opening parenthesis") ||
          AtEnd() || !IsNameToken(Current()) ||
          CanonicalTokenText(Current()) != column_names[0]) {
        if (!document_.messages.has_errors()) {
          Refuse("cube_first_key_invalid",
                 "native CUBE profile requires the first projected grouping key");
        }
        return FinishRefusal();
      }
      Consume();
      if (!RequireSymbol(",", "cube_key_separator_required",
                         "native CUBE profile requires two grouping keys") ||
          AtEnd() || !IsNameToken(Current()) ||
          CanonicalTokenText(Current()) != column_names[1]) {
        if (!document_.messages.has_errors()) {
          Refuse("cube_second_key_invalid",
                 "native CUBE profile requires the second projected grouping key");
        }
        return FinishRefusal();
      }
      Consume();
      if (!AtSymbol(")")) {
        Refuse("cube_close_required",
               "native CUBE profile requires exactly two grouping keys");
        return FinishRefusal();
      }
      query_end = &Consume();
    } else {
      Refuse("grouping_form_required",
             "native aggregate profile requires ordinary GROUP BY, GROUPING "
             "SETS, ROLLUP, or CUBE");
      return FinishRefusal();
    }

    std::optional<std::uint32_t> having_predicate;
    if (!AtEnd() && IsWord(Current(), "HAVING")) {
      // QOW-SOURCE-QRY-001-HAVING-SUM-GT-V1
      // QOW-SOURCE-QRY-001-HAVING-COUNT-SUM-AND-GT-V1
      // QOW-SOURCE-QRY-001-TWO-KEY-HAVING-COUNT-SUM-AND-GT-V1
      // QOW-SOURCE-QRY-001-TWO-KEY-HAVING-SUM-GT-V1
      // QOW-SOURCE-QRY-001-GROUPING-SETS-HAVING-COUNT-SUM-AND-GT-V1
      // QOW-SOURCE-QRY-001-GROUPING-SETS-HAVING-SUM-GT-V1
      // QOW-SOURCE-QRY-001-GROUPING-SETS-GROUPING-METADATA-HAVING-V1
      // QOW-SOURCE-QRY-001-ROLLUP-HAVING-COUNT-SUM-AND-GT-V1
      // QOW-SOURCE-QRY-001-ROLLUP-HAVING-SUM-GT-V1
      // QOW-SOURCE-QRY-001-ROLLUP-GROUPING-METADATA-HAVING-V1
      // QOW-SOURCE-QRY-001-CUBE-HAVING-COUNT-SUM-AND-GT-V1
      // QOW-SOURCE-QRY-001-CUBE-HAVING-SUM-GT-V1
      // QOW-SOURCE-QRY-001-CUBE-GROUPING-METADATA-HAVING-V1
      const bool ordinary_count_sum_profile =
          grouping_form == NativeAggregateGroupingForm::kSimple &&
          (projection_form == NativeAggregateProjectionForm::kKeyCountSum ||
           projection_form == NativeAggregateProjectionForm::kKeysCountSum);
      const bool grouping_sets_count_sum_profile =
          grouping_form == NativeAggregateGroupingForm::kGroupingSets &&
          (projection_form == NativeAggregateProjectionForm::kKeysCountSum ||
           projection_form ==
               NativeAggregateProjectionForm::kKeysCountSumGrouping);
      const bool rollup_count_sum_profile =
          grouping_form == NativeAggregateGroupingForm::kRollup &&
          (projection_form == NativeAggregateProjectionForm::kKeysCountSum ||
           projection_form ==
               NativeAggregateProjectionForm::kKeysCountSumGrouping);
      const bool cube_count_sum_profile =
          grouping_form == NativeAggregateGroupingForm::kCube &&
          (projection_form == NativeAggregateProjectionForm::kKeysCountSum ||
           projection_form ==
               NativeAggregateProjectionForm::kKeysCountSumGrouping);
      if (!ordinary_count_sum_profile &&
          !grouping_sets_count_sum_profile && !rollup_count_sum_profile &&
          !cube_count_sum_profile) {
        Refuse("having_profile_not_admitted",
               "native HAVING profile requires an admitted grouping form");
        return FinishRefusal();
      }
      Consume();
      allow_count_star_expression_ = true;
      having_predicate = ParseExpression(0, 0);
      allow_count_star_expression_ = false;
      if (!having_predicate.has_value()) return FinishRefusal();
      const auto& predicate = document_.expressions[*having_predicate - 1];
      const auto expression_by_id = [&](const std::uint32_t expression_id)
          -> const NativeExpressionAstNode* {
        if (expression_id == 0 ||
            expression_id > document_.expressions.size()) {
          return nullptr;
        }
        return &document_.expressions[expression_id - 1];
      };
      const auto match_numeric_threshold =
          [&](const std::uint32_t expression_id) {
            const auto* threshold = expression_by_id(expression_id);
            return threshold != nullptr &&
                   threshold->expression_kind ==
                       NativeExpressionAstKind::kLiteral &&
                   threshold->literal_kind == NativeLiteralAstKind::kNumeric;
          };
      const auto match_sum_comparison =
          [&](const NativeExpressionAstNode& comparison) {
            if (comparison.expression_kind !=
                    NativeExpressionAstKind::kBinary ||
                comparison.operator_name != ">" ||
                comparison.child_expression_ids.size() != 2 ||
                !match_numeric_threshold(
                    comparison.child_expression_ids[1])) {
              return false;
            }
            const auto* having_sum =
                expression_by_id(comparison.child_expression_ids[0]);
            if (having_sum == nullptr ||
                having_sum->expression_kind !=
                    NativeExpressionAstKind::kFunctionCall ||
                ToUpperAscii(having_sum->operator_name) != "SUM" ||
                having_sum->child_expression_ids.size() != 1) {
              return false;
            }
            const auto* argument =
                expression_by_id(having_sum->child_expression_ids[0]);
            return argument != nullptr &&
                   argument->expression_kind ==
                       NativeExpressionAstKind::kIdentifier &&
                   ToUpperAscii(argument->spelling) == column_names.back();
          };
      const auto match_count_comparison =
          [&](const NativeExpressionAstNode& comparison) {
            if (comparison.expression_kind !=
                    NativeExpressionAstKind::kBinary ||
                comparison.operator_name != ">" ||
                comparison.child_expression_ids.size() != 2 ||
                !match_numeric_threshold(
                    comparison.child_expression_ids[1])) {
              return false;
            }
            const auto* having_count =
                expression_by_id(comparison.child_expression_ids[0]);
            return having_count != nullptr &&
                   having_count->expression_kind ==
                       NativeExpressionAstKind::kFunctionCall &&
                   ToUpperAscii(having_count->operator_name) == "COUNT" &&
                   having_count->child_expression_ids.empty();
      };
      const bool simple_sum_profile = match_sum_comparison(predicate);
      const auto* count_conjunct =
          predicate.child_expression_ids.size() == 2
              ? expression_by_id(predicate.child_expression_ids[0])
              : nullptr;
      const auto* sum_conjunct =
          predicate.child_expression_ids.size() == 2
              ? expression_by_id(predicate.child_expression_ids[1])
              : nullptr;
      const bool count_sum_and_profile =
          predicate.expression_kind == NativeExpressionAstKind::kBinary &&
          predicate.operator_name == "AND" &&
          predicate.child_expression_ids.size() == 2 &&
          count_conjunct != nullptr && sum_conjunct != nullptr &&
          match_count_comparison(*count_conjunct) &&
          match_sum_comparison(*sum_conjunct);
      if (!simple_sum_profile && !count_sum_and_profile) {
        Refuse("having_predicate_shape_invalid",
               "native HAVING profile requires SUM(value) > numeric literal "
               "or COUNT(*) > numeric literal AND SUM(value) > numeric literal");
        return FinishRefusal();
      }
      const bool ordinary_two_key_sum_profile =
          !one_key_grouping_profile && simple_sum_profile &&
          grouping_form == NativeAggregateGroupingForm::kSimple &&
          projection_form == NativeAggregateProjectionForm::kKeysCountSum;
      const bool grouping_sets_sum_profile =
          !one_key_grouping_profile && simple_sum_profile &&
          grouping_form == NativeAggregateGroupingForm::kGroupingSets &&
          projection_form == NativeAggregateProjectionForm::kKeysCountSum;
      const bool rollup_sum_profile =
          !one_key_grouping_profile && simple_sum_profile &&
          grouping_form == NativeAggregateGroupingForm::kRollup &&
          projection_form == NativeAggregateProjectionForm::kKeysCountSum;
      const bool cube_sum_profile =
          !one_key_grouping_profile && simple_sum_profile &&
          grouping_form == NativeAggregateGroupingForm::kCube &&
          projection_form == NativeAggregateProjectionForm::kKeysCountSum;
      if (!one_key_grouping_profile && !count_sum_and_profile &&
          !ordinary_two_key_sum_profile && !grouping_sets_sum_profile &&
          !rollup_sum_profile && !cube_sum_profile) {
        Refuse("having_profile_not_admitted",
               "native multi-key HAVING profile requires an exact admitted "
               "SUM comparison or ordered COUNT/SUM AND predicate");
        return FinishRefusal();
      }
    }

    if (AtSymbol(";")) Consume();
    if (!AtEnd()) {
      Refuse("trailing_input",
             "unexpected input follows the native grouped aggregate query");
      return FinishRefusal();
    }

    NativeRelationAstNode values_relation;
    values_relation.relation_id = 1;
    values_relation.relation_kind = NativeRelationAstKind::kValues;
    values_relation.range = Span(values_token, values_end);
    for (const auto& row : document_.values_rows) {
      values_relation.values_row_ids.push_back(row.row_id);
    }
    values_relation.output_expression_ids =
        document_.values_rows.front().expression_ids;

    NativeRelationAstNode aggregate_relation;
    aggregate_relation.relation_id = 2;
    aggregate_relation.relation_kind = NativeRelationAstKind::kAggregate;
    aggregate_relation.aggregate_grouping_form = grouping_form;
    aggregate_relation.aggregate_projection_form = projection_form;
    aggregate_relation.input_relation_ids = {1};
    aggregate_relation.output_expression_ids = {*key_a};
    if (key_b.has_value()) {
      aggregate_relation.output_expression_ids.push_back(*key_b);
    }
    aggregate_relation.output_expression_ids.push_back(*count);
    aggregate_relation.output_expression_ids.push_back(*sum);
    aggregate_relation.output_expression_ids.insert(
        aggregate_relation.output_expression_ids.end(),
        grouping_projection_ids.begin(), grouping_projection_ids.end());
    aggregate_relation.grouping_key_expression_ids = {*key_a};
    if (key_b.has_value()) {
      aggregate_relation.grouping_key_expression_ids.push_back(*key_b);
    }
    aggregate_relation.aggregate_expression_ids = {*count, *sum};
    aggregate_relation.range = Span(select_token, *query_end);
    document_.relations.push_back(std::move(values_relation));
    document_.relations.push_back(std::move(aggregate_relation));
    if (having_predicate.has_value()) {
      NativeRelationAstNode filter_relation;
      filter_relation.relation_id = 3;
      filter_relation.relation_kind = NativeRelationAstKind::kFilter;
      filter_relation.input_relation_ids = {2};
      filter_relation.output_expression_ids =
          document_.relations.back().output_expression_ids;
      filter_relation.predicate_expression_ids = {*having_predicate};
      filter_relation.range =
          MergeRanges(document_.relations.back().range,
                      document_.expressions[*having_predicate - 1].range);
      document_.relations.push_back(std::move(filter_relation));
      document_.root_relation_id = 3;
    } else {
      document_.root_relation_id = 2;
    }
    document_.status = NativeRelationalParseStatus::kAccepted;
    return std::move(document_);
  }

  std::string SourceSpelling(const Token& first, const Token& last) const {
    const auto offset = first.offset;
    const auto length = last.offset + last.length - offset;
    if (offset > cst_.source.size() || length > cst_.source.size() - offset) {
      return {};
    }
    return cst_.source.substr(offset, length);
  }

  void Refuse(const std::string_view reason, const std::string_view message) {
    if (document_.messages.has_errors()) return;
    std::vector<Field> fields{{"reason", std::string(reason)}};
    if (!AtEnd()) {
      fields.push_back({"token", Current().text});
      fields.push_back({"offset", std::to_string(Current().offset)});
    }
    document_.messages.diagnostics.push_back(MakeDiagnostic(
        "QOW-DIAG-QRY-001-AST-MALFORMED", "ERROR", std::string(message),
        "sbp_sbsql.native_relational_parser", std::move(fields)));
  }

  NativeRelationalAstDocument FinishRefusal() {
    document_.status = NativeRelationalParseStatus::kRefused;
    document_.root_relation_id = 0;
    document_.relations.clear();
    document_.values_rows.clear();
    document_.grouping_sets.clear();
    document_.expressions.clear();
    return std::move(document_);
  }

  std::optional<std::uint32_t> ParseValuesRow() {
    if (!AtSymbol("(")) {
      Refuse("row_constructor_expected",
             "VALUES requires a parenthesized row constructor");
      return std::nullopt;
    }
    const Token& first = Consume();
    if (AtSymbol(")")) {
      Refuse("empty_row", "VALUES row constructor cannot be empty");
      return std::nullopt;
    }

    NativeValuesRowAstNode row;
    row.row_id = static_cast<std::uint32_t>(document_.values_rows.size() + 1);
    while (!AtEnd()) {
      const auto expression_id = ParseExpression(0, 0);
      if (!expression_id.has_value()) return std::nullopt;
      row.expression_ids.push_back(*expression_id);
      if (AtSymbol(")")) break;
      if (!AtSymbol(",")) {
        Refuse("row_separator_expected",
               "VALUES row expressions must be separated by commas");
        return std::nullopt;
      }
      Consume();
      if (AtEnd() || AtSymbol(")") || AtSymbol(",")) {
        Refuse("missing_expression_after_separator",
               "VALUES expression separator must be followed by an expression");
        return std::nullopt;
      }
    }

    if (AtEnd()) {
      Refuse("row_not_closed", "VALUES row constructor is not closed");
      return std::nullopt;
    }
    const Token& last = Consume();
    row.range = Span(first, last);
    document_.values_rows.push_back(std::move(row));
    return document_.values_rows.back().row_id;
  }

  std::optional<std::uint32_t> ParseExpression(const int minimum_precedence,
                                               const std::size_t depth) {
    if (depth >= kMaximumNativeExpressionDepth) {
      Refuse("expression_depth_exceeded", "native expression depth limit exceeded");
      return std::nullopt;
    }
    auto left_id = ParseUnary(depth + 1);
    if (!left_id.has_value()) return std::nullopt;

    while (!AtEnd()) {
      const auto binary_operator = BinaryOperatorFor(Current());
      if (!binary_operator.has_value() ||
          binary_operator->precedence < minimum_precedence) {
        break;
      }
      const Token& operator_token = Consume();
      const auto right_id = ParseExpression(binary_operator->precedence + 1, depth + 1);
      if (!right_id.has_value()) {
        Refuse("binary_operand_missing",
               "binary operator must be followed by an expression");
        return std::nullopt;
      }

      const auto& left = document_.expressions[*left_id - 1];
      const auto& right = document_.expressions[*right_id - 1];
      NativeExpressionAstNode binary;
      binary.expression_id = NextExpressionId();
      binary.expression_kind = NativeExpressionAstKind::kBinary;
      binary.child_expression_ids = {*left_id, *right_id};
      binary.operator_name = CanonicalTokenText(operator_token);
      binary.spelling = SourceSpelling(TokenForRangeStart(left.range),
                                       TokenForRangeEnd(right.range));
      binary.range = MergeRanges(left.range, right.range);
      document_.expressions.push_back(std::move(binary));
      left_id = document_.expressions.back().expression_id;
    }
    return left_id;
  }

  std::optional<std::uint32_t> ParseUnary(const std::size_t depth) {
    if (AtEnd()) {
      Refuse("expression_expected", "VALUES row requires an expression");
      return std::nullopt;
    }
    const auto word = CanonicalTokenText(Current());
    if (word != "+" && word != "-" && word != "NOT") {
      return ParsePrimary(depth);
    }

    const Token& first = Consume();
    const auto child_id = ParseUnary(depth + 1);
    if (!child_id.has_value()) return std::nullopt;
    const auto& child = document_.expressions[*child_id - 1];
    NativeExpressionAstNode unary;
    unary.expression_id = NextExpressionId();
    unary.expression_kind = NativeExpressionAstKind::kUnary;
    unary.child_expression_ids = {*child_id};
    unary.operator_name = word;
    unary.range = RangeFromTokenAndRange(first, child.range);
    unary.spelling = SourceForRange(unary.range);
    document_.expressions.push_back(std::move(unary));
    return document_.expressions.back().expression_id;
  }

  std::optional<std::uint32_t> ParsePrimary(const std::size_t depth) {
    if (AtEnd()) {
      Refuse("expression_expected", "VALUES row requires an expression");
      return std::nullopt;
    }

    const Token& first = Current();
    if (const auto literal_kind = LiteralKindFor(first.kind); literal_kind.has_value()) {
      Consume();
      NativeExpressionAstNode literal;
      literal.expression_id = NextExpressionId();
      literal.expression_kind = NativeExpressionAstKind::kLiteral;
      literal.literal_kind = *literal_kind;
      literal.spelling = first.text;
      literal.range = TokenSourceRange(first);
      document_.expressions.push_back(std::move(literal));
      return document_.expressions.back().expression_id;
    }

    if (first.kind == TokenKind::kParameter) {
      Consume();
      NativeExpressionAstNode parameter;
      parameter.expression_id = NextExpressionId();
      parameter.expression_kind = NativeExpressionAstKind::kParameter;
      parameter.spelling = first.text;
      parameter.range = TokenSourceRange(first);
      document_.expressions.push_back(std::move(parameter));
      return document_.expressions.back().expression_id;
    }

    if (AtSymbol("(")) {
      const Token& open = Consume();
      const auto child_id = ParseExpression(0, depth + 1);
      if (!child_id.has_value()) return std::nullopt;
      if (!AtSymbol(")")) {
        Refuse("expression_not_closed", "parenthesized expression is not closed");
        return std::nullopt;
      }
      const Token& close = Consume();
      NativeExpressionAstNode parenthesized;
      parenthesized.expression_id = NextExpressionId();
      parenthesized.expression_kind = NativeExpressionAstKind::kParenthesized;
      parenthesized.child_expression_ids = {*child_id};
      parenthesized.range = Span(open, close);
      parenthesized.spelling = SourceSpelling(open, close);
      document_.expressions.push_back(std::move(parenthesized));
      return document_.expressions.back().expression_id;
    }

    if (first.kind != TokenKind::kIdentifier && first.kind != TokenKind::kKeyword) {
      Refuse("expression_primary_expected", "unsupported VALUES expression primary");
      return std::nullopt;
    }

    Consume();
    const Token* last_name_token = &first;
    while (AtSymbol(".")) {
      Consume();
      if (AtEnd() || (Current().kind != TokenKind::kIdentifier &&
                      Current().kind != TokenKind::kKeyword)) {
        Refuse("qualified_name_incomplete", "qualified expression name is incomplete");
        return std::nullopt;
      }
      last_name_token = &Consume();
    }

    if (!AtSymbol("(")) {
      NativeExpressionAstNode identifier;
      identifier.expression_id = NextExpressionId();
      identifier.expression_kind = NativeExpressionAstKind::kIdentifier;
      identifier.range = Span(first, *last_name_token);
      identifier.spelling = SourceSpelling(first, *last_name_token);
      document_.expressions.push_back(std::move(identifier));
      return document_.expressions.back().expression_id;
    }

    Consume();
    if (allow_count_star_expression_ &&
        ToUpperAscii(SourceSpelling(first, *last_name_token)) == "COUNT" &&
        AtSymbol("*")) {
      Consume();
      if (!AtSymbol(")")) {
        Refuse("count_star_close_required",
               "COUNT(*) requires a closing parenthesis");
        return std::nullopt;
      }
      const Token& close = Consume();
      NativeExpressionAstNode function_call;
      function_call.expression_id = NextExpressionId();
      function_call.expression_kind = NativeExpressionAstKind::kFunctionCall;
      function_call.operator_name = SourceSpelling(first, *last_name_token);
      function_call.range = Span(first, close);
      function_call.spelling = SourceSpelling(first, close);
      document_.expressions.push_back(std::move(function_call));
      return document_.expressions.back().expression_id;
    }
    std::vector<std::uint32_t> argument_ids;
    if (!AtSymbol(")")) {
      while (!AtEnd()) {
        const auto argument_id = ParseExpression(0, depth + 1);
        if (!argument_id.has_value()) return std::nullopt;
        argument_ids.push_back(*argument_id);
        if (AtSymbol(")")) break;
        if (!AtSymbol(",")) {
          Refuse("function_argument_separator_expected",
                 "function arguments must be separated by commas");
          return std::nullopt;
        }
        Consume();
      }
    }
    if (!AtSymbol(")")) {
      Refuse("function_call_not_closed", "function call is not closed");
      return std::nullopt;
    }
    const Token& close = Consume();
    NativeExpressionAstNode function_call;
    function_call.expression_id = NextExpressionId();
    function_call.expression_kind = NativeExpressionAstKind::kFunctionCall;
    function_call.child_expression_ids = std::move(argument_ids);
    function_call.operator_name = SourceSpelling(first, *last_name_token);
    function_call.range = Span(first, close);
    function_call.spelling = SourceSpelling(first, close);
    document_.expressions.push_back(std::move(function_call));
    return document_.expressions.back().expression_id;
  }

  std::uint32_t NextExpressionId() const {
    const auto next = document_.expressions.size() + 1;
    if (next > std::numeric_limits<std::uint32_t>::max()) return 0;
    return static_cast<std::uint32_t>(next);
  }

  SourceRange MergeRanges(const SourceRange& first, const SourceRange& last) const {
    SourceRange range = first;
    range.length = last.offset + last.length - first.offset;
    range.end_line = last.end_line;
    range.end_column = last.end_column;
    return range;
  }

  SourceRange RangeFromTokenAndRange(const Token& first, const SourceRange& last) const {
    SourceRange range = TokenSourceRange(first);
    range.length = last.offset + last.length - first.offset;
    range.end_line = last.end_line;
    range.end_column = last.end_column;
    return range;
  }

  std::string SourceForRange(const SourceRange& range) const {
    if (range.offset > cst_.source.size() ||
        range.length > cst_.source.size() - range.offset) {
      return {};
    }
    return cst_.source.substr(range.offset, range.length);
  }

  const Token& TokenForRangeStart(const SourceRange& range) const {
    for (const auto* token : tokens_) {
      if (token->offset == range.offset) return *token;
    }
    return *tokens_.front();
  }

  const Token& TokenForRangeEnd(const SourceRange& range) const {
    const auto end = range.offset + range.length;
    for (auto iterator = tokens_.rbegin(); iterator != tokens_.rend(); ++iterator) {
      if ((*iterator)->offset + (*iterator)->length == end) return **iterator;
    }
    return *tokens_.back();
  }

  const CstDocument& cst_;
  std::vector<const Token*> tokens_;
  std::size_t cursor_{0};
  bool allow_count_star_expression_{false};
  NativeRelationalAstDocument document_;
};

} // namespace

// QOW-SOURCE-QRY-001-AST-V1
NativeRelationalAstDocument ParseNativeRelationalAst(const CstDocument& cst) {
  return NativeRelationalParser(cst).Parse();
}

} // namespace scratchbird::parser::sbsql

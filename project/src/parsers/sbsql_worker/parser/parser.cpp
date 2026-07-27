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
  NativeRelationalAstDocument document_;
};

} // namespace

// QOW-SOURCE-QRY-001-AST-V1
NativeRelationalAstDocument ParseNativeRelationalAst(const CstDocument& cst) {
  return NativeRelationalParser(cst).Parse();
}

} // namespace scratchbird::parser::sbsql

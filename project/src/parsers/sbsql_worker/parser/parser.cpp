// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "ast/ast.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdint>
#include <functional>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
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

std::string ToLowerAscii(std::string value) {
  for (auto& ch : value) {
    if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch - 'A' + 'a');
  }
  return value;
}

bool SameIdentifier(const NativeIdentifierAstNode& expected,
                    const Token& presented) {
  if (expected.quoted != presented.quoted) return false;
  return expected.quoted
             ? expected.spelling == presented.text
             : ToLowerAscii(expected.spelling) == ToLowerAscii(presented.text);
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
    const auto contains_word = [&](const std::string_view wanted) {
      return std::ranges::any_of(tokens_, [&](const auto* token) {
        return !token->quoted && IsWord(*token, wanted);
      });
    };
    if (contains_word("MONGO_PIPELINE") || contains_word("CYPHER_TEXT") ||
        contains_word("REDIS_COMMAND") ||
        contains_word("INFLUX_LINE_PROTOCOL")) {
      RefuseExact("SB_MODEL_GRAMMAR_DONOR_TEXT_REFUSED_V1",
                  "opaque donor text is not an executable SBSQL model source");
      return FinishRefusal();
    }
    if (contains_word("TIME_SERIES_SOURCE") || contains_word("TIME_RANGE") ||
        contains_word("TIME_BUCKET") || contains_word("TIME_DOWNSAMPLE") ||
        contains_word("TIME_SERIES_APPEND")) {
      if (tokens_.empty() || !IsWord(*tokens_.front(), "SELECT")) {
        RefuseExact("SB_MODEL_TIME_SERIES_APPEND_NOT_QUERY_SOURCE_V1",
                    "time-series append/write identity is not a query source");
        return FinishRefusal();
      }
      if (contains_word("TIME_SERIES_APPEND")) {
        RefuseExact("SB_MODEL_TIME_SERIES_APPEND_NOT_QUERY_SOURCE_V1",
                    "time-series append opcode is not a query source");
        return FinishRefusal();
      }
      if (contains_word("TIME_DOWNSAMPLE") &&
          (contains_word("DISTINCT") || contains_word("FILTER"))) {
        RefuseExact("SB_MODEL_TIME_SERIES_AGGREGATE_REFUSED_V1",
                    "time-series downsample does not admit DISTINCT or FILTER");
        return FinishRefusal();
      }
      return ParseTimeSeriesModelSelect();
    }
    if (contains_word("GRAPH_SOURCE") || contains_word("GRAPH_MATCH") ||
        contains_word("GRAPH_EXPAND")) {
      if (tokens_.empty() || !IsWord(*tokens_.front(), "SELECT")) {
        RefuseExact("SB_MODEL_QUERY_WRITE_REFUSED_V1",
                    "graph model sources are read-only query inputs");
        return FinishRefusal();
      }
      return ParseGraphModelSelect();
    }
    if (contains_word("KEY_VALUE_SOURCE") || contains_word("KV_KEY") ||
        contains_word("KV_MULTI_GET") || contains_word("KV_PREFIX")) {
      if (tokens_.empty() || !IsWord(*tokens_.front(), "SELECT")) {
        RefuseExact("SB_MODEL_QUERY_WRITE_REFUSED_V1",
                    "key/value model sources are read-only query inputs");
        return FinishRefusal();
      }
      return ParseKeyValueModelSelect();
    }
    if (contains_word("DOCUMENT_SOURCE") ||
        contains_word("DOCUMENT_UNNEST") ||
        contains_word("DOCUMENT_PATH")) {
      if (tokens_.empty() || !IsWord(*tokens_.front(), "SELECT")) {
        RefuseExact("SB_MODEL_QUERY_WRITE_REFUSED_V1",
                    "document model sources are read-only query inputs");
        return FinishRefusal();
      }
      return ParseDocumentModelSelect();
    }
    if (!tokens_.empty() && IsWord(*tokens_.front(), "SELECT") &&
        LooksLikeSupportedGroupingQuery()) {
      return ParseGroupedAggregateSelect();
    }
    if (!tokens_.empty() && IsWord(*tokens_.front(), "SELECT") &&
        LooksLikeBoundedWindowSelect()) {
      return ParseWindowSelect();
    }
    if (!tokens_.empty() && IsWord(*tokens_.front(), "SELECT") &&
        LooksLikeBoundedCatalogJoinSelect()) {
      return ParseCatalogJoinSelect();
    }
    if (!tokens_.empty() && IsWord(*tokens_.front(), "SELECT") &&
        LooksLikeBoundedCatalogRelationSelect()) {
      return ParseCatalogRelationSelect();
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
  void RefuseExact(const std::string_view diagnostic_id,
                   const std::string_view message) {
    if (document_.messages.has_errors()) return;
    document_.messages.diagnostics.push_back(MakeDiagnostic(
        std::string(diagnostic_id), "ERROR", std::string(message),
        "sbp_sbsql.document_model_parser"));
  }

  NativeRelationalAstDocument ParseTimeSeriesModelSelect() {
    // QOW-SOURCE-RCP-076-TIME-SERIES-GRAMMAR-V1
    document_.status = NativeRelationalParseStatus::kRefused;
    if (cst_.messages.has_errors()) {
      document_.messages = cst_.messages;
      return FinishRefusal();
    }
    if (tokens_.size() > kMaximumNativeRelationalTokens) {
      Refuse("token_limit_exceeded", "time-series query token limit exceeded");
      return FinishRefusal();
    }

    const Token& select = Consume();
    std::vector<std::uint32_t> projection_expression_ids;
    if (AtSymbol("*")) {
      const Token& token = Consume();
      NativeExpressionAstNode wildcard;
      wildcard.expression_id = NextExpressionId();
      wildcard.expression_kind = NativeExpressionAstKind::kWildcard;
      wildcard.spelling = token.text;
      wildcard.range = TokenSourceRange(token);
      projection_expression_ids.push_back(wildcard.expression_id);
      document_.expressions.push_back(std::move(wildcard));
    } else {
      while (!AtEnd()) {
        const auto expression_id = ParseExpression(0, 0);
        if (!expression_id.has_value()) return FinishRefusal();
        projection_expression_ids.push_back(*expression_id);
        if (!AtSymbol(",")) break;
        Consume();
      }
    }
    if (!RequireWord("FROM", "time_series_from_required",
                     "time-series source requires FROM") ||
        AtEnd() || !IsWord(Current(), "TIME_SERIES_SOURCE")) {
      RefuseExact("SB_MODEL_OPERATION_SEMANTIC_REFUSED_V1",
                  "FROM requires exactly one TIME_SERIES_SOURCE");
      return FinishRefusal();
    }

    const Token& source_operator = Consume();
    if (!RequireSymbol("(", "time_series_source_open_required",
                       "TIME_SERIES_SOURCE requires an opening parenthesis") ||
        AtEnd() || !IsNameToken(Current())) {
      RefuseExact("SB_MODEL_BINDING_INCOMPLETE_V1",
                  "TIME_SERIES_SOURCE requires a qualified object name");
      return FinishRefusal();
    }
    NativeCatalogRelationSourceAstNode source;
    source.source_id = 1;
    source.source_kind = NativeRelationSourceAstKind::kTimeSeries;
    source.model_family_id = "time_series";
    const Token& first_name = Consume();
    const Token* last_name = &first_name;
    source.qualified_name.push_back(
        {first_name.text, first_name.quoted, TokenSourceRange(first_name)});
    while (AtSymbol(".")) {
      Consume();
      if (AtEnd() || !IsNameToken(Current())) {
        RefuseExact("SB_MODEL_BINDING_INCOMPLETE_V1",
                    "qualified time-series object name is incomplete");
        return FinishRefusal();
      }
      last_name = &Consume();
      source.qualified_name.push_back(
          {last_name->text, last_name->quoted, TokenSourceRange(*last_name)});
    }
    source.qualified_name_range = Span(first_name, *last_name);
    if (!RequireSymbol(")", "time_series_source_close_required",
                       "TIME_SERIES_SOURCE requires a closing parenthesis")) {
      return FinishRefusal();
    }
    const Token* source_end = &Previous();
    if (!AtEnd() && IsWord(Current(), "AS")) {
      Consume();
      if (AtEnd() || !IsNameToken(Current())) {
        RefuseExact("SB_MODEL_BINDING_INCOMPLETE_V1",
                    "TIME_SERIES_SOURCE AS requires an alias");
        return FinishRefusal();
      }
      const Token& alias = Consume();
      source.alias = NativeIdentifierAstNode{
          alias.text, alias.quoted, TokenSourceRange(alias)};
      source.alias_is_explicit = true;
      source_end = &alias;
    } else if (!AtEnd() && Current().kind == TokenKind::kIdentifier &&
               !IsWord(Current(), "WHERE")) {
      const Token& alias = Consume();
      source.alias = NativeIdentifierAstNode{
          alias.text, alias.quoted, TokenSourceRange(alias)};
      source_end = &alias;
    } else {
      source.alias = NativeIdentifierAstNode{
          last_name->text, last_name->quoted, TokenSourceRange(*last_name)};
    }

    if (AtEnd() || !IsWord(Current(), "WHERE")) {
      RefuseExact("SB_MODEL_TIME_SERIES_RANGE_INVALID_V1",
                  "TIME_SERIES_SOURCE requires exactly one TIME_RANGE");
      return FinishRefusal();
    }
    Consume();
    const auto range_expression_id = ParseExpression(0, 0);
    if (!range_expression_id.has_value()) return FinishRefusal();
    auto* range_expression = &document_.expressions[*range_expression_id - 1];
    if (range_expression->expression_kind !=
            NativeExpressionAstKind::kFunctionCall ||
        ToUpperAscii(range_expression->operator_name) != "TIME_RANGE" ||
        range_expression->child_expression_ids.size() != 3) {
      RefuseExact("SB_MODEL_TIME_SERIES_RANGE_INVALID_V1",
                  "TIME_RANGE requires alias, start, and end in exact order");
      return FinishRefusal();
    }
    range_expression->operator_name = "TIME_RANGE";
    const auto expression_at = [&](const std::uint32_t id)
        -> NativeExpressionAstNode* {
      return id == 0 || id > document_.expressions.size()
                 ? nullptr
                 : &document_.expressions[id - 1];
    };
    auto* alias_expression =
        expression_at(range_expression->child_expression_ids[0]);
    auto* start_expression =
        expression_at(range_expression->child_expression_ids[1]);
    auto* end_expression =
        expression_at(range_expression->child_expression_ids[2]);
    if (alias_expression == nullptr || start_expression == nullptr ||
        end_expression == nullptr || !source.alias.has_value() ||
        alias_expression->expression_kind !=
            NativeExpressionAstKind::kIdentifier ||
        alias_expression->qualified_identifier.size() != 1 ||
        !SameIdentifier(*source.alias,
                        TokenForRangeStart(alias_expression->range)) ||
        start_expression->expression_kind != NativeExpressionAstKind::kLiteral ||
        start_expression->literal_kind != NativeLiteralAstKind::kTemporal ||
        end_expression->expression_kind != NativeExpressionAstKind::kLiteral ||
        end_expression->literal_kind != NativeLiteralAstKind::kTemporal) {
      RefuseExact("SB_MODEL_TIME_SERIES_RANGE_INVALID_V1",
                  "TIME_RANGE alias or typed endpoints are invalid");
      return FinishRefusal();
    }
    source.model_time_series_alias_expression_id =
        alias_expression->expression_id;
    source.model_range_expression_id = range_expression->expression_id;
    source.model_range_start_expression_id = start_expression->expression_id;
    source.model_range_end_expression_id = end_expression->expression_id;
    if (!AtEnd() && (IsWord(Current(), "AND") ||
                     IsWord(Current(), "OR"))) {
      RefuseExact("SB_MODEL_TIME_SERIES_RANGE_INVALID_V1",
                  "TIME_SERIES_SOURCE admits exactly one TIME_RANGE predicate");
      return FinishRefusal();
    }

    NativeExpressionAstNode* bucket = nullptr;
    NativeExpressionAstNode* downsample = nullptr;
    for (const auto projection_id : projection_expression_ids) {
      auto* expression = expression_at(projection_id);
      if (expression == nullptr ||
          expression->expression_kind != NativeExpressionAstKind::kFunctionCall) {
        continue;
      }
      const auto operation = ToUpperAscii(expression->operator_name);
      if (operation == "TIME_BUCKET") {
        if (bucket != nullptr || expression->child_expression_ids.size() != 2) {
          RefuseExact("SB_MODEL_TIME_SERIES_INTERVAL_INVALID_V1",
                      "TIME_BUCKET requires one interval and timestamp expression");
          return FinishRefusal();
        }
        expression->operator_name = "TIME_BUCKET";
        bucket = expression;
      } else if (operation == "TIME_DOWNSAMPLE") {
        if (downsample != nullptr || expression->child_expression_ids.size() != 3) {
          RefuseExact("SB_MODEL_TIME_SERIES_AGGREGATE_REFUSED_V1",
                      "TIME_DOWNSAMPLE requires aggregate, interval, and value");
          return FinishRefusal();
        }
        expression->operator_name = "TIME_DOWNSAMPLE";
        downsample = expression;
      }
    }
    if (bucket != nullptr) {
      auto* interval = expression_at(bucket->child_expression_ids[0]);
      auto* timestamp = expression_at(bucket->child_expression_ids[1]);
      if (interval == nullptr || timestamp == nullptr ||
          interval->expression_kind != NativeExpressionAstKind::kLiteral ||
          interval->literal_kind != NativeLiteralAstKind::kTemporal ||
          timestamp->expression_kind != NativeExpressionAstKind::kIdentifier) {
        RefuseExact("SB_MODEL_TIME_SERIES_INTERVAL_INVALID_V1",
                    "TIME_BUCKET operands are not typed interval/timestamp expressions");
        return FinishRefusal();
      }
      source.model_bucket_expression_id = bucket->expression_id;
      source.model_bucket_interval_expression_id = interval->expression_id;
      source.model_bucket_time_input_expression_id = timestamp->expression_id;
      source.model_interval_expression_id = interval->expression_id;
      source.model_time_input_expression_id = timestamp->expression_id;
    }
    if (downsample != nullptr) {
      auto* aggregate = expression_at(downsample->child_expression_ids[0]);
      auto* interval = expression_at(downsample->child_expression_ids[1]);
      auto* value = expression_at(downsample->child_expression_ids[2]);
      const auto aggregate_id = aggregate == nullptr
                                    ? std::string{}
                                    : aggregate->spelling;
      const bool exact_value_input =
          value != nullptr && source.alias.has_value() &&
          value->qualified_identifier.size() == 2 &&
          value->qualified_identifier[0].quoted == source.alias->quoted &&
          (source.alias->quoted
               ? value->qualified_identifier[0].spelling ==
                     source.alias->spelling
               : ToLowerAscii(value->qualified_identifier[0].spelling) ==
                     ToLowerAscii(source.alias->spelling)) &&
          (value->qualified_identifier[1].quoted
               ? value->qualified_identifier[1].spelling == "value"
               : ToLowerAscii(value->qualified_identifier[1].spelling) ==
                     "value");
      if (aggregate == nullptr || interval == nullptr || value == nullptr ||
          aggregate->expression_kind != NativeExpressionAstKind::kIdentifier ||
          aggregate->qualified_identifier.size() != 1 ||
          (aggregate_id != "COUNT" && aggregate_id != "SUM" &&
           aggregate_id != "MIN" && aggregate_id != "MAX" &&
           aggregate_id != "AVG") ||
          interval->expression_kind != NativeExpressionAstKind::kLiteral ||
          interval->literal_kind != NativeLiteralAstKind::kTemporal ||
          value->expression_kind != NativeExpressionAstKind::kIdentifier ||
          !exact_value_input) {
        RefuseExact("SB_MODEL_TIME_SERIES_AGGREGATE_REFUSED_V1",
                    "TIME_DOWNSAMPLE aggregate or typed operands are invalid");
        return FinishRefusal();
      }
      source.model_downsample_expression_id = downsample->expression_id;
      source.model_interval_expression_id = interval->expression_id;
      source.model_time_input_expression_id = value->expression_id;
      source.model_time_series_aggregate_id = aggregate_id;
      // The closed aggregate ID is syntax, not a catalog identifier. Carry it
      // as an exact typed literal so no name-resolution UUID can become
      // aggregate execution authority.
      aggregate->expression_kind = NativeExpressionAstKind::kLiteral;
      aggregate->literal_kind = NativeLiteralAstKind::kString;
      aggregate->qualified_identifier.clear();
      aggregate->spelling = aggregate_id;
    }
    source.model_operation_id = downsample != nullptr
                                    ? "TIME_SERIES_DOWNSAMPLE"
                                    : "TIME_SERIES_RANGE_READ";

    if (AtSymbol(";")) Consume();
    if (!AtEnd()) {
      RefuseExact("SB_MODEL_OPERATION_SEMANTIC_REFUSED_V1",
                  "unexpected input follows the bounded time-series query");
      return FinishRefusal();
    }
    NativeRelationAstNode relation;
    relation.relation_id = 1;
    relation.relation_kind = NativeRelationAstKind::kCatalogSource;
    relation.relation_source_ids = {source.source_id};
    relation.output_expression_ids = projection_expression_ids;
    relation.predicate_expression_ids = {range_expression->expression_id};
    relation.range = Span(source_operator, *source_end);
    source.range = relation.range;
    document_.catalog_relation_sources.push_back(std::move(source));
    document_.relations.push_back(std::move(relation));
    document_.root_relation_id = 1;
    document_.status = NativeRelationalParseStatus::kAccepted;
    (void)select;
    return std::move(document_);
  }

  NativeRelationalAstDocument ParseKeyValueModelSelect() {
    // QOW-SOURCE-RCP-075-KEY-VALUE-GRAMMAR-V1
    document_.status = NativeRelationalParseStatus::kRefused;
    if (cst_.messages.has_errors()) {
      document_.messages = cst_.messages;
      return FinishRefusal();
    }
    if (tokens_.size() > kMaximumNativeRelationalTokens) {
      Refuse("token_limit_exceeded",
             "key/value model query token limit exceeded");
      return FinishRefusal();
    }

    const Token& select = Consume();
    std::vector<std::uint32_t> projection_expression_ids;
    if (AtSymbol("*")) {
      const Token& wildcard_token = Consume();
      NativeExpressionAstNode wildcard;
      wildcard.expression_id = NextExpressionId();
      wildcard.expression_kind = NativeExpressionAstKind::kWildcard;
      wildcard.spelling = wildcard_token.text;
      wildcard.range = TokenSourceRange(wildcard_token);
      projection_expression_ids.push_back(wildcard.expression_id);
      document_.expressions.push_back(std::move(wildcard));
    } else {
      while (!AtEnd()) {
        const auto expression_id = ParseExpression(0, 0);
        if (!expression_id.has_value()) return FinishRefusal();
        projection_expression_ids.push_back(*expression_id);
        if (!AtSymbol(",")) break;
        Consume();
      }
    }
    if (!RequireWord("FROM", "key_value_from_required",
                     "key/value model source requires FROM") ||
        AtEnd() || !IsWord(Current(), "KEY_VALUE_SOURCE")) {
      Refuse("key_value_source_required", "FROM requires KEY_VALUE_SOURCE");
      return FinishRefusal();
    }

    const Token& source_operator = Consume();
    if (!RequireSymbol("(", "key_value_source_open_required",
                       "KEY_VALUE_SOURCE requires an opening parenthesis") ||
        AtEnd() || !IsNameToken(Current())) {
      Refuse("key_value_qualified_name_required",
             "KEY_VALUE_SOURCE requires a qualified collection name");
      return FinishRefusal();
    }
    NativeCatalogRelationSourceAstNode source;
    source.source_id = 1;
    source.source_kind = NativeRelationSourceAstKind::kKeyValue;
    source.model_family_id = "key_value";
    const Token& first_name = Consume();
    const Token* last_name = &first_name;
    source.qualified_name.push_back(
        {first_name.text, first_name.quoted, TokenSourceRange(first_name)});
    while (AtSymbol(".")) {
      Consume();
      if (AtEnd() || !IsNameToken(Current())) {
        Refuse("key_value_qualified_name_incomplete",
               "qualified key/value collection name is incomplete");
        return FinishRefusal();
      }
      last_name = &Consume();
      source.qualified_name.push_back(
          {last_name->text, last_name->quoted, TokenSourceRange(*last_name)});
    }
    source.qualified_name_range = Span(first_name, *last_name);
    if (!RequireSymbol(")", "key_value_source_close_required",
                       "KEY_VALUE_SOURCE requires a closing parenthesis")) {
      return FinishRefusal();
    }
    const Token* source_end = &Previous();
    if (!AtEnd() && IsWord(Current(), "AS")) {
      Consume();
      if (AtEnd() || !IsNameToken(Current())) {
        Refuse("key_value_alias_invalid",
               "KEY_VALUE_SOURCE AS requires an identifier");
        return FinishRefusal();
      }
      const Token& alias = Consume();
      source.alias = NativeIdentifierAstNode{
          alias.text, alias.quoted, TokenSourceRange(alias)};
      source.alias_is_explicit = true;
      source_end = &alias;
    } else if (!AtEnd() && Current().kind == TokenKind::kIdentifier &&
               !IsWord(Current(), "WHERE")) {
      const Token& alias = Consume();
      source.alias = NativeIdentifierAstNode{
          alias.text, alias.quoted, TokenSourceRange(alias)};
      source_end = &alias;
    } else {
      source.alias = NativeIdentifierAstNode{
          last_name->text, last_name->quoted, TokenSourceRange(*last_name)};
    }

    NativeRelationAstNode relation;
    relation.relation_id = 1;
    relation.relation_kind = NativeRelationAstKind::kCatalogSource;
    relation.relation_source_ids = {source.source_id};
    relation.output_expression_ids = projection_expression_ids;

    if (AtEnd() || !IsWord(Current(), "WHERE")) {
      RefuseExact("SB_MODEL_OPERATION_SEMANTIC_REFUSED_V1",
                  "KEY_VALUE_SOURCE requires one exact get, multi-get, or prefix operation");
      return FinishRefusal();
    }
    Consume();
    if (AtEnd() || (!IsWord(Current(), "KV_KEY") &&
                    !IsWord(Current(), "KV_MULTI_GET") &&
                    !IsWord(Current(), "KV_PREFIX"))) {
      RefuseExact("SB_MODEL_OPERATION_SEMANTIC_REFUSED_V1",
                  "key/value WHERE operation is outside the closed profile");
      return FinishRefusal();
    }
    const Token& operation = Consume();
    const bool exact_get = IsWord(operation, "KV_KEY");
    const bool multi_get = IsWord(operation, "KV_MULTI_GET");
    source.model_operation_id = exact_get
                                    ? "KEY_VALUE_GET"
                                    : (multi_get ? "KEY_VALUE_MULTI_GET"
                                                 : "KEY_VALUE_PREFIX_RANGE");
    if (!RequireSymbol("(", "key_value_operation_open_required",
                       "key/value operation requires an opening parenthesis") ||
        AtEnd() || !IsNameToken(Current())) {
      return FinishRefusal();
    }
    const Token& operation_alias = Consume();
    if (!source.alias.has_value() ||
        !SameIdentifier(*source.alias, operation_alias)) {
      Refuse("key_value_alias_mismatch",
             "key/value operation alias does not name KEY_VALUE_SOURCE");
      return FinishRefusal();
    }
    NativeExpressionAstNode alias_expression;
    alias_expression.expression_id = NextExpressionId();
    alias_expression.expression_kind = NativeExpressionAstKind::kIdentifier;
    alias_expression.qualified_identifier.push_back(
        {operation_alias.text, operation_alias.quoted,
         TokenSourceRange(operation_alias)});
    alias_expression.spelling = operation_alias.text;
    alias_expression.range = TokenSourceRange(operation_alias);
    document_.expressions.push_back(std::move(alias_expression));
    const auto alias_expression_id = document_.expressions.back().expression_id;

    std::vector<std::uint32_t> key_expression_ids;
    if (exact_get) {
      if (!RequireSymbol(")", "key_value_operation_close_required",
                         "KV_KEY accepts only its bound source alias")) {
        return FinishRefusal();
      }
    } else {
      if (!AtSymbol(",")) {
        RefuseExact(multi_get
                        ? "SB_MODEL_KEY_VALUE_MULTI_GET_EMPTY_REFUSED_V1"
                        : "SB_MODEL_OPERATION_SEMANTIC_REFUSED_V1",
                    multi_get
                        ? "KV_MULTI_GET requires at least one key expression"
                        : "KV_PREFIX requires one prefix expression");
        return FinishRefusal();
      }
      Consume();
      while (!AtEnd() && !AtSymbol(")")) {
        const auto key_expression_id = ParseExpression(0, 0);
        if (!key_expression_id.has_value()) return FinishRefusal();
        key_expression_ids.push_back(*key_expression_id);
        if (!AtSymbol(",")) break;
        if (!multi_get) {
          RefuseExact("SB_MODEL_OPERATION_SEMANTIC_REFUSED_V1",
                      "prefix accepts exactly one expression");
          return FinishRefusal();
        }
        Consume();
        if (AtEnd() || AtSymbol(")")) {
          RefuseExact("SB_MODEL_KEY_VALUE_MULTI_GET_EMPTY_REFUSED_V1",
                      "KV_MULTI_GET key separator requires another expression");
          return FinishRefusal();
        }
      }
      if (key_expression_ids.empty()) {
        RefuseExact(multi_get
                        ? "SB_MODEL_KEY_VALUE_MULTI_GET_EMPTY_REFUSED_V1"
                        : "SB_MODEL_OPERATION_SEMANTIC_REFUSED_V1",
                    "key/value operation has no key or prefix expression");
        return FinishRefusal();
      }
      if ((!multi_get && key_expression_ids.size() != 1) ||
          !RequireSymbol(")", "key_value_operation_close_required",
                         "key/value operation requires a closing parenthesis")) {
        return FinishRefusal();
      }
      source.model_key_expression_ids = key_expression_ids;
    }

    NativeExpressionAstNode operation_root;
    operation_root.expression_id = NextExpressionId();
    operation_root.expression_kind = NativeExpressionAstKind::kFunctionCall;
    operation_root.child_expression_ids.push_back(alias_expression_id);
    if (!exact_get) {
      operation_root.child_expression_ids.insert(
          operation_root.child_expression_ids.end(), key_expression_ids.begin(),
          key_expression_ids.end());
    }
    operation_root.operator_name = exact_get
                                       ? "KV_KEY"
                                       : (multi_get ? "KV_MULTI_GET"
                                                    : "KV_PREFIX");
    operation_root.spelling = SourceSpelling(operation, Previous());
    operation_root.range = Span(operation, Previous());
    document_.expressions.push_back(std::move(operation_root));
    auto root_expression_id = document_.expressions.back().expression_id;

    if (exact_get) {
      if (AtEnd()) {
        RefuseExact("SB_MODEL_KEY_VALUE_OPERATOR_REFUSED_V1",
                    "KV_KEY requires equality");
        return FinishRefusal();
      }
      const auto comparison = BinaryOperatorFor(Current());
      if (!comparison.has_value() || comparison->canonical_name != "=") {
        RefuseExact("SB_MODEL_KEY_VALUE_OPERATOR_REFUSED_V1",
                    "KV_KEY admits equality only");
        return FinishRefusal();
      }
      Consume();
      const auto value_expression_id = ParseExpression(0, 0);
      if (!value_expression_id.has_value()) return FinishRefusal();
      // The grammar carries the exact key as the equality right operand. The
      // KV_KEY functionless node carries only its bound source alias.
      source.model_key_expression_ids = {*value_expression_id};
      source.model_comparison_operator = "=";
      NativeExpressionAstNode predicate;
      predicate.expression_id = NextExpressionId();
      predicate.expression_kind = NativeExpressionAstKind::kBinary;
      predicate.child_expression_ids = {root_expression_id,
                                         *value_expression_id};
      predicate.operator_name = "=";
      const Token& value_end = TokenForRangeEnd(
          document_.expressions[*value_expression_id - 1].range);
      predicate.spelling = SourceSpelling(operation, value_end);
      predicate.range = Span(operation, value_end);
      document_.expressions.push_back(std::move(predicate));
      root_expression_id = document_.expressions.back().expression_id;
    }
    relation.predicate_expression_ids = {root_expression_id};
    source_end = &Previous();
    if (AtSymbol(";")) Consume();
    if (!AtEnd()) {
      Refuse("key_value_trailing_input",
             "unexpected input follows the bounded key/value model query");
      return FinishRefusal();
    }
    relation.range = Span(source_operator, *source_end);
    source.range = relation.range;
    document_.catalog_relation_sources.push_back(std::move(source));
    document_.relations.push_back(std::move(relation));
    document_.root_relation_id = 1;
    document_.status = NativeRelationalParseStatus::kAccepted;
    (void)select;
    return std::move(document_);
  }

  NativeRelationalAstDocument ParseDocumentModelSelect() {
    document_.status = NativeRelationalParseStatus::kRefused;
    if (cst_.messages.has_errors()) {
      document_.messages = cst_.messages;
      return FinishRefusal();
    }
    if (tokens_.size() > kMaximumNativeRelationalTokens) {
      Refuse("token_limit_exceeded", "document model query token limit exceeded");
      return FinishRefusal();
    }

    const Token& select = Consume();
    std::vector<std::uint32_t> projection_expression_ids;
    if (AtSymbol("*")) {
      const Token& wildcard_token = Consume();
      NativeExpressionAstNode wildcard;
      wildcard.expression_id = NextExpressionId();
      wildcard.expression_kind = NativeExpressionAstKind::kWildcard;
      wildcard.spelling = wildcard_token.text;
      wildcard.range = TokenSourceRange(wildcard_token);
      projection_expression_ids.push_back(wildcard.expression_id);
      document_.expressions.push_back(std::move(wildcard));
    } else {
      while (!AtEnd()) {
        const auto expression_id = ParseExpression(0, 0);
        if (!expression_id.has_value()) return FinishRefusal();
        projection_expression_ids.push_back(*expression_id);
        if (!AtSymbol(",")) break;
        Consume();
      }
    }
    if (!RequireWord("FROM", "document_from_required",
                     "document model source requires FROM")) {
      return FinishRefusal();
    }
    if (AtEnd() || (!IsWord(Current(), "DOCUMENT_SOURCE") &&
                    !IsWord(Current(), "DOCUMENT_UNNEST"))) {
      Refuse("document_source_required",
             "FROM requires DOCUMENT_SOURCE or DOCUMENT_UNNEST");
      return FinishRefusal();
    }

    const Token& source_operator = Consume();
    const bool unnest = IsWord(source_operator, "DOCUMENT_UNNEST");
    if (!RequireSymbol("(", "document_source_open_required",
                       "document model source requires an opening parenthesis")) {
      return FinishRefusal();
    }
    NativeCatalogRelationSourceAstNode source;
    source.source_id = 1;
    source.source_kind = NativeRelationSourceAstKind::kDocument;
    source.model_family_id = "document";
    source.model_operation_id = unnest ? "DOCUMENT_UNNEST" : "DOCUMENT_FIND";
    if (unnest) {
      const auto document_expression_id = ParseExpression(0, 0);
      if (!document_expression_id.has_value()) return FinishRefusal();
      source.model_document_expression_id = *document_expression_id;
      const auto& document_expression =
          document_.expressions[*document_expression_id - 1];
      source.qualified_name_range = document_expression.range;
      if (!RequireSymbol(",", "document_unnest_path_required",
                         "DOCUMENT_UNNEST requires a typed path literal")) {
        return FinishRefusal();
      }
      if (AtEnd() || Current().kind != TokenKind::kStringLiteral) {
        Refuse("document_typed_path_required",
               "DOCUMENT_UNNEST path must be a typed string literal");
        return FinishRefusal();
      }
      const Token& path = Consume();
      NativeExpressionAstNode path_expression;
      path_expression.expression_id = NextExpressionId();
      path_expression.expression_kind = NativeExpressionAstKind::kLiteral;
      path_expression.literal_kind = NativeLiteralAstKind::kString;
      path_expression.spelling = path.text;
      path_expression.range = TokenSourceRange(path);
      source.model_path_expression_id = path_expression.expression_id;
      source.model_wildcard_path = path.text.find('*') != std::string::npos;
      document_.expressions.push_back(std::move(path_expression));
    } else {
      if (AtEnd() || !IsNameToken(Current())) {
        Refuse("document_qualified_name_required",
               "DOCUMENT_SOURCE requires a qualified collection name");
        return FinishRefusal();
      }
      const Token& first_name = Consume();
      const Token* last_name = &first_name;
      source.qualified_name.push_back(
          {first_name.text, first_name.quoted, TokenSourceRange(first_name)});
      while (AtSymbol(".")) {
        Consume();
        if (AtEnd() || !IsNameToken(Current())) {
          Refuse("document_qualified_name_incomplete",
                 "qualified document collection name is incomplete");
          return FinishRefusal();
        }
        last_name = &Consume();
        source.qualified_name.push_back(
            {last_name->text, last_name->quoted, TokenSourceRange(*last_name)});
      }
      source.qualified_name_range = Span(first_name, *last_name);
    }
    if (!RequireSymbol(")", "document_source_close_required",
                       "document model source requires a closing parenthesis")) {
      return FinishRefusal();
    }

    const Token* source_end = &Previous();
    if (!AtEnd() && IsWord(Current(), "AS")) {
      Consume();
      if (AtEnd() || !IsNameToken(Current())) {
        Refuse("document_alias_required", "AS requires a document source alias");
        return FinishRefusal();
      }
      const Token& alias = Consume();
      source.alias = NativeIdentifierAstNode{
          alias.text, alias.quoted, TokenSourceRange(alias)};
      source.alias_is_explicit = true;
      source_end = &alias;
    } else if (!AtEnd() && Current().kind == TokenKind::kIdentifier) {
      const Token& alias = Consume();
      source.alias = NativeIdentifierAstNode{
          alias.text, alias.quoted, TokenSourceRange(alias)};
      source_end = &alias;
    }

    NativeRelationAstNode relation;
    relation.relation_id = 1;
    relation.relation_kind = NativeRelationAstKind::kCatalogSource;
    relation.relation_source_ids = {source.source_id};
    relation.output_expression_ids = projection_expression_ids;
    relation.range = Span(source_operator, *source_end);

    if (!unnest && !AtEnd() && IsWord(Current(), "WHERE")) {
      Consume();
      if (AtEnd() || !IsWord(Current(), "DOCUMENT_PATH")) {
        Refuse("document_path_predicate_required",
               "bounded document WHERE requires DOCUMENT_PATH");
        return FinishRefusal();
      }
      const Token& function = Consume();
      if (!RequireSymbol("(", "document_path_open_required",
                         "DOCUMENT_PATH requires an opening parenthesis") ||
          AtEnd() || !IsNameToken(Current())) {
        return FinishRefusal();
      }
      const Token& alias = Consume();
      if (!source.alias.has_value() ||
          !SameIdentifier(*source.alias, alias)) {
        Refuse("document_alias_mismatch",
               "DOCUMENT_PATH alias does not name the bound document source");
        return FinishRefusal();
      }
      NativeExpressionAstNode alias_expression;
      alias_expression.expression_id = NextExpressionId();
      alias_expression.expression_kind = NativeExpressionAstKind::kIdentifier;
      alias_expression.qualified_identifier.push_back(
          {alias.text, alias.quoted, TokenSourceRange(alias)});
      alias_expression.spelling = alias.text;
      alias_expression.range = TokenSourceRange(alias);
      document_.expressions.push_back(std::move(alias_expression));
      const auto alias_expression_id = document_.expressions.back().expression_id;
      if (!RequireSymbol(",", "document_path_literal_required",
                         "DOCUMENT_PATH requires a typed path literal") ||
          AtEnd() || Current().kind != TokenKind::kStringLiteral) {
        return FinishRefusal();
      }
      const Token& path = Consume();
      NativeExpressionAstNode path_expression;
      path_expression.expression_id = NextExpressionId();
      path_expression.expression_kind = NativeExpressionAstKind::kLiteral;
      path_expression.literal_kind = NativeLiteralAstKind::kString;
      path_expression.spelling = path.text;
      path_expression.range = TokenSourceRange(path);
      document_.expressions.push_back(std::move(path_expression));
      const auto path_expression_id = document_.expressions.back().expression_id;
      source.model_path_expression_id = path_expression_id;
      source.model_wildcard_path = path.text.find('*') != std::string::npos;
      if (!RequireSymbol(")", "document_path_close_required",
                         "DOCUMENT_PATH requires a closing parenthesis")) {
        return FinishRefusal();
      }
      NativeExpressionAstNode path_call;
      path_call.expression_id = NextExpressionId();
      path_call.expression_kind = NativeExpressionAstKind::kFunctionCall;
      path_call.child_expression_ids = {alias_expression_id, path_expression_id};
      path_call.operator_name = "DOCUMENT_PATH";
      path_call.spelling = SourceSpelling(function, Previous());
      path_call.range = Span(function, Previous());
      document_.expressions.push_back(std::move(path_call));
      const auto path_call_id = document_.expressions.back().expression_id;

      if (AtEnd()) {
        Refuse("document_comparison_required",
               "DOCUMENT_PATH requires a comparison operator and typed expression");
        return FinishRefusal();
      }
      const auto comparison = BinaryOperatorFor(Current());
      if (!comparison.has_value() || comparison->precedence != 3 ||
          comparison->canonical_name == "LIKE" ||
          comparison->canonical_name == "ILIKE" ||
          comparison->canonical_name == "IS") {
        Refuse("document_comparison_unsupported",
               "DOCUMENT_PATH comparison operator is not in the signed profile");
        return FinishRefusal();
      }
      Consume();
      const auto value_expression = ParseExpression(0, 0);
      if (!value_expression.has_value()) return FinishRefusal();
      const auto value_expression_id = *value_expression;
      source.model_value_expression_id = value_expression_id;
      source.model_comparison_operator = comparison->canonical_name;
      source.model_operation_id = "DOCUMENT_PATH";

      NativeExpressionAstNode predicate;
      predicate.expression_id = NextExpressionId();
      predicate.expression_kind = NativeExpressionAstKind::kBinary;
      predicate.child_expression_ids = {path_call_id, value_expression_id};
      predicate.operator_name = comparison->canonical_name;
      const Token& value_end = TokenForRangeEnd(
          document_.expressions[value_expression_id - 1].range);
      predicate.spelling = SourceSpelling(function, value_end);
      predicate.range = Span(function, value_end);
      document_.expressions.push_back(std::move(predicate));
      relation.predicate_expression_ids = {document_.expressions.back().expression_id};
    }

    if (AtSymbol(";")) Consume();
    if (!AtEnd()) {
      Refuse("document_trailing_input",
             "unexpected input follows the bounded document model query");
      return FinishRefusal();
    }
    source.range = Span(source_operator, *source_end);
    document_.catalog_relation_sources.push_back(std::move(source));
    document_.relations.push_back(std::move(relation));
    document_.root_relation_id = 1;
    document_.status = NativeRelationalParseStatus::kAccepted;
    (void)select;
    return std::move(document_);
  }

  NativeRelationalAstDocument ParseGraphModelSelect() {
    // QOW-SOURCE-RCP-074-GRAPH-GRAMMAR-V1
    document_.status = NativeRelationalParseStatus::kRefused;
    if (cst_.messages.has_errors()) {
      document_.messages = cst_.messages;
      return FinishRefusal();
    }
    if (tokens_.size() > kMaximumNativeRelationalTokens) {
      Refuse("token_limit_exceeded", "graph model query token limit exceeded");
      return FinishRefusal();
    }

    const Token& select = Consume();
    std::vector<std::uint32_t> projection_expression_ids;
    if (AtSymbol("*")) {
      const Token& wildcard_token = Consume();
      NativeExpressionAstNode wildcard;
      wildcard.expression_id = NextExpressionId();
      wildcard.expression_kind = NativeExpressionAstKind::kWildcard;
      wildcard.spelling = wildcard_token.text;
      wildcard.range = TokenSourceRange(wildcard_token);
      projection_expression_ids.push_back(wildcard.expression_id);
      document_.expressions.push_back(std::move(wildcard));
    } else {
      while (!AtEnd()) {
        const auto expression_id = ParseExpression(0, 0);
        if (!expression_id.has_value()) return FinishRefusal();
        projection_expression_ids.push_back(*expression_id);
        if (!AtSymbol(",")) break;
        Consume();
      }
    }
    if (!RequireWord("FROM", "graph_from_required",
                     "graph model source requires FROM") ||
        AtEnd() || !IsWord(Current(), "GRAPH_SOURCE")) {
      Refuse("graph_source_required", "FROM requires GRAPH_SOURCE");
      return FinishRefusal();
    }

    const Token& source_operator = Consume();
    if (!RequireSymbol("(", "graph_source_open_required",
                       "GRAPH_SOURCE requires an opening parenthesis") ||
        AtEnd() || !IsNameToken(Current())) {
      Refuse("graph_qualified_name_required",
             "GRAPH_SOURCE requires a qualified graph name");
      return FinishRefusal();
    }
    NativeCatalogRelationSourceAstNode source;
    source.source_id = 1;
    source.source_kind = NativeRelationSourceAstKind::kGraph;
    source.model_family_id = "graph";
    source.model_operation_id = "GRAPH_MATCH";
    source.model_graph_cycle_policy = "visited_set";
    const Token& first_name = Consume();
    const Token* last_name = &first_name;
    source.qualified_name.push_back(
        {first_name.text, first_name.quoted, TokenSourceRange(first_name)});
    while (AtSymbol(".")) {
      Consume();
      if (AtEnd() || !IsNameToken(Current())) {
        Refuse("graph_qualified_name_incomplete",
               "qualified graph name is incomplete");
        return FinishRefusal();
      }
      last_name = &Consume();
      source.qualified_name.push_back(
          {last_name->text, last_name->quoted, TokenSourceRange(*last_name)});
    }
    source.qualified_name_range = Span(first_name, *last_name);
    if (!RequireSymbol(")", "graph_source_close_required",
                       "GRAPH_SOURCE requires a closing parenthesis")) {
      return FinishRefusal();
    }
    const Token* source_end = &Previous();
    if (!AtEnd() && IsWord(Current(), "AS")) {
      Consume();
      if (AtEnd() || !IsNameToken(Current())) {
        Refuse("graph_alias_invalid",
               "GRAPH_SOURCE AS requires an identifier");
        return FinishRefusal();
      }
      const Token& source_alias = Consume();
      source.alias = NativeIdentifierAstNode{
          source_alias.text, source_alias.quoted,
          TokenSourceRange(source_alias)};
      source.alias_is_explicit = true;
      source_end = &source_alias;
    } else {
      source.alias = NativeIdentifierAstNode{
          last_name->text, last_name->quoted, TokenSourceRange(*last_name)};
      source.alias_is_explicit = false;
    }

    const auto append_alias_expression = [&](const Token& alias) {
      NativeExpressionAstNode expression;
      expression.expression_id = NextExpressionId();
      expression.expression_kind = NativeExpressionAstKind::kIdentifier;
      expression.qualified_identifier.push_back(
          {alias.text, alias.quoted, TokenSourceRange(alias)});
      expression.spelling = alias.text;
      expression.range = TokenSourceRange(alias);
      document_.expressions.push_back(std::move(expression));
      return document_.expressions.back().expression_id;
    };
    const auto append_literal = [&](const Token& token,
                                    const NativeLiteralAstKind kind) {
      NativeExpressionAstNode expression;
      expression.expression_id = NextExpressionId();
      expression.expression_kind = NativeExpressionAstKind::kLiteral;
      expression.literal_kind = kind;
      expression.spelling = token.text;
      expression.range = TokenSourceRange(token);
      document_.expressions.push_back(std::move(expression));
      return document_.expressions.back().expression_id;
    };
    const auto parse_unsigned_depth = [&](std::uint64_t* value,
                                          std::uint32_t* expression_id) {
      if (AtEnd() || Current().kind != TokenKind::kNumericLiteral ||
          Current().text.empty()) {
        return false;
      }
      const Token& token = Consume();
      const auto parsed = std::from_chars(
          token.text.data(), token.text.data() + token.text.size(), *value);
      if (parsed.ec != std::errc{} ||
          parsed.ptr != token.text.data() + token.text.size()) {
        return false;
      }
      *expression_id = append_literal(token, NativeLiteralAstKind::kNumeric);
      document_.expressions.back().spelling = std::to_string(*value);
      return true;
    };

    NativeRelationAstNode relation;
    relation.relation_id = 1;
    relation.relation_kind = NativeRelationAstKind::kCatalogSource;
    relation.relation_source_ids = {source.source_id};
    relation.output_expression_ids = projection_expression_ids;

    if (AtSymbol(",")) {
      Consume();
      if (AtEnd() || !IsWord(Current(), "GRAPH_EXPAND")) {
        Refuse("graph_expand_required",
               "a second graph source must be GRAPH_EXPAND");
        return FinishRefusal();
      }
      const Token& expand = Consume();
      if (!RequireSymbol("(", "graph_expand_open_required",
                         "GRAPH_EXPAND requires an opening parenthesis") ||
          AtEnd() || !IsNameToken(Current())) {
        return FinishRefusal();
      }
      const Token& alias = Consume();
      if (!SameIdentifier(*source.alias, alias)) {
        Refuse("graph_alias_mismatch",
               "GRAPH_EXPAND alias does not name GRAPH_SOURCE");
        return FinishRefusal();
      }
      source.model_source_alias = source.alias;
      const auto alias_expression_id = append_alias_expression(alias);
      source.model_graph_alias_expression_id = alias_expression_id;
      if (!RequireSymbol(",", "graph_direction_required",
                         "GRAPH_EXPAND requires a direction") ||
          AtEnd()) {
        return FinishRefusal();
      }
      const Token& direction = Consume();
      const auto direction_name = CanonicalTokenText(direction);
      if (direction.quoted ||
          (direction_name != "OUTGOING" && direction_name != "INCOMING" &&
           direction_name != "BOTH")) {
        Refuse("graph_direction_invalid",
               "GRAPH_EXPAND direction must be OUTGOING, INCOMING, or BOTH");
        return FinishRefusal();
      }
      source.model_graph_direction = ToLowerAscii(direction_name);
      const auto direction_expression_id =
          append_literal(direction, NativeLiteralAstKind::kString);
      if (!RequireSymbol(",", "graph_minimum_depth_required",
                         "GRAPH_EXPAND requires a minimum depth")) {
        return FinishRefusal();
      }
      std::uint64_t minimum_depth = 0;
      std::uint32_t minimum_expression_id = 0;
      if (!parse_unsigned_depth(&minimum_depth, &minimum_expression_id) ||
          !RequireSymbol(",", "graph_maximum_depth_required",
                         "GRAPH_EXPAND requires a maximum depth")) {
        RefuseExact("SB_MODEL_GRAPH_UNBOUNDED_EXPANSION_REFUSED_V1",
                    "GRAPH_EXPAND requires finite unsigned depth bounds");
        return FinishRefusal();
      }
      std::uint64_t maximum_depth = 0;
      std::uint32_t maximum_expression_id = 0;
      if (!parse_unsigned_depth(&maximum_depth, &maximum_expression_id) ||
          maximum_depth < minimum_depth) {
        RefuseExact("SB_MODEL_GRAPH_UNBOUNDED_EXPANSION_REFUSED_V1",
                    "GRAPH_EXPAND depth bounds are inverted or unbounded");
        return FinishRefusal();
      }
      if (!RequireSymbol(")", "graph_expand_close_required",
                         "GRAPH_EXPAND requires a closing parenthesis")) {
        return FinishRefusal();
      }
      NativeExpressionAstNode cycle_policy;
      cycle_policy.expression_id = NextExpressionId();
      cycle_policy.expression_kind = NativeExpressionAstKind::kLiteral;
      cycle_policy.literal_kind = NativeLiteralAstKind::kString;
      cycle_policy.spelling = "visited_set";
      cycle_policy.range = TokenSourceRange(expand);
      document_.expressions.push_back(std::move(cycle_policy));
      const auto cycle_policy_expression_id =
          document_.expressions.back().expression_id;
      NativeExpressionAstNode root;
      root.expression_id = NextExpressionId();
      root.expression_kind = NativeExpressionAstKind::kFunctionCall;
      root.child_expression_ids = {alias_expression_id,
                                   direction_expression_id,
                                   minimum_expression_id,
                                   maximum_expression_id,
                                   cycle_policy_expression_id};
      root.operator_name = "GRAPH_EXPAND";
      root.spelling = SourceSpelling(expand, Previous());
      root.range = Span(expand, Previous());
      document_.expressions.push_back(std::move(root));
      relation.predicate_expression_ids = {
          document_.expressions.back().expression_id};
      source.model_operation_id = "GRAPH_EXPAND";
      source.model_graph_minimum_depth = minimum_depth;
      source.model_graph_maximum_depth = maximum_depth;
      if (!AtEnd() && IsWord(Current(), "AS")) {
        Consume();
        if (AtEnd() || !IsNameToken(Current())) {
          Refuse("graph_expand_alias_invalid",
                 "GRAPH_EXPAND AS requires a result identifier");
          return FinishRefusal();
        }
        const Token& result_alias = Consume();
        source.alias = NativeIdentifierAstNode{
            result_alias.text, result_alias.quoted,
            TokenSourceRange(result_alias)};
        source.alias_is_explicit = true;
        source_end = &result_alias;
      } else {
        source.alias = source.model_source_alias;
        source.alias_is_explicit = false;
      }
    } else {
      if (AtEnd() || !IsWord(Current(), "WHERE")) {
        Refuse("graph_match_required",
               "GRAPH_SOURCE requires typed GRAPH_MATCH in this bounded profile");
        return FinishRefusal();
      }
      Consume();
      if (AtEnd() || !IsWord(Current(), "GRAPH_MATCH")) {
        Refuse("graph_match_required", "graph WHERE requires GRAPH_MATCH");
        return FinishRefusal();
      }
      const Token& match = Consume();
      if (!RequireSymbol("(", "graph_match_open_required",
                         "GRAPH_MATCH requires an opening parenthesis") ||
          AtEnd() || !IsNameToken(Current())) {
        return FinishRefusal();
      }
      const Token& alias = Consume();
      if (!SameIdentifier(*source.alias, alias)) {
        Refuse("graph_alias_mismatch",
               "GRAPH_MATCH alias does not name GRAPH_SOURCE");
        return FinishRefusal();
      }
      const auto alias_expression_id = append_alias_expression(alias);
      source.model_graph_alias_expression_id = alias_expression_id;
      if (!RequireSymbol(",", "graph_pattern_required",
                         "GRAPH_MATCH requires a typed pattern literal") ||
          AtEnd() || Current().kind != TokenKind::kStringLiteral) {
        Refuse("graph_pattern_invalid",
               "GRAPH_MATCH pattern must be a typed string literal");
        return FinishRefusal();
      }
      const Token& pattern = Consume();
      if (!ExactBoundedGraphPatternV1(pattern.text)) {
        RefuseExact("SB_MODEL_OPERATION_SEMANTIC_REFUSED_V1",
                    "GRAPH_MATCH typed pattern is outside the bounded profile");
        return FinishRefusal();
      }
      const auto pattern_expression_id =
          append_literal(pattern, NativeLiteralAstKind::kString);
      source.model_pattern_expression_id = pattern_expression_id;
      if (!RequireSymbol(")", "graph_match_close_required",
                         "GRAPH_MATCH requires a closing parenthesis")) {
        return FinishRefusal();
      }
      NativeExpressionAstNode root;
      root.expression_id = NextExpressionId();
      root.expression_kind = NativeExpressionAstKind::kFunctionCall;
      root.child_expression_ids = {alias_expression_id,
                                   pattern_expression_id};
      root.operator_name = "GRAPH_MATCH";
      root.spelling = SourceSpelling(match, Previous());
      root.range = Span(match, Previous());
      document_.expressions.push_back(std::move(root));
      relation.predicate_expression_ids = {
          document_.expressions.back().expression_id};
      source_end = &Previous();
    }

    if (AtSymbol(";")) Consume();
    if (!AtEnd()) {
      Refuse("graph_trailing_input",
             "unexpected input follows the bounded graph model query");
      return FinishRefusal();
    }
    relation.range = Span(source_operator, *source_end);
    source.range = relation.range;
    document_.catalog_relation_sources.push_back(std::move(source));
    document_.relations.push_back(std::move(relation));
    document_.root_relation_id = 1;
    document_.status = NativeRelationalParseStatus::kAccepted;
    (void)select;
    return std::move(document_);
  }

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

  static bool IsBoundedCatalogGlobalAggregate(const Token& token) {
    static constexpr std::array<std::string_view, 43> kFunctionNames{
        "COUNT",       "SUM",          "AVG",      "MIN",
        "MAX",         "BOOL_AND",     "BOOL_OR",  "EVERY",
        "STDDEV_POP",  "VARIANCE_POP", "STDDEV",   "VARIANCE",
        "STDDEV_SAMP", "VARIANCE_SAMP", "CORR",     "COVAR_POP",
        "COVAR_SAMP",  "REGR_COUNT",   "REGR_AVGX", "REGR_AVGY",
        "REGR_INTERCEPT", "REGR_R2",   "REGR_SLOPE", "REGR_SXX",
        "REGR_SXY",    "REGR_SYY",     "APPROX_COUNT_DISTINCT",
        "APPROX_MEDIAN", "STRING_AGG",  "LISTAGG", "MODE",
        "PERCENTILE_CONT", "PERCENTILE_DISC", "RANK", "DENSE_RANK",
        "PERCENT_RANK", "CUME_DIST", "APPROX_PERCENTILE_CONT",
        "APPROX_PERCENTILE_DISC", "ARRAY_AGG", "JSON_AGG",
        "JSON_OBJECT_AGG", "APPROX_TOP_K"};
    const auto canonical = CanonicalTokenText(token);
    return std::ranges::find(kFunctionNames, canonical) !=
           kFunctionNames.end();
  }

  static bool IsBoundedCatalogPairAggregate(const Token& token) {
    static constexpr std::array<std::string_view, 12> kFunctionNames{
        "CORR",       "COVAR_POP",  "COVAR_SAMP", "REGR_COUNT",
        "REGR_AVGX",  "REGR_AVGY",  "REGR_INTERCEPT", "REGR_R2",
        "REGR_SLOPE", "REGR_SXX",   "REGR_SXY",   "REGR_SYY"};
    const auto canonical = CanonicalTokenText(token);
    return std::ranges::find(kFunctionNames, canonical) !=
           kFunctionNames.end();
  }

  static bool IsBoundedCatalogStringAggregate(const Token& token) {
    return CanonicalTokenText(token) == "STRING_AGG";
  }

  static bool IsBoundedCatalogListagg(const Token& token) {
    return CanonicalTokenText(token) == "LISTAGG";
  }

  static bool IsBoundedCatalogMode(const Token& token) {
    return CanonicalTokenText(token) == "MODE";
  }

  static bool IsBoundedCatalogPercentile(const Token& token) {
    const auto function = CanonicalTokenText(token);
    return function == "PERCENTILE_CONT" || function == "PERCENTILE_DISC" ||
           function == "APPROX_PERCENTILE_CONT" ||
           function == "APPROX_PERCENTILE_DISC";
  }

  static bool IsBoundedCatalogHypotheticalSet(const Token& token) {
    const auto function = CanonicalTokenText(token);
    return function == "RANK" || function == "DENSE_RANK" ||
           function == "PERCENT_RANK" || function == "CUME_DIST";
  }

  static bool IsBoundedCatalogOrderedSingleCollection(const Token& token) {
    const auto function = CanonicalTokenText(token);
    return function == "ARRAY_AGG" || function == "JSON_AGG";
  }

  static bool IsBoundedCatalogJsonObjectAggregate(const Token& token) {
    return CanonicalTokenText(token) == "JSON_OBJECT_AGG";
  }

  static bool IsBoundedCatalogApproxTopK(const Token& token) {
    return CanonicalTokenText(token) == "APPROX_TOP_K";
  }

  bool LooksLikeBoundedWindowSelect() const {
    return tokens_.size() >= 9 &&
           IsWord(*tokens_[1], "ROW_NUMBER") && tokens_[2]->text == "(" &&
           tokens_[3]->text == ")" && IsWord(*tokens_[4], "OVER") &&
           (tokens_[5]->text == "(" ||
            tokens_[5]->kind == TokenKind::kIdentifier);
  }

  NativeRelationalAstDocument ParseWindowSelect() {
    // QOW-SOURCE-RCP-050-TYPED-WINDOW-AST-V1
    // This first typed window surface deliberately recognizes only
    // ROW_NUMBER. The window specification itself is complete across the
    // partition/order/frame/exclusion axes and is carried independently of
    // the function-call expression for later engine-owned binding.
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
    const Token& function_token = Consume();
    if (!RequireSymbol("(", "window_function_open_required",
                       "ROW_NUMBER requires an opening parenthesis") ||
        !RequireSymbol(")", "window_function_close_required",
                       "ROW_NUMBER requires a closing parenthesis") ||
        !RequireWord("OVER", "window_over_required",
                     "ROW_NUMBER requires OVER")) {
      return FinishRefusal();
    }
    const Token* specification_open = nullptr;
    const Token* named_reference = nullptr;
    if (AtSymbol("(")) {
      specification_open = &Consume();
    } else if (!AtEnd() && Current().kind == TokenKind::kIdentifier &&
               !Current().quoted) {
      named_reference = &Consume();
    } else {
      Refuse("window_specification_or_name_required",
             "OVER requires a parenthesized specification or unquoted window name");
      return FinishRefusal();
    }

    NativeExpressionAstNode function;
    function.expression_id = NextExpressionId();
    function.expression_kind = NativeExpressionAstKind::kFunctionCall;
    function.operator_name = "ROW_NUMBER";
    function.spelling = SourceSpelling(function_token, *tokens_[3]);
    function.range = Span(function_token, *tokens_[3]);
    const auto function_expression_id = function.expression_id;
    document_.expressions.push_back(std::move(function));

    NativeWindowDefinitionAstNode definition;
    definition.window_id = 1;
    std::vector<std::uint32_t> source_expression_ids;
    const auto parse_identifier_list =
        [&](std::vector<std::uint32_t>* expression_ids,
            std::vector<NativeOrderingAstTerm>* ordering_terms) -> bool {
      while (true) {
        if (AtEnd() || Current().kind != TokenKind::kIdentifier) {
          Refuse("window_identifier_required",
                 "window PARTITION/ORDER terms require column identifiers");
          return false;
        }
        const Token& identifier_token = Consume();
        std::optional<std::uint32_t> interned_expression_id;
        for (const auto source_expression_id : source_expression_ids) {
          const auto& candidate =
              document_.expressions[source_expression_id - 1];
          if (candidate.expression_kind ==
                  NativeExpressionAstKind::kIdentifier &&
              candidate.spelling == identifier_token.text) {
            interned_expression_id = source_expression_id;
            break;
          }
        }
        if (!interned_expression_id.has_value()) {
          NativeExpressionAstNode identifier;
          identifier.expression_id = NextExpressionId();
          identifier.expression_kind = NativeExpressionAstKind::kIdentifier;
          identifier.spelling = identifier_token.text;
          identifier.range = TokenSourceRange(identifier_token);
          interned_expression_id = identifier.expression_id;
          document_.expressions.push_back(std::move(identifier));
          source_expression_ids.push_back(*interned_expression_id);
        }
        const auto expression_id = *interned_expression_id;
        expression_ids->push_back(expression_id);

        if (ordering_terms != nullptr) {
          NativeOrderingAstTerm term;
          term.expression_id = expression_id;
          term.direction = NativeSortDirection::kAscending;
          term.null_placement = NativeNullPlacement::kNullsLast;
          const Token* term_end = &identifier_token;
          if (!AtEnd() &&
              (IsWord(Current(), "ASC") || IsWord(Current(), "DESC"))) {
            const Token& direction = Consume();
            term.direction = IsWord(direction, "DESC")
                                 ? NativeSortDirection::kDescending
                                 : NativeSortDirection::kAscending;
            term.null_placement =
                term.direction == NativeSortDirection::kDescending
                    ? NativeNullPlacement::kNullsFirst
                    : NativeNullPlacement::kNullsLast;
            term_end = &direction;
          }
          if (!AtEnd() && IsWord(Current(), "NULLS")) {
            Consume();
            if (AtEnd() ||
                (!IsWord(Current(), "FIRST") &&
                 !IsWord(Current(), "LAST"))) {
              Refuse("window_null_placement_required",
                     "window NULLS requires FIRST or LAST");
              return false;
            }
            const Token& placement = Consume();
            term.null_placement = IsWord(placement, "FIRST")
                                      ? NativeNullPlacement::kNullsFirst
                                      : NativeNullPlacement::kNullsLast;
            term_end = &placement;
          }
          term.range = Span(identifier_token, *term_end);
          ordering_terms->push_back(std::move(term));
        }
        if (!AtSymbol(",")) break;
        Consume();
      }
      return true;
    };

    const auto parse_frame_bound = [&]()
        -> std::optional<NativeWindowFrameBoundAstNode> {
      NativeWindowFrameBoundAstNode bound;
      const Token* first = AtEnd() ? nullptr : &Current();
      if (first == nullptr) return std::nullopt;
      if (IsWord(Current(), "UNBOUNDED")) {
        Consume();
        if (AtEnd() ||
            (!IsWord(Current(), "PRECEDING") &&
             !IsWord(Current(), "FOLLOWING"))) {
          Refuse("window_unbounded_direction_required",
                 "UNBOUNDED requires PRECEDING or FOLLOWING");
          return std::nullopt;
        }
        const Token& direction = Consume();
        bound.bound_kind = IsWord(direction, "PRECEDING")
                               ? NativeWindowFrameBoundKind::kUnboundedPreceding
                               : NativeWindowFrameBoundKind::kUnboundedFollowing;
        bound.range = Span(*first, direction);
        return bound;
      }
      if (IsWord(Current(), "CURRENT")) {
        Consume();
        if (!RequireWord("ROW", "window_current_row_required",
                         "CURRENT requires ROW")) {
          return std::nullopt;
        }
        bound.bound_kind = NativeWindowFrameBoundKind::kCurrentRow;
        bound.range = Span(*first, Previous());
        return bound;
      }
      if (Current().kind != TokenKind::kNumericLiteral) {
        Refuse("window_frame_offset_required",
               "window frame bound requires a nonnegative numeric offset");
        return std::nullopt;
      }
      const Token& offset = Consume();
      NativeExpressionAstNode literal;
      literal.expression_id = NextExpressionId();
      literal.expression_kind = NativeExpressionAstKind::kLiteral;
      literal.literal_kind = NativeLiteralAstKind::kNumeric;
      literal.spelling = offset.text;
      literal.range = TokenSourceRange(offset);
      bound.offset_expression_id = literal.expression_id;
      document_.expressions.push_back(std::move(literal));
      if (AtEnd() ||
          (!IsWord(Current(), "PRECEDING") &&
           !IsWord(Current(), "FOLLOWING"))) {
        Refuse("window_frame_offset_direction_required",
               "window frame offset requires PRECEDING or FOLLOWING");
        return std::nullopt;
      }
      const Token& direction = Consume();
      bound.bound_kind = IsWord(direction, "PRECEDING")
                             ? NativeWindowFrameBoundKind::kPreceding
                             : NativeWindowFrameBoundKind::kFollowing;
      bound.range = Span(*first, direction);
      return bound;
    };
    const auto parse_specification = [&](NativeWindowDefinitionAstNode* target,
                                         const bool allow_base_name) {
      if (target == nullptr) return false;
      if (allow_base_name && !AtEnd() &&
          Current().kind == TokenKind::kIdentifier && !Current().quoted &&
          !IsWord(Current(), "PARTITION") && !IsWord(Current(), "ORDER") &&
          !IsWord(Current(), "ROWS") && !IsWord(Current(), "RANGE") &&
          !IsWord(Current(), "GROUPS") && !IsWord(Current(), "EXCLUDE")) {
        const Token& base = Consume();
        target->base_name = NativeIdentifierAstNode{
            ToLowerAscii(base.text), false, TokenSourceRange(base)};
      }
      if (!AtEnd() && IsWord(Current(), "PARTITION")) {
        Consume();
        if (!RequireWord("BY", "window_partition_by_required",
                         "window PARTITION requires BY") ||
            !parse_identifier_list(&target->partition_expression_ids,
                                   nullptr)) {
          return false;
        }
      }
      if (!AtEnd() && IsWord(Current(), "ORDER")) {
        Consume();
        if (!RequireWord("BY", "window_order_by_required",
                         "window ORDER requires BY")) {
          return false;
        }
        std::vector<std::uint32_t> ordered_expression_ids;
        if (!parse_identifier_list(&ordered_expression_ids,
                                   &target->ordering_terms)) {
          return false;
        }
      }
      if (!AtEnd() &&
          (IsWord(Current(), "ROWS") || IsWord(Current(), "RANGE") ||
           IsWord(Current(), "GROUPS"))) {
        const Token& unit = Consume();
        target->frame_unit =
            IsWord(unit, "ROWS")
                ? NativeWindowFrameUnit::kRows
                : (IsWord(unit, "RANGE") ? NativeWindowFrameUnit::kRange
                                          : NativeWindowFrameUnit::kGroups);
        if (!AtEnd() && IsWord(Current(), "BETWEEN")) {
          Consume();
          target->frame_start = parse_frame_bound();
          if (!target->frame_start.has_value() ||
              !RequireWord("AND", "window_frame_between_and_required",
                           "window BETWEEN requires AND")) {
            return false;
          }
          target->frame_end = parse_frame_bound();
          if (!target->frame_end.has_value()) return false;
        } else {
          target->frame_start = parse_frame_bound();
          if (!target->frame_start.has_value()) return false;
        }
      }
      if (!AtEnd() && IsWord(Current(), "EXCLUDE")) {
        Consume();
        if (!target->frame_unit.has_value() || AtEnd()) {
          Refuse("window_exclusion_required",
                 "EXCLUDE requires a frame and CURRENT ROW, GROUP, TIES, or NO OTHERS");
          return false;
        }
        if (IsWord(Current(), "CURRENT")) {
          Consume();
          if (!RequireWord("ROW", "window_exclude_current_row_required",
                           "EXCLUDE CURRENT requires ROW")) {
            return false;
          }
          target->exclusion = NativeWindowFrameExclusion::kCurrentRow;
        } else if (IsWord(Current(), "GROUP")) {
          Consume();
          target->exclusion = NativeWindowFrameExclusion::kGroup;
        } else if (IsWord(Current(), "TIES")) {
          Consume();
          target->exclusion = NativeWindowFrameExclusion::kTies;
        } else if (IsWord(Current(), "NO")) {
          Consume();
          if (!RequireWord("OTHERS", "window_exclude_no_others_required",
                           "EXCLUDE NO requires OTHERS")) {
            return false;
          }
        } else {
          Refuse("window_exclusion_invalid",
                 "EXCLUDE requires CURRENT ROW, GROUP, TIES, or NO OTHERS");
          return false;
        }
      }
      return true;
    };

    const Token* invocation_window_end = named_reference;
    if (specification_open != nullptr) {
      if (!parse_specification(&definition, false) ||
          !RequireSymbol(")", "window_specification_close_required",
                         "OVER specification requires a closing parenthesis")) {
        return FinishRefusal();
      }
      invocation_window_end = &Previous();
      definition.range = Span(*specification_open, *invocation_window_end);
      if (definition.partition_expression_ids.empty() &&
          definition.ordering_terms.empty()) {
        Refuse("window_partition_or_order_required",
               "typed ROW_NUMBER requires PARTITION BY or ORDER BY");
        return FinishRefusal();
      }
    }

    NativeWindowInvocationAstNode invocation;
    invocation.invocation_id = 1;
    invocation.function_expression_id = function_expression_id;
    invocation.window_definition_id = definition.window_id;
    invocation.range = Span(function_token, *invocation_window_end);
    if (!AtEnd() && IsWord(Current(), "AS")) {
      Consume();
      if (AtEnd() || Current().kind != TokenKind::kIdentifier) {
        Refuse("window_output_alias_required",
               "window AS requires an output alias");
        return FinishRefusal();
      }
      const Token& output_alias = Consume();
      invocation.output_alias = NativeIdentifierAstNode{
          output_alias.text, output_alias.quoted,
          TokenSourceRange(output_alias)};
    }
    if (!RequireWord("FROM", "window_from_required",
                     "typed window SELECT requires FROM") ||
        AtEnd() || Current().kind != TokenKind::kIdentifier) {
      Refuse("window_relation_name_required",
             "typed window FROM requires a catalog relation");
      return FinishRefusal();
    }

    NativeCatalogRelationSourceAstNode source;
    source.source_id = 1;
    const Token& first_name = Consume();
    const Token* last_name = &first_name;
    source.qualified_name.push_back({first_name.text, first_name.quoted,
                                     TokenSourceRange(first_name)});
    while (AtSymbol(".")) {
      Consume();
      if (AtEnd() || Current().kind != TokenKind::kIdentifier) {
        Refuse("window_relation_name_incomplete",
               "typed window relation name is incomplete");
        return FinishRefusal();
      }
      last_name = &Consume();
      source.qualified_name.push_back({last_name->text, last_name->quoted,
                                       TokenSourceRange(*last_name)});
    }
    source.qualified_name_range = Span(first_name, *last_name);
    const Token* source_end = last_name;
    if (!AtEnd() && IsWord(Current(), "AS")) {
      Consume();
      if (AtEnd() || Current().kind != TokenKind::kIdentifier) {
        Refuse("window_relation_alias_required",
               "typed window relation AS requires an alias");
        return FinishRefusal();
      }
      const Token& alias = Consume();
      source.alias = NativeIdentifierAstNode{
          alias.text, alias.quoted, TokenSourceRange(alias)};
      source.alias_is_explicit = true;
      source_end = &alias;
    } else if (!AtEnd() && Current().kind == TokenKind::kIdentifier &&
               !IsWord(Current(), "WINDOW") &&
               !IsWord(Current(), "QUALIFY")) {
      const Token& alias = Consume();
      source.alias = NativeIdentifierAstNode{
          alias.text, alias.quoted, TokenSourceRange(alias)};
      source_end = &alias;
    }
    source.range = Span(first_name, *source_end);
    const Token* statement_end = source_end;
    if (named_reference != nullptr) {
      if (!RequireWord("WINDOW", "named_window_clause_required",
                       "named OVER reference requires a WINDOW clause")) {
        return FinishRefusal();
      }
      while (true) {
        if (document_.window_definitions.size() >= 1024) {
          Refuse("named_window_definition_limit",
                 "named WINDOW definition bound was exceeded");
          return FinishRefusal();
        }
        if (AtEnd() || Current().kind != TokenKind::kIdentifier ||
            Current().quoted) {
          Refuse("named_window_name_required",
                 "WINDOW requires an unquoted definition name");
          return FinishRefusal();
        }
        const Token& name = Consume();
        NativeWindowDefinitionAstNode named_definition;
        named_definition.window_id = static_cast<std::uint32_t>(
            document_.window_definitions.size() + 1);
        named_definition.name = NativeIdentifierAstNode{
            ToLowerAscii(name.text), false, TokenSourceRange(name)};
        if (!RequireWord("AS", "named_window_as_required",
                         "named WINDOW definition requires AS") ||
            !RequireSymbol("(", "named_window_specification_open_required",
                           "named WINDOW definition requires an opening parenthesis")) {
          return FinishRefusal();
        }
        if (!parse_specification(&named_definition, true) ||
            !RequireSymbol(")", "named_window_specification_close_required",
                           "named WINDOW definition requires a closing parenthesis")) {
          return FinishRefusal();
        }
        const Token& close = Previous();
        named_definition.range = Span(name, close);
        statement_end = &close;
        document_.window_definitions.push_back(std::move(named_definition));
        if (!AtSymbol(",")) break;
        Consume();
      }

      struct EffectiveWindowShape {
        bool partition{false};
        bool ordering{false};
        bool frame{false};
      };
      std::unordered_map<std::string, std::size_t> definitions_by_name;
      std::vector<EffectiveWindowShape> effective_shapes;
      effective_shapes.reserve(document_.window_definitions.size());
      for (std::size_t index = 0;
           index < document_.window_definitions.size(); ++index) {
        const auto& candidate = document_.window_definitions[index];
        if (!candidate.name.has_value() || candidate.name->spelling.empty() ||
            !definitions_by_name
                 .emplace(candidate.name->spelling, index)
                 .second) {
          Refuse("named_window_duplicate",
                 "WINDOW definition names must be unique in declaration scope");
          return FinishRefusal();
        }
        EffectiveWindowShape shape{
            !candidate.partition_expression_ids.empty(),
            !candidate.ordering_terms.empty(), candidate.frame_unit.has_value()};
        if (candidate.base_name.has_value()) {
          const auto base =
              definitions_by_name.find(candidate.base_name->spelling);
          if (base == definitions_by_name.end() || base->second >= index) {
            Refuse("named_window_base_unknown_or_forward",
                   "named WINDOW base must be an earlier declaration");
            return FinishRefusal();
          }
          const auto& inherited = effective_shapes[base->second];
          if ((inherited.partition && shape.partition) ||
              (inherited.ordering && shape.ordering) ||
              (inherited.frame && shape.frame)) {
            Refuse("named_window_override",
                   "named WINDOW inheritance cannot replace PARTITION, ORDER, or frame state");
            return FinishRefusal();
          }
          shape.partition = shape.partition || inherited.partition;
          shape.ordering = shape.ordering || inherited.ordering;
          shape.frame = shape.frame || inherited.frame;
        }
        effective_shapes.push_back(shape);
      }
      const auto reference_key = ToLowerAscii(named_reference->text);
      const auto selected = definitions_by_name.find(reference_key);
      if (selected == definitions_by_name.end() ||
          (!effective_shapes[selected->second].partition &&
           !effective_shapes[selected->second].ordering)) {
        Refuse("named_window_reference_unknown_or_empty",
               "OVER name must resolve to a keyed WINDOW definition");
        return FinishRefusal();
      }
      invocation.window_definition_id =
          document_.window_definitions[selected->second].window_id;
    } else {
      document_.window_definitions.push_back(std::move(definition));
    }
    std::optional<std::uint32_t> qualify_predicate_expression_id;
    const Token* qualify_end = nullptr;
    if (!AtEnd() && IsWord(Current(), "QUALIFY")) {
      Consume();
      if (AtEnd() ||
          (Current().kind != TokenKind::kIdentifier &&
           Current().kind != TokenKind::kKeyword) ||
          Current().quoted ||
          (invocation.output_alias.has_value() &&
           invocation.output_alias->quoted)) {
        Refuse("qualify_window_output_required",
               "QUALIFY requires an unquoted window-result name");
        return FinishRefusal();
      }
      const Token& output_reference = Consume();
      const auto expected_output_name = ToLowerAscii(
          invocation.output_alias.has_value()
              ? invocation.output_alias->spelling
              : std::string("row_number"));
      if (ToLowerAscii(output_reference.text) != expected_output_name) {
        Refuse("qualify_window_output_unresolved",
               "QUALIFY may reference only the selected window result in this bounded profile");
        return FinishRefusal();
      }
      if (AtEnd()) {
        Refuse("qualify_comparison_required",
               "QUALIFY window result requires a comparison operator");
        return FinishRefusal();
      }
      const Token& comparison = Consume();
      const auto canonical_comparison = CanonicalTokenText(comparison);
      if (canonical_comparison != "=" && canonical_comparison != "<>" &&
          canonical_comparison != "!=" && canonical_comparison != "<" &&
          canonical_comparison != "<=" && canonical_comparison != ">" &&
          canonical_comparison != ">=") {
        Refuse("qualify_comparison_unsupported",
               "QUALIFY requires a canonical numeric comparison operator");
        return FinishRefusal();
      }
      if (AtEnd() || Current().kind != TokenKind::kNumericLiteral) {
        Refuse("qualify_numeric_literal_required",
               "QUALIFY comparison requires an unsigned numeric literal");
        return FinishRefusal();
      }
      const Token& literal_token = Consume();
      NativeExpressionAstNode literal;
      literal.expression_id = NextExpressionId();
      literal.expression_kind = NativeExpressionAstKind::kLiteral;
      literal.literal_kind = NativeLiteralAstKind::kNumeric;
      literal.spelling = literal_token.text;
      literal.range = TokenSourceRange(literal_token);
      const auto literal_id = literal.expression_id;
      document_.expressions.push_back(std::move(literal));

      NativeExpressionAstNode predicate;
      predicate.expression_id = NextExpressionId();
      predicate.expression_kind = NativeExpressionAstKind::kBinary;
      predicate.child_expression_ids = {function_expression_id, literal_id};
      predicate.operator_name = canonical_comparison;
      predicate.spelling =
          SourceSpelling(output_reference, literal_token);
      predicate.range = Span(output_reference, literal_token);
      qualify_predicate_expression_id = predicate.expression_id;
      document_.expressions.push_back(std::move(predicate));
      qualify_end = &literal_token;
      statement_end = qualify_end;
    }
    if (AtSymbol(";")) Consume();
    if (!AtEnd()) {
      Refuse("window_clause_unsupported",
             "typed window slice does not admit trailing clauses");
      return FinishRefusal();
    }

    NativeRelationAstNode source_relation;
    source_relation.relation_id = 1;
    source_relation.relation_kind = NativeRelationAstKind::kCatalogSource;
    source_relation.relation_source_ids = {source.source_id};
    source_relation.output_expression_ids = source_expression_ids;
    source_relation.range = Span(select_token, *source_end);
    NativeRelationAstNode window_relation;
    window_relation.relation_id = 2;
    window_relation.relation_kind = NativeRelationAstKind::kWindow;
    window_relation.input_relation_ids = {source_relation.relation_id};
    window_relation.output_expression_ids = {function_expression_id};
    window_relation.window_invocation_ids = {invocation.invocation_id};
    window_relation.range = Span(select_token, *statement_end);

    document_.catalog_relation_sources.push_back(std::move(source));
    document_.window_invocations.push_back(std::move(invocation));
    document_.relations.push_back(std::move(source_relation));
    document_.relations.push_back(std::move(window_relation));
    document_.root_relation_id = 2;
    if (qualify_predicate_expression_id.has_value()) {
      NativeRelationAstNode qualify_relation;
      qualify_relation.relation_id = 3;
      qualify_relation.relation_kind = NativeRelationAstKind::kQualify;
      qualify_relation.input_relation_ids = {2};
      qualify_relation.output_expression_ids = {function_expression_id};
      qualify_relation.predicate_expression_ids = {
          *qualify_predicate_expression_id};
      qualify_relation.range = Span(select_token, *qualify_end);
      document_.relations.push_back(std::move(qualify_relation));
      document_.root_relation_id = 3;
    }
    document_.status = NativeRelationalParseStatus::kAccepted;
    return std::move(document_);
  }

  bool LooksLikeBoundedCatalogJoinSelect() const {
    if (tokens_.size() < 8 || tokens_[1]->text != "*" ||
        !IsWord(*tokens_[2], "FROM")) {
      return false;
    }
    for (std::size_t index = 3; index + 1 < tokens_.size(); ++index) {
      const bool accepted_kind =
          IsWord(*tokens_[index], "CROSS") ||
          IsWord(*tokens_[index], "INNER") ||
          IsWord(*tokens_[index], "LEFT") ||
          IsWord(*tokens_[index], "RIGHT") ||
          IsWord(*tokens_[index], "FULL");
      if (accepted_kind && IsWord(*tokens_[index + 1], "JOIN")) {
        return true;
      }
      if (accepted_kind && index + 2 < tokens_.size() &&
          IsWord(*tokens_[index + 1], "OUTER") &&
          IsWord(*tokens_[index + 2], "JOIN")) {
        return true;
      }
      if (IsWord(*tokens_[index], "LEFT") &&
          index + 2 < tokens_.size() &&
          (IsWord(*tokens_[index + 1], "SEMI") ||
           IsWord(*tokens_[index + 1], "ANTI")) &&
          IsWord(*tokens_[index + 2], "JOIN")) {
        return true;
      }
    }
    return false;
  }

  NativeRelationalAstDocument ParseCatalogJoinSelect() {
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
    if (!RequireSymbol("*", "catalog_cross_join_wildcard_required",
                       "bounded catalog CROSS JOIN requires SELECT *") ||
        !RequireWord("FROM", "catalog_cross_join_from_required",
                     "bounded catalog CROSS JOIN requires FROM")) {
      return FinishRefusal();
    }

    const auto parse_source = [&](const std::uint32_t source_id)
        -> std::optional<NativeCatalogRelationSourceAstNode> {
      if (AtEnd() || Current().kind != TokenKind::kIdentifier) {
        Refuse("catalog_cross_join_relation_required",
               "bounded catalog CROSS JOIN requires a qualified relation");
        return std::nullopt;
      }
      NativeCatalogRelationSourceAstNode source;
      source.source_id = source_id;
      source.source_kind = NativeRelationSourceAstKind::kCatalogRelation;
      const Token& first = Consume();
      const Token* last = &first;
      source.qualified_name.push_back(
          {first.text, first.quoted, TokenSourceRange(first)});
      while (AtSymbol(".")) {
        Consume();
        if (AtEnd() || Current().kind != TokenKind::kIdentifier) {
          Refuse("catalog_cross_join_relation_name_incomplete",
                 "bounded catalog CROSS JOIN relation name is incomplete");
          return std::nullopt;
        }
        last = &Consume();
        source.qualified_name.push_back(
            {last->text, last->quoted, TokenSourceRange(*last)});
      }
      source.qualified_name_range = Span(first, *last);
      source.range = source.qualified_name_range;
      return source;
    };

    auto left_source = parse_source(1);
    if (!left_source.has_value()) return FinishRefusal();
    NativeJoinAstKind join_kind = NativeJoinAstKind::kNone;
    std::string_view join_word;
    if (!AtEnd() && IsWord(Current(), "CROSS")) {
      join_kind = NativeJoinAstKind::kCross;
      join_word = "CROSS";
    } else if (!AtEnd() && IsWord(Current(), "INNER")) {
      join_kind = NativeJoinAstKind::kInner;
      join_word = "INNER";
    } else if (!AtEnd() && IsWord(Current(), "LEFT")) {
      join_kind = NativeJoinAstKind::kLeftOuter;
      join_word = "LEFT";
    } else if (!AtEnd() && IsWord(Current(), "RIGHT")) {
      join_kind = NativeJoinAstKind::kRightOuter;
      join_word = "RIGHT";
    } else if (!AtEnd() && IsWord(Current(), "FULL")) {
      join_kind = NativeJoinAstKind::kFullOuter;
      join_word = "FULL";
    }
    if (join_kind == NativeJoinAstKind::kNone ||
        !RequireWord(join_word,
                     "catalog_join_kind_required",
                     "bounded catalog JOIN kind is required")) {
      return FinishRefusal();
    }
    const bool outer_join =
        join_kind == NativeJoinAstKind::kLeftOuter ||
        join_kind == NativeJoinAstKind::kRightOuter ||
        join_kind == NativeJoinAstKind::kFullOuter;
    if (join_kind == NativeJoinAstKind::kLeftOuter && !AtEnd() &&
        IsWord(Current(), "SEMI")) {
      Consume();
      join_kind = NativeJoinAstKind::kLeftSemi;
    } else if (join_kind == NativeJoinAstKind::kLeftOuter && !AtEnd() &&
               IsWord(Current(), "ANTI")) {
      Consume();
      join_kind = NativeJoinAstKind::kLeftAnti;
    } else if (outer_join && !AtEnd() && IsWord(Current(), "OUTER")) {
      Consume();
    }
    if (
        !RequireWord("JOIN", "catalog_join_join_required",
                     "bounded catalog JOIN requires JOIN")) {
      return FinishRefusal();
    }
    auto right_source = parse_source(2);
    if (!right_source.has_value()) return FinishRefusal();
    struct ParsedJoinPredicateNode {
      bool comparison{false};
      const Token* left_key{nullptr};
      const Token* right_key{nullptr};
      std::string operator_name;
      std::size_t left_child{0};
      std::size_t right_child{0};
      const Token* first{nullptr};
      const Token* last{nullptr};
    };
    std::vector<ParsedJoinPredicateNode> predicate_nodes;
    std::optional<std::size_t> predicate_root;
    const auto parse_join_comparison = [&]()
        -> std::optional<std::size_t> {
      if (AtEnd() || Current().kind != TokenKind::kIdentifier) {
        Refuse("catalog_inner_join_left_key_required",
               "bounded catalog JOIN requires a left key identifier");
        return std::nullopt;
      }
      ParsedJoinPredicateNode comparison;
      comparison.comparison = true;
      comparison.left_key = &Consume();
      comparison.first = comparison.left_key;
      if (!AtEnd() && Current().kind == TokenKind::kOperator &&
          (Current().text == "=" || Current().text == "<>" ||
           Current().text == "!=" || Current().text == "<" ||
           Current().text == "<=" || Current().text == ">" ||
           Current().text == ">=")) {
        comparison.operator_name = CanonicalTokenText(Consume());
      } else if (!AtEnd() && IsWord(Current(), "IS")) {
        Consume();
        const bool negate = !AtEnd() && IsWord(Current(), "NOT");
        if (negate) Consume();
        if (!RequireWord(
                "DISTINCT", "catalog_join_distinct_operator_required",
                "bounded catalog JOIN requires DISTINCT after IS [NOT]") ||
            !RequireWord(
                "FROM", "catalog_join_distinct_from_required",
                "bounded catalog JOIN requires FROM after IS [NOT] DISTINCT")) {
          return std::nullopt;
        }
        comparison.operator_name =
            negate ? "IS NOT DISTINCT FROM" : "IS DISTINCT FROM";
      } else {
        Refuse("catalog_join_comparison_required",
               "bounded catalog JOIN requires a typed comparison predicate");
        return std::nullopt;
      }
      if (AtEnd() || Current().kind != TokenKind::kIdentifier) {
        Refuse("catalog_inner_join_right_key_required",
               "bounded catalog JOIN requires a right key identifier");
        return std::nullopt;
      }
      comparison.right_key = &Consume();
      comparison.last = comparison.right_key;
      predicate_nodes.push_back(std::move(comparison));
      return predicate_nodes.size() - 1;
    };
    std::function<std::optional<std::size_t>()> parse_join_or;
    std::function<std::optional<std::size_t>()> parse_join_and;
    std::function<std::optional<std::size_t>()> parse_join_primary;
    const auto make_boolean_node = [&](const std::size_t left,
                                       const std::size_t right,
                                       std::string operator_name) {
      ParsedJoinPredicateNode boolean_node;
      boolean_node.left_child = left;
      boolean_node.right_child = right;
      boolean_node.operator_name = std::move(operator_name);
      boolean_node.first = predicate_nodes[left].first;
      boolean_node.last = predicate_nodes[right].last;
      predicate_nodes.push_back(std::move(boolean_node));
      return predicate_nodes.size() - 1;
    };
    parse_join_primary = [&]() -> std::optional<std::size_t> {
      if (!AtSymbol("(")) return parse_join_comparison();
      Consume();
      const auto nested = parse_join_or();
      if (!nested.has_value() ||
          !RequireSymbol(
              ")", "catalog_join_predicate_parenthesis_required",
              "bounded catalog JOIN predicate parenthesis is not closed")) {
        return std::nullopt;
      }
      return nested;
    };
    parse_join_and = [&]() -> std::optional<std::size_t> {
      auto left = parse_join_primary();
      while (left.has_value() && !AtEnd() && IsWord(Current(), "AND")) {
        Consume();
        const auto right = parse_join_primary();
        if (!right.has_value()) return std::nullopt;
        left = make_boolean_node(*left, *right, "AND");
      }
      return left;
    };
    parse_join_or = [&]() -> std::optional<std::size_t> {
      auto left = parse_join_and();
      while (left.has_value() && !AtEnd() && IsWord(Current(), "OR")) {
        Consume();
        const auto right = parse_join_and();
        if (!right.has_value()) return std::nullopt;
        left = make_boolean_node(*left, *right, "OR");
      }
      return left;
    };
    if (join_kind != NativeJoinAstKind::kCross) {
      if (!RequireWord("ON", "catalog_inner_join_on_required",
                       "bounded catalog JOIN requires ON")) {
        return FinishRefusal();
      }
      predicate_root = parse_join_or();
      if (!predicate_root.has_value()) return FinishRefusal();
      if (predicate_nodes.size() > 32) {
        Refuse("catalog_join_predicate_node_limit_exceeded",
               "bounded catalog JOIN predicate exceeds 32 typed nodes");
        return FinishRefusal();
      }
    }
    const Token& query_end = join_kind != NativeJoinAstKind::kCross
                                 ? *predicate_nodes[*predicate_root].last
                                 : TokenForRangeEnd(right_source->range);
    if (AtSymbol(";")) Consume();
    if (!AtEnd()) {
      Refuse("catalog_cross_join_clause_unsupported",
             "bounded catalog CROSS JOIN does not admit aliases or clauses");
      return FinishRefusal();
    }

    std::vector<std::uint32_t> source_wildcard_ids;
    for (std::uint32_t source_id = 1; source_id <= 2; ++source_id) {
      NativeExpressionAstNode wildcard;
      wildcard.expression_id = NextExpressionId();
      wildcard.expression_kind = NativeExpressionAstKind::kWildcard;
      wildcard.spelling = "*";
      wildcard.range = TokenSourceRange(select_token);
      const auto wildcard_id = wildcard.expression_id;
      document_.expressions.push_back(std::move(wildcard));
      source_wildcard_ids.push_back(wildcard_id);

      NativeRelationAstNode source_relation;
      source_relation.relation_id = source_id;
      source_relation.relation_kind = NativeRelationAstKind::kCatalogSource;
      source_relation.relation_source_ids = {source_id};
      source_relation.output_expression_ids = {wildcard_id};
      source_relation.range = source_id == 1 ? left_source->range
                                             : right_source->range;
      document_.relations.push_back(std::move(source_relation));
    }
    NativeRelationAstNode join;
    join.relation_id = 3;
    join.relation_kind = NativeRelationAstKind::kJoin;
    join.join_kind = join_kind;
    join.input_relation_ids = {1, 2};
    join.output_expression_ids =
        join_kind == NativeJoinAstKind::kLeftSemi ||
                join_kind == NativeJoinAstKind::kLeftAnti
            ? std::vector<std::uint32_t>{source_wildcard_ids.front()}
            : source_wildcard_ids;
    if (join_kind != NativeJoinAstKind::kCross) {
      std::vector<std::uint32_t> predicate_expression_ids(
          predicate_nodes.size());
      for (std::size_t node_ordinal = 0;
           node_ordinal < predicate_nodes.size(); ++node_ordinal) {
        const auto& parsed = predicate_nodes[node_ordinal];
        if (parsed.comparison) {
          NativeExpressionAstNode left_key;
          left_key.expression_id = NextExpressionId();
          left_key.expression_kind = NativeExpressionAstKind::kIdentifier;
          left_key.spelling = parsed.left_key->text;
          left_key.range = TokenSourceRange(*parsed.left_key);
          const auto left_key_id = left_key.expression_id;
          document_.expressions.push_back(std::move(left_key));

          NativeExpressionAstNode right_key;
          right_key.expression_id = NextExpressionId();
          right_key.expression_kind = NativeExpressionAstKind::kIdentifier;
          right_key.spelling = parsed.right_key->text;
          right_key.range = TokenSourceRange(*parsed.right_key);
          const auto right_key_id = right_key.expression_id;
          document_.expressions.push_back(std::move(right_key));

          NativeExpressionAstNode predicate;
          predicate.expression_id = NextExpressionId();
          predicate.expression_kind = NativeExpressionAstKind::kBinary;
          predicate.child_expression_ids = {left_key_id, right_key_id};
          predicate.operator_name = parsed.operator_name;
          predicate.range = Span(*parsed.first, *parsed.last);
          predicate_expression_ids[node_ordinal] = predicate.expression_id;
          document_.expressions.push_back(std::move(predicate));
          continue;
        }
        NativeExpressionAstNode predicate;
        predicate.expression_id = NextExpressionId();
        predicate.expression_kind = NativeExpressionAstKind::kBinary;
        predicate.child_expression_ids = {
            predicate_expression_ids[parsed.left_child],
            predicate_expression_ids[parsed.right_child]};
        predicate.operator_name = parsed.operator_name;
        predicate.range = Span(*parsed.first, *parsed.last);
        predicate_expression_ids[node_ordinal] = predicate.expression_id;
        document_.expressions.push_back(std::move(predicate));
      }
      join.predicate_expression_ids = {
          predicate_expression_ids[*predicate_root]};
    }
    join.range = Span(select_token, query_end);
    document_.relations.push_back(std::move(join));
    document_.catalog_relation_sources.push_back(std::move(*left_source));
    document_.catalog_relation_sources.push_back(std::move(*right_source));
    document_.root_relation_id = 3;
    document_.status = NativeRelationalParseStatus::kAccepted;
    return std::move(document_);
  }

  bool LooksLikeBoundedCatalogRelationSelect() const {
    // The candidate owns wildcard, simple identifier-list projection, and the
    // exact global COUNT(*) projection. Other computed projections and
    // aggregates remain available to their established parser routes.
    if (tokens_.size() < 3) return false;
    std::size_t cursor = 1;
    if (tokens_[cursor]->text == "*") {
      ++cursor;
    } else if (cursor + 3 < tokens_.size() &&
               IsBoundedCatalogGlobalAggregate(*tokens_[cursor]) &&
               tokens_[cursor + 1]->text == "(") {
      if (IsBoundedCatalogPercentile(*tokens_[cursor]) ||
          IsBoundedCatalogHypotheticalSet(*tokens_[cursor])) {
        if (cursor + 10 >= tokens_.size() ||
            tokens_[cursor + 2]->kind != TokenKind::kNumericLiteral ||
            tokens_[cursor + 3]->text != ")" ||
            !IsWord(*tokens_[cursor + 4], "WITHIN") ||
            !IsWord(*tokens_[cursor + 5], "GROUP") ||
            tokens_[cursor + 6]->text != "(" ||
            !IsWord(*tokens_[cursor + 7], "ORDER") ||
            !IsWord(*tokens_[cursor + 8], "BY") ||
            tokens_[cursor + 9]->kind != TokenKind::kIdentifier ||
            tokens_[cursor + 10]->text != ")") {
          return false;
        }
        cursor += 11;
      } else if (IsBoundedCatalogOrderedSingleCollection(
                     *tokens_[cursor])) {
        if (cursor + 6 >= tokens_.size() ||
            tokens_[cursor + 2]->kind != TokenKind::kIdentifier ||
            !IsWord(*tokens_[cursor + 3], "ORDER") ||
            !IsWord(*tokens_[cursor + 4], "BY") ||
            tokens_[cursor + 5]->kind != TokenKind::kIdentifier ||
            tokens_[cursor + 6]->text != ")") {
          return false;
        }
        cursor += 7;
      } else if (IsBoundedCatalogJsonObjectAggregate(*tokens_[cursor])) {
        if (cursor + 8 >= tokens_.size() ||
            tokens_[cursor + 2]->kind != TokenKind::kIdentifier ||
            tokens_[cursor + 3]->text != "," ||
            tokens_[cursor + 4]->kind != TokenKind::kIdentifier ||
            !IsWord(*tokens_[cursor + 5], "ORDER") ||
            !IsWord(*tokens_[cursor + 6], "BY") ||
            tokens_[cursor + 7]->kind != TokenKind::kIdentifier ||
            tokens_[cursor + 8]->text != ")") {
          return false;
        }
        cursor += 9;
      } else if (IsBoundedCatalogApproxTopK(*tokens_[cursor])) {
        if (cursor + 5 >= tokens_.size() ||
            tokens_[cursor + 2]->kind != TokenKind::kIdentifier ||
            tokens_[cursor + 3]->text != "," ||
            tokens_[cursor + 4]->kind != TokenKind::kNumericLiteral ||
            tokens_[cursor + 5]->text != ")") {
          return false;
        }
        cursor += 6;
      } else if (IsBoundedCatalogMode(*tokens_[cursor])) {
        if (cursor + 9 >= tokens_.size() ||
            tokens_[cursor + 2]->text != ")" ||
            !IsWord(*tokens_[cursor + 3], "WITHIN") ||
            !IsWord(*tokens_[cursor + 4], "GROUP") ||
            tokens_[cursor + 5]->text != "(" ||
            !IsWord(*tokens_[cursor + 6], "ORDER") ||
            !IsWord(*tokens_[cursor + 7], "BY") ||
            tokens_[cursor + 8]->kind != TokenKind::kIdentifier ||
            tokens_[cursor + 9]->text != ")") {
          return false;
        }
        cursor += 10;
      } else if (IsBoundedCatalogPairAggregate(*tokens_[cursor])) {
        if (cursor + 5 >= tokens_.size() ||
            tokens_[cursor + 2]->kind != TokenKind::kIdentifier ||
            tokens_[cursor + 3]->text != "," ||
            tokens_[cursor + 4]->kind != TokenKind::kIdentifier ||
            tokens_[cursor + 5]->text != ")") {
          return false;
        }
        cursor += 6;
      } else if (IsBoundedCatalogStringAggregate(*tokens_[cursor])) {
        if (cursor + 5 >= tokens_.size() ||
            tokens_[cursor + 2]->kind != TokenKind::kIdentifier ||
            tokens_[cursor + 3]->text != "," ||
            tokens_[cursor + 4]->kind != TokenKind::kStringLiteral ||
            tokens_[cursor + 5]->text != ")") {
          return false;
        }
        cursor += 6;
      } else if (IsBoundedCatalogListagg(*tokens_[cursor])) {
        if (cursor + 12 >= tokens_.size() ||
            tokens_[cursor + 2]->kind != TokenKind::kIdentifier ||
            tokens_[cursor + 3]->text != "," ||
            tokens_[cursor + 4]->kind != TokenKind::kStringLiteral ||
            tokens_[cursor + 5]->text != ")" ||
            !IsWord(*tokens_[cursor + 6], "WITHIN") ||
            !IsWord(*tokens_[cursor + 7], "GROUP") ||
            tokens_[cursor + 8]->text != "(" ||
            !IsWord(*tokens_[cursor + 9], "ORDER") ||
            !IsWord(*tokens_[cursor + 10], "BY") ||
            tokens_[cursor + 11]->kind != TokenKind::kIdentifier ||
            tokens_[cursor + 12]->text != ")") {
          return false;
        }
        cursor += 13;
      } else {
        if ((tokens_[cursor + 2]->text != "*" &&
             tokens_[cursor + 2]->kind != TokenKind::kIdentifier) ||
            tokens_[cursor + 3]->text != ")") {
          return false;
        }
        cursor += 4;
      }
    } else {
      while (cursor < tokens_.size()) {
        if (tokens_[cursor]->kind != TokenKind::kIdentifier) return false;
        ++cursor;
        if (cursor >= tokens_.size() || tokens_[cursor]->text != ",") break;
        ++cursor;
      }
    }
    return cursor < tokens_.size() && IsWord(*tokens_[cursor], "FROM");
  }

  NativeRelationalAstDocument ParseCatalogRelationSelect() {
    // This bounded parser slice records catalog source syntax only. Catalog
    // lookup, UUID binding, SBLR lowering, planning, and execution remain in
    // their separately owned stages.
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
    std::vector<std::uint32_t> projection_expression_ids;
    std::vector<std::uint32_t> source_projection_expression_ids;
    std::optional<std::uint32_t> global_aggregate_expression_id;
    std::string global_aggregate_function;
    const bool global_aggregate =
        !AtEnd() &&
        IsBoundedCatalogGlobalAggregate(Current());
    bool global_count_star = false;
    if (global_aggregate) {
      const Token& function_token = Consume();
      global_aggregate_function = CanonicalTokenText(function_token);
      const bool pair_aggregate =
          IsBoundedCatalogPairAggregate(function_token);
      const bool string_aggregate =
          IsBoundedCatalogStringAggregate(function_token);
      const bool listagg = IsBoundedCatalogListagg(function_token);
      const bool mode = IsBoundedCatalogMode(function_token);
      const bool percentile = IsBoundedCatalogPercentile(function_token);
      const bool hypothetical_set =
          IsBoundedCatalogHypotheticalSet(function_token);
      const bool ordered_single_collection =
          IsBoundedCatalogOrderedSingleCollection(function_token);
      const bool json_object_aggregate =
          IsBoundedCatalogJsonObjectAggregate(function_token);
      const bool ordered_collection =
          ordered_single_collection || json_object_aggregate;
      const bool approx_top_k = IsBoundedCatalogApproxTopK(function_token);
      const bool direct_numeric_ordered_set =
          percentile || hypothetical_set;
      if (!RequireSymbol("(", "catalog_aggregate_open_required",
                         "bounded catalog aggregate requires an opening parenthesis")) {
        return FinishRefusal();
      }
      std::vector<std::uint32_t> argument_expression_ids;
      std::optional<std::uint32_t> reserved_aggregate_expression_id;
      std::optional<std::size_t> reserved_aggregate_expression_index;
      std::optional<std::string> deferred_separator_spelling;
      std::optional<SourceRange> deferred_separator_range;
      std::optional<std::string> deferred_numeric_spelling;
      std::optional<SourceRange> deferred_numeric_range;
      if (direct_numeric_ordered_set) {
        if (AtEnd() || Current().kind != TokenKind::kNumericLiteral) {
          Refuse("catalog_percentile_fraction_required",
                 "bounded percentile aggregate requires a numeric fraction");
          return FinishRefusal();
        }
        const Token& fraction_token = Consume();
        deferred_numeric_spelling = fraction_token.text;
        deferred_numeric_range = TokenSourceRange(fraction_token);
      } else if (mode) {
        // MODE owns no direct argument. Its ordered value is bound below
        // from the exact WITHIN GROUP clause so source handles remain first.
      } else if (AtSymbol("*")) {
        if (global_aggregate_function != "COUNT") {
          Refuse("catalog_aggregate_wildcard_unsupported",
                 "only bounded catalog COUNT admits a wildcard argument");
          return FinishRefusal();
        }
        global_count_star = true;
        Consume();
        NativeExpressionAstNode wildcard;
        wildcard.expression_id = NextExpressionId();
        wildcard.expression_kind = NativeExpressionAstKind::kWildcard;
        wildcard.spelling = "*";
        wildcard.range = TokenSourceRange(function_token);
        source_projection_expression_ids.push_back(wildcard.expression_id);
        document_.expressions.push_back(std::move(wildcard));
      } else if (!AtEnd() && Current().kind == TokenKind::kIdentifier) {
        const Token& argument_token = Consume();
        NativeExpressionAstNode argument;
        argument.expression_id = NextExpressionId();
        argument.expression_kind = NativeExpressionAstKind::kIdentifier;
        argument.spelling = argument_token.text;
        argument.range = TokenSourceRange(argument_token);
        argument_expression_ids.push_back(argument.expression_id);
        source_projection_expression_ids.push_back(argument.expression_id);
        document_.expressions.push_back(std::move(argument));
        if (pair_aggregate) {
          if (!AtSymbol(",")) {
            Refuse("catalog_aggregate_pair_separator_required",
                   "bounded pair aggregate requires two source identifiers");
            return FinishRefusal();
          }
          Consume();
          if (AtEnd() || Current().kind != TokenKind::kIdentifier) {
            Refuse("catalog_aggregate_pair_argument_required",
                   "bounded pair aggregate requires a second source identifier");
            return FinishRefusal();
          }
          const Token& second_argument_token = Consume();
          if (second_argument_token.text == argument_token.text) {
            argument_expression_ids.push_back(
                argument_expression_ids.front());
          } else {
            NativeExpressionAstNode second_argument;
            second_argument.expression_id = NextExpressionId();
            second_argument.expression_kind =
                NativeExpressionAstKind::kIdentifier;
            second_argument.spelling = second_argument_token.text;
            second_argument.range = TokenSourceRange(second_argument_token);
            argument_expression_ids.push_back(second_argument.expression_id);
            source_projection_expression_ids.push_back(
                second_argument.expression_id);
            document_.expressions.push_back(std::move(second_argument));
          }
        } else if (approx_top_k) {
          if (!AtSymbol(",")) {
            Refuse("catalog_approx_top_k_separator_required",
                   "bounded APPROX_TOP_K requires a numeric result bound");
            return FinishRefusal();
          }
          Consume();
          if (AtEnd() || Current().kind != TokenKind::kNumericLiteral) {
            Refuse("catalog_approx_top_k_bound_required",
                   "bounded APPROX_TOP_K requires a numeric result bound");
            return FinishRefusal();
          }
          const Token& bound_token = Consume();
          reserved_aggregate_expression_id = NextExpressionId();
          NativeExpressionAstNode aggregate_placeholder;
          aggregate_placeholder.expression_id =
              *reserved_aggregate_expression_id;
          reserved_aggregate_expression_index = document_.expressions.size();
          document_.expressions.push_back(std::move(aggregate_placeholder));
          NativeExpressionAstNode bound;
          bound.expression_id = NextExpressionId();
          bound.expression_kind = NativeExpressionAstKind::kLiteral;
          bound.literal_kind = NativeLiteralAstKind::kNumeric;
          bound.spelling = bound_token.text;
          bound.range = TokenSourceRange(bound_token);
          argument_expression_ids.insert(argument_expression_ids.begin(),
                                         bound.expression_id);
          document_.expressions.push_back(std::move(bound));
        } else if (json_object_aggregate) {
          if (!AtSymbol(",")) {
            Refuse("catalog_json_object_aggregate_separator_required",
                   "bounded JSON_OBJECT_AGG requires key and value identifiers");
            return FinishRefusal();
          }
          Consume();
          if (AtEnd() || Current().kind != TokenKind::kIdentifier) {
            Refuse("catalog_json_object_aggregate_value_required",
                   "bounded JSON_OBJECT_AGG requires a value identifier");
            return FinishRefusal();
          }
          const Token& value_token = Consume();
          if (value_token.text == argument_token.text) {
            argument_expression_ids.push_back(
                argument_expression_ids.front());
          } else {
            NativeExpressionAstNode value_expression;
            value_expression.expression_id = NextExpressionId();
            value_expression.expression_kind =
                NativeExpressionAstKind::kIdentifier;
            value_expression.spelling = value_token.text;
            value_expression.range = TokenSourceRange(value_token);
            argument_expression_ids.push_back(value_expression.expression_id);
            source_projection_expression_ids.push_back(
                value_expression.expression_id);
            document_.expressions.push_back(std::move(value_expression));
          }
        } else if (string_aggregate || listagg) {
          if (!AtSymbol(",")) {
            Refuse("catalog_string_aggregate_separator_required",
                   "bounded STRING_AGG requires a text separator");
            return FinishRefusal();
          }
          Consume();
          if (AtEnd() || Current().kind != TokenKind::kStringLiteral) {
            Refuse("catalog_string_aggregate_literal_required",
                   "bounded STRING_AGG separator must be a text literal");
            return FinishRefusal();
          }
          const Token& separator_token = Consume();
          if (listagg) {
            deferred_separator_spelling = separator_token.text;
            deferred_separator_range = TokenSourceRange(separator_token);
          } else {
            reserved_aggregate_expression_id = NextExpressionId();
            NativeExpressionAstNode aggregate_placeholder;
            aggregate_placeholder.expression_id =
                *reserved_aggregate_expression_id;
            reserved_aggregate_expression_index =
                document_.expressions.size();
            document_.expressions.push_back(
                std::move(aggregate_placeholder));
            NativeExpressionAstNode separator;
            separator.expression_id = NextExpressionId();
            separator.expression_kind = NativeExpressionAstKind::kLiteral;
            separator.literal_kind = NativeLiteralAstKind::kString;
            separator.spelling = separator_token.text;
            separator.range = TokenSourceRange(separator_token);
            argument_expression_ids.push_back(separator.expression_id);
            document_.expressions.push_back(std::move(separator));
          }
        }
      } else {
        Refuse("catalog_aggregate_argument_required",
               "bounded catalog aggregate requires one source identifier");
        return FinishRefusal();
      }
      if (ordered_collection) {
        if (AtEnd() || !IsWord(Current(), "ORDER")) {
          Refuse("catalog_collection_order_required",
                 "bounded collection aggregate requires ORDER BY");
          return FinishRefusal();
        }
        Consume();
        if (AtEnd() || !IsWord(Current(), "BY")) {
          Refuse("catalog_collection_order_by_required",
                 "bounded collection aggregate requires ORDER BY");
          return FinishRefusal();
        }
        Consume();
        if (AtEnd() || Current().kind != TokenKind::kIdentifier) {
          Refuse("catalog_collection_order_binding_required",
                 "bounded collection aggregate requires a persisted order identifier");
          return FinishRefusal();
        }
        const Token& order_token = Consume();
        const auto existing_order_expression = std::ranges::find_if(
            document_.expressions, [&](const auto& candidate) {
              return candidate.expression_kind ==
                         NativeExpressionAstKind::kIdentifier &&
                     candidate.spelling == order_token.text;
            });
        if (existing_order_expression != document_.expressions.end()) {
          argument_expression_ids.push_back(
              existing_order_expression->expression_id);
        } else {
          NativeExpressionAstNode order_expression;
          order_expression.expression_id = NextExpressionId();
          order_expression.expression_kind =
              NativeExpressionAstKind::kIdentifier;
          order_expression.spelling = order_token.text;
          order_expression.range = TokenSourceRange(order_token);
          argument_expression_ids.push_back(order_expression.expression_id);
          source_projection_expression_ids.push_back(
              order_expression.expression_id);
          document_.expressions.push_back(std::move(order_expression));
        }
      }
      if (!AtSymbol(")")) {
        if (!document_.messages.has_errors()) {
          Refuse("catalog_aggregate_close_required",
                 "bounded catalog aggregate requires a closing parenthesis");
        }
        return FinishRefusal();
      }
      const Token& close_token = Consume();
      const Token* aggregate_close_token = &close_token;
      if (listagg || mode || direct_numeric_ordered_set) {
        if (AtEnd() || !IsWord(Current(), "WITHIN")) {
          Refuse("catalog_ordered_set_within_group_required",
                 "bounded ordered aggregate requires WITHIN GROUP ordering");
          return FinishRefusal();
        }
        Consume();
        if (AtEnd() || !IsWord(Current(), "GROUP")) {
          Refuse("catalog_ordered_set_group_required",
                 "bounded ordered aggregate requires WITHIN GROUP ordering");
          return FinishRefusal();
        }
        Consume();
        if (!RequireSymbol("(", "catalog_ordered_set_order_open_required",
                           "bounded ordered aggregate requires an opening parenthesis")) {
          return FinishRefusal();
        }
        if (AtEnd() || !IsWord(Current(), "ORDER")) {
          Refuse("catalog_ordered_set_order_required",
                 "bounded ordered aggregate requires ORDER BY");
          return FinishRefusal();
        }
        Consume();
        if (AtEnd() || !IsWord(Current(), "BY")) {
          Refuse("catalog_ordered_set_order_by_required",
                 "bounded ordered aggregate requires ORDER BY");
          return FinishRefusal();
        }
        Consume();
        if (AtEnd() || Current().kind != TokenKind::kIdentifier) {
          Refuse("catalog_ordered_set_order_binding_required",
                 "bounded ordered aggregate requires a persisted identifier");
          return FinishRefusal();
        }
        const Token& order_token = Consume();
        std::uint32_t order_expression_id = 0;
        const auto value_expression = (mode || direct_numeric_ordered_set)
            ? document_.expressions.end()
            : std::ranges::find_if(
                  document_.expressions, [&](const auto& candidate) {
                    return candidate.expression_id ==
                           argument_expression_ids.front();
                  });
        if (!mode && !direct_numeric_ordered_set &&
            value_expression != document_.expressions.end() &&
            order_token.text == value_expression->spelling) {
          order_expression_id = argument_expression_ids.front();
        } else {
          NativeExpressionAstNode order_expression;
          order_expression.expression_id = NextExpressionId();
          order_expression.expression_kind =
              NativeExpressionAstKind::kIdentifier;
          order_expression.spelling = order_token.text;
          order_expression.range = TokenSourceRange(order_token);
          order_expression_id = order_expression.expression_id;
          source_projection_expression_ids.push_back(order_expression_id);
          document_.expressions.push_back(std::move(order_expression));
        }
        if (!AtSymbol(")")) {
          Refuse("catalog_ordered_set_order_close_required",
                 "bounded ordered aggregate requires a closing parenthesis");
          return FinishRefusal();
        }
        aggregate_close_token = &Consume();
        if (listagg) {
          reserved_aggregate_expression_id = NextExpressionId();
          NativeExpressionAstNode aggregate_placeholder;
          aggregate_placeholder.expression_id =
              *reserved_aggregate_expression_id;
          reserved_aggregate_expression_index = document_.expressions.size();
          document_.expressions.push_back(std::move(aggregate_placeholder));
          NativeExpressionAstNode separator;
          separator.expression_id = NextExpressionId();
          separator.expression_kind = NativeExpressionAstKind::kLiteral;
          separator.literal_kind = NativeLiteralAstKind::kString;
          separator.spelling = *deferred_separator_spelling;
          separator.range = *deferred_separator_range;
          argument_expression_ids.push_back(separator.expression_id);
          document_.expressions.push_back(std::move(separator));
        } else if (direct_numeric_ordered_set) {
          reserved_aggregate_expression_id = NextExpressionId();
          NativeExpressionAstNode aggregate_placeholder;
          aggregate_placeholder.expression_id =
              *reserved_aggregate_expression_id;
          reserved_aggregate_expression_index = document_.expressions.size();
          document_.expressions.push_back(std::move(aggregate_placeholder));
          NativeExpressionAstNode fraction;
          fraction.expression_id = NextExpressionId();
          fraction.expression_kind = NativeExpressionAstKind::kLiteral;
          fraction.literal_kind = NativeLiteralAstKind::kNumeric;
          fraction.spelling = *deferred_numeric_spelling;
          fraction.range = *deferred_numeric_range;
          argument_expression_ids.push_back(fraction.expression_id);
          document_.expressions.push_back(std::move(fraction));
        }
        argument_expression_ids.push_back(order_expression_id);
      }
      NativeExpressionAstNode aggregate;
      aggregate.expression_id =
          reserved_aggregate_expression_id.has_value()
              ? *reserved_aggregate_expression_id
              : NextExpressionId();
      aggregate.expression_kind = NativeExpressionAstKind::kFunctionCall;
      aggregate.operator_name = global_aggregate_function;
      aggregate.child_expression_ids = std::move(argument_expression_ids);
      aggregate.spelling =
          SourceSpelling(function_token, *aggregate_close_token);
      aggregate.range = Span(function_token, *aggregate_close_token);
      global_aggregate_expression_id = aggregate.expression_id;
      projection_expression_ids.push_back(aggregate.expression_id);
      if (reserved_aggregate_expression_index.has_value()) {
        document_.expressions[*reserved_aggregate_expression_index] =
            std::move(aggregate);
      } else {
        document_.expressions.push_back(std::move(aggregate));
      }
    } else if (AtSymbol("*")) {
      const Token& wildcard_token = Consume();
      NativeExpressionAstNode wildcard;
      wildcard.expression_id = NextExpressionId();
      wildcard.expression_kind = NativeExpressionAstKind::kWildcard;
      wildcard.spelling = wildcard_token.text;
      wildcard.range = TokenSourceRange(wildcard_token);
      projection_expression_ids.push_back(wildcard.expression_id);
      source_projection_expression_ids.push_back(wildcard.expression_id);
      document_.expressions.push_back(std::move(wildcard));
    } else {
      while (true) {
        if (AtEnd() || Current().kind != TokenKind::kIdentifier) {
          Refuse("catalog_select_projection_identifier_required",
                 "bounded catalog SELECT projection requires an identifier");
          return FinishRefusal();
        }
        const Token& identifier_token = Consume();
        NativeExpressionAstNode identifier;
        identifier.expression_id = NextExpressionId();
        identifier.expression_kind = NativeExpressionAstKind::kIdentifier;
        identifier.spelling = identifier_token.text;
        identifier.range = TokenSourceRange(identifier_token);
        projection_expression_ids.push_back(identifier.expression_id);
        source_projection_expression_ids.push_back(identifier.expression_id);
        document_.expressions.push_back(std::move(identifier));
        if (!AtSymbol(",")) break;
        Consume();
      }
    }

    if (!RequireWord("FROM", "catalog_select_from_required",
                     "bounded catalog SELECT requires FROM")) {
      return FinishRefusal();
    }
    if (AtEnd() || Current().kind != TokenKind::kIdentifier) {
      Refuse("catalog_relation_name_required",
             "catalog relation source requires an identifier");
      return FinishRefusal();
    }

    NativeCatalogRelationSourceAstNode source;
    source.source_id = 1;
    source.source_kind = NativeRelationSourceAstKind::kCatalogRelation;
    const Token& first_name_token = Consume();
    const Token* last_name_token = &first_name_token;
    source.qualified_name.push_back(NativeIdentifierAstNode{
        first_name_token.text, first_name_token.quoted,
        TokenSourceRange(first_name_token)});

    while (AtSymbol(".")) {
      Consume();
      if (AtEnd() || Current().kind != TokenKind::kIdentifier) {
        Refuse("catalog_relation_name_incomplete",
               "qualified catalog relation name is incomplete");
        return FinishRefusal();
      }
      last_name_token = &Consume();
      source.qualified_name.push_back(NativeIdentifierAstNode{
          last_name_token->text, last_name_token->quoted,
          TokenSourceRange(*last_name_token)});
    }
    source.qualified_name_range = Span(first_name_token, *last_name_token);

    const Token* source_end = last_name_token;
    if (!AtEnd() && IsWord(Current(), "AS")) {
      Consume();
      if (AtEnd() || Current().kind != TokenKind::kIdentifier) {
        Refuse("catalog_relation_alias_required",
               "AS must be followed by a catalog relation alias");
        return FinishRefusal();
      }
      const Token& alias_token = Consume();
      source.alias = NativeIdentifierAstNode{
          alias_token.text, alias_token.quoted, TokenSourceRange(alias_token)};
      source.alias_is_explicit = true;
      source_end = &alias_token;
    } else if (!AtEnd() && Current().kind == TokenKind::kIdentifier) {
      const Token& alias_token = Consume();
      source.alias = NativeIdentifierAstNode{
          alias_token.text, alias_token.quoted, TokenSourceRange(alias_token)};
      source.alias_is_explicit = false;
      source_end = &alias_token;
    }
    source.range = Span(first_name_token, *source_end);

    std::optional<std::uint32_t> predicate_expression_id;
    std::optional<std::uint32_t> hidden_predicate_expression_id;
    if (!AtEnd() && IsWord(Current(), "WHERE")) {
      Consume();
      predicate_expression_id = ParseExpression(0, 0);
      if (!predicate_expression_id.has_value()) return FinishRefusal();
      const auto& predicate =
          document_.expressions[*predicate_expression_id - 1];
      const NativeExpressionAstNode* left = nullptr;
      const NativeExpressionAstNode* right = nullptr;
      if (predicate.expression_kind == NativeExpressionAstKind::kBinary &&
          predicate.child_expression_ids.size() == 2) {
        left = &document_.expressions[predicate.child_expression_ids[0] - 1];
        right = &document_.expressions[predicate.child_expression_ids[1] - 1];
      }
      const bool accepted_operator =
          predicate.operator_name == "=" || predicate.operator_name == "<>" ||
          predicate.operator_name == "!=" || predicate.operator_name == "<" ||
          predicate.operator_name == "<=" || predicate.operator_name == ">" ||
          predicate.operator_name == ">=";
      if (left == nullptr || right == nullptr || !accepted_operator ||
          left->expression_kind != NativeExpressionAstKind::kIdentifier ||
          right->expression_kind != NativeExpressionAstKind::kLiteral ||
          right->literal_kind != NativeLiteralAstKind::kNumeric ||
          !left->child_expression_ids.empty() ||
          !right->child_expression_ids.empty()) {
        Refuse("catalog_select_where_profile_unsupported",
               "bounded catalog WHERE requires an identifier comparison to "
               "an unsigned numeric literal");
        return FinishRefusal();
      }
      const bool wildcard_projection =
          global_count_star ||
          (projection_expression_ids.size() == 1 &&
          document_.expressions[projection_expression_ids.front() - 1]
                  .expression_kind == NativeExpressionAstKind::kWildcard);
      const bool predicate_is_projected = std::ranges::any_of(
          projection_expression_ids, [&](const auto expression_id) {
            const auto& projection =
                document_.expressions[expression_id - 1];
            return projection.expression_kind ==
                       NativeExpressionAstKind::kIdentifier &&
                   projection.spelling == left->spelling;
          });
      if (!wildcard_projection && !predicate_is_projected) {
        hidden_predicate_expression_id = left->expression_id;
      }
    }

    std::vector<NativeOrderingAstTerm> ordering_terms;
    std::vector<std::uint32_t> hidden_order_expression_ids;
    const Token* ordering_end = nullptr;
    std::vector<std::uint32_t> limit_expression_ids;
    const Token* query_end = source_end;
    if (predicate_expression_id.has_value()) {
      query_end = &TokenForRangeEnd(
          document_.expressions[*predicate_expression_id - 1].range);
    }
    if (!AtEnd() && IsWord(Current(), "ORDER")) {
      if (global_aggregate) {
        Refuse("catalog_aggregate_order_unsupported",
               "bounded catalog aggregate does not admit ORDER BY");
        return FinishRefusal();
      }
      Consume();
      if (!RequireWord("BY", "catalog_select_order_by_required",
                       "bounded catalog SELECT ORDER requires BY")) {
        return FinishRefusal();
      }
      std::unordered_set<std::string> ordered_names;
      while (true) {
        if (AtEnd() || Current().kind != TokenKind::kIdentifier) {
          Refuse("catalog_select_order_identifier_required",
                 "bounded catalog ORDER BY requires a column identifier");
          return FinishRefusal();
        }
        const Token& identifier_token = Consume();
        if (!ordered_names.insert(identifier_token.text).second) {
          Refuse("catalog_select_order_identifier_duplicate",
                 "bounded catalog ORDER BY does not repeat a column identifier");
          return FinishRefusal();
        }

        std::optional<std::uint32_t> source_expression_id;
        for (const auto expression_id : projection_expression_ids) {
          const auto& expression = document_.expressions[expression_id - 1];
          if (expression.expression_kind ==
                  NativeExpressionAstKind::kIdentifier &&
              expression.spelling == identifier_token.text) {
            source_expression_id = expression_id;
            break;
          }
        }
        if (!source_expression_id.has_value() &&
            hidden_predicate_expression_id.has_value()) {
          const auto& expression = document_.expressions[
              *hidden_predicate_expression_id - 1];
          if (expression.spelling == identifier_token.text) {
            source_expression_id = *hidden_predicate_expression_id;
          }
        }
        if (!source_expression_id.has_value()) {
          NativeExpressionAstNode identifier;
          identifier.expression_id = NextExpressionId();
          identifier.expression_kind = NativeExpressionAstKind::kIdentifier;
          identifier.spelling = identifier_token.text;
          identifier.range = TokenSourceRange(identifier_token);
          source_expression_id = identifier.expression_id;
          const bool wildcard_projection =
              projection_expression_ids.size() == 1 &&
              document_.expressions[projection_expression_ids.front() - 1]
                      .expression_kind == NativeExpressionAstKind::kWildcard;
          if (!wildcard_projection) {
            hidden_order_expression_ids.push_back(identifier.expression_id);
          }
          document_.expressions.push_back(std::move(identifier));
        }

        NativeOrderingAstTerm term;
        term.expression_id = *source_expression_id;
        term.direction = NativeSortDirection::kAscending;
        term.null_placement = NativeNullPlacement::kNullsLast;
        const Token* term_end = &identifier_token;
        if (!AtEnd() &&
            (IsWord(Current(), "ASC") || IsWord(Current(), "DESC"))) {
          const Token& direction_token = Consume();
          term.direction = IsWord(direction_token, "DESC")
                               ? NativeSortDirection::kDescending
                               : NativeSortDirection::kAscending;
          term.null_placement =
              term.direction == NativeSortDirection::kDescending
                  ? NativeNullPlacement::kNullsFirst
                  : NativeNullPlacement::kNullsLast;
          term_end = &direction_token;
        }
        if (!AtEnd() && IsWord(Current(), "NULLS")) {
          Consume();
          if (AtEnd() ||
              (!IsWord(Current(), "FIRST") && !IsWord(Current(), "LAST"))) {
            Refuse("catalog_select_order_null_placement_required",
                   "bounded catalog ORDER BY NULLS requires FIRST or LAST");
            return FinishRefusal();
          }
          const Token& placement_token = Consume();
          term.null_placement = IsWord(placement_token, "FIRST")
                                    ? NativeNullPlacement::kNullsFirst
                                    : NativeNullPlacement::kNullsLast;
          term_end = &placement_token;
        }
        term.range = Span(identifier_token, *term_end);
        query_end = term_end;
        ordering_end = term_end;
        ordering_terms.push_back(std::move(term));
        if (!AtSymbol(",")) break;
        Consume();
      }
    }
    if (!AtEnd() && IsWord(Current(), "LIMIT")) {
      Consume();
      const auto parse_row_bound = [&](const std::string_view diagnostic_id,
                                       const std::string_view detail)
          -> std::optional<std::uint32_t> {
        if (AtEnd() || Current().kind != TokenKind::kNumericLiteral) {
          Refuse(std::string(diagnostic_id), std::string(detail));
          return std::nullopt;
        }
        const Token& literal_token = Consume();
        std::uint64_t parsed = 0;
        const auto [end, error] = std::from_chars(
            literal_token.text.data(),
            literal_token.text.data() + literal_token.text.size(), parsed);
        if (error != std::errc{} ||
            end != literal_token.text.data() + literal_token.text.size()) {
          Refuse(std::string(diagnostic_id), std::string(detail));
          return std::nullopt;
        }
        NativeExpressionAstNode literal;
        literal.expression_id = NextExpressionId();
        literal.expression_kind = NativeExpressionAstKind::kLiteral;
        literal.literal_kind = NativeLiteralAstKind::kNumeric;
        literal.spelling = literal_token.text;
        literal.range = TokenSourceRange(literal_token);
        const auto expression_id = literal.expression_id;
        document_.expressions.push_back(std::move(literal));
        query_end = &literal_token;
        return expression_id;
      };
      const auto count = parse_row_bound(
          "catalog_select_limit_bound_required",
          "bounded catalog SELECT LIMIT requires an unsigned numeric bound");
      if (!count.has_value()) return FinishRefusal();
      limit_expression_ids.push_back(*count);
      if (!AtEnd() && IsWord(Current(), "OFFSET")) {
        Consume();
        const auto offset = parse_row_bound(
            "catalog_select_offset_bound_required",
            "bounded catalog SELECT OFFSET requires an unsigned numeric bound");
        if (!offset.has_value()) return FinishRefusal();
        limit_expression_ids.push_back(*offset);
      }
    }

    if (AtSymbol(";")) Consume();
    if (!AtEnd()) {
      Refuse("catalog_select_clause_unsupported",
             "bounded catalog SELECT does not admit additional clauses or sources");
      return FinishRefusal();
    }

    NativeRelationAstNode relation;
    relation.relation_id = 1;
    relation.relation_kind = NativeRelationAstKind::kCatalogSource;
    relation.relation_source_ids = {source.source_id};
    relation.output_expression_ids = source_projection_expression_ids;
    if (hidden_predicate_expression_id.has_value()) {
      relation.output_expression_ids.push_back(
          *hidden_predicate_expression_id);
    }
    relation.output_expression_ids.insert(
        relation.output_expression_ids.end(), hidden_order_expression_ids.begin(),
        hidden_order_expression_ids.end());
    relation.range = Span(select_token, *source_end);
    document_.catalog_relation_sources.push_back(std::move(source));
    document_.relations.push_back(std::move(relation));
    document_.root_relation_id = 1;
    if (predicate_expression_id.has_value()) {
      NativeRelationAstNode filter;
      filter.relation_id = 2;
      filter.relation_kind = NativeRelationAstKind::kFilter;
      filter.input_relation_ids = {1};
      filter.output_expression_ids =
          document_.relations.front().output_expression_ids;
      filter.predicate_expression_ids = {*predicate_expression_id};
      filter.range = Span(
          select_token,
          TokenForRangeEnd(
              document_.expressions[*predicate_expression_id - 1].range));
      document_.relations.push_back(std::move(filter));
      document_.root_relation_id = 2;
    }
    if (!ordering_terms.empty()) {
      NativeRelationAstNode sort;
      sort.relation_id = document_.root_relation_id + 1;
      sort.relation_kind = NativeRelationAstKind::kSort;
      sort.input_relation_ids = {document_.root_relation_id};
      sort.output_expression_ids =
          document_.relations.front().output_expression_ids;
      sort.ordering_terms = std::move(ordering_terms);
      sort.range = Span(select_token, *ordering_end);
      document_.relations.push_back(std::move(sort));
      document_.root_relation_id = document_.relations.back().relation_id;
    }
    if (!global_aggregate &&
        (hidden_predicate_expression_id.has_value() ||
         !hidden_order_expression_ids.empty())) {
      NativeRelationAstNode project;
      project.relation_id = document_.root_relation_id + 1;
      project.relation_kind = NativeRelationAstKind::kProject;
      project.input_relation_ids = {document_.root_relation_id};
      project.output_expression_ids = projection_expression_ids;
      project.range = Span(
          select_token, *query_end);
      document_.relations.push_back(std::move(project));
      document_.root_relation_id = document_.relations.back().relation_id;
    }
    if (global_aggregate) {
      NativeRelationAstNode aggregate;
      aggregate.relation_id = document_.root_relation_id + 1;
      aggregate.relation_kind = NativeRelationAstKind::kAggregate;
      aggregate.aggregate_grouping_form = NativeAggregateGroupingForm::kNone;
      aggregate.aggregate_projection_form =
          NativeAggregateProjectionForm::kGlobalUnary;
      aggregate.input_relation_ids = {document_.root_relation_id};
      aggregate.output_expression_ids = {*global_aggregate_expression_id};
      aggregate.aggregate_expression_ids = {*global_aggregate_expression_id};
      aggregate.range = Span(select_token, *query_end);
      document_.relations.push_back(std::move(aggregate));
      document_.root_relation_id = document_.relations.back().relation_id;
    }
    if (!limit_expression_ids.empty()) {
      NativeRelationAstNode limit;
      limit.relation_id = document_.root_relation_id + 1;
      limit.relation_kind = NativeRelationAstKind::kLimit;
      limit.input_relation_ids = {document_.root_relation_id};
      limit.output_expression_ids = projection_expression_ids;
      limit.limit_expression_ids = std::move(limit_expression_ids);
      limit.range = Span(select_token, *query_end);
      document_.relations.push_back(std::move(limit));
      document_.root_relation_id = document_.relations.back().relation_id;
    }
    document_.status = NativeRelationalParseStatus::kAccepted;
    return std::move(document_);
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
      // QOW-SOURCE-QRY-001-HAVING-COUNT-SUM-OR-GT-V1
      // QOW-SOURCE-QRY-001-TWO-KEY-HAVING-COUNT-SUM-OR-GT-V1
      // QOW-SOURCE-QRY-001-GROUPING-SETS-HAVING-COUNT-SUM-OR-GT-V1
      // QOW-SOURCE-QRY-001-ROLLUP-HAVING-COUNT-SUM-OR-GT-V1
      // QOW-SOURCE-QRY-001-CUBE-HAVING-COUNT-SUM-OR-GT-V1
      // QOW-SOURCE-QRY-001-TWO-KEY-HAVING-COUNT-SUM-AND-GT-V1
      // QOW-SOURCE-QRY-001-TWO-KEY-HAVING-SUM-GT-V1
      // QOW-SOURCE-QRY-001-GROUPING-SETS-HAVING-COUNT-SUM-AND-GT-V1
      // QOW-SOURCE-QRY-001-GROUPING-SETS-HAVING-SUM-GT-V1
      // QOW-SOURCE-QRY-001-GROUPING-SETS-GROUPING-METADATA-HAVING-V1
      // QOW-SOURCE-QRY-001-GROUPING-SETS-GROUPING-METADATA-HAVING-SUM-GT-V1
      // QOW-SOURCE-QRY-001-ROLLUP-HAVING-COUNT-SUM-AND-GT-V1
      // QOW-SOURCE-QRY-001-ROLLUP-HAVING-SUM-GT-V1
      // QOW-SOURCE-QRY-001-ROLLUP-HAVING-NOT-SUM-GT-V1
      // QOW-SOURCE-QRY-001-ROLLUP-GROUPING-METADATA-HAVING-NOT-SUM-GT-V1
      // QOW-SOURCE-QRY-001-ROLLUP-GROUPING-METADATA-HAVING-V1
      // QOW-SOURCE-QRY-001-ROLLUP-GROUPING-METADATA-HAVING-SUM-GT-V1
      // QOW-SOURCE-QRY-001-CUBE-HAVING-COUNT-SUM-AND-GT-V1
      // QOW-SOURCE-QRY-001-CUBE-HAVING-SUM-GT-V1
      // QOW-SOURCE-QRY-001-CUBE-HAVING-NOT-SUM-GT-V1
      // QOW-SOURCE-QRY-001-CUBE-GROUPING-METADATA-HAVING-NOT-SUM-GT-V1
      // QOW-SOURCE-QRY-001-CUBE-GROUPING-METADATA-HAVING-V1
      // QOW-SOURCE-QRY-001-CUBE-GROUPING-METADATA-HAVING-SUM-GT-V1
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
            if (threshold == nullptr ||
                threshold->expression_kind !=
                    NativeExpressionAstKind::kLiteral ||
                threshold->literal_kind != NativeLiteralAstKind::kNumeric) {
              return false;
            }
            std::int64_t value = 0;
            const auto [end, error] = std::from_chars(
                threshold->spelling.data(),
                threshold->spelling.data() + threshold->spelling.size(),
                value);
            return error == std::errc{} &&
                   end == threshold->spelling.data() +
                              threshold->spelling.size();
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
      const bool count_sum_or_profile =
          predicate.expression_kind == NativeExpressionAstKind::kBinary &&
          predicate.operator_name == "OR" &&
          predicate.child_expression_ids.size() == 2 &&
          count_conjunct != nullptr && sum_conjunct != nullptr &&
          match_count_comparison(*count_conjunct) &&
          match_sum_comparison(*sum_conjunct);
      const auto* not_operand =
          predicate.expression_kind == NativeExpressionAstKind::kUnary &&
                  predicate.operator_name == "NOT" &&
                  predicate.child_expression_ids.size() == 1
              ? expression_by_id(predicate.child_expression_ids.front())
              : nullptr;
      const auto* double_not_operand =
          not_operand != nullptr &&
                  not_operand->expression_kind ==
                      NativeExpressionAstKind::kUnary &&
                  not_operand->operator_name == "NOT" &&
                  not_operand->child_expression_ids.size() == 1
              ? expression_by_id(not_operand->child_expression_ids.front())
              : nullptr;
      const auto has_parenthesized_not_operand =
          [](const NativeExpressionAstNode* expression) {
            if (expression == nullptr || expression->spelling.size() < 4) {
              return false;
            }
            std::size_t offset = 3;
            while (offset < expression->spelling.size() &&
                   (expression->spelling[offset] == ' ' ||
                    expression->spelling[offset] == '\t' ||
                    expression->spelling[offset] == '\r' ||
                    expression->spelling[offset] == '\n')) {
              ++offset;
            }
            return offset < expression->spelling.size() &&
                   expression->spelling[offset] == '(';
          };
      // QOW-SOURCE-QRY-001-TWO-KEY-HAVING-NOT-NOT-SUM-GT-V1
      const bool not_not_sum_profile =
          double_not_operand != nullptr &&
          has_parenthesized_not_operand(&predicate) &&
          has_parenthesized_not_operand(not_operand) &&
          match_sum_comparison(*double_not_operand);
      // QOW-SOURCE-QRY-001-TWO-KEY-HAVING-NOT-NOT-COUNT-GT-V1
      const bool not_not_count_profile =
          double_not_operand != nullptr &&
          has_parenthesized_not_operand(&predicate) &&
          has_parenthesized_not_operand(not_operand) &&
          match_count_comparison(*double_not_operand);
      const auto* not_not_count_conjunct =
          double_not_operand != nullptr &&
                  double_not_operand->expression_kind ==
                      NativeExpressionAstKind::kBinary &&
                  (double_not_operand->operator_name == "AND" ||
                   double_not_operand->operator_name == "OR") &&
                  double_not_operand->child_expression_ids.size() == 2
              ? expression_by_id(double_not_operand->child_expression_ids[0])
              : nullptr;
      const auto* not_not_sum_conjunct =
          double_not_operand != nullptr &&
                  double_not_operand->expression_kind ==
                      NativeExpressionAstKind::kBinary &&
                  (double_not_operand->operator_name == "AND" ||
                   double_not_operand->operator_name == "OR") &&
                  double_not_operand->child_expression_ids.size() == 2
              ? expression_by_id(double_not_operand->child_expression_ids[1])
              : nullptr;
      // QOW-SOURCE-QRY-001-TWO-KEY-HAVING-NOT-NOT-COUNT-SUM-AND-GT-V1
      const bool not_not_count_sum_and_profile =
          double_not_operand != nullptr &&
          double_not_operand->operator_name == "AND" &&
          has_parenthesized_not_operand(&predicate) &&
          has_parenthesized_not_operand(not_operand) &&
          not_not_count_conjunct != nullptr &&
          not_not_sum_conjunct != nullptr &&
          match_count_comparison(*not_not_count_conjunct) &&
          match_sum_comparison(*not_not_sum_conjunct);
      // QOW-SOURCE-QRY-001-TWO-KEY-HAVING-NOT-NOT-COUNT-SUM-OR-GT-V1
      const bool not_not_count_sum_or_profile =
          double_not_operand != nullptr &&
          double_not_operand->operator_name == "OR" &&
          has_parenthesized_not_operand(&predicate) &&
          has_parenthesized_not_operand(not_operand) &&
          not_not_count_conjunct != nullptr &&
          not_not_sum_conjunct != nullptr &&
          match_count_comparison(*not_not_count_conjunct) &&
          match_sum_comparison(*not_not_sum_conjunct);
      // QOW-SOURCE-QRY-001-TWO-KEY-HAVING-NOT-NOT-SUM-COUNT-OR-GT-V1
      const bool not_not_sum_count_or_profile =
          double_not_operand != nullptr &&
          double_not_operand->operator_name == "OR" &&
          has_parenthesized_not_operand(&predicate) &&
          has_parenthesized_not_operand(not_operand) &&
          not_not_count_conjunct != nullptr &&
          not_not_sum_conjunct != nullptr &&
          match_sum_comparison(*not_not_count_conjunct) &&
          match_count_comparison(*not_not_sum_conjunct);
      // QOW-SOURCE-QRY-001-TWO-KEY-HAVING-NOT-SUM-GT-V1
      const bool not_sum_profile =
          not_operand != nullptr && match_sum_comparison(*not_operand);
      // QOW-SOURCE-QRY-001-TWO-KEY-HAVING-NOT-COUNT-GT-V1
      const bool not_count_profile =
          not_operand != nullptr && match_count_comparison(*not_operand);
      const auto* not_count_conjunct =
          not_operand != nullptr &&
                  not_operand->expression_kind ==
                      NativeExpressionAstKind::kBinary &&
                  (not_operand->operator_name == "AND" ||
                   not_operand->operator_name == "OR") &&
                  not_operand->child_expression_ids.size() == 2
              ? expression_by_id(not_operand->child_expression_ids[0])
              : nullptr;
      const auto* not_sum_conjunct =
          not_operand != nullptr &&
                  not_operand->expression_kind ==
                      NativeExpressionAstKind::kBinary &&
                  (not_operand->operator_name == "AND" ||
                   not_operand->operator_name == "OR") &&
                  not_operand->child_expression_ids.size() == 2
              ? expression_by_id(not_operand->child_expression_ids[1])
              : nullptr;
      // QOW-SOURCE-QRY-001-TWO-KEY-HAVING-NOT-COUNT-SUM-AND-GT-V1
      const bool not_count_sum_and_profile =
          not_operand != nullptr && not_operand->operator_name == "AND" &&
          not_count_conjunct != nullptr &&
          not_sum_conjunct != nullptr &&
          match_count_comparison(*not_count_conjunct) &&
          match_sum_comparison(*not_sum_conjunct);
      // QOW-SOURCE-QRY-001-TWO-KEY-HAVING-NOT-COUNT-SUM-OR-GT-V1
      const bool not_count_sum_or_profile =
          not_operand != nullptr && not_operand->operator_name == "OR" &&
          not_count_conjunct != nullptr && not_sum_conjunct != nullptr &&
          match_count_comparison(*not_count_conjunct) &&
          match_sum_comparison(*not_sum_conjunct);
      if (!simple_sum_profile && !count_sum_and_profile &&
          !count_sum_or_profile && !not_not_sum_profile &&
          !not_not_count_profile && !not_not_count_sum_and_profile &&
          !not_not_count_sum_or_profile && !not_not_sum_count_or_profile &&
          !not_sum_profile && !not_count_profile &&
          !not_count_sum_and_profile && !not_count_sum_or_profile) {
        Refuse("having_predicate_shape_invalid",
               "native HAVING profile requires an exact admitted aggregate "
               "comparison or Boolean wrapper");
        return FinishRefusal();
      }
      const bool ordinary_two_key_not_sum_profile =
          !one_key_grouping_profile && not_sum_profile &&
          grouping_form == NativeAggregateGroupingForm::kSimple &&
          projection_form == NativeAggregateProjectionForm::kKeysCountSum &&
          document_.grouping_sets.empty();
      const bool ordinary_two_key_not_not_sum_profile =
          !one_key_grouping_profile && not_not_sum_profile &&
          grouping_form == NativeAggregateGroupingForm::kSimple &&
          projection_form == NativeAggregateProjectionForm::kKeysCountSum &&
          document_.grouping_sets.empty();
      const bool ordinary_two_key_not_not_count_profile =
          !one_key_grouping_profile && not_not_count_profile &&
          grouping_form == NativeAggregateGroupingForm::kSimple &&
          projection_form == NativeAggregateProjectionForm::kKeysCountSum &&
          document_.grouping_sets.empty();
      const bool ordinary_two_key_not_not_count_sum_and_profile =
          !one_key_grouping_profile && not_not_count_sum_and_profile &&
          grouping_form == NativeAggregateGroupingForm::kSimple &&
          projection_form == NativeAggregateProjectionForm::kKeysCountSum &&
          document_.grouping_sets.empty();
      const bool ordinary_two_key_not_not_count_sum_or_profile =
          !one_key_grouping_profile && not_not_count_sum_or_profile &&
          grouping_form == NativeAggregateGroupingForm::kSimple &&
          projection_form == NativeAggregateProjectionForm::kKeysCountSum &&
          document_.grouping_sets.empty();
      const bool ordinary_two_key_not_not_sum_count_or_profile =
          !one_key_grouping_profile && not_not_sum_count_or_profile &&
          grouping_form == NativeAggregateGroupingForm::kSimple &&
          projection_form == NativeAggregateProjectionForm::kKeysCountSum &&
          document_.grouping_sets.empty();
      const bool ordinary_two_key_not_count_profile =
          !one_key_grouping_profile && not_count_profile &&
          grouping_form == NativeAggregateGroupingForm::kSimple &&
          projection_form == NativeAggregateProjectionForm::kKeysCountSum &&
          document_.grouping_sets.empty();
      const bool ordinary_two_key_not_count_sum_and_profile =
          !one_key_grouping_profile && not_count_sum_and_profile &&
          grouping_form == NativeAggregateGroupingForm::kSimple &&
          projection_form == NativeAggregateProjectionForm::kKeysCountSum &&
          document_.grouping_sets.empty();
      const bool ordinary_two_key_not_count_sum_or_profile =
          !one_key_grouping_profile && not_count_sum_or_profile &&
          grouping_form == NativeAggregateGroupingForm::kSimple &&
          projection_form == NativeAggregateProjectionForm::kKeysCountSum &&
          document_.grouping_sets.empty();
      // QOW-SOURCE-QRY-001-GROUPING-SETS-HAVING-NOT-COUNT-SUM-AND-GT-V1
      const bool grouping_sets_not_count_sum_and_profile =
          !one_key_grouping_profile && not_count_sum_and_profile &&
          grouping_form == NativeAggregateGroupingForm::kGroupingSets &&
          projection_form == NativeAggregateProjectionForm::kKeysCountSum &&
          key_a.has_value() && key_b.has_value() &&
          document_.grouping_sets.size() == 4 &&
          document_.grouping_sets[0].ordinal == 0 &&
          document_.grouping_sets[0].expression_ids ==
              std::vector<std::uint32_t>{*key_b} &&
          document_.grouping_sets[1].ordinal == 1 &&
          document_.grouping_sets[1].expression_ids.empty() &&
          document_.grouping_sets[2].ordinal == 2 &&
          document_.grouping_sets[2].expression_ids ==
              std::vector<std::uint32_t>{*key_b, *key_a} &&
          document_.grouping_sets[3].ordinal == 3 &&
          document_.grouping_sets[3].expression_ids ==
              document_.grouping_sets[0].expression_ids;
      // QOW-SOURCE-QRY-001-GROUPING-SETS-GROUPING-METADATA-HAVING-NOT-COUNT-SUM-AND-GT-V1
      const bool grouping_sets_metadata_not_count_sum_and_profile =
          !one_key_grouping_profile && not_count_sum_and_profile &&
          grouping_form == NativeAggregateGroupingForm::kGroupingSets &&
          projection_form ==
              NativeAggregateProjectionForm::kKeysCountSumGrouping &&
          key_a.has_value() && key_b.has_value() &&
          document_.grouping_sets.size() == 4 &&
          document_.grouping_sets[0].ordinal == 0 &&
          document_.grouping_sets[0].expression_ids ==
              std::vector<std::uint32_t>{*key_b} &&
          document_.grouping_sets[1].ordinal == 1 &&
          document_.grouping_sets[1].expression_ids.empty() &&
          document_.grouping_sets[2].ordinal == 2 &&
          document_.grouping_sets[2].expression_ids ==
              std::vector<std::uint32_t>{*key_b, *key_a} &&
          document_.grouping_sets[3].ordinal == 3 &&
          document_.grouping_sets[3].expression_ids ==
              document_.grouping_sets[0].expression_ids;
      // QOW-SOURCE-QRY-001-ROLLUP-HAVING-NOT-COUNT-SUM-AND-GT-V1
      const bool rollup_not_count_sum_and_profile =
          !one_key_grouping_profile && not_count_sum_and_profile &&
          grouping_form == NativeAggregateGroupingForm::kRollup &&
          projection_form == NativeAggregateProjectionForm::kKeysCountSum &&
          key_a.has_value() && key_b.has_value() &&
          document_.grouping_sets.empty();
      // QOW-SOURCE-QRY-001-ROLLUP-GROUPING-METADATA-HAVING-NOT-COUNT-SUM-AND-GT-V1
      const bool rollup_metadata_not_count_sum_and_profile =
          !one_key_grouping_profile && not_count_sum_and_profile &&
          grouping_form == NativeAggregateGroupingForm::kRollup &&
          projection_form ==
              NativeAggregateProjectionForm::kKeysCountSumGrouping &&
          key_a.has_value() && key_b.has_value() &&
          document_.grouping_sets.empty();
      // QOW-SOURCE-QRY-001-CUBE-HAVING-NOT-COUNT-SUM-AND-GT-V1
      const bool cube_not_count_sum_and_profile =
          !one_key_grouping_profile && not_count_sum_and_profile &&
          grouping_form == NativeAggregateGroupingForm::kCube &&
          projection_form == NativeAggregateProjectionForm::kKeysCountSum &&
          key_a.has_value() && key_b.has_value() &&
          document_.grouping_sets.empty();
      // QOW-SOURCE-QRY-001-CUBE-GROUPING-METADATA-HAVING-NOT-COUNT-SUM-AND-GT-V1
      const bool cube_metadata_not_count_sum_and_profile =
          !one_key_grouping_profile && not_count_sum_and_profile &&
          grouping_form == NativeAggregateGroupingForm::kCube &&
          projection_form ==
              NativeAggregateProjectionForm::kKeysCountSumGrouping &&
          key_a.has_value() && key_b.has_value() &&
          document_.grouping_sets.empty();
      // QOW-SOURCE-QRY-001-GROUPING-SETS-HAVING-NOT-SUM-GT-V1
      // QOW-SOURCE-QRY-001-GROUPING-SETS-GROUPING-METADATA-HAVING-NOT-SUM-GT-V1
      const bool grouping_sets_not_sum_profile =
          !one_key_grouping_profile && not_sum_profile &&
          grouping_form == NativeAggregateGroupingForm::kGroupingSets &&
          projection_form == NativeAggregateProjectionForm::kKeysCountSum &&
          key_a.has_value() && key_b.has_value() &&
          document_.grouping_sets.size() == 4 &&
          document_.grouping_sets[0].ordinal == 0 &&
          document_.grouping_sets[0].expression_ids ==
              std::vector<std::uint32_t>{*key_b} &&
          document_.grouping_sets[1].ordinal == 1 &&
          document_.grouping_sets[1].expression_ids.empty() &&
          document_.grouping_sets[2].ordinal == 2 &&
          document_.grouping_sets[2].expression_ids ==
              std::vector<std::uint32_t>{*key_b, *key_a} &&
          document_.grouping_sets[3].ordinal == 3 &&
          document_.grouping_sets[3].expression_ids ==
              document_.grouping_sets[0].expression_ids;
      const bool grouping_sets_metadata_not_sum_profile =
          !one_key_grouping_profile && not_sum_profile &&
          grouping_form == NativeAggregateGroupingForm::kGroupingSets &&
          projection_form ==
              NativeAggregateProjectionForm::kKeysCountSumGrouping &&
          key_a.has_value() && key_b.has_value() &&
          document_.grouping_sets.size() == 4 &&
          document_.grouping_sets[0].ordinal == 0 &&
          document_.grouping_sets[0].expression_ids ==
              std::vector<std::uint32_t>{*key_b} &&
          document_.grouping_sets[1].ordinal == 1 &&
          document_.grouping_sets[1].expression_ids.empty() &&
          document_.grouping_sets[2].ordinal == 2 &&
          document_.grouping_sets[2].expression_ids ==
              std::vector<std::uint32_t>{*key_b, *key_a} &&
          document_.grouping_sets[3].ordinal == 3 &&
          document_.grouping_sets[3].expression_ids ==
              document_.grouping_sets[0].expression_ids;
      // QOW-SOURCE-QRY-001-ROLLUP-HAVING-NOT-SUM-GT-V1
      const bool rollup_not_sum_profile =
          !one_key_grouping_profile && not_sum_profile &&
          grouping_form == NativeAggregateGroupingForm::kRollup &&
          projection_form == NativeAggregateProjectionForm::kKeysCountSum &&
          key_a.has_value() && key_b.has_value() &&
          document_.grouping_sets.empty();
      // QOW-SOURCE-QRY-001-ROLLUP-GROUPING-METADATA-HAVING-NOT-SUM-GT-V1
      const bool rollup_metadata_not_sum_profile =
          !one_key_grouping_profile && not_sum_profile &&
          grouping_form == NativeAggregateGroupingForm::kRollup &&
          projection_form ==
              NativeAggregateProjectionForm::kKeysCountSumGrouping &&
          key_a.has_value() && key_b.has_value() &&
          document_.grouping_sets.empty();
      // QOW-SOURCE-QRY-001-CUBE-HAVING-NOT-SUM-GT-V1
      const bool cube_not_sum_profile =
          !one_key_grouping_profile && not_sum_profile &&
          grouping_form == NativeAggregateGroupingForm::kCube &&
          projection_form == NativeAggregateProjectionForm::kKeysCountSum &&
          key_a.has_value() && key_b.has_value() &&
          document_.grouping_sets.empty();
      // QOW-SOURCE-QRY-001-CUBE-GROUPING-METADATA-HAVING-NOT-SUM-GT-V1
      const bool cube_metadata_not_sum_profile =
          !one_key_grouping_profile && not_sum_profile &&
          grouping_form == NativeAggregateGroupingForm::kCube &&
          projection_form ==
              NativeAggregateProjectionForm::kKeysCountSumGrouping &&
          key_a.has_value() && key_b.has_value() &&
          document_.grouping_sets.empty();
      if (one_key_grouping_profile &&
          (not_not_sum_profile || not_not_count_profile ||
           not_not_count_sum_and_profile || not_not_count_sum_or_profile ||
           not_not_sum_count_or_profile ||
           not_sum_profile ||
           not_count_profile || not_count_sum_and_profile ||
           not_count_sum_or_profile)) {
        Refuse("having_profile_not_admitted",
               "native NOT aggregate HAVING requires an exact admitted "
               "two-key grouping profile");
        return FinishRefusal();
      }
      const bool ordinary_two_key_sum_profile =
          !one_key_grouping_profile && simple_sum_profile &&
          grouping_form == NativeAggregateGroupingForm::kSimple &&
          projection_form == NativeAggregateProjectionForm::kKeysCountSum;
      const bool ordinary_two_key_or_profile =
          !one_key_grouping_profile && count_sum_or_profile &&
          grouping_form == NativeAggregateGroupingForm::kSimple &&
          projection_form == NativeAggregateProjectionForm::kKeysCountSum;
      const bool grouping_sets_or_profile =
          !one_key_grouping_profile && count_sum_or_profile &&
          grouping_form == NativeAggregateGroupingForm::kGroupingSets &&
          projection_form == NativeAggregateProjectionForm::kKeysCountSum &&
          key_a.has_value() && key_b.has_value() &&
          document_.grouping_sets.size() == 4 &&
          document_.grouping_sets[0].ordinal == 0 &&
          document_.grouping_sets[0].expression_ids ==
              std::vector<std::uint32_t>{*key_b} &&
          document_.grouping_sets[1].ordinal == 1 &&
          document_.grouping_sets[1].expression_ids.empty() &&
          document_.grouping_sets[2].ordinal == 2 &&
          document_.grouping_sets[2].expression_ids ==
              std::vector<std::uint32_t>{*key_b, *key_a} &&
          document_.grouping_sets[3].ordinal == 3 &&
          document_.grouping_sets[3].expression_ids ==
              document_.grouping_sets[0].expression_ids;
      // QOW-SOURCE-QRY-001-GROUPING-SETS-GROUPING-METADATA-HAVING-COUNT-SUM-OR-GT-V1
      const bool grouping_sets_metadata_or_profile =
          !one_key_grouping_profile && count_sum_or_profile &&
          grouping_form == NativeAggregateGroupingForm::kGroupingSets &&
          projection_form ==
              NativeAggregateProjectionForm::kKeysCountSumGrouping &&
          key_a.has_value() && key_b.has_value() &&
          document_.grouping_sets.size() == 4 &&
          document_.grouping_sets[0].ordinal == 0 &&
          document_.grouping_sets[0].expression_ids ==
              std::vector<std::uint32_t>{*key_b} &&
          document_.grouping_sets[1].ordinal == 1 &&
          document_.grouping_sets[1].expression_ids.empty() &&
          document_.grouping_sets[2].ordinal == 2 &&
          document_.grouping_sets[2].expression_ids ==
              std::vector<std::uint32_t>{*key_b, *key_a} &&
          document_.grouping_sets[3].ordinal == 3 &&
          document_.grouping_sets[3].expression_ids ==
              document_.grouping_sets[0].expression_ids;
      const bool rollup_or_profile =
          !one_key_grouping_profile && count_sum_or_profile &&
          grouping_form == NativeAggregateGroupingForm::kRollup &&
          projection_form == NativeAggregateProjectionForm::kKeysCountSum &&
          document_.grouping_sets.empty();
      // QOW-SOURCE-QRY-001-ROLLUP-GROUPING-METADATA-HAVING-COUNT-SUM-OR-GT-V1
      const bool rollup_metadata_or_profile =
          !one_key_grouping_profile && count_sum_or_profile &&
          grouping_form == NativeAggregateGroupingForm::kRollup &&
          projection_form ==
              NativeAggregateProjectionForm::kKeysCountSumGrouping &&
          document_.grouping_sets.empty();
      const bool cube_or_profile =
          !one_key_grouping_profile && count_sum_or_profile &&
          grouping_form == NativeAggregateGroupingForm::kCube &&
          projection_form == NativeAggregateProjectionForm::kKeysCountSum &&
          document_.grouping_sets.empty();
      // QOW-SOURCE-QRY-001-CUBE-GROUPING-METADATA-HAVING-COUNT-SUM-OR-GT-V1
      const bool cube_metadata_or_profile =
          !one_key_grouping_profile && count_sum_or_profile &&
          grouping_form == NativeAggregateGroupingForm::kCube &&
          projection_form ==
              NativeAggregateProjectionForm::kKeysCountSumGrouping &&
          document_.grouping_sets.empty();
      const bool grouping_sets_sum_profile =
          !one_key_grouping_profile && simple_sum_profile &&
          grouping_form == NativeAggregateGroupingForm::kGroupingSets &&
          projection_form == NativeAggregateProjectionForm::kKeysCountSum;
      const bool grouping_sets_metadata_sum_profile =
          !one_key_grouping_profile && simple_sum_profile &&
          grouping_form == NativeAggregateGroupingForm::kGroupingSets &&
          projection_form ==
              NativeAggregateProjectionForm::kKeysCountSumGrouping;
      const bool rollup_sum_profile =
          !one_key_grouping_profile && simple_sum_profile &&
          grouping_form == NativeAggregateGroupingForm::kRollup &&
          projection_form == NativeAggregateProjectionForm::kKeysCountSum;
      const bool rollup_metadata_sum_profile =
          !one_key_grouping_profile && simple_sum_profile &&
          grouping_form == NativeAggregateGroupingForm::kRollup &&
          projection_form ==
              NativeAggregateProjectionForm::kKeysCountSumGrouping;
      const bool cube_sum_profile =
          !one_key_grouping_profile && simple_sum_profile &&
          grouping_form == NativeAggregateGroupingForm::kCube &&
          projection_form == NativeAggregateProjectionForm::kKeysCountSum;
      const bool cube_metadata_sum_profile =
          !one_key_grouping_profile && simple_sum_profile &&
          grouping_form == NativeAggregateGroupingForm::kCube &&
          projection_form ==
              NativeAggregateProjectionForm::kKeysCountSumGrouping;
      if (!one_key_grouping_profile && !count_sum_and_profile &&
          !ordinary_two_key_not_not_sum_profile &&
          !ordinary_two_key_not_not_count_profile &&
          !ordinary_two_key_not_not_count_sum_and_profile &&
          !ordinary_two_key_not_not_count_sum_or_profile &&
          !ordinary_two_key_not_not_sum_count_or_profile &&
          !ordinary_two_key_not_sum_profile &&
          !ordinary_two_key_not_count_profile &&
          !ordinary_two_key_not_count_sum_and_profile &&
          !ordinary_two_key_not_count_sum_or_profile &&
          !grouping_sets_not_count_sum_and_profile &&
          !grouping_sets_metadata_not_count_sum_and_profile &&
          !rollup_not_count_sum_and_profile &&
          !rollup_metadata_not_count_sum_and_profile &&
          !cube_not_count_sum_and_profile &&
          !cube_metadata_not_count_sum_and_profile &&
          !grouping_sets_not_sum_profile &&
          !grouping_sets_metadata_not_sum_profile &&
          !rollup_not_sum_profile &&
          !rollup_metadata_not_sum_profile &&
          !cube_not_sum_profile &&
          !cube_metadata_not_sum_profile &&
          !ordinary_two_key_or_profile && !grouping_sets_or_profile &&
          !grouping_sets_metadata_or_profile &&
          !rollup_or_profile && !rollup_metadata_or_profile &&
          !cube_or_profile && !cube_metadata_or_profile &&
          !ordinary_two_key_sum_profile &&
          !grouping_sets_sum_profile &&
          !grouping_sets_metadata_sum_profile && !rollup_sum_profile &&
          !rollup_metadata_sum_profile && !cube_sum_profile &&
          !cube_metadata_sum_profile) {
        Refuse("having_profile_not_admitted",
               "native multi-key HAVING profile requires an exact admitted "
               "SUM comparison or Boolean predicate");
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
    document_.catalog_relation_sources.clear();
    document_.values_rows.clear();
    document_.grouping_sets.clear();
    document_.window_definitions.clear();
    document_.window_invocations.clear();
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
    auto canonical_child_id = *child_id;
    const auto child_range = child.range;
    if (allow_count_star_expression_ && word == "NOT" &&
        child.expression_kind == NativeExpressionAstKind::kParenthesized &&
        child.child_expression_ids.size() == 1 &&
        child.expression_id == document_.expressions.size()) {
      canonical_child_id = child.child_expression_ids.front();
      document_.expressions.pop_back();
    }
    NativeExpressionAstNode unary;
    unary.expression_id = NextExpressionId();
    unary.expression_kind = NativeExpressionAstKind::kUnary;
    unary.child_expression_ids = {canonical_child_id};
    unary.operator_name = word;
    unary.range = RangeFromTokenAndRange(first, child_range);
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
    std::vector<NativeIdentifierAstNode> qualified_identifier{
        {first.text, first.quoted, TokenSourceRange(first)}};
    while (AtSymbol(".")) {
      Consume();
      if (AtEnd() || (Current().kind != TokenKind::kIdentifier &&
                      Current().kind != TokenKind::kKeyword)) {
        Refuse("qualified_name_incomplete", "qualified expression name is incomplete");
        return std::nullopt;
      }
      last_name_token = &Consume();
      qualified_identifier.push_back(
          {last_name_token->text, last_name_token->quoted,
           TokenSourceRange(*last_name_token)});
    }

    if (!AtSymbol("(")) {
      NativeExpressionAstNode identifier;
      identifier.expression_id = NextExpressionId();
      identifier.expression_kind = NativeExpressionAstKind::kIdentifier;
      identifier.range = Span(first, *last_name_token);
      identifier.qualified_identifier = std::move(qualified_identifier);
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

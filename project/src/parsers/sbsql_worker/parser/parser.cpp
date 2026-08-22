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
#include <tuple>
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
        contains_word("INFLUX_LINE_PROTOCOL") ||
        contains_word("VECTOR_PROVIDER_REQUEST") ||
        contains_word("OPENSEARCH_DSL") ||
        contains_word("COLUMNAR_ENGINE_HINT")) {
      RefuseExact("SB_MODEL_GRAMMAR_DONOR_TEXT_REFUSED_V1",
                  "opaque donor text is not an executable SBSQL model source");
      return FinishRefusal();
    }
    const std::size_t model_source_token_count = std::ranges::count_if(
        tokens_, [&](const auto* token) {
          return IsWord(*token, "DOCUMENT_SOURCE") ||
                 IsWord(*token, "GRAPH_SOURCE") ||
                 IsWord(*token, "KEY_VALUE_SOURCE") ||
                 IsWord(*token, "TIME_SERIES_SOURCE") ||
                 IsWord(*token, "VECTOR_SOURCE") ||
                 IsWord(*token, "SEARCH_SOURCE") ||
                 IsWord(*token, "SPATIAL_SOURCE") ||
                 IsWord(*token, "COLUMNAR_SOURCE");
        });
    if (model_source_token_count != 0 &&
        LooksLikeBoundedCatalogJoinSelect()) {
      if (tokens_.empty() || !IsWord(*tokens_.front(), "SELECT") ||
          !LooksLikeBoundedCatalogJoinSelect()) {
        RefuseExact("SB_MODEL_OPERATION_SEMANTIC_REFUSED_V1",
                    "mixed model sources require a bounded SELECT JOIN");
        return FinishRefusal();
      }
      return ParseCatalogJoinSelect();
    }
    if (contains_word("SPATIAL_SOURCE") || contains_word("SPATIAL_MATCH") ||
        contains_word("SPATIAL_NEAREST")) {
      if (tokens_.empty() || !IsWord(*tokens_.front(), "SELECT")) {
        RefuseExact("SB_MODEL_QUERY_WRITE_REFUSED_V1",
                    "spatial model sources are read-only query inputs");
        return FinishRefusal();
      }
      return ParseSpatialModelSelect();
    }
    if (contains_word("COLUMNAR_SOURCE") ||
        contains_word("COLUMNAR_PROJECT") ||
        contains_word("COLUMNAR_FILTER") ||
        contains_word("COLUMNAR_ENGINE_HINT")) {
      if (tokens_.empty() || !IsWord(*tokens_.front(), "SELECT")) {
        RefuseExact("SB_MODEL_QUERY_WRITE_REFUSED_V1",
                    "columnar model sources are read-only query inputs");
        return FinishRefusal();
      }
      return ParseColumnarModelSelect();
    }
    if (contains_word("SEARCH_SOURCE") || contains_word("SEARCH_MATCH") ||
        contains_word("SEARCH_TERMS") || contains_word("SEARCH_PHRASE") ||
        contains_word("SEARCH_FUZZY") || contains_word("SEARCH_FILTER") ||
        contains_word("SEARCH_INSERT") || contains_word("SEARCH_UPSERT")) {
      if (tokens_.empty() || !IsWord(*tokens_.front(), "SELECT") ||
          contains_word("SEARCH_INSERT") || contains_word("SEARCH_UPSERT")) {
        RefuseExact("SB_MODEL_QUERY_WRITE_REFUSED_V1",
                    "search model sources are read-only query inputs");
        return FinishRefusal();
      }
      return ParseSearchModelSelect();
    }
    if (contains_word("VECTOR_SOURCE") || contains_word("VECTOR_NEAREST") ||
        contains_word("VECTOR_FILTER") || contains_word("VECTOR_INSERT") ||
        contains_word("VECTOR_UPSERT")) {
      if (tokens_.empty() || !IsWord(*tokens_.front(), "SELECT") ||
          contains_word("VECTOR_INSERT") || contains_word("VECTOR_UPSERT")) {
        RefuseExact("SB_MODEL_QUERY_WRITE_REFUSED_V1",
                    "vector model sources are read-only query inputs");
        return FinishRefusal();
      }
      return ParseVectorModelSelect();
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

  NativeRelationalAstDocument ParseSpatialModelSelect() {
    // QOW-SOURCE-RCP079-SPATIAL-GRAMMAR-V1
    document_.status = NativeRelationalParseStatus::kRefused;
    if (cst_.messages.has_errors()) {
      document_.messages = cst_.messages;
      return FinishRefusal();
    }
    if (tokens_.size() > kMaximumNativeRelationalTokens) {
      Refuse("token_limit_exceeded", "spatial query token limit exceeded");
      return FinishRefusal();
    }
    Consume();  // SELECT
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
    if (!RequireWord("FROM", "spatial_from_required",
                     "spatial source requires FROM") ||
        AtEnd() || !IsWord(Current(), "SPATIAL_SOURCE")) {
      RefuseExact("SB_MODEL_OPERATION_SEMANTIC_REFUSED_V1",
                  "FROM requires exactly one SPATIAL_SOURCE");
      return FinishRefusal();
    }
    const Token& source_operator = Consume();
    if (!RequireSymbol("(", "spatial_source_open_required",
                       "SPATIAL_SOURCE requires an opening parenthesis") ||
        AtEnd() || !IsNameToken(Current())) {
      RefuseExact("SB_MODEL_BINDING_INCOMPLETE_V1",
                  "SPATIAL_SOURCE requires a qualified collection name");
      return FinishRefusal();
    }
    NativeCatalogRelationSourceAstNode source;
    source.source_id = 1;
    source.source_kind = NativeRelationSourceAstKind::kSpatial;
    source.model_family_id = "spatial";
    const Token& first_name = Consume();
    const Token* last_name = &first_name;
    source.qualified_name.push_back(
        {first_name.text, first_name.quoted, TokenSourceRange(first_name)});
    while (AtSymbol(".")) {
      Consume();
      if (AtEnd() || !IsNameToken(Current())) {
        RefuseExact("SB_MODEL_BINDING_INCOMPLETE_V1",
                    "qualified spatial collection name is incomplete");
        return FinishRefusal();
      }
      last_name = &Consume();
      source.qualified_name.push_back(
          {last_name->text, last_name->quoted, TokenSourceRange(*last_name)});
    }
    source.qualified_name_range = Span(first_name, *last_name);
    if (!RequireSymbol(")", "spatial_source_close_required",
                       "SPATIAL_SOURCE requires a closing parenthesis")) {
      return FinishRefusal();
    }
    const Token& source_close = Previous();
    const Token* source_end = &source_close;
    if (!AtEnd() && IsWord(Current(), "AS")) {
      Consume();
      if (AtEnd() || !IsNameToken(Current())) {
        RefuseExact("SB_MODEL_BINDING_INCOMPLETE_V1",
                    "SPATIAL_SOURCE AS requires an alias");
        return FinishRefusal();
      }
      const Token& alias = Consume();
      source.alias = NativeIdentifierAstNode{
          alias.text, alias.quoted, TokenSourceRange(alias)};
      source.alias_is_explicit = true;
      source_end = &alias;
    } else {
      source.alias = NativeIdentifierAstNode{
          last_name->text, last_name->quoted, TokenSourceRange(*last_name)};
    }

    NativeExpressionAstNode source_root;
    source_root.expression_id = NextExpressionId();
    source_root.expression_kind = NativeExpressionAstKind::kFunctionCall;
    source_root.operator_name = "SPATIAL_SOURCE";
    source_root.spelling = SourceSpelling(source_operator, source_close);
    source_root.range = Span(source_operator, source_close);
    source.model_operation_ids.push_back(source_root.operator_name);
    source.model_operation_expression_ids.push_back(source_root.expression_id);
    document_.expressions.push_back(std::move(source_root));

    const auto expression_at = [&](const std::uint32_t id)
        -> NativeExpressionAstNode* {
      return id == 0 || id > document_.expressions.size()
                 ? nullptr
                 : &document_.expressions[id - 1];
    };
    const auto same_alias = [&](const NativeExpressionAstNode* expression,
                                const NativeIdentifierAstNode& expected) {
      if (expression == nullptr ||
          expression->expression_kind != NativeExpressionAstKind::kIdentifier ||
          expression->qualified_identifier.size() != 1) {
        return false;
      }
      const auto& presented = expression->qualified_identifier.front();
      return presented.quoted == expected.quoted &&
             (presented.quoted
                  ? presented.spelling == expected.spelling
                  : ToLowerAscii(presented.spelling) ==
                        ToLowerAscii(expected.spelling));
    };
    const auto valid_geometry = [&](const NativeExpressionAstNode* expression) {
      return expression != nullptr &&
             (expression->expression_kind ==
                  NativeExpressionAstKind::kParameter ||
              (expression->expression_kind ==
                   NativeExpressionAstKind::kFunctionCall &&
               ToUpperAscii(expression->operator_name) == "POINT"));
    };
    const auto valid_crs = [&](const NativeExpressionAstNode* expression) {
      return expression != nullptr &&
             expression->expression_kind ==
                 NativeExpressionAstKind::kIdentifier &&
             expression->qualified_identifier.size() >= 2;
    };
    const auto parse_top_k = [&](const NativeExpressionAstNode* expression,
                                 std::uint64_t* value) {
      if (expression == nullptr ||
          expression->expression_kind != NativeExpressionAstKind::kLiteral ||
          expression->literal_kind != NativeLiteralAstKind::kNumeric) {
        return false;
      }
      const auto parsed = std::from_chars(
          expression->spelling.data(),
          expression->spelling.data() + expression->spelling.size(), *value);
      return !expression->spelling.empty() &&
             !(expression->spelling.size() > 1 &&
               expression->spelling.front() == '0') &&
             parsed.ec == std::errc{} &&
             parsed.ptr == expression->spelling.data() +
                               expression->spelling.size() &&
             *value >= 1 && *value <= 4096;
    };

    const NativeIdentifierAstNode source_alias = *source.alias;
    if (AtSymbol(",")) {
      if (!source.alias_is_explicit) {
        RefuseExact("SB_MODEL_BINDING_INCOMPLETE_V1",
                    "SPATIAL_NEAREST requires an explicit source alias");
        return FinishRefusal();
      }
      Consume();
      if (AtEnd() || !IsWord(Current(), "SPATIAL_NEAREST")) {
        RefuseExact("SB_MODEL_OPERATION_SEMANTIC_REFUSED_V1",
                    "spatial source attachment must be SPATIAL_NEAREST");
        return FinishRefusal();
      }
      const auto nearest_id = ParseExpression(3, 0);
      if (!nearest_id.has_value()) return FinishRefusal();
      auto* nearest = expression_at(*nearest_id);
      if (nearest == nullptr ||
          nearest->expression_kind != NativeExpressionAstKind::kFunctionCall ||
          ToUpperAscii(nearest->operator_name) != "SPATIAL_NEAREST" ||
          nearest->child_expression_ids.size() != 4 ||
          !same_alias(expression_at(nearest->child_expression_ids[0]),
                      source_alias) ||
          !valid_geometry(expression_at(nearest->child_expression_ids[1])) ||
          !valid_crs(expression_at(nearest->child_expression_ids[2]))) {
        RefuseExact("SB_MODEL_SPATIAL_PROFILE_UNSUPPORTED_V1",
                    "SPATIAL_NEAREST requires source alias, typed geometry, qualified CRS, and top-k");
        return FinishRefusal();
      }
      std::uint64_t top_k = 0;
      if (!parse_top_k(expression_at(nearest->child_expression_ids[3]),
                       &top_k)) {
        RefuseExact("SB_MODEL_SPATIAL_TOP_K_REFUSED_V1",
                    "SPATIAL_NEAREST top-k is outside 1..4096");
        return FinishRefusal();
      }
      nearest->operator_name = "SPATIAL_NEAREST";
      source.model_source_alias = source.alias;
      source.model_spatial_alias_expression_id =
          nearest->child_expression_ids[0];
      source.model_spatial_nearest_expression_id = nearest->expression_id;
      source.model_spatial_query_expression_ids.push_back(
          nearest->child_expression_ids[1]);
      source.model_spatial_crs_expression_ids.push_back(
          nearest->child_expression_ids[2]);
      source.model_spatial_crs_names.push_back(
          expression_at(nearest->child_expression_ids[2])
              ->qualified_identifier);
      source.model_spatial_top_k_expression_id =
          nearest->child_expression_ids[3];
      source.model_spatial_top_k = top_k;
      source_end = &Previous();
      if (!AtEnd() && IsWord(Current(), "AS")) {
        Consume();
        if (AtEnd() || !IsNameToken(Current())) {
          RefuseExact("SB_MODEL_BINDING_INCOMPLETE_V1",
                      "SPATIAL_NEAREST AS requires a result alias");
          return FinishRefusal();
        }
        const Token& result_alias = Consume();
        source.alias = NativeIdentifierAstNode{
            result_alias.text, result_alias.quoted,
            TokenSourceRange(result_alias)};
        source.alias_is_explicit = true;
        source_end = &result_alias;
      }
    }

    std::vector<std::uint32_t> predicate_expression_ids;
    if (!AtEnd() && IsWord(Current(), "WHERE")) {
      Consume();
      const auto match_id = ParseExpression(3, 0);
      if (!match_id.has_value()) return FinishRefusal();
      auto* match = expression_at(*match_id);
      if (match == nullptr ||
          match->expression_kind != NativeExpressionAstKind::kFunctionCall ||
          ToUpperAscii(match->operator_name) != "SPATIAL_MATCH") {
        RefuseExact("SB_MODEL_OPERATION_SEMANTIC_REFUSED_V1",
                    "spatial WHERE requires a positive top-level SPATIAL_MATCH conjunct");
        return FinishRefusal();
      }
      if (match->child_expression_ids.size() != 4) {
        RefuseExact("SB_MODEL_OPERATION_SEMANTIC_REFUSED_V1",
                    "SPATIAL_MATCH requires exactly four operands");
        return FinishRefusal();
      }
      auto* alias = expression_at(match->child_expression_ids[0]);
      auto* predicate = expression_at(match->child_expression_ids[1]);
      auto* query = expression_at(match->child_expression_ids[2]);
      auto* crs = expression_at(match->child_expression_ids[3]);
      if (!same_alias(alias, source_alias) || !valid_geometry(query) ||
          !valid_crs(crs)) {
        RefuseExact(crs != nullptr &&
                            crs->expression_kind ==
                                NativeExpressionAstKind::kIdentifier &&
                            crs->qualified_identifier.size() < 2
                        ? "SB_MODEL_SPATIAL_CRS_BINDING_REQUIRED_V1"
                        : "SB_MODEL_SPATIAL_PROFILE_UNSUPPORTED_V1",
                    "spatial operands are not alias, POINT/parameter, and qualified bound CRS");
        return FinishRefusal();
      }
      source.model_spatial_alias_expression_id = alias->expression_id;
      source.model_spatial_match_expression_id = match->expression_id;
      source.model_spatial_query_expression_ids.insert(
          source.model_spatial_query_expression_ids.begin(),
          query->expression_id);
      source.model_spatial_crs_expression_ids.insert(
          source.model_spatial_crs_expression_ids.begin(), crs->expression_id);
      source.model_spatial_crs_names.insert(
          source.model_spatial_crs_names.begin(), crs->qualified_identifier);
      match->operator_name = "SPATIAL_MATCH";
      if (predicate == nullptr ||
          predicate->expression_kind != NativeExpressionAstKind::kIdentifier ||
          predicate->qualified_identifier.size() != 1 ||
          predicate->qualified_identifier.front().quoted) {
        RefuseExact("SB_MODEL_OPERATION_SEMANTIC_REFUSED_V1",
                    "SPATIAL_MATCH predicate identity is not canonical");
        return FinishRefusal();
      }
      const auto predicate_id =
          ToUpperAscii(predicate->qualified_identifier.front().spelling);
      if (predicate_id != "INTERSECTS" && predicate_id != "CONTAINS") {
        RefuseExact("SB_MODEL_OPERATION_SEMANTIC_REFUSED_V1",
                    "spatial predicate is outside the closed point set");
        return FinishRefusal();
      }
      predicate->expression_kind = NativeExpressionAstKind::kLiteral;
      predicate->literal_kind = NativeLiteralAstKind::kString;
      predicate->qualified_identifier.clear();
      predicate->spelling = predicate_id;
      source.model_spatial_predicate_expression_id = predicate->expression_id;
      source.model_spatial_predicate_id = predicate_id;
      predicate_expression_ids.push_back(*match_id);
      if (!AtEnd() && IsWord(Current(), "AND")) {
        Consume();
        const auto residual_id = ParseExpression(0, 0);
        if (!residual_id.has_value()) return FinishRefusal();
        predicate_expression_ids.push_back(*residual_id);
      }
    }
    if (source.model_spatial_match_expression_id.has_value()) {
      source.model_operation_ids.insert(source.model_operation_ids.begin() + 1,
                                        "SPATIAL_MATCH");
      source.model_operation_expression_ids.insert(
          source.model_operation_expression_ids.begin() + 1,
          *source.model_spatial_match_expression_id);
    }
    if (source.model_spatial_nearest_expression_id.has_value()) {
      source.model_operation_ids.push_back("SPATIAL_NEAREST");
      source.model_operation_expression_ids.push_back(
          *source.model_spatial_nearest_expression_id);
    }
    source.model_operation_id = source.model_operation_ids.size() == 1
                                    ? source.model_operation_ids.front()
                                    : std::string{};
    source.model_spatial_operation_expression_id =
        source.model_operation_expression_ids.size() == 2
            ? std::optional<std::uint32_t>{
                  source.model_operation_expression_ids.back()}
            : std::nullopt;
    if (source.model_spatial_query_expression_ids.size() == 1) {
      source.model_spatial_query_expression_id =
          source.model_spatial_query_expression_ids.front();
      source.model_spatial_crs_expression_id =
          source.model_spatial_crs_expression_ids.front();
      source.model_spatial_crs_name = source.model_spatial_crs_names.front();
    }
    const auto spatial_match_count = std::ranges::count_if(
        document_.expressions, [](const auto& expression) {
          return expression.expression_kind ==
                     NativeExpressionAstKind::kFunctionCall &&
                 ToUpperAscii(expression.operator_name) == "SPATIAL_MATCH";
        });
    const auto spatial_nearest_count = std::ranges::count_if(
        document_.expressions, [](const auto& expression) {
          return expression.expression_kind ==
                     NativeExpressionAstKind::kFunctionCall &&
                 ToUpperAscii(expression.operator_name) == "SPATIAL_NEAREST";
        });
    if (spatial_match_count !=
            static_cast<std::ptrdiff_t>(
                source.model_spatial_match_expression_id.has_value()) ||
        spatial_nearest_count !=
            static_cast<std::ptrdiff_t>(
                source.model_spatial_nearest_expression_id.has_value())) {
      RefuseExact("SB_MODEL_OPERATION_SEMANTIC_REFUSED_V1",
                  "spatial model operations must appear exactly once in their admitted source positions");
      return FinishRefusal();
    }
    if (AtSymbol(";")) Consume();
    if (!AtEnd()) {
      RefuseExact("SB_MODEL_OPERATION_SEMANTIC_REFUSED_V1",
                  "unexpected input follows the bounded spatial query");
      return FinishRefusal();
    }
    NativeRelationAstNode relation;
    relation.relation_id = 1;
    relation.relation_kind = NativeRelationAstKind::kCatalogSource;
    relation.relation_source_ids = {source.source_id};
    relation.output_expression_ids = std::move(projection_expression_ids);
    relation.predicate_expression_ids = std::move(predicate_expression_ids);
    relation.range = Span(source_operator, *source_end);
    source.range = relation.range;
    document_.catalog_relation_sources.push_back(std::move(source));
    document_.relations.push_back(std::move(relation));
    document_.root_relation_id = 1;
    document_.status = NativeRelationalParseStatus::kAccepted;
    return std::move(document_);
  }

  NativeRelationalAstDocument ParseColumnarModelSelect() {
    // QOW-SOURCE-RCP079-COLUMNAR-GRAMMAR-V1
    document_.status = NativeRelationalParseStatus::kRefused;
    if (cst_.messages.has_errors()) {
      document_.messages = cst_.messages;
      return FinishRefusal();
    }
    Consume();  // SELECT
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
    if (!RequireWord("FROM", "columnar_from_required",
                     "columnar source requires FROM") ||
        AtEnd() || !IsWord(Current(), "COLUMNAR_SOURCE")) {
      RefuseExact("SB_MODEL_OPERATION_SEMANTIC_REFUSED_V1",
                  "FROM requires exactly one COLUMNAR_SOURCE");
      return FinishRefusal();
    }
    const Token& source_operator = Consume();
    if (!RequireSymbol("(", "columnar_source_open_required",
                       "COLUMNAR_SOURCE requires an opening parenthesis") ||
        AtEnd() || !IsNameToken(Current())) {
      RefuseExact("SB_MODEL_BINDING_INCOMPLETE_V1",
                  "COLUMNAR_SOURCE requires a qualified relation name");
      return FinishRefusal();
    }
    NativeCatalogRelationSourceAstNode source;
    source.source_id = 1;
    source.source_kind = NativeRelationSourceAstKind::kColumnar;
    source.model_family_id = "columnar";
    const Token& first_name = Consume();
    const Token* last_name = &first_name;
    source.qualified_name.push_back(
        {first_name.text, first_name.quoted, TokenSourceRange(first_name)});
    while (AtSymbol(".")) {
      Consume();
      if (AtEnd() || !IsNameToken(Current())) {
        RefuseExact("SB_MODEL_BINDING_INCOMPLETE_V1",
                    "qualified columnar relation name is incomplete");
        return FinishRefusal();
      }
      last_name = &Consume();
      source.qualified_name.push_back(
          {last_name->text, last_name->quoted, TokenSourceRange(*last_name)});
    }
    source.qualified_name_range = Span(first_name, *last_name);
    if (!RequireSymbol(")", "columnar_source_close_required",
                       "COLUMNAR_SOURCE requires a closing parenthesis")) {
      return FinishRefusal();
    }
    const Token& source_close = Previous();
    const Token* source_end = &source_close;
    if (!AtEnd() && IsWord(Current(), "AS")) {
      Consume();
      if (AtEnd() || !IsNameToken(Current())) {
        RefuseExact("SB_MODEL_BINDING_INCOMPLETE_V1",
                    "COLUMNAR_SOURCE AS requires an alias");
        return FinishRefusal();
      }
      const Token& alias = Consume();
      source.alias = NativeIdentifierAstNode{
          alias.text, alias.quoted, TokenSourceRange(alias)};
      source.alias_is_explicit = true;
      source_end = &alias;
    } else {
      source.alias = NativeIdentifierAstNode{
          last_name->text, last_name->quoted, TokenSourceRange(*last_name)};
    }

    NativeExpressionAstNode source_root;
    source_root.expression_id = NextExpressionId();
    source_root.expression_kind = NativeExpressionAstKind::kFunctionCall;
    source_root.operator_name = "COLUMNAR_SOURCE";
    source_root.spelling = SourceSpelling(source_operator, source_close);
    source_root.range = Span(source_operator, source_close);
    source.model_operation_ids.push_back(source_root.operator_name);
    source.model_operation_expression_ids.push_back(source_root.expression_id);
    document_.expressions.push_back(std::move(source_root));
    const auto expression_at = [&](const std::uint32_t id)
        -> NativeExpressionAstNode* {
      return id == 0 || id > document_.expressions.size()
                 ? nullptr
                 : &document_.expressions[id - 1];
    };
    const auto same_alias = [&](const NativeExpressionAstNode* expression,
                                const NativeIdentifierAstNode& expected) {
      if (expression == nullptr ||
          expression->expression_kind != NativeExpressionAstKind::kIdentifier ||
          expression->qualified_identifier.size() != 1) return false;
      const auto& presented = expression->qualified_identifier.front();
      return presented.quoted == expected.quoted &&
             (presented.quoted
                  ? presented.spelling == expected.spelling
                  : ToLowerAscii(presented.spelling) ==
                        ToLowerAscii(expected.spelling));
    };
    NativeExpressionAstNode* select_project = nullptr;
    for (const auto expression_id : projection_expression_ids) {
      auto* candidate = expression_at(expression_id);
      if (candidate != nullptr &&
          candidate->expression_kind == NativeExpressionAstKind::kFunctionCall &&
          ToUpperAscii(candidate->operator_name) == "COLUMNAR_PROJECT") {
        if (select_project != nullptr) {
          RefuseExact("SB_MODEL_COLUMNAR_PROJECT_DUPLICATE_REFUSED_V1",
                      "COLUMNAR_PROJECT appears more than once");
          return FinishRefusal();
        }
        select_project = candidate;
      }
    }
    if (select_project != nullptr) {
      RefuseExact("SB_MODEL_OPERATION_SEMANTIC_REFUSED_V1",
                  "COLUMNAR_PROJECT is a source attachment, not a SELECT expression");
      return FinishRefusal();
    }

    const NativeIdentifierAstNode source_alias = *source.alias;
    if (AtSymbol(",")) {
      if (!source.alias_is_explicit) {
        RefuseExact("SB_MODEL_BINDING_INCOMPLETE_V1",
                    "COLUMNAR_PROJECT requires an explicit source alias");
        return FinishRefusal();
      }
      Consume();
      if (AtEnd() || !IsWord(Current(), "COLUMNAR_PROJECT")) {
        RefuseExact("SB_MODEL_OPERATION_SEMANTIC_REFUSED_V1",
                    "columnar source attachment must be COLUMNAR_PROJECT");
        return FinishRefusal();
      }
      const auto project_id = ParseExpression(3, 0);
      if (!project_id.has_value()) return FinishRefusal();
      auto* project = expression_at(*project_id);
      if (project == nullptr ||
          project->expression_kind != NativeExpressionAstKind::kFunctionCall ||
          ToUpperAscii(project->operator_name) != "COLUMNAR_PROJECT" ||
          project->child_expression_ids.size() < 2 ||
          project->child_expression_ids.size() > 257 ||
          !same_alias(expression_at(project->child_expression_ids.front()),
                      source_alias)) {
        RefuseExact("SB_MODEL_COLUMNAR_PROJECTION_INVALID_V1",
                    "COLUMNAR_PROJECT requires alias and 1..256 columns");
        return FinishRefusal();
      }
      std::unordered_set<std::string> names;
      source.model_columnar_alias_expression_id =
          project->child_expression_ids.front();
      source.model_columnar_operation_expression_id = project->expression_id;
      for (std::size_t index = 1; index < project->child_expression_ids.size();
           ++index) {
        auto* column = expression_at(project->child_expression_ids[index]);
        if (column == nullptr ||
            column->expression_kind != NativeExpressionAstKind::kIdentifier ||
            column->qualified_identifier.size() < 2 ||
            column->qualified_identifier.front().quoted !=
                source_alias.quoted ||
            (source_alias.quoted
                 ? column->qualified_identifier.front().spelling !=
                       source_alias.spelling
                 : ToLowerAscii(
                       column->qualified_identifier.front().spelling) !=
                       ToLowerAscii(source_alias.spelling))) {
          RefuseExact("SB_MODEL_COLUMNAR_PROJECTION_INVALID_V1",
                      "COLUMNAR_PROJECT contains a non-qualified source column");
          return FinishRefusal();
        }
        std::string key;
        for (const auto& part : column->qualified_identifier) {
          key += (part.quoted ? "Q:" + part.spelling
                              : "U:" + ToLowerAscii(part.spelling));
          key.push_back('/');
        }
        if (!names.insert(key).second) {
          RefuseExact("SB_MODEL_COLUMNAR_PROJECT_DUPLICATE_REFUSED_V1",
                      "COLUMNAR_PROJECT contains a duplicate column");
          return FinishRefusal();
        }
        source.model_columnar_project_expression_ids.push_back(
            column->expression_id);
        source.model_columnar_project_names.push_back(
            column->qualified_identifier);
      }
      project->operator_name = "COLUMNAR_PROJECT";
      source.model_source_alias = source.alias;
      source.model_columnar_project_expression_id = project->expression_id;
      source_end = &Previous();
      if (!AtEnd() && IsWord(Current(), "AS")) {
        Consume();
        if (AtEnd() || !IsNameToken(Current())) {
          RefuseExact("SB_MODEL_BINDING_INCOMPLETE_V1",
                      "COLUMNAR_PROJECT AS requires a result alias");
          return FinishRefusal();
        }
        const Token& result_alias = Consume();
        source.alias = NativeIdentifierAstNode{
            result_alias.text, result_alias.quoted,
            TokenSourceRange(result_alias)};
        source.alias_is_explicit = true;
        source_end = &result_alias;
      }
    }
    std::vector<std::uint32_t> predicate_expression_ids;
    if (!AtEnd() && IsWord(Current(), "WHERE")) {
      Consume();
      const auto filter_id = ParseExpression(3, 0);
      if (!filter_id.has_value()) return FinishRefusal();
      auto* filter = expression_at(*filter_id);
      if (filter == nullptr ||
          filter->expression_kind != NativeExpressionAstKind::kFunctionCall ||
          ToUpperAscii(filter->operator_name) != "COLUMNAR_FILTER" ||
          filter->child_expression_ids.size() != 2 ||
          !same_alias(expression_at(filter->child_expression_ids[0]),
                      source_alias)) {
        RefuseExact("SB_MODEL_COLUMNAR_FILTER_INVALID_V1",
                    "COLUMNAR_FILTER requires a positive top-level source-alias predicate");
        return FinishRefusal();
      }
      filter->operator_name = "COLUMNAR_FILTER";
      source.model_columnar_alias_expression_id =
          filter->child_expression_ids[0];
      source.model_columnar_operation_expression_id = filter->expression_id;
      source.model_columnar_filter_expression_id = filter->expression_id;
      source.model_columnar_predicate_expression_id =
          filter->child_expression_ids[1];
      predicate_expression_ids.push_back(filter->expression_id);
      if (!AtEnd() && IsWord(Current(), "AND")) {
        Consume();
        const auto residual_id = ParseExpression(0, 0);
        if (!residual_id.has_value()) return FinishRefusal();
        predicate_expression_ids.push_back(*residual_id);
      }
    }
    if (source.model_columnar_filter_expression_id.has_value()) {
      source.model_operation_ids.push_back("COLUMNAR_FILTER");
      source.model_operation_expression_ids.push_back(
          *source.model_columnar_filter_expression_id);
    }
    if (source.model_columnar_project_expression_id.has_value()) {
      source.model_operation_ids.push_back("COLUMNAR_PROJECT");
      source.model_operation_expression_ids.push_back(
          *source.model_columnar_project_expression_id);
    }
    source.model_operation_id = source.model_operation_ids.size() == 1
                                    ? source.model_operation_ids.front()
                                    : std::string{};
    source.model_columnar_operation_expression_id =
        source.model_operation_expression_ids.size() == 2
            ? std::optional<std::uint32_t>{
                  source.model_operation_expression_ids.back()}
            : std::nullopt;
    const auto columnar_filter_count = std::ranges::count_if(
        document_.expressions, [](const auto& expression) {
          return expression.expression_kind ==
                     NativeExpressionAstKind::kFunctionCall &&
                 ToUpperAscii(expression.operator_name) == "COLUMNAR_FILTER";
        });
    const auto columnar_project_count = std::ranges::count_if(
        document_.expressions, [](const auto& expression) {
          return expression.expression_kind ==
                     NativeExpressionAstKind::kFunctionCall &&
                 ToUpperAscii(expression.operator_name) == "COLUMNAR_PROJECT";
        });
    if (columnar_filter_count !=
            static_cast<std::ptrdiff_t>(
                source.model_columnar_filter_expression_id.has_value())) {
      RefuseExact("SB_MODEL_OPERATION_SEMANTIC_REFUSED_V1",
                  "COLUMNAR_FILTER must appear exactly once as a positive top-level conjunct");
      return FinishRefusal();
    }
    if (columnar_project_count !=
        static_cast<std::ptrdiff_t>(
            source.model_columnar_project_expression_id.has_value())) {
      RefuseExact("SB_MODEL_COLUMNAR_PROJECT_DUPLICATE_REFUSED_V1",
                  "COLUMNAR_PROJECT must appear exactly once as a source attachment");
      return FinishRefusal();
    }
    if (AtSymbol(";")) Consume();
    if (!AtEnd()) {
      RefuseExact("SB_MODEL_OPERATION_SEMANTIC_REFUSED_V1",
                  "unexpected input follows the bounded columnar query");
      return FinishRefusal();
    }
    NativeRelationAstNode relation;
    relation.relation_id = 1;
    relation.relation_kind = NativeRelationAstKind::kCatalogSource;
    relation.relation_source_ids = {source.source_id};
    relation.output_expression_ids = std::move(projection_expression_ids);
    relation.predicate_expression_ids = std::move(predicate_expression_ids);
    relation.range = Span(source_operator, *source_end);
    source.range = relation.range;
    document_.catalog_relation_sources.push_back(std::move(source));
    document_.relations.push_back(std::move(relation));
    document_.root_relation_id = 1;
    document_.status = NativeRelationalParseStatus::kAccepted;
    return std::move(document_);
  }

  NativeRelationalAstDocument ParseSearchModelSelect() {
    // QOW-SOURCE-RCP-078-SEARCH-GRAMMAR-V1
    document_.status = NativeRelationalParseStatus::kRefused;
    if (cst_.messages.has_errors()) {
      document_.messages = cst_.messages;
      return FinishRefusal();
    }
    if (tokens_.size() > kMaximumNativeRelationalTokens) {
      Refuse("token_limit_exceeded", "search query token limit exceeded");
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
    if (!RequireWord("FROM", "search_from_required",
                     "search source requires FROM") ||
        AtEnd() || !IsWord(Current(), "SEARCH_SOURCE")) {
      RefuseExact("SB_MODEL_OPERATION_SEMANTIC_REFUSED_V1",
                  "FROM requires exactly one SEARCH_SOURCE");
      return FinishRefusal();
    }
    const Token& source_operator = Consume();
    if (!RequireSymbol("(", "search_source_open_required",
                       "SEARCH_SOURCE requires an opening parenthesis") ||
        AtEnd() || !IsNameToken(Current())) {
      RefuseExact("SB_MODEL_BINDING_INCOMPLETE_V1",
                  "SEARCH_SOURCE requires a qualified collection name");
      return FinishRefusal();
    }
    NativeCatalogRelationSourceAstNode source;
    source.source_id = 1;
    source.source_kind = NativeRelationSourceAstKind::kSearch;
    source.model_family_id = "search";
    const Token& first_name = Consume();
    const Token* last_name = &first_name;
    source.qualified_name.push_back(
        {first_name.text, first_name.quoted, TokenSourceRange(first_name)});
    while (AtSymbol(".")) {
      Consume();
      if (AtEnd() || !IsNameToken(Current())) {
        RefuseExact("SB_MODEL_BINDING_INCOMPLETE_V1",
                    "qualified search collection name is incomplete");
        return FinishRefusal();
      }
      last_name = &Consume();
      source.qualified_name.push_back(
          {last_name->text, last_name->quoted, TokenSourceRange(*last_name)});
    }
    source.qualified_name_range = Span(first_name, *last_name);
    if (!RequireSymbol(")", "search_source_close_required",
                       "SEARCH_SOURCE requires a closing parenthesis")) {
      return FinishRefusal();
    }
    const Token* source_end = &Previous();
    if (!AtEnd() && IsWord(Current(), "AS")) {
      Consume();
      if (AtEnd() || !IsNameToken(Current())) {
        RefuseExact("SB_MODEL_BINDING_INCOMPLETE_V1",
                    "SEARCH_SOURCE AS requires an alias");
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
      RefuseExact("SB_MODEL_OPERATION_SEMANTIC_REFUSED_V1",
                  "SEARCH_SOURCE requires exactly one SEARCH_MATCH");
      return FinishRefusal();
    }
    Consume();
    const auto match_id = ParseExpression(3, 0);
    if (!match_id.has_value()) return FinishRefusal();
    const auto expression_at = [&](const std::uint32_t id)
        -> NativeExpressionAstNode* {
      return id == 0 || id > document_.expressions.size()
                 ? nullptr
                 : &document_.expressions[id - 1];
    };
    if (!AtEnd() && IsWord(Current(), "AS")) {
      Consume();
      if (AtEnd() || !IsNameToken(Current())) {
        RefuseExact("SB_MODEL_BINDING_INCOMPLETE_V1",
                    "SEARCH_MATCH AS requires a result alias");
        return FinishRefusal();
      }
      const Token& result_alias = Consume();
      source.model_search_result_alias = NativeIdentifierAstNode{
          result_alias.text, result_alias.quoted,
          TokenSourceRange(result_alias)};
    }
    std::optional<std::uint32_t> filter_id;
    std::uint32_t predicate_id = *match_id;
    if (!AtEnd() && IsWord(Current(), "AND")) {
      const Token& and_token = Consume();
      const auto parsed_filter_id = ParseExpression(3, 0);
      if (!parsed_filter_id.has_value()) return FinishRefusal();
      filter_id = *parsed_filter_id;
      const auto* parsed_filter = expression_at(*parsed_filter_id);
      NativeExpressionAstNode conjunction;
      conjunction.expression_id = NextExpressionId();
      conjunction.expression_kind = NativeExpressionAstKind::kBinary;
      conjunction.child_expression_ids = {*match_id, *parsed_filter_id};
      conjunction.operator_name = "AND";
      conjunction.range = RangeFromTokenAndRange(
          and_token, parsed_filter == nullptr ? TokenSourceRange(and_token)
                                              : parsed_filter->range);
      conjunction.spelling = SourceForRange(conjunction.range);
      predicate_id = conjunction.expression_id;
      document_.expressions.push_back(std::move(conjunction));
    }
    auto* match = expression_at(*match_id);
    auto* filter = filter_id.has_value() ? expression_at(*filter_id) : nullptr;
    if (match == nullptr ||
        match->expression_kind != NativeExpressionAstKind::kFunctionCall ||
        ToUpperAscii(match->operator_name) != "SEARCH_MATCH" ||
        match->child_expression_ids.size() != 4) {
      RefuseExact("SB_MODEL_OPERATION_SEMANTIC_REFUSED_V1",
                  "SEARCH_MATCH requires alias, query, analyzer, and top-k");
      return FinishRefusal();
    }
    match->operator_name = "SEARCH_MATCH";
    auto* alias = expression_at(match->child_expression_ids[0]);
    auto* query = expression_at(match->child_expression_ids[1]);
    auto* analyzer = expression_at(match->child_expression_ids[2]);
    auto* top_k = expression_at(match->child_expression_ids[3]);
    const auto same_alias = [&](const NativeExpressionAstNode* expression) {
      if (expression == nullptr || !source.alias.has_value() ||
          expression->expression_kind != NativeExpressionAstKind::kIdentifier ||
          expression->qualified_identifier.size() != 1) {
        return false;
      }
      const auto& presented = expression->qualified_identifier.front();
      return presented.quoted == source.alias->quoted &&
             (presented.quoted
                  ? presented.spelling == source.alias->spelling
                  : ToLowerAscii(presented.spelling) ==
                        ToLowerAscii(source.alias->spelling));
    };
    if (!same_alias(alias) || query == nullptr || analyzer == nullptr ||
        top_k == nullptr ||
        query->expression_kind != NativeExpressionAstKind::kFunctionCall ||
        analyzer->expression_kind != NativeExpressionAstKind::kIdentifier ||
        analyzer->qualified_identifier.empty() ||
        top_k->expression_kind != NativeExpressionAstKind::kLiteral ||
        top_k->literal_kind != NativeLiteralAstKind::kNumeric) {
      RefuseExact("SB_MODEL_SEARCH_QUERY_TYPE_REFUSED_V1",
                  "SEARCH_MATCH operands are not the exact typed shape");
      return FinishRefusal();
    }
    const auto query_kind = ToUpperAscii(query->operator_name);
    const bool fuzzy = query_kind == "SEARCH_FUZZY";
    if (query_kind != "SEARCH_TERMS" && query_kind != "SEARCH_PHRASE" &&
        !fuzzy) {
      RefuseExact("SB_MODEL_SEARCH_QUERY_TYPE_REFUSED_V1",
                  "search query constructor is outside the closed v1 set");
      return FinishRefusal();
    }
    if ((!fuzzy && query->child_expression_ids.size() != 1) ||
        (fuzzy && query->child_expression_ids.size() != 2)) {
      RefuseExact("SB_MODEL_SEARCH_QUERY_TYPE_REFUSED_V1",
                  "search query constructor has invalid arity");
      return FinishRefusal();
    }
    auto* text = expression_at(query->child_expression_ids[0]);
    auto* edit = fuzzy ? expression_at(query->child_expression_ids[1]) : nullptr;
    if (text == nullptr ||
        !((text->expression_kind == NativeExpressionAstKind::kLiteral &&
           text->literal_kind == NativeLiteralAstKind::kString) ||
          text->expression_kind == NativeExpressionAstKind::kParameter) ||
        (fuzzy &&
         (edit == nullptr ||
          edit->expression_kind != NativeExpressionAstKind::kLiteral ||
          edit->literal_kind != NativeLiteralAstKind::kNumeric ||
          edit->spelling != "1"))) {
      RefuseExact(fuzzy ? "SB_MODEL_SEARCH_QUERY_TOKEN_LIMIT_REFUSED_V1"
                        : "SB_MODEL_SEARCH_QUERY_TYPE_REFUSED_V1",
                  "search query TEXT/edit operands are invalid");
      return FinishRefusal();
    }
    std::uint64_t top_k_value = 0;
    const auto converted = std::from_chars(
        top_k->spelling.data(), top_k->spelling.data() + top_k->spelling.size(),
        top_k_value);
    if (top_k->spelling.empty() ||
        (top_k->spelling.size() > 1 && top_k->spelling.front() == '0') ||
        converted.ec != std::errc{} ||
        converted.ptr != top_k->spelling.data() + top_k->spelling.size() ||
        top_k_value == 0 || top_k_value > 0xffffffffULL) {
      RefuseExact("SB_MODEL_SEARCH_TOP_K_REFUSED_V1",
                  "search top-k must be a canonical uint32 literal");
      return FinishRefusal();
    }
    query->operator_name = query_kind;
    source.model_search_alias_expression_id = alias->expression_id;
    source.model_search_match_expression_id = match->expression_id;
    source.model_search_query_expression_id = query->expression_id;
    source.model_search_text_expression_id = text->expression_id;
    if (edit != nullptr) source.model_search_edit_expression_id = edit->expression_id;
    source.model_search_analyzer_expression_id = analyzer->expression_id;
    source.model_search_top_k_expression_id = top_k->expression_id;
    source.model_search_analyzer_name = analyzer->qualified_identifier;
    source.model_search_query_kind = query_kind;
    source.model_search_top_k = top_k_value;

    if (filter != nullptr) {
      if (filter->expression_kind != NativeExpressionAstKind::kFunctionCall ||
          ToUpperAscii(filter->operator_name) != "SEARCH_FILTER" ||
          filter->child_expression_ids.size() != 2) {
        RefuseExact("SB_MODEL_SEARCH_FILTER_REFUSED_V1",
                    "SEARCH_FILTER has an invalid function shape");
        return FinishRefusal();
      }
      filter->operator_name = "SEARCH_FILTER";
      auto* filter_alias = expression_at(filter->child_expression_ids[0]);
      auto* predicate = expression_at(filter->child_expression_ids[1]);
      auto* column = predicate != nullptr &&
                             predicate->child_expression_ids.size() == 2
                         ? expression_at(predicate->child_expression_ids[0])
                         : nullptr;
      auto* value = predicate != nullptr &&
                            predicate->child_expression_ids.size() == 2
                        ? expression_at(predicate->child_expression_ids[1])
                        : nullptr;
      const bool exact_category =
          column != nullptr && source.alias.has_value() &&
          column->expression_kind == NativeExpressionAstKind::kIdentifier &&
          column->qualified_identifier.size() == 2 &&
          column->qualified_identifier[0].quoted == source.alias->quoted &&
          (source.alias->quoted
               ? column->qualified_identifier[0].spelling ==
                     source.alias->spelling
               : ToLowerAscii(column->qualified_identifier[0].spelling) ==
                     ToLowerAscii(source.alias->spelling)) &&
          (column->qualified_identifier[1].quoted
               ? column->qualified_identifier[1].spelling == "category"
               : ToLowerAscii(column->qualified_identifier[1].spelling) ==
                     "category");
      if (!same_alias(filter_alias) || predicate == nullptr ||
          predicate->expression_kind != NativeExpressionAstKind::kBinary ||
          predicate->operator_name != "=" || !exact_category ||
          value == nullptr ||
          !((value->expression_kind == NativeExpressionAstKind::kLiteral &&
             value->literal_kind == NativeLiteralAstKind::kString) ||
            value->expression_kind == NativeExpressionAstKind::kParameter)) {
        RefuseExact("SB_MODEL_SEARCH_FILTER_REFUSED_V1",
                    "SEARCH_FILTER requires alias and category = TEXT");
        return FinishRefusal();
      }
      source.model_search_filter_expression_id = filter->expression_id;
      source.model_search_category_predicate_expression_id =
          predicate->expression_id;
      source.model_search_category_column_expression_id = column->expression_id;
      source.model_search_category_value_expression_id = value->expression_id;
    }
    source.model_operation_id =
        query_kind == "SEARCH_PHRASE"
            ? "SEARCH_PHRASE_QUERY"
            : (query_kind == "SEARCH_FUZZY" ? "SEARCH_FUZZY_QUERY"
                                             : "SEARCH_RANKED_QUERY");
    if (AtSymbol(";")) Consume();
    if (!AtEnd()) {
      RefuseExact("SB_MODEL_OPERATION_SEMANTIC_REFUSED_V1",
                  "unexpected input follows the bounded search query");
      return FinishRefusal();
    }
    NativeRelationAstNode relation;
    relation.relation_id = 1;
    relation.relation_kind = NativeRelationAstKind::kCatalogSource;
    relation.relation_source_ids = {source.source_id};
    relation.output_expression_ids = projection_expression_ids;
    relation.predicate_expression_ids = {predicate_id};
    relation.range = Span(source_operator, *source_end);
    source.range = relation.range;
    document_.catalog_relation_sources.push_back(std::move(source));
    document_.relations.push_back(std::move(relation));
    document_.root_relation_id = 1;
    document_.status = NativeRelationalParseStatus::kAccepted;
    (void)select;
    return std::move(document_);
  }

  NativeRelationalAstDocument ParseVectorModelSelect() {
    // QOW-SOURCE-RCP-077-VECTOR-GRAMMAR-V1
    document_.status = NativeRelationalParseStatus::kRefused;
    if (cst_.messages.has_errors()) {
      document_.messages = cst_.messages;
      return FinishRefusal();
    }
    if (tokens_.size() > kMaximumNativeRelationalTokens) {
      Refuse("token_limit_exceeded", "vector query token limit exceeded");
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
    if (!RequireWord("FROM", "vector_from_required",
                     "vector source requires FROM") ||
        AtEnd() || !IsWord(Current(), "VECTOR_SOURCE")) {
      RefuseExact("SB_MODEL_OPERATION_SEMANTIC_REFUSED_V1",
                  "FROM requires exactly one VECTOR_SOURCE");
      return FinishRefusal();
    }

    const Token& source_operator = Consume();
    if (!RequireSymbol("(", "vector_source_open_required",
                       "VECTOR_SOURCE requires an opening parenthesis") ||
        AtEnd() || !IsNameToken(Current())) {
      RefuseExact("SB_MODEL_BINDING_INCOMPLETE_V1",
                  "VECTOR_SOURCE requires a qualified collection name");
      return FinishRefusal();
    }
    NativeCatalogRelationSourceAstNode source;
    source.source_id = 1;
    source.source_kind = NativeRelationSourceAstKind::kVector;
    source.model_family_id = "vector";
    const Token& first_name = Consume();
    const Token* last_name = &first_name;
    source.qualified_name.push_back(
        {first_name.text, first_name.quoted, TokenSourceRange(first_name)});
    while (AtSymbol(".")) {
      Consume();
      if (AtEnd() || !IsNameToken(Current())) {
        RefuseExact("SB_MODEL_BINDING_INCOMPLETE_V1",
                    "qualified vector collection name is incomplete");
        return FinishRefusal();
      }
      last_name = &Consume();
      source.qualified_name.push_back(
          {last_name->text, last_name->quoted, TokenSourceRange(*last_name)});
    }
    source.qualified_name_range = Span(first_name, *last_name);
    if (!RequireSymbol(")", "vector_source_close_required",
                       "VECTOR_SOURCE requires a closing parenthesis")) {
      return FinishRefusal();
    }
    const Token* source_end = &Previous();
    if (!AtEnd() && IsWord(Current(), "AS")) {
      Consume();
      if (AtEnd() || !IsNameToken(Current())) {
        RefuseExact("SB_MODEL_BINDING_INCOMPLETE_V1",
                    "VECTOR_SOURCE AS requires an alias");
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
      RefuseExact("SB_MODEL_VECTOR_NEAREST_REFUSED_V1",
                  "VECTOR_SOURCE requires exactly one VECTOR_NEAREST");
      return FinishRefusal();
    }
    Consume();
    // Parse the nearest producer independently so its optional result alias is
    // not confused with the VECTOR_SOURCE alias or a filter child.
    const auto nearest_id = ParseExpression(3, 0);
    if (!nearest_id.has_value()) return FinishRefusal();
    const auto expression_at = [&](const std::uint32_t id)
        -> NativeExpressionAstNode* {
      return id == 0 || id > document_.expressions.size()
                 ? nullptr
                 : &document_.expressions[id - 1];
    };
    if (!AtEnd() && IsWord(Current(), "AS")) {
      Consume();
      if (AtEnd() || !IsNameToken(Current())) {
        RefuseExact("SB_MODEL_BINDING_INCOMPLETE_V1",
                    "VECTOR_NEAREST AS requires a result alias");
        return FinishRefusal();
      }
      const Token& result_alias = Consume();
      source.model_vector_result_alias = NativeIdentifierAstNode{
          result_alias.text, result_alias.quoted,
          TokenSourceRange(result_alias)};
    }
    std::optional<std::uint32_t> vector_filter_id;
    std::uint32_t predicate_id = *nearest_id;
    if (!AtEnd() && IsWord(Current(), "AND")) {
      const Token& and_token = Consume();
      const auto filter_id = ParseExpression(3, 0);
      if (!filter_id.has_value()) return FinishRefusal();
      vector_filter_id = *filter_id;
      const auto* parsed_filter = expression_at(*filter_id);
      NativeExpressionAstNode conjunction;
      conjunction.expression_id = NextExpressionId();
      conjunction.expression_kind = NativeExpressionAstKind::kBinary;
      conjunction.child_expression_ids = {*nearest_id, *filter_id};
      conjunction.operator_name = "AND";
      conjunction.range = RangeFromTokenAndRange(
          and_token, parsed_filter == nullptr ? TokenSourceRange(and_token)
                                              : parsed_filter->range);
      conjunction.spelling = SourceForRange(conjunction.range);
      predicate_id = conjunction.expression_id;
      document_.expressions.push_back(std::move(conjunction));
    }
    auto* nearest = expression_at(*nearest_id);
    auto* filter = vector_filter_id.has_value()
                       ? expression_at(*vector_filter_id)
                       : nullptr;
    if (nearest == nullptr ||
        nearest->expression_kind != NativeExpressionAstKind::kFunctionCall ||
        ToUpperAscii(nearest->operator_name) != "VECTOR_NEAREST" ||
        nearest->child_expression_ids.size() != 4) {
      RefuseExact("SB_MODEL_VECTOR_NEAREST_REFUSED_V1",
                  "VECTOR_NEAREST requires alias, query, metric, and top-k");
      return FinishRefusal();
    }
    nearest->operator_name = "VECTOR_NEAREST";
    auto* alias = expression_at(nearest->child_expression_ids[0]);
    auto* query = expression_at(nearest->child_expression_ids[1]);
    auto* metric = expression_at(nearest->child_expression_ids[2]);
    auto* top_k = expression_at(nearest->child_expression_ids[3]);
    const auto same_alias = [&](const NativeExpressionAstNode* expression) {
      if (expression == nullptr || !source.alias.has_value() ||
          expression->expression_kind != NativeExpressionAstKind::kIdentifier ||
          expression->qualified_identifier.size() != 1) {
        return false;
      }
      const auto& presented = expression->qualified_identifier.front();
      return presented.quoted == source.alias->quoted &&
             (presented.quoted
                  ? presented.spelling == source.alias->spelling
                  : ToLowerAscii(presented.spelling) ==
                        ToLowerAscii(source.alias->spelling));
    };
    if (!same_alias(alias) || query == nullptr || metric == nullptr ||
        top_k == nullptr ||
        !((query->expression_kind == NativeExpressionAstKind::kLiteral &&
           query->literal_kind == NativeLiteralAstKind::kVector) ||
          query->expression_kind == NativeExpressionAstKind::kParameter) ||
        metric->expression_kind != NativeExpressionAstKind::kIdentifier ||
        metric->qualified_identifier.size() != 1 ||
        metric->qualified_identifier.front().quoted ||
        top_k->expression_kind != NativeExpressionAstKind::kLiteral ||
        top_k->literal_kind != NativeLiteralAstKind::kNumeric) {
      RefuseExact("SB_MODEL_VECTOR_VALUE_REFUSED_V1",
                  "VECTOR_NEAREST operands are not the exact typed shape");
      return FinishRefusal();
    }
    const auto metric_id =
        ToUpperAscii(metric->qualified_identifier.front().spelling);
    if (metric_id != "L2_SQUARED" && metric_id != "COSINE" &&
        metric_id != "INNER_PRODUCT") {
      RefuseExact("SB_MODEL_VECTOR_METRIC_REFUSED_V1",
                  "vector metric is outside the closed v1 set");
      return FinishRefusal();
    }
    std::uint64_t top_k_value = 0;
    const auto converted = std::from_chars(
        top_k->spelling.data(), top_k->spelling.data() + top_k->spelling.size(),
        top_k_value);
    if (top_k->spelling.empty() ||
        (top_k->spelling.size() > 1 && top_k->spelling.front() == '0') ||
        converted.ec != std::errc{} ||
        converted.ptr != top_k->spelling.data() + top_k->spelling.size() ||
        top_k_value == 0 || top_k_value > 0xffffffffULL) {
      RefuseExact("SB_MODEL_VECTOR_TOP_K_REFUSED_V1",
                  "vector top-k must be a canonical uint32 literal");
      return FinishRefusal();
    }
    metric->expression_kind = NativeExpressionAstKind::kLiteral;
    metric->literal_kind = NativeLiteralAstKind::kString;
    metric->qualified_identifier.clear();
    metric->spelling = metric_id;

    source.model_vector_alias_expression_id = alias->expression_id;
    source.model_vector_nearest_expression_id = nearest->expression_id;
    source.model_vector_query_expression_id = query->expression_id;
    source.model_vector_metric_expression_id = metric->expression_id;
    source.model_vector_top_k_expression_id = top_k->expression_id;
    source.model_vector_metric_id = metric_id;
    source.model_vector_top_k = top_k_value;

    if (filter != nullptr) {
      if (filter->expression_kind != NativeExpressionAstKind::kFunctionCall ||
          ToUpperAscii(filter->operator_name) != "VECTOR_FILTER" ||
          filter->child_expression_ids.size() != 2) {
        RefuseExact("SB_MODEL_VECTOR_FILTER_REFUSED_V1",
                    "VECTOR_FILTER has an invalid function shape");
        return FinishRefusal();
      }
      filter->operator_name = "VECTOR_FILTER";
      auto* filter_alias = expression_at(filter->child_expression_ids[0]);
      auto* metadata_predicate =
          expression_at(filter->child_expression_ids[1]);
      auto* metadata_column =
          metadata_predicate != nullptr &&
                  metadata_predicate->child_expression_ids.size() == 2
              ? expression_at(metadata_predicate->child_expression_ids[0])
              : nullptr;
      auto* metadata_value =
          metadata_predicate != nullptr &&
                  metadata_predicate->child_expression_ids.size() == 2
              ? expression_at(metadata_predicate->child_expression_ids[1])
              : nullptr;
      const bool exact_metadata_column =
          metadata_column != nullptr && source.alias.has_value() &&
          metadata_column->expression_kind ==
              NativeExpressionAstKind::kIdentifier &&
          metadata_column->qualified_identifier.size() == 2 &&
          metadata_column->qualified_identifier[0].quoted ==
              source.alias->quoted &&
          (source.alias->quoted
               ? metadata_column->qualified_identifier[0].spelling ==
                     source.alias->spelling
               : ToLowerAscii(
                     metadata_column->qualified_identifier[0].spelling) ==
                     ToLowerAscii(source.alias->spelling)) &&
          (metadata_column->qualified_identifier[1].quoted
               ? metadata_column->qualified_identifier[1].spelling ==
                     "metadata"
               : ToLowerAscii(
                     metadata_column->qualified_identifier[1].spelling) ==
                     "metadata");
      if (!same_alias(filter_alias) || metadata_predicate == nullptr ||
          metadata_predicate->expression_kind !=
              NativeExpressionAstKind::kBinary ||
          metadata_predicate->operator_name != "=" ||
          !exact_metadata_column || metadata_value == nullptr ||
          !((metadata_value->expression_kind == NativeExpressionAstKind::kLiteral &&
             metadata_value->literal_kind == NativeLiteralAstKind::kString) ||
            metadata_value->expression_kind ==
                NativeExpressionAstKind::kParameter)) {
        RefuseExact("SB_MODEL_VECTOR_FILTER_REFUSED_V1",
                    "VECTOR_FILTER requires alias and metadata = TEXT");
        return FinishRefusal();
      }
      source.model_vector_filter_expression_id = filter->expression_id;
      source.model_vector_metadata_predicate_expression_id =
          metadata_predicate->expression_id;
      source.model_vector_metadata_column_expression_id =
          metadata_column->expression_id;
      source.model_vector_metadata_value_expression_id =
          metadata_value->expression_id;
      source.model_operation_id = "VECTOR_FILTERED_SEARCH";
    } else {
      source.model_operation_id = "VECTOR_EXACT_SEARCH";
    }

    if (AtSymbol(";")) Consume();
    if (!AtEnd()) {
      RefuseExact("SB_MODEL_OPERATION_SEMANTIC_REFUSED_V1",
                  "unexpected input follows the bounded vector query");
      return FinishRefusal();
    }
    NativeRelationAstNode relation;
    relation.relation_id = 1;
    relation.relation_kind = NativeRelationAstKind::kCatalogSource;
    relation.relation_source_ids = {source.source_id};
    relation.output_expression_ids = projection_expression_ids;
    relation.predicate_expression_ids = {predicate_id};
    relation.range = Span(source_operator, *source_end);
    source.range = relation.range;
    document_.catalog_relation_sources.push_back(std::move(source));
    document_.relations.push_back(std::move(relation));
    document_.root_relation_id = 1;
    document_.status = NativeRelationalParseStatus::kAccepted;
    (void)select;
    return std::move(document_);
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
    if (tokens_.size() < 9 || tokens_[2]->text != "(") return false;
    if (IsWord(*tokens_[1], "SUM") || IsWord(*tokens_[1], "MIN") ||
        IsWord(*tokens_[1], "MAX") || IsWord(*tokens_[1], "COUNT") ||
        IsWord(*tokens_[1], "BOOL_AND") ||
        IsWord(*tokens_[1], "BOOL_OR") || IsWord(*tokens_[1], "EVERY")) {
      return tokens_.size() > 6 && tokens_[4]->text == ")" &&
             IsWord(*tokens_[5], "OVER") &&
             (tokens_[6]->text == "(" ||
              tokens_[6]->kind == TokenKind::kIdentifier);
    }
    if (IsWord(*tokens_[1], "NTILE") || IsWord(*tokens_[1], "LAG") ||
        IsWord(*tokens_[1], "LEAD") ||
        IsWord(*tokens_[1], "FIRST_VALUE") ||
        IsWord(*tokens_[1], "LAST_VALUE") ||
        IsWord(*tokens_[1], "NTH_VALUE")) {
      return true;
    }
    return (IsWord(*tokens_[1], "ROW_NUMBER") ||
            IsWord(*tokens_[1], "RANK") ||
            IsWord(*tokens_[1], "DENSE_RANK") ||
            IsWord(*tokens_[1], "PERCENT_RANK") ||
            IsWord(*tokens_[1], "CUME_DIST")) &&
           tokens_[3]->text == ")" && IsWord(*tokens_[4], "OVER") &&
           (tokens_[5]->text == "(" ||
            tokens_[5]->kind == TokenKind::kIdentifier);
  }

  NativeRelationalAstDocument ParseWindowSelect() {
    // QOW-SOURCE-RCP-050-TYPED-WINDOW-AST-V1
    // The general ROW_NUMBER surface preserves the complete window
    // specification. RANK, DENSE_RANK, PERCENT_RANK, CUME_DIST, NTILE, LAG,
    // LEAD, FIRST_VALUE, LAST_VALUE, NTH_VALUE, and the exact aggregate
    // SUM/MIN/MAX/COUNT/BOOL_AND/BOOL_OR/EVERY(identifier) cohort and the exact
    // COUNT(*) form are admitted only for the exact global, one-direct-column
    // ordering profile executed by the canonical spine. NTILE additionally
    // requires one exact positive signed-int64 literal operand. LAG and LEAD
    // admit one direct exact signed-int64 or boolean column with only their
    // implicit offset and implicit NULL default. FIRST_VALUE and LAST_VALUE
    // admit the same exact value cohort and consume the effective implicit
    // ordered frame. NTH_VALUE admits that same value cohort plus one exact
    // positive signed-int64 literal position and normalizes omitted
    // origin/NULL-treatment state to FROM FIRST RESPECT NULLS in the canonical
    // execution route. Aggregate
    // Numeric aggregates admit one direct bounded-signed value column; boolean
    // aggregates admit one direct boolean value column. COUNT(identifier)
    // admits one direct engine-bound canonical value column of any type, while
    // COUNT(*) carries no value expression and counts every row in the
    // effective frame. All use the same exact implicit ordered frame and bind
    // through the engine-owned aggregate registry rather than the native
    // window-function registry.
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
    const bool rank_window = IsWord(function_token, "RANK");
    const bool dense_rank_window = IsWord(function_token, "DENSE_RANK");
    const bool percent_rank_window = IsWord(function_token, "PERCENT_RANK");
    const bool cume_dist_window = IsWord(function_token, "CUME_DIST");
    const bool ntile_window = IsWord(function_token, "NTILE");
    const bool lag_window = IsWord(function_token, "LAG");
    const bool lead_window = IsWord(function_token, "LEAD");
    const bool first_value_window = IsWord(function_token, "FIRST_VALUE");
    const bool last_value_window = IsWord(function_token, "LAST_VALUE");
    const bool nth_value_window = IsWord(function_token, "NTH_VALUE");
    const bool aggregate_window = IsWord(function_token, "SUM") ||
                                  IsWord(function_token, "MIN") ||
                                  IsWord(function_token, "MAX") ||
                                  IsWord(function_token, "COUNT") ||
                                  IsWord(function_token, "BOOL_AND") ||
                                  IsWord(function_token, "BOOL_OR") ||
                                  IsWord(function_token, "EVERY");
    const bool navigation_window = lag_window || lead_window;
    const bool value_window =
        navigation_window || first_value_window || last_value_window ||
        nth_value_window || aggregate_window;
    const bool peer_ranking_window =
        rank_window || dense_rank_window || percent_rank_window ||
        cume_dist_window;
    const bool strict_ordered_window =
        peer_ranking_window || ntile_window || value_window;
    const std::string function_name =
        aggregate_window
            ? CanonicalTokenText(function_token)
            : (value_window
            ? (first_value_window ? "FIRST_VALUE"
                                  : (last_value_window
                                         ? "LAST_VALUE"
                                         : (nth_value_window
                                                ? "NTH_VALUE"
                                                : (lag_window ? "LAG"
                                                              : "LEAD"))))
            : (ntile_window
            ? "NTILE"
            : (cume_dist_window
            ? "CUME_DIST"
            : (percent_rank_window
                   ? "PERCENT_RANK"
                   : (dense_rank_window
                          ? "DENSE_RANK"
                          : (rank_window ? "RANK" : "ROW_NUMBER"))))));
    if (!RequireSymbol("(", "window_function_open_required",
                       function_name + " requires an opening parenthesis")) {
      return FinishRefusal();
    }
    const bool aggregate_count_star_window =
        IsWord(function_token, "COUNT") && AtSymbol("*");
    const auto function_expression_id = NextExpressionId();
    NativeExpressionAstNode function;
    function.expression_id = function_expression_id;
    function.expression_kind = NativeExpressionAstKind::kFunctionCall;
    function.operator_name =
        aggregate_count_star_window ? "COUNT_STAR" : function_name;
    document_.expressions.push_back(std::move(function));
    std::optional<std::uint32_t> ntile_operand_expression_id;
    std::optional<std::uint32_t> navigation_operand_expression_id;
    std::optional<std::uint32_t> nth_position_expression_id;
    if (aggregate_count_star_window) {
      Consume();
    } else if (ntile_window) {
      if (AtEnd() || Current().kind != TokenKind::kNumericLiteral) {
        Refuse("ntile_operand_required",
               "NTILE requires one positive signed-int64 literal operand");
        return FinishRefusal();
      }
      const Token& operand_token = Consume();
      std::uint64_t bucket_count = 0;
      const auto [end, error] = std::from_chars(
          operand_token.text.data(),
          operand_token.text.data() + operand_token.text.size(), bucket_count);
      if (operand_token.text.empty() ||
          (operand_token.text.size() > 1 && operand_token.text.front() == '0') ||
          error != std::errc{} ||
          end != operand_token.text.data() + operand_token.text.size() ||
          bucket_count == 0 ||
          bucket_count > static_cast<std::uint64_t>(
                             std::numeric_limits<std::int64_t>::max())) {
        Refuse("ntile_operand_invalid",
               "NTILE operand must be a canonical positive signed-int64 literal");
        return FinishRefusal();
      }
      NativeExpressionAstNode operand;
      operand.expression_id = NextExpressionId();
      operand.expression_kind = NativeExpressionAstKind::kLiteral;
      operand.literal_kind = NativeLiteralAstKind::kNumeric;
      operand.spelling = operand_token.text;
      operand.range = TokenSourceRange(operand_token);
      ntile_operand_expression_id = operand.expression_id;
      document_.expressions.push_back(std::move(operand));
    } else if (value_window) {
      if (AtEnd() || Current().kind != TokenKind::kIdentifier) {
        const std::string_view operand_requirement =
            IsWord(function_token, "COUNT")
                ? " requires one direct canonical column operand"
                : ((IsWord(function_token, "BOOL_AND") ||
                    IsWord(function_token, "BOOL_OR") ||
                    IsWord(function_token, "EVERY"))
                       ? " requires one direct boolean column operand"
                       : (aggregate_window
                              ? " requires one direct bounded-signed column operand"
                              : " requires one direct signed-int64 or boolean column operand"));
        Refuse(aggregate_window
                   ? "aggregate_window_operand_required"
                   : (first_value_window
                   ? "first_value_operand_required"
                   : (last_value_window
                          ? "last_value_operand_required"
                          : (nth_value_window
                                 ? "nth_value_operand_required"
                                 : (lag_window ? "lag_operand_required"
                                               : "lead_operand_required")))),
               function_name + std::string(operand_requirement));
        return FinishRefusal();
      }
      const Token& operand_token = Consume();
      NativeExpressionAstNode operand;
      operand.expression_id = NextExpressionId();
      operand.expression_kind = NativeExpressionAstKind::kIdentifier;
      operand.spelling = operand_token.text;
      operand.range = TokenSourceRange(operand_token);
      navigation_operand_expression_id = operand.expression_id;
      document_.expressions.push_back(std::move(operand));
      if (nth_value_window) {
        if (!RequireSymbol(",", "nth_value_position_separator_required",
                           "NTH_VALUE requires a value and position separated by a comma")) {
          return FinishRefusal();
        }
        if (AtEnd() || Current().kind != TokenKind::kNumericLiteral) {
          Refuse("nth_value_position_required",
                 "NTH_VALUE requires one positive signed-int64 literal position");
          return FinishRefusal();
        }
        const Token& position_token = Consume();
        std::uint64_t position = 0;
        const auto [end, error] = std::from_chars(
            position_token.text.data(),
            position_token.text.data() + position_token.text.size(), position);
        if (position_token.text.empty() ||
            (position_token.text.size() > 1 &&
             position_token.text.front() == '0') ||
            error != std::errc{} ||
            end != position_token.text.data() + position_token.text.size() ||
            position == 0 ||
            position > static_cast<std::uint64_t>(
                           std::numeric_limits<std::int64_t>::max())) {
          Refuse("nth_value_position_invalid",
                 "NTH_VALUE position must be a canonical positive signed-int64 literal");
          return FinishRefusal();
        }
        NativeExpressionAstNode position_operand;
        position_operand.expression_id = NextExpressionId();
        position_operand.expression_kind = NativeExpressionAstKind::kLiteral;
        position_operand.literal_kind = NativeLiteralAstKind::kNumeric;
        position_operand.spelling = position_token.text;
        position_operand.range = TokenSourceRange(position_token);
        nth_position_expression_id = position_operand.expression_id;
        document_.expressions.push_back(std::move(position_operand));
        if (!AtEnd() &&
            (IsWord(Current(), "FROM") || IsWord(Current(), "RESPECT") ||
             IsWord(Current(), "IGNORE"))) {
          Refuse("nth_value_option_not_available",
                 "bounded NTH_VALUE accepts omitted FROM FIRST and RESPECT NULLS only");
          return FinishRefusal();
        }
      }
    }
    if (!RequireSymbol(")", "window_function_close_required",
                       function_name + " requires a closing parenthesis")) {
      return FinishRefusal();
    }
    const Token& function_close = Previous();
    if (nth_value_window && !AtEnd() &&
        (IsWord(Current(), "FROM") || IsWord(Current(), "RESPECT") ||
         IsWord(Current(), "IGNORE"))) {
      Refuse("nth_value_option_not_available",
             "bounded NTH_VALUE accepts omitted FROM FIRST and RESPECT NULLS only");
      return FinishRefusal();
    }
    if (!RequireWord("OVER", "window_over_required",
                     function_name + " requires OVER")) {
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

    auto& stored_function =
        document_.expressions[function_expression_id - 1];
    if (ntile_operand_expression_id.has_value()) {
      stored_function.child_expression_ids = {*ntile_operand_expression_id};
    } else if (navigation_operand_expression_id.has_value()) {
      stored_function.child_expression_ids = {*navigation_operand_expression_id};
      if (nth_position_expression_id.has_value()) {
        stored_function.child_expression_ids.push_back(
            *nth_position_expression_id);
      }
    }
    stored_function.spelling = SourceSpelling(function_token, function_close);
    stored_function.range = Span(function_token, function_close);

    NativeWindowDefinitionAstNode definition;
    definition.window_id = 1;
    std::vector<std::uint32_t> source_expression_ids;
    if (navigation_operand_expression_id.has_value()) {
      source_expression_ids.push_back(*navigation_operand_expression_id);
    }
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
               "typed ranking requires PARTITION BY or ORDER BY");
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
              : (aggregate_window
                     ? ToLowerAscii(function_name)
                     : (value_window
                     ? std::string(first_value_window
                                       ? "first_value"
                                       : (last_value_window
                                              ? "last_value"
                                              : (nth_value_window
                                                     ? "nth_value"
                                                     : (lag_window ? "lag"
                                                                   : "lead"))))
                     : (ntile_window
                     ? std::string("ntile")
                     : (cume_dist_window
                     ? std::string("cume_dist")
                     : (percent_rank_window
                            ? std::string("percent_rank")
                            : (dense_rank_window
                                   ? std::string("dense_rank")
                                   : (rank_window
                                          ? std::string("rank")
                                          : std::string("row_number")))))))));
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
    if (strict_ordered_window) {
      if (document_.window_definitions.size() != 1) {
        Refuse(aggregate_window
                   ? "aggregate_window_shape_unsupported"
                   : (value_window
                   ? (first_value_window
                          ? "first_value_window_shape_unsupported"
                          : (last_value_window
                                 ? "last_value_window_shape_unsupported"
                                 : (nth_value_window
                                        ? "nth_value_window_shape_unsupported"
                                        : (lag_window
                                               ? "lag_window_shape_unsupported"
                                               : "lead_window_shape_unsupported"))))
                   : (ntile_window
                   ? "ntile_window_shape_unsupported"
                   : (cume_dist_window
                   ? "cume_dist_window_shape_unsupported"
                   : (percent_rank_window
                          ? "percent_rank_window_shape_unsupported"
                          : (dense_rank_window
                                 ? "dense_rank_window_shape_unsupported"
                                 : "rank_window_shape_unsupported"))))),
               "typed " + function_name +
                   " requires one inline direct-column ORDER BY key");
        return FinishRefusal();
      }
      const auto& rank_definition = document_.window_definitions.front();
      if (named_reference != nullptr || qualify_predicate_expression_id.has_value() ||
          rank_definition.name.has_value() ||
          rank_definition.base_name.has_value() ||
          !rank_definition.partition_expression_ids.empty() ||
          rank_definition.ordering_terms.size() != 1 ||
          rank_definition.frame_unit.has_value() ||
          rank_definition.frame_start.has_value() ||
          rank_definition.frame_end.has_value() ||
          rank_definition.exclusion !=
              NativeWindowFrameExclusion::kNoOthers) {
        Refuse(aggregate_window
                   ? "aggregate_window_shape_unsupported"
                   : (value_window
                   ? (first_value_window
                          ? "first_value_window_shape_unsupported"
                          : (last_value_window
                                 ? "last_value_window_shape_unsupported"
                                 : (nth_value_window
                                        ? "nth_value_window_shape_unsupported"
                                        : (lag_window
                                               ? "lag_window_shape_unsupported"
                                               : "lead_window_shape_unsupported"))))
                   : (ntile_window
                   ? "ntile_window_shape_unsupported"
                   : (cume_dist_window
                   ? "cume_dist_window_shape_unsupported"
                   : (percent_rank_window
                          ? "percent_rank_window_shape_unsupported"
                          : (dense_rank_window
                                 ? "dense_rank_window_shape_unsupported"
                                 : "rank_window_shape_unsupported"))))),
               "typed " + function_name +
                   " requires one inline direct-column ORDER BY key");
        return FinishRefusal();
      }
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
    if (tokens_.size() < 7) return false;
    std::size_t from_index = 1;
    if (tokens_[from_index]->text == "*") {
      ++from_index;
    } else {
      bool expect_identifier = true;
      while (from_index < tokens_.size()) {
        if (expect_identifier) {
          if (tokens_[from_index]->kind != TokenKind::kIdentifier) return false;
          ++from_index;
          expect_identifier = false;
        } else if (tokens_[from_index]->text == ",") {
          ++from_index;
          expect_identifier = true;
        } else {
          break;
        }
      }
      if (expect_identifier) return false;
    }
    if (from_index >= tokens_.size() ||
        !IsWord(*tokens_[from_index], "FROM")) {
      return false;
    }
    for (std::size_t index = from_index + 1;
         index + 1 < tokens_.size(); ++index) {
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
    bool wildcard_projection = false;
    std::vector<std::uint32_t> projection_expression_ids;
    if (AtSymbol("*")) {
      Consume();
      wildcard_projection = true;
    } else {
      std::unordered_set<std::string> projection_names;
      while (true) {
        if (AtEnd() || Current().kind != TokenKind::kIdentifier) {
          Refuse("catalog_join_projection_identifier_required",
                 "bounded catalog JOIN projection requires an unqualified "
                 "column identifier");
          return FinishRefusal();
        }
        const Token& identifier_token = Consume();
        auto projection_key = identifier_token.text;
        if (!identifier_token.quoted) {
          for (auto& ch : projection_key) {
            if (ch >= 'A' && ch <= 'Z') {
              ch = static_cast<char>(ch - 'A' + 'a');
            }
          }
        }
        if (!projection_names.insert(std::move(projection_key)).second) {
          Refuse("catalog_join_projection_identifier_duplicate",
                 "bounded catalog JOIN projection identifiers must be unique");
          return FinishRefusal();
        }
        NativeExpressionAstNode identifier;
        identifier.expression_id = NextExpressionId();
        identifier.expression_kind = NativeExpressionAstKind::kIdentifier;
        identifier.qualified_identifier.push_back(NativeIdentifierAstNode{
            identifier_token.text, identifier_token.quoted,
            TokenSourceRange(identifier_token)});
        identifier.spelling = identifier_token.text;
        identifier.range = TokenSourceRange(identifier_token);
        projection_expression_ids.push_back(identifier.expression_id);
        document_.expressions.push_back(std::move(identifier));
        if (!AtSymbol(",")) break;
        Consume();
      }
    }
    if (!RequireWord("FROM", "catalog_cross_join_from_required",
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
      const std::array<std::tuple<std::string_view,
                                  NativeRelationSourceAstKind,
                                  std::string_view,
                                  std::string_view,
                                  std::string_view>, 8>
          model_sources = {{
              {"DOCUMENT_SOURCE", NativeRelationSourceAstKind::kDocument,
               "document", "DOCUMENT_FIND", "DOCUMENT_SOURCE"},
              {"GRAPH_SOURCE", NativeRelationSourceAstKind::kGraph, "graph",
               "GRAPH_MATCH", "GRAPH_MATCH"},
              {"KEY_VALUE_SOURCE", NativeRelationSourceAstKind::kKeyValue,
               "key_value", "KEY_VALUE_GET", "KV_KEY"},
              {"TIME_SERIES_SOURCE", NativeRelationSourceAstKind::kTimeSeries,
               "time_series", "TIME_SERIES_RANGE_READ", "TIME_RANGE"},
              {"VECTOR_SOURCE", NativeRelationSourceAstKind::kVector, "vector",
               "VECTOR_EXACT_SEARCH", "VECTOR_NEAREST"},
              {"SEARCH_SOURCE", NativeRelationSourceAstKind::kSearch, "search",
               "SEARCH_RANKED_QUERY", "SEARCH_MATCH"},
              {"SPATIAL_SOURCE", NativeRelationSourceAstKind::kSpatial,
               "spatial", "SPATIAL_SOURCE", "SPATIAL_SOURCE"},
              {"COLUMNAR_SOURCE", NativeRelationSourceAstKind::kColumnar,
               "columnar", "COLUMNAR_SOURCE", "COLUMNAR_SOURCE"},
          }};
      const auto model_profile = std::ranges::find_if(
          model_sources, [&](const auto& candidate) {
            return IsWord(Current(), std::get<0>(candidate));
          });
      const bool model_source = model_profile != model_sources.end();
      source.source_kind =
          model_source ? std::get<1>(*model_profile)
                       : NativeRelationSourceAstKind::kCatalogRelation;
      const Token* model_operator = nullptr;
      if (model_source) {
        model_operator = &Consume();
        if (!RequireSymbol("(", "model_join_source_open_required",
                           "model JOIN source requires an opening parenthesis") ||
            AtEnd() || !IsNameToken(Current())) {
          RefuseExact("SB_MODEL_BINDING_INCOMPLETE_V1",
                      "model JOIN source requires a qualified object name");
          return std::nullopt;
        }
      }
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
      const Token* source_end = last;
      if (model_source) {
        if (!RequireSymbol(")", "model_join_source_close_required",
                           "model JOIN source requires a closing parenthesis")) {
          return std::nullopt;
        }
        source_end = &Previous();
        if (!AtEnd() && IsWord(Current(), "AS")) {
          Consume();
          if (AtEnd() || !IsNameToken(Current())) {
            RefuseExact("SB_MODEL_BINDING_INCOMPLETE_V1",
                        "model JOIN source AS requires an alias");
            return std::nullopt;
          }
          const Token& alias = Consume();
          source.alias = NativeIdentifierAstNode{
              alias.text, alias.quoted, TokenSourceRange(alias)};
          source.alias_is_explicit = true;
          source_end = &alias;
        } else {
          source.alias = NativeIdentifierAstNode{
              last->text, last->quoted, TokenSourceRange(*last)};
        }
        source.model_family_id = std::string(std::get<2>(*model_profile));
        source.model_operation_id = std::string(std::get<3>(*model_profile));
        // A bounded composition carries the same explicit operation closure as
        // the established single-family grammar.  Only the source operations
        // whose complete semantics are present in the table-primary itself are
        // emitted here.  Graph/KV/time/vector/search roots are parsed from the
        // final source-local WHERE inventory below; synthesizing them without
        // their operands would create a different operation.
        if (source.source_kind == NativeRelationSourceAstKind::kDocument) {
          NativeExpressionAstNode alias;
          alias.expression_id = NextExpressionId();
          alias.expression_kind = NativeExpressionAstKind::kIdentifier;
          alias.qualified_identifier.push_back(*source.alias);
          alias.spelling = source.alias->spelling;
          alias.range = source.alias->range;
          const auto alias_id = alias.expression_id;
          document_.expressions.push_back(std::move(alias));

          NativeExpressionAstNode operation;
          operation.expression_id = NextExpressionId();
          operation.expression_kind = NativeExpressionAstKind::kFunctionCall;
          operation.child_expression_ids = {alias_id};
          operation.operator_name = "DOCUMENT_SOURCE";
          operation.spelling = SourceSpelling(*model_operator, *source_end);
          operation.range = Span(*model_operator, *source_end);
          source.model_operation_ids = {operation.operator_name};
          source.model_operation_expression_ids = {operation.expression_id};
          source.model_document_expression_id = alias_id;
          document_.expressions.push_back(std::move(operation));
        } else if (source.source_kind == NativeRelationSourceAstKind::kSpatial ||
                   source.source_kind == NativeRelationSourceAstKind::kColumnar) {
          NativeExpressionAstNode operation;
          operation.expression_id = NextExpressionId();
          operation.expression_kind = NativeExpressionAstKind::kFunctionCall;
          operation.operator_name = std::string(std::get<4>(*model_profile));
          operation.spelling = SourceSpelling(*model_operator, *source_end);
          operation.range = Span(*model_operator, *source_end);
          source.model_operation_ids = {operation.operator_name};
          source.model_operation_expression_ids = {operation.expression_id};
          document_.expressions.push_back(std::move(operation));
        }
      }
      source.range = model_source ? Span(*model_operator, *source_end)
                                  : source.qualified_name_range;
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
    const auto parse_join_key = [&](const auto& source,
                                    const char* missing_detail)
        -> std::optional<std::pair<const Token*, const Token*>> {
      if (AtEnd() || Current().kind != TokenKind::kIdentifier) {
        Refuse("catalog_inner_join_key_required", missing_detail);
        return std::nullopt;
      }
      const Token* first = &Consume();
      const Token* key = first;
      if (AtSymbol(".")) {
        Consume();
        if (AtEnd() || Current().kind != TokenKind::kIdentifier) {
          Refuse("catalog_inner_join_qualified_key_incomplete",
                 "bounded catalog JOIN qualified key is incomplete");
          return std::nullopt;
        }
        key = &Consume();
        const auto expected_alias =
            source.alias.has_value() ? &*source.alias
                                     : &source.qualified_name.back();
        const bool alias_matches =
            first->quoted == expected_alias->quoted &&
            (first->quoted
                 ? first->text == expected_alias->spelling
                 : ToLowerAscii(first->text) ==
                       ToLowerAscii(expected_alias->spelling));
        if (!alias_matches) {
          Refuse("catalog_inner_join_qualifier_mismatch",
                 "bounded catalog JOIN key qualifier does not match its leg");
          return std::nullopt;
        }
      }
      return std::pair<const Token*, const Token*>{first, key};
    };
    const auto parse_join_comparison = [&]()
        -> std::optional<std::size_t> {
      ParsedJoinPredicateNode comparison;
      comparison.comparison = true;
      const auto left_key = parse_join_key(
          *left_source,
          "bounded catalog JOIN requires a left key identifier");
      if (!left_key.has_value()) return std::nullopt;
      comparison.first = left_key->first;
      comparison.left_key = left_key->second;
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
      const auto right_key = parse_join_key(
          *right_source,
          "bounded catalog JOIN requires a right key identifier");
      if (!right_key.has_value()) return std::nullopt;
      comparison.right_key = right_key->second;
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
    std::vector<NativeCatalogRelationSourceAstNode> parsed_sources;
    parsed_sources.push_back(std::move(*left_source));
    parsed_sources.push_back(std::move(*right_source));
    if (join_kind == NativeJoinAstKind::kCross) {
      while (!AtEnd() && IsWord(Current(), "CROSS")) {
        Consume();
        if (!RequireWord("JOIN", "catalog_join_join_required",
                         "bounded catalog JOIN requires JOIN") ||
            parsed_sources.size() == 9) {
          if (parsed_sources.size() == 9) {
            RefuseExact("SB_MODEL_COMPOSITION_PROFILE_REFUSED_V1",
                        "bounded multi-source JOIN admits at most nine sources");
          }
          return FinishRefusal();
        }
        auto next_source =
            parse_source(static_cast<std::uint32_t>(parsed_sources.size() + 1));
        if (!next_source.has_value()) return FinishRefusal();
        parsed_sources.push_back(std::move(*next_source));
      }
    }
    std::vector<std::uint32_t> source_predicate_ids(parsed_sources.size());
    const Token* query_end =
        join_kind != NativeJoinAstKind::kCross
            ? predicate_nodes[*predicate_root].last
            : &TokenForRangeEnd(parsed_sources.back().range);
    if (join_kind == NativeJoinAstKind::kCross) {
      const auto expression_at = [&](const std::uint32_t id)
          -> NativeExpressionAstNode* {
        return id == 0 || id > document_.expressions.size()
                   ? nullptr
                   : &document_.expressions[id - 1];
      };
      const auto same_source_alias = [&](const NativeExpressionAstNode* expression,
                                         const auto& source) {
        if (expression == nullptr || !source.alias.has_value() ||
            expression->expression_kind !=
                NativeExpressionAstKind::kIdentifier ||
            expression->qualified_identifier.size() != 1) {
          return false;
        }
        const auto& presented = expression->qualified_identifier.front();
        return presented.quoted == source.alias->quoted &&
               (presented.quoted
                    ? presented.spelling == source.alias->spelling
                    : ToLowerAscii(presented.spelling) ==
                          ToLowerAscii(source.alias->spelling));
      };
      const auto parse_explicit_model_root = [&](auto& source)
          -> std::optional<std::uint32_t> {
        const auto parsed_id = ParseExpression(3, 0);
        if (!parsed_id.has_value()) return std::nullopt;
        auto* parsed = expression_at(*parsed_id);
        if (parsed == nullptr) return std::nullopt;
        NativeExpressionAstNode* root = parsed;
        if (source.source_kind == NativeRelationSourceAstKind::kKeyValue) {
          if (parsed->expression_kind != NativeExpressionAstKind::kBinary ||
              parsed->operator_name != "=" ||
              parsed->child_expression_ids.size() != 2) {
            RefuseExact("SB_MODEL_KEY_VALUE_OPERATOR_REFUSED_V1",
                        "multimodel KV_KEY requires one exact equality");
            return std::nullopt;
          }
          root = expression_at(parsed->child_expression_ids.front());
          const auto* key = expression_at(parsed->child_expression_ids.back());
          if (root == nullptr || key == nullptr ||
              !((key->expression_kind == NativeExpressionAstKind::kLiteral &&
                 key->literal_kind == NativeLiteralAstKind::kString) ||
                key->expression_kind == NativeExpressionAstKind::kParameter)) {
            RefuseExact("SB_MODEL_OPERATION_SEMANTIC_REFUSED_V1",
                        "multimodel KV_KEY requires one typed TEXT key");
            return std::nullopt;
          }
          source.model_key_expression_ids = {key->expression_id};
          source.model_comparison_operator = "=";
        }
        if (root->expression_kind != NativeExpressionAstKind::kFunctionCall) {
          RefuseExact("SB_MODEL_OPERATION_SEMANTIC_REFUSED_V1",
                      "multimodel source operation is not a functionless root");
          return std::nullopt;
        }
        const auto canonical_root = ToUpperAscii(root->operator_name);
        root->operator_name = canonical_root;
        const auto expected_root =
            source.source_kind == NativeRelationSourceAstKind::kGraph
                ? std::string_view{"GRAPH_MATCH"}
            : source.source_kind == NativeRelationSourceAstKind::kKeyValue
                ? std::string_view{"KV_KEY"}
            : source.source_kind == NativeRelationSourceAstKind::kTimeSeries
                ? std::string_view{"TIME_RANGE"}
            : source.source_kind == NativeRelationSourceAstKind::kVector
                ? std::string_view{"VECTOR_NEAREST"}
            : source.source_kind == NativeRelationSourceAstKind::kSearch
                ? std::string_view{"SEARCH_MATCH"}
                : std::string_view{};
        const auto expected_arity =
            source.source_kind == NativeRelationSourceAstKind::kGraph ? 2U
            : source.source_kind == NativeRelationSourceAstKind::kKeyValue ? 1U
            : source.source_kind == NativeRelationSourceAstKind::kTimeSeries ? 3U
                                                                           : 4U;
        if (canonical_root != expected_root ||
            root->child_expression_ids.size() != expected_arity ||
            !same_source_alias(expression_at(root->child_expression_ids.front()),
                               source)) {
          RefuseExact("SB_MODEL_OPERATION_SEMANTIC_REFUSED_V1",
                      "multimodel operation root alias, identity, or arity is invalid");
          return std::nullopt;
        }

        if (source.source_kind == NativeRelationSourceAstKind::kGraph) {
          auto* pattern = expression_at(root->child_expression_ids[1]);
          if (pattern == nullptr ||
              pattern->expression_kind != NativeExpressionAstKind::kLiteral ||
              pattern->literal_kind != NativeLiteralAstKind::kString ||
              !ExactBoundedGraphPatternV1(pattern->spelling)) {
            RefuseExact("SB_MODEL_BINDING_INCOMPLETE_V1",
                        "multimodel GRAPH_MATCH requires one bounded typed pattern");
            return std::nullopt;
          }
          source.model_graph_alias_expression_id =
              root->child_expression_ids.front();
          source.model_pattern_expression_id = pattern->expression_id;
          source.model_graph_cycle_policy = "visited_set";
        } else if (source.source_kind ==
                   NativeRelationSourceAstKind::kTimeSeries) {
          auto* start = expression_at(root->child_expression_ids[1]);
          auto* end = expression_at(root->child_expression_ids[2]);
          if (start == nullptr || end == nullptr ||
              start->expression_kind != NativeExpressionAstKind::kLiteral ||
              start->literal_kind != NativeLiteralAstKind::kTemporal ||
              end->expression_kind != NativeExpressionAstKind::kLiteral ||
              end->literal_kind != NativeLiteralAstKind::kTemporal) {
            RefuseExact("SB_MODEL_TIME_SERIES_RANGE_INVALID_V1",
                        "multimodel TIME_RANGE requires typed temporal endpoints");
            return std::nullopt;
          }
          source.model_time_series_alias_expression_id =
              root->child_expression_ids.front();
          source.model_range_expression_id = root->expression_id;
          source.model_range_start_expression_id = start->expression_id;
          source.model_range_end_expression_id = end->expression_id;
        } else if (source.source_kind == NativeRelationSourceAstKind::kVector) {
          auto* query = expression_at(root->child_expression_ids[1]);
          auto* metric = expression_at(root->child_expression_ids[2]);
          auto* top_k = expression_at(root->child_expression_ids[3]);
          if (query == nullptr || metric == nullptr || top_k == nullptr ||
              !((query->expression_kind == NativeExpressionAstKind::kLiteral &&
                 query->literal_kind == NativeLiteralAstKind::kVector) ||
                query->expression_kind == NativeExpressionAstKind::kParameter) ||
              metric->expression_kind != NativeExpressionAstKind::kIdentifier ||
              metric->qualified_identifier.size() != 1 ||
              metric->qualified_identifier.front().quoted ||
              top_k->expression_kind != NativeExpressionAstKind::kLiteral ||
              top_k->literal_kind != NativeLiteralAstKind::kNumeric) {
            RefuseExact("SB_MODEL_VECTOR_VALUE_REFUSED_V1",
                        "multimodel VECTOR_NEAREST operands are not exact");
            return std::nullopt;
          }
          const auto metric_id =
              ToUpperAscii(metric->qualified_identifier.front().spelling);
          std::uint64_t top_k_value = 0;
          const auto converted = std::from_chars(
              top_k->spelling.data(),
              top_k->spelling.data() + top_k->spelling.size(), top_k_value);
          if ((metric_id != "L2_SQUARED" && metric_id != "COSINE" &&
               metric_id != "INNER_PRODUCT") ||
              converted.ec != std::errc{} ||
              converted.ptr != top_k->spelling.data() + top_k->spelling.size() ||
              top_k_value == 0 || top_k_value > 0xffffffffULL) {
            RefuseExact("SB_MODEL_VECTOR_METRIC_REFUSED_V1",
                        "multimodel vector metric or top-k is invalid");
            return std::nullopt;
          }
          metric->expression_kind = NativeExpressionAstKind::kLiteral;
          metric->literal_kind = NativeLiteralAstKind::kString;
          metric->qualified_identifier.clear();
          metric->spelling = metric_id;
          source.model_vector_alias_expression_id =
              root->child_expression_ids.front();
          source.model_vector_nearest_expression_id = root->expression_id;
          source.model_vector_query_expression_id = query->expression_id;
          source.model_vector_metric_expression_id = metric->expression_id;
          source.model_vector_top_k_expression_id = top_k->expression_id;
          source.model_vector_metric_id = metric_id;
          source.model_vector_top_k = top_k_value;
        } else if (source.source_kind == NativeRelationSourceAstKind::kSearch) {
          auto* query = expression_at(root->child_expression_ids[1]);
          auto* analyzer = expression_at(root->child_expression_ids[2]);
          auto* top_k = expression_at(root->child_expression_ids[3]);
          if (query == nullptr || analyzer == nullptr || top_k == nullptr ||
              query->expression_kind != NativeExpressionAstKind::kFunctionCall ||
              ToUpperAscii(query->operator_name) != "SEARCH_TERMS" ||
              query->child_expression_ids.size() != 1 ||
              analyzer->expression_kind != NativeExpressionAstKind::kIdentifier ||
              analyzer->qualified_identifier.empty() ||
              top_k->expression_kind != NativeExpressionAstKind::kLiteral ||
              top_k->literal_kind != NativeLiteralAstKind::kNumeric) {
            RefuseExact("SB_MODEL_SEARCH_QUERY_TYPE_REFUSED_V1",
                        "multimodel SEARCH_MATCH operands are not exact");
            return std::nullopt;
          }
          auto* text = expression_at(query->child_expression_ids.front());
          std::uint64_t top_k_value = 0;
          const auto converted = std::from_chars(
              top_k->spelling.data(),
              top_k->spelling.data() + top_k->spelling.size(), top_k_value);
          if (text == nullptr ||
              !((text->expression_kind == NativeExpressionAstKind::kLiteral &&
                 text->literal_kind == NativeLiteralAstKind::kString) ||
                text->expression_kind == NativeExpressionAstKind::kParameter) ||
              converted.ec != std::errc{} ||
              converted.ptr != top_k->spelling.data() + top_k->spelling.size() ||
              top_k_value == 0 || top_k_value > 0xffffffffULL) {
            RefuseExact("SB_MODEL_SEARCH_TOP_K_REFUSED_V1",
                        "multimodel search query text or top-k is invalid");
            return std::nullopt;
          }
          query->operator_name = "SEARCH_TERMS";
          source.model_search_alias_expression_id =
              root->child_expression_ids.front();
          source.model_search_match_expression_id = root->expression_id;
          source.model_search_query_expression_id = query->expression_id;
          source.model_search_text_expression_id = text->expression_id;
          source.model_search_analyzer_expression_id = analyzer->expression_id;
          source.model_search_top_k_expression_id = top_k->expression_id;
          source.model_search_analyzer_name = analyzer->qualified_identifier;
          source.model_search_query_kind = "SEARCH_TERMS";
          source.model_search_top_k = top_k_value;
        }
        source.model_operation_ids = {canonical_root};
        source.model_operation_expression_ids = {root->expression_id};
        return *parsed_id;
      };

      std::vector<std::size_t> explicit_source_ordinals;
      for (std::size_t ordinal = 0; ordinal < parsed_sources.size(); ++ordinal) {
        const auto kind = parsed_sources[ordinal].source_kind;
        if (kind == NativeRelationSourceAstKind::kGraph ||
            kind == NativeRelationSourceAstKind::kKeyValue ||
            kind == NativeRelationSourceAstKind::kTimeSeries ||
            kind == NativeRelationSourceAstKind::kVector ||
            kind == NativeRelationSourceAstKind::kSearch) {
          explicit_source_ordinals.push_back(ordinal);
        }
      }
      if (!explicit_source_ordinals.empty()) {
        if (!RequireWord("WHERE", "model_composition_where_required",
                         "bounded multimodel composition requires explicit source operations")) {
          return FinishRefusal();
        }
        for (std::size_t operation_ordinal = 0;
             operation_ordinal < explicit_source_ordinals.size();
             ++operation_ordinal) {
          if (operation_ordinal != 0 &&
              !RequireWord("AND", "model_composition_operation_separator_required",
                           "multimodel source operations require lexical AND separation")) {
            return FinishRefusal();
          }
          const auto source_ordinal = explicit_source_ordinals[operation_ordinal];
          const auto predicate =
              parse_explicit_model_root(parsed_sources[source_ordinal]);
          if (!predicate.has_value()) return FinishRefusal();
          source_predicate_ids[source_ordinal] = *predicate;
          query_end = &TokenForRangeEnd(
              document_.expressions[*predicate - 1].range);
        }
      }
    }
    const Token* join_end = query_end;
    std::optional<std::uint32_t> join_filter_predicate_id;
    const auto parsed_model_source_count = std::ranges::count_if(
        parsed_sources, [](const auto& source) {
          return source.source_kind !=
                 NativeRelationSourceAstKind::kCatalogRelation;
        });
    const bool all_ordinary_catalog_sources =
        std::ranges::all_of(parsed_sources, [](const auto& source) {
          return source.source_kind ==
                 NativeRelationSourceAstKind::kCatalogRelation;
        });
    const bool ordinary_two_source_join =
        parsed_sources.size() == 2 && all_ordinary_catalog_sources;
    const bool ordinary_multi_catalog_cross_join =
        parsed_sources.size() >= 3 && parsed_sources.size() <= 9 &&
        join_kind == NativeJoinAstKind::kCross &&
        parsed_model_source_count == 0 && all_ordinary_catalog_sources;
    if (!wildcard_projection && !ordinary_two_source_join &&
        !ordinary_multi_catalog_cross_join) {
      Refuse("catalog_join_projection_profile_unsupported",
             "bounded catalog JOIN identifier projection requires two "
             "ordinary sources or a three-to-nine-source catalog CROSS JOIN");
      return FinishRefusal();
    }
    if (ordinary_two_source_join && !AtEnd() && IsWord(Current(), "WHERE")) {
      Consume();
      join_filter_predicate_id = ParseExpression(0, 0);
      if (!join_filter_predicate_id.has_value()) return FinishRefusal();
      const auto& predicate =
          document_.expressions[*join_filter_predicate_id - 1];
      const NativeExpressionAstNode* identifier = nullptr;
      const NativeExpressionAstNode* value = nullptr;
      if (predicate.expression_kind == NativeExpressionAstKind::kBinary &&
          predicate.child_expression_ids.size() == 2) {
        identifier =
            &document_.expressions[predicate.child_expression_ids[0] - 1];
        value = &document_.expressions[predicate.child_expression_ids[1] - 1];
      }
      const bool accepted_operator =
          predicate.operator_name == "=" || predicate.operator_name == "<>" ||
          predicate.operator_name == "!=" || predicate.operator_name == "<" ||
          predicate.operator_name == "<=" || predicate.operator_name == ">" ||
          predicate.operator_name == ">=";
      if (identifier == nullptr || value == nullptr || !accepted_operator ||
          identifier->expression_kind != NativeExpressionAstKind::kIdentifier ||
          identifier->qualified_identifier.size() != 1 ||
          !identifier->child_expression_ids.empty() ||
          !((value->expression_kind == NativeExpressionAstKind::kLiteral &&
             value->literal_kind == NativeLiteralAstKind::kNumeric) ||
            value->expression_kind == NativeExpressionAstKind::kParameter ||
            value->expression_kind == NativeExpressionAstKind::kVariable) ||
          !value->child_expression_ids.empty()) {
        Refuse("catalog_join_where_profile_unsupported",
               "bounded catalog JOIN WHERE requires one unqualified column "
               "comparison to an unsigned numeric literal, structural "
               "parameter occurrence, or structural variable occurrence");
        return FinishRefusal();
      }
      query_end = &TokenForRangeEnd(predicate.range);
    }
    if (AtSymbol(";")) Consume();
    if (!AtEnd()) {
      Refuse("catalog_cross_join_clause_unsupported",
             "bounded catalog CROSS JOIN does not admit aliases or clauses");
      return FinishRefusal();
    }

    const bool bounded_multimodel_join =
        parsed_sources.size() >= 3 && parsed_sources.size() <= 9 &&
        parsed_model_source_count >= 2;
    if (parsed_sources.size() > 2 &&
        !ordinary_multi_catalog_cross_join && !bounded_multimodel_join) {
      RefuseExact("SB_MODEL_COMPOSITION_PROFILE_REFUSED_V1",
                  "multi-source JOIN requires either three to nine ordinary "
                  "catalog CROSS JOIN legs or at least two model-family legs");
      return FinishRefusal();
    }
    std::vector<std::uint32_t> source_wildcard_ids;
    for (std::uint32_t source_id = 1;
         source_id <= parsed_sources.size(); ++source_id) {
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
      if (source_predicate_ids[source_id - 1] != 0) {
        source_relation.predicate_expression_ids = {
            source_predicate_ids[source_id - 1]};
      }
      source_relation.range = parsed_sources[source_id - 1].range;
      document_.relations.push_back(std::move(source_relation));
    }
    std::vector<std::uint32_t> joined_wildcard_ids;
    joined_wildcard_ids.push_back(source_wildcard_ids.front());
    std::uint32_t prior_relation_id = 1;
    for (std::size_t source_ordinal = 1;
         source_ordinal < parsed_sources.size(); ++source_ordinal) {
      NativeRelationAstNode join;
      join.relation_id = static_cast<std::uint32_t>(
          parsed_sources.size() + source_ordinal);
      join.relation_kind = NativeRelationAstKind::kJoin;
      join.join_kind = join_kind;
      join.input_relation_ids = {
          prior_relation_id, static_cast<std::uint32_t>(source_ordinal + 1)};
      if (join_kind == NativeJoinAstKind::kLeftSemi ||
          join_kind == NativeJoinAstKind::kLeftAnti) {
        join.output_expression_ids = joined_wildcard_ids;
      } else {
        joined_wildcard_ids.push_back(source_wildcard_ids[source_ordinal]);
        join.output_expression_ids = joined_wildcard_ids;
      }
      join.range = Span(select_token, *join_end);
      if (source_ordinal + 1 == parsed_sources.size() &&
          join_kind != NativeJoinAstKind::kCross) {
        std::vector<std::uint32_t> predicate_expression_ids(
            predicate_nodes.size());
        for (std::size_t node_ordinal = 0;
             node_ordinal < predicate_nodes.size(); ++node_ordinal) {
          const auto& parsed = predicate_nodes[node_ordinal];
          if (parsed.comparison) {
            NativeExpressionAstNode left_key;
            left_key.expression_id = NextExpressionId();
            left_key.expression_kind = NativeExpressionAstKind::kIdentifier;
            left_key.qualified_identifier.push_back(NativeIdentifierAstNode{
                parsed.left_key->text, parsed.left_key->quoted,
                TokenSourceRange(*parsed.left_key)});
            left_key.spelling = parsed.left_key->text;
            left_key.range = TokenSourceRange(*parsed.left_key);
            const auto left_key_id = left_key.expression_id;
            document_.expressions.push_back(std::move(left_key));

            NativeExpressionAstNode right_key;
            right_key.expression_id = NextExpressionId();
            right_key.expression_kind = NativeExpressionAstKind::kIdentifier;
            right_key.qualified_identifier.push_back(NativeIdentifierAstNode{
                parsed.right_key->text, parsed.right_key->quoted,
                TokenSourceRange(*parsed.right_key)});
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
      prior_relation_id = join.relation_id;
      document_.relations.push_back(std::move(join));
    }
    document_.catalog_relation_sources = std::move(parsed_sources);
    document_.root_relation_id = prior_relation_id;
    if (join_filter_predicate_id.has_value()) {
      NativeRelationAstNode filter;
      filter.relation_id = prior_relation_id + 1;
      filter.relation_kind = NativeRelationAstKind::kFilter;
      filter.input_relation_ids = {prior_relation_id};
      filter.output_expression_ids = joined_wildcard_ids;
      filter.predicate_expression_ids = {*join_filter_predicate_id};
      filter.range = Span(select_token, *query_end);
      document_.relations.push_back(std::move(filter));
      document_.root_relation_id = document_.relations.back().relation_id;
    }
    if (!wildcard_projection) {
      NativeRelationAstNode project;
      project.relation_id = document_.root_relation_id + 1;
      project.relation_kind = NativeRelationAstKind::kProject;
      project.input_relation_ids = {document_.root_relation_id};
      project.output_expression_ids = projection_expression_ids;
      project.range = Span(select_token, *query_end);
      document_.relations.push_back(std::move(project));
      document_.root_relation_id = document_.relations.back().relation_id;
    }
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
          !((right->expression_kind == NativeExpressionAstKind::kLiteral &&
            right->literal_kind == NativeLiteralAstKind::kNumeric) ||
            right->expression_kind == NativeExpressionAstKind::kParameter ||
            right->expression_kind == NativeExpressionAstKind::kVariable) ||
          !left->child_expression_ids.empty() ||
          !right->child_expression_ids.empty()) {
        Refuse("catalog_select_where_profile_unsupported",
               "bounded catalog WHERE requires an identifier comparison to "
               "an unsigned numeric literal, structural parameter occurrence, or structural variable occurrence");
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

    if (first.kind == TokenKind::kVariable) {
      Consume();
      NativeExpressionAstNode variable;
      variable.expression_id = NextExpressionId();
      variable.expression_kind = NativeExpressionAstKind::kVariable;
      // Spelling is retained only as source presentation. It is excluded from
      // binding/lowering authority; the engine maps the structural occurrence.
      variable.spelling = first.text;
      variable.range = TokenSourceRange(first);
      document_.expressions.push_back(std::move(variable));
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
  auto document = NativeRelationalParser(cst).Parse();
  std::vector<NativeExpressionAstNode*> literals;
  std::vector<NativeExpressionAstNode*> parameters;
  std::vector<NativeExpressionAstNode*> variables;
  for (auto& expression : document.expressions) {
    if (expression.expression_kind == NativeExpressionAstKind::kLiteral) {
      literals.push_back(&expression);
    } else if (expression.expression_kind == NativeExpressionAstKind::kParameter) {
      parameters.push_back(&expression);
    } else if (expression.expression_kind == NativeExpressionAstKind::kVariable) {
      variables.push_back(&expression);
    }
  }
  std::ranges::sort(literals, {}, [](const auto* expression) {
    return expression->range.offset;
  });
  std::uint64_t occurrence_id = 1;
  for (auto* expression : literals) {
    expression->structural_literal_occurrence_id = occurrence_id++;
  }
  std::ranges::sort(parameters, {}, [](const auto* expression) {
    return expression->range.offset;
  });
  occurrence_id = 1;
  for (auto* expression : parameters) {
    expression->structural_parameter_occurrence_id = occurrence_id++;
  }
  std::ranges::sort(variables, {}, [](const auto* expression) {
    return expression->range.offset;
  });
  occurrence_id = 1;
  for (auto* expression : variables) {
    expression->structural_variable_occurrence_id = occurrence_id++;
  }
  return document;
}

} // namespace scratchbird::parser::sbsql

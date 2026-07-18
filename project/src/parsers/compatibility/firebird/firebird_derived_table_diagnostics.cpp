// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "firebird_dialect.hpp"

#include <algorithm>
#include <cstddef>
#include <limits>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace scratchbird::parser::firebird {
namespace {

constexpr std::size_t kNoIndex = std::numeric_limits<std::size_t>::max();

struct TokenStream {
  std::vector<const Token*> tokens;
  std::vector<std::size_t> depth;
  std::vector<std::size_t> matching_parenthesis;
};

bool IsComment(const Token& token) {
  return token.kind == "line_comment" || token.kind == "block_comment";
}

bool IsWord(const Token& token, std::string_view word) {
  return token.kind == "identifier_or_keyword" &&
         ToUpperAscii(token.lexeme) == word;
}

bool IsIdentifier(const Token& token) {
  return token.kind == "identifier_or_keyword" ||
         token.kind == "quoted_identifier";
}

bool IsPunctuation(const Token& token, std::string_view punctuation) {
  return token.kind == "punctuation" && token.lexeme == punctuation;
}

std::string UnquoteIdentifier(std::string_view identifier) {
  if (identifier.size() < 2 || identifier.front() != '"' ||
      identifier.back() != '"') {
    return ToUpperAscii(identifier);
  }
  std::string value;
  value.reserve(identifier.size() - 2);
  for (std::size_t index = 1; index + 1 < identifier.size(); ++index) {
    if (identifier[index] == '"' && index + 2 < identifier.size() &&
        identifier[index + 1] == '"') {
      value.push_back('"');
      ++index;
      continue;
    }
    value.push_back(identifier[index]);
  }
  return value;
}

std::string CanonicalIdentifier(const Token& token) {
  return token.kind == "quoted_identifier"
             ? UnquoteIdentifier(token.lexeme)
             : ToUpperAscii(token.lexeme);
}

std::string DisplayIdentifier(const Token& token) {
  return token.kind == "quoted_identifier"
             ? UnquoteIdentifier(token.lexeme)
             : ToUpperAscii(token.lexeme);
}

bool IsReservedAliasWord(const Token& token) {
  if (token.kind != "identifier_or_keyword") return false;
  static const std::unordered_set<std::string> kReserved{
      "AS",       "CROSS",  "FETCH",   "FIRST", "FULL",  "GROUP",
      "HAVING",   "INNER",  "JOIN",    "LATERAL", "LEFT", "NATURAL",
      "OFFSET",   "ON",     "ORDER",   "OUTER", "PLAN",  "RIGHT",
      "ROWS",     "UNION",  "WHERE",   "WINDOW", "WITH"};
  return kReserved.contains(ToUpperAscii(token.lexeme));
}

TokenStream BuildTokenStream(std::span<const Token> input) {
  TokenStream stream;
  stream.tokens.reserve(input.size());
  for (const auto& token : input) {
    if (!IsComment(token)) stream.tokens.push_back(&token);
  }
  stream.depth.resize(stream.tokens.size());
  stream.matching_parenthesis.assign(stream.tokens.size(), kNoIndex);
  std::vector<std::size_t> opens;
  for (std::size_t index = 0; index < stream.tokens.size(); ++index) {
    stream.depth[index] = opens.size();
    if (IsPunctuation(*stream.tokens[index], "(")) {
      opens.push_back(index);
    } else if (IsPunctuation(*stream.tokens[index], ")") && !opens.empty()) {
      const auto open = opens.back();
      opens.pop_back();
      stream.matching_parenthesis[open] = index;
      stream.matching_parenthesis[index] = open;
    }
  }
  return stream;
}

bool IsSourceMarker(const Token& token) {
  return IsWord(token, "FROM") || IsWord(token, "JOIN");
}

struct DerivedSource {
  std::size_t open{kNoIndex};
  std::size_t close{kNoIndex};
  std::size_t select{kNoIndex};
  std::size_t alias{kNoIndex};
  bool lateral{false};
};

std::optional<DerivedSource> ClassifyDerivedSource(
    const TokenStream& stream,
    std::size_t open) {
  if (open >= stream.tokens.size() ||
      !IsPunctuation(*stream.tokens[open], "(") ||
      stream.matching_parenthesis[open] == kNoIndex) {
    return std::nullopt;
  }
  const auto close = stream.matching_parenthesis[open];
  if (open + 1 >= close || !IsWord(*stream.tokens[open + 1], "SELECT")) {
    return std::nullopt;
  }

  if (open == 0) return std::nullopt;
  std::size_t marker = open - 1;
  bool lateral = false;
  if (IsWord(*stream.tokens[marker], "LATERAL")) {
    lateral = true;
    if (marker == 0) return std::nullopt;
    --marker;
  }
  if (!IsSourceMarker(*stream.tokens[marker])) return std::nullopt;

  std::size_t alias = close + 1;
  if (alias < stream.tokens.size() && IsWord(*stream.tokens[alias], "AS")) {
    ++alias;
  }
  if (alias >= stream.tokens.size() || !IsIdentifier(*stream.tokens[alias]) ||
      IsReservedAliasWord(*stream.tokens[alias])) {
    return std::nullopt;
  }
  return DerivedSource{open, close, open + 1, alias, lateral};
}

std::optional<std::pair<std::string, std::string>> FirstDuplicateIdentifier(
    const std::vector<const Token*>& identifiers) {
  std::unordered_map<std::string, std::string> seen;
  for (const auto* identifier : identifiers) {
    const auto canonical = CanonicalIdentifier(*identifier);
    const auto display = DisplayIdentifier(*identifier);
    if (seen.contains(canonical)) return std::pair{canonical, display};
    seen.emplace(canonical, display);
  }
  return std::nullopt;
}

struct ExplicitColumnListAnalysis {
  bool present{false};
  std::optional<std::string> duplicate;
};

ExplicitColumnListAnalysis AnalyzeExplicitColumnList(
    const TokenStream& stream,
    const DerivedSource& source) {
  const auto open = source.alias + 1;
  if (open >= stream.tokens.size() ||
      !IsPunctuation(*stream.tokens[open], "(") ||
      stream.matching_parenthesis[open] == kNoIndex) {
    return {};
  }
  ExplicitColumnListAnalysis analysis;
  analysis.present = true;
  const auto close = stream.matching_parenthesis[open];
  std::vector<const Token*> identifiers;
  bool expect_identifier = true;
  for (std::size_t index = open + 1; index < close; ++index) {
    const auto& token = *stream.tokens[index];
    if (expect_identifier) {
      if (!IsIdentifier(token) || IsReservedAliasWord(token)) {
        return analysis;
      }
      identifiers.push_back(&token);
    } else if (!IsPunctuation(token, ",")) {
      return analysis;
    }
    expect_identifier = !expect_identifier;
  }
  if (identifiers.empty() || expect_identifier) return analysis;
  if (const auto duplicate = FirstDuplicateIdentifier(identifiers)) {
    analysis.duplicate = duplicate->second;
  }
  return analysis;
}

std::optional<const Token*> ProjectionOutputName(
    const TokenStream& stream,
    std::size_t begin,
    std::size_t end,
    std::size_t item_depth) {
  if (begin >= end) return std::nullopt;
  for (std::size_t index = begin; index < end; ++index) {
    if (IsPunctuation(*stream.tokens[index], "*")) return std::nullopt;
  }

  for (std::size_t index = end; index-- > begin;) {
    if (stream.depth[index] != item_depth ||
        !IsWord(*stream.tokens[index], "AS")) {
      continue;
    }
    if (index + 2 == end && IsIdentifier(*stream.tokens[index + 1]) &&
        !IsReservedAliasWord(*stream.tokens[index + 1])) {
      return stream.tokens[index + 1];
    }
    return std::nullopt;
  }

  if (end == begin + 1 && IsIdentifier(*stream.tokens[begin]) &&
      !IsReservedAliasWord(*stream.tokens[begin])) {
    return stream.tokens[begin];
  }
  if (end == begin + 3 && IsIdentifier(*stream.tokens[begin]) &&
      IsPunctuation(*stream.tokens[begin + 1], ".") &&
      IsIdentifier(*stream.tokens[begin + 2])) {
    return stream.tokens[begin + 2];
  }
  return std::nullopt;
}

std::optional<std::string> DuplicateProjectedColumnName(
    const TokenStream& stream,
    const DerivedSource& source) {
  const auto content_depth = stream.depth[source.open] + 1;
  std::size_t from = kNoIndex;
  for (std::size_t index = source.select + 1; index < source.close; ++index) {
    if (stream.depth[index] == content_depth &&
        IsWord(*stream.tokens[index], "FROM")) {
      from = index;
      break;
    }
  }
  if (from == kNoIndex) return std::nullopt;

  std::vector<const Token*> names;
  std::size_t item_begin = source.select + 1;
  for (std::size_t index = item_begin; index <= from; ++index) {
    const bool at_end = index == from;
    const bool separator = !at_end && stream.depth[index] == content_depth &&
                           IsPunctuation(*stream.tokens[index], ",");
    if (!at_end && !separator) continue;
    if (const auto name = ProjectionOutputName(
            stream, item_begin, index, content_depth)) {
      names.push_back(*name);
    }
    item_begin = index + 1;
  }
  if (const auto duplicate = FirstDuplicateIdentifier(names)) {
    return duplicate->second;
  }
  return std::nullopt;
}

std::size_t FindContainingSelect(
    const TokenStream& stream,
    const DerivedSource& source) {
  const auto outer_depth = stream.depth[source.open];
  for (std::size_t index = source.open; index-- > 0;) {
    if (stream.depth[index] == outer_depth &&
        IsWord(*stream.tokens[index], "SELECT")) {
      return index;
    }
  }
  return kNoIndex;
}

void CollectRelationAliasAfterMarker(
    const TokenStream& stream,
    std::size_t marker,
    std::size_t end,
    std::size_t required_depth,
    std::unordered_set<std::string>* aliases) {
  if (marker + 1 >= end) return;
  std::size_t index = marker + 1;
  if (IsWord(*stream.tokens[index], "LATERAL")) ++index;
  if (index >= end || stream.depth[index] != required_depth ||
      !IsIdentifier(*stream.tokens[index]) ||
      IsReservedAliasWord(*stream.tokens[index])) {
    return;
  }

  const Token* last_relation_part = stream.tokens[index++];
  while (index + 1 < end && stream.depth[index] == required_depth &&
         IsPunctuation(*stream.tokens[index], ".") &&
         stream.depth[index + 1] == required_depth &&
         IsIdentifier(*stream.tokens[index + 1])) {
    last_relation_part = stream.tokens[index + 1];
    index += 2;
  }

  if (index < end && stream.depth[index] == required_depth &&
      IsWord(*stream.tokens[index], "AS")) {
    ++index;
  }
  if (index < end && stream.depth[index] == required_depth &&
      IsIdentifier(*stream.tokens[index]) &&
      !IsReservedAliasWord(*stream.tokens[index])) {
    aliases->insert(CanonicalIdentifier(*stream.tokens[index]));
  } else {
    aliases->insert(CanonicalIdentifier(*last_relation_part));
  }
}

std::unordered_set<std::string> CollectAliases(
    const TokenStream& stream,
    std::size_t begin,
    std::size_t end,
    std::size_t depth) {
  std::unordered_set<std::string> aliases;
  for (std::size_t index = begin; index < end; ++index) {
    if (stream.depth[index] == depth && IsSourceMarker(*stream.tokens[index])) {
      CollectRelationAliasAfterMarker(
          stream, index, end, depth, &aliases);
    }
  }
  return aliases;
}

std::pair<std::size_t, std::size_t> LineAndColumn(
    std::string_view sql,
    std::size_t offset) {
  offset = std::min(offset, sql.size());
  std::size_t line = 1;
  std::size_t column = 1;
  for (std::size_t index = 0; index < offset; ++index) {
    if (sql[index] == '\n') {
      ++line;
      column = 1;
    } else {
      ++column;
    }
  }
  return {line, column};
}

std::optional<FirebirdDerivedTableDiagnostic> IllegalOuterReference(
    const TokenStream& stream,
    const DerivedSource& source,
    std::string_view original_sql) {
  if (source.lateral) return std::nullopt;
  const auto containing_select = FindContainingSelect(stream, source);
  if (containing_select == kNoIndex) return std::nullopt;

  const auto outer_depth = stream.depth[source.open];
  const auto outer_aliases = CollectAliases(
      stream, containing_select + 1, source.open, outer_depth);
  if (outer_aliases.empty()) return std::nullopt;

  const auto inner_depth = outer_depth + 1;
  const auto inner_aliases = CollectAliases(
      stream, source.select + 1, source.close, inner_depth);
  for (std::size_t index = source.select + 1; index + 2 < source.close;
       ++index) {
    if (stream.depth[index] != inner_depth ||
        !IsIdentifier(*stream.tokens[index]) ||
        !IsPunctuation(*stream.tokens[index + 1], ".") ||
        !IsIdentifier(*stream.tokens[index + 2])) {
      continue;
    }
    const auto qualifier = CanonicalIdentifier(*stream.tokens[index]);
    if (!outer_aliases.contains(qualifier) ||
        inner_aliases.contains(qualifier)) {
      continue;
    }
    const auto qualifier_display = DisplayIdentifier(*stream.tokens[index]);
    const auto field_display = DisplayIdentifier(*stream.tokens[index + 2]);
    // Firebird reports the parser cursor immediately after the qualified
    // field, not the first byte of its relation qualifier.
    const auto& field_token = *stream.tokens[index + 2];
    const auto [line, column] = LineAndColumn(
        original_sql, field_token.offset + field_token.lexeme.size());
    FirebirdDerivedTableDiagnostic diagnostic;
    diagnostic.kind =
        FirebirdDerivedTableDiagnosticKind::kIllegalOuterReference;
    diagnostic.derived_table_alias =
        DisplayIdentifier(*stream.tokens[source.alias]);
    diagnostic.column_name = field_display;
    diagnostic.outer_relation_alias = qualifier_display;
    diagnostic.qualified_field_name =
        qualifier_display + "." + field_display;
    diagnostic.line = line;
    diagnostic.column = column;
    return diagnostic;
  }
  return std::nullopt;
}

}  // namespace

std::optional<FirebirdDerivedTableDiagnostic>
AnalyzeFirebirdDerivedTableDiagnostics(
    std::span<const Token> tokens,
    std::string_view original_sql) {
  const auto stream = BuildTokenStream(tokens);
  for (std::size_t index = 0; index < stream.tokens.size(); ++index) {
    const auto source = ClassifyDerivedSource(stream, index);
    if (!source) continue;

    const auto explicit_columns = AnalyzeExplicitColumnList(stream, *source);
    std::optional<std::string> duplicate = explicit_columns.duplicate;
    // An explicit derived-column list names the output columns. Projected
    // names are irrelevant once that list is present, including when the
    // underlying projection repeats a source identifier.
    if (!explicit_columns.present) {
      duplicate = DuplicateProjectedColumnName(stream, *source);
    }
    if (duplicate) {
      FirebirdDerivedTableDiagnostic diagnostic;
      diagnostic.kind =
          FirebirdDerivedTableDiagnosticKind::kDuplicateOutputName;
      diagnostic.derived_table_alias =
          DisplayIdentifier(*stream.tokens[source->alias]);
      diagnostic.column_name = *duplicate;
      return diagnostic;
    }

    if (const auto outer =
            IllegalOuterReference(stream, *source, original_sql)) {
      return outer;
    }
  }
  return std::nullopt;
}

}  // namespace scratchbird::parser::firebird

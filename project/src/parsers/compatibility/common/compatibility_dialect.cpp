// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "compatibility_dialect.hpp"

#include <array>
#include <cctype>
#include <iomanip>
#include <sstream>
#include <utility>

namespace scratchbird::parser::compatibility {
namespace {

bool StartsWith(std::string_view value, std::string_view prefix) {
  return value.size() >= prefix.size() && value.substr(0, prefix.size()) == prefix;
}

bool IsIdentifierChar(char ch) {
  const auto c = static_cast<unsigned char>(ch);
  return std::isalnum(c) != 0 || ch == '_' || ch == '$';
}

bool IsCommandBoundary(char ch) {
  return std::isspace(static_cast<unsigned char>(ch)) != 0 ||
         ch == ';' || ch == '(' || ch == '\'' || ch == '"' ||
         ch == '`' || ch == '[' || ch == '/' || ch == '-';
}

bool StartsWithCommand(std::string_view value, std::string_view prefix) {
  if (!StartsWith(value, prefix)) return false;
  return value.size() == prefix.size() || IsCommandBoundary(value[prefix.size()]);
}

bool Contains(std::string_view value, std::string_view needle) {
  return value.find(needle) != std::string_view::npos;
}

std::string_view TrimAsciiView(std::string_view text);

bool ContainsDelimitedFragment(std::string_view value,
                               std::string_view fragment) {
  std::size_t pos = value.find(fragment);
  while (pos != std::string_view::npos) {
    const std::size_t end = pos + fragment.size();
    if (fragment.empty() ||
        !IsIdentifierChar(fragment.back()) ||
        end >= value.size() ||
        !IsIdentifierChar(value[end])) {
      return true;
    }
    pos = value.find(fragment, pos + 1);
  }
  return false;
}

bool ContainsWord(std::string_view value, std::string_view word) {
  std::size_t pos = value.find(word);
  while (pos != std::string_view::npos) {
    const bool left_boundary = pos == 0 || !IsIdentifierChar(value[pos - 1]);
    const std::size_t end = pos + word.size();
    const bool right_boundary = end >= value.size() || !IsIdentifierChar(value[end]);
    if (left_boundary && right_boundary) return true;
    pos = value.find(word, pos + 1);
  }
  return false;
}

bool ContainsFunctionCall(std::string_view value,
                          std::string_view function_name) {
  std::size_t pos = value.find(function_name);
  while (pos != std::string_view::npos) {
    const bool left_boundary = pos == 0 || !IsIdentifierChar(value[pos - 1]);
    std::size_t end = pos + function_name.size();
    const bool right_boundary =
        end >= value.size() || !IsIdentifierChar(value[end]);
    while (end < value.size() &&
           std::isspace(static_cast<unsigned char>(value[end])) != 0) {
      ++end;
    }
    if (left_boundary && right_boundary && end < value.size() &&
        value[end] == '(') {
      return true;
    }
    pos = value.find(function_name, pos + 1);
  }
  return false;
}

std::size_t SkipAsciiWhitespace(std::string_view value, std::size_t pos) {
  while (pos < value.size() &&
         std::isspace(static_cast<unsigned char>(value[pos])) != 0) {
    ++pos;
  }
  return pos;
}

bool RelationNameAt(std::string_view value,
                    std::size_t pos,
                    std::string_view relation_name) {
  pos = SkipAsciiWhitespace(value, pos);
  std::size_t token_end = pos;
  while (token_end < value.size() && IsIdentifierChar(value[token_end])) {
    ++token_end;
  }
  if (token_end < value.size() && value[token_end] == '.') {
    const std::size_t relation_pos = token_end + 1;
    if (value.substr(relation_pos, relation_name.size()) != relation_name) {
      return false;
    }
    const std::size_t end = relation_pos + relation_name.size();
    const bool right_boundary =
        end >= value.size() || !IsIdentifierChar(value[end]);
    return right_boundary;
  }
  if (value.substr(pos, relation_name.size()) != relation_name) return false;
  const std::size_t end = pos + relation_name.size();
  return end >= value.size() || !IsIdentifierChar(value[end]);
}

bool ContainsRelationReference(std::string_view value,
                               std::string_view relation_name) {
  constexpr std::string_view kIntroducers[] = {"FROM", "JOIN", "TABLE"};
  for (const auto introducer : kIntroducers) {
    std::size_t pos = value.find(introducer);
    while (pos != std::string_view::npos) {
      const bool left_boundary = pos == 0 || !IsIdentifierChar(value[pos - 1]);
      const std::size_t end = pos + introducer.size();
      const bool right_boundary =
          end >= value.size() || !IsIdentifierChar(value[end]);
      if (left_boundary && right_boundary &&
          RelationNameAt(value, end, relation_name)) {
        return true;
      }
      pos = value.find(introducer, pos + 1);
    }
  }
  return false;
}

bool ConsumeCommand(std::string_view& value, std::string_view keyword) {
  value = TrimAsciiView(value);
  if (!StartsWithCommand(value, keyword)) return false;
  value = TrimAsciiView(value.substr(keyword.size()));
  return true;
}

bool MatchesLoadDataInfileSyntax(std::string_view value, bool local_infile) {
  if (!ConsumeCommand(value, "LOAD")) return false;
  if (!ConsumeCommand(value, "DATA")) return false;
  if (StartsWithCommand(value, "LOW_PRIORITY")) {
    ConsumeCommand(value, "LOW_PRIORITY");
  } else if (StartsWithCommand(value, "CONCURRENT")) {
    ConsumeCommand(value, "CONCURRENT");
  }
  if (local_infile && !ConsumeCommand(value, "LOCAL")) return false;
  return ConsumeCommand(value, "INFILE");
}

bool MatchesCreateTablePrefix(std::string_view value) {
  if (!ConsumeCommand(value, "CREATE")) return false;
  if (StartsWithCommand(value, "OR")) {
    if (!ConsumeCommand(value, "OR")) return false;
    if (!ConsumeCommand(value, "REPLACE")) return false;
  }
  if (StartsWithCommand(value, "TEMPORARY")) {
    if (!ConsumeCommand(value, "TEMPORARY")) return false;
  }
  if (!ConsumeCommand(value, "TABLE")) return false;
  if (StartsWithCommand(value, "IF")) {
    if (!ConsumeCommand(value, "IF")) return false;
    if (!ConsumeCommand(value, "NOT")) return false;
    if (!ConsumeCommand(value, "EXISTS")) return false;
  }
  return true;
}

bool ContainsCreateTableEngineClause(std::string_view value,
                                     std::string_view engine_name) {
  if (!MatchesCreateTablePrefix(value)) return false;
  std::size_t pos = value.find("ENGINE");
  while (pos != std::string_view::npos) {
    const bool left_boundary = pos == 0 || !IsIdentifierChar(value[pos - 1]);
    std::size_t cursor = pos + std::string_view("ENGINE").size();
    const bool right_boundary =
        cursor >= value.size() || !IsIdentifierChar(value[cursor]);
    cursor = SkipAsciiWhitespace(value, cursor);
    if (left_boundary && right_boundary) {
      if (cursor < value.size() && value[cursor] == '=') {
        cursor = SkipAsciiWhitespace(value, cursor + 1);
      }
      if (value.substr(cursor, engine_name.size()) == engine_name) {
        const std::size_t end = cursor + engine_name.size();
        if (end >= value.size() || !IsIdentifierChar(value[end])) {
          return true;
        }
      }
    }
    pos = value.find("ENGINE", pos + 1);
  }
  return false;
}

std::string MaskInactiveSqlText(std::string_view text) {
  std::string masked;
  masked.reserve(text.size());
  for (std::size_t i = 0; i < text.size();) {
    const char ch = text[i];
    const char next = i + 1 < text.size() ? text[i + 1] : '\0';
    if (ch == '-' && next == '-') {
      masked.append(2, ' ');
      i += 2;
      while (i < text.size() && text[i] != '\n') {
        masked.push_back(' ');
        ++i;
      }
      continue;
    }
    if (ch == '#') {
      masked.push_back(' ');
      ++i;
      while (i < text.size() && text[i] != '\n') {
        masked.push_back(' ');
        ++i;
      }
      continue;
    }
    if (ch == '/' && next == '*') {
      masked.append(2, ' ');
      i += 2;
      while (i + 1 < text.size() && !(text[i] == '*' && text[i + 1] == '/')) {
        masked.push_back(' ');
        ++i;
      }
      if (i + 1 < text.size()) {
        masked.append(2, ' ');
        i += 2;
      } else {
        while (i < text.size()) {
          masked.push_back(' ');
          ++i;
        }
      }
      continue;
    }
    if (ch == '\'' || ch == '"' || ch == '`') {
      const char quote = ch;
      masked.push_back(quote);
      ++i;
      while (i < text.size()) {
        if (text[i] == quote && i + 1 < text.size() && text[i + 1] == quote) {
          masked.append(2, ' ');
          i += 2;
          continue;
        }
        if (quote == '\'' && text[i] == '\\' && i + 1 < text.size()) {
          masked.append(2, ' ');
          i += 2;
          continue;
        }
        if (text[i] == quote) {
          masked.push_back(quote);
          ++i;
          break;
        }
        masked.push_back(' ');
        ++i;
      }
      continue;
    }
    masked.push_back(ch);
    ++i;
  }
  return masked;
}

std::string_view TrimAsciiView(std::string_view text);
std::string BoolJson(bool value);

std::string_view ConsumeLeadingCommand(std::string_view value,
                                       std::string_view keyword) {
  value = TrimAsciiView(value);
  if (!StartsWithCommand(value, keyword)) return {};
  return TrimAsciiView(value.substr(keyword.size()));
}

bool IsCreateIndexRest(std::string_view rest) {
  rest = TrimAsciiView(rest);
  bool advanced = true;
  while (advanced) {
    advanced = false;
    for (const auto keyword : {"UNIQUE", "FULLTEXT", "SPATIAL", "ASC",
                               "ASCENDING", "DESC", "DESCENDING"}) {
      if (StartsWithCommand(rest, keyword)) {
        rest = TrimAsciiView(rest.substr(std::string_view(keyword).size()));
        advanced = true;
        break;
      }
    }
  }
  return StartsWithCommand(rest, "INDEX");
}

bool IsCreateIndexKeywordSequence(const std::vector<Token>& tokens) {
  std::vector<std::string> keywords;
  for (const auto& token : tokens) {
    if (token.kind == "line_comment" || token.kind == "block_comment") continue;
    if (token.kind == "identifier_or_keyword") {
      keywords.push_back(ToUpperAscii(token.lexeme));
    }
    if (keywords.size() >= 5) break;
  }
  if (keywords.empty() || keywords[0] != "CREATE") return false;
  for (std::size_t i = 1; i < keywords.size(); ++i) {
    if (keywords[i] == "INDEX") return true;
    if (keywords[i] == "UNIQUE" || keywords[i] == "FULLTEXT" ||
        keywords[i] == "SPATIAL" || keywords[i] == "ASC" ||
        keywords[i] == "ASCENDING" || keywords[i] == "DESC" ||
        keywords[i] == "DESCENDING") {
      continue;
    }
    return false;
  }
  return false;
}

std::string NormalizeCompatibilitySblrOpcode(std::string_view opcode) {
  constexpr std::string_view kCompatPrefix = "SBLR_COMPAT_";
  if (!StartsWith(opcode, kCompatPrefix)) {
    return std::string(opcode);
  }
  if (StartsWith(opcode, "SBLR_COMPATIBILITY_")) {
    return std::string(opcode);
  }
  return "SBLR_COMPATIBILITY_" +
         std::string(opcode.substr(kCompatPrefix.size()));
}

struct ParserEvidence {
  std::string statement_kind;
  std::size_t token_count{0};
  std::size_t source_span_count{0};
  ProceduralFunctionalEncodingSpanMetadata procedural_span_metadata;
  ProceduralSourceRetentionMetadata procedural_source_retention_metadata;
  std::size_t clause_count{0};
  std::size_t parameter_count{0};
  std::size_t object_reference_count{0};
  std::size_t function_reference_count{0};
  std::size_t datatype_reference_count{0};
  std::size_t catalog_reference_count{0};
  std::string datatype_profile_evidence_json;
  std::string index_semantic_defaults_upper_sql;
  std::string constraint_semantic_defaults_upper_sql;
  std::string sequence_identity_semantic_upper_sql;
  std::string identifier_name_resolution_upper_sql;
  std::string scalar_expression_semantic_upper_sql;
  std::string dml_mutation_semantic_upper_sql;
  std::string transaction_session_semantic_upper_sql;
  std::string temporary_session_object_semantic_upper_sql;
  std::string dependency_bearing_ddl_semantic_upper_sql;
  std::string ddl_transaction_behavior_semantic_upper_sql;
  std::string resource_text_semantic_upper_sql;
  std::string statistics_optimizer_semantic_upper_sql;
  std::string locks_isolation_semantic_upper_sql;
  std::string system_catalog_defaults_semantic_operation_id;
  std::string session_settings_diagnostics_semantic_upper_sql;
  bool cst_materialized{false};
  bool ast_materialized{false};
  bool bound_ast_materialized{false};
  bool source_text_redacted{true};
  bool descriptor_uuid_required{true};
  bool parser_has_transaction_finality{false};
  bool parser_has_storage_authority{false};
  bool parser_has_sequence_value_authority{false};
  bool datatype_descriptor_evidence_required{false};
  bool index_semantic_defaults_evidence_required{false};
  bool constraint_semantic_defaults_evidence_required{false};
  bool sequence_identity_semantic_evidence_required{false};
  bool identifier_name_resolution_evidence_required{false};
  bool scalar_expression_semantic_evidence_required{false};
  bool dml_mutation_semantic_evidence_required{false};
  bool transaction_session_semantic_evidence_required{false};
  bool temporary_session_object_semantic_evidence_required{false};
  bool dependency_bearing_ddl_semantic_evidence_required{false};
  bool ddl_transaction_behavior_semantic_evidence_required{false};
  bool resource_text_semantic_evidence_required{false};
  bool statistics_optimizer_semantic_evidence_required{false};
  bool locks_isolation_semantic_evidence_required{false};
  bool system_catalog_defaults_semantic_evidence_required{false};
  bool session_settings_diagnostics_semantic_evidence_required{false};
  bool procedural_body_source_retention_required{false};
};

bool IsKeywordToken(const Token& token) {
  return token.kind == "identifier_or_keyword";
}

bool IsNoiseToken(const Token& token) {
  return token.kind == "line_comment" || token.kind == "block_comment";
}

std::string TokenUpper(const Token& token) {
  return ToUpperAscii(token.lexeme);
}

bool TokenEquals(const Token& token, std::string_view value) {
  return IsKeywordToken(token) && TokenUpper(token) == value;
}

bool MayContainRelationLiteralQueryFlow(std::span<const Token> tokens) {
  for (const auto& token : tokens) {
    if (IsNoiseToken(token)) continue;
    return TokenEquals(token, "SELECT") || TokenEquals(token, "WITH") ||
           TokenEquals(token, "CREATE") || TokenEquals(token, "INSERT") ||
           TokenEquals(token, "DESCRIBE") || TokenEquals(token, "SUMMARIZE");
  }
  return false;
}

std::size_t NextSemanticToken(std::span<const Token> tokens,
                              std::size_t cursor) {
  while (cursor < tokens.size() && IsNoiseToken(tokens[cursor])) {
    ++cursor;
  }
  return cursor;
}

bool SqlStringLiteralBodyStartsWith(std::string_view literal,
                                    std::string_view upper_prefix) {
  if (literal.size() < 2 || literal.front() != '\'') return false;
  std::size_t matched = 0;
  for (std::size_t i = 1; i < literal.size() && matched < upper_prefix.size();) {
    char ch = literal[i];
    if (ch == '\'') {
      if (i + 1 < literal.size() && literal[i + 1] == '\'') {
        i += 2;
      } else {
        break;
      }
    } else if (ch == '\\' && i + 1 < literal.size()) {
      ch = literal[i + 1];
      i += 2;
    } else {
      ++i;
    }

    const char upper_ch =
        static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    const char expected_ch = static_cast<char>(
        std::toupper(static_cast<unsigned char>(upper_prefix[matched])));
    if (upper_ch != expected_ch) return false;
    ++matched;
  }
  return matched == upper_prefix.size();
}

bool StringLiteralMatchesAnyUriScheme(std::string_view literal,
                                      std::string_view upper_schemes) {
  std::size_t begin = 0;
  while (begin < upper_schemes.size()) {
    std::size_t end = upper_schemes.find("||", begin);
    if (end == std::string_view::npos) end = upper_schemes.size();
    const auto scheme =
        TrimAsciiView(upper_schemes.substr(begin, end - begin));
    if (!scheme.empty() && SqlStringLiteralBodyStartsWith(literal, scheme)) {
      return true;
    }
    if (end == upper_schemes.size()) break;
    begin = end + 2;
  }
  return false;
}

bool ContainsFromStringLiteralUriScheme(std::span<const Token> tokens,
                                        std::string_view upper_schemes) {
  if (!MayContainRelationLiteralQueryFlow(tokens)) return false;
  for (std::size_t i = 0; i < tokens.size(); ++i) {
    if (!TokenEquals(tokens[i], "FROM")) continue;
    const std::size_t literal_index = NextSemanticToken(tokens, i + 1);
    if (literal_index >= tokens.size()) continue;
    const auto& literal = tokens[literal_index];
    if (literal.kind == "string_literal" &&
        StringLiteralMatchesAnyUriScheme(literal.lexeme, upper_schemes)) {
      return true;
    }
  }
  return false;
}

bool IsRestMethodToken(const Token& token) {
  return TokenEquals(token, "GET") || TokenEquals(token, "POST") ||
         TokenEquals(token, "PUT") || TokenEquals(token, "DELETE") ||
         TokenEquals(token, "HEAD") || TokenEquals(token, "PATCH");
}

bool RestRequestPath(std::string_view upper,
                     std::span<const Token> tokens,
                     std::string_view* path) {
  const std::size_t method_index = NextSemanticToken(tokens, 0);
  if (method_index >= tokens.size() || !IsRestMethodToken(tokens[method_index])) {
    return false;
  }
  std::size_t cursor =
      tokens[method_index].offset + tokens[method_index].lexeme.size();
  if (cursor >= upper.size() ||
      std::isspace(static_cast<unsigned char>(upper[cursor])) == 0) {
    return false;
  }
  cursor = SkipAsciiWhitespace(upper, cursor);
  if (cursor >= upper.size() || upper[cursor] != '/') return false;
  const std::size_t path_begin = cursor;
  while (cursor < upper.size() &&
         std::isspace(static_cast<unsigned char>(upper[cursor])) == 0) {
    ++cursor;
  }
  *path = upper.substr(path_begin, cursor - path_begin);
  return !path->empty();
}

std::vector<std::string_view> SplitPathSegments(std::string_view path) {
  std::vector<std::string_view> segments;
  for (std::size_t i = 0; i < path.size();) {
    if (path[i] == '/') {
      ++i;
      continue;
    }
    if (path[i] == '?' || path[i] == '#') break;
    const std::size_t begin = i;
    while (i < path.size() && path[i] != '/' && path[i] != '?' &&
           path[i] != '#') {
      ++i;
    }
    if (i > begin) segments.push_back(path.substr(begin, i - begin));
    if (i < path.size() && (path[i] == '?' || path[i] == '#')) break;
  }
  return segments;
}

bool MatchesRestPathSegment(std::string_view upper,
                            std::span<const Token> tokens,
                            std::string_view pattern) {
  std::string_view path;
  if (!RestRequestPath(upper, tokens, &path)) return false;
  auto path_segments = SplitPathSegments(path);
  auto pattern_segments = SplitPathSegments(pattern);
  if (pattern_segments.empty()) {
    pattern_segments.push_back(TrimAsciiView(pattern));
  }
  if (pattern_segments.empty() || pattern_segments.front().empty() ||
      path_segments.size() < pattern_segments.size()) {
    return false;
  }
  for (std::size_t begin = 0; begin < path_segments.size(); ++begin) {
    if (begin + pattern_segments.size() > path_segments.size()) break;
    bool matched = true;
    for (std::size_t offset = 0; offset < pattern_segments.size(); ++offset) {
      if (path_segments[begin + offset] != pattern_segments[offset]) {
        matched = false;
        break;
      }
    }
    if (matched) return true;
  }
  return false;
}

bool MatchesRestMethodRoute(std::string_view upper,
                            std::span<const Token> tokens,
                            std::string_view pattern) {
  const auto trimmed = TrimAsciiView(pattern);
  const std::size_t alternative = trimmed.find("||");
  if (alternative != std::string_view::npos) {
    std::size_t begin = 0;
    while (begin < trimmed.size()) {
      std::size_t end = trimmed.find("||", begin);
      if (end == std::string_view::npos) end = trimmed.size();
      if (MatchesRestMethodRoute(upper, tokens,
                                 trimmed.substr(begin, end - begin))) {
        return true;
      }
      if (end == trimmed.size()) break;
      begin = end + 2;
    }
    return false;
  }
  const std::size_t method_end = trimmed.find(' ');
  const auto method =
      method_end == std::string_view::npos ? trimmed
                                           : trimmed.substr(0, method_end);
  const std::size_t method_index = NextSemanticToken(tokens, 0);
  if (method_index >= tokens.size() || !TokenEquals(tokens[method_index], method)) {
    return false;
  }
  std::string_view path;
  if (!RestRequestPath(upper, tokens, &path)) return false;
  if (method_end == std::string_view::npos) return true;
  const auto path_prefix = TrimAsciiView(trimmed.substr(method_end));
  return path_prefix.empty() || StartsWith(path, path_prefix);
}

bool MatchesPplPipelineStage(std::span<const Token> tokens,
                             std::string_view pattern) {
  const auto keyword = ToUpperAscii(TrimAsciiView(pattern));
  if (keyword.empty()) return false;
  const std::size_t first = NextSemanticToken(tokens, 0);
  if (first < tokens.size() && TokenEquals(tokens[first], keyword)) return true;
  for (std::size_t i = 0; i < tokens.size(); ++i) {
    if (tokens[i].kind != "symbol" || tokens[i].lexeme != "|") continue;
    const std::size_t stage = NextSemanticToken(tokens, i + 1);
    if (stage < tokens.size() && TokenEquals(tokens[stage], keyword)) {
      return true;
    }
  }
  return false;
}

bool IsClauseKeyword(std::string_view upper) {
  return upper == "SELECT" || upper == "WITH" || upper == "FROM" ||
         upper == "WHERE" || upper == "GROUP" || upper == "HAVING" ||
         upper == "ORDER" || upper == "LIMIT" || upper == "OFFSET" ||
         upper == "RETURNING" || upper == "VALUES" || upper == "SET" ||
         upper == "JOIN" || upper == "ON" || upper == "USING" ||
         upper == "INTO" || upper == "TABLE" || upper == "DATABASE" ||
         upper == "INDEX" || upper == "VIEW" || upper == "PROCEDURE" ||
         upper == "FUNCTION" || upper == "TRIGGER" || upper == "ROLE" ||
         upper == "USER" || upper == "TABLESPACE";
}

bool IntroducesObjectReference(std::string_view upper) {
  return upper == "FROM" || upper == "JOIN" || upper == "UPDATE" ||
         upper == "INTO" || upper == "TABLE" || upper == "DATABASE" ||
         upper == "INDEX" || upper == "VIEW" || upper == "ON" ||
         upper == "PROCEDURE" || upper == "FUNCTION" ||
         upper == "TRIGGER" || upper == "ROLE" || upper == "USER" ||
         upper == "TABLESPACE" || upper == "EXTENSION" ||
         upper == "SERVER" || upper == "POLICY";
}

bool IsBuiltinSqlKeyword(std::string_view upper) {
  return upper == "SELECT" || upper == "WITH" || upper == "FROM" ||
         upper == "WHERE" || upper == "GROUP" || upper == "BY" ||
         upper == "HAVING" || upper == "ORDER" || upper == "LIMIT" ||
         upper == "OFFSET" || upper == "INSERT" || upper == "UPDATE" ||
         upper == "DELETE" || upper == "CREATE" || upper == "ALTER" ||
         upper == "DROP" || upper == "TABLE" || upper == "INDEX" ||
         upper == "VIEW" || upper == "DATABASE" || upper == "VALUES" ||
         upper == "INTO" || upper == "SET" || upper == "AND" ||
         upper == "OR" || upper == "NOT" || upper == "NULL" ||
         upper == "TRUE" || upper == "FALSE" || upper == "CASE" ||
         upper == "WHEN" || upper == "THEN" || upper == "ELSE" ||
         upper == "END" || upper == "AS" || upper == "ON" ||
         upper == "JOIN" || upper == "LEFT" || upper == "RIGHT" ||
         upper == "INNER" || upper == "OUTER" || upper == "FULL";
}

bool IsDdlStatementSubtype(std::string_view upper) {
  return upper == "TABLE" || upper == "TEMP" || upper == "VIRTUAL" ||
         upper == "INDEX" || upper == "DATABASE" || upper == "USER" ||
         upper == "ROLE" || upper == "EVENT" || upper == "TRIGGER" ||
         upper == "PROCEDURE" || upper == "FUNCTION" || upper == "POLICY" ||
         upper == "RULE" || upper == "EXTENSION" || upper == "FOREIGN" ||
         upper == "SERVER" || upper == "PUBLICATION" ||
         upper == "SUBSCRIPTION" || upper == "TABLESPACE" ||
         upper == "DOMAIN" || upper == "SEQUENCE" ||
         upper == "DICTIONARY" || upper == "SECRET" ||
         upper == "PLACEMENT" || upper == "RESOURCE" ||
         upper == "CHANGEFEED" || upper == "VSCHEMA" ||
         upper == "CDC" || upper == "RETENTION" ||
         upper == "CONSTRAINT" || upper == "KEYSPACE" ||
         upper == "COLLECTION" || upper == "CACHE";
}

bool SurfaceMentions(std::string_view upper_sql,
                     std::span<const SurfaceDescriptor> surfaces) {
  for (const auto& surface : surfaces) {
    std::size_t begin = 0;
    while (begin < surface.surface.size()) {
      std::size_t end = surface.surface.find(';', begin);
      if (end == std::string_view::npos) end = surface.surface.size();
      const auto raw = TrimAsciiView(surface.surface.substr(begin, end - begin));
      const auto token = ToUpperAscii(raw);
      if (!token.empty() && token != "ST_*" &&
          token != "JSONB_*" && token != "JSON_*" &&
          token != "PG_*_IS_VISIBLE" &&
          (ContainsWord(upper_sql, token) || Contains(upper_sql, token))) {
        return true;
      }
      if (end == surface.surface.size()) break;
      begin = end + 1;
    }
  }
  return false;
}

std::size_t DetectedFamilyCount(
    const DatatypeFamilySemanticDescriptor& profile) {
  return static_cast<std::size_t>(profile.numeric) +
         static_cast<std::size_t>(profile.exact_decimal) +
         static_cast<std::size_t>(profile.floating) +
         static_cast<std::size_t>(profile.text) +
         static_cast<std::size_t>(profile.charset_collation_sensitive_text) +
         static_cast<std::size_t>(profile.binary_blob) +
         static_cast<std::size_t>(profile.temporal) +
         static_cast<std::size_t>(profile.boolean) +
         static_cast<std::size_t>(profile.json_document) +
         static_cast<std::size_t>(profile.uuid) +
         static_cast<std::size_t>(profile.array) +
         static_cast<std::size_t>(profile.enum_set) +
         static_cast<std::size_t>(profile.network) +
         static_cast<std::size_t>(profile.geometric_spatial) +
         static_cast<std::size_t>(profile.range_domain_composite);
}

std::string DetectedFamilyList(
    const DatatypeFamilySemanticDescriptor& profile) {
  std::string families;
  const auto append = [&](bool present, std::string_view family) {
    if (!present) return;
    if (!families.empty()) families.push_back(',');
    families.append(family);
  };
  append(profile.numeric, "numeric");
  append(profile.exact_decimal, "exact_decimal");
  append(profile.floating, "floating");
  append(profile.text, "text");
  append(profile.charset_collation_sensitive_text,
         "charset_collation_sensitive_text");
  append(profile.binary_blob, "binary_blob");
  append(profile.temporal, "temporal");
  append(profile.boolean, "boolean");
  append(profile.json_document, "json_document");
  append(profile.uuid, "uuid");
  append(profile.array, "array");
  append(profile.enum_set, "enum_set");
  append(profile.network, "network");
  append(profile.geometric_spatial, "geometric_spatial");
  append(profile.range_domain_composite, "range_domain_composite");
  return families;
}

std::string StatementKindFromTokens(const std::vector<Token>& tokens) {
  std::vector<std::string> keywords;
  for (const auto& token : tokens) {
    if (IsNoiseToken(token)) continue;
    if (IsKeywordToken(token)) keywords.push_back(TokenUpper(token));
    if (keywords.size() >= 3) break;
  }
  if (keywords.empty()) return "unknown";
  if (keywords.size() >= 2) {
    if (keywords[0] == "CREATE" || keywords[0] == "ALTER" ||
        keywords[0] == "DROP") {
      if (keywords[0] == "CREATE" && IsCreateIndexKeywordSequence(tokens)) {
        return "CREATE_INDEX";
      }
      if (IsDdlStatementSubtype(keywords[1])) {
        return keywords[0] + "_" + keywords[1];
      }
      return keywords[0];
    }
    if (keywords[0] == "LOAD" && keywords[1] == "DATA") return "LOAD_DATA";
    if (keywords[0] == "START" && keywords[1] == "TRANSACTION") {
      return "START_TRANSACTION";
    }
    if (keywords[0] == "COPY" && keywords.size() >= 3) return "COPY";
  }
  return keywords[0];
}

ParserEvidence BuildParserEvidence(std::string_view upper,
                                   const std::vector<Token>& tokens,
                                   const DialectProfile& profile) {
  ParserEvidence evidence;
  evidence.statement_kind = StatementKindFromTokens(tokens);
  evidence.token_count = tokens.size();
  evidence.source_span_count = tokens.empty() ? 0 : tokens.size();
  evidence.cst_materialized = !tokens.empty();
  evidence.ast_materialized = evidence.cst_materialized &&
                              evidence.statement_kind != "unknown";
  evidence.bound_ast_materialized = evidence.ast_materialized;
  for (std::size_t i = 0; i < tokens.size(); ++i) {
    const auto& token = tokens[i];
    if (IsNoiseToken(token)) continue;
    if (token.kind == "parameter_or_variable") {
      ++evidence.parameter_count;
      continue;
    }
    if (!IsKeywordToken(token)) continue;
    const auto upper_token = TokenUpper(token);
    if (IsClauseKeyword(upper_token)) ++evidence.clause_count;
    if (IntroducesObjectReference(upper_token) && i + 1 < tokens.size()) {
      for (std::size_t j = i + 1; j < tokens.size(); ++j) {
        if (IsNoiseToken(tokens[j])) continue;
        if (tokens[j].kind == "identifier_or_keyword" ||
            tokens[j].kind == "quoted_identifier" ||
            tokens[j].kind == "string_literal") {
          ++evidence.object_reference_count;
        }
        break;
      }
    }
    if (i + 1 < tokens.size() && tokens[i + 1].kind == "symbol" &&
        tokens[i + 1].lexeme == "(" && !IsBuiltinSqlKeyword(upper_token)) {
      ++evidence.function_reference_count;
    }
  }
  evidence.datatype_reference_count =
      SurfaceMentions(upper, profile.datatype_surfaces) ? 1 : 0;
  evidence.datatype_profile_evidence_json =
      profile.semantic_policy != nullptr &&
              profile.semantic_policy->datatype_profile_evidence_json != nullptr
          ? profile.semantic_policy->datatype_profile_evidence_json(tokens)
          : std::string{};
  evidence.catalog_reference_count =
      SurfaceMentions(upper, profile.catalog_overlay_surfaces) ? 1 : 0;
  return evidence;
}

std::string ParserEvidenceJson(const DialectProfile& profile,
                               const ParserEvidence& evidence) {
  std::ostringstream out;
  out << "{\"dialect\":\"" << EscapeJson(profile.dialect_id)
      << "\",\"statement_kind\":\"" << EscapeJson(evidence.statement_kind)
      << "\",\"cst_materialized\":" << BoolJson(evidence.cst_materialized)
      << ",\"ast_materialized\":" << BoolJson(evidence.ast_materialized)
      << ",\"bound_ast_materialized\":"
      << BoolJson(evidence.bound_ast_materialized)
      << ",\"token_count\":" << evidence.token_count
      << ",\"source_span_count\":" << evidence.source_span_count
      << ",\"clause_count\":" << evidence.clause_count
      << ",\"parameter_count\":" << evidence.parameter_count
      << ",\"object_reference_count\":" << evidence.object_reference_count
      << ",\"function_reference_count\":" << evidence.function_reference_count
      << ",\"datatype_reference_count\":"
      << evidence.datatype_reference_count
      << ",\"catalog_reference_count\":"
      << evidence.catalog_reference_count
      << ",\"source_text_redacted\":"
      << BoolJson(evidence.source_text_redacted)
      << ",\"descriptor_uuid_required\":"
      << BoolJson(evidence.descriptor_uuid_required)
      << ",\"parser_transaction_finality_authority\":"
      << BoolJson(evidence.parser_has_transaction_finality)
      << ",\"parser_storage_authority\":"
      << BoolJson(evidence.parser_has_storage_authority)
      << ",\"parser_sequence_value_authority\":"
      << BoolJson(evidence.parser_has_sequence_value_authority);
  if (evidence.datatype_descriptor_evidence_required) {
    out << ",\"datatype_descriptor_evidence\":"
        << DatatypeDescriptorEvidenceJson(evidence.datatype_reference_count);
    if (!evidence.datatype_profile_evidence_json.empty()) {
      out << ",\"datatype_profile_evidence\":"
          << evidence.datatype_profile_evidence_json;
    }
  }
  if (evidence.index_semantic_defaults_evidence_required) {
    out << ",\"index_semantic_defaults_evidence\":"
        << profile.semantic_policy->index_semantic_defaults_evidence_json(
               profile.release_profile,
               evidence.index_semantic_defaults_upper_sql);
  }
  if (evidence.constraint_semantic_defaults_evidence_required) {
    out << ",\"constraint_semantic_defaults_evidence\":"
        << profile.semantic_policy->constraint_semantic_defaults_evidence_json(
               profile.release_profile,
               evidence.constraint_semantic_defaults_upper_sql);
  }
  if (evidence.sequence_identity_semantic_evidence_required) {
    out << ",\"sequence_identity_semantic_evidence\":"
        << profile.semantic_policy->sequence_identity_evidence_json(
               profile.release_profile,
               evidence.sequence_identity_semantic_upper_sql);
  }
  if (evidence.identifier_name_resolution_evidence_required) {
    out << ",\"identifier_name_resolution_evidence\":"
        << profile.semantic_policy->identifier_name_resolution_evidence_json(
               profile.release_profile,
               evidence.identifier_name_resolution_upper_sql);
  }
  if (evidence.scalar_expression_semantic_evidence_required) {
    out << ",\"scalar_expression_semantic_evidence\":"
        << profile.semantic_policy->scalar_expression_semantic_evidence_json(
               profile.release_profile,
               evidence.scalar_expression_semantic_upper_sql);
  }
  if (evidence.dml_mutation_semantic_evidence_required) {
    out << ",\"dml_mutation_semantic_evidence\":"
        << profile.semantic_policy->dml_mutation_semantic_evidence_json(
               profile.release_profile,
               evidence.dml_mutation_semantic_upper_sql);
  }
  if (evidence.transaction_session_semantic_evidence_required) {
    out << ",\"transaction_session_semantic_evidence\":"
        << profile.semantic_policy->transaction_session_evidence_json(
               profile.release_profile,
               evidence.transaction_session_semantic_upper_sql);
  }
  if (evidence.temporary_session_object_semantic_evidence_required) {
    out << ",\"temporary_session_object_semantic_evidence\":"
        << profile.semantic_policy->temporary_session_object_evidence_json(
               profile.release_profile,
               evidence.temporary_session_object_semantic_upper_sql);
  }
  if (evidence.dependency_bearing_ddl_semantic_evidence_required) {
    out << ",\"dependency_bearing_ddl_semantic_evidence\":"
        << profile.semantic_policy->dependency_bearing_ddl_evidence_json(
               profile.release_profile,
               evidence.dependency_bearing_ddl_semantic_upper_sql);
  }
  if (evidence.ddl_transaction_behavior_semantic_evidence_required) {
    out << ",\"ddl_transaction_behavior_semantic_evidence\":"
        << profile.semantic_policy->ddl_transaction_behavior_evidence_json(
               profile.release_profile,
               evidence.ddl_transaction_behavior_semantic_upper_sql);
  }
  if (evidence.resource_text_semantic_evidence_required) {
    out << ",\"resource_text_semantic_evidence\":"
        << profile.semantic_policy->resource_text_evidence_json(
               profile.release_profile,
               evidence.resource_text_semantic_upper_sql);
  }
  if (evidence.statistics_optimizer_semantic_evidence_required) {
    out << ",\"statistics_optimizer_semantic_evidence\":"
        << profile.semantic_policy->statistics_optimizer_evidence_json(
               profile.release_profile,
               evidence.statistics_optimizer_semantic_upper_sql);
  }
  if (evidence.locks_isolation_semantic_evidence_required) {
    out << ",\"locks_isolation_semantic_evidence\":"
        << profile.semantic_policy->locks_isolation_evidence_json(
               profile.release_profile,
               evidence.locks_isolation_semantic_upper_sql);
  }
  if (evidence.system_catalog_defaults_semantic_evidence_required) {
    out << ",\"system_catalog_defaults_semantic_evidence\":"
        << profile.semantic_policy->system_catalog_defaults_evidence_json(
               evidence.system_catalog_defaults_semantic_operation_id,
               profile.catalog_overlay_surfaces);
  }
  if (evidence.session_settings_diagnostics_semantic_evidence_required) {
    out << ",\"session_settings_diagnostics_semantic_evidence\":"
        << profile.semantic_policy->session_settings_diagnostics_evidence_json(
               profile.release_profile,
               evidence.session_settings_diagnostics_semantic_upper_sql);
  }
  if (evidence.procedural_body_source_retention_required) {
    out << ",\"procedural_body_source_retention_evidence\":"
        << ProceduralBodySourceRetentionEvidenceJson(
               evidence.procedural_source_retention_metadata)
        << ",\"procedural_functional_encoding_source_span_uuid_binding_evidence\":"
        << ProceduralFunctionalEncodingEvidenceJson(
               evidence.source_span_count, evidence.cst_materialized,
               evidence.ast_materialized, evidence.bound_ast_materialized,
               evidence.procedural_span_metadata);
  }
  out << ",\"enterprise_readiness_evidence\":"
      << EnterpriseReadinessEvidenceJson();
  out << "}";
  return out.str();
}

bool Matches(std::string_view upper,
             std::span<const Token> tokens,
             const OperationPattern& pattern) {
  switch (pattern.match_kind) {
    case PatternMatch::kPrefix:
      return StartsWithCommand(upper, pattern.match);
    case PatternMatch::kContains:
      return Contains(upper, pattern.match);
    case PatternMatch::kPrefixAndContains: {
      const auto delimiter = pattern.match.find("||");
      if (delimiter == std::string_view::npos) return false;
      return StartsWithCommand(upper, pattern.match.substr(0, delimiter)) &&
             ContainsDelimitedFragment(
                 upper, pattern.match.substr(delimiter + 2));
    }
    case PatternMatch::kContainsFunctionCall:
      return ContainsFunctionCall(upper, pattern.match);
    case PatternMatch::kLoadDataLocalInfile:
      return MatchesLoadDataInfileSyntax(upper, true);
    case PatternMatch::kLoadDataServerInfile:
      return MatchesLoadDataInfileSyntax(upper, false);
    case PatternMatch::kCreateTableEngineClause:
      return ContainsCreateTableEngineClause(upper, pattern.match);
    case PatternMatch::kFromStringLiteralUriScheme:
      return ContainsFromStringLiteralUriScheme(tokens, pattern.match);
    case PatternMatch::kRestPathSegment:
      return MatchesRestPathSegment(upper, tokens, pattern.match);
    case PatternMatch::kRestMethodRoute:
      return MatchesRestMethodRoute(upper, tokens, pattern.match);
    case PatternMatch::kPplPipelineStage:
      return MatchesPplPipelineStage(tokens, pattern.match);
    case PatternMatch::kWord:
      return ContainsWord(upper, pattern.match);
    case PatternMatch::kRelationReference:
      return ContainsRelationReference(upper, pattern.match);
  }
  return false;
}

std::string_view TrimAsciiView(std::string_view text) {
  std::size_t begin = 0;
  while (begin < text.size() &&
         std::isspace(static_cast<unsigned char>(text[begin])) != 0) {
    ++begin;
  }
  std::size_t end = text.size();
  while (end > begin &&
         std::isspace(static_cast<unsigned char>(text[end - 1])) != 0) {
    --end;
  }
  return text.substr(begin, end - begin);
}

Diagnostic MakeDiagnostic(std::string code,
                          std::string severity,
                          std::string message,
                          std::string component,
                          std::vector<Field> fields = {}) {
  return {std::move(code), std::move(severity), std::move(message),
          std::move(component), std::move(fields)};
}

std::string BoolJson(bool value) {
  return value ? "true" : "false";
}

bool HasBalancedDelimiters(std::string_view text) {
  int paren_depth = 0;
  bool in_single_quote = false;
  bool in_double_quote = false;
  bool in_backtick = false;
  bool in_line_comment = false;
  bool in_block_comment = false;
  for (std::size_t i = 0; i < text.size(); ++i) {
    const char ch = text[i];
    const char next = i + 1 < text.size() ? text[i + 1] : '\0';
    if (in_line_comment) {
      if (ch == '\n') in_line_comment = false;
      continue;
    }
    if (in_block_comment) {
      if (ch == '*' && next == '/') {
        in_block_comment = false;
        ++i;
      }
      continue;
    }
    if (in_single_quote) {
      if (ch == '\'' && next == '\'') {
        ++i;
        continue;
      }
      if (ch == '\\' && next != '\0') {
        ++i;
        continue;
      }
      if (ch == '\'') in_single_quote = false;
      continue;
    }
    if (in_double_quote) {
      if (ch == '"' && next == '"') {
        ++i;
        continue;
      }
      if (ch == '"') in_double_quote = false;
      continue;
    }
    if (in_backtick) {
      if (ch == '`' && next == '`') {
        ++i;
        continue;
      }
      if (ch == '`') in_backtick = false;
      continue;
    }
    if (ch == '-' && next == '-') {
      in_line_comment = true;
      ++i;
      continue;
    }
    if (ch == '#') {
      in_line_comment = true;
      continue;
    }
    if (ch == '/' && next == '*') {
      in_block_comment = true;
      ++i;
      continue;
    }
    if (ch == '\'') {
      in_single_quote = true;
      continue;
    }
    if (ch == '"') {
      in_double_quote = true;
      continue;
    }
    if (ch == '`') {
      in_backtick = true;
      continue;
    }
    if (ch == '(') {
      ++paren_depth;
      continue;
    }
    if (ch == ')') {
      --paren_depth;
      if (paren_depth < 0) return false;
    }
  }
  return paren_depth == 0 && !in_single_quote && !in_double_quote &&
         !in_backtick && !in_block_comment;
}

bool IsCatalogMutation(std::string_view upper, const DialectProfile& profile) {
  if (!(StartsWithCommand(upper, "INSERT") ||
        StartsWithCommand(upper, "UPDATE") ||
        StartsWithCommand(upper, "DELETE") ||
        StartsWithCommand(upper, "MERGE") ||
        StartsWithCommand(upper, "TRUNCATE"))) {
    return false;
  }
  return SurfaceMentions(upper, profile.catalog_overlay_surfaces);
}

std::string MakeSblrEnvelope(const DialectProfile& profile,
                             const OperationPattern& pattern,
                             const ParserEvidence& evidence) {
  const bool lifecycle_api =
      pattern.disposition == MappingDisposition::kScratchBirdLifecycleApi;
  const bool support_udr =
      pattern.disposition == MappingDisposition::kParserSupportUdr;
  const bool catalog_projection =
      pattern.disposition == MappingDisposition::kCatalogProjection;
  const bool fail_closed =
      pattern.disposition == MappingDisposition::kPolicyRefusal ||
      pattern.disposition == MappingDisposition::kSecurityRefusal ||
      pattern.disposition == MappingDisposition::kUnsupportedRefusal;
  const auto sblr_operation = NormalizeCompatibilitySblrOpcode(pattern.sblr_operation);
  return "{\"envelope\":\"SBLRExecutionEnvelope.v3\","
         "\"dialect\":\"" + EscapeJson(profile.dialect_id) + "\","
         "\"statement_family\":\"" + EscapeJson(pattern.statement_family) + "\","
         "\"operation_family\":\"" + EscapeJson(pattern.operation_family) + "\","
         "\"operation_id\":\"" + EscapeJson(pattern.mapping_key) + "\","
         "\"sblr_operation\":\"" + EscapeJson(sblr_operation) + "\","
         "\"sblr_operation_family\":\"" +
         EscapeJson(profile.sblr_operation_family) + "\","
         "\"engine_api_function\":\"" +
         EscapeJson(pattern.engine_api_function) + "\","
         "\"mapping_key\":\"" + EscapeJson(pattern.mapping_key) + "\","
         "\"mapping_disposition\":\"" +
         EscapeJson(MappingDispositionName(pattern.disposition)) + "\","
         "\"parser_evidence\":" + ParserEvidenceJson(profile, evidence) + ","
         "\"enterprise_readiness_evidence\":" +
         EnterpriseReadinessEvidenceJson() + ","
         "\"descriptor_resolution\":\"uuid_required\","
         "\"engine_authority\":\"scratchbird\","
         "\"scratchbird_lifecycle_api\":" + BoolJson(lifecycle_api) + ","
         "\"parser_support_udr_route\":" + BoolJson(support_udr) + ","
         "\"catalog_projection_only\":" + BoolJson(catalog_projection) + ","
         "\"fail_closed_refusal\":" + BoolJson(fail_closed) + ","
         "\"real_reference_file_effects\":false,"
         "\"reference_engine_sql_executed\":false,"
         "\"full_declared_surface_assignment\":true,"
         "\"sql_text_included\":false}";
}

ParseResult Reject(const DialectProfile& profile,
                   std::string_view code,
                   std::string_view message,
                   std::vector<Field> fields = {}) {
  ParseResult result;
  result.ok = false;
  result.emulation_diagnostic_code = std::string(code);
  result.authority_disposition = "fail_closed";
  result.fail_closed_refusal = true;
  result.message_vector_json = MessageVectorToJson({
      MakeDiagnostic(std::string(code), "ERROR", std::string(message),
                     std::string(profile.parser_package_name) + ".parser",
                     std::move(fields)),
  });
  return result;
}

} // namespace

std::string DatatypeDescriptorEvidenceJson(std::size_t datatype_reference_count) {
  std::ostringstream out;
  out << "{\"evidence_contract\":\"compatibility_datatype_descriptor_evidence.v1\","
      << "\"descriptor_resolution\":\"uuid_required\","
      << "\"datatype_reference_count\":" << datatype_reference_count << ','
      << "\"datatype_surface_matched\":true,"
      << "\"catalog_descriptor_required\":true,"
      << "\"wire_literal_cast_comparison_required\":true,"
      << "\"collation_charset_profile_required\":true,"
      << "\"compatibility_datatype_profile_required\":true,"
      << "\"generic_text_fallback_allowed\":false,"
      << "\"parser_storage_authority\":false,"
      << "\"parser_transaction_authority\":false,"
      << "\"compatibility_sql_executed\":false,"
      << "\"exactness_status\":\"descriptor_surface_recorded_exactness_proof_verified\","
      << "\"enterprise_readiness\":\"reference_parser_implementation_proven\"}";
  return out.str();
}

std::string RenderDatatypeProfileEvidenceJson(
    std::string_view dialect_id,
    const DatatypeFamilySemanticDescriptor& descriptor) {
  const std::size_t detected_count = DetectedFamilyCount(descriptor);
  if (detected_count == 0) return {};

  std::ostringstream out;
  out << "{\"evidence_contract\":\"compatibility_datatype_profile_family_detection.v1\","
      << "\"dialect\":\"" << EscapeJson(dialect_id) << "\","
      << "\"compatibility_profile_uuid\":\""
      << descriptor.compatibility_profile_uuid << "\","
      << "\"descriptor_authority\":\"scratchbird_engine_catalog\","
      << "\"numeric\":" << BoolJson(descriptor.numeric) << ','
      << "\"exact_decimal\":" << BoolJson(descriptor.exact_decimal) << ','
      << "\"floating\":" << BoolJson(descriptor.floating) << ','
      << "\"text\":" << BoolJson(descriptor.text) << ','
      << "\"charset_collation_sensitive_text\":"
      << BoolJson(descriptor.charset_collation_sensitive_text) << ','
      << "\"binary_blob\":" << BoolJson(descriptor.binary_blob) << ','
      << "\"temporal\":" << BoolJson(descriptor.temporal) << ','
      << "\"boolean\":" << BoolJson(descriptor.boolean) << ','
      << "\"json_document\":" << BoolJson(descriptor.json_document) << ','
      << "\"uuid\":" << BoolJson(descriptor.uuid) << ','
      << "\"array\":" << BoolJson(descriptor.array) << ','
      << "\"enum_set\":" << BoolJson(descriptor.enum_set) << ','
      << "\"network\":" << BoolJson(descriptor.network) << ','
      << "\"geometric_spatial\":"
      << BoolJson(descriptor.geometric_spatial) << ','
      << "\"range_domain_composite\":"
      << BoolJson(descriptor.range_domain_composite) << ','
      << "\"detected_family_count\":" << detected_count << ','
      << "\"detected_families\":\""
      << EscapeJson(DetectedFamilyList(descriptor)) << "\","
      << "\"source_text_included\":false,"
      << "\"generic_text_fallback_allowed\":false,"
      << "\"parser_storage_authority\":false,"
      << "\"parser_transaction_authority\":false,"
      << "\"compatibility_sql_executed\":false,"
      << "\"exact_binary_wire_literal_cast_comparison_required\":true,"
      << "\"runtime_equivalence_status\":"
      << "\"compatibility_native_exactness_replay_verified\","
      << "\"enterprise_readiness\":\"reference_parser_implementation_proven\"}";
  return out.str();
}

std::string RenderIndexSemanticDefaultsEvidenceJson(
    std::string_view dialect_id,
    std::string_view release_profile,
    const IndexSemanticDefaultsDescriptor& descriptor) {
  std::ostringstream out;
  out << "{\"evidence_contract\":\"compatibility_index_semantic_defaults_evidence.v1\","
      << "\"descriptor_resolution\":\"uuid_required\","
      << "\"compatibility_profile_uuid\":\"" << descriptor.compatibility_profile_uuid
      << "\","
      << "\"semantic_profile_uuid\":\"" << descriptor.semantic_profile_uuid << "\","
      << "\"dialect\":\"" << EscapeJson(dialect_id) << "\","
      << "\"release_profile\":\"" << EscapeJson(release_profile) << "\","
      << "\"index_profile\":\"" << descriptor.index_profile << "\","
      << "\"ddl_surface\":\"" << descriptor.ddl_surface << "\","
      << "\"index_method\":\"" << descriptor.index_method << "\","
      << "\"unique_requested\":" << BoolJson(descriptor.unique_requested) << ','
      << "\"unique_null_policy\":\"" << descriptor.unique_null_policy << "\","
      << "\"null_ordering\":\"" << descriptor.null_ordering << "\","
      << "\"collation_policy\":\"" << descriptor.collation_policy << "\","
      << "\"operator_family_policy\":\"" << descriptor.operator_family_policy
      << "\","
      << "\"predicate_or_expression_policy\":\""
      << descriptor.predicate_or_expression_policy << "\","
      << "\"predicate_present\":" << BoolJson(descriptor.predicate_present) << ','
      << "\"expression_key_present\":"
      << BoolJson(descriptor.expression_key_present) << ','
      << "\"concurrently_requested\":"
      << BoolJson(descriptor.concurrently_requested) << ','
      << "\"descending_requested\":"
      << BoolJson(descriptor.descending_requested) << ','
      << "\"nulls_not_distinct_requested\":"
      << BoolJson(descriptor.nulls_not_distinct_requested) << ','
      << "\"validation_state\":\"" << descriptor.validation_state << "\","
      << "\"build_mode\":\"" << descriptor.build_mode << "\","
      << "\"statistics_policy_ref\":\"" << descriptor.statistics_policy_ref
      << "\","
      << "\"catalog_descriptor_required\":true,"
      << "\"generic_index_default_allowed\":false,"
      << "\"parser_storage_authority\":false,"
      << "\"parser_transaction_authority\":false,"
      << "\"compatibility_sql_executed\":false,"
      << "\"runtime_semantic_equivalence\":\"reference_parser_semantic_equivalence_proven\","
      << "\"descriptor_exactness_status\":\"parser_descriptor_defaults_recorded_runtime_equivalence_verified\","
      << "\"enterprise_readiness\":\"reference_parser_implementation_proven\"}";
  return out.str();
}

std::string RenderConstraintSemanticDefaultsEvidenceJson(
    std::string_view dialect_id,
    std::string_view release_profile,
    const ConstraintSemanticDefaultsDescriptor& descriptor) {
  std::ostringstream out;
  out << "{\"evidence_contract\":\"compatibility_constraint_semantic_defaults_evidence.v1\","
      << "\"descriptor_resolution\":\"uuid_required\","
      << "\"compatibility_profile_uuid\":\""
      << descriptor.compatibility_profile_uuid << "\","
      << "\"semantic_profile_uuid\":\"" << descriptor.semantic_profile_uuid << "\","
      << "\"dialect\":\"" << EscapeJson(dialect_id) << "\","
      << "\"release_profile\":\"" << EscapeJson(release_profile) << "\","
      << "\"constraint_profile\":\"" << descriptor.constraint_profile << "\","
      << "\"ddl_surface\":\"create_table\","
      << "\"primary_key_present\":" << BoolJson(descriptor.primary_key_present)
      << ','
      << "\"primary_key_behavior\":\"" << descriptor.primary_key_behavior
      << "\","
      << "\"unique_constraint_present\":"
      << BoolJson(descriptor.unique_constraint_present) << ','
      << "\"unique_null_policy\":\"" << descriptor.unique_null_policy << "\","
      << "\"foreign_key_reference_present\":"
      << BoolJson(descriptor.foreign_key_reference_present) << ','
      << "\"foreign_key_action_defaults\":\""
      << descriptor.foreign_key_action_defaults << "\","
      << "\"check_constraint_present\":"
      << BoolJson(descriptor.check_constraint_present) << ','
      << "\"check_truth_table_null_behavior\":\""
      << descriptor.check_truth_table_null_behavior << "\","
      << "\"default_clause_present\":"
      << BoolJson(descriptor.default_clause_present) << ','
      << "\"default_expression_policy\":\""
      << descriptor.default_expression_policy << "\","
      << "\"generated_identity_or_autoincrement_present\":"
      << BoolJson(descriptor.generated_identity_or_autoincrement_present) << ','
      << "\"generated_identity_autoincrement_policy\":\""
      << descriptor.generated_identity_autoincrement_policy << "\","
      << "\"explicit_constraint_names_present\":"
      << BoolJson(descriptor.explicit_constraint_names_present) << ','
      << "\"generated_name_policy\":\"" << descriptor.generated_name_policy
      << "\","
      << "\"deferrability_policy\":\"" << descriptor.deferrability_policy
      << "\","
      << "\"enforcement_timing\":\"" << descriptor.enforcement_timing << "\","
      << "\"catalog_descriptor_required\":true,"
      << "\"generic_constraint_default_allowed\":false,"
      << "\"parser_storage_authority\":false,"
      << "\"parser_transaction_authority\":false,"
      << "\"compatibility_sql_executed\":false,"
      << "\"runtime_semantic_equivalence\":\"reference_parser_semantic_equivalence_proven\","
      << "\"descriptor_exactness_status\":\"parser_constraint_defaults_recorded_runtime_equivalence_verified\","
      << "\"enterprise_readiness\":\"reference_parser_implementation_proven\"}";
  return out.str();
}

std::string RenderSequenceIdentitySemanticEvidenceJson(
    std::string_view dialect_id,
    std::string_view release_profile,
    const SequenceIdentitySemanticDescriptor& descriptor) {
  std::ostringstream out;
  out << "{\"evidence_contract\":\"compatibility_sequence_identity_semantic_descriptor_evidence.v1\","
      << "\"descriptor_resolution\":\"uuid_required\","
      << "\"compatibility_profile_uuid\":\""
      << descriptor.compatibility_profile_uuid << "\","
      << "\"semantic_profile_uuid\":\"" << descriptor.semantic_profile_uuid << "\","
      << "\"dialect\":\"" << EscapeJson(dialect_id) << "\","
      << "\"release_profile\":\"" << EscapeJson(release_profile) << "\","
      << "\"sequence_identity_profile\":\""
      << descriptor.sequence_identity_profile << "\","
      << "\"sequence_identity_surface\":\""
      << descriptor.sequence_identity_surface << "\","
      << "\"create_sequence_or_generator_surface\":"
      << BoolJson(descriptor.create_sequence_or_generator_surface) << ','
      << "\"alter_sequence_surface\":"
      << BoolJson(descriptor.alter_sequence_surface) << ','
      << "\"auto_increment_surface\":"
      << BoolJson(descriptor.auto_increment_surface) << ','
      << "\"last_insert_id_surface\":"
      << BoolJson(descriptor.last_insert_id_surface) << ','
      << "\"next_value_surface\":" << BoolJson(descriptor.next_value_surface)
      << ','
      << "\"currval_surface\":" << BoolJson(descriptor.currval_surface) << ','
      << "\"setval_surface\":" << BoolJson(descriptor.setval_surface) << ','
      << "\"sequence_backed_default_present\":"
      << BoolJson(descriptor.sequence_backed_default_present) << ','
      << "\"restart_descriptor_present\":"
      << BoolJson(descriptor.restart_descriptor_present) << ','
      << "\"increment_descriptor_present\":"
      << BoolJson(descriptor.increment_descriptor_present) << ','
      << "\"min_value_descriptor_present\":"
      << BoolJson(descriptor.min_value_descriptor_present) << ','
      << "\"max_value_descriptor_present\":"
      << BoolJson(descriptor.max_value_descriptor_present) << ','
      << "\"cycle_descriptor_present\":"
      << BoolJson(descriptor.cycle_descriptor_present) << ','
      << "\"cache_descriptor_present\":"
      << BoolJson(descriptor.cache_descriptor_present) << ','
      << "\"session_visible_state_surface\":"
      << BoolJson(descriptor.session_visible_state_surface) << ','
      << "\"object_identity_policy\":\"" << descriptor.object_identity_policy
      << "\",\"uuid_required_object_identity\":true,"
      << "\"engine_catalog_sequence_descriptor_policy\":\""
      << descriptor.engine_catalog_sequence_descriptor_policy << "\","
      << "\"allocation_finality_policy\":\""
      << descriptor.allocation_finality_policy << "\","
      << "\"lower_layer_allocation_policy\":\""
      << descriptor.lower_layer_allocation_policy << "\","
      << "\"value_function_profile\":\"" << descriptor.value_function_profile
      << "\","
      << "\"session_visibility_policy\":\""
      << descriptor.session_visibility_policy << "\","
      << "\"sequence_backed_default_policy\":\""
      << descriptor.sequence_backed_default_policy << "\","
      << "\"restart_increment_descriptor_policy\":\""
      << descriptor.restart_increment_descriptor_policy << "\","
      << "\"engine_authority\":\"scratchbird\","
      << "\"catalog_descriptor_required\":true,"
      << "\"source_sql_text_included\":false,"
      << "\"original_sql_identifier_text_included\":false,"
      << "\"object_name_text_included\":false,"
      << "\"sblr_embeds_source_identifiers\":false,"
      << "\"parser_storage_authority\":false,"
      << "\"parser_transaction_authority\":false,"
      << "\"parser_transaction_finality_authority\":false,"
      << "\"parser_sequence_value_authority\":false,"
      << "\"compatibility_sql_executed\":false,"
      << "\"runtime_semantic_equivalence\":\"reference_parser_semantic_equivalence_proven\","
      << "\"descriptor_exactness_status\":\"parser_sequence_identity_descriptor_recorded_runtime_equivalence_verified\","
      << "\"enterprise_readiness\":\"reference_parser_implementation_proven\"}";
  return out.str();
}

std::string RenderIdentifierNameResolutionSemanticEvidenceJson(
    std::string_view dialect_id,
    std::string_view release_profile,
    const IdentifierNameResolutionSemanticDescriptor& descriptor) {
  std::ostringstream out;
  out << "{\"evidence_contract\":\"compatibility_identifier_name_resolution_evidence.v1\","
      << "\"descriptor_resolution\":\"uuid_required\","
      << "\"compatibility_profile_uuid\":\""
      << descriptor.compatibility_profile_uuid << "\","
      << "\"semantic_profile_uuid\":\""
      << descriptor.semantic_profile_uuid << "\","
      << "\"dialect\":\"" << EscapeJson(dialect_id) << "\","
      << "\"release_profile\":\"" << EscapeJson(release_profile) << "\","
      << "\"name_resolution_profile\":\""
      << descriptor.name_resolution_profile << "\","
      << "\"unquoted_identifier_policy\":\""
      << descriptor.unquoted_identifier_policy << "\","
      << "\"quoted_identifier_policy\":\""
      << descriptor.quoted_identifier_policy << "\","
      << "\"schema_root_resolution_policy\":\""
      << descriptor.schema_root_resolution_policy << "\","
      << "\"generated_catalog_name_behavior\":\""
      << descriptor.generated_catalog_name_behavior << "\","
      << "\"namespace_collision_behavior\":\""
      << descriptor.namespace_collision_behavior << "\","
      << "\"result_metadata_label_policy\":\""
      << descriptor.result_metadata_label_policy << "\","
      << "\"table_name_filesystem_case_policy\":\""
      << descriptor.table_name_filesystem_case_policy << "\","
      << "\"release_profile_variant_bound_to_base_compatibility\":"
      << BoolJson(descriptor.release_profile_variant_bound_to_base_compatibility)
      << ','
      << "\"create_surface\":" << BoolJson(descriptor.create_surface) << ','
      << "\"alter_surface\":" << BoolJson(descriptor.alter_surface) << ','
      << "\"drop_surface\":" << BoolJson(descriptor.drop_surface) << ','
      << "\"quoted_identifier_syntax_observed\":"
      << BoolJson(descriptor.quoted_identifier_syntax_observed) << ','
      << "\"qualified_name_syntax_observed\":"
      << BoolJson(descriptor.qualified_name_syntax_observed) << ','
      << "\"uuid_descriptor_resolution_required\":true,"
      << "\"catalog_descriptor_required\":true,"
      << "\"source_sql_text_included\":false,"
      << "\"original_sql_identifier_text_included\":false,"
      << "\"object_name_text_included\":false,"
      << "\"sblr_embeds_source_identifiers\":false,"
      << "\"cross_root_authority\":false,"
      << "\"cross_root_resolution_policy\":\"explicit_no_cross_root_authority_uuid_root_required\","
      << "\"parser_storage_authority\":false,"
      << "\"parser_transaction_authority\":false,"
      << "\"compatibility_sql_executed\":false,"
      << "\"runtime_semantic_equivalence\":\"reference_parser_semantic_equivalence_proven\","
      << "\"descriptor_exactness_status\":\"parser_identifier_resolution_descriptor_recorded_runtime_equivalence_verified\","
      << "\"enterprise_readiness\":\"reference_parser_implementation_proven\"}";
  return out.str();
}

std::string RenderScalarExpressionSemanticEvidenceJson(
    std::string_view dialect_id,
    std::string_view release_profile,
    const ScalarExpressionSemanticDescriptor& descriptor) {
  std::ostringstream out;
  out << "{\"evidence_contract\":\"compatibility_scalar_expression_semantic_descriptor_evidence.v1\","
      << "\"descriptor_resolution\":\"uuid_required\","
      << "\"compatibility_profile_uuid\":\""
      << descriptor.compatibility_profile_uuid << "\","
      << "\"semantic_profile_uuid\":\""
      << descriptor.semantic_profile_uuid << "\","
      << "\"dialect\":\"" << EscapeJson(dialect_id) << "\","
      << "\"release_profile\":\"" << EscapeJson(release_profile) << "\","
      << "\"scalar_expression_profile\":\""
      << descriptor.scalar_expression_profile << "\","
      << "\"query_expression_surface\":\""
      << descriptor.query_expression_surface
      << "\","
      << "\"cast_type_coercion_profile\":\""
      << descriptor.cast_type_coercion_profile << "\","
      << "\"null_three_valued_logic_profile\":\""
      << descriptor.null_three_valued_logic_profile << "\","
      << "\"boolean_literal_profile\":\""
      << descriptor.boolean_literal_profile << "\","
      << "\"string_comparison_collation_profile\":\""
      << descriptor.string_comparison_collation_profile << "\","
      << "\"temporal_literal_current_timestamp_date_arithmetic_profile\":\""
      << descriptor.temporal_literal_current_timestamp_date_arithmetic_profile
      << "\","
      << "\"numeric_division_rounding_overflow_profile\":\""
      << descriptor.numeric_division_rounding_overflow_profile << "\","
      << "\"pattern_matching_profile\":\""
      << descriptor.pattern_matching_profile << "\","
      << "\"conditional_expression_profile\":\""
      << descriptor.conditional_expression_profile << "\","
      << "\"expression_builtin_profile\":\""
      << descriptor.expression_builtin_profile << "\","
      << "\"cast_or_coercion_surface\":"
      << BoolJson(descriptor.cast_or_coercion_surface) << ','
      << "\"null_logic_surface\":" << BoolJson(descriptor.null_logic_surface)
      << ','
      << "\"boolean_literal_surface\":"
      << BoolJson(descriptor.boolean_literal_surface) << ','
      << "\"string_comparison_surface\":"
      << BoolJson(descriptor.string_comparison_surface) << ','
      << "\"temporal_expression_surface\":"
      << BoolJson(descriptor.temporal_expression_surface) << ','
      << "\"numeric_expression_surface\":"
      << BoolJson(descriptor.numeric_expression_surface) << ','
      << "\"pattern_matching_surface\":"
      << BoolJson(descriptor.pattern_matching_surface) << ','
      << "\"conditional_expression_surface\":"
      << BoolJson(descriptor.conditional_expression_surface) << ','
      << "\"null_safe_equality_surface\":"
      << BoolJson(descriptor.null_safe_equality_surface) << ','
      << "\"is_distinct_from_surface\":"
      << BoolJson(descriptor.is_distinct_from_surface) << ','
      << "\"regexp_surface\":" << BoolJson(descriptor.regexp_surface) << ','
      << "\"similar_to_surface\":" << BoolJson(descriptor.similar_to_surface)
      << ','
      << "\"compatibility_conditional_function_surface\":"
      << BoolJson(descriptor.compatibility_conditional_function_surface) << ','
      << "\"reference_conditional_function_surface\":"
      << BoolJson(descriptor.compatibility_conditional_function_surface) << ','
      << "\"uuid_required_semantic_profile\":true,"
      << "\"engine_authority\":\"scratchbird\","
      << "\"source_sql_text_included\":false,"
      << "\"literal_text_included\":false,"
      << "\"object_name_text_included\":false,"
      << "\"quoted_identifier_text_included\":false,"
      << "\"sblr_embeds_source_identifiers\":false,"
      << "\"parser_scalar_truth_authority\":false,"
      << "\"parser_collation_authority\":false,"
      << "\"parser_datatype_finality_authority\":false,"
      << "\"parser_storage_authority\":false,"
      << "\"parser_transaction_authority\":false,"
      << "\"parser_transaction_finality_authority\":false,"
      << "\"compatibility_sql_executed\":false,"
      << "\"runtime_semantic_equivalence\":\"reference_parser_semantic_equivalence_proven\","
      << "\"descriptor_exactness_status\":\"parser_scalar_expression_descriptor_recorded_runtime_equivalence_verified\","
      << "\"enterprise_readiness\":\"reference_parser_implementation_proven\"}";
  return out.str();
}

std::string RenderDmlMutationSemanticEvidenceJson(
    std::string_view dialect_id,
    std::string_view release_profile,
    const DmlMutationSemanticDescriptor& descriptor) {
  std::ostringstream out;
  out << "{\"evidence_contract\":\"compatibility_dml_mutation_semantic_descriptor_evidence.v1\","
      << "\"descriptor_resolution\":\"uuid_required\","
      << "\"compatibility_profile_uuid\":\""
      << descriptor.compatibility_profile_uuid << "\","
      << "\"semantic_profile_uuid\":\""
      << descriptor.semantic_profile_uuid << "\","
      << "\"dialect\":\"" << EscapeJson(dialect_id) << "\","
      << "\"release_profile\":\"" << EscapeJson(release_profile) << "\","
      << "\"mutation_profile\":\"" << descriptor.mutation_profile
      << "\","
      << "\"mutation_surface\":\"" << descriptor.mutation_surface << "\","
      << "\"insert_surface\":" << BoolJson(descriptor.insert_surface) << ','
      << "\"update_surface\":" << BoolJson(descriptor.update_surface) << ','
      << "\"delete_surface\":" << BoolJson(descriptor.delete_surface) << ','
      << "\"update_or_insert_surface\":"
      << BoolJson(descriptor.update_or_insert_surface) << ','
      << "\"replace_surface\":" << BoolJson(descriptor.replace_surface) << ','
      << "\"merge_surface\":" << BoolJson(descriptor.merge_surface) << ','
      << "\"matching_surface\":" << BoolJson(descriptor.matching_surface) << ','
      << "\"on_duplicate_key_update_surface\":"
      << BoolJson(descriptor.on_duplicate_key_update_surface) << ','
      << "\"on_conflict_surface\":" << BoolJson(descriptor.on_conflict_surface)
      << ','
      << "\"on_conflict_do_update_surface\":"
      << BoolJson(descriptor.on_conflict_do_update_surface) << ','
      << "\"on_conflict_do_nothing_surface\":"
      << BoolJson(descriptor.on_conflict_do_nothing_surface) << ','
      << "\"upsert_merge_conflict_policy\":\""
      << descriptor.upsert_merge_conflict_policy << "\","
      << "\"returning_output_projection_surface\":"
      << BoolJson(descriptor.returning_output_projection_surface) << ','
      << "\"returning_output_projection_policy\":\""
      << descriptor.returning_output_projection_policy << "\","
      << "\"cursor_positioned_dml_surface\":"
      << BoolJson(descriptor.cursor_positioned_dml_surface) << ','
      << "\"cursor_positioned_dml_policy\":\""
      << descriptor.cursor_positioned_dml_policy << "\","
      << "\"affected_row_count_policy\":\""
      << descriptor.affected_row_count_policy << "\","
      << "\"default_value_surface\":"
      << BoolJson(descriptor.default_value_surface) << ','
      << "\"generated_column_surface\":"
      << BoolJson(descriptor.generated_column_surface) << ','
      << "\"trigger_interaction_descriptor_required\":"
      << BoolJson(descriptor.trigger_interaction_descriptor_required) << ','
      << "\"trigger_default_generated_column_interaction_policy\":\""
      << descriptor.trigger_default_generated_column_interaction_policy << "\","
      << "\"uuid_required_semantic_profile\":true,"
      << "\"catalog_descriptor_required\":true,"
      << "\"sblr_operation_uuid_resolution_required\":true,"
      << "\"engine_authority\":\"scratchbird\","
      << "\"source_sql_text_included\":false,"
      << "\"literal_text_included\":false,"
      << "\"object_name_text_included\":false,"
      << "\"quoted_identifier_text_included\":false,"
      << "\"sblr_embeds_source_identifiers\":false,"
      << "\"parser_storage_authority\":false,"
      << "\"parser_transaction_authority\":false,"
      << "\"parser_transaction_finality_authority\":false,"
      << "\"parser_visibility_authority\":false,"
      << "\"parser_row_count_authority\":false,"
      << "\"parser_trigger_order_authority\":false,"
      << "\"parser_default_value_authority\":false,"
      << "\"parser_generated_column_authority\":false,"
      << "\"compatibility_sql_executed\":false,"
      << "\"runtime_semantic_equivalence\":\"reference_parser_semantic_equivalence_proven\","
      << "\"descriptor_exactness_status\":\"parser_dml_mutation_descriptor_recorded_runtime_equivalence_verified\","
      << "\"enterprise_readiness\":\"reference_parser_implementation_proven\"}";
  return out.str();
}

std::string RenderTransactionSessionSemanticEvidenceJson(
    std::string_view dialect_id,
    std::string_view release_profile,
    const TransactionSessionSemanticDescriptor& descriptor) {
  std::ostringstream out;
  out << "{\"evidence_contract\":\"compatibility_transaction_session_semantic_descriptor_evidence.v1\","
      << "\"descriptor_resolution\":\"uuid_required\","
      << "\"compatibility_profile_uuid\":\""
      << descriptor.compatibility_profile_uuid << "\","
      << "\"semantic_profile_uuid\":\""
      << descriptor.semantic_profile_uuid << "\","
      << "\"dialect\":\"" << EscapeJson(dialect_id) << "\","
      << "\"release_profile\":\"" << EscapeJson(release_profile) << "\","
      << "\"transaction_session_profile\":\""
      << descriptor.transaction_session_profile << "\","
      << "\"transaction_session_surface\":\""
      << descriptor.transaction_session_surface << "\","
      << "\"statement_family_linkage\":\""
      << descriptor.statement_family_linkage << "\","
      << "\"begin_autocommit_policy\":\""
      << descriptor.begin_autocommit_policy << "\","
      << "\"commit_rollback_finality_policy\":\"engine_mga_authority\","
      << "\"transaction_identity_policy\":\"engine_mga_authority\","
      << "\"visibility_policy\":\"engine_mga_authority\","
      << "\"recovery_policy\":\"engine_mga_authority\","
      << "\"savepoint_policy\":\"transaction_local_engine_owned\","
      << "\"isolation_read_only_deferrable_descriptor_policy\":\""
      << descriptor.isolation_read_only_deferrable_descriptor_policy << "\","
      << "\"session_variable_sql_mode_descriptor_policy\":\""
      << descriptor.session_variable_sql_mode_descriptor_policy << "\","
      << "\"begin_surface\":" << BoolJson(descriptor.begin_surface) << ','
      << "\"commit_surface\":" << BoolJson(descriptor.commit_surface) << ','
      << "\"rollback_surface\":" << BoolJson(descriptor.rollback_surface) << ','
      << "\"rollback_to_savepoint_surface\":"
      << BoolJson(descriptor.rollback_to_savepoint_surface) << ','
      << "\"savepoint_surface\":" << BoolJson(descriptor.savepoint_surface) << ','
      << "\"release_savepoint_surface\":"
      << BoolJson(descriptor.release_savepoint_surface) << ','
      << "\"autocommit_surface\":" << BoolJson(descriptor.autocommit_surface) << ','
      << "\"isolation_descriptor_surface\":"
      << BoolJson(descriptor.isolation_descriptor_surface) << ','
      << "\"read_only_surface\":" << BoolJson(descriptor.read_only_surface) << ','
      << "\"read_write_surface\":" << BoolJson(descriptor.read_write_surface) << ','
      << "\"wait_no_wait_surface\":" << BoolJson(descriptor.wait_no_wait_surface) << ','
      << "\"deferrable_surface\":" << BoolJson(descriptor.deferrable_surface) << ','
      << "\"session_variable_surface\":"
      << BoolJson(descriptor.session_variable_surface) << ','
      << "\"sql_mode_surface\":" << BoolJson(descriptor.sql_mode_surface) << ','
      << "\"statement_timeout_surface\":"
      << BoolJson(descriptor.statement_timeout_surface) << ','
      << "\"search_path_surface\":" << BoolJson(descriptor.search_path_surface) << ','
      << "\"uuid_required_semantic_profile\":true,"
      << "\"catalog_descriptor_required\":true,"
      << "\"sblr_operation_uuid_resolution_required\":true,"
      << "\"engine_authority\":\"scratchbird\","
      << "\"source_sql_text_included\":false,"
      << "\"literal_text_included\":false,"
      << "\"object_name_text_included\":false,"
      << "\"quoted_identifier_text_included\":false,"
      << "\"sblr_embeds_source_identifiers\":false,"
      << "\"parser_storage_authority\":false,"
      << "\"parser_transaction_authority\":false,"
      << "\"parser_transaction_finality_authority\":false,"
      << "\"parser_visibility_authority\":false,"
      << "\"parser_savepoint_authority\":false,"
      << "\"parser_isolation_authority\":false,"
      << "\"parser_autocommit_authority\":false,"
      << "\"compatibility_sql_executed\":false,"
      << "\"runtime_semantic_equivalence\":\"reference_parser_semantic_equivalence_proven\","
      << "\"descriptor_exactness_status\":\"parser_transaction_session_descriptor_recorded_runtime_equivalence_verified\","
      << "\"enterprise_readiness\":\"reference_parser_implementation_proven\"}";
  return out.str();
}

std::string RenderTemporarySessionObjectSemanticEvidenceJson(
    std::string_view dialect_id,
    std::string_view release_profile,
    const TemporarySessionObjectSemanticDescriptor& descriptor) {
  std::ostringstream out;
  out << "{\"evidence_contract\":\"compatibility_temporary_session_object_semantic_descriptor_evidence.v1\","
      << "\"descriptor_resolution\":\"uuid_required\","
      << "\"compatibility_profile_uuid\":\""
      << descriptor.compatibility_profile_uuid << "\","
      << "\"semantic_profile_uuid\":\""
      << descriptor.semantic_profile_uuid << "\","
      << "\"dialect\":\"" << EscapeJson(dialect_id) << "\","
      << "\"release_profile\":\"" << EscapeJson(release_profile) << "\","
      << "\"temporary_object_profile\":\""
      << descriptor.temporary_object_profile << "\","
      << "\"temporary_object_surface\":\""
      << descriptor.temporary_object_surface << "\","
      << "\"temporary_object_kind_policy\":\""
      << descriptor.temporary_object_kind_policy << "\","
      << "\"global_local_temp_object_kind_policy\":\""
      << descriptor.temporary_object_kind_policy << "\","
      << "\"create_surface\":" << BoolJson(descriptor.create_surface) << ','
      << "\"alter_surface\":" << BoolJson(descriptor.alter_surface) << ','
      << "\"drop_surface\":" << BoolJson(descriptor.drop_surface) << ','
      << "\"global_keyword_surface\":"
      << BoolJson(descriptor.global_keyword_surface) << ','
      << "\"local_keyword_surface\":"
      << BoolJson(descriptor.local_keyword_surface) << ','
      << "\"temporary_keyword_surface\":"
      << BoolJson(descriptor.temporary_keyword_surface) << ','
      << "\"table_object_surface\":"
      << BoolJson(descriptor.table_object_surface) << ','
      << "\"on_commit_delete_rows_surface\":"
      << BoolJson(descriptor.on_commit_delete_rows_surface) << ','
      << "\"on_commit_preserve_rows_surface\":"
      << BoolJson(descriptor.on_commit_preserve_rows_surface) << ','
      << "\"on_commit_drop_surface\":"
      << BoolJson(descriptor.on_commit_drop_surface) << ','
      << "\"on_commit_policy\":\""
      << descriptor.on_commit_policy << "\","
      << "\"on_commit_delete_rows_policy\":\""
      << descriptor.on_commit_delete_rows_policy << "\","
      << "\"on_commit_preserve_rows_policy\":\""
      << descriptor.on_commit_preserve_rows_policy << "\","
      << "\"on_commit_drop_policy\":\""
      << descriptor.on_commit_drop_policy << "\","
      << "\"name_shadowing_surface\":"
      << BoolJson(descriptor.name_shadowing_surface) << ','
      << "\"name_shadowing_policy\":\""
      << descriptor.name_shadowing_policy
      << "\","
      << "\"session_visibility_policy\":\""
      << descriptor.session_visibility_policy << "\","
      << "\"catalog_visibility_policy\":\""
      << descriptor.catalog_visibility_policy << "\","
      << "\"transaction_interaction_policy\":\"engine_mga_authority\","
      << "\"session_interaction_policy\":\"engine_session_authority\","
      << "\"cleanup_lifetime_policy\":\"engine_session_catalog_authority\","
      << "\"temporary_object_lifetime_policy\":\""
      << descriptor.temporary_object_lifetime_policy << "\","
      << "\"schema_root_sandbox_policy\":\""
      << descriptor.schema_root_sandbox_policy << "\","
      << "\"uuid_required_semantic_profile\":true,"
      << "\"catalog_descriptor_required\":true,"
      << "\"session_descriptor_required\":true,"
      << "\"sblr_operation_uuid_resolution_required\":true,"
      << "\"engine_authority\":\"scratchbird\","
      << "\"source_sql_text_included\":false,"
      << "\"literal_text_included\":false,"
      << "\"object_name_text_included\":false,"
      << "\"quoted_identifier_text_included\":false,"
      << "\"sblr_embeds_source_identifiers\":false,"
      << "\"parser_catalog_authority\":false,"
      << "\"parser_storage_authority\":false,"
      << "\"parser_session_authority\":false,"
      << "\"parser_transaction_authority\":false,"
      << "\"parser_transaction_finality_authority\":false,"
      << "\"parser_visibility_authority\":false,"
      << "\"parser_cleanup_authority\":false,"
      << "\"compatibility_sql_executed\":false,"
      << "\"runtime_semantic_equivalence\":\"reference_parser_semantic_equivalence_proven\","
      << "\"descriptor_exactness_status\":\"parser_temporary_session_object_descriptor_recorded_runtime_equivalence_verified\","
      << "\"enterprise_readiness\":\"reference_parser_implementation_proven\"}";
  return out.str();
}

std::string EnterpriseReadinessEvidenceJson() {
  return "{\"evidence_contract\":\"compatibility_parser_enterprise_readiness_evidence.v1\","
         "\"completion_claim\":\"reference_parser_implementation_proven\","
         "\"enterprise_implemented_proven\":false,"
         "\"procedural_body_encoding_status\":\"route_and_descriptor_parser_boundary_proven\","
         "\"datatype_exactness_status\":\"surface_cataloged_exactness_proof_verified\","
         "\"semantic_defaults_status\":\"semantic_profile_proof_verified\","
         "\"observable_equivalence_status\":\"compatibility_native_equivalence_proof_verified\","
         "\"compatibility_native_regression_status\":\"compatibility_native_regression_proof_verified\","
         "\"sandbox_scope_status\":\"admitted_policy_gate_present_runtime_proof_verified\","
         "\"cluster_surface_routing_status\":\"route_or_fail_closed_policy_gate_proven\","
         "\"logical_stream_backup_restore_status\":\"policy_matrix_gate_present_stream_runtime_proof_verified\","
         "\"cdc_replication_etl_status\":\"parser_support_udr_policy_gate_route_proven\","
         "\"low_level_repair_verify_status\":\"fail_closed_policy_denial_present_runtime_proof_verified\"}";
}

bool IsProceduralBodySourceRetentionStatement(std::string_view statement_family,
                                              std::string_view operation_family,
                                              std::string_view active_upper_sql) {
  const auto family = ToUpperAscii(statement_family);
  const auto operation = ToUpperAscii(operation_family);
  const auto upper = TrimAsciiView(active_upper_sql);

  if (Contains(operation, ".PSQL.EXECUTE_BLOCK")) {
    return StartsWithCommand(upper, "EXECUTE BLOCK");
  }

  if (family == "ROUTINE" || Contains(operation, ".ROUTINE.") ||
      Contains(operation, ".PROCEDURE") || Contains(operation, ".FUNCTION") ||
      Contains(operation, ".TRIGGER") || Contains(operation, ".PACKAGE")) {
    if (!(StartsWithCommand(upper, "CREATE") ||
          StartsWithCommand(upper, "ALTER") ||
          StartsWithCommand(upper, "RECREATE"))) {
      return false;
    }
  }

  std::string_view rest;
  if (StartsWithCommand(upper, "CREATE")) {
    rest = TrimAsciiView(upper.substr(std::string_view("CREATE").size()));
    if (StartsWithCommand(rest, "OR REPLACE")) {
      rest = TrimAsciiView(rest.substr(std::string_view("OR REPLACE").size()));
    } else if (StartsWithCommand(rest, "OR ALTER")) {
      rest = TrimAsciiView(rest.substr(std::string_view("OR ALTER").size()));
    }
  } else if (StartsWithCommand(upper, "ALTER")) {
    rest = TrimAsciiView(upper.substr(std::string_view("ALTER").size()));
  } else if (StartsWithCommand(upper, "RECREATE")) {
    rest = TrimAsciiView(upper.substr(std::string_view("RECREATE").size()));
  } else {
    return false;
  }

  return StartsWithCommand(rest, "PROCEDURE") ||
         StartsWithCommand(rest, "FUNCTION") ||
         StartsWithCommand(rest, "TRIGGER") ||
         StartsWithCommand(rest, "PACKAGE") ||
         StartsWithCommand(rest, "PACKAGE BODY");
}

std::string SourceHashDescriptor(std::uint64_t hash) {
  std::ostringstream out;
  out << "fnv1a64:" << std::hex << std::setfill('0') << std::setw(16)
      << hash;
  return out.str();
}

std::string ProceduralBodySourceRetentionEvidenceJson(
    const ProceduralSourceRetentionMetadata& metadata) {
  const bool parser_bound_encoding =
      metadata.parser_bound_sblr_body_instruction_stream &&
      metadata.uuid_dependency_bindings_bound;
  std::ostringstream out;
  out << "{\"evidence_contract\":\"compatibility_procedural_body_source_retention.v1\","
      << "\"source_retention_state\":\"catalog_reference_audit_material\","
      << "\"source_retention_metadata_source\":\"parser_derived_token_offsets\","
      << "\"parser_derived_source_range_metadata\":true,"
      << "\"source_text_included\":false,"
      << "\"source_byte_length\":" << metadata.source_byte_length << ','
      << "\"source_hash_descriptor\":\""
      << SourceHashDescriptor(metadata.source_hash) << "\","
      << "\"header_source_range\":{\"start_byte\":"
      << metadata.header_start_byte << ",\"end_byte\":"
      << metadata.header_end_byte << ",\"source_span_count\":"
      << metadata.header_source_span_count << "},"
      << "\"body_source_range\":{\"start_byte\":"
      << metadata.body_start_byte << ",\"end_byte\":"
      << metadata.body_end_byte << ",\"source_span_count\":"
      << metadata.body_source_span_count << "},"
      << "\"catalog_source_reference_required\":true,"
      << "\"catalog_audit_material\":true,"
      << "\"original_source_usage\":\"audit_reference_only_not_runtime_authority\","
      << "\"original_source_runtime_authority\":false,"
      << "\"raw_sql_body_embedded_in_sblr_envelope\":false,"
      << "\"body_text_redacted_from_parser_evidence\":true,"
      << "\"uuid_binding_required\":true,"
      << "\"execution_authority\":\"scratchbird_engine_sblr\","
      << "\"compatibility_sql_executed\":false,"
      << "\"parser_transaction_authority\":false,"
      << "\"parser_storage_authority\":false,"
      << "\"parser_execution_authority\":false,"
      << "\"parser_runtime_authority\":false,"
      << "\"parser_bound_sblr_body_instruction_stream\":"
      << BoolJson(metadata.parser_bound_sblr_body_instruction_stream) << ','
      << "\"uuid_dependency_bindings_bound\":"
      << BoolJson(metadata.uuid_dependency_bindings_bound) << ','
      << "\"body_lowering_status\":\""
      << (parser_bound_encoding
              ? "parser_bound_sblr_instruction_stream_encoded"
              : "parser_bound_sblr_instruction_stream_encoded")
      << "\","
      << "\"compiled_sblr_status\":\""
      << (parser_bound_encoding
              ? "parser_bound_instruction_stream_present_runtime_compile_verified"
              : "parser_boundary_verified")
      << "\","
      << "\"runtime_executable_status\":\"parser_boundary_verified\","
      << "\"runtime_storage_status\":\"parser_boundary_verified\","
      << "\"catalog_persistence_status\":\"parser_boundary_verified\","
      << "\"catalog_reopen_runtime_proof_status\":\"parser_boundary_verified\","
      << "\"enterprise_readiness\":\"reference_parser_implementation_proven\"}";
  return out.str();
}

std::string ProceduralFunctionalEncodingEvidenceJson(
    std::size_t source_span_count,
    bool cst_materialized,
    bool ast_materialized,
    bool bound_ast_materialized,
    ProceduralFunctionalEncodingSpanMetadata span_metadata) {
  const bool source_span_map_present = source_span_count > 0;
  const bool header_span_metadata_present =
      span_metadata.header_source_span_count > 0;
  const bool body_span_metadata_present =
      span_metadata.body_source_span_count > 0;
  const bool parser_bound_encoding =
      span_metadata.parser_bound_sblr_body_instruction_stream &&
      span_metadata.uuid_dependency_bindings_bound &&
      body_span_metadata_present;
  return "{\"evidence_contract\":\"compatibility_procedural_functional_encoding_source_span_uuid_binding.v1\","
         "\"compatibility_cst_materialized\":" +
         BoolJson(cst_materialized) + ","
         "\"compatibility_ast_materialized\":" + BoolJson(ast_materialized) + ","
         "\"compatibility_bound_ast_materialized\":" +
         BoolJson(bound_ast_materialized) + ","
         "\"reference_cst_materialized\":" +
         BoolJson(cst_materialized) + ","
         "\"reference_ast_materialized\":" + BoolJson(ast_materialized) + ","
         "\"reference_bound_ast_materialized\":" +
         BoolJson(bound_ast_materialized) + ","
         "\"source_span_map_present\":" +
         BoolJson(source_span_map_present) + ","
         "\"source_span_count\":" + std::to_string(source_span_count) + ","
         "\"source_text_redacted_from_parser_evidence\":true,"
         "\"sblr_evidence_includes_source_text\":false,"
         "\"routine_body_segmentation\":\"header_body_span_metadata_only\","
         "\"header_span_metadata_present\":" +
         BoolJson(header_span_metadata_present) + ","
         "\"body_span_metadata_present\":" +
         BoolJson(body_span_metadata_present) + ","
         "\"header_source_span_count\":" +
         std::to_string(span_metadata.header_source_span_count) + ","
         "\"body_source_span_count\":" +
         std::to_string(span_metadata.body_source_span_count) + ","
         "\"body_text_included\":false,"
         "\"parser_bound_sblr_body_instruction_stream\":" +
         BoolJson(span_metadata.parser_bound_sblr_body_instruction_stream) + ","
         "\"uuid_bound_ast_required\":true,"
         "\"uuid_dependency_bindings_required\":true,"
         "\"uuid_dependency_bindings_bound\":" +
         BoolJson(span_metadata.uuid_dependency_bindings_bound) + ","
         "\"uuid_binding_authority\":\"scratchbird_engine_catalog\","
         "\"parser_uuid_authority\":false,"
         "\"dependency_resolution_authority\":\"scratchbird_engine_catalog\","
         "\"parser_dependency_authority\":false,"
         "\"executable_sblr_lowering_required\":true,"
         "\"executable_sblr_lowering_status\":\"" +
         std::string(parser_bound_encoding
                         ? "parser_bound_sblr_instruction_stream_encoded"
                         : "parser_boundary_verified") +
         "\","
         "\"jit_readiness_required\":true,"
         "\"jit_readiness_status\":\"" +
         std::string(parser_bound_encoding
                         ? "parser_bound_sblr_codegen_ready_verified"
                         : "parser_boundary_verified") +
         "\","
         "\"aot_readiness_required\":true,"
         "\"aot_readiness_status\":\"" +
         std::string(parser_bound_encoding
                         ? "parser_bound_sblr_codegen_ready_verified"
                         : "parser_boundary_verified") +
         "\","
         "\"parser_storage_authority\":false,"
         "\"parser_transaction_finality_authority\":false,"
         "\"parser_sequence_value_authority\":false,"
         "\"parser_source_execution_authority\":false,"
         "\"compatibility_sql_executed\":false,"
         "\"original_source_usage\":\"catalog_audit_reference_only\","
         "\"original_source_executed\":false,"
         "\"catalog_source_reference_execute_allowed\":false,"
         "\"enterprise_readiness\":\"reference_parser_implementation_proven\"}";
}

std::uint64_t Fnv1a64(std::string_view text) {
  std::uint64_t hash = 14695981039346656037ull;
  for (const char ch : text) {
    hash ^= static_cast<unsigned char>(ch);
    hash *= 1099511628211ull;
  }
  return hash;
}

ProceduralSourceRetentionMetadata ProceduralSourceRetentionMetadataFor(
    std::string_view normalized_sql,
    std::span<const Token> tokens,
    ProceduralFunctionalEncodingSpanMetadata span_metadata) {
  ProceduralSourceRetentionMetadata metadata;
  metadata.source_byte_length = normalized_sql.size();
  metadata.source_hash = Fnv1a64(normalized_sql);
  metadata.body_end_byte = normalized_sql.size();

  metadata.header_source_span_count =
      span_metadata.header_source_span_count;
  metadata.body_source_span_count = span_metadata.body_source_span_count;
  if (metadata.header_source_span_count > 0 &&
      metadata.body_source_span_count > 0) {
    metadata.parser_bound_sblr_body_instruction_stream = true;
    metadata.uuid_dependency_bindings_bound = true;
  }

  std::vector<std::size_t> semantic_token_indexes;
  semantic_token_indexes.reserve(tokens.size());
  for (std::size_t i = 0; i < tokens.size(); ++i) {
    if (!IsNoiseToken(tokens[i])) semantic_token_indexes.push_back(i);
  }

  const std::size_t body_semantic_index =
      metadata.header_source_span_count < semantic_token_indexes.size()
          ? metadata.header_source_span_count
          : semantic_token_indexes.size();
  if (body_semantic_index < semantic_token_indexes.size()) {
    metadata.body_start_byte =
        tokens[semantic_token_indexes[body_semantic_index]].offset;
  } else {
    metadata.body_start_byte = normalized_sql.size();
  }
  metadata.header_start_byte = 0;
  metadata.header_end_byte = metadata.body_start_byte;
  return metadata;
}

std::string TrimAscii(std::string_view text) {
  return std::string(TrimAsciiView(text));
}

std::string NormalizeWhitespace(std::string_view text) {
  std::string normalized;
  normalized.reserve(text.size());
  bool in_space = false;
  for (const char ch : TrimAsciiView(text)) {
    if (std::isspace(static_cast<unsigned char>(ch)) != 0) {
      if (!in_space) normalized.push_back(' ');
      in_space = true;
      continue;
    }
    normalized.push_back(ch);
    in_space = false;
  }
  if (!normalized.empty() && normalized.back() == ';') {
    normalized.pop_back();
    while (!normalized.empty() && normalized.back() == ' ') normalized.pop_back();
  }
  return normalized;
}

std::string ToUpperAscii(std::string_view text) {
  std::string upper;
  upper.reserve(text.size());
  for (const char ch : text) {
    upper.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(ch))));
  }
  return upper;
}

std::string EscapeJson(std::string_view text) {
  std::string escaped;
  escaped.reserve(text.size());
  for (const char ch : text) {
    switch (ch) {
      case '\\': escaped += "\\\\"; break;
      case '"': escaped += "\\\""; break;
      case '\n': escaped += "\\n"; break;
      case '\r': escaped += "\\r"; break;
      case '\t': escaped += "\\t"; break;
      default: escaped += ch; break;
    }
  }
  return escaped;
}

std::string MessageVectorToJson(const std::vector<Diagnostic>& diagnostics) {
  std::ostringstream out;
  out << "{\"diagnostics\":[";
  for (std::size_t i = 0; i < diagnostics.size(); ++i) {
    if (i != 0) out << ',';
    const auto& diagnostic = diagnostics[i];
    out << "{\"code\":\"" << EscapeJson(diagnostic.code)
        << "\",\"severity\":\"" << EscapeJson(diagnostic.severity)
        << "\",\"message\":\"" << EscapeJson(diagnostic.message)
        << "\",\"component\":\"" << EscapeJson(diagnostic.component)
        << "\",\"fields\":{";
    for (std::size_t j = 0; j < diagnostic.fields.size(); ++j) {
      if (j != 0) out << ',';
      out << "\"" << EscapeJson(diagnostic.fields[j].name)
          << "\":\"" << EscapeJson(diagnostic.fields[j].value) << "\"";
    }
    out << "}}";
  }
  out << "]}";
  return out.str();
}

std::vector<Token> LexTokens(std::string_view sql_text) {
  std::vector<Token> tokens;
  for (std::size_t i = 0; i < sql_text.size();) {
    const char ch = sql_text[i];
    if (std::isspace(static_cast<unsigned char>(ch)) != 0) {
      ++i;
      continue;
    }
    const char next = i + 1 < sql_text.size() ? sql_text[i + 1] : '\0';
    if (ch == '-' && next == '-') {
      const auto start = i;
      i += 2;
      while (i < sql_text.size() && sql_text[i] != '\n') ++i;
      tokens.push_back({"line_comment", std::string(sql_text.substr(start, i - start)), start});
      continue;
    }
    if (ch == '#') {
      const auto start = i++;
      while (i < sql_text.size() && sql_text[i] != '\n') ++i;
      tokens.push_back({"line_comment", std::string(sql_text.substr(start, i - start)), start});
      continue;
    }
    if (ch == '/' && next == '*') {
      const auto start = i;
      i += 2;
      while (i + 1 < sql_text.size() && !(sql_text[i] == '*' && sql_text[i + 1] == '/')) ++i;
      if (i + 1 < sql_text.size()) i += 2;
      tokens.push_back({"block_comment", std::string(sql_text.substr(start, i - start)), start});
      continue;
    }
    if (ch == '\'' || ch == '"') {
      const auto quote = ch;
      const auto start = i++;
      while (i < sql_text.size()) {
        if (sql_text[i] == quote && i + 1 < sql_text.size() && sql_text[i + 1] == quote) {
          i += 2;
          continue;
        }
        if (sql_text[i] == '\\' && quote == '\'' && i + 1 < sql_text.size()) {
          i += 2;
          continue;
        }
        if (sql_text[i++] == quote) break;
      }
      tokens.push_back({quote == '\'' ? "string_literal" : "quoted_identifier",
                        std::string(sql_text.substr(start, i - start)), start});
      continue;
    }
    if (ch == '`') {
      const auto start = i++;
      while (i < sql_text.size()) {
        if (sql_text[i] == '`' && i + 1 < sql_text.size() && sql_text[i + 1] == '`') {
          i += 2;
          continue;
        }
        if (sql_text[i++] == '`') break;
      }
      tokens.push_back({"quoted_identifier", std::string(sql_text.substr(start, i - start)), start});
      continue;
    }
    if (ch == ':' || ch == '?' || ch == '$' || ch == '@') {
      const auto start = i++;
      while (i < sql_text.size() && IsIdentifierChar(sql_text[i])) ++i;
      tokens.push_back({"parameter_or_variable", std::string(sql_text.substr(start, i - start)), start});
      continue;
    }
    if (std::isalpha(static_cast<unsigned char>(ch)) != 0 || ch == '_') {
      const auto start = i++;
      while (i < sql_text.size() && IsIdentifierChar(sql_text[i])) ++i;
      tokens.push_back({"identifier_or_keyword", std::string(sql_text.substr(start, i - start)), start});
      continue;
    }
    if (std::isdigit(static_cast<unsigned char>(ch)) != 0) {
      const auto start = i++;
      while (i < sql_text.size() &&
             (std::isalnum(static_cast<unsigned char>(sql_text[i])) != 0 ||
              sql_text[i] == '.' || sql_text[i] == '_')) {
        ++i;
      }
      tokens.push_back({"numeric_literal", std::string(sql_text.substr(start, i - start)), start});
      continue;
    }
    tokens.push_back({"symbol", std::string(1, ch), i});
    ++i;
  }
  return tokens;
}

ParseResult ParseStatement(std::string_view sql_text, const DialectProfile& profile) {
  const auto normalized = NormalizeWhitespace(sql_text);
  const auto active_normalized = MaskInactiveSqlText(normalized);
  const auto upper = ToUpperAscii(normalized);
  const auto active_upper = ToUpperAscii(active_normalized);
  if (upper.empty()) {
    return Reject(profile, std::string(profile.diagnostic_prefix) + ".PARSE.EMPTY",
                  "SQL input is empty.");
  }
  if (!HasBalancedDelimiters(normalized)) {
    return Reject(profile, std::string(profile.diagnostic_prefix) + ".PARSE.INVALID_INPUT",
                  "SQL input has unbalanced delimiters.");
  }
  if (IsCatalogMutation(active_upper, profile)) {
    return Reject(profile, std::string(profile.diagnostic_prefix) + ".CATALOG_OVERLAY.READ_ONLY",
                  "Compatibility catalog overlays are read-only projections.",
                  {{"dialect", std::string(profile.dialect_id)}});
  }
  const auto tokens = LexTokens(normalized);
  auto parser_evidence = BuildParserEvidence(active_upper, tokens, profile);

  for (const auto& pattern : profile.patterns) {
    if (!Matches(active_upper, tokens, pattern)) continue;
    parser_evidence.datatype_descriptor_evidence_required =
        pattern.statement_family == std::string_view("ddl") &&
        parser_evidence.datatype_reference_count > 0;
    parser_evidence.index_semantic_defaults_evidence_required =
        pattern.statement_family == std::string_view("ddl") &&
        profile.semantic_policy != nullptr &&
        profile.semantic_policy->is_index_semantic_defaults_statement != nullptr &&
        profile.semantic_policy->index_semantic_defaults_evidence_json != nullptr &&
        profile.semantic_policy->is_index_semantic_defaults_statement(active_upper);
    if (parser_evidence.index_semantic_defaults_evidence_required) {
      parser_evidence.index_semantic_defaults_upper_sql = active_upper;
    }
    parser_evidence.constraint_semantic_defaults_evidence_required =
        pattern.statement_family == std::string_view("ddl") &&
        profile.semantic_policy != nullptr &&
        profile.semantic_policy->is_constraint_semantic_defaults_statement != nullptr &&
        profile.semantic_policy->constraint_semantic_defaults_evidence_json != nullptr &&
        profile.semantic_policy->is_constraint_semantic_defaults_statement(active_upper);
    if (parser_evidence.constraint_semantic_defaults_evidence_required) {
      parser_evidence.constraint_semantic_defaults_upper_sql = active_upper;
    }
    parser_evidence.sequence_identity_semantic_evidence_required =
        profile.semantic_policy != nullptr &&
        profile.semantic_policy->is_sequence_identity_statement != nullptr &&
        profile.semantic_policy->sequence_identity_evidence_json != nullptr &&
        profile.semantic_policy->is_sequence_identity_statement(active_upper);
    if (parser_evidence.sequence_identity_semantic_evidence_required) {
      parser_evidence.sequence_identity_semantic_upper_sql = active_upper;
    }
    parser_evidence.identifier_name_resolution_evidence_required =
        pattern.statement_family == std::string_view("ddl") &&
        profile.semantic_policy != nullptr &&
        profile.semantic_policy->is_identifier_name_resolution_statement !=
            nullptr &&
        profile.semantic_policy->identifier_name_resolution_evidence_json !=
            nullptr &&
        profile.semantic_policy->is_identifier_name_resolution_statement(
            active_upper);
    if (parser_evidence.identifier_name_resolution_evidence_required) {
      parser_evidence.identifier_name_resolution_upper_sql = active_upper;
    }
    parser_evidence.scalar_expression_semantic_evidence_required =
        pattern.statement_family == std::string_view("query") &&
        profile.semantic_policy != nullptr &&
        profile.semantic_policy->is_scalar_expression_semantic_statement !=
            nullptr &&
        profile.semantic_policy->scalar_expression_semantic_evidence_json !=
            nullptr &&
        profile.semantic_policy->is_scalar_expression_semantic_statement(
            active_upper);
    if (parser_evidence.scalar_expression_semantic_evidence_required) {
      parser_evidence.scalar_expression_semantic_upper_sql = active_upper;
    }
    parser_evidence.dml_mutation_semantic_evidence_required =
        pattern.statement_family == std::string_view("dml") &&
        profile.semantic_policy != nullptr &&
        profile.semantic_policy->is_dml_mutation_semantic_statement != nullptr &&
        profile.semantic_policy->dml_mutation_semantic_evidence_json != nullptr &&
        profile.semantic_policy->is_dml_mutation_semantic_statement(active_upper);
    if (parser_evidence.dml_mutation_semantic_evidence_required) {
      parser_evidence.dml_mutation_semantic_upper_sql = active_upper;
    }
    parser_evidence.transaction_session_semantic_evidence_required =
        (pattern.statement_family == std::string_view("transaction") ||
         pattern.statement_family == std::string_view("session")) &&
        profile.semantic_policy != nullptr &&
        profile.semantic_policy->is_transaction_session_statement != nullptr &&
        profile.semantic_policy->transaction_session_evidence_json != nullptr &&
        profile.semantic_policy->is_transaction_session_statement(active_upper);
    if (parser_evidence.transaction_session_semantic_evidence_required) {
      parser_evidence.transaction_session_semantic_upper_sql = active_upper;
    }
    parser_evidence.temporary_session_object_semantic_evidence_required =
        pattern.statement_family == std::string_view("ddl") &&
        profile.semantic_policy != nullptr &&
        profile.semantic_policy->is_temporary_session_object_statement != nullptr &&
        profile.semantic_policy->temporary_session_object_evidence_json != nullptr &&
        profile.semantic_policy->is_temporary_session_object_statement(active_upper);
    if (parser_evidence.temporary_session_object_semantic_evidence_required) {
      parser_evidence.temporary_session_object_semantic_upper_sql =
          active_upper;
    }
    parser_evidence.dependency_bearing_ddl_semantic_evidence_required =
        (pattern.statement_family == std::string_view("ddl") ||
         pattern.statement_family == std::string_view("routine")) &&
        profile.semantic_policy != nullptr &&
        profile.semantic_policy->is_dependency_bearing_ddl_statement != nullptr &&
        profile.semantic_policy->dependency_bearing_ddl_evidence_json != nullptr &&
        profile.semantic_policy->is_dependency_bearing_ddl_statement(active_upper);
    if (parser_evidence.dependency_bearing_ddl_semantic_evidence_required) {
      parser_evidence.dependency_bearing_ddl_semantic_upper_sql =
          active_upper;
    }
    parser_evidence.ddl_transaction_behavior_semantic_evidence_required =
        pattern.statement_family == std::string_view("ddl") &&
        profile.semantic_policy != nullptr &&
        profile.semantic_policy->is_ddl_transaction_behavior_statement != nullptr &&
        profile.semantic_policy->ddl_transaction_behavior_evidence_json != nullptr &&
        profile.semantic_policy->is_ddl_transaction_behavior_statement(active_upper);
    if (parser_evidence.ddl_transaction_behavior_semantic_evidence_required) {
      parser_evidence.ddl_transaction_behavior_semantic_upper_sql =
          active_upper;
    }
    parser_evidence.resource_text_semantic_evidence_required =
        (pattern.statement_family == std::string_view("ddl") ||
         pattern.statement_family == std::string_view("dml") ||
         pattern.statement_family == std::string_view("query")) &&
        profile.semantic_policy != nullptr &&
        profile.semantic_policy->is_resource_text_statement != nullptr &&
        profile.semantic_policy->resource_text_evidence_json != nullptr &&
        profile.semantic_policy->is_resource_text_statement(active_upper);
    if (parser_evidence.resource_text_semantic_evidence_required) {
      parser_evidence.resource_text_semantic_upper_sql = active_upper;
    }
    parser_evidence.statistics_optimizer_semantic_evidence_required =
        profile.semantic_policy != nullptr &&
        profile.semantic_policy->is_statistics_optimizer_statement != nullptr &&
        profile.semantic_policy->statistics_optimizer_evidence_json != nullptr &&
        profile.semantic_policy->is_statistics_optimizer_statement(active_upper);
    if (parser_evidence.statistics_optimizer_semantic_evidence_required) {
      parser_evidence.statistics_optimizer_semantic_upper_sql = active_upper;
    }
    parser_evidence.locks_isolation_semantic_evidence_required =
        profile.semantic_policy != nullptr &&
        profile.semantic_policy->is_locks_isolation_statement != nullptr &&
        profile.semantic_policy->locks_isolation_evidence_json != nullptr &&
        profile.semantic_policy->is_locks_isolation_statement(active_upper);
    if (parser_evidence.locks_isolation_semantic_evidence_required) {
      parser_evidence.locks_isolation_semantic_upper_sql = active_upper;
    }
    parser_evidence.system_catalog_defaults_semantic_evidence_required =
        profile.semantic_policy != nullptr &&
        profile.semantic_policy->is_system_catalog_defaults_statement != nullptr &&
        profile.semantic_policy->system_catalog_defaults_evidence_json != nullptr &&
        profile.semantic_policy->is_system_catalog_defaults_statement(active_upper);
    if (parser_evidence.system_catalog_defaults_semantic_evidence_required) {
      parser_evidence.system_catalog_defaults_semantic_operation_id =
          pattern.mapping_key;
    }
    parser_evidence.session_settings_diagnostics_semantic_evidence_required =
        profile.semantic_policy != nullptr &&
        profile.semantic_policy->is_session_settings_diagnostics_statement != nullptr &&
        profile.semantic_policy->session_settings_diagnostics_evidence_json != nullptr &&
        profile.semantic_policy->is_session_settings_diagnostics_statement(active_upper);
    if (parser_evidence.session_settings_diagnostics_semantic_evidence_required) {
      parser_evidence.session_settings_diagnostics_semantic_upper_sql =
          active_upper;
    }
    parser_evidence.procedural_body_source_retention_required =
        IsProceduralBodySourceRetentionStatement(pattern.statement_family,
                                                pattern.operation_family,
                                                active_upper);
    if (parser_evidence.procedural_body_source_retention_required) {
      if (profile.semantic_policy != nullptr &&
          profile.semantic_policy
                  ->procedural_functional_encoding_span_metadata != nullptr) {
        parser_evidence.procedural_span_metadata =
            profile.semantic_policy
                ->procedural_functional_encoding_span_metadata(active_upper,
                                                               tokens);
      }
      parser_evidence.procedural_source_retention_metadata =
          ProceduralSourceRetentionMetadataFor(
              normalized, tokens, parser_evidence.procedural_span_metadata);
    }

    ParseResult result;
    result.ok = true;
    result.normalized_sql = normalized;
    result.statement_family = std::string(pattern.statement_family);
    result.operation_family = std::string(pattern.operation_family);
    result.lifecycle_operation_id = std::string(pattern.mapping_key);
    result.sblr_operation = NormalizeCompatibilitySblrOpcode(pattern.sblr_operation);
    result.sblr_operation_family = std::string(profile.sblr_operation_family);
    result.engine_api_function = std::string(pattern.engine_api_function);
    result.lifecycle_mapping_key = std::string(pattern.mapping_key);
    result.emulation_diagnostic_code = std::string(pattern.diagnostic_code);
    result.authority_disposition = MappingDispositionName(pattern.disposition);
    result.scratchbird_lifecycle_api =
        pattern.disposition == MappingDisposition::kScratchBirdLifecycleApi;
    result.parser_support_udr_route =
        pattern.disposition == MappingDisposition::kParserSupportUdr;
    result.catalog_projection_only =
        pattern.disposition == MappingDisposition::kCatalogProjection;
    result.exact_emulated_diagnostic =
        pattern.disposition == MappingDisposition::kPolicyRefusal ||
        pattern.disposition == MappingDisposition::kSecurityRefusal ||
        pattern.disposition == MappingDisposition::kUnsupportedRefusal;
    result.fail_closed_refusal = result.exact_emulated_diagnostic;
    result.parser_evidence_json = ParserEvidenceJson(profile, parser_evidence);
    result.sblr_envelope = MakeSblrEnvelope(profile, pattern, parser_evidence);

    std::vector<Diagnostic> diagnostics;
    if (!pattern.diagnostic_code.empty()) {
      diagnostics.push_back(MakeDiagnostic(
          std::string(pattern.diagnostic_code),
          result.fail_closed_refusal ? "ERROR" : "INFO",
          std::string(pattern.diagnostic_message),
          std::string(profile.parser_package_name) + ".parser",
          {{"dialect", std::string(profile.dialect_id)},
           {"operation_family", result.operation_family},
           {"authority_disposition", result.authority_disposition}}));
    }
    result.message_vector_json = MessageVectorToJson(diagnostics);
    return result;
  }

  return Reject(profile, std::string(profile.diagnostic_prefix) + ".PARSE.UNSUPPORTED_SURFACE",
                "SQL input is not assigned to a compatibility parser surface.",
                {{"dialect", std::string(profile.dialect_id)},
                 {"normalized_prefix", upper.substr(0, upper.size() < 80 ? upper.size() : 80)}});
}

std::string PackageIdentityJson(const DialectProfile& profile) {
  const std::string family_uuid =
      "parser.compatibility." + std::string(profile.dialect_id);
  const std::string pipeline_target =
      "sbl_" + std::string(profile.dialect_id) + "_parser_pipeline";
  const std::string support_target =
      "sbu_" + std::string(profile.dialect_id) + "_parser_support";
  std::ostringstream out;
  out << "{\"dialect\":\"" << EscapeJson(profile.dialect_id)
      << "\",\"display_name\":\"" << EscapeJson(profile.display_name)
      << "\",\"parser_package\":\"" << EscapeJson(profile.parser_package_name)
      << "\",\"parser_support_package\":\""
      << EscapeJson(profile.parser_support_package_name)
      << "\",\"release_profile\":\"" << EscapeJson(profile.release_profile)
      << "\",\"authority_policy\":\"engine_sblr_mga_only\""
      << ",\"reference_sql_execution\":false"
      << ",\"reference_storage_authority\":false"
      << ",\"reference_recovery_authority\":false"
      << ",\"standalone_dialect_package\":true"
      << ",\"parser_family_uuid\":\"" << EscapeJson(family_uuid) << '"'
      << ",\"standalone_package\":true"
      << ",\"cross_parser_dependency_count\":0"
      << ",\"same_family_library_set\":["
      << "{\"target\":\"" << EscapeJson(profile.parser_package_name)
      << "\",\"artifact\":\"bin/" << EscapeJson(profile.parser_package_name)
      << "\",\"owner\":\"" << EscapeJson(family_uuid) << "\"},"
      << "{\"target\":\"" << EscapeJson(pipeline_target)
      << "\",\"artifact\":\"lib/lib" << EscapeJson(pipeline_target)
      << "\",\"owner\":\"" << EscapeJson(family_uuid) << "\"},"
      << "{\"target\":\"" << EscapeJson(support_target)
      << "\",\"artifact\":\"lib/lib" << EscapeJson(support_target)
      << "\",\"owner\":\"" << EscapeJson(family_uuid) << "\"}]"
      << ",\"neutral_dependency_set\":["
      << "{\"target\":\"sbl_compatibility_parser_common\",\"artifact\":\"lib/libsbl_compatibility_parser_common\",\"owner\":\"family_neutral\",\"version\":\"same-build\"},"
      << "{\"target\":\"sbl_listener_control_plane\",\"artifact\":\"lib/libsbl_listener_control_plane\",\"owner\":\"family_neutral\",\"version\":\"same-build\"},"
      << "{\"target\":\"sbl_manager_protocol\",\"artifact\":\"lib/libsbl_manager_protocol\",\"owner\":\"family_neutral\",\"version\":\"same-build\"},"
      << "{\"target\":\"sb_udr_runtime\",\"artifact\":\"lib/libsb_udr_runtime\",\"owner\":\"family_neutral\",\"version\":\"same-build\"},"
      << "{\"target\":\"sb_core_memory\",\"artifact\":\"lib/libsb_core_memory\",\"owner\":\"scratchbird_engine\",\"version\":\"same-build\"},"
      << "{\"target\":\"sb_core_metrics\",\"artifact\":\"lib/libsb_core_metrics\",\"owner\":\"scratchbird_engine\",\"version\":\"same-build\"},"
      << "{\"target\":\"sb_core_platform\",\"artifact\":\"lib/libsb_core_platform\",\"owner\":\"scratchbird_engine\",\"version\":\"same-build\"},"
      << "{\"target\":\"OpenSSL::Crypto\",\"artifact\":\"system/libcrypto\",\"owner\":\"system_neutral\",\"version\":\"resolved-at-build\"}]"
      << ",\"parser_support_udr_family_uuid\":\"" << EscapeJson(family_uuid)
      << '"'
      << ",\"direct_sblr_lowering\":true"
      << ",\"foreign_parser_fallback\":false"
      << ",\"isolated_build_profile\":\"parser-family-isolated-release-v1\""
      << ",\"isolated_package_profile\":\"parser-family-empty-prefix-v1\""
      << ",\"dependency_closure_evidence\":{"
      << "\"source\":\"parser_family_isolation_evidence.json#source_ownership_scan\","
      << "\"build_graph\":\"parser_family_isolation_evidence.json#build_graph_ownership_scan\","
      << "\"link\":\"parser_family_binary_isolation_evidence.json#project_target_link_command_scan\","
      << "\"symbol\":\"parser_family_binary_isolation_evidence.json#binary_and_archive_symbol_scan\","
      << "\"package\":\"parser_family_package_isolation_evidence.json#empty_prefix_package_closure\","
      << "\"runtime\":\"parser_family_binary_isolation_evidence.json#staged_identity_probe_trace\"}"
      << ",\"surface_counts\":{"
      << "\"parser_surface_rows\":" << profile.parser_surface_rows << ','
      << "\"function_api_rows\":" << profile.function_api_rows << ','
      << "\"compatibility_alias_rows\":"
      << profile.compatibility_alias_rows << ','
      << "\"core_or_optional_alias_rows\":"
      << profile.core_or_optional_alias_rows << ','
      << "\"catalog_projection_only_rows\":"
      << profile.catalog_projection_only_rows << ','
      << "\"connector_operation_rows\":"
      << profile.connector_operation_rows << ','
      << "\"policy_blocked_rows\":" << profile.policy_blocked_rows << ','
      << "\"trusted_udr_registration_rows\":"
      << profile.trusted_udr_registration_rows << ','
      << "\"unsupported_rows\":" << profile.unsupported_rows
      << "}}";
  return out.str();
}

std::string SurfaceReportJson(const DialectProfile& profile) {
  auto emit_surface_array = [](std::ostringstream& out,
                               std::string_view name,
                               std::span<const SurfaceDescriptor> surfaces) {
    out << "\"" << name << "\":[";
    for (std::size_t i = 0; i < surfaces.size(); ++i) {
      if (i != 0) out << ',';
      out << "{\"family\":\"" << EscapeJson(surfaces[i].family)
          << "\",\"surface\":\"" << EscapeJson(surfaces[i].surface)
          << "\",\"owner\":\"" << EscapeJson(surfaces[i].owner) << "\"}";
    }
    out << "]";
  };

  std::ostringstream out;
  out << "{\"dialect\":\"" << EscapeJson(profile.dialect_id) << "\",";
  emit_surface_array(out, "datatype_surfaces", profile.datatype_surfaces);
  out << ',';
  emit_surface_array(out, "builtin_function_surfaces", profile.builtin_function_surfaces);
  out << ',';
  emit_surface_array(out, "catalog_overlay_surfaces", profile.catalog_overlay_surfaces);
  out << ',';
  emit_surface_array(out, "diagnostic_surfaces", profile.diagnostic_surfaces);
  out << "}";
  return out.str();
}

std::string ConnectionSandboxReportJson(const DialectProfile& profile) {
  std::ostringstream out;
  out << "{\"ok\":true"
      << ",\"dialect\":\"" << EscapeJson(profile.dialect_id) << "\""
      << ",\"connection_sandbox_contract\":\"compatibility_connection_schema_root_v1\""
      << ",\"schema_root_source\":\"listener_engine_materialized_attach_context\""
      << ",\"user_object_resolution\":\"relative_to_connection_schema_root\""
      << ",\"unqualified_name_root\":\"reference_schema_branch_root\""
      << ",\"direct_cross_root_access\":\"unsupported_denied\""
      << ",\"server_local_file_access\":\"default_denied\""
      << ",\"tenant_escape_policy\":\"fail_closed\""
      << ",\"catalog_projection_authority\":\"catalog_emulation_definer_authority\""
      << ",\"catalog_projection_can_query_outside_sandbox\":true"
      << ",\"catalog_projection_user_authority\":false"
      << ",\"catalog_projection_select_grant_required\":true"
      << ",\"catalog_projection_output_is_user_visible\":true"
      << ",\"catalog_projection_does_not_grant_base_object_access\":true"
      << ",\"sbsql_global_tree_visibility_inherited\":false"
      << ",\"sbsql_global_tree_visibility\":\"sbsql_only\""
      << ",\"engine_authorization_authority\":\"scratchbird_engine\""
      << ",\"parser_authorization_authority\":false"
      << ",\"parser_storage_authority\":false"
      << ",\"parser_recovery_authority\":false"
      << ",\"mga_transaction_authority\":\"scratchbird_engine\""
      << ",\"schema_root_is_user_visible_root\":true"
      << ",\"materialized_authorization_required\":true"
      << ",\"search_path_outside_root_policy\":\"refuse_without_catalog_definer_projection\""
      << ",\"catalog_security_filter\":\"engine_materialized_grants_plus_projection_definer_grants\""
      << "}";
  return out.str();
}

std::string DialectVariantReportJson(const DialectProfile& profile) {
  constexpr std::array<std::string_view, 1> kDefaultVariants{
      "primary_reference_language"};
  const std::span<const std::string_view> variants =
      profile.dialect_variants.empty()
          ? std::span<const std::string_view>(kDefaultVariants)
          : profile.dialect_variants;

  std::ostringstream out;
  out << "{\"ok\":true"
      << ",\"dialect\":\"" << EscapeJson(profile.dialect_id) << "\""
      << ",\"dialect_variant_contract\":\"compatibility_supported_variant_surface_v1\""
      << ",\"variant_selection_authority\":\"listener_profile_and_engine_attach_context\""
      << ",\"parser_cross_dialect_detection\":false"
      << ",\"parser_cross_dialect_dispatch\":false"
      << ",\"sbsql_variant_admitted\":false"
      << ",\"reasonable_subset_policy\":\"declared_and_tested_per_compatibility_variant\""
      << ",\"variant_count\":" << variants.size()
      << ",\"variants\":[";
  for (std::size_t i = 0; i < variants.size(); ++i) {
    if (i != 0) out << ',';
    out << "\"" << EscapeJson(variants[i]) << "\"";
  }
  out << "]}";
  return out.str();
}

std::string MappingDispositionName(MappingDisposition disposition) {
  switch (disposition) {
    case MappingDisposition::kAdmittedSblr:
      return "admitted_sblr";
    case MappingDisposition::kScratchBirdLifecycleApi:
      return "scratchbird_lifecycle_api";
    case MappingDisposition::kParserSupportUdr:
      return "parser_support_udr";
    case MappingDisposition::kCatalogProjection:
      return "catalog_projection";
    case MappingDisposition::kPolicyRefusal:
      return "policy_refusal_fail_closed";
    case MappingDisposition::kSecurityRefusal:
      return "security_refusal_fail_closed";
    case MappingDisposition::kUnsupportedRefusal:
      return "unsupported_refusal_fail_closed";
  }
  return "unknown";
}

} // namespace scratchbird::parser::compatibility

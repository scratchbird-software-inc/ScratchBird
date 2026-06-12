// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "lexer/lexer.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <string_view>
#include <unordered_set>

namespace scratchbird::parser::sbsql {
namespace {

bool IsAsciiAlpha(char c) {
  return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

bool IsAsciiDigit(char c) { return c >= '0' && c <= '9'; }

bool IsAsciiHex(char c) {
  return IsAsciiDigit(c) || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f');
}

bool IsUtf8Continuation(unsigned char c) { return (c & 0xC0) == 0x80; }

bool IsUnicodeBidiControl(std::uint32_t cp) {
  return (cp >= 0x202A && cp <= 0x202E) || (cp >= 0x2066 && cp <= 0x2069);
}

bool IsUnicodeCombiningMark(std::uint32_t cp) {
  return (cp >= 0x0300 && cp <= 0x036F) ||
         (cp >= 0x0591 && cp <= 0x05BD) || cp == 0x05BF ||
         (cp >= 0x05C1 && cp <= 0x05C2) ||
         (cp >= 0x05C4 && cp <= 0x05C5) || cp == 0x05C7 ||
         (cp >= 0x0610 && cp <= 0x061A) ||
         (cp >= 0x064B && cp <= 0x065F) || cp == 0x0670 ||
         (cp >= 0x06D6 && cp <= 0x06DC) ||
         (cp >= 0x06DF && cp <= 0x06E4) ||
         (cp >= 0x06E7 && cp <= 0x06E8) ||
         (cp >= 0x06EA && cp <= 0x06ED) ||
         (cp >= 0x1AB0 && cp <= 0x1AFF) ||
         (cp >= 0x1DC0 && cp <= 0x1DFF) ||
         (cp >= 0x20D0 && cp <= 0x20FF) ||
         (cp >= 0xFE20 && cp <= 0xFE2F);
}

bool CanAnchorCombiningMark(std::uint32_t cp) {
  if (IsUnicodeBidiControl(cp) || IsUnicodeCombiningMark(cp)) return false;
  if (cp < 0x80) {
    const char c = static_cast<char>(cp);
    return IsAsciiAlpha(c) || IsAsciiDigit(c) || c == '_' || c == '$';
  }
  return true;
}

bool IsIdentifierStart(char c) {
  const auto uc = static_cast<unsigned char>(c);
  return IsAsciiAlpha(c) || c == '_' || uc >= 0x80;
}

bool IsIdentifierContinue(char c) {
  const auto uc = static_cast<unsigned char>(c);
  return IsAsciiAlpha(c) || IsAsciiDigit(c) || c == '_' || c == '$' || uc >= 0x80;
}

bool IsParameterIdentifierContinue(char c) {
  return IsIdentifierContinue(c) || c == '.';
}

bool IsHorizontalWhitespace(char c) {
  return c == ' ' || c == '\t' || c == '\r' || c == '\f' || c == '\v';
}

bool IsKeywordBoundary(char c) {
  return !IsIdentifierContinue(c);
}

bool Contains(const std::unordered_set<std::string>& values, std::string_view value) {
  return values.contains(std::string(value));
}

std::string CanonicalTokenId(TokenKind kind, std::string_view canonical_text) {
  switch (kind) {
    case TokenKind::kKeyword:
      return std::string("SBSQL.TOKEN.") + std::string(canonical_text);
    case TokenKind::kIdentifier:
      return "SBSQL.TOKEN.IDENTIFIER";
    case TokenKind::kNumericLiteral:
      return "SBSQL.TOKEN.NUMERIC_LITERAL";
    case TokenKind::kStringLiteral:
      return "SBSQL.TOKEN.STRING_LITERAL";
    case TokenKind::kBinaryLiteral:
      return "SBSQL.TOKEN.BINARY_LITERAL";
    case TokenKind::kTemporalLiteral:
      return "SBSQL.TOKEN.TEMPORAL_LITERAL";
    case TokenKind::kUuidLiteral:
      return "SBSQL.TOKEN.UUID_LITERAL";
    case TokenKind::kBooleanLiteral:
      return "SBSQL.TOKEN.BOOLEAN_LITERAL";
    case TokenKind::kNullLiteral:
      return "SBSQL.TOKEN.NULL_LITERAL";
    case TokenKind::kDefaultLiteral:
      return "SBSQL.TOKEN.DEFAULT_LITERAL";
    case TokenKind::kDocumentLiteral:
      return "SBSQL.TOKEN.DOCUMENT_LITERAL";
    case TokenKind::kVectorLiteral:
      return "SBSQL.TOKEN.VECTOR_LITERAL";
    case TokenKind::kRegexLiteral:
      return "SBSQL.TOKEN.REGEX_LITERAL";
    case TokenKind::kRangeLiteral:
      return "SBSQL.TOKEN.RANGE_LITERAL";
    case TokenKind::kParameter:
      return "SBSQL.TOKEN.PARAMETER";
    case TokenKind::kVariable:
      return "SBSQL.TOKEN.VARIABLE";
    case TokenKind::kOperator:
      return "SBSQL.TOKEN.OPERATOR";
    case TokenKind::kSymbol:
      return "SBSQL.TOKEN.SYMBOL";
    case TokenKind::kStatementTerminator:
      return "SBSQL.TOKEN.STATEMENT_TERMINATOR";
    case TokenKind::kComment:
      return "SBSQL.TOKEN.COMMENT";
    case TokenKind::kWhitespace:
      return "SBSQL.TOKEN.WHITESPACE";
    case TokenKind::kMetaCommand:
      return "SBSQL.TOKEN.META_COMMAND";
    case TokenKind::kParserDirective:
      return "SBSQL.TOKEN.PARSER_DIRECTIVE";
    case TokenKind::kEnd:
      return "SBSQL.TOKEN.END";
  }
  return "SBSQL.TOKEN.UNKNOWN";
}

std::string KeywordClass(std::string_view text) {
  // SBsql is context-sensitive: command words are grammar tokens only in the
  // syntactic positions that ask for them. The lexer exposes a keyword class
  // for diagnostics and rendering, but it must not define a broad SQL reserved
  // word set.
  static const std::unordered_set<std::string> contextual = {
      "ADD",       "ALL",       "ALTER",      "AND",        "AS",
      "BEGIN",     "BY",        "CALL",       "CASE",       "CAST",
      "COMMIT",    "CREATE",    "DEALLOCATE", "DELETE",     "DISTINCT",   "DROP",
      "ELSE",      "END",       "EXCEPT",     "EXECUTE",    "EXISTS",
      "FALSE",     "FROM",      "FULL",       "GROUP",      "HAVING",
      "IN",        "INNER",     "INSERT",     "INTERSECT",  "INTO",
      "IS",        "JOIN",      "LEFT",       "LIKE",       "LIMIT",
      "MERGE",     "NOT",       "NULL",       "OFFSET",     "ON",
      "OR",        "ORDER",     "OUTER",      "PREPARE",    "REGISTER",   "RETURNING",  "RIGHT",
      "ROLLBACK",  "SAVEPOINT", "SELECT",     "SET",        "SHOW",
      "TABLE",     "THEN",      "TRUE",       "UNION",      "UPDATE",
      "UPSERT",    "VALUES",    "WHEN",       "WHERE",      "WITH",

      "ARCHIVE",     "ARRAY",       "BINARY",      "BOOLEAN",     "BUFFER",
      "CACHE",       "CATALOG",     "CHANGEFEED",  "COLLATE",     "CONFIG",
      "CANCEL",      "DATABASE",    "DATE",        "DEFAULT",     "DESCRIPTOR",  "DEVICE",
      "DOCUMENT",    "DOMAIN",      "FILES",       "FILESPACE",   "FILTER",
      "FUNCTION",    "GPU",         "GRAPH",       "IDENTITY",    "INDEX",
      "INTERVAL",    "JSON",        "JOB",         "LANGUAGE",    "LLVM",
      "MAP",         "METRICS",     "MIGRATION",   "MODULE",      "NATIONAL",
      "PACKAGE",     "PATH",        "PAUSE",       "POLICY",      "PROCEDURE",   "PROFILE",
      "RANGE",       "REGEX",       "RENAME",      "REPLACE",     "REPLICA",
      "REPLICATION", "RESTORE",     "RESUME",      "ROLE",        "ROUTINE",     "RUN",         "SCHEDULE",
      "SCHEMA",      "SECURITY",    "SESSION",     "SUPPORT",     "SYSTEM",
      "TIME",        "TIMESTAMP",   "TIMEZONE",    "TRANSACTION", "TRIGGER",
      "TUPLE",       "TYPE",        "UDR",         "UUID",        "VECTOR",
      "VIEW",        "WINDOW",      "XML",         "ZONE",
  };
  static const std::unordered_set<std::string> private_only = {
      "CLUSTER", "FAILOVER", "MEMBER", "NODE", "QUORUM", "RECONCILE",
      "THROTTLE", "TOPOLOGY",
  };
  static const std::unordered_set<std::string> reference = {
      "DELIMITER", "DESCRIBE", "EXPLAIN", "PRAGMA", "VACUUM",
  };
  static const std::unordered_set<std::string> refusal = {
      "MYSQL", "POSTGRESQL", "SQLITE", "WAL",
  };

  const auto upper = ToUpperAscii(text);
  if (Contains(private_only, upper)) return "private_cluster";
  if (Contains(contextual, upper)) return "contextual_native";
  if (Contains(reference, upper)) return "reference_contextual";
  if (Contains(refusal, upper)) return "refusal_only";
  return {};
}

bool IsTemporalLiteralPrefix(std::string_view upper) {
  return upper == "DATE" || upper == "TIME" || upper == "TIMESTAMP" ||
         upper == "INTERVAL";
}

bool IsStructuredLiteralPrefix(std::string_view upper) {
  return upper == "JSON" || upper == "DOCUMENT" || upper == "VECTOR" ||
         upper == "REGEX" || upper == "RANGE" || upper == "GRAPH" ||
         upper == "PATH";
}

bool IsCanonicalUuidText(std::string_view value) {
  if (value.size() != 36) return false;
  for (std::size_t index = 0; index < value.size(); ++index) {
    const bool hyphen = index == 8 || index == 13 || index == 18 || index == 23;
    if (hyphen) {
      if (value[index] != '-') return false;
    } else if (!IsAsciiHex(value[index])) {
      return false;
    }
  }
  return true;
}

TokenKind StructuredLiteralKind(std::string_view upper) {
  if (upper == "JSON" || upper == "DOCUMENT") return TokenKind::kDocumentLiteral;
  if (upper == "VECTOR") return TokenKind::kVectorLiteral;
  if (upper == "REGEX") return TokenKind::kRegexLiteral;
  if (upper == "RANGE") return TokenKind::kRangeLiteral;
  return TokenKind::kStringLiteral;
}

class Lexer {
 public:
  explicit Lexer(std::string_view source) : source_(source) {}

  LexResult Run() {
    ValidateUtf8Input();
    while (offset_ < source_.size()) {
      const char c = Peek();
      if (std::isspace(static_cast<unsigned char>(c))) {
        LexWhitespace();
      } else if (StartsWith("--") || StartsWith("//") || (c == '#' && Peek(1) != '>')) {
        LexLineComment();
      } else if (StartsWith("/*+")) {
        LexBlockLike(TokenKind::kParserDirective, "SBSQL.LEXER.DIRECTIVE_UNCLOSED",
                     "parser directive is not closed", true);
      } else if (StartsWith("/*")) {
        LexBlockLike(TokenKind::kComment, "SBSQL.LEX.UNTERMINATED_COMMENT",
                     "block comment is not closed", false);
      } else if (c == '"' || c == '`') {
        LexDelimitedIdentifier(c, c);
      } else if (c == '[') {
        if (PreviousNonTriviaTokenTextEquals("ARRAY")) {
          LexOperatorSymbolOrTerminator();
        } else {
          LexDelimitedIdentifier('[', ']');
        }
      } else if (c == '\'') {
        LexSingleQuoted(TokenKind::kStringLiteral, "string", offset_, "SBSQL.LEXER.STRING_UNCLOSED",
                        "string literal is not closed");
      } else if (c == '$') {
        if (!TryDollarString()) LexDollarParameterOrSymbol();
      } else if (IsIdentifierStart(c)) {
        LexIdentifierKeywordOrPrefixedLiteral();
      } else if (IsAsciiDigit(c)) {
        LexUuidOrNumber();
      } else if (c == '?' || c == ':' || c == '@') {
        LexParameterVariableOrOperator();
      } else if (c == '\\') {
        LexMetaCommand();
      } else if (static_cast<unsigned char>(c) < 0x20) {
        LexInvalidControl();
      } else {
        LexOperatorSymbolOrTerminator();
      }
    }

    AddToken(TokenKind::kEnd, source_.size(), line_, column_, source_.size(), line_,
             column_, "", false, "", "", "");
    return std::move(result_);
  }

 private:
  void AddUtf8Diagnostic(std::size_t offset,
                         std::size_t line,
                         std::size_t column) {
    result_.messages.diagnostics.push_back(MakeDiagnostic(
        "SBSQL.ENCODING.INVALID_UTF8", "ERROR",
        "input is not valid UTF-8 for the active parser profile",
        "sbp_sbsql.lexer",
        {{"byte_offset", std::to_string(offset)},
         {"encoding_name", "UTF-8"},
         {"line", std::to_string(line)},
         {"column", std::to_string(column)}}));
  }

  void AddUnicodeDiagnostic(std::string code,
                            std::string message,
                            std::uint32_t codepoint,
                            std::size_t offset,
                            std::size_t line,
                            std::size_t column) {
    result_.messages.diagnostics.push_back(MakeDiagnostic(
        std::move(code), "ERROR", std::move(message), "sbp_sbsql.lexer",
        {{"byte_offset", std::to_string(offset)},
         {"codepoint", std::to_string(codepoint)},
         {"line", std::to_string(line)},
         {"column", std::to_string(column)}}));
  }

  void ValidateUtf8Input() {
    bool emitted = false;
    std::size_t cursor = 0;
    std::size_t line = 1;
    std::size_t column = 1;
    bool previous_can_anchor_combining = false;
    while (cursor < source_.size()) {
      const auto c = static_cast<unsigned char>(source_[cursor]);
      if (c < 0x80) {
        if (source_[cursor] == '\n') {
          ++line;
          column = 1;
          previous_can_anchor_combining = false;
        } else {
          ++column;
          previous_can_anchor_combining = CanAnchorCombiningMark(c);
        }
        ++cursor;
        continue;
      }

      std::size_t length = 0;
      bool invalid = false;
      std::uint32_t codepoint = 0;
      if (c >= 0xC2 && c <= 0xDF) {
        length = 2;
        codepoint = c & 0x1Fu;
      } else if (c >= 0xE0 && c <= 0xEF) {
        length = 3;
        codepoint = c & 0x0Fu;
      } else if (c >= 0xF0 && c <= 0xF4) {
        length = 4;
        codepoint = c & 0x07u;
      } else {
        invalid = true;
      }

      if (!invalid && cursor + length > source_.size()) {
        invalid = true;
      }
      if (!invalid) {
        for (std::size_t i = 1; i < length; ++i) {
          const auto next = static_cast<unsigned char>(source_[cursor + i]);
          if (!IsUtf8Continuation(next)) {
            invalid = true;
            break;
          }
          codepoint = (codepoint << 6u) | (next & 0x3Fu);
        }
      }
      if (!invalid && length == 3) {
        const auto next = static_cast<unsigned char>(source_[cursor + 1]);
        if ((c == 0xE0 && next < 0xA0) || (c == 0xED && next >= 0xA0)) {
          invalid = true;
        }
      }
      if (!invalid && length == 4) {
        const auto next = static_cast<unsigned char>(source_[cursor + 1]);
        if ((c == 0xF0 && next < 0x90) || (c == 0xF4 && next > 0x8F)) {
          invalid = true;
        }
      }

      if (invalid) {
        if (!emitted) AddUtf8Diagnostic(cursor, line, column);
        emitted = true;
        ++cursor;
        ++column;
        previous_can_anchor_combining = false;
        continue;
      }
      if (IsUnicodeBidiControl(codepoint)) {
        AddUnicodeDiagnostic("SBSQL.UNICODE.BIDI_CONTROL_FORBIDDEN",
                             "Unicode bidi controls are not admitted in SBsql source",
                             codepoint, cursor, line, column);
        previous_can_anchor_combining = false;
      } else if (IsUnicodeCombiningMark(codepoint)) {
        if (!previous_can_anchor_combining) {
          AddUnicodeDiagnostic(
              "SBSQL.UNICODE.COMBINING_MARK_WITHOUT_BASE",
              "Unicode combining marks require a visible base code point in SBsql source",
              codepoint, cursor, line, column);
        }
      } else {
        previous_can_anchor_combining = CanAnchorCombiningMark(codepoint);
      }
      cursor += length;
      column += length;
    }
  }

  char Peek(std::size_t ahead = 0) const {
    const auto index = offset_ + ahead;
    return index < source_.size() ? source_[index] : '\0';
  }

  bool PreviousNonTriviaTokenTextEquals(std::string_view expected) const {
    for (auto it = result_.tokens.rbegin(); it != result_.tokens.rend(); ++it) {
      if (it->kind == TokenKind::kWhitespace ||
          it->kind == TokenKind::kComment ||
          it->kind == TokenKind::kParserDirective) {
        continue;
      }
      return ToUpperAscii(it->text) == ToUpperAscii(expected);
    }
    return false;
  }

  bool StartsWith(std::string_view prefix) const {
    return source_.substr(offset_, prefix.size()) == prefix;
  }

  void Advance() {
    if (offset_ >= source_.size()) return;
    const char c = source_[offset_++];
    if (c == '\n') {
      ++line_;
      column_ = 1;
    } else {
      ++column_;
    }
  }

  void Advance(std::size_t count) {
    while (count-- > 0) Advance();
  }

  void AddToken(TokenKind kind,
                std::size_t start,
                std::size_t start_line,
                std::size_t start_column,
                std::size_t end,
                std::size_t end_line,
                std::size_t end_column,
                std::string text,
                bool quoted,
                std::string literal_family,
                std::string keyword_class,
                std::string render_hint) {
    Token token;
    token.kind = kind;
    token.text = std::move(text);
    token.canonical_text = kind == TokenKind::kKeyword ? ToUpperAscii(token.text) : token.text;
    token.canonical_token_id = CanonicalTokenId(kind, token.canonical_text);
    token.canonical_alias_id = token.canonical_text == token.text ? "" : "alias." + token.text;
    token.offset = start;
    token.length = end - start;
    token.quoted = quoted;
    token.line = start_line;
    token.column = start_column;
    token.end_line = end_line;
    token.end_column = end_column;
    token.raw_text = std::string(source_.substr(start, end - start));
    token.literal_family = std::move(literal_family);
    token.keyword_class = std::move(keyword_class);
    token.render_hint = std::move(render_hint);
    result_.tokens.push_back(std::move(token));
  }

  void AddDiagnostic(std::string code,
                     std::string message,
                     std::size_t start,
                     std::size_t start_line,
                     std::size_t start_column) {
    result_.messages.diagnostics.push_back(MakeDiagnostic(
        std::move(code), "ERROR", std::move(message), "sbp_sbsql.lexer",
        {{"offset", std::to_string(start)},
         {"line", std::to_string(start_line)},
         {"column", std::to_string(start_column)}}));
  }

  void LexWhitespace() {
    const auto start = offset_;
    const auto start_line = line_;
    const auto start_column = column_;
    while (offset_ < source_.size() &&
           std::isspace(static_cast<unsigned char>(Peek()))) {
      Advance();
    }
    AddToken(TokenKind::kWhitespace, start, start_line, start_column, offset_, line_,
             column_, std::string(source_.substr(start, offset_ - start)), false, "",
             "", "preserve_trivia");
  }

  void LexLineComment() {
    const auto start = offset_;
    const auto start_line = line_;
    const auto start_column = column_;
    if (StartsWith("--") || StartsWith("//")) {
      Advance(2);
    } else {
      Advance();
    }
    while (offset_ < source_.size() && Peek() != '\n') Advance();
    const auto text = std::string(source_.substr(start, offset_ - start));
    const auto literal_family =
        text.rfind("--!", 0) == 0 ? "doc_comment" : "line_comment";
    AddToken(TokenKind::kComment, start, start_line, start_column, offset_, line_,
             column_, text, false, literal_family, "", "preserve_comment");
  }

  void LexBlockLike(TokenKind kind,
                    std::string_view diagnostic_code,
                    std::string_view diagnostic_message,
                    bool nested) {
    const auto start = offset_;
    const auto start_line = line_;
    const auto start_column = column_;
    Advance(2);
    if (kind == TokenKind::kParserDirective) Advance();
    int depth = 1;
    while (offset_ < source_.size()) {
      if (nested && StartsWith("/*")) {
        Advance(2);
        ++depth;
        continue;
      }
      if (StartsWith("*/")) {
        Advance(2);
        --depth;
        if (depth == 0) {
          const auto text = std::string(source_.substr(start, offset_ - start));
          const auto literal_family =
              kind == TokenKind::kComment
                  ? (text.rfind("/**", 0) == 0 ? "doc_comment" : "block_comment")
                  : "parser_directive";
          AddToken(kind, start, start_line, start_column, offset_, line_, column_,
                   text, false, literal_family, "",
                   kind == TokenKind::kComment ? "preserve_comment"
                                                : "preserve_directive");
          return;
        }
        continue;
      }
      Advance();
    }

    AddDiagnostic(std::string(diagnostic_code), std::string(diagnostic_message), start,
                  start_line, start_column);
    const auto text = std::string(source_.substr(start, offset_ - start));
    const auto literal_family =
        kind == TokenKind::kComment
            ? (text.rfind("/**", 0) == 0 ? "doc_comment" : "block_comment")
            : "parser_directive";
    AddToken(kind, start, start_line, start_column, offset_, line_, column_,
             text, false, literal_family, "", "unterminated");
  }

  bool ConsumeSingleQuoted(std::string* decoded) {
    if (Peek() != '\'') return false;
    Advance();
    while (offset_ < source_.size()) {
      if (Peek() == '\'') {
        if (Peek(1) == '\'') {
          decoded->push_back('\'');
          Advance(2);
          continue;
        }
        Advance();
        return true;
      }
      decoded->push_back(Peek());
      Advance();
    }
    return false;
  }

  void LexSingleQuoted(TokenKind kind,
                       std::string literal_family,
                       std::size_t start,
                       std::string_view diagnostic_code,
                       std::string_view diagnostic_message) {
    const auto start_line = line_;
    const auto start_column = column_;
    std::string decoded;
    const bool closed = ConsumeSingleQuoted(&decoded);
    if (!closed) {
      AddDiagnostic(std::string(diagnostic_code), std::string(diagnostic_message), start,
                    start_line, start_column);
    }
    AddToken(kind, start, start_line, start_column, offset_, line_, column_,
             std::move(decoded), true, std::move(literal_family), "",
             closed ? "quoted_literal" : "unterminated");
  }

  void LexDelimitedIdentifier(char open, char close) {
    const auto start = offset_;
    const auto start_line = line_;
    const auto start_column = column_;
    std::string decoded;
    Advance();
    bool closed = false;
    while (offset_ < source_.size()) {
      if (Peek() == close) {
        if (Peek(1) == close && open != '[') {
          decoded.push_back(close);
          Advance(2);
          continue;
        }
        Advance();
        closed = true;
        break;
      }
      decoded.push_back(Peek());
      Advance();
    }
    if (!closed) {
      AddDiagnostic(open == '[' ? "SBSQL.LEXER.DELIMITED_IDENTIFIER_UNCLOSED"
                                : "SBSQL.LEXER.IDENTIFIER_UNCLOSED",
                    "quoted identifier is not closed", start, start_line, start_column);
    }
    AddToken(TokenKind::kIdentifier, start, start_line, start_column, offset_, line_,
             column_, std::move(decoded), true, "delimited_identifier", "",
             closed ? "quoted_identifier" : "unterminated");
  }

  bool TryDollarString() {
    const auto start = offset_;
    const auto start_line = line_;
    const auto start_column = column_;
    if (Peek() != '$') return false;
    std::size_t cursor = offset_ + 1;
    while (cursor < source_.size() &&
           (IsAsciiAlpha(source_[cursor]) || IsAsciiDigit(source_[cursor]) ||
            source_[cursor] == '_')) {
      ++cursor;
    }
    if (cursor >= source_.size() || source_[cursor] != '$') return false;
    const std::string delimiter(source_.substr(offset_, cursor - offset_ + 1));
    Advance(delimiter.size());
    const auto content_start = offset_;
    while (offset_ < source_.size()) {
      if (source_.substr(offset_, delimiter.size()) == delimiter) {
        const auto content_end = offset_;
        Advance(delimiter.size());
        AddToken(TokenKind::kStringLiteral, start, start_line, start_column, offset_,
                 line_, column_,
                 std::string(source_.substr(content_start, content_end - content_start)),
                 true, "dollar_quoted_string", "", "quoted_literal");
        return true;
      }
      Advance();
    }
    AddDiagnostic("SBSQL.LEXER.STRING_UNCLOSED", "dollar-quoted string is not closed",
                  start, start_line, start_column);
    AddToken(TokenKind::kStringLiteral, start, start_line, start_column, offset_, line_,
             column_, std::string(source_.substr(content_start)), true,
             "dollar_quoted_string", "", "unterminated");
    return true;
  }

  bool TryPrefixedLiteral(std::size_t start,
                          std::size_t start_line,
                          std::size_t start_column,
                          std::string_view word,
                          bool allow_separating_space) {
    const auto upper = ToUpperAscii(word);
    const auto word_end = offset_;
    std::size_t cursor = offset_;
    std::size_t cursor_line = line_;
    std::size_t cursor_column = column_;
    if (allow_separating_space) {
      while (cursor < source_.size() && IsHorizontalWhitespace(source_[cursor])) {
        if (source_[cursor] == '\t') {
          ++cursor_column;
        } else {
          ++cursor_column;
        }
        ++cursor;
      }
    }
    if (cursor >= source_.size() || source_[cursor] != '\'') return false;

    TokenKind kind = TokenKind::kStringLiteral;
    std::string family;
    if (upper == "N" || upper == "NATIONAL") {
      family = "national_string";
    } else if (upper == "E") {
      family = "escaped_string";
    } else if (upper == "X" || upper == "B" || upper == "BINARY") {
      kind = TokenKind::kBinaryLiteral;
      family = upper == "B" ? "bit_binary" : "hex_binary";
    } else if (upper == "UUID") {
      kind = TokenKind::kUuidLiteral;
      family = "uuid";
    } else if (IsTemporalLiteralPrefix(upper)) {
      kind = TokenKind::kTemporalLiteral;
      family = ToUpperAscii(word);
    } else if (IsStructuredLiteralPrefix(upper)) {
      kind = StructuredLiteralKind(upper);
      family = ToUpperAscii(word);
    } else {
      return false;
    }

    offset_ = word_end;
    line_ = cursor_line;
    column_ = cursor_column - (cursor - word_end);
    while (offset_ < cursor) Advance();
    std::string decoded;
    const bool closed = ConsumeSingleQuoted(&decoded);
    if (!closed) {
      AddDiagnostic("SBSQL.LEXER.STRING_UNCLOSED", "prefixed literal is not closed",
                    start, start_line, start_column);
    } else if (kind == TokenKind::kUuidLiteral && !IsCanonicalUuidText(decoded)) {
      AddDiagnostic("SBSQL.LEXER.UUID_LITERAL_INVALID",
                    "UUID literal is not a canonical UUID value",
                    start, start_line, start_column);
    }
    AddToken(kind, start, start_line, start_column, offset_, line_, column_,
             std::move(decoded), true, std::move(family), "",
             closed ? "prefixed_literal" : "unterminated");
    return true;
  }

  void LexIdentifierKeywordOrPrefixedLiteral() {
    const auto start = offset_;
    const auto start_line = line_;
    const auto start_column = column_;
    Advance();
    while (offset_ < source_.size() && IsIdentifierContinue(Peek())) Advance();
    const std::string word(source_.substr(start, offset_ - start));
    const auto upper = ToUpperAscii(word);

    if (TryPrefixedLiteral(start, start_line, start_column, word, true)) return;

    if (upper == "TRUE" || upper == "FALSE") {
      AddToken(TokenKind::kBooleanLiteral, start, start_line, start_column, offset_,
               line_, column_, word, false, "boolean", "contextual_literal",
               "literal");
      return;
    }
    if (upper == "NULL") {
      AddToken(TokenKind::kNullLiteral, start, start_line, start_column, offset_, line_,
               column_, word, false, "null", "contextual_literal", "literal");
      return;
    }
    if (upper == "DEFAULT") {
      AddToken(TokenKind::kDefaultLiteral, start, start_line, start_column, offset_,
               line_, column_, word, false, "default", "contextual_native",
               "literal");
      return;
    }

    const auto keyword_class = KeywordClass(word);
    AddToken(keyword_class.empty() ? TokenKind::kIdentifier : TokenKind::kKeyword, start,
             start_line, start_column, offset_, line_, column_, word, false, "",
             keyword_class, keyword_class.empty() ? "identifier" : "keyword");
  }

  bool TryBareUuid() {
    if (offset_ + 36 > source_.size()) return false;
    for (std::size_t pos = 0; pos < 36; ++pos) {
      const char c = source_[offset_ + pos];
      const bool hyphen_pos = pos == 8 || pos == 13 || pos == 18 || pos == 23;
      if (hyphen_pos) {
        if (c != '-') return false;
      } else if (!IsAsciiHex(c)) {
        return false;
      }
    }
    if (offset_ + 36 < source_.size() && !IsKeywordBoundary(source_[offset_ + 36])) {
      return false;
    }
    return true;
  }

  void LexUuidOrNumber() {
    const auto start = offset_;
    const auto start_line = line_;
    const auto start_column = column_;

    if (TryBareUuid()) {
      Advance(36);
      AddToken(TokenKind::kUuidLiteral, start, start_line, start_column, offset_,
               line_, column_, std::string(source_.substr(start, offset_ - start)),
               false, "uuid", "", "literal");
      return;
    }

    bool seen_dot = false;
    bool seen_exponent = false;
    std::string family = "integer";

    if (Peek() == '0' && (Peek(1) == 'x' || Peek(1) == 'X')) {
      Advance(2);
      while (offset_ < source_.size() && (IsAsciiHex(Peek()) || Peek() == '_')) Advance();
      family = "hex_integer";
    } else if (Peek() == '0' && (Peek(1) == 'b' || Peek(1) == 'B')) {
      Advance(2);
      while (offset_ < source_.size() &&
             (Peek() == '0' || Peek() == '1' || Peek() == '_')) {
        Advance();
      }
      family = "binary_integer";
    } else if (Peek() == '0' && (Peek(1) == 'o' || Peek(1) == 'O')) {
      Advance(2);
      while (offset_ < source_.size() &&
             ((Peek() >= '0' && Peek() <= '7') || Peek() == '_')) {
        Advance();
      }
      family = "octal_integer";
    } else {
      while (offset_ < source_.size()) {
        if (IsAsciiDigit(Peek()) || Peek() == '_') {
          Advance();
          continue;
        }
        if (Peek() == '.' && !seen_dot && IsAsciiDigit(Peek(1))) {
          seen_dot = true;
          family = "decimal";
          Advance();
          continue;
        }
        if ((Peek() == 'e' || Peek() == 'E') && !seen_exponent) {
          seen_exponent = true;
          family = "float";
          Advance();
          if (Peek() == '+' || Peek() == '-') Advance();
          continue;
        }
        break;
      }
    }

    const auto suffix_start = offset_;
    while (offset_ < source_.size() &&
           (IsAsciiAlpha(Peek()) || IsAsciiDigit(Peek()) || Peek() == '_')) {
      Advance();
    }
    const auto suffix = ToUpperAscii(source_.substr(suffix_start, offset_ - suffix_start));
    if (suffix == "U" || suffix == "UINT") family = "uint";
    if (suffix == "I128" || suffix == "INT128") family = "int128";
    if (suffix == "U128" || suffix == "UINT128") family = "uint128";
    if (suffix == "R128" || suffix == "REAL128") family = "real128";
    if (suffix == "D" || suffix == "DEC" || suffix == "DECIMAL") family = "decimal";
    if (suffix == "F" || suffix == "FLOAT" || suffix == "DOUBLE") family = "float";

    AddToken(TokenKind::kNumericLiteral, start, start_line, start_column, offset_,
             line_, column_, std::string(source_.substr(start, offset_ - start)),
             false, family, "", "literal");
  }

  void LexDollarParameterOrSymbol() {
    const auto start = offset_;
    const auto start_line = line_;
    const auto start_column = column_;
    Advance();
    if (IsAsciiDigit(Peek())) {
      while (offset_ < source_.size() && IsAsciiDigit(Peek())) Advance();
      AddToken(TokenKind::kParameter, start, start_line, start_column, offset_, line_,
               column_, std::string(source_.substr(start, offset_ - start)), false,
               "ordinal_parameter", "", "parameter");
      return;
    }
    AddToken(TokenKind::kOperator, start, start_line, start_column, offset_, line_,
             column_, "$", false, "", "", "operator");
  }

  void LexParameterVariableOrOperator() {
    const auto start = offset_;
    const auto start_line = line_;
    const auto start_column = column_;
    const char c = Peek();
    if (c == '?' && (Peek(1) == '|' || Peek(1) == '&' || Peek(1) == '?')) {
      LexOperatorSymbolOrTerminator();
      return;
    }
    if (c == ':' && Peek(1) == ':') {
      LexOperatorSymbolOrTerminator();
      return;
    }

    Advance();
    if (c == '?') {
      AddToken(TokenKind::kParameter, start, start_line, start_column, offset_, line_,
               column_, "?", false, "anonymous_parameter", "", "parameter");
      return;
    }
    if (c == ':') {
      while (offset_ < source_.size() && IsParameterIdentifierContinue(Peek())) Advance();
      const auto kind = offset_ > start + 1 ? TokenKind::kParameter : TokenKind::kSymbol;
      AddToken(kind, start, start_line, start_column, offset_, line_, column_,
               std::string(source_.substr(start, offset_ - start)), false,
               kind == TokenKind::kParameter ? "named_parameter" : "", "",
               kind == TokenKind::kParameter ? "parameter" : "symbol");
      return;
    }

    if (Peek() == '@') Advance();
    while (offset_ < source_.size() && IsParameterIdentifierContinue(Peek())) Advance();
    AddToken(TokenKind::kVariable, start, start_line, start_column, offset_, line_,
             column_, std::string(source_.substr(start, offset_ - start)), false,
             "session_variable", "", "variable");
  }

  void LexMetaCommand() {
    const auto start = offset_;
    const auto start_line = line_;
    const auto start_column = column_;
    Advance();
    while (offset_ < source_.size() && !std::isspace(static_cast<unsigned char>(Peek()))) {
      Advance();
    }
    AddToken(TokenKind::kMetaCommand, start, start_line, start_column, offset_, line_,
             column_, std::string(source_.substr(start, offset_ - start)), false,
             "meta_command", "meta_command", "meta_command");
  }

  void LexInvalidControl() {
    const auto start = offset_;
    const auto start_line = line_;
    const auto start_column = column_;
    Advance();
    AddDiagnostic("SBSQL.LEXER.INVALID_CONTROL", "invalid control character in input",
                  start, start_line, start_column);
    AddToken(TokenKind::kSymbol, start, start_line, start_column, offset_, line_,
             column_, std::string(source_.substr(start, offset_ - start)), false,
             "", "", "invalid");
  }

  void LexOperatorSymbolOrTerminator() {
    const auto start = offset_;
    const auto start_line = line_;
    const auto start_column = column_;
    if (Peek() == ';') {
      Advance();
      AddToken(TokenKind::kStatementTerminator, start, start_line, start_column,
               offset_, line_, column_, ";", false, "", "", "terminator");
      return;
    }

    static constexpr std::array<std::string_view, 44> operators = {
        "<<=", ">>=", "#>>", "->>", "<->", "<=>", "<#>", "<+>", "<~>", "<%>",
        "::",  ":=",  "=>",  "<=",  ">=",  "<>",  "!=",  "||",  "&&",  "->",
        "@>",  "<@",  "?|",  "?&",  "??",  "#>",  "<<",  ">>",  "+=",  "-=",
        "*=",  "/=",  "%=",  "**",  "!!",  "..",  "+",   "-",   "*",   "/",
        "%",   "=",   ">",   "<"};
    for (const auto op : operators) {
      if (source_.substr(offset_, op.size()) == op) {
        Advance(op.size());
        AddToken(TokenKind::kOperator, start, start_line, start_column, offset_, line_,
                 column_, std::string(op), false, "", "", "operator");
        return;
      }
    }

    const char c = Peek();
    Advance();
    AddToken(TokenKind::kSymbol, start, start_line, start_column, offset_, line_,
             column_, std::string(1, c), false, "", "", "symbol");
  }

  std::string_view source_;
  std::size_t offset_{0};
  std::size_t line_{1};
  std::size_t column_{1};
  LexResult result_;
};

} // namespace

std::string TokenKindName(TokenKind kind) {
  switch (kind) {
    case TokenKind::kIdentifier: return "identifier";
    case TokenKind::kKeyword: return "keyword";
    case TokenKind::kNumericLiteral: return "numeric_literal";
    case TokenKind::kStringLiteral: return "string_literal";
    case TokenKind::kBinaryLiteral: return "binary_literal";
    case TokenKind::kTemporalLiteral: return "temporal_literal";
    case TokenKind::kUuidLiteral: return "uuid_literal";
    case TokenKind::kBooleanLiteral: return "boolean_literal";
    case TokenKind::kNullLiteral: return "null_literal";
    case TokenKind::kDefaultLiteral: return "default_literal";
    case TokenKind::kDocumentLiteral: return "document_literal";
    case TokenKind::kVectorLiteral: return "vector_literal";
    case TokenKind::kRegexLiteral: return "regex_literal";
    case TokenKind::kRangeLiteral: return "range_literal";
    case TokenKind::kParameter: return "parameter";
    case TokenKind::kVariable: return "variable";
    case TokenKind::kOperator: return "operator";
    case TokenKind::kSymbol: return "symbol";
    case TokenKind::kStatementTerminator: return "statement_terminator";
    case TokenKind::kComment: return "comment";
    case TokenKind::kWhitespace: return "whitespace";
    case TokenKind::kMetaCommand: return "meta_command";
    case TokenKind::kParserDirective: return "parser_directive";
    case TokenKind::kEnd: return "end";
  }
  return "end";
}

bool IsTriviaToken(const Token& token) {
  return token.kind == TokenKind::kWhitespace || token.kind == TokenKind::kComment ||
         token.kind == TokenKind::kParserDirective;
}

LexResult Lex(std::string_view source) { return Lexer(source).Run(); }

} // namespace scratchbird::parser::sbsql

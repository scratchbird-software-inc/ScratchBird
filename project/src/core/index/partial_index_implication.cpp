// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "partial_index_implication.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <sstream>
#include <string_view>
#include <utility>

namespace scratchbird::core::index {
namespace {

enum class TokenKind {
  kEnd,
  kIdentifier,
  kNumber,
  kString,
  kLParen,
  kRParen,
  kComma,
  kOperator,
};

struct Token {
  TokenKind kind = TokenKind::kEnd;
  std::string text;
};

std::string LowerAscii(std::string value) {
  for (char& ch : value) {
    ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  }
  return value;
}

bool StartsWith(std::string_view value, std::string_view prefix) {
  return value.size() >= prefix.size() && value.substr(0, prefix.size()) == prefix;
}

bool EndsWith(std::string_view value, std::string_view suffix) {
  return value.size() >= suffix.size() &&
         value.substr(value.size() - suffix.size()) == suffix;
}

bool IsKeyword(const Token& token, std::string_view keyword) {
  return token.kind == TokenKind::kIdentifier && LowerAscii(token.text) == keyword;
}

bool IsComparisonOperator(const std::string& op) {
  return op == "=" || op == "!=" || op == "<>" || op == "<" || op == "<=" ||
         op == ">" || op == ">=";
}

std::string NormalizedOperator(std::string op) {
  return op == "!=" ? "<>" : std::move(op);
}

std::string CommuteOperator(const std::string& op) {
  if (op == "<") return ">";
  if (op == "<=") return ">=";
  if (op == ">") return "<";
  if (op == ">=") return "<=";
  return op;
}

std::uint64_t Fnv1a64(std::string_view input) {
  std::uint64_t hash = 1469598103934665603ull;
  for (const unsigned char ch : input) {
    hash ^= ch;
    hash *= 1099511628211ull;
  }
  return hash;
}

std::string Hex64(std::uint64_t value) {
  constexpr char digits[] = "0123456789abcdef";
  std::string out(16, '0');
  for (int i = 15; i >= 0; --i) {
    out[static_cast<std::size_t>(i)] = digits[value & 0x0f];
    value >>= 4;
  }
  return out;
}

std::string HexEncode(std::string_view value) {
  constexpr char digits[] = "0123456789abcdef";
  std::string out;
  out.reserve(value.size() * 2);
  for (const unsigned char ch : value) {
    out.push_back(digits[(ch >> 4) & 0x0f]);
    out.push_back(digits[ch & 0x0f]);
  }
  return out;
}

int HexValue(char ch) {
  if (ch >= '0' && ch <= '9') return ch - '0';
  if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
  if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
  return -1;
}

bool HexDecode(std::string_view value, std::string* out) {
  if (out == nullptr || value.size() % 2 != 0) return false;
  out->clear();
  out->reserve(value.size() / 2);
  for (std::size_t i = 0; i < value.size(); i += 2) {
    const int high = HexValue(value[i]);
    const int low = HexValue(value[i + 1]);
    if (high < 0 || low < 0) return false;
    out->push_back(static_cast<char>((high << 4) | low));
  }
  return true;
}

std::vector<std::string> SortedUnique(std::vector<std::string> values) {
  std::sort(values.begin(), values.end());
  values.erase(std::unique(values.begin(), values.end()), values.end());
  return values;
}

void AddUnique(std::vector<std::string>* values, std::string value) {
  if (values == nullptr || value.empty()) return;
  if (std::find(values->begin(), values->end(), value) == values->end()) {
    values->push_back(std::move(value));
  }
}

std::string Join(const std::vector<std::string>& values, std::string_view sep) {
  std::ostringstream out;
  for (std::size_t i = 0; i < values.size(); ++i) {
    if (i != 0) out << sep;
    out << values[i];
  }
  return out.str();
}

std::vector<std::string> SplitTopLevelArgs(const std::string& inner) {
  std::vector<std::string> values;
  std::size_t start = 0;
  int depth = 0;
  for (std::size_t i = 0; i < inner.size(); ++i) {
    if (inner[i] == '(') {
      ++depth;
    } else if (inner[i] == ')') {
      --depth;
    } else if (inner[i] == ',' && depth == 0) {
      values.push_back(inner.substr(start, i - start));
      start = i + 1;
    }
  }
  values.push_back(inner.substr(start));
  return values;
}

bool IsConstExpr(const std::string& value) {
  return StartsWith(value, "const(") || value == "true" || value == "false" ||
         value == "null";
}

bool IsUnsafeFunctionName(std::string_view name) {
  const std::string fn = LowerAscii(std::string(name));
  static const char* const kUnsafe[] = {
      "auth_context",
      "clock_timestamp",
      "current_date",
      "current_role",
      "current_schema",
      "current_time",
      "current_timestamp",
      "current_user",
      "gen_random_uuid",
      "has_column_privilege",
      "has_function_privilege",
      "has_schema_privilege",
      "has_table_privilege",
      "is_visible",
      "localtime",
      "localtimestamp",
      "mga_visible",
      "now",
      "rand",
      "random",
      "row_visible",
      "security_label",
      "session_user",
      "statement_timestamp",
      "transaction_id",
      "transaction_timestamp",
      "txid_current",
      "user",
      "uuid_generate_v4",
      "uuid_v7",
      "visibility_context",
      "visible_to",
  };
  for (const char* unsafe : kUnsafe) {
    if (fn == unsafe) return true;
  }
  return StartsWith(fn, "auth_") || StartsWith(fn, "security_") ||
         StartsWith(fn, "visibility_") || StartsWith(fn, "mga_");
}

bool IsKnownImmutableFunctionName(std::string_view name) {
  const std::string fn = LowerAscii(std::string(name));
  static const char* const kKnownImmutable[] = {
      "abs",
      "char_length",
      "coalesce",
      "fn.abs_i64",
      "fn.coalesce_text",
      "fn.length_text",
      "fn.lower_ascii",
      "fn.upper_ascii",
      "length",
      "lower",
      "octet_length",
      "upper",
  };
  for (const char* known : kKnownImmutable) {
    if (fn == known) return true;
  }
  return false;
}

struct Parsed {
  bool ok = true;
  std::string text;
  bool boolean_equivalent = false;
  bool like_prefix_predicate = false;
  std::string like_prefix;
  std::string like_refusal_reason;
  std::vector<std::string> searchable_expression_digests;
  std::vector<std::string> function_names;
  std::vector<std::string> unsafe_function_names;
  std::vector<std::string> diagnostics;
};

void MergeParsedMetadata(const Parsed& source, Parsed* target) {
  if (target == nullptr) return;
  target->boolean_equivalent = target->boolean_equivalent ||
                               source.boolean_equivalent;
  target->like_prefix_predicate = target->like_prefix_predicate ||
                                  source.like_prefix_predicate;
  if (target->like_prefix.empty()) target->like_prefix = source.like_prefix;
  if (target->like_refusal_reason.empty()) {
    target->like_refusal_reason = source.like_refusal_reason;
  }
  target->searchable_expression_digests.insert(
      target->searchable_expression_digests.end(),
      source.searchable_expression_digests.begin(),
      source.searchable_expression_digests.end());
  target->function_names.insert(target->function_names.end(),
                                source.function_names.begin(),
                                source.function_names.end());
  target->unsafe_function_names.insert(target->unsafe_function_names.end(),
                                       source.unsafe_function_names.begin(),
                                       source.unsafe_function_names.end());
  target->diagnostics.insert(target->diagnostics.end(),
                             source.diagnostics.begin(),
                             source.diagnostics.end());
}

class Lexer {
 public:
  explicit Lexer(std::string_view input) : input_(input) {}

  Token Next() {
    while (pos_ < input_.size() &&
           std::isspace(static_cast<unsigned char>(input_[pos_]))) {
      ++pos_;
    }
    if (pos_ >= input_.size()) return {};
    const char ch = input_[pos_];
    if (std::isalpha(static_cast<unsigned char>(ch)) || ch == '_' ||
        ch == '.') {
      std::string text;
      while (pos_ < input_.size()) {
        const char c = input_[pos_];
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_' &&
            c != '.') {
          break;
        }
        text.push_back(
            static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
        ++pos_;
      }
      return {TokenKind::kIdentifier, std::move(text)};
    }
    if (std::isdigit(static_cast<unsigned char>(ch)) ||
        (ch == '-' && pos_ + 1 < input_.size() &&
         std::isdigit(static_cast<unsigned char>(input_[pos_ + 1])))) {
      std::string text;
      text.push_back(input_[pos_++]);
      while (pos_ < input_.size() &&
             (std::isdigit(static_cast<unsigned char>(input_[pos_])) ||
              input_[pos_] == '.')) {
        text.push_back(input_[pos_++]);
      }
      return {TokenKind::kNumber, std::move(text)};
    }
    if (ch == '\'') {
      ++pos_;
      std::string text;
      bool closed = false;
      while (pos_ < input_.size()) {
        const char c = input_[pos_++];
        if (c == '\'') {
          if (pos_ < input_.size() && input_[pos_] == '\'') {
            text.push_back('\'');
            ++pos_;
            continue;
          }
          closed = true;
          break;
        }
        text.push_back(c);
      }
      if (!closed) {
        diagnostics_.push_back("predicate_string_literal_unterminated");
        return {TokenKind::kOperator, "unterminated_string"};
      }
      return {TokenKind::kString, std::move(text)};
    }
    ++pos_;
    if (ch == '(') return {TokenKind::kLParen, "("};
    if (ch == ')') return {TokenKind::kRParen, ")"};
    if (ch == ',') return {TokenKind::kComma, ","};
    if ((ch == '<' || ch == '>' || ch == '!' || ch == '=') &&
        pos_ < input_.size() && input_[pos_] == '=') {
      return {TokenKind::kOperator, std::string{ch} + input_[pos_++]};
    }
    if (ch == '<' && pos_ < input_.size() && input_[pos_] == '>') {
      ++pos_;
      return {TokenKind::kOperator, "<>"};
    }
    if (ch == '<' || ch == '>' || ch == '=') {
      return {TokenKind::kOperator, std::string{ch}};
    }
    diagnostics_.push_back("predicate_unknown_token:" + std::string(1, ch));
    return {TokenKind::kOperator, std::string{ch}};
  }

  const std::vector<std::string>& diagnostics() const { return diagnostics_; }

 private:
  std::string_view input_;
  std::size_t pos_ = 0;
  std::vector<std::string> diagnostics_;
};

class Parser {
 public:
  explicit Parser(std::string_view input) {
    Lexer lexer(input);
    for (;;) {
      auto token = lexer.Next();
      tokens_.push_back(token);
      if (token.kind == TokenKind::kEnd) break;
    }
    diagnostics_ = lexer.diagnostics();
  }

  Parsed ParsePredicate() {
    auto parsed = ParseOr();
    if (Peek().kind != TokenKind::kEnd) {
      parsed.ok = false;
      parsed.diagnostics.push_back("predicate_trailing_tokens_refused");
    }
    parsed.diagnostics.insert(parsed.diagnostics.end(), diagnostics_.begin(),
                              diagnostics_.end());
    return parsed;
  }

  Parsed ParseExpressionOnly() {
    auto parsed = ParseValue();
    if (Peek().kind != TokenKind::kEnd) {
      parsed.ok = false;
      parsed.diagnostics.push_back("expression_trailing_tokens_refused");
    }
    if (parsed.ok) {
      parsed.searchable_expression_digests = {
          PartialIndexPredicateDigest(parsed.text)};
    }
    parsed.diagnostics.insert(parsed.diagnostics.end(), diagnostics_.begin(),
                              diagnostics_.end());
    return parsed;
  }

 private:
  const Token& Peek() const { return tokens_[pos_]; }
  const Token& Consume() { return tokens_[pos_++]; }

  bool MatchKeyword(std::string_view keyword) {
    if (!IsKeyword(Peek(), keyword)) return false;
    ++pos_;
    return true;
  }

  bool Match(TokenKind kind) {
    if (Peek().kind != kind) return false;
    ++pos_;
    return true;
  }

  Parsed ParseOr() {
    auto left = ParseAnd();
    std::vector<std::string> terms{left.text};
    Parsed out = left;
    while (MatchKeyword("or")) {
      auto right = ParseAnd();
      out.ok = out.ok && right.ok;
      MergeParsedMetadata(right, &out);
      if (StartsWith(right.text, "or(") && EndsWith(right.text, ")")) {
        auto nested = SplitTopLevelArgs(right.text.substr(3, right.text.size() - 4));
        terms.insert(terms.end(), nested.begin(), nested.end());
      } else {
        terms.push_back(right.text);
      }
    }
    if (terms.size() == 1) return left;
    terms = SortedUnique(std::move(terms));
    out.text = "or(" + Join(terms, ",") + ")";
    out.searchable_expression_digests =
        SortedUnique(std::move(out.searchable_expression_digests));
    out.function_names = SortedUnique(std::move(out.function_names));
    out.unsafe_function_names = SortedUnique(std::move(out.unsafe_function_names));
    return out;
  }

  Parsed ParseAnd() {
    auto left = ParseNot();
    std::vector<std::string> terms{left.text};
    Parsed out = left;
    while (MatchKeyword("and")) {
      auto right = ParseNot();
      out.ok = out.ok && right.ok;
      MergeParsedMetadata(right, &out);
      if (StartsWith(right.text, "and(") && EndsWith(right.text, ")")) {
        auto nested = SplitTopLevelArgs(right.text.substr(4, right.text.size() - 5));
        terms.insert(terms.end(), nested.begin(), nested.end());
      } else {
        terms.push_back(right.text);
      }
    }
    if (terms.size() == 1) return left;
    terms = SortedUnique(std::move(terms));
    out.text = "and(" + Join(terms, ",") + ")";
    out.searchable_expression_digests =
        SortedUnique(std::move(out.searchable_expression_digests));
    out.function_names = SortedUnique(std::move(out.function_names));
    out.unsafe_function_names = SortedUnique(std::move(out.unsafe_function_names));
    return out;
  }

  Parsed ParseNot() {
    if (!MatchKeyword("not")) return ParseAtomPredicate();
    auto child = ParseNot();
    if (StartsWith(child.text, "not(") && EndsWith(child.text, ")")) {
      child.text = child.text.substr(4, child.text.size() - 5);
      child.boolean_equivalent = true;
      return child;
    }
    if (StartsWith(child.text, "bool(") && EndsWith(child.text, ")")) {
      const auto args = SplitTopLevelArgs(child.text.substr(5, child.text.size() - 6));
      if (args.size() == 2 && (args[1] == "true" || args[1] == "false")) {
        child.text = "bool(" + args[0] + "," +
                     (args[1] == "true" ? "false" : "true") + ")";
        child.boolean_equivalent = true;
        return child;
      }
    }
    child.text = "not(" + child.text + ")";
    return child;
  }

  Parsed ParseAtomPredicate() {
    if (Match(TokenKind::kLParen)) {
      auto inner = ParseOr();
      inner.ok = inner.ok && Match(TokenKind::kRParen);
      if (!inner.ok) inner.diagnostics.push_back("predicate_parenthesis_refused");
      return inner;
    }

    auto left = ParseValue();
    if (!left.ok) return left;

    if (IsKeyword(Peek(), "is")) {
      Consume();
      bool negated = MatchKeyword("not");
      if (!MatchKeyword("null")) {
        left.ok = false;
        left.diagnostics.push_back("predicate_is_null_expected_refused");
        return left;
      }
      Parsed out;
      out.text = "is_null(" + left.text + "," + (negated ? "false" : "true") + ")";
      out.searchable_expression_digests = {PartialIndexPredicateDigest(left.text)};
      MergeParsedMetadata(left, &out);
      return out;
    }

    if (IsKeyword(Peek(), "like")) {
      Consume();
      auto pattern = ParseValue();
      std::string escape;
      Parsed out;
      out.ok = left.ok && pattern.ok;
      MergeParsedMetadata(left, &out);
      MergeParsedMetadata(pattern, &out);
      if (MatchKeyword("escape")) {
        auto escape_value = ParseValue();
        out.ok = out.ok && escape_value.ok;
        MergeParsedMetadata(escape_value, &out);
        escape = escape_value.text;
      }
      out.text = "like(" + left.text + "," + pattern.text;
      if (!escape.empty()) out.text += ",escape=" + escape;
      out.text += ")";
      out.searchable_expression_digests.push_back(
          PartialIndexPredicateDigest(left.text));
      out.searchable_expression_digests =
          SortedUnique(std::move(out.searchable_expression_digests));
      out.like_prefix_predicate = true;
      ClassifyLikePattern(pattern.text, escape, &out);
      return out;
    }

    if (MatchKeyword("in")) {
      Parsed out;
      MergeParsedMetadata(left, &out);
      if (!Match(TokenKind::kLParen)) {
        out.ok = false;
        out.text = left.text;
        out.diagnostics.push_back("predicate_in_list_open_refused");
        return out;
      }
      std::vector<std::string> values;
      if (!Match(TokenKind::kRParen)) {
        for (;;) {
          auto item = ParseValue();
          out.ok = out.ok && item.ok;
          MergeParsedMetadata(item, &out);
          values.push_back(item.text);
          if (Match(TokenKind::kRParen)) break;
          if (!Match(TokenKind::kComma)) {
            out.ok = false;
            out.diagnostics.push_back("predicate_in_list_separator_refused");
            break;
          }
        }
      }
      values = SortedUnique(std::move(values));
      out.text = "in(" + left.text;
      for (const auto& value : values) out.text += "," + value;
      out.text += ")";
      out.searchable_expression_digests.push_back(
          PartialIndexPredicateDigest(left.text));
      out.searchable_expression_digests =
          SortedUnique(std::move(out.searchable_expression_digests));
      return out;
    }

    if (Peek().kind == TokenKind::kOperator &&
        IsComparisonOperator(Peek().text)) {
      std::string op = NormalizedOperator(Consume().text);
      auto right = ParseValue();
      if (!right.ok) {
        right.diagnostics.push_back("predicate_comparison_value_refused");
        return right;
      }
      return BuildComparison(std::move(left), std::move(op), std::move(right));
    }

    if (left.text == "true" || left.text == "false") return left;
    const std::string expression_text = left.text;
    left.text = "bool(" + expression_text + ",true)";
    left.boolean_equivalent = true;
    left.searchable_expression_digests.push_back(
        PartialIndexPredicateDigest(expression_text));
    left.searchable_expression_digests =
        SortedUnique(std::move(left.searchable_expression_digests));
    return left;
  }

  Parsed ParseValue() {
    if (Peek().kind == TokenKind::kNumber) {
      return {true, "const(number:" + Consume().text + ")"};
    }
    if (Peek().kind == TokenKind::kString) {
      return {true, "const(string_hex:" + HexEncode(Consume().text) + ")"};
    }
    if (MatchKeyword("true")) return {true, "true"};
    if (MatchKeyword("false")) return {true, "false"};
    if (MatchKeyword("null")) return {true, "null"};
    if (Peek().kind != TokenKind::kIdentifier) {
      return {false, "parse_error", false, false, {}, {}, {}, {}, {},
              {"predicate_value_expected_refused"}};
    }

    std::string name = Consume().text;
    if (Match(TokenKind::kLParen)) {
      Parsed out;
      std::vector<std::string> args;
      if (!Match(TokenKind::kRParen)) {
        for (;;) {
          auto arg = ParseValue();
          out.ok = out.ok && arg.ok;
          MergeParsedMetadata(arg, &out);
          args.push_back(arg.text);
          if (Match(TokenKind::kRParen)) break;
          if (!Match(TokenKind::kComma)) {
            out.ok = false;
            out.diagnostics.push_back("function_argument_separator_refused");
            break;
          }
        }
      }
      out.text = "fn(" + name + ":" + Join(args, ",") + ")";
      out.function_names.push_back(name);
      if (IsUnsafeFunctionName(name)) out.unsafe_function_names.push_back(name);
      out.function_names = SortedUnique(std::move(out.function_names));
      out.unsafe_function_names = SortedUnique(std::move(out.unsafe_function_names));
      return out;
    }
    if (IsUnsafeFunctionName(name)) {
      Parsed out;
      out.text = "fn(" + name + ":)";
      out.function_names = {name};
      out.unsafe_function_names = {name};
      return out;
    }
    return {true, "col(" + name + ")"};
  }

  Parsed BuildComparison(Parsed left, std::string op, Parsed right) {
    Parsed out;
    out.ok = left.ok && right.ok;
    MergeParsedMetadata(left, &out);
    MergeParsedMetadata(right, &out);
    if (IsConstExpr(left.text) && !IsConstExpr(right.text)) {
      std::swap(left, right);
      op = CommuteOperator(op);
    } else if (!IsConstExpr(left.text) && !IsConstExpr(right.text) &&
               right.text < left.text) {
      std::swap(left, right);
      op = CommuteOperator(op);
    }
    if ((right.text == "true" || right.text == "false") &&
        (op == "=" || op == "<>")) {
      const bool truth = (right.text == "true") == (op == "=");
      out.text = "bool(" + left.text + "," + (truth ? "true" : "false") + ")";
      out.boolean_equivalent = true;
      out.searchable_expression_digests.push_back(
          PartialIndexPredicateDigest(left.text));
      out.searchable_expression_digests =
          SortedUnique(std::move(out.searchable_expression_digests));
      return out;
    }
    if ((left.text == "true" || left.text == "false") &&
        (op == "=" || op == "<>")) {
      const bool truth = (left.text == "true") == (op == "=");
      out.text = "bool(" + right.text + "," + (truth ? "true" : "false") + ")";
      out.boolean_equivalent = true;
      out.searchable_expression_digests.push_back(
          PartialIndexPredicateDigest(right.text));
      out.searchable_expression_digests =
          SortedUnique(std::move(out.searchable_expression_digests));
      return out;
    }
    out.text = "cmp(" + NormalizedOperator(op) + "," + left.text + "," +
               right.text + ")";
    out.searchable_expression_digests.push_back(
        PartialIndexPredicateDigest(left.text));
    out.searchable_expression_digests =
        SortedUnique(std::move(out.searchable_expression_digests));
    return out;
  }

 public:
  static void ClassifyLikePattern(const std::string& pattern_text,
                                  const std::string& escape_text,
                                  Parsed* out) {
    if (out == nullptr) return;
    if (!StartsWith(pattern_text, "const(string_hex:") ||
        !EndsWith(pattern_text, ")")) {
      out->like_refusal_reason = "like_prefix_nonconstant_pattern_refused";
      return;
    }
    std::string escape;
    if (!escape_text.empty()) {
      if (!StartsWith(escape_text, "const(string_hex:") ||
          !EndsWith(escape_text, ")") ||
          !HexDecode(escape_text.substr(17, escape_text.size() - 18), &escape) ||
          escape != "\\") {
        out->like_refusal_reason = "like_prefix_unsupported_escape_refused";
        return;
      }
    }
    std::string pattern;
    if (!HexDecode(pattern_text.substr(17, pattern_text.size() - 18), &pattern)) {
      out->like_refusal_reason = "like_prefix_nonconstant_pattern_refused";
      return;
    }
    if (pattern.empty() || pattern.front() == '%' || pattern.front() == '_') {
      out->like_refusal_reason = "like_prefix_leading_wildcard_refused";
      return;
    }
    std::string prefix;
    bool escaping = false;
    for (const char ch : pattern) {
      if (escaping) {
        prefix.push_back(ch);
        escaping = false;
        continue;
      }
      if (!escape.empty() && ch == '\\') {
        escaping = true;
        continue;
      }
      if (ch == '%') {
        out->like_prefix = prefix;
        return;
      }
      if (ch == '_') {
        out->like_refusal_reason = "like_prefix_unsupported_escape_refused";
        return;
      }
      prefix.push_back(ch);
    }
    out->like_prefix = prefix;
  }

 private:
  std::vector<Token> tokens_;
  std::size_t pos_ = 0;
  std::vector<std::string> diagnostics_;
};

std::vector<std::string> ConjunctsForCanonicalText(const std::string& text) {
  if (!StartsWith(text, "and(") || !EndsWith(text, ")")) return {text};
  std::vector<std::string> values;
  const std::string inner = text.substr(4, text.size() - 5);
  std::size_t start = 0;
  int depth = 0;
  for (std::size_t i = 0; i < inner.size(); ++i) {
    if (inner[i] == '(') {
      ++depth;
    } else if (inner[i] == ')') {
      --depth;
    } else if (inner[i] == ',' && depth == 0) {
      const auto child =
          ConjunctsForCanonicalText(inner.substr(start, i - start));
      values.insert(values.end(), child.begin(), child.end());
      start = i + 1;
    }
  }
  const auto child = ConjunctsForCanonicalText(inner.substr(start));
  values.insert(values.end(), child.begin(), child.end());
  return SortedUnique(std::move(values));
}

std::string NormalizeCanonicalLogicalText(const std::string& text) {
  const auto normalize_function = [&](std::string_view name) -> std::string {
    const std::string prefix = std::string(name) + "(";
    if (!StartsWith(text, prefix) || !EndsWith(text, ")")) return {};
    std::vector<std::string> terms;
    for (auto term :
         SplitTopLevelArgs(text.substr(prefix.size(),
                                       text.size() - prefix.size() - 1))) {
      term = NormalizeCanonicalLogicalText(term);
      if (StartsWith(term, prefix) && EndsWith(term, ")")) {
        auto nested = SplitTopLevelArgs(
            term.substr(prefix.size(), term.size() - prefix.size() - 1));
        terms.insert(terms.end(), nested.begin(), nested.end());
      } else {
        terms.push_back(std::move(term));
      }
    }
    terms = SortedUnique(std::move(terms));
    return prefix + Join(terms, ",") + ")";
  };
  if (auto normalized = normalize_function("and"); !normalized.empty()) {
    return normalized;
  }
  if (auto normalized = normalize_function("or"); !normalized.empty()) {
    return normalized;
  }
  return text;
}

enum class TermKind {
  kUnknown,
  kTrue,
  kFalse,
  kBool,
  kCmp,
  kIsNull,
  kLike,
  kIn,
  kOr,
  kNot,
};

struct TermInfo {
  TermKind kind = TermKind::kUnknown;
  std::string original;
  std::string expression;
  std::string op;
  std::string value;
  std::vector<std::string> values;
  bool bool_value = false;
  bool is_null = false;
  bool like_prefix_safe = false;
  std::string like_prefix;
  std::string like_refusal_reason;
};

TermInfo ParseTermInfo(const std::string& term) {
  TermInfo info;
  info.original = term;
  if (term == "true") {
    info.kind = TermKind::kTrue;
    return info;
  }
  if (term == "false") {
    info.kind = TermKind::kFalse;
    return info;
  }
  if (StartsWith(term, "bool(") && EndsWith(term, ")")) {
    auto args = SplitTopLevelArgs(term.substr(5, term.size() - 6));
    if (args.size() == 2 && (args[1] == "true" || args[1] == "false")) {
      info.kind = TermKind::kBool;
      info.expression = args[0];
      info.bool_value = args[1] == "true";
      return info;
    }
  }
  if (StartsWith(term, "cmp(") && EndsWith(term, ")")) {
    auto args = SplitTopLevelArgs(term.substr(4, term.size() - 5));
    if (args.size() == 3) {
      info.kind = TermKind::kCmp;
      info.op = args[0];
      info.expression = args[1];
      info.value = args[2];
      return info;
    }
  }
  if (StartsWith(term, "is_null(") && EndsWith(term, ")")) {
    auto args = SplitTopLevelArgs(term.substr(8, term.size() - 9));
    if (args.size() == 2 && (args[1] == "true" || args[1] == "false")) {
      info.kind = TermKind::kIsNull;
      info.expression = args[0];
      info.is_null = args[1] == "true";
      return info;
    }
  }
  if (StartsWith(term, "like(") && EndsWith(term, ")")) {
    auto args = SplitTopLevelArgs(term.substr(5, term.size() - 6));
    if (args.size() >= 2) {
      info.kind = TermKind::kLike;
      info.expression = args[0];
      Parsed tmp;
      Parser::ClassifyLikePattern(args[1],
                                  args.size() == 3 && StartsWith(args[2], "escape=")
                                      ? args[2].substr(7)
                                      : std::string{},
                                  &tmp);
      info.like_prefix = tmp.like_prefix;
      info.like_refusal_reason = tmp.like_refusal_reason;
      info.like_prefix_safe = !tmp.like_prefix.empty() &&
                              tmp.like_refusal_reason.empty();
      return info;
    }
  }
  if (StartsWith(term, "in(") && EndsWith(term, ")")) {
    auto args = SplitTopLevelArgs(term.substr(3, term.size() - 4));
    if (args.size() >= 2) {
      info.kind = TermKind::kIn;
      info.expression = args.front();
      info.values.assign(args.begin() + 1, args.end());
      info.values = SortedUnique(std::move(info.values));
      return info;
    }
  }
  if (StartsWith(term, "or(") && EndsWith(term, ")")) {
    info.kind = TermKind::kOr;
    return info;
  }
  if (StartsWith(term, "not(") && EndsWith(term, ")")) {
    info.kind = TermKind::kNot;
    return info;
  }
  return info;
}

struct ConstantValue {
  enum class Type { kUnknown, kNumber, kString, kNull, kBool } type = Type::kUnknown;
  long double number = 0.0;
  std::string text;
  bool boolean = false;
};

ConstantValue DecodeConstant(const std::string& value) {
  ConstantValue out;
  if (value == "null") {
    out.type = ConstantValue::Type::kNull;
    return out;
  }
  if (value == "true" || value == "false") {
    out.type = ConstantValue::Type::kBool;
    out.boolean = value == "true";
    return out;
  }
  if (StartsWith(value, "const(number:") && EndsWith(value, ")")) {
    const std::string number = value.substr(13, value.size() - 14);
    char* end = nullptr;
    out.number = std::strtold(number.c_str(), &end);
    if (end != nullptr && *end == '\0' && std::isfinite(out.number)) {
      out.type = ConstantValue::Type::kNumber;
    }
    return out;
  }
  if (StartsWith(value, "const(string_hex:") && EndsWith(value, ")")) {
    out.type = ConstantValue::Type::kString;
    if (!HexDecode(value.substr(17, value.size() - 18), &out.text)) {
      out.type = ConstantValue::Type::kUnknown;
    }
    return out;
  }
  return out;
}

int CompareConstants(const std::string& left, const std::string& right) {
  const auto l = DecodeConstant(left);
  const auto r = DecodeConstant(right);
  if (l.type == ConstantValue::Type::kNumber &&
      r.type == ConstantValue::Type::kNumber) {
    if (l.number < r.number) return -1;
    if (l.number > r.number) return 1;
    return 0;
  }
  if (l.type == ConstantValue::Type::kString &&
      r.type == ConstantValue::Type::kString) {
    if (l.text < r.text) return -1;
    if (l.text > r.text) return 1;
    return 0;
  }
  if (l.type == ConstantValue::Type::kBool &&
      r.type == ConstantValue::Type::kBool) {
    return l.boolean == r.boolean ? 0 : (l.boolean ? 1 : -1);
  }
  return 99;
}

bool SameComparableType(const std::string& left, const std::string& right) {
  const auto l = DecodeConstant(left);
  const auto r = DecodeConstant(right);
  return l.type != ConstantValue::Type::kUnknown && l.type == r.type &&
         l.type != ConstantValue::Type::kNull;
}

bool IsNonNullConstant(const std::string& value) {
  const auto decoded = DecodeConstant(value);
  return decoded.type != ConstantValue::Type::kUnknown &&
         decoded.type != ConstantValue::Type::kNull;
}

bool ValueSatisfiesComparison(const std::string& value,
                              const std::string& op,
                              const std::string& required) {
  if (!SameComparableType(value, required)) return false;
  const int cmp = CompareConstants(value, required);
  if (cmp == 99) return false;
  if (op == "=") return cmp == 0;
  if (op == "<>") return cmp != 0;
  if (op == "<") return cmp < 0;
  if (op == "<=") return cmp <= 0;
  if (op == ">") return cmp > 0;
  if (op == ">=") return cmp >= 0;
  return false;
}

bool LowerBoundImplies(const TermInfo& query, const TermInfo& required) {
  if ((query.op != ">" && query.op != ">=") ||
      (required.op != ">" && required.op != ">=") ||
      !SameComparableType(query.value, required.value)) {
    return false;
  }
  const int cmp = CompareConstants(query.value, required.value);
  if (cmp > 0) return true;
  if (cmp < 0) return false;
  const bool query_strict = query.op == ">";
  const bool required_strict = required.op == ">";
  return query_strict || !required_strict;
}

bool UpperBoundImplies(const TermInfo& query, const TermInfo& required) {
  if ((query.op != "<" && query.op != "<=") ||
      (required.op != "<" && required.op != "<=") ||
      !SameComparableType(query.value, required.value)) {
    return false;
  }
  const int cmp = CompareConstants(query.value, required.value);
  if (cmp < 0) return true;
  if (cmp > 0) return false;
  const bool query_strict = query.op == "<";
  const bool required_strict = required.op == "<";
  return query_strict || !required_strict;
}

bool EqualityImpliesComparison(const TermInfo& query, const TermInfo& required) {
  return query.op == "=" &&
         ValueSatisfiesComparison(query.value, required.op, required.value);
}

bool QueryTermImpliesRequired(const TermInfo& query, const TermInfo& required) {
  if (query.original == required.original) return true;
  if (required.kind == TermKind::kTrue) return true;
  if (required.kind == TermKind::kFalse) return query.kind == TermKind::kFalse;
  if (required.kind == TermKind::kBool) {
    return query.kind == TermKind::kBool &&
           query.expression == required.expression &&
           query.bool_value == required.bool_value;
  }
  if (required.kind == TermKind::kIsNull) {
    if (query.expression != required.expression) return false;
    if (required.is_null) {
      return query.kind == TermKind::kIsNull && query.is_null;
    }
    if (query.kind == TermKind::kIsNull) return !query.is_null;
    if (query.kind == TermKind::kBool) return true;
    if (query.kind == TermKind::kCmp) return IsNonNullConstant(query.value);
    if (query.kind == TermKind::kLike) return query.like_prefix_safe;
    if (query.kind == TermKind::kIn) {
      return !query.values.empty() &&
             std::all_of(query.values.begin(), query.values.end(),
                         IsNonNullConstant);
    }
    return false;
  }
  if (required.kind == TermKind::kCmp) {
    if (query.expression != required.expression) return false;
    if (query.kind == TermKind::kCmp) {
      if (required.op == "=") {
        return query.op == "=" && CompareConstants(query.value, required.value) == 0;
      }
      if (required.op == "<>") {
        if (query.op == "=") {
          return SameComparableType(query.value, required.value) &&
                 CompareConstants(query.value, required.value) != 0;
        }
        return false;
      }
      if (query.op == "=") return EqualityImpliesComparison(query, required);
      return LowerBoundImplies(query, required) ||
             UpperBoundImplies(query, required);
    }
    if (query.kind == TermKind::kIn) {
      return !query.values.empty() &&
             std::all_of(query.values.begin(), query.values.end(),
                         [&](const std::string& value) {
                           return ValueSatisfiesComparison(value, required.op,
                                                           required.value);
                         });
    }
    return false;
  }
  if (required.kind == TermKind::kIn) {
    if (query.expression != required.expression) return false;
    if (query.kind == TermKind::kCmp && query.op == "=") {
      return std::find(required.values.begin(), required.values.end(),
                       query.value) != required.values.end();
    }
    if (query.kind == TermKind::kIn) {
      return std::all_of(query.values.begin(), query.values.end(),
                         [&](const std::string& value) {
                           return std::find(required.values.begin(),
                                            required.values.end(),
                                            value) != required.values.end();
                         });
    }
    return false;
  }
  if (required.kind == TermKind::kLike) {
    if (!required.like_prefix_safe || query.expression != required.expression) {
      return false;
    }
    if (query.kind == TermKind::kLike) {
      return query.like_prefix_safe &&
             StartsWith(query.like_prefix, required.like_prefix);
    }
    if (query.kind == TermKind::kCmp && query.op == "=") {
      const auto value = DecodeConstant(query.value);
      return value.type == ConstantValue::Type::kString &&
             StartsWith(value.text, required.like_prefix);
    }
    if (query.kind == TermKind::kIn) {
      return !query.values.empty() &&
             std::all_of(query.values.begin(), query.values.end(),
                         [&](const std::string& value) {
                           const auto decoded = DecodeConstant(value);
                           return decoded.type == ConstantValue::Type::kString &&
                                  StartsWith(decoded.text,
                                             required.like_prefix);
                         });
    }
    return false;
  }
  return false;
}

bool RequiredTermUnsupported(const TermInfo& required) {
  if (required.kind == TermKind::kUnknown || required.kind == TermKind::kOr ||
      required.kind == TermKind::kNot) {
    return true;
  }
  if (required.kind == TermKind::kLike && !required.like_prefix_safe) {
    return true;
  }
  return false;
}

void AddNonAuthorityEvidence(PartialPredicateImplicationProof* proof) {
  proof->evidence.push_back("partial_index_implication_engine=core_index");
  proof->evidence.push_back("partial_index_visibility_authority=false");
  proof->evidence.push_back("partial_index_authorization_authority=false");
  proof->evidence.push_back("partial_index_transaction_finality_authority=false");
  proof->evidence.push_back("partial_index_cleanup_authority=false");
  proof->evidence.push_back("partial_index_recovery_authority=false");
  proof->evidence.push_back("partial_index_parser_or_reference_finality_authority=false");
  proof->evidence.push_back("base_row_mga_recheck_required=true");
  proof->evidence.push_back("base_row_security_recheck_required=true");
}

void AddRefusal(PartialPredicateImplicationProof* proof, std::string reason) {
  AddUnique(&proof->refusal_reasons, std::move(reason));
}

PartialPredicateCanonicalForm BuildCanonicalForm(const Parsed& parsed) {
  PartialPredicateCanonicalForm result;
  result.ok = parsed.ok && parsed.diagnostics.empty();
  result.canonical_text = NormalizeCanonicalLogicalText(parsed.text);
  result.digest = PartialIndexPredicateDigest(result.canonical_text);
  result.diagnostics = SortedUnique(parsed.diagnostics);
  result.conjuncts = ConjunctsForCanonicalText(result.canonical_text);
  result.searchable_expression_digests =
      SortedUnique(parsed.searchable_expression_digests);
  result.function_names = SortedUnique(parsed.function_names);
  result.unsafe_function_names = SortedUnique(parsed.unsafe_function_names);
  result.boolean_equivalent = parsed.boolean_equivalent;
  result.like_prefix_predicate = parsed.like_prefix_predicate;
  result.like_prefix = parsed.like_prefix;
  result.like_refusal_reason = parsed.like_refusal_reason;
  if (!result.ok) AddUnique(&result.diagnostics, "predicate_parse_refused");
  return result;
}

}  // namespace

std::string PartialIndexPredicateDigest(const std::string& canonical_text) {
  return "sbpred64:" + Hex64(Fnv1a64(canonical_text));
}

PartialPredicateCanonicalForm CanonicalizePartialIndexPredicateText(
    const std::string& predicate_text) {
  Parser parser(predicate_text);
  return BuildCanonicalForm(parser.ParsePredicate());
}

PartialPredicateCanonicalForm CanonicalizePartialIndexExpressionText(
    const std::string& expression_text) {
  Parser parser(expression_text);
  return BuildCanonicalForm(parser.ParseExpressionOnly());
}

PartialPredicateImplicationProof ProvePartialIndexPredicateImplication(
    const PartialPredicateImplicationRequest& request) {
  PartialPredicateImplicationProof proof;
  AddNonAuthorityEvidence(&proof);

  const auto query = CanonicalizePartialIndexPredicateText(
      request.query_predicate_text);
  const auto index = CanonicalizePartialIndexPredicateText(
      request.index_predicate_text);
  proof.canonical_query_text = query.canonical_text;
  proof.canonical_query_digest = query.digest;
  proof.canonical_index_predicate_text = index.canonical_text;
  proof.canonical_index_predicate_digest = index.digest;
  proof.index_predicate_conjuncts = index.conjuncts;

  if (!query.ok) {
    AddRefusal(&proof, "query_predicate_parse_refused");
    for (const auto& diagnostic : query.diagnostics) AddRefusal(&proof, diagnostic);
  }
  if (!index.ok) {
    AddRefusal(&proof, "index_predicate_parse_refused");
    for (const auto& diagnostic : index.diagnostics) AddRefusal(&proof, diagnostic);
  }
  if (!request.predicate_immutable) {
    AddRefusal(&proof, "partial_predicate_non_immutable_refused");
  }
  if (!request.predicate_security_safe) {
    AddRefusal(&proof, "partial_predicate_security_unsafe_refused");
  }
  if (!request.descriptor_epoch_valid) {
    AddRefusal(&proof, "partial_predicate_descriptor_epoch_stale");
  }
  if (!request.resource_epoch_valid) {
    AddRefusal(&proof, "partial_predicate_resource_epoch_stale");
  }
  if (!request.collation_epoch_valid) {
    AddRefusal(&proof, "partial_predicate_collation_epoch_stale");
  }
  if (!request.function_epoch_valid) {
    AddRefusal(&proof, "partial_predicate_function_epoch_stale");
  }
  if (!request.base_row_mga_recheck_planned) {
    AddRefusal(&proof, "partial_predicate_mga_recheck_missing");
  }
  if (!request.base_row_security_recheck_planned) {
    AddRefusal(&proof, "partial_predicate_security_recheck_missing");
  }
  for (const auto& function_name : query.unsafe_function_names) {
    AddRefusal(&proof, "partial_predicate_volatile_function_refused:" + function_name);
  }
  for (const auto& function_name : index.unsafe_function_names) {
    AddRefusal(&proof, "partial_predicate_volatile_function_refused:" + function_name);
  }
  for (const auto& function_name : query.function_names) {
    if (!IsUnsafeFunctionName(function_name) &&
        !IsKnownImmutableFunctionName(function_name)) {
      AddRefusal(&proof,
                 "partial_predicate_function_not_proven:" + function_name);
    }
  }
  for (const auto& function_name : index.function_names) {
    if (!IsUnsafeFunctionName(function_name) &&
        !IsKnownImmutableFunctionName(function_name)) {
      AddRefusal(&proof,
                 "partial_predicate_function_not_proven:" + function_name);
    }
  }

  if (!proof.refusal_reasons.empty()) {
    proof.evidence.push_back("partial_predicate_implication_proven=false");
    return proof;
  }

  std::vector<TermInfo> query_terms;
  query_terms.reserve(query.conjuncts.size());
  for (const auto& term : query.conjuncts) query_terms.push_back(ParseTermInfo(term));

  std::vector<std::string> used;
  for (const auto& required_text : index.conjuncts) {
    const auto required = ParseTermInfo(required_text);
    if (RequiredTermUnsupported(required)) {
      AddRefusal(&proof,
                 required.kind == TermKind::kLike
                     ? "partial_predicate_like_prefix_refused:" +
                           required.like_refusal_reason
                     : "partial_predicate_unsupported_required_term:" +
                           required_text);
      continue;
    }

    bool implied = false;
    for (const auto& query_term : query_terms) {
      if (QueryTermImpliesRequired(query_term, required)) {
        implied = true;
        AddUnique(&used, query_term.original);
        break;
      }
    }
    if (!implied) {
      AddRefusal(&proof, "partial_predicate_not_proven");
      AddRefusal(&proof, "partial_predicate_term_not_implied:" + required_text);
    }
  }

  if (!proof.refusal_reasons.empty()) {
    proof.evidence.push_back("partial_predicate_implication_proven=false");
    return proof;
  }

  proof.safe_to_consider_index = true;
  proof.predicate_implied = true;
  proof.implied_query_conjuncts = SortedUnique(std::move(used));
  for (const auto& query_term : query.conjuncts) {
    if (std::find(proof.implied_query_conjuncts.begin(),
                  proof.implied_query_conjuncts.end(),
                  query_term) == proof.implied_query_conjuncts.end()) {
      proof.remaining_query_conjuncts.push_back(query_term);
    }
  }
  proof.remaining_query_conjuncts =
      SortedUnique(std::move(proof.remaining_query_conjuncts));
  proof.acceptance_reasons.push_back("partial_predicate_proven");
  proof.acceptance_reasons.push_back("partial_predicate_core_implication_proven");
  proof.acceptance_reasons.push_back(
      "partial_predicate_mga_security_recheck_preserved");
  proof.evidence.push_back("partial_predicate_implication_proven=true");
  proof.evidence.push_back("partial_predicate_implied_conjuncts=" +
                           Join(proof.implied_query_conjuncts, "|"));
  proof.evidence.push_back("partial_predicate_remaining_query_conjuncts=" +
                           Join(proof.remaining_query_conjuncts, "|"));
  proof.evidence.push_back("partial_predicate_descriptor_epoch_valid=true");
  proof.evidence.push_back("partial_predicate_resource_epoch_valid=true");
  proof.evidence.push_back("partial_predicate_collation_epoch_valid=true");
  proof.evidence.push_back("partial_predicate_function_epoch_valid=true");
  return proof;
}

}  // namespace scratchbird::core::index

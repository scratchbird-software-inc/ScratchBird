// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "predicate_normalization.hpp"

#include "partial_index_implication.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <sstream>
#include <string_view>
#include <utility>

namespace scratchbird::engine::optimizer {
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
  for (char& ch : value) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  return value;
}

bool IsKeyword(const Token& token, std::string_view keyword) {
  return token.kind == TokenKind::kIdentifier && LowerAscii(token.text) == keyword;
}

bool IsComparisonOperator(const std::string& op) {
  return op == "=" || op == "!=" || op == "<>" || op == "<" || op == "<=" || op == ">" || op == ">=";
}

std::string NormalizedOperator(std::string op) {
  if (op == "!=") return "<>";
  return op;
}

std::string InvertOperator(const std::string& op) {
  if (op == "=") return "<>";
  if (op == "<>") return "=";
  if (op == "<") return ">=";
  if (op == "<=") return ">";
  if (op == ">") return "<=";
  if (op == ">=") return "<";
  return op;
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

bool StartsWith(std::string_view value, std::string_view prefix) {
  return value.size() >= prefix.size() && value.substr(0, prefix.size()) == prefix;
}

bool IsConstExpr(const std::string& value) {
  return StartsWith(value, "const(") || value == "true" || value == "false" || value == "null";
}

std::vector<std::string> SortedUnique(std::vector<std::string> values) {
  std::sort(values.begin(), values.end());
  values.erase(std::unique(values.begin(), values.end()), values.end());
  return values;
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
    if (inner[i] == '(') ++depth;
    if (inner[i] == ')') --depth;
    if (inner[i] == ',' && depth == 0) {
      values.push_back(inner.substr(start, i - start));
      start = i + 1;
    }
  }
  values.push_back(inner.substr(start));
  return values;
}

struct Parsed {
  bool ok = true;
  std::string text;
  bool boolean_equivalent = false;
  bool like_prefix_predicate = false;
  std::string like_prefix;
  std::string like_refusal_reason;
  std::vector<std::string> searchable_expression_digests;
};

class Lexer {
 public:
  explicit Lexer(std::string_view input) : input_(input) {}

  Token Next() {
    while (pos_ < input_.size() && std::isspace(static_cast<unsigned char>(input_[pos_]))) ++pos_;
    if (pos_ >= input_.size()) return {};
    const char ch = input_[pos_];
    if (std::isalpha(static_cast<unsigned char>(ch)) || ch == '_' || ch == '.') {
      std::string text;
      while (pos_ < input_.size()) {
        const char c = input_[pos_];
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_' && c != '.') break;
        text.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
        ++pos_;
      }
      return {TokenKind::kIdentifier, std::move(text)};
    }
    if (std::isdigit(static_cast<unsigned char>(ch))) {
      std::string text;
      while (pos_ < input_.size() &&
             (std::isdigit(static_cast<unsigned char>(input_[pos_])) || input_[pos_] == '.')) {
        text.push_back(input_[pos_++]);
      }
      return {TokenKind::kNumber, std::move(text)};
    }
    if (ch == '\'') {
      ++pos_;
      std::string text;
      while (pos_ < input_.size()) {
        const char c = input_[pos_++];
        if (c == '\'') {
          if (pos_ < input_.size() && input_[pos_] == '\'') {
            text.push_back('\'');
            ++pos_;
            continue;
          }
          break;
        }
        text.push_back(c);
      }
      return {TokenKind::kString, std::move(text)};
    }
    ++pos_;
    if (ch == '(') return {TokenKind::kLParen, "("};
    if (ch == ')') return {TokenKind::kRParen, ")"};
    if (ch == ',') return {TokenKind::kComma, ","};
    if ((ch == '<' || ch == '>' || ch == '!' || ch == '=') && pos_ < input_.size() && input_[pos_] == '=') {
      return {TokenKind::kOperator, std::string{ch} + input_[pos_++]};
    }
    if (ch == '<' && pos_ < input_.size() && input_[pos_] == '>') {
      ++pos_;
      return {TokenKind::kOperator, "<>"};
    }
    if (ch == '<' || ch == '>' || ch == '=') return {TokenKind::kOperator, std::string{ch}};
    return {TokenKind::kOperator, std::string{ch}};
  }

 private:
  std::string_view input_;
  std::size_t pos_ = 0;
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
  }

  Parsed ParsePredicate() {
    auto parsed = ParseOr();
    if (Peek().kind != TokenKind::kEnd) parsed.ok = false;
    return parsed;
  }

  Parsed ParseExpressionOnly() {
    auto parsed = ParseValue();
    if (Peek().kind != TokenKind::kEnd) parsed.ok = false;
    if (parsed.ok) {
      parsed.searchable_expression_digests = {CanonicalPredicateDigest(parsed.text)};
    }
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
    bool ok = left.ok;
    bool boolean_equivalent = left.boolean_equivalent;
    auto expressions = left.searchable_expression_digests;
    while (MatchKeyword("or")) {
      auto right = ParseAnd();
      ok = ok && right.ok;
      boolean_equivalent = boolean_equivalent || right.boolean_equivalent;
      expressions.insert(expressions.end(),
                         right.searchable_expression_digests.begin(),
                         right.searchable_expression_digests.end());
      if (StartsWith(right.text, "or(") && right.text.back() == ')') {
        auto nested = SplitTopLevelArgs(right.text.substr(3, right.text.size() - 4));
        terms.insert(terms.end(), nested.begin(), nested.end());
      } else {
        terms.push_back(right.text);
      }
    }
    if (terms.size() == 1) return left;
    terms = SortedUnique(std::move(terms));
    return {ok, "or(" + Join(terms, ",") + ")", boolean_equivalent, false, {}, {}, SortedUnique(std::move(expressions))};
  }

  Parsed ParseAnd() {
    auto left = ParseNot();
    std::vector<std::string> terms{left.text};
    bool ok = left.ok;
    bool boolean_equivalent = left.boolean_equivalent;
    bool like = left.like_prefix_predicate;
    std::string like_prefix = left.like_prefix;
    std::string like_refusal = left.like_refusal_reason;
    auto expressions = left.searchable_expression_digests;
    while (MatchKeyword("and")) {
      auto right = ParseNot();
      ok = ok && right.ok;
      boolean_equivalent = boolean_equivalent || right.boolean_equivalent;
      like = like || right.like_prefix_predicate;
      if (like_prefix.empty()) like_prefix = right.like_prefix;
      if (like_refusal.empty()) like_refusal = right.like_refusal_reason;
      expressions.insert(expressions.end(),
                         right.searchable_expression_digests.begin(),
                         right.searchable_expression_digests.end());
      if (StartsWith(right.text, "and(") && right.text.back() == ')') {
        auto nested = SplitTopLevelArgs(right.text.substr(4, right.text.size() - 5));
        terms.insert(terms.end(), nested.begin(), nested.end());
      } else {
        terms.push_back(right.text);
      }
    }
    if (terms.size() == 1) return left;
    terms = SortedUnique(std::move(terms));
    return {ok, "and(" + Join(terms, ",") + ")", boolean_equivalent, like, like_prefix, like_refusal,
            SortedUnique(std::move(expressions))};
  }

  Parsed ParseNot() {
    if (!MatchKeyword("not")) return ParseAtomPredicate();
    auto child = ParseNot();
    if (StartsWith(child.text, "not(") && child.text.back() == ')') {
      child.text = child.text.substr(4, child.text.size() - 5);
      child.boolean_equivalent = true;
      return child;
    }
    if (StartsWith(child.text, "bool(") && child.text.back() == ')') {
      const auto comma = child.text.rfind(',');
      if (comma != std::string::npos) {
        const auto expr = child.text.substr(5, comma - 5);
        const auto value = child.text.substr(comma + 1, child.text.size() - comma - 2);
        child.text = "bool(" + expr + "," + (value == "true" ? "false" : "true") + ")";
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
      return inner;
    }

    auto left = ParseValue();
    if (!left.ok) return left;

    if (IsKeyword(Peek(), "is")) {
      Consume();
      bool negated = MatchKeyword("not");
      if (!MatchKeyword("null")) return {false, left.text};
      return {true, "is_null(" + left.text + "," + (negated ? "false" : "true") + ")", false, false, {}, {},
              {CanonicalPredicateDigest(left.text)}};
    }

    if (IsKeyword(Peek(), "like")) {
      Consume();
      auto pattern = ParseValue();
      std::string escape;
      if (MatchKeyword("escape")) {
        auto escape_value = ParseValue();
        escape = escape_value.text;
      }
      Parsed out;
      out.ok = left.ok && pattern.ok;
      out.text = "like(" + left.text + "," + pattern.text;
      if (!escape.empty()) out.text += ",escape=" + escape;
      out.text += ")";
      out.searchable_expression_digests = {CanonicalPredicateDigest(left.text)};
      out.like_prefix_predicate = true;
      ClassifyLikePattern(pattern.text, escape, &out);
      return out;
    }

    if (IsKeyword(Peek(), "in")) {
      Consume();
      Parsed out;
      out.ok = left.ok && Match(TokenKind::kLParen);
      std::vector<std::string> values;
      if (out.ok && !Match(TokenKind::kRParen)) {
        for (;;) {
          auto item = ParseValue();
          out.ok = out.ok && item.ok;
          values.push_back(item.text);
          if (Match(TokenKind::kRParen)) break;
          if (!Match(TokenKind::kComma)) {
            out.ok = false;
            break;
          }
        }
      }
      values = SortedUnique(std::move(values));
      out.text = "in(" + left.text;
      for (const auto& value : values) out.text += "," + value;
      out.text += ")";
      out.searchable_expression_digests = {CanonicalPredicateDigest(left.text)};
      return out;
    }

    if (Peek().kind == TokenKind::kOperator && IsComparisonOperator(Peek().text)) {
      std::string op = NormalizedOperator(Consume().text);
      auto right = ParseValue();
      if (!right.ok) return {false, left.text};
      return BuildComparison(std::move(left), std::move(op), std::move(right));
    }

    if (left.text == "true" || left.text == "false") {
      return left;
    }
    const std::string expression_text = left.text;
    left.text = "bool(" + expression_text + ",true)";
    left.boolean_equivalent = true;
    left.searchable_expression_digests = {CanonicalPredicateDigest(expression_text)};
    return left;
  }

  Parsed ParseValue() {
    if (Peek().kind == TokenKind::kNumber) return {true, "const(number:" + Consume().text + ")"};
    if (Peek().kind == TokenKind::kString) return {true, "const(string:" + Consume().text + ")"};
    if (MatchKeyword("true")) return {true, "true"};
    if (MatchKeyword("false")) return {true, "false"};
    if (MatchKeyword("null")) return {true, "null"};
    if (Peek().kind != TokenKind::kIdentifier) return {false, "parse_error"};

    std::string name = Consume().text;
    if (Match(TokenKind::kLParen)) {
      std::vector<std::string> args;
      if (!Match(TokenKind::kRParen)) {
        for (;;) {
          auto arg = ParseValue();
          if (!arg.ok) return arg;
          args.push_back(arg.text);
          if (Match(TokenKind::kRParen)) break;
          if (!Match(TokenKind::kComma)) return {false, name};
        }
      }
      return {true, "fn(" + name + ":" + Join(args, ",") + ")"};
    }
    return {true, "col(" + name + ")"};
  }

  Parsed BuildComparison(Parsed left, std::string op, Parsed right) {
    bool commuted = false;
    if (IsConstExpr(left.text) && !IsConstExpr(right.text)) {
      std::swap(left, right);
      op = CommuteOperator(op);
      commuted = true;
    } else if (!IsConstExpr(left.text) && !IsConstExpr(right.text) && right.text < left.text) {
      std::swap(left, right);
      op = CommuteOperator(op);
      commuted = true;
    }
    if ((right.text == "true" || right.text == "false") && (op == "=" || op == "<>")) {
      const bool truth = (right.text == "true") == (op == "=");
      return {true, "bool(" + left.text + "," + (truth ? "true" : "false") + ")",
              true, false, {}, {}, {CanonicalPredicateDigest(left.text)}};
    }
    if ((left.text == "true" || left.text == "false") && (op == "=" || op == "<>")) {
      const bool truth = (left.text == "true") == (op == "=");
      return {true, "bool(" + right.text + "," + (truth ? "true" : "false") + ")",
              true, false, {}, {}, {CanonicalPredicateDigest(right.text)}};
    }
    if (op == "!=") op = "<>";
    (void)commuted;
    return {true, "cmp(" + op + "," + left.text + "," + right.text + ")", false, false, {}, {},
            {CanonicalPredicateDigest(left.text)}};
  }

  static void ClassifyLikePattern(const std::string& pattern_text,
                                  const std::string& escape_text,
                                  Parsed* out) {
    if (out == nullptr) return;
    if (!StartsWith(pattern_text, "const(string:") || pattern_text.back() != ')') {
      out->like_refusal_reason = "like_prefix_nonconstant_pattern_refused";
      return;
    }
    if (!escape_text.empty() && escape_text != "const(string:\\)") {
      out->like_refusal_reason = "like_prefix_unsupported_escape_refused";
      return;
    }
    const std::string pattern = pattern_text.substr(13, pattern_text.size() - 14);
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
      if (!escape_text.empty() && ch == '\\') {
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

  std::vector<Token> tokens_;
  std::size_t pos_ = 0;
};

std::vector<std::string> ConjunctsForCanonicalText(const std::string& text) {
  if (!StartsWith(text, "and(") || text.back() != ')') return {text};
  std::vector<std::string> values;
  const std::string inner = text.substr(4, text.size() - 5);
  std::size_t start = 0;
  int depth = 0;
  for (std::size_t i = 0; i < inner.size(); ++i) {
    if (inner[i] == '(') ++depth;
    if (inner[i] == ')') --depth;
    if (inner[i] == ',' && depth == 0) {
      const auto child = ConjunctsForCanonicalText(inner.substr(start, i - start));
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
    if (!StartsWith(text, prefix) || text.back() != ')') return {};
    std::vector<std::string> terms;
    for (auto term : SplitTopLevelArgs(text.substr(prefix.size(), text.size() - prefix.size() - 1))) {
      term = NormalizeCanonicalLogicalText(term);
      if (StartsWith(term, prefix) && term.back() == ')') {
        auto nested = SplitTopLevelArgs(term.substr(prefix.size(), term.size() - prefix.size() - 1));
        terms.insert(terms.end(), nested.begin(), nested.end());
      } else {
        terms.push_back(std::move(term));
      }
    }
    terms = SortedUnique(std::move(terms));
    return prefix + Join(terms, ",") + ")";
  };
  if (auto normalized = normalize_function("and"); !normalized.empty()) return normalized;
  if (auto normalized = normalize_function("or"); !normalized.empty()) return normalized;
  return text;
}

bool HasDigest(const std::vector<std::string>& values, const std::string& digest) {
  return !digest.empty() && std::find(values.begin(), values.end(), digest) != values.end();
}

bool AnyDigestMatches(const std::vector<std::string>& left, const std::vector<std::string>& right) {
  for (const auto& value : left) {
    if (HasDigest(right, value)) return true;
  }
  return false;
}

bool HasVolatilityMismatch(const IndexStats& index) {
  return !index.function_volatility.empty() &&
         index.function_volatility != "immutable";
}

bool SafeSblrIdentityToken(const std::string& value) {
  if (value.empty()) return true;
  if (value.size() > 200) return false;
  return std::all_of(value.begin(), value.end(), [](unsigned char ch) {
    return std::isalnum(ch) || ch == '_' || ch == ':' || ch == '.' ||
           ch == '-' || ch == '=' || ch == '/';
  });
}

void AddSblrDiagnostic(CanonicalSblrExpression* expression,
                       std::string diagnostic) {
  if (expression == nullptr) return;
  expression->ok = false;
  expression->diagnostics.push_back(std::move(diagnostic));
}

std::string CanonicalizeSblrExpressionNode(
    const CanonicalSblrExpressionNode& node,
    CanonicalSblrExpression* result) {
  if (node.raw_sql_text_present ||
      node.parser_execution_authority_claimed) {
    AddSblrDiagnostic(result, "sblr_expression_parser_sql_authority_refused");
  }
  if (node.donor_or_legacy_authority_claimed) {
    AddSblrDiagnostic(result, "sblr_expression_donor_authority_refused");
  }
  if (node.name_authority_claimed) {
    AddSblrDiagnostic(result, "sblr_expression_name_authority_refused");
  }
  if (!node.normalized_sblr_metadata) {
    AddSblrDiagnostic(result, "sblr_expression_metadata_not_normalized");
  }
  if (node.operator_id.empty()) {
    AddSblrDiagnostic(result, "sblr_expression_operator_required");
  }
  if (!SafeSblrIdentityToken(node.operator_id) ||
      !SafeSblrIdentityToken(node.descriptor_digest) ||
      !SafeSblrIdentityToken(node.object_uuid) ||
      !SafeSblrIdentityToken(node.function_uuid) ||
      !SafeSblrIdentityToken(node.literal_digest)) {
    AddSblrDiagnostic(result, "sblr_expression_identity_token_unsafe");
  }

  std::vector<std::string> children;
  children.reserve(node.children.size());
  for (const auto& child : node.children) {
    children.push_back(CanonicalizeSblrExpressionNode(child, result));
  }
  if (node.commutative) {
    children = SortedUnique(std::move(children));
  }

  std::ostringstream out;
  out << "sblr_expr(op=" << node.operator_id
      << ";desc=" << node.descriptor_digest
      << ";object=" << node.object_uuid
      << ";function=" << node.function_uuid
      << ";literal=" << node.literal_digest
      << ";commutative=" << (node.commutative ? "true" : "false")
      << ";children=[" << Join(children, "|") << "])";
  return out.str();
}

void AddMetadataRecheckAcceptance(const PredicateIndexMatchRequest& request,
                                  PredicateIndexMatchResult* result) {
  result->acceptance_reasons.push_back("metadata_match_only");
  result->acceptance_reasons.push_back("metadata_finality_not_cached");
  if (request.base_row_mga_recheck_planned) {
    result->acceptance_reasons.push_back("metadata_match_only_mga_visibility_recheck_required");
  } else {
    result->refusal_reasons.push_back("metadata_match_only_mga_visibility_recheck_missing");
  }
  if (request.base_row_security_recheck_planned) {
    result->acceptance_reasons.push_back("metadata_match_only_security_recheck_required");
  } else {
    result->refusal_reasons.push_back("metadata_match_only_security_recheck_missing");
  }
}

scratchbird::core::index::PartialPredicateImplicationRequest
MakePartialImplicationRequest(const PredicateIndexMatchRequest& request) {
  scratchbird::core::index::PartialPredicateImplicationRequest proof_request;
  proof_request.query_predicate_text = request.query_predicate_text;
  proof_request.index_predicate_text = request.index.partial_predicate_text;
  proof_request.predicate_immutable = request.index.partial_predicate_immutable &&
                                      !HasVolatilityMismatch(request.index);
  proof_request.predicate_security_safe = request.index.partial_predicate_security_safe;
  proof_request.descriptor_epoch_valid = request.index.descriptor_epoch_valid &&
                                         (request.index.descriptor_digest.empty() ||
                                          request.descriptor_digest.empty() ||
                                          request.index.descriptor_digest ==
                                              request.descriptor_digest);
  proof_request.resource_epoch_valid = request.index.resource_epoch_valid;
  proof_request.collation_epoch_valid = request.index.collation_epoch_valid &&
                                        request.index.collation_deterministic &&
                                        (request.index.collation_identity.empty() ||
                                         request.collation_identity.empty() ||
                                         request.index.collation_identity ==
                                             request.collation_identity);
  proof_request.function_epoch_valid = request.index.function_epoch_valid;
  proof_request.base_row_mga_recheck_planned =
      request.base_row_mga_recheck_planned;
  proof_request.base_row_security_recheck_planned =
      request.base_row_security_recheck_planned;
  return proof_request;
}

}  // namespace

std::string CanonicalPredicateDigest(const std::string& canonical_text) {
  return "sbpred64:" + Hex64(Fnv1a64(canonical_text));
}

std::string CanonicalSblrExpressionDigest(const std::string& canonical_text) {
  return "sblrexpr64:" + Hex64(Fnv1a64(canonical_text));
}

CanonicalPredicate CanonicalizePredicateText(const std::string& predicate_text) {
  CanonicalPredicate result;
  Parser parser(predicate_text);
  const auto parsed = parser.ParsePredicate();
  result.ok = parsed.ok;
  result.canonical_text = NormalizeCanonicalLogicalText(parsed.text);
  result.digest = CanonicalPredicateDigest(result.canonical_text);
  result.boolean_equivalent = parsed.boolean_equivalent;
  result.like_prefix_predicate = parsed.like_prefix_predicate;
  result.like_prefix = parsed.like_prefix;
  result.like_refusal_reason = parsed.like_refusal_reason;
  result.searchable_expression_digests = SortedUnique(parsed.searchable_expression_digests);
  result.conjuncts = ConjunctsForCanonicalText(result.canonical_text);
  if (!result.ok) result.diagnostics.push_back("predicate_parse_refused");
  return result;
}

CanonicalPredicate CanonicalizeExpressionText(const std::string& expression_text) {
  CanonicalPredicate result;
  Parser parser(expression_text);
  const auto parsed = parser.ParseExpressionOnly();
  result.ok = parsed.ok;
  result.canonical_text = parsed.text;
  result.digest = CanonicalPredicateDigest(result.canonical_text);
  result.searchable_expression_digests = {result.digest};
  if (!result.ok) result.diagnostics.push_back("expression_parse_refused");
  return result;
}

CanonicalSblrExpression CanonicalizeSblrExpressionTree(
    const CanonicalSblrExpressionNode& expression) {
  CanonicalSblrExpression result;
  result.ok = true;
  result.canonical_text = CanonicalizeSblrExpressionNode(expression, &result);
  result.digest = CanonicalSblrExpressionDigest(result.canonical_text);
  result.searchable_expression_digests = {result.digest};
  if (result.ok) {
    result.evidence.push_back("canonical_sblr_expression_tree=true");
    result.evidence.push_back("canonical_sblr_expression_digest=" +
                              result.digest);
    result.evidence.push_back("parser_sql_expression_authority=false");
    result.evidence.push_back("donor_expression_authority=false");
    result.evidence.push_back("name_expression_authority=false");
    result.evidence.push_back("mga_visibility_recheck_required=true");
    result.evidence.push_back(
        "transaction_finality_authority=engine_transaction_inventory");
  }
  return result;
}

std::string DeterministicPredicateDescriptorText(const CanonicalPredicate& predicate) {
  std::ostringstream out;
  out << "canonical_predicate_v1{text=" << predicate.canonical_text
      << ";digest=" << predicate.digest
      << ";conjuncts=" << Join(predicate.conjuncts, "|")
      << ";expressions=" << Join(predicate.searchable_expression_digests, "|") << "}";
  return out.str();
}

bool CanonicalPredicateImplies(const CanonicalPredicate& query,
                               const CanonicalPredicate& required) {
  if (!query.ok || !required.ok) return false;
  if (query.digest == required.digest) return true;
  const auto query_terms = SortedUnique(query.conjuncts);
  const auto required_terms = SortedUnique(required.conjuncts);
  return std::all_of(required_terms.begin(), required_terms.end(), [&](const std::string& required_term) {
    return std::find(query_terms.begin(), query_terms.end(), required_term) != query_terms.end();
  });
}

PredicateIndexMatchResult MatchPredicateToIndex(const PredicateIndexMatchRequest& request) {
  PredicateIndexMatchResult result;
  const auto query = CanonicalizePredicateText(request.query_predicate_text);
  result.canonical_predicate_text = query.canonical_text;
  result.canonical_predicate_digest = query.digest;
  if (!query.ok) {
    result.refusal_reasons = query.diagnostics;
    return result;
  }

  if (!request.index.descriptor_digest.empty() &&
      !request.descriptor_digest.empty() &&
      request.index.descriptor_digest != request.descriptor_digest) {
    result.refusal_reasons.push_back("descriptor_digest_mismatch");
  }
  if (!request.index.collation_identity.empty() &&
      !request.collation_identity.empty() &&
      request.index.collation_identity != request.collation_identity) {
    result.refusal_reasons.push_back("collation_mismatch");
  }
  if (HasVolatilityMismatch(request.index)) {
    result.refusal_reasons.push_back("function_volatility_mismatch");
  }
  if (request.index.partial) {
    if (request.index.partial_predicate_text.empty()) {
      result.refusal_reasons.push_back("partial_predicate_text_missing");
      result.refusal_reasons.push_back("partial_predicate_not_proven");
    } else {
      const auto proof = scratchbird::core::index::ProvePartialIndexPredicateImplication(
          MakePartialImplicationRequest(request));
      result.acceptance_reasons.insert(result.acceptance_reasons.end(),
                                       proof.acceptance_reasons.begin(),
                                       proof.acceptance_reasons.end());
      if (proof.predicate_implied) {
        result.acceptance_reasons.insert(result.acceptance_reasons.end(),
                                         proof.evidence.begin(),
                                         proof.evidence.end());
      } else {
        result.refusal_reasons.insert(result.refusal_reasons.end(),
                                      proof.refusal_reasons.begin(),
                                      proof.refusal_reasons.end());
        if (std::find(result.refusal_reasons.begin(),
                      result.refusal_reasons.end(),
                      "partial_predicate_not_proven") ==
            result.refusal_reasons.end()) {
          result.refusal_reasons.push_back("partial_predicate_not_proven");
        }
      }
    }
  }

  bool expression_matched = false;
  if (request.index.expression_index) {
    if (AnyDigestMatches(query.searchable_expression_digests, request.index.key_expression_digests)) {
      result.acceptance_reasons.push_back("functional_index_expression_digest_match");
      expression_matched = true;
    } else {
      result.refusal_reasons.push_back("expression_digest_mismatch");
    }
  }
  if (!request.index.generated_column_expression_digest.empty()) {
    if (HasDigest(query.searchable_expression_digests, request.index.generated_column_expression_digest)) {
      result.acceptance_reasons.push_back("generated_column_expression_digest_match");
      expression_matched = true;
    } else {
      result.refusal_reasons.push_back("generated_column_expression_digest_mismatch");
    }
  }
  if (!request.index.computed_expression_digest.empty()) {
    if (HasDigest(query.searchable_expression_digests, request.index.computed_expression_digest)) {
      result.acceptance_reasons.push_back("computed_expression_digest_match");
      expression_matched = true;
    } else {
      result.refusal_reasons.push_back("computed_expression_digest_mismatch");
    }
  }
  if (query.boolean_equivalent && expression_matched) {
    result.acceptance_reasons.push_back("boolean_equivalent_match");
  }
  if (query.like_prefix_predicate) {
    if (!request.index.like_prefix_capable) {
      result.refusal_reasons.push_back("like_prefix_index_not_capable_refused");
    } else if (!request.index.collation_deterministic) {
      result.refusal_reasons.push_back("like_prefix_nondeterministic_collation_refused");
    } else if (!query.like_refusal_reason.empty()) {
      result.refusal_reasons.push_back(query.like_refusal_reason);
    } else if (!request.index.key_expression_digests.empty() &&
               !AnyDigestMatches(query.searchable_expression_digests,
                                 request.index.key_expression_digests)) {
      result.refusal_reasons.push_back("like_prefix_expression_digest_mismatch");
    } else {
      result.acceptance_reasons.push_back("like_prefix_accepted");
      expression_matched = true;
    }
  }

  if (!request.index.expression_index &&
      request.index.generated_column_expression_digest.empty() &&
      request.index.computed_expression_digest.empty() &&
      !query.like_prefix_predicate) {
    expression_matched = true;
  }

  AddMetadataRecheckAcceptance(request, &result);
  result.matches = expression_matched && result.refusal_reasons.empty();
  return result;
}

SblrExpressionIndexMatchResult MatchSblrExpressionToIndex(
    const SblrExpressionIndexMatchRequest& request) {
  SblrExpressionIndexMatchResult result;
  const auto expression = CanonicalizeSblrExpressionTree(request.query_expression);
  result.canonical_expression_text = expression.canonical_text;
  result.canonical_expression_digest = expression.digest;
  result.evidence = expression.evidence;
  if (!expression.ok) {
    result.refusal_reasons = expression.diagnostics;
    return result;
  }

  if (!request.index.descriptor_digest.empty() &&
      !request.descriptor_digest.empty() &&
      request.index.descriptor_digest != request.descriptor_digest) {
    result.refusal_reasons.push_back("descriptor_digest_mismatch");
  }
  if (!request.index.collation_identity.empty() &&
      !request.collation_identity.empty() &&
      request.index.collation_identity != request.collation_identity) {
    result.refusal_reasons.push_back("collation_mismatch");
  }
  if (HasVolatilityMismatch(request.index)) {
    result.refusal_reasons.push_back("function_volatility_mismatch");
  }
  if (!request.base_row_mga_recheck_planned) {
    result.refusal_reasons.push_back(
        "metadata_match_only_mga_visibility_recheck_missing");
  }
  if (!request.base_row_security_recheck_planned) {
    result.refusal_reasons.push_back(
        "metadata_match_only_security_recheck_missing");
  }

  bool expression_matched = false;
  if (request.index.expression_index) {
    if (HasDigest(request.index.key_expression_digests, expression.digest)) {
      result.acceptance_reasons.push_back(
          "canonical_sblr_expression_digest_match");
      expression_matched = true;
    } else {
      result.refusal_reasons.push_back("sblr_expression_digest_mismatch");
    }
  }
  if (!request.index.generated_column_expression_digest.empty()) {
    if (request.index.generated_column_expression_digest == expression.digest) {
      result.acceptance_reasons.push_back(
          "canonical_sblr_generated_column_digest_match");
      expression_matched = true;
    } else {
      result.refusal_reasons.push_back(
          "sblr_generated_column_expression_digest_mismatch");
    }
  }
  if (!request.index.computed_expression_digest.empty()) {
    if (request.index.computed_expression_digest == expression.digest) {
      result.acceptance_reasons.push_back(
          "canonical_sblr_computed_expression_digest_match");
      expression_matched = true;
    } else {
      result.refusal_reasons.push_back(
          "sblr_computed_expression_digest_mismatch");
    }
  }
  if (!request.index.expression_index &&
      request.index.generated_column_expression_digest.empty() &&
      request.index.computed_expression_digest.empty()) {
    result.refusal_reasons.push_back("sblr_expression_index_digest_required");
  }

  if (request.base_row_mga_recheck_planned) {
    result.acceptance_reasons.push_back(
        "metadata_match_only_mga_visibility_recheck_required");
  }
  if (request.base_row_security_recheck_planned) {
    result.acceptance_reasons.push_back(
        "metadata_match_only_security_recheck_required");
  }
  result.acceptance_reasons.push_back("metadata_match_only");
  result.acceptance_reasons.push_back("metadata_finality_not_cached");
  result.evidence.push_back("sblr_expression_match_metadata_only=true");
  result.evidence.push_back(
      "sblr_expression_match_parser_sql_authority=false");
  result.evidence.push_back(
      "sblr_expression_match_donor_authority=false");
  result.matches = expression_matched && result.refusal_reasons.empty();
  return result;
}

}  // namespace scratchbird::engine::optimizer

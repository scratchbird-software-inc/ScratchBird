// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "lexer/lexer.hpp"

#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace sbsql = scratchbird::parser::sbsql;

namespace {

using CsvRow = std::unordered_map<std::string, std::string>;

std::vector<std::string> SplitCsvLine(std::string_view line) {
  std::vector<std::string> fields;
  std::string current;
  bool in_quotes = false;
  for (std::size_t i = 0; i < line.size(); ++i) {
    const char ch = line[i];
    if (in_quotes) {
      if (ch == '"') {
        if (i + 1 < line.size() && line[i + 1] == '"') {
          current.push_back('"');
          ++i;
        } else {
          in_quotes = false;
        }
      } else {
        current.push_back(ch);
      }
      continue;
    }
    if (ch == '"') {
      in_quotes = true;
    } else if (ch == ',') {
      fields.push_back(current);
      current.clear();
    } else {
      current.push_back(ch);
    }
  }
  fields.push_back(current);
  return fields;
}

std::vector<CsvRow> ReadCsv(const std::string& path) {
  std::ifstream input(path);
  if (!input) throw std::runtime_error("failed to open " + path);
  std::string line;
  if (!std::getline(input, line)) throw std::runtime_error("empty CSV " + path);
  if (!line.empty() && line.back() == '\r') line.pop_back();
  const auto headers = SplitCsvLine(line);

  std::vector<CsvRow> rows;
  while (std::getline(input, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.empty()) continue;
    const auto fields = SplitCsvLine(line);
    if (fields.size() != headers.size()) throw std::runtime_error("malformed CSV " + path);
    CsvRow row;
    for (std::size_t index = 0; index < headers.size(); ++index) {
      row.emplace(headers[index], fields[index]);
    }
    rows.push_back(std::move(row));
  }
  return rows;
}

std::string Field(const CsvRow& row, std::string_view field) {
  const auto found = row.find(std::string(field));
  if (found == row.end()) return {};
  return found->second;
}

std::vector<sbsql::Token> MeaningfulTokens(const sbsql::LexResult& lexed) {
  std::vector<sbsql::Token> tokens;
  for (const auto& token : lexed.tokens) {
    if (!sbsql::IsTriviaToken(token) && token.kind != sbsql::TokenKind::kEnd) {
      tokens.push_back(token);
    }
  }
  return tokens;
}

bool Require(bool condition, std::string_view message) {
  if (condition) return true;
  std::cerr << message << "\n";
  return false;
}

bool HasTokenKind(const sbsql::LexResult& lexed, sbsql::TokenKind kind) {
  for (const auto& token : lexed.tokens) {
    if (token.kind == kind) return true;
  }
  return false;
}

bool HasLiteralFamily(const sbsql::LexResult& lexed, std::string_view family) {
  for (const auto& token : lexed.tokens) {
    if (token.literal_family == family) return true;
  }
  return false;
}

bool ValidateNoErrors(std::string_view source) {
  const auto lexed = sbsql::Lex(source);
  if (!lexed.messages.has_errors()) return true;
  std::cerr << "unexpected lexer error for source: " << source << "\n";
  return false;
}

bool ValidateCoreTokenClasses() {
  bool ok = true;

  const auto trivia = sbsql::Lex("-- line\nSELECT /* block */ 1;");
  ok &= Require(HasTokenKind(trivia, sbsql::TokenKind::kComment),
                "comment token was not preserved");
  ok &= Require(HasTokenKind(trivia, sbsql::TokenKind::kWhitespace),
                "whitespace token was not preserved");
  ok &= Require(HasTokenKind(trivia, sbsql::TokenKind::kStatementTerminator),
                "statement terminator token missing");
  const auto meaningful = MeaningfulTokens(trivia);
  ok &= Require(meaningful.size() == 3, "unexpected meaningful token count");
  ok &= Require(!meaningful.empty() && meaningful.front().kind == sbsql::TokenKind::kKeyword,
                "SELECT was not the first meaningful token");
  ok &= Require(!meaningful.empty() && meaningful.front().line == 2,
                "line span was not preserved");

  const auto identifiers = sbsql::Lex(R"SQL("Quoted Name" `reference name` [bracket name] bare_name)SQL");
  ok &= Require(!identifiers.messages.has_errors(), "identifier forms produced errors");
  const auto id_tokens = MeaningfulTokens(identifiers);
  ok &= Require(id_tokens.size() == 4, "identifier token count mismatch");
  ok &= Require(id_tokens[0].quoted && id_tokens[1].quoted && id_tokens[2].quoted,
                "quoted identifier flags missing");

  const auto numbers = sbsql::Lex(
      "0 123u 170141183460469231731687303715884105727i128 "
      "340282366920938463463374607431768211455u128 1.25dec 6.02e23 "
      "1.0r128 0xFF 0b1010 0o77");
  ok &= Require(!numbers.messages.has_errors(), "numeric literal forms produced errors");
  ok &= Require(HasLiteralFamily(numbers, "uint"), "uint literal missing");
  ok &= Require(HasLiteralFamily(numbers, "int128"), "int128 literal missing");
  ok &= Require(HasLiteralFamily(numbers, "uint128"), "uint128 literal missing");
  ok &= Require(HasLiteralFamily(numbers, "decimal"), "decimal literal missing");
  ok &= Require(HasLiteralFamily(numbers, "float"), "float literal missing");
  ok &= Require(HasLiteralFamily(numbers, "real128"), "real128 literal missing");
  ok &= Require(HasLiteralFamily(numbers, "hex_integer"), "hex literal missing");
  ok &= Require(HasLiteralFamily(numbers, "binary_integer"), "binary integer missing");
  ok &= Require(HasLiteralFamily(numbers, "octal_integer"), "octal literal missing");

  const auto strings = sbsql::Lex(
      "'can''t' N'national' E'escape' X'0AFF' B'0101' $$body$$");
  ok &= Require(!strings.messages.has_errors(), "string/binary literal forms produced errors");
  ok &= Require(HasLiteralFamily(strings, "string"), "string literal missing");
  ok &= Require(HasLiteralFamily(strings, "national_string"), "national string missing");
  ok &= Require(HasLiteralFamily(strings, "escaped_string"), "escaped string missing");
  ok &= Require(HasLiteralFamily(strings, "hex_binary"), "hex binary missing");
  ok &= Require(HasLiteralFamily(strings, "bit_binary"), "bit binary missing");
  ok &= Require(HasLiteralFamily(strings, "dollar_quoted_string"),
                "dollar quoted string missing");

  const auto typed = sbsql::Lex(
      "019dffbb-f000-7cd2-bcbf-1a1c6956fa31 UUID '019dffbb-f000-7cd2-bcbf-1a1c6956fa31' "
      "DATE '2026-05-07' TIME '12:13:14' TIMESTAMP '2026-05-07T12:13:14Z' "
      "INTERVAL 'P1D' JSON '{\"a\":1}' VECTOR '[1,2,3]' REGEX '^a+$' RANGE '[1,2)'");
  ok &= Require(!typed.messages.has_errors(), "typed literal forms produced errors");
  ok &= Require(HasTokenKind(typed, sbsql::TokenKind::kUuidLiteral), "UUID literal missing");
  ok &= Require(HasTokenKind(typed, sbsql::TokenKind::kTemporalLiteral),
                "temporal literal missing");
  ok &= Require(HasTokenKind(typed, sbsql::TokenKind::kDocumentLiteral),
                "document literal missing");
  ok &= Require(HasTokenKind(typed, sbsql::TokenKind::kVectorLiteral),
                "vector literal missing");
  ok &= Require(HasTokenKind(typed, sbsql::TokenKind::kRegexLiteral),
                "regex literal missing");
  ok &= Require(HasTokenKind(typed, sbsql::TokenKind::kRangeLiteral),
                "range literal missing");

  const auto parameters = sbsql::Lex("? :name $1 @session @@identity");
  ok &= Require(!parameters.messages.has_errors(), "parameter/variable forms produced errors");
  ok &= Require(HasTokenKind(parameters, sbsql::TokenKind::kParameter),
                "parameter token missing");
  ok &= Require(HasTokenKind(parameters, sbsql::TokenKind::kVariable),
                "variable token missing");

  const auto operators = sbsql::Lex("a->>'b' <= 3 :: int #>> '{a}' ?| ARRAY['x'];");
  ok &= Require(!operators.messages.has_errors(), "operator forms produced errors");
  ok &= Require(HasTokenKind(operators, sbsql::TokenKind::kOperator),
                "operator token missing");
  ok &= Require(HasTokenKind(operators, sbsql::TokenKind::kStatementTerminator),
                "terminator missing in operator case");

  return ok;
}

bool ValidateDiagnostics() {
  bool ok = true;
  ok &= Require(sbsql::Lex("'unterminated").messages.has_errors(),
                "unterminated string did not fail");
  ok &= Require(sbsql::Lex("/* unterminated").messages.has_errors(),
                "unterminated block comment did not fail");
  ok &= Require(sbsql::Lex("\"unterminated").messages.has_errors(),
                "unterminated quoted identifier did not fail");
  return ok;
}

bool ValidateGeneratedGrammarRows(const std::string& artifact_root) {
  const auto rows = ReadCsv(artifact_root + "/SURFACE_IMPLEMENTATION_BACKLOG.csv");
  std::size_t checked = 0;
  bool ok = true;
  for (const auto& row : rows) {
    const auto target_group = Field(row, "target_file_group");
    if (target_group.find("project/src/parsers/sbsql_worker/lexer") == std::string::npos) {
      continue;
    }
    const auto canonical_name = Field(row, "canonical_name");
    if (canonical_name.empty()) {
      std::cerr << "lexer-target artifact row missing canonical_name\n";
      ok = false;
      continue;
    }
    ok &= ValidateNoErrors(canonical_name);
    ++checked;
  }
  ok &= Require(checked == 541, "unexpected generated lexer-target row count");
  return ok;
}

} // namespace

int main(int argc, char** argv) {
  if (argc != 2) {
    std::cerr << "usage: sbp_sbsql_lexer_conformance_probe <artifact-root>\n";
    return 1;
  }
  try {
    bool ok = true;
    ok &= ValidateCoreTokenClasses();
    ok &= ValidateDiagnostics();
    ok &= ValidateGeneratedGrammarRows(argv[1]);
    if (!ok) return 1;
    std::cout << "SBSQL lexer conformance passed\n";
    return 0;
  } catch (const std::exception& ex) {
    std::cerr << "lexer conformance failed: " << ex.what() << "\n";
    return 1;
  }
}

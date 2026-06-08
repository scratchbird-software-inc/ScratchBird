// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include <cctype>
#include <iostream>
#include <set>
#include <string>
#include <vector>

namespace {

enum class Kind { Keyword, Identifier, DelimitedIdentifier, String, Number, Symbol, End };
struct Token { Kind kind; std::string text; };

bool IsKeyword(const std::string& text) {
  static const std::set<std::string> keywords = {"SELECT","FROM","WHERE","UUID","AND","SHOW","CREATE","TABLE","VECTOR","GRAPH","DOCUMENT","TIMESERIES","KV"};
  std::string upper;
  for (char c : text) upper.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
  return keywords.contains(upper);
}

std::string Upper(std::string text) {
  for (char& c : text) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  return text;
}

std::vector<Token> Lex(const std::string& input) {
  std::vector<Token> tokens;
  std::size_t i = 0;
  while (i < input.size()) {
    unsigned char ch = static_cast<unsigned char>(input[i]);
    if (std::isspace(ch)) { ++i; continue; }
    if (input.compare(i, 2, "--") == 0) { while (i < input.size() && input[i] != '\n') ++i; continue; }
    if (input.compare(i, 2, "/*") == 0) { i += 2; while (i + 1 < input.size() && input.compare(i, 2, "*/") != 0) ++i; if (i + 1 < input.size()) i += 2; continue; }
    if (input[i] == '\'') {
      std::string value; ++i;
      while (i < input.size()) {
        if (input[i] == '\'' && i + 1 < input.size() && input[i + 1] == '\'') { value.push_back('\''); i += 2; continue; }
        if (input[i] == '\'') { ++i; break; }
        value.push_back(input[i++]);
      }
      tokens.push_back({Kind::String, value});
      continue;
    }
    if (input[i] == '"') {
      std::string value; ++i;
      while (i < input.size()) {
        if (input[i] == '"' && i + 1 < input.size() && input[i + 1] == '"') { value.push_back('"'); i += 2; continue; }
        if (input[i] == '"') { ++i; break; }
        value.push_back(input[i++]);
      }
      tokens.push_back({Kind::DelimitedIdentifier, value});
      continue;
    }
    if (std::isalpha(ch) || input[i] == '_') {
      std::string value;
      while (i < input.size()) {
        unsigned char c = static_cast<unsigned char>(input[i]);
        if (!(std::isalnum(c) || input[i] == '_' || input[i] == '$')) break;
        value.push_back(input[i++]);
      }
      tokens.push_back({IsKeyword(value) ? Kind::Keyword : Kind::Identifier, IsKeyword(value) ? Upper(value) : value});
      continue;
    }
    if (std::isdigit(ch)) {
      std::string value;
      while (i < input.size()) {
        unsigned char c = static_cast<unsigned char>(input[i]);
        if (!(std::isalnum(c) || input[i] == '.' || input[i] == '-' || input[i] == '+')) break;
        value.push_back(input[i++]);
      }
      tokens.push_back({Kind::Number, value});
      continue;
    }
    tokens.push_back({Kind::Symbol, std::string(1, input[i++])});
  }
  tokens.push_back({Kind::End, {}});
  return tokens;
}

bool Has(const std::vector<Token>& tokens, Kind kind, const std::string& text) {
  for (const auto& token : tokens) if (token.kind == kind && token.text == text) return true;
  return false;
}

}  // namespace

int main() {
  const std::string source = "SELECT row_uuid, \"Localized Name\" FROM sys.catalog WHERE row_uuid = UUID '018f0000-0000-7000-8000-000000000001' AND vector_distance(v, [1.0, 2.0]) < 0.5; -- ignored";
  const auto tokens = Lex(source);
  const bool ok = Has(tokens, Kind::Keyword, "SELECT") && Has(tokens, Kind::Identifier, "row_uuid") &&
                  Has(tokens, Kind::DelimitedIdentifier, "Localized Name") && Has(tokens, Kind::Keyword, "UUID") &&
                  Has(tokens, Kind::String, "018f0000-0000-7000-8000-000000000001") &&
                  Has(tokens, Kind::Symbol, "[") && Has(tokens, Kind::Symbol, "]") &&
                  Has(tokens, Kind::Number, "1.0") && !Has(tokens, Kind::Identifier, "ignored");
  std::cout << "{\"ok\":" << (ok ? "true" : "false") << ",\"token_count\":" << tokens.size() << "}\n";
  return ok ? 0 : 1;
}

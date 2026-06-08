// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "firebird_dialect.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

std::string ReadFile(const std::filesystem::path& path) {
  std::ifstream in(path);
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

std::string StripComments(std::string_view text) {
  std::string out;
  out.reserve(text.size());
  bool in_single = false;
  bool in_double = false;
  auto q_quote_close = [](char open) {
    switch (open) {
      case '{': return '}';
      case '[': return ']';
      case '(': return ')';
      case '<': return '>';
      default: return open;
    }
  };
  for (std::size_t i = 0; i < text.size();) {
    const char ch = text[i];
    if (!in_single && !in_double && (ch == 'q' || ch == 'Q') &&
        i + 2 < text.size() && text[i + 1] == '\'' && text[i + 2] != '\'') {
      const char delimiter = q_quote_close(text[i + 2]);
      out.push_back(text[i++]);
      out.push_back(text[i++]);
      out.push_back(text[i++]);
      while (i < text.size()) {
        out.push_back(text[i]);
        if (text[i] == delimiter && i + 1 < text.size() && text[i + 1] == '\'') {
          out.push_back(text[++i]);
          ++i;
          break;
        }
        ++i;
      }
      continue;
    }
    if (!in_double && ch == '\'') {
      out.push_back(ch);
      if (in_single && i + 1 < text.size() && text[i + 1] == '\'') {
        out.push_back(text[i + 1]);
        i += 2;
        continue;
      }
      in_single = !in_single;
      ++i;
      continue;
    }
    if (!in_single && ch == '"') {
      out.push_back(ch);
      if (in_double && i + 1 < text.size() && text[i + 1] == '"') {
        out.push_back(text[i + 1]);
        i += 2;
        continue;
      }
      in_double = !in_double;
      ++i;
      continue;
    }
    if (!in_single && !in_double && ch == '-' && i + 1 < text.size() &&
        text[i + 1] == '-') {
      while (i < text.size() && text[i] != '\n') ++i;
      continue;
    }
    if (!in_single && !in_double && ch == '/' && i + 1 < text.size() &&
        text[i + 1] == '*') {
      i += 2;
      while (i + 1 < text.size() && !(text[i] == '*' && text[i + 1] == '/')) {
        ++i;
      }
      if (i + 1 < text.size()) i += 2;
      continue;
    }
    out.push_back(ch);
    ++i;
  }
  return out;
}

std::vector<std::string> SplitStatements(std::string_view text) {
  std::vector<std::string> statements;
  std::string current;
  std::string terminator = ";";
  bool in_single = false;
  bool in_double = false;
  auto q_quote_close = [](char open) {
    switch (open) {
      case '{': return '}';
      case '[': return ']';
      case '(': return ')';
      case '<': return '>';
      default: return open;
    }
  };

  auto update_terminator = [](std::string_view statement,
                              std::string& current_terminator) {
    const auto normalized =
        scratchbird::parser::firebird::ToUpperAscii(
            scratchbird::parser::firebird::NormalizeWhitespace(statement));
    constexpr std::string_view prefix = "SET TERM ";
    if (normalized.rfind(prefix, 0) != 0) return;

    auto value = scratchbird::parser::firebird::TrimAscii(
        std::string_view(statement).substr(prefix.size()));
    if (value.empty()) return;
    const auto space = value.find_first_of(" \t\r\n");
    if (space != std::string::npos) value.resize(space);
    if (!value.empty()) current_terminator = value;
  };

  auto flush_statement = [&]() {
    auto statement = scratchbird::parser::firebird::TrimAscii(current);
    while (!statement.empty() && statement.front() == ';') {
      statement = scratchbird::parser::firebird::TrimAscii(
          std::string_view(statement).substr(1));
    }
    if (!statement.empty()) {
      statements.push_back(statement);
      update_terminator(statement, terminator);
    }
    current.clear();
  };

  for (std::size_t i = 0; i < text.size();) {
    const char ch = text[i];
    if (!in_single && !in_double && (ch == 'q' || ch == 'Q') &&
        i + 2 < text.size() && text[i + 1] == '\'' && text[i + 2] != '\'') {
      const char delimiter = q_quote_close(text[i + 2]);
      current.push_back(text[i++]);
      current.push_back(text[i++]);
      current.push_back(text[i++]);
      while (i < text.size()) {
        current.push_back(text[i]);
        if (text[i] == delimiter && i + 1 < text.size() && text[i + 1] == '\'') {
          current.push_back(text[++i]);
          ++i;
          break;
        }
        ++i;
      }
      continue;
    }
    if (!in_double && ch == '\'') {
      current.push_back(ch);
      if (in_single && i + 1 < text.size() && text[i + 1] == '\'') {
        current.push_back(text[++i]);
        ++i;
        continue;
      }
      in_single = !in_single;
      ++i;
      continue;
    }
    if (!in_single && ch == '"') {
      current.push_back(ch);
      if (in_double && i + 1 < text.size() && text[i + 1] == '"') {
        current.push_back(text[++i]);
        ++i;
        continue;
      }
      in_double = !in_double;
      ++i;
      continue;
    }
    if (!in_single && !in_double && !terminator.empty() &&
        i + terminator.size() <= text.size() &&
        text.substr(i, terminator.size()) == terminator) {
      flush_statement();
      i += terminator.size();
      continue;
    }
    current.push_back(ch);
    ++i;
  }
  if (!scratchbird::parser::firebird::TrimAscii(current).empty()) {
    flush_statement();
  }
  return statements;
}

bool ProbeScript(const std::filesystem::path& path, std::size_t& total_statements) {
  const auto script = StripComments(ReadFile(path));
  const auto statements = SplitStatements(script);
  std::size_t accepted = 0;
  for (const auto& raw_statement : statements) {
    const auto statement = scratchbird::parser::firebird::TrimAscii(raw_statement);
    if (statement.empty()) continue;
    const auto parsed = scratchbird::parser::firebird::ParseStatement(statement);
    if (!parsed.ok) {
      if (parsed.message_vector_json.find("FIREBIRD.CATALOG_OVERLAY.READ_ONLY") !=
          std::string::npos) {
        ++accepted;
        continue;
      }
      std::cerr << "Firebird parser rejected original isql statement from "
                << path << ": " << statement << "\n"
                << parsed.message_vector_json << '\n';
      return false;
    }
    ++accepted;
  }
  if (accepted == 0) {
    std::cout << path << ",accepted_statements=0,no_replayable_sql=true\n";
    return true;
  }
  total_statements += accepted;
  std::cout << path << ",accepted_statements=" << accepted << '\n';
  return true;
}

} // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::cerr << "usage: firebird_isql_original_regression_probe <script.sql>...\n";
    return EXIT_FAILURE;
  }

  std::size_t total_statements = 0;
  for (int i = 1; i < argc; ++i) {
    const std::filesystem::path path = argv[i];
    if (!std::filesystem::is_regular_file(path)) {
      std::cerr << "missing isql replay script: " << path << '\n';
      return EXIT_FAILURE;
    }
    if (!ProbeScript(path, total_statements)) return EXIT_FAILURE;
  }
  std::cout << "total_accepted_statements=" << total_statements << '\n';
  return EXIT_SUCCESS;
}

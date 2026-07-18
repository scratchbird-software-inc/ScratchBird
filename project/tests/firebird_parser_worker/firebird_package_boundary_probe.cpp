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
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace {

bool Contains(std::string_view haystack, std::string_view needle) {
  return haystack.find(needle) != std::string_view::npos;
}

std::string LowerAscii(std::string value) {
  for (char& byte : value) {
    if (byte >= 'A' && byte <= 'Z') {
      byte = static_cast<char>(byte - 'A' + 'a');
    }
  }
  return value;
}

bool FileContainsForbiddenToken(const std::filesystem::path& path,
                                const std::vector<std::string>& forbidden) {
  std::ifstream in(path);
  if (!in) {
    std::cerr << "cannot read " << path << '\n';
    return true;
  }
  const std::string content = LowerAscii(
      std::string((std::istreambuf_iterator<char>(in)),
                  std::istreambuf_iterator<char>()));
  for (const auto& token : forbidden) {
    if (Contains(content, token)) {
      std::cerr << "forbidden token " << token << " in " << path << '\n';
      return true;
    }
  }
  return false;
}

std::vector<std::string> OtherParserTokens(
    const std::filesystem::path& project_root) {
  std::set<std::string> tokens;
  const auto compatibility_root =
      project_root / "src/parsers/compatibility";
  for (const auto& entry :
       std::filesystem::directory_iterator(compatibility_root)) {
    if (!entry.is_directory()) continue;
    const auto family = LowerAscii(entry.path().filename().string());
    if (family == "firebird" || family == "common") continue;
    tokens.insert("compatibility/" + family);
    tokens.insert("compatibility\\" + family);
    tokens.insert("parser::" + family);
    tokens.insert("sbl_" + family);
    tokens.insert("sbp_" + family);
    tokens.insert("sbu_" + family);
    tokens.insert(family + "_dialect");
    tokens.insert(family + "_worker");
  }

  // Top-level parser implementations are also independent families.  Shared
  // parser-neutral contracts remain legal, but their SBSql-specific branches
  // do not.
  tokens.insert("sbsql");
  tokens.insert("parser::native");
  tokens.insert("sbl_native");
  tokens.insert("sbp_native");
  tokens.insert("native_minimal_parser");
  tokens.insert("native/v3");
  tokens.insert("native\\v3");
  tokens.insert("src/parsers/native");
  // The legacy compatibility/common archive owns a shared SQL lexer/parser,
  // semantic normalization, and text-worker command intake.  It is not a
  // parser-neutral dependency and is forbidden in strict Firebird closure.
  tokens.insert("sbl_compatibility_parser_common");
  tokens.insert("libsbl_compatibility_parser_common");
  tokens.insert("sbl_parser_neutral_evidence");
  tokens.insert("libsbl_parser_neutral_evidence");
  tokens.insert("src/parsers/compatibility/common");
  tokens.insert("compatibility_dialect.hpp");
  tokens.insert("compatibility_dialect.cpp");
  tokens.insert("compatibility_worker_session.hpp");
  tokens.insert("compatibility_worker_session.cpp");
  tokens.insert("parser::compatibility::parsestatement");
  tokens.insert("parser::compatibility::lextokens");
  tokens.insert("parser::compatibility::handleworkercommand");
  tokens.insert("parser::compatibility::servetextworkersession");
  return {tokens.begin(), tokens.end()};
}

} // namespace

int main(int argc, char** argv) {
  if (argc != 2) {
    std::cerr << "usage: firebird_package_boundary_probe <project-root>\n";
    return EXIT_FAILURE;
  }

  const std::filesystem::path project_root = argv[1];
  const std::vector<std::filesystem::path> roots{
      project_root / "src/parsers/compatibility/firebird",
      project_root / "src/udr/sbu_firebird_parser_support"};
  const auto forbidden = OtherParserTokens(project_root);

  for (const auto& root : roots) {
    if (!std::filesystem::exists(root)) {
      std::cerr << "missing Firebird package root: " << root << '\n';
      return EXIT_FAILURE;
    }
    for (const auto& entry : std::filesystem::recursive_directory_iterator(root)) {
      if (!entry.is_regular_file()) continue;
      const auto ext = entry.path().extension().string();
      if (ext != ".cpp" && ext != ".hpp" && ext != ".txt") continue;
      if (FileContainsForbiddenToken(entry.path(), forbidden)) {
        return EXIT_FAILURE;
      }
    }
  }

  const auto identity = scratchbird::parser::firebird::FirebirdPackageIdentityJson();
  if (!Contains(identity, "\"cross_dialect_dependencies\":false")) {
    std::cerr << "Firebird identity does not declare dependency isolation: "
              << identity << '\n';
    return EXIT_FAILURE;
  }
  if (Contains(identity, "sbl_compatibility_parser_common") ||
      Contains(identity, "sbl_parser_neutral_evidence") ||
      !Contains(identity, "\"target\":\"sbl_firebird_parser_pipeline\"")) {
    std::cerr << "Firebird identity does not declare the strict family-owned "
                 "semantic evidence closure: "
              << identity << '\n';
    return EXIT_FAILURE;
  }

  return EXIT_SUCCESS;
}

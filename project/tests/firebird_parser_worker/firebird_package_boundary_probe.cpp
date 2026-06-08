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

bool Contains(std::string_view haystack, std::string_view needle) {
  return haystack.find(needle) != std::string_view::npos;
}

bool FileContainsForbiddenToken(const std::filesystem::path& path,
                                const std::vector<std::string_view>& forbidden) {
  std::ifstream in(path);
  if (!in) {
    std::cerr << "cannot read " << path << '\n';
    return true;
  }
  const std::string content((std::istreambuf_iterator<char>(in)),
                            std::istreambuf_iterator<char>());
  for (const auto token : forbidden) {
    if (Contains(content, token)) {
      std::cerr << "forbidden token " << token << " in " << path << '\n';
      return true;
    }
  }
  return false;
}

} // namespace

int main(int argc, char** argv) {
  if (argc != 2) {
    std::cerr << "usage: firebird_package_boundary_probe <project-root>\n";
    return EXIT_FAILURE;
  }

  const std::filesystem::path project_root = argv[1];
  const std::vector<std::filesystem::path> roots{
      project_root / "src/parsers/donor/firebird",
      project_root / "src/udr/sbu_firebird_parser_support"};
  const std::vector<std::string_view> forbidden{
      "sbsql_worker", "sbl_sbsql", "sbu_sbsql", "sbp_sbsql", "SBSQL",
      "parser::sbsql", "scratchbird::parser::sbsql"};

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

  return EXIT_SUCCESS;
}

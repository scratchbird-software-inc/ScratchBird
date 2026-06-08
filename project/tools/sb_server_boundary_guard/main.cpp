// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// SEARCH_KEY: SB_SERVER_BOUNDARY_GUARD_MAIN

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

struct Violation {
  std::filesystem::path path;
  std::string pattern;
  std::string line;
};

bool IsSourceFile(const std::filesystem::path& path) {
  const auto extension = path.extension().string();
  return extension == ".cpp" || extension == ".cc" || extension == ".cxx" ||
         extension == ".hpp" || extension == ".hh" || extension == ".h" ||
         path.filename() == "CMakeLists.txt";
}

bool Contains(std::string_view haystack, std::string_view needle) {
  return haystack.find(needle) != std::string_view::npos;
}

std::vector<std::string> ForbiddenPatterns() {
  return {
      "#include \"../parsers/",
      "#include \"parsers/",
      "#include <parsers/",
      "#include \"../../parsers/",
      "#include \"donor/",
      "#include <donor/",
      "#include \"wire/client/",
      "#include <wire/client/",
      "src/engine/internal_api",
      "src/engine/sblr",
      "engine/internal_api",
      "engine/sblr",
      "sb_engine_internal_api",
      "#include \"api_context.hpp\"",
      "#include \"api_request.hpp\"",
      "#include \"api_result.hpp\"",
      "#include \"api_types.hpp\"",
      "#include \"engine_internal_api.hpp\"",
      "#include \"sblr_dispatch.hpp\"",
      "#include \"sblr_engine_envelope.hpp\"",
  };
}

void ScanFile(const std::filesystem::path& path,
              const std::vector<std::string>& patterns,
              std::vector<Violation>* violations) {
  std::ifstream input(path);
  if (!input) {
    violations->push_back({path, "file_unreadable", "unable to read source file"});
    return;
  }
  std::string line;
  while (std::getline(input, line)) {
    for (const auto& pattern : patterns) {
      if (Contains(line, pattern)) {
        violations->push_back({path, pattern, line});
      }
    }
  }
}

}  // namespace

int main(int argc, char** argv) {
  const std::filesystem::path root = argc > 1 ? std::filesystem::path(argv[1])
                                              : std::filesystem::path("project/src/server");
  if (!std::filesystem::exists(root)) {
    std::cerr << "SERVER_BOUNDARY_GUARD.ROOT_MISSING " << root << '\n';
    return 2;
  }

  const auto patterns = ForbiddenPatterns();
  std::vector<Violation> violations;
  for (const auto& entry : std::filesystem::recursive_directory_iterator(root)) {
    if (!entry.is_regular_file() || !IsSourceFile(entry.path())) {
      continue;
    }
    ScanFile(entry.path(), patterns, &violations);
  }

  std::cout << "{\n";
  std::cout << "  \"root\": \"" << root.string() << "\",\n";
  std::cout << "  \"violation_count\": " << violations.size() << "\n";
  std::cout << "}\n";

  for (const auto& violation : violations) {
    std::cerr << "SERVER_BOUNDARY_GUARD.FORBIDDEN_DEPENDENCY path="
              << violation.path.string() << " pattern=" << violation.pattern
              << " line=" << violation.line << '\n';
  }

  return violations.empty() ? 0 : 1;
}

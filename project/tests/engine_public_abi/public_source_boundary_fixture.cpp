// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

bool contains(std::string_view haystack, std::string_view needle) {
  return haystack.find(needle) != std::string_view::npos;
}

bool source_file(const std::filesystem::path& path) {
  const auto ext = path.extension().string();
  return ext == ".cpp" || ext == ".hpp" || ext == ".h" || ext == ".c" || ext == ".cc" || ext == ".cxx" ||
         path.filename() == "CMakeLists.txt";
}

bool file_has_forbidden_text(const std::filesystem::path& path, const std::vector<std::string>& forbidden) {
  std::ifstream in(path);
  std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  for (const auto& needle : forbidden) {
    if (contains(text, needle)) {
      return true;
    }
  }
  return false;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 3) {
    return 1;
  }
  const std::vector<std::string> no_spin = {"spinlock", "SpinLock", "pthread_spin", "atomic_flag"};
  for (int i = 1; i < argc; ++i) {
    const std::filesystem::path root = argv[i];
    for (const auto& entry : std::filesystem::recursive_directory_iterator(root)) {
      if (!entry.is_regular_file() || !source_file(entry.path())) {
        continue;
      }
      if (file_has_forbidden_text(entry.path(), no_spin)) {
        return 2;
      }
    }
  }

  const std::vector<std::string> server_forbidden = {
      "src/engine/internal_api",
      "src/engine/sblr",
      "engine/internal_api",
      "engine/sblr/sblr_dispatch.hpp",
      "sb_engine_internal_api",
      "#include \"api_context.hpp\"",
      "#include \"api_request.hpp\"",
      "#include \"api_result.hpp\"",
      "#include \"api_types.hpp\"",
      "#include \"engine_internal_api.hpp\"",
      "#include \"sblr_dispatch.hpp\"",
      "#include \"sblr_engine_envelope.hpp\"",
  };
  const std::filesystem::path server_root = argv[2];
  for (const auto& entry : std::filesystem::recursive_directory_iterator(server_root)) {
    if (!entry.is_regular_file() || !source_file(entry.path())) {
      continue;
    }
    if (file_has_forbidden_text(entry.path(), server_forbidden)) {
      return 3;
    }
  }
  return 0;
}

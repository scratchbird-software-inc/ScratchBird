// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

int main(int argc, char** argv) {
  if (argc != 2) return EXIT_FAILURE;
  const std::filesystem::path root(argv[1]);
  for (const auto& entry : std::filesystem::recursive_directory_iterator(root)) {
    if (!entry.is_regular_file()) continue;
    const auto ext = entry.path().extension().string();
    if (ext != ".cpp" && ext != ".hpp") continue;
    std::ifstream in(entry.path());
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (text.find("spinlock") != std::string::npos || text.find("busy_wait") != std::string::npos) {
      std::cerr << "forbidden spin/busy wait term in " << entry.path() << "\n";
      return EXIT_FAILURE;
    }
  }
  return EXIT_SUCCESS;
}

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
#include <vector>

namespace fs = std::filesystem;

int main(int argc, char** argv) {
  if (argc != 2) {
    std::cerr << "usage: sb_listener_no_spin_gate <source-dir>\n";
    return EXIT_FAILURE;
  }
  const fs::path root = argv[1];
  const std::vector<std::string> banned = {
      "SpinLock", "spin_lock", "pthread_spin", "_mm_pause", "std::this_thread::yield", "try_lock()"};
  bool ok = true;
  for (const auto& entry : fs::recursive_directory_iterator(root)) {
    if (!entry.is_regular_file()) continue;
    const auto ext = entry.path().extension().string();
    if (ext != ".cpp" && ext != ".hpp") continue;
    std::ifstream input(entry.path());
    std::string text((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    for (const auto& needle : banned) {
      if (text.find(needle) != std::string::npos) {
        std::cerr << "banned busy-wait primitive found: " << needle << " in " << entry.path() << '\n';
        ok = false;
      }
    }
  }
  return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}

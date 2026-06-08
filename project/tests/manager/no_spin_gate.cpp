// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// SEARCH_KEY: SBMI_NODE_MANAGER_NO_SPIN_GATE

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace {

bool ContainsForbidden(const std::string& text, std::string* pattern) {
  static const std::vector<std::string> forbidden = {
      "SpinLock", "spinlock", "atomic_flag", "test_and_set", "_mm_pause",
      "sched_yield", "std::this_thread::yield", "while (;", "for (;;) ;"};
  for (const auto& entry : forbidden) {
    if (text.find(entry) != std::string::npos) {
      *pattern = entry;
      return true;
    }
  }
  return false;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::cerr << "usage: sbmn_manager_no_spin_gate PATH...\n";
    return 2;
  }
  bool ok = true;
  for (int i = 1; i < argc; ++i) {
    const std::filesystem::path root = argv[i];
    if (!std::filesystem::exists(root)) continue;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(root)) {
      if (!entry.is_regular_file()) continue;
      const auto ext = entry.path().extension().string();
      if (ext != ".cpp" && ext != ".hpp" && ext != ".h" && ext != ".cc") continue;
      if (entry.path().filename() == "no_spin_gate.cpp") continue;
      std::ifstream in(entry.path());
      const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
      std::string pattern;
      if (ContainsForbidden(text, &pattern)) {
        std::cerr << "forbidden no-spin pattern " << pattern << " in " << entry.path() << '\n';
        ok = false;
      }
    }
  }
  return ok ? 0 : 1;
}

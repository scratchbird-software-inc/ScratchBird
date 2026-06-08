// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace {

std::string ReadFile(const std::string& path) {
  std::ifstream in(path);
  std::ostringstream out;
  out << in.rdbuf();
  return out.str();
}

bool Contains(std::string_view haystack, std::string_view needle) {
  return haystack.find(needle) != std::string_view::npos;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::cerr << "usage: native_worker_storage_policy_gate <file> [file...]\n";
    return 2;
  }

  const std::vector<std::string> forbidden = {
      "sqlite",
      "SQLite",
      "sqlite3",
      "PRAGMA",
      "journal_mode",
      "WAL",
      "write ahead",
      "write-ahead",
      "TODO",
      "NotImplemented",
      "not implemented",
      "stub",
  };

  bool ok = true;
  for (int i = 1; i < argc; ++i) {
    const std::string text = ReadFile(argv[i]);
    if (text.empty()) {
      std::cerr << "could not read " << argv[i] << '\n';
      ok = false;
      continue;
    }
    for (const auto& token : forbidden) {
      if (Contains(text, token)) {
        std::cerr << argv[i] << " contains forbidden native-worker token: " << token << '\n';
        ok = false;
      }
    }
  }
  return ok ? 0 : 1;
}

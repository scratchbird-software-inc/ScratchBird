// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include <filesystem>
#include <fstream>
#include <iostream>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace {

std::string ReadFile(const std::filesystem::path& path) {
  std::ifstream in(path);
  if (!in) return {};
  return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

void AddRegisteredCodes(const std::filesystem::path& registry,
                        std::set<std::string>* registered) {
  const auto text = ReadFile(registry);
  std::istringstream lines(text);
  std::string line;
  const std::regex code_line(R"(^\s*-\s+code:\s+([A-Z0-9_.]+)\s*$)");
  while (std::getline(lines, line)) {
    std::smatch match;
    if (std::regex_match(line, match, code_line)) {
      registered->insert(match[1].str());
    }
  }
}

void AddSourceCodes(const std::filesystem::path& path,
                    std::set<std::string>* used) {
  const auto text = ReadFile(path);
  const std::regex code_string("\"((?:LISTENER|CONTROL)\\.[A-Z0-9_.]+)\"");
  for (auto it = std::sregex_iterator(text.begin(), text.end(), code_string);
       it != std::sregex_iterator();
       ++it) {
    used->insert((*it)[1].str());
  }
}

void AddSourceTreeCodes(const std::filesystem::path& root,
                        std::set<std::string>* used) {
  if (!std::filesystem::exists(root)) return;
  for (const auto& entry : std::filesystem::recursive_directory_iterator(root)) {
    if (!entry.is_regular_file()) continue;
    const auto ext = entry.path().extension().string();
    if (ext == ".cpp" || ext == ".hpp") {
      AddSourceCodes(entry.path(), used);
    }
  }
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 2) {
    std::cerr << "usage: sb_listener_diagnostic_registry_gate <project-source-dir>\n";
    return 2;
  }
  const std::filesystem::path project = argv[1];
  const auto repo_root = project.parent_path();
  const auto registry =
      repo_root / "docs" / "contracts" / "registries" / "reconciliation-diagnostic-codes.yaml";

  std::set<std::string> registered;
  AddRegisteredCodes(registry, &registered);
  if (registered.empty()) {
    std::cerr << "diagnostic registry not readable or empty: " << registry << '\n';
    return 1;
  }

  std::set<std::string> used;
  AddSourceTreeCodes(project / "src" / "listener", &used);
  AddSourceCodes(project / "src" / "server" / "listener_orchestrator.cpp", &used);
  AddSourceCodes(project / "src" / "manager" / "node" / "manager_listener_control.cpp", &used);

  bool ok = true;
  for (const auto& code : used) {
    if (!registered.contains(code)) {
      std::cerr << "unregistered listener/interface diagnostic code: " << code << '\n';
      ok = false;
    }
  }
  return ok ? 0 : 1;
}

// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include <array>
#include <cstdio>
#include <cstdlib>
#include <set>
#include <string>
#include <vector>

namespace {

std::string shell_quote(const std::string& input) {
  std::string out = "'";
  for (char c : input) {
    if (c == '\'') {
      out += "'\\''";
    } else {
      out += c;
    }
  }
  out += "'";
  return out;
}

std::vector<std::string> command_lines(const std::string& command) {
  std::vector<std::string> lines;
  std::array<char, 4096> buffer{};
  FILE* pipe = popen(command.c_str(), "r");
  if (pipe == nullptr) {
    return lines;
  }
  while (fgets(buffer.data(), static_cast<int>(buffer.size()), pipe) != nullptr) {
    std::string line(buffer.data());
    while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) {
      line.pop_back();
    }
    lines.push_back(std::move(line));
  }
  (void)pclose(pipe);
  return lines;
}

std::string symbol_name_from_nm_line(const std::string& line) {
  auto pos = line.find(" sb_engine_");
  if (pos != std::string::npos) {
    return line.substr(pos + 1);
  }
  pos = line.find(" T ");
  if (pos != std::string::npos) {
    return line.substr(pos + 3);
  }
  pos = line.find(" W ");
  if (pos != std::string::npos) {
    return line.substr(pos + 3);
  }
  return {};
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 2) {
    return 2;
  }
  const std::string library_path = argv[1];
  auto lines = command_lines("nm -D --defined-only " + shell_quote(library_path));
  if (lines.empty()) {
    return 3;
  }

  const std::set<std::string> required = {
      "sb_engine_abi_build_id",
      "sb_engine_abi_version_packed",
      "sb_engine_close",
      "sb_engine_describe_capabilities",
      "sb_engine_dispatch_sblr",
      "sb_engine_metric_root",
      "sb_engine_open",
      "sb_engine_result_class",
      "sb_engine_result_completion",
      "sb_engine_result_diagnostics",
      "sb_engine_result_next_batch",
      "sb_engine_result_payload",
      "sb_engine_result_release",
      "sb_engine_result_summary",
      "sb_engine_session_begin",
      "sb_engine_session_end",
      "sb_engine_status_name",
      "sb_engine_transaction_begin",
      "sb_engine_transaction_commit",
      "sb_engine_transaction_rollback",
  };

  std::set<std::string> seen;
  for (const auto& line : lines) {
    auto name = symbol_name_from_nm_line(line);
    if (name.empty()) {
      continue;
    }
    if (name.rfind("sb_engine_", 0) != 0) {
      return 4;
    }
    seen.insert(name);
  }

  for (const auto& symbol : required) {
    if (seen.find(symbol) == seen.end()) {
      return 5;
    }
  }
  return 0;
}

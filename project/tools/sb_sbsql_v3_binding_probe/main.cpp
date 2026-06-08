// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include <fstream>
#include <iostream>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace {

std::string Trim(std::string_view text) {
  std::size_t first = 0;
  while (first < text.size() && (text[first] == ' ' || text[first] == '\t' || text[first] == '\r' || text[first] == '\n')) {
    ++first;
  }
  std::size_t last = text.size();
  while (last > first && (text[last - 1] == ' ' || text[last - 1] == '\t' || text[last - 1] == '\r' || text[last - 1] == '\n')) {
    --last;
  }
  return std::string(text.substr(first, last - first));
}

std::pair<std::string, std::string> ParseKeyValue(std::string line) {
  line = Trim(line);
  if (line.rfind("- ", 0) == 0) {
    line = Trim(std::string_view(line).substr(2));
  }
  const auto colon = line.find(':');
  if (colon == std::string::npos) {
    return {"", ""};
  }
  auto key = Trim(std::string_view(line).substr(0, colon));
  auto value = Trim(std::string_view(line).substr(colon + 1));
  if (value.size() >= 2 && ((value.front() == '"' && value.back() == '"') || (value.front() == '\'' && value.back() == '\''))) {
    value = value.substr(1, value.size() - 2);
  }
  return {key, value};
}

bool BoolValue(const std::map<std::string, std::string>& row, std::string_view key) {
  const auto found = row.find(std::string(key));
  return found != row.end() && found->second == "true";
}

void FinalizeRow(const std::map<std::string, std::string>& row,
                 std::size_t* row_count,
                 std::size_t* cluster_rows,
                 std::vector<std::string>* errors) {
  if (row.empty()) {
    return;
  }
  ++*row_count;
  const auto surface = row.contains("surface_key") ? row.at("surface_key") : std::string("<missing-surface>");
  const char* required_keys[] = {
      "surface_key",
      "command_family",
      "bound_ast_node",
      "required_right",
      "scope_mode",
      "catalog_authority",
      "descriptor_binding_profile",
      "name_resolution",
      "security_binding",
      "transaction_context_binding",
      "cluster_authority_required",
      "fail_closed_without_cluster_authority",
      "sblr_lowering_allowed_after_binding",
  };
  for (const char* key : required_keys) {
    if (!row.contains(key) || row.at(key).empty()) {
      errors->push_back(surface + ": missing " + key);
    }
  }
  const auto require_equals = [&](std::string_view key, std::string_view expected) {
    const auto found = row.find(std::string(key));
    if (found == row.end() || found->second != expected) {
      errors->push_back(surface + ": " + std::string(key) + " must be " + std::string(expected));
    }
  };
  require_equals("name_resolution", "uuid_required_before_engine");
  require_equals("security_binding", "engine_rechecked");
  require_equals("transaction_context_binding", "engine_rechecked");
  require_equals("sblr_lowering_allowed_after_binding", "true");
  if (BoolValue(row, "cluster_authority_required")) {
    ++*cluster_rows;
    if (!BoolValue(row, "fail_closed_without_cluster_authority")) {
      errors->push_back(surface + ": cluster authority rows must fail closed without cluster authority");
    }
  }
}

std::string JsonEscape(std::string_view text) {
  std::string out;
  for (char ch : text) {
    switch (ch) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      default: out += ch; break;
    }
  }
  return out;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 2) {
    std::cerr << "usage: sb_sbsql_v3_binding_probe <SBSQL_V3_BINDING_MATRIX.yaml>\n";
    return 2;
  }

  std::ifstream input(argv[1]);
  if (!input) {
    std::cerr << "failed to open binding matrix: " << argv[1] << "\n";
    return 2;
  }

  std::map<std::string, std::string> row;
  std::size_t row_count = 0;
  std::size_t cluster_rows = 0;
  std::size_t declared_row_count = 0;
  bool in_commands = false;
  std::vector<std::string> errors;
  std::string line;
  while (std::getline(input, line)) {
    const auto trimmed = Trim(line);
    if (trimmed.empty() || trimmed.rfind("#", 0) == 0) {
      continue;
    }
    if (!in_commands) {
      const auto [key, value] = ParseKeyValue(trimmed);
      if (key == "row_count" && !value.empty()) {
        declared_row_count = static_cast<std::size_t>(std::stoul(value));
      }
      if (trimmed == "commands:") {
        in_commands = true;
      }
      continue;
    }
    if (trimmed.rfind("- ", 0) == 0) {
      FinalizeRow(row, &row_count, &cluster_rows, &errors);
      row.clear();
    }
    const auto [key, value] = ParseKeyValue(trimmed);
    if (!key.empty()) {
      row[key] = value;
    }
  }
  FinalizeRow(row, &row_count, &cluster_rows, &errors);

  if (row_count == 0) {
    errors.push_back("binding matrix has no command rows");
  }
  if (declared_row_count != 0 && declared_row_count != row_count) {
    errors.push_back("declared row_count does not match parsed command rows");
  }
  if (cluster_rows == 0) {
    errors.push_back("binding matrix has no cluster fail-closed rows");
  }

  std::cout << "{\n";
  std::cout << "  \"ok\": " << (errors.empty() ? "true" : "false") << ",\n";
  std::cout << "  \"declared_row_count\": " << declared_row_count << ",\n";
  std::cout << "  \"row_count\": " << row_count << ",\n";
  std::cout << "  \"cluster_rows\": " << cluster_rows << ",\n";
  std::cout << "  \"errors\": [";
  for (std::size_t i = 0; i < errors.size(); ++i) {
    if (i != 0) {
      std::cout << ", ";
    }
    std::cout << '"' << JsonEscape(errors[i]) << '"';
  }
  std::cout << "]\n}\n";

  return errors.empty() ? 0 : 1;
}

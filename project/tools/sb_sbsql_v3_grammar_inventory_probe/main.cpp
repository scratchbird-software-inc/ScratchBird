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
#include <set>
#include <string>
#include <vector>

namespace {

struct Row { std::string surface_key; std::string command_family; std::string support_status; std::string grammar_status; std::string parser_entry; std::string grammar_rule; std::string cluster_behavior; };

std::string Trim(std::string value) {
  const auto first = value.find_first_not_of(" \t\r\n");
  if (first == std::string::npos) return {};
  const auto last = value.find_last_not_of(" \t\r\n");
  value = value.substr(first, last - first + 1);
  if (value.size() >= 2 && ((value.front() == '"' && value.back() == '"') || (value.front() == '\'' && value.back() == '\''))) value = value.substr(1, value.size() - 2);
  if (value == "''") return {};
  return value;
}

bool Starts(const std::string& value, const std::string& prefix) { return value.rfind(prefix, 0) == 0; }
std::string Field(const std::string& line, const std::string& key) { return Trim(line.substr(line.find(key + ":") + key.size() + 1)); }

std::map<std::string, Row> Parse(const std::string& path, std::vector<std::string>* errors) {
  std::ifstream in(path);
  if (!in) { errors->push_back("file_not_readable:" + path); return {}; }
  std::map<std::string, Row> rows;
  Row cur; bool active = false;
  std::string line;
  while (std::getline(in, line)) {
    if (Starts(line, "  - surface_key:")) {
      if (active) rows[cur.surface_key] = cur;
      cur = Row{}; cur.surface_key = Field(line, "surface_key"); active = true; continue;
    }
    if (!active) continue;
    if (Starts(line, "    command_family:")) cur.command_family = Field(line, "command_family");
    else if (Starts(line, "    support_status:")) cur.support_status = Field(line, "support_status");
    else if (Starts(line, "    grammar_status:")) cur.grammar_status = Field(line, "grammar_status");
    else if (Starts(line, "    parser_entry:")) cur.parser_entry = Field(line, "parser_entry");
    else if (Starts(line, "    grammar_rule:")) cur.grammar_rule = Field(line, "grammar_rule");
    else if (Starts(line, "    cluster_behavior:")) cur.cluster_behavior = Field(line, "cluster_behavior");
  }
  if (active) rows[cur.surface_key] = cur;
  return rows;
}

}  // namespace

int main(int argc, char** argv) {
  const std::string matrix_path = argc > 1 ? argv[1] : "docs" "/execution-plans/sbsql-native-v3-full-dialect-support/SBSQL_V3_COMMAND_SUPPORT_MATRIX.yaml";
  const std::string grammar_path = argc > 2 ? argv[2] : "docs" "/execution-plans/sbsql-native-v3-full-dialect-support/SBSQL_V3_GRAMMAR_INVENTORY.yaml";
  std::vector<std::string> errors;
  const auto matrix = Parse(matrix_path, &errors);
  const auto grammar = Parse(grammar_path, &errors);
  if (matrix.empty()) errors.push_back("matrix_empty");
  if (grammar.empty()) errors.push_back("grammar_empty");
  for (const auto& [key, m] : matrix) {
    auto it = grammar.find(key);
    if (it == grammar.end()) { errors.push_back("grammar_row_missing:" + key); continue; }
    const Row& g = it->second;
    if (g.command_family != m.command_family) errors.push_back("command_family_mismatch:" + key);
    if (g.support_status != m.support_status) errors.push_back("support_status_mismatch:" + key);
    if (g.grammar_status != "specified_pending_parser_implementation") errors.push_back("grammar_status_invalid:" + key);
    if (g.parser_entry.empty()) errors.push_back("parser_entry_missing:" + key);
    if (g.grammar_rule.empty()) errors.push_back("grammar_rule_missing:" + key);
    if (m.support_status == "cluster_deferred" && g.cluster_behavior != "fail_closed_cluster_placeholder") errors.push_back("cluster_behavior_invalid:" + key);
  }
  for (const auto& [key, g] : grammar) if (!matrix.contains(key)) errors.push_back("extra_grammar_row:" + key);
  std::cout << "{\n  \"ok\": " << (errors.empty() ? "true" : "false") << ",\n  \"matrix_rows\": " << matrix.size() << ",\n  \"grammar_rows\": " << grammar.size() << ",\n  \"errors\": [";
  for (std::size_t i = 0; i < errors.size(); ++i) { if (i) std::cout << ", "; std::cout << "\"" << errors[i] << "\""; }
  std::cout << "]\n}\n";
  return errors.empty() ? 0 : 1;
}

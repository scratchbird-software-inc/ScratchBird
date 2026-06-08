// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "sbsql_v3_sblr_catalog.hpp"

#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace sblr = scratchbird::parser::sbsql_v3_sblr;

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

void FinalizeRow(const std::map<std::string, std::string>& row,
                 std::set<std::string>* surfaces,
                 std::set<std::string>* operations,
                 std::set<std::uint32_t>* opcodes,
                 std::size_t* rows,
                 std::size_t* cluster_rows,
                 std::size_t* missing_api_rows,
                 std::vector<std::string>* errors) {
  if (row.empty()) {
    return;
  }
  ++*rows;
  const auto surface = row.contains("surface_key") ? row.at("surface_key") : std::string("<missing-surface>");
  const char* required[] = {"surface_key", "sblr_operation", "opcode_value", "payload_class", "raw_sql_payload_allowed", "engine_input_contract"};
  for (const char* key : required) {
    if (!row.contains(key) || row.at(key).empty()) {
      errors->push_back(surface + ": missing " + key);
    }
  }
  if (!surfaces->insert(surface).second) {
    errors->push_back(surface + ": duplicate surface key");
  }
  const auto op = row.contains("sblr_operation") ? row.at("sblr_operation") : std::string();
  if (!operations->insert(op).second) {
    errors->push_back(surface + ": duplicate SBLR operation");
  }
  const auto opcode = row.contains("opcode_value") ? sblr::ParseOpcodeValue(row.at("opcode_value")) : std::nullopt;
  if (!opcode) {
    errors->push_back(surface + ": invalid opcode value");
  } else if (!opcodes->insert(*opcode).second) {
    errors->push_back(surface + ": duplicate opcode value");
  }
  sblr::SblrOpcodeEntry entry;
  entry.sblr_operation = op;
  entry.opcode_value = opcode.value_or(0);
  entry.payload_class = row.contains("payload_class") ? row.at("payload_class") : std::string();
  entry.api_operation_id = row.contains("api_operation_id") ? row.at("api_operation_id") : std::string();
  entry.cluster_authority_required = BoolValue(row, "cluster_authority_required");
  entry.fail_closed_without_cluster_authority = BoolValue(row, "fail_closed_without_cluster_authority");
  entry.raw_sql_payload_allowed = BoolValue(row, "raw_sql_payload_allowed");
  sblr::ValidateOpcodeEntry(entry, errors);
  if (entry.cluster_authority_required) {
    ++*cluster_rows;
  }
  if (row.contains("api_mapping_status") && row.at("api_mapping_status") == "stage_5_api_mapping_required") {
    ++*missing_api_rows;
  }
  if (row.contains("engine_input_contract") && row.at("engine_input_contract") != "sblr_only_no_sql_text") {
    errors->push_back(surface + ": engine input contract must be sblr_only_no_sql_text");
  }
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 2) {
    std::cerr << "usage: sb_sblr_opcode_registry_probe <SBSQL_V3_SBLR_OPCODE_REGISTRY.yaml>\n";
    return 2;
  }
  std::ifstream input(argv[1]);
  if (!input) {
    std::cerr << "failed to open opcode registry: " << argv[1] << "\n";
    return 2;
  }
  std::size_t declared_row_count = 0;
  std::size_t rows = 0;
  std::size_t cluster_rows = 0;
  std::size_t missing_api_rows = 0;
  bool in_commands = false;
  std::map<std::string, std::string> row;
  std::set<std::string> surfaces;
  std::set<std::string> operations;
  std::set<std::uint32_t> opcodes;
  std::vector<std::string> errors;
  std::string line;
  while (std::getline(input, line)) {
    const auto trimmed = Trim(line);
    if (trimmed.empty() || trimmed.rfind("#", 0) == 0) {
      continue;
    }
    if (!in_commands) {
      const auto [key, value] = ParseKeyValue(trimmed);
      if (key == "row_count") {
        declared_row_count = static_cast<std::size_t>(std::stoul(value));
      }
      if (trimmed == "commands:") {
        in_commands = true;
      }
      continue;
    }
    if (trimmed.rfind("- ", 0) == 0) {
      FinalizeRow(row, &surfaces, &operations, &opcodes, &rows, &cluster_rows, &missing_api_rows, &errors);
      row.clear();
    }
    const auto [key, value] = ParseKeyValue(trimmed);
    if (!key.empty()) {
      row[key] = value;
    }
  }
  FinalizeRow(row, &surfaces, &operations, &opcodes, &rows, &cluster_rows, &missing_api_rows, &errors);
  if (declared_row_count != rows) {
    errors.push_back("declared row_count does not match parsed rows");
  }
  if (rows == 0) {
    errors.push_back("opcode registry has no rows");
  }
  if (cluster_rows == 0) {
    errors.push_back("opcode registry has no cluster fail-closed rows");
  }
  std::cout << "{\n";
  std::cout << "  \"ok\": " << (errors.empty() ? "true" : "false") << ",\n";
  std::cout << "  \"declared_row_count\": " << declared_row_count << ",\n";
  std::cout << "  \"row_count\": " << rows << ",\n";
  std::cout << "  \"cluster_rows\": " << cluster_rows << ",\n";
  std::cout << "  \"stage_5_api_mapping_required_rows\": " << missing_api_rows << ",\n";
  std::cout << "  \"errors\": [";
  for (std::size_t i = 0; i < errors.size(); ++i) {
    if (i != 0) std::cout << ", ";
    std::cout << '"' << JsonEscape(errors[i]) << '"';
  }
  std::cout << "]\n}\n";
  return errors.empty() ? 0 : 1;
}

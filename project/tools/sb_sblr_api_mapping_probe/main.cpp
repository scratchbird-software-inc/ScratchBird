// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "sbsql_v3_api_mapping_catalog.hpp"

#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace mapping = scratchbird::parser::sbsql_v3_api_mapping;

namespace {

std::string Trim(std::string_view text) {
  std::size_t first = 0;
  while (first < text.size() && (text[first] == ' ' || text[first] == '\t' || text[first] == '\r' || text[first] == '\n')) ++first;
  std::size_t last = text.size();
  while (last > first && (text[last - 1] == ' ' || text[last - 1] == '\t' || text[last - 1] == '\r' || text[last - 1] == '\n')) --last;
  return std::string(text.substr(first, last - first));
}

std::pair<std::string, std::string> ParseKeyValue(std::string line) {
  line = Trim(line);
  if (line.rfind("- ", 0) == 0) line = Trim(std::string_view(line).substr(2));
  const auto colon = line.find(':');
  if (colon == std::string::npos) return {"", ""};
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
                 std::set<std::string>* sblr_ops,
                 std::size_t* rows,
                 std::size_t* existing_rows,
                 std::size_t* generated_rows,
                 std::size_t* cluster_rows,
                 std::vector<std::string>* errors) {
  if (row.empty()) return;
  ++*rows;
  const auto surface = row.contains("surface_key") ? row.at("surface_key") : std::string("<missing-surface>");
  const char* required[] = {"surface_key", "sblr_operation", "api_operation_id", "api_function_name", "request_type", "result_type", "mapping_status", "typed_request_mapping", "raw_sql_fallback_allowed"};
  for (const char* key : required) {
    if (!row.contains(key) || row.at(key).empty()) errors->push_back(surface + ": missing " + key);
  }
  if (!surfaces->insert(surface).second) errors->push_back(surface + ": duplicate surface key");
  const auto op = row.contains("sblr_operation") ? row.at("sblr_operation") : std::string();
  if (!sblr_ops->insert(op).second) errors->push_back(surface + ": duplicate SBLR operation");
  mapping::ApiMappingEntry entry;
  entry.sblr_operation = op;
  entry.api_operation_id = row.contains("api_operation_id") ? row.at("api_operation_id") : std::string();
  entry.api_function_name = row.contains("api_function_name") ? row.at("api_function_name") : std::string();
  entry.request_type = row.contains("request_type") ? row.at("request_type") : std::string();
  entry.result_type = row.contains("result_type") ? row.at("result_type") : std::string();
  entry.payload_class = row.contains("payload_class") ? row.at("payload_class") : std::string();
  entry.mapping_status = row.contains("mapping_status") ? row.at("mapping_status") : std::string();
  entry.typed_request_mapping = BoolValue(row, "typed_request_mapping");
  entry.raw_sql_fallback_allowed = BoolValue(row, "raw_sql_fallback_allowed");
  entry.cluster_authority_required = BoolValue(row, "cluster_authority_required");
  entry.fail_closed_without_cluster_authority = BoolValue(row, "fail_closed_without_cluster_authority");
  mapping::ValidateApiMappingEntry(entry, errors);
  if (entry.mapping_status == "existing_engine_api_contract") ++*existing_rows;
  else if (entry.mapping_status == "stage_5_contract_defined_behavior_pending") ++*generated_rows;
  else errors->push_back(surface + ": unexpected mapping_status " + entry.mapping_status);
  if (entry.cluster_authority_required) ++*cluster_rows;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 2) {
    std::cerr << "usage: sb_sblr_api_mapping_probe <SBSQL_V3_API_MAPPING_MATRIX.yaml>\n";
    return 2;
  }
  std::ifstream input(argv[1]);
  if (!input) {
    std::cerr << "failed to open API mapping matrix: " << argv[1] << "\n";
    return 2;
  }
  bool in_operations = false;
  std::map<std::string, std::string> row;
  std::set<std::string> surfaces;
  std::set<std::string> sblr_ops;
  std::vector<std::string> errors;
  std::size_t declared_rows = 0, rows = 0, existing_rows = 0, generated_rows = 0, cluster_rows = 0;
  std::string line;
  while (std::getline(input, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    const auto trimmed = Trim(line);
    if (trimmed.empty() || trimmed.rfind("#", 0) == 0) continue;
    if (!in_operations) {
      const auto [key, value] = ParseKeyValue(trimmed);
      if (key == "row_count") declared_rows = static_cast<std::size_t>(std::stoul(value));
      if (trimmed == "operations:") in_operations = true;
      continue;
    }
    if (line.rfind("  - ", 0) == 0) {
      FinalizeRow(row, &surfaces, &sblr_ops, &rows, &existing_rows, &generated_rows, &cluster_rows, &errors);
      row.clear();
    }
    if ((line.rfind("    ", 0) == 0 && line.rfind("      ", 0) != 0) || line.rfind("  - ", 0) == 0) {
      const auto [key, value] = ParseKeyValue(trimmed);
      if (!key.empty()) row[key] = value;
    }
  }
  FinalizeRow(row, &surfaces, &sblr_ops, &rows, &existing_rows, &generated_rows, &cluster_rows, &errors);
  if (declared_rows != rows) errors.push_back("declared row_count does not match parsed rows");
  if (rows == 0) errors.push_back("API mapping matrix has no rows");
  if (existing_rows == 0 || generated_rows == 0) errors.push_back("API mapping matrix must include existing and generated contracts");
  if (cluster_rows == 0) errors.push_back("API mapping matrix must include cluster fail-closed rows");
  std::cout << "{\n";
  std::cout << "  \"ok\": " << (errors.empty() ? "true" : "false") << ",\n";
  std::cout << "  \"declared_row_count\": " << declared_rows << ",\n";
  std::cout << "  \"row_count\": " << rows << ",\n";
  std::cout << "  \"existing_engine_api_contract_rows\": " << existing_rows << ",\n";
  std::cout << "  \"generated_pending_contract_rows\": " << generated_rows << ",\n";
  std::cout << "  \"cluster_rows\": " << cluster_rows << ",\n";
  std::cout << "  \"errors\": [";
  for (std::size_t i = 0; i < errors.size(); ++i) {
    if (i != 0) std::cout << ", ";
    std::cout << '"' << JsonEscape(errors[i]) << '"';
  }
  std::cout << "]\n}\n";
  return errors.empty() ? 0 : 1;
}

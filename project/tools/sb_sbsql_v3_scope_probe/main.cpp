// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace {

struct CommandRow {
  std::string surface_key;
  std::string command_family;
  std::string support_status;
  std::string edition_scope;
  std::string primary_engine_api_operation_id;
  std::vector<std::string> gaps;
};

std::string Trim(std::string value) {
  const auto first = value.find_first_not_of(" \t\r\n");
  if (first == std::string::npos) return {};
  const auto last = value.find_last_not_of(" \t\r\n");
  value = value.substr(first, last - first + 1);
  if (value.size() >= 2 && ((value.front() == '"' && value.back() == '"') ||
                            (value.front() == '\'' && value.back() == '\''))) {
    value = value.substr(1, value.size() - 2);
  }
  return value;
}

bool StartsWith(const std::string& value, const std::string& prefix) {
  return value.rfind(prefix, 0) == 0;
}

std::string FieldValue(const std::string& line, const std::string& field) {
  const std::string marker = field + ":";
  const auto pos = line.find(marker);
  if (pos == std::string::npos) return {};
  return Trim(line.substr(pos + marker.size()));
}

std::vector<CommandRow> ParseRows(const std::string& path, std::vector<std::string>* errors) {
  std::ifstream in(path);
  if (!in) {
    errors->push_back("matrix_file_not_readable:" + path);
    return {};
  }

  std::vector<CommandRow> rows;
  CommandRow current;
  bool in_row = false;
  bool in_gaps = false;
  std::string line;
  while (std::getline(in, line)) {
    if (StartsWith(line, "  - surface_key:")) {
      if (in_row) rows.push_back(current);
      current = CommandRow{};
      current.surface_key = FieldValue(line, "surface_key");
      in_row = true;
      in_gaps = false;
      continue;
    }
    if (!in_row) continue;
    if (StartsWith(line, "    command_family:")) current.command_family = FieldValue(line, "command_family");
    else if (StartsWith(line, "    support_status:")) current.support_status = FieldValue(line, "support_status");
    else if (StartsWith(line, "    edition_scope:")) current.edition_scope = FieldValue(line, "edition_scope");
    else if (StartsWith(line, "    primary_engine_api_operation_id:")) current.primary_engine_api_operation_id = FieldValue(line, "primary_engine_api_operation_id");
    else if (StartsWith(line, "    gaps:")) in_gaps = true;
    else if (in_gaps && StartsWith(line, "      - ")) current.gaps.push_back(Trim(line.substr(8)));
    else if (!StartsWith(line, "      - ")) in_gaps = false;
  }
  if (in_row) rows.push_back(current);
  return rows;
}

bool IsAllowedStatus(const std::string& status) {
  static const std::set<std::string> allowed = {
      "supported_now",
      "implement_in_this_execution_plan",
      "cluster_deferred",
      "enterprise_deferred",
      "unsupported_by_design",
      "donor_only",
  };
  return allowed.contains(status);
}

}  // namespace

int main(int argc, char** argv) {
  const std::string path = argc > 1 ? argv[1] :
      "docs" "/execution-plans/sbsql-native-v3-full-dialect-support/SBSQL_V3_COMMAND_SUPPORT_MATRIX.yaml";
  std::vector<std::string> errors;
  const auto rows = ParseRows(path, &errors);

  std::set<std::string> seen;
  std::map<std::string, int> status_counts;
  std::map<std::string, int> family_counts;
  int cluster_rows = 0;
  int rows_with_gaps = 0;

  if (rows.empty()) errors.push_back("no_command_rows");

  for (const auto& row : rows) {
    if (row.surface_key.empty()) errors.push_back("surface_key_missing");
    if (!row.surface_key.empty() && !seen.insert(row.surface_key).second) {
      errors.push_back("duplicate_surface_key:" + row.surface_key);
    }
    if (row.command_family.empty()) errors.push_back("command_family_missing:" + row.surface_key);
    if (row.support_status.empty()) errors.push_back("support_status_missing:" + row.surface_key);
    if (!row.support_status.empty() && !IsAllowedStatus(row.support_status)) {
      errors.push_back("support_status_invalid:" + row.surface_key + ":" + row.support_status);
    }
    if (row.edition_scope.empty()) errors.push_back("edition_scope_missing:" + row.surface_key);
    if (row.support_status == "cluster_deferred") {
      ++cluster_rows;
      if (row.command_family != "sbsql.private_cluster" && row.edition_scope != "private_cluster") {
        errors.push_back("cluster_deferred_without_private_scope:" + row.surface_key);
      }
    }
    if (row.support_status == "supported_now" && !row.gaps.empty()) {
      errors.push_back("supported_now_has_open_gaps:" + row.surface_key);
    }
    if (!row.gaps.empty()) ++rows_with_gaps;
    status_counts[row.support_status] += 1;
    family_counts[row.command_family] += 1;
  }

  std::ostringstream out;
  out << "{\n";
  out << "  \"ok\": " << (errors.empty() ? "true" : "false") << ",\n";
  out << "  \"row_count\": " << rows.size() << ",\n";
  out << "  \"cluster_deferred_rows\": " << cluster_rows << ",\n";
  out << "  \"rows_with_gaps\": " << rows_with_gaps << ",\n";
  out << "  \"status_counts\": {";
  bool first = true;
  for (const auto& [status, count] : status_counts) {
    if (!first) out << ", ";
    first = false;
    out << "\"" << status << "\": " << count;
  }
  out << "},\n";
  out << "  \"family_count\": " << family_counts.size() << ",\n";
  out << "  \"errors\": [";
  for (std::size_t i = 0; i < errors.size(); ++i) {
    if (i) out << ", ";
    out << "\"" << errors[i] << "\"";
  }
  out << "]\n";
  out << "}\n";
  std::cout << out.str();
  return errors.empty() ? 0 : 1;
}

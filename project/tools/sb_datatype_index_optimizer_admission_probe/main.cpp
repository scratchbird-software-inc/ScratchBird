// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "catalog/datatype_index_optimizer_admission_api.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

using namespace scratchbird::engine::internal_api;

namespace {

struct Args {
  std::filesystem::path matrix_path =
      "docs" "/completed-execution-plans/datatypes-and-domains-final-coverage-closure/CANONICAL_DATATYPE_CLOSURE_MATRIX.csv";
};

struct CanonicalRow {
  std::string type_group;
  std::string canonical_type_ids;
  std::string support_path;
  std::string descriptor_status;
  std::string storage_status;
  std::string operation_status;
  std::string wire_driver_status;
  std::string index_stats_status;
  std::string backup_replication_status;
  std::string status;
  std::string notes;
};

bool ParseArgs(int argc, char** argv, Args* args) {
  for (int i = 1; i < argc; ++i) {
    const std::string key = argv[i];
    if (i + 1 >= argc) { return false; }
    const std::string value = argv[++i];
    if (key == "--matrix") { args->matrix_path = value; }
    else { return false; }
  }
  return true;
}

std::vector<std::string> ParseCsvLine(const std::string& line) {
  std::vector<std::string> fields;
  std::string current;
  bool quoted = false;
  for (std::size_t i = 0; i < line.size(); ++i) {
    const char c = line[i];
    if (c == '"') {
      if (quoted && i + 1 < line.size() && line[i + 1] == '"') {
        current.push_back('"');
        ++i;
      } else {
        quoted = !quoted;
      }
    } else if (c == ',' && !quoted) {
      fields.push_back(current);
      current.clear();
    } else {
      current.push_back(c);
    }
  }
  fields.push_back(current);
  return fields;
}

std::string FirstTypeId(const std::string& ids) {
  std::istringstream in(ids);
  std::string id;
  in >> id;
  return id;
}

std::vector<CanonicalRow> LoadRows(const std::filesystem::path& path, std::vector<std::string>* errors) {
  std::ifstream in(path);
  std::vector<CanonicalRow> rows;
  if (!in) {
    errors->push_back("matrix_not_readable");
    return rows;
  }
  std::string line;
  if (!std::getline(in, line)) {
    errors->push_back("matrix_empty");
    return rows;
  }
  const auto header = ParseCsvLine(line);
  if (header.size() != 11 || header[0] != "type_group" || header[7] != "index_stats_status") {
    errors->push_back("unexpected_header");
    return rows;
  }
  std::size_t line_no = 1;
  while (std::getline(in, line)) {
    ++line_no;
    if (line.empty()) { continue; }
    const auto fields = ParseCsvLine(line);
    if (fields.size() != 11) {
      errors->push_back("bad_field_count:" + std::to_string(line_no));
      continue;
    }
    rows.push_back({fields[0], fields[1], fields[2], fields[3], fields[4], fields[5],
                    fields[6], fields[7], fields[8], fields[9], fields[10]});
  }
  return rows;
}

EngineDescriptor DescriptorForRow(const CanonicalRow& row) {
  EngineDescriptor descriptor;
  descriptor.descriptor_kind = row.support_path == "domain_over_native" ? "domain" : "scalar";
  descriptor.canonical_type_name = FirstTypeId(row.canonical_type_ids);
  descriptor.encoded_descriptor = "canonical=" + descriptor.canonical_type_name + ";type_group=" + row.type_group;
  if (row.support_path == "opaque_preserve_render") {
    descriptor.canonical_type_name = "opaque_extension";
    descriptor.encoded_descriptor = "canonical=opaque_extension;type_group=" + row.type_group;
  }
  if (descriptor.descriptor_kind == "domain") {
    descriptor.encoded_descriptor = "domain_uuid=generated-by-index-probe;base_type=" +
                                    descriptor.canonical_type_name + ";type_group=" + row.type_group;
  }
  return descriptor;
}

bool IsKnownIndexStatus(const std::string& status) {
  return status == "implemented" || status == "validated" || status == "decided" || status == "blocked";
}

bool IsAdmittedStatus(const std::string& status) {
  return status == "implemented" || status == "validated";
}

void PrintBool(const std::string& name, bool value, bool comma) {
  std::cout << "  \"" << name << "\": " << (value ? "true" : "false") << (comma ? "," : "") << "\n";
}

}  // namespace

int main(int argc, char** argv) {
  Args args;
  if (!ParseArgs(argc, argv, &args)) {
    std::cerr << "usage: sb_datatype_index_optimizer_admission_probe [--matrix PATH]\n";
    return 2;
  }

  std::vector<std::string> errors;
  const auto rows = LoadRows(args.matrix_path, &errors);
  std::set<std::string> groups;
  bool status_values_ok = true;
  bool no_duplicate_groups = true;
  bool admission_cases_ok = true;
  bool diagnostic_cases_ok = true;
  bool optimizer_descriptor_use_ok = true;
  std::size_t admitted_index_cases = 0;
  std::size_t admitted_statistics_cases = 0;
  std::size_t diagnostic_denial_cases = 0;
  std::size_t optimizer_descriptor_cases = 0;

  for (const auto& row : rows) {
    if (!groups.insert(row.type_group).second) { no_duplicate_groups = false; }
    if (!IsKnownIndexStatus(row.index_stats_status)) { status_values_ok = false; }

    EngineDatatypeIndexOptimizerAdmissionRequest request;
    request.type_group = row.type_group;
    request.descriptor = DescriptorForRow(row);
    request.support_path = row.support_path;
    request.index_stats_status = row.index_stats_status;
    request.donor_label = "DONOR_LABEL_" + row.type_group;
    const auto result = EvaluateDatatypeIndexOptimizerAdmission(request);
    if (!result.optimizer_uses_canonical_descriptor) { optimizer_descriptor_use_ok = false; }
    ++optimizer_descriptor_cases;

    if (IsAdmittedStatus(row.index_stats_status) && row.support_path != "opaque_preserve_render") {
      if (!result.ok || !result.index_admitted || !result.statistics_admitted) { admission_cases_ok = false; }
      ++admitted_index_cases;
      ++admitted_statistics_cases;
    } else {
      if (result.diagnostic_detail.empty()) { diagnostic_cases_ok = false; }
      ++diagnostic_denial_cases;
    }
  }

  const bool ok = errors.empty() && !rows.empty() && status_values_ok && no_duplicate_groups &&
                  admission_cases_ok && diagnostic_cases_ok && optimizer_descriptor_use_ok &&
                  optimizer_descriptor_cases == rows.size();

  std::cout << "{\n";
  PrintBool("ok", ok, true);
  std::cout << "  \"row_count\": " << rows.size() << ",\n";
  std::cout << "  \"admitted_index_cases\": " << admitted_index_cases << ",\n";
  std::cout << "  \"admitted_statistics_cases\": " << admitted_statistics_cases << ",\n";
  std::cout << "  \"diagnostic_denial_cases\": " << diagnostic_denial_cases << ",\n";
  std::cout << "  \"optimizer_descriptor_cases\": " << optimizer_descriptor_cases << ",\n";
  PrintBool("status_values_ok", status_values_ok, true);
  PrintBool("no_duplicate_groups", no_duplicate_groups, true);
  PrintBool("admission_cases_ok", admission_cases_ok, true);
  PrintBool("diagnostic_cases_ok", diagnostic_cases_ok, true);
  PrintBool("optimizer_descriptor_use_ok", optimizer_descriptor_use_ok, false);
  std::cout << "}\n";
  return ok ? 0 : 1;
}

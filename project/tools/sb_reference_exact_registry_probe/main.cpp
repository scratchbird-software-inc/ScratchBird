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
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace {

struct Args {
  std::filesystem::path matrix_path =
      "docs" "/execution-plans/datatypes-and-domains-final-coverage-closure/REFERENCE_EXACT_DATATYPE_REGISTRY_MATRIX.csv";
};

struct ExactRow {
  std::string reference_engine;
  std::string source_row;
  std::string exact_reference_type;
  std::string exact_aliases;
  std::string normalized_family;
  std::string sb_support_path;
  std::string sb_descriptor_or_domain;
  std::string wire_behavior;
  std::string registry_status;
  std::string coverage_status;
  std::string diagnostic_code;
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

std::vector<ExactRow> LoadRows(const std::filesystem::path& path, std::vector<std::string>* errors) {
  std::ifstream in(path);
  std::vector<ExactRow> rows;
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
  const std::vector<std::string> expected = {
      "reference_engine", "source_row", "exact_reference_type", "exact_aliases", "normalized_family",
      "sb_support_path", "sb_descriptor_or_domain", "wire_behavior", "registry_status",
      "coverage_status", "diagnostic_code", "notes"};
  if (header != expected) {
    errors->push_back("unexpected_header");
    return rows;
  }

  std::size_t line_no = 1;
  while (std::getline(in, line)) {
    ++line_no;
    if (line.empty()) { continue; }
    const auto fields = ParseCsvLine(line);
    if (fields.size() != expected.size()) {
      errors->push_back("bad_field_count:" + std::to_string(line_no));
      continue;
    }
    rows.push_back({fields[0], fields[1], fields[2], fields[3], fields[4], fields[5],
                    fields[6], fields[7], fields[8], fields[9], fields[10], fields[11]});
  }
  return rows;
}

bool IsAllowedSupportPath(const std::string& value) {
  return value == "native_substrate" || value == "native_with_wire_conversion" ||
         value == "domain_over_native" || value == "opaque_preserve_render" ||
         value == "unsupported_by_version";
}

bool IsAllowedRegistryStatus(const std::string& value) {
  return value == "exact" || value == "expanded_from_group" ||
         value == "family_fallback_authorized" || value == "rejected_by_version";
}

bool IsAllowedCoverageStatus(const std::string& value) {
  return value == "implemented" || value == "decided" || value == "rejected_by_version";
}

bool IsRejected(const ExactRow& row) {
  return row.registry_status == "rejected_by_version" || row.coverage_status == "rejected_by_version" ||
         row.sb_support_path == "unsupported_by_version" || row.wire_behavior == "reject";
}

bool HasPrefix(const std::string& value, const std::string& prefix) {
  return value.rfind(prefix, 0) == 0;
}

void PrintBool(const std::string& name, bool value, bool comma) {
  std::cout << "  \"" << name << "\": " << (value ? "true" : "false") << (comma ? "," : "") << "\n";
}

}  // namespace

int main(int argc, char** argv) {
  Args args;
  if (!ParseArgs(argc, argv, &args)) {
    std::cerr << "usage: sb_reference_exact_registry_probe [--matrix PATH]\n";
    return 2;
  }

  std::vector<std::string> errors;
  const auto rows = LoadRows(args.matrix_path, &errors);
  std::set<std::string> keys;
  std::set<std::string> source_rows;
  bool required_fields_present = true;
  bool no_duplicate_exact_rows = true;
  bool support_paths_ok = true;
  bool registry_status_ok = true;
  bool coverage_status_ok = true;
  bool rejection_diagnostics_ok = true;
  bool positive_diagnostics_ok = true;
  bool explicit_family_fallback_only = true;
  bool parse_bind_render_metadata_cases_ok = true;
  std::size_t exact_rows = 0;
  std::size_t expanded_rows = 0;
  std::size_t explicit_family_fallback_rows = 0;
  std::size_t rejected_rows = 0;

  for (const auto& row : rows) {
    if (row.reference_engine.empty() || row.source_row.empty() || row.exact_reference_type.empty() ||
        row.exact_aliases.empty() || row.normalized_family.empty() || row.sb_support_path.empty() ||
        row.sb_descriptor_or_domain.empty() || row.wire_behavior.empty() || row.registry_status.empty() ||
        row.coverage_status.empty() || row.diagnostic_code.empty() || row.notes.empty()) {
      required_fields_present = false;
    }
    const std::string key = row.reference_engine + "|" + row.exact_reference_type;
    if (!keys.insert(key).second) { no_duplicate_exact_rows = false; }
    source_rows.insert(row.reference_engine + "|" + row.source_row);
    if (!IsAllowedSupportPath(row.sb_support_path)) { support_paths_ok = false; }
    if (!IsAllowedRegistryStatus(row.registry_status)) { registry_status_ok = false; }
    if (!IsAllowedCoverageStatus(row.coverage_status)) { coverage_status_ok = false; }

    if (row.registry_status == "exact") { ++exact_rows; }
    if (row.registry_status == "expanded_from_group") { ++expanded_rows; }
    if (row.registry_status == "family_fallback_authorized") {
      ++explicit_family_fallback_rows;
      if (row.notes.find("explicit_family_fallback") == std::string::npos) {
        explicit_family_fallback_only = false;
      }
    }

    if (IsRejected(row)) {
      ++rejected_rows;
      if (row.coverage_status != "rejected_by_version" || !HasPrefix(row.diagnostic_code, "SB-DATATYPE-")) {
        rejection_diagnostics_ok = false;
      }
    } else {
      if (row.diagnostic_code != "SB-OK" || HasPrefix(row.sb_descriptor_or_domain, "SB-DATATYPE-") ||
          row.wire_behavior == "reject") {
        positive_diagnostics_ok = false;
      }
    }

    if (row.registry_status != "family_fallback_authorized" &&
        row.notes.find("family_fallback") != std::string::npos) {
      explicit_family_fallback_only = false;
    }

    const bool has_parse_case = !row.exact_reference_type.empty();
    const bool has_bind_case = !row.sb_descriptor_or_domain.empty();
    const bool has_render_case = !row.wire_behavior.empty();
    const bool has_metadata_case = !row.normalized_family.empty() && !row.exact_aliases.empty();
    if (!has_parse_case || !has_bind_case || !has_render_case || !has_metadata_case) {
      parse_bind_render_metadata_cases_ok = false;
    }
  }

  const bool ok = errors.empty() && !rows.empty() && required_fields_present && no_duplicate_exact_rows &&
                  support_paths_ok && registry_status_ok && coverage_status_ok && rejection_diagnostics_ok &&
                  positive_diagnostics_ok && explicit_family_fallback_only && parse_bind_render_metadata_cases_ok;

  std::cout << "{\n";
  PrintBool("ok", ok, true);
  std::cout << "  \"row_count\": " << rows.size() << ",\n";
  std::cout << "  \"source_row_count\": " << source_rows.size() << ",\n";
  std::cout << "  \"exact_rows\": " << exact_rows << ",\n";
  std::cout << "  \"expanded_rows\": " << expanded_rows << ",\n";
  std::cout << "  \"explicit_family_fallback_rows\": " << explicit_family_fallback_rows << ",\n";
  std::cout << "  \"rejected_rows\": " << rejected_rows << ",\n";
  PrintBool("required_fields_present", required_fields_present, true);
  PrintBool("no_duplicate_exact_rows", no_duplicate_exact_rows, true);
  PrintBool("support_paths_ok", support_paths_ok, true);
  PrintBool("registry_status_ok", registry_status_ok, true);
  PrintBool("coverage_status_ok", coverage_status_ok, true);
  PrintBool("rejection_diagnostics_ok", rejection_diagnostics_ok, true);
  PrintBool("positive_diagnostics_ok", positive_diagnostics_ok, true);
  PrintBool("explicit_family_fallback_only", explicit_family_fallback_only, true);
  PrintBool("parse_bind_render_metadata_cases_ok", parse_bind_render_metadata_cases_ok, false);
  std::cout << "}\n";
  return ok ? 0 : 1;
}

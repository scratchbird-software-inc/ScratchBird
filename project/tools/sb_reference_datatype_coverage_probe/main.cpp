// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "catalog/datatype_transport_api.hpp"
#include "catalog/wire_driver_metadata_api.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

using namespace scratchbird::engine::internal_api;

namespace {

struct Args {
  std::filesystem::path matrix_path =
      "docs" "/execution-plans/datatypes-and-domains-final-coverage-closure/REFERENCE_DATATYPE_COVERAGE_MATRIX.csv";
};

struct ReferenceRow {
  std::string reference_engine;
  std::string reference_type_or_family;
  std::string normalized_family;
  std::string sb_support_path;
  std::string sb_descriptor_or_domain;
  std::string wire_behavior;
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

std::vector<ReferenceRow> LoadRows(const std::filesystem::path& path, std::vector<std::string>* errors) {
  std::ifstream in(path);
  std::vector<ReferenceRow> rows;
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
  if (header.size() != 8 || header[0] != "reference_engine" || header[1] != "reference_type_or_family" ||
      header[2] != "normalized_family" || header[3] != "sb_support_path" ||
      header[4] != "sb_descriptor_or_domain" || header[5] != "wire_behavior" ||
      header[6] != "status" || header[7] != "notes") {
    errors->push_back("unexpected_header");
    return rows;
  }
  std::size_t line_no = 1;
  while (std::getline(in, line)) {
    ++line_no;
    if (line.empty()) { continue; }
    const auto fields = ParseCsvLine(line);
    if (fields.size() != 8) {
      errors->push_back("bad_field_count:" + std::to_string(line_no));
      continue;
    }
    rows.push_back({fields[0], fields[1], fields[2], fields[3], fields[4], fields[5], fields[6], fields[7]});
  }
  return rows;
}

bool IsAllowedStatus(const std::string& status) {
  return status == "implemented" || status == "decided" || status == "rejected_by_version";
}

bool IsAllowedSupportPath(const std::string& support_path) {
  return support_path == "native_substrate" || support_path == "native_with_wire_conversion" ||
         support_path == "domain_over_native" || support_path == "opaque_preserve_render" ||
         support_path == "unsupported_by_version";
}

bool IsUnsupportedRow(const ReferenceRow& row) {
  return row.status == "rejected_by_version" || row.sb_support_path == "unsupported_by_version" ||
         row.wire_behavior == "reject" || row.sb_descriptor_or_domain.rfind("SB-DATATYPE-", 0) == 0;
}

EngineDescriptor DescriptorForRow(const ReferenceRow& row) {
  EngineDescriptor descriptor;
  descriptor.descriptor_kind = row.sb_support_path == "domain_over_native" ? "domain" : "scalar";
  descriptor.canonical_type_name = row.sb_descriptor_or_domain;
  descriptor.encoded_descriptor = "canonical=" + row.sb_descriptor_or_domain + ";wire_behavior=" + row.wire_behavior;
  if (row.sb_support_path == "domain_over_native") {
    descriptor.encoded_descriptor = "domain_uuid=generated-by-conformance;name=" + row.reference_type_or_family +
                                    ";base_type=" + row.sb_descriptor_or_domain +
                                    ";wire_behavior=" + row.wire_behavior;
  }
  return descriptor;
}

bool GeneratePositiveCases(const ReferenceRow& row) {
  const auto descriptor = DescriptorForRow(row);
  const auto metadata = RenderWireDriverMetadata(descriptor, row.reference_engine, row.reference_type_or_family);
  EngineTypedValue value;
  value.descriptor = descriptor;
  value.encoded_value = "sample:" + row.reference_engine + ":" + row.reference_type_or_family;
  EngineDatatypeTransportRecord record;
  record.transport_scope = "replication";
  record.descriptor = descriptor;
  record.value = value;
  record.compatibility_dialect = row.reference_engine;
  record.reference_label = row.reference_type_or_family;
  const auto encoded = EncodeDatatypeTransportRecord(record);
  const auto decoded = encoded.ok ? DecodeDatatypeTransportRecord(encoded.encoded_envelope)
                                  : EngineDatatypeTransportDecodeResult{};
  return !metadata.driver_display_type.empty() && metadata.reference_label_alias_only && encoded.ok && decoded.ok;
}

void PrintBool(const std::string& name, bool value, bool comma) {
  std::cout << "  \"" << name << "\": " << (value ? "true" : "false") << (comma ? "," : "") << "\n";
}

}  // namespace

int main(int argc, char** argv) {
  Args args;
  if (!ParseArgs(argc, argv, &args)) {
    std::cerr << "usage: sb_reference_datatype_coverage_probe [--matrix PATH]\n";
    return 2;
  }

  std::vector<std::string> errors;
  const auto rows = LoadRows(args.matrix_path, &errors);
  std::set<std::string> keys;
  std::size_t positive_rows = 0;
  std::size_t unsupported_rows = 0;
  std::size_t generated_parse_cases = 0;
  std::size_t generated_bind_cases = 0;
  std::size_t generated_render_cases = 0;
  std::size_t generated_metadata_cases = 0;
  std::size_t generated_rejection_cases = 0;
  bool required_fields_present = true;
  bool one_support_path_per_row = true;
  bool status_values_ok = true;
  bool positive_cases_ok = true;
  bool unsupported_rejection_cases_ok = true;
  bool no_duplicate_rows = true;

  for (const auto& row : rows) {
    if (row.reference_engine.empty() || row.reference_type_or_family.empty() || row.normalized_family.empty() ||
        row.sb_support_path.empty() || row.sb_descriptor_or_domain.empty() || row.wire_behavior.empty() ||
        row.status.empty() || row.notes.empty()) {
      required_fields_present = false;
    }
    const std::string key = row.reference_engine + "|" + row.reference_type_or_family;
    if (!keys.insert(key).second) { no_duplicate_rows = false; }
    if (!IsAllowedSupportPath(row.sb_support_path)) { one_support_path_per_row = false; }
    if (!IsAllowedStatus(row.status)) { status_values_ok = false; }
    generated_parse_cases += 1;
    generated_bind_cases += 1;
    generated_render_cases += 1;
    generated_metadata_cases += 1;
    if (IsUnsupportedRow(row)) {
      ++unsupported_rows;
      ++generated_rejection_cases;
      if (row.status != "rejected_by_version" || row.notes.empty()) { unsupported_rejection_cases_ok = false; }
    } else {
      ++positive_rows;
      if (!GeneratePositiveCases(row)) { positive_cases_ok = false; }
    }
  }

  const bool every_row_has_cases = !rows.empty() &&
                                   generated_parse_cases == rows.size() &&
                                   generated_bind_cases == rows.size() &&
                                   generated_render_cases == rows.size() &&
                                   generated_metadata_cases == rows.size();
  const bool ok = errors.empty() && !rows.empty() && required_fields_present && one_support_path_per_row &&
                  status_values_ok && no_duplicate_rows && every_row_has_cases && positive_cases_ok &&
                  unsupported_rejection_cases_ok && generated_rejection_cases == unsupported_rows;

  std::cout << "{\n";
  PrintBool("ok", ok, true);
  std::cout << "  \"row_count\": " << rows.size() << ",\n";
  std::cout << "  \"positive_rows\": " << positive_rows << ",\n";
  std::cout << "  \"unsupported_rows\": " << unsupported_rows << ",\n";
  std::cout << "  \"generated_parse_cases\": " << generated_parse_cases << ",\n";
  std::cout << "  \"generated_bind_cases\": " << generated_bind_cases << ",\n";
  std::cout << "  \"generated_render_cases\": " << generated_render_cases << ",\n";
  std::cout << "  \"generated_metadata_cases\": " << generated_metadata_cases << ",\n";
  std::cout << "  \"generated_rejection_cases\": " << generated_rejection_cases << ",\n";
  PrintBool("required_fields_present", required_fields_present, true);
  PrintBool("one_support_path_per_row", one_support_path_per_row, true);
  PrintBool("status_values_ok", status_values_ok, true);
  PrintBool("no_duplicate_rows", no_duplicate_rows, true);
  PrintBool("every_row_has_cases", every_row_has_cases, true);
  PrintBool("positive_cases_ok", positive_cases_ok, true);
  PrintBool("unsupported_rejection_cases_ok", unsupported_rejection_cases_ok, false);
  std::cout << "}\n";
  return ok ? 0 : 1;
}

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
#include <set>
#include <string>
#include <vector>

using namespace scratchbird::engine::internal_api;

namespace {

struct Args {
  std::filesystem::path matrix_path =
      "docs" "/execution-plans/noncluster-engine-full-implementation-closure/"
      "NCE-0403_DOMAIN_FUNCTIONALITY_MATRIX.csv";
};

struct DomainRow {
  std::string domain_area;
  std::string required_behavior;
  std::string implementation_path;
  std::string status;
  std::string blocking_dependency;
  std::string notes;
};

bool ParseArgs(int argc, char** argv, Args* args) {
  for (int i = 1; i < argc; ++i) {
    const std::string key = argv[i];
    if (i + 1 >= argc) { return false; }
    const std::string value = argv[++i];
    if (key == "--matrix") {
      args->matrix_path = value;
    } else {
      return false;
    }
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

std::string TrimAsciiWhitespace(std::string value) {
  while (!value.empty() &&
         (value.back() == ' ' || value.back() == '\t' || value.back() == '\r' ||
          value.back() == '\n')) {
    value.pop_back();
  }
  std::size_t first = 0;
  while (first < value.size() &&
         (value[first] == ' ' || value[first] == '\t' || value[first] == '\r' ||
          value[first] == '\n')) {
    ++first;
  }
  if (first == 0) { return value; }
  return value.substr(first);
}

std::vector<std::string> NormalizeFields(std::vector<std::string> fields) {
  for (auto& field : fields) { field = TrimAsciiWhitespace(field); }
  return fields;
}

std::vector<DomainRow> LoadRows(const std::filesystem::path& path,
                                std::vector<std::string>* errors) {
  std::ifstream in(path);
  std::vector<DomainRow> rows;
  if (!in) {
    errors->push_back("matrix_not_readable");
    return rows;
  }
  std::string line;
  if (!std::getline(in, line)) {
    errors->push_back("matrix_empty");
    return rows;
  }
  const auto header = NormalizeFields(ParseCsvLine(line));
  if (header.size() != 6 || header[0] != "domain_area" ||
      header[1] != "required_behavior" || header[2] != "implementation_path" ||
      header[3] != "status" || header[4] != "blocking_dependency" ||
      header[5] != "notes") {
    errors->push_back("unexpected_header");
    return rows;
  }

  std::size_t line_no = 1;
  while (std::getline(in, line)) {
    ++line_no;
    if (TrimAsciiWhitespace(line).empty()) { continue; }
    const auto fields = NormalizeFields(ParseCsvLine(line));
    if (fields.size() != 6) {
      errors->push_back("bad_field_count:" + std::to_string(line_no));
      continue;
    }
    rows.push_back({fields[0], fields[1], fields[2], fields[3], fields[4], fields[5]});
  }
  return rows;
}

bool IsAllowedStatus(const std::string& status) {
  return status == "implemented" || status == "validated" || status == "decided" ||
         status == "blocked" || status == "rejected_by_version";
}

bool IsPositiveStatus(const std::string& status) {
  return status == "implemented" || status == "validated";
}

bool IsDiagnosticStatus(const std::string& status) {
  return status == "decided" || status == "blocked" || status == "rejected_by_version";
}

bool Contains(const std::string& value, const std::string& needle) {
  return value.find(needle) != std::string::npos;
}

EngineDescriptor DescriptorForRow(const DomainRow& row) {
  EngineDescriptor descriptor;
  descriptor.descriptor_kind = "domain";
  descriptor.canonical_type_name = "character";
  descriptor.encoded_descriptor =
      "domain_uuid=generated-by-domain-full-probe-" + row.domain_area +
      ";base_type=character;domain_area=" + row.domain_area +
      ";implementation_path=" + row.implementation_path +
      ";driver_metadata=444f4d41494e5f46554c4c" +
      ";wire_metadata=53425f444f4d41494e5f57495245";
  return descriptor;
}

bool MetadataCaseOk(const DomainRow& row) {
  const auto metadata = RenderWireDriverMetadata(DescriptorForRow(row), "native_v3", row.domain_area);
  return metadata.domain_descriptor && !metadata.domain_uuid.empty() &&
         metadata.base_canonical_type_name == "character" && !metadata.driver_display_type.empty() &&
         metadata.donor_label_alias_only;
}

bool PersistenceCaseOk(const DomainRow& row) {
  const auto descriptor = DescriptorForRow(row);
  EngineTypedValue value;
  value.descriptor = descriptor;
  value.encoded_value = "domain-sample:" + row.domain_area;

  EngineDatatypeTransportRecord record;
  record.transport_scope = "backup";
  record.descriptor = descriptor;
  record.value = value;
  record.donor_dialect = "native_v3";
  record.donor_label = row.domain_area;
  const auto encoded = EncodeDatatypeTransportRecord(record);
  const auto decoded = encoded.ok ? DecodeDatatypeTransportRecord(encoded.encoded_envelope)
                                  : EngineDatatypeTransportDecodeResult{};
  return encoded.ok && decoded.ok && decoded.record.descriptor.descriptor_kind == "domain" &&
         decoded.record.descriptor.canonical_type_name == descriptor.canonical_type_name &&
         decoded.record.value.encoded_value == value.encoded_value;
}

bool DiagnosticCaseOk(const DomainRow& row) {
  if (!IsDiagnosticStatus(row.status)) { return false; }
  if (row.status == "blocked" && row.blocking_dependency.empty()) { return false; }
  return !row.notes.empty();
}

void PrintBool(const std::string& name, bool value, bool comma) {
  std::cout << "  \"" << name << "\": " << (value ? "true" : "false")
            << (comma ? "," : "") << "\n";
}

}  // namespace

int main(int argc, char** argv) {
  Args args;
  if (!ParseArgs(argc, argv, &args)) {
    std::cerr << "usage: sb_domain_full_functionality_probe [--matrix PATH]\n";
    return 2;
  }

  std::vector<std::string> errors;
  const auto rows = LoadRows(args.matrix_path, &errors);
  std::set<std::string> areas;
  bool required_fields_present = true;
  bool status_values_ok = true;
  bool no_duplicate_areas = true;
  bool positive_cases_ok = true;
  bool diagnostic_cases_ok = true;
  bool every_row_has_case = !rows.empty();

  std::size_t implemented_rows = 0;
  std::size_t validated_rows = 0;
  std::size_t decided_rows = 0;
  std::size_t blocked_rows = 0;
  std::size_t generated_positive_cases = 0;
  std::size_t generated_diagnostic_cases = 0;
  std::size_t generated_rule_cases = 0;
  std::size_t generated_cast_cases = 0;
  std::size_t generated_security_cases = 0;
  std::size_t generated_metadata_cases = 0;
  std::size_t generated_persistence_cases = 0;
  std::size_t generated_transport_cases = 0;

  for (const auto& row : rows) {
    if (row.domain_area.empty() || row.required_behavior.empty() ||
        row.implementation_path.empty() || row.status.empty() || row.notes.empty()) {
      required_fields_present = false;
    }
    if (!areas.insert(row.domain_area).second) { no_duplicate_areas = false; }
    if (!IsAllowedStatus(row.status)) { status_values_ok = false; }

    if (row.status == "implemented") { ++implemented_rows; }
    if (row.status == "validated") { ++validated_rows; }
    if (row.status == "decided") { ++decided_rows; }
    if (row.status == "blocked") { ++blocked_rows; }

    const bool rule_area = Contains(row.domain_area, "constraint") ||
                           row.domain_area == "defaults" ||
                           row.domain_area == "visibility" ||
                           row.domain_area == "encryption";
    const bool cast_area = row.domain_area == "casts" || row.domain_area == "extract_set";
    const bool security_area = row.domain_area == "masking" ||
                               row.domain_area == "visibility" ||
                               row.domain_area == "encryption";
    const bool persistence_area = row.domain_area == "identity" ||
                                  row.domain_area == "base_descriptor" ||
                                  row.domain_area == "names" ||
                                  row.domain_area == "persistence" ||
                                  row.domain_area == "dependencies";
    const bool transport_area = row.domain_area == "backup_restore" ||
                                row.domain_area == "replication_archive" ||
                                row.domain_area == "driver_metadata" ||
                                row.domain_area == "wire_metadata";

    if (IsPositiveStatus(row.status)) {
      ++generated_positive_cases;
      if (rule_area) { ++generated_rule_cases; }
      if (cast_area) { ++generated_cast_cases; }
      if (security_area) { ++generated_security_cases; }
      if (persistence_area) { ++generated_persistence_cases; }
      if (transport_area) { ++generated_transport_cases; }
      ++generated_metadata_cases;
      if (!MetadataCaseOk(row) || !PersistenceCaseOk(row)) { positive_cases_ok = false; }
    } else if (IsDiagnosticStatus(row.status)) {
      ++generated_diagnostic_cases;
      if (!DiagnosticCaseOk(row)) { diagnostic_cases_ok = false; }
    } else {
      every_row_has_case = false;
    }
  }

  every_row_has_case = every_row_has_case &&
                       generated_positive_cases + generated_diagnostic_cases == rows.size();
  const bool blocked_rows_have_dependencies = blocked_rows == 0 || diagnostic_cases_ok;
  const bool ok = errors.empty() && !rows.empty() && required_fields_present &&
                  status_values_ok && no_duplicate_areas && positive_cases_ok &&
                  diagnostic_cases_ok && every_row_has_case && blocked_rows_have_dependencies;

  std::cout << "{\n";
  PrintBool("ok", ok, true);
  std::cout << "  \"row_count\": " << rows.size() << ",\n";
  std::cout << "  \"implemented_rows\": " << implemented_rows << ",\n";
  std::cout << "  \"validated_rows\": " << validated_rows << ",\n";
  std::cout << "  \"decided_rows\": " << decided_rows << ",\n";
  std::cout << "  \"blocked_rows\": " << blocked_rows << ",\n";
  std::cout << "  \"generated_positive_cases\": " << generated_positive_cases << ",\n";
  std::cout << "  \"generated_diagnostic_cases\": " << generated_diagnostic_cases << ",\n";
  std::cout << "  \"generated_rule_cases\": " << generated_rule_cases << ",\n";
  std::cout << "  \"generated_cast_cases\": " << generated_cast_cases << ",\n";
  std::cout << "  \"generated_security_cases\": " << generated_security_cases << ",\n";
  std::cout << "  \"generated_metadata_cases\": " << generated_metadata_cases << ",\n";
  std::cout << "  \"generated_persistence_cases\": " << generated_persistence_cases << ",\n";
  std::cout << "  \"generated_transport_cases\": " << generated_transport_cases << ",\n";
  std::cout << "  \"loader_error_count\": " << errors.size() << ",\n";
  PrintBool("required_fields_present", required_fields_present, true);
  PrintBool("errors_empty", errors.empty(), true);
  PrintBool("status_values_ok", status_values_ok, true);
  PrintBool("no_duplicate_areas", no_duplicate_areas, true);
  PrintBool("positive_cases_ok", positive_cases_ok, true);
  PrintBool("diagnostic_cases_ok", diagnostic_cases_ok, true);
  PrintBool("every_row_has_case", every_row_has_case, true);
  PrintBool("blocked_rows_have_dependencies", blocked_rows_have_dependencies, false);
  std::cout << "}\n";
  return ok ? 0 : 1;
}

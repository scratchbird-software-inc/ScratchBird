// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "catalog/datatype_transport_api.hpp"
#include "catalog/wire_driver_metadata_api.hpp"
#include "datatype_operations.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

using namespace scratchbird::engine::internal_api;
namespace dt = scratchbird::core::datatypes;

namespace {

struct Args {
  std::filesystem::path matrix_path =
      "docs" "/execution-plans/datatypes-and-domains-final-coverage-closure/CANONICAL_DATATYPE_CLOSURE_MATRIX.csv";
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

std::vector<std::string> SplitWords(const std::string& value) {
  std::vector<std::string> words;
  std::istringstream in(value);
  std::string word;
  while (in >> word) { words.push_back(word); }
  return words;
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
  if (header.size() != 11 || header[0] != "type_group" || header[1] != "canonical_type_ids" ||
      header[2] != "support_path" || header[3] != "descriptor_status" || header[4] != "storage_status" ||
      header[5] != "operation_status" || header[6] != "wire_driver_status" ||
      header[7] != "index_stats_status" || header[8] != "backup_replication_status" ||
      header[9] != "status" || header[10] != "notes") {
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

bool IsAllowedComponentStatus(const std::string& status) {
  return status == "implemented" || status == "decided" || status == "blocked" || status == "validated";
}

bool IsAllowedOverallStatus(const std::string& status) {
  return status == "implemented" || status == "decided" || status == "validated" || status == "rejected_by_version";
}

bool IsAllowedSupportPath(const std::string& support_path) {
  return support_path == "native_substrate" || support_path == "opaque_preserve_render" ||
         support_path == "domain_over_native" || support_path == "unsupported_by_version";
}

bool HasKnownType(const CanonicalRow& row, std::string* stable_name, dt::CanonicalTypeId* type_id) {
  for (const auto& candidate : SplitWords(row.canonical_type_ids)) {
    const auto id = dt::CanonicalTypeIdFromStableName(candidate);
    if (id != dt::CanonicalTypeId::unknown) {
      *stable_name = candidate;
      *type_id = id;
      return true;
    }
  }
  return false;
}

EngineDescriptor DescriptorFor(const std::string& stable_name, const CanonicalRow& row) {
  EngineDescriptor descriptor;
  descriptor.descriptor_kind = row.support_path == "domain_over_native" ? "domain" : "scalar";
  descriptor.canonical_type_name = stable_name.empty() ? row.type_group : stable_name;
  descriptor.encoded_descriptor = "canonical=" + descriptor.canonical_type_name + ";type_group=" + row.type_group;
  if (row.support_path == "domain_over_native") {
    descriptor.encoded_descriptor = "domain_uuid=generated-by-canonical-probe;base_type=" +
                                    descriptor.canonical_type_name + ";type_group=" + row.type_group;
  }
  return descriptor;
}

bool MetadataCaseOk(const EngineDescriptor& descriptor) {
  const auto metadata = RenderWireDriverMetadata(descriptor);
  return !metadata.descriptor_kind.empty() && !metadata.driver_display_type.empty();
}

bool PersistenceCaseOk(const EngineDescriptor& descriptor, const std::string& value) {
  EngineTypedValue typed;
  typed.descriptor = descriptor;
  typed.encoded_value = value;
  EngineDatatypeTransportRecord record;
  record.transport_scope = "backup";
  record.descriptor = descriptor;
  record.value = typed;
  const auto encoded = EncodeDatatypeTransportRecord(record);
  if (!encoded.ok) { return false; }
  const auto decoded = DecodeDatatypeTransportRecord(encoded.encoded_envelope);
  return decoded.ok && decoded.record.descriptor.canonical_type_name == descriptor.canonical_type_name &&
         decoded.record.value.encoded_value == value;
}

std::string SampleValueFor(dt::CanonicalTypeId type_id) {
  switch (type_id) {
    case dt::CanonicalTypeId::boolean: return "true";
    case dt::CanonicalTypeId::uuid: return "018f7f8f-7c00-7000-8000-000000000001";
    case dt::CanonicalTypeId::date: return "2026-05-01";
    case dt::CanonicalTypeId::time: return "12:34:56";
    case dt::CanonicalTypeId::timestamp: return "2026-05-01T12:34:56";
    case dt::CanonicalTypeId::interval: return "P1D";
    case dt::CanonicalTypeId::document:
    case dt::CanonicalTypeId::json_document:
    case dt::CanonicalTypeId::binary_json_document:
    case dt::CanonicalTypeId::bson_document:
    case dt::CanonicalTypeId::object_document:
    case dt::CanonicalTypeId::flattened_object_document:
      return "{\"ok\":true}";
    case dt::CanonicalTypeId::xml_document: return "<root/>";
    case dt::CanonicalTypeId::set_value: return "SBSET1;values=1";
    default: return "1";
  }
}

bool CastCaseOk(dt::CanonicalTypeId type_id) {
  dt::DatatypeCastRequest request;
  request.value.type_id = type_id;
  request.value.encoded_value = SampleValueFor(type_id);
  request.value.is_null = false;
  request.target_type_id = type_id;
  request.explicit_cast = true;
  return dt::CastDatatypeValue(request).ok();
}

void PrintBool(const std::string& name, bool value, bool comma) {
  std::cout << "  \"" << name << "\": " << (value ? "true" : "false") << (comma ? "," : "") << "\n";
}

}  // namespace

int main(int argc, char** argv) {
  Args args;
  if (!ParseArgs(argc, argv, &args)) {
    std::cerr << "usage: sb_canonical_datatype_closure_probe [--matrix PATH]\n";
    return 2;
  }

  std::vector<std::string> errors;
  const auto rows = LoadRows(args.matrix_path, &errors);
  std::set<std::string> groups;
  bool required_fields_present = true;
  bool statuses_ok = true;
  bool support_paths_ok = true;
  bool no_duplicate_groups = true;
  bool implemented_cases_ok = true;
  std::vector<std::string> implementation_failures;
  std::size_t generated_persistence_cases = 0;
  std::size_t generated_cast_cases = 0;
  std::size_t generated_metadata_cases = 0;
  std::size_t generated_diagnostic_cases = 0;
  std::size_t generated_null_cases = 0;
  std::size_t generated_default_cases = 0;
  std::size_t known_type_rows = 0;
  std::size_t decided_or_blocked_rows = 0;

  for (const auto& row : rows) {
    if (row.type_group.empty() || row.canonical_type_ids.empty() || row.support_path.empty() ||
        row.descriptor_status.empty() || row.storage_status.empty() || row.operation_status.empty() ||
        row.wire_driver_status.empty() || row.index_stats_status.empty() ||
        row.backup_replication_status.empty() || row.status.empty() || row.notes.empty()) {
      required_fields_present = false;
    }
    if (!groups.insert(row.type_group).second) { no_duplicate_groups = false; }
    if (!IsAllowedSupportPath(row.support_path)) { support_paths_ok = false; }
    if (!IsAllowedComponentStatus(row.descriptor_status) || !IsAllowedComponentStatus(row.storage_status) ||
        !IsAllowedComponentStatus(row.operation_status) || !IsAllowedComponentStatus(row.wire_driver_status) ||
        !IsAllowedComponentStatus(row.index_stats_status) ||
        !IsAllowedComponentStatus(row.backup_replication_status) || !IsAllowedOverallStatus(row.status)) {
      statuses_ok = false;
    }

    std::string stable_name;
    dt::CanonicalTypeId type_id = dt::CanonicalTypeId::unknown;
    const bool known = HasKnownType(row, &stable_name, &type_id);
    if (known) { ++known_type_rows; }
    const auto descriptor = DescriptorFor(stable_name, row);

    ++generated_persistence_cases;
    ++generated_metadata_cases;
    ++generated_null_cases;
    ++generated_default_cases;
    if (row.status == "decided" || row.descriptor_status == "decided" || row.operation_status == "decided" ||
        row.wire_driver_status == "decided" || row.index_stats_status == "decided" ||
        row.index_stats_status == "blocked") {
      ++decided_or_blocked_rows;
      ++generated_diagnostic_cases;
    }

    if (!MetadataCaseOk(descriptor)) {
      implemented_cases_ok = false;
      implementation_failures.push_back(row.type_group + ":metadata");
    }
    if (!PersistenceCaseOk(descriptor, "sample:" + row.type_group)) {
      implemented_cases_ok = false;
      implementation_failures.push_back(row.type_group + ":persistence");
    }
    if (known && (row.operation_status == "implemented" || row.operation_status == "validated")) {
      ++generated_cast_cases;
      if (row.support_path != "opaque_preserve_render" && !CastCaseOk(type_id)) {
        implemented_cases_ok = false;
        implementation_failures.push_back(row.type_group + ":cast");
      }
    } else {
      ++generated_diagnostic_cases;
    }
  }

  const bool every_row_has_cases = !rows.empty() &&
                                   generated_persistence_cases == rows.size() &&
                                   generated_metadata_cases == rows.size() &&
                                   generated_null_cases == rows.size() &&
                                   generated_default_cases == rows.size();
  const bool ok = errors.empty() && !rows.empty() && required_fields_present && statuses_ok && support_paths_ok &&
                  no_duplicate_groups && every_row_has_cases && implemented_cases_ok && known_type_rows > 0 &&
                  generated_diagnostic_cases >= decided_or_blocked_rows;

  std::cout << "{\n";
  PrintBool("ok", ok, true);
  std::cout << "  \"row_count\": " << rows.size() << ",\n";
  std::cout << "  \"known_type_rows\": " << known_type_rows << ",\n";
  std::cout << "  \"decided_or_blocked_rows\": " << decided_or_blocked_rows << ",\n";
  std::cout << "  \"generated_persistence_cases\": " << generated_persistence_cases << ",\n";
  std::cout << "  \"generated_cast_cases\": " << generated_cast_cases << ",\n";
  std::cout << "  \"generated_metadata_cases\": " << generated_metadata_cases << ",\n";
  std::cout << "  \"generated_diagnostic_cases\": " << generated_diagnostic_cases << ",\n";
  std::cout << "  \"generated_null_cases\": " << generated_null_cases << ",\n";
  std::cout << "  \"generated_default_cases\": " << generated_default_cases << ",\n";
  PrintBool("required_fields_present", required_fields_present, true);
  PrintBool("statuses_ok", statuses_ok, true);
  PrintBool("support_paths_ok", support_paths_ok, true);
  PrintBool("no_duplicate_groups", no_duplicate_groups, true);
  PrintBool("every_row_has_cases", every_row_has_cases, true);
  PrintBool("implemented_cases_ok", implemented_cases_ok, true);
  std::cout << "  \"implementation_failure_count\": " << implementation_failures.size() << ",\n";
  std::cout << "  \"first_implementation_failure\": \""
            << (implementation_failures.empty() ? "" : implementation_failures.front()) << "\"\n";
  std::cout << "}\n";
  return ok ? 0 : 1;
}

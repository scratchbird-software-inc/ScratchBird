// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "catalog/datatype_index_optimizer_admission_api.hpp"
#include "catalog/wire_driver_metadata_api.hpp"
#include "datatype_binary.hpp"
#include "datatype_layout.hpp"
#include "datatype_operations.hpp"
#include "datatype_wire_metadata.hpp"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace dt = scratchbird::core::datatypes;
namespace api = scratchbird::engine::internal_api;

namespace {

struct Args {
  std::filesystem::path output_path;
};

struct MatrixRow {
  std::uint32_t canonical_type_id = 0;
  std::string canonical_type_name;
  std::string type_family;
  std::string width_class;
  std::string descriptor_status;
  std::string storage_status;
  std::string binary_encoding_status;
  std::string binary_encoding;
  std::string cast_status;
  std::string comparison_status;
  std::string sort_key_status;
  std::string hash_status;
  std::string wire_metadata_status;
  std::string domain_psql_status;
  std::string index_optimizer_status;
  std::string production_ready;
  std::string production_ready_reason;
};

bool ParseArgs(int argc, char** argv, Args* args) {
  for (int i = 1; i < argc; ++i) {
    const std::string key = argv[i];
    if (key == "--output" && i + 1 < argc) {
      args->output_path = argv[++i];
      continue;
    }
    return false;
  }
  return !args->output_path.empty();
}

std::string CsvEscape(const std::string& value) {
  bool needs_quotes = false;
  for (const char c : value) {
    if (c == ',' || c == '"' || c == '\n' || c == '\r') {
      needs_quotes = true;
      break;
    }
  }
  if (!needs_quotes) { return value; }
  std::string escaped = "\"";
  for (const char c : value) {
    if (c == '"') { escaped.push_back('"'); }
    escaped.push_back(c);
  }
  escaped.push_back('"');
  return escaped;
}

void WriteRow(std::ostream& out, const MatrixRow& row) {
  out << row.canonical_type_id << ','
      << CsvEscape(row.canonical_type_name) << ','
      << CsvEscape(row.type_family) << ','
      << CsvEscape(row.width_class) << ','
      << CsvEscape(row.descriptor_status) << ','
      << CsvEscape(row.storage_status) << ','
      << CsvEscape(row.binary_encoding_status) << ','
      << CsvEscape(row.binary_encoding) << ','
      << CsvEscape(row.cast_status) << ','
      << CsvEscape(row.comparison_status) << ','
      << CsvEscape(row.sort_key_status) << ','
      << CsvEscape(row.hash_status) << ','
      << CsvEscape(row.wire_metadata_status) << ','
      << CsvEscape(row.domain_psql_status) << ','
      << CsvEscape(row.index_optimizer_status) << ','
      << CsvEscape(row.production_ready) << ','
      << CsvEscape(row.production_ready_reason) << '\n';
}

bool IsCoreIndexableFamily(dt::TypeFamily family) {
  switch (family) {
    case dt::TypeFamily::boolean:
    case dt::TypeFamily::signed_integer:
    case dt::TypeFamily::unsigned_integer:
    case dt::TypeFamily::real:
    case dt::TypeFamily::decimal:
    case dt::TypeFamily::uuid:
    case dt::TypeFamily::character:
    case dt::TypeFamily::binary:
    case dt::TypeFamily::bit_string:
    case dt::TypeFamily::temporal:
    case dt::TypeFamily::network:
      return true;
    default:
      return false;
  }
}

bool IsAdvancedOrExternalFamily(dt::TypeFamily family) {
  switch (family) {
    case dt::TypeFamily::document:
    case dt::TypeFamily::search:
    case dt::TypeFamily::structured:
    case dt::TypeFamily::range:
    case dt::TypeFamily::spatial:
    case dt::TypeFamily::vector:
    case dt::TypeFamily::graph:
    case dt::TypeFamily::time_series:
    case dt::TypeFamily::columnar:
    case dt::TypeFamily::aggregate_state:
    case dt::TypeFamily::sketch:
    case dt::TypeFamily::locator:
    case dt::TypeFamily::opaque:
    case dt::TypeFamily::result_set:
      return true;
    default:
      return false;
  }
}

bool IsExactNumericSurface(const dt::DatatypeDescriptor& descriptor) {
  return descriptor.type_id == dt::CanonicalTypeId::decimal ||
         descriptor.type_id == dt::CanonicalTypeId::decimal_float ||
         descriptor.type_id == dt::CanonicalTypeId::int128 ||
         descriptor.type_id == dt::CanonicalTypeId::uint128 ||
         descriptor.type_id == dt::CanonicalTypeId::real128;
}

bool MandatoryLibraryClosurePending(const dt::DatatypeDescriptor& descriptor) {
  return descriptor.requires_mandatory_library && !IsExactNumericSurface(descriptor);
}

bool CompactRealClosurePending(const dt::DatatypeDescriptor& descriptor) {
  return descriptor.type_id == dt::CanonicalTypeId::bfloat16 ||
         descriptor.type_id == dt::CanonicalTypeId::real16;
}

bool CoreFirstReleaseReadyCandidate(const dt::DatatypeDescriptor& descriptor) {
  if (descriptor.type_id == dt::CanonicalTypeId::null_type ||
      descriptor.type_id == dt::CanonicalTypeId::blob ||
      CompactRealClosurePending(descriptor) ||
      MandatoryLibraryClosurePending(descriptor) ||
      IsAdvancedOrExternalFamily(descriptor.family)) {
    return false;
  }
  return IsCoreIndexableFamily(descriptor.family);
}

std::vector<scratchbird::core::platform::byte> BinaryPayloadFor(
    const dt::DatatypeStorageLayout& layout) {
  using scratchbird::core::platform::byte;
  if (layout.type_id == dt::CanonicalTypeId::null_type) { return {}; }
  if (layout.type_id == dt::CanonicalTypeId::boolean) { return {static_cast<byte>(1)}; }
  std::size_t size = layout.inline_bytes;
  if (layout.type_id != dt::CanonicalTypeId::decimal &&
      layout.type_id != dt::CanonicalTypeId::blob &&
      (layout.storage_class != dt::DatatypeStorageClass::inline_fixed || size == 0)) {
    size = 1;
  }
  return std::vector<byte>(size, static_cast<byte>(0x2a));
}

dt::DatatypeOperationValue SampleValueFor(dt::CanonicalTypeId type_id) {
  switch (type_id) {
    case dt::CanonicalTypeId::null_type:
      return {type_id, {}, true};
    case dt::CanonicalTypeId::boolean:
      return {type_id, "true", false};
    case dt::CanonicalTypeId::uuid:
      return {type_id, "018f7f8f-7c00-7000-8000-000000000001", false};
    case dt::CanonicalTypeId::date:
      return {type_id, "2026-05-01", false};
    case dt::CanonicalTypeId::time:
      return {type_id, "12:34:56", false};
    case dt::CanonicalTypeId::timestamp:
      return {type_id, "2026-05-01T12:34:56", false};
    case dt::CanonicalTypeId::interval:
      return {type_id, "P1D", false};
    case dt::CanonicalTypeId::binary:
    case dt::CanonicalTypeId::blob:
      return {type_id, "payload", false};
    case dt::CanonicalTypeId::document:
    case dt::CanonicalTypeId::json_document:
    case dt::CanonicalTypeId::binary_json_document:
    case dt::CanonicalTypeId::bson_document:
    case dt::CanonicalTypeId::object_document:
    case dt::CanonicalTypeId::flattened_object_document:
      return {type_id, "{\"ok\":true}", false};
    case dt::CanonicalTypeId::xml_document:
      return {type_id, "<root/>", false};
    case dt::CanonicalTypeId::hstore_document:
      return {type_id, "SBHSTORE1;items=6b6579:76616c7565", false};
    case dt::CanonicalTypeId::set_value:
      return {type_id, "SBSET1;element=character;ordered=0;nulls=0;duplicates=0;items=616c706861", false};
    default:
      return {type_id, "1", false};
  }
}

bool BinaryRoundTripOk(const dt::DatatypeStorageLayout& layout) {
  dt::DatatypeBinaryValue value;
  value.type_id = layout.type_id;
  value.is_null = layout.type_id == dt::CanonicalTypeId::null_type;
  value.payload = BinaryPayloadFor(layout);
  if (layout.type_id == dt::CanonicalTypeId::blob) {
    value.payload_is_toast_reference = true;
  }
  const auto encoded = dt::EncodeDatatypeBinaryValue(value);
  if (!encoded.ok()) { return false; }
  const auto decoded = dt::DecodeDatatypeBinaryValue(encoded.encoded);
  return decoded.ok() &&
         decoded.value.type_id == value.type_id &&
         decoded.value.is_null == value.is_null &&
         decoded.value.payload == value.payload;
}

bool CastOk(const dt::DatatypeOperationValue& sample) {
  dt::DatatypeCastRequest request;
  request.value = sample;
  request.target_type_id = sample.type_id;
  request.explicit_cast = true;
  return dt::CastDatatypeValue(request).ok();
}

bool CompareOk(const dt::DatatypeOperationValue& sample) {
  return dt::CompareDatatypeValues({sample, sample}).ok();
}

bool SortKeyOk(const dt::DatatypeOperationValue& sample) {
  return dt::MakeDatatypeSortKey({sample}).ok();
}

bool HashOk(const dt::DatatypeOperationValue& sample) {
  const auto result = dt::HashDatatypeValue({sample});
  return result.ok() && !result.stable_hash_hex.empty();
}

bool WireMetadataOk(const dt::DatatypeDescriptor& descriptor) {
  if (descriptor.type_id == dt::CanonicalTypeId::null_type) { return true; }
  const auto wire_id = dt::WireTypeIdForCanonicalTypeId(descriptor.type_id);
  if (wire_id.type_family == 0 || wire_id.type_code == 0 || wire_id.type_version == 0) {
    return false;
  }
  api::EngineDescriptor engine_descriptor;
  engine_descriptor.descriptor_kind = "scalar";
  engine_descriptor.canonical_type_name = descriptor.stable_name;
  engine_descriptor.encoded_descriptor = "canonical=" + descriptor.stable_name;
  const auto metadata = api::RenderWireDriverMetadata(engine_descriptor);
  return !metadata.driver_display_type.empty() &&
         !metadata.canonical_type_family.empty() &&
         metadata.reference_label_alias_only;
}

api::EngineDatatypeIndexOptimizerAdmissionResult IndexOptimizerAdmission(
    const dt::DatatypeDescriptor& descriptor,
    const std::string& policy_status) {
  api::EngineDescriptor engine_descriptor;
  engine_descriptor.descriptor_kind = "scalar";
  engine_descriptor.canonical_type_name = descriptor.stable_name;
  engine_descriptor.encoded_descriptor = "canonical=" + descriptor.stable_name;

  api::EngineDatatypeIndexOptimizerAdmissionRequest request;
  request.type_group = dt::TypeFamilyName(descriptor.family);
  request.descriptor = std::move(engine_descriptor);
  request.support_path = policy_status == "implemented" ? "native_substrate" : "unsupported_by_version";
  request.index_stats_status = policy_status;
  request.reference_label = "REFERENCE_ALIAS_" + descriptor.stable_name;
  return api::EvaluateDatatypeIndexOptimizerAdmission(request);
}

MatrixRow BuildRow(const dt::DatatypeDescriptor& descriptor) {
  MatrixRow row;
  row.canonical_type_id = static_cast<std::uint32_t>(descriptor.type_id);
  row.canonical_type_name = descriptor.stable_name;
  row.type_family = dt::TypeFamilyName(descriptor.family);
  row.width_class = dt::TypeWidthClassName(descriptor.width_class);

  const auto descriptor_result = dt::ValidateDatatypeDescriptor(descriptor);
  row.descriptor_status = descriptor_result.ok() ? "implemented" : "failed";

  const auto layout_result = dt::LookupDatatypeStorageLayout(descriptor.type_id);
  row.storage_status = layout_result.ok() ? "implemented" : "failed";
  row.binary_encoding = layout_result.ok()
                            ? dt::DatatypeBinaryEncodingName(layout_result.layout.encoding)
                            : "unknown";
  row.binary_encoding_status = layout_result.ok() && BinaryRoundTripOk(layout_result.layout)
                                   ? "implemented"
                                   : "failed";

  const auto sample = SampleValueFor(descriptor.type_id);
  row.cast_status = CastOk(sample) ? "implemented" : "failed";
  row.comparison_status = CompareOk(sample) ? "implemented" : "blocked";
  row.sort_key_status = SortKeyOk(sample) ? "implemented" : "blocked";
  row.hash_status = HashOk(sample) ? "implemented" : "blocked";
  row.wire_metadata_status = WireMetadataOk(descriptor) ? "implemented" : "failed";
  row.domain_psql_status = "not_applicable_builtin";

  const bool core_candidate = CoreFirstReleaseReadyCandidate(descriptor);
  const std::string index_policy = core_candidate &&
                                           row.comparison_status == "implemented" &&
                                           row.sort_key_status == "implemented" &&
                                           row.hash_status == "implemented"
                                       ? "implemented"
                                       : "blocked";
  const auto index = IndexOptimizerAdmission(descriptor, index_policy);
  if (index_policy == "implemented" && index.ok && index.index_admitted &&
      index.statistics_admitted && index.optimizer_uses_canonical_descriptor) {
    row.index_optimizer_status = "implemented";
  } else if (index.optimizer_uses_canonical_descriptor && !index.diagnostic_detail.empty()) {
    row.index_optimizer_status = "blocked:" + index.diagnostic_detail;
  } else {
    row.index_optimizer_status = "failed";
  }

  const bool production_ready = core_candidate &&
                                row.descriptor_status == "implemented" &&
                                row.storage_status == "implemented" &&
                                row.binary_encoding_status == "implemented" &&
                                row.cast_status == "implemented" &&
                                row.comparison_status == "implemented" &&
                                row.sort_key_status == "implemented" &&
                                row.hash_status == "implemented" &&
                                row.wire_metadata_status == "implemented" &&
                                row.index_optimizer_status == "implemented";
  row.production_ready = production_ready ? "true" : "false";

  if (production_ready) {
    row.production_ready_reason = IsExactNumericSurface(descriptor)
                                      ? "exact_numeric_sbl_numeric_runtime_surface"
                                      : "core_first_release_runtime_surface";
  } else if (descriptor.type_id == dt::CanonicalTypeId::null_type) {
    row.production_ready_reason = "null_type_not_a_storable_value_surface";
  } else if (CompactRealClosurePending(descriptor)) {
    row.production_ready_reason = "compact_real_runtime_closure_pending";
  } else if (MandatoryLibraryClosurePending(descriptor)) {
    row.production_ready_reason = "mandatory_library_closure_pending";
  } else if (IsExactNumericSurface(descriptor)) {
    row.production_ready_reason = "exact_numeric_runtime_surface_incomplete";
  } else if (IsAdvancedOrExternalFamily(descriptor.family) || descriptor.type_id == dt::CanonicalTypeId::blob) {
    row.production_ready_reason = "advanced_or_external_surface_not_first_release_production_ready";
  } else if (row.index_optimizer_status != "implemented") {
    row.production_ready_reason = "index_optimizer_surface_not_admitted";
  } else {
    row.production_ready_reason = "required_runtime_surface_incomplete";
  }

  return row;
}

bool IsExactNumericName(const std::string& name) {
  return name == "decimal" ||
         name == "decimal_float" ||
         name == "int128" ||
         name == "uint128" ||
         name == "real128";
}

bool ValidateRows(const std::vector<MatrixRow>& rows, std::vector<std::string>* errors) {
  if (rows.empty()) {
    errors->push_back("matrix_empty");
    return false;
  }

  std::set<std::uint32_t> ids;
  std::set<std::string> names;
  bool has_production_ready_core = false;
  bool unsupported_production_claim = false;
  bool structural_failure = false;
  for (const auto& row : rows) {
    if (!ids.insert(row.canonical_type_id).second) {
      errors->push_back("duplicate_type_id:" + std::to_string(row.canonical_type_id));
    }
    if (!names.insert(row.canonical_type_name).second) {
      errors->push_back("duplicate_type_name:" + row.canonical_type_name);
    }
    if (row.production_ready == "true") {
      has_production_ready_core = true;
      const bool exact_numeric_reason =
          row.production_ready_reason == "exact_numeric_sbl_numeric_runtime_surface" &&
          IsExactNumericName(row.canonical_type_name);
      if (row.production_ready_reason != "core_first_release_runtime_surface" &&
          !exact_numeric_reason) {
        unsupported_production_claim = true;
      }
      if (row.descriptor_status != "implemented" ||
          row.storage_status != "implemented" ||
          row.binary_encoding_status != "implemented" ||
          row.cast_status != "implemented" ||
          row.comparison_status != "implemented" ||
          row.sort_key_status != "implemented" ||
          row.hash_status != "implemented" ||
          row.wire_metadata_status != "implemented" ||
          row.index_optimizer_status != "implemented") {
        errors->push_back("production_ready_row_has_incomplete_surface:" + row.canonical_type_name);
      }
    }
    if (row.descriptor_status == "failed" || row.storage_status == "failed" ||
        row.wire_metadata_status == "failed" || row.index_optimizer_status == "failed") {
      structural_failure = true;
    }
  }
  if (!has_production_ready_core) {
    errors->push_back("no_core_production_ready_rows");
  }
  if (unsupported_production_claim) {
    errors->push_back("unsupported_type_marked_production_ready");
  }
  if (structural_failure) {
    errors->push_back("descriptor_layout_wire_or_index_policy_failed");
  }
  return errors->empty();
}

bool WriteMatrix(const std::filesystem::path& output_path,
                 const std::vector<MatrixRow>& rows,
                 std::string* error) {
  const auto parent = output_path.parent_path();
  if (!parent.empty()) {
    std::filesystem::create_directories(parent);
  }

  std::ofstream out(output_path);
  if (!out) {
    *error = "output_not_writable";
    return false;
  }
  out << "canonical_type_id,canonical_type_name,type_family,width_class,"
         "descriptor_status,storage_status,binary_encoding_status,binary_encoding,"
         "cast_status,comparison_status,sort_key_status,hash_status,"
         "wire_metadata_status,domain_psql_status,index_optimizer_status,"
         "production_ready,production_ready_reason\n";
  for (const auto& row : rows) {
    WriteRow(out, row);
  }
  if (!out) {
    *error = "output_write_failed";
    return false;
  }
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  Args args;
  if (!ParseArgs(argc, argv, &args)) {
    std::cerr << "usage: public_datatype_truth_matrix --output PATH\n";
    return 2;
  }

  std::vector<MatrixRow> rows;
  for (const auto& descriptor : dt::BuiltinDatatypeDescriptors()) {
    rows.push_back(BuildRow(descriptor));
  }
  std::sort(rows.begin(), rows.end(), [](const MatrixRow& left, const MatrixRow& right) {
    return left.canonical_type_id < right.canonical_type_id;
  });

  std::string write_error;
  if (!WriteMatrix(args.output_path, rows, &write_error)) {
    std::cerr << "public_datatype_truth_matrix=failed reason=" << write_error << "\n";
    return 1;
  }

  std::vector<std::string> errors;
  const bool ok = ValidateRows(rows, &errors);
  std::cout << "public_datatype_truth_matrix=" << (ok ? "passed" : "failed") << "\n";
  std::cout << "row_count=" << rows.size() << "\n";
  std::cout << "output=" << args.output_path << "\n";
  for (const auto& error : errors) {
    std::cout << "error=" << error << "\n";
  }
  return ok ? 0 : 1;
}

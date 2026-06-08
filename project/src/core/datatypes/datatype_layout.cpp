// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "datatype_layout.hpp"

#include <utility>
#include <vector>

namespace scratchbird::core::datatypes {
namespace {

using scratchbird::core::platform::DiagnosticArgument;
using scratchbird::core::platform::MakeDiagnostic;
using scratchbird::core::platform::Severity;
using scratchbird::core::platform::StatusCode;
using scratchbird::core::platform::Subsystem;

Status LayoutOkStatus() {
  return {StatusCode::ok, Severity::info, Subsystem::datatypes};
}

Status LayoutErrorStatus() {
  return {StatusCode::platform_required_feature_missing, Severity::error, Subsystem::datatypes};
}

DatatypeStorageLayout Layout(CanonicalTypeId type_id,
                             DatatypeStorageClass storage_class,
                             DatatypeBinaryEncoding encoding,
                             u32 inline_bytes,
                             u32 alignment_bytes,
                             bool requires_descriptor = false,
                             bool requires_charset = false,
                             bool requires_collation = false,
                             bool requires_timezone = false,
                             bool may_overflow_to_toast = false,
                             bool fixed_sort_key = false,
                             std::string notes = {}) {
  DatatypeStorageLayout layout;
  layout.type_id = type_id;
  layout.storage_class = storage_class;
  layout.encoding = encoding;
  layout.inline_bytes = inline_bytes;
  layout.alignment_bytes = alignment_bytes;
  layout.nullable = true;
  layout.requires_descriptor = requires_descriptor;
  layout.requires_charset = requires_charset;
  layout.requires_collation = requires_collation;
  layout.requires_timezone = requires_timezone;
  layout.may_overflow_to_toast = may_overflow_to_toast;
  layout.fixed_sort_key = fixed_sort_key;
  layout.notes = std::move(notes);
  return layout;
}

DatatypeStorageLayoutResult LayoutError(std::string diagnostic_code,
                                        std::string message_key,
                                        std::string detail = {}) {
  DatatypeStorageLayoutResult result;
  result.status = LayoutErrorStatus();
  result.diagnostic = MakeDatatypeLayoutDiagnostic(result.status,
                                                   std::move(diagnostic_code),
                                                   std::move(message_key),
                                                   std::move(detail));
  return result;
}

}  // namespace

const char* DatatypeStorageClassName(DatatypeStorageClass storage_class) {
  switch (storage_class) {
    case DatatypeStorageClass::inline_fixed: return "inline_fixed";
    case DatatypeStorageClass::inline_variable: return "inline_variable";
    case DatatypeStorageClass::descriptor_payload: return "descriptor_payload";
    case DatatypeStorageClass::toast_reference: return "toast_reference";
    case DatatypeStorageClass::unknown: return "unknown";
  }
  return "unknown";
}

const char* DatatypeBinaryEncodingName(DatatypeBinaryEncoding encoding) {
  switch (encoding) {
    case DatatypeBinaryEncoding::none: return "none";
    case DatatypeBinaryEncoding::twos_complement_little_endian: return "twos_complement_little_endian";
    case DatatypeBinaryEncoding::unsigned_little_endian: return "unsigned_little_endian";
    case DatatypeBinaryEncoding::ieee754_binary32_little_endian: return "ieee754_binary32_little_endian";
    case DatatypeBinaryEncoding::ieee754_binary64_little_endian: return "ieee754_binary64_little_endian";
    case DatatypeBinaryEncoding::ieee754_binary128_little_endian: return "ieee754_binary128_little_endian";
    case DatatypeBinaryEncoding::decimal128_descriptor: return "decimal128_descriptor";
    case DatatypeBinaryEncoding::uuid_16_bytes: return "uuid_16_bytes";
    case DatatypeBinaryEncoding::utf8_or_descriptor_charset_bytes: return "utf8_or_descriptor_charset_bytes";
    case DatatypeBinaryEncoding::opaque_bytes: return "opaque_bytes";
    case DatatypeBinaryEncoding::bit_packed_bytes: return "bit_packed_bytes";
    case DatatypeBinaryEncoding::network_address_binary: return "network_address_binary";
    case DatatypeBinaryEncoding::days_since_unix_epoch_i32: return "days_since_unix_epoch_i32";
    case DatatypeBinaryEncoding::nanoseconds_since_midnight_u64: return "nanoseconds_since_midnight_u64";
    case DatatypeBinaryEncoding::timestamp_utc_tuple: return "timestamp_utc_tuple";
    case DatatypeBinaryEncoding::interval_tuple: return "interval_tuple";
    case DatatypeBinaryEncoding::toast_locator: return "toast_locator";
    case DatatypeBinaryEncoding::structured_canonical_binary: return "structured_canonical_binary";
    case DatatypeBinaryEncoding::search_canonical_binary: return "search_canonical_binary";
    case DatatypeBinaryEncoding::spatial_canonical_binary: return "spatial_canonical_binary";
    case DatatypeBinaryEncoding::vector_canonical_binary: return "vector_canonical_binary";
    case DatatypeBinaryEncoding::graph_canonical_binary: return "graph_canonical_binary";
    case DatatypeBinaryEncoding::time_series_canonical_binary: return "time_series_canonical_binary";
    case DatatypeBinaryEncoding::columnar_segment_descriptor: return "columnar_segment_descriptor";
    case DatatypeBinaryEncoding::aggregate_state_canonical_binary: return "aggregate_state_canonical_binary";
    case DatatypeBinaryEncoding::sketch_canonical_binary: return "sketch_canonical_binary";
    case DatatypeBinaryEncoding::locator_envelope: return "locator_envelope";
    case DatatypeBinaryEncoding::opaque_extension_binary: return "opaque_extension_binary";
    case DatatypeBinaryEncoding::result_set_descriptor: return "result_set_descriptor";
    case DatatypeBinaryEncoding::unknown: return "unknown";
  }
  return "unknown";
}

const std::vector<DatatypeStorageLayout>& BuiltinDatatypeStorageLayouts() {
  static const std::vector<DatatypeStorageLayout> layouts = {
      Layout(CanonicalTypeId::null_type, DatatypeStorageClass::inline_fixed, DatatypeBinaryEncoding::none, 0, 1, false, false, false, false, false, true),
      Layout(CanonicalTypeId::boolean, DatatypeStorageClass::inline_fixed, DatatypeBinaryEncoding::unsigned_little_endian, 1, 1, false, false, false, false, false, true),
      Layout(CanonicalTypeId::int8, DatatypeStorageClass::inline_fixed, DatatypeBinaryEncoding::twos_complement_little_endian, 1, 1, false, false, false, false, false, true),
      Layout(CanonicalTypeId::int16, DatatypeStorageClass::inline_fixed, DatatypeBinaryEncoding::twos_complement_little_endian, 2, 2, false, false, false, false, false, true),
      Layout(CanonicalTypeId::int32, DatatypeStorageClass::inline_fixed, DatatypeBinaryEncoding::twos_complement_little_endian, 4, 4, false, false, false, false, false, true),
      Layout(CanonicalTypeId::int64, DatatypeStorageClass::inline_fixed, DatatypeBinaryEncoding::twos_complement_little_endian, 8, 8, false, false, false, false, false, true),
      Layout(CanonicalTypeId::int128, DatatypeStorageClass::inline_fixed, DatatypeBinaryEncoding::twos_complement_little_endian, 16, 16, false, false, false, false, false, true),
      Layout(CanonicalTypeId::uint8, DatatypeStorageClass::inline_fixed, DatatypeBinaryEncoding::unsigned_little_endian, 1, 1, false, false, false, false, false, true),
      Layout(CanonicalTypeId::uint16, DatatypeStorageClass::inline_fixed, DatatypeBinaryEncoding::unsigned_little_endian, 2, 2, false, false, false, false, false, true),
      Layout(CanonicalTypeId::uint32, DatatypeStorageClass::inline_fixed, DatatypeBinaryEncoding::unsigned_little_endian, 4, 4, false, false, false, false, false, true),
      Layout(CanonicalTypeId::uint64, DatatypeStorageClass::inline_fixed, DatatypeBinaryEncoding::unsigned_little_endian, 8, 8, false, false, false, false, false, true),
      Layout(CanonicalTypeId::uint128, DatatypeStorageClass::inline_fixed, DatatypeBinaryEncoding::unsigned_little_endian, 16, 16, false, false, false, false, false, true),
      Layout(CanonicalTypeId::bfloat16, DatatypeStorageClass::inline_fixed, DatatypeBinaryEncoding::opaque_bytes, 2, 2),
      Layout(CanonicalTypeId::real16, DatatypeStorageClass::inline_fixed, DatatypeBinaryEncoding::opaque_bytes, 2, 2),
      Layout(CanonicalTypeId::real32, DatatypeStorageClass::inline_fixed, DatatypeBinaryEncoding::ieee754_binary32_little_endian, 4, 4),
      Layout(CanonicalTypeId::real64, DatatypeStorageClass::inline_fixed, DatatypeBinaryEncoding::ieee754_binary64_little_endian, 8, 8),
      Layout(CanonicalTypeId::real128, DatatypeStorageClass::inline_fixed, DatatypeBinaryEncoding::ieee754_binary128_little_endian, 16, 16),
      Layout(CanonicalTypeId::decimal, DatatypeStorageClass::descriptor_payload, DatatypeBinaryEncoding::decimal128_descriptor, 16, 16, true),
      Layout(CanonicalTypeId::decimal_float, DatatypeStorageClass::descriptor_payload, DatatypeBinaryEncoding::decimal128_descriptor, 16, 16, true),
      Layout(CanonicalTypeId::uuid, DatatypeStorageClass::inline_fixed, DatatypeBinaryEncoding::uuid_16_bytes, 16, 16, false, false, false, false, false, true),
      Layout(CanonicalTypeId::ip_address, DatatypeStorageClass::inline_fixed, DatatypeBinaryEncoding::network_address_binary, 16, 16, true, false, false, false, false, true),
      Layout(CanonicalTypeId::network_prefix, DatatypeStorageClass::inline_fixed, DatatypeBinaryEncoding::network_address_binary, 18, 2, true, false, false, false, false, true),
      Layout(CanonicalTypeId::mac_address, DatatypeStorageClass::inline_fixed, DatatypeBinaryEncoding::network_address_binary, 8, 8, true, false, false, false, false, true),
      Layout(CanonicalTypeId::character, DatatypeStorageClass::inline_variable, DatatypeBinaryEncoding::utf8_or_descriptor_charset_bytes, 0, 1, true, true, true, false, true),
      Layout(CanonicalTypeId::binary, DatatypeStorageClass::inline_variable, DatatypeBinaryEncoding::opaque_bytes, 0, 1, true, false, false, false, true),
      Layout(CanonicalTypeId::bit_string, DatatypeStorageClass::descriptor_payload, DatatypeBinaryEncoding::bit_packed_bytes, 0, 1, true, false, false, false, true),
      Layout(CanonicalTypeId::date, DatatypeStorageClass::inline_fixed, DatatypeBinaryEncoding::days_since_unix_epoch_i32, 4, 4, false, false, false, false, false, true),
      Layout(CanonicalTypeId::time, DatatypeStorageClass::inline_fixed, DatatypeBinaryEncoding::nanoseconds_since_midnight_u64, 8, 8, false, false, false, true, false, true),
      Layout(CanonicalTypeId::timestamp, DatatypeStorageClass::inline_fixed, DatatypeBinaryEncoding::timestamp_utc_tuple, 16, 8, false, false, false, true, false, true),
      Layout(CanonicalTypeId::interval, DatatypeStorageClass::inline_fixed, DatatypeBinaryEncoding::interval_tuple, 16, 8, true),
      Layout(CanonicalTypeId::blob, DatatypeStorageClass::toast_reference, DatatypeBinaryEncoding::toast_locator, 24, 8, true, false, false, false, true),
      Layout(CanonicalTypeId::document, DatatypeStorageClass::descriptor_payload, DatatypeBinaryEncoding::structured_canonical_binary, 0, 8, true, false, false, false, true),
      Layout(CanonicalTypeId::json_document, DatatypeStorageClass::descriptor_payload, DatatypeBinaryEncoding::structured_canonical_binary, 0, 8, true, false, false, false, true),
      Layout(CanonicalTypeId::binary_json_document, DatatypeStorageClass::descriptor_payload, DatatypeBinaryEncoding::structured_canonical_binary, 0, 8, true, false, false, false, true),
      Layout(CanonicalTypeId::bson_document, DatatypeStorageClass::descriptor_payload, DatatypeBinaryEncoding::structured_canonical_binary, 0, 8, true, false, false, false, true),
      Layout(CanonicalTypeId::xml_document, DatatypeStorageClass::descriptor_payload, DatatypeBinaryEncoding::structured_canonical_binary, 0, 8, true, false, false, false, true),
      Layout(CanonicalTypeId::hstore_document, DatatypeStorageClass::descriptor_payload, DatatypeBinaryEncoding::structured_canonical_binary, 0, 8, true, false, false, false, true),
      Layout(CanonicalTypeId::object_document, DatatypeStorageClass::descriptor_payload, DatatypeBinaryEncoding::structured_canonical_binary, 0, 8, true, false, false, false, true),
      Layout(CanonicalTypeId::flattened_object_document, DatatypeStorageClass::descriptor_payload, DatatypeBinaryEncoding::structured_canonical_binary, 0, 8, true, false, false, false, true),
      Layout(CanonicalTypeId::enum_value, DatatypeStorageClass::inline_fixed, DatatypeBinaryEncoding::uuid_16_bytes, 16, 16, true, false, false, false, false, true),
      Layout(CanonicalTypeId::set_value, DatatypeStorageClass::descriptor_payload, DatatypeBinaryEncoding::structured_canonical_binary, 0, 8, true, false, false, false, true),
      Layout(CanonicalTypeId::array, DatatypeStorageClass::descriptor_payload, DatatypeBinaryEncoding::structured_canonical_binary, 0, 8, true, false, false, false, true),
      Layout(CanonicalTypeId::list, DatatypeStorageClass::descriptor_payload, DatatypeBinaryEncoding::structured_canonical_binary, 0, 8, true, false, false, false, true),
      Layout(CanonicalTypeId::map, DatatypeStorageClass::descriptor_payload, DatatypeBinaryEncoding::structured_canonical_binary, 0, 8, true, false, false, false, true),
      Layout(CanonicalTypeId::row, DatatypeStorageClass::descriptor_payload, DatatypeBinaryEncoding::structured_canonical_binary, 0, 8, true, false, false, false, true),
      Layout(CanonicalTypeId::composite, DatatypeStorageClass::descriptor_payload, DatatypeBinaryEncoding::structured_canonical_binary, 0, 8, true, false, false, false, true),
      Layout(CanonicalTypeId::variant, DatatypeStorageClass::descriptor_payload, DatatypeBinaryEncoding::structured_canonical_binary, 0, 8, true, false, false, false, true),
      Layout(CanonicalTypeId::range, DatatypeStorageClass::descriptor_payload, DatatypeBinaryEncoding::structured_canonical_binary, 0, 8, true, false, false, false, true),
      Layout(CanonicalTypeId::multirange, DatatypeStorageClass::descriptor_payload, DatatypeBinaryEncoding::structured_canonical_binary, 0, 8, true, false, false, false, true),
      Layout(CanonicalTypeId::token_stream, DatatypeStorageClass::descriptor_payload, DatatypeBinaryEncoding::search_canonical_binary, 0, 8, true, false, false, false, true),
      Layout(CanonicalTypeId::search_query, DatatypeStorageClass::descriptor_payload, DatatypeBinaryEncoding::search_canonical_binary, 0, 8, true, false, false, false, true),
      Layout(CanonicalTypeId::search_rank_feature, DatatypeStorageClass::descriptor_payload, DatatypeBinaryEncoding::search_canonical_binary, 0, 8, true, false, false, false, true),
      Layout(CanonicalTypeId::search_completion, DatatypeStorageClass::descriptor_payload, DatatypeBinaryEncoding::search_canonical_binary, 0, 8, true, false, false, false, true),
      Layout(CanonicalTypeId::search_percolator, DatatypeStorageClass::descriptor_payload, DatatypeBinaryEncoding::search_canonical_binary, 0, 8, true, false, false, false, true),
      Layout(CanonicalTypeId::geometry, DatatypeStorageClass::descriptor_payload, DatatypeBinaryEncoding::spatial_canonical_binary, 0, 8, true, false, false, false, true),
      Layout(CanonicalTypeId::geography, DatatypeStorageClass::descriptor_payload, DatatypeBinaryEncoding::spatial_canonical_binary, 0, 8, true, false, false, false, true),
      Layout(CanonicalTypeId::point, DatatypeStorageClass::descriptor_payload, DatatypeBinaryEncoding::spatial_canonical_binary, 0, 8, true, false, false, false, true),
      Layout(CanonicalTypeId::shape, DatatypeStorageClass::descriptor_payload, DatatypeBinaryEncoding::spatial_canonical_binary, 0, 8, true, false, false, false, true),
      Layout(CanonicalTypeId::raster, DatatypeStorageClass::descriptor_payload, DatatypeBinaryEncoding::spatial_canonical_binary, 0, 8, true, false, false, false, true),
      Layout(CanonicalTypeId::vector, DatatypeStorageClass::descriptor_payload, DatatypeBinaryEncoding::vector_canonical_binary, 0, 16, true, false, false, false, true),
      Layout(CanonicalTypeId::dense_vector, DatatypeStorageClass::descriptor_payload, DatatypeBinaryEncoding::vector_canonical_binary, 0, 16, true, false, false, false, true),
      Layout(CanonicalTypeId::sparse_vector, DatatypeStorageClass::descriptor_payload, DatatypeBinaryEncoding::vector_canonical_binary, 0, 16, true, false, false, false, true),
      Layout(CanonicalTypeId::binary_vector, DatatypeStorageClass::descriptor_payload, DatatypeBinaryEncoding::vector_canonical_binary, 0, 16, true, false, false, false, true),
      Layout(CanonicalTypeId::quantized_vector, DatatypeStorageClass::descriptor_payload, DatatypeBinaryEncoding::vector_canonical_binary, 0, 16, true, false, false, false, true),
      Layout(CanonicalTypeId::graph_node, DatatypeStorageClass::descriptor_payload, DatatypeBinaryEncoding::graph_canonical_binary, 0, 8, true, false, false, false, true),
      Layout(CanonicalTypeId::graph_edge, DatatypeStorageClass::descriptor_payload, DatatypeBinaryEncoding::graph_canonical_binary, 0, 8, true, false, false, false, true),
      Layout(CanonicalTypeId::graph_path, DatatypeStorageClass::descriptor_payload, DatatypeBinaryEncoding::graph_canonical_binary, 0, 8, true, false, false, false, true),
      Layout(CanonicalTypeId::time_series_value, DatatypeStorageClass::descriptor_payload, DatatypeBinaryEncoding::time_series_canonical_binary, 0, 8, true, false, false, true, true),
      Layout(CanonicalTypeId::columnar_segment, DatatypeStorageClass::descriptor_payload, DatatypeBinaryEncoding::columnar_segment_descriptor, 0, 8, true, false, false, false, true),
      Layout(CanonicalTypeId::aggregate_state, DatatypeStorageClass::descriptor_payload, DatatypeBinaryEncoding::aggregate_state_canonical_binary, 0, 8, true, false, false, false, true),
      Layout(CanonicalTypeId::hll_sketch, DatatypeStorageClass::descriptor_payload, DatatypeBinaryEncoding::sketch_canonical_binary, 0, 8, true, false, false, false, true),
      Layout(CanonicalTypeId::bloom_filter, DatatypeStorageClass::descriptor_payload, DatatypeBinaryEncoding::sketch_canonical_binary, 0, 8, true, false, false, false, true),
      Layout(CanonicalTypeId::quantile_sketch, DatatypeStorageClass::descriptor_payload, DatatypeBinaryEncoding::sketch_canonical_binary, 0, 8, true, false, false, false, true),
      Layout(CanonicalTypeId::histogram_sketch, DatatypeStorageClass::descriptor_payload, DatatypeBinaryEncoding::sketch_canonical_binary, 0, 8, true, false, false, false, true),
      Layout(CanonicalTypeId::ranking_summary, DatatypeStorageClass::descriptor_payload, DatatypeBinaryEncoding::sketch_canonical_binary, 0, 8, true, false, false, false, true),
      Layout(CanonicalTypeId::vector_summary, DatatypeStorageClass::descriptor_payload, DatatypeBinaryEncoding::sketch_canonical_binary, 0, 8, true, false, false, false, true),
      Layout(CanonicalTypeId::lob_locator, DatatypeStorageClass::descriptor_payload, DatatypeBinaryEncoding::locator_envelope, 0, 8, true, false, false, false, false),
      Layout(CanonicalTypeId::external_file_locator, DatatypeStorageClass::descriptor_payload, DatatypeBinaryEncoding::locator_envelope, 0, 8, true, false, false, false, false),
      Layout(CanonicalTypeId::remote_object_locator, DatatypeStorageClass::descriptor_payload, DatatypeBinaryEncoding::locator_envelope, 0, 8, true, false, false, false, false),
      Layout(CanonicalTypeId::bridge_handle, DatatypeStorageClass::descriptor_payload, DatatypeBinaryEncoding::locator_envelope, 0, 8, true, false, false, false, false),
      Layout(CanonicalTypeId::cursor_handle, DatatypeStorageClass::descriptor_payload, DatatypeBinaryEncoding::locator_envelope, 0, 8, true, false, false, false, false),
      Layout(CanonicalTypeId::system_reference, DatatypeStorageClass::descriptor_payload, DatatypeBinaryEncoding::locator_envelope, 0, 8, true, false, false, false, false),
      Layout(CanonicalTypeId::opaque_extension, DatatypeStorageClass::descriptor_payload, DatatypeBinaryEncoding::opaque_extension_binary, 0, 8, true, false, false, false, true),
      Layout(CanonicalTypeId::cursor, DatatypeStorageClass::descriptor_payload, DatatypeBinaryEncoding::result_set_descriptor, 0, 8, true, false, false, false, false),
      Layout(CanonicalTypeId::result_set, DatatypeStorageClass::descriptor_payload, DatatypeBinaryEncoding::result_set_descriptor, 0, 8, true, false, false, false, false),
      Layout(CanonicalTypeId::table_value, DatatypeStorageClass::descriptor_payload, DatatypeBinaryEncoding::result_set_descriptor, 0, 8, true, false, false, false, true),
  };
  return layouts;
}

DatatypeStorageLayoutResult LookupDatatypeStorageLayout(CanonicalTypeId type_id) {
  for (const DatatypeStorageLayout& layout : BuiltinDatatypeStorageLayouts()) {
    if (layout.type_id == type_id) {
      return ValidateDatatypeStorageLayout(layout);
    }
  }
  return LayoutError("SB-DATATYPE-LAYOUT-UNKNOWN-TYPE",
                     "datatype.layout.unknown_type",
                     CanonicalTypeName(type_id));
}

DatatypeStorageLayoutResult ValidateDatatypeStorageLayout(const DatatypeStorageLayout& layout) {
  const auto descriptor = LookupDatatypeDescriptor(layout.type_id);
  if (!descriptor.ok()) {
    DatatypeStorageLayoutResult result;
    result.status = descriptor.status;
    result.diagnostic = descriptor.diagnostic;
    return result;
  }
  if (layout.storage_class == DatatypeStorageClass::unknown || layout.encoding == DatatypeBinaryEncoding::unknown) {
    return LayoutError("SB-DATATYPE-LAYOUT-INCOMPLETE",
                       "datatype.layout.incomplete",
                       CanonicalTypeName(layout.type_id));
  }
  if (layout.storage_class == DatatypeStorageClass::inline_fixed &&
      layout.type_id != CanonicalTypeId::null_type &&
      layout.inline_bytes == 0) {
    return LayoutError("SB-DATATYPE-LAYOUT-INLINE-BYTES-MISSING",
                       "datatype.layout.inline_bytes_missing",
                       CanonicalTypeName(layout.type_id));
  }
  if (layout.alignment_bytes == 0) {
    return LayoutError("SB-DATATYPE-LAYOUT-ALIGNMENT-MISSING",
                       "datatype.layout.alignment_missing",
                       CanonicalTypeName(layout.type_id));
  }

  DatatypeStorageLayoutResult result;
  result.status = LayoutOkStatus();
  result.layout = layout;
  return result;
}

DiagnosticRecord MakeDatatypeLayoutDiagnostic(Status status,
                                             std::string diagnostic_code,
                                             std::string message_key,
                                             std::string detail) {
  std::vector<DiagnosticArgument> arguments;
  if (!detail.empty()) {
    arguments.push_back({"detail", detail});
  }

  return MakeDiagnostic(status.code,
                        status.severity,
                        status.subsystem,
                        std::move(diagnostic_code),
                        std::move(message_key),
                        std::move(arguments),
                        {},
                        "core.datatypes.layout");
}

}  // namespace scratchbird::core::datatypes

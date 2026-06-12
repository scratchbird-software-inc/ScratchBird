// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "datatype_descriptor.hpp"

#include <cstdint>
#include <utility>
#include <vector>

namespace scratchbird::core::datatypes {
namespace {

using scratchbird::core::platform::CapabilityState;
using scratchbird::core::platform::DiagnosticArgument;
using scratchbird::core::platform::MakeDiagnostic;
using scratchbird::core::platform::Severity;
using scratchbird::core::platform::StatusCode;
using scratchbird::core::platform::Subsystem;

Status DatatypeOkStatus() {
  return {StatusCode::ok, Severity::info, Subsystem::datatypes};
}

Status DatatypeWarningStatus() {
  return {StatusCode::platform_required_feature_missing, Severity::warning, Subsystem::datatypes};
}

Status DatatypeErrorStatus() {
  return {StatusCode::platform_required_feature_missing, Severity::error, Subsystem::datatypes};
}

bool IsNilEngineUuid(const scratchbird::engine::Uuid& uuid) {
  for (const std::uint8_t byte : uuid.bytes) {
    if (byte != 0) {
      return false;
    }
  }
  return true;
}

scratchbird::engine::Uuid ToEngineUuid(const scratchbird::core::platform::Uuid& source) {
  scratchbird::engine::Uuid out{};
  for (std::size_t index = 0; index < source.bytes.size(); ++index) {
    out.bytes[index] = source.bytes[index];
  }
  return out;
}

scratchbird::engine::ExecutionTypeFamily ToExecutionFamily(TypeFamily family) {
  using EngineFamily = scratchbird::engine::ExecutionTypeFamily;
  switch (family) {
    case TypeFamily::null_type: return EngineFamily::null_type;
    case TypeFamily::boolean: return EngineFamily::boolean;
    case TypeFamily::signed_integer: return EngineFamily::signed_integer;
    case TypeFamily::unsigned_integer: return EngineFamily::unsigned_integer;
    case TypeFamily::real: return EngineFamily::real;
    case TypeFamily::decimal: return EngineFamily::decimal;
    case TypeFamily::uuid: return EngineFamily::uuid;
    case TypeFamily::character: return EngineFamily::character;
    case TypeFamily::binary: return EngineFamily::binary;
    case TypeFamily::bit_string: return EngineFamily::bit_string;
    case TypeFamily::temporal: return EngineFamily::temporal;
    case TypeFamily::blob: return EngineFamily::blob;
    case TypeFamily::network: return EngineFamily::network;
    case TypeFamily::document: return EngineFamily::document;
    case TypeFamily::search: return EngineFamily::search;
    case TypeFamily::structured: return EngineFamily::structured;
    case TypeFamily::range: return EngineFamily::range;
    case TypeFamily::spatial: return EngineFamily::spatial;
    case TypeFamily::vector: return EngineFamily::vector;
    case TypeFamily::graph: return EngineFamily::graph;
    case TypeFamily::time_series: return EngineFamily::time_series;
    case TypeFamily::columnar: return EngineFamily::columnar;
    case TypeFamily::aggregate_state: return EngineFamily::aggregate_state;
    case TypeFamily::sketch: return EngineFamily::sketch;
    case TypeFamily::locator: return EngineFamily::locator;
    case TypeFamily::opaque: return EngineFamily::opaque;
    case TypeFamily::result_set: return EngineFamily::result_set;
    case TypeFamily::unknown: return EngineFamily::unknown;
  }
  return EngineFamily::unknown;
}

scratchbird::engine::ExecutionTypeWidthClass ToExecutionWidthClass(TypeWidthClass width_class) {
  using EngineWidth = scratchbird::engine::ExecutionTypeWidthClass;
  switch (width_class) {
    case TypeWidthClass::fixed: return EngineWidth::fixed;
    case TypeWidthClass::variable: return EngineWidth::variable;
    case TypeWidthClass::descriptor_defined: return EngineWidth::descriptor_defined;
    case TypeWidthClass::unknown: return EngineWidth::unknown;
  }
  return EngineWidth::unknown;
}

void SetModifier(std::uint64_t* flags, scratchbird::engine::ExecutionTypeModifierFlag flag) {
  *flags |= scratchbird::engine::ExecutionTypeModifierFlagBit(flag);
}

ExecutionTypeDescriptorResult DescriptorBuildFailure(std::string diagnostic_code,
                                                     std::string message_key,
                                                     std::string detail) {
  ExecutionTypeDescriptorResult result;
  result.status = DatatypeErrorStatus();
  result.diagnostic = MakeDatatypeDiagnostic(result.status,
                                             std::move(diagnostic_code),
                                             std::move(message_key),
                                             std::move(detail));
  return result;
}

bool AttachTypedUuid(const TypedUuid& typed_uuid,
                     scratchbird::engine::ExecutionTypeModifierFlag flag,
                     scratchbird::engine::Uuid* out,
                     std::uint64_t* modifier_flags) {
  if (!typed_uuid.valid()) {
    return false;
  }
  *out = ToEngineUuid(typed_uuid.value);
  SetModifier(modifier_flags, flag);
  return true;
}

DatatypeDescriptor Descriptor(CanonicalTypeId type_id,
                              TypeFamily family,
                              TypeWidthClass width_class,
                              std::string stable_name,
                              u32 bit_width,
                              u32 default_precision = 0,
                              u32 default_scale = 0,
                              bool nullable_allowed = true,
                              bool requires_mandatory_library = false,
                              std::string required_capability_key = {}) {
  DatatypeDescriptor descriptor;
  descriptor.type_id = type_id;
  descriptor.family = family;
  descriptor.width_class = width_class;
  descriptor.stable_name = std::move(stable_name);
  descriptor.bit_width = bit_width;
  descriptor.default_precision = default_precision;
  descriptor.default_scale = default_scale;
  descriptor.nullable_allowed = nullable_allowed;
  descriptor.requires_mandatory_library = requires_mandatory_library;
  descriptor.required_capability_key = std::move(required_capability_key);
  return descriptor;
}

bool CapabilityPresent(const RuntimeCapabilityManifest& manifest, const std::string& key) {
  for (const auto& capability : manifest.capabilities) {
    if (capability.key == key && capability.state == CapabilityState::present) {
      return true;
    }
  }
  return false;
}

}  // namespace

const char* TypeFamilyName(TypeFamily family) {
  switch (family) {
    case TypeFamily::null_type: return "null_type";
    case TypeFamily::boolean: return "boolean";
    case TypeFamily::signed_integer: return "signed_integer";
    case TypeFamily::unsigned_integer: return "unsigned_integer";
    case TypeFamily::real: return "real";
    case TypeFamily::decimal: return "decimal";
    case TypeFamily::uuid: return "uuid";
    case TypeFamily::character: return "character";
    case TypeFamily::binary: return "binary";
    case TypeFamily::bit_string: return "bit_string";
    case TypeFamily::temporal: return "temporal";
    case TypeFamily::blob: return "blob";
    case TypeFamily::network: return "network";
    case TypeFamily::document: return "document";
    case TypeFamily::search: return "search";
    case TypeFamily::structured: return "structured";
    case TypeFamily::range: return "range";
    case TypeFamily::spatial: return "spatial";
    case TypeFamily::vector: return "vector";
    case TypeFamily::graph: return "graph";
    case TypeFamily::time_series: return "time_series";
    case TypeFamily::columnar: return "columnar";
    case TypeFamily::aggregate_state: return "aggregate_state";
    case TypeFamily::sketch: return "sketch";
    case TypeFamily::locator: return "locator";
    case TypeFamily::opaque: return "opaque";
    case TypeFamily::result_set: return "result_set";
    case TypeFamily::unknown: return "unknown";
  }
  return "unknown";
}

const char* CanonicalTypeName(CanonicalTypeId type_id) {
  switch (type_id) {
    case CanonicalTypeId::null_type: return "null";
    case CanonicalTypeId::boolean: return "boolean";
    case CanonicalTypeId::int8: return "int8";
    case CanonicalTypeId::int16: return "int16";
    case CanonicalTypeId::int32: return "int32";
    case CanonicalTypeId::int64: return "int64";
    case CanonicalTypeId::int128: return "int128";
    case CanonicalTypeId::uint8: return "uint8";
    case CanonicalTypeId::uint16: return "uint16";
    case CanonicalTypeId::uint32: return "uint32";
    case CanonicalTypeId::uint64: return "uint64";
    case CanonicalTypeId::uint128: return "uint128";
    case CanonicalTypeId::bfloat16: return "bfloat16";
    case CanonicalTypeId::real16: return "real16";
    case CanonicalTypeId::real32: return "real32";
    case CanonicalTypeId::real64: return "real64";
    case CanonicalTypeId::real128: return "real128";
    case CanonicalTypeId::decimal: return "decimal";
    case CanonicalTypeId::decimal_float: return "decimal_float";
    case CanonicalTypeId::uuid: return "uuid";
    case CanonicalTypeId::ip_address: return "ip_address";
    case CanonicalTypeId::network_prefix: return "network_prefix";
    case CanonicalTypeId::mac_address: return "mac_address";
    case CanonicalTypeId::character: return "character";
    case CanonicalTypeId::binary: return "binary";
    case CanonicalTypeId::bit_string: return "bit_string";
    case CanonicalTypeId::date: return "date";
    case CanonicalTypeId::time: return "time";
    case CanonicalTypeId::timestamp: return "timestamp";
    case CanonicalTypeId::interval: return "interval";
    case CanonicalTypeId::blob: return "blob";
    case CanonicalTypeId::document: return "document";
    case CanonicalTypeId::json_document: return "json_document";
    case CanonicalTypeId::binary_json_document: return "binary_json_document";
    case CanonicalTypeId::bson_document: return "bson_document";
    case CanonicalTypeId::xml_document: return "xml_document";
    case CanonicalTypeId::hstore_document: return "hstore_document";
    case CanonicalTypeId::object_document: return "object_document";
    case CanonicalTypeId::flattened_object_document: return "flattened_object_document";
    case CanonicalTypeId::enum_value: return "enum_value";
    case CanonicalTypeId::set_value: return "set_value";
    case CanonicalTypeId::array: return "array";
    case CanonicalTypeId::list: return "list";
    case CanonicalTypeId::map: return "map";
    case CanonicalTypeId::row: return "row";
    case CanonicalTypeId::composite: return "composite";
    case CanonicalTypeId::variant: return "variant";
    case CanonicalTypeId::range: return "range";
    case CanonicalTypeId::multirange: return "multirange";
    case CanonicalTypeId::token_stream: return "token_stream";
    case CanonicalTypeId::search_query: return "search_query";
    case CanonicalTypeId::search_rank_feature: return "search_rank_feature";
    case CanonicalTypeId::search_completion: return "search_completion";
    case CanonicalTypeId::search_percolator: return "search_percolator";
    case CanonicalTypeId::geometry: return "geometry";
    case CanonicalTypeId::geography: return "geography";
    case CanonicalTypeId::point: return "point";
    case CanonicalTypeId::shape: return "shape";
    case CanonicalTypeId::raster: return "raster";
    case CanonicalTypeId::vector: return "vector";
    case CanonicalTypeId::dense_vector: return "dense_vector";
    case CanonicalTypeId::sparse_vector: return "sparse_vector";
    case CanonicalTypeId::binary_vector: return "binary_vector";
    case CanonicalTypeId::quantized_vector: return "quantized_vector";
    case CanonicalTypeId::graph_node: return "graph_node";
    case CanonicalTypeId::graph_edge: return "graph_edge";
    case CanonicalTypeId::graph_path: return "graph_path";
    case CanonicalTypeId::time_series_value: return "time_series_value";
    case CanonicalTypeId::columnar_segment: return "columnar_segment";
    case CanonicalTypeId::aggregate_state: return "aggregate_state";
    case CanonicalTypeId::hll_sketch: return "hll_sketch";
    case CanonicalTypeId::bloom_filter: return "bloom_filter";
    case CanonicalTypeId::quantile_sketch: return "quantile_sketch";
    case CanonicalTypeId::histogram_sketch: return "histogram_sketch";
    case CanonicalTypeId::ranking_summary: return "ranking_summary";
    case CanonicalTypeId::vector_summary: return "vector_summary";
    case CanonicalTypeId::lob_locator: return "lob_locator";
    case CanonicalTypeId::external_file_locator: return "external_file_locator";
    case CanonicalTypeId::remote_object_locator: return "remote_object_locator";
    case CanonicalTypeId::bridge_handle: return "bridge_handle";
    case CanonicalTypeId::cursor_handle: return "cursor_handle";
    case CanonicalTypeId::system_reference: return "system_reference";
    case CanonicalTypeId::opaque_extension: return "opaque_extension";
    case CanonicalTypeId::cursor: return "cursor";
    case CanonicalTypeId::result_set: return "result_set";
    case CanonicalTypeId::table_value: return "table_value";
    case CanonicalTypeId::unknown: return "unknown";
  }
  return "unknown";
}

const char* TypeWidthClassName(TypeWidthClass width_class) {
  switch (width_class) {
    case TypeWidthClass::fixed: return "fixed";
    case TypeWidthClass::variable: return "variable";
    case TypeWidthClass::descriptor_defined: return "descriptor_defined";
    case TypeWidthClass::unknown: return "unknown";
  }
  return "unknown";
}

const std::vector<DatatypeDescriptor>& BuiltinDatatypeDescriptors() {
  static const std::vector<DatatypeDescriptor> descriptors = {
      Descriptor(CanonicalTypeId::null_type, TypeFamily::null_type, TypeWidthClass::fixed, "null", 0),
      Descriptor(CanonicalTypeId::boolean, TypeFamily::boolean, TypeWidthClass::fixed, "boolean", 1),
      Descriptor(CanonicalTypeId::int8, TypeFamily::signed_integer, TypeWidthClass::fixed, "int8", 8),
      Descriptor(CanonicalTypeId::int16, TypeFamily::signed_integer, TypeWidthClass::fixed, "int16", 16),
      Descriptor(CanonicalTypeId::int32, TypeFamily::signed_integer, TypeWidthClass::fixed, "int32", 32),
      Descriptor(CanonicalTypeId::int64, TypeFamily::signed_integer, TypeWidthClass::fixed, "int64", 64),
      Descriptor(CanonicalTypeId::int128,
                 TypeFamily::signed_integer,
                 TypeWidthClass::fixed,
                 "int128",
                 128,
                 128,
                 0,
                 true,
                 true,
                 "numeric.int128"),
      Descriptor(CanonicalTypeId::uint8, TypeFamily::unsigned_integer, TypeWidthClass::fixed, "uint8", 8),
      Descriptor(CanonicalTypeId::uint16, TypeFamily::unsigned_integer, TypeWidthClass::fixed, "uint16", 16),
      Descriptor(CanonicalTypeId::uint32, TypeFamily::unsigned_integer, TypeWidthClass::fixed, "uint32", 32),
      Descriptor(CanonicalTypeId::uint64, TypeFamily::unsigned_integer, TypeWidthClass::fixed, "uint64", 64),
      Descriptor(CanonicalTypeId::uint128,
                 TypeFamily::unsigned_integer,
                 TypeWidthClass::fixed,
                 "uint128",
                 128,
                 128,
                 0,
                 true,
                 true,
                 "numeric.uint128"),
      Descriptor(CanonicalTypeId::bfloat16, TypeFamily::real, TypeWidthClass::fixed, "bfloat16", 16),
      Descriptor(CanonicalTypeId::real16, TypeFamily::real, TypeWidthClass::fixed, "real16", 16),
      Descriptor(CanonicalTypeId::real32, TypeFamily::real, TypeWidthClass::fixed, "real32", 32),
      Descriptor(CanonicalTypeId::real64, TypeFamily::real, TypeWidthClass::fixed, "real64", 64),
      Descriptor(CanonicalTypeId::real128,
                 TypeFamily::real,
                 TypeWidthClass::fixed,
                 "real128",
                 128,
                 113,
                 0,
                 true,
                 true,
                 "numeric.real128"),
      Descriptor(CanonicalTypeId::decimal,
                 TypeFamily::decimal,
                 TypeWidthClass::descriptor_defined,
                 "decimal",
                 0,
                 38,
                 0,
                 true,
                 true,
                 "numeric.decimal"),
      Descriptor(CanonicalTypeId::decimal_float,
                 TypeFamily::decimal,
                 TypeWidthClass::descriptor_defined,
                 "decimal_float",
                 0,
                 34,
                 0,
                 true,
                 true,
                 "numeric.decimal_float"),
      Descriptor(CanonicalTypeId::uuid, TypeFamily::uuid, TypeWidthClass::fixed, "uuid", 128),
      Descriptor(CanonicalTypeId::ip_address, TypeFamily::network, TypeWidthClass::fixed, "ip_address", 128),
      Descriptor(CanonicalTypeId::network_prefix, TypeFamily::network, TypeWidthClass::fixed, "network_prefix", 144),
      Descriptor(CanonicalTypeId::mac_address, TypeFamily::network, TypeWidthClass::fixed, "mac_address", 64),
      Descriptor(CanonicalTypeId::character, TypeFamily::character, TypeWidthClass::variable, "character", 0),
      Descriptor(CanonicalTypeId::binary, TypeFamily::binary, TypeWidthClass::variable, "binary", 0),
      Descriptor(CanonicalTypeId::bit_string, TypeFamily::bit_string, TypeWidthClass::descriptor_defined, "bit_string", 0),
      Descriptor(CanonicalTypeId::date, TypeFamily::temporal, TypeWidthClass::fixed, "date", 32),
      Descriptor(CanonicalTypeId::time, TypeFamily::temporal, TypeWidthClass::fixed, "time", 64),
      Descriptor(CanonicalTypeId::timestamp, TypeFamily::temporal, TypeWidthClass::fixed, "timestamp", 128),
      Descriptor(CanonicalTypeId::interval, TypeFamily::temporal, TypeWidthClass::fixed, "interval", 128),
      Descriptor(CanonicalTypeId::blob, TypeFamily::blob, TypeWidthClass::descriptor_defined, "blob", 0),
      Descriptor(CanonicalTypeId::document, TypeFamily::document, TypeWidthClass::descriptor_defined, "document", 0),
      Descriptor(CanonicalTypeId::json_document, TypeFamily::document, TypeWidthClass::descriptor_defined, "json_document", 0),
      Descriptor(CanonicalTypeId::binary_json_document, TypeFamily::document, TypeWidthClass::descriptor_defined, "binary_json_document", 0),
      Descriptor(CanonicalTypeId::bson_document, TypeFamily::document, TypeWidthClass::descriptor_defined, "bson_document", 0),
      Descriptor(CanonicalTypeId::xml_document, TypeFamily::document, TypeWidthClass::descriptor_defined, "xml_document", 0),
      Descriptor(CanonicalTypeId::hstore_document, TypeFamily::document, TypeWidthClass::descriptor_defined, "hstore_document", 0),
      Descriptor(CanonicalTypeId::object_document, TypeFamily::document, TypeWidthClass::descriptor_defined, "object_document", 0),
      Descriptor(CanonicalTypeId::flattened_object_document, TypeFamily::document, TypeWidthClass::descriptor_defined, "flattened_object_document", 0),
      Descriptor(CanonicalTypeId::enum_value, TypeFamily::structured, TypeWidthClass::fixed, "enum_value", 128),
      Descriptor(CanonicalTypeId::set_value, TypeFamily::structured, TypeWidthClass::descriptor_defined, "set_value", 0),
      Descriptor(CanonicalTypeId::array, TypeFamily::structured, TypeWidthClass::descriptor_defined, "array", 0),
      Descriptor(CanonicalTypeId::list, TypeFamily::structured, TypeWidthClass::descriptor_defined, "list", 0),
      Descriptor(CanonicalTypeId::map, TypeFamily::structured, TypeWidthClass::descriptor_defined, "map", 0),
      Descriptor(CanonicalTypeId::row, TypeFamily::structured, TypeWidthClass::descriptor_defined, "row", 0),
      Descriptor(CanonicalTypeId::composite, TypeFamily::structured, TypeWidthClass::descriptor_defined, "composite", 0),
      Descriptor(CanonicalTypeId::variant, TypeFamily::structured, TypeWidthClass::descriptor_defined, "variant", 0),
      Descriptor(CanonicalTypeId::range, TypeFamily::range, TypeWidthClass::descriptor_defined, "range", 0),
      Descriptor(CanonicalTypeId::multirange, TypeFamily::range, TypeWidthClass::descriptor_defined, "multirange", 0),
      Descriptor(CanonicalTypeId::token_stream, TypeFamily::search, TypeWidthClass::descriptor_defined, "token_stream", 0),
      Descriptor(CanonicalTypeId::search_query, TypeFamily::search, TypeWidthClass::descriptor_defined, "search_query", 0),
      Descriptor(CanonicalTypeId::search_rank_feature, TypeFamily::search, TypeWidthClass::descriptor_defined, "search_rank_feature", 0),
      Descriptor(CanonicalTypeId::search_completion, TypeFamily::search, TypeWidthClass::descriptor_defined, "search_completion", 0),
      Descriptor(CanonicalTypeId::search_percolator, TypeFamily::search, TypeWidthClass::descriptor_defined, "search_percolator", 0),
      Descriptor(CanonicalTypeId::geometry, TypeFamily::spatial, TypeWidthClass::descriptor_defined, "geometry", 0),
      Descriptor(CanonicalTypeId::geography, TypeFamily::spatial, TypeWidthClass::descriptor_defined, "geography", 0),
      Descriptor(CanonicalTypeId::point, TypeFamily::spatial, TypeWidthClass::descriptor_defined, "point", 0),
      Descriptor(CanonicalTypeId::shape, TypeFamily::spatial, TypeWidthClass::descriptor_defined, "shape", 0),
      Descriptor(CanonicalTypeId::raster, TypeFamily::spatial, TypeWidthClass::descriptor_defined, "raster", 0),
      Descriptor(CanonicalTypeId::vector, TypeFamily::vector, TypeWidthClass::descriptor_defined, "vector", 0),
      Descriptor(CanonicalTypeId::dense_vector, TypeFamily::vector, TypeWidthClass::descriptor_defined, "dense_vector", 0),
      Descriptor(CanonicalTypeId::sparse_vector, TypeFamily::vector, TypeWidthClass::descriptor_defined, "sparse_vector", 0),
      Descriptor(CanonicalTypeId::binary_vector, TypeFamily::vector, TypeWidthClass::descriptor_defined, "binary_vector", 0),
      Descriptor(CanonicalTypeId::quantized_vector, TypeFamily::vector, TypeWidthClass::descriptor_defined, "quantized_vector", 0),
      Descriptor(CanonicalTypeId::graph_node, TypeFamily::graph, TypeWidthClass::descriptor_defined, "graph_node", 0),
      Descriptor(CanonicalTypeId::graph_edge, TypeFamily::graph, TypeWidthClass::descriptor_defined, "graph_edge", 0),
      Descriptor(CanonicalTypeId::graph_path, TypeFamily::graph, TypeWidthClass::descriptor_defined, "graph_path", 0),
      Descriptor(CanonicalTypeId::time_series_value, TypeFamily::time_series, TypeWidthClass::descriptor_defined, "time_series_value", 0),
      Descriptor(CanonicalTypeId::columnar_segment, TypeFamily::columnar, TypeWidthClass::descriptor_defined, "columnar_segment", 0),
      Descriptor(CanonicalTypeId::aggregate_state, TypeFamily::aggregate_state, TypeWidthClass::descriptor_defined, "aggregate_state", 0),
      Descriptor(CanonicalTypeId::hll_sketch, TypeFamily::sketch, TypeWidthClass::descriptor_defined, "hll_sketch", 0),
      Descriptor(CanonicalTypeId::bloom_filter, TypeFamily::sketch, TypeWidthClass::descriptor_defined, "bloom_filter", 0),
      Descriptor(CanonicalTypeId::quantile_sketch, TypeFamily::sketch, TypeWidthClass::descriptor_defined, "quantile_sketch", 0),
      Descriptor(CanonicalTypeId::histogram_sketch, TypeFamily::sketch, TypeWidthClass::descriptor_defined, "histogram_sketch", 0),
      Descriptor(CanonicalTypeId::ranking_summary, TypeFamily::sketch, TypeWidthClass::descriptor_defined, "ranking_summary", 0),
      Descriptor(CanonicalTypeId::vector_summary, TypeFamily::sketch, TypeWidthClass::descriptor_defined, "vector_summary", 0),
      Descriptor(CanonicalTypeId::lob_locator, TypeFamily::locator, TypeWidthClass::descriptor_defined, "lob_locator", 0),
      Descriptor(CanonicalTypeId::external_file_locator, TypeFamily::locator, TypeWidthClass::descriptor_defined, "external_file_locator", 0),
      Descriptor(CanonicalTypeId::remote_object_locator, TypeFamily::locator, TypeWidthClass::descriptor_defined, "remote_object_locator", 0),
      Descriptor(CanonicalTypeId::bridge_handle, TypeFamily::locator, TypeWidthClass::descriptor_defined, "bridge_handle", 0),
      Descriptor(CanonicalTypeId::cursor_handle, TypeFamily::locator, TypeWidthClass::descriptor_defined, "cursor_handle", 0),
      Descriptor(CanonicalTypeId::system_reference, TypeFamily::locator, TypeWidthClass::descriptor_defined, "system_reference", 0),
      Descriptor(CanonicalTypeId::opaque_extension, TypeFamily::opaque, TypeWidthClass::descriptor_defined, "opaque_extension", 0),
      Descriptor(CanonicalTypeId::cursor, TypeFamily::result_set, TypeWidthClass::descriptor_defined, "cursor", 0),
      Descriptor(CanonicalTypeId::result_set, TypeFamily::result_set, TypeWidthClass::descriptor_defined, "result_set", 0),
      Descriptor(CanonicalTypeId::table_value, TypeFamily::result_set, TypeWidthClass::descriptor_defined, "table_value", 0),
  };
  return descriptors;
}

DatatypeDescriptorResult LookupDatatypeDescriptor(CanonicalTypeId type_id) {
  for (const DatatypeDescriptor& descriptor : BuiltinDatatypeDescriptors()) {
    if (descriptor.type_id == type_id) {
      DatatypeDescriptorResult result;
      result.status = DatatypeOkStatus();
      result.descriptor = descriptor;
      return result;
    }
  }

  DatatypeDescriptorResult result;
  result.status = DatatypeErrorStatus();
  result.diagnostic = MakeDatatypeDiagnostic(result.status,
                                             "SB-DATATYPE-UNKNOWN-CANONICAL-TYPE",
                                             "datatype.unknown_canonical_type",
                                             CanonicalTypeName(type_id));
  return result;
}

DatatypeDescriptorResult ValidateDatatypeDescriptor(const DatatypeDescriptor& descriptor) {
  DatatypeDescriptorResult result;
  result.status = DatatypeOkStatus();
  result.descriptor = descriptor;

  if (descriptor.type_id == CanonicalTypeId::unknown || descriptor.family == TypeFamily::unknown ||
      descriptor.width_class == TypeWidthClass::unknown || descriptor.stable_name.empty()) {
    result.status = DatatypeErrorStatus();
    result.diagnostic = MakeDatatypeDiagnostic(result.status,
                                               "SB-DATATYPE-DESCRIPTOR-INCOMPLETE",
                                               "datatype.descriptor_incomplete",
                                               descriptor.stable_name);
    return result;
  }

  if (!descriptor.descriptor_authoritative || !descriptor.reference_name_is_alias_only) {
    result.status = DatatypeErrorStatus();
    result.diagnostic = MakeDatatypeDiagnostic(result.status,
                                               "SB-DATATYPE-DESCRIPTOR-AUTHORITY-VIOLATION",
                                               "datatype.descriptor_authority_violation",
                                               descriptor.stable_name);
    return result;
  }

  if (descriptor.requires_mandatory_library && descriptor.required_capability_key.empty()) {
    result.status = DatatypeErrorStatus();
    result.diagnostic = MakeDatatypeDiagnostic(result.status,
                                               "SB-DATATYPE-MISSING-CAPABILITY-KEY",
                                               "datatype.missing_capability_key",
                                               descriptor.stable_name);
    return result;
  }

  if (descriptor.width_class == TypeWidthClass::fixed && descriptor.type_id != CanonicalTypeId::null_type &&
      descriptor.bit_width == 0) {
    result.status = DatatypeErrorStatus();
    result.diagnostic = MakeDatatypeDiagnostic(result.status,
                                               "SB-DATATYPE-FIXED-WIDTH-MISSING",
                                               "datatype.fixed_width_missing",
                                               descriptor.stable_name);
    return result;
  }

  return result;
}

DatatypeCapabilityCheck CheckDatatypeMandatoryCapabilities(const RuntimeCapabilityManifest& manifest) {
  DatatypeCapabilityCheck result;
  result.status = DatatypeOkStatus();

  for (const DatatypeDescriptor& descriptor : BuiltinDatatypeDescriptors()) {
    if (!descriptor.requires_mandatory_library) {
      continue;
    }

    if (!CapabilityPresent(manifest, descriptor.required_capability_key)) {
      result.diagnostics.push_back(MakeDatatypeDiagnostic(DatatypeErrorStatus(),
                                                          "SB-DATATYPE-MANDATORY-CAPABILITY-MISSING",
                                                          "datatype.mandatory_capability_missing",
                                                          descriptor.required_capability_key));
    }
  }

  if (!result.diagnostics.empty()) {
    result.status = DatatypeErrorStatus();
  }

  return result;
}

ExecutionTypeDescriptorResult BuildExecutionTypeDescriptorFromCatalog(
    const DatatypeDescriptor& descriptor,
    const CatalogExecutionTypeMetadata& metadata) {
  const auto validation = ValidateDatatypeDescriptor(descriptor);
  if (!validation.ok()) {
    ExecutionTypeDescriptorResult result;
    result.status = validation.status;
    result.diagnostic = validation.diagnostic;
    return result;
  }

  if (!metadata.descriptor_uuid.valid()) {
    return DescriptorBuildFailure("SB-EDR-DESCRIPTOR-MISSING-UUID",
                                  "execution_type_descriptor.missing_descriptor_uuid",
                                  descriptor.stable_name);
  }
  if (metadata.descriptor_epoch == 0) {
    return DescriptorBuildFailure("SB-EDR-DESCRIPTOR-MISSING-EPOCH",
                                  "execution_type_descriptor.missing_descriptor_epoch",
                                  descriptor.stable_name);
  }

  ExecutionTypeDescriptorResult result;
  result.status = DatatypeOkStatus();
  result.descriptor.descriptor_uuid = ToEngineUuid(metadata.descriptor_uuid.value);
  result.descriptor.descriptor_epoch = metadata.descriptor_epoch;
  result.descriptor.canonical_type_id = static_cast<std::uint32_t>(descriptor.type_id);
  result.descriptor.family = ToExecutionFamily(descriptor.family);
  result.descriptor.width_class = ToExecutionWidthClass(descriptor.width_class);
  result.descriptor.stable_name = descriptor.stable_name;
  result.descriptor.bit_width = descriptor.bit_width;
  result.descriptor.precision = metadata.precision != 0 ? metadata.precision : descriptor.default_precision;
  result.descriptor.scale = metadata.scale != 0 ? metadata.scale : descriptor.default_scale;
  result.descriptor.length = metadata.length;
  result.descriptor.vector_dimensions = metadata.vector_dimensions;
  result.descriptor.container_rank = metadata.container_rank;
  result.descriptor.nullable_allowed = descriptor.nullable_allowed;
  result.descriptor.descriptor_authoritative = descriptor.descriptor_authoritative;
  result.descriptor.parser_independent = true;

  if (result.descriptor.precision != 0) {
    SetModifier(&result.descriptor.modifier_flags,
                scratchbird::engine::ExecutionTypeModifierFlag::precision);
  }
  if (result.descriptor.scale != 0 || descriptor.default_scale != 0 ||
      descriptor.type_id == CanonicalTypeId::decimal ||
      descriptor.type_id == CanonicalTypeId::decimal_float) {
    SetModifier(&result.descriptor.modifier_flags,
                scratchbird::engine::ExecutionTypeModifierFlag::scale);
  }
  if (result.descriptor.length != 0) {
    SetModifier(&result.descriptor.modifier_flags,
                scratchbird::engine::ExecutionTypeModifierFlag::length);
  }
  if (result.descriptor.vector_dimensions != 0) {
    SetModifier(&result.descriptor.modifier_flags,
                scratchbird::engine::ExecutionTypeModifierFlag::vector_dimensions);
  }
  if (result.descriptor.container_rank != 0) {
    SetModifier(&result.descriptor.modifier_flags,
                scratchbird::engine::ExecutionTypeModifierFlag::container_rank);
  }

  if (metadata.domain_uuid.valid()) {
    AttachTypedUuid(metadata.domain_uuid,
                    scratchbird::engine::ExecutionTypeModifierFlag::domain_uuid,
                    &result.descriptor.domain_uuid,
                    &result.descriptor.modifier_flags);
  }

  for (const TypedUuid& domain_uuid : metadata.domain_stack) {
    if (!domain_uuid.valid()) {
      return DescriptorBuildFailure("SB-EDR-DESCRIPTOR-BAD-DOMAIN-STACK",
                                    "execution_type_descriptor.bad_domain_stack",
                                    descriptor.stable_name);
    }
    result.descriptor.domain_stack.push_back(ToEngineUuid(domain_uuid.value));
  }
  if (!result.descriptor.domain_stack.empty()) {
    SetModifier(&result.descriptor.modifier_flags,
                scratchbird::engine::ExecutionTypeModifierFlag::domain_stack);
    if (IsNilEngineUuid(result.descriptor.domain_uuid)) {
      result.descriptor.domain_uuid = result.descriptor.domain_stack.back();
      SetModifier(&result.descriptor.modifier_flags,
                  scratchbird::engine::ExecutionTypeModifierFlag::domain_uuid);
    }
  }

  if (metadata.charset_uuid.valid()) {
    AttachTypedUuid(metadata.charset_uuid,
                    scratchbird::engine::ExecutionTypeModifierFlag::charset_uuid,
                    &result.descriptor.charset_uuid,
                    &result.descriptor.modifier_flags);
  }
  if (metadata.collation_uuid.valid()) {
    AttachTypedUuid(metadata.collation_uuid,
                    scratchbird::engine::ExecutionTypeModifierFlag::collation_uuid,
                    &result.descriptor.collation_uuid,
                    &result.descriptor.modifier_flags);
  }
  if (metadata.timezone_uuid.valid()) {
    AttachTypedUuid(metadata.timezone_uuid,
                    scratchbird::engine::ExecutionTypeModifierFlag::timezone_uuid,
                    &result.descriptor.timezone_uuid,
                    &result.descriptor.modifier_flags);
  }
  if (metadata.element_descriptor_uuid.valid()) {
    AttachTypedUuid(metadata.element_descriptor_uuid,
                    scratchbird::engine::ExecutionTypeModifierFlag::element_descriptor_uuid,
                    &result.descriptor.element_descriptor_uuid,
                    &result.descriptor.modifier_flags);
  }
  if (metadata.security_policy_uuid.valid()) {
    AttachTypedUuid(metadata.security_policy_uuid,
                    scratchbird::engine::ExecutionTypeModifierFlag::security_policy_uuid,
                    &result.descriptor.security_policy_uuid,
                    &result.descriptor.modifier_flags);
  }

  return result;
}

ExecutionTypeDescriptorResult LookupExecutionTypeDescriptorFromCatalog(
    CanonicalTypeId type_id,
    const CatalogExecutionTypeMetadata& metadata) {
  const auto descriptor = LookupDatatypeDescriptor(type_id);
  if (!descriptor.ok()) {
    ExecutionTypeDescriptorResult result;
    result.status = descriptor.status;
    result.diagnostic = descriptor.diagnostic;
    return result;
  }
  return BuildExecutionTypeDescriptorFromCatalog(descriptor.descriptor, metadata);
}

DiagnosticRecord MakeDatatypeDiagnostic(Status status,
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
                        "core.datatypes.descriptor");
}

}  // namespace scratchbird::core::datatypes

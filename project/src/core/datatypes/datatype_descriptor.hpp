// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "scratchbird/engine/execution_type_descriptor.hpp"

#include "runtime_capabilities.hpp"
#include "runtime_platform.hpp"

#include <string>
#include <vector>

namespace scratchbird::core::datatypes {

using scratchbird::core::platform::DiagnosticRecord;
using scratchbird::core::platform::RuntimeCapabilityManifest;
using scratchbird::core::platform::Status;
using scratchbird::core::platform::TypedUuid;
using scratchbird::core::platform::u16;
using scratchbird::core::platform::u32;
using scratchbird::core::platform::u64;

enum class TypeFamily : u16 {
  null_type,
  boolean,
  signed_integer,
  unsigned_integer,
  real,
  decimal,
  uuid,
  character,
  binary,
  bit_string,
  temporal,
  blob,
  network,
  document,
  search,
  structured,
  range,
  spatial,
  vector,
  graph,
  time_series,
  columnar,
  aggregate_state,
  sketch,
  locator,
  opaque,
  result_set,
  unknown
};

enum class CanonicalTypeId : u32 {
  null_type = 0,
  boolean = 1,
  int8 = 100,
  int16 = 101,
  int32 = 102,
  int64 = 103,
  int128 = 104,
  uint8 = 120,
  uint16 = 121,
  uint32 = 122,
  uint64 = 123,
  uint128 = 124,
  bfloat16 = 138,
  real16 = 139,
  real32 = 140,
  real64 = 141,
  real128 = 142,
  decimal = 160,
  decimal_float = 161,
  uuid = 200,
  ip_address = 210,
  network_prefix = 211,
  mac_address = 212,
  character = 300,
  binary = 301,
  bit_string = 302,
  date = 400,
  time = 401,
  timestamp = 402,
  interval = 403,
  blob = 500,
  document = 600,
  json_document = 601,
  binary_json_document = 602,
  bson_document = 603,
  xml_document = 604,
  hstore_document = 605,
  object_document = 606,
  flattened_object_document = 607,
  enum_value = 620,
  set_value = 621,
  array = 622,
  list = 623,
  map = 624,
  row = 625,
  composite = 626,
  variant = 627,
  range = 628,
  multirange = 629,
  token_stream = 650,
  search_query = 651,
  search_rank_feature = 652,
  search_completion = 653,
  search_percolator = 654,
  geometry = 680,
  geography = 681,
  point = 682,
  shape = 683,
  raster = 684,
  vector = 700,
  dense_vector = 701,
  sparse_vector = 702,
  binary_vector = 703,
  quantized_vector = 704,
  graph_node = 800,
  graph_edge = 801,
  graph_path = 802,
  time_series_value = 850,
  columnar_segment = 860,
  aggregate_state = 870,
  hll_sketch = 880,
  bloom_filter = 881,
  quantile_sketch = 882,
  histogram_sketch = 883,
  ranking_summary = 884,
  vector_summary = 885,
  lob_locator = 900,
  external_file_locator = 901,
  remote_object_locator = 902,
  bridge_handle = 903,
  cursor_handle = 904,
  system_reference = 905,
  opaque_extension = 920,
  cursor = 930,
  result_set = 931,
  table_value = 932,
  unknown = 0xffffffffu
};

enum class TypeWidthClass : u16 {
  fixed,
  variable,
  descriptor_defined,
  unknown
};

struct DatatypeDescriptor {
  CanonicalTypeId type_id = CanonicalTypeId::unknown;
  TypeFamily family = TypeFamily::unknown;
  TypeWidthClass width_class = TypeWidthClass::unknown;
  std::string stable_name;
  u32 bit_width = 0;
  u32 default_precision = 0;
  u32 default_scale = 0;
  bool nullable_allowed = true;
  bool descriptor_authoritative = true;
  bool donor_name_is_alias_only = true;
  bool requires_mandatory_library = false;
  std::string required_capability_key;
};

struct DatatypeDescriptorResult {
  Status status;
  DatatypeDescriptor descriptor;
  DiagnosticRecord diagnostic;

  bool ok() const {
    return status.ok();
  }
};

struct DatatypeCapabilityCheck {
  Status status;
  std::vector<DiagnosticRecord> diagnostics;

  bool ok() const {
    return status.ok() && diagnostics.empty();
  }
};

struct CatalogExecutionTypeMetadata {
  TypedUuid descriptor_uuid;
  u64 descriptor_epoch = 0;
  u32 precision = 0;
  u32 scale = 0;
  u32 length = 0;
  u32 vector_dimensions = 0;
  u32 container_rank = 0;
  TypedUuid domain_uuid;
  std::vector<TypedUuid> domain_stack;
  TypedUuid charset_uuid;
  TypedUuid collation_uuid;
  TypedUuid timezone_uuid;
  TypedUuid element_descriptor_uuid;
  TypedUuid security_policy_uuid;
};

struct ExecutionTypeDescriptorResult {
  Status status;
  scratchbird::engine::ExecutionTypeDescriptor descriptor;
  DiagnosticRecord diagnostic;

  bool ok() const {
    return status.ok();
  }
};

const char* TypeFamilyName(TypeFamily family);
const char* CanonicalTypeName(CanonicalTypeId type_id);
const char* TypeWidthClassName(TypeWidthClass width_class);
const std::vector<DatatypeDescriptor>& BuiltinDatatypeDescriptors();
DatatypeDescriptorResult LookupDatatypeDescriptor(CanonicalTypeId type_id);
DatatypeDescriptorResult ValidateDatatypeDescriptor(const DatatypeDescriptor& descriptor);
DatatypeCapabilityCheck CheckDatatypeMandatoryCapabilities(const RuntimeCapabilityManifest& manifest);
ExecutionTypeDescriptorResult BuildExecutionTypeDescriptorFromCatalog(
    const DatatypeDescriptor& descriptor,
    const CatalogExecutionTypeMetadata& metadata);
ExecutionTypeDescriptorResult LookupExecutionTypeDescriptorFromCatalog(
    CanonicalTypeId type_id,
    const CatalogExecutionTypeMetadata& metadata);
DiagnosticRecord MakeDatatypeDiagnostic(Status status,
                                         std::string diagnostic_code,
                                         std::string message_key,
                                         std::string detail = {});

}  // namespace scratchbird::core::datatypes

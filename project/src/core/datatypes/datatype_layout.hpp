// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// SB-DATATYPE-LAYOUT-ANCHOR
#include "datatype_descriptor.hpp"

#include <string>
#include <vector>

namespace scratchbird::core::datatypes {

using scratchbird::core::platform::DiagnosticRecord;
using scratchbird::core::platform::Status;
using scratchbird::core::platform::u16;
using scratchbird::core::platform::u32;

enum class DatatypeStorageClass : u16 {
  inline_fixed,
  inline_variable,
  descriptor_payload,
  toast_reference,
  unknown
};

enum class DatatypeBinaryEncoding : u16 {
  none,
  twos_complement_little_endian,
  unsigned_little_endian,
  ieee754_binary32_little_endian,
  ieee754_binary64_little_endian,
  ieee754_binary128_little_endian,
  decimal128_descriptor,
  uuid_16_bytes,
  utf8_or_descriptor_charset_bytes,
  opaque_bytes,
  bit_packed_bytes,
  network_address_binary,
  days_since_unix_epoch_i32,
  nanoseconds_since_midnight_u64,
  timestamp_utc_tuple,
  interval_tuple,
  toast_locator,
  structured_canonical_binary,
  search_canonical_binary,
  spatial_canonical_binary,
  vector_canonical_binary,
  graph_canonical_binary,
  time_series_canonical_binary,
  columnar_segment_descriptor,
  aggregate_state_canonical_binary,
  sketch_canonical_binary,
  locator_envelope,
  opaque_extension_binary,
  result_set_descriptor,
  unknown
};

struct DatatypeStorageLayout {
  CanonicalTypeId type_id = CanonicalTypeId::unknown;
  DatatypeStorageClass storage_class = DatatypeStorageClass::unknown;
  DatatypeBinaryEncoding encoding = DatatypeBinaryEncoding::unknown;
  u32 inline_bytes = 0;
  u32 alignment_bytes = 1;
  bool nullable = true;
  bool requires_descriptor = false;
  bool requires_charset = false;
  bool requires_collation = false;
  bool requires_timezone = false;
  bool may_overflow_to_toast = false;
  bool fixed_sort_key = false;
  std::string notes;
};

struct DatatypeStorageLayoutResult {
  Status status;
  DatatypeStorageLayout layout;
  DiagnosticRecord diagnostic;

  bool ok() const {
    return status.ok();
  }
};

const char* DatatypeStorageClassName(DatatypeStorageClass storage_class);
const char* DatatypeBinaryEncodingName(DatatypeBinaryEncoding encoding);
const std::vector<DatatypeStorageLayout>& BuiltinDatatypeStorageLayouts();
DatatypeStorageLayoutResult LookupDatatypeStorageLayout(CanonicalTypeId type_id);
DatatypeStorageLayoutResult ValidateDatatypeStorageLayout(const DatatypeStorageLayout& layout);
DiagnosticRecord MakeDatatypeLayoutDiagnostic(Status status,
                                             std::string diagnostic_code,
                                             std::string message_key,
                                             std::string detail = {});

}  // namespace scratchbird::core::datatypes

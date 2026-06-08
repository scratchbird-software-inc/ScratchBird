// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// SB-DATATYPE-ADVANCED-FAMILY-ANCHOR
#include "datatype_descriptor.hpp"

#include <string>
#include <vector>

namespace scratchbird::core::datatypes {

using scratchbird::core::platform::DiagnosticRecord;
using scratchbird::core::platform::Status;
using scratchbird::core::platform::u16;
using scratchbird::core::platform::u32;

enum class AdvancedDatatypeFamily : u16 {
  spatial,
  vector,
  search,
  graph,
  time_series,
  sketch,
  aggregate_state,
  locator,
  unsupported
};

enum class AdvancedDatatypeOperationKind : u16 {
  validate,
  compare,
  hash,
  contains,
  intersects,
  distance,
  nearest_neighbor,
  tokenize,
  search_match,
  rank,
  graph_traverse,
  path_match,
  append_point,
  aggregate_window,
  estimate,
  merge,
  resolve_locator,
  unsupported
};

enum class AdvancedDatatypeIndexKind : u16 {
  none,
  btree,
  rtree,
  geohash,
  inverted,
  hnsw,
  ivfflat,
  adjacency,
  time_partition,
  sketch_summary,
  aggregate_state,
  locator_exact,
  unsupported
};

struct AdvancedDatatypeFamilyRequest {
  CanonicalTypeId type_id = CanonicalTypeId::unknown;
  AdvancedDatatypeOperationKind operation = AdvancedDatatypeOperationKind::validate;
  AdvancedDatatypeIndexKind index_kind = AdvancedDatatypeIndexKind::none;
  std::string descriptor_profile;
  u32 vector_dimension = 0;
};

struct AdvancedDatatypeFamilyResult {
  Status status;
  AdvancedDatatypeFamily family = AdvancedDatatypeFamily::unsupported;
  bool descriptor_supported = false;
  bool operation_supported = false;
  bool index_supported = false;
  bool optimizer_admitted = false;
  bool compare_supported = false;
  bool hash_supported = false;
  std::string canonical_descriptor_profile;
  std::vector<std::string> required_descriptor_fields;
  std::string compare_hash_refusal_detail;
  std::string optimizer_support_path;
  std::string diagnostic_detail;
  DiagnosticRecord diagnostic;

  bool ok() const {
    return status.ok();
  }
};

const char* AdvancedDatatypeFamilyName(AdvancedDatatypeFamily family);
const char* AdvancedDatatypeOperationKindName(AdvancedDatatypeOperationKind operation);
const char* AdvancedDatatypeIndexKindName(AdvancedDatatypeIndexKind index_kind);
AdvancedDatatypeFamily AdvancedDatatypeFamilyFor(CanonicalTypeId type_id);
AdvancedDatatypeFamilyResult EvaluateAdvancedDatatypeFamily(const AdvancedDatatypeFamilyRequest& request);
DiagnosticRecord MakeAdvancedDatatypeFamilyDiagnostic(Status status,
                                                     std::string diagnostic_code,
                                                     std::string message_key,
                                                     std::string detail = {});

}  // namespace scratchbird::core::datatypes

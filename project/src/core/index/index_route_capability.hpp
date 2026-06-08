// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// SB-INDEX-ROUTE-CAPABILITY-CLOSURE-ANCHOR

#include "index_family_registry.hpp"

#include <string>
#include <vector>

namespace scratchbird::core::index {

using scratchbird::core::platform::u32;

enum class IndexRouteKind : u32 {
  dml_insert,
  dml_update,
  dml_delete,
  sql_select,
  bulk_build,
  nosql_document,
  nosql_graph,
  nosql_vector,
  nosql_search,
  maintenance,
  validate_repair,
  unknown
};

struct IndexRouteCapabilityState {
  IndexRouteKind route = IndexRouteKind::unknown;
  IndexFamily family = IndexFamily::unknown;
  bool family_physical_complete = false;
  bool route_declared = false;
  bool route_supported = false;
  bool supports_read = false;
  bool supports_write = false;
  bool supports_mutation = false;
  bool supports_bulk_build = false;
  bool supports_reopen = false;
  bool supports_ordered_range = false;
  bool supports_equality_lookup = false;
  bool supports_negative_prune = false;
  bool supports_summary_segment_prune = false;
  bool produces_candidate_set = false;
  bool produces_ranking_or_seed = false;
  bool approximate_candidate_source = false;
  bool requires_exact_recheck = true;
  bool requires_mga_recheck = true;
  bool requires_security_recheck = true;
  bool requires_exact_rerank = false;
  bool hash_requires_keyed_algorithm = false;
  bool hash_legacy_benchmark_clean_requires_policy = false;
  bool hash_high_assurance_required_by_policy = false;
  bool benchmark_clean = false;
  std::string route_diagnostic_code;
  std::string route_message_key;
  std::string route_detail;
  std::vector<std::string> evidence;

  bool route_complete() const {
    return family_physical_complete && route_declared && route_supported &&
           benchmark_clean;
  }
};

const char* IndexRouteKindName(IndexRouteKind route);
const std::vector<IndexRouteCapabilityState>&
BuiltinIndexRouteCapabilityStates();
const IndexRouteCapabilityState* FindBuiltinIndexRouteCapabilityState(
    IndexRouteKind route,
    IndexFamily family);
DiagnosticRecord MakeIndexRouteCapabilityDiagnostic(
    Status status,
    const IndexRouteCapabilityState& state);

}  // namespace scratchbird::core::index

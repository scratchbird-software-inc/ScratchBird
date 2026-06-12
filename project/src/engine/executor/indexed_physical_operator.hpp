// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "api_types.hpp"
#include "index_btree_page.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace scratchbird::engine::executor {

enum class IndexedPhysicalOperatorKind {
  point_lookup,
  range_scan,
  ordered_limit,
  indexed_nested_loop,
  merge_ordered_input,
  runtime_filter
};

struct IndexedPhysicalOuterProbe {
  std::uint64_t outer_ordinal = 0;
  std::vector<scratchbird::core::platform::byte> encoded_key;
  scratchbird::storage::page::IndexBtreePhysicalScanBound lower_bound;
  scratchbird::storage::page::IndexBtreePhysicalScanBound upper_bound;
  bool range_probe = false;
};

struct IndexedPhysicalOperatorRequest {
  IndexedPhysicalOperatorKind kind = IndexedPhysicalOperatorKind::point_lookup;
  const scratchbird::storage::page::IndexBtreePhysicalTree* physical_tree = nullptr;
  const scratchbird::storage::page::IndexBtreePhysicalTree* right_physical_tree = nullptr;
  std::vector<scratchbird::core::platform::byte> encoded_point_key;
  scratchbird::storage::page::IndexBtreePhysicalScanBound lower_bound;
  scratchbird::storage::page::IndexBtreePhysicalScanBound upper_bound;
  std::uint64_t limit = 0;
  std::vector<IndexedPhysicalOuterProbe> outer_probes;
  std::vector<std::vector<scratchbird::core::platform::byte>> runtime_filter_keys;
  bool plan_safe = true;
  bool physical_tree_available = true;
  bool encoded_key_proof = true;
  bool encoded_bounds_proof = true;
  bool durable_mga_inventory_proof = true;
  bool mga_visibility_recheck_planned = true;
  bool security_recheck_planned = true;
  bool parser_or_reference_authority = false;
  bool index_or_cache_finality_authority = false;
};

struct IndexedPhysicalOperatorLocator {
  std::string row_uuid;
  std::string version_uuid;
  std::vector<scratchbird::core::platform::byte> encoded_key;
  std::uint64_t outer_ordinal = 0;
  std::uint64_t leaf_page_number = 0;
  std::uint32_t cell_ordinal = 0;
  bool from_physical_index = false;
  bool mga_recheck_required = true;
  bool security_recheck_required = true;
};

struct IndexedPhysicalMergePair {
  std::uint64_t left_ordinal = 0;
  std::uint64_t right_ordinal = 0;
  IndexedPhysicalOperatorLocator left;
  IndexedPhysicalOperatorLocator right;
};

struct IndexedPhysicalOperatorResult {
  bool ok = false;
  std::string diagnostic_code;
  std::string diagnostic_detail;
  std::vector<scratchbird::engine::internal_api::EngineEvidenceReference> evidence;
  std::vector<IndexedPhysicalOperatorLocator> locators;
  std::vector<IndexedPhysicalMergePair> merge_pairs;
  bool runtime_route_capability = false;
  bool benchmark_clean = false;
  bool table_scan_consumed = false;
};

const char* IndexedPhysicalOperatorKindName(IndexedPhysicalOperatorKind kind);

IndexedPhysicalOperatorResult ExecuteIndexedPhysicalOperator(
    const IndexedPhysicalOperatorRequest& request);

}  // namespace scratchbird::engine::executor

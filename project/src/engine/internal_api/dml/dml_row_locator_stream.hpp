// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "api_types.hpp"
#include "dml/dml_target_access_plan.hpp"
#include "index_btree_page.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace scratchbird::engine::internal_api {

enum class DmlRowLocatorStreamConsumer {
  on_conflict,
  merge,
  update,
  delete_row
};

enum class DmlRowLocatorStreamSource {
  refused,
  row_uuid_singleton,
  row_uuid_list,
  physical_unique_btree_point,
  physical_btree_point,
  physical_btree_range,
  table_scan_fallback
};

struct DmlMergeLocatorOrdinal {
  std::uint64_t source_ordinal = 0;
  std::uint64_t action_ordinal = 0;
  bool matched = false;
};

struct DmlRowLocatorStreamRequest {
  DmlRowLocatorStreamConsumer consumer = DmlRowLocatorStreamConsumer::update;
  DmlTargetAccessPlan access_plan;
  bool access_plan_engine_authority_proof = false;
  bool durable_mga_inventory_proof = false;
  bool mga_visibility_recheck_planned = true;
  bool security_recheck_planned = true;
  bool parser_or_reference_authority = false;
  bool index_or_cache_finality_authority = false;
  bool applicable_physical_index_exists = false;
  bool table_scan_fallback_allowed = false;
  bool index_unique = false;
  const scratchbird::storage::page::IndexBtreePhysicalTree* physical_tree = nullptr;
  std::vector<scratchbird::core::platform::byte> encoded_point_key;
  scratchbird::storage::page::IndexBtreePhysicalScanBound lower_bound;
  scratchbird::storage::page::IndexBtreePhysicalScanBound upper_bound;
  std::vector<DmlMergeLocatorOrdinal> merge_ordinals;
};

struct DmlRowLocator {
  std::string row_uuid;
  std::string version_uuid;
  std::string index_uuid;
  std::uint64_t leaf_page_number = 0;
  std::uint32_t cell_ordinal = 0;
  bool from_physical_index = false;
  bool mga_recheck_required = true;
  bool security_recheck_required = true;
};

struct DmlRowLocatorStreamResult {
  bool ok = false;
  DmlRowLocatorStreamSource source = DmlRowLocatorStreamSource::refused;
  EngineApiDiagnostic diagnostic;
  std::vector<EngineEvidenceReference> evidence;
  std::vector<DmlRowLocator> locators;
  bool table_scan_fallback = false;
  bool runtime_route_capability = false;
  bool benchmark_clean = false;
};

const char* DmlRowLocatorStreamConsumerName(DmlRowLocatorStreamConsumer consumer);
const char* DmlRowLocatorStreamSourceName(DmlRowLocatorStreamSource source);

DmlRowLocatorStreamResult BuildDmlRowLocatorStream(
    const DmlRowLocatorStreamRequest& request);

}  // namespace scratchbird::engine::internal_api

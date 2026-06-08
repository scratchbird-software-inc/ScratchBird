// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace scratchbird::engine::optimizer {

struct BulkPlacementOrderRow {
  std::uint64_t source_ordinal = 0;
  std::string row_uuid;
  std::string placement_key;
};

struct BulkPlacementOrderRequest {
  bool ordered_ingest_requested = false;
  bool derive_for_large_load = false;
  std::uint64_t large_load_row_threshold = 1024;
  std::string placement_key_column;
  std::vector<BulkPlacementOrderRow> rows;
};

struct BulkPlacementOrderResult {
  bool ok = true;
  bool ordered_ingest_selected = false;
  bool derived_order = false;
  bool row_identity_preserved = true;
  std::string diagnostic_code;
  std::string placement_key_column;
  std::uint64_t placement_key_run_count = 0;
  std::uint64_t reordered_row_count = 0;
  std::vector<std::uint64_t> source_ordinals_in_apply_order;
  std::vector<std::pair<std::string, std::string>> evidence;
};

BulkPlacementOrderResult PlanBulkPlacementOrder(
    const BulkPlacementOrderRequest& request);

}  // namespace scratchbird::engine::optimizer

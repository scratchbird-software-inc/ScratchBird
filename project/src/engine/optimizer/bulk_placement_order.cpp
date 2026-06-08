// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "bulk_placement_order.hpp"

#include <algorithm>

namespace scratchbird::engine::optimizer {
namespace {

void AddEvidence(BulkPlacementOrderResult* result,
                 std::string kind,
                 std::string id) {
  result->evidence.push_back({std::move(kind), std::move(id)});
}

}  // namespace

BulkPlacementOrderResult PlanBulkPlacementOrder(
    const BulkPlacementOrderRequest& request) {
  BulkPlacementOrderResult result;
  result.placement_key_column = request.placement_key_column;
  AddEvidence(&result, "bulk_placement_order_planner", "engine_optimizer");
  AddEvidence(&result,
              "bulk_placement_order_requested",
              request.ordered_ingest_requested ? "true" : "false");
  AddEvidence(&result,
              "bulk_placement_order_large_load_threshold",
              std::to_string(request.large_load_row_threshold));
  AddEvidence(&result,
              "bulk_placement_order_input_rows",
              std::to_string(request.rows.size()));

  const bool large_load =
      request.large_load_row_threshold != 0 &&
      request.rows.size() >= request.large_load_row_threshold;
  const bool selected =
      request.ordered_ingest_requested ||
      (request.derive_for_large_load && large_load);
  if (!selected) {
    AddEvidence(&result, "bulk_placement_order_selected", "false");
    return result;
  }

  if (request.placement_key_column.empty()) {
    result.ok = false;
    result.diagnostic_code = "SB_OPT_BULK_PLACEMENT_KEY_REQUIRED";
    AddEvidence(&result, "bulk_placement_order_refused", "placement_key_required");
    return result;
  }
  for (const auto& row : request.rows) {
    if (row.placement_key.empty()) {
      result.ok = false;
      result.diagnostic_code = "SB_OPT_BULK_PLACEMENT_KEY_VALUE_REQUIRED";
      AddEvidence(&result,
                  "bulk_placement_order_refused",
                  "placement_key_value_required");
      return result;
    }
  }

  result.ordered_ingest_selected = true;
  result.derived_order = !request.ordered_ingest_requested;
  result.source_ordinals_in_apply_order.reserve(request.rows.size());
  std::vector<BulkPlacementOrderRow> sorted = request.rows;
  std::stable_sort(sorted.begin(),
                   sorted.end(),
                   [](const BulkPlacementOrderRow& left,
                      const BulkPlacementOrderRow& right) {
                     if (left.placement_key != right.placement_key) {
                       return left.placement_key < right.placement_key;
                     }
                     return left.source_ordinal < right.source_ordinal;
                   });

  std::string previous_key;
  bool first_key = true;
  for (std::size_t index = 0; index < sorted.size(); ++index) {
    const auto& row = sorted[index];
    result.source_ordinals_in_apply_order.push_back(row.source_ordinal);
    if (row.source_ordinal != index) {
      ++result.reordered_row_count;
    }
    if (first_key || row.placement_key != previous_key) {
      ++result.placement_key_run_count;
      previous_key = row.placement_key;
      first_key = false;
    }
  }

  AddEvidence(&result, "bulk_placement_order_selected", "true");
  AddEvidence(&result,
              "bulk_placement_order_source",
              result.derived_order ? "optimizer_derived_large_load"
                                   : "explicit_request");
  AddEvidence(&result,
              "bulk_placement_order_key_column",
              result.placement_key_column);
  AddEvidence(&result,
              "bulk_placement_order_apply_permutation",
              std::to_string(result.source_ordinals_in_apply_order.size()));
  AddEvidence(&result,
              "bulk_placement_order_reordered_rows",
              std::to_string(result.reordered_row_count));
  AddEvidence(&result,
              "bulk_placement_order_key_runs",
              std::to_string(result.placement_key_run_count));
  AddEvidence(&result, "bulk_placement_row_identity_preserved", "true");
  AddEvidence(&result, "uuid_order_finality_authority", "false");
  AddEvidence(&result, "parser_finality_authority", "false");
  return result;
}

}  // namespace scratchbird::engine::optimizer

// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "api_types.hpp"

#include <span>
#include <vector>

namespace scratchbird::engine::internal_api {

// SEARCH_KEY: SB_ENGINE_INTERNAL_API_DML_INSERT_API
struct EngineInsertRowsRequest : EngineApiRequest {
  EngineObjectReference target_table;
  std::vector<EngineRowValue> input_rows;
  // Optional borrowed input window. Callers that use this path must keep the
  // pointed-to rows alive for the duration of EngineInsertRows.
  std::span<const EngineRowValue> borrowed_input_rows;
  bool HasAmbiguousInputRows() const {
    return !input_rows.empty() && !borrowed_input_rows.empty();
  }
  std::span<const EngineRowValue> EffectiveInputRows() const {
    if (!input_rows.empty()) {
      return std::span<const EngineRowValue>(input_rows.data(), input_rows.size());
    }
    return borrowed_input_rows;
  }
  bool require_generated_row_uuid = true;
  // SEARCH_KEY: SB_INSERT_BATCH_API_SURFACE
  // Optional canonical insert optimization controls. Existing callers can omit
  // these fields and still execute through the singleton/multi-values wrapper.
  EngineApiU64 estimated_row_count = 0;
  std::string insert_mode;
  std::string duplicate_mode = "error";
  std::string on_conflict_action;
  std::string conflict_target_column;
  std::vector<std::string> conflict_update_columns;
  bool strict_bulk_load_requested = false;
  bool donor_unique_checks_relaxed = false;
  bool donor_foreign_key_checks_relaxed = false;
};
struct EngineInsertRowsResult : EngineApiResult {
  EngineApiU64 inserted_count = 0;
  EngineApiU64 updated_count = 0;
  EngineApiU64 skipped_count = 0;
  std::vector<EngineUuid> row_uuids;
};
EngineInsertRowsResult EngineInsertRows(const EngineInsertRowsRequest& request);

}  // namespace scratchbird::engine::internal_api

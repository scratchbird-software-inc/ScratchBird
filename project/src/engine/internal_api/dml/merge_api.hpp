// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "api_types.hpp"

namespace scratchbird::engine::internal_api {

// SEARCH_KEY: SB_ENGINE_INTERNAL_API_DML_MERGE_API
struct EngineMergeRowsRequest : EngineApiRequest {
  EngineObjectReference target_table;
  EnginePredicateEnvelope match_predicate;
  std::vector<EngineRowValue> input_rows;
  std::vector<std::pair<std::string, EngineTypedValue>> update_assignments;
  bool update_when_matched = true;
  bool insert_when_not_matched = true;
  bool delete_when_matched = false;
};
struct EngineMergeRowsResult : EngineApiResult {
  EngineApiU64 matched_count = 0;
  EngineApiU64 inserted_count = 0;
  EngineApiU64 updated_count = 0;
  EngineApiU64 merged_count = 0;
};
EngineMergeRowsResult EngineMergeRows(const EngineMergeRowsRequest& request);

}  // namespace scratchbird::engine::internal_api

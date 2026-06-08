// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "api_types.hpp"

#include <cstddef>
#include <string>
#include <vector>

namespace scratchbird::engine::internal_api {

// SEARCH_KEY: SB_ENGINE_INTERNAL_API_QUERY_PLAN_API
struct EngineQueryRelation {
  std::string relation_name;
  std::string descriptor_digest;
  EngineObjectReference source_object;
  std::vector<EngineDescriptor> columns;
  std::vector<EngineRowValue> rows;
};

struct EnginePlanOperationRequest : EngineApiRequest {
  bool execute = false;
  std::string query_operation;
  std::string join_algorithm;
  std::string set_operation = "union_distinct";
  bool set_by_name = false;
  std::vector<EngineQueryRelation> relations;
  std::vector<std::size_t> projected_columns;
  std::size_t left_key_column = 0;
  std::size_t right_key_column = 0;
  std::string left_key_field;
  std::string right_key_field;
  std::size_t group_key_column = 0;
  std::size_t aggregate_value_column = 1;
  std::size_t aggregate_pair_value_column = 2;
  std::string group_key_field;
  std::string aggregate_value_field;
  std::string aggregate_pair_value_field;
  std::string aggregate_function = "sum";
  std::size_t order_column = 0;
  std::string order_field;
  std::string window_function = "row_number";
  std::size_t window_value_column = 0;
  std::string window_value_field;
  std::size_t partition_key_column = 0;
  std::string partition_key_field;
  EngineApiU64 window_n = 1;
  EngineApiU64 limit = 0;
  EngineApiU64 offset = 0;
  bool ascending = true;
};
struct EnginePlanOperationResult : EngineApiResult {
  std::string plan_kind;
  EngineApiU64 output_row_count = 0;
};
EnginePlanOperationResult EnginePlanOperation(const EnginePlanOperationRequest& request);

}  // namespace scratchbird::engine::internal_api

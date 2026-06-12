// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "api_types.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace scratchbird::engine::internal_api {

// SEARCH_KEY: SB_ENGINE_INTERNAL_API_CLUSTER_PLACEMENT_API
struct EnginePlaceClusterObjectRequest : EngineApiRequest {};
struct EnginePlaceClusterObjectResult : EngineApiResult {};
EnginePlaceClusterObjectResult EnginePlaceClusterObject(const EnginePlaceClusterObjectRequest& request);

struct EngineShardPlacementDescriptor {
  std::string shard_uuid;
  std::string source_filespace_uuid;
  std::string target_filespace_uuid;
  std::string range_begin;
  std::string range_end;
  std::uint64_t placement_epoch = 0;
  std::uint64_t placement_generation = 0;
  std::string state = "planned";
};

struct EngineShardPlacementOperationRequest : EngineApiRequest {
  std::string placement_operation;
  EngineShardPlacementDescriptor descriptor;
  std::vector<EngineShardPlacementDescriptor> merge_inputs;
  bool operator_authorized = false;
  bool physical_data_movement_requested = false;
};

struct EngineShardPlacementOperationResult : EngineApiResult {
  bool descriptor_validated = false;
  bool operation_planned = false;
  bool operation_verified = false;
  bool operator_review_required = false;
  bool physical_data_movement_required = false;
  bool durable_state_changed = false;
  bool private_cluster_execution = false;
  bool cluster_provider_dispatch = false;
  std::string placement_state;
  std::string recommended_action;
};

EngineShardPlacementOperationResult EnginePlanShardPlacementOperation(
    const EngineShardPlacementOperationRequest& request);

}  // namespace scratchbird::engine::internal_api

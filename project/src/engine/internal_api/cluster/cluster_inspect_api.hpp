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

// SEARCH_KEY: SB_ENGINE_INTERNAL_API_CLUSTER_CLUSTER_INSPECT_API
struct EngineInspectClusterStateRequest : EngineApiRequest {};
struct EngineInspectClusterStateResult : EngineApiResult {};
EngineInspectClusterStateResult EngineInspectClusterState(const EngineInspectClusterStateRequest& request);

struct EngineInspectClusterRoutingPlanRequest : EngineApiRequest {};
struct EngineInspectClusterRoutingPlanResult : EngineApiResult {};
EngineInspectClusterRoutingPlanResult EngineInspectClusterRoutingPlan(const EngineInspectClusterRoutingPlanRequest& request);

}  // namespace scratchbird::engine::internal_api

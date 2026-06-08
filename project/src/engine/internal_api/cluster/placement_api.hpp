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

// SEARCH_KEY: SB_ENGINE_INTERNAL_API_CLUSTER_PLACEMENT_API
struct EnginePlaceClusterObjectRequest : EngineApiRequest {};
struct EnginePlaceClusterObjectResult : EngineApiResult {};
EnginePlaceClusterObjectResult EnginePlaceClusterObject(const EnginePlaceClusterObjectRequest& request);

}  // namespace scratchbird::engine::internal_api

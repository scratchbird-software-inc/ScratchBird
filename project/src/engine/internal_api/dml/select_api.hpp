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

// SEARCH_KEY: SB_ENGINE_INTERNAL_API_DML_SELECT_API
struct EngineSelectRowsRequest : EngineApiRequest {
  EngineObjectReference source_object;
  EngineProjectionEnvelope select_projection;
  EnginePredicateEnvelope select_predicate;
  EngineOrderingEnvelope select_ordering;
  EngineApiU64 limit = 0;
  EngineApiU64 offset = 0;
};
struct EngineSelectRowsResult : EngineApiResult {
  EngineApiU64 visible_count = 0;
};
EngineSelectRowsResult EngineSelectRows(const EngineSelectRowsRequest& request);

}  // namespace scratchbird::engine::internal_api

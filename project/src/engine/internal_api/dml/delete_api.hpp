// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "api_types.hpp"
#include "catalog/relation_projection_view.hpp"

namespace scratchbird::engine::internal_api {

// SEARCH_KEY: SB_ENGINE_INTERNAL_API_DML_DELETE_API
struct EngineDeleteRowsRequest : EngineApiRequest {
  EngineObjectReference target_table;
  EnginePredicateEnvelope delete_predicate;
  EngineRelationProjectionViewDeleteEnvelope relation_projection_view;
  std::string delete_surface_variant = "delete";
  std::string batch_on_column;
  EngineApiU64 batch_limit_rows = 0;
  EngineApiU64 limit = 0;
  EngineApiU64 offset = 0;
  std::string series_name;
  bool tombstone_only = true;
};
struct EngineDeleteRowsResult : EngineApiResult {
  EngineApiU64 matched_count = 0;
  EngineApiU64 deleted_count = 0;
};
EngineDeleteRowsResult EngineDeleteRows(const EngineDeleteRowsRequest& request);

}  // namespace scratchbird::engine::internal_api

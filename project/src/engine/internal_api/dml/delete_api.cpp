// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "dml/delete_api.hpp"

#include "dml/update_delete_optimized.hpp"

namespace scratchbird::engine::internal_api {

// SEARCH_KEY: SB_ENGINE_INTERNAL_API_DML_DELETE_API_BEHAVIOR
EngineDeleteRowsResult EngineDeleteRows(const EngineDeleteRowsRequest& request) {
  return ExecuteOptimizedDeleteRows(request);
}

}  // namespace scratchbird::engine::internal_api

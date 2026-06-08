// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "dml/delete_api.hpp"
#include "dml/update_api.hpp"

namespace scratchbird::engine::internal_api {

// SEARCH_KEY: SB_PID004_OPTIMIZED_UPDATE_DELETE_EXECUTOR
// Shared UPDATE/DELETE execution entrypoints. Public API files stay as thin
// wrappers so future physical optimization can be switched here without
// changing the parser/SBLR/API contract.

EngineUpdateRowsResult ExecuteOptimizedUpdateRows(const EngineUpdateRowsRequest& request);
EngineDeleteRowsResult ExecuteOptimizedDeleteRows(const EngineDeleteRowsRequest& request);

}  // namespace scratchbird::engine::internal_api

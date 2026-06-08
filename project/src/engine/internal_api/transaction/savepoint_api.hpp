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

// SEARCH_KEY: SB_ENGINE_INTERNAL_API_TRANSACTION_SAVEPOINT_API
struct EngineCreateSavepointRequest : EngineApiRequest {};
struct EngineCreateSavepointResult : EngineApiResult {};
EngineCreateSavepointResult EngineCreateSavepoint(const EngineCreateSavepointRequest& request);

struct EngineReleaseSavepointRequest : EngineApiRequest {};
struct EngineReleaseSavepointResult : EngineApiResult {};
EngineReleaseSavepointResult EngineReleaseSavepoint(const EngineReleaseSavepointRequest& request);

struct EngineRollbackToSavepointRequest : EngineApiRequest {};
struct EngineRollbackToSavepointResult : EngineApiResult {};
EngineRollbackToSavepointResult EngineRollbackToSavepoint(const EngineRollbackToSavepointRequest& request);

}  // namespace scratchbird::engine::internal_api

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

// SEARCH_KEY: SB_ENGINE_INTERNAL_API_DDL_ALTER_API
struct EngineAlterObjectRequest : EngineApiRequest {};
struct EngineAlterObjectResult : EngineApiResult {};
EngineAlterObjectResult EngineAlterObject(const EngineAlterObjectRequest& request);

struct EngineAlterConstraintRequest : EngineApiRequest {};
struct EngineAlterConstraintResult : EngineApiResult {
  EngineBoundObjectIdentity bound_object_identity;
  std::uint64_t metadata_cache_epoch = 0;
};
EngineAlterConstraintResult EngineAlterConstraint(const EngineAlterConstraintRequest& request);

}  // namespace scratchbird::engine::internal_api

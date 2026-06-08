// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "ddl/comment_api.hpp"

#include "behavior_support/api_behavior_store.hpp"

namespace scratchbird::engine::internal_api {

// SEARCH_KEY: SB_ENGINE_INTERNAL_API_DDL_COMMENT_API_BEHAVIOR
EngineCommentOnObjectResult EngineCommentOnObject(const EngineCommentOnObjectRequest& request) {
  return PersistedRecordResult<EngineCommentOnObjectResult>(request, "ddl.comment_on_object", "object_comment", true, "active");
}

}  // namespace scratchbird::engine::internal_api

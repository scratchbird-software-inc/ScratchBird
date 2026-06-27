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
  auto comment_request = request;
  const std::string comment_target_uuid = request.target_object.uuid.canonical;
  const std::string comment_target_kind =
      request.target_object.object_kind.empty() ? "object" : request.target_object.object_kind;

  // COMMENT records describe a catalog object; they must not replace the
  // object's descriptor row in the behavior stream.
  comment_request.target_object.uuid.canonical.clear();
  comment_request.target_object.object_kind.clear();

  auto result = PersistedRecordResult<EngineCommentOnObjectResult>(
      comment_request, "ddl.comment_on_object", "object_comment", true, "active");
  if (!result.ok) return result;

  AddApiBehaviorEvidence(&result, "comment_target", comment_target_uuid);
  AddApiBehaviorEvidence(&result, "comment_target_kind", comment_target_kind);
  AddApiBehaviorRow(&result,
                    {{"comment_target_uuid", comment_target_uuid},
                     {"comment_target_kind", comment_target_kind},
                     {"descriptor_replaced", "false"},
                     {"parser_executes_sql", "false"}});
  return result;
}

}  // namespace scratchbird::engine::internal_api

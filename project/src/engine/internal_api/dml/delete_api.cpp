// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "dml/delete_api.hpp"

#include "crud_support/crud_store.hpp"
#include "dml/update_delete_optimized.hpp"

namespace scratchbird::engine::internal_api {

// SEARCH_KEY: SB_ENGINE_INTERNAL_API_DML_DELETE_API_BEHAVIOR
EngineDeleteRowsResult EngineDeleteRows(const EngineDeleteRowsRequest& request) {
  if (IsEngineRelationProjectionViewDeleteRequest(request)) {
    EngineDeleteRowsRequest expanded;
    EngineRelationProjectionViewDescriptor view;
    const auto expansion = ExpandEngineRelationProjectionViewDelete(
        request, &expanded, &view);
    if (expansion.error) {
      return MakeCrudDiagnosticResult<EngineDeleteRowsResult>(
          request.context, "dml.delete_rows", expansion);
    }
    auto result = ExecuteOptimizedDeleteRows(expanded);
    if (result.ok) {
      result.evidence.push_back(
          {"relation_projection_view_marker", view.marker});
      result.evidence.push_back(
          {"relation_projection_view_uuid", view.view_uuid.canonical});
      result.evidence.push_back(
          {"relation_projection_view_descriptor_uuid",
           view.view_descriptor_uuid.canonical});
      result.evidence.push_back(
          {"relation_projection_view_descriptor_generation",
           std::to_string(view.view_descriptor_generation)});
      result.evidence.push_back(
          {"relation_projection_view_source_resource_epoch",
           std::to_string(view.source_resource_epoch)});
      result.evidence.push_back(
          {"relation_projection_view_source_relation_uuid",
           view.source_relation_uuid.canonical});
      result.evidence.push_back(
          {"relation_projection_view_delete_expansion",
           "engine_owned_sql_free"});
      result.evidence.push_back(
          {"relation_projection_view_delete_mga_authority",
           "ordinary_optimized_delete"});
      result.evidence.push_back(
          {"relation_projection_view_parser_sql", "false"});
    }
    return result;
  }
  return ExecuteOptimizedDeleteRows(request);
}

}  // namespace scratchbird::engine::internal_api

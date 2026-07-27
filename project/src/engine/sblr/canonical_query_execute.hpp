// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "engine/optimizer/optimizer_catalog_backed_planning.hpp"
#include "query/plan_api.hpp"

#include <cstddef>
#include <string>

namespace scratchbird::engine::sblr {

struct CanonicalObjectFreeValuesExecutionRequest {
  scratchbird::engine::internal_api::EngineRequestContext context;
  scratchbird::engine::internal_api::TypedRelationalDag relational_dag;
  scratchbird::engine::optimizer::CanonicalOptimizerAdmissionRequest
      optimizer_request;
  scratchbird::engine::optimizer::CanonicalOptimizerAdmissionResult
      optimizer_admission;
};

struct CanonicalObjectFreeValuesExecutionResult {
  bool profile_matched{false};
  bool optimizer_selected{false};
  bool physical_dag_published{false};
  bool physical_dag_executed{false};
  bool runtime_actuals_attached{false};
  bool canonical_result_published{false};
  std::size_t physical_node_count{0};
  std::size_t canonical_result_column_count{0};
  std::size_t canonical_result_row_count{0};
  std::string selected_plan_uuid;
  std::string canonical_result_bytes;
  scratchbird::engine::internal_api::EngineApiResult api_result;
};

// QOW-SOURCE-INTEGRATION-306-211-LIVE-VALUES-V1
// This bounded live native query profile accepts one object-free VALUES root,
// the exact two-node VALUES/FILTER, descriptor-direct VALUES/PROJECT, or
// bound-count VALUES/LIMIT shape, the exact property-bound VALUES/SORT shape,
// the exact VALUES/global COUNT(*) or COUNT(input-column) aggregate shape,
// the exact three-node VALUES/VALUES/UNION ALL ordinal shape, or the exact
// three-node VALUES/VALUES/INNER JOIN shape.
// FILTER and INNER JOIN each require one object-free bound boolean predicate.
// The complete typed literal/composed-scalar payload must be derivable from the
// admitted relational DAG. Every other logical shape remains on the existing
// physical-dispatch-pending path until its bound production payload adapter
// exists.
CanonicalObjectFreeValuesExecutionResult
ExecuteCanonicalObjectFreeValuesQuery(
    const CanonicalObjectFreeValuesExecutionRequest& request);

}  // namespace scratchbird::engine::sblr

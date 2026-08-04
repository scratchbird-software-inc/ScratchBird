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
  bool optimizer_admitted{false};
  bool optimizer_admission_degraded{false};
  bool optimizer_benchmark_clean_ready{false};
  bool optimizer_selected{false};
  bool physical_dag_published{false};
  bool physical_dag_executed{false};
  bool runtime_actuals_attached{false};
  bool canonical_result_published{false};
  std::size_t optimizer_admission_stage_count{0};
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
// the exact VALUES/global COUNT(*), COUNT(input-column), integer
// SUM(input-column), integer-input AVG(input-column), integer MIN/MAX,
// boolean BOOL_AND/BOOL_OR/EVERY, int64-input unary statistical aggregate,
// exact two-int64-input pair statistical/regression aggregate, or text
// STRING_AGG(input-column, literal-separator), ordered text
// STRING_AGG(input-column, literal-separator, int64-order-column), ordered text
// LISTAGG(input-column, literal-separator, int64-order-column) with exact
// optional overflow-bound/error/truncate literals, ordered text
// ARRAY_AGG(input-column, int64-order-column), or ordered text
// JSON_AGG(input-column, int64-order-column), or ordered text-key/int64-value
// JSON_OBJECT_AGG(key-column, value-column, int64-order-column) shape,
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

struct CanonicalCurrentHeapExecutionRequest {
  scratchbird::engine::internal_api::EngineRequestContext context;
  scratchbird::engine::internal_api::TypedRelationalDag relational_dag;
};

// QOW-SOURCE-PACKET7-OBJECT-BACKED-HEAP-ROUTE-V1
// Accepts only the exact one-leaf current relation.source.v1 profile. Catalog,
// authorization, descriptor, optimizer, physical-plan, and MGA evidence are
// derived and revalidated by existing engine-owned components; the caller
// supplies no object snapshot, physical DAG, or visibility authority.
CanonicalObjectFreeValuesExecutionResult ExecuteCanonicalCurrentHeapQuery(
    const CanonicalCurrentHeapExecutionRequest& request);

}  // namespace scratchbird::engine::sblr

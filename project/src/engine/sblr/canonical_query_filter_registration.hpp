// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "canonical_relational_expression.hpp"

#include "engine/executor/executor_foundation.hpp"
#include "engine/internal_api/api_types.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace scratchbird::engine::sblr {

namespace exec = scratchbird::engine::executor;

struct LiveFilterRuntimeNodeConfiguration {
  std::uint32_t relational_node_id{0};
  std::uint32_t predicate_expression_id{0};
  CanonicalRelationalExpressionRowBinding predicate_row_binding;
};

// Registers exact-cardinality FILTER execution for the immutable relational
// route. The callback evaluates through a private predicate receipt and only
// revalidates the engine-selected MGA statement context.
exec::CanonicalPhysicalExecutorRegistration MakeLiveFilterRegistration(
    std::uint32_t predicate_expression_id,
    CanonicalRelationalExpressionRowBinding predicate_row_binding,
    scratchbird::engine::internal_api::TypedRelationalDag relational_dag,
    CanonicalRelationalExpressionRuntimeServices expression_services,
    std::string capability_uuid,
    std::size_t expected_input_row_count,
    scratchbird::engine::internal_api::EngineRequestContext mga_context);

// Registers row-dependent three-valued FILTER execution over one bounded,
// already-materialized typed input. The callback consumes optimizer-published
// memory bounds and can only revalidate engine-selected MGA authority.
exec::CanonicalPhysicalExecutorRegistration MakeLiveHeapFilterRegistration(
    std::uint32_t predicate_expression_id,
    CanonicalRelationalExpressionRowBinding predicate_row_binding,
    scratchbird::engine::internal_api::TypedRelationalDag relational_dag,
    CanonicalRelationalExpressionRuntimeServices expression_services,
    std::string capability_uuid,
    std::size_t maximum_input_row_count,
    scratchbird::engine::internal_api::EngineRequestContext mga_context,
    scratchbird::engine::internal_api::EngineCanonicalExpressionConsumer
        expression_consumer = scratchbird::engine::internal_api::
            EngineCanonicalExpressionConsumer::filter,
    scratchbird::engine::internal_api::EnginePredicateConsumer
        predicate_consumer = scratchbird::engine::internal_api::
            EnginePredicateConsumer::filter,
    const scratchbird::engine::internal_api::TypedRelationalDag*
        borrowed_relational_dag = nullptr,
    const scratchbird::engine::internal_api::EngineRequestContext*
        borrowed_mga_context = nullptr,
    const exec::CanonicalExecutionMgaAuthority* borrowed_mga_authority =
        nullptr,
    std::vector<LiveFilterRuntimeNodeConfiguration>
        runtime_node_configurations = {});

}  // namespace scratchbird::engine::sblr

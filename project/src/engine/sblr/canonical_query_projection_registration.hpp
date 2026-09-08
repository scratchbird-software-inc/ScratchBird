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
#include <limits>
#include <string>
#include <vector>

namespace scratchbird::engine::sblr {

namespace exec = scratchbird::engine::executor;

struct LiveProjectExpressionRegistration {
  std::uint32_t expression_id{0};
  std::string expected_type;
  CanonicalRelationalExpressionRowBinding row_binding;
};

struct LiveProjectRegistrationProfile {
  bool expression_projection{false};
  std::vector<std::size_t> projected_columns;
  std::vector<LiveProjectExpressionRegistration> expressions;
  std::vector<exec::ExecutorColumnDescriptor> expression_output_columns;
};

// Materializes one descriptor-exact expression projection. This helper owns no
// plan selection, storage access, snapshot construction, or transaction state.
bool MaterializeExpressionProjectBatch(
    const scratchbird::engine::internal_api::TypedRelationalDag& dag,
    const std::vector<LiveProjectExpressionRegistration>& expressions,
    const std::vector<exec::ExecutorColumnDescriptor>& output_columns,
    const exec::DescriptorBatch& input_batch,
    const CanonicalRelationalExpressionRuntimeServices& expression_services,
    exec::DescriptorBatch* output_batch,
    std::string* detail,
    std::uint64_t maximum_output_bytes =
        std::numeric_limits<std::uint64_t>::max(),
    std::uint64_t* actual_output_bytes = nullptr,
    bool* resource_refused = nullptr);

// Registers direct-column or expression PROJECT execution over one bounded,
// already-materialized typed input. The callback consumes optimizer-published
// memory bounds and can only revalidate engine-selected MGA authority.
exec::CanonicalPhysicalExecutorRegistration MakeLiveProjectRegistration(
    LiveProjectRegistrationProfile profile,
    std::string implementation_id,
    std::string capability_uuid,
    std::size_t expected_input_row_count,
    scratchbird::engine::internal_api::TypedRelationalDag relational_dag,
    CanonicalRelationalExpressionRuntimeServices expression_services,
    scratchbird::engine::internal_api::EngineRequestContext mga_context,
    bool input_row_count_is_upper_bound = false,
    const scratchbird::engine::internal_api::TypedRelationalDag*
        borrowed_relational_dag = nullptr,
    const scratchbird::engine::internal_api::EngineRequestContext*
        borrowed_mga_context = nullptr,
    const exec::CanonicalExecutionMgaAuthority* borrowed_mga_authority =
        nullptr);

}  // namespace scratchbird::engine::sblr

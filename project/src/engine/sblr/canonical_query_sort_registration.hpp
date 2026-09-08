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
#include <optional>
#include <string>
#include <vector>

namespace scratchbird::engine::sblr {

namespace exec = scratchbird::engine::executor;

struct PreparedSortExpression {
  std::uint32_t expression_id{0};
  std::string expected_type;
  CanonicalRelationalExpressionRowBinding row_binding;
  exec::ExecutorColumnDescriptor materialized_column;
  std::optional<scratchbird::engine::internal_api::EngineTypedValue>
      row_independent_value;
};

struct PreparedSortRoot {
  bool ok{false};
  bool expression_ordering{false};
  std::vector<exec::CanonicalDescriptorOrderTerm> order_terms;
  std::vector<PreparedSortExpression> expressions;
  exec::DescriptorBatch expression_input_batch;
  std::vector<exec::CanonicalResultColumnBinding> result_bindings;
  std::string ordering_property_uuid;
  std::string detail;
};

bool MaterializeExpressionSortBatch(
    const scratchbird::engine::internal_api::TypedRelationalDag& dag,
    const std::vector<PreparedSortExpression>& expressions,
    const exec::DescriptorBatch& input_batch,
    const CanonicalRelationalExpressionRuntimeServices& expression_services,
    exec::DescriptorBatch* sort_batch,
    std::string* detail,
    std::uint64_t maximum_batch_bytes =
        std::numeric_limits<std::uint64_t>::max(),
    exec::DescriptorCancellationProbe cancellation_requested = nullptr,
    const void* cancellation_context = nullptr,
    bool* cancellation_observed = nullptr,
    bool* cancellation_probe_failed = nullptr);

// Registers expression-aware SORT execution against exact optimizer-published
// order, memory, cancellation, and MGA evidence.
exec::CanonicalPhysicalExecutorRegistration
MakeLiveExpressionSortRegistration(
    PreparedSortRoot prepared,
    std::string deterministic_tie_evidence_uuid,
    std::string capability_uuid,
    std::size_t maximum_input_row_count,
    std::size_t maximum_pair_comparisons,
    scratchbird::engine::internal_api::TypedRelationalDag relational_dag,
    CanonicalRelationalExpressionRuntimeServices expression_services,
    scratchbird::engine::internal_api::EngineRequestContext mga_context,
    const scratchbird::engine::internal_api::TypedRelationalDag*
        borrowed_relational_dag = nullptr,
    const scratchbird::engine::internal_api::EngineRequestContext*
        borrowed_mga_context = nullptr,
    const exec::CanonicalExecutionMgaAuthority* borrowed_mga_authority =
        nullptr);

}  // namespace scratchbird::engine::sblr

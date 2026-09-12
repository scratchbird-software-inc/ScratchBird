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
#include "uuid.hpp"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <vector>

namespace scratchbird::engine::sblr {

namespace exec = scratchbird::engine::executor;

// Identity portion of expression-key receipt admission only. The issuer still
// verifies logical/physical lineage, full descriptor/value authority, MGA,
// cancellation, memory grants and actual key materialization independently.
inline bool CanonicalSortExpressionIdentityBinding(
    const scratchbird::engine::internal_api::EngineUuid& ordering_property_uuid,
    const scratchbird::engine::internal_api::EngineUuid& deterministic_tie_evidence_uuid,
    const scratchbird::engine::internal_api::EngineDescriptor& descriptor,
    const scratchbird::engine::internal_api::EngineUuid& collation_uuid) {
  using scratchbird::core::uuid::IsEngineIdentityUuid;
  return IsEngineIdentityUuid(ordering_property_uuid) &&
      IsEngineIdentityUuid(deterministic_tie_evidence_uuid) &&
      ordering_property_uuid != deterministic_tie_evidence_uuid &&
      IsEngineIdentityUuid(descriptor.descriptor_uuid) &&
      IsEngineIdentityUuid(descriptor.type_uuid) &&
      (collation_uuid.is_nil() || IsEngineIdentityUuid(collation_uuid)) &&
      descriptor.collation_uuid == collation_uuid &&
      deterministic_tie_evidence_uuid != descriptor.descriptor_uuid &&
      deterministic_tie_evidence_uuid != descriptor.type_uuid &&
      deterministic_tie_evidence_uuid != collation_uuid;
}

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
  scratchbird::engine::internal_api::EngineUuid ordering_property_uuid;
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
    scratchbird::engine::internal_api::EngineUuid deterministic_tie_evidence_uuid,
    scratchbird::engine::internal_api::EngineUuid capability_uuid,
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

// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "engine/executor/executor_foundation.hpp"
#include "engine/internal_api/api_types.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace scratchbird::engine::sblr {

namespace exec = scratchbird::engine::executor;

struct LiveNonrecursiveCteRuntimeNodeConfiguration {
  std::uint32_t relational_node_id{0};
};

struct LiveProjectRuntimeNodeConfiguration {
  std::uint32_t relational_node_id{0};
  std::vector<std::size_t> projected_columns;
};

enum class LiveCardinalitySubqueryKind : std::uint8_t {
  kScalar,
  kRow,
};

struct LiveCardinalitySubqueryRegistrationProfile {
  LiveCardinalitySubqueryKind kind{LiveCardinalitySubqueryKind::kScalar};
  std::vector<exec::ExecutorColumnDescriptor> result_columns;
  std::string implementation_id;
};

enum class LivePredicateSubqueryKind : std::uint8_t {
  kExists,
  kQuantified,
};

struct LivePredicateSubqueryRegistrationProfile {
  LivePredicateSubqueryKind kind{LivePredicateSubqueryKind::kExists};
  exec::ExecutorColumnDescriptor result_column;
  exec::ExecutorColumnDescriptor left_operand_column;
  scratchbird::engine::internal_api::EngineTypedValue left_value;
  std::uint32_t right_expression_descriptor_id{0};
  scratchbird::engine::internal_api::EngineComparisonPredicateOperator
      comparison_operator = scratchbird::engine::internal_api::
          EngineComparisonPredicateOperator::unspecified;
  exec::CanonicalQuantifiedSubqueryQuantifier quantifier =
      exec::CanonicalQuantifiedSubqueryQuantifier::kAny;
  bool comparison_authority_required{false};
  std::string implementation_id;
  std::string transformation_id;
};

struct CanonicalRelationalExpressionRuntimeServices;

// Registers direct descriptor projection over a bounded materialized input.
// The callback consumes an optimizer-published memory grant and either borrows
// existing MGA authority or constructs only a revalidation handle over the
// engine-selected statement context.
exec::CanonicalPhysicalExecutorRegistration MakeLiveHeapProjectRegistration(
    std::vector<std::size_t> projected_columns,
    std::string capability_uuid,
    std::size_t maximum_input_row_count,
    scratchbird::engine::internal_api::EngineRequestContext mga_context,
    const scratchbird::engine::internal_api::EngineRequestContext*
        borrowed_mga_context = nullptr,
    const exec::CanonicalExecutionMgaAuthority* borrowed_mga_authority =
        nullptr,
    std::vector<LiveProjectRuntimeNodeConfiguration>
        runtime_node_configurations = {});

// Registers query DISTINCT over a bounded, already-materialized input and
// optimizer-published equality terms. The callback can only revalidate the
// engine-selected MGA statement context.
exec::CanonicalPhysicalExecutorRegistration
MakeLiveQueryDistinctRegistration(
    std::vector<exec::CanonicalDescriptorOrderTerm> equality_terms,
    std::string capability_uuid,
    std::size_t maximum_input_row_count,
    std::size_t maximum_value_comparisons,
    scratchbird::engine::internal_api::EngineRequestContext mga_context);

// Registers LIMIT/OFFSET and FETCH FIRST over a bounded, already-materialized
// input. The callback consumes an optimizer-published memory grant when the
// dispatcher supplies borrowed MGA authority and otherwise creates only a
// revalidation handle over the engine-selected statement context.
exec::CanonicalPhysicalExecutorRegistration MakeLiveLimitRegistration(
    std::string implementation_id,
    std::string capability_uuid,
    std::uint64_t row_limit,
    std::uint64_t row_offset,
    bool fetch_first_rows_only,
    std::size_t maximum_input_row_count,
    scratchbird::engine::internal_api::EngineRequestContext mga_context,
    const scratchbird::engine::internal_api::EngineRequestContext*
        borrowed_mga_context = nullptr,
    const exec::CanonicalExecutionMgaAuthority* borrowed_mga_authority =
        nullptr);

// Registers global COUNT(*) over a bounded, already-materialized typed input
// or an exact engine-published streaming cardinality. The callback consumes an
// optimizer-published memory grant and can only revalidate MGA authority.
exec::CanonicalPhysicalExecutorRegistration MakeLiveCountStarRegistration(
    exec::ExecutorColumnDescriptor result_column,
    std::string capability_uuid,
    std::size_t maximum_input_row_count,
    scratchbird::engine::internal_api::EngineRequestContext mga_context,
    const scratchbird::engine::internal_api::EngineRequestContext*
        borrowed_mga_context = nullptr,
    const exec::CanonicalExecutionMgaAuthority* borrowed_mga_authority =
        nullptr);

// Registers canonical typed ordering over a bounded, already-materialized
// input. The callback consumes optimizer-published comparison/memory bounds,
// engine cancellation policy, and revalidation-only MGA authority.
exec::CanonicalPhysicalExecutorRegistration MakeLiveSortRegistration(
    std::vector<exec::CanonicalDescriptorOrderTerm> order_terms,
    std::string deterministic_tie_evidence_uuid,
    std::string capability_uuid,
    std::size_t maximum_input_row_count,
    std::size_t maximum_pair_comparisons,
    scratchbird::engine::internal_api::EngineRequestContext mga_context,
    const scratchbird::engine::internal_api::EngineRequestContext*
        borrowed_mga_context = nullptr,
    const exec::CanonicalExecutionMgaAuthority* borrowed_mga_authority =
        nullptr);

// Registers the bounded A+ row-pattern operator over one canonical int64
// input. The callback consumes optimizer-published pattern/memory bounds,
// engine cancellation policy, and revalidation-only MGA authority.
exec::CanonicalPhysicalExecutorRegistration
MakeLiveMatchRecognizeRegistration(
    std::string capability_uuid,
    std::size_t maximum_partition_rows,
    std::size_t maximum_active_states,
    std::size_t maximum_output_rows,
    const scratchbird::engine::internal_api::EngineRequestContext*
        borrowed_mga_context,
    const exec::CanonicalExecutionMgaAuthority* borrowed_mga_authority);

// Registers scalar and row cardinality subqueries over one bounded,
// already-materialized typed input. The callback can only revalidate the
// engine-selected MGA statement context.
exec::CanonicalPhysicalExecutorRegistration
MakeLiveCardinalitySubqueryRegistration(
    LiveCardinalitySubqueryRegistrationProfile profile,
    std::string capability_uuid,
    std::size_t maximum_input_row_count,
    scratchbird::engine::internal_api::EngineRequestContext mga_context);

// Registers EXISTS and quantified predicate subqueries over one bounded,
// already-materialized typed input. Comparison authority is supplied by the
// engine and the callback can only revalidate the selected MGA context.
exec::CanonicalPhysicalExecutorRegistration
MakeLivePredicateSubqueryRegistration(
    LivePredicateSubqueryRegistrationProfile profile,
    std::string capability_uuid,
    std::size_t maximum_input_row_count,
    CanonicalRelationalExpressionRuntimeServices expression_services,
    scratchbird::engine::internal_api::EngineRequestContext mga_context);

// Registers inline or materialized nonrecursive CTE publication over an exact
// bounded typed input. The callback consumes only optimizer-published runtime
// configuration and memory grants and can only revalidate MGA authority.
exec::CanonicalPhysicalExecutorRegistration MakeLiveNonrecursiveCteRegistration(
    std::string implementation_id,
    std::string capability_uuid,
    std::size_t maximum_input_row_count,
    scratchbird::engine::internal_api::EngineRequestContext mga_context,
    const scratchbird::engine::internal_api::EngineRequestContext*
        borrowed_mga_context = nullptr,
    const exec::CanonicalExecutionMgaAuthority* borrowed_mga_authority =
        nullptr,
    std::vector<LiveNonrecursiveCteRuntimeNodeConfiguration>
        runtime_node_configurations = {});

}  // namespace scratchbird::engine::sblr

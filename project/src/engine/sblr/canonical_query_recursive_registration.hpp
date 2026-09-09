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
#include <exception>
#include <string>
#include <string_view>
#include <vector>

namespace scratchbird::engine::sblr {

namespace exec = scratchbird::engine::executor;

struct LivePhysicalNodeProfile;

struct LiveRecursiveCteProfile {
  bool matched{false};
  bool search_cycle{false};
  exec::CanonicalRecursiveCteUnionMode union_mode =
      exec::CanonicalRecursiveCteUnionMode::kAll;
  bool emit_current_duplicate{false};
  std::string implementation_id;
  std::string transformation_id;
};

enum class LiveRecursiveCteTermMode : std::uint8_t {
  kBoundedIncrement = 1,
  kBoundedIncrementWithCurrent,
  kIncrementWrapToOne,
};

struct PreparedRecursiveCteTerm {
  LiveRecursiveCteTermMode mode{
      LiveRecursiveCteTermMode::kBoundedIncrement};
  std::vector<exec::ExecutorColumnDescriptor> columns;
  std::int64_t upper_bound{0};
};

struct LiveRecursiveCteTermExecution {
  exec::CanonicalRecursiveCteGeneratedBatch generated;
  bool ok{false};
  bool resource_refused{false};
  bool cancellation_observed{false};
  std::exception_ptr cancellation_probe_failure;
  std::string detail;
};

struct PreparedRecursiveCteRoot {
  LiveRecursiveCteProfile profile;
  std::vector<exec::ExecutorColumnDescriptor> anchor_columns;
  PreparedRecursiveCteTerm term;
  exec::ExecutorColumnDescriptor search_sequence_column;
  exec::ExecutorColumnDescriptor cycle_mark_column;
  std::size_t maximum_anchor_row_count{0};
  std::size_t maximum_iteration_count{0};
  std::size_t maximum_working_row_count{0};
  std::size_t maximum_term_output_row_count{0};
  std::size_t maximum_result_row_count{0};
  std::size_t maximum_value_comparison_count{0};
  std::size_t rows_examined{0};
  std::size_t planned_peak_payload_bytes{0};
  std::size_t planned_resident_structural_bytes{0};
  std::size_t planned_peak_memory_bytes{0};
};

bool BoundPreparedRecursiveCtePeakPayload(
    const PreparedRecursiveCteRoot& prepared,
    std::size_t* peak_payload_bytes);

bool BindPreparedRecursiveCtePeakMemory(
    PreparedRecursiveCteRoot* prepared,
    std::uint64_t memory_budget_bytes);

bool BindPreparedRecursiveCteCardinality(
    PreparedRecursiveCteRoot* prepared,
    const std::vector<LivePhysicalNodeProfile>& profiles,
    std::uint32_t anchor_logical_node_id,
    std::uint64_t recursive_upper_bound,
    std::size_t execution_row_ceiling);

PreparedRecursiveCteTerm PrepareLiveRecursiveCteTerm(
    const LiveRecursiveCteProfile& profile,
    std::vector<exec::ExecutorColumnDescriptor> columns,
    std::int64_t upper_bound);

bool LiveRecursiveCteTermNodeBound(
    const PreparedRecursiveCteTerm& prepared,
    const exec::PhysicalNodeRecord& node,
    std::string_view capability_uuid);

LiveRecursiveCteTermExecution ExecutePreparedRecursiveCteTerm(
    const PreparedRecursiveCteTerm& prepared,
    const exec::DescriptorBatch& current,
    std::size_t iteration,
    std::size_t maximum_output_row_count,
    const exec::CanonicalRecursiveCteCancellationProbe&
        cancellation_requested);

// Registers the descriptor-only recursive-term leaf. The callback can only
// validate and revalidate the engine-selected MGA statement context; it cannot
// select, refresh, or finalize transaction authority.
exec::CanonicalPhysicalExecutorRegistration
MakeLiveRecursiveCteTermRegistration(
    PreparedRecursiveCteTerm prepared,
    std::string capability_uuid,
    scratchbird::engine::internal_api::EngineRequestContext mga_context);

// Registers the bounded recursive root over optimizer-published memory and
// cancellation evidence. The callback only revalidates the engine-selected
// MGA statement context.
exec::CanonicalPhysicalExecutorRegistration MakeLiveRecursiveCteRegistration(
    PreparedRecursiveCteRoot prepared,
    std::string recursive_term_capability_uuid,
    std::string capability_uuid,
    scratchbird::engine::internal_api::EngineRequestContext mga_context);

}  // namespace scratchbird::engine::sblr

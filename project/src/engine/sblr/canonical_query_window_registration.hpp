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
#include <optional>
#include <string>
#include <string_view>

namespace scratchbird::engine::sblr {

namespace exec = scratchbird::engine::executor;

// Immutable identity/profile data shared by window binding and callback
// registration. It carries no planning, execution, or transaction authority.
struct GlobalRankingWindowProfile {
  std::string_view semantic_variant_id;
  std::string_view builtin_id;
  std::string_view function_uuid;
  std::string_view display_name;
  std::string_view result_type_name;
};

// Registers the ROW_NUMBER callback over optimizer-published physical and
// engine-selected MGA authority. Borrowed authority is revalidated only; this
// factory cannot create, refresh, or finalize a transaction or snapshot.
exec::CanonicalPhysicalExecutorRegistration MakeLiveRowNumberRegistration(
    exec::ExecutorColumnDescriptor row_number_column,
    std::string deterministic_order_evidence_uuid,
    std::string capability_uuid,
    std::size_t maximum_input_row_count,
    scratchbird::engine::internal_api::EngineRequestContext mga_context,
    const scratchbird::engine::internal_api::EngineRequestContext*
        borrowed_mga_context = nullptr,
    const exec::CanonicalExecutionMgaAuthority* borrowed_mga_authority =
        nullptr);

// Registers NTILE over a bounded, optimizer-published sorted input. The
// callback consumes and revalidates an engine-selected MGA statement context;
// it cannot create, refresh, or finalize a transaction or snapshot.
exec::CanonicalPhysicalExecutorRegistration MakeLiveNtileRegistration(
    exec::ExecutorColumnDescriptor ntile_column,
    exec::CanonicalDescriptorOrderTerm order_term,
    scratchbird::engine::internal_api::EngineTypedValue bucket_count_operand,
    std::string function_uuid,
    std::string order_term_binding_evidence_uuid,
    std::string deterministic_order_evidence_uuid,
    std::string capability_uuid,
    std::size_t maximum_input_row_count,
    scratchbird::engine::internal_api::EngineRequestContext mga_context);

// Registers RANK/DENSE_RANK/PERCENT_RANK/CUME_DIST over a bounded,
// optimizer-published sorted input. The callback consumes explicit peer
// metadata and can only revalidate the engine-selected MGA statement context.
exec::CanonicalPhysicalExecutorRegistration MakeLivePeerRankingRegistration(
    exec::ExecutorColumnDescriptor ranking_column,
    exec::CanonicalDescriptorOrderTerm order_term,
    std::string order_term_binding_evidence_uuid,
    std::string deterministic_order_evidence_uuid,
    std::string capability_uuid,
    std::size_t maximum_input_row_count,
    std::size_t maximum_peer_comparisons,
    GlobalRankingWindowProfile profile,
    scratchbird::engine::internal_api::EngineRequestContext mga_context);

// Registers LAG/LEAD/FIRST_VALUE/LAST_VALUE/NTH_VALUE over a bounded,
// optimizer-published sorted input and frame. The callback only revalidates
// the engine-selected MGA statement context.
exec::CanonicalPhysicalExecutorRegistration
MakeLiveNavigationWindowRegistration(
    exec::ExecutorColumnDescriptor result_column,
    exec::CanonicalDescriptorOrderTerm order_term,
    std::size_t value_column,
    std::optional<scratchbird::engine::internal_api::EngineTypedValue>
        nth_value_position_operand,
    std::string window_frame_descriptor_uuid,
    std::string order_term_binding_evidence_uuid,
    std::string deterministic_order_evidence_uuid,
    std::string frame_property_binding_evidence_uuid,
    std::string capability_uuid,
    std::size_t maximum_input_row_count,
    std::size_t maximum_pair_comparisons,
    std::size_t maximum_effective_row_references,
    GlobalRankingWindowProfile profile,
    scratchbird::engine::internal_api::EngineRequestContext mga_context);

// Registers aggregate-window execution over a bounded, optimizer-published
// frame. Runtime work is checked against the published grant and the callback
// can only revalidate the engine-selected MGA statement context.
exec::CanonicalPhysicalExecutorRegistration
MakeLiveAggregateWindowRegistration(
    exec::ExecutorColumnDescriptor result_column,
    exec::CanonicalDescriptorOrderTerm order_term,
    std::optional<std::size_t> value_column,
    exec::CanonicalAggregateDescriptor aggregate_descriptor,
    std::string window_frame_descriptor_uuid,
    std::string order_term_binding_evidence_uuid,
    std::string deterministic_order_evidence_uuid,
    std::string frame_property_binding_evidence_uuid,
    std::string capability_uuid,
    std::size_t maximum_input_row_count,
    std::size_t maximum_pair_comparisons,
    std::size_t maximum_effective_row_references,
    std::size_t maximum_transition_count,
    scratchbird::engine::internal_api::EngineRequestContext mga_context);

}  // namespace scratchbird::engine::sblr

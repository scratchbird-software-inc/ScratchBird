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
#include <optional>
#include <string>
#include <unordered_map>

#if !defined(SCRATCHBIRD_QOW_QUERY_ROUTE_CONTRACT_ONLY)
namespace scratchbird::transaction::mga {
struct SnapshotVectorDescriptor;
}
#endif

namespace scratchbird::engine::sblr {

namespace exec = scratchbird::engine::executor;

#if !defined(SCRATCHBIRD_QOW_QUERY_ROUTE_CONTRACT_ONLY)
exec::PhysicalMgaStatementContext PhysicalMgaContextFromResolvedSnapshot(
    const scratchbird::engine::internal_api::EngineRequestContext& context,
    const scratchbird::transaction::mga::SnapshotVectorDescriptor& descriptor);
#endif

// Builds a revalidation-only handle over the MGA statement context already
// selected by engine authority. This helper cannot begin, commit, roll back,
// persist, or recover a transaction.
exec::CanonicalExecutionMgaAuthority BuildCanonicalExecutionMgaAuthority(
    const scratchbird::engine::internal_api::EngineRequestContext& context,
    const exec::TypedPhysicalNodeDag& physical_dag);

// Retains the selected operator and its transitive physical inputs while
// preserving the optimizer-published identity and admission evidence.
bool BuildOperatorLocalPhysicalDag(
    const exec::TypedPhysicalNodeDag& dag,
    std::uint64_t root_physical_node_id,
    exec::TypedPhysicalNodeDag* operator_dag,
    std::string* detail);

std::optional<std::uint64_t> BoundOperatorLocalPhysicalDagCopyMemoryBytes(
    const exec::TypedPhysicalNodeDag& dag);

bool RebindOperatorLocalPhysicalMemoryGrant(
    exec::PhysicalNodeRecord* node,
    exec::TypedPhysicalNodeDag* dag,
    std::uint64_t rebound_memory_bytes,
    std::string* detail);

bool BuildStrictUnaryOperatorLocalPhysicalDag(
    const exec::TypedPhysicalNodeDag& dag,
    const exec::PhysicalNodeRecord& node,
    const exec::DescriptorBatch& input_batch,
    std::uint64_t maximum_additional_batch_copies,
    std::uint64_t auxiliary_memory_bytes,
    exec::TypedPhysicalNodeDag* operator_dag,
    std::size_t* callback_memory_bound,
    std::string* detail);

bool BuildStrictBinaryOperatorLocalPhysicalDag(
    const exec::TypedPhysicalNodeDag& dag,
    const exec::PhysicalNodeRecord& node,
    const exec::DescriptorBatch& left_input_batch,
    const exec::DescriptorBatch& right_input_batch,
    std::uint64_t auxiliary_memory_bytes,
    exec::TypedPhysicalNodeDag* operator_dag,
    std::size_t* callback_memory_bound,
    std::string* detail);

// Adapts the engine-owned cancellation callback and binds one exact
// optimizer-published cancellation-policy evidence row to dispatch failure.
bool InvokeLiveSortCancellationProbe(const void* context);

const exec::PhysicalAdmissionEvidence* FindLiveCancellationPolicy(
    const exec::TypedPhysicalNodeDag& dag);

void BindLiveCancellationFailure(
    exec::DescriptorRuntimeDiagnostic diagnostic,
    const exec::PhysicalAdmissionEvidence* cancellation_policy,
    exec::CanonicalPhysicalDispatchStepResult* step);

template <typename ExecutionReceipt>
bool CanonicalOperatorExecutionReceiptMatches(
    const ExecutionReceipt& receipt,
    const exec::TypedPhysicalNodeDag& execution_dag,
    const exec::PhysicalNodeRecord& node,
    const exec::PhysicalMgaStatementContext& expected_mga_context) {
  return receipt.selected_plan_uuid == execution_dag.selected_plan_uuid &&
         receipt.executed_physical_node_id == node.physical_node_id &&
         receipt.causal_counter_id == node.causal_counter_id &&
         exec::PhysicalMgaStatementContextEqual(
             receipt.mga_statement_context, expected_mga_context);
}

#if defined(SCRATCHBIRD_QOW_QUERY_ROUTE_CONTRACT_ONLY)
void ArmCanonicalPhysicalRegistrationPreResultRevocationForContractTest();
std::size_t CanonicalPhysicalRegistrationRevalidationCountForTest();
#endif

// Registers a bounded, already-materialized object-free source. The callback
// may revalidate borrowed MGA authority, but cannot create or alter it.
exec::CanonicalPhysicalExecutorRegistration
MakeLiveMaterializedSourceRegistration(
    std::unordered_map<std::uint64_t, exec::DescriptorBatch> batches,
    std::string capability_uuid,
    std::string diagnostic_id,
    std::string operation_name,
    exec::PhysicalNodeKind node_kind,
    std::string implementation_id,
    std::string payload_name,
    bool strict_dispatcher_memory,
    const exec::CanonicalExecutionMgaAuthority* borrowed_mga_authority);

exec::CanonicalPhysicalExecutorRegistration MakeLiveValuesRegistration(
    std::unordered_map<std::uint64_t, exec::DescriptorBatch> batches,
    std::string capability_uuid,
    std::string diagnostic_id,
    std::string operation_name,
    bool strict_dispatcher_memory = false);

// Registers the bounded table-subquery materialization callback over an
// engine-selected statement snapshot. The callback revalidates MGA authority
// but cannot create or refresh that authority.
exec::CanonicalPhysicalExecutorRegistration
MakeLiveTableSubqueryRegistration(
    std::string capability_uuid,
    std::size_t maximum_input_row_count,
    scratchbird::engine::internal_api::EngineRequestContext mga_context);

}  // namespace scratchbird::engine::sblr

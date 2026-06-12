// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// CEIC-012 executor bridge for reservation-backed memory resources.
#include "reservation_backed_executor_memory_bridge.hpp"

#include "runtime_platform.hpp"

#include <cstddef>
#include <utility>

namespace scratchbird::engine::executor {
namespace {

using scratchbird::core::platform::MakeDiagnostic;
using scratchbird::core::platform::Severity;
using scratchbird::core::platform::Status;
using scratchbird::core::platform::StatusCode;
using scratchbird::core::platform::Subsystem;

constexpr const char* kAnchor =
    "CEIC-012_QUERY_OPERATOR_PLANNER_PARSER_ARENAS";

Status OkStatus() {
  return {StatusCode::ok, Severity::info, Subsystem::engine};
}

Status ErrorStatus() {
  return {StatusCode::memory_invalid_request, Severity::error, Subsystem::engine};
}

bool UnsafeAuthority(const ExecutorOperatorMemoryAuthority& authority) {
  return !authority.engine_mga_snapshot_bound ||
         !authority.transaction_inventory_authoritative ||
         !authority.security_recheck_required ||
         authority.parser_client_or_reference_memory_authority ||
         authority.memory_visibility_or_finality_authority ||
         authority.memory_recovery_authority ||
         authority.memory_authorization_authority;
}

const char* OperatorKindName(ExecutorMemoryOperatorKind kind) {
  switch (kind) {
    case ExecutorMemoryOperatorKind::scan: return "scan";
    case ExecutorMemoryOperatorKind::sort: return "sort";
    case ExecutorMemoryOperatorKind::hash_join: return "hash_join";
    case ExecutorMemoryOperatorKind::merge_join: return "merge_join";
    case ExecutorMemoryOperatorKind::aggregate: return "aggregate";
    case ExecutorMemoryOperatorKind::window: return "window";
    case ExecutorMemoryOperatorKind::candidate_set: return "candidate_set";
    case ExecutorMemoryOperatorKind::vector_search: return "vector_search";
    case ExecutorMemoryOperatorKind::full_text_search: return "full_text_search";
    case ExecutorMemoryOperatorKind::graph_traversal: return "graph_traversal";
    case ExecutorMemoryOperatorKind::document_path: return "document_path";
    case ExecutorMemoryOperatorKind::time_series_rollup: return "time_series_rollup";
    case ExecutorMemoryOperatorKind::dml_write: return "dml_write";
    case ExecutorMemoryOperatorKind::streaming_result: return "streaming_result";
  }
  return "unknown";
}

ExecutorReservationBackedMemoryResult Refuse(
    ExecutorMemoryOperatorKind operator_kind,
    std::string purpose,
    std::string code,
    std::string message,
    std::string reason) {
  ExecutorReservationBackedMemoryResult result;
  result.status = ErrorStatus();
  result.fail_closed = true;
  result.diagnostic = MakeDiagnostic(
      result.status.code,
      result.status.severity,
      result.status.subsystem,
      std::move(code),
      std::move(message),
      {{"operator_kind", OperatorKindName(operator_kind)},
       {"purpose", std::move(purpose)},
       {"reason", std::move(reason)}},
      {},
      "engine.executor.reservation_backed_memory",
      "executor hot paths require a CEIC-011 committed reservation-backed resource");
  result.evidence.push_back(kAnchor);
  result.evidence.push_back("executor.reservation_backed_memory.fail_closed=true");
  result.evidence.push_back("executor.reservation_backed_memory.operator_kind=" +
                            std::string(OperatorKindName(operator_kind)));
  result.evidence.push_back(
      "executor.reservation_backed_memory.authority_scope=evidence_only_not_transaction_finality_visibility_recovery_parser_reference_benchmark_cluster_optimizer_index_or_agent_authority");
  return result;
}

}  // namespace

ExecutorReservationBackedMemoryResult AllocateExecutorOperatorFromReservedResource(
    scratchbird::core::memory::ReservationBackedMemoryResource* resource,
    ExecutorMemoryOperatorKind operator_kind,
    scratchbird::core::platform::u64 bytes,
    std::string purpose,
    ExecutorOperatorMemoryAuthority authority) {
  if (resource == nullptr || !resource->active()) {
    return Refuse(operator_kind,
                  std::move(purpose),
                  "SB_CEIC_012_EXECUTOR_MEMORY.RESOURCE_REQUIRED",
                  "executor.ceic_012.memory.resource_required",
                  "active_reservation_backed_resource_required");
  }
  if (UnsafeAuthority(authority)) {
    return Refuse(operator_kind,
                  std::move(purpose),
                  "SB_CEIC_012_EXECUTOR_MEMORY.UNSAFE_AUTHORITY",
                  "executor.ceic_012.memory.unsafe_authority",
                  "mga_security_and_non_authority_evidence_required");
  }

  scratchbird::core::memory::ReservationBackedMemoryAllocationRequest allocation;
  allocation.bytes = bytes;
  allocation.alignment = alignof(std::max_align_t);
  allocation.purpose =
      purpose.empty()
          ? std::string("executor.operator.") +
                OperatorKindName(operator_kind)
          : std::move(purpose);
  const auto allocated = resource->Allocate(std::move(allocation));
  if (!allocated.ok()) {
    ExecutorReservationBackedMemoryResult result;
    result.status = allocated.status;
    result.fail_closed = true;
    result.diagnostic = allocated.diagnostic;
    result.evidence.push_back(kAnchor);
    result.evidence.push_back("executor.reservation_backed_memory.fail_closed=true");
    result.evidence.push_back("executor.reservation_backed_memory.refused=allocation_refused");
    return result;
  }

  ExecutorReservationBackedMemoryResult result;
  result.status = OkStatus();
  result.allocated_bytes = allocated.bytes;
  result.evidence.push_back(kAnchor);
  result.evidence.push_back("executor.reservation_backed_memory.operator_kind=" +
                            std::string(OperatorKindName(operator_kind)));
  result.evidence.push_back("executor.reservation_backed_memory.allocated_bytes=" +
                            std::to_string(result.allocated_bytes));
  result.evidence.push_back("executor.reservation_backed_memory.resource_passed=true");
  result.evidence.push_back("executor.reservation_backed_memory.after_reservation=true");
  result.evidence.push_back(
      "executor.reservation_backed_memory.authority_scope=evidence_only_not_transaction_finality_visibility_recovery_parser_reference_benchmark_cluster_optimizer_index_or_agent_authority");
  return result;
}

}  // namespace scratchbird::engine::executor

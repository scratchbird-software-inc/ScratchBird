// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// CEIC-012 optimizer bridge for reservation-backed temporary memory.
#include "reservation_backed_optimizer_memory_bridge.hpp"

#include "runtime_platform.hpp"

#include <iomanip>
#include <limits>
#include <sstream>
#include <utility>

namespace scratchbird::engine::optimizer {
namespace {

using scratchbird::core::platform::MakeDiagnostic;
using scratchbird::core::platform::Severity;
using scratchbird::core::platform::Status;
using scratchbird::core::platform::StatusCode;
using scratchbird::core::platform::Subsystem;
using scratchbird::core::platform::u64;

constexpr const char* kAnchor =
    "CEIC-012_QUERY_OPERATOR_PLANNER_PARSER_ARENAS";

struct OptimizerScratchCandidate {
  u64 ordinal = 0;
  u64 cost = 0;
  u64 rows = 0;
};

Status OkStatus() {
  return {StatusCode::ok, Severity::info, Subsystem::engine};
}

Status ErrorStatus(StatusCode code = StatusCode::memory_invalid_request) {
  return {code, Severity::error, Subsystem::engine};
}

std::string Digest(u64 candidate_count, u64 total_cost, u64 total_rows) {
  u64 hash = 1469598103934665603ull;
  auto mix = [&hash](u64 value) {
    for (int i = 0; i < 8; ++i) {
      hash ^= (value >> (i * 8)) & 0xffu;
      hash *= 1099511628211ull;
    }
  };
  mix(candidate_count);
  mix(total_cost);
  mix(total_rows);
  std::ostringstream out;
  out << "optimizer-reserved-arena-v1:" << std::hex << std::setw(16)
      << std::setfill('0') << hash;
  return out.str();
}

OptimizerReservationBackedMemoryResult Refuse(
    std::string route,
    std::string code,
    std::string message,
    std::string reason,
    StatusCode status_code = StatusCode::memory_invalid_request) {
  OptimizerReservationBackedMemoryResult result;
  result.status = ErrorStatus(status_code);
  result.fail_closed = true;
  result.diagnostic = MakeDiagnostic(
      result.status.code,
      result.status.severity,
      result.status.subsystem,
      std::move(code),
      std::move(message),
      {{"planning_route_label", std::move(route)}, {"reason", std::move(reason)}},
      {},
      "engine.optimizer.reservation_backed_memory",
      "optimizer temporary work requires a CEIC-011 committed reservation-backed resource");
  result.evidence.push_back(kAnchor);
  result.evidence.push_back("optimizer.reservation_backed_memory.fail_closed=true");
  result.evidence.push_back(
      "optimizer.reservation_backed_memory.authority_scope=evidence_only_not_transaction_finality_visibility_recovery_parser_reference_benchmark_cluster_optimizer_plan_index_or_agent_authority");
  return result;
}

}  // namespace

OptimizerReservationBackedMemoryResult BuildOptimizerTemporaryWorkFromReservedResource(
    scratchbird::core::memory::ReservationBackedMemoryResource* resource,
    std::string planning_route_label,
    u64 candidate_count,
    bool catalog_stats_authoritative,
    bool parser_or_reference_authority,
    bool memory_benchmark_authority) {
  if (resource == nullptr || !resource->active()) {
    return Refuse(std::move(planning_route_label),
                  "SB_CEIC_012_OPTIMIZER_MEMORY.RESOURCE_REQUIRED",
                  "optimizer.ceic_012.memory.resource_required",
                  "active_reservation_backed_resource_required");
  }
  if (planning_route_label.empty() || candidate_count == 0) {
    return Refuse(std::move(planning_route_label),
                  "SB_CEIC_012_OPTIMIZER_MEMORY.REQUEST_INVALID",
                  "optimizer.ceic_012.memory.request_invalid",
                  "planning_route_and_candidate_count_required");
  }
  if (!catalog_stats_authoritative || parser_or_reference_authority ||
      memory_benchmark_authority) {
    return Refuse(std::move(planning_route_label),
                  "SB_CEIC_012_OPTIMIZER_MEMORY.UNSAFE_AUTHORITY",
                  "optimizer.ceic_012.memory.unsafe_authority",
                  "catalog_stats_authority_required_and_parser_reference_benchmark_memory_authority_refused");
  }
  if (candidate_count >
      std::numeric_limits<u64>::max() / sizeof(OptimizerScratchCandidate)) {
    return Refuse(std::move(planning_route_label),
                  "SB_CEIC_012_OPTIMIZER_MEMORY.SIZE_OVERFLOW",
                  "optimizer.ceic_012.memory.size_overflow",
                  "candidate_count_overflow",
                  StatusCode::memory_limit_exceeded);
  }

  scratchbird::core::memory::ReservationBackedMemoryAllocationRequest allocation;
  allocation.bytes = candidate_count * sizeof(OptimizerScratchCandidate);
  allocation.alignment = alignof(OptimizerScratchCandidate);
  allocation.purpose = "optimizer.temporary_work";
  const auto allocated = resource->Allocate(std::move(allocation));
  if (!allocated.ok()) {
    OptimizerReservationBackedMemoryResult result;
    result.status = allocated.status;
    result.fail_closed = true;
    result.diagnostic = allocated.diagnostic;
    result.evidence.push_back(kAnchor);
    result.evidence.push_back("optimizer.reservation_backed_memory.fail_closed=true");
    result.evidence.push_back("optimizer.reservation_backed_memory.refused=allocation_refused");
    return result;
  }

  auto* candidates = static_cast<OptimizerScratchCandidate*>(allocated.pointer);
  u64 total_cost = 0;
  u64 total_rows = 0;
  for (u64 i = 0; i < candidate_count; ++i) {
    candidates[i] = OptimizerScratchCandidate{i, 100 + i * 17, 10 + i};
    total_cost += candidates[i].cost;
    total_rows += candidates[i].rows;
  }

  OptimizerReservationBackedMemoryResult result;
  result.status = OkStatus();
  result.candidate_count = candidate_count;
  result.allocated_bytes = allocated.bytes;
  result.digest = Digest(candidate_count, total_cost, total_rows);
  result.evidence.push_back(kAnchor);
  result.evidence.push_back("optimizer.reservation_backed_memory.route_label=" +
                            planning_route_label);
  result.evidence.push_back("optimizer.reservation_backed_memory.resource_passed=true");
  result.evidence.push_back("optimizer.reservation_backed_memory.after_reservation=true");
  result.evidence.push_back("optimizer.reservation_backed_memory.candidate_count=" +
                            std::to_string(candidate_count));
  result.evidence.push_back("optimizer.reservation_backed_memory.digest=" +
                            result.digest);
  result.evidence.push_back(
      "optimizer.reservation_backed_memory.authority_scope=evidence_only_not_transaction_finality_visibility_recovery_parser_reference_benchmark_cluster_optimizer_plan_index_or_agent_authority");
  return result;
}

}  // namespace scratchbird::engine::optimizer

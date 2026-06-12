// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// CEIC-012 planner bridge for reservation-backed temporary memory.
#include "reservation_backed_planner_memory_bridge.hpp"

#include "runtime_platform.hpp"

#include <cstddef>
#include <iomanip>
#include <limits>
#include <sstream>
#include <utility>

namespace scratchbird::engine::planner {
namespace {

using scratchbird::core::platform::MakeDiagnostic;
using scratchbird::core::platform::Severity;
using scratchbird::core::platform::Status;
using scratchbird::core::platform::StatusCode;
using scratchbird::core::platform::Subsystem;
using scratchbird::core::platform::u64;

constexpr const char* kAnchor =
    "CEIC-012_QUERY_OPERATOR_PLANNER_PARSER_ARENAS";

struct PlannerScratchNode {
  u64 ordinal = 0;
  u64 shape_hash = 0;
};

Status OkStatus() {
  return {StatusCode::ok, Severity::info, Subsystem::engine};
}

Status ErrorStatus(StatusCode code = StatusCode::memory_invalid_request) {
  return {code, Severity::error, Subsystem::engine};
}

std::string Digest(u64 node_count, u64 sum) {
  u64 hash = 1469598103934665603ull;
  auto mix = [&hash](u64 value) {
    for (int i = 0; i < 8; ++i) {
      hash ^= (value >> (i * 8)) & 0xffu;
      hash *= 1099511628211ull;
    }
  };
  mix(node_count);
  mix(sum);
  std::ostringstream out;
  out << "planner-reserved-arena-v1:" << std::hex << std::setw(16)
      << std::setfill('0') << hash;
  return out.str();
}

PlannerReservationBackedMemoryResult Refuse(std::string route,
                                            std::string code,
                                            std::string message,
                                            std::string reason,
                                            StatusCode status_code =
                                                StatusCode::memory_invalid_request) {
  PlannerReservationBackedMemoryResult result;
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
      "engine.planner.reservation_backed_memory",
      "planner temporary work requires a CEIC-011 committed reservation-backed resource");
  result.evidence.push_back(kAnchor);
  result.evidence.push_back("planner.reservation_backed_memory.fail_closed=true");
  result.evidence.push_back(
      "planner.reservation_backed_memory.authority_scope=evidence_only_not_transaction_finality_visibility_recovery_parser_reference_benchmark_cluster_optimizer_index_or_agent_authority");
  return result;
}

}  // namespace

PlannerReservationBackedMemoryResult BuildPlannerTemporaryWorkFromReservedResource(
    scratchbird::core::memory::ReservationBackedMemoryResource* resource,
    std::string planning_route_label,
    u64 planned_node_count,
    bool parser_or_reference_authority,
    bool memory_plan_authority) {
  if (resource == nullptr || !resource->active()) {
    return Refuse(std::move(planning_route_label),
                  "SB_CEIC_012_PLANNER_MEMORY.RESOURCE_REQUIRED",
                  "planner.ceic_012.memory.resource_required",
                  "active_reservation_backed_resource_required");
  }
  if (planning_route_label.empty() || planned_node_count == 0) {
    return Refuse(std::move(planning_route_label),
                  "SB_CEIC_012_PLANNER_MEMORY.REQUEST_INVALID",
                  "planner.ceic_012.memory.request_invalid",
                  "planning_route_and_node_count_required");
  }
  if (parser_or_reference_authority || memory_plan_authority) {
    return Refuse(std::move(planning_route_label),
                  "SB_CEIC_012_PLANNER_MEMORY.UNSAFE_AUTHORITY",
                  "planner.ceic_012.memory.unsafe_authority",
                  "parser_reference_or_memory_plan_authority_refused");
  }
  if (planned_node_count >
      std::numeric_limits<u64>::max() / sizeof(PlannerScratchNode)) {
    return Refuse(std::move(planning_route_label),
                  "SB_CEIC_012_PLANNER_MEMORY.SIZE_OVERFLOW",
                  "planner.ceic_012.memory.size_overflow",
                  "planned_node_count_overflow",
                  StatusCode::memory_limit_exceeded);
  }

  scratchbird::core::memory::ReservationBackedMemoryAllocationRequest allocation;
  allocation.bytes = planned_node_count * sizeof(PlannerScratchNode);
  allocation.alignment = alignof(PlannerScratchNode);
  allocation.purpose = "planner.temporary_work";
  const auto allocated = resource->Allocate(std::move(allocation));
  if (!allocated.ok()) {
    PlannerReservationBackedMemoryResult result;
    result.status = allocated.status;
    result.fail_closed = true;
    result.diagnostic = allocated.diagnostic;
    result.evidence.push_back(kAnchor);
    result.evidence.push_back("planner.reservation_backed_memory.fail_closed=true");
    result.evidence.push_back("planner.reservation_backed_memory.refused=allocation_refused");
    return result;
  }

  auto* nodes = static_cast<PlannerScratchNode*>(allocated.pointer);
  u64 sum = 0;
  for (u64 i = 0; i < planned_node_count; ++i) {
    nodes[i] = PlannerScratchNode{i, 0x9e3779b97f4a7c15ull ^ (i * 131u)};
    sum += nodes[i].shape_hash;
  }

  PlannerReservationBackedMemoryResult result;
  result.status = OkStatus();
  result.planned_node_count = planned_node_count;
  result.allocated_bytes = allocated.bytes;
  result.digest = Digest(planned_node_count, sum);
  result.evidence.push_back(kAnchor);
  result.evidence.push_back("planner.reservation_backed_memory.route_label=" +
                            planning_route_label);
  result.evidence.push_back("planner.reservation_backed_memory.resource_passed=true");
  result.evidence.push_back("planner.reservation_backed_memory.after_reservation=true");
  result.evidence.push_back("planner.reservation_backed_memory.node_count=" +
                            std::to_string(planned_node_count));
  result.evidence.push_back("planner.reservation_backed_memory.digest=" +
                            result.digest);
  result.evidence.push_back(
      "planner.reservation_backed_memory.authority_scope=evidence_only_not_transaction_finality_visibility_recovery_parser_reference_benchmark_cluster_optimizer_index_or_agent_authority");
  return result;
}

}  // namespace scratchbird::engine::planner

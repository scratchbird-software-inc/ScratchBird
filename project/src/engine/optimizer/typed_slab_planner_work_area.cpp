// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// CEIC-013 optimizer/planner adapter for typed slab hot work areas.
#include "typed_slab_planner_work_area.hpp"

#include "runtime_platform.hpp"

#include <array>
#include <utility>

namespace scratchbird::engine::optimizer {
namespace {

using scratchbird::core::memory::TypedSlabPool;
using scratchbird::core::memory::TypedSlabPoolObjectKind;
using scratchbird::core::platform::MakeDiagnostic;
using scratchbird::core::platform::Severity;
using scratchbird::core::platform::StatusCode;
using scratchbird::core::platform::Subsystem;

constexpr const char* kAuthorityScope =
    "optimizer.planner_typed_slab.authority_scope=evidence_only_not_transaction_finality_visibility_recovery_parser_donor_benchmark_cluster_optimizer_plan_or_index_finality_authority";

struct PlannerNode {
  u64 node_id = 0;
  u64 estimated_rows = 0;
  std::array<u64, 6> child_refs{};
};

struct CandidateChunk {
  u64 chunk_id = 0;
  u64 candidate_count = 0;
  std::array<double, 8> costs{};
};

Status OkStatus() {
  return {StatusCode::ok, Severity::info, Subsystem::engine};
}

PlannerTypedSlabWorkAreaResult Refuse(PlannerTypedSlabWorkAreaRequest request,
                                      std::string code,
                                      std::string message,
                                      std::string reason) {
  PlannerTypedSlabWorkAreaResult result;
  result.status = {StatusCode::memory_invalid_request, Severity::error, Subsystem::engine};
  result.fail_closed = true;
  result.diagnostic = MakeDiagnostic(result.status.code,
                                     result.status.severity,
                                     result.status.subsystem,
                                     std::move(code),
                                     std::move(message),
                                     {{"route_label", request.route_label},
                                      {"reason", std::move(reason)}},
                                     {},
                                     "engine.optimizer.typed_slab_planner_work_area",
                                     "planner typed slabs require a reservation-backed CEIC-013 size-class allocator");
  result.evidence.push_back("CEIC-013_TYPED_SLAB_POOLS_SIZE_CLASS_ALLOCATORS");
  result.evidence.push_back(kAuthorityScope);
  result.evidence.push_back("optimizer.planner_typed_slab.fail_closed=true");
  return result;
}

}  // namespace

PlannerTypedSlabWorkAreaResult BuildPlannerTypedSlabWorkArea(
    PlannerTypedSlabWorkAreaRequest request) {
  if (request.allocator == nullptr) {
    return Refuse(std::move(request),
                  "SB_CEIC_013_PLANNER_TYPED_SLAB.ALLOCATOR_REQUIRED",
                  "optimizer.ceic_013.planner_typed_slab.allocator_required",
                  "allocator_required");
  }
  if (request.route_label.empty() || request.candidate_count == 0) {
    return Refuse(std::move(request),
                  "SB_CEIC_013_PLANNER_TYPED_SLAB.ROUTE_REQUIRED",
                  "optimizer.ceic_013.planner_typed_slab.route_required",
                  "route_and_candidate_count_required");
  }
  if (!request.catalog_stats_authoritative ||
      request.parser_or_donor_authority ||
      request.memory_plan_authority) {
    return Refuse(std::move(request),
                  "SB_CEIC_013_PLANNER_TYPED_SLAB.UNSAFE_AUTHORITY",
                  "optimizer.ceic_013.planner_typed_slab.unsafe_authority",
                  "catalog_stats_and_non_authority_evidence_required");
  }

  TypedSlabPool<PlannerNode> nodes(
      request.allocator,
      TypedSlabPoolObjectKind::planner_node,
      "planner_node");
  TypedSlabPool<CandidateChunk> chunks(
      request.allocator,
      TypedSlabPoolObjectKind::candidate_chunk,
      "planner_candidate_chunk");

  PlannerTypedSlabWorkAreaResult result;
  result.status = OkStatus();
  result.candidate_count = request.candidate_count;
  for (u64 i = 0; i < request.candidate_count; ++i) {
    auto node = nodes.Make(i, 100 + i);
    if (!node.ok()) {
      result.status = node.status;
      result.fail_closed = true;
      result.diagnostic = node.diagnostic;
      result.evidence = node.evidence;
      return result;
    }
    ++result.typed_object_count;
  }
  auto chunk = chunks.Make(1, request.candidate_count);
  if (!chunk.ok()) {
    result.status = chunk.status;
    result.fail_closed = true;
    result.diagnostic = chunk.diagnostic;
    result.evidence = chunk.evidence;
    return result;
  }
  ++result.typed_object_count;

  result.evidence.push_back("CEIC-013_TYPED_SLAB_POOLS_SIZE_CLASS_ALLOCATORS");
  result.evidence.push_back(kAuthorityScope);
  result.evidence.push_back("optimizer.planner_typed_slab.route_label=" +
                            request.route_label);
  result.evidence.push_back("optimizer.planner_typed_slab.planner_node=true");
  result.evidence.push_back("optimizer.planner_typed_slab.candidate_chunk=true");
  return result;
}

}  // namespace scratchbird::engine::optimizer

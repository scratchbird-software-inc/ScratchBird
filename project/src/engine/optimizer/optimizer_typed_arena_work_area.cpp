// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "optimizer_typed_arena_work_area.hpp"

#include "typed_arena.hpp"

#include <cstddef>
#include <iomanip>
#include <sstream>
#include <utility>

namespace scratchbird::engine::optimizer {
namespace {

using scratchbird::core::memory::MemoryCategory;
using scratchbird::core::memory::MemoryLifetime;
using scratchbird::core::memory::MemoryTag;
using scratchbird::core::memory::TypedArena;
using scratchbird::core::platform::MakeDiagnostic;
using scratchbird::core::platform::Severity;
using scratchbird::core::platform::StatusCode;
using scratchbird::core::platform::Subsystem;

constexpr const char* kAuthorityBoundary =
    "optimizer_typed_arena.authority_scope=evidence_only_not_transaction_finality_visibility_security_recovery_parser_reference_wal_or_benchmark_authority";

struct OptimizerScratchCandidate {
  u64 ordinal = 0;
  u64 estimated_cost = 0;
  u64 estimated_rows = 0;
};

Status OkStatus() {
  return {StatusCode::ok, Severity::info, Subsystem::engine};
}

Status ErrorStatus() {
  return {StatusCode::memory_invalid_request, Severity::error, Subsystem::engine};
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
  out << "optimizer-typed-arena-v1:" << std::hex << std::setw(16)
      << std::setfill('0') << hash;
  return out.str();
}

OptimizerTypedArenaWorkAreaResult Refuse(OptimizerTypedArenaWorkAreaRequest request,
                                         std::string code,
                                         std::string message,
                                         std::string reason) {
  OptimizerTypedArenaWorkAreaResult result;
  result.status = ErrorStatus();
  result.fail_closed = true;
  result.diagnostic = MakeDiagnostic(StatusCode::memory_invalid_request,
                                     Severity::error,
                                     Subsystem::engine,
                                     std::move(code),
                                     std::move(message),
                                     {{"planning_route_label", request.planning_route_label},
                                      {"reason", std::move(reason)}},
                                     {},
                                     "engine.optimizer.typed_arena_work_area",
                                     "typed arena work areas require catalog planning evidence");
  result.evidence.push_back("MMCH_EXECUTOR_PLANNER_TYPED_ARENA_MIGRATION");
  result.evidence.push_back(kAuthorityBoundary);
  result.evidence.push_back("optimizer_typed_arena.fail_closed=true");
  return result;
}

}  // namespace

OptimizerTypedArenaWorkAreaResult BuildOptimizerTypedArenaWorkArea(
    OptimizerTypedArenaWorkAreaRequest request) {
  if (request.memory_manager == nullptr) {
    return Refuse(std::move(request), "optimizer_typed_arena_missing_memory_manager",
                  "optimizer.typed_arena.missing_memory_manager",
                  "memory_manager_required");
  }
  if (request.planning_route_label.empty()) {
    return Refuse(std::move(request), "optimizer_typed_arena_missing_route",
                  "optimizer.typed_arena.missing_route",
                  "planning_route_required");
  }
  if (!request.catalog_stats_authoritative) {
    return Refuse(std::move(request), "optimizer_typed_arena_catalog_stats_unproven",
                  "optimizer.typed_arena.catalog_stats_unproven",
                  "catalog_stats_authority_required");
  }
  if (request.parser_or_reference_authority || request.memory_benchmark_authority) {
    return Refuse(std::move(request), "optimizer_typed_arena_unsafe_authority",
                  "optimizer.typed_arena.unsafe_authority",
                  "parser_reference_or_memory_benchmark_authority_claimed");
  }
  if (request.candidate_count == 0) {
    return Refuse(std::move(request), "optimizer_typed_arena_empty_work",
                  "optimizer.typed_arena.empty_work",
                  "candidate_count_required");
  }

  MemoryTag tag;
  tag.subsystem = Subsystem::engine;
  tag.purpose = "optimizer_typed_arena_work_area";
  tag.category = MemoryCategory::executor_query_reserved;
  tag.lifetime = MemoryLifetime::arena;
  tag.owner = "engine.optimizer";
  tag.context_id = request.planning_route_label;
  tag.callsite = "engine.optimizer.typed_arena_work_area";

  const auto before = request.memory_manager->Snapshot();
  TypedArena arena(request.memory_manager->CreateArena(tag));
  auto candidates = arena.MakeVector<OptimizerScratchCandidate>(
      static_cast<std::size_t>(request.candidate_count));
  OptimizerTypedArenaWorkAreaResult result;
  if (!candidates.ok()) {
    result.status = candidates.status;
    result.fail_closed = true;
    result.diagnostic = candidates.diagnostic;
    result.evidence.push_back("MMCH_EXECUTOR_PLANNER_TYPED_ARENA_MIGRATION");
    result.evidence.push_back(kAuthorityBoundary);
    result.evidence.push_back("optimizer_typed_arena.fail_closed=true");
    return result;
  }

  u64 total_cost = 0;
  u64 total_rows = 0;
  for (u64 i = 0; i < request.candidate_count; ++i) {
    OptimizerScratchCandidate candidate{i, 100 + (i * 17), 10 + i};
    auto inserted = candidates.vector.EmplaceBack(candidate);
    if (!inserted.ok()) {
      result.status = inserted.status;
      result.fail_closed = true;
      result.diagnostic = inserted.diagnostic;
      return result;
    }
    total_cost += candidates.vector[static_cast<std::size_t>(i)].estimated_cost;
    total_rows += candidates.vector[static_cast<std::size_t>(i)].estimated_rows;
  }
  result.result_digest = Digest(request.candidate_count, total_cost, total_rows);
  result.candidate_count = request.candidate_count;
  const auto after_build = request.memory_manager->Snapshot();
  result.typed_arena_allocation_count =
      after_build.allocation_count >= before.allocation_count
          ? after_build.allocation_count - before.allocation_count
          : 0;
  result.baseline_allocation_count = request.candidate_count;
  const auto reset = arena.Reset();
  result.status = reset.status.ok() ? OkStatus() : reset.status;
  result.fail_closed = !result.status.ok();
  result.evidence.push_back("MMCH_EXECUTOR_PLANNER_TYPED_ARENA_MIGRATION");
  result.evidence.push_back(kAuthorityBoundary);
  result.evidence.push_back("optimizer_typed_arena.planning_route_label=" +
                            request.planning_route_label);
  result.evidence.push_back("optimizer_typed_arena.candidate_count=" +
                            std::to_string(result.candidate_count));
  result.evidence.push_back("optimizer_typed_arena.result_digest=" + result.result_digest);
  result.evidence.push_back("optimizer_typed_arena.typed_allocation_count=" +
                            std::to_string(result.typed_arena_allocation_count));
  result.evidence.push_back("optimizer_typed_arena.baseline_allocation_count=" +
                            std::to_string(result.baseline_allocation_count));
  result.evidence.push_back("optimizer_typed_arena.allocation_count_reduced=" +
                            std::string(result.typed_arena_allocation_count <
                                                result.baseline_allocation_count
                                            ? "true"
                                            : "false"));
  return result;
}

}  // namespace scratchbird::engine::optimizer

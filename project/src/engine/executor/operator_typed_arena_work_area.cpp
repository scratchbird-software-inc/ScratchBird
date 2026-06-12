// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "operator_typed_arena_work_area.hpp"

#include "runtime_platform.hpp"
#include "typed_arena.hpp"

#include <algorithm>
#include <cstddef>
#include <iomanip>
#include <sstream>
#include <utility>

namespace scratchbird::engine::executor {
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
    "executor_typed_arena.authority_scope=evidence_only_not_transaction_finality_visibility_security_recovery_parser_reference_wal_or_benchmark_authority";

struct ExecutorScratchRow {
  u64 ordinal = 0;
  u64 projected_value = 0;
};

Status OkStatus() {
  return {StatusCode::ok, Severity::info, Subsystem::engine};
}

Status ErrorStatus() {
  return {StatusCode::memory_invalid_request, Severity::error, Subsystem::engine};
}

std::string Digest(u64 row_count, u64 sum) {
  u64 hash = 1469598103934665603ull;
  auto mix = [&hash](u64 value) {
    for (int i = 0; i < 8; ++i) {
      hash ^= (value >> (i * 8)) & 0xffu;
      hash *= 1099511628211ull;
    }
  };
  mix(row_count);
  mix(sum);
  std::ostringstream out;
  out << "executor-typed-arena-v1:" << std::hex << std::setw(16)
      << std::setfill('0') << hash;
  return out.str();
}

ExecutorTypedArenaWorkAreaResult Refuse(ExecutorTypedArenaWorkAreaRequest request,
                                        std::string code,
                                        std::string message,
                                        std::string reason) {
  ExecutorTypedArenaWorkAreaResult result;
  result.status = ErrorStatus();
  result.fail_closed = true;
  result.diagnostic = MakeDiagnostic(StatusCode::memory_invalid_request,
                                     Severity::error,
                                     Subsystem::engine,
                                     std::move(code),
                                     std::move(message),
                                     {{"route_label", request.route_label},
                                      {"reason", std::move(reason)}},
                                     {},
                                     "engine.executor.typed_arena_work_area",
                                     "typed arena work areas require route and MGA authority evidence");
  result.evidence.push_back("MMCH_EXECUTOR_PLANNER_TYPED_ARENA_MIGRATION");
  result.evidence.push_back(kAuthorityBoundary);
  result.evidence.push_back("executor_typed_arena.fail_closed=true");
  return result;
}

}  // namespace

ExecutorTypedArenaWorkAreaResult BuildExecutorTypedArenaWorkArea(
    ExecutorTypedArenaWorkAreaRequest request) {
  if (request.memory_manager == nullptr) {
    return Refuse(std::move(request), "executor_typed_arena_missing_memory_manager",
                  "executor.typed_arena.missing_memory_manager",
                  "memory_manager_required");
  }
  if (request.route_label.empty()) {
    return Refuse(std::move(request), "executor_typed_arena_missing_route",
                  "executor.typed_arena.missing_route",
                  "route_label_required");
  }
  if (!request.engine_mga_snapshot_bound ||
      !request.transaction_inventory_authoritative) {
    return Refuse(std::move(request), "executor_typed_arena_mga_unproven",
                  "executor.typed_arena.mga_unproven",
                  "mga_snapshot_and_inventory_required");
  }
  if (request.parser_or_reference_authority ||
      request.memory_finality_or_visibility_authority) {
    return Refuse(std::move(request), "executor_typed_arena_unsafe_authority",
                  "executor.typed_arena.unsafe_authority",
                  "parser_reference_or_memory_finality_authority_claimed");
  }
  if (request.row_count == 0) {
    return Refuse(std::move(request), "executor_typed_arena_empty_work",
                  "executor.typed_arena.empty_work",
                  "row_count_required");
  }

  MemoryTag tag;
  tag.subsystem = Subsystem::engine;
  tag.purpose = "executor_typed_arena_work_area";
  tag.category = MemoryCategory::executor_query_reserved;
  tag.lifetime = MemoryLifetime::arena;
  tag.owner = "engine.executor";
  tag.context_id = request.route_label;
  tag.statement_id = request.route_label;
  tag.query_id = request.route_label;
  tag.callsite = "engine.executor.typed_arena_work_area";

  const auto before = request.memory_manager->Snapshot();
  TypedArena arena(request.memory_manager->CreateArena(tag));
  auto rows = arena.MakeVector<ExecutorScratchRow>(
      static_cast<std::size_t>(request.row_count));
  ExecutorTypedArenaWorkAreaResult result;
  if (!rows.ok()) {
    result.status = rows.status;
    result.fail_closed = true;
    result.diagnostic = rows.diagnostic;
    result.evidence.push_back("MMCH_EXECUTOR_PLANNER_TYPED_ARENA_MIGRATION");
    result.evidence.push_back(kAuthorityBoundary);
    result.evidence.push_back("executor_typed_arena.fail_closed=true");
    return result;
  }

  u64 sum = 0;
  for (u64 i = 0; i < request.row_count; ++i) {
    auto inserted = rows.vector.EmplaceBack(ExecutorScratchRow{i, i * 3 + 7});
    if (!inserted.ok()) {
      result.status = inserted.status;
      result.fail_closed = true;
      result.diagnostic = inserted.diagnostic;
      return result;
    }
    sum += rows.vector[static_cast<std::size_t>(i)].projected_value;
  }
  result.result_digest = Digest(request.row_count, sum);
  result.row_count = request.row_count;
  const auto after_build = request.memory_manager->Snapshot();
  result.typed_arena_allocation_count =
      after_build.allocation_count >= before.allocation_count
          ? after_build.allocation_count - before.allocation_count
          : 0;
  result.baseline_allocation_count = request.row_count;
  const auto reset = arena.Reset();
  result.status = reset.status.ok() ? OkStatus() : reset.status;
  result.fail_closed = !result.status.ok();
  result.evidence.push_back("MMCH_EXECUTOR_PLANNER_TYPED_ARENA_MIGRATION");
  result.evidence.push_back(kAuthorityBoundary);
  result.evidence.push_back("executor_typed_arena.route_label=" + request.route_label);
  result.evidence.push_back("executor_typed_arena.row_count=" +
                            std::to_string(result.row_count));
  result.evidence.push_back("executor_typed_arena.result_digest=" + result.result_digest);
  result.evidence.push_back("executor_typed_arena.typed_allocation_count=" +
                            std::to_string(result.typed_arena_allocation_count));
  result.evidence.push_back("executor_typed_arena.baseline_allocation_count=" +
                            std::to_string(result.baseline_allocation_count));
  result.evidence.push_back("executor_typed_arena.allocation_count_reduced=" +
                            std::string(result.typed_arena_allocation_count <
                                                result.baseline_allocation_count
                                            ? "true"
                                            : "false"));
  return result;
}

}  // namespace scratchbird::engine::executor

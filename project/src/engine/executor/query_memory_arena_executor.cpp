// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "query_memory_arena_executor.hpp"

#include "runtime_platform.hpp"

#include <utility>

namespace scratchbird::engine::executor {
namespace {

using scratchbird::core::platform::MakeDiagnostic;
using scratchbird::core::platform::Severity;
using scratchbird::core::platform::Status;
using scratchbird::core::platform::StatusCode;
using scratchbird::core::platform::Subsystem;
namespace agents = scratchbird::core::agents;

Status ErrorStatus() {
  return {StatusCode::memory_invalid_request, Severity::error, Subsystem::engine};
}

}  // namespace

scratchbird::core::memory::QueryMemoryFamily QueryMemoryFamilyForExecutorShape(
    ExecutorQueryShape shape) {
  using scratchbird::core::memory::QueryMemoryFamily;
  switch (shape) {
    case ExecutorQueryShape::relational: return QueryMemoryFamily::relational;
    case ExecutorQueryShape::search: return QueryMemoryFamily::search;
    case ExecutorQueryShape::vector: return QueryMemoryFamily::vector;
    case ExecutorQueryShape::graph: return QueryMemoryFamily::graph;
    case ExecutorQueryShape::document: return QueryMemoryFamily::document;
    case ExecutorQueryShape::time_series: return QueryMemoryFamily::time_series;
    case ExecutorQueryShape::dml: return QueryMemoryFamily::dml;
    case ExecutorQueryShape::candidate_set: return QueryMemoryFamily::candidate_set;
  }
  return QueryMemoryFamily::unknown;
}

ExecutorQueryMemoryResult RequestExecutorQueryMemory(
    scratchbird::core::memory::QueryMemoryArena* arena,
    ExecutorQueryMemoryRequest request) {
  ExecutorQueryMemoryResult result;
  auto governance_request = request.resource_governance;
  governance_request.expected_family =
      agents::ResourceGovernanceFamily::kQueryMemoryArena;
  const auto governance = agents::AdmitResourceGovernance(governance_request);
  auto append_governance = [&](ExecutorQueryMemoryResult* target) {
    target->evidence.insert(target->evidence.end(), governance.evidence.begin(),
                            governance.evidence.end());
    target->evidence.push_back("executor.query_memory.resource_governance_action=" +
                               std::string(agents::ResourceGovernanceActionName(
                                   governance.action)));
  };
  if (governance.action == agents::ResourceGovernanceAction::kFailClosed) {
    result.status = ErrorStatus();
    result.fail_closed = true;
    result.diagnostic = MakeDiagnostic(
        result.status.code,
        result.status.severity,
        result.status.subsystem,
        "SB_EXECUTOR_QUERY_MEMORY.ODF106_QUOTA_REFUSED",
        "executor.query_memory.odf106_quota_refused",
        {},
        {},
        "engine.executor.query_memory",
        governance.diagnostic_code);
    result.evidence.push_back("executor.query_memory.fail_closed=true");
    append_governance(&result);
    return result;
  }
  if (governance.action == agents::ResourceGovernanceAction::kCancel) {
    result.status = ErrorStatus();
    result.fail_closed = true;
    result.diagnostic = MakeDiagnostic(
        result.status.code,
        result.status.severity,
        result.status.subsystem,
        "SB_EXECUTOR_QUERY_MEMORY.ODF106_CANCELLED",
        "executor.query_memory.odf106_cancelled",
        {},
        {},
        "engine.executor.query_memory",
        governance.diagnostic_code);
    result.evidence.push_back("executor.query_memory.cancelled=true");
    append_governance(&result);
    return result;
  }
  if (governance.action == agents::ResourceGovernanceAction::kSlowdownDegrade ||
      governance.action ==
          agents::ResourceGovernanceAction::kExactScalarFallback) {
    result.status = ErrorStatus();
    result.fail_closed = true;
    result.diagnostic = MakeDiagnostic(
        result.status.code,
        result.status.severity,
        result.status.subsystem,
        "SB_EXECUTOR_QUERY_MEMORY.ODF106_QUOTA_REFUSED",
        "executor.query_memory.odf106_quota_refused",
        {},
        {},
        "engine.executor.query_memory",
        governance.diagnostic_code);
    result.evidence.push_back("executor.query_memory.degrade_unavailable=true");
    append_governance(&result);
    return result;
  }
  if (arena == nullptr) {
    result.status = ErrorStatus();
    result.fail_closed = true;
    result.diagnostic = MakeDiagnostic(
        result.status.code,
        result.status.severity,
        result.status.subsystem,
        "SB_EXECUTOR_QUERY_MEMORY.ARENA_REQUIRED",
        "executor.query_memory.arena_required",
        {},
        {},
        "engine.executor.query_memory",
        "Bind an executor query memory arena before requesting shape memory.");
    result.evidence.push_back("executor.query_memory.fail_closed=true");
    result.evidence.push_back("executor.query_memory.refused=arena_required");
    append_governance(&result);
    return result;
  }

  scratchbird::core::memory::QueryMemoryGrantRequest arena_request;
  arena_request.family = QueryMemoryFamilyForExecutorShape(request.shape);
  arena_request.bytes = request.bytes;
  arena_request.spillable = request.spillable;
  arena_request.purpose =
      request.purpose.empty() ? ExecutorQueryShapeName(request.shape) : request.purpose;

  result.arena_result = arena->Grant(std::move(arena_request));
  result.status = result.arena_result.status;
  result.fail_closed = result.arena_result.fail_closed;
  result.diagnostic = result.arena_result.diagnostic;
  result.evidence = result.arena_result.evidence;
  result.evidence.push_back("executor.query_memory.shape=" +
                            std::string(ExecutorQueryShapeName(request.shape)));
  result.evidence.push_back("executor.query_memory.primitive=query_memory_arena");
  result.evidence.push_back("executor.query_memory.transaction_finality_authority=false");
  result.evidence.push_back("executor.query_memory.visibility_authority=false");
  append_governance(&result);
  return result;
}

scratchbird::core::memory::QueryMemoryArenaReleaseResult ReleaseExecutorQueryMemory(
    scratchbird::core::memory::QueryMemoryArena* arena,
    const std::string& grant_id) {
  if (arena != nullptr) {
    auto result = arena->Release(grant_id);
    result.evidence.push_back("executor.query_memory.release_routed=true");
    return result;
  }
  scratchbird::core::memory::QueryMemoryArenaReleaseResult result;
  result.status = ErrorStatus();
  result.fail_closed = true;
  result.diagnostic = MakeDiagnostic(
      result.status.code,
      result.status.severity,
      result.status.subsystem,
      "SB_EXECUTOR_QUERY_MEMORY.ARENA_REQUIRED",
      "executor.query_memory.arena_required",
      {},
      {},
      "engine.executor.query_memory",
      "Bind an executor query memory arena before releasing shape memory.");
  result.evidence.push_back("executor.query_memory.fail_closed=true");
  result.evidence.push_back("executor.query_memory.refused=arena_required");
  return result;
}

scratchbird::core::memory::QueryMemoryArenaReleaseResult CancelExecutorQueryMemory(
    scratchbird::core::memory::QueryMemoryArena* arena,
    std::string reason) {
  if (arena != nullptr) {
    auto result = arena->Cancel(std::move(reason));
    result.evidence.push_back("executor.query_memory.cancel_routed=true");
    return result;
  }
  scratchbird::core::memory::QueryMemoryArenaReleaseResult result;
  result.status = ErrorStatus();
  result.fail_closed = true;
  result.diagnostic = MakeDiagnostic(
      result.status.code,
      result.status.severity,
      result.status.subsystem,
      "SB_EXECUTOR_QUERY_MEMORY.ARENA_REQUIRED",
      "executor.query_memory.arena_required",
      {},
      {},
      "engine.executor.query_memory",
      "Bind an executor query memory arena before cancelling shape memory.");
  result.evidence.push_back("executor.query_memory.fail_closed=true");
  result.evidence.push_back("executor.query_memory.refused=arena_required");
  return result;
}

const char* ExecutorQueryShapeName(ExecutorQueryShape shape) {
  switch (shape) {
    case ExecutorQueryShape::relational: return "relational";
    case ExecutorQueryShape::search: return "search";
    case ExecutorQueryShape::vector: return "vector";
    case ExecutorQueryShape::graph: return "graph";
    case ExecutorQueryShape::document: return "document";
    case ExecutorQueryShape::time_series: return "time_series";
    case ExecutorQueryShape::dml: return "dml";
    case ExecutorQueryShape::candidate_set: return "candidate_set";
  }
  return "unknown";
}

}  // namespace scratchbird::engine::executor

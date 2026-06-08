// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// SB-EXECUTOR-QUERY-MEMORY-ARENA-ANCHOR
#include "query_memory_arena.hpp"
#include "resource_governance_admission.hpp"

#include <string>
#include <vector>

namespace scratchbird::engine::executor {

enum class ExecutorQueryShape {
  relational,
  search,
  vector,
  graph,
  document,
  time_series,
  dml,
  candidate_set
};

struct ExecutorQueryMemoryRequest {
  ExecutorQueryShape shape = ExecutorQueryShape::relational;
  scratchbird::core::platform::u64 bytes = 0;
  bool spillable = false;
  std::string purpose;
  scratchbird::core::agents::ResourceGovernanceAdmissionRequest
      resource_governance;
};

struct ExecutorQueryMemoryResult {
  scratchbird::core::platform::Status status;
  bool fail_closed = false;
  scratchbird::core::memory::QueryMemoryArenaResult arena_result;
  std::vector<std::string> evidence;
  scratchbird::core::platform::DiagnosticRecord diagnostic;

  bool ok() const { return status.ok() && !fail_closed; }
};

scratchbird::core::memory::QueryMemoryFamily QueryMemoryFamilyForExecutorShape(
    ExecutorQueryShape shape);

ExecutorQueryMemoryResult RequestExecutorQueryMemory(
    scratchbird::core::memory::QueryMemoryArena* arena,
    ExecutorQueryMemoryRequest request);

scratchbird::core::memory::QueryMemoryArenaReleaseResult ReleaseExecutorQueryMemory(
    scratchbird::core::memory::QueryMemoryArena* arena,
    const std::string& grant_id);

scratchbird::core::memory::QueryMemoryArenaReleaseResult CancelExecutorQueryMemory(
    scratchbird::core::memory::QueryMemoryArena* arena,
    std::string reason);

const char* ExecutorQueryShapeName(ExecutorQueryShape shape);

}  // namespace scratchbird::engine::executor

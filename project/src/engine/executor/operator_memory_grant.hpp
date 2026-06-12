// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "query_memory_arena.hpp"
#include "query_memory_arena_executor.hpp"
#include "resource_governance_admission.hpp"

#include <string>
#include <vector>

namespace scratchbird::engine::executor {

// MMCH_LIVE_OPERATOR_MEMORY_GRANTS
enum class ExecutorMemoryOperatorKind {
  scan,
  sort,
  hash_join,
  merge_join,
  aggregate,
  window,
  candidate_set,
  vector_search,
  full_text_search,
  graph_traversal,
  document_path,
  time_series_rollup,
  dml_write,
  streaming_result
};

struct ExecutorOperatorMemoryAuthority {
  bool engine_mga_snapshot_bound = false;
  bool transaction_inventory_authoritative = false;
  bool security_recheck_required = false;
  bool parser_client_or_reference_memory_authority = false;
  bool memory_visibility_or_finality_authority = false;
  bool memory_recovery_authority = false;
  bool memory_authorization_authority = false;
};

struct ExecutorOperatorMemoryRequest {
  ExecutorMemoryOperatorKind operator_kind = ExecutorMemoryOperatorKind::scan;
  std::string route_label;
  scratchbird::core::platform::u64 bytes = 0;
  bool spillable = false;
  std::string purpose;
  scratchbird::core::memory::QueryMemoryArena* arena = nullptr;
  scratchbird::core::agents::ResourceGovernanceAdmissionRequest resource_governance;
  ExecutorOperatorMemoryAuthority authority;
};

struct ExecutorOperatorMemoryResult {
  scratchbird::core::platform::Status status;
  bool fail_closed = false;
  ExecutorQueryMemoryResult query_memory;
  std::string grant_id;
  scratchbird::core::platform::DiagnosticRecord diagnostic;
  std::vector<std::string> evidence;

  bool ok() const { return status.ok() && !fail_closed; }
};

const char* ExecutorMemoryOperatorKindName(ExecutorMemoryOperatorKind kind);
ExecutorQueryShape ExecutorQueryShapeForOperator(ExecutorMemoryOperatorKind kind);

ExecutorOperatorMemoryResult RequestExecutorOperatorMemory(
    ExecutorOperatorMemoryRequest request);

scratchbird::core::memory::QueryMemoryArenaReleaseResult ReleaseExecutorOperatorMemory(
    scratchbird::core::memory::QueryMemoryArena* arena,
    ExecutorMemoryOperatorKind operator_kind,
    const std::string& grant_id);

}  // namespace scratchbird::engine::executor

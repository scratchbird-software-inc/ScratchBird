// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "memory.hpp"

#include <string>
#include <vector>

namespace scratchbird::engine::executor {

using scratchbird::core::platform::DiagnosticRecord;
using scratchbird::core::platform::Status;
using scratchbird::core::platform::u64;

// MMCH_EXECUTOR_PLANNER_TYPED_ARENA_MIGRATION
struct ExecutorTypedArenaWorkAreaRequest {
  scratchbird::core::memory::MemoryManager* memory_manager = nullptr;
  std::string route_label;
  u64 row_count = 0;
  bool engine_mga_snapshot_bound = true;
  bool transaction_inventory_authoritative = true;
  bool parser_or_reference_authority = false;
  bool memory_finality_or_visibility_authority = false;
};

struct ExecutorTypedArenaWorkAreaResult {
  Status status;
  bool fail_closed = false;
  u64 typed_arena_allocation_count = 0;
  u64 baseline_allocation_count = 0;
  u64 row_count = 0;
  std::string result_digest;
  DiagnosticRecord diagnostic;
  std::vector<std::string> evidence;

  bool ok() const {
    return status.ok() && !fail_closed;
  }
};

ExecutorTypedArenaWorkAreaResult BuildExecutorTypedArenaWorkArea(
    ExecutorTypedArenaWorkAreaRequest request);

}  // namespace scratchbird::engine::executor

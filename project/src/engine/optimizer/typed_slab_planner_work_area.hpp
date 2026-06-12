// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// CEIC-013 optimizer/planner adapter for typed slab hot work areas.
#include "typed_slab_pool.hpp"

#include <string>
#include <vector>

namespace scratchbird::engine::optimizer {

using scratchbird::core::platform::DiagnosticRecord;
using scratchbird::core::platform::Status;
using scratchbird::core::platform::u64;

struct PlannerTypedSlabWorkAreaRequest {
  scratchbird::core::memory::SizeClassAllocator* allocator = nullptr;
  std::string route_label;
  u64 candidate_count = 0;
  bool catalog_stats_authoritative = true;
  bool parser_or_reference_authority = false;
  bool memory_plan_authority = false;
};

struct PlannerTypedSlabWorkAreaResult {
  Status status;
  bool fail_closed = false;
  u64 candidate_count = 0;
  u64 typed_object_count = 0;
  DiagnosticRecord diagnostic;
  std::vector<std::string> evidence;

  bool ok() const { return status.ok() && !fail_closed; }
};

PlannerTypedSlabWorkAreaResult BuildPlannerTypedSlabWorkArea(
    PlannerTypedSlabWorkAreaRequest request);

}  // namespace scratchbird::engine::optimizer

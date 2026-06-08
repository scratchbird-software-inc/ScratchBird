// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "access_path.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace scratchbird::engine::optimizer {

// SEARCH_KEY: SB_OPTIMIZER_JOIN_PLANNER
struct JoinPlanningInput {
  std::uint64_t left_cardinality = 0;
  std::uint64_t right_cardinality = 0;
  bool reorder_safe = false;
  bool equi_join = false;
  bool ordered_inputs = false;
  bool hash_join_executor_available = true;
  bool merge_join_executor_available = true;
  std::uint64_t memory_budget_bytes = 0;
};

struct JoinPlanDecision {
  bool ok = false;
  scratchbird::engine::planner::PhysicalAccessKind selected_method = scratchbird::engine::planner::PhysicalAccessKind::kJoinNestedLoop;
  std::vector<PlanCandidate> candidates;
  std::vector<std::string> diagnostics;
};

JoinPlanDecision PlanLocalJoin(const JoinPlanningInput& input);

}  // namespace scratchbird::engine::optimizer

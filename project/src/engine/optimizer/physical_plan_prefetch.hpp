// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// SB-OPTIMIZER-PLAN-AWARE-PREFETCH-ANCHOR
#include "physical_plan.hpp"
#include "plan_aware_prefetch.hpp"

#include <vector>

namespace scratchbird::engine::optimizer {

struct PhysicalPlanPrefetchInput {
  scratchbird::core::platform::u64 physical_plan_generation = 0;
  std::vector<scratchbird::storage::page::PlanAwarePrefetchDescriptor>
      descriptors;
  scratchbird::storage::page::PlanAwarePrefetchBudget budget;
  scratchbird::storage::page::PlanAwarePrefetchCancellation cancellation;
};

scratchbird::storage::page::PlanAwarePrefetchResult
ExecutePhysicalPlanDrivenPrefetch(
    const PhysicalPlanNode& physical_plan_root,
    const PhysicalPlanPrefetchInput& input);

}  // namespace scratchbird::engine::optimizer

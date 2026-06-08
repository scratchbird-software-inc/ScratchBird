// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "scoring_kernel_executor.hpp"

namespace scratchbird::engine::executor {

ScoringKernelExecutionResult ExecuteOptionalScoringKernel(
    const ScoringKernelExecutionRequest& request) {
  auto result =
      scratchbird::engine::gpu_acceleration::ExecuteScoringKernelAcceleration(
          request);
  result.evidence.push_back("executor.scoring_kernel_route=odf105");
  result.evidence.push_back("executor.scalar_reference_authority=true");
  return result;
}

}  // namespace scratchbird::engine::executor

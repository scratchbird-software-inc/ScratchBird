// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "scoring_kernel_acceleration.hpp"

namespace scratchbird::engine::executor {

// SEARCH_KEY: SB_EXECUTOR_SCORING_KERNEL_ROUTE_ODF_105
// Executor-owned route for optional SIMD/GPU scoring kernels. The executor
// supplies materialized authorized batches and exact scalar references; the
// accelerator provider is never transaction, visibility, security, parser,
// reference, recovery, page, or catalog authority.

using ScoringKernelExecutionRequest =
    scratchbird::engine::gpu_acceleration::ScoringKernelRequest;
using ScoringKernelExecutionResult =
    scratchbird::engine::gpu_acceleration::ScoringKernelResult;

ScoringKernelExecutionResult ExecuteOptionalScoringKernel(
    const ScoringKernelExecutionRequest& request);

}  // namespace scratchbird::engine::executor

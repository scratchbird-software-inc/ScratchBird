// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "api_types.hpp"
#include "observability/performance_optimization_surface.hpp"

namespace scratchbird::engine::internal_api {

// SEARCH_KEY: SB_ENGINE_INTERNAL_API_OBSERVABILITY_EXPLAIN_API
struct EngineExplainOperationRequest : EngineApiRequest {
  PerformanceOptimizationSurfaceSnapshot performance_optimization_snapshot;
  bool performance_optimization_snapshot_present = false;
};
struct EngineExplainOperationResult : EngineApiResult {};
EngineExplainOperationResult EngineExplainOperation(const EngineExplainOperationRequest& request);

// SEARCH_KEY: SB_ENGINE_INTERNAL_API_EXPLAIN_OPTIMIZER_EVIDENCE
struct EngineExplainOptimizerEvidenceRequest : EngineApiRequest {
  std::vector<std::string> candidate_evidence_rows;
  PerformanceOptimizationSurfaceSnapshot performance_optimization_snapshot;
  bool performance_optimization_snapshot_present = false;
};
struct EngineExplainOptimizerEvidenceResult : EngineApiResult {};
EngineExplainOptimizerEvidenceResult EngineExplainOptimizerEvidence(
    const EngineExplainOptimizerEvidenceRequest& request);

}  // namespace scratchbird::engine::internal_api

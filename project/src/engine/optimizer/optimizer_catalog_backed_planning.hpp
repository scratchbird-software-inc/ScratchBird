// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "optimizer_contract.hpp"
#include "optimizer_plan_cache.hpp"
#include "optimizer_request.hpp"

#include <string>
#include <vector>

namespace scratchbird::engine::optimizer {

// SEARCH_KEY: PCR061_CATALOG_BACKED_PRODUCTION_PLANNING
// Production optimizer admission requires catalog-backed table/index
// statistics, descriptor/route/security/redaction/memory dependency digests,
// and engine-owned SBLR/MGA/security context. Local/default statistics are
// diagnostic fallback evidence only and cannot satisfy production admission.
struct CatalogBackedProductionPlanningRequest {
  BoundOptimizerRequest bound_request;
  OptimizerPlanCacheKeyInput plan_cache_key_input;
  bool production_build = true;
  bool require_index_stats = true;
};

struct CatalogBackedProductionPlanningValidation {
  bool ok = false;
  bool benchmark_clean_ready = false;
  bool catalog_backed = false;
  bool local_or_policy_default_diagnostic_only = false;
  std::string diagnostic_code;
  std::vector<std::string> diagnostics;
  std::vector<std::string> evidence;
};

struct CatalogBackedProductionPlanningResult {
  CatalogBackedProductionPlanningValidation validation;
  OptimizedPlan optimized_plan;
  BoundOptimizerResult bound_result;
};

CatalogBackedProductionPlanningValidation
ValidateCatalogBackedProductionPlanningRequest(
    const CatalogBackedProductionPlanningRequest& request);

CatalogBackedProductionPlanningResult OptimizeCatalogBackedProductionPlan(
    const CatalogBackedProductionPlanningRequest& request);

}  // namespace scratchbird::engine::optimizer

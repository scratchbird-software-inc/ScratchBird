// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// SB-OPTIMIZER-PLAN-LIFECYCLE-ANCHOR
#include "api_types.hpp"
#include "index_statistics_lifecycle.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace scratchbird::engine::internal_api {

namespace index_lifecycle = scratchbird::core::index;

inline constexpr const char* kOptimizerPlanLifecycleEventMagic = "SBPLANL1";

inline constexpr const char* kOptimizerPlanDiagnosticOk = "OPTIMIZER.PLAN.OK";
inline constexpr const char* kOptimizerPlanDiagnosticDatabasePathRequired =
    "OPTIMIZER.PLAN.DATABASE_PATH_REQUIRED";
inline constexpr const char* kOptimizerPlanDiagnosticMgaTransactionRequired =
    "OPTIMIZER.PLAN.MGA_TRANSACTION_REQUIRED";
inline constexpr const char* kOptimizerPlanDiagnosticInvalidRequest =
    "OPTIMIZER.PLAN.INVALID_REQUEST";
inline constexpr const char* kOptimizerPlanDiagnosticStatisticsStale =
    "OPTIMIZER.PLAN.STATISTICS_STALE";
inline constexpr const char* kOptimizerPlanDiagnosticCacheMiss =
    "OPTIMIZER.PLAN.CACHE_MISS";
inline constexpr const char* kOptimizerPlanDiagnosticCacheInvalidated =
    "OPTIMIZER.PLAN.CACHE_INVALIDATED";
inline constexpr const char* kOptimizerPlanDiagnosticEpochMismatch =
    "OPTIMIZER.PLAN.EPOCH_MISMATCH";
inline constexpr const char* kOptimizerPlanDiagnosticWriteFailed =
    "OPTIMIZER.PLAN.WRITE_FAILED";

struct EngineOptimizerPlanCacheEntry {
  std::uint64_t creator_tx = 0;
  std::uint64_t event_sequence = 0;
  std::uint64_t plan_cache_epoch = 0;
  std::string plan_uuid;
  std::string query_fingerprint;
  std::string relation_uuid;
  std::string index_uuid;
  std::string catalog_physical_profile_key;
  std::string plan_shape_digest;
  std::uint64_t index_generation = 0;
  std::uint64_t statistics_generation = 0;
  std::uint64_t catalog_generation_id = 0;
  std::uint64_t resource_epoch = 0;
  std::uint64_t charset_epoch = 0;
  std::uint64_t collation_epoch = 0;
  bool invalidated = false;
  std::string invalidation_reason;
  bool recovered_from_persisted_evidence = false;
};

struct EngineOptimizerPlanLifecycleState {
  std::vector<EngineOptimizerPlanCacheEntry> entries;
  std::uint64_t plan_cache_epoch = 0;
  std::uint64_t max_event_sequence = 0;
  std::uint64_t invalidation_events = 0;
  bool recovered_from_persisted_evidence = false;
  std::string recovery_snapshot_uuid;
};

struct EngineLoadOptimizerPlanLifecycleStateResult {
  bool ok = false;
  EngineApiDiagnostic diagnostic;
  EngineOptimizerPlanLifecycleState state;
};

struct EngineOptimizerCachePlanRequest : EngineApiRequest {
  std::string plan_uuid;
  std::string query_fingerprint;
  std::string relation_uuid;
  std::string index_uuid;
  std::string plan_shape_digest;
  index_lifecycle::IndexLifecycleDescriptor index_descriptor;
  index_lifecycle::IndexStatisticsSnapshot statistics;
  index_lifecycle::IndexStatisticsFreshnessPolicy freshness_policy =
      index_lifecycle::IndexStatisticsFreshnessPolicy::refuse_stale;
};

struct EngineOptimizerCachePlanResult : EngineApiResult {
  EngineOptimizerPlanCacheEntry entry;
  std::uint64_t plan_cache_epoch = 0;
};
EngineOptimizerCachePlanResult EngineOptimizerCachePlan(
    const EngineOptimizerCachePlanRequest& request);

struct EngineOptimizerValidateCachedPlanRequest : EngineApiRequest {
  std::string plan_uuid;
  std::string query_fingerprint;
  std::string index_uuid;
  std::uint64_t current_index_generation = 0;
  std::uint64_t current_statistics_generation = 0;
  std::uint64_t current_catalog_generation_id = 0;
  index_lifecycle::IndexResourceEpochVector current_resource_epochs;
  bool require_current_statistics = true;
  bool statistics_stale = false;
};

struct EngineOptimizerValidateCachedPlanResult : EngineApiResult {
  EngineOptimizerPlanCacheEntry entry;
  bool cache_hit = false;
  bool invalidation_required = false;
  std::uint64_t plan_cache_epoch = 0;
};
EngineOptimizerValidateCachedPlanResult EngineOptimizerValidateCachedPlan(
    const EngineOptimizerValidateCachedPlanRequest& request);

struct EngineOptimizerInvalidatePlanCacheRequest : EngineApiRequest {
  std::string index_uuid;
  std::string reason = "explicit_invalidation";
  std::uint64_t new_index_generation = 0;
  std::uint64_t new_statistics_generation = 0;
  std::uint64_t new_catalog_generation_id = 0;
  index_lifecycle::IndexResourceEpochVector new_resource_epochs;
  bool invalidate_all = false;
};

struct EngineOptimizerInvalidatePlanCacheResult : EngineApiResult {
  EngineOptimizerPlanLifecycleState state;
  std::uint64_t plan_cache_epoch = 0;
};
EngineOptimizerInvalidatePlanCacheResult EngineOptimizerInvalidatePlanCache(
    const EngineOptimizerInvalidatePlanCacheRequest& request);

struct EngineOptimizerRecoverPlanCacheRequest : EngineApiRequest {};

struct EngineOptimizerRecoverPlanCacheResult : EngineApiResult {
  EngineOptimizerPlanLifecycleState state;
  std::string recovery_snapshot_uuid;
};
EngineOptimizerRecoverPlanCacheResult EngineOptimizerRecoverPlanCache(
    const EngineOptimizerRecoverPlanCacheRequest& request);

EngineLoadOptimizerPlanLifecycleStateResult LoadOptimizerPlanLifecycleState(
    const EngineRequestContext& context);

}  // namespace scratchbird::engine::internal_api

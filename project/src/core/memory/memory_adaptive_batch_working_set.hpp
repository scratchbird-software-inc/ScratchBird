// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// CEIC-029_ADAPTIVE_BATCH_WORKING_SET_LOCALITY
#include "memory_locality_policy.hpp"
#include "memory_pressure_response.hpp"
#include "memory_support_bundle.hpp"
#include "metric_registry.hpp"
#include "thread_local_memory_cache.hpp"

#include <string>
#include <vector>

namespace scratchbird::core::memory {

using scratchbird::core::platform::DiagnosticRecord;
using scratchbird::core::platform::Status;
using scratchbird::core::platform::u64;

// CEIC-029_PRESSURE_AWARE_ADAPTIVE_BATCH
enum class AdaptiveBatchOperationKind {
  copy,
  result_frame,
  vector,
  hash_join,
  sort,
  index_build,
  prefetch,
  cleanup_page_cache
};

enum class WorkingSetTemperature {
  hot,
  warm,
  cold
};

enum class AdaptiveBatchAdmissionAction {
  admit,
  reduce,
  throttle,
  spill,
  cancel,
  refuse
};

struct AdaptiveBatchSizingPolicy {
  u64 normal_percent = 100;
  u64 recovery_percent = 75;
  u64 soft_pressure_percent = 50;
  u64 high_pressure_percent = 25;
  u64 emergency_pressure_percent = 10;
  u64 hot_working_set_bonus_percent = 125;
  u64 warm_working_set_percent = 100;
  u64 cold_working_set_percent = 50;
  u64 scan_resistant_percent = 35;
  u64 tenant_quota_pressure_percent = 50;
  u64 dirty_pressure_percent = 50;
  u64 min_batch_rows = 1;
  u64 min_batch_bytes = 4096;
  u64 max_prefetch_pages = 16;
  u64 max_cleanup_pages = 32;
  bool require_ceic017_pressure_evidence = true;
  bool require_deterministic_boundaries_for_result_hash = true;
  bool refuse_without_locality_evidence = true;
};

struct WorkingSetLocalityObservation {
  std::string database_id;
  std::string tenant_id;
  std::string user_id;
  std::string session_id;
  std::string query_id;
  std::string operator_id;
  std::string route_label;
  u64 page_cache_resident_pages = 0;
  u64 page_cache_resident_bytes = 0;
  u64 page_cache_hot_pages = 0;
  u64 page_cache_warm_pages = 0;
  u64 page_cache_cold_pages = 0;
  u64 page_cache_recent_accesses = 0;
  u64 page_cache_recent_hits = 0;
  u64 page_cache_reuse_count = 0;
  u64 page_cache_allocate_count = 0;
  u64 dirty_pages = 0;
  u64 dirty_page_limit = 0;
  u64 prefetch_requested_pages = 0;
  u64 prefetch_budget_pages = 0;
  u64 tenant_active_bytes = 0;
  u64 tenant_quota_bytes = 0;
  u64 projected_bytes = 0;
  u64 sequential_scan_pages = 0;
  u64 random_reuse_pages = 0;
  bool ceic019_page_cache_frame_pool_evidence = false;
  bool page_cache_snapshot_deterministic = false;
  bool dirty_page_policy_evidence = false;
  bool prefetch_budget_evidence = false;
  bool tenant_quota_evidence = false;
  bool scan_resistance_evidence = false;
  bool clock_replacement_policy_evaluated = false;
  bool lru2_replacement_policy_evaluated = false;
  bool arc_replacement_policy_evaluated = false;
  std::string selected_replacement_policy;
  bool numa_locality_evidence = false;
  bool huge_page_evidence = false;
};

struct AdaptiveBatchSizingRequest {
  AdaptiveBatchOperationKind operation =
      AdaptiveBatchOperationKind::result_frame;
  AdaptiveBatchSizingPolicy policy;
  MemoryPressureDecision pressure_decision;
  WorkingSetLocalityObservation working_set;
  MemoryLocalityPolicy locality_policy;
  ThreadLocalCacheSnapshot thread_local_cache_snapshot;
  u64 requested_batch_rows = 0;
  u64 requested_batch_bytes = 0;
  u64 max_batch_rows = 0;
  u64 max_batch_bytes = 0;
  bool spill_supported = false;
  bool throttle_supported = true;
  bool cancel_supported = false;
  bool cleanup_supported = false;
  bool result_hash_stability_required = false;
  bool deterministic_boundary_evidence = false;
  bool deterministic_route_evidence = false;
  bool stable_result_hash_evidence = false;
  bool route_requires_stable_result_hash = false;
  bool production_route = true;
  bool cluster_route = false;
  bool external_cluster_provider = true;
};

struct AdaptiveBatchSizingDecision {
  Status status;
  DiagnosticRecord diagnostic;
  AdaptiveBatchAdmissionAction action = AdaptiveBatchAdmissionAction::refuse;
  AdaptiveBatchOperationKind operation =
      AdaptiveBatchOperationKind::result_frame;
  WorkingSetTemperature working_set_temperature =
      WorkingSetTemperature::cold;
  MemoryPressureState pressure_state = MemoryPressureState::normal;
  u64 requested_batch_rows = 0;
  u64 admitted_batch_rows = 0;
  u64 requested_batch_bytes = 0;
  u64 admitted_batch_bytes = 0;
  u64 prefetch_admitted_pages = 0;
  u64 cleanup_admitted_pages = 0;
  u64 reduction_percent = 100;
  bool admission_allowed = false;
  bool fail_closed = false;
  bool deterministic_boundaries_required = false;
  bool deterministic_boundaries_preserved = false;
  bool result_hash_stability_preserved = false;
  bool tenant_quota_limited = false;
  bool scan_resistance_applied = false;
  bool replacement_policy_evaluation_present = false;
  bool dirty_page_policy_applied = false;
  bool prefetch_budget_applied = false;
  bool locality_fallback_used = false;
  bool huge_page_fallback_used = false;
  bool support_bundle_ready = false;
  bool metrics_ready = false;
  std::vector<std::string> evidence;
  std::vector<MemorySupportBundleRow> support_bundle_rows;
  std::vector<scratchbird::core::metrics::MetricValue> metrics;

  bool ok() const {
    return status.ok() && !fail_closed && admission_allowed;
  }
};

const char* AdaptiveBatchOperationKindName(AdaptiveBatchOperationKind kind);
const char* WorkingSetTemperatureName(WorkingSetTemperature temperature);
const char* AdaptiveBatchAdmissionActionName(
    AdaptiveBatchAdmissionAction action);

AdaptiveBatchSizingDecision PlanAdaptiveBatchWorkingSetLocality(
    AdaptiveBatchSizingRequest request);

}  // namespace scratchbird::core::memory

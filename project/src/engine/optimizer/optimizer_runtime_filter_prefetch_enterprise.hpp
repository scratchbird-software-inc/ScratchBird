// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "physical_plan_prefetch.hpp"
#include "runtime_filter_pushdown.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace scratchbird::engine::optimizer {

// SEARCH_KEY: OEIC_RUNTIME_FILTER_PREFETCH_CLOSURE
struct EnterpriseRuntimeFilterPrefetchMetricSnapshot {
  std::string metric_snapshot_id;
  std::string route_label;
  std::string result_contract_hash;
  std::string source_provenance = "engine_runtime_metrics";
  std::string trust_provenance = "engine_metric_registry";
  std::uint64_t generation = 0;
  std::uint64_t route_epoch = 0;
  std::uint64_t stats_epoch = 0;
  std::uint64_t security_epoch = 0;
  std::uint64_t redaction_epoch = 0;
  std::uint64_t memory_epoch = 0;
  std::uint64_t runtime_filter_effectiveness_ppm = 0;
  std::uint64_t prefetch_hit_rate_ppm = 0;
  std::uint64_t prefetch_waste_ppm = 0;
  std::uint64_t prefetch_latency_saved_units = 0;
  std::uint64_t prefetch_io_cost_units = 0;
  std::uint64_t late_materialization_recheck_rows = 0;
  bool fresh = false;
  bool trusted = false;
  bool engine_runtime_scope = false;
  bool redacted_for_explain = true;
  bool exact_fallback_available = false;
  bool parser_or_reference_authority = false;
  bool client_authority = false;
  bool provider_finality_or_visibility_authority = false;
  bool recovery_or_wal_authority = false;
  bool cluster_route_or_metric_projection = false;
  bool fixture_or_test_only = false;
};

struct EnterpriseRuntimeFilterPrefetchRequest {
  std::string plan_id;
  RuntimeFilterPushdownRequest runtime_filter_request;
  PhysicalPlanNode physical_plan_root;
  PhysicalPlanPrefetchInput prefetch_input;
  EnterpriseRuntimeFilterPrefetchMetricSnapshot metrics;
  bool runtime_filter_requested = true;
  bool prefetch_requested = true;
  bool late_materialization_requested = false;
  std::uint64_t min_runtime_filter_effectiveness_ppm = 10'000;
  std::uint64_t min_prefetch_hit_rate_ppm = 200'000;
  std::uint64_t max_prefetch_waste_ppm = 250'000;
};

struct EnterpriseRuntimeFilterPrefetchDecision {
  bool ok = false;
  bool fail_closed = true;
  std::string diagnostic_code;
  std::vector<std::string> evidence;
  RuntimeFilterPushdownDecision runtime_filter_decision;
  scratchbird::storage::page::PlanAwarePrefetchResult prefetch_result;
  bool runtime_filter_selected = false;
  bool runtime_filter_suppressed_by_feedback = false;
  bool prefetch_scheduled = false;
  bool prefetch_suppressed_by_feedback = false;
  bool late_materialization_planned = false;
  bool exact_fallback_selected = false;
};

EnterpriseRuntimeFilterPrefetchDecision PlanEnterpriseRuntimeFilterPrefetch(
    const EnterpriseRuntimeFilterPrefetchRequest& request);

}  // namespace scratchbird::engine::optimizer

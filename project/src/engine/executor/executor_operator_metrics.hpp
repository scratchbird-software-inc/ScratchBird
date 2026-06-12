// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "metric_contracts.hpp"
#include "metric_registry.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace scratchbird::engine::executor {

// SEARCH_KEY: OEIC_EXECUTOR_OPERATOR_ACTUALS_METRICS
// Production executor actuals producer. These metrics are optimizer-advisory
// only: they calibrate cost, cardinality, memory, spill, and EXPLAIN evidence,
// but never become transaction finality, visibility, security, recovery,
// parser, reference, or benchmark authority.

struct ExecutorOperatorMetricAuthority {
  bool engine_mga_snapshot_bound = false;
  bool transaction_inventory_authoritative = false;
  bool security_recheck_preserved = false;
  bool parser_or_reference_authority = false;
  bool client_supplied_finality = false;
  bool metric_visibility_or_finality_authority = false;
  bool metric_recovery_authority = false;
  bool benchmark_authority = false;
};

struct ExecutorOperatorActualsSample {
  std::string scope_uuid;
  std::string route_label;
  std::string plan_node_id;
  std::string operator_family;
  std::string plan_shape;
  std::string evidence_digest;
  std::uint64_t source_generation = 0;
  std::uint64_t estimated_rows = 0;
  std::uint64_t actual_rows = 0;
  std::uint64_t rows_examined = 0;
  std::uint64_t rows_filtered = 0;
  std::uint64_t loop_count = 0;
  std::uint64_t estimated_pages = 0;
  std::uint64_t actual_pages = 0;
  std::uint64_t estimated_io_operations = 0;
  std::uint64_t actual_io_operations = 0;
  std::uint64_t estimated_visibility_recheck_rows = 0;
  std::uint64_t actual_visibility_recheck_rows = 0;
  std::uint64_t estimated_spill_bytes = 0;
  std::uint64_t actual_spill_bytes = 0;
  std::uint64_t spill_passes = 0;
  std::uint64_t memory_grant_bytes = 0;
  std::uint64_t peak_memory_bytes = 0;
  std::uint64_t estimated_latency_microseconds = 0;
  std::uint64_t actual_latency_microseconds = 0;
  std::uint64_t cpu_time_microseconds = 0;
  std::uint64_t estimated_resource_units = 0;
  std::uint64_t actual_resource_units = 0;
  std::uint64_t freshness_microseconds = 0;
  std::uint64_t max_freshness_microseconds = 60000000;
  ExecutorOperatorMetricAuthority authority;
};

struct ExecutorOperatorMetricPublishResult {
  bool ok = false;
  std::string diagnostic_code;
  std::string detail;
  std::vector<std::string> evidence;
  std::vector<scratchbird::core::metrics::MetricValidationResult>
      metric_results;
};

scratchbird::core::metrics::MetricValidationResult
EnsureExecutorOperatorActualsMetricDescriptors(
    scratchbird::core::metrics::MetricRegistry* registry = nullptr);

ExecutorOperatorMetricPublishResult PublishExecutorOperatorActuals(
    const ExecutorOperatorActualsSample& sample);

}  // namespace scratchbird::engine::executor

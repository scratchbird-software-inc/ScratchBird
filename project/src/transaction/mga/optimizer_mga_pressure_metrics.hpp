// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "metric_registry.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace scratchbird::transaction::mga {

// SEARCH_KEY: OEIC_MGA_PRESSURE_OPTIMIZER_METRICS
// MGA pressure metrics are optimizer-advisory only. They adjust cost/risk and
// never become transaction finality, visibility, security, recovery, parser,
// reference, external log replay, or benchmark authority.

struct OptimizerMgaPressureAuthority {
  bool transaction_inventory_authoritative = false;
  bool cleanup_horizon_authoritative = false;
  bool row_version_runtime_authoritative = false;
  bool engine_scope_bound = false;
  bool parser_or_reference_authority = false;
  bool client_finality_or_visibility_authority = false;
  bool metric_visibility_or_finality_authority = false;
  bool metric_recovery_authority = false;
  bool external_log_replay_authority = false;
  bool benchmark_authority = false;
};

struct OptimizerMgaPressureSample {
  std::string scope_uuid;
  std::string route_label;
  std::string relation_uuid;
  std::string page_class = "data";
  std::string evidence_digest;
  std::uint64_t source_generation = 0;
  std::uint64_t cleanup_debt_bytes = 0;
  std::uint64_t retained_dead_bytes = 0;
  std::uint64_t chain_depth_bucket = 0;
  std::uint64_t chain_scatter_bucket = 0;
  double same_page_update_ratio = 0.0;
  std::uint64_t commit_fence_backlog = 0;
  std::uint64_t authoritative_cleanup_horizon_local_transaction_id = 0;
  std::uint64_t freshness_microseconds = 0;
  std::uint64_t max_freshness_microseconds = 60000000;
  OptimizerMgaPressureAuthority authority;
};

struct OptimizerMgaPressurePublishResult {
  bool ok = false;
  std::string diagnostic_code;
  std::string detail;
  std::vector<std::string> evidence;
  std::vector<scratchbird::core::metrics::MetricValidationResult>
      metric_results;
};

scratchbird::core::metrics::MetricValidationResult
EnsureOptimizerMgaPressureMetricDescriptors(
    scratchbird::core::metrics::MetricRegistry* registry = nullptr);

OptimizerMgaPressurePublishResult PublishOptimizerMgaPressureMetrics(
    const OptimizerMgaPressureSample& sample);

}  // namespace scratchbird::transaction::mga

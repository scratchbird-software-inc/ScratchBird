// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "metric_registry.hpp"
#include "runtime_consumption_evidence.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace scratchbird::engine::optimizer {

// SEARCH_KEY: OEIC_ROUTE_DRIVER_OPTIMIZER_METRICS
// Route and driver-visible optimizer metrics record plan/result/explain
// equivalence evidence only. They cannot provide transaction finality,
// visibility, security, parser, donor, recovery, WAL, cluster, or benchmark
// authority.

struct OptimizerRouteMetricAuthority {
  bool route_executor_authoritative = false;
  bool optimizer_explain_authoritative = false;
  bool result_contract_authoritative = false;
  bool driver_surface_authoritative = false;
  bool route_equivalence_validated = false;
  bool engine_scope_bound = false;
  bool exact_diagnostics_preserved = false;
  bool redaction_applied = false;
  bool parser_or_donor_authority = false;
  bool client_finality_or_visibility_authority = false;
  bool metric_visibility_or_finality_authority = false;
  bool metric_recovery_authority = false;
  bool wal_or_redo_authority = false;
  bool cluster_authority = false;
  bool benchmark_authority = false;
};

struct OptimizerRouteMetricSample {
  std::string scope_uuid;
  std::string route_kind;
  std::string route_label;
  std::string plan_node_id;
  std::string plan_hash;
  std::string result_hash;
  std::string explain_digest;
  std::string result_contract_hash;
  std::string redaction_digest;
  std::string diagnostic_code;
  std::string evidence_digest;
  std::uint64_t source_generation = 0;
  std::uint64_t freshness_microseconds = 0;
  std::uint64_t max_freshness_microseconds = 60000000;
  std::vector<DriverVisibleExplainRouteEvidence> driver_routes;
  std::vector<std::string> required_driver_routes;
  OptimizerRouteMetricAuthority authority;
};

struct OptimizerRouteMetricPublishResult {
  bool ok = false;
  std::string diagnostic_code;
  std::string detail;
  std::vector<std::string> evidence;
  std::vector<scratchbird::core::metrics::MetricValidationResult>
      metric_results;
};

scratchbird::core::metrics::MetricValidationResult
EnsureOptimizerRouteMetricDescriptors(
    scratchbird::core::metrics::MetricRegistry* registry = nullptr);

OptimizerRouteMetricPublishResult PublishOptimizerRouteMetrics(
    const OptimizerRouteMetricSample& sample);

}  // namespace scratchbird::engine::optimizer

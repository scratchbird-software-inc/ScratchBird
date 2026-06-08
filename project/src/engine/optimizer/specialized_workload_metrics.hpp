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
#include <optional>
#include <string>
#include <vector>

namespace scratchbird::engine::optimizer {

// SEARCH_KEY: OEIC_SPECIALIZED_WORKLOAD_OPTIMIZER_METRICS
// Live specialized-workload metrics are route-costing and feedback evidence
// only. They cannot provide transaction finality, visibility, security,
// parser, donor, recovery, WAL, cluster, or benchmark authority.

struct SpecializedWorkloadMetricAuthority {
  bool provider_contract_authoritative = false;
  bool route_runtime_authoritative = false;
  bool descriptor_visibility_proof_present = false;
  bool index_generation_proof_present = false;
  bool engine_scope_bound = false;
  bool exact_recheck_preserved = false;
  bool mga_recheck_preserved = false;
  bool security_recheck_preserved = false;
  bool candidate_set_runtime_authoritative = false;
  bool document_runtime_authoritative = false;
  bool search_runtime_authoritative = false;
  bool vector_runtime_authoritative = false;
  bool graph_runtime_authoritative = false;
  bool time_series_runtime_authoritative = false;
  bool parser_or_donor_authority = false;
  bool client_finality_or_visibility_authority = false;
  bool metric_visibility_or_finality_authority = false;
  bool metric_recovery_authority = false;
  bool wal_or_redo_authority = false;
  bool cluster_authority = false;
  bool benchmark_authority = false;
};

struct SpecializedWorkloadMetricSample {
  std::string scope_uuid;
  std::string route_label;
  std::string plan_node_id;
  std::string workload_family;
  std::string provider_id;
  std::string index_generation;
  std::string result_contract_hash;
  std::string evidence_digest;
  std::uint64_t source_generation = 0;
  std::uint64_t freshness_microseconds = 0;
  std::uint64_t max_freshness_microseconds = 60000000;

  std::optional<double> candidate_set_cardinality;
  std::optional<double> candidate_set_density;
  std::optional<double> candidate_set_recheck_ratio;
  std::optional<double> specialized_exact_recheck_rows;
  std::optional<double> specialized_false_positive_ratio;
  std::optional<double> document_path_selectivity;
  std::optional<double> text_posting_length;
  std::optional<std::uint64_t> text_blockmax_skips;
  std::optional<double> vector_recall_observed;
  std::optional<double> vector_rerank_count;
  std::optional<double> graph_frontier_width;
  std::optional<double> graph_adjacency_degree;
  std::optional<double> time_series_bucket_count;
  std::optional<double> time_series_rollup_selectivity;

  SpecializedWorkloadMetricAuthority authority;
};

struct SpecializedWorkloadMetricPublishResult {
  bool ok = false;
  std::string diagnostic_code;
  std::string detail;
  std::vector<std::string> evidence;
  std::vector<scratchbird::core::metrics::MetricValidationResult>
      metric_results;
};

scratchbird::core::metrics::MetricValidationResult
EnsureSpecializedWorkloadMetricDescriptors(
    scratchbird::core::metrics::MetricRegistry* registry = nullptr);

SpecializedWorkloadMetricPublishResult PublishSpecializedWorkloadMetrics(
    const SpecializedWorkloadMetricSample& sample);

}  // namespace scratchbird::engine::optimizer

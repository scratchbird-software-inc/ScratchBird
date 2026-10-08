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

namespace scratchbird::core::index {

// SEARCH_KEY: OEIC_INDEX_FAMILY_OPTIMIZER_METRICS
// Index-family optimizer metrics are advisory costing inputs only. They are
// produced by physical/runtime index providers and must never become
// transaction finality, visibility, security, parser, reference, recovery, WAL, or
// benchmark authority.

struct IndexOptimizerRuntimeMetricAuthority {
  bool index_descriptor_authoritative = false;
  bool index_generation_authoritative = false;
  bool family_provider_authoritative = false;
  bool engine_scope_bound = false;
  bool exact_recheck_preserved = false;
  bool exact_rerank_preserved = false;
  bool maintenance_runtime_authoritative = false;
  bool candidate_set_runtime_authoritative = false;
  bool search_runtime_authoritative = false;
  bool vector_runtime_authoritative = false;
  bool graph_runtime_authoritative = false;
  bool document_path_runtime_authoritative = false;
  bool parser_or_reference_authority = false;
  bool client_finality_or_visibility_authority = false;
  bool metric_visibility_or_finality_authority = false;
  bool metric_recovery_authority = false;
  bool wal_or_redo_authority = false;
  bool cluster_authority = false;
  bool benchmark_authority = false;
};

struct IndexOptimizerRuntimeMetricSample {
  scratchbird::core::metrics::MetricUuid scope_uuid;
  scratchbird::core::metrics::MetricUuid database_uuid;
  scratchbird::core::metrics::MetricUuid node_uuid;
  std::string route_label;
  std::string plan_node_id;
  scratchbird::core::metrics::MetricUuid index_uuid;
  std::string index_family;
  std::uint64_t index_generation = 0;
  std::string evidence_digest;
  std::uint64_t source_generation = 0;
  std::uint64_t freshness_microseconds = 0;
  std::uint64_t max_freshness_microseconds = 60000000;

  std::optional<double> index_selectivity;
  std::optional<double> index_false_positive_ratio;
  std::optional<std::uint64_t> index_recheck_count;
  std::optional<std::uint64_t> index_backlog_entries;
  std::optional<std::uint64_t> btree_depth;
  std::optional<std::uint64_t> btree_leaf_pages;
  std::optional<std::uint64_t> hash_collision_depth;
  std::optional<std::uint64_t> hash_overflow_depth;
  std::optional<double> bitmap_density;
  std::optional<double> bloom_observed_fpr;
  std::optional<double> zone_prune_selectivity;
  std::optional<std::uint64_t> text_posting_length;
  std::optional<std::uint64_t> text_blockmax_skips;
  std::optional<double> vector_recall_observed;
  std::optional<std::uint64_t> vector_rerank_count;
  std::optional<double> vector_tombstone_ratio;
  std::optional<std::uint64_t> graph_frontier_width;
  std::optional<std::uint64_t> graph_adjacency_degree;
  std::optional<double> document_path_selectivity;

  IndexOptimizerRuntimeMetricAuthority authority;
};

struct IndexOptimizerRuntimeMetricPublishResult {
  bool ok = false;
  std::string diagnostic_code;
  std::string detail;
  std::vector<std::string> evidence;
  scratchbird::core::metrics::MetricUuid index_uuid;
  std::uint64_t index_generation = 0;
  std::vector<scratchbird::core::metrics::MetricValidationResult>
      metric_results;
};

const std::vector<scratchbird::core::metrics::MetricDescriptorDefinition>&
IndexOptimizerRuntimeMetricDescriptorDefinitions();

scratchbird::core::metrics::MetricValidationResult
EnsureIndexOptimizerRuntimeMetricDescriptors(
    scratchbird::core::metrics::MetricRegistry* registry = nullptr);

IndexOptimizerRuntimeMetricPublishResult PublishIndexOptimizerRuntimeMetrics(
    const IndexOptimizerRuntimeMetricSample& sample);

}  // namespace scratchbird::core::index

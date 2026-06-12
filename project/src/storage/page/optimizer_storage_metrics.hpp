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

namespace scratchbird::storage::page {

// SEARCH_KEY: OEIC_STORAGE_IO_OPTIMIZER_METRICS
// Storage-owned optimizer metrics. These samples are costing and diagnostics
// inputs only. They never supply transaction finality, visibility, security,
// recovery, parser, reference, or benchmark authority.

struct OptimizerStorageMetricAuthority {
  bool storage_page_manager_authoritative = false;
  bool filespace_identity_authoritative = false;
  bool engine_scope_bound = false;
  bool parser_or_reference_authority = false;
  bool client_finality_or_visibility_authority = false;
  bool metric_visibility_or_finality_authority = false;
  bool metric_recovery_authority = false;
  bool benchmark_authority = false;
};

struct OptimizerStorageMetricSample {
  std::string scope_uuid;
  std::string database_uuid;
  std::string filespace_uuid;
  std::string node_uuid = "local";
  std::string route_label;
  std::string page_family;
  std::string page_class;
  std::string device_profile = "default";
  std::string evidence_digest;
  std::uint64_t source_generation = 0;
  std::uint64_t page_count = 0;
  std::uint64_t resident_pages = 0;
  std::uint64_t pinned_pages = 0;
  std::uint64_t dirty_pages = 0;
  std::uint64_t writeback_pages = 0;
  std::uint64_t cache_hits = 0;
  std::uint64_t cache_misses = 0;
  std::uint64_t prefetch_considered = 0;
  std::uint64_t prefetch_scheduled = 0;
  std::uint64_t prefetch_used = 0;
  std::uint64_t prefetch_wasted = 0;
  std::uint64_t sequential_read_latency_microseconds = 0;
  std::uint64_t random_read_latency_microseconds = 0;
  std::uint64_t filespace_total_bytes = 0;
  std::uint64_t filespace_used_bytes = 0;
  std::uint64_t filespace_free_bytes = 0;
  std::uint64_t filespace_reserved_bytes = 0;
  std::uint64_t freshness_microseconds = 0;
  std::uint64_t max_freshness_microseconds = 60000000;
  OptimizerStorageMetricAuthority authority;
};

struct OptimizerStorageMetricPublishResult {
  bool ok = false;
  std::string diagnostic_code;
  std::string detail;
  std::vector<std::string> evidence;
  std::vector<scratchbird::core::metrics::MetricValidationResult>
      metric_results;
};

scratchbird::core::metrics::MetricValidationResult
EnsureOptimizerStorageMetricDescriptors(
    scratchbird::core::metrics::MetricRegistry* registry = nullptr);

OptimizerStorageMetricPublishResult PublishOptimizerStorageMetrics(
    const OptimizerStorageMetricSample& sample);

}  // namespace scratchbird::storage::page

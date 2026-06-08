// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// SB-INDEX-METRICS-CLOSURE-ANCHOR

#include "index_access_method.hpp"
#include "metric_registry.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace scratchbird::core::index {

using scratchbird::core::metrics::MetricValidationResult;
using scratchbird::core::metrics::MetricValue;
using scratchbird::core::platform::u64;

struct IndexMetricIdentity {
  std::string index_uuid;
  std::string index_family;
  std::string route_kind = "unspecified";
  std::string index_generation;
  u64 route_generation = 0;
  u64 source_generation = 0;
  u64 freshness_microseconds = 0;
  u64 max_freshness_microseconds = 60000000;
  std::string source_kind = "index_operation_runtime";
  std::string provenance = "CEIC-040_INDEX_OPERATION_METRICS";
  std::string evidence_digest;
  std::string semantic_profile_id = "sb_native_default";
  std::string operation = "unspecified";
  std::string result = "ok";
  std::string reason = "none";
  std::string page_family = "index";
  std::string filespace_uuid = "none";
  std::string agent_class = "none";
};

struct IndexLogicalMetricDelta {
  double candidates = 0;
  double visible = 0;
  double rechecks = 0;
  double fallback_sorts = 0;
};

struct IndexPhysicalMetricDelta {
  double pages_read = 0;
  double pages_written = 0;
  double splits = 0;
  double merges = 0;
  double depth = 0;
  double density_ratio = 0;
  double fragmentation_ratio = 0;
};

struct IndexMaintenanceMetricDelta {
  double operations = 0;
  double verify_failures = 0;
  double repair_actions = 0;
  double stale_resources = 0;
  double quarantine_events = 0;
  double progress_percent = 0;
};

struct IndexOptimizerMetricDelta {
  double estimate_error_ratio = 0;
  double stale_stats = 0;
  double invalidations = 0;
  double fallback_refusals = 0;
};

struct IndexDonorProfileMetricDelta {
  double profile_hits = 0;
  double profile_refusals = 0;
  double rechecks = 0;
  double fallback_sorts = 0;
  double order_proofs = 0;
  double catalog_projections = 0;
  double compatibility_diagnostics = 0;
};

struct IndexResidencyMetricDelta {
  double resident_bytes = 0;
  double hits = 0;
  double misses = 0;
  double evictions = 0;
  double pressure_score = 0;
  double degraded = 0;
  double refused = 0;
};

struct IndexPageFilespaceMetricDelta {
  double allocation_requests = 0;
  double relocation_requests = 0;
  double shrink_ready_bytes = 0;
};

struct IndexMetricPublishResult {
  bool ok = true;
  std::vector<MetricValidationResult> results;
};

// CEIC_040_INDEX_OPERATION_METRICS_SUPPORT_BUNDLE
// Index operation metrics and support-bundle rows are evidence only. They must
// never become transaction finality, visibility, authorization/security,
// recovery, parser, donor, WAL, benchmark-clean, optimizer-plan, index-finality,
// provider-finality, cluster, or agent authority.
enum class IndexOperationCounterKind : std::uint8_t {
  probe,
  insert,
  remove,
  split,
  merge,
  collision,
  recheck,
  false_positive,
  cleanup,
  corruption,
  recovery,
  benchmark
};

struct IndexOperationMetricCounters {
  u64 probe = 0;
  u64 insert = 0;
  u64 remove = 0;
  u64 split = 0;
  u64 merge = 0;
  u64 collision = 0;
  u64 recheck = 0;
  u64 false_positive = 0;
  u64 cleanup = 0;
  u64 corruption = 0;
  u64 recovery = 0;
  u64 benchmark = 0;
};

struct IndexOperationMetricAuthorityBoundary {
  bool transaction_finality_authority = false;
  bool visibility_authority = false;
  bool authorization_security_authority = false;
  bool security_authority = false;
  bool recovery_authority = false;
  bool parser_authority = false;
  bool donor_authority = false;
  bool wal_authority = false;
  bool benchmark_authority = false;
  bool benchmark_clean_authority = false;
  bool optimizer_plan_authority = false;
  bool optimizer_plan_finality_authority = false;
  bool index_finality_authority = false;
  bool provider_finality_authority = false;
  bool local_cluster_authority = false;
  bool cluster_authority = false;
  bool cluster_action_authority = false;
  bool agent_action_authority = false;
};

struct IndexOperationMetricSample {
  IndexMetricIdentity identity;
  IndexOperationMetricCounters counters;
  IndexOperationMetricAuthorityBoundary authority_boundary;
  bool runtime_operation_path = false;
  bool descriptor_only_static_evidence = false;
  bool support_bundle_row_producer = true;
  bool protected_material_present = false;
  bool local_cluster_participation = false;
  bool cluster_participation_requested = false;
  bool ceic_041_crash_matrix_claimed = false;
  bool ceic_042_readiness_drift_claimed = false;
  bool ceic_090_integrated_metrics_coverage_claimed = false;
  bool ceic_091_integrated_support_bundle_claimed = false;
  bool donor_dominance_claimed = false;
  bool all_index_readiness_claimed = false;
  bool enterprise_readiness_claimed = false;
};

struct IndexOperationMetricPublishResult {
  bool ok = false;
  std::string diagnostic_code;
  std::string detail;
  std::vector<std::string> evidence;
  std::vector<std::string> emitted_counter_families;
  std::vector<MetricValidationResult> metric_results;
};

struct IndexOperationMetricSupportBundleLimits {
  u64 max_rows = 128;
  u64 max_output_bytes = 32ull * 1024ull;
  u64 max_key_bytes = 128;
  u64 max_value_bytes = 512;
  u64 max_label_bytes = 512;
};

struct IndexOperationMetricSupportBundleRow {
  std::string key;
  std::string value;
  std::string metric_family;
  std::string labels;
  std::string index_uuid;
  std::string index_family;
  std::string route_kind;
  std::string operation;
  std::string result;
  std::string reason;
  std::string index_generation;
  std::string route_generation;
  std::string source_generation;
  std::string source_kind;
  std::string provenance;
  std::string evidence_digest;
  u64 freshness_microseconds = 0;
  bool redacted = false;
  std::string redaction_class = "public";
  std::string tamper_evidence_digest;
  bool evidence_only = true;
  bool transaction_finality_authority = false;
  bool visibility_authority = false;
  bool authorization_security_authority = false;
  bool recovery_authority = false;
  bool parser_authority = false;
  bool donor_authority = false;
  bool wal_authority = false;
  bool benchmark_authority = false;
  bool optimizer_plan_authority = false;
  bool index_finality_authority = false;
  bool provider_finality_authority = false;
  bool local_cluster_authority = false;
  bool cluster_authority = false;
  bool agent_action_authority = false;
};

struct IndexOperationMetricSupportBundleRequest {
  std::vector<MetricValue> metrics;
  IndexOperationMetricSupportBundleLimits limits;
  IndexOperationMetricAuthorityBoundary authority_boundary;
  std::string filter_index_uuid;
  bool require_all_operation_counters = true;
  bool descriptor_only_static_evidence = false;
  bool local_cluster_participation = false;
  bool cluster_participation_requested = false;
  bool ceic_041_crash_matrix_claimed = false;
  bool ceic_042_readiness_drift_claimed = false;
  bool ceic_090_integrated_metrics_coverage_claimed = false;
  bool ceic_091_integrated_support_bundle_claimed = false;
  bool donor_dominance_claimed = false;
  bool all_index_readiness_claimed = false;
  bool enterprise_readiness_claimed = false;
};

struct IndexOperationMetricSupportBundleResult {
  bool ok = false;
  std::string diagnostic_code;
  std::string detail;
  std::vector<IndexOperationMetricSupportBundleRow> rows;
  std::vector<std::string> evidence;
  std::vector<std::string> missing_counter_families;
  u64 redacted_row_count = 0;
  u64 dropped_row_count = 0;
  u64 output_bytes = 0;
  u64 row_limit = 0;
  bool bounded = true;
  bool protected_material_excluded = true;
  bool deterministic_labels = true;
  bool route_family_generation_evidence = false;
  bool freshness_source_provenance_present = false;
  bool evidence_only = true;
  bool all_required_counters_present = false;
};

MetricValidationResult EnsureIndexMetricDescriptors();
MetricValidationResult EnsureIndexOperationMetricDescriptors(
    scratchbird::core::metrics::MetricRegistry* registry = nullptr);
IndexMetricPublishResult PublishIndexLogicalMetrics(const IndexMetricIdentity& identity,
                                                    const IndexLogicalMetricDelta& delta);
IndexMetricPublishResult PublishIndexPhysicalMetrics(const IndexMetricIdentity& identity,
                                                     const IndexPhysicalMetricDelta& delta);
IndexMetricPublishResult PublishIndexMaintenanceMetrics(const IndexMetricIdentity& identity,
                                                        const IndexMaintenanceMetricDelta& delta);
IndexMetricPublishResult PublishIndexOptimizerMetrics(const IndexMetricIdentity& identity,
                                                      const IndexOptimizerMetricDelta& delta);
IndexMetricPublishResult PublishIndexDonorProfileMetrics(const IndexMetricIdentity& identity,
                                                         const IndexDonorProfileMetricDelta& delta);
IndexMetricPublishResult PublishIndexResidencyMetrics(const IndexMetricIdentity& identity,
                                                      const IndexResidencyMetricDelta& delta);
IndexMetricPublishResult PublishIndexPageFilespaceMetrics(const IndexMetricIdentity& identity,
                                                          const IndexPageFilespaceMetricDelta& delta);
const char* IndexOperationCounterName(IndexOperationCounterKind kind);
const char* IndexOperationCounterFamily(IndexOperationCounterKind kind);
const std::vector<IndexOperationCounterKind>& RequiredIndexOperationCounterKinds();
IndexOperationMetricPublishResult PublishIndexOperationMetrics(
    const IndexOperationMetricSample& sample);
IndexOperationMetricSupportBundleResult BuildIndexOperationMetricSupportBundle(
    IndexOperationMetricSupportBundleRequest request);

}  // namespace scratchbird::core::index

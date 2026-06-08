// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "optimizer_metric_manifest.hpp"

#include <set>
#include <string_view>
#include <utility>

namespace scratchbird::engine::optimizer {
namespace {

namespace metrics = scratchbird::core::metrics;

using metrics::MetricDescriptor;
using metrics::MetricLabelDescriptor;
using metrics::MetricReadiness;
using metrics::MetricType;
using metrics::MetricUnit;

OptimizerEnterpriseMetricEntry Metric(
    std::string metric_family,
    MetricType metric_type,
    MetricUnit metric_unit,
    std::string producer_owner,
    std::string consumer_owner,
    std::string producer_anchor,
    std::string consumer_anchor,
    std::vector<std::string> required_evidence,
    OptimizerMetricProducerState producer_state,
    OptimizerMetricFreshnessPolicy freshness_policy,
    OptimizerMetricRetentionClass retention_class,
    OptimizerMetricRedactionClass redaction_class =
        OptimizerMetricRedactionClass::redacted_scope,
    OptimizerMetricSupportBundleClass support_bundle_class =
        OptimizerMetricSupportBundleClass::included_redacted,
    bool enterprise_route_consumable = false,
    bool benchmark_clean_consumable = false) {
  OptimizerEnterpriseMetricEntry entry;
  entry.metric_family = std::move(metric_family);
  entry.registry_family = "sb_optimizer_" + entry.metric_family;
  entry.metric_type = metric_type;
  entry.metric_unit = metric_unit;
  entry.producer_owner = std::move(producer_owner);
  entry.consumer_owner = std::move(consumer_owner);
  entry.producer_anchor = std::move(producer_anchor);
  entry.consumer_anchor = std::move(consumer_anchor);
  entry.required_evidence = std::move(required_evidence);
  entry.producer_state = producer_state;
  entry.freshness_policy = freshness_policy;
  entry.retention_class = retention_class;
  entry.redaction_class = redaction_class;
  entry.support_bundle_class = support_bundle_class;
  entry.enterprise_route_consumable = enterprise_route_consumable;
  entry.benchmark_clean_consumable = benchmark_clean_consumable;
  return entry;
}

bool Contains(std::string_view value, std::string_view token) {
  return value.find(token) != std::string_view::npos;
}

MetricReadiness DescriptorReadiness(OptimizerMetricProducerState state) {
  switch (state) {
    case OptimizerMetricProducerState::live_maintained:
    case OptimizerMetricProducerState::derived_from_registered_sources:
      return MetricReadiness::implemented;
    case OptimizerMetricProducerState::owned_runtime_required:
    case OptimizerMetricProducerState::cluster_external:
      return MetricReadiness::contract_ready_unwired;
  }
  return MetricReadiness::contract_ready_unwired;
}

MetricDescriptor DescriptorForEntry(const OptimizerEnterpriseMetricEntry& entry) {
  MetricDescriptor descriptor;
  descriptor.family = entry.registry_family;
  descriptor.type = entry.metric_type;
  descriptor.unit = entry.metric_unit;
  descriptor.namespace_path = "sys.metrics.optimizer.enterprise";
  descriptor.help = "Optimizer enterprise metric family " + entry.metric_family + ".";
  descriptor.producer_owner = entry.producer_owner;
  descriptor.security_family = "OPTIMIZER_METRICS";
  descriptor.visibility = metrics::MetricVisibilityScope::family;
  descriptor.readiness = DescriptorReadiness(entry.producer_state);
  descriptor.cluster_only =
      entry.producer_state == OptimizerMetricProducerState::cluster_external;
  descriptor.labels = {MetricLabelDescriptor{"scope_uuid", true, false},
                       MetricLabelDescriptor{"route_label", true, false},
                       MetricLabelDescriptor{"plan_node_id", false, false},
                       MetricLabelDescriptor{"metric_family", true, false},
                       MetricLabelDescriptor{"result", false, false},
                       MetricLabelDescriptor{"source_generation", true, false},
                       MetricLabelDescriptor{"evidence_digest", true, true}};
  descriptor.aliases = {entry.metric_family};
  if (entry.metric_type == MetricType::histogram) {
    descriptor.histogram_buckets = {1, 10, 100, 1000, 10000, 100000,
                                    1000000, 10000000};
  }
  return descriptor;
}

}  // namespace

const char* OptimizerMetricProducerStateName(OptimizerMetricProducerState value) {
  switch (value) {
    case OptimizerMetricProducerState::live_maintained: return "live_maintained";
    case OptimizerMetricProducerState::owned_runtime_required: return "owned_runtime_required";
    case OptimizerMetricProducerState::derived_from_registered_sources:
      return "derived_from_registered_sources";
    case OptimizerMetricProducerState::cluster_external: return "cluster_external";
  }
  return "owned_runtime_required";
}

const char* OptimizerMetricFreshnessPolicyName(OptimizerMetricFreshnessPolicy value) {
  switch (value) {
    case OptimizerMetricFreshnessPolicy::per_operator_completion:
      return "per_operator_completion";
    case OptimizerMetricFreshnessPolicy::per_route_execution:
      return "per_route_execution";
    case OptimizerMetricFreshnessPolicy::per_metric_snapshot:
      return "per_metric_snapshot";
    case OptimizerMetricFreshnessPolicy::per_catalog_epoch:
      return "per_catalog_epoch";
    case OptimizerMetricFreshnessPolicy::per_storage_generation:
      return "per_storage_generation";
    case OptimizerMetricFreshnessPolicy::per_index_generation:
      return "per_index_generation";
    case OptimizerMetricFreshnessPolicy::per_transaction_horizon:
      return "per_transaction_horizon";
    case OptimizerMetricFreshnessPolicy::per_support_bundle_capture:
      return "per_support_bundle_capture";
  }
  return "per_metric_snapshot";
}

const char* OptimizerMetricRetentionClassName(OptimizerMetricRetentionClass value) {
  switch (value) {
    case OptimizerMetricRetentionClass::route_short_window: return "route_short_window";
    case OptimizerMetricRetentionClass::plan_feedback_history: return "plan_feedback_history";
    case OptimizerMetricRetentionClass::catalog_statistics_history:
      return "catalog_statistics_history";
    case OptimizerMetricRetentionClass::support_bundle_window:
      return "support_bundle_window";
    case OptimizerMetricRetentionClass::security_audit_window:
      return "security_audit_window";
  }
  return "route_short_window";
}

const char* OptimizerMetricRedactionClassName(OptimizerMetricRedactionClass value) {
  switch (value) {
    case OptimizerMetricRedactionClass::public_aggregate: return "public_aggregate";
    case OptimizerMetricRedactionClass::redacted_scope: return "redacted_scope";
    case OptimizerMetricRedactionClass::protected_digest: return "protected_digest";
    case OptimizerMetricRedactionClass::security_restricted: return "security_restricted";
  }
  return "redacted_scope";
}

const char* OptimizerMetricSupportBundleClassName(
    OptimizerMetricSupportBundleClass value) {
  switch (value) {
    case OptimizerMetricSupportBundleClass::included_redacted:
      return "included_redacted";
    case OptimizerMetricSupportBundleClass::digest_only: return "digest_only";
    case OptimizerMetricSupportBundleClass::protected_summary:
      return "protected_summary";
    case OptimizerMetricSupportBundleClass::omitted_from_default_bundle:
      return "omitted_from_default_bundle";
  }
  return "included_redacted";
}

const std::vector<OptimizerEnterpriseMetricEntry>&
OptimizerEnterpriseMetricManifest() {
  // SEARCH_KEY: OEIC_OPTIMIZER_METRIC_OWNERSHIP_MATRIX
  static const std::vector<OptimizerEnterpriseMetricEntry> entries = {
      Metric("operator_actual_rows", MetricType::gauge, MetricUnit::count,
             "executor_runtime", "adaptive_cardinality_feedback",
             "project/src/engine/executor", "adaptive_cardinality_feedback.*",
             {"route_label", "plan_node_id"},
             OptimizerMetricProducerState::live_maintained,
             OptimizerMetricFreshnessPolicy::per_operator_completion,
             OptimizerMetricRetentionClass::plan_feedback_history),
      Metric("operator_rows_examined", MetricType::gauge, MetricUnit::count,
             "executor_runtime", "optimizer_cost_feedback",
             "project/src/engine/executor", "optimizer_feedback.*",
             {"plan_node_id"},
             OptimizerMetricProducerState::live_maintained,
             OptimizerMetricFreshnessPolicy::per_operator_completion,
             OptimizerMetricRetentionClass::plan_feedback_history),
      Metric("operator_rows_filtered", MetricType::gauge, MetricUnit::count,
             "executor_runtime", "selectivity_feedback",
             "project/src/engine/executor", "selectivity_model.*",
             {"predicate_digest"},
             OptimizerMetricProducerState::live_maintained,
             OptimizerMetricFreshnessPolicy::per_operator_completion,
             OptimizerMetricRetentionClass::plan_feedback_history),
      Metric("operator_loop_count", MetricType::gauge, MetricUnit::count,
             "executor_runtime", "runtime_plan_payload",
             "project/src/engine/executor", "runtime_consumption_evidence.*",
             {"plan_node_id"},
             OptimizerMetricProducerState::live_maintained,
             OptimizerMetricFreshnessPolicy::per_operator_completion,
             OptimizerMetricRetentionClass::plan_feedback_history),
      Metric("operator_cpu_time", MetricType::histogram, MetricUnit::microseconds,
             "executor_runtime", "cost_calibration",
             "project/src/engine/executor", "optimizer_cost_full.*",
             {"redacted_timing_profile"},
             OptimizerMetricProducerState::live_maintained,
             OptimizerMetricFreshnessPolicy::per_operator_completion,
             OptimizerMetricRetentionClass::plan_feedback_history,
             OptimizerMetricRedactionClass::protected_digest,
             OptimizerMetricSupportBundleClass::digest_only),
      Metric("memory_grant_bytes", MetricType::gauge, MetricUnit::bytes,
             "core_memory", "optimizer_memory_feedback",
             "project/src/core/memory/query_memory_arena.*",
             "optimizer_memory_feedback_bridge.*", {"reservation_id"},
             OptimizerMetricProducerState::live_maintained,
             OptimizerMetricFreshnessPolicy::per_metric_snapshot,
             OptimizerMetricRetentionClass::plan_feedback_history),
      Metric("memory_peak_bytes", MetricType::gauge, MetricUnit::bytes,
             "core_memory", "spill_costing_and_explain",
             "project/src/core/memory/query_memory_arena.*",
             "optimizer_memory_feedback_bridge.*;optimizer_explain.*",
             {"query_id", "plan_node_id"},
             OptimizerMetricProducerState::live_maintained,
             OptimizerMetricFreshnessPolicy::per_metric_snapshot,
             OptimizerMetricRetentionClass::plan_feedback_history),
      Metric("spill_bytes", MetricType::gauge, MetricUnit::bytes,
             "temp_workspace", "spill_cost_model",
             "project/src/core/memory/temp_workspace_lifecycle.*",
             "optimizer_cost_full.*", {"workspace_id", "operator_id"},
             OptimizerMetricProducerState::live_maintained,
             OptimizerMetricFreshnessPolicy::per_metric_snapshot,
             OptimizerMetricRetentionClass::plan_feedback_history),
      Metric("spill_passes", MetricType::gauge, MetricUnit::count,
             "executor_runtime", "spill_cost_model",
             "project/src/engine/executor", "optimizer_cost_full.*",
             {"operator_id"},
             OptimizerMetricProducerState::live_maintained,
             OptimizerMetricFreshnessPolicy::per_operator_completion,
             OptimizerMetricRetentionClass::plan_feedback_history),
      Metric("page_cache_hit_miss", MetricType::counter, MetricUnit::count,
             "storage_page", "scan_lookup_costing", "storage/page/page_cache.*",
             "optimizer_cost_full.*", {"filespace_uuid", "page_class"},
             OptimizerMetricProducerState::live_maintained,
             OptimizerMetricFreshnessPolicy::per_storage_generation,
             OptimizerMetricRetentionClass::route_short_window),
      Metric("prefetch_usefulness", MetricType::gauge, MetricUnit::ratio,
             "storage_prefetch", "prefetch_planning",
             "project/src/storage;project/src/engine/executor",
             "physical_plan_prefetch.*", {"prefetch_request_id"},
             OptimizerMetricProducerState::live_maintained,
             OptimizerMetricFreshnessPolicy::per_route_execution,
             OptimizerMetricRetentionClass::plan_feedback_history),
      Metric("page_count", MetricType::gauge, MetricUnit::count,
             "storage_page", "scan_lookup_costing",
             "storage/page/page_cache.*;storage/page/optimizer_storage_metrics.*",
             "optimizer_cost_full.*", {"filespace_uuid", "page_class"},
             OptimizerMetricProducerState::live_maintained,
             OptimizerMetricFreshnessPolicy::per_storage_generation,
             OptimizerMetricRetentionClass::catalog_statistics_history),
      Metric("page_cache_dirty_pressure", MetricType::gauge, MetricUnit::ratio,
             "storage_page", "scan_spill_costing",
             "storage/page/page_cache.*;storage/page/optimizer_storage_metrics.*",
             "optimizer_cost_full.*", {"filespace_uuid", "dirty_pages"},
             OptimizerMetricProducerState::live_maintained,
             OptimizerMetricFreshnessPolicy::per_storage_generation,
             OptimizerMetricRetentionClass::route_short_window),
      Metric("page_cache_pin_pressure", MetricType::gauge, MetricUnit::ratio,
             "storage_page", "scan_spill_costing",
             "storage/page/page_cache.*;storage/page/optimizer_storage_metrics.*",
             "optimizer_cost_full.*", {"filespace_uuid", "pinned_pages"},
             OptimizerMetricProducerState::live_maintained,
             OptimizerMetricFreshnessPolicy::per_storage_generation,
             OptimizerMetricRetentionClass::route_short_window),
      Metric("writeback_pressure", MetricType::gauge, MetricUnit::ratio,
             "storage_page", "scan_spill_costing",
             "storage/page/page_cache.*;storage/page/optimizer_storage_metrics.*",
             "optimizer_cost_full.*", {"filespace_uuid", "writeback_pages"},
             OptimizerMetricProducerState::live_maintained,
             OptimizerMetricFreshnessPolicy::per_storage_generation,
             OptimizerMetricRetentionClass::route_short_window),
      Metric("sequential_page_cost", MetricType::histogram, MetricUnit::microseconds,
             "storage_disk", "cost_model_calibration",
             "storage/disk;core/metrics/metric_contracts.*",
             "optimizer_cost_full.*", {"device_profile"},
             OptimizerMetricProducerState::live_maintained,
             OptimizerMetricFreshnessPolicy::per_storage_generation,
             OptimizerMetricRetentionClass::catalog_statistics_history),
      Metric("random_page_cost", MetricType::histogram, MetricUnit::microseconds,
             "storage_disk", "cost_model_calibration",
             "storage/disk;core/metrics/metric_contracts.*",
             "optimizer_cost_full.*", {"device_profile"},
             OptimizerMetricProducerState::live_maintained,
             OptimizerMetricFreshnessPolicy::per_storage_generation,
             OptimizerMetricRetentionClass::catalog_statistics_history),
      Metric("filespace_pressure", MetricType::gauge, MetricUnit::ratio,
             "storage_filespace", "scan_spill_costing",
             "storage/filespace;core/metrics/metric_contracts.*",
             "optimizer_cost_full.*", {"filespace_uuid"},
             OptimizerMetricProducerState::live_maintained,
             OptimizerMetricFreshnessPolicy::per_storage_generation,
             OptimizerMetricRetentionClass::route_short_window),
      Metric("mga_cleanup_debt", MetricType::gauge, MetricUnit::bytes,
             "transaction_mga_cleanup", "mga_pressure_costing",
             "transaction/mga/transaction_cleanup.*", "optimizer_cost_full.*",
             {"transaction_horizon_evidence"},
             OptimizerMetricProducerState::live_maintained,
             OptimizerMetricFreshnessPolicy::per_transaction_horizon,
             OptimizerMetricRetentionClass::plan_feedback_history),
      Metric("mga_retained_dead_bytes", MetricType::gauge, MetricUnit::bytes,
             "transaction_mga_cleanup", "mga_pressure_costing",
             "transaction/mga;storage/page", "optimizer_cost_full.*",
             {"relation_or_filespace_scope"},
             OptimizerMetricProducerState::live_maintained,
             OptimizerMetricFreshnessPolicy::per_transaction_horizon,
             OptimizerMetricRetentionClass::plan_feedback_history),
      Metric("mga_chain_depth", MetricType::gauge, MetricUnit::count,
             "row_version_runtime", "visibility_cost",
             "transaction/mga/row_version.*", "optimizer_cost_full.*",
             {"relation_uuid", "page_class"},
             OptimizerMetricProducerState::live_maintained,
             OptimizerMetricFreshnessPolicy::per_transaction_horizon,
             OptimizerMetricRetentionClass::plan_feedback_history),
      Metric("mga_chain_scatter", MetricType::gauge, MetricUnit::count,
             "row_version_runtime", "visibility_cost",
             "transaction/mga/row_version.*", "optimizer_cost_full.*",
             {"relation_uuid", "locality_bucket"},
             OptimizerMetricProducerState::live_maintained,
             OptimizerMetricFreshnessPolicy::per_transaction_horizon,
             OptimizerMetricRetentionClass::plan_feedback_history),
      Metric("same_page_update_ratio", MetricType::gauge, MetricUnit::ratio,
             "row_version_runtime", "mga_pressure_costing",
             "transaction/mga/row_version.*", "optimizer_cost_full.*",
             {"relation_uuid", "page_class"},
             OptimizerMetricProducerState::live_maintained,
             OptimizerMetricFreshnessPolicy::per_transaction_horizon,
             OptimizerMetricRetentionClass::plan_feedback_history),
      Metric("index_backlog_entries", MetricType::gauge, MetricUnit::count,
             "index_runtime", "index_route_costing", "core/index/index_metrics.*",
             "optimizer_cost_full.*;access_path_full.*", {"index_uuid"},
             OptimizerMetricProducerState::live_maintained,
             OptimizerMetricFreshnessPolicy::per_index_generation,
             OptimizerMetricRetentionClass::plan_feedback_history,
             OptimizerMetricRedactionClass::redacted_scope,
             OptimizerMetricSupportBundleClass::included_redacted, true, true),
      Metric("index_selectivity", MetricType::gauge, MetricUnit::ratio,
             "index_runtime", "index_route_costing",
             "core/index/index_optimizer_runtime_metrics.*",
             "optimizer_cost_full.*;access_path_full.*",
             {"index_uuid", "index_generation", "exact_recheck_evidence"},
             OptimizerMetricProducerState::live_maintained,
             OptimizerMetricFreshnessPolicy::per_index_generation,
             OptimizerMetricRetentionClass::catalog_statistics_history,
             OptimizerMetricRedactionClass::redacted_scope,
             OptimizerMetricSupportBundleClass::included_redacted, true, true),
      Metric("index_false_positive_ratio", MetricType::gauge, MetricUnit::ratio,
             "index_runtime", "index_route_costing",
             "core/index/index_optimizer_runtime_metrics.*",
             "optimizer_cost_full.*;access_path_full.*",
             {"index_uuid", "index_generation", "exact_recheck_evidence"},
             OptimizerMetricProducerState::live_maintained,
             OptimizerMetricFreshnessPolicy::per_index_generation,
             OptimizerMetricRetentionClass::plan_feedback_history,
             OptimizerMetricRedactionClass::redacted_scope,
             OptimizerMetricSupportBundleClass::included_redacted, true, true),
      Metric("index_recheck_count", MetricType::counter, MetricUnit::count,
             "index_runtime", "index_route_costing",
             "core/index/index_optimizer_runtime_metrics.*",
             "optimizer_cost_full.*;access_path_full.*",
             {"index_uuid", "index_generation", "exact_recheck_evidence"},
             OptimizerMetricProducerState::live_maintained,
             OptimizerMetricFreshnessPolicy::per_route_execution,
             OptimizerMetricRetentionClass::plan_feedback_history,
             OptimizerMetricRedactionClass::redacted_scope,
             OptimizerMetricSupportBundleClass::included_redacted, true, true),
      Metric("commit_fence_pressure", MetricType::gauge, MetricUnit::count,
             "transaction_manager", "dml_visibility_costing",
             "transaction/mga/transaction_manager.*", "optimizer_cost_full.*",
             {"local_transaction_inventory_generation"},
             OptimizerMetricProducerState::live_maintained,
             OptimizerMetricFreshnessPolicy::per_transaction_horizon,
             OptimizerMetricRetentionClass::plan_feedback_history),
      Metric("btree_depth", MetricType::gauge, MetricUnit::count,
             "index_runtime", "index_lookup_costing", "core/index/index_metrics.*",
             "access_path_full.*", {"index_uuid"},
             OptimizerMetricProducerState::live_maintained,
             OptimizerMetricFreshnessPolicy::per_index_generation,
             OptimizerMetricRetentionClass::catalog_statistics_history,
             OptimizerMetricRedactionClass::redacted_scope,
             OptimizerMetricSupportBundleClass::included_redacted, true, true),
      Metric("btree_leaf_pages", MetricType::gauge, MetricUnit::count,
             "index_runtime", "index_range_costing", "core/index/index_metrics.*",
             "access_path_full.*", {"index_uuid"},
             OptimizerMetricProducerState::live_maintained,
             OptimizerMetricFreshnessPolicy::per_index_generation,
             OptimizerMetricRetentionClass::catalog_statistics_history,
             OptimizerMetricRedactionClass::redacted_scope,
             OptimizerMetricSupportBundleClass::included_redacted, true, true),
      Metric("hash_collision_depth", MetricType::gauge, MetricUnit::count,
             "index_runtime", "hash_lookup_costing",
             "core/index/index_optimizer_runtime_metrics.*",
             "access_path_full.*", {"hash_algorithm", "index_uuid"},
             OptimizerMetricProducerState::live_maintained,
             OptimizerMetricFreshnessPolicy::per_index_generation,
             OptimizerMetricRetentionClass::catalog_statistics_history,
             OptimizerMetricRedactionClass::redacted_scope,
             OptimizerMetricSupportBundleClass::included_redacted, true, true),
      Metric("hash_overflow_depth", MetricType::gauge, MetricUnit::count,
             "index_runtime", "hash_lookup_costing",
             "core/index/index_optimizer_runtime_metrics.*",
             "access_path_full.*", {"bucket_generation"},
             OptimizerMetricProducerState::live_maintained,
             OptimizerMetricFreshnessPolicy::per_index_generation,
             OptimizerMetricRetentionClass::catalog_statistics_history,
             OptimizerMetricRedactionClass::redacted_scope,
             OptimizerMetricSupportBundleClass::included_redacted, true, true),
      Metric("bitmap_density", MetricType::gauge, MetricUnit::ratio,
             "index_runtime", "bitmap_algebra_costing",
             "core/index/index_optimizer_runtime_metrics.*",
             "runtime_filter_pushdown.*", {"candidate_set_id"},
             OptimizerMetricProducerState::live_maintained,
             OptimizerMetricFreshnessPolicy::per_index_generation,
             OptimizerMetricRetentionClass::plan_feedback_history,
             OptimizerMetricRedactionClass::redacted_scope,
             OptimizerMetricSupportBundleClass::included_redacted, true, true),
      Metric("candidate_set_cardinality", MetricType::gauge, MetricUnit::count,
             "candidate_set_runtime", "specialized_fusion_costing",
             "engine/optimizer/specialized_workload_metrics.*",
             "runtime_filter_pushdown.*;specialized_planner.*",
             {"candidate_set_id", "exact_recheck_evidence"},
             OptimizerMetricProducerState::live_maintained,
             OptimizerMetricFreshnessPolicy::per_route_execution,
             OptimizerMetricRetentionClass::plan_feedback_history,
             OptimizerMetricRedactionClass::redacted_scope,
             OptimizerMetricSupportBundleClass::included_redacted, true, true),
      Metric("candidate_set_density", MetricType::gauge, MetricUnit::ratio,
             "candidate_set_runtime", "specialized_fusion_costing",
             "engine/optimizer/specialized_workload_metrics.*",
             "runtime_filter_pushdown.*;specialized_planner.*",
             {"candidate_set_id", "exact_recheck_evidence"},
             OptimizerMetricProducerState::live_maintained,
             OptimizerMetricFreshnessPolicy::per_route_execution,
             OptimizerMetricRetentionClass::plan_feedback_history,
             OptimizerMetricRedactionClass::redacted_scope,
             OptimizerMetricSupportBundleClass::included_redacted, true, true),
      Metric("candidate_set_recheck_ratio", MetricType::gauge, MetricUnit::ratio,
             "candidate_set_runtime", "specialized_fusion_costing",
             "engine/optimizer/specialized_workload_metrics.*",
             "runtime_filter_pushdown.*;specialized_planner.*",
             {"candidate_set_id", "mga_security_recheck_evidence"},
             OptimizerMetricProducerState::live_maintained,
             OptimizerMetricFreshnessPolicy::per_route_execution,
             OptimizerMetricRetentionClass::plan_feedback_history,
             OptimizerMetricRedactionClass::redacted_scope,
             OptimizerMetricSupportBundleClass::included_redacted, true, true),
      Metric("bloom_observed_fpr", MetricType::gauge, MetricUnit::ratio,
             "index_runtime", "negative_prune_costing",
             "core/index/index_optimizer_runtime_metrics.*",
             "access_path_full.*", {"index_uuid"},
             OptimizerMetricProducerState::live_maintained,
             OptimizerMetricFreshnessPolicy::per_index_generation,
             OptimizerMetricRetentionClass::catalog_statistics_history,
             OptimizerMetricRedactionClass::redacted_scope,
             OptimizerMetricSupportBundleClass::included_redacted, true, true),
      Metric("zone_prune_selectivity", MetricType::gauge, MetricUnit::ratio,
             "index_runtime", "summary_prune_costing",
             "core/index/index_optimizer_runtime_metrics.*",
             "partition_segment_pruning.*", {"summary_generation"},
             OptimizerMetricProducerState::live_maintained,
             OptimizerMetricFreshnessPolicy::per_index_generation,
             OptimizerMetricRetentionClass::catalog_statistics_history,
             OptimizerMetricRedactionClass::redacted_scope,
             OptimizerMetricSupportBundleClass::included_redacted, true, true),
      Metric("text_posting_length", MetricType::gauge, MetricUnit::count,
             "search_runtime", "search_costing", "core/index/text_*",
             "specialized_planner.*", {"term_digest", "segment_digest"},
             OptimizerMetricProducerState::live_maintained,
             OptimizerMetricFreshnessPolicy::per_index_generation,
             OptimizerMetricRetentionClass::catalog_statistics_history,
             OptimizerMetricRedactionClass::protected_digest,
             OptimizerMetricSupportBundleClass::digest_only, true, true),
      Metric("text_blockmax_skips", MetricType::counter, MetricUnit::count,
             "search_runtime", "topk_costing", "core/index/text_*",
             "specialized_planner.*", {"segment_generation"},
             OptimizerMetricProducerState::live_maintained,
             OptimizerMetricFreshnessPolicy::per_route_execution,
             OptimizerMetricRetentionClass::plan_feedback_history,
             OptimizerMetricRedactionClass::redacted_scope,
             OptimizerMetricSupportBundleClass::included_redacted, true, true),
      Metric("vector_recall_observed", MetricType::gauge, MetricUnit::ratio,
             "vector_runtime", "ann_costing", "core/index/vector_*",
             "specialized_planner.*", {"index_generation"},
             OptimizerMetricProducerState::live_maintained,
             OptimizerMetricFreshnessPolicy::per_index_generation,
             OptimizerMetricRetentionClass::catalog_statistics_history,
             OptimizerMetricRedactionClass::protected_digest,
             OptimizerMetricSupportBundleClass::protected_summary, true, true),
      Metric("vector_rerank_count", MetricType::gauge, MetricUnit::count,
             "vector_runtime", "exact_rerank_costing", "core/index/vector_*",
             "specialized_planner.*", {"query_family"},
             OptimizerMetricProducerState::live_maintained,
             OptimizerMetricFreshnessPolicy::per_route_execution,
             OptimizerMetricRetentionClass::plan_feedback_history,
             OptimizerMetricRedactionClass::redacted_scope,
             OptimizerMetricSupportBundleClass::included_redacted, true, true),
      Metric("vector_tombstone_ratio", MetricType::gauge, MetricUnit::ratio,
             "vector_runtime", "rebuild_query_costing", "core/index/vector_*",
             "specialized_planner.*", {"generation"},
             OptimizerMetricProducerState::live_maintained,
             OptimizerMetricFreshnessPolicy::per_index_generation,
             OptimizerMetricRetentionClass::catalog_statistics_history,
             OptimizerMetricRedactionClass::redacted_scope,
             OptimizerMetricSupportBundleClass::included_redacted, true, true),
      Metric("graph_frontier_width", MetricType::gauge, MetricUnit::count,
             "graph_runtime", "graph_route_costing", "core/index/graph_*",
             "specialized_planner.*", {"traversal_digest"},
             OptimizerMetricProducerState::live_maintained,
             OptimizerMetricFreshnessPolicy::per_route_execution,
             OptimizerMetricRetentionClass::plan_feedback_history,
             OptimizerMetricRedactionClass::redacted_scope,
             OptimizerMetricSupportBundleClass::included_redacted, true, true),
      Metric("graph_adjacency_degree", MetricType::gauge, MetricUnit::count,
             "graph_runtime", "traversal_costing", "core/index/graph_*",
             "specialized_planner.*", {"label_property_scope"},
             OptimizerMetricProducerState::live_maintained,
             OptimizerMetricFreshnessPolicy::per_index_generation,
             OptimizerMetricRetentionClass::catalog_statistics_history,
             OptimizerMetricRedactionClass::redacted_scope,
             OptimizerMetricSupportBundleClass::included_redacted, true, true),
      Metric("document_path_selectivity", MetricType::gauge, MetricUnit::ratio,
             "document_provider", "document_route_costing",
             "engine/internal_api/nosql;core/index/index_optimizer_runtime_metrics.*",
             "nosql_statistics_advisor.*;specialized_planner.*", {"path_digest"},
             OptimizerMetricProducerState::live_maintained,
             OptimizerMetricFreshnessPolicy::per_index_generation,
             OptimizerMetricRetentionClass::catalog_statistics_history,
             OptimizerMetricRedactionClass::protected_digest,
             OptimizerMetricSupportBundleClass::digest_only, true, true),
      Metric("specialized_exact_recheck_rows", MetricType::gauge,
             MetricUnit::count, "specialized_runtime",
             "specialized_fusion_costing",
             "engine/optimizer/specialized_workload_metrics.*",
             "specialized_planner.*;runtime_filter_pushdown.*",
             {"result_contract_hash", "exact_recheck_evidence"},
             OptimizerMetricProducerState::live_maintained,
             OptimizerMetricFreshnessPolicy::per_route_execution,
             OptimizerMetricRetentionClass::plan_feedback_history,
             OptimizerMetricRedactionClass::redacted_scope,
             OptimizerMetricSupportBundleClass::included_redacted, true, true),
      Metric("specialized_false_positive_ratio", MetricType::gauge,
             MetricUnit::ratio, "specialized_runtime",
             "specialized_fusion_costing",
             "engine/optimizer/specialized_workload_metrics.*",
             "specialized_planner.*;runtime_filter_pushdown.*",
             {"result_contract_hash", "exact_recheck_evidence"},
             OptimizerMetricProducerState::live_maintained,
             OptimizerMetricFreshnessPolicy::per_route_execution,
             OptimizerMetricRetentionClass::plan_feedback_history,
             OptimizerMetricRedactionClass::redacted_scope,
             OptimizerMetricSupportBundleClass::included_redacted, true, true),
      Metric("time_series_bucket_count", MetricType::gauge,
             MetricUnit::count, "time_series_runtime",
             "time_series_route_costing",
             "engine/optimizer/specialized_workload_metrics.*",
             "specialized_planner.*", {"bucket_generation"},
             OptimizerMetricProducerState::live_maintained,
             OptimizerMetricFreshnessPolicy::per_route_execution,
             OptimizerMetricRetentionClass::plan_feedback_history,
             OptimizerMetricRedactionClass::redacted_scope,
             OptimizerMetricSupportBundleClass::included_redacted, true, true),
      Metric("time_series_rollup_selectivity", MetricType::gauge,
             MetricUnit::ratio, "time_series_runtime",
             "time_series_route_costing",
             "engine/optimizer/specialized_workload_metrics.*",
             "specialized_planner.*", {"rollup_generation"},
             OptimizerMetricProducerState::live_maintained,
             OptimizerMetricFreshnessPolicy::per_route_execution,
             OptimizerMetricRetentionClass::plan_feedback_history,
             OptimizerMetricRedactionClass::redacted_scope,
             OptimizerMetricSupportBundleClass::included_redacted, true, true),
      Metric("route_plan_hash", MetricType::state, MetricUnit::state,
             "optimizer_explain", "driver_visible_parity",
             "optimizer_route_metrics.*;optimizer_explain.*",
             "wire;drivers;runtime_consumption_evidence.*",
             {"route_label", "plan_hash"},
             OptimizerMetricProducerState::live_maintained,
             OptimizerMetricFreshnessPolicy::per_route_execution,
             OptimizerMetricRetentionClass::support_bundle_window,
             OptimizerMetricRedactionClass::protected_digest,
             OptimizerMetricSupportBundleClass::digest_only, true, true),
      Metric("route_result_hash", MetricType::state, MetricUnit::state,
             "route_executor", "route_equivalence",
             "optimizer_route_metrics.*;engine/internal_api/query;wire;drivers", "runtime_consumption_evidence.*",
             {"route_label"},
             OptimizerMetricProducerState::live_maintained,
             OptimizerMetricFreshnessPolicy::per_route_execution,
             OptimizerMetricRetentionClass::support_bundle_window,
             OptimizerMetricRedactionClass::protected_digest,
             OptimizerMetricSupportBundleClass::digest_only, true, true),
      Metric("explain_digest", MetricType::state, MetricUnit::state,
             "optimizer_explain", "driver_visible_parity",
             "optimizer_route_metrics.*;optimizer_explain.*",
             "wire;drivers;runtime_consumption_evidence.*",
             {"route_label", "redaction_epoch"},
             OptimizerMetricProducerState::live_maintained,
             OptimizerMetricFreshnessPolicy::per_route_execution,
             OptimizerMetricRetentionClass::support_bundle_window,
             OptimizerMetricRedactionClass::protected_digest,
             OptimizerMetricSupportBundleClass::digest_only, true, true),
      Metric("route_equivalence_status", MetricType::state, MetricUnit::state,
             "route_executor", "route_equivalence",
             "optimizer_route_metrics.*;runtime_consumption_evidence.*",
             "wire;drivers;runtime_consumption_evidence.*",
             {"route_label", "required_driver_routes"},
             OptimizerMetricProducerState::live_maintained,
             OptimizerMetricFreshnessPolicy::per_route_execution,
             OptimizerMetricRetentionClass::support_bundle_window,
             OptimizerMetricRedactionClass::protected_digest,
             OptimizerMetricSupportBundleClass::digest_only, true, true),
      Metric("driver_visible_route_count", MetricType::gauge,
             MetricUnit::count, "route_executor", "driver_visible_parity",
             "optimizer_route_metrics.*;runtime_consumption_evidence.*",
             "wire;drivers;runtime_consumption_evidence.*",
             {"route_label", "required_driver_routes"},
             OptimizerMetricProducerState::live_maintained,
             OptimizerMetricFreshnessPolicy::per_route_execution,
             OptimizerMetricRetentionClass::support_bundle_window,
             OptimizerMetricRedactionClass::redacted_scope,
             OptimizerMetricSupportBundleClass::included_redacted, true, true),
      Metric("plan_cache_hit_miss", MetricType::counter, MetricUnit::count,
             "optimizer_plan_cache", "plan_cache_policy", "optimizer_plan_cache.*",
             "optimizer_plan_cache.*", {"plan_key_digest"},
             OptimizerMetricProducerState::owned_runtime_required,
             OptimizerMetricFreshnessPolicy::per_catalog_epoch,
             OptimizerMetricRetentionClass::plan_feedback_history),
      Metric("invalidation_reason", MetricType::state, MetricUnit::state,
             "catalog_stats_security_redaction", "cache_invalidation",
             "catalog;security;optimizer_statistics_lifecycle.*",
             "optimizer_plan_cache.*;optimizer_statistics_lifecycle.*",
             {"dependency_digest"},
             OptimizerMetricProducerState::owned_runtime_required,
             OptimizerMetricFreshnessPolicy::per_catalog_epoch,
             OptimizerMetricRetentionClass::security_audit_window,
             OptimizerMetricRedactionClass::protected_digest,
             OptimizerMetricSupportBundleClass::protected_summary),
  };
  return entries;
}

OptimizerMetricManifestValidation ValidateOptimizerEnterpriseMetricManifest() {
  OptimizerMetricManifestValidation validation;
  std::set<std::string> metric_families;
  std::set<std::string> registry_families;
  const auto& entries = OptimizerEnterpriseMetricManifest();

  if (entries.size() < 39) {
    validation.diagnostics.push_back("OEIC.METRIC_MANIFEST.TOO_FEW_METRICS");
  }

  for (const auto& entry : entries) {
    if (entry.metric_family.empty() || entry.registry_family.empty()) {
      validation.diagnostics.push_back("OEIC.METRIC_MANIFEST.EMPTY_FAMILY");
    }
    if (!metric_families.insert(entry.metric_family).second) {
      validation.diagnostics.push_back("OEIC.METRIC_MANIFEST.DUPLICATE_METRIC:" +
                                       entry.metric_family);
    }
    if (!registry_families.insert(entry.registry_family).second) {
      validation.diagnostics.push_back("OEIC.METRIC_MANIFEST.DUPLICATE_REGISTRY:" +
                                       entry.registry_family);
    }
    if (!Contains(entry.registry_family, "sb_optimizer_")) {
      validation.diagnostics.push_back("OEIC.METRIC_MANIFEST.BAD_REGISTRY_PREFIX:" +
                                       entry.metric_family);
    }
    if (entry.producer_owner.empty() || entry.consumer_owner.empty() ||
        entry.producer_anchor.empty() || entry.consumer_anchor.empty()) {
      validation.diagnostics.push_back("OEIC.METRIC_MANIFEST.OWNER_OR_ANCHOR_MISSING:" +
                                       entry.metric_family);
    }
    if (entry.required_evidence.empty()) {
      validation.diagnostics.push_back("OEIC.METRIC_MANIFEST.EVIDENCE_MISSING:" +
                                       entry.metric_family);
    }
    if (entry.producer_state == OptimizerMetricProducerState::cluster_external &&
        (entry.enterprise_route_consumable || entry.benchmark_clean_consumable)) {
      validation.diagnostics.push_back("OEIC.METRIC_MANIFEST.CLUSTER_METRIC_CONSUMABLE:" +
                                       entry.metric_family);
    }
    if (entry.benchmark_clean_consumable &&
        entry.producer_state != OptimizerMetricProducerState::live_maintained &&
        entry.producer_state !=
            OptimizerMetricProducerState::derived_from_registered_sources) {
      validation.diagnostics.push_back(
          "OEIC.METRIC_MANIFEST.BENCHMARK_CLEAN_WITHOUT_LIVE_PRODUCER:" +
          entry.metric_family);
    }
    if (entry.benchmark_clean_consumable &&
        entry.support_bundle_class ==
            OptimizerMetricSupportBundleClass::omitted_from_default_bundle) {
      validation.diagnostics.push_back(
          "OEIC.METRIC_MANIFEST.BENCHMARK_CLEAN_OMITTED_SUPPORT_BUNDLE:" +
          entry.metric_family);
    }
    if (entry.redaction_class == OptimizerMetricRedactionClass::public_aggregate &&
        (Contains(entry.metric_family, "hash") ||
         Contains(entry.metric_family, "digest"))) {
      validation.diagnostics.push_back("OEIC.METRIC_MANIFEST.UNSAFE_PUBLIC_DIGEST:" +
                                       entry.metric_family);
    }
  }

  const std::vector<std::string_view> required = {
      "operator_actual_rows", "operator_rows_examined", "operator_rows_filtered",
      "operator_loop_count", "operator_cpu_time", "memory_grant_bytes",
      "memory_peak_bytes", "spill_bytes", "spill_passes", "page_cache_hit_miss",
      "prefetch_usefulness", "page_count", "page_cache_dirty_pressure",
      "page_cache_pin_pressure", "writeback_pressure",
      "sequential_page_cost", "random_page_cost", "filespace_pressure",
      "mga_cleanup_debt", "mga_retained_dead_bytes",
      "mga_chain_depth", "mga_chain_scatter", "same_page_update_ratio",
      "index_backlog_entries", "index_selectivity",
      "index_false_positive_ratio", "index_recheck_count",
      "commit_fence_pressure", "btree_depth", "btree_leaf_pages",
      "hash_collision_depth", "hash_overflow_depth", "bitmap_density",
      "candidate_set_cardinality", "candidate_set_density",
      "candidate_set_recheck_ratio",
      "bloom_observed_fpr", "zone_prune_selectivity", "text_posting_length",
      "text_blockmax_skips", "vector_recall_observed", "vector_rerank_count",
      "vector_tombstone_ratio", "graph_frontier_width",
      "graph_adjacency_degree", "document_path_selectivity",
      "specialized_exact_recheck_rows", "specialized_false_positive_ratio",
      "time_series_bucket_count", "time_series_rollup_selectivity",
      "route_plan_hash", "route_result_hash", "explain_digest",
      "route_equivalence_status", "driver_visible_route_count",
      "plan_cache_hit_miss",
      "invalidation_reason"};
  for (const auto required_metric : required) {
    if (metric_families.find(std::string(required_metric)) ==
        metric_families.end()) {
      validation.diagnostics.push_back("OEIC.METRIC_MANIFEST.REQUIRED_MISSING:" +
                                       std::string(required_metric));
    }
  }

  validation.ok = validation.diagnostics.empty();
  return validation;
}

metrics::MetricValidationResult EnsureOptimizerEnterpriseMetricDescriptors(
    metrics::MetricRegistry* registry) {
  auto& target = registry == nullptr ? metrics::DefaultMetricRegistry() : *registry;
  for (const auto& entry : OptimizerEnterpriseMetricManifest()) {
    if (target.FindDescriptor(entry.registry_family) != nullptr) {
      continue;
    }
    const auto result = target.RegisterDescriptor(DescriptorForEntry(entry));
    if (!result.ok) {
      return result;
    }
  }
  return metrics::MetricOk();
}

}  // namespace scratchbird::engine::optimizer

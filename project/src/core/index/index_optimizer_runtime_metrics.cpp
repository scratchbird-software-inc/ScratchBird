// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "index_optimizer_runtime_metrics.hpp"

#include <utility>

namespace scratchbird::core::index {
namespace {

namespace metrics = scratchbird::core::metrics;

using metrics::MetricDescriptor;
using metrics::MetricLabelDescriptor;
using metrics::MetricReadiness;
using metrics::MetricType;
using metrics::MetricUnit;
using metrics::MetricValidationResult;

MetricDescriptor Descriptor(std::string family,
                            MetricType type,
                            MetricUnit unit,
                            std::string producer_owner,
                            std::string help) {
  MetricDescriptor descriptor;
  descriptor.family = std::move(family);
  descriptor.type = type;
  descriptor.unit = unit;
  descriptor.namespace_path = "sys.metrics.optimizer.enterprise";
  descriptor.help = std::move(help);
  descriptor.producer_owner = std::move(producer_owner);
  descriptor.security_family = "OPTIMIZER_METRICS";
  descriptor.readiness = MetricReadiness::implemented;
  descriptor.labels = {MetricLabelDescriptor{"scope_uuid", true, false},
                       MetricLabelDescriptor{"route_label", true, false},
                       MetricLabelDescriptor{"plan_node_id", false, false},
                       MetricLabelDescriptor{"metric_family", true, false},
                       MetricLabelDescriptor{"result", false, false},
                       MetricLabelDescriptor{"source_generation", true, false},
                       MetricLabelDescriptor{"evidence_digest", true, true}};
  return descriptor;
}

MetricValidationResult RegisterIfMissing(metrics::MetricRegistry* registry,
                                         MetricDescriptor descriptor) {
  auto& target = registry == nullptr ? metrics::DefaultMetricRegistry() : *registry;
  if (target.FindDescriptor(descriptor.family) != nullptr) {
    return metrics::MetricOk();
  }
  return target.RegisterDescriptor(std::move(descriptor));
}

bool UnsafeAuthority(const IndexOptimizerRuntimeMetricAuthority& authority) {
  return authority.parser_or_reference_authority ||
         authority.client_finality_or_visibility_authority ||
         authority.metric_visibility_or_finality_authority ||
         authority.metric_recovery_authority ||
         authority.wal_or_redo_authority ||
         authority.cluster_authority ||
         authority.benchmark_authority;
}

void AddEvidence(IndexOptimizerRuntimeMetricPublishResult* result,
                 std::string evidence) {
  if (result != nullptr) {
    result->evidence.push_back(std::move(evidence));
  }
}

IndexOptimizerRuntimeMetricPublishResult Refuse(
    const IndexOptimizerRuntimeMetricSample& sample,
    std::string code,
    std::string detail) {
  IndexOptimizerRuntimeMetricPublishResult result;
  result.ok = false;
  result.diagnostic_code = std::move(code);
  result.detail = std::move(detail);
  AddEvidence(&result, "OEIC_INDEX_FAMILY_OPTIMIZER_METRICS");
  AddEvidence(&result, "optimizer.index_metrics.fail_closed=true");
  AddEvidence(&result, "optimizer.index_metrics.index_uuid=" +
                           sample.index_uuid);
  AddEvidence(&result, "optimizer.index_metrics.index_family=" +
                           sample.index_family);
  AddEvidence(&result, "optimizer.index_metrics.refused=" +
                           result.diagnostic_code);
  return result;
}

bool EmptyRequiredField(const IndexOptimizerRuntimeMetricSample& sample,
                        std::string* field) {
  if (sample.scope_uuid.empty()) {
    if (field != nullptr) *field = "scope_uuid";
    return true;
  }
  if (sample.route_label.empty()) {
    if (field != nullptr) *field = "route_label";
    return true;
  }
  if (sample.index_uuid.empty()) {
    if (field != nullptr) *field = "index_uuid";
    return true;
  }
  if (sample.index_family.empty()) {
    if (field != nullptr) *field = "index_family";
    return true;
  }
  if (sample.index_generation.empty()) {
    if (field != nullptr) *field = "index_generation";
    return true;
  }
  if (sample.evidence_digest.empty()) {
    if (field != nullptr) *field = "evidence_digest";
    return true;
  }
  return false;
}

bool RatioInvalid(const std::optional<double>& value) {
  return value.has_value() && (*value < 0.0 || *value > 1.0);
}

metrics::MetricLabelSet LabelsFor(
    const IndexOptimizerRuntimeMetricSample& sample,
    std::string metric_family) {
  metrics::MetricLabelSet labels = {
      {"scope_uuid", sample.scope_uuid},
      {"route_label", sample.route_label},
      {"metric_family", std::move(metric_family)},
      {"source_generation", std::to_string(sample.source_generation)},
      {"evidence_digest", sample.evidence_digest}};
  if (!sample.plan_node_id.empty()) {
    labels.push_back({"plan_node_id", sample.plan_node_id});
  }
  return labels;
}

void Push(IndexOptimizerRuntimeMetricPublishResult* result,
          MetricValidationResult metric_result) {
  if (!metric_result.ok && result != nullptr) {
    result->ok = false;
    if (result->diagnostic_code.empty()) {
      result->diagnostic_code = metric_result.diagnostic_code;
      result->detail = metric_result.detail;
    }
  }
  if (result != nullptr) {
    result->metric_results.push_back(std::move(metric_result));
  }
}

void Gauge(IndexOptimizerRuntimeMetricPublishResult* result,
           const IndexOptimizerRuntimeMetricSample& sample,
           const std::string& family,
           const std::string& metric_family,
           double value,
           const std::string& producer_owner) {
  Push(result,
       metrics::DefaultMetricRegistry().SetGauge(
           family, LabelsFor(sample, metric_family), value, producer_owner));
}

void Counter(IndexOptimizerRuntimeMetricPublishResult* result,
             const IndexOptimizerRuntimeMetricSample& sample,
             const std::string& family,
             const std::string& metric_family,
             std::uint64_t value,
             const std::string& producer_owner) {
  if (value == 0) {
    return;
  }
  Push(result,
       metrics::DefaultMetricRegistry().IncrementCounter(
           family, LabelsFor(sample, metric_family), static_cast<double>(value),
           producer_owner));
}

template <typename T>
bool Present(const std::optional<T>& value) {
  return value.has_value();
}

}  // namespace

metrics::MetricValidationResult EnsureIndexOptimizerRuntimeMetricDescriptors(
    metrics::MetricRegistry* registry) {
  const MetricDescriptor descriptors[] = {
      Descriptor("sb_optimizer_index_selectivity", MetricType::gauge,
                 MetricUnit::ratio, "index_runtime",
                 "Optimizer-visible index selectivity observation."),
      Descriptor("sb_optimizer_index_false_positive_ratio", MetricType::gauge,
                 MetricUnit::ratio, "index_runtime",
                 "Optimizer-visible index false-positive ratio."),
      Descriptor("sb_optimizer_index_recheck_count", MetricType::counter,
                 MetricUnit::count, "index_runtime",
                 "Optimizer-visible exact index recheck count."),
      Descriptor("sb_optimizer_index_backlog_entries", MetricType::gauge,
                 MetricUnit::count, "index_runtime",
                 "Optimizer-visible index maintenance backlog entries."),
      Descriptor("sb_optimizer_btree_depth", MetricType::gauge,
                 MetricUnit::count, "index_runtime",
                 "Optimizer-visible B-tree depth."),
      Descriptor("sb_optimizer_btree_leaf_pages", MetricType::gauge,
                 MetricUnit::count, "index_runtime",
                 "Optimizer-visible B-tree leaf pages."),
      Descriptor("sb_optimizer_hash_collision_depth", MetricType::gauge,
                 MetricUnit::count, "index_runtime",
                 "Optimizer-visible hash collision depth."),
      Descriptor("sb_optimizer_hash_overflow_depth", MetricType::gauge,
                 MetricUnit::count, "index_runtime",
                 "Optimizer-visible hash overflow depth."),
      Descriptor("sb_optimizer_bitmap_density", MetricType::gauge,
                 MetricUnit::ratio, "index_runtime",
                 "Optimizer-visible bitmap density."),
      Descriptor("sb_optimizer_bloom_observed_fpr", MetricType::gauge,
                 MetricUnit::ratio, "index_runtime",
                 "Optimizer-visible Bloom observed false-positive rate."),
      Descriptor("sb_optimizer_zone_prune_selectivity", MetricType::gauge,
                 MetricUnit::ratio, "index_runtime",
                 "Optimizer-visible zone prune selectivity."),
      Descriptor("sb_optimizer_text_posting_length", MetricType::gauge,
                 MetricUnit::count, "search_runtime",
                 "Optimizer-visible text posting length."),
      Descriptor("sb_optimizer_text_blockmax_skips", MetricType::counter,
                 MetricUnit::count, "search_runtime",
                 "Optimizer-visible text block-max skip count."),
      Descriptor("sb_optimizer_vector_recall_observed", MetricType::gauge,
                 MetricUnit::ratio, "vector_runtime",
                 "Optimizer-visible vector recall observation."),
      Descriptor("sb_optimizer_vector_rerank_count", MetricType::gauge,
                 MetricUnit::count, "vector_runtime",
                 "Optimizer-visible exact vector rerank count."),
      Descriptor("sb_optimizer_vector_tombstone_ratio", MetricType::gauge,
                 MetricUnit::ratio, "vector_runtime",
                 "Optimizer-visible vector tombstone ratio."),
      Descriptor("sb_optimizer_graph_frontier_width", MetricType::gauge,
                 MetricUnit::count, "graph_runtime",
                 "Optimizer-visible graph frontier width."),
      Descriptor("sb_optimizer_graph_adjacency_degree", MetricType::gauge,
                 MetricUnit::count, "graph_runtime",
                 "Optimizer-visible graph adjacency degree."),
      Descriptor("sb_optimizer_document_path_selectivity", MetricType::gauge,
                 MetricUnit::ratio, "document_provider",
                 "Optimizer-visible document-path selectivity.")};
  for (const auto& descriptor : descriptors) {
    const auto result = RegisterIfMissing(registry, descriptor);
    if (!result.ok) {
      return result;
    }
  }
  return metrics::MetricOk();
}

IndexOptimizerRuntimeMetricPublishResult PublishIndexOptimizerRuntimeMetrics(
    const IndexOptimizerRuntimeMetricSample& sample) {
  std::string missing_field;
  if (EmptyRequiredField(sample, &missing_field)) {
    return Refuse(sample,
                  "SB_OPTIMIZER_INDEX_METRICS.MISSING_SCOPE",
                  "optimizer.index_metrics.required_field_missing:" +
                      missing_field);
  }
  if (sample.source_generation == 0) {
    return Refuse(sample,
                  "SB_OPTIMIZER_INDEX_METRICS.GENERATION_REQUIRED",
                  "optimizer.index_metrics.generation_required");
  }
  if (sample.freshness_microseconds > sample.max_freshness_microseconds) {
    return Refuse(sample,
                  "SB_OPTIMIZER_INDEX_METRICS.STALE",
                  "optimizer.index_metrics.stale");
  }
  if (!sample.authority.index_descriptor_authoritative ||
      !sample.authority.index_generation_authoritative ||
      !sample.authority.family_provider_authoritative ||
      !sample.authority.engine_scope_bound ||
      !sample.authority.exact_recheck_preserved) {
    return Refuse(sample,
                  "SB_OPTIMIZER_INDEX_METRICS.INDEX_AUTHORITY_REQUIRED",
                  "optimizer.index_metrics.index_authority_required");
  }
  if (UnsafeAuthority(sample.authority)) {
    return Refuse(sample,
                  "SB_OPTIMIZER_INDEX_METRICS.UNSAFE_AUTHORITY",
                  "optimizer.index_metrics.unsafe_authority");
  }
  if (Present(sample.index_backlog_entries) &&
      !sample.authority.maintenance_runtime_authoritative) {
    return Refuse(sample,
                  "SB_OPTIMIZER_INDEX_METRICS.MAINTENANCE_AUTHORITY_REQUIRED",
                  "optimizer.index_metrics.maintenance_authority_required");
  }
  if (Present(sample.bitmap_density) &&
      !sample.authority.candidate_set_runtime_authoritative) {
    return Refuse(sample,
                  "SB_OPTIMIZER_INDEX_METRICS.CANDIDATE_SET_AUTHORITY_REQUIRED",
                  "optimizer.index_metrics.candidate_set_authority_required");
  }
  if ((Present(sample.text_posting_length) ||
       Present(sample.text_blockmax_skips)) &&
      !sample.authority.search_runtime_authoritative) {
    return Refuse(sample,
                  "SB_OPTIMIZER_INDEX_METRICS.SEARCH_AUTHORITY_REQUIRED",
                  "optimizer.index_metrics.search_authority_required");
  }
  if ((Present(sample.vector_recall_observed) ||
       Present(sample.vector_rerank_count) ||
       Present(sample.vector_tombstone_ratio)) &&
      (!sample.authority.vector_runtime_authoritative ||
       !sample.authority.exact_rerank_preserved)) {
    return Refuse(sample,
                  "SB_OPTIMIZER_INDEX_METRICS.VECTOR_AUTHORITY_REQUIRED",
                  "optimizer.index_metrics.vector_authority_required");
  }
  if ((Present(sample.graph_frontier_width) ||
       Present(sample.graph_adjacency_degree)) &&
      !sample.authority.graph_runtime_authoritative) {
    return Refuse(sample,
                  "SB_OPTIMIZER_INDEX_METRICS.GRAPH_AUTHORITY_REQUIRED",
                  "optimizer.index_metrics.graph_authority_required");
  }
  if (Present(sample.document_path_selectivity) &&
      !sample.authority.document_path_runtime_authoritative) {
    return Refuse(sample,
                  "SB_OPTIMIZER_INDEX_METRICS.DOCUMENT_AUTHORITY_REQUIRED",
                  "optimizer.index_metrics.document_authority_required");
  }
  if (RatioInvalid(sample.index_selectivity) ||
      RatioInvalid(sample.index_false_positive_ratio) ||
      RatioInvalid(sample.bitmap_density) ||
      RatioInvalid(sample.bloom_observed_fpr) ||
      RatioInvalid(sample.zone_prune_selectivity) ||
      RatioInvalid(sample.vector_recall_observed) ||
      RatioInvalid(sample.vector_tombstone_ratio) ||
      RatioInvalid(sample.document_path_selectivity)) {
    return Refuse(sample,
                  "SB_OPTIMIZER_INDEX_METRICS.RATIO_INVALID",
                  "optimizer.index_metrics.ratio_invalid");
  }

  IndexOptimizerRuntimeMetricPublishResult result;
  result.ok = true;
  result.diagnostic_code = "SB_OPTIMIZER_INDEX_METRICS.OK";
  AddEvidence(&result, "OEIC_INDEX_FAMILY_OPTIMIZER_METRICS");
  AddEvidence(&result, "optimizer.index_metrics.fail_closed=false");
  AddEvidence(&result, "optimizer.index_metrics.advisory_only=true");
  AddEvidence(&result, "optimizer.index_metrics.finality_authority=false");
  AddEvidence(&result, "optimizer.index_metrics.visibility_authority=false");
  AddEvidence(&result, "optimizer.index_metrics.security_authority=false");
  AddEvidence(&result, "optimizer.index_metrics.recovery_authority=false");
  AddEvidence(&result, "optimizer.index_metrics.wal_redo_authority=false");
  AddEvidence(&result, "optimizer.index_metrics.cluster_authority=false");

  Push(&result, EnsureIndexOptimizerRuntimeMetricDescriptors());
  if (sample.index_selectivity) {
    Gauge(&result, sample, "sb_optimizer_index_selectivity",
          "index_selectivity", *sample.index_selectivity, "index_runtime");
  }
  if (sample.index_false_positive_ratio) {
    Gauge(&result, sample, "sb_optimizer_index_false_positive_ratio",
          "index_false_positive_ratio", *sample.index_false_positive_ratio,
          "index_runtime");
  }
  if (sample.index_recheck_count) {
    Counter(&result, sample, "sb_optimizer_index_recheck_count",
            "index_recheck_count", *sample.index_recheck_count,
            "index_runtime");
  }
  if (sample.index_backlog_entries) {
    Gauge(&result, sample, "sb_optimizer_index_backlog_entries",
          "index_backlog_entries",
          static_cast<double>(*sample.index_backlog_entries), "index_runtime");
  }
  if (sample.btree_depth) {
    Gauge(&result, sample, "sb_optimizer_btree_depth", "btree_depth",
          *sample.btree_depth, "index_runtime");
  }
  if (sample.btree_leaf_pages) {
    Gauge(&result, sample, "sb_optimizer_btree_leaf_pages",
          "btree_leaf_pages", *sample.btree_leaf_pages, "index_runtime");
  }
  if (sample.hash_collision_depth) {
    Gauge(&result, sample, "sb_optimizer_hash_collision_depth",
          "hash_collision_depth", *sample.hash_collision_depth,
          "index_runtime");
  }
  if (sample.hash_overflow_depth) {
    Gauge(&result, sample, "sb_optimizer_hash_overflow_depth",
          "hash_overflow_depth", *sample.hash_overflow_depth,
          "index_runtime");
  }
  if (sample.bitmap_density) {
    Gauge(&result, sample, "sb_optimizer_bitmap_density", "bitmap_density",
          *sample.bitmap_density, "index_runtime");
  }
  if (sample.bloom_observed_fpr) {
    Gauge(&result, sample, "sb_optimizer_bloom_observed_fpr",
          "bloom_observed_fpr", *sample.bloom_observed_fpr, "index_runtime");
  }
  if (sample.zone_prune_selectivity) {
    Gauge(&result, sample, "sb_optimizer_zone_prune_selectivity",
          "zone_prune_selectivity", *sample.zone_prune_selectivity,
          "index_runtime");
  }
  if (sample.text_posting_length) {
    Gauge(&result, sample, "sb_optimizer_text_posting_length",
          "text_posting_length", *sample.text_posting_length,
          "search_runtime");
  }
  if (sample.text_blockmax_skips) {
    Counter(&result, sample, "sb_optimizer_text_blockmax_skips",
            "text_blockmax_skips", *sample.text_blockmax_skips,
            "search_runtime");
  }
  if (sample.vector_recall_observed) {
    Gauge(&result, sample, "sb_optimizer_vector_recall_observed",
          "vector_recall_observed", *sample.vector_recall_observed,
          "vector_runtime");
  }
  if (sample.vector_rerank_count) {
    Gauge(&result, sample, "sb_optimizer_vector_rerank_count",
          "vector_rerank_count", *sample.vector_rerank_count,
          "vector_runtime");
  }
  if (sample.vector_tombstone_ratio) {
    Gauge(&result, sample, "sb_optimizer_vector_tombstone_ratio",
          "vector_tombstone_ratio", *sample.vector_tombstone_ratio,
          "vector_runtime");
  }
  if (sample.graph_frontier_width) {
    Gauge(&result, sample, "sb_optimizer_graph_frontier_width",
          "graph_frontier_width", *sample.graph_frontier_width,
          "graph_runtime");
  }
  if (sample.graph_adjacency_degree) {
    Gauge(&result, sample, "sb_optimizer_graph_adjacency_degree",
          "graph_adjacency_degree", *sample.graph_adjacency_degree,
          "graph_runtime");
  }
  if (sample.document_path_selectivity) {
    Gauge(&result, sample, "sb_optimizer_document_path_selectivity",
          "document_path_selectivity", *sample.document_path_selectivity,
          "document_provider");
  }

  if (!result.ok &&
      result.diagnostic_code == "SB_OPTIMIZER_INDEX_METRICS.OK") {
    result.diagnostic_code =
        "SB_OPTIMIZER_INDEX_METRICS.METRIC_PUBLISH_FAILED";
    result.detail = "optimizer.index_metrics.metric_publish_failed";
  }
  return result;
}

}  // namespace scratchbird::core::index

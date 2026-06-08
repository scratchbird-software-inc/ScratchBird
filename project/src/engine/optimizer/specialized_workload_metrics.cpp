// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "specialized_workload_metrics.hpp"

#include <utility>

namespace scratchbird::engine::optimizer {
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

bool UnsafeAuthority(const SpecializedWorkloadMetricAuthority& authority) {
  return authority.parser_or_donor_authority ||
         authority.client_finality_or_visibility_authority ||
         authority.metric_visibility_or_finality_authority ||
         authority.metric_recovery_authority ||
         authority.wal_or_redo_authority ||
         authority.cluster_authority ||
         authority.benchmark_authority;
}

void AddEvidence(SpecializedWorkloadMetricPublishResult* result,
                 std::string evidence) {
  if (result != nullptr) {
    result->evidence.push_back(std::move(evidence));
  }
}

SpecializedWorkloadMetricPublishResult Refuse(
    const SpecializedWorkloadMetricSample& sample,
    std::string code,
    std::string detail) {
  SpecializedWorkloadMetricPublishResult result;
  result.ok = false;
  result.diagnostic_code = std::move(code);
  result.detail = std::move(detail);
  AddEvidence(&result, "OEIC_SPECIALIZED_WORKLOAD_OPTIMIZER_METRICS");
  AddEvidence(&result, "optimizer.specialized_metrics.fail_closed=true");
  AddEvidence(&result, "optimizer.specialized_metrics.workload_family=" +
                           sample.workload_family);
  AddEvidence(&result, "optimizer.specialized_metrics.provider_id=" +
                           sample.provider_id);
  AddEvidence(&result, "optimizer.specialized_metrics.refused=" +
                           result.diagnostic_code);
  return result;
}

bool EmptyRequiredField(const SpecializedWorkloadMetricSample& sample,
                        std::string* field) {
  if (sample.scope_uuid.empty()) {
    if (field != nullptr) *field = "scope_uuid";
    return true;
  }
  if (sample.route_label.empty()) {
    if (field != nullptr) *field = "route_label";
    return true;
  }
  if (sample.workload_family.empty()) {
    if (field != nullptr) *field = "workload_family";
    return true;
  }
  if (sample.provider_id.empty()) {
    if (field != nullptr) *field = "provider_id";
    return true;
  }
  if (sample.index_generation.empty()) {
    if (field != nullptr) *field = "index_generation";
    return true;
  }
  if (sample.result_contract_hash.empty()) {
    if (field != nullptr) *field = "result_contract_hash";
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

template <typename T>
bool Present(const std::optional<T>& value) {
  return value.has_value();
}

metrics::MetricLabelSet LabelsFor(const SpecializedWorkloadMetricSample& sample,
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

void Push(SpecializedWorkloadMetricPublishResult* result,
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

void Gauge(SpecializedWorkloadMetricPublishResult* result,
           const SpecializedWorkloadMetricSample& sample,
           const std::string& family,
           const std::string& metric_family,
           double value,
           const std::string& producer_owner) {
  Push(result,
       metrics::DefaultMetricRegistry().SetGauge(
           family, LabelsFor(sample, metric_family), value, producer_owner));
}

void Counter(SpecializedWorkloadMetricPublishResult* result,
             const SpecializedWorkloadMetricSample& sample,
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

}  // namespace

metrics::MetricValidationResult EnsureSpecializedWorkloadMetricDescriptors(
    metrics::MetricRegistry* registry) {
  const MetricDescriptor descriptors[] = {
      Descriptor("sb_optimizer_candidate_set_cardinality", MetricType::gauge,
                 MetricUnit::count, "candidate_set_runtime",
                 "Optimizer-visible specialized route candidate-set cardinality."),
      Descriptor("sb_optimizer_candidate_set_density", MetricType::gauge,
                 MetricUnit::ratio, "candidate_set_runtime",
                 "Optimizer-visible specialized route candidate-set density."),
      Descriptor("sb_optimizer_candidate_set_recheck_ratio", MetricType::gauge,
                 MetricUnit::ratio, "candidate_set_runtime",
                 "Optimizer-visible specialized route exact-recheck ratio."),
      Descriptor("sb_optimizer_specialized_exact_recheck_rows", MetricType::gauge,
                 MetricUnit::count, "specialized_runtime",
                 "Optimizer-visible specialized route exact recheck rows."),
      Descriptor("sb_optimizer_specialized_false_positive_ratio", MetricType::gauge,
                 MetricUnit::ratio, "specialized_runtime",
                 "Optimizer-visible specialized route false-positive ratio."),
      Descriptor("sb_optimizer_document_path_selectivity", MetricType::gauge,
                 MetricUnit::ratio, "document_provider",
                 "Optimizer-visible document path route selectivity."),
      Descriptor("sb_optimizer_text_posting_length", MetricType::gauge,
                 MetricUnit::count, "search_runtime",
                 "Optimizer-visible text posting length."),
      Descriptor("sb_optimizer_text_blockmax_skips", MetricType::counter,
                 MetricUnit::count, "search_runtime",
                 "Optimizer-visible text block-max skips."),
      Descriptor("sb_optimizer_vector_recall_observed", MetricType::gauge,
                 MetricUnit::ratio, "vector_runtime",
                 "Optimizer-visible vector recall observation."),
      Descriptor("sb_optimizer_vector_rerank_count", MetricType::gauge,
                 MetricUnit::count, "vector_runtime",
                 "Optimizer-visible vector exact rerank count."),
      Descriptor("sb_optimizer_graph_frontier_width", MetricType::gauge,
                 MetricUnit::count, "graph_runtime",
                 "Optimizer-visible graph frontier width."),
      Descriptor("sb_optimizer_graph_adjacency_degree", MetricType::gauge,
                 MetricUnit::count, "graph_runtime",
                 "Optimizer-visible graph adjacency degree."),
      Descriptor("sb_optimizer_time_series_bucket_count", MetricType::gauge,
                 MetricUnit::count, "time_series_runtime",
                 "Optimizer-visible time-series bucket count."),
      Descriptor("sb_optimizer_time_series_rollup_selectivity", MetricType::gauge,
                 MetricUnit::ratio, "time_series_runtime",
                 "Optimizer-visible time-series rollup selectivity.")};
  for (const auto& descriptor : descriptors) {
    const auto result = RegisterIfMissing(registry, descriptor);
    if (!result.ok) {
      return result;
    }
  }
  return metrics::MetricOk();
}

SpecializedWorkloadMetricPublishResult PublishSpecializedWorkloadMetrics(
    const SpecializedWorkloadMetricSample& sample) {
  std::string missing_field;
  if (EmptyRequiredField(sample, &missing_field)) {
    return Refuse(sample,
                  "SB_OPTIMIZER_SPECIALIZED_METRICS.MISSING_SCOPE",
                  "optimizer.specialized_metrics.required_field_missing:" +
                      missing_field);
  }
  if (sample.source_generation == 0) {
    return Refuse(sample,
                  "SB_OPTIMIZER_SPECIALIZED_METRICS.GENERATION_REQUIRED",
                  "optimizer.specialized_metrics.generation_required");
  }
  if (sample.freshness_microseconds > sample.max_freshness_microseconds) {
    return Refuse(sample,
                  "SB_OPTIMIZER_SPECIALIZED_METRICS.STALE",
                  "optimizer.specialized_metrics.stale");
  }
  if (!sample.authority.provider_contract_authoritative ||
      !sample.authority.route_runtime_authoritative ||
      !sample.authority.descriptor_visibility_proof_present ||
      !sample.authority.index_generation_proof_present ||
      !sample.authority.engine_scope_bound ||
      !sample.authority.exact_recheck_preserved ||
      !sample.authority.mga_recheck_preserved ||
      !sample.authority.security_recheck_preserved) {
    return Refuse(sample,
                  "SB_OPTIMIZER_SPECIALIZED_METRICS.ROUTE_AUTHORITY_REQUIRED",
                  "optimizer.specialized_metrics.route_authority_required");
  }
  if (UnsafeAuthority(sample.authority)) {
    return Refuse(sample,
                  "SB_OPTIMIZER_SPECIALIZED_METRICS.UNSAFE_AUTHORITY",
                  "optimizer.specialized_metrics.unsafe_authority");
  }
  if ((Present(sample.candidate_set_cardinality) ||
       Present(sample.candidate_set_density) ||
       Present(sample.candidate_set_recheck_ratio)) &&
      !sample.authority.candidate_set_runtime_authoritative) {
    return Refuse(sample,
                  "SB_OPTIMIZER_SPECIALIZED_METRICS.CANDIDATE_SET_AUTHORITY_REQUIRED",
                  "optimizer.specialized_metrics.candidate_set_authority_required");
  }
  if (Present(sample.document_path_selectivity) &&
      !sample.authority.document_runtime_authoritative) {
    return Refuse(sample,
                  "SB_OPTIMIZER_SPECIALIZED_METRICS.DOCUMENT_AUTHORITY_REQUIRED",
                  "optimizer.specialized_metrics.document_authority_required");
  }
  if ((Present(sample.text_posting_length) ||
       Present(sample.text_blockmax_skips)) &&
      !sample.authority.search_runtime_authoritative) {
    return Refuse(sample,
                  "SB_OPTIMIZER_SPECIALIZED_METRICS.SEARCH_AUTHORITY_REQUIRED",
                  "optimizer.specialized_metrics.search_authority_required");
  }
  if ((Present(sample.vector_recall_observed) ||
       Present(sample.vector_rerank_count)) &&
      !sample.authority.vector_runtime_authoritative) {
    return Refuse(sample,
                  "SB_OPTIMIZER_SPECIALIZED_METRICS.VECTOR_AUTHORITY_REQUIRED",
                  "optimizer.specialized_metrics.vector_authority_required");
  }
  if ((Present(sample.graph_frontier_width) ||
       Present(sample.graph_adjacency_degree)) &&
      !sample.authority.graph_runtime_authoritative) {
    return Refuse(sample,
                  "SB_OPTIMIZER_SPECIALIZED_METRICS.GRAPH_AUTHORITY_REQUIRED",
                  "optimizer.specialized_metrics.graph_authority_required");
  }
  if ((Present(sample.time_series_bucket_count) ||
       Present(sample.time_series_rollup_selectivity)) &&
      !sample.authority.time_series_runtime_authoritative) {
    return Refuse(sample,
                  "SB_OPTIMIZER_SPECIALIZED_METRICS.TIME_SERIES_AUTHORITY_REQUIRED",
                  "optimizer.specialized_metrics.time_series_authority_required");
  }
  if (RatioInvalid(sample.candidate_set_density) ||
      RatioInvalid(sample.candidate_set_recheck_ratio) ||
      RatioInvalid(sample.specialized_false_positive_ratio) ||
      RatioInvalid(sample.document_path_selectivity) ||
      RatioInvalid(sample.vector_recall_observed) ||
      RatioInvalid(sample.time_series_rollup_selectivity)) {
    return Refuse(sample,
                  "SB_OPTIMIZER_SPECIALIZED_METRICS.RATIO_INVALID",
                  "optimizer.specialized_metrics.ratio_invalid");
  }

  SpecializedWorkloadMetricPublishResult result;
  result.ok = true;
  result.diagnostic_code = "SB_OPTIMIZER_SPECIALIZED_METRICS.OK";
  AddEvidence(&result, "OEIC_SPECIALIZED_WORKLOAD_OPTIMIZER_METRICS");
  AddEvidence(&result, "optimizer.specialized_metrics.fail_closed=false");
  AddEvidence(&result, "optimizer.specialized_metrics.advisory_only=true");
  AddEvidence(&result, "optimizer.specialized_metrics.finality_authority=false");
  AddEvidence(&result, "optimizer.specialized_metrics.visibility_authority=false");
  AddEvidence(&result, "optimizer.specialized_metrics.security_authority=false");
  AddEvidence(&result, "optimizer.specialized_metrics.recovery_authority=false");
  AddEvidence(&result, "optimizer.specialized_metrics.wal_redo_authority=false");
  AddEvidence(&result, "optimizer.specialized_metrics.cluster_authority=false");

  Push(&result, EnsureSpecializedWorkloadMetricDescriptors());
  if (sample.candidate_set_cardinality) {
    Gauge(&result, sample, "sb_optimizer_candidate_set_cardinality",
          "candidate_set_cardinality", *sample.candidate_set_cardinality,
          "candidate_set_runtime");
  }
  if (sample.candidate_set_density) {
    Gauge(&result, sample, "sb_optimizer_candidate_set_density",
          "candidate_set_density", *sample.candidate_set_density,
          "candidate_set_runtime");
  }
  if (sample.candidate_set_recheck_ratio) {
    Gauge(&result, sample, "sb_optimizer_candidate_set_recheck_ratio",
          "candidate_set_recheck_ratio", *sample.candidate_set_recheck_ratio,
          "candidate_set_runtime");
  }
  if (sample.specialized_exact_recheck_rows) {
    Gauge(&result, sample, "sb_optimizer_specialized_exact_recheck_rows",
          "specialized_exact_recheck_rows",
          *sample.specialized_exact_recheck_rows, "specialized_runtime");
  }
  if (sample.specialized_false_positive_ratio) {
    Gauge(&result, sample, "sb_optimizer_specialized_false_positive_ratio",
          "specialized_false_positive_ratio",
          *sample.specialized_false_positive_ratio, "specialized_runtime");
  }
  if (sample.document_path_selectivity) {
    Gauge(&result, sample, "sb_optimizer_document_path_selectivity",
          "document_path_selectivity", *sample.document_path_selectivity,
          "document_provider");
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
  if (sample.time_series_bucket_count) {
    Gauge(&result, sample, "sb_optimizer_time_series_bucket_count",
          "time_series_bucket_count", *sample.time_series_bucket_count,
          "time_series_runtime");
  }
  if (sample.time_series_rollup_selectivity) {
    Gauge(&result, sample, "sb_optimizer_time_series_rollup_selectivity",
          "time_series_rollup_selectivity",
          *sample.time_series_rollup_selectivity, "time_series_runtime");
  }

  if (!result.ok &&
      result.diagnostic_code == "SB_OPTIMIZER_SPECIALIZED_METRICS.OK") {
    result.diagnostic_code =
        "SB_OPTIMIZER_SPECIALIZED_METRICS.METRIC_PUBLISH_FAILED";
    result.detail = "optimizer.specialized_metrics.metric_publish_failed";
  }
  return result;
}

}  // namespace scratchbird::engine::optimizer

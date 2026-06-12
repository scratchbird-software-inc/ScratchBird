// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "optimizer_vector_ann_costing_enterprise.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <string>
#include <utility>

namespace scratchbird::engine::optimizer {
namespace {

void Add(std::vector<std::string>* evidence, std::string value) {
  evidence->push_back(std::move(value));
}

EnterpriseVectorAnnCostingResult Refuse(const EnterpriseVectorAnnCostingRequest& request,
                                        std::string diagnostic_code,
                                        std::string evidence) {
  EnterpriseVectorAnnCostingResult result;
  result.accepted = false;
  result.selectable = false;
  result.profile = request.profile;
  result.diagnostic_code = std::move(diagnostic_code);
  result.cost.selectable = false;
  result.cost.confidence = CostConfidence::kRejected;
  result.cost.rejection_reason = result.diagnostic_code;
  result.cost.reason = "enterprise_vector_ann_cost_refused";
  Add(&result.evidence, "OEIC_VECTOR_ANN_RECALL_COSTING");
  Add(&result.evidence, std::move(evidence));
  Add(&result.evidence, "vector_ann.finality_authority=false");
  Add(&result.evidence, "vector_ann.visibility_authority=false");
  Add(&result.evidence, "vector_ann.security_authority=false");
  return result;
}

bool ApproximateProfile(EnterpriseVectorIndexProfile profile) {
  return profile == EnterpriseVectorIndexProfile::kHnsw ||
         profile == EnterpriseVectorIndexProfile::kIvfFlat ||
         profile == EnterpriseVectorIndexProfile::kIvfPq ||
         profile == EnterpriseVectorIndexProfile::kIvfSq8;
}

bool RatioInvalid(double value) {
  return value < 0.0 || value > 1.0 || std::isnan(value);
}

bool UnsafeMetricAuthority(const EnterpriseVectorRuntimeMetric& metric) {
  return metric.parser_or_reference_authority ||
         metric.client_authority ||
         metric.metric_finality_or_visibility_authority ||
         metric.provider_finality_or_visibility_authority ||
         metric.recovery_or_wal_authority ||
         metric.cluster_route_or_metric_projection;
}

bool MetricComplete(const EnterpriseVectorRuntimeMetric& metric) {
  return !metric.metric_snapshot_id.empty() &&
         !metric.route_label.empty() &&
         !metric.provider_id.empty() &&
         !metric.index_generation.empty() &&
         !metric.result_contract_hash.empty() &&
         !metric.evidence_digest.empty() &&
         metric.generation != 0 &&
         metric.route_epoch != 0 &&
         metric.stats_epoch != 0 &&
         metric.index_generation_epoch != 0 &&
         metric.vector_count != 0 &&
         metric.dimensions != 0 &&
         metric.top_k != 0 &&
         metric.candidate_rows != 0 &&
         metric.fresh &&
         metric.trusted &&
         metric.exact_payload_available &&
         !UnsafeMetricAuthority(metric) &&
         !RatioInvalid(metric.recall_observed) &&
         !RatioInvalid(metric.metadata_prefilter_selectivity) &&
         !RatioInvalid(metric.list_imbalance_ratio) &&
         !RatioInvalid(metric.quantization_error_ratio);
}

std::uint64_t SaturatingAdd(std::uint64_t left, std::uint64_t right) {
  if (left > std::numeric_limits<std::uint64_t>::max() - right) {
    return std::numeric_limits<std::uint64_t>::max();
  }
  return left + right;
}

std::uint64_t RatioRows(std::uint64_t base, double ratio) {
  if (base == 0 || ratio <= 0.0) return 0;
  return static_cast<std::uint64_t>(
      std::ceil(static_cast<double>(base) * std::clamp(ratio, 0.0, 1.0)));
}

IndexStats IndexForProfile(IndexStats index, EnterpriseVectorIndexProfile profile) {
  switch (profile) {
    case EnterpriseVectorIndexProfile::kExact:
      index.index_family = "vector_exact";
      break;
    case EnterpriseVectorIndexProfile::kHnsw:
      index.index_family = "vector_hnsw";
      break;
    case EnterpriseVectorIndexProfile::kIvfFlat:
    case EnterpriseVectorIndexProfile::kIvfPq:
    case EnterpriseVectorIndexProfile::kIvfSq8:
      index.index_family = "vector_ivf";
      break;
  }
  index.candidate_set_producer = true;
  index.exact_recheck_required = true;
  index.mga_recheck_required = true;
  index.security_recheck_required = true;
  index.route_benchmark_clean = true;
  index.false_positive_ratio = std::clamp(index.false_positive_ratio, 0.0, 1.0);
  return index;
}

CostVector ApplyVectorMetricAdjustments(CostVector cost,
                                        const EnterpriseVectorAnnCostingRequest& request,
                                        std::uint64_t exact_rerank_rows,
                                        bool rebuild_recommended) {
  const auto& metric = request.metric;
  cost.row_cost = SaturatingAdd(cost.row_cost, exact_rerank_rows);
  cost.memory_cost = SaturatingAdd(cost.memory_cost,
                                   metric.candidate_rows * metric.dimensions);
  cost.uncertainty_cost = SaturatingAdd(
      cost.uncertainty_cost,
      RatioRows(metric.candidate_rows, metric.quantization_error_ratio));
  cost.uncertainty_cost = SaturatingAdd(
      cost.uncertainty_cost,
      RatioRows(metric.candidate_rows, metric.list_imbalance_ratio) / 2);
  if (rebuild_recommended) {
    cost.uncertainty_cost = SaturatingAdd(cost.uncertainty_cost,
                                          metric.candidate_rows);
  }
  cost.total_cost = SaturatingAdd(
      SaturatingAdd(cost.startup_cost, cost.row_cost),
      SaturatingAdd(cost.io_cost,
                    SaturatingAdd(cost.memory_cost, cost.uncertainty_cost)));
  cost.confidence = request.metric.recall_observed >= request.min_recall
                        ? CostConfidence::kMedium
                        : CostConfidence::kLow;
  return cost;
}

}  // namespace

const char* EnterpriseVectorIndexProfileName(EnterpriseVectorIndexProfile profile) {
  switch (profile) {
    case EnterpriseVectorIndexProfile::kExact:
      return "vector_exact";
    case EnterpriseVectorIndexProfile::kHnsw:
      return "vector_hnsw";
    case EnterpriseVectorIndexProfile::kIvfFlat:
      return "vector_ivf_flat";
    case EnterpriseVectorIndexProfile::kIvfPq:
      return "vector_ivf_pq";
    case EnterpriseVectorIndexProfile::kIvfSq8:
      return "vector_ivf_sq8";
  }
  return "vector_unknown";
}

EnterpriseVectorAnnCostingResult EstimateEnterpriseVectorAnnCost(
    const EnterpriseVectorAnnCostingRequest& request) {
  if (!MetricComplete(request.metric)) {
    return Refuse(request,
                  "SB_OPT_VECTOR_ANN_METRIC_REQUIRED",
                  "vector_ann.fresh_trusted_metric_required");
  }
  if (ApproximateProfile(request.profile) &&
      (!request.metric.exact_rerank_available ||
       !request.authority.exact_rerank_proven)) {
    return Refuse(request,
                  "SB_OPT_VECTOR_ANN_EXACT_RERANK_REQUIRED",
                  "vector_ann.exact_rerank_required");
  }
  if (request.profile == EnterpriseVectorIndexProfile::kExact &&
      !request.metric.exact_payload_available) {
    return Refuse(request,
                  "SB_OPT_VECTOR_EXACT_PAYLOAD_REQUIRED",
                  "vector_exact.payload_required");
  }

  auto cost_request = EnterpriseIndexCostRequest{};
  cost_request.index = IndexForProfile(request.index, request.profile);
  cost_request.table = request.table;
  cost_request.environment = request.environment;
  cost_request.intent = EnterpriseIndexAccessIntent::kVectorSearch;
  cost_request.authority = request.authority;
  cost_request.authority.exact_rerank_proven =
      !ApproximateProfile(request.profile) || request.authority.exact_rerank_proven;
  cost_request.requested_limit = request.metric.top_k;
  cost_request.require_benchmark_clean = true;
  auto base = EstimateEnterpriseIndexAccessCost(cost_request);
  if (!base.accepted) {
    return Refuse(request, base.diagnostic_code,
                  "vector_ann.base_index_cost_refused");
  }

  const auto tombstone_ratio =
      static_cast<double>(request.metric.tombstone_rows) /
      static_cast<double>(std::max<std::uint64_t>(request.metric.vector_count, 1));
  const bool rebuild_recommended =
      tombstone_ratio > request.max_tombstone_ratio ||
      request.metric.list_imbalance_ratio > request.max_list_imbalance_ratio;
  const bool quantization_too_high =
      request.metric.quantization_error_ratio >
      request.max_quantization_error_ratio;
  const bool recall_too_low =
      ApproximateProfile(request.profile) &&
      request.metric.recall_observed < request.min_recall;

  EnterpriseVectorAnnCostingResult result;
  result.accepted = true;
  result.selectable = true;
  result.profile = request.profile;
  result.diagnostic_code = "SB_OPT_VECTOR_ANN_COST_OK";
  result.estimated_candidate_rows = request.metric.candidate_rows;
  result.exact_rerank_rows =
      request.metric.exact_rerank_rows == 0
          ? std::max(request.metric.top_k, request.metric.candidate_rows)
          : request.metric.exact_rerank_rows;
  result.tombstone_rows = request.metric.tombstone_rows;
  result.rebuild_recommended = rebuild_recommended;

  if ((recall_too_low || quantization_too_high) &&
      request.metric.exact_fallback_available) {
    result.exact_fallback_selected = true;
    result.profile = EnterpriseVectorIndexProfile::kExact;
    result.diagnostic_code = "SB_OPT_VECTOR_ANN_EXACT_FALLBACK_SELECTED";
  } else if (recall_too_low) {
    return Refuse(request,
                  "SB_OPT_VECTOR_ANN_RECALL_BELOW_POLICY",
                  "vector_ann.recall_below_policy_without_exact_fallback");
  } else if (quantization_too_high) {
    return Refuse(request,
                  "SB_OPT_VECTOR_ANN_QUANTIZATION_ERROR",
                  "vector_ann.quantization_error_without_exact_fallback");
  }

  result.cost = ApplyVectorMetricAdjustments(base.cost, request,
                                             result.exact_rerank_rows,
                                             rebuild_recommended);
  result.evidence.push_back("OEIC_VECTOR_ANN_RECALL_COSTING");
  result.evidence.push_back(std::string("vector_profile=") +
                            EnterpriseVectorIndexProfileName(request.profile));
  result.evidence.push_back("metric_snapshot_id=" + request.metric.metric_snapshot_id);
  result.evidence.push_back("route_label=" + request.metric.route_label);
  result.evidence.push_back("result_contract_hash=" +
                            request.metric.result_contract_hash);
  result.evidence.push_back("recall_observed=" +
                            std::to_string(request.metric.recall_observed));
  result.evidence.push_back("exact_rerank_rows=" +
                            std::to_string(result.exact_rerank_rows));
  result.evidence.push_back("tombstone_rows=" +
                            std::to_string(result.tombstone_rows));
  result.evidence.push_back("list_imbalance_ratio=" +
                            std::to_string(request.metric.list_imbalance_ratio));
  result.evidence.push_back("quantization_error_ratio=" +
                            std::to_string(request.metric.quantization_error_ratio));
  result.evidence.push_back("metadata_prefilter_selectivity=" +
                            std::to_string(request.metric.metadata_prefilter_selectivity));
  result.evidence.push_back(std::string("exact_fallback_selected=") +
                            (result.exact_fallback_selected ? "true" : "false"));
  result.evidence.push_back(std::string("rebuild_recommended=") +
                            (result.rebuild_recommended ? "true" : "false"));
  result.evidence.push_back("exact_recheck_required=true");
  result.evidence.push_back("mga_security_recheck_preserved=true");
  result.evidence.push_back("cluster_authority=false");
  return result;
}

}  // namespace scratchbird::engine::optimizer

// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "optimizer_graph_document_costing_enterprise.hpp"

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

EnterpriseGraphDocumentCostingResult Refuse(
    const EnterpriseGraphDocumentCostingRequest& request,
    std::string diagnostic_code,
    std::string evidence) {
  EnterpriseGraphDocumentCostingResult result;
  result.accepted = false;
  result.selectable = false;
  result.profile = request.profile;
  result.diagnostic_code = std::move(diagnostic_code);
  result.cost.selectable = false;
  result.cost.confidence = CostConfidence::kRejected;
  result.cost.reason = "enterprise_graph_document_cost_refused";
  result.cost.rejection_reason = result.diagnostic_code;
  Add(&result.evidence, "OEIC_GRAPH_DOCUMENT_ROUTE_COSTING");
  Add(&result.evidence, std::move(evidence));
  Add(&result.evidence, "graph_document.finality_authority=false");
  Add(&result.evidence, "graph_document.visibility_authority=false");
  Add(&result.evidence, "graph_document.security_authority=false");
  return result;
}

bool RatioInvalid(double value) {
  return value < 0.0 || value > 1.0 || std::isnan(value);
}

bool UnsafeAuthority(const EnterpriseGraphDocumentMetric& metric) {
  return metric.parser_or_donor_authority ||
         metric.client_authority ||
         metric.metric_finality_or_visibility_authority ||
         metric.provider_finality_or_visibility_authority ||
         metric.recovery_or_wal_authority ||
         metric.cluster_route_or_metric_projection;
}

bool MetricCommonComplete(const EnterpriseGraphDocumentMetric& metric) {
  return !metric.metric_snapshot_id.empty() &&
         !metric.route_label.empty() &&
         !metric.provider_id.empty() &&
         !metric.index_generation.empty() &&
         !metric.result_contract_hash.empty() &&
         !metric.evidence_digest.empty() &&
         metric.generation != 0 &&
         metric.route_epoch != 0 &&
         metric.stats_epoch != 0 &&
         metric.candidate_rows != 0 &&
         metric.exact_recheck_rows != 0 &&
         metric.fresh &&
         metric.trusted &&
         metric.exact_recheck_available &&
         !UnsafeAuthority(metric) &&
         !RatioInvalid(metric.document_path_selectivity) &&
         !RatioInvalid(metric.document_shape_selectivity) &&
         !RatioInvalid(metric.false_positive_ratio);
}

bool MetricCompleteForProfile(EnterpriseGraphDocumentProfile profile,
                              const EnterpriseGraphDocumentMetric& metric) {
  if (!MetricCommonComplete(metric)) return false;
  if (profile == EnterpriseGraphDocumentProfile::kDocumentPath) {
    return metric.document_shape_count != 0 &&
           metric.document_path_selectivity > 0.0 &&
           metric.document_shape_selectivity > 0.0 &&
           (metric.document_wildcard_fanout == 0 ||
            metric.path_wildcard_proof_present) &&
           (metric.document_array_expansion_rows == 0 ||
            metric.array_expansion_proof_present);
  }
  return metric.graph_frontier_width != 0 &&
         metric.graph_adjacency_degree != 0 &&
         metric.graph_label_selectivity_ppm != 0 &&
         metric.graph_property_selectivity_ppm != 0 &&
         metric.graph_frontier_proof_present &&
         metric.graph_adjacency_proof_present;
}

IndexStats IndexForProfile(IndexStats index, EnterpriseGraphDocumentProfile profile) {
  index.index_family = profile == EnterpriseGraphDocumentProfile::kDocumentPath
                           ? "document_path"
                           : "graph";
  index.candidate_set_producer = true;
  index.exact_recheck_required = true;
  index.mga_recheck_required = true;
  index.security_recheck_required = true;
  index.route_benchmark_clean = true;
  return index;
}

std::uint64_t SaturatingAdd(std::uint64_t left, std::uint64_t right) {
  if (left > std::numeric_limits<std::uint64_t>::max() - right) {
    return std::numeric_limits<std::uint64_t>::max();
  }
  return left + right;
}

CostVector ApplyMetricAdjustments(CostVector cost,
                                  const EnterpriseGraphDocumentCostingRequest& request) {
  const auto& metric = request.metric;
  if (request.profile == EnterpriseGraphDocumentProfile::kDocumentPath) {
    cost.row_cost = SaturatingAdd(cost.row_cost, metric.document_array_expansion_rows);
    cost.row_cost = SaturatingAdd(cost.row_cost, metric.document_wildcard_fanout);
    cost.memory_cost = SaturatingAdd(cost.memory_cost, metric.document_shape_count);
  } else {
    cost.row_cost = SaturatingAdd(cost.row_cost,
                                  metric.graph_frontier_width *
                                      metric.graph_adjacency_degree);
    cost.memory_cost = SaturatingAdd(cost.memory_cost,
                                     metric.graph_visited_bitmap_density_ppm / 1000);
  }
  cost.uncertainty_cost = SaturatingAdd(
      cost.uncertainty_cost,
      static_cast<std::uint64_t>(
          std::ceil(metric.candidate_rows * metric.false_positive_ratio)));
  cost.total_cost = SaturatingAdd(
      SaturatingAdd(cost.startup_cost, cost.row_cost),
      SaturatingAdd(cost.io_cost,
                    SaturatingAdd(cost.memory_cost, cost.uncertainty_cost)));
  cost.confidence = CostConfidence::kMedium;
  return cost;
}

}  // namespace

const char* EnterpriseGraphDocumentProfileName(EnterpriseGraphDocumentProfile profile) {
  switch (profile) {
    case EnterpriseGraphDocumentProfile::kDocumentPath:
      return "document_path";
    case EnterpriseGraphDocumentProfile::kGraphSeed:
      return "graph_seed";
  }
  return "unknown";
}

EnterpriseGraphDocumentCostingResult EstimateEnterpriseGraphDocumentCost(
    const EnterpriseGraphDocumentCostingRequest& request) {
  if (!MetricCompleteForProfile(request.profile, request.metric)) {
    return Refuse(request,
                  "SB_OPT_GRAPH_DOCUMENT_METRIC_REQUIRED",
                  "graph_document.fresh_trusted_metric_required");
  }
  if (request.metric.false_positive_ratio > request.max_false_positive_ratio) {
    return Refuse(request,
                  "SB_OPT_GRAPH_DOCUMENT_FALSE_POSITIVE_POLICY",
                  "graph_document.false_positive_policy");
  }

  auto cost_request = EnterpriseIndexCostRequest{};
  cost_request.index = IndexForProfile(request.index, request.profile);
  cost_request.table = request.table;
  cost_request.environment = request.environment;
  cost_request.intent = request.profile == EnterpriseGraphDocumentProfile::kDocumentPath
                            ? EnterpriseIndexAccessIntent::kDocumentProbe
                            : EnterpriseIndexAccessIntent::kGraphSeed;
  cost_request.authority = request.authority;
  cost_request.requested_limit = request.metric.candidate_rows;
  cost_request.require_benchmark_clean = true;
  auto base = EstimateEnterpriseIndexAccessCost(cost_request);
  if (!base.accepted) {
    return Refuse(request, base.diagnostic_code,
                  "graph_document.base_index_cost_refused");
  }

  EnterpriseGraphDocumentCostingResult result;
  result.accepted = true;
  result.selectable = true;
  result.profile = request.profile;
  result.diagnostic_code = "SB_OPT_GRAPH_DOCUMENT_COST_OK";
  result.candidate_rows = request.metric.candidate_rows;
  result.exact_recheck_rows = request.metric.exact_recheck_rows;
  result.exact_fallback_required = request.metric.false_positive_ratio > 0.0;
  result.cost = ApplyMetricAdjustments(base.cost, request);
  result.evidence.push_back("OEIC_GRAPH_DOCUMENT_ROUTE_COSTING");
  result.evidence.push_back(std::string("graph_document_profile=") +
                            EnterpriseGraphDocumentProfileName(request.profile));
  result.evidence.push_back("metric_snapshot_id=" + request.metric.metric_snapshot_id);
  result.evidence.push_back("route_label=" + request.metric.route_label);
  result.evidence.push_back("result_contract_hash=" +
                            request.metric.result_contract_hash);
  result.evidence.push_back("candidate_rows=" +
                            std::to_string(result.candidate_rows));
  result.evidence.push_back("exact_recheck_rows=" +
                            std::to_string(result.exact_recheck_rows));
  result.evidence.push_back("document_path_selectivity=" +
                            std::to_string(request.metric.document_path_selectivity));
  result.evidence.push_back("document_shape_selectivity=" +
                            std::to_string(request.metric.document_shape_selectivity));
  result.evidence.push_back("document_array_expansion_rows=" +
                            std::to_string(request.metric.document_array_expansion_rows));
  result.evidence.push_back("document_wildcard_fanout=" +
                            std::to_string(request.metric.document_wildcard_fanout));
  result.evidence.push_back("graph_frontier_width=" +
                            std::to_string(request.metric.graph_frontier_width));
  result.evidence.push_back("graph_adjacency_degree=" +
                            std::to_string(request.metric.graph_adjacency_degree));
  result.evidence.push_back("graph_visited_bitmap_density_ppm=" +
                            std::to_string(request.metric.graph_visited_bitmap_density_ppm));
  result.evidence.push_back("false_positive_ratio=" +
                            std::to_string(request.metric.false_positive_ratio));
  result.evidence.push_back("exact_recheck_required=true");
  result.evidence.push_back("mga_security_recheck_preserved=true");
  result.evidence.push_back("cluster_authority=false");
  return result;
}

}  // namespace scratchbird::engine::optimizer

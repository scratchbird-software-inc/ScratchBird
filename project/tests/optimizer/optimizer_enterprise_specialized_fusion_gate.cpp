// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "optimizer_specialized_fusion_enterprise.hpp"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace opt = scratchbird::engine::optimizer;
namespace planner = scratchbird::engine::planner;

namespace {

bool Require(bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << "OEIC-060 gate failure: " << message << '\n';
    return false;
  }
  return true;
}

bool ContainsPrefix(const std::vector<std::string>& values, const std::string& prefix) {
  return std::any_of(values.begin(), values.end(), [&](const std::string& value) {
    return value.find(prefix) == 0;
  });
}

opt::EnterpriseFusionFamilyMetric Metric(opt::EnterpriseFusionFamily family,
                                         std::uint64_t rows,
                                         std::uint64_t candidate_rows,
                                         std::uint64_t recheck_rows,
                                         std::uint64_t cost) {
  const std::string name = opt::EnterpriseFusionFamilyName(family);
  opt::EnterpriseFusionFamilyMetric metric;
  metric.family = family;
  metric.metric_snapshot_id = "metrics:oeic060:" + name;
  metric.route_label = "embedded.local.oeic060";
  metric.provider_id = "provider:" + name;
  metric.plan_node_id = "plan_node:" + name;
  metric.result_contract_hash = "sha256:oeic060-result";
  metric.evidence_digest = "sha256:oeic060-evidence-" + name;
  metric.generation = 6001 + cost;
  metric.route_epoch = 6010;
  metric.stats_epoch = 6011;
  metric.security_epoch = 6012;
  metric.redaction_epoch = 6013;
  metric.estimated_rows = rows;
  metric.candidate_rows = candidate_rows;
  metric.exact_recheck_rows = recheck_rows;
  metric.cost_units = cost;
  metric.selectivity = static_cast<double>(candidate_rows) /
                       static_cast<double>(rows == 0 ? 1 : rows);
  metric.false_positive_ratio = 0.05;
  metric.route_consumed = true;
  metric.fresh = true;
  metric.trusted = true;
  metric.exact_recheck_required = true;
  metric.exact_recheck_available = true;
  metric.exact_rerank_required = family == opt::EnterpriseFusionFamily::kVector ||
                                 family == opt::EnterpriseFusionFamily::kSearch;
  metric.exact_rerank_available = true;
  metric.mga_recheck_required = true;
  metric.security_recheck_required = true;
  return metric;
}

opt::PlanCandidate Candidate(opt::EnterpriseFusionFamily family) {
  const std::string name = opt::EnterpriseFusionFamilyName(family);
  opt::PlanCandidate candidate;
  candidate.candidate_id = "oeic060." + name + ".candidate";
  candidate.scope = "local." + name;
  candidate.access_kind = family == opt::EnterpriseFusionFamily::kDocument
                              ? planner::PhysicalAccessKind::kDocumentPathProbe
                          : family == opt::EnterpriseFusionFamily::kSearch
                              ? planner::PhysicalAccessKind::kFullTextProbe
                          : family == opt::EnterpriseFusionFamily::kVector
                              ? planner::PhysicalAccessKind::kVectorApproximateWithFallback
                          : family == opt::EnterpriseFusionFamily::kGraph
                              ? planner::PhysicalAccessKind::kGraphTraversalSeed
                              : planner::PhysicalAccessKind::kBitmapSummaryScan;
  candidate.required_facts = {"fusion_family=" + name};
  candidate.acceptance_reasons = {"specialized_fusion_family=" + name};
  candidate.runtime_evidence = {"specialized_fusion.metric_family=" + name,
                                "exact_recheck_required=true",
                                "mga_recheck_required=true",
                                "security_recheck_required=true"};
  candidate.cost.selectable = true;
  candidate.cost.total_cost = 100;
  candidate.estimated_rows = 1000;
  return candidate;
}

opt::EnterpriseSpecializedFusionRequest Request() {
  opt::EnterpriseSpecializedFusionRequest request;
  request.route_label = "embedded.local.oeic060";
  request.sql_result_hash = "sha256:oeic060-result";
  request.fusion_result_hash = "sha256:oeic060-result";
  request.sql_route_consumed = true;
  request.family_metrics = {
      Metric(opt::EnterpriseFusionFamily::kDocument, 10'000, 2'000, 2'000, 100),
      Metric(opt::EnterpriseFusionFamily::kSearch, 8'000, 1'500, 1'500, 120),
      Metric(opt::EnterpriseFusionFamily::kVector, 7'000, 700, 700, 180),
      Metric(opt::EnterpriseFusionFamily::kGraph, 4'000, 500, 500, 160),
      Metric(opt::EnterpriseFusionFamily::kCandidateSet, 3'000, 300, 300, 40)};
  request.candidates = {
      Candidate(opt::EnterpriseFusionFamily::kDocument),
      Candidate(opt::EnterpriseFusionFamily::kSearch),
      Candidate(opt::EnterpriseFusionFamily::kVector),
      Candidate(opt::EnterpriseFusionFamily::kGraph),
      Candidate(opt::EnterpriseFusionFamily::kCandidateSet)};
  return request;
}

bool CompleteFusionConsumesAllFamilyMetrics() {
  const auto decision = opt::PlanEnterpriseSpecializedFusion(Request());
  if (!Require(decision.ok, "complete specialized fusion request was refused")) {
    return false;
  }
  return Require(decision.selected_candidates.size() == 5,
                 "not all specialized candidates were selected") &&
         Require(decision.fused_candidate_rows == 300,
                 "fused candidate rows did not use selective intersection") &&
         Require(decision.fused_exact_recheck_rows == 5'000,
                 "exact recheck rows were not accumulated") &&
         Require(ContainsPrefix(decision.evidence,
                                "specialized_fusion.metric_consumed:vector"),
                 "vector metric evidence missing") &&
         Require(ContainsPrefix(decision.evidence,
                                "specialized_fusion.metric_consumed:candidate_set"),
                 "candidate-set metric evidence missing");
}

bool RefusesMissingFamilyMetric() {
  auto request = Request();
  request.family_metrics.pop_back();
  const auto decision = opt::PlanEnterpriseSpecializedFusion(request);
  return Require(!decision.ok, "missing candidate-set metric was accepted") &&
         Require(decision.diagnostic_code ==
                     "SB_OPT_SPECIALIZED_FUSION.FAMILY_METRIC_REQUIRED",
                 "missing family diagnostic changed");
}

bool RefusesMissingExactRerank() {
  auto request = Request();
  for (auto& metric : request.family_metrics) {
    if (metric.family == opt::EnterpriseFusionFamily::kVector) {
      metric.exact_rerank_available = false;
    }
  }
  const auto decision = opt::PlanEnterpriseSpecializedFusion(request);
  return Require(!decision.ok, "vector route without exact rerank was accepted") &&
         Require(decision.diagnostic_code ==
                     "SB_OPT_SPECIALIZED_FUSION.EXACT_RERANK_REQUIRED",
                 "exact rerank diagnostic changed");
}

bool RefusesDescriptorAndClusterFallback() {
  auto request = Request();
  request.family_metrics.front().descriptor_scan_fallback = true;
  const auto descriptor = opt::PlanEnterpriseSpecializedFusion(request);
  if (!Require(!descriptor.ok, "descriptor-scan fallback was accepted")) {
    return false;
  }

  request = Request();
  request.family_metrics.front().cluster_route_or_metric_projection = true;
  const auto cluster = opt::PlanEnterpriseSpecializedFusion(request);
  return Require(!cluster.ok, "cluster metric projection was accepted") &&
         Require(cluster.diagnostic_code ==
                     "SB_OPT_SPECIALIZED_FUSION.FAMILY_METRIC_UNSAFE",
                 "cluster metric diagnostic changed");
}

bool RefusesNonEquivalentResultHash() {
  auto request = Request();
  request.fusion_result_hash = "sha256:different";
  const auto decision = opt::PlanEnterpriseSpecializedFusion(request);
  return Require(!decision.ok, "non-equivalent fusion result was accepted") &&
         Require(decision.diagnostic_code ==
                     "SB_OPT_SPECIALIZED_FUSION.SQL_EQUIVALENCE_REQUIRED",
                 "result equivalence diagnostic changed");
}

}  // namespace

int main() {
  // SEARCH_KEY: OEIC_SPECIALIZED_FUSION_ENTERPRISE
  if (!CompleteFusionConsumesAllFamilyMetrics()) return EXIT_FAILURE;
  if (!RefusesMissingFamilyMetric()) return EXIT_FAILURE;
  if (!RefusesMissingExactRerank()) return EXIT_FAILURE;
  if (!RefusesDescriptorAndClusterFallback()) return EXIT_FAILURE;
  if (!RefusesNonEquivalentResultHash()) return EXIT_FAILURE;
  return EXIT_SUCCESS;
}

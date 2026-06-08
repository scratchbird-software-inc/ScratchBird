// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "optimizer_specialized_fusion_enterprise.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <string>
#include <utility>

namespace scratchbird::engine::optimizer {
namespace {

constexpr std::array<EnterpriseFusionFamily, 5> kRequiredFamilies = {
    EnterpriseFusionFamily::kDocument,
    EnterpriseFusionFamily::kSearch,
    EnterpriseFusionFamily::kVector,
    EnterpriseFusionFamily::kGraph,
    EnterpriseFusionFamily::kCandidateSet};

void Add(std::vector<std::string>* evidence, std::string value) {
  evidence->push_back(std::move(value));
}

EnterpriseSpecializedFusionDecision Refuse(std::string diagnostic_code,
                                           std::string evidence) {
  EnterpriseSpecializedFusionDecision result;
  result.ok = false;
  result.fail_closed = true;
  result.diagnostic_code = std::move(diagnostic_code);
  Add(&result.evidence, "OEIC_SPECIALIZED_FUSION_ENTERPRISE");
  Add(&result.evidence, "specialized_fusion.accepted=false");
  Add(&result.evidence, std::move(evidence));
  Add(&result.evidence, "specialized_fusion.finality_authority=false");
  Add(&result.evidence, "specialized_fusion.visibility_authority=false");
  Add(&result.evidence, "specialized_fusion.security_authority=false");
  return result;
}

bool FamilyEquals(EnterpriseFusionFamily left, EnterpriseFusionFamily right) {
  return left == right;
}

bool RatioInvalid(double ratio) {
  return ratio < 0.0 || ratio > 1.0;
}

bool UnsafeAuthority(const EnterpriseFusionFamilyMetric& metric) {
  return metric.parser_or_donor_authority ||
         metric.client_authority ||
         metric.provider_finality_or_visibility_authority ||
         metric.metric_finality_or_visibility_authority ||
         metric.recovery_or_wal_authority ||
         metric.cluster_route_or_metric_projection;
}

const EnterpriseFusionFamilyMetric* FindMetric(
    const std::vector<EnterpriseFusionFamilyMetric>& metrics,
    EnterpriseFusionFamily family) {
  const auto it = std::find_if(metrics.begin(), metrics.end(),
                              [&](const EnterpriseFusionFamilyMetric& metric) {
                                return FamilyEquals(metric.family, family);
                              });
  return it == metrics.end() ? nullptr : &*it;
}

bool CandidateMentionsFamily(const PlanCandidate& candidate,
                             EnterpriseFusionFamily family) {
  const std::string family_name = EnterpriseFusionFamilyName(family);
  if (candidate.candidate_id.find(family_name) != std::string::npos) {
    return true;
  }
  if (candidate.scope.find(family_name) != std::string::npos) {
    return true;
  }
  auto mentions = [&](const std::vector<std::string>& values) {
    return std::any_of(values.begin(), values.end(), [&](const std::string& value) {
      return value.find(family_name) != std::string::npos;
    });
  };
  return mentions(candidate.required_facts) ||
         mentions(candidate.acceptance_reasons) ||
         mentions(candidate.runtime_evidence);
}

bool CandidateSelectableForFamily(const std::vector<PlanCandidate>& candidates,
                                  EnterpriseFusionFamily family) {
  return std::any_of(candidates.begin(), candidates.end(),
                     [&](const PlanCandidate& candidate) {
                       return candidate.cost.selectable &&
                              candidate.missing_facts.empty() &&
                              candidate.refusal_reasons.empty() &&
                              CandidateMentionsFamily(candidate, family);
                     });
}

std::uint64_t SaturatingAdd(std::uint64_t left, std::uint64_t right) {
  if (left > std::numeric_limits<std::uint64_t>::max() - right) {
    return std::numeric_limits<std::uint64_t>::max();
  }
  return left + right;
}

std::uint64_t MinNonZero(std::uint64_t left, std::uint64_t right) {
  if (left == 0) return right;
  if (right == 0) return left;
  return std::min(left, right);
}

bool MetricComplete(const EnterpriseFusionFamilyMetric& metric,
                    const std::string& route_label,
                    const std::string& result_contract_hash) {
  return !metric.metric_snapshot_id.empty() &&
         !metric.route_label.empty() &&
         metric.route_label == route_label &&
         !metric.provider_id.empty() &&
         !metric.plan_node_id.empty() &&
         metric.result_contract_hash == result_contract_hash &&
         !metric.evidence_digest.empty() &&
         metric.generation != 0 &&
         metric.route_epoch != 0 &&
         metric.stats_epoch != 0 &&
         metric.security_epoch != 0 &&
         metric.redaction_epoch != 0 &&
         metric.estimated_rows != 0 &&
         metric.candidate_rows != 0 &&
         metric.cost_units != 0 &&
         metric.route_consumed &&
         metric.fresh &&
         metric.trusted &&
         metric.exact_recheck_required &&
         metric.exact_recheck_available &&
         metric.mga_recheck_required &&
         metric.security_recheck_required &&
         !metric.descriptor_scan_fallback &&
         !metric.behavior_store_scan_fallback &&
         !UnsafeAuthority(metric) &&
         !RatioInvalid(metric.selectivity) &&
         !RatioInvalid(metric.false_positive_ratio);
}

}  // namespace

const char* EnterpriseFusionFamilyName(EnterpriseFusionFamily family) {
  switch (family) {
    case EnterpriseFusionFamily::kDocument:
      return "document";
    case EnterpriseFusionFamily::kSearch:
      return "search";
    case EnterpriseFusionFamily::kVector:
      return "vector";
    case EnterpriseFusionFamily::kGraph:
      return "graph";
    case EnterpriseFusionFamily::kCandidateSet:
      return "candidate_set";
  }
  return "unknown";
}

EnterpriseSpecializedFusionDecision PlanEnterpriseSpecializedFusion(
    const EnterpriseSpecializedFusionRequest& request) {
  if (request.route_label.empty()) {
    return Refuse("SB_OPT_SPECIALIZED_FUSION.ROUTE_REQUIRED",
                  "specialized_fusion.route_label_required");
  }
  if (!request.sql_route_consumed ||
      request.sql_result_hash.empty() ||
      request.sql_result_hash != request.fusion_result_hash) {
    return Refuse("SB_OPT_SPECIALIZED_FUSION.SQL_EQUIVALENCE_REQUIRED",
                  "specialized_fusion.sql_route_and_result_equivalence_required");
  }
  if (request.candidates.empty()) {
    return Refuse("SB_OPT_SPECIALIZED_FUSION.CANDIDATE_REQUIRED",
                  "specialized_fusion.candidate_required");
  }

  EnterpriseSpecializedFusionDecision decision;
  decision.ok = true;
  decision.fail_closed = false;
  decision.diagnostic_code = "SB_OPT_SPECIALIZED_FUSION.OK";
  Add(&decision.evidence, "OEIC_SPECIALIZED_FUSION_ENTERPRISE");
  Add(&decision.evidence, "specialized_fusion.accepted=true");
  Add(&decision.evidence, "specialized_fusion.route_label=" + request.route_label);
  Add(&decision.evidence, "specialized_fusion.result_equivalence=true");
  Add(&decision.evidence, "specialized_fusion.sql_route_consumed=true");
  Add(&decision.evidence, "specialized_fusion.descriptor_scan_fallback=false");
  Add(&decision.evidence, "specialized_fusion.behavior_store_scan_fallback=false");
  Add(&decision.evidence, "specialized_fusion.exact_recheck_required=true");
  Add(&decision.evidence, "specialized_fusion.mga_recheck_required=true");
  Add(&decision.evidence, "specialized_fusion.security_recheck_required=true");

  for (const auto family : kRequiredFamilies) {
    const auto* metric = FindMetric(request.family_metrics, family);
    if (metric == nullptr) {
      return Refuse("SB_OPT_SPECIALIZED_FUSION.FAMILY_METRIC_REQUIRED",
                    std::string("specialized_fusion.metric_missing:") +
                        EnterpriseFusionFamilyName(family));
    }
    if (!MetricComplete(*metric, request.route_label, request.fusion_result_hash)) {
      return Refuse("SB_OPT_SPECIALIZED_FUSION.FAMILY_METRIC_UNSAFE",
                    std::string("specialized_fusion.metric_unsafe:") +
                        EnterpriseFusionFamilyName(family));
    }
    if (metric->exact_rerank_required && !metric->exact_rerank_available) {
      return Refuse("SB_OPT_SPECIALIZED_FUSION.EXACT_RERANK_REQUIRED",
                    std::string("specialized_fusion.exact_rerank_missing:") +
                        EnterpriseFusionFamilyName(family));
    }
    if (!CandidateSelectableForFamily(request.candidates, family)) {
      return Refuse("SB_OPT_SPECIALIZED_FUSION.CANDIDATE_REFUSED",
                    std::string("specialized_fusion.candidate_refused:") +
                        EnterpriseFusionFamilyName(family));
    }

    decision.fused_estimated_rows =
        MinNonZero(decision.fused_estimated_rows, metric->estimated_rows);
    decision.fused_candidate_rows =
        MinNonZero(decision.fused_candidate_rows, metric->candidate_rows);
    decision.fused_exact_recheck_rows =
        SaturatingAdd(decision.fused_exact_recheck_rows,
                      metric->exact_recheck_rows);
    decision.fused_cost_units =
        SaturatingAdd(decision.fused_cost_units, metric->cost_units);
    Add(&decision.evidence,
        std::string("specialized_fusion.metric_consumed:") +
            EnterpriseFusionFamilyName(family) +
            ";generation=" + std::to_string(metric->generation) +
            ";candidate_rows=" + std::to_string(metric->candidate_rows));
  }

  for (const auto& candidate : request.candidates) {
    if (candidate.cost.selectable &&
        candidate.missing_facts.empty() &&
        candidate.refusal_reasons.empty()) {
      decision.selected_candidates.push_back(candidate);
    }
  }
  Add(&decision.evidence,
      "specialized_fusion.selected_candidate_count=" +
          std::to_string(decision.selected_candidates.size()));
  Add(&decision.evidence,
      "specialized_fusion.fused_estimated_rows=" +
          std::to_string(decision.fused_estimated_rows));
  Add(&decision.evidence,
      "specialized_fusion.fused_candidate_rows=" +
          std::to_string(decision.fused_candidate_rows));
  Add(&decision.evidence,
      "specialized_fusion.fused_exact_recheck_rows=" +
          std::to_string(decision.fused_exact_recheck_rows));
  Add(&decision.evidence,
      "specialized_fusion.fused_cost_units=" +
          std::to_string(decision.fused_cost_units));
  return decision;
}

}  // namespace scratchbird::engine::optimizer

// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "optimizer_text_wand_costing_enterprise.hpp"

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

EnterpriseTextWandCostingResult Refuse(const EnterpriseTextWandCostingRequest& request,
                                       std::string diagnostic_code,
                                       std::string evidence) {
  EnterpriseTextWandCostingResult result;
  result.accepted = false;
  result.selectable = false;
  result.profile = request.profile;
  result.diagnostic_code = std::move(diagnostic_code);
  result.cost.selectable = false;
  result.cost.confidence = CostConfidence::kRejected;
  result.cost.reason = "enterprise_text_wand_cost_refused";
  result.cost.rejection_reason = result.diagnostic_code;
  Add(&result.evidence, "OEIC_TEXT_WAND_RANKING_COSTING");
  Add(&result.evidence, std::move(evidence));
  Add(&result.evidence, "text_wand.finality_authority=false");
  Add(&result.evidence, "text_wand.visibility_authority=false");
  Add(&result.evidence, "text_wand.security_authority=false");
  return result;
}

bool RatioInvalid(double value) {
  return value < 0.0 || value > 1.0 || std::isnan(value);
}

bool UnsafeAuthority(const EnterpriseTextSearchMetric& metric) {
  return metric.parser_or_reference_authority ||
         metric.client_authority ||
         metric.metric_finality_or_visibility_authority ||
         metric.provider_finality_or_visibility_authority ||
         metric.recovery_or_wal_authority ||
         metric.cluster_route_or_metric_projection;
}

bool MetricComplete(const EnterpriseTextSearchMetric& metric) {
  return !metric.metric_snapshot_id.empty() &&
         !metric.route_label.empty() &&
         !metric.analyzer_id.empty() &&
         !metric.analyzer_epoch.empty() &&
         !metric.index_generation.empty() &&
         !metric.result_contract_hash.empty() &&
         !metric.evidence_digest.empty() &&
         metric.generation != 0 &&
         metric.route_epoch != 0 &&
         metric.stats_epoch != 0 &&
         metric.corpus_docs != 0 &&
         metric.query_terms != 0 &&
         metric.posting_length != 0 &&
         metric.candidate_rows != 0 &&
         metric.top_k != 0 &&
         metric.fresh &&
         metric.trusted &&
         metric.exact_recheck_available &&
         !UnsafeAuthority(metric) &&
         !RatioInvalid(metric.bm25_selectivity) &&
         !RatioInvalid(metric.false_positive_ratio);
}

bool RankedProfile(EnterpriseTextSearchProfile profile) {
  return profile == EnterpriseTextSearchProfile::kFullText ||
         profile == EnterpriseTextSearchProfile::kInverted ||
         profile == EnterpriseTextSearchProfile::kGin ||
         profile == EnterpriseTextSearchProfile::kSparseWand;
}

bool SparseWandProfile(EnterpriseTextSearchProfile profile) {
  return profile == EnterpriseTextSearchProfile::kSparseWand;
}

IndexStats IndexForProfile(IndexStats index, EnterpriseTextSearchProfile profile) {
  switch (profile) {
    case EnterpriseTextSearchProfile::kFullText:
      index.index_family = "full_text";
      break;
    case EnterpriseTextSearchProfile::kInverted:
      index.index_family = "inverted";
      break;
    case EnterpriseTextSearchProfile::kGin:
      index.index_family = "gin";
      break;
    case EnterpriseTextSearchProfile::kNgram:
      index.index_family = "ngram";
      break;
    case EnterpriseTextSearchProfile::kSparseWand:
      index.index_family = "sparse_wand";
      break;
  }
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

CostVector ApplyTextMetricAdjustments(CostVector cost,
                                      const EnterpriseTextWandCostingRequest& request,
                                      std::uint64_t rerank_rows) {
  const auto& metric = request.metric;
  const auto skipped = std::min(metric.blockmax_skips, metric.posting_length);
  const auto effective_postings = metric.posting_length - skipped;
  cost.row_cost = SaturatingAdd(cost.row_cost, effective_postings);
  cost.row_cost = SaturatingAdd(cost.row_cost, rerank_rows);
  cost.memory_cost = SaturatingAdd(cost.memory_cost,
                                   metric.query_terms * metric.top_k);
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

const char* EnterpriseTextSearchProfileName(EnterpriseTextSearchProfile profile) {
  switch (profile) {
    case EnterpriseTextSearchProfile::kFullText:
      return "full_text";
    case EnterpriseTextSearchProfile::kInverted:
      return "inverted";
    case EnterpriseTextSearchProfile::kGin:
      return "gin";
    case EnterpriseTextSearchProfile::kNgram:
      return "ngram";
    case EnterpriseTextSearchProfile::kSparseWand:
      return "sparse_wand";
  }
  return "text_unknown";
}

EnterpriseTextWandCostingResult EstimateEnterpriseTextWandCost(
    const EnterpriseTextWandCostingRequest& request) {
  if (!MetricComplete(request.metric)) {
    return Refuse(request,
                  "SB_OPT_TEXT_WAND_METRIC_REQUIRED",
                  "text_wand.fresh_trusted_metric_required");
  }
  if (request.metric.false_positive_ratio > request.max_false_positive_ratio &&
      !request.metric.exact_fallback_available) {
    return Refuse(request,
                  "SB_OPT_TEXT_WAND_FALSE_POSITIVE_POLICY",
                  "text_wand.false_positive_policy_without_exact_fallback");
  }
  if (RankedProfile(request.profile) &&
      (!request.metric.exact_rerank_available ||
       !request.authority.exact_rerank_proven)) {
    return Refuse(request,
                  "SB_OPT_TEXT_WAND_EXACT_RERANK_REQUIRED",
                  "text_wand.exact_rerank_required");
  }
  if (SparseWandProfile(request.profile) &&
      !request.metric.wand_topk_exact_equivalence) {
    if (!request.metric.exact_fallback_available) {
      return Refuse(request,
                    "SB_OPT_TEXT_WAND_EQUIVALENCE_REQUIRED",
                    "text_wand.topk_equivalence_without_exact_fallback");
    }
  }
  if (request.metric.phrase_position_hits != 0 &&
      !request.metric.phrase_position_proof_present) {
    return Refuse(request,
                  "SB_OPT_TEXT_WAND_PHRASE_PROOF_REQUIRED",
                  "text_wand.phrase_position_proof_required");
  }

  auto cost_request = EnterpriseIndexCostRequest{};
  cost_request.index = IndexForProfile(request.index, request.profile);
  cost_request.table = request.table;
  cost_request.environment = request.environment;
  cost_request.intent = EnterpriseIndexAccessIntent::kRankedSearch;
  cost_request.authority = request.authority;
  cost_request.authority.exact_rerank_proven =
      request.authority.exact_rerank_proven || !RankedProfile(request.profile);
  cost_request.requested_limit = request.metric.top_k;
  cost_request.require_benchmark_clean = true;
  auto base = EstimateEnterpriseIndexAccessCost(cost_request);
  if (!base.accepted) {
    return Refuse(request, base.diagnostic_code,
                  "text_wand.base_index_cost_refused");
  }

  EnterpriseTextWandCostingResult result;
  result.accepted = true;
  result.selectable = true;
  result.profile = request.profile;
  result.diagnostic_code = "SB_OPT_TEXT_WAND_COST_OK";
  result.candidate_rows = request.metric.candidate_rows;
  result.exact_rerank_rows =
      request.metric.exact_rerank_rows == 0
          ? std::max(request.metric.top_k, request.metric.candidate_rows)
          : request.metric.exact_rerank_rows;
  if ((request.metric.false_positive_ratio > request.max_false_positive_ratio ||
       (SparseWandProfile(request.profile) &&
        !request.metric.wand_topk_exact_equivalence)) &&
      request.metric.exact_fallback_available) {
    result.exact_fallback_selected = true;
    result.diagnostic_code = "SB_OPT_TEXT_WAND_EXACT_FALLBACK_SELECTED";
  }
  result.cost = ApplyTextMetricAdjustments(base.cost, request,
                                           result.exact_rerank_rows);
  result.evidence.push_back("OEIC_TEXT_WAND_RANKING_COSTING");
  result.evidence.push_back(std::string("text_profile=") +
                            EnterpriseTextSearchProfileName(request.profile));
  result.evidence.push_back("metric_snapshot_id=" + request.metric.metric_snapshot_id);
  result.evidence.push_back("route_label=" + request.metric.route_label);
  result.evidence.push_back("analyzer_id=" + request.metric.analyzer_id);
  result.evidence.push_back("posting_length=" +
                            std::to_string(request.metric.posting_length));
  result.evidence.push_back("blockmax_skips=" +
                            std::to_string(request.metric.blockmax_skips));
  result.evidence.push_back("impact_ordered_postings=" +
                            std::to_string(request.metric.impact_ordered_postings));
  result.evidence.push_back("bm25_selectivity=" +
                            std::to_string(request.metric.bm25_selectivity));
  result.evidence.push_back("false_positive_ratio=" +
                            std::to_string(request.metric.false_positive_ratio));
  result.evidence.push_back("candidate_rows=" +
                            std::to_string(result.candidate_rows));
  result.evidence.push_back("exact_rerank_rows=" +
                            std::to_string(result.exact_rerank_rows));
  result.evidence.push_back(std::string("wand_topk_exact_equivalence=") +
                            (request.metric.wand_topk_exact_equivalence
                                 ? "true"
                                 : "false"));
  result.evidence.push_back(std::string("exact_fallback_selected=") +
                            (result.exact_fallback_selected ? "true" : "false"));
  result.evidence.push_back("exact_recheck_required=true");
  result.evidence.push_back("mga_security_recheck_preserved=true");
  result.evidence.push_back("cluster_authority=false");
  return result;
}

}  // namespace scratchbird::engine::optimizer

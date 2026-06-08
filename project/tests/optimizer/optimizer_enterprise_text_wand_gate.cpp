// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "optimizer_text_wand_costing_enterprise.hpp"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace opt = scratchbird::engine::optimizer;

namespace {

bool Require(bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << "OEIC-062 gate failure: " << message << '\n';
    return false;
  }
  return true;
}

bool ContainsPrefix(const std::vector<std::string>& values, const std::string& prefix) {
  return std::any_of(values.begin(), values.end(), [&](const std::string& value) {
    return value.find(prefix) == 0;
  });
}

opt::OptimizerStatsIdentity Identity(const std::string& object_uuid) {
  opt::OptimizerStatsIdentity identity;
  identity.object_uuid = object_uuid;
  identity.statistic_uuid = "stats:" + object_uuid;
  identity.stats_epoch = 6201;
  identity.catalog_epoch = 6200;
  identity.transaction_visibility_epoch = 6202;
  identity.freshness = opt::OptimizerStatsFreshnessState::kFresh;
  identity.source = opt::StatisticSource::kCatalogExact;
  identity.confidence = opt::CostConfidence::kHigh;
  return identity;
}

opt::IndexStats TextIndex(const std::string& family) {
  opt::IndexStats index;
  index.identity = Identity("idx.text.062");
  index.index_uuid = "idx.text.062";
  index.relation_uuid = "rel.docs.062";
  index.index_family = family;
  index.height = 3;
  index.leaf_pages = 4096;
  index.distinct_keys = 800'000;
  index.clustering_factor = 0.5;
  index.visibility_coverage = 1.0;
  index.predicate_coverage = 0.1;
  index.candidate_set_producer = true;
  index.exact_recheck_required = true;
  index.mga_recheck_required = true;
  index.security_recheck_required = true;
  index.route_benchmark_clean = true;
  index.false_positive_ratio = 0.04;
  return index;
}

opt::TableCardinalityStats Table() {
  opt::TableCardinalityStats table;
  table.identity = Identity("rel.docs.062");
  table.row_count = 2'000'000;
  table.visible_row_count = 1'950'000;
  table.page_count = 50'000;
  table.average_row_bytes = 1024;
  return table;
}

opt::EnterpriseIndexCostAuthority Authority(bool exact_rerank = true) {
  opt::EnterpriseIndexCostAuthority authority;
  authority.optimizer_scope = true;
  authority.catalog_descriptor_authority = true;
  authority.index_stats_authority = true;
  authority.route_capability_authority = true;
  authority.runtime_metric_authority = true;
  authority.generated_index_readiness_manifest = true;
  authority.readiness_manifest_current = true;
  authority.route_runtime_proof = true;
  authority.operation_metric_producer_proof = true;
  authority.support_bundle_producer_proof = true;
  authority.crash_cleanup_corruption_proof = true;
  authority.storage_integration_proof = true;
  authority.exact_recheck_preserved = true;
  authority.mga_recheck_preserved = true;
  authority.security_recheck_preserved = true;
  authority.exact_rerank_proven = exact_rerank;
  return authority;
}

opt::EnterpriseTextSearchMetric Metric() {
  opt::EnterpriseTextSearchMetric metric;
  metric.metric_snapshot_id = "metrics:text:062";
  metric.route_label = "embedded.local.text.062";
  metric.analyzer_id = "analyzer:unicode-en-v1";
  metric.analyzer_epoch = "analyzer_epoch:062";
  metric.index_generation = "text_generation:062";
  metric.result_contract_hash = "sha256:text-result-062";
  metric.evidence_digest = "sha256:text-evidence-062";
  metric.generation = 6210;
  metric.route_epoch = 6211;
  metric.stats_epoch = 6212;
  metric.corpus_docs = 2'000'000;
  metric.query_terms = 3;
  metric.posting_length = 100'000;
  metric.phrase_position_hits = 300;
  metric.blockmax_skips = 75'000;
  metric.impact_ordered_postings = 25'000;
  metric.candidate_rows = 2'000;
  metric.exact_rerank_rows = 2'000;
  metric.top_k = 50;
  metric.bm25_selectivity = 0.001;
  metric.false_positive_ratio = 0.06;
  metric.fresh = true;
  metric.trusted = true;
  metric.phrase_position_proof_present = true;
  metric.exact_recheck_available = true;
  metric.exact_rerank_available = true;
  metric.exact_fallback_available = true;
  metric.wand_topk_exact_equivalence = true;
  return metric;
}

opt::EnterpriseTextWandCostingRequest Request(opt::EnterpriseTextSearchProfile profile) {
  opt::EnterpriseTextWandCostingRequest request;
  request.profile = profile;
  request.index = TextIndex(profile == opt::EnterpriseTextSearchProfile::kSparseWand
                                ? "sparse_wand"
                            : profile == opt::EnterpriseTextSearchProfile::kGin
                                ? "gin"
                            : profile == opt::EnterpriseTextSearchProfile::kNgram
                                ? "ngram"
                            : profile == opt::EnterpriseTextSearchProfile::kInverted
                                ? "inverted"
                                : "full_text");
  request.table = Table();
  request.environment.cost_profile_id = "enterprise-text-062";
  request.environment.memory_budget_bytes = 128 * 1024 * 1024;
  request.authority = Authority(true);
  request.metric = Metric();
  return request;
}

bool SparseWandConsumesRankingMetrics() {
  const auto result = opt::EstimateEnterpriseTextWandCost(
      Request(opt::EnterpriseTextSearchProfile::kSparseWand));
  if (!Require(result.accepted, "sparse-WAND costing was refused")) return false;
  return Require(result.selectable, "sparse-WAND costing was not selectable") &&
         Require(!result.exact_fallback_selected,
                 "exact WAND equivalence should not fallback") &&
         Require(result.exact_rerank_rows == 2'000,
                 "exact rerank rows were not consumed") &&
         Require(ContainsPrefix(result.evidence, "blockmax_skips="),
                 "block-max skip evidence missing") &&
         Require(ContainsPrefix(result.evidence, "wand_topk_exact_equivalence=true"),
                 "WAND equivalence evidence missing");
}

bool MissingWandEquivalenceSelectsExactFallback() {
  auto request = Request(opt::EnterpriseTextSearchProfile::kSparseWand);
  request.metric.wand_topk_exact_equivalence = false;
  const auto result = opt::EstimateEnterpriseTextWandCost(request);
  return Require(result.accepted, "WAND without equivalence and fallback was refused") &&
         Require(result.exact_fallback_selected,
                 "WAND without equivalence did not select exact fallback") &&
         Require(result.diagnostic_code ==
                     "SB_OPT_TEXT_WAND_EXACT_FALLBACK_SELECTED",
                 "WAND fallback diagnostic changed");
}

bool MissingExactRerankFailsClosed() {
  auto request = Request(opt::EnterpriseTextSearchProfile::kFullText);
  request.metric.exact_rerank_available = false;
  request.authority.exact_rerank_proven = false;
  const auto result = opt::EstimateEnterpriseTextWandCost(request);
  return Require(!result.accepted, "ranked text route without rerank was accepted") &&
         Require(result.diagnostic_code == "SB_OPT_TEXT_WAND_EXACT_RERANK_REQUIRED",
                 "missing text rerank diagnostic changed");
}

bool MissingPhraseProofFailsClosed() {
  auto request = Request(opt::EnterpriseTextSearchProfile::kFullText);
  request.metric.phrase_position_proof_present = false;
  const auto result = opt::EstimateEnterpriseTextWandCost(request);
  return Require(!result.accepted, "phrase route without position proof was accepted") &&
         Require(result.diagnostic_code == "SB_OPT_TEXT_WAND_PHRASE_PROOF_REQUIRED",
                 "phrase proof diagnostic changed");
}

bool UnsafeMetricAuthorityFailsClosed() {
  auto request = Request(opt::EnterpriseTextSearchProfile::kGin);
  request.metric.cluster_route_or_metric_projection = true;
  const auto result = opt::EstimateEnterpriseTextWandCost(request);
  return Require(!result.accepted, "cluster text metric was accepted") &&
         Require(result.diagnostic_code == "SB_OPT_TEXT_WAND_METRIC_REQUIRED",
                 "unsafe text metric diagnostic changed");
}

}  // namespace

int main() {
  // SEARCH_KEY: OEIC_TEXT_WAND_RANKING_COSTING
  if (!SparseWandConsumesRankingMetrics()) return EXIT_FAILURE;
  if (!MissingWandEquivalenceSelectsExactFallback()) return EXIT_FAILURE;
  if (!MissingExactRerankFailsClosed()) return EXIT_FAILURE;
  if (!MissingPhraseProofFailsClosed()) return EXIT_FAILURE;
  if (!UnsafeMetricAuthorityFailsClosed()) return EXIT_FAILURE;
  return EXIT_SUCCESS;
}

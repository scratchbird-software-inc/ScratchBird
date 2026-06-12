// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "optimizer_index_costing.hpp"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace opt = scratchbird::engine::optimizer;

namespace {

bool Require(bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << "OEIC index costing gate failure: " << message << '\n';
    return false;
  }
  return true;
}

bool HasStatus(const std::vector<opt::StatisticsContractStatus>& statuses,
               const std::string& code) {
  return std::any_of(statuses.begin(), statuses.end(), [&](const auto& status) {
    return status.diagnostic_code == code;
  });
}

opt::OptimizerStatsIdentity Identity(const std::string& object,
                                     const std::string& statistic) {
  opt::OptimizerStatsIdentity identity;
  identity.object_uuid = object;
  identity.statistic_uuid = statistic;
  identity.stats_epoch = 30;
  identity.catalog_epoch = 40;
  identity.transaction_visibility_epoch = 50;
  identity.freshness = opt::OptimizerStatsFreshnessState::kFresh;
  identity.source = opt::StatisticSource::kCatalogSample;
  identity.confidence = opt::CostConfidence::kHigh;
  return identity;
}

opt::TableCardinalityStats Table() {
  opt::TableCardinalityStats table;
  table.identity = Identity("rel.index.cost", "rel.index.cost:table_stats");
  table.row_count = 100000;
  table.visible_row_count = 98000;
  table.page_count = 1200;
  table.average_row_bytes = 128;
  return table;
}

opt::IndexStats Index(std::string family, std::string uuid) {
  opt::IndexStats index;
  index.identity = Identity("rel.index.cost", uuid + ":stats");
  index.index_uuid = std::move(uuid);
  index.relation_uuid = "rel.index.cost";
  index.index_family = std::move(family);
  index.descriptor_digest = "descriptor:" + index.index_uuid;
  index.height = 3;
  index.leaf_pages = 96;
  index.distinct_keys = 10000;
  index.clustering_factor = 0.65;
  index.fragmentation_ratio = 0.02;
  index.visibility_coverage = 1.0;
  index.predicate_coverage = 0.25;
  index.false_positive_ratio = 0.02;
  index.exact_recheck_required = true;
  index.mga_recheck_required = true;
  index.security_recheck_required = true;
  index.route_benchmark_clean = true;
  return index;
}

opt::EnterpriseIndexCostAuthority Authority() {
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
  authority.exact_rerank_proven = true;
  return authority;
}

opt::EnterpriseIndexCostRequest Request(opt::IndexStats index,
                                        opt::EnterpriseIndexAccessIntent intent) {
  opt::EnterpriseIndexCostRequest request;
  request.index = std::move(index);
  request.table = Table();
  request.intent = intent;
  request.authority = Authority();
  request.environment.memory_budget_bytes = 8 * 1024 * 1024;
  request.require_benchmark_clean = true;
  return request;
}

bool EnterpriseIndexCostingCoversBtreeAndHashRoutes() {
  // SEARCH_KEY: OEIC_INDEX_COSTING_ENTERPRISE_CLOSURE
  auto btree = Index("btree", "idx.cost.btree");
  btree.equality_lookup_supported = true;
  btree.ordered_range_supported = true;
  auto btree_eq =
      opt::EstimateEnterpriseIndexAccessCost(
          Request(btree, opt::EnterpriseIndexAccessIntent::kEqualityLookup));
  auto btree_range_req =
      Request(btree, opt::EnterpriseIndexAccessIntent::kOrderedRange);
  btree_range_req.requested_range_fraction = 0.10;
  auto btree_range = opt::EstimateEnterpriseIndexAccessCost(btree_range_req);

  auto hash = Index("hash", "idx.cost.hash");
  hash.equality_lookup_supported = true;
  hash.predicate_coverage = 1.0;
  auto hash_eq =
      opt::EstimateEnterpriseIndexAccessCost(
          Request(hash, opt::EnterpriseIndexAccessIntent::kEqualityLookup));

  return Require(btree_eq.accepted && btree_eq.selectable,
                 "btree equality route rejected") &&
         Require(btree_eq.estimated_rows > 0 &&
                     btree_eq.estimated_rows < Table().row_count,
                 "btree equality selectivity not applied") &&
         Require(btree_range.accepted && btree_range.selectable,
                 "btree range route rejected") &&
         Require(hash_eq.accepted && hash_eq.selectable,
                 "hash equality route rejected") &&
         Require(hash_eq.cost.selectable, "hash cost was not selectable");
}

bool EnterpriseIndexCostingBlocksUnsafeFamilyClaims() {
  auto bloom = Index("bloom", "idx.cost.bloom");
  bloom.negative_prune_supported = true;
  bloom.candidate_set_producer = true;
  auto bloom_eq =
      opt::EstimateEnterpriseIndexAccessCost(
          Request(bloom, opt::EnterpriseIndexAccessIntent::kEqualityLookup));
  auto bloom_prune =
      opt::EstimateEnterpriseIndexAccessCost(
          Request(bloom, opt::EnterpriseIndexAccessIntent::kNegativePrune));

  auto vector = Index("vector_hnsw", "idx.cost.vector");
  vector.candidate_set_producer = true;
  auto vector_req = Request(vector, opt::EnterpriseIndexAccessIntent::kVectorSearch);
  vector_req.authority.exact_rerank_proven = false;
  auto vector_bad = opt::EstimateEnterpriseIndexAccessCost(vector_req);
  vector_req.authority.exact_rerank_proven = true;
  auto vector_ok = opt::EstimateEnterpriseIndexAccessCost(vector_req);

  auto reference = Index("reference_emulated", "idx.cost.reference");
  reference.candidate_set_producer = true;
  auto reference_result =
      opt::EstimateEnterpriseIndexAccessCost(
          Request(reference, opt::EnterpriseIndexAccessIntent::kCandidateSet));

  return Require(!bloom_eq.accepted &&
                     bloom_eq.diagnostic_code ==
                         "SB_OPT_INDEX_COST_BLOOM_NEGATIVE_PRUNE_ONLY",
                 "Bloom equality claim was accepted") &&
         Require(bloom_prune.accepted && bloom_prune.recheck_rows != 0,
                 "Bloom negative-prune route rejected or missing recheck") &&
         Require(!vector_bad.accepted &&
                     vector_bad.diagnostic_code ==
                         "SB_OPT_INDEX_COST_EXACT_RERANK_REQUIRED",
                 "approximate vector route accepted without rerank") &&
         Require(vector_ok.accepted && vector_ok.selectable,
                 "approximate vector route rejected with rerank") &&
         Require(!reference_result.accepted &&
                     reference_result.diagnostic_code ==
                         "INDEX.CAPABILITY.REFERENCE_EMULATED.CONTRACT_ONLY_NON_AUTHORITY_MAPPING",
                 "reference-emulated index claim was accepted");
}

bool EnterpriseIndexCostingRejectsAuthorityDriftAndMissingBenchmarkProof() {
  auto btree = Index("btree", "idx.cost.authority");
  btree.equality_lookup_supported = true;
  auto unsafe = Request(btree, opt::EnterpriseIndexAccessIntent::kEqualityLookup);
  unsafe.authority.parser_or_reference_authority = true;
  const auto unsafe_result = opt::EstimateEnterpriseIndexAccessCost(unsafe);

  auto unclean = Request(btree, opt::EnterpriseIndexAccessIntent::kEqualityLookup);
  unclean.index.route_benchmark_clean = false;
  const auto unclean_result = opt::EstimateEnterpriseIndexAccessCost(unclean);

  return Require(!unsafe_result.accepted &&
                     unsafe_result.diagnostic_code ==
                         "SB_OPT_INDEX_COST_UNSAFE_AUTHORITY",
                 "unsafe authority was accepted") &&
         Require(!unclean_result.accepted &&
                     unclean_result.diagnostic_code ==
                         "SB_OPT_INDEX_COST_ROUTE_NOT_BENCHMARK_CLEAN",
                 "missing benchmark-clean route proof was accepted");
}

bool EnterpriseIndexFamilyMatrixIsComplete() {
  const auto statuses = opt::ValidateEnterpriseIndexCostingFamilyMatrix();
  return Require(HasStatus(statuses, "SB_OPT_INDEX_COST_FAMILY_MATRIX_OK"),
                 "noncluster index family costing matrix is incomplete") &&
         Require(HasStatus(statuses,
                           "SB_OPT_INDEX_COST_REFERENCE_EMULATED_CLAIM_REMOVED"),
                 "reference-emulated claim removal missing") &&
         Require(HasStatus(statuses,
                           "SB_OPT_INDEX_COST_POLICY_BLOCKED_CLAIM_REMOVED"),
                 "policy-blocked claim removal missing");
}

}  // namespace

int main() {
  if (!EnterpriseIndexCostingCoversBtreeAndHashRoutes()) return EXIT_FAILURE;
  if (!EnterpriseIndexCostingBlocksUnsafeFamilyClaims()) return EXIT_FAILURE;
  if (!EnterpriseIndexCostingRejectsAuthorityDriftAndMissingBenchmarkProof()) {
    return EXIT_FAILURE;
  }
  if (!EnterpriseIndexFamilyMatrixIsComplete()) return EXIT_FAILURE;
  return EXIT_SUCCESS;
}

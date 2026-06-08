// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "optimizer_index_costing.hpp"

#include "index_family_registry.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <set>
#include <utility>

namespace scratchbird::engine::optimizer {
namespace planner = scratchbird::engine::planner;
namespace idx = scratchbird::core::index;
namespace {

StatisticsContractStatus Status(bool ok, std::string code, std::string detail) {
  StatisticsContractStatus status;
  status.ok = ok;
  status.diagnostic_code = std::move(code);
  status.detail = std::move(detail);
  return status;
}

EnterpriseIndexCostResult Refuse(const EnterpriseIndexCostRequest& request,
                                 std::string code,
                                 std::string evidence) {
  EnterpriseIndexCostResult result;
  result.accepted = false;
  result.selectable = false;
  result.intent = request.intent;
  result.family_id = request.index.index_family;
  result.diagnostic_code = std::move(code);
  result.evidence.push_back(std::move(evidence));
  result.cost.selectable = false;
  result.cost.confidence = CostConfidence::kRejected;
  result.cost.rejection_reason = result.diagnostic_code;
  result.cost.reason = "enterprise_index_cost_refused";
  return result;
}

bool UnsafeAuthority(const EnterpriseIndexCostAuthority& authority) {
  return authority.parser_or_donor_authority ||
         authority.client_finality_authority ||
         authority.client_visibility_authority ||
         authority.metric_finality_authority ||
         authority.metric_visibility_authority ||
         authority.external_recovery_authority ||
         authority.cluster_authority ||
         authority.external_cluster_overclaim ||
         authority.benchmark_authority;
}

bool ReadinessEvidenceUnsafe(const EnterpriseIndexCostAuthority& authority) {
  return authority.static_registry_only ||
         authority.smoke_only ||
         authority.stale_manifest ||
         authority.synthetic_or_fixture_evidence;
}

bool AuthorityPresent(const EnterpriseIndexCostAuthority& authority) {
  return authority.optimizer_scope &&
         authority.catalog_descriptor_authority &&
         authority.index_stats_authority &&
         authority.route_capability_authority &&
         authority.runtime_metric_authority &&
         authority.generated_index_readiness_manifest &&
         authority.readiness_manifest_current &&
         authority.route_runtime_proof &&
         authority.operation_metric_producer_proof &&
         authority.support_bundle_producer_proof &&
         authority.crash_cleanup_corruption_proof &&
         authority.storage_integration_proof &&
         authority.exact_recheck_preserved &&
         authority.mga_recheck_preserved &&
         authority.security_recheck_preserved;
}

bool IsFamily(const IndexStats& index, const std::set<std::string>& families) {
  return families.find(index.index_family) != families.end();
}

bool BtreeAdjacent(const IndexStats& index) {
  return IsFamily(index, {"btree", "unique_btree", "expression", "partial", "covering"});
}

bool TextAdjacent(const IndexStats& index) {
  return IsFamily(index, {"full_text", "gin", "inverted", "ngram", "sparse_wand"});
}

bool SpatialAdjacent(const IndexStats& index) {
  return IsFamily(index, {"spatial", "rtree", "gist", "spgist"});
}

bool VectorApproximate(const IndexStats& index) {
  return IsFamily(index, {"vector_hnsw", "vector_ivf"});
}

bool VectorFamily(const IndexStats& index) {
  return IsFamily(index, {"vector_exact", "vector_hnsw", "vector_ivf"});
}

bool NegativePruneFamily(const IndexStats& index) {
  return IsFamily(index, {"brin_zone", "bloom", "columnar_zone"});
}

bool CandidateFamily(const IndexStats& index) {
  return IsFamily(index,
                  {"bitmap", "brin_zone", "bloom", "columnar_zone",
                   "full_text", "gin", "inverted", "ngram", "sparse_wand",
                   "spatial", "rtree", "gist", "spgist",
                   "vector_exact", "vector_hnsw", "vector_ivf",
                   "document_path", "graph", "temporary_work", "in_memory"});
}

bool IntentAllowedByFamily(const IndexStats& index,
                           EnterpriseIndexAccessIntent intent) {
  switch (intent) {
    case EnterpriseIndexAccessIntent::kEqualityLookup:
      return (BtreeAdjacent(index) || index.index_family == "hash" ||
              index.index_family == "in_memory") &&
             index.equality_lookup_supported;
    case EnterpriseIndexAccessIntent::kOrderedRange:
    case EnterpriseIndexAccessIntent::kOrderedScan:
      return (BtreeAdjacent(index) || index.index_family == "vector_exact" ||
              index.index_family == "temporary_work" ||
              index.index_family == "in_memory") &&
             index.ordered_range_supported;
    case EnterpriseIndexAccessIntent::kNegativePrune:
      return NegativePruneFamily(index) && index.negative_prune_supported;
    case EnterpriseIndexAccessIntent::kCandidateSet:
      return CandidateFamily(index) && index.candidate_set_producer;
    case EnterpriseIndexAccessIntent::kRankedSearch:
      return TextAdjacent(index) && index.candidate_set_producer;
    case EnterpriseIndexAccessIntent::kVectorSearch:
      return VectorFamily(index) && index.candidate_set_producer;
    case EnterpriseIndexAccessIntent::kSpatialProbe:
      return SpatialAdjacent(index) && index.candidate_set_producer;
    case EnterpriseIndexAccessIntent::kDocumentProbe:
      return index.index_family == "document_path" && index.candidate_set_producer;
    case EnterpriseIndexAccessIntent::kGraphSeed:
      return index.index_family == "graph" && index.candidate_set_producer;
  }
  return false;
}

planner::PhysicalAccessKind AccessKindForIntent(const EnterpriseIndexCostRequest& request) {
  switch (request.intent) {
    case EnterpriseIndexAccessIntent::kEqualityLookup:
      return request.index.index_family == "hash"
                 ? planner::PhysicalAccessKind::kScalarHashLookup
                 : planner::PhysicalAccessKind::kScalarBtreeLookup;
    case EnterpriseIndexAccessIntent::kOrderedRange:
    case EnterpriseIndexAccessIntent::kOrderedScan:
      return planner::PhysicalAccessKind::kScalarBtreeRange;
    case EnterpriseIndexAccessIntent::kNegativePrune:
      return planner::PhysicalAccessKind::kBitmapSummaryScan;
    case EnterpriseIndexAccessIntent::kCandidateSet:
      return planner::PhysicalAccessKind::kBitmapSummaryScan;
    case EnterpriseIndexAccessIntent::kRankedSearch:
      return planner::PhysicalAccessKind::kFullTextProbe;
    case EnterpriseIndexAccessIntent::kVectorSearch:
      return request.index.index_family == "vector_exact"
                 ? planner::PhysicalAccessKind::kVectorExactSearch
                 : planner::PhysicalAccessKind::kVectorApproximateWithFallback;
    case EnterpriseIndexAccessIntent::kSpatialProbe:
      return planner::PhysicalAccessKind::kBitmapSummaryScan;
    case EnterpriseIndexAccessIntent::kDocumentProbe:
      return planner::PhysicalAccessKind::kDocumentPathProbe;
    case EnterpriseIndexAccessIntent::kGraphSeed:
      return planner::PhysicalAccessKind::kGraphTraversalSeed;
  }
  return planner::PhysicalAccessKind::kBitmapSummaryScan;
}

double Clamp01(double value) {
  return std::clamp(value, 0.0, 1.0);
}

std::uint64_t CostUnits(double value) {
  if (value <= 0.0) return 0;
  if (value >= static_cast<double>(std::numeric_limits<std::uint64_t>::max())) {
    return std::numeric_limits<std::uint64_t>::max();
  }
  return static_cast<std::uint64_t>(std::ceil(value));
}

std::uint64_t SaturatingAdd(std::uint64_t left, std::uint64_t right) {
  if (right > std::numeric_limits<std::uint64_t>::max() - left) {
    return std::numeric_limits<std::uint64_t>::max();
  }
  return left + right;
}

SelectivityEstimate SelectivityForIndex(const EnterpriseIndexCostRequest& request) {
  SelectivityEstimate estimate;
  estimate.confidence = request.index.identity.confidence;
  estimate.conservative = false;
  const auto rows = std::max<std::uint64_t>(request.table.row_count, 1);
  switch (request.intent) {
    case EnterpriseIndexAccessIntent::kEqualityLookup:
      estimate.selectivity = request.index.distinct_keys == 0
                                  ? 0.10
                                  : std::max(1.0 / static_cast<double>(rows),
                                             1.0 / static_cast<double>(request.index.distinct_keys));
      estimate.diagnostic_code = "SB_OPT_INDEX_COST_SELECTIVITY_EQUALITY";
      break;
    case EnterpriseIndexAccessIntent::kOrderedRange:
      estimate.selectivity = request.requested_range_fraction > 0.0
                                  ? Clamp01(request.requested_range_fraction)
                                  : std::clamp(0.25 * request.index.predicate_coverage, 0.01, 1.0);
      estimate.diagnostic_code = "SB_OPT_INDEX_COST_SELECTIVITY_RANGE";
      break;
    case EnterpriseIndexAccessIntent::kOrderedScan:
      estimate.selectivity = 1.0;
      estimate.diagnostic_code = "SB_OPT_INDEX_COST_SELECTIVITY_ORDERED_SCAN";
      break;
    case EnterpriseIndexAccessIntent::kNegativePrune:
      estimate.selectivity = std::clamp(request.index.predicate_coverage *
                                            (1.0 - request.index.false_positive_ratio),
                                        0.01,
                                        1.0);
      estimate.conservative = request.index.false_positive_ratio > 0.0;
      estimate.diagnostic_code = "SB_OPT_INDEX_COST_SELECTIVITY_NEGATIVE_PRUNE";
      break;
    case EnterpriseIndexAccessIntent::kCandidateSet:
    case EnterpriseIndexAccessIntent::kSpatialProbe:
    case EnterpriseIndexAccessIntent::kDocumentProbe:
    case EnterpriseIndexAccessIntent::kGraphSeed:
      estimate.selectivity = std::clamp(request.index.predicate_coverage *
                                            (1.0 + request.index.false_positive_ratio),
                                        0.01,
                                        1.0);
      estimate.conservative = true;
      estimate.diagnostic_code = "SB_OPT_INDEX_COST_SELECTIVITY_CANDIDATE_SET";
      break;
    case EnterpriseIndexAccessIntent::kRankedSearch:
      estimate.selectivity = std::clamp(0.10 + request.index.false_positive_ratio,
                                        0.001,
                                        1.0);
      estimate.conservative = true;
      estimate.diagnostic_code = "SB_OPT_INDEX_COST_SELECTIVITY_RANKED_SEARCH";
      break;
    case EnterpriseIndexAccessIntent::kVectorSearch:
      estimate.selectivity = request.requested_limit != 0
                                  ? std::clamp(static_cast<double>(request.requested_limit) /
                                                   static_cast<double>(rows),
                                               1.0 / static_cast<double>(rows),
                                               1.0)
                                  : std::clamp(0.05 + request.index.false_positive_ratio,
                                               0.001,
                                               1.0);
      estimate.conservative = VectorApproximate(request.index);
      estimate.diagnostic_code = "SB_OPT_INDEX_COST_SELECTIVITY_VECTOR";
      break;
  }
  estimate.selectivity = Clamp01(estimate.selectivity);
  return estimate;
}

std::uint64_t RowsAfterSelectivity(std::uint64_t rows, double selectivity) {
  if (rows == 0 || selectivity <= 0.0) return 0;
  return std::max<std::uint64_t>(
      1,
      CostUnits(static_cast<double>(rows) * Clamp01(selectivity)));
}

std::uint64_t RowsPerLeafPage(const EnterpriseIndexCostRequest& request) {
  const auto row_bytes = std::max<std::uint64_t>(request.table.average_row_bytes, 1);
  return std::max<std::uint64_t>(1, 8192 / row_bytes);
}

CostFormulaInput BuildCostFormulaInput(const EnterpriseIndexCostRequest& request,
                                       const EnterpriseIndexCostResult& partial) {
  CostFormulaInput input;
  input.access_kind = AccessKindForIntent(request);
  input.estimated_rows = partial.estimated_rows;
  input.row_width_bytes = std::max<std::uint64_t>(request.table.average_row_bytes, 1);
  input.index_probe_count = request.intent == EnterpriseIndexAccessIntent::kOrderedScan
                                ? 1
                                : std::max<std::uint64_t>(1, request.index.height);
  input.index_tuple_visits = partial.estimated_rows;
  input.visibility_recheck_rows = partial.recheck_rows;
  input.false_positive_rows = partial.false_positive_rows;
  input.required_memory_bytes =
      SaturatingAdd(partial.estimated_rows, partial.false_positive_rows) *
      input.row_width_bytes;
  input.input_confidence = partial.selectivity.confidence;
  input.heap_correlation = request.index.clustering_factor;
  input.cache_hit_ratio = std::clamp(1.0 - request.index.contention_ratio, 0.0, 1.0);
  input.reason = "enterprise_index_cost:" + request.index.index_family + ":" +
                 EnterpriseIndexAccessIntentName(request.intent);

  const auto leaf_pages = std::max<std::uint64_t>(request.index.leaf_pages, 1);
  const auto rows_per_page = RowsPerLeafPage(request);
  const auto touched_leaf_pages =
      std::min<std::uint64_t>(leaf_pages,
                              std::max<std::uint64_t>(1, partial.estimated_rows / rows_per_page));
  if (request.intent == EnterpriseIndexAccessIntent::kOrderedRange ||
      request.intent == EnterpriseIndexAccessIntent::kOrderedScan ||
      NegativePruneFamily(request.index) ||
      request.index.index_family == "columnar_zone") {
    input.sequential_pages = touched_leaf_pages;
    input.random_pages = std::max<std::uint64_t>(1, request.index.height);
    input.prefetch_pages = touched_leaf_pages / 2;
  } else {
    input.random_pages = SaturatingAdd(std::max<std::uint64_t>(1, request.index.height),
                                       touched_leaf_pages);
  }
  return input;
}

std::string FamilyBlockerDiagnostic(const IndexStats& index) {
  const auto lookup = idx::FindBuiltinIndexFamilyById(index.index_family);
  if (!lookup.ok()) return "SB_OPT_INDEX_COST_UNKNOWN_FAMILY";
  const auto* capability =
      idx::FindBuiltinIndexFamilyPhysicalCapabilityState(lookup.descriptor->family);
  if (capability == nullptr || !capability->runtime_available ||
      capability->blocker != idx::IndexFamilyPhysicalCapabilityBlocker::none) {
    return capability == nullptr || capability->blocker_diagnostic_code.empty()
               ? "SB_OPT_INDEX_COST_FAMILY_NOT_RUNTIME_AVAILABLE"
               : capability->blocker_diagnostic_code;
  }
  if (lookup.descriptor->persistence == idx::IndexPersistenceClass::donor_emulated) {
    return "SB_OPT_INDEX_COST_DONOR_EMULATED_BLOCKED";
  }
  if (lookup.descriptor->persistence == idx::IndexPersistenceClass::policy_blocked) {
    return "SB_OPT_INDEX_COST_POLICY_BLOCKED";
  }
  return {};
}

}  // namespace

const char* EnterpriseIndexAccessIntentName(EnterpriseIndexAccessIntent intent) {
  switch (intent) {
    case EnterpriseIndexAccessIntent::kEqualityLookup:
      return "equality_lookup";
    case EnterpriseIndexAccessIntent::kOrderedRange:
      return "ordered_range";
    case EnterpriseIndexAccessIntent::kOrderedScan:
      return "ordered_scan";
    case EnterpriseIndexAccessIntent::kNegativePrune:
      return "negative_prune";
    case EnterpriseIndexAccessIntent::kCandidateSet:
      return "candidate_set";
    case EnterpriseIndexAccessIntent::kRankedSearch:
      return "ranked_search";
    case EnterpriseIndexAccessIntent::kVectorSearch:
      return "vector_search";
    case EnterpriseIndexAccessIntent::kSpatialProbe:
      return "spatial_probe";
    case EnterpriseIndexAccessIntent::kDocumentProbe:
      return "document_probe";
    case EnterpriseIndexAccessIntent::kGraphSeed:
      return "graph_seed";
  }
  return "equality_lookup";
}

EnterpriseIndexCostResult EstimateEnterpriseIndexAccessCost(
    const EnterpriseIndexCostRequest& request) {
  if (request.index.index_uuid.empty() || request.index.index_family.empty()) {
    return Refuse(request,
                  "SB_OPT_INDEX_COST_IDENTITY_REQUIRED",
                  "index identity required");
  }
  if (!AuthorityPresent(request.authority)) {
    return Refuse(request,
                  "SB_OPT_INDEX_COST_AUTHORITY_REQUIRED",
                  "optimizer/catalog/stats/generated-readiness/route/recheck authority required");
  }
  if (ReadinessEvidenceUnsafe(request.authority)) {
    return Refuse(request,
                  "SB_OPT_INDEX_COST_READINESS_EVIDENCE_UNSAFE",
                  "generated readiness manifest must be current runtime proof, not static/smoke/synthetic evidence");
  }
  if (UnsafeAuthority(request.authority)) {
    return Refuse(request,
                  "SB_OPT_INDEX_COST_UNSAFE_AUTHORITY",
                  "unsafe authority refused");
  }
  const auto blocker = FamilyBlockerDiagnostic(request.index);
  if (!blocker.empty()) {
    return Refuse(request, blocker, "family blocked from enterprise index costing");
  }
  if (!OptimizerStatsIdentityIsUsable(request.index.identity) ||
      !OptimizerStatsIdentityIsUsable(request.table.identity)) {
    return Refuse(request,
                  "SB_OPT_INDEX_COST_STATS_UNUSABLE",
                  "fresh catalog-backed table and index stats required");
  }
  if (request.require_benchmark_clean && !request.index.route_benchmark_clean) {
    return Refuse(request,
                  "SB_OPT_INDEX_COST_ROUTE_NOT_BENCHMARK_CLEAN",
                  "route benchmark clean proof required");
  }
  if (request.index.rebuild_in_progress) {
    return Refuse(request,
                  "SB_OPT_INDEX_COST_REBUILD_IN_PROGRESS",
                  "index rebuild in progress");
  }
  if (!request.index.exact_recheck_required ||
      !request.index.mga_recheck_required ||
      !request.index.security_recheck_required) {
    return Refuse(request,
                  "SB_OPT_INDEX_COST_RECHECK_REQUIRED",
                  "exact MGA security recheck proof required");
  }
  if ((VectorApproximate(request.index) ||
       request.index.index_family == "sparse_wand") &&
      !request.authority.exact_rerank_proven) {
    return Refuse(request,
                  "SB_OPT_INDEX_COST_EXACT_RERANK_REQUIRED",
                  "approximate/ranked route requires exact rerank proof");
  }
  if (request.index.index_family == "bloom" &&
      request.intent != EnterpriseIndexAccessIntent::kNegativePrune &&
      request.intent != EnterpriseIndexAccessIntent::kCandidateSet) {
    return Refuse(request,
                  "SB_OPT_INDEX_COST_BLOOM_NEGATIVE_PRUNE_ONLY",
                  "Bloom index is negative-prune/candidate-only");
  }
  if (!IntentAllowedByFamily(request.index, request.intent)) {
    return Refuse(request,
                  "SB_OPT_INDEX_COST_INTENT_UNSUPPORTED",
                  EnterpriseIndexAccessIntentName(request.intent));
  }

  EnterpriseIndexCostResult result;
  result.accepted = true;
  result.selectable = true;
  result.intent = request.intent;
  result.family_id = request.index.index_family;
  result.diagnostic_code = "SB_OPT_INDEX_COST_OK";
  result.selectivity = SelectivityForIndex(request);
  result.estimated_rows = RowsAfterSelectivity(request.table.row_count,
                                               result.selectivity.selectivity);
  if (request.requested_limit != 0) {
    result.estimated_rows = std::min(result.estimated_rows, request.requested_limit);
  }
  result.false_positive_rows =
      RowsAfterSelectivity(request.table.row_count, request.index.false_positive_ratio);
  result.recheck_rows =
      request.index.exact_recheck_required
          ? SaturatingAdd(result.estimated_rows, result.false_positive_rows)
          : 0;
  auto input = BuildCostFormulaInput(request, result);
  result.cost = EstimateCostVector(request.environment, input);
  result.cost = ApplyIndexHealthCostAdjustment(std::move(result.cost), request.index);
  result.cost.selectable = result.cost.selectable && result.selectable;
  result.evidence.push_back("family=" + request.index.index_family);
  result.evidence.push_back("intent=" +
                            std::string(EnterpriseIndexAccessIntentName(request.intent)));
  result.evidence.push_back("selectivity_diagnostic=" +
                            result.selectivity.diagnostic_code);
  result.evidence.push_back("exact_recheck_preserved=true");
  result.evidence.push_back("mga_security_recheck_preserved=true");
  result.evidence.push_back("generated_index_readiness_manifest=true");
  result.evidence.push_back("route_runtime_proof=true");
  result.evidence.push_back("operation_metrics_support_bundle_proof=true");
  result.evidence.push_back("crash_cleanup_corruption_storage_proof=true");
  result.evidence.push_back("parser_or_donor_authority=false");
  result.evidence.push_back("cluster_authority=false");
  if (VectorApproximate(request.index) ||
      request.index.index_family == "sparse_wand") {
    result.evidence.push_back("exact_rerank_proven=true");
  }
  return result;
}

std::vector<StatisticsContractStatus> ValidateEnterpriseIndexCostingFamilyMatrix() {
  std::vector<StatisticsContractStatus> statuses;
  for (const auto& descriptor : idx::BuiltinIndexFamilyDescriptors()) {
    if (descriptor.persistence == idx::IndexPersistenceClass::donor_emulated) {
      statuses.push_back(Status(true,
                                "SB_OPT_INDEX_COST_DONOR_EMULATED_CLAIM_REMOVED",
                                descriptor.id));
      continue;
    }
    if (descriptor.persistence == idx::IndexPersistenceClass::policy_blocked) {
      statuses.push_back(Status(true,
                                "SB_OPT_INDEX_COST_POLICY_BLOCKED_CLAIM_REMOVED",
                                descriptor.id));
      continue;
    }
    const auto* capability =
        idx::FindBuiltinIndexFamilyPhysicalCapabilityState(descriptor.family);
    if (capability == nullptr || !capability->runtime_available ||
        !capability->benchmark_clean ||
        capability->blocker != idx::IndexFamilyPhysicalCapabilityBlocker::none) {
      statuses.push_back(Status(false,
                                "SB_OPT_INDEX_COST_FAMILY_RUNTIME_CAPABILITY_MISSING",
                                descriptor.id));
      continue;
    }
    const bool costed =
        BtreeAdjacent(IndexStats{.index_family = descriptor.id}) ||
        descriptor.id == "hash" ||
        CandidateFamily(IndexStats{.index_family = descriptor.id}) ||
        NegativePruneFamily(IndexStats{.index_family = descriptor.id}) ||
        VectorFamily(IndexStats{.index_family = descriptor.id}) ||
        TextAdjacent(IndexStats{.index_family = descriptor.id}) ||
        SpatialAdjacent(IndexStats{.index_family = descriptor.id}) ||
        descriptor.id == "document_path" ||
        descriptor.id == "graph";
    if (!costed) {
      statuses.push_back(Status(false,
                                "SB_OPT_INDEX_COST_FAMILY_PROFILE_MISSING",
                                descriptor.id));
    }
  }
  const bool has_failure =
      std::any_of(statuses.begin(), statuses.end(), [](const auto& status) {
        return !status.ok;
      });
  if (!has_failure) {
    statuses.push_back(Status(true,
                              "SB_OPT_INDEX_COST_FAMILY_MATRIX_OK",
                              "builtin_noncluster_families"));
  }
  return statuses;
}

}  // namespace scratchbird::engine::optimizer

// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "access_path_full.hpp"

#include "predicate_normalization.hpp"
#include "selectivity_model.hpp"

#include <algorithm>
#include <string>
#include <utility>

namespace scratchbird::engine::optimizer {
namespace planner = scratchbird::engine::planner;
namespace {

PlanCandidate MakeAccessCandidate(std::string id,
                                  planner::PhysicalAccessKind kind,
                                  CostVector cost,
                                  std::uint64_t rows,
                                  std::vector<std::string> required,
                                  std::vector<std::string> missing = {}) {
  PlanCandidate candidate;
  candidate.candidate_id = std::move(id);
  candidate.access_kind = kind;
  candidate.cost = std::move(cost);
  candidate.estimated_rows = rows;
  candidate.required_facts = std::move(required);
  candidate.missing_facts = std::move(missing);
  if (!candidate.missing_facts.empty()) {
    candidate.refusal_reasons = candidate.missing_facts;
    candidate.cost.selectable = false;
    candidate.cost.confidence = CostConfidence::kRejected;
    candidate.cost.rejection_reason = candidate.missing_facts.front();
  } else {
    candidate.acceptance_reasons = candidate.required_facts;
  }
  return candidate;
}

CostVector CostFor(planner::PhysicalAccessKind kind) {
  return EstimateNodeCost(planner::MakeLogicalPlanNode(planner::LogicalPlanNodeKind::kDmlRead, kind, "dml.select_rows", planner::PhysicalAccessKindName(kind)));
}

bool IsSpecializedPredicate(const std::string& predicate_kind, const std::string& index_family) {
  return (predicate_kind == "full_text" && index_family == "full_text") ||
         ((predicate_kind == "vector_exact" || predicate_kind == "vector_approx") && index_family == "vector") ||
         (predicate_kind == "document_path" && index_family == "document") ||
         (predicate_kind == "graph_seed" && index_family == "graph") ||
         (predicate_kind == "timeseries_append" && index_family == "timeseries");
}

bool VectorContains(const std::vector<std::string>& values, const std::string& value) {
  return std::find(values.begin(), values.end(), value) != values.end();
}

bool VectorPrefixMatches(const std::vector<std::string>& prefix, const std::vector<std::string>& values) {
  if (prefix.empty() || values.size() < prefix.size()) return false;
  for (std::size_t i = 0; i < prefix.size(); ++i) {
    if (prefix[i] != values[i]) return false;
  }
  return true;
}

CostVector SelectableBitmapSummaryCost(std::string reason) {
  auto cost = CostFor(planner::PhysicalAccessKind::kScalarBtreeRange);
  cost.reason = std::move(reason);
  cost.confidence = CostConfidence::kMedium;
  cost.selectable = true;
  cost.rejection_reason.clear();
  return cost;
}

std::vector<std::string> CoveringRefusalReasons(const IndexStats& index,
                                                const std::vector<std::string>& projected_column_uuids,
                                                bool index_visibility_native,
                                                const CoveringPayloadPlanningProof& payload) {
  std::vector<std::string> reasons;
  if (!OptimizerStatsIdentityIsUsable(index.identity) || index.rebuild_in_progress) {
    reasons.push_back("covering_index_rebuild_or_stale");
  }
  if (!index.covering) {
    reasons.push_back("covering_index_not_covering");
  }
  if (projected_column_uuids.empty()) {
    reasons.push_back("covering_projection_not_covered");
  } else if (!index.covered_column_uuids.empty()) {
    for (const auto& column_uuid : projected_column_uuids) {
      if (!VectorContains(index.covered_column_uuids, column_uuid)) {
        reasons.push_back("covering_projection_not_covered");
        break;
      }
    }
  }
  if (!index_visibility_native || index.visibility_coverage < 1.0) {
    reasons.push_back("covering_visibility_index_native_proof_missing");
  }
  if (!payload.physical_payload_proof_present) {
    reasons.push_back("covering_payload_proof_missing");
  } else {
    if (!payload.freshness_proven) {
      reasons.push_back("covering_payload_freshness_unproven");
    }
    if (!payload.redaction_safe) {
      reasons.push_back("covering_payload_redaction_epoch_unproven");
    }
    if (!payload.result_contract_proven) {
      reasons.push_back("covering_payload_result_contract_unproven");
    }
    if (!payload.index_only_admitted &&
        !payload.base_row_recheck_handoff_proven) {
      reasons.push_back("covering_base_row_recheck_handoff_missing");
    }
    if (payload.runtime_route_consumption_required) {
      reasons.push_back("covering_runtime_route_consumption_pending");
    }
  }
  reasons.insert(reasons.end(), payload.blockers.begin(), payload.blockers.end());
  return reasons;
}

void AttachCoveringPayloadEvidence(PlanCandidate* candidate,
                                   const CoveringPayloadPlanningProof& payload,
                                   const std::vector<std::string>& refused) {
  if (!payload.physical_payload_proof_present) return;
  candidate->runtime_evidence.push_back("covering_payload.physical_payload_proof_present=true");
  candidate->runtime_evidence.push_back(std::string("covering_payload.freshness_proven=") +
                                        (payload.freshness_proven ? "true" : "false"));
  candidate->runtime_evidence.push_back(std::string("covering_payload.redaction_safe=") +
                                        (payload.redaction_safe ? "true" : "false"));
  candidate->runtime_evidence.push_back(std::string("covering_payload.result_contract_proven=") +
                                        (payload.result_contract_proven ? "true" : "false"));
  candidate->runtime_evidence.push_back(std::string("covering_payload.base_row_recheck_handoff_proven=") +
                                        (payload.base_row_recheck_handoff_proven ? "true" : "false"));
  candidate->runtime_evidence.push_back(std::string("covering_payload.index_only_admitted=") +
                                        (payload.index_only_admitted ? "true" : "false"));
  candidate->runtime_evidence.push_back(std::string("covering_payload.runtime_route_consumption_pending=") +
                                        (payload.runtime_route_consumption_required ? "true" : "false"));
  candidate->runtime_evidence.insert(candidate->runtime_evidence.end(),
                                     payload.evidence.begin(),
                                     payload.evidence.end());
  for (const auto& reason : refused) {
    candidate->runtime_evidence.push_back("covering_payload.blocker=" + reason);
  }
}

bool IsBitmapCompatibleIndex(const IndexStats& index, const std::string& predicate_kind) {
  if (!OptimizerStatsIdentityIsUsable(index.identity) || index.rebuild_in_progress) return false;
  if (index.index_family != "btree" && index.index_family != "hash") return false;
  return predicate_kind == "scalar_eq" ||
         predicate_kind == "unique_eq" ||
         predicate_kind == "scalar_range" ||
         predicate_kind == "row_uuid_eq";
}

bool HasCanonicalPredicateMetadata(const IndexStats& index, const AccessPathPlanningRequest& request) {
  return (request.sblr_expression.has_value() || !request.predicate_text.empty()) &&
         (index.expression_index ||
          !index.key_expression_digests.empty() ||
          !index.generated_column_expression_digest.empty() ||
          !index.computed_expression_digest.empty() ||
          index.partial ||
          index.like_prefix_capable ||
          !index.descriptor_digest.empty() ||
          !index.collation_identity.empty());
}

PredicateIndexMatchResult MatchCanonicalPredicate(const IndexStats& index,
                                                  const AccessPathPlanningRequest& request) {
  if (request.sblr_expression.has_value()) {
    SblrExpressionIndexMatchRequest match_request;
    match_request.query_expression = *request.sblr_expression;
    match_request.descriptor_digest = request.descriptor_digest;
    match_request.collation_identity = request.collation_identity;
    match_request.base_row_mga_recheck_planned =
        request.base_row_mga_recheck_planned;
    match_request.base_row_security_recheck_planned =
        request.base_row_security_recheck_planned;
    match_request.index = index;
    const auto sblr_match = MatchSblrExpressionToIndex(match_request);
    PredicateIndexMatchResult result;
    result.matches = sblr_match.matches;
    result.canonical_predicate_text = sblr_match.canonical_expression_text;
    result.canonical_predicate_digest = sblr_match.canonical_expression_digest;
    result.acceptance_reasons = sblr_match.acceptance_reasons;
    result.acceptance_reasons.insert(result.acceptance_reasons.end(),
                                     sblr_match.evidence.begin(),
                                     sblr_match.evidence.end());
    result.refusal_reasons = sblr_match.refusal_reasons;
    return result;
  }
  if (request.predicate_text.empty()) {
    PredicateIndexMatchResult result;
    result.refusal_reasons.push_back(
        "canonical_predicate_or_sblr_expression_missing");
    return result;
  }
  PredicateIndexMatchRequest match_request;
  match_request.query_predicate_text = request.predicate_text;
  match_request.descriptor_digest = request.descriptor_digest;
  match_request.collation_identity = request.collation_identity;
  match_request.base_row_mga_recheck_planned = request.base_row_mga_recheck_planned;
  match_request.base_row_security_recheck_planned = request.base_row_security_recheck_planned;
  match_request.index = index;
  return MatchPredicateToIndex(match_request);
}

planner::PhysicalAccessKind SpecializedAccessKind(const std::string& predicate_kind) {
  if (predicate_kind == "full_text") return planner::PhysicalAccessKind::kFullTextProbe;
  if (predicate_kind == "vector_exact") return planner::PhysicalAccessKind::kVectorExactSearch;
  if (predicate_kind == "vector_approx") return planner::PhysicalAccessKind::kVectorApproximateWithFallback;
  if (predicate_kind == "document_path") return planner::PhysicalAccessKind::kDocumentPathProbe;
  if (predicate_kind == "graph_seed") return planner::PhysicalAccessKind::kGraphTraversalSeed;
  if (predicate_kind == "timeseries_append") return planner::PhysicalAccessKind::kTimeSeriesAppendPath;
  return planner::PhysicalAccessKind::kNone;
}

void AttachPartitionSegmentPruneEvidence(
    std::vector<PlanCandidate>* candidates,
    const OptimizerPartitionSegmentPrunePlan& pruning_plan) {
  if (!pruning_plan.pruning_evaluated) return;
  for (auto& candidate : *candidates) {
    candidate.partition_segment_prune_evidence = pruning_plan.evidence;
    candidate.acceptance_reasons.insert(candidate.acceptance_reasons.end(),
                                        pruning_plan.evidence.acceptance_reasons.begin(),
                                        pruning_plan.evidence.acceptance_reasons.end());
    candidate.statistics_diagnostics.push_back(
        "partition_segment_prune_pages_considered=" +
        std::to_string(pruning_plan.evidence.pages_considered));
    candidate.statistics_diagnostics.push_back(
        "partition_segment_prune_pages_pruned=" +
        std::to_string(pruning_plan.evidence.pages_pruned));
    candidate.statistics_diagnostics.push_back(
        "partition_segment_prune_pages_scanned=" +
        std::to_string(pruning_plan.evidence.pages_scanned));
  }
}

}  // namespace

bool IndexCanSatisfyPredicate(const IndexStats& index, const std::string& predicate_kind) {
  if (!OptimizerStatsIdentityIsUsable(index.identity)) return false;
  if (index.rebuild_in_progress) return false;
  if (IsSpecializedPredicate(predicate_kind, index.index_family)) return true;
  if ((predicate_kind.empty() || predicate_kind == "ordered_limit") &&
      index.index_family == "btree" && index.leaf_pages > 0) return true;
  if ((predicate_kind == "scalar_eq" || predicate_kind == "unique_eq") &&
      (index.index_family == "btree" || index.index_family == "hash") &&
      (index.unique || index.distinct_keys > 0)) return true;
  if (predicate_kind == "scalar_range") return index.index_family == "btree" && index.leaf_pages > 0;
  if (predicate_kind == "like_prefix") return index.index_family == "btree" && index.leaf_pages > 0;
  if (predicate_kind == "row_uuid_eq") return index.unique;
  return false;
}

bool IndexCanSatisfyOrdering(const IndexStats& index, const OrderedLimitPlanningRequest& ordered_limit) {
  if (!ordered_limit.present) return false;
  if (index.index_family != "btree") return false;
  if (!OptimizerStatsIdentityIsUsable(index.identity) || index.rebuild_in_progress) return false;
  return VectorPrefixMatches(ordered_limit.order_by_column_uuids, index.key_column_uuids);
}

bool IndexCanCoverProjection(const IndexStats& index, const std::vector<std::string>& projected_column_uuids) {
  if (!index.covering || projected_column_uuids.empty() || index.rebuild_in_progress ||
      !OptimizerStatsIdentityIsUsable(index.identity)) {
    return false;
  }
  if (index.covered_column_uuids.empty()) return true;
  return std::all_of(projected_column_uuids.begin(), projected_column_uuids.end(), [&](const std::string& column_uuid) {
    return VectorContains(index.covered_column_uuids, column_uuid);
  });
}

std::vector<PlanCandidate> GenerateFullAccessPathCandidates(const AccessPathPlanningRequest& request) {
  std::vector<PlanCandidate> candidates;
  const std::uint64_t base_rows = request.table_stats ? request.table_stats->visible_row_count : 1000;
  const std::uint64_t base_pages = request.table_stats ? request.table_stats->page_count : 64;
  auto partition_segment_prune = request.partition_segment_prune;
  partition_segment_prune.base_row_mga_recheck_planned =
      partition_segment_prune.base_row_mga_recheck_planned &&
      request.base_row_mga_recheck_planned;
  partition_segment_prune.base_row_security_recheck_planned =
      partition_segment_prune.base_row_security_recheck_planned &&
      request.base_row_security_recheck_planned;
  const auto pruning_plan = PlanPartitionSegmentPruning(partition_segment_prune);
  const std::uint64_t rows = EstimateRowsAfterPartitionSegmentPruning(base_rows, pruning_plan);
  const std::uint64_t pages = EstimatePagesAfterPartitionSegmentPruning(base_pages, pruning_plan);

  auto scan_cost = CostFor(planner::PhysicalAccessKind::kTableScan);
  scan_cost.row_cost += rows / 10;
  scan_cost.io_cost += pages;
  scan_cost.total_cost = scan_cost.startup_cost + scan_cost.row_cost + scan_cost.io_cost + scan_cost.memory_cost + scan_cost.uncertainty_cost;
  candidates.push_back(MakeAccessCandidate("CAND-OPT-FULL-SCAN", planner::PhysicalAccessKind::kTableScan, scan_cost, rows,
                                           {"relation_uuid", "visibility_rules", "table_stats"},
                                           request.visibility_proven && request.grants_proven ? std::vector<std::string>{} : std::vector<std::string>{"visibility_or_grants_not_proven"}));

  if (request.predicate_kind == "row_uuid_eq") {
    candidates.push_back(MakeAccessCandidate("CAND-OPT-ROW-UUID", planner::PhysicalAccessKind::kRowUuidLookup, CostFor(planner::PhysicalAccessKind::kRowUuidLookup), 1,
                                             {"row_uuid_predicate", "relation_uuid"}, request.visibility_proven ? std::vector<std::string>{} : std::vector<std::string>{"visibility_not_proven"}));
  }

  for (const auto& index : request.candidate_indexes) {
    const bool canonical_metadata = HasCanonicalPredicateMetadata(index, request);
    const auto canonical_match = canonical_metadata
                                     ? MatchCanonicalPredicate(index, request)
                                     : PredicateIndexMatchResult{};
    if (!IndexCanSatisfyPredicate(index, request.predicate_kind) ||
        (canonical_metadata && !canonical_match.matches)) {
      const auto reason = (!OptimizerStatsIdentityIsUsable(index.identity) || index.rebuild_in_progress)
                              ? "index_rebuild_or_stale"
                              : (canonical_metadata && !canonical_match.refusal_reasons.empty()
                                     ? canonical_match.refusal_reasons.front()
                                     : "index_predicate_mismatch");
      auto refusal_reasons = canonical_metadata && !canonical_match.refusal_reasons.empty()
                                 ? canonical_match.refusal_reasons
                                 : std::vector<std::string>{reason};
      auto rejected = MakeAccessCandidate("CAND-OPT-INDEX-REFUSED:" + index.index_uuid, planner::PhysicalAccessKind::kScalarBtreeLookup, RejectedCost(reason), rows,
                                          {"index_uuid", "predicate_compatibility"}, refusal_reasons);
      candidates.push_back(std::move(rejected));
      auto covering_refused = CoveringRefusalReasons(index,
                                                     request.projected_column_uuids,
                                                     request.index_visibility_native,
                                                     request.covering_payload);
      if (covering_refused.empty()) covering_refused = refusal_reasons;
      auto covering = MakeAccessCandidate("CAND-OPT-COVERING:" + index.index_uuid,
                                          planner::PhysicalAccessKind::kCoveringIndexScan,
                                          RejectedCost(covering_refused.front()),
                                          rows,
                                          {"covering_projection_covered",
                                           "covering_index_covering",
                                           "covering_visibility_index_native_proof"},
                                          covering_refused);
      AttachCoveringPayloadEvidence(&covering, request.covering_payload,
                                    covering_refused);
      candidates.push_back(std::move(covering));
      continue;
    }
    if (IsSpecializedPredicate(request.predicate_kind, index.index_family)) {
      const auto kind = SpecializedAccessKind(request.predicate_kind);
      auto specialized_cost = ApplyIndexHealthCostAdjustment(CostFor(kind), index);
      candidates.push_back(MakeAccessCandidate("CAND-OPT-SPECIALIZED:" + index.index_uuid, kind, specialized_cost, std::max<std::uint64_t>(1, rows / 10),
                                               {"specialized_index_family", "predicate_compatibility", "descriptor_compatibility"},
                                               request.grants_proven ? std::vector<std::string>{} : std::vector<std::string>{"grants_not_proven"}));
      continue;
    }
    const bool equality = request.predicate_kind == "scalar_eq" || request.predicate_kind == "unique_eq" || request.predicate_kind == "row_uuid_eq";
    auto kind = equality
                    ? (index.index_family == "hash" ? planner::PhysicalAccessKind::kScalarHashLookup : planner::PhysicalAccessKind::kScalarBtreeLookup)
                    : planner::PhysicalAccessKind::kScalarBtreeRange;
    auto cost = ApplyIndexHealthCostAdjustment(CostFor(kind), index);
    PredicateSelectivityInput selectivity_input;
    selectivity_input.predicate_kind = request.predicate_kind == "row_uuid_eq" || index.unique ? "unique_eq" :
                                       (equality ? "scalar_eq" : "scalar_range");
    selectivity_input.input_rows = rows;
    selectivity_input.distinct_values = index.distinct_keys;
    selectivity_input.has_histogram = false;
    selectivity_input.range_fraction = 0.25;
    selectivity_input.input_confidence = index.identity.confidence;
    const auto selectivity = EstimatePredicateSelectivity(selectivity_input);
    const std::uint64_t estimate = EstimateRowsAfterSelectivity(rows, selectivity);
    candidates.push_back(MakeAccessCandidate("CAND-OPT-INDEX:" + index.index_uuid, kind, cost, estimate,
                                             {"index_uuid", "predicate_compatibility", "descriptor_compatibility", "index_depth", "index_leaf_pages", "index_fragmentation_ratio", "index_coverage"},
                                             request.grants_proven ? std::vector<std::string>{} : std::vector<std::string>{"grants_not_proven"}));
    if (canonical_metadata && candidates.back().cost.selectable) {
      candidates.back().acceptance_reasons.insert(candidates.back().acceptance_reasons.end(),
                                                  canonical_match.acceptance_reasons.begin(),
                                                  canonical_match.acceptance_reasons.end());
    }

    if (request.ordered_limit.present) {
      std::vector<std::string> refused;
      if (!IndexCanSatisfyOrdering(index, request.ordered_limit)) {
        refused.push_back("ordered_limit_index_order_mismatch");
      }
      if (request.ordered_limit.limit_count == 0) {
        refused.push_back("ordered_limit_bound_missing");
      }
      auto ordered_cost = CostFor(planner::PhysicalAccessKind::kScalarBtreeRange);
      ordered_cost.reason = "ordered_limit_index_scan";
      if (refused.empty()) {
        ordered_cost.startup_cost = 2;
        ordered_cost.row_cost = std::max<std::uint64_t>(1, request.ordered_limit.limit_count / 16);
        ordered_cost.io_cost = std::max<std::uint64_t>(1, index.height);
        ordered_cost.memory_cost = 1;
        ordered_cost.uncertainty_cost = 0;
        ordered_cost.total_cost = ordered_cost.startup_cost + ordered_cost.row_cost + ordered_cost.io_cost + ordered_cost.memory_cost;
      }
      auto ordered = MakeAccessCandidate("CAND-OPT-ORDERED-LIMIT:" + index.index_uuid,
                                         planner::PhysicalAccessKind::kScalarBtreeRange,
                                         refused.empty() ? ordered_cost : RejectedCost(refused.front()),
                                         refused.empty() ? std::max<std::uint64_t>(1, request.ordered_limit.limit_count) : estimate,
                                         {"ordered_limit_index_order_satisfied", "ordered_limit_bound_applied", "ordered_limit_preserves_mga_recheck"},
                                         refused);
      ordered.ordered_limit_evidence.present = true;
      ordered.ordered_limit_evidence.index_uuid = index.index_uuid;
      ordered.ordered_limit_evidence.order_by_column_uuids = request.ordered_limit.order_by_column_uuids;
      ordered.ordered_limit_evidence.limit_count = request.ordered_limit.limit_count;
      ordered.ordered_limit_evidence.index_order_satisfied = refused.empty();
      ordered.ordered_limit_evidence.sort_avoided = refused.empty();
      if (refused.empty()) {
        ordered.acceptance_reasons = {"ordered_limit_index_order_satisfied",
                                      "ordered_limit_bound_applied",
                                      "ordered_limit_sort_avoided",
                                      "ordered_limit_preserves_mga_recheck"};
      }
      candidates.push_back(std::move(ordered));
    }

    {
      auto refused = CoveringRefusalReasons(index,
                                            request.projected_column_uuids,
                                            request.index_visibility_native,
                                            request.covering_payload);
      auto covering_cost = refused.empty()
                               ? CostFor(planner::PhysicalAccessKind::kCoveringIndexScan)
                               : RejectedCost(refused.front());
      auto covering = MakeAccessCandidate("CAND-OPT-COVERING:" + index.index_uuid,
                                          planner::PhysicalAccessKind::kCoveringIndexScan,
                                          std::move(covering_cost),
                                          estimate,
                                          {"covering_projection_covered",
                                           "covering_index_covering",
                                           "covering_visibility_index_native_proof"},
                                          refused);
      if (refused.empty()) {
        covering.acceptance_reasons = {"covering_projection_covered",
                                       "covering_index_covering",
                                       "covering_visibility_index_native_proof"};
      }
      AttachCoveringPayloadEvidence(&covering, request.covering_payload,
                                    refused);
      candidates.push_back(std::move(covering));
    }
  }

  if (request.bitmap.requested || request.candidate_indexes.size() > 1) {
    const auto compatible_indexes = std::count_if(request.candidate_indexes.begin(),
                                                 request.candidate_indexes.end(),
                                                 [&](const IndexStats& index) {
                                                   return IsBitmapCompatibleIndex(index, request.predicate_kind);
                                                 });
    const bool has_rebuild_or_stale = std::any_of(request.candidate_indexes.begin(),
                                                  request.candidate_indexes.end(),
                                                  [](const IndexStats& index) {
                                                    return !OptimizerStatsIdentityIsUsable(index.identity) ||
                                                           index.rebuild_in_progress;
                                                  });
    std::vector<std::string> refused;
    if (!request.bitmap.executor_supported) refused.push_back("bitmap_executor_not_supported");
    if (request.predicate_kind != "scalar_eq" &&
        request.predicate_kind != "unique_eq" &&
        request.predicate_kind != "scalar_range" &&
        request.predicate_kind != "row_uuid_eq") {
      refused.push_back("bitmap_unsupported_predicate");
    }
    if (compatible_indexes < 2) refused.push_back("bitmap_not_enough_compatible_indexes");
    if (has_rebuild_or_stale) refused.push_back("bitmap_index_rebuild_or_stale");
    auto bitmap = MakeAccessCandidate("CAND-OPT-BITMAP",
                                      planner::PhysicalAccessKind::kBitmapSummaryScan,
                                      refused.empty() ? SelectableBitmapSummaryCost("bitmap_index_intersection")
                                                      : RejectedCost(refused.front()),
                                      std::max<std::uint64_t>(1, rows / 3),
                                      {"bitmap_candidate_indexes_compatible",
                                       "bitmap_executor_supported",
                                       "bitmap_mga_recheck_preserved"},
                                      refused);
    if (refused.empty()) {
      bitmap.acceptance_reasons = {"bitmap_candidate_indexes_compatible",
                                   "bitmap_executor_supported",
                                   "bitmap_mga_recheck_preserved"};
    }
    candidates.push_back(std::move(bitmap));
  }

  if (request.summary_prune.requested) {
    std::vector<std::string> refused;
    if (!request.summary_prune.summary_present) refused.push_back("summary_missing");
    if (request.summary_prune.summary_generation != request.summary_prune.relation_generation) {
      refused.push_back("summary_stale_generation_mismatch");
    }
    if (!request.summary_prune.predicate_supported) refused.push_back("summary_unsupported_predicate");
    if (!request.summary_prune.base_row_mga_recheck_planned) refused.push_back("summary_mga_recheck_required");
    if (!request.summary_prune.base_row_security_recheck_planned) refused.push_back("summary_security_recheck_required");
    auto summary = MakeAccessCandidate("CAND-OPT-SUMMARY-PRUNE",
                                       planner::PhysicalAccessKind::kBitmapSummaryScan,
                                       refused.empty() ? SelectableBitmapSummaryCost("summary_page_prune")
                                                       : RejectedCost(refused.front()),
                                       std::max<std::uint64_t>(1, rows / 4),
                                       {"summary_present",
                                        "summary_generation_fresh",
                                        "summary_predicate_supported",
                                        "summary_base_row_mga_recheck_required",
                                        "summary_base_row_security_recheck_required"},
                                       refused);
    if (refused.empty()) {
      summary.acceptance_reasons = {"summary_present",
                                    "summary_generation_fresh",
                                    "summary_predicate_supported",
                                    "summary_base_row_mga_recheck_required",
                                    "summary_base_row_security_recheck_required"};
    }
    summary.summary_prune_evidence.present = true;
    summary.summary_prune_evidence.selected_access = "summary_page_prune";
    summary.summary_prune_evidence.prune_reason = request.summary_prune.predicate_supported
                                                      ? "summary_predicate_supported"
                                                      : "summary_unsupported_predicate";
    summary.summary_prune_evidence.fallback_reason = refused.empty() ? "none" : refused.front();
    summary.summary_prune_evidence.summary_status = request.summary_prune.summary_present ? "present" : "missing";
    summary.summary_prune_evidence.summary_generation = request.summary_prune.summary_generation;
    summary.summary_prune_evidence.candidate_ranges = request.summary_prune.candidate_ranges;
    summary.summary_prune_evidence.ranges_pruned = request.summary_prune.ranges_pruned;
    summary.summary_prune_evidence.ranges_scanned =
        request.summary_prune.candidate_ranges > request.summary_prune.ranges_pruned
            ? request.summary_prune.candidate_ranges - request.summary_prune.ranges_pruned
            : 0;
    summary.summary_prune_evidence.pages_considered = request.summary_prune.pages_considered;
    summary.summary_prune_evidence.pages_pruned = request.summary_prune.pages_pruned;
    summary.summary_prune_evidence.pages_scanned =
        request.summary_prune.pages_considered > request.summary_prune.pages_pruned
            ? request.summary_prune.pages_considered - request.summary_prune.pages_pruned
            : 0;
    summary.summary_prune_evidence.base_row_mga_recheck_required = true;
    summary.summary_prune_evidence.base_row_security_recheck_required = true;
    summary.summary_prune_evidence.summary_metadata_visibility_authority = false;
    summary.summary_prune_evidence.summary_metadata_finality_authority = false;
    candidates.push_back(std::move(summary));
  }
  AttachPartitionSegmentPruneEvidence(&candidates, pruning_plan);
  return candidates;
}

std::vector<PhysicalPlanNode> BuildPhysicalAccessNodes(const std::vector<PlanCandidate>& candidates,
                                                       const std::string& descriptor_digest) {
  std::vector<PhysicalPlanNode> nodes;
  for (const auto& candidate : candidates) {
    nodes.push_back(PhysicalPlanNodeFromCandidate(candidate, RequiredExecutorCapabilityForAccessKind(candidate.access_kind), descriptor_digest));
  }
  return nodes;
}

}  // namespace scratchbird::engine::optimizer

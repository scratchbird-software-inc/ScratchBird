// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "access_path_full.hpp"
#include "logical_plan.hpp"
#include "optimizer_contract.hpp"

#include <algorithm>
#include <iostream>
#include <string>
#include <vector>

namespace opt = scratchbird::engine::optimizer;
namespace plan = scratchbird::engine::planner;

namespace {

bool Require(bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << message << '\n';
    return false;
  }
  return true;
}

bool Has(const std::vector<std::string>& values, const std::string& value) {
  return std::find(values.begin(), values.end(), value) != values.end();
}

const opt::PlanCandidate* FindCandidate(const std::vector<opt::PlanCandidate>& candidates,
                                        const std::string& id) {
  const auto found = std::find_if(candidates.begin(), candidates.end(), [&](const opt::PlanCandidate& candidate) {
    return candidate.candidate_id == id;
  });
  return found == candidates.end() ? nullptr : &*found;
}

const opt::OptimizerCandidate* FindOptimizedCandidate(const opt::OptimizedPlan& optimized,
                                                     const std::string& id) {
  const auto found = std::find_if(optimized.candidates.begin(), optimized.candidates.end(), [&](const opt::OptimizerCandidate& candidate) {
    return candidate.plan_candidate.candidate_id == id;
  });
  return found == optimized.candidates.end() ? nullptr : &*found;
}

bool TreeHasEvidence(const opt::PhysicalPlanNode& node, const std::string& evidence) {
  if (Has(node.runtime_evidence, evidence)) return true;
  return std::any_of(node.children.begin(), node.children.end(), [&](const opt::PhysicalPlanNode& child) {
    return TreeHasEvidence(child, evidence);
  });
}

opt::OptimizerStatsIdentity FreshIdentity(const std::string& object_uuid,
                                          const std::string& statistic_uuid) {
  opt::OptimizerStatsIdentity identity;
  identity.object_uuid = object_uuid;
  identity.statistic_uuid = statistic_uuid;
  identity.stats_epoch = 19;
  identity.catalog_epoch = 19;
  identity.transaction_visibility_epoch = 19;
  identity.freshness = opt::OptimizerStatsFreshnessState::kFresh;
  identity.source = opt::StatisticSource::kCatalogExact;
  identity.confidence = opt::CostConfidence::kHigh;
  return identity;
}

opt::OptimizerStatsIdentity StaleIdentity(const std::string& object_uuid,
                                          const std::string& statistic_uuid) {
  auto identity = FreshIdentity(object_uuid, statistic_uuid);
  identity.freshness = opt::OptimizerStatsFreshnessState::kStale;
  return identity;
}

opt::TableCardinalityStats TableStats(const std::string& relation_uuid) {
  opt::TableCardinalityStats stats;
  stats.identity = FreshIdentity(relation_uuid, relation_uuid + ":table");
  stats.row_count = 50000;
  stats.visible_row_count = 48000;
  stats.page_count = 1200;
  stats.average_row_bytes = 96;
  return stats;
}

opt::IndexStats IndexStats(const std::string& relation_uuid,
                           const std::string& index_uuid,
                           const std::string& family,
                           std::vector<std::string> keys,
                           std::vector<std::string> covered,
                           bool unique = false,
                           bool covering = true) {
  opt::IndexStats stats;
  stats.identity = FreshIdentity(index_uuid, index_uuid + ":index");
  stats.index_uuid = index_uuid;
  stats.relation_uuid = relation_uuid;
  stats.index_family = family;
  stats.key_column_uuids = std::move(keys);
  stats.covered_column_uuids = std::move(covered);
  stats.unique = unique;
  stats.covering = covering;
  stats.height = 3;
  stats.leaf_pages = 80;
  stats.distinct_keys = unique ? 50000 : 25000;
  stats.clustering_factor = 0.85;
  stats.fragmentation_ratio = 0.02;
  stats.visibility_coverage = 1.0;
  stats.predicate_coverage = 1.0;
  return stats;
}

opt::AccessPathPlanningRequest BaseRequest(const std::string& relation_uuid) {
  opt::AccessPathPlanningRequest request;
  request.relation_uuid = relation_uuid;
  request.predicate_kind = "scalar_eq";
  request.descriptor_digest = "desc:odf019";
  request.projected_column_uuids = {"col.customer_id"};
  request.visibility_proven = true;
  request.grants_proven = true;
  request.index_visibility_native = true;
  request.covering_payload.physical_payload_proof_present = true;
  request.covering_payload.freshness_proven = true;
  request.covering_payload.redaction_safe = true;
  request.covering_payload.result_contract_proven = true;
  request.covering_payload.base_row_recheck_handoff_proven = true;
  request.covering_payload.index_only_admitted = true;
  request.table_stats = TableStats(relation_uuid);
  return request;
}

bool OrderedLimitSelectsOrderedIndexPath() {
  const std::string relation_uuid = "rel.odf019.ordered";
  auto request = BaseRequest(relation_uuid);
  request.predicate_kind = "ordered_limit";
  request.ordered_limit.present = true;
  request.ordered_limit.order_by_column_uuids = {"col.created_at"};
  request.ordered_limit.limit_count = 25;
  request.candidate_indexes = {
      IndexStats(relation_uuid,
                 "idx.odf019.created_at",
                 "btree",
                 {"col.created_at", "col.customer_id"},
                 {"col.created_at", "col.customer_id"},
                 false,
                 true),
      IndexStats(relation_uuid,
                 "idx.odf019.hash_customer",
                 "hash",
                 {"col.customer_id"},
                 {"col.customer_id"},
                 false,
                 false),
  };

  const auto candidates = opt::GenerateFullAccessPathCandidates(request);
  const auto* ordered = FindCandidate(candidates, "CAND-OPT-ORDERED-LIMIT:idx.odf019.created_at");
  const auto* scan = FindCandidate(candidates, "CAND-OPT-FULL-SCAN");
  if (!Require(ordered != nullptr, "ordered LIMIT index candidate missing") ||
      !Require(scan != nullptr, "table scan candidate missing for ordered LIMIT comparison") ||
      !Require(ordered->cost.selectable, "ordered LIMIT candidate was not selectable") ||
      !Require(ordered->access_kind == plan::PhysicalAccessKind::kScalarBtreeRange,
               "ordered LIMIT candidate did not use ordered btree range access") ||
      !Require(ordered->cost.total_cost < scan->cost.total_cost,
               "ordered LIMIT candidate did not beat table scan before sort") ||
      !Require(Has(ordered->acceptance_reasons, "ordered_limit_index_order_satisfied"),
               "ordered LIMIT acceptance reason missing") ||
      !Require(Has(ordered->acceptance_reasons, "ordered_limit_sort_avoided"),
               "ordered LIMIT sort-avoidance reason missing")) {
    return false;
  }

  plan::LogicalPlan logical;
  logical.ok = true;
  logical.plan_id = "odf019.ordered.limit";
  auto base = plan::MakeLogicalPlanNode(plan::LogicalPlanNodeKind::kDmlRead,
                                        plan::PhysicalAccessKind::kNone,
                                        "dml.select_rows",
                                        "ordered_limit_base");
  base.required_object_uuids.push_back(relation_uuid);
  base.required_descriptors.push_back("desc:odf019");
  auto topn = plan::MakeLogicalPlanNode(plan::LogicalPlanNodeKind::kDmlRead,
                                        plan::PhysicalAccessKind::kTopN,
                                        "query.top_n",
                                        "ordered_limit_topn");
  topn.required_object_uuids.push_back(relation_uuid);
  topn.required_descriptors.push_back("limit.present");
  logical.nodes = {base, topn};

  const auto optimized = opt::OptimizeLogicalPlanWithAccessPathRequest(logical, request);
  const auto* selected = FindOptimizedCandidate(optimized, "CAND-OPT-ORDERED-LIMIT:idx.odf019.created_at");
  return Require(optimized.ok, "ordered LIMIT optimized plan not ok") &&
         Require(optimized.has_physical_plan, "ordered LIMIT physical plan missing") &&
         Require(optimized.physical_root.access_kind == plan::PhysicalAccessKind::kTopN,
                 "ordered LIMIT physical root was not top-N") &&
         Require(!optimized.physical_root.children.empty(),
                 "ordered LIMIT top-N root did not keep the ordered access child") &&
         Require(optimized.physical_root.children.front().access_kind == plan::PhysicalAccessKind::kScalarBtreeRange,
                 "ordered LIMIT child was not the ordered index range path") &&
         Require(selected != nullptr && selected->selected,
                 "ordered LIMIT candidate was not selected as the primary access path") &&
         Require(TreeHasEvidence(optimized.physical_root, "ordered_limit_sort_avoided=true"),
                 "ordered LIMIT physical evidence did not carry sort avoidance");
}

bool CoveringCandidateReasonsAreExact() {
  const std::string relation_uuid = "rel.odf019.covering";
  auto accepted_request = BaseRequest(relation_uuid);
  accepted_request.candidate_indexes = {
      IndexStats(relation_uuid,
                 "idx.odf019.covering.accept",
                 "btree",
                 {"col.customer_id"},
                 {"col.customer_id"},
                 true,
                 true),
  };
  const auto accepted_candidates = opt::GenerateFullAccessPathCandidates(accepted_request);
  const auto* accepted = FindCandidate(accepted_candidates, "CAND-OPT-COVERING:idx.odf019.covering.accept");
  if (!Require(accepted != nullptr, "accepted covering candidate missing") ||
      !Require(accepted->cost.selectable, "covering candidate with covered projection was refused") ||
      !Require(Has(accepted->acceptance_reasons, "covering_projection_covered"),
               "covering projection acceptance reason missing") ||
      !Require(Has(accepted->acceptance_reasons, "covering_index_covering"),
               "covering index acceptance reason missing") ||
      !Require(Has(accepted->acceptance_reasons, "covering_visibility_index_native_proof"),
               "covering visibility acceptance reason missing")) {
    return false;
  }

  auto projection_request = accepted_request;
  projection_request.projected_column_uuids = {"col.payload"};
  const auto projection_candidates = opt::GenerateFullAccessPathCandidates(projection_request);
  const auto* projection = FindCandidate(projection_candidates, "CAND-OPT-COVERING:idx.odf019.covering.accept");
  if (!Require(projection != nullptr, "projection-refused covering candidate missing") ||
      !Require(!projection->cost.selectable, "covering candidate with uncovered projection was selectable") ||
      !Require(projection->cost.rejection_reason == "covering_projection_not_covered",
               "covering projection refusal reason was not exact")) {
    return false;
  }

  auto not_covering_request = BaseRequest(relation_uuid);
  not_covering_request.candidate_indexes = {
      IndexStats(relation_uuid,
                 "idx.odf019.covering.not_covering",
                 "btree",
                 {"col.customer_id"},
                 {},
                 true,
                 false),
  };
  const auto not_covering_candidates = opt::GenerateFullAccessPathCandidates(not_covering_request);
  const auto* not_covering = FindCandidate(not_covering_candidates, "CAND-OPT-COVERING:idx.odf019.covering.not_covering");
  if (!Require(not_covering != nullptr, "not-covering refusal candidate missing") ||
      !Require(!not_covering->cost.selectable, "non-covering index candidate was selectable") ||
      !Require(not_covering->cost.rejection_reason == "covering_index_not_covering",
               "non-covering index refusal reason was not exact")) {
    return false;
  }

  auto visibility_request = accepted_request;
  visibility_request.index_visibility_native = false;
  const auto visibility_candidates = opt::GenerateFullAccessPathCandidates(visibility_request);
  const auto* visibility = FindCandidate(visibility_candidates, "CAND-OPT-COVERING:idx.odf019.covering.accept");
  if (!Require(visibility != nullptr, "visibility-refused covering candidate missing") ||
      !Require(!visibility->cost.selectable, "covering candidate without native visibility proof was selectable") ||
      !Require(visibility->cost.rejection_reason == "covering_visibility_index_native_proof_missing",
               "visibility proof refusal reason was not exact")) {
    return false;
  }

  auto stale_request = accepted_request;
  stale_request.candidate_indexes.front().identity = StaleIdentity("idx.odf019.covering.accept",
                                                                   "idx.odf019.covering.accept:index");
  const auto stale_candidates = opt::GenerateFullAccessPathCandidates(stale_request);
  const auto* stale = FindCandidate(stale_candidates, "CAND-OPT-COVERING:idx.odf019.covering.accept");
  return Require(stale != nullptr, "stale covering candidate missing") &&
         Require(!stale->cost.selectable, "stale covering candidate was selectable") &&
         Require(stale->cost.rejection_reason == "covering_index_rebuild_or_stale",
                 "stale covering refusal reason was not exact");
}

bool BitmapCandidateReasonsAreExact() {
  const std::string relation_uuid = "rel.odf019.bitmap";
  auto accepted_request = BaseRequest(relation_uuid);
  accepted_request.bitmap.requested = true;
  accepted_request.bitmap.executor_supported = true;
  accepted_request.candidate_indexes = {
      IndexStats(relation_uuid, "idx.odf019.bitmap.a", "btree", {"col.customer_id"}, {"col.customer_id"}, false, true),
      IndexStats(relation_uuid, "idx.odf019.bitmap.b", "hash", {"col.status"}, {"col.status"}, false, true),
  };
  const auto accepted_candidates = opt::GenerateFullAccessPathCandidates(accepted_request);
  const auto* accepted = FindCandidate(accepted_candidates, "CAND-OPT-BITMAP");
  if (!Require(accepted != nullptr, "accepted bitmap candidate missing") ||
      !Require(accepted->cost.selectable, "compatible bitmap candidate was refused") ||
      !Require(Has(accepted->acceptance_reasons, "bitmap_candidate_indexes_compatible"),
               "bitmap compatibility acceptance reason missing") ||
      !Require(Has(accepted->acceptance_reasons, "bitmap_executor_supported"),
               "bitmap executor acceptance reason missing") ||
      !Require(Has(accepted->acceptance_reasons, "bitmap_mga_recheck_preserved"),
               "bitmap MGA recheck acceptance reason missing")) {
    return false;
  }

  auto executor_request = accepted_request;
  executor_request.bitmap.executor_supported = false;
  const auto executor_candidates = opt::GenerateFullAccessPathCandidates(executor_request);
  const auto* executor = FindCandidate(executor_candidates, "CAND-OPT-BITMAP");
  if (!Require(executor != nullptr, "executor-refused bitmap candidate missing") ||
      !Require(!executor->cost.selectable, "bitmap without executor support was selectable") ||
      !Require(executor->cost.rejection_reason == "bitmap_executor_not_supported",
               "bitmap executor refusal reason was not exact")) {
    return false;
  }

  auto count_request = accepted_request;
  count_request.candidate_indexes.resize(1);
  const auto count_candidates = opt::GenerateFullAccessPathCandidates(count_request);
  const auto* count = FindCandidate(count_candidates, "CAND-OPT-BITMAP");
  if (!Require(count != nullptr, "not-enough-indexes bitmap candidate missing") ||
      !Require(!count->cost.selectable, "bitmap with one compatible index was selectable") ||
      !Require(Has(count->refusal_reasons, "bitmap_not_enough_compatible_indexes"),
               "bitmap not-enough-compatible-indexes refusal reason missing")) {
    return false;
  }

  auto predicate_request = accepted_request;
  predicate_request.predicate_kind = "full_text";
  const auto predicate_candidates = opt::GenerateFullAccessPathCandidates(predicate_request);
  const auto* predicate = FindCandidate(predicate_candidates, "CAND-OPT-BITMAP");
  return Require(predicate != nullptr, "unsupported-predicate bitmap candidate missing") &&
         Require(!predicate->cost.selectable, "bitmap with unsupported predicate was selectable") &&
         Require(Has(predicate->refusal_reasons, "bitmap_unsupported_predicate"),
                 "bitmap unsupported predicate refusal reason missing");
}

bool SummaryCandidateReasonsAreExact() {
  const std::string relation_uuid = "rel.odf019.summary";
  auto accepted_request = BaseRequest(relation_uuid);
  accepted_request.summary_prune.requested = true;
  accepted_request.summary_prune.summary_present = true;
  accepted_request.summary_prune.predicate_supported = true;
  accepted_request.summary_prune.summary_generation = 44;
  accepted_request.summary_prune.relation_generation = 44;
  accepted_request.summary_prune.base_row_mga_recheck_planned = true;
  accepted_request.summary_prune.base_row_security_recheck_planned = true;
  accepted_request.summary_prune.candidate_ranges = 16;
  accepted_request.summary_prune.ranges_pruned = 12;
  accepted_request.summary_prune.pages_considered = 4096;
  accepted_request.summary_prune.pages_pruned = 3000;
  accepted_request.candidate_indexes = {
      IndexStats(relation_uuid, "idx.odf019.summary", "btree", {"col.customer_id"}, {"col.customer_id"}, false, true),
  };
  const auto accepted_candidates = opt::GenerateFullAccessPathCandidates(accepted_request);
  const auto* accepted = FindCandidate(accepted_candidates, "CAND-OPT-SUMMARY-PRUNE");
  if (!Require(accepted != nullptr, "accepted summary candidate missing") ||
      !Require(accepted->cost.selectable, "fresh summary candidate was refused") ||
      !Require(Has(accepted->acceptance_reasons, "summary_present"),
               "summary-present acceptance reason missing") ||
      !Require(Has(accepted->acceptance_reasons, "summary_generation_fresh"),
               "summary generation acceptance reason missing") ||
      !Require(Has(accepted->acceptance_reasons, "summary_predicate_supported"),
               "summary predicate acceptance reason missing") ||
      !Require(Has(accepted->acceptance_reasons, "summary_base_row_mga_recheck_required"),
               "summary MGA recheck acceptance reason missing") ||
      !Require(accepted->summary_prune_evidence.present,
               "summary prune evidence missing") ||
      !Require(accepted->summary_prune_evidence.base_row_mga_recheck_required,
               "summary candidate did not preserve base-row MGA recheck") ||
      !Require(accepted->summary_prune_evidence.base_row_security_recheck_required,
               "summary candidate did not preserve base-row security recheck")) {
    return false;
  }

  auto missing_request = accepted_request;
  missing_request.summary_prune.summary_present = false;
  const auto missing_candidates = opt::GenerateFullAccessPathCandidates(missing_request);
  const auto* missing = FindCandidate(missing_candidates, "CAND-OPT-SUMMARY-PRUNE");
  if (!Require(missing != nullptr, "missing-summary refusal candidate missing") ||
      !Require(!missing->cost.selectable, "candidate with missing summary was selectable") ||
      !Require(missing->cost.rejection_reason == "summary_missing",
               "missing-summary refusal reason was not exact")) {
    return false;
  }

  auto stale_request = accepted_request;
  stale_request.summary_prune.summary_generation = 43;
  const auto stale_candidates = opt::GenerateFullAccessPathCandidates(stale_request);
  const auto* stale = FindCandidate(stale_candidates, "CAND-OPT-SUMMARY-PRUNE");
  if (!Require(stale != nullptr, "stale-summary refusal candidate missing") ||
      !Require(!stale->cost.selectable, "candidate with stale summary was selectable") ||
      !Require(stale->cost.rejection_reason == "summary_stale_generation_mismatch",
               "stale-summary refusal reason was not exact")) {
    return false;
  }

  auto predicate_request = accepted_request;
  predicate_request.summary_prune.predicate_supported = false;
  const auto predicate_candidates = opt::GenerateFullAccessPathCandidates(predicate_request);
  const auto* predicate = FindCandidate(predicate_candidates, "CAND-OPT-SUMMARY-PRUNE");
  if (!Require(predicate != nullptr, "unsupported-summary-predicate refusal candidate missing") ||
      !Require(!predicate->cost.selectable, "candidate with unsupported summary predicate was selectable") ||
      !Require(Has(predicate->refusal_reasons, "summary_unsupported_predicate"),
               "unsupported summary predicate refusal reason missing")) {
    return false;
  }

  auto recheck_request = accepted_request;
  recheck_request.summary_prune.base_row_mga_recheck_planned = false;
  recheck_request.summary_prune.base_row_security_recheck_planned = false;
  const auto recheck_candidates = opt::GenerateFullAccessPathCandidates(recheck_request);
  const auto* recheck = FindCandidate(recheck_candidates, "CAND-OPT-SUMMARY-PRUNE");
  return Require(recheck != nullptr, "missing-recheck refusal candidate missing") &&
         Require(!recheck->cost.selectable, "summary candidate without rechecks was selectable") &&
         Require(Has(recheck->refusal_reasons, "summary_mga_recheck_required"),
                 "summary MGA recheck refusal reason missing") &&
         Require(Has(recheck->refusal_reasons, "summary_security_recheck_required"),
                 "summary security recheck refusal reason missing");
}

}  // namespace

int main() {
  if (!OrderedLimitSelectsOrderedIndexPath()) return 1;
  if (!CoveringCandidateReasonsAreExact()) return 1;
  if (!BitmapCandidateReasonsAreExact()) return 1;
  if (!SummaryCandidateReasonsAreExact()) return 1;
  return 0;
}

// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "access_path_full.hpp"

#include <algorithm>
#include <iostream>
#include <string>
#include <vector>

namespace opt = scratchbird::engine::optimizer;
namespace idx = scratchbird::core::index;
namespace plan = scratchbird::engine::planner;

namespace {

bool Require(bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << message << '\n';
    return false;
  }
  return true;
}

bool Has(const std::vector<std::string>& values, const std::string& expected) {
  return std::find(values.begin(), values.end(), expected) != values.end();
}

const opt::PlanCandidate* FindCandidate(const std::vector<opt::PlanCandidate>& candidates,
                                        const std::string& id) {
  const auto found = std::find_if(candidates.begin(), candidates.end(), [&](const opt::PlanCandidate& candidate) {
    return candidate.candidate_id == id;
  });
  return found == candidates.end() ? nullptr : &*found;
}

opt::OptimizerStatsIdentity FreshIdentity(const std::string& object_uuid) {
  opt::OptimizerStatsIdentity identity;
  identity.object_uuid = object_uuid;
  identity.statistic_uuid = object_uuid + ":stats";
  identity.stats_epoch = 23;
  identity.catalog_epoch = 23;
  identity.transaction_visibility_epoch = 23;
  identity.freshness = opt::OptimizerStatsFreshnessState::kFresh;
  identity.source = opt::StatisticSource::kCatalogExact;
  identity.confidence = opt::CostConfidence::kHigh;
  return identity;
}

opt::TableCardinalityStats TableStats(const std::string& relation_uuid) {
  opt::TableCardinalityStats stats;
  stats.identity = FreshIdentity(relation_uuid);
  stats.row_count = 100000;
  stats.visible_row_count = 90000;
  stats.page_count = 1000;
  stats.average_row_bytes = 128;
  return stats;
}

opt::IndexStats IndexStats(const std::string& relation_uuid) {
  opt::IndexStats index;
  index.identity = FreshIdentity("11111111-1111-4111-8111-111111111111");
  index.index_uuid = "idx.odf023.amount";
  index.relation_uuid = relation_uuid;
  index.index_family = "btree";
  index.key_column_uuids = {"col.amount"};
  index.height = 3;
  index.leaf_pages = 100;
  index.distinct_keys = 50000;
  return index;
}

opt::OptimizerPruneBoundary Boundary(const std::string& min_value,
                                     const std::string& max_value) {
  opt::OptimizerPruneBoundary boundary;
  boundary.scalar_type_key = "int64";
  boundary.encoded_min = min_value;
  boundary.encoded_max = max_value;
  boundary.min_present = true;
  boundary.max_present = true;
  return boundary;
}

opt::OptimizerPrunePredicate Predicate() {
  opt::OptimizerPrunePredicate predicate;
  predicate.scalar_type_key = "int64";
  predicate.encoded_lower = "050";
  predicate.encoded_upper = "060";
  predicate.lower_present = true;
  predicate.upper_present = true;
  return predicate;
}

opt::AccessPathPlanningRequest BaseAccessRequest(const std::string& relation_uuid) {
  opt::AccessPathPlanningRequest request;
  request.relation_uuid = relation_uuid;
  request.predicate_kind = "scalar_range";
  request.predicate_text = "amount between 50 and 60";
  request.visibility_proven = true;
  request.grants_proven = true;
  request.base_row_mga_recheck_planned = true;
  request.base_row_security_recheck_planned = true;
  request.table_stats = TableStats(relation_uuid);
  request.candidate_indexes = {IndexStats(relation_uuid)};
  request.partition_segment_prune.requested = true;
  request.partition_segment_prune.relation_uuid = relation_uuid;
  request.partition_segment_prune.predicate = Predicate();
  return request;
}

idx::PageExtentSummaryFormatCompatibility Format() {
  idx::PageExtentSummaryFormatCompatibility format;
  format.observed = {2, 0};
  format.open_class = idx::PageExtentSummaryFormatOpenClass::current;
  format.compatible = true;
  format.migration_required = false;
  format.diagnostic_code = "format.current";
  return format;
}

idx::PageExtentSummaryMetadata PageSummary(const std::string& uuid,
                                           const std::string& relation_uuid,
                                           const std::string& min_value,
                                           const std::string& max_value,
                                           std::uint64_t first_page,
                                           std::uint32_t page_count) {
  idx::PageExtentSummaryMetadata summary;
  summary.relation_uuid = relation_uuid;
  summary.summary_uuid = uuid;
  summary.range.kind = idx::PageExtentSummaryRangeKind::page_range;
  summary.range.first_page_id = first_page;
  summary.range.page_count = page_count;
  summary.boundary.scalar_type_key = "int64";
  summary.boundary.encoded_min = min_value;
  summary.boundary.encoded_max = max_value;
  summary.boundary.min_present = true;
  summary.boundary.max_present = true;
  summary.row_count = 1000;
  summary.status = idx::PageExtentSummaryStatus::current;
  summary.format_version = {2, 0};
  summary.generation = 23;
  summary.persisted_record_present = true;
  summary.checksum_valid = true;
  return summary;
}

idx::TimeRangeSummaryDescriptor TimeSummary(const std::string& uuid,
                                            const std::string& relation_uuid,
                                            const std::string& min_value,
                                            const std::string& max_value,
                                            std::uint64_t first_page,
                                            std::uint32_t page_count) {
  idx::TimeRangeSummaryDescriptor summary;
  summary.table_uuid = relation_uuid;
  summary.index_uuid = "22222222-2222-4222-8222-222222222222";
  summary.range_family_uuid = "33333333-3333-4333-8333-333333333333";
  summary.summary_uuid = uuid;
  summary.range.kind = idx::PageExtentSummaryRangeKind::page_range;
  summary.range.first_page_id = first_page;
  summary.range.page_count = page_count;
  summary.time_scalar_type_key = "int64";
  summary.encoded_min_time = min_value;
  summary.encoded_max_time = max_value;
  summary.min_time_present = true;
  summary.max_time_present = true;
  summary.row_count = 1000;
  summary.status = idx::PageExtentSummaryStatus::current;
  summary.format_version = {2, 0};
  summary.generation = 23;
  summary.persisted_record_present = true;
  summary.checksum_valid = true;
  return summary;
}

idx::PageExtentSummaryPrunePredicate PagePredicate() {
  idx::PageExtentSummaryPrunePredicate predicate;
  predicate.scalar_type_key = "int64";
  predicate.encoded_lower = "050";
  predicate.encoded_upper = "060";
  predicate.lower_present = true;
  predicate.upper_present = true;
  return predicate;
}

idx::TimeRangeSummaryPredicate TimePredicate() {
  idx::TimeRangeSummaryPredicate predicate;
  predicate.time_scalar_type_key = "int64";
  predicate.encoded_lower_time = "050";
  predicate.encoded_upper_time = "060";
  predicate.lower_present = true;
  predicate.upper_present = true;
  return predicate;
}

bool AcceptedPartitionSegmentPlacementPruningIsBeforeCosting() {
  const std::string relation_uuid = "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa";
  auto request = BaseAccessRequest(relation_uuid);
  request.partition_segment_prune.partitions = {
      {"partition.old", Boundary("001", "010"), 200, true, true, true},
      {"partition.hit", Boundary("050", "070"), 300, true, true, true}};
  request.partition_segment_prune.segments = {
      {"segment.old", "partition.old", Boundary("011", "020"), 100, true, true, true},
      {"segment.hit", "partition.hit", Boundary("055", "080"), 150, true, true, true}};
  request.partition_segment_prune.placements = {
      {"placement.old", "filespace.cold", "segment.old", Boundary("021", "030"), 80, true, true, true},
      {"placement.hit", "filespace.hot", "segment.hit", Boundary("050", "052"), 90, true, true, true}};

  const auto candidates = opt::GenerateFullAccessPathCandidates(request);
  const auto* scan = FindCandidate(candidates, "CAND-OPT-FULL-SCAN");
  const auto* index = FindCandidate(candidates, "CAND-OPT-INDEX:idx.odf023.amount");
  return Require(scan != nullptr && index != nullptr, "expected scan and index candidates") &&
         Require(scan->partition_segment_prune_evidence.present,
                 "partition/segment evidence was not attached to scan candidate") &&
         Require(scan->partition_segment_prune_evidence.partitions_pruned == 1,
                 "partition range proof did not prune exactly one partition") &&
         Require(scan->partition_segment_prune_evidence.segments_pruned == 1,
                 "segment range proof did not prune exactly one segment") &&
         Require(scan->partition_segment_prune_evidence.placements_pruned == 1,
                 "placement range proof did not prune exactly one placement") &&
         Require(scan->partition_segment_prune_evidence.pages_pruned == 380,
                 "pruned page counter did not include partition, segment, and placement pages") &&
         Require(scan->estimated_rows < request.table_stats->visible_row_count,
                 "scan estimate was not reduced before costing") &&
         Require(index->estimated_rows < 22500,
                 "index candidate estimate did not reflect pre-cost pruning") &&
         Require(Has(scan->partition_segment_prune_evidence.acceptance_reasons,
                     "partition_pruned_by_range_proof"),
                 "partition prune acceptance reason missing") &&
         Require(Has(scan->partition_segment_prune_evidence.acceptance_reasons,
                     "segment_pruned_by_range_proof"),
                 "segment prune acceptance reason missing") &&
         Require(Has(scan->partition_segment_prune_evidence.acceptance_reasons,
                     "placement_pruned_by_range_proof"),
                 "placement prune acceptance reason missing") &&
         Require(scan->partition_segment_prune_evidence.base_row_mga_recheck_required,
                 "MGA recheck evidence was not preserved") &&
         Require(scan->partition_segment_prune_evidence.base_row_security_recheck_required,
                 "security recheck evidence was not preserved") &&
         Require(!scan->partition_segment_prune_evidence.pruning_metadata_visibility_authority,
                 "pruning metadata became visibility authority") &&
         Require(!scan->partition_segment_prune_evidence.pruning_metadata_finality_authority,
                 "pruning metadata became finality authority");
}

bool ConservativeFallbackReasonsAreExact() {
  const std::string relation_uuid = "bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb";
  opt::OptimizerPartitionSegmentPruneRequest request;
  request.requested = true;
  request.relation_uuid = relation_uuid;
  request.predicate = Predicate();
  request.partitions = {{"partition.missing", Boundary("001", "010"), 10, false, true, true}};
  request.segments = {{"segment.stale", "partition.missing", Boundary("001", "010"), 10, true, false, true}};
  request.placements = {{"placement.incompat", "filespace.hot", "segment.stale", Boundary("001", "010"), 10, true, true, false}};
  const auto fallback = opt::PlanPartitionSegmentPruning(request);

  auto no_recheck = request;
  no_recheck.base_row_mga_recheck_planned = false;
  no_recheck.base_row_security_recheck_planned = false;
  const auto missing_recheck = opt::PlanPartitionSegmentPruning(no_recheck);

  return Require(!fallback.any_pruned, "conservative fallback pruned without usable proof") &&
         Require(Has(fallback.evidence.refusal_reasons, "partition_scanned_metadata_missing"),
                 "partition missing metadata refusal reason missing") &&
         Require(Has(fallback.evidence.refusal_reasons, "segment_scanned_metadata_stale"),
                 "segment stale metadata refusal reason missing") &&
         Require(Has(fallback.evidence.refusal_reasons, "placement_scanned_predicate_incompatible"),
                 "placement incompatible refusal reason missing") &&
         Require(!missing_recheck.any_pruned, "missing recheck plan still pruned") &&
         Require(Has(missing_recheck.evidence.refusal_reasons,
                     "partition_segment_prune_mga_recheck_missing"),
                 "missing MGA planning refusal reason missing") &&
         Require(Has(missing_recheck.evidence.refusal_reasons,
                     "partition_segment_prune_security_recheck_missing"),
                 "missing security planning refusal reason missing") &&
         Require(Has(missing_recheck.evidence.refusal_reasons,
                     "partition_scanned_mga_recheck_missing"),
                 "partition missing-MGA scan reason missing");
}

bool AccessPathTopLevelRecheckFlagsGatePartitionPruning() {
  const std::string relation_uuid = "eeeeeeee-eeee-4eee-8eee-eeeeeeeeeeee";
  auto request = BaseAccessRequest(relation_uuid);
  request.base_row_mga_recheck_planned = false;
  request.base_row_security_recheck_planned = false;
  request.partition_segment_prune.partitions = {
      {"partition.old", Boundary("001", "010"), 200, true, true, true},
      {"partition.hit", Boundary("050", "070"), 300, true, true, true}};

  const auto candidates = opt::GenerateFullAccessPathCandidates(request);
  const auto* scan = FindCandidate(candidates, "CAND-OPT-FULL-SCAN");
  return Require(scan != nullptr, "top-level recheck gate scan candidate missing") &&
         Require(scan->partition_segment_prune_evidence.present,
                 "top-level recheck gate evidence missing") &&
         Require(scan->partition_segment_prune_evidence.partitions_pruned == 0,
                 "top-level missing rechecks still allowed partition pruning") &&
         Require(Has(scan->partition_segment_prune_evidence.refusal_reasons,
                     "partition_segment_prune_mga_recheck_missing"),
                 "top-level missing MGA recheck refusal missing") &&
         Require(Has(scan->partition_segment_prune_evidence.refusal_reasons,
                     "partition_segment_prune_security_recheck_missing"),
                 "top-level missing security recheck refusal missing") &&
         Require(Has(scan->partition_segment_prune_evidence.refusal_reasons,
                     "partition_scanned_mga_recheck_missing"),
                 "top-level missing recheck did not force partition scan");
}

opt::OptimizerPartitionSegmentPruneRequest SummaryBridgeRequest() {
  const std::string relation_uuid = "cccccccc-cccc-4ccc-8ccc-cccccccccccc";
  opt::OptimizerPartitionSegmentPruneRequest request;
  request.requested = true;
  request.relation_uuid = relation_uuid;
  request.predicate = Predicate();
  request.summaries.page_summary_requested = true;
  request.summaries.page_summary.format = Format();
  request.summaries.page_summary.predicate = PagePredicate();
  request.summaries.page_summary.summaries = {
      PageSummary("44444444-4444-4444-8444-444444444444", relation_uuid, "001", "010", 0, 64),
      PageSummary("55555555-5555-4555-8555-555555555555", relation_uuid, "055", "070", 64, 64)};
  request.summaries.time_summary_requested = true;
  request.summaries.time_summary.format = Format();
  request.summaries.time_summary.predicate = TimePredicate();
  request.summaries.time_summary.summaries = {
      TimeSummary("66666666-6666-4666-8666-666666666666", relation_uuid, "001", "010", 128, 32),
      TimeSummary("77777777-7777-4777-8777-777777777777", relation_uuid, "055", "070", 160, 32)};
  return request;
}

bool SummaryPageAndTimeRangeBridgeBehaviorIsPreserved() {
  const auto plan_result = opt::PlanPartitionSegmentPruning(SummaryBridgeRequest());
  return Require(plan_result.any_pruned, "summary bridge did not expose accepted pruning") &&
         Require(plan_result.evidence.ranges_pruned == 2,
                 "page/time summary bridge pruned wrong number of ranges") &&
         Require(plan_result.evidence.pages_pruned == 96,
                 "page/time summary bridge pruned wrong page count") &&
         Require(Has(plan_result.evidence.acceptance_reasons,
                     "summary_accepted_page_extent_current"),
                 "page extent summary acceptance reason missing") &&
         Require(Has(plan_result.evidence.acceptance_reasons,
                     "summary_accepted_time_range_current"),
                 "time range summary acceptance reason missing");
}

bool SummaryRefusalsAreExact() {
  auto missing = SummaryBridgeRequest();
  missing.summaries.time_summary_requested = false;
  missing.summaries.page_summary.summaries.clear();

  auto stale = SummaryBridgeRequest();
  stale.summaries.time_summary_requested = false;
  stale.summaries.page_summary.summaries[0].status = idx::PageExtentSummaryStatus::stale;

  auto corrupt = SummaryBridgeRequest();
  corrupt.summaries.time_summary_requested = false;
  corrupt.summaries.page_summary.summaries[0].status = idx::PageExtentSummaryStatus::corrupt;
  corrupt.summaries.page_summary.summaries[0].checksum_valid = false;

  auto incompatible = SummaryBridgeRequest();
  incompatible.summaries.time_summary_requested = false;
  incompatible.summaries.page_summary.format.compatible = false;
  incompatible.summaries.page_summary.summaries[0].status = idx::PageExtentSummaryStatus::incompatible_format;

  auto non_authoritative = SummaryBridgeRequest();
  non_authoritative.summaries.time_summary_requested = false;
  non_authoritative.summaries.page_summary.summaries[0].parser_finality_authority_claimed = true;

  const auto missing_plan = opt::PlanPartitionSegmentPruning(missing);
  const auto stale_plan = opt::PlanPartitionSegmentPruning(stale);
  const auto corrupt_plan = opt::PlanPartitionSegmentPruning(corrupt);
  const auto incompatible_plan = opt::PlanPartitionSegmentPruning(incompatible);
  const auto non_authoritative_plan = opt::PlanPartitionSegmentPruning(non_authoritative);
  return Require(Has(missing_plan.evidence.refusal_reasons, "summary_missing"),
                 "summary missing refusal reason missing") &&
         Require(Has(stale_plan.evidence.refusal_reasons, "summary_stale"),
                 "summary stale refusal reason missing") &&
         Require(Has(corrupt_plan.evidence.refusal_reasons, "summary_corrupt"),
                 "summary corrupt refusal reason missing") &&
         Require(Has(incompatible_plan.evidence.refusal_reasons, "summary_incompatible"),
                 "summary incompatible refusal reason missing") &&
         Require(Has(non_authoritative_plan.evidence.refusal_reasons, "summary_non_authoritative"),
                 "summary non-authoritative refusal reason missing");
}

bool NoRuntimeDocsExecution_PlanDependencyLeaksIntoPlanEvidence() {
  const std::string relation_uuid = "dddddddd-dddd-4ddd-8ddd-dddddddddddd";
  auto request = BaseAccessRequest(relation_uuid);
  request.partition_segment_prune.partitions = {
      {"partition.old", Boundary("001", "010"), 10, true, true, true}};
  const auto candidates = opt::GenerateFullAccessPathCandidates(request);
  const auto* scan = FindCandidate(candidates, "CAND-OPT-FULL-SCAN");
  if (!Require(scan != nullptr, "scan candidate missing for dependency check")) return false;
  const auto json = opt::SerializePlanCandidateToJson(*scan);
  const std::vector<std::string> forbidden = {
      "docs/", "execution-plans", "findings", "audit", "contracts", "references"};
  for (const auto& token : forbidden) {
    if (!Require(json.find(token) == std::string::npos,
                 "runtime plan evidence leaked forbidden planning/spec token: " + token)) {
      return false;
    }
  }
  return Require(scan->access_kind == plan::PhysicalAccessKind::kTableScan,
                 "dependency check inspected wrong candidate kind");
}

}  // namespace

int main() {
  if (!AcceptedPartitionSegmentPlacementPruningIsBeforeCosting()) return 1;
  if (!ConservativeFallbackReasonsAreExact()) return 1;
  if (!AccessPathTopLevelRecheckFlagsGatePartitionPruning()) return 1;
  if (!SummaryPageAndTimeRangeBridgeBehaviorIsPreserved()) return 1;
  if (!SummaryRefusalsAreExact()) return 1;
  if (!NoRuntimeDocsExecution_PlanDependencyLeaksIntoPlanEvidence()) return 1;
  return 0;
}

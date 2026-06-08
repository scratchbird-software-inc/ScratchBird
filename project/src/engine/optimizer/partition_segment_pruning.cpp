// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "partition_segment_pruning.hpp"

#include <algorithm>
#include <string_view>

namespace scratchbird::engine::optimizer {
namespace idx = scratchbird::core::index;
namespace {

bool EncodedLess(std::string_view left, std::string_view right) {
  return left.compare(right) < 0;
}

bool EncodedGreater(std::string_view left, std::string_view right) {
  return left.compare(right) > 0;
}

bool EncodedEqual(std::string_view left, std::string_view right) {
  return left == right;
}

bool BoundaryOutsidePredicate(const OptimizerPruneBoundary& boundary,
                              const OptimizerPrunePredicate& predicate) {
  if (!boundary.min_present || !boundary.max_present) return false;
  if (!predicate.scalar_type_key.empty() &&
      !boundary.scalar_type_key.empty() &&
      boundary.scalar_type_key != predicate.scalar_type_key) {
    return false;
  }
  if (predicate.lower_present) {
    if (EncodedLess(boundary.encoded_max, predicate.encoded_lower)) return true;
    if (EncodedEqual(boundary.encoded_max, predicate.encoded_lower) &&
        (!boundary.max_inclusive || !predicate.lower_inclusive)) {
      return true;
    }
  }
  if (predicate.upper_present) {
    if (EncodedGreater(boundary.encoded_min, predicate.encoded_upper)) return true;
    if (EncodedEqual(boundary.encoded_min, predicate.encoded_upper) &&
        (!boundary.min_inclusive || !predicate.upper_inclusive)) {
      return true;
    }
  }
  return false;
}

std::string SummaryFallbackClass(std::string_view fallback_reason,
                                 std::string_view summary_status) {
  if (summary_status == "missing" || fallback_reason.find("missing_summary") != std::string_view::npos) {
    return "summary_missing";
  }
  if (summary_status == "stale" || fallback_reason.find("stale_summary") != std::string_view::npos) {
    return "summary_stale";
  }
  if (summary_status == "corrupt" || fallback_reason.find("corrupt_summary") != std::string_view::npos) {
    return "summary_corrupt";
  }
  if (summary_status == "incompatible_format" ||
      fallback_reason.find("incompatible_summary") != std::string_view::npos) {
    return "summary_incompatible";
  }
  if (summary_status == "authority_refused" ||
      fallback_reason.find("non_authoritative_summary") != std::string_view::npos ||
      fallback_reason.find("external_finality_authority") != std::string_view::npos) {
    return "summary_non_authoritative";
  }
  return "summary_fallback_full_scan";
}

std::uint64_t PageSummarySpan(const idx::PageExtentSummaryMetadata& metadata) {
  if (metadata.range.page_count != 0) return metadata.range.page_count;
  return metadata.range.extent_count;
}

std::uint64_t TimeSummarySpan(const idx::TimeRangeSummaryDescriptor& descriptor) {
  if (descriptor.range.page_count != 0) return descriptor.range.page_count;
  return descriptor.range.extent_count;
}

void AddDecision(PlanPartitionSegmentPruneEvidence* evidence,
                 std::string object_type,
                 std::string object_uuid,
                 std::string parent_uuid,
                 std::string filespace_uuid,
                 std::string decision,
                 std::string reason,
                 std::uint64_t pages) {
  OptimizerPruneDecisionEvidence item;
  item.object_type = std::move(object_type);
  item.object_uuid = std::move(object_uuid);
  item.parent_uuid = std::move(parent_uuid);
  item.filespace_uuid = std::move(filespace_uuid);
  item.decision = std::move(decision);
  item.reason = std::move(reason);
  item.pages = pages;
  evidence->decisions.push_back(std::move(item));
}

void CountRangeDecision(PlanPartitionSegmentPruneEvidence* evidence,
                        bool pruned,
                        std::uint64_t pages) {
  ++evidence->candidate_ranges;
  evidence->pages_considered += pages;
  if (pruned) {
    ++evidence->ranges_pruned;
    evidence->pages_pruned += pages;
  } else {
    ++evidence->ranges_scanned;
    evidence->pages_scanned += pages;
  }
}

template <typename Metadata>
std::string MetadataRefusalReason(const Metadata& metadata,
                                  std::string_view prefix) {
  if (!metadata.metadata_present) return std::string(prefix) + "_scanned_metadata_missing";
  if (!metadata.metadata_current) return std::string(prefix) + "_scanned_metadata_stale";
  if (!metadata.predicate_compatible) return std::string(prefix) + "_scanned_predicate_incompatible";
  if (!metadata.boundary.min_present || !metadata.boundary.max_present) {
    return std::string(prefix) + "_scanned_missing_range_bounds";
  }
  return std::string(prefix) + "_scanned_no_exclusion_proof";
}

template <typename Metadata>
bool MetadataUsableForProof(const Metadata& metadata) {
  return metadata.metadata_present &&
         metadata.metadata_current &&
         metadata.predicate_compatible &&
         metadata.boundary.min_present &&
         metadata.boundary.max_present;
}

std::string FirstRefusalOrNone(const std::vector<std::string>& refusals) {
  return refusals.empty() ? "none" : refusals.front();
}

}  // namespace

OptimizerPartitionSegmentPrunePlan PlanPartitionSegmentPruning(
    const OptimizerPartitionSegmentPruneRequest& request) {
  OptimizerPartitionSegmentPrunePlan plan;
  if (!request.requested) return plan;

  plan.pruning_evaluated = true;
  auto& evidence = plan.evidence;
  evidence.present = true;
  evidence.selected_access = "partition_segment_placement_prune";
  evidence.base_row_mga_recheck_required = true;
  evidence.base_row_security_recheck_required = true;
  evidence.pruning_metadata_visibility_authority = false;
  evidence.pruning_metadata_finality_authority = false;
  evidence.acceptance_reasons.push_back("pruning_evaluated_before_costing");
  evidence.acceptance_reasons.push_back("pruning_metadata_not_visibility_or_finality_authority");

  if (!request.base_row_mga_recheck_planned) {
    evidence.refusal_reasons.push_back("partition_segment_prune_mga_recheck_missing");
  }
  if (!request.base_row_security_recheck_planned) {
    evidence.refusal_reasons.push_back("partition_segment_prune_security_recheck_missing");
  }

  const bool authority_ok =
      request.base_row_mga_recheck_planned &&
      request.base_row_security_recheck_planned;

  for (const auto& partition : request.partitions) {
    ++evidence.partitions_considered;
    const auto pages = partition.page_count;
    bool pruned = false;
    std::string reason;
    if (!authority_ok) {
      reason = !request.base_row_mga_recheck_planned
                   ? "partition_scanned_mga_recheck_missing"
                   : "partition_scanned_security_recheck_missing";
    } else if (MetadataUsableForProof(partition) &&
               BoundaryOutsidePredicate(partition.boundary, request.predicate)) {
      pruned = true;
      reason = "partition_pruned_by_range_proof";
    } else {
      reason = MetadataRefusalReason(partition, "partition");
    }
    CountRangeDecision(&evidence, pruned, pages);
    if (pruned) {
      ++evidence.partitions_pruned;
      plan.any_pruned = true;
      evidence.acceptance_reasons.push_back(reason);
    } else {
      ++evidence.partitions_scanned;
      evidence.refusal_reasons.push_back(reason);
    }
    AddDecision(&evidence, "partition", partition.partition_uuid, request.relation_uuid,
                "", pruned ? "pruned" : "scan", reason, pages);
  }

  for (const auto& segment : request.segments) {
    ++evidence.segments_considered;
    const auto pages = segment.page_count;
    bool pruned = false;
    std::string reason;
    if (!authority_ok) {
      reason = !request.base_row_mga_recheck_planned
                   ? "segment_scanned_mga_recheck_missing"
                   : "segment_scanned_security_recheck_missing";
    } else if (MetadataUsableForProof(segment) &&
               BoundaryOutsidePredicate(segment.boundary, request.predicate)) {
      pruned = true;
      reason = "segment_pruned_by_range_proof";
    } else {
      reason = MetadataRefusalReason(segment, "segment");
    }
    CountRangeDecision(&evidence, pruned, pages);
    if (pruned) {
      ++evidence.segments_pruned;
      plan.any_pruned = true;
      evidence.acceptance_reasons.push_back(reason);
    } else {
      ++evidence.segments_scanned;
      evidence.refusal_reasons.push_back(reason);
    }
    AddDecision(&evidence, "segment", segment.segment_uuid, segment.partition_uuid,
                "", pruned ? "pruned" : "scan", reason, pages);
  }

  for (const auto& placement : request.placements) {
    ++evidence.placements_considered;
    const auto pages = placement.page_count;
    bool pruned = false;
    std::string reason;
    if (!authority_ok) {
      reason = !request.base_row_mga_recheck_planned
                   ? "placement_scanned_mga_recheck_missing"
                   : "placement_scanned_security_recheck_missing";
    } else if (MetadataUsableForProof(placement) &&
               BoundaryOutsidePredicate(placement.boundary, request.predicate)) {
      pruned = true;
      reason = "placement_pruned_by_range_proof";
    } else {
      reason = MetadataRefusalReason(placement, "placement");
    }
    CountRangeDecision(&evidence, pruned, pages);
    if (pruned) {
      ++evidence.placements_pruned;
      plan.any_pruned = true;
      evidence.acceptance_reasons.push_back(reason);
    } else {
      ++evidence.placements_scanned;
      evidence.refusal_reasons.push_back(reason);
    }
    AddDecision(&evidence, "placement", placement.placement_uuid,
                placement.segment_uuid, placement.filespace_uuid,
                pruned ? "pruned" : "scan", reason, pages);
  }

  if (request.summaries.page_summary_requested) {
    const auto page_plan = idx::PlanPageExtentSummaryPrune(request.summaries.page_summary);
    evidence.candidate_ranges += page_plan.counters.candidate_ranges;
    evidence.ranges_pruned += page_plan.counters.ranges_pruned;
    evidence.ranges_scanned += page_plan.counters.ranges_scanned;
    evidence.pages_considered += page_plan.counters.pages_considered;
    evidence.pages_pruned += page_plan.counters.pages_pruned;
    evidence.pages_scanned += page_plan.counters.pages_scanned;
    if (page_plan.ok()) {
      plan.any_pruned = plan.any_pruned || page_plan.counters.ranges_pruned > 0;
      evidence.acceptance_reasons.push_back("summary_accepted_page_extent_current");
      for (const auto& summary : request.summaries.page_summary.summaries) {
        AddDecision(&evidence, "summary_page", summary.summary_uuid, summary.relation_uuid,
                    "", "accepted", "summary_accepted", PageSummarySpan(summary));
      }
    } else {
      const auto reason = SummaryFallbackClass(page_plan.fallback_reason,
                                               page_plan.summary_status);
      evidence.refusal_reasons.push_back(reason);
      AddDecision(&evidence, "summary_page", "page_extent_summary", request.relation_uuid,
                  "", "scan", reason, page_plan.counters.pages_considered);
    }
  }

  if (request.summaries.time_summary_requested) {
    const auto time_plan = idx::PlanTimeRangeSummaryPrune(request.summaries.time_summary);
    evidence.candidate_ranges += time_plan.counters.prune_candidates;
    evidence.ranges_pruned += time_plan.counters.ranges_pruned;
    evidence.ranges_scanned += time_plan.counters.ranges_scanned;
    evidence.pages_considered += time_plan.counters.pages_considered;
    evidence.pages_pruned += time_plan.counters.pages_pruned;
    evidence.pages_scanned += time_plan.counters.pages_scanned;
    if (time_plan.ok()) {
      plan.any_pruned = plan.any_pruned || time_plan.counters.ranges_pruned > 0;
      evidence.acceptance_reasons.push_back("summary_accepted_time_range_current");
      for (const auto& summary : request.summaries.time_summary.summaries) {
        AddDecision(&evidence, "summary_time", summary.summary_uuid, summary.table_uuid,
                    "", "accepted", "summary_accepted", TimeSummarySpan(summary));
      }
    } else {
      const auto reason = SummaryFallbackClass(time_plan.fallback_reason,
                                               time_plan.summary_status);
      evidence.refusal_reasons.push_back(reason);
      AddDecision(&evidence, "summary_time", "time_range_summary", request.relation_uuid,
                  "", "scan", reason, time_plan.counters.pages_considered);
    }
  }

  evidence.fallback_reason = FirstRefusalOrNone(evidence.refusal_reasons);
  if (!plan.any_pruned && evidence.refusal_reasons.empty()) {
    evidence.refusal_reasons.push_back("partition_segment_placement_scanned_no_prunable_ranges");
    evidence.fallback_reason = evidence.refusal_reasons.front();
  }
  return plan;
}

std::uint64_t EstimateRowsAfterPartitionSegmentPruning(
    std::uint64_t input_rows,
    const OptimizerPartitionSegmentPrunePlan& plan) {
  if (!plan.pruning_evaluated || plan.evidence.pages_considered == 0 ||
      plan.evidence.pages_pruned == 0) {
    return input_rows;
  }
  if (plan.evidence.pages_pruned >= plan.evidence.pages_considered) return 0;
  const auto pages_scanned =
      plan.evidence.pages_considered - plan.evidence.pages_pruned;
  return (input_rows * pages_scanned + plan.evidence.pages_considered - 1) /
         plan.evidence.pages_considered;
}

std::uint64_t EstimatePagesAfterPartitionSegmentPruning(
    std::uint64_t input_pages,
    const OptimizerPartitionSegmentPrunePlan& plan) {
  if (!plan.pruning_evaluated || plan.evidence.pages_considered == 0 ||
      plan.evidence.pages_pruned == 0) {
    return input_pages;
  }
  if (plan.evidence.pages_pruned >= plan.evidence.pages_considered) return 0;
  const auto pages_scanned =
      plan.evidence.pages_considered - plan.evidence.pages_pruned;
  return std::max<std::uint64_t>(
      1, (input_pages * pages_scanned + plan.evidence.pages_considered - 1) /
             plan.evidence.pages_considered);
}

}  // namespace scratchbird::engine::optimizer

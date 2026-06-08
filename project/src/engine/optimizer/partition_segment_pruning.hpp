// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "index_optimizer_integration.hpp"
#include "time_range_summary_pruning.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace scratchbird::engine::optimizer {

// SEARCH_KEY: SB_OPTIMIZER_PARTITION_SEGMENT_PRUNING_ODF_023
// Optimizer-facing pruning metadata. These facts admit or refuse physical
// ranges before costing; they never become MGA visibility or finality authority.
struct OptimizerPruneBoundary {
  std::string scalar_type_key;
  std::string encoded_min;
  std::string encoded_max;
  bool min_present = false;
  bool max_present = false;
  bool min_inclusive = true;
  bool max_inclusive = true;
};

struct OptimizerPrunePredicate {
  std::string scalar_type_key;
  std::string encoded_lower;
  std::string encoded_upper;
  bool lower_present = false;
  bool upper_present = false;
  bool lower_inclusive = true;
  bool upper_inclusive = true;
};

struct OptimizerPartitionPruneMetadata {
  std::string partition_uuid;
  OptimizerPruneBoundary boundary;
  std::uint64_t page_count = 0;
  bool metadata_present = true;
  bool metadata_current = true;
  bool predicate_compatible = true;
};

struct OptimizerSegmentPruneMetadata {
  std::string segment_uuid;
  std::string partition_uuid;
  OptimizerPruneBoundary boundary;
  std::uint64_t page_count = 0;
  bool metadata_present = true;
  bool metadata_current = true;
  bool predicate_compatible = true;
};

struct OptimizerPlacementPruneMetadata {
  std::string placement_uuid;
  std::string filespace_uuid;
  std::string segment_uuid;
  OptimizerPruneBoundary boundary;
  std::uint64_t page_count = 0;
  bool metadata_present = true;
  bool metadata_current = true;
  bool predicate_compatible = true;
};

struct OptimizerSummaryPruneBridgeRequest {
  bool page_summary_requested = false;
  scratchbird::core::index::PageExtentSummaryPruneRequest page_summary;
  bool time_summary_requested = false;
  scratchbird::core::index::TimeRangeSummaryPruneRequest time_summary;
};

struct OptimizerPartitionSegmentPruneRequest {
  bool requested = false;
  std::string relation_uuid;
  OptimizerPrunePredicate predicate;
  std::vector<OptimizerPartitionPruneMetadata> partitions;
  std::vector<OptimizerSegmentPruneMetadata> segments;
  std::vector<OptimizerPlacementPruneMetadata> placements;
  OptimizerSummaryPruneBridgeRequest summaries;
  bool base_row_mga_recheck_planned = true;
  bool base_row_security_recheck_planned = true;
};

struct OptimizerPruneDecisionEvidence {
  std::string object_type;
  std::string object_uuid;
  std::string parent_uuid;
  std::string filespace_uuid;
  std::string decision = "scan";
  std::string reason = "scan_no_exclusion_proof";
  std::uint64_t pages = 0;
};

struct PlanPartitionSegmentPruneEvidence {
  bool present = false;
  std::string selected_access = "full_scan";
  std::string fallback_reason = "none";
  std::string authority_source = "engine_mga_base_pages";
  bool base_row_mga_recheck_required = true;
  bool base_row_security_recheck_required = true;
  bool pruning_metadata_visibility_authority = false;
  bool pruning_metadata_finality_authority = false;
  std::uint64_t partitions_considered = 0;
  std::uint64_t partitions_pruned = 0;
  std::uint64_t partitions_scanned = 0;
  std::uint64_t segments_considered = 0;
  std::uint64_t segments_pruned = 0;
  std::uint64_t segments_scanned = 0;
  std::uint64_t placements_considered = 0;
  std::uint64_t placements_pruned = 0;
  std::uint64_t placements_scanned = 0;
  std::uint64_t candidate_ranges = 0;
  std::uint64_t ranges_pruned = 0;
  std::uint64_t ranges_scanned = 0;
  std::uint64_t pages_considered = 0;
  std::uint64_t pages_pruned = 0;
  std::uint64_t pages_scanned = 0;
  std::vector<OptimizerPruneDecisionEvidence> decisions;
  std::vector<std::string> acceptance_reasons;
  std::vector<std::string> refusal_reasons;
};

struct OptimizerPartitionSegmentPrunePlan {
  bool pruning_evaluated = false;
  bool any_pruned = false;
  PlanPartitionSegmentPruneEvidence evidence;
};

OptimizerPartitionSegmentPrunePlan PlanPartitionSegmentPruning(
    const OptimizerPartitionSegmentPruneRequest& request);

std::uint64_t EstimateRowsAfterPartitionSegmentPruning(
    std::uint64_t input_rows,
    const OptimizerPartitionSegmentPrunePlan& plan);
std::uint64_t EstimatePagesAfterPartitionSegmentPruning(
    std::uint64_t input_pages,
    const OptimizerPartitionSegmentPrunePlan& plan);

}  // namespace scratchbird::engine::optimizer

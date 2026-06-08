// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "cost_model.hpp"
#include "mga_page_finality_evidence.hpp"
#include "partition_segment_pruning.hpp"
#include "statistics_catalog.hpp"

#include <optional>
#include <string>
#include <vector>

namespace scratchbird::engine::optimizer {

// SEARCH_KEY: DPC_PAGE_EXTENT_SUMMARY_PRUNE_PLANNER
struct PlanSummaryPruneEvidence {
  bool present = false;
  std::string selected_access = "full_scan";
  std::string prune_reason = "none";
  std::string fallback_reason = "none";
  std::string summary_status = "missing";
  std::uint64_t summary_generation = 0;
  std::uint64_t candidate_ranges = 0;
  std::uint64_t ranges_pruned = 0;
  std::uint64_t ranges_scanned = 0;
  std::uint64_t pages_considered = 0;
  std::uint64_t pages_pruned = 0;
  std::uint64_t pages_scanned = 0;
  std::string authority_source = "engine_mga_base_pages";
  bool base_row_mga_recheck_required = true;
  bool base_row_security_recheck_required = true;
  bool summary_metadata_visibility_authority = false;
  bool summary_metadata_finality_authority = false;
  std::string redaction_state = "catalog_identity_redacted";
};

struct PlanOrderedLimitEvidence {
  bool present = false;
  std::string index_uuid;
  std::vector<std::string> order_by_column_uuids;
  std::uint64_t limit_count = 0;
  bool index_order_satisfied = false;
  bool sort_avoided = false;
};

// SEARCH_KEY: SB_OPTIMIZER_ACCESS_PATH_CANDIDATES
struct PlanCandidate {
  std::string candidate_id;
  scratchbird::engine::planner::PhysicalAccessKind access_kind = scratchbird::engine::planner::PhysicalAccessKind::kNone;
  std::string scope = "local";
  std::vector<std::string> required_facts;
  std::vector<std::string> missing_facts;
  std::vector<std::string> acceptance_reasons;
  std::vector<std::string> refusal_reasons;
  std::vector<std::string> statistics_diagnostics;
  std::vector<std::string> runtime_evidence;
  CostVector cost;
  std::uint64_t estimated_rows = 0;
  bool selected = false;
  bool cluster_candidate = false;
  bool uses_local_default_statistics = false;
  bool uses_policy_default_statistics = false;
  PlanOrderedLimitEvidence ordered_limit_evidence;
  PlanSummaryPruneEvidence summary_prune_evidence;
  PlanPartitionSegmentPruneEvidence partition_segment_prune_evidence;
  OptimizerMgaPageFinalityEvidence mga_page_finality_evidence;
};

std::vector<PlanCandidate> GenerateLocalAccessPathCandidates(
    const scratchbird::engine::planner::LogicalPlanNode& node,
    const OptimizerStatisticsCatalog& statistics);
std::optional<PlanCandidate> ChooseBestSelectableCandidate(const std::vector<PlanCandidate>& candidates);
std::string SerializePlanCandidateToJson(const PlanCandidate& candidate);

}  // namespace scratchbird::engine::optimizer

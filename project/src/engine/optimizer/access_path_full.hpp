// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "access_path.hpp"
#include "optimizer_statistics_full.hpp"
#include "predicate_normalization.hpp"
#include "physical_plan.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace scratchbird::engine::optimizer {

struct OrderedLimitPlanningRequest {
  bool present = false;
  std::vector<std::string> order_by_column_uuids;
  std::uint64_t limit_count = 0;
};

struct BitmapPlanningRequest {
  bool requested = false;
  bool executor_supported = false;
};

struct SummaryPrunePlanningRequest {
  bool requested = false;
  bool summary_present = false;
  bool predicate_supported = false;
  std::uint64_t summary_generation = 0;
  std::uint64_t relation_generation = 0;
  bool base_row_mga_recheck_planned = true;
  bool base_row_security_recheck_planned = true;
  std::uint64_t candidate_ranges = 0;
  std::uint64_t ranges_pruned = 0;
  std::uint64_t pages_considered = 0;
  std::uint64_t pages_pruned = 0;
};

struct CoveringPayloadPlanningProof {
  bool physical_payload_proof_present = false;
  bool freshness_proven = false;
  bool redaction_safe = false;
  bool result_contract_proven = false;
  bool base_row_recheck_handoff_proven = false;
  bool index_only_admitted = false;
  bool runtime_route_consumption_required = false;
  std::vector<std::string> evidence;
  std::vector<std::string> blockers;
};

// SEARCH_KEY: SB_OPTIMIZER_ACCESS_PATH_FULL
struct AccessPathPlanningRequest {
  std::string relation_uuid;
  std::string predicate_kind;
  std::string predicate_text;
  std::optional<CanonicalSblrExpressionNode> sblr_expression;
  std::string descriptor_digest;
  std::string collation_identity;
  std::vector<std::string> projected_column_uuids;
  OrderedLimitPlanningRequest ordered_limit;
  BitmapPlanningRequest bitmap;
  SummaryPrunePlanningRequest summary_prune;
  OptimizerPartitionSegmentPruneRequest partition_segment_prune;
  bool visibility_proven = false;
  bool grants_proven = false;
  bool base_row_mga_recheck_planned = true;
  bool base_row_security_recheck_planned = true;
  bool index_visibility_native = false;
  CoveringPayloadPlanningProof covering_payload;
  std::vector<IndexStats> candidate_indexes;
  std::optional<TableCardinalityStats> table_stats;
};

std::vector<PlanCandidate> GenerateFullAccessPathCandidates(const AccessPathPlanningRequest& request);
std::vector<PhysicalPlanNode> BuildPhysicalAccessNodes(const std::vector<PlanCandidate>& candidates,
                                                       const std::string& descriptor_digest);
bool IndexCanSatisfyPredicate(const IndexStats& index, const std::string& predicate_kind);
bool IndexCanSatisfyOrdering(const IndexStats& index, const OrderedLimitPlanningRequest& ordered_limit);
bool IndexCanCoverProjection(const IndexStats& index, const std::vector<std::string>& projected_column_uuids);

}  // namespace scratchbird::engine::optimizer

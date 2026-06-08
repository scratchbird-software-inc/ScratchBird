// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "logical_plan.hpp"

#include <cstdint>
#include <string>

namespace scratchbird::engine::optimizer {

// SEARCH_KEY: SB_OPTIMIZER_COST_MODEL
// Deterministic local-node costing contract. Costs are relative work units, not
// wall-clock promises. Missing facts must add uncertainty or reject a candidate;
// they must not silently become optimistic constants.
enum class CostConfidence {
  kExact,
  kHigh,
  kMedium,
  kLow,
  kUnknown,
  kRejected,
};

struct CostVector {
  std::uint64_t startup_cost = 0;
  std::uint64_t row_cost = 0;
  std::uint64_t io_cost = 0;
  std::uint64_t memory_cost = 0;
  std::uint64_t total_cost = 0;
  std::string reason;
  std::uint64_t uncertainty_cost = 0;
  CostConfidence confidence = CostConfidence::kUnknown;
  bool selectable = true;
  std::string rejection_reason;
};

struct CostModelConstants {
  std::uint64_t command_startup = 1;
  std::uint64_t catalog_lookup_startup = 2;
  std::uint64_t row_uuid_lookup_startup = 2;
  std::uint64_t btree_lookup_startup = 3;
  std::uint64_t btree_range_startup = 4;
  std::uint64_t table_scan_startup = 10;
  std::uint64_t nested_loop_join_startup = 15;
  std::uint64_t hash_join_startup = 20;
  std::uint64_t merge_join_startup = 24;
  std::uint64_t sort_startup = 12;
  std::uint64_t topn_startup = 8;
  std::uint64_t cluster_missing_authority_penalty = 1000000000ULL;
  std::uint64_t unknown_stats_uncertainty = 500;
};

const CostModelConstants& DefaultCostModelConstants();
const char* CostConfidenceName(CostConfidence confidence);
CostVector EstimateNodeCost(const scratchbird::engine::planner::LogicalPlanNode& node);
CostVector RejectedCost(std::string reason, std::uint64_t penalty = DefaultCostModelConstants().cluster_missing_authority_penalty);
bool IsBetterCost(const CostVector& left, const CostVector& right);
std::string SerializeCostVectorToJson(const CostVector& cost);

}  // namespace scratchbird::engine::optimizer

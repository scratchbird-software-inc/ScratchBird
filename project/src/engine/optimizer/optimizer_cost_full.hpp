// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "optimizer_feedback.hpp"
#include "optimizer_statistics_full.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace scratchbird::engine::optimizer {

// SEARCH_KEY: SB_OPTIMIZER_COST_FULL_MODEL
struct OptimizerCostEnvironment {
  std::string cost_profile_id = "local_default_v1";
  double cpu_tuple_cost = 0.01;
  double cpu_index_tuple_cost = 0.005;
  double cpu_operator_cost = 0.0025;
  double visibility_tuple_cost = 0.003;
  double sequential_page_cost = 1.0;
  double random_page_cost = 4.0;
  double memory_kib_cost = 0.001;
  double spill_page_cost = 1.5;
  std::uint64_t memory_budget_bytes = 0;
};

struct CostFormulaInput {
  scratchbird::engine::planner::PhysicalAccessKind access_kind = scratchbird::engine::planner::PhysicalAccessKind::kNone;
  std::uint64_t estimated_rows = 0;
  std::uint64_t row_width_bytes = 0;
  std::uint64_t sequential_pages = 0;
  std::uint64_t random_pages = 0;
  std::uint64_t index_probe_count = 0;
  std::uint64_t index_tuple_visits = 0;
  std::uint64_t visibility_recheck_rows = 0;
  std::uint64_t false_positive_rows = 0;
  std::uint64_t duplicate_rows = 0;
  std::uint64_t required_memory_bytes = 0;
  std::uint64_t spill_bytes = 0;
  std::uint64_t sort_input_rows = 0;
  std::uint64_t sort_key_count = 0;
  bool ordering_satisfied = false;
  bool spill_capable = true;
  double heap_correlation = 0.0;
  double cache_hit_ratio = 0.0;
  std::uint64_t prefetch_pages = 0;
  CostConfidence input_confidence = CostConfidence::kUnknown;
  std::string reason = "legacy_grade_cost_formula";
};

struct OptimizerMetricCostInput {
  std::string metric_name;
  double value = 0.0;
  std::uint64_t freshness_microseconds = 0;
  bool policy_allowed = false;
  std::string operator_family;
  std::string plan_shape;
  std::string cost_profile_id;
  bool advisory_only = true;
  bool mga_visibility_recheck_preserved = true;
  bool parser_or_donor_authority = false;
  std::string transaction_finality_authority = "engine_transaction_inventory";
};

struct AgentCostRecommendation {
  std::string agent_id;
  std::string recommendation_kind;
  double cost_multiplier = 1.0;
  bool policy_accepted = false;
};

// SEARCH_KEY: OPCH_MGA_PRESSURE_COSTING
// MGA pressure evidence adjusts cost/risk only. It is not transaction finality,
// visibility authority, authorization authority, parser authority, donor
// authority, or recovery authority.
struct OptimizerMgaPressureEvidence {
  std::uint64_t cleanup_debt_bytes = 0;
  std::uint64_t retained_dead_bytes = 0;
  std::uint64_t index_backlog_entries = 0;
  std::uint64_t chain_depth_bucket = 0;
  std::uint64_t chain_scatter_bucket = 0;
  double same_page_update_ratio = 0.0;
  std::uint64_t commit_fence_backlog = 0;
  bool observed_runtime_metric = false;
  bool trusted_metric_digest = false;
  bool fresh = false;
  bool exact_recheck_required = true;
  bool mga_visibility_recheck_required = true;
  bool security_recheck_required = true;
  bool parser_or_donor_authority = false;
  bool transaction_finality_authority = false;
  bool visibility_authority = false;
  bool recovery_authority = false;
};

CostVector EstimateBaseOperatorCost(const OptimizerCostEnvironment& environment,
                                    scratchbird::engine::planner::PhysicalAccessKind access_kind,
                                    std::uint64_t rows,
                                    std::uint64_t row_width_bytes,
                                    std::uint64_t page_count);
CostVector EstimateCostVector(const OptimizerCostEnvironment& environment,
                              const CostFormulaInput& input);
CostVector ApplyFilespaceCost(CostVector cost, const PageFilespaceStats& filespace);
CostVector ApplyMemoryAndSpillCost(CostVector cost,
                                   const OptimizerCostEnvironment& environment,
                                   std::uint64_t required_memory_bytes,
                                   bool spill_capable);
CostVector ApplyMetricFeedbackCost(CostVector cost, const std::vector<OptimizerMetricCostInput>& metrics);
CostVector ApplyOptimizerCalibratedCostProfile(CostVector cost, const OptimizerCalibratedCostProfile& profile);
CostVector ApplyOptimizerRuntimeFeedbackCost(CostVector cost, const OptimizerRuntimeFeedback& feedback);
CostVector ApplyAgentRecommendationCost(CostVector cost, const std::vector<AgentCostRecommendation>& recommendations);
CostVector ApplyMgaPressureCost(CostVector cost, const OptimizerMgaPressureEvidence& pressure);
CostVector ApplyMissingStatsFallbackCost(CostVector cost, CostConfidence fallback_confidence, std::string diagnostic_reason);

}  // namespace scratchbird::engine::optimizer

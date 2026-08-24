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
  std::string scalarization_policy_id;
  // Production optimizer dimensions. The legacy aggregate fields above are
  // retained for source compatibility, but these terms remain independently
  // visible to ranking and EXPLAIN instead of being collapsed into I/O/row
  // cost before selection.
  std::uint64_t cpu_units = 0;
  std::uint64_t sequential_io_units = 0;
  std::uint64_t random_io_units = 0;
  std::uint64_t page_write_units = 0;
  std::uint64_t cache_units = 0;
  std::uint64_t memory_grant_bytes = 0;
  std::uint64_t spill_units = 0;
  std::uint64_t network_units = 0;
  std::uint64_t compression_units = 0;
  std::uint64_t encryption_units = 0;
  std::uint64_t predicate_evaluation_units = 0;
  std::uint64_t vector_distance_units = 0;
  std::uint64_t text_scoring_units = 0;
  std::uint64_t spatial_evaluation_units = 0;
  std::uint64_t udr_invocation_units = 0;
  std::uint64_t mga_units = 0;
  std::uint64_t index_maintenance_units = 0;
  // Exact Core vector terms.  The aggregate compatibility fields above remain
  // available to older costers, but a finalized vector preserves each of the
  // normative dimensions independently for ranking and EXPLAIN.
  std::uint64_t cache_miss_units = 0;
  std::uint64_t cache_residency_benefit_units = 0;
  std::uint64_t memory_allocation_units = 0;
  std::uint64_t memory_grant_opportunity_units = 0;
  std::uint64_t spill_write_units = 0;
  std::uint64_t spill_read_units = 0;
  std::uint64_t temp_space_pressure_units = 0;
  std::uint64_t decompression_units = 0;
  std::uint64_t decryption_units = 0;
  std::uint64_t expression_evaluation_units = 0;
  std::uint64_t domain_cast_units = 0;
  std::uint64_t datatype_conversion_units = 0;
  std::uint64_t collation_comparison_units = 0;
  std::uint64_t mga_version_traversal_units = 0;
  std::uint64_t mga_visibility_check_units = 0;
  std::uint64_t archive_fetch_units = 0;
  std::uint64_t garbage_retention_pressure_units = 0;
  std::uint64_t lock_latch_wait_risk_units = 0;
  std::uint64_t network_latency_units = 0;
  std::uint64_t network_bandwidth_units = 0;
  std::uint64_t remote_execution_startup_units = 0;
  std::uint64_t cluster_coordination_units = 0;
  std::uint64_t repartition_units = 0;
  std::uint64_t broadcast_units = 0;
  std::uint64_t replica_staleness_risk_units = 0;
  std::uint64_t quorum_availability_risk_units = 0;
  std::uint64_t donor_compatibility_enforcement_units = 0;
  std::uint64_t result_ordering_enforcement_units = 0;
  std::uint64_t plan_instability_penalty = 0;
  bool complete_dimension_vector = false;
  // Internal compatibility bookkeeping. This is not a cost dimension and is
  // intentionally absent from EXPLAIN/publication. It records the portion of
  // cpu_units projected from the legacy startup/row fields so a later legacy
  // mutation can be re-finalized without either retaining stale CPU work or
  // overwriting independently supplied CPU work.
  std::uint64_t compatibility_cpu_projection_units = 0;
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
// Canonical vector bookkeeping used by every optimizer route.  The legacy
// aggregate fields remain compatibility projections; selection and EXPLAIN
// retain the independent dimensions and use saturating arithmetic.
void FinalizeCostVector(CostVector* cost);
void AccumulateCostVector(CostVector* destination, const CostVector& source);
CostVector EstimateNodeCost(const scratchbird::engine::planner::LogicalPlanNode& node);
CostVector RejectedCost(std::string reason, std::uint64_t penalty = DefaultCostModelConstants().cluster_missing_authority_penalty);
bool IsBetterCost(const CostVector& left, const CostVector& right);
std::string SerializeCostVectorToJson(const CostVector& cost);

}  // namespace scratchbird::engine::optimizer

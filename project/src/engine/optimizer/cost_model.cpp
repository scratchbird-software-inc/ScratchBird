// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "cost_model.hpp"

#include <algorithm>
#include <limits>
#include <sstream>
#include <string_view>
#include <utility>

namespace scratchbird::engine::optimizer {
namespace planner = scratchbird::engine::planner;
namespace {

std::uint64_t SaturatingAdd(const std::uint64_t left,
                            const std::uint64_t right) {
  if (right > std::numeric_limits<std::uint64_t>::max() - left) {
    return std::numeric_limits<std::uint64_t>::max();
  }
  return left + right;
}

std::string JsonEscape(std::string_view input) {
  std::ostringstream out;
  for (const unsigned char ch : input) {
    switch (ch) {
      case '\\': out << "\\\\"; break;
      case '"': out << "\\\""; break;
      default: out << ch;
    }
  }
  return out.str();
}

}  // namespace

void FinalizeCostVector(CostVector* cost) {
  if (cost == nullptr) return;
  // Preserve independently supplied CPU work while replacing, rather than
  // accumulating, the compatibility projection from legacy startup/row
  // fields. Legacy costers frequently adjust row_cost after obtaining an
  // already-finalized base vector; tracking the prior projection makes this
  // operation both mutation-aware and idempotent.
  const std::uint64_t independent_cpu_units =
      cost->cpu_units >= cost->compatibility_cpu_projection_units
          ? cost->cpu_units - cost->compatibility_cpu_projection_units
          : cost->cpu_units;
  const std::uint64_t compatibility_cpu_units =
      SaturatingAdd(cost->startup_cost, cost->row_cost);
  cost->cpu_units =
      SaturatingAdd(independent_cpu_units, compatibility_cpu_units);
  cost->compatibility_cpu_projection_units = compatibility_cpu_units;

  std::uint64_t categorized_io = 0;
  const std::uint64_t categorized_io_terms[] = {
      cost->random_io_units, cost->page_write_units, cost->cache_units,
      cost->spill_units, cost->network_units, cost->compression_units,
      cost->encryption_units};
  for (const auto term : categorized_io_terms) {
    categorized_io = SaturatingAdd(categorized_io, term);
  }
  if (cost->io_cost > categorized_io) {
    cost->sequential_io_units = std::max(
        cost->sequential_io_units, cost->io_cost - categorized_io);
  }
  cost->memory_grant_bytes =
      std::max(cost->memory_grant_bytes, cost->memory_cost);

  std::uint64_t total = 0;
  const std::uint64_t dimensions[] = {
      cost->cpu_units,
      cost->sequential_io_units,
      cost->random_io_units,
      cost->page_write_units,
      cost->cache_units,
      cost->memory_grant_bytes,
      cost->spill_units,
      cost->network_units,
      cost->compression_units,
      cost->encryption_units,
      cost->predicate_evaluation_units,
      cost->vector_distance_units,
      cost->text_scoring_units,
      cost->spatial_evaluation_units,
      cost->udr_invocation_units,
      cost->mga_units,
      cost->index_maintenance_units,
      cost->uncertainty_cost};
  for (const auto dimension : dimensions) {
    total = SaturatingAdd(total, dimension);
  }
  cost->total_cost = total;
}

void AccumulateCostVector(CostVector* destination, const CostVector& source) {
  if (destination == nullptr) return;
  FinalizeCostVector(destination);
  auto finalized_source = source;
  FinalizeCostVector(&finalized_source);
  const auto destination_independent_cpu =
      destination->cpu_units >=
              destination->compatibility_cpu_projection_units
          ? destination->cpu_units -
                destination->compatibility_cpu_projection_units
          : destination->cpu_units;
  const auto source_independent_cpu =
      finalized_source.cpu_units >=
              finalized_source.compatibility_cpu_projection_units
          ? finalized_source.cpu_units -
                finalized_source.compatibility_cpu_projection_units
          : finalized_source.cpu_units;
  destination->startup_cost =
      SaturatingAdd(destination->startup_cost, finalized_source.startup_cost);
  destination->row_cost =
      SaturatingAdd(destination->row_cost, finalized_source.row_cost);
  destination->io_cost =
      SaturatingAdd(destination->io_cost, finalized_source.io_cost);
  destination->memory_cost =
      SaturatingAdd(destination->memory_cost, finalized_source.memory_cost);
  destination->uncertainty_cost = SaturatingAdd(
      destination->uncertainty_cost, finalized_source.uncertainty_cost);
  destination->cpu_units = SaturatingAdd(destination_independent_cpu,
                                         source_independent_cpu);
  destination->compatibility_cpu_projection_units = 0;
  destination->sequential_io_units = SaturatingAdd(
      destination->sequential_io_units,
      finalized_source.sequential_io_units);
  destination->random_io_units =
      SaturatingAdd(destination->random_io_units,
                    finalized_source.random_io_units);
  destination->page_write_units =
      SaturatingAdd(destination->page_write_units,
                    finalized_source.page_write_units);
  destination->cache_units =
      SaturatingAdd(destination->cache_units, finalized_source.cache_units);
  destination->memory_grant_bytes = SaturatingAdd(
      destination->memory_grant_bytes,
      finalized_source.memory_grant_bytes);
  destination->spill_units =
      SaturatingAdd(destination->spill_units, finalized_source.spill_units);
  destination->network_units =
      SaturatingAdd(destination->network_units,
                    finalized_source.network_units);
  destination->compression_units = SaturatingAdd(
      destination->compression_units, finalized_source.compression_units);
  destination->encryption_units =
      SaturatingAdd(destination->encryption_units,
                    finalized_source.encryption_units);
  destination->predicate_evaluation_units = SaturatingAdd(
      destination->predicate_evaluation_units,
      finalized_source.predicate_evaluation_units);
  destination->vector_distance_units = SaturatingAdd(
      destination->vector_distance_units,
      finalized_source.vector_distance_units);
  destination->text_scoring_units = SaturatingAdd(
      destination->text_scoring_units, finalized_source.text_scoring_units);
  destination->spatial_evaluation_units = SaturatingAdd(
      destination->spatial_evaluation_units,
      finalized_source.spatial_evaluation_units);
  destination->udr_invocation_units = SaturatingAdd(
      destination->udr_invocation_units,
      finalized_source.udr_invocation_units);
  destination->mga_units =
      SaturatingAdd(destination->mga_units, finalized_source.mga_units);
  destination->index_maintenance_units = SaturatingAdd(
      destination->index_maintenance_units,
      finalized_source.index_maintenance_units);
  destination->selectable =
      destination->selectable && finalized_source.selectable;
  if (destination->rejection_reason.empty() &&
      !finalized_source.rejection_reason.empty()) {
    destination->rejection_reason = finalized_source.rejection_reason;
  }
  if (finalized_source.confidence > destination->confidence) {
    destination->confidence = finalized_source.confidence;
  }
  FinalizeCostVector(destination);
}

const CostModelConstants& DefaultCostModelConstants() {
  static const CostModelConstants constants;
  return constants;
}

const char* CostConfidenceName(CostConfidence confidence) {
  switch (confidence) {
    case CostConfidence::kExact: return "exact";
    case CostConfidence::kHigh: return "high";
    case CostConfidence::kMedium: return "medium";
    case CostConfidence::kLow: return "low";
    case CostConfidence::kUnknown: return "unknown";
    case CostConfidence::kRejected: return "rejected";
  }
  return "unknown";
}

CostVector EstimateNodeCost(const planner::LogicalPlanNode& node) {
  const auto& c = DefaultCostModelConstants();
  CostVector cost;
  switch (node.access_kind) {
    case planner::PhysicalAccessKind::kNone:
      cost = {c.command_startup, 0, 0, 1, 2, "command_or_metadata", 0, CostConfidence::kExact, true, ""};
      break;
    case planner::PhysicalAccessKind::kCatalogUuidLookup:
      cost = {c.catalog_lookup_startup, 1, 1, 1, 5, "catalog_uuid_lookup", 0, CostConfidence::kHigh, true, ""};
      break;
    case planner::PhysicalAccessKind::kRowUuidLookup:
      cost = {c.row_uuid_lookup_startup, 1, 2, 1, 6, "row_uuid_lookup", 0, CostConfidence::kHigh, true, ""};
      break;
    case planner::PhysicalAccessKind::kScalarBtreeLookup:
      cost = {c.btree_lookup_startup, 2, 2, 2, 9, "scalar_btree_lookup", 0, CostConfidence::kHigh, true, ""};
      break;
    case planner::PhysicalAccessKind::kScalarHashLookup:
      cost = {c.btree_lookup_startup, 1, 1, 3, 8, "scalar_hash_lookup", 0, CostConfidence::kHigh, true, ""};
      break;
    case planner::PhysicalAccessKind::kScalarBtreeRange:
      cost = {c.btree_range_startup, 8, 5, 3, 20, "scalar_btree_range", 0, CostConfidence::kMedium, true, ""};
      break;
    case planner::PhysicalAccessKind::kCoveringIndexScan:
      cost = {4, 6, 3, 4, 17, "covering_index_scan", 0, CostConfidence::kMedium, true, ""};
      break;
    case planner::PhysicalAccessKind::kBitmapSummaryScan:
      cost = RejectedCost("index_family_not_supported", 1000000ULL);
      cost.reason = "bitmap_summary_scan_deferred";
      break;
    case planner::PhysicalAccessKind::kFullTextProbe:
      cost = {5, 12, 6, 4, 27, "full_text_probe", 0, CostConfidence::kMedium, true, ""};
      break;
    case planner::PhysicalAccessKind::kVectorExactSearch:
      cost = {8, 30, 12, 8, 58, "vector_exact_search", 0, CostConfidence::kMedium, true, ""};
      break;
    case planner::PhysicalAccessKind::kVectorApproximateWithFallback:
      cost = {6, 18, 8, 10, 42, "vector_approximate_with_fallback", 0, CostConfidence::kMedium, true, ""};
      break;
    case planner::PhysicalAccessKind::kDocumentPathProbe:
      cost = {4, 10, 4, 4, 22, "document_path_probe", 0, CostConfidence::kMedium, true, ""};
      break;
    case planner::PhysicalAccessKind::kGraphTraversalSeed:
      cost = {6, 20, 8, 8, 42, "graph_traversal_seed", 0, CostConfidence::kMedium, true, ""};
      break;
    case planner::PhysicalAccessKind::kTimeSeriesAppendPath:
      cost = {2, 2, 4, 2, 10, "time_series_append_path", 0, CostConfidence::kMedium, true, ""};
      break;
    case planner::PhysicalAccessKind::kJoinNestedLoop:
      cost = {c.nested_loop_join_startup, 120, 20, 12, 167, "join_nested_loop", 0, CostConfidence::kMedium, true, ""};
      break;
    case planner::PhysicalAccessKind::kJoinHash:
      cost = {c.hash_join_startup, 70, 25, 40, 155, "join_hash", 0, CostConfidence::kMedium, true, ""};
      break;
    case planner::PhysicalAccessKind::kJoinMerge:
      cost = {c.merge_join_startup, 80, 18, 24, 146, "join_merge", 0, CostConfidence::kMedium, true, ""};
      break;
    case planner::PhysicalAccessKind::kAggregateGeneric:
      cost = {8, 40, 8, 12, 68, "aggregate_generic", 0, CostConfidence::kMedium, true, ""};
      break;
    case planner::PhysicalAccessKind::kAggregateHash:
      cost = {10, 35, 8, 30, 83, "aggregate_hash", 0, CostConfidence::kMedium, true, ""};
      break;
    case planner::PhysicalAccessKind::kSort:
      cost = {c.sort_startup, 45, 20, 32, 109, "sort", 0, CostConfidence::kMedium, true, ""};
      break;
    case planner::PhysicalAccessKind::kTopN:
      cost = {c.topn_startup, 20, 8, 20, 56, "top_n", 0, CostConfidence::kMedium, true, ""};
      break;
    case planner::PhysicalAccessKind::kSortThenWindow:
      cost = {14, 80, 24, 40, 158, "sort_then_window", 0, CostConfidence::kLow, true, ""};
      break;
    case planner::PhysicalAccessKind::kCteInline:
      cost = {3, 8, 2, 2, 15, "cte_inline", 0, CostConfidence::kMedium, true, ""};
      break;
    case planner::PhysicalAccessKind::kCteMaterialize:
      cost = {8, 30, 20, 30, 88, "cte_materialize", 0, CostConfidence::kMedium, true, ""};
      break;
    case planner::PhysicalAccessKind::kSetOperation:
      cost = {8, 55, 14, 24, 101, "set_operation", 0, CostConfidence::kMedium, true, ""};
      break;
    case planner::PhysicalAccessKind::kClusterFragmentScan:
      cost = RejectedCost("cluster_authority_unavailable");
      cost.reason = "cluster_fragment_scan_fail_closed";
      break;
    case planner::PhysicalAccessKind::kRemoteNodePushdown:
      cost = RejectedCost("remote_stats_unavailable");
      cost.reason = "remote_node_pushdown_fail_closed";
      break;
    case planner::PhysicalAccessKind::kTableScan:
      cost = {c.table_scan_startup, 100, 40, 8, 158, "table_scan", 0, CostConfidence::kMedium, true, ""};
      break;
  }
  if (node.access_kind == planner::PhysicalAccessKind::kFullTextProbe) {
    cost.text_scoring_units = cost.row_cost;
  } else if (node.access_kind ==
                 planner::PhysicalAccessKind::kVectorExactSearch ||
             node.access_kind ==
                 planner::PhysicalAccessKind::kVectorApproximateWithFallback) {
    cost.vector_distance_units = cost.row_cost;
  }
  FinalizeCostVector(&cost);
  return cost;
}

CostVector RejectedCost(std::string reason, std::uint64_t penalty) {
  CostVector cost;
  cost.total_cost = penalty;
  cost.reason = std::move(reason);
  cost.uncertainty_cost = penalty;
  cost.confidence = CostConfidence::kRejected;
  cost.selectable = false;
  cost.rejection_reason = cost.reason;
  return cost;
}

bool IsBetterCost(const CostVector& left, const CostVector& right) {
  if (left.selectable != right.selectable) return left.selectable;
  if (left.total_cost != right.total_cost) return left.total_cost < right.total_cost;
  if (left.io_cost != right.io_cost) return left.io_cost < right.io_cost;
  if (left.memory_cost != right.memory_cost) return left.memory_cost < right.memory_cost;
  return left.reason < right.reason;
}

std::string SerializeCostVectorToJson(const CostVector& cost) {
  std::ostringstream out;
  out << "{\n";
  out << "  \"startup_cost\": " << cost.startup_cost << ",\n";
  out << "  \"row_cost\": " << cost.row_cost << ",\n";
  out << "  \"io_cost\": " << cost.io_cost << ",\n";
  out << "  \"memory_cost\": " << cost.memory_cost << ",\n";
  out << "  \"cpu_units\": " << cost.cpu_units << ",\n";
  out << "  \"sequential_io_units\": " << cost.sequential_io_units << ",\n";
  out << "  \"random_io_units\": " << cost.random_io_units << ",\n";
  out << "  \"page_write_units\": " << cost.page_write_units << ",\n";
  out << "  \"cache_units\": " << cost.cache_units << ",\n";
  out << "  \"memory_grant_bytes\": " << cost.memory_grant_bytes << ",\n";
  out << "  \"spill_units\": " << cost.spill_units << ",\n";
  out << "  \"network_units\": " << cost.network_units << ",\n";
  out << "  \"compression_units\": " << cost.compression_units << ",\n";
  out << "  \"encryption_units\": " << cost.encryption_units << ",\n";
  out << "  \"predicate_evaluation_units\": " << cost.predicate_evaluation_units << ",\n";
  out << "  \"vector_distance_units\": " << cost.vector_distance_units << ",\n";
  out << "  \"text_scoring_units\": " << cost.text_scoring_units << ",\n";
  out << "  \"spatial_evaluation_units\": " << cost.spatial_evaluation_units << ",\n";
  out << "  \"udr_invocation_units\": " << cost.udr_invocation_units << ",\n";
  out << "  \"mga_units\": " << cost.mga_units << ",\n";
  out << "  \"index_maintenance_units\": " << cost.index_maintenance_units << ",\n";
  out << "  \"uncertainty_cost\": " << cost.uncertainty_cost << ",\n";
  out << "  \"total_cost\": " << cost.total_cost << ",\n";
  out << "  \"reason\": \"" << JsonEscape(cost.reason) << "\",\n";
  out << "  \"confidence\": \"" << CostConfidenceName(cost.confidence) << "\",\n";
  out << "  \"selectable\": " << (cost.selectable ? "true" : "false") << ",\n";
  out << "  \"rejection_reason\": \"" << JsonEscape(cost.rejection_reason) << "\"\n";
  out << "}\n";
  return out.str();
}

}  // namespace scratchbird::engine::optimizer

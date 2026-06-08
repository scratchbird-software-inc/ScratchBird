// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "cost_model.hpp"

#include <sstream>
#include <string_view>
#include <utility>

namespace scratchbird::engine::optimizer {
namespace planner = scratchbird::engine::planner;
namespace {

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
  cost.total_cost = cost.startup_cost + cost.row_cost + cost.io_cost + cost.memory_cost + cost.uncertainty_cost;
  return cost;
}

CostVector RejectedCost(std::string reason, std::uint64_t penalty) {
  CostVector cost;
  cost.startup_cost = penalty;
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

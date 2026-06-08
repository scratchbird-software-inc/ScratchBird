// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "logical_plan.hpp"

#include <sstream>
#include <string_view>
#include <utility>

namespace scratchbird::engine::planner {
namespace {

std::string JsonEscape(std::string_view input) {
  std::ostringstream out;
  for (const unsigned char ch : input) {
    switch (ch) {
      case '\\': out << "\\\\"; break;
      case '"': out << "\\\""; break;
      case '\n': out << "\\n"; break;
      case '\r': out << "\\r"; break;
      case '\t': out << "\\t"; break;
      default: out << ch;
    }
  }
  return out.str();
}

}  // namespace

const char* LogicalPlanNodeKindName(LogicalPlanNodeKind kind) {
  switch (kind) {
    case LogicalPlanNodeKind::kCommand: return "command";
    case LogicalPlanNodeKind::kCatalogLookup: return "catalog_lookup";
    case LogicalPlanNodeKind::kTransactionControl: return "transaction_control";
    case LogicalPlanNodeKind::kDdlMutation: return "ddl_mutation";
    case LogicalPlanNodeKind::kDmlRead: return "dml_read";
    case LogicalPlanNodeKind::kDmlMutation: return "dml_mutation";
    case LogicalPlanNodeKind::kNoSqlOperation: return "nosql_operation";
    case LogicalPlanNodeKind::kManagementOperation: return "management_operation";
    case LogicalPlanNodeKind::kExtensibilityOperation: return "extensibility_operation";
    case LogicalPlanNodeKind::kUnsupported: return "unsupported";
  }
  return "unsupported";
}

const char* PhysicalAccessKindName(PhysicalAccessKind kind) {
  switch (kind) {
    case PhysicalAccessKind::kNone: return "none";
    case PhysicalAccessKind::kCatalogUuidLookup: return "catalog_uuid_lookup";
    case PhysicalAccessKind::kTableScan: return "table_scan";
    case PhysicalAccessKind::kRowUuidLookup: return "row_uuid_lookup";
    case PhysicalAccessKind::kScalarBtreeLookup: return "scalar_btree_lookup";
    case PhysicalAccessKind::kScalarHashLookup: return "scalar_hash_lookup";
    case PhysicalAccessKind::kScalarBtreeRange: return "scalar_btree_range";
    case PhysicalAccessKind::kCoveringIndexScan: return "covering_index_scan";
    case PhysicalAccessKind::kBitmapSummaryScan: return "bitmap_summary_scan";
    case PhysicalAccessKind::kFullTextProbe: return "full_text_probe";
    case PhysicalAccessKind::kVectorExactSearch: return "vector_exact_search";
    case PhysicalAccessKind::kVectorApproximateWithFallback: return "vector_approximate_with_fallback";
    case PhysicalAccessKind::kDocumentPathProbe: return "document_path_probe";
    case PhysicalAccessKind::kGraphTraversalSeed: return "graph_traversal_seed";
    case PhysicalAccessKind::kTimeSeriesAppendPath: return "time_series_append_path";
    case PhysicalAccessKind::kJoinNestedLoop: return "join_nested_loop";
    case PhysicalAccessKind::kJoinHash: return "join_hash";
    case PhysicalAccessKind::kJoinMerge: return "join_merge";
    case PhysicalAccessKind::kAggregateGeneric: return "aggregate_generic";
    case PhysicalAccessKind::kAggregateHash: return "aggregate_hash";
    case PhysicalAccessKind::kSort: return "sort";
    case PhysicalAccessKind::kTopN: return "top_n";
    case PhysicalAccessKind::kSortThenWindow: return "sort_then_window";
    case PhysicalAccessKind::kCteInline: return "cte_inline";
    case PhysicalAccessKind::kCteMaterialize: return "cte_materialize";
    case PhysicalAccessKind::kSetOperation: return "set_operation";
    case PhysicalAccessKind::kClusterFragmentScan: return "cluster_fragment_scan";
    case PhysicalAccessKind::kRemoteNodePushdown: return "remote_node_pushdown";
  }
  return "none";
}


const char* QueryShapeKindName(QueryShapeKind kind) {
  switch (kind) {
    case QueryShapeKind::kPointLookup: return "point_lookup";
    case QueryShapeKind::kRangeQuery: return "range_query";
    case QueryShapeKind::kJoinQuery: return "join_query";
    case QueryShapeKind::kAggregateQuery: return "aggregate_query";
    case QueryShapeKind::kWindowQuery: return "window_query";
    case QueryShapeKind::kCteSubquery: return "cte_subquery";
    case QueryShapeKind::kSetOperation: return "set_operation";
    case QueryShapeKind::kSpecializedWorkload: return "specialized_workload";
  }
  return "point_lookup";
}

const char* LogicalPlanSortDirectionName(LogicalPlanSortDirection direction) {
  switch (direction) {
    case LogicalPlanSortDirection::kUnspecified: return "unspecified";
    case LogicalPlanSortDirection::kAscending: return "ascending";
    case LogicalPlanSortDirection::kDescending: return "descending";
  }
  return "unspecified";
}

const char* LogicalPlanNullOrderingName(LogicalPlanNullOrdering null_ordering) {
  switch (null_ordering) {
    case LogicalPlanNullOrdering::kUnspecified: return "unspecified";
    case LogicalPlanNullOrdering::kNullsFirst: return "nulls_first";
    case LogicalPlanNullOrdering::kNullsLast: return "nulls_last";
  }
  return "unspecified";
}

bool LogicalPlanPropertyMetadataSafe(const LogicalPlanPropertyMetadata& metadata) {
  if (metadata.raw_sql_text_present ||
      metadata.parser_execution_authority_claimed ||
      metadata.parser_visibility_or_finality_authority_claimed) {
    return false;
  }
  for (const auto& fact : metadata.ordering_facts) {
    if (!fact.normalized_sblr_metadata || fact.terms.empty()) return false;
    for (const auto& term : fact.terms) {
      if (term.expression_id.empty()) return false;
    }
  }
  for (const auto& fact : metadata.grouping_facts) {
    if (!fact.normalized_sblr_metadata || fact.group_expression_ids.empty()) return false;
  }
  for (const auto& fact : metadata.window_facts) {
    if (!fact.normalized_sblr_metadata ||
        (fact.partition_expression_ids.empty() && fact.ordering_terms.empty())) {
      return false;
    }
    for (const auto& term : fact.ordering_terms) {
      if (term.expression_id.empty()) return false;
    }
  }
  for (const auto& fact : metadata.expression_equivalence_facts) {
    if (!fact.normalized_sblr_metadata || fact.equivalence_class_id.empty() ||
        fact.expression_ids.size() < 2) {
      return false;
    }
  }
  return true;
}

LogicalPlan BuildQueryShapePlan(const QueryShapeEvidence& evidence) {
  LogicalPlan plan;
  plan.ok = true;
  plan.plan_id = std::string("shape-plan:") + QueryShapeKindName(evidence.shape);
  switch (evidence.shape) {
    case QueryShapeKind::kPointLookup: {
      auto node = MakeLogicalPlanNode(LogicalPlanNodeKind::kDmlRead,
                                      PhysicalAccessKind::kNone,
                                      "dml.select_rows",
                                      "point_lookup");
      node.required_descriptors.push_back("predicate.scalar_eq");
      if (evidence.has_usable_index) {
        node.required_descriptors.push_back("catalog.index_candidate_hint");
      }
      plan.nodes.push_back(std::move(node));
      break;
    }
    case QueryShapeKind::kRangeQuery: {
      auto node = MakeLogicalPlanNode(LogicalPlanNodeKind::kDmlRead,
                                      PhysicalAccessKind::kNone,
                                      "dml.select_rows",
                                      "range_query");
      node.required_descriptors.push_back("predicate.scalar_range");
      if (evidence.has_usable_index) {
        node.required_descriptors.push_back("catalog.index_candidate_hint");
      }
      plan.nodes.push_back(std::move(node));
      break;
    }
    case QueryShapeKind::kJoinQuery:
      plan.nodes.push_back(MakeLogicalPlanNode(LogicalPlanNodeKind::kDmlRead, evidence.has_cardinality_estimate ? PhysicalAccessKind::kJoinHash : PhysicalAccessKind::kJoinNestedLoop, "query.join", "join_query"));
      if (!evidence.has_cardinality_estimate) plan.diagnostics.push_back("SBSQL_V3_PLANNER_DETERMINISTIC_FALLBACK");
      break;
    case QueryShapeKind::kAggregateQuery:
      plan.nodes.push_back(MakeLogicalPlanNode(LogicalPlanNodeKind::kDmlRead, evidence.grouping_present ? PhysicalAccessKind::kAggregateHash : PhysicalAccessKind::kAggregateGeneric, "query.aggregate", "aggregate_query"));
      break;
    case QueryShapeKind::kWindowQuery:
      plan.nodes.push_back(MakeLogicalPlanNode(LogicalPlanNodeKind::kDmlRead, evidence.has_ordered_access ? PhysicalAccessKind::kNone : PhysicalAccessKind::kSortThenWindow, "query.window", "window_query"));
      if (!evidence.has_ordered_access) plan.diagnostics.push_back("SBSQL_V3_PLANNER_DETERMINISTIC_FALLBACK");
      break;
    case QueryShapeKind::kCteSubquery:
      plan.nodes.push_back(MakeLogicalPlanNode(LogicalPlanNodeKind::kDmlRead, (!evidence.cte_reused && !evidence.volatile_or_side_effecting) ? PhysicalAccessKind::kCteInline : PhysicalAccessKind::kCteMaterialize, "query.cte_subquery", "cte_subquery"));
      break;
    case QueryShapeKind::kSetOperation:
      plan.nodes.push_back(MakeLogicalPlanNode(LogicalPlanNodeKind::kDmlRead, PhysicalAccessKind::kSetOperation, "query.set_operation", "set_operation"));
      break;
    case QueryShapeKind::kSpecializedWorkload:
      if (evidence.specialized_kind == "vector") plan.nodes.push_back(MakeLogicalPlanNode(LogicalPlanNodeKind::kNoSqlOperation, PhysicalAccessKind::kVectorApproximateWithFallback, "nosql.vector_search", "specialized_vector"));
      else if (evidence.specialized_kind == "document") plan.nodes.push_back(MakeLogicalPlanNode(LogicalPlanNodeKind::kNoSqlOperation, PhysicalAccessKind::kDocumentPathProbe, "nosql.document_find", "specialized_document"));
      else if (evidence.specialized_kind == "graph") plan.nodes.push_back(MakeLogicalPlanNode(LogicalPlanNodeKind::kNoSqlOperation, PhysicalAccessKind::kGraphTraversalSeed, "nosql.graph_query", "specialized_graph"));
      else if (evidence.specialized_kind == "search") plan.nodes.push_back(MakeLogicalPlanNode(LogicalPlanNodeKind::kNoSqlOperation, PhysicalAccessKind::kFullTextProbe, "nosql.search_query", "specialized_search"));
      else plan.nodes.push_back(MakeLogicalPlanNode(LogicalPlanNodeKind::kNoSqlOperation, PhysicalAccessKind::kTimeSeriesAppendPath, "nosql.time_series_append", "specialized_timeseries"));
      break;
  }
  return plan;
}

LogicalPlanNode MakeLogicalPlanNode(LogicalPlanNodeKind kind,
                                    PhysicalAccessKind access_kind,
                                    std::string operation_id,
                                    std::string stable_name) {
  LogicalPlanNode node;
  node.kind = kind;
  node.access_kind = access_kind;
  node.operation_id = std::move(operation_id);
  node.stable_name = std::move(stable_name);
  return node;
}

std::string SerializeLogicalPlanToJson(const LogicalPlan& plan) {
  std::ostringstream out;
  out << "{\n";
  out << "  \"ok\": " << (plan.ok ? "true" : "false") << ",\n";
  out << "  \"plan_id\": \"" << JsonEscape(plan.plan_id) << "\",\n";
  out << "  \"property_metadata\": {";
  out << "\"present\": " << (plan.property_metadata.metadata_present ? "true" : "false") << ", ";
  out << "\"ordering_fact_count\": " << plan.property_metadata.ordering_facts.size() << ", ";
  out << "\"grouping_fact_count\": " << plan.property_metadata.grouping_facts.size() << ", ";
  out << "\"window_fact_count\": " << plan.property_metadata.window_facts.size() << ", ";
  out << "\"expression_equivalence_fact_count\": "
      << plan.property_metadata.expression_equivalence_facts.size() << ", ";
  out << "\"parser_authority\": "
      << (plan.property_metadata.parser_execution_authority_claimed ||
                  plan.property_metadata.parser_visibility_or_finality_authority_claimed
              ? "true"
              : "false")
      << ", ";
  out << "\"raw_sql_text_present\": "
      << (plan.property_metadata.raw_sql_text_present ? "true" : "false") << "},\n";
  out << "  \"nodes\": [\n";
  for (std::size_t i = 0; i < plan.nodes.size(); ++i) {
    const auto& node = plan.nodes[i];
    out << "    {\"kind\": \"" << LogicalPlanNodeKindName(node.kind) << "\", \"access_kind\": \""
        << PhysicalAccessKindName(node.access_kind) << "\", \"operation_id\": \""
        << JsonEscape(node.operation_id) << "\", \"stable_name\": \"" << JsonEscape(node.stable_name) << "\"}";
    if (i + 1 != plan.nodes.size()) out << ",";
    out << "\n";
  }
  out << "  ],\n";
  out << "  \"diagnostics\": [";
  for (std::size_t i = 0; i < plan.diagnostics.size(); ++i) {
    out << "\"" << JsonEscape(plan.diagnostics[i]) << "\"";
    if (i + 1 != plan.diagnostics.size()) out << ", ";
  }
  out << "]\n";
  out << "}\n";
  return out.str();
}

}  // namespace scratchbird::engine::planner

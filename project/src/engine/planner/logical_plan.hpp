// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace scratchbird::engine::planner {

enum class CanonicalLogicalRelationalNodeKind : std::uint8_t {
  kRelationSource = 1,
  kFilter,
  kProject,
  kJoin,
  kAggregate,
  kSort,
  kLimit,
  kWindow,
  kSetOperation,
  kSubquery,
  kCte,
  kRecursiveCte,
  kValues,
  kPivot,
  kUnpivot,
  kMatchRecognize,
  kTableFunctionInvoke,
};

struct CanonicalLogicalRelationalNode {
  std::uint32_t logical_node_id{0};
  CanonicalLogicalRelationalNodeKind node_kind{
      CanonicalLogicalRelationalNodeKind::kValues};
  std::vector<std::uint32_t> input_logical_node_ids;
  std::vector<std::uint32_t> output_descriptor_ids;
  std::vector<std::uint32_t> bound_expression_ids;
  std::vector<std::uint32_t> origin_relational_node_ids;
  std::vector<std::string> required_object_uuids;
  std::string semantic_variant_id;
  bool shareable{false};
};

struct CanonicalLogicalRelationalGraph {
  std::uint16_t abi_version{1};
  std::string bound_sblr_tree_uuid;
  std::string catalog_epoch_uuid;
  std::string security_context_uuid;
  std::uint64_t local_transaction_id{0};
  std::uint64_t statement_snapshot_id{0};
  std::uint32_t root_logical_node_id{0};
  std::vector<std::uint32_t> result_descriptor_ids;
  std::vector<CanonicalLogicalRelationalNode> nodes;
  bool raw_sql_text_present{false};
  bool parser_execution_authority_claimed{false};
  bool transaction_finality_authority_claimed{false};
};

struct CanonicalLogicalRelationalGraphLimits {
  std::size_t maximum_nodes{131072};
  std::size_t maximum_depth{256};
  std::size_t maximum_fanout{1024};
  std::size_t maximum_bound_references{524288};
};

struct CanonicalLogicalRelationalGraphIssue {
  std::string diagnostic_id;
  std::uint32_t logical_node_id{0};
  std::string field_id;
};

struct CanonicalLogicalRelationalGraphValidationResult {
  bool accepted{false};
  std::size_t validated_node_count{0};
  std::size_t maximum_observed_depth{0};
  std::vector<CanonicalLogicalRelationalGraphIssue> issues;
};

inline const char* CanonicalLogicalRelationalNodeKindName(
    const CanonicalLogicalRelationalNodeKind kind) {
  switch (kind) {
    case CanonicalLogicalRelationalNodeKind::kRelationSource:
      return "relation_source";
    case CanonicalLogicalRelationalNodeKind::kFilter:
      return "filter";
    case CanonicalLogicalRelationalNodeKind::kProject:
      return "project";
    case CanonicalLogicalRelationalNodeKind::kJoin:
      return "join";
    case CanonicalLogicalRelationalNodeKind::kAggregate:
      return "aggregate";
    case CanonicalLogicalRelationalNodeKind::kSort:
      return "sort";
    case CanonicalLogicalRelationalNodeKind::kLimit:
      return "limit";
    case CanonicalLogicalRelationalNodeKind::kWindow:
      return "window";
    case CanonicalLogicalRelationalNodeKind::kSetOperation:
      return "set_operation";
    case CanonicalLogicalRelationalNodeKind::kSubquery:
      return "subquery";
    case CanonicalLogicalRelationalNodeKind::kCte:
      return "cte";
    case CanonicalLogicalRelationalNodeKind::kRecursiveCte:
      return "recursive_cte";
    case CanonicalLogicalRelationalNodeKind::kValues:
      return "values";
    case CanonicalLogicalRelationalNodeKind::kPivot:
      return "pivot";
    case CanonicalLogicalRelationalNodeKind::kUnpivot:
      return "unpivot";
    case CanonicalLogicalRelationalNodeKind::kMatchRecognize:
      return "match_recognize";
    case CanonicalLogicalRelationalNodeKind::kTableFunctionInvoke:
      return "table_function_invoke";
  }
  return "unknown";
}

// QOW-SOURCE-OPT-001-V1
// Validates the canonical logical relational graph populated from bound
// SBLR/BoundAST identities.  This graph records semantics and dependencies;
// it intentionally has no physical access, join-algorithm, device, provider,
// memory-grant, or spill-selection field.
inline CanonicalLogicalRelationalGraphValidationResult
ValidateCanonicalLogicalRelationalGraph(
    const CanonicalLogicalRelationalGraph& graph,
    const CanonicalLogicalRelationalGraphLimits& limits = {}) {
  CanonicalLogicalRelationalGraphValidationResult result;
  const auto refuse = [&](std::string diagnostic_id,
                          const std::uint32_t node_id,
                          std::string field_id) {
    result.accepted = false;
    result.issues.push_back({std::move(diagnostic_id), node_id,
                             std::move(field_id)});
    return result;
  };
  const auto canonical_uuid = [](const std::string_view value) {
    if (value.size() != 36 || value[8] != '-' || value[13] != '-' ||
        value[18] != '-' || value[23] != '-') {
      return false;
    }
    for (std::size_t index = 0; index < value.size(); ++index) {
      if (index == 8 || index == 13 || index == 18 || index == 23) continue;
      const auto ch = static_cast<unsigned char>(value[index]);
      if (!std::isxdigit(ch) || std::isupper(ch)) return false;
    }
    return true;
  };
  const auto known_kind = [](const CanonicalLogicalRelationalNodeKind kind) {
    return kind >= CanonicalLogicalRelationalNodeKind::kRelationSource &&
           kind <=
               CanonicalLogicalRelationalNodeKind::kTableFunctionInvoke;
  };
  const auto valid_semantic_variant = [](const std::string_view value) {
    if (value.empty() || value.size() > 128) return false;
    if (!std::ranges::all_of(value, [](const unsigned char ch) {
          return (ch >= 'a' && ch <= 'z') ||
                 (ch >= '0' && ch <= '9') || ch == '.' || ch == '_' ||
                 ch == '-';
        })) {
      return false;
    }
    constexpr std::string_view kPhysicalTokens[] = {
        "btree", "bitmap", "covering", "index", "hash", "merge",
        "nested", "scan", "topn", "gpu", "pushdown", "spill"};
    return std::ranges::none_of(kPhysicalTokens, [&](const auto token) {
      return value.find(token) != std::string_view::npos;
    });
  };

  if (graph.abi_version != 1) {
    return refuse("QOW-DIAG-LOGICAL-GRAPH-VERSION-V1", 0,
                  "abi_version");
  }
  if (!canonical_uuid(graph.bound_sblr_tree_uuid) ||
      !canonical_uuid(graph.catalog_epoch_uuid) ||
      !canonical_uuid(graph.security_context_uuid) ||
      graph.local_transaction_id == 0 || graph.statement_snapshot_id == 0) {
    return refuse("QOW-DIAG-LOGICAL-GRAPH-BOUNDARY-V1", 0,
                  "bound_authority_context");
  }
  if (graph.raw_sql_text_present ||
      graph.parser_execution_authority_claimed ||
      graph.transaction_finality_authority_claimed) {
    return refuse("QOW-DIAG-LOGICAL-GRAPH-AUTHORITY-V1", 0,
                  "forbidden_authority_claim");
  }
  if (limits.maximum_nodes == 0 || limits.maximum_depth == 0 ||
      limits.maximum_fanout == 0 ||
      limits.maximum_bound_references == 0 || graph.nodes.empty() ||
      graph.nodes.size() > limits.maximum_nodes) {
    return refuse("SBLR.PLAN_TREE.RESOURCE_LIMIT", 0, "node_count");
  }
  if (graph.result_descriptor_ids.empty()) {
    return refuse("SBLR.PLAN_TREE.INVALID_HANDLE", 0,
                  "result_descriptor_ids");
  }
  std::unordered_set<std::uint32_t> result_descriptors;
  for (const auto descriptor_id : graph.result_descriptor_ids) {
    if (descriptor_id == 0 ||
        !result_descriptors.insert(descriptor_id).second) {
      return refuse("SBLR.PLAN_TREE.INVALID_HANDLE", 0,
                    "result_descriptor_ids");
    }
  }

  std::unordered_map<std::uint32_t, const CanonicalLogicalRelationalNode*>
      nodes_by_id;
  std::unordered_map<std::uint32_t, std::size_t> incoming_reference_count;
  std::size_t bound_reference_count = graph.result_descriptor_ids.size();
  if (bound_reference_count > limits.maximum_bound_references) {
    return refuse("SBLR.PLAN_TREE.RESOURCE_LIMIT", 0,
                  "bound_reference_count");
  }
  for (const auto& node : graph.nodes) {
    if (node.logical_node_id == 0 || !known_kind(node.node_kind) ||
        !valid_semantic_variant(node.semantic_variant_id) ||
        !nodes_by_id.emplace(node.logical_node_id, &node).second) {
      return refuse("SBLR.PLAN_TREE.INVALID_HANDLE", node.logical_node_id,
                    "logical_node_record");
    }
    if (node.input_logical_node_ids.size() > limits.maximum_fanout ||
        node.origin_relational_node_ids.empty() ||
        node.output_descriptor_ids.empty()) {
      return refuse(node.input_logical_node_ids.size() >
                            limits.maximum_fanout
                        ? "SBLR.PLAN_TREE.RESOURCE_LIMIT"
                        : "SBLR.PLAN_TREE.INVALID_HANDLE",
                    node.logical_node_id, "logical_node_references");
    }
    std::unordered_set<std::uint32_t> descriptor_ids;
    for (const auto descriptor_id : node.output_descriptor_ids) {
      if (descriptor_id == 0 ||
          !descriptor_ids.insert(descriptor_id).second) {
        return refuse("SBLR.PLAN_TREE.INVALID_HANDLE",
                      node.logical_node_id, "output_descriptor_ids");
      }
    }
    std::unordered_set<std::uint32_t> expression_ids;
    for (const auto expression_id : node.bound_expression_ids) {
      if (expression_id == 0 ||
          !expression_ids.insert(expression_id).second) {
        return refuse("SBLR.PLAN_TREE.INVALID_HANDLE",
                      node.logical_node_id, "bound_expression_ids");
      }
    }
    std::unordered_set<std::uint32_t> origin_ids;
    for (const auto origin_id : node.origin_relational_node_ids) {
      if (origin_id == 0 || !origin_ids.insert(origin_id).second) {
        return refuse("SBLR.PLAN_TREE.INVALID_HANDLE",
                      node.logical_node_id, "origin_relational_node_ids");
      }
    }
    std::unordered_set<std::string> object_uuids;
    for (const auto& object_uuid : node.required_object_uuids) {
      if (!canonical_uuid(object_uuid) ||
          !object_uuids.insert(object_uuid).second) {
        return refuse("SBLR.PLAN_TREE.INVALID_HANDLE",
                      node.logical_node_id, "required_object_uuids");
      }
    }
    const auto add_bound_references = [&](const std::size_t count) {
      if (count > limits.maximum_bound_references - bound_reference_count) {
        return false;
      }
      bound_reference_count += count;
      return true;
    };
    if (!add_bound_references(node.input_logical_node_ids.size()) ||
        !add_bound_references(node.output_descriptor_ids.size()) ||
        !add_bound_references(node.bound_expression_ids.size()) ||
        !add_bound_references(node.origin_relational_node_ids.size()) ||
        !add_bound_references(node.required_object_uuids.size())) {
      return refuse("SBLR.PLAN_TREE.RESOURCE_LIMIT",
                    node.logical_node_id, "bound_reference_count");
    }
  }
  if (graph.root_logical_node_id == 0 ||
      !nodes_by_id.contains(graph.root_logical_node_id)) {
    return refuse("SBLR.PLAN_TREE.INVALID_HANDLE",
                  graph.root_logical_node_id, "root_logical_node_id");
  }
  if (nodes_by_id.at(graph.root_logical_node_id)->output_descriptor_ids !=
      graph.result_descriptor_ids) {
    return refuse("SBLR.PLAN_TREE.INVALID_HANDLE",
                  graph.root_logical_node_id, "result_descriptor_ids");
  }
  for (const auto& node : graph.nodes) {
    for (const auto input_id : node.input_logical_node_ids) {
      if (input_id == 0 || !nodes_by_id.contains(input_id)) {
        return refuse("SBLR.PLAN_TREE.INVALID_HANDLE",
                      node.logical_node_id, "input_logical_node_ids");
      }
      ++incoming_reference_count[input_id];
    }
  }
  for (const auto& [node_id, reference_count] : incoming_reference_count) {
    if (reference_count > 1 && !nodes_by_id.at(node_id)->shareable) {
      return refuse("SBLR.PLAN_TREE.INVALID_HANDLE", node_id, "shareable");
    }
  }

  std::unordered_map<std::uint32_t, std::uint8_t> visit_state;
  std::unordered_set<std::uint32_t> reachable;
  std::uint32_t failing_node_id = 0;
  std::string failing_field;
  std::function<bool(std::uint32_t, std::size_t)> visit =
      [&](const std::uint32_t node_id, const std::size_t depth) {
        if (depth > limits.maximum_depth) {
          failing_node_id = node_id;
          failing_field = "maximum_depth";
          return false;
        }
        result.maximum_observed_depth =
            std::max(result.maximum_observed_depth, depth);
        const auto state = visit_state[node_id];
        if (state == 1) {
          failing_node_id = node_id;
          failing_field = "cycle";
          return false;
        }
        if (state == 2) return true;
        visit_state[node_id] = 1;
        reachable.insert(node_id);
        for (const auto input_id :
             nodes_by_id.at(node_id)->input_logical_node_ids) {
          if (!visit(input_id, depth + 1)) return false;
        }
        visit_state[node_id] = 2;
        return true;
      };
  if (!visit(graph.root_logical_node_id, 1)) {
    return refuse(failing_field == "maximum_depth"
                      ? "SBLR.PLAN_TREE.RESOURCE_LIMIT"
                      : "SBLR.PLAN_TREE.INVALID_HANDLE",
                  failing_node_id, failing_field);
  }
  if (reachable.size() != graph.nodes.size()) {
    return refuse("SBLR.PLAN_TREE.INVALID_HANDLE", 0,
                  "orphan_logical_node");
  }

  result.accepted = true;
  result.validated_node_count = reachable.size();
  return result;
}

struct CanonicalPhysicalAlternativeRecord {
  std::string alternative_uuid;
  std::uint32_t logical_node_id{0};
  std::string implementation_id;
  std::string capability_uuid;
  std::vector<std::uint32_t> output_descriptor_ids;
  bool available{false};
  std::string refusal_diagnostic_id;
};

struct CanonicalPhysicalAlternativeCatalog {
  std::uint16_t abi_version{1};
  std::string bound_sblr_tree_uuid;
  std::string catalog_epoch_uuid;
  std::string security_context_uuid;
  std::uint64_t local_transaction_id{0};
  std::uint64_t statement_snapshot_id{0};
  std::vector<CanonicalPhysicalAlternativeRecord> alternatives;
  bool raw_sql_text_present{false};
  bool parser_execution_authority_claimed{false};
  bool transaction_finality_authority_claimed{false};
};

struct CanonicalLogicalPhysicalBoundaryIssue {
  std::string diagnostic_id;
  std::uint32_t logical_node_id{0};
  std::string field_id;
};

struct CanonicalLogicalPhysicalBoundaryValidationResult {
  bool accepted{false};
  bool data_access_allowed{false};
  std::size_t validated_alternative_count{0};
  std::size_t executable_alternative_count{0};
  std::vector<CanonicalLogicalPhysicalBoundaryIssue> issues;
};

// QOW-SOURCE-OPT-002-V1
// Physical alternatives are catalogued outside the immutable semantic graph.
// This boundary validates capability and result-shape compatibility only; it
// does not select an alternative, alter a logical node, or authorize a read.
inline CanonicalLogicalPhysicalBoundaryValidationResult
ValidateCanonicalLogicalPhysicalBoundary(
    const CanonicalLogicalRelationalGraph& graph,
    const CanonicalPhysicalAlternativeCatalog& catalog,
    const std::size_t maximum_alternatives = 524288) {
  CanonicalLogicalPhysicalBoundaryValidationResult result;
  const auto refuse = [&](std::string diagnostic_id,
                          const std::uint32_t node_id,
                          std::string field_id) {
    result.accepted = false;
    result.data_access_allowed = false;
    result.validated_alternative_count = 0;
    result.executable_alternative_count = 0;
    result.issues.push_back({std::move(diagnostic_id), node_id,
                             std::move(field_id)});
    return result;
  };
  const auto graph_validation =
      ValidateCanonicalLogicalRelationalGraph(graph);
  if (!graph_validation.accepted) {
    const auto& issue = graph_validation.issues.front();
    return refuse(issue.diagnostic_id, issue.logical_node_id,
                  issue.field_id);
  }
  const auto canonical_uuid = [](const std::string_view value) {
    if (value.size() != 36 || value[8] != '-' || value[13] != '-' ||
        value[18] != '-' || value[23] != '-') {
      return false;
    }
    for (std::size_t index = 0; index < value.size(); ++index) {
      if (index == 8 || index == 13 || index == 18 || index == 23) continue;
      const auto ch = static_cast<unsigned char>(value[index]);
      if (!std::isxdigit(ch) || std::isupper(ch)) return false;
    }
    return true;
  };
  const auto stable_id = [](const std::string_view value) {
    return !value.empty() && value.size() <= 128 &&
           std::ranges::all_of(value, [](const unsigned char ch) {
             return (ch >= 'a' && ch <= 'z') ||
                    (ch >= '0' && ch <= '9') || ch == '.' || ch == '_' ||
                    ch == '-';
           });
  };
  const auto diagnostic_id = [](const std::string_view value) {
    return !value.empty() && value.size() <= 160 &&
           std::ranges::all_of(value, [](const unsigned char ch) {
             return std::isalnum(ch) || ch == '.' || ch == '_' || ch == '-';
           });
  };

  if (catalog.abi_version != 1) {
    return refuse("QOW-DIAG-PHYSICAL-ALTERNATIVE-VERSION-V1", 0,
                  "abi_version");
  }
  if (catalog.bound_sblr_tree_uuid != graph.bound_sblr_tree_uuid ||
      catalog.catalog_epoch_uuid != graph.catalog_epoch_uuid ||
      catalog.security_context_uuid != graph.security_context_uuid ||
      catalog.local_transaction_id != graph.local_transaction_id ||
      catalog.statement_snapshot_id != graph.statement_snapshot_id) {
    return refuse("QOW-DIAG-PHYSICAL-ALTERNATIVE-BOUNDARY-V1", 0,
                  "logical_scope_identity");
  }
  if (catalog.raw_sql_text_present ||
      catalog.parser_execution_authority_claimed ||
      catalog.transaction_finality_authority_claimed) {
    return refuse("QOW-DIAG-PHYSICAL-ALTERNATIVE-AUTHORITY-V1", 0,
                  "forbidden_authority_claim");
  }
  if (maximum_alternatives == 0 || catalog.alternatives.empty() ||
      catalog.alternatives.size() > maximum_alternatives) {
    return refuse("SBLR.PLAN_TREE.RESOURCE_LIMIT", 0,
                  "physical_alternative_count");
  }

  std::unordered_map<std::uint32_t, const CanonicalLogicalRelationalNode*>
      nodes_by_id;
  for (const auto& node : graph.nodes) {
    nodes_by_id.emplace(node.logical_node_id, &node);
  }
  std::unordered_set<std::string> alternative_uuids;
  std::unordered_set<std::string> node_implementations;
  std::unordered_map<std::uint32_t, std::size_t> available_by_node;
  for (const auto& alternative : catalog.alternatives) {
    const auto logical_node = nodes_by_id.find(alternative.logical_node_id);
    const auto implementation_key =
        std::to_string(alternative.logical_node_id) + ":" +
        alternative.implementation_id;
    if (!canonical_uuid(alternative.alternative_uuid) ||
        !alternative_uuids.insert(alternative.alternative_uuid).second ||
        logical_node == nodes_by_id.end() ||
        !stable_id(alternative.implementation_id) ||
        !canonical_uuid(alternative.capability_uuid) ||
        !node_implementations.insert(implementation_key).second) {
      return refuse("QOW-DIAG-PHYSICAL-ALTERNATIVE-IDENTITY-V1",
                    alternative.logical_node_id,
                    "physical_alternative_record");
    }
    if (alternative.output_descriptor_ids !=
        logical_node->second->output_descriptor_ids) {
      return refuse("QOW-DIAG-PHYSICAL-ALTERNATIVE-SHAPE-V1",
                    alternative.logical_node_id,
                    "output_descriptor_ids");
    }
    if ((alternative.available &&
         !alternative.refusal_diagnostic_id.empty()) ||
        (!alternative.available &&
         !diagnostic_id(alternative.refusal_diagnostic_id))) {
      return refuse("QOW-DIAG-PHYSICAL-ALTERNATIVE-AVAILABILITY-V1",
                    alternative.logical_node_id,
                    "availability_or_refusal");
    }
    if (alternative.available) {
      ++available_by_node[alternative.logical_node_id];
      ++result.executable_alternative_count;
    }
  }
  for (const auto& node : graph.nodes) {
    if (available_by_node[node.logical_node_id] == 0) {
      return refuse("QOW-DIAG-PHYSICAL-ALTERNATIVE-UNAVAILABLE-V1",
                    node.logical_node_id, "available_implementation");
    }
  }

  result.accepted = true;
  result.data_access_allowed = false;
  result.validated_alternative_count = catalog.alternatives.size();
  return result;
}

enum class LogicalPlanNodeKind {
  kCommand,
  kCatalogLookup,
  kTransactionControl,
  kDdlMutation,
  kDmlRead,
  kDmlMutation,
  kNoSqlOperation,
  kManagementOperation,
  kExtensibilityOperation,
  kUnsupported,
};

enum class PhysicalAccessKind {
  kNone,
  kCatalogUuidLookup,
  kTableScan,
  kRowUuidLookup,
  kScalarBtreeLookup,
  kScalarHashLookup,
  kScalarBtreeRange,
  kCoveringIndexScan,
  kBitmapSummaryScan,
  kFullTextProbe,
  kVectorExactSearch,
  kVectorApproximateWithFallback,
  kDocumentPathProbe,
  kGraphTraversalSeed,
  kTimeSeriesAppendPath,
  kJoinNestedLoop,
  kJoinHash,
  kJoinMerge,
  kAggregateGeneric,
  kAggregateHash,
  kSort,
  kTopN,
  kSortThenWindow,
  kCteInline,
  kCteMaterialize,
  kSetOperation,
  kClusterFragmentScan,
  kRemoteNodePushdown,
};


enum class QueryShapeKind {
  kPointLookup,
  kRangeQuery,
  kJoinQuery,
  kAggregateQuery,
  kWindowQuery,
  kCteSubquery,
  kSetOperation,
  kSpecializedWorkload,
};

enum class LogicalPlanSortDirection {
  kUnspecified,
  kAscending,
  kDescending,
};

enum class LogicalPlanNullOrdering {
  kUnspecified,
  kNullsFirst,
  kNullsLast,
};

struct LogicalPlanExpressionEquivalenceFact {
  std::string equivalence_class_id;
  std::vector<std::string> expression_ids;
  bool normalized_sblr_metadata = true;
};

struct LogicalPlanOrderingTerm {
  std::string expression_id;
  LogicalPlanSortDirection direction = LogicalPlanSortDirection::kUnspecified;
  LogicalPlanNullOrdering null_ordering = LogicalPlanNullOrdering::kUnspecified;
  std::vector<std::string> equivalent_expression_ids;
};

struct LogicalPlanOrderingFact {
  std::string fact_id;
  std::vector<LogicalPlanOrderingTerm> terms;
  bool normalized_sblr_metadata = true;
};

struct LogicalPlanGroupingFact {
  std::string fact_id;
  std::vector<std::string> group_expression_ids;
  std::vector<std::string> equivalent_group_expression_ids;
  bool normalized_sblr_metadata = true;
};

struct LogicalPlanWindowFact {
  std::string fact_id;
  std::vector<std::string> partition_expression_ids;
  std::vector<LogicalPlanOrderingTerm> ordering_terms;
  bool normalized_sblr_metadata = true;
};

// SEARCH_KEY: OPCH_LOGICAL_PROPERTY_METADATA
// SEARCH_KEY: OPCH_ORDERING_GROUPING_WINDOW_METADATA
// Logical-plan property facts are normalized SBLR/logical-plan metadata only.
// They may guide optimizer ordering, grouping, window, and equivalence choices,
// but they cannot carry SQL text, execute parser work, or become transaction,
// visibility, security, authorization, or recovery authority.
struct LogicalPlanPropertyMetadata {
  bool metadata_present = false;
  std::vector<LogicalPlanOrderingFact> ordering_facts;
  std::vector<LogicalPlanGroupingFact> grouping_facts;
  std::vector<LogicalPlanWindowFact> window_facts;
  std::vector<LogicalPlanExpressionEquivalenceFact> expression_equivalence_facts;
  bool raw_sql_text_present = false;
  bool parser_execution_authority_claimed = false;
  bool parser_visibility_or_finality_authority_claimed = false;
};

struct QueryShapeEvidence {
  QueryShapeKind shape = QueryShapeKind::kPointLookup;
  bool has_usable_index = false;
  bool has_ordered_access = false;
  bool has_cardinality_estimate = false;
  bool grouping_present = false;
  bool cte_reused = false;
  bool volatile_or_side_effecting = false;
  std::string specialized_kind;
};

struct NormalizedOptimizerPolicyControls {
  std::string plan_profile_id = "plan_profile:default";
  std::string join_search_policy_id = "join_search:default";
  std::string memory_policy_id = "memory_policy:default";
  std::string spill_policy_id = "spill_policy:default";
  std::string parallelism_policy_id = "parallelism:default";
  std::string what_if_policy_id = "what_if:disabled";
  std::vector<std::string> safe_control_ids;
};

// SEARCH_KEY: OPCH_ENGINE_BOUNDARY_PARSER_SAFE_CONTROLS
// Optimizer policy reaches the engine only as normalized SBLR/API/logical-plan
// metadata. SQL text, parser execution authority, and reference/legacy authority
// claims are rejected by the optimizer request boundary.
struct OptimizerPolicyMetadata {
  bool optimizer_policy_metadata_present = false;
  std::string policy_source_kind;
  std::uint64_t policy_epoch = 0;
  NormalizedOptimizerPolicyControls normalized_controls;
  std::vector<std::string> safe_control_ids;
  bool raw_sql_text_present = false;
  bool parser_execution_authority_claimed = false;
  bool parser_session_directives_unbound = false;
  bool reference_or_legacy_policy_authority_claimed = false;
};

struct LogicalPlanNode {
  LogicalPlanNodeKind kind = LogicalPlanNodeKind::kUnsupported;
  PhysicalAccessKind access_kind = PhysicalAccessKind::kNone;
  std::string operation_id;
  std::string stable_name;
  std::vector<std::string> required_descriptors;
  std::vector<std::string> required_object_uuids;
  std::vector<std::string> diagnostics;
};

struct LogicalPlan {
  bool ok = false;
  std::string plan_id;
  OptimizerPolicyMetadata optimizer_policy;
  LogicalPlanPropertyMetadata property_metadata;
  std::vector<LogicalPlanNode> nodes;
  std::vector<std::string> diagnostics;
};

const char* LogicalPlanNodeKindName(LogicalPlanNodeKind kind);
const char* PhysicalAccessKindName(PhysicalAccessKind kind);
const char* QueryShapeKindName(QueryShapeKind kind);
const char* LogicalPlanSortDirectionName(LogicalPlanSortDirection direction);
const char* LogicalPlanNullOrderingName(LogicalPlanNullOrdering null_ordering);
bool LogicalPlanPropertyMetadataSafe(const LogicalPlanPropertyMetadata& metadata);
LogicalPlanNode MakeLogicalPlanNode(LogicalPlanNodeKind kind,
                                    PhysicalAccessKind access_kind,
                                    std::string operation_id,
                                    std::string stable_name);
std::string SerializeLogicalPlanToJson(const LogicalPlan& plan);
LogicalPlan BuildQueryShapePlan(const QueryShapeEvidence& evidence);

}  // namespace scratchbird::engine::planner

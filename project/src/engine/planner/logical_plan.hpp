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
#include <iterator>
#include <limits>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace scratchbird::engine::planner {

struct CanonicalMgaStatementContext {
  std::string statement_uuid;
  std::string owning_transaction_uuid;
  std::string statement_snapshot_uuid;
  std::string statement_metadata_snapshot_uuid;
  std::uint64_t owning_local_transaction_id{0};
  std::uint64_t visible_committed_high_watermark{0};
  std::uint64_t oldest_active_transaction_id{0};
  std::uint64_t oldest_interesting_transaction_id{0};
  std::uint64_t oldest_snapshot_transaction_id{0};
  std::uint64_t retention_horizon_transaction_id{0};
  std::vector<std::uint64_t> active_excluded_local_transaction_ids;
  std::vector<std::uint64_t> in_doubt_excluded_local_transaction_ids;
  std::string snapshot_kind;
  std::uint64_t publication_inventory_next_local_transaction_id{0};
  bool inventory_authoritative{false};
  bool complete{false};
  bool current{false};
  std::string statement_timestamp;
};

inline bool CanonicalMgaStatementContextEqual(
    const CanonicalMgaStatementContext& left,
    const CanonicalMgaStatementContext& right) {
  return left.statement_uuid == right.statement_uuid &&
         left.statement_timestamp == right.statement_timestamp &&
         left.owning_transaction_uuid == right.owning_transaction_uuid &&
         left.statement_snapshot_uuid == right.statement_snapshot_uuid &&
         left.statement_metadata_snapshot_uuid ==
             right.statement_metadata_snapshot_uuid &&
         left.owning_local_transaction_id ==
             right.owning_local_transaction_id &&
         left.visible_committed_high_watermark ==
             right.visible_committed_high_watermark &&
         left.oldest_active_transaction_id ==
             right.oldest_active_transaction_id &&
         left.oldest_interesting_transaction_id ==
             right.oldest_interesting_transaction_id &&
         left.oldest_snapshot_transaction_id ==
             right.oldest_snapshot_transaction_id &&
         left.retention_horizon_transaction_id ==
             right.retention_horizon_transaction_id &&
         left.active_excluded_local_transaction_ids ==
             right.active_excluded_local_transaction_ids &&
         left.in_doubt_excluded_local_transaction_ids ==
             right.in_doubt_excluded_local_transaction_ids &&
         left.snapshot_kind == right.snapshot_kind &&
         left.publication_inventory_next_local_transaction_id ==
             right.publication_inventory_next_local_transaction_id &&
         left.inventory_authoritative == right.inventory_authoritative &&
         left.complete == right.complete && left.current == right.current;
}

inline bool CanonicalMgaStatementContextStructurallyValid(
    const CanonicalMgaStatementContext& context,
    const bool require_current = false) {
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
    return value != "00000000-0000-0000-0000-000000000000";
  };
  if (!canonical_uuid(context.statement_uuid) ||
      !canonical_uuid(context.owning_transaction_uuid) ||
      !canonical_uuid(context.statement_snapshot_uuid) ||
      !canonical_uuid(context.statement_metadata_snapshot_uuid) ||
      context.owning_local_transaction_id == 0) {
    return false;
  }
  const bool has_descriptor_evidence =
      context.oldest_active_transaction_id != 0 ||
      context.oldest_interesting_transaction_id != 0 ||
      context.oldest_snapshot_transaction_id != 0 ||
      context.retention_horizon_transaction_id != 0 ||
      !context.active_excluded_local_transaction_ids.empty() ||
      !context.in_doubt_excluded_local_transaction_ids.empty() ||
      !context.snapshot_kind.empty() ||
      context.publication_inventory_next_local_transaction_id != 0 ||
      context.inventory_authoritative || context.complete || context.current;
  if (!has_descriptor_evidence) {
    return !require_current && !context.inventory_authoritative &&
           !context.complete && !context.current;
  }
  if (!context.inventory_authoritative || !context.complete ||
      (require_current && !context.current) ||
      context.snapshot_kind != "statement_stable" ||
      context.publication_inventory_next_local_transaction_id == 0 ||
      context.visible_committed_high_watermark >=
          context.publication_inventory_next_local_transaction_id ||
      context.owning_local_transaction_id >=
          context.publication_inventory_next_local_transaction_id ||
      context.oldest_active_transaction_id == 0 ||
      context.oldest_interesting_transaction_id == 0 ||
      context.oldest_snapshot_transaction_id == 0 ||
      context.retention_horizon_transaction_id == 0 ||
      context.oldest_active_transaction_id >=
          context.publication_inventory_next_local_transaction_id ||
      context.oldest_interesting_transaction_id >=
          context.publication_inventory_next_local_transaction_id ||
      context.oldest_snapshot_transaction_id >=
          context.publication_inventory_next_local_transaction_id ||
      context.retention_horizon_transaction_id >=
          context.publication_inventory_next_local_transaction_id ||
      context.oldest_snapshot_transaction_id !=
          context.retention_horizon_transaction_id ||
      context.oldest_snapshot_transaction_id !=
          std::min(context.oldest_interesting_transaction_id,
                   context.owning_local_transaction_id)) {
    return false;
  }
  const auto canonical_exclusions = [&](const auto& values) {
    return std::ranges::is_sorted(values) &&
           std::adjacent_find(values.begin(), values.end()) == values.end() &&
           std::ranges::all_of(values, [&](const std::uint64_t value) {
             return value != 0 &&
                    value <
                        context.publication_inventory_next_local_transaction_id;
           });
  };
  if (!canonical_exclusions(context.active_excluded_local_transaction_ids) ||
      !canonical_exclusions(
          context.in_doubt_excluded_local_transaction_ids) ||
      !std::ranges::binary_search(
          context.active_excluded_local_transaction_ids,
          context.owning_local_transaction_id)) {
    return false;
  }
  std::vector<std::uint64_t> intersection;
  std::ranges::set_intersection(
      context.active_excluded_local_transaction_ids,
      context.in_doubt_excluded_local_transaction_ids,
      std::back_inserter(intersection));
  return intersection.empty();
}

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

// QOW-SOURCE-RCP-074-CANONICAL-MODEL-FAMILY-IDENTITY-V1
// Closed semantic provenance populated only after exact typed-DAG model
// operation admission.  It does not grant planning, access, visibility, or
// transaction-finality authority.
enum class CanonicalLogicalModelFamilyIdentity : std::uint8_t {
  kUnspecified = 0,
  kDocument,
  kGraph,
  kKeyValue,
  kTimeSeries,
  kVector,
  kSearch,
  kSpatial,
  kColumnar,
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
  std::vector<std::string> required_property_uuids;
  std::vector<std::string> delivered_property_uuids;
  CanonicalLogicalModelFamilyIdentity model_family_identity{
      CanonicalLogicalModelFamilyIdentity::kUnspecified};
};

struct CanonicalLogicalRelationalGraph {
  std::uint16_t abi_version{1};
  std::string bound_sblr_tree_uuid;
  std::string catalog_epoch_uuid;
  std::string security_context_uuid;
  std::uint64_t local_transaction_id{0};
  std::uint64_t statement_snapshot_id{0};
  CanonicalMgaStatementContext mga_statement_context;
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
    return value != "00000000-0000-0000-0000-000000000000";
  };
  const auto known_kind = [](const CanonicalLogicalRelationalNodeKind kind) {
    return kind >= CanonicalLogicalRelationalNodeKind::kRelationSource &&
           kind <=
               CanonicalLogicalRelationalNodeKind::kTableFunctionInvoke;
  };
  const auto known_model_family = [](
                                      const CanonicalLogicalModelFamilyIdentity
                                          family) {
    return family == CanonicalLogicalModelFamilyIdentity::kUnspecified ||
           family == CanonicalLogicalModelFamilyIdentity::kDocument ||
           family == CanonicalLogicalModelFamilyIdentity::kGraph ||
           family == CanonicalLogicalModelFamilyIdentity::kKeyValue ||
           family == CanonicalLogicalModelFamilyIdentity::kTimeSeries ||
           family == CanonicalLogicalModelFamilyIdentity::kVector ||
           family == CanonicalLogicalModelFamilyIdentity::kSearch ||
           family == CanonicalLogicalModelFamilyIdentity::kSpatial ||
           family == CanonicalLogicalModelFamilyIdentity::kColumnar;
  };
  constexpr std::string_view kModelSourceSemantic =
      "SBLR_MODEL_SOURCE_V1";
  constexpr std::string_view kModelExpandSemantic =
      "SBLR_MODEL_EXPAND_V1";
  constexpr std::string_view kModelAggregateSemantic =
      "SBLR_MODEL_AGGREGATE_V1";
  const auto valid_semantic_variant = [](const std::string_view value) {
    if (value == kModelSourceSemantic || value == kModelExpandSemantic ||
        value == kModelAggregateSemantic) {
      return true;
    }
    // The model identities above are bound SBLR identities, not a general
    // relaxation of the canonical lowercase semantic-variant grammar.
    if (value == "sblr_model_source_v1" ||
        value == "sblr_model_expand_v1" ||
        value == "sblr_model_aggregate_v1") {
      return false;
    }
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
      !CanonicalMgaStatementContextStructurallyValid(
          graph.mga_statement_context) ||
      graph.catalog_epoch_uuid ==
          graph.mga_statement_context.statement_metadata_snapshot_uuid ||
      graph.local_transaction_id !=
          graph.mga_statement_context.owning_local_transaction_id ||
      graph.statement_snapshot_id !=
          graph.mga_statement_context.visible_committed_high_watermark) {
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
    const bool model_source =
        node.semantic_variant_id == kModelSourceSemantic;
    const bool model_expand =
        node.semantic_variant_id == kModelExpandSemantic;
    const bool model_aggregate =
        node.semantic_variant_id == kModelAggregateSemantic;
    const bool model_semantic =
        model_source || model_expand || model_aggregate;
    const bool graph_family =
        node.model_family_identity ==
        CanonicalLogicalModelFamilyIdentity::kGraph;
    const bool document_family =
        node.model_family_identity ==
        CanonicalLogicalModelFamilyIdentity::kDocument;
    const bool key_value_family =
        node.model_family_identity ==
        CanonicalLogicalModelFamilyIdentity::kKeyValue;
    const bool time_series_family =
        node.model_family_identity ==
        CanonicalLogicalModelFamilyIdentity::kTimeSeries;
    const bool vector_family =
        node.model_family_identity ==
        CanonicalLogicalModelFamilyIdentity::kVector;
    const bool search_family =
        node.model_family_identity ==
        CanonicalLogicalModelFamilyIdentity::kSearch;
    const bool spatial_or_columnar_family =
        node.model_family_identity ==
            CanonicalLogicalModelFamilyIdentity::kSpatial ||
        node.model_family_identity ==
            CanonicalLogicalModelFamilyIdentity::kColumnar;
    const bool exact_time_series_attachment_width =
        time_series_family &&
        ((model_source &&
          ((node.output_descriptor_ids.size() == 6 &&
            node.bound_expression_ids.size() == 7) ||
           (node.output_descriptor_ids.size() == 1 &&
            node.bound_expression_ids.size() == 6) ||
           // RCP-080 bounded multileg range: the admitted bridge has proved
           // the exact ordered output prefix plus TIME_RANGE, object alias,
           // and the ordered non-null temporal start/end operands.
           (node.bound_expression_ids.size() ==
            node.output_descriptor_ids.size() + 4))) ||
           (model_aggregate && node.output_descriptor_ids.size() == 7 &&
          (node.bound_expression_ids.size() == 9 ||
           node.bound_expression_ids.size() == 10)));
    const bool exact_vector_attachment_width =
        vector_family && model_source &&
        node.output_descriptor_ids.size() == 3 &&
        (node.bound_expression_ids.size() == 8 ||
         node.bound_expression_ids.size() == 14);
    const bool exact_search_attachment_width =
        search_family && model_source &&
        node.output_descriptor_ids.size() == 5 &&
        (node.bound_expression_ids.size() == 12 ||
         node.bound_expression_ids.size() == 13 ||
         node.bound_expression_ids.size() == 17 ||
         node.bound_expression_ids.size() == 18 ||
         // RCP-080 bounded multileg term search: the admitted typed-DAG
         // bridge has proved the exact ordered five-output prefix plus the
         // six-record SEARCH_MATCH-owned closure.  This is not a generic
         // output-plus-six exception.
         node.bound_expression_ids.size() ==
             node.output_descriptor_ids.size() + 6);
    // RCP-079 spatial/columnar relations carry the public output
    // expressions first, followed by one to three functionless operation
    // roots and their exact typed operands.  The admitted typed-DAG bridge
    // has already validated root order, arity, attachment, and reachability;
    // retain that complete vector here instead of applying the legacy
    // one-expression-per-output fallback used by older model families.
    const bool exact_spatial_columnar_attachment_width =
        spatial_or_columnar_family && model_source &&
        node.output_descriptor_ids.size() <= 256 &&
        node.bound_expression_ids.size() > node.output_descriptor_ids.size() &&
        node.bound_expression_ids.size() <= 1024;
    // RCP-080 bounded multileg source nodes retain their complete admitted
    // typed-operation closure after the public output expressions.  The
    // earlier typed-DAG validator has already proved the exact ordered output
    // prefix and owned suffix membership in addition to functionless roots,
    // operands, identity, descriptors, reachability, and single-leg
    // ownership.  Admit only the closed family-specific widths here; never
    // narrow, strip, or replace that semantic closure.
    const bool exact_document_graph_key_value_attachment_width =
        model_source &&
        node.bound_expression_ids.size() > node.output_descriptor_ids.size() &&
        ((document_family &&
          node.bound_expression_ids.size() -
                  node.output_descriptor_ids.size() ==
              2) ||
         (graph_family &&
          node.bound_expression_ids.size() -
                  node.output_descriptor_ids.size() ==
              3) ||
         (key_value_family &&
          node.bound_expression_ids.size() -
                  node.output_descriptor_ids.size() ==
              4));
    if (!known_model_family(node.model_family_identity) ||
        (!model_semantic &&
         node.model_family_identity !=
             CanonicalLogicalModelFamilyIdentity::kUnspecified) ||
        (model_semantic &&
         (((model_source || model_expand) &&
           node.node_kind !=
               CanonicalLogicalRelationalNodeKind::kRelationSource) ||
          (model_aggregate &&
           node.node_kind != CanonicalLogicalRelationalNodeKind::kAggregate) ||
          (model_aggregate && !time_series_family) ||
          !node.input_logical_node_ids.empty() ||
          node.bound_expression_ids.empty() ||
          node.output_descriptor_ids.empty() ||
          (!exact_time_series_attachment_width &&
           !exact_vector_attachment_width &&
           !exact_search_attachment_width &&
           !exact_spatial_columnar_attachment_width &&
           !exact_document_graph_key_value_attachment_width &&
           node.bound_expression_ids.size() !=
               node.output_descriptor_ids.size()) ||
          (model_source &&
           (node.required_object_uuids.size() != 1 ||
            !canonical_uuid(node.required_object_uuids.front()))) ||
          (model_aggregate &&
           (node.required_object_uuids.size() != 1 ||
            !canonical_uuid(node.required_object_uuids.front()))) ||
          (model_expand && graph_family &&
           (node.required_object_uuids.size() != 1 ||
            !canonical_uuid(node.required_object_uuids.front()))) ||
          (model_expand && !graph_family &&
           !node.required_object_uuids.empty())))) {
      return refuse("SBLR.PLAN_TREE.INVALID_HANDLE", node.logical_node_id,
                    "model_semantic_node_shape");
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
    for (std::size_t role = 0; role < node.bound_expression_ids.size();
         ++role) {
      const auto expression_id = node.bound_expression_ids[role];
      const bool inserted = expression_ids.insert(expression_id).second;
      const bool exact_same_column_value_window_role =
          !inserted && role == 1 &&
          node.node_kind == CanonicalLogicalRelationalNodeKind::kWindow &&
          (node.semantic_variant_id == "window.lag.v1" ||
           node.semantic_variant_id == "window.lead.v1" ||
           node.semantic_variant_id == "window.first-value.v1" ||
           node.semantic_variant_id == "window.last-value.v1") &&
          node.bound_expression_ids.size() == 3 &&
          node.bound_expression_ids[0] == node.bound_expression_ids[1] &&
          node.bound_expression_ids[1] != node.bound_expression_ids[2];
      if (expression_id == 0 ||
          (!inserted && !exact_same_column_value_window_role)) {
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
  std::vector<std::string> required_property_uuids;
  std::vector<std::string> delivered_property_uuids;
};

struct CanonicalPhysicalAlternativeCatalog {
  std::uint16_t abi_version{1};
  std::string bound_sblr_tree_uuid;
  std::string catalog_epoch_uuid;
  std::string security_context_uuid;
  std::uint64_t local_transaction_id{0};
  std::uint64_t statement_snapshot_id{0};
  CanonicalMgaStatementContext mga_statement_context;
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
    return value != "00000000-0000-0000-0000-000000000000";
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
      catalog.statement_snapshot_id != graph.statement_snapshot_id ||
      !CanonicalMgaStatementContextEqual(catalog.mga_statement_context,
                                         graph.mga_statement_context)) {
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

enum class CanonicalLogicalPropertyKind : std::uint8_t {
  kOrdering = 1,
  kGrouping,
  kPartitioning,
  kWindow,
  kExpressionEquivalence,
};

enum class CanonicalLogicalPropertySortDirection : std::uint8_t {
  kAscending = 1,
  kDescending,
};

enum class CanonicalLogicalPropertyNullPlacement : std::uint8_t {
  kNullsFirst = 1,
  kNullsLast,
};

struct CanonicalLogicalPropertyOrderingTerm {
  std::uint32_t expression_id{0};
  CanonicalLogicalPropertySortDirection direction{
      CanonicalLogicalPropertySortDirection::kAscending};
  CanonicalLogicalPropertyNullPlacement null_placement{
      CanonicalLogicalPropertyNullPlacement::kNullsLast};
  std::string collation_uuid;
};

struct CanonicalLogicalPropertyRecord {
  std::string property_uuid;
  CanonicalLogicalPropertyKind property_kind{
      CanonicalLogicalPropertyKind::kOrdering};
  std::uint32_t origin_logical_node_id{0};
  std::vector<std::uint32_t> expression_ids;
  std::vector<CanonicalLogicalPropertyOrderingTerm> ordering_terms;
  std::vector<std::string> dependency_property_uuids;
  std::string window_frame_descriptor_uuid;
  bool populated_from_bound_sblr{false};
};

struct CanonicalLogicalPropertyCatalog {
  std::uint16_t abi_version{1};
  std::string bound_sblr_tree_uuid;
  std::string catalog_epoch_uuid;
  std::string security_context_uuid;
  std::uint64_t local_transaction_id{0};
  std::uint64_t statement_snapshot_id{0};
  CanonicalMgaStatementContext mga_statement_context;
  std::vector<CanonicalLogicalPropertyRecord> properties;
  bool raw_sql_text_present{false};
  bool parser_execution_authority_claimed{false};
  bool transaction_finality_authority_claimed{false};
};

struct CanonicalLogicalNodePropertyBinding {
  std::uint32_t logical_node_id{0};
  std::vector<std::string> required_property_uuids;
  std::vector<std::string> delivered_property_uuids;
};

struct CanonicalLogicalPropertyIssue {
  std::string diagnostic_id;
  std::uint32_t logical_node_id{0};
  std::string field_id;
};

struct CanonicalLogicalPropertyValidationResult {
  bool accepted{false};
  std::size_t validated_property_count{0};
  std::vector<CanonicalLogicalPropertyIssue> issues;
};

struct CanonicalLogicalPropertyPopulationResult {
  bool accepted{false};
  CanonicalLogicalRelationalGraph logical_graph;
  CanonicalLogicalPropertyCatalog property_catalog;
  std::vector<CanonicalLogicalPropertyIssue> issues;
};

inline const char* CanonicalLogicalPropertyKindName(
    const CanonicalLogicalPropertyKind kind) {
  switch (kind) {
    case CanonicalLogicalPropertyKind::kOrdering:
      return "ordering";
    case CanonicalLogicalPropertyKind::kGrouping:
      return "grouping";
    case CanonicalLogicalPropertyKind::kPartitioning:
      return "partitioning";
    case CanonicalLogicalPropertyKind::kWindow:
      return "window";
    case CanonicalLogicalPropertyKind::kExpressionEquivalence:
      return "expression_equivalence";
  }
  return "unknown";
}

inline std::string SerializeCanonicalLogicalPropertyIdentity(
    const CanonicalLogicalPropertyRecord& property) {
  std::vector<std::uint32_t> expression_ids = property.expression_ids;
  if (property.property_kind == CanonicalLogicalPropertyKind::kGrouping ||
      property.property_kind ==
          CanonicalLogicalPropertyKind::kPartitioning ||
      property.property_kind ==
          CanonicalLogicalPropertyKind::kExpressionEquivalence) {
    std::ranges::sort(expression_ids);
  }
  std::vector<std::string> dependencies =
      property.dependency_property_uuids;
  std::ranges::sort(dependencies);
  std::string serialized =
      "logical-property-v1|" + property.property_uuid + "|" +
      CanonicalLogicalPropertyKindName(property.property_kind) + "|" +
      std::to_string(property.origin_logical_node_id) + "|expressions=";
  for (const auto expression_id : expression_ids) {
    serialized += std::to_string(expression_id) + ",";
  }
  serialized += "|ordering=";
  for (const auto& term : property.ordering_terms) {
    serialized += std::to_string(term.expression_id) + ":" +
                  (term.direction ==
                           CanonicalLogicalPropertySortDirection::kAscending
                       ? "asc"
                       : "desc") +
                  ":" +
                  (term.null_placement ==
                           CanonicalLogicalPropertyNullPlacement::kNullsFirst
                       ? "nulls_first"
                       : "nulls_last") +
                  ":" + term.collation_uuid + ",";
  }
  serialized += "|dependencies=";
  for (const auto& dependency : dependencies) {
    serialized += dependency + ",";
  }
  serialized += "|frame=" + property.window_frame_descriptor_uuid;
  return serialized;
}

// QOW-SOURCE-OPT-003-V1
// Stable property identities are scoped to one bound SBLR tree and its exact
// catalog/security/MGA statement boundary.  Every property must be populated
// from typed bound SBLR records; prebound/manual node metadata is rejected by
// PopulateCanonicalLogicalPropertiesFromBoundSblr below.
inline CanonicalLogicalPropertyValidationResult
ValidateCanonicalLogicalPropertyCatalog(
    const CanonicalLogicalRelationalGraph& graph,
    const CanonicalLogicalPropertyCatalog& catalog,
    const std::size_t maximum_properties = 524288,
    const std::size_t maximum_property_references = 1048576) {
  CanonicalLogicalPropertyValidationResult result;
  const auto refuse = [&](std::string diagnostic_id,
                          const std::uint32_t node_id,
                          std::string field_id) {
    result.accepted = false;
    result.validated_property_count = 0;
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
    return value != "00000000-0000-0000-0000-000000000000";
  };
  const auto graph_validation = ValidateCanonicalLogicalRelationalGraph(graph);
  if (!graph_validation.accepted) {
    const auto& issue = graph_validation.issues.front();
    return refuse(issue.diagnostic_id, issue.logical_node_id, issue.field_id);
  }
  if (catalog.abi_version != 1) {
    return refuse("QOW-DIAG-LOGICAL-PROPERTY-VERSION-V1", 0,
                  "abi_version");
  }
  if (catalog.bound_sblr_tree_uuid != graph.bound_sblr_tree_uuid ||
      catalog.catalog_epoch_uuid != graph.catalog_epoch_uuid ||
      catalog.security_context_uuid != graph.security_context_uuid ||
      catalog.local_transaction_id != graph.local_transaction_id ||
      catalog.statement_snapshot_id != graph.statement_snapshot_id ||
      !CanonicalMgaStatementContextEqual(catalog.mga_statement_context,
                                         graph.mga_statement_context)) {
    return refuse("QOW-DIAG-LOGICAL-PROPERTY-SCOPE-V1", 0,
                  "bound_property_scope");
  }
  if (catalog.raw_sql_text_present ||
      catalog.parser_execution_authority_claimed ||
      catalog.transaction_finality_authority_claimed) {
    return refuse("QOW-DIAG-LOGICAL-PROPERTY-AUTHORITY-V1", 0,
                  "forbidden_authority_claim");
  }
  if (maximum_properties == 0 || maximum_property_references == 0 ||
      catalog.properties.size() > maximum_properties) {
    return refuse("SBLR.PLAN_TREE.RESOURCE_LIMIT", 0, "property_count");
  }

  std::unordered_map<std::uint32_t, const CanonicalLogicalRelationalNode*>
      nodes_by_id;
  std::unordered_set<std::uint32_t> bound_expression_ids;
  for (const auto& node : graph.nodes) {
    nodes_by_id.emplace(node.logical_node_id, &node);
    bound_expression_ids.insert(node.bound_expression_ids.begin(),
                                node.bound_expression_ids.end());
  }
  std::unordered_map<std::string, const CanonicalLogicalPropertyRecord*>
      properties_by_uuid;
  std::size_t property_reference_count = 0;
  for (const auto& property : catalog.properties) {
    const auto add_references = [&](const std::size_t count) {
      if (count > maximum_property_references - property_reference_count) {
        return false;
      }
      property_reference_count += count;
      return true;
    };
    if (!add_references(property.expression_ids.size()) ||
        !add_references(property.ordering_terms.size()) ||
        !add_references(property.dependency_property_uuids.size())) {
      return refuse("SBLR.PLAN_TREE.RESOURCE_LIMIT",
                    property.origin_logical_node_id,
                    "property_reference_count");
    }
    if (!canonical_uuid(property.property_uuid) ||
        !properties_by_uuid.emplace(property.property_uuid, &property).second ||
        !nodes_by_id.contains(property.origin_logical_node_id) ||
        property.property_kind < CanonicalLogicalPropertyKind::kOrdering ||
        property.property_kind >
            CanonicalLogicalPropertyKind::kExpressionEquivalence ||
        !property.populated_from_bound_sblr) {
      return refuse("QOW-DIAG-LOGICAL-PROPERTY-IDENTITY-V1",
                    property.origin_logical_node_id, "property_record");
    }
    std::unordered_set<std::uint32_t> expressions;
    for (const auto expression_id : property.expression_ids) {
      if (expression_id == 0 ||
          !bound_expression_ids.contains(expression_id) ||
          !expressions.insert(expression_id).second) {
        return refuse("QOW-DIAG-LOGICAL-PROPERTY-EXPRESSION-V1",
                      property.origin_logical_node_id, "expression_ids");
      }
    }
    for (const auto& term : property.ordering_terms) {
      if (term.expression_id == 0 ||
          !bound_expression_ids.contains(term.expression_id) ||
          !expressions.insert(term.expression_id).second ||
          (term.direction !=
               CanonicalLogicalPropertySortDirection::kAscending &&
           term.direction !=
               CanonicalLogicalPropertySortDirection::kDescending) ||
          (term.null_placement !=
               CanonicalLogicalPropertyNullPlacement::kNullsFirst &&
           term.null_placement !=
               CanonicalLogicalPropertyNullPlacement::kNullsLast) ||
          (!term.collation_uuid.empty() &&
           !canonical_uuid(term.collation_uuid))) {
        return refuse("QOW-DIAG-LOGICAL-PROPERTY-ORDERING-V1",
                      property.origin_logical_node_id, "ordering_terms");
      }
    }
    std::unordered_set<std::string> dependencies;
    for (const auto& dependency : property.dependency_property_uuids) {
      if (!canonical_uuid(dependency) ||
          !dependencies.insert(dependency).second ||
          dependency == property.property_uuid) {
        return refuse("QOW-DIAG-LOGICAL-PROPERTY-DEPENDENCY-V1",
                      property.origin_logical_node_id,
                      "dependency_property_uuids");
      }
    }
    const bool ordering =
        property.property_kind == CanonicalLogicalPropertyKind::kOrdering;
    const bool expression_set =
        property.property_kind == CanonicalLogicalPropertyKind::kGrouping ||
        property.property_kind ==
            CanonicalLogicalPropertyKind::kPartitioning ||
        property.property_kind ==
            CanonicalLogicalPropertyKind::kExpressionEquivalence;
    const bool window =
        property.property_kind == CanonicalLogicalPropertyKind::kWindow;
    const bool shape_valid =
        (ordering && property.expression_ids.empty() &&
         !property.ordering_terms.empty() &&
         property.dependency_property_uuids.empty() &&
         property.window_frame_descriptor_uuid.empty()) ||
        (expression_set &&
         property.expression_ids.size() >=
             (property.property_kind ==
                      CanonicalLogicalPropertyKind::kExpressionEquivalence
                  ? 2U
                  : 1U) &&
         property.ordering_terms.empty() &&
         property.dependency_property_uuids.empty() &&
         property.window_frame_descriptor_uuid.empty()) ||
        (window && property.expression_ids.empty() &&
         property.ordering_terms.empty() &&
         !property.dependency_property_uuids.empty() &&
         property.dependency_property_uuids.size() <= 2 &&
         canonical_uuid(property.window_frame_descriptor_uuid));
    if (!shape_valid) {
      return refuse("QOW-DIAG-LOGICAL-PROPERTY-SHAPE-V1",
                    property.origin_logical_node_id, "property_shape");
    }
  }

  for (const auto& property : catalog.properties) {
    if (property.property_kind != CanonicalLogicalPropertyKind::kWindow) {
      continue;
    }
    bool saw_ordering = false;
    bool saw_partitioning = false;
    for (const auto& dependency_uuid : property.dependency_property_uuids) {
      const auto dependency = properties_by_uuid.find(dependency_uuid);
      if (dependency == properties_by_uuid.end()) {
        return refuse("QOW-DIAG-LOGICAL-PROPERTY-DEPENDENCY-V1",
                      property.origin_logical_node_id,
                      "unknown_window_dependency");
      }
      if (dependency->second->property_kind ==
          CanonicalLogicalPropertyKind::kOrdering) {
        if (saw_ordering) {
          return refuse("QOW-DIAG-LOGICAL-PROPERTY-DEPENDENCY-V1",
                        property.origin_logical_node_id,
                        "duplicate_window_ordering");
        }
        saw_ordering = true;
      } else if (dependency->second->property_kind ==
                 CanonicalLogicalPropertyKind::kPartitioning) {
        if (saw_partitioning) {
          return refuse("QOW-DIAG-LOGICAL-PROPERTY-DEPENDENCY-V1",
                        property.origin_logical_node_id,
                        "duplicate_window_partitioning");
        }
        saw_partitioning = true;
      } else {
        return refuse("QOW-DIAG-LOGICAL-PROPERTY-DEPENDENCY-V1",
                      property.origin_logical_node_id,
                      "window_dependency_kind");
      }
    }
  }

  std::unordered_set<std::string> referenced_properties;
  for (const auto& node : graph.nodes) {
    if (node.required_property_uuids.size() >
            maximum_property_references - property_reference_count) {
      return refuse("SBLR.PLAN_TREE.RESOURCE_LIMIT",
                    node.logical_node_id, "property_reference_count");
    }
    property_reference_count += node.required_property_uuids.size();
    if (node.delivered_property_uuids.size() >
            maximum_property_references - property_reference_count) {
      return refuse("SBLR.PLAN_TREE.RESOURCE_LIMIT",
                    node.logical_node_id, "property_reference_count");
    }
    property_reference_count += node.delivered_property_uuids.size();
    std::unordered_set<std::string> node_refs;
    for (const auto& property_uuid : node.required_property_uuids) {
      if (!node_refs.insert("r:" + property_uuid).second ||
          !properties_by_uuid.contains(property_uuid)) {
        return refuse("QOW-DIAG-LOGICAL-PROPERTY-REFERENCE-V1",
                      node.logical_node_id, "required_property_uuids");
      }
      referenced_properties.insert(property_uuid);
    }
    for (const auto& property_uuid : node.delivered_property_uuids) {
      if (!node_refs.insert("d:" + property_uuid).second ||
          !properties_by_uuid.contains(property_uuid)) {
        return refuse("QOW-DIAG-LOGICAL-PROPERTY-REFERENCE-V1",
                      node.logical_node_id, "delivered_property_uuids");
      }
      referenced_properties.insert(property_uuid);
      const auto* property = properties_by_uuid.at(property_uuid);
      if (property->origin_logical_node_id == node.logical_node_id) continue;
      const bool delivered_by_input =
          std::ranges::any_of(node.input_logical_node_ids,
                              [&](const std::uint32_t input_id) {
            const auto* input = nodes_by_id.at(input_id);
            return std::ranges::find(input->delivered_property_uuids,
                                     property_uuid) !=
                   input->delivered_property_uuids.end();
          });
      if (!delivered_by_input) {
        return refuse("QOW-DIAG-LOGICAL-PROPERTY-PROPAGATION-V1",
                      node.logical_node_id, property_uuid);
      }
    }
  }
  if (referenced_properties.size() != catalog.properties.size()) {
    return refuse("QOW-DIAG-LOGICAL-PROPERTY-REFERENCE-V1", 0,
                  "orphan_property");
  }

  result.accepted = true;
  result.validated_property_count = catalog.properties.size();
  return result;
}

struct CanonicalLogicalPropertySerializationResult {
  bool accepted{false};
  std::string canonical_serialization;
  std::vector<CanonicalLogicalPropertyIssue> issues;
};

// QOW-SOURCE-IAS-006-V1
// Serializes the complete adopted property state after validation. Semantic
// sets and catalog record order are canonicalized; ordering-term order remains
// significant. The exact bound SBLR/catalog/security/MGA scope is part of the
// preimage, so any invalidating scope change produces refusal, not reuse.
inline std::string SerializeCanonicalMgaStatementContext(
    const CanonicalMgaStatementContext& context) {
  std::string serialized =
      "mga-v1|statement=" + context.statement_uuid +
      "|owner_uuid=" + context.owning_transaction_uuid +
      "|snapshot_uuid=" + context.statement_snapshot_uuid +
      "|metadata_uuid=" + context.statement_metadata_snapshot_uuid +
      "|owner_local=" +
      std::to_string(context.owning_local_transaction_id) +
      "|visible_high_water=" +
      std::to_string(context.visible_committed_high_watermark) +
      "|oldest_active=" +
      std::to_string(context.oldest_active_transaction_id) +
      "|oldest_interesting=" +
      std::to_string(context.oldest_interesting_transaction_id) +
      "|oldest_snapshot=" +
      std::to_string(context.oldest_snapshot_transaction_id) +
      "|retention=" +
      std::to_string(context.retention_horizon_transaction_id) +
      "|active_excluded=";
  for (const auto id : context.active_excluded_local_transaction_ids) {
    serialized += std::to_string(id) + ",";
  }
  if (!context.statement_timestamp.empty()) {
    serialized += "|statement_timestamp=" + context.statement_timestamp;
  }
  serialized += "|in_doubt_excluded=";
  for (const auto id : context.in_doubt_excluded_local_transaction_ids) {
    serialized += std::to_string(id) + ",";
  }
  serialized +=
      "|kind=" + context.snapshot_kind + "|publication_next=" +
      std::to_string(
          context.publication_inventory_next_local_transaction_id) +
      "|inventory_authoritative=" +
      (context.inventory_authoritative ? "true" : "false") +
      "|complete=" + (context.complete ? "true" : "false") +
      "|current=" + (context.current ? "true" : "false");
  return serialized;
}

inline CanonicalLogicalPropertySerializationResult
SerializeCanonicalLogicalPropertyCatalog(
    const CanonicalLogicalRelationalGraph& graph,
    const CanonicalLogicalPropertyCatalog& catalog) {
  CanonicalLogicalPropertySerializationResult result;
  const auto validation =
      ValidateCanonicalLogicalPropertyCatalog(graph, catalog);
  if (!validation.accepted) {
    result.issues = validation.issues;
    return result;
  }
  std::string serialized =
      "logical-property-catalog-v1|bound_sblr=" +
      catalog.bound_sblr_tree_uuid + "|catalog=" + catalog.catalog_epoch_uuid +
      "|security=" + catalog.security_context_uuid + "|transaction=" +
      std::to_string(catalog.local_transaction_id) + "|snapshot=" +
      std::to_string(catalog.statement_snapshot_id) + "|" +
      SerializeCanonicalMgaStatementContext(
          catalog.mga_statement_context) + "|";

  std::vector<const CanonicalLogicalPropertyRecord*> properties;
  properties.reserve(catalog.properties.size());
  for (const auto& property : catalog.properties) {
    properties.push_back(&property);
  }
  std::ranges::sort(properties, {},
                    &CanonicalLogicalPropertyRecord::property_uuid);
  for (const auto* property : properties) {
    const auto identity = SerializeCanonicalLogicalPropertyIdentity(*property);
    serialized += "property=" + std::to_string(identity.size()) + ":" +
                  identity + ";";
  }

  std::vector<const CanonicalLogicalRelationalNode*> nodes;
  nodes.reserve(graph.nodes.size());
  for (const auto& node : graph.nodes) nodes.push_back(&node);
  std::ranges::sort(nodes, {},
                    &CanonicalLogicalRelationalNode::logical_node_id);
  for (const auto* node : nodes) {
    auto required = node->required_property_uuids;
    auto delivered = node->delivered_property_uuids;
    std::ranges::sort(required);
    std::ranges::sort(delivered);
    serialized += "node=" + std::to_string(node->logical_node_id) +
                  "|required=";
    for (const auto& property_uuid : required) {
      serialized += property_uuid + ",";
    }
    serialized += "|delivered=";
    for (const auto& property_uuid : delivered) {
      serialized += property_uuid + ",";
    }
    serialized += ";";
  }
  result.accepted = true;
  result.canonical_serialization = std::move(serialized);
  return result;
}

inline CanonicalLogicalPropertyPopulationResult
PopulateCanonicalLogicalPropertiesFromBoundSblr(
    CanonicalLogicalRelationalGraph graph,
    CanonicalLogicalPropertyCatalog catalog,
    const std::vector<CanonicalLogicalNodePropertyBinding>& bindings) {
  CanonicalLogicalPropertyPopulationResult result;
  result.logical_graph = std::move(graph);
  result.property_catalog = std::move(catalog);
  const auto refuse = [&](std::string diagnostic_id,
                          const std::uint32_t node_id,
                          std::string field_id) {
    result.accepted = false;
    result.issues.push_back({std::move(diagnostic_id), node_id,
                             std::move(field_id)});
    return result;
  };
  std::unordered_map<std::uint32_t, CanonicalLogicalRelationalNode*>
      nodes_by_id;
  for (auto& node : result.logical_graph.nodes) {
    if (!node.required_property_uuids.empty() ||
        !node.delivered_property_uuids.empty()) {
      return refuse("QOW-DIAG-LOGICAL-PROPERTY-PREBOUND-V1",
                    node.logical_node_id, "manual_property_metadata");
    }
    nodes_by_id.emplace(node.logical_node_id, &node);
  }
  std::unordered_set<std::uint32_t> bound_nodes;
  for (const auto& binding : bindings) {
    const auto node = nodes_by_id.find(binding.logical_node_id);
    if (node == nodes_by_id.end() ||
        !bound_nodes.insert(binding.logical_node_id).second) {
      return refuse("QOW-DIAG-LOGICAL-PROPERTY-BINDING-V1",
                    binding.logical_node_id, "logical_node_id");
    }
    node->second->required_property_uuids =
        binding.required_property_uuids;
    node->second->delivered_property_uuids =
        binding.delivered_property_uuids;
  }
  const auto validation = ValidateCanonicalLogicalPropertyCatalog(
      result.logical_graph, result.property_catalog);
  if (!validation.accepted) {
    result.issues = validation.issues;
    return result;
  }
  result.accepted = true;
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

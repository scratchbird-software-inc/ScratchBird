// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "physical_plan.hpp"

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
      case '\n': out << "\\n"; break;
      case '\r': out << "\\r"; break;
      case '\t': out << "\\t"; break;
      default: out << ch;
    }
  }
  return out.str();
}

std::string Indent(std::size_t count) {
  return std::string(count, ' ');
}

bool IsStorageAccessKind(planner::PhysicalAccessKind access_kind) {
  switch (access_kind) {
    case planner::PhysicalAccessKind::kCatalogUuidLookup:
    case planner::PhysicalAccessKind::kTableScan:
    case planner::PhysicalAccessKind::kRowUuidLookup:
    case planner::PhysicalAccessKind::kScalarBtreeLookup:
    case planner::PhysicalAccessKind::kScalarHashLookup:
    case planner::PhysicalAccessKind::kScalarBtreeRange:
    case planner::PhysicalAccessKind::kCoveringIndexScan:
    case planner::PhysicalAccessKind::kBitmapSummaryScan:
    case planner::PhysicalAccessKind::kFullTextProbe:
    case planner::PhysicalAccessKind::kVectorExactSearch:
    case planner::PhysicalAccessKind::kVectorApproximateWithFallback:
    case planner::PhysicalAccessKind::kDocumentPathProbe:
    case planner::PhysicalAccessKind::kGraphTraversalSeed:
    case planner::PhysicalAccessKind::kTimeSeriesAppendPath:
    case planner::PhysicalAccessKind::kClusterFragmentScan:
    case planner::PhysicalAccessKind::kRemoteNodePushdown:
      return true;
    case planner::PhysicalAccessKind::kNone:
    case planner::PhysicalAccessKind::kJoinNestedLoop:
    case planner::PhysicalAccessKind::kJoinHash:
    case planner::PhysicalAccessKind::kJoinMerge:
    case planner::PhysicalAccessKind::kAggregateGeneric:
    case planner::PhysicalAccessKind::kAggregateHash:
    case planner::PhysicalAccessKind::kSort:
    case planner::PhysicalAccessKind::kTopN:
    case planner::PhysicalAccessKind::kSortThenWindow:
    case planner::PhysicalAccessKind::kCteInline:
    case planner::PhysicalAccessKind::kCteMaterialize:
    case planner::PhysicalAccessKind::kSetOperation:
      return false;
  }
  return false;
}

bool IsJoinAccessKind(planner::PhysicalAccessKind access_kind) {
  return access_kind == planner::PhysicalAccessKind::kJoinNestedLoop ||
         access_kind == planner::PhysicalAccessKind::kJoinHash ||
         access_kind == planner::PhysicalAccessKind::kJoinMerge;
}

bool IsUnaryUpperAccessKind(planner::PhysicalAccessKind access_kind) {
  return access_kind == planner::PhysicalAccessKind::kAggregateGeneric ||
         access_kind == planner::PhysicalAccessKind::kAggregateHash ||
         access_kind == planner::PhysicalAccessKind::kSort ||
         access_kind == planner::PhysicalAccessKind::kTopN ||
         access_kind == planner::PhysicalAccessKind::kSortThenWindow ||
         access_kind == planner::PhysicalAccessKind::kCteInline ||
         access_kind == planner::PhysicalAccessKind::kCteMaterialize;
}

bool IsMaterializingAccessKind(planner::PhysicalAccessKind access_kind) {
  return access_kind == planner::PhysicalAccessKind::kJoinHash ||
         access_kind == planner::PhysicalAccessKind::kAggregateHash ||
         access_kind == planner::PhysicalAccessKind::kSort ||
         access_kind == planner::PhysicalAccessKind::kTopN ||
         access_kind == planner::PhysicalAccessKind::kSortThenWindow ||
         access_kind == planner::PhysicalAccessKind::kCteMaterialize;
}

bool PreservesOrderByDefault(planner::PhysicalAccessKind access_kind) {
  return access_kind == planner::PhysicalAccessKind::kScalarBtreeRange ||
         access_kind == planner::PhysicalAccessKind::kCoveringIndexScan ||
         access_kind == planner::PhysicalAccessKind::kJoinMerge ||
         access_kind == planner::PhysicalAccessKind::kSort ||
         access_kind == planner::PhysicalAccessKind::kTopN ||
         access_kind == planner::PhysicalAccessKind::kSortThenWindow;
}

}  // namespace

const char* RequiredExecutorCapabilityForAccessKind(planner::PhysicalAccessKind access_kind) {
  switch (access_kind) {
    case planner::PhysicalAccessKind::kNone: return "constant_result";
    case planner::PhysicalAccessKind::kCatalogUuidLookup: return "catalog_lookup";
    case planner::PhysicalAccessKind::kTableScan: return "table_scan";
    case planner::PhysicalAccessKind::kRowUuidLookup: return "row_uuid_lookup";
    case planner::PhysicalAccessKind::kScalarBtreeLookup: return "index_lookup";
    case planner::PhysicalAccessKind::kScalarHashLookup: return "hash_index_lookup";
    case planner::PhysicalAccessKind::kScalarBtreeRange: return "index_range_scan";
    case planner::PhysicalAccessKind::kCoveringIndexScan: return "covering_index_scan";
    case planner::PhysicalAccessKind::kBitmapSummaryScan: return "bitmap_summary_scan";
    case planner::PhysicalAccessKind::kFullTextProbe: return "full_text_probe";
    case planner::PhysicalAccessKind::kVectorExactSearch: return "vector_exact_search";
    case planner::PhysicalAccessKind::kVectorApproximateWithFallback: return "vector_approximate_with_fallback";
    case planner::PhysicalAccessKind::kDocumentPathProbe: return "document_path_probe";
    case planner::PhysicalAccessKind::kGraphTraversalSeed: return "graph_traversal_seed";
    case planner::PhysicalAccessKind::kTimeSeriesAppendPath: return "time_series_append_path";
    case planner::PhysicalAccessKind::kJoinNestedLoop: return "nested_loop_join";
    case planner::PhysicalAccessKind::kJoinHash: return "hash_join";
    case planner::PhysicalAccessKind::kJoinMerge: return "merge_join";
    case planner::PhysicalAccessKind::kAggregateGeneric: return "aggregate";
    case planner::PhysicalAccessKind::kAggregateHash: return "hash_aggregate";
    case planner::PhysicalAccessKind::kSort: return "sort";
    case planner::PhysicalAccessKind::kTopN: return "limit_offset";
    case planner::PhysicalAccessKind::kSortThenWindow: return "window";
    case planner::PhysicalAccessKind::kCteInline: return "cte_inline";
    case planner::PhysicalAccessKind::kCteMaterialize: return "cte_materialize";
    case planner::PhysicalAccessKind::kSetOperation: return "set_operation";
    case planner::PhysicalAccessKind::kClusterFragmentScan: return "cluster_fragment_scan";
    case planner::PhysicalAccessKind::kRemoteNodePushdown: return "remote_node_pushdown";
  }
  return "unknown";
}

PhysicalPlanNode PhysicalPlanNodeFromCandidate(const PlanCandidate& candidate,
                                               std::string executor_capability_id,
                                               std::string descriptor_digest) {
  PhysicalPlanNode node;
  node.node_id = candidate.candidate_id;
  node.access_kind = candidate.access_kind;
  node.executor_capability_id = std::move(executor_capability_id);
  node.descriptor_digest = std::move(descriptor_digest);
  node.cost = candidate.cost;
  node.estimated_rows = candidate.estimated_rows;
  node.storage_backed = IsStorageAccessKind(candidate.access_kind);
  node.materializes = IsMaterializingAccessKind(candidate.access_kind);
  node.preserves_order = PreservesOrderByDefault(candidate.access_kind);
  node.runtime_evidence.push_back("selected_candidate_id=" + candidate.candidate_id);
  node.runtime_evidence.push_back(std::string("selected_access_kind=") + planner::PhysicalAccessKindName(candidate.access_kind));
  node.runtime_evidence.push_back("executor_capability_id=" + node.executor_capability_id);
  node.runtime_evidence.push_back("descriptor_digest=" + node.descriptor_digest);
  for (const auto& reason : candidate.acceptance_reasons) {
    node.runtime_evidence.push_back("acceptance_reason=" + reason);
  }
  for (const auto& reason : candidate.refusal_reasons) {
    node.runtime_evidence.push_back("refusal_reason=" + reason);
  }
  for (const auto& evidence : candidate.runtime_evidence) {
    node.runtime_evidence.push_back(evidence);
  }
  if (candidate.ordered_limit_evidence.present) {
    node.runtime_evidence.push_back("ordered_limit_index_uuid=" + candidate.ordered_limit_evidence.index_uuid);
    node.runtime_evidence.push_back("ordered_limit_count=" + std::to_string(candidate.ordered_limit_evidence.limit_count));
    node.runtime_evidence.push_back(std::string("ordered_limit_index_order_satisfied=") +
                                    (candidate.ordered_limit_evidence.index_order_satisfied ? "true" : "false"));
    node.runtime_evidence.push_back(std::string("ordered_limit_sort_avoided=") +
                                    (candidate.ordered_limit_evidence.sort_avoided ? "true" : "false"));
  }
  if (candidate.summary_prune_evidence.present) {
    node.runtime_evidence.push_back("summary_prune_status=" + candidate.summary_prune_evidence.summary_status);
    node.runtime_evidence.push_back("summary_prune_reason=" + candidate.summary_prune_evidence.prune_reason);
    node.runtime_evidence.push_back("summary_prune_fallback_reason=" + candidate.summary_prune_evidence.fallback_reason);
    node.runtime_evidence.push_back(std::string("summary_mga_recheck_required=") +
                                    (candidate.summary_prune_evidence.base_row_mga_recheck_required ? "true" : "false"));
    node.runtime_evidence.push_back(std::string("summary_security_recheck_required=") +
                                    (candidate.summary_prune_evidence.base_row_security_recheck_required ? "true" : "false"));
  }
  if (candidate.partition_segment_prune_evidence.present) {
    node.runtime_evidence.push_back("partition_segment_prune_selected_access=" +
                                    candidate.partition_segment_prune_evidence.selected_access);
    node.runtime_evidence.push_back("partition_segment_prune_fallback_reason=" +
                                    candidate.partition_segment_prune_evidence.fallback_reason);
    node.runtime_evidence.push_back("partition_segment_prune_pages_pruned=" +
                                    std::to_string(candidate.partition_segment_prune_evidence.pages_pruned));
    node.runtime_evidence.push_back(std::string("partition_segment_mga_recheck_required=") +
                                    (candidate.partition_segment_prune_evidence.base_row_mga_recheck_required ? "true" : "false"));
    node.runtime_evidence.push_back(std::string("partition_segment_security_recheck_required=") +
                                    (candidate.partition_segment_prune_evidence.base_row_security_recheck_required ? "true" : "false"));
  }
  return node;
}

PhysicalPlanValidation ValidatePhysicalPlanNode(const PhysicalPlanNode& node) {
  PhysicalPlanValidation validation;
  if (node.node_id.empty()) validation.diagnostics.push_back("SB_OPT_PHYSICAL_NODE_ID_REQUIRED");
  if (node.executor_capability_id.empty()) validation.diagnostics.push_back("SB_OPT_EXECUTOR_CAPABILITY_REQUIRED");
  if (node.descriptor_digest.empty()) validation.diagnostics.push_back("SB_OPT_DESCRIPTOR_DIGEST_REQUIRED");
  if (node.executor_capability_id != RequiredExecutorCapabilityForAccessKind(node.access_kind)) {
    validation.diagnostics.push_back("SB_OPT_EXECUTOR_CAPABILITY_ACCESS_MISMATCH");
  }
  if (!node.preserves_visibility) validation.diagnostics.push_back("SB_OPT_VISIBILITY_PRESERVATION_REQUIRED");
  if (node.parser_or_reference_evidence_authority) {
    validation.diagnostics.push_back("SB_OPT_PHYSICAL_PLAN_PARSER_REFERENCE_AUTHORITY_FORBIDDEN");
  }
  if (node.memory_evidence_required &&
      (!node.memory_evidence_present || !node.memory_evidence_trusted)) {
    validation.diagnostics.push_back("SB_OPT_PHYSICAL_PLAN_MEMORY_EVIDENCE_REQUIRED");
  }
  if (node.agent_evidence_required &&
      (!node.agent_evidence_present || !node.agent_evidence_trusted)) {
    validation.diagnostics.push_back("SB_OPT_PHYSICAL_PLAN_AGENT_EVIDENCE_REQUIRED");
  }
  if (IsStorageAccessKind(node.access_kind) && !node.children.empty()) {
    validation.diagnostics.push_back("SB_OPT_STORAGE_ACCESS_NODE_MUST_BE_LEAF");
  }
  if (IsStorageAccessKind(node.access_kind) && !node.storage_backed) {
    validation.diagnostics.push_back("SB_OPT_STORAGE_BACKED_ACCESS_REQUIRED");
  }
  if (!IsStorageAccessKind(node.access_kind) && node.storage_backed) {
    validation.diagnostics.push_back("SB_OPT_NON_STORAGE_OPERATOR_MARKED_STORAGE_BACKED");
  }
  if (IsJoinAccessKind(node.access_kind) && node.children.size() != 2) {
    validation.diagnostics.push_back("SB_OPT_JOIN_REQUIRES_TWO_CHILDREN");
  }
  if (IsUnaryUpperAccessKind(node.access_kind) && node.children.size() != 1) {
    validation.diagnostics.push_back("SB_OPT_UNARY_OPERATOR_REQUIRES_ONE_CHILD");
  }
  for (const auto& child : node.children) {
    const auto child_validation = ValidatePhysicalPlanNode(child);
    validation.diagnostics.insert(validation.diagnostics.end(), child_validation.diagnostics.begin(), child_validation.diagnostics.end());
  }
  validation.ok = validation.diagnostics.empty();
  return validation;
}

std::string SerializePhysicalPlanNodeToJson(const PhysicalPlanNode& node, std::size_t indent) {
  const auto pad = Indent(indent);
  const auto child_pad = Indent(indent + 2);
  std::ostringstream out;
  out << pad << "{\n";
  out << child_pad << "\"node_id\": \"" << JsonEscape(node.node_id) << "\",\n";
  out << child_pad << "\"access_kind\": \"" << planner::PhysicalAccessKindName(node.access_kind) << "\",\n";
  out << child_pad << "\"executor_capability_id\": \"" << JsonEscape(node.executor_capability_id) << "\",\n";
  out << child_pad << "\"descriptor_digest\": \"" << JsonEscape(node.descriptor_digest) << "\",\n";
  out << child_pad << "\"estimated_rows\": " << node.estimated_rows << ",\n";
  out << child_pad << "\"total_cost\": " << node.cost.total_cost << ",\n";
  out << child_pad << "\"storage_backed\": " << (node.storage_backed ? "true" : "false") << ",\n";
  out << child_pad << "\"materializes\": " << (node.materializes ? "true" : "false") << ",\n";
  out << child_pad << "\"preserves_order\": " << (node.preserves_order ? "true" : "false") << ",\n";
  out << child_pad << "\"preserves_visibility\": " << (node.preserves_visibility ? "true" : "false") << ",\n";
  out << child_pad << "\"memory_evidence_required\": " << (node.memory_evidence_required ? "true" : "false") << ",\n";
  out << child_pad << "\"memory_evidence_present\": " << (node.memory_evidence_present ? "true" : "false") << ",\n";
  out << child_pad << "\"memory_evidence_trusted\": " << (node.memory_evidence_trusted ? "true" : "false") << ",\n";
  out << child_pad << "\"agent_evidence_required\": " << (node.agent_evidence_required ? "true" : "false") << ",\n";
  out << child_pad << "\"agent_evidence_present\": " << (node.agent_evidence_present ? "true" : "false") << ",\n";
  out << child_pad << "\"agent_evidence_trusted\": " << (node.agent_evidence_trusted ? "true" : "false") << ",\n";
  out << child_pad << "\"runtime_evidence\": [";
  for (std::size_t i = 0; i < node.runtime_evidence.size(); ++i) {
    out << "\"" << JsonEscape(node.runtime_evidence[i]) << "\"";
    if (i + 1 != node.runtime_evidence.size()) out << ", ";
  }
  out << "],\n";
  out << child_pad << "\"diagnostics\": [";
  for (std::size_t i = 0; i < node.diagnostics.size(); ++i) {
    out << "\"" << JsonEscape(node.diagnostics[i]) << "\"";
    if (i + 1 != node.diagnostics.size()) out << ", ";
  }
  out << "],\n";
  out << child_pad << "\"children\": [\n";
  for (std::size_t i = 0; i < node.children.size(); ++i) {
    out << SerializePhysicalPlanNodeToJson(node.children[i], indent + 4);
    if (i + 1 != node.children.size()) out << ",";
    out << "\n";
  }
  out << child_pad << "]\n";
  out << pad << "}";
  return out.str();
}

}  // namespace scratchbird::engine::optimizer

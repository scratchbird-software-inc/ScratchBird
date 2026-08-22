// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "query/canonical_heap_optimizer_admission.hpp"

#include "datatype_catalog_manifest.hpp"
#include "engine/executor/canonical_aggregate_registry.hpp"
#include "hash_digest.hpp"
#include "mga_relation_store/mga_relation_store.hpp"
#include "query/canonical_relational_bridge.hpp"
#include "security/security_model.hpp"
#include "transaction/transaction_api.hpp"
#include "uuid.hpp"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <functional>
#include <optional>
#include <ranges>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace scratchbird::engine::internal_api {
namespace {

namespace opt = scratchbird::engine::optimizer;
namespace plan = scratchbird::engine::planner;

bool IsCanonicalUuid(const std::string_view value) {
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
}

std::string CanonicalCoreDatatypeUuid(const std::string_view stable_name) {
  static const auto manifest =
      scratchbird::core::datatypes::LoadCurrentCoreDatatypeCatalogManifest();
  if (!manifest.ok()) return {};
  const auto found = std::ranges::find_if(
      manifest.manifest.descriptor_rows, [&](const auto& row) {
        return row.stable_name == stable_name;
      });
  return found == manifest.manifest.descriptor_rows.end()
             ? std::string{}
             : scratchbird::core::uuid::UuidToString(
                   found->descriptor_uuid.value);
}

bool ParsePositiveU64(const std::string_view text, std::uint64_t* value) {
  if (value == nullptr || text.empty()) return false;
  std::uint64_t parsed = 0;
  const auto [end, error] =
      std::from_chars(text.data(), text.data() + text.size(), parsed);
  if (error != std::errc{} || end != text.data() + text.size() ||
      parsed == 0) {
    return false;
  }
  *value = parsed;
  return true;
}

std::optional<std::string> ExactDescriptorField(
    const std::string_view descriptor,
    const std::string_view field_name) {
  const std::string prefix = std::string(field_name) + "=";
  std::optional<std::string> value;
  std::size_t offset = 0;
  while (offset <= descriptor.size()) {
    const auto next = descriptor.find(';', offset);
    const auto end = next == std::string_view::npos ? descriptor.size() : next;
    const auto field = descriptor.substr(offset, end - offset);
    if (field.rfind(prefix, 0) == 0) {
      if (value.has_value() || field.size() == prefix.size()) {
        return std::nullopt;
      }
      value = std::string(field.substr(prefix.size()));
    }
    if (next == std::string_view::npos) break;
    offset = next + 1;
  }
  return value;
}

bool ExactOptionalDescriptorFieldMatches(
    const std::string_view descriptor,
    const std::string_view field_name,
    const std::optional<std::string>& expected) {
  const std::string prefix = std::string(field_name) + "=";
  std::size_t matches = 0;
  std::string_view actual;
  std::size_t offset = 0;
  while (offset <= descriptor.size()) {
    const auto next = descriptor.find(';', offset);
    const auto end = next == std::string_view::npos ? descriptor.size() : next;
    const auto field = descriptor.substr(offset, end - offset);
    if (field.rfind(prefix, 0) == 0) {
      ++matches;
      actual = field.substr(prefix.size());
    }
    if (next == std::string_view::npos) break;
    offset = next + 1;
  }
  if (!expected.has_value()) return matches == 0;
  return matches == 1 && !actual.empty() && actual == *expected;
}

bool ExactNullabilityCarrierMatches(const std::string_view descriptor,
                                    const bool expected_nullable) {
  std::optional<bool> admitted;
  std::size_t canonical_count = 0;
  std::size_t storage_count = 0;
  std::size_t offset = 0;
  while (offset <= descriptor.size()) {
    const auto next = descriptor.find(';', offset);
    const auto end = next == std::string_view::npos ? descriptor.size() : next;
    const auto field = descriptor.substr(offset, end - offset);
    std::optional<bool> current;
    if (field.rfind("nullability=", 0) == 0) {
      ++canonical_count;
      const auto value = field.substr(std::string_view("nullability=").size());
      if (value == "nullable") {
        current = true;
      } else if (value == "non_null") {
        current = false;
      } else {
        return false;
      }
    } else if (field.rfind("nullable=", 0) == 0) {
      ++storage_count;
      const auto value = field.substr(std::string_view("nullable=").size());
      if (value == "true") {
        current = true;
      } else if (value == "false") {
        current = false;
      } else {
        return false;
      }
    }
    if (current.has_value()) {
      if (admitted.has_value() && *admitted != *current) return false;
      admitted = current;
    }
    if (next == std::string_view::npos) break;
    offset = next + 1;
  }
  return canonical_count <= 1 && storage_count <= 1 && admitted.has_value() &&
         *admitted == expected_nullable;
}

CanonicalHeapOptimizerAdmissionResult Refuse(std::string diagnostic_id,
                                             std::string field_id) {
  CanonicalHeapOptimizerAdmissionResult result;
  result.issue.diagnostic_id = std::move(diagnostic_id);
  result.issue.field_id = std::move(field_id);
  return result;
}

plan::CanonicalMgaStatementContext CanonicalMgaContextFromResolvedSnapshot(
    const EngineRequestContext& context,
    const scratchbird::transaction::mga::SnapshotVectorDescriptor& descriptor) {
  plan::CanonicalMgaStatementContext result;
  result.statement_uuid = context.statement_uuid.canonical;
  result.owning_transaction_uuid = context.transaction_uuid.canonical;
  result.statement_snapshot_uuid = context.statement_snapshot_uuid.canonical;
  result.statement_metadata_snapshot_uuid =
      context.statement_metadata_snapshot_uuid.canonical;
  result.owning_local_transaction_id = descriptor.owning_transaction.value;
  result.visible_committed_high_watermark =
      descriptor.visible_committed_high_watermark;
  result.oldest_active_transaction_id =
      descriptor.oldest_active_transaction.value;
  result.oldest_interesting_transaction_id =
      descriptor.oldest_interesting_transaction.value;
  result.oldest_snapshot_transaction_id =
      descriptor.oldest_snapshot_transaction.value;
  result.retention_horizon_transaction_id =
      descriptor.retention_horizon_transaction.value;
  result.active_excluded_local_transaction_ids =
      descriptor.active_excluded_local_transaction_ids;
  result.in_doubt_excluded_local_transaction_ids =
      descriptor.in_doubt_excluded_local_transaction_ids;
  result.snapshot_kind = scratchbird::transaction::mga::SnapshotVectorKindName(
      descriptor.snapshot_kind);
  result.publication_inventory_next_local_transaction_id =
      descriptor.publication_inventory_next_local_transaction_id;
  result.inventory_authoritative = descriptor.inventory_authoritative;
  result.complete = descriptor.complete;
  result.current = true;
  return result;
}

CanonicalHeapOptimizerAdmissionResult BuildCanonicalCrossJoinHeapAdmission(
    const CanonicalHeapOptimizerAdmissionRequest& request,
    const plan::CanonicalMgaStatementContext& canonical_mga,
    const std::uint64_t admitted_at_monotonic_ns) {
  const auto& context = request.context;
  const auto& relational = request.relational_dag;
  std::vector<const RelationalDagNode*> scans;
  std::vector<const RelationalDagNode*> joins;
  std::vector<const RelationalDagNode*> filters;
  std::vector<const RelationalDagNode*> projects;
  std::vector<const RelationalDagNode*> ctes;
  const RelationalDagNode* limit = nullptr;
  for (const auto& node : relational.nodes) {
    if (node.node_kind == RelationalDagNodeKind::kScan) {
      scans.push_back(&node);
    } else if (node.node_kind == RelationalDagNodeKind::kJoin) {
      joins.push_back(&node);
    } else if (node.node_kind == RelationalDagNodeKind::kFilter) {
      filters.push_back(&node);
    } else if (node.node_kind == RelationalDagNodeKind::kProject) {
      projects.push_back(&node);
    } else if (node.node_kind == RelationalDagNodeKind::kCte) {
      ctes.push_back(&node);
    } else if (node.node_kind == RelationalDagNodeKind::kLimit &&
               limit == nullptr) {
      limit = &node;
    } else {
      return Refuse("QOW-DIAG-QRY-004-HEAP-CROSS-JOIN-PROFILE-V1",
                    "bounded_heap_scan_join_tree_with_unary_tail");
    }
  }
  if (scans.size() < 2 || scans.size() > 9 ||
      joins.size() != scans.size() - 1 ||
      filters.size() > scans.size() + joins.size() ||
      projects.size() > scans.size() + joins.size() ||
      ctes.size() > scans.size() + joins.size() ||
      relational.nodes.size() !=
          scans.size() + joins.size() + filters.size() +
              projects.size() + ctes.size() +
              static_cast<std::size_t>(limit != nullptr) ||
      !relational.values_rows.empty() || !relational.grouping_sets.empty() ||
      !relational.properties.empty()) {
    return Refuse("QOW-DIAG-QRY-004-HEAP-CROSS-JOIN-PROFILE-V1",
                  "bounded_heap_scan_join_tree_with_unary_tail");
  }
  std::unordered_map<std::uint32_t, const RelationalDagNode*> nodes_by_id;
  for (const auto& node : relational.nodes) {
    if (node.node_id == 0 || !nodes_by_id.emplace(node.node_id, &node).second) {
      return Refuse("QOW-DIAG-QRY-004-HEAP-CROSS-JOIN-PROFILE-V1",
                    "unique_join_tree_node_identity");
    }
  }
  const auto node_for = [&](const std::uint32_t node_id) {
    const auto found = nodes_by_id.find(node_id);
    return found == nodes_by_id.end() ? nullptr : found->second;
  };
  const auto* terminal = node_for(relational.root_node_id);
  const auto* join_root = terminal;
  const auto consume_unary = [&](const RelationalDagNodeKind kind,
                                 const RelationalDagNode* expected) {
    if (join_root == nullptr || join_root->node_kind != kind ||
        join_root != expected || join_root->input_node_ids.size() != 1) {
      return false;
    }
    join_root = node_for(join_root->input_node_ids.front());
    return join_root != nullptr;
  };
  const RelationalDagNode* terminal_cte = nullptr;
  if (join_root != nullptr &&
      join_root->node_kind == RelationalDagNodeKind::kCte) {
    terminal_cte = join_root;
    if (!consume_unary(RelationalDagNodeKind::kCte, terminal_cte)) {
      return Refuse("QOW-DIAG-QRY-004-HEAP-CROSS-JOIN-PROFILE-V1",
                    "terminal_nonrecursive_cte_tail");
    }
  }
  std::vector<const RelationalDagNode*> local_ctes;
  local_ctes.reserve(ctes.size());
  for (const auto* candidate : ctes) {
    if (candidate != terminal_cte) local_ctes.push_back(candidate);
  }
  const RelationalDagNode* terminal_limit = nullptr;
  if (join_root != nullptr &&
      join_root->node_kind == RelationalDagNodeKind::kLimit) {
    terminal_limit = join_root;
    if (!consume_unary(RelationalDagNodeKind::kLimit, terminal_limit)) {
      return Refuse("QOW-DIAG-QRY-004-HEAP-CROSS-JOIN-PROFILE-V1",
                    "terminal_limit_tail");
    }
  }
  const RelationalDagNode* terminal_project = nullptr;
  if (join_root != nullptr &&
      join_root->node_kind == RelationalDagNodeKind::kProject) {
    terminal_project = join_root;
    if (!consume_unary(RelationalDagNodeKind::kProject, terminal_project)) {
      return Refuse("QOW-DIAG-QRY-004-HEAP-CROSS-JOIN-PROFILE-V1",
                    "terminal_descriptor_project_tail");
    }
  }
  std::vector<const RelationalDagNode*> local_projects;
  local_projects.reserve(projects.size());
  for (const auto* candidate : projects) {
    if (candidate != terminal_project) local_projects.push_back(candidate);
  }
  const RelationalDagNode* terminal_filter = nullptr;
  if (join_root != nullptr &&
      join_root->node_kind == RelationalDagNodeKind::kFilter) {
    terminal_filter = join_root;
    if (!consume_unary(RelationalDagNodeKind::kFilter, terminal_filter)) {
      return Refuse("QOW-DIAG-QRY-004-HEAP-CROSS-JOIN-PROFILE-V1",
                    "terminal_filter_tail");
    }
  }
  std::vector<const RelationalDagNode*> local_filters;
  local_filters.reserve(filters.size());
  for (const auto* candidate : filters) {
    if (candidate != terminal_filter) local_filters.push_back(candidate);
  }
  if (join_root == nullptr ||
      join_root->node_kind != RelationalDagNodeKind::kJoin ||
      std::ranges::find(joins, join_root) == joins.end()) {
    return Refuse("QOW-DIAG-QRY-004-HEAP-CROSS-JOIN-PROFILE-V1",
                  "join_tree_root_before_unary_tail");
  }
  const auto unary_empty = [](const RelationalDagNode& node) {
    return node.required_object_uuids.empty() && node.values_row_ids.empty() &&
           node.required_property_uuids.empty() &&
           node.delivered_property_uuids.empty();
  };
  const auto* limit_input =
      terminal_project != nullptr
          ? terminal_project
          : (terminal_filter != nullptr ? terminal_filter : join_root);
  if ((limit == nullptr) != (terminal_limit == nullptr) ||
      (terminal_limit != nullptr &&
       (!local_filters.empty() || !local_projects.empty() || !ctes.empty())) ||
      (terminal_limit != nullptr &&
       (terminal_limit->shareable ||
        terminal_limit->semantic_variant_id != "limit.bound-count.v1" ||
        terminal_limit->input_node_ids !=
            std::vector<std::uint32_t>{limit_input->node_id} ||
        terminal_limit->bound_expression_ids.size() != 1 ||
        terminal_limit->output_descriptor_ids !=
            limit_input->output_descriptor_ids ||
        !unary_empty(*terminal_limit)))) {
    return Refuse("QOW-DIAG-QRY-004-HEAP-CROSS-JOIN-PROFILE-V1",
                  "exact_terminal_limit_binding");
  }
  const auto filter_input_for = [&](const RelationalDagNode* candidate) {
    if (candidate == nullptr) {
      return static_cast<const RelationalDagNode*>(nullptr);
    }
    if (candidate == terminal_filter) return join_root;
    return candidate->input_node_ids.size() == 1
               ? node_for(candidate->input_node_ids.front())
               : nullptr;
  };
  std::unordered_set<std::uint32_t> local_filter_scan_node_ids;
  std::unordered_set<std::uint32_t> local_filter_join_node_ids;
  std::unordered_set<std::uint32_t> filter_predicate_expression_ids;
  for (const auto* filter : filters) {
    const auto* filter_input = filter_input_for(filter);
    const bool local = filter != terminal_filter;
    const bool local_scan =
        local && filter_input != nullptr &&
        filter_input->node_kind == RelationalDagNodeKind::kScan;
    const bool local_join_subtree =
        local && filter_input != nullptr && filter_input != join_root &&
        filter_input->node_kind == RelationalDagNodeKind::kJoin;
    if (filter->semantic_variant_id !=
            "filter.catalog-column-numeric-comparison.v1" ||
        filter_input == nullptr ||
        filter->input_node_ids !=
            std::vector<std::uint32_t>{filter_input->node_id} ||
        (local && (!local_scan && !local_join_subtree)) ||
        (local && filter->shareable) ||
        filter->bound_expression_ids.size() != 1 ||
        !filter_predicate_expression_ids
             .insert(filter->bound_expression_ids.front())
             .second ||
        (local_scan &&
         !local_filter_scan_node_ids.insert(filter_input->node_id).second) ||
        (local_join_subtree &&
         !local_filter_join_node_ids.insert(filter_input->node_id).second) ||
        filter->output_descriptor_ids != filter_input->output_descriptor_ids ||
        !unary_empty(*filter)) {
      return Refuse("QOW-DIAG-QRY-004-HEAP-CROSS-JOIN-PROFILE-V1",
                    "exact_filter_binding");
    }
  }
  const auto project_input_for = [&](const RelationalDagNode* candidate) {
    if (candidate == nullptr) {
      return static_cast<const RelationalDagNode*>(nullptr);
    }
    if (candidate == terminal_project) {
      return terminal_filter != nullptr ? terminal_filter : join_root;
    }
    return candidate->input_node_ids.size() == 1
               ? node_for(candidate->input_node_ids.front())
               : nullptr;
  };
  std::unordered_set<std::uint32_t> local_project_scan_node_ids;
  std::unordered_set<std::uint32_t> local_project_join_node_ids;
  for (const auto* project : projects) {
    const auto* project_input = project_input_for(project);
    const bool local = project != terminal_project;
    const RelationalDagNode* project_scan = nullptr;
    const RelationalDagNode* project_join = nullptr;
    if (local && project_input != nullptr) {
      if (project_input->node_kind == RelationalDagNodeKind::kScan) {
        project_scan = project_input;
      } else if (
          project_input->node_kind == RelationalDagNodeKind::kFilter &&
          std::ranges::find(local_filters, project_input) !=
              local_filters.end() &&
          project_input->input_node_ids.size() == 1) {
        const auto* filter_input =
            node_for(project_input->input_node_ids.front());
        if (filter_input != nullptr &&
            filter_input->node_kind == RelationalDagNodeKind::kScan) {
          project_scan = filter_input;
        } else if (
            filter_input != nullptr && filter_input != join_root &&
            filter_input->node_kind == RelationalDagNodeKind::kJoin &&
            local_filter_join_node_ids.contains(filter_input->node_id)) {
          project_join = filter_input;
        }
      } else if (project_input != join_root &&
                 project_input->node_kind ==
                     RelationalDagNodeKind::kJoin) {
        project_join = project_input;
      }
    }
    if (project_input == nullptr ||
        (local &&
         ((project_scan == nullptr) == (project_join == nullptr) ||
          project->shareable ||
          (project_scan != nullptr &&
           !local_project_scan_node_ids.insert(project_scan->node_id).second) ||
          (project_join != nullptr &&
           !local_project_join_node_ids.insert(project_join->node_id).second))) ||
        project->semantic_variant_id !=
            "project.catalog-visible-columns.v1" ||
        project->input_node_ids !=
            std::vector<std::uint32_t>{project_input->node_id} ||
        project->bound_expression_ids.empty() ||
        project->bound_expression_ids.size() !=
            project->output_descriptor_ids.size() ||
        project->output_descriptor_ids.size() >=
            project_input->output_descriptor_ids.size() ||
        std::ranges::any_of(
            project->output_descriptor_ids, [&](const auto descriptor_id) {
              return std::ranges::find(project_input->output_descriptor_ids,
                                       descriptor_id) ==
                     project_input->output_descriptor_ids.end();
            }) ||
        !unary_empty(*project)) {
      return Refuse("QOW-DIAG-QRY-004-HEAP-CROSS-JOIN-PROFILE-V1",
                    "exact_descriptor_project_binding");
    }
  }
  const auto branch_scan_for = [&](const RelationalDagNode* input) {
    if (input == nullptr) return static_cast<const RelationalDagNode*>(nullptr);
    if (input->node_kind == RelationalDagNodeKind::kScan) return input;
    if (input->node_kind == RelationalDagNodeKind::kFilter &&
        std::ranges::find(local_filters, input) != local_filters.end() &&
        input->input_node_ids.size() == 1) {
      const auto* scan = node_for(input->input_node_ids.front());
      return scan != nullptr && scan->node_kind == RelationalDagNodeKind::kScan
                 ? scan
                 : nullptr;
    }
    if (input->node_kind == RelationalDagNodeKind::kProject &&
        std::ranges::find(local_projects, input) != local_projects.end() &&
        input->input_node_ids.size() == 1) {
      const auto* project_input = node_for(input->input_node_ids.front());
      if (project_input != nullptr &&
          project_input->node_kind == RelationalDagNodeKind::kScan) {
        return project_input;
      }
      if (project_input != nullptr &&
          project_input->node_kind == RelationalDagNodeKind::kFilter &&
          std::ranges::find(local_filters, project_input) !=
              local_filters.end() &&
          project_input->input_node_ids.size() == 1) {
        const auto* scan = node_for(project_input->input_node_ids.front());
        return scan != nullptr &&
                       scan->node_kind == RelationalDagNodeKind::kScan
                   ? scan
                   : nullptr;
      }
    }
    return static_cast<const RelationalDagNode*>(nullptr);
  };
  const auto branch_join_for = [&](const RelationalDagNode* input) {
    if (input == nullptr) return static_cast<const RelationalDagNode*>(nullptr);
    if (input != join_root &&
        input->node_kind == RelationalDagNodeKind::kJoin) {
      return input;
    }
    if (input->node_kind == RelationalDagNodeKind::kFilter &&
        std::ranges::find(local_filters, input) != local_filters.end() &&
        input->input_node_ids.size() == 1) {
      const auto* filter_input = node_for(input->input_node_ids.front());
      return filter_input != nullptr && filter_input != join_root &&
                     filter_input->node_kind == RelationalDagNodeKind::kJoin &&
                     local_filter_join_node_ids.contains(filter_input->node_id)
                 ? filter_input
                 : nullptr;
    }
    if (input->node_kind == RelationalDagNodeKind::kProject &&
        std::ranges::find(local_projects, input) != local_projects.end() &&
        input->input_node_ids.size() == 1) {
      const auto* project_input = node_for(input->input_node_ids.front());
      if (project_input != nullptr && project_input != join_root &&
          project_input->node_kind == RelationalDagNodeKind::kJoin &&
          local_project_join_node_ids.contains(project_input->node_id)) {
        return project_input;
      }
      if (project_input != nullptr &&
          project_input->node_kind == RelationalDagNodeKind::kFilter &&
          std::ranges::find(local_filters, project_input) !=
              local_filters.end() &&
          project_input->input_node_ids.size() == 1) {
        const auto* filter_input =
            node_for(project_input->input_node_ids.front());
        if (filter_input != nullptr && filter_input != join_root &&
            filter_input->node_kind == RelationalDagNodeKind::kJoin &&
            local_filter_join_node_ids.contains(filter_input->node_id) &&
            local_project_join_node_ids.contains(filter_input->node_id)) {
          return filter_input;
        }
      }
    }
    return static_cast<const RelationalDagNode*>(nullptr);
  };
  const auto cte_input_for = [&](const RelationalDagNode* candidate) {
    if (candidate == nullptr) {
      return static_cast<const RelationalDagNode*>(nullptr);
    }
    if (candidate == terminal_cte) {
      return terminal_project != nullptr
                 ? terminal_project
                 : (terminal_filter != nullptr ? terminal_filter : join_root);
    }
    return candidate->input_node_ids.size() == 1
               ? node_for(candidate->input_node_ids.front())
               : nullptr;
  };
  std::unordered_set<std::uint32_t> local_cte_scan_node_ids;
  std::unordered_set<std::uint32_t> local_cte_join_node_ids;
  for (const auto* cte : ctes) {
    const auto* cte_input = cte_input_for(cte);
    const bool local = cte != terminal_cte;
    const auto* cte_scan = local ? branch_scan_for(cte_input) : nullptr;
    const auto* cte_join = local ? branch_join_for(cte_input) : nullptr;
    if (cte_input == nullptr ||
        (local &&
         ((cte_scan == nullptr) == (cte_join == nullptr) ||
          (cte_scan != nullptr &&
           !local_cte_scan_node_ids.insert(cte_scan->node_id).second) ||
          (cte_join != nullptr &&
           !local_cte_join_node_ids.insert(cte_join->node_id).second))) ||
        cte->semantic_variant_id != "cte.bound.v1" ||
        cte->input_node_ids !=
            std::vector<std::uint32_t>{cte_input->node_id} ||
        cte->output_descriptor_ids != cte_input->output_descriptor_ids ||
        !cte->bound_expression_ids.empty() || !unary_empty(*cte)) {
      return Refuse("QOW-DIAG-QRY-004-HEAP-CROSS-JOIN-PROFILE-V1",
                    "exact_nonrecursive_cte_binding");
    }
  }
  for (const auto* scan : scans) {
    if (scan->semantic_variant_id != "relation.source.v1" ||
        !scan->input_node_ids.empty() || scan->shareable ||
        scan->required_object_uuids.size() != 1 ||
        !IsCanonicalUuid(scan->required_object_uuids.front()) ||
        scan->output_descriptor_ids.empty() ||
        scan->output_descriptor_ids.size() !=
            scan->bound_expression_ids.size() ||
        !scan->values_row_ids.empty() ||
        !scan->required_property_uuids.empty() ||
        !scan->delivered_property_uuids.empty()) {
      return Refuse("QOW-DIAG-QRY-004-HEAP-CROSS-JOIN-PROFILE-V1",
                    "relation_source_leaf");
    }
  }
  std::unordered_set<std::uint32_t> visiting;
  std::unordered_set<std::uint32_t> completed;
  std::function<bool(std::uint32_t)> validate_tree =
      [&](const std::uint32_t node_id) {
        const auto found = nodes_by_id.find(node_id);
        if (found == nodes_by_id.end()) return false;
        const auto& node = *found->second;
        if (node.node_kind == RelationalDagNodeKind::kScan) {
          return completed.insert(node_id).second;
        }
        if (node.node_kind == RelationalDagNodeKind::kFilter) {
          if (std::ranges::find(local_filters, &node) == local_filters.end() ||
              node.input_node_ids.size() != 1 ||
              node.input_node_ids.front() == node.node_id ||
              completed.contains(node_id) ||
              !visiting.insert(node_id).second ||
              !validate_tree(node.input_node_ids.front())) {
            return false;
          }
          visiting.erase(node_id);
          completed.insert(node_id);
          return true;
        }
        if (node.node_kind == RelationalDagNodeKind::kProject) {
          if (std::ranges::find(local_projects, &node) ==
                  local_projects.end() ||
              node.input_node_ids.size() != 1 ||
              node.input_node_ids.front() == node.node_id ||
              completed.contains(node_id) ||
              !visiting.insert(node_id).second ||
              !validate_tree(node.input_node_ids.front())) {
            return false;
          }
          visiting.erase(node_id);
          completed.insert(node_id);
          return true;
        }
        if (node.node_kind == RelationalDagNodeKind::kCte) {
          if (std::ranges::find(local_ctes, &node) == local_ctes.end() ||
              node.input_node_ids.size() != 1 ||
              node.input_node_ids.front() == node.node_id ||
              completed.contains(node_id) ||
              !visiting.insert(node_id).second ||
              !validate_tree(node.input_node_ids.front())) {
            return false;
          }
          visiting.erase(node_id);
          completed.insert(node_id);
          return true;
        }
        const bool accepted_join =
            node.node_kind == RelationalDagNodeKind::kJoin &&
            (node.semantic_variant_id == "join.cross.v1" ||
             node.semantic_variant_id == "join.inner.v1" ||
             node.semantic_variant_id == "join.left-outer.v1" ||
             node.semantic_variant_id == "join.right-outer.v1" ||
             node.semantic_variant_id == "join.full-outer.v1" ||
             node.semantic_variant_id == "join.left-semi.v1" ||
             node.semantic_variant_id == "join.left-anti.v1");
        const bool predicate_join =
            accepted_join && node.semantic_variant_id != "join.cross.v1";
        const bool left_only_join =
            accepted_join &&
            (node.semantic_variant_id == "join.left-semi.v1" ||
             node.semantic_variant_id == "join.left-anti.v1");
        if (!accepted_join || node.input_node_ids.size() != 2 ||
            node.input_node_ids[0] == node.input_node_ids[1] ||
            node.bound_expression_ids.size() !=
                static_cast<std::size_t>(predicate_join) ||
            !node.required_object_uuids.empty() ||
            !node.values_row_ids.empty() ||
            !node.required_property_uuids.empty() ||
            !node.delivered_property_uuids.empty() ||
            completed.contains(node_id) || !visiting.insert(node_id).second ||
            !validate_tree(node.input_node_ids[0]) ||
            !validate_tree(node.input_node_ids[1])) {
          return false;
        }
        const auto* left = nodes_by_id.at(node.input_node_ids[0]);
        const auto* right = nodes_by_id.at(node.input_node_ids[1]);
        std::vector<std::uint32_t> expected = left->output_descriptor_ids;
        if (!left_only_join) {
          expected.insert(expected.end(), right->output_descriptor_ids.begin(),
                          right->output_descriptor_ids.end());
        }
        if (node.output_descriptor_ids != expected) return false;
        visiting.erase(node_id);
        completed.insert(node_id);
        return true;
      };
  if (!validate_tree(join_root->node_id) ||
      completed.size() != scans.size() + joins.size() +
                              local_filters.size() + local_projects.size() +
                              local_ctes.size()) {
    return Refuse("QOW-DIAG-QRY-004-HEAP-CROSS-JOIN-PROFILE-V1",
                  "connected_acyclic_join_output_lineage");
  }

  CanonicalRelationalPlanningScope planning_scope;
  planning_scope.catalog_epoch_uuid = context.catalog_epoch_uuid.canonical;
  planning_scope.security_context_uuid =
      context.authorization_context.authority_uuid.canonical;
  planning_scope.statement_uuid = context.statement_uuid.canonical;
  planning_scope.owning_transaction_uuid = context.transaction_uuid.canonical;
  planning_scope.statement_snapshot_uuid =
      context.statement_snapshot_uuid.canonical;
  planning_scope.statement_metadata_snapshot_uuid =
      context.statement_metadata_snapshot_uuid.canonical;
  planning_scope.local_transaction_id = context.local_transaction_id;
  planning_scope.snapshot_visible_through_local_transaction_id =
      context.snapshot_visible_through_local_transaction_id;
  planning_scope.metadata_snapshot_engine_owned =
      context.statement_metadata_snapshot_engine_owned;
  planning_scope.authorization_context_engine_owned =
      context.authorization_context.present;
  auto logical = PopulateCanonicalLogicalGraphFromAdmittedTypedRelationalDag(
      relational, planning_scope);
  if (!logical.accepted || !logical.property_catalog.properties.empty()) {
    if (!logical.issues.empty()) {
      return Refuse(logical.issues.front().diagnostic_id,
                    logical.issues.front().field_id);
    }
    return Refuse("QOW-DIAG-QRY-004-HEAP-CROSS-JOIN-PROFILE-V1",
                  "object_source_properties");
  }
  auto registered_mga = canonical_mga;
  registered_mga.current = false;
  if (!plan::CanonicalMgaStatementContextEqual(
          logical.logical_graph.mga_statement_context, registered_mga) ||
      !plan::CanonicalMgaStatementContextEqual(
          logical.property_catalog.mga_statement_context, registered_mga)) {
    return Refuse("QOW-DIAG-QRY-004-HEAP-OPTIMIZER-MGA-SNAPSHOT-V1",
                  "bridge_statement_snapshot_carriage");
  }
  logical.logical_graph.mga_statement_context = canonical_mga;
  logical.property_catalog.mga_statement_context = canonical_mga;

  std::unordered_map<std::uint32_t, const RelationalExpressionRecord*>
      expressions_by_id;
  std::unordered_map<std::uint32_t, const RelationalTypeDescriptor*>
      descriptors_by_id;
  for (const auto& expression : relational.expressions) {
    expressions_by_id.emplace(expression.expression_id, &expression);
  }
  for (const auto& descriptor : relational.descriptors) {
    descriptors_by_id.emplace(descriptor.descriptor_id, &descriptor);
  }
  if (terminal_limit != nullptr) {
    const auto expression =
        expressions_by_id.find(terminal_limit->bound_expression_ids.front());
    const auto descriptor =
        expression == expressions_by_id.end()
            ? descriptors_by_id.end()
            : descriptors_by_id.find(expression->second->result_descriptor_id);
    const bool numeric_literal =
        expression != expressions_by_id.end() &&
        expression->second->expression_kind ==
            RelationalExpressionKind::kLiteral &&
        expression->second->literal_kind == RelationalLiteralKind::kNumeric &&
        expression->second->literal_or_parameter_ref.has_value() &&
        !expression->second->parameter_typed_value_v1.has_value();
    const bool parameter_value =
        expression != expressions_by_id.end() &&
        expression->second->expression_kind ==
            RelationalExpressionKind::kParameter &&
        !expression->second->literal_kind.has_value() &&
        !expression->second->literal_or_parameter_ref.has_value() &&
        !expression->second->literal_typed_value_v1.has_value() &&
        expression->second->parameter_typed_value_v1.has_value();
    std::uint64_t parsed = 0;
    const auto* encoded =
        numeric_literal
            ? &*expression->second->literal_or_parameter_ref
            : nullptr;
    const auto converted =
        encoded == nullptr
            ? std::from_chars_result{}
            : std::from_chars(encoded->data(),
                              encoded->data() + encoded->size(), parsed);
    bool exact_parameter = parameter_value;
    if (exact_parameter) {
      const auto& typed = *expression->second->parameter_typed_value_v1;
      const auto digest = scratchbird::core::hash::ComputeSha256Digest(
          typed.canonical_value_bytes);
      exact_parameter =
          typed.descriptor_generation != 0 && typed.value_state == "value" &&
          typed.canonical_value_bytes.size() == 8 &&
          (typed.canonical_value_bytes.back() & 0x80U) == 0 && digest.ok() &&
          digest.digest == typed.canonical_value_sha256 &&
          descriptor != descriptors_by_id.end() &&
          typed.descriptor_uuid == descriptor->second->descriptor_uuid;
    }
    if (expression == expressions_by_id.end() ||
        (!numeric_literal && !exact_parameter) ||
        !expression->second->child_expression_ids.empty() ||
        expression->second->function_uuid.has_value() ||
        expression->second->bound_name_uuid.has_value() ||
        expression->second->operator_name.has_value() ||
        (numeric_literal &&
         (encoded == nullptr || encoded->empty() ||
          (encoded->size() > 1 && encoded->front() == '0') ||
          converted.ec != std::errc{} ||
          converted.ptr != encoded->data() + encoded->size() ||
          parsed > static_cast<std::uint64_t>(
                       std::numeric_limits<std::int64_t>::max()))) ||
        descriptor == descriptors_by_id.end() ||
        descriptor->second->nullability != RelationalNullability::kNonNull ||
        descriptor->second->type_uuid != CanonicalCoreDatatypeUuid("int64") ||
        descriptor->second->collation_uuid.has_value() ||
        descriptor->second->timezone_profile_id.has_value() ||
        descriptor->second->width.has_value() ||
        descriptor->second->precision.has_value() ||
        descriptor->second->scale.has_value()) {
      return Refuse("QOW-DIAG-QRY-004-HEAP-CROSS-JOIN-PROFILE-V1",
                    "exact_terminal_limit_literal_binding");
    }
  }
  if (terminal_limit != nullptr && terminal_filter != nullptr) {
    const auto predicate = terminal_filter->bound_expression_ids.size() == 1
                               ? expressions_by_id.find(
                                     terminal_filter->bound_expression_ids.front())
                               : expressions_by_id.end();
    const RelationalExpressionRecord* filter_identifier = nullptr;
    const RelationalExpressionRecord* filter_value = nullptr;
    if (predicate != expressions_by_id.end() &&
        predicate->second->child_expression_ids.size() == 2) {
      const auto identifier = expressions_by_id.find(
          predicate->second->child_expression_ids[0]);
      const auto value = expressions_by_id.find(
          predicate->second->child_expression_ids[1]);
      if (identifier != expressions_by_id.end()) {
        filter_identifier = identifier->second;
      }
      if (value != expressions_by_id.end()) filter_value = value->second;
    }
    const auto limit_value = expressions_by_id.find(
        terminal_limit->bound_expression_ids.front());
    const auto identifier_descriptor =
        filter_identifier == nullptr
            ? descriptors_by_id.end()
            : descriptors_by_id.find(filter_identifier->result_descriptor_id);
    const auto literal_descriptor =
        filter_value == nullptr
            ? descriptors_by_id.end()
            : descriptors_by_id.find(filter_value->result_descriptor_id);
    const auto predicate_descriptor =
        predicate == expressions_by_id.end()
            ? descriptors_by_id.end()
            : descriptors_by_id.find(predicate->second->result_descriptor_id);
    const auto limit_descriptor =
        limit_value == expressions_by_id.end()
            ? descriptors_by_id.end()
            : descriptors_by_id.find(limit_value->second->result_descriptor_id);
    const bool accepted_operator =
        predicate != expressions_by_id.end() &&
        predicate->second->operator_name.has_value() &&
        (*predicate->second->operator_name == "=" ||
         *predicate->second->operator_name == "<>" ||
         *predicate->second->operator_name == "!=" ||
         *predicate->second->operator_name == "<" ||
         *predicate->second->operator_name == "<=" ||
         *predicate->second->operator_name == ">" ||
         *predicate->second->operator_name == ">=");
    const bool literal_filter_value =
        filter_value != nullptr &&
        filter_value->expression_kind == RelationalExpressionKind::kLiteral &&
        filter_value->literal_kind == RelationalLiteralKind::kNumeric &&
        filter_value->literal_typed_value_v1.has_value() &&
        !filter_value->parameter_typed_value_v1.has_value();
    const bool parameter_filter_value =
        filter_value != nullptr &&
        filter_value->expression_kind ==
            RelationalExpressionKind::kParameter &&
        !filter_value->literal_kind.has_value() &&
        !filter_value->literal_typed_value_v1.has_value() &&
        filter_value->parameter_typed_value_v1.has_value();
    const bool literal_limit_value =
        limit_value != expressions_by_id.end() &&
        limit_value->second->expression_kind ==
            RelationalExpressionKind::kLiteral;
    const bool parameter_limit_value =
        limit_value != expressions_by_id.end() &&
        limit_value->second->expression_kind ==
            RelationalExpressionKind::kParameter;
    const bool exact_operand_pair =
        (literal_filter_value && parameter_limit_value) ||
        (parameter_filter_value && literal_limit_value) ||
        (parameter_filter_value && parameter_limit_value) ||
        (literal_filter_value && literal_limit_value);
    bool exact_filter_value = literal_filter_value || parameter_filter_value;
    if (exact_filter_value) {
      const auto& typed_bytes =
          literal_filter_value
              ? filter_value->literal_typed_value_v1->canonical_value_bytes
              : filter_value->parameter_typed_value_v1->canonical_value_bytes;
      const auto& typed_descriptor_uuid =
          literal_filter_value
              ? filter_value->literal_typed_value_v1->descriptor_uuid
              : filter_value->parameter_typed_value_v1->descriptor_uuid;
      const auto typed_descriptor_generation =
          literal_filter_value
              ? filter_value->literal_typed_value_v1->descriptor_generation
              : filter_value->parameter_typed_value_v1->descriptor_generation;
      const auto& typed_value_state =
          literal_filter_value
              ? filter_value->literal_typed_value_v1->value_state
              : filter_value->parameter_typed_value_v1->value_state;
      const auto& typed_sha =
          literal_filter_value
              ? filter_value->literal_typed_value_v1->canonical_value_sha256
              : filter_value->parameter_typed_value_v1->canonical_value_sha256;
      const auto digest = scratchbird::core::hash::ComputeSha256Digest(
          typed_bytes);
      exact_filter_value =
          typed_descriptor_generation != 0 && typed_value_state == "value" &&
          typed_bytes.size() == 8 && (typed_bytes.back() & 0x80U) == 0 &&
          digest.ok() && digest.digest == typed_sha &&
          literal_descriptor != descriptors_by_id.end() &&
          typed_descriptor_uuid == literal_descriptor->second->descriptor_uuid;
    }
    const auto identifier_source_count =
        filter_identifier == nullptr
            ? 0
            : std::ranges::count_if(
                  relational.outputs, [&](const auto& output) {
                    return output.relation_node_id == join_root->node_id &&
                           output.visible &&
                           output.expression_id ==
                               filter_identifier->expression_id &&
                           output.descriptor_id ==
                               filter_identifier->result_descriptor_id;
                  });
    const bool distinct_expression_ids =
        predicate != expressions_by_id.end() &&
        filter_identifier != nullptr && filter_value != nullptr &&
        limit_value != expressions_by_id.end() &&
        predicate->second->expression_id != filter_identifier->expression_id &&
        predicate->second->expression_id != filter_value->expression_id &&
        predicate->second->expression_id != limit_value->second->expression_id &&
        filter_identifier->expression_id != filter_value->expression_id &&
        filter_identifier->expression_id != limit_value->second->expression_id &&
        filter_value->expression_id != limit_value->second->expression_id;
    const bool distinct_descriptor_ids =
        identifier_descriptor != descriptors_by_id.end() &&
        literal_descriptor != descriptors_by_id.end() &&
        predicate_descriptor != descriptors_by_id.end() &&
        limit_descriptor != descriptors_by_id.end() &&
        identifier_descriptor->first != literal_descriptor->first &&
        identifier_descriptor->first != predicate_descriptor->first &&
        identifier_descriptor->first != limit_descriptor->first &&
        literal_descriptor->first != predicate_descriptor->first &&
        literal_descriptor->first != limit_descriptor->first &&
        predicate_descriptor->first != limit_descriptor->first;
    if (!accepted_operator || filter_identifier == nullptr ||
        filter_identifier->expression_kind !=
            RelationalExpressionKind::kIdentifier ||
        !filter_identifier->child_expression_ids.empty() ||
        filter_identifier->function_uuid.has_value() ||
        !filter_identifier->bound_name_uuid.has_value() ||
        filter_identifier->literal_kind.has_value() ||
        filter_identifier->operator_name.has_value() ||
        filter_identifier->literal_or_parameter_ref.has_value() ||
        filter_identifier->literal_typed_value_v1.has_value() ||
        filter_identifier->parameter_typed_value_v1.has_value() ||
        predicate == expressions_by_id.end() ||
        predicate->second->expression_kind !=
            RelationalExpressionKind::kBinary ||
        predicate->second->function_uuid.has_value() ||
        predicate->second->bound_name_uuid.has_value() ||
        predicate->second->literal_kind.has_value() ||
        predicate->second->literal_or_parameter_ref.has_value() ||
        predicate->second->literal_typed_value_v1.has_value() ||
        predicate->second->parameter_typed_value_v1.has_value() ||
        filter_value == nullptr || !exact_operand_pair ||
        !filter_value->child_expression_ids.empty() ||
        filter_value->function_uuid.has_value() ||
        filter_value->bound_name_uuid.has_value() ||
        filter_value->operator_name.has_value() ||
        filter_value->literal_or_parameter_ref.has_value() ||
        !exact_filter_value || identifier_source_count != 1 ||
        identifier_descriptor == descriptors_by_id.end() ||
        literal_descriptor == descriptors_by_id.end() ||
        predicate_descriptor == descriptors_by_id.end() ||
        identifier_descriptor->second->type_uuid !=
            CanonicalCoreDatatypeUuid("int64") ||
        literal_descriptor->second->type_uuid !=
            CanonicalCoreDatatypeUuid("int64") ||
        identifier_descriptor->second->type_uuid !=
            literal_descriptor->second->type_uuid ||
        literal_descriptor->second->nullability !=
            RelationalNullability::kNonNull ||
        predicate_descriptor->second->nullability !=
            RelationalNullability::kNullable ||
        predicate_descriptor->second->type_uuid !=
            CanonicalCoreDatatypeUuid("boolean") ||
        identifier_descriptor->second->collation_uuid.has_value() ||
        literal_descriptor->second->collation_uuid.has_value() ||
        predicate_descriptor->second->collation_uuid.has_value() ||
        identifier_descriptor->second->timezone_profile_id.has_value() ||
        literal_descriptor->second->timezone_profile_id.has_value() ||
        predicate_descriptor->second->timezone_profile_id.has_value() ||
        identifier_descriptor->second->width.has_value() ||
        identifier_descriptor->second->precision.has_value() ||
        identifier_descriptor->second->scale.has_value() ||
        literal_descriptor->second->width.has_value() ||
        literal_descriptor->second->precision.has_value() ||
        literal_descriptor->second->scale.has_value() ||
        predicate_descriptor->second->width.has_value() ||
        predicate_descriptor->second->precision.has_value() ||
        predicate_descriptor->second->scale.has_value() ||
        limit_value == expressions_by_id.end() ||
        !distinct_expression_ids || !distinct_descriptor_ids) {
      return Refuse("QOW-DIAG-QRY-004-HEAP-CROSS-JOIN-PROFILE-V1",
                    "exact_terminal_filter_limit_operand_profile");
    }
  }
  const auto lineage_output_node_for = [&](const RelationalDagNode* candidate) {
    for (std::size_t depth = 0;
         candidate != nullptr &&
             candidate->node_kind == RelationalDagNodeKind::kCte &&
             depth <= ctes.size();
         ++depth) {
      candidate = candidate->input_node_ids.size() == 1
                      ? node_for(candidate->input_node_ids.front())
                      : nullptr;
    }
    return candidate != nullptr &&
                   candidate->node_kind != RelationalDagNodeKind::kCte
               ? candidate
               : nullptr;
  };
  const auto exact_join_output_lineage = [&](const RelationalDagNode& join) {
    if (join.input_node_ids.size() != 2) return false;
    const auto* left = node_for(join.input_node_ids[0]);
    const auto* right = node_for(join.input_node_ids[1]);
    const auto* left_output_node = lineage_output_node_for(left);
    const auto* right_output_node = lineage_output_node_for(right);
    if (left == nullptr || right == nullptr || left_output_node == nullptr ||
        right_output_node == nullptr ||
        left_output_node->output_descriptor_ids != left->output_descriptor_ids ||
        right_output_node->output_descriptor_ids !=
            right->output_descriptor_ids) {
      return false;
    }
    std::vector<const RelationalOutputRecord*> left_outputs;
    std::vector<const RelationalOutputRecord*> right_outputs;
    std::vector<const RelationalOutputRecord*> join_outputs;
    for (const auto& output : relational.outputs) {
      if (output.relation_node_id == left_output_node->node_id) {
        left_outputs.push_back(&output);
      }
      if (output.relation_node_id == right_output_node->node_id) {
        right_outputs.push_back(&output);
      }
      if (output.relation_node_id == join.node_id) {
        join_outputs.push_back(&output);
      }
    }
    std::ranges::sort(left_outputs, {}, &RelationalOutputRecord::ordinal);
    std::ranges::sort(right_outputs, {}, &RelationalOutputRecord::ordinal);
    std::ranges::sort(join_outputs, {}, &RelationalOutputRecord::ordinal);
    if (left_outputs.size() != left->output_descriptor_ids.size() ||
        right_outputs.size() != right->output_descriptor_ids.size()) {
      return false;
    }
    for (std::size_t ordinal = 0; ordinal < left_outputs.size(); ++ordinal) {
      if (!left_outputs[ordinal]->visible ||
          left_outputs[ordinal]->ordinal != ordinal ||
          left_outputs[ordinal]->descriptor_id !=
              left->output_descriptor_ids[ordinal]) {
        return false;
      }
    }
    for (std::size_t ordinal = 0; ordinal < right_outputs.size(); ++ordinal) {
      if (!right_outputs[ordinal]->visible ||
          right_outputs[ordinal]->ordinal != ordinal ||
          right_outputs[ordinal]->descriptor_id !=
              right->output_descriptor_ids[ordinal]) {
        return false;
      }
    }
    const bool left_only =
        join.semantic_variant_id == "join.left-semi.v1" ||
        join.semantic_variant_id == "join.left-anti.v1";
    std::vector<const RelationalOutputRecord*> expected_outputs = left_outputs;
    if (!left_only) {
      expected_outputs.insert(expected_outputs.end(), right_outputs.begin(),
                              right_outputs.end());
    }
    if (join_outputs.size() != expected_outputs.size() ||
        join_outputs.size() != join.output_descriptor_ids.size()) {
      return false;
    }
    std::unordered_set<std::uint32_t> join_output_ids;
    for (std::size_t ordinal = 0; ordinal < join_outputs.size(); ++ordinal) {
      const auto& output = *join_outputs[ordinal];
      const auto& expected = *expected_outputs[ordinal];
      if (!output.visible || output.ordinal != ordinal ||
          output.output_id == 0 ||
          !join_output_ids.insert(output.output_id).second ||
          output.descriptor_id != join.output_descriptor_ids[ordinal] ||
          output.descriptor_id != expected.descriptor_id ||
          output.expression_id != expected.expression_id ||
          output.output_name_utf8 != expected.output_name_utf8) {
        return false;
      }
    }
    return true;
  };
  if (std::ranges::any_of(joins, [&](const auto* join) {
        return !exact_join_output_lineage(*join);
      })) {
    return Refuse("QOW-DIAG-QRY-004-HEAP-CROSS-JOIN-PROFILE-V1",
                  "exact_join_output_record_lineage");
  }
  for (const auto* join : joins) {
    if (join->semantic_variant_id == "join.cross.v1") continue;
    const auto* left = node_for(join->input_node_ids[0]);
    const auto* right = node_for(join->input_node_ids[1]);
    if (left == nullptr || right == nullptr) {
      return Refuse("QOW-DIAG-QRY-004-HEAP-CROSS-JOIN-PROFILE-V1",
                    "join_predicate_immediate_input_lineage");
    }
    std::vector<std::uint32_t> input_descriptor_ids =
        left->output_descriptor_ids;
    input_descriptor_ids.insert(input_descriptor_ids.end(),
                                right->output_descriptor_ids.begin(),
                                right->output_descriptor_ids.end());
    std::unordered_set<std::uint32_t> active_expression_ids;
    std::unordered_set<std::uint32_t> completed_expression_ids;
    const auto validate_join_expression_source =
        [&](auto&& self, const std::uint32_t expression_id,
            const std::size_t depth) -> bool {
      if (expression_id == 0 || depth > 64) return false;
      if (completed_expression_ids.contains(expression_id)) return true;
      if (!active_expression_ids.insert(expression_id).second) return false;
      const auto found = expressions_by_id.find(expression_id);
      if (found == expressions_by_id.end()) return false;
      const auto& expression = *found->second;
      if (expression.expression_kind ==
          RelationalExpressionKind::kIdentifier) {
        if (!expression.child_expression_ids.empty() ||
            std::ranges::count(input_descriptor_ids,
                               expression.result_descriptor_id) != 1) {
          return false;
        }
      } else {
        for (const auto child_id : expression.child_expression_ids) {
          if (!self(self, child_id, depth + 1)) return false;
        }
      }
      active_expression_ids.erase(expression_id);
      completed_expression_ids.insert(expression_id);
      return true;
    };
    if (!validate_join_expression_source(
            validate_join_expression_source,
            join->bound_expression_ids.front(), 1)) {
      return Refuse("QOW-DIAG-QRY-004-HEAP-CROSS-JOIN-PROFILE-V1",
                    "join_predicate_immediate_input_lineage");
    }
  }
  for (const auto* filter : filters) {
    const auto* filter_input = filter_input_for(filter);
    std::unordered_set<std::uint32_t> active_filter_expression_ids;
    std::unordered_set<std::uint32_t> completed_filter_expression_ids;
    const auto validate_filter_expression_source =
        [&](auto&& self, const std::uint32_t expression_id,
            const std::size_t depth) -> bool {
      if (expression_id == 0 || depth > 64) return false;
      if (completed_filter_expression_ids.contains(expression_id)) return true;
      if (!active_filter_expression_ids.insert(expression_id).second) {
        return false;
      }
      const auto found = expressions_by_id.find(expression_id);
      if (found == expressions_by_id.end()) return false;
      const auto& expression = *found->second;
      if (expression.expression_kind ==
          RelationalExpressionKind::kIdentifier) {
        if (!expression.child_expression_ids.empty() ||
            std::ranges::count(filter_input->output_descriptor_ids,
                               expression.result_descriptor_id) != 1) {
          return false;
        }
      } else {
        for (const auto child_id : expression.child_expression_ids) {
          if (!self(self, child_id, depth + 1)) return false;
        }
      }
      active_filter_expression_ids.erase(expression_id);
      completed_filter_expression_ids.insert(expression_id);
      return true;
    };
    if (!validate_filter_expression_source(
            validate_filter_expression_source,
            filter->bound_expression_ids.front(), 1)) {
      return Refuse("QOW-DIAG-QRY-004-HEAP-CROSS-JOIN-PROFILE-V1",
                    "filter_predicate_immediate_input_lineage");
    }
  }
  for (const auto* filter : filters) {
    const auto* filter_input = filter_input_for(filter);
    std::vector<const RelationalOutputRecord*> filter_outputs;
    std::vector<const RelationalOutputRecord*> input_outputs;
    for (const auto& output : relational.outputs) {
      if (output.relation_node_id == filter->node_id) {
        filter_outputs.push_back(&output);
      }
      if (output.relation_node_id == filter_input->node_id) {
        input_outputs.push_back(&output);
      }
    }
    std::ranges::sort(filter_outputs, {}, &RelationalOutputRecord::ordinal);
    std::ranges::sort(input_outputs, {}, &RelationalOutputRecord::ordinal);
    bool exact_filter_outputs =
        filter_outputs.size() == filter->output_descriptor_ids.size() &&
        input_outputs.size() == filter_input->output_descriptor_ids.size() &&
        filter_outputs.size() == input_outputs.size();
    std::unordered_set<std::uint32_t> filter_output_ids;
    for (std::size_t ordinal = 0;
         exact_filter_outputs && ordinal < filter_outputs.size(); ++ordinal) {
      exact_filter_outputs =
          filter_outputs[ordinal]->visible &&
          filter_outputs[ordinal]->ordinal == ordinal &&
          filter_outputs[ordinal]->output_id != 0 &&
          filter_output_ids.insert(filter_outputs[ordinal]->output_id).second &&
          filter_outputs[ordinal]->descriptor_id ==
              filter->output_descriptor_ids[ordinal] &&
          input_outputs[ordinal]->ordinal == ordinal &&
          input_outputs[ordinal]->descriptor_id ==
              filter_input->output_descriptor_ids[ordinal] &&
          filter_outputs[ordinal]->descriptor_id ==
              input_outputs[ordinal]->descriptor_id &&
          filter_outputs[ordinal]->expression_id ==
              input_outputs[ordinal]->expression_id &&
          filter_outputs[ordinal]->output_name_utf8 ==
              input_outputs[ordinal]->output_name_utf8;
    }
    if (!exact_filter_outputs) {
      return Refuse("QOW-DIAG-QRY-004-HEAP-CROSS-JOIN-PROFILE-V1",
                    "exact_filter_output_lineage");
    }
  }
  for (const auto* cte : ctes) {
    if (std::ranges::any_of(relational.outputs, [&](const auto& output) {
          return output.relation_node_id == cte->node_id;
        })) {
      return Refuse("QOW-DIAG-QRY-004-HEAP-CROSS-JOIN-PROFILE-V1",
                    "outputless_nonrecursive_cte");
    }
  }
  for (const auto* project : projects) {
    const auto* project_input = project_input_for(project);
    std::vector<const RelationalOutputRecord*> project_outputs;
    std::vector<const RelationalOutputRecord*> lineage_outputs;
    for (const auto& output : relational.outputs) {
      if (output.relation_node_id == project->node_id) {
        project_outputs.push_back(&output);
      }
      if (output.relation_node_id == project_input->node_id) {
        lineage_outputs.push_back(&output);
      }
    }
    std::ranges::sort(project_outputs, {}, &RelationalOutputRecord::ordinal);
    std::ranges::sort(lineage_outputs, {}, &RelationalOutputRecord::ordinal);
    bool exact_project_outputs =
        project_outputs.size() == project->output_descriptor_ids.size() &&
        lineage_outputs.size() == project_input->output_descriptor_ids.size();
    for (std::size_t ordinal = 0;
         exact_project_outputs && ordinal < lineage_outputs.size(); ++ordinal) {
      exact_project_outputs =
          lineage_outputs[ordinal]->ordinal == ordinal &&
          lineage_outputs[ordinal]->descriptor_id ==
              project_input->output_descriptor_ids[ordinal];
    }
    std::unordered_set<std::size_t> projected_source_ordinals;
    for (std::size_t ordinal = 0;
         exact_project_outputs && ordinal < project_outputs.size(); ++ordinal) {
      const auto& output = *project_outputs[ordinal];
      const auto expression =
          expressions_by_id.find(project->bound_expression_ids[ordinal]);
      const auto source = std::ranges::find_if(
          lineage_outputs, [&](const auto* candidate) {
            return candidate->expression_id ==
                       project->bound_expression_ids[ordinal] &&
                   candidate->descriptor_id == output.descriptor_id;
          });
      if (expression == expressions_by_id.end() ||
          source == lineage_outputs.end() || !output.visible ||
          output.ordinal != ordinal || output.output_id == 0 ||
          output.expression_id != project->bound_expression_ids[ordinal] ||
          output.descriptor_id != project->output_descriptor_ids[ordinal] ||
          output.output_name_utf8 != (*source)->output_name_utf8 ||
          expression->second->result_descriptor_id != output.descriptor_id) {
        exact_project_outputs = false;
        break;
      }
      const auto source_ordinal = static_cast<std::size_t>(
          std::distance(lineage_outputs.begin(), source));
      if (!projected_source_ordinals.insert(source_ordinal).second) {
        exact_project_outputs = false;
      }
    }
    if (!exact_project_outputs) {
      return Refuse("QOW-DIAG-QRY-004-HEAP-CROSS-JOIN-PROFILE-V1",
                    "exact_descriptor_project_output_lineage");
    }
  }
  if (terminal_limit != nullptr) {
    std::vector<const RelationalOutputRecord*> limit_outputs;
    std::vector<const RelationalOutputRecord*> input_outputs;
    for (const auto& output : relational.outputs) {
      if (output.relation_node_id == terminal_limit->node_id) {
        limit_outputs.push_back(&output);
      }
      if (output.relation_node_id == limit_input->node_id) {
        input_outputs.push_back(&output);
      }
    }
    std::ranges::sort(limit_outputs, {}, &RelationalOutputRecord::ordinal);
    std::ranges::sort(input_outputs, {}, &RelationalOutputRecord::ordinal);
    bool exact_limit_outputs =
        limit_outputs.size() == terminal_limit->output_descriptor_ids.size() &&
        input_outputs.size() == limit_input->output_descriptor_ids.size() &&
        limit_outputs.size() == input_outputs.size();
    std::unordered_set<std::uint32_t> limit_output_ids;
    for (std::size_t ordinal = 0;
         exact_limit_outputs && ordinal < limit_outputs.size(); ++ordinal) {
      exact_limit_outputs =
          limit_outputs[ordinal]->visible &&
          limit_outputs[ordinal]->ordinal == ordinal &&
          limit_outputs[ordinal]->output_id != 0 &&
          limit_output_ids.insert(limit_outputs[ordinal]->output_id).second &&
          input_outputs[ordinal]->visible &&
          input_outputs[ordinal]->ordinal == ordinal &&
          limit_outputs[ordinal]->descriptor_id ==
              terminal_limit->output_descriptor_ids[ordinal] &&
          limit_outputs[ordinal]->descriptor_id ==
              input_outputs[ordinal]->descriptor_id &&
          limit_outputs[ordinal]->expression_id ==
              input_outputs[ordinal]->expression_id &&
          limit_outputs[ordinal]->output_name_utf8 ==
              input_outputs[ordinal]->output_name_utf8;
    }
    if (!exact_limit_outputs) {
      return Refuse("QOW-DIAG-QRY-004-HEAP-CROSS-JOIN-PROFILE-V1",
                    "exact_terminal_limit_output_lineage");
    }
  }

  std::vector<std::string> relation_uuids;
  std::vector<std::string> projection_type_names;
  std::vector<EngineDescriptor> projection_descriptors;
  for (const auto* scan : scans) {
    const auto& relation_uuid = scan->required_object_uuids.front();
    relation_uuids.push_back(relation_uuid);
    const auto temporary = CheckMgaTemporaryTableVisibility(context,
                                                              relation_uuid);
    if (!temporary.ok || !temporary.table_visible || temporary.known_temporary ||
        temporary.table.temporary) {
      return Refuse("QOW-DIAG-QRY-004-HEAP-OPTIMIZER-RELATION-V1",
                    temporary.ok ? "current_non_temporary_relation"
                                 : temporary.diagnostic.detail);
    }
    const auto loaded = LoadMgaRelationStorageDescriptor(context,
                                                          relation_uuid);
    if (!loaded.ok) {
      return Refuse("QOW-DIAG-QRY-004-HEAP-OPTIMIZER-DESCRIPTOR-V1",
                    loaded.diagnostic.detail);
    }
    const auto& persisted = loaded.descriptor;
    const auto width = scan->output_descriptor_ids.size();
    if (persisted.relation_uuid.canonical != relation_uuid ||
        persisted.database_uuid.canonical != context.database_uuid.canonical ||
        persisted.relation_kind != "table" ||
        persisted.storage_profile != "local_mga_rowstore_v1" ||
        !IsCanonicalUuid(persisted.descriptor_uuid.canonical) ||
        persisted.descriptor_generation == 0 ||
        (persisted.descriptor_status != "production_descriptor" &&
         persisted.descriptor_status != "metadata_bridge_vetted_descriptor") ||
        persisted.columns.size() < width) {
      return Refuse("QOW-DIAG-QRY-004-HEAP-OPTIMIZER-DESCRIPTOR-V1",
                    "current_persisted_local_heap_descriptor");
    }
    std::vector<const RelationalOutputRecord*> outputs;
    for (const auto& output : relational.outputs) {
      if (output.relation_node_id == scan->node_id) outputs.push_back(&output);
    }
    std::ranges::sort(outputs, {}, &RelationalOutputRecord::ordinal);
    if (outputs.size() != width) {
      return Refuse("QOW-DIAG-QRY-004-HEAP-OPTIMIZER-BINDING-V1",
                    "complete_visible_scan_width");
    }
    std::unordered_set<std::uint32_t> output_ids;
    std::unordered_set<std::string> column_uuids;
    for (std::size_t ordinal = 0; ordinal < width; ++ordinal) {
      const auto& output = *outputs[ordinal];
      const auto expression_it =
          expressions_by_id.find(scan->bound_expression_ids[ordinal]);
      const auto descriptor_it = descriptors_by_id.find(output.descriptor_id);
      if (expression_it == expressions_by_id.end() ||
          descriptor_it == descriptors_by_id.end() ||
          !expression_it->second->bound_name_uuid.has_value()) {
        return Refuse("QOW-DIAG-QRY-004-HEAP-OPTIMIZER-BINDING-V1",
                      "scan_expression_descriptor_resolution");
      }
      const auto& expression = *expression_it->second;
      const auto& descriptor = *descriptor_it->second;
      const auto persisted_column = std::ranges::find_if(
          persisted.columns, [&](const auto& candidate) {
            return candidate.column_uuid.canonical ==
                   *expression.bound_name_uuid;
          });
      if (persisted_column == persisted.columns.end()) {
        return Refuse("QOW-DIAG-QRY-004-HEAP-OPTIMIZER-BINDING-V1",
                      "scan_projected_column_resolution");
      }
      const auto& column = *persisted_column;
      const auto persisted_type_uuid = ExactDescriptorField(
          column.value_descriptor.encoded_descriptor, "type_uuid");
      const bool nullable =
          descriptor.nullability == RelationalNullability::kNullable;
      if (!output.visible || output.ordinal != ordinal || output.output_id == 0 ||
          !output_ids.insert(output.output_id).second ||
          output.descriptor_id != scan->output_descriptor_ids[ordinal] ||
          expression.expression_id != output.expression_id ||
          expression.expression_id != scan->bound_expression_ids[ordinal] ||
          expression.expression_kind !=
              RelationalExpressionKind::kIdentifier ||
          !expression.child_expression_ids.empty() ||
          expression.result_descriptor_id != output.descriptor_id ||
          expression.function_uuid.has_value() ||
          expression.literal_kind.has_value() ||
          expression.operator_name.has_value() ||
          expression.literal_or_parameter_ref.has_value() ||
          descriptor.descriptor_id != output.descriptor_id ||
          !IsCanonicalUuid(descriptor.descriptor_uuid) ||
          !IsCanonicalUuid(descriptor.type_uuid) ||
          descriptor.nullability == RelationalNullability::kUnknown ||
          !IsCanonicalUuid(column.column_uuid.canonical) ||
          !column_uuids.insert(column.column_uuid.canonical).second ||
          column.column_uuid.canonical != *expression.bound_name_uuid ||
          output.output_name_utf8 != column.canonical_name_key ||
          column.value_descriptor.descriptor_uuid.canonical !=
              descriptor.descriptor_uuid ||
          !persisted_type_uuid.has_value() ||
          *persisted_type_uuid != descriptor.type_uuid ||
          column.nullable != nullable ||
          !ExactNullabilityCarrierMatches(
              column.value_descriptor.encoded_descriptor, nullable) ||
          (descriptor.collation_uuid.has_value()
               ? column.collation_uuid != *descriptor.collation_uuid
               : !column.collation_uuid.empty()) ||
          !ExactOptionalDescriptorFieldMatches(
              column.value_descriptor.encoded_descriptor, "collation_uuid",
              descriptor.collation_uuid) ||
          !ExactOptionalDescriptorFieldMatches(
              column.value_descriptor.encoded_descriptor,
              "timezone_profile_id", descriptor.timezone_profile_id)) {
        return Refuse("QOW-DIAG-QRY-004-HEAP-OPTIMIZER-BINDING-V1",
                      "persisted_ordinal_descriptor_binding");
      }
      projection_type_names.push_back(
          column.value_descriptor.canonical_type_name);
      auto projection_descriptor = column.value_descriptor;
      projection_descriptor.descriptor_kind = "scalar";
      projection_descriptors.push_back(std::move(projection_descriptor));
    }
    const auto authorization = EvaluateMaterializedAuthorization(
        context, context.authorization_context, "SELECT", relation_uuid);
    if (!authorization.authorized || authorization.denied ||
        authorization.policy_recheck_required ||
        !authorization.diagnostics.empty()) {
      return Refuse("QOW-DIAG-QRY-004-HEAP-OPTIMIZER-SECURITY-V1",
                    authorization.diagnostics.empty()
                        ? "select_authorization"
                        : authorization.diagnostics.front().detail);
    }
  }

  std::ranges::sort(relation_uuids);
  relation_uuids.erase(
      std::unique(relation_uuids.begin(), relation_uuids.end()),
      relation_uuids.end());

  opt::CanonicalNativeObjectAdmissionContext admission_context;
  admission_context.statement_uuid = context.statement_uuid.canonical;
  admission_context.catalog_snapshot_uuid =
      context.statement_metadata_snapshot_uuid.canonical;
  admission_context.security_context_uuid =
      context.authorization_context.authority_uuid.canonical;
  admission_context.catalog_generation = context.catalog_generation_id;
  admission_context.authorization_catalog_generation =
      context.authorization_context.catalog_generation_id;
  admission_context.security_epoch = context.authorization_context.security_epoch;
  admission_context.policy_epoch = context.authorization_context.policy_epoch;
  admission_context.resource_epoch = context.resource_epoch;
  admission_context.capability_snapshot_uuid =
      context.optimizer_capability_snapshot_uuid.canonical;
  admission_context.resource_snapshot_uuid =
      context.optimizer_resource_snapshot_uuid.canonical;
  admission_context.route_snapshot_uuid =
      context.optimizer_route_snapshot_uuid.canonical;
  admission_context.route_epoch = context.optimizer_route_epoch;
  admission_context.route_generation = context.optimizer_route_generation;
  admission_context.memory_budget_bytes = context.optimizer_memory_budget_bytes;
  admission_context.maximum_candidate_count =
      context.optimizer_maximum_candidate_count;
  admission_context.maximum_memo_groups = context.optimizer_maximum_memo_groups;
  admission_context.maximum_search_steps =
      context.optimizer_maximum_search_steps;
  admission_context.maximum_planning_time_ns =
      context.optimizer_maximum_planning_time_ns;
  admission_context.spill_allowed = context.optimizer_spill_allowed;
  admission_context.local_transaction_id = context.local_transaction_id;
  admission_context.statement_snapshot_id =
      context.snapshot_visible_through_local_transaction_id;
  admission_context.mga_statement_context = canonical_mga;
  admission_context.admitted_at_monotonic_ns = admitted_at_monotonic_ns;
  admission_context.metadata_snapshot_engine_owned = true;
  admission_context.authorization_context_engine_owned = true;
  admission_context.catalog_object_uuids = relation_uuids;
  admission_context.authorized_object_uuids = relation_uuids;
  admission_context.catalog_object_evidence_engine_owned = true;
  admission_context.authorization_object_evidence_engine_owned = true;
  auto built = opt::BuildCanonicalObjectAwareNativeOptimizerAdmissionRequest(
      logical.logical_graph, logical.property_catalog, admission_context);
  if (!built.built || !built.admission.admitted ||
      !built.admission.planning_allowed ||
      !built.admission.degraded_for_unknown_statistics ||
      built.admission.benchmark_clean_ready ||
      built.admission.data_access_allowed ||
      built.admission.evidence.size() != 8 ||
      built.request.statistics.node_estimates.size() !=
          relational.nodes.size()) {
    return Refuse(
        built.diagnostic_id.empty()
            ? "QOW-DIAG-QRY-004-HEAP-OPTIMIZER-ADMISSION-V1"
            : built.diagnostic_id,
        built.field_id.empty() ? "heap_cross_join_unknown_statistics_admission"
                               : built.field_id);
  }
  CanonicalHeapOptimizerAdmissionResult result;
  result.built = true;
  result.request = std::move(built.request);
  result.admission = std::move(built.admission);
  result.current_relation_projection_type_names =
      std::move(projection_type_names);
  result.current_relation_projection_descriptors =
      std::move(projection_descriptors);
  return result;
}

}  // namespace

CanonicalHeapOptimizerAdmissionResult
BuildCanonicalCurrentHeapOptimizerAdmission(
    const CanonicalHeapOptimizerAdmissionRequest& request) {
  const auto& context = request.context;
  const auto& relational = request.relational_dag;

  std::uint64_t admitted_at_monotonic_ns = 0;
  if (context.database_path.empty() ||
      !IsCanonicalUuid(context.database_uuid.canonical) ||
      !IsCanonicalUuid(context.statement_uuid.canonical) ||
      !IsCanonicalUuid(context.transaction_uuid.canonical) ||
      !IsCanonicalUuid(context.statement_snapshot_uuid.canonical) ||
      !IsCanonicalUuid(context.catalog_epoch_uuid.canonical) ||
      context.local_transaction_id == 0 ||
      !context.statement_metadata_snapshot_engine_owned ||
      !IsCanonicalUuid(context.statement_metadata_snapshot_uuid.canonical) ||
      !context.security_context_present ||
      !context.authorization_context.present ||
      !IsCanonicalUuid(context.authorization_context.authority_uuid.canonical) ||
      context.authorization_context.principal_uuid.canonical !=
          context.principal_uuid.canonical ||
      context.catalog_generation_id == 0 || context.security_epoch == 0 ||
      context.resource_epoch == 0 ||
      context.authorization_context.catalog_generation_id !=
          context.catalog_generation_id ||
      context.authorization_context.security_epoch != context.security_epoch ||
      context.authorization_context.policy_epoch == 0 ||
      !IsCanonicalUuid(context.optimizer_capability_snapshot_uuid.canonical) ||
      !IsCanonicalUuid(context.optimizer_resource_snapshot_uuid.canonical) ||
      !IsCanonicalUuid(context.optimizer_route_snapshot_uuid.canonical) ||
      context.optimizer_route_epoch == 0 ||
      context.optimizer_route_generation == 0 ||
      context.optimizer_memory_budget_bytes == 0 ||
      context.optimizer_maximum_candidate_count == 0 ||
      context.optimizer_maximum_memo_groups == 0 ||
      context.optimizer_maximum_search_steps == 0 ||
      context.optimizer_maximum_planning_time_ns == 0 ||
      !ParsePositiveU64(context.current_monotonic_ns,
                        &admitted_at_monotonic_ns)) {
    return Refuse("QOW-DIAG-QRY-004-HEAP-OPTIMIZER-CONTEXT-V1",
                  "engine_planning_context");
  }
  if (!context.prepared_metadata_required_object_uuid.canonical.empty() ||
      context.prepared_metadata_required_executable_generation != 0 ||
      context.prepared_metadata_required_metadata_epoch != 0) {
    return Refuse("QOW-DIAG-QRY-004-HEAP-OPTIMIZER-SCOPE-V1",
                  "prepared_or_cached_execution");
  }

  const auto inventory_guard =
      AcquireTransactionInventoryGuard(context.database_path);
  EngineResolveStatementSnapshotRequest snapshot_request;
  snapshot_request.context = context;
  const auto snapshot = EngineResolveStatementSnapshot(snapshot_request);
  if (!snapshot.ok) {
    return Refuse(
        snapshot.diagnostics.empty()
            ? "QOW-DIAG-QRY-004-HEAP-OPTIMIZER-MGA-SNAPSHOT-V1"
            : snapshot.diagnostics.front().code,
        snapshot.diagnostics.empty()
            ? "current_statement_snapshot"
            : snapshot.diagnostics.front().detail);
  }
  const auto canonical_mga =
      CanonicalMgaContextFromResolvedSnapshot(context,
                                              snapshot.snapshot_vector);

  const auto validation = ValidateTypedRelationalDag(relational);
  if (!validation.accepted) {
    const auto& issue = validation.issues.front();
    return Refuse(issue.diagnostic_id,
                  issue.field_id + ":node_id=" +
                      std::to_string(issue.node_id));
  }
  if (relational.wire_version != 2 || relational.nodes.empty() ||
      relational.nodes.size() > 68) {
    return Refuse("QOW-DIAG-QRY-004-HEAP-OPTIMIZER-PROFILE-V1",
                  "bounded_heap_scan_composition");
  }
  if (std::ranges::count_if(relational.nodes, [](const auto& node) {
        return node.node_kind == RelationalDagNodeKind::kScan;
      }) >= 2) {
    return BuildCanonicalCrossJoinHeapAdmission(
        request, canonical_mga, admitted_at_monotonic_ns);
  }
  const RelationalDagNode* scan_node = nullptr;
  const RelationalDagNode* filter_node = nullptr;
  const RelationalDagNode* project_node = nullptr;
  const RelationalDagNode* sort_node = nullptr;
  const RelationalDagNode* window_node = nullptr;
  const RelationalDagNode* aggregate_node = nullptr;
  const RelationalDagNode* cte_node = nullptr;
  const RelationalDagNode* limit_node = nullptr;
  for (const auto& candidate : relational.nodes) {
    if (candidate.node_kind == RelationalDagNodeKind::kScan) {
      if (scan_node != nullptr) {
        return Refuse("QOW-DIAG-QRY-004-HEAP-OPTIMIZER-PROFILE-V1",
                      "one_heap_scan_leaf");
      }
      scan_node = &candidate;
    } else if (candidate.node_kind == RelationalDagNodeKind::kFilter) {
      if (filter_node != nullptr) {
        return Refuse("QOW-DIAG-QRY-004-HEAP-OPTIMIZER-PROFILE-V1",
                      "one_optional_heap_filter");
      }
      filter_node = &candidate;
    } else if (candidate.node_kind == RelationalDagNodeKind::kProject) {
      if (project_node != nullptr) {
        return Refuse("QOW-DIAG-QRY-004-HEAP-OPTIMIZER-PROFILE-V1",
                      "one_optional_heap_project");
      }
      project_node = &candidate;
    } else if (candidate.node_kind == RelationalDagNodeKind::kSort) {
      if (sort_node != nullptr) {
        return Refuse("QOW-DIAG-QRY-004-HEAP-OPTIMIZER-PROFILE-V1",
                      "one_optional_heap_sort");
      }
      sort_node = &candidate;
    } else if (candidate.node_kind == RelationalDagNodeKind::kWindow) {
      if (window_node != nullptr) {
        return Refuse("QOW-DIAG-QRY-004-HEAP-OPTIMIZER-PROFILE-V1",
                      "one_optional_heap_window");
      }
      window_node = &candidate;
    } else if (candidate.node_kind == RelationalDagNodeKind::kAggregate) {
      if (aggregate_node != nullptr) {
        return Refuse("QOW-DIAG-QRY-004-HEAP-OPTIMIZER-PROFILE-V1",
                      "one_optional_heap_aggregate");
      }
      aggregate_node = &candidate;
    } else if (candidate.node_kind == RelationalDagNodeKind::kCte) {
      if (cte_node != nullptr) {
        return Refuse("QOW-DIAG-QRY-004-HEAP-OPTIMIZER-PROFILE-V1",
                      "one_optional_nonrecursive_cte");
      }
      cte_node = &candidate;
    } else if (candidate.node_kind == RelationalDagNodeKind::kLimit) {
      if (limit_node != nullptr) {
        return Refuse("QOW-DIAG-QRY-004-HEAP-OPTIMIZER-PROFILE-V1",
                      "one_optional_limit_root");
      }
      limit_node = &candidate;
    } else {
      return Refuse("QOW-DIAG-QRY-004-HEAP-OPTIMIZER-PROFILE-V1",
                    "heap_scan_filter_project_limit_node_kinds");
    }
  }
  const auto* terminal_node =
      limit_node != nullptr ? limit_node
                            : (aggregate_node != nullptr
                                   ? aggregate_node
                                   : (project_node != nullptr
                                   ? project_node
                                   : (window_node != nullptr
                                          ? window_node
                                          : (sort_node != nullptr
                                                 ? sort_node
                                                 : (filter_node != nullptr
                                                        ? filter_node
                                                        : scan_node)))));
  const auto direct_or_cte_input =
      [&](const RelationalDagNode* consumer,
          const std::uint32_t producer_node_id) {
        if (consumer == nullptr) return false;
        if (consumer->input_node_ids ==
            std::vector<std::uint32_t>{producer_node_id}) {
          return true;
        }
        return cte_node != nullptr &&
               consumer->input_node_ids ==
                   std::vector<std::uint32_t>{cte_node->node_id} &&
               cte_node->input_node_ids ==
                   std::vector<std::uint32_t>{producer_node_id};
      };
  const auto expected_project_input =
      window_node != nullptr
          ? window_node->node_id
          : (sort_node != nullptr
                 ? sort_node->node_id
                 : (filter_node != nullptr
                        ? filter_node->node_id
                        : (scan_node == nullptr ? 0 : scan_node->node_id)));
  // FILTER and SORT are schema-preserving and intentionally own no output
  // records in this profile.  Project lineage therefore resolves at the
  // Window when present, otherwise at the Scan.
  const auto* project_input_node =
      window_node != nullptr ? window_node : scan_node;
  const auto* early_window_aggregate_row =
      window_node != nullptr &&
              window_node->semantic_variant_id ==
                  "window.aggregate-bridge.v1" &&
              relational.window_invocations.size() == 1
          ? executor::LookupCanonicalAggregateByUuidV1(
                relational.window_invocations.front().function_uuid)
          : nullptr;
  const bool exact_count_star_window_shape =
      early_window_aggregate_row != nullptr &&
      early_window_aggregate_row->function ==
          executor::CanonicalAggregateFunction::count &&
      early_window_aggregate_row->builtin_id ==
          relational.window_invocations.front().builtin_id &&
      early_window_aggregate_row->abi_version ==
          relational.window_invocations.front().function_abi_version &&
      relational.window_invocations.front().argument_expression_ids.empty();
  bool exact_project_outputs = project_node == nullptr;
  if (project_node != nullptr && project_input_node != nullptr &&
      project_node->bound_expression_ids.size() ==
          project_node->output_descriptor_ids.size()) {
    std::vector<const RelationalOutputRecord*> project_outputs;
    std::vector<const RelationalOutputRecord*> project_input_outputs;
    for (const auto& output : relational.outputs) {
      if (output.relation_node_id == project_node->node_id) {
        project_outputs.push_back(&output);
      }
      if (output.relation_node_id == project_input_node->node_id) {
        project_input_outputs.push_back(&output);
      }
    }
    std::ranges::sort(project_outputs, {},
                      &RelationalOutputRecord::ordinal);
    std::ranges::sort(project_input_outputs, {},
                      &RelationalOutputRecord::ordinal);
    exact_project_outputs =
        project_outputs.size() == project_node->output_descriptor_ids.size() &&
        project_input_outputs.size() ==
            project_input_node->output_descriptor_ids.size();
    for (std::size_t ordinal = 0;
         exact_project_outputs && ordinal < project_input_outputs.size();
         ++ordinal) {
      exact_project_outputs =
          project_input_outputs[ordinal]->ordinal == ordinal &&
          project_input_outputs[ordinal]->descriptor_id ==
              project_input_node->output_descriptor_ids[ordinal];
    }
    for (std::size_t ordinal = 0;
         exact_project_outputs && ordinal < project_outputs.size();
         ++ordinal) {
      const auto expression = std::ranges::find_if(
          relational.expressions, [&](const auto& candidate) {
            return candidate.expression_id ==
                   project_node->bound_expression_ids[ordinal];
          });
      const auto source_descriptor = std::ranges::find(
          project_input_node->output_descriptor_ids,
          project_node->output_descriptor_ids[ordinal]);
      const auto source_ordinal = static_cast<std::size_t>(std::distance(
          project_input_node->output_descriptor_ids.begin(),
          source_descriptor));
      exact_project_outputs =
          expression != relational.expressions.end() &&
          source_descriptor !=
              project_input_node->output_descriptor_ids.end() &&
          project_input_outputs[source_ordinal]->expression_id ==
              project_node->bound_expression_ids[ordinal] &&
          project_outputs[ordinal]->ordinal == ordinal &&
          project_outputs[ordinal]->visible &&
          project_outputs[ordinal]->expression_id ==
              project_node->bound_expression_ids[ordinal] &&
          project_outputs[ordinal]->descriptor_id ==
              project_node->output_descriptor_ids[ordinal] &&
          expression->result_descriptor_id ==
              project_node->output_descriptor_ids[ordinal];
    }
  }
  const auto expected_limit_input =
      aggregate_node != nullptr
          ? aggregate_node->node_id
          : (project_node != nullptr
          ? project_node->node_id
          : (sort_node != nullptr
                 ? sort_node->node_id
                 : (filter_node != nullptr
                        ? filter_node->node_id
                        : (scan_node == nullptr ? 0 : scan_node->node_id))));
  const auto cte_input_node =
      cte_node == nullptr || cte_node->input_node_ids.size() != 1
          ? relational.nodes.end()
          : std::ranges::find_if(relational.nodes, [&](const auto& candidate) {
              return candidate.node_id == cte_node->input_node_ids.front();
            });
  const bool cte_is_root =
      cte_node != nullptr && terminal_node != nullptr &&
      relational.root_node_id == cte_node->node_id &&
      cte_node->input_node_ids ==
          std::vector<std::uint32_t>{terminal_node->node_id};
  const auto cte_consumer_count =
      cte_node == nullptr
          ? std::size_t{0}
          : static_cast<std::size_t>(std::ranges::count_if(
                relational.nodes, [&](const auto& candidate) {
                  return candidate.node_id != cte_node->node_id &&
                         candidate.input_node_ids ==
                             std::vector<std::uint32_t>{cte_node->node_id};
                }));
  if (scan_node == nullptr ||
      terminal_node == nullptr ||
      (relational.root_node_id != terminal_node->node_id && !cte_is_root) ||
      relational.nodes.size() !=
          1 + static_cast<std::size_t>(filter_node != nullptr) +
              static_cast<std::size_t>(project_node != nullptr) +
              static_cast<std::size_t>(sort_node != nullptr) +
              static_cast<std::size_t>(window_node != nullptr) +
              static_cast<std::size_t>(aggregate_node != nullptr) +
              static_cast<std::size_t>(cte_node != nullptr) +
              static_cast<std::size_t>(limit_node != nullptr) ||
      (cte_node != nullptr &&
       (cte_input_node == relational.nodes.end() ||
        cte_node->semantic_variant_id != "cte.bound.v1" ||
        cte_node->output_descriptor_ids !=
            cte_input_node->output_descriptor_ids ||
        !cte_node->bound_expression_ids.empty() ||
        !cte_node->required_object_uuids.empty() ||
        !cte_node->values_row_ids.empty() ||
        !cte_node->required_property_uuids.empty() ||
        !cte_node->delivered_property_uuids.empty() ||
        (cte_is_root ? cte_consumer_count != 0
                     : cte_consumer_count != 1))) ||
      (aggregate_node != nullptr &&
       (project_node != nullptr || sort_node != nullptr)) ||
      (window_node != nullptr &&
       (sort_node == nullptr || aggregate_node != nullptr ||
        limit_node != nullptr)) ||
      (filter_node != nullptr &&
       (!direct_or_cte_input(filter_node, scan_node->node_id) ||
        filter_node->semantic_variant_id !=
            "filter.catalog-column-numeric-comparison.v1" ||
        filter_node->bound_expression_ids.size() != 1 ||
        filter_node->output_descriptor_ids !=
            scan_node->output_descriptor_ids ||
        !filter_node->required_object_uuids.empty() ||
        !filter_node->values_row_ids.empty() ||
        !filter_node->required_property_uuids.empty() ||
        !filter_node->delivered_property_uuids.empty())) ||
      (project_node != nullptr &&
       (!direct_or_cte_input(project_node, expected_project_input) ||
        !exact_project_outputs ||
        project_node->semantic_variant_id !=
            "project.catalog-visible-columns.v1" ||
        project_node->bound_expression_ids.empty() ||
        project_node->bound_expression_ids.size() !=
            project_node->output_descriptor_ids.size() ||
        project_node->output_descriptor_ids.empty() ||
        project_node->output_descriptor_ids.size() >=
            project_input_node->output_descriptor_ids.size() ||
        std::ranges::any_of(
            project_node->output_descriptor_ids,
            [&](const auto descriptor_id) {
              return std::ranges::find(
                         project_input_node->output_descriptor_ids,
                         descriptor_id) ==
                     project_input_node->output_descriptor_ids.end();
            }) ||
        !project_node->required_object_uuids.empty() ||
        !project_node->values_row_ids.empty() ||
        !project_node->required_property_uuids.empty() ||
        !project_node->delivered_property_uuids.empty())) ||
      (sort_node != nullptr &&
       (!direct_or_cte_input(
            sort_node, filter_node != nullptr ? filter_node->node_id
                                               : scan_node->node_id) ||
        sort_node->semantic_variant_id != "sort.required-order.v1" ||
        sort_node->bound_expression_ids.empty() ||
        sort_node->output_descriptor_ids != scan_node->output_descriptor_ids ||
        !sort_node->required_object_uuids.empty() ||
        !sort_node->values_row_ids.empty() ||
        sort_node->required_property_uuids.size() != 1 ||
        sort_node->delivered_property_uuids !=
            sort_node->required_property_uuids)) ||
      (window_node != nullptr &&
       (window_node->input_node_ids !=
            std::vector<std::uint32_t>{sort_node->node_id} ||
        (window_node->semantic_variant_id != "window.row-number.v1" &&
         window_node->semantic_variant_id != "window.rank.v1" &&
         window_node->semantic_variant_id != "window.dense-rank.v1" &&
         window_node->semantic_variant_id != "window.percent-rank.v1" &&
         window_node->semantic_variant_id != "window.cume-dist.v1" &&
         window_node->semantic_variant_id != "window.ntile.v1" &&
         window_node->semantic_variant_id != "window.lag.v1" &&
         window_node->semantic_variant_id != "window.lead.v1" &&
         window_node->semantic_variant_id != "window.first-value.v1" &&
         window_node->semantic_variant_id != "window.last-value.v1" &&
         window_node->semantic_variant_id != "window.nth-value.v1" &&
         window_node->semantic_variant_id != "window.aggregate-bridge.v1") ||
        window_node->bound_expression_ids.size() !=
            ((window_node->semantic_variant_id == "window.ntile.v1" ||
              window_node->semantic_variant_id == "window.lag.v1" ||
              window_node->semantic_variant_id == "window.lead.v1" ||
              window_node->semantic_variant_id == "window.first-value.v1" ||
              window_node->semantic_variant_id == "window.last-value.v1" ||
              window_node->semantic_variant_id == "window.nth-value.v1" ||
              window_node->semantic_variant_id ==
                  "window.aggregate-bridge.v1")
                 ? (window_node->semantic_variant_id == "window.nth-value.v1"
                        ? 4U
                        : (exact_count_star_window_shape ? 2U : 3U))
                 : 2U) ||
        window_node->output_descriptor_ids.size() !=
            sort_node->output_descriptor_ids.size() + 1 ||
        !std::equal(sort_node->output_descriptor_ids.begin(),
                    sort_node->output_descriptor_ids.end(),
                    window_node->output_descriptor_ids.begin()) ||
        !window_node->required_object_uuids.empty() ||
        !window_node->values_row_ids.empty() ||
        window_node->required_property_uuids !=
            sort_node->delivered_property_uuids ||
        window_node->delivered_property_uuids.size() != 2 ||
        std::ranges::find(window_node->delivered_property_uuids,
                          sort_node->delivered_property_uuids.front()) ==
            window_node->delivered_property_uuids.end())) ||
      (aggregate_node != nullptr &&
       (!direct_or_cte_input(
            aggregate_node, filter_node != nullptr ? filter_node->node_id
                                                    : scan_node->node_id) ||
        (aggregate_node->semantic_variant_id !=
             "aggregate.global-count-star.v1" &&
         aggregate_node->semantic_variant_id !=
             "aggregate.global-count-expression.v1" &&
         aggregate_node->semantic_variant_id !=
             "aggregate.global-sum-expression.v1" &&
         aggregate_node->semantic_variant_id !=
             "aggregate.global-avg-expression.v1" &&
         aggregate_node->semantic_variant_id !=
             "aggregate.global-min-expression.v1" &&
         aggregate_node->semantic_variant_id !=
             "aggregate.global-max-expression.v1" &&
         aggregate_node->semantic_variant_id !=
             "aggregate.global-bool-and-expression.v1" &&
         aggregate_node->semantic_variant_id !=
             "aggregate.global-bool-or-expression.v1" &&
         aggregate_node->semantic_variant_id !=
             "aggregate.global-every-expression.v1" &&
         aggregate_node->semantic_variant_id !=
             "aggregate.global-corr-expression.v1" &&
         aggregate_node->semantic_variant_id !=
             "aggregate.global-covar-pop-expression.v1" &&
         aggregate_node->semantic_variant_id !=
             "aggregate.global-covar-samp-expression.v1" &&
         aggregate_node->semantic_variant_id !=
             "aggregate.global-regr-count-expression.v1" &&
         aggregate_node->semantic_variant_id !=
             "aggregate.global-regr-avgx-expression.v1" &&
         aggregate_node->semantic_variant_id !=
             "aggregate.global-regr-avgy-expression.v1" &&
         aggregate_node->semantic_variant_id !=
             "aggregate.global-regr-intercept-expression.v1" &&
         aggregate_node->semantic_variant_id !=
             "aggregate.global-regr-r2-expression.v1" &&
         aggregate_node->semantic_variant_id !=
             "aggregate.global-regr-slope-expression.v1" &&
         aggregate_node->semantic_variant_id !=
             "aggregate.global-regr-sxx-expression.v1" &&
         aggregate_node->semantic_variant_id !=
             "aggregate.global-regr-sxy-expression.v1" &&
         aggregate_node->semantic_variant_id !=
             "aggregate.global-regr-syy-expression.v1" &&
         aggregate_node->semantic_variant_id !=
             "aggregate.global-stddev-pop-expression.v1" &&
         aggregate_node->semantic_variant_id !=
             "aggregate.global-variance-pop-expression.v1" &&
         aggregate_node->semantic_variant_id !=
             "aggregate.global-stddev-expression.v1" &&
         aggregate_node->semantic_variant_id !=
             "aggregate.global-variance-expression.v1" &&
         aggregate_node->semantic_variant_id !=
             "aggregate.global-stddev-samp-expression.v1" &&
         aggregate_node->semantic_variant_id !=
             "aggregate.global-variance-samp-expression.v1" &&
         aggregate_node->semantic_variant_id !=
             "aggregate.global-approx-count-distinct-expression.v1" &&
         aggregate_node->semantic_variant_id !=
             "aggregate.global-approx-median-expression.v1" &&
         aggregate_node->semantic_variant_id !=
             "aggregate.global-string-agg-expression.v1" &&
         aggregate_node->semantic_variant_id !=
             "aggregate.global-listagg-ordered-expression.v1" &&
         aggregate_node->semantic_variant_id !=
             "aggregate.global-mode-ordered-expression.v1" &&
         aggregate_node->semantic_variant_id !=
             "aggregate.global-percentile-cont-ordered-expression.v1" &&
         aggregate_node->semantic_variant_id !=
             "aggregate.global-percentile-disc-ordered-expression.v1" &&
         aggregate_node->semantic_variant_id !=
             "aggregate.global-rank-hypothetical-expression.v1" &&
         aggregate_node->semantic_variant_id !=
             "aggregate.global-dense-rank-hypothetical-expression.v1" &&
         aggregate_node->semantic_variant_id !=
             "aggregate.global-percent-rank-hypothetical-expression.v1" &&
         aggregate_node->semantic_variant_id !=
             "aggregate.global-cume-dist-hypothetical-expression.v1" &&
         aggregate_node->semantic_variant_id !=
             "aggregate.global-approx-percentile-cont-ordered-expression.v1" &&
         aggregate_node->semantic_variant_id !=
             "aggregate.global-approx-percentile-disc-ordered-expression.v1" &&
         aggregate_node->semantic_variant_id !=
             "aggregate.global-array-agg-ordered-expression.v1" &&
         aggregate_node->semantic_variant_id !=
             "aggregate.global-json-agg-ordered-expression.v1" &&
         aggregate_node->semantic_variant_id !=
             "aggregate.global-json-object-agg-ordered-expression.v1" &&
         aggregate_node->semantic_variant_id !=
             "aggregate.global-approx-top-k-expression.v1") ||
        aggregate_node->bound_expression_ids.size() != 1 ||
        aggregate_node->output_descriptor_ids.size() != 1 ||
        !aggregate_node->required_object_uuids.empty() ||
        !aggregate_node->values_row_ids.empty() ||
        !aggregate_node->required_property_uuids.empty() ||
        !aggregate_node->delivered_property_uuids.empty())) ||
      (limit_node != nullptr &&
       (!direct_or_cte_input(limit_node, expected_limit_input) ||
        (limit_node->semantic_variant_id != "limit.bound-count.v1" &&
         limit_node->semantic_variant_id !=
             "limit.bound-count-offset.v1") ||
        limit_node->bound_expression_ids.size() !=
            (limit_node->semantic_variant_id == "limit.bound-count.v1"
                 ? 1
                 : 2) ||
        limit_node->output_descriptor_ids !=
            (aggregate_node != nullptr
                 ? aggregate_node->output_descriptor_ids
                 : (project_node != nullptr
                 ? project_node->output_descriptor_ids
                 : scan_node->output_descriptor_ids)) ||
        !limit_node->required_object_uuids.empty() ||
        !limit_node->values_row_ids.empty() ||
        !limit_node->required_property_uuids.empty() ||
        !limit_node->delivered_property_uuids.empty()))) {
    return Refuse("QOW-DIAG-QRY-004-HEAP-OPTIMIZER-PROFILE-V1",
                  "heap_scan_optional_filter_project_limit_shape");
  }
  if (sort_node != nullptr) {
    const auto property = std::ranges::find_if(
        relational.properties, [&](const auto& candidate) {
          return candidate.property_uuid ==
                 sort_node->required_property_uuids.front();
        });
    std::unordered_set<std::uint32_t> ordering_expression_ids;
    const bool exact_terms =
        property != relational.properties.end() &&
        std::ranges::all_of(
            property->ordering_terms, [&](const auto& term) {
              return term.expression_id != 0 &&
                     ordering_expression_ids.insert(term.expression_id).second &&
                     std::ranges::find(sort_node->bound_expression_ids,
                                       term.expression_id) !=
                         sort_node->bound_expression_ids.end() &&
                     std::ranges::find(scan_node->bound_expression_ids,
                                       term.expression_id) !=
                         scan_node->bound_expression_ids.end();
            });
    if (property == relational.properties.end() ||
        property->property_kind != RelationalPropertyKind::kOrdering ||
        property->origin_node_id != sort_node->node_id ||
        property->ordering_terms.empty() || !exact_terms ||
        property->ordering_terms.size() !=
            sort_node->bound_expression_ids.size() ||
        !property->expression_ids.empty() ||
        !property->dependency_property_uuids.empty() ||
        !property->window_frame_descriptor_uuid.empty()) {
      return Refuse("QOW-DIAG-QRY-004-HEAP-OPTIMIZER-PROFILE-V1",
                    "heap_sort_ordering_property");
    }
  }
  if (window_node != nullptr) {
    constexpr std::string_view kRowNumberFunctionUuid =
        "019de5fc-2400-7539-bcce-00eef3ae7220";
    constexpr std::string_view kRankFunctionUuid =
        "019de5fc-2400-7b94-870d-0dd789ca70ab";
    constexpr std::string_view kDenseRankFunctionUuid =
        "019de5fc-2400-741d-bef0-f079fd3ba494";
    constexpr std::string_view kPercentRankFunctionUuid =
        "019de5fc-2400-7d86-86fe-96f3f27b5dd6";
    constexpr std::string_view kCumeDistFunctionUuid =
        "019de5fc-2400-721c-be64-2568b64a02b9";
    constexpr std::string_view kNtileFunctionUuid =
        "019de5fc-2400-7047-9474-232ca488c094";
    constexpr std::string_view kLagFunctionUuid =
        "019de5fc-2400-782c-8436-9ac310301738";
    constexpr std::string_view kLeadFunctionUuid =
        "019de5fc-2400-7a06-bc3c-6747cf5be66f";
    constexpr std::string_view kFirstValueFunctionUuid =
        "019de5fc-2400-7264-90fb-d25bd0f806f2";
    constexpr std::string_view kLastValueFunctionUuid =
        "019de5fc-2400-7d23-a5be-7ed3f1a5c3ec";
    constexpr std::string_view kNthValueFunctionUuid =
        "019de5fc-2400-7dc9-80e6-9f2ccf08076f";
    const bool rank_window =
        window_node->semantic_variant_id == "window.rank.v1";
    const bool dense_rank_window =
        window_node->semantic_variant_id == "window.dense-rank.v1";
    const bool percent_rank_window =
        window_node->semantic_variant_id == "window.percent-rank.v1";
    const bool cume_dist_window =
        window_node->semantic_variant_id == "window.cume-dist.v1";
    const bool ntile_window =
        window_node->semantic_variant_id == "window.ntile.v1";
    const bool lag_window =
        window_node->semantic_variant_id == "window.lag.v1";
    const bool lead_window =
        window_node->semantic_variant_id == "window.lead.v1";
    const bool first_value_window =
        window_node->semantic_variant_id == "window.first-value.v1";
    const bool last_value_window =
        window_node->semantic_variant_id == "window.last-value.v1";
    const bool nth_value_window =
        window_node->semantic_variant_id == "window.nth-value.v1";
    const bool aggregate_window =
        window_node->semantic_variant_id == "window.aggregate-bridge.v1";
    const auto* aggregate_window_row =
        aggregate_window && relational.window_invocations.size() == 1
            ? executor::LookupCanonicalAggregateByUuidV1(
                  relational.window_invocations.front().function_uuid)
            : nullptr;
    const bool exact_unary_aggregate =
        aggregate_window_row != nullptr &&
        (aggregate_window_row->function ==
             executor::CanonicalAggregateFunction::sum ||
         aggregate_window_row->function ==
             executor::CanonicalAggregateFunction::min ||
         aggregate_window_row->function ==
             executor::CanonicalAggregateFunction::max ||
         aggregate_window_row->function ==
             executor::CanonicalAggregateFunction::count ||
         aggregate_window_row->function ==
             executor::CanonicalAggregateFunction::bool_and ||
         aggregate_window_row->function ==
             executor::CanonicalAggregateFunction::bool_or ||
         aggregate_window_row->function ==
             executor::CanonicalAggregateFunction::every);
    const bool aggregate_count_window =
        aggregate_window_row != nullptr &&
        aggregate_window_row->function ==
            executor::CanonicalAggregateFunction::count;
    const bool aggregate_count_star_window =
        aggregate_count_window && relational.window_invocations.size() == 1 &&
        relational.window_invocations.front().argument_expression_ids.empty();
    const bool aggregate_boolean_window =
        aggregate_window_row != nullptr &&
        (aggregate_window_row->function ==
             executor::CanonicalAggregateFunction::bool_and ||
         aggregate_window_row->function ==
             executor::CanonicalAggregateFunction::bool_or ||
         aggregate_window_row->function ==
             executor::CanonicalAggregateFunction::every);
    const bool aggregate_bounded_signed_window =
        aggregate_window && !aggregate_count_window &&
        !aggregate_boolean_window;
    const bool navigation_window = lag_window || lead_window;
    const bool navigation_value_window =
        navigation_window || first_value_window || last_value_window ||
        nth_value_window;
    const bool value_window =
        navigation_window || first_value_window || last_value_window ||
        nth_value_window || aggregate_window;
    const bool value_operand_window =
        value_window && !aggregate_count_star_window;
    const auto canonical_int64_type_uuid =
        value_window ? CanonicalCoreDatatypeUuid("int64") : std::string{};
    const auto canonical_boolean_type_uuid =
        (aggregate_boolean_window || navigation_value_window)
            ? CanonicalCoreDatatypeUuid("boolean")
            : std::string{};
    const std::array<std::string, 4> canonical_bounded_signed_type_uuids = {
        aggregate_window ? CanonicalCoreDatatypeUuid("int8") : std::string{},
        aggregate_window ? CanonicalCoreDatatypeUuid("int16") : std::string{},
        aggregate_window ? CanonicalCoreDatatypeUuid("int32") : std::string{},
        aggregate_window ? canonical_int64_type_uuid : std::string{}};
    const std::string_view expected_builtin_id =
        aggregate_window
            ? (aggregate_window_row == nullptr
                   ? std::string_view{}
                   : std::string_view{aggregate_window_row->builtin_id})
            : value_window
            ? (first_value_window
                   ? "sb.window.first_value"
                   : (last_value_window
                          ? "sb.window.last_value"
                          : (nth_value_window
                                 ? "sb.window.nth_value"
                                 : (lag_window ? "sb.window.lag"
                                               : "sb.window.lead"))))
            : (ntile_window
            ? "sb.window.ntile"
            : (cume_dist_window
            ? "sb.window.cume_dist"
            : (percent_rank_window
                   ? "sb.window.percent_rank"
                   : (dense_rank_window
                          ? "sb.window.dense_rank"
                          : (rank_window ? "sb.window.rank"
                                         : "sb.window.row_number")))));
    const std::string_view expected_function_uuid =
        aggregate_window
            ? (aggregate_window_row == nullptr
                   ? std::string_view{}
                   : std::string_view{aggregate_window_row->function_uuid})
            : value_window
            ? (first_value_window
                   ? kFirstValueFunctionUuid
                   : (last_value_window
                          ? kLastValueFunctionUuid
                          : (nth_value_window
                                 ? kNthValueFunctionUuid
                                 : (lag_window ? kLagFunctionUuid
                                               : kLeadFunctionUuid))))
            : (ntile_window
            ? kNtileFunctionUuid
            : (cume_dist_window
            ? kCumeDistFunctionUuid
            : (percent_rank_window
                   ? kPercentRankFunctionUuid
                   : (dense_rank_window
                          ? kDenseRankFunctionUuid
                          : (rank_window ? kRankFunctionUuid
                                         : kRowNumberFunctionUuid)))));
    const auto ordering_property_uuid =
        sort_node->delivered_property_uuids.front();
    const auto window_property_uuid = std::ranges::find_if(
        window_node->delivered_property_uuids,
        [&](const auto& property_uuid) {
          return property_uuid != ordering_property_uuid;
        });
    const auto window_property = std::ranges::find_if(
        relational.properties, [&](const auto& property) {
          return window_property_uuid !=
                     window_node->delivered_property_uuids.end() &&
                 property.property_uuid == *window_property_uuid;
        });
    const auto function = relational.window_invocations.size() == 1
                              ? std::ranges::find_if(
                                    relational.expressions,
                                    [&](const auto& expression) {
                                      return expression.expression_id ==
                                             relational.window_invocations
                                                 .front()
                                                 .function_expression_id;
                                    })
                              : relational.expressions.end();
    const auto result_descriptor =
        relational.window_invocations.size() == 1
            ? std::ranges::find_if(
                  relational.descriptors, [&](const auto& descriptor) {
                    return descriptor.descriptor_id ==
                           relational.window_invocations.front()
                               .result_descriptor_id;
                  })
            : relational.descriptors.end();
    const bool exact_argument_arity =
        relational.window_invocations.size() == 1 &&
        (nth_value_window
             ? relational.window_invocations.front()
                       .argument_expression_ids.size() == 2
             : aggregate_count_star_window
             ? relational.window_invocations.front()
                   .argument_expression_ids.empty()
             : ((ntile_window || value_operand_window)
                    ? relational.window_invocations.front()
                              .argument_expression_ids.size() == 1
                    : relational.window_invocations.front()
                          .argument_expression_ids.empty()));
    const auto ntile_argument =
        (ntile_window || value_operand_window) && exact_argument_arity
            ? std::ranges::find_if(
                  relational.expressions, [&](const auto& expression) {
                    return expression.expression_id ==
                           relational.window_invocations.front()
                               .argument_expression_ids.front();
                  })
            : relational.expressions.end();
    const auto ntile_argument_descriptor =
        ntile_argument == relational.expressions.end()
            ? relational.descriptors.end()
            : std::ranges::find_if(
                  relational.descriptors, [&](const auto& descriptor) {
                  return descriptor.descriptor_id ==
                           ntile_argument->result_descriptor_id;
                  });
    const auto nth_position_argument =
        nth_value_window && exact_argument_arity
            ? std::ranges::find_if(
                  relational.expressions, [&](const auto& expression) {
                    return expression.expression_id ==
                           relational.window_invocations.front()
                               .argument_expression_ids[1];
                  })
            : relational.expressions.end();
    const auto nth_position_descriptor =
        nth_position_argument == relational.expressions.end()
            ? relational.descriptors.end()
            : std::ranges::find_if(
                  relational.descriptors, [&](const auto& descriptor) {
                  return descriptor.descriptor_id ==
                           nth_position_argument->result_descriptor_id;
                  });
    const auto nth_order_argument =
        nth_value_window && sort_node->bound_expression_ids.size() == 1
            ? std::ranges::find_if(
                  relational.expressions, [&](const auto& expression) {
                    return expression.expression_id ==
                           sort_node->bound_expression_ids.front();
                  })
            : relational.expressions.end();
    const auto nth_order_descriptor =
        nth_order_argument == relational.expressions.end()
            ? relational.descriptors.end()
            : std::ranges::find_if(
                  relational.descriptors, [&](const auto& descriptor) {
                    return descriptor.descriptor_id ==
                           nth_order_argument->result_descriptor_id;
                  });
    const auto aggregate_order_argument =
        aggregate_window && sort_node->bound_expression_ids.size() == 1
            ? std::ranges::find_if(
                  relational.expressions, [&](const auto& expression) {
                    return expression.expression_id ==
                           sort_node->bound_expression_ids.front();
                  })
            : relational.expressions.end();
    const auto aggregate_order_descriptor =
        aggregate_order_argument == relational.expressions.end()
            ? relational.descriptors.end()
            : std::ranges::find_if(
                  relational.descriptors, [&](const auto& descriptor) {
                    return descriptor.descriptor_id ==
                           aggregate_order_argument->result_descriptor_id;
                  });
    std::vector<std::uint32_t> expected_window_bound_expression_ids;
    if (sort_node->bound_expression_ids.size() == 1 &&
        relational.window_invocations.size() == 1 && exact_argument_arity) {
      expected_window_bound_expression_ids.push_back(
          sort_node->bound_expression_ids.front());
      expected_window_bound_expression_ids.insert(
          expected_window_bound_expression_ids.end(),
          relational.window_invocations.front().argument_expression_ids.begin(),
          relational.window_invocations.front().argument_expression_ids.end());
      expected_window_bound_expression_ids.push_back(
          relational.window_invocations.front().function_expression_id);
    }
    std::vector<const RelationalOutputRecord*> window_outputs;
    std::vector<const RelationalOutputRecord*> ordered_input_outputs;
    for (const auto& output : relational.outputs) {
      if (output.relation_node_id == window_node->node_id) {
        window_outputs.push_back(&output);
      }
      if (output.relation_node_id == scan_node->node_id) {
        ordered_input_outputs.push_back(&output);
      }
    }
    std::ranges::sort(window_outputs, {},
                      &RelationalOutputRecord::ordinal);
    std::ranges::sort(ordered_input_outputs, {},
                      &RelationalOutputRecord::ordinal);
    bool passthrough_outputs =
        window_outputs.size() == sort_node->output_descriptor_ids.size() + 1 &&
        ordered_input_outputs.size() ==
            sort_node->output_descriptor_ids.size();
    for (std::size_t ordinal = 0;
         passthrough_outputs &&
         ordinal < sort_node->output_descriptor_ids.size(); ++ordinal) {
      passthrough_outputs =
          ordered_input_outputs[ordinal]->ordinal == ordinal &&
          ordered_input_outputs[ordinal]->descriptor_id ==
              sort_node->output_descriptor_ids[ordinal] &&
          window_outputs[ordinal]->ordinal == ordinal &&
          window_outputs[ordinal]->visible &&
          window_outputs[ordinal]->descriptor_id ==
              sort_node->output_descriptor_ids[ordinal] &&
          window_outputs[ordinal]->expression_id ==
              ordered_input_outputs[ordinal]->expression_id;
    }
    if (window_property_uuid ==
            window_node->delivered_property_uuids.end() ||
        window_property == relational.properties.end() ||
        window_property->property_kind != RelationalPropertyKind::kWindow ||
        window_property->origin_node_id != window_node->node_id ||
        !window_property->expression_ids.empty() ||
        !window_property->ordering_terms.empty() ||
        window_property->dependency_property_uuids !=
            std::vector<std::string>{ordering_property_uuid} ||
        window_property->window_frame_descriptor_uuid.empty() ||
        sort_node->bound_expression_ids.size() != 1 ||
        relational.window_definitions.size() != 1 ||
        relational.window_invocations.size() != 1 || !passthrough_outputs ||
        relational.window_definitions.front().relation_node_id !=
            window_node->node_id ||
        relational.window_definitions.front().canonical_name_key.has_value() ||
        relational.window_definitions.front().inherited_window_id.has_value() ||
        !relational.window_definitions.front()
             .partition_expression_ids.empty() ||
        relational.window_definitions.front().ordering_terms.size() != 1 ||
        relational.window_definitions.front()
                .ordering_terms.front()
                .expression_id != sort_node->bound_expression_ids.front() ||
        relational.window_definitions.front().frame_unit.has_value() ||
        relational.window_definitions.front().frame_start.has_value() ||
        relational.window_definitions.front().frame_end.has_value() ||
        relational.window_definitions.front().exclusion !=
            RelationalWindowFrameExclusion::kNoOthers ||
        relational.window_invocations.front().relation_node_id !=
            window_node->node_id ||
        relational.window_invocations.front().window_definition_id !=
            relational.window_definitions.front().window_id ||
        (aggregate_window &&
         (!exact_unary_aggregate ||
          !aggregate_window_row->executable ||
          !aggregate_window_row->aggregate_as_window ||
          aggregate_window_row->abi_version != 1)) ||
        relational.window_invocations.front().function_abi_version != 1 ||
        relational.window_invocations.front().builtin_id !=
            expected_builtin_id ||
        relational.window_invocations.front().function_uuid !=
            expected_function_uuid ||
        !exact_argument_arity ||
        function == relational.expressions.end() ||
        function->expression_kind != RelationalExpressionKind::kFunctionCall ||
        function->function_uuid !=
            std::optional<std::string>(expected_function_uuid) ||
        function->bound_name_uuid.has_value() ||
        function->operator_name.has_value() ||
        function->literal_kind.has_value() ||
        function->literal_or_parameter_ref.has_value() ||
        function->child_expression_ids !=
            relational.window_invocations.front().argument_expression_ids ||
        function->result_descriptor_id !=
            relational.window_invocations.front().result_descriptor_id ||
        result_descriptor == relational.descriptors.end() ||
        (aggregate_window &&
         (canonical_int64_type_uuid.empty() ||
          (aggregate_boolean_window && canonical_boolean_type_uuid.empty()) ||
          result_descriptor->type_uuid !=
              (aggregate_boolean_window ? canonical_boolean_type_uuid
                                        : canonical_int64_type_uuid))) ||
        result_descriptor->nullability !=
            (value_window && !aggregate_count_window
                 ? RelationalNullability::kNullable
                 : RelationalNullability::kNonNull) ||
        window_node->bound_expression_ids !=
            expected_window_bound_expression_ids ||
        (ntile_window &&
         (ntile_argument == relational.expressions.end() ||
          ntile_argument->expression_kind !=
              RelationalExpressionKind::kLiteral ||
          !ntile_argument->child_expression_ids.empty() ||
          ntile_argument->bound_name_uuid.has_value() ||
          ntile_argument->function_uuid.has_value() ||
          ntile_argument->literal_kind != RelationalLiteralKind::kNumeric ||
          ntile_argument->operator_name.has_value() ||
          !ntile_argument->literal_or_parameter_ref.has_value() ||
          ntile_argument_descriptor == relational.descriptors.end() ||
          ntile_argument_descriptor->descriptor_id ==
              result_descriptor->descriptor_id ||
          ntile_argument_descriptor->descriptor_uuid ==
              result_descriptor->descriptor_uuid ||
          ntile_argument_descriptor->descriptor_uuid ==
              expected_function_uuid ||
          ntile_argument_descriptor->type_uuid !=
              result_descriptor->type_uuid ||
          ntile_argument_descriptor->nullability !=
              RelationalNullability::kNonNull ||
          ntile_argument_descriptor->collation_uuid.has_value() ||
          ntile_argument_descriptor->timezone_profile_id.has_value() ||
          ntile_argument_descriptor->width.has_value() ||
          ntile_argument_descriptor->precision.has_value() ||
          ntile_argument_descriptor->scale.has_value())) ||
        (value_operand_window &&
         (ntile_argument == relational.expressions.end() ||
          ntile_argument->expression_kind !=
              RelationalExpressionKind::kIdentifier ||
          !ntile_argument->child_expression_ids.empty() ||
          !ntile_argument->bound_name_uuid.has_value() ||
          ntile_argument->function_uuid.has_value() ||
          ntile_argument->literal_kind.has_value() ||
          ntile_argument->literal_or_parameter_ref.has_value() ||
          ntile_argument->operator_name.has_value() ||
          ntile_argument_descriptor == relational.descriptors.end() ||
          ntile_argument_descriptor->descriptor_id ==
              result_descriptor->descriptor_id ||
          ntile_argument_descriptor->descriptor_uuid ==
              result_descriptor->descriptor_uuid ||
          ntile_argument_descriptor->descriptor_uuid ==
              expected_function_uuid ||
          (aggregate_window && !aggregate_count_window &&
           !(aggregate_bounded_signed_window
                 ? std::ranges::find(canonical_bounded_signed_type_uuids,
                                     ntile_argument_descriptor->type_uuid) !=
                       canonical_bounded_signed_type_uuids.end()
                 : aggregate_boolean_window &&
                       ntile_argument_descriptor->type_uuid ==
                           canonical_boolean_type_uuid)) ||
          (!aggregate_window &&
           (canonical_int64_type_uuid.empty() ||
            (ntile_argument_descriptor->type_uuid !=
                 canonical_int64_type_uuid &&
             (!navigation_value_window ||
              canonical_boolean_type_uuid.empty() ||
              ntile_argument_descriptor->type_uuid !=
                  canonical_boolean_type_uuid)) ||
            ntile_argument_descriptor->type_uuid !=
                result_descriptor->type_uuid ||
            ntile_argument_descriptor->collation_uuid.has_value() ||
            ntile_argument_descriptor->timezone_profile_id.has_value() ||
            ntile_argument_descriptor->width.has_value() ||
            ntile_argument_descriptor->precision.has_value() ||
            ntile_argument_descriptor->scale.has_value() ||
            result_descriptor->collation_uuid.has_value() ||
            result_descriptor->timezone_profile_id.has_value() ||
            result_descriptor->width.has_value() ||
            result_descriptor->precision.has_value() ||
            result_descriptor->scale.has_value() ||
            ntile_argument_descriptor->collation_uuid !=
                result_descriptor->collation_uuid ||
            ntile_argument_descriptor->timezone_profile_id !=
                result_descriptor->timezone_profile_id ||
            ntile_argument_descriptor->width != result_descriptor->width ||
            ntile_argument_descriptor->precision !=
                result_descriptor->precision ||
            ntile_argument_descriptor->scale != result_descriptor->scale)) ||
          (aggregate_window &&
           ((!aggregate_count_window &&
             (ntile_argument_descriptor->collation_uuid.has_value() ||
              ntile_argument_descriptor->timezone_profile_id.has_value() ||
              ntile_argument_descriptor->width.has_value() ||
              ntile_argument_descriptor->precision.has_value() ||
              ntile_argument_descriptor->scale.has_value())) ||
            result_descriptor->collation_uuid.has_value() ||
            result_descriptor->timezone_profile_id.has_value() ||
            result_descriptor->width.has_value() ||
            result_descriptor->precision.has_value() ||
            result_descriptor->scale.has_value())) ||
          std::ranges::find(sort_node->output_descriptor_ids,
                            ntile_argument_descriptor->descriptor_id) ==
              sort_node->output_descriptor_ids.end())) ||
        (aggregate_window &&
         (aggregate_order_argument == relational.expressions.end() ||
          aggregate_order_argument->expression_kind !=
              RelationalExpressionKind::kIdentifier ||
          !aggregate_order_argument->child_expression_ids.empty() ||
          !aggregate_order_argument->bound_name_uuid.has_value() ||
          aggregate_order_argument->function_uuid.has_value() ||
          aggregate_order_argument->literal_kind.has_value() ||
          aggregate_order_argument->literal_or_parameter_ref.has_value() ||
          aggregate_order_argument->operator_name.has_value() ||
          aggregate_order_descriptor == relational.descriptors.end() ||
          canonical_int64_type_uuid.empty() ||
          std::ranges::find(canonical_bounded_signed_type_uuids,
                            aggregate_order_descriptor->type_uuid) ==
              canonical_bounded_signed_type_uuids.end() ||
          aggregate_order_descriptor->collation_uuid.has_value() ||
          aggregate_order_descriptor->timezone_profile_id.has_value() ||
          aggregate_order_descriptor->width.has_value() ||
          aggregate_order_descriptor->precision.has_value() ||
          aggregate_order_descriptor->scale.has_value() ||
          std::ranges::find(sort_node->output_descriptor_ids,
                            aggregate_order_descriptor->descriptor_id) ==
              sort_node->output_descriptor_ids.end())) ||
        (nth_value_window &&
         (nth_order_argument == relational.expressions.end() ||
          nth_order_argument->expression_kind !=
              RelationalExpressionKind::kIdentifier ||
          !nth_order_argument->child_expression_ids.empty() ||
          !nth_order_argument->bound_name_uuid.has_value() ||
          nth_order_argument->function_uuid.has_value() ||
          nth_order_argument->literal_kind.has_value() ||
          nth_order_argument->literal_or_parameter_ref.has_value() ||
          nth_order_argument->operator_name.has_value() ||
          nth_order_descriptor == relational.descriptors.end() ||
          canonical_int64_type_uuid.empty() ||
          nth_order_descriptor->type_uuid != canonical_int64_type_uuid ||
          nth_order_descriptor->collation_uuid.has_value() ||
          nth_order_descriptor->timezone_profile_id.has_value() ||
          nth_order_descriptor->width.has_value() ||
          nth_order_descriptor->precision.has_value() ||
          nth_order_descriptor->scale.has_value() ||
          std::ranges::find(sort_node->output_descriptor_ids,
                            nth_order_descriptor->descriptor_id) ==
              sort_node->output_descriptor_ids.end() ||
          nth_position_argument == relational.expressions.end() ||
          nth_position_argument->expression_kind !=
              RelationalExpressionKind::kLiteral ||
          !nth_position_argument->child_expression_ids.empty() ||
          nth_position_argument->bound_name_uuid.has_value() ||
          nth_position_argument->function_uuid.has_value() ||
          nth_position_argument->literal_kind !=
              RelationalLiteralKind::kNumeric ||
          nth_position_argument->operator_name.has_value() ||
          !nth_position_argument->literal_or_parameter_ref.has_value() ||
          nth_position_descriptor == relational.descriptors.end() ||
          nth_position_descriptor->descriptor_id ==
              ntile_argument_descriptor->descriptor_id ||
          nth_position_descriptor->descriptor_id ==
              result_descriptor->descriptor_id ||
          nth_position_descriptor->descriptor_id ==
              nth_order_descriptor->descriptor_id ||
          nth_position_descriptor->descriptor_uuid ==
              ntile_argument_descriptor->descriptor_uuid ||
          nth_position_descriptor->descriptor_uuid ==
              result_descriptor->descriptor_uuid ||
          nth_position_descriptor->descriptor_uuid ==
              nth_order_descriptor->descriptor_uuid ||
          nth_position_descriptor->descriptor_uuid == expected_function_uuid ||
          nth_position_descriptor->type_uuid != canonical_int64_type_uuid ||
          nth_position_descriptor->nullability !=
              RelationalNullability::kNonNull ||
          nth_position_descriptor->collation_uuid.has_value() ||
          nth_position_descriptor->timezone_profile_id.has_value() ||
          nth_position_descriptor->width.has_value() ||
          nth_position_descriptor->precision.has_value() ||
          nth_position_descriptor->scale.has_value())) ||
        window_outputs.back()->ordinal !=
            sort_node->output_descriptor_ids.size() ||
        !window_outputs.back()->visible ||
        window_outputs.back()->expression_id != function->expression_id ||
        window_outputs.back()->descriptor_id !=
            result_descriptor->descriptor_id ||
        window_outputs.back()->output_name_utf8 !=
            relational.window_invocations.front().output_name_utf8) {
      return Refuse("QOW-DIAG-QRY-004-HEAP-OPTIMIZER-PROFILE-V1",
                    aggregate_window
                        ? "heap_int64_aggregate_window_binding"
                        : value_window
                        ? (first_value_window
                               ? "heap_first_value_window_binding"
                               : (last_value_window
                                      ? "heap_last_value_window_binding"
                                      : (nth_value_window
                                             ? "heap_nth_value_window_binding"
                                             : (lag_window
                                                    ? "heap_lag_window_binding"
                                                    : "heap_lead_window_binding"))))
                        : (ntile_window
                        ? "heap_ntile_window_binding"
                        : (cume_dist_window
                        ? "heap_cume_dist_window_binding"
                        : (percent_rank_window
                               ? "heap_percent_rank_window_binding"
                               : (dense_rank_window
                                      ? "heap_dense_rank_window_binding"
                                      : (rank_window
                                             ? "heap_rank_window_binding"
                                             : "heap_row_number_window_binding"))))));
    }
  }
  const auto& node = *scan_node;
  if (node.node_kind != RelationalDagNodeKind::kScan ||
      node.semantic_variant_id != "relation.source.v1" ||
      !node.input_node_ids.empty() || node.shareable ||
      node.required_object_uuids.size() != 1 ||
      !IsCanonicalUuid(node.required_object_uuids.front()) ||
      node.output_descriptor_ids.empty() ||
      node.output_descriptor_ids.size() != node.bound_expression_ids.size() ||
      !node.values_row_ids.empty() || !node.required_property_uuids.empty() ||
      !node.delivered_property_uuids.empty() ||
      !relational.values_rows.empty() || !relational.grouping_sets.empty() ||
      relational.properties.size() !=
          static_cast<std::size_t>(sort_node != nullptr) +
              static_cast<std::size_t>(window_node != nullptr)) {
    return Refuse("QOW-DIAG-QRY-004-HEAP-OPTIMIZER-PROFILE-V1",
                  "relation_source_leaf");
  }
  const std::size_t width = node.output_descriptor_ids.size();
  const auto scan_output_count = std::ranges::count_if(
      relational.outputs, [&](const auto& output) {
        return output.relation_node_id == node.node_id;
      });
  if (scan_output_count != width) {
    return Refuse("QOW-DIAG-QRY-004-HEAP-OPTIMIZER-BINDING-V1",
                  "complete_visible_scan_width");
  }

  CanonicalRelationalPlanningScope planning_scope;
  planning_scope.catalog_epoch_uuid =
      context.catalog_epoch_uuid.canonical;
  planning_scope.security_context_uuid =
      context.authorization_context.authority_uuid.canonical;
  planning_scope.statement_uuid = context.statement_uuid.canonical;
  planning_scope.owning_transaction_uuid = context.transaction_uuid.canonical;
  planning_scope.statement_snapshot_uuid =
      context.statement_snapshot_uuid.canonical;
  planning_scope.statement_metadata_snapshot_uuid =
      context.statement_metadata_snapshot_uuid.canonical;
  planning_scope.local_transaction_id = context.local_transaction_id;
  planning_scope.snapshot_visible_through_local_transaction_id =
      context.snapshot_visible_through_local_transaction_id;
  planning_scope.metadata_snapshot_engine_owned =
      context.statement_metadata_snapshot_engine_owned;
  planning_scope.authorization_context_engine_owned =
      context.authorization_context.present;
  auto logical =
      PopulateCanonicalLogicalGraphFromAdmittedTypedRelationalDag(
          relational, planning_scope);
  if (!logical.accepted ||
      logical.property_catalog.properties.size() !=
          static_cast<std::size_t>(sort_node != nullptr) +
              static_cast<std::size_t>(window_node != nullptr)) {
    if (!logical.issues.empty()) {
      return Refuse(logical.issues.front().diagnostic_id,
                    logical.issues.front().field_id);
    }
    return Refuse("QOW-DIAG-QRY-004-HEAP-OPTIMIZER-PROFILE-V1",
                  "object_source_properties");
  }
  auto registered_mga = canonical_mga;
  registered_mga.current = false;
  if (!plan::CanonicalMgaStatementContextEqual(
          logical.logical_graph.mga_statement_context, registered_mga) ||
      !plan::CanonicalMgaStatementContextEqual(
          logical.property_catalog.mga_statement_context, registered_mga)) {
    return Refuse("QOW-DIAG-QRY-004-HEAP-OPTIMIZER-MGA-SNAPSHOT-V1",
                  "bridge_statement_snapshot_carriage");
  }
  logical.logical_graph.mga_statement_context = canonical_mga;
  logical.property_catalog.mga_statement_context = canonical_mga;

  const std::string& relation_uuid = node.required_object_uuids.front();
  const auto temporary =
      CheckMgaTemporaryTableVisibility(context, relation_uuid);
  if (!temporary.ok || !temporary.table_visible ||
      temporary.known_temporary || temporary.table.temporary) {
    return Refuse("QOW-DIAG-QRY-004-HEAP-OPTIMIZER-RELATION-V1",
                  temporary.ok ? "current_non_temporary_relation"
                               : temporary.diagnostic.detail);
  }
  const auto loaded = LoadMgaRelationStorageDescriptor(context, relation_uuid);
  if (!loaded.ok) {
    return Refuse("QOW-DIAG-QRY-004-HEAP-OPTIMIZER-DESCRIPTOR-V1",
                  loaded.diagnostic.detail);
  }
  const auto& persisted = loaded.descriptor;
  if (persisted.relation_uuid.canonical != relation_uuid ||
      persisted.database_uuid.canonical != context.database_uuid.canonical ||
      persisted.relation_kind != "table" ||
      persisted.storage_profile != "local_mga_rowstore_v1" ||
      !IsCanonicalUuid(persisted.descriptor_uuid.canonical) ||
      persisted.descriptor_generation == 0 ||
      (persisted.descriptor_status != "production_descriptor" &&
       persisted.descriptor_status != "metadata_bridge_vetted_descriptor") ||
      persisted.columns.size() < width) {
    return Refuse("QOW-DIAG-QRY-004-HEAP-OPTIMIZER-DESCRIPTOR-V1",
                  "current_persisted_local_heap_descriptor");
  }

  std::unordered_set<std::uint32_t> output_ids;
  std::unordered_set<std::uint32_t> expression_ids;
  std::unordered_set<std::uint32_t> descriptor_ids;
  std::unordered_set<std::string> column_uuids;
  std::unordered_set<std::string> column_names;
  std::vector<const RelationalOutputRecord*> scan_outputs;
  std::vector<std::string> projection_type_names;
  std::vector<EngineDescriptor> projection_descriptors;
  std::unordered_map<std::uint32_t, const RelationalExpressionRecord*>
      expressions_by_id;
  std::unordered_map<std::uint32_t, const RelationalTypeDescriptor*>
      descriptors_by_id;
  for (const auto& output : relational.outputs) {
    if (output.relation_node_id == node.node_id) {
      scan_outputs.push_back(&output);
    }
  }
  std::ranges::sort(scan_outputs, {}, &RelationalOutputRecord::ordinal);
  projection_type_names.reserve(scan_outputs.size());
  projection_descriptors.reserve(scan_outputs.size());
  for (const auto& expression : relational.expressions) {
    expressions_by_id.emplace(expression.expression_id, &expression);
  }
  for (const auto& descriptor : relational.descriptors) {
    descriptors_by_id.emplace(descriptor.descriptor_id, &descriptor);
  }
  for (std::size_t ordinal = 0; ordinal < width; ++ordinal) {
    const auto& output = *scan_outputs[ordinal];
    const auto expression_it =
        expressions_by_id.find(node.bound_expression_ids[ordinal]);
    const auto descriptor_it =
        descriptors_by_id.find(output.descriptor_id);
    if (expression_it == expressions_by_id.end() ||
        descriptor_it == descriptors_by_id.end()) {
      return Refuse("QOW-DIAG-QRY-004-HEAP-OPTIMIZER-BINDING-V1",
                    "scan_expression_descriptor_resolution");
    }
    const auto& expression = *expression_it->second;
    const auto& descriptor = *descriptor_it->second;
    if (!expression.bound_name_uuid.has_value() ||
        !IsCanonicalUuid(*expression.bound_name_uuid)) {
      return Refuse("QOW-DIAG-QRY-004-HEAP-OPTIMIZER-BINDING-V1",
                    "scan_column_uuid_binding");
    }
    const auto persisted_column = std::ranges::find_if(
        persisted.columns, [&](const auto& candidate) {
          return candidate.column_uuid.canonical ==
                 *expression.bound_name_uuid;
        });
    if (persisted_column == persisted.columns.end()) {
      return Refuse("QOW-DIAG-QRY-004-HEAP-OPTIMIZER-BINDING-V1",
                    "scan_projected_column_resolution");
    }
    const auto& column = *persisted_column;
    const auto persisted_type_uuid = ExactDescriptorField(
        column.value_descriptor.encoded_descriptor, "type_uuid");
    const bool nullable =
        descriptor.nullability == RelationalNullability::kNullable;
    if (output.relation_node_id != node.node_id || !output.visible ||
        output.ordinal != ordinal || output.output_id == 0 ||
        !output_ids.insert(output.output_id).second ||
        output.descriptor_id != node.output_descriptor_ids[ordinal] ||
        expression.expression_id != output.expression_id ||
        !expression_ids.insert(expression.expression_id).second ||
        node.bound_expression_ids[ordinal] != expression.expression_id ||
        expression.expression_kind != RelationalExpressionKind::kIdentifier ||
        !expression.child_expression_ids.empty() ||
        expression.result_descriptor_id != output.descriptor_id ||
        expression.function_uuid.has_value() || expression.literal_kind.has_value() ||
        expression.operator_name.has_value() ||
        expression.literal_or_parameter_ref.has_value() ||
        descriptor.descriptor_id != output.descriptor_id ||
        !descriptor_ids.insert(descriptor.descriptor_id).second ||
        !IsCanonicalUuid(descriptor.descriptor_uuid) ||
        !IsCanonicalUuid(descriptor.type_uuid) ||
        descriptor.nullability == RelationalNullability::kUnknown ||
        (descriptor.collation_uuid.has_value() &&
         !IsCanonicalUuid(*descriptor.collation_uuid)) ||
        (descriptor.timezone_profile_id.has_value() &&
         descriptor.timezone_profile_id->empty()) ||
        !IsCanonicalUuid(column.column_uuid.canonical) ||
        !column_uuids.insert(column.column_uuid.canonical).second ||
        column.column_uuid.canonical != *expression.bound_name_uuid ||
        column.canonical_name_key.empty() ||
        !column_names.insert(column.canonical_name_key).second ||
        output.output_name_utf8 != column.canonical_name_key ||
        column.value_descriptor.descriptor_uuid.canonical !=
            descriptor.descriptor_uuid ||
        column.value_descriptor.encoded_descriptor.empty() ||
        column.value_descriptor.canonical_type_name.empty() ||
        !persisted_type_uuid.has_value() ||
        !IsCanonicalUuid(*persisted_type_uuid) ||
        *persisted_type_uuid != descriptor.type_uuid ||
        column.nullable != nullable ||
        !ExactNullabilityCarrierMatches(
            column.value_descriptor.encoded_descriptor, nullable) ||
        (descriptor.collation_uuid.has_value()
             ? column.collation_uuid != *descriptor.collation_uuid
             : !column.collation_uuid.empty()) ||
        !ExactOptionalDescriptorFieldMatches(
            column.value_descriptor.encoded_descriptor, "collation_uuid",
            descriptor.collation_uuid) ||
        !ExactOptionalDescriptorFieldMatches(
            column.value_descriptor.encoded_descriptor,
            "timezone_profile_id", descriptor.timezone_profile_id)) {
      return Refuse("QOW-DIAG-QRY-004-HEAP-OPTIMIZER-BINDING-V1",
                    "persisted_ordinal_descriptor_binding");
    }
    projection_type_names.push_back(
        column.value_descriptor.canonical_type_name);
    auto projection_descriptor = column.value_descriptor;
    projection_descriptor.descriptor_kind = "scalar";
    projection_descriptors.push_back(std::move(projection_descriptor));
  }

  const auto authorization = EvaluateMaterializedAuthorization(
      context, context.authorization_context, "SELECT", relation_uuid);
  if (!authorization.authorized || authorization.denied ||
      authorization.policy_recheck_required ||
      !authorization.diagnostics.empty()) {
    return Refuse("QOW-DIAG-QRY-004-HEAP-OPTIMIZER-SECURITY-V1",
                  authorization.diagnostics.empty()
                      ? "select_authorization"
                      : authorization.diagnostics.front().detail);
  }

  opt::CanonicalNativeObjectAdmissionContext admission_context;
  admission_context.statement_uuid = context.statement_uuid.canonical;
  admission_context.catalog_snapshot_uuid =
      context.statement_metadata_snapshot_uuid.canonical;
  admission_context.security_context_uuid =
      context.authorization_context.authority_uuid.canonical;
  admission_context.catalog_generation = context.catalog_generation_id;
  admission_context.authorization_catalog_generation =
      context.authorization_context.catalog_generation_id;
  admission_context.security_epoch =
      context.authorization_context.security_epoch;
  admission_context.policy_epoch =
      context.authorization_context.policy_epoch;
  admission_context.resource_epoch = context.resource_epoch;
  admission_context.capability_snapshot_uuid =
      context.optimizer_capability_snapshot_uuid.canonical;
  admission_context.resource_snapshot_uuid =
      context.optimizer_resource_snapshot_uuid.canonical;
  admission_context.route_snapshot_uuid =
      context.optimizer_route_snapshot_uuid.canonical;
  admission_context.route_epoch = context.optimizer_route_epoch;
  admission_context.route_generation = context.optimizer_route_generation;
  admission_context.memory_budget_bytes =
      context.optimizer_memory_budget_bytes;
  admission_context.maximum_candidate_count =
      context.optimizer_maximum_candidate_count;
  admission_context.maximum_memo_groups =
      context.optimizer_maximum_memo_groups;
  admission_context.maximum_search_steps =
      context.optimizer_maximum_search_steps;
  admission_context.maximum_planning_time_ns =
      context.optimizer_maximum_planning_time_ns;
  admission_context.spill_allowed = context.optimizer_spill_allowed;
  admission_context.local_transaction_id = context.local_transaction_id;
  admission_context.statement_snapshot_id =
      context.snapshot_visible_through_local_transaction_id;
  admission_context.mga_statement_context = canonical_mga;
  admission_context.admitted_at_monotonic_ns = admitted_at_monotonic_ns;
  admission_context.metadata_snapshot_engine_owned = true;
  admission_context.authorization_context_engine_owned = true;
  admission_context.catalog_object_uuids = {relation_uuid};
  admission_context.authorized_object_uuids = {relation_uuid};
  admission_context.catalog_object_evidence_engine_owned = true;
  admission_context.authorization_object_evidence_engine_owned = true;

  auto built = opt::BuildCanonicalObjectAwareNativeOptimizerAdmissionRequest(
      logical.logical_graph, logical.property_catalog, admission_context);
  if (!built.built || !built.admission.admitted ||
      !built.admission.planning_allowed ||
      !built.admission.degraded_for_unknown_statistics ||
      built.admission.benchmark_clean_ready ||
      built.admission.data_access_allowed ||
      built.admission.evidence.size() != 8 ||
      built.request.statistics.node_estimates.size() !=
          relational.nodes.size()) {
    return Refuse(
        built.diagnostic_id.empty()
            ? "QOW-DIAG-QRY-004-HEAP-OPTIMIZER-ADMISSION-V1"
            : built.diagnostic_id,
        built.field_id.empty() ? "heap_source_unknown_statistics_admission"
                               : built.field_id);
  }

  CanonicalHeapOptimizerAdmissionResult result;
  result.built = true;
  result.request = std::move(built.request);
  result.admission = std::move(built.admission);
  result.current_relation_descriptor_uuid =
      persisted.descriptor_uuid.canonical;
  result.current_relation_descriptor_generation =
      persisted.descriptor_generation;
  result.current_relation_projection_type_names =
      std::move(projection_type_names);
  result.current_relation_projection_descriptors =
      std::move(projection_descriptors);
  return result;
}

}  // namespace scratchbird::engine::internal_api

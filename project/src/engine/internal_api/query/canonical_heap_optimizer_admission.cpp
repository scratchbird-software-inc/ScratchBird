// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "query/canonical_heap_optimizer_admission.hpp"

#include "mga_relation_store/mga_relation_store.hpp"
#include "query/canonical_relational_bridge.hpp"
#include "security/security_model.hpp"
#include "transaction/transaction_api.hpp"

#include <algorithm>
#include <charconv>
#include <cctype>
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
      relational.nodes.size() > 5) {
    return Refuse("QOW-DIAG-QRY-004-HEAP-OPTIMIZER-PROFILE-V1",
                  "heap_scan_with_optional_filter_project_limit_root");
  }
  const RelationalDagNode* scan_node = nullptr;
  const RelationalDagNode* filter_node = nullptr;
  const RelationalDagNode* project_node = nullptr;
  const RelationalDagNode* sort_node = nullptr;
  const RelationalDagNode* aggregate_node = nullptr;
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
    } else if (candidate.node_kind == RelationalDagNodeKind::kAggregate) {
      if (aggregate_node != nullptr) {
        return Refuse("QOW-DIAG-QRY-004-HEAP-OPTIMIZER-PROFILE-V1",
                      "one_optional_heap_aggregate");
      }
      aggregate_node = &candidate;
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
                                   : (sort_node != nullptr
                                          ? sort_node
                                          : (filter_node != nullptr ? filter_node
                                                                    : scan_node))));
  const auto expected_project_input =
      sort_node != nullptr
          ? sort_node->node_id
          : (filter_node == nullptr ? 0 : filter_node->node_id);
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
  if (scan_node == nullptr ||
      relational.root_node_id != terminal_node->node_id ||
      relational.nodes.size() !=
          1 + static_cast<std::size_t>(filter_node != nullptr) +
              static_cast<std::size_t>(project_node != nullptr) +
              static_cast<std::size_t>(sort_node != nullptr) +
              static_cast<std::size_t>(aggregate_node != nullptr) +
              static_cast<std::size_t>(limit_node != nullptr) ||
      (aggregate_node != nullptr &&
       (project_node != nullptr || sort_node != nullptr)) ||
      (filter_node != nullptr &&
       (filter_node->input_node_ids !=
            std::vector<std::uint32_t>{scan_node->node_id} ||
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
       (project_node->input_node_ids !=
            std::vector<std::uint32_t>{expected_project_input} ||
        project_node->semantic_variant_id !=
            "project.catalog-visible-columns.v1" ||
        project_node->bound_expression_ids.empty() ||
        project_node->bound_expression_ids.size() !=
            project_node->output_descriptor_ids.size() ||
        project_node->output_descriptor_ids.empty() ||
        project_node->output_descriptor_ids.size() >=
            scan_node->output_descriptor_ids.size() ||
        std::ranges::any_of(
            project_node->output_descriptor_ids,
            [&](const auto descriptor_id) {
              return std::ranges::find(scan_node->output_descriptor_ids,
                                       descriptor_id) ==
                     scan_node->output_descriptor_ids.end();
            }) ||
        !project_node->required_object_uuids.empty() ||
        !project_node->values_row_ids.empty() ||
        !project_node->required_property_uuids.empty() ||
        !project_node->delivered_property_uuids.empty())) ||
      (sort_node != nullptr &&
       (sort_node->input_node_ids !=
            std::vector<std::uint32_t>{
                filter_node != nullptr ? filter_node->node_id
                                       : scan_node->node_id} ||
        sort_node->semantic_variant_id != "sort.required-order.v1" ||
        sort_node->bound_expression_ids.empty() ||
        sort_node->output_descriptor_ids != scan_node->output_descriptor_ids ||
        !sort_node->required_object_uuids.empty() ||
        !sort_node->values_row_ids.empty() ||
        sort_node->required_property_uuids.size() != 1 ||
        sort_node->delivered_property_uuids !=
            sort_node->required_property_uuids)) ||
      (aggregate_node != nullptr &&
       (aggregate_node->input_node_ids !=
            std::vector<std::uint32_t>{
                filter_node != nullptr ? filter_node->node_id
                                       : scan_node->node_id} ||
        (aggregate_node->semantic_variant_id !=
             "aggregate.global-count-star.v1" &&
         aggregate_node->semantic_variant_id !=
             "aggregate.global-count-expression.v1" &&
         aggregate_node->semantic_variant_id !=
             "aggregate.global-sum-expression.v1") ||
        aggregate_node->bound_expression_ids.size() != 1 ||
        aggregate_node->output_descriptor_ids.size() != 1 ||
        !aggregate_node->required_object_uuids.empty() ||
        !aggregate_node->values_row_ids.empty() ||
        !aggregate_node->required_property_uuids.empty() ||
        !aggregate_node->delivered_property_uuids.empty())) ||
      (limit_node != nullptr &&
       (limit_node->input_node_ids !=
            std::vector<std::uint32_t>{expected_limit_input} ||
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
          static_cast<std::size_t>(sort_node != nullptr)) {
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
          static_cast<std::size_t>(sort_node != nullptr)) {
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
  return result;
}

}  // namespace scratchbird::engine::internal_api

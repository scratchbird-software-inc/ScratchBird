// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "canonical_query_current_heap_composition.hpp"
#include "canonical_query_aggregate_composition.hpp"
#include "canonical_query_aggregate_registration.hpp"
#include "canonical_query_descriptor_support.hpp"
#include "canonical_query_filter_registration.hpp"
#include "canonical_query_model_family_composition_support.hpp"
#include "canonical_query_node_composition.hpp"
#include "canonical_query_object_free_composition_support.hpp"
#include "canonical_query_object_free_profile.hpp"
#include "canonical_query_projection_registration.hpp"
#include "canonical_query_relational_registration.hpp"
#include "canonical_query_runtime_memory_support.hpp"
#include "canonical_query_scalar_support.hpp"
#include "canonical_query_sort_registration.hpp"
#include "canonical_query_window_registration.hpp"
#include "canonical_relational_expression.hpp"
#include "datatype_catalog_manifest.hpp"
#include "datatype_operations.hpp"
#include "engine/executor/descriptor_value_runtime.hpp"
#include "hash_digest.hpp"
#include "mga_relation_store/mga_relation_store.hpp"
#include "query/canonical_heap_optimizer_admission.hpp"
#include "query/expression_api.hpp"
#include "security/security_model.hpp"
#include "transaction/transaction_api.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <fstream>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace scratchbird::engine::sblr {
namespace api = scratchbird::engine::internal_api;
namespace dt = scratchbird::core::datatypes;
namespace exec = scratchbird::engine::executor;
namespace plan = scratchbird::engine::planner;

// SEARCH_KEY: SB_ENGINE_CANONICAL_QUERY_CURRENT_HEAP_COMPOSITION_AUTHORITY
// Owns admitted single-source heap streaming, compact row memory/binding,
// and relational tails under engine-issued MGA authority. Does not select
// public routes, construct snapshots, mutate storage, or finalize transactions.

namespace {

struct CurrentHeapStreamingScanBinding {
  const api::RelationalDagNode* node{nullptr};
  std::string relation_uuid;
  api::MgaRelationStorageDescriptor persisted;
  std::vector<const api::MgaRelationColumnStorageDescriptor*> columns;
  std::vector<api::EngineDescriptor> descriptors;
};

struct CurrentHeapStreamingCompactRow {
  std::uint64_t source_ordinal{0};
  std::vector<std::string> values;
  std::vector<std::uint8_t> nulls;
};

bool CurrentHeapMemoryAdd(const std::uint64_t value, std::uint64_t* total) {
  if (total == nullptr ||
      value > std::numeric_limits<std::uint64_t>::max() - *total) {
    return false;
  }
  *total += value;
  return true;
}

bool CurrentHeapMemoryMultiply(const std::uint64_t count,
                               const std::uint64_t width,
                               std::uint64_t* product) {
  if (product == nullptr ||
      (width != 0 &&
       count > std::numeric_limits<std::uint64_t>::max() / width)) {
    return false;
  }
  *product = count * width;
  return true;
}

bool CurrentHeapAccountString(const std::string& value,
                              std::uint64_t* total) {
  return value.capacity() < std::numeric_limits<std::uint64_t>::max() &&
         CurrentHeapMemoryAdd(static_cast<std::uint64_t>(value.capacity()) + 1,
                              total);
}

bool CurrentHeapAccountDescriptor(const api::EngineDescriptor& descriptor,
                                  std::uint64_t* total) {
  return CurrentHeapAccountString(descriptor.descriptor_uuid.canonical, total) &&
         CurrentHeapAccountString(descriptor.descriptor_kind, total) &&
         CurrentHeapAccountString(descriptor.canonical_type_name, total) &&
         CurrentHeapAccountString(descriptor.encoded_descriptor, total);
}

bool CurrentHeapStreamingBindingMemory(
    const CurrentHeapStreamingScanBinding& binding,
    std::uint64_t* total) {
  if (total == nullptr) return false;
  *total = sizeof(binding);
  std::uint64_t allocation = 0;
  if (!CurrentHeapAccountString(binding.relation_uuid, total) ||
      !CurrentHeapAccountString(binding.persisted.descriptor_uuid.canonical,
                                total) ||
      !CurrentHeapAccountString(binding.persisted.database_uuid.canonical,
                                total) ||
      !CurrentHeapAccountString(binding.persisted.schema_uuid.canonical,
                                total) ||
      !CurrentHeapAccountString(binding.persisted.relation_uuid.canonical,
                                total) ||
      !CurrentHeapAccountString(
          binding.persisted.primary_filespace_uuid.canonical, total) ||
      !CurrentHeapAccountString(binding.persisted.relation_kind, total) ||
      !CurrentHeapAccountString(binding.persisted.storage_profile, total) ||
      !CurrentHeapMemoryMultiply(
          binding.persisted.columns.capacity(),
          sizeof(api::MgaRelationColumnStorageDescriptor), &allocation) ||
      !CurrentHeapMemoryAdd(allocation, total) ||
      !CurrentHeapMemoryMultiply(
          binding.columns.capacity(),
          sizeof(const api::MgaRelationColumnStorageDescriptor*), &allocation) ||
      !CurrentHeapMemoryAdd(allocation, total) ||
      !CurrentHeapMemoryMultiply(binding.descriptors.capacity(),
                                 sizeof(api::EngineDescriptor), &allocation) ||
      !CurrentHeapMemoryAdd(allocation, total)) {
    return false;
  }
  for (const auto& column : binding.persisted.columns) {
    if (!CurrentHeapAccountString(column.column_uuid.canonical, total) ||
        !CurrentHeapAccountString(column.canonical_name_key, total) ||
        !CurrentHeapAccountDescriptor(column.value_descriptor, total) ||
        !CurrentHeapAccountString(column.storage_class, total) ||
        !CurrentHeapAccountString(column.charset_uuid, total) ||
        !CurrentHeapAccountString(column.collation_uuid, total) ||
        !CurrentHeapAccountString(column.overflow_policy, total)) {
      return false;
    }
  }
  for (const auto& descriptor : binding.descriptors) {
    if (!CurrentHeapAccountDescriptor(descriptor, total)) return false;
  }
  return true;
}

bool CurrentHeapStreamingCompactRowMemory(
    const CurrentHeapStreamingCompactRow& row,
    std::uint64_t* total) {
  if (total == nullptr) return false;
  *total = sizeof(row);
  std::uint64_t allocation = 0;
  if (!CurrentHeapMemoryMultiply(row.values.capacity(), sizeof(std::string),
                                 &allocation) ||
      !CurrentHeapMemoryAdd(allocation, total) ||
      !CurrentHeapMemoryAdd(row.nulls.capacity(), total)) {
    return false;
  }
  for (const auto& value : row.values) {
    if (!CurrentHeapAccountString(value, total)) return false;
  }
  return true;
}

bool PrepareCurrentHeapStreamingScanBinding(
    const api::TypedRelationalDag& dag,
    const api::RelationalDagNode& scan,
    api::MgaRelationStorageDescriptor descriptor,
    CurrentHeapStreamingScanBinding* binding,
    std::string* detail) {
  if (binding == nullptr || detail == nullptr ||
      scan.required_object_uuids.size() != 1 ||
      scan.bound_expression_ids.size() != scan.output_descriptor_ids.size() ||
      descriptor.relation_uuid.canonical != scan.required_object_uuids.front() ||
      descriptor.descriptor_generation == 0 || descriptor.columns.empty()) {
    if (detail != nullptr) {
      *detail = "streaming heap scan descriptor authority is incomplete";
    }
    return false;
  }
  CurrentHeapStreamingScanBinding prepared;
  prepared.node = &scan;
  prepared.relation_uuid = scan.required_object_uuids.front();
  prepared.persisted = std::move(descriptor);
  prepared.columns.reserve(scan.output_descriptor_ids.size());
  prepared.descriptors.reserve(scan.output_descriptor_ids.size());
  std::unordered_set<std::string> projected_column_uuids;
  for (std::size_t ordinal = 0; ordinal < scan.output_descriptor_ids.size();
       ++ordinal) {
    const auto expression = std::ranges::find_if(
        dag.expressions, [&](const auto& candidate) {
          return candidate.expression_id == scan.bound_expression_ids[ordinal];
        });
    const auto relational_descriptor = std::ranges::find_if(
        dag.descriptors, [&](const auto& candidate) {
          return candidate.descriptor_id == scan.output_descriptor_ids[ordinal];
        });
    const auto output = std::ranges::find_if(
        dag.outputs, [&](const auto& candidate) {
          return candidate.relation_node_id == scan.node_id &&
                 candidate.ordinal == ordinal;
        });
    if (expression == dag.expressions.end() ||
        relational_descriptor == dag.descriptors.end() ||
        output == dag.outputs.end() ||
        expression->expression_kind !=
            api::RelationalExpressionKind::kIdentifier ||
        !expression->bound_name_uuid.has_value() ||
        expression->result_descriptor_id != relational_descriptor->descriptor_id ||
        !projected_column_uuids.insert(*expression->bound_name_uuid).second ||
        !output->visible ||
        output->descriptor_id != relational_descriptor->descriptor_id) {
      *detail = "streaming heap scan relational binding is not bijective";
      return false;
    }
    const auto column = std::ranges::find_if(
        prepared.persisted.columns, [&](const auto& candidate) {
          return candidate.column_uuid.canonical ==
                 *expression->bound_name_uuid;
        });
    if (column == prepared.persisted.columns.end()) {
      *detail =
          "streaming heap projected column is absent from persisted descriptor";
      return false;
    }
    const bool nullable = relational_descriptor->nullability ==
                          api::RelationalNullability::kNullable;
    const auto expected_collation =
        relational_descriptor->collation_uuid.has_value()
            ? std::optional<std::string_view>{
                  *relational_descriptor->collation_uuid}
            : std::nullopt;
    const auto expected_timezone =
        relational_descriptor->timezone_profile_id.has_value()
            ? std::optional<std::string_view>{
                  *relational_descriptor->timezone_profile_id}
            : std::nullopt;
    const auto canonical_nullability = CanonicalDescriptorFieldEqualsForComposition(
        column->value_descriptor, "nullability",
        std::optional<std::string_view>{nullable ? "nullable" : "non_null"});
    const auto storage_nullability = CanonicalDescriptorFieldEqualsForComposition(
        column->value_descriptor, "nullable",
        std::optional<std::string_view>{nullable ? "true" : "false"});
    const auto canonical_nullability_absent =
        CanonicalDescriptorFieldEqualsForComposition(column->value_descriptor,
                                         "nullability", std::nullopt);
    const auto storage_nullability_absent =
        CanonicalDescriptorFieldEqualsForComposition(column->value_descriptor, "nullable",
                                         std::nullopt);
    const bool exact_nullability_carrier =
        (canonical_nullability && storage_nullability_absent) ||
        (canonical_nullability_absent && storage_nullability) ||
        (canonical_nullability && storage_nullability);
    if (column->ordinal >= prepared.persisted.columns.size() ||
        output->output_name_utf8 != column->canonical_name_key ||
        column->value_descriptor.descriptor_uuid.canonical !=
            relational_descriptor->descriptor_uuid ||
        column->value_descriptor.canonical_type_name.empty() ||
        column->nullable != nullable ||
        !CanonicalDescriptorFieldEqualsForComposition(
            column->value_descriptor, "type_uuid",
            std::optional<std::string_view>{relational_descriptor->type_uuid}) ||
        !exact_nullability_carrier ||
        !CanonicalDescriptorFieldEqualsForComposition(column->value_descriptor,
                                           "collation_uuid",
                                           expected_collation) ||
        !CanonicalDescriptorFieldEqualsForComposition(column->value_descriptor,
                                           "timezone_profile_id",
                                           expected_timezone) ||
        (relational_descriptor->collation_uuid.has_value()
             ? column->collation_uuid != *relational_descriptor->collation_uuid
             : !column->collation_uuid.empty())) {
      *detail = "streaming heap persisted descriptor differs from binding";
      return false;
    }
    prepared.columns.push_back(&*column);
    prepared.descriptors.push_back(column->value_descriptor);
    prepared.descriptors.back().descriptor_kind = "scalar";
  }
  *binding = std::move(prepared);
  return true;
}

bool MaterializeCurrentHeapStreamingRow(
    const CurrentHeapStreamingScanBinding& binding,
    const CurrentHeapStreamingCompactRow& compact,
    std::vector<api::EngineTypedValue>* values) {
  if (values == nullptr || compact.values.size() != binding.descriptors.size() ||
      compact.nulls.size() != binding.descriptors.size()) {
    return false;
  }
  values->clear();
  values->reserve(binding.descriptors.size());
  for (std::size_t ordinal = 0; ordinal < binding.descriptors.size(); ++ordinal) {
    api::EngineTypedValue value;
    value.descriptor = binding.descriptors[ordinal];
    if (compact.nulls[ordinal] != 0) {
      value.is_null = true;
      value.state = api::EngineValueState::sql_null;
    } else {
      value.encoded_value = compact.values[ordinal];
      value.state = api::EngineValueState::value;
    }
    values->push_back(std::move(value));
  }
  return true;
}

}  // namespace

CanonicalObjectFreeValuesExecutionResult ExecuteCanonicalCurrentHeapSingleSourceQuery(
    const CanonicalCurrentHeapExecutionRequest& input) {
  CanonicalObjectFreeValuesExecutionResult result;
  const auto& dag = input.relational_dag;
  const auto scan_node = std::ranges::find_if(
      dag.nodes, [](const auto& node) {
        return node.node_kind == api::RelationalDagNodeKind::kScan;
      });
  const auto filter_node = std::ranges::find_if(
      dag.nodes, [](const auto& node) {
        return node.node_kind == api::RelationalDagNodeKind::kFilter;
      });
  const auto project_node = std::ranges::find_if(
      dag.nodes, [](const auto& node) {
        return node.node_kind == api::RelationalDagNodeKind::kProject;
      });
  const auto sort_node = std::ranges::find_if(
      dag.nodes, [](const auto& node) {
        return node.node_kind == api::RelationalDagNodeKind::kSort;
      });
  const auto window_node = std::ranges::find_if(
      dag.nodes, [](const auto& node) {
        return node.node_kind == api::RelationalDagNodeKind::kWindow;
      });
  const auto aggregate_node = std::ranges::find_if(
      dag.nodes, [](const auto& node) {
        return node.node_kind == api::RelationalDagNodeKind::kAggregate;
      });
  const auto cte_node = std::ranges::find_if(
      dag.nodes, [](const auto& node) {
        return node.node_kind == api::RelationalDagNodeKind::kCte;
      });
  const auto limit_node = std::ranges::find_if(
      dag.nodes, [](const auto& node) {
        return node.node_kind == api::RelationalDagNodeKind::kLimit;
      });
  const bool filter_composition = filter_node != dag.nodes.end();
  const bool project_composition = project_node != dag.nodes.end();
  const bool sort_composition = sort_node != dag.nodes.end();
  const bool window_composition = window_node != dag.nodes.end();
  const bool aggregate_composition = aggregate_node != dag.nodes.end();
  const bool cte_composition = cte_node != dag.nodes.end();
  const bool limit_composition = limit_node != dag.nodes.end();
  const auto expected_terminal =
      limit_composition
          ? limit_node->node_id
          : (aggregate_composition
                 ? aggregate_node->node_id
                 : (project_composition
                 ? project_node->node_id
                 : (window_composition
                        ? window_node->node_id
                        : (sort_composition
                               ? sort_node->node_id
                               : (filter_composition
                                      ? filter_node->node_id
                                      : (scan_node == dag.nodes.end()
                                             ? 0
                                             : scan_node->node_id))))));
  const auto direct_or_cte_input =
      [&](const api::RelationalDagNode* consumer,
          const std::uint32_t producer_node_id) {
        if (consumer == nullptr) return false;
        if (consumer->input_node_ids ==
            std::vector<std::uint32_t>{producer_node_id}) {
          return true;
        }
        return cte_composition &&
               consumer->input_node_ids ==
                   std::vector<std::uint32_t>{cte_node->node_id} &&
               cte_node->input_node_ids ==
                   std::vector<std::uint32_t>{producer_node_id};
      };
  const auto expected_project_input =
      window_composition
          ? window_node->node_id
          : (sort_composition
                 ? sort_node->node_id
                 : (filter_composition
                        ? filter_node->node_id
                        : (scan_node == dag.nodes.end() ? 0
                                                        : scan_node->node_id)));
  const auto expected_limit_input =
      aggregate_composition
          ? aggregate_node->node_id
          : (project_composition
          ? project_node->node_id
          : (sort_composition
                 ? sort_node->node_id
                 : (filter_composition
                        ? filter_node->node_id
                        : (scan_node == dag.nodes.end() ? 0
                                                       : scan_node->node_id))));
  const auto cte_input_node =
      !cte_composition || cte_node->input_node_ids.size() != 1
          ? dag.nodes.end()
          : std::ranges::find_if(dag.nodes, [&](const auto& candidate) {
              return candidate.node_id == cte_node->input_node_ids.front();
            });
  const bool cte_is_root =
      cte_composition && dag.root_node_id == cte_node->node_id &&
      cte_node->input_node_ids ==
          std::vector<std::uint32_t>{expected_terminal};
  const auto cte_consumer_count =
      cte_composition
          ? static_cast<std::size_t>(std::ranges::count_if(
                dag.nodes, [&](const auto& candidate) {
                  return candidate.node_id != cte_node->node_id &&
                         candidate.input_node_ids ==
                             std::vector<std::uint32_t>{cte_node->node_id};
                }))
          : std::size_t{0};
  const bool exact_composition =
      scan_node != dag.nodes.end() &&
      dag.nodes.size() ==
          1 + static_cast<std::size_t>(filter_composition) +
              static_cast<std::size_t>(sort_composition) +
              static_cast<std::size_t>(window_composition) +
              static_cast<std::size_t>(project_composition) +
              static_cast<std::size_t>(aggregate_composition) +
              static_cast<std::size_t>(cte_composition) +
              static_cast<std::size_t>(limit_composition) &&
      (dag.root_node_id == expected_terminal || cte_is_root) &&
      (!cte_composition ||
       (cte_input_node != dag.nodes.end() &&
        cte_node->semantic_variant_id == "cte.bound.v1" &&
        cte_node->output_descriptor_ids ==
            cte_input_node->output_descriptor_ids &&
        cte_node->bound_expression_ids.empty() &&
        cte_node->required_object_uuids.empty() &&
        cte_node->values_row_ids.empty() &&
        cte_node->required_property_uuids.empty() &&
        cte_node->delivered_property_uuids.empty() &&
        (cte_is_root ? cte_consumer_count == 0
                     : cte_consumer_count == 1))) &&
      (!filter_composition ||
       direct_or_cte_input(&*filter_node, scan_node->node_id)) &&
      (!project_composition ||
       direct_or_cte_input(&*project_node, expected_project_input)) &&
      (!sort_composition ||
       direct_or_cte_input(
           &*sort_node, filter_composition ? filter_node->node_id
                                           : scan_node->node_id)) &&
      (!window_composition ||
       (sort_composition &&
        window_node->input_node_ids ==
            std::vector<std::uint32_t>{sort_node->node_id} &&
        !aggregate_composition && !limit_composition)) &&
      (!aggregate_composition ||
       (direct_or_cte_input(
            &*aggregate_node,
            filter_composition ? filter_node->node_id : scan_node->node_id) &&
        !project_composition && !sort_composition)) &&
      (!limit_composition ||
       direct_or_cte_input(&*limit_node, expected_limit_input));
  if (dag.wire_version != 2 ||
      !exact_composition ||
      scan_node->semantic_variant_id != "relation.source.v1" ||
      !scan_node->input_node_ids.empty() ||
      scan_node->required_object_uuids.size() != 1) {
    return result;
  }
  result.profile_matched = true;

  CanonicalObjectFreeValuesExecutionRequest response_context;
  response_context.context = input.context;
  response_context.relational_dag = input.relational_dag;
  const auto refuse = [&](std::string diagnostic_id, std::string detail) {
    result.optimizer_selected = false;
    result.physical_dag_published = false;
    result.physical_dag_executed = false;
    result.runtime_actuals_attached = false;
    result.canonical_result_published = false;
    result.physical_node_count = 0;
    result.canonical_result_column_count = 0;
    result.canonical_result_row_count = 0;
    result.selected_plan_uuid.clear();
    result.canonical_result_bytes.clear();
    result.api_result = Failure(response_context, std::move(diagnostic_id),
                                std::move(detail));
    return result;
  };

  const auto admission = api::BuildCanonicalCurrentHeapOptimizerAdmission(
      {input.context, input.relational_dag});
  if (!admission.built || !admission.admission.admitted ||
      !admission.admission.planning_allowed ||
      admission.admission.data_access_allowed) {
    return refuse(
        admission.issue.diagnostic_id.empty()
            ? "QOW-DIAG-PACKET7-OBJECT-HEAP-ADMISSION-V1"
            : admission.issue.diagnostic_id,
        admission.issue.field_id.empty()
            ? "current object-backed heap optimizer admission failed"
            : admission.issue.field_id);
  }
  result.optimizer_admitted = true;
  result.optimizer_admission_degraded =
      admission.admission.degraded_for_unknown_statistics;
  result.optimizer_benchmark_clean_ready =
      admission.admission.benchmark_clean_ready;
  result.optimizer_admission_stage_count =
      admission.admission.evidence.size();

  CanonicalObjectFreeValuesExecutionRequest planning_request{
      input.context, input.relational_dag, admission.request,
      admission.admission};
  const auto& graph = admission.request.logical_graph;
  const auto identity_scope =
      graph.bound_sblr_tree_uuid + ":" + input.context.statement_uuid.canonical;
  LivePhysicalNodeProfile scan_profile;
  scan_profile.logical_node_id = scan_node->node_id;
  scan_profile.implementation_id = "scan.heap.v1";
  scan_profile.capability_uuid =
      DerivedCanonicalUuid(identity_scope, "heap-scan.capability");
  scan_profile.logical_node_kind =
      plan::CanonicalLogicalRelationalNodeKind::kRelationSource;
  scan_profile.physical_node_kind = exec::PhysicalNodeKind::kScan;
  scan_profile.transformation_rule_id = "canonical.heap.scan.v1";
  scan_profile.estimated_rows = 1;
  scan_profile.memory_bytes_required =
      planning_request.optimizer_request.resource.memory_budget_bytes;
  scan_profile.minimum_input_count = 0;
  scan_profile.maximum_input_count = 0;
  scan_profile.page_read_sequential_units = 1;
  scan_profile.mga_visibility_checks_expected = 1;
  scan_profile.storage_read_capable = true;
  scan_profile.mga_visibility_capable = true;
  std::vector<LivePhysicalNodeProfile> profiles;
  profiles.push_back(std::move(scan_profile));
  CanonicalRelationalExpressionRowBinding filter_row_binding;
  std::string filter_capability_uuid;
  if (filter_composition) {
    std::string detail;
    if (filter_node->semantic_variant_id !=
            "filter.catalog-column-numeric-comparison.v1" ||
        filter_node->bound_expression_ids.size() != 1 ||
        filter_node->output_descriptor_ids !=
            scan_node->output_descriptor_ids ||
        !PrepareInputRowBindingForComposition(
            input.relational_dag, filter_node->bound_expression_ids.front(),
            scan_node->output_descriptor_ids, &filter_row_binding, &detail)) {
      return refuse(
          "QOW-DIAG-PACKET7-OBJECT-HEAP-FILTER-V1",
          detail.empty() ? "object-backed WHERE binding is not exact"
                         : detail);
    }
    filter_capability_uuid =
        DerivedCanonicalUuid(identity_scope, "heap-filter.capability");
    profiles.push_back(
        {filter_node->node_id, "filter.3vl.row.v1",
         filter_capability_uuid,
         plan::CanonicalLogicalRelationalNodeKind::kFilter,
         exec::PhysicalNodeKind::kFilter,
         "canonical.heap.filter.catalog-column-numeric-comparison.v1",
         1, planning_request.optimizer_request.resource.memory_budget_bytes,
         1, 1});
  }
  PreparedGlobalAggregateRoot prepared_heap_aggregate;
  std::string aggregate_capability_uuid;
  const bool grouped_sum_int64_int128_profile =
      aggregate_composition && !filter_composition && !project_composition &&
      !sort_composition && !window_composition && !cte_composition &&
      !limit_composition &&
      aggregate_node->semantic_variant_id ==
          "aggregate.grouped-int64-key-sum.v1";
  if (aggregate_composition) {
    if (grouped_sum_int64_int128_profile) {
      constexpr std::string_view kBigintDescriptorUuid =
          "019d0000-0000-7000-8000-00000000d711";
      constexpr std::string_view kBigintTypeUuid =
          "019d0000-0000-7000-8000-00000000d712";
      constexpr std::string_view kInt128DescriptorUuid =
          "019d0000-0000-7000-8000-00000000d714";
      constexpr std::string_view kInt128TypeUuid =
          "019d0000-0000-7000-8000-00000000d715";
      constexpr std::string_view kSumFunctionUuid =
          "019de5fc-2400-72e4-8549-82b2eef5a777";
      const auto descriptor_for = [&](const std::uint32_t id) {
        const auto found = std::ranges::find_if(
            dag.descriptors, [&](const auto& descriptor) {
              return descriptor.descriptor_id == id;
            });
        return found == dag.descriptors.end() ? nullptr : &*found;
      };
      const auto expression_for = [&](const std::uint32_t id) {
        const auto found = std::ranges::find_if(
            dag.expressions, [&](const auto& expression) {
              return expression.expression_id == id;
            });
        return found == dag.expressions.end() ? nullptr : &*found;
      };
      const auto* key_descriptor =
          scan_node->output_descriptor_ids.size() == 2
              ? descriptor_for(scan_node->output_descriptor_ids[0])
              : nullptr;
      const auto* value_descriptor =
          scan_node->output_descriptor_ids.size() == 2
              ? descriptor_for(scan_node->output_descriptor_ids[1])
              : nullptr;
      const auto* result_descriptor =
          aggregate_node->output_descriptor_ids.size() == 2
              ? descriptor_for(aggregate_node->output_descriptor_ids[1])
              : nullptr;
      const auto* key_expression =
          scan_node->bound_expression_ids.size() == 2
              ? expression_for(scan_node->bound_expression_ids[0])
              : nullptr;
      const auto* value_expression =
          scan_node->bound_expression_ids.size() == 2
              ? expression_for(scan_node->bound_expression_ids[1])
              : nullptr;
      const auto* sum_expression =
          aggregate_node->bound_expression_ids.size() == 2
              ? expression_for(aggregate_node->bound_expression_ids[1])
              : nullptr;
      const auto exact_identity = [&](const api::RelationalTypeDescriptor* d,
                                      const std::string_view descriptor_uuid,
                                      const std::string_view type_uuid,
                                      const std::string_view codec_id,
                                      const api::RelationalNullability nullability) {
        if (d == nullptr || !d->datatype_identity_authoritative ||
            !CanonicalUuidText(d->descriptor_uuid) ||
            d->descriptor_generation != 1 || d->type_uuid != type_uuid ||
            d->type_generation != 1 || d->codec_id != codec_id ||
            d->codec_version != 1 || d->codec_generation != 1 ||
            d->nullability != nullability || d->collation_uuid.has_value() ||
            d->timezone_profile_id.has_value() || d->width.has_value() ||
            d->precision.has_value() || d->scale.has_value() ||
            d->statement_receipt_uuid !=
                input.context.statement_receipt_uuid.canonical ||
            d->datatype_catalog_snapshot_uuid !=
                input.context.datatype_catalog_snapshot_uuid.canonical ||
            d->datatype_catalog_generation !=
                input.context.datatype_catalog_generation ||
            d->datatype_registry_generation !=
                input.context.datatype_registry_generation) {
          return false;
        }
        const auto identity = dt::LookupDatatypeTypeCodecIdentityV1(
            d->datatype_catalog_snapshot_uuid,
            d->datatype_catalog_generation,
            d->datatype_registry_generation, std::string(descriptor_uuid),
            d->descriptor_generation);
        return identity.ok && identity.row.type_uuid == d->type_uuid &&
               identity.row.type_generation == d->type_generation &&
               identity.row.codec_id == d->codec_id &&
               identity.row.codec_version == d->codec_version &&
               identity.row.codec_generation == d->codec_generation;
      };
      const bool key_nullable =
          key_descriptor != nullptr &&
          key_descriptor->nullability == api::RelationalNullability::kNullable;
      const bool value_nullable =
          value_descriptor != nullptr &&
          value_descriptor->nullability ==
              api::RelationalNullability::kNullable;
      if (scan_node->output_descriptor_ids.size() != 2 ||
          scan_node->bound_expression_ids.size() != 2 ||
          aggregate_node->input_node_ids !=
              std::vector<std::uint32_t>{scan_node->node_id} ||
          aggregate_node->output_descriptor_ids.size() != 2 ||
          aggregate_node->output_descriptor_ids[0] !=
              scan_node->output_descriptor_ids[0] ||
          aggregate_node->bound_expression_ids.size() != 2 ||
          aggregate_node->bound_expression_ids[0] !=
              scan_node->bound_expression_ids[0] ||
          key_expression == nullptr || value_expression == nullptr ||
          sum_expression == nullptr ||
          key_expression->expression_kind !=
              api::RelationalExpressionKind::kIdentifier ||
          value_expression->expression_kind !=
              api::RelationalExpressionKind::kIdentifier ||
          sum_expression->expression_kind !=
              api::RelationalExpressionKind::kFunctionCall ||
          sum_expression->function_uuid != kSumFunctionUuid ||
          sum_expression->child_expression_ids !=
              std::vector<std::uint32_t>{value_expression->expression_id} ||
          result_descriptor == nullptr ||
          sum_expression->result_descriptor_id !=
              result_descriptor->descriptor_id ||
          !dag.grouping_sets.empty() ||
          !exact_identity(key_descriptor, kBigintDescriptorUuid,
                          kBigintTypeUuid, "datatype.int64.le.v1",
                          key_nullable ? api::RelationalNullability::kNullable
                                       : api::RelationalNullability::kNonNull) ||
          !exact_identity(value_descriptor, kBigintDescriptorUuid,
                          kBigintTypeUuid, "datatype.int64.le.v1",
                          value_nullable
                              ? api::RelationalNullability::kNullable
                              : api::RelationalNullability::kNonNull) ||
          !exact_identity(result_descriptor, kInt128DescriptorUuid,
                          kInt128TypeUuid, "datatype.int128.le.v1",
                          api::RelationalNullability::kNullable)) {
        return refuse(
            "DATATYPE.DESCRIPTOR_INVALID",
            "catalog grouped SUM exact DAG or live datatype identity is invalid");
      }
      aggregate_capability_uuid =
          DerivedCanonicalUuid(identity_scope, "heap-grouped-sum.capability");
      profiles.push_back(
          {aggregate_node->node_id,
           "aggregate.grouped-int64-key-sum-int128.streaming.v1",
           aggregate_capability_uuid,
           plan::CanonicalLogicalRelationalNodeKind::kAggregate,
           exec::PhysicalNodeKind::kAggregate,
           "canonical.heap.aggregate.grouped-int64-key-sum-int128.v1", 1,
           planning_request.optimizer_request.resource.memory_budget_bytes,
           1, 1});
    } else {
    const auto aggregate_profile =
        MatchLiveUnaryAggregateExpressionProfileForComposition(
            aggregate_node->semantic_variant_id);
    const auto logical_aggregate = std::ranges::find_if(
        graph.nodes, [&](const auto& candidate) {
          return candidate.logical_node_id == aggregate_node->node_id;
        });
    const auto aggregate_input_node =
        filter_composition ? &*filter_node : &*scan_node;
    const auto logical_input = std::ranges::find_if(
        graph.nodes, [&](const auto& candidate) {
          return candidate.logical_node_id == aggregate_input_node->node_id;
        });
    MaterializedValues planning_input;
    std::vector<const api::RelationalOutputRecord*> input_outputs;
    for (const auto& output : dag.outputs) {
      if (output.relation_node_id == aggregate_input_node->node_id) {
        input_outputs.push_back(&output);
      }
    }
    std::ranges::sort(input_outputs, {},
                      &api::RelationalOutputRecord::ordinal);
    if (!aggregate_profile.matched ||
        aggregate_profile.distinct || aggregate_profile.has_filter ||
        logical_aggregate == graph.nodes.end() ||
        logical_input == graph.nodes.end() ||
        input_outputs.size() !=
            aggregate_input_node->output_descriptor_ids.size() ||
        admission.current_relation_projection_type_names.size() !=
            input_outputs.size() ||
        admission.current_relation_projection_descriptors.size() !=
            input_outputs.size()) {
      return refuse("QOW-DIAG-PACKET7-OBJECT-HEAP-AGGREGATE-V1",
                    "object-backed global aggregate profile is not exact");
    }
    for (std::size_t ordinal = 0; ordinal < input_outputs.size(); ++ordinal) {
      const auto descriptor = std::ranges::find_if(
          dag.descriptors, [&](const auto& candidate) {
            return candidate.descriptor_id ==
                   aggregate_input_node->output_descriptor_ids[ordinal];
          });
      if (descriptor == dag.descriptors.end() ||
          input_outputs[ordinal]->ordinal != ordinal ||
          input_outputs[ordinal]->descriptor_id != descriptor->descriptor_id) {
        return refuse("QOW-DIAG-PACKET7-OBJECT-HEAP-AGGREGATE-V1",
                      "object-backed aggregate input descriptor is unresolved");
      }
      const auto persisted_type_name =
          admission.current_relation_projection_type_names[ordinal];
      const auto& engine_descriptor =
          admission.current_relation_projection_descriptors[ordinal];
      if (engine_descriptor.descriptor_uuid.canonical !=
              descriptor->descriptor_uuid ||
          engine_descriptor.descriptor_kind != "scalar" ||
          engine_descriptor.canonical_type_name != persisted_type_name ||
          !api::QowCanonicalDescriptorIdentityV1(engine_descriptor)) {
        return refuse(
            "QOW-DIAG-PACKET7-OBJECT-HEAP-AGGREGATE-V1",
            "object-backed aggregate persisted input descriptor drifted");
      }
      planning_input.batch.columns.push_back(
          {input_outputs[ordinal]->output_name_utf8, engine_descriptor,
           descriptor->nullability == api::RelationalNullability::kNullable,
           descriptor->descriptor_id});
      planning_input.result_bindings.push_back({ordinal, false, std::nullopt});
    }
    planning_input.ok = true;
    prepared_heap_aggregate = PrepareGlobalAggregateRootForComposition(
        dag, *logical_aggregate, *logical_input, planning_input,
        aggregate_profile.function, aggregate_profile.count_star,
        aggregate_profile.distinct, aggregate_profile.has_filter);
    if (!prepared_heap_aggregate.ok) {
      return refuse(
          "QOW-DIAG-PACKET7-OBJECT-HEAP-AGGREGATE-V1",
          prepared_heap_aggregate.detail.empty()
              ? "object-backed global aggregate binding is unresolved"
              : prepared_heap_aggregate.detail);
    }
    aggregate_capability_uuid =
        DerivedCanonicalUuid(identity_scope, "heap-aggregate.capability");
    profiles.push_back(
        {aggregate_node->node_id,
         aggregate_profile.count_star ? "aggregate.count-star.v1"
                                      : "aggregate.registry-core.v1",
         aggregate_capability_uuid,
         plan::CanonicalLogicalRelationalNodeKind::kAggregate,
         exec::PhysicalNodeKind::kAggregate,
         aggregate_profile.transformation_id, 1,
         planning_request.optimizer_request.resource.memory_budget_bytes,
         1, 1});
    }
  }
  std::vector<plan::CanonicalLogicalPropertyOrderingTerm> heap_order_terms;
  std::vector<exec::CanonicalDescriptorOrderTerm> heap_descriptor_order_terms;
  std::string sort_capability_uuid;
  std::string ordering_property_uuid;
  PreparedSortRoot heap_sort_binding;
  if (sort_composition) {
    const auto& properties = admission.request.logical_properties.properties;
    const auto property = std::ranges::find_if(
        properties, [&](const auto& candidate) {
          return sort_node->required_property_uuids.size() == 1 &&
                 candidate.property_uuid ==
                     sort_node->required_property_uuids.front();
        });
    if (sort_node->semantic_variant_id != "sort.required-order.v1" ||
        sort_node->bound_expression_ids.empty() ||
        sort_node->output_descriptor_ids != scan_node->output_descriptor_ids ||
        property == properties.end() ||
        property->property_kind !=
            plan::CanonicalLogicalPropertyKind::kOrdering ||
        property->origin_logical_node_id != sort_node->node_id ||
        property->ordering_terms.size() !=
            sort_node->bound_expression_ids.size() ||
        admission.current_relation_projection_descriptors.size() !=
            scan_node->output_descriptor_ids.size()) {
      return refuse("QOW-DIAG-PACKET7-OBJECT-HEAP-SORT-V1",
                    "object-backed ORDER BY property is not exact");
    }
    for (const auto& term : property->ordering_terms) {
      const auto expression = std::ranges::find_if(
          dag.expressions, [&](const auto& candidate) {
            return candidate.expression_id == term.expression_id;
          });
      if (expression == dag.expressions.end() ||
          std::ranges::find(sort_node->bound_expression_ids,
                            term.expression_id) ==
              sort_node->bound_expression_ids.end()) {
        return refuse("QOW-DIAG-PACKET7-OBJECT-HEAP-SORT-V1",
                      "object-backed ORDER BY expression is unbound");
      }
      const auto descriptor = std::ranges::find(
          scan_node->output_descriptor_ids,
          expression->result_descriptor_id);
      if (descriptor == scan_node->output_descriptor_ids.end()) {
        return refuse("QOW-DIAG-PACKET7-OBJECT-HEAP-SORT-V1",
                      "object-backed ORDER BY descriptor is not in the source width");
      }
      const auto source_column = static_cast<std::size_t>(std::distance(
          scan_node->output_descriptor_ids.begin(), descriptor));
      const auto source_output = std::ranges::find_if(
          dag.outputs, [&](const auto& candidate) {
            return candidate.relation_node_id == scan_node->node_id &&
                   candidate.ordinal == source_column &&
                   candidate.descriptor_id == expression->result_descriptor_id;
          });
      const auto relational_descriptor = std::ranges::find_if(
          dag.descriptors, [&](const auto& candidate) {
            return candidate.descriptor_id == expression->result_descriptor_id;
          });
      if (source_output == dag.outputs.end() ||
          relational_descriptor == dag.descriptors.end()) {
        return refuse("QOW-DIAG-PACKET7-OBJECT-HEAP-SORT-V1",
                      "object-backed ORDER BY source descriptor is unresolved");
      }
      const auto& persisted_descriptor =
          admission.current_relation_projection_descriptors[source_column];
      exec::ExecutorColumnDescriptor executor_column{
          source_output->output_name_utf8, persisted_descriptor,
          relational_descriptor->nullability ==
              api::RelationalNullability::kNullable,
          relational_descriptor->descriptor_id};
      exec::CanonicalDescriptorOrderTerm descriptor_term;
      std::string binding_detail;
      if (!PrepareCanonicalSortOrderTermForComposition(
              input.context, term, executor_column, source_column,
              &descriptor_term, &binding_detail)) {
        return refuse(
            "QOW-DIAG-PACKET7-OBJECT-HEAP-SORT-BINDING-V1",
            binding_detail.empty()
                ? "object-backed ORDER BY descriptor binding is unresolved"
                : binding_detail);
      }
      heap_order_terms.push_back(term);
      heap_descriptor_order_terms.push_back(std::move(descriptor_term));
    }
    ordering_property_uuid = property->property_uuid;
    heap_sort_binding.ok = true;
    heap_sort_binding.ordering_property_uuid = ordering_property_uuid;
    heap_sort_binding.order_terms = heap_descriptor_order_terms;
    sort_capability_uuid =
        DerivedCanonicalUuid(identity_scope, "heap-sort.capability");
    profiles.push_back(
        {sort_node->node_id, "sort.typed.terms.v1", sort_capability_uuid,
         plan::CanonicalLogicalRelationalNodeKind::kSort,
         exec::PhysicalNodeKind::kSort,
         "canonical.heap.sort.required-order.v1", 1,
         planning_request.optimizer_request.resource.memory_budget_bytes,
         1, 1, {},
         {ordering_property_uuid},
         {plan::CanonicalLogicalPropertyKind::kOrdering}});
  }
  std::optional<exec::ExecutorColumnDescriptor> prepared_heap_row_number;
  std::optional<exec::ExecutorColumnDescriptor> prepared_heap_ntile;
  std::optional<exec::CanonicalDescriptorOrderTerm>
      prepared_heap_ntile_order_term;
  std::optional<api::EngineTypedValue>
      prepared_heap_ntile_bucket_count_operand;
  std::optional<exec::ExecutorColumnDescriptor> prepared_heap_peer_ranking;
  std::optional<exec::CanonicalDescriptorOrderTerm>
      prepared_heap_peer_ranking_order_term;
  GlobalRankingWindowProfile prepared_heap_peer_ranking_profile;
  std::optional<exec::ExecutorColumnDescriptor>
      prepared_heap_navigation_window;
  std::optional<exec::CanonicalDescriptorOrderTerm>
      prepared_heap_navigation_order_term;
  std::optional<std::size_t> prepared_heap_navigation_value_column;
  std::optional<api::EngineTypedValue>
      prepared_heap_navigation_nth_value_position_operand;
  std::string prepared_heap_navigation_frame_descriptor_uuid;
  GlobalRankingWindowProfile prepared_heap_navigation_profile;
  std::string heap_navigation_order_term_binding_evidence_uuid;
  std::string heap_navigation_frame_property_binding_evidence_uuid;
  std::optional<exec::ExecutorColumnDescriptor>
      prepared_heap_aggregate_window;
  std::optional<exec::CanonicalDescriptorOrderTerm>
      prepared_heap_aggregate_window_order_term;
  std::optional<std::size_t> prepared_heap_aggregate_window_value_column;
  exec::CanonicalAggregateDescriptor
      prepared_heap_aggregate_window_descriptor;
  std::string prepared_heap_aggregate_window_frame_descriptor_uuid;
  std::string heap_aggregate_window_order_term_binding_evidence_uuid;
  std::string heap_aggregate_window_frame_property_binding_evidence_uuid;
  std::string heap_aggregate_window_capability_uuid;
  std::string window_capability_uuid;
  std::string heap_ntile_order_term_binding_evidence_uuid;
  std::string heap_peer_ranking_order_term_binding_evidence_uuid;
  std::string window_order_evidence_uuid;
  if (window_composition) {
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
    const bool navigation_window = lag_window || lead_window;
    const bool value_window =
        navigation_window || first_value_window || last_value_window ||
        nth_value_window || aggregate_window;
    const bool peer_ranking_window =
        rank_window || dense_rank_window || percent_rank_window ||
        cume_dist_window;
    auto ranking_profile =
        aggregate_window
            ? GlobalAggregateWindowProfileForComposition(dag,
                                                   window_node->node_id)
            : value_window
            ? (first_value_window
                   ? kGlobalFirstValueProfile
                   : (last_value_window
                          ? kGlobalLastValueProfile
                          : (nth_value_window
                                 ? kGlobalNthValueProfile
                                 : (lag_window ? kGlobalLagProfile
                                               : kGlobalLeadProfile))))
            : (ntile_window
            ? kGlobalNtileProfile
            : (cume_dist_window
            ? kGlobalCumeDistProfile
            : (percent_rank_window
                   ? kGlobalPercentRankProfile
                   : (dense_rank_window
                          ? kGlobalDenseRankProfile
                           : (rank_window ? kGlobalRankProfile
                                          : kGlobalRowNumberProfile)))));
    const bool aggregate_count_window =
        aggregate_window &&
        ranking_profile.builtin_id == "sb.aggregate.count";
    const bool aggregate_boolean_window =
        aggregate_window &&
        (ranking_profile.builtin_id == "sb.aggregate.bool_and" ||
         ranking_profile.builtin_id == "sb.aggregate.bool_or" ||
         ranking_profile.builtin_id == "sb.aggregate.every");
    const bool aggregate_bounded_signed_window =
        aggregate_window && !aggregate_count_window &&
        !aggregate_boolean_window;
    const std::string_view ranking_name = ranking_profile.display_name;
    const auto logical_window = std::ranges::find_if(
        graph.nodes, [&](const auto& candidate) {
          return candidate.logical_node_id == window_node->node_id;
        });
    const auto logical_sort = std::ranges::find_if(
        graph.nodes, [&](const auto& candidate) {
          return candidate.logical_node_id == sort_node->node_id;
        });
    const auto core_manifest =
        dt::LoadCurrentCoreDatatypeCatalogManifest();
    if (logical_window == graph.nodes.end() ||
        logical_sort == graph.nodes.end() || !core_manifest.ok()) {
      return refuse("QOW-DIAG-PACKET7-OBJECT-HEAP-WINDOW-V1",
                    "object-backed " + std::string(ranking_name) +
                        " authority is unavailable");
    }
    const auto order_type_uuid =
        ExactCanonicalCoreDatatypeTypeUuidV1("int64");
    const auto boolean_type_uuid =
        ExactCanonicalCoreDatatypeTypeUuidV1("boolean");
    if (!aggregate_window &&
        DirectValueWindowUsesExactTypeForComposition(
            dag, window_node->node_id, ranking_profile.builtin_id,
            boolean_type_uuid)) {
      ranking_profile.result_type_name = "boolean";
    }
    const auto result_type_uuid = ExactCanonicalCoreDatatypeTypeUuidV1(
        ranking_profile.result_type_name);
    const std::array<std::string, 4> bounded_signed_type_uuids = {
        ExactCanonicalCoreDatatypeTypeUuidV1("int8"),
        ExactCanonicalCoreDatatypeTypeUuidV1("int16"),
        ExactCanonicalCoreDatatypeTypeUuidV1("int32"),
        order_type_uuid};
    const auto ranking = PrepareGlobalRankingWindowBindingForComposition(
        dag, admission.request.logical_properties, *window_node,
        *logical_window, *logical_sort, heap_sort_binding,
        sort_node->output_descriptor_ids.size(),
        sort_node->output_descriptor_ids.size(), result_type_uuid,
        order_type_uuid, boolean_type_uuid, bounded_signed_type_uuids, "heap",
        ranking_profile, project_composition);
    if (!ranking.ok) {
      return refuse(ranking.diagnostic_id, ranking.detail);
    }
    if (ntile_window && !ranking.ntile_bucket_count_operand.has_value()) {
      return refuse("QOW-DIAG-PACKET7-OBJECT-HEAP-WINDOW-V1",
                    "object-backed NTILE bucket operand is unresolved");
    }
    if (value_window && !ranking.aggregate_count_star &&
        !ranking.navigation_value_column.has_value()) {
      return refuse("QOW-DIAG-PACKET7-OBJECT-HEAP-WINDOW-V1",
                    "object-backed " + std::string(ranking_name) +
                        " value column is unresolved");
    }
    if (nth_value_window &&
        !ranking.nth_value_position_operand.has_value()) {
      return refuse("QOW-DIAG-PACKET7-OBJECT-HEAP-WINDOW-V1",
                    "object-backed NTH_VALUE position is unresolved");
    }
    const auto* output_descriptor = ranking.result_descriptor;
    api::EngineDescriptor descriptor;
    if (aggregate_window) {
      const auto order_column = heap_descriptor_order_terms.front().column;
      const auto order_relational_descriptor = std::ranges::find_if(
          dag.descriptors, [&](const auto& candidate) {
            return order_column < sort_node->output_descriptor_ids.size() &&
                   candidate.descriptor_id ==
                       sort_node->output_descriptor_ids[order_column];
          });
      if (order_column >=
              admission.current_relation_projection_descriptors.size() ||
          order_relational_descriptor == dag.descriptors.end() ||
          !ExactCanonicalBoundedSignedWindowOrderForComposition(
              *order_relational_descriptor,
              admission.current_relation_projection_descriptors[order_column],
              order_relational_descriptor->nullability ==
                  api::RelationalNullability::kNullable,
              bounded_signed_type_uuids, ranking_profile.function_uuid,
              output_descriptor->descriptor_uuid,
              result_type_uuid,
              heap_sort_binding.ordering_property_uuid,
              ranking.window_property_uuid,
              ranking.window_frame_descriptor_uuid) ||
          (!ranking.aggregate_count_star &&
           (*ranking.navigation_value_column >=
                admission.current_relation_projection_descriptors.size() ||
            (order_column != *ranking.navigation_value_column &&
             admission.current_relation_projection_descriptors[order_column]
                     .descriptor_uuid.canonical ==
                 admission.current_relation_projection_descriptors
                     [*ranking.navigation_value_column]
                         .descriptor_uuid.canonical)))) {
        return refuse(
            "QOW-DIAG-PACKET7-OBJECT-HEAP-WINDOW-V1",
            "object-backed " + std::string(ranking_name) +
                " order key is not one exact core bounded-signed column");
      }
    }
    if (value_window && !ranking.aggregate_count_star) {
      if (*ranking.navigation_value_column >=
          admission.current_relation_projection_descriptors.size()) {
        return refuse("QOW-DIAG-PACKET7-OBJECT-HEAP-WINDOW-V1",
                      "object-backed " + std::string(ranking_name) +
                          " source descriptor is unresolved");
      }
      const auto& source_descriptor =
          admission.current_relation_projection_descriptors
              [*ranking.navigation_value_column];
      const auto source_relational_descriptor = std::ranges::find_if(
          dag.descriptors, [&](const auto& candidate) {
            return candidate.descriptor_id ==
                   sort_node->output_descriptor_ids
                       [*ranking.navigation_value_column];
          });
      if (source_relational_descriptor == dag.descriptors.end()) {
        return refuse("QOW-DIAG-PACKET7-OBJECT-HEAP-WINDOW-V1",
                      "object-backed " + std::string(ranking_name) +
                          " source relational descriptor is unresolved");
      }
      if (aggregate_count_window || !aggregate_window) {
        const auto order_column = heap_descriptor_order_terms.front().column;
        const auto order_relational_descriptor = std::ranges::find_if(
            dag.descriptors, [&](const auto& candidate) {
              return order_column < sort_node->output_descriptor_ids.size() &&
                     candidate.descriptor_id ==
                         sort_node->output_descriptor_ids[order_column];
            });
        if (order_column >=
                admission.current_relation_projection_descriptors.size() ||
            order_relational_descriptor == dag.descriptors.end() ||
            !ExactCanonicalScalarWindowOperandForComposition(
                *source_relational_descriptor, source_descriptor,
                source_relational_descriptor->nullability ==
                    api::RelationalNullability::kNullable,
                ranking_profile.function_uuid,
                output_descriptor->descriptor_uuid, result_type_uuid,
                admission.current_relation_projection_descriptors[order_column]
                    .descriptor_uuid.canonical,
                order_relational_descriptor->type_uuid,
                *ranking.navigation_value_column == order_column,
                heap_sort_binding.ordering_property_uuid,
                ranking.window_property_uuid,
                ranking.window_frame_descriptor_uuid) ||
            (!aggregate_window &&
             !ExactCanonicalScalarWindowOperandForComposition(
                 *order_relational_descriptor,
                 admission.current_relation_projection_descriptors
                     [order_column],
                 order_relational_descriptor->nullability ==
                     api::RelationalNullability::kNullable,
                 ranking_profile.function_uuid,
                 output_descriptor->descriptor_uuid, result_type_uuid,
                 source_descriptor.descriptor_uuid.canonical,
                 source_relational_descriptor->type_uuid,
                 *ranking.navigation_value_column == order_column,
                 heap_sort_binding.ordering_property_uuid,
                 ranking.window_property_uuid,
                 ranking.window_frame_descriptor_uuid))) {
          return refuse(
              "QOW-DIAG-PACKET7-OBJECT-HEAP-WINDOW-V1",
              "object-backed " + std::string(ranking_name) +
                  " source/order descriptors are not exact");
        }
      }
      if ((aggregate_boolean_window || aggregate_bounded_signed_window) &&
          (aggregate_boolean_window
               ? !ExactCanonicalBooleanWindowSourceForComposition(
                     *source_relational_descriptor, source_descriptor,
                     source_relational_descriptor->nullability ==
                         api::RelationalNullability::kNullable,
                     boolean_type_uuid, ranking_profile.function_uuid,
                     output_descriptor->descriptor_uuid,
                     heap_sort_binding.ordering_property_uuid,
                     ranking.window_property_uuid,
                     ranking.window_frame_descriptor_uuid)
               : !ExactCanonicalBoundedSignedWindowSourceForComposition(
                     *source_relational_descriptor, source_descriptor,
                     source_relational_descriptor->nullability ==
                         api::RelationalNullability::kNullable,
                     bounded_signed_type_uuids,
                     ranking_profile.function_uuid,
                     output_descriptor->descriptor_uuid,
                     heap_sort_binding.ordering_property_uuid,
                     ranking.window_property_uuid,
                     ranking.window_frame_descriptor_uuid))) {
        return refuse(
            "QOW-DIAG-PACKET7-OBJECT-HEAP-WINDOW-V1",
            "object-backed " + std::string(ranking_name) +
                " source is not one exact core " +
                (aggregate_boolean_window ? "boolean" : "bounded-signed") +
                " column");
      }
      if (aggregate_bounded_signed_window) {
        descriptor.descriptor_uuid.canonical =
            output_descriptor->descriptor_uuid;
        descriptor.descriptor_kind = "scalar";
        descriptor.canonical_type_name = "int64";
        descriptor.encoded_descriptor =
            "type_uuid=" + output_descriptor->type_uuid +
            ";nullability=nullable";
      } else if (!aggregate_count_window) {
        descriptor = source_descriptor;
        descriptor.descriptor_uuid.canonical =
            output_descriptor->descriptor_uuid;
        if (!exec::DeriveCanonicalNullableDescriptorEncoding(&descriptor) ||
            !exec::CanonicalDerivedDescriptorTypeMatches(
                source_descriptor,
                source_relational_descriptor->nullability ==
                    api::RelationalNullability::kNullable,
                descriptor, true) ||
            (aggregate_boolean_window &&
             descriptor.encoded_descriptor !=
                 "type_uuid=" + boolean_type_uuid +
                     ";nullability=nullable")) {
          return refuse(
              "QOW-DIAG-PACKET7-OBJECT-HEAP-WINDOW-V1",
              "object-backed " + std::string(ranking_name) +
                  " nullable result descriptor does not preserve "
              "its exact source shape");
        }
      } else {
        descriptor.descriptor_uuid.canonical =
            output_descriptor->descriptor_uuid;
        descriptor.descriptor_kind = "scalar";
        descriptor.canonical_type_name = "int64";
        descriptor.encoded_descriptor =
            "type_uuid=" + output_descriptor->type_uuid +
            ";nullability=non_null";
      }
    } else {
      descriptor.descriptor_uuid.canonical =
          output_descriptor->descriptor_uuid;
      descriptor.descriptor_kind = "scalar";
      descriptor.canonical_type_name =
          std::string(ranking_profile.result_type_name);
      descriptor.encoded_descriptor =
          "type_uuid=" + output_descriptor->type_uuid +
          ";nullability=non_null";
    }
    exec::ExecutorColumnDescriptor ranking_column{
        ranking.outputs.back()->output_name_utf8, descriptor,
        value_window && !aggregate_count_window,
        output_descriptor->descriptor_id};
    if (!api::QowCanonicalDescriptorIdentityV1(descriptor)) {
      return refuse("QOW-DIAG-PACKET7-OBJECT-HEAP-WINDOW-V1",
                    "object-backed " + std::string(ranking_name) +
                        " descriptor is invalid");
    }
    if (peer_ranking_window || ntile_window || value_window) {
      if (heap_order_terms.size() != 1) {
        return refuse("QOW-DIAG-PACKET7-OBJECT-HEAP-WINDOW-V1",
                      "object-backed " + std::string(ranking_name) +
                          " requires one direct order term");
      }
      std::uint64_t planned_receipt_workspace_bytes = 0;
      std::uint64_t actual_receipt_workspace_bytes = 0;
      if (!exec::PlanCanonicalDescriptorOrderTermBindingEvidenceWorkspace(
              heap_descriptor_order_terms.front(), ordering_property_uuid,
              &planned_receipt_workspace_bytes) ||
          planned_receipt_workspace_bytes >
              planning_request.optimizer_request.resource.memory_budget_bytes) {
        return refuse("QOW-DIAG-PACKET7-OBJECT-HEAP-WINDOW-V1",
                      "object-backed " + std::string(ranking_name) +
                          " order-term receipt exceeds its memory budget");
      }
      const auto order_term_binding_evidence_uuid =
          exec::ComputeCanonicalDescriptorOrderTermBindingEvidenceUuid(
              heap_descriptor_order_terms.front(), ordering_property_uuid,
              planned_receipt_workspace_bytes,
              &actual_receipt_workspace_bytes);
      if (order_term_binding_evidence_uuid.empty() ||
          actual_receipt_workspace_bytes !=
              planned_receipt_workspace_bytes) {
        return refuse("QOW-DIAG-PACKET7-OBJECT-HEAP-WINDOW-V1",
                      "object-backed " + std::string(ranking_name) +
                          " order-term receipt is unresolved");
      }
      if (aggregate_window) {
        const auto* aggregate_row =
            exec::LookupCanonicalAggregateByUuidV1(
                ranking_profile.function_uuid);
        if (aggregate_row == nullptr || !aggregate_row->executable ||
            !aggregate_row->aggregate_as_window ||
            aggregate_row->abi_version != 1 ||
            aggregate_row->builtin_id != ranking_profile.builtin_id ||
            aggregate_row->function_uuid != ranking_profile.function_uuid) {
          return refuse(
              "QOW-DIAG-PACKET7-OBJECT-HEAP-WINDOW-V1",
              "object-backed " + std::string(ranking_name) +
                  " aggregate registry identity drifted");
        }
        prepared_heap_aggregate_window = std::move(ranking_column);
        prepared_heap_aggregate_window_order_term =
            heap_descriptor_order_terms.front();
        prepared_heap_aggregate_window_value_column =
            ranking.navigation_value_column;
        prepared_heap_aggregate_window_descriptor = {
            aggregate_row->abi_version, aggregate_row->function,
            aggregate_row->builtin_id, aggregate_row->function_uuid,
            ranking.aggregate_count_star};
        prepared_heap_aggregate_window_frame_descriptor_uuid =
            ranking.window_frame_descriptor_uuid;
        heap_aggregate_window_order_term_binding_evidence_uuid =
            order_term_binding_evidence_uuid;
        heap_aggregate_window_frame_property_binding_evidence_uuid =
            DerivedCanonicalUuid(
                identity_scope + ":" +
                    std::string(ranking_profile.function_uuid) + ":" +
                    ranking.window_property_uuid + ":" +
                    ranking.window_frame_descriptor_uuid,
                "heap-window.aggregate-sum.frame-property-binding");
        heap_aggregate_window_capability_uuid = DerivedCanonicalUuid(
            identity_scope + ":" +
                std::string(ranking_profile.function_uuid) + ":" +
                output_descriptor->descriptor_uuid + ":" +
                heap_aggregate_window_order_term_binding_evidence_uuid + ":" +
                heap_aggregate_window_frame_property_binding_evidence_uuid,
            "heap-window.aggregate-sum.capability");
        window_capability_uuid = heap_aggregate_window_capability_uuid;
        if (window_capability_uuid.empty() ||
            window_capability_uuid ==
                heap_aggregate_window_order_term_binding_evidence_uuid ||
            window_capability_uuid ==
                heap_aggregate_window_frame_property_binding_evidence_uuid) {
          return refuse(
              "QOW-DIAG-PACKET7-OBJECT-HEAP-WINDOW-V1",
              "object-backed " + std::string(ranking_name) +
                  " capability identity is not independent");
        }
      } else if (value_window) {
        prepared_heap_navigation_window = std::move(ranking_column);
        prepared_heap_navigation_order_term =
            heap_descriptor_order_terms.front();
        prepared_heap_navigation_value_column =
            *ranking.navigation_value_column;
        prepared_heap_navigation_nth_value_position_operand =
            ranking.nth_value_position_operand;
        prepared_heap_navigation_frame_descriptor_uuid =
            ranking.window_frame_descriptor_uuid;
        prepared_heap_navigation_profile = ranking_profile;
        heap_navigation_order_term_binding_evidence_uuid =
            order_term_binding_evidence_uuid;
        heap_navigation_frame_property_binding_evidence_uuid =
            DerivedCanonicalUuid(
                identity_scope + ":" + ranking.window_property_uuid + ":" +
                    ranking.window_frame_descriptor_uuid,
                "heap-window.frame-property-binding");
        window_capability_uuid = DerivedCanonicalUuid(
            identity_scope + ":" + output_descriptor->descriptor_uuid + ":" +
                heap_navigation_order_term_binding_evidence_uuid + ":" +
                heap_navigation_frame_property_binding_evidence_uuid,
            first_value_window
                ? "heap-window.first-value.capability"
                : (last_value_window
                       ? "heap-window.last-value.capability"
                       : (nth_value_window
                              ? "heap-window.nth-value.capability"
                              : (lag_window ? "heap-window.lag.capability"
                                            : "heap-window.lead.capability"))));
        if (window_capability_uuid.empty() ||
            window_capability_uuid ==
                heap_navigation_order_term_binding_evidence_uuid ||
            window_capability_uuid ==
                heap_navigation_frame_property_binding_evidence_uuid) {
          return refuse("QOW-DIAG-PACKET7-OBJECT-HEAP-WINDOW-V1",
                        "object-backed " + std::string(ranking_name) +
                            " capability identity is not independent");
        }
      } else if (ntile_window) {
        prepared_heap_ntile = std::move(ranking_column);
        prepared_heap_ntile_order_term =
            heap_descriptor_order_terms.front();
        prepared_heap_ntile_bucket_count_operand =
            *ranking.ntile_bucket_count_operand;
        heap_ntile_order_term_binding_evidence_uuid =
            order_term_binding_evidence_uuid;
        window_capability_uuid =
            heap_ntile_order_term_binding_evidence_uuid;
      } else {
        prepared_heap_peer_ranking = std::move(ranking_column);
        prepared_heap_peer_ranking_order_term =
            heap_descriptor_order_terms.front();
        prepared_heap_peer_ranking_profile = ranking_profile;
        heap_peer_ranking_order_term_binding_evidence_uuid =
            order_term_binding_evidence_uuid;
        window_capability_uuid =
            heap_peer_ranking_order_term_binding_evidence_uuid;
      }
    } else {
      prepared_heap_row_number = std::move(ranking_column);
      window_capability_uuid =
          DerivedCanonicalUuid(identity_scope, "heap-window.capability");
    }
    window_order_evidence_uuid = DerivedCanonicalUuid(
        identity_scope + ":" + ordering_property_uuid,
        "heap-window.deterministic-order");
    profiles.push_back(
        {window_node->node_id,
         aggregate_window
             ? "window.aggregate-registry-frame-recompute.v1"
             : std::string(ranking_profile.semantic_variant_id),
         window_capability_uuid,
         plan::CanonicalLogicalRelationalNodeKind::kWindow,
         exec::PhysicalNodeKind::kWindow,
         aggregate_window
             ? "canonical.heap.window.aggregate-registry-frame-recompute.v1"
             : value_window
             ? (first_value_window
                    ? "canonical.heap.window.first-value.v1"
                    : (last_value_window
                           ? "canonical.heap.window.last-value.v1"
                           : (nth_value_window
                                  ? "canonical.heap.window.nth-value.v1"
                                  : (lag_window
                                         ? "canonical.heap.window.lag.v1"
                                         : "canonical.heap.window.lead.v1"))))
             : (ntile_window
             ? "canonical.heap.window.ntile.v1"
             : (cume_dist_window
             ? "canonical.heap.window.cume-dist.v1"
             : (percent_rank_window
                    ? "canonical.heap.window.percent-rank.v1"
                    : (dense_rank_window
                           ? "canonical.heap.window.dense-rank.v1"
                           : (rank_window ? "canonical.heap.window.rank.v1"
                                          : "canonical.heap.window.row-number.v1"))))),
         1,
         planning_request.optimizer_request.resource.memory_budget_bytes,
         1, 1, window_node->required_property_uuids,
         window_node->delivered_property_uuids,
         {plan::CanonicalLogicalPropertyKind::kOrdering,
          plan::CanonicalLogicalPropertyKind::kWindow}});
  }
  std::vector<std::size_t> projected_columns;
  std::string project_capability_uuid;
  if (project_composition) {
    const auto& project_input_descriptor_ids =
        window_composition ? window_node->output_descriptor_ids
                           : scan_node->output_descriptor_ids;
    const auto project_lineage_node_id =
        window_composition ? window_node->node_id : scan_node->node_id;
    std::vector<const api::RelationalOutputRecord*> project_outputs;
    std::vector<const api::RelationalOutputRecord*> project_input_outputs;
    for (const auto& output : dag.outputs) {
      if (output.relation_node_id == project_node->node_id) {
        project_outputs.push_back(&output);
      }
      if (output.relation_node_id == project_lineage_node_id) {
        project_input_outputs.push_back(&output);
      }
    }
    std::ranges::sort(project_outputs, {},
                      &api::RelationalOutputRecord::ordinal);
    std::ranges::sort(project_input_outputs, {},
                      &api::RelationalOutputRecord::ordinal);
    if (project_outputs.size() !=
            project_node->output_descriptor_ids.size() ||
        project_node->bound_expression_ids.size() !=
            project_outputs.size() ||
        project_input_outputs.size() !=
            project_input_descriptor_ids.size()) {
      return refuse("QOW-DIAG-PACKET7-OBJECT-HEAP-PROJECT-V1",
                    "object-backed PROJECT output binding is incomplete");
    }
    for (std::size_t ordinal = 0; ordinal < project_input_outputs.size();
         ++ordinal) {
      if (project_input_outputs[ordinal]->ordinal != ordinal ||
          project_input_outputs[ordinal]->descriptor_id !=
              project_input_descriptor_ids[ordinal]) {
        return refuse("QOW-DIAG-PACKET7-OBJECT-HEAP-PROJECT-V1",
                      "object-backed PROJECT input lineage is incomplete");
      }
    }
    std::unordered_set<std::size_t> projected_source_ordinals;
    for (std::size_t ordinal = 0; ordinal < project_outputs.size();
         ++ordinal) {
      const auto& output = *project_outputs[ordinal];
      const auto expression = std::ranges::find_if(
          dag.expressions, [&](const auto& candidate) {
            return candidate.expression_id ==
                   project_node->bound_expression_ids[ordinal];
          });
      if (expression == dag.expressions.end() || !output.visible ||
          output.ordinal != ordinal ||
          output.expression_id !=
              project_node->bound_expression_ids[ordinal] ||
          output.descriptor_id !=
              project_node->output_descriptor_ids[ordinal] ||
          expression->result_descriptor_id != output.descriptor_id) {
        return refuse("QOW-DIAG-PACKET7-OBJECT-HEAP-PROJECT-V1",
                      "object-backed PROJECT expression binding is not "
                      "exact");
      }
      const auto descriptor_id = output.descriptor_id;
      const auto source_descriptor = std::ranges::find(
          project_input_descriptor_ids, descriptor_id);
      if (source_descriptor == project_input_descriptor_ids.end()) {
        return refuse("QOW-DIAG-PACKET7-OBJECT-HEAP-PROJECT-V1",
                      "object-backed PROJECT output is not supplied by its "
                      "input");
      }
      const auto source_ordinal = static_cast<std::size_t>(std::distance(
          project_input_descriptor_ids.begin(), source_descriptor));
      if (project_input_outputs[source_ordinal]->expression_id !=
          project_node->bound_expression_ids[ordinal]) {
        return refuse("QOW-DIAG-PACKET7-OBJECT-HEAP-PROJECT-V1",
                      "object-backed PROJECT does not reference its exact "
                      "input expression");
      }
      if (!projected_source_ordinals.insert(source_ordinal).second) {
        return refuse("QOW-DIAG-PACKET7-OBJECT-HEAP-PROJECT-V1",
                      "object-backed PROJECT repeats a source column");
      }
      projected_columns.push_back(source_ordinal);
    }
    if (project_node->semantic_variant_id !=
            "project.catalog-visible-columns.v1" ||
        project_node->bound_expression_ids.size() !=
            projected_columns.size() ||
        projected_columns.empty() ||
        projected_columns.size() >=
            project_input_descriptor_ids.size()) {
      return refuse("QOW-DIAG-PACKET7-OBJECT-HEAP-PROJECT-V1",
                    "object-backed hidden-column PROJECT binding is not exact");
    }
    project_capability_uuid =
        DerivedCanonicalUuid(identity_scope, "heap-project.capability");
    profiles.push_back(
        {project_node->node_id, "project.descriptor-direct.v1",
         project_capability_uuid,
         plan::CanonicalLogicalRelationalNodeKind::kProject,
         exec::PhysicalNodeKind::kProject,
         "canonical.heap.project.visible-columns.v1", 1,
         planning_request.optimizer_request.resource.memory_budget_bytes,
         1, 1});
  }
  std::uint64_t row_limit = 0;
  std::uint64_t row_offset = 0;
  std::string limit_implementation_id;
  std::string limit_capability_uuid;
  if (limit_composition) {
    CanonicalRelationalExpressionRuntime expression_runtime(
        input.relational_dag, {});
    std::string detail;
    const auto expected_arity =
        limit_node->semantic_variant_id == "limit.bound-count.v1" ? 1U : 2U;
    if ((limit_node->semantic_variant_id != "limit.bound-count.v1" &&
         limit_node->semantic_variant_id !=
             "limit.bound-count-offset.v1") ||
        limit_node->bound_expression_ids.size() != expected_arity ||
        !EvaluateNonNegativeRowBoundForComposition(
            &expression_runtime, limit_node->bound_expression_ids.front(),
            &row_limit, &detail) ||
        (expected_arity == 2 &&
         !EvaluateNonNegativeRowBoundForComposition(
             &expression_runtime, limit_node->bound_expression_ids[1],
             &row_offset, &detail))) {
      return refuse("QOW-DIAG-PACKET7-OBJECT-HEAP-LIMIT-V1",
                    detail.empty() ? "object-backed LIMIT is not exact"
                                   : detail);
    }
    limit_implementation_id = "limit.typed.v1";
    limit_capability_uuid =
        DerivedCanonicalUuid(identity_scope, "heap-limit.capability");
    profiles.push_back(
        {limit_node->node_id, limit_implementation_id,
         limit_capability_uuid,
         plan::CanonicalLogicalRelationalNodeKind::kLimit,
         exec::PhysicalNodeKind::kLimit,
         expected_arity == 1
             ? "canonical.heap.limit.bound-count.v1"
             : "canonical.heap.limit.bound-count-offset.v1",
         1,
         planning_request.optimizer_request.resource.memory_budget_bytes,
         1, 1});
  }
  std::string cte_implementation_id;
  std::string cte_capability_uuid;
  if (cte_composition) {
    cte_implementation_id =
        cte_node->shareable ? "cte.bound.materialize.typed.v1"
                            : "cte.bound.inline.typed.v1";
    cte_capability_uuid =
        DerivedCanonicalUuid(identity_scope, "heap-cte.capability");
    LivePhysicalNodeProfile cte_profile;
    cte_profile.logical_node_id = cte_node->node_id;
    cte_profile.implementation_id = cte_implementation_id;
    cte_profile.capability_uuid = cte_capability_uuid;
    cte_profile.logical_node_kind =
        plan::CanonicalLogicalRelationalNodeKind::kCte;
    cte_profile.physical_node_kind = exec::PhysicalNodeKind::kCte;
    cte_profile.transformation_rule_id =
        cte_node->shareable ? "canonical.cte.composed-materialize.v1"
                            : "canonical.cte.composed-inline.v1";
    cte_profile.estimated_rows = 1;
    cte_profile.memory_bytes_required =
        planning_request.optimizer_request.resource.memory_budget_bytes;
    cte_profile.minimum_input_count = 1;
    cte_profile.maximum_input_count = 1;
    cte_profile.runtime_peak_from_callback_batches = true;
    cte_profile.runtime_auxiliary_from_first_input_batch =
        cte_node->shareable;
    profiles.push_back(std::move(cte_profile));
  }
  for (auto& profile : profiles) {
    if (!profile.implementation_id.starts_with("scan.heap")) {
      profile.runtime_peak_from_callback_batches = true;
    }
  }
  if (!CompleteLiveRuntimeMemoryReceipts(&profiles)) {
    return refuse("QOW-DIAG-OPT-017-REFUSAL-V1",
                  "heap composition runtime memory receipts are incomplete");
  }
  const auto physical = PlanAndPublishLivePhysicalDag(
      planning_request, profiles,
      cte_is_root
          ? "heap-cte.selected-plan"
          : limit_composition
          ? "heap-limit.selected-plan"
          : (aggregate_composition
                 ? "heap-aggregate.selected-plan"
                 : (project_composition
                 ? "heap-project.selected-plan"
                 : (window_composition
                        ? "heap-window.selected-plan"
                        : (sort_composition
                               ? "heap-sort.selected-plan"
                               : (filter_composition
                                      ? "heap-filter.selected-plan"
                                      : "heap-scan.selected-plan"))))),
      cte_is_root
          ? "object-backed heap nonrecursive CTE composition"
          : limit_composition
          ? "object-backed heap LIMIT composition"
          : (aggregate_composition
                 ? "object-backed heap global aggregate composition"
                 : (project_composition
                 ? "object-backed heap hidden-column PROJECT composition"
                 : (window_composition
                        ? (prepared_heap_aggregate_window.has_value()
                               ? "object-backed heap SUM composition"
                               : (prepared_heap_navigation_window.has_value()
                               ? "object-backed heap " +
                                     std::string(
                                         prepared_heap_navigation_profile
                                             .display_name) +
                                     " composition"
                               : (prepared_heap_ntile.has_value()
                               ? "object-backed heap NTILE composition"
                               : (prepared_heap_peer_ranking.has_value()
                               ? "object-backed heap " +
                                     std::string(prepared_heap_peer_ranking_profile
                                                     .display_name) +
                                     " composition"
                               : "object-backed heap ROW_NUMBER composition"))))
                        : (sort_composition
                               ? "object-backed heap ORDER BY composition"
                               : (filter_composition
                                      ? "object-backed heap WHERE composition"
                                      : "object-backed heap scan"))))));
  if (!physical.ok) {
    return refuse(
        physical.diagnostic_id.empty()
            ? "QOW-DIAG-PACKET7-OBJECT-HEAP-PLANNING-V1"
            : physical.diagnostic_id,
        physical.detail.empty()
            ? "object-backed heap physical DAG was not published"
            : physical.detail);
  }
  result.optimizer_selected = true;
  result.physical_dag_published = true;
  result.physical_node_count = physical.physical_dag.nodes.size();
  result.selected_plan_uuid = physical.physical_dag.selected_plan_uuid;

  const auto bounded_size = [](const std::uint64_t value) {
    return static_cast<std::size_t>(std::min<std::uint64_t>(
        value, std::numeric_limits<std::size_t>::max()));
  };
  const std::size_t maximum_scanned_row_versions =
      bounded_size(std::min(input.context.optimizer_maximum_search_steps,
                            input.context.optimizer_maximum_candidate_count));
  const std::size_t maximum_decoded_bytes =
      bounded_size(input.context.optimizer_memory_budget_bytes);
  const std::size_t maximum_output_rows =
      bounded_size(input.context.optimizer_maximum_candidate_count);
  const std::size_t maximum_output_columns = dag.outputs.size();
  std::uint64_t maximum_pair_comparisons_u64 = 0;
  if (maximum_scanned_row_versions == 0 || maximum_decoded_bytes == 0 ||
      maximum_output_rows == 0 || maximum_output_columns == 0 ||
      maximum_output_rows >
          std::numeric_limits<std::size_t>::max() / maximum_output_columns ||
      (sort_composition &&
       !CheckedMultiply(maximum_output_rows, maximum_output_rows,
                        &maximum_pair_comparisons_u64))) {
    return refuse("SBLR.PLAN_TREE.RESOURCE_LIMIT",
                  "object-backed heap execution bounds are absent or overflow");
  }

  if (filter_composition || sort_composition || window_composition ||
      project_composition || aggregate_composition || cte_composition ||
      limit_composition) {
    exec::CanonicalHeapPhysicalDagDispatchRequest heap_request;
    heap_request.context = &input.context;
    heap_request.relational_dag = &input.relational_dag;
    heap_request.physical_dag = physical.physical_dag;
    heap_request.heap_read_authority_cohort = admission.authority_cohort;
    heap_request.maximum_scanned_row_versions =
        maximum_scanned_row_versions;
    heap_request.maximum_decoded_bytes = maximum_decoded_bytes;
    heap_request.maximum_output_rows = maximum_output_rows;
    heap_request.maximum_output_columns = maximum_output_columns;
    heap_request.maximum_output_cells =
        maximum_output_rows * maximum_output_columns;
    heap_request.cancellation_requested =
        input.context.query_cancellation_requested
            ? input.context.query_cancellation_requested
            : std::function<bool()>([] { return false; });
    auto heap_registration =
        exec::BuildCanonicalHeapPhysicalRegistration(heap_request);
    if (!heap_registration.diagnostic.ok ||
        !heap_registration.registration.has_value() ||
        heap_registration.mga_authority == nullptr) {
      return refuse(
          heap_registration.diagnostic.diagnostic_code.empty()
              ? "QOW-DIAG-PACKET7-OBJECT-HEAP-REGISTRATION-V1"
              : heap_registration.diagnostic.diagnostic_code,
          heap_registration.diagnostic.detail.empty()
              ? "object-backed heap executor registration is unavailable"
              : heap_registration.diagnostic.detail);
    }

    std::vector<const api::RelationalOutputRecord*> ordered_outputs;
    const auto* publication_node =
        limit_composition
            ? &*limit_node
            : (aggregate_composition
                   ? &*aggregate_node
                   : (project_composition
                   ? &*project_node
                   : (window_composition
                          ? &*window_node
                          : (sort_composition
                                 ? &*sort_node
                                 : (filter_composition ? &*filter_node
                                                       : &*scan_node)))));
    for (const auto& output : dag.outputs) {
      if (output.relation_node_id == publication_node->node_id) {
        ordered_outputs.push_back(&output);
      }
    }
    std::ranges::sort(ordered_outputs, {},
                      &api::RelationalOutputRecord::ordinal);
    if (ordered_outputs.size() !=
        publication_node->output_descriptor_ids.size()) {
      return refuse("QOW-DIAG-PACKET7-OBJECT-HEAP-BINDING-V1",
                    "object-backed heap result bindings are incomplete");
    }

    if (grouped_sum_int64_int128_profile) {
      struct GroupedSumState {
        exec::CanonicalInt128SumStateV1 accumulator;
      };
      constexpr std::uint64_t kGroupedSumStateMemoryCharge = 256;
      constexpr std::string_view kInt128DescriptorUuid =
          "019d0000-0000-7000-8000-00000000d714";
      constexpr std::string_view kInt128TypeUuid =
          "019d0000-0000-7000-8000-00000000d715";
      const auto& cancellation_requested =
          input.context.query_cancellation_requested
              ? input.context.query_cancellation_requested
              : std::function<bool()>([] { return false; });
      if (cancellation_requested()) {
        return refuse("SB_EXECUTION_CANCELLED",
                      "grouped SUM cancelled before source admission");
      }
      const auto entry_authority =
          exec::RevalidateCanonicalExecutionMgaAuthority(
              *heap_registration.mga_authority, physical.physical_dag);
      if (!entry_authority.ok) {
        return refuse(entry_authority.diagnostic_code, entry_authority.detail);
      }
      const auto authorization = api::EvaluateMaterializedAuthorization(
          input.context, input.context.authorization_context, "SELECT",
          scan_node->required_object_uuids.front());
      if (!authorization.authorized || authorization.denied ||
          authorization.policy_recheck_required ||
          !authorization.diagnostics.empty()) {
        return refuse(
            "SECURITY.ACCESS_DENIED",
            authorization.diagnostics.empty()
                ? "grouped SUM SELECT authorization was refused"
                : authorization.diagnostics.front().detail);
      }
      auto loaded = api::LoadMgaRelationStorageDescriptor(
          input.context, scan_node->required_object_uuids.front());
      CurrentHeapStreamingScanBinding binding;
      std::string stream_detail;
      if (!loaded.ok || !PrepareCurrentHeapStreamingScanBinding(
                            dag, *scan_node, std::move(loaded.descriptor),
                            &binding, &stream_detail) ||
          binding.columns.size() != 2 || binding.descriptors.size() != 2) {
        return refuse(
            "SB_DIAG_MGA_READ_RELATION_DESCRIPTOR_INVALID",
            !stream_detail.empty()
                ? stream_detail
                : (loaded.diagnostic.detail.empty()
                       ? "grouped SUM source descriptor is unavailable"
                       : loaded.diagnostic.detail));
      }
      const auto descriptor_authority =
          api::SerializeMgaRelationStorageDescriptor(binding.persisted);
      std::map<std::optional<std::int64_t>, GroupedSumState> groups;
      std::uint64_t retained_memory = 0;
      if (!CurrentHeapStreamingBindingMemory(binding, &retained_memory) ||
          !CurrentHeapMemoryAdd(sizeof(groups), &retained_memory) ||
          retained_memory > input.context.optimizer_memory_budget_bytes) {
        return refuse("SBLR.PLAN_TREE.RESOURCE_LIMIT",
                      "grouped SUM binding exceeds the memory grant");
      }
      std::uint64_t expected_visible_rows = 0;
      bool stream_resource_refusal = false;
      bool stream_descriptor_refusal = false;
      bool numeric_overflow = false;
      api::MgaVisibleHeapRelationStreamRequest stream_request;
      stream_request.borrowed_relation_uuid = &binding.relation_uuid;
      stream_request.maximum_decoded_bytes_per_pass =
          input.context.optimizer_memory_budget_bytes;
      stream_request.maximum_memory_bytes =
          input.context.optimizer_memory_budget_bytes;
      stream_request.borrowed_cancellation_requested = &cancellation_requested;
      stream_request.prepare_consumer_for_visible_rows =
          [&](const api::MgaRelationStorageDescriptor& descriptor,
              const std::uint64_t visible_rows,
              std::uint64_t* maximum_growth) {
            if (api::SerializeMgaRelationStorageDescriptor(descriptor) !=
                descriptor_authority) {
              stream_descriptor_refusal = true;
              stream_detail =
                  "grouped SUM descriptor changed before value delivery";
              return false;
            }
            std::uint64_t growth = kGroupedSumStateMemoryCharge;
            for (const auto* column : binding.columns) {
              if (!CurrentHeapMemoryAdd(
                      std::max<std::uint64_t>(column->max_inline_bytes, 64) + 1,
                      &growth) ||
                  !CurrentHeapAccountDescriptor(column->value_descriptor,
                                                &growth)) {
                stream_resource_refusal = true;
                stream_detail = "grouped SUM row growth bound overflow";
                return false;
              }
            }
            if (maximum_growth == nullptr ||
                growth > input.context.optimizer_memory_budget_bytes) {
              stream_resource_refusal = true;
              stream_detail = "grouped SUM row growth exceeds the memory grant";
              return false;
            }
            *maximum_growth = growth;
            expected_visible_rows = visible_rows;
            return true;
          };
      stream_request.consumer_retained_memory_bytes = [&] {
        return retained_memory;
      };
      stream_request.consume_visible_row =
          [&](const std::uint64_t,
              const api::CrudRowVersionRecord& stored) {
            if (cancellation_requested()) {
              stream_detail = "grouped SUM cancelled while scanning";
              return false;
            }
            if (stored.values.size() != binding.persisted.columns.size()) {
              stream_descriptor_refusal = true;
              stream_detail = "grouped SUM stored row width changed";
              return false;
            }
            std::array<const std::string*, 2> payloads{};
            for (std::size_t ordinal = 0; ordinal < binding.columns.size();
                 ++ordinal) {
              for (const auto& [name, value] : stored.values) {
                if (name != binding.columns[ordinal]->canonical_name_key) {
                  continue;
                }
                if (payloads[ordinal] != nullptr) {
                  stream_descriptor_refusal = true;
                  stream_detail = "grouped SUM stored row repeats a field";
                  return false;
                }
                payloads[ordinal] = &value;
              }
              if (payloads[ordinal] == nullptr) {
                stream_descriptor_refusal = true;
                stream_detail = "grouped SUM stored row omits a field";
                return false;
              }
            }
            std::optional<std::int64_t> key;
            if (*payloads[0] != "<NULL>") {
              api::EngineTypedValue key_value;
              key_value.descriptor = binding.descriptors[0];
              key_value.encoded_value = *payloads[0];
              key_value.state = api::EngineValueState::value;
              const auto decoded = exec::DecodeInt64Value(key_value);
              if (!decoded.ok()) {
                stream_descriptor_refusal = true;
                stream_detail = "grouped SUM key is not canonical int64";
                return false;
              }
              key = decoded.value;
            }
            auto group = groups.find(key);
            if (group == groups.end()) {
              auto prospective = retained_memory;
              if (groups.size() >= maximum_output_rows ||
                  !CurrentHeapMemoryAdd(kGroupedSumStateMemoryCharge,
                                        &prospective) ||
                  prospective > input.context.optimizer_memory_budget_bytes) {
                stream_resource_refusal = true;
                stream_detail =
                    "grouped SUM state exceeds the row or memory grant";
                return false;
              }
              try {
                group = groups.emplace(key, GroupedSumState{}).first;
              } catch (...) {
                stream_resource_refusal = true;
                stream_detail = "grouped SUM state allocation failed";
                return false;
              }
              retained_memory = prospective;
            }
            if (*payloads[1] == "<NULL>") return true;
            api::EngineTypedValue value;
            value.descriptor = binding.descriptors[1];
            value.encoded_value = *payloads[1];
            value.state = api::EngineValueState::value;
            const auto decoded = exec::DecodeInt64Value(value);
            if (!decoded.ok()) {
              stream_descriptor_refusal = true;
              stream_detail = "grouped SUM input is not canonical int64";
              return false;
            }
            const auto transitioned = exec::TransitionCanonicalInt128SumV1(
                &group->second.accumulator, false, decoded.value);
            if (!transitioned.ok) {
              numeric_overflow =
                  transitioned.diagnostic_code == "NUMERIC.INT128.OVERFLOW";
              stream_detail = transitioned.detail.empty()
                                  ? "grouped SUM exact int128 transition refused"
                                  : transitioned.detail;
              return false;
            }
            return true;
          };
      const auto streamed =
          api::StreamVisibleMgaHeapRelation(input.context, stream_request);
      if (!streamed.ok || !streamed.memory_receipt_complete ||
          !streamed.complete_mga_chain_validation ||
          !streamed.exact_segment_extent_revalidated ||
          streamed.visible_row_count != expected_visible_rows ||
          api::SerializeMgaRelationStorageDescriptor(streamed.descriptor) !=
              descriptor_authority) {
        if (const char* trace_path =
                std::getenv("SCRATCHBIRD_SBLR_DISPATCH_PHASE_TRACE_FILE");
            trace_path != nullptr && *trace_path != '\0') {
          std::ofstream trace(trace_path, std::ios::app | std::ios::binary);
          if (trace) {
            trace << "layer=canonical_grouped_sum_heap_stream"
                  << "\taccepted=false"
                  << "\tstream_ok=" << streamed.ok
                  << "\tmemory_receipt_complete="
                  << streamed.memory_receipt_complete
                  << "\tchain_complete="
                  << streamed.complete_mga_chain_validation
                  << "\textent_revalidated="
                  << streamed.exact_segment_extent_revalidated
                  << "\tscanned_rows="
                  << streamed.scanned_row_version_count
                  << "\tsecond_pass_scanned_rows="
                  << streamed.second_pass_scanned_row_version_count
                  << "\tdelivered_rows=" << streamed.delivered_row_count
                  << "\tvisible_rows=" << streamed.visible_row_count
                  << "\texpected_visible_rows=" << expected_visible_rows
                  << "\tcategory="
                  << static_cast<unsigned>(streamed.failure_category)
                  << "\tdetail=" << streamed.diagnostic.detail << '\n';
          }
        }
        const auto diagnostic =
            numeric_overflow
                ? "NUMERIC.INT128.OVERFLOW"
                : (streamed.cancellation_observed
                       ? "SB_EXECUTION_CANCELLED"
                       : (stream_resource_refusal ||
                                  streamed.failure_category ==
                                      api::MgaHeapReadFailureCategoryV1::kResource
                              ? "SBLR.PLAN_TREE.RESOURCE_LIMIT"
                              : (stream_descriptor_refusal
                                     ? "DATATYPE.DESCRIPTOR_INVALID"
                                     : "QOW-DIAG-QRY-004-HEAP-READ-V1")));
        return refuse(diagnostic,
                      !stream_detail.empty()
                          ? stream_detail
                          : (streamed.diagnostic.detail.empty()
                                 ? "grouped SUM source receipt is incomplete"
                                 : streamed.diagnostic.detail));
      }

      const auto result_descriptor = std::ranges::find_if(
          dag.descriptors, [&](const auto& descriptor) {
            return descriptor.descriptor_id ==
                   aggregate_node->output_descriptor_ids[1];
          });
      if (ordered_outputs.size() != 2 ||
          result_descriptor == dag.descriptors.end() ||
          result_descriptor->descriptor_uuid != kInt128DescriptorUuid ||
          result_descriptor->type_uuid != kInt128TypeUuid) {
        return refuse("RESULT_SET.SHAPE_INVALID",
                      "grouped SUM result descriptor is not exact int128");
      }
      api::EngineDescriptor int128_descriptor;
      int128_descriptor.descriptor_uuid.canonical =
          result_descriptor->descriptor_uuid;
      int128_descriptor.descriptor_kind = "scalar";
      int128_descriptor.canonical_type_name = "int128";
      int128_descriptor.encoded_descriptor =
          "type_uuid=" + result_descriptor->type_uuid +
          ";nullability=nullable";
      std::vector<exec::CanonicalResultColumnBinding> column_bindings;
      exec::DescriptorBatch pending_page;
      pending_page.columns = {
          {ordered_outputs[0]->output_name_utf8, binding.descriptors[0],
           binding.columns[0]->nullable,
           aggregate_node->output_descriptor_ids[0]},
          {ordered_outputs[1]->output_name_utf8, int128_descriptor, true,
           aggregate_node->output_descriptor_ids[1]}};
      for (std::size_t ordinal = 0; ordinal < ordered_outputs.size(); ++ordinal) {
        const auto descriptor = std::ranges::find_if(
            dag.descriptors, [&](const auto& candidate) {
              return candidate.descriptor_id ==
                     ordered_outputs[ordinal]->descriptor_id;
            });
        if (descriptor == dag.descriptors.end()) {
          return refuse("RESULT_SET.SHAPE_INVALID",
                        "grouped SUM publication descriptor is unresolved");
        }
        exec::CanonicalResultColumnDescriptor published;
        published.ordinal = static_cast<std::uint32_t>(ordinal);
        published.name_utf8 = ordered_outputs[ordinal]->output_name_utf8;
        published.descriptor_uuid = descriptor->descriptor_uuid;
        published.type_uuid = descriptor->type_uuid;
        published.nullability = ResultNullability(descriptor->nullability);
        published.collation_uuid = descriptor->collation_uuid;
        published.timezone_profile_id = descriptor->timezone_profile_id;
        column_bindings.push_back({ordinal, true, std::move(published)});
      }
      result.api_result.ok = true;
      result.api_result.operation_id = "query.execute";
      result.api_result.result_shape.result_kind = "rows";
      result.api_result.local_transaction_id =
          input.context.local_transaction_id;
      result.api_result.transaction_uuid = input.context.transaction_uuid;
      result.api_result.embedded_trust_mode_observed =
          input.context.trust_mode == api::EngineTrustMode::embedded_in_process;
      for (const auto& column : pending_page.columns) {
        result.api_result.result_shape.columns.push_back(column.descriptor);
      }
      constexpr std::size_t kGroupedSumPageRows = 128;
      pending_page.rows.reserve(kGroupedSumPageRows);
      std::shared_ptr<exec::CanonicalResultCursorSession> cursor_session;
      const auto cursor_uuid =
          DerivedCanonicalUuid(identity_scope, "heap-grouped-sum-result.cursor");
      const auto execution_attempt_uuid = DerivedCanonicalUuid(
          identity_scope + ":" + input.context.current_monotonic_ns,
          "heap-grouped-sum.execution-attempt");
      const auto transaction_effect_uuid = DerivedCanonicalUuid(
          identity_scope + ":" +
              std::to_string(input.context.local_transaction_id),
          "heap-grouped-sum.transaction-effect-unchanged");
      std::uint64_t cursor_batch_ordinal = 0;
      std::uint64_t cursor_first_row_ordinal = 0;
      bool initial_page = true;
      const auto publish_page = [&](const bool final_page) {
        exec::CanonicalResultPublicationRequest publication;
        publication.statement_uuid = input.context.statement_uuid.canonical;
        publication.mga_authority = *heap_registration.mga_authority;
        publication.selected_physical_dag = physical.physical_dag;
        publication.selected_catalog_epoch_uuid =
            input.context.catalog_epoch_uuid.canonical;
        publication.execution_attempt_uuid = execution_attempt_uuid;
        publication.result_kind = exec::CanonicalResultKind::kCursor;
        publication.invocation_mode =
            exec::CanonicalResultInvocationMode::kDirect;
        publication.physical_output_batch = std::move(pending_page);
        publication.column_bindings = column_bindings;
        publication.cursor_state =
            final_page ? exec::CanonicalResultCursorState::kClosed
                       : exec::CanonicalResultCursorState::kOpen;
        publication.cursor_uuid = cursor_uuid;
        publication.cursor_session = cursor_session;
        publication.cursor_batch_ordinal = cursor_batch_ordinal;
        publication.cursor_first_row_ordinal = cursor_first_row_ordinal;
        publication.transaction_effect_evidence_uuid = transaction_effect_uuid;
        publication.maximum_row_count = maximum_output_rows;
        publication.maximum_rows_per_batch = kGroupedSumPageRows;
        if (initial_page) {
          publication.cursor_cancellation_requested = cancellation_requested;
          publication.cursor_cancellation_diagnostic = {
              "QOW-DIAGNOSTIC-INSTANCE-GROUPED-SUM-CANCEL",
              "SB_EXECUTION_CANCELLED",
              exec::CanonicalResultDiagnosticSeverity::kError, "57014",
              "query.grouped_sum.cursor.delivery.cancelled", {},
              exec::CanonicalResultDiagnosticPhase::kFinalize, "result.cursor",
              "cancellation_probe", 1,
              exec::CanonicalResultTransactionEffect::
                  kStatementFailedTransactionUsable,
              exec::CanonicalResultRetryability::kNotRetryable};
          publication.cursor_release = [](const auto) {};
        }
        auto published = exec::PublishCanonicalResultEnvelope(publication);
        pending_page = {};
        pending_page.columns = publication.physical_output_batch.columns;
        pending_page.rows.reserve(kGroupedSumPageRows);
        if (!published.published || !published.diagnostic.ok ||
            published.row_stream.rows.size() !=
                publication.physical_output_batch.rows.size() ||
            published.cursor_end_of_stream != final_page) {
          stream_detail = published.diagnostic.detail.empty()
                              ? "grouped SUM cursor publication failed"
                              : published.diagnostic.detail;
          return false;
        }
        if (initial_page) {
          cursor_session = published.cursor_session;
          initial_page = false;
        }
        cursor_batch_ordinal = published.cursor_next_batch_ordinal;
        cursor_first_row_ordinal = published.cursor_next_row_ordinal;
        result.canonical_result_bytes =
            std::move(published.canonical_envelope_bytes);
        for (auto& row : published.row_stream.rows) {
          api::EngineRowValue api_row;
          for (std::size_t column = 0; column < row.values.size(); ++column) {
            api_row.fields.emplace_back(
                published.envelope.column_descriptors[column].name_utf8,
                std::move(row.values[column]));
          }
          result.api_result.result_shape.rows.push_back(std::move(api_row));
        }
        return true;
      };
      for (const auto& [key, state] : groups) {
        if (cancellation_requested()) {
          return refuse("SB_EXECUTION_CANCELLED",
                        "grouped SUM cancelled during publication");
        }
        if (pending_page.rows.size() == kGroupedSumPageRows &&
            !publish_page(false)) {
          return refuse("QOW-RESULT-DIAGNOSTIC-ABI-V1", stream_detail);
        }
        exec::DescriptorTuple tuple;
        api::EngineTypedValue key_value;
        key_value.descriptor = binding.descriptors[0];
        if (key.has_value()) {
          key_value.encoded_value = std::to_string(*key);
          key_value.state = api::EngineValueState::value;
        } else {
          key_value.is_null = true;
          key_value.state = api::EngineValueState::sql_null;
        }
        tuple.values.push_back(std::move(key_value));
        auto finalized = exec::FinalizeCanonicalInt128SumV1(
            state.accumulator, int128_descriptor);
        if (!finalized.diagnostic.ok) {
          return refuse(finalized.diagnostic.diagnostic_code,
                        finalized.diagnostic.detail);
        }
        tuple.values.push_back(std::move(finalized.value));
        pending_page.rows.push_back(std::move(tuple));
      }
      if (!publish_page(true)) {
        return refuse(cancellation_requested()
                          ? "SB_EXECUTION_CANCELLED"
                          : "QOW-RESULT-DIAGNOSTIC-ABI-V1",
                      stream_detail);
      }
      const auto result_authority =
          exec::RevalidateCanonicalExecutionMgaAuthority(
              *heap_registration.mga_authority, physical.physical_dag);
      if (!result_authority.ok) {
        return refuse(result_authority.diagnostic_code,
                      result_authority.detail);
      }
      result.physical_dag_executed = true;
      result.runtime_actuals_attached = true;
      result.canonical_result_published = true;
      result.canonical_result_column_count = ordered_outputs.size();
      result.canonical_result_row_count = groups.size();
      result.api_result.evidence.push_back(
          {"canonical.selected_plan", physical.physical_dag.selected_plan_uuid});
      result.api_result.evidence.push_back(
          {"canonical.heap_grouped_sum_streaming_mga", "complete"});
      result.api_result.evidence.push_back(
          {"canonical.heap_grouped_sum_result_codec",
           "datatype.int128.le.v1"});
      return result;
    }

    // A filter/order/limit query has a semantic output bound independent of
    // source cardinality.  Keep only the best OFFSET+LIMIT rows while the MGA
    // stream owns visibility and extent validation.  The generic physical
    // dispatcher materializes the complete source before filtering, which is
    // neither necessary nor admitted for a bounded top-K profile.
    const bool streaming_topk_profile =
        filter_composition && sort_composition && limit_composition &&
        !project_composition && !window_composition &&
        !aggregate_composition && !cte_composition;
    if (streaming_topk_profile) {
      if (row_offset > std::numeric_limits<std::uint64_t>::max() - row_limit) {
        return refuse("SBLR.PLAN_TREE.RESOURCE_LIMIT",
                      "streaming top-K offset and limit overflow");
      }
      const auto retained_row_limit_u64 = row_offset + row_limit;
      if (retained_row_limit_u64 >
              static_cast<std::uint64_t>(
                  std::numeric_limits<std::size_t>::max()) ||
          retained_row_limit_u64 > maximum_output_rows) {
        return refuse(
            "SBLR.PLAN_TREE.RESOURCE_LIMIT",
            "streaming top-K semantic carrier exceeds the admitted row bound");
      }
      const auto retained_row_limit =
          static_cast<std::size_t>(retained_row_limit_u64);
      const auto& cancellation_requested =
          input.context.query_cancellation_requested
              ? input.context.query_cancellation_requested
              : std::function<bool()>([] { return false; });
      if (cancellation_requested()) {
        return refuse("SB_EXECUTION_CANCELLED",
                      "streaming top-K cancelled before source admission");
      }
      const auto entry_authority =
          exec::RevalidateCanonicalExecutionMgaAuthority(
              *heap_registration.mga_authority, physical.physical_dag);
      if (!entry_authority.ok) {
        return refuse(entry_authority.diagnostic_code, entry_authority.detail);
      }
      const auto authorization = api::EvaluateMaterializedAuthorization(
          input.context, input.context.authorization_context, "SELECT",
          scan_node->required_object_uuids.front());
      if (!authorization.authorized || authorization.denied ||
          authorization.policy_recheck_required ||
          !authorization.diagnostics.empty()) {
        return refuse(
            "SECURITY.ACCESS_DENIED",
            authorization.diagnostics.empty()
                ? "streaming top-K SELECT authorization was refused"
                : authorization.diagnostics.front().detail);
      }

      auto loaded = api::LoadMgaRelationStorageDescriptor(
          input.context, scan_node->required_object_uuids.front());
      CurrentHeapStreamingScanBinding binding;
      std::string stream_detail;
      if (!loaded.ok || !PrepareCurrentHeapStreamingScanBinding(
                            dag, *scan_node, std::move(loaded.descriptor),
                            &binding, &stream_detail)) {
        return refuse(
            "SB_DIAG_MGA_READ_RELATION_DESCRIPTOR_INVALID",
            !stream_detail.empty()
                ? stream_detail
                : (loaded.diagnostic.detail.empty()
                       ? "streaming top-K source descriptor is unavailable"
                       : loaded.diagnostic.detail));
      }
      const auto descriptor_authority =
          api::SerializeMgaRelationStorageDescriptor(binding.persisted);
      std::uint64_t retained_memory = 0;
      std::uint64_t allocation = 0;
      if (!CurrentHeapStreamingBindingMemory(binding, &retained_memory) ||
          !CurrentHeapMemoryMultiply(
              retained_row_limit, sizeof(CurrentHeapStreamingCompactRow),
              &allocation) ||
          !CurrentHeapMemoryAdd(allocation, &retained_memory) ||
          retained_memory > input.context.optimizer_memory_budget_bytes) {
        return refuse("SBLR.PLAN_TREE.RESOURCE_LIMIT",
                      "streaming top-K binding exceeds the memory grant");
      }
      std::vector<CurrentHeapStreamingCompactRow> retained_rows;
      try {
        retained_rows.reserve(retained_row_limit);
      } catch (...) {
        return refuse("SBLR.PLAN_TREE.RESOURCE_LIMIT",
                      "streaming top-K row reservation failed");
      }
      if (!CurrentHeapStreamingBindingMemory(binding, &retained_memory) ||
          !CurrentHeapMemoryMultiply(
              retained_rows.capacity(), sizeof(CurrentHeapStreamingCompactRow),
              &allocation) ||
          !CurrentHeapMemoryAdd(allocation, &retained_memory) ||
          retained_memory > input.context.optimizer_memory_budget_bytes) {
        return refuse("SBLR.PLAN_TREE.RESOURCE_LIMIT",
                      "streaming top-K row reservation exceeds the memory grant");
      }

      CanonicalRelationalExpressionRuntimeServices predicate_services;
      predicate_services.comparison_evaluator =
          [context = &input.context](const api::EngineTypedValue& left,
                                     const api::EngineTypedValue& right,
                                     int* comparison,
                                     std::string* diagnostic_id,
                                     std::string* refusal_detail) {
            return CompareCanonicalRelationalScalarsV1(
                *context, left, right, comparison, diagnostic_id,
                refusal_detail);
          };
      BindCanonicalPersistedRowDescriptorAuthorityForComposition(input.context,
                                                     &predicate_services);
      CanonicalRelationalExpressionRuntime predicate_runtime(
          dag, std::move(predicate_services));
      std::optional<std::size_t> direct_filter_ordinal;
      std::optional<std::int64_t> direct_filter_equal_value;
      const auto filter_root = std::ranges::find_if(
          dag.expressions, [&](const auto& expression) {
            return expression.expression_id ==
                   filter_node->bound_expression_ids.front();
          });
      if (filter_root != dag.expressions.end() &&
          filter_root->operator_name == std::optional<std::string>("=") &&
          filter_root->child_expression_ids.size() == 2) {
        const api::RelationalExpressionRecord* identifier = nullptr;
        const api::RelationalExpressionRecord* literal = nullptr;
        for (const auto child_id : filter_root->child_expression_ids) {
          const auto child = std::ranges::find_if(
              dag.expressions, [&](const auto& expression) {
                return expression.expression_id == child_id;
              });
          if (child == dag.expressions.end()) continue;
          if (child->expression_kind ==
              api::RelationalExpressionKind::kIdentifier) {
            identifier = &*child;
          } else if (child->expression_kind ==
                         api::RelationalExpressionKind::kLiteral &&
                     child->literal_kind ==
                         api::RelationalLiteralKind::kNumeric) {
            literal = &*child;
          }
        }
        const auto slot =
            identifier == nullptr
                ? filter_row_binding.slots.end()
                : std::ranges::find_if(
                      filter_row_binding.slots, [&](const auto& candidate) {
                        return candidate.expression_id ==
                                   identifier->expression_id &&
                               candidate.slot_kind ==
                                   CanonicalRelationalExpressionRowSlotKind::
                                       input_identifier;
                      });
        const auto literal_descriptor =
            literal == nullptr
                ? dag.descriptors.end()
                : std::ranges::find_if(
                      dag.descriptors, [&](const auto& descriptor) {
                        return descriptor.descriptor_id ==
                               literal->result_descriptor_id;
                      });
        if (identifier != nullptr && literal != nullptr &&
            slot != filter_row_binding.slots.end() &&
            slot->row_ordinal < binding.descriptors.size() &&
            literal_descriptor != dag.descriptors.end() &&
            ExactBoundedSignedIntegerTypeRankForComposition(
                literal_descriptor->type_uuid) != 0) {
          api::EngineTypedValue literal_value;
          std::string literal_detail;
          if (predicate_runtime.EvaluateForConsumer(
                  literal->expression_id,
                  binding.descriptors[slot->row_ordinal].canonical_type_name,
                  api::EngineCanonicalExpressionConsumer::filter,
                  &literal_value, &literal_detail)) {
            const auto decoded = exec::DecodeInt64Value(literal_value);
            if (decoded.ok()) {
              direct_filter_ordinal = slot->row_ordinal;
              direct_filter_equal_value = decoded.value;
            }
          }
        }
      }
      std::vector<api::EngineTypedValue> candidate_values;
      std::vector<api::EngineTypedValue> retained_values;
      candidate_values.reserve(binding.descriptors.size());
      retained_values.reserve(binding.descriptors.size());
      bool stream_resource_refusal = false;
      bool stream_descriptor_refusal = false;
      bool stream_predicate_refusal = false;
      bool stream_order_refusal = false;
      std::uint64_t expected_visible_rows = 0;
      const auto maximum_row_growth = [&] (std::uint64_t* growth) {
        if (growth == nullptr) return false;
        *growth = sizeof(CurrentHeapStreamingCompactRow);
        std::uint64_t bytes = 0;
        if (!CurrentHeapMemoryMultiply(binding.columns.size(),
                                       sizeof(std::string), &bytes) ||
            !CurrentHeapMemoryAdd(bytes, growth) ||
            !CurrentHeapMemoryAdd(binding.columns.size(), growth) ||
            !CurrentHeapMemoryMultiply(binding.descriptors.size(),
                                       sizeof(api::EngineTypedValue), &bytes) ||
            !CurrentHeapMemoryAdd(bytes, growth)) {
          return false;
        }
        for (const auto* column : binding.columns) {
          if (!CurrentHeapMemoryAdd(
                  std::max<std::uint64_t>(column->max_inline_bytes, 64) + 1,
                  growth) ||
              !CurrentHeapAccountDescriptor(column->value_descriptor, growth)) {
            return false;
          }
        }
        return true;
      };
      const auto row_less = [&](const CurrentHeapStreamingCompactRow& left,
                                const CurrentHeapStreamingCompactRow& right,
                                bool* less) {
        if (less == nullptr ||
            !MaterializeCurrentHeapStreamingRow(binding, left,
                                                &candidate_values) ||
            !MaterializeCurrentHeapStreamingRow(binding, right,
                                                &retained_values)) {
          stream_detail = "streaming top-K typed row materialization failed";
          return false;
        }
        for (const auto& term : heap_descriptor_order_terms) {
          if (term.column >= candidate_values.size()) {
            stream_detail = "streaming top-K order column is out of range";
            return false;
          }
          const auto compared = exec::CompareCanonicalDescriptorOrderValues(
              candidate_values[term.column], retained_values[term.column],
              term);
          if (!compared.diagnostic.ok) {
            stream_detail = compared.diagnostic.detail.empty()
                                ? "streaming top-K order comparison refused"
                                : compared.diagnostic.detail;
            return false;
          }
          if (compared.comparison != 0) {
            *less = compared.comparison < 0;
            return true;
          }
        }
        *less = left.source_ordinal < right.source_ordinal;
        return true;
      };

      api::MgaVisibleHeapRelationStreamRequest stream_request;
      stream_request.borrowed_relation_uuid = &binding.relation_uuid;
      stream_request.maximum_decoded_bytes_per_pass =
          input.context.optimizer_memory_budget_bytes;
      stream_request.maximum_memory_bytes =
          input.context.optimizer_memory_budget_bytes;
      stream_request.borrowed_cancellation_requested = &cancellation_requested;
      stream_request.prepare_consumer_for_visible_rows =
          [&](const api::MgaRelationStorageDescriptor& descriptor,
              const std::uint64_t visible_rows,
              std::uint64_t* maximum_growth) {
            if (api::SerializeMgaRelationStorageDescriptor(descriptor) !=
                descriptor_authority) {
              stream_descriptor_refusal = true;
              stream_detail =
                  "streaming top-K descriptor changed before value delivery";
              return false;
            }
            if (!maximum_row_growth(maximum_growth) ||
                *maximum_growth >
                    input.context.optimizer_memory_budget_bytes) {
              stream_resource_refusal = true;
              stream_detail =
                  "streaming top-K row growth bound is unavailable";
              return false;
            }
            expected_visible_rows = visible_rows;
            return true;
          };
      stream_request.consumer_retained_memory_bytes = [&] {
        return retained_memory;
      };
      stream_request.consume_visible_row =
          [&](const std::uint64_t source_ordinal,
              const api::CrudRowVersionRecord& stored) {
            if (cancellation_requested()) {
              stream_detail = "streaming top-K cancelled while scanning";
              return false;
            }
            if (stored.values.size() != binding.persisted.columns.size()) {
              stream_descriptor_refusal = true;
              stream_detail = "streaming top-K stored row width changed";
              return false;
            }
            CurrentHeapStreamingCompactRow row;
            row.source_ordinal = source_ordinal;
            row.values.resize(binding.columns.size());
            row.nulls.resize(binding.columns.size(), 0);
            for (std::size_t ordinal = 0; ordinal < binding.columns.size();
                 ++ordinal) {
              const auto* column = binding.columns[ordinal];
              const std::string* payload = nullptr;
              for (const auto& [name, value] : stored.values) {
                if (name != column->canonical_name_key) continue;
                if (payload != nullptr) {
                  stream_descriptor_refusal = true;
                  stream_detail = "streaming top-K stored row repeats a field";
                  return false;
                }
                payload = &value;
              }
              if (payload == nullptr) {
                stream_descriptor_refusal = true;
                stream_detail = "streaming top-K stored row omits a field";
                return false;
              }
              if (*payload == "<NULL>") {
                row.nulls[ordinal] = 1;
              } else {
                row.values[ordinal] = *payload;
              }
            }
            if (!MaterializeCurrentHeapStreamingRow(binding, row,
                                                    &candidate_values)) {
              stream_descriptor_refusal = true;
              stream_detail = "streaming top-K typed row is invalid";
              return false;
            }
            api::EngineSqlTruthValue truth =
                api::EngineSqlTruthValue::unknown;
            if (direct_filter_ordinal.has_value() &&
                direct_filter_equal_value.has_value()) {
              const auto& value = candidate_values[*direct_filter_ordinal];
              if (value.state == api::EngineValueState::sql_null) {
                truth = api::EngineSqlTruthValue::unknown;
              } else {
                const auto decoded = exec::DecodeInt64Value(value);
                if (!decoded.ok()) {
                  stream_predicate_refusal = true;
                  stream_detail =
                      "streaming top-K direct predicate input is invalid";
                  return false;
                }
                truth = decoded.value == *direct_filter_equal_value
                            ? api::EngineSqlTruthValue::true_value
                            : api::EngineSqlTruthValue::false_value;
              }
            } else {
              std::string predicate_detail;
              if (!predicate_runtime.EvaluatePredicateForConsumer(
                      filter_node->bound_expression_ids.front(),
                      filter_row_binding,
                      CanonicalRelationalExpressionRowView{candidate_values,
                                                           {}},
                      api::EngineCanonicalExpressionConsumer::filter, &truth,
                      &predicate_detail)) {
                stream_predicate_refusal = true;
                stream_detail = predicate_detail.empty()
                                    ? "streaming top-K predicate refused"
                                    : predicate_detail;
                return false;
              }
            }
            if (truth != api::EngineSqlTruthValue::true_value ||
                retained_row_limit == 0) {
              return true;
            }
            std::size_t insertion = 0;
            for (; insertion < retained_rows.size(); ++insertion) {
              bool less = false;
              if (!row_less(row, retained_rows[insertion], &less)) {
                stream_order_refusal = true;
                return false;
              }
              if (less) break;
            }
            if (retained_rows.size() == retained_row_limit &&
                insertion == retained_rows.size()) {
              return true;
            }
            std::uint64_t row_memory = 0;
            std::uint64_t removed_memory = 0;
            auto prospective = retained_memory;
            if (!CurrentHeapStreamingCompactRowMemory(row, &row_memory) ||
                row_memory < sizeof(row)) {
              stream_resource_refusal = true;
              stream_detail = "streaming top-K row memory overflow";
              return false;
            }
            row_memory -= sizeof(row);
            if (retained_rows.size() == retained_row_limit) {
              if (!CurrentHeapStreamingCompactRowMemory(retained_rows.back(),
                                                        &removed_memory) ||
                  removed_memory < sizeof(CurrentHeapStreamingCompactRow) ||
                  prospective <
                      removed_memory - sizeof(CurrentHeapStreamingCompactRow)) {
                stream_resource_refusal = true;
                stream_detail = "streaming top-K replacement memory underflow";
                return false;
              }
              prospective -=
                  removed_memory - sizeof(CurrentHeapStreamingCompactRow);
            }
            if (!CurrentHeapMemoryAdd(row_memory, &prospective) ||
                prospective > input.context.optimizer_memory_budget_bytes) {
              stream_resource_refusal = true;
              stream_detail = "streaming top-K rows exceed the memory grant";
              return false;
            }
            try {
              retained_rows.insert(retained_rows.begin() + insertion,
                                   std::move(row));
              if (retained_rows.size() > retained_row_limit) {
                retained_rows.pop_back();
              }
            } catch (...) {
              stream_resource_refusal = true;
              stream_detail = "streaming top-K row insertion failed";
              return false;
            }
            retained_memory = prospective;
            return true;
          };
      const auto streamed =
          api::StreamVisibleMgaHeapRelation(input.context, stream_request);
      if (!streamed.ok || !streamed.memory_receipt_complete ||
          !streamed.complete_mga_chain_validation ||
          !streamed.exact_segment_extent_revalidated ||
          streamed.visible_row_count != expected_visible_rows ||
          api::SerializeMgaRelationStorageDescriptor(streamed.descriptor) !=
              descriptor_authority) {
        const auto diagnostic =
            streamed.cancellation_observed
                ? "SB_EXECUTION_CANCELLED"
                : (stream_resource_refusal ||
                           streamed.failure_category ==
                               api::MgaHeapReadFailureCategoryV1::kResource
                       ? "SBLR.PLAN_TREE.RESOURCE_LIMIT"
                       : (stream_descriptor_refusal
                              ? "SB_DIAG_MGA_READ_RELATION_DESCRIPTOR_INVALID"
                              : (stream_predicate_refusal
                                     ? "QOW-DIAG-PACKET7-OBJECT-HEAP-FILTER-V1"
                                     : (stream_order_refusal
                                            ? "QOW-DIAG-PACKET7-OBJECT-HEAP-SORT-V1"
                                            : "QOW-DIAG-QRY-004-HEAP-READ-V1"))));
        return refuse(diagnostic,
                      !stream_detail.empty()
                          ? stream_detail
                          : (streamed.diagnostic.detail.empty()
                                 ? "streaming top-K source receipt is incomplete"
                                 : streamed.diagnostic.detail));
      }

      const auto first_result =
          std::min<std::size_t>(retained_rows.size(),
                                static_cast<std::size_t>(row_offset));
      const auto available = retained_rows.size() - first_result;
      const auto result_rows = std::min<std::size_t>(
          available, static_cast<std::size_t>(row_limit));
      std::vector<exec::CanonicalResultColumnBinding> column_bindings;
      exec::DescriptorBatch pending_page;
      column_bindings.reserve(ordered_outputs.size());
      pending_page.columns.reserve(ordered_outputs.size());
      for (std::size_t ordinal = 0; ordinal < ordered_outputs.size(); ++ordinal) {
        const auto descriptor = std::ranges::find_if(
            dag.descriptors, [&](const auto& candidate) {
              return candidate.descriptor_id ==
                     ordered_outputs[ordinal]->descriptor_id;
            });
        if (descriptor == dag.descriptors.end() ||
            ordinal >= binding.descriptors.size()) {
          return refuse("RESULT_SET.SHAPE_INVALID",
                        "streaming top-K result descriptor is unresolved");
        }
        exec::CanonicalResultColumnDescriptor published;
        published.ordinal = static_cast<std::uint32_t>(ordinal);
        published.name_utf8 = ordered_outputs[ordinal]->output_name_utf8;
        published.descriptor_uuid = descriptor->descriptor_uuid;
        published.type_uuid = descriptor->type_uuid;
        published.nullability = ResultNullability(descriptor->nullability);
        published.collation_uuid = descriptor->collation_uuid;
        published.timezone_profile_id = descriptor->timezone_profile_id;
        column_bindings.push_back({ordinal, true, std::move(published)});
        pending_page.columns.push_back(
            {ordered_outputs[ordinal]->output_name_utf8,
             binding.descriptors[ordinal],
             descriptor->nullability == api::RelationalNullability::kNullable,
             descriptor->descriptor_id});
      }
      result.api_result.ok = true;
      result.api_result.operation_id = "query.execute";
      result.api_result.result_shape.result_kind = "rows";
      result.api_result.local_transaction_id =
          input.context.local_transaction_id;
      result.api_result.transaction_uuid = input.context.transaction_uuid;
      result.api_result.embedded_trust_mode_observed =
          input.context.trust_mode == api::EngineTrustMode::embedded_in_process;
      for (const auto& column : pending_page.columns) {
        result.api_result.result_shape.columns.push_back(column.descriptor);
      }
      constexpr std::size_t kStreamingTopkPageRows = 128;
      pending_page.rows.reserve(kStreamingTopkPageRows);
      std::shared_ptr<exec::CanonicalResultCursorSession> cursor_session;
      const auto cursor_uuid =
          DerivedCanonicalUuid(identity_scope, "heap-topk-result.cursor");
      const auto execution_attempt_uuid = DerivedCanonicalUuid(
          identity_scope + ":" + input.context.current_monotonic_ns,
          "heap-topk.execution-attempt");
      const auto transaction_effect_uuid = DerivedCanonicalUuid(
          identity_scope + ":" +
              std::to_string(input.context.local_transaction_id),
          "heap-topk.transaction-effect-unchanged");
      std::uint64_t cursor_batch_ordinal = 0;
      std::uint64_t cursor_first_row_ordinal = 0;
      bool initial_page = true;
      const auto publish_page = [&](const bool final_page) {
        exec::CanonicalResultPublicationRequest publication;
        publication.statement_uuid = input.context.statement_uuid.canonical;
        publication.mga_authority = *heap_registration.mga_authority;
        publication.selected_physical_dag = physical.physical_dag;
        publication.selected_catalog_epoch_uuid =
            input.context.catalog_epoch_uuid.canonical;
        publication.execution_attempt_uuid = execution_attempt_uuid;
        publication.result_kind = exec::CanonicalResultKind::kCursor;
        publication.invocation_mode =
            exec::CanonicalResultInvocationMode::kDirect;
        publication.physical_output_batch = std::move(pending_page);
        publication.column_bindings = column_bindings;
        publication.cursor_state =
            final_page ? exec::CanonicalResultCursorState::kClosed
                       : exec::CanonicalResultCursorState::kOpen;
        publication.cursor_uuid = cursor_uuid;
        publication.cursor_session = cursor_session;
        publication.cursor_batch_ordinal = cursor_batch_ordinal;
        publication.cursor_first_row_ordinal = cursor_first_row_ordinal;
        publication.transaction_effect_evidence_uuid = transaction_effect_uuid;
        publication.maximum_row_count = maximum_output_rows;
        publication.maximum_rows_per_batch = kStreamingTopkPageRows;
        if (initial_page) {
          publication.cursor_cancellation_requested = cancellation_requested;
          publication.cursor_cancellation_diagnostic = {
              "QOW-DIAGNOSTIC-INSTANCE-TOPK-CANCEL", "SB_EXECUTION_CANCELLED",
              exec::CanonicalResultDiagnosticSeverity::kError, "57014",
              "query.topk.cursor.delivery.cancelled", {},
              exec::CanonicalResultDiagnosticPhase::kFinalize, "result.cursor",
              "cancellation_probe", 1,
              exec::CanonicalResultTransactionEffect::
                  kStatementFailedTransactionUsable,
              exec::CanonicalResultRetryability::kNotRetryable};
          publication.cursor_release = [](const auto) {};
        }
        auto published = exec::PublishCanonicalResultEnvelope(publication);
        pending_page = {};
        pending_page.columns = publication.physical_output_batch.columns;
        pending_page.rows.reserve(kStreamingTopkPageRows);
        if (!published.published || !published.diagnostic.ok ||
            published.row_stream.rows.size() !=
                publication.physical_output_batch.rows.size() ||
            published.cursor_end_of_stream != final_page) {
          stream_detail = published.diagnostic.detail.empty()
                              ? "streaming top-K cursor publication failed"
                              : published.diagnostic.detail;
          return false;
        }
        if (initial_page) {
          cursor_session = published.cursor_session;
          initial_page = false;
        }
        cursor_batch_ordinal = published.cursor_next_batch_ordinal;
        cursor_first_row_ordinal = published.cursor_next_row_ordinal;
        result.canonical_result_bytes =
            std::move(published.canonical_envelope_bytes);
        for (auto& row : published.row_stream.rows) {
          api::EngineRowValue api_row;
          for (std::size_t column = 0; column < row.values.size(); ++column) {
            api_row.fields.emplace_back(
                published.envelope.column_descriptors[column].name_utf8,
                std::move(row.values[column]));
          }
          result.api_result.result_shape.rows.push_back(std::move(api_row));
        }
        return true;
      };
      for (std::size_t index = 0; index < result_rows; ++index) {
        if (cancellation_requested()) {
          return refuse("SB_EXECUTION_CANCELLED",
                        "streaming top-K cancelled during publication");
        }
        if (pending_page.rows.size() == kStreamingTopkPageRows &&
            !publish_page(false)) {
          return refuse("QOW-RESULT-DIAGNOSTIC-ABI-V1", stream_detail);
        }
        exec::DescriptorTuple tuple;
        if (!MaterializeCurrentHeapStreamingRow(
                binding, retained_rows[first_result + index], &candidate_values)) {
          return refuse("RESULT_SET.SHAPE_INVALID",
                        "streaming top-K result row is invalid");
        }
        tuple.values = candidate_values;
        pending_page.rows.push_back(std::move(tuple));
      }
      if (!publish_page(true)) {
        return refuse(cancellation_requested()
                          ? "SB_EXECUTION_CANCELLED"
                          : "QOW-RESULT-DIAGNOSTIC-ABI-V1",
                      stream_detail);
      }
      const auto result_authority =
          exec::RevalidateCanonicalExecutionMgaAuthority(
              *heap_registration.mga_authority, physical.physical_dag);
      if (!result_authority.ok) {
        return refuse(result_authority.diagnostic_code,
                      result_authority.detail);
      }
      result.physical_dag_executed = true;
      result.runtime_actuals_attached = true;
      result.canonical_result_published = true;
      result.canonical_result_column_count = ordered_outputs.size();
      result.canonical_result_row_count = result_rows;
      result.api_result.evidence.push_back(
          {"canonical.selected_plan", physical.physical_dag.selected_plan_uuid});
      result.api_result.evidence.push_back(
          {"canonical.heap_topk_streaming_mga", "complete"});
      result.api_result.evidence.push_back(
          {"canonical.heap_predicate_pushdown", "complete"});
      return result;
    }

    api::CanonicalOptimizerSelectedExecutionRequest selected;
    selected.selected_physical_dag = physical.physical_dag;
    selected.borrowed_mga_authority =
        heap_registration.mga_authority.get();
    selected.pre_access_statistics_snapshot_uuid =
        physical.physical_dag.statistics_snapshot_uuid;
    selected.available_executors.push_back(
        std::move(*heap_registration.registration));
    if (filter_composition) {
      CanonicalRelationalExpressionRuntimeServices filter_services;
      BindCanonicalPersistedRowDescriptorAuthorityForComposition(input.context,
                                                     &filter_services);
      selected.available_executors.push_back(
          MakeLiveHeapFilterRegistration(
              filter_node->bound_expression_ids.front(), filter_row_binding,
              {}, std::move(filter_services), filter_capability_uuid,
              maximum_output_rows, {},
              api::EngineCanonicalExpressionConsumer::filter,
              api::EnginePredicateConsumer::filter, &input.relational_dag,
              &input.context, selected.borrowed_mga_authority));
    }
    if (aggregate_composition) {
      const auto aggregate_profile =
          MatchLiveUnaryAggregateExpressionProfileForComposition(
              aggregate_node->semantic_variant_id);
      if (aggregate_profile.count_star) {
        selected.available_executors.push_back(
            MakeLiveCountStarRegistration(
                prepared_heap_aggregate.result_column,
                aggregate_capability_uuid, maximum_output_rows,
                input.context, &input.context,
                selected.borrowed_mga_authority));
      } else {
        selected.available_executors.push_back(
            MakeLiveAggregateRegistryRegistration(
                prepared_heap_aggregate, aggregate_capability_uuid,
                maximum_output_rows, 0, input.context, true));
      }
    }
    if (sort_composition) {
      selected.available_executors.push_back(
          MakeLiveSortRegistration(
              heap_descriptor_order_terms,
              DerivedCanonicalUuid(identity_scope + ":" + ordering_property_uuid,
                                   "heap-sort.deterministic-tie"),
              sort_capability_uuid, maximum_output_rows,
              std::max<std::size_t>(
                  1, static_cast<std::size_t>(maximum_pair_comparisons_u64)),
              {}, &input.context, selected.borrowed_mga_authority));
    }
    if (window_composition) {
      if (prepared_heap_aggregate_window.has_value() &&
          prepared_heap_aggregate_window_order_term.has_value()) {
        const auto maximum_quadratic_work = std::max<std::size_t>(
            1, bounded_size(maximum_pair_comparisons_u64));
        selected.available_executors.push_back(
            MakeLiveAggregateWindowRegistration(
                *prepared_heap_aggregate_window,
                *prepared_heap_aggregate_window_order_term,
                prepared_heap_aggregate_window_value_column,
                prepared_heap_aggregate_window_descriptor,
                prepared_heap_aggregate_window_frame_descriptor_uuid,
                heap_aggregate_window_order_term_binding_evidence_uuid,
                window_order_evidence_uuid,
                heap_aggregate_window_frame_property_binding_evidence_uuid,
                heap_aggregate_window_capability_uuid,
                maximum_output_rows, maximum_quadratic_work,
                maximum_quadratic_work, maximum_quadratic_work,
                input.context));
      } else if (prepared_heap_navigation_window.has_value() &&
          prepared_heap_navigation_order_term.has_value() &&
          prepared_heap_navigation_value_column.has_value()) {
        selected.available_executors.push_back(
            MakeLiveNavigationWindowRegistration(
                *prepared_heap_navigation_window,
                *prepared_heap_navigation_order_term,
                *prepared_heap_navigation_value_column,
                prepared_heap_navigation_nth_value_position_operand,
                prepared_heap_navigation_frame_descriptor_uuid,
                heap_navigation_order_term_binding_evidence_uuid,
                window_order_evidence_uuid,
                heap_navigation_frame_property_binding_evidence_uuid,
                window_capability_uuid,
                maximum_output_rows,
                std::max<std::size_t>(
                    1, static_cast<std::size_t>(
                           maximum_pair_comparisons_u64)),
                std::max<std::size_t>(
                    1, static_cast<std::size_t>(
                           maximum_pair_comparisons_u64)),
                prepared_heap_navigation_profile, input.context));
      } else if (prepared_heap_ntile.has_value() &&
          prepared_heap_ntile_order_term.has_value() &&
          prepared_heap_ntile_bucket_count_operand.has_value()) {
        selected.available_executors.push_back(
            MakeLiveNtileRegistration(
                *prepared_heap_ntile, *prepared_heap_ntile_order_term,
                *prepared_heap_ntile_bucket_count_operand,
                std::string(kGlobalNtileProfile.function_uuid),
                heap_ntile_order_term_binding_evidence_uuid,
                window_order_evidence_uuid, window_capability_uuid,
                maximum_output_rows, input.context));
      } else if (prepared_heap_peer_ranking.has_value() &&
          prepared_heap_peer_ranking_order_term.has_value()) {
        selected.available_executors.push_back(
            MakeLivePeerRankingRegistration(
                *prepared_heap_peer_ranking,
                *prepared_heap_peer_ranking_order_term,
                heap_peer_ranking_order_term_binding_evidence_uuid,
                window_order_evidence_uuid, window_capability_uuid,
                maximum_output_rows,
                std::max<std::size_t>(1, maximum_output_rows - 1),
                prepared_heap_peer_ranking_profile,
                input.context));
      } else {
        selected.available_executors.push_back(
            MakeLiveRowNumberRegistration(
                *prepared_heap_row_number, window_order_evidence_uuid,
                window_capability_uuid, maximum_output_rows, {},
                &input.context, selected.borrowed_mga_authority));
      }
    }
    if (project_composition) {
      selected.available_executors.push_back(
          MakeLiveHeapProjectRegistration(
              projected_columns, project_capability_uuid,
              maximum_output_rows, {}, &input.context,
              selected.borrowed_mga_authority));
    }
    if (cte_composition) {
      selected.available_executors.push_back(
          MakeLiveNonrecursiveCteRegistration(
              cte_implementation_id, cte_capability_uuid,
              maximum_output_rows, {}, &input.context,
              selected.borrowed_mga_authority));
    }
    if (limit_composition) {
      selected.available_executors.push_back(
          MakeLiveLimitRegistration(
              limit_implementation_id, limit_capability_uuid, row_limit,
              row_offset, false, maximum_output_rows, {}, &input.context,
              selected.borrowed_mga_authority));
    }
    selected.engine_execution_authorized = true;
    selected.result_publication_request.statement_uuid =
        input.context.statement_uuid.canonical;
    selected.result_publication_request.execution_attempt_uuid =
        DerivedCanonicalUuid(
            identity_scope + ":" + input.context.current_monotonic_ns,
            cte_is_root
                ? "heap-cte.execution-attempt"
                : limit_composition
                ? "heap-limit.execution-attempt"
                : (aggregate_composition
                       ? "heap-aggregate.execution-attempt"
                       : (project_composition
                       ? "heap-project.execution-attempt"
                       : (window_composition
                              ? "heap-window.execution-attempt"
                              : (sort_composition
                                     ? "heap-sort.execution-attempt"
                                     : "heap-filter.execution-attempt")))));
    selected.result_publication_request.transaction_effect_evidence_uuid =
        DerivedCanonicalUuid(
            identity_scope + ":" +
                std::to_string(input.context.local_transaction_id) + ":" +
            std::to_string(
                    input.context
                        .snapshot_visible_through_local_transaction_id),
            cte_is_root
                ? "heap-cte.transaction-effect-unchanged"
                : limit_composition
                ? "heap-limit.transaction-effect-unchanged"
                : (aggregate_composition
                       ? "heap-aggregate.transaction-effect-unchanged"
                       : (project_composition
                       ? "heap-project.transaction-effect-unchanged"
                       : (window_composition
                              ? "heap-window.transaction-effect-unchanged"
                              : (sort_composition
                                     ? "heap-sort.transaction-effect-unchanged"
                                     : "heap-filter.transaction-effect-unchanged")))));
    selected.result_publication_request.result_kind =
        exec::CanonicalResultKind::kRows;
    selected.result_publication_request.invocation_mode =
        exec::CanonicalResultInvocationMode::kDirect;
    for (std::size_t ordinal = 0; ordinal < ordered_outputs.size();
         ++ordinal) {
      const auto& output = *ordered_outputs[ordinal];
      const auto descriptor = std::ranges::find_if(
          dag.descriptors, [&](const auto& candidate) {
            return candidate.descriptor_id == output.descriptor_id;
          });
      if (descriptor == dag.descriptors.end()) {
        return refuse("QOW-DIAG-PACKET7-OBJECT-HEAP-BINDING-V1",
                      "object-backed heap descriptor is unresolved");
      }
      exec::CanonicalResultColumnDescriptor published;
      published.ordinal = static_cast<std::uint32_t>(ordinal);
      published.name_utf8 = output.output_name_utf8;
      published.descriptor_uuid = descriptor->descriptor_uuid;
      published.type_uuid = descriptor->type_uuid;
      published.nullability =
          descriptor->nullability == api::RelationalNullability::kNullable
              ? exec::CanonicalResultNullability::kNullable
              : exec::CanonicalResultNullability::kNonNull;
      published.collation_uuid = descriptor->collation_uuid;
      published.timezone_profile_id = descriptor->timezone_profile_id;
      selected.result_publication_request.column_bindings.push_back(
          {ordinal, true, std::move(published)});
    }
    selected.result_publication_request.maximum_row_count =
        maximum_output_rows;

    const auto execution = ExecuteSelectedCanonicalObjectFreeDag(
        input.context, selected, physical.ordinary_runtime_memory_receipts);
    if (!execution.accepted || !execution.exact_selected_nodes_executed ||
        !execution.causal_counters_attached ||
        !execution.canonical_result_published || !execution.issues.empty()) {
      return refuse(
          execution.issues.empty()
              ? (cte_is_root
                     ? "QOW-DIAG-PACKET7-OBJECT-HEAP-CTE-EXECUTION-V1"
                     : (limit_composition
                     ? "QOW-DIAG-PACKET7-OBJECT-HEAP-LIMIT-EXECUTION-V1"
                     : (aggregate_composition
                            ? "QOW-DIAG-PACKET7-OBJECT-HEAP-AGGREGATE-EXECUTION-V1"
                            : (project_composition
                            ? "QOW-DIAG-PACKET7-OBJECT-HEAP-PROJECT-EXECUTION-V1"
                            : (window_composition
                                   ? "QOW-DIAG-PACKET7-OBJECT-HEAP-WINDOW-EXECUTION-V1"
                                   : (sort_composition
                                          ? "QOW-DIAG-PACKET7-OBJECT-HEAP-SORT-EXECUTION-V1"
                                          : "QOW-DIAG-PACKET7-OBJECT-HEAP-FILTER-EXECUTION-V1"))))))
              : execution.issues.front().diagnostic_id,
          execution.issues.empty()
              ? (cte_is_root
                     ? "object-backed heap nonrecursive CTE selected DAG was not completed"
                     : (limit_composition
                     ? "object-backed heap LIMIT selected DAG was not completed"
                     : (aggregate_composition
                            ? "object-backed heap aggregate selected DAG was not completed"
                            : (project_composition
                            ? "object-backed heap PROJECT selected DAG was not completed"
                            : (window_composition
                                   ? (prepared_heap_peer_ranking.has_value()
                                          ? "object-backed heap " +
                                                std::string(
                                                    prepared_heap_peer_ranking_profile
                                                        .display_name) +
                                                " selected DAG was not completed"
                                          : "object-backed heap ROW_NUMBER selected DAG was not completed")
                                   : (sort_composition
                                          ? "object-backed heap ORDER BY selected DAG was not completed"
                                          : "object-backed heap WHERE selected DAG was not completed"))))))
              : execution.issues.front().field_id);
    }
    result.physical_dag_executed = true;
    result.runtime_actuals_attached = execution.runtime_actuals.accepted;
    result.canonical_result_published =
        execution.result_publication.published;
    result.canonical_result_column_count =
        execution.result_publication.envelope.column_descriptors.size();
    result.canonical_result_row_count =
        execution.result_publication.row_stream.rows.size();
    result.canonical_result_bytes =
        execution.result_publication.canonical_envelope_bytes;
    result.api_result = SuccessfulApiResult(planning_request, execution);
    return result;
  }

  api::CanonicalHeapOptimizerSelectedExecutionRequest execution_request;
  execution_request.context = input.context;
  execution_request.relational_dag = input.relational_dag;
  execution_request.selected_physical_dag = physical.physical_dag;
  execution_request.maximum_scanned_row_versions =
      maximum_scanned_row_versions;
  execution_request.maximum_decoded_bytes = maximum_decoded_bytes;
  execution_request.maximum_output_rows = maximum_output_rows;
  execution_request.maximum_output_columns = maximum_output_columns;
  execution_request.maximum_output_cells =
      maximum_output_rows * maximum_output_columns;
  execution_request.cancellation_requested =
      input.context.query_cancellation_requested
          ? input.context.query_cancellation_requested
          : std::function<bool()>([] { return false; });
  execution_request.execution_attempt_uuid = DerivedCanonicalUuid(
      identity_scope + ":" + input.context.current_monotonic_ns,
      "heap-scan.execution-attempt");
  execution_request.transaction_effect_evidence_uuid = DerivedCanonicalUuid(
      identity_scope + ":" +
          std::to_string(input.context.local_transaction_id) + ":" +
          std::to_string(
              input.context.snapshot_visible_through_local_transaction_id),
      "heap-scan.transaction-effect-unchanged");
  execution_request.authority_cohort = admission.authority_cohort;

  const auto execution =
      api::ExecuteCanonicalHeapOptimizerSelectedDag(execution_request);
  if (!execution.accepted || !execution.exact_selected_nodes_executed ||
      !execution.causal_counters_attached ||
      !execution.canonical_result_published || !execution.issues.empty()) {
    return refuse(
        execution.issues.empty()
            ? "QOW-DIAG-PACKET7-OBJECT-HEAP-EXECUTION-V1"
            : execution.issues.front().diagnostic_id,
        execution.issues.empty()
            ? "object-backed heap selected DAG was not completed"
            : execution.issues.front().field_id);
  }
  result.physical_dag_executed = true;
  result.runtime_actuals_attached = execution.runtime_actuals.accepted;
  result.canonical_result_published = execution.result_publication.published;
  result.canonical_result_column_count =
      execution.result_publication.envelope.column_descriptors.size();
  result.canonical_result_row_count =
      execution.result_publication.row_stream.rows.size();
  result.canonical_result_bytes =
      execution.result_publication.canonical_envelope_bytes;
  result.api_result = SuccessfulApiResult(planning_request, execution);
  return result;
}

}  // namespace scratchbird::engine::sblr

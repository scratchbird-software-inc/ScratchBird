// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "canonical_query_pivot_composition.hpp"

#include "canonical_query_aggregate_registration.hpp"
#include "canonical_query_descriptor_support.hpp"
#include "canonical_query_object_free_composition_support.hpp"
#include "canonical_query_object_free_profile.hpp"
#include "canonical_query_physical_registration.hpp"
#include "canonical_query_runtime_memory_support.hpp"
#include "canonical_query_scalar_support.hpp"
#include "canonical_relational_expression.hpp"

#include "engine/executor/descriptor_value_runtime.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <ranges>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace scratchbird::engine::sblr {

namespace api = scratchbird::engine::internal_api;
namespace exec = scratchbird::engine::executor;
namespace plan = scratchbird::engine::planner;

namespace {

constexpr std::string_view kValuesImplementationId =
    "values.materialize.canonical.v1";
constexpr std::uint64_t kCanonicalAggregateKernelBaseMemoryBytes = 1024;

bool CanonicalPivotCausalStructureExactlyMatches(
    const exec::CanonicalPivotRequest& request,
    const exec::CanonicalPivotResult& result) {
  std::vector<std::size_t> representative_rows;
  std::size_t matched_input_row_count = 0;
  std::size_t key_comparison_count = 0;
  try {
    representative_rows.reserve(request.input_batch.rows.size());
    const auto terms_equal = [&](const exec::DescriptorTuple& left,
                                 const exec::DescriptorTuple& right,
                                 const auto& terms,
                                 bool* equal) {
      if (equal == nullptr) return false;
      *equal = true;
      for (const auto& term : terms) {
        if (term.column >= left.values.size() ||
            term.column >= right.values.size() ||
            key_comparison_count ==
                request.maximum_key_comparison_count) {
          return false;
        }
        ++key_comparison_count;
        const auto compared = exec::CompareCanonicalDescriptorOrderValues(
            left.values[term.column], right.values[term.column], term);
        if (!compared.diagnostic.ok) return false;
        if (compared.comparison != 0) {
          *equal = false;
          return true;
        }
      }
      return true;
    };
    for (std::size_t row = 0; row < request.input_batch.rows.size(); ++row) {
      bool found_group = false;
      for (const auto representative : representative_rows) {
        bool equal = false;
        if (!terms_equal(request.input_batch.rows[row],
                         request.input_batch.rows[representative],
                         request.group_key_terms, &equal)) {
          return false;
        }
        if (equal) {
          found_group = true;
          break;
        }
      }
      if (!found_group) representative_rows.push_back(row);

      std::optional<std::size_t> matched_item;
      for (std::size_t item = 0; item < request.in_items.size(); ++item) {
        if (request.in_items[item].values.size() !=
            request.for_key_terms.size()) {
          return false;
        }
        bool matches = true;
        for (std::size_t key = 0;
             key < request.for_key_terms.size(); ++key) {
          const auto& term = request.for_key_terms[key];
          if (term.column >= request.input_batch.rows[row].values.size()) {
            return false;
          }
          const auto& value =
              request.input_batch.rows[row].values[term.column];
          if (request.null_policy ==
                  exec::CanonicalPivotNullPolicy::kExclude &&
              value.state == api::EngineValueState::sql_null) {
            matches = false;
            break;
          }
          if (key_comparison_count ==
              request.maximum_key_comparison_count) {
            return false;
          }
          ++key_comparison_count;
          const auto compared = exec::CompareCanonicalDescriptorOrderValues(
              value, request.in_items[item].values[key], term);
          if (!compared.diagnostic.ok) return false;
          if (compared.comparison != 0) {
            matches = false;
            break;
          }
        }
        if (!matches) continue;
        if (matched_item.has_value()) return false;
        matched_item = item;
      }
      if (matched_item.has_value()) ++matched_input_row_count;
    }
  } catch (const std::bad_alloc&) {
    return false;
  } catch (const std::length_error&) {
    return false;
  }
  if (representative_rows.size() != result.output_batch.rows.size() ||
      representative_rows.size() != result.group_count ||
      matched_input_row_count != result.matched_input_row_count ||
      key_comparison_count != result.key_comparison_count) {
    return false;
  }
  for (std::size_t group = 0; group < representative_rows.size(); ++group) {
    const auto& source =
        request.input_batch.rows[representative_rows[group]];
    const auto& output = result.output_batch.rows[group];
    if (output.values.size() != request.result_columns.size()) return false;
    for (std::size_t key = 0;
         key < request.group_key_terms.size(); ++key) {
      const auto source_column = request.group_key_terms[key].column;
      if (source_column >= source.values.size() ||
          !CanonicalQueryEngineDescriptorExactlyEqual(
              output.values[key].descriptor,
              request.result_columns[key].descriptor) ||
          !CanonicalQueryTypedValuePayloadExactlyEqual(
              output.values[key], source.values[source_column])) {
        return false;
      }
    }
  }
  return true;
}

bool CanonicalPivotExecutionReceiptMatches(
    const exec::CanonicalPivotRequest& request,
    const exec::PhysicalNodeRecord& node,
    const exec::CanonicalPivotResult& result) {
  if (!CanonicalOperatorExecutionReceiptMatches(
          result, request.physical_dag, node,
          request.mga_authority.statement_context) ||
      node.memory_bytes_required == 0 ||
      node.memory_bytes_required >
          static_cast<std::uint64_t>(
              std::numeric_limits<std::size_t>::max()) ||
      result.input_row_count != request.input_batch.rows.size() ||
      result.group_count != result.output_batch.rows.size() ||
      result.group_count > request.maximum_output_row_count ||
      result.group_count > result.input_row_count ||
      result.in_item_count != request.in_items.size() ||
      result.aggregate_count != request.aggregates.size() ||
      result.matched_input_row_count > result.input_row_count ||
      result.key_comparison_count >
          request.maximum_key_comparison_count ||
      result.aggregate_transition_count >
          request.maximum_total_aggregate_transition_count ||
      (request.maximum_combined_final_output_bytes != 0 &&
       result.combined_final_output_bytes >
           request.maximum_combined_final_output_bytes) ||
      result.output_batch.rows.size() >
          request.maximum_output_row_count ||
      result.output_batch.rows.size() != result.group_count) {
    return false;
  }
  if (result.matched_input_row_count != 0 &&
      result.aggregate_count >
          std::numeric_limits<std::size_t>::max() /
              result.matched_input_row_count) {
    return false;
  }
  if (result.aggregate_transition_count >
      result.matched_input_row_count * result.aggregate_count) {
    return false;
  }
  if (result.output_batch.rows.size() != 0 &&
      request.result_columns.size() >
          std::numeric_limits<std::size_t>::max() /
              result.output_batch.rows.size()) {
    return false;
  }
  const auto output_cell_count =
      result.output_batch.rows.size() * request.result_columns.size();
  if (output_cell_count > request.maximum_output_cell_count) return false;
  std::size_t maximum_finalization_workspace_bytes = 0;
  for (const auto& binding : request.aggregates) {
    const auto bound =
        binding.aggregate_template.maximum_finalization_workspace_bytes == 0
            ? node.memory_bytes_required
            : std::min<std::uint64_t>(
                  binding.aggregate_template
                      .maximum_finalization_workspace_bytes,
                  node.memory_bytes_required);
    maximum_finalization_workspace_bytes = std::max<std::size_t>(
        maximum_finalization_workspace_bytes,
        static_cast<std::size_t>(bound));
  }
  if (result.peak_finalization_workspace_bytes >
      maximum_finalization_workspace_bytes) {
    return false;
  }
  const auto output_validation = exec::ValidateCanonicalDescriptorBatch(
      result.output_batch, node.output_descriptor_ids);
  return output_validation.ok &&
         CanonicalPivotCausalStructureExactlyMatches(request, result);
}

bool CanonicalUnpivotOutputExactlyMatches(
    const exec::CanonicalUnpivotRequest& request,
    const exec::DescriptorBatch& output_batch) {
  const auto value_column_count = request.in_items.empty()
                                      ? 0
                                      : request.in_items.front()
                                            .source_columns.size();
  const auto expected_width =
      request.group_columns.size() + 1 + value_column_count;
  if (value_column_count == 0 ||
      request.result_columns.size() != expected_width) {
    return false;
  }
  std::size_t output_row = 0;
  for (const auto& input_row : request.input_batch.rows) {
    for (const auto& item : request.in_items) {
      if (item.source_columns.size() != value_column_count) return false;
      const bool all_null = std::ranges::all_of(
          item.source_columns, [&](const auto column) {
            return column < input_row.values.size() &&
                   input_row.values[column].state ==
                       api::EngineValueState::sql_null;
          });
      if (request.null_policy == exec::CanonicalPivotNullPolicy::kExclude &&
          all_null) {
        continue;
      }
      if (output_row >= output_batch.rows.size() ||
          output_batch.rows[output_row].values.size() != expected_width) {
        return false;
      }
      const auto& actual = output_batch.rows[output_row];
      for (std::size_t group = 0;
           group < request.group_columns.size(); ++group) {
        if (request.group_columns[group] >= input_row.values.size()) {
          return false;
        }
        exec::DescriptorRuntimeDiagnostic diagnostic;
        const auto expected = exec::CastDescriptorValue(
            input_row.values[request.group_columns[group]],
            request.result_columns[group].descriptor, &diagnostic);
        if (!diagnostic.ok ||
            !CanonicalQueryEngineDescriptorExactlyEqual(
                expected.descriptor, actual.values[group].descriptor) ||
            !CanonicalQueryTypedValuePayloadExactlyEqual(
                expected, actual.values[group])) {
          return false;
        }
      }
      exec::DescriptorRuntimeDiagnostic label_diagnostic;
      const auto label_column = request.group_columns.size();
      const auto expected_label = exec::CastDescriptorValue(
          item.pivot_value,
          request.result_columns[label_column].descriptor,
          &label_diagnostic);
      if (!label_diagnostic.ok ||
          !CanonicalQueryEngineDescriptorExactlyEqual(
              expected_label.descriptor,
              actual.values[label_column].descriptor) ||
          !CanonicalQueryTypedValuePayloadExactlyEqual(
              expected_label, actual.values[label_column])) {
        return false;
      }
      for (std::size_t value = 0; value < value_column_count; ++value) {
        if (item.source_columns[value] >= input_row.values.size()) {
          return false;
        }
        const auto result_column = label_column + 1 + value;
        exec::DescriptorRuntimeDiagnostic diagnostic;
        const auto expected = exec::CastDescriptorValue(
            input_row.values[item.source_columns[value]],
            request.result_columns[result_column].descriptor,
            &diagnostic);
        if (!diagnostic.ok ||
            !CanonicalQueryEngineDescriptorExactlyEqual(
                expected.descriptor,
                actual.values[result_column].descriptor) ||
            !CanonicalQueryTypedValuePayloadExactlyEqual(
                expected, actual.values[result_column])) {
          return false;
        }
      }
      ++output_row;
    }
  }
  return output_row == output_batch.rows.size();
}

bool CanonicalUnpivotExecutionReceiptMatches(
    const exec::CanonicalUnpivotRequest& request,
    const exec::PhysicalNodeRecord& node,
    const exec::CanonicalUnpivotResult& result) {
  if (!CanonicalOperatorExecutionReceiptMatches(
          result, request.physical_dag, node,
          request.mga_authority.statement_context) ||
      result.input_row_count != request.input_batch.rows.size() ||
      result.in_item_count != request.in_items.size() ||
      result.emitted_row_count != result.output_batch.rows.size() ||
      result.emitted_row_count > request.maximum_output_row_count) {
    return false;
  }
  if (result.input_row_count != 0 &&
      result.in_item_count >
          std::numeric_limits<std::size_t>::max() /
              result.input_row_count) {
    return false;
  }
  const auto candidate_row_count =
      result.input_row_count * result.in_item_count;
  std::size_t expected_null_excluded_row_count = 0;
  if (request.null_policy == exec::CanonicalPivotNullPolicy::kExclude) {
    for (const auto& input_row : request.input_batch.rows) {
      for (const auto& item : request.in_items) {
        if (std::ranges::all_of(
                item.source_columns, [&](const auto column) {
                  return column < input_row.values.size() &&
                         input_row.values[column].state ==
                             api::EngineValueState::sql_null;
                })) {
          ++expected_null_excluded_row_count;
        }
      }
    }
  }
  if (result.emitted_row_count > candidate_row_count ||
      result.null_excluded_row_count >
          candidate_row_count - result.emitted_row_count ||
      result.emitted_row_count + result.null_excluded_row_count !=
          candidate_row_count ||
      result.null_excluded_row_count !=
          expected_null_excluded_row_count) {
    return false;
  }
  if (result.output_batch.rows.size() != 0 &&
      request.result_columns.size() >
          std::numeric_limits<std::size_t>::max() /
              result.output_batch.rows.size()) {
    return false;
  }
  const auto output_cell_count =
      result.output_batch.rows.size() * request.result_columns.size();
  if (output_cell_count > request.maximum_output_cell_count) return false;
  const auto output_validation = exec::ValidateCanonicalDescriptorBatch(
      result.output_batch, node.output_descriptor_ids);
  return output_validation.ok &&
         CanonicalUnpivotOutputExactlyMatches(request,
                                              result.output_batch);
}

}  // namespace

// SEARCH_KEY: SB_ENGINE_CANONICAL_QUERY_PIVOT_COMPOSITION_AUTHORITY
// Coordinates already-admitted object-free PIVOT/UNPIVOT routes. It consumes
// engine-selected MGA statement context and cannot create a snapshot, access
// storage, or publish transaction finality.

// QOW-SOURCE-QRY-019-PIVOT-LIVE-V1
// The bound carrier is item-major: group identifiers, one FOR identifier,
// one aggregate expression per (IN item, aggregate), then one fixed literal
// per IN item.  This keeps parser syntax out of execution while allowing the
// selected physical node to consume an arbitrary aggregate list.
CanonicalObjectFreeValuesExecutionResult
ExecuteCanonicalObjectFreePivotQuery(
    const CanonicalObjectFreeValuesExecutionRequest& request) {
  CanonicalObjectFreeValuesExecutionResult result;
  const auto& graph = request.optimizer_request.logical_graph;
  const auto root = std::ranges::find_if(graph.nodes, [&](const auto& node) {
    return node.logical_node_id == graph.root_logical_node_id;
  });
  if (graph.nodes.size() != 2 || root == graph.nodes.end() ||
      root->node_kind != plan::CanonicalLogicalRelationalNodeKind::kPivot ||
      root->input_logical_node_ids.size() != 1 ||
      !request.optimizer_request.logical_properties.properties.empty()) {
    return result;
  }
  const bool include_nulls =
      root->semantic_variant_id ==
      "pivot.fixed-aggregate-list-one-for.include-nulls.v1";
  const bool exclude_nulls =
      root->semantic_variant_id ==
      "pivot.fixed-aggregate-list-one-for.exclude-nulls.v1";
  if (!include_nulls && !exclude_nulls) return result;
  const auto input_node =
      std::ranges::find_if(graph.nodes, [&](const auto& node) {
        return node.logical_node_id == root->input_logical_node_ids.front();
      });
  if (input_node == graph.nodes.end() || input_node == root ||
      input_node->node_kind !=
          plan::CanonicalLogicalRelationalNodeKind::kValues ||
      input_node->semantic_variant_id != "values.literal-table.v1" ||
      !input_node->input_logical_node_ids.empty()) {
    return result;
  }
  for (const auto& node : graph.nodes) {
    if (!node.required_object_uuids.empty()) return result;
  }

  result.profile_matched = true;
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
    result.api_result =
        Failure(request, std::move(diagnostic_id), std::move(detail));
    return result;
  };
  if (!request.optimizer_admission.admitted ||
      !request.optimizer_admission.planning_allowed) {
    return refuse("QOW-DIAG-RELATIONAL-LIVE-PIVOT-ADMISSION-V1",
                  "PIVOT lacks optimizer admission");
  }

  auto input = MaterializeValues(request.relational_dag, *input_node,
                                 request.expression_services);
  if (!input.ok) {
    return refuse("QOW-DIAG-RELATIONAL-LIVE-PIVOT-PAYLOAD-V1",
                  "PIVOT input VALUES: " + input.detail);
  }
  std::vector<const api::RelationalOutputRecord*> outputs;
  for (const auto& output : request.relational_dag.outputs) {
    if (output.relation_node_id == root->logical_node_id) {
      outputs.push_back(&output);
    }
  }
  std::ranges::sort(outputs, [](const auto* left, const auto* right) {
    return left->ordinal < right->ordinal;
  });
  const auto find_expression = [&](const std::uint32_t expression_id) {
    return std::ranges::find_if(
        request.relational_dag.expressions, [&](const auto& expression) {
          return expression.expression_id == expression_id;
        });
  };
  if (outputs.size() != root->output_descriptor_ids.size() ||
      outputs.size() < 2 || root->bound_expression_ids.size() <=
                                outputs.size() + 1) {
    return refuse("QOW-DIAG-RELATIONAL-LIVE-PIVOT-PAYLOAD-V1",
                  "PIVOT output or bound-expression coverage is incomplete");
  }
  std::size_t group_count = 0;
  while (group_count < outputs.size()) {
    const auto expression = find_expression(outputs[group_count]->expression_id);
    if (expression == request.relational_dag.expressions.end() ||
        expression->expression_kind !=
            api::RelationalExpressionKind::kIdentifier) {
      break;
    }
    ++group_count;
  }
  const auto aggregate_output_count = outputs.size() - group_count;
  const auto item_count =
      root->bound_expression_ids.size() - outputs.size() - 1;
  if (group_count == 0 || item_count == 0 || aggregate_output_count == 0 ||
      aggregate_output_count % item_count != 0) {
    return refuse("QOW-DIAG-RELATIONAL-LIVE-PIVOT-PAYLOAD-V1",
                  "PIVOT group, aggregate, or fixed IN arity is unresolved");
  }
  const auto aggregate_count = aggregate_output_count / item_count;
  const auto map_identifier = [&](const std::uint32_t expression_id,
                                  std::size_t* column,
                                  std::string* detail) {
    const auto expression = find_expression(expression_id);
    if (expression == request.relational_dag.expressions.end() ||
        expression->expression_kind !=
            api::RelationalExpressionKind::kIdentifier ||
        !expression->child_expression_ids.empty() ||
        !expression->bound_name_uuid.has_value() ||
        expression->function_uuid.has_value() ||
        expression->literal_kind.has_value() ||
        expression->operator_name.has_value() ||
        expression->literal_or_parameter_ref.has_value()) {
      *detail = "PIVOT key is not an exact bound identifier";
      return false;
    }
    const auto descriptor = std::ranges::find(
        input_node->output_descriptor_ids, expression->result_descriptor_id);
    if (descriptor == input_node->output_descriptor_ids.end() ||
        std::ranges::count(input_node->output_descriptor_ids,
                           expression->result_descriptor_id) != 1) {
      *detail = "PIVOT identifier is not uniquely supplied by VALUES";
      return false;
    }
    *column = static_cast<std::size_t>(std::distance(
        input_node->output_descriptor_ids.begin(), descriptor));
    if (*column >= input.batch.columns.size() ||
        input.batch.columns[*column].descriptor_id !=
            expression->result_descriptor_id) {
      *detail = "PIVOT identifier ordinal is not descriptor-exact";
      return false;
    }
    return true;
  };

  std::vector<exec::CanonicalDescriptorOrderTerm> group_terms;
  std::vector<std::size_t> group_columns;
  std::string detail;
  for (std::size_t group = 0; group < group_count; ++group) {
    if (outputs[group]->ordinal != group || !outputs[group]->visible ||
        outputs[group]->descriptor_id != root->output_descriptor_ids[group] ||
        outputs[group]->expression_id != root->bound_expression_ids[group]) {
      return refuse("QOW-DIAG-RELATIONAL-LIVE-PIVOT-PAYLOAD-V1",
                    "PIVOT group output lineage is not exact");
    }
    std::size_t column = 0;
    if (!map_identifier(root->bound_expression_ids[group], &column, &detail)) {
      return refuse("QOW-DIAG-RELATIONAL-LIVE-PIVOT-PAYLOAD-V1", detail);
    }
    exec::CanonicalDescriptorOrderTerm term;
    term.column = column;
    term.expression_descriptor_id = input.batch.columns[column].descriptor_id;
    const auto validation = exec::ValidateCanonicalDescriptorOrderTerm(
        term, input.batch.columns[column]);
    if (!validation.ok) {
      return refuse("QOW-DIAG-RELATIONAL-LIVE-PIVOT-PAYLOAD-V1",
                    validation.diagnostic_code + ":" + validation.detail);
    }
    group_columns.push_back(column);
    group_terms.push_back(std::move(term));
  }
  std::size_t for_column = 0;
  if (!map_identifier(root->bound_expression_ids[group_count], &for_column,
                      &detail) ||
      std::ranges::find(group_columns, for_column) != group_columns.end()) {
    return refuse("QOW-DIAG-RELATIONAL-LIVE-PIVOT-PAYLOAD-V1",
                  detail.empty() ? "PIVOT FOR key overlaps its group keys"
                                 : detail);
  }
  exec::CanonicalDescriptorOrderTerm for_term;
  for_term.column = for_column;
  for_term.expression_descriptor_id =
      input.batch.columns[for_column].descriptor_id;
  const auto for_validation = exec::ValidateCanonicalDescriptorOrderTerm(
      for_term, input.batch.columns[for_column]);
  if (!for_validation.ok) {
    return refuse("QOW-DIAG-RELATIONAL-LIVE-PIVOT-PAYLOAD-V1",
                  for_validation.diagnostic_code + ":" +
                      for_validation.detail);
  }

  const auto aggregate_expression_offset = group_count + 1;
  for (std::size_t output = 0; output < aggregate_output_count; ++output) {
    if (outputs[group_count + output]->ordinal != group_count + output ||
        !outputs[group_count + output]->visible ||
        outputs[group_count + output]->descriptor_id !=
            root->output_descriptor_ids[group_count + output] ||
        outputs[group_count + output]->expression_id !=
            root->bound_expression_ids[aggregate_expression_offset + output]) {
      return refuse("QOW-DIAG-RELATIONAL-LIVE-PIVOT-PAYLOAD-V1",
                    "PIVOT aggregate output lineage is not exact");
    }
  }

  std::vector<std::vector<PreparedGlobalAggregateRoot>> prepared_aggregates(
      aggregate_count);
  for (std::size_t aggregate = 0; aggregate < aggregate_count; ++aggregate) {
    const exec::CanonicalAggregateRegistryEntry* expected_registry = nullptr;
    for (std::size_t item = 0; item < item_count; ++item) {
      const auto output_ordinal =
          group_count + item * aggregate_count + aggregate;
      const auto expression_id =
          root->bound_expression_ids[aggregate_expression_offset +
                                     item * aggregate_count + aggregate];
      const auto expression = find_expression(expression_id);
      const auto* registry =
          expression == request.relational_dag.expressions.end() ||
                  !expression->function_uuid.has_value()
              ? nullptr
              : exec::LookupCanonicalAggregateByUuidV1(
                    *expression->function_uuid);
      if (registry == nullptr || !registry->executable ||
          (expected_registry != nullptr &&
           registry->function != expected_registry->function)) {
        return refuse("QOW-DIAG-RELATIONAL-LIVE-PIVOT-PAYLOAD-V1",
                      "PIVOT aggregate identity differs across fixed IN items");
      }
      expected_registry = registry;
      const bool count_star =
          registry->function == exec::CanonicalAggregateFunction::count &&
          expression->child_expression_ids.empty();
      auto aggregate_root = *root;
      aggregate_root.output_descriptor_ids = {
          root->output_descriptor_ids[output_ordinal]};
      aggregate_root.bound_expression_ids = {expression_id};
      auto prepared = PrepareGlobalAggregateRootForComposition(
          request.relational_dag, aggregate_root, *input_node, input,
          registry->function, count_star, false, false,
          static_cast<std::uint32_t>(output_ordinal), true);
      if (!prepared.ok) {
        return refuse("QOW-DIAG-RELATIONAL-LIVE-PIVOT-PAYLOAD-V1",
                      prepared.detail);
      }
      if (!prepared_aggregates[aggregate].empty()) {
        const auto& first = prepared_aggregates[aggregate].front();
        if (prepared.aggregate_descriptor.function !=
                first.aggregate_descriptor.function ||
            prepared.aggregate_descriptor.count_star !=
                first.aggregate_descriptor.count_star ||
            prepared.value_columns != first.value_columns ||
            prepared.value_descriptor_ids != first.value_descriptor_ids ||
            prepared.distinct != first.distinct) {
          return refuse(
              "QOW-DIAG-RELATIONAL-LIVE-PIVOT-PAYLOAD-V1",
              "PIVOT aggregate binding differs across fixed IN items");
        }
      }
      prepared_aggregates[aggregate].push_back(std::move(prepared));
    }
  }

  CanonicalRelationalExpressionRuntime expression_runtime(
      request.relational_dag, request.expression_services);
  std::vector<exec::CanonicalPivotInItem> in_items;
  const auto literal_offset =
      aggregate_expression_offset + aggregate_output_count;
  for (std::size_t item = 0; item < item_count; ++item) {
    const auto expression_id = root->bound_expression_ids[literal_offset + item];
    const auto expression = find_expression(expression_id);
    api::EngineTypedValue value;
    std::string literal_detail;
    if (expression == request.relational_dag.expressions.end() ||
        expression->expression_kind !=
            api::RelationalExpressionKind::kLiteral ||
        !expression->child_expression_ids.empty() ||
        expression->bound_name_uuid.has_value() ||
        expression->function_uuid.has_value() ||
        !expression->literal_kind.has_value() ||
        expression->operator_name.has_value() ||
        !expression->literal_or_parameter_ref.has_value() ||
        !expression_runtime.EvaluateForConsumer(
            expression_id,
            input.batch.columns[for_column].descriptor.canonical_type_name,
            api::EngineCanonicalExpressionConsumer::aggregate, &value,
            &literal_detail)) {
      return refuse("QOW-DIAG-RELATIONAL-LIVE-PIVOT-PAYLOAD-V1",
                    "PIVOT fixed IN item is not a canonical literal: " +
                        literal_detail);
    }
    in_items.push_back({{std::move(value)}});
  }

  std::vector<exec::ExecutorColumnDescriptor> result_columns;
  std::vector<exec::CanonicalResultColumnBinding> result_bindings;
  result_columns.reserve(outputs.size());
  result_bindings.reserve(outputs.size());
  for (std::size_t group = 0; group < group_count; ++group) {
    auto column = input.batch.columns[group_columns[group]];
    column.stable_name = outputs[group]->output_name_utf8;
    column.descriptor_id = outputs[group]->descriptor_id;
    result_columns.push_back(std::move(column));
    auto binding = input.result_bindings[group_columns[group]];
    binding.physical_column_ordinal = group;
    if (!binding.published_descriptor.has_value()) {
      return refuse("QOW-DIAG-RELATIONAL-LIVE-PIVOT-PAYLOAD-V1",
                    "PIVOT group result descriptor is not publishable");
    }
    binding.published_descriptor->ordinal = static_cast<std::uint32_t>(group);
    binding.published_descriptor->name_utf8 = outputs[group]->output_name_utf8;
    result_bindings.push_back(std::move(binding));
  }
  for (std::size_t item = 0; item < item_count; ++item) {
    for (std::size_t aggregate = 0; aggregate < aggregate_count; ++aggregate) {
      result_columns.push_back(
          prepared_aggregates[aggregate][item].result_column);
      result_bindings.push_back(
          prepared_aggregates[aggregate][item].result_bindings.front());
    }
  }
  std::vector<exec::CanonicalPivotAggregateBinding> aggregate_bindings;
  aggregate_bindings.reserve(aggregate_count);
  for (std::size_t aggregate = 0; aggregate < aggregate_count; ++aggregate) {
    const auto& prepared = prepared_aggregates[aggregate].front();
    exec::CanonicalPivotAggregateBinding binding;
    binding.aggregate_template.descriptor = prepared.aggregate_descriptor;
    binding.aggregate_template.value_columns = prepared.value_columns;
    binding.aggregate_template.value_expression_descriptor_ids =
        prepared.value_descriptor_ids;
    binding.aggregate_template.direct_arguments = prepared.direct_arguments;
    binding.aggregate_template.distinct = prepared.distinct;
    binding.aggregate_template.aggregate_order_terms =
        prepared.aggregate_order_terms;
    binding.aggregate_template.aggregate_separator =
        prepared.aggregate_separator;
    binding.aggregate_template.listagg_overflow_mode =
        prepared.listagg_overflow_mode;
    binding.aggregate_template.listagg_max_output_bytes =
        prepared.listagg_max_output_bytes;
    binding.aggregate_template.listagg_truncation_indicator =
        prepared.listagg_truncation_indicator;
    binding.aggregate_template.listagg_with_count = prepared.listagg_with_count;
    binding.aggregate_template.forced_strategy =
        exec::CanonicalAggregateExecutionStrategy::serial;
    for (std::size_t item = 0; item < item_count; ++item) {
      binding.result_columns_by_item.push_back(
          prepared_aggregates[aggregate][item].result_column);
    }
    aggregate_bindings.push_back(std::move(binding));
  }

  const auto input_row_count = input.batch.rows.size();
  std::uint64_t input_memory = 1;
  std::uint64_t output_cells = 0;
  std::uint64_t output_memory = 0;
  std::uint64_t total_memory = 0;
  std::uint64_t group_comparisons = 0;
  std::uint64_t item_comparisons = 0;
  std::uint64_t maximum_key_comparisons = 0;
  std::uint64_t maximum_transitions = 0;
  std::uint64_t aggregate_workspace_memory = 0;
  if (!AddBatchMemoryBytes(input.batch, &input_memory) ||
      !CheckedMultiply(input_row_count, outputs.size(), &output_cells) ||
      !CheckedMultiply(output_cells, 64U, &output_memory) ||
      !CheckedAdd(input_memory, output_memory, &total_memory) ||
      !CheckedAdd(total_memory, input_memory, &total_memory) ||
      !CheckedMultiply(input_row_count, sizeof(std::size_t),
                       &aggregate_workspace_memory) ||
      !CheckedAdd(total_memory, aggregate_workspace_memory, &total_memory) ||
      !CheckedAdd(total_memory,
                  kCanonicalAggregateKernelBaseMemoryBytes,
                  &total_memory) ||
      !CheckedMultiply(input_row_count, input_row_count,
                       &group_comparisons) ||
      !CheckedMultiply(group_comparisons, group_count,
                       &group_comparisons) ||
      !CheckedMultiply(input_row_count, item_count, &item_comparisons) ||
      !CheckedAdd(group_comparisons, item_comparisons,
                  &maximum_key_comparisons) ||
      !CheckedMultiply(input_row_count, aggregate_count,
                       &maximum_transitions) ||
      total_memory > request.optimizer_request.resource.memory_budget_bytes ||
      maximum_key_comparisons > std::numeric_limits<std::size_t>::max() ||
      maximum_transitions > std::numeric_limits<std::size_t>::max() ||
      output_cells > std::numeric_limits<std::size_t>::max()) {
    return refuse("QOW-DIAG-OPTIMIZER-SEARCH-COST-VECTOR-V1",
                  "PIVOT live cost or resource bound is invalid");
  }

  const auto identity_scope = graph.bound_sblr_tree_uuid + ":" +
                              request.context.statement_uuid.canonical;
  const auto values_capability_uuid =
      DerivedCanonicalUuid(identity_scope, "values.capability");
  const auto pivot_capability_uuid =
      DerivedCanonicalUuid(identity_scope, "pivot.capability");
  const std::string pivot_implementation_id =
      include_nulls ? "pivot.canonical.include-nulls.typed.v1"
                    : "pivot.canonical.exclude-nulls.typed.v1";
  std::vector<LivePhysicalNodeProfile> profiles = {
      {input_node->logical_node_id,
       std::string(kValuesImplementationId),
       values_capability_uuid,
       plan::CanonicalLogicalRelationalNodeKind::kValues,
       exec::PhysicalNodeKind::kValues,
       "canonical.values.materialize.v1",
       input_row_count,
       input_memory,
       0,
       0},
      {root->logical_node_id,
       pivot_implementation_id,
       pivot_capability_uuid,
       plan::CanonicalLogicalRelationalNodeKind::kPivot,
       exec::PhysicalNodeKind::kPivot,
       "canonical." + root->semantic_variant_id,
       input_row_count,
       std::max<std::uint64_t>(1, total_memory),
       1,
       1}};
  if (!CompleteLiveRuntimeMemoryReceipts(&profiles)) {
    return refuse("QOW-DIAG-OPT-017-REFUSAL-V1",
                  "PIVOT runtime memory receipts are incomplete");
  }
  auto planning = PlanAndPublishLivePhysicalDag(
      request, profiles, "pivot.selected-plan", "PIVOT");
  if (!planning.ok) return refuse(planning.diagnostic_id, planning.detail);
  result.optimizer_selected = true;
  result.physical_dag_published = true;
  result.physical_node_count = planning.physical_dag.nodes.size();
  result.selected_plan_uuid = planning.physical_dag.selected_plan_uuid;

  std::unordered_map<std::uint64_t, exec::DescriptorBatch> values_batches;
  values_batches.emplace(input_node->logical_node_id, std::move(input.batch));
  auto values_registration = MakeLiveValuesRegistration(
      std::move(values_batches), values_capability_uuid,
      "QOW-DIAG-RELATIONAL-LIVE-PIVOT-VALUES-V1", "PIVOT");
  exec::CanonicalPhysicalExecutorRegistration pivot_registration;
  pivot_registration.node_kind = exec::PhysicalNodeKind::kPivot;
  pivot_registration.implementation_id = pivot_implementation_id;
  pivot_registration.executor_capability_uuid = pivot_capability_uuid;
  pivot_registration.executor_capability_abi_version = 1;
  pivot_registration.engine_owned = true;
  pivot_registration.accepts_optimizer_publication_v2 = true;
  pivot_registration.execute =
      [group_terms = std::move(group_terms), for_term = std::move(for_term),
       in_items = std::move(in_items),
       aggregate_bindings = std::move(aggregate_bindings),
       result_columns = std::move(result_columns), include_nulls,
       input_row_count,
       maximum_key_comparisons = static_cast<std::size_t>(
           std::max<std::uint64_t>(1, maximum_key_comparisons)),
       maximum_transitions = static_cast<std::size_t>(
           std::max<std::uint64_t>(1, maximum_transitions)),
       maximum_output_cells = static_cast<std::size_t>(
           std::max<std::uint64_t>(1, output_cells)),
       maximum_state_bytes = static_cast<std::size_t>(
           std::max<std::uint64_t>(1, total_memory)),
       mga_context = request.context](
          const exec::TypedPhysicalNodeDag& dag,
          const exec::PhysicalNodeRecord& node,
          const std::vector<exec::CanonicalPhysicalDispatchInput>& inputs) {
        exec::CanonicalPhysicalDispatchStepResult step;
        step.selected_plan_uuid = dag.selected_plan_uuid;
        step.mga_statement_context = dag.mga_statement_context;
        step.executed_physical_node_id = node.physical_node_id;
        step.causal_counter_id = node.causal_counter_id;
        step.output_descriptor_ids = node.output_descriptor_ids;
        step.authority.engine_mga_snapshot_bound = true;
        if (inputs.size() != 1 ||
            node.input_physical_node_ids.size() != 1 ||
            inputs.front().physical_node_id !=
                node.input_physical_node_ids.front() ||
            !inputs.front().materialized_output_batch.has_value() ||
            inputs.front().materialized_output_batch->rows.size() !=
                input_row_count) {
          step.diagnostic.ok = false;
          step.diagnostic.diagnostic_code =
              "QOW-DIAG-RELATIONAL-LIVE-PIVOT-INPUT-V1";
          step.diagnostic.detail =
              "PIVOT executor did not receive its exact selected input batch";
          return step;
        }
        auto bindings = aggregate_bindings;
        for (auto& binding : bindings) {
          binding.aggregate_template.maximum_transition_count =
              std::max<std::size_t>(1, input_row_count);
          binding.aggregate_template.maximum_state_bytes = maximum_state_bytes;
          binding.aggregate_template.maximum_final_output_bytes =
              maximum_state_bytes;
          binding.aggregate_template.maximum_finalization_workspace_bytes =
              maximum_state_bytes;
          std::string equality_detail;
          if (!BindCanonicalAggregateEqualityTerms(
                  mga_context,
                  *inputs.front().materialized_output_batch,
                  &binding.aggregate_template, &equality_detail)) {
            step.diagnostic.ok = false;
            step.diagnostic.diagnostic_code =
                "QOW-DIAG-RELATIONAL-LIVE-PIVOT-EQUALITY-V1";
            step.diagnostic.detail = std::move(equality_detail);
            return step;
          }
        }
        exec::CanonicalPivotRequest pivot_request;
        pivot_request.physical_dag = dag;
        pivot_request.selected_physical_node_id = node.physical_node_id;
        pivot_request.input_batch =
            *inputs.front().materialized_output_batch;
        pivot_request.group_key_terms = group_terms;
        pivot_request.for_key_terms = {for_term};
        pivot_request.in_items = in_items;
        pivot_request.aggregates = std::move(bindings);
        pivot_request.result_columns = result_columns;
        pivot_request.null_policy =
            include_nulls ? exec::CanonicalPivotNullPolicy::kInclude
                          : exec::CanonicalPivotNullPolicy::kExclude;
        pivot_request.maximum_key_comparison_count = maximum_key_comparisons;
        pivot_request.maximum_total_aggregate_transition_count =
            maximum_transitions;
        pivot_request.maximum_output_row_count =
            std::max<std::size_t>(1, input_row_count);
        pivot_request.maximum_output_cell_count = maximum_output_cells;
        pivot_request.maximum_combined_final_output_bytes =
            maximum_state_bytes;
        pivot_request.mga_authority =
            BuildCanonicalExecutionMgaAuthority(mga_context, dag);
        auto pivot = exec::ExecuteCanonicalPivot(pivot_request);
        if (!pivot.diagnostic.ok) {
          step.diagnostic = std::move(pivot.diagnostic);
          return step;
        }
        if (!CanonicalPivotExecutionReceiptMatches(
                pivot_request, node, pivot)) {
          step.diagnostic.ok = false;
          step.diagnostic.diagnostic_code =
              "QOW-DIAG-RELATIONAL-LIVE-PIVOT-EXECUTION-V1";
          step.diagnostic.detail = "PIVOT execution receipt changed";
          return step;
        }
        step.result_handle_id = node.physical_node_id;
        step.input_row_count = input_row_count;
        step.rows_examined = input_row_count;
        step.output_row_count = pivot.output_batch.rows.size();
        step.materialized_output_batch = std::move(pivot.output_batch);
        step.mga_statement_context =
            std::move(pivot.mga_statement_context);
        return step;
      };

  api::CanonicalOptimizerSelectedExecutionRequest execution_request;
  execution_request.pre_access_statistics_snapshot_uuid =
      planning.physical_dag.statistics_snapshot_uuid;
  execution_request.mga_authority = BuildCanonicalExecutionMgaAuthority(
      request.context, planning.physical_dag);
  execution_request.selected_physical_dag =
      std::move(planning.physical_dag);
  execution_request.available_executors.push_back(
      std::move(values_registration));
  execution_request.available_executors.push_back(
      std::move(pivot_registration));
  execution_request.engine_execution_authorized = true;
  execution_request.result_publication_request.statement_uuid =
      request.context.statement_uuid.canonical;
  execution_request.result_publication_request.execution_attempt_uuid =
      DerivedCanonicalUuid(identity_scope + ":" +
                               request.context.current_monotonic_ns,
                           "pivot.execution-attempt");
  execution_request.result_publication_request
      .transaction_effect_evidence_uuid = DerivedCanonicalUuid(
      identity_scope + ":" +
          std::to_string(request.context.local_transaction_id) + ":" +
          std::to_string(
              request.context.snapshot_visible_through_local_transaction_id),
      "pivot.transaction-effect-unchanged");
  execution_request.result_publication_request.result_kind =
      exec::CanonicalResultKind::kRows;
  execution_request.result_publication_request.invocation_mode =
      exec::CanonicalResultInvocationMode::kDirect;
  execution_request.result_publication_request.column_bindings =
      std::move(result_bindings);
  execution_request.result_publication_request.maximum_row_count =
      std::max<std::size_t>(1, input_row_count);
  const auto execution =
      ExecuteSelectedCanonicalObjectFreeDag(
          request.context, execution_request,
          planning.ordinary_runtime_memory_receipts);
  if (!execution.accepted || !execution.exact_selected_nodes_executed ||
      !execution.causal_counters_attached ||
      !execution.canonical_result_published || !execution.issues.empty()) {
    return refuse(
        execution.issues.empty()
            ? "QOW-DIAG-RELATIONAL-LIVE-PIVOT-EXECUTION-V1"
            : execution.issues.front().diagnostic_id,
        execution.issues.empty()
            ? "PIVOT selected DAG was not completed"
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
  result.api_result = SuccessfulApiResult(request, execution);
  return result;
}

// QOW-SOURCE-QRY-019-UNPIVOT-LIVE-V1
// The bound carrier is item-major: group identifiers, every source value
// identifier for each IN item, then one fixed label literal per item.  The
// result output identifies the first item's label and value descriptors; all
// later items must cast through those exact engine-owned descriptors.
CanonicalObjectFreeValuesExecutionResult
ExecuteCanonicalObjectFreeUnpivotQuery(
    const CanonicalObjectFreeValuesExecutionRequest& request) {
  CanonicalObjectFreeValuesExecutionResult result;
  const auto& graph = request.optimizer_request.logical_graph;
  const auto root = std::ranges::find_if(graph.nodes, [&](const auto& node) {
    return node.logical_node_id == graph.root_logical_node_id;
  });
  if (graph.nodes.size() != 2 || root == graph.nodes.end() ||
      root->node_kind != plan::CanonicalLogicalRelationalNodeKind::kUnpivot ||
      root->input_logical_node_ids.size() != 1 ||
      !request.optimizer_request.logical_properties.properties.empty()) {
    return result;
  }
  const bool include_nulls =
      root->semantic_variant_id ==
      "unpivot.fixed-value-list.include-nulls.v1";
  const bool exclude_nulls =
      root->semantic_variant_id ==
      "unpivot.fixed-value-list.exclude-nulls.v1";
  if (!include_nulls && !exclude_nulls) return result;
  const auto input_node =
      std::ranges::find_if(graph.nodes, [&](const auto& node) {
        return node.logical_node_id == root->input_logical_node_ids.front();
      });
  if (input_node == graph.nodes.end() || input_node == root ||
      input_node->node_kind !=
          plan::CanonicalLogicalRelationalNodeKind::kValues ||
      input_node->semantic_variant_id != "values.literal-table.v1" ||
      !input_node->input_logical_node_ids.empty()) {
    return result;
  }
  for (const auto& node : graph.nodes) {
    if (!node.required_object_uuids.empty()) return result;
  }

  result.profile_matched = true;
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
    result.api_result =
        Failure(request, std::move(diagnostic_id), std::move(detail));
    return result;
  };
  if (!request.optimizer_admission.admitted ||
      !request.optimizer_admission.planning_allowed) {
    return refuse("QOW-DIAG-RELATIONAL-LIVE-UNPIVOT-ADMISSION-V1",
                  "UNPIVOT lacks optimizer admission");
  }
  auto input = MaterializeValues(request.relational_dag, *input_node,
                                 request.expression_services);
  if (!input.ok) {
    return refuse("QOW-DIAG-RELATIONAL-LIVE-UNPIVOT-PAYLOAD-V1",
                  "UNPIVOT input VALUES: " + input.detail);
  }
  std::vector<const api::RelationalOutputRecord*> outputs;
  for (const auto& output : request.relational_dag.outputs) {
    if (output.relation_node_id == root->logical_node_id) {
      outputs.push_back(&output);
    }
  }
  std::ranges::sort(outputs, [](const auto* left, const auto* right) {
    return left->ordinal < right->ordinal;
  });
  const auto find_expression = [&](const std::uint32_t expression_id) {
    return std::ranges::find_if(
        request.relational_dag.expressions, [&](const auto& expression) {
          return expression.expression_id == expression_id;
        });
  };
  if (outputs.size() != root->output_descriptor_ids.size() ||
      outputs.size() < 3 || root->bound_expression_ids.empty()) {
    return refuse("QOW-DIAG-RELATIONAL-LIVE-UNPIVOT-PAYLOAD-V1",
                  "UNPIVOT output or bound-expression coverage is incomplete");
  }
  std::size_t group_count = 0;
  while (group_count < outputs.size()) {
    const auto expression = find_expression(outputs[group_count]->expression_id);
    if (expression == request.relational_dag.expressions.end() ||
        expression->expression_kind !=
            api::RelationalExpressionKind::kIdentifier) {
      break;
    }
    ++group_count;
  }
  if (group_count == 0 || group_count + 1 >= outputs.size()) {
    return refuse("QOW-DIAG-RELATIONAL-LIVE-UNPIVOT-PAYLOAD-V1",
                  "UNPIVOT group, label, or value output arity is unresolved");
  }
  const auto label_output_expression =
      find_expression(outputs[group_count]->expression_id);
  if (label_output_expression == request.relational_dag.expressions.end() ||
      label_output_expression->expression_kind !=
          api::RelationalExpressionKind::kLiteral) {
    return refuse("QOW-DIAG-RELATIONAL-LIVE-UNPIVOT-PAYLOAD-V1",
                  "UNPIVOT label output is not a fixed literal");
  }
  const auto value_count = outputs.size() - group_count - 1;
  const auto bound_tail_count =
      root->bound_expression_ids.size() -
      std::min(group_count, root->bound_expression_ids.size());
  if (root->bound_expression_ids.size() <= group_count || value_count == 0 ||
      bound_tail_count % (value_count + 1) != 0) {
    return refuse("QOW-DIAG-RELATIONAL-LIVE-UNPIVOT-PAYLOAD-V1",
                  "UNPIVOT fixed IN item/value arity is unresolved");
  }
  const auto item_count = bound_tail_count / (value_count + 1);
  if (item_count == 0) {
    return refuse("QOW-DIAG-RELATIONAL-LIVE-UNPIVOT-PAYLOAD-V1",
                  "UNPIVOT requires at least one fixed IN item");
  }
  const auto source_offset = group_count;
  const auto label_offset = group_count + item_count * value_count;
  if (label_offset + item_count != root->bound_expression_ids.size() ||
      outputs[group_count]->expression_id !=
          root->bound_expression_ids[label_offset]) {
    return refuse("QOW-DIAG-RELATIONAL-LIVE-UNPIVOT-PAYLOAD-V1",
                  "UNPIVOT label lineage is not item-major and exact");
  }

  const auto map_identifier = [&](const std::uint32_t expression_id,
                                  std::size_t* column,
                                  std::string* detail) {
    const auto expression = find_expression(expression_id);
    if (expression == request.relational_dag.expressions.end() ||
        expression->expression_kind !=
            api::RelationalExpressionKind::kIdentifier ||
        !expression->child_expression_ids.empty() ||
        !expression->bound_name_uuid.has_value() ||
        expression->function_uuid.has_value() ||
        expression->literal_kind.has_value() ||
        expression->operator_name.has_value() ||
        expression->literal_or_parameter_ref.has_value()) {
      *detail = "UNPIVOT source is not an exact bound identifier";
      return false;
    }
    const auto descriptor = std::ranges::find(
        input_node->output_descriptor_ids, expression->result_descriptor_id);
    if (descriptor == input_node->output_descriptor_ids.end() ||
        std::ranges::count(input_node->output_descriptor_ids,
                           expression->result_descriptor_id) != 1) {
      *detail = "UNPIVOT identifier is not uniquely supplied by VALUES";
      return false;
    }
    *column = static_cast<std::size_t>(std::distance(
        input_node->output_descriptor_ids.begin(), descriptor));
    if (*column >= input.batch.columns.size() ||
        input.batch.columns[*column].descriptor_id !=
            expression->result_descriptor_id) {
      *detail = "UNPIVOT identifier ordinal is not descriptor-exact";
      return false;
    }
    return true;
  };

  std::vector<std::size_t> group_columns;
  std::string detail;
  for (std::size_t group = 0; group < group_count; ++group) {
    if (outputs[group]->ordinal != group || !outputs[group]->visible ||
        outputs[group]->descriptor_id != root->output_descriptor_ids[group] ||
        outputs[group]->expression_id != root->bound_expression_ids[group]) {
      return refuse("QOW-DIAG-RELATIONAL-LIVE-UNPIVOT-PAYLOAD-V1",
                    "UNPIVOT group output lineage is not exact");
    }
    std::size_t column = 0;
    if (!map_identifier(root->bound_expression_ids[group], &column, &detail)) {
      return refuse("QOW-DIAG-RELATIONAL-LIVE-UNPIVOT-PAYLOAD-V1", detail);
    }
    group_columns.push_back(column);
  }

  std::vector<exec::CanonicalUnpivotInItem> in_items(item_count);
  std::vector<std::size_t> first_item_columns;
  for (std::size_t item = 0; item < item_count; ++item) {
    for (std::size_t value = 0; value < value_count; ++value) {
      const auto expression_id =
          root->bound_expression_ids[source_offset + item * value_count + value];
      std::size_t column = 0;
      if (!map_identifier(expression_id, &column, &detail) ||
          std::ranges::find(group_columns, column) != group_columns.end()) {
        return refuse(
            "QOW-DIAG-RELATIONAL-LIVE-UNPIVOT-PAYLOAD-V1",
            detail.empty() ? "UNPIVOT source overlaps a group column" : detail);
      }
      if (item == 0) {
        if (outputs[group_count + 1 + value]->expression_id != expression_id ||
            outputs[group_count + 1 + value]->descriptor_id !=
                input.batch.columns[column].descriptor_id) {
          return refuse("QOW-DIAG-RELATIONAL-LIVE-UNPIVOT-PAYLOAD-V1",
                        "UNPIVOT value output lineage is not exact");
        }
        first_item_columns.push_back(column);
      } else if (input.batch.columns[column].descriptor.canonical_type_name !=
                 input.batch.columns[first_item_columns[value]]
                     .descriptor.canonical_type_name) {
        return refuse("QOW-DIAG-RELATIONAL-LIVE-UNPIVOT-PAYLOAD-V1",
                      "UNPIVOT item value types are not reconcilable");
      }
      in_items[item].source_columns.push_back(column);
    }
  }

  CanonicalRelationalExpressionRuntime expression_runtime(
      request.relational_dag, request.expression_services);
  std::string label_type;
  if (!expression_runtime.InferType(root->bound_expression_ids[label_offset],
                                    std::nullopt, &label_type, &detail) ||
      label_type == "null") {
    return refuse("QOW-DIAG-RELATIONAL-LIVE-UNPIVOT-PAYLOAD-V1",
                  "UNPIVOT label type is unresolved: " + detail);
  }
  for (std::size_t item = 0; item < item_count; ++item) {
    const auto expression_id = root->bound_expression_ids[label_offset + item];
    const auto expression = find_expression(expression_id);
    api::EngineTypedValue label;
    std::string label_detail;
    if (expression == request.relational_dag.expressions.end() ||
        expression->expression_kind !=
            api::RelationalExpressionKind::kLiteral ||
        !expression->child_expression_ids.empty() ||
        expression->bound_name_uuid.has_value() ||
        expression->function_uuid.has_value() ||
        !expression->literal_kind.has_value() ||
        expression->operator_name.has_value() ||
        !expression->literal_or_parameter_ref.has_value() ||
        !expression_runtime.EvaluateForConsumer(
            expression_id, label_type,
            api::EngineCanonicalExpressionConsumer::projection, &label,
            &label_detail) ||
        label.state != api::EngineValueState::value || label.is_null) {
      return refuse("QOW-DIAG-RELATIONAL-LIVE-UNPIVOT-PAYLOAD-V1",
                    "UNPIVOT fixed label is invalid: " + label_detail);
    }
    in_items[item].pivot_value = std::move(label);
  }

  std::vector<exec::ExecutorColumnDescriptor> result_columns;
  result_columns.reserve(outputs.size());
  for (std::size_t group = 0; group < group_count; ++group) {
    auto column = input.batch.columns[group_columns[group]];
    column.stable_name = outputs[group]->output_name_utf8;
    result_columns.push_back(std::move(column));
  }
  const auto label_descriptor = std::ranges::find_if(
      request.relational_dag.descriptors, [&](const auto& descriptor) {
        return descriptor.descriptor_id == outputs[group_count]->descriptor_id;
      });
  if (label_descriptor == request.relational_dag.descriptors.end() ||
      label_descriptor->descriptor_uuid !=
          in_items.front().pivot_value.descriptor.descriptor_uuid.canonical ||
      outputs[group_count]->output_name_utf8.empty()) {
    return refuse("QOW-DIAG-RELATIONAL-LIVE-UNPIVOT-PAYLOAD-V1",
                  "UNPIVOT label result descriptor is unresolved");
  }
  result_columns.push_back(
      {outputs[group_count]->output_name_utf8,
       in_items.front().pivot_value.descriptor,
       label_descriptor->nullability == api::RelationalNullability::kNullable,
       label_descriptor->descriptor_id});
  for (std::size_t value = 0; value < value_count; ++value) {
    auto column = input.batch.columns[first_item_columns[value]];
    column.stable_name = outputs[group_count + 1 + value]->output_name_utf8;
    result_columns.push_back(std::move(column));
  }
  std::vector<exec::CanonicalResultColumnBinding> result_bindings;
  result_bindings.reserve(outputs.size());
  for (std::size_t ordinal = 0; ordinal < outputs.size(); ++ordinal) {
    const auto descriptor = std::ranges::find_if(
        request.relational_dag.descriptors, [&](const auto& candidate) {
          return candidate.descriptor_id == outputs[ordinal]->descriptor_id;
        });
    if (descriptor == request.relational_dag.descriptors.end() ||
        !outputs[ordinal]->visible || outputs[ordinal]->ordinal != ordinal ||
        outputs[ordinal]->output_name_utf8.empty()) {
      return refuse("QOW-DIAG-RELATIONAL-LIVE-UNPIVOT-PAYLOAD-V1",
                    "UNPIVOT result binding is not publishable");
    }
    exec::CanonicalResultColumnBinding binding;
    binding.physical_column_ordinal = ordinal;
    binding.visible = true;
    binding.published_descriptor = exec::CanonicalResultColumnDescriptor{
        static_cast<std::uint32_t>(ordinal),
        outputs[ordinal]->output_name_utf8,
        descriptor->descriptor_uuid,
        descriptor->type_uuid,
        ResultNullability(descriptor->nullability),
        descriptor->collation_uuid,
        descriptor->timezone_profile_id};
    result_bindings.push_back(std::move(binding));
  }

  const auto input_row_count = input.batch.rows.size();
  std::uint64_t maximum_output_rows = 0;
  std::uint64_t maximum_output_cells = 0;
  std::uint64_t input_memory = 1;
  std::uint64_t output_memory = 0;
  std::uint64_t total_memory = 0;
  if (!CheckedMultiply(input_row_count, item_count, &maximum_output_rows) ||
      !CheckedMultiply(maximum_output_rows, outputs.size(),
                       &maximum_output_cells) ||
      !AddBatchMemoryBytes(input.batch, &input_memory) ||
      !CheckedMultiply(maximum_output_cells, 64U, &output_memory) ||
      maximum_output_rows > std::numeric_limits<std::size_t>::max() ||
      maximum_output_cells > std::numeric_limits<std::size_t>::max()) {
    return refuse("QOW-DIAG-OPTIMIZER-SEARCH-COST-VECTOR-V1",
                  "UNPIVOT live cost or resource bound is invalid");
  }
  if (!CheckedAdd(input_memory, output_memory, &total_memory) ||
      total_memory > request.optimizer_request.resource.memory_budget_bytes) {
    return refuse("QOW-DIAG-OPTIMIZER-SEARCH-COST-VECTOR-V1",
                  "UNPIVOT live cost or resource bound is invalid");
  }
  std::uint64_t exact_output_payload_bytes = 0;
  std::string exact_output_detail;
  const auto account_cast_output = [&](const api::EngineTypedValue& source,
                                       const api::EngineDescriptor& target) {
    exec::DescriptorRuntimeDiagnostic cast_diagnostic;
    const auto casted = exec::CastDescriptorValue(
        source, target, &cast_diagnostic);
    if (!cast_diagnostic.ok) {
      exact_output_detail = cast_diagnostic.diagnostic_code + ":" +
                            cast_diagnostic.detail;
      return false;
    }
    if (!CheckedAdd(exact_output_payload_bytes,
                    casted.encoded_value.size(),
                    &exact_output_payload_bytes) ||
        !CheckedAdd(exact_output_payload_bytes,
                    casted.binary_value.size(),
                    &exact_output_payload_bytes)) {
      exact_output_detail = "UNPIVOT exact output payload overflowed";
      return false;
    }
    return true;
  };
  for (const auto& input_row : input.batch.rows) {
    for (const auto& item : in_items) {
      const bool all_null = std::ranges::all_of(
          item.source_columns, [&](const auto column) {
            return input_row.values[column].state ==
                   api::EngineValueState::sql_null;
          });
      if (exclude_nulls && all_null) continue;
      for (std::size_t group = 0; group < group_count; ++group) {
        if (!account_cast_output(
                input_row.values[group_columns[group]],
                result_columns[group].descriptor)) {
          return refuse("QOW-DIAG-RELATIONAL-LIVE-UNPIVOT-PAYLOAD-V1",
                        exact_output_detail);
        }
      }
      if (!account_cast_output(
              item.pivot_value, result_columns[group_count].descriptor)) {
        return refuse("QOW-DIAG-RELATIONAL-LIVE-UNPIVOT-PAYLOAD-V1",
                      exact_output_detail);
      }
      for (std::size_t value = 0; value < value_count; ++value) {
        if (!account_cast_output(
                input_row.values[item.source_columns[value]],
                result_columns[group_count + 1 + value].descriptor)) {
          return refuse("QOW-DIAG-RELATIONAL-LIVE-UNPIVOT-PAYLOAD-V1",
                        exact_output_detail);
        }
      }
    }
  }
  output_memory = std::max(output_memory, exact_output_payload_bytes);
  if (!CheckedAdd(input_memory, output_memory, &total_memory) ||
      total_memory > request.optimizer_request.resource.memory_budget_bytes) {
    return refuse("QOW-DIAG-OPTIMIZER-SEARCH-COST-VECTOR-V1",
                  "UNPIVOT live cost or resource bound is invalid");
  }

  const auto identity_scope = graph.bound_sblr_tree_uuid + ":" +
                              request.context.statement_uuid.canonical;
  const auto values_capability_uuid =
      DerivedCanonicalUuid(identity_scope, "values.capability");
  const auto unpivot_capability_uuid =
      DerivedCanonicalUuid(identity_scope, "unpivot.capability");
  const std::string unpivot_implementation_id =
      include_nulls ? "unpivot.canonical.include-nulls.typed.v1"
                    : "unpivot.canonical.exclude-nulls.typed.v1";
  std::vector<LivePhysicalNodeProfile> profiles = {
      {input_node->logical_node_id,
       std::string(kValuesImplementationId),
       values_capability_uuid,
       plan::CanonicalLogicalRelationalNodeKind::kValues,
       exec::PhysicalNodeKind::kValues,
       "canonical.values.materialize.v1",
       input_row_count,
       input_memory,
       0,
       0},
      {root->logical_node_id,
       unpivot_implementation_id,
       unpivot_capability_uuid,
       plan::CanonicalLogicalRelationalNodeKind::kUnpivot,
       exec::PhysicalNodeKind::kUnpivot,
       "canonical." + root->semantic_variant_id,
       maximum_output_rows,
       std::max<std::uint64_t>(1, total_memory),
       1,
       1}};
  if (!CompleteLiveRuntimeMemoryReceipts(&profiles)) {
    return refuse("QOW-DIAG-OPT-017-REFUSAL-V1",
                  "UNPIVOT runtime memory receipts are incomplete");
  }
  auto planning = PlanAndPublishLivePhysicalDag(
      request, profiles, "unpivot.selected-plan", "UNPIVOT");
  if (!planning.ok) return refuse(planning.diagnostic_id, planning.detail);
  result.optimizer_selected = true;
  result.physical_dag_published = true;
  result.physical_node_count = planning.physical_dag.nodes.size();
  result.selected_plan_uuid = planning.physical_dag.selected_plan_uuid;

  std::unordered_map<std::uint64_t, exec::DescriptorBatch> values_batches;
  values_batches.emplace(input_node->logical_node_id, std::move(input.batch));
  auto values_registration = MakeLiveValuesRegistration(
      std::move(values_batches), values_capability_uuid,
      "QOW-DIAG-RELATIONAL-LIVE-UNPIVOT-VALUES-V1", "UNPIVOT");
  exec::CanonicalPhysicalExecutorRegistration unpivot_registration;
  unpivot_registration.node_kind = exec::PhysicalNodeKind::kUnpivot;
  unpivot_registration.implementation_id = unpivot_implementation_id;
  unpivot_registration.executor_capability_uuid = unpivot_capability_uuid;
  unpivot_registration.executor_capability_abi_version = 1;
  unpivot_registration.engine_owned = true;
  unpivot_registration.accepts_optimizer_publication_v2 = true;
  unpivot_registration.execute =
      [group_columns = std::move(group_columns), in_items = std::move(in_items),
       result_columns = std::move(result_columns), include_nulls,
       input_row_count,
       maximum_output_rows = static_cast<std::size_t>(
           std::max<std::uint64_t>(1, maximum_output_rows)),
       maximum_output_cells = static_cast<std::size_t>(
           std::max<std::uint64_t>(1, maximum_output_cells)),
       mga_context = request.context](
          const exec::TypedPhysicalNodeDag& dag,
          const exec::PhysicalNodeRecord& node,
          const std::vector<exec::CanonicalPhysicalDispatchInput>& inputs) {
        exec::CanonicalPhysicalDispatchStepResult step;
        step.selected_plan_uuid = dag.selected_plan_uuid;
        step.mga_statement_context = dag.mga_statement_context;
        step.executed_physical_node_id = node.physical_node_id;
        step.causal_counter_id = node.causal_counter_id;
        step.output_descriptor_ids = node.output_descriptor_ids;
        step.authority.engine_mga_snapshot_bound = true;
        if (inputs.size() != 1 ||
            node.input_physical_node_ids.size() != 1 ||
            inputs.front().physical_node_id !=
                node.input_physical_node_ids.front() ||
            !inputs.front().materialized_output_batch.has_value() ||
            inputs.front().materialized_output_batch->rows.size() !=
                input_row_count) {
          step.diagnostic.ok = false;
          step.diagnostic.diagnostic_code =
              "QOW-DIAG-RELATIONAL-LIVE-UNPIVOT-INPUT-V1";
          step.diagnostic.detail =
              "UNPIVOT executor did not receive its exact selected input batch";
          return step;
        }
        exec::CanonicalUnpivotRequest unpivot_request;
        unpivot_request.physical_dag = dag;
        unpivot_request.selected_physical_node_id = node.physical_node_id;
        unpivot_request.input_batch =
            *inputs.front().materialized_output_batch;
        unpivot_request.group_columns = group_columns;
        unpivot_request.in_items = in_items;
        unpivot_request.result_columns = result_columns;
        unpivot_request.null_policy =
            include_nulls ? exec::CanonicalPivotNullPolicy::kInclude
                          : exec::CanonicalPivotNullPolicy::kExclude;
        unpivot_request.maximum_output_row_count = maximum_output_rows;
        unpivot_request.maximum_output_cell_count = maximum_output_cells;
        unpivot_request.mga_authority =
            BuildCanonicalExecutionMgaAuthority(mga_context, dag);
        auto unpivot = exec::ExecuteCanonicalUnpivot(unpivot_request);
        if (!unpivot.diagnostic.ok) {
          step.diagnostic = std::move(unpivot.diagnostic);
          return step;
        }
        if (!CanonicalUnpivotExecutionReceiptMatches(
                unpivot_request, node, unpivot)) {
          step.diagnostic.ok = false;
          step.diagnostic.diagnostic_code =
              "QOW-DIAG-RELATIONAL-LIVE-UNPIVOT-EXECUTION-V1";
          step.diagnostic.detail = "UNPIVOT execution receipt changed";
          return step;
        }
        step.result_handle_id = node.physical_node_id;
        step.input_row_count = input_row_count;
        step.rows_examined = input_row_count;
        step.output_row_count = unpivot.output_batch.rows.size();
        step.materialized_output_batch = std::move(unpivot.output_batch);
        step.mga_statement_context =
            std::move(unpivot.mga_statement_context);
        return step;
      };

  api::CanonicalOptimizerSelectedExecutionRequest execution_request;
  execution_request.pre_access_statistics_snapshot_uuid =
      planning.physical_dag.statistics_snapshot_uuid;
  execution_request.mga_authority = BuildCanonicalExecutionMgaAuthority(
      request.context, planning.physical_dag);
  execution_request.selected_physical_dag =
      std::move(planning.physical_dag);
  execution_request.available_executors.push_back(
      std::move(values_registration));
  execution_request.available_executors.push_back(
      std::move(unpivot_registration));
  execution_request.engine_execution_authorized = true;
  execution_request.result_publication_request.statement_uuid =
      request.context.statement_uuid.canonical;
  execution_request.result_publication_request.execution_attempt_uuid =
      DerivedCanonicalUuid(identity_scope + ":" +
                               request.context.current_monotonic_ns,
                           "unpivot.execution-attempt");
  execution_request.result_publication_request
      .transaction_effect_evidence_uuid = DerivedCanonicalUuid(
      identity_scope + ":" +
          std::to_string(request.context.local_transaction_id) + ":" +
          std::to_string(
              request.context.snapshot_visible_through_local_transaction_id),
      "unpivot.transaction-effect-unchanged");
  execution_request.result_publication_request.result_kind =
      exec::CanonicalResultKind::kRows;
  execution_request.result_publication_request.invocation_mode =
      exec::CanonicalResultInvocationMode::kDirect;
  execution_request.result_publication_request.column_bindings =
      std::move(result_bindings);
  execution_request.result_publication_request.maximum_row_count =
      std::max<std::size_t>(1,
                            static_cast<std::size_t>(maximum_output_rows));
  const auto execution =
      ExecuteSelectedCanonicalObjectFreeDag(
          request.context, execution_request,
          planning.ordinary_runtime_memory_receipts);
  if (!execution.accepted || !execution.exact_selected_nodes_executed ||
      !execution.causal_counters_attached ||
      !execution.canonical_result_published || !execution.issues.empty()) {
    return refuse(
        execution.issues.empty()
            ? "QOW-DIAG-RELATIONAL-LIVE-UNPIVOT-EXECUTION-V1"
            : execution.issues.front().diagnostic_id,
        execution.issues.empty()
            ? "UNPIVOT selected DAG was not completed"
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
  result.api_result = SuccessfulApiResult(request, execution);
  return result;
}

}  // namespace scratchbird::engine::sblr

// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "canonical_query_join_pipeline_composition.hpp"

#include "canonical_query_filter_registration.hpp"
#include "canonical_query_join_composition.hpp"
#include "canonical_query_join_registration.hpp"
#include "canonical_query_object_free_composition_support.hpp"
#include "canonical_query_object_free_profile.hpp"
#include "canonical_query_physical_registration.hpp"
#include "canonical_query_projection_registration.hpp"
#include "canonical_query_relational_registration.hpp"
#include "canonical_query_runtime_memory_support.hpp"
#include "canonical_query_scalar_support.hpp"

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

}  // namespace

// SEARCH_KEY: SB_ENGINE_CANONICAL_QUERY_JOIN_PIPELINE_COMPOSITION_AUTHORITY
// Coordinates an already-admitted object-free INNER JOIN pipeline. It
// consumes engine-selected MGA statement context and cannot create a
// snapshot, access storage, or publish transaction finality.

// RCP-041 through RCP-044: compose one accepted INNER JOIN with a
// row-dependent FILTER, computed PROJECT, optional query DISTINCT and SORT,
// and optional final LIMIT as one selected canonical physical DAG.
CanonicalObjectFreeValuesExecutionResult
ExecuteCanonicalObjectFreeInnerJoinFilterProjectQuery(
    const CanonicalObjectFreeValuesExecutionRequest& request) {
  CanonicalObjectFreeValuesExecutionResult result;
  const auto& graph = request.optimizer_request.logical_graph;
  const auto find_node = [&](const std::uint32_t node_id) {
    return std::ranges::find_if(graph.nodes, [&](const auto& node) {
      return node.logical_node_id == node_id;
    });
  };
  const auto root = find_node(graph.root_logical_node_id);
  if (root == graph.nodes.end()) {
    return result;
  }
  const bool has_limit =
      root->node_kind == plan::CanonicalLogicalRelationalNodeKind::kLimit;
  const bool fetch_first_rows_only =
      has_limit &&
      root->semantic_variant_id == "fetch.first-rows-only-offset.v1";
  const bool has_offset =
      has_limit && root->bound_expression_ids.size() == 2;
  if (has_limit &&
      ((root->semantic_variant_id != "limit.bound-count.v1" &&
        root->semantic_variant_id != "limit.bound-count-offset.v1" &&
        !fetch_first_rows_only) ||
       root->input_logical_node_ids.size() != 1 ||
       (root->semantic_variant_id == "limit.bound-count.v1"
            ? root->bound_expression_ids.size() != 1
            : root->bound_expression_ids.size() != 2))) {
    return result;
  }
  const auto sort_node =
      has_limit ? find_node(root->input_logical_node_ids.front()) : root;
  if (sort_node == graph.nodes.end()) {
    return result;
  }
  const bool has_sort =
      sort_node->node_kind == plan::CanonicalLogicalRelationalNodeKind::kSort;
  const auto sort_input_node =
      has_sort && sort_node->input_logical_node_ids.size() == 1
          ? find_node(sort_node->input_logical_node_ids.front())
          : graph.nodes.end();
  const bool has_distinct =
      has_sort && sort_input_node != graph.nodes.end() &&
      sort_input_node->node_kind ==
          plan::CanonicalLogicalRelationalNodeKind::kAggregate;
  if (has_distinct &&
      (!has_limit ||
       sort_input_node->semantic_variant_id !=
           "aggregate.query-distinct.v1" ||
       sort_input_node->input_logical_node_ids.size() != 1)) {
    return result;
  }
  if (has_offset && !has_distinct) {
    return result;
  }
  if ((has_limit && !has_sort) ||
      graph.nodes.size() !=
          (has_distinct ? 8U
                        : (has_limit ? 7U : (has_sort ? 6U : 5U))) ||
      (has_sort &&
       (sort_node->semantic_variant_id != "sort.required-order.v1" ||
        sort_node->input_logical_node_ids.size() != 1)) ||
      (!has_sort &&
       root->node_kind !=
           plan::CanonicalLogicalRelationalNodeKind::kProject) ||
      (!has_sort &&
       !request.optimizer_request.logical_properties.properties.empty())) {
    return result;
  }
  const auto project_input_node =
      has_distinct
          ? find_node(sort_input_node->input_logical_node_ids.front())
          : sort_input_node;
  const auto project_node = has_sort ? project_input_node : root;
  if (project_node == graph.nodes.end() ||
      project_node->node_kind !=
          plan::CanonicalLogicalRelationalNodeKind::kProject ||
      project_node->semantic_variant_id != "project.select-list.v1" ||
      project_node->input_logical_node_ids.size() != 1 ||
      project_node->bound_expression_ids.empty()) {
    return result;
  }
  const auto filter_node =
      find_node(project_node->input_logical_node_ids.front());
  if (filter_node == graph.nodes.end() ||
      filter_node->node_kind !=
          plan::CanonicalLogicalRelationalNodeKind::kFilter ||
      filter_node->semantic_variant_id != "filter.where.v1" ||
      filter_node->input_logical_node_ids.size() != 1 ||
      filter_node->bound_expression_ids.size() != 1) {
    return result;
  }
  const auto join_node = find_node(filter_node->input_logical_node_ids.front());
  if (join_node == graph.nodes.end() ||
      join_node->node_kind != plan::CanonicalLogicalRelationalNodeKind::kJoin ||
      join_node->semantic_variant_id != "join.inner.v1" ||
      join_node->input_logical_node_ids.size() != 2 ||
      join_node->input_logical_node_ids[0] ==
          join_node->input_logical_node_ids[1] ||
      join_node->bound_expression_ids.size() != 1) {
    return result;
  }
  const auto left_node = find_node(join_node->input_logical_node_ids[0]);
  const auto right_node = find_node(join_node->input_logical_node_ids[1]);
  if (left_node == graph.nodes.end() || right_node == graph.nodes.end() ||
      left_node->node_kind !=
          plan::CanonicalLogicalRelationalNodeKind::kValues ||
      right_node->node_kind !=
          plan::CanonicalLogicalRelationalNodeKind::kValues ||
      left_node->semantic_variant_id != "values.literal-table.v1" ||
      right_node->semantic_variant_id != "values.literal-table.v1" ||
      !left_node->input_logical_node_ids.empty() ||
      !right_node->input_logical_node_ids.empty()) {
    return result;
  }
  for (const auto& node : graph.nodes) {
    if (!node.required_object_uuids.empty() ||
        ((!has_sort ||
          node.logical_node_id != sort_node->logical_node_id) &&
         (!node.required_property_uuids.empty() ||
          !node.delivered_property_uuids.empty()))) {
      return result;
    }
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
  constexpr std::string_view kPayloadDiagnostic =
      "QOW-DIAG-RELATIONAL-LIVE-JOIN-FILTER-PROJECT-PAYLOAD-V1";
  if (!request.optimizer_admission.admitted ||
      !request.optimizer_admission.planning_allowed) {
    return refuse(
        "QOW-DIAG-RELATIONAL-LIVE-JOIN-FILTER-PROJECT-ADMISSION-V1",
        "INNER JOIN/FILTER/PROJECT lacks optimizer admission");
  }

  auto left = MaterializeValues(request.relational_dag, *left_node,
                                request.expression_services);
  auto right = MaterializeValues(request.relational_dag, *right_node,
                                 request.expression_services);
  if (!left.ok || !right.ok) {
    return refuse(std::string(kPayloadDiagnostic),
                  !left.ok ? "left VALUES: " + left.detail
                           : "right VALUES: " + right.detail);
  }
  auto prepared_join = PrepareJoinRootForComposition(
      request.relational_dag, *join_node, *left_node, *right_node, left,
      right, exec::CanonicalAcceptedJoinKind::kInner);
  if (!prepared_join.ok) {
    return refuse(std::string(kPayloadDiagnostic), prepared_join.detail);
  }

  const auto left_count = left.batch.rows.size();
  const auto right_count = right.batch.rows.size();
  if (left_count != 0 &&
      right_count > std::numeric_limits<std::size_t>::max() / left_count) {
    return refuse("QOW-DIAG-OPTIMIZER-SEARCH-COST-OVERFLOW-V1",
                  "INNER JOIN/FILTER/PROJECT pair count overflowed");
  }
  const auto pair_count = left_count * right_count;
  CanonicalRelationalExpressionRuntime expression_runtime(
      request.relational_dag, request.expression_services);
  std::vector<api::EngineSqlTruthValue> join_truth_values;
  join_truth_values.reserve(pair_count);
  MaterializedValues joined_input;
  joined_input.ok = true;
  joined_input.batch.columns = left.batch.columns;
  joined_input.batch.columns.insert(joined_input.batch.columns.end(),
                                    right.batch.columns.begin(),
                                    right.batch.columns.end());
  joined_input.batch.rows.reserve(pair_count);
  joined_input.result_bindings = prepared_join.result_bindings;
  std::vector<api::EngineTypedValue> pair_values;
  pair_values.reserve(joined_input.batch.columns.size());
  for (const auto& left_row : left.batch.rows) {
    for (const auto& right_row : right.batch.rows) {
      pair_values.clear();
      pair_values.insert(pair_values.end(), left_row.values.begin(),
                         left_row.values.end());
      pair_values.insert(pair_values.end(), right_row.values.begin(),
                         right_row.values.end());
      api::EngineSqlTruthValue truth = api::EngineSqlTruthValue::unknown;
      std::string detail;
      if (!expression_runtime.EvaluatePredicateForConsumer(
              prepared_join.predicate_expression_id,
              prepared_join.predicate_row_binding, pair_values,
              api::EngineCanonicalExpressionConsumer::join, &truth,
              &detail)) {
        return refuse(std::string(kPayloadDiagnostic),
                      "INNER JOIN pair " +
                          std::to_string(join_truth_values.size()) + ": " +
                          detail);
      }
      join_truth_values.push_back(truth);
      if (truth == api::EngineSqlTruthValue::true_value) {
        exec::DescriptorTuple joined_row;
        joined_row.values = pair_values;
        joined_input.batch.rows.push_back(std::move(joined_row));
      }
    }
  }

  auto prepared_filter = PrepareFilterRootForComposition(
      request.relational_dag, *filter_node, *join_node, joined_input);
  if (!prepared_filter.ok) {
    return refuse(std::string(kPayloadDiagnostic), prepared_filter.detail);
  }
  const auto joined_row_count = joined_input.batch.rows.size();
  std::vector<api::EngineSqlTruthValue> filter_truth_values;
  filter_truth_values.reserve(joined_row_count);
  MaterializedValues filtered_input;
  filtered_input.ok = true;
  filtered_input.batch.columns = joined_input.batch.columns;
  filtered_input.batch.rows.reserve(joined_row_count);
  filtered_input.result_bindings = prepared_filter.result_bindings;
  for (const auto& row : joined_input.batch.rows) {
    api::EngineSqlTruthValue truth = api::EngineSqlTruthValue::unknown;
    std::string detail;
    if (!expression_runtime.EvaluatePredicateForConsumer(
            prepared_filter.predicate_expression_id,
            prepared_filter.predicate_row_binding, row.values,
            api::EngineCanonicalExpressionConsumer::filter, &truth,
            &detail)) {
      return refuse(std::string(kPayloadDiagnostic),
                    "post-JOIN FILTER row " +
                        std::to_string(filter_truth_values.size()) + ": " +
                        detail);
    }
    filter_truth_values.push_back(truth);
    if (truth == api::EngineSqlTruthValue::true_value) {
      filtered_input.batch.rows.push_back(row);
    }
  }

  auto prepared_project = PrepareExpressionProjectRootForComposition(
      request.relational_dag, *project_node, *filter_node, filtered_input,
      request.expression_services);
  if (!prepared_project.ok || !prepared_project.expression_projection) {
    return refuse(std::string(kPayloadDiagnostic),
                  prepared_project.detail.empty()
                      ? "post-JOIN PROJECT is not an expression projection"
                      : prepared_project.detail);
  }
  PreparedDistinctRoot prepared_distinct;
  PreparedSortRoot prepared_sort;
  PreparedLimitRoot prepared_limit;
  MaterializedValues projected_input;
  if (has_sort) {
    projected_input.ok = true;
    projected_input.batch = prepared_project.expression_output_batch;
    projected_input.result_bindings = prepared_project.result_bindings;
    if (has_distinct) {
      prepared_distinct = PrepareQueryDistinctRootForComposition(
          request.context, request.relational_dag, *sort_input_node,
          *project_node, projected_input);
      if (!prepared_distinct.ok) {
        return refuse(std::string(kPayloadDiagnostic),
                      prepared_distinct.detail);
      }
    }
    prepared_sort = PrepareSortRootForComposition(
        request.context, request.relational_dag,
        request.optimizer_request.logical_properties, *sort_node,
        has_distinct ? *sort_input_node : *project_node, projected_input);
    if (!prepared_sort.ok) {
      return refuse(std::string(kPayloadDiagnostic), prepared_sort.detail);
    }
  }
  std::uint64_t row_limit = 0;
  std::uint64_t row_offset = 0;
  if (has_limit) {
    prepared_limit = PrepareLimitRootForComposition(
        request.relational_dag, *root, *sort_node, projected_input);
    if (!prepared_limit.ok) {
      return refuse(std::string(kPayloadDiagnostic), prepared_limit.detail);
    }
    std::string bound_detail;
    if (!EvaluateNonNegativeRowBoundForComposition(
            &expression_runtime, root->bound_expression_ids.front(),
            &row_limit, &bound_detail) ||
        (has_offset &&
         !EvaluateNonNegativeRowBoundForComposition(
             &expression_runtime, root->bound_expression_ids[1],
             &row_offset, &bound_detail))) {
      return refuse(std::string(kPayloadDiagnostic),
                    "LIMIT/FETCH bound: " + bound_detail);
    }
  }
  const auto filtered_row_count = filtered_input.batch.rows.size();
  std::uint64_t project_work = 0;
  std::uint64_t total_work = 0;
  if (!CheckedMultiply(filtered_row_count,
                       project_node->bound_expression_ids.size(),
                       &project_work) ||
      !CheckedAdd(pair_count, joined_row_count, &total_work) ||
      !CheckedAdd(total_work, project_work, &total_work) ||
      (has_limit &&
       !CheckedAdd(total_work, root->bound_expression_ids.size(),
                   &total_work))) {
    return refuse("QOW-DIAG-OPTIMIZER-SEARCH-COST-OVERFLOW-V1",
                  "INNER JOIN/FILTER/PROJECT work overflowed");
  }
  if (total_work >
      request.optimizer_request.resource.maximum_candidate_count) {
    return refuse("QOW-DIAG-OPTIMIZER-SEARCH-COST-VECTOR-V1",
                  "INNER JOIN/FILTER/PROJECT work exceeds the admitted "
                  "candidate bound");
  }

  std::uint64_t left_memory = 1;
  std::uint64_t right_memory = 1;
  std::uint64_t joined_memory = 1;
  std::uint64_t filtered_memory = 1;
  std::uint64_t projected_memory = 1;
  std::uint64_t join_state_memory = 0;
  std::uint64_t filter_state_memory = 0;
  std::uint64_t join_memory = 0;
  std::uint64_t filter_memory = 0;
  std::uint64_t project_peak_memory = 0;
  std::uint64_t project_memory = 0;
  std::uint64_t distinct_comparison_count = 0;
  std::uint64_t distinct_self_comparison_count = 0;
  std::uint64_t distinct_output_memory = 1;
  std::uint64_t distinct_auxiliary_memory = 0;
  std::uint64_t distinct_memory = 0;
  std::uint64_t comparison_count = 0;
  std::uint64_t row_order_memory = 0;
  std::uint64_t sort_memory = 0;
  std::uint64_t limit_memory = 0;
  if (!AddBatchMemoryBytes(left.batch, &left_memory) ||
      !AddBatchMemoryBytes(right.batch, &right_memory) ||
      !AddBatchMemoryBytes(joined_input.batch, &joined_memory) ||
      !AddBatchMemoryBytes(filtered_input.batch, &filtered_memory) ||
      !AddBatchMemoryBytes(prepared_project.expression_output_batch,
                           &projected_memory) ||
      !exec::BoundCanonicalJoinRetainedStateBytes(
          left_count, right_count, &join_state_memory) ||
      !CheckedMultiply(joined_row_count, sizeof(api::EngineSqlTruthValue),
                       &filter_state_memory) ||
      !CheckedAdd(left_memory, right_memory, &join_memory) ||
      !CheckedAdd(join_memory, join_state_memory, &join_memory) ||
      !CheckedAdd(join_memory, joined_memory, &join_memory) ||
      !CheckedAdd(join_memory, filter_state_memory, &filter_memory) ||
      !CheckedAdd(filter_memory, filtered_memory, &filter_memory) ||
      !CheckedAdd(filtered_memory, projected_memory,
                  &project_peak_memory) ||
      !CheckedAdd(filter_memory, projected_memory, &project_memory)) {
    return refuse("QOW-DIAG-OPTIMIZER-SEARCH-COST-OVERFLOW-V1",
                  "INNER JOIN/FILTER/PROJECT memory overflowed");
  }
  std::string distinct_memory_detail;
  if (has_distinct) {
    if (!QueryDistinctAuxiliaryMemoryBytes(
            filtered_row_count,
            prepared_project.expression_output_batch.columns.size(),
            &distinct_auxiliary_memory) ||
        !CheckedMultiply(filtered_row_count, filtered_row_count,
                         &distinct_comparison_count) ||
        !CheckedMultiply(
            distinct_comparison_count,
            prepared_project.expression_output_batch.columns.size(),
            &distinct_comparison_count) ||
        !CheckedMultiply(
            filtered_row_count,
            prepared_project.expression_output_batch.columns.size(),
            &distinct_self_comparison_count) ||
        !CheckedAdd(distinct_comparison_count,
                    distinct_self_comparison_count,
                    &distinct_comparison_count) ||
        distinct_comparison_count >
            std::numeric_limits<std::size_t>::max() ||
        !CheckedAdd(total_work, distinct_comparison_count, &total_work)) {
      return refuse(
          "QOW-DIAG-OPTIMIZER-SEARCH-COST-OVERFLOW-V1",
          "INNER JOIN/FILTER/PROJECT/DISTINCT resident state or comparison "
          "count overflowed");
    }
    if (total_work >
        request.optimizer_request.resource.maximum_candidate_count) {
      return refuse(
          "QOW-DIAG-OPTIMIZER-SEARCH-COST-VECTOR-V1",
          "INNER JOIN/FILTER/PROJECT/DISTINCT work exceeds the admitted "
          "candidate bound");
    }
    if (!QueryDistinctOutputMemoryBytes(
            prepared_project.expression_output_batch,
            prepared_distinct.equality_terms,
            static_cast<std::size_t>(distinct_comparison_count),
            &distinct_output_memory, &distinct_memory_detail) ||
        !CheckedAdd(projected_memory, distinct_output_memory,
                    &distinct_memory) ||
        !CheckedAdd(distinct_memory, distinct_auxiliary_memory,
                    &distinct_memory)) {
      return refuse(
          "QOW-DIAG-OPTIMIZER-SEARCH-COST-OVERFLOW-V1",
          "INNER JOIN/FILTER/PROJECT/DISTINCT output memory projection "
          "failed: " + distinct_memory_detail);
    }
  }
  const auto pre_sort_memory =
      has_distinct ? distinct_memory : project_memory;
  if (has_sort &&
      (!CheckedMultiply(filtered_row_count, filtered_row_count,
                        &comparison_count) ||
       !CheckedMultiply(filtered_row_count, sizeof(std::size_t),
                        &row_order_memory) ||
       !CheckedAdd(pre_sort_memory, projected_memory, &sort_memory) ||
       !CheckedAdd(sort_memory, comparison_count, &sort_memory) ||
       !CheckedAdd(sort_memory, row_order_memory, &sort_memory) ||
       comparison_count > std::numeric_limits<std::size_t>::max())) {
    return refuse("QOW-DIAG-OPTIMIZER-SEARCH-COST-OVERFLOW-V1",
                  "INNER JOIN/FILTER/PROJECT/SORT memory or comparison "
                  "count overflowed");
  }
  if (has_limit &&
      !CheckedAdd(sort_memory, projected_memory, &limit_memory)) {
    return refuse("QOW-DIAG-OPTIMIZER-SEARCH-COST-OVERFLOW-V1",
                  "INNER JOIN/FILTER/PROJECT/SORT/LIMIT memory overflowed");
  }
  const auto final_memory =
      has_limit ? limit_memory
                : (has_sort ? sort_memory
                            : (has_distinct ? distinct_memory
                                            : project_memory));
  if (final_memory >
      request.optimizer_request.resource.memory_budget_bytes) {
    return refuse("QOW-DIAG-OPTIMIZER-SEARCH-COST-VECTOR-V1",
                  has_limit
                      ? "INNER JOIN/FILTER/PROJECT/SORT/LIMIT exceeds the "
                        "admitted memory budget"
                  : has_sort
                      ? "INNER JOIN/FILTER/PROJECT/SORT exceeds the admitted "
                        "memory budget"
                      : "INNER JOIN/FILTER/PROJECT exceeds the admitted "
                        "memory budget");
  }

  const auto offset_bound =
      row_offset > filtered_row_count
          ? filtered_row_count
          : static_cast<std::size_t>(row_offset);
  const auto remaining_bound = filtered_row_count - offset_bound;
  const auto output_row_bound =
      has_limit
          ? (row_limit > remaining_bound
                 ? remaining_bound
                 : static_cast<std::size_t>(row_limit))
          : filtered_row_count;

  const auto identity_scope = graph.bound_sblr_tree_uuid + ":" +
                              request.context.statement_uuid;
  const auto values_capability_uuid =
      DerivedCanonicalUuid(identity_scope, "values.capability");
  const auto join_capability_uuid =
      DerivedCanonicalUuid(identity_scope, "join.inner.capability");
  const auto filter_capability_uuid =
      DerivedCanonicalUuid(identity_scope, "filter.capability");
  const auto project_capability_uuid =
      DerivedCanonicalUuid(identity_scope, "project.capability");
  const auto distinct_capability_uuid =
      DerivedCanonicalUuid(identity_scope, "distinct.capability");
  const auto sort_capability_uuid =
      DerivedCanonicalUuid(identity_scope, "sort.capability");
  const auto limit_capability_uuid = DerivedCanonicalUuid(
      identity_scope,
      fetch_first_rows_only
          ? "fetch.capability"
          : (has_offset ? "limit-offset.capability" : "limit.capability"));
  const auto deterministic_tie_evidence_uuid =
      has_sort
          ? DerivedCanonicalUuid(
                identity_scope + ":" + prepared_sort.ordering_property_uuid,
                has_distinct
                    ? "inner-join-filter-project-distinct-sort."
                      "deterministic-tie"
                    : "inner-join-filter-project-sort.deterministic-tie")
          : std::string{};
  const std::string operation_name =
      fetch_first_rows_only
          ? "INNER JOIN/FILTER/PROJECT/DISTINCT/SORT/FETCH"
          : (has_offset
                 ? "INNER JOIN/FILTER/PROJECT/DISTINCT/SORT/LIMIT/OFFSET"
                 : (has_distinct
                        ? "INNER JOIN/FILTER/PROJECT/DISTINCT/SORT/LIMIT"
                        : (has_limit
                               ? "INNER JOIN/FILTER/PROJECT/SORT/LIMIT"
                               : (has_sort
                                      ? "INNER JOIN/FILTER/PROJECT/SORT"
                                      : "INNER JOIN/FILTER/PROJECT"))));
  constexpr std::string_view kJoinImplementationId =
      "join.inner.3vl.nested.v1";
  const std::string limit_implementation_id =
      fetch_first_rows_only ? "fetch.native.rows-only.v1"
                            : "limit.typed.v1";
  const std::string limit_semantic_id =
      fetch_first_rows_only
          ? "canonical.fetch.first-rows-only-offset.filtered-projected-"
            "distinct-joined-order.v1"
          : (has_offset
                 ? "canonical.limit.bound-count-offset.filtered-projected-"
                   "distinct-joined-order.v1"
                 : "canonical.limit.filtered-projected-joined-order.v1");
  std::vector<LivePhysicalNodeProfile> profiles;
  for (const auto& node : graph.nodes) {
    if (node.logical_node_id == left_node->logical_node_id) {
      profiles.push_back(
          {node.logical_node_id, std::string(kValuesImplementationId),
           values_capability_uuid, node.node_kind,
           exec::PhysicalNodeKind::kValues,
           "canonical.values.materialize.v1", left_count, left_memory, 0, 0});
    } else if (node.logical_node_id == right_node->logical_node_id) {
      profiles.push_back(
          {node.logical_node_id, std::string(kValuesImplementationId),
           values_capability_uuid, node.node_kind,
           exec::PhysicalNodeKind::kValues,
           "canonical.values.materialize.v1", right_count, right_memory, 0,
           0});
    } else if (node.logical_node_id == join_node->logical_node_id) {
      profiles.push_back(
          {node.logical_node_id, std::string(kJoinImplementationId),
           join_capability_uuid, node.node_kind,
           exec::PhysicalNodeKind::kJoin,
           "canonical.join.inner.3vl.nested.v1", joined_row_count,
           request.optimizer_request.resource.memory_budget_bytes, 2, 2});
    } else if (node.logical_node_id == filter_node->logical_node_id) {
      profiles.push_back(
          {node.logical_node_id, "filter.3vl.row.v1", filter_capability_uuid,
           node.node_kind, exec::PhysicalNodeKind::kFilter,
           "canonical.filter.joined-row.3vl.v1", filtered_row_count,
           filter_memory, 1, 1});
    } else if (node.logical_node_id == project_node->logical_node_id) {
      profiles.push_back(
          {node.logical_node_id, "project.typed.expression-row.v1",
           project_capability_uuid, node.node_kind,
           exec::PhysicalNodeKind::kProject,
           "canonical.project.filtered-joined-expression-row.v1",
           filtered_row_count, project_peak_memory, 1, 1});
    } else if (has_distinct &&
               node.logical_node_id == sort_input_node->logical_node_id) {
      profiles.push_back(
          {node.logical_node_id, "aggregate.query-distinct.typed.v1",
           distinct_capability_uuid, node.node_kind,
           exec::PhysicalNodeKind::kAggregate,
           "canonical.aggregate.filtered-projected-joined-query-distinct.v1",
           filtered_row_count, distinct_memory, 1, 1});
    } else if (node.logical_node_id == sort_node->logical_node_id) {
      profiles.push_back(
          {node.logical_node_id, "sort.typed.terms.v1",
           sort_capability_uuid, node.node_kind,
           exec::PhysicalNodeKind::kSort,
           "canonical.sort.filtered-projected-joined-expression.v1",
           filtered_row_count, sort_memory, 1, 1, {},
           {prepared_sort.ordering_property_uuid},
           {plan::CanonicalLogicalPropertyKind::kOrdering}});
    } else {
      profiles.push_back(
          {node.logical_node_id, limit_implementation_id,
           limit_capability_uuid, node.node_kind,
           exec::PhysicalNodeKind::kLimit, limit_semantic_id,
           output_row_bound, limit_memory, 1, 1});
    }
  }
  std::vector<std::pair<std::uint32_t, std::uint64_t>> runtime_auxiliary;
  runtime_auxiliary.emplace_back(join_node->logical_node_id,
                                 join_state_memory);
  runtime_auxiliary.emplace_back(filter_node->logical_node_id,
                                 filter_state_memory);
  if (has_distinct) {
    runtime_auxiliary.emplace_back(sort_input_node->logical_node_id,
                                   distinct_auxiliary_memory);
  }
  if (has_sort) {
    runtime_auxiliary.emplace_back(sort_node->logical_node_id,
                                   comparison_count);
    runtime_auxiliary.emplace_back(sort_node->logical_node_id,
                                   row_order_memory);
  }
  if (!CompleteLiveRuntimeMemoryReceipts(&profiles,
                                         runtime_auxiliary)) {
    return refuse("QOW-DIAG-OPT-017-REFUSAL-V1",
                  "JOIN SQL-tail runtime memory receipts are incomplete");
  }
  auto planning = PlanAndPublishLivePhysicalDag(
      request, profiles,
      fetch_first_rows_only
          ? "inner-join-filter-project-distinct-sort-fetch.selected-plan"
          : (has_offset
                 ? "inner-join-filter-project-distinct-sort-limit-offset."
                   "selected-plan"
                 : (has_distinct
                        ? "inner-join-filter-project-distinct-sort-limit."
                          "selected-plan"
                        : (has_limit
                               ? "inner-join-filter-project-sort-limit."
                                 "selected-plan"
                               : (has_sort
                                      ? "inner-join-filter-project-sort."
                                        "selected-plan"
                                      : "inner-join-filter-project."
                                        "selected-plan")))),
      operation_name);
  if (!planning.ok) return refuse(planning.diagnostic_id, planning.detail);
  result.optimizer_selected = true;
  result.physical_dag_published = true;
  result.physical_node_count = planning.physical_dag.nodes.size();
  result.selected_plan_uuid = planning.physical_dag.selected_plan_uuid;

  std::unordered_map<std::uint64_t, exec::DescriptorBatch> values_batches;
  values_batches.emplace(left_node->logical_node_id, std::move(left.batch));
  values_batches.emplace(right_node->logical_node_id, std::move(right.batch));
  auto values_registration = MakeLiveValuesRegistration(
      std::move(values_batches), values_capability_uuid,
      "QOW-DIAG-RELATIONAL-LIVE-JOIN-FILTER-PROJECT-VALUES-V1",
      operation_name);
  auto join_registration = MakeLiveJoinRegistration(
      std::string(kJoinImplementationId), join_capability_uuid,
      std::move(join_truth_values), pair_count, joined_row_count,
      exec::CanonicalAcceptedJoinKind::kInner, "INNER JOIN", request.context);
  std::vector<api::EngineSqlTruthValue>().swap(filter_truth_values);
  auto filter_registration = MakeLiveFilterRegistration(
      prepared_filter.predicate_expression_id,
      prepared_filter.predicate_row_binding, request.relational_dag,
      request.expression_services, filter_capability_uuid, joined_row_count,
      request.context);
  auto project_registration = MakeLiveProjectRegistration(
      MakeLiveProjectRegistrationProfileForComposition(prepared_project),
      "project.typed.expression-row.v1",
      project_capability_uuid, filtered_row_count, request.relational_dag,
      request.expression_services, request.context);

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
      std::move(join_registration));
  execution_request.available_executors.push_back(
      std::move(filter_registration));
  execution_request.available_executors.push_back(
      std::move(project_registration));
  if (has_distinct) {
    execution_request.available_executors.push_back(
        MakeLiveQueryDistinctRegistration(
            std::move(prepared_distinct.equality_terms),
            distinct_capability_uuid, filtered_row_count,
            std::max<std::size_t>(
                1, static_cast<std::size_t>(distinct_comparison_count)),
            request.context));
  }
  if (has_sort) {
    execution_request.available_executors.push_back(
        MakeLiveSortRegistration(
            std::move(prepared_sort.order_terms),
            deterministic_tie_evidence_uuid, sort_capability_uuid,
            filtered_row_count,
            std::max<std::size_t>(
                1, static_cast<std::size_t>(comparison_count)),
            request.context));
  }
  if (has_limit) {
    execution_request.available_executors.push_back(
        MakeLiveLimitRegistration(
            limit_implementation_id, limit_capability_uuid, row_limit,
            row_offset, fetch_first_rows_only, filtered_row_count,
            request.context));
  }
  execution_request.engine_execution_authorized = true;
  execution_request.result_publication_request.statement_uuid =
      request.context.statement_uuid;
  execution_request.result_publication_request.execution_attempt_uuid =
      DerivedCanonicalUuid(
          identity_scope + ":" + request.context.current_monotonic_ns,
          fetch_first_rows_only
              ? "inner-join-filter-project-distinct-sort-fetch."
                "execution-attempt"
              : (has_offset
                     ? "inner-join-filter-project-distinct-sort-limit-offset."
                       "execution-attempt"
                     : (has_distinct
                            ? "inner-join-filter-project-distinct-sort-limit."
                              "execution-attempt"
                            : (has_limit
                                   ? "inner-join-filter-project-sort-limit."
                                     "execution-attempt"
                                   : (has_sort
                                          ? "inner-join-filter-project-sort."
                                            "execution-attempt"
                                          : "inner-join-filter-project."
                                            "execution-attempt")))));
  execution_request.result_publication_request
      .transaction_effect_evidence_uuid = DerivedCanonicalUuid(
      identity_scope + ":" +
          std::to_string(request.context.local_transaction_id) + ":" +
          std::to_string(
              request.context.snapshot_visible_through_local_transaction_id),
      fetch_first_rows_only
          ? "inner-join-filter-project-distinct-sort-fetch."
            "transaction-effect-unchanged"
          : (has_offset
                 ? "inner-join-filter-project-distinct-sort-limit-offset."
                   "transaction-effect-unchanged"
                 : (has_distinct
                        ? "inner-join-filter-project-distinct-sort-limit."
                          "transaction-effect-unchanged"
                        : (has_limit
                               ? "inner-join-filter-project-sort-limit."
                                 "transaction-effect-unchanged"
                               : (has_sort
                                      ? "inner-join-filter-project-sort."
                                        "transaction-effect-unchanged"
                                      : "inner-join-filter-project."
                                        "transaction-effect-unchanged")))));
  execution_request.result_publication_request.result_kind =
      exec::CanonicalResultKind::kRows;
  execution_request.result_publication_request.invocation_mode =
      exec::CanonicalResultInvocationMode::kDirect;
  if (has_limit) {
    execution_request.result_publication_request.column_bindings =
        std::move(prepared_limit.result_bindings);
  } else if (has_sort) {
    execution_request.result_publication_request.column_bindings =
        std::move(prepared_sort.result_bindings);
  } else {
    execution_request.result_publication_request.column_bindings =
        std::move(prepared_project.result_bindings);
  }
  execution_request.result_publication_request.maximum_row_count =
      std::max<std::size_t>(1, output_row_bound);

  const auto execution =
      ExecuteSelectedCanonicalObjectFreeDag(
          request.context, execution_request,
          planning.ordinary_runtime_memory_receipts);
  if (!execution.accepted || !execution.exact_selected_nodes_executed ||
      !execution.causal_counters_attached ||
      !execution.canonical_result_published || !execution.issues.empty()) {
    return refuse(
        execution.issues.empty()
            ? "QOW-DIAG-RELATIONAL-LIVE-JOIN-FILTER-PROJECT-EXECUTION-V1"
            : execution.issues.front().diagnostic_id,
        execution.issues.empty()
            ? operation_name + " selected DAG was not completed"
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

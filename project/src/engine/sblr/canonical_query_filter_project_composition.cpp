// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "canonical_query_filter_project_composition.hpp"

#include "canonical_query_filter_registration.hpp"
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

// SEARCH_KEY: SB_ENGINE_CANONICAL_QUERY_FILTER_PROJECT_COMPOSITION_AUTHORITY
// Coordinates already-admitted object-free FILTER/PROJECT pipelines. It
// consumes engine-selected MGA statement context and cannot create a
// snapshot, access storage, or publish transaction finality.

CanonicalObjectFreeValuesExecutionResult
ExecuteCanonicalObjectFreeFilterQuery(
    const CanonicalObjectFreeValuesExecutionRequest& request) {
  CanonicalObjectFreeValuesExecutionResult result;
  const auto& graph = request.optimizer_request.logical_graph;
  const auto root = std::ranges::find_if(graph.nodes, [&](const auto& node) {
    return node.logical_node_id == graph.root_logical_node_id;
  });
  if (graph.nodes.size() != 2 || root == graph.nodes.end() ||
      root->node_kind != plan::CanonicalLogicalRelationalNodeKind::kFilter ||
      root->semantic_variant_id != "filter.where.v1" ||
      root->input_logical_node_ids.size() != 1 ||
      root->bound_expression_ids.size() != 1 ||
      !request.optimizer_request.logical_properties.properties.empty()) {
    return result;
  }
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
    if (!node.required_object_uuids.empty() ||
        !node.required_property_uuids.empty() ||
        !node.delivered_property_uuids.empty()) {
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
  if (!request.optimizer_admission.admitted ||
      !request.optimizer_admission.planning_allowed) {
    return refuse("QOW-DIAG-RELATIONAL-LIVE-FILTER-ADMISSION-V1",
                  "live FILTER execution lacks optimizer admission");
  }

  auto input = MaterializeValues(request.relational_dag, *input_node,
                                 request.expression_services);
  if (!input.ok) {
    return refuse("QOW-DIAG-RELATIONAL-LIVE-FILTER-PAYLOAD-V1",
                  "FILTER input VALUES: " + input.detail);
  }
  auto prepared_root = PrepareFilterRootForComposition(request.relational_dag, *root,
                                         *input_node, input);
  if (!prepared_root.ok) {
    return refuse("QOW-DIAG-RELATIONAL-LIVE-FILTER-PAYLOAD-V1",
                  prepared_root.detail);
  }

  const auto input_row_count = input.batch.rows.size();
  if (input_row_count >
      request.optimizer_request.resource.maximum_candidate_count) {
    return refuse("QOW-DIAG-OPTIMIZER-SEARCH-COST-VECTOR-V1",
                  "live FILTER row evaluation exceeds the admitted candidate bound");
  }
  std::uint64_t input_memory = 1;
  if (!AddBatchMemoryBytes(input.batch, &input_memory)) {
    return refuse("QOW-DIAG-OPTIMIZER-SEARCH-COST-OVERFLOW-V1",
                  "live FILTER input size overflowed");
  }
  std::uint64_t predicate_memory = 0;
  std::uint64_t total_memory = 0;
  if (!CheckedMultiply(input_row_count, sizeof(api::EngineSqlTruthValue),
                       &predicate_memory) ||
      !CheckedAdd(input_memory, predicate_memory, &total_memory)) {
    return refuse("QOW-DIAG-OPTIMIZER-SEARCH-COST-OVERFLOW-V1",
                  "live FILTER predicate state size overflowed");
  }
  if (total_memory >
      request.optimizer_request.resource.memory_budget_bytes) {
    return refuse("QOW-DIAG-OPTIMIZER-SEARCH-COST-VECTOR-V1",
                  "live FILTER predicate state exceeds the admitted memory budget");
  }

  CanonicalRelationalExpressionRuntime expression_runtime(
      request.relational_dag, request.expression_services);
  std::vector<api::EngineSqlTruthValue> predicate_truth_values;
  predicate_truth_values.reserve(input_row_count);
  std::size_t output_row_bound = 0;
  std::uint64_t output_memory = 1;
  for (const auto& row : input.batch.rows) {
    api::EngineSqlTruthValue predicate_truth =
        api::EngineSqlTruthValue::unknown;
    std::string predicate_detail;
    if (!expression_runtime.EvaluatePredicateForConsumer(
            prepared_root.predicate_expression_id,
            prepared_root.predicate_row_binding, row.values,
            api::EngineCanonicalExpressionConsumer::filter,
            &predicate_truth, &predicate_detail)) {
      return refuse("QOW-DIAG-RELATIONAL-LIVE-FILTER-PAYLOAD-V1",
                    "FILTER predicate row " +
                        std::to_string(predicate_truth_values.size()) +
                        ": " + predicate_detail);
    }
    predicate_truth_values.push_back(predicate_truth);
    if (predicate_truth != api::EngineSqlTruthValue::true_value) continue;
    ++output_row_bound;
    for (const auto& value : row.values) {
      if (!CheckedAdd(output_memory, value.encoded_value.size(),
                      &output_memory) ||
          !CheckedAdd(output_memory, value.binary_value.size(),
                      &output_memory)) {
        return refuse("QOW-DIAG-OPTIMIZER-SEARCH-COST-OVERFLOW-V1",
                      "live FILTER output payload overflowed");
      }
    }
  }
  if (!CheckedAdd(total_memory, output_memory, &total_memory)) {
    return refuse("QOW-DIAG-OPTIMIZER-SEARCH-COST-OVERFLOW-V1",
                  "live FILTER output size overflowed");
  }
  if (total_memory >
      request.optimizer_request.resource.memory_budget_bytes) {
    return refuse("QOW-DIAG-OPTIMIZER-SEARCH-COST-VECTOR-V1",
                  "live FILTER exceeds the admitted memory budget");
  }

  const auto identity_scope =
      graph.bound_sblr_tree_uuid + ":" + request.context.statement_uuid.canonical;
  const auto values_capability_uuid =
      DerivedCanonicalUuid(identity_scope, "values.capability");
  const auto filter_capability_uuid =
      DerivedCanonicalUuid(identity_scope, "filter.capability");
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
       "filter.3vl.row.v1",
       filter_capability_uuid,
       plan::CanonicalLogicalRelationalNodeKind::kFilter,
       exec::PhysicalNodeKind::kFilter,
       "canonical.filter.3vl.row.v1",
       output_row_bound,
       total_memory,
       1,
       1}};
  if (!CompleteLiveRuntimeMemoryReceipts(
          &profiles, {{root->logical_node_id, predicate_memory}})) {
    return refuse("QOW-DIAG-OPT-017-REFUSAL-V1",
                  "FILTER runtime memory receipts are incomplete");
  }
  auto planning = PlanAndPublishLivePhysicalDag(
      request, profiles, "filter.selected-plan", "FILTER");
  if (!planning.ok) {
    return refuse(planning.diagnostic_id, planning.detail);
  }
  result.optimizer_selected = true;
  result.physical_dag_published = true;
  result.physical_node_count = planning.physical_dag.nodes.size();
  result.selected_plan_uuid = planning.physical_dag.selected_plan_uuid;

  std::unordered_map<std::uint64_t, exec::DescriptorBatch> values_batches;
  values_batches.emplace(input_node->logical_node_id, std::move(input.batch));
  auto values_registration = MakeLiveValuesRegistration(
      std::move(values_batches), values_capability_uuid,
      "QOW-DIAG-RELATIONAL-LIVE-FILTER-VALUES-V1", "FILTER");

  std::vector<api::EngineSqlTruthValue>().swap(predicate_truth_values);
  auto filter_registration = MakeLiveFilterRegistration(
      prepared_root.predicate_expression_id,
      prepared_root.predicate_row_binding, request.relational_dag,
      request.expression_services, filter_capability_uuid, input_row_count,
      request.context);

  api::CanonicalOptimizerSelectedExecutionRequest execution_request;
  execution_request.pre_access_statistics_snapshot_uuid =
      planning.physical_dag.statistics_snapshot_uuid;
  execution_request.mga_authority =
      BuildCanonicalExecutionMgaAuthority(request.context,
                                          planning.physical_dag);
  execution_request.selected_physical_dag =
      std::move(planning.physical_dag);
  execution_request.available_executors.push_back(
      std::move(values_registration));
  execution_request.available_executors.push_back(
      std::move(filter_registration));
  execution_request.engine_execution_authorized = true;
  execution_request.result_publication_request.statement_uuid =
      request.context.statement_uuid.canonical;
  execution_request.result_publication_request.execution_attempt_uuid =
      DerivedCanonicalUuid(
          identity_scope + ":" + request.context.current_monotonic_ns,
          "filter.execution-attempt");
  execution_request.result_publication_request.transaction_effect_evidence_uuid =
      DerivedCanonicalUuid(
          identity_scope + ":" +
              std::to_string(request.context.local_transaction_id) + ":" +
              std::to_string(
                  request.context.snapshot_visible_through_local_transaction_id),
          "filter.transaction-effect-unchanged");
  execution_request.result_publication_request.result_kind =
      exec::CanonicalResultKind::kRows;
  execution_request.result_publication_request.invocation_mode =
      exec::CanonicalResultInvocationMode::kDirect;
  execution_request.result_publication_request.column_bindings =
      std::move(prepared_root.result_bindings);
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
            ? "QOW-DIAG-RELATIONAL-LIVE-FILTER-EXECUTION-V1"
            : execution.issues.front().diagnostic_id,
        execution.issues.empty()
            ? "live FILTER selected DAG was not completed"
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

CanonicalObjectFreeValuesExecutionResult
ExecuteCanonicalObjectFreeProjectQuery(
    const CanonicalObjectFreeValuesExecutionRequest& request) {
  CanonicalObjectFreeValuesExecutionResult result;
  const auto& graph = request.optimizer_request.logical_graph;
  const auto root = std::ranges::find_if(graph.nodes, [&](const auto& node) {
    return node.logical_node_id == graph.root_logical_node_id;
  });
  if (graph.nodes.size() != 2 || root == graph.nodes.end() ||
      root->node_kind != plan::CanonicalLogicalRelationalNodeKind::kProject ||
      root->semantic_variant_id != "project.select-list.v1" ||
      root->input_logical_node_ids.size() != 1 ||
      !request.optimizer_request.logical_properties.properties.empty()) {
    return result;
  }
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
    if (!node.required_object_uuids.empty() ||
        !node.required_property_uuids.empty() ||
        !node.delivered_property_uuids.empty()) {
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
  if (!request.optimizer_admission.admitted ||
      !request.optimizer_admission.planning_allowed) {
    return refuse("QOW-DIAG-RELATIONAL-LIVE-PROJECT-ADMISSION-V1",
                  "live PROJECT execution lacks optimizer admission");
  }
  auto input = MaterializeValues(request.relational_dag, *input_node,
                                 request.expression_services);
  if (!input.ok) {
    return refuse("QOW-DIAG-RELATIONAL-LIVE-PROJECT-PAYLOAD-V1",
                  "PROJECT input VALUES: " + input.detail);
  }
  auto prepared_root = root->bound_expression_ids.empty()
      ? PrepareDescriptorDirectProjectRootForComposition(
            request.relational_dag, *root, *input_node, input)
      : PrepareExpressionProjectRootForComposition(
            request.relational_dag, *root, *input_node, input,
            request.expression_services);
  if (!prepared_root.ok) {
    return refuse("QOW-DIAG-RELATIONAL-LIVE-PROJECT-PAYLOAD-V1",
                  prepared_root.detail);
  }

  const auto input_row_count = input.batch.rows.size();
  if (prepared_root.expression_projection &&
      input_row_count >
          request.optimizer_request.resource.maximum_candidate_count) {
    return refuse("QOW-DIAG-OPTIMIZER-SEARCH-COST-VECTOR-V1",
                  "live expression PROJECT row evaluation exceeds the "
                  "admitted candidate bound");
  }
  std::uint64_t input_memory = 1;
  std::uint64_t output_memory = 1;
  std::uint64_t total_memory = 0;
  if (!AddBatchMemoryBytes(input.batch, &input_memory) ||
      !(prepared_root.expression_projection
            ? AddBatchMemoryBytes(prepared_root.expression_output_batch,
                                  &output_memory)
            : AddBatchProjectionMemoryBytes(
                  input.batch, prepared_root.projected_columns,
                  &output_memory)) ||
      !CheckedAdd(input_memory, output_memory, &total_memory)) {
    return refuse("QOW-DIAG-OPTIMIZER-SEARCH-COST-OVERFLOW-V1",
                  "live PROJECT input or output size overflowed");
  }
  if (total_memory >
      request.optimizer_request.resource.memory_budget_bytes) {
    return refuse("QOW-DIAG-OPTIMIZER-SEARCH-COST-VECTOR-V1",
                  "live PROJECT exceeds the admitted memory budget");
  }

  const auto identity_scope =
      graph.bound_sblr_tree_uuid + ":" + request.context.statement_uuid.canonical;
  const auto values_capability_uuid =
      DerivedCanonicalUuid(identity_scope, "values.capability");
  const auto project_capability_uuid =
      DerivedCanonicalUuid(identity_scope, "project.capability");
  const std::string project_implementation_id =
      prepared_root.expression_projection
          ? "project.typed.expression-row.v1"
          : "project.typed.row.v1";
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
       project_implementation_id,
       project_capability_uuid,
       plan::CanonicalLogicalRelationalNodeKind::kProject,
       exec::PhysicalNodeKind::kProject,
       prepared_root.expression_projection
           ? "canonical.project.expression-row.v1"
           : "canonical.project.descriptor-direct.v1",
       input_row_count,
       total_memory,
       1,
       1}};
  if (!CompleteLiveRuntimeMemoryReceipts(&profiles)) {
    return refuse("QOW-DIAG-OPT-017-REFUSAL-V1",
                  "PROJECT runtime memory receipts are incomplete");
  }
  auto planning = PlanAndPublishLivePhysicalDag(
      request, profiles, "project.selected-plan", "PROJECT");
  if (!planning.ok) {
    return refuse(planning.diagnostic_id, planning.detail);
  }
  result.optimizer_selected = true;
  result.physical_dag_published = true;
  result.physical_node_count = planning.physical_dag.nodes.size();
  result.selected_plan_uuid = planning.physical_dag.selected_plan_uuid;

  std::unordered_map<std::uint64_t, exec::DescriptorBatch> values_batches;
  values_batches.emplace(input_node->logical_node_id, std::move(input.batch));
  auto values_registration = MakeLiveValuesRegistration(
      std::move(values_batches), values_capability_uuid,
      "QOW-DIAG-RELATIONAL-LIVE-PROJECT-VALUES-V1", "PROJECT");

  auto project_registration = MakeLiveProjectRegistration(
      MakeLiveProjectRegistrationProfileForComposition(prepared_root),
      project_implementation_id, project_capability_uuid,
      input_row_count, request.relational_dag, request.expression_services,
      request.context);

  api::CanonicalOptimizerSelectedExecutionRequest execution_request;
  execution_request.pre_access_statistics_snapshot_uuid =
      planning.physical_dag.statistics_snapshot_uuid;
  execution_request.mga_authority =
      BuildCanonicalExecutionMgaAuthority(request.context,
                                          planning.physical_dag);
  execution_request.selected_physical_dag =
      std::move(planning.physical_dag);
  execution_request.available_executors.push_back(
      std::move(values_registration));
  execution_request.available_executors.push_back(
      std::move(project_registration));
  execution_request.engine_execution_authorized = true;
  execution_request.result_publication_request.statement_uuid =
      request.context.statement_uuid.canonical;
  execution_request.result_publication_request.execution_attempt_uuid =
      DerivedCanonicalUuid(
          identity_scope + ":" + request.context.current_monotonic_ns,
          "project.execution-attempt");
  execution_request.result_publication_request.transaction_effect_evidence_uuid =
      DerivedCanonicalUuid(
          identity_scope + ":" +
              std::to_string(request.context.local_transaction_id) + ":" +
              std::to_string(
                  request.context.snapshot_visible_through_local_transaction_id),
          "project.transaction-effect-unchanged");
  execution_request.result_publication_request.result_kind =
      exec::CanonicalResultKind::kRows;
  execution_request.result_publication_request.invocation_mode =
      exec::CanonicalResultInvocationMode::kDirect;
  execution_request.result_publication_request.column_bindings =
      std::move(prepared_root.result_bindings);
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
            ? "QOW-DIAG-RELATIONAL-LIVE-PROJECT-EXECUTION-V1"
            : execution.issues.front().diagnostic_id,
        execution.issues.empty()
            ? "live PROJECT selected DAG was not completed"
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

// RCP-035: one selected VALUES -> row-dependent FILTER -> computed PROJECT
// DAG. The PROJECT consumes only the physical FILTER batch, so rejected rows
// cannot be evaluated or recovered by the SELECT-list route.
CanonicalObjectFreeValuesExecutionResult
ExecuteCanonicalObjectFreeFilterProjectQuery(
    const CanonicalObjectFreeValuesExecutionRequest& request) {
  CanonicalObjectFreeValuesExecutionResult result;
  const auto& graph = request.optimizer_request.logical_graph;
  const auto root = std::ranges::find_if(graph.nodes, [&](const auto& node) {
    return node.logical_node_id == graph.root_logical_node_id;
  });
  if (graph.nodes.size() != 3 || root == graph.nodes.end() ||
      root->node_kind != plan::CanonicalLogicalRelationalNodeKind::kProject ||
      root->semantic_variant_id != "project.select-list.v1" ||
      root->input_logical_node_ids.size() != 1 ||
      root->bound_expression_ids.empty() ||
      !request.optimizer_request.logical_properties.properties.empty()) {
    return result;
  }
  const auto filter_node =
      std::ranges::find_if(graph.nodes, [&](const auto& node) {
        return node.logical_node_id == root->input_logical_node_ids.front();
      });
  if (filter_node == graph.nodes.end() || filter_node == root ||
      filter_node->node_kind !=
          plan::CanonicalLogicalRelationalNodeKind::kFilter ||
      filter_node->semantic_variant_id != "filter.where.v1" ||
      filter_node->input_logical_node_ids.size() != 1 ||
      filter_node->bound_expression_ids.size() != 1) {
    return result;
  }
  const auto values_node =
      std::ranges::find_if(graph.nodes, [&](const auto& node) {
        return node.logical_node_id ==
               filter_node->input_logical_node_ids.front();
      });
  if (values_node == graph.nodes.end() || values_node == root ||
      values_node == filter_node ||
      values_node->node_kind !=
          plan::CanonicalLogicalRelationalNodeKind::kValues ||
      values_node->semantic_variant_id != "values.literal-table.v1" ||
      !values_node->input_logical_node_ids.empty()) {
    return result;
  }
  for (const auto& node : graph.nodes) {
    if (!node.required_object_uuids.empty() ||
        !node.required_property_uuids.empty() ||
        !node.delivered_property_uuids.empty()) {
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
  if (!request.optimizer_admission.admitted ||
      !request.optimizer_admission.planning_allowed) {
    return refuse("QOW-DIAG-RELATIONAL-LIVE-FILTER-PROJECT-ADMISSION-V1",
                  "FILTER/PROJECT composition lacks optimizer admission");
  }

  auto input = MaterializeValues(request.relational_dag, *values_node,
                                 request.expression_services);
  if (!input.ok) {
    return refuse("QOW-DIAG-RELATIONAL-LIVE-FILTER-PROJECT-PAYLOAD-V1",
                  "FILTER/PROJECT input VALUES: " + input.detail);
  }
  auto prepared_filter = PrepareFilterRootForComposition(
      request.relational_dag, *filter_node, *values_node, input);
  if (!prepared_filter.ok) {
    return refuse("QOW-DIAG-RELATIONAL-LIVE-FILTER-PROJECT-PAYLOAD-V1",
                  prepared_filter.detail);
  }

  const auto input_row_count = input.batch.rows.size();
  CanonicalRelationalExpressionRuntime expression_runtime(
      request.relational_dag, request.expression_services);
  std::vector<api::EngineSqlTruthValue> predicate_truth_values;
  predicate_truth_values.reserve(input_row_count);
  MaterializedValues filtered_input;
  filtered_input.ok = true;
  filtered_input.batch.columns = input.batch.columns;
  filtered_input.batch.rows.reserve(input_row_count);
  filtered_input.result_bindings = prepared_filter.result_bindings;
  for (const auto& row : input.batch.rows) {
    api::EngineSqlTruthValue predicate_truth =
        api::EngineSqlTruthValue::unknown;
    std::string predicate_detail;
    if (!expression_runtime.EvaluatePredicateForConsumer(
            prepared_filter.predicate_expression_id,
            prepared_filter.predicate_row_binding, row.values,
            api::EngineCanonicalExpressionConsumer::filter,
            &predicate_truth, &predicate_detail)) {
      return refuse(
          "QOW-DIAG-RELATIONAL-LIVE-FILTER-PROJECT-PAYLOAD-V1",
          "FILTER predicate row " +
              std::to_string(predicate_truth_values.size()) + ": " +
              predicate_detail);
    }
    predicate_truth_values.push_back(predicate_truth);
    if (predicate_truth == api::EngineSqlTruthValue::true_value) {
      filtered_input.batch.rows.push_back(row);
    }
  }
  std::uint64_t project_expression_work = 0;
  std::uint64_t total_expression_work = 0;
  if (!CheckedMultiply(filtered_input.batch.rows.size(),
                       root->bound_expression_ids.size(),
                       &project_expression_work) ||
      !CheckedAdd(input_row_count, project_expression_work,
                  &total_expression_work)) {
    return refuse("QOW-DIAG-OPTIMIZER-SEARCH-COST-OVERFLOW-V1",
                  "FILTER/PROJECT expression work overflowed");
  }
  if (total_expression_work >
      request.optimizer_request.resource.maximum_candidate_count) {
    return refuse("QOW-DIAG-OPTIMIZER-SEARCH-COST-VECTOR-V1",
                  "FILTER/PROJECT expression work exceeds the admitted "
                  "candidate bound");
  }

  auto prepared_project = PrepareExpressionProjectRootForComposition(
      request.relational_dag, *root, *filter_node, filtered_input,
      request.expression_services);
  if (!prepared_project.ok || !prepared_project.expression_projection) {
    return refuse("QOW-DIAG-RELATIONAL-LIVE-FILTER-PROJECT-PAYLOAD-V1",
                  prepared_project.detail.empty()
                      ? "FILTER/PROJECT requires an expression projection"
                      : prepared_project.detail);
  }

  std::uint64_t input_memory = 1;
  std::uint64_t predicate_memory = 0;
  std::uint64_t filtered_memory = 1;
  std::uint64_t project_output_memory = 1;
  std::uint64_t filter_memory = 0;
  std::uint64_t project_peak_memory = 0;
  std::uint64_t total_memory = 0;
  if (!AddBatchMemoryBytes(input.batch, &input_memory) ||
      !CheckedMultiply(input_row_count, sizeof(api::EngineSqlTruthValue),
                       &predicate_memory) ||
      !AddBatchMemoryBytes(filtered_input.batch, &filtered_memory) ||
      !AddBatchMemoryBytes(prepared_project.expression_output_batch,
                           &project_output_memory) ||
      !CheckedAdd(input_memory, predicate_memory, &filter_memory) ||
      !CheckedAdd(filter_memory, filtered_memory, &filter_memory) ||
      !CheckedAdd(filtered_memory, project_output_memory,
                  &project_peak_memory) ||
      !CheckedAdd(filter_memory, project_output_memory, &total_memory)) {
    return refuse("QOW-DIAG-OPTIMIZER-SEARCH-COST-OVERFLOW-V1",
                  "FILTER/PROJECT materialization size overflowed");
  }
  if (total_memory >
      request.optimizer_request.resource.memory_budget_bytes) {
    return refuse("QOW-DIAG-OPTIMIZER-SEARCH-COST-VECTOR-V1",
                  "FILTER/PROJECT exceeds the admitted memory budget");
  }

  const auto identity_scope = graph.bound_sblr_tree_uuid + ":" +
                              request.context.statement_uuid.canonical;
  const auto values_capability_uuid =
      DerivedCanonicalUuid(identity_scope, "values.capability");
  const auto filter_capability_uuid =
      DerivedCanonicalUuid(identity_scope, "filter.capability");
  const auto project_capability_uuid =
      DerivedCanonicalUuid(identity_scope, "project.capability");
  const auto filtered_row_count = filtered_input.batch.rows.size();
  std::vector<LivePhysicalNodeProfile> profiles = {
      {values_node->logical_node_id,
       std::string(kValuesImplementationId),
       values_capability_uuid,
       plan::CanonicalLogicalRelationalNodeKind::kValues,
       exec::PhysicalNodeKind::kValues,
       "canonical.values.materialize.v1",
       input_row_count,
       input_memory,
       0,
       0},
      {filter_node->logical_node_id,
       "filter.3vl.row.v1",
       filter_capability_uuid,
       plan::CanonicalLogicalRelationalNodeKind::kFilter,
       exec::PhysicalNodeKind::kFilter,
       "canonical.filter.3vl.row.v1",
       filtered_row_count,
       filter_memory,
       1,
       1},
      {root->logical_node_id,
       "project.typed.expression-row.v1",
       project_capability_uuid,
       plan::CanonicalLogicalRelationalNodeKind::kProject,
       exec::PhysicalNodeKind::kProject,
       "canonical.project.filtered-expression-row.v1",
       filtered_row_count,
       project_peak_memory,
       1,
       1}};
  if (!CompleteLiveRuntimeMemoryReceipts(
          &profiles,
          {{filter_node->logical_node_id, predicate_memory}})) {
    return refuse("QOW-DIAG-OPT-017-REFUSAL-V1",
                  "FILTER/PROJECT runtime memory receipts are incomplete");
  }
  auto planning = PlanAndPublishLivePhysicalDag(
      request, profiles, "filter-project.selected-plan", "FILTER/PROJECT");
  if (!planning.ok) {
    return refuse(planning.diagnostic_id, planning.detail);
  }
  result.optimizer_selected = true;
  result.physical_dag_published = true;
  result.physical_node_count = planning.physical_dag.nodes.size();
  result.selected_plan_uuid = planning.physical_dag.selected_plan_uuid;

  std::unordered_map<std::uint64_t, exec::DescriptorBatch> values_batches;
  values_batches.emplace(values_node->logical_node_id, std::move(input.batch));
  auto values_registration = MakeLiveValuesRegistration(
      std::move(values_batches), values_capability_uuid,
      "QOW-DIAG-RELATIONAL-LIVE-FILTER-PROJECT-VALUES-V1",
      "FILTER/PROJECT");
  std::vector<api::EngineSqlTruthValue>().swap(predicate_truth_values);
  auto filter_registration = MakeLiveFilterRegistration(
      prepared_filter.predicate_expression_id,
      prepared_filter.predicate_row_binding, request.relational_dag,
      request.expression_services, filter_capability_uuid, input_row_count,
      request.context);
  auto project_registration = MakeLiveProjectRegistration(
      MakeLiveProjectRegistrationProfileForComposition(prepared_project),
      "project.typed.expression-row.v1",
      project_capability_uuid, filtered_row_count, request.relational_dag,
      request.expression_services, request.context);

  api::CanonicalOptimizerSelectedExecutionRequest execution_request;
  execution_request.pre_access_statistics_snapshot_uuid =
      planning.physical_dag.statistics_snapshot_uuid;
  execution_request.mga_authority =
      BuildCanonicalExecutionMgaAuthority(request.context,
                                          planning.physical_dag);
  execution_request.selected_physical_dag =
      std::move(planning.physical_dag);
  execution_request.available_executors.push_back(
      std::move(values_registration));
  execution_request.available_executors.push_back(
      std::move(filter_registration));
  execution_request.available_executors.push_back(
      std::move(project_registration));
  execution_request.engine_execution_authorized = true;
  execution_request.result_publication_request.statement_uuid =
      request.context.statement_uuid.canonical;
  execution_request.result_publication_request.execution_attempt_uuid =
      DerivedCanonicalUuid(
          identity_scope + ":" + request.context.current_monotonic_ns,
          "filter-project.execution-attempt");
  execution_request.result_publication_request
      .transaction_effect_evidence_uuid = DerivedCanonicalUuid(
      identity_scope + ":" +
          std::to_string(request.context.local_transaction_id) + ":" +
          std::to_string(
              request.context.snapshot_visible_through_local_transaction_id),
      "filter-project.transaction-effect-unchanged");
  execution_request.result_publication_request.result_kind =
      exec::CanonicalResultKind::kRows;
  execution_request.result_publication_request.invocation_mode =
      exec::CanonicalResultInvocationMode::kDirect;
  execution_request.result_publication_request.column_bindings =
      std::move(prepared_project.result_bindings);
  execution_request.result_publication_request.maximum_row_count =
      std::max<std::size_t>(1, filtered_row_count);

  const auto execution =
      ExecuteSelectedCanonicalObjectFreeDag(
          request.context, execution_request,
          planning.ordinary_runtime_memory_receipts);
  if (!execution.accepted || !execution.exact_selected_nodes_executed ||
      !execution.causal_counters_attached ||
      !execution.canonical_result_published || !execution.issues.empty()) {
    return refuse(
        execution.issues.empty()
            ? "QOW-DIAG-RELATIONAL-LIVE-FILTER-PROJECT-EXECUTION-V1"
            : execution.issues.front().diagnostic_id,
        execution.issues.empty()
            ? "FILTER/PROJECT selected DAG was not completed"
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

// RCP-034: one selected VALUES -> computed PROJECT -> ORDER BY DAG. The
// projected scalar batch is the sole SORT input; source-only descriptors and
// caller-substituted payloads remain outside the route.
CanonicalObjectFreeValuesExecutionResult
ExecuteCanonicalObjectFreeProjectSortQuery(
    const CanonicalObjectFreeValuesExecutionRequest& request) {
  CanonicalObjectFreeValuesExecutionResult result;
  const auto& graph = request.optimizer_request.logical_graph;
  const auto root = std::ranges::find_if(graph.nodes, [&](const auto& node) {
    return node.logical_node_id == graph.root_logical_node_id;
  });
  if (graph.nodes.size() != 3 || root == graph.nodes.end() ||
      root->node_kind != plan::CanonicalLogicalRelationalNodeKind::kSort ||
      root->semantic_variant_id != "sort.required-order.v1" ||
      root->input_logical_node_ids.size() != 1) {
    return result;
  }
  const auto project_node =
      std::ranges::find_if(graph.nodes, [&](const auto& node) {
        return node.logical_node_id == root->input_logical_node_ids.front();
      });
  if (project_node == graph.nodes.end() || project_node == root ||
      project_node->node_kind !=
          plan::CanonicalLogicalRelationalNodeKind::kProject ||
      project_node->semantic_variant_id != "project.select-list.v1" ||
      project_node->input_logical_node_ids.size() != 1 ||
      project_node->bound_expression_ids.empty()) {
    return result;
  }
  const auto values_node =
      std::ranges::find_if(graph.nodes, [&](const auto& node) {
        return node.logical_node_id ==
               project_node->input_logical_node_ids.front();
      });
  if (values_node == graph.nodes.end() || values_node == root ||
      values_node == project_node ||
      values_node->node_kind !=
          plan::CanonicalLogicalRelationalNodeKind::kValues ||
      values_node->semantic_variant_id != "values.literal-table.v1" ||
      !values_node->input_logical_node_ids.empty()) {
    return result;
  }
  for (const auto& node : graph.nodes) {
    if (!node.required_object_uuids.empty() ||
        (node.logical_node_id != root->logical_node_id &&
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
  if (!request.optimizer_admission.admitted ||
      !request.optimizer_admission.planning_allowed) {
    return refuse("QOW-DIAG-RELATIONAL-LIVE-PROJECT-SORT-ADMISSION-V1",
                  "PROJECT/SORT composition lacks optimizer admission");
  }

  auto input = MaterializeValues(request.relational_dag, *values_node,
                                 request.expression_services);
  if (!input.ok) {
    return refuse("QOW-DIAG-RELATIONAL-LIVE-PROJECT-SORT-PAYLOAD-V1",
                  "PROJECT/SORT input VALUES: " + input.detail);
  }
  std::uint64_t expression_work = 0;
  if (!CheckedMultiply(input.batch.rows.size(),
                       project_node->bound_expression_ids.size(),
                       &expression_work) ||
      expression_work >
          request.optimizer_request.resource.maximum_candidate_count) {
    return refuse("QOW-DIAG-OPTIMIZER-SEARCH-COST-VECTOR-V1",
                  "PROJECT/SORT expression work exceeds the admitted "
                  "candidate bound");
  }
  auto prepared_project = PrepareExpressionProjectRootForComposition(
      request.relational_dag, *project_node, *values_node, input,
      request.expression_services);
  if (!prepared_project.ok || !prepared_project.expression_projection) {
    return refuse("QOW-DIAG-RELATIONAL-LIVE-PROJECT-SORT-PAYLOAD-V1",
                  prepared_project.detail.empty()
                      ? "PROJECT/SORT requires an expression projection"
                      : prepared_project.detail);
  }
  MaterializedValues projected_input;
  projected_input.ok = true;
  projected_input.batch = prepared_project.expression_output_batch;
  projected_input.result_bindings = prepared_project.result_bindings;
  auto prepared_sort = PrepareSortRootForComposition(
      request.context, request.relational_dag,
      request.optimizer_request.logical_properties, *root, *project_node,
      projected_input);
  if (!prepared_sort.ok) {
    return refuse("QOW-DIAG-RELATIONAL-LIVE-PROJECT-SORT-PAYLOAD-V1",
                  prepared_sort.detail);
  }

  const auto row_count = input.batch.rows.size();
  std::uint64_t input_memory = 1;
  std::uint64_t project_output_memory = 1;
  std::uint64_t project_memory = 0;
  std::uint64_t comparison_count = 0;
  std::uint64_t row_order_memory = 0;
  std::uint64_t sort_memory = 0;
  if (!AddBatchMemoryBytes(input.batch, &input_memory) ||
      !AddBatchMemoryBytes(prepared_project.expression_output_batch,
                           &project_output_memory) ||
      !CheckedAdd(input_memory, project_output_memory, &project_memory) ||
      !CheckedMultiply(row_count, row_count, &comparison_count) ||
      !CheckedMultiply(row_count, sizeof(std::size_t), &row_order_memory) ||
      !CheckedAdd(project_memory, project_output_memory, &sort_memory) ||
      !CheckedAdd(sort_memory, comparison_count, &sort_memory) ||
      !CheckedAdd(sort_memory, row_order_memory, &sort_memory) ||
      comparison_count > std::numeric_limits<std::size_t>::max()) {
    return refuse("QOW-DIAG-OPTIMIZER-SEARCH-COST-OVERFLOW-V1",
                  "PROJECT/SORT materialization or comparison size "
                  "overflowed");
  }
  if (sort_memory >
      request.optimizer_request.resource.memory_budget_bytes) {
    return refuse("QOW-DIAG-OPTIMIZER-SEARCH-COST-VECTOR-V1",
                  "PROJECT/SORT exceeds the admitted memory budget");
  }

  const auto identity_scope =
      graph.bound_sblr_tree_uuid + ":" + request.context.statement_uuid.canonical;
  const auto values_capability_uuid =
      DerivedCanonicalUuid(identity_scope, "values.capability");
  const auto project_capability_uuid =
      DerivedCanonicalUuid(identity_scope, "project.capability");
  const auto sort_capability_uuid =
      DerivedCanonicalUuid(identity_scope, "sort.capability");
  const auto deterministic_tie_evidence_uuid = DerivedCanonicalUuid(
      identity_scope + ":" + prepared_sort.ordering_property_uuid,
      "project-sort.deterministic-tie");
  std::vector<LivePhysicalNodeProfile> profiles = {
      {values_node->logical_node_id,
       std::string(kValuesImplementationId),
       values_capability_uuid,
       plan::CanonicalLogicalRelationalNodeKind::kValues,
       exec::PhysicalNodeKind::kValues,
       "canonical.values.materialize.v1",
       row_count,
       input_memory,
       0,
       0},
      {project_node->logical_node_id,
       "project.typed.expression-row.v1",
       project_capability_uuid,
       plan::CanonicalLogicalRelationalNodeKind::kProject,
       exec::PhysicalNodeKind::kProject,
       "canonical.project.expression-row.v1",
       row_count,
       project_memory,
       1,
       1},
      {root->logical_node_id,
       "sort.typed.terms.v1",
       sort_capability_uuid,
       plan::CanonicalLogicalRelationalNodeKind::kSort,
       exec::PhysicalNodeKind::kSort,
       "canonical.sort.projected-expression.v1",
       row_count,
       sort_memory,
       1,
       1,
       {},
       {prepared_sort.ordering_property_uuid},
       {plan::CanonicalLogicalPropertyKind::kOrdering}}};
  if (!CompleteLiveRuntimeMemoryReceipts(
          &profiles,
          {{root->logical_node_id, comparison_count},
           {root->logical_node_id, row_order_memory}})) {
    return refuse("QOW-DIAG-OPT-017-REFUSAL-V1",
                  "PROJECT/SORT runtime memory receipts are incomplete");
  }
  auto planning = PlanAndPublishLivePhysicalDag(
      request, profiles, "project-sort.selected-plan", "PROJECT/SORT");
  if (!planning.ok) {
    return refuse(planning.diagnostic_id, planning.detail);
  }
  result.optimizer_selected = true;
  result.physical_dag_published = true;
  result.physical_node_count = planning.physical_dag.nodes.size();
  result.selected_plan_uuid = planning.physical_dag.selected_plan_uuid;

  std::unordered_map<std::uint64_t, exec::DescriptorBatch> values_batches;
  values_batches.emplace(values_node->logical_node_id, std::move(input.batch));
  auto values_registration = MakeLiveValuesRegistration(
      std::move(values_batches), values_capability_uuid,
      "QOW-DIAG-RELATIONAL-LIVE-PROJECT-SORT-VALUES-V1", "PROJECT/SORT");
  auto project_registration = MakeLiveProjectRegistration(
      MakeLiveProjectRegistrationProfileForComposition(prepared_project),
      "project.typed.expression-row.v1",
      project_capability_uuid, row_count, request.relational_dag,
      request.expression_services, request.context);
  auto sort_registration = MakeLiveSortRegistration(
      std::move(prepared_sort.order_terms),
      deterministic_tie_evidence_uuid, sort_capability_uuid, row_count,
      std::max<std::size_t>(1,
                            static_cast<std::size_t>(comparison_count)),
      request.context);

  api::CanonicalOptimizerSelectedExecutionRequest execution_request;
  execution_request.pre_access_statistics_snapshot_uuid =
      planning.physical_dag.statistics_snapshot_uuid;
  execution_request.mga_authority =
      BuildCanonicalExecutionMgaAuthority(request.context,
                                          planning.physical_dag);
  execution_request.selected_physical_dag =
      std::move(planning.physical_dag);
  execution_request.available_executors.push_back(
      std::move(values_registration));
  execution_request.available_executors.push_back(
      std::move(project_registration));
  execution_request.available_executors.push_back(
      std::move(sort_registration));
  execution_request.engine_execution_authorized = true;
  execution_request.result_publication_request.statement_uuid =
      request.context.statement_uuid.canonical;
  execution_request.result_publication_request.execution_attempt_uuid =
      DerivedCanonicalUuid(
          identity_scope + ":" + request.context.current_monotonic_ns,
          "project-sort.execution-attempt");
  execution_request.result_publication_request
      .transaction_effect_evidence_uuid = DerivedCanonicalUuid(
      identity_scope + ":" +
          std::to_string(request.context.local_transaction_id) + ":" +
          std::to_string(
              request.context.snapshot_visible_through_local_transaction_id),
      "project-sort.transaction-effect-unchanged");
  execution_request.result_publication_request.result_kind =
      exec::CanonicalResultKind::kRows;
  execution_request.result_publication_request.invocation_mode =
      exec::CanonicalResultInvocationMode::kDirect;
  execution_request.result_publication_request.column_bindings =
      std::move(prepared_sort.result_bindings);
  execution_request.result_publication_request.maximum_row_count =
      std::max<std::size_t>(1, row_count);

  const auto execution =
      ExecuteSelectedCanonicalObjectFreeDag(
          request.context, execution_request,
          planning.ordinary_runtime_memory_receipts);
  if (!execution.accepted || !execution.exact_selected_nodes_executed ||
      !execution.causal_counters_attached ||
      !execution.canonical_result_published || !execution.issues.empty()) {
    return refuse(
        execution.issues.empty()
            ? "QOW-DIAG-RELATIONAL-LIVE-PROJECT-SORT-EXECUTION-V1"
            : execution.issues.front().diagnostic_id,
        execution.issues.empty()
            ? "PROJECT/SORT selected DAG was not completed"
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

// RCP-036: one selected VALUES -> FILTER -> PROJECT -> SORT DAG. Each upper
// operator consumes only the materialized descriptor batch emitted by its
// immediate selected predecessor.
CanonicalObjectFreeValuesExecutionResult
ExecuteCanonicalObjectFreeFilterProjectSortQuery(
    const CanonicalObjectFreeValuesExecutionRequest& request) {
  CanonicalObjectFreeValuesExecutionResult result;
  const auto& graph = request.optimizer_request.logical_graph;
  const auto root = std::ranges::find_if(graph.nodes, [&](const auto& node) {
    return node.logical_node_id == graph.root_logical_node_id;
  });
  if (graph.nodes.size() != 4 || root == graph.nodes.end() ||
      root->node_kind != plan::CanonicalLogicalRelationalNodeKind::kSort ||
      root->semantic_variant_id != "sort.required-order.v1" ||
      root->input_logical_node_ids.size() != 1) {
    return result;
  }
  const auto project_node =
      std::ranges::find_if(graph.nodes, [&](const auto& node) {
        return node.logical_node_id == root->input_logical_node_ids.front();
      });
  if (project_node == graph.nodes.end() || project_node == root ||
      project_node->node_kind !=
          plan::CanonicalLogicalRelationalNodeKind::kProject ||
      project_node->semantic_variant_id != "project.select-list.v1" ||
      project_node->input_logical_node_ids.size() != 1 ||
      project_node->bound_expression_ids.empty()) {
    return result;
  }
  const auto filter_node =
      std::ranges::find_if(graph.nodes, [&](const auto& node) {
        return node.logical_node_id ==
               project_node->input_logical_node_ids.front();
      });
  if (filter_node == graph.nodes.end() || filter_node == root ||
      filter_node == project_node ||
      filter_node->node_kind !=
          plan::CanonicalLogicalRelationalNodeKind::kFilter ||
      filter_node->semantic_variant_id != "filter.where.v1" ||
      filter_node->input_logical_node_ids.size() != 1 ||
      filter_node->bound_expression_ids.size() != 1) {
    return result;
  }
  const auto values_node =
      std::ranges::find_if(graph.nodes, [&](const auto& node) {
        return node.logical_node_id ==
               filter_node->input_logical_node_ids.front();
      });
  if (values_node == graph.nodes.end() || values_node == root ||
      values_node == project_node || values_node == filter_node ||
      values_node->node_kind !=
          plan::CanonicalLogicalRelationalNodeKind::kValues ||
      values_node->semantic_variant_id != "values.literal-table.v1" ||
      !values_node->input_logical_node_ids.empty()) {
    return result;
  }
  for (const auto& node : graph.nodes) {
    if (!node.required_object_uuids.empty() ||
        (node.logical_node_id != root->logical_node_id &&
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
  if (!request.optimizer_admission.admitted ||
      !request.optimizer_admission.planning_allowed) {
    return refuse(
        "QOW-DIAG-RELATIONAL-LIVE-FILTER-PROJECT-SORT-ADMISSION-V1",
        "FILTER/PROJECT/SORT composition lacks optimizer admission");
  }

  auto input = MaterializeValues(request.relational_dag, *values_node,
                                 request.expression_services);
  if (!input.ok) {
    return refuse(
        "QOW-DIAG-RELATIONAL-LIVE-FILTER-PROJECT-SORT-PAYLOAD-V1",
        "FILTER/PROJECT/SORT input VALUES: " + input.detail);
  }
  auto prepared_filter = PrepareFilterRootForComposition(
      request.relational_dag, *filter_node, *values_node, input);
  if (!prepared_filter.ok) {
    return refuse(
        "QOW-DIAG-RELATIONAL-LIVE-FILTER-PROJECT-SORT-PAYLOAD-V1",
        prepared_filter.detail);
  }

  const auto input_row_count = input.batch.rows.size();
  CanonicalRelationalExpressionRuntime expression_runtime(
      request.relational_dag, request.expression_services);
  std::vector<api::EngineSqlTruthValue> predicate_truth_values;
  predicate_truth_values.reserve(input_row_count);
  MaterializedValues filtered_input;
  filtered_input.ok = true;
  filtered_input.batch.columns = input.batch.columns;
  filtered_input.batch.rows.reserve(input_row_count);
  filtered_input.result_bindings = prepared_filter.result_bindings;
  for (const auto& row : input.batch.rows) {
    api::EngineSqlTruthValue predicate_truth =
        api::EngineSqlTruthValue::unknown;
    std::string predicate_detail;
    if (!expression_runtime.EvaluatePredicateForConsumer(
            prepared_filter.predicate_expression_id,
            prepared_filter.predicate_row_binding, row.values,
            api::EngineCanonicalExpressionConsumer::filter,
            &predicate_truth, &predicate_detail)) {
      return refuse(
          "QOW-DIAG-RELATIONAL-LIVE-FILTER-PROJECT-SORT-PAYLOAD-V1",
          "FILTER predicate row " +
              std::to_string(predicate_truth_values.size()) + ": " +
              predicate_detail);
    }
    predicate_truth_values.push_back(predicate_truth);
    if (predicate_truth == api::EngineSqlTruthValue::true_value) {
      filtered_input.batch.rows.push_back(row);
    }
  }
  std::uint64_t project_expression_work = 0;
  std::uint64_t total_expression_work = 0;
  if (!CheckedMultiply(filtered_input.batch.rows.size(),
                       project_node->bound_expression_ids.size(),
                       &project_expression_work) ||
      !CheckedAdd(input_row_count, project_expression_work,
                  &total_expression_work)) {
    return refuse("QOW-DIAG-OPTIMIZER-SEARCH-COST-OVERFLOW-V1",
                  "FILTER/PROJECT/SORT expression work overflowed");
  }
  if (total_expression_work >
      request.optimizer_request.resource.maximum_candidate_count) {
    return refuse("QOW-DIAG-OPTIMIZER-SEARCH-COST-VECTOR-V1",
                  "FILTER/PROJECT/SORT expression work exceeds the admitted "
                  "candidate bound");
  }

  auto prepared_project = PrepareExpressionProjectRootForComposition(
      request.relational_dag, *project_node, *filter_node, filtered_input,
      request.expression_services);
  if (!prepared_project.ok || !prepared_project.expression_projection) {
    return refuse(
        "QOW-DIAG-RELATIONAL-LIVE-FILTER-PROJECT-SORT-PAYLOAD-V1",
        prepared_project.detail.empty()
            ? "FILTER/PROJECT/SORT requires an expression projection"
            : prepared_project.detail);
  }
  MaterializedValues projected_input;
  projected_input.ok = true;
  projected_input.batch = prepared_project.expression_output_batch;
  projected_input.result_bindings = prepared_project.result_bindings;
  auto prepared_sort = PrepareSortRootForComposition(
      request.context, request.relational_dag,
      request.optimizer_request.logical_properties, *root, *project_node,
      projected_input);
  if (!prepared_sort.ok) {
    return refuse(
        "QOW-DIAG-RELATIONAL-LIVE-FILTER-PROJECT-SORT-PAYLOAD-V1",
        prepared_sort.detail);
  }

  const auto filtered_row_count = filtered_input.batch.rows.size();
  std::uint64_t input_memory = 1;
  std::uint64_t predicate_memory = 0;
  std::uint64_t filtered_memory = 1;
  std::uint64_t project_output_memory = 1;
  std::uint64_t filter_memory = 0;
  std::uint64_t project_peak_memory = 0;
  std::uint64_t project_memory = 0;
  std::uint64_t comparison_count = 0;
  std::uint64_t row_order_memory = 0;
  std::uint64_t sort_memory = 0;
  if (!AddBatchMemoryBytes(input.batch, &input_memory) ||
      !CheckedMultiply(input_row_count, sizeof(api::EngineSqlTruthValue),
                       &predicate_memory) ||
      !AddBatchMemoryBytes(filtered_input.batch, &filtered_memory) ||
      !AddBatchMemoryBytes(prepared_project.expression_output_batch,
                           &project_output_memory) ||
      !CheckedAdd(input_memory, predicate_memory, &filter_memory) ||
      !CheckedAdd(filter_memory, filtered_memory, &filter_memory) ||
      !CheckedAdd(filtered_memory, project_output_memory,
                  &project_peak_memory) ||
      !CheckedAdd(filter_memory, project_output_memory, &project_memory) ||
      !CheckedMultiply(filtered_row_count, filtered_row_count,
                       &comparison_count) ||
      !CheckedMultiply(filtered_row_count, sizeof(std::size_t),
                       &row_order_memory) ||
      !CheckedAdd(project_memory, project_output_memory, &sort_memory) ||
      !CheckedAdd(sort_memory, comparison_count, &sort_memory) ||
      !CheckedAdd(sort_memory, row_order_memory, &sort_memory) ||
      comparison_count > std::numeric_limits<std::size_t>::max()) {
    return refuse("QOW-DIAG-OPTIMIZER-SEARCH-COST-OVERFLOW-V1",
                  "FILTER/PROJECT/SORT materialization or comparison size "
                  "overflowed");
  }
  if (sort_memory >
      request.optimizer_request.resource.memory_budget_bytes) {
    return refuse("QOW-DIAG-OPTIMIZER-SEARCH-COST-VECTOR-V1",
                  "FILTER/PROJECT/SORT exceeds the admitted memory budget");
  }

  const auto identity_scope = graph.bound_sblr_tree_uuid + ":" +
                              request.context.statement_uuid.canonical;
  const auto values_capability_uuid =
      DerivedCanonicalUuid(identity_scope, "values.capability");
  const auto filter_capability_uuid =
      DerivedCanonicalUuid(identity_scope, "filter.capability");
  const auto project_capability_uuid =
      DerivedCanonicalUuid(identity_scope, "project.capability");
  const auto sort_capability_uuid =
      DerivedCanonicalUuid(identity_scope, "sort.capability");
  const auto deterministic_tie_evidence_uuid = DerivedCanonicalUuid(
      identity_scope + ":" + prepared_sort.ordering_property_uuid,
      "filter-project-sort.deterministic-tie");
  std::vector<LivePhysicalNodeProfile> profiles = {
      {values_node->logical_node_id,
       std::string(kValuesImplementationId),
       values_capability_uuid,
       plan::CanonicalLogicalRelationalNodeKind::kValues,
       exec::PhysicalNodeKind::kValues,
       "canonical.values.materialize.v1",
       input_row_count,
       input_memory,
       0,
       0},
      {filter_node->logical_node_id,
       "filter.3vl.row.v1",
       filter_capability_uuid,
       plan::CanonicalLogicalRelationalNodeKind::kFilter,
       exec::PhysicalNodeKind::kFilter,
       "canonical.filter.3vl.row.v1",
       filtered_row_count,
       filter_memory,
       1,
       1},
      {project_node->logical_node_id,
       "project.typed.expression-row.v1",
       project_capability_uuid,
       plan::CanonicalLogicalRelationalNodeKind::kProject,
       exec::PhysicalNodeKind::kProject,
       "canonical.project.filtered-expression-row.v1",
       filtered_row_count,
       project_peak_memory,
       1,
       1},
      {root->logical_node_id,
       "sort.typed.terms.v1",
       sort_capability_uuid,
       plan::CanonicalLogicalRelationalNodeKind::kSort,
       exec::PhysicalNodeKind::kSort,
       "canonical.sort.filtered-projected-expression.v1",
       filtered_row_count,
       sort_memory,
       1,
       1,
       {},
       {prepared_sort.ordering_property_uuid},
       {plan::CanonicalLogicalPropertyKind::kOrdering}}};
  if (!CompleteLiveRuntimeMemoryReceipts(
          &profiles,
          {{filter_node->logical_node_id, predicate_memory},
           {root->logical_node_id, comparison_count},
           {root->logical_node_id, row_order_memory}})) {
    return refuse("QOW-DIAG-OPT-017-REFUSAL-V1",
                  "FILTER/PROJECT/SORT runtime memory receipts are incomplete");
  }
  auto planning = PlanAndPublishLivePhysicalDag(
      request, profiles, "filter-project-sort.selected-plan",
      "FILTER/PROJECT/SORT");
  if (!planning.ok) {
    return refuse(planning.diagnostic_id, planning.detail);
  }
  result.optimizer_selected = true;
  result.physical_dag_published = true;
  result.physical_node_count = planning.physical_dag.nodes.size();
  result.selected_plan_uuid = planning.physical_dag.selected_plan_uuid;

  std::unordered_map<std::uint64_t, exec::DescriptorBatch> values_batches;
  values_batches.emplace(values_node->logical_node_id, std::move(input.batch));
  auto values_registration = MakeLiveValuesRegistration(
      std::move(values_batches), values_capability_uuid,
      "QOW-DIAG-RELATIONAL-LIVE-FILTER-PROJECT-SORT-VALUES-V1",
      "FILTER/PROJECT/SORT");
  std::vector<api::EngineSqlTruthValue>().swap(predicate_truth_values);
  auto filter_registration = MakeLiveFilterRegistration(
      prepared_filter.predicate_expression_id,
      prepared_filter.predicate_row_binding, request.relational_dag,
      request.expression_services, filter_capability_uuid, input_row_count,
      request.context);
  auto project_registration = MakeLiveProjectRegistration(
      MakeLiveProjectRegistrationProfileForComposition(prepared_project),
      "project.typed.expression-row.v1",
      project_capability_uuid, filtered_row_count, request.relational_dag,
      request.expression_services, request.context);
  auto sort_registration = MakeLiveSortRegistration(
      std::move(prepared_sort.order_terms),
      deterministic_tie_evidence_uuid, sort_capability_uuid,
      filtered_row_count,
      std::max<std::size_t>(1,
                            static_cast<std::size_t>(comparison_count)),
      request.context);

  api::CanonicalOptimizerSelectedExecutionRequest execution_request;
  execution_request.pre_access_statistics_snapshot_uuid =
      planning.physical_dag.statistics_snapshot_uuid;
  execution_request.mga_authority =
      BuildCanonicalExecutionMgaAuthority(request.context,
                                          planning.physical_dag);
  execution_request.selected_physical_dag =
      std::move(planning.physical_dag);
  execution_request.available_executors.push_back(
      std::move(values_registration));
  execution_request.available_executors.push_back(
      std::move(filter_registration));
  execution_request.available_executors.push_back(
      std::move(project_registration));
  execution_request.available_executors.push_back(
      std::move(sort_registration));
  execution_request.engine_execution_authorized = true;
  execution_request.result_publication_request.statement_uuid =
      request.context.statement_uuid.canonical;
  execution_request.result_publication_request.execution_attempt_uuid =
      DerivedCanonicalUuid(
          identity_scope + ":" + request.context.current_monotonic_ns,
          "filter-project-sort.execution-attempt");
  execution_request.result_publication_request
      .transaction_effect_evidence_uuid = DerivedCanonicalUuid(
      identity_scope + ":" +
          std::to_string(request.context.local_transaction_id) + ":" +
          std::to_string(
              request.context.snapshot_visible_through_local_transaction_id),
      "filter-project-sort.transaction-effect-unchanged");
  execution_request.result_publication_request.result_kind =
      exec::CanonicalResultKind::kRows;
  execution_request.result_publication_request.invocation_mode =
      exec::CanonicalResultInvocationMode::kDirect;
  execution_request.result_publication_request.column_bindings =
      std::move(prepared_sort.result_bindings);
  execution_request.result_publication_request.maximum_row_count =
      std::max<std::size_t>(1, filtered_row_count);

  const auto execution =
      ExecuteSelectedCanonicalObjectFreeDag(
          request.context, execution_request,
          planning.ordinary_runtime_memory_receipts);
  if (!execution.accepted || !execution.exact_selected_nodes_executed ||
      !execution.causal_counters_attached ||
      !execution.canonical_result_published || !execution.issues.empty()) {
    return refuse(
        execution.issues.empty()
            ? "QOW-DIAG-RELATIONAL-LIVE-FILTER-PROJECT-SORT-EXECUTION-V1"
            : execution.issues.front().diagnostic_id,
        execution.issues.empty()
            ? "FILTER/PROJECT/SORT selected DAG was not completed"
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

// RCP-037: append a bounded LIMIT to the accepted FILTER/PROJECT/SORT chain
// without republishing or re-planning any intermediate result.
CanonicalObjectFreeValuesExecutionResult
ExecuteCanonicalObjectFreeFilterProjectSortLimitQuery(
    const CanonicalObjectFreeValuesExecutionRequest& request) {
  CanonicalObjectFreeValuesExecutionResult result;
  const auto& graph = request.optimizer_request.logical_graph;
  const auto root = std::ranges::find_if(graph.nodes, [&](const auto& node) {
    return node.logical_node_id == graph.root_logical_node_id;
  });
  if (graph.nodes.size() != 5 || root == graph.nodes.end() ||
      root->node_kind != plan::CanonicalLogicalRelationalNodeKind::kLimit ||
      root->semantic_variant_id != "limit.bound-count.v1" ||
      root->input_logical_node_ids.size() != 1 ||
      root->bound_expression_ids.size() != 1) {
    return result;
  }
  const auto sort_node =
      std::ranges::find_if(graph.nodes, [&](const auto& node) {
        return node.logical_node_id == root->input_logical_node_ids.front();
      });
  if (sort_node == graph.nodes.end() || sort_node == root ||
      sort_node->node_kind !=
          plan::CanonicalLogicalRelationalNodeKind::kSort ||
      sort_node->semantic_variant_id != "sort.required-order.v1" ||
      sort_node->input_logical_node_ids.size() != 1) {
    return result;
  }
  const auto project_node =
      std::ranges::find_if(graph.nodes, [&](const auto& node) {
        return node.logical_node_id == sort_node->input_logical_node_ids.front();
      });
  if (project_node == graph.nodes.end() || project_node == root ||
      project_node == sort_node ||
      project_node->node_kind !=
          plan::CanonicalLogicalRelationalNodeKind::kProject ||
      project_node->semantic_variant_id != "project.select-list.v1" ||
      project_node->input_logical_node_ids.size() != 1 ||
      project_node->bound_expression_ids.empty()) {
    return result;
  }
  const auto filter_node =
      std::ranges::find_if(graph.nodes, [&](const auto& node) {
        return node.logical_node_id ==
               project_node->input_logical_node_ids.front();
      });
  if (filter_node == graph.nodes.end() || filter_node == root ||
      filter_node == sort_node || filter_node == project_node ||
      filter_node->node_kind !=
          plan::CanonicalLogicalRelationalNodeKind::kFilter ||
      filter_node->semantic_variant_id != "filter.where.v1" ||
      filter_node->input_logical_node_ids.size() != 1 ||
      filter_node->bound_expression_ids.size() != 1) {
    return result;
  }
  const auto values_node =
      std::ranges::find_if(graph.nodes, [&](const auto& node) {
        return node.logical_node_id ==
               filter_node->input_logical_node_ids.front();
      });
  if (values_node == graph.nodes.end() || values_node == root ||
      values_node == sort_node || values_node == project_node ||
      values_node == filter_node ||
      values_node->node_kind !=
          plan::CanonicalLogicalRelationalNodeKind::kValues ||
      values_node->semantic_variant_id != "values.literal-table.v1" ||
      !values_node->input_logical_node_ids.empty()) {
    return result;
  }
  for (const auto& node : graph.nodes) {
    if (!node.required_object_uuids.empty() ||
        (node.logical_node_id != sort_node->logical_node_id &&
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
  if (!request.optimizer_admission.admitted ||
      !request.optimizer_admission.planning_allowed) {
    return refuse(
        "QOW-DIAG-RELATIONAL-LIVE-FILTER-PROJECT-SORT-LIMIT-ADMISSION-V1",
        "FILTER/PROJECT/SORT/LIMIT composition lacks optimizer admission");
  }

  auto input = MaterializeValues(request.relational_dag, *values_node,
                                 request.expression_services);
  if (!input.ok) {
    return refuse(
        "QOW-DIAG-RELATIONAL-LIVE-FILTER-PROJECT-SORT-LIMIT-PAYLOAD-V1",
        "FILTER/PROJECT/SORT/LIMIT input VALUES: " + input.detail);
  }
  auto prepared_filter = PrepareFilterRootForComposition(
      request.relational_dag, *filter_node, *values_node, input);
  if (!prepared_filter.ok) {
    return refuse(
        "QOW-DIAG-RELATIONAL-LIVE-FILTER-PROJECT-SORT-LIMIT-PAYLOAD-V1",
        prepared_filter.detail);
  }

  const auto input_row_count = input.batch.rows.size();
  CanonicalRelationalExpressionRuntime expression_runtime(
      request.relational_dag, request.expression_services);
  std::vector<api::EngineSqlTruthValue> predicate_truth_values;
  predicate_truth_values.reserve(input_row_count);
  MaterializedValues filtered_input;
  filtered_input.ok = true;
  filtered_input.batch.columns = input.batch.columns;
  filtered_input.batch.rows.reserve(input_row_count);
  filtered_input.result_bindings = prepared_filter.result_bindings;
  for (const auto& row : input.batch.rows) {
    api::EngineSqlTruthValue predicate_truth =
        api::EngineSqlTruthValue::unknown;
    std::string predicate_detail;
    if (!expression_runtime.EvaluatePredicateForConsumer(
            prepared_filter.predicate_expression_id,
            prepared_filter.predicate_row_binding, row.values,
            api::EngineCanonicalExpressionConsumer::filter,
            &predicate_truth, &predicate_detail)) {
      return refuse(
          "QOW-DIAG-RELATIONAL-LIVE-FILTER-PROJECT-SORT-LIMIT-PAYLOAD-V1",
          "FILTER predicate row " +
              std::to_string(predicate_truth_values.size()) + ": " +
              predicate_detail);
    }
    predicate_truth_values.push_back(predicate_truth);
    if (predicate_truth == api::EngineSqlTruthValue::true_value) {
      filtered_input.batch.rows.push_back(row);
    }
  }
  std::uint64_t project_expression_work = 0;
  std::uint64_t total_expression_work = 0;
  if (!CheckedMultiply(filtered_input.batch.rows.size(),
                       project_node->bound_expression_ids.size(),
                       &project_expression_work) ||
      !CheckedAdd(input_row_count, project_expression_work,
                  &total_expression_work) ||
      !CheckedAdd(total_expression_work, root->bound_expression_ids.size(),
                  &total_expression_work)) {
    return refuse("QOW-DIAG-OPTIMIZER-SEARCH-COST-OVERFLOW-V1",
                  "FILTER/PROJECT/SORT/LIMIT expression work overflowed");
  }
  if (total_expression_work >
      request.optimizer_request.resource.maximum_candidate_count) {
    return refuse("QOW-DIAG-OPTIMIZER-SEARCH-COST-VECTOR-V1",
                  "FILTER/PROJECT/SORT/LIMIT expression work exceeds the "
                  "admitted candidate bound");
  }

  auto prepared_project = PrepareExpressionProjectRootForComposition(
      request.relational_dag, *project_node, *filter_node, filtered_input,
      request.expression_services);
  if (!prepared_project.ok || !prepared_project.expression_projection) {
    return refuse(
        "QOW-DIAG-RELATIONAL-LIVE-FILTER-PROJECT-SORT-LIMIT-PAYLOAD-V1",
        prepared_project.detail.empty()
            ? "FILTER/PROJECT/SORT/LIMIT requires an expression projection"
            : prepared_project.detail);
  }
  MaterializedValues projected_input;
  projected_input.ok = true;
  projected_input.batch = prepared_project.expression_output_batch;
  projected_input.result_bindings = prepared_project.result_bindings;
  auto prepared_sort = PrepareSortRootForComposition(
      request.context, request.relational_dag,
      request.optimizer_request.logical_properties, *sort_node,
      *project_node, projected_input);
  if (!prepared_sort.ok) {
    return refuse(
        "QOW-DIAG-RELATIONAL-LIVE-FILTER-PROJECT-SORT-LIMIT-PAYLOAD-V1",
        prepared_sort.detail);
  }
  auto prepared_limit = PrepareLimitRootForComposition(
      request.relational_dag, *root, *sort_node, projected_input);
  if (!prepared_limit.ok) {
    return refuse(
        "QOW-DIAG-RELATIONAL-LIVE-FILTER-PROJECT-SORT-LIMIT-PAYLOAD-V1",
        prepared_limit.detail);
  }
  std::uint64_t row_limit = 0;
  std::string bound_detail;
  if (!EvaluateNonNegativeRowBoundForComposition(
          &expression_runtime, root->bound_expression_ids.front(), &row_limit,
          &bound_detail)) {
    return refuse(
        "QOW-DIAG-RELATIONAL-LIVE-FILTER-PROJECT-SORT-LIMIT-PAYLOAD-V1",
        "LIMIT bound: " + bound_detail);
  }

  const auto filtered_row_count = filtered_input.batch.rows.size();
  const auto output_row_bound =
      row_limit > filtered_row_count
          ? filtered_row_count
          : static_cast<std::size_t>(row_limit);
  std::uint64_t input_memory = 1;
  std::uint64_t predicate_memory = 0;
  std::uint64_t filtered_memory = 1;
  std::uint64_t project_output_memory = 1;
  std::uint64_t filter_memory = 0;
  std::uint64_t project_peak_memory = 0;
  std::uint64_t project_memory = 0;
  std::uint64_t comparison_count = 0;
  std::uint64_t row_order_memory = 0;
  std::uint64_t sort_memory = 0;
  std::uint64_t limit_memory = 0;
  if (!AddBatchMemoryBytes(input.batch, &input_memory) ||
      !CheckedMultiply(input_row_count, sizeof(api::EngineSqlTruthValue),
                       &predicate_memory) ||
      !AddBatchMemoryBytes(filtered_input.batch, &filtered_memory) ||
      !AddBatchMemoryBytes(prepared_project.expression_output_batch,
                           &project_output_memory) ||
      !CheckedAdd(input_memory, predicate_memory, &filter_memory) ||
      !CheckedAdd(filter_memory, filtered_memory, &filter_memory) ||
      !CheckedAdd(filtered_memory, project_output_memory,
                  &project_peak_memory) ||
      !CheckedAdd(filter_memory, project_output_memory, &project_memory) ||
      !CheckedMultiply(filtered_row_count, filtered_row_count,
                       &comparison_count) ||
      !CheckedMultiply(filtered_row_count, sizeof(std::size_t),
                       &row_order_memory) ||
      !CheckedAdd(project_memory, project_output_memory, &sort_memory) ||
      !CheckedAdd(sort_memory, comparison_count, &sort_memory) ||
      !CheckedAdd(sort_memory, row_order_memory, &sort_memory) ||
      !CheckedAdd(sort_memory, project_output_memory, &limit_memory) ||
      comparison_count > std::numeric_limits<std::size_t>::max()) {
    return refuse("QOW-DIAG-OPTIMIZER-SEARCH-COST-OVERFLOW-V1",
                  "FILTER/PROJECT/SORT/LIMIT materialization or comparison "
                  "size overflowed");
  }
  if (limit_memory >
      request.optimizer_request.resource.memory_budget_bytes) {
    return refuse("QOW-DIAG-OPTIMIZER-SEARCH-COST-VECTOR-V1",
                  "FILTER/PROJECT/SORT/LIMIT exceeds the admitted memory "
                  "budget");
  }

  const auto identity_scope = graph.bound_sblr_tree_uuid + ":" +
                              request.context.statement_uuid.canonical;
  const auto values_capability_uuid =
      DerivedCanonicalUuid(identity_scope, "values.capability");
  const auto filter_capability_uuid =
      DerivedCanonicalUuid(identity_scope, "filter.capability");
  const auto project_capability_uuid =
      DerivedCanonicalUuid(identity_scope, "project.capability");
  const auto sort_capability_uuid =
      DerivedCanonicalUuid(identity_scope, "sort.capability");
  const auto limit_capability_uuid =
      DerivedCanonicalUuid(identity_scope, "limit.capability");
  const auto deterministic_tie_evidence_uuid = DerivedCanonicalUuid(
      identity_scope + ":" + prepared_sort.ordering_property_uuid,
      "filter-project-sort-limit.deterministic-tie");
  std::vector<LivePhysicalNodeProfile> profiles = {
      {values_node->logical_node_id,
       std::string(kValuesImplementationId),
       values_capability_uuid,
       plan::CanonicalLogicalRelationalNodeKind::kValues,
       exec::PhysicalNodeKind::kValues,
       "canonical.values.materialize.v1",
       input_row_count,
       input_memory,
       0,
       0},
      {filter_node->logical_node_id,
       "filter.3vl.row.v1",
       filter_capability_uuid,
       plan::CanonicalLogicalRelationalNodeKind::kFilter,
       exec::PhysicalNodeKind::kFilter,
       "canonical.filter.3vl.row.v1",
       filtered_row_count,
       filter_memory,
       1,
       1},
      {project_node->logical_node_id,
       "project.typed.expression-row.v1",
       project_capability_uuid,
       plan::CanonicalLogicalRelationalNodeKind::kProject,
       exec::PhysicalNodeKind::kProject,
       "canonical.project.filtered-expression-row.v1",
       filtered_row_count,
       project_peak_memory,
       1,
       1},
      {sort_node->logical_node_id,
       "sort.typed.terms.v1",
       sort_capability_uuid,
       plan::CanonicalLogicalRelationalNodeKind::kSort,
       exec::PhysicalNodeKind::kSort,
       "canonical.sort.filtered-projected-expression.v1",
       filtered_row_count,
       sort_memory,
       1,
       1,
       {},
       {prepared_sort.ordering_property_uuid},
       {plan::CanonicalLogicalPropertyKind::kOrdering}},
      {root->logical_node_id,
       "limit.typed.v1",
       limit_capability_uuid,
       plan::CanonicalLogicalRelationalNodeKind::kLimit,
       exec::PhysicalNodeKind::kLimit,
       "canonical.limit.filtered-projected-order.v1",
       output_row_bound,
       limit_memory,
       1,
       1}};
  if (!CompleteLiveRuntimeMemoryReceipts(
          &profiles,
          {{filter_node->logical_node_id, predicate_memory},
           {sort_node->logical_node_id, comparison_count},
           {sort_node->logical_node_id, row_order_memory}})) {
    return refuse("QOW-DIAG-OPT-017-REFUSAL-V1",
                  "FILTER/PROJECT/SORT/LIMIT runtime memory receipts are incomplete");
  }
  auto planning = PlanAndPublishLivePhysicalDag(
      request, profiles, "filter-project-sort-limit.selected-plan",
      "FILTER/PROJECT/SORT/LIMIT");
  if (!planning.ok) {
    return refuse(planning.diagnostic_id, planning.detail);
  }
  result.optimizer_selected = true;
  result.physical_dag_published = true;
  result.physical_node_count = planning.physical_dag.nodes.size();
  result.selected_plan_uuid = planning.physical_dag.selected_plan_uuid;

  std::unordered_map<std::uint64_t, exec::DescriptorBatch> values_batches;
  values_batches.emplace(values_node->logical_node_id, std::move(input.batch));
  auto values_registration = MakeLiveValuesRegistration(
      std::move(values_batches), values_capability_uuid,
      "QOW-DIAG-RELATIONAL-LIVE-FILTER-PROJECT-SORT-LIMIT-VALUES-V1",
      "FILTER/PROJECT/SORT/LIMIT");
  std::vector<api::EngineSqlTruthValue>().swap(predicate_truth_values);
  auto filter_registration = MakeLiveFilterRegistration(
      prepared_filter.predicate_expression_id,
      prepared_filter.predicate_row_binding, request.relational_dag,
      request.expression_services, filter_capability_uuid, input_row_count,
      request.context);
  auto project_registration = MakeLiveProjectRegistration(
      MakeLiveProjectRegistrationProfileForComposition(prepared_project),
      "project.typed.expression-row.v1",
      project_capability_uuid, filtered_row_count, request.relational_dag,
      request.expression_services, request.context);
  auto sort_registration = MakeLiveSortRegistration(
      std::move(prepared_sort.order_terms),
      deterministic_tie_evidence_uuid, sort_capability_uuid,
      filtered_row_count,
      std::max<std::size_t>(1,
                            static_cast<std::size_t>(comparison_count)),
      request.context);
  auto limit_registration = MakeLiveLimitRegistration(
      "limit.typed.v1", limit_capability_uuid, row_limit, 0, false,
      filtered_row_count, request.context);

  api::CanonicalOptimizerSelectedExecutionRequest execution_request;
  execution_request.pre_access_statistics_snapshot_uuid =
      planning.physical_dag.statistics_snapshot_uuid;
  execution_request.mga_authority =
      BuildCanonicalExecutionMgaAuthority(request.context,
                                          planning.physical_dag);
  execution_request.selected_physical_dag =
      std::move(planning.physical_dag);
  execution_request.available_executors.push_back(
      std::move(values_registration));
  execution_request.available_executors.push_back(
      std::move(filter_registration));
  execution_request.available_executors.push_back(
      std::move(project_registration));
  execution_request.available_executors.push_back(
      std::move(sort_registration));
  execution_request.available_executors.push_back(
      std::move(limit_registration));
  execution_request.engine_execution_authorized = true;
  execution_request.result_publication_request.statement_uuid =
      request.context.statement_uuid.canonical;
  execution_request.result_publication_request.execution_attempt_uuid =
      DerivedCanonicalUuid(
          identity_scope + ":" + request.context.current_monotonic_ns,
          "filter-project-sort-limit.execution-attempt");
  execution_request.result_publication_request
      .transaction_effect_evidence_uuid = DerivedCanonicalUuid(
      identity_scope + ":" +
          std::to_string(request.context.local_transaction_id) + ":" +
          std::to_string(
              request.context.snapshot_visible_through_local_transaction_id),
      "filter-project-sort-limit.transaction-effect-unchanged");
  execution_request.result_publication_request.result_kind =
      exec::CanonicalResultKind::kRows;
  execution_request.result_publication_request.invocation_mode =
      exec::CanonicalResultInvocationMode::kDirect;
  execution_request.result_publication_request.column_bindings =
      std::move(prepared_limit.result_bindings);
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
            ? "QOW-DIAG-RELATIONAL-LIVE-FILTER-PROJECT-SORT-LIMIT-EXECUTION-V1"
            : execution.issues.front().diagnostic_id,
        execution.issues.empty()
            ? "FILTER/PROJECT/SORT/LIMIT selected DAG was not completed"
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

// RCP-038: add full projected-row DISTINCT before ORDER BY and LIMIT in the
// accepted filtered SQL tail.
// RCP-039: carry the already signed LIMIT/OFFSET and FETCH FIRST ROWS ONLY
// profiles through the same exact six-node causal route.
CanonicalObjectFreeValuesExecutionResult
ExecuteCanonicalObjectFreeFilterProjectDistinctSortLimitQuery(
    const CanonicalObjectFreeValuesExecutionRequest& request) {
  CanonicalObjectFreeValuesExecutionResult result;
  const auto& graph = request.optimizer_request.logical_graph;
  const auto root = std::ranges::find_if(graph.nodes, [&](const auto& node) {
    return node.logical_node_id == graph.root_logical_node_id;
  });
  if (graph.nodes.size() != 6 || root == graph.nodes.end() ||
      root->node_kind != plan::CanonicalLogicalRelationalNodeKind::kLimit ||
      (root->semantic_variant_id != "limit.bound-count.v1" &&
       root->semantic_variant_id != "limit.bound-count-offset.v1" &&
       root->semantic_variant_id !=
           "fetch.first-rows-only-offset.v1") ||
      root->input_logical_node_ids.size() != 1 ||
      (root->semantic_variant_id == "limit.bound-count.v1"
           ? root->bound_expression_ids.size() != 1
           : root->bound_expression_ids.size() != 2)) {
    return result;
  }
  const bool fetch_first_rows_only =
      root->semantic_variant_id == "fetch.first-rows-only-offset.v1";
  const bool has_offset = root->bound_expression_ids.size() == 2;
  const auto sort_node =
      std::ranges::find_if(graph.nodes, [&](const auto& node) {
        return node.logical_node_id == root->input_logical_node_ids.front();
      });
  if (sort_node == graph.nodes.end() || sort_node == root ||
      sort_node->node_kind !=
          plan::CanonicalLogicalRelationalNodeKind::kSort ||
      sort_node->semantic_variant_id != "sort.required-order.v1" ||
      sort_node->input_logical_node_ids.size() != 1) {
    return result;
  }
  const auto distinct_node =
      std::ranges::find_if(graph.nodes, [&](const auto& node) {
        return node.logical_node_id == sort_node->input_logical_node_ids.front();
      });
  if (distinct_node == graph.nodes.end() || distinct_node == root ||
      distinct_node == sort_node ||
      distinct_node->node_kind !=
          plan::CanonicalLogicalRelationalNodeKind::kAggregate ||
      distinct_node->semantic_variant_id != "aggregate.query-distinct.v1" ||
      distinct_node->input_logical_node_ids.size() != 1) {
    return result;
  }
  const auto project_node =
      std::ranges::find_if(graph.nodes, [&](const auto& node) {
        return node.logical_node_id ==
               distinct_node->input_logical_node_ids.front();
      });
  if (project_node == graph.nodes.end() || project_node == root ||
      project_node == sort_node || project_node == distinct_node ||
      project_node->node_kind !=
          plan::CanonicalLogicalRelationalNodeKind::kProject ||
      project_node->semantic_variant_id != "project.select-list.v1" ||
      project_node->input_logical_node_ids.size() != 1 ||
      project_node->bound_expression_ids.empty()) {
    return result;
  }
  const auto filter_node =
      std::ranges::find_if(graph.nodes, [&](const auto& node) {
        return node.logical_node_id ==
               project_node->input_logical_node_ids.front();
      });
  if (filter_node == graph.nodes.end() || filter_node == root ||
      filter_node == sort_node || filter_node == distinct_node ||
      filter_node == project_node ||
      filter_node->node_kind !=
          plan::CanonicalLogicalRelationalNodeKind::kFilter ||
      filter_node->semantic_variant_id != "filter.where.v1" ||
      filter_node->input_logical_node_ids.size() != 1 ||
      filter_node->bound_expression_ids.size() != 1) {
    return result;
  }
  const auto values_node =
      std::ranges::find_if(graph.nodes, [&](const auto& node) {
        return node.logical_node_id ==
               filter_node->input_logical_node_ids.front();
      });
  if (values_node == graph.nodes.end() || values_node == root ||
      values_node == sort_node || values_node == distinct_node ||
      values_node == project_node || values_node == filter_node ||
      values_node->node_kind !=
          plan::CanonicalLogicalRelationalNodeKind::kValues ||
      values_node->semantic_variant_id != "values.literal-table.v1" ||
      !values_node->input_logical_node_ids.empty()) {
    return result;
  }
  for (const auto& node : graph.nodes) {
    if (!node.required_object_uuids.empty() ||
        (node.logical_node_id != sort_node->logical_node_id &&
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
      "QOW-DIAG-RELATIONAL-LIVE-FILTER-PROJECT-DISTINCT-SORT-LIMIT-PAYLOAD-V1";
  if (!request.optimizer_admission.admitted ||
      !request.optimizer_admission.planning_allowed) {
    return refuse(
        "QOW-DIAG-RELATIONAL-LIVE-FILTER-PROJECT-DISTINCT-SORT-LIMIT-ADMISSION-V1",
        "FILTER/PROJECT/DISTINCT/SORT/LIMIT lacks optimizer admission");
  }

  auto input = MaterializeValues(request.relational_dag, *values_node,
                                 request.expression_services);
  if (!input.ok) {
    return refuse(std::string(kPayloadDiagnostic),
                  "full SQL tail input VALUES: " + input.detail);
  }
  auto prepared_filter = PrepareFilterRootForComposition(
      request.relational_dag, *filter_node, *values_node, input);
  if (!prepared_filter.ok) {
    return refuse(std::string(kPayloadDiagnostic), prepared_filter.detail);
  }

  const auto input_row_count = input.batch.rows.size();
  CanonicalRelationalExpressionRuntime expression_runtime(
      request.relational_dag, request.expression_services);
  std::vector<api::EngineSqlTruthValue> predicate_truth_values;
  predicate_truth_values.reserve(input_row_count);
  MaterializedValues filtered_input;
  filtered_input.ok = true;
  filtered_input.batch.columns = input.batch.columns;
  filtered_input.batch.rows.reserve(input_row_count);
  filtered_input.result_bindings = prepared_filter.result_bindings;
  for (const auto& row : input.batch.rows) {
    api::EngineSqlTruthValue predicate_truth =
        api::EngineSqlTruthValue::unknown;
    std::string predicate_detail;
    if (!expression_runtime.EvaluatePredicateForConsumer(
            prepared_filter.predicate_expression_id,
            prepared_filter.predicate_row_binding, row.values,
            api::EngineCanonicalExpressionConsumer::filter,
            &predicate_truth, &predicate_detail)) {
      return refuse(std::string(kPayloadDiagnostic),
                    "FILTER predicate row " +
                        std::to_string(predicate_truth_values.size()) + ": " +
                        predicate_detail);
    }
    predicate_truth_values.push_back(predicate_truth);
    if (predicate_truth == api::EngineSqlTruthValue::true_value) {
      filtered_input.batch.rows.push_back(row);
    }
  }
  std::uint64_t project_expression_work = 0;
  std::uint64_t total_expression_work = 0;
  if (!CheckedMultiply(filtered_input.batch.rows.size(),
                       project_node->bound_expression_ids.size(),
                       &project_expression_work) ||
      !CheckedAdd(input_row_count, project_expression_work,
                  &total_expression_work) ||
      !CheckedAdd(total_expression_work, root->bound_expression_ids.size(),
                  &total_expression_work)) {
    return refuse("QOW-DIAG-OPTIMIZER-SEARCH-COST-OVERFLOW-V1",
                  "full SQL-tail expression work overflowed");
  }
  if (total_expression_work >
      request.optimizer_request.resource.maximum_candidate_count) {
    return refuse("QOW-DIAG-OPTIMIZER-SEARCH-COST-VECTOR-V1",
                  "full SQL-tail expression work exceeds the admitted "
                  "candidate bound");
  }

  auto prepared_project = PrepareExpressionProjectRootForComposition(
      request.relational_dag, *project_node, *filter_node, filtered_input,
      request.expression_services);
  if (!prepared_project.ok || !prepared_project.expression_projection) {
    return refuse(std::string(kPayloadDiagnostic),
                  prepared_project.detail.empty()
                      ? "full SQL tail requires an expression projection"
                      : prepared_project.detail);
  }
  MaterializedValues projected_input;
  projected_input.ok = true;
  projected_input.batch = prepared_project.expression_output_batch;
  projected_input.result_bindings = prepared_project.result_bindings;
  auto prepared_distinct = PrepareQueryDistinctRootForComposition(
      request.context, request.relational_dag, *distinct_node, *project_node,
      projected_input);
  if (!prepared_distinct.ok) {
    return refuse(std::string(kPayloadDiagnostic), prepared_distinct.detail);
  }
  auto prepared_sort = PrepareSortRootForComposition(
      request.context, request.relational_dag,
      request.optimizer_request.logical_properties, *sort_node,
      *distinct_node, projected_input);
  if (!prepared_sort.ok) {
    return refuse(std::string(kPayloadDiagnostic), prepared_sort.detail);
  }
  auto prepared_limit = PrepareLimitRootForComposition(
      request.relational_dag, *root, *sort_node, projected_input);
  if (!prepared_limit.ok) {
    return refuse(std::string(kPayloadDiagnostic), prepared_limit.detail);
  }
  std::uint64_t row_limit = 0;
  std::uint64_t row_offset = 0;
  std::string bound_detail;
  if (!EvaluateNonNegativeRowBoundForComposition(
          &expression_runtime, root->bound_expression_ids.front(), &row_limit,
          &bound_detail) ||
      (has_offset &&
       !EvaluateNonNegativeRowBoundForComposition(
           &expression_runtime, root->bound_expression_ids[1], &row_offset,
           &bound_detail))) {
    return refuse(std::string(kPayloadDiagnostic),
                  "LIMIT/FETCH bound: " + bound_detail);
  }

  const auto filtered_row_count = filtered_input.batch.rows.size();
  const auto offset_bound =
      row_offset > filtered_row_count
          ? filtered_row_count
          : static_cast<std::size_t>(row_offset);
  const auto remaining_bound = filtered_row_count - offset_bound;
  const auto output_row_bound =
      row_limit > remaining_bound
          ? remaining_bound
          : static_cast<std::size_t>(row_limit);
  std::uint64_t input_memory = 1;
  std::uint64_t predicate_memory = 0;
  std::uint64_t filtered_memory = 1;
  std::uint64_t project_output_memory = 1;
  std::uint64_t filter_memory = 0;
  std::uint64_t project_peak_memory = 0;
  std::uint64_t project_memory = 0;
  std::uint64_t distinct_comparison_count = 0;
  std::uint64_t distinct_self_comparison_count = 0;
  std::uint64_t distinct_output_memory = 1;
  std::uint64_t distinct_auxiliary_memory = 0;
  std::uint64_t distinct_memory = 0;
  std::uint64_t sort_comparison_count = 0;
  std::uint64_t row_order_memory = 0;
  std::uint64_t sort_memory = 0;
  std::uint64_t limit_memory = 0;
  std::string distinct_memory_detail;
  if (!QueryDistinctAuxiliaryMemoryBytes(
          filtered_row_count, projected_input.batch.columns.size(),
          &distinct_auxiliary_memory) ||
      !CheckedMultiply(filtered_row_count, filtered_row_count,
                       &distinct_comparison_count) ||
      !CheckedMultiply(distinct_comparison_count,
                       projected_input.batch.columns.size(),
                       &distinct_comparison_count) ||
      !CheckedMultiply(filtered_row_count,
                       projected_input.batch.columns.size(),
                       &distinct_self_comparison_count) ||
      !CheckedAdd(distinct_comparison_count,
                  distinct_self_comparison_count,
                  &distinct_comparison_count) ||
      distinct_comparison_count > std::numeric_limits<std::size_t>::max() ||
      !CheckedAdd(total_expression_work, distinct_comparison_count,
                  &total_expression_work)) {
    return refuse("QOW-DIAG-OPTIMIZER-SEARCH-COST-OVERFLOW-V1",
                  "full SQL-tail DISTINCT work or resident state overflowed");
  }
  if (total_expression_work >
      request.optimizer_request.resource.maximum_candidate_count) {
    return refuse("QOW-DIAG-OPTIMIZER-SEARCH-COST-VECTOR-V1",
                  "full SQL-tail DISTINCT work exceeds the admitted "
                  "candidate bound");
  }
  if (!AddBatchMemoryBytes(input.batch, &input_memory) ||
      !CheckedMultiply(input_row_count, sizeof(api::EngineSqlTruthValue),
                       &predicate_memory) ||
      !AddBatchMemoryBytes(filtered_input.batch, &filtered_memory) ||
      !AddBatchMemoryBytes(prepared_project.expression_output_batch,
                           &project_output_memory) ||
      !CheckedAdd(input_memory, predicate_memory, &filter_memory) ||
      !CheckedAdd(filter_memory, filtered_memory, &filter_memory) ||
      !CheckedAdd(filtered_memory, project_output_memory,
                  &project_peak_memory) ||
      !CheckedAdd(filter_memory, project_output_memory, &project_memory) ||
      !QueryDistinctOutputMemoryBytes(
          projected_input.batch, prepared_distinct.equality_terms,
          static_cast<std::size_t>(distinct_comparison_count),
          &distinct_output_memory, &distinct_memory_detail) ||
      !CheckedAdd(project_output_memory, distinct_output_memory,
                  &distinct_memory) ||
      !CheckedAdd(distinct_memory, distinct_auxiliary_memory,
                  &distinct_memory) ||
      !CheckedMultiply(filtered_row_count, filtered_row_count,
                       &sort_comparison_count) ||
      !CheckedMultiply(filtered_row_count, sizeof(std::size_t),
                       &row_order_memory) ||
      !CheckedAdd(distinct_memory, project_output_memory, &sort_memory) ||
      !CheckedAdd(sort_memory, sort_comparison_count, &sort_memory) ||
      !CheckedAdd(sort_memory, row_order_memory, &sort_memory) ||
      !CheckedAdd(sort_memory, project_output_memory, &limit_memory) ||
      distinct_comparison_count > std::numeric_limits<std::size_t>::max() ||
      sort_comparison_count > std::numeric_limits<std::size_t>::max()) {
    return refuse("QOW-DIAG-OPTIMIZER-SEARCH-COST-OVERFLOW-V1",
                  "full SQL-tail materialization or comparison size "
                  "overflowed");
  }
  if (limit_memory >
      request.optimizer_request.resource.memory_budget_bytes) {
    return refuse("QOW-DIAG-OPTIMIZER-SEARCH-COST-VECTOR-V1",
                  "full SQL tail exceeds the admitted memory budget");
  }

  const auto identity_scope = graph.bound_sblr_tree_uuid + ":" +
                              request.context.statement_uuid.canonical;
  const auto values_capability_uuid =
      DerivedCanonicalUuid(identity_scope, "values.capability");
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
  const auto deterministic_tie_evidence_uuid = DerivedCanonicalUuid(
      identity_scope + ":" + prepared_sort.ordering_property_uuid,
      "filter-project-distinct-sort-limit.deterministic-tie");
  const std::string limit_implementation_id =
      fetch_first_rows_only ? "fetch.native.rows-only.v1"
                            : "limit.typed.v1";
  const std::string limit_semantic_id =
      fetch_first_rows_only
          ? "canonical.fetch.first-rows-only-offset.filtered-projected-"
            "distinct-order.v1"
          : (has_offset
                 ? "canonical.limit.bound-count-offset.filtered-projected-"
                   "distinct-order.v1"
                 : "canonical.limit.filtered-projected-distinct-order.v1");
  std::vector<LivePhysicalNodeProfile> profiles = {
      {values_node->logical_node_id, std::string(kValuesImplementationId),
       values_capability_uuid,
       plan::CanonicalLogicalRelationalNodeKind::kValues,
       exec::PhysicalNodeKind::kValues, "canonical.values.materialize.v1",
       input_row_count, input_memory, 0, 0},
      {filter_node->logical_node_id, "filter.3vl.row.v1",
       filter_capability_uuid,
       plan::CanonicalLogicalRelationalNodeKind::kFilter,
       exec::PhysicalNodeKind::kFilter, "canonical.filter.3vl.row.v1",
       filtered_row_count, filter_memory, 1, 1},
      {project_node->logical_node_id, "project.typed.expression-row.v1",
       project_capability_uuid,
       plan::CanonicalLogicalRelationalNodeKind::kProject,
       exec::PhysicalNodeKind::kProject,
       "canonical.project.filtered-expression-row.v1", filtered_row_count,
       project_peak_memory, 1, 1},
      {distinct_node->logical_node_id, "aggregate.query-distinct.typed.v1",
       distinct_capability_uuid,
       plan::CanonicalLogicalRelationalNodeKind::kAggregate,
       exec::PhysicalNodeKind::kAggregate,
       "canonical.aggregate.projected-query-distinct.v1", filtered_row_count,
       distinct_memory, 1, 1},
      {sort_node->logical_node_id, "sort.typed.terms.v1",
       sort_capability_uuid,
       plan::CanonicalLogicalRelationalNodeKind::kSort,
       exec::PhysicalNodeKind::kSort,
       "canonical.sort.distinct-projected-expression.v1", filtered_row_count,
       sort_memory, 1, 1, {}, {prepared_sort.ordering_property_uuid},
       {plan::CanonicalLogicalPropertyKind::kOrdering}},
      {root->logical_node_id, limit_implementation_id, limit_capability_uuid,
       plan::CanonicalLogicalRelationalNodeKind::kLimit,
       exec::PhysicalNodeKind::kLimit,
       limit_semantic_id, output_row_bound, limit_memory, 1, 1}};
  if (!CompleteLiveRuntimeMemoryReceipts(
          &profiles,
          {{filter_node->logical_node_id, predicate_memory},
           {distinct_node->logical_node_id, distinct_auxiliary_memory},
           {sort_node->logical_node_id, sort_comparison_count},
           {sort_node->logical_node_id, row_order_memory}})) {
    return refuse("QOW-DIAG-OPT-017-REFUSAL-V1",
                  "full SQL-tail runtime memory receipts are incomplete");
  }
  auto planning = PlanAndPublishLivePhysicalDag(
      request, profiles,
      fetch_first_rows_only
          ? "filter-project-distinct-sort-fetch.selected-plan"
          : (has_offset
                 ? "filter-project-distinct-sort-limit-offset.selected-plan"
                 : "filter-project-distinct-sort-limit.selected-plan"),
      "FILTER/PROJECT/DISTINCT/SORT/LIMIT/FETCH");
  if (!planning.ok) return refuse(planning.diagnostic_id, planning.detail);
  result.optimizer_selected = true;
  result.physical_dag_published = true;
  result.physical_node_count = planning.physical_dag.nodes.size();
  result.selected_plan_uuid = planning.physical_dag.selected_plan_uuid;

  std::unordered_map<std::uint64_t, exec::DescriptorBatch> values_batches;
  values_batches.emplace(values_node->logical_node_id, std::move(input.batch));
  auto values_registration = MakeLiveValuesRegistration(
      std::move(values_batches), values_capability_uuid,
      "QOW-DIAG-RELATIONAL-LIVE-FULL-SQL-TAIL-VALUES-V1",
      "FILTER/PROJECT/DISTINCT/SORT/LIMIT/FETCH");
  std::vector<api::EngineSqlTruthValue>().swap(predicate_truth_values);
  auto filter_registration = MakeLiveFilterRegistration(
      prepared_filter.predicate_expression_id,
      prepared_filter.predicate_row_binding, request.relational_dag,
      request.expression_services, filter_capability_uuid, input_row_count,
      request.context);
  auto project_registration = MakeLiveProjectRegistration(
      MakeLiveProjectRegistrationProfileForComposition(prepared_project),
      "project.typed.expression-row.v1",
      project_capability_uuid, filtered_row_count, request.relational_dag,
      request.expression_services, request.context);
  auto distinct_registration = MakeLiveQueryDistinctRegistration(
      std::move(prepared_distinct.equality_terms), distinct_capability_uuid,
      filtered_row_count,
      std::max<std::size_t>(
          1, static_cast<std::size_t>(distinct_comparison_count)),
      request.context);
  auto sort_registration = MakeLiveSortRegistration(
      std::move(prepared_sort.order_terms), deterministic_tie_evidence_uuid,
      sort_capability_uuid, filtered_row_count,
      std::max<std::size_t>(1,
                            static_cast<std::size_t>(sort_comparison_count)),
      request.context);
  auto limit_registration = MakeLiveLimitRegistration(
      limit_implementation_id, limit_capability_uuid, row_limit, row_offset,
      fetch_first_rows_only, filtered_row_count, request.context);

  api::CanonicalOptimizerSelectedExecutionRequest execution_request;
  execution_request.pre_access_statistics_snapshot_uuid =
      planning.physical_dag.statistics_snapshot_uuid;
  execution_request.mga_authority =
      BuildCanonicalExecutionMgaAuthority(request.context,
                                          planning.physical_dag);
  execution_request.selected_physical_dag =
      std::move(planning.physical_dag);
  execution_request.available_executors.push_back(
      std::move(values_registration));
  execution_request.available_executors.push_back(
      std::move(filter_registration));
  execution_request.available_executors.push_back(
      std::move(project_registration));
  execution_request.available_executors.push_back(
      std::move(distinct_registration));
  execution_request.available_executors.push_back(
      std::move(sort_registration));
  execution_request.available_executors.push_back(
      std::move(limit_registration));
  execution_request.engine_execution_authorized = true;
  execution_request.result_publication_request.statement_uuid =
      request.context.statement_uuid.canonical;
  execution_request.result_publication_request.execution_attempt_uuid =
      DerivedCanonicalUuid(
          identity_scope + ":" + request.context.current_monotonic_ns,
          "filter-project-distinct-sort-limit.execution-attempt");
  execution_request.result_publication_request
      .transaction_effect_evidence_uuid = DerivedCanonicalUuid(
      identity_scope + ":" +
          std::to_string(request.context.local_transaction_id) + ":" +
          std::to_string(
              request.context.snapshot_visible_through_local_transaction_id),
      "filter-project-distinct-sort-limit.transaction-effect-unchanged");
  execution_request.result_publication_request.result_kind =
      exec::CanonicalResultKind::kRows;
  execution_request.result_publication_request.invocation_mode =
      exec::CanonicalResultInvocationMode::kDirect;
  execution_request.result_publication_request.column_bindings =
      std::move(prepared_limit.result_bindings);
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
            ? "QOW-DIAG-RELATIONAL-LIVE-FILTER-PROJECT-DISTINCT-SORT-LIMIT-EXECUTION-V1"
            : execution.issues.front().diagnostic_id,
        execution.issues.empty()
            ? "full selected SQL-tail DAG was not completed"
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

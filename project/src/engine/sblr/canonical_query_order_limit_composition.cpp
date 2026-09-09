// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "canonical_query_order_limit_composition.hpp"

#include "canonical_query_object_free_composition_support.hpp"
#include "canonical_query_object_free_profile.hpp"
#include "canonical_query_physical_registration.hpp"
#include "canonical_query_relational_registration.hpp"
#include "canonical_query_runtime_memory_support.hpp"
#include "canonical_query_scalar_support.hpp"
#include "canonical_query_sort_registration.hpp"
#include "canonical_relational_expression.hpp"

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

// SEARCH_KEY: SB_ENGINE_CANONICAL_QUERY_ORDER_LIMIT_COMPOSITION_AUTHORITY
// Coordinates already-admitted object-free ORDER/LIMIT tails. It consumes
// engine-selected MGA statement context and cannot create a snapshot, access
// storage, or publish transaction finality.

CanonicalObjectFreeValuesExecutionResult
ExecuteCanonicalObjectFreeLimitQuery(
    const CanonicalObjectFreeValuesExecutionRequest& request) {
  CanonicalObjectFreeValuesExecutionResult result;
  const auto& graph = request.optimizer_request.logical_graph;
  const auto root = std::ranges::find_if(graph.nodes, [&](const auto& node) {
    return node.logical_node_id == graph.root_logical_node_id;
  });
  if (graph.nodes.size() != 2 || root == graph.nodes.end() ||
      root->node_kind != plan::CanonicalLogicalRelationalNodeKind::kLimit ||
      root->semantic_variant_id != "limit.bound-count.v1" ||
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
    return refuse("QOW-DIAG-RELATIONAL-LIVE-LIMIT-ADMISSION-V1",
                  "live LIMIT execution lacks optimizer admission");
  }
  if (root->bound_expression_ids.size() != 1) {
    return refuse("QOW-DIAG-RELATIONAL-LIVE-LIMIT-PAYLOAD-V1",
                  "bound-count LIMIT requires exactly one expression");
  }

  auto input = MaterializeValues(request.relational_dag, *input_node,
                                 request.expression_services);
  if (!input.ok) {
    return refuse("QOW-DIAG-RELATIONAL-LIVE-LIMIT-PAYLOAD-V1",
                  "LIMIT input VALUES: " + input.detail);
  }
  auto prepared_root = PrepareLimitRootForComposition(request.relational_dag, *root,
                                        *input_node, input);
  if (!prepared_root.ok) {
    return refuse("QOW-DIAG-RELATIONAL-LIVE-LIMIT-PAYLOAD-V1",
                  prepared_root.detail);
  }
  std::uint64_t row_limit = 0;
  std::string bound_detail;
  CanonicalRelationalExpressionRuntime expression_runtime(
      request.relational_dag, request.expression_services);
  if (!EvaluateNonNegativeRowBoundForComposition(
          &expression_runtime, root->bound_expression_ids.front(),
          &row_limit, &bound_detail)) {
    return refuse("QOW-DIAG-RELATIONAL-LIVE-LIMIT-PAYLOAD-V1",
                  "LIMIT count: " + bound_detail);
  }

  const auto input_row_count = input.batch.rows.size();
  const auto output_row_bound =
      row_limit > input_row_count
          ? input_row_count
          : static_cast<std::size_t>(row_limit);
  std::uint64_t input_memory = 1;
  std::uint64_t output_memory = 1;
  std::uint64_t total_memory = 0;
  if (!AddBatchMemoryBytes(input.batch, &input_memory) ||
      !AddBatchRowRangeMemoryBytes(input.batch, 0, output_row_bound,
                                   &output_memory) ||
      !CheckedAdd(input_memory, output_memory, &total_memory)) {
    return refuse("QOW-DIAG-OPTIMIZER-SEARCH-COST-OVERFLOW-V1",
                  "live LIMIT input or output size overflowed");
  }
  if (total_memory >
      request.optimizer_request.resource.memory_budget_bytes) {
    return refuse("QOW-DIAG-OPTIMIZER-SEARCH-COST-VECTOR-V1",
                  "live LIMIT exceeds the admitted memory budget");
  }

  const auto identity_scope =
      graph.bound_sblr_tree_uuid + ":" + request.context.statement_uuid.canonical;
  const auto values_capability_uuid =
      DerivedCanonicalUuid(identity_scope, "values.capability");
  const auto limit_capability_uuid =
      DerivedCanonicalUuid(identity_scope, "limit.capability");
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
       "limit.typed.v1",
       limit_capability_uuid,
       plan::CanonicalLogicalRelationalNodeKind::kLimit,
       exec::PhysicalNodeKind::kLimit,
       "canonical.limit.bound-count.v1",
       output_row_bound,
       total_memory,
       1,
       1}};
  if (!CompleteLiveRuntimeMemoryReceipts(&profiles)) {
    return refuse("QOW-DIAG-OPT-017-REFUSAL-V1",
                  "LIMIT runtime memory receipts are incomplete");
  }
  auto planning = PlanAndPublishLivePhysicalDag(
      request, profiles, "limit.selected-plan", "LIMIT");
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
      "QOW-DIAG-RELATIONAL-LIVE-LIMIT-VALUES-V1", "LIMIT");

  exec::CanonicalPhysicalExecutorRegistration limit_registration;
  limit_registration.node_kind = exec::PhysicalNodeKind::kLimit;
  limit_registration.implementation_id = "limit.typed.v1";
  limit_registration.executor_capability_uuid = limit_capability_uuid;
  limit_registration.executor_capability_abi_version = 1;
  limit_registration.engine_owned = true;
  limit_registration.accepts_optimizer_publication_v2 = true;
  limit_registration.execute =
      [row_limit, input_row_count, mga_context = request.context](
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
            !inputs.front().materialized_output_batch.has_value()) {
          step.diagnostic.ok = false;
          step.diagnostic.diagnostic_code =
              "QOW-DIAG-RELATIONAL-LIVE-LIMIT-INPUT-V1";
          step.diagnostic.detail =
              "LIMIT executor did not receive one typed input batch";
          return step;
        }
        const auto& input_batch = *inputs.front().materialized_output_batch;
        if (input_batch.rows.size() != input_row_count) {
          step.diagnostic.ok = false;
          step.diagnostic.diagnostic_code =
              "QOW-DIAG-RELATIONAL-LIVE-LIMIT-INPUT-V1";
          step.diagnostic.detail =
              "LIMIT input cardinality differs from its selected cost";
          return step;
        }
        exec::CanonicalDescriptorLimitRequest limit_request;
        limit_request.selected_physical_node_id = node.physical_node_id;
        limit_request.limit = row_limit;
        limit_request.offset = 0;
        limit_request.mga_authority =
            BuildCanonicalExecutionMgaAuthority(mga_context, dag);
        auto limit_result = exec::ExecuteCanonicalDescriptorLimit(
            limit_request, dag, input_batch);
        if (!limit_result.diagnostic.ok) {
          step.diagnostic = std::move(limit_result.diagnostic);
          return step;
        }
        step.result_handle_id = node.physical_node_id;
        step.input_row_count = input_batch.rows.size();
        step.rows_examined = limit_result.output_batch.rows.size();
        step.output_row_count = limit_result.output_batch.rows.size();
        step.materialized_output_batch = std::move(limit_result.output_batch);
        step.mga_statement_context =
            std::move(limit_result.mga_statement_context);
        return step;
      };

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
      std::move(limit_registration));
  execution_request.engine_execution_authorized = true;
  execution_request.result_publication_request.statement_uuid =
      request.context.statement_uuid.canonical;
  execution_request.result_publication_request.execution_attempt_uuid =
      DerivedCanonicalUuid(
          identity_scope + ":" + request.context.current_monotonic_ns,
          "limit.execution-attempt");
  execution_request.result_publication_request.transaction_effect_evidence_uuid =
      DerivedCanonicalUuid(
          identity_scope + ":" +
              std::to_string(request.context.local_transaction_id) + ":" +
              std::to_string(
                  request.context.snapshot_visible_through_local_transaction_id),
          "limit.transaction-effect-unchanged");
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
            ? "QOW-DIAG-RELATIONAL-LIVE-LIMIT-EXECUTION-V1"
            : execution.issues.front().diagnostic_id,
        execution.issues.empty()
            ? "live LIMIT selected DAG was not completed"
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
ExecuteCanonicalObjectFreeSortQuery(
    const CanonicalObjectFreeValuesExecutionRequest& request) {
  CanonicalObjectFreeValuesExecutionResult result;
  const auto& graph = request.optimizer_request.logical_graph;
  const auto root = std::ranges::find_if(graph.nodes, [&](const auto& node) {
    return node.logical_node_id == graph.root_logical_node_id;
  });
  if (graph.nodes.size() != 2 || root == graph.nodes.end() ||
      root->node_kind != plan::CanonicalLogicalRelationalNodeKind::kSort ||
      root->semantic_variant_id != "sort.required-order.v1" ||
      root->input_logical_node_ids.size() != 1) {
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
        (node.logical_node_id == input_node->logical_node_id &&
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
    return refuse("QOW-DIAG-RELATIONAL-LIVE-SORT-ADMISSION-V1",
                  "live SORT execution lacks optimizer admission");
  }

  auto input = MaterializeValues(request.relational_dag, *input_node,
                                 request.expression_services);
  if (!input.ok) {
    return refuse("QOW-DIAG-RELATIONAL-LIVE-SORT-PAYLOAD-V1",
                  "SORT input VALUES: " + input.detail);
  }
  const bool expression_ordering = std::ranges::any_of(
      root->bound_expression_ids, [&](const auto expression_id) {
        return std::ranges::find(input_node->bound_expression_ids,
                                 expression_id) ==
               input_node->bound_expression_ids.end();
      });
  std::uint64_t expression_work = 0;
  if (expression_ordering &&
      (!CheckedMultiply(input.batch.rows.size(),
                        root->bound_expression_ids.size(),
                        &expression_work) ||
       expression_work >
           request.optimizer_request.resource.maximum_candidate_count)) {
    return refuse("QOW-DIAG-OPTIMIZER-SEARCH-COST-VECTOR-V1",
                  "live expression SORT row evaluation exceeds the admitted "
                  "candidate bound");
  }
  auto prepared_root = expression_ordering
      ? PrepareExpressionSortRootForComposition(
            request.context, request.relational_dag,
            request.optimizer_request.logical_properties, *root, *input_node,
            input, request.expression_services)
      : PrepareSortRootForComposition(
            request.context, request.relational_dag,
            request.optimizer_request.logical_properties, *root, *input_node,
            input);
  if (!prepared_root.ok) {
    return refuse("QOW-DIAG-RELATIONAL-LIVE-SORT-PAYLOAD-V1",
                  prepared_root.detail);
  }

  const auto input_row_count = input.batch.rows.size();
  std::uint64_t input_memory = 1;
  std::uint64_t expression_memory = 1;
  std::uint64_t comparison_count = 0;
  std::uint64_t row_order_memory = 0;
  std::uint64_t total_memory = 0;
  if (!AddBatchMemoryBytes(input.batch, &input_memory) ||
      (prepared_root.expression_ordering &&
       !AddBatchMemoryBytes(prepared_root.expression_input_batch,
                            &expression_memory)) ||
      !CheckedMultiply(input_row_count, input_row_count,
                       &comparison_count) ||
      !CheckedMultiply(input_row_count, sizeof(std::size_t),
                       &row_order_memory) ||
      !CheckedAdd(input_memory, input_memory, &total_memory) ||
      (prepared_root.expression_ordering &&
       !CheckedAdd(total_memory, expression_memory, &total_memory)) ||
      !CheckedAdd(total_memory, comparison_count, &total_memory) ||
      !CheckedAdd(total_memory, row_order_memory, &total_memory) ||
      comparison_count > std::numeric_limits<std::size_t>::max()) {
    return refuse("QOW-DIAG-OPTIMIZER-SEARCH-COST-OVERFLOW-V1",
                  "live SORT comparison or materialization size overflowed");
  }
  if (total_memory >
      request.optimizer_request.resource.memory_budget_bytes) {
    return refuse("QOW-DIAG-OPTIMIZER-SEARCH-COST-VECTOR-V1",
                  "live SORT exceeds the admitted memory budget");
  }

  const auto identity_scope =
      graph.bound_sblr_tree_uuid + ":" + request.context.statement_uuid.canonical;
  const auto values_capability_uuid =
      DerivedCanonicalUuid(identity_scope, "values.capability");
  const auto sort_capability_uuid =
      DerivedCanonicalUuid(identity_scope, "sort.capability");
  const auto deterministic_tie_evidence_uuid = DerivedCanonicalUuid(
      identity_scope + ":" + prepared_root.ordering_property_uuid,
      "sort.deterministic-tie");
  const std::string sort_implementation_id =
      prepared_root.expression_ordering
          ? "sort.typed.expression-row.v1"
          : "sort.typed.terms.v1";
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
       sort_implementation_id,
       sort_capability_uuid,
       plan::CanonicalLogicalRelationalNodeKind::kSort,
       exec::PhysicalNodeKind::kSort,
       prepared_root.expression_ordering
           ? "canonical.sort.expression-row.v1"
           : "canonical.sort.typed.terms.v1",
       input_row_count,
       total_memory,
       1,
       1,
       {},
       {prepared_root.ordering_property_uuid},
       {plan::CanonicalLogicalPropertyKind::kOrdering}}};
  if (!CompleteLiveRuntimeMemoryReceipts(
          &profiles,
          {{root->logical_node_id, comparison_count},
           {root->logical_node_id, row_order_memory}})) {
    return refuse("QOW-DIAG-OPT-017-REFUSAL-V1",
                  "SORT runtime memory receipts are incomplete");
  }
  auto planning = PlanAndPublishLivePhysicalDag(
      request, profiles, "sort.selected-plan", "SORT");
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
      "QOW-DIAG-RELATIONAL-LIVE-SORT-VALUES-V1", "SORT");
  auto result_bindings = std::move(prepared_root.result_bindings);

  auto sort_registration =
      prepared_root.expression_ordering
          ? MakeLiveExpressionSortRegistration(
                std::move(prepared_root), deterministic_tie_evidence_uuid,
                sort_capability_uuid, input_row_count,
                std::max<std::size_t>(
                    1, static_cast<std::size_t>(comparison_count)),
                request.relational_dag, request.expression_services,
                request.context)
          : MakeLiveSortRegistration(
                std::move(prepared_root.order_terms),
                deterministic_tie_evidence_uuid,
                sort_capability_uuid, input_row_count,
                std::max<std::size_t>(
                    1, static_cast<std::size_t>(comparison_count)),
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
      std::move(sort_registration));
  execution_request.engine_execution_authorized = true;
  execution_request.result_publication_request.statement_uuid =
      request.context.statement_uuid.canonical;
  execution_request.result_publication_request.execution_attempt_uuid =
      DerivedCanonicalUuid(
          identity_scope + ":" + request.context.current_monotonic_ns,
          "sort.execution-attempt");
  execution_request.result_publication_request.transaction_effect_evidence_uuid =
      DerivedCanonicalUuid(
          identity_scope + ":" +
              std::to_string(request.context.local_transaction_id) + ":" +
              std::to_string(
                  request.context.snapshot_visible_through_local_transaction_id),
          "sort.transaction-effect-unchanged");
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
            ? "QOW-DIAG-RELATIONAL-LIVE-SORT-EXECUTION-V1"
            : execution.issues.front().diagnostic_id,
        execution.issues.empty()
            ? "live SORT selected DAG was not completed"
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

// QOW-SOURCE-RCP-020-ORDER-LIMIT-DISTINCT-COMPOSITION-V1
// Execute the canonical SQL evaluation tail as one selected physical DAG:
// projected VALUES -> query DISTINCT -> ORDER BY -> OFFSET plus either LIMIT
// or the one signed native FETCH FIRST ROWS ONLY profile. TOP and WITH TIES
// remain the exact QRY-010 profile refusals and are not promoted here.
CanonicalObjectFreeValuesExecutionResult
ExecuteCanonicalObjectFreeDistinctSortLimitQuery(
    const CanonicalObjectFreeValuesExecutionRequest& request) {
  CanonicalObjectFreeValuesExecutionResult result;
  const auto& graph = request.optimizer_request.logical_graph;
  const auto root = std::ranges::find_if(graph.nodes, [&](const auto& node) {
    return node.logical_node_id == graph.root_logical_node_id;
  });
  if (graph.nodes.size() != 4 || root == graph.nodes.end() ||
      root->node_kind != plan::CanonicalLogicalRelationalNodeKind::kLimit ||
      (root->semantic_variant_id != "limit.bound-count-offset.v1" &&
       root->semantic_variant_id !=
           "fetch.first-rows-only-offset.v1") ||
      root->input_logical_node_ids.size() != 1 ||
      root->bound_expression_ids.size() != 2) {
    return result;
  }
  const bool fetch_first_rows_only =
      root->semantic_variant_id == "fetch.first-rows-only-offset.v1";
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
        return node.logical_node_id ==
               sort_node->input_logical_node_ids.front();
      });
  if (distinct_node == graph.nodes.end() || distinct_node == root ||
      distinct_node == sort_node ||
      distinct_node->node_kind !=
          plan::CanonicalLogicalRelationalNodeKind::kAggregate ||
      distinct_node->semantic_variant_id != "aggregate.query-distinct.v1" ||
      distinct_node->input_logical_node_ids.size() != 1) {
    return result;
  }
  const auto values_node =
      std::ranges::find_if(graph.nodes, [&](const auto& node) {
        return node.logical_node_id ==
               distinct_node->input_logical_node_ids.front();
      });
  if (values_node == graph.nodes.end() || values_node == root ||
      values_node == sort_node || values_node == distinct_node ||
      values_node->node_kind !=
          plan::CanonicalLogicalRelationalNodeKind::kValues ||
      values_node->semantic_variant_id != "values.literal-table.v1" ||
      !values_node->input_logical_node_ids.empty()) {
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
    return refuse("QOW-DIAG-RELATIONAL-LIVE-COMPOSITION-ADMISSION-V1",
                  "DISTINCT/ORDER/LIMIT composition lacks optimizer admission");
  }

  auto input = MaterializeValues(request.relational_dag, *values_node,
                                 request.expression_services);
  if (!input.ok) {
    return refuse("QOW-DIAG-RELATIONAL-LIVE-COMPOSITION-PAYLOAD-V1",
                  "composition input VALUES: " + input.detail);
  }
  auto prepared_distinct = PrepareQueryDistinctRootForComposition(
      request.context, request.relational_dag, *distinct_node, *values_node,
      input);
  if (!prepared_distinct.ok) {
    return refuse("QOW-DIAG-RELATIONAL-LIVE-COMPOSITION-PAYLOAD-V1",
                  prepared_distinct.detail);
  }
  auto prepared_sort = PrepareSortRootForComposition(
      request.context, request.relational_dag,
      request.optimizer_request.logical_properties, *sort_node,
      *distinct_node, input);
  if (!prepared_sort.ok) {
    return refuse("QOW-DIAG-RELATIONAL-LIVE-COMPOSITION-PAYLOAD-V1",
                  prepared_sort.detail);
  }
  auto prepared_limit = PrepareLimitRootForComposition(
      request.relational_dag, *root, *sort_node, input);
  if (!prepared_limit.ok) {
    return refuse("QOW-DIAG-RELATIONAL-LIVE-COMPOSITION-PAYLOAD-V1",
                  prepared_limit.detail);
  }

  CanonicalRelationalExpressionRuntime expression_runtime(
      request.relational_dag, request.expression_services);
  std::uint64_t row_limit = 0;
  std::uint64_t row_offset = 0;
  std::string bound_detail;
  if (!EvaluateNonNegativeRowBoundForComposition(
          &expression_runtime, root->bound_expression_ids[0], &row_limit,
          &bound_detail) ||
      !EvaluateNonNegativeRowBoundForComposition(
          &expression_runtime, root->bound_expression_ids[1], &row_offset,
          &bound_detail)) {
    return refuse("QOW-DIAG-RELATIONAL-LIVE-COMPOSITION-PAYLOAD-V1",
                  "LIMIT/FETCH bound: " + bound_detail);
  }

  const auto input_row_count = input.batch.rows.size();
  const auto offset_bound =
      row_offset > input_row_count
          ? input_row_count
          : static_cast<std::size_t>(row_offset);
  const auto remaining_bound = input_row_count - offset_bound;
  const auto output_row_bound =
      row_limit > remaining_bound
          ? remaining_bound
          : static_cast<std::size_t>(row_limit);
  std::uint64_t input_memory = 1;
  std::uint64_t distinct_output_memory = 1;
  std::uint64_t distinct_auxiliary_memory = 0;
  std::uint64_t distinct_memory = 0;
  std::uint64_t distinct_comparison_count = 0;
  std::uint64_t distinct_self_comparison_count = 0;
  std::uint64_t sort_comparison_count = 0;
  std::uint64_t row_order_memory = 0;
  std::uint64_t sort_memory = 0;
  std::uint64_t limit_memory = 0;
  std::string distinct_memory_detail;
  std::uint64_t total_work = 0;
  if (!QueryDistinctAuxiliaryMemoryBytes(
          input_row_count, input.batch.columns.size(),
          &distinct_auxiliary_memory) ||
      !CheckedMultiply(input_row_count, input_row_count,
                       &distinct_comparison_count) ||
      !CheckedMultiply(distinct_comparison_count,
                       input.batch.columns.size(),
                       &distinct_comparison_count) ||
      !CheckedMultiply(input_row_count, input.batch.columns.size(),
                       &distinct_self_comparison_count) ||
      !CheckedAdd(distinct_comparison_count,
                  distinct_self_comparison_count,
                  &distinct_comparison_count) ||
      !CheckedMultiply(input_row_count, input_row_count,
                       &sort_comparison_count) ||
      !CheckedMultiply(input_row_count, sizeof(std::size_t),
                       &row_order_memory) ||
      distinct_comparison_count > std::numeric_limits<std::size_t>::max() ||
      sort_comparison_count > std::numeric_limits<std::size_t>::max() ||
      !CheckedAdd(distinct_comparison_count, sort_comparison_count,
                  &total_work) ||
      !CheckedAdd(total_work, root->bound_expression_ids.size(),
                  &total_work)) {
    return refuse("QOW-DIAG-OPTIMIZER-SEARCH-COST-OVERFLOW-V1",
                  "composition work or DISTINCT resident state overflowed");
  }
  if (total_work >
      request.optimizer_request.resource.maximum_candidate_count) {
    return refuse("QOW-DIAG-OPTIMIZER-SEARCH-COST-VECTOR-V1",
                  "composition work exceeds the admitted candidate bound");
  }
  if (!AddBatchMemoryBytes(input.batch, &input_memory) ||
      !QueryDistinctOutputMemoryBytes(
          input.batch, prepared_distinct.equality_terms,
          static_cast<std::size_t>(distinct_comparison_count),
          &distinct_output_memory, &distinct_memory_detail) ||
      !CheckedAdd(input_memory, distinct_output_memory, &distinct_memory) ||
      !CheckedAdd(distinct_memory, distinct_auxiliary_memory,
                  &distinct_memory) ||
      !CheckedAdd(distinct_memory, input_memory, &sort_memory) ||
      !CheckedAdd(sort_memory, sort_comparison_count, &sort_memory) ||
      !CheckedAdd(sort_memory, row_order_memory, &sort_memory) ||
      !CheckedAdd(sort_memory, input_memory, &limit_memory) ||
      distinct_comparison_count > std::numeric_limits<std::size_t>::max() ||
      sort_comparison_count > std::numeric_limits<std::size_t>::max()) {
    return refuse("QOW-DIAG-OPTIMIZER-SEARCH-COST-OVERFLOW-V1",
                  "composition comparison or materialization size overflowed");
  }
  if (limit_memory >
      request.optimizer_request.resource.memory_budget_bytes) {
    return refuse("QOW-DIAG-OPTIMIZER-SEARCH-COST-VECTOR-V1",
                  "composition exceeds the admitted memory budget");
  }

  const auto identity_scope =
      graph.bound_sblr_tree_uuid + ":" +
      request.context.statement_uuid.canonical;
  const auto values_capability_uuid =
      DerivedCanonicalUuid(identity_scope, "values.capability");
  const auto distinct_capability_uuid =
      DerivedCanonicalUuid(identity_scope, "query-distinct.capability");
  const auto sort_capability_uuid =
      DerivedCanonicalUuid(identity_scope, "sort.capability");
  const auto limit_capability_uuid = DerivedCanonicalUuid(
      identity_scope,
      fetch_first_rows_only ? "fetch.capability" : "limit.capability");
  const auto deterministic_tie_evidence_uuid = DerivedCanonicalUuid(
      identity_scope + ":" + prepared_sort.ordering_property_uuid,
      "sort.deterministic-tie");
  const std::string limit_implementation_id =
      fetch_first_rows_only ? "fetch.native.rows-only.v1"
                            : "limit.typed.v1";
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
      {distinct_node->logical_node_id,
       "aggregate.query-distinct.typed.v1",
       distinct_capability_uuid,
       plan::CanonicalLogicalRelationalNodeKind::kAggregate,
       exec::PhysicalNodeKind::kAggregate,
       "canonical.aggregate.query-distinct.v1",
       input_row_count,
       distinct_memory,
       1,
       1},
      {sort_node->logical_node_id,
       "sort.typed.terms.v1",
       sort_capability_uuid,
       plan::CanonicalLogicalRelationalNodeKind::kSort,
       exec::PhysicalNodeKind::kSort,
       "canonical.sort.typed.terms.v1",
       input_row_count,
       sort_memory,
       1,
       1,
       {},
       {prepared_sort.ordering_property_uuid},
       {plan::CanonicalLogicalPropertyKind::kOrdering}},
      {root->logical_node_id,
       limit_implementation_id,
       limit_capability_uuid,
       plan::CanonicalLogicalRelationalNodeKind::kLimit,
       exec::PhysicalNodeKind::kLimit,
       fetch_first_rows_only
           ? "canonical.fetch.first-rows-only-offset.v1"
           : "canonical.limit.bound-count-offset.v1",
       output_row_bound,
       limit_memory,
       1,
       1}};
  if (!CompleteLiveRuntimeMemoryReceipts(
          &profiles,
          {{distinct_node->logical_node_id, distinct_auxiliary_memory},
           {sort_node->logical_node_id, sort_comparison_count},
           {sort_node->logical_node_id, row_order_memory}})) {
    return refuse("QOW-DIAG-OPT-017-REFUSAL-V1",
                  "DISTINCT/SORT/LIMIT runtime memory receipts are incomplete");
  }
  auto planning = PlanAndPublishLivePhysicalDag(
      request, profiles,
      fetch_first_rows_only ? "fetch-distinct-sort.selected-plan"
                            : "limit-distinct-sort.selected-plan",
      "DISTINCT/ORDER/LIMIT");
  if (!planning.ok) {
    return refuse(planning.diagnostic_id, planning.detail);
  }
  result.optimizer_selected = true;
  result.physical_dag_published = true;
  result.physical_node_count = planning.physical_dag.nodes.size();
  result.selected_plan_uuid = planning.physical_dag.selected_plan_uuid;

  std::unordered_map<std::uint64_t, exec::DescriptorBatch> values_batches;
  values_batches.emplace(values_node->logical_node_id,
                          std::move(input.batch));
  auto values_registration = MakeLiveValuesRegistration(
      std::move(values_batches), values_capability_uuid,
      "QOW-DIAG-RELATIONAL-LIVE-COMPOSITION-VALUES-V1",
      "DISTINCT/ORDER/LIMIT");
  auto distinct_registration = MakeLiveQueryDistinctRegistration(
      std::move(prepared_distinct.equality_terms),
      distinct_capability_uuid, input_row_count,
      std::max<std::size_t>(
          1, static_cast<std::size_t>(distinct_comparison_count)),
      request.context);
  auto sort_registration = MakeLiveSortRegistration(
      std::move(prepared_sort.order_terms),
      deterministic_tie_evidence_uuid, sort_capability_uuid,
      input_row_count,
      std::max<std::size_t>(1,
                            static_cast<std::size_t>(sort_comparison_count)),
      request.context);
  auto limit_registration = MakeLiveLimitRegistration(
      limit_implementation_id, limit_capability_uuid, row_limit, row_offset,
      fetch_first_rows_only, input_row_count, request.context);

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
          "distinct-sort-limit.execution-attempt");
  execution_request.result_publication_request
      .transaction_effect_evidence_uuid = DerivedCanonicalUuid(
      identity_scope + ":" +
          std::to_string(request.context.local_transaction_id) + ":" +
          std::to_string(
              request.context.snapshot_visible_through_local_transaction_id),
      "distinct-sort-limit.transaction-effect-unchanged");
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
            ? "QOW-DIAG-RELATIONAL-LIVE-COMPOSITION-EXECUTION-V1"
            : execution.issues.front().diagnostic_id,
        execution.issues.empty()
            ? "DISTINCT/ORDER/LIMIT selected DAG was not completed"
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

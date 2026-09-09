// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "canonical_query_set_composition.hpp"

#include "canonical_query_descriptor_support.hpp"
#include "canonical_query_object_free_profile.hpp"
#include "canonical_query_physical_registration.hpp"
#include "canonical_query_runtime_memory_support.hpp"
#include "canonical_query_scalar_support.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <ranges>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
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

// SEARCH_KEY: SB_ENGINE_CANONICAL_QUERY_SET_COMPOSITION_AUTHORITY
// Coordinates already-admitted object-free binary and nested set-operation
// DAGs. It consumes engine-selected MGA statement context and cannot create
// snapshots, access storage, or publish transaction finality.

CanonicalObjectFreeValuesExecutionResult
ExecuteCanonicalObjectFreeSetOperationQuery(
    const CanonicalObjectFreeValuesExecutionRequest& request) {
  CanonicalObjectFreeValuesExecutionResult result;
  const auto& graph = request.optimizer_request.logical_graph;
  const auto root = std::ranges::find_if(graph.nodes, [&](const auto& node) {
    return node.logical_node_id == graph.root_logical_node_id;
  });
  const auto set_profile =
      root == graph.nodes.end()
          ? LiveSetOperationProfile{}
          : ResolveLiveSetOperationProfileForComposition(root->semantic_variant_id);
  if (graph.nodes.size() != 3 || root == graph.nodes.end() ||
      root->node_kind !=
          plan::CanonicalLogicalRelationalNodeKind::kSetOperation ||
      !set_profile.matched ||
      root->input_logical_node_ids.size() != 2 ||
      root->input_logical_node_ids[0] == root->input_logical_node_ids[1] ||
      !root->bound_expression_ids.empty() ||
      !request.optimizer_request.logical_properties.properties.empty()) {
    return result;
  }
  const auto find_node = [&](const std::uint32_t node_id) {
    return std::ranges::find_if(graph.nodes, [&](const auto& node) {
      return node.logical_node_id == node_id;
    });
  };
  const auto left_node = find_node(root->input_logical_node_ids[0]);
  const auto right_node = find_node(root->input_logical_node_ids[1]);
  if (left_node == graph.nodes.end() || right_node == graph.nodes.end() ||
      left_node->node_kind !=
          plan::CanonicalLogicalRelationalNodeKind::kValues ||
      right_node->node_kind !=
          plan::CanonicalLogicalRelationalNodeKind::kValues ||
      left_node->semantic_variant_id != "values.literal-table.v1" ||
      right_node->semantic_variant_id != "values.literal-table.v1") {
    return result;
  }
  for (const auto& node : graph.nodes) {
    const bool values =
        node.node_kind == plan::CanonicalLogicalRelationalNodeKind::kValues;
    if (!node.required_object_uuids.empty() ||
        !node.required_property_uuids.empty() ||
        !node.delivered_property_uuids.empty() ||
        (values && !node.input_logical_node_ids.empty())) {
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
    return refuse("QOW-DIAG-RELATIONAL-LIVE-SET-ADMISSION-V1",
                  "live set-operation execution lacks optimizer admission");
  }

  auto left = MaterializeValues(request.relational_dag, *left_node,
                                request.expression_services);
  if (!left.ok) {
    return refuse("QOW-DIAG-RELATIONAL-LIVE-SET-PAYLOAD-V1",
                  "left VALUES: " + left.detail);
  }
  auto right = MaterializeValues(request.relational_dag, *right_node,
                                 request.expression_services);
  if (!right.ok) {
    return refuse("QOW-DIAG-RELATIONAL-LIVE-SET-PAYLOAD-V1",
                  "right VALUES: " + right.detail);
  }
  auto prepared_root = PrepareSetOperationRootForComposition(
      request.context, request.relational_dag, *root, left, right,
      set_profile);
  if (!prepared_root.ok) {
    return refuse("QOW-DIAG-RELATIONAL-LIVE-SET-PAYLOAD-V1",
                  prepared_root.detail);
  }

  if (right.batch.rows.size() >
      std::numeric_limits<std::size_t>::max() - left.batch.rows.size()) {
    return refuse("QOW-DIAG-OPTIMIZER-SEARCH-COST-OVERFLOW-V1",
                  "live set-operation row bound overflowed");
  }
  const auto set_output_row_bound =
      left.batch.rows.size() + right.batch.rows.size();
  std::uint64_t set_comparison_bound = 0;
  std::uint64_t set_collation_comparison_count = 0;
  if (!BoundSetOperationEqualityComparisonsForComposition(
          prepared_root, set_profile, set_output_row_bound,
          &set_comparison_bound, &set_collation_comparison_count) ||
      set_comparison_bound > std::numeric_limits<std::size_t>::max()) {
    return refuse("QOW-DIAG-OPTIMIZER-SEARCH-COST-OVERFLOW-V1",
                  "live set-operation comparison bound overflowed");
  }
  const bool concatenation_only =
      set_profile.operation == exec::CanonicalSetOperationKind::kUnion &&
      set_profile.quantifier == exec::CanonicalSetOperationQuantifier::kAll;
  std::uint64_t set_base_work = set_output_row_bound;
  std::uint64_t set_work = 0;
  if ((!concatenation_only &&
       !CheckedMultiply(set_output_row_bound, set_output_row_bound,
                        &set_base_work)) ||
      !CheckedAdd(set_base_work, set_collation_comparison_count,
                  &set_work)) {
    return refuse("QOW-DIAG-OPTIMIZER-SEARCH-COST-OVERFLOW-V1",
                  "live set-operation work bound overflowed");
  }
  if (set_work >
      request.optimizer_request.resource.maximum_candidate_count) {
    return refuse("QOW-DIAG-OPTIMIZER-SEARCH-COST-VECTOR-V1",
                  "live set-operation work exceeds the admitted candidate "
                  "bound");
  }

  std::uint64_t memory_bytes = 1;
  if (!AddBatchMemoryBytes(left.batch, &memory_bytes) ||
      !AddBatchMemoryBytes(right.batch, &memory_bytes) ||
      !CheckedAdd(memory_bytes, set_work, &memory_bytes)) {
    return refuse("QOW-DIAG-OPTIMIZER-SEARCH-COST-OVERFLOW-V1",
                  "live set-operation materialization size overflowed");
  }
  if (memory_bytes > request.optimizer_request.resource.memory_budget_bytes) {
    return refuse("QOW-DIAG-OPTIMIZER-SEARCH-COST-VECTOR-V1",
                  "live set-operation inputs exceed the admitted memory budget");
  }

  const auto identity_scope =
      graph.bound_sblr_tree_uuid + ":" + request.context.statement_uuid.canonical;
  const auto values_capability_uuid =
      DerivedCanonicalUuid(identity_scope, "values.capability");
  const auto set_capability_uuid = DerivedCanonicalUuid(
      identity_scope, "set." + set_profile.identity_component +
                          ".capability");
  std::vector<LivePhysicalNodeProfile> profiles;
  for (const auto& node : graph.nodes) {
    const bool values =
        node.node_kind == plan::CanonicalLogicalRelationalNodeKind::kValues;
    std::uint64_t node_rows = set_output_row_bound;
    std::uint64_t node_memory = memory_bytes;
    if (node.logical_node_id == left_node->logical_node_id) {
      node_rows = left.batch.rows.size();
      node_memory = 1;
      if (!AddBatchMemoryBytes(left.batch, &node_memory)) {
        return refuse("QOW-DIAG-OPTIMIZER-SEARCH-COST-OVERFLOW-V1",
                      "left VALUES cost size overflowed");
      }
    } else if (node.logical_node_id == right_node->logical_node_id) {
      node_rows = right.batch.rows.size();
      node_memory = 1;
      if (!AddBatchMemoryBytes(right.batch, &node_memory)) {
        return refuse("QOW-DIAG-OPTIMIZER-SEARCH-COST-OVERFLOW-V1",
                      "right VALUES cost size overflowed");
      }
    }
    profiles.push_back(
        {node.logical_node_id,
         values ? std::string(kValuesImplementationId)
                : set_profile.implementation_id,
         values ? values_capability_uuid : set_capability_uuid,
         node.node_kind,
         values ? exec::PhysicalNodeKind::kValues
                : exec::PhysicalNodeKind::kSetOperation,
         values ? "canonical.values.materialize.v1"
                : set_profile.physical_semantic_id,
         node_rows,
         values ? node_memory
                : request.optimizer_request.resource.memory_budget_bytes,
         values ? 0U : 2U,
         values ? 0U : 2U});
  }
  if (!CompleteLiveRuntimeMemoryReceipts(
          &profiles,
          {{root->logical_node_id, set_work}})) {
    return refuse("QOW-DIAG-OPT-017-REFUSAL-V1",
                  "set runtime memory receipts are incomplete");
  }
  auto planning = PlanAndPublishLivePhysicalDag(
      request, profiles,
      "set." + set_profile.identity_component + ".selected-plan",
      set_profile.operation_name);
  if (!planning.ok) {
    return refuse(planning.diagnostic_id, planning.detail);
  }
  result.optimizer_selected = true;
  result.physical_dag_published = true;
  result.physical_node_count = planning.physical_dag.nodes.size();
  result.selected_plan_uuid = planning.physical_dag.selected_plan_uuid;

  std::unordered_map<std::uint64_t, exec::DescriptorBatch> values_batches;
  values_batches.emplace(left_node->logical_node_id, std::move(left.batch));
  values_batches.emplace(right_node->logical_node_id, std::move(right.batch));
  auto values_registration = MakeLiveValuesRegistration(
      std::move(values_batches), values_capability_uuid,
      "QOW-DIAG-RELATIONAL-LIVE-SET-VALUES-V1",
      set_profile.operation_name);

  exec::CanonicalPhysicalExecutorRegistration set_registration;
  set_registration.node_kind = exec::PhysicalNodeKind::kSetOperation;
  set_registration.implementation_id = set_profile.implementation_id;
  set_registration.executor_capability_uuid = set_capability_uuid;
  set_registration.executor_capability_abi_version = 1;
  set_registration.engine_owned = true;
  set_registration.accepts_optimizer_publication_v2 = true;
  set_registration.publishes_runtime_observation_v1 = true;
  set_registration.honors_dispatcher_memory_limit_v1 = true;
  set_registration.execute =
      [result_columns = prepared_root.result_columns,
       collation_bindings = prepared_root.collation_bindings,
       set_profile, set_output_row_bound, set_comparison_bound,
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
        if (inputs.size() != 2 ||
            node.input_physical_node_ids.size() != 2 ||
            node.input_physical_node_ids[0] ==
                node.input_physical_node_ids[1] ||
            inputs[0].physical_node_id !=
                node.input_physical_node_ids[0] ||
            inputs[1].physical_node_id !=
                node.input_physical_node_ids[1] ||
            !inputs[0].materialized_output_batch.has_value() ||
            !inputs[1].materialized_output_batch.has_value()) {
          step.diagnostic.ok = false;
          step.diagnostic.diagnostic_code =
              "QOW-DIAG-RELATIONAL-LIVE-SET-INPUT-V1";
          step.diagnostic.detail =
              "set-operation executor did not receive two typed input batches";
          return step;
        }
        const auto& left_input_batch =
            *inputs[0].materialized_output_batch;
        const auto& right_input_batch =
            *inputs[1].materialized_output_batch;
        exec::TypedPhysicalNodeDag execution_dag;
        std::size_t callback_memory_bound = 0;
        std::string preflight_detail;
        if (!BuildStrictBinaryOperatorLocalPhysicalDag(
                dag, node, left_input_batch, right_input_batch,
                64 * 1024, &execution_dag, &callback_memory_bound,
                &preflight_detail)) {
          step.diagnostic.ok = false;
          step.diagnostic.diagnostic_code =
              "SBLR.PLAN_TREE.RESOURCE_LIMIT";
          step.diagnostic.detail =
              "set-operation " + std::move(preflight_detail);
          return step;
        }
        const auto execution_node = std::ranges::find_if(
            execution_dag.nodes, [&](const auto& candidate) {
              return candidate.physical_node_id == node.physical_node_id;
            });
        if (execution_node == execution_dag.nodes.end() ||
            execution_node->memory_bytes_required !=
                callback_memory_bound) {
          step.diagnostic.ok = false;
          step.diagnostic.diagnostic_code =
              "SBLR.PLAN_TREE.RESOURCE_LIMIT";
          step.diagnostic.detail =
              "set-operation callback grant rebinding changed";
          return step;
        }
        exec::CanonicalSetOperationAllRequest set_request;
        set_request.physical_dag = execution_dag;
        set_request.selected_physical_node_id = node.physical_node_id;
        set_request.borrowed_left_batch =
            &left_input_batch;
        set_request.borrowed_right_batch =
            &right_input_batch;
        set_request.result_columns = result_columns;
        set_request.operation = set_profile.operation;
        set_request.alignment = set_profile.alignment;
        set_request.quantifier = set_profile.quantifier;
        set_request.equality_profile = set_profile.equality_profile;
        set_request.type_profile = set_profile.type_profile;
        set_request.collation_bindings = collation_bindings;
        set_request.maximum_equality_comparison_count =
            std::max<std::size_t>(
                1, static_cast<std::size_t>(set_comparison_bound));
        set_request.maximum_output_row_count =
            std::max<std::size_t>(1, set_output_row_bound);
        set_request.enforce_payload_memory_grant = true;
        set_request.mga_authority =
            BuildCanonicalExecutionMgaAuthority(mga_context, execution_dag);
        auto set_result =
            set_profile.quantifier ==
                    exec::CanonicalSetOperationQuantifier::kAll
                ? exec::ExecuteCanonicalSetOperationAll(set_request)
                : exec::ExecuteCanonicalSetOperationDistinct(set_request);
        if (!set_result.diagnostic.ok) {
          step.diagnostic = std::move(set_result.diagnostic);
          return step;
        }
        if (!CanonicalSetOperationExecutionReceiptMatches(
                set_request, *execution_node, set_result)) {
          step.diagnostic.ok = false;
          step.diagnostic.diagnostic_code =
              "QOW-DIAG-RELATIONAL-LIVE-SET-EXECUTION-V1";
          step.diagnostic.detail =
              "set-operation execution receipt is inconsistent";
          return step;
        }
        std::uint64_t current_memory_bytes = 0;
        std::uint64_t peak_memory_bytes = 0;
        std::string memory_detail;
        if (!ValidateLiveSetMemoryReceipt(
                set_request.physical_dag, *execution_node, set_result,
                &current_memory_bytes, &peak_memory_bytes,
                &memory_detail)) {
          step.diagnostic.ok = false;
          step.diagnostic.diagnostic_code =
              "QOW-DIAG-OPT-017-REFUSAL-V1";
          step.diagnostic.detail = std::move(memory_detail);
          return step;
        }
        step.result_handle_id = node.physical_node_id;
        step.input_row_count = set_result.left_input_row_count +
                               set_result.right_input_row_count;
        step.rows_examined = step.input_row_count;
        step.output_row_count = set_result.output_batch.rows.size();
        step.materialized_output_batch = std::move(set_result.output_batch);
        step.mga_statement_context =
            std::move(set_result.mga_statement_context);
        step.data_access_observation_known = true;
        step.data_access_observed = false;
        PublishRuntimeMemoryObservation(
            &step, current_memory_bytes, peak_memory_bytes);
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
  execution_request.available_executors.push_back(std::move(set_registration));
  execution_request.engine_execution_authorized = true;
  execution_request.result_publication_request.statement_uuid =
      request.context.statement_uuid.canonical;
  execution_request.result_publication_request.execution_attempt_uuid =
      DerivedCanonicalUuid(
          identity_scope + ":" + request.context.current_monotonic_ns,
          "set." + set_profile.identity_component + ".execution-attempt");
  execution_request.result_publication_request.transaction_effect_evidence_uuid =
      DerivedCanonicalUuid(
          identity_scope + ":" +
              std::to_string(request.context.local_transaction_id) + ":" +
              std::to_string(
                  request.context.snapshot_visible_through_local_transaction_id),
          "set." + set_profile.identity_component +
              ".transaction-effect-unchanged");
  execution_request.result_publication_request.result_kind =
      exec::CanonicalResultKind::kRows;
  execution_request.result_publication_request.invocation_mode =
      exec::CanonicalResultInvocationMode::kDirect;
  execution_request.result_publication_request.column_bindings =
      std::move(prepared_root.result_bindings);
  execution_request.result_publication_request.maximum_row_count =
      std::max<std::size_t>(1, set_output_row_bound);

  const auto execution =
      ExecuteSelectedCanonicalObjectFreeDag(
          request.context, execution_request,
          planning.ordinary_runtime_memory_receipts);
  if (!execution.accepted || !execution.exact_selected_nodes_executed ||
      !execution.causal_counters_attached ||
      !execution.canonical_result_published || !execution.issues.empty()) {
    return refuse(
        execution.issues.empty()
            ? "QOW-DIAG-RELATIONAL-LIVE-SET-EXECUTION-V1"
            : execution.issues.front().diagnostic_id,
        execution.issues.empty()
            ? "live set-operation selected DAG was not completed"
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
ExecuteCanonicalObjectFreeNestedSetOperationQuery(
    const CanonicalObjectFreeValuesExecutionRequest& request) {
  CanonicalObjectFreeValuesExecutionResult result;
  const auto& graph = request.optimizer_request.logical_graph;
  const auto root = std::ranges::find_if(graph.nodes, [&](const auto& node) {
    return node.logical_node_id == graph.root_logical_node_id;
  });
  if (graph.nodes.size() < 5 || root == graph.nodes.end() ||
      root->node_kind !=
          plan::CanonicalLogicalRelationalNodeKind::kSetOperation ||
      !request.optimizer_request.logical_properties.properties.empty()) {
    return result;
  }

  std::unordered_map<std::uint64_t, const plan::CanonicalLogicalRelationalNode*>
      nodes;
  std::unordered_map<std::uint64_t, LiveSetOperationProfile> set_profiles;
  std::size_t values_count = 0;
  for (const auto& node : graph.nodes) {
    if (!nodes.emplace(node.logical_node_id, &node).second ||
        !node.required_object_uuids.empty() ||
        !node.required_property_uuids.empty() ||
        !node.delivered_property_uuids.empty()) {
      return result;
    }
    if (node.node_kind ==
        plan::CanonicalLogicalRelationalNodeKind::kValues) {
      if (!node.input_logical_node_ids.empty() ||
          node.semantic_variant_id != "values.literal-table.v1") {
        return result;
      }
      ++values_count;
      continue;
    }
    if (node.node_kind !=
        plan::CanonicalLogicalRelationalNodeKind::kSetOperation) {
      return result;
    }
    auto profile = ResolveLiveSetOperationProfileForComposition(node.semantic_variant_id);
    if (!profile.matched || node.input_logical_node_ids.size() != 2 ||
        node.input_logical_node_ids[0] == node.input_logical_node_ids[1] ||
        !node.bound_expression_ids.empty()) {
      return result;
    }
    set_profiles.emplace(node.logical_node_id, std::move(profile));
  }
  if (values_count < 3 || set_profiles.size() < 2 ||
      values_count + set_profiles.size() != graph.nodes.size()) {
    return result;
  }

  std::unordered_set<std::uint64_t> reachable;
  std::vector<std::uint64_t> pending_reachability{root->logical_node_id};
  while (!pending_reachability.empty()) {
    const auto node_id = pending_reachability.back();
    pending_reachability.pop_back();
    if (!reachable.insert(node_id).second) continue;
    const auto found = nodes.find(node_id);
    if (found == nodes.end()) return result;
    for (const auto input_id : found->second->input_logical_node_ids) {
      pending_reachability.push_back(input_id);
    }
  }
  if (reachable.size() != graph.nodes.size()) return result;

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
    return refuse("QOW-DIAG-RELATIONAL-LIVE-SET-ADMISSION-V1",
                  "nested set-operation execution lacks optimizer admission");
  }

  std::unordered_map<std::uint64_t, MaterializedValues> materialized_schemas;
  std::unordered_map<std::uint64_t, exec::DescriptorBatch> values_batches;
  std::unordered_map<std::uint64_t, std::uint64_t> row_bounds;
  std::unordered_map<std::uint64_t, std::uint64_t> leaf_memory;
  std::uint64_t memory_bytes = 1;
  for (const auto& node : graph.nodes) {
    if (node.node_kind !=
        plan::CanonicalLogicalRelationalNodeKind::kValues) {
      continue;
    }
    auto materialized = MaterializeValues(
        request.relational_dag, node, request.expression_services);
    if (!materialized.ok) {
      return refuse("QOW-DIAG-RELATIONAL-LIVE-SET-PAYLOAD-V1",
                    "nested VALUES: " + materialized.detail);
    }
    std::uint64_t node_memory = 1;
    if (!AddBatchMemoryBytes(materialized.batch, &node_memory) ||
        !AddBatchMemoryBytes(materialized.batch, &memory_bytes)) {
      return refuse("QOW-DIAG-OPTIMIZER-SEARCH-COST-OVERFLOW-V1",
                    "nested set-operation leaf memory overflowed");
    }
    row_bounds.emplace(node.logical_node_id,
                       materialized.batch.rows.size());
    leaf_memory.emplace(node.logical_node_id, node_memory);
    values_batches.emplace(node.logical_node_id, materialized.batch);
    materialized_schemas.emplace(node.logical_node_id,
                                 std::move(materialized));
  }

  std::unordered_map<std::uint64_t, PreparedLiveSetNode> prepared_set_nodes;
  std::unordered_set<std::uint64_t> pending_set_nodes;
  for (const auto& [node_id, profile] : set_profiles) {
    (void)profile;
    pending_set_nodes.insert(node_id);
  }
  std::uint64_t total_set_work = 0;
  while (!pending_set_nodes.empty()) {
    bool progressed = false;
    for (auto pending = pending_set_nodes.begin();
         pending != pending_set_nodes.end();) {
      const auto node_id = *pending;
      const auto* node = nodes.at(node_id);
      const auto left = materialized_schemas.find(
          node->input_logical_node_ids[0]);
      const auto right = materialized_schemas.find(
          node->input_logical_node_ids[1]);
      if (left == materialized_schemas.end() ||
          right == materialized_schemas.end()) {
        ++pending;
        continue;
      }
      auto prepared = PrepareSetOperationRootForComposition(
          request.context, request.relational_dag, *node, left->second,
          right->second, set_profiles.at(node_id));
      if (!prepared.ok) {
        return refuse("QOW-DIAG-RELATIONAL-LIVE-SET-PAYLOAD-V1",
                      prepared.detail);
      }
      std::uint64_t output_bound = 0;
      std::uint64_t comparison_bound = 0;
      std::uint64_t collation_comparison_count = 0;
      const auto left_bound =
          row_bounds.at(node->input_logical_node_ids[0]);
      const auto right_bound =
          row_bounds.at(node->input_logical_node_ids[1]);
      if (!CheckedAdd(left_bound, right_bound, &output_bound) ||
          !BoundSetOperationEqualityComparisonsForComposition(
              prepared, set_profiles.at(node_id), output_bound,
              &comparison_bound, &collation_comparison_count) ||
          comparison_bound > std::numeric_limits<std::size_t>::max()) {
        return refuse("QOW-DIAG-OPTIMIZER-SEARCH-COST-OVERFLOW-V1",
                      "nested set-operation row/comparison bound overflowed");
      }
      const bool concatenation_only =
          set_profiles.at(node_id).operation ==
              exec::CanonicalSetOperationKind::kUnion &&
          set_profiles.at(node_id).quantifier ==
              exec::CanonicalSetOperationQuantifier::kAll;
      std::uint64_t node_base_work = output_bound;
      std::uint64_t node_work = 0;
      if ((!concatenation_only &&
           !CheckedMultiply(output_bound, output_bound,
                            &node_base_work)) ||
          !CheckedAdd(node_base_work, collation_comparison_count,
                      &node_work) ||
          !CheckedAdd(total_set_work, node_work, &total_set_work)) {
        return refuse("QOW-DIAG-OPTIMIZER-SEARCH-COST-OVERFLOW-V1",
                      "nested set-operation work bound overflowed");
      }
      MaterializedValues schema;
      schema.ok = true;
      schema.batch.columns = prepared.result_columns;
      schema.result_bindings = prepared.result_bindings;
      row_bounds.emplace(node_id, output_bound);
      materialized_schemas.emplace(node_id, std::move(schema));
      prepared_set_nodes.emplace(
          node_id,
          PreparedLiveSetNode{
              set_profiles.at(node_id), std::move(prepared),
              static_cast<std::size_t>(output_bound),
              static_cast<std::size_t>(std::max<std::uint64_t>(
                  1, comparison_bound))});
      pending = pending_set_nodes.erase(pending);
      progressed = true;
    }
    if (!progressed) {
      return refuse("QOW-DIAG-RELATIONAL-LIVE-SET-PAYLOAD-V1",
                    "nested set-operation graph is cyclic or unresolved");
    }
  }
  if (total_set_work >
      request.optimizer_request.resource.maximum_candidate_count) {
    return refuse("QOW-DIAG-OPTIMIZER-SEARCH-COST-VECTOR-V1",
                  "nested set-operation work exceeds the admitted candidate "
                  "bound");
  }
  if (!CheckedAdd(memory_bytes, total_set_work, &memory_bytes)) {
    return refuse("QOW-DIAG-OPTIMIZER-SEARCH-COST-OVERFLOW-V1",
                  "nested set-operation memory bound overflowed");
  }
  if (memory_bytes > request.optimizer_request.resource.memory_budget_bytes) {
    return refuse("QOW-DIAG-OPTIMIZER-SEARCH-COST-VECTOR-V1",
                  "nested set-operation exceeds the admitted memory budget");
  }

  const auto identity_scope = graph.bound_sblr_tree_uuid + ":" +
                              request.context.statement_uuid.canonical;
  const auto values_capability_uuid =
      DerivedCanonicalUuid(identity_scope, "values.capability");
  std::unordered_map<std::string, std::string> set_capability_uuids;
  std::string graph_identity = "nested-set";
  for (const auto& node : graph.nodes) {
    const auto prepared = prepared_set_nodes.find(node.logical_node_id);
    if (prepared == prepared_set_nodes.end()) continue;
    graph_identity += "." + std::to_string(node.logical_node_id) + "." +
                      prepared->second.profile.identity_component;
    set_capability_uuids.try_emplace(
        prepared->second.profile.implementation_id,
        DerivedCanonicalUuid(
            identity_scope,
            "set." + prepared->second.profile.implementation_id +
                ".capability"));
  }

  std::vector<LivePhysicalNodeProfile> profiles;
  profiles.reserve(graph.nodes.size());
  for (const auto& node : graph.nodes) {
    if (node.node_kind ==
        plan::CanonicalLogicalRelationalNodeKind::kValues) {
      profiles.push_back(
          {node.logical_node_id, std::string(kValuesImplementationId),
           values_capability_uuid, node.node_kind,
           exec::PhysicalNodeKind::kValues,
           "canonical.values.materialize.v1", row_bounds.at(node.logical_node_id),
           leaf_memory.at(node.logical_node_id), 0, 0});
      continue;
    }
    const auto& prepared = prepared_set_nodes.at(node.logical_node_id);
    profiles.push_back(
        {node.logical_node_id, prepared.profile.implementation_id,
         set_capability_uuids.at(prepared.profile.implementation_id),
         node.node_kind, exec::PhysicalNodeKind::kSetOperation,
         prepared.profile.physical_semantic_id,
         prepared.maximum_output_row_count,
         request.optimizer_request.resource.memory_budget_bytes, 2, 2});
    profiles.back().runtime_accounted_auxiliary_memory_bytes =
        prepared.profile.operation ==
                    exec::CanonicalSetOperationKind::kUnion &&
                prepared.profile.quantifier ==
                    exec::CanonicalSetOperationQuantifier::kAll
            ? prepared.maximum_output_row_count
            : prepared.maximum_equality_comparison_count;
  }
  if (!CompleteLiveRuntimeMemoryReceipts(&profiles)) {
    return refuse("QOW-DIAG-OPT-017-REFUSAL-V1",
                  "nested set runtime memory receipts are incomplete");
  }
  auto planning = PlanAndPublishLivePhysicalDag(
      request, profiles, graph_identity + ".selected-plan",
      "NESTED SET OPERATION");
  if (!planning.ok) {
    return refuse(planning.diagnostic_id, planning.detail);
  }
  result.optimizer_selected = true;
  result.physical_dag_published = true;
  result.physical_node_count = planning.physical_dag.nodes.size();
  result.selected_plan_uuid = planning.physical_dag.selected_plan_uuid;

  auto values_registration = MakeLiveValuesRegistration(
      std::move(values_batches), values_capability_uuid,
      "QOW-DIAG-RELATIONAL-LIVE-SET-VALUES-V1",
      "NESTED SET OPERATION");
  api::CanonicalOptimizerSelectedExecutionRequest execution_request;
  execution_request.pre_access_statistics_snapshot_uuid =
      planning.physical_dag.statistics_snapshot_uuid;
  execution_request.mga_authority = BuildCanonicalExecutionMgaAuthority(
      request.context, planning.physical_dag);
  execution_request.selected_physical_dag =
      std::move(planning.physical_dag);
  execution_request.available_executors.push_back(
      std::move(values_registration));

  std::unordered_set<std::string> registered_implementations;
  for (const auto& [node_id, prepared] : prepared_set_nodes) {
    (void)node_id;
    if (!registered_implementations
             .insert(prepared.profile.implementation_id)
             .second) {
      continue;
    }
    execution_request.available_executors.push_back(
        MakeLiveSetOperationRegistration(
            MakeLiveSetRegistrationProfilesForComposition(prepared_set_nodes),
            prepared.profile.implementation_id,
            set_capability_uuids.at(prepared.profile.implementation_id),
            request.context));
  }

  execution_request.engine_execution_authorized = true;
  execution_request.result_publication_request.statement_uuid =
      request.context.statement_uuid.canonical;
  execution_request.result_publication_request.execution_attempt_uuid =
      DerivedCanonicalUuid(
          identity_scope + ":" + request.context.current_monotonic_ns,
          graph_identity + ".execution-attempt");
  execution_request.result_publication_request
      .transaction_effect_evidence_uuid = DerivedCanonicalUuid(
      identity_scope + ":" +
          std::to_string(request.context.local_transaction_id) + ":" +
          std::to_string(
              request.context.snapshot_visible_through_local_transaction_id),
      graph_identity + ".transaction-effect-unchanged");
  execution_request.result_publication_request.result_kind =
      exec::CanonicalResultKind::kRows;
  execution_request.result_publication_request.invocation_mode =
      exec::CanonicalResultInvocationMode::kDirect;
  execution_request.result_publication_request.column_bindings =
      prepared_set_nodes.at(root->logical_node_id).prepared.result_bindings;
  const auto root_output_bound =
      prepared_set_nodes.at(root->logical_node_id).maximum_output_row_count;
  execution_request.result_publication_request.maximum_row_count =
      std::max<std::size_t>(1, root_output_bound);

  const auto execution =
      ExecuteSelectedCanonicalObjectFreeDag(
          request.context, execution_request,
          planning.ordinary_runtime_memory_receipts);
  if (!execution.accepted || !execution.exact_selected_nodes_executed ||
      !execution.causal_counters_attached ||
      !execution.canonical_result_published || !execution.issues.empty()) {
    return refuse(
        execution.issues.empty()
            ? "QOW-DIAG-RELATIONAL-LIVE-SET-EXECUTION-V1"
            : execution.issues.front().diagnostic_id,
        execution.issues.empty()
            ? "nested set-operation selected DAG was not completed"
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

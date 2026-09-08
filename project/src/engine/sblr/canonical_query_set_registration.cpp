// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "canonical_query_set_registration.hpp"

#include "canonical_query_physical_registration.hpp"
#include "canonical_query_runtime_memory_support.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <ranges>
#include <string>
#include <utility>
#include <vector>

namespace scratchbird::engine::sblr {

namespace api = scratchbird::engine::internal_api;

// SEARCH_KEY: SB_ENGINE_CANONICAL_QUERY_SET_REGISTRATION_AUTHORITY
bool ValidateLiveSetMemoryReceipt(
    const exec::TypedPhysicalNodeDag& dag,
    const exec::PhysicalNodeRecord& node,
    const exec::CanonicalSetOperationAllResult& result,
    std::uint64_t* current_memory_bytes,
    std::uint64_t* peak_memory_bytes,
    std::string* detail) {
  if (current_memory_bytes == nullptr || peak_memory_bytes == nullptr ||
      detail == nullptr) {
    return false;
  }
  *current_memory_bytes = 0;
  *peak_memory_bytes = 0;
  detail->clear();
  const auto resource_evidence = std::ranges::find_if(
      dag.admission_evidence,
      [](const exec::PhysicalAdmissionEvidence& evidence) {
        return evidence.stage == exec::PhysicalAdmissionStage::kResource;
      });
  const auto resource_evidence_count = std::ranges::count_if(
      dag.admission_evidence,
      [](const exec::PhysicalAdmissionEvidence& evidence) {
        return evidence.stage == exec::PhysicalAdmissionStage::kResource;
      });
  std::uint64_t measured_output_payload_bytes = 0;
  if (resource_evidence_count != 1 ||
      resource_evidence == dag.admission_evidence.end() ||
      resource_evidence->evidence_uuid.empty() ||
      dag.memory_budget_bytes == 0 || node.memory_bytes_required == 0 ||
      node.memory_bytes_required > dag.memory_budget_bytes ||
      (node.retained_cost.memory_bytes_required != 0 &&
       node.retained_cost.memory_bytes_required !=
           node.memory_bytes_required) ||
      !RuntimeMaterializedBatchMemoryBytes(
          result.output_batch, &measured_output_payload_bytes) ||
      measured_output_payload_bytes == 0 ||
      result.output_payload_bytes != measured_output_payload_bytes ||
      result.resident_structural_bytes != 0 ||
      result.current_live_memory_bytes != measured_output_payload_bytes ||
      result.peak_live_payload_bytes < result.output_payload_bytes ||
      result.peak_live_memory_bytes < result.peak_live_payload_bytes ||
      result.peak_live_memory_bytes < result.current_live_memory_bytes ||
      result.peak_live_memory_bytes > node.memory_bytes_required ||
      result.memory_grant_bytes != node.memory_bytes_required ||
      result.memory_grant_evidence_uuid !=
          resource_evidence->evidence_uuid ||
      result.implementation_id != node.implementation_id ||
      result.selected_plan_uuid != dag.selected_plan_uuid ||
      result.executed_physical_node_id != node.physical_node_id ||
      result.causal_counter_id != node.causal_counter_id) {
    *detail =
        "set-operation runtime logical-memory receipt is inconsistent";
    return false;
  }
  *current_memory_bytes = result.current_live_memory_bytes;
  *peak_memory_bytes = result.peak_live_memory_bytes;
  return true;
}

bool CanonicalSetOperationExecutionReceiptMatches(
    const exec::CanonicalSetOperationAllRequest& request,
    const exec::PhysicalNodeRecord& node,
    const exec::CanonicalSetOperationAllResult& result) {
  const auto& left_batch = request.borrowed_left_batch != nullptr
                               ? *request.borrowed_left_batch
                               : request.left_batch;
  const auto& right_batch = request.borrowed_right_batch != nullptr
                                ? *request.borrowed_right_batch
                                : request.right_batch;
  if (!CanonicalOperatorExecutionReceiptMatches(
          result, request.physical_dag, node,
          request.mga_authority.statement_context) ||
      result.left_input_row_count != left_batch.rows.size() ||
      result.right_input_row_count != right_batch.rows.size() ||
      left_batch.rows.size() >
          std::numeric_limits<std::size_t>::max() -
              right_batch.rows.size() ||
      result.equality_comparison_count >
          request.maximum_equality_comparison_count ||
      result.output_batch.rows.size() >
          request.maximum_output_row_count) {
    return false;
  }
  const auto input_row_count =
      left_batch.rows.size() + right_batch.rows.size();
  switch (request.operation) {
    case exec::CanonicalSetOperationKind::kUnion:
      return request.quantifier ==
                     exec::CanonicalSetOperationQuantifier::kAll
                 ? result.output_batch.rows.size() == input_row_count
                 : (request.quantifier ==
                        exec::CanonicalSetOperationQuantifier::kDistinct &&
                    result.output_batch.rows.size() <= input_row_count);
    case exec::CanonicalSetOperationKind::kIntersect:
      return result.output_batch.rows.size() <=
             std::min(left_batch.rows.size(), right_batch.rows.size());
    case exec::CanonicalSetOperationKind::kExcept:
      return result.output_batch.rows.size() <= left_batch.rows.size();
  }
  return false;
}

exec::CanonicalPhysicalExecutorRegistration MakeLiveSetOperationRegistration(
    LiveSetRegistrationProfiles prepared_set_nodes,
    std::string implementation_id,
    std::string capability_uuid,
    api::EngineRequestContext mga_context) {
  exec::CanonicalPhysicalExecutorRegistration registration;
  registration.node_kind = exec::PhysicalNodeKind::kSetOperation;
  registration.implementation_id = std::move(implementation_id);
  registration.executor_capability_uuid = std::move(capability_uuid);
  registration.executor_capability_abi_version = 1;
  registration.engine_owned = true;
  registration.accepts_optimizer_publication_v2 = true;
  registration.publishes_runtime_observation_v1 = true;
  registration.honors_dispatcher_memory_limit_v1 = true;
  registration.execute =
      [prepared_set_nodes = std::move(prepared_set_nodes),
       mga_context = std::move(mga_context)](
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
        const auto prepared =
            prepared_set_nodes.find(node.relational_node_id);
        if (prepared == prepared_set_nodes.end() ||
            prepared->second.implementation_id != node.implementation_id ||
            inputs.size() != 2 ||
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
              "set-operation executor input/profile is unresolved";
          return step;
        }
        const auto& config = prepared->second;
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
        set_request.borrowed_left_batch = &left_input_batch;
        set_request.borrowed_right_batch = &right_input_batch;
        set_request.result_columns = config.result_columns;
        set_request.operation = config.operation;
        set_request.alignment = config.alignment;
        set_request.quantifier = config.quantifier;
        set_request.equality_profile = config.equality_profile;
        set_request.type_profile = config.type_profile;
        set_request.collation_bindings = config.collation_bindings;
        set_request.maximum_equality_comparison_count =
            std::max<std::size_t>(
                1, config.maximum_equality_comparison_count);
        set_request.maximum_output_row_count =
            std::max<std::size_t>(1, config.maximum_output_row_count);
        set_request.enforce_payload_memory_grant = true;
        set_request.mga_authority = BuildCanonicalExecutionMgaAuthority(
            mga_context, execution_dag);
        auto set_result =
            config.quantifier ==
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
  return registration;
}

}  // namespace scratchbird::engine::sblr

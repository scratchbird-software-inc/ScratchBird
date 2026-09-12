// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "canonical_query_window_registration.hpp"

#include "canonical_query_physical_registration.hpp"
#include "canonical_query_runtime_memory_support.hpp"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace scratchbird::engine::sblr {

namespace api = scratchbird::engine::internal_api;

// SEARCH_KEY: SB_ENGINE_CANONICAL_QUERY_WINDOW_REGISTRATION_AUTHORITY
exec::CanonicalPhysicalExecutorRegistration MakeLiveRowNumberRegistration(
    exec::ExecutorColumnDescriptor row_number_column,
    std::string deterministic_order_evidence_uuid,
    std::string capability_uuid,
    const std::size_t maximum_input_row_count,
    api::EngineRequestContext mga_context,
    const api::EngineRequestContext* borrowed_mga_context,
    const exec::CanonicalExecutionMgaAuthority* borrowed_mga_authority) {
  const bool strict_dispatcher_memory =
      borrowed_mga_context != nullptr && borrowed_mga_authority != nullptr;
  std::uint64_t registration_retained_bytes =
      strict_dispatcher_memory
          ? sizeof(exec::ExecutorColumnDescriptor) + sizeof(std::string) +
                sizeof(api::EngineRequestContext) + 8 * sizeof(void*) + 512
          : 0;
  const auto account_string = [&](const std::string& value) {
    return value.capacity() != std::numeric_limits<std::size_t>::max() &&
           CheckedAdd(registration_retained_bytes,
                      static_cast<std::uint64_t>(value.capacity()) + 1,
                      &registration_retained_bytes);
  };
  if (!strict_dispatcher_memory ||
      !account_string(row_number_column.stable_name) ||
      !account_string(
          row_number_column.descriptor.descriptor_uuid) ||
      !account_string(row_number_column.descriptor.descriptor_kind) ||
      !account_string(
          row_number_column.descriptor.canonical_type_name) ||
      !account_string(row_number_column.descriptor.encoded_descriptor) ||
      !account_string(deterministic_order_evidence_uuid)) {
    registration_retained_bytes = 0;
  }
  exec::CanonicalPhysicalExecutorRegistration registration;
  registration.node_kind = exec::PhysicalNodeKind::kWindow;
  registration.implementation_id = "window.row-number.v1";
  registration.executor_capability_uuid = std::move(capability_uuid);
  registration.executor_capability_abi_version = 1;
  registration.engine_owned = true;
  registration.accepts_optimizer_publication_v2 = true;
  registration.honors_dispatcher_memory_limit_v1 = strict_dispatcher_memory;
  registration.retained_live_memory_bytes_v1 =
      registration_retained_bytes;
  registration.execute =
      [row_number_column = std::move(row_number_column),
       deterministic_order_evidence_uuid =
           std::move(deterministic_order_evidence_uuid),
       maximum_input_row_count, mga_context = std::move(mga_context),
       borrowed_mga_authority, strict_dispatcher_memory](
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
            inputs.front().materialized_output_batch->rows.size() >
                maximum_input_row_count) {
          step.diagnostic.ok = false;
          step.diagnostic.diagnostic_code =
              "QOW-DIAG-QRY-007-WINDOW-INPUT-V1";
          step.diagnostic.detail =
              "ROW_NUMBER did not receive its bounded sorted input batch";
          return step;
        }
        const auto& input_batch = *inputs.front().materialized_output_batch;
        const exec::TypedPhysicalNodeDag* execution_dag = &dag;
        std::optional<exec::TypedPhysicalNodeDag> scoped_execution_dag;
        std::size_t callback_memory_bound = 0;
        if (strict_dispatcher_memory) {
          std::uint64_t row_number_memory_bytes = 0;
          if (!CheckedMultiply(input_batch.rows.size(),
                               sizeof(std::uint64_t),
                               &row_number_memory_bytes)) {
            step.diagnostic.ok = false;
            step.diagnostic.diagnostic_code =
                "SBLR.PLAN_TREE.RESOURCE_LIMIT";
            step.diagnostic.detail =
                "ROW_NUMBER result workspace overflows";
            return step;
          }
          scoped_execution_dag.emplace();
          std::string scope_detail;
          if (!BuildStrictUnaryOperatorLocalPhysicalDag(
                  dag, node, input_batch, 2, row_number_memory_bytes,
                  &*scoped_execution_dag, &callback_memory_bound,
                  &scope_detail)) {
            step.diagnostic.ok = false;
            step.diagnostic.diagnostic_code =
                "SBLR.PLAN_TREE.RESOURCE_LIMIT";
            step.diagnostic.detail = "ROW_NUMBER " + scope_detail;
            return step;
          }
          execution_dag = &*scoped_execution_dag;
        } else if (node.physical_node_id != dag.root_physical_node_id) {
          scoped_execution_dag.emplace();
          std::string scope_detail;
          if (!BuildOperatorLocalPhysicalDag(
                  dag, node.physical_node_id, &*scoped_execution_dag,
                  &scope_detail)) {
            step.diagnostic.ok = false;
            step.diagnostic.diagnostic_code =
                "QOW-DIAG-QRY-007-WINDOW-INPUT-V1";
            step.diagnostic.detail =
                "ROW_NUMBER execution view is unresolved";
            return step;
          }
          execution_dag = &*scoped_execution_dag;
        }
        exec::CanonicalDescriptorRowNumberRequest request;
        request.selected_physical_node_id = node.physical_node_id;
        request.row_number_column = row_number_column;
        request.deterministic_order_evidence_uuid =
            deterministic_order_evidence_uuid;
        request.mga_authority = strict_dispatcher_memory
                                    ? *borrowed_mga_authority
                                    : BuildCanonicalExecutionMgaAuthority(
                                          mga_context, *execution_dag);
        auto window = exec::ExecuteCanonicalDescriptorRowNumber(
            request, *execution_dag, input_batch);
        if (!window.diagnostic.ok) {
          step.diagnostic = std::move(window.diagnostic);
          return step;
        }
        if (!CanonicalOperatorExecutionReceiptMatches(
                window, *execution_dag, node,
                request.mga_authority.statement_context)) {
          step.diagnostic.ok = false;
          step.diagnostic.diagnostic_code =
              "QOW-DIAG-QRY-007-WINDOW-PHYSICAL-ROUTE-V1";
          step.diagnostic.detail =
              "ROW_NUMBER execution receipt changed";
          return step;
        }
        step.result_handle_id = node.physical_node_id;
        step.input_row_count = input_batch.rows.size();
        step.rows_examined = input_batch.rows.size();
        step.output_row_count = window.output_batch.rows.size();
        step.materialized_output_batch = std::move(window.output_batch);
        step.mga_statement_context =
            std::move(window.mga_statement_context);
        return step;
      };
  return registration;
}

exec::CanonicalPhysicalExecutorRegistration MakeLiveNtileRegistration(
    exec::ExecutorColumnDescriptor ntile_column,
    exec::CanonicalDescriptorOrderTerm order_term,
    api::EngineTypedValue bucket_count_operand,
    std::string function_uuid,
    std::string deterministic_order_evidence_uuid,
    std::string capability_uuid,
    const std::size_t maximum_input_row_count,
    api::EngineRequestContext mga_context) {
  exec::CanonicalPhysicalExecutorRegistration registration;
  registration.node_kind = exec::PhysicalNodeKind::kWindow;
  registration.implementation_id = "window.ntile.v1";
  registration.executor_capability_uuid = std::move(capability_uuid);
  registration.executor_capability_abi_version = 1;
  registration.engine_owned = true;
  registration.accepts_optimizer_publication_v2 = true;
  registration.publishes_runtime_observation_v1 = true;
  registration.honors_dispatcher_memory_limit_v1 = true;
  registration.execute =
      [ntile_column = std::move(ntile_column),
       order_term = std::move(order_term),
       bucket_count_operand = std::move(bucket_count_operand),
       function_uuid = std::move(function_uuid),
       deterministic_order_evidence_uuid =
           std::move(deterministic_order_evidence_uuid),
       maximum_input_row_count, mga_context = std::move(mga_context)](
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
            inputs.front().materialized_output_batch->rows.size() >
                maximum_input_row_count) {
          step.diagnostic.ok = false;
          step.diagnostic.diagnostic_code =
              "QOW-DIAG-QRY-007-WINDOW-INPUT-V1";
          step.diagnostic.detail =
              "NTILE did not receive its bounded sorted input batch";
          return step;
        }
        const auto& input_batch = *inputs.front().materialized_output_batch;
        const exec::TypedPhysicalNodeDag* execution_dag = &dag;
        std::optional<exec::TypedPhysicalNodeDag> scoped_execution_dag;
        if (node.physical_node_id != dag.root_physical_node_id) {
          scoped_execution_dag.emplace();
          std::string scope_detail;
          if (!BuildOperatorLocalPhysicalDag(
                  dag, node.physical_node_id, &*scoped_execution_dag,
                  &scope_detail)) {
            step.diagnostic.ok = false;
            step.diagnostic.diagnostic_code =
                "QOW-DIAG-QRY-007-WINDOW-INPUT-V1";
            step.diagnostic.detail = "NTILE execution view is unresolved";
            return step;
          }
          execution_dag = &*scoped_execution_dag;
        }
        exec::CanonicalDescriptorNtileRequest request;
        request.selected_physical_node_id = node.physical_node_id;
        request.order_term = order_term;
        request.ntile_column = ntile_column;
        request.bucket_count_operand = bucket_count_operand;
        request.function_abi_version = 1;
        request.builtin_id = "sb.window.ntile";
        request.function_uuid = function_uuid;
        request.deterministic_order_evidence_uuid =
            deterministic_order_evidence_uuid;
        request.mga_authority =
            BuildCanonicalExecutionMgaAuthority(mga_context, *execution_dag);
        if (order_term.column >= input_batch.columns.size() ||
            node.required_property_uuids.size() != 1) {
          step.diagnostic.ok = false;
          step.diagnostic.diagnostic_code = "QOW-DIAG-WINDOW-PROPERTY-BINDING";
          step.diagnostic.detail = "window order-term binding is unresolved";
          return step;
        }
        request.order_term_binding_receipt =
            exec::CanonicalWindowOrderBindingReceipt::Issue(
                *execution_dag, node.physical_node_id,
                input_batch.columns[order_term.column], order_term,
                node.required_property_uuids.front(), request.mga_authority,
                node.memory_bytes_required);
        if (!request.order_term_binding_receipt) {
          step.diagnostic.ok = false;
          step.diagnostic.diagnostic_code = "QOW-DIAG-WINDOW-PROPERTY-BINDING";
          step.diagnostic.detail = "window order-term receipt admission failed";
          return step;
        }
        request.order_term_binding_evidence_uuid =
            request.order_term_binding_receipt->identity();
        auto window = exec::ExecuteCanonicalDescriptorNtile(
            request, *execution_dag, input_batch);
        if (!window.diagnostic.ok) {
          step.diagnostic = std::move(window.diagnostic);
          return step;
        }
        if (!CanonicalOperatorExecutionReceiptMatches(
                window, *execution_dag, node,
                request.mga_authority.statement_context)) {
          step.diagnostic.ok = false;
          step.diagnostic.diagnostic_code =
              "QOW-DIAG-QRY-007-WINDOW-PHYSICAL-ROUTE-V1";
          step.diagnostic.detail = "NTILE execution receipt changed";
          return step;
        }
        const auto decoded_bucket_count =
            exec::DecodeInt64Value(bucket_count_operand);
        if (!decoded_bucket_count.ok() || decoded_bucket_count.value <= 0 ||
            window.resolved_bucket_count !=
                static_cast<std::uint64_t>(decoded_bucket_count.value)) {
          step.diagnostic.ok = false;
          step.diagnostic.diagnostic_code =
              "QOW-DIAG-QRY-007-WINDOW-PHYSICAL-ROUTE-V1";
          step.diagnostic.detail =
              "NTILE executor did not consume its exact retained bucket operand";
          return step;
        }
        std::uint64_t input_memory_bytes = 0;
        std::uint64_t output_memory_bytes = 0;
        std::uint64_t peak_memory_bytes = 0;
        if (!RuntimeMaterializedBatchMemoryBytes(input_batch,
                                                 &input_memory_bytes) ||
            !RuntimeMaterializedBatchMemoryBytes(window.output_batch,
                                                 &output_memory_bytes) ||
            !CheckedAdd(input_memory_bytes, output_memory_bytes,
                        &peak_memory_bytes) ||
            !CheckedAdd(peak_memory_bytes,
                        window.peak_auxiliary_workspace_bytes,
                        &peak_memory_bytes) ||
            peak_memory_bytes > node.memory_bytes_required) {
          step.diagnostic.ok = false;
          step.diagnostic.diagnostic_code = "QOW-DIAG-OPT-017-REFUSAL-V1";
          step.diagnostic.detail =
              "NTILE runtime work or memory observation exceeded its grant";
          return step;
        }
        step.result_handle_id = node.physical_node_id;
        step.input_row_count = input_batch.rows.size();
        step.rows_examined = input_batch.rows.size();
        step.output_row_count = window.output_batch.rows.size();
        step.materialized_output_batch = std::move(window.output_batch);
        step.mga_statement_context = std::move(window.mga_statement_context);
        PublishRuntimeMemoryObservation(
            &step, output_memory_bytes, peak_memory_bytes);
        return step;
      };
  return registration;
}

exec::CanonicalPhysicalExecutorRegistration
MakeLivePeerRankingRegistration(
    exec::ExecutorColumnDescriptor ranking_column,
    exec::CanonicalDescriptorOrderTerm order_term,
    std::string deterministic_order_evidence_uuid,
    std::string capability_uuid,
    const std::size_t maximum_input_row_count,
    const std::size_t maximum_peer_comparisons,
    const GlobalRankingWindowProfile profile,
    api::EngineRequestContext mga_context) {
  exec::CanonicalPhysicalExecutorRegistration registration;
  registration.node_kind = exec::PhysicalNodeKind::kWindow;
  registration.implementation_id = std::string(profile.semantic_variant_id);
  registration.executor_capability_uuid = std::move(capability_uuid);
  registration.executor_capability_abi_version = 1;
  registration.engine_owned = true;
  registration.accepts_optimizer_publication_v2 = true;
  registration.publishes_runtime_observation_v1 = true;
  registration.execute =
      [ranking_column = std::move(ranking_column),
       order_term = std::move(order_term),
       deterministic_order_evidence_uuid =
           std::move(deterministic_order_evidence_uuid),
       maximum_input_row_count, maximum_peer_comparisons, profile,
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
        if (inputs.size() != 1 ||
            node.input_physical_node_ids.size() != 1 ||
            inputs.front().physical_node_id !=
                node.input_physical_node_ids.front() ||
            !inputs.front().materialized_output_batch.has_value() ||
            inputs.front().materialized_output_batch->rows.size() >
                maximum_input_row_count) {
          step.diagnostic.ok = false;
          step.diagnostic.diagnostic_code =
              "QOW-DIAG-QRY-007-WINDOW-INPUT-V1";
          step.diagnostic.detail = std::string(profile.display_name) +
                                   " did not receive its bounded sorted "
                                   "input batch";
          return step;
        }
        const auto& input_batch = *inputs.front().materialized_output_batch;
        const exec::TypedPhysicalNodeDag* execution_dag = &dag;
        std::optional<exec::TypedPhysicalNodeDag> scoped_execution_dag;
        if (node.physical_node_id != dag.root_physical_node_id) {
          scoped_execution_dag.emplace();
          std::string scope_detail;
          if (!BuildOperatorLocalPhysicalDag(
                  dag, node.physical_node_id, &*scoped_execution_dag,
                  &scope_detail)) {
            step.diagnostic.ok = false;
            step.diagnostic.diagnostic_code =
                "QOW-DIAG-QRY-007-WINDOW-INPUT-V1";
            step.diagnostic.detail = std::string(profile.display_name) +
                                     " execution view is unresolved";
            return step;
          }
          execution_dag = &*scoped_execution_dag;
        }
        exec::CanonicalDescriptorPeerRankingRequest request;
        request.selected_physical_node_id = node.physical_node_id;
        request.order_term = order_term;
        request.ranking_column = ranking_column;
        request.function_abi_version = 1;
        request.builtin_id = std::string(profile.builtin_id);
        request.function_uuid = std::string(profile.function_uuid);
        request.deterministic_order_evidence_uuid =
            deterministic_order_evidence_uuid;
        request.maximum_peer_comparisons = maximum_peer_comparisons;
        request.mga_authority =
            BuildCanonicalExecutionMgaAuthority(mga_context, *execution_dag);
        if (order_term.column >= input_batch.columns.size() ||
            node.required_property_uuids.size() != 1) {
          step.diagnostic.ok = false;
          step.diagnostic.diagnostic_code = "QOW-DIAG-WINDOW-PROPERTY-BINDING";
          step.diagnostic.detail = "window order-term binding is unresolved";
          return step;
        }
        request.order_term_binding_receipt =
            exec::CanonicalWindowOrderBindingReceipt::Issue(
                *execution_dag, node.physical_node_id,
                input_batch.columns[order_term.column], order_term,
                node.required_property_uuids.front(), request.mga_authority,
                node.memory_bytes_required);
        if (!request.order_term_binding_receipt) {
          step.diagnostic.ok = false;
          step.diagnostic.diagnostic_code = "QOW-DIAG-WINDOW-PROPERTY-BINDING";
          step.diagnostic.detail = "window order-term receipt admission failed";
          return step;
        }
        request.order_term_binding_evidence_uuid =
            request.order_term_binding_receipt->identity();
        auto window = exec::ExecuteCanonicalDescriptorPeerRanking(
            request, *execution_dag, input_batch);
        if (!window.diagnostic.ok) {
          step.diagnostic = std::move(window.diagnostic);
          return step;
        }
        if (!CanonicalOperatorExecutionReceiptMatches(
                window, *execution_dag, node,
                request.mga_authority.statement_context)) {
          step.diagnostic.ok = false;
          step.diagnostic.diagnostic_code =
              "QOW-DIAG-QRY-007-WINDOW-PHYSICAL-ROUTE-V1";
          step.diagnostic.detail = std::string(profile.display_name) +
                                   " execution receipt changed";
          return step;
        }
        std::uint64_t input_memory_bytes = 0;
        std::uint64_t output_memory_bytes = 0;
        std::uint64_t peak_memory_bytes = 0;
        std::uint64_t rows_examined = 0;
        if (!RuntimeMaterializedBatchMemoryBytes(input_batch,
                                                 &input_memory_bytes) ||
            !RuntimeMaterializedBatchMemoryBytes(window.output_batch,
                                                 &output_memory_bytes) ||
            !CheckedAdd(
                static_cast<std::uint64_t>(input_batch.rows.size()),
                static_cast<std::uint64_t>(window.peer_comparison_count),
                &rows_examined) ||
            !CheckedAdd(input_memory_bytes, output_memory_bytes,
                        &peak_memory_bytes) ||
            !CheckedAdd(peak_memory_bytes,
                        window.peak_auxiliary_workspace_bytes,
                        &peak_memory_bytes) ||
            peak_memory_bytes > node.memory_bytes_required ||
            window.peer_comparison_count > maximum_peer_comparisons) {
          step.diagnostic.ok = false;
          step.diagnostic.diagnostic_code =
              "QOW-DIAG-OPT-017-REFUSAL-V1";
          step.diagnostic.detail = std::string(profile.display_name) +
                                   " runtime work or memory observation "
                                   "exceeded its grant";
          return step;
        }
        step.result_handle_id = node.physical_node_id;
        step.input_row_count = input_batch.rows.size();
        step.rows_examined = rows_examined;
        step.output_row_count = window.output_batch.rows.size();
        step.materialized_output_batch = std::move(window.output_batch);
        step.mga_statement_context =
            std::move(window.mga_statement_context);
        PublishRuntimeMemoryObservation(
            &step, output_memory_bytes, peak_memory_bytes);
        return step;
      };
  return registration;
}

exec::CanonicalPhysicalExecutorRegistration
MakeLiveNavigationWindowRegistration(
    exec::ExecutorColumnDescriptor result_column,
    exec::CanonicalDescriptorOrderTerm order_term,
    const std::size_t value_column,
    std::optional<api::EngineTypedValue> nth_value_position_operand,
    std::string window_frame_descriptor_uuid,
    std::string deterministic_order_evidence_uuid,
    std::string frame_property_binding_evidence_uuid,
    std::string capability_uuid,
    const std::size_t maximum_input_row_count,
    const std::size_t maximum_pair_comparisons,
    const std::size_t maximum_effective_row_references,
    const GlobalRankingWindowProfile profile,
    api::EngineRequestContext mga_context) {
  exec::CanonicalPhysicalExecutorRegistration registration;
  registration.node_kind = exec::PhysicalNodeKind::kWindow;
  registration.implementation_id = std::string(profile.semantic_variant_id);
  registration.executor_capability_uuid = capability_uuid;
  registration.executor_capability_abi_version = 1;
  registration.engine_owned = true;
  registration.accepts_optimizer_publication_v2 = true;
  registration.publishes_runtime_observation_v1 = true;
  registration.execute =
      [result_column = std::move(result_column),
       order_term = std::move(order_term), value_column,
       nth_value_position_operand = std::move(nth_value_position_operand),
       window_frame_descriptor_uuid =
           std::move(window_frame_descriptor_uuid),
       deterministic_order_evidence_uuid =
           std::move(deterministic_order_evidence_uuid),
       frame_property_binding_evidence_uuid =
           std::move(frame_property_binding_evidence_uuid),
       capability_uuid = std::move(capability_uuid),
       maximum_input_row_count, maximum_pair_comparisons,
       maximum_effective_row_references, profile,
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
        if (inputs.size() != 1 ||
            node.input_physical_node_ids.size() != 1 ||
            inputs.front().physical_node_id !=
                node.input_physical_node_ids.front() ||
            !inputs.front().materialized_output_batch.has_value() ||
            inputs.front().materialized_output_batch->rows.size() >
                maximum_input_row_count) {
          step.diagnostic.ok = false;
          step.diagnostic.diagnostic_code =
              "QOW-DIAG-QRY-007-WINDOW-INPUT-V1";
          step.diagnostic.detail = std::string(profile.display_name) +
                                   " did not receive its bounded sorted input batch";
          return step;
        }
        const auto& input_batch = *inputs.front().materialized_output_batch;
        const exec::TypedPhysicalNodeDag* execution_dag = &dag;
        std::optional<exec::TypedPhysicalNodeDag> scoped_execution_dag;
        if (node.physical_node_id != dag.root_physical_node_id) {
          scoped_execution_dag.emplace();
          std::string scope_detail;
          if (!BuildOperatorLocalPhysicalDag(
                  dag, node.physical_node_id, &*scoped_execution_dag,
                  &scope_detail)) {
            step.diagnostic.ok = false;
            step.diagnostic.diagnostic_code =
                "QOW-DIAG-QRY-007-WINDOW-INPUT-V1";
            step.diagnostic.detail = std::string(profile.display_name) +
                                     " execution view is unresolved";
            return step;
          }
          execution_dag = &*scoped_execution_dag;
        }
        exec::CanonicalDescriptorNavigationWindowRequest request;
        request.selected_physical_node_id = node.physical_node_id;
        request.order_term = order_term;
        request.value_column = value_column;
        request.result_column = result_column;
        request.nth_value_position_operand = nth_value_position_operand;
        request.nth_value_from_first_explicit =
            profile.builtin_id == "sb.window.nth_value";
        request.nth_value_respect_nulls_explicit =
            profile.builtin_id == "sb.window.nth_value";
        request.function_abi_version = 1;
        request.builtin_id = std::string(profile.builtin_id);
        request.function_uuid = std::string(profile.function_uuid);
        request.window_frame_descriptor_uuid =
            window_frame_descriptor_uuid;
        request.deterministic_order_evidence_uuid =
            deterministic_order_evidence_uuid;
        request.frame_property_binding_evidence_uuid =
            frame_property_binding_evidence_uuid;
        request.executor_capability_uuid = capability_uuid;
        request.maximum_pair_comparisons = maximum_pair_comparisons;
        request.maximum_effective_row_references =
            maximum_effective_row_references;
        request.mga_authority =
            BuildCanonicalExecutionMgaAuthority(mga_context, *execution_dag);
        if (order_term.column >= input_batch.columns.size() ||
            node.required_property_uuids.size() != 1) {
          step.diagnostic.ok = false;
          step.diagnostic.diagnostic_code = "QOW-DIAG-WINDOW-PROPERTY-BINDING";
          step.diagnostic.detail = "window order-term binding is unresolved";
          return step;
        }
        request.order_term_binding_receipt =
            exec::CanonicalWindowOrderBindingReceipt::Issue(
                *execution_dag, node.physical_node_id,
                input_batch.columns[order_term.column], order_term,
                node.required_property_uuids.front(), request.mga_authority,
                node.memory_bytes_required);
        if (!request.order_term_binding_receipt) {
          step.diagnostic.ok = false;
          step.diagnostic.diagnostic_code = "QOW-DIAG-WINDOW-PROPERTY-BINDING";
          step.diagnostic.detail = "window order-term receipt admission failed";
          return step;
        }
        request.order_term_binding_evidence_uuid =
            request.order_term_binding_receipt->identity();
        auto window = exec::ExecuteCanonicalDescriptorNavigationWindow(
            request, *execution_dag, input_batch);
        if (!window.diagnostic.ok) {
          step.diagnostic = std::move(window.diagnostic);
          return step;
        }
        if (!CanonicalOperatorExecutionReceiptMatches(
                window, *execution_dag, node,
                request.mga_authority.statement_context)) {
          step.diagnostic.ok = false;
          step.diagnostic.diagnostic_code =
              "QOW-DIAG-QRY-007-WINDOW-PHYSICAL-ROUTE-V1";
          step.diagnostic.detail = std::string(profile.display_name) +
                                   " execution receipt changed";
          return step;
        }
        std::uint64_t input_memory_bytes = 0;
        std::uint64_t output_memory_bytes = 0;
        std::uint64_t peak_memory_bytes = 0;
        std::uint64_t rows_examined = 0;
        if (!RuntimeMaterializedBatchMemoryBytes(input_batch,
                                                 &input_memory_bytes) ||
            !RuntimeMaterializedBatchMemoryBytes(window.output_batch,
                                                 &output_memory_bytes) ||
            !CheckedAdd(input_memory_bytes, output_memory_bytes,
                        &peak_memory_bytes) ||
            !CheckedAdd(peak_memory_bytes,
                        window.peak_auxiliary_workspace_bytes,
                        &peak_memory_bytes) ||
            !CheckedAdd(
                static_cast<std::uint64_t>(input_batch.rows.size()),
                static_cast<std::uint64_t>(
                    window.partition_order_comparison_count),
                &rows_examined) ||
            !CheckedAdd(
                rows_examined,
                static_cast<std::uint64_t>(
                    window.effective_frame_row_reference_count),
                &rows_examined) ||
            peak_memory_bytes > node.memory_bytes_required ||
            window.partition_order_comparison_count >
                maximum_pair_comparisons ||
            window.effective_frame_row_reference_count >
                maximum_effective_row_references) {
          step.diagnostic.ok = false;
          step.diagnostic.diagnostic_code = "QOW-DIAG-OPT-017-REFUSAL-V1";
          step.diagnostic.detail = std::string(profile.display_name) +
                                   " runtime work or memory observation exceeded its grant";
          return step;
        }
        step.result_handle_id = node.physical_node_id;
        step.input_row_count = input_batch.rows.size();
        step.rows_examined = rows_examined;
        step.output_row_count = window.output_batch.rows.size();
        step.materialized_output_batch = std::move(window.output_batch);
        step.mga_statement_context = std::move(window.mga_statement_context);
        PublishRuntimeMemoryObservation(
            &step, output_memory_bytes, peak_memory_bytes);
        return step;
      };
  return registration;
}

exec::CanonicalPhysicalExecutorRegistration
MakeLiveAggregateWindowRegistration(
    exec::ExecutorColumnDescriptor result_column,
    exec::CanonicalDescriptorOrderTerm order_term,
    std::optional<std::size_t> value_column,
    exec::CanonicalAggregateDescriptor aggregate_descriptor,
    std::string window_frame_descriptor_uuid,
    std::string deterministic_order_evidence_uuid,
    std::string frame_property_binding_evidence_uuid,
    std::string capability_uuid,
    const std::size_t maximum_input_row_count,
    const std::size_t maximum_pair_comparisons,
    const std::size_t maximum_effective_row_references,
    const std::size_t maximum_transition_count,
    api::EngineRequestContext mga_context) {
  exec::CanonicalPhysicalExecutorRegistration registration;
  registration.node_kind = exec::PhysicalNodeKind::kWindow;
  registration.implementation_id =
      "window.aggregate-registry-frame-recompute.v1";
  registration.executor_capability_uuid = capability_uuid;
  registration.executor_capability_abi_version = 1;
  registration.engine_owned = true;
  registration.accepts_optimizer_publication_v2 = true;
  registration.publishes_runtime_observation_v1 = true;
  registration.execute =
      [result_column = std::move(result_column),
       order_term = std::move(order_term), value_column,
       aggregate_descriptor = std::move(aggregate_descriptor),
       window_frame_descriptor_uuid =
           std::move(window_frame_descriptor_uuid),
       deterministic_order_evidence_uuid =
           std::move(deterministic_order_evidence_uuid),
       frame_property_binding_evidence_uuid =
           std::move(frame_property_binding_evidence_uuid),
       capability_uuid = std::move(capability_uuid), maximum_input_row_count,
       maximum_pair_comparisons, maximum_effective_row_references,
       maximum_transition_count, mga_context = std::move(mga_context)](
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
            inputs.front().materialized_output_batch->rows.size() >
                maximum_input_row_count) {
          step.diagnostic.ok = false;
          step.diagnostic.diagnostic_code =
              "QOW-DIAG-WINDOW-AGGREGATE-REGISTRY-INPUT";
          step.diagnostic.detail =
              "aggregate window did not receive its bounded sorted input batch";
          return step;
        }
        const auto& input_batch = *inputs.front().materialized_output_batch;
        const exec::TypedPhysicalNodeDag* execution_dag = &dag;
        std::optional<exec::TypedPhysicalNodeDag> scoped_execution_dag;
        if (node.physical_node_id != dag.root_physical_node_id) {
          scoped_execution_dag.emplace();
          std::string scope_detail;
          if (!BuildOperatorLocalPhysicalDag(
                  dag, node.physical_node_id, &*scoped_execution_dag,
                  &scope_detail)) {
            step.diagnostic.ok = false;
            step.diagnostic.diagnostic_code =
                "QOW-DIAG-WINDOW-AGGREGATE-REGISTRY-INPUT";
            step.diagnostic.detail =
                "aggregate window execution view is unresolved";
            return step;
          }
          execution_dag = &*scoped_execution_dag;
        }
        exec::CanonicalDescriptorAggregateWindowRequest request;
        request.selected_physical_node_id = node.physical_node_id;
        request.order_term = order_term;
        request.value_column = value_column;
        request.aggregate_descriptor = aggregate_descriptor;
        request.result_column = result_column;
        request.window_frame_descriptor_uuid =
            window_frame_descriptor_uuid;
        request.deterministic_order_evidence_uuid =
            deterministic_order_evidence_uuid;
        request.frame_property_binding_evidence_uuid =
            frame_property_binding_evidence_uuid;
        request.executor_capability_uuid = capability_uuid;
        request.maximum_pair_comparisons = maximum_pair_comparisons;
        request.maximum_effective_row_references =
            maximum_effective_row_references;
        request.maximum_transition_count = maximum_transition_count;
        request.mga_authority = BuildCanonicalExecutionMgaAuthority(
            mga_context, *execution_dag);
        if (order_term.column >= input_batch.columns.size() ||
            node.required_property_uuids.size() != 1) {
          step.diagnostic.ok = false;
          step.diagnostic.diagnostic_code = "QOW-DIAG-WINDOW-PROPERTY-BINDING";
          step.diagnostic.detail = "window order-term binding is unresolved";
          return step;
        }
        request.order_term_binding_receipt =
            exec::CanonicalWindowOrderBindingReceipt::Issue(
                *execution_dag, node.physical_node_id,
                input_batch.columns[order_term.column], order_term,
                node.required_property_uuids.front(), request.mga_authority,
                node.memory_bytes_required);
        if (!request.order_term_binding_receipt) {
          step.diagnostic.ok = false;
          step.diagnostic.diagnostic_code = "QOW-DIAG-WINDOW-PROPERTY-BINDING";
          step.diagnostic.detail = "window order-term receipt admission failed";
          return step;
        }
        request.order_term_binding_evidence_uuid =
            request.order_term_binding_receipt->identity();
        auto window = exec::ExecuteCanonicalDescriptorAggregateWindow(
            request, *execution_dag, input_batch);
        if (!window.diagnostic.ok) {
          step.diagnostic = std::move(window.diagnostic);
          return step;
        }
        if (!CanonicalOperatorExecutionReceiptMatches(
                window, *execution_dag, node,
                request.mga_authority.statement_context)) {
          step.diagnostic.ok = false;
          step.diagnostic.diagnostic_code =
              "QOW-DIAG-WINDOW-AGGREGATE-REGISTRY-PHYSICAL";
          step.diagnostic.detail =
              "aggregate window execution receipt changed";
          return step;
        }
        std::uint64_t input_memory_bytes = 0;
        std::uint64_t output_memory_bytes = 0;
        std::uint64_t peak_memory_bytes = 0;
        std::uint64_t rows_examined = 0;
        if (!RuntimeMaterializedBatchMemoryBytes(input_batch,
                                                 &input_memory_bytes) ||
            !RuntimeMaterializedBatchMemoryBytes(window.output_batch,
                                                 &output_memory_bytes) ||
            !CheckedAdd(input_memory_bytes, output_memory_bytes,
                        &peak_memory_bytes) ||
            !CheckedAdd(peak_memory_bytes,
                        window.peak_auxiliary_workspace_bytes,
                        &peak_memory_bytes) ||
            !CheckedAdd(input_batch.rows.size(),
                        window.partition_order_comparison_count,
                        &rows_examined) ||
            !CheckedAdd(rows_examined,
                        window.effective_frame_row_reference_count,
                        &rows_examined) ||
            !CheckedAdd(rows_examined,
                        window.aggregate_transition_count,
                        &rows_examined) ||
            peak_memory_bytes > node.memory_bytes_required ||
            window.partition_order_comparison_count >
                maximum_pair_comparisons ||
            window.effective_frame_row_reference_count >
                maximum_effective_row_references ||
            window.aggregate_transition_count > maximum_transition_count) {
          step.diagnostic.ok = false;
          step.diagnostic.diagnostic_code = "QOW-DIAG-OPT-017-REFUSAL-V1";
          step.diagnostic.detail =
              "aggregate window runtime work or memory observation exceeded its grant";
          return step;
        }
        step.authority.engine_mga_snapshot_bound = true;
        step.result_handle_id = node.physical_node_id;
        step.input_row_count = input_batch.rows.size();
        step.rows_examined = rows_examined;
        step.output_row_count = window.output_batch.rows.size();
        step.materialized_output_batch = std::move(window.output_batch);
        step.mga_statement_context = std::move(window.mga_statement_context);
        PublishRuntimeMemoryObservation(
            &step, output_memory_bytes, peak_memory_bytes);
        return step;
      };
  return registration;
}

}  // namespace scratchbird::engine::sblr

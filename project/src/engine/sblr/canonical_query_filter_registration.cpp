// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "canonical_query_filter_registration.hpp"

#include "canonical_query_physical_registration.hpp"
#include "canonical_query_predicate_support.hpp"
#include "canonical_query_runtime_memory_support.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace scratchbird::engine::sblr {

namespace api = scratchbird::engine::internal_api;

// SEARCH_KEY: SB_ENGINE_CANONICAL_QUERY_FILTER_REGISTRATION_AUTHORITY
exec::CanonicalPhysicalExecutorRegistration MakeLiveFilterRegistration(
    const std::uint32_t predicate_expression_id,
    CanonicalRelationalExpressionRowBinding predicate_row_binding,
    api::TypedRelationalDag relational_dag,
    CanonicalRelationalExpressionRuntimeServices expression_services,
    std::string capability_uuid,
    const std::size_t expected_input_row_count,
    api::EngineRequestContext mga_context) {
  exec::CanonicalPhysicalExecutorRegistration registration;
  registration.node_kind = exec::PhysicalNodeKind::kFilter;
  registration.implementation_id = "filter.3vl.row.v1";
  registration.executor_capability_uuid = std::move(capability_uuid);
  registration.executor_capability_abi_version = 1;
  registration.engine_owned = true;
  registration.accepts_optimizer_publication_v2 = true;
  registration.execute =
      [predicate_expression_id,
       predicate_row_binding = std::move(predicate_row_binding),
       relational_dag = std::move(relational_dag),
       expression_services = std::move(expression_services),
       expected_input_row_count, mga_context = std::move(mga_context)](
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
        if (node.input_physical_node_ids.size() != 1 ||
            inputs.size() != 1 ||
            inputs.front().physical_node_id !=
                node.input_physical_node_ids.front() ||
            !inputs.front().materialized_output_batch.has_value()) {
          step.diagnostic.ok = false;
          step.diagnostic.diagnostic_code =
              "QOW-DIAG-RELATIONAL-LIVE-FILTER-INPUT-V1";
          step.diagnostic.detail =
              "FILTER executor did not receive one typed input batch";
          return step;
        }
        const auto& input_batch = *inputs.front().materialized_output_batch;
        if (input_batch.rows.size() != expected_input_row_count) {
          step.diagnostic.ok = false;
          step.diagnostic.diagnostic_code =
              "QOW-DIAG-RELATIONAL-LIVE-FILTER-INPUT-V1";
          step.diagnostic.detail =
              "FILTER input cardinality differs from its selected cost";
          return step;
        }
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
                "QOW-DIAG-RELATIONAL-LIVE-FILTER-INPUT-V1";
            step.diagnostic.detail = "FILTER execution view is unresolved";
            return step;
          }
          execution_dag = &*scoped_execution_dag;
        }
        const auto mga_authority =
            BuildCanonicalExecutionMgaAuthority(mga_context, *execution_dag);
        auto filter_result = IssueAndExecuteCanonicalFilterPredicateBorrowed(
            relational_dag, predicate_expression_id, predicate_row_binding,
            input_batch, expression_services,
            api::EngineCanonicalExpressionConsumer::filter,
            api::EnginePredicateConsumer::filter, *execution_dag,
            node.physical_node_id, node.physical_node_id,
            expected_input_row_count, mga_authority);
        if (!filter_result.diagnostic.ok) {
          step.diagnostic = std::move(filter_result.diagnostic);
          return step;
        }
        if (!CanonicalOperatorExecutionReceiptMatches(
                filter_result, *execution_dag, node,
                mga_authority.statement_context)) {
          step.diagnostic.ok = false;
          step.diagnostic.diagnostic_code =
              "QOW-DIAG-RELATIONAL-LIVE-FILTER-EXECUTION-V1";
          step.diagnostic.detail = "FILTER execution receipt changed";
          return step;
        }
        step.result_handle_id = node.physical_node_id;
        step.input_row_count = input_batch.rows.size();
        step.rows_examined = input_batch.rows.size();
        step.output_row_count = filter_result.output_batch.rows.size();
        step.materialized_output_batch =
            std::move(filter_result.output_batch);
        step.mga_statement_context =
            std::move(filter_result.mga_statement_context);
        return step;
      };
  return registration;
}

exec::CanonicalPhysicalExecutorRegistration MakeLiveHeapFilterRegistration(
    const std::uint32_t predicate_expression_id,
    CanonicalRelationalExpressionRowBinding predicate_row_binding,
    api::TypedRelationalDag relational_dag,
    CanonicalRelationalExpressionRuntimeServices expression_services,
    std::string capability_uuid,
    const std::size_t maximum_input_row_count,
    api::EngineRequestContext mga_context,
    const api::EngineCanonicalExpressionConsumer expression_consumer,
    const api::EnginePredicateConsumer predicate_consumer,
    const api::TypedRelationalDag* borrowed_relational_dag,
    const api::EngineRequestContext* borrowed_mga_context,
    const exec::CanonicalExecutionMgaAuthority* borrowed_mga_authority,
    std::vector<LiveFilterRuntimeNodeConfiguration>
        runtime_node_configurations) {
  const bool strict_dispatcher_memory =
      borrowed_relational_dag != nullptr &&
      borrowed_mga_context != nullptr && borrowed_mga_authority != nullptr;
  std::uint64_t registration_retained_bytes =
      sizeof(CanonicalRelationalExpressionRowBinding) +
      sizeof(api::TypedRelationalDag) +
      sizeof(CanonicalRelationalExpressionRuntimeServices) +
      sizeof(api::EngineRequestContext) +
      sizeof(std::vector<LiveFilterRuntimeNodeConfiguration>) +
      12 * sizeof(void*) + 1024;
  const auto account_registration_array =
      [&](const std::size_t count, const std::size_t width) {
        std::uint64_t bytes = 0;
        return CheckedMultiply(count, width, &bytes) &&
               CheckedAdd(registration_retained_bytes, bytes,
                          &registration_retained_bytes);
      };
  if (!strict_dispatcher_memory ||
      !account_registration_array(
          predicate_row_binding.row_descriptor_ids.capacity(),
          sizeof(std::uint32_t)) ||
      !account_registration_array(
          (predicate_row_binding.row_nullable.capacity() + 63) / 64,
          sizeof(std::uint64_t)) ||
      !account_registration_array(
          predicate_row_binding.slots.capacity(),
          sizeof(CanonicalRelationalExpressionRowSlotBinding)) ||
      !account_registration_array(
          runtime_node_configurations.capacity(),
          sizeof(LiveFilterRuntimeNodeConfiguration))) {
    registration_retained_bytes = 0;
  }
  for (const auto& configuration : runtime_node_configurations) {
    if (registration_retained_bytes == 0 ||
        !account_registration_array(
            configuration.predicate_row_binding.row_descriptor_ids.capacity(),
            sizeof(std::uint32_t)) ||
        !account_registration_array(
            (configuration.predicate_row_binding.row_nullable.capacity() + 63) /
                64,
            sizeof(std::uint64_t)) ||
        !account_registration_array(
            configuration.predicate_row_binding.slots.capacity(),
            sizeof(CanonicalRelationalExpressionRowSlotBinding))) {
      registration_retained_bytes = 0;
      break;
    }
  }
  exec::CanonicalPhysicalExecutorRegistration registration;
  registration.node_kind = exec::PhysicalNodeKind::kFilter;
  registration.implementation_id = "filter.3vl.row.v1";
  registration.executor_capability_uuid = std::move(capability_uuid);
  registration.executor_capability_abi_version = 1;
  registration.engine_owned = true;
  registration.accepts_optimizer_publication_v2 = true;
  registration.honors_dispatcher_memory_limit_v1 = strict_dispatcher_memory;
  registration.retained_live_memory_bytes_v1 = registration_retained_bytes;
  registration.execute =
      [predicate_expression_id,
       predicate_row_binding = std::move(predicate_row_binding),
       relational_dag = std::move(relational_dag),
       expression_services = std::move(expression_services),
       maximum_input_row_count, mga_context = std::move(mga_context),
       expression_consumer, predicate_consumer, borrowed_relational_dag,
       borrowed_mga_authority, strict_dispatcher_memory,
       runtime_node_configurations = std::move(runtime_node_configurations)](
          const exec::TypedPhysicalNodeDag& dag,
          const exec::PhysicalNodeRecord& node,
          const std::vector<exec::CanonicalPhysicalDispatchInput>& inputs) {
        exec::CanonicalPhysicalDispatchStepResult step;
        step.executed_physical_node_id = node.physical_node_id;
        step.causal_counter_id = node.causal_counter_id;
        const LiveFilterRuntimeNodeConfiguration* runtime_node_configuration =
            nullptr;
        for (const auto& candidate : runtime_node_configurations) {
          if (candidate.relational_node_id != node.relational_node_id) continue;
          if (runtime_node_configuration != nullptr) {
            step.diagnostic.ok = false;
            step.diagnostic.diagnostic_code =
                "QOW-DIAG-PACKET7-OBJECT-HEAP-FILTER-INPUT-V1";
            step.diagnostic.detail =
                "object-backed FILTER has duplicate logical-node configuration";
            return step;
          }
          runtime_node_configuration = &candidate;
        }
        if (!runtime_node_configurations.empty() &&
            runtime_node_configuration == nullptr) {
          step.diagnostic.ok = false;
          step.diagnostic.diagnostic_code =
              "QOW-DIAG-PACKET7-OBJECT-HEAP-FILTER-INPUT-V1";
          step.diagnostic.detail =
              "object-backed FILTER logical-node configuration is absent";
          return step;
        }
        const auto active_predicate_expression_id =
            runtime_node_configuration == nullptr
                ? predicate_expression_id
                : runtime_node_configuration->predicate_expression_id;
        const auto& active_predicate_row_binding =
            runtime_node_configuration == nullptr
                ? predicate_row_binding
                : runtime_node_configuration->predicate_row_binding;
        if (node.input_physical_node_ids.size() != 1 ||
            inputs.size() != 1 ||
            inputs.front().physical_node_id !=
                node.input_physical_node_ids.front() ||
            !inputs.front().materialized_output_batch.has_value() ||
            inputs.front().materialized_output_batch->rows.size() >
                maximum_input_row_count) {
          step.diagnostic.ok = false;
          step.diagnostic.diagnostic_code =
              "QOW-DIAG-PACKET7-OBJECT-HEAP-FILTER-INPUT-V1";
          step.diagnostic.detail =
              "object-backed FILTER input exceeds its admitted bound";
          return step;
        }
        const auto& input_batch = *inputs.front().materialized_output_batch;
        const exec::TypedPhysicalNodeDag* execution_dag = &dag;
        std::optional<exec::TypedPhysicalNodeDag> scoped_execution_dag;
        std::size_t callback_memory_bound = 0;
        if (strict_dispatcher_memory) {
          std::uint64_t input_live_bytes = 0;
          std::uint64_t truth_bytes = 0;
          std::uint64_t auxiliary_bytes = 64 * 1024;
          if (!BoundDescriptorBatchLiveMemoryBytes(input_batch,
                                                   &input_live_bytes) ||
              !CheckedMultiply(input_batch.rows.size(),
                               sizeof(api::EngineSqlTruthValue),
                               &truth_bytes) ||
              !CheckedAdd(auxiliary_bytes, truth_bytes,
                          &auxiliary_bytes)) {
            step.diagnostic.ok = false;
            step.diagnostic.diagnostic_code =
                "SBLR.PLAN_TREE.RESOURCE_LIMIT";
            step.diagnostic.detail =
                "object-backed FILTER predicate accounting overflowed";
            return step;
          }
          const auto& active_relational_dag = *borrowed_relational_dag;
          const auto predicate_scratch = BoundCanonicalPredicateScratchBytes(
              active_relational_dag, active_predicate_expression_id,
              active_predicate_row_binding, input_live_bytes,
              [] { return false; });
          if (!predicate_scratch.ok ||
              !CheckedAdd(auxiliary_bytes,
                          predicate_scratch.maximum_payload_bytes,
                          &auxiliary_bytes) ||
              !CheckedAdd(auxiliary_bytes,
                          predicate_scratch.maximum_structural_bytes,
                          &auxiliary_bytes)) {
            step.diagnostic.ok = false;
            step.diagnostic.diagnostic_code =
                "SBLR.PLAN_TREE.RESOURCE_LIMIT";
            step.diagnostic.detail =
                "object-backed FILTER predicate scratch is unbounded";
            return step;
          }
          scoped_execution_dag.emplace();
          std::string scope_detail;
          if (!BuildStrictUnaryOperatorLocalPhysicalDag(
                  dag, node, input_batch, 1, auxiliary_bytes,
                  &*scoped_execution_dag, &callback_memory_bound,
                  &scope_detail)) {
            step.diagnostic.ok = false;
            step.diagnostic.diagnostic_code =
                "SBLR.PLAN_TREE.RESOURCE_LIMIT";
            step.diagnostic.detail = "object-backed FILTER " + scope_detail;
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
                "QOW-DIAG-PACKET7-OBJECT-HEAP-FILTER-INPUT-V1";
            step.diagnostic.detail =
                "object-backed FILTER execution view is unresolved";
            return step;
          }
          execution_dag = &*scoped_execution_dag;
        }
        const auto input_validation = exec::ValidateCanonicalDescriptorBatch(
            input_batch, inputs.front().output_descriptor_ids);
        if (!input_validation.ok) {
          step.diagnostic = input_validation;
          return step;
        }
        std::optional<exec::CanonicalExecutionMgaAuthority> owned_authority;
        const exec::CanonicalExecutionMgaAuthority* active_authority =
            borrowed_mga_authority;
        if (!strict_dispatcher_memory) {
          owned_authority =
              BuildCanonicalExecutionMgaAuthority(mga_context,
                                                  *execution_dag);
          active_authority = &*owned_authority;
        }
        auto filtered = IssueAndExecuteCanonicalFilterPredicateBorrowed(
            strict_dispatcher_memory ? *borrowed_relational_dag
                                     : relational_dag,
            active_predicate_expression_id, active_predicate_row_binding,
            input_batch, expression_services, expression_consumer,
            predicate_consumer, *execution_dag, node.physical_node_id,
            node.physical_node_id, maximum_input_row_count, *active_authority,
            strict_dispatcher_memory);
        if (!filtered.diagnostic.ok) {
          step.diagnostic = std::move(filtered.diagnostic);
          return step;
        }
        if (!CanonicalOperatorExecutionReceiptMatches(
                filtered, *execution_dag, node,
                active_authority->statement_context)) {
          step.diagnostic.ok = false;
          step.diagnostic.diagnostic_code =
              "QOW-DIAG-PACKET7-OBJECT-HEAP-FILTER-EXECUTION-V1";
          step.diagnostic.detail =
              "object-backed FILTER execution receipt changed";
          return step;
        }
        step.selected_plan_uuid = std::move(filtered.selected_plan_uuid);
        step.output_descriptor_ids = node.output_descriptor_ids;
        step.authority.engine_mga_snapshot_bound = true;
        step.result_handle_id = node.physical_node_id;
        step.input_row_count = input_batch.rows.size();
        step.rows_examined = input_batch.rows.size();
        step.output_row_count = filtered.output_batch.rows.size();
        step.materialized_output_batch = std::move(filtered.output_batch);
        step.mga_statement_context =
            std::move(filtered.mga_statement_context);
        return step;
      };
  return registration;
}

}  // namespace scratchbird::engine::sblr

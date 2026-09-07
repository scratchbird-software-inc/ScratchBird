// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "canonical_query_relational_registration.hpp"

#include "canonical_query_physical_registration.hpp"
#include "canonical_query_runtime_memory_support.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <ranges>
#include <string>
#include <utility>
#include <vector>

namespace scratchbird::engine::sblr {

namespace api = scratchbird::engine::internal_api;

// SEARCH_KEY: SB_ENGINE_CANONICAL_QUERY_RELATIONAL_REGISTRATION_AUTHORITY
exec::CanonicalPhysicalExecutorRegistration
MakeLiveQueryDistinctRegistration(
    std::vector<exec::CanonicalDescriptorOrderTerm> equality_terms,
    std::string capability_uuid,
    const std::size_t maximum_input_row_count,
    const std::size_t maximum_value_comparisons,
    api::EngineRequestContext mga_context) {
  exec::CanonicalPhysicalExecutorRegistration registration;
  registration.node_kind = exec::PhysicalNodeKind::kAggregate;
  registration.implementation_id =
      "aggregate.query-distinct.typed.v1";
  registration.executor_capability_uuid = std::move(capability_uuid);
  registration.executor_capability_abi_version = 1;
  registration.engine_owned = true;
  registration.accepts_optimizer_publication_v2 = true;
  registration.execute =
      [equality_terms = std::move(equality_terms), maximum_input_row_count,
       maximum_value_comparisons, mga_context = std::move(mga_context)](
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
              "QOW-DIAG-RELATIONAL-LIVE-DISTINCT-INPUT-V1";
          step.diagnostic.detail =
              "query DISTINCT did not receive its bounded typed input batch";
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
                "QOW-DIAG-RELATIONAL-LIVE-DISTINCT-INPUT-V1";
            step.diagnostic.detail =
                "query DISTINCT execution view is unresolved";
            return step;
          }
          execution_dag = &*scoped_execution_dag;
        }
        exec::CanonicalDescriptorDistinctRequest distinct_request;
        distinct_request.selected_physical_node_id = node.physical_node_id;
        distinct_request.maximum_value_comparisons =
            maximum_value_comparisons;
        distinct_request.mga_authority =
            BuildCanonicalExecutionMgaAuthority(mga_context, *execution_dag);
        auto distinct_result = exec::ExecuteCanonicalDescriptorDistinct(
            distinct_request, *execution_dag, input_batch, equality_terms);
        if (!distinct_result.diagnostic.ok) {
          step.diagnostic = std::move(distinct_result.diagnostic);
          return step;
        }
        if (!CanonicalOperatorExecutionReceiptMatches(
                distinct_result, *execution_dag, node,
                distinct_request.mga_authority.statement_context)) {
          step.diagnostic.ok = false;
          step.diagnostic.diagnostic_code =
              "QOW-DIAG-QRY-010-DISTINCT-REFUSAL-V1";
          step.diagnostic.detail =
              "query DISTINCT execution receipt changed";
          return step;
        }
        if (distinct_result.output_batch.rows.size() >
                input_batch.rows.size() ||
            distinct_result.eliminated_duplicate_row_count !=
                input_batch.rows.size() -
                    distinct_result.output_batch.rows.size() ||
            distinct_result.value_comparison_count >
                maximum_value_comparisons) {
          step.diagnostic.ok = false;
          step.diagnostic.diagnostic_code =
              "QOW-DIAG-QRY-010-DISTINCT-REFUSAL-V1";
          step.diagnostic.detail =
              "query DISTINCT execution counters changed";
          return step;
        }
        step.result_handle_id = node.physical_node_id;
        step.input_row_count = input_batch.rows.size();
        step.rows_examined = input_batch.rows.size();
        step.output_row_count = distinct_result.output_batch.rows.size();
        step.materialized_output_batch =
            std::move(distinct_result.output_batch);
        step.mga_statement_context =
            std::move(distinct_result.mga_statement_context);
        return step;
      };
  return registration;
}

exec::CanonicalPhysicalExecutorRegistration MakeLiveLimitRegistration(
    std::string implementation_id,
    std::string capability_uuid,
    const std::uint64_t row_limit,
    const std::uint64_t row_offset,
    const bool fetch_first_rows_only,
    const std::size_t maximum_input_row_count,
    api::EngineRequestContext mga_context,
    const api::EngineRequestContext* borrowed_mga_context,
    const exec::CanonicalExecutionMgaAuthority* borrowed_mga_authority) {
  const bool strict_dispatcher_memory =
      borrowed_mga_context != nullptr && borrowed_mga_authority != nullptr;
  const std::uint64_t registration_retained_bytes =
      strict_dispatcher_memory
          ? sizeof(api::EngineRequestContext) + 8 * sizeof(void*) + 512
          : 0;
  exec::CanonicalPhysicalExecutorRegistration registration;
  registration.node_kind = exec::PhysicalNodeKind::kLimit;
  registration.implementation_id = std::move(implementation_id);
  registration.executor_capability_uuid = std::move(capability_uuid);
  registration.executor_capability_abi_version = 1;
  registration.engine_owned = true;
  registration.accepts_optimizer_publication_v2 = true;
  registration.honors_dispatcher_memory_limit_v1 = strict_dispatcher_memory;
  registration.retained_live_memory_bytes_v1 = registration_retained_bytes;
  registration.execute =
      [row_limit, row_offset, fetch_first_rows_only,
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
        if (node.input_physical_node_ids.size() != 1 ||
            inputs.size() != 1 ||
            inputs.front().physical_node_id !=
                node.input_physical_node_ids.front() ||
            !inputs.front().materialized_output_batch.has_value() ||
            inputs.front().materialized_output_batch->rows.size() >
                maximum_input_row_count) {
          step.diagnostic.ok = false;
          step.diagnostic.diagnostic_code =
              "QOW-DIAG-RELATIONAL-LIVE-LIMIT-INPUT-V1";
          step.diagnostic.detail =
              "LIMIT/FETCH did not receive its bounded typed input batch";
          return step;
        }
        const auto& input_batch = *inputs.front().materialized_output_batch;
        const exec::TypedPhysicalNodeDag* execution_dag = &dag;
        std::optional<exec::TypedPhysicalNodeDag> scoped_execution_dag;
        std::size_t callback_memory_bound = 0;
        if (strict_dispatcher_memory) {
          scoped_execution_dag.emplace();
          std::string scope_detail;
          if (!BuildStrictUnaryOperatorLocalPhysicalDag(
                  dag, node, input_batch, 1, 64 * 1024,
                  &*scoped_execution_dag, &callback_memory_bound,
                  &scope_detail)) {
            step.diagnostic.ok = false;
            step.diagnostic.diagnostic_code =
                "SBLR.PLAN_TREE.RESOURCE_LIMIT";
            step.diagnostic.detail = "object-backed LIMIT " + scope_detail;
            return step;
          }
          execution_dag = &*scoped_execution_dag;
        } else if (node.physical_node_id != dag.root_physical_node_id) {
          scoped_execution_dag.emplace();
          std::string scope_detail;
          if (!BuildOperatorLocalPhysicalDag(
                  dag,
                  node.physical_node_id,
                  &*scoped_execution_dag,
                  &scope_detail)) {
            step.diagnostic.ok = false;
            step.diagnostic.diagnostic_code =
                "QOW-DIAG-RELATIONAL-LIVE-LIMIT-INPUT-V1";
            step.diagnostic.detail =
                "LIMIT/FETCH execution view is unresolved";
            return step;
          }
          execution_dag = &*scoped_execution_dag;
        }
        exec::CanonicalDescriptorLimitResult limited;
        if (fetch_first_rows_only) {
          exec::CanonicalDescriptorFetchProfileRequest fetch_request;
          fetch_request.selected_physical_node_id = node.physical_node_id;
          fetch_request.form =
              exec::CanonicalFetchTopProfileForm::fetch_first_rows_only;
          fetch_request.row_count = row_limit;
          fetch_request.offset = row_offset;
          fetch_request.row_count_is_bound = true;
          fetch_request.mga_authority = strict_dispatcher_memory
                                            ? *borrowed_mga_authority
                                            : BuildCanonicalExecutionMgaAuthority(
                                                  mga_context, *execution_dag);
          auto fetched = exec::ExecuteCanonicalDescriptorFetchProfile(
              fetch_request, *execution_dag, input_batch);
          limited.diagnostic = std::move(fetched.diagnostic);
          limited.output_batch = std::move(fetched.output_batch);
          limited.selected_plan_uuid =
              std::move(fetched.selected_plan_uuid);
          limited.executed_physical_node_id =
              fetched.executed_physical_node_id;
          limited.causal_counter_id = fetched.causal_counter_id;
          limited.mga_statement_context =
              std::move(fetched.mga_statement_context);
        } else {
          exec::CanonicalDescriptorLimitRequest limit_request;
          limit_request.selected_physical_node_id = node.physical_node_id;
          limit_request.limit = row_limit;
          limit_request.offset = row_offset;
          limit_request.mga_authority = strict_dispatcher_memory
                                            ? *borrowed_mga_authority
                                            : BuildCanonicalExecutionMgaAuthority(
                                                  mga_context, *execution_dag);
          limited = exec::ExecuteCanonicalDescriptorLimit(
              limit_request, *execution_dag, input_batch);
        }
        if (!limited.diagnostic.ok) {
          step.diagnostic = std::move(limited.diagnostic);
          return step;
        }
        const auto& expected_statement_context =
            strict_dispatcher_memory
                ? borrowed_mga_authority->statement_context
                : execution_dag->mga_statement_context;
        const auto maximum_output_row_count =
            row_offset >= input_batch.rows.size()
                ? std::size_t{0}
                : std::min<std::uint64_t>(
                      row_limit,
                      static_cast<std::uint64_t>(input_batch.rows.size()) -
                          row_offset);
        if (!CanonicalOperatorExecutionReceiptMatches(
                limited, *execution_dag, node, expected_statement_context) ||
            limited.output_batch.rows.size() > maximum_output_row_count) {
          step.diagnostic.ok = false;
          step.diagnostic.diagnostic_code =
              "QOW-DIAG-RELATIONAL-LIVE-LIMIT-EXECUTION-V1";
          step.diagnostic.detail =
              "LIMIT/FETCH execution receipt changed";
          return step;
        }
        step.result_handle_id = node.physical_node_id;
        step.input_row_count = input_batch.rows.size();
        step.rows_examined = input_batch.rows.size();
        step.output_row_count = limited.output_batch.rows.size();
        step.materialized_output_batch = std::move(limited.output_batch);
        step.mga_statement_context =
            std::move(limited.mga_statement_context);
        return step;
      };
  return registration;
}

exec::CanonicalPhysicalExecutorRegistration MakeLiveNonrecursiveCteRegistration(
    std::string implementation_id,
    std::string capability_uuid,
    const std::size_t maximum_input_row_count,
    api::EngineRequestContext mga_context,
    const api::EngineRequestContext* borrowed_mga_context,
    const exec::CanonicalExecutionMgaAuthority* borrowed_mga_authority,
    std::vector<LiveNonrecursiveCteRuntimeNodeConfiguration>
        runtime_node_configurations) {
  const bool strict_dispatcher_memory =
      borrowed_mga_context != nullptr && borrowed_mga_authority != nullptr;
  std::uint64_t registration_retained_bytes =
      strict_dispatcher_memory
          ? sizeof(api::EngineRequestContext) +
                sizeof(std::vector<LiveNonrecursiveCteRuntimeNodeConfiguration>) +
                8 * sizeof(void*) + 512
          : 0;
  std::uint64_t configuration_bytes = 0;
  if (!strict_dispatcher_memory ||
      !CheckedMultiply(runtime_node_configurations.capacity(),
                       sizeof(LiveNonrecursiveCteRuntimeNodeConfiguration),
                       &configuration_bytes) ||
      !CheckedAdd(registration_retained_bytes, configuration_bytes,
                  &registration_retained_bytes)) {
    registration_retained_bytes = 0;
  }
  exec::CanonicalPhysicalExecutorRegistration registration;
  const bool inline_cte =
      implementation_id == "cte.bound.inline.typed.v1";
  const bool materialized_cte =
      implementation_id == "cte.bound.materialize.typed.v1";
  registration.node_kind = exec::PhysicalNodeKind::kCte;
  registration.implementation_id = std::move(implementation_id);
  registration.executor_capability_uuid = std::move(capability_uuid);
  registration.executor_capability_abi_version = 1;
  registration.engine_owned = true;
  registration.accepts_optimizer_publication_v2 = true;
  registration.honors_dispatcher_memory_limit_v1 = strict_dispatcher_memory;
  registration.retained_live_memory_bytes_v1 =
      registration_retained_bytes;
  registration.execute =
      [inline_cte, materialized_cte, maximum_input_row_count,
       mga_context = std::move(mga_context), borrowed_mga_authority,
       strict_dispatcher_memory,
       runtime_node_configurations = std::move(runtime_node_configurations)](
          const exec::TypedPhysicalNodeDag& dag,
          const exec::PhysicalNodeRecord& node,
          const std::vector<exec::CanonicalPhysicalDispatchInput>& inputs) {
        exec::CanonicalPhysicalDispatchStepResult step;
        step.executed_physical_node_id = node.physical_node_id;
        step.causal_counter_id = node.causal_counter_id;
        const LiveNonrecursiveCteRuntimeNodeConfiguration*
            runtime_node_configuration = nullptr;
        for (const auto& candidate : runtime_node_configurations) {
          if (candidate.relational_node_id != node.relational_node_id) continue;
          if (runtime_node_configuration != nullptr) {
            step.diagnostic.ok = false;
            step.diagnostic.diagnostic_code =
                "QOW-DIAG-RELATIONAL-LIVE-CTE-INPUT-V1";
            step.diagnostic.detail =
                "nonrecursive CTE has duplicate logical-node configuration";
            return step;
          }
          runtime_node_configuration = &candidate;
        }
        if (!runtime_node_configurations.empty() &&
            runtime_node_configuration == nullptr) {
          step.diagnostic.ok = false;
          step.diagnostic.diagnostic_code =
              "QOW-DIAG-RELATIONAL-LIVE-CTE-INPUT-V1";
          step.diagnostic.detail =
              "nonrecursive CTE logical-node configuration is absent";
          return step;
        }
        if (inputs.size() != 1 ||
            node.input_physical_node_ids.size() != 1 ||
            inputs.front().physical_node_id !=
                node.input_physical_node_ids.front() ||
            !inputs.front().materialized_output_batch.has_value() ||
            inputs.front().materialized_output_batch->rows.size() >
                maximum_input_row_count ||
            node.output_descriptor_ids != inputs.front().output_descriptor_ids) {
          step.diagnostic.ok = false;
          step.diagnostic.diagnostic_code =
              "QOW-DIAG-RELATIONAL-LIVE-CTE-INPUT-V1";
          step.diagnostic.detail =
              "nonrecursive CTE did not receive its exact typed input";
          return step;
        }
        const auto& input_batch =
            *inputs.front().materialized_output_batch;
        const exec::TypedPhysicalNodeDag* execution_dag = &dag;
        std::optional<exec::TypedPhysicalNodeDag> scoped_execution_dag;
        std::size_t callback_memory_bound = 0;
        if (strict_dispatcher_memory) {
          scoped_execution_dag.emplace();
          std::string scope_detail;
          if (!BuildStrictUnaryOperatorLocalPhysicalDag(
                  dag, node, input_batch, materialized_cte ? 2 : 1,
                  64 * 1024, &*scoped_execution_dag,
                  &callback_memory_bound, &scope_detail)) {
            step.diagnostic.ok = false;
            step.diagnostic.diagnostic_code =
                "SBLR.PLAN_TREE.RESOURCE_LIMIT";
            step.diagnostic.detail =
                "nonrecursive CTE " + scope_detail;
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
                "QOW-DIAG-RELATIONAL-LIVE-CTE-INPUT-V1";
            step.diagnostic.detail =
                "nonrecursive CTE execution view is unresolved";
            return step;
          }
          execution_dag = &*scoped_execution_dag;
        }
        const auto execution_node = std::ranges::find_if(
            execution_dag->nodes, [&](const auto& candidate) {
              return candidate.physical_node_id == node.physical_node_id;
            });
        std::uint64_t input_memory_bytes = 0;
        if ((!inline_cte && !materialized_cte) ||
            execution_node == execution_dag->nodes.end() ||
            !RuntimeMaterializedBatchMemoryBytes(
                input_batch, &input_memory_bytes) ||
            execution_node->memory_bytes_required == 0 ||
            execution_node->memory_bytes_required >
                execution_dag->memory_budget_bytes ||
            execution_node->memory_bytes_required >
                static_cast<std::uint64_t>(
                    std::numeric_limits<std::size_t>::max())) {
          step.diagnostic.ok = false;
          step.diagnostic.diagnostic_code =
              "SBLR.PLAN_TREE.RESOURCE_LIMIT";
          step.diagnostic.detail =
              "nonrecursive CTE memory grant or runtime payload accounting "
              "is invalid";
          return step;
        }
        auto remaining_memory_bytes = execution_node->memory_bytes_required;
        const auto charge = [&](const std::uint64_t bytes) {
          if (bytes > remaining_memory_bytes) return false;
          remaining_memory_bytes -= bytes;
          return true;
        };
        if (!charge(input_memory_bytes) || !charge(input_memory_bytes) ||
            (materialized_cte && !charge(input_memory_bytes))) {
          step.diagnostic.ok = false;
          step.diagnostic.diagnostic_code =
              "SBLR.PLAN_TREE.RESOURCE_LIMIT";
          step.diagnostic.detail =
              "nonrecursive CTE materialization exceeds the selected node "
              "memory grant";
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
        const auto before =
            exec::RevalidateCanonicalExecutionMgaAuthority(
                *active_authority, *execution_dag);
        if (!before.ok) {
          step.diagnostic = before;
          return step;
        }
        exec::DescriptorBatch output = input_batch;
        const auto validated = exec::ValidateCanonicalDescriptorBatch(
            output, node.output_descriptor_ids);
        if (!validated.ok) {
          step.diagnostic = validated;
          return step;
        }
        const auto after =
            exec::RevalidateCanonicalExecutionMgaAuthority(
                *active_authority, *execution_dag);
        if (!after.ok) {
          step.diagnostic = after;
          return step;
        }
        step.selected_plan_uuid = execution_dag->selected_plan_uuid;
        step.output_descriptor_ids = node.output_descriptor_ids;
        step.authority.engine_mga_snapshot_bound = true;
        step.result_handle_id = node.physical_node_id;
        step.input_row_count = output.rows.size();
        step.rows_examined = output.rows.size();
        step.output_row_count = output.rows.size();
        step.materialized_output_batch = std::move(output);
        step.mga_statement_context = active_authority->statement_context;
        return step;
      };
  return registration;
}

exec::CanonicalPhysicalExecutorRegistration MakeLiveCountStarRegistration(
    exec::ExecutorColumnDescriptor result_column,
    std::string capability_uuid,
    const std::size_t maximum_input_row_count,
    api::EngineRequestContext mga_context,
    const api::EngineRequestContext* borrowed_mga_context,
    const exec::CanonicalExecutionMgaAuthority* borrowed_mga_authority) {
  const bool strict_dispatcher_memory =
      borrowed_mga_context != nullptr && borrowed_mga_authority != nullptr;
  std::uint64_t registration_retained_bytes =
      strict_dispatcher_memory
          ? sizeof(exec::ExecutorColumnDescriptor) +
                sizeof(api::EngineRequestContext) + 8 * sizeof(void*) + 512
          : 0;
  const auto account_string = [&](const std::string& value) {
    return value.capacity() != std::numeric_limits<std::size_t>::max() &&
           CheckedAdd(registration_retained_bytes,
                      static_cast<std::uint64_t>(value.capacity()) + 1,
                      &registration_retained_bytes);
  };
  if (!strict_dispatcher_memory ||
      !account_string(result_column.stable_name) ||
      !account_string(result_column.descriptor.descriptor_uuid.canonical) ||
      !account_string(result_column.descriptor.descriptor_kind) ||
      !account_string(result_column.descriptor.canonical_type_name) ||
      !account_string(result_column.descriptor.encoded_descriptor)) {
    registration_retained_bytes = 0;
  }
  exec::CanonicalPhysicalExecutorRegistration registration;
  registration.node_kind = exec::PhysicalNodeKind::kAggregate;
  registration.implementation_id = "aggregate.count-star.v1";
  registration.executor_capability_uuid = std::move(capability_uuid);
  registration.executor_capability_abi_version = 1;
  registration.engine_owned = true;
  registration.accepts_optimizer_publication_v2 = true;
  registration.publishes_runtime_observation_v1 = true;
  registration.honors_dispatcher_memory_limit_v1 =
      strict_dispatcher_memory;
  registration.retained_live_memory_bytes_v1 =
      registration_retained_bytes;
  registration.execute =
      [result_column = std::move(result_column), maximum_input_row_count,
       mga_context = std::move(mga_context), borrowed_mga_authority,
       strict_dispatcher_memory](
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
        const bool streaming_count =
            inputs.size() == 1 &&
            inputs.front().exact_count_star_cardinality.has_value();
        if (inputs.size() != 1 ||
            node.input_physical_node_ids.size() != 1 ||
            inputs.front().physical_node_id !=
                node.input_physical_node_ids.front() ||
            !inputs.front().materialized_output_batch.has_value() ||
            (!streaming_count &&
             inputs.front().materialized_output_batch->rows.size() >
                 maximum_input_row_count)) {
          step.diagnostic.ok = false;
          step.diagnostic.diagnostic_code =
              "QOW-DIAG-RELATIONAL-LIVE-AGGREGATE-INPUT-V1";
          step.diagnostic.detail =
              "COUNT(*) did not receive its bounded typed input batch";
          return step;
        }
        const auto& input_batch = *inputs.front().materialized_output_batch;
        const auto* exact_cardinality =
            streaming_count
                ? &*inputs.front().exact_count_star_cardinality
                : nullptr;
        if (exact_cardinality != nullptr &&
            (!input_batch.rows.empty() ||
             exact_cardinality->visible_row_count >
                 static_cast<std::uint64_t>(
                     std::numeric_limits<std::int64_t>::max()))) {
          step.diagnostic.ok = false;
          step.diagnostic.diagnostic_code =
              "QOW-DIAG-RELATIONAL-LIVE-AGGREGATE-INPUT-V1";
          step.diagnostic.detail =
              "COUNT(*) streaming cardinality carrier is invalid";
          return step;
        }
        const exec::TypedPhysicalNodeDag* execution_dag = &dag;
        std::optional<exec::TypedPhysicalNodeDag> scoped_execution_dag;
        std::size_t callback_memory_bound = 0;
        exec::CanonicalDescriptorCountRequest aggregate_request;
        if (strict_dispatcher_memory) {
          scoped_execution_dag.emplace();
          std::string scope_detail;
          if (!BuildStrictUnaryOperatorLocalPhysicalDag(
                  dag, node, input_batch, 1, sizeof(std::int64_t),
                  &*scoped_execution_dag, &callback_memory_bound,
                  &scope_detail)) {
            step.diagnostic.ok = false;
            step.diagnostic.diagnostic_code =
                "SBLR.PLAN_TREE.RESOURCE_LIMIT";
            step.diagnostic.detail = "COUNT(*) " + scope_detail;
            return step;
          }
          execution_dag = &*scoped_execution_dag;
        } else if (node.physical_node_id != dag.root_physical_node_id) {
          scoped_execution_dag.emplace();
          std::string scope_detail;
          if (!BuildOperatorLocalPhysicalDag(
                  dag, node.physical_node_id,
                  &*scoped_execution_dag, &scope_detail)) {
            step.diagnostic.ok = false;
            step.diagnostic.diagnostic_code =
                "QOW-DIAG-RELATIONAL-LIVE-AGGREGATE-INPUT-V1";
            step.diagnostic.detail =
                "COUNT(*) execution view is unresolved";
            return step;
          }
          execution_dag = &*scoped_execution_dag;
        }
        aggregate_request.selected_physical_node_id = node.physical_node_id;
        aggregate_request.mga_authority =
            strict_dispatcher_memory
                ? *borrowed_mga_authority
                : BuildCanonicalExecutionMgaAuthority(
                      mga_context, *execution_dag);
        auto aggregate_result =
            exact_cardinality == nullptr
                ? exec::ExecuteCanonicalDescriptorCountStar(
                      aggregate_request, *execution_dag, input_batch,
                      result_column)
                : exec::ExecuteCanonicalDescriptorCountStarExactCardinality(
                      aggregate_request, *execution_dag, input_batch,
                      result_column,
                      exact_cardinality->visible_row_count);
        if (!aggregate_result.diagnostic.ok) {
          step.diagnostic = std::move(aggregate_result.diagnostic);
          return step;
        }
        if (!CanonicalOperatorExecutionReceiptMatches(
                aggregate_result, *execution_dag, node,
                aggregate_request.mga_authority.statement_context)) {
          step.diagnostic.ok = false;
          step.diagnostic.diagnostic_code =
              "QOW-DIAG-RELATIONAL-LIVE-AGGREGATE-EXECUTION-V1";
          step.diagnostic.detail =
              "COUNT(*) execution receipt changed";
          return step;
        }
        std::uint64_t input_memory_bytes = 0;
        std::uint64_t output_memory_bytes = 0;
        std::uint64_t expected_peak_memory_bytes = 0;
        if (!RuntimeMaterializedBatchMemoryBytes(input_batch,
                                                 &input_memory_bytes) ||
            !RuntimeMaterializedBatchMemoryBytes(
                aggregate_result.output_batch, &output_memory_bytes) ||
            !CheckedAdd(input_memory_bytes, sizeof(std::int64_t),
                        &expected_peak_memory_bytes) ||
            !CheckedAdd(expected_peak_memory_bytes, output_memory_bytes,
                        &expected_peak_memory_bytes) ||
            aggregate_result.input_payload_bytes != input_memory_bytes ||
            aggregate_result.state_bytes != sizeof(std::int64_t) ||
            aggregate_result.output_payload_bytes != output_memory_bytes ||
            aggregate_result.current_memory_bytes != output_memory_bytes ||
            aggregate_result.current_memory_bytes >
                aggregate_result.peak_memory_bytes ||
            aggregate_result.peak_memory_bytes !=
                expected_peak_memory_bytes ||
            aggregate_result.peak_memory_bytes > node.memory_bytes_required) {
          step.diagnostic.ok = false;
          step.diagnostic.diagnostic_code = "QOW-DIAG-OPT-017-REFUSAL-V1";
          step.diagnostic.detail =
              "COUNT(*) runtime phase memory receipt is inconsistent";
          return step;
        }
        step.result_handle_id = node.physical_node_id;
        step.input_row_count =
            exact_cardinality == nullptr
                ? input_batch.rows.size()
                : exact_cardinality->visible_row_count;
        step.rows_examined = step.input_row_count;
        step.output_row_count = aggregate_result.output_batch.rows.size();
        step.materialized_output_batch =
            std::move(aggregate_result.output_batch);
        PublishRuntimeMemoryObservation(
            &step, output_memory_bytes, aggregate_result.peak_memory_bytes);
        step.mga_statement_context =
            aggregate_request.mga_authority.statement_context;
        return step;
      };
  return registration;
}

}  // namespace scratchbird::engine::sblr

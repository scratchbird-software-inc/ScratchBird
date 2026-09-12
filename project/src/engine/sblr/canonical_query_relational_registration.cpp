// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "canonical_query_relational_registration.hpp"

#include "canonical_query_descriptor_support.hpp"
#include "canonical_query_physical_registration.hpp"
#include "canonical_query_runtime_memory_support.hpp"
#include "canonical_query_scalar_support.hpp"
#include "canonical_relational_expression.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <optional>
#include <ranges>
#include <string>
#include <utility>
#include <vector>

namespace scratchbird::engine::sblr {

namespace api = scratchbird::engine::internal_api;

namespace {

enum class LiveCancellationProbeState : std::uint8_t {
  kRunning = 0,
  kCancelled,
  kProbeFailed,
};

bool PollLiveCancellationProbe(
    const std::function<bool()>& cancellation_requested,
    LiveCancellationProbeState* state) noexcept {
  if (state == nullptr) return true;
  if (*state != LiveCancellationProbeState::kRunning) return true;
  try {
    if (!cancellation_requested || !cancellation_requested()) return false;
    *state = LiveCancellationProbeState::kCancelled;
  } catch (...) {
    *state = LiveCancellationProbeState::kProbeFailed;
  }
  return true;
}

bool EvaluateCanonicalQuantifiedSubqueryTruth(
    const exec::CanonicalQuantifiedSubqueryRequest& request,
    const exec::DescriptorBatch& input_batch,
    api::EngineSqlTruthValue* expected_truth) {
  if (expected_truth == nullptr || input_batch.columns.size() != 1 ||
      request.maximum_comparison_count < input_batch.rows.size()) {
    return false;
  }
  const bool any = request.quantifier ==
                   exec::CanonicalQuantifiedSubqueryQuantifier::kAny;
  const bool all = request.quantifier ==
                   exec::CanonicalQuantifiedSubqueryQuantifier::kAll;
  if (!any && !all) return false;
  auto operation = api::EngineCanonicalExpressionOperation::equal;
  switch (request.comparison_operator) {
    case api::EngineComparisonPredicateOperator::equal:
      break;
    case api::EngineComparisonPredicateOperator::not_equal:
      operation = api::EngineCanonicalExpressionOperation::not_equal;
      break;
    case api::EngineComparisonPredicateOperator::less_than:
      operation = api::EngineCanonicalExpressionOperation::less_than;
      break;
    case api::EngineComparisonPredicateOperator::less_than_or_equal:
      operation = api::EngineCanonicalExpressionOperation::less_than_or_equal;
      break;
    case api::EngineComparisonPredicateOperator::greater_than:
      operation = api::EngineCanonicalExpressionOperation::greater_than;
      break;
    case api::EngineComparisonPredicateOperator::greater_than_or_equal:
      operation =
          api::EngineCanonicalExpressionOperation::greater_than_or_equal;
      break;
    default:
      return false;
  }
  if ((request.comparison_authority_engine_owned &&
       request.precomputed_comparisons.size() != input_batch.rows.size()) ||
      (!request.comparison_authority_engine_owned &&
       !request.precomputed_comparisons.empty())) {
    return false;
  }
  auto truth = any ? api::EngineSqlTruthValue::false_value
                   : api::EngineSqlTruthValue::true_value;
  for (std::size_t row_index = 0; row_index < input_batch.rows.size();
       ++row_index) {
    const auto& row = input_batch.rows[row_index];
    if (row.values.size() != 1) return false;
    api::EngineCanonicalExpressionEvaluationRequest expression_request;
    expression_request.consumer =
        api::EngineCanonicalExpressionConsumer::subquery;
    expression_request.operation = operation;
    expression_request.left_value = request.left_value;
    expression_request.right_value = row.values.front();
    expression_request.result_descriptor =
        request.result_column.descriptor;
    if (request.comparison_authority_engine_owned) {
      const auto& comparison = request.precomputed_comparisons[row_index];
      const bool null_comparison = request.left_value.isSqlNull() ||
                                   row.values.front().isSqlNull();
      if (comparison.has_value() == null_comparison ||
          (comparison.has_value() &&
           (*comparison < -1 || *comparison > 1))) {
        return false;
      }
      expression_request.precomputed_comparison = comparison;
    }
    api::EngineCanonicalExpressionEvaluationResult expression_result;
    std::string detail;
    if (!api::QowEvaluateCanonicalTypedExpressionV1(
            expression_request, &expression_result, &detail)) {
      return false;
    }
    if (any) {
      if (expression_result.truth == api::EngineSqlTruthValue::true_value) {
        truth = api::EngineSqlTruthValue::true_value;
      } else if (expression_result.truth == api::EngineSqlTruthValue::unknown &&
                 truth == api::EngineSqlTruthValue::false_value) {
        truth = api::EngineSqlTruthValue::unknown;
      }
    } else {
      if (expression_result.truth == api::EngineSqlTruthValue::false_value) {
        truth = api::EngineSqlTruthValue::false_value;
      } else if (expression_result.truth == api::EngineSqlTruthValue::unknown &&
                 truth == api::EngineSqlTruthValue::true_value) {
        truth = api::EngineSqlTruthValue::unknown;
      }
    }
  }
  *expected_truth = truth;
  return true;
}

}  // namespace

// SEARCH_KEY: SB_ENGINE_CANONICAL_QUERY_RELATIONAL_REGISTRATION_AUTHORITY
exec::CanonicalPhysicalExecutorRegistration MakeLiveHeapProjectRegistration(
    std::vector<std::size_t> projected_columns,
    std::string capability_uuid,
    const std::size_t maximum_input_row_count,
    api::EngineRequestContext mga_context,
    const api::EngineRequestContext* borrowed_mga_context,
    const exec::CanonicalExecutionMgaAuthority* borrowed_mga_authority,
    std::vector<LiveProjectRuntimeNodeConfiguration>
        runtime_node_configurations) {
  const bool strict_dispatcher_memory =
      borrowed_mga_context != nullptr && borrowed_mga_authority != nullptr;
  std::uint64_t registration_retained_bytes =
      sizeof(std::vector<std::size_t>) +
      sizeof(api::EngineRequestContext) +
      sizeof(std::vector<LiveProjectRuntimeNodeConfiguration>) +
      8 * sizeof(void*) + 512;
  std::uint64_t projected_column_bytes = 0;
  if (!strict_dispatcher_memory ||
      !CheckedMultiply(projected_columns.capacity(), sizeof(std::size_t),
                       &projected_column_bytes) ||
      !CheckedAdd(registration_retained_bytes, projected_column_bytes,
                  &registration_retained_bytes)) {
    registration_retained_bytes = 0;
  }
  std::uint64_t configuration_bytes = 0;
  if (registration_retained_bytes == 0 ||
      !CheckedMultiply(runtime_node_configurations.capacity(),
                       sizeof(LiveProjectRuntimeNodeConfiguration),
                       &configuration_bytes) ||
      !CheckedAdd(registration_retained_bytes, configuration_bytes,
                  &registration_retained_bytes)) {
    registration_retained_bytes = 0;
  }
  for (const auto& configuration : runtime_node_configurations) {
    std::uint64_t nested_bytes = 0;
    if (registration_retained_bytes == 0 ||
        !CheckedMultiply(configuration.projected_columns.capacity(),
                         sizeof(std::size_t), &nested_bytes) ||
        !CheckedAdd(registration_retained_bytes, nested_bytes,
                    &registration_retained_bytes)) {
      registration_retained_bytes = 0;
      break;
    }
  }
  exec::CanonicalPhysicalExecutorRegistration registration;
  registration.node_kind = exec::PhysicalNodeKind::kProject;
  registration.implementation_id = "project.descriptor-direct.v1";
  registration.executor_capability_uuid = std::move(capability_uuid);
  registration.executor_capability_abi_version = 1;
  registration.engine_owned = true;
  registration.accepts_optimizer_publication_v2 = true;
  registration.honors_dispatcher_memory_limit_v1 = strict_dispatcher_memory;
  registration.retained_live_memory_bytes_v1 =
      registration_retained_bytes;
  registration.execute =
      [projected_columns = std::move(projected_columns),
       maximum_input_row_count, mga_context = std::move(mga_context),
       borrowed_mga_authority, strict_dispatcher_memory,
       runtime_node_configurations = std::move(runtime_node_configurations)](
          const exec::TypedPhysicalNodeDag& dag,
          const exec::PhysicalNodeRecord& node,
          const std::vector<exec::CanonicalPhysicalDispatchInput>& inputs) {
        exec::CanonicalPhysicalDispatchStepResult step;
        step.executed_physical_node_id = node.physical_node_id;
        step.causal_counter_id = node.causal_counter_id;
        const LiveProjectRuntimeNodeConfiguration* runtime_node_configuration =
            nullptr;
        for (const auto& candidate : runtime_node_configurations) {
          if (candidate.relational_node_id != node.relational_node_id) continue;
          if (runtime_node_configuration != nullptr) {
            step.diagnostic.ok = false;
            step.diagnostic.diagnostic_code =
                "QOW-DIAG-PACKET7-OBJECT-HEAP-PROJECT-INPUT-V1";
            step.diagnostic.detail =
                "object-backed PROJECT has duplicate logical-node configuration";
            return step;
          }
          runtime_node_configuration = &candidate;
        }
        if (!runtime_node_configurations.empty() &&
            runtime_node_configuration == nullptr) {
          step.diagnostic.ok = false;
          step.diagnostic.diagnostic_code =
              "QOW-DIAG-PACKET7-OBJECT-HEAP-PROJECT-INPUT-V1";
          step.diagnostic.detail =
              "object-backed PROJECT logical-node configuration is absent";
          return step;
        }
        const auto& active_projected_columns =
            runtime_node_configuration == nullptr
                ? projected_columns
                : runtime_node_configuration->projected_columns;
        if (inputs.size() != 1 ||
            node.input_physical_node_ids.size() != 1 ||
            inputs.front().physical_node_id !=
                node.input_physical_node_ids.front() ||
            !inputs.front().materialized_output_batch.has_value() ||
            inputs.front().materialized_output_batch->rows.size() >
                maximum_input_row_count) {
          step.diagnostic.ok = false;
          step.diagnostic.diagnostic_code =
              "QOW-DIAG-PACKET7-OBJECT-HEAP-PROJECT-INPUT-V1";
          step.diagnostic.detail =
              "object-backed PROJECT input exceeds its admitted bound";
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
                  dag, node, input_batch, 1, 64 * 1024,
                  &*scoped_execution_dag, &callback_memory_bound,
                  &scope_detail)) {
            step.diagnostic.ok = false;
            step.diagnostic.diagnostic_code =
                "SBLR.PLAN_TREE.RESOURCE_LIMIT";
            step.diagnostic.detail =
                "object-backed PROJECT " + scope_detail;
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
                "QOW-DIAG-PACKET7-OBJECT-HEAP-PROJECT-INPUT-V1";
            step.diagnostic.detail =
                "object-backed PROJECT execution view is unresolved";
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
        exec::CanonicalDescriptorProjectionRequest project_request;
        project_request.selected_physical_node_id = node.physical_node_id;
        if (strict_dispatcher_memory) {
          project_request.borrowed_mga_authority = borrowed_mga_authority;
        } else {
          project_request.mga_authority =
              BuildCanonicalExecutionMgaAuthority(mga_context,
                                                  *execution_dag);
        }
        auto project_result = exec::ExecuteCanonicalDescriptorProjection(
            project_request, *execution_dag, input_batch,
            active_projected_columns);
        if (!project_result.diagnostic.ok) {
          step.diagnostic = std::move(project_result.diagnostic);
          return step;
        }
        const auto& active_mga_authority =
            project_request.borrowed_mga_authority == nullptr
                ? project_request.mga_authority
                : *project_request.borrowed_mga_authority;
        if (!CanonicalOperatorExecutionReceiptMatches(
                project_result, *execution_dag, node,
                active_mga_authority.statement_context)) {
          step.diagnostic.ok = false;
          step.diagnostic.diagnostic_code =
              "QOW-DIAG-PACKET7-OBJECT-HEAP-PROJECT-EXECUTION-V1";
          step.diagnostic.detail =
              "object-backed PROJECT execution receipt changed";
          return step;
        }
        step.selected_plan_uuid =
            std::move(project_result.selected_plan_uuid);
        step.output_descriptor_ids = node.output_descriptor_ids;
        step.authority.engine_mga_snapshot_bound = true;
        step.result_handle_id = node.physical_node_id;
        step.input_row_count = input_batch.rows.size();
        step.rows_examined = input_batch.rows.size();
        step.output_row_count = project_result.output_batch.rows.size();
        step.materialized_output_batch =
            std::move(project_result.output_batch);
        step.mga_statement_context =
            std::move(project_result.mga_statement_context);
        return step;
      };
  return registration;
}

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
      !account_string(result_column.descriptor.descriptor_uuid) ||
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

exec::CanonicalPhysicalExecutorRegistration MakeLiveSortRegistration(
    std::vector<exec::CanonicalDescriptorOrderTerm> order_terms,
    std::string deterministic_tie_evidence_uuid,
    std::string capability_uuid,
    const std::size_t maximum_input_row_count,
    const std::size_t maximum_pair_comparisons,
    api::EngineRequestContext mga_context,
    const api::EngineRequestContext* borrowed_mga_context,
    const exec::CanonicalExecutionMgaAuthority* borrowed_mga_authority) {
  const bool strict_dispatcher_memory =
      borrowed_mga_context != nullptr && borrowed_mga_authority != nullptr;
  std::uint64_t registration_retained_bytes =
      strict_dispatcher_memory
          ? sizeof(std::vector<exec::CanonicalDescriptorOrderTerm>) +
                sizeof(std::string) + sizeof(api::EngineRequestContext) +
                8 * sizeof(void*) + 512
          : 0;
  const auto account_string = [&](const std::string& value) {
    return value.capacity() != std::numeric_limits<std::size_t>::max() &&
           CheckedAdd(registration_retained_bytes,
                      static_cast<std::uint64_t>(value.capacity()) + 1,
                      &registration_retained_bytes);
  };
  std::uint64_t order_term_bytes = 0;
  if (!strict_dispatcher_memory ||
      !CheckedMultiply(order_terms.capacity(),
                       sizeof(exec::CanonicalDescriptorOrderTerm),
                       &order_term_bytes) ||
      !CheckedAdd(registration_retained_bytes, order_term_bytes,
                  &registration_retained_bytes) ||
      !account_string(deterministic_tie_evidence_uuid)) {
    registration_retained_bytes = 0;
  }
  for (const auto& term : order_terms) {
    if (registration_retained_bytes == 0 ||
        !account_string(term.collation_uuid) ||
        !account_string(term.text_seed.seed_pack_name) ||
        !account_string(term.text_seed.seed_pack_version) ||
        !account_string(term.text_seed.charset_name) ||
        !account_string(term.text_seed.collation_name) ||
        !account_string(term.timezone_seed.seed_pack_name) ||
        !account_string(term.timezone_seed.seed_pack_version) ||
        !account_string(term.timezone_seed.content_hash)) {
      registration_retained_bytes = 0;
      break;
    }
    std::uint64_t timezone_name_slots = 0;
    if (!CheckedMultiply(term.timezone_seed.timezone_names.capacity(),
                         sizeof(std::string), &timezone_name_slots) ||
        !CheckedAdd(registration_retained_bytes, timezone_name_slots,
                    &registration_retained_bytes)) {
      registration_retained_bytes = 0;
      break;
    }
    for (const auto& name : term.timezone_seed.timezone_names) {
      if (!account_string(name)) {
        registration_retained_bytes = 0;
        break;
      }
    }
  }
  exec::CanonicalPhysicalExecutorRegistration registration;
  registration.node_kind = exec::PhysicalNodeKind::kSort;
  registration.implementation_id = "sort.typed.terms.v1";
  registration.executor_capability_uuid = std::move(capability_uuid);
  registration.executor_capability_abi_version = 1;
  registration.engine_owned = true;
  registration.accepts_optimizer_publication_v2 = true;
  registration.honors_dispatcher_memory_limit_v1 = strict_dispatcher_memory;
  registration.retained_live_memory_bytes_v1 =
      registration_retained_bytes;
  registration.execute =
      [order_terms = std::move(order_terms),
       deterministic_tie_evidence_uuid =
           std::move(deterministic_tie_evidence_uuid),
       maximum_input_row_count, maximum_pair_comparisons,
       mga_context = std::move(mga_context), borrowed_mga_context,
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
        const auto* cancellation_policy =
            FindLiveCancellationPolicy(dag);
        const auto& active_mga_context =
            borrowed_mga_context == nullptr ? mga_context
                                            : *borrowed_mga_context;
        const void* cancellation_context =
            active_mga_context.query_cancellation_requested
                ? &active_mga_context.query_cancellation_requested
                : nullptr;
        const exec::DescriptorCancellationProbe cancellation_probe =
            cancellation_context == nullptr
                ? nullptr
                : &InvokeLiveSortCancellationProbe;
        if (cancellation_policy == nullptr) {
          step.diagnostic.ok = false;
          step.diagnostic.diagnostic_code =
              "QOW-DIAG-RELATIONAL-LIVE-SORT-CANCELLATION-POLICY-V1";
          step.diagnostic.detail =
              "SORT requires one exact cancellation policy evidence row";
          return step;
        }
        if (inputs.size() != 1 ||
            node.input_physical_node_ids.size() != 1 ||
            inputs.front().physical_node_id !=
                node.input_physical_node_ids.front() ||
            !inputs.front().materialized_output_batch.has_value() ||
            inputs.front().materialized_output_batch->rows.size() >
                maximum_input_row_count) {
          step.diagnostic.ok = false;
          step.diagnostic.diagnostic_code =
              "QOW-DIAG-RELATIONAL-LIVE-SORT-INPUT-V1";
          step.diagnostic.detail =
              "SORT did not receive its bounded typed input batch";
          return step;
        }
        const auto& input_batch = *inputs.front().materialized_output_batch;
        const exec::TypedPhysicalNodeDag* execution_dag = &dag;
        std::optional<exec::TypedPhysicalNodeDag> scoped_execution_dag;
        std::size_t callback_memory_bound = 0;
        if (strict_dispatcher_memory) {
          std::uint64_t order_memory_bytes = 0;
          std::uint64_t pair_memory_bytes = 0;
          std::uint64_t actual_pair_bound = 0;
          if (!CheckedMultiply(input_batch.rows.size(),
                               sizeof(std::size_t),
                               &order_memory_bytes) ||
              !CheckedMultiply(input_batch.rows.size(),
                               input_batch.rows.size(),
                               &actual_pair_bound) ||
              actual_pair_bound > maximum_pair_comparisons ||
              !CheckedMultiply(actual_pair_bound,
                               sizeof(std::uint8_t),
                               &pair_memory_bytes) ||
              !CheckedAdd(order_memory_bytes, pair_memory_bytes,
                          &order_memory_bytes)) {
            step.diagnostic.ok = false;
            step.diagnostic.diagnostic_code =
                "SBLR.PLAN_TREE.RESOURCE_LIMIT";
            step.diagnostic.detail =
                "SORT retained order workspace overflows";
            return step;
          }
          scoped_execution_dag.emplace();
          std::string scope_detail;
          if (!BuildStrictUnaryOperatorLocalPhysicalDag(
                  dag, node, input_batch, 1, order_memory_bytes,
                  &*scoped_execution_dag, &callback_memory_bound,
                  &scope_detail)) {
            step.diagnostic.ok = false;
            step.diagnostic.diagnostic_code =
                "SBLR.PLAN_TREE.RESOURCE_LIMIT";
            step.diagnostic.detail = "SORT " + scope_detail;
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
                "QOW-DIAG-RELATIONAL-LIVE-SORT-INPUT-V1";
            step.diagnostic.detail = "SORT execution view is unresolved";
            return step;
          }
          execution_dag = &*scoped_execution_dag;
        }
        exec::CanonicalDescriptorSortRequest sort_request;
        sort_request.selected_physical_node_id = node.physical_node_id;
        sort_request.maximum_pair_comparisons = maximum_pair_comparisons;
        sort_request.mga_authority = strict_dispatcher_memory
                                         ? *borrowed_mga_authority
                                         : BuildCanonicalExecutionMgaAuthority(
                                               mga_context, *execution_dag);
        auto sort_result = exec::ExecuteCanonicalDescriptorSort(
            sort_request, *execution_dag, input_batch, order_terms,
            deterministic_tie_evidence_uuid, cancellation_probe,
            cancellation_context);
        if (!sort_result.diagnostic.ok) {
          BindLiveCancellationFailure(std::move(sort_result.diagnostic),
                                      cancellation_policy, &step);
          return step;
        }
        if (!CanonicalOperatorExecutionReceiptMatches(
                sort_result, *execution_dag, node,
                sort_request.mga_authority.statement_context) ||
            sort_result.output_batch.rows.size() !=
                input_batch.rows.size()) {
          step.diagnostic.ok = false;
          step.diagnostic.diagnostic_code =
              "QOW-DIAG-RELATIONAL-LIVE-SORT-EXECUTION-V1";
          step.diagnostic.detail = "SORT execution receipt changed";
          return step;
        }
        step.result_handle_id = node.physical_node_id;
        step.input_row_count = input_batch.rows.size();
        step.rows_examined = input_batch.rows.size();
        step.output_row_count = sort_result.output_batch.rows.size();
        step.materialized_output_batch = std::move(sort_result.output_batch);
        step.mga_statement_context =
            std::move(sort_result.mga_statement_context);
        return step;
      };
  return registration;
}

exec::CanonicalPhysicalExecutorRegistration
MakeLiveMatchRecognizeRegistration(
    std::string capability_uuid,
    const std::size_t maximum_partition_rows,
    const std::size_t maximum_active_states,
    const std::size_t maximum_output_rows,
    const api::EngineRequestContext* borrowed_mga_context,
    const exec::CanonicalExecutionMgaAuthority* borrowed_mga_authority) {
  const bool strict_dispatcher_memory =
      borrowed_mga_context != nullptr && borrowed_mga_authority != nullptr;
  const std::uint64_t registration_retained_bytes =
      strict_dispatcher_memory ? 8 * sizeof(void*) + 512 : 0;
  exec::CanonicalPhysicalExecutorRegistration registration;
  registration.node_kind = exec::PhysicalNodeKind::kMatchRecognize;
  registration.implementation_id =
      "match-recognize.partition-order.a-plus.v1";
  registration.executor_capability_uuid = std::move(capability_uuid);
  registration.executor_capability_abi_version = 1;
  registration.engine_owned = true;
  registration.accepts_optimizer_publication_v2 = true;
  registration.honors_dispatcher_memory_limit_v1 =
      strict_dispatcher_memory;
  registration.retained_live_memory_bytes_v1 =
      registration_retained_bytes;
  registration.execute =
      [maximum_partition_rows, maximum_active_states, maximum_output_rows,
       borrowed_mga_context, borrowed_mga_authority,
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
        const auto* cancellation_policy = FindLiveCancellationPolicy(dag);
        if (!strict_dispatcher_memory || cancellation_policy == nullptr ||
            maximum_partition_rows == 0 || maximum_active_states < 1 ||
            maximum_output_rows == 0 ||
            node.input_physical_node_ids.size() != 1 || inputs.size() != 1 ||
            inputs.front().physical_node_id !=
                node.input_physical_node_ids.front() ||
            !inputs.front().materialized_output_batch.has_value()) {
          step.diagnostic.ok = false;
          step.diagnostic.diagnostic_code =
              "QOW-DIAG-QRY-004-MATCH-RECOGNIZE-INPUT-V1";
          step.diagnostic.detail =
              "MATCH_RECOGNIZE did not receive its exact bounded input";
          return step;
        }
        const auto before = exec::RevalidateCanonicalExecutionMgaAuthority(
            *borrowed_mga_authority, dag);
        if (!before.ok) {
          step.diagnostic = before;
          return step;
        }
        step.mga_statement_context = borrowed_mga_authority->statement_context;
        const auto& input_batch =
            *inputs.front().materialized_output_batch;
        const auto canonical_input = exec::ValidateCanonicalDescriptorBatch(
            input_batch, node.output_descriptor_ids);
        const auto value_input = exec::ValidateDescriptorBatch(input_batch);
        if (!canonical_input.ok || !value_input.ok ||
            input_batch.columns.size() != 1 ||
            input_batch.rows.size() > maximum_output_rows ||
            input_batch.rows.size() > maximum_partition_rows) {
          step.diagnostic.ok = false;
          step.diagnostic.diagnostic_code =
              "QOW-DIAG-QRY-004-MATCH-RECOGNIZE-INPUT-V1";
          step.diagnostic.detail =
              !canonical_input.ok
                  ? canonical_input.detail
                  : (!value_input.ok
                         ? value_input.detail
                         : "MATCH_RECOGNIZE input exceeds its row-pattern budget");
          return step;
        }
        std::uint64_t order_workspace_bytes = 0;
        if (!CheckedMultiply(
                input_batch.rows.size(),
                sizeof(std::pair<std::int64_t, std::size_t>),
                &order_workspace_bytes) ||
            !CheckedAdd(order_workspace_bytes, 64 * 1024,
                        &order_workspace_bytes)) {
          step.diagnostic.ok = false;
          step.diagnostic.diagnostic_code = "SBLR.PLAN_TREE.RESOURCE_LIMIT";
          step.diagnostic.detail =
              "MATCH_RECOGNIZE ordering workspace overflows";
          return step;
        }
        exec::TypedPhysicalNodeDag operator_dag;
        std::size_t callback_memory_bound = 0;
        std::string scope_detail;
        if (!BuildStrictUnaryOperatorLocalPhysicalDag(
                dag, node, input_batch, 1, order_workspace_bytes,
                &operator_dag, &callback_memory_bound, &scope_detail)) {
          step.diagnostic.ok = false;
          step.diagnostic.diagnostic_code = "SBLR.PLAN_TREE.RESOURCE_LIMIT";
          step.diagnostic.detail = "MATCH_RECOGNIZE " + scope_detail;
          return step;
        }
        LiveCancellationProbeState cancellation_state =
            LiveCancellationProbeState::kRunning;
        const auto poll_cancellation = [&]() {
          return PollLiveCancellationProbe(
              borrowed_mga_context->query_cancellation_requested,
              &cancellation_state);
        };
        std::vector<std::pair<std::int64_t, std::size_t>> order;
        order.reserve(input_batch.rows.size());
        for (std::size_t ordinal = 0; ordinal < input_batch.rows.size();
             ++ordinal) {
          if (poll_cancellation()) break;
          if (input_batch.rows[ordinal].values.size() != 1) {
            step.diagnostic.ok = false;
            step.diagnostic.diagnostic_code =
                "QOW-DIAG-QRY-004-MATCH-RECOGNIZE-INPUT-V1";
            step.diagnostic.detail =
                "MATCH_RECOGNIZE input row width changed";
            return step;
          }
          std::int64_t key = 0;
          std::string decode_detail;
          if (!DecodeCanonicalInt64Scalar(
                  input_batch.rows[ordinal].values.front(), &key,
                  &decode_detail)) {
            step.diagnostic.ok = false;
            step.diagnostic.diagnostic_code =
                "QOW-DIAG-QRY-004-MATCH-RECOGNIZE-INPUT-V1";
            step.diagnostic.detail = decode_detail;
            return step;
          }
          order.emplace_back(key, ordinal);
        }
        std::ranges::sort(order, [](const auto& left, const auto& right) {
          return left.first < right.first ||
                 (left.first == right.first && left.second < right.second);
        });
        poll_cancellation();
        if (cancellation_state != LiveCancellationProbeState::kRunning) {
          exec::DescriptorRuntimeDiagnostic diagnostic;
          diagnostic.ok = false;
          diagnostic.diagnostic_code =
              cancellation_state == LiveCancellationProbeState::kCancelled
                  ? "SB_MODEL_EXECUTION_CANCELLED_V1"
                  : "SB_MODEL_COORDINATOR_LEG_FAILED_V1";
          diagnostic.detail =
              cancellation_state == LiveCancellationProbeState::kCancelled
                  ? "MATCH_RECOGNIZE execution was cancelled"
                  : "MATCH_RECOGNIZE cancellation probe failed";
          BindLiveCancellationFailure(std::move(diagnostic),
                                      cancellation_policy, &step);
          return step;
        }
        exec::DescriptorBatch output_batch;
        output_batch.columns = input_batch.columns;
        output_batch.rows.reserve(order.size());
        std::size_t partition_count = 0;
        std::size_t partition_row_count = 0;
        std::optional<std::int64_t> previous_key;
        for (const auto& [key, ordinal] : order) {
          if (!previous_key.has_value() || *previous_key != key) {
            ++partition_count;
            partition_row_count = 0;
            previous_key = key;
          }
          ++partition_row_count;
          if (partition_row_count > maximum_partition_rows ||
              maximum_active_states < 1 ||
              output_batch.rows.size() >= maximum_output_rows) {
            step.diagnostic.ok = false;
            step.diagnostic.diagnostic_code =
                "SBLR.PLAN_TREE.RESOURCE_LIMIT";
            step.diagnostic.detail =
                "MATCH_RECOGNIZE pattern budget was exceeded";
            return step;
          }
          output_batch.rows.push_back(input_batch.rows[ordinal]);
        }
        const auto canonical_output =
            exec::ValidateCanonicalDescriptorBatch(
                output_batch, node.output_descriptor_ids);
        const auto value_output = exec::ValidateDescriptorBatch(output_batch);
        std::uint64_t output_memory_bytes = 0;
        if (!canonical_output.ok || !value_output.ok ||
            output_batch.rows.size() != input_batch.rows.size() ||
            partition_count > maximum_output_rows ||
            !RuntimeMaterializedBatchMemoryBytes(
                output_batch, &output_memory_bytes) ||
            output_memory_bytes == 0 ||
            output_memory_bytes > callback_memory_bound) {
          step.diagnostic.ok = false;
          step.diagnostic.diagnostic_code =
              "QOW-DIAG-QRY-004-MATCH-RECOGNIZE-OUTPUT-V1";
          step.diagnostic.detail =
              !canonical_output.ok
                  ? canonical_output.detail
                  : (!value_output.ok
                         ? value_output.detail
                         : "MATCH_RECOGNIZE output receipt is invalid");
          return step;
        }
        const auto after = exec::RevalidateCanonicalExecutionMgaAuthority(
            *borrowed_mga_authority, operator_dag);
        if (!after.ok) {
          step.diagnostic = after;
          return step;
        }
        step.result_handle_id = node.physical_node_id;
        step.input_row_count = input_batch.rows.size();
        step.rows_examined = input_batch.rows.size();
        step.output_row_count = output_batch.rows.size();
        step.materialized_output_batch = std::move(output_batch);
        step.mga_statement_context = borrowed_mga_authority->statement_context;
        step.data_access_observation_known = true;
        step.data_access_observed = false;
        return step;
      };
  return registration;
}

exec::CanonicalPhysicalExecutorRegistration
MakeLiveCardinalitySubqueryRegistration(
    LiveCardinalitySubqueryRegistrationProfile profile,
    std::string capability_uuid,
    const std::size_t maximum_input_row_count,
    api::EngineRequestContext mga_context) {
  exec::CanonicalPhysicalExecutorRegistration registration;
  registration.node_kind = exec::PhysicalNodeKind::kSubquery;
  registration.implementation_id = profile.implementation_id;
  registration.executor_capability_uuid = std::move(capability_uuid);
  registration.executor_capability_abi_version = 1;
  registration.engine_owned = true;
  registration.accepts_optimizer_publication_v2 = true;
  registration.execute =
      [profile = std::move(profile), maximum_input_row_count,
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
              "QOW-DIAG-RELATIONAL-LIVE-SUBQUERY-INPUT-V1";
          step.diagnostic.detail =
              "cardinality subquery did not receive its bounded typed input";
          return step;
        }
        const auto& input_batch =
            *inputs.front().materialized_output_batch;
        exec::CanonicalTableSubqueryRequest table_request;
        table_request.selected_physical_node_id = node.physical_node_id;
        table_request.maximum_materialized_row_count =
            std::max<std::size_t>(1, maximum_input_row_count);
        table_request.mga_authority =
            BuildCanonicalExecutionMgaAuthority(mga_context, dag);

        exec::DescriptorBatch output;
        exec::PhysicalMgaStatementContext result_mga;
        if (profile.kind == LiveCardinalitySubqueryKind::kScalar) {
          exec::CanonicalScalarSubqueryRequest scalar_request;
          scalar_request.table_request = std::move(table_request);
          scalar_request.value_expression_descriptor_id =
              profile.result_columns.front().descriptor_id;
          scalar_request.result_column = profile.result_columns.front();
          auto scalar = exec::ExecuteCanonicalScalarSubquery(
              scalar_request, dag, node.physical_node_id, input_batch);
          if (!scalar.diagnostic.ok) {
            step.diagnostic = std::move(scalar.diagnostic);
            return step;
          }
          if (!CanonicalOperatorExecutionReceiptMatches(
                  scalar, dag, node,
                  scalar_request.table_request.mga_authority
                      .statement_context) ||
              scalar.source_row_count != input_batch.rows.size() ||
              scalar.source_row_count > 1 ||
              scalar.output_batch.rows.size() != 1) {
            step.diagnostic.ok = false;
            step.diagnostic.diagnostic_code =
                "QOW-DIAG-RELATIONAL-LIVE-SUBQUERY-EXECUTION-V1";
            step.diagnostic.detail =
                "scalar subquery execution receipt changed";
            return step;
          }
          const auto output_validation =
              exec::ValidateCanonicalDescriptorBatch(
                  scalar.output_batch, node.output_descriptor_ids);
          const bool scalar_value_preserved =
              scalar.source_row_count == 0
                  ? std::ranges::all_of(
                        scalar.output_batch.rows.front().values,
                        [](const auto& value) { return value.isSqlNull(); })
                  : CanonicalQueryDescriptorTuplePayloadExactlyEqual(
                        scalar.output_batch.rows.front(),
                        input_batch.rows.front());
          if (!output_validation.ok || !scalar_value_preserved) {
            step.diagnostic.ok = false;
            step.diagnostic.diagnostic_code =
                "QOW-DIAG-RELATIONAL-LIVE-SUBQUERY-EXECUTION-V1";
            step.diagnostic.detail =
                "scalar subquery result value changed";
            return step;
          }
          output = std::move(scalar.output_batch);
          result_mga = std::move(scalar.mga_statement_context);
        } else {
          exec::CanonicalRowSubqueryRequest row_request;
          row_request.table_request = std::move(table_request);
          row_request.result_columns = profile.result_columns;
          for (const auto& column : profile.result_columns) {
            row_request.row_expression_descriptor_ids.push_back(
                column.descriptor_id);
          }
          auto row = exec::ExecuteCanonicalRowSubquery(
              row_request, dag, node.physical_node_id, input_batch);
          if (!row.diagnostic.ok) {
            step.diagnostic = std::move(row.diagnostic);
            return step;
          }
          if (!CanonicalOperatorExecutionReceiptMatches(
                  row, dag, node,
                  row_request.table_request.mga_authority
                      .statement_context) ||
              row.source_row_count != input_batch.rows.size() ||
              row.source_row_count > 1 ||
              row.output_batch.rows.size() != 1) {
            step.diagnostic.ok = false;
            step.diagnostic.diagnostic_code =
                "QOW-DIAG-RELATIONAL-LIVE-SUBQUERY-EXECUTION-V1";
            step.diagnostic.detail =
                "row subquery execution receipt changed";
            return step;
          }
          const auto output_validation =
              exec::ValidateCanonicalDescriptorBatch(
                  row.output_batch, node.output_descriptor_ids);
          const bool row_value_preserved =
              row.source_row_count == 0
                  ? std::ranges::all_of(
                        row.output_batch.rows.front().values,
                        [](const auto& value) { return value.isSqlNull(); })
                  : CanonicalQueryDescriptorTuplePayloadExactlyEqual(
                        row.output_batch.rows.front(),
                        input_batch.rows.front());
          if (!output_validation.ok || !row_value_preserved) {
            step.diagnostic.ok = false;
            step.diagnostic.diagnostic_code =
                "QOW-DIAG-RELATIONAL-LIVE-SUBQUERY-EXECUTION-V1";
            step.diagnostic.detail =
                "row subquery result value changed";
            return step;
          }
          output = std::move(row.output_batch);
          result_mga = std::move(row.mga_statement_context);
        }
        step.result_handle_id = node.physical_node_id;
        step.input_row_count = input_batch.rows.size();
        step.rows_examined = step.input_row_count;
        step.output_row_count = output.rows.size();
        step.materialized_output_batch = std::move(output);
        step.mga_statement_context = std::move(result_mga);
        return step;
      };
  return registration;
}

exec::CanonicalPhysicalExecutorRegistration
MakeLivePredicateSubqueryRegistration(
    LivePredicateSubqueryRegistrationProfile prepared,
    std::string capability_uuid,
    const std::size_t maximum_input_row_count,
    CanonicalRelationalExpressionRuntimeServices expression_services,
    api::EngineRequestContext mga_context) {
  exec::CanonicalPhysicalExecutorRegistration registration;
  registration.node_kind = exec::PhysicalNodeKind::kSubquery;
  registration.implementation_id = prepared.implementation_id;
  registration.executor_capability_uuid = std::move(capability_uuid);
  registration.executor_capability_abi_version = 1;
  registration.engine_owned = true;
  registration.accepts_optimizer_publication_v2 = true;
  registration.execute =
      [prepared = std::move(prepared), maximum_input_row_count,
       expression_services = std::move(expression_services),
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
              "QOW-DIAG-RELATIONAL-LIVE-SUBQUERY-INPUT-V1";
          step.diagnostic.detail =
              "predicate subquery did not receive its bounded typed input";
          return step;
        }
        exec::TypedPhysicalNodeDag operator_dag;
        std::string detail;
        if (!BuildOperatorLocalPhysicalDag(
                dag, node.physical_node_id, &operator_dag, &detail)) {
          step.diagnostic.ok = false;
          step.diagnostic.diagnostic_code =
              "QOW-DIAG-RELATIONAL-LIVE-SUBQUERY-INPUT-V1";
          step.diagnostic.detail = std::move(detail);
          return step;
        }

        // The canonical predicate executors deliberately evaluate a table
        // subquery first. Its selected subquery node therefore carries the
        // input table descriptor handles, while this enclosing physical node
        // continues to publish the separately bound boolean result handle.
        auto selected = std::ranges::find_if(
            operator_dag.nodes, [&](const auto& candidate) {
              return candidate.physical_node_id == node.physical_node_id;
            });
        if (selected == operator_dag.nodes.end() ||
            selected->input_physical_node_ids.size() != 1) {
          step.diagnostic.ok = false;
          step.diagnostic.diagnostic_code =
              "QOW-DIAG-RELATIONAL-LIVE-SUBQUERY-INPUT-V1";
          step.diagnostic.detail =
              "predicate subquery local root is unresolved";
          return step;
        }
        const auto input_id = selected->input_physical_node_ids.front();
        const auto table_input = std::ranges::find_if(
            operator_dag.nodes, [&](const auto& candidate) {
              return candidate.physical_node_id == input_id;
            });
        if (table_input == operator_dag.nodes.end()) {
          step.diagnostic.ok = false;
          step.diagnostic.diagnostic_code =
              "QOW-DIAG-RELATIONAL-LIVE-SUBQUERY-INPUT-V1";
          step.diagnostic.detail =
              "predicate subquery table input is unresolved";
          return step;
        }
        selected->output_descriptor_ids =
            table_input->output_descriptor_ids;

        exec::CanonicalTableSubqueryRequest table_request;
        table_request.selected_physical_node_id = node.physical_node_id;
        table_request.maximum_materialized_row_count =
            std::max<std::size_t>(1, maximum_input_row_count);
        table_request.mga_authority =
            BuildCanonicalExecutionMgaAuthority(mga_context, operator_dag);

        const auto& input_batch =
            *inputs.front().materialized_output_batch;
        exec::DescriptorBatch output;
        exec::PhysicalMgaStatementContext result_mga;
        std::size_t comparison_count = 0;
        if (prepared.kind == LivePredicateSubqueryKind::kExists) {
          exec::CanonicalExistsSubqueryRequest exists_request;
          exists_request.table_request = std::move(table_request);
          exists_request.exists_expression_descriptor_id =
              prepared.result_column.descriptor_id;
          exists_request.result_column = prepared.result_column;
          auto exists = exec::ExecuteCanonicalExistsSubquery(
              exists_request, operator_dag, node.physical_node_id,
              input_batch);
          if (!exists.diagnostic.ok) {
            step.diagnostic = std::move(exists.diagnostic);
            return step;
          }
          if (!CanonicalOperatorExecutionReceiptMatches(
                  exists, operator_dag, node,
                  exists_request.table_request.mga_authority
                      .statement_context) ||
              exists.source_row_count != input_batch.rows.size() ||
              exists.output_batch.rows.size() != 1 ||
              exists.exists != !input_batch.rows.empty()) {
            step.diagnostic.ok = false;
            step.diagnostic.diagnostic_code =
                "QOW-DIAG-RELATIONAL-LIVE-SUBQUERY-EXECUTION-V1";
            step.diagnostic.detail =
                "EXISTS subquery execution receipt changed";
            return step;
          }
          const auto output_validation =
              exec::ValidateCanonicalDescriptorBatch(
                  exists.output_batch, node.output_descriptor_ids);
          api::EngineSqlTruthValue output_truth =
              api::EngineSqlTruthValue::unspecified;
          std::string output_truth_detail;
          const bool output_truth_bound =
              output_validation.ok &&
              exists.output_batch.rows.front().values.size() == 1 &&
              api::QowCanonicalTruthFromTypedValueV1(
                  exists.output_batch.rows.front().values.front(),
                  &output_truth, &output_truth_detail);
          if (!output_truth_bound ||
              output_truth !=
                  (exists.exists
                       ? api::EngineSqlTruthValue::true_value
                       : api::EngineSqlTruthValue::false_value)) {
            step.diagnostic.ok = false;
            step.diagnostic.diagnostic_code =
                "QOW-DIAG-RELATIONAL-LIVE-SUBQUERY-EXECUTION-V1";
            step.diagnostic.detail =
                "EXISTS subquery result truth changed";
            return step;
          }
          output = std::move(exists.output_batch);
          result_mga = std::move(exists.mga_statement_context);
        } else {
          exec::CanonicalQuantifiedSubqueryRequest quantified_request;
          if (prepared.comparison_authority_required) {
            const auto& authority_input =
                *inputs.front().materialized_output_batch;
            const auto authority_validation =
                exec::ValidateCanonicalDescriptorBatch(
                    authority_input, table_input->output_descriptor_ids);
            if (!authority_validation.ok) {
              step.diagnostic = authority_validation;
              return step;
            }
            quantified_request.comparison_authority_engine_owned = true;
            quantified_request.precomputed_comparisons.reserve(
                authority_input.rows.size());
            for (const auto& row : authority_input.rows) {
              std::optional<int> comparison;
              if (!BindCanonicalRelationalComparisonAuthorityV1(
                      prepared.left_value, row.values.front(),
                      expression_services, &comparison, &detail)) {
                step.diagnostic.ok = false;
                step.diagnostic.diagnostic_code =
                    "QOW-DIAG-RELATIONAL-LIVE-SUBQUERY-"
                    "COMPARISON-AUTHORITY-V1";
                step.diagnostic.detail = std::move(detail);
                return step;
              }
              quantified_request.precomputed_comparisons.push_back(
                  comparison);
            }
          }
          quantified_request.table_request = std::move(table_request);
          quantified_request.left_operand_column =
              prepared.left_operand_column;
          quantified_request.left_value = prepared.left_value;
          quantified_request.right_expression_descriptor_id =
              prepared.right_expression_descriptor_id;
          quantified_request.comparison_operator =
              prepared.comparison_operator;
          quantified_request.quantifier = prepared.quantifier;
          quantified_request.result_expression_descriptor_id =
              prepared.result_column.descriptor_id;
          quantified_request.result_column = prepared.result_column;
          quantified_request.maximum_comparison_count =
              std::max<std::size_t>(1, maximum_input_row_count);
          auto quantified = exec::ExecuteCanonicalQuantifiedSubquery(
              quantified_request, operator_dag, node.physical_node_id,
              input_batch);
          if (!quantified.diagnostic.ok) {
            step.diagnostic = std::move(quantified.diagnostic);
            return step;
          }
          if (!CanonicalOperatorExecutionReceiptMatches(
                  quantified, operator_dag, node,
                  quantified_request.table_request.mga_authority
                      .statement_context) ||
              quantified.comparison_count != input_batch.rows.size() ||
              quantified.comparison_count >
                  quantified_request.maximum_comparison_count ||
              quantified.truth_value ==
                  api::EngineSqlTruthValue::unspecified ||
              quantified.output_batch.rows.size() != 1) {
            step.diagnostic.ok = false;
            step.diagnostic.diagnostic_code =
                "QOW-DIAG-RELATIONAL-LIVE-SUBQUERY-EXECUTION-V1";
            step.diagnostic.detail =
                "quantified subquery execution receipt changed";
            return step;
          }
          api::EngineSqlTruthValue expected_truth =
              api::EngineSqlTruthValue::unspecified;
          if (!EvaluateCanonicalQuantifiedSubqueryTruth(
                  quantified_request, input_batch, &expected_truth) ||
              quantified.truth_value != expected_truth) {
            step.diagnostic.ok = false;
            step.diagnostic.diagnostic_code =
                "QOW-DIAG-RELATIONAL-LIVE-SUBQUERY-EXECUTION-V1";
            step.diagnostic.detail =
                "quantified subquery causal truth changed";
            return step;
          }
          const auto output_validation =
              exec::ValidateCanonicalDescriptorBatch(
                  quantified.output_batch, node.output_descriptor_ids);
          api::EngineSqlTruthValue output_truth =
              api::EngineSqlTruthValue::unspecified;
          std::string output_truth_detail;
          const bool output_truth_bound =
              output_validation.ok &&
              quantified.output_batch.rows.front().values.size() == 1 &&
              api::QowCanonicalTruthFromTypedValueV1(
                  quantified.output_batch.rows.front().values.front(),
                  &output_truth, &output_truth_detail);
          if (!output_truth_bound || output_truth != expected_truth) {
            step.diagnostic.ok = false;
            step.diagnostic.diagnostic_code =
                "QOW-DIAG-RELATIONAL-LIVE-SUBQUERY-EXECUTION-V1";
            step.diagnostic.detail =
                "quantified subquery result truth changed";
            return step;
          }
          output = std::move(quantified.output_batch);
          result_mga = std::move(quantified.mga_statement_context);
          comparison_count = quantified.comparison_count;
        }
        if (input_batch.rows.size() >
            std::numeric_limits<std::size_t>::max() - comparison_count) {
          step.diagnostic.ok = false;
          step.diagnostic.diagnostic_code =
              "QOW-DIAG-RELATIONAL-LIVE-SUBQUERY-EXECUTION-V1";
          step.diagnostic.detail =
              "predicate subquery observation count overflowed";
          return step;
        }
        step.result_handle_id = node.physical_node_id;
        step.input_row_count = input_batch.rows.size();
        step.rows_examined = step.input_row_count + comparison_count;
        step.output_row_count = output.rows.size();
        step.materialized_output_batch = std::move(output);
        step.mga_statement_context = std::move(result_mga);
        return step;
      };
  return registration;
}

}  // namespace scratchbird::engine::sblr

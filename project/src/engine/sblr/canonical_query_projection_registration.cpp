// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "canonical_query_projection_registration.hpp"

#include "canonical_query_physical_registration.hpp"
#include "canonical_query_runtime_memory_support.hpp"

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

// SEARCH_KEY: SB_ENGINE_CANONICAL_QUERY_PROJECTION_REGISTRATION_AUTHORITY
bool MaterializeExpressionProjectBatch(
    const api::TypedRelationalDag& dag,
    const std::vector<LiveProjectExpressionRegistration>& expressions,
    const std::vector<exec::ExecutorColumnDescriptor>& output_columns,
    const exec::DescriptorBatch& input_batch,
    const CanonicalRelationalExpressionRuntimeServices& expression_services,
    exec::DescriptorBatch* output_batch,
    std::string* detail,
    const std::uint64_t maximum_output_bytes,
    std::uint64_t* actual_output_bytes,
    bool* resource_refused) {
  if (actual_output_bytes != nullptr) *actual_output_bytes = 0;
  if (resource_refused != nullptr) *resource_refused = false;
  if (output_batch == nullptr || detail == nullptr || expressions.empty() ||
      expressions.size() != output_columns.size()) {
    if (detail != nullptr) {
      *detail = "expression PROJECT materialization request is incomplete";
    }
    return false;
  }
  *output_batch = {};
  detail->clear();
  std::uint64_t materialized_bytes = 1;
  if (materialized_bytes > maximum_output_bytes) {
    *detail = "expression PROJECT output exceeds its materialization ceiling";
    if (resource_refused != nullptr) *resource_refused = true;
    return false;
  }
  output_batch->columns = output_columns;
  output_batch->rows.reserve(input_batch.rows.size());
  CanonicalRelationalExpressionRuntime runtime(dag, expression_services);
  for (const auto& input_row : input_batch.rows) {
    exec::DescriptorTuple output_row;
    output_row.values.reserve(expressions.size());
    for (const auto& expression : expressions) {
      api::EngineTypedValue value;
      if (!runtime.EvaluateForConsumer(
              expression.expression_id, expression.expected_type,
              expression.row_binding, input_row.values,
              api::EngineCanonicalExpressionConsumer::projection, &value,
              detail)) {
        *output_batch = {};
        return false;
      }
      std::uint64_t value_bytes = 0;
      if (!CheckedAdd(value.encoded_value.size(), value.binary_value.size(),
                      &value_bytes) ||
          !CheckedAdd(materialized_bytes, value_bytes,
                      &materialized_bytes) ||
          materialized_bytes > maximum_output_bytes) {
        *detail =
            "expression PROJECT value exceeds its materialization ceiling";
        *output_batch = {};
        if (resource_refused != nullptr) *resource_refused = true;
        return false;
      }
      output_row.values.push_back(std::move(value));
    }
    output_batch->rows.push_back(std::move(output_row));
  }
  std::vector<std::uint32_t> descriptor_ids;
  descriptor_ids.reserve(output_columns.size());
  for (const auto& column : output_columns) {
    descriptor_ids.push_back(column.descriptor_id);
  }
  const auto canonical =
      exec::ValidateCanonicalDescriptorBatch(*output_batch, descriptor_ids);
  const auto values = exec::ValidateDescriptorBatch(*output_batch);
  if (!canonical.ok || !values.ok) {
    *detail = !canonical.ok
                  ? canonical.diagnostic_code + ":" + canonical.detail
                  : values.diagnostic_code + ":" + values.detail;
    *output_batch = {};
    return false;
  }
  if (actual_output_bytes != nullptr) {
    *actual_output_bytes = materialized_bytes;
  }
  return true;
}

exec::CanonicalPhysicalExecutorRegistration MakeLiveProjectRegistration(
    LiveProjectRegistrationProfile profile,
    std::string implementation_id,
    std::string capability_uuid,
    const std::size_t expected_input_row_count,
    api::TypedRelationalDag relational_dag,
    CanonicalRelationalExpressionRuntimeServices expression_services,
    api::EngineRequestContext mga_context,
    const bool input_row_count_is_upper_bound,
    const api::TypedRelationalDag* borrowed_relational_dag,
    const api::EngineRequestContext* borrowed_mga_context,
    const exec::CanonicalExecutionMgaAuthority* borrowed_mga_authority) {
  const bool strict_dispatcher_memory =
      borrowed_relational_dag != nullptr && borrowed_mga_context != nullptr &&
      borrowed_mga_authority != nullptr;
  std::uint64_t registration_retained_bytes =
      strict_dispatcher_memory
          ? sizeof(std::vector<std::size_t>) +
                sizeof(std::vector<LiveProjectExpressionRegistration>) +
                sizeof(std::vector<exec::ExecutorColumnDescriptor>) +
                sizeof(CanonicalRelationalExpressionRuntimeServices) +
                sizeof(api::EngineRequestContext) + 12 * sizeof(void*) + 1024
          : 0;
  const auto account_array = [&](const std::size_t count,
                                 const std::size_t width) {
    std::uint64_t bytes = 0;
    return CheckedMultiply(count, width, &bytes) &&
           CheckedAdd(registration_retained_bytes, bytes,
                      &registration_retained_bytes);
  };
  const auto account_string = [&](const std::string& value) {
    return value.capacity() != std::numeric_limits<std::size_t>::max() &&
           CheckedAdd(registration_retained_bytes,
                      static_cast<std::uint64_t>(value.capacity()) + 1,
                      &registration_retained_bytes);
  };
  if (!strict_dispatcher_memory ||
      !account_array(profile.projected_columns.capacity(),
                     sizeof(std::size_t)) ||
      !account_array(profile.expressions.capacity(),
                     sizeof(LiveProjectExpressionRegistration)) ||
      !account_array(profile.expression_output_columns.capacity(),
                     sizeof(exec::ExecutorColumnDescriptor))) {
    registration_retained_bytes = 0;
  }
  for (const auto& expression : profile.expressions) {
    if (registration_retained_bytes == 0 ||
        !account_string(expression.expected_type) ||
        !account_array(expression.row_binding.row_descriptor_ids.capacity(),
                       sizeof(std::uint32_t)) ||
        !account_array(
            (expression.row_binding.row_nullable.capacity() + 63) / 64,
            sizeof(std::uint64_t)) ||
        !account_array(expression.row_binding.slots.capacity(),
                       sizeof(CanonicalRelationalExpressionRowSlotBinding))) {
      registration_retained_bytes = 0;
      break;
    }
  }
  for (const auto& column : profile.expression_output_columns) {
    if (registration_retained_bytes == 0 ||
        !account_string(column.stable_name) ||
        !account_string(column.descriptor.descriptor_uuid.canonical) ||
        !account_string(column.descriptor.descriptor_kind) ||
        !account_string(column.descriptor.canonical_type_name) ||
        !account_string(column.descriptor.encoded_descriptor)) {
      registration_retained_bytes = 0;
      break;
    }
  }
  exec::CanonicalPhysicalExecutorRegistration registration;
  registration.node_kind = exec::PhysicalNodeKind::kProject;
  registration.implementation_id = std::move(implementation_id);
  registration.executor_capability_uuid = std::move(capability_uuid);
  registration.executor_capability_abi_version = 1;
  registration.engine_owned = true;
  registration.accepts_optimizer_publication_v2 = true;
  registration.honors_dispatcher_memory_limit_v1 = strict_dispatcher_memory;
  registration.retained_live_memory_bytes_v1 = registration_retained_bytes;
  registration.execute =
      [projected_columns = std::move(profile.projected_columns),
       expression_projection = profile.expression_projection,
       expressions = std::move(profile.expressions),
       expression_output_columns =
           std::move(profile.expression_output_columns),
       relational_dag = strict_dispatcher_memory
                            ? api::TypedRelationalDag{}
                            : std::move(relational_dag),
       expression_services = std::move(expression_services),
       expected_input_row_count, input_row_count_is_upper_bound,
       mga_context = std::move(mga_context), borrowed_relational_dag,
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
            (input_row_count_is_upper_bound
                 ? inputs.front().materialized_output_batch->rows.size() >
                       expected_input_row_count
                 : inputs.front().materialized_output_batch->rows.size() !=
                       expected_input_row_count)) {
          step.diagnostic.ok = false;
          step.diagnostic.diagnostic_code =
              "QOW-DIAG-RELATIONAL-LIVE-PROJECT-INPUT-V1";
          step.diagnostic.detail =
              "PROJECT input cardinality differs from its selected cost";
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
            step.diagnostic.detail =
                "expression PROJECT " + std::move(scope_detail);
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
                "QOW-DIAG-RELATIONAL-LIVE-PROJECT-INPUT-V1";
            step.diagnostic.detail = "PROJECT execution view is unresolved";
            return step;
          }
          execution_dag = &*scoped_execution_dag;
        }
        const auto& active_relational_dag =
            strict_dispatcher_memory ? *borrowed_relational_dag
                                     : relational_dag;
        if (expression_projection) {
          const auto input_validation = exec::ValidateCanonicalDescriptorBatch(
              input_batch, inputs.front().output_descriptor_ids);
          if (!input_validation.ok) {
            step.diagnostic = input_validation;
            return step;
          }
          const auto mga_authority =
              strict_dispatcher_memory
                  ? *borrowed_mga_authority
                  : BuildCanonicalExecutionMgaAuthority(mga_context,
                                                         *execution_dag);
          const auto before = exec::RevalidateCanonicalExecutionMgaAuthority(
              mga_authority, *execution_dag);
          if (!before.ok) {
            step.diagnostic = before;
            return step;
          }
          const auto execution_node = std::ranges::find_if(
              execution_dag->nodes, [&](const auto& candidate) {
                return candidate.physical_node_id == node.physical_node_id;
              });
          std::uint64_t input_memory_bytes = 0;
          if (!RuntimeMaterializedBatchMemoryBytes(
                  input_batch, &input_memory_bytes) ||
              execution_node == execution_dag->nodes.end() ||
              execution_node->memory_bytes_required == 0 ||
              execution_node->memory_bytes_required >
                  execution_dag->memory_budget_bytes ||
              execution_node->memory_bytes_required >
                  static_cast<std::uint64_t>(
                      std::numeric_limits<std::size_t>::max()) ||
              input_memory_bytes >= execution_node->memory_bytes_required) {
            step.diagnostic.ok = false;
            step.diagnostic.diagnostic_code =
                "SBLR.PLAN_TREE.RESOURCE_LIMIT";
            step.diagnostic.detail =
                "expression PROJECT memory grant or input payload "
                "accounting is invalid";
            return step;
          }
          const auto maximum_output_bytes =
              execution_node->memory_bytes_required - input_memory_bytes;
          exec::DescriptorBatch output_batch;
          std::string expression_detail;
          std::uint64_t actual_output_bytes = 0;
          bool expression_resource_refused = false;
          if (!MaterializeExpressionProjectBatch(
                  active_relational_dag, expressions,
                  expression_output_columns, input_batch, expression_services,
                  &output_batch, &expression_detail, maximum_output_bytes,
                  &actual_output_bytes, &expression_resource_refused)) {
            step.diagnostic.ok = false;
            step.diagnostic.diagnostic_code =
                expression_resource_refused
                    ? "SBLR.PLAN_TREE.RESOURCE_LIMIT"
                    : "QOW-DIAG-RELATIONAL-LIVE-PROJECT-EXPRESSION-V1";
            step.diagnostic.detail = std::move(expression_detail);
            return step;
          }
          if (actual_output_bytes == 0 ||
              actual_output_bytes > maximum_output_bytes) {
            step.diagnostic.ok = false;
            step.diagnostic.diagnostic_code =
                "SBLR.PLAN_TREE.RESOURCE_LIMIT";
            step.diagnostic.detail =
                "expression PROJECT output exceeds the selected node "
                "memory grant";
            return step;
          }
          const auto output_validation = exec::ValidateCanonicalDescriptorBatch(
              output_batch, node.output_descriptor_ids);
          if (!output_validation.ok) {
            step.diagnostic = output_validation;
            return step;
          }
          const auto after = exec::RevalidateCanonicalExecutionMgaAuthority(
              mga_authority, *execution_dag);
          if (!after.ok) {
            step.diagnostic = after;
            return step;
          }
          step.result_handle_id = node.physical_node_id;
          step.input_row_count = input_batch.rows.size();
          step.rows_examined = input_batch.rows.size();
          step.output_row_count = output_batch.rows.size();
          step.materialized_output_batch = std::move(output_batch);
          step.mga_statement_context = mga_authority.statement_context;
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
            project_request, *execution_dag, input_batch, projected_columns);
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
              "QOW-DIAG-RELATIONAL-LIVE-PROJECT-EXECUTION-V1";
          step.diagnostic.detail = "PROJECT execution receipt changed";
          return step;
        }
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

}  // namespace scratchbird::engine::sblr

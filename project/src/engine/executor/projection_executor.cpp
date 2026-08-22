// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "descriptor_value_runtime.hpp"

#include <limits>
#include <string_view>
#include <utility>

namespace scratchbird::engine::executor {
namespace {

DescriptorRuntimeDiagnostic Refusal(std::string code,
                                    std::string detail = {},
                                    const std::size_t row = 0,
                                    const std::size_t column = 0) {
  DescriptorRuntimeDiagnostic diagnostic;
  diagnostic.ok = false;
  diagnostic.diagnostic_code = std::move(code);
  diagnostic.detail = std::move(detail);
  diagnostic.row_index = row;
  diagnostic.column_index = column;
  return diagnostic;
}

bool DescriptorBatchCarrierIsExactDefault(const DescriptorBatch& batch) {
  const DescriptorBatch empty;
  return batch.columns.empty() &&
         batch.columns.capacity() == empty.columns.capacity() &&
         batch.rows.empty() && batch.rows.capacity() == empty.rows.capacity();
}

bool ProjectionColumnCarrierIsExactDefault(
    const std::vector<std::size_t>& projected_columns) {
  const std::vector<std::size_t> empty;
  return projected_columns.empty() &&
         projected_columns.capacity() == empty.capacity();
}

bool CanonicalExecutionMgaAuthorityCarrierIsExactDefault(
    const CanonicalExecutionMgaAuthority& authority) {
  const CanonicalExecutionMgaAuthority empty;
  const auto exact_empty_string = [](const std::string& value,
                                     const std::string& baseline) {
    return value.empty() && value.capacity() == baseline.capacity();
  };
  const auto& context = authority.statement_context;
  const auto& empty_context = empty.statement_context;
  return authority.origin == empty.origin && !authority.resolve_current &&
         PhysicalMgaStatementContextEqual(context, empty_context) &&
         exact_empty_string(context.statement_uuid,
                            empty_context.statement_uuid) &&
         exact_empty_string(context.owning_transaction_uuid,
                            empty_context.owning_transaction_uuid) &&
         exact_empty_string(context.statement_snapshot_uuid,
                            empty_context.statement_snapshot_uuid) &&
         exact_empty_string(context.statement_metadata_snapshot_uuid,
                            empty_context.statement_metadata_snapshot_uuid) &&
         exact_empty_string(context.snapshot_kind,
                            empty_context.snapshot_kind) &&
         exact_empty_string(context.statement_timestamp,
                            empty_context.statement_timestamp) &&
         context.active_excluded_local_transaction_ids.empty() &&
         context.active_excluded_local_transaction_ids.capacity() ==
             empty_context.active_excluded_local_transaction_ids.capacity() &&
         context.in_doubt_excluded_local_transaction_ids.empty() &&
         context.in_doubt_excluded_local_transaction_ids.capacity() ==
             empty_context.in_doubt_excluded_local_transaction_ids.capacity();
}

bool IsCanonicalDescriptorProjectionImplementation(
    const std::string_view implementation_id) {
  return implementation_id == "project.typed.row.v1" ||
         implementation_id == "project.descriptor-direct.v1";
}

bool AddProjectionValueMemoryBytes(
    const internal_api::EngineTypedValue& value,
    std::uint64_t* memory_bytes) {
  if (memory_bytes == nullptr ||
      value.encoded_value.size() >
          std::numeric_limits<std::uint64_t>::max() - *memory_bytes) {
    return false;
  }
  *memory_bytes += value.encoded_value.size();
  if (value.binary_value.size() >
      std::numeric_limits<std::uint64_t>::max() - *memory_bytes) {
    return false;
  }
  *memory_bytes += value.binary_value.size();
  return true;
}

bool ProjectionInputMemoryBytes(const DescriptorBatch& batch,
                                std::uint64_t* memory_bytes) {
  if (memory_bytes == nullptr) return false;
  *memory_bytes = 1;
  for (const auto& row : batch.rows) {
    for (const auto& value : row.values) {
      if (!AddProjectionValueMemoryBytes(value, memory_bytes)) return false;
    }
  }
  return true;
}

bool ProjectionOutputMemoryBytes(
    const DescriptorBatch& batch,
    const std::vector<std::size_t>& projected_columns,
    std::uint64_t* memory_bytes) {
  if (memory_bytes == nullptr) return false;
  *memory_bytes = 1;
  for (const auto& row : batch.rows) {
    for (const auto source_column : projected_columns) {
      if (source_column >= row.values.size() ||
          !AddProjectionValueMemoryBytes(row.values[source_column],
                                         memory_bytes)) {
        return false;
      }
    }
  }
  return true;
}

}  // namespace

// QOW-SOURCE-QRY-007-PROJECTION-V1
// Canonical typed project-node implementation.  The physical DAG retains the
// engine MGA statement context and immutable descriptor handles; only after
// they validate does this operator consume reusable descriptor mechanics.
namespace {
CanonicalDescriptorProjectionResult ExecuteCanonicalDescriptorProjectionBound(
    const CanonicalDescriptorProjectionRequest& request,
    const TypedPhysicalNodeDag& execution_dag,
    const DescriptorBatch& execution_input_batch,
    const std::vector<std::size_t>& execution_projected_columns,
    const bool borrowed_execution_carriers) {
  CanonicalDescriptorProjectionResult result;
  const auto refuse = [&](DescriptorRuntimeDiagnostic diagnostic) {
    result.diagnostic = std::move(diagnostic);
    result.output_batch = {};
    return result;
  };

  if (borrowed_execution_carriers &&
      (!TypedPhysicalNodeDagCarrierIsExactDefault(request.physical_dag) ||
       !DescriptorBatchCarrierIsExactDefault(request.input_batch) ||
       !ProjectionColumnCarrierIsExactDefault(
           request.projected_columns))) {
    return refuse(Refusal(
        "QOW-DIAG-QRY-029-CANONICAL-PHYSICAL-ROUTE-V1",
        "descriptor projection request carries conflicting owned execution "
        "carriers"));
  }
  if (request.borrowed_mga_authority != nullptr &&
      !CanonicalExecutionMgaAuthorityCarrierIsExactDefault(
          request.mga_authority)) {
    return refuse(Refusal(
        "QOW-DIAG-QRY-029-CANONICAL-PHYSICAL-ROUTE-V1",
        "descriptor projection carries conflicting MGA authority"));
  }
  const auto& active_mga_authority =
      request.borrowed_mga_authority == nullptr
          ? request.mga_authority
          : *request.borrowed_mga_authority;

  const auto authority_validation = RevalidateCanonicalExecutionMgaAuthority(
      active_mga_authority, execution_dag);
  if (!authority_validation.ok) {
    return refuse(authority_validation);
  }
  const PhysicalNodeRecord* selected_node = nullptr;
  const PhysicalNodeRecord* input_node = nullptr;
  for (const auto& node : execution_dag.nodes) {
    if (node.physical_node_id == request.selected_physical_node_id) {
      selected_node = &node;
    }
  }
  if (request.selected_physical_node_id == 0 ||
      request.selected_physical_node_id !=
          execution_dag.root_physical_node_id ||
      selected_node == nullptr) {
    return refuse(Refusal("SBLR.PLAN_TREE.INVALID_HANDLE",
                          "selected projection node is not the physical root"));
  }
  if (selected_node->node_kind != PhysicalNodeKind::kProject ||
      !IsCanonicalDescriptorProjectionImplementation(
          selected_node->implementation_id) ||
      selected_node->input_physical_node_ids.size() != 1) {
    return refuse(Refusal(
        "QOW-DIAG-QRY-029-CANONICAL-PHYSICAL-ROUTE-V1",
        "descriptor projection requires one selected canonical direct "
        "project implementation"));
  }
  for (const auto& node : execution_dag.nodes) {
    if (node.physical_node_id ==
        selected_node->input_physical_node_ids.front()) {
      input_node = &node;
      break;
    }
  }
  if (input_node == nullptr) {
    return refuse(Refusal("SBLR.PLAN_TREE.INVALID_HANDLE",
                          "project input node is unresolved"));
  }
  if (execution_projected_columns.size() !=
      selected_node->output_descriptor_ids.size()) {
    return refuse(Refusal("SBLR.PLAN_TREE.INVALID_HANDLE",
                          "project output handle width mismatch"));
  }

  auto input_validation = ValidateCanonicalDescriptorBatch(
      execution_input_batch, input_node->output_descriptor_ids);
  if (!input_validation.ok) return refuse(std::move(input_validation));
  for (std::size_t ordinal = 0;
       ordinal < execution_projected_columns.size(); ++ordinal) {
    const auto source_column = execution_projected_columns[ordinal];
    if (source_column >= execution_input_batch.columns.size() ||
        execution_input_batch.columns[source_column].descriptor_id !=
            selected_node->output_descriptor_ids[ordinal]) {
      return refuse(Refusal(
          "SBLR.PLAN_TREE.INVALID_HANDLE",
          "projected column does not resolve to selected output descriptor",
          0, source_column));
    }
  }

  std::uint64_t input_memory_bytes = 0;
  std::uint64_t output_memory_bytes = 0;
  if (!ProjectionInputMemoryBytes(execution_input_batch,
                                  &input_memory_bytes) ||
      !ProjectionOutputMemoryBytes(execution_input_batch,
                                   execution_projected_columns,
                                   &output_memory_bytes) ||
      selected_node->memory_bytes_required == 0 ||
      selected_node->memory_bytes_required >
          execution_dag.memory_budget_bytes ||
      selected_node->memory_bytes_required >
          static_cast<std::uint64_t>(
              std::numeric_limits<std::size_t>::max())) {
    return refuse(Refusal(
        "SBLR.PLAN_TREE.RESOURCE_LIMIT",
        "projection memory grant or runtime payload accounting is invalid"));
  }
  auto remaining_memory_bytes = selected_node->memory_bytes_required;
  const auto charge = [&](const std::uint64_t bytes) {
    if (bytes > remaining_memory_bytes) return false;
    remaining_memory_bytes -= bytes;
    return true;
  };
  if (!charge(input_memory_bytes) || !charge(output_memory_bytes)) {
    return refuse(Refusal(
        "SBLR.PLAN_TREE.RESOURCE_LIMIT",
        "projection materialization exceeds the selected node memory grant"));
  }

  result.output_batch = ProjectDescriptorBatch(
      execution_input_batch, execution_projected_columns);
  for (std::size_t row = 0; row < result.output_batch.rows.size(); ++row) {
    for (std::size_t column = 0;
         column < result.output_batch.columns.size(); ++column) {
      internal_api::EngineCanonicalExpressionEvaluationRequest
          expression_request;
      expression_request.consumer =
          internal_api::EngineCanonicalExpressionConsumer::projection;
      expression_request.operation =
          internal_api::EngineCanonicalExpressionOperation::identity;
      expression_request.left_value =
          std::move(result.output_batch.rows[row].values[column]);
      expression_request.result_descriptor =
          result.output_batch.columns[column].descriptor;
      internal_api::EngineCanonicalExpressionEvaluationResult
          expression_result;
      std::string expression_detail;
      if (!internal_api::QowEvaluateCanonicalTypedExpressionV1(
              expression_request, &expression_result,
              &expression_detail)) {
        return refuse(Refusal(
            "QOW-DIAG-QRY-008-CANONICAL-EXPRESSION-RUNTIME-V1",
            std::move(expression_detail), row, column));
      }
      result.output_batch.rows[row].values[column] =
          std::move(expression_result.value);
    }
  }
  auto output_validation = ValidateCanonicalDescriptorBatch(
      result.output_batch, selected_node->output_descriptor_ids);
  if (!output_validation.ok) return refuse(std::move(output_validation));
  const auto result_authority = RevalidateCanonicalExecutionMgaAuthority(
      active_mga_authority, execution_dag);
  if (!result_authority.ok) return refuse(result_authority);
  result.diagnostic = {};
  result.selected_plan_uuid = execution_dag.selected_plan_uuid;
  result.executed_physical_node_id = selected_node->physical_node_id;
  result.causal_counter_id = selected_node->causal_counter_id;
  result.mga_statement_context = active_mga_authority.statement_context;
  return result;
}
}  // namespace

CanonicalDescriptorProjectionResult ExecuteCanonicalDescriptorProjection(
    const CanonicalDescriptorProjectionRequest& request) {
  return ExecuteCanonicalDescriptorProjectionBound(
      request, request.physical_dag, request.input_batch,
      request.projected_columns, false);
}

CanonicalDescriptorProjectionResult ExecuteCanonicalDescriptorProjection(
    const CanonicalDescriptorProjectionRequest& request,
    const TypedPhysicalNodeDag& borrowed_execution_dag,
    const DescriptorBatch& borrowed_input_batch,
    const std::vector<std::size_t>& borrowed_projected_columns) {
  return ExecuteCanonicalDescriptorProjectionBound(
      request, borrowed_execution_dag, borrowed_input_batch,
      borrowed_projected_columns, true);
}

}  // namespace scratchbird::engine::executor

// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "descriptor_value_runtime.hpp"

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

}  // namespace

// QOW-SOURCE-QRY-007-PROJECTION-V1
// Canonical typed project-node implementation.  The physical DAG retains the
// engine MGA statement context and immutable descriptor handles; only after
// they validate does this operator consume reusable descriptor mechanics.
CanonicalDescriptorProjectionResult ExecuteCanonicalDescriptorProjection(
    const CanonicalDescriptorProjectionRequest& request) {
  CanonicalDescriptorProjectionResult result;
  const auto refuse = [&](DescriptorRuntimeDiagnostic diagnostic) {
    result.diagnostic = std::move(diagnostic);
    result.output_batch = {};
    return result;
  };

  const auto authority_validation = RevalidateCanonicalExecutionMgaAuthority(
      request.mga_authority, request.physical_dag);
  if (!authority_validation.ok) {
    return refuse(authority_validation);
  }
  const PhysicalNodeRecord* selected_node = nullptr;
  const PhysicalNodeRecord* input_node = nullptr;
  for (const auto& node : request.physical_dag.nodes) {
    if (node.physical_node_id == request.selected_physical_node_id) {
      selected_node = &node;
    }
  }
  if (request.selected_physical_node_id == 0 ||
      request.selected_physical_node_id !=
          request.physical_dag.root_physical_node_id ||
      selected_node == nullptr) {
    return refuse(Refusal("SBLR.PLAN_TREE.INVALID_HANDLE",
                          "selected projection node is not the physical root"));
  }
  if (selected_node->node_kind != PhysicalNodeKind::kProject ||
      selected_node->input_physical_node_ids.size() != 1) {
    return refuse(Refusal(
        "QOW-DIAG-QRY-029-CANONICAL-PHYSICAL-ROUTE-V1",
        "descriptor projection requires one selected project node"));
  }
  for (const auto& node : request.physical_dag.nodes) {
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
  if (request.projected_columns.size() !=
      selected_node->output_descriptor_ids.size()) {
    return refuse(Refusal("SBLR.PLAN_TREE.INVALID_HANDLE",
                          "project output handle width mismatch"));
  }

  auto input_validation = ValidateCanonicalDescriptorBatch(
      request.input_batch, input_node->output_descriptor_ids);
  if (!input_validation.ok) return refuse(std::move(input_validation));
  for (std::size_t ordinal = 0;
       ordinal < request.projected_columns.size(); ++ordinal) {
    const auto source_column = request.projected_columns[ordinal];
    if (source_column >= request.input_batch.columns.size() ||
        request.input_batch.columns[source_column].descriptor_id !=
            selected_node->output_descriptor_ids[ordinal]) {
      return refuse(Refusal(
          "SBLR.PLAN_TREE.INVALID_HANDLE",
          "projected column does not resolve to selected output descriptor",
          0, source_column));
    }
  }

  result.output_batch =
      ProjectDescriptorBatch(request.input_batch, request.projected_columns);
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
          result.output_batch.rows[row].values[column];
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
      request.mga_authority, request.physical_dag);
  if (!result_authority.ok) return refuse(result_authority);
  result.diagnostic = {};
  result.selected_plan_uuid = request.physical_dag.selected_plan_uuid;
  result.executed_physical_node_id = selected_node->physical_node_id;
  result.causal_counter_id = selected_node->causal_counter_id;
  result.mga_statement_context = request.mga_authority.statement_context;
  return result;
}

}  // namespace scratchbird::engine::executor

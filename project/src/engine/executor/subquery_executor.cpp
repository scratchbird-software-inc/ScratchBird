// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

//
// SBSQL bounded source-layout anchor. Runtime behavior for this family is
// implemented by the active dispatcher, executor, planner, or function modules
// linked beside this translation unit and covered by the corresponding proof
// gates. Keep family-specific growth in this bounded area or in the named
// shared runtime module, not in broad catch-all files.

#include "descriptor_value_runtime.hpp"

#include <utility>

namespace scratchbird::engine::executor {
namespace {

DescriptorRuntimeDiagnostic TableSubqueryRefusal(std::string detail) {
  DescriptorRuntimeDiagnostic diagnostic;
  diagnostic.ok = false;
  diagnostic.diagnostic_code =
      "QOW-DIAG-QRY-013-TABLE-REFUSAL-V1";
  diagnostic.detail = std::move(detail);
  return diagnostic;
}

}  // namespace

// QOW-SOURCE-QRY-013-TABLE-V1
// Materialize one table-subquery result through the selected canonical
// relational node. The engine-owned physical DAG supplies MGA statement
// context and immutable descriptor handles; parser or donor syntax is never
// consulted here. Validation and the resource bound complete before any
// result batch is published.
CanonicalTableSubqueryResult ExecuteCanonicalTableSubquery(
    const CanonicalTableSubqueryRequest& request) {
  CanonicalTableSubqueryResult result;
  const auto refuse = [&](std::string detail) {
    result.diagnostic = TableSubqueryRefusal(std::move(detail));
    result.output_batch = {};
    result.materialized_row_count = 0;
    result.selected_plan_uuid.clear();
    result.executed_physical_node_id = 0;
    result.causal_counter_id = 0;
    return result;
  };

  const auto dag_validation =
      ValidateTypedPhysicalNodeDag(request.physical_dag);
  if (!dag_validation.accepted) {
    const auto& issue = dag_validation.issues.front();
    return refuse(issue.diagnostic_id + ":" + issue.field_id);
  }
  if (request.selected_physical_node_id == 0 ||
      request.selected_physical_node_id !=
          request.physical_dag.root_physical_node_id) {
    return refuse("selected table-subquery node is not the physical root");
  }

  const PhysicalNodeRecord* selected_node = nullptr;
  for (const auto& node : request.physical_dag.nodes) {
    if (node.physical_node_id == request.selected_physical_node_id) {
      selected_node = &node;
      break;
    }
  }
  if (selected_node == nullptr ||
      selected_node->node_kind != PhysicalNodeKind::kSubquery ||
      selected_node->input_physical_node_ids.size() != 1) {
    return refuse("table subquery requires one selected subquery node");
  }

  const auto input_id = selected_node->input_physical_node_ids.front();
  const PhysicalNodeRecord* input_node = nullptr;
  for (const auto& node : request.physical_dag.nodes) {
    if (node.physical_node_id == input_id) {
      input_node = &node;
      break;
    }
  }
  if (input_node == nullptr) {
    return refuse("table-subquery relational input is unresolved");
  }
  if (selected_node->output_descriptor_ids !=
      input_node->output_descriptor_ids) {
    return refuse("table-subquery output descriptor handles drifted");
  }

  auto input_validation = ValidateCanonicalDescriptorBatch(
      request.input_batch, input_node->output_descriptor_ids);
  if (!input_validation.ok) {
    return refuse(input_validation.diagnostic_code + ":" +
                  input_validation.detail);
  }
  if (request.maximum_materialized_row_count == 0 ||
      request.input_batch.rows.size() >
          request.maximum_materialized_row_count) {
    return refuse("table-subquery materialization row bound was exceeded");
  }

  DescriptorBatch materialized = request.input_batch;
  auto output_validation = ValidateCanonicalDescriptorBatch(
      materialized, selected_node->output_descriptor_ids);
  if (!output_validation.ok) {
    return refuse(output_validation.diagnostic_code + ":" +
                  output_validation.detail);
  }

  result.diagnostic = {};
  result.output_batch = std::move(materialized);
  result.materialized_row_count = result.output_batch.rows.size();
  result.selected_plan_uuid = request.physical_dag.selected_plan_uuid;
  result.executed_physical_node_id = selected_node->physical_node_id;
  result.causal_counter_id = selected_node->causal_counter_id;
  return result;
}

// QOW-SOURCE-QRY-013-SCALAR-V1
// Enforce the scalar-subquery zero/one/many-row contract over the canonical
// table-subquery result. Zero rows produce one typed SQL NULL, one row
// preserves its value, and more than one row fails before any scalar result is
// published.
CanonicalScalarSubqueryResult ExecuteCanonicalScalarSubquery(
    const CanonicalScalarSubqueryRequest& request) {
  using scratchbird::engine::internal_api::EngineTypedValue;
  using scratchbird::engine::internal_api::EngineValueState;

  CanonicalScalarSubqueryResult result;
  const auto refuse = [&](std::string detail) {
    result.diagnostic.ok = false;
    result.diagnostic.diagnostic_code =
        "QOW-DIAG-QRY-013-SCALAR-REFUSAL-V1";
    result.diagnostic.detail = std::move(detail);
    result.output_batch = {};
    result.source_row_count = 0;
    result.selected_plan_uuid.clear();
    result.executed_physical_node_id = 0;
    result.causal_counter_id = 0;
    return result;
  };

  auto table = ExecuteCanonicalTableSubquery(request.table_request);
  if (!table.diagnostic.ok) {
    return refuse(table.diagnostic.diagnostic_code + ":" +
                  table.diagnostic.detail);
  }
  if (table.output_batch.columns.size() != 1) {
    return refuse("scalar subquery requires exactly one result column");
  }

  const auto& source_column = table.output_batch.columns.front();
  const auto& source_descriptor = source_column.descriptor;
  const auto& result_descriptor = request.result_column.descriptor;
  if (request.value_expression_descriptor_id == 0 ||
      request.value_expression_descriptor_id != source_column.descriptor_id ||
      request.result_column.descriptor_id != source_column.descriptor_id ||
      request.result_column.stable_name.empty() ||
      !request.result_column.nullable ||
      result_descriptor.descriptor_uuid.canonical !=
          source_descriptor.descriptor_uuid.canonical ||
      result_descriptor.descriptor_kind != source_descriptor.descriptor_kind ||
      result_descriptor.canonical_type_name !=
          source_descriptor.canonical_type_name ||
      result_descriptor.encoded_descriptor !=
          source_descriptor.encoded_descriptor) {
    return refuse("scalar result descriptor is not the bound source column");
  }

  DescriptorBatch output;
  output.columns = {request.result_column};
  auto schema_validation = ValidateCanonicalDescriptorBatch(
      output, {request.result_column.descriptor_id});
  if (!schema_validation.ok) {
    return refuse(schema_validation.diagnostic_code + ":" +
                  schema_validation.detail);
  }
  if (table.materialized_row_count > 1) {
    return refuse("scalar subquery produced more than one row");
  }

  EngineTypedValue scalar_value;
  if (table.materialized_row_count == 0) {
    scalar_value.descriptor = request.result_column.descriptor;
    scalar_value.is_null = true;
    scalar_value.state = EngineValueState::sql_null;
  } else {
    scalar_value = table.output_batch.rows.front().values.front();
  }
  output.rows = {{{std::move(scalar_value)}}};
  auto output_validation = ValidateCanonicalDescriptorBatch(
      output, {request.result_column.descriptor_id});
  if (!output_validation.ok) {
    return refuse(output_validation.diagnostic_code + ":" +
                  output_validation.detail);
  }

  result.diagnostic = {};
  result.output_batch = std::move(output);
  result.source_row_count = table.materialized_row_count;
  result.selected_plan_uuid = std::move(table.selected_plan_uuid);
  result.executed_physical_node_id = table.executed_physical_node_id;
  result.causal_counter_id = table.causal_counter_id;
  return result;
}

}  // namespace scratchbird::engine::executor

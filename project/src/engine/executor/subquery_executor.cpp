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

#include <limits>
#include <string_view>
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

// QOW-SOURCE-QRY-013-ROW-V1
// Enforce the row-subquery cardinality contract over the canonical table
// result. Zero rows produce one typed all-NULL row, one row preserves every
// field, and more than one row fails before any row value is published.
CanonicalRowSubqueryResult ExecuteCanonicalRowSubquery(
    const CanonicalRowSubqueryRequest& request) {
  using scratchbird::engine::internal_api::EngineTypedValue;
  using scratchbird::engine::internal_api::EngineValueState;

  CanonicalRowSubqueryResult result;
  const auto refuse = [&](std::string detail) {
    result.diagnostic.ok = false;
    result.diagnostic.diagnostic_code =
        "QOW-DIAG-QRY-013-ROW-REFUSAL-V1";
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
  const auto width = table.output_batch.columns.size();
  if (width == 0 || request.row_expression_descriptor_ids.size() != width ||
      request.result_columns.size() != width) {
    return refuse("row subquery result width is not completely bound");
  }

  std::vector<std::uint32_t> result_descriptor_ids;
  result_descriptor_ids.reserve(width);
  for (std::size_t column = 0; column < width; ++column) {
    const auto& source = table.output_batch.columns[column];
    const auto& bound = request.result_columns[column];
    const auto& source_descriptor = source.descriptor;
    const auto& bound_descriptor = bound.descriptor;
    if (request.row_expression_descriptor_ids[column] == 0 ||
        request.row_expression_descriptor_ids[column] !=
            source.descriptor_id ||
        bound.descriptor_id != source.descriptor_id ||
        bound.stable_name.empty() || !bound.nullable ||
        bound_descriptor.descriptor_uuid.canonical !=
            source_descriptor.descriptor_uuid.canonical ||
        bound_descriptor.descriptor_kind !=
            source_descriptor.descriptor_kind ||
        bound_descriptor.canonical_type_name !=
            source_descriptor.canonical_type_name ||
        bound_descriptor.encoded_descriptor !=
            source_descriptor.encoded_descriptor) {
      return refuse("row result descriptor is not the bound source field");
    }
    result_descriptor_ids.push_back(bound.descriptor_id);
  }

  DescriptorBatch output;
  output.columns = request.result_columns;
  auto schema_validation =
      ValidateCanonicalDescriptorBatch(output, result_descriptor_ids);
  if (!schema_validation.ok) {
    return refuse(schema_validation.diagnostic_code + ":" +
                  schema_validation.detail);
  }
  if (table.materialized_row_count > 1) {
    return refuse("row subquery produced more than one row");
  }

  DescriptorTuple row;
  if (table.materialized_row_count == 0) {
    row.values.reserve(width);
    for (const auto& column : request.result_columns) {
      EngineTypedValue null_value;
      null_value.descriptor = column.descriptor;
      null_value.is_null = true;
      null_value.state = EngineValueState::sql_null;
      row.values.push_back(std::move(null_value));
    }
  } else {
    row = table.output_batch.rows.front();
  }
  output.rows.push_back(std::move(row));
  auto output_validation =
      ValidateCanonicalDescriptorBatch(output, result_descriptor_ids);
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

// QOW-SOURCE-QRY-013-EXISTS-V1
// Evaluate EXISTS only after the complete canonical table-subquery result has
// validated. The result is always one bound non-NULL boolean row; no input
// value or parser-level predicate can supply existence authority.
CanonicalExistsSubqueryResult ExecuteCanonicalExistsSubquery(
    const CanonicalExistsSubqueryRequest& request) {
  using scratchbird::engine::internal_api::EngineTypedValue;
  using scratchbird::engine::internal_api::EngineValueState;

  CanonicalExistsSubqueryResult result;
  const auto refuse = [&](std::string detail) {
    result.diagnostic.ok = false;
    result.diagnostic.diagnostic_code =
        "QOW-DIAG-QRY-013-EXISTS-REFUSAL-V1";
    result.diagnostic.detail = std::move(detail);
    result.output_batch = {};
    result.source_row_count = 0;
    result.exists = false;
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
  if (request.exists_expression_descriptor_id == 0 ||
      request.result_column.descriptor_id !=
          request.exists_expression_descriptor_id ||
      request.result_column.stable_name.empty() ||
      request.result_column.nullable ||
      request.result_column.descriptor.descriptor_kind != "scalar" ||
      request.result_column.descriptor.canonical_type_name != "boolean") {
    return refuse("EXISTS result is not a bound non-null boolean");
  }

  DescriptorBatch output;
  output.columns = {request.result_column};
  auto schema_validation = ValidateCanonicalDescriptorBatch(
      output, {request.result_column.descriptor_id});
  if (!schema_validation.ok) {
    return refuse(schema_validation.diagnostic_code + ":" +
                  schema_validation.detail);
  }

  const bool exists = table.materialized_row_count != 0;
  EngineTypedValue value;
  value.descriptor = request.result_column.descriptor;
  value.encoded_value = exists ? "true" : "false";
  value.state = EngineValueState::value;
  output.rows = {{{std::move(value)}}};
  auto output_validation = ValidateCanonicalDescriptorBatch(
      output, {request.result_column.descriptor_id});
  if (!output_validation.ok) {
    return refuse(output_validation.diagnostic_code + ":" +
                  output_validation.detail);
  }

  result.diagnostic = {};
  result.output_batch = std::move(output);
  result.source_row_count = table.materialized_row_count;
  result.exists = exists;
  result.selected_plan_uuid = std::move(table.selected_plan_uuid);
  result.executed_physical_node_id = table.executed_physical_node_id;
  result.causal_counter_id = table.causal_counter_id;
  return result;
}

// QOW-SOURCE-QRY-013-QUANTIFIED-V1
// Evaluate one bound int64 comparison against a one-column canonical table
// result with SQL ANY/ALL three-valued folding. Every operand is decoded before
// publication, so a decisive early truth cannot hide malformed later input.
CanonicalQuantifiedSubqueryResult ExecuteCanonicalQuantifiedSubquery(
    const CanonicalQuantifiedSubqueryRequest& request) {
  namespace api = scratchbird::engine::internal_api;

  CanonicalQuantifiedSubqueryResult result;
  const auto refuse = [&](std::string detail) {
    result.diagnostic.ok = false;
    result.diagnostic.diagnostic_code =
        "QOW-DIAG-QRY-013-QUANTIFIED-REFUSAL-V1";
    result.diagnostic.detail = std::move(detail);
    result.output_batch = {};
    result.truth_value = api::EngineSqlTruthValue::unspecified;
    result.comparison_count = 0;
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
    return refuse("quantified subquery requires exactly one result column");
  }
  const auto& right_column = table.output_batch.columns.front();
  if (request.right_expression_descriptor_id == 0 ||
      request.right_expression_descriptor_id != right_column.descriptor_id ||
      request.left_operand_column.descriptor_id == 0 ||
      request.left_operand_column.stable_name.empty() ||
      request.left_operand_column.descriptor.canonical_type_name != "int64" ||
      right_column.descriptor.canonical_type_name != "int64") {
    return refuse("quantified comparison operands are not bound int64");
  }
  const bool any = request.quantifier ==
                   CanonicalQuantifiedSubqueryQuantifier::kAny;
  const bool all = request.quantifier ==
                   CanonicalQuantifiedSubqueryQuantifier::kAll;
  if (!any && !all) {
    return refuse("quantified comparison quantifier is not bound");
  }
  using Operation = api::EngineComparisonPredicateOperator;
  switch (request.comparison_operator) {
    case Operation::equal:
    case Operation::not_equal:
    case Operation::less_than:
    case Operation::less_than_or_equal:
    case Operation::greater_than:
    case Operation::greater_than_or_equal:
      break;
    default:
      return refuse("quantified comparison operator is not bound");
  }
  if (request.maximum_comparison_count == 0 ||
      table.materialized_row_count > request.maximum_comparison_count) {
    return refuse("quantified comparison resource bound was exceeded");
  }

  DescriptorBatch left_batch;
  left_batch.columns = {request.left_operand_column};
  left_batch.rows = {{{request.left_value}}};
  auto left_validation = ValidateCanonicalDescriptorBatch(
      left_batch, {request.left_operand_column.descriptor_id});
  if (!left_validation.ok) {
    return refuse(left_validation.diagnostic_code + ":" +
                  left_validation.detail);
  }
  std::int64_t decoded_left = 0;
  const bool left_is_null = request.left_value.state ==
                            api::EngineValueState::sql_null;
  if (!left_is_null) {
    const auto decoded = DecodeInt64Value(request.left_value);
    if (!decoded.ok()) {
      return refuse(decoded.diagnostic.diagnostic_code + ":" +
                    decoded.diagnostic.detail);
    }
    decoded_left = decoded.value;
  }

  std::vector<api::EngineSqlTruthValue> comparison_truths;
  comparison_truths.reserve(table.materialized_row_count);
  for (const auto& row : table.output_batch.rows) {
    const auto& right_value = row.values.front();
    if (left_is_null ||
        right_value.state == api::EngineValueState::sql_null) {
      comparison_truths.push_back(api::EngineSqlTruthValue::unknown);
      continue;
    }
    const auto decoded_right = DecodeInt64Value(right_value);
    if (!decoded_right.ok()) {
      return refuse(decoded_right.diagnostic.diagnostic_code + ":" +
                    decoded_right.diagnostic.detail);
    }
    const auto right = decoded_right.value;
    bool predicate = false;
    switch (request.comparison_operator) {
      case Operation::equal:
        predicate = decoded_left == right;
        break;
      case Operation::not_equal:
        predicate = decoded_left != right;
        break;
      case Operation::less_than:
        predicate = decoded_left < right;
        break;
      case Operation::less_than_or_equal:
        predicate = decoded_left <= right;
        break;
      case Operation::greater_than:
        predicate = decoded_left > right;
        break;
      case Operation::greater_than_or_equal:
        predicate = decoded_left >= right;
        break;
      default:
        return refuse("quantified comparison operator changed after binding");
    }
    comparison_truths.push_back(
        predicate ? api::EngineSqlTruthValue::true_value
                  : api::EngineSqlTruthValue::false_value);
  }

  auto truth = any ? api::EngineSqlTruthValue::false_value
                   : api::EngineSqlTruthValue::true_value;
  for (const auto comparison_truth : comparison_truths) {
    if (any) {
      if (comparison_truth == api::EngineSqlTruthValue::true_value) {
        truth = api::EngineSqlTruthValue::true_value;
      } else if (comparison_truth == api::EngineSqlTruthValue::unknown &&
                 truth == api::EngineSqlTruthValue::false_value) {
        truth = api::EngineSqlTruthValue::unknown;
      }
    } else {
      if (comparison_truth == api::EngineSqlTruthValue::false_value) {
        truth = api::EngineSqlTruthValue::false_value;
      } else if (comparison_truth == api::EngineSqlTruthValue::unknown &&
                 truth == api::EngineSqlTruthValue::true_value) {
        truth = api::EngineSqlTruthValue::unknown;
      }
    }
  }

  if (request.result_expression_descriptor_id == 0 ||
      request.result_column.descriptor_id !=
          request.result_expression_descriptor_id ||
      request.result_column.stable_name.empty() ||
      !request.result_column.nullable ||
      request.result_column.descriptor.descriptor_kind != "scalar" ||
      request.result_column.descriptor.canonical_type_name != "boolean") {
    return refuse("quantified result is not a bound nullable boolean");
  }
  DescriptorBatch output;
  output.columns = {request.result_column};
  api::EngineTypedValue value;
  value.descriptor = request.result_column.descriptor;
  if (truth == api::EngineSqlTruthValue::unknown) {
    value.is_null = true;
    value.state = api::EngineValueState::sql_null;
  } else {
    value.encoded_value = truth == api::EngineSqlTruthValue::true_value
                              ? "true"
                              : "false";
    value.state = api::EngineValueState::value;
  }
  output.rows = {{{std::move(value)}}};
  auto output_validation = ValidateCanonicalDescriptorBatch(
      output, {request.result_column.descriptor_id});
  if (!output_validation.ok) {
    return refuse(output_validation.diagnostic_code + ":" +
                  output_validation.detail);
  }

  result.diagnostic = {};
  result.output_batch = std::move(output);
  result.truth_value = truth;
  result.comparison_count = comparison_truths.size();
  result.selected_plan_uuid = std::move(table.selected_plan_uuid);
  result.executed_physical_node_id = table.executed_physical_node_id;
  result.causal_counter_id = table.causal_counter_id;
  return result;
}

// QOW-SOURCE-QRY-013-CORRELATED-V1
// Bind and execute one correlated int64-equality subquery scope for every
// outer row. The physical root owns both input relations and emits the inner
// descriptor shape; scope results retain physical outer-row identity and
// deterministic inner order without granting the parser execution authority.
CanonicalCorrelatedSubqueryResult ExecuteCanonicalCorrelatedSubquery(
    const CanonicalCorrelatedSubqueryRequest& request) {
  namespace api = scratchbird::engine::internal_api;

  CanonicalCorrelatedSubqueryResult result;
  const auto refuse = [&](std::string detail) {
    result.diagnostic.ok = false;
    result.diagnostic.diagnostic_code =
        "QOW-DIAG-QRY-013-CORRELATED-REFUSAL-V1";
    result.diagnostic.detail = std::move(detail);
    result.scopes.clear();
    result.scope_execution_count = 0;
    result.comparison_count = 0;
    result.result_row_count = 0;
    result.selected_plan_uuid.clear();
    result.executed_physical_node_id = 0;
    result.causal_counter_id = 0;
    return result;
  };

  const auto dag_validation = ValidateTypedPhysicalNodeDag(request.physical_dag);
  if (!dag_validation.accepted) {
    const auto& issue = dag_validation.issues.front();
    return refuse(issue.diagnostic_id + ":" + issue.field_id);
  }
  if (request.selected_physical_node_id == 0 ||
      request.selected_physical_node_id !=
          request.physical_dag.root_physical_node_id) {
    return refuse("selected correlated subquery is not the physical root");
  }

  const PhysicalNodeRecord* selected_node = nullptr;
  const PhysicalNodeRecord* outer_node = nullptr;
  const PhysicalNodeRecord* inner_node = nullptr;
  for (const auto& node : request.physical_dag.nodes) {
    if (node.physical_node_id == request.selected_physical_node_id) {
      selected_node = &node;
      break;
    }
  }
  if (selected_node == nullptr ||
      selected_node->node_kind != PhysicalNodeKind::kSubquery ||
      selected_node->implementation_id !=
          "subquery.correlated.int64-equality.typed.v1" ||
      selected_node->input_physical_node_ids.size() != 2) {
    return refuse("correlated subquery physical profile is not bound");
  }
  for (const auto& node : request.physical_dag.nodes) {
    if (node.physical_node_id ==
        selected_node->input_physical_node_ids[0]) {
      outer_node = &node;
    }
    if (node.physical_node_id ==
        selected_node->input_physical_node_ids[1]) {
      inner_node = &node;
    }
  }
  if (outer_node == nullptr || inner_node == nullptr ||
      selected_node->output_descriptor_ids !=
          inner_node->output_descriptor_ids) {
    return refuse("correlated subquery input or output handles are unresolved");
  }

  auto outer_validation = ValidateCanonicalDescriptorBatch(
      request.outer_batch, outer_node->output_descriptor_ids);
  if (!outer_validation.ok) {
    return refuse(outer_validation.diagnostic_code + ":" +
                  outer_validation.detail);
  }
  auto inner_validation = ValidateCanonicalDescriptorBatch(
      request.inner_batch, inner_node->output_descriptor_ids);
  if (!inner_validation.ok) {
    return refuse(inner_validation.diagnostic_code + ":" +
                  inner_validation.detail);
  }
  if (request.outer_binding_column >= request.outer_batch.columns.size() ||
      request.inner_reference_column >= request.inner_batch.columns.size()) {
    return refuse("correlated binding column is outside its scope");
  }
  const auto& outer_column =
      request.outer_batch.columns[request.outer_binding_column];
  const auto& inner_column =
      request.inner_batch.columns[request.inner_reference_column];
  if (request.outer_binding_expression_descriptor_id == 0 ||
      request.outer_binding_expression_descriptor_id !=
          outer_column.descriptor_id ||
      request.inner_reference_expression_descriptor_id == 0 ||
      request.inner_reference_expression_descriptor_id !=
          inner_column.descriptor_id ||
      outer_column.descriptor.canonical_type_name != "int64" ||
      inner_column.descriptor.canonical_type_name != "int64") {
    return refuse("correlated binding is not an exact int64 handle pair");
  }

  const auto outer_count = request.outer_batch.rows.size();
  const auto inner_count = request.inner_batch.rows.size();
  if (request.maximum_scope_execution_count == 0 ||
      outer_count > request.maximum_scope_execution_count ||
      request.maximum_comparison_count == 0 ||
      (outer_count != 0 &&
       inner_count > request.maximum_comparison_count / outer_count) ||
      outer_count * inner_count > request.maximum_comparison_count ||
      request.maximum_result_row_count == 0) {
    return refuse("correlated subquery resource bound was exceeded");
  }

  std::vector<std::optional<std::int64_t>> outer_keys;
  outer_keys.reserve(outer_count);
  for (const auto& row : request.outer_batch.rows) {
    const auto& value = row.values[request.outer_binding_column];
    if (value.state == api::EngineValueState::sql_null) {
      outer_keys.push_back(std::nullopt);
      continue;
    }
    const auto decoded = DecodeInt64Value(value);
    if (!decoded.ok()) {
      return refuse(decoded.diagnostic.diagnostic_code + ":" +
                    decoded.diagnostic.detail);
    }
    outer_keys.push_back(decoded.value);
  }
  std::vector<std::optional<std::int64_t>> inner_keys;
  inner_keys.reserve(inner_count);
  for (const auto& row : request.inner_batch.rows) {
    const auto& value = row.values[request.inner_reference_column];
    if (value.state == api::EngineValueState::sql_null) {
      inner_keys.push_back(std::nullopt);
      continue;
    }
    const auto decoded = DecodeInt64Value(value);
    if (!decoded.ok()) {
      return refuse(decoded.diagnostic.diagnostic_code + ":" +
                    decoded.diagnostic.detail);
    }
    inner_keys.push_back(decoded.value);
  }

  std::vector<CanonicalCorrelatedScopeResult> scopes;
  scopes.reserve(outer_count);
  std::size_t result_row_count = 0;
  for (std::size_t outer_index = 0; outer_index < outer_count; ++outer_index) {
    CanonicalCorrelatedScopeResult scope;
    scope.outer_row_index = outer_index;
    scope.bound_outer_value =
        request.outer_batch.rows[outer_index]
            .values[request.outer_binding_column];
    scope.output_batch.columns = request.inner_batch.columns;
    if (outer_keys[outer_index].has_value()) {
      for (std::size_t inner_index = 0; inner_index < inner_count;
           ++inner_index) {
        if (inner_keys[inner_index].has_value() &&
            outer_keys[outer_index].value() ==
                inner_keys[inner_index].value()) {
          if (result_row_count == request.maximum_result_row_count) {
            return refuse("correlated subquery result bound was exceeded");
          }
          scope.output_batch.rows.push_back(request.inner_batch.rows[inner_index]);
          ++result_row_count;
        }
      }
    }
    auto scope_validation = ValidateCanonicalDescriptorBatch(
        scope.output_batch, selected_node->output_descriptor_ids);
    if (!scope_validation.ok) {
      return refuse(scope_validation.diagnostic_code + ":" +
                    scope_validation.detail);
    }
    scopes.push_back(std::move(scope));
  }

  result.diagnostic = {};
  result.scopes = std::move(scopes);
  result.scope_execution_count = outer_count;
  result.comparison_count = outer_count * inner_count;
  result.result_row_count = result_row_count;
  result.selected_plan_uuid = request.physical_dag.selected_plan_uuid;
  result.executed_physical_node_id = selected_node->physical_node_id;
  result.causal_counter_id = selected_node->causal_counter_id;
  return result;
}

// QOW-SOURCE-QRY-013-LATERAL-V2
// Bind and execute INNER/LEFT LATERAL and CROSS/OUTER APPLY table expansion by
// consuming proven correlated scopes. The lateral physical plan shares the
// exact engine MGA statement boundary and owns only outer-plus-inner relational
// flattening/null extension; correlation remains authoritative in
// ExecuteCanonicalCorrelatedSubquery.
CanonicalLateralSubqueryResult ExecuteCanonicalLateralSubquery(
    const CanonicalLateralSubqueryRequest& request) {
  CanonicalLateralSubqueryResult result;
  const auto refuse = [&](std::string detail) {
    result.diagnostic.ok = false;
    result.diagnostic.diagnostic_code =
        "QOW-DIAG-QRY-013-LATERAL-REFUSAL-V1";
    result.diagnostic.detail = std::move(detail);
    result.output_batch = {};
    result.form = CanonicalLateralJoinForm::kInnerLateral;
    result.scope_execution_count = 0;
    result.matched_scope_count = 0;
    result.null_extended_outer_row_count = 0;
    result.output_row_count = 0;
    result.correlated_plan_uuid.clear();
    result.selected_plan_uuid.clear();
    result.executed_physical_node_id = 0;
    result.causal_counter_id = 0;
    return result;
  };

  const auto dag_validation = ValidateTypedPhysicalNodeDag(request.physical_dag);
  if (!dag_validation.accepted) {
    const auto& issue = dag_validation.issues.front();
    return refuse(issue.diagnostic_id + ":" + issue.field_id);
  }
  if (request.selected_physical_node_id == 0 ||
      request.selected_physical_node_id !=
          request.physical_dag.root_physical_node_id) {
    return refuse("selected LATERAL node is not the physical root");
  }
  if (request.physical_dag.local_transaction_id !=
          request.correlated_request.physical_dag.local_transaction_id ||
      request.physical_dag.statement_snapshot_id !=
          request.correlated_request.physical_dag.statement_snapshot_id) {
    return refuse("LATERAL and correlation MGA statement contexts differ");
  }

  const PhysicalNodeRecord* selected_node = nullptr;
  const PhysicalNodeRecord* outer_node = nullptr;
  const PhysicalNodeRecord* subquery_node = nullptr;
  for (const auto& node : request.physical_dag.nodes) {
    if (node.physical_node_id == request.selected_physical_node_id) {
      selected_node = &node;
      break;
    }
  }
  std::string_view expected_implementation;
  bool null_extend_empty_scopes = false;
  switch (request.form) {
    case CanonicalLateralJoinForm::kInnerLateral:
      expected_implementation =
          "join.lateral-inner.correlated.typed.v1";
      break;
    case CanonicalLateralJoinForm::kLeftLateral:
      expected_implementation =
          "join.lateral-left.correlated.typed.v1";
      null_extend_empty_scopes = true;
      break;
    case CanonicalLateralJoinForm::kCrossApply:
      expected_implementation =
          "join.cross-apply.correlated.typed.v1";
      break;
    case CanonicalLateralJoinForm::kOuterApply:
      expected_implementation =
          "join.outer-apply.correlated.typed.v1";
      null_extend_empty_scopes = true;
      break;
    default:
      return refuse("LATERAL/APPLY form is outside the accepted profile");
  }
  if (selected_node == nullptr ||
      selected_node->node_kind != PhysicalNodeKind::kJoin ||
      selected_node->implementation_id != expected_implementation ||
      selected_node->input_physical_node_ids.size() != 2) {
    return refuse("LATERAL/APPLY physical profile is not exactly bound");
  }
  for (const auto& node : request.physical_dag.nodes) {
    if (node.physical_node_id ==
        selected_node->input_physical_node_ids[0]) {
      outer_node = &node;
    }
    if (node.physical_node_id ==
        selected_node->input_physical_node_ids[1]) {
      subquery_node = &node;
    }
  }
  if (outer_node == nullptr || subquery_node == nullptr ||
      subquery_node->node_kind != PhysicalNodeKind::kSubquery) {
    return refuse("LATERAL outer or correlated input node is unresolved");
  }

  std::vector<std::uint32_t> outer_descriptor_ids;
  outer_descriptor_ids.reserve(
      request.correlated_request.outer_batch.columns.size());
  for (const auto& column : request.correlated_request.outer_batch.columns) {
    outer_descriptor_ids.push_back(column.descriptor_id);
  }
  std::vector<std::uint32_t> inner_descriptor_ids;
  inner_descriptor_ids.reserve(
      request.correlated_request.inner_batch.columns.size());
  for (const auto& column : request.correlated_request.inner_batch.columns) {
    inner_descriptor_ids.push_back(column.descriptor_id);
  }
  std::vector<std::uint32_t> output_descriptor_ids = outer_descriptor_ids;
  output_descriptor_ids.insert(output_descriptor_ids.end(),
                               inner_descriptor_ids.begin(),
                               inner_descriptor_ids.end());
  if (outer_node->output_descriptor_ids != outer_descriptor_ids ||
      subquery_node->output_descriptor_ids != inner_descriptor_ids ||
      selected_node->output_descriptor_ids != output_descriptor_ids) {
    return refuse("LATERAL physical descriptor handles drifted");
  }
  if (request.maximum_output_row_count == 0) {
    return refuse("LATERAL output resource bound is zero");
  }

  auto correlated =
      ExecuteCanonicalCorrelatedSubquery(request.correlated_request);
  if (!correlated.diagnostic.ok) {
    return refuse(correlated.diagnostic.diagnostic_code + ":" +
                  correlated.diagnostic.detail);
  }
  if (correlated.scopes.size() !=
      request.correlated_request.outer_batch.rows.size()) {
    return refuse("correlated scopes do not cover the outer relation");
  }
  std::size_t matched_scope_count = 0;
  std::size_t null_extended_outer_row_count = 0;
  for (std::size_t scope_index = 0; scope_index < correlated.scopes.size();
       ++scope_index) {
    const auto& scope = correlated.scopes[scope_index];
    if (scope.outer_row_index != scope_index) {
      return refuse("correlated scope lost canonical outer-row order");
    }
    if (scope.output_batch.rows.empty()) {
      if (null_extend_empty_scopes) ++null_extended_outer_row_count;
    } else {
      ++matched_scope_count;
    }
  }
  if (correlated.result_row_count >
          std::numeric_limits<std::size_t>::max() -
              null_extended_outer_row_count ||
      correlated.result_row_count + null_extended_outer_row_count >
          request.maximum_output_row_count) {
    return refuse("LATERAL output resource bound was exceeded");
  }

  DescriptorBatch output;
  output.columns = request.correlated_request.outer_batch.columns;
  output.columns.insert(output.columns.end(),
                        request.correlated_request.inner_batch.columns.begin(),
                        request.correlated_request.inner_batch.columns.end());
  if (null_extend_empty_scopes) {
    for (std::size_t column =
             request.correlated_request.outer_batch.columns.size();
         column < output.columns.size(); ++column) {
      output.columns[column].nullable = true;
    }
  }
  output.rows.reserve(correlated.result_row_count +
                      null_extended_outer_row_count);
  for (const auto& scope : correlated.scopes) {
    const auto& outer_row =
        request.correlated_request.outer_batch.rows[scope.outer_row_index];
    if (scope.output_batch.rows.empty() && null_extend_empty_scopes) {
      DescriptorTuple lateral_row;
      lateral_row.values = outer_row.values;
      for (const auto& column :
           request.correlated_request.inner_batch.columns) {
        scratchbird::engine::internal_api::EngineTypedValue null_value;
        null_value.descriptor = column.descriptor;
        null_value.is_null = true;
        null_value.state = scratchbird::engine::internal_api::
            EngineValueState::sql_null;
        lateral_row.values.push_back(std::move(null_value));
      }
      output.rows.push_back(std::move(lateral_row));
      continue;
    }
    for (const auto& inner_row : scope.output_batch.rows) {
      DescriptorTuple lateral_row;
      lateral_row.values = outer_row.values;
      lateral_row.values.insert(lateral_row.values.end(),
                                inner_row.values.begin(),
                                inner_row.values.end());
      output.rows.push_back(std::move(lateral_row));
    }
  }
  auto output_validation =
      ValidateCanonicalDescriptorBatch(output, output_descriptor_ids);
  if (!output_validation.ok) {
    return refuse(output_validation.diagnostic_code + ":" +
                  output_validation.detail);
  }

  result.diagnostic = {};
  result.output_batch = std::move(output);
  result.form = request.form;
  result.scope_execution_count = correlated.scope_execution_count;
  result.matched_scope_count = matched_scope_count;
  result.null_extended_outer_row_count = null_extended_outer_row_count;
  result.output_row_count = result.output_batch.rows.size();
  result.correlated_plan_uuid = std::move(correlated.selected_plan_uuid);
  result.selected_plan_uuid = request.physical_dag.selected_plan_uuid;
  result.executed_physical_node_id = selected_node->physical_node_id;
  result.causal_counter_id = selected_node->causal_counter_id;
  return result;
}

}  // namespace scratchbird::engine::executor

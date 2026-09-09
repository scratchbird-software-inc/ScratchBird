// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "canonical_query_object_free_composition_support.hpp"

#include "canonical_query_descriptor_support.hpp"
#include "canonical_query_scalar_support.hpp"

#include "datatype_operations.hpp"

#include <algorithm>
#include <optional>
#include <ranges>
#include <unordered_map>
#include <utility>

namespace scratchbird::engine::sblr {

namespace dt = scratchbird::core::datatypes;

// SEARCH_KEY: SB_ENGINE_CANONICAL_QUERY_OBJECT_FREE_COMPOSITION_SUPPORT_AUTHORITY
api::EngineApiResult Failure(const CanonicalObjectFreeValuesExecutionRequest& request,
                             std::string diagnostic_id,
                             std::string detail) {
  api::EngineApiResult result;
  result.operation_id = "query.execute";
  result.local_transaction_id = request.context.local_transaction_id;
  result.transaction_uuid = request.context.transaction_uuid;
  result.embedded_trust_mode_observed =
      request.context.trust_mode == api::EngineTrustMode::embedded_in_process;
  api::EngineApiDiagnostic diagnostic;
  diagnostic.code = std::move(diagnostic_id);
  diagnostic.message_key = "engine.sblr.query_execute.refused";
  diagnostic.detail = std::move(detail);
  diagnostic.error = true;
  result.diagnostics.push_back(std::move(diagnostic));
  return result;
}

MaterializedValues MaterializeValues(
    const api::TypedRelationalDag& dag,
    const plan::CanonicalLogicalRelationalNode& logical_node,
    const CanonicalRelationalExpressionRuntimeServices& expression_services) {
  MaterializedValues result;
  const auto node_it = std::ranges::find_if(
      dag.nodes, [&](const auto& node) {
        return node.node_id == logical_node.logical_node_id;
      });
  if (node_it == dag.nodes.end() ||
      node_it->node_kind != api::RelationalDagNodeKind::kValues ||
      !node_it->input_node_ids.empty() || node_it->values_row_ids.empty() ||
      node_it->output_descriptor_ids.empty()) {
    result.detail = "live VALUES root shape is incomplete";
    return result;
  }

  std::unordered_map<std::uint32_t, const api::RelationalTypeDescriptor*>
      descriptors;
  std::unordered_map<std::uint32_t, const api::RelationalExpressionRecord*>
      expressions;
  std::unordered_map<std::uint32_t, const api::RelationalValuesRowRecord*> rows;
  for (const auto& descriptor : dag.descriptors) {
    descriptors.emplace(descriptor.descriptor_id, &descriptor);
  }
  for (const auto& expression : dag.expressions) {
    expressions.emplace(expression.expression_id, &expression);
  }
  for (const auto& row : dag.values_rows) rows.emplace(row.row_id, &row);
  CanonicalRelationalExpressionRuntime expression_runtime(
      dag, expression_services);
  const auto exact_values_literal_descriptor =
      [&](const api::RelationalExpressionRecord& expression,
          const std::uint32_t output_descriptor_id) {
    const auto expression_descriptor =
        descriptors.find(expression.result_descriptor_id);
    const auto output_descriptor = descriptors.find(output_descriptor_id);
    return expression.expression_kind ==
               api::RelationalExpressionKind::kLiteral &&
           expression.literal_kind ==
               api::RelationalLiteralKind::kNumeric &&
           expression.child_expression_ids.empty() &&
           !expression.function_uuid.has_value() &&
           !expression.bound_name_uuid.has_value() &&
           !expression.operator_name.has_value() &&
           !expression.literal_or_parameter_ref.has_value() &&
           expression.literal_typed_value_v1.has_value() &&
           !expression.parameter_typed_value_v1.has_value() &&
           expression_descriptor != descriptors.end() &&
           output_descriptor != descriptors.end() &&
           expression.literal_typed_value_v1->descriptor_uuid ==
               expression_descriptor->second->descriptor_uuid &&
           expression.literal_typed_value_v1->descriptor_generation != 0 &&
           expression.literal_typed_value_v1->value_state == "value" &&
           !expression.literal_typed_value_v1->canonical_value_bytes.empty() &&
           expression_descriptor->second->type_uuid ==
               output_descriptor->second->type_uuid &&
           expression_descriptor->second->nullability ==
               api::RelationalNullability::kNonNull &&
           output_descriptor->second->nullability ==
               api::RelationalNullability::kNonNull &&
           !expression_descriptor->second->collation_uuid.has_value() &&
           !output_descriptor->second->collation_uuid.has_value() &&
           !expression_descriptor->second->timezone_profile_id.has_value() &&
           !output_descriptor->second->timezone_profile_id.has_value() &&
           !expression_descriptor->second->width.has_value() &&
           !expression_descriptor->second->precision.has_value() &&
           !expression_descriptor->second->scale.has_value() &&
           !output_descriptor->second->width.has_value() &&
           !output_descriptor->second->precision.has_value() &&
           !output_descriptor->second->scale.has_value();
  };

  std::vector<const api::RelationalOutputRecord*> outputs;
  for (const auto& output : dag.outputs) {
    if (output.relation_node_id == node_it->node_id) outputs.push_back(&output);
  }
  std::ranges::sort(outputs, {}, &api::RelationalOutputRecord::ordinal);
  if (outputs.size() != node_it->output_descriptor_ids.size()) {
    result.detail = "live VALUES result output coverage is incomplete";
    return result;
  }

  std::vector<std::string> type_names(node_it->output_descriptor_ids.size());
  for (const auto row_id : node_it->values_row_ids) {
    const auto row = rows.find(row_id);
    if (row == rows.end() ||
        row->second->expression_ids.size() != type_names.size()) {
      result.detail = "live VALUES row width is inconsistent";
      return result;
    }
    for (std::size_t column = 0; column < type_names.size(); ++column) {
      const auto expression =
          expressions.find(row->second->expression_ids[column]);
      if (expression == expressions.end() ||
          (expression->second->result_descriptor_id !=
               node_it->output_descriptor_ids[column] &&
           !exact_values_literal_descriptor(
               *expression->second,
               node_it->output_descriptor_ids[column]))) {
        result.detail =
            "live VALUES expression result descriptor is not column-bound";
        return result;
      }
      std::string type_name;
      if (!expression_runtime.InferType(expression->first, std::nullopt,
                                        &type_name, &result.detail)) {
        return result;
      }
      if (type_name == "null") continue;
      if (!type_names[column].empty() &&
          dt::CanonicalTypeIdFromStableName(type_names[column]) !=
              dt::CanonicalTypeIdFromStableName(type_name)) {
        result.detail = "live VALUES column has unreconciled literal types";
        return result;
      }
      type_names[column] = type_name;
    }
  }

  for (const auto row_id : node_it->values_row_ids) {
    const auto* row = rows.at(row_id);
    for (std::size_t column = 0; column < type_names.size(); ++column) {
      std::string reconciled_type;
      if (type_names[column].empty() ||
          !expression_runtime.InferType(row->expression_ids[column],
                                        type_names[column], &reconciled_type,
                                        &result.detail)) {
        if (result.detail.empty()) {
          result.detail = "live VALUES column type is unresolved";
        }
        return result;
      }
    }
  }

  std::size_t published_ordinal = 0;
  for (std::size_t column = 0; column < type_names.size(); ++column) {
    const auto descriptor =
        descriptors.find(node_it->output_descriptor_ids[column]);
    if (descriptor == descriptors.end() || type_names[column].empty() ||
        descriptor->second->nullability ==
            api::RelationalNullability::kUnknown ||
        (descriptor->second->collation_uuid.has_value() &&
         dt::CanonicalTypeIdFromStableName(type_names[column]) !=
             dt::CanonicalTypeId::character) ||
        (descriptor->second->timezone_profile_id.has_value() &&
         type_names[column] != "timestamp") ||
        outputs[column]->ordinal != column ||
        outputs[column]->descriptor_id !=
            node_it->output_descriptor_ids[column] ||
        outputs[column]->output_name_utf8.empty()) {
      result.detail = "live VALUES descriptor or output binding is unresolved";
      return result;
    }
    api::EngineDescriptor engine_descriptor;
    engine_descriptor.descriptor_uuid.canonical =
        descriptor->second->descriptor_uuid;
    engine_descriptor.descriptor_kind = "scalar";
    engine_descriptor.canonical_type_name = type_names[column];
    engine_descriptor.encoded_descriptor =
        "type_uuid=" + descriptor->second->type_uuid + ";nullability=" +
        (descriptor->second->nullability ==
                 api::RelationalNullability::kNullable
             ? "nullable"
             : "non_null");
    if (descriptor->second->collation_uuid.has_value()) {
      engine_descriptor.encoded_descriptor +=
          ";collation_uuid=" + *descriptor->second->collation_uuid;
    }
    if (descriptor->second->timezone_profile_id.has_value()) {
      engine_descriptor.encoded_descriptor +=
          ";timezone_profile_id=" + *descriptor->second->timezone_profile_id;
    }
    if (descriptor->second->width.has_value()) {
      engine_descriptor.encoded_descriptor +=
          ";width=" + std::to_string(*descriptor->second->width);
    }
    if (descriptor->second->precision.has_value()) {
      engine_descriptor.encoded_descriptor +=
          ";precision=" + std::to_string(*descriptor->second->precision);
    }
    if (descriptor->second->scale.has_value()) {
      engine_descriptor.encoded_descriptor +=
          ";scale=" + std::to_string(*descriptor->second->scale);
    }
    result.batch.columns.push_back(
        {outputs[column]->output_name_utf8, engine_descriptor,
         descriptor->second->nullability ==
             api::RelationalNullability::kNullable,
         descriptor->second->descriptor_id});

    exec::CanonicalResultColumnBinding binding;
    binding.physical_column_ordinal = column;
    binding.visible = outputs[column]->visible;
    if (binding.visible) {
      binding.published_descriptor = exec::CanonicalResultColumnDescriptor{
          static_cast<std::uint32_t>(published_ordinal++),
          outputs[column]->output_name_utf8,
          descriptor->second->descriptor_uuid,
          descriptor->second->type_uuid,
          ResultNullability(descriptor->second->nullability),
          descriptor->second->collation_uuid,
          descriptor->second->timezone_profile_id};
    }
    result.result_bindings.push_back(std::move(binding));
  }

  result.batch.rows.reserve(node_it->values_row_ids.size());
  for (const auto row_id : node_it->values_row_ids) {
    const auto* row = rows.at(row_id);
    exec::DescriptorTuple tuple;
    tuple.values.reserve(row->expression_ids.size());
    for (std::size_t column = 0; column < row->expression_ids.size(); ++column) {
      api::EngineTypedValue value;
      if (!expression_runtime.EvaluateForConsumer(
              row->expression_ids[column], type_names[column],
              api::EngineCanonicalExpressionConsumer::projection, &value,
              &result.detail)) {
        result.batch = {};
        result.result_bindings.clear();
        return result;
      }
      if (!value.binary_value.empty()) {
        std::int64_t decoded = 0;
        if (!DecodeCanonicalInt64Scalar(value, &decoded, &result.detail)) {
          result.batch = {};
          result.result_bindings.clear();
          return result;
        }
        value.encoded_value = std::to_string(decoded);
        value.binary_value.clear();
      }
      if (value.descriptor.descriptor_uuid.canonical !=
          result.batch.columns[column].descriptor.descriptor_uuid.canonical) {
        auto rebound = api::QowPreserveCanonicalDescriptorAfterScalarV1(
            result.batch.columns[column].descriptor, std::move(value));
        if (rebound.descriptor.descriptor_uuid.canonical !=
            result.batch.columns[column].descriptor.descriptor_uuid.canonical) {
          result.batch = {};
          result.result_bindings.clear();
          result.detail =
              "live VALUES literal could not adopt its column descriptor";
          return result;
        }
        value = std::move(rebound);
      }
      tuple.values.push_back(std::move(value));
    }
    result.batch.rows.push_back(std::move(tuple));
  }
  const auto canonical = exec::ValidateCanonicalDescriptorBatch(
      result.batch, node_it->output_descriptor_ids);
  const auto values = exec::ValidateDescriptorBatch(result.batch);
  if (!canonical.ok || !values.ok) {
    result.batch = {};
    result.result_bindings.clear();
    result.detail = !canonical.ok ? canonical.diagnostic_code + ":" + canonical.detail
                                  : values.diagnostic_code + ":" + values.detail;
    return result;
  }
  result.ok = true;
  return result;
}

api::EngineApiResult SuccessfulApiResult(
    const CanonicalObjectFreeValuesExecutionRequest& request,
    const api::CanonicalOptimizerSelectedExecutionResult& execution) {
  api::EngineApiResult result;
  result.ok = true;
  result.operation_id = "query.execute";
  result.result_shape.result_kind = "rows";
  result.local_transaction_id = request.context.local_transaction_id;
  result.transaction_uuid = request.context.transaction_uuid;
  result.embedded_trust_mode_observed =
      request.context.trust_mode == api::EngineTrustMode::embedded_in_process;
  for (const auto& column : execution.result_publication.row_stream.columns) {
    result.result_shape.columns.push_back(column.descriptor);
  }
  for (const auto& row : execution.result_publication.row_stream.rows) {
    api::EngineRowValue api_row;
    for (std::size_t column = 0; column < row.values.size(); ++column) {
      api_row.fields.emplace_back(
          execution.result_publication.envelope.column_descriptors[column]
              .name_utf8,
          row.values[column]);
    }
    result.result_shape.rows.push_back(std::move(api_row));
  }
  result.evidence.push_back(
      {"canonical.selected_plan",
       execution.dispatch.selected_plan_uuid});
  result.evidence.push_back(
      {"canonical.result_abi", "QOW-RESULT-DIAGNOSTIC-ABI-V1"});
  return result;
}

}  // namespace scratchbird::engine::sblr

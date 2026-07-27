// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "canonical_query_execute.hpp"

#include "canonical_relational_expression.hpp"

#include "engine/optimizer/optimizer_contract.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <iomanip>
#include <limits>
#include <optional>
#include <ranges>
#include <sstream>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace scratchbird::engine::sblr {
namespace api = scratchbird::engine::internal_api;
namespace exec = scratchbird::engine::executor;
namespace opt = scratchbird::engine::optimizer;
namespace plan = scratchbird::engine::planner;
namespace {

constexpr std::string_view kValuesImplementationId =
    "values.materialize.canonical.v1";

std::uint64_t Fnv1a64(const std::string_view value,
                      std::uint64_t hash = 14695981039346656037ull) {
  for (const auto byte : value) {
    hash ^= static_cast<std::uint8_t>(byte);
    hash *= 1099511628211ull;
  }
  return hash;
}

std::string DerivedCanonicalUuid(const std::string_view scope,
                                 const std::string_view purpose) {
  const auto first = Fnv1a64(purpose, Fnv1a64(scope));
  const auto second = Fnv1a64(scope, Fnv1a64(purpose));
  std::array<std::uint8_t, 16> bytes{};
  for (std::size_t index = 0; index < 8; ++index) {
    bytes[index] = static_cast<std::uint8_t>(first >> ((7 - index) * 8));
    bytes[8 + index] =
        static_cast<std::uint8_t>(second >> ((7 - index) * 8));
  }
  bytes[6] = static_cast<std::uint8_t>((bytes[6] & 0x0f) | 0x50);
  bytes[8] = static_cast<std::uint8_t>((bytes[8] & 0x3f) | 0x80);
  std::ostringstream out;
  out << std::hex << std::setfill('0');
  for (std::size_t index = 0; index < bytes.size(); ++index) {
    if (index == 4 || index == 6 || index == 8 || index == 10) out << '-';
    out << std::setw(2) << static_cast<unsigned>(bytes[index]);
  }
  return out.str();
}

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

exec::CanonicalResultNullability ResultNullability(
    const api::RelationalNullability nullability) {
  switch (nullability) {
    case api::RelationalNullability::kNonNull:
      return exec::CanonicalResultNullability::kNonNull;
    case api::RelationalNullability::kNullable:
      return exec::CanonicalResultNullability::kNullable;
    case api::RelationalNullability::kUnknown:
      return exec::CanonicalResultNullability::kUnknown;
  }
  return exec::CanonicalResultNullability::kUnknown;
}

struct MaterializedValues {
  bool ok{false};
  exec::DescriptorBatch batch;
  std::vector<exec::CanonicalResultColumnBinding> result_bindings;
  std::string detail;
};

struct PreparedSetOperationRoot {
  bool ok{false};
  std::vector<exec::ExecutorColumnDescriptor> result_columns;
  std::vector<exec::CanonicalResultColumnBinding> result_bindings;
  std::string detail;
};

struct PreparedInnerJoinRoot {
  bool ok{false};
  std::vector<exec::CanonicalResultColumnBinding> result_bindings;
  std::string detail;
};

struct PreparedFilterRoot {
  bool ok{false};
  std::vector<exec::CanonicalResultColumnBinding> result_bindings;
  std::string detail;
};

struct PreparedProjectRoot {
  bool ok{false};
  std::vector<std::size_t> projected_columns;
  std::vector<exec::CanonicalResultColumnBinding> result_bindings;
  std::string detail;
};

PreparedProjectRoot PrepareDescriptorDirectProjectRoot(
    const api::TypedRelationalDag& dag,
    const plan::CanonicalLogicalRelationalNode& root,
    const plan::CanonicalLogicalRelationalNode& input_node,
    const MaterializedValues& input) {
  PreparedProjectRoot result;
  if (root.output_descriptor_ids.empty() ||
      input.result_bindings.size() != input.batch.columns.size() ||
      input_node.output_descriptor_ids.size() != input.batch.columns.size()) {
    result.detail = "project input or output descriptor coverage is incomplete";
    return result;
  }
  if (std::ranges::any_of(dag.outputs, [&](const auto& output) {
        return output.relation_node_id == root.logical_node_id;
      })) {
    result.detail =
        "descriptor-direct project does not admit root output lineage";
    return result;
  }

  std::unordered_map<std::uint32_t, std::size_t> input_ordinals;
  for (std::size_t ordinal = 0;
       ordinal < input_node.output_descriptor_ids.size(); ++ordinal) {
    input_ordinals.emplace(input_node.output_descriptor_ids[ordinal], ordinal);
  }
  std::size_t published_ordinal = 0;
  for (std::size_t output_ordinal = 0;
       output_ordinal < root.output_descriptor_ids.size(); ++output_ordinal) {
    const auto source =
        input_ordinals.find(root.output_descriptor_ids[output_ordinal]);
    if (source == input_ordinals.end()) {
      result.projected_columns.clear();
      result.result_bindings.clear();
      result.detail =
          "descriptor-direct project output is not an input column";
      return result;
    }
    result.projected_columns.push_back(source->second);
    auto binding = input.result_bindings[source->second];
    binding.physical_column_ordinal = output_ordinal;
    if (binding.visible) {
      if (!binding.published_descriptor.has_value()) {
        result.projected_columns.clear();
        result.result_bindings.clear();
        result.detail = "project visible result binding is incomplete";
        return result;
      }
      binding.published_descriptor->ordinal =
          static_cast<std::uint32_t>(published_ordinal++);
    }
    result.result_bindings.push_back(std::move(binding));
  }
  result.ok = true;
  return result;
}

PreparedFilterRoot PrepareFilterRoot(
    const api::TypedRelationalDag& dag,
    const plan::CanonicalLogicalRelationalNode& root,
    const plan::CanonicalLogicalRelationalNode& input_node,
    const MaterializedValues& input) {
  PreparedFilterRoot result;
  if (root.output_descriptor_ids.empty() ||
      root.output_descriptor_ids != input_node.output_descriptor_ids ||
      input.result_bindings.size() != input.batch.columns.size()) {
    result.detail = "filter output does not preserve its bound input schema";
    return result;
  }
  if (std::ranges::any_of(dag.outputs, [&](const auto& output) {
        return output.relation_node_id == root.logical_node_id;
      })) {
    result.detail = "filter root output lineage is not admitted by this profile";
    return result;
  }
  result.result_bindings = input.result_bindings;
  result.ok = true;
  return result;
}

PreparedInnerJoinRoot PrepareInnerJoinRoot(
    const api::TypedRelationalDag& dag,
    const plan::CanonicalLogicalRelationalNode& root,
    const plan::CanonicalLogicalRelationalNode& left_node,
    const plan::CanonicalLogicalRelationalNode& right_node,
    const MaterializedValues& left,
    const MaterializedValues& right) {
  PreparedInnerJoinRoot result;
  std::vector<std::uint32_t> concatenated_descriptors =
      left_node.output_descriptor_ids;
  concatenated_descriptors.insert(concatenated_descriptors.end(),
                                  right_node.output_descriptor_ids.begin(),
                                  right_node.output_descriptor_ids.end());
  if (root.output_descriptor_ids.empty() ||
      root.output_descriptor_ids != concatenated_descriptors ||
      left.result_bindings.size() != left.batch.columns.size() ||
      right.result_bindings.size() != right.batch.columns.size()) {
    result.detail = "inner-join output does not concatenate both bound inputs";
    return result;
  }
  if (std::ranges::any_of(dag.outputs, [&](const auto& output) {
        return output.relation_node_id == root.logical_node_id;
      })) {
    result.detail =
        "inner-join root output lineage is not admitted by this profile";
    return result;
  }

  std::size_t published_ordinal = 0;
  const auto append_bindings =
      [&](const std::vector<exec::CanonicalResultColumnBinding>& bindings,
          const std::size_t physical_base) {
    for (const auto& source : bindings) {
      auto binding = source;
      binding.physical_column_ordinal += physical_base;
      if (binding.visible) {
        if (!binding.published_descriptor.has_value()) return false;
        binding.published_descriptor->ordinal =
            static_cast<std::uint32_t>(published_ordinal++);
      }
      result.result_bindings.push_back(std::move(binding));
    }
    return true;
  };
  if (!append_bindings(left.result_bindings, 0) ||
      !append_bindings(right.result_bindings, left.batch.columns.size())) {
    result.result_bindings.clear();
    result.detail = "inner-join visible result binding is incomplete";
    return result;
  }
  result.ok = true;
  return result;
}

PreparedSetOperationRoot PrepareOrdinalSetOperationRoot(
    const api::TypedRelationalDag& dag,
    const plan::CanonicalLogicalRelationalNode& root,
    const MaterializedValues& left,
    const MaterializedValues& right) {
  PreparedSetOperationRoot result;
  if (root.output_descriptor_ids.empty() ||
      left.batch.columns.size() != root.output_descriptor_ids.size() ||
      right.batch.columns.size() != root.output_descriptor_ids.size() ||
      left.result_bindings.size() != root.output_descriptor_ids.size()) {
    result.detail = "set-operation input/result arity is inconsistent";
    return result;
  }
  if (std::ranges::any_of(dag.outputs, [&](const auto& output) {
        return output.relation_node_id == root.logical_node_id;
      })) {
    result.detail =
        "set-operation root output lineage is not admitted by this ordinal profile";
    return result;
  }

  std::unordered_map<std::uint32_t, const api::RelationalTypeDescriptor*>
      descriptors;
  for (const auto& descriptor : dag.descriptors) {
    descriptors.emplace(descriptor.descriptor_id, &descriptor);
  }

  std::size_t published_ordinal = 0;
  for (std::size_t column = 0; column < root.output_descriptor_ids.size();
       ++column) {
    const auto descriptor = descriptors.find(root.output_descriptor_ids[column]);
    const auto& left_column = left.batch.columns[column];
    const auto& right_column = right.batch.columns[column];
    if (descriptor == descriptors.end() ||
        descriptor->second->nullability == api::RelationalNullability::kUnknown ||
        descriptor->second->collation_uuid.has_value() ||
        descriptor->second->timezone_profile_id.has_value() ||
        left_column.descriptor.canonical_type_name.empty() ||
        left_column.descriptor.canonical_type_name !=
            right_column.descriptor.canonical_type_name) {
      result.detail =
          "set-operation exact ordinal descriptor reconciliation is unresolved";
      return result;
    }

    api::EngineDescriptor engine_descriptor;
    engine_descriptor.descriptor_uuid.canonical =
        descriptor->second->descriptor_uuid;
    engine_descriptor.descriptor_kind = "scalar";
    engine_descriptor.canonical_type_name =
        left_column.descriptor.canonical_type_name;
    engine_descriptor.encoded_descriptor =
        "type_uuid=" + descriptor->second->type_uuid + ";nullability=" +
        (descriptor->second->nullability ==
                 api::RelationalNullability::kNullable
             ? "nullable"
             : "non_null");
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
    if (engine_descriptor.encoded_descriptor !=
            left_column.descriptor.encoded_descriptor ||
        engine_descriptor.encoded_descriptor !=
            right_column.descriptor.encoded_descriptor ||
        (descriptor->second->nullability ==
             api::RelationalNullability::kNullable) !=
            (left_column.nullable || right_column.nullable)) {
      result.detail =
          "set-operation exact ordinal descriptors do not share one bound type";
      return result;
    }

    result.result_columns.push_back(
        {left_column.stable_name, engine_descriptor,
         descriptor->second->nullability ==
             api::RelationalNullability::kNullable,
         descriptor->second->descriptor_id});
    auto binding = left.result_bindings[column];
    if (binding.visible) {
      binding.published_descriptor = exec::CanonicalResultColumnDescriptor{
          static_cast<std::uint32_t>(published_ordinal++),
          left_column.stable_name,
          descriptor->second->descriptor_uuid,
          descriptor->second->type_uuid,
          ResultNullability(descriptor->second->nullability),
          std::nullopt,
          std::nullopt};
    }
    result.result_bindings.push_back(std::move(binding));
  }
  result.ok = true;
  return result;
}

bool AddBatchMemoryBytes(const exec::DescriptorBatch& batch,
                         std::uint64_t* memory_bytes) {
  if (memory_bytes == nullptr) return false;
  for (const auto& row : batch.rows) {
    for (const auto& value : row.values) {
      if (value.encoded_value.size() >
          std::numeric_limits<std::uint64_t>::max() - *memory_bytes) {
        return false;
      }
      *memory_bytes += value.encoded_value.size();
    }
  }
  return true;
}

bool CheckedAdd(const std::uint64_t left, const std::uint64_t right,
                std::uint64_t* result) {
  if (result == nullptr ||
      right > std::numeric_limits<std::uint64_t>::max() - left) {
    return false;
  }
  *result = left + right;
  return true;
}

bool CheckedMultiply(const std::uint64_t left, const std::uint64_t right,
                     std::uint64_t* result) {
  if (result == nullptr ||
      (left != 0 && right >
                        std::numeric_limits<std::uint64_t>::max() / left)) {
    return false;
  }
  *result = left * right;
  return true;
}

MaterializedValues MaterializeValues(
    const api::TypedRelationalDag& dag,
    const plan::CanonicalLogicalRelationalNode& logical_node) {
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
  CanonicalRelationalExpressionRuntime expression_runtime(dag);

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
          expression->second->result_descriptor_id !=
              node_it->output_descriptor_ids[column]) {
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
      if (!type_names[column].empty() && type_names[column] != type_name) {
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
         type_names[column] != "text") ||
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
      if (!expression_runtime.Evaluate(row->expression_ids[column],
                                       type_names[column], &value,
                                       &result.detail)) {
        result.batch = {};
        result.result_bindings.clear();
        return result;
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

struct LivePhysicalNodeProfile {
  std::uint32_t logical_node_id{0};
  std::string implementation_id;
  std::string capability_uuid;
  plan::CanonicalLogicalRelationalNodeKind logical_node_kind{
      plan::CanonicalLogicalRelationalNodeKind::kValues};
  exec::PhysicalNodeKind physical_node_kind{exec::PhysicalNodeKind::kValues};
  std::string transformation_rule_id;
  std::uint64_t estimated_rows{0};
  std::uint64_t memory_bytes_required{0};
  std::size_t minimum_input_count{0};
  std::size_t maximum_input_count{0};
};

struct LivePhysicalPlanningResult {
  bool ok{false};
  exec::TypedPhysicalNodeDag physical_dag;
  std::string diagnostic_id;
  std::string detail;
};

LivePhysicalPlanningResult PlanAndPublishLivePhysicalDag(
    const CanonicalObjectFreeValuesExecutionRequest& request,
    const std::vector<LivePhysicalNodeProfile>& profiles,
    const std::string_view selected_plan_purpose,
    const std::string_view operation_name) {
  LivePhysicalPlanningResult result;
  const auto& graph = request.optimizer_request.logical_graph;
  if (profiles.size() != graph.nodes.size()) {
    result.diagnostic_id = "QOW-DIAG-OPTIMIZER-SEARCH-NO-PLAN-V1";
    result.detail = std::string(operation_name) +
                    " live profile does not cover every logical node";
    return result;
  }

  const auto identity_scope =
      graph.bound_sblr_tree_uuid + ":" + request.context.statement_uuid.canonical;
  const auto calibration_uuid =
      DerivedCanonicalUuid(identity_scope, "relational.calibration");
  plan::CanonicalPhysicalAlternativeCatalog alternatives;
  alternatives.bound_sblr_tree_uuid = graph.bound_sblr_tree_uuid;
  alternatives.catalog_epoch_uuid = graph.catalog_epoch_uuid;
  alternatives.security_context_uuid = graph.security_context_uuid;
  alternatives.local_transaction_id = graph.local_transaction_id;
  alternatives.statement_snapshot_id = graph.statement_snapshot_id;

  std::unordered_set<std::uint32_t> covered_nodes;
  std::unordered_set<std::string> published_capabilities;
  std::vector<opt::CanonicalOptimizerSearchCandidateInput> candidates;
  opt::CanonicalExecutorCapabilityCatalog capabilities;
  capabilities.capability_snapshot_uuid =
      request.optimizer_admission.capability_snapshot_uuid;
  capabilities.policy_epoch = request.optimizer_admission.policy_epoch;
  capabilities.engine_owned = true;
  for (const auto& profile : profiles) {
    const auto node = std::ranges::find_if(graph.nodes, [&](const auto& item) {
      return item.logical_node_id == profile.logical_node_id;
    });
    if (profile.logical_node_id == 0 || node == graph.nodes.end() ||
        node->node_kind != profile.logical_node_kind ||
        profile.implementation_id.empty() || profile.capability_uuid.empty() ||
        profile.transformation_rule_id.empty() ||
        profile.memory_bytes_required == 0 ||
        !covered_nodes.insert(profile.logical_node_id).second) {
      result.diagnostic_id = "QOW-DIAG-OPTIMIZER-SEARCH-NO-PLAN-V1";
      result.detail = std::string(operation_name) +
                      " live node profile is incomplete or inconsistent";
      return result;
    }
    const auto suffix = std::to_string(profile.logical_node_id);
    const auto alternative_uuid =
        DerivedCanonicalUuid(identity_scope, "alternative." + suffix);
    alternatives.alternatives.push_back(
        {alternative_uuid,
         profile.logical_node_id,
         profile.implementation_id,
         profile.capability_uuid,
         node->output_descriptor_ids,
         true,
         {},
         {},
         {}});

    opt::CanonicalOptimizerSearchCandidateInput candidate;
    candidate.alternative_uuid = alternative_uuid;
    candidate.transformation_uuid =
        DerivedCanonicalUuid(identity_scope, "transformation." + suffix);
    candidate.transformation_rule_id = profile.transformation_rule_id;
    candidate.bound_sblr_tree_uuid = graph.bound_sblr_tree_uuid;
    candidate.statistics_snapshot_uuid =
        request.optimizer_admission.statistics_snapshot_uuid;
    candidate.statistics_generation =
        request.optimizer_admission.statistics_generation;
    candidate.model_family_id = "relational.local.v1";
    candidate.cost_terms.cost_vector_uuid =
        DerivedCanonicalUuid(identity_scope, "cost-vector." + suffix);
    candidate.cost_terms.calibration_profile_uuid = calibration_uuid;
    candidate.cost_terms.cpu_units =
        std::max<std::uint64_t>(1, profile.estimated_rows);
    candidate.cost_terms.memory_bytes_required =
        profile.memory_bytes_required;
    candidate.cost_terms.confidence = opt::CostConfidence::kHigh;
    candidate.semantic_preserving = true;
    candidate.derived_from_admitted_statistics = true;
    candidate.engine_coster_owned = true;
    candidates.push_back(std::move(candidate));

    if (published_capabilities.insert(profile.capability_uuid).second) {
      opt::CanonicalExecutorCapabilityRecord capability;
      capability.capability_uuid = profile.capability_uuid;
      capability.capability_abi_version = 1;
      capability.implementation_id = profile.implementation_id;
      capability.logical_node_kind = profile.logical_node_kind;
      capability.physical_node_kind = profile.physical_node_kind;
      capability.minimum_input_count = profile.minimum_input_count;
      capability.maximum_input_count = profile.maximum_input_count;
      capability.maximum_memory_bytes =
          request.optimizer_request.resource.memory_budget_bytes;
      capability.spill_supported = false;
      capability.available = true;
      capability.engine_owned = true;
      capabilities.capabilities.push_back(std::move(capability));
    }
  }

  opt::CanonicalOptimizerSearchPolicy search_policy;
  search_policy.maximum_exhaustive_plan_count = 1;
  search_policy.bounded_beam_width = 1;
  search_policy.deterministic_step_cost_ns = 1;
  search_policy.engine_owned = true;
  const auto search = opt::SearchCanonicalRelationalMemo(
      request.optimizer_request, request.optimizer_admission, alternatives,
      candidates, search_policy);
  if (!search.accepted || !search.selected || !search.issues.empty()) {
    result.diagnostic_id =
        search.issues.empty() ? "QOW-DIAG-OPTIMIZER-SEARCH-NO-PLAN-V1"
                              : search.issues.front().diagnostic_id;
    result.detail = search.issues.empty()
                        ? std::string(operation_name) +
                              " live search returned no selected plan"
                        : search.issues.front().field_id;
    return result;
  }

  opt::CanonicalOptimizerPhysicalPublicationIdentity publication_identity;
  publication_identity.selected_plan_uuid =
      DerivedCanonicalUuid(identity_scope, selected_plan_purpose);
  publication_identity.first_causal_counter_id = 1;
  publication_identity.engine_owned = true;
  const auto publication = opt::PublishCanonicalPhysicalDag(
      request.optimizer_request, request.optimizer_admission, alternatives,
      search, capabilities, publication_identity);
  if (!publication.accepted || !publication.published ||
      !publication.issues.empty()) {
    result.diagnostic_id =
        publication.issues.empty()
            ? "QOW-DIAG-OPTIMIZER-PHYSICAL-PUBLICATION-V1"
            : publication.issues.front().diagnostic_id;
    result.detail = publication.issues.empty()
                        ? std::string(operation_name) +
                              " live physical DAG was not published"
                        : publication.issues.front().field_id;
    return result;
  }
  result.ok = true;
  result.physical_dag = publication.physical_dag;
  return result;
}

exec::CanonicalPhysicalExecutorRegistration MakeLiveValuesRegistration(
    std::unordered_map<std::uint64_t, exec::DescriptorBatch> batches,
    std::string capability_uuid,
    std::string diagnostic_id,
    std::string operation_name) {
  exec::CanonicalPhysicalExecutorRegistration registration;
  registration.node_kind = exec::PhysicalNodeKind::kValues;
  registration.implementation_id = std::string(kValuesImplementationId);
  registration.executor_capability_uuid = std::move(capability_uuid);
  registration.executor_capability_abi_version = 1;
  registration.engine_owned = true;
  registration.accepts_optimizer_publication_v2 = true;
  registration.execute =
      [batches = std::move(batches), diagnostic_id = std::move(diagnostic_id),
       operation_name = std::move(operation_name)](
          const exec::TypedPhysicalNodeDag& dag,
          const exec::PhysicalNodeRecord& node,
          const std::vector<exec::CanonicalPhysicalDispatchInput>& inputs) {
        exec::CanonicalPhysicalDispatchStepResult step;
        step.selected_plan_uuid = dag.selected_plan_uuid;
        step.executed_physical_node_id = node.physical_node_id;
        step.causal_counter_id = node.causal_counter_id;
        step.output_descriptor_ids = node.output_descriptor_ids;
        step.authority.engine_mga_snapshot_bound = true;
        const auto batch = batches.find(node.relational_node_id);
        if (!inputs.empty() || batch == batches.end()) {
          step.diagnostic.ok = false;
          step.diagnostic.diagnostic_code = diagnostic_id;
          step.diagnostic.detail = operation_name +
                                   " VALUES executor input or payload identity differs";
          return step;
        }
        step.result_handle_id = node.physical_node_id;
        step.output_row_count = batch->second.rows.size();
        step.rows_examined = batch->second.rows.size();
        step.materialized_output_batch = batch->second;
        return step;
      };
  return registration;
}

CanonicalObjectFreeValuesExecutionResult
ExecuteCanonicalObjectFreeUnionAllQuery(
    const CanonicalObjectFreeValuesExecutionRequest& request) {
  CanonicalObjectFreeValuesExecutionResult result;
  const auto& graph = request.optimizer_request.logical_graph;
  const auto root = std::ranges::find_if(graph.nodes, [&](const auto& node) {
    return node.logical_node_id == graph.root_logical_node_id;
  });
  if (graph.nodes.size() != 3 || root == graph.nodes.end() ||
      root->node_kind !=
          plan::CanonicalLogicalRelationalNodeKind::kSetOperation ||
      root->semantic_variant_id != "set-operation.union-all.v1" ||
      root->input_logical_node_ids.size() != 2 ||
      root->input_logical_node_ids[0] == root->input_logical_node_ids[1] ||
      !root->bound_expression_ids.empty() ||
      !request.optimizer_request.logical_properties.properties.empty()) {
    return result;
  }
  const auto find_node = [&](const std::uint32_t node_id) {
    return std::ranges::find_if(graph.nodes, [&](const auto& node) {
      return node.logical_node_id == node_id;
    });
  };
  const auto left_node = find_node(root->input_logical_node_ids[0]);
  const auto right_node = find_node(root->input_logical_node_ids[1]);
  if (left_node == graph.nodes.end() || right_node == graph.nodes.end() ||
      left_node->node_kind !=
          plan::CanonicalLogicalRelationalNodeKind::kValues ||
      right_node->node_kind !=
          plan::CanonicalLogicalRelationalNodeKind::kValues ||
      left_node->semantic_variant_id != "values.literal-table.v1" ||
      right_node->semantic_variant_id != "values.literal-table.v1") {
    return result;
  }
  for (const auto& node : graph.nodes) {
    const bool values =
        node.node_kind == plan::CanonicalLogicalRelationalNodeKind::kValues;
    if (!node.required_object_uuids.empty() ||
        !node.required_property_uuids.empty() ||
        !node.delivered_property_uuids.empty() ||
        (values && !node.input_logical_node_ids.empty())) {
      return result;
    }
  }
  result.profile_matched = true;
  const auto refuse = [&](std::string diagnostic_id, std::string detail) {
    result.optimizer_selected = false;
    result.physical_dag_published = false;
    result.physical_dag_executed = false;
    result.runtime_actuals_attached = false;
    result.canonical_result_published = false;
    result.physical_node_count = 0;
    result.canonical_result_column_count = 0;
    result.canonical_result_row_count = 0;
    result.selected_plan_uuid.clear();
    result.canonical_result_bytes.clear();
    result.api_result =
        Failure(request, std::move(diagnostic_id), std::move(detail));
    return result;
  };
  if (!request.optimizer_admission.admitted ||
      !request.optimizer_admission.planning_allowed) {
    return refuse("QOW-DIAG-RELATIONAL-LIVE-SET-ADMISSION-V1",
                  "live UNION ALL execution lacks optimizer admission");
  }

  auto left = MaterializeValues(request.relational_dag, *left_node);
  if (!left.ok) {
    return refuse("QOW-DIAG-RELATIONAL-LIVE-SET-PAYLOAD-V1",
                  "left VALUES: " + left.detail);
  }
  auto right = MaterializeValues(request.relational_dag, *right_node);
  if (!right.ok) {
    return refuse("QOW-DIAG-RELATIONAL-LIVE-SET-PAYLOAD-V1",
                  "right VALUES: " + right.detail);
  }
  auto prepared_root = PrepareOrdinalSetOperationRoot(
      request.relational_dag, *root, left, right);
  if (!prepared_root.ok) {
    return refuse("QOW-DIAG-RELATIONAL-LIVE-SET-PAYLOAD-V1",
                  prepared_root.detail);
  }

  if (right.batch.rows.size() >
      std::numeric_limits<std::size_t>::max() - left.batch.rows.size()) {
    return refuse("QOW-DIAG-OPTIMIZER-SEARCH-COST-OVERFLOW-V1",
                  "live UNION ALL row bound overflowed");
  }
  const auto set_output_row_bound =
      left.batch.rows.size() + right.batch.rows.size();

  std::uint64_t memory_bytes = 1;
  if (!AddBatchMemoryBytes(left.batch, &memory_bytes) ||
      !AddBatchMemoryBytes(right.batch, &memory_bytes)) {
    return refuse("QOW-DIAG-OPTIMIZER-SEARCH-COST-OVERFLOW-V1",
                  "live UNION ALL materialization size overflowed");
  }
  if (memory_bytes > request.optimizer_request.resource.memory_budget_bytes) {
    return refuse("QOW-DIAG-OPTIMIZER-SEARCH-COST-VECTOR-V1",
                  "live UNION ALL inputs exceed the admitted memory budget");
  }

  const auto identity_scope =
      graph.bound_sblr_tree_uuid + ":" + request.context.statement_uuid.canonical;
  const auto values_capability_uuid =
      DerivedCanonicalUuid(identity_scope, "values.capability");
  const auto set_capability_uuid =
      DerivedCanonicalUuid(identity_scope, "union-all.capability");
  std::vector<LivePhysicalNodeProfile> profiles;
  for (const auto& node : graph.nodes) {
    const bool values =
        node.node_kind == plan::CanonicalLogicalRelationalNodeKind::kValues;
    std::uint64_t node_rows = set_output_row_bound;
    std::uint64_t node_memory = memory_bytes;
    if (node.logical_node_id == left_node->logical_node_id) {
      node_rows = left.batch.rows.size();
      node_memory = 1;
      if (!AddBatchMemoryBytes(left.batch, &node_memory)) {
        return refuse("QOW-DIAG-OPTIMIZER-SEARCH-COST-OVERFLOW-V1",
                      "left VALUES cost size overflowed");
      }
    } else if (node.logical_node_id == right_node->logical_node_id) {
      node_rows = right.batch.rows.size();
      node_memory = 1;
      if (!AddBatchMemoryBytes(right.batch, &node_memory)) {
        return refuse("QOW-DIAG-OPTIMIZER-SEARCH-COST-OVERFLOW-V1",
                      "right VALUES cost size overflowed");
      }
    }
    profiles.push_back(
        {node.logical_node_id,
         values ? std::string(kValuesImplementationId)
                : std::string("setop.union-all.ordinal.typed.v1"),
         values ? values_capability_uuid : set_capability_uuid,
         node.node_kind,
         values ? exec::PhysicalNodeKind::kValues
                : exec::PhysicalNodeKind::kSetOperation,
         values ? "canonical.values.materialize.v1"
                : "canonical.setop.union-all.ordinal.v1",
         node_rows,
         node_memory,
         values ? 0U : 2U,
         values ? 0U : 2U});
  }
  const auto planning = PlanAndPublishLivePhysicalDag(
      request, profiles, "set.selected-plan", "UNION ALL");
  if (!planning.ok) {
    return refuse(planning.diagnostic_id, planning.detail);
  }
  result.optimizer_selected = true;
  result.physical_dag_published = true;
  result.physical_node_count = planning.physical_dag.nodes.size();
  result.selected_plan_uuid = planning.physical_dag.selected_plan_uuid;

  std::unordered_map<std::uint64_t, exec::DescriptorBatch> values_batches;
  values_batches.emplace(left_node->logical_node_id, std::move(left.batch));
  values_batches.emplace(right_node->logical_node_id, std::move(right.batch));
  auto values_registration = MakeLiveValuesRegistration(
      std::move(values_batches), values_capability_uuid,
      "QOW-DIAG-RELATIONAL-LIVE-SET-VALUES-V1", "UNION ALL");

  exec::CanonicalPhysicalExecutorRegistration set_registration;
  set_registration.node_kind = exec::PhysicalNodeKind::kSetOperation;
  set_registration.implementation_id = "setop.union-all.ordinal.typed.v1";
  set_registration.executor_capability_uuid = set_capability_uuid;
  set_registration.executor_capability_abi_version = 1;
  set_registration.engine_owned = true;
  set_registration.accepts_optimizer_publication_v2 = true;
  set_registration.execute =
      [result_columns = prepared_root.result_columns, set_output_row_bound](
          const exec::TypedPhysicalNodeDag& dag,
          const exec::PhysicalNodeRecord& node,
          const std::vector<exec::CanonicalPhysicalDispatchInput>& inputs) {
        exec::CanonicalPhysicalDispatchStepResult step;
        step.selected_plan_uuid = dag.selected_plan_uuid;
        step.executed_physical_node_id = node.physical_node_id;
        step.causal_counter_id = node.causal_counter_id;
        step.output_descriptor_ids = node.output_descriptor_ids;
        step.authority.engine_mga_snapshot_bound = true;
        if (inputs.size() != 2 ||
            !inputs[0].materialized_output_batch.has_value() ||
            !inputs[1].materialized_output_batch.has_value()) {
          step.diagnostic.ok = false;
          step.diagnostic.diagnostic_code =
              "QOW-DIAG-RELATIONAL-LIVE-SET-INPUT-V1";
          step.diagnostic.detail =
              "UNION ALL executor did not receive two typed input batches";
          return step;
        }
        exec::CanonicalSetOperationAllRequest set_request;
        set_request.physical_dag = dag;
        set_request.selected_physical_node_id = node.physical_node_id;
        set_request.left_batch = *inputs[0].materialized_output_batch;
        set_request.right_batch = *inputs[1].materialized_output_batch;
        set_request.result_columns = result_columns;
        set_request.operation = exec::CanonicalSetOperationKind::kUnion;
        set_request.alignment = exec::CanonicalSetOperationAlignment::kOrdinal;
        set_request.quantifier = exec::CanonicalSetOperationQuantifier::kAll;
        set_request.equality_profile =
            exec::CanonicalSetOperationEqualityProfile::kExactTyped;
        set_request.type_profile =
            exec::CanonicalSetOperationTypeProfile::kExact;
        set_request.maximum_output_row_count =
            std::max<std::size_t>(1, set_output_row_bound);
        const auto set_result =
            exec::ExecuteCanonicalSetOperationAll(set_request);
        if (!set_result.diagnostic.ok) {
          step.diagnostic = set_result.diagnostic;
          return step;
        }
        step.result_handle_id = node.physical_node_id;
        step.input_row_count = set_result.left_input_row_count +
                               set_result.right_input_row_count;
        step.rows_examined = step.input_row_count;
        step.output_row_count = set_result.output_batch.rows.size();
        step.materialized_output_batch = set_result.output_batch;
        return step;
      };

  api::CanonicalOptimizerSelectedExecutionRequest execution_request;
  execution_request.selected_physical_dag = planning.physical_dag;
  execution_request.pre_access_statistics_snapshot_uuid =
      planning.physical_dag.statistics_snapshot_uuid;
  execution_request.inventory_local_transaction_id =
      planning.physical_dag.local_transaction_id;
  execution_request.inventory_statement_snapshot_id =
      planning.physical_dag.statement_snapshot_id;
  execution_request.available_executors.push_back(
      std::move(values_registration));
  execution_request.available_executors.push_back(std::move(set_registration));
  execution_request.engine_execution_authorized = true;
  execution_request.result_publication_request.statement_uuid =
      request.context.statement_uuid.canonical;
  execution_request.result_publication_request.execution_attempt_uuid =
      DerivedCanonicalUuid(
          identity_scope + ":" + request.context.current_monotonic_ns,
          "set.execution-attempt");
  execution_request.result_publication_request.transaction_effect_evidence_uuid =
      DerivedCanonicalUuid(
          identity_scope + ":" +
              std::to_string(request.context.local_transaction_id) + ":" +
              std::to_string(
                  request.context.snapshot_visible_through_local_transaction_id),
          "set.transaction-effect-unchanged");
  execution_request.result_publication_request.result_kind =
      exec::CanonicalResultKind::kRows;
  execution_request.result_publication_request.invocation_mode =
      exec::CanonicalResultInvocationMode::kDirect;
  execution_request.result_publication_request.column_bindings =
      std::move(prepared_root.result_bindings);
  execution_request.result_publication_request.maximum_row_count =
      std::max<std::size_t>(1, set_output_row_bound);

  const auto execution =
      api::ExecuteCanonicalOptimizerSelectedDag(execution_request);
  if (!execution.accepted || !execution.exact_selected_nodes_executed ||
      !execution.causal_counters_attached ||
      !execution.canonical_result_published || !execution.issues.empty()) {
    return refuse(
        execution.issues.empty()
            ? "QOW-DIAG-RELATIONAL-LIVE-SET-EXECUTION-V1"
            : execution.issues.front().diagnostic_id,
        execution.issues.empty()
            ? "live UNION ALL selected DAG was not completed"
            : execution.issues.front().field_id);
  }
  result.physical_dag_executed = true;
  result.runtime_actuals_attached = execution.runtime_actuals.accepted;
  result.canonical_result_published = execution.result_publication.published;
  result.canonical_result_column_count =
      execution.result_publication.envelope.column_descriptors.size();
  result.canonical_result_row_count =
      execution.result_publication.row_stream.rows.size();
  result.canonical_result_bytes =
      execution.result_publication.canonical_envelope_bytes;
  result.api_result = SuccessfulApiResult(request, execution);
  return result;
}

CanonicalObjectFreeValuesExecutionResult
ExecuteCanonicalObjectFreeInnerJoinQuery(
    const CanonicalObjectFreeValuesExecutionRequest& request) {
  CanonicalObjectFreeValuesExecutionResult result;
  const auto& graph = request.optimizer_request.logical_graph;
  const auto root = std::ranges::find_if(graph.nodes, [&](const auto& node) {
    return node.logical_node_id == graph.root_logical_node_id;
  });
  if (graph.nodes.size() != 3 || root == graph.nodes.end() ||
      root->node_kind != plan::CanonicalLogicalRelationalNodeKind::kJoin ||
      root->semantic_variant_id != "join.inner.v1" ||
      root->input_logical_node_ids.size() != 2 ||
      root->input_logical_node_ids[0] == root->input_logical_node_ids[1] ||
      root->bound_expression_ids.size() != 1 ||
      !request.optimizer_request.logical_properties.properties.empty()) {
    return result;
  }
  const auto find_node = [&](const std::uint32_t node_id) {
    return std::ranges::find_if(graph.nodes, [&](const auto& node) {
      return node.logical_node_id == node_id;
    });
  };
  const auto left_node = find_node(root->input_logical_node_ids[0]);
  const auto right_node = find_node(root->input_logical_node_ids[1]);
  if (left_node == graph.nodes.end() || right_node == graph.nodes.end() ||
      left_node->node_kind !=
          plan::CanonicalLogicalRelationalNodeKind::kValues ||
      right_node->node_kind !=
          plan::CanonicalLogicalRelationalNodeKind::kValues ||
      left_node->semantic_variant_id != "values.literal-table.v1" ||
      right_node->semantic_variant_id != "values.literal-table.v1") {
    return result;
  }
  for (const auto& node : graph.nodes) {
    const bool values =
        node.node_kind == plan::CanonicalLogicalRelationalNodeKind::kValues;
    if (!node.required_object_uuids.empty() ||
        !node.required_property_uuids.empty() ||
        !node.delivered_property_uuids.empty() ||
        (values && !node.input_logical_node_ids.empty())) {
      return result;
    }
  }
  result.profile_matched = true;
  const auto refuse = [&](std::string diagnostic_id, std::string detail) {
    result.optimizer_selected = false;
    result.physical_dag_published = false;
    result.physical_dag_executed = false;
    result.runtime_actuals_attached = false;
    result.canonical_result_published = false;
    result.physical_node_count = 0;
    result.canonical_result_column_count = 0;
    result.canonical_result_row_count = 0;
    result.selected_plan_uuid.clear();
    result.canonical_result_bytes.clear();
    result.api_result =
        Failure(request, std::move(diagnostic_id), std::move(detail));
    return result;
  };
  if (!request.optimizer_admission.admitted ||
      !request.optimizer_admission.planning_allowed) {
    return refuse("QOW-DIAG-RELATIONAL-LIVE-JOIN-ADMISSION-V1",
                  "live INNER JOIN execution lacks optimizer admission");
  }

  auto left = MaterializeValues(request.relational_dag, *left_node);
  if (!left.ok) {
    return refuse("QOW-DIAG-RELATIONAL-LIVE-JOIN-PAYLOAD-V1",
                  "left VALUES: " + left.detail);
  }
  auto right = MaterializeValues(request.relational_dag, *right_node);
  if (!right.ok) {
    return refuse("QOW-DIAG-RELATIONAL-LIVE-JOIN-PAYLOAD-V1",
                  "right VALUES: " + right.detail);
  }
  auto prepared_root = PrepareInnerJoinRoot(
      request.relational_dag, *root, *left_node, *right_node, left, right);
  if (!prepared_root.ok) {
    return refuse("QOW-DIAG-RELATIONAL-LIVE-JOIN-PAYLOAD-V1",
                  prepared_root.detail);
  }

  api::EngineSqlTruthValue predicate_truth =
      api::EngineSqlTruthValue::unknown;
  std::string predicate_detail;
  CanonicalRelationalExpressionRuntime expression_runtime(
      request.relational_dag);
  if (!expression_runtime.EvaluatePredicate(root->bound_expression_ids.front(),
                                            &predicate_truth,
                                            &predicate_detail)) {
    return refuse("QOW-DIAG-RELATIONAL-LIVE-JOIN-PAYLOAD-V1",
                  "INNER JOIN predicate: " + predicate_detail);
  }

  const auto left_count = left.batch.rows.size();
  const auto right_count = right.batch.rows.size();
  if (left_count != 0 &&
      right_count > std::numeric_limits<std::size_t>::max() / left_count) {
    return refuse("QOW-DIAG-OPTIMIZER-SEARCH-COST-OVERFLOW-V1",
                  "live INNER JOIN pair cardinality overflowed");
  }
  const auto pair_count = left_count * right_count;
  const auto output_row_bound =
      predicate_truth == api::EngineSqlTruthValue::true_value ? pair_count : 0;

  std::uint64_t left_memory = 1;
  std::uint64_t right_memory = 1;
  if (!AddBatchMemoryBytes(left.batch, &left_memory) ||
      !AddBatchMemoryBytes(right.batch, &right_memory)) {
    return refuse("QOW-DIAG-OPTIMIZER-SEARCH-COST-OVERFLOW-V1",
                  "live INNER JOIN input size overflowed");
  }
  std::uint64_t total_memory = 0;
  std::uint64_t predicate_memory = 0;
  if (!CheckedAdd(left_memory, right_memory, &total_memory) ||
      !CheckedMultiply(pair_count, sizeof(api::EngineSqlTruthValue),
                       &predicate_memory) ||
      !CheckedAdd(total_memory, predicate_memory, &total_memory)) {
    return refuse("QOW-DIAG-OPTIMIZER-SEARCH-COST-OVERFLOW-V1",
                  "live INNER JOIN predicate state size overflowed");
  }
  if (output_row_bound != 0) {
    std::uint64_t repeated_left = 0;
    std::uint64_t repeated_right = 0;
    std::uint64_t output_memory = 0;
    if (!CheckedMultiply(left_memory, right_count, &repeated_left) ||
        !CheckedMultiply(right_memory, left_count, &repeated_right) ||
        !CheckedAdd(repeated_left, repeated_right, &output_memory) ||
        !CheckedAdd(output_memory, pair_count, &output_memory) ||
        !CheckedAdd(total_memory, output_memory, &total_memory)) {
      return refuse("QOW-DIAG-OPTIMIZER-SEARCH-COST-OVERFLOW-V1",
                    "live INNER JOIN output size overflowed");
    }
  }
  if (total_memory >
      request.optimizer_request.resource.memory_budget_bytes) {
    return refuse("QOW-DIAG-OPTIMIZER-SEARCH-COST-VECTOR-V1",
                  "live INNER JOIN exceeds the admitted memory budget");
  }

  const auto identity_scope =
      graph.bound_sblr_tree_uuid + ":" + request.context.statement_uuid.canonical;
  const auto values_capability_uuid =
      DerivedCanonicalUuid(identity_scope, "values.capability");
  const auto join_capability_uuid =
      DerivedCanonicalUuid(identity_scope, "inner-join.capability");
  std::vector<LivePhysicalNodeProfile> profiles;
  for (const auto& node : graph.nodes) {
    const bool values =
        node.node_kind == plan::CanonicalLogicalRelationalNodeKind::kValues;
    std::uint64_t node_rows = pair_count;
    std::uint64_t node_memory = total_memory;
    if (node.logical_node_id == left_node->logical_node_id) {
      node_rows = left_count;
      node_memory = left_memory;
    } else if (node.logical_node_id == right_node->logical_node_id) {
      node_rows = right_count;
      node_memory = right_memory;
    }
    profiles.push_back(
        {node.logical_node_id,
         values ? std::string(kValuesImplementationId)
                : std::string("join.inner.3vl.nested.v1"),
         values ? values_capability_uuid : join_capability_uuid,
         node.node_kind,
         values ? exec::PhysicalNodeKind::kValues
                : exec::PhysicalNodeKind::kJoin,
         values ? "canonical.values.materialize.v1"
                : "canonical.join.inner.3vl.nested.v1",
         node_rows,
         node_memory,
         values ? 0U : 2U,
         values ? 0U : 2U});
  }
  const auto planning = PlanAndPublishLivePhysicalDag(
      request, profiles, "join.selected-plan", "INNER JOIN");
  if (!planning.ok) {
    return refuse(planning.diagnostic_id, planning.detail);
  }
  result.optimizer_selected = true;
  result.physical_dag_published = true;
  result.physical_node_count = planning.physical_dag.nodes.size();
  result.selected_plan_uuid = planning.physical_dag.selected_plan_uuid;

  std::unordered_map<std::uint64_t, exec::DescriptorBatch> values_batches;
  values_batches.emplace(left_node->logical_node_id, std::move(left.batch));
  values_batches.emplace(right_node->logical_node_id, std::move(right.batch));
  auto values_registration = MakeLiveValuesRegistration(
      std::move(values_batches), values_capability_uuid,
      "QOW-DIAG-RELATIONAL-LIVE-JOIN-VALUES-V1", "INNER JOIN");

  exec::CanonicalPhysicalExecutorRegistration join_registration;
  join_registration.node_kind = exec::PhysicalNodeKind::kJoin;
  join_registration.implementation_id = "join.inner.3vl.nested.v1";
  join_registration.executor_capability_uuid = join_capability_uuid;
  join_registration.executor_capability_abi_version = 1;
  join_registration.engine_owned = true;
  join_registration.accepts_optimizer_publication_v2 = true;
  join_registration.execute =
      [predicate_truth, pair_count](
          const exec::TypedPhysicalNodeDag& dag,
          const exec::PhysicalNodeRecord& node,
          const std::vector<exec::CanonicalPhysicalDispatchInput>& inputs) {
        exec::CanonicalPhysicalDispatchStepResult step;
        step.selected_plan_uuid = dag.selected_plan_uuid;
        step.executed_physical_node_id = node.physical_node_id;
        step.causal_counter_id = node.causal_counter_id;
        step.output_descriptor_ids = node.output_descriptor_ids;
        step.authority.engine_mga_snapshot_bound = true;
        if (inputs.size() != 2 ||
            !inputs[0].materialized_output_batch.has_value() ||
            !inputs[1].materialized_output_batch.has_value()) {
          step.diagnostic.ok = false;
          step.diagnostic.diagnostic_code =
              "QOW-DIAG-RELATIONAL-LIVE-JOIN-INPUT-V1";
          step.diagnostic.detail =
              "INNER JOIN executor did not receive two typed input batches";
          return step;
        }
        const auto& left_batch = *inputs[0].materialized_output_batch;
        const auto& right_batch = *inputs[1].materialized_output_batch;
        if (left_batch.rows.size() != 0 &&
            right_batch.rows.size() >
                std::numeric_limits<std::size_t>::max() /
                    left_batch.rows.size()) {
          step.diagnostic.ok = false;
          step.diagnostic.diagnostic_code =
              "QOW-DIAG-RELATIONAL-LIVE-JOIN-INPUT-V1";
          step.diagnostic.detail = "INNER JOIN pair cardinality overflowed";
          return step;
        }
        const auto actual_pair_count =
            left_batch.rows.size() * right_batch.rows.size();
        if (actual_pair_count != pair_count ||
            right_batch.rows.size() >
                std::numeric_limits<std::size_t>::max() -
                    left_batch.rows.size()) {
          step.diagnostic.ok = false;
          step.diagnostic.diagnostic_code =
              "QOW-DIAG-RELATIONAL-LIVE-JOIN-INPUT-V1";
          step.diagnostic.detail =
              "INNER JOIN input cardinality differs or overflows its "
              "selected cost";
          return step;
        }
        exec::CanonicalDescriptorInnerJoinRequest join_request;
        join_request.physical_dag = dag;
        join_request.selected_physical_node_id = node.physical_node_id;
        join_request.left_batch = left_batch;
        join_request.right_batch = right_batch;
        join_request.pair_truth_values.assign(pair_count, predicate_truth);
        join_request.consumer = api::EnginePredicateConsumer::join_on;
        const auto join_result =
            exec::ExecuteCanonicalDescriptorInnerJoin(join_request);
        if (!join_result.diagnostic.ok) {
          step.diagnostic = join_result.diagnostic;
          return step;
        }
        step.result_handle_id = node.physical_node_id;
        step.input_row_count =
            left_batch.rows.size() + right_batch.rows.size();
        step.rows_examined = pair_count;
        step.output_row_count = join_result.output_batch.rows.size();
        step.materialized_output_batch = join_result.output_batch;
        return step;
      };

  api::CanonicalOptimizerSelectedExecutionRequest execution_request;
  execution_request.selected_physical_dag = planning.physical_dag;
  execution_request.pre_access_statistics_snapshot_uuid =
      planning.physical_dag.statistics_snapshot_uuid;
  execution_request.inventory_local_transaction_id =
      planning.physical_dag.local_transaction_id;
  execution_request.inventory_statement_snapshot_id =
      planning.physical_dag.statement_snapshot_id;
  execution_request.available_executors.push_back(
      std::move(values_registration));
  execution_request.available_executors.push_back(std::move(join_registration));
  execution_request.engine_execution_authorized = true;
  execution_request.result_publication_request.statement_uuid =
      request.context.statement_uuid.canonical;
  execution_request.result_publication_request.execution_attempt_uuid =
      DerivedCanonicalUuid(
          identity_scope + ":" + request.context.current_monotonic_ns,
          "join.execution-attempt");
  execution_request.result_publication_request.transaction_effect_evidence_uuid =
      DerivedCanonicalUuid(
          identity_scope + ":" +
              std::to_string(request.context.local_transaction_id) + ":" +
              std::to_string(
                  request.context.snapshot_visible_through_local_transaction_id),
          "join.transaction-effect-unchanged");
  execution_request.result_publication_request.result_kind =
      exec::CanonicalResultKind::kRows;
  execution_request.result_publication_request.invocation_mode =
      exec::CanonicalResultInvocationMode::kDirect;
  execution_request.result_publication_request.column_bindings =
      std::move(prepared_root.result_bindings);
  execution_request.result_publication_request.maximum_row_count =
      std::max<std::size_t>(1, output_row_bound);

  const auto execution =
      api::ExecuteCanonicalOptimizerSelectedDag(execution_request);
  if (!execution.accepted || !execution.exact_selected_nodes_executed ||
      !execution.causal_counters_attached ||
      !execution.canonical_result_published || !execution.issues.empty()) {
    return refuse(
        execution.issues.empty()
            ? "QOW-DIAG-RELATIONAL-LIVE-JOIN-EXECUTION-V1"
            : execution.issues.front().diagnostic_id,
        execution.issues.empty()
            ? "live INNER JOIN selected DAG was not completed"
            : execution.issues.front().field_id);
  }
  result.physical_dag_executed = true;
  result.runtime_actuals_attached = execution.runtime_actuals.accepted;
  result.canonical_result_published = execution.result_publication.published;
  result.canonical_result_column_count =
      execution.result_publication.envelope.column_descriptors.size();
  result.canonical_result_row_count =
      execution.result_publication.row_stream.rows.size();
  result.canonical_result_bytes =
      execution.result_publication.canonical_envelope_bytes;
  result.api_result = SuccessfulApiResult(request, execution);
  return result;
}

CanonicalObjectFreeValuesExecutionResult
ExecuteCanonicalObjectFreeFilterQuery(
    const CanonicalObjectFreeValuesExecutionRequest& request) {
  CanonicalObjectFreeValuesExecutionResult result;
  const auto& graph = request.optimizer_request.logical_graph;
  const auto root = std::ranges::find_if(graph.nodes, [&](const auto& node) {
    return node.logical_node_id == graph.root_logical_node_id;
  });
  if (graph.nodes.size() != 2 || root == graph.nodes.end() ||
      root->node_kind != plan::CanonicalLogicalRelationalNodeKind::kFilter ||
      root->semantic_variant_id != "filter.where.v1" ||
      root->input_logical_node_ids.size() != 1 ||
      root->bound_expression_ids.size() != 1 ||
      !request.optimizer_request.logical_properties.properties.empty()) {
    return result;
  }
  const auto input_node =
      std::ranges::find_if(graph.nodes, [&](const auto& node) {
        return node.logical_node_id == root->input_logical_node_ids.front();
      });
  if (input_node == graph.nodes.end() || input_node == root ||
      input_node->node_kind !=
          plan::CanonicalLogicalRelationalNodeKind::kValues ||
      input_node->semantic_variant_id != "values.literal-table.v1" ||
      !input_node->input_logical_node_ids.empty()) {
    return result;
  }
  for (const auto& node : graph.nodes) {
    if (!node.required_object_uuids.empty() ||
        !node.required_property_uuids.empty() ||
        !node.delivered_property_uuids.empty()) {
      return result;
    }
  }
  result.profile_matched = true;
  const auto refuse = [&](std::string diagnostic_id, std::string detail) {
    result.optimizer_selected = false;
    result.physical_dag_published = false;
    result.physical_dag_executed = false;
    result.runtime_actuals_attached = false;
    result.canonical_result_published = false;
    result.physical_node_count = 0;
    result.canonical_result_column_count = 0;
    result.canonical_result_row_count = 0;
    result.selected_plan_uuid.clear();
    result.canonical_result_bytes.clear();
    result.api_result =
        Failure(request, std::move(diagnostic_id), std::move(detail));
    return result;
  };
  if (!request.optimizer_admission.admitted ||
      !request.optimizer_admission.planning_allowed) {
    return refuse("QOW-DIAG-RELATIONAL-LIVE-FILTER-ADMISSION-V1",
                  "live FILTER execution lacks optimizer admission");
  }

  auto input = MaterializeValues(request.relational_dag, *input_node);
  if (!input.ok) {
    return refuse("QOW-DIAG-RELATIONAL-LIVE-FILTER-PAYLOAD-V1",
                  "FILTER input VALUES: " + input.detail);
  }
  auto prepared_root = PrepareFilterRoot(request.relational_dag, *root,
                                         *input_node, input);
  if (!prepared_root.ok) {
    return refuse("QOW-DIAG-RELATIONAL-LIVE-FILTER-PAYLOAD-V1",
                  prepared_root.detail);
  }

  api::EngineSqlTruthValue predicate_truth =
      api::EngineSqlTruthValue::unknown;
  std::string predicate_detail;
  CanonicalRelationalExpressionRuntime expression_runtime(
      request.relational_dag);
  if (!expression_runtime.EvaluatePredicate(root->bound_expression_ids.front(),
                                            &predicate_truth,
                                            &predicate_detail)) {
    return refuse("QOW-DIAG-RELATIONAL-LIVE-FILTER-PAYLOAD-V1",
                  "FILTER predicate: " + predicate_detail);
  }

  const auto input_row_count = input.batch.rows.size();
  const auto output_row_bound =
      predicate_truth == api::EngineSqlTruthValue::true_value
          ? input_row_count
          : 0;
  std::uint64_t input_memory = 1;
  if (!AddBatchMemoryBytes(input.batch, &input_memory)) {
    return refuse("QOW-DIAG-OPTIMIZER-SEARCH-COST-OVERFLOW-V1",
                  "live FILTER input size overflowed");
  }
  std::uint64_t predicate_memory = 0;
  std::uint64_t total_memory = 0;
  if (!CheckedMultiply(input_row_count, sizeof(api::EngineSqlTruthValue),
                       &predicate_memory) ||
      !CheckedAdd(input_memory, predicate_memory, &total_memory) ||
      (output_row_bound != 0 &&
       !CheckedAdd(total_memory, input_memory, &total_memory))) {
    return refuse("QOW-DIAG-OPTIMIZER-SEARCH-COST-OVERFLOW-V1",
                  "live FILTER predicate or output size overflowed");
  }
  if (total_memory >
      request.optimizer_request.resource.memory_budget_bytes) {
    return refuse("QOW-DIAG-OPTIMIZER-SEARCH-COST-VECTOR-V1",
                  "live FILTER exceeds the admitted memory budget");
  }

  const auto identity_scope =
      graph.bound_sblr_tree_uuid + ":" + request.context.statement_uuid.canonical;
  const auto values_capability_uuid =
      DerivedCanonicalUuid(identity_scope, "values.capability");
  const auto filter_capability_uuid =
      DerivedCanonicalUuid(identity_scope, "filter.capability");
  const std::vector<LivePhysicalNodeProfile> profiles = {
      {input_node->logical_node_id,
       std::string(kValuesImplementationId),
       values_capability_uuid,
       plan::CanonicalLogicalRelationalNodeKind::kValues,
       exec::PhysicalNodeKind::kValues,
       "canonical.values.materialize.v1",
       input_row_count,
       input_memory,
       0,
       0},
      {root->logical_node_id,
       "filter.3vl.row.v1",
       filter_capability_uuid,
       plan::CanonicalLogicalRelationalNodeKind::kFilter,
       exec::PhysicalNodeKind::kFilter,
       "canonical.filter.3vl.row.v1",
       input_row_count,
       total_memory,
       1,
       1}};
  const auto planning = PlanAndPublishLivePhysicalDag(
      request, profiles, "filter.selected-plan", "FILTER");
  if (!planning.ok) {
    return refuse(planning.diagnostic_id, planning.detail);
  }
  result.optimizer_selected = true;
  result.physical_dag_published = true;
  result.physical_node_count = planning.physical_dag.nodes.size();
  result.selected_plan_uuid = planning.physical_dag.selected_plan_uuid;

  std::unordered_map<std::uint64_t, exec::DescriptorBatch> values_batches;
  values_batches.emplace(input_node->logical_node_id, std::move(input.batch));
  auto values_registration = MakeLiveValuesRegistration(
      std::move(values_batches), values_capability_uuid,
      "QOW-DIAG-RELATIONAL-LIVE-FILTER-VALUES-V1", "FILTER");

  exec::CanonicalPhysicalExecutorRegistration filter_registration;
  filter_registration.node_kind = exec::PhysicalNodeKind::kFilter;
  filter_registration.implementation_id = "filter.3vl.row.v1";
  filter_registration.executor_capability_uuid = filter_capability_uuid;
  filter_registration.executor_capability_abi_version = 1;
  filter_registration.engine_owned = true;
  filter_registration.accepts_optimizer_publication_v2 = true;
  filter_registration.execute =
      [predicate_truth, input_row_count](
          const exec::TypedPhysicalNodeDag& dag,
          const exec::PhysicalNodeRecord& node,
          const std::vector<exec::CanonicalPhysicalDispatchInput>& inputs) {
        exec::CanonicalPhysicalDispatchStepResult step;
        step.selected_plan_uuid = dag.selected_plan_uuid;
        step.executed_physical_node_id = node.physical_node_id;
        step.causal_counter_id = node.causal_counter_id;
        step.output_descriptor_ids = node.output_descriptor_ids;
        step.authority.engine_mga_snapshot_bound = true;
        if (inputs.size() != 1 ||
            !inputs.front().materialized_output_batch.has_value()) {
          step.diagnostic.ok = false;
          step.diagnostic.diagnostic_code =
              "QOW-DIAG-RELATIONAL-LIVE-FILTER-INPUT-V1";
          step.diagnostic.detail =
              "FILTER executor did not receive one typed input batch";
          return step;
        }
        const auto& input_batch = *inputs.front().materialized_output_batch;
        if (input_batch.rows.size() != input_row_count) {
          step.diagnostic.ok = false;
          step.diagnostic.diagnostic_code =
              "QOW-DIAG-RELATIONAL-LIVE-FILTER-INPUT-V1";
          step.diagnostic.detail =
              "FILTER input cardinality differs from its selected cost";
          return step;
        }
        exec::CanonicalDescriptorFilterRequest filter_request;
        filter_request.physical_dag = dag;
        filter_request.selected_physical_node_id = node.physical_node_id;
        filter_request.input_batch = input_batch;
        filter_request.row_truth_values.assign(input_row_count,
                                               predicate_truth);
        filter_request.consumer = api::EnginePredicateConsumer::filter;
        const auto filter_result =
            exec::ExecuteCanonicalDescriptorFilter(filter_request);
        if (!filter_result.diagnostic.ok) {
          step.diagnostic = filter_result.diagnostic;
          return step;
        }
        step.result_handle_id = node.physical_node_id;
        step.input_row_count = input_batch.rows.size();
        step.rows_examined = input_batch.rows.size();
        step.output_row_count = filter_result.output_batch.rows.size();
        step.materialized_output_batch = filter_result.output_batch;
        return step;
      };

  api::CanonicalOptimizerSelectedExecutionRequest execution_request;
  execution_request.selected_physical_dag = planning.physical_dag;
  execution_request.pre_access_statistics_snapshot_uuid =
      planning.physical_dag.statistics_snapshot_uuid;
  execution_request.inventory_local_transaction_id =
      planning.physical_dag.local_transaction_id;
  execution_request.inventory_statement_snapshot_id =
      planning.physical_dag.statement_snapshot_id;
  execution_request.available_executors.push_back(
      std::move(values_registration));
  execution_request.available_executors.push_back(
      std::move(filter_registration));
  execution_request.engine_execution_authorized = true;
  execution_request.result_publication_request.statement_uuid =
      request.context.statement_uuid.canonical;
  execution_request.result_publication_request.execution_attempt_uuid =
      DerivedCanonicalUuid(
          identity_scope + ":" + request.context.current_monotonic_ns,
          "filter.execution-attempt");
  execution_request.result_publication_request.transaction_effect_evidence_uuid =
      DerivedCanonicalUuid(
          identity_scope + ":" +
              std::to_string(request.context.local_transaction_id) + ":" +
              std::to_string(
                  request.context.snapshot_visible_through_local_transaction_id),
          "filter.transaction-effect-unchanged");
  execution_request.result_publication_request.result_kind =
      exec::CanonicalResultKind::kRows;
  execution_request.result_publication_request.invocation_mode =
      exec::CanonicalResultInvocationMode::kDirect;
  execution_request.result_publication_request.column_bindings =
      std::move(prepared_root.result_bindings);
  execution_request.result_publication_request.maximum_row_count =
      std::max<std::size_t>(1, output_row_bound);

  const auto execution =
      api::ExecuteCanonicalOptimizerSelectedDag(execution_request);
  if (!execution.accepted || !execution.exact_selected_nodes_executed ||
      !execution.causal_counters_attached ||
      !execution.canonical_result_published || !execution.issues.empty()) {
    return refuse(
        execution.issues.empty()
            ? "QOW-DIAG-RELATIONAL-LIVE-FILTER-EXECUTION-V1"
            : execution.issues.front().diagnostic_id,
        execution.issues.empty()
            ? "live FILTER selected DAG was not completed"
            : execution.issues.front().field_id);
  }
  result.physical_dag_executed = true;
  result.runtime_actuals_attached = execution.runtime_actuals.accepted;
  result.canonical_result_published = execution.result_publication.published;
  result.canonical_result_column_count =
      execution.result_publication.envelope.column_descriptors.size();
  result.canonical_result_row_count =
      execution.result_publication.row_stream.rows.size();
  result.canonical_result_bytes =
      execution.result_publication.canonical_envelope_bytes;
  result.api_result = SuccessfulApiResult(request, execution);
  return result;
}

CanonicalObjectFreeValuesExecutionResult
ExecuteCanonicalObjectFreeProjectQuery(
    const CanonicalObjectFreeValuesExecutionRequest& request) {
  CanonicalObjectFreeValuesExecutionResult result;
  const auto& graph = request.optimizer_request.logical_graph;
  const auto root = std::ranges::find_if(graph.nodes, [&](const auto& node) {
    return node.logical_node_id == graph.root_logical_node_id;
  });
  if (graph.nodes.size() != 2 || root == graph.nodes.end() ||
      root->node_kind != plan::CanonicalLogicalRelationalNodeKind::kProject ||
      root->semantic_variant_id != "project.select-list.v1" ||
      root->input_logical_node_ids.size() != 1 ||
      !request.optimizer_request.logical_properties.properties.empty()) {
    return result;
  }
  const auto input_node =
      std::ranges::find_if(graph.nodes, [&](const auto& node) {
        return node.logical_node_id == root->input_logical_node_ids.front();
      });
  if (input_node == graph.nodes.end() || input_node == root ||
      input_node->node_kind !=
          plan::CanonicalLogicalRelationalNodeKind::kValues ||
      input_node->semantic_variant_id != "values.literal-table.v1" ||
      !input_node->input_logical_node_ids.empty()) {
    return result;
  }
  for (const auto& node : graph.nodes) {
    if (!node.required_object_uuids.empty() ||
        !node.required_property_uuids.empty() ||
        !node.delivered_property_uuids.empty()) {
      return result;
    }
  }
  result.profile_matched = true;
  const auto refuse = [&](std::string diagnostic_id, std::string detail) {
    result.optimizer_selected = false;
    result.physical_dag_published = false;
    result.physical_dag_executed = false;
    result.runtime_actuals_attached = false;
    result.canonical_result_published = false;
    result.physical_node_count = 0;
    result.canonical_result_column_count = 0;
    result.canonical_result_row_count = 0;
    result.selected_plan_uuid.clear();
    result.canonical_result_bytes.clear();
    result.api_result =
        Failure(request, std::move(diagnostic_id), std::move(detail));
    return result;
  };
  if (!request.optimizer_admission.admitted ||
      !request.optimizer_admission.planning_allowed) {
    return refuse("QOW-DIAG-RELATIONAL-LIVE-PROJECT-ADMISSION-V1",
                  "live PROJECT execution lacks optimizer admission");
  }
  if (!root->bound_expression_ids.empty()) {
    return refuse("QOW-DIAG-RELATIONAL-LIVE-PROJECT-PAYLOAD-V1",
                  "descriptor-direct PROJECT does not admit bound expressions");
  }

  auto input = MaterializeValues(request.relational_dag, *input_node);
  if (!input.ok) {
    return refuse("QOW-DIAG-RELATIONAL-LIVE-PROJECT-PAYLOAD-V1",
                  "PROJECT input VALUES: " + input.detail);
  }
  auto prepared_root = PrepareDescriptorDirectProjectRoot(
      request.relational_dag, *root, *input_node, input);
  if (!prepared_root.ok) {
    return refuse("QOW-DIAG-RELATIONAL-LIVE-PROJECT-PAYLOAD-V1",
                  prepared_root.detail);
  }

  const auto input_row_count = input.batch.rows.size();
  std::uint64_t input_memory = 1;
  std::uint64_t total_memory = 0;
  if (!AddBatchMemoryBytes(input.batch, &input_memory) ||
      !CheckedAdd(input_memory, input_memory, &total_memory)) {
    return refuse("QOW-DIAG-OPTIMIZER-SEARCH-COST-OVERFLOW-V1",
                  "live PROJECT input or output size overflowed");
  }
  if (total_memory >
      request.optimizer_request.resource.memory_budget_bytes) {
    return refuse("QOW-DIAG-OPTIMIZER-SEARCH-COST-VECTOR-V1",
                  "live PROJECT exceeds the admitted memory budget");
  }

  const auto identity_scope =
      graph.bound_sblr_tree_uuid + ":" + request.context.statement_uuid.canonical;
  const auto values_capability_uuid =
      DerivedCanonicalUuid(identity_scope, "values.capability");
  const auto project_capability_uuid =
      DerivedCanonicalUuid(identity_scope, "project.capability");
  const std::vector<LivePhysicalNodeProfile> profiles = {
      {input_node->logical_node_id,
       std::string(kValuesImplementationId),
       values_capability_uuid,
       plan::CanonicalLogicalRelationalNodeKind::kValues,
       exec::PhysicalNodeKind::kValues,
       "canonical.values.materialize.v1",
       input_row_count,
       input_memory,
       0,
       0},
      {root->logical_node_id,
       "project.typed.row.v1",
       project_capability_uuid,
       plan::CanonicalLogicalRelationalNodeKind::kProject,
       exec::PhysicalNodeKind::kProject,
       "canonical.project.descriptor-direct.v1",
       input_row_count,
       total_memory,
       1,
       1}};
  const auto planning = PlanAndPublishLivePhysicalDag(
      request, profiles, "project.selected-plan", "PROJECT");
  if (!planning.ok) {
    return refuse(planning.diagnostic_id, planning.detail);
  }
  result.optimizer_selected = true;
  result.physical_dag_published = true;
  result.physical_node_count = planning.physical_dag.nodes.size();
  result.selected_plan_uuid = planning.physical_dag.selected_plan_uuid;

  std::unordered_map<std::uint64_t, exec::DescriptorBatch> values_batches;
  values_batches.emplace(input_node->logical_node_id, std::move(input.batch));
  auto values_registration = MakeLiveValuesRegistration(
      std::move(values_batches), values_capability_uuid,
      "QOW-DIAG-RELATIONAL-LIVE-PROJECT-VALUES-V1", "PROJECT");

  exec::CanonicalPhysicalExecutorRegistration project_registration;
  project_registration.node_kind = exec::PhysicalNodeKind::kProject;
  project_registration.implementation_id = "project.typed.row.v1";
  project_registration.executor_capability_uuid = project_capability_uuid;
  project_registration.executor_capability_abi_version = 1;
  project_registration.engine_owned = true;
  project_registration.accepts_optimizer_publication_v2 = true;
  project_registration.execute =
      [projected_columns = prepared_root.projected_columns, input_row_count](
          const exec::TypedPhysicalNodeDag& dag,
          const exec::PhysicalNodeRecord& node,
          const std::vector<exec::CanonicalPhysicalDispatchInput>& inputs) {
        exec::CanonicalPhysicalDispatchStepResult step;
        step.selected_plan_uuid = dag.selected_plan_uuid;
        step.executed_physical_node_id = node.physical_node_id;
        step.causal_counter_id = node.causal_counter_id;
        step.output_descriptor_ids = node.output_descriptor_ids;
        step.authority.engine_mga_snapshot_bound = true;
        if (inputs.size() != 1 ||
            !inputs.front().materialized_output_batch.has_value()) {
          step.diagnostic.ok = false;
          step.diagnostic.diagnostic_code =
              "QOW-DIAG-RELATIONAL-LIVE-PROJECT-INPUT-V1";
          step.diagnostic.detail =
              "PROJECT executor did not receive one typed input batch";
          return step;
        }
        const auto& input_batch = *inputs.front().materialized_output_batch;
        if (input_batch.rows.size() != input_row_count) {
          step.diagnostic.ok = false;
          step.diagnostic.diagnostic_code =
              "QOW-DIAG-RELATIONAL-LIVE-PROJECT-INPUT-V1";
          step.diagnostic.detail =
              "PROJECT input cardinality differs from its selected cost";
          return step;
        }
        exec::CanonicalDescriptorProjectionRequest project_request;
        project_request.physical_dag = dag;
        project_request.selected_physical_node_id = node.physical_node_id;
        project_request.input_batch = input_batch;
        project_request.projected_columns = projected_columns;
        const auto project_result =
            exec::ExecuteCanonicalDescriptorProjection(project_request);
        if (!project_result.diagnostic.ok) {
          step.diagnostic = project_result.diagnostic;
          return step;
        }
        step.result_handle_id = node.physical_node_id;
        step.input_row_count = input_batch.rows.size();
        step.rows_examined = input_batch.rows.size();
        step.output_row_count = project_result.output_batch.rows.size();
        step.materialized_output_batch = project_result.output_batch;
        return step;
      };

  api::CanonicalOptimizerSelectedExecutionRequest execution_request;
  execution_request.selected_physical_dag = planning.physical_dag;
  execution_request.pre_access_statistics_snapshot_uuid =
      planning.physical_dag.statistics_snapshot_uuid;
  execution_request.inventory_local_transaction_id =
      planning.physical_dag.local_transaction_id;
  execution_request.inventory_statement_snapshot_id =
      planning.physical_dag.statement_snapshot_id;
  execution_request.available_executors.push_back(
      std::move(values_registration));
  execution_request.available_executors.push_back(
      std::move(project_registration));
  execution_request.engine_execution_authorized = true;
  execution_request.result_publication_request.statement_uuid =
      request.context.statement_uuid.canonical;
  execution_request.result_publication_request.execution_attempt_uuid =
      DerivedCanonicalUuid(
          identity_scope + ":" + request.context.current_monotonic_ns,
          "project.execution-attempt");
  execution_request.result_publication_request.transaction_effect_evidence_uuid =
      DerivedCanonicalUuid(
          identity_scope + ":" +
              std::to_string(request.context.local_transaction_id) + ":" +
              std::to_string(
                  request.context.snapshot_visible_through_local_transaction_id),
          "project.transaction-effect-unchanged");
  execution_request.result_publication_request.result_kind =
      exec::CanonicalResultKind::kRows;
  execution_request.result_publication_request.invocation_mode =
      exec::CanonicalResultInvocationMode::kDirect;
  execution_request.result_publication_request.column_bindings =
      std::move(prepared_root.result_bindings);
  execution_request.result_publication_request.maximum_row_count =
      std::max<std::size_t>(1, input_row_count);

  const auto execution =
      api::ExecuteCanonicalOptimizerSelectedDag(execution_request);
  if (!execution.accepted || !execution.exact_selected_nodes_executed ||
      !execution.causal_counters_attached ||
      !execution.canonical_result_published || !execution.issues.empty()) {
    return refuse(
        execution.issues.empty()
            ? "QOW-DIAG-RELATIONAL-LIVE-PROJECT-EXECUTION-V1"
            : execution.issues.front().diagnostic_id,
        execution.issues.empty()
            ? "live PROJECT selected DAG was not completed"
            : execution.issues.front().field_id);
  }
  result.physical_dag_executed = true;
  result.runtime_actuals_attached = execution.runtime_actuals.accepted;
  result.canonical_result_published = execution.result_publication.published;
  result.canonical_result_column_count =
      execution.result_publication.envelope.column_descriptors.size();
  result.canonical_result_row_count =
      execution.result_publication.row_stream.rows.size();
  result.canonical_result_bytes =
      execution.result_publication.canonical_envelope_bytes;
  result.api_result = SuccessfulApiResult(request, execution);
  return result;
}

}  // namespace

CanonicalObjectFreeValuesExecutionResult
ExecuteCanonicalObjectFreeValuesQuery(
    const CanonicalObjectFreeValuesExecutionRequest& request) {
  auto project = ExecuteCanonicalObjectFreeProjectQuery(request);
  if (project.profile_matched) return project;
  auto filter = ExecuteCanonicalObjectFreeFilterQuery(request);
  if (filter.profile_matched) return filter;
  auto inner_join = ExecuteCanonicalObjectFreeInnerJoinQuery(request);
  if (inner_join.profile_matched) return inner_join;
  auto set_operation = ExecuteCanonicalObjectFreeUnionAllQuery(request);
  if (set_operation.profile_matched) return set_operation;
  CanonicalObjectFreeValuesExecutionResult result;
  const auto refuse = [&](std::string diagnostic_id, std::string detail) {
    result.optimizer_selected = false;
    result.physical_dag_published = false;
    result.physical_dag_executed = false;
    result.runtime_actuals_attached = false;
    result.canonical_result_published = false;
    result.physical_node_count = 0;
    result.canonical_result_column_count = 0;
    result.canonical_result_row_count = 0;
    result.selected_plan_uuid.clear();
    result.canonical_result_bytes.clear();
    result.api_result = Failure(request, std::move(diagnostic_id),
                                std::move(detail));
    return result;
  };

  const auto& graph = request.optimizer_request.logical_graph;
  if (graph.nodes.size() != 1 ||
      graph.root_logical_node_id != graph.nodes.front().logical_node_id ||
      graph.nodes.front().node_kind !=
          plan::CanonicalLogicalRelationalNodeKind::kValues ||
      graph.nodes.front().semantic_variant_id != "values.literal-table.v1" ||
      !graph.nodes.front().input_logical_node_ids.empty() ||
      !graph.nodes.front().required_object_uuids.empty() ||
      !graph.nodes.front().required_property_uuids.empty() ||
      !graph.nodes.front().delivered_property_uuids.empty() ||
      !request.optimizer_request.logical_properties.properties.empty()) {
    return result;
  }
  result.profile_matched = true;
  if (!request.optimizer_admission.admitted ||
      !request.optimizer_admission.planning_allowed) {
    return refuse("QOW-DIAG-RELATIONAL-LIVE-VALUES-ADMISSION-V1",
                  "live VALUES execution lacks optimizer admission");
  }

  auto materialized = MaterializeValues(request.relational_dag,
                                        graph.nodes.front());
  if (!materialized.ok) {
    return refuse("QOW-DIAG-RELATIONAL-LIVE-VALUES-PAYLOAD-V1",
                  materialized.detail);
  }

  const auto identity_scope =
      graph.bound_sblr_tree_uuid + ":" + request.context.statement_uuid.canonical;
  const auto alternative_uuid =
      DerivedCanonicalUuid(identity_scope, "values.alternative");
  const auto capability_uuid =
      DerivedCanonicalUuid(identity_scope, "values.capability");
  const auto transformation_uuid =
      DerivedCanonicalUuid(identity_scope, "values.transformation");
  const auto cost_vector_uuid =
      DerivedCanonicalUuid(identity_scope, "values.cost-vector");
  const auto calibration_uuid =
      DerivedCanonicalUuid(identity_scope, "values.calibration");

  plan::CanonicalPhysicalAlternativeCatalog alternatives;
  alternatives.bound_sblr_tree_uuid = graph.bound_sblr_tree_uuid;
  alternatives.catalog_epoch_uuid = graph.catalog_epoch_uuid;
  alternatives.security_context_uuid = graph.security_context_uuid;
  alternatives.local_transaction_id = graph.local_transaction_id;
  alternatives.statement_snapshot_id = graph.statement_snapshot_id;
  alternatives.alternatives.push_back(
      {alternative_uuid,
       graph.nodes.front().logical_node_id,
       std::string(kValuesImplementationId),
       capability_uuid,
       graph.nodes.front().output_descriptor_ids,
       true,
       {},
       {},
       {}});

  std::uint64_t memory_bytes = 1;
  for (const auto& row : materialized.batch.rows) {
    for (const auto& value : row.values) {
      if (value.encoded_value.size() >
          std::numeric_limits<std::uint64_t>::max() - memory_bytes) {
        return refuse("QOW-DIAG-OPTIMIZER-SEARCH-COST-OVERFLOW-V1",
                      "live VALUES materialization size overflowed");
      }
      memory_bytes += value.encoded_value.size();
    }
  }
  if (memory_bytes > request.optimizer_request.resource.memory_budget_bytes) {
    return refuse("QOW-DIAG-OPTIMIZER-SEARCH-COST-VECTOR-V1",
                  "live VALUES materialization exceeds the admitted memory budget");
  }

  opt::CanonicalOptimizerSearchCandidateInput candidate;
  candidate.alternative_uuid = alternative_uuid;
  candidate.transformation_uuid = transformation_uuid;
  candidate.transformation_rule_id = "canonical.values.materialize.v1";
  candidate.bound_sblr_tree_uuid = graph.bound_sblr_tree_uuid;
  candidate.statistics_snapshot_uuid =
      request.optimizer_admission.statistics_snapshot_uuid;
  candidate.statistics_generation =
      request.optimizer_admission.statistics_generation;
  candidate.model_family_id = "relational.local.v1";
  candidate.cost_terms.cost_vector_uuid = cost_vector_uuid;
  candidate.cost_terms.calibration_profile_uuid = calibration_uuid;
  candidate.cost_terms.cpu_units = materialized.batch.rows.size();
  candidate.cost_terms.memory_bytes_required = memory_bytes;
  candidate.cost_terms.confidence = opt::CostConfidence::kExact;
  candidate.semantic_preserving = true;
  candidate.derived_from_admitted_statistics = true;
  candidate.engine_coster_owned = true;

  opt::CanonicalOptimizerSearchPolicy search_policy;
  search_policy.maximum_exhaustive_plan_count = 1;
  search_policy.bounded_beam_width = 1;
  search_policy.deterministic_step_cost_ns = 1;
  search_policy.engine_owned = true;
  const auto search = opt::SearchCanonicalRelationalMemo(
      request.optimizer_request, request.optimizer_admission, alternatives,
      {candidate}, search_policy);
  if (!search.accepted || !search.selected || !search.issues.empty()) {
    const auto diagnostic = search.issues.empty()
                                ? "QOW-DIAG-OPTIMIZER-SEARCH-NO-PLAN-V1"
                                : search.issues.front().diagnostic_id;
    const auto detail = search.issues.empty()
                            ? "live VALUES search returned no selected plan"
                            : search.issues.front().field_id;
    return refuse(diagnostic, detail);
  }
  result.optimizer_selected = true;

  opt::CanonicalExecutorCapabilityCatalog capabilities;
  capabilities.capability_snapshot_uuid =
      request.optimizer_admission.capability_snapshot_uuid;
  capabilities.policy_epoch = request.optimizer_admission.policy_epoch;
  capabilities.engine_owned = true;
  opt::CanonicalExecutorCapabilityRecord capability;
  capability.capability_uuid = capability_uuid;
  capability.capability_abi_version = 1;
  capability.implementation_id = kValuesImplementationId;
  capability.logical_node_kind =
      plan::CanonicalLogicalRelationalNodeKind::kValues;
  capability.physical_node_kind = exec::PhysicalNodeKind::kValues;
  capability.maximum_memory_bytes =
      request.optimizer_request.resource.memory_budget_bytes;
  capability.spill_supported = false;
  capability.available = true;
  capability.engine_owned = true;
  capabilities.capabilities.push_back(std::move(capability));

  opt::CanonicalOptimizerPhysicalPublicationIdentity publication_identity;
  publication_identity.selected_plan_uuid =
      DerivedCanonicalUuid(identity_scope, "values.selected-plan");
  publication_identity.first_causal_counter_id = 1;
  publication_identity.engine_owned = true;
  const auto publication = opt::PublishCanonicalPhysicalDag(
      request.optimizer_request, request.optimizer_admission, alternatives,
      search, capabilities, publication_identity);
  if (!publication.accepted || !publication.published ||
      !publication.issues.empty()) {
    const auto diagnostic = publication.issues.empty()
                                ? "QOW-DIAG-OPTIMIZER-PHYSICAL-PUBLICATION-V1"
                                : publication.issues.front().diagnostic_id;
    const auto detail = publication.issues.empty()
                            ? "live VALUES physical DAG was not published"
                            : publication.issues.front().field_id;
    return refuse(diagnostic, detail);
  }
  result.physical_dag_published = true;
  result.physical_node_count = publication.physical_dag.nodes.size();
  result.selected_plan_uuid = publication.physical_dag.selected_plan_uuid;

  exec::CanonicalPhysicalExecutorRegistration registration;
  registration.node_kind = exec::PhysicalNodeKind::kValues;
  registration.implementation_id = kValuesImplementationId;
  registration.executor_capability_uuid = capability_uuid;
  registration.executor_capability_abi_version = 1;
  registration.engine_owned = true;
  registration.accepts_optimizer_publication_v2 = true;
  registration.execute =
      [batch = materialized.batch](const exec::TypedPhysicalNodeDag& dag,
                                   const exec::PhysicalNodeRecord& node,
                                   const std::vector<
                                       exec::CanonicalPhysicalDispatchInput>&
                                       inputs) {
        exec::CanonicalPhysicalDispatchStepResult step;
        step.selected_plan_uuid = dag.selected_plan_uuid;
        step.executed_physical_node_id = node.physical_node_id;
        step.causal_counter_id = node.causal_counter_id;
        step.output_descriptor_ids = node.output_descriptor_ids;
        step.authority.engine_mga_snapshot_bound = true;
        if (!inputs.empty()) {
          step.diagnostic.ok = false;
          step.diagnostic.diagnostic_code =
              "QOW-DIAG-RELATIONAL-LIVE-VALUES-INPUT-V1";
          step.diagnostic.detail = "VALUES executor received an input edge";
          return step;
        }
        step.result_handle_id = node.physical_node_id;
        step.output_row_count = batch.rows.size();
        step.rows_examined = batch.rows.size();
        step.materialized_output_batch = batch;
        return step;
      };

  api::CanonicalOptimizerSelectedExecutionRequest execution_request;
  execution_request.selected_physical_dag = publication.physical_dag;
  execution_request.pre_access_statistics_snapshot_uuid =
      publication.physical_dag.statistics_snapshot_uuid;
  execution_request.inventory_local_transaction_id =
      publication.physical_dag.local_transaction_id;
  execution_request.inventory_statement_snapshot_id =
      publication.physical_dag.statement_snapshot_id;
  execution_request.available_executors.push_back(std::move(registration));
  execution_request.engine_execution_authorized = true;
  execution_request.result_publication_request.statement_uuid =
      request.context.statement_uuid.canonical;
  execution_request.result_publication_request.execution_attempt_uuid =
      DerivedCanonicalUuid(
          identity_scope + ":" + request.context.current_monotonic_ns,
          "values.execution-attempt");
  execution_request.result_publication_request.transaction_effect_evidence_uuid =
      DerivedCanonicalUuid(
          identity_scope + ":" +
              std::to_string(request.context.local_transaction_id) + ":" +
              std::to_string(
                  request.context.snapshot_visible_through_local_transaction_id),
          "values.transaction-effect-unchanged");
  execution_request.result_publication_request.result_kind =
      exec::CanonicalResultKind::kRows;
  execution_request.result_publication_request.invocation_mode =
      exec::CanonicalResultInvocationMode::kDirect;
  execution_request.result_publication_request.column_bindings =
      std::move(materialized.result_bindings);
  execution_request.result_publication_request.maximum_row_count =
      std::max<std::size_t>(1, materialized.batch.rows.size());

  const auto execution =
      api::ExecuteCanonicalOptimizerSelectedDag(execution_request);
  if (!execution.accepted || !execution.exact_selected_nodes_executed ||
      !execution.causal_counters_attached ||
      !execution.canonical_result_published || !execution.issues.empty()) {
    const auto diagnostic = execution.issues.empty()
                                ? "QOW-DIAG-RELATIONAL-LIVE-VALUES-EXECUTION-V1"
                                : execution.issues.front().diagnostic_id;
    const auto detail = execution.issues.empty()
                            ? "live VALUES selected DAG was not completed"
                            : execution.issues.front().field_id;
    return refuse(diagnostic, detail);
  }
  result.physical_dag_executed = true;
  result.runtime_actuals_attached = execution.runtime_actuals.accepted;
  result.canonical_result_published =
      execution.result_publication.published;
  result.canonical_result_column_count =
      execution.result_publication.envelope.column_descriptors.size();
  result.canonical_result_row_count =
      execution.result_publication.row_stream.rows.size();
  result.canonical_result_bytes =
      execution.result_publication.canonical_envelope_bytes;
  result.api_result = SuccessfulApiResult(request, execution);
  return result;
}

}  // namespace scratchbird::engine::sblr

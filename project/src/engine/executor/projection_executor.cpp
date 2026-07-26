// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "descriptor_value_runtime.hpp"

#include <cctype>
#include <string_view>
#include <unordered_set>
#include <utility>

namespace scratchbird::engine::executor {
namespace {

using scratchbird::engine::internal_api::EngineDescriptor;
using scratchbird::engine::internal_api::EngineValueState;

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

bool IsCanonicalUuid(const std::string_view value) {
  if (value.size() != 36 || value[8] != '-' || value[13] != '-' ||
      value[18] != '-' || value[23] != '-') {
    return false;
  }
  for (std::size_t index = 0; index < value.size(); ++index) {
    if (index == 8 || index == 13 || index == 18 || index == 23) continue;
    const auto ch = static_cast<unsigned char>(value[index]);
    if (!std::isxdigit(ch) || std::isupper(ch)) return false;
  }
  return true;
}

bool SameCanonicalDescriptor(const EngineDescriptor& left,
                             const EngineDescriptor& right) {
  return left.descriptor_uuid.canonical == right.descriptor_uuid.canonical &&
         left.descriptor_kind == right.descriptor_kind &&
         left.canonical_type_name == right.canonical_type_name &&
         left.encoded_descriptor == right.encoded_descriptor;
}

DescriptorRuntimeDiagnostic ValidateCanonicalDescriptorBatchForNode(
    const DescriptorBatch& batch,
    const std::vector<std::uint32_t>& output_descriptor_ids) {
  if (batch.columns.size() != output_descriptor_ids.size() ||
      batch.columns.empty()) {
    return Refusal("SBLR.PLAN_TREE.INVALID_HANDLE",
                   "physical output descriptor width mismatch");
  }
  std::unordered_set<std::uint32_t> descriptor_ids;
  for (std::size_t column = 0; column < batch.columns.size(); ++column) {
    const auto& bound_column = batch.columns[column];
    const auto& descriptor = bound_column.descriptor;
    if (bound_column.descriptor_id == 0 ||
        bound_column.descriptor_id != output_descriptor_ids[column] ||
        !descriptor_ids.insert(bound_column.descriptor_id).second ||
        bound_column.stable_name.empty() ||
        !IsCanonicalUuid(descriptor.descriptor_uuid.canonical) ||
        descriptor.descriptor_kind != "scalar" ||
        descriptor.canonical_type_name.empty() ||
        descriptor.encoded_descriptor.empty()) {
      return Refusal("SBLR.PLAN_TREE.INVALID_HANDLE",
                     "canonical output descriptor is unresolved", 0,
                     column);
    }
  }
  for (std::size_t row = 0; row < batch.rows.size(); ++row) {
    if (batch.rows[row].values.size() != batch.columns.size()) {
      return Refusal("SBLR.PLAN_TREE.INVALID_HANDLE",
                     "typed row width does not match physical output", row,
                     0);
    }
    for (std::size_t column = 0; column < batch.columns.size(); ++column) {
      const auto& value = batch.rows[row].values[column];
      const auto& bound_column = batch.columns[column];
      if (!SameCanonicalDescriptor(value.descriptor,
                                   bound_column.descriptor)) {
        return Refusal("SBLR.PLAN_TREE.INVALID_HANDLE",
                       "typed value lost its canonical descriptor", row,
                       column);
      }
      if (value.state == EngineValueState::sql_null) {
        if (!value.is_null || !value.encoded_value.empty() ||
            !value.binary_value.empty() || !bound_column.nullable) {
          return Refusal(
              "QOW-DIAG-QRY-029-TYPED-VALUE-REFUSAL-V1",
              "canonical SQL NULL state is malformed or non-nullable", row,
              column);
        }
        continue;
      }
      if (value.state != EngineValueState::value || value.is_null) {
        return Refusal(
            "QOW-DIAG-QRY-029-TYPED-VALUE-REFUSAL-V1",
            "legacy NULL flag or non-value sentinel reached projection", row,
            column);
      }
    }
  }
  return {};
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

  const auto dag_validation =
      ValidateTypedPhysicalNodeDag(request.physical_dag);
  if (!dag_validation.accepted) {
    const auto& issue = dag_validation.issues.front();
    return refuse(Refusal(issue.diagnostic_id, issue.field_id));
  }
  if (request.selected_physical_node_id == 0 ||
      request.selected_physical_node_id !=
          request.physical_dag.root_physical_node_id) {
    return refuse(Refusal("SBLR.PLAN_TREE.INVALID_HANDLE",
                          "selected physical node is not the root"));
  }

  const PhysicalNodeRecord* selected_node = nullptr;
  const PhysicalNodeRecord* input_node = nullptr;
  for (const auto& node : request.physical_dag.nodes) {
    if (node.physical_node_id == request.selected_physical_node_id) {
      selected_node = &node;
    }
  }
  if (selected_node == nullptr) {
    return refuse(Refusal("SBLR.PLAN_TREE.INVALID_HANDLE",
                          "selected physical node is unresolved"));
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

  auto input_validation = ValidateCanonicalDescriptorBatchForNode(
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
  auto output_validation = ValidateCanonicalDescriptorBatchForNode(
      result.output_batch, selected_node->output_descriptor_ids);
  if (!output_validation.ok) return refuse(std::move(output_validation));
  result.diagnostic = {};
  result.selected_plan_uuid = request.physical_dag.selected_plan_uuid;
  result.executed_physical_node_id = selected_node->physical_node_id;
  result.causal_counter_id = selected_node->causal_counter_id;
  return result;
}

}  // namespace scratchbird::engine::executor

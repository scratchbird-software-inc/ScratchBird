// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "descriptor_value_runtime.hpp"

#include <cctype>
#include <limits>
#include <string_view>
#include <utility>

namespace scratchbird::engine::executor {
namespace {

DescriptorRuntimeDiagnostic Refusal(std::string code,
                                    std::string detail = {}) {
  DescriptorRuntimeDiagnostic diagnostic;
  diagnostic.ok = false;
  diagnostic.diagnostic_code = std::move(code);
  diagnostic.detail = std::move(detail);
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

}  // namespace

// QOW-SOURCE-QRY-007-WINDOW-V1
// First canonical implementation in this module: global ROW_NUMBER over a
// typed batch whose deterministic order has already been established by its
// physical sort input and retained as explicit evidence.
CanonicalDescriptorRowNumberResult ExecuteCanonicalDescriptorRowNumber(
    const CanonicalDescriptorRowNumberRequest& request) {
  using scratchbird::engine::internal_api::EngineTypedValue;
  using scratchbird::engine::internal_api::EngineValueState;

  CanonicalDescriptorRowNumberResult result;
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
                          "selected window node is not the root"));
  }

  const PhysicalNodeRecord* selected_node = nullptr;
  const PhysicalNodeRecord* input_node = nullptr;
  for (const auto& node : request.physical_dag.nodes) {
    if (node.physical_node_id == request.selected_physical_node_id) {
      selected_node = &node;
    }
  }
  if (selected_node == nullptr ||
      selected_node->node_kind != PhysicalNodeKind::kWindow ||
      selected_node->input_physical_node_ids.size() != 1) {
    return refuse(Refusal("QOW-DIAG-QRY-007-WINDOW-PHYSICAL-ROUTE-V1",
                          "ROW_NUMBER requires one selected window node"));
  }
  for (const auto& node : request.physical_dag.nodes) {
    if (node.physical_node_id ==
        selected_node->input_physical_node_ids.front()) {
      input_node = &node;
      break;
    }
  }
  if (input_node == nullptr || input_node->node_kind != PhysicalNodeKind::kSort ||
      !IsCanonicalUuid(request.deterministic_order_evidence_uuid)) {
    return refuse(Refusal(
        "QOW-DIAG-QRY-007-WINDOW-ORDER-REQUIRED-V1",
        "ROW_NUMBER input lacks deterministic physical order evidence"));
  }
  std::vector<std::uint32_t> output_descriptor_ids =
      input_node->output_descriptor_ids;
  output_descriptor_ids.push_back(request.row_number_column.descriptor_id);
  if (selected_node->output_descriptor_ids != output_descriptor_ids ||
      request.row_number_column.descriptor_id == 0 ||
      request.row_number_column.nullable ||
      request.row_number_column.descriptor.canonical_type_name != "int64") {
    return refuse(Refusal("SBLR.PLAN_TREE.INVALID_HANDLE",
                          "ROW_NUMBER output descriptor is not bound int64"));
  }
  auto input_validation = ValidateCanonicalDescriptorBatch(
      request.ordered_input_batch, input_node->output_descriptor_ids);
  if (!input_validation.ok) return refuse(std::move(input_validation));
  if (request.ordered_input_batch.rows.size() >
      static_cast<std::size_t>(std::numeric_limits<std::int64_t>::max())) {
    return refuse(Refusal("QOW-DIAG-QRY-007-WINDOW-OVERFLOW-V1",
                          "ROW_NUMBER exceeds int64 result width"));
  }

  result.output_batch.columns = request.ordered_input_batch.columns;
  result.output_batch.columns.push_back(request.row_number_column);
  result.output_batch.rows = request.ordered_input_batch.rows;
  for (std::size_t row = 0; row < result.output_batch.rows.size(); ++row) {
    EngineTypedValue row_number;
    row_number.descriptor = request.row_number_column.descriptor;
    row_number.encoded_value = std::to_string(row + 1);
    row_number.state = EngineValueState::value;
    result.output_batch.rows[row].values.push_back(std::move(row_number));
  }
  auto output_validation = ValidateCanonicalDescriptorBatch(
      result.output_batch, selected_node->output_descriptor_ids);
  if (!output_validation.ok) return refuse(std::move(output_validation));

  result.diagnostic = {};
  result.selected_plan_uuid = request.physical_dag.selected_plan_uuid;
  result.executed_physical_node_id = selected_node->physical_node_id;
  result.causal_counter_id = selected_node->causal_counter_id;
  return result;
}

}  // namespace scratchbird::engine::executor
